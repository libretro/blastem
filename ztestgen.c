/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "z80inst.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

extern z80inst z80_tbl_a[256];
extern z80inst z80_tbl_extd[0xC0-0x40];
extern z80inst z80_tbl_bit[256];
extern z80inst z80_tbl_ix[256];
extern z80inst z80_tbl_iy[256];
extern z80inst z80_tbl_ix_bit[256];
extern z80inst z80_tbl_iy_bit[256];
extern char *z80_mnemonics[Z80_OTDR+1];
extern char * z80_regs[Z80_USE_IMMED];
#define PRE_IX  0xDD
#define PRE_IY  0xFD
#define LD_IR16 0x01
#define INC_R8  0x04
#define LD_IR8  0x06
#define LD_RR8  0x40
#define AND_R   0xA0
#define PUSH    0xC5
#define POP     0xC1

uint8_t * ld_ir16(uint8_t * dst, uint8_t reg, uint16_t val)
{
	if (reg == Z80_IX) {
		*(dst++) = PRE_IX;
		return ld_ir16(dst, Z80_HL, val);
	} else if(reg == Z80_IY) {
		*(dst++) = PRE_IY;
		return ld_ir16(dst, Z80_HL, val);
	} else {
		*(dst++) = LD_IR16 | ((reg - Z80_BC) << 4);
		*(dst++) = val & 0xFF;
		*(dst++) = val >> 8;
		return dst;
	}
}

uint8_t * ld_ir8(uint8_t * dst, uint8_t reg, uint8_t val)
{
	if (reg <= Z80_H) {
		reg = (reg - Z80_C) ^ 1;
	} else {
		reg = 0x7;
	}
	*(dst++) = LD_IR8 | (reg << 3);
	*(dst++) = val;
	return dst;
}

uint8_t * ld_rr8(uint8_t * dst, uint8_t src, uint8_t dstr)
{
	if (src <= Z80_H) {
		src = (src - Z80_C) ^ 1;
	} else {
		src = 0x7;
	}
	if (dstr <= Z80_H) {
		dstr = (dstr - Z80_C) ^ 1;
	} else {
		dstr = 0x7;
	}
	*(dst++) = LD_RR8 | (dstr << 3) | src;
	return dst;
}

uint8_t * ld_amem(uint8_t * dst, uint16_t address)
{
	*(dst++) = 0x32;
	*(dst++) = address & 0xFF;
	*(dst++) = address >> 8;
	return dst;
}

uint8_t * ld_mema(uint8_t * dst, uint16_t address)
{
	*(dst++) = 0x3A;
	*(dst++) = address & 0xFF;
	*(dst++) = address >> 8;
	return dst;
}

uint8_t * push(uint8_t * dst, uint8_t reg)
{
	if (reg == Z80_IX) {
		*(dst++) = PRE_IX;
		return push(dst, Z80_HL);
	} else if(reg == Z80_IY) {
		*(dst++) = PRE_IY;
		return push(dst, Z80_HL);
	} else {
		if (reg == Z80_AF) {
			reg--;
		}
		*(dst++) = PUSH | ((reg - Z80_BC) << 4);
		return dst;
	}
}

uint8_t * pop(uint8_t * dst, uint8_t reg)
{
	if (reg == Z80_IX) {
		*(dst++) = PRE_IX;
		return pop(dst, Z80_HL);
	} else if(reg == Z80_IY) {
		*(dst++) = PRE_IY;
		return pop(dst, Z80_HL);
	} else {
		if (reg == Z80_AF) {
			reg--;
		}
		*(dst++) = POP | ((reg - Z80_BC) << 4);
		return dst;
	}
}

uint8_t * and_r(uint8_t * dst, uint8_t reg)
{
	if (reg == Z80_IXH || reg == Z80_IXL) {
		*(dst++) = PRE_IX;
		return and_r(dst, reg - (Z80_IXL - Z80_L));
	} else if(reg == Z80_IYH || reg == Z80_IYL) {
		*(dst++) = PRE_IY;
		return and_r(dst, reg - (Z80_IYL - Z80_L));
	} else {
		if (reg == Z80_A) {
			reg = 7;
		} else {
			reg = (reg - Z80_C) ^ 1;
		}
		*(dst++) = AND_R | reg;
		return dst;
	}
}

