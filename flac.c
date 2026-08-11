#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "flac.h"

static uint8_t read_byte_buffer(flac_file *f)
{
	if (f->offset >= f->buffer_size) {
		return 0;
	}
	uint8_t *buf = f->read_data;
	return buf[f->offset++];
}

static void seek_buffer(flac_file *f, uint32_t offset, uint8_t relative)
{
	f->offset = relative ? f->offset + offset : offset;
}

static uint32_t tell_buffer(flac_file *f)
{
	return f->offset;
}

static uint8_t read_byte_file(flac_file *f)
{
	int result = media_fgetc(f->read_data);
	if (result == EOF) {
		return 0;
	}
	return result;
}

static void seek_file(flac_file *f, uint32_t offset, uint8_t relative)
{
	media_fseek(f->read_data, offset, relative ? SEEK_CUR : SEEK_SET);
}

static uint32_t tell_file(flac_file *f)
{
	return media_ftell(f->read_data);
}

static void read_chars(flac_file *f, char *dest, uint32_t count)
{
	for (; count > 0; --count)
	{
		*(dest++) = f->read_byte(f);
	}
}

static uint16_t read16(flac_file *f)
{
	uint16_t ret = f->read_byte(f) << 8;
	ret |= f->read_byte(f);
	return ret;
}

static uint64_t read64(flac_file *f)
{
	uint64_t value = ((uint64_t)f->read_byte(f)) << 56;
	value |= ((uint64_t)f->read_byte(f)) << 48;
	value |= ((uint64_t)f->read_byte(f)) << 40;
	value |= ((uint64_t)f->read_byte(f)) << 32;
	value |= ((uint64_t)f->read_byte(f)) << 24;
	value |= f->read_byte(f) << 16;
	value |= f->read_byte(f) << 8;
	value |= f->read_byte(f);
	return value;
}

static uint32_t read_bits(flac_file *f, uint32_t num_bits)
{
	uint32_t ret = 0;
	while (num_bits)
	{
		if (!f->bits) {
			f->cur_byte = f->read_byte(f);
			f->bits = 8;
		}
		uint32_t new_bits = f->bits;
		if (new_bits > num_bits) {
			new_bits = num_bits;
		}
		ret <<= new_bits;
		uint32_t mask = (1 << new_bits) - 1;
		ret |= (f->cur_byte >> (f->bits - new_bits)) & mask;
		f->bits -= new_bits;
		num_bits -= new_bits;
	}
	return ret;
}

static uint64_t read_bits64(flac_file *f, uint64_t num_bits)
{
	uint64_t ret = 0;
	while (num_bits)
	{
		if (!f->bits) {
			f->cur_byte = f->read_byte(f);
			f->bits = 8;
		}
		uint64_t new_bits = f->bits;
		if (new_bits > num_bits) {
			new_bits = num_bits;
		}
		ret <<= new_bits;
		uint64_t mask = (1 << new_bits) - 1;
		ret |= f->cur_byte & mask;
		f->cur_byte >>= new_bits;
		f->bits -= new_bits;
		num_bits -= new_bits;
	}
	return ret;
}

typedef struct {
	uint32_t size;
	uint8_t  type;
	uint8_t  is_last;
} meta_block_header;

enum {
	STREAMINFO,
	PADDING,
	APPLICATION,
	SEEKTABLE,
	VORBIS_COMMENT,
	CUESHEET,
	PICTURE
};

static void read_meta_block_header(flac_file *f, meta_block_header *dest)
{
	dest->is_last = read_bits(f, 1);
	dest->type = read_bits(f, 7);
	dest->size = read_bits(f, 24);
}

static void parse_streaminfo(flac_file *f)
{
	read16(f);//min block size
	read16(f);//max block size
	read_bits(f, 24);//min frame size
	read_bits(f, 24);//max frame size
	f->sample_rate = read_bits(f, 20);
	f->channels = read_bits(f, 3) + 1;
	f->bits_per_sample = read_bits(f, 5) + 1;
	f->total_samples = read_bits64(f, 36);
	f->seek(f, 16, 1);//MD5
}

