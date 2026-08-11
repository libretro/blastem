#ifndef FLAC_H_
#define FLAC_H_

#include <stdint.h>
#include <stdio.h>
#include "media_file.h"

typedef struct flac_file flac_file;

typedef uint8_t (*flac_read_fun)(flac_file *f);
typedef void (*flac_seek_fun)(flac_file *f, uint32_t offset, uint8_t relative);
typedef uint32_t (*flac_tell_fun)(flac_file *f);

typedef struct {
	uint32_t allocated_samples;
	int32_t *decoded;
} flac_subframe;

typedef struct {
	uint64_t sample_number;
	uint64_t offset;
	uint16_t sample_count;
} flac_seekpoint;

struct flac_file {
	uint64_t       total_samples;
	uint64_t       frame_start_sample;
	void           *read_data;
	flac_read_fun  read_byte;
	flac_seek_fun  seek;
	flac_tell_fun  tell;
	flac_subframe  *subframes;
	flac_seekpoint *seekpoints;
	uint32_t       num_seekpoints;
	uint32_t       offset;
	uint32_t       buffer_size;
	uint32_t       first_frame_offset;

	uint32_t       frame_sample_pos;

	uint32_t       sample_rate;
	uint32_t       frame_sample_rate;
	uint32_t       frame_block_size;
	uint8_t        bits_per_sample;
	uint8_t        frame_bits_per_sample;
	uint8_t        channels;
	uint8_t        frame_channels;
	uint8_t        frame_joint_stereo;
	uint8_t        subframe_alloc;

	uint8_t        cur_byte;
	uint8_t        bits;
};

flac_file *flac_file_from_buffer(void *buffer, uint32_t size);
flac_file *flac_file_from_file(media_file *file);
uint8_t flac_get_sample(flac_file *f, int16_t *out, uint8_t desired_channels);
void flac_seek(flac_file *f, uint64_t sample_number);

#endif //FLAC_H_
