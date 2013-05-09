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

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	x86_z80_options opts;
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
	init_x86_z80_opts(&opts);
	init_z80_context(&context, &opts);
	//Z80 RAM
	context.mem_pointers[0] = z80_ram;
	context.sync_cycle = context.target_cycle = 1000;
	context.int_cycle = CYCLE_NEVER;
	//cartridge/bank
	context.mem_pointers[1] = context.mem_pointers[2] = (uint8_t *)cart;
	z80_reset(&context);
	while (context.current_cycle < 1000) {
		z80_run(&context);
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
