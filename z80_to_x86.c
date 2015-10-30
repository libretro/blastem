/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "z80inst.h"
#include "z80_to_x86.h"
#include "gen_x86.h"
#include "mem.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define MODE_UNUSED (MODE_IMMED-1)

//#define DO_DEBUG_PRINT

#ifdef DO_DEBUG_PRINT
#define dprintf printf
#else
#define dprintf
#endif

uint32_t zbreakpoint_patch(z80_context * context, uint16_t address, code_ptr dst);
void z80_handle_deferred(z80_context * context);

uint8_t z80_size(z80inst * inst)
{
	uint8_t reg = (inst->reg & 0x1F);
	if (reg != Z80_UNUSED && reg != Z80_USE_IMMED) {
		return reg < Z80_BC ? SZ_B : SZ_W;
	}
	//TODO: Handle any necessary special cases
	return SZ_B;
}

uint8_t zf_off(uint8_t flag)
{
	return offsetof(z80_context, flags) + flag;
}

uint8_t zaf_off(uint8_t flag)
{
	return offsetof(z80_context, alt_flags) + flag;
}

uint8_t zr_off(uint8_t reg)
{
	if (reg > Z80_A) {
		reg = z80_low_reg(reg);
	}
	return offsetof(z80_context, regs) + reg;
}

uint8_t zar_off(uint8_t reg)
{
	if (reg > Z80_A) {
		reg = z80_low_reg(reg);
	}
	return offsetof(z80_context, alt_regs) + reg;
}

void zreg_to_native(z80_options *opts, uint8_t reg, uint8_t native_reg)
{
	if (opts->regs[reg] >= 0) {
		mov_rr(&opts->gen.code, opts->regs[reg], native_reg, reg > Z80_A ? SZ_W : SZ_B);
	} else {
		mov_rdispr(&opts->gen.code, opts->gen.context_reg, zr_off(reg), native_reg, reg > Z80_A ? SZ_W : SZ_B);
	}
}

void native_to_zreg(z80_options *opts, uint8_t native_reg, uint8_t reg)
{
	if (opts->regs[reg] >= 0) {
		mov_rr(&opts->gen.code, native_reg, opts->regs[reg], reg > Z80_A ? SZ_W : SZ_B);
	} else {
		mov_rrdisp(&opts->gen.code, native_reg, opts->gen.context_reg, zr_off(reg), reg > Z80_A ? SZ_W : SZ_B);
	}
}

void translate_z80_reg(z80inst * inst, host_ea * ea, z80_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->reg == Z80_USE_IMMED) {
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
	} else if ((inst->reg & 0x1F) == Z80_UNUSED) {
		ea->mode = MODE_UNUSED;
	} else {
		ea->mode = MODE_REG_DIRECT;
		if (inst->reg == Z80_IYH && opts->regs[Z80_IYL] >= 0) {
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
			ea->disp = zr_off(inst->reg);
		}
	}
}

void z80_save_reg(z80inst * inst, z80_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->reg == Z80_USE_IMMED || inst->reg == Z80_UNUSED) {
		return;
	}
	if (inst->reg == Z80_IYH && opts->regs[Z80_IYL] >= 0) {
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

void translate_z80_ea(z80inst * inst, host_ea * ea, z80_options * opts, uint8_t read, uint8_t modify)
{
	code_info *code = &opts->gen.code;
	uint8_t size, areg;
	int8_t reg;
	ea->mode = MODE_REG_DIRECT;
	areg = read ? opts->gen.scratch1 : opts->gen.scratch2;
	switch(inst->addr_mode & 0x1F)
	{
	case Z80_REG:
		if (inst->ea_reg == Z80_IYH && opts->regs[Z80_IYL] >= 0) {
			if (inst->reg == Z80_IYL) {
				mov_rr(code, opts->regs[Z80_IY], opts->gen.scratch1, SZ_W);
				ror_ir(code, 8, opts->gen.scratch1, SZ_W);
				ea->base = opts->gen.scratch1;
			} else {
				ea->base = opts->regs[Z80_IYL];
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			}
		} else if(opts->regs[inst->ea_reg] >= 0) {
			ea->base = opts->regs[inst->ea_reg];
			if (ea->base >= AH && ea->base <= BH && inst->reg != Z80_UNUSED && inst->reg != Z80_USE_IMMED) {
				uint8_t other_reg = opts->regs[inst->reg];
#ifdef X86_64
				if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
					//we can't mix an *H reg with a register that requires the REX prefix
					ea->base = opts->regs[z80_low_reg(inst->ea_reg)];
					ror_ir(code, 8, ea->base, SZ_W);
				}
#endif
			}
		} else {
			ea->mode = MODE_REG_DISPLACE8;
			ea->base = opts->gen.context_reg;
			ea->disp = zr_off(inst->ea_reg);
		}
		break;
	case Z80_REG_INDIRECT:
		zreg_to_native(opts, inst->ea_reg, areg);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				//push_r(code, opts->gen.scratch1);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(z80_context, scratch1), SZ_W);
			}
			if (size == SZ_B) {
				call(code, opts->read_8);
			} else {
				call(code, opts->read_16);
			}
			if (modify) {
				//pop_r(code, opts->gen.scratch2);
				mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, scratch1), opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_IMMED:
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
		break;
	case Z80_IMMED_INDIRECT:
		mov_ir(code, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			/*if (modify) {
				push_r(code, opts->gen.scratch1);
			}*/
			if (size == SZ_B) {
				call(code, opts->read_8);
			} else {
				call(code, opts->read_16);
			}
			if (modify) {
				//pop_r(code, opts->gen.scratch2);
				mov_ir(code, inst->immed, opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_IX_DISPLACE:
	case Z80_IY_DISPLACE:
		zreg_to_native(opts, (inst->addr_mode & 0x1F) == Z80_IX_DISPLACE ? Z80_IX : Z80_IY, areg);
		add_ir(code, inst->ea_reg & 0x80 ? inst->ea_reg - 256 : inst->ea_reg, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				//push_r(code, opts->gen.scratch1);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(z80_context, scratch1), SZ_W);
			}
			if (size == SZ_B) {
				call(code, opts->read_8);
			} else {
				call(code, opts->read_16);
			}
			if (modify) {
				//pop_r(code, opts->gen.scratch2);
				mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, scratch1), opts->gen.scratch2, SZ_W);
			}
		}
		ea->base = opts->gen.scratch1;
		break;
	case Z80_UNUSED:
		ea->mode = MODE_UNUSED;
		break;
	default:
		fatal_error("Unrecognized Z80 addressing mode %d\n", inst->addr_mode & 0x1F);
	}
}

void z80_save_ea(code_info *code, z80inst * inst, z80_options * opts)
{
	if ((inst->addr_mode & 0x1F) == Z80_REG) {
		if (inst->ea_reg == Z80_IYH && opts->regs[Z80_IYL] >= 0) {
			if (inst->reg == Z80_IYL) {
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
				mov_rr(code, opts->gen.scratch1, opts->regs[Z80_IYL], SZ_B);
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			} else {
				ror_ir(code, 8, opts->regs[Z80_IY], SZ_W);
			}
		} else if (inst->reg != Z80_UNUSED && inst->reg != Z80_USE_IMMED && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			uint8_t other_reg = opts->regs[inst->reg];
#ifdef X86_64
			if (other_reg >= R8 || (other_reg >= RSP && other_reg <= RDI)) {
				//we can't mix an *H reg with a register that requires the REX prefix
				ror_ir(code, 8, opts->regs[z80_low_reg(inst->ea_reg)], SZ_W);
			}
#endif
		}
	}
}

void z80_save_result(z80_options *opts, z80inst * inst)
{
	switch(inst->addr_mode & 0x1f)
	{
	case Z80_REG_INDIRECT:
	case Z80_IMMED_INDIRECT:
	case Z80_IX_DISPLACE:
	case Z80_IY_DISPLACE:
		if (z80_size(inst) == SZ_B) {
			call(&opts->gen.code, opts->write_8);
		} else {
			call(&opts->gen.code, opts->write_16_lowfirst);
		}
	}
}

enum {
	DONT_READ=0,
	READ
};

enum {
	DONT_MODIFY=0,
	MODIFY
};

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

