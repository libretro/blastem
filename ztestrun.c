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

#define MCLKS_PER_Z80 15
//TODO: Figure out the exact value for this
#define MCLKS_PER_FRAME (MCLKS_LINE*262)
#define VINT_CYCLE ((MCLKS_LINE * 226)/MCLKS_PER_Z80)
#define CYCLE_NEVER 0xFFFFFFFF

uint8_t z80_unmapped_read(uint32_t location, void * context)
{
	return 0xFF;
}

void * z80_unmapped_write(uint32_t location, void * context, uint8_t value)
{
	return context;
}

const memmap_chunk z80_map[] = {
	{ 0x0000, 0x4000,  0x1FFF, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, z80_ram, NULL, NULL, NULL,              NULL },
	{ 0x4000, 0x10000, 0xFFFF, 0, 0,                                  NULL,    NULL, NULL, z80_unmapped_read, z80_unmapped_write}
};

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
	init_z80_opts(&opts, z80_map, 2);
	init_z80_context(&context, &opts);
	//Z80 RAM
	context.mem_pointers[0] = z80_ram;
	context.sync_cycle = context.target_cycle = 1000;
	context.int_cycle = CYCLE_NEVER;
	z80_reset(&context);
	while (context.current_cycle < 1000) {
		context.run(&context);
	}
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\nIX: %X\nIY: %X\nSP: %X\n\nIM: %d, IFF1: %d, IFF2: %d\n",
		context.regs[Z80_A], context.regs[Z80_B], context.regs[Z80_C],
		context.regs[Z80_D], context.regs[Z80_E],
		(context.regs[Z80_H] << 8) | context.regs[Z80_L],
		(context.regs[Z80_IXH] << 8) | context.regs[Z80_IXL],
		(context.regs[Z80_IYH] << 8) | context.regs[Z80_IYL],
		context.sp, context.im, context.iff1, context.iff2);
	printf("Flags: SZVNC\n"
	       "       %d%d%d%d%d\n", context.flags[ZF_S], context.flags[ZF_Z], context.flags[ZF_PV], context.flags[ZF_N], context.flags[ZF_C]);
	puts("--Alternate Regs--");
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\n",
		context.alt_regs[Z80_A], context.alt_regs[Z80_B], context.alt_regs[Z80_C],
		context.alt_regs[Z80_D], context.alt_regs[Z80_E],
		(context.alt_regs[Z80_H] << 8) | context.alt_regs[Z80_L]);
	return 0;
}
