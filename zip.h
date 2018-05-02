#ifndef ZIP_H_
#define ZIP_H_

#include <stdint.h>
#include <stdio.h>

typedef struct {
	uint64_t compressed_size;
	uint64_t size;
	uint64_t local_header_off;
	char     *name;
	uint16_t compression_method;
} zip_entry;

typedef struct {
	zip_entry *entries;
	FILE      *file;
	uint32_t  num_entries;
} zip_file;

zip_file *zip_open(const char *filename);
uint8_t *zip_read(zip_file *f, uint32_t index, size_t *out_size);
void zip_close(zip_file *f);

#endif //ZIP_H_