void translate_z80inst(z80inst * inst, z80_context * context, uint16_t address, uint8_t interp)
{
	uint32_t num_cycles;
	host_ea src_op, dst_op;
	uint8_t size;
	z80_options *opts = context->options;
	uint8_t * start = opts->gen.code.cur;
	code_info *code = &opts->gen.code;
	if (!interp) {
		check_cycles_int(&opts->gen, address);
		if (context->breakpoint_flags[address / 8] & (1 << (address % 8))) {
			zbreakpoint_patch(context, address, start);
		}
#ifdef Z80_LOG_ADDRESS
		log_address(&opts->gen, address, "Z80: %X @ %d\n");
#endif
	}
	switch(inst->op)
	{
	case Z80_LD:
		size = z80_size(inst);
		switch (inst->addr_mode & 0x1F)
		{
		case Z80_REG:
		case Z80_REG_INDIRECT:
 			num_cycles = size == SZ_B ? 4 : 6;
			if (inst->ea_reg == Z80_IX || inst->ea_reg == Z80_IY) {
				num_cycles += 4;
			}
			if (inst->reg == Z80_I || inst->ea_reg == Z80_I) {
				num_cycles += 5;
			}
			break;
		case Z80_IMMED:
			num_cycles = size == SZ_B ? 7 : 10;
			break;
		case Z80_IMMED_INDIRECT:
			num_cycles = 10;
			break;
		case Z80_IX_DISPLACE:
		case Z80_IY_DISPLACE:
			num_cycles = 16;
			break;
		}
		if ((inst->reg >= Z80_IXL && inst->reg <= Z80_IYH) || inst->reg == Z80_IX || inst->reg == Z80_IY) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode & Z80_DIR) {
			translate_z80_ea(inst, &dst_op, opts, DONT_READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts);
		} else {
			translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			if(dst_op.mode == MODE_REG_DISPLACE8) {
				mov_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
			} else {
				mov_rr(code, src_op.base, dst_op.base, size);
			}
		} else if(src_op.mode == MODE_IMMED) {
			if(dst_op.mode == MODE_REG_DISPLACE8) {
				mov_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, size);
			} else {
				mov_ir(code, src_op.disp, dst_op.base, size);
			}
		} else {
			if(dst_op.mode == MODE_REG_DISPLACE8) {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, size);
				mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, size);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, dst_op.base, size);
			}
		}
		if (inst->ea_reg == Z80_I && inst->addr_mode == Z80_REG) {
			//ld a, i sets some flags
			cmp_ir(code, 0, dst_op.base, SZ_B);
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
			mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_H), SZ_B);;
			mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);;
			mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, iff2), opts->gen.scratch1, SZ_B);
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		if (inst->addr_mode & Z80_DIR) {
			z80_save_result(opts, inst);
		}
		break;
	case Z80_PUSH:
		cycles(&opts->gen, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 9 : 5);
		sub_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {
			zreg_to_native(opts, Z80_A, opts->gen.scratch1);
			shl_ir(code, 8, opts->gen.scratch1, SZ_W);
			mov_rdispr(code, opts->gen.context_reg, zf_off(ZF_S), opts->gen.scratch1, SZ_B);
			shl_ir(code, 1, opts->gen.scratch1, SZ_B);
			or_rdispr(code, opts->gen.context_reg, zf_off(ZF_Z), opts->gen.scratch1, SZ_B);
			shl_ir(code, 2, opts->gen.scratch1, SZ_B);
			or_rdispr(code, opts->gen.context_reg, zf_off(ZF_H), opts->gen.scratch1, SZ_B);
			shl_ir(code, 2, opts->gen.scratch1, SZ_B);
			or_rdispr(code, opts->gen.context_reg, zf_off(ZF_PV), opts->gen.scratch1, SZ_B);
			shl_ir(code, 1, opts->gen.scratch1, SZ_B);
			or_rdispr(code, opts->gen.context_reg, zf_off(ZF_N), opts->gen.scratch1, SZ_B);
			shl_ir(code, 1, opts->gen.scratch1, SZ_B);
			or_rdispr(code, opts->gen.context_reg, zf_off(ZF_C), opts->gen.scratch1, SZ_B);
		} else {
			zreg_to_native(opts, inst->reg, opts->gen.scratch1);
		}
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		call(code, opts->write_16_highfirst);
		//no call to save_z80_reg needed since there's no chance we'll use the only
		//the upper half of a register pair
		break;
	case Z80_POP:
		cycles(&opts->gen, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 8 : 4);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		call(code, opts->read_16);
		add_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {

			bt_ir(code, 0, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
			bt_ir(code, 1, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_N));
			bt_ir(code, 2, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_PV));
			bt_ir(code, 4, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_H));
			bt_ir(code, 6, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_Z));
			bt_ir(code, 7, opts->gen.scratch1, SZ_W);
			setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_S));
			shr_ir(code, 8, opts->gen.scratch1, SZ_W);
			native_to_zreg(opts, opts->gen.scratch1, Z80_A);
		} else {
			native_to_zreg(opts, opts->gen.scratch1, inst->reg);
		}
		//no call to save_z80_reg needed since there's no chance we'll use the only
		//the upper half of a register pair
		break;
	case Z80_EX:
		if (inst->addr_mode == Z80_REG || inst->reg == Z80_HL) {
			num_cycles = 4;
		} else {
			num_cycles = 8;
		}
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode == Z80_REG) {
			if(inst->reg == Z80_AF) {
				zreg_to_native(opts, Z80_A, opts->gen.scratch1);
				mov_rdispr(code, opts->gen.context_reg, zar_off(Z80_A), opts->gen.scratch2, SZ_B);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_A), SZ_B);
				native_to_zreg(opts, opts->gen.scratch2, Z80_A);

				//Flags are currently word aligned, so we can move
				//them efficiently a word at a time
				for (int f = ZF_C; f < ZF_NUM; f+=2) {
					mov_rdispr(code, opts->gen.context_reg, zf_off(f), opts->gen.scratch1, SZ_W);
					mov_rdispr(code, opts->gen.context_reg, zaf_off(f), opts->gen.scratch2, SZ_W);
					mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zaf_off(f), SZ_W);
					mov_rrdisp(code, opts->gen.scratch2, opts->gen.context_reg, zf_off(f), SZ_W);
				}
			} else {
				if (opts->regs[Z80_DE] >= 0 && opts->regs[Z80_HL] >= 0) {
					xchg_rr(code, opts->regs[Z80_DE], opts->regs[Z80_HL], SZ_W);
				} else {
					zreg_to_native(opts, Z80_DE, opts->gen.scratch1);
					zreg_to_native(opts, Z80_HL, opts->gen.scratch2);
					native_to_zreg(opts, opts->gen.scratch1, Z80_HL);
					native_to_zreg(opts, opts->gen.scratch2, Z80_DE);
				}
			}
		} else {
			mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
			call(code, opts->read_8);
			if (opts->regs[inst->reg] >= 0) {
				xchg_rr(code, opts->regs[inst->reg], opts->gen.scratch1, SZ_B);
			} else {
				zreg_to_native(opts, inst->reg, opts->gen.scratch2);
				xchg_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_B);
				native_to_zreg(opts, opts->gen.scratch2, inst->reg);
			}
			mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
			call(code, opts->write_8);
			cycles(&opts->gen, 1);
			uint8_t high_reg = z80_high_reg(inst->reg);
			mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
			add_ir(code, 1, opts->gen.scratch1, SZ_W);
			call(code, opts->read_8);
			if (opts->regs[inst->reg] >= 0) {
				//even though some of the upper halves can be used directly
				//the limitations on mixing *H regs with the REX prefix
				//prevent us from taking advantage of it
				uint8_t use_reg = opts->regs[inst->reg];
				ror_ir(code, 8, use_reg, SZ_W);
				xchg_rr(code, use_reg, opts->gen.scratch1, SZ_B);
				//restore reg to normal rotation
				ror_ir(code, 8, use_reg, SZ_W);
			} else {
				zreg_to_native(opts, high_reg, opts->gen.scratch2);
				xchg_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_B);
				native_to_zreg(opts, opts->gen.scratch2, high_reg);
			}
			mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
			add_ir(code, 1, opts->gen.scratch2, SZ_W);
			call(code, opts->write_8);
			cycles(&opts->gen, 2);
		}
		break;
	case Z80_EXX:
		cycles(&opts->gen, 4);
		zreg_to_native(opts, Z80_BC, opts->gen.scratch1);
		mov_rdispr(code, opts->gen.context_reg, zar_off(Z80_BC), opts->gen.scratch2, SZ_W);
		mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_BC), SZ_W);
		native_to_zreg(opts, opts->gen.scratch2, Z80_BC);

		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		mov_rdispr(code, opts->gen.context_reg, zar_off(Z80_HL), opts->gen.scratch2, SZ_W);
		mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_HL), SZ_W);
		native_to_zreg(opts, opts->gen.scratch2, Z80_HL);

		zreg_to_native(opts, Z80_DE, opts->gen.scratch1);
		mov_rdispr(code, opts->gen.context_reg, zar_off(Z80_DE), opts->gen.scratch2, SZ_W);
		mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, zar_off(Z80_DE), SZ_W);
		native_to_zreg(opts, opts->gen.scratch2, Z80_DE);
		break;
	case Z80_LDI: {
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		zreg_to_native(opts, Z80_DE, opts->gen.scratch2);
		call(code, opts->write_8);
		cycles(&opts->gen, 2);
		if (opts->regs[Z80_DE] >= 0) {
			add_ir(code, 1, opts->regs[Z80_DE], SZ_W);
		} else {
			add_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_DE), SZ_W);
		}
		if (opts->regs[Z80_HL] >= 0) {
			add_ir(code, 1, opts->regs[Z80_HL], SZ_W);
		} else {
			add_irdisp(code, 1, opts->gen.context_reg,  zr_off(Z80_HL), SZ_W);
		}
		if (opts->regs[Z80_BC] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_BC], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg,  zr_off(Z80_BC), SZ_W);
		}
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_H), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_NZ, opts->gen.context_reg, zf_off(ZF_PV));
		break;
	}
	case Z80_LDIR: {
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		zreg_to_native(opts, Z80_DE, opts->gen.scratch2);
		call(code, opts->write_8);
		if (opts->regs[Z80_DE] >= 0) {
			add_ir(code, 1, opts->regs[Z80_DE], SZ_W);
		} else {
			add_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_DE), SZ_W);
		}
		if (opts->regs[Z80_HL] >= 0) {
			add_ir(code, 1, opts->regs[Z80_HL], SZ_W);
		} else {
			add_irdisp(code, 1, opts->gen.context_reg,  zr_off(Z80_HL), SZ_W);
		}
		if (opts->regs[Z80_BC] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_BC], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg,  zr_off(Z80_BC), SZ_W);
		}
		uint8_t * cont = code->cur+1;
		jcc(code, CC_Z, code->cur+2);
		cycles(&opts->gen, 7);
		//TODO: Figure out what the flag state should be here
		//TODO: Figure out whether an interrupt can interrupt this
		jmp(code, start);
		*cont = code->cur - (cont + 1);
		cycles(&opts->gen, 2);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_H), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
		break;
	}
	case Z80_LDD: {
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		zreg_to_native(opts, Z80_DE, opts->gen.scratch2);
		call(code, opts->write_8);
		cycles(&opts->gen, 2);
		if (opts->regs[Z80_DE] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_DE], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_DE), SZ_W);
		}
		if (opts->regs[Z80_HL] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_HL], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_HL), SZ_W);
		}
		if (opts->regs[Z80_BC] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_BC], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_BC), SZ_W);
		}
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_H), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_NZ, opts->gen.context_reg, zf_off(ZF_PV));
		break;
	}
	case Z80_LDDR: {
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		zreg_to_native(opts, Z80_DE, opts->gen.scratch2);
		call(code, opts->write_8);
		if (opts->regs[Z80_DE] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_DE], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_DE), SZ_W);
		}
		if (opts->regs[Z80_HL] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_HL], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_HL), SZ_W);
		}
		if (opts->regs[Z80_BC] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_BC], SZ_W);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_BC), SZ_W);
		}
		uint8_t * cont = code->cur+1;
		jcc(code, CC_Z, code->cur+2);
		cycles(&opts->gen, 7);
		//TODO: Figure out what the flag state should be here
		jmp(code, start);
		*cont = code->cur - (cont + 1);
		cycles(&opts->gen, 2);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_H), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
		break;
	}
	/*case Z80_CPI:
	case Z80_CPIR:
	case Z80_CPD:
	case Z80_CPDR:
		break;*/
	case Z80_ADD:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				add_rr(code, src_op.base, dst_op.base, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				add_ir(code, src_op.disp, dst_op.base, z80_size(inst));
			} else {
				add_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				add_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				add_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, z80_size(inst));
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, z80_size(inst));
				add_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, z80_size(inst));
			}
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_ADC:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		bt_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				adc_rr(code, src_op.base, dst_op.base, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				adc_ir(code, src_op.disp, dst_op.base, z80_size(inst));
			} else {
				adc_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				adc_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				adc_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, z80_size(inst));
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, z80_size(inst));
				adc_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, z80_size(inst));
			}
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_SUB:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				sub_rr(code, src_op.base, dst_op.base, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				sub_ir(code, src_op.disp, dst_op.base, z80_size(inst));
			} else {
				sub_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				sub_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				sub_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, z80_size(inst));
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, z80_size(inst));
				sub_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, z80_size(inst));
			}
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_SBC:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		bt_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				sbb_rr(code, src_op.base, dst_op.base, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				sbb_ir(code, src_op.disp, dst_op.base, z80_size(inst));
			} else {
				sbb_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				sbb_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, z80_size(inst));
			} else if (src_op.mode == MODE_IMMED) {
				sbb_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, z80_size(inst));
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, z80_size(inst));
				sbb_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, z80_size(inst));
			}
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_AND:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			and_rr(code, src_op.base, dst_op.base, z80_size(inst));
		} else if (src_op.mode == MODE_IMMED) {
			and_ir(code, src_op.disp, dst_op.base, z80_size(inst));
		} else {
			and_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_OR:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			or_rr(code, src_op.base, dst_op.base, z80_size(inst));
		} else if (src_op.mode == MODE_IMMED) {
			or_ir(code, src_op.disp, dst_op.base, z80_size(inst));
		} else {
			or_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_XOR:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			xor_rr(code, src_op.base, dst_op.base, z80_size(inst));
		} else if (src_op.mode == MODE_IMMED) {
			xor_ir(code, src_op.disp, dst_op.base, z80_size(inst));
		} else {
			xor_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_CP:
		num_cycles = 4;
		if (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			num_cycles += 3;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			cmp_rr(code, src_op.base, dst_op.base, z80_size(inst));
		} else if (src_op.mode == MODE_IMMED) {
			cmp_ir(code, src_op.disp, dst_op.base, z80_size(inst));
		} else {
			cmp_rdispr(code, src_op.base, src_op.disp, dst_op.base, z80_size(inst));
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		break;
	case Z80_INC:
		num_cycles = 4;
		if (inst->reg == Z80_IX || inst->reg == Z80_IY) {
			num_cycles += 6;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 2;
		} else if(inst->reg == Z80_IXH || inst->reg == Z80_IXL || inst->reg == Z80_IYH || inst->reg == Z80_IYL || inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		if (dst_op.mode == MODE_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			add_ir(code, 1, dst_op.base, z80_size(inst));
		} else {
			add_irdisp(code, 1, dst_op.base, dst_op.disp, z80_size(inst));
		}
		if (z80_size(inst) == SZ_B) {
			mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		z80_save_result(opts, inst);
		break;
	case Z80_DEC:
		num_cycles = 4;
		if (inst->reg == Z80_IX || inst->reg == Z80_IY) {
			num_cycles += 6;
		} else if(z80_size(inst) == SZ_W) {
			num_cycles += 2;
		} else if(inst->reg == Z80_IXH || inst->reg == Z80_IXL || inst->reg == Z80_IYH || inst->reg == Z80_IYL || inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		translate_z80_reg(inst, &dst_op, opts);
		if (dst_op.mode == MODE_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			sub_ir(code, 1, dst_op.base, z80_size(inst));
		} else {
			sub_irdisp(code, 1, dst_op.base, dst_op.disp, z80_size(inst));
		}

		if (z80_size(inst) == SZ_B) {
			mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		z80_save_reg(inst, opts);
		z80_save_ea(code, inst, opts);
		z80_save_result(opts, inst);
		break;
	//case Z80_DAA:
	case Z80_CPL:
		cycles(&opts->gen, 4);
		not_r(code, opts->regs[Z80_A], SZ_B);
		//TODO: Implement half-carry flag
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		break;
	case Z80_NEG:
		cycles(&opts->gen, 8);
		neg_r(code, opts->regs[Z80_A], SZ_B);
		//TODO: Implement half-carry flag
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		setcc_rdisp(code, CC_O, opts->gen.context_reg, zf_off(ZF_PV));
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		break;
	case Z80_CCF:
		cycles(&opts->gen, 4);
		xor_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_SCF:
		cycles(&opts->gen, 4);
		mov_irdisp(code, 1, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_NOP:
		if (inst->immed == 42) {
			call(code, opts->gen.save_context);
			call_args(code, (code_ptr)z80_print_regs_exit, 1, opts->gen.context_reg);
		} else {
			cycles(&opts->gen, 4 * inst->immed);
		}
		break;
	case Z80_HALT: {
		code_ptr loop_top = code->cur;
		//this isn't terribly efficient, but it's good enough for now
		cycles(&opts->gen, 4);
		check_cycles_int(&opts->gen, address);
		jmp(code, loop_top);
		break;
	}
	case Z80_DI:
		cycles(&opts->gen, 4);
		mov_irdisp(code, 0, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		mov_irdisp(code, 0, opts->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
		mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, sync_cycle), opts->gen.limit, SZ_D);
		mov_irdisp(code, 0xFFFFFFFF, opts->gen.context_reg, offsetof(z80_context, int_cycle), SZ_D);
		break;
	case Z80_EI:
		cycles(&opts->gen, 4);
		mov_rrdisp(code, opts->gen.cycles, opts->gen.context_reg, offsetof(z80_context, int_enable_cycle), SZ_D);
		mov_irdisp(code, 1, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		mov_irdisp(code, 1, opts->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
		//interrupt enable has a one-instruction latency, minimum instruction duration is 4 cycles
		add_irdisp(code, 4*opts->gen.clock_divider, opts->gen.context_reg, offsetof(z80_context, int_enable_cycle), SZ_D);
		call(code, opts->do_sync);
		break;
	case Z80_IM:
		cycles(&opts->gen, 8);
		mov_irdisp(code, inst->immed, opts->gen.context_reg, offsetof(z80_context, im), SZ_B);
		break;
	case Z80_RLC:
		num_cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			rol_ir(code, 1, dst_op.base, SZ_B);
		} else {
			rol_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (inst->immed) {
			//rlca does not set these flags
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, SZ_B);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
			}
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_RL:
		num_cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		bt_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (dst_op.mode == MODE_REG_DIRECT) {
			rcl_ir(code, 1, dst_op.base, SZ_B);
		} else {
			rcl_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (inst->immed) {
			//rla does not set these flags
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, SZ_B);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
			}
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_RRC:
		num_cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			ror_ir(code, 1, dst_op.base, SZ_B);
		} else {
			ror_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (inst->immed) {
			//rrca does not set these flags
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, SZ_B);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
			}
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_RR:
		num_cycles = inst->immed == 0 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		bt_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
		if (dst_op.mode == MODE_REG_DIRECT) {
			rcr_ir(code, 1, dst_op.base, SZ_B);
		} else {
			rcr_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (inst->immed) {
			//rra does not set these flags
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, SZ_B);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
			}
			setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
			setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		}
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_SLA:
	case Z80_SLL:
		num_cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			shl_ir(code, 1, dst_op.base, SZ_B);
		} else {
			shl_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		if (inst->op == Z80_SLL) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				or_ir(code, 1, dst_op.base, SZ_B);
			} else {
				or_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
			}
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, dst_op.base, SZ_B);
		} else {
			cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_SRA:
		num_cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			sar_ir(code, 1, dst_op.base, SZ_B);
		} else {
			sar_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, dst_op.base, SZ_B);
		} else {
			cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_SRL:
		num_cycles = inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8;
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_UNUSED) {
			translate_z80_ea(inst, &dst_op, opts, READ, MODIFY);
			translate_z80_reg(inst, &src_op, opts); //For IX/IY variants that also write to a register
			cycles(&opts->gen, 1);
		} else {
			src_op.mode = MODE_UNUSED;
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			shr_ir(code, 1, dst_op.base, SZ_B);
		} else {
			shr_irdisp(code, 1, dst_op.base, dst_op.disp, SZ_B);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, src_op.base, SZ_B);
		} else if(src_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_op.base, src_op.base, src_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_C, opts->gen.context_reg, zf_off(ZF_C));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, dst_op.base, SZ_B);
		} else {
			cmp_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		if (inst->addr_mode != Z80_UNUSED) {
			z80_save_result(opts, inst);
			if (src_op.mode != MODE_UNUSED) {
				z80_save_reg(inst, opts);
			}
		} else {
			z80_save_reg(inst, opts);
		}
		break;
	case Z80_RLD:
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		//Before: (HL) = 0x12, A = 0x34
		//After: (HL) = 0x24, A = 0x31
		zreg_to_native(opts, Z80_A, opts->gen.scratch2);
		shl_ir(code, 4, opts->gen.scratch1, SZ_W);
		and_ir(code, 0xF, opts->gen.scratch2, SZ_W);
		and_ir(code, 0xFFF, opts->gen.scratch1, SZ_W);
		and_ir(code, 0xF0, opts->regs[Z80_A], SZ_B);
		or_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_W);
		//opts->gen.scratch1 = 0x0124
		ror_ir(code, 8, opts->gen.scratch1, SZ_W);
		cycles(&opts->gen, 4);
		or_rr(code, opts->gen.scratch1, opts->regs[Z80_A], SZ_B);
		//set flags
		//TODO: Implement half-carry flag
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));

		zreg_to_native(opts, Z80_HL, opts->gen.scratch2);
		ror_ir(code, 8, opts->gen.scratch1, SZ_W);
		call(code, opts->write_8);
		break;
	case Z80_RRD:
		cycles(&opts->gen, 8);
		zreg_to_native(opts, Z80_HL, opts->gen.scratch1);
		call(code, opts->read_8);
		//Before: (HL) = 0x12, A = 0x34
		//After: (HL) = 0x41, A = 0x32
		zreg_to_native(opts, Z80_A, opts->gen.scratch2);
		ror_ir(code, 4, opts->gen.scratch1, SZ_W);
		shl_ir(code, 4, opts->gen.scratch2, SZ_W);
		and_ir(code, 0xF00F, opts->gen.scratch1, SZ_W);
		and_ir(code, 0xF0, opts->regs[Z80_A], SZ_B);
		//opts->gen.scratch1 = 0x2001
		//opts->gen.scratch2 = 0x0040
		or_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_W);
		//opts->gen.scratch1 = 0x2041
		ror_ir(code, 8, opts->gen.scratch1, SZ_W);
		cycles(&opts->gen, 4);
		shr_ir(code, 4, opts->gen.scratch1, SZ_B);
		or_rr(code, opts->gen.scratch1, opts->regs[Z80_A], SZ_B);
		//set flags
		//TODO: Implement half-carry flag
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		setcc_rdisp(code, CC_P, opts->gen.context_reg, zf_off(ZF_PV));
		setcc_rdisp(code, CC_Z, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));

		zreg_to_native(opts, Z80_HL, opts->gen.scratch2);
		ror_ir(code, 8, opts->gen.scratch1, SZ_W);
		call(code, opts->write_8);
		break;
	case Z80_BIT: {
		num_cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		cycles(&opts->gen, num_cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			src_op.mode = MODE_REG_DIRECT;
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			translate_z80_ea(inst, &src_op, opts, READ, DONT_MODIFY);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read at the end of a bit instruction takes 4
			cycles(&opts->gen, 1);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			bt_ir(code, bit, src_op.base, size);
		} else {
			bt_irdisp(code, bit, src_op.base, src_op.disp, size);
		}
		setcc_rdisp(code, CC_NC, opts->gen.context_reg, zf_off(ZF_Z));
		setcc_rdisp(code, CC_NC, opts->gen.context_reg, zf_off(ZF_PV));
		mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_N), SZ_B);
		if (inst->immed == 7) {
			if (src_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, src_op.base, size);
			} else {
				cmp_irdisp(code, 0, src_op.base, src_op.disp, size);
			}
			setcc_rdisp(code, CC_S, opts->gen.context_reg, zf_off(ZF_S));
		} else {
			mov_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
		}
		break;
	}
	case Z80_SET: {
		num_cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		cycles(&opts->gen, num_cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			src_op.mode = MODE_REG_DIRECT;
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			translate_z80_ea(inst, &src_op, opts, READ, MODIFY);
		}
		if (inst->reg != Z80_USE_IMMED) {
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			cycles(&opts->gen, 1);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			bts_ir(code, bit, src_op.base, size);
		} else {
			bts_irdisp(code, bit, src_op.base, src_op.disp, size);
		}
		if (inst->reg != Z80_USE_IMMED) {
			if (size == SZ_W) {
#ifdef X86_64
				if (dst_op.base >= R8) {
					ror_ir(code, 8, src_op.base, SZ_W);
					mov_rr(code, opts->regs[z80_low_reg(inst->ea_reg)], dst_op.base, SZ_B);
					ror_ir(code, 8, src_op.base, SZ_W);
				} else {
#endif
					if (dst_op.mode == MODE_REG_DIRECT) {
						zreg_to_native(opts, inst->ea_reg, dst_op.base);
					} else {
						zreg_to_native(opts, inst->ea_reg, opts->gen.scratch1);
						mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
					}
#ifdef X86_64
				}
#endif
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (src_op.mode == MODE_REG_DIRECT) {
						mov_rr(code, src_op.base, dst_op.base, SZ_B);
					} else {
						mov_rdispr(code, src_op.base, src_op.disp, dst_op.base, SZ_B);
					}
				} else if (src_op.mode == MODE_REG_DIRECT) {
					mov_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, SZ_B);
				} else {
					mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
					mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
				}
			}
		}
		if ((inst->addr_mode & 0x1F) != Z80_REG) {
			z80_save_result(opts, inst);
			if (inst->reg != Z80_USE_IMMED) {
				z80_save_reg(inst, opts);
			}
		}
		break;
	}
	case Z80_RES: {
		num_cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		cycles(&opts->gen, num_cycles);
		uint8_t bit;
		if ((inst->addr_mode & 0x1F) == Z80_REG && opts->regs[inst->ea_reg] >= AH && opts->regs[inst->ea_reg] <= BH) {
			src_op.base = opts->regs[z80_word_reg(inst->ea_reg)];
			src_op.mode = MODE_REG_DIRECT;
			size = SZ_W;
			bit = inst->immed + 8;
		} else {
			size = SZ_B;
			bit = inst->immed;
			translate_z80_ea(inst, &src_op, opts, READ, MODIFY);
		}
		if (inst->reg != Z80_USE_IMMED) {
			translate_z80_reg(inst, &dst_op, opts);
		}
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			cycles(&opts->gen, 1);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			btr_ir(code, bit, src_op.base, size);
		} else {
			btr_irdisp(code, bit, src_op.base, src_op.disp, size);
		}
		if (inst->reg != Z80_USE_IMMED) {
			if (size == SZ_W) {
#ifdef X86_64
				if (dst_op.base >= R8) {
					ror_ir(code, 8, src_op.base, SZ_W);
					mov_rr(code, opts->regs[z80_low_reg(inst->ea_reg)], dst_op.base, SZ_B);
					ror_ir(code, 8, src_op.base, SZ_W);
				} else {
#endif
					if (dst_op.mode == MODE_REG_DIRECT) {
						zreg_to_native(opts, inst->ea_reg, dst_op.base);
					} else {
						zreg_to_native(opts, inst->ea_reg, opts->gen.scratch1);
						mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
					}
#ifdef X86_64
				}
#endif
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (src_op.mode == MODE_REG_DIRECT) {
						mov_rr(code, src_op.base, dst_op.base, SZ_B);
					} else {
						mov_rdispr(code, src_op.base, src_op.disp, dst_op.base, SZ_B);
					}
				} else if (src_op.mode == MODE_REG_DIRECT) {
					mov_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, SZ_B);
				} else {
					mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
					mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
				}
			}
		}
		if (inst->addr_mode != Z80_REG) {
			z80_save_result(opts, inst);
			if (inst->reg != Z80_USE_IMMED) {
				z80_save_reg(inst, opts);
			}
		}
		break;
	}
	case Z80_JP: {
		num_cycles = 4;
		if (inst->addr_mode != Z80_REG_INDIRECT) {
			num_cycles += 6;
		} else if(inst->ea_reg == Z80_IX || inst->ea_reg == Z80_IY) {
			num_cycles += 4;
		}
		cycles(&opts->gen, num_cycles);
		if (inst->addr_mode != Z80_REG_INDIRECT) {
			code_ptr call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
				opts->gen.deferred = defer_address(opts->gen.deferred, inst->immed, code->cur + 1);
				//fake address to force large displacement
				call_dst = code->cur + 256;
			}
			jmp(code, call_dst);
		} else {
			if (inst->addr_mode == Z80_REG_INDIRECT) {
				zreg_to_native(opts, inst->ea_reg, opts->gen.scratch1);
			} else {
				mov_ir(code, inst->immed, opts->gen.scratch1, SZ_W);
			}
			call(code, opts->native_addr);
			jmp_r(code, opts->gen.scratch1);
		}
		break;
	}
	case Z80_JPCC: {
		cycles(&opts->gen, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_jump_off = code->cur+1;
		jcc(code, cond, code->cur+2);
		cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = inst->immed;
		code_ptr call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, dest_addr, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		*no_jump_off = code->cur - (no_jump_off+1);
		break;
	}
	case Z80_JR: {
		cycles(&opts->gen, 12);//T States: 4,3,5
		uint16_t dest_addr = address + inst->immed + 2;
		code_ptr call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, dest_addr, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		break;
	}
	case Z80_JRCC: {
		cycles(&opts->gen, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		}
		uint8_t *no_jump_off = code->cur+1;
		jcc(code, cond, code->cur+2);
		cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = address + inst->immed + 2;
		code_ptr call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, dest_addr, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		*no_jump_off = code->cur - (no_jump_off+1);
		break;
	}
	case Z80_DJNZ: {
		cycles(&opts->gen, 8);//T States: 5,3
		if (opts->regs[Z80_B] >= 0) {
			sub_ir(code, 1, opts->regs[Z80_B], SZ_B);
		} else {
			sub_irdisp(code, 1, opts->gen.context_reg, zr_off(Z80_B), SZ_B);
		}
		uint8_t *no_jump_off = code->cur+1;
		jcc(code, CC_Z, code->cur+2);
		cycles(&opts->gen, 5);//T States: 5
		uint16_t dest_addr = address + inst->immed + 2;
		code_ptr call_dst = z80_get_native_address(context, dest_addr);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, dest_addr, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		*no_jump_off = code->cur - (no_jump_off+1);
		break;
		}
	case Z80_CALL: {
		cycles(&opts->gen, 11);//T States: 4,3,4
		sub_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		mov_ir(code, address + 3, opts->gen.scratch1, SZ_W);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		call(code, opts->write_16_highfirst);//T States: 3, 3
		code_ptr call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, inst->immed, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		break;
	}
	case Z80_CALLCC: {
		cycles(&opts->gen, 10);//T States: 4,3,3 (false case)
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = code->cur+1;
		jcc(code, cond, code->cur+2);
		cycles(&opts->gen, 1);//Last of the above T states takes an extra cycle in the true case
		sub_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		mov_ir(code, address + 3, opts->gen.scratch1, SZ_W);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		call(code, opts->write_16_highfirst);//T States: 3, 3
		code_ptr call_dst = z80_get_native_address(context, inst->immed);
			if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, inst->immed, code->cur + 1);
				//fake address to force large displacement
			call_dst = code->cur + 256;
			}
		jmp(code, call_dst);
		*no_call_off = code->cur - (no_call_off+1);
		break;
		}
	case Z80_RET:
		cycles(&opts->gen, 4);//T States: 4
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		call(code, opts->read_16);//T STates: 3, 3
		add_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case Z80_RETCC: {
		cycles(&opts->gen, 5);//T States: 5
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
			cond = CC_NZ;
		case Z80_CC_M:
			cmp_irdisp(code, 0, opts->gen.context_reg, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = code->cur+1;
		jcc(code, cond, code->cur+2);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		call(code, opts->read_16);//T STates: 3, 3
		add_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		*no_call_off = code->cur - (no_call_off+1);
		break;
	}
	case Z80_RETI:
		//For some systems, this may need a callback for signalling interrupt routine completion
		cycles(&opts->gen, 8);//T States: 4, 4
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		call(code, opts->read_16);//T STates: 3, 3
		add_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case Z80_RETN:
		cycles(&opts->gen, 8);//T States: 4, 4
		mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, iff2), opts->gen.scratch2, SZ_B);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch1, SZ_W);
		mov_rrdisp(code, opts->gen.scratch2, opts->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
		call(code, opts->read_16);//T STates: 3, 3
		add_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case Z80_RST: {
		//RST is basically CALL to an address in page 0
		cycles(&opts->gen, 5);//T States: 5
		sub_ir(code, 2, opts->regs[Z80_SP], SZ_W);
		mov_ir(code, address + 1, opts->gen.scratch1, SZ_W);
		mov_rr(code, opts->regs[Z80_SP], opts->gen.scratch2, SZ_W);
		call(code, opts->write_16_highfirst);//T States: 3, 3
		code_ptr call_dst = z80_get_native_address(context, inst->immed);
		if (!call_dst) {
			opts->gen.deferred = defer_address(opts->gen.deferred, inst->immed, code->cur + 1);
			//fake address to force large displacement
			call_dst = code->cur + 256;
		}
		jmp(code, call_dst);
		break;
	}
	case Z80_IN:
		cycles(&opts->gen, inst->reg == Z80_A ? 7 : 8);//T States: 4 3/4
		if (inst->addr_mode == Z80_IMMED_INDIRECT) {
			mov_ir(code, inst->immed, opts->gen.scratch1, SZ_B);
		} else {
			mov_rr(code, opts->regs[Z80_C], opts->gen.scratch1, SZ_B);
		}
		call(code, opts->read_io);
		translate_z80_reg(inst, &dst_op, opts);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, opts->gen.scratch1, dst_op.base, SZ_B);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
		}
		z80_save_reg(inst, opts);
		break;
	/*case Z80_INI:
	case Z80_INIR:
	case Z80_IND:
	case Z80_INDR:*/
	case Z80_OUT:
		cycles(&opts->gen, inst->reg == Z80_A ? 7 : 8);//T States: 4 3/4
		if ((inst->addr_mode & 0x1F) == Z80_IMMED_INDIRECT) {
			mov_ir(code, inst->immed, opts->gen.scratch2, SZ_B);
		} else {
			zreg_to_native(opts, Z80_C, opts->gen.scratch2);
			mov_rr(code, opts->regs[Z80_C], opts->gen.scratch2, SZ_B);
		}
		translate_z80_reg(inst, &src_op, opts);
		if (src_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, src_op.base, opts->gen.scratch1, SZ_B);
		} else if (src_op.mode == MODE_IMMED) {
			mov_ir(code, src_op.disp, opts->gen.scratch1, SZ_B);
		} else {
			mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
		}
		call(code, opts->write_io);
		z80_save_reg(inst, opts);
		break;
	/*case Z80_OUTI:
	case Z80_OTIR:
	case Z80_OUTD:
	case Z80_OTDR:*/
	default: {
		char disbuf[80];
		z80_disasm(inst, disbuf, address);
		FILE * f = fopen("zram.bin", "wb");
		fwrite(context->mem_pointers[0], 1, 8 * 1024, f);
		fclose(f);
		fatal_error("unimplemented Z80 instruction: %s at %X\nZ80 RAM has been saved to zram.bin for debugging", disbuf, address);
	}
	}
}