uint8_t * inc_r(uint8_t *dst, uint8_t reg)
{
	if (reg == Z80_IXH || reg == Z80_IXL) {
		*(dst++) = PRE_IX;
		return inc_r(dst, reg - (Z80_IXL - Z80_L));
	} else if(reg == Z80_IYH || reg == Z80_IYL) {
		*(dst++) = PRE_IY;
		return inc_r(dst, reg - (Z80_IYL - Z80_L));
	} else {
		*(dst++) = INC_R8 | reg << 3;
		return dst;
	}
}

void mark_used8(uint8_t *reg_usage, uint16_t *reg_values, uint8_t reg, uint8_t init_value)
{
	reg_usage[reg] = 1;
	reg_values[reg] = init_value;
	uint8_t word_reg = z80_word_reg(reg);
	if (word_reg != Z80_UNUSED) {
		reg_usage[word_reg] = 1;
		reg_values[word_reg] = (reg_values[z80_high_reg(word_reg)] << 8) | (reg_values[z80_low_reg(word_reg)] & 0xFF);
	}
}

uint8_t alloc_reg8(uint8_t *reg_usage, uint16_t *reg_values, uint8_t init_value)
{
	for (uint8_t reg = 0; reg < Z80_BC; reg++)
	{
		if (!reg_usage[reg]) {
			mark_used8(reg_usage, reg_values, reg, init_value);
			return reg;
		}
	}
	return Z80_UNUSED;
}

