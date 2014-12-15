/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "z80inst.h"
#include "z80_to_x86.h"
#include "gen_x86.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define MODE_UNUSED (MODE_IMMED-1)

#define ZCYCLES RBP
#define ZLIMIT RDI
#define SCRATCH1 R13
#define SCRATCH2 R14
#define CONTEXT RSI

//#define DO_DEBUG_PRINT

#ifdef DO_DEBUG_PRINT
#define dprintf printf
#else
#define dprintf
#endif

void z80_read_byte();
void z80_read_word();
void z80_write_byte();
void z80_write_word_highfirst();
void z80_write_word_lowfirst();
void z80_save_context();
void z80_native_addr();
void z80_do_sync();
void z80_handle_cycle_limit_int();
void z80_retrans_stub();
void z80_io_read();
void z80_io_write();
void z80_halt();
void z80_save_context();
void z80_load_context();

uint8_t z80_size(z80inst * inst)
{
	uint8_t reg = (inst->reg & 0x1F);
	if (reg != Z80_UNUSED && reg != Z80_USE_IMMED) {
		return reg < Z80_BC ? SZ_B : SZ_W;
	}
	//TODO: Handle any necessary special cases
	return SZ_B;
}

void translate_z80_reg(z80inst * inst, x86_ea * ea, z80_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->reg == Z80_USE_IMMED) {
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
	} else if ((inst->reg & 0x1F) == Z80_UNUSED) {
		ea->mode = MODE_UNUSED;
	} else {
		ea->mode = MODE_REG_DIRECT;
		if (inst->reg == Z80_IYH) {
			if ((inst->addr_mode & 0x1F) == Z80_REG && inst->ea_reg == Z80_IYL) {
				mov_rr(code, opts->regs[Z80_IY], opts->gen.scratch1, SZ_W);
				ror_ir(code, 8, opts->gen.scratch1, SZ_W);
				ea->base = opts->gen.scratch1;
			} else {
				ea->base = opts->regs[Z80_IYL];
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			}
		} else if(opts->regs[inst->reg] >= 0) {
			ea->base = opts->regs[inst->reg];
			if (ea->base >= AH && ea->base <= BH) {
				if ((inst->addr_mode & 0x1F) == Z80_REG) {
					uint8_t other_reg = opts->regs[inst->ea_reg];
					if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
						//we can't mix an *H reg with a register that requires the REX prefix
						ea->base = opts->regs[z80_low_reg(inst->reg)];
						ror_ir(code, 8, ea->base, SZ_W);
					}
				} else if((inst->addr_mode & 0x1F) != Z80_UNUSED && (inst->addr_mode & 0x1F) != Z80_IMMED) {
					//temp regs require REX prefix too
					ea->base = opts->regs[z80_low_reg(inst->reg)];
					ror_ir(code, 8, ea->base, SZ_W);
				}
			}
		} else {
			ea->mode = MODE_REG_DISPLACE8;
			ea->base = opts->gen.context_reg;
			ea->disp = offsetof(z80_context, regs) + inst->reg;
		}
	}
}

void z80_save_reg(z80inst * inst, z80_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->reg == Z80_IYH) {
		if ((inst->addr_mode & 0x1F) == Z80_REG && inst->ea_reg == Z80_IYL) {
			ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			mov_rr(code, opts->gen.scratch1, opts->regs[Z80_IYL], SZ_B);
			ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
		} else {
			ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
		}
	} else if (opts->regs[inst->reg] >= AH && opts->regs[inst->reg] <= BH) {
		if ((inst->addr_mode & 0x1F) == Z80_REG) {
			uint8_t other_reg = opts->regs[inst->ea_reg];
			if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
				//we can't mix an *H reg with a register that requires the REX prefix
				ror_ir(code, 8, opts->regs[z80_low_reg(inst->reg)], SZ_W);
			}
		} else if((inst->addr_mode & 0x1F) != Z80_UNUSED && (inst->addr_mode & 0x1F) != Z80_IMMED) {
			//temp regs require REX prefix too
			ror_ir(code, 8, opts->regs[z80_low_reg(inst->reg)], SZ_W);
		}
	}
}

