#include <stdlib.h>
#include <string.h>
#include "cdd_mcu.h"
#include "backend.h"
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

#define SCD_MCLKS 50000000
#define CD_BLOCK_CLKS 16934400
#define CDD_MCU_DIVIDER 8
#define SECTORS_PER_SECOND 75
#define SECTOR_CLOCKS (CD_BLOCK_CLKS/SECTORS_PER_SECOND)
#define NIBBLE_CLOCKS (CDD_MCU_DIVIDER * 77)
#define BYTE_CLOCKS (SECTOR_CLOCKS/2352) // 96
#define SUBCODE_CLOCKS (SECTOR_CLOCKS/98)
#define PROCESSING_DELAY 121600 //approximate, based on relative level 4 and level 5 interrupt timing on MCD2 in pause_test

//lead in start max diameter 46 mm
//program area start max diameter 50 mm
//difference 4 mm = 4000 um
//radius difference 2 mm = 2000 um
//track pitch 1.6 um
//1250 physical tracks in between
//linear speed 1.2 m/s - 1.4 m/s
// 1.2 m = 1200 mm
// circumference at 46 mm ~ 144.51 mm
// circumference at 50 mm ~ 157.08 mm
// avg is 150.795
// 75 sectors per second
// 16 mm "typical" length of a sector
// ~9.4 sectors per track in lead-in area
#define LEADIN_SECTORS 11780

static uint32_t cd_block_to_mclks(uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)SCD_MCLKS) / ((uint64_t)CD_BLOCK_CLKS);
}

static uint32_t mclks_to_cd_block(uint32_t cycles)
{
	return ((uint64_t)cycles) * ((uint64_t)CD_BLOCK_CLKS) / ((uint64_t)SCD_MCLKS);
}

void cdd_mcu_init(cdd_mcu *context, system_media *media)
{
	context->next_int_cycle = CYCLE_NEVER;
	context->next_subcode_int_cycle = CYCLE_NEVER;
	context->last_sector_cycle = CYCLE_NEVER;
	context->last_nibble_cycle = CYCLE_NEVER;
	context->next_byte_cycle = 0;
	context->next_subcode_cycle = CYCLE_NEVER;
	context->requested_format = SF_NOTREADY;
	context->media = media;
	context->current_status_nibble = -1;
	context->current_cmd_nibble = -1;
	context->current_sector_byte = -1;
	context->current_subcode_byte = -1;
	context->current_subcode_dest = 0;
}

enum {
	GAO_CDD_CTRL,
	GAO_CDD_STATUS,
	GAO_CDD_CMD = GAO_CDD_STATUS+5,
	GAO_SUBCODE_ADDR = (0x68-0x36)/2,
	GAO_SUBCODE_START = (0x100-0x36)/2
};
//GAO_CDD_CTRL
#define BIT_MUTE 0x100
#define BIT_HOCK 0x0004
#define BIT_DRS  0x0002
#define BIT_DTS  0x0001

static uint8_t checksum(uint8_t *vbuffer)
{
	uint8_t *buffer = vbuffer;
	uint8_t sum = 0;
	for (int i = 0; i < 9; i++)
	{
		sum += buffer[i];
	}
	return (~sum) & 0xF;
}

#define MIN_CIRCUMFERENCE 144.51f
#define MAX_CIRCUMFERENCE 364.42f
#define SECTOR_LENGTH 16.0f
// max diameter for program area 116 mm
// circumference ~ 364.42 mm
// ~ 23 sectors per physical track at edge
// ~9 sectors per physical track at start of lead-in
// seek test suggests average somewhere around 54-60 tracks for a long seek, with a peak around 80
// Sonic CD title screen seems to need a much higher value to get reasonable sync
#define COARSE_SEEK_TRACKS 60
static float sectors_per_track_at_pba(uint32_t pba)
{
	//TODO: better estimate of sectors per track at current head location
	float circumference = (MAX_CIRCUMFERENCE-MIN_CIRCUMFERENCE) * ((float)pba) / ((74 * 60 + 41) * SECTORS_PER_SECOND + LEADIN_SECTORS + 22) + MIN_CIRCUMFERENCE;
	return circumference / SECTOR_LENGTH;
}

