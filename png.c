#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "zlib/zlib.h"

static const char png_magic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
static const char ihdr[] = {'I', 'H', 'D', 'R'};
static const char plte[] = {'P', 'L', 'T', 'E'};
static const char idat[] = {'I', 'D', 'A', 'T'};
static const char iend[] = {'I', 'E', 'N', 'D'};

enum {
	COLOR_GRAY,
	COLOR_TRUE = 2,
	COLOR_INDEXED,
	COLOR_GRAY_ALPHA,
	COLOR_TRUE_ALPHA=6
};

static void write_chunk(FILE *f, const char*id, uint8_t *buffer, uint32_t size)
{
	uint8_t tmp[4] = {size >> 24, size >> 16, size >> 8, size};
	uint8_t warn = 0;
	warn = warn || (sizeof(tmp) != fwrite(tmp, 1, sizeof(tmp), f));
	warn = warn || (4 != fwrite(id, 1, 4, f));
	if (size) {
		warn = warn || (size != fwrite(buffer, 1, size, f));
	}
	
	uint32_t crc = crc32(0, NULL, 0);
	crc = crc32(crc, id, 4);
	if (size) {
		crc = crc32(crc, buffer, size);
	}
	tmp[0] = crc >> 24;
	tmp[1] = crc >> 16;
	tmp[2] = crc >> 8;
	tmp[3] = crc;
	warn = warn || (sizeof(tmp) != fwrite(tmp, 1, sizeof(tmp), f));
	if (warn) {
		fprintf(stderr, "Failure during write of %c%c%c%c chunk\n", id[0], id[1], id[2], id[3]);
	}
}

static void write_header(FILE *f, uint32_t width, uint32_t height, uint8_t color_type)
{
	uint8_t chunk[13] = {
		width >> 24, width >> 16, width >> 8, width,
		height >> 24, height >> 16, height >> 8, height,
		8, color_type, 0, 0, 0
	};
	if (sizeof(png_magic) != fwrite(png_magic, 1, sizeof(png_magic), f)) {
		fputs("Error writing PNG magic\n", stderr);
	}
	write_chunk(f, ihdr, chunk, sizeof(chunk));
}

void save_png24(FILE *f, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t pitch)
{
	uint32_t idat_size = (1 + width*3) * height;
	uint8_t *idat_buffer = malloc(idat_size);
	uint32_t *pixel = buffer;
	uint8_t *cur = idat_buffer;
	for (uint32_t y = 0; y < height; y++)
	{
		//save filter type
		*(cur++) = 0;
		uint32_t *start = pixel;
		for (uint32_t x = 0; x < width; x++, pixel++)
		{
			uint32_t value = *pixel;
			*(cur++) = value >> 16;
			*(cur++) = value >> 8;
			*(cur++) = value;
		}
		pixel = start + pitch / sizeof(uint32_t);
	}
	write_header(f, width, height, COLOR_TRUE);
	uLongf compress_buffer_size = idat_size + 5 * (idat_size/16383 + 1) + 3;
	uint8_t *compressed = malloc(compress_buffer_size);
	compress(compressed, &compress_buffer_size, idat_buffer, idat_size);
	free(idat_buffer);
	write_chunk(f, idat, compressed, compress_buffer_size);
	write_chunk(f, iend, NULL, 0);
	free(compressed);
}

void save_png(FILE *f, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t pitch)
{
	uint32_t palette[256];
	uint8_t pal_buffer[256*3];
	uint32_t num_pal = 0;
	uint32_t index_size = (1 + width) * height;
	uint8_t *index_buffer = malloc(index_size);
	uint8_t *cur = index_buffer;
	uint32_t *pixel = buffer;
	for (uint32_t y = 0; y < height; y++)
	{
		//save filter type
		*(cur++) = 0;
		uint32_t *start = pixel;
		for (uint32_t x = 0; x < width; x++, pixel++, cur++)
		{
			uint32_t value = (*pixel) & 0xFFFFFF;
			uint32_t i;
			for (i = 0; i < num_pal; i++)
			{
				if (palette[i] == value) {
					break;
				}
			}
			if (i == num_pal) {
				if (num_pal == 256) {
					free(index_buffer);
					save_png24(f, buffer, width, height, pitch);
					return;
				}
				palette[i] = value;
				num_pal++;
			}
			*cur = i;
		}
		pixel = start + pitch / sizeof(uint32_t);
	}
	write_header(f, width, height, COLOR_INDEXED);
	cur = pal_buffer;
	for (uint32_t i = 0; i < num_pal; i++)
	{
		*(cur++) = palette[i] >> 16;
		*(cur++) = palette[i] >> 8;
		*(cur++) = palette[i];
	}
	write_chunk(f, plte, pal_buffer, num_pal * 3);
	uLongf compress_buffer_size = index_size + 5 * (index_size/16383 + 1) + 3;
	uint8_t *compressed = malloc(compress_buffer_size);
	compress(compressed, &compress_buffer_size, index_buffer, index_size);
	free(index_buffer);
	write_chunk(f, idat, compressed, compress_buffer_size);
	write_chunk(f, iend, NULL, 0);
	free(compressed);
}

