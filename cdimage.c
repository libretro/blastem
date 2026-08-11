#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "system.h"
#include "cdimage.h"
#include "paths.h"
#include "wave.h"

uint8_t cdrom_scramble(uint16_t *lsfr, uint8_t data)
{
	data ^= *lsfr;
	for (int i = 0; i < 8; i++)
	{
		uint16_t new_bit = *lsfr;
		*lsfr >>= 1;
		new_bit = (new_bit ^ *lsfr) & 1;
		*lsfr |= new_bit << 14;
	}
	return data;
}

static char* cmd_start(char *cur)
{
	while (*cur && isblank(*cur))
	{
		cur++;
	}
	return cur;
}

static char* cmd_start_sameline(char *cur)
{
	while (*cur && isblank(*cur) && *cur != '\n')
	{
		cur++;
	}
	return cur;
}

static char* word_end(char *cur)
{
	while (*cur && !isblank(*cur))
	{
		cur++;
	}
	return cur;
}

static char* next_line(char *cur)
{
	while (*cur && *cur != '\n')
	{
		cur++;
	}
	if (*cur) {
		return cur + 1;
	}
	return NULL;
}

static char* next_blank(char *cur)
{
	while (*cur && !isblank(*cur))
	{
		cur++;
	}
	return cur;
}

static uint32_t timecode_to_lba(char *timecode)
{
	char *end;
	int seconds = 0, minutes = 0;
	int frames = strtol(timecode, &end, 10);
	if (end && *end == ':') {
		timecode = end + 1;
		seconds = frames;
		frames = strtol(timecode, &end, 10);
		if (end && *end == ':') {
			minutes = seconds;
			seconds = frames;
			timecode = end + 1;
			frames = strtol(timecode, NULL, 10);
		}
	}
	seconds += minutes * 60;
	return seconds * 75 + frames;

}

enum {
	FAKE_DATA = 1,
	FAKE_AUDIO,
};

static uint8_t bin_seek(system_media *media, uint32_t sector)
{
	media->cur_sector = sector;
	uint32_t lba = sector;
	uint32_t track;
	uint32_t rel;
	for (track = 0; track < media->num_tracks; track++)
	{
		rel = lba - media->tracks[track].pregap_lba;
		if (rel < media->tracks[track].fake_pregap) {
			media->in_fake_pregap = media->tracks[track].type == TRACK_DATA ? FAKE_DATA : FAKE_AUDIO;
			break;
		}
		if (lba < media->tracks[track].end_lba) {
			media->in_fake_pregap = 0;
			rel -= media->tracks[track].fake_pregap;
			break;
		}
	}
	if (track < media->num_tracks) {
		media->cur_track = track;
		if (!media->in_fake_pregap) {
			if (media->tracks[track].flac) {
				flac_seek(media->tracks[track].flac, (media->tracks[track].file_offset + rel * media->tracks[track].sector_bytes) / 4);
			} else {
				if (media->tracks[track].has_subcodes) {
					if (!media->tmp_buffer) {
						media->tmp_buffer = calloc(1, 96);
					}
					media_fseek(media->tracks[track].f, media->tracks[track].file_offset + (rel + 1) * media->tracks[track].sector_bytes - 96, SEEK_SET);
					int bytes = media_fread(media->tmp_buffer, 1, 96, media->tracks[track].f);
					if (bytes != 96) {
						fprintf(stderr, "Only read %d subcode bytes\n", bytes);
					}
				}
				media_fseek(media->tracks[track].f, media->tracks[track].file_offset + rel * media->tracks[track].sector_bytes, SEEK_SET);
			}
		}
		if (media->tracks[track].type == TRACK_DATA) {
			media->cdrom_scramble_lsfr = 1;
		}
	}
	return track;
}

uint8_t cdimage_fake_read(uint32_t sector, uint32_t offset)
{
	if (!offset || offset == 11 || (offset >= 16)) {
		return 0;
		//TODO: error detection and correction bytes
	} else if (offset < 11) {
		return 0xFF;
	} else if (offset == 12) {
		uint32_t minute = (sector / 75) / 60;
		return (minute % 10) | ((minute / 10 ) << 4);
	} else if (offset == 13) {
		uint32_t seconds = (sector / 75) % 60;
		return (seconds % 10) | ((seconds / 10 ) << 4);
	} else if (offset == 14) {
		uint32_t frames = sector % 75;
		return (frames % 10) | ((frames / 10 ) << 4);
	} else {
		return 1;
	}
}