static void handle_seek(cdd_mcu *context)
{
	uint32_t old_coarse = context->coarse_seek;
	if (context->seeking == 2) {
		context->head_pba = context->seek_pba;
		context->coarse_seek = 6;
		context->seeking = 0;
	} else if (context->seeking) {
		if (context->seek_pba == context->head_pba) {
			context->seeking = 0;
			context->coarse_seek = 0;
			if (context->status == DS_PAUSE && !context->pause_pba) {
				context->pause_pba = context->head_pba;
			}
		} else {

			//TODO: drive will periodically lose tracking when seeking which slows
			//things down periodically, I estimate the average
			float sectors_per_track = sectors_per_track_at_pba(context->head_pba);
			uint32_t max_seek = sectors_per_track * COARSE_SEEK_TRACKS;
			uint32_t min_seek = sectors_per_track;

			uint32_t old_pba = context->head_pba;
			if (context->seek_pba > context->head_pba) {
				uint32_t seek_amount;
				for (seek_amount = max_seek; seek_amount >= min_seek; seek_amount >>= 1)
				{
					if (context->seek_pba - context->head_pba >= seek_amount) {
						break;
					}
				}
				if (seek_amount >= min_seek) {
					context->head_pba += seek_amount;
				} else {
					context->head_pba++;
				}
			} else {
				uint32_t seek_amount;
				for (seek_amount = max_seek; seek_amount >= min_seek;)
				{
					uint32_t next_seek = seek_amount >> 1;
					if (context->head_pba - context->seek_pba > next_seek) {
						break;
					}
					seek_amount = next_seek;
				}
				if (seek_amount >= min_seek && context->head_pba >= seek_amount) {
					context->head_pba -= seek_amount;
				} else if (context->head_pba >= min_seek){
					context->head_pba -= min_seek;
				} else {
					context->head_pba = 0;
				}
			}
			if (context->head_pba != old_pba + 1) {
				context->coarse_seek++;
			} else {
				context->coarse_seek = 0;
			}
		}
	} else {
		context->coarse_seek = 0;
	}
}

static void lba_to_status(cdd_mcu *context, uint32_t lba)
{
	uint32_t seconds = lba / 75;
	uint32_t frames = lba % 75;
	uint32_t minutes = seconds / 60;
	seconds = seconds % 60;
	context->status_buffer.b.time.min_high = minutes / 10;
	context->status_buffer.b.time.min_low = minutes % 10;
	context->status_buffer.b.time.sec_high = seconds / 10;
	context->status_buffer.b.time.sec_low = seconds % 10;
	context->status_buffer.b.time.frame_high = frames / 10;
	context->status_buffer.b.time.frame_low = frames % 10;
}