uint8_t * z80_interp_handler(uint8_t opcode, z80_context * context)
{
	if (!context->interp_code[opcode]) {
		if (opcode == 0xCB || (opcode >= 0xDD && (opcode & 0xF) == 0xD)) {
			fatal_error("Encountered prefix byte %X at address %X. Z80 interpeter doesn't support those yet.", opcode, context->pc);
		}
		uint8_t codebuf[8];
		memset(codebuf, 0, sizeof(codebuf));
		codebuf[0] = opcode;
		z80inst inst;
		uint8_t * after = z80_decode(codebuf, &inst);
		if (after - codebuf > 1) {
			fatal_error("Encountered multi-byte Z80 instruction at %X. Z80 interpeter doesn't support those yet.", context->pc);
		}

		z80_options * opts = context->options;
		code_info *code = &opts->gen.code;
		check_alloc_code(code, ZMAX_NATIVE_SIZE);
		context->interp_code[opcode] = code->cur;
		translate_z80inst(&inst, context, 0, 1);
		mov_rdispr(code, opts->gen.context_reg, offsetof(z80_context, pc), opts->gen.scratch1, SZ_W);
		add_ir(code, after - codebuf, opts->gen.scratch1, SZ_W);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		z80_handle_deferred(context);
	}
	return context->interp_code[opcode];
}