static uint8_t bin_read(system_media *media, uint32_t offset)
{
	uint8_t retval;
	if (media->in_fake_pregap == FAKE_DATA) {
		retval = cdimage_fake_read(media->cur_sector, offset);
	} else if (media->in_fake_pregap == FAKE_AUDIO) {
		retval = 0;
	} else if ((media->tracks[media->cur_track].sector_bytes < 2352 && offset < 16) || offset > (media->tracks[media->cur_track].sector_bytes + 16)) {
		retval = cdimage_fake_read(media->cur_sector, offset);
	} else if (media->tracks[media->cur_track].flac) {
		if (offset & 3) {
			retval = media->byte_storage[(offset & 3) - 1];
		} else {
			int16_t samples[2];
			flac_get_sample(media->tracks[media->cur_track].flac, samples, 2);
			media->byte_storage[0] = samples[0] >> 8;
			media->byte_storage[1] = samples[1];
			media->byte_storage[2] = samples[1] >> 8;
			retval = samples[0];
		}
	} else {
		if (media->tracks[media->cur_track].need_swap) {
			if (offset & 1) {
				retval = media->byte_storage[0];
			}
			media->byte_storage[0] = media_fgetc(media->tracks[media->cur_track].f);
		} else {
			retval = media_fgetc(media->tracks[media->cur_track].f);
		}
	}
	if (offset >= 12 && media->tracks[media->cur_track].type == TRACK_DATA) {
		retval = cdrom_scramble(&media->cdrom_scramble_lsfr, retval);
	}
	return retval;
}

static uint8_t bin_subcode_read(system_media *media, uint32_t offset)
{
	if (media->in_fake_pregap || !media->tracks[media->cur_track].has_subcodes) {
		//TODO: Fake PQ subcodes
		return 0;
	}
	//TODO: Translate "cooked" subcodes back to raw format
	return media->tmp_buffer[offset];
}

void cdimage_print_toc(system_media *media)
{
	track_info * tracks = media->tracks;
	for (uint32_t i = 0; i < media->num_tracks; i++)
	{
		uint32_t m,s,f;
		f = tracks[i].pregap_lba % 75;
		s = tracks[i].pregap_lba / 75;
		m = s / 60;
		s = s % 60;
		printf("Track %02u - Index 0 %02u:%02u:%02u, Index 1 ", i + 1, m, s, f);
		f = tracks[i].start_lba % 75;
		s = tracks[i].start_lba / 75;
		m = s / 60;
		s = s % 60;
		printf("%02u:%02u:%02u, Fake Pregap: %u\n", m, s, f, tracks[i].fake_pregap);
	}
}