static void update_status(cdd_mcu *context, uint16_t *gate_array)
{
	gate_array[GAO_CDD_CTRL] |= BIT_MUTE;
	switch (context->status)
	{
	case DS_STOP:
		handle_seek(context);
		break;
	case DS_PLAY:
		handle_seek(context);
		if (!context->seeking) {
			context->head_pba++;
		}
		if (context->head_pba >= LEADIN_SECTORS) {
			uint8_t track = context->media->seek(context->media, context->head_pba - LEADIN_SECTORS);
			if (!context->seeking && context->media->tracks[track].type == TRACK_AUDIO) {
				gate_array[GAO_CDD_CTRL] &= ~BIT_MUTE;
			}
		}
		break;
	case DS_PAUSE:
		handle_seek(context);
		if (!context->seeking) {
			context->head_pba++;
			if (context->head_pba > context->pause_pba) {
				uint32_t back = sectors_per_track_at_pba(context->head_pba) + 0.5f;
				if (back > context->head_pba) {
					back = context->head_pba;
				}
				context->head_pba -= back;
				context->coarse_seek = 6;
			}
		}
		if (context->head_pba >= LEADIN_SECTORS) {
			context->media->seek(context->media, context->head_pba - LEADIN_SECTORS);
		}
		break;
	case DS_TOC_READ:
		handle_seek(context);
		if (!context->seeking) {
			context->head_pba++;
			if (context->media && context->media->type == MEDIA_CDROM && context->media->num_tracks) {
				if (context->head_pba > 3*(context->media->num_tracks + 2)) {
					context->toc_valid = 1;
					context->seeking = 1;
					context->seek_pba = LEADIN_SECTORS + context->media->tracks[0].start_lba;
					context->status = DS_PAUSE;
				}

			} else {
				context->status = DS_NO_DISC;
			}
		}
		break;
	case DS_TRACKING:
		handle_seek(context);
		if (!context->seeking) {
			context->status = DS_PAUSE;
			context->pause_pba = context->head_pba;
		}
		if (context->head_pba >= LEADIN_SECTORS) {
			uint8_t track = context->media->seek(context->media, context->head_pba - LEADIN_SECTORS);
			if (!context->seeking && context->media->tracks[track].type == TRACK_AUDIO) {
				gate_array[GAO_CDD_CTRL] &= ~BIT_MUTE;
			}
		}
		break;
	}
	uint8_t force_not_ready = 0;
	if (context->coarse_seek  && !(context->coarse_seek % 6)) {
		//TODO: adjust seeking for focus error when these bad statuses happen
		//BIOS depends on getting a not ready status during seeking to clear certain state
		force_not_ready = context->status_buffer.format != SF_NOTREADY;
	}
	if (context->first_cmd_received) {
		switch (force_not_ready ? SF_NOTREADY : context->requested_format)
		{
		case SF_ABSOLUTE:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				lba_to_status(context, context->head_pba - LEADIN_SECTORS);
				context->status_buffer.format = SF_ABSOLUTE;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_RELATIVE:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				uint32_t lba =context->head_pba - LEADIN_SECTORS;
				for (uint32_t i = 0; i < context->media->num_tracks; i++)
				{
					if (lba < context->media->tracks[i].end_lba) {
						if (lba < context->media->tracks[i].start_lba) {
							//relative time counts down to 0 in pregap
							lba = context->media->tracks[i].start_lba - lba;
						} else {
							lba -= context->media->tracks[i].start_lba;
						}
						break;
					}
				}
				lba_to_status(context, lba);
				context->status_buffer.format = SF_RELATIVE;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TRACK:
			if (context->toc_valid && context->head_pba >= LEADIN_SECTORS) {
				uint32_t lba =context->head_pba - LEADIN_SECTORS;
				uint32_t i;
				for (i = 0; i < context->media->num_tracks; i++)
				{
					if (lba < context->media->tracks[i].end_lba) {
						break;
					}
				}
				context->status_buffer.b.track.track_high = (i + 1) / 10;
				context->status_buffer.b.track.track_low = (i + 1) % 10;
				if (context->media->tracks[i].type == TRACK_DATA) {
					context->status_buffer.b.track.control = 4;
				} else {
					//TODO: pre-emphasis flag
					//TODO: copy permitted flag
					context->status_buffer.b.track.control = 0;
				}
				context->status_buffer.b.track.adr = 1;
				context->status_buffer.format = SF_TRACK;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCO:
			if (context->toc_valid) {
				lba_to_status(context, context->media->tracks[context->media->num_tracks - 1].end_lba);
				context->status_buffer.format = SF_TOCO;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCT:
			if (context->toc_valid) {
				context->status_buffer.b.toct.first_track_high = 0;
				context->status_buffer.b.toct.first_track_low = 1;
				context->status_buffer.b.toct.last_track_high = (context->media->num_tracks) / 10;
				context->status_buffer.b.toct.last_track_low = (context->media->num_tracks) % 10;
				context->status_buffer.b.toct.version = 0;
				context->status_buffer.format = SF_TOCT;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_TOCN:
			if (context->toc_valid) {
				if (context->requested_track > context->media->num_tracks) {
					dprintf("track number %d is bad\n", context->requested_track);
					exit(0);
				}
				uint32_t lba = context->media->tracks[context->requested_track - 1].start_lba;
				lba_to_status(context, lba);
				if (context->media->tracks[context->requested_track - 1].type == TRACK_DATA) {
					context->status_buffer.b.tocn.frame_high |= 0x8;
				}
				context->status_buffer.b.tocn.track_low = context->requested_track % 10;
				context->status_buffer.format = SF_TOCN;
			} else {
				context->status_buffer.format = SF_NOTREADY;
			}
			break;
		case SF_NOTREADY:
			memset(&context->status_buffer, 0, sizeof(context->status_buffer) - 1);
			context->status_buffer.format = SF_NOTREADY;
			break;
		}
		if (context->error_status == DS_STOP) {
			if (context->status == DS_NO_DISC) {
				//a drive with no disc answers a TOC request with no disc, rather
				//than claiming to read a TOC that is never going to come
				context->status_buffer.status = DS_NO_DISC;
			} else if (context->requested_format >= SF_TOCO && context->requested_format <= SF_TOCN) {
				context->status_buffer.status = DS_TOC_READ;
			} else if (context->seeking && context->status != DS_TRACKING) {
				context->status_buffer.status = DS_SEEK;
			} else {
				context->status_buffer.status = context->status;
			}
		} else {
			context->status_buffer.status = context->error_status;
			context->status_buffer.format = SF_NOTREADY;
			context->error_status = DS_STOP;
		}
		if (context->requested_format != SF_TOCN) {
			context->status_buffer.b.time.flags = !!(gate_array[GAO_CDD_CTRL] & BIT_MUTE); //TODO: populate these
		}
	} else {
		// Did not receive our first command so just send zeroes
		memset(&context->status_buffer, 0, sizeof(context->status_buffer) - 1);
	}
	context->status_buffer.checksum = checksum((uint8_t *)&context->status_buffer);
	if (context->status_buffer.format != SF_NOTREADY || (context->status != DS_STOP && context->status < DS_SUM_ERROR)) {
		dprintf("CDD Status %X%X.%X%X%X%X%X%X.%X%X (lba %u)\n",
			context->status_buffer.status, context->status_buffer.format,
			context->status_buffer.b.time.min_high, context->status_buffer.b.time.min_low,
			context->status_buffer.b.time.sec_high, context->status_buffer.b.time.sec_low,
			context->status_buffer.b.time.frame_high, context->status_buffer.b.time.frame_low,
			context->status_buffer.b.time.flags, context->status_buffer.checksum, context->head_pba - LEADIN_SECTORS
		);
	}
}

static void run_command(cdd_mcu *context)
{
	uint8_t check = checksum((uint8_t*)&context->cmd_buffer);
	if (check != context->cmd_buffer.checksum) {
		context->error_status = DS_SUM_ERROR;
		return;
	}
	if (context->cmd_buffer.must_be_zero) {
		context->error_status = DS_CMD_ERROR;
		return;
	}
	context->first_cmd_received = 1;
	switch (context->cmd_buffer.cmd_type)
	{
	case CMD_NOP:
		break;
	case CMD_STOP:
		dputs("CDD CMD: STOP");
		context->status = DS_STOP;
		context->requested_format = SF_ABSOLUTE;
		break;
	case CMD_READ:
	case CMD_SEEK: {
		if (context->status == DS_DOOR_OPEN || context->status == DS_TRAY_MOVING || context->status == DS_DISC_LEADOUT || context->status == DS_DISC_LEADIN) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN || context->requested_format == SF_TOCO) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		uint32_t lba = context->cmd_buffer.b.time.min_high * 10 + context->cmd_buffer.b.time.min_low;
		lba *= 60;
		lba += context->cmd_buffer.b.time.sec_high * 10 + context->cmd_buffer.b.time.sec_low;
		lba *= 75;
		lba += context->cmd_buffer.b.time.frame_high * 10 + context->cmd_buffer.b.time.frame_low;
		dprintf("CDD CMD: %s cmd for lba %d, MM:SS:FF %u%u:%u%u:%u%u\n",
			context->cmd_buffer.cmd_type == CMD_READ ? "READ" : "SEEK", lba,
			context->cmd_buffer.b.time.min_high, context->cmd_buffer.b.time.min_low,
			context->cmd_buffer.b.time.sec_high, context->cmd_buffer.b.time.sec_low,
			context->cmd_buffer.b.time.frame_high, context->cmd_buffer.b.time.frame_low
		);
		if (lba >= context->media->tracks[context->media->num_tracks - 1].end_lba) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		context->seek_pba = lba + LEADIN_SECTORS - 3;
		if (context->cmd_buffer.cmd_type == CMD_SEEK) {
			context->pause_pba = lba + LEADIN_SECTORS;
		}
		context->seeking = 1;
		context->status = context->cmd_buffer.cmd_type == CMD_READ ? DS_PLAY : DS_PAUSE;
		break;
	}
	case CMD_REPORT_REQUEST:
		switch (context->cmd_buffer.b.format.status_type)
		{
		case SF_ABSOLUTE:
		case SF_RELATIVE:
		case SF_TRACK:
			context->requested_format = context->cmd_buffer.b.format.status_type;
			break;
		case SF_TOCO:
			if (context->toc_valid) {
				context->requested_format = SF_TOCO;
			} else {
				context->error_status = DS_CMD_ERROR;
				context->requested_format = SF_ABSOLUTE;
			}
			break;
		case SF_TOCT:
			if (context->toc_valid) {
				if (context->status == DS_STOP) {
					context->status = DS_TOC_READ;
					context->seeking = 1;
					context->seek_pba = 0;
				}
			} else {
				context->status = DS_TOC_READ;
				context->seeking = 1;
				context->seek_pba = 0;
			}
			context->requested_format = SF_TOCT;
			break;
		case SF_TOCN:
			context->requested_track = context->cmd_buffer.b.format.track_high * 10;
			context->requested_track += context->cmd_buffer.b.format.track_low;
			if (!context->media || context->requested_track > context->media->num_tracks) {
				context->requested_format = SF_ABSOLUTE;
				context->error_status = DS_CMD_ERROR;
				break;
			}
			context->status = DS_TOC_READ;
			context->seeking = 1;
			context->seek_pba = 0;
			context->requested_format = SF_TOCN;
			break;
		}
		dprintf("CDD CMD: REPORT REQUEST(%d), format set to %d\n", context->cmd_buffer.b.format.status_type, context->requested_format);
		break;
	case CMD_PAUSE:
		if (context->status == DS_DOOR_OPEN || context->status == DS_TRAY_MOVING || context->status == DS_DISC_LEADOUT || context->status == DS_DISC_LEADIN) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->status == DS_STOP) {
			context->seeking = 1;
			context->seek_pba = LEADIN_SECTORS + context->media->tracks[0].start_lba;
			dprintf("CDD CMD: PAUSE, seeking to %u\n", context->seek_pba);
		} else {
			uint32_t lba = context->head_pba - LEADIN_SECTORS;
			uint32_t seconds = lba / 75;
			uint32_t frames = lba % 75;
			uint32_t minutes = seconds / 60;
			seconds = seconds % 60;
			dprintf("CDD CMD: PAUSE, current lba %u, MM:SS:FF %02u:%02u:%02u\n", lba, minutes, seconds, frames);
		}
		context->status = DS_PAUSE;
		if (context->seeking) {
			//handle_seek will populate this
			context->pause_pba = 0;
		} else {
			context->pause_pba = context->head_pba;
			uint32_t back = 2.1f * sectors_per_track_at_pba(context->head_pba) + 0.5f;
			if (back > context->head_pba) {
				back = context->head_pba;
			}
			context->seek_pba = context->head_pba - back;
			context->seeking = 2;
		}
		break;
	case CMD_PLAY:
		if (context->status == DS_DOOR_OPEN || context->status == DS_TRAY_MOVING || context->status == DS_DISC_LEADOUT || context->status == DS_DISC_LEADIN) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->status == DS_STOP || context->status == DS_TOC_READ) {
			context->seeking = 1;
			context->seek_pba = LEADIN_SECTORS + context->media->tracks[0].start_lba - 4;
			dprintf("CDD CMD: PLAY, seeking to %u\n", context->seek_pba);
		} else {
			dputs("CDD CMD: PLAY");
		}
		context->status = DS_PLAY;
		break;
	//TODO: CMD_FFWD, CMD_RWD
	case CMD_TRACK_SKIP:
		if (context->status != DS_PLAY && context->status != DS_PAUSE && context->status != DS_DISC_LEADOUT) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		if (context->requested_format == SF_TOCT || context->requested_format == SF_TOCN) {
			context->requested_format = SF_ABSOLUTE;
		}
		if (!context->toc_valid) {
			context->error_status = DS_CMD_ERROR;
			break;
		}
		{
			int32_t to_skip = context->cmd_buffer.b.skip.tracks_highest << 12 | context->cmd_buffer.b.skip.tracks_midhigh << 8
				| context->cmd_buffer.b.skip.tracks_midlow << 4 | context->cmd_buffer.b.skip.tracks_lowest;
			if (context->cmd_buffer.b.skip.direction) {
				to_skip = -to_skip;
			}
			dprintf("CDD CMD: TRACK_SKIP direction %u, num_tracks %i, delta %i\n", context->cmd_buffer.b.skip.direction, abs(to_skip), to_skip);
			//circumference at 83mm point (roughly half way between inner and outer edge of program area)
			//~ 260.75cm ~ 15 sectors
			context->seek_pba = context->head_pba + to_skip * 15;
			context->seeking = 1;
		}
		context->status = DS_TRACKING;
		break;
	default:
		dprintf("CDD CMD: Unimplemented(%d)\n", context->cmd_buffer.cmd_type);
	}
}