code_info z80_make_interp_stub(z80_context * context, uint16_t address)
{
	z80_options *opts = context->options;
	code_info * code = &opts->gen.code;
	check_alloc_code(code, 32);
	code_info stub = {code->cur, NULL};
	//TODO: make this play well with the breakpoint code
	mov_ir(code, address, opts->gen.scratch1, SZ_W);
	call(code, opts->read_8);
	//normal opcode fetch is already factored into instruction timing
	//back out the base 3 cycles from a read here
	//not quite perfect, but it will have to do for now
	cycles(&opts->gen, -3);
	check_cycles_int(&opts->gen, address);
	call(code, opts->gen.save_context);
	mov_irdisp(code, address, opts->gen.context_reg, offsetof(z80_context, pc), SZ_W);
	push_r(code, opts->gen.context_reg);
	call_args(code, (code_ptr)z80_interp_handler, 2, opts->gen.scratch1, opts->gen.context_reg);
	mov_rr(code, RAX, opts->gen.scratch1, SZ_PTR);
	pop_r(code, opts->gen.context_reg);
	call(code, opts->gen.load_context);
	jmp_r(code, opts->gen.scratch1);
	stub.last = code->cur;
	return stub;
}


uint8_t * z80_get_native_address(z80_context * context, uint32_t address)
{
	native_map_slot *map;
	if (address < 0x4000) {
		address &= 0x1FFF;
		map = context->static_code_map;
	} else {
		address -= 0x4000;
		map = context->banked_code_map;
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
	//TODO: Fix for addresses >= 0x4000
	if (address >= 0x4000) {
		return 0;
	}
	return opts->gen.ram_inst_sizes[0][address & 0x1FFF];
}

void z80_map_native_address(z80_context * context, uint32_t address, uint8_t * native_address, uint8_t size, uint8_t native_size)
{
	uint32_t orig_address = address;
	native_map_slot *map;
	z80_options * opts = context->options;
	if (address < 0x4000) {
		address &= 0x1FFF;
		map = context->static_code_map;
		opts->gen.ram_inst_sizes[0][address] = native_size;
		context->ram_code_flags[(address & 0x1C00) >> 10] |= 1 << ((address & 0x380) >> 7);
		context->ram_code_flags[((address + size) & 0x1C00) >> 10] |= 1 << (((address + size) & 0x380) >> 7);
	} else {
		//HERE
		address -= 0x4000;
		map = context->banked_code_map;
		if (!map->offsets) {
			map->offsets = malloc(sizeof(int32_t) * 0xC000);
			memset(map->offsets, 0xFF, sizeof(int32_t) * 0xC000);
		}
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
		} else {
			address -= 0x4000;
			map = context->banked_code_map;
		}
		if (!map->offsets) {
			map->offsets = malloc(sizeof(int32_t) * 0xC000);
			memset(map->offsets, 0xFF, sizeof(int32_t) * 0xC000);
		}
		map->offsets[address] = EXTENSION_WORD;
	}
}