uint8_t parse_cue(system_media *media)
{
	char *line = media->buffer;
	media->num_tracks = 0;
	do {
		char *cmd = cmd_start(line);
		if (cmd) {
			if (startswith(cmd, "TRACK ")) {
				media->num_tracks++;
			}
			line = next_line(cmd);
		} else {
			line = NULL;
		}
	} while (line);
	track_info *tracks = calloc(sizeof(track_info), media->num_tracks);
	media->tracks = tracks;
	line = media->buffer;
	int track = -1;
	uint8_t audio_byte_swap = 0;
	media_file *f = NULL;
	flac_file *flac = NULL;
	int track_of_file = -1;
	uint8_t has_index_0 = 0;
	uint32_t extra_offset = 0;
	do {
		char *cmd = cmd_start(line);
		if (*cmd) {
			if (startswith(cmd, "TRACK ")) {
				track++;
				track_of_file++;
				has_index_0 = 0;
				cmd += 6;
				char *end;
				int file_track = strtol(cmd, &end, 10);
				if (file_track != (track + 1)) {
					warning("Expected track %d, but found track %d in CUE sheet\n", track + 1, file_track);
				}
				tracks[track].f = f;
				tracks[track].flac = flac;

				cmd = cmd_start(end);
				if (*cmd) {
					if (startswith(cmd, "AUDIO")) {
						tracks[track].type = TRACK_AUDIO;
						tracks[track].need_swap = audio_byte_swap;
						tracks[track].sector_bytes = 2352;
					} else {
						tracks[track].type = TRACK_DATA;
						tracks[track].need_swap = 0;
						tracks[track].sector_bytes = 0;
						char *slash = strchr(cmd, '/');
						if (slash) {
							tracks[track].sector_bytes = atoi(slash+1);
						}
						if (!tracks[track].sector_bytes) {
							warning("Missing sector size for data track %d in cue", track + 1);
							tracks[track].sector_bytes = 2352;
						}
					}

				}
			} else if (startswith(cmd, "FILE ")) {
				cmd += 5;
				cmd = strchr(cmd, '"');
				if (cmd) {
					cmd++;
					char *end = strchr(cmd, '"');
					if (end) {
						char *fname;
						//TODO: zipped BIN/CUE support
						if (is_absolute_path(cmd)) {
							fname = malloc(end-cmd + 1);
							memcpy(fname, cmd, end-cmd);
							fname[end-cmd] = 0;
						} else {
							size_t dirlen = strlen(media->dir);
							fname = malloc(dirlen + 1 + (end-cmd) + 1);
							memcpy(fname, media->dir, dirlen);
							fname[dirlen] = PATH_SEP[0];
							memcpy(fname + dirlen + 1, cmd, end-cmd);
							fname[dirlen + 1 + (end-cmd)] = 0;
						}
						
						if (track_of_file >= 0) {
							long track_size = 0;
							if (flac) {
								track_size = flac->total_samples * 4;
							} else if (f) {
								track_size = media_file_size(f);
							}
							track_size -= tracks[track].file_offset;
							tracks[track].end_lba = tracks[track].pregap_lba + tracks[track].fake_pregap + track_size / tracks[track].sector_bytes;
						}
						flac = NULL;
						f = media_fopen(fname, "rb");
						if (!f) {
							fatal_error("Failed to open %s specified by FILE command in CUE sheet %s.%s\n", fname, media->name, media->extension);
						}

						track_of_file = -1;
						for (end++; *end && *end != '\n' && *end != '\r'; end++)
						{
							if (!isspace(*end)) {
								extra_offset = 0;
								if (startswith(end, "BINARY")) {
									audio_byte_swap = 0;
								} else if (startswith(end, "MOTOROLA")) {
									audio_byte_swap = 1;
								} else if (startswith(end, "WAVE")) {
									audio_byte_swap = 0;
									wave_header wave;
									if (wave_read_header(f, &wave)) {
										if (wave.audio_format != 1 || wave.num_channels != 2 || wave.sample_rate != 44100 || wave.bits_per_sample != 16) {
											warning("BlastEm only suports WAVE tracks in 16-bit stereo PCM format at 44100 hz, file %s does not match\n", fname);
										}
										extra_offset = wave.data_offset;
									} else {
										media_fseek(f, 0, SEEK_SET);
										flac = flac_file_from_file(f);
										if (!flac) {
											fatal_error("WAVE file %s in cue sheet %s.%s is neither a valid WAVE nor a valid FLAC file\n", fname, media->name, media->extension);
										}
										if (flac->sample_rate != 44100 || flac->bits_per_sample != 16 || flac->channels != 2) {
											warning("FLAC files in a CUE sheet should match CD audio specs, %s does not\n", fname);
										}

									}
								} else {
									warning("Unsupported FILE type in CUE sheet. Only BINARY and MOTOROLA are supported\n");
								}
								break;
							}
						}
						free(fname);
					}
				}
			} else if (track >= 0) {
				if (startswith(cmd, "PREGAP ")) {
					tracks[track].fake_pregap = timecode_to_lba(cmd + 7);
				} else if (startswith(cmd, "INDEX ")) {
					char *after;
					int index = strtol(cmd + 6, &after, 10);
					uint8_t has_start_lba = 0;
					uint32_t start_lba = timecode_to_lba(after);
					if (!index) {
						tracks[track].file_offset = start_lba * tracks[track].sector_bytes + extra_offset;
						if (track > 0) {
							if (track_of_file > 0) {
								//Previous track end is implicit based on this index position
								uint32_t last_track_size = tracks[track].file_offset - tracks[track-1].file_offset;
								last_track_size /= tracks[track-1].sector_bytes;
								tracks[track-1].end_lba = tracks[track-1].pregap_lba + tracks[track-1].fake_pregap + last_track_size;
							}
							tracks[track].pregap_lba = tracks[track-1].end_lba;
						} else {
							tracks[track].pregap_lba = 0;
						}
						has_index_0 = 1;
						has_start_lba = 1;
					} else if (index == 1) {
						if (has_index_0) {
							uint32_t real_pregap_size = start_lba * tracks[track].sector_bytes + extra_offset - tracks[track].file_offset;
							real_pregap_size /= tracks[track].sector_bytes;
							tracks[track].start_lba = tracks[track].pregap_lba + tracks[track].fake_pregap + real_pregap_size;
						} else {
							tracks[track].file_offset = start_lba * tracks[track].sector_bytes + extra_offset;
							if (track > 0) {
								if (track_of_file > 0) {
									//Previous track end is implicit based on this index position
									uint32_t last_track_size = tracks[track].file_offset - tracks[track-1].file_offset;
									last_track_size /= tracks[track-1].sector_bytes;
									tracks[track-1].end_lba = tracks[track-1].pregap_lba + tracks[track-1].fake_pregap + last_track_size;
								}
								tracks[track].pregap_lba = tracks[track-1].end_lba;
								tracks[track].start_lba = tracks[track].pregap_lba + tracks[track].fake_pregap;
							} else {
								tracks[track].pregap_lba = 0;
								if (!tracks[track].fake_pregap) {
									if (tracks[track].type == TRACK_DATA && tracks[track].sector_bytes == 2352) {
										//Infer pregap from position in sector header
										media_fseek(f, start_lba + 12, SEEK_SET);
										uint8_t timecode[3];
										if (sizeof(timecode) == media_fread(timecode, 1, sizeof(timecode), f)) {
											tracks[track].fake_pregap = (timecode[0] >> 4) * 600;
											tracks[track].fake_pregap += (timecode[0] & 0xF) * 60;
											tracks[track].fake_pregap += (timecode[1] >> 4) * 10;
											tracks[track].fake_pregap += timecode[1] & 0xF;
											tracks[track].fake_pregap *= 75;
											tracks[track].fake_pregap += (timecode[2] >> 4) * 10;
											tracks[track].fake_pregap += timecode[2] & 0xF;
										} else {
											fatal_error("Failed to read from CD image");
										}
									} else {
										//Just assume a 2-second pre-gap for first track
										tracks[track].fake_pregap = 2 * 75;
									}
								}
								tracks[track].start_lba = tracks[track].fake_pregap;
							}
						}
					}
				}
			}
			if (cmd && *cmd) {
				line = next_line(cmd);
			} else {
				line = NULL;
			}
		} else {
			line = NULL;
		}
	} while (line);
	if (media->num_tracks > 0 && media->tracks[0].f) {
		//end of last track in a file is implictly based on file size
		long track_size = 0;
		if (flac) {
			track_size = flac->total_samples * 4;
		} else if (f) {
			track_size = media_file_size(f);
		}
		track_size -= tracks[track].file_offset;
		tracks[track].end_lba = tracks[track].pregap_lba + tracks[track].fake_pregap + track_size / tracks[track].sector_bytes;
		
		if (tracks[0].type == TRACK_DATA) {
			//replace cue sheet with first sector
			aligned_free(media->buffer);
			media->buffer = calloc(2048, 1);
			media_fseek(tracks[0].f, tracks[0].sector_bytes >= 2352 ? 16 : 0, SEEK_SET);
			media->size = media_fread(media->buffer, 1, 2048, tracks[0].f);
		}
		media->seek = bin_seek;
		media->read = bin_read;
		media->read_subcodes = bin_subcode_read;
	}
	cdimage_print_toc(media);
	uint8_t valid = media->num_tracks > 0 && media->tracks[0].f != NULL;
	media->type = valid ? MEDIA_CDROM : MEDIA_CART;
	return valid;
}