void cdd_mcu_run(cdd_mcu *context, uint32_t cycle, uint16_t *gate_array, lc8951* cdc, cdd_fader* fader)
{
	uint32_t cd_cycle = mclks_to_cd_block(cycle);
	if (!(gate_array[GAO_CDD_CTRL] & BIT_HOCK)) {
		//it's a little unclear if this gates the actual cd block clock or just handshaking
		//assum it's actually the clock for now
		for (; context->cycle < cd_cycle; context->cycle += CDD_MCU_DIVIDER) {
			if (context->cycle >= context->next_byte_cycle) {
				cdd_fader_data(fader, 0);
				lc8951_write_byte(cdc, cd_block_to_mclks(context->cycle), 0, 0);
				context->next_byte_cycle += BYTE_CLOCKS;
			}
		}
		gate_array[GAO_CDD_CTRL] |= BIT_MUTE;
		//the host dropping HOCK ends the conversation; when it is raised again
		//the drive reports zeroes until it gets a command, as it does at power
		//on. The BIOS checks for exactly that after the sub CPU is reset, and a
		//Mode 1 cartridge that resets the sub CPU a second time otherwise gets
		//the drive's last status, which the BIOS takes for a drive error
		context->first_cmd_received = 0;
		return;
	}
	uint32_t next_subcode = context->last_sector_cycle + SECTOR_CLOCKS;
	uint32_t next_nibble;
	if (context->current_status_nibble > 0) {
		next_nibble = context->last_nibble_cycle + NIBBLE_CLOCKS;
	} else if (!context->current_status_nibble) {
		next_nibble = context->last_sector_cycle + PROCESSING_DELAY;
		if (context->coarse_seek % 3) {
			next_nibble += SECTOR_CLOCKS * (3 - (context->coarse_seek % 3));
		}
	} else {
		next_nibble = CYCLE_NEVER;
	}
	uint32_t next_cmd_nibble = context->current_cmd_nibble >= 0 ? context->last_nibble_cycle + NIBBLE_CLOCKS : CYCLE_NEVER;

	for (; context->cycle < cd_cycle; context->cycle += CDD_MCU_DIVIDER)
	{
		if (context->cycle >= next_subcode) {
			uint32_t old_coarse = context->coarse_seek;
			context->last_sector_cycle = context->cycle;
			next_subcode = context->cycle + SECTOR_CLOCKS;
			update_status(context, gate_array);
			next_nibble = context->cycle + PROCESSING_DELAY;
			if (context->coarse_seek % 3) {
				next_nibble += SECTOR_CLOCKS * (3 - (context->coarse_seek % 3));
			}
			context->current_status_nibble = 0;
			if (context->next_subcode_int_cycle != CYCLE_NEVER) {
				context->subcode_int_pending = 1;
			}
			if ((context->status == DS_PLAY || context->status == DS_PAUSE) && context->head_pba >= LEADIN_SECTORS && !context->seeking) {
				context->current_sector_byte = 0;
				context->current_subcode_byte = 0;
				context->next_subcode_cycle = context->cycle;
				context->next_subcode_int_cycle = cd_block_to_mclks(next_subcode);
			} else {
				context->next_subcode_int_cycle = CYCLE_NEVER;
			}
			if (old_coarse != context->coarse_seek) {
				context->next_int_cycle = cd_block_to_mclks(next_nibble + 7 * NIBBLE_CLOCKS);
			}
		}
		if (context->cycle >= next_nibble) {
			if (context->current_status_nibble == sizeof(cdd_status)) {
				context->current_status_nibble = -1;
				gate_array[GAO_CDD_CTRL] &= ~BIT_DRS;
				if (context->cmd_recv_pending) {
					context->cmd_recv_pending = 0;
					context->current_cmd_nibble = 0;
					gate_array[GAO_CDD_CTRL] |= BIT_DTS;
					next_cmd_nibble = context->cycle + NIBBLE_CLOCKS;
				} else {
					context->cmd_recv_wait = 1;
				}
				next_nibble = CYCLE_NEVER;
			} else {
				gate_array[GAO_CDD_CTRL] |= BIT_DRS;
				uint8_t value = ((uint8_t *)&context->status_buffer)[context->current_status_nibble];
				int ga_index = GAO_CDD_STATUS + (context->current_status_nibble >> 1);
				if (context->current_status_nibble & 1) {
					gate_array[ga_index] = value | (gate_array[ga_index]  & 0xFF00);
				} else {
					gate_array[ga_index] = (value << 8) | (gate_array[ga_index]  & 0x00FF);
				}
				if (context->current_status_nibble == 7) {
					if (!(context->coarse_seek % 3)) {
						context->int_pending = 1;
						if (context->coarse_seek) {
							context->next_int_cycle = cd_block_to_mclks(context->cycle + 3 * SECTOR_CLOCKS);
						} else {
							context->next_int_cycle = cd_block_to_mclks(context->cycle + SECTOR_CLOCKS);
						}
					}
				}
				context->current_status_nibble++;
				context->last_nibble_cycle = context->cycle;
				next_nibble = context->cycle + NIBBLE_CLOCKS;
			}
		} else if (context->cycle >= next_cmd_nibble) {
			if (context->current_cmd_nibble == sizeof(cdd_cmd)) {
				next_cmd_nibble = CYCLE_NEVER;
				context->current_cmd_nibble = -1;
				gate_array[GAO_CDD_CTRL] &= ~BIT_DTS;
				run_command(context);
			} else {
				int ga_index = GAO_CDD_CMD + (context->current_cmd_nibble >> 1);
				uint8_t value = (context->current_cmd_nibble & 1) ? gate_array[ga_index] : gate_array[ga_index] >> 8;
				((uint8_t *)&context->cmd_buffer)[context->current_cmd_nibble] = value;
				context->current_cmd_nibble++;
				context->last_nibble_cycle = context->cycle;
				next_cmd_nibble = context->cycle + NIBBLE_CLOCKS;
			}
		}
		if (context->cycle >= context->next_byte_cycle) {
			if (context->current_sector_byte >= 0/* && (!fader->byte_counter || context->current_sector_byte)*/) {
				if (!context->current_sector_byte) {
					//HACK: things can get a little out of sync currently which causes a mess in the fader code
					// since it expects even multiples of 4 bytes (1 stereo sample)
					while (fader->byte_counter)
					{
						lc8951_write_byte(cdc, cd_block_to_mclks(context->cycle), 0, 0);
						cdd_fader_data(fader, 0);
					}
				}
				uint8_t byte = context->media->read(context->media, context->current_sector_byte);
				if (context->status != DS_PLAY) {
					byte = 0;
				}
				lc8951_write_byte(cdc, cd_block_to_mclks(context->cycle), context->current_sector_byte++, byte);
				cdd_fader_data(fader, gate_array[GAO_CDD_CTRL] & BIT_MUTE ? 0 : byte);
			} else {
				lc8951_write_byte(cdc, cd_block_to_mclks(context->cycle), 0, 0);
				cdd_fader_data(fader, 0);
			}
			if (context->current_sector_byte == 2352) {
				context->current_sector_byte = -1;
			}
			context->next_byte_cycle += BYTE_CLOCKS;
		}
		if (context->cycle >= context->next_subcode_cycle) {
			uint8_t byte;
			if (!context->current_subcode_byte) {
				byte = 0x9F;
				//This probably happens after the second sync symbol, but doing it here simplifies things a little
				context->current_subcode_dest &= 0x7E;
				gate_array[GAO_SUBCODE_ADDR] = (context->current_subcode_dest - 96) & 0x7E;
			} else if (context->current_subcode_byte == 1) {
				byte = 0xFD;
			} else {
				byte = context->media->read_subcodes(context->media, context->current_subcode_byte - 2);
			}
			int offset = GAO_SUBCODE_START + (context->current_subcode_dest >> 1);
			if (context->current_subcode_dest & 1) {
				gate_array[offset] &= 0xFF00;
				gate_array[offset] |= byte;
			} else {
				gate_array[offset] &= 0x00FF;
				gate_array[offset] |= byte << 8;
			}
			context->current_subcode_byte++;
			if (context->current_subcode_byte == 98) {
				context->current_subcode_byte = 0;
			} else if (context->current_subcode_byte == 32) {
				gate_array[GAO_SUBCODE_ADDR] |= 0x80;
			}
			context->current_subcode_dest++;
			context->current_subcode_dest &= 0x7F;
			context->next_subcode_cycle += SUBCODE_CLOCKS;
		}
	}
}

