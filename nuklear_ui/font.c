#include <stdio.h>
#include <stdlib.h>

char *default_font_path(void)
{
	FILE *fc_pipe = popen("fc-match -f '%{file}'", "r");
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