#define INVALID_INSTRUCTION_START 0xFEEDFEED

uint32_t z80_get_instruction_start(native_map_slot * static_code_map, uint32_t address)
{
	//TODO: Fixme for address >= 0x4000
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
		code_ptr dst = z80_get_native_address(context, inst_start);
		code_info code = {dst, dst+16};
		z80_options * opts = context->options;
		dprintf("patching code at %p for Z80 instruction at %X due to write to %X\n", code.cur, inst_start, address);
		mov_ir(&code, inst_start, opts->gen.scratch1, SZ_D);
		call(&code, opts->retrans_stub);
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
	process_deferred(&opts->gen.deferred, context, (native_addr_func)z80_get_native_address);
	if (opts->gen.deferred) {
		translate_z80_stream(context, opts->gen.deferred->address);
	}
}

extern void * z80_retranslate_inst(uint32_t address, z80_context * context, uint8_t * orig_start) asm("z80_retranslate_inst");
void * z80_retranslate_inst(uint32_t address, z80_context * context, uint8_t * orig_start)
{
	char disbuf[80];
	z80_options * opts = context->options;
	uint8_t orig_size = z80_get_native_inst_size(opts, address);
	code_info *code = &opts->gen.code;
	uint8_t *after, *inst = get_native_pointer(address, (void **)context->mem_pointers, &opts->gen);
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
		check_alloc_code(code, ZMAX_NATIVE_SIZE);
		code_ptr start = code->cur;
		deferred_addr * orig_deferred = opts->gen.deferred;
		translate_z80inst(&instbuf, context, address, 0);
		/*
		if ((native_end - dst) <= orig_size) {
			uint8_t * native_next = z80_get_native_address(context, address + after-inst);
			if (native_next && ((native_next == orig_start + orig_size) || (orig_size - (native_end - dst)) > 5)) {
				remove_deferred_until(&opts->gen.deferred, orig_deferred);
				native_end = translate_z80inst(&instbuf, orig_start, context, address, 0);
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
		}*/
		z80_map_native_address(context, address, start, after-inst, ZMAX_NATIVE_SIZE);
		code_info tmp_code = {orig_start, orig_start + 16};
		jmp(&tmp_code, start);
		tmp_code = *code;
		code->cur = start + ZMAX_NATIVE_SIZE;
		if (!z80_is_terminal(&instbuf)) {
			jmp(&tmp_code, z80_get_native_address_trans(context, address + after-inst));
		}
		z80_handle_deferred(context);
		return start;
	} else {
		code_info tmp_code = *code;
		code->cur = orig_start;
		code->last = orig_start + ZMAX_NATIVE_SIZE;
		translate_z80inst(&instbuf, context, address, 0);
		code_info tmp2 = *code;
		*code = tmp_code;
		if (!z80_is_terminal(&instbuf)) {

			jmp(&tmp2, z80_get_native_address_trans(context, address + after-inst));
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

	do
	{
		z80inst inst;
		dprintf("translating Z80 code at address %X\n", address);
		do {
			uint8_t * existing = z80_get_native_address(context, address);
			if (existing) {
				jmp(&opts->gen.code, existing);
				break;
			}
			uint8_t * encoded, *next;
			encoded = get_native_pointer(address, (void **)context->mem_pointers, &opts->gen);
			if (!encoded) {
				code_info stub = z80_make_interp_stub(context, address);
				z80_map_native_address(context, address, stub.cur, 1, stub.last - stub.cur);
				break;
			}
			//make sure prologue is in a contiguous chunk of code
			check_code_prologue(&opts->gen.code);
			next = z80_decode(encoded, &inst);
			#ifdef DO_DEBUG_PRINT
			z80_disasm(&inst, disbuf, address);
			if (inst.op == Z80_NOP) {
				printf("%X\t%s(%d)\n", address, disbuf, inst.immed);
			} else {
				printf("%X\t%s\n", address, disbuf);
			}
			#endif
			code_ptr start = opts->gen.code.cur;
			translate_z80inst(&inst, context, address, 0);
			z80_map_native_address(context, address, start, next-encoded, opts->gen.code.cur - start);
			address += next-encoded;
				address &= 0xFFFF;
		} while (!z80_is_terminal(&inst));
		process_deferred(&opts->gen.deferred, context, (native_addr_func)z80_get_native_address);
		if (opts->gen.deferred) {
			address = opts->gen.deferred->address;
			dprintf("defferred address: %X\n", address);
		}
	} while (opts->gen.deferred);
}

void init_z80_opts(z80_options * options, memmap_chunk const * chunks, uint32_t num_chunks, memmap_chunk const * io_chunks, uint32_t num_io_chunks, uint32_t clock_divider)
{
	memset(options, 0, sizeof(*options));

	options->gen.memmap = chunks;
	options->gen.memmap_chunks = num_chunks;
	options->gen.address_size = SZ_W;
	options->gen.address_mask = 0xFFFF;
	options->gen.max_address = 0x10000;
	options->gen.bus_cycles = 3;
	options->gen.clock_divider = clock_divider;
	options->gen.mem_ptr_off = offsetof(z80_context, mem_pointers);
	options->gen.ram_flags_off = offsetof(z80_context, ram_code_flags);
	options->gen.ram_flags_shift = 7;

	options->flags = 0;
#ifdef X86_64
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

	options->gen.scratch1 = R13;
	options->gen.scratch2 = R14;
#else
	memset(options->regs, -1, sizeof(options->regs));
	options->regs[Z80_A] = RAX;
	options->regs[Z80_SP] = RBX;

	options->gen.scratch1 = RCX;
	options->gen.scratch2 = RDX;
#endif

	options->gen.context_reg = RSI;
	options->gen.cycles = RBP;
	options->gen.limit = RDI;

	options->gen.native_code_map = malloc(sizeof(native_map_slot));
	memset(options->gen.native_code_map, 0, sizeof(native_map_slot));
	options->gen.deferred = NULL;
	options->gen.ram_inst_sizes = malloc(sizeof(uint8_t) * 0x2000 + sizeof(uint8_t *));
	options->gen.ram_inst_sizes[0] = (uint8_t *)(options->gen.ram_inst_sizes + 1);
	memset(options->gen.ram_inst_sizes[0], 0, sizeof(uint8_t) * 0x2000);

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
			reg = i /2 + Z80_BC + (i > Z80_H ? 2 : 0);
			size = SZ_W;
		} else {
			reg = i;
			size = SZ_B;
}
		if (options->regs[reg] >= 0) {
			mov_rrdisp(code, options->regs[reg], options->gen.context_reg, offsetof(z80_context, regs) + i, size);
		}
		if (size == SZ_W) {
			i++;
		}
	}
	if (options->regs[Z80_SP] >= 0) {
		mov_rrdisp(code, options->regs[Z80_SP], options->gen.context_reg, offsetof(z80_context, sp), SZ_W);
	}
	mov_rrdisp(code, options->gen.limit, options->gen.context_reg, offsetof(z80_context, target_cycle), SZ_D);
	mov_rrdisp(code, options->gen.cycles, options->gen.context_reg, offsetof(z80_context, current_cycle), SZ_D);
	retn(code);

	options->load_context_scratch = code->cur;
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, scratch1), options->gen.scratch1, SZ_W);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, scratch2), options->gen.scratch2, SZ_W);
	options->gen.load_context = code->cur;
	for (int i = 0; i <= Z80_A; i++)
	{
		int reg;
		uint8_t size;
		if (i < Z80_I) {
			reg = i /2 + Z80_BC + (i > Z80_H ? 2 : 0);
			size = SZ_W;
		} else {
			reg = i;
			size = SZ_B;
		}
		if (options->regs[reg] >= 0) {
			mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, regs) + i, options->regs[reg], size);
		}
		if (size == SZ_W) {
			i++;
		}
	}
	if (options->regs[Z80_SP] >= 0) {
		mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, sp), options->regs[Z80_SP], SZ_W);
	}
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, target_cycle), options->gen.limit, SZ_D);
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, current_cycle), options->gen.cycles, SZ_D);
	retn(code);

	options->native_addr = code->cur;
	call(code, options->gen.save_context);
	push_r(code, options->gen.context_reg);
	movzx_rr(code, options->gen.scratch1, options->gen.scratch1, SZ_W, SZ_D);
	call_args(code, (code_ptr)z80_get_native_address_trans, 2, options->gen.context_reg, options->gen.scratch1);
	mov_rr(code, RAX, options->gen.scratch1, SZ_PTR);
	pop_r(code, options->gen.context_reg);
	call(code, options->gen.load_context);
	retn(code);

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
	restore_callee_save_regs(code);
	*no_sync = code->cur - (no_sync + 1);
	//return to caller of z80_run
	retn(code);

	options->gen.handle_code_write = (code_ptr)z80_handle_code_write;

	options->read_8 = gen_mem_fun(&options->gen, chunks, num_chunks, READ_8, &options->read_8_noinc);
	options->write_8 = gen_mem_fun(&options->gen, chunks, num_chunks, WRITE_8, &options->write_8_noinc);

	options->gen.handle_cycle_limit_int = code->cur;
	cmp_rdispr(code, options->gen.context_reg, offsetof(z80_context, int_cycle), options->gen.cycles, SZ_D);
	code_ptr skip_int = code->cur+1;
	jcc(code, CC_B, skip_int);
	//set limit to the cycle limit
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, sync_cycle), options->gen.limit, SZ_D);
	//disable interrupts
	mov_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, iff1), SZ_B);
	mov_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, iff2), SZ_B);
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
	pop_r(code, options->gen.scratch2);
	//TODO: Support interrupt mode 0 and 2
	mov_ir(code, 0x38, options->gen.scratch1, SZ_W);
	call(code, options->native_addr);
	mov_rrind(code, options->gen.scratch1, options->gen.context_reg, SZ_PTR);
	restore_callee_save_regs(code);
	//return to caller of z80_run to sync
	retn(code);
	*skip_int = code->cur - (skip_int+1);
	cmp_rdispr(code, options->gen.context_reg, offsetof(z80_context, sync_cycle), options->gen.cycles, SZ_D);
	code_ptr skip_sync = code->cur + 1;
	jcc(code, CC_B, skip_sync);
	//save PC
	mov_rrdisp(code, options->gen.scratch1, options->gen.context_reg, offsetof(z80_context, pc), SZ_D);
	options->do_sync = code->cur;
	call(code, options->gen.save_context);
	pop_rind(code, options->gen.context_reg);
	//restore callee saved registers
	restore_callee_save_regs(code);
	//return to caller of z80_run
	*skip_sync = code->cur - (skip_sync+1);
	retn(code);

	//HACK
	options->gen.address_size = SZ_D;
	options->gen.address_mask = 0xFF;
	options->read_io = gen_mem_fun(&options->gen, io_chunks, num_io_chunks, READ_8, NULL);
	options->write_io = gen_mem_fun(&options->gen, io_chunks, num_io_chunks, WRITE_8, NULL);
	options->gen.address_size = SZ_W;
	options->gen.address_mask = 0xFFFF;

	options->read_16 = code->cur;
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	//TODO: figure out how to handle the extra wait state for word reads to bank area
	//may also need special handling to avoid too much stack depth when access is blocked
	push_r(code, options->gen.scratch1);
	call(code, options->read_8_noinc);
	mov_rr(code, options->gen.scratch1, options->gen.scratch2, SZ_B);