void cdd_mcu_start_cmd_recv(cdd_mcu *context, uint16_t *gate_array)
{
	if (gate_array[GAO_CDD_CTRL] & BIT_DTS) {
		return;
	}
	if (context->cmd_recv_wait) {
		context->current_cmd_nibble = 0;
		gate_array[GAO_CDD_CTRL] |= BIT_DTS;
		context->last_nibble_cycle = context->cycle;
		context->cmd_recv_wait = 0;
	} else {
		context->cmd_recv_pending = 1;
	}
}

void cdd_hock_enabled(cdd_mcu *context)
{
	context->last_sector_cycle = context->cycle;
	context->next_int_cycle = cd_block_to_mclks(context->cycle + SECTOR_CLOCKS + PROCESSING_DELAY + 7 * NIBBLE_CLOCKS);
	if (context->coarse_seek % 3) {
		context->next_int_cycle += cd_block_to_mclks(SECTOR_CLOCKS * (3 - (context->coarse_seek % 3)));
	}
}

void cdd_hock_disabled(cdd_mcu *context)
{
	context->last_sector_cycle = CYCLE_NEVER;
	context->next_int_cycle = CYCLE_NEVER;
	context->last_nibble_cycle = CYCLE_NEVER;
	context->current_status_nibble = -1;
	context->current_cmd_nibble = -1;
}