typedef uint8_t (*filter_fun)(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x);
typedef uint32_t (*pixel_fun)(uint8_t **cur, uint8_t **last, uint8_t bpp, uint32_t x, filter_fun);

static uint8_t filter_none(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x)
{
	return *cur;
}

static uint8_t filter_sub(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x)
{
	if (x) {
		return *cur + *(cur - bpp);
	} else {
		return *cur;
	}
}

static uint8_t filter_up(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x)
{
	if (last) {
		return *cur + *last;
	} else {
		return *cur;
	}
}

static uint8_t filter_avg(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x)
{
	uint8_t prev = x ? *(cur - bpp) : 0;
	uint8_t prior = last ? *last : 0;
	return *cur + ((prev + prior) >> 1);
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
	int32_t p = a + b - c;
	int32_t pa = abs(p - a);
	int32_t pb = abs(p - b);
	int32_t pc = abs(p - c);
	if (pa <= pb && pa <= pc) {
		return a;
	}
	if (pb <= pc) {
		return b;
	}
	return c;
}

static uint8_t filter_paeth(uint8_t *cur, uint8_t *last, uint8_t bpp, uint32_t x)
{
	uint8_t prev, prev_prior;
	if (x) {
		prev = *(cur - bpp);
		prev_prior = *(last - bpp);
	} else {
		prev = prev_prior = 0;
	}
	uint8_t prior = last ? *last : 0;
	return *cur + paeth(prev, prior, prev_prior);
}

static uint32_t pixel_gray(uint8_t **cur, uint8_t **last, uint8_t bpp, uint32_t x, filter_fun filter)
{
	uint8_t value = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	return 0xFF000000 | value << 16 | value << 8 | value;
}

static uint32_t pixel_true(uint8_t **cur, uint8_t **last, uint8_t bpp, uint32_t x, filter_fun filter)
{
	uint8_t red = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t green = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t blue = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	return 0xFF000000 | red << 16 | green << 8 | blue;
}

static uint32_t pixel_gray_alpha(uint8_t **cur, uint8_t **last, uint8_t bpp, uint32_t x, filter_fun filter)
{
	uint8_t value = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t alpha = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	return alpha << 24 | value << 16 | value << 8 | value;
}

static uint32_t pixel_true_alpha(uint8_t **cur, uint8_t **last, uint8_t bpp, uint32_t x, filter_fun filter)
{
	uint8_t red = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t green = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t blue = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	uint8_t alpha = **cur = filter(*cur, *last, bpp, x);
	(*cur)++;
	if (*last) {
		(*last)++;
	}
	return alpha << 24 | red << 16 | green << 8 | blue;
}

static filter_fun filters[] = {filter_none, filter_sub, filter_up, filter_avg, filter_paeth};

