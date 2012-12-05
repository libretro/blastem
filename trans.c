#include "68kinst.h"
#include "m68k_to_x86.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char ** argv)
{
	long filesize;
	unsigned short *filebuf;
	char disbuf[1024];
	size_t size = 1024 * 1024;
	uint8_t * transbuf = alloc_code(&size);
	uint8_t *trans_cur, *end;
	unsigned short * cur;
	x86_68k_options opts;
	m68k_context context;
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	filebuf = malloc(filesize);
	fread(filebuf, 2, filesize/2, f);
	fclose(f);
	for(cur = filebuf; cur - filebuf < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	init_x86_68k_opts(&opts);
	init_68k_context(&context, opts.native_code_map, &opts);
	//cartridge ROM
	context.mem_pointers[0] = filebuf;
	context.target_cycle = 0x7FFFFFFF;
	//work RAM
	context.mem_pointers[1] = malloc(64 * 1024);
	translate_m68k_stream(transbuf, transbuf + size, 0, &context);
	m68k_reset(&context);
	return 0;
}