void cdd_mcu_adjust_cycle(cdd_mcu *context, uint32_t deduction)
{
	uint32_t cd_deduction = mclks_to_cd_block(deduction);
	if (context->cycle > cd_deduction) {
		context->cycle -= cd_deduction;
	} else {
		context->cycle = 0;
	}
	if (context->next_int_cycle != CYCLE_NEVER) {
		context->next_int_cycle -= deduction;
	}
	if (context->last_sector_cycle != CYCLE_NEVER) {
		if (context->last_sector_cycle > cd_deduction) {
			context->last_sector_cycle -= cd_deduction;
		} else {
			context->last_sector_cycle = 0;
		}
	}
	if (context->last_nibble_cycle != CYCLE_NEVER) {
		if (context->last_nibble_cycle > cd_deduction) {
			context->last_nibble_cycle -= cd_deduction;
		} else {
			context->last_nibble_cycle = 0;
		}
	}
	context->next_byte_cycle -= cd_deduction;
	if (context->next_subcode_cycle != CYCLE_NEVER) {
		context->next_subcode_cycle -= cd_deduction;
	}
}

void cdd_mcu_serialize(cdd_mcu *context, serialize_buffer *buf)
{
	save_int32(buf, context->cycle);
	save_int32(buf, context->next_int_cycle);
	save_int32(buf, context->next_subcode_int_cycle);
	save_int32(buf, context->last_sector_cycle);
	save_int32(buf, context->last_nibble_cycle);
	save_int32(buf, context->next_byte_cycle);
	save_int32(buf, context->next_subcode_cycle);
	save_int8(buf, context->current_status_nibble);
	save_int8(buf, context->current_cmd_nibble);
	save_int16(buf, context->current_sector_byte);
	save_int8(buf, context->current_subcode_byte);
	save_int8(buf, context->current_subcode_dest);
	save_int32(buf, context->head_pba);
	save_int32(buf, context->seek_pba);
	save_int32(buf, context->pause_pba);
	save_int32(buf, context->coarse_seek);
	save_buffer8(buf, (uint8_t *)&context->cmd_buffer, sizeof(context->cmd_buffer));
	save_buffer8(buf, (uint8_t *)&context->status_buffer, sizeof(context->status_buffer));
	save_int8(buf, context->requested_format);
	save_int8(buf, context->status);
	save_int8(buf, context->error_status);
	save_int8(buf, context->requested_track);
	save_int8(buf, context->cmd_recv_wait);
	save_int8(buf, context->cmd_recv_pending);
	save_int8(buf, context->int_pending);
	save_int8(buf, context->subcode_int_pending);
	save_int8(buf, context->toc_valid);
	save_int8(buf, context->first_cmd_received);
	save_int8(buf, context->seeking);
	save_int8(buf, context->in_fake_pregap);
}