uint8_t parse_toc(system_media *media)
{
	char *line = media->buffer;
	media->num_tracks = 0;
	do {
		char *cmd = cmd_start(line);
		if (cmd) {
			if (startswith(cmd, "TRACK ")) {
				media->num_tracks++;
			}
			line = next_line(cmd);
		} else {
			line = NULL;
		}
	} while (line);
	track_info *tracks = calloc(sizeof(track_info), media->num_tracks);
	media->tracks = tracks;
	line = media->buffer;
	char *last_file_name = NULL;
	media_file *f = NULL;
	int track = -1;
	do {
		char *cmd = cmd_start(line);
		if (*cmd) {
			if (startswith(cmd, "TRACK ")) {
				track++;
				cmd = cmd_start(cmd + 6);
				if (startswith(cmd, "AUDIO")) {
					tracks[track].type = TRACK_AUDIO;
					tracks[track].sector_bytes = 2352;
					tracks[track].need_swap = 1;
				} else {
					tracks[track].type = TRACK_DATA;
					tracks[track].need_swap = 0;
					if (startswith(cmd, "MODE1_RAW") || startswith(cmd, "MODE2_RAW")) {
						tracks[track].sector_bytes = 2352;
					} else if (startswith(cmd, "MODE2_FORM2")) {
						tracks[track].sector_bytes = 2324;
					} else if (startswith(cmd, "MODE1") || startswith(cmd, "MODE2_FORM1")) {
						tracks[track].sector_bytes = 2048;
					} else if (startswith(cmd, "MODE2")) {
						tracks[track].sector_bytes = 2336;
					}
				}
				cmd = word_end(cmd);
				if (*cmd && *cmd != '\n') {
					cmd = cmd_start_sameline(cmd);
					if (*cmd && *cmd != '\n') {
						//TODO: record whether subcode is in raw format or not
						if (startswith(cmd, "RW_RAW")) {
							tracks[track].sector_bytes += 96;
							tracks[track].has_subcodes = SUBCODES_RAW;
						} else if (startswith(cmd, "RW")) {
							tracks[track].sector_bytes += 96;
							tracks[track].has_subcodes = SUBCODES_COOKED;
						}
					}
				}
				if (track) {
					tracks[track].start_lba = tracks[track].pregap_lba = tracks[track].end_lba = tracks[track-1].end_lba;
				}
			} else if (track >= 0) {
				uint8_t is_datafile = startswith(cmd, "DATAFILE");
				if (is_datafile || startswith(cmd, "FILE")) {

					if (tracks[track].f) {
						warning("TOC file has more than one file for track %d, only one is supported\n", track + 1);
					} else {
						cmd += is_datafile ? 8 : 4;
						char *fname_start = strchr(cmd, '"');
						if (fname_start) {
							++fname_start;
							char *fname_end = strchr(fname_start, '"');
							if (fname_end) {
								if (!last_file_name || strncmp(last_file_name, fname_start, fname_end-fname_start)) {
									free(last_file_name);
									last_file_name = calloc(1, 1 + fname_end-fname_start);
									memcpy(last_file_name, fname_start, fname_end-fname_start);
									char *fname;
									//TODO: zipped BIN/TOC support
									if (is_absolute_path(last_file_name)) {
										fname = last_file_name;
									} else {
										size_t dirlen = strlen(media->dir);
										fname = malloc(dirlen + 1 + (fname_end-fname_start) + 1);
										memcpy(fname, media->dir, dirlen);
										fname[dirlen] = PATH_SEP[0];
										memcpy(fname + dirlen + 1, fname_start, fname_end-fname_start);
										fname[dirlen + 1 + (fname_end-fname_start)] = 0;
									}
									f = media_fopen(fname, "rb");
									if (!f) {
										fatal_error("Failed to open %s specified by DATAFILE command in TOC file %s.%s\n", fname, media->name, media->extension);
									}
									if (fname != last_file_name) {
										free(fname);
									}
								}
								tracks[track].f = f;
								cmd = fname_end + 1;
								cmd = cmd_start_sameline(cmd);
								if (*cmd == '#') {
									char *end;
									tracks[track].file_offset = strtol(cmd + 1, &end, 10);
									cmd = cmd_start_sameline(end);
								}
								if (!is_datafile) {
									if (isdigit(*cmd)) {
										uint32_t start = timecode_to_lba(cmd);
										tracks[track].file_offset += start * tracks[track].sector_bytes;
										cmd = cmd_start_sameline(word_end(cmd));
									}
								}
								if (isdigit(*cmd)) {
									uint32_t length = timecode_to_lba(cmd);
									tracks[track].end_lba += length;
								} else {
									long fsize = media_file_size(f);
									tracks[track].end_lba += (fsize - tracks[track].file_offset) / tracks[track].sector_bytes;
								}
							}
						}
					}
				} else if (startswith(cmd, "SILENCE")) {
					cmd = cmd_start_sameline(cmd + 7);
					uint32_t length = timecode_to_lba(cmd);
					tracks[track].fake_pregap += length;
					tracks[track].end_lba += length;
				} else if (startswith(cmd, "START")) {
					cmd = cmd_start_sameline(cmd + 5);
					tracks[track].start_lba = tracks[track].pregap_lba + timecode_to_lba(cmd);
				}
			}
			if (cmd && *cmd) {
				line = next_line(cmd);
			} else {
				line = NULL;
			}
		} else {
			line = NULL;
		}
	} while (line);
	if (media->num_tracks > 0 && media->tracks[0].f) {
		//replace cue sheet with first sector
		aligned_free(media->buffer);
		media->buffer = calloc(2048, 1);
		uint32_t old_fake_pregap = tracks[0].fake_pregap;
		if (tracks[0].type == TRACK_DATA && tracks[0].sector_bytes == 2352) {
			// if the first track is a data track, don't trust the TOC file and look at the MM:SS:FF from first sector
			uint8_t msf[3];
			media_fseek(tracks[0].f, 12, SEEK_SET);
			if (sizeof(msf) == media_fread(msf, 1, sizeof(msf), tracks[0].f)) {
				tracks[0].fake_pregap = msf[2] + (msf[0] * 60 + msf[1]) * 75;
			}
		} else if (!tracks[0].fake_pregap) {
			tracks[0].fake_pregap = 2 * 75;
		}
		if (tracks[0].fake_pregap != old_fake_pregap) {
			if (!tracks[0].start_lba) {
				tracks[0].start_lba = tracks[0].fake_pregap;
			}
			uint32_t diff = tracks[0].fake_pregap - old_fake_pregap;
			tracks[0].end_lba += diff;
			for (uint32_t i = 1; i < media->num_tracks; i++)
			{
				tracks[i].pregap_lba += diff;
				tracks[i].start_lba += diff;
				tracks[i].end_lba += diff;
			}
		}

		media_fseek(tracks[0].f, tracks[0].sector_bytes == 2352 ? 16 : 0, SEEK_SET);
		media->size = media_fread(media->buffer, 1, 2048, tracks[0].f);
		media->seek = bin_seek;
		media->read = bin_read;
		media->read_subcodes = bin_subcode_read;
	}
	cdimage_print_toc(media);
	uint8_t valid = media->num_tracks > 0 && media->tracks[0].f != NULL;
	media->type = valid ? MEDIA_CDROM : MEDIA_CART;
	return valid;
}