static void parse_seektable(flac_file *f, uint32_t size)
{
	f->num_seekpoints = size / 18;
	f->seekpoints = calloc(f->num_seekpoints, sizeof(flac_seekpoint));
	for (uint32_t i = 0; i < f->num_seekpoints; i++)
	{
		f->seekpoints[i].sample_number = read64(f);
		f->seekpoints[i].offset = read64(f);
		f->seekpoints[i].sample_count = read16(f);
	}
}

static uint8_t parse_header(flac_file *f)
{
	char id[4];
	read_chars(f, id, sizeof(id));
	if (memcmp("fLaC", id, sizeof(id))) {
		return 0;
	}
	meta_block_header header;
	do {
		read_meta_block_header(f, &header);
		if (header.type == STREAMINFO) {
			parse_streaminfo(f);
		} else if (header.type == SEEKTABLE) {
			parse_seektable(f, header.size);
		} else {
			f->seek(f, header.size, 1);
		}
	} while (!header.is_last);
	f->first_frame_offset = f->tell(f);
	return 1;
}

flac_file *flac_file_from_buffer(void *buffer, uint32_t size)
{
	flac_file *f = calloc(1, sizeof(flac_file));
	f->read_data = buffer;
	f->read_byte = read_byte_buffer;
	f->seek = seek_buffer;
	f->tell = tell_buffer;
	f->buffer_size = size;
	if (parse_header(f)) {
		return f;
	}
	free(f);
	return NULL;
}

flac_file *flac_file_from_file(media_file *file)
{
	flac_file *f = calloc(1, sizeof(flac_file));
	f->read_data = file;
	f->read_byte = read_byte_file;
	f->seek = seek_file;
	f->tell = tell_file;
	if (parse_header(f)) {
		return f;
	}
	free(f);
	return NULL;
}

static uint64_t read_utf64(flac_file *f)
{
	uint8_t byte = f->read_byte(f);
	if (!(byte & 0x80)) {
		return byte;
	}
	uint8_t mask = 0x40;
	uint8_t length = 0;
	while (byte & mask)
	{
		mask >>= 1;
		length++;
	}
	uint64_t value = byte & (mask - 1);
	for (uint8_t i = 0; i < length; i++)
	{
		value <<= 6;
		value |= f->read_byte(f) & 0x3F;
	}
	return value;
}

static uint32_t read_utf32(flac_file *f)
{
	uint8_t byte = f->read_byte(f);
	if (!(byte & 0x80)) {
		return byte;
	}
	uint8_t mask = 0x40;
	uint8_t length = 0;
	while (byte & mask)
	{
		mask >>= 1;
		length++;
	}
	uint32_t value = byte & (mask - 1);
	for (uint8_t i = 0; i < length; i++)
	{
		value <<= 6;
		value |= f->read_byte(f) & 0x3F;
	}
	return value;
}