static int sign_extend8(uint8_t value)
{
	if (value & 0x80) {
		return value | 0xFFFFFF00;
	} else {
		return value;
	}
}

static int sign_extend16(uint16_t value)
{
	if (value & 0x8000) {
		return value | 0xFFFF0000;
	} else {
		return value;
	}
}

void cdd_mcu_deserialize(deserialize_buffer *buf, void *vcontext)
{
	cdd_mcu *context = vcontext;
	context->cycle = load_int32(buf);
	context->next_int_cycle = load_int32(buf);
	context->next_subcode_int_cycle = load_int32(buf);
	context->last_sector_cycle = load_int32(buf);
	context->last_nibble_cycle = load_int32(buf);
	context->next_byte_cycle = load_int32(buf);
	context->next_subcode_cycle = load_int32(buf);
	context->current_status_nibble = sign_extend8(load_int8(buf));
	context->current_cmd_nibble = sign_extend8(load_int8(buf));
	context->current_sector_byte = sign_extend16(load_int16(buf));
	context->current_subcode_byte = sign_extend8(load_int8(buf));
	context->current_subcode_dest = sign_extend8(load_int8(buf));
	context->head_pba = load_int32(buf);
	context->seek_pba = load_int32(buf);
	context->pause_pba = load_int32(buf);
	context->coarse_seek = load_int32(buf);
	load_buffer8(buf, (uint8_t *)&context->cmd_buffer, sizeof(context->cmd_buffer));
	load_buffer8(buf, (uint8_t *)&context->status_buffer, sizeof(context->status_buffer));
	context->requested_format = load_int8(buf);
	context->status = load_int8(buf);
	context->error_status = load_int8(buf);
	context->requested_track = load_int8(buf);
	context->cmd_recv_wait = load_int8(buf);
	context->cmd_recv_pending = load_int8(buf);
	context->int_pending = load_int8(buf);
	context->subcode_int_pending = load_int8(buf);
	context->toc_valid = load_int8(buf);
	context->first_cmd_received = load_int8(buf);
	context->seeking = load_int8(buf);
	context->in_fake_pregap = load_int8(buf);
}