#define MIN_CHUNK_SIZE 12
#define MIN_IHDR_SIZE 0xD
#define MAX_SUPPORTED_DIM 32767 //chosen to avoid possibility of overflow when calculating uncompressed size
uint32_t *load_png(uint8_t *buffer, uint32_t buf_size, uint32_t *width, uint32_t *height)
{
	if (buf_size < sizeof(png_magic) || memcmp(buffer, png_magic, sizeof(png_magic))) {
		return NULL;
	}
	uint32_t cur = sizeof(png_magic);
	uint8_t has_header = 0;
	uint8_t bits, color_type, comp_type, filter_type, interlace;
	uint8_t *idat_buf = NULL;
	uint8_t idat_needs_free = 0;
	uint32_t idat_size;
	uint32_t *out = NULL;
	uint32_t *palette = NULL;
	while(cur + MIN_CHUNK_SIZE <= buf_size)
	{
		uint32_t chunk_size = buffer[cur++] << 24;
		chunk_size |=  buffer[cur++] << 16;
		chunk_size |=  buffer[cur++] << 8;
		chunk_size |=  buffer[cur++];
		if (!memcmp(ihdr, buffer + cur, sizeof(ihdr))) {
			if (chunk_size < MIN_IHDR_SIZE || cur + MIN_IHDR_SIZE > buf_size) {
				return NULL;
			}
			cur += sizeof(ihdr);
			*width = buffer[cur++] << 24;
			*width |=  buffer[cur++] << 16;
			*width |=  buffer[cur++] << 8;
			*width |=  buffer[cur++];
			*height = buffer[cur++] << 24;
			*height |=  buffer[cur++] << 16;
			*height |=  buffer[cur++] << 8;
			*height |=  buffer[cur++];
			if (*width > MAX_SUPPORTED_DIM || *height > MAX_SUPPORTED_DIM) {
				return NULL;
			}
			bits = buffer[cur++];
			if (bits != 8) {
				//only support 8-bits per element for now
				return NULL;
			}
			color_type = buffer[cur++];
			if (color_type > COLOR_TRUE_ALPHA || color_type == 1 || color_type == 5) {
				//reject invalid color type
				return NULL;
			}
			comp_type = buffer[cur++];
			if (comp_type) {
				//only compression type 0 is defined by the spec
				return NULL;
			}
			filter_type = buffer[cur++];
			interlace = buffer[cur++];
			if (interlace) {
				//interlacing not supported for now
				return NULL;
			}
			cur += chunk_size - MIN_IHDR_SIZE;
			has_header = 1;
		} else {
			if (!has_header) {
				//IHDR is required to be the first chunk, fail if it isn't
				break;
			}
			if (!memcmp(plte, buffer + cur, sizeof(plte))) {
				//TODO: implement paletted images
			} else if (!memcmp(idat, buffer + cur, sizeof(idat))) {
				cur += sizeof(idat);
				if (idat_buf) {
					if (idat_needs_free) {
						idat_buf = realloc(idat_buf, idat_size + chunk_size);
					} else {
						uint8_t *tmp = idat_buf;
						idat_buf = malloc(idat_size + chunk_size);
						memcpy(idat_buf, tmp, idat_size);
					}
					memcpy(idat_buf + idat_size, buffer + cur, chunk_size);
					idat_size += chunk_size;
					idat_needs_free = 1;
				} else {
					idat_buf = buffer + cur;
					idat_size = chunk_size;
				}
				cur += chunk_size;
			} else if (!memcmp(iend, buffer + cur, sizeof(iend))) {
				if (!idat_buf) {
					break;
				}
				if (!palette && color_type == COLOR_INDEXED) {
					//indexed color, but no PLTE chunk found
					return NULL;
				}
				uLongf uncompressed_size = *width * *height;
				uint8_t bpp;
				pixel_fun pixel;
				switch (color_type)
				{
				case COLOR_GRAY:
					uncompressed_size *= bits / 8;
					bpp = bits/8;
					pixel = pixel_gray;
					break;
				case COLOR_TRUE:
					uncompressed_size *= 3 * bits / 8;
					bpp = 3 * bits/8;
					pixel = pixel_true;
					break;
				case COLOR_INDEXED: {
					uint32_t pixels_per_byte = 8 / bits;
					uncompressed_size = (*width / pixels_per_byte) * *height;
					if (*width % pixels_per_byte) {
						uncompressed_size += *height;
					}
					bpp = 1;
					break;
				}
				case COLOR_GRAY_ALPHA:
					uncompressed_size *= bits / 4;
					bpp = bits / 4;
					pixel = pixel_gray_alpha;
					break;
				case COLOR_TRUE_ALPHA:
					uncompressed_size *= bits / 2;
					bpp = bits / 2;
					pixel = pixel_true_alpha;
					break;
				}
				//add filter type byte
				uncompressed_size += *height;
				uint8_t *decomp_buffer = malloc(uncompressed_size);
				if (Z_OK != uncompress(decomp_buffer, &uncompressed_size, idat_buf, idat_size)) {
					free(decomp_buffer);
					break;
				}
				out = calloc(*width * *height, sizeof(uint32_t));
				uint32_t *cur_pixel = out;
				uint8_t *cur_byte = decomp_buffer;
				uint8_t *last_line = NULL;
				for (uint32_t y = 0; y < *height; y++)
				{
					uint8_t filter_type = *(cur_byte++);
					if (filter_type >= sizeof(filters)/sizeof(*filters)) {
						free(out);
						out = NULL;
						free(decomp_buffer);
						break;
					}
					filter_fun filter = filters[filter_type];
					uint8_t *line_start = cur_byte;
					for (uint32_t x = 0; x < *width; x++)
					{
						*(cur_pixel++) = pixel(&cur_byte, &last_line, bpp, x, filter);
					}
					last_line = line_start;
				}
				free(decomp_buffer);
			} else {
				//skip uncrecognized chunks
				cur += 4 + chunk_size;
			}
		}
		//skip CRC for now
		cur += sizeof(uint32_t);
	}
	if (idat_needs_free) {
		free(idat_buf);
	}
	free(palette);
	return out;
}
