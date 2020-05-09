#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../util.h"
#include "sfnt.h"

char *default_font_path(void)
{
#ifdef FONT_PATH
	FILE *f = fopen(FONT_PATH, "rb");
	if (f) {
		fclose(f);
		return strdup(FONT_PATH);
	}
#endif
	//TODO: specify language dynamically once BlastEm is localized
	FILE *fc_pipe = popen("fc-match :lang=en -f '%{file}'", "r");
	if (!fc_pipe) {
		return NULL;
	}
	size_t buf_size = 128;
	char *buffer = NULL;
	size_t total = 0, read = 0;
	do {
		total += read;
		buf_size *= 2;
		buffer = realloc(buffer, buf_size);
		if (!buffer) {
			return NULL;
		}
		read = fread(buffer, 1, buf_size - total, fc_pipe);
	} while (read == (buf_size - total));
	total += read;
	buffer[total] = 0;
	
	return buffer;
}

uint8_t *default_font(uint32_t *size_out)
{
	char *path = default_font_path();
	if (!path) {
		goto error;
	}
	FILE *f = fopen(path, "rb");
	free(path);
	if (!f) {
		goto error;
	}
	long size = file_size(f);
	uint8_t *buffer = malloc(size);
	if (size != fread(buffer, 1, size, f)) {
		fclose(f);
		goto error;
	}
	fclose(f);
	sfnt_container *sfnt = load_sfnt(buffer, size);
	if (!sfnt) {
		free(buffer);
		goto error;
	}
	return sfnt_flatten(sfnt->tables, size_out);
error:
	//TODO: try to find a suitable font in /usr/share/fonts as a fallback
	return NULL;
}