static uint8_t parse_frame_header(flac_file *f)
{
	uint16_t sync = read_bits(f, 14);
	if (sync != 0x3FFE) {
		fprintf(stderr, "Invalid sync FLAC sync pattern: %X\n", sync);
		return 0;
	}
	read_bits(f, 1);//reserved
	uint8_t block_size_strategy = read_bits(f, 1);
	uint8_t block_size_code = read_bits(f, 4);
	uint8_t sample_rate_code = read_bits(f, 4);
	uint8_t channels = read_bits(f, 4);
	uint8_t joint_stereo = 0;
	if (channels > 7) {
		joint_stereo = (channels & 7) + 1;
		channels = 2;
	} else {
		++channels;
	}
	f->frame_channels = channels;
	f->frame_joint_stereo = joint_stereo;
	uint8_t bits_per_sample_code = read_bits(f, 3);
	if (!bits_per_sample_code) {
		f->frame_bits_per_sample = f->bits_per_sample;
	} else if (bits_per_sample_code < 3) {
		f->frame_bits_per_sample = 4 + 4 * bits_per_sample_code;
	} else {
		f->frame_bits_per_sample = 4 * bits_per_sample_code;
	}
	read_bits(f, 1);//reserved
	uint32_t block_num;
	if (block_size_strategy) {
		f->frame_start_sample = read_utf64(f);
	} else {
		block_num = read_utf32(f);
	}
	uint32_t block_size = 0;
	switch (block_size_code)
	{
	case 0:
		fputs("Detected reserved block size 0", stderr);
		return 0;
	case 1:
		block_size = 192;
		break;
	case 6:
		block_size = f->read_byte(f) + 1;
		break;
	case 7:
		block_size = read16(f) + 1;
		break;
	default:
		if (block_size_code < 8) {
			block_size = 576 * (1 << (block_size_code - 2));
		} else {
			block_size = 256 * (1 << (block_size_code - 8));
		}
		break;
	}
	f->frame_block_size = block_size;
	if (!block_size_strategy) {
		f->frame_start_sample  = ((uint64_t)block_num) * ((uint64_t)block_size);
	}
	uint32_t sample_rate;
	switch (sample_rate_code)
	{
	case 15:
		fputs("Invalid frame header sample rate", stderr);
	case 0:
		sample_rate = f->sample_rate;
		break;
	case 1:
		sample_rate = 88200;
		break;
	case 2:
		sample_rate = 176400;
		break;
	case 3:
		sample_rate = 192000;
		break;
	case 4:
		sample_rate = 8000;
		break;
	case 5:
		sample_rate = 16000;
		break;
	case 6:
		sample_rate = 22050;
		break;
	case 7:
		sample_rate = 44100;
		break;
	case 8:
		sample_rate = 32000;
		break;
	case 9:
		sample_rate = 44100;
		break;
	case 10:
		sample_rate = 48000;
		break;
	case 11:
		sample_rate = 96000;
		break;
	case 12:
		sample_rate = f->read_byte(f) * 1000;
		break;
	case 13:
		sample_rate = read16(f);
		break;
	case 14:
		sample_rate = read16(f) * 10;
		break;
	}
	f->frame_sample_rate = sample_rate;
	f->read_byte(f);//CRC-8
	return 1;
}

enum {
	SUBFRAME_CONSTANT,
	SUBFRAME_VERBATIM,
	SUBFRAME_FIXED = 8,
	SUBFRAME_LPC = 0x20,
};

static int32_t sign_extend(uint32_t value, uint32_t bits)
{
	if (value & (1 << (bits - 1))) {
		value |= ~((1 << bits) - 1);
	}
	return value;
}

static int32_t signed_sample(uint32_t sample_bits, uint32_t sample, uint8_t wasted_bits)
{
	sample <<= wasted_bits;
	sample = sign_extend(sample, sample_bits);
	return sample;
}

static void decode_residuals(flac_file *f, flac_subframe *sub, int64_t *coefficients, uint32_t order, int64_t shift)
{
	uint8_t residual_method = read_bits(f, 2);
	uint8_t rice_param_bits = residual_method ? 5 : 4;
	uint32_t partition_count = 1 << read_bits(f, 4);
	uint32_t cur = order;
	uint32_t partition_size = f->frame_block_size / partition_count;
	for (uint32_t partition = 0; partition < partition_count; partition++)
	{
		uint32_t rice_param = read_bits(f, rice_param_bits);
		if (rice_param == (1 << rice_param_bits) - 1) {
			//escape code, residuals are unencoded
			rice_param = read_bits(f, rice_param_bits);
			for (uint32_t end = partition ? cur + partition_size : partition_size; cur < end; cur++)
			{
				int64_t prediction = 0;
				for (uint32_t i = 0; i < order; i++)
				{
					prediction += ((int64_t)sub->decoded[cur - 1 - i]) * coefficients[i];
				}
				if (shift) {
					prediction >>= shift;
				}
				prediction += sign_extend(read_bits(f, rice_param), rice_param);
				sub->decoded[cur] = prediction;
			}
		} else {
			for (uint32_t end = partition ? cur + partition_size : partition_size; cur < end; cur++)
			{
				int64_t prediction = 0;
				for (uint32_t i = 0; i < order; i++)
				{
					prediction += ((int64_t)sub->decoded[cur - 1 - i]) * coefficients[i];
				}
				if (shift) {
					prediction >>= shift;
				}
				uint32_t residual = 0;
				while (!read_bits(f, 1))
				{
					++residual;
				}
				residual <<= rice_param;
				residual |= read_bits(f, rice_param);
				if (residual & 1) {
					sub->decoded[cur] = prediction - (residual >> 1) - 1;
				} else {
					sub->decoded[cur] = prediction + (residual >> 1);
				}
			}
		}
	}
}