uint32_t make_iso_media(system_media *media, const char *filename)
{
	media_file *f = media_fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	media->buffer = calloc(2048, 1);
	media->size = media_fread(media->buffer, 1, 2048, f);
	media->num_tracks = 1;
	media->tracks = calloc(sizeof(track_info), 1);
	media->tracks[0] = (track_info){
		.f = f,
		.file_offset = 0,
		.fake_pregap = 2 * 75,
		.start_lba = 0,
		.end_lba = media_file_size(f),
		.sector_bytes = 2048,
		.has_subcodes = SUBCODES_NONE,
		.need_swap = 0,
		.type = TRACK_DATA
	};
	media->type = MEDIA_CDROM;
	media->seek = bin_seek;
	media->read = bin_read;
	media->read_subcodes = bin_subcode_read;
	return media->size;
}

void cdimage_serialize(system_media *media, serialize_buffer *buf)
{
	if (media->type != MEDIA_CDROM) {
		return;
	}
	save_int32(buf, media->cur_track);
	save_int32(buf, media->cur_sector);
	if (media->cur_track < media->num_tracks && media->tracks[media->cur_track].f) {
		save_int32(buf, media_ftell(media->tracks[media->cur_track].f));
	} else {
		save_int32(buf, 0);
	}
	save_int8(buf, media->in_fake_pregap);
	save_int8(buf, media->byte_storage[0]);
	if (media->tmp_buffer) {
		save_buffer8(buf, media->tmp_buffer, 96);
	}
	save_int8(buf, media->byte_storage[1]);
	save_int8(buf, media->byte_storage[2]);
}

void cdimage_deserialize(deserialize_buffer *buf, void *vmedia)
{
	system_media *media = vmedia;
	if (media->type != MEDIA_CDROM) {
		return;
	}
	media->cur_track = load_int32(buf);
	media->cur_sector = load_int32(buf);
	uint32_t seekpos = load_int32(buf);
	if (media->cur_track < media->num_tracks && media->tracks[media->cur_track].f) {
		media_fseek(media->tracks[media->cur_track].f, seekpos, SEEK_SET);
	}
	media->in_fake_pregap = load_int8(buf);
	media->byte_storage[0] = load_int8(buf);
	if (media->tmp_buffer) {
		load_buffer8(buf, media->tmp_buffer, 96);
	}
	if (buf->size - buf->cur_pos >= 2) {
		media->byte_storage[1] = load_int8(buf);
		media->byte_storage[2] = load_int8(buf);
	}
}

void cdimage_free(system_media *media)
{
	if (media->type != MEDIA_CDROM) {
		return;
	}
	//dir, name, extension and orig_path are handled elsewhere
	chdimage_free(media);
	free(media->buffer);
	free(media->tracks);
}