#ifndef X86_64
	//scratch 2 is a caller save register in 32-bit builds and may be clobbered by something called from the read8 fun
	mov_rrdisp(code, options->gen.scratch1, options->gen.context_reg, offsetof(z80_context, scratch2), SZ_B);
#endif
	pop_r(code, options->gen.scratch1);
	add_ir(code, 1, options->gen.scratch1, SZ_W);
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	call(code, options->read_8_noinc);
	shl_ir(code, 8, options->gen.scratch1, SZ_W);
#ifdef X86_64
	mov_rr(code, options->gen.scratch2, options->gen.scratch1, SZ_B);
#else
	mov_rdispr(code, options->gen.context_reg, offsetof(z80_context, scratch2), options->gen.scratch1, SZ_B);
#endif
	retn(code);

	options->write_16_highfirst = code->cur;
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	push_r(code, options->gen.scratch2);
	push_r(code, options->gen.scratch1);
	add_ir(code, 1, options->gen.scratch2, SZ_W);
	shr_ir(code, 8, options->gen.scratch1, SZ_W);
	call(code, options->write_8_noinc);
	pop_r(code, options->gen.scratch1);
	pop_r(code, options->gen.scratch2);
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	//TODO: Check if we can get away with TCO here
	call(code, options->write_8_noinc);
	retn(code);

	options->write_16_lowfirst = code->cur;
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	push_r(code, options->gen.scratch2);
	push_r(code, options->gen.scratch1);
	call(code, options->write_8_noinc);
	pop_r(code, options->gen.scratch1);
	pop_r(code, options->gen.scratch2);
	add_ir(code, 1, options->gen.scratch2, SZ_W);
	shr_ir(code, 8, options->gen.scratch1, SZ_W);
	cycles(&options->gen, 3);
	check_cycles(&options->gen);
	//TODO: Check if we can get away with TCO here
	call(code, options->write_8_noinc);
	retn(code);

	options->retrans_stub = code->cur;
	//pop return address
	pop_r(code, options->gen.scratch2);
	call(code, options->gen.save_context);
	//adjust pointer before move and call instructions that got us here
	sub_ir(code, options->gen.scratch1 >= R8 ? 11 : 10, options->gen.scratch2, SZ_PTR);
	push_r(code, options->gen.context_reg);
	call_args(code, (code_ptr)z80_retranslate_inst, 3, options->gen.scratch1, options->gen.context_reg, options->gen.scratch2);
	pop_r(code, options->gen.context_reg);
	mov_rr(code, RAX, options->gen.scratch1, SZ_PTR);
	call(code, options->gen.load_context);
	jmp_r(code, options->gen.scratch1);

	options->run = (z80_run_fun)code->cur;
	save_callee_save_regs(code);