static void decode_subframe(flac_file *f, flac_subframe *sub)
{
	if (f->frame_block_size > sub->allocated_samples) {
		sub->decoded = realloc(sub->decoded, sizeof(int32_t) * f->frame_block_size);
		sub->allocated_samples = f->frame_block_size ;
	}
	int64_t prediction_coefficients[32];
	read_bits(f, 1);//reserved
	uint8_t type = read_bits(f, 6);
	uint8_t has_wasted_bits = read_bits(f, 1);
	uint8_t wasted_bits = 0;
	if (has_wasted_bits) {
		++wasted_bits;
		while (!read_bits(f, 1))
		{
			++wasted_bits;
		}
	}
	uint32_t sample_bits = f->frame_bits_per_sample - wasted_bits;
	if (f->frame_joint_stereo) {
		int channel = sub - f->subframes;
		if (f->frame_joint_stereo == 2 && !channel || (channel && f->frame_joint_stereo != 2)) {
			sample_bits++;
		}
	}
	if (type == SUBFRAME_CONSTANT) {
		int32_t sample = signed_sample(sample_bits, read_bits(f, sample_bits), wasted_bits);
		for (uint32_t i = 0; i < f->frame_block_size; i++)
		{
			sub->decoded[i] = sample;
		}
	} else if (type == SUBFRAME_VERBATIM) {
		for (uint32_t i = 0; i < f->frame_block_size; i++)
		{
			sub->decoded[i] = signed_sample(sample_bits, read_bits(f, sample_bits), wasted_bits);
		}
	} else if (type & SUBFRAME_LPC) {
		uint32_t order = (type & 0x1F) + 1;
		for (uint32_t i = 0; i < order; i++)
		{
			sub->decoded[i] = signed_sample(sample_bits, read_bits(f, sample_bits), wasted_bits);
		}
		uint32_t coefficient_bits = read_bits(f, 4) + 1;
		int64_t shift_bits = read_bits(f, 5);
		for (uint32_t i = 0; i < order; i++)
		{
			prediction_coefficients[i] = sign_extend(read_bits(f, coefficient_bits), coefficient_bits);
		}
		decode_residuals(f, sub, prediction_coefficients, order, shift_bits);
	} else if (type & SUBFRAME_FIXED) {
		uint32_t order = type & 7;
		for (uint32_t i = 0; i < order; i++)
		{
			sub->decoded[i] = signed_sample(sample_bits, read_bits(f, sample_bits), wasted_bits);
		}
		switch (order)
		{
		case 1:
			prediction_coefficients[0] = 1;
			break;
		case 2:
			prediction_coefficients[0] = 2;
			prediction_coefficients[1] = -1;
			break;
		case 3:
			prediction_coefficients[0] = 3;
			prediction_coefficients[1] = -3;
			prediction_coefficients[2] = 1;
			break;
		case 4:
			prediction_coefficients[0] = 4;
			prediction_coefficients[1] = -6;
			prediction_coefficients[2] = 4;
			prediction_coefficients[3] = -1;
			break;
		}
		decode_residuals(f, sub, prediction_coefficients, order, 0);
	} else {
		fprintf(stderr, "Invalid subframe type %X\n", type);
	}
}