void z80_gen_test(z80inst * inst, uint8_t *instbuf, uint8_t instlen)
{
	z80inst copy;
	uint16_t reg_values[Z80_UNUSED];
	uint8_t reg_usage[Z80_UNUSED];
	memset(reg_values, 0, sizeof(reg_values));
	memset(reg_usage, 0, sizeof(reg_usage));
	uint8_t addr_mode = inst->addr_mode & 0x1F;
	uint8_t word_sized = ((inst->reg != Z80_USE_IMMED && inst->reg != Z80_UNUSED && inst->reg >= Z80_BC) || (addr_mode == Z80_REG && inst->ea_reg >= Z80_BC)) ? 1 : 0;

	if (inst->reg == Z80_USE_IMMED || addr_mode == Z80_IMMED || addr_mode == Z80_IMMED_INDIRECT
		|| addr_mode == Z80_IX_DISPLACE || addr_mode == Z80_IY_DISPLACE)
	{
		memcpy(&copy, inst, sizeof(copy));
		inst = &copy;
		if ((inst->reg == Z80_USE_IMMED && inst->op != Z80_BIT && inst->op != Z80_RES && inst->op != Z80_SET)
			|| (addr_mode == Z80_IMMED && inst->op != Z80_IM))
		{
			copy.immed = rand() % (word_sized ? 65536 : 256);
		}
		if (addr_mode == Z80_IX_DISPLACE || addr_mode == Z80_IY_DISPLACE) {
			copy.ea_reg = rand() % 256;
		}
		if (addr_mode == Z80_IMMED_INDIRECT) {
			copy.immed = 0x1000 + (rand() % 256 - 128);
		}
	}
	uint8_t is_mem = 0;
	uint16_t address;
	int16_t offset;
	switch(addr_mode)
	{
	case Z80_REG:
		reg_usage[inst->ea_reg] = 1;
		if (word_sized) {
			reg_values[inst->ea_reg] = rand() % 65536;
			reg_values[z80_high_reg(inst->ea_reg)] = reg_values[inst->ea_reg] >> 8;
			reg_usage[z80_high_reg(inst->ea_reg)] = 1;
			reg_values[z80_low_reg(inst->ea_reg)] = reg_values[inst->ea_reg] & 0xFF;
			reg_usage[z80_low_reg(inst->ea_reg)] = 1;
		} else {
			mark_used8(reg_usage, reg_values, inst->ea_reg, rand() % 256);
		}
		break;
	case Z80_REG_INDIRECT:
		is_mem = 1;
		reg_values[inst->ea_reg] = 0x1000 + (rand() % 256 - 128);
		reg_usage[inst->ea_reg] = 1;
		address = reg_values[inst->ea_reg];
		reg_usage[z80_high_reg(inst->ea_reg)] = 1;
		reg_values[z80_high_reg(inst->ea_reg)] = reg_values[inst->ea_reg] >> 8;
		reg_usage[z80_low_reg(inst->ea_reg)] = 1;
		reg_values[z80_low_reg(inst->ea_reg)] = reg_values[inst->ea_reg] & 0xFF;
		break;
	case Z80_IMMED_INDIRECT:
		is_mem = 1;
		address = inst->immed;
		break;
	case Z80_IX_DISPLACE:
		reg_values[Z80_IX] = 0x1000;
		reg_usage[Z80_IX] = 1;
		reg_values[Z80_IXH] = 0x10;
		reg_usage[Z80_IXH] = 1;
		reg_values[Z80_IXL] = 0;
		reg_usage[Z80_IXL] = 1;
		is_mem = 1;
		offset = inst->ea_reg;
		if (offset > 0x7F) {
			offset -= 256;
		}
		address = 0x1000 + offset;
		break;
	case Z80_IY_DISPLACE:
		reg_values[Z80_IY] = 0x1000;
		reg_usage[Z80_IY] = 1;
		reg_values[Z80_IYH] = 0x10;
		reg_usage[Z80_IYH] = 1;
		reg_values[Z80_IYL] = 0;
		reg_usage[Z80_IYL] = 1;
		is_mem = 1;
		offset = inst->ea_reg;
		if (offset > 0x7F) {
			offset -= 256;
		}
		address = 0x1000 + offset;
		break;
	}
	if (inst->reg != Z80_UNUSED && inst->reg != Z80_USE_IMMED) {

		if (word_sized) {
			reg_values[inst->reg] = rand() % 65536;
			reg_usage[z80_high_reg(inst->reg)] = 1;
			reg_values[z80_high_reg(inst->reg)] = reg_values[inst->reg] >> 8;
			reg_usage[z80_low_reg(inst->reg)] = 1;
			reg_values[z80_low_reg(inst->reg)] = reg_values[inst->reg] & 0xFF;
		} else {
			if (!reg_usage[inst->reg]) {
				reg_values[inst->reg] = rand() % 255;
				uint8_t word_reg = z80_word_reg(inst->reg);
				if (word_reg != Z80_UNUSED) {
					reg_usage[word_reg] = 1;
					reg_values[word_reg] = (reg_values[z80_high_reg(word_reg)] << 8) | (reg_values[z80_low_reg(word_reg)] & 0xFF);
				}
			}
		}
		reg_usage[inst->reg] = 1;
	}
	uint8_t counter_reg = Z80_UNUSED;
	if (inst->op >= Z80_JP && inst->op <= Z80_JRCC) {
		counter_reg = alloc_reg8(reg_usage, reg_values, 0);
	}
	puts("--------------");
	for (uint8_t reg = 0; reg < Z80_UNUSED; reg++) {
		if (reg_values[reg]) {
			printf("%s: %X\n", z80_regs[reg], reg_values[reg]);
		}
	}
	char disbuf[80];
	z80_disasm(inst, disbuf, 0);
	puts(disbuf);
	char pathbuf[128];
	sprintf(pathbuf, "ztests/%s", z80_mnemonics[inst->op]);
	if (mkdir(pathbuf, 0777) != 0) {
		if (errno != EEXIST) {
			fprintf(stderr, "Failed to create directory %s\n", disbuf);
			exit(1);
		}
	}
	uint8_t prog[200];
	uint8_t *cur = prog;
	uint8_t mem_val;
	//disable interrupts
	*(cur++) = 0xF3;
	//setup SP
	cur = ld_ir16(cur, Z80_SP, 0x2000);
	for (int i = 0; i < 2; i ++) {
		//setup memory
		if (is_mem) {
			mem_val = rand() % 256;
			cur = ld_ir8(cur, Z80_A, mem_val);
			cur = ld_amem(cur, address);
		}
		//setup AF
		cur = ld_ir16(cur, Z80_BC, reg_values[Z80_A] << 8 | (i ? 0xFF : 0));
		cur = push(cur, Z80_BC);
		cur = pop(cur, Z80_AF);

		//setup other regs
		for (uint8_t reg = Z80_BC; reg <= Z80_IY; reg++) {
			if (reg != Z80_AF && reg != Z80_SP && (inst->op != Z80_JP || addr_mode != Z80_REG_INDIRECT || inst->ea_reg != reg)) {
				if (i == 1 && (z80_high_reg(reg) == counter_reg || z80_low_reg(reg) == counter_reg)) {
					if (z80_high_reg(reg) == counter_reg) {
						if (reg_usage[z80_low_reg(reg)]) {
							cur = ld_ir8(cur, z80_low_reg(reg), reg_values[z80_low_reg(reg)]);
						}
					} else if (reg_usage[z80_high_reg(reg)]) {
						cur = ld_ir8(cur, z80_high_reg(reg), reg_values[z80_high_reg(reg)]);
					}
				} else {
					cur = ld_ir16(cur, reg, reg_values[reg]);
				}
			}
		}

		if (inst->op == Z80_JP && addr_mode == Z80_REG_INDIRECT) {
			uint16_t address = cur - prog + (inst->ea_reg == Z80_HL ? 3 : 4) + instlen + 1 + i;
			cur = ld_ir16(cur, inst->ea_reg, address);
		}

		//copy instruction
		if (instlen == 3) {
			memcpy(cur, instbuf, 2);
			cur += 2;
		} else {
			memcpy(cur, instbuf, instlen);
			cur += instlen;
		}

		//immed/displacement byte(s)
		if (addr_mode == Z80_IX_DISPLACE || addr_mode == Z80_IY_DISPLACE) {
			*(cur++) = inst->ea_reg;
		} else if ((inst->op == Z80_JP || inst->op == Z80_JPCC) && addr_mode == Z80_IMMED) {
			uint16_t address = cur - prog + 3 + i; //2 for immed address, 1/2 for instruction(s) to skip
			*(cur++) = address;
			*(cur++) = address >> 8;
		} else if(inst->op == Z80_JR || inst->op == Z80_JRCC) {
			*(cur++) = 1 + i; //skip one or 2 instructions based on value of i
		} else if (addr_mode == Z80_IMMED & inst->op != Z80_IM) {
			*(cur++) = inst->immed & 0xFF;
			if (word_sized) {
				*(cur++) = inst->immed >> 8;
			}
		} else if (addr_mode == Z80_IMMED_INDIRECT) {
			*(cur++) = inst->immed & 0xFF;
			*(cur++) = inst->immed >> 8;
		}
		if (inst->reg == Z80_USE_IMMED && inst->op != Z80_BIT && inst->op != Z80_RES && inst->op != Z80_SET) {
			*(cur++) = inst->immed & 0xFF;
		}
		if (instlen == 3) {
			*(cur++) = instbuf[2];
		}
		if (inst->op >= Z80_JP && inst->op <= Z80_JRCC) {
			cur = inc_r(cur, counter_reg);
			if (i) {
				//inc twice on second iteration so we can differentiate the two
				cur = inc_r(cur, counter_reg);
			}
		}
		if (!i) {
			//Save AF from first run
			cur = push(cur, Z80_AF);
			if (is_mem) {
				//Save memory location from frist run
				cur = ld_mema(cur, address);
				cur = push(cur, Z80_AF);
			}
		} else {
			//Pop AF from first run for final result
			for (int reg = Z80_BC; reg <= Z80_IY; reg++) {
				if (reg != Z80_AF && !reg_usage[reg]) {
					cur = pop(cur, reg);
					cur = push(cur, Z80_AF);
					cur = ld_ir8(cur, Z80_A, 0xC7);
					cur = and_r(cur, z80_low_reg(reg));
					cur = ld_rr8(cur, Z80_A, z80_low_reg(reg));
					cur = pop(cur, Z80_AF);
					reg_usage[reg] = 1;
					reg_usage[z80_low_reg(reg)] = 1;
					break;
				}
			}
			if (is_mem) {
				//Pop memory location from frist run
				for (int reg = Z80_BC; reg <= Z80_IY; reg++) {
					if (reg != Z80_AF && !reg_usage[reg]) {
						cur = pop(cur, reg);
						cur = push(cur, Z80_AF);
						cur = ld_mema(cur, address);
						cur = ld_rr8(cur, Z80_A, z80_low_reg(reg));
						cur = pop(cur, Z80_AF);
						reg_usage[reg] = 1;
						reg_usage[z80_low_reg(reg)] = 1;
						break;
					}
				}
			}
		}
	}

	for (char * cur = disbuf; *cur != 0; cur++) {
		if (*cur == ',' || *cur == ' ') {
			*cur = '_';
		}
	}
	//save memory result
	if (is_mem) {
		if (reg_usage[Z80_A]) {
			cur = push(cur, Z80_AF);
		}
		cur = ld_mema(cur, address);
		if (reg_usage[Z80_A]) {
			for (int reg = 0; reg < Z80_I; reg++) {
				if (!reg_usage[reg]) {
					cur = ld_rr8(cur, Z80_A, reg);
					break;
				}
			}
			cur = pop(cur, Z80_AF);
		}
	}

	//halt
	*(cur++) = 0x76;
	sprintf(pathbuf + strlen(pathbuf), "/%s.bin", disbuf);
	FILE * progfile = fopen(pathbuf, "wb");
	fwrite(prog, 1, cur - prog, progfile);
	fclose(progfile);
}


