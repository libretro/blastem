/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "z80inst.h"
#include "z80_to_x86.h"
#include "mem.h"
#include "vdp.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t z80_ram[0x2000];
uint16_t cart[0x200000];

#define MCLKS_PER_Z80 15
//TODO: Figure out the exact value for this
#define MCLKS_PER_FRAME (MCLKS_LINE*262)
#define VINT_CYCLE ((MCLKS_LINE * 226)/MCLKS_PER_Z80)
#define CYCLE_NEVER 0xFFFFFFFF

uint8_t z80_read_ym(uint16_t location, z80_context * context)
{
	return 0xFF;
}

z80_context * z80_write_ym(uint16_t location, z80_context * context, uint8_t value)
{
	return context;
}

z80_context * z80_vdp_port_write(uint16_t location, z80_context * context, uint8_t value)
{
	return context;
}

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	z80_options opts;
	z80_context context;
	if (argc < 2) {
		fputs("usage: transz80 zrom [cartrom]\n", stderr);
		exit(1);
	}
	FILE * f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "unable to open file %s\n", argv[2]);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	fread(z80_ram, 1, filesize < sizeof(z80_ram) ? filesize : sizeof(z80_ram), f);
	fclose(f);
	if (argc > 2) {
		f = fopen(argv[2], "rb");
		if (!f) {
			fprintf(stderr, "unable to open file %s\n", argv[2]);
			exit(1);
		}
		fseek(f, 0, SEEK_END);
		filesize = ftell(f);
		fseek(f, 0, SEEK_SET);
		fread(cart, 1, filesize < sizeof(cart) ? filesize : sizeof(cart), f);
		fclose(f);
		for(unsigned short * cur = cart; cur - cart < (filesize/2); ++cur)
		{
			*cur = (*cur >> 8) | (*cur << 8);
		}
	}
	init_z80_opts(&opts);
	init_z80_context(&context, &opts);
	//Z80 RAM
	context.mem_pointers[0] = z80_ram;
	context.sync_cycle = context.target_cycle = MCLKS_PER_FRAME/MCLKS_PER_Z80;
	context.int_cycle = CYCLE_NEVER;
	//cartridge/bank
	context.mem_pointers[1] = context.mem_pointers[2] = (uint8_t *)cart;
	z80_reset(&context);
	for(;;)
	{
		z80_run(&context);
		if (context.current_cycle >= MCLKS_PER_FRAME/MCLKS_PER_Z80) {
			context.current_cycle -= MCLKS_PER_FRAME/MCLKS_PER_Z80;
		}
		if (context.current_cycle < VINT_CYCLE && context.iff1) {
			context.int_cycle = VINT_CYCLE;
		}
		context.target_cycle = context.sync_cycle < context.int_cycle ? context.sync_cycle : context.int_cycle;
	}
	return 0;
}