static uint8_t decode_frame(flac_file *f)
{
	if (!parse_frame_header(f)) {
		return 0;
	}
	if (f->frame_channels > f->subframe_alloc) {
		f->subframes = realloc(f->subframes, sizeof(flac_subframe) * f->frame_channels);
		memset(f->subframes + f->subframe_alloc, 0, sizeof(flac_subframe) * (f->frame_channels - f->subframe_alloc));
		f->subframe_alloc = f->frame_channels;
	}
	for (uint8_t channel = 0; channel < f->frame_channels; channel++)
	{
		decode_subframe(f, f->subframes + channel);
	}
	f->bits = 0;
	read16(f);//Frame footer CRC-16
	f->frame_sample_pos = 0;
	return 1;
}

uint8_t flac_get_sample(flac_file *f, int16_t *out, uint8_t desired_channels)
{
	if (f->frame_sample_pos == f->frame_block_size) {
		if (!decode_frame(f)) {
			return 0;
		}
	}
	uint8_t copy_channels;
	if (f->frame_channels == 1 && desired_channels > 1) {
		int16_t sample = f->subframes->decoded[f->frame_sample_pos];
		*(out++) = sample;
		*(out++) = sample;
		copy_channels = 2;
	} else {
		int32_t left, right, mid, diff;
		switch (f->frame_joint_stereo)
		{
		case 0:
			copy_channels = desired_channels;
			if (copy_channels > f->frame_channels) {
				copy_channels = f->frame_channels;
			}
			for (uint8_t i = 0; i < copy_channels; i++)
			{
				*(out++) = f->subframes[i].decoded[f->frame_sample_pos];
			}
			break;
		case 1:
			//left-side
			copy_channels = 2;
			*(out++) = left = f->subframes[0].decoded[f->frame_sample_pos];
			if (desired_channels > 1) {
				*(out++) = left + f->subframes[1].decoded[f->frame_sample_pos];
			}
			break;
		case 2:
			//side-right
			copy_channels = 2;
			right = f->subframes[1].decoded[f->frame_sample_pos];
			left = right + f->subframes[0].decoded[f->frame_sample_pos];
			*(out++) = left;
			if (desired_channels > 1) {
				*(out++) = right;
			}
			break;
		case 3:
			//mid-side
			copy_channels = 2;
			mid = f->subframes[0].decoded[f->frame_sample_pos];
			diff = f->subframes[1].decoded[f->frame_sample_pos];
			left = (diff + 2 * mid) >> 1;
			*(out++) = left;
			if (desired_channels > 1) {
				*(out++) = left - diff;
			}
			break;
		}
	}
	for (uint8_t i = copy_channels; i < desired_channels; i++)
	{
		*(out++) = 0;
	}
	f->frame_sample_pos++;

	return 1;
}

void flac_seek(flac_file *f, uint64_t sample_number)
{
	if (sample_number >= f->frame_start_sample && sample_number < f->frame_start_sample + f->frame_block_size) {
		f->frame_sample_pos = sample_number - f->frame_start_sample;
		return;
	}
	uint32_t best_seekpoint = f->num_seekpoints + 1;
	if (f->num_seekpoints) {
		uint64_t best_diff;
		for (uint32_t i = 0; i < f->num_seekpoints; i++)
		{
			if (f->seekpoints[i].sample_number > sample_number) {
				continue;
			}
			uint64_t diff = sample_number - f->seekpoints[i].sample_number;
			if (best_seekpoint > f->num_seekpoints || diff < best_diff) {
				best_seekpoint = i;
				best_diff = diff;
			}
		}
	}
	//TODO: more efficient seeking
	if (best_seekpoint > f->num_seekpoints) {
		f->seek(f, f->first_frame_offset, 0);
	} else if (f->seekpoints[best_seekpoint].sample_number > f->frame_start_sample || f->frame_start_sample > sample_number){
		f->seek(f, f->seekpoints[best_seekpoint].offset + f->first_frame_offset, 0);
	}
	do {
		if (!decode_frame(f)) {
			return;
		}
	} while ((f->frame_start_sample + f->frame_block_size) <= sample_number);
	f->frame_sample_pos = sample_number - f->frame_start_sample;
}
