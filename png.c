#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "zlib/zlib.h"

static const char png_magic[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
static const char ihdr[] = {'I', 'H', 'D', 'R'};
static const char plte[] = {'P', 'L', 'T', 'E'};
static const char idat[] = {'I', 'D', 'A', 'T'};
static const char iend[] = {'I', 'E', 'N', 'D'};

enum {
	COLOR_TRUE = 2,
	COLOR_INDEXED
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