#ifdef X86_64
	mov_rr(code, RDI, options->gen.context_reg, SZ_PTR);
#else
	mov_rdispr(code, RSP, 5 * sizeof(int32_t), options->gen.context_reg, SZ_PTR);
#endif
	call(code, options->load_context_scratch);
	cmp_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, extra_pc), SZ_PTR);
	code_ptr no_extra = code->cur+1;
	jcc(code, CC_Z, no_extra);
	push_rdisp(code, options->gen.context_reg, offsetof(z80_context, extra_pc));
	mov_irdisp(code, 0, options->gen.context_reg, offsetof(z80_context, extra_pc), SZ_PTR);
	*no_extra = code->cur - (no_extra + 1);
	jmp_rind(code, options->gen.context_reg);
}

void init_z80_context(z80_context * context, z80_options * options)
{
	memset(context, 0, sizeof(*context));
	context->static_code_map = malloc(sizeof(*context->static_code_map));
	context->static_code_map->base = NULL;
	context->static_code_map->offsets = malloc(sizeof(int32_t) * 0x2000);
	memset(context->static_code_map->offsets, 0xFF, sizeof(int32_t) * 0x2000);
	context->banked_code_map = malloc(sizeof(native_map_slot));
	memset(context->banked_code_map, 0, sizeof(native_map_slot));
	context->options = options;
	context->int_cycle = CYCLE_NEVER;
	context->int_pulse_start = CYCLE_NEVER;
	context->int_pulse_end = CYCLE_NEVER;
}

