/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gst.h"
#include <string.h>
#include <stdlib.h>

uint8_t busreq;
uint8_t reset;

void latch_mode(vdp_context * context)
{
}
void ym_data_write(ym2612_context * context, uint8_t value)
{
	if (context->selected_reg >= YM_REG_END) {
		return;
	}
	if (context->selected_part) {
		if (context->selected_reg < YM_PART2_START) {
			return;
		}
		context->part2_regs[context->selected_reg - YM_PART2_START] = value;
	} else {
		if (context->selected_reg < YM_PART1_START) {
			return;
		}
		context->part1_regs[context->selected_reg - YM_PART1_START] = value;
	}
}

void ym_address_write_part1(ym2612_context * context, uint8_t address)
{
	//printf("address_write_part1: %X\n", address);
	context->selected_reg = address;
	context->selected_part = 0;
}

void ym_address_write_part2(ym2612_context * context, uint8_t address)
{
	//printf("address_write_part2: %X\n", address);
	context->selected_reg = address;
	context->selected_part = 1;
}

uint16_t ram[64*1024];
uint8_t zram[8*1024];


int main(int argc, char ** argv)
{
	vdp_context vdp;
	ym2612_context ym;
	psg_context psg;
	m68k_context m68k;
	z80_context z80;
	genesis_context gen;
	if (argc < 3) {
		fputs("Usage: testgst infile outfile\n", stderr);
		return 1;
	}
	memset(&gen, 0, sizeof(gen));
	memset(&m68k, 0, sizeof(m68k));
	memset(&z80, 0, sizeof(z80));
	memset(&ym, 0, sizeof(ym));
	memset(&vdp, 0, sizeof(vdp));
	memset(&psg, 0, sizeof(psg));
	m68k.mem_pointers[1] = ram;
	z80.mem_pointers[0] = zram;
	vdp.vdpmem = malloc(VRAM_SIZE);
	gen.vdp = &vdp;
	gen.ym = &ym;
	gen.psg = &psg;
	gen.m68k = &m68k;
	gen.z80 = &z80;
	uint32_t pc = load_gst(&gen, argv[1]);
	save_gst(&gen, argv[2], pc);
	return 0;
}
