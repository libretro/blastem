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
#include <stddef.h>
#include <stdarg.h>

void fatal_error(char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}

uint8_t z80_ram[0x2000];

uint8_t z80_unmapped_read(uint32_t location, void * context)
{
	return 0xFF;
}

void * z80_unmapped_write(uint32_t location, void * context, uint8_t value)
{
	return context;
}

const memmap_chunk z80_map[] = {
	{ 0x0000, 0x4000,  0x1FFF, 0, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, z80_ram, NULL, NULL, NULL,              NULL },
	{ 0x4000, 0x10000, 0xFFFF, 0, 0, 0,                                  NULL,    NULL, NULL, z80_unmapped_read, z80_unmapped_write}
};

const memmap_chunk port_map[] = {
	{ 0x0000, 0x100, 0xFF, 0, 0, 0,                                  NULL,    NULL, NULL, z80_unmapped_read, z80_unmapped_write}
};

void z80_next_int_pulse(z80_context * context)
{
	context->int_pulse_start = context->int_pulse_end = CYCLE_NEVER;
}

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	z80_options opts;
	z80_context *context;
	char *fname = NULL;
	uint8_t retranslate = 0;
	for (int i = 1; i < argc; i++)
	{
		if (argv[i][0] == '-') {
			switch(argv[i][1])
			{
			case 'r':
				retranslate = 1;
				break;
			default:
				fprintf(stderr, "Unrecognized switch -%c\n", argv[i][1]);
				exit(1);
			}
		} else if (!fname) {
			fname = argv[i];
		}
	}
	if (!fname) {
		fputs("usage: ztestrun zrom [cartrom]\n", stderr);
		exit(1);
	}
	FILE * f = fopen(fname, "rb");
	if (!f) {
		fprintf(stderr, "unable to open file %s\n", fname);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	filesize = filesize < sizeof(z80_ram) ? filesize : sizeof(z80_ram);
	if (fread(z80_ram, 1, filesize, f) != filesize) {
		fprintf(stderr, "error reading %s\n",fname);
		exit(1);
	}
	fclose(f);
	init_z80_opts(&opts, z80_map, 2, port_map, 1, 1, 0xFF);
	context = init_z80_context(&opts);
	//Z80 RAM
	context->mem_pointers[0] = z80_ram;
	if (retranslate) {
		//run core long enough to translate code
		z80_run(context, 1);
		for (int i = 0; i < filesize; i++)
		{
			z80_handle_code_write(i, context);
		}
		z80_assert_reset(context, context->current_cycle);
		z80_clear_reset(context, context->current_cycle + 3);
		z80_adjust_cycles(context, context->current_cycle);
	}
	z80_run(context, 1000);
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\nIX: %X\nIY: %X\nSP: %X\n\nIM: %d, IFF1: %d, IFF2: %d\n",
		context->regs[Z80_A], context->regs[Z80_B], context->regs[Z80_C],
		context->regs[Z80_D], context->regs[Z80_E],
		(context->regs[Z80_H] << 8) | context->regs[Z80_L],
		(context->regs[Z80_IXH] << 8) | context->regs[Z80_IXL],
		(context->regs[Z80_IYH] << 8) | context->regs[Z80_IYL],
		context->sp, context->im, context->iff1, context->iff2);
	printf("Flags: SZYHXVNC\n"
	       "       %d%d%d%d%d%d%d%d\n", 
			context->flags[ZF_S], context->flags[ZF_Z], context->flags[ZF_XY] >> 5 & 1, context->flags[ZF_H], 
			context->flags[ZF_XY] >> 3 & 1, context->flags[ZF_PV], context->flags[ZF_N], context->flags[ZF_C]
	);
	puts("--Alternate Regs--");
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\n",
		context->alt_regs[Z80_A], context->alt_regs[Z80_B], context->alt_regs[Z80_C],
		context->alt_regs[Z80_D], context->alt_regs[Z80_E],
		(context->alt_regs[Z80_H] << 8) | context->alt_regs[Z80_L]);
	return 0;
}