uint8_t should_skip(z80inst * inst)
{
	return inst->op >= Z80_DJNZ || (inst->op >= Z80_LDI && inst->op <= Z80_CPDR) || inst->op == Z80_HALT
		|| inst->op == Z80_RLD || inst->op == Z80_RRD || inst->op == Z80_NOP
		|| inst->op == Z80_DI || inst->op == Z80_EI;
}

void z80_gen_all()
{
	uint8_t inst[3];
	for (int op = 0; op < 256; op++) {
		inst[0] = op;
		if (op == 0xCB) {
			for (int subop = 0; subop < 256; subop++) {
				if (!should_skip(z80_tbl_bit + subop)) {
					inst[1] = subop;
					z80_gen_test(z80_tbl_bit + subop, inst, 2);
				}
			}
		} else if(op == 0xDD) {
			for (int ixop = 0; ixop < 256; ixop++) {
				inst[1] = ixop;
				if (ixop == 0xCB) {
					for (int subop = 0; subop < 256; subop++) {
						if (!should_skip(z80_tbl_ix_bit + subop)) {
							inst[2] = subop;
							z80_gen_test(z80_tbl_ix_bit + subop, inst, 3);
						}
					}
				} else {
					if (!should_skip(z80_tbl_ix + ixop)) {
						z80_gen_test(z80_tbl_ix + ixop, inst, 2);
					}
				}
			}
		} else if(op == 0xED) {
			for (int subop = 0; subop < sizeof(z80_tbl_extd)/sizeof(z80inst); subop++) {
				if (!should_skip(z80_tbl_extd + subop)) {
					inst[1] = subop + 0x40;
					z80_gen_test(z80_tbl_extd + subop, inst, 2);
				}
			}
		} else if(op == 0xFD) {
			for (int iyop = 0; iyop < 256; iyop++) {
				inst[1] = iyop;
				if (iyop == 0xCB) {
					for (int subop = 0; subop < 256; subop++) {
						if (!should_skip(z80_tbl_iy_bit + subop)) {
							inst[2] = subop;
							z80_gen_test(z80_tbl_iy_bit + subop, inst, 3);
						}
					}
				} else {
					if (!should_skip(z80_tbl_iy + iyop)) {
						z80_gen_test(z80_tbl_iy + iyop, inst, 2);
					}
				}
			}
		} else {
			if (!should_skip(z80_tbl_a + op)) {
				z80_gen_test(z80_tbl_a + op, inst, 1);
			}
		}
	}
}

int main(int argc, char ** argv)
{
	srand(time(NULL));
	z80_gen_all();
	return 0;
}