void z80_run(z80_context * context, uint32_t target_cycle)
{
	if (context->reset || context->busack) {
		context->current_cycle = target_cycle;
	} else {
		if (context->current_cycle < target_cycle) {
			//busreq is sampled at the end of an m-cycle
			//we can approximate that by running for a single m-cycle after a bus request
			context->sync_cycle = context->busreq ? context->current_cycle + 3*context->options->gen.clock_divider : target_cycle;
			if (!context->native_pc) {
				context->native_pc = z80_get_native_address_trans(context, context->pc);
			}
			while (context->current_cycle < context->sync_cycle)
			{
				if (context->int_pulse_end < context->current_cycle || context->int_pulse_end == CYCLE_NEVER) {
					z80_next_int_pulse(context);
				}
				if (context->iff1) {
					context->int_cycle = context->int_pulse_start < context->int_enable_cycle ? context->int_enable_cycle : context->int_pulse_start;
				} else {
					context->int_cycle = CYCLE_NEVER;
				}
				context->target_cycle = context->sync_cycle < context->int_cycle ? context->sync_cycle : context->int_cycle;
				dprintf("Running Z80 from cycle %d to cycle %d. Int cycle: %d (%d - %d)\n", context->current_cycle, context->sync_cycle, context->int_cycle, context->int_pulse_start, context->int_pulse_end);
				context->options->run(context);
				dprintf("Z80 ran to cycle %d\n", context->current_cycle);
			}
			if (context->busreq) {
				context->busack = 1;
				context->current_cycle = target_cycle;
			}
		}
	}
}

void z80_assert_reset(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->reset = 1;
}

void z80_clear_reset(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	if (context->reset) {
		//TODO: Handle case where reset is not asserted long enough
		context->im = 0;
		context->iff1 = context->iff2 = 0;
		context->native_pc = NULL;
		context->extra_pc = NULL;
		context->pc = 0;
		context->reset = 0;
		if (context->busreq) {
			//TODO: Figure out appropriate delay
			context->busack = 1;
		}
	}
}

void z80_assert_busreq(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->busreq = 1;
}

void z80_clear_busreq(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->busreq = 0;
	context->busack = 0;
}

uint8_t z80_get_busack(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	return context->busack;
}

void z80_adjust_cycles(z80_context * context, uint32_t deduction)
{
	if (context->current_cycle < deduction) {
		fprintf(stderr, "WARNING: Deduction of %u cycles when Z80 cycle counter is only %u\n", deduction, context->current_cycle);
		context->current_cycle = 0;
	} else {
		context->current_cycle -= deduction;
	}
	if (context->int_enable_cycle != CYCLE_NEVER) {
		if (context->int_enable_cycle < deduction) {
			context->int_enable_cycle = 0;
		} else {
			context->int_enable_cycle -= deduction;
		}
	}
	if (context->int_pulse_start != CYCLE_NEVER) {
		if (context->int_pulse_end < deduction) {
			context->int_pulse_start = context->int_pulse_end = CYCLE_NEVER;
		} else {
			context->int_pulse_end -= deduction;
			if (context->int_pulse_start < deduction) {
				context->int_pulse_start = 0;
			} else {
				context->int_pulse_start -= deduction;
			}
		}
	}
}

uint32_t zbreakpoint_patch(z80_context * context, uint16_t address, code_ptr dst)
{
	code_info code = {dst, dst+16};
	mov_ir(&code, address, context->options->gen.scratch1, SZ_W);
	call(&code, context->bp_stub);
	return code.cur-dst;
}

void zcreate_stub(z80_context * context)
{
	z80_options * opts = context->options;
	code_info *code = &opts->gen.code;
	check_code_prologue(code);
	context->bp_stub = code->cur;

		//Calculate length of prologue
	check_cycles_int(&opts->gen, 0);
	int check_int_size = code->cur-context->bp_stub;
	code->cur = context->bp_stub;

	//Calculate length of patch
	int patch_size = zbreakpoint_patch(context, 0, code->cur);

		//Save context and call breakpoint handler
	call(code, opts->gen.save_context);
	push_r(code, opts->gen.scratch1);
	call_args_abi(code, context->bp_handler, 2, opts->gen.context_reg, opts->gen.scratch1);
	mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
		//Restore context
	call(code, opts->gen.load_context);
	pop_r(code, opts->gen.scratch1);
		//do prologue stuff
	cmp_rr(code, opts->gen.cycles, opts->gen.limit, SZ_D);
	uint8_t * jmp_off = code->cur+1;
	jcc(code, CC_NC, code->cur + 7);
	pop_r(code, opts->gen.scratch1);
	add_ir(code, check_int_size - patch_size, opts->gen.scratch1, SZ_PTR);
	push_r(code, opts->gen.scratch1);
	jmp(code, opts->gen.handle_cycle_limit_int);
	*jmp_off = code->cur - (jmp_off+1);
		//jump back to body of translated instruction
	pop_r(code, opts->gen.scratch1);
	add_ir(code, check_int_size - patch_size, opts->gen.scratch1, SZ_PTR);
	jmp_r(code, opts->gen.scratch1);
}

void zinsert_breakpoint(z80_context * context, uint16_t address, uint8_t * bp_handler)
{
	context->bp_handler = bp_handler;
	uint8_t bit = 1 << (address % 8);
	if (!(bit & context->breakpoint_flags[address / 8])) {
		context->breakpoint_flags[address / 8] |= bit;
		if (!context->bp_stub) {
			zcreate_stub(context);
		}
		uint8_t * native = z80_get_native_address(context, address);
		if (native) {
			zbreakpoint_patch(context, address, native);
		}
	}
}

void zremove_breakpoint(z80_context * context, uint16_t address)
{
	context->breakpoint_flags[address / 8] &= ~(1 << (address % 8));
	uint8_t * native = z80_get_native_address(context, address);
	if (native) {
		z80_options * opts = context->options;
		code_info tmp_code = opts->gen.code;
		opts->gen.code.cur = native;
		opts->gen.code.last = native + 16;
		check_cycles_int(&opts->gen, address);
		opts->gen.code = tmp_code;
	}
}

