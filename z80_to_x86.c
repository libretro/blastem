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

uint8_t z80_size(z80inst * inst)
{
	uint8_t reg = (inst->reg & 0x1F);
	if (reg != Z80_UNUSED && reg != Z80_USE_IMMED) {
		return reg < Z80_BC ? SZ_B : SZ_W;
	}
	//TODO: Handle any necessary special cases
	return SZ_B;
}

uint8_t z80_high_reg(uint8_t reg)
{
	switch(reg)
	{
	case Z80_C:
	case Z80_BC:
		return Z80_B;
	case Z80_E:
	case Z80_DE:
		return Z80_D;
	case Z80_L:
	case Z80_HL:
		return Z80_H;
	case Z80_IXL:
	case Z80_IX:
		return Z80_IXH;
	case Z80_IYL:
	case Z80_IY:
		return Z80_IYH;
	default:
		return Z80_UNUSED;
	}
}

uint8_t * zcycles(uint8_t * dst, uint32_t num_cycles)
{
	return add_ir(dst, num_cycles, ZCYCLES, SZ_D);
}

uint8_t * z80_check_cycles_int(uint8_t * dst, uint16_t address)
{
	dst = cmp_rr(dst, ZCYCLES, ZLIMIT, SZ_D);
	uint8_t * jmp_off = dst+1;
	dst = jcc(dst, CC_NC, dst + 7);
	dst = mov_ir(dst, address, SCRATCH2, SZ_W);
	dst = call(dst, (uint8_t *)z80_handle_cycle_limit_int);
	*jmp_off = dst - (jmp_off+1);
	return dst;
}

uint8_t * translate_z80_reg(z80inst * inst, x86_ea * ea, uint8_t * dst, x86_z80_options * opts)
{
	if (inst->reg == Z80_USE_IMMED) {
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
	} else if ((inst->reg & 0x1F) == Z80_UNUSED) {
		ea->mode = MODE_UNUSED;
	} else {
		ea->mode = MODE_REG_DIRECT;
		if (inst->reg == Z80_IYH) {
			ea->base = opts->regs[Z80_IYL];
			dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
		} else {
			ea->base = opts->regs[inst->reg];
		}
	}
	return dst;
}

uint8_t * z80_save_reg(uint8_t * dst, z80inst * inst, x86_z80_options * opts)
{
	if (inst->reg == Z80_IYH) {
		dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
	}
	return dst;
}

