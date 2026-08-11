/*
 CHD (MAME Compressed Hunks of Data) CD images.

 A CHD keeps every track in one file, so this fills in the same track_info table
 the cue and toc parsers build and then serves sectors out of the CHD instead of
 out of a per-track FILE. Layout, from MAME:

  - Every sector occupies CD_FRAME_SIZE (2448) bytes: the sector data from byte
    0, and 96 bytes of subcode from byte 2352, whatever the track's data size is.
  - Tracks are stored back to back, each padded out to a multiple of four
    frames, so a track's first frame is not simply the sum of the frames before
    it.
  - CD-DA is stored big endian, the opposite of what a BIN file holds, which is
    the same situation the toc parser is in.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#include "system.h"
#include "cdimage.h"
#include "util.h"

#define FAKE_DATA 1
#define FAKE_AUDIO 2

//The metadata strings are short; MAME writes well under 256 bytes.
#define METADATA_MAX 512

static uint8_t read_frame(system_media *media, uint32_t frame)
{
	if (media->chd_frame_valid && media->chd_frame_no == frame) {
		return 1;
	}
	const chd_header *header = chd_get_header(media->chd);
	uint32_t frames_per_hunk = header->hunkbytes / CD_FRAME_SIZE;
	if (!frames_per_hunk) {
		return 0;
	}
	uint32_t hunk = frame / frames_per_hunk;
	if (hunk >= header->totalhunks) {
		return 0;
	}
	if (!media->chd_hunk_valid || media->chd_hunk_no != hunk) {
		if (chd_read(media->chd, hunk, media->chd_hunk) != CHDERR_NONE) {
			media->chd_hunk_valid = 0;
			return 0;
		}
		media->chd_hunk_no = hunk;
		media->chd_hunk_valid = 1;
	}
	memcpy(media->chd_frame, media->chd_hunk + (frame % frames_per_hunk) * CD_FRAME_SIZE, CD_FRAME_SIZE);
	media->chd_frame_no = frame;
	media->chd_frame_valid = 1;
	return 1;
}

//Mirrors bin_seek(), including which track a given LBA belongs to and how a
//pregap that is not in the image is flagged, but resolves to a CHD frame rather
//than a file position.
static uint8_t chdmedia_seek(system_media *media, uint32_t sector)
{
	media->cur_sector = sector;
	uint32_t lba = sector;
	uint32_t track;
	uint32_t rel = 0;
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
			if (!read_frame(media, media->tracks[track].file_offset + rel)) {
				//Leave the stale frame flagged as invalid; the read functions
				//return zeroes rather than the previous sector's contents.
				media->chd_frame_valid = 0;
			}
		}
		if (media->tracks[track].type == TRACK_DATA) {
			media->cdrom_scramble_lsfr = 1;
		}
	}
	return track;
}

//The offset the emulated drive asks for is the offset into a full 2352 byte
//sector, so a track stored cooked needs its header faked exactly as bin_read()
//fakes it, and the data lines up 16 bytes in.
static uint8_t chdmedia_read(system_media *media, uint32_t offset)
{
	uint8_t retval;
	track_info *track = media->tracks + media->cur_track;
	if (media->in_fake_pregap == FAKE_DATA) {
		retval = cdimage_fake_read(media->cur_sector, offset);
	} else if (media->in_fake_pregap == FAKE_AUDIO) {
		retval = 0;
	} else if ((track->sector_bytes < 2352 && offset < 16) || offset > (track->sector_bytes + 16)) {
		retval = cdimage_fake_read(media->cur_sector, offset);
	} else if (!media->chd_frame_valid) {
		retval = 0;
	} else {
		uint32_t pos = track->sector_bytes < 2352 ? offset - 16 : offset;
		//CD-DA in a CHD is big endian, so the two bytes of a sample swap places.
		if (track->need_swap) {
			pos ^= 1;
		}
		retval = pos < CD_MAX_SECTOR_DATA ? media->chd_frame[pos] : 0;
	}
	if (offset >= 12 && track->type == TRACK_DATA) {
		retval = cdrom_scramble(&media->cdrom_scramble_lsfr, retval);
	}
	return retval;
}

static uint8_t chdmedia_subcode_read(system_media *media, uint32_t offset)
{
	if (media->in_fake_pregap || !media->tracks[media->cur_track].has_subcodes || !media->chd_frame_valid) {
		return 0;
	}
	return offset < CD_MAX_SUBCODE_DATA ? media->chd_frame[CD_MAX_SECTOR_DATA + offset] : 0;
}

static void parse_track_type(track_info *track, const char *type)
{
	if (!strcmp(type, "AUDIO")) {
		track->type = TRACK_AUDIO;
		track->sector_bytes = 2352;
		track->need_swap = 1;
		return;
	}
	track->type = TRACK_DATA;
	track->need_swap = 0;
	if (!strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW")) {
		track->sector_bytes = 2352;
	} else if (!strcmp(type, "MODE2_FORM2")) {
		track->sector_bytes = 2324;
	} else if (!strcmp(type, "MODE1") || !strcmp(type, "MODE2_FORM1")) {
		track->sector_bytes = 2048;
	} else {
		//MODE2 and MODE2_FORM_MIX
		track->sector_bytes = 2336;
	}
}

//Unlike a cue sheet, where the subcode shares the sector with the data, a CHD
//always has room for 96 bytes of it - the subtype only says whether any was
//stored.
static uint8_t parse_subcode_type(const char *subtype)
{
	if (!strcmp(subtype, "RW_RAW")) {
		return SUBCODES_RAW;
	}
	if (!strcmp(subtype, "RW")) {
		return SUBCODES_COOKED;
	}
	return SUBCODES_NONE;
}

static uint8_t read_track_metadata(chd_file *chd, uint32_t index, track_info *track, uint32_t *frames_out)
{
	char metadata[METADATA_MAX];
	char type[32], subtype[32], pgtype[32], pgsub[32];
	uint32_t resultlen = 0;
	int tracknum = 0, frames = 0, pregap = 0, postgap = 0;

	if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, metadata, sizeof(metadata) - 1, &resultlen, NULL, NULL) == CHDERR_NONE) {
		metadata[resultlen < sizeof(metadata) ? resultlen : sizeof(metadata) - 1] = 0;
		if (sscanf(metadata, CDROM_TRACK_METADATA2_FORMAT, &tracknum, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap) != 8) {
			return 0;
		}
	} else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, metadata, sizeof(metadata) - 1, &resultlen, NULL, NULL) == CHDERR_NONE) {
		metadata[resultlen < sizeof(metadata) ? resultlen : sizeof(metadata) - 1] = 0;
		if (sscanf(metadata, CDROM_TRACK_METADATA_FORMAT, &tracknum, type, subtype, &frames) != 4) {
			return 0;
		}
		strcpy(pgtype, "V");
	} else {
		return 0;
	}

	parse_track_type(track, type);
	track->has_subcodes = parse_subcode_type(subtype);
	//A pregap type starting with V is one MAME generated rather than stored, so
	//those frames are not in the image and blastem has to fake them. Anything
	//else means the pregap is part of the frames the metadata counts.
	track->fake_pregap = pgtype[0] == 'V' ? pregap : 0;
	*frames_out = frames;
	return 1;
}

//libchdr can be handed either a path or a core_file of callbacks. The libretro
//build takes the second route so a CHD behind the frontend's VFS - an Android
//content:// URI, a file on an SMB share - opens like any other. media_file is
//plain stdio outside that build, so this is the same code either way.
static uint64_t chd_media_fsize(struct chd_core_file *file)
{
	long size = media_file_size((media_file *)file->argp);
	return size < 0 ? (uint64_t)-1 : (uint64_t)size;
}

static size_t chd_media_fread(void *dst, size_t size, size_t count, struct chd_core_file *file)
{
	return media_fread(dst, size, count, (media_file *)file->argp);
}

static int chd_media_fclose(struct chd_core_file *file)
{
	int ret = media_fclose((media_file *)file->argp);
	free(file);
	return ret;
}

static int chd_media_fseek(struct chd_core_file *file, int64_t offset, int whence)
{
	return media_fseek((media_file *)file->argp, offset, whence);
}

//Owns the media_file: a successful chd_open_core_file() hands the lifetime to
//libchdr, which closes it through the callback above.
static core_file *chd_media_open(const char *path)
{
	media_file *f = media_fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	core_file *cf = calloc(1, sizeof(core_file));
	if (!cf) {
		media_fclose(f);
		return NULL;
	}
	cf->argp = f;
	cf->fsize = chd_media_fsize;
	cf->fread = chd_media_fread;
	cf->fclose = chd_media_fclose;
	cf->fseek = chd_media_fseek;
	return cf;
}

//chd_open() fails on a zstd compressed CHD because the codec is stubbed out (see
//libchdr/zstd.h), and a failed open says nothing about why. The header parses
//without any codec, so ask it directly.
static uint8_t uses_zstd(const char *path)
{
	chd_header header;
	core_file *cf = chd_media_open(path);
	if (!cf) {
		return 0;
	}
	chd_error err = chd_read_header_core_file(cf, &header);
	core_fclose(cf);
	if (err != CHDERR_NONE) {
		return 0;
	}
	for (int i = 0; i < 4; i++)
	{
		if (header.compression[i] == CHD_CODEC_ZSTD || header.compression[i] == CHD_CODEC_CD_ZSTD) {
			return 1;
		}
	}
	return 0;
}

uint8_t parse_chd(system_media *media)
{
	chd_file *chd = NULL;
	core_file *cf = chd_media_open(media->orig_path);
	chd_error err = cf ? chd_open_core_file(cf, CHD_OPEN_READ, NULL, &chd) : CHDERR_FILE_NOT_FOUND;
	if (err != CHDERR_NONE) {
		//libchdr closes the core_file for us here: its failure path runs
		//chd_close(), which calls the fclose callback. The one exception is
		//failing to allocate its own handle, an OOM path not worth unwinding.
		//The one codec this build leaves out is the one worth naming, since the
		//file is perfectly good and chdman can rewrite it with another.
		if (uses_zstd(media->orig_path)) {
			warning("%s is compressed with zstd, which this build cannot read. Recompress it with 'chdman createcd -c cdlz,cdzl,cdfl'\n", media->orig_path);
		} else {
			warning("Failed to open %s as a CHD: %s\n", media->orig_path, chd_error_string(err));
		}
		return 0;
	}

	uint32_t num_tracks = 0;
	track_info scratch;
	uint32_t frames;
	while (read_track_metadata(chd, num_tracks, &scratch, &frames))
	{
		num_tracks++;
	}
	if (!num_tracks) {
		warning("%s has no CD track metadata, so it is not a CD image\n", media->orig_path);
		chd_close(chd);
		return 0;
	}

	track_info *tracks = calloc(num_tracks, sizeof(track_info));
	media->tracks = tracks;
	media->num_tracks = num_tracks;
	media->chd = chd;

	uint32_t lba = 0;
	uint32_t chd_frame = 0;
	for (uint32_t i = 0; i < num_tracks; i++)
	{
		read_track_metadata(chd, i, tracks + i, &frames);
		tracks[i].pregap_lba = lba;
		//file_offset is the frame the track starts at within the CHD rather than
		//a byte offset; nothing outside this file interprets it.
		tracks[i].file_offset = chd_frame;
		tracks[i].start_lba = lba + tracks[i].fake_pregap;
		tracks[i].end_lba = tracks[i].start_lba + frames;
		lba = tracks[i].end_lba;
		//Each track is padded out to a four frame boundary in the CHD.
		chd_frame += frames;
		chd_frame += (CD_TRACK_PADDING - (frames % CD_TRACK_PADDING)) % CD_TRACK_PADDING;
	}

	const chd_header *header = chd_get_header(chd);
	media->chd_hunk = calloc(1, header->hunkbytes);
	media->chd_frame = calloc(1, CD_FRAME_SIZE);
	media->chd_hunk_valid = 0;
	media->chd_frame_valid = 0;

	media->seek = chdmedia_seek;
	media->read = chdmedia_read;
	media->read_subcodes = chdmedia_subcode_read;
	media->type = MEDIA_CDROM;

	//Same as the cue parser: the buffer detection looks at has to be the first
	//sector of the first data track, not the file the user named.
	if (tracks[0].type == TRACK_DATA) {
		free(media->buffer);
		media->buffer = calloc(2048, 1);
		media->size = 0;
		if (read_frame(media, tracks[0].file_offset)) {
			uint32_t data_start = tracks[0].sector_bytes >= 2352 ? 16 : 0;
			memcpy(media->buffer, media->chd_frame + data_start, 2048);
			media->size = 2048;
		}
	}

	cdimage_print_toc(media);
	return 1;
}

void chdimage_free(system_media *media)
{
	if (media->chd) {
		chd_close(media->chd);
		media->chd = NULL;
	}
	free(media->chd_hunk);
	media->chd_hunk = NULL;
	free(media->chd_frame);
	media->chd_frame = NULL;
}
