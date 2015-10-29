/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "68kinst.h"
#include "m68k_core.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int headless = 1;
void render_errorbox(char * title, char * buf)
{
}

void render_infobox(char * title, char * buf)
{
}

m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	if (context->current_cycle > 0x80000000) {
		context->current_cycle -= 0x80000000;
	}
	return context;
}

int main(int argc, char ** argv)
{
	long filesize;
	unsigned short *filebuf;
	char disbuf[1024];
	unsigned short * cur;
	m68k_options opts;
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	filebuf = malloc(0x400000);
	memset(filebuf, 0, 0x400000);
	fread(filebuf, 2, filesize/2 > 0x200000 ? 0x200000 : filesize/2, f);
	fclose(f);
	for(cur = filebuf; cur - filebuf < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	memmap_chunk memmap[2];
	memset(memmap, 0, sizeof(memmap_chunk)*2);
	memmap[0].end = 0x400000;
	memmap[0].mask = 0xFFFFFF;
	memmap[0].flags = MMAP_READ;
	memmap[0].buffer = filebuf;

	memmap[1].start = 0xE00000;
	memmap[1].end = 0x1000000;
	memmap[1].mask = 0xFFFF;
	memmap[1].flags = MMAP_READ | MMAP_WRITE | MMAP_CODE;
	memmap[1].buffer = malloc(64 * 1024);
	memset(memmap[1].buffer, 0, 64 * 1024);
	init_m68k_opts(&opts, memmap, 2, 1);
	m68k_context * context = init_68k_context(&opts);
	context->mem_pointers[0] = memmap[0].buffer;
	context->mem_pointers[1] = memmap[1].buffer;
	context->target_cycle = context->sync_cycle = 0x80000000;
	uint32_t address;
	address = filebuf[2] << 16 | filebuf[3];
	translate_m68k_stream(address, context);
	m68k_reset(context);
	return 0;
}