uint8_t * translate_z80_ea(z80inst * inst, x86_ea * ea, uint8_t * dst, x86_z80_options * opts, uint8_t read, uint8_t modify)
{
	uint8_t size, reg, areg;
	ea->mode = MODE_REG_DIRECT;
	areg = read ? SCRATCH1 : SCRATCH2;
	switch(inst->addr_mode & 0x1F)
	{
	case Z80_REG:
		if (inst->ea_reg == Z80_IYH) {
			ea->base = opts->regs[Z80_IYL];
			dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
		} else {
			ea->base = opts->regs[inst->ea_reg];
		}
		break;
	case Z80_REG_INDIRECT:
		dst = mov_rr(dst, opts->regs[inst->ea_reg], areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		ea->base = SCRATCH1;
		break;
	case Z80_IMMED:
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
		break;
	case Z80_IMMED_INDIRECT:
		dst = mov_ir(dst, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		ea->base = SCRATCH1;
		break;
	case Z80_IX_DISPLACE:
	case Z80_IY_DISPLACE:
		reg = opts->regs[inst->addr_mode == Z80_IX_DISPLACE ? Z80_IX : Z80_IY];
		dst = mov_rr(dst, reg, areg, SZ_W);
		dst = add_ir(dst, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		break;
	case Z80_UNUSED:
		ea->mode = MODE_UNUSED;
		break;
	default:
		fprintf(stderr, "Unrecognized Z80 addressing mode %d\n", inst->addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * z80_save_ea(uint8_t * dst, z80inst * inst, x86_z80_options * opts)
{
	if (inst->addr_mode == Z80_REG && inst->ea_reg == Z80_IYH) {
		dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
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

uint8_t * translate_z80inst(z80inst * inst, uint8_t * dst, z80_context * context, uint16_t address)
{
	uint32_t cycles;
	x86_ea src_op, dst_op;
	uint8_t size;
	x86_z80_options *opts = context->options;
	dst = z80_check_cycles_int(dst, address);
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
			break;
		case Z80_IMMED:
			cycles = size == SZ_B ? 7 : 10;
			break;
		case Z80_IMMED_INDIRECT:
			cycles = 10;
			break;
		case Z80_IX_DISPLACE:
		case Z80_IY_DISPLACE:
			cycles = 12;
			break;
		}
		if ((inst->reg >= Z80_IXL && inst->reg <= Z80_IYH) || inst->reg == Z80_IX || inst->reg == Z80_IY) {
			cycles += 4;
		}
		dst = zcycles(dst, cycles);
		if (inst->addr_mode & Z80_DIR) {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = translate_z80_ea(inst, &dst_op, dst, opts, DONT_READ, MODIFY);
		} else {
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, src_op.base, dst_op.base, size);
		} else {
			dst = mov_ir(dst, src_op.disp, dst_op.base, size);
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		if (inst->addr_mode & Z80_DIR) {
			dst = z80_save_result(dst, inst);
		}
		break;
	case Z80_PUSH:
		dst = zcycles(dst, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 9 : 5);
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {
			dst = mov_rdisp8r(dst, CONTEXT, zf_off(ZF_S), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 1, SCRATCH1, SZ_B);
			dst = or_rdisp8r(dst, CONTEXT, zf_off(ZF_Z), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 2, SCRATCH1, SZ_B);
			dst = or_rdisp8r(dst, CONTEXT, zf_off(ZF_H), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 2, SCRATCH1, SZ_B);
			dst = or_rdisp8r(dst, CONTEXT, zf_off(ZF_PV), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 1, SCRATCH1, SZ_B);
			dst = or_rdisp8r(dst, CONTEXT, zf_off(ZF_N), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 1, SCRATCH1, SZ_B);
			dst = or_rdisp8r(dst, CONTEXT, zf_off(ZF_C), SCRATCH1, SZ_B);
			dst = shl_ir(dst, 8, SCRATCH1, SZ_W);
			dst = mov_rr(dst, opts->regs[Z80_A], SCRATCH1, SZ_B);
		} else {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_W);
		}
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
		dst = call(dst, (uint8_t *)z80_write_word_highfirst);
		//no call to save_z80_reg needed since there's no chance we'll use the only
		//the upper half of a register pair
		break;
	case Z80_POP:
		dst = zcycles(dst, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 8 : 4);
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		if (inst->reg == Z80_AF) {
			dst = mov_rr(dst, SCRATCH1, opts->regs[Z80_A], SZ_B);
			dst = bt_ir(dst, 8, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
			dst = bt_ir(dst, 9, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_N));
			dst = bt_ir(dst, 10, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_PV));
			dst = bt_ir(dst, 12, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_H));
			dst = bt_ir(dst, 14, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_Z));
			dst = bt_ir(dst, 15, SCRATCH1, SZ_W);
			dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_S));
		} else {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = mov_rr(dst, SCRATCH1, src_op.base, SZ_W);
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
		dst = zcycles(dst, cycles);
		if (inst->addr_mode == Z80_REG) {
			if(inst->reg == Z80_AF) {
				dst = mov_rr(dst, opts->regs[Z80_A], SCRATCH1, SZ_B);
				dst = mov_rdisp8r(dst, CONTEXT, zar_off(Z80_A), opts->regs[Z80_A], SZ_B);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, zar_off(Z80_A), SZ_B);
		
				//Flags are currently word aligned, so we can move
				//them efficiently a word at a time
				for (int f = ZF_C; f < ZF_NUM; f+=2) {
					dst = mov_rdisp8r(dst, CONTEXT, zf_off(f), SCRATCH1, SZ_W);
					dst = mov_rdisp8r(dst, CONTEXT, zaf_off(f), SCRATCH2, SZ_W);
					dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, zaf_off(f), SZ_W);
					dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, zf_off(f), SZ_W);
				}
			} else {
				dst = xchg_rr(dst, opts->regs[Z80_DE], opts->regs[Z80_HL], SZ_W);
			}
		} else {
			dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_read_byte);
			dst = xchg_rr(dst, opts->regs[inst->reg], SCRATCH1, SZ_B);
			dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
			dst = call(dst, (uint8_t *)z80_write_byte);
			dst = zcycles(dst, 1);
			uint8_t high_reg = z80_high_reg(inst->reg);
			uint8_t use_reg;
			//even though some of the upper halves can be used directly
			//the limitations on mixing *H regs with the REX prefix
			//prevent us from taking advantage of it
			use_reg = opts->regs[inst->reg];
			dst = ror_ir(dst, 8, use_reg, SZ_W);
			dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH1, SZ_W);
			dst = add_ir(dst, 1, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_read_byte);
			dst = xchg_rr(dst, use_reg, SCRATCH1, SZ_B);
			dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
			dst = add_ir(dst, 1, SCRATCH2, SZ_W);
			dst = call(dst, (uint8_t *)z80_write_byte);
			//restore reg to normal rotation
			dst = ror_ir(dst, 8, use_reg, SZ_W);
			dst = zcycles(dst, 2);
		}
		break;
	case Z80_EXX:
		dst = zcycles(dst, 4);
		dst = mov_rr(dst, opts->regs[Z80_BC], SCRATCH1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_HL], SCRATCH2, SZ_W);
		dst = mov_rdisp8r(dst, CONTEXT, zar_off(Z80_C), opts->regs[Z80_BC], SZ_W);
		dst = mov_rdisp8r(dst, CONTEXT, zar_off(Z80_L), opts->regs[Z80_HL], SZ_W);
		dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, zar_off(Z80_C), SZ_W);
		dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, zar_off(Z80_L), SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_DE], SCRATCH1, SZ_W);
		dst = mov_rdisp8r(dst, CONTEXT, zar_off(Z80_E), opts->regs[Z80_DE], SZ_W);
		dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, zar_off(Z80_E), SZ_W);
		break;
	/*case Z80_LDI:
	case Z80_LDIR:
	case Z80_LDD:
	case Z80_LDDR:
	case Z80_CPI:
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = add_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = add_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = adc_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = adc_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = sub_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = sub_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, CONTEXT, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = sbb_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = sbb_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = and_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = and_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = or_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = or_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = add_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = add_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		//TODO: Cleanup flags
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = cmp_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = cmp_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, CONTEXT, zf_off(ZF_N), SZ_B);
		dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
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
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		if (dst_op.mode == MODE_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
		}
		dst = add_ir(dst, 1, dst_op.base, z80_size(inst));
		if (z80_size(inst) == SZ_B) {
			dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
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
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		if (dst_op.mode == MODE_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
		}
		dst = sub_ir(dst, 1, dst_op.base, z80_size(inst));
		if (z80_size(inst) == SZ_B) {
			dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
			//TODO: Implement half-carry flag
			dst = setcc_rdisp8(dst, CC_O, CONTEXT, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	/*case Z80_DAA:
	case Z80_CPL:
	case Z80_NEG:*/
	case Z80_CCF:
		dst = zcycles(dst, 4);
		dst = xor_irdisp8(dst, 1, CONTEXT, zf_off(ZF_C), SZ_B);
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_SCF:
		dst = zcycles(dst, 4);
		dst = mov_irdisp8(dst, 1, CONTEXT, zf_off(ZF_C), SZ_B);
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		break;
	case Z80_NOP:
		if (inst->immed == 42) {
			dst = call(dst, (uint8_t *)z80_save_context);
			dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
			dst = jmp(dst, (uint8_t *)z80_print_regs_exit);
		} else {
			dst = zcycles(dst, 4 * inst->immed);
		}
		break;
	//case Z80_HALT:
	case Z80_DI:
		dst = zcycles(dst, 4);
		dst = mov_irdisp8(dst, 0, CONTEXT, offsetof(z80_context, iff1), SZ_B);
		dst = mov_irdisp8(dst, 0, CONTEXT, offsetof(z80_context, iff2), SZ_B);
		dst = mov_rdisp8r(dst, CONTEXT, offsetof(z80_context, sync_cycle), ZLIMIT, SZ_D);
		break;
	case Z80_EI:
		//TODO: Implement interrupt enable latency of 1 instruction afer EI
		dst = zcycles(dst, 4);
		dst = mov_irdisp8(dst, 1, CONTEXT, offsetof(z80_context, iff1), SZ_B);
		dst = mov_irdisp8(dst, 1, CONTEXT, offsetof(z80_context, iff2), SZ_B);
		dst = call(dst, (uint8_t *)z80_do_sync);
		break;
	case Z80_IM:
		dst = zcycles(dst, 4);
		dst = mov_irdisp8(dst, inst->immed, CONTEXT, offsetof(z80_context, im), SZ_B);
		break;
	case Z80_RLC:
		cycles = inst->immed == 1 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = zcycles(dst, cycles);
		if (inst->reg == Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = zcycles(dst, 1);
		} else {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = rol_ir(dst, 1, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		if (inst->reg == Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RL:
		cycles = inst->immed == 1 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = zcycles(dst, cycles);
		if (inst->reg == Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = zcycles(dst, 1);
		} else {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = bt_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
		dst = rcl_ir(dst, 1, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		if (inst->reg == Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RRC:
		cycles = inst->immed == 1 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = zcycles(dst, cycles);
		if (inst->reg == Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = zcycles(dst, 1);
		} else {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = ror_ir(dst, 1, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		if (inst->reg == Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	case Z80_RR:
		cycles = inst->immed == 1 ? 4 : (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE ? 16 : 8);
		dst = zcycles(dst, cycles);
		if (inst->reg == Z80_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
			dst = zcycles(dst, 1);
		} else {
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		}
		dst = bt_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
		dst = rcr_ir(dst, 1, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N), SZ_B);
		//TODO: Implement half-carry flag
		dst = cmp_ir(dst, 0, dst_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_P, CONTEXT, zf_off(ZF_PV));
		dst = setcc_rdisp8(dst, CC_Z, CONTEXT, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, CONTEXT, zf_off(ZF_S));
		if (inst->reg == Z80_UNUSED) {
			dst = z80_save_result(dst, inst);
		} else {
			dst = z80_save_reg(dst, inst, opts);
		}
		break;
	/*case Z80_SLA:
	case Z80_SRA:
	case Z80_SLL:
	case Z80_SRL:
	case Z80_RLD:
	case Z80_RRD:*/
	case Z80_BIT:
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = zcycles(dst, cycles);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read at the end of a bit instruction takes 4
			dst = zcycles(dst, 1);
		}
		dst = bt_ir(dst, inst->immed, src_op.base, SZ_B);
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_Z));
		break;
	case Z80_SET:
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = zcycles(dst, cycles);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			dst = zcycles(dst, 1);
		}
		dst = bts_ir(dst, inst->immed, src_op.base, SZ_B);
		if (inst->addr_mode != Z80_REG) {
			dst = z80_save_result(dst, inst);
		}
		break;
	case Z80_RES:
		cycles = (inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) ? 8 : 16;
		dst = zcycles(dst, cycles);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (inst->addr_mode != Z80_REG) {
			//Reads normally take 3 cycles, but the read in the middle of a set instruction takes 4
			dst = zcycles(dst, 1);
		}
		dst = btr_ir(dst, inst->immed, src_op.base, SZ_B);
		if (inst->addr_mode != Z80_REG) {
			dst = z80_save_result(dst, inst);
		}
		break;
	case Z80_JP: {
		cycles = 4;
		if (inst->addr_mode != Z80_REG) {
			cycles += 6;
		} else if(inst->ea_reg == Z80_IX || inst->ea_reg == Z80_IY) {
			cycles += 4;
		}
		dst = zcycles(dst, cycles);
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
				dst = mov_rr(dst, opts->regs[inst->ea_reg], SCRATCH1, SZ_W);
			} else {
				dst = mov_ir(dst, inst->immed, SCRATCH1, SZ_W);
			}
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	}
	case Z80_JPCC: {
		dst = zcycles(dst, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = zcycles(dst, 5);//T States: 5
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
			dst = mov_ir(dst, dest_addr, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	}
	case Z80_JR: {
		dst = zcycles(dst, 12);//T States: 4,3,5
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
			dst = mov_ir(dst, dest_addr, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	}
	case Z80_JRCC: {
		dst = zcycles(dst, 7);//T States: 4,3
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
			break;
		}
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = zcycles(dst, 5);//T States: 5
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
			dst = mov_ir(dst, dest_addr, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	}
	case Z80_DJNZ:
		dst = zcycles(dst, 8);//T States: 5,3
		dst = sub_ir(dst, 1, opts->regs[Z80_B], SZ_B);
		uint8_t *no_jump_off = dst+1;
		dst = jcc(dst, CC_Z, dst+2);
		dst = zcycles(dst, 5);//T States: 5
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
			dst = mov_ir(dst, dest_addr, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		*no_jump_off = dst - (no_jump_off+1);
		break;
	case Z80_CALL: {
		dst = zcycles(dst, 11);//T States: 4,3,4
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 3, SCRATCH1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
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
			dst = mov_ir(dst, inst->immed, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	}
	case Z80_CALLCC:
		dst = zcycles(dst, 10);//T States: 4,3,3 (false case)
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = zcycles(dst, 1);//Last of the above T states takes an extra cycle in the true case
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 3, SCRATCH1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
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
			dst = mov_ir(dst, inst->immed, SCRATCH1, SZ_W);
			dst = call(dst, (uint8_t *)z80_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		*no_call_off = dst - (no_call_off+1);
		break;
	case Z80_RET:
		dst = zcycles(dst, 4);//T States: 4
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case Z80_RETCC: {
		dst = zcycles(dst, 5);//T States: 5
		uint8_t cond = CC_Z;
		switch (inst->reg)
		{
		case Z80_CC_NZ:
			cond = CC_NZ;
		case Z80_CC_Z:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_Z), SZ_B);
			break;
		case Z80_CC_NC:
			cond = CC_NZ;
		case Z80_CC_C:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_C), SZ_B);
			break;
		case Z80_CC_PO:
			cond = CC_NZ;
		case Z80_CC_PE:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_PV), SZ_B);
			break;
		case Z80_CC_P:
		case Z80_CC_M:
			dst = cmp_irdisp8(dst, 0, CONTEXT, zf_off(ZF_S), SZ_B);
			break;
		}
		uint8_t *no_call_off = dst+1;
		dst = jcc(dst, cond, dst+2);
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH1, SZ_W);
		dst = call(dst, (uint8_t *)z80_read_word);//T STates: 3, 3
		dst = add_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = call(dst, (uint8_t *)z80_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		*no_call_off = dst - (no_call_off+1);
		break;
	}
	/*case Z80_RETI:
	case Z80_RETN:*/
	case Z80_RST: {
		//RST is basically CALL to an address in page 0
		dst = zcycles(dst, 5);//T States: 5
		dst = sub_ir(dst, 2, opts->regs[Z80_SP], SZ_W);
		dst = mov_ir(dst, address + 3, SCRATCH1, SZ_W);
		dst = mov_rr(dst, opts->regs[Z80_SP], SCRATCH2, SZ_W);
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
	/*case Z80_IN:
	case Z80_INI:
	case Z80_INIR:
	case Z80_IND:
	case Z80_INDR:
	case Z80_OUT:
	case Z80_OUTI:
	case Z80_OTIR:
	case Z80_OUTD:
	case Z80_OTDR:*/
	default: {
		char disbuf[80];
		z80_disasm(inst, disbuf);
		fprintf(stderr, "unimplemented instruction: %s\n", disbuf);
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
		map = context->banked_code_map + (context->bank_reg << 15);
	} else {
		return NULL;
	}
	if (!map->base || !map->offsets || map->offsets[address] == INVALID_OFFSET) {
		return NULL;
	}
	return map->base + map->offsets[address];
}

uint8_t z80_get_native_inst_size(x86_z80_options * opts, uint32_t address)
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
	x86_z80_options * opts = context->options;
	if (address < 0x4000) {
		address &= 0x1FFF;
		map = context->static_code_map;
		opts->ram_inst_sizes[address] = native_size;
		context->ram_code_flags[(address & 0x1C00) >> 10] |= 1 << ((address & 0x380) >> 7);
		context->ram_code_flags[((address + size) & 0x1C00) >> 10] |= 1 << (((address + size) & 0x380) >> 7);
	} else if (address >= 0x8000) {
		address &= 0x7FFF;
		map = context->banked_code_map + (context->bank_reg << 15);
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
			map = context->banked_code_map + (context->bank_reg << 15);
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
		dst = mov_ir(dst, inst_start, SCRATCH1, SZ_D);
		dst = jmp(dst, (uint8_t *)z80_retrans_stub);
	}
	return context;
}

void * z80_retranslate_inst(uint32_t address, z80_context * context)
{
	x86_z80_options * opts = context->options;
	uint8_t orig_size = z80_get_native_inst_size(opts, address);
	uint8_t * orig_start = z80_get_native_address(context, address);
	uint32_t orig = address;
	address &= 0x1FFF;
	uint8_t * dst = opts->cur_code;
	uint8_t * dst_end = opts->code_end;
	uint8_t *after, *inst = context->mem_pointers[0] + address;
	z80inst instbuf;
	after = z80_decode(inst, &instbuf);
	if (orig_size != ZMAX_NATIVE_SIZE) {
		if (dst_end - dst < ZMAX_NATIVE_SIZE) {
			size_t size = 1024*1024;
			dst = alloc_code(&size);
			opts->code_end = dst_end = dst + size;
			opts->cur_code = dst;
		}
		uint8_t * native_end = translate_z80inst(&instbuf, dst, context, address);
		if ((native_end - dst) <= orig_size) {
			native_end = translate_z80inst(&instbuf, orig_start, context, address);
			while (native_end < orig_start + orig_size) {
				*(native_end++) = 0x90; //NOP
			}
			return orig_start;
		} else {
			z80_map_native_address(context, address, dst, after-inst, ZMAX_NATIVE_SIZE);
			opts->code_end = dst+ZMAX_NATIVE_SIZE;
			if(!(instbuf.op == Z80_RET || instbuf.op == Z80_RETI || instbuf.op == Z80_RETN || instbuf.op == Z80_JP || (instbuf.op == Z80_NOP && instbuf.immed == 42))) {
				jmp(native_end, z80_get_native_address(context, address + after-inst));
			}
			return dst;
		}
	} else {
		dst = translate_z80inst(&instbuf, orig_start, context, address);
		if(!(instbuf.op == Z80_RET || instbuf.op == Z80_RETI || instbuf.op == Z80_RETN || instbuf.op == Z80_JP || (instbuf.op == Z80_NOP && instbuf.immed == 42))) {
			dst = jmp(dst, z80_get_native_address(context, address + after-inst));
		}
		return orig_start;
	}
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

void translate_z80_stream(z80_context * context, uint32_t address)
{
	char disbuf[80];
	if (z80_get_native_address(context, address)) {
		return;
	}
	x86_z80_options * opts = context->options;
	uint8_t * encoded = NULL, *next;
	if (address < 0x4000) {
		encoded = context->mem_pointers[0] + (address & 0x1FFF);
	} else if(address >= 0x8000 && context->mem_pointers[1]) {
		encoded = context->mem_pointers[1] + (address & 0x7FFF);
	}
	while (encoded != NULL)
	{
		z80inst inst;
		printf("translating Z80 code at address %X\n", address);
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
			z80_disasm(&inst, disbuf);
			if (inst.op == Z80_NOP) {
				printf("%X\t%s(%d)\n", address, disbuf, inst.immed);
			} else {
				printf("%X\t%s\n", address, disbuf);
			}
			uint8_t *after = translate_z80inst(&inst, opts->cur_code, context, address);
			z80_map_native_address(context, address, opts->cur_code, next-encoded, after - opts->cur_code);
			opts->cur_code = after;
			address += next-encoded;
			if (address > 0xFFFF) {
				address &= 0xFFFF;
				
			} else {
				encoded = next;
			}
		} while (!(inst.op == Z80_RET || inst.op == Z80_RETI || inst.op == Z80_RETN || inst.op == Z80_JP || (inst.op == Z80_NOP && inst.immed == 42)));
		process_deferred(&opts->deferred, context, (native_addr_func)z80_get_native_address);
		if (opts->deferred) {
			address = opts->deferred->address;
			printf("defferred address: %X\n", address);
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

void init_x86_z80_opts(x86_z80_options * options)
{
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
	size_t size = 1024 * 1024;
	options->cur_code = alloc_code(&size);
	options->code_end = options->cur_code + size;
	options->ram_inst_sizes = malloc(sizeof(uint8_t) * 0x2000);
	memset(options->ram_inst_sizes, 0, sizeof(uint8_t) * 0x2000);
	options->deferred = NULL;
}

void init_z80_context(z80_context * context, x86_z80_options * options)
{
	memset(context, 0, sizeof(*context));
	context->static_code_map = malloc(sizeof(context->static_code_map));
	context->static_code_map->offsets = malloc(sizeof(int32_t) * 0x2000);
	memset(context->static_code_map->offsets, 0xFF, sizeof(int32_t) * 0x2000);
	context->banked_code_map = malloc(sizeof(native_map_slot) * (1 << 9));
	context->options = options;
}

void z80_reset(z80_context * context)
{
	context->native_pc = z80_get_native_address_trans(context, 0);
}