void translate_z80_ea(z80inst * inst, x86_ea * ea, z80_options * opts, uint8_t read, uint8_t modify)
{
	code_info *code = &opts->gen.code;
	uint8_t size, reg, areg;
	ea->mode = MODE_REG_DIRECT;
	areg = read ? opts->gen.scratch1 : opts->gen.scratch2;
	switch(inst->addr_mode & 0x1F)
	{
	case Z80_REG:
		if (inst->ea_reg == Z80_IYH) {
			if (inst->reg == Z80_IYL) {
				mov_rr(code, opts->regs[Z80_IY], opts->gen.scratch1, SZ_W);
				ror_ir(code, 8, opts->gen.scratch1, SZ_W);
				ea->base = opts->gen.scratch1;
			} else {
				ea->base = opts->regs[Z80_IYL];
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			}
		} else {
			ea->base = opts->regs[inst->ea_reg];
			if (ea->base >= AH && ea->base <= BH && inst->reg != Z80_UNUSED && inst->reg != Z80_USE_IMMED) {
				uint8_t other_reg = opts->regs[inst->reg];
				if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
					//we can't mix an *H reg with a register that requires the REX prefix
					ea->base = opts->regs[z80_low_reg(inst->ea_reg)];
					ror_ir(code, 8, ea->base, SZ_W);
				}
			}
		}
		break;
	case Z80_REG_INDIRECT:
		mov_rr(code, opts->regs[inst->ea_reg], areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				//push_r(code, opts->gen.scratch1);
				mov_rrdisp8(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(z80_context, scratch1), SZ_W);
			}
			if (size == SZ_B) {
				call(code, opts->read_8);
			} else {
				dst = call(dst, opts->read_16);
			}
			if (modify) {
				//pop_r(code, opts->gen.scratch2);
				mov_rdisp8r(code, opts->gen.context_reg, offsetof(z80_context, scratch1), opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_IMMED:
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
		break;
	case Z80_IMMED_INDIRECT:
		dst = mov_ir(dst, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			/*if (modify) {
				dst = push_r(dst, opts->gen.scratch1);
			}*/
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				//dst = pop_r(dst, opts->gen.scratch2);
				dst = mov_ir(dst, inst->immed, opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_IX_DISPLACE:
	case Z80_IY_DISPLACE:
		reg = opts->regs[(inst->addr_mode & 0x1F) == Z80_IX_DISPLACE ? Z80_IX : Z80_IY];
		dst = mov_rr(dst, reg, areg, SZ_W);
		dst = add_ir(dst, inst->ea_reg & 0x80 ? inst->ea_reg - 256 : inst->ea_reg, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				//dst = push_r(dst, opts->gen.scratch1);
				dst = mov_rrdisp8(dst, opts->gen.scratch1, opts->gen.context_reg, offsetof(z80_context, scratch1), SZ_W);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				//dst = pop_r(dst, opts->gen.scratch2);
				dst = mov_rdisp8r(dst, opts->gen.context_reg, offsetof(z80_context, scratch1), opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_UNUSED:
		ea->mode = MODE_UNUSED;
		break;
	default:
		fprintf(stderr, "Unrecognized Z80 addressing mode %d\n", inst->addr_mode & 0x1F);
		exit(1);
	}
	return dst;
}

uint8_t * z80_save_ea(uint8_t * dst, z80inst * inst, z80_options * opts)
{
	if ((inst->addr_mode & 0x1F) == Z80_REG) {
		if (inst->ea_reg == Z80_IYH) {
			if (inst->reg == Z80_IYL) {
				dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
				dst = mov_rr(dst, opts->gen.scratch1, opts->regs[Z80_IYL], SZ_B);
				dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
			} else {
				dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
			}
		} else if (inst->reg != Z80_UNUSED && inst->reg != Z80_USE_IMMED && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			uint8_t other_reg = opts->regs[inst->reg];
			if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
				//we can't mix an *H reg with a register that requires the REX prefix
				dst = ror_ir(dst, 8, opts->regs[z80_low_reg(inst->ea_reg)], SZ_W);
			}
		}
	}
	return dst;
}

uint8_t * z80_save_result(uint8_t * dst, z80inst * inst)
{
	switch(inst->addr_mode & 0x1f)
	{
	case Z80_REG_INDIRECT:
	case Z80_IMMED_INDIRECT:
	case Z80_IX_DISPLACE:
	case Z80_IY_DISPLACE:
		if (z80_size(inst) == SZ_B) {
			dst = call(dst, (uint8_t *)z80_write_byte);
		} else {
			dst = call(dst, (uint8_t *)z80_write_word_lowfirst);
		}
	}
	return dst;
}

enum {
	DONT_READ=0,
	READ
};

enum {
	DONT_MODIFY=0,
	MODIFY
};

uint8_t zf_off(uint8_t flag)
{
	return offsetof(z80_context, flags) + flag;
}

uint8_t zaf_off(uint8_t flag)
{
	return offsetof(z80_context, alt_flags) + flag;
}

uint8_t zar_off(uint8_t reg)
{
	return offsetof(z80_context, alt_regs) + reg;
}

void z80_print_regs_exit(z80_context * context)
{
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\nIX: %X\nIY: %X\nSP: %X\n\nIM: %d, IFF1: %d, IFF2: %d\n",
		context->regs[Z80_A], context->regs[Z80_B], context->regs[Z80_C],
		context->regs[Z80_D], context->regs[Z80_E],
		(context->regs[Z80_H] << 8) | context->regs[Z80_L],
		(context->regs[Z80_IXH] << 8) | context->regs[Z80_IXL],
		(context->regs[Z80_IYH] << 8) | context->regs[Z80_IYL],
		context->sp, context->im, context->iff1, context->iff2);
	puts("--Alternate Regs--");
	printf("A: %X\nB: %X\nC: %X\nD: %X\nE: %X\nHL: %X\nIX: %X\nIY: %X\n",
		context->alt_regs[Z80_A], context->alt_regs[Z80_B], context->alt_regs[Z80_C],
		context->alt_regs[Z80_D], context->alt_regs[Z80_E],
		(context->alt_regs[Z80_H] << 8) | context->alt_regs[Z80_L],
		(context->alt_regs[Z80_IXH] << 8) | context->alt_regs[Z80_IXL],
		(context->alt_regs[Z80_IYH] << 8) | context->alt_regs[Z80_IYL]);
	exit(0);
}

uint8_t * translate_z80inst(z80inst * inst, z80_context * context, uint16_t address)
{
	uint32_t cycles;
	x86_ea src_op, dst_op;
	uint8_t size;
	z80_options *opts = context->options;
	uint8_t * start = opts->code.cur;
	check_cycles_int(&opts->gen, address);
	switch(inst->op)
	{
	case Z80_LD:
		size = z80_size(inst);
		switch (inst->addr_mode & 0x1F)
		{
		case Z80_REG:
		case Z80_REG_INDIRECT:
 			cycles = size == SZ_B ? 4 : 6;
			if (inst->ea_reg == Z80_IX || inst->ea_reg == Z80_IY) {
				cycles += 4;
			}
			if (inst->reg == Z80_I || inst->ea_reg == Z80_I) {
				cycles += 5;
			}
			break;
		case Z80_IMMED:
			cycles = size == SZ_B ? 7 : 10;
			break;
		case Z80_IMMED_INDIRECT:
			cycles = 10;
			break;
		case Z80_IX_DISPLACE:
		case Z80_IY_DISPLACE:
			cycles = 16;
			break;
		}
		if ((inst->reg >= Z80_IXL && inst->reg <= Z80_IYH) || inst->reg == Z80_IX || inst->reg == Z80_IY) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode & Z80_DIR) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, DONT_READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts);
		} else {
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			if(dst_op.mode == MODE_REG_DISPLACE8) {
				dst = mov_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
			} else {
				dst = mov_rr(dst, src_op.base, dst_op.base, size);
			}
		} else if(src_op.mode == MODE_IMMED) {
			dst = mov_ir(dst, src_op.disp, dst_op.base, size);
		} else {
			dst = mov_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, size);
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		if (inst->addr_mode & Z80_DIR) {
			dst = z80_save_result(dst, inst);
		}
		break;
	case Z80_PUSH:
		dst = cycles(&opts->gen, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 9 : 5);
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {
			dst = mov_rr(dst, opts->regs[Z80_A], opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 8, opts->gen.scratch1, SZ_W);
			dst = mov_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_S), opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 1, opts->gen.scratch1, SZ_B);
			dst = or_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_Z), opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 2, opts->gen.scratch1, SZ_B);
			dst = or_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_H), opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 2, opts->gen.scratch1, SZ_B);
			dst = or_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_PV), opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 1, opts->gen.scratch1, SZ_B);
			dst = or_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_N), opts->gen.scratch1, SZ_B);
			dst = shl_ir(dst, 1, opts->gen.scratch1, SZ_B);
			dst = or_rdisp8r(dst, opts->gen.context_reg, zf_off(ZF_C), opts->gen.scratch1, SZ_B);
		} else {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = mov_rr(dst, src_op.base, opts->gen.scratch1, SZ_W);
		}
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_word_highfirst);
		//no call to save_z80_reg needed since there's no chance we'll use the only
		//the upper half of a register pair
		break;
	case Z80_POP:
		dst = cycles(&opts->gen, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 8 : 4);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {

			dst = bt_ir(dst, 0, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
			dst = bt_ir(dst, 1, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_N));
			dst = bt_ir(dst, 2, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_PV));
			dst = bt_ir(dst, 4, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_H));
			dst = bt_ir(dst, 6, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_Z));
			dst = bt_ir(dst, 7, opts->gen.scratch1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_S));
			dst = shr_ir(dst, 8, opts->gen.scratch1, SZ_W);
			dst = mov_rr(dst, opts->gen.scratch1, opts->regs[Z80_A], SZ_B);
		} else {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = mov_rr(dst, opts->gen.scratch1, src_op.base, SZ_W);
		}
		//no call to save_z80_reg needed since there's no chance we'll use the only
		//the upper half of a register pair
		break;
	case Z80_EX:
		if (inst->addr_mode == Z80_REG || inst->reg == Z80_HL) {
			cycles = 4;
		} else {
			cycles = 8;
		}
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode == Z80_REG) {
			if(inst->reg == Z80_AF) {
				dst = mov_rr(dst, opts->regs[Z80_A], opts->gen.scratch1, SZ_B);
				dst = mov_rdisp8r(dst, opts->gen.context_reg, zar_off(Z80_A), opts->regs[Z80_A], SZ_B);
				dst = mov_rrdisp8(dst, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_A), SZ_B);

				//Flags are currently word aligned, so we can move
				//them efficiently a word at a time
				for (int f = ZF_C; f < ZF_NUM; f+=2) {
					dst = mov_rdisp8r(dst, opts->gen.context_reg, zf_off(f), opts->gen.scratch1, SZ_W);
					dst = mov_rdisp8r(dst, opts->gen.context_reg, zaf_off(f), opts->gen.scratch2, SZ_W);
					dst = mov_rrdisp8(dst, opts->gen.scratch1, opts->gen.context_reg, zaf_off(f), SZ_W);
					dst = mov_rrdisp8(dst, opts->gen.scratch2, opts->gen.context_reg, zf_off(f), SZ_W);
				}
			} else {
				dst = xchg_rr(dst, opts->regs[Z80_DE], opts->regs[Z80_HL], SZ_W);
			}
		} else {
			dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_read_byte);
			dst = xchg_rr(dst, opts->regs[inst->reg], opts->gen.scratch1, SZ_B);
			dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
			dst = call(dst, (uint8_t *)z80_write_byte);
			dst = cycles(&opts->gen, 1);
			uint8_t high_reg = z80_high_reg(inst->reg);
			uint8_t use_reg;
			//even though some of the upper halves can be used directly
			//the limitations on mixing *H regs with the REX prefix
			//prevent us from taking advantage of it
			use_reg = opts->regs[inst->reg];
			dst = ror_ir(dst, 8, use_reg, SZ_W);
			dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
			dst = add_ir(dst, 1, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_read_byte);
			dst = xchg_rr(dst, use_reg, opts->gen.scratch1, SZ_B);
			dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
			dst = add_ir(dst, 1, opts->gen.scratch2, SZ_W);
			dst = call(dst, (uint8_t *)z80_write_byte);
			//restore reg to normal rotation
			dst = ror_ir(dst, 8, use_reg, SZ_W);
			dst = cycles(&opts->gen, 2);
		}
		break;
	case Z80_EXX:
		dst = cycles(&opts->gen, 4);
		dst = mov_rr(dst, opts->regs[Z80_BC], opts->gen.scratch1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch2, SZ_W);
		dst = mov_rdisp8r(dst, opts->gen.context_reg, zar_off(Z80_C), opts->regs[Z80_BC], SZ_W);
		dst = mov_rdisp8r(dst, opts->gen.context_reg, zar_off(Z80_L), opts->regs[Z80_HL], SZ_W);
		dst = mov_rrdisp8(dst, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_C), SZ_W);
		dst = mov_rrdisp8(dst, opts->gen.scratch2, opts->gen.context_reg, zar_off(Z80_L), SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_DE], opts->gen.scratch1, SZ_W);
		dst = mov_rdisp8r(dst, opts->gen.context_reg, zar_off(Z80_E), opts->regs[Z80_DE], SZ_W);
		dst = mov_rrdisp8(dst, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_E), SZ_W);
		break;
	case Z80_LDI: {
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		dst = mov_rr(dst, opts->regs[Z80_DE], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		dst = cycles(&opts->gen, 2);
		dst = add_ir(dst, 1, opts->regs[Z80_DE], SZ_W);
		dst = add_ir(dst, 1, opts->regs[Z80_HL], SZ_W);
		dst = sub_ir(dst, 1, opts->regs[Z80_BC], SZ_W);
		//TODO: Implement half-carry
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_NZ, opts->gen.context_reg, zf_off(ZF_PV));
		break;
	}
	case Z80_LDIR: {
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		dst = mov_rr(dst, opts->regs[Z80_DE], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		dst = add_ir(dst, 1, opts->regs[Z80_DE], SZ_W);
		dst = add_ir(dst, 1, opts->regs[Z80_HL], SZ_W);

		dst = sub_ir(dst, 1, opts->regs[Z80_BC], SZ_W);
		uint8_t * cont = dst+1;
		dst = jcc(dst, CC_Z, dst+2);
		dst = cycles(&opts->gen, 7);
		//TODO: Figure out what the flag state should be here
		//TODO: Figure out whether an interrupt can interrupt this
		dst = jmp(dst, start);
		*cont = dst - (cont + 1);
		dst = cycles(&opts->gen, 2);
		//TODO: Implement half-carry
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
		break;
	}
	case Z80_LDD: {
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		dst = mov_rr(dst, opts->regs[Z80_DE], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		dst = cycles(&opts->gen, 2);
		dst = sub_ir(dst, 1, opts->regs[Z80_DE], SZ_W);
		dst = sub_ir(dst, 1, opts->regs[Z80_HL], SZ_W);
		dst = sub_ir(dst, 1, opts->regs[Z80_BC], SZ_W);
		//TODO: Implement half-carry
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_NZ, opts->gen.context_reg, zf_off(ZF_PV));
		break;
	}
	case Z80_LDDR: {
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		dst = mov_rr(dst, opts->regs[Z80_DE], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		dst = sub_ir(dst, 1, opts->regs[Z80_DE], SZ_W);
		dst = sub_ir(dst, 1, opts->regs[Z80_HL], SZ_W);

		dst = sub_ir(dst, 1, opts->regs[Z80_BC], SZ_W);
		uint8_t * cont = dst+1;
		dst = jcc(dst, CC_Z, dst+2);
		dst = cycles(&opts->gen, 7);
		//TODO: Figure out what the flag state should be here
		//TODO: Figure out whether an interrupt can interrupt this
		dst = jmp(dst, start);
		*cont = dst - (cont + 1);
		dst = cycles(&opts->gen, 2);
		//TODO: Implement half-carry
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
		break;
	}
	/*case Z80_CPI:
	case Z80_CPIR:
	case Z80_CPD:
	case Z80_CPDR:
		break;*/
	case Z80_ADD:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = add_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = add_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_ADC:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		dst = bt_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = adc_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = adc_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_SUB:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = sub_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = sub_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_SBC:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		dst = bt_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = sbb_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = sbb_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_AND:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = and_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = and_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_OR:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = or_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = or_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_XOR:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = xor_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = xor_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_CP:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = cmp_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = cmp_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	case Z80_INC:
		cycles = 4;
		if (inst->reg == Z80_IX || inst->reg == Z80_IY) {
			cycles += 6;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 2;
		} else if(inst->reg == Z80_IXH || inst->reg == Z80_IXL || inst->reg == Z80_IYH || inst->reg == Z80_IYL || inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		if (dst_op.mode == MODE_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
		}
		dst = add_ir(dst, 1, dst_op.base, z80_size(inst));
		if (z80_size(inst) == SZ_B) {
			dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		dst = z80_save_result(dst, inst);
		break;
	case Z80_DEC:
		cycles = 4;
		if (inst->reg == Z80_IX || inst->reg == Z80_IY) {
			cycles += 6;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 2;
		} else if(inst->reg == Z80_IXH || inst->reg == Z80_IXL || inst->reg == Z80_IYH || inst->reg == Z80_IYL || inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		if (dst_op.mode == MODE_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
		}
		dst = sub_ir(dst, 1, dst_op.base, z80_size(inst));
		if (z80_size(inst) == SZ_B) {
			dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		dst = z80_save_result(dst, inst);
		break;
	//case Z80_DAA:
	case Z80_CPL:
		dst = cycles(&opts->gen, 4);
		dst = not_r(dst, opts->regs[Z80_A], SZ_B);
		//TODO: Implement half-carry flag
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		break;
	case Z80_NEG:
		dst = cycles(&opts->gen, 8);
		dst = neg_r(dst, opts->regs[Z80_A], SZ_B);
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = setcc_rdisp8(dst, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		break;
	case Z80_CCF:
		dst = cycles(&opts->gen, 4);
		dst = xor_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_SCF:
		dst = cycles(&opts->gen, 4);
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_NOP:
		if (inst->immed == 42) {
			dst = call(dst, (uint8_t *)z80_save_context);
			dst = mov_rr(dst, opts->gen.context_reg, RDI, SZ_Q);
			dst = jmp(dst, (uint8_t *)z80_print_regs_exit);
		} else {
			dst = cycles(&opts->gen, 4 * inst->immed);
		}
		break;
	case Z80_HALT:
		dst = cycles(&opts->gen, 4);
		dst = mov_ir(dst, address, opts->gen.scratch1, SZ_W);
		uint8_t * call_inst = dst;
		dst = call(dst, (uint8_t *)z80_halt);
		dst = jmp(dst, call_inst);
		break;
	case Z80_DI:
		dst = cycles(&opts->gen, 4);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
		dst = mov_rdisp8r(dst, opts->gen.context_reg, offsetof(z80_context, sync_cycle), opts->gen.limit, SZ_D);
		dst = mov_irdisp8(dst, 0xFFFFFFFF, opts->gen.context_reg, offsetof(z80_context, int_cycle), SZ_D);
		break;
	case Z80_EI:
		dst = cycles(&opts->gen, 4);
		dst = mov_rrdisp32(dst, opts->gen.cycles, opts->gen.context_reg, offsetof(z80_context, int_enable_cycle), SZ_D);
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		dst = mov_irdisp8(dst, 1, opts->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
		//interrupt enable has a one-instruction latency, minimum instruction duration is 4 cycles
		dst = add_irdisp32(dst, 4, opts->gen.context_reg, offsetof(z80_context, int_enable_cycle), SZ_D);
		dst = call(dst, (uint8_t *)z80_do_sync);
		break;
	case Z80_IM:
		dst = cycles(&opts->gen, 4);
		dst = mov_irdisp8(dst, inst->immed, opts->gen.context_reg, offsetof(z80_context, im), SZ_B);
		break;
	case Z80_RLC:
		cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = rol_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RL:
		cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = bt_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		dst = rcl_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RRC:
		cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = ror_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RR:
		cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = bt_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		dst = rcr_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_SLA:
	case Z80_SLL:
		cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = shl_ir(dst, 1, dst_op.base, SZ_B);
		dst  = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		if (inst->op == Z80_SLL) {
			dst = or_ir(dst, 1, dst_op.base, SZ_B);
		}
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_SRA:
		cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = sar_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst  = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_SRL:
		cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = translate_z80_reg(inst, &src_op, dst, opts); //For IX/IY variants that also write to a register
			dst = cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = shr_ir(dst, 1, dst_op.base, SZ_B);
		if (src_op.mode != MODE_UNUSED) {
			dst = mov_rr(dst, dst_op.base, src_op.base, SZ_B);
		}
		dst  = setcc_rdisp8(dst, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
			if (src_op.mode != MODE_UNUSED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RLD:
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		//Before: (HL) = 0x12, A = 0x34
		//After: (HL) = 0x24, A = 0x31
		dst = mov_rr(dst, opts->regs[Z80_A], opts->gen.scratch2, SZ_B);
		dst = shl_ir(dst, 4, opts->gen.scratch1, SZ_W);
		dst = and_ir(dst, 0xF, opts->gen.scratch2, SZ_W);
		dst = and_ir(dst, 0xFFF, opts->gen.scratch1, SZ_W);
		dst = and_ir(dst, 0xF0, opts->regs[Z80_A], SZ_B);
		dst = or_rr(dst, opts->gen.scratch2, opts->gen.scratch1, SZ_W);
		//opts->gen.scratch1 = 0x0124
		dst = ror_ir(dst, 8, opts->gen.scratch1, SZ_W);
		dst = cycles(&opts->gen, 4);
		dst = or_rr(dst, opts->gen.scratch1, opts->regs[Z80_A], SZ_B);
		//set flags
		//TODO: Implement half-carry flag
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));

		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch2, SZ_W);
		dst = ror_ir(dst, 8, opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		break;
	case Z80_RRD:
		dst = cycles(&opts->gen, 8);
		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_byte);
		//Before: (HL) = 0x12, A = 0x34
		//After: (HL) = 0x41, A = 0x32
		dst = movzx_rr(dst, opts->regs[Z80_A], opts->gen.scratch2, SZ_B, SZ_W);
		dst = ror_ir(dst, 4, opts->gen.scratch1, SZ_W);
		dst = shl_ir(dst, 4, opts->gen.scratch2, SZ_W);
		dst = and_ir(dst, 0xF00F, opts->gen.scratch1, SZ_W);
		dst = and_ir(dst, 0xF0, opts->regs[Z80_A], SZ_B);
		//opts->gen.scratch1 = 0x2001
		//opts->gen.scratch2 = 0x0040
		dst = or_rr(dst, opts->gen.scratch2, opts->gen.scratch1, SZ_W);
		//opts->gen.scratch1 = 0x2041
		dst = ror_ir(dst, 8, opts->gen.scratch1, SZ_W);
		dst = cycles(&opts->gen, 4);
		dst = shr_ir(dst, 4, opts->gen.scratch1, SZ_B);
		dst = or_rr(dst, opts->gen.scratch1, opts->regs[Z80_A], SZ_B);
		//set flags
		//TODO: Implement half-carry flag
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));

		dst = mov_rr(dst, opts->regs[Z80_HL], opts->gen.scratch2, SZ_W);
		dst = ror_ir(dst, 8, opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_byte);
		break;
	case Z80_BIT: {
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = cycles(&opts->gen, cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read at the end of a bit instruction takes 4
			dst = cycles(&opts->gen, 1);
		}
		dst = bt_ir(dst, bit, src_op.base, size);
		dst = setcc_rdisp8(dst, CC_NC, opts->gen.context_reg, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_NC, opts->gen.context_reg, zf_off(ZF_PV));
		dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		if (inst->immed == 7) {
			dst = cmp_ir(dst, 0, src_op.base, size);
			dst = setcc_rdisp8(dst, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		} else {
			dst = mov_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
		}
		break;
	}
	case Z80_SET: {
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = cycles(&opts->gen, cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, MODIFY);
		}
		if (inst->reg != Z80_USE_IMMED) {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			dst = cycles(&opts->gen, 1);
		}
		dst = bts_ir(dst, bit, src_op.base, size);
		if (inst->reg != Z80_USE_IMMED) {
			if (size == SZ_W) {
				if (dst_op.base >= R8) {
					dst = ror_ir(dst, 8, src_op.base, SZ_W);
					dst = mov_rr(dst, opts->regs[z80_low_reg(inst->ea_reg)], dst_op.base, SZ_B);
					dst = ror_ir(dst, 8, src_op.base, SZ_W);
				} else {
					dst = mov_rr(dst, opts->regs[inst->ea_reg], dst_op.base, SZ_B);
				}
			} else {
				dst = mov_rr(dst, src_op.base, dst_op.base, SZ_B);
			}
		}
		if ((inst->addr_mode & 0x1F) != Z80_REG) {
			dst = z80_save_result(dst, inst);
			if (inst->reg != Z80_USE_IMMED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		}
		break;
	}
	case Z80_RES: {
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = cycles(&opts->gen, cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, MODIFY);
		}
		if (inst->reg != Z80_USE_IMMED) {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			dst = cycles(&opts->gen, 1);
		}
		dst = btr_ir(dst, bit, src_op.base, size);
		if (inst->reg != Z80_USE_IMMED) {
			if (size == SZ_W) {
				if (dst_op.base >= R8) {
					dst = ror_ir(dst, 8, src_op.base, SZ_W);
					dst = mov_rr(dst, opts->regs[z80_low_reg(inst->ea_reg)], dst_op.base, SZ_B);
					dst = ror_ir(dst, 8, src_op.base, SZ_W);
				} else {
					dst = mov_rr(dst, opts->regs[inst->ea_reg], dst_op.base, SZ_B);
				}
			} else {
				dst = mov_rr(dst, src_op.base, dst_op.base, SZ_B);
			}
		}
		if (inst->addr_mode != Z80_REG) {
			dst = z80_save_result(dst, inst);
			if (inst->reg != Z80_USE_IMMED) {
				dst = z80_save_reg(dst, inst, opts);
			}
		}
		break;
	}
	case Z80_JP: {
		cycles = 4;
		if (inst->addr_mode != Z80_REG_INDIRECT) {
			cycles += 6;
		} else if(inst->ea_reg == Z80_IX || inst->ea_reg == Z80_IY) {
			cycles += 4;
		}
		dst = cycles(&opts->gen, cycles);
		if (inst->addr_mode != Z80_REG_INDIRECT && inst->immed < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, inst->immed, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			if (inst->addr_mode == Z80_REG_INDIRECT) {
				dst = mov_rr(dst, opts->regs[inst->ea_reg], opts->gen.scratch1, SZ_W);
			} else {
				dst = mov_ir(dst, inst->immed, opts->gen.scratch1, SZ_W);
			}
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		break;
	}
	case Z80_JPCC: {
		dst = cycles(&opts->gen, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = inst->immed;
		if (dest_addr < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, dest_addr, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, dest_addr, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	}
	case Z80_JR: {
		dst = cycles(&opts->gen, 12);//T States: 4,3,5
		uint16_t dest_addr = address + inst->immed + 2;
		if (dest_addr < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, dest_addr, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, dest_addr, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		break;
	}
	case Z80_JRCC: {
		dst = cycles(&opts->gen, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		}
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = address + inst->immed + 2;
		if (dest_addr < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, dest_addr, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, dest_addr, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	}
	case Z80_DJNZ:
		dst = cycles(&opts->gen, 8);//T States: 5,3
		dst = sub_ir(dst, 1, opts->regs[Z80_B], SZ_B);
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, CC_Z, dst+2);
		dst = cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = address + inst->immed + 2;
		if (dest_addr < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, dest_addr, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, dest_addr, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	case Z80_CALL: {
		dst = cycles(&opts->gen, 11);//T States: 4,3,4
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 3, opts->gen.scratch1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_word_highfirst);//T States: 3, 3
		if (inst->immed < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, inst->immed, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, inst->immed, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		break;
	}
	case Z80_CALLCC:
		dst = cycles(&opts->gen, 10);//T States: 4,3,3 (false case)
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = cycles(&opts->gen, 1);//Last of the above T states takes an extra cycle in the true case
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 3, opts->gen.scratch1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_word_highfirst);//T States: 3, 3
		if (inst->immed < 0x4000) {
			uint8_t * call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
				opts->deferred = defer_address(opts->deferred, inst->immed, dst + 1);
				//fake address to force large displacement
				call_dst = dst + 256;
			}
			dst = jmp(dst, call_dst);
		} else {
			dst = mov_ir(dst, inst->immed, opts->gen.scratch1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, opts->gen.scratch1);
		}
		*no_call_off = dst - (no_call_off+1);
		break;
	case Z80_RET:
		dst = cycles(&opts->gen, 4);//T States: 4
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, opts->gen.scratch1);
		break;
	case Z80_RETCC: {
		dst = cycles(&opts->gen, 5);//T States: 5
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, opts->gen.scratch1);
		*no_call_off = dst - (no_call_off+1);
		break;
	}
	case Z80_RETI:
		//For some systems, this may need a callback for signalling interrupt routine completion
		dst = cycles(&opts->gen, 8);//T States: 4, 4
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, opts->gen.scratch1);
		break;
	case Z80_RETN:
		dst = cycles(&opts->gen, 8);//T States: 4, 4
		dst = mov_rdisp8r(dst, opts->gen.context_reg, offsetof(z80_context, iff2), opts->gen.scratch2, SZ_B);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		dst = mov_rrdisp8(dst, opts->gen.scratch2, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, opts->gen.scratch1);
		break;
	case Z80_RST: {
		//RST is basically CALL to an address in page 0
		dst = cycles(&opts->gen, 5);//T States: 5
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 1, opts->gen.scratch1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_word_highfirst);//T States: 3, 3
		uint8_t * call_dst = z80_get_native_address(context, inst->immed);
		if (!call_dst) {
			opts->deferred = defer_address(opts->deferred, inst->immed, dst + 1);
			//fake address to force large displacement
			call_dst = dst + 256;
		}
		dst = jmp(dst, call_dst);
		break;
	}
	case Z80_IN:
		dst = cycles(&opts->gen, inst->reg == Z80_A ? 7 : 8);//T States: 4 3/4
		if (inst->addr_mode == Z80_IMMED_INDIRECT) {
			dst = mov_ir(dst, inst->immed, opts->gen.scratch1, SZ_B);
		} else {
			dst = mov_rr(dst, opts->regs[Z80_C], opts->gen.scratch1, SZ_B);
		}
		dst = call(dst, (uint8_t *)z80_io_read);
		translate_z80_reg(inst, &dst_op, dst, opts);
		dst = mov_rr(dst, opts->gen.scratch1, dst_op.base, SZ_B);
		dst = z80_save_reg(dst, inst, opts);
		break;
	/*case Z80_INI:
	case Z80_INIR:
	case Z80_IND:
	case Z80_INDR:*/
	case Z80_OUT:
		dst = cycles(&opts->gen, inst->reg == Z80_A ? 7 : 8);//T States: 4 3/4
		if ((inst->addr_mode & 0x1F) == Z80_IMMED_INDIRECT) {
			dst = mov_ir(dst, inst->immed, opts->gen.scratch2, SZ_B);
		} else {
			dst = mov_rr(dst, opts->regs[Z80_C], opts->gen.scratch2, SZ_B);
		}
		translate_z80_reg(inst, &src_op, dst, opts);
		dst = mov_rr(dst, dst_op.base, opts->gen.scratch1, SZ_B);
		dst = call(dst, (uint8_t *)z80_io_write);
		dst = z80_save_reg(dst, inst, opts);
		break;
	/*case Z80_OUTI:
	case Z80_OTIR:
	case Z80_OUTD:
	case Z80_OTDR:*/
	default: {
		char disbuf[80];
		z80_disasm(inst, disbuf, address);
		fprintf(stderr, "unimplemented instruction: %s at %X\n", disbuf, address);
		FILE * f = fopen("zram.bin", "wb");
		fwrite(context->mem_pointers[0], 1, 8 * 1024, f);
		fclose(f);
		exit(1);
	}
	}
	return dst;
}

uint8_t * z80_get_native_address(z80_context * context, uint32_t address)
{
	native_map_slot *map;
	if (address < 0x4000) {
		address &= 0x1FFF;
		map = context->static_code_map;
	} else if (address >= 0x8000) {
		address &= 0x7FFF;
		map = context->banked_code_map + context->bank_reg;
	} else {
		//dprintf("z80_get_native_address: %X NULL\n", address);
		return NULL;
	}
	if (!map->base || !map->offsets || map->offsets[address] == INVALID_OFFSET || map->offsets[address] == EXTENSION_WORD) {
		//dprintf("z80_get_native_address: %X NULL\n", address);
		return NULL;
	}
	//dprintf("z80_get_native_address: %X %p\n", address, map->base + map->offsets[address]);
	return map->base + map->offsets[address];
}

uint8_t z80_get_native_inst_size(z80_options * opts, uint32_t address)
{
	if (address >= 0x4000) {
		return 0;
	}
	return opts->ram_inst_sizes[address & 0x1FFF];
}

void z80_map_native_address(z80_context * context, uint32_t address, uint8_t * native_address, uint8_t size, uint8_t native_size)
{
	uint32_t orig_address = address;
	native_map_slot *map;
	z80_options * opts = context->options;
	if (address < 0x4000) {
		address &= 0x1FFF;
		map = context->static_code_map;
		opts->ram_inst_sizes[address] = native_size;
		context->ram_code_flags[(address & 0x1C00) >> 10] |= 1 << ((address & 0x380) >> 7);
		context->ram_code_flags[((address + size) & 0x1C00) >> 10] |= 1 << (((address + size) & 0x380) >> 7);
	} else if (address >= 0x8000) {
		address &= 0x7FFF;
		map = context->banked_code_map + context->bank_reg;
		if (!map->offsets) {
			map->offsets = malloc(sizeof(int32_t) * 0x8000);
			memset(map->offsets, 0xFF, sizeof(int32_t) * 0x8000);
		}
	} else {
		return;
	}
	if (!map->base) {
		map->base = native_address;
	}
	map->offsets[address] = native_address - map->base;
	for(--size, orig_address++; size; --size, orig_address++) {
		address = orig_address;
		if (address < 0x4000) {
			address &= 0x1FFF;
			map = context->static_code_map;
		} else if (address >= 0x8000) {
			address &= 0x7FFF;
			map = context->banked_code_map + context->bank_reg;
		} else {
			return;
		}
		if (!map->offsets) {
			map->offsets = malloc(sizeof(int32_t) * 0x8000);
			memset(map->offsets, 0xFF, sizeof(int32_t) * 0x8000);
		}
		map->offsets[address] = EXTENSION_WORD;
	}
}

#define INVALID_INSTRUCTION_START 0xFEEDFEED

uint32_t z80_get_instruction_start(native_map_slot * static_code_map, uint32_t address)
{
	if (!static_code_map->base || address >= 0x4000) {
		return INVALID_INSTRUCTION_START;
	}
	address &= 0x1FFF;
	if (static_code_map->offsets[address] == INVALID_OFFSET) {
		return INVALID_INSTRUCTION_START;
	}
	while (static_code_map->offsets[address] == EXTENSION_WORD) {
		--address;
		address &= 0x1FFF;
	}
	return address;
}

z80_context * z80_handle_code_write(uint32_t address, z80_context * context)
{
	uint32_t inst_start = z80_get_instruction_start(context->static_code_map, address);
	if (inst_start != INVALID_INSTRUCTION_START) {
		uint8_t * dst = z80_get_native_address(context, inst_start);
		dprintf("patching code at %p for Z80 instruction at %X due to write to %X\n", dst, inst_start, address);
		dst = mov_ir(dst, inst_start, opts->gen.scratch1, SZ_D);
		dst = call(dst, (uint8_t *)z80_retrans_stub);
	}
	return context;
}

uint8_t * z80_get_native_address_trans(z80_context * context, uint32_t address)
{
	uint8_t * addr = z80_get_native_address(context, address);
	if (!addr) {
		translate_z80_stream(context, address);
		addr = z80_get_native_address(context, address);
		if (!addr) {
			printf("Failed to translate %X to native code\n", address);
		}
	}
	return addr;
}

void z80_handle_deferred(z80_context * context)
{
	z80_options * opts = context->options;
	process_deferred(&opts->deferred, context, (native_addr_func)z80_get_native_address);
	if (opts->deferred) {
		translate_z80_stream(context, opts->deferred->address);
	}
}

void * z80_retranslate_inst(uint32_t address, z80_context * context, uint8_t * orig_start)
{
	char disbuf[80];
	z80_options * opts = context->options;
	uint8_t orig_size = z80_get_native_inst_size(opts, address);
	uint32_t orig = address;
	address &= 0x1FFF;
	uint8_t * dst = opts->cur_code;
	uint8_t * dst_end = opts->code_end;
	uint8_t *after, *inst = context->mem_pointers[0] + address;
	z80inst instbuf;
	dprintf("Retranslating code at Z80 address %X, native address %p\n", address, orig_start);
	after = z80_decode(inst, &instbuf);
	#ifdef DO_DEBUG_PRINT
	z80_disasm(&instbuf, disbuf, address);
	if (instbuf.op == Z80_NOP) {
		printf("%X\t%s(%d)\n", address, disbuf, instbuf.immed);
	} else {
		printf("%X\t%s\n", address, disbuf);
	}
	#endif
	if (orig_size != ZMAX_NATIVE_SIZE) {
		if (dst_end - dst < ZMAX_NATIVE_SIZE) {
			size_t size = 1024*1024;
			dst = alloc_code(&size);
			opts->code_end = dst_end = dst + size;
			opts->cur_code = dst;
		}
		deferred_addr * orig_deferred = opts->deferred;
		uint8_t * native_end = translate_z80inst(&instbuf, dst, context, address);
		if ((native_end - dst) <= orig_size) {
			uint8_t * native_next = z80_get_native_address(context, address + after-inst);
			if (native_next && ((native_next == orig_start + orig_size) || (orig_size - (native_end - dst)) > 5)) {
				remove_deferred_until(&opts->deferred, orig_deferred);
				native_end = translate_z80inst(&instbuf, orig_start, context, address);
				if (native_next == orig_start + orig_size && (native_next-native_end) < 2) {
					while (native_end < orig_start + orig_size) {
						*(native_end++) = 0x90; //NOP
					}
				} else {
					jmp(native_end, native_next);
				}
				z80_handle_deferred(context);
				return orig_start;
			}
		}
		z80_map_native_address(context, address, dst, after-inst, ZMAX_NATIVE_SIZE);
		opts->cur_code = dst+ZMAX_NATIVE_SIZE;
		jmp(orig_start, dst);
		if (!z80_is_terminal(&instbuf)) {
			jmp(native_end, z80_get_native_address_trans(context, address + after-inst));
		}
		z80_handle_deferred(context);
		return dst;
	} else {
		dst = translate_z80inst(&instbuf, orig_start, context, address);
		if (!z80_is_terminal(&instbuf)) {
			dst = jmp(dst, z80_get_native_address_trans(context, address + after-inst));
		}
		z80_handle_deferred(context);
		return orig_start;
	}
}

void translate_z80_stream(z80_context * context, uint32_t address)
{
	char disbuf[80];
	if (z80_get_native_address(context, address)) {
		return;
	}
	z80_options * opts = context->options;
	uint32_t start_address = address;
	uint8_t * encoded = NULL, *next;
	if (address < 0x4000) {
		encoded = context->mem_pointers[0] + (address & 0x1FFF);
	} else if(address >= 0x8000 && context->mem_pointers[1]) {
		printf("attempt to translate Z80 code from banked area at address %X\n", address);
		exit(1);
		//encoded = context->mem_pointers[1] + (address & 0x7FFF);
	}
	while (encoded != NULL)
	{
		z80inst inst;
		dprintf("translating Z80 code at address %X\n", address);
		do {
			if (opts->code_end-opts->cur_code < ZMAX_NATIVE_SIZE) {
				if (opts->code_end-opts->cur_code < 5) {
					puts("out of code memory, not enough space for jmp to next chunk");
					exit(1);
				}
				size_t size = 1024*1024;
				opts->cur_code = alloc_code(&size);
				opts->code_end = opts->cur_code + size;
				jmp(opts->cur_code, opts->cur_code);
			}
			if (address > 0x4000 && address < 0x8000) {
				opts->cur_code = xor_rr(opts->cur_code, RDI, RDI, SZ_D);
				opts->cur_code = call(opts->cur_code, (uint8_t *)exit);
				break;
			}
			uint8_t * existing = z80_get_native_address(context, address);
			if (existing) {
				opts->cur_code = jmp(opts->cur_code, existing);
				break;
			}
			next = z80_decode(encoded, &inst);
			#ifdef DO_DEBUG_PRINT
			z80_disasm(&inst, disbuf, address);
			if (inst.op == Z80_NOP) {
				printf("%X\t%s(%d)\n", address, disbuf, inst.immed);
			} else {
				printf("%X\t%s\n", address, disbuf);
			}
			#endif
			uint8_t *after = translate_z80inst(&inst, opts->cur_code, context, address);
			z80_map_native_address(context, address, opts->cur_code, next-encoded, after - opts->cur_code);
			opts->cur_code = after;
			address += next-encoded;
			if (address > 0xFFFF) {
				address &= 0xFFFF;

			} else {
				encoded = next;
			}
		} while (!z80_is_terminal(&inst));
		process_deferred(&opts->deferred, context, (native_addr_func)z80_get_native_address);
		if (opts->deferred) {
			address = opts->deferred->address;
			dprintf("defferred address: %X\n", address);
			if (address < 0x4000) {
				encoded = context->mem_pointers[0] + (address & 0x1FFF);
			} else if (address > 0x8000 && context->mem_pointers[1]) {
				encoded = context->mem_pointers[1] + (address  & 0x7FFF);
			} else {
				printf("attempt to translate non-memory address: %X\n", address);
				exit(1);
			}
		} else {
			encoded = NULL;
		}
	}
}

void init_x86_z80_opts(z80_options * options, memmap_chunk * chunks, uint32_t num_chunks)
{
	memset(options, 0, sizeof(*options));

	options->gen.address_size = SZ_W;
	options->gen.address_mask = 0xFFFF;
	options->gen.max_address = 0x10000;
	options->gen.bus_cycles = 3;
	options->gen.mem_ptr_off = offsetof(z80_context, mem_pointers);
	options->gen.ram_flags_off = offsetof(z80_context, ram_code_flags);

	options->flags = 0;
	options->regs[Z80_B] = BH;
	options->regs[Z80_C] = RBX;
	options->regs[Z80_D] = CH;
	options->regs[Z80_E] = RCX;
	options->regs[Z80_H] = AH;
	options->regs[Z80_L] = RAX;
	options->regs[Z80_IXH] = DH;
	options->regs[Z80_IXL] = RDX;
	options->regs[Z80_IYH] = -1;
	options->regs[Z80_IYL] = R8;
	options->regs[Z80_I] = -1;
	options->regs[Z80_R] = -1;
	options->regs[Z80_A] = R10;
	options->regs[Z80_BC] = RBX;
	options->regs[Z80_DE] = RCX;
	options->regs[Z80_HL] = RAX;
	options->regs[Z80_SP] = R9;
	options->regs[Z80_AF] = -1;
	options->regs[Z80_IX] = RDX;
	options->regs[Z80_IY] = R8;

	options->bank_reg = R15;
	options->bank_pointer = R12;

	options->gen.context_reg = RSI;
	options->gen.cycles = RBP;
	options->gen.limit = RDI;
	options->gen.scratch1 = R13;
	options->gen.scratch2 = R14;

	options->gen.native_code_map = malloc(sizeof(native_map_slot));
	memset(options->gen.native_code_map, 0, sizeof(native_map_slot));
	options->gen.deferred = NULL;
	options->gen.ram_inst_sizes = malloc(sizeof(uint8_t) * 0x2000);
	memset(options->ram_inst_sizes, 0, sizeof(uint8_t) * 0x2000);

	code_info *code = &options->gen.code;
	init_code_info(code);

	options->save_context_scratch = code->cur;
	mov_rrdisp(code, options->gen.scratch1, options->gen.context_reg, offsetof(z80_context, scratch1), SZ_W);
	mov_rrdisp(code, options->gen.scratch2, options->gen.context_reg, offsetof(z80_context, scratch2), SZ_W);

	options->gen.save_context = code->cur;
	for (int i = 0; i <= Z80_A; i++)
	{
		int reg;
		uint8_t size;
		if (i < Z80_I) {
			int reg = i /2 + Z80_BC;
			size = SZ_W;

		} else {
			reg = i;
			size = SZ_B;
		}
		if (options->regs[reg] >= 0) {
			mov_rrdisp(code, options->regs[reg], options->gen.context_reg, offsetof(z80_context, regs) + i, size);
		}
	}
	if (options->regs[Z80_SP] >= 0) {
		mov_rrdisp(code, options->regs[Z80_SP], options->gen.context_reg, offsetof(z80_context, sp), SZ_W);
	}
	mov_rrdisp(code, options->gen.limit, options->gen.context_reg, offsetof(z80_context, target_cycle), SZ_D);
	mov_rrdisp(code, options->gen.cycles, options->gen.context_reg, offsetof(z80_context, current_cycle), SZ_D);
	mov_rrdisp(code, options->bank_reg, options->gen.context_reg, offsetof(z80_context, bank_reg), SZ_W);
	mov_rrdisp(code, options->bank_pointer, options->gen.context_reg, offsetof(z80_context, mem_pointers) + sizeof(uint8_t *) * 1, SZ_PTR);

	options->load_context_scratch = code->cur;
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, scratch1), options->gen.scratch1, SZ_W);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, scratch2), options->gen.scratch2, SZ_W);
	options->gen.load_context = code->cur;
	for (int i = 0; i <= Z80_A; i++)
	{
		int reg;
		uint8_t size;
		if (i < Z80_I) {
			int reg = i /2 + Z80_BC;
			size = SZ_W;

		} else {
			reg = i;
			size = SZ_B;
		}
		if (options->regs[reg] >= 0) {
			mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, regs) + i, options->regs[reg], size);
		}
	}
	if (options->regs[Z80_SP] >= 0) {
		mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, sp), options->regs[Z80_SP], SZ_W);
	}
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, target_cycle), options->gen.limit, SZ_D);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, current_cycle), options->gen.cycles, SZ_D);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, bank_reg), options->bank_reg, SZ_W);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, mem_pointers) + sizeof(uint8_t *) * 1, options->bank_pointer, SZ_PTR);

	options->gen.handle_cycle_limit = code->cur;
	cmp_rdispr(code, options->gen.context_reg, offsetof(z80_context, sync_cycle), options->gen.cycles, SZ_D);
	code_ptr no_sync = code->cur+1;
	jcc(code, CC_B, no_sync);
	mov_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, pc), SZ_W);
	call(code, options->save_context_scratch);
	pop_r(code, RAX); //return address in read/write func
	pop_r(code, RBX); //return address in translated code
	sub_ir(code, 5, RAX, SZ_PTR); //adjust return address to point to the call that got us here
	mov_rrdisp(code, RBX, options->gen.context_reg, offsetof(z80_context, extra_pc), SZ_PTR);
	mov_rrind(code, RAX, options->gen.context_reg, SZ_PTR);
	//restore callee saved registers
	pop_r(code, R15)
	pop_r(code, R14)
	pop_r(code, R13)
	pop_r(code, R12)
	pop_r(code, RBP)
	pop_r(code, RBX)
	*no_sync = code->cur - no_sync;
	//return to caller of z80_run
	retn(code);

	options->gen.read_8 = gen_mem_fun(&options->gen, chunks, num_chunks, READ_8, NULL);
	options->gen.write_8 = gen_mem_fun(&options->gen, chunks, num_chunks, WRITE_8, &options->write_8_noinc);

	options->gen.handle_cycle_limit_int = code->cur;
	cmp_rdispr(code, options->gen.context_reg, offsetof(z80_context, int_cycle), options->gen.cycles, SZ_D);
	code_ptr skip_int = code->cur+1;
	jcc(code, CC_B, skip_int);
	//set limit to the cycle limit
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, sync_cycle), options->gen.limit, SZ_D);
	//disable interrupts
	move_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
	move_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
	cycles(&options->gen, 7);
	//save return address (in scratch1) to Z80 stack
	sub_ir(code, 2, options->regs[Z80_SP], SZ_W);
	mov_rr(code, options->regs[Z80_SP], options->gen.scratch2, SZ_W);
	//we need to do check_cycles and cycles outside of the write_8 call
	//so that the stack has the correct depth if we need to return to C
	//for a synchronization
	check_cycles(&options->gen);
	cycles(&options->gen, 3);
	//save word to write before call to write_8_noinc
	push_r(code, options->gen.scratch1);
	call(code, options->write_8_noinc);
	//restore word to write
	pop_r(code, options->gen.scratch1);
	//write high byte to SP+1
	mov_rr(code, options->regs[Z80_SP], options->gen.scratch2, SZ_W);
	add_ir(code, 1, options->gen.scratch2, SZ_W);
	shr_ir(code, 8, options->gen.scratch1, SZ_W);
	check_cycles(&options->gen);
	cycles(&options->gen, 3);
	call(code, options->write_8_noinc);
	//dispose of return address as we'll be jumping somewhere else
	pop_r(options->gen.scratch2);
	//TODO: Support interrupt mode 0 and 2
	mov_ir(code, 0x38, options->gen.scratch1, SZ_W);
	call(code, (code_ptr)z80_native_addr);
	jmp_r(code, options->gen.scratch1);
}

