#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "util.h"
#include "zip.h"
#ifndef DISABLE_ZLIB
#include "zlib/zlib.h"
#endif

static const char cdfd_magic[4] = {'P', 'K', 1, 2};
static const char eocd_magic[4] = {'P', 'K', 5, 6};
#define MIN_EOCD_SIZE 22
#define MIN_CDFD_SIZE 46
#define ZIP_MAX_EOCD_OFFSET (64*1024+MIN_EOCD_SIZE)

enum {
	ZIP_STORE = 0,
	ZIP_DEFLATE = 8
};

zip_file *zip_open(const char *filename)
{
	FILE *f = fopen(filename, "rb");
	if (!f) {
		return NULL;
	}
	long fsize = file_size(f);
	if (fsize < MIN_EOCD_SIZE) {
		//too small to be a zip file
		goto fail;
	}
	
	long max_offset = fsize > ZIP_MAX_EOCD_OFFSET ? ZIP_MAX_EOCD_OFFSET : fsize;
	fseek(f, -max_offset, SEEK_END);
	uint8_t *buf = malloc(max_offset);
	if (max_offset != fread(buf, 1, max_offset, f)) {
		goto fail;
	}
	
	long current_offset;
	uint32_t cd_start, cd_size;
	uint16_t cd_count;
	for (current_offset = max_offset - MIN_EOCD_SIZE; current_offset >= 0; current_offset--)
	{
		if (memcmp(eocd_magic, buf + current_offset, sizeof(eocd_magic))) {
			continue;
		}
		uint16_t comment_size = buf[current_offset + 20] | buf[current_offset + 21] << 8;
		if (comment_size != (max_offset - current_offset - MIN_EOCD_SIZE)) {
			continue;
		}
		cd_start = buf[current_offset + 16] | buf[current_offset + 17] << 8
			| buf[current_offset + 18] << 16 | buf[current_offset + 19] << 24;
		if (cd_start > (fsize - (max_offset - current_offset))) {
			continue;
		}
		cd_size = buf[current_offset + 12] | buf[current_offset + 13] << 8
			| buf[current_offset + 14] << 16 | buf[current_offset + 15] << 24;
		if ((cd_start + cd_size) > (fsize - (max_offset - current_offset))) {
			continue;
		}
		cd_count = buf[current_offset + 10] | buf[current_offset + 11] << 8;
		break;
	}
	free(buf);
	if (current_offset < 0) {
		//failed to find EOCD
		goto fail;
	}
	buf = malloc(cd_size);
	fseek(f, cd_start, SEEK_SET);
	if (cd_size != fread(buf, 1, cd_size, f)) {
		goto fail_free;
	}
	zip_entry *entries = calloc(cd_count, sizeof(zip_entry));
	uint32_t cd_max_last = cd_size - MIN_CDFD_SIZE;
	zip_entry *cur_entry = entries;
	for (uint32_t off = 0; cd_count && off <= cd_max_last; cur_entry++, cd_count--)
	{
		if (memcmp(buf + off, cdfd_magic, sizeof(cdfd_magic))) {
			goto fail_entries;
		}
		uint32_t name_length = buf[off + 28] | buf[off + 29] << 8;
		uint32_t extra_length = buf[off + 30] | buf[off + 31] << 8;
		//TODO: verify name length doesn't go past end of CD
		
		cur_entry->name = malloc(name_length + 1);
		memcpy(cur_entry->name, buf + off + MIN_CDFD_SIZE, name_length);
		cur_entry->name[name_length] = 0;
		
		cur_entry->compressed_size = buf[off + 20] | buf[off + 21] << 8 
			| buf[off + 22] << 16 | buf[off + 23] << 24;
		cur_entry->size = buf[off + 24] | buf[off + 25] << 8 
			| buf[off + 26] << 16 | buf[off + 27] << 24;
			
		cur_entry->local_header_off = buf[off + 42] | buf[off + 43] << 8 
			| buf[off + 44] << 16 | buf[off + 45] << 24;
			
		cur_entry->compression_method = buf[off + 10] | buf[off + 11] << 8;
		
		off += name_length + extra_length + MIN_CDFD_SIZE;
	}
	
	zip_file *z = malloc(sizeof(zip_file));
	z->entries = entries;
	z->file = f;
	z->num_entries = cur_entry - entries;
	return z;
	
fail_entries:
	for (cur_entry--; cur_entry >= entries; cur_entry--)
	{
		free(cur_entry->name);
	}
	free(entries);
fail_free:
	free(buf);
fail:
	fclose(f);
	return NULL;
}

uint8_t *zip_read(zip_file *f, uint32_t index, size_t *out_size)
{
	
	fseek(f->file, f->entries[index].local_header_off + 26, SEEK_SET);
	uint8_t tmp[4];
	if (sizeof(tmp) != fread(tmp, 1, sizeof(tmp), f->file)) {
		return NULL;
	}
	uint32_t local_variable = (tmp[0] | tmp[1] << 8) + (tmp[2] | tmp[3] << 8);
	fseek(f->file, f->entries[index].local_header_off + local_variable + 30, SEEK_SET);
	
	size_t int_size;
	if (!out_size) {
		out_size = &int_size;
		int_size = f->entries[index].size;
	}
	
	uint8_t *buf = malloc(*out_size);
	if (*out_size > f->entries[index].size) {
		*out_size = f->entries[index].size;
	}
	switch(f->entries[index].compression_method)
	{
	case ZIP_STORE:
		if (*out_size != fread(buf, 1, *out_size, f->file)) {
			free(buf);
			return NULL;
		}
		break;
#ifndef DISABLE_ZLIB
	case ZIP_DEFLATE: {
		//note in unzip.c in zlib/contrib suggests a dummy byte is needed, so we allocate an extra byte here
		uint8_t *src_buf = malloc(f->entries[index].compressed_size + 1);
		if (f->entries[index].compressed_size != fread(src_buf, 1, f->entries[index].compressed_size, f->file)) {
			free(src_buf);
			return NULL;
		}
		uLongf destLen = *out_size;
		z_stream stream;
		memset(&stream, 0, sizeof(stream));
		stream.avail_in = f->entries[index].compressed_size + 1;
		stream.next_in = src_buf;
		stream.next_out = buf;
		stream.avail_out = *out_size;
		if (Z_OK == inflateInit2(&stream, -15)) {
			int result = inflate(&stream, Z_FINISH);
			*out_size = stream.total_out;
			free(src_buf);
			inflateEnd(&stream);
			if (result != Z_OK && result != Z_STREAM_END && result != Z_BUF_ERROR) {
				free(buf);
				return NULL;
			}
		}
		break;
	}
#endif
	default:
		free(buf);
		return NULL;
	}
	
	return buf;
}

void zip_close(zip_file *f)
{
	fclose(f->file);
	for (uint32_t i = 0; i < f->num_entries; i++)
	{
		free(f->entries[i].name);
	}
	free(f->entries);
	free(f);
}

 