void init_z80_context(z80_context * context, z80_options * options)
{
	memset(context, 0, sizeof(*context));
	context->static_code_map = malloc(sizeof(*context->static_code_map));
	context->static_code_map->base = NULL;
	context->static_code_map->offsets = malloc(sizeof(int32_t) * 0x2000);
	memset(context->static_code_map->offsets, 0xFF, sizeof(int32_t) * 0x2000);
	context->banked_code_map = malloc(sizeof(native_map_slot) * (1 << 9));
	memset(context->banked_code_map, 0, sizeof(native_map_slot) * (1 << 9));
	context->options = options;
}

void z80_reset(z80_context * context)
{
	context->im = 0;
	context->iff1 = context->iff2 = 0;
	context->native_pc = z80_get_native_address_trans(context, 0);
	context->extra_pc = NULL;
}

void zinsert_breakpoint(z80_context * context, uint16_t address, uint8_t * bp_handler)
{
	static uint8_t * bp_stub = NULL;
	uint8_t * native = z80_get_native_address_trans(context, address);
	uint8_t * start_native = native;
	native = mov_ir(native, address, opts->gen.scratch1, SZ_W);
	if (!bp_stub) {
		z80_options * opts = context->options;
		uint8_t * dst = opts->cur_code;
		uint8_t * dst_end = opts->code_end;
		if (dst_end - dst < 128) {
			size_t size = 1024*1024;
			dst = alloc_code(&size);
			opts->code_end = dst_end = dst + size;
		}
		bp_stub = dst;
		native = call(native, bp_stub);

		//Calculate length of prologue
		dst = z80_check_cycles_int(dst, address);
		int check_int_size = dst-bp_stub;
		dst = bp_stub;

		//Save context and call breakpoint handler
		dst = call(dst, (uint8_t *)z80_save_context);
		dst = push_r(dst, opts->gen.scratch1);
		dst = mov_rr(dst, opts->gen.context_reg, RDI, SZ_Q);
		dst = mov_rr(dst, opts->gen.scratch1, RSI, SZ_W);
		dst = call(dst, bp_handler);
		dst = mov_rr(dst, RAX, opts->gen.context_reg, SZ_Q);
		//Restore context
		dst = call(dst, (uint8_t *)z80_load_context);
		dst = pop_r(dst, opts->gen.scratch1);
		//do prologue stuff
		dst = cmp_rr(dst, opts->gen.cycles, opts->gen.limit, SZ_D);
		uint8_t * jmp_off = dst+1;
		dst = jcc(dst, CC_NC, dst + 7);
		dst = pop_r(dst, opts->gen.scratch1);
		dst = add_ir(dst, check_int_size - (native-start_native), opts->gen.scratch1, SZ_Q);
		dst = push_r(dst, opts->gen.scratch1);
		dst = jmp(dst, (uint8_t *)z80_handle_cycle_limit_int);
		*jmp_off = dst - (jmp_off+1);
		//jump back to body of translated instruction
		dst = pop_r(dst, opts->gen.scratch1);
		dst = add_ir(dst, check_int_size - (native-start_native), opts->gen.scratch1, SZ_Q);
		dst = jmp_r(dst, opts->gen.scratch1);
		opts->cur_code = dst;
	} else {
		native = call(native, bp_stub);
	}
}

void zremove_breakpoint(z80_context * context, uint16_t address)
{
	uint8_t * native = z80_get_native_address(context, address);
	z80_check_cycles_int(native, address);
}


