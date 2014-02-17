/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gen_x86.h"
#include "m68k_to_x86.h"
#include "68kinst.h"
#include "mem.h"
#include "x86_backend.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BUS 4
#define PREDEC_PENALTY 2
#define CYCLES RAX
#define LIMIT RBP
#define SCRATCH1 RCX
#define SCRATCH2 RDI
#define CONTEXT RSI

#define FLAG_N RBX
#define FLAG_V BH
#define FLAG_Z RDX
#define FLAG_C DH

char disasm_buf[1024];

m68k_context * sync_components(m68k_context * context, uint32_t address);

void handle_cycle_limit();
void m68k_native_addr();
void m68k_native_addr_and_sync();
void m68k_invalid();
void set_sr();
void set_ccr();
void get_sr();
void do_sync();
void bcd_add();
void bcd_sub();

uint8_t * cycles(uint8_t * dst, uint32_t num)
{
	dst = add_ir(dst, num, CYCLES, SZ_D);
	return dst;
}

uint8_t * check_cycles_int(uint8_t * dst, uint32_t address, x86_68k_options * opts)
{
	dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
	uint8_t * jmp_off = dst+1;
	dst = jcc(dst, CC_NC, dst + 7);
	dst = mov_ir(dst, address, SCRATCH1, SZ_D);
	dst = call(dst, opts->handle_cycle_limit_int);
	*jmp_off = dst - (jmp_off+1);
	return dst;
}

uint8_t * check_cycles(uint8_t * dst)
{
	dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
	uint8_t * jmp_off = dst+1;
	dst = jcc(dst, CC_NC, dst + 7);
	dst = call(dst, (uint8_t *)handle_cycle_limit);
	*jmp_off = dst - (jmp_off+1);
	return dst;
}

int8_t native_reg(m68k_op_info * op, x86_68k_options * opts)
{
	if (op->addr_mode == MODE_REG) {
		return opts->dregs[op->params.regs.pri];
	}
	if (op->addr_mode == MODE_AREG) {
		return opts->aregs[op->params.regs.pri];
	}
	return -1;
}

//must be called with an m68k_op_info that uses a register
size_t reg_offset(m68k_op_info *op)
{
	if (op->addr_mode == MODE_REG) {
		return offsetof(m68k_context, dregs) + sizeof(uint32_t) * op->params.regs.pri;
	}
	return offsetof(m68k_context, aregs) + sizeof(uint32_t) * op->params.regs.pri;
}

void print_regs_exit(m68k_context * context)
{
	printf("XNZVC\n%d%d%d%d%d\n", context->flags[0], context->flags[1], context->flags[2], context->flags[3], context->flags[4]);
	for (int i = 0; i < 8; i++) {
		printf("d%d: %X\n", i, context->dregs[i]);
	}
	for (int i = 0; i < 8; i++) {
		printf("a%d: %X\n", i, context->aregs[i]);
	}
	exit(0);
}

uint8_t * translate_m68k_src(m68kinst * inst, x86_ea * ea, uint8_t * out, x86_68k_options * opts)
{
	int8_t reg = native_reg(&(inst->src), opts);
	uint8_t sec_reg;
	int32_t dec_amount,inc_amount;
	if (reg >= 0) {
		ea->mode = MODE_REG_DIRECT;
		if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
			out = movsx_rr(out, reg, SCRATCH1, SZ_W, SZ_D);
			ea->base = SCRATCH1;
		} else {
			ea->base = reg;
		}
		return out;
	}
	switch (inst->src.addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		//We only get one memory parameter, so if the dst operand is a register in memory,
		//we need to copy this to a temp register first
		reg = native_reg(&(inst->dst), opts);
		if (reg >= 0 || inst->dst.addr_mode == MODE_UNUSED || !(inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG)
		    || inst->op == M68K_EXG) {

			ea->mode = MODE_REG_DISPLACE8;
			ea->base = CONTEXT;
			ea->disp = reg_offset(&(inst->src));
		} else {
			if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
				out = movsx_rdisp8r(out, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_W, SZ_D);
			} else {
				out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, inst->extra.size);
			}
			ea->mode = MODE_REG_DIRECT;
			ea->base = SCRATCH1;
			//we're explicitly handling the areg dest here, so we exit immediately
			return out;
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->src.params.regs.pri == 7 ? 2 :1));
		out = cycles(out, PREDEC_PENALTY);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = sub_ir(out, dec_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
		} else {
			out = sub_irdisp8(out, dec_amount, CONTEXT, reg_offset(&(inst->src)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}

		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->src.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				out = add_ir(out, inc_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
			} else {
				out = add_irdisp8(out, inc_amount, CONTEXT, reg_offset(&(inst->src)), SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) ? SCRATCH2 : SCRATCH1;
		break;
	case MODE_AREG_DISPLACE:
		out = cycles(out, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		out = add_ir(out, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_AREG_INDEX_DISP8:
		out = cycles(out, 6);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = add_rr(out, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = add_rr(out, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			out = add_rr(out, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			out = add_ir(out, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_PC_DISPLACE:
		out = cycles(out, BUS);
		out = mov_ir(out, inst->src.params.regs.displacement + inst->address+2, SCRATCH1, SZ_D);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_PC_INDEX_DISP8:
		out = cycles(out, 6);
		out = mov_ir(out, inst->address+2, SCRATCH1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = add_rr(out, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = add_rr(out, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			out = add_rr(out, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			out = add_ir(out, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		if (inst->src.addr_mode == MODE_ABSOLUTE) {
			out = cycles(out, BUS*2);
		} else {
			out = cycles(out, BUS);
		}
		out = mov_ir(out, inst->src.params.immed, SCRATCH1, SZ_D);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->read_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->read_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->read_32);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_IMMEDIATE:
	case MODE_IMMEDIATE_WORD:
		if (inst->variant != VAR_QUICK) {
			out = cycles(out, (inst->extra.size == OPSIZE_LONG && inst->src.addr_mode == MODE_IMMEDIATE) ? BUS*2 : BUS);
		}
		ea->mode = MODE_IMMED;
		ea->disp = inst->src.params.immed;
		if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD && ea->disp & 0x8000) {
			ea->disp |= 0xFFFF0000;
		}
		return out;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
		if (ea->mode == MODE_REG_DIRECT) {
			out = movsx_rr(out, ea->base, SCRATCH1, SZ_W, SZ_D);
		} else {
			out = movsx_rdisp8r(out, ea->base, ea->disp, SCRATCH1, SZ_W, SZ_D);
			ea->mode = MODE_REG_DIRECT;
		}
		ea->base = SCRATCH1;
	}
	return out;
}

uint8_t * translate_m68k_dst(m68kinst * inst, x86_ea * ea, uint8_t * out, x86_68k_options * opts, uint8_t fake_read)
{
	int8_t reg = native_reg(&(inst->dst), opts), sec_reg;
	int32_t dec_amount, inc_amount;
	if (reg >= 0) {
		ea->mode = MODE_REG_DIRECT;
		ea->base = reg;
		return out;
	}
	switch (inst->dst.addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		ea->mode = MODE_REG_DISPLACE8;
		ea->base = CONTEXT;
		ea->disp = reg_offset(&(inst->dst));
		break;
	case MODE_AREG_PREDEC:
		if (inst->src.addr_mode == MODE_AREG_PREDEC) {
			out = push_r(out, SCRATCH1);
		}
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = sub_ir(out, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			out = sub_irdisp8(out, dec_amount, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (fake_read) {
			out = cycles(out, inst->extra.size == OPSIZE_LONG ? 8 : 4);
		} else {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH1, SZ_D);
			} else {
				out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->dst)), SCRATCH1, SZ_D);
			}
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
		}
		if (inst->src.addr_mode == MODE_AREG_PREDEC) {
			//restore src operand to SCRATCH2
			out =pop_r(out, SCRATCH2);
		} else {
			//save reg value in SCRATCH2 so we can use it to save the result in memory later
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
			} else {
				out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
			}
		}

		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				out = add_ir(out, inc_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				out = add_irdisp8(out, inc_amount, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_AREG_DISPLACE:
		out = cycles(out, fake_read ? BUS+(inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : BUS);
		reg = fake_read ? SCRATCH2 : SCRATCH1;
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], reg, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  reg_offset(&(inst->dst)), reg, SZ_D);
		}
		out = add_ir(out, inst->dst.params.regs.displacement, reg, SZ_D);
		if (!fake_read) {
			out = push_r(out, SCRATCH1);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_AREG_INDEX_DISP8:
		out = cycles(out, fake_read ? (6 + inst->extra.size == OPSIZE_LONG ? 8 : 4) : 6);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  reg_offset(&(inst->dst)), SCRATCH1, SZ_D);
		}
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = add_rr(out, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = add_rr(out, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			out = add_rr(out, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			out = add_ir(out, inst->dst.params.regs.displacement, SCRATCH1, SZ_D);
		}
		if (fake_read) {
			out = mov_rr(out, SCRATCH1, SCRATCH2, SZ_D);
		} else {
			out = push_r(out, SCRATCH1);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_PC_DISPLACE:
		out = cycles(out, fake_read ? BUS+(inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : BUS);
		out = mov_ir(out, inst->dst.params.regs.displacement + inst->address+2, fake_read ? SCRATCH2 : SCRATCH1, SZ_D);
		if (!fake_read) {
			out = push_r(out, SCRATCH1);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_PC_INDEX_DISP8:
		out = cycles(out, fake_read ? (6 + inst->extra.size == OPSIZE_LONG ? 8 : 4) : 6);
		out = mov_ir(out, inst->address+2, SCRATCH1, SZ_D);
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = add_rr(out, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = add_rr(out, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					out = add_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					out = movsx_rr(out, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					out = movsx_rdisp8r(out, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			out = add_rr(out, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			out = add_ir(out, inst->dst.params.regs.displacement, SCRATCH1, SZ_D);
		}
		if (fake_read) {
			out = mov_rr(out, SCRATCH1, SCRATCH2, SZ_D);
		} else {
			out = push_r(out, SCRATCH1);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//Add cycles for reading address from instruction stream
		out = cycles(out, (inst->dst.addr_mode == MODE_ABSOLUTE ? BUS*2 : BUS) + (fake_read ? (inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : 0));
		out = mov_ir(out, inst->dst.params.immed, fake_read ? SCRATCH2 : SCRATCH1, SZ_D);
		if (!fake_read) {
			out = push_r(out, SCRATCH1);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				out = call(out, opts->read_8);
				break;
			case OPSIZE_WORD:
				out = call(out, opts->read_16);
				break;
			case OPSIZE_LONG:
				out = call(out, opts->read_32);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		exit(1);
	}
	return out;
}

uint8_t * m68k_save_result(m68kinst * inst, uint8_t * out, x86_68k_options * opts)
{
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG) {
		if (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->src.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
			} else {
				out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
			}
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, opts->write_8);
			break;
		case OPSIZE_WORD:
			out = call(out, opts->write_16);
			break;
		case OPSIZE_LONG:
			out = call(out, opts->write_32_lowfirst);
			break;
		}
	}
	return out;
}

uint8_t * get_native_address(native_map_slot * native_code_map, uint32_t address)
{
	address &= 0xFFFFFF;
	address /= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		return NULL;
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET || native_code_map[chunk].offsets[offset] == EXTENSION_WORD) {
		return NULL;
	}
	return native_code_map[chunk].base + native_code_map[chunk].offsets[offset];
}

uint8_t * get_native_from_context(m68k_context * context, uint32_t address)
{
	return get_native_address(context->native_code_map, address);
}

uint32_t get_instruction_start(native_map_slot * native_code_map, uint32_t address)
{
	address &= 0xFFFFFF;
	address /= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		return 0;
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET) {
		return 0;
	}
	while (native_code_map[chunk].offsets[offset] == EXTENSION_WORD) {
		--address;
		chunk = address / NATIVE_CHUNK_SIZE;
		offset = address % NATIVE_CHUNK_SIZE;
	}
	return address*2;
}

void map_native_address(m68k_context * context, uint32_t address, uint8_t * native_addr, uint8_t size, uint8_t native_size)
{
	native_map_slot * native_code_map = context->native_code_map;
	x86_68k_options * opts = context->options;
	address &= 0xFFFFFF;
	if (address > 0xE00000) {
		context->ram_code_flags[(address & 0xC000) >> 14] |= 1 << ((address & 0x3800) >> 11);
		if (((address & 0x3FFF) + size) & 0xC000) {
			context->ram_code_flags[((address+size) & 0xC000) >> 14] |= 1 << (((address+size) & 0x3800) >> 11);
		}
		uint32_t slot = (address & 0xFFFF)/1024;
		if (!opts->ram_inst_sizes[slot]) {
			opts->ram_inst_sizes[slot] = malloc(sizeof(uint8_t) * 512);
		}
		opts->ram_inst_sizes[slot][((address & 0xFFFF)/2)%512] = native_size;
	}
	address/= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		native_code_map[chunk].base = native_addr;
		native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	native_code_map[chunk].offsets[offset] = native_addr-native_code_map[chunk].base;
	for(address++,size-=2; size; address++,size-=2) {
		chunk = address / NATIVE_CHUNK_SIZE;
		offset = address % NATIVE_CHUNK_SIZE;
		if (!native_code_map[chunk].base) {
			native_code_map[chunk].base = native_addr;
			native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
			memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		}
		native_code_map[chunk].offsets[offset] = EXTENSION_WORD;
	}
}

uint8_t get_native_inst_size(x86_68k_options * opts, uint32_t address)
{
	if (address < 0xE00000) {
		return 0;
	}
	uint32_t slot = (address & 0xFFFF)/1024;
	return opts->ram_inst_sizes[slot][((address & 0xFFFF)/2)%512];
}

uint8_t * translate_m68k_move(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t reg, flags_reg, sec_reg;
	uint8_t dir = 0;
	int32_t offset;
	int32_t inc_amount, dec_amount;
	x86_ea src;
	dst = translate_m68k_src(inst, &src, dst, opts);
	reg = native_reg(&(inst->dst), opts);
	if (inst->dst.addr_mode != MODE_AREG) {
		//update statically set flags
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
	}

	if (inst->dst.addr_mode != MODE_AREG) {
		if (src.mode == MODE_REG_DIRECT) {
			flags_reg = src.base;
		} else {
			if (reg >= 0) {
				flags_reg = reg;
			} else {
				if(src.mode == MODE_REG_DISPLACE8) {
					dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
				} else {
					dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
				}
				src.mode = MODE_REG_DIRECT;
				flags_reg = src.base = SCRATCH1;
			}
		}
	}
	uint8_t size = inst->extra.size;
	switch(inst->dst.addr_mode)
	{
	case MODE_AREG:
		size = OPSIZE_LONG;
	case MODE_REG:
		if (reg >= 0) {
			if (src.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, src.base, reg, size);
			} else if (src.mode == MODE_REG_DISPLACE8) {
				dst = mov_rdisp8r(dst, src.base, src.disp, reg, size);
			} else {
				dst = mov_ir(dst, src.disp, reg, size);
			}
		} else if(src.mode == MODE_REG_DIRECT) {
			dst = mov_rrdisp8(dst, src.base, CONTEXT, reg_offset(&(inst->dst)), size);
		} else {
			dst = mov_irdisp8(dst, src.disp, CONTEXT, reg_offset(&(inst->dst)), size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = sub_ir(dst, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			dst = sub_irdisp8(dst, dec_amount, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				dst = add_ir(dst, inc_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				dst = add_irdisp8(dst, inc_amount, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
			}
		}
		break;
	case MODE_AREG_DISPLACE:
		dst = cycles(dst, BUS);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
		}
		dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		break;
	case MODE_AREG_INDEX_DISP8:
		dst = cycles(dst, 6);//TODO: Check to make sure this is correct
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
		}
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			}
		} else {
			if (src.base == SCRATCH1) {
				dst = push_r(dst, SCRATCH1);
			}
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_D);
			if (src.base == SCRATCH1) {
				dst = pop_r(dst, SCRATCH1);
			}
		}
		if (inst->dst.params.regs.displacement) {
			dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		break;
	case MODE_PC_DISPLACE:
		dst = cycles(dst, BUS);
		dst = mov_ir(dst, inst->dst.params.regs.displacement + inst->address+2, SCRATCH2, SZ_D);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		break;
	case MODE_PC_INDEX_DISP8:
		dst = cycles(dst, 6);//TODO: Check to make sure this is correct
		dst = mov_ir(dst, inst->address, SCRATCH2, SZ_D);
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			}
		} else {
			if (src.base == SCRATCH1) {
				dst = push_r(dst, SCRATCH1);
			}
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_D);
			if (src.base == SCRATCH1) {
				dst = pop_r(dst, SCRATCH1);
			}
		}
		if (inst->dst.params.regs.displacement) {
			dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != SCRATCH1) {
				dst = mov_rr(dst, src.base, SCRATCH1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			dst = mov_rdisp8r(dst, src.base, src.disp, SCRATCH1, inst->extra.size);
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, inst->extra.size);
		}
		if (inst->dst.addr_mode == MODE_ABSOLUTE) {
			dst = cycles(dst, BUS*2);
		} else {
			dst = cycles(dst, BUS);
		}
		dst = mov_ir(dst, inst->dst.params.immed, SCRATCH2, SZ_D);
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, opts->write_8);
			break;
		case OPSIZE_WORD:
			dst = call(dst, opts->write_16);
			break;
		case OPSIZE_LONG:
			dst = call(dst, opts->write_32_highfirst);
			break;
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (move dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		exit(1);
	}

	//add cycles for prefetch
	dst = cycles(dst, BUS);
	return dst;
}

uint8_t * translate_m68k_movem(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t bit,reg,sec_reg;
	uint8_t early_cycles;
	if(inst->src.addr_mode == MODE_REG) {
		//reg to mem
		early_cycles = 8;
		int8_t dir;
		switch (inst->dst.addr_mode)
		{
		case MODE_AREG_INDIRECT:
		case MODE_AREG_PREDEC:
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
			}
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			reg = SCRATCH2;
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
			}
			dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
			}
			sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
			if (inst->dst.params.regs.sec & 1) {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
					}
				}
			} else {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
					}
				}
				dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_D);
			}
			if (inst->dst.params.regs.displacement) {
				dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
			}
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			dst = mov_ir(dst, inst->dst.params.regs.displacement + inst->address+2, SCRATCH2, SZ_D);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			dst = mov_ir(dst, inst->address+2, SCRATCH2, SZ_D);
			sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
			if (inst->dst.params.regs.sec & 1) {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
					}
				}
			} else {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
					}
				}
				dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_D);
			}
			if (inst->dst.params.regs.displacement) {
				dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
			}
			break;
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			dst = mov_ir(dst, inst->dst.params.immed, SCRATCH2, SZ_D);
			break;
		default:
			m68k_disasm(inst, disasm_buf);
			printf("%X: %s\naddress mode %d not implemented (movem dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
			exit(1);
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			reg = 15;
			dir = -1;
		} else {
			reg = 0;
			dir = 1;
		}
		dst = cycles(dst, early_cycles);
		for(bit=0; reg < 16 && reg >= 0; reg += dir, bit++) {
			if (inst->src.params.immed & (1 << bit)) {
				if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
					dst = sub_ir(dst, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, SCRATCH2, SZ_D);
				}
				dst = push_r(dst, SCRATCH2);
				if (reg > 7) {
					if (opts->aregs[reg-8] >= 0) {
						dst = mov_rr(dst, opts->aregs[reg-8], SCRATCH1, inst->extra.size);
					} else {
						dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * (reg-8), SCRATCH1, inst->extra.size);
					}
				} else {
					if (opts->dregs[reg] >= 0) {
						dst = mov_rr(dst, opts->dregs[reg], SCRATCH1, inst->extra.size);
					} else {
						dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t) * (reg), SCRATCH1, inst->extra.size);
					}
				}
				if (inst->extra.size == OPSIZE_LONG) {
					dst = call(dst, opts->write_32_lowfirst);
				} else {
					dst = call(dst, opts->write_16);
				}
				dst = pop_r(dst, SCRATCH2);
				if (inst->dst.addr_mode != MODE_AREG_PREDEC) {
					dst = add_ir(dst, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, SCRATCH2, SZ_D);
				}
			}
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				dst = mov_rr(dst, SCRATCH2, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
			}
		}
	} else {
		//mem to reg
		early_cycles = 4;
		switch (inst->src.addr_mode)
		{
		case MODE_AREG_INDIRECT:
		case MODE_AREG_POSTINC:
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
			}
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			reg = SCRATCH2;
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
			}
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
			}
			sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
			if (inst->src.params.regs.sec & 1) {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
					}
				}
			} else {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
					}
				}
				dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
			}
			if (inst->src.params.regs.displacement) {
				dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
			}
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			dst = mov_ir(dst, inst->src.params.regs.displacement + inst->address+2, SCRATCH1, SZ_D);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
			sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
			if (inst->src.params.regs.sec & 1) {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
					} else {
						dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
					}
				}
			} else {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
					} else {
						dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
					}
				}
				dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
			}
			if (inst->src.params.regs.displacement) {
				dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
			}
			break;
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			dst = mov_ir(dst, inst->src.params.immed, SCRATCH1, SZ_D);
			break;
		default:
			m68k_disasm(inst, disasm_buf);
			printf("%X: %s\naddress mode %d not implemented (movem src)\n", inst->address, disasm_buf, inst->src.addr_mode);
			exit(1);
		}
		dst = cycles(dst, early_cycles);
		for(reg = 0; reg < 16; reg ++) {
			if (inst->dst.params.immed & (1 << reg)) {
				dst = push_r(dst, SCRATCH1);
				if (inst->extra.size == OPSIZE_LONG) {
					dst = call(dst, opts->read_32);
				} else {
					dst = call(dst, opts->read_16);
				}
				if (inst->extra.size == OPSIZE_WORD) {
					dst = movsx_rr(dst, SCRATCH1, SCRATCH1, SZ_W, SZ_D);
				}
				if (reg > 7) {
					if (opts->aregs[reg-8] >= 0) {
						dst = mov_rr(dst, SCRATCH1, opts->aregs[reg-8], SZ_D);
					} else {
						dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * (reg-8), SZ_D);
					}
				} else {
					if (opts->dregs[reg] >= 0) {
						dst = mov_rr(dst, SCRATCH1, opts->dregs[reg], SZ_D);
					} else {
						dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t) * (reg), SZ_D);
					}
				}
				dst = pop_r(dst, SCRATCH1);
				dst = add_ir(dst, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, SCRATCH1, SZ_D);
			}
		}
		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rr(dst, SCRATCH1, opts->aregs[inst->src.params.regs.pri], SZ_D);
			} else {
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->src)), SZ_D);
			}
		}
	}
	//prefetch
	dst = cycles(dst, 4);
	return dst;
}

uint8_t * translate_m68k_clr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	dst = mov_ir(dst, 0, FLAG_N, SZ_B);
	dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	dst = mov_ir(dst, 0, FLAG_C, SZ_B);
	dst = mov_ir(dst, 1, FLAG_Z, SZ_B);
	int8_t reg = native_reg(&(inst->dst), opts);
	if (reg >= 0) {
		dst = cycles(dst, (inst->extra.size == OPSIZE_LONG ? 6 : 4));
		return  xor_rr(dst, reg, reg, inst->extra.size);
	}
	x86_ea dst_op;
	dst = translate_m68k_dst(inst, &dst_op, dst, opts, 1);
	if (dst_op.mode == MODE_REG_DIRECT) {
		dst = xor_rr(dst, dst_op.base, dst_op.base, inst->extra.size);
	} else {
		dst = mov_irdisp8(dst, 0, dst_op.base, dst_op.disp, inst->extra.size);
	}
	dst = m68k_save_result(inst, dst, opts);
	return dst;
}

uint8_t * translate_m68k_ext(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	x86_ea dst_op;
	uint8_t dst_size = inst->extra.size;
	inst->extra.size--;
	dst = translate_m68k_dst(inst, &dst_op, dst, opts, 0);
	if (dst_op.mode == MODE_REG_DIRECT) {
		dst = movsx_rr(dst, dst_op.base, dst_op.base, inst->extra.size, dst_size);
		dst = cmp_ir(dst, 0, dst_op.base, dst_size);
	} else {
		dst = movsx_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH1, inst->extra.size, dst_size);
		dst = cmp_ir(dst, 0, SCRATCH1, dst_size);
		dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, dst_size);
	}
	inst->extra.size = dst_size;
	dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	dst = mov_ir(dst, 0, FLAG_C, SZ_B);
	dst = setcc_r(dst, CC_Z, FLAG_Z);
	dst = setcc_r(dst, CC_S, FLAG_N);
	//M68K EXT only operates on registers so no need for a call to save result here
	return dst;
}

uint8_t * translate_m68k_lea(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t dst_reg = native_reg(&(inst->dst), opts), sec_reg;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		dst = cycles(dst, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			if (dst_reg >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], dst_reg, SZ_D);
			} else {
				dst = mov_rrdisp8(dst, opts->aregs[inst->src.params.regs.pri], CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
		} else {
			if (dst_reg >= 0) {
				dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, dst_reg, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
		}
		break;
	case MODE_AREG_DISPLACE:
		dst = cycles(dst, 8);
		if (dst_reg >= 0) {
			if (inst->src.params.regs.pri != inst->dst.params.regs.pri) {
				if (opts->aregs[inst->src.params.regs.pri] >= 0) {
					dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], dst_reg, SZ_D);
				} else {
					dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), dst_reg, SZ_D);
				}
			}
			dst = add_ir(dst, inst->src.params.regs.displacement, dst_reg, SZ_D);
		} else {
			if (inst->src.params.regs.pri != inst->dst.params.regs.pri) {
				if (opts->aregs[inst->src.params.regs.pri] >= 0) {
					dst = mov_rrdisp8(dst, opts->aregs[inst->src.params.regs.pri], CONTEXT, reg_offset(&(inst->dst)), SZ_D);
				} else {
					dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
					dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
				}
			}
			dst = add_irdisp8(dst, inst->src.params.regs.displacement, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_AREG_INDEX_DISP8:
		dst = cycles(dst, 12);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH2, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH2, SZ_D);
		}
		if (dst_reg >= 0) {
			dst = mov_rr(dst, SCRATCH2, dst_reg, SZ_D);
		} else {
			dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_PC_DISPLACE:
		dst = cycles(dst, 8);
		if (dst_reg >= 0) {
			dst = mov_ir(dst, inst->src.params.regs.displacement + inst->address+2, dst_reg, SZ_D);
		} else {
			dst = mov_irdisp8(dst, inst->src.params.regs.displacement + inst->address+2, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		dst = cycles(dst, BUS*3);
		dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		if (dst_reg >= 0) {
			dst = mov_rr(dst, SCRATCH1, dst_reg, SZ_D);
		} else {
			dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		dst = cycles(dst, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		if (dst_reg >= 0) {
			dst = mov_ir(dst, inst->src.params.immed, dst_reg, SZ_D);
		} else {
			dst = mov_irdisp8(dst, inst->src.params.immed, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * translate_m68k_pea(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t sec_reg;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		dst = cycles(dst, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
		}
		break;
	case MODE_AREG_DISPLACE:
		dst = cycles(dst, 8);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		break;
	case MODE_AREG_INDEX_DISP8:
		dst = cycles(dst, 6);//TODO: Check to make sure this is correct
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		break;
	case MODE_PC_DISPLACE:
		dst = cycles(dst, 8);
		dst = mov_ir(dst, inst->src.params.regs.displacement + inst->address+2, SCRATCH1, SZ_D);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		dst = cycles(dst, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		dst = mov_ir(dst, inst->src.params.immed, SCRATCH1, SZ_D);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_32_lowfirst);
	return dst;
}

uint8_t * translate_m68k_bsr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + (inst->variant == VAR_BYTE ? 2 : 4);
	//TODO: Add cycles in the right place relative to pushing the return address on the stack
	dst = cycles(dst, 10);
	dst = mov_ir(dst, after, SCRATCH1, SZ_D);
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_32_highfirst);
	uint8_t * dest_addr = get_native_address(opts->native_code_map, (inst->address+2) + disp);
	if (!dest_addr) {
		opts->deferred = defer_address(opts->deferred, (inst->address+2) + disp, dst + 1);
		//dummy address to be replaced later
		dest_addr = dst + 256;
	}
	dst = jmp(dst, (char *)dest_addr);
	return dst;
}

uint8_t * translate_m68k_bcc(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	dst = cycles(dst, 10);//TODO: Adjust this for branch not taken case
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + 2;
	uint8_t * dest_addr = get_native_address(opts->native_code_map, after + disp);
	if (inst->extra.cond == COND_TRUE) {
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, after + disp, dst + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 256;
		}
		dst = jmp(dst, dest_addr);
	} else {
		uint8_t cond = CC_NZ;
		switch (inst->extra.cond)
		{
		case COND_HIGH:
			cond = CC_Z;
		case COND_LOW_SAME:
			dst = mov_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_C, SCRATCH1, SZ_B);
			break;
		case COND_CARRY_CLR:
			cond = CC_Z;
		case COND_CARRY_SET:
			dst = cmp_ir(dst, 0, FLAG_C, SZ_B);
			break;
		case COND_NOT_EQ:
			cond = CC_Z;
		case COND_EQ:
			dst = cmp_ir(dst, 0, FLAG_Z, SZ_B);
			break;
		case COND_OVERF_CLR:
			cond = CC_Z;
		case COND_OVERF_SET:
			dst = cmp_ir(dst, 0, FLAG_V, SZ_B);
			break;
		case COND_PLUS:
			cond = CC_Z;
		case COND_MINUS:
			dst = cmp_ir(dst, 0, FLAG_N, SZ_B);
			break;
		case COND_GREATER_EQ:
			cond = CC_Z;
		case COND_LESS:
			dst = cmp_rr(dst, FLAG_N, FLAG_V, SZ_B);
			break;
		case COND_GREATER:
			cond = CC_Z;
		case COND_LESS_EQ:
			dst = mov_rr(dst, FLAG_V, SCRATCH1, SZ_B);
			dst = xor_rr(dst, FLAG_N, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			break;
		}
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, after + disp, dst + 2);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 256;
		}
		dst = jcc(dst, cond, dest_addr);
	}
	return dst;
}

uint8_t * translate_m68k_scc(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t cond = inst->extra.cond;
	x86_ea dst_op;
	inst->extra.size = OPSIZE_BYTE;
	dst = translate_m68k_dst(inst, &dst_op, dst, opts, 1);
	if (cond == COND_TRUE || cond == COND_FALSE) {
		if ((inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG) && inst->extra.cond == COND_TRUE) {
			dst = cycles(dst, 6);
		} else {
			dst = cycles(dst, BUS);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_ir(dst, cond == COND_TRUE ? 0xFF : 0, dst_op.base, SZ_B);
		} else {
			dst = mov_irdisp8(dst, cond == COND_TRUE ? 0xFF : 0, dst_op.base, dst_op.disp, SZ_B);
		}
	} else {
		uint8_t cc = CC_NZ;
		switch (cond)
		{
		case COND_HIGH:
			cc = CC_Z;
		case COND_LOW_SAME:
			dst = mov_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_C, SCRATCH1, SZ_B);
			break;
		case COND_CARRY_CLR:
			cc = CC_Z;
		case COND_CARRY_SET:
			dst = cmp_ir(dst, 0, FLAG_C, SZ_B);
			break;
		case COND_NOT_EQ:
			cc = CC_Z;
		case COND_EQ:
			dst = cmp_ir(dst, 0, FLAG_Z, SZ_B);
			break;
		case COND_OVERF_CLR:
			cc = CC_Z;
		case COND_OVERF_SET:
			dst = cmp_ir(dst, 0, FLAG_V, SZ_B);
			break;
		case COND_PLUS:
			cc = CC_Z;
		case COND_MINUS:
			dst = cmp_ir(dst, 0, FLAG_N, SZ_B);
			break;
		case COND_GREATER_EQ:
			cc = CC_Z;
		case COND_LESS:
			dst = cmp_rr(dst, FLAG_N, FLAG_V, SZ_B);
			break;
		case COND_GREATER:
			cc = CC_Z;
		case COND_LESS_EQ:
			dst = mov_rr(dst, FLAG_V, SCRATCH1, SZ_B);
			dst = xor_rr(dst, FLAG_N, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			break;
		}
		uint8_t *true_off = dst + 1;
		dst = jcc(dst, cc, dst+2);
		dst = cycles(dst, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_ir(dst, 0, dst_op.base, SZ_B);
		} else {
			dst = mov_irdisp8(dst, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		uint8_t *end_off = dst+1;
		dst = jmp(dst, dst+2);
		*true_off = dst - (true_off+1);
		dst = cycles(dst, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_ir(dst, 0xFF, dst_op.base, SZ_B);
		} else {
			dst = mov_irdisp8(dst, 0xFF, dst_op.base, dst_op.disp, SZ_B);
		}
		*end_off = dst - (end_off+1);
	}
	dst = m68k_save_result(inst, dst, opts);
	return dst;
}

uint8_t * translate_m68k_jmp(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * dest_addr, sec_reg;
	uint32_t m68k_addr;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		dst = cycles(dst, BUS*2);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_AREG_INDEX_DISP8:
		dst = cycles(dst, BUS*3);//TODO: CHeck that this is correct
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			//32-bit index register
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			//16-bit index register
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_PC_DISPLACE:
		dst = cycles(dst, 10);
		m68k_addr = inst->src.params.regs.displacement + inst->address + 2;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->deferred = defer_address(opts->deferred, m68k_addr, dst + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = dst + 256;
			}
			dst = jmp(dst, dest_addr);
		} else {
			dst = mov_ir(dst, m68k_addr, SCRATCH1, SZ_D);
			dst = call(dst, (uint8_t *)m68k_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		dst = cycles(dst, BUS*3);//TODO: CHeck that this is correct
		dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		dst = cycles(dst, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		m68k_addr = inst->src.params.immed;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->deferred = defer_address(opts->deferred, m68k_addr, dst + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = dst + 256;
			}
			dst = jmp(dst, dest_addr);
		} else {
			dst = mov_ir(dst, m68k_addr, SCRATCH1, SZ_D);
			dst = call(dst, (uint8_t *)m68k_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%s\naddress mode %d not yet supported (jmp)\n", disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * translate_m68k_jsr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * dest_addr, sec_reg;
	uint32_t after;
	uint32_t m68k_addr;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		dst = cycles(dst, BUS*2);
		dst = mov_ir(dst, inst->address + 2, SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_AREG_DISPLACE:
		dst = cycles(dst, BUS*2);
		dst = mov_ir(dst, inst->address + 4, SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
		}
		dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_AREG_INDEX_DISP8:
		dst = cycles(dst, BUS*3);//TODO: CHeck that this is correct
		dst = mov_ir(dst, inst->address + 4, SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT,  reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_PC_DISPLACE:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		dst = cycles(dst, 10);
		dst = mov_ir(dst, inst->address + 4, SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		m68k_addr = inst->src.params.regs.displacement + inst->address + 2;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->deferred = defer_address(opts->deferred, m68k_addr, dst + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = dst + 256;
			}
			dst = jmp(dst, dest_addr);
		} else {
			dst = mov_ir(dst, m68k_addr, SCRATCH1, SZ_D);
			dst = call(dst, (uint8_t *)m68k_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		dst = cycles(dst, BUS*3);//TODO: CHeck that this is correct
		dst = mov_ir(dst, inst->address + 4, SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->aregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = add_rr(dst, opts->dregs[sec_reg], SCRATCH1, SZ_D);
				} else {
					dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->aregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					dst = movsx_rr(dst, opts->dregs[sec_reg], SCRATCH2, SZ_W, SZ_D);
				} else {
					dst = movsx_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, SCRATCH2, SZ_W, SZ_D);
				}
			}
			dst = add_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		dst = cycles(dst, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		dst = mov_ir(dst, inst->address + (inst->src.addr_mode == MODE_ABSOLUTE ? 6 : 4), SCRATCH1, SZ_D);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, opts->write_32_highfirst);
		m68k_addr = inst->src.params.immed;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->deferred = defer_address(opts->deferred, m68k_addr, dst + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = dst + 256;
			}
			dst = jmp(dst, dest_addr);
		} else {
			dst = mov_ir(dst, m68k_addr, SCRATCH1, SZ_D);
			dst = call(dst, (uint8_t *)m68k_native_addr);
			dst = jmp_r(dst, SCRATCH1);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%s\naddress mode %d not yet supported (jsr)\n", disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * translate_m68k_rts(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	//TODO: Add cycles
	dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
	dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = call(dst, opts->read_32);
	dst = call(dst, (uint8_t *)m68k_native_addr);
	dst = jmp_r(dst, SCRATCH1);
	return dst;
}

uint8_t * translate_m68k_dbcc(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	//best case duration
	dst = cycles(dst, 10);
	uint8_t * skip_loc = NULL;
	//TODO: Check if COND_TRUE technically valid here even though
	//it's basically a slow NOP
	if (inst->extra.cond != COND_FALSE) {
		uint8_t cond = CC_NZ;
		switch (inst->extra.cond)
		{
		case COND_HIGH:
			cond = CC_Z;
		case COND_LOW_SAME:
			dst = mov_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_C, SCRATCH1, SZ_B);
			break;
		case COND_CARRY_CLR:
			cond = CC_Z;
		case COND_CARRY_SET:
			dst = cmp_ir(dst, 0, FLAG_C, SZ_B);
			break;
		case COND_NOT_EQ:
			cond = CC_Z;
		case COND_EQ:
			dst = cmp_ir(dst, 0, FLAG_Z, SZ_B);
			break;
		case COND_OVERF_CLR:
			cond = CC_Z;
		case COND_OVERF_SET:
			dst = cmp_ir(dst, 0, FLAG_V, SZ_B);
			break;
		case COND_PLUS:
			cond = CC_Z;
		case COND_MINUS:
			dst = cmp_ir(dst, 0, FLAG_N, SZ_B);
			break;
		case COND_GREATER_EQ:
			cond = CC_Z;
		case COND_LESS:
			dst = cmp_rr(dst, FLAG_N, FLAG_V, SZ_B);
			break;
		case COND_GREATER:
			cond = CC_Z;
		case COND_LESS_EQ:
			dst = mov_rr(dst, FLAG_V, SCRATCH1, SZ_B);
			dst = xor_rr(dst, FLAG_N, SCRATCH1, SZ_B);
			dst = or_rr(dst, FLAG_Z, SCRATCH1, SZ_B);
			break;
		}
		skip_loc = dst + 1;
		dst = jcc(dst, cond, dst + 2);
	}
	if (opts->dregs[inst->dst.params.regs.pri] >= 0) {
		dst = sub_ir(dst, 1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
		dst = cmp_ir(dst, -1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
	} else {
		dst = sub_irdisp8(dst, 1, CONTEXT, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
		dst = cmp_irdisp8(dst, -1, CONTEXT, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
	}
	uint8_t *loop_end_loc = dst+1;
	dst = jcc(dst, CC_Z, dst+2);
	uint32_t after = inst->address + 2;
	uint8_t * dest_addr = get_native_address(opts->native_code_map, after + inst->src.params.immed);
	if (!dest_addr) {
		opts->deferred = defer_address(opts->deferred, after + inst->src.params.immed, dst + 1);
		//dummy address to be replaced later, make sure it generates a 4-byte displacement
		dest_addr = dst + 256;
	}
	dst = jmp(dst, dest_addr);
	*loop_end_loc = dst - (loop_end_loc+1);
	if (skip_loc) {
		dst = cycles(dst, 2);
		*skip_loc = dst - (skip_loc+1);
		dst = cycles(dst, 2);
	} else {
		dst = cycles(dst, 4);
	}
	return dst;
}

uint8_t * translate_m68k_link(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t reg = native_reg(&(inst->src), opts);
	//compensate for displacement word
	dst = cycles(dst, BUS);
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	if (reg >= 0) {
		dst = mov_rr(dst, reg, SCRATCH1, SZ_D);
	} else {
		dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
	}
	dst = call(dst, opts->write_32_highfirst);
	if (reg >= 0) {
		dst = mov_rr(dst, opts->aregs[7], reg, SZ_D);
	} else {
		dst = mov_rrdisp8(dst, opts->aregs[7], CONTEXT, reg_offset(&(inst->src)), SZ_D);
	}
	dst = add_ir(dst, inst->dst.params.immed, opts->aregs[7], SZ_D);
	//prefetch
	dst = cycles(dst, BUS);
	return dst;
}

uint8_t * translate_m68k_movep(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t reg;
	dst = cycles(dst, BUS*2);
	if (inst->src.addr_mode == MODE_REG) {
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			dst = add_ir(dst, inst->dst.params.regs.displacement, SCRATCH2, SZ_D);
		}
		reg = native_reg(&(inst->src), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				dst = mov_rr(dst, reg, SCRATCH1, SZ_D);
				dst = shr_ir(dst, 24, SCRATCH1, SZ_D);
				dst = push_r(dst, SCRATCH2);
				dst = call(dst, opts->write_8);
				dst = pop_r(dst, SCRATCH2);
				dst = mov_rr(dst, reg, SCRATCH1, SZ_D);
				dst = shr_ir(dst, 16, SCRATCH1, SZ_D);

			} else {
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src))+3, SCRATCH1, SZ_B);
				dst = push_r(dst, SCRATCH2);
				dst = call(dst, opts->write_8);
				dst = pop_r(dst, SCRATCH2);
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src))+2, SCRATCH1, SZ_B);
			}
			dst = add_ir(dst, 2, SCRATCH2, SZ_D);
			dst = push_r(dst, SCRATCH2);
			dst = call(dst, opts->write_8);
			dst = pop_r(dst, SCRATCH2);
			dst = add_ir(dst, 2, SCRATCH2, SZ_D);
		}
		if (reg >= 0) {
			dst = mov_rr(dst, reg, SCRATCH1, SZ_W);
			dst = shr_ir(dst, 8, SCRATCH1, SZ_W);
			dst = push_r(dst, SCRATCH2);
			dst = call(dst, opts->write_8);
			dst = pop_r(dst, SCRATCH2);
			dst = mov_rr(dst, reg, SCRATCH1, SZ_W);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src))+1, SCRATCH1, SZ_B);
			dst = push_r(dst, SCRATCH2);
			dst = call(dst, opts->write_8);
			dst = pop_r(dst, SCRATCH2);
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_B);
		}
		dst = add_ir(dst, 2, SCRATCH2, SZ_D);
		dst = call(dst, opts->write_8);
	} else {
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			dst = add_ir(dst, inst->src.params.regs.displacement, SCRATCH1, SZ_D);
		}
		reg = native_reg(&(inst->dst), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				dst = push_r(dst, SCRATCH1);
				dst = call(dst, opts->read_8);
				dst = shl_ir(dst, 24, SCRATCH1, SZ_D);
				dst = mov_rr(dst, SCRATCH1, reg, SZ_D);
				dst = pop_r(dst, SCRATCH1);
				dst = add_ir(dst, 2, SCRATCH1, SZ_D);
				dst = push_r(dst, SCRATCH1);
				dst = call(dst, opts->read_8);
				dst = shl_ir(dst, 16, SCRATCH1, SZ_D);
				dst = or_rr(dst, SCRATCH1, reg, SZ_D);
			} else {
				dst = push_r(dst, SCRATCH1);
				dst = call(dst, opts->read_8);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst))+3, SZ_B);
				dst = pop_r(dst, SCRATCH1);
				dst = add_ir(dst, 2, SCRATCH1, SZ_D);
				dst = push_r(dst, SCRATCH1);
				dst = call(dst, opts->read_8);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst))+2, SZ_B);
			}
			dst = pop_r(dst, SCRATCH1);
			dst = add_ir(dst, 2, SCRATCH1, SZ_D);
		}
		dst = push_r(dst, SCRATCH1);
		dst = call(dst, opts->read_8);
		if (reg >= 0) {

			dst = shl_ir(dst, 8, SCRATCH1, SZ_W);
			dst = mov_rr(dst, SCRATCH1, reg, SZ_W);
			dst = pop_r(dst, SCRATCH1);
			dst = add_ir(dst, 2, SCRATCH1, SZ_D);
			dst = call(dst, opts->read_8);
			dst = mov_rr(dst, SCRATCH1, reg, SZ_B);
		} else {
			dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst))+1, SZ_B);
			dst = pop_r(dst, SCRATCH1);
			dst = add_ir(dst, 2, SCRATCH1, SZ_D);
			dst = call(dst, opts->read_8);
			dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst)), SZ_B);
		}
	}
	return dst;
}

uint8_t * translate_m68k_cmp(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t size = inst->extra.size;
	x86_ea src_op, dst_op;
	dst = translate_m68k_src(inst, &src_op, dst, opts);
	if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
		dst = push_r(dst, SCRATCH1);
		dst = translate_m68k_dst(inst, &dst_op, dst, opts, 0);
		dst = pop_r(dst, SCRATCH2);
		src_op.base = SCRATCH2;
	} else {
		dst = translate_m68k_dst(inst, &dst_op, dst, opts, 0);
		if (inst->dst.addr_mode == MODE_AREG && size == OPSIZE_WORD) {
			size = OPSIZE_LONG;
		}
	}
	dst = cycles(dst, BUS);
	if (src_op.mode == MODE_REG_DIRECT) {
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = cmp_rr(dst, src_op.base, dst_op.base, size);
		} else {
			dst = cmp_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
		}
	} else if (src_op.mode == MODE_REG_DISPLACE8) {
		dst = cmp_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, size);
	} else {
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = cmp_ir(dst, src_op.disp, dst_op.base, size);
		} else {
			dst = cmp_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, size);
		}
	}
	dst = setcc_r(dst, CC_C, FLAG_C);
	dst = setcc_r(dst, CC_Z, FLAG_Z);
	dst = setcc_r(dst, CC_S, FLAG_N);
	dst = setcc_r(dst, CC_O, FLAG_V);
	return dst;
}

typedef uint8_t * (*shift_ir_t)(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
typedef uint8_t * (*shift_irdisp8_t)(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
typedef uint8_t * (*shift_clr_t)(uint8_t * out, uint8_t dst, uint8_t size);
typedef uint8_t * (*shift_clrdisp8_t)(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size);

uint8_t * translate_shift(uint8_t * dst, m68kinst * inst, x86_ea *src_op, x86_ea * dst_op, x86_68k_options * opts, shift_ir_t shift_ir, shift_irdisp8_t shift_irdisp8, shift_clr_t shift_clr, shift_clrdisp8_t shift_clrdisp8, shift_ir_t special, shift_irdisp8_t special_disp8)
{
	uint8_t * end_off = NULL;
	uint8_t * nz_off = NULL;
	uint8_t * z_off = NULL;
	if (inst->src.addr_mode == MODE_UNUSED) {
		dst = cycles(dst, BUS);
		//Memory shift
		dst = shift_ir(dst, 1, dst_op->base, SZ_W);
	} else {
		dst = cycles(dst, inst->extra.size == OPSIZE_LONG ? 8 : 6);
		if (src_op->mode == MODE_IMMED) {
			if (src_op->disp != 1 && inst->op == M68K_ASL) {
				dst = mov_ir(dst, 0, FLAG_V, SZ_B);
				for (int i = 0; i < src_op->disp; i++) {
					if (dst_op->mode == MODE_REG_DIRECT) {
						dst = shift_ir(dst, 1, dst_op->base, inst->extra.size);
					} else {
						dst = shift_irdisp8(dst, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}
					//dst = setcc_r(dst, CC_O, FLAG_V);
					dst = jcc(dst, CC_NO, dst+4);
					dst = mov_ir(dst, 1, FLAG_V, SZ_B);
				}
			} else {
				if (dst_op->mode == MODE_REG_DIRECT) {
					dst = shift_ir(dst, src_op->disp, dst_op->base, inst->extra.size);
				} else {
					dst = shift_irdisp8(dst, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
				}
				dst = setcc_r(dst, CC_O, FLAG_V);
			}
		} else {
			if (src_op->base != RCX) {
				if (src_op->mode == MODE_REG_DIRECT) {
					dst = mov_rr(dst, src_op->base, RCX, SZ_B);
				} else {
					dst = mov_rdisp8r(dst, src_op->base, src_op->disp, RCX, SZ_B);
				}

			}
			dst = and_ir(dst, 63, RCX, SZ_D);
			nz_off = dst+1;
			dst = jcc(dst, CC_NZ, dst+2);
			//Flag behavior for shift count of 0 is different for x86 than 68K
			if (dst_op->mode == MODE_REG_DIRECT) {
				dst = cmp_ir(dst, 0, dst_op->base, inst->extra.size);
			} else {
				dst = cmp_irdisp8(dst, 0, dst_op->base, dst_op->disp, inst->extra.size);
			}
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = mov_ir(dst, 0, FLAG_C, SZ_B);
			//For other instructions, this flag will be set below
			if (inst->op == M68K_ASL) {
				dst = mov_ir(dst, 0, FLAG_V, SZ_B);
			}
			z_off = dst+1;
			dst = jmp(dst, dst+2);
			*nz_off = dst - (nz_off + 1);
			//add 2 cycles for every bit shifted
			dst = add_rr(dst, RCX, CYCLES, SZ_D);
			dst = add_rr(dst, RCX, CYCLES, SZ_D);
			if (inst->op == M68K_ASL) {
				//ASL has Overflow flag behavior that depends on all of the bits shifted through the MSB
				//Easiest way to deal with this is to shift one bit at a time
				dst = mov_ir(dst, 0, FLAG_V, SZ_B);
				uint8_t * loop_start = dst;
				if (dst_op->mode == MODE_REG_DIRECT) {
					dst = shift_ir(dst, 1, dst_op->base, inst->extra.size);
				} else {
					dst = shift_irdisp8(dst, 1, dst_op->base, dst_op->disp, inst->extra.size);
				}
				//dst = setcc_r(dst, CC_O, FLAG_V);
				dst = jcc(dst, CC_NO, dst+4);
				dst = mov_ir(dst, 1, FLAG_V, SZ_B);
				dst = loop(dst, loop_start);
			} else {
				//x86 shifts modulo 32 for operand sizes less than 64-bits
				//but M68K shifts modulo 64, so we need to check for large shifts here
				dst = cmp_ir(dst, 32, RCX, SZ_B);
				uint8_t * norm_shift_off = dst + 1;
				dst = jcc(dst, CC_L, dst+2);
				if (special) {
					if (inst->extra.size == OPSIZE_LONG) {
						uint8_t * neq_32_off = dst + 1;
						dst = jcc(dst, CC_NZ, dst+2);

						//set the carry bit to the lsb
						if (dst_op->mode == MODE_REG_DIRECT) {
							dst = special(dst, 1, dst_op->base, SZ_D);
						} else {
							dst = special_disp8(dst, 1, dst_op->base, dst_op->disp, SZ_D);
						}
						dst = setcc_r(dst, CC_C, FLAG_C);
						dst = jmp(dst, dst+4);
						*neq_32_off = dst - (neq_32_off+1);
					}
					dst = mov_ir(dst, 0, FLAG_C, SZ_B);
					dst = mov_ir(dst, 1, FLAG_Z, SZ_B);
					dst = mov_ir(dst, 0, FLAG_N, SZ_B);
					if (dst_op->mode == MODE_REG_DIRECT) {
						dst = xor_rr(dst, dst_op->base, dst_op->base, inst->extra.size);
					} else {
						dst = mov_irdisp8(dst, 0, dst_op->base, dst_op->disp, inst->extra.size);
					}
				} else {
					if (dst_op->mode == MODE_REG_DIRECT) {
						dst = shift_ir(dst, 31, dst_op->base, inst->extra.size);
						dst = shift_ir(dst, 1, dst_op->base, inst->extra.size);
					} else {
						dst = shift_irdisp8(dst, 31, dst_op->base, dst_op->disp, inst->extra.size);
						dst = shift_irdisp8(dst, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}

				}
				end_off = dst+1;
				dst = jmp(dst, dst+2);
				*norm_shift_off = dst - (norm_shift_off+1);
				if (dst_op->mode == MODE_REG_DIRECT) {
					dst = shift_clr(dst, dst_op->base, inst->extra.size);
				} else {
					dst = shift_clrdisp8(dst, dst_op->base, dst_op->disp, inst->extra.size);
				}
			}
		}

	}
	if (!special && end_off) {
		*end_off = dst - (end_off + 1);
	}
	dst = setcc_r(dst, CC_C, FLAG_C);
	dst = setcc_r(dst, CC_Z, FLAG_Z);
	dst = setcc_r(dst, CC_S, FLAG_N);
	if (special && end_off) {
		*end_off = dst - (end_off + 1);
	}
	//set X flag to same as C flag
	dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
	if (z_off) {
		*z_off = dst - (z_off + 1);
	}
	if (inst->op != M68K_ASL) {
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	}
	if (inst->src.addr_mode == MODE_UNUSED) {
		dst = m68k_save_result(inst, dst, opts);
	}
	return dst;
}

#define BIT_SUPERVISOR 5

uint8_t * translate_m68k(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * end_off, *zero_off, *norm_off;
	uint8_t dst_reg;
	dst = check_cycles_int(dst, inst->address, opts);
	if (inst->op == M68K_MOVE) {
		return translate_m68k_move(dst, inst, opts);
	} else if(inst->op == M68K_LEA) {
		return translate_m68k_lea(dst, inst, opts);
	} else if(inst->op == M68K_PEA) {
		return translate_m68k_pea(dst, inst, opts);
	} else if(inst->op == M68K_BSR) {
		return translate_m68k_bsr(dst, inst, opts);
	} else if(inst->op == M68K_BCC) {
		return translate_m68k_bcc(dst, inst, opts);
	} else if(inst->op == M68K_JMP) {
		return translate_m68k_jmp(dst, inst, opts);
	} else if(inst->op == M68K_JSR) {
		return translate_m68k_jsr(dst, inst, opts);
	} else if(inst->op == M68K_RTS) {
		return translate_m68k_rts(dst, inst, opts);
	} else if(inst->op == M68K_DBCC) {
		return translate_m68k_dbcc(dst, inst, opts);
	} else if(inst->op == M68K_CLR) {
		return translate_m68k_clr(dst, inst, opts);
	} else if(inst->op == M68K_MOVEM) {
		return translate_m68k_movem(dst, inst, opts);
	} else if(inst->op == M68K_LINK) {
		return translate_m68k_link(dst, inst, opts);
	} else if(inst->op == M68K_EXT) {
		return translate_m68k_ext(dst, inst, opts);
	} else if(inst->op == M68K_SCC) {
		return translate_m68k_scc(dst, inst, opts);
	} else if(inst->op == M68K_MOVEP) {
		return translate_m68k_movep(dst, inst, opts);
	} else if(inst->op == M68K_INVALID) {
		if (inst->src.params.immed == 0x7100) {
			return retn(dst);
		}
		dst = mov_ir(dst, inst->address, SCRATCH1, SZ_D);
		return call(dst, (uint8_t *)m68k_invalid);
	} else if(inst->op == M68K_CMP) {
		return translate_m68k_cmp(dst, inst, opts);
	}
	x86_ea src_op, dst_op;
	if (inst->src.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_src(inst, &src_op, dst, opts);
	}
	if (inst->dst.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_dst(inst, &dst_op, dst, opts, 0);
	}
	uint8_t size;
	switch(inst->op)
	{
	case M68K_ABCD:
		if (src_op.base != SCRATCH2) {
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, src_op.base, SCRATCH2, SZ_B);
			} else {
				dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_B);
			}
		}
		if (dst_op.base != SCRATCH1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, dst_op.base, SCRATCH1, SZ_B);
			} else {
				dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH1, SZ_B);
			}
		}
		dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
		dst = jcc(dst, CC_NC, dst+5);
		dst = add_ir(dst, 1, SCRATCH1, SZ_B);
		dst = call(dst, (uint8_t *)bcd_add);
		dst = mov_rr(dst, CH, FLAG_C, SZ_B);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = cmp_ir(dst, 0, SCRATCH1, SZ_B);
		dst = jcc(dst, CC_Z, dst+4);
		dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		if (dst_op.base != SCRATCH1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, SCRATCH1, dst_op.base, SZ_B);
			} else {
				dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_B);
			}
		}
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ADD:
		dst = cycles(dst, BUS);
		size = inst->dst.addr_mode == MODE_AREG ? OPSIZE_LONG : inst->extra.size;
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = add_rr(dst, src_op.base, dst_op.base, size);
			} else {
				dst = add_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = add_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = add_ir(dst, src_op.disp, dst_op.base, size);
			} else {
				dst = add_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, size);
			}
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = setcc_r(dst, CC_C, FLAG_C);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = setcc_r(dst, CC_O, FLAG_V);
			dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		}
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ADDX:
		dst = cycles(dst, BUS);
		dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = adc_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = adc_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = adc_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = adc_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = adc_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = jcc(dst, CC_Z, dst+4);
		dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_AND:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = and_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = and_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = and_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = and_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = and_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ANDI_CCR:
	case M68K_ANDI_SR:
		dst = cycles(dst, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (!(inst->src.params.immed & 0x1)) {
			dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		}
		if (!(inst->src.params.immed & 0x2)) {
			dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		}
		if (!(inst->src.params.immed & 0x4)) {
			dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		}
		if (!(inst->src.params.immed & 0x8)) {
			dst = mov_ir(dst, 0, FLAG_N, SZ_B);
		}
		if (!(inst->src.params.immed & 0x10)) {
			dst = mov_irind(dst, 0, CONTEXT, SZ_B);
		}
		if (inst->op == M68K_ANDI_SR) {
			dst = and_irdisp8(dst, inst->src.params.immed >> 8, CONTEXT, offsetof(m68k_context, status), SZ_B);
			if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
				//leave supervisor mode
				dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_B);
				dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_B);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_B);
			}
			if (inst->src.params.immed & 0x700) {
				dst = call(dst, (uint8_t *)do_sync);
			}
		}
		break;
	case M68K_ASL:
	case M68K_LSL:
		dst = translate_shift(dst, inst, &src_op, &dst_op, opts, shl_ir, shl_irdisp8, shl_clr, shl_clrdisp8, shr_ir, shr_irdisp8);
		break;
	case M68K_ASR:
		dst = translate_shift(dst, inst, &src_op, &dst_op, opts, sar_ir, sar_irdisp8, sar_clr, sar_clrdisp8, NULL, NULL);
		break;
	case M68K_LSR:
		dst = translate_shift(dst, inst, &src_op, &dst_op, opts, shr_ir, shr_irdisp8, shr_clr, shr_clrdisp8, shl_ir, shl_irdisp8);
		break;
	case M68K_BCHG:
	case M68K_BCLR:
	case M68K_BSET:
	case M68K_BTST:
		dst = cycles(dst, inst->extra.size == OPSIZE_BYTE ? 4 : (
			inst->op == M68K_BTST ? 6 : (inst->op == M68K_BCLR ? 10 : 8))
		);
		if (src_op.mode == MODE_IMMED) {
			if (inst->extra.size == OPSIZE_BYTE) {
				src_op.disp &= 0x7;
			}
			if (inst->op == M68K_BTST) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = bt_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					dst = bt_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else if (inst->op == M68K_BSET) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = bts_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					dst = bts_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else if (inst->op == M68K_BCLR) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = btr_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					dst = btr_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = btc_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					dst = btc_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			}
		} else {
			if (src_op.mode == MODE_REG_DISPLACE8 || (inst->dst.addr_mode != MODE_REG && src_op.base != SCRATCH1 && src_op.base != SCRATCH2)) {
				if (dst_op.base == SCRATCH1) {
					dst = push_r(dst, SCRATCH2);
					if (src_op.mode == MODE_REG_DIRECT) {
						dst = mov_rr(dst, src_op.base, SCRATCH2, SZ_B);
					} else {
						dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_B);
					}
					src_op.base = SCRATCH2;
				} else {
					if (src_op.mode == MODE_REG_DIRECT) {
						dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_B);
					} else {
						dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_B);
					}
					src_op.base = SCRATCH1;
				}
			}
			uint8_t size = inst->extra.size;
			if (dst_op.mode == MODE_REG_DISPLACE8) {
				if (src_op.base != SCRATCH1 && src_op.base != SCRATCH2) {
					if (src_op.mode == MODE_REG_DIRECT) {
						dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_D);
					} else {
						dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_D);
						src_op.mode = MODE_REG_DIRECT;
					}
					src_op.base = SCRATCH1;
				}
				//b### with register destination is modulo 32
				//x86 with a memory destination isn't modulo anything
				//so use an and here to force the value to be modulo 32
				dst = and_ir(dst, 31, SCRATCH1, SZ_D);
			} else if(inst->dst.addr_mode != MODE_REG) {
				//b### with memory destination is modulo 8
				//x86-64 doesn't support 8-bit bit operations
				//so we fake it by forcing the bit number to be modulo 8
				dst = and_ir(dst, 7, src_op.base, SZ_D);
				size = SZ_D;
			}
			if (inst->op == M68K_BTST) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = bt_rr(dst, src_op.base, dst_op.base, size);
				} else {
					dst = bt_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else if (inst->op == M68K_BSET) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = bts_rr(dst, src_op.base, dst_op.base, size);
				} else {
					dst = bts_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else if (inst->op == M68K_BCLR) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = btr_rr(dst, src_op.base, dst_op.base, size);
				} else {
					dst = btr_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					dst = btc_rr(dst, src_op.base, dst_op.base, size);
				} else {
					dst = btc_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
				}
			}
			if (src_op.base == SCRATCH2) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		//x86 sets the carry flag to the value of the bit tested
		//68K sets the zero flag to the complement of the bit tested
		dst = setcc_r(dst, CC_NC, FLAG_Z);
		if (inst->op != M68K_BTST) {
			dst = m68k_save_result(inst, dst, opts);
		}
		break;
	case M68K_CHK:
	{
		dst = cycles(dst, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
		} else {
			dst = cmp_irdisp8(dst, 0, dst_op.base, dst_op.disp, inst->extra.size);
		}
		uint32_t isize;
		switch(inst->src.addr_mode)
		{
		case MODE_AREG_DISPLACE:
		case MODE_AREG_INDEX_DISP8:
		case MODE_ABSOLUTE_SHORT:
		case MODE_PC_INDEX_DISP8:
		case MODE_PC_DISPLACE:
		case MODE_IMMEDIATE:
			isize = 4;
			break;
		case MODE_ABSOLUTE:
			isize = 6;
			break;
		default:
			isize = 2;
		}
		uint8_t * passed = dst+1;
		dst = jcc(dst, CC_GE, dst+2);
		dst = mov_ir(dst, 1, FLAG_N, SZ_B);
		dst = mov_ir(dst, VECTOR_CHK, SCRATCH2, SZ_D);
		dst = mov_ir(dst, inst->address+isize, SCRATCH1, SZ_D);
		dst = jmp(dst, opts->trap);
		*passed = dst - (passed+1);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = cmp_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else if(src_op.mode == MODE_REG_DISPLACE8) {
				dst = cmp_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = cmp_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			}
		} else if(dst_op.mode == MODE_REG_DISPLACE8) {
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = cmp_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			} else {
				dst = cmp_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		passed = dst+1;
		dst = jcc(dst, CC_LE, dst+2);
		dst = mov_ir(dst, 0, FLAG_N, SZ_B);
		dst = mov_ir(dst, VECTOR_CHK, SCRATCH2, SZ_D);
		dst = mov_ir(dst, inst->address+isize, SCRATCH1, SZ_D);
		dst = jmp(dst, opts->trap);
		*passed = dst - (passed+1);
		dst = cycles(dst, 4);
		break;
	}
	case M68K_DIVS:
	case M68K_DIVU:
	{
		//TODO: cycle exact division
		dst = cycles(dst, inst->op == M68K_DIVS ? 158 : 140);
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = push_r(dst, RDX);
		dst = push_r(dst, RAX);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, dst_op.base, RAX, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, RAX, SZ_D);
		}
		if (src_op.mode == MODE_IMMED) {
			dst = mov_ir(dst, (src_op.disp & 0x8000) && inst->op == M68K_DIVS ? src_op.disp | 0xFFFF0000 : src_op.disp, SCRATCH2, SZ_D);
		} else if (src_op.mode == MODE_REG_DIRECT) {
			if (inst->op == M68K_DIVS) {
				dst = movsx_rr(dst, src_op.base, SCRATCH2, SZ_W, SZ_D);
			} else {
				dst = movzx_rr(dst, src_op.base, SCRATCH2, SZ_W, SZ_D);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			if (inst->op == M68K_DIVS) {
				dst = movsx_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_W, SZ_D);
			} else {
				dst = movzx_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_W, SZ_D);
			}
		}
		dst = cmp_ir(dst, 0, SCRATCH2, SZ_D);
		uint8_t * not_zero = dst+1;
		dst = jcc(dst, CC_NZ, dst+2);
		dst = pop_r(dst, RAX);
		dst = pop_r(dst, RDX);
		dst = mov_ir(dst, VECTOR_INT_DIV_ZERO, SCRATCH2, SZ_D);
		dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
		dst = jmp(dst, opts->trap);
		*not_zero = dst - (not_zero+1);
		if (inst->op == M68K_DIVS) {
			dst = cdq(dst);
		} else {
			dst = xor_rr(dst, RDX, RDX, SZ_D);
		}
		if (inst->op == M68K_DIVS) {
			dst = idiv_r(dst, SCRATCH2, SZ_D);
		} else {
			dst = div_r(dst, SCRATCH2, SZ_D);
		}
		uint8_t * skip_sec_check;
		if (inst->op == M68K_DIVS) {
			dst = cmp_ir(dst, 0x8000, RAX, SZ_D);
			skip_sec_check = dst + 1;
			dst = jcc(dst, CC_GE, dst+2);
			dst = cmp_ir(dst, -0x8000, RAX, SZ_D);
			norm_off = dst+1;
			dst = jcc(dst, CC_L, dst+2);
		} else {
			dst = cmp_ir(dst, 0x10000, RAX, SZ_D);
			norm_off = dst+1;
			dst = jcc(dst, CC_NC, dst+2);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, RDX, dst_op.base, SZ_W);
			dst = shl_ir(dst, 16, dst_op.base, SZ_D);
			dst = mov_rr(dst, RAX, dst_op.base, SZ_W);
		} else {
			dst = mov_rrdisp8(dst, RDX, dst_op.base, dst_op.disp, SZ_W);
			dst = shl_irdisp8(dst, 16, dst_op.base, dst_op.disp, SZ_D);
			dst = mov_rrdisp8(dst, RAX, dst_op.base, dst_op.disp, SZ_W);
		}
		dst = cmp_ir(dst, 0, RAX, SZ_W);
		dst = pop_r(dst, RAX);
		dst = pop_r(dst, RDX);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		end_off = dst+1;
		dst = jmp(dst, dst+2);
		*norm_off = dst - (norm_off + 1);
		if (inst->op == M68K_DIVS) {
			*skip_sec_check = dst - (skip_sec_check+1);
		}
		dst = pop_r(dst, RAX);
		dst = pop_r(dst, RDX);
		dst = mov_ir(dst, 1, FLAG_V, SZ_B);
		*end_off = dst - (end_off + 1);
		break;
	}
	case M68K_EOR:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = xor_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = xor_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = xor_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = xor_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = xor_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_EORI_CCR:
	case M68K_EORI_SR:
		dst = cycles(dst, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (inst->src.params.immed & 0x1) {
			dst = xor_ir(dst, 1, FLAG_C, SZ_B);
		}
		if (inst->src.params.immed & 0x2) {
			dst = xor_ir(dst, 1, FLAG_V, SZ_B);
		}
		if (inst->src.params.immed & 0x4) {
			dst = xor_ir(dst, 1, FLAG_Z, SZ_B);
		}
		if (inst->src.params.immed & 0x8) {
			dst = xor_ir(dst, 1, FLAG_N, SZ_B);
		}
		if (inst->src.params.immed & 0x10) {
			dst = xor_irdisp8(dst, 1, CONTEXT, 0, SZ_B);
		}
		if (inst->op == M68K_ORI_SR) {
			dst = xor_irdisp8(dst, inst->src.params.immed >> 8, CONTEXT, offsetof(m68k_context, status), SZ_B);
			if (inst->src.params.immed & 0x700) {
				dst = call(dst, (uint8_t *)do_sync);
			}
		}
		break;
	case M68K_EXG:
		dst = cycles(dst, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, dst_op.base, SCRATCH2, SZ_D);
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, src_op.base, dst_op.base, SZ_D);
				dst = mov_rr(dst, SCRATCH2, src_op.base, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH2, src_op.base, src_op.disp, SZ_D);
			}
		} else {
			dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH2, SZ_D);
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = mov_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, SZ_D);
				dst = mov_rr(dst, SCRATCH2, src_op.base, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH2, src_op.base, src_op.disp, SZ_D);
			}
		}
		break;
	case M68K_ILLEGAL:
		dst = call(dst, opts->save_context);
		dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
		dst = call(dst, (uint8_t *)print_regs_exit);
		break;
	case M68K_MOVE_FROM_SR:
		//TODO: Trap if not in system mode
		dst = call(dst, (uint8_t *)get_sr);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, SCRATCH1, dst_op.base, SZ_W);
		} else {
			dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_W);
		}
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_MOVE_CCR:
	case M68K_MOVE_SR:
		//TODO: Privilege check for MOVE to SR
		if (src_op.mode == MODE_IMMED) {
			dst = mov_ir(dst, src_op.disp & 0x1, FLAG_C, SZ_B);
			dst = mov_ir(dst, (src_op.disp >> 1) & 0x1, FLAG_V, SZ_B);
			dst = mov_ir(dst, (src_op.disp >> 2) & 0x1, FLAG_Z, SZ_B);
			dst = mov_ir(dst, (src_op.disp >> 3) & 0x1, FLAG_N, SZ_B);
			dst = mov_irind(dst, (src_op.disp >> 4) & 0x1, CONTEXT, SZ_B);
			if (inst->op == M68K_MOVE_SR) {
				dst = mov_irdisp8(dst, (src_op.disp >> 8), CONTEXT, offsetof(m68k_context, status), SZ_B);
				if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
					//leave supervisor mode
					dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
					dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
					dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
				}
				dst = call(dst, (uint8_t *)do_sync);
			}
			dst = cycles(dst, 12);
		} else {
			if (src_op.base != SCRATCH1) {
				if (src_op.mode == MODE_REG_DIRECT) {
					dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_W);
				} else {
					dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_W);
				}
			}
			dst = call(dst, (uint8_t *)(inst->op == M68K_MOVE_SR ? set_sr : set_ccr));
			dst = cycles(dst, 12);

		}
		break;
	case M68K_MOVE_USP:
		dst = cycles(dst, BUS);
		//TODO: Trap if not in supervisor mode
		//dst = bt_irdisp8(dst, BIT_SUPERVISOR, CONTEXT, offsetof(m68k_context, status), SZ_B);
		if (inst->src.addr_mode == MODE_UNUSED) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, dst_op.base, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SCRATCH1, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_D);
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = mov_rrdisp8(dst, src_op.base, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
			}
		}
		break;
	//case M68K_MOVEP:
	case M68K_MULS:
	case M68K_MULU:
		dst = cycles(dst, 70); //TODO: Calculate the actual value based on the value of the <ea> parameter
		if (src_op.mode == MODE_IMMED) {
			dst = mov_ir(dst, inst->op == M68K_MULU ? (src_op.disp & 0xFFFF) : ((src_op.disp & 0x8000) ? src_op.disp | 0xFFFF0000 : src_op.disp), SCRATCH1, SZ_D);
		} else if (src_op.mode == MODE_REG_DIRECT) {
			if (inst->op == M68K_MULS) {
				dst = movsx_rr(dst, src_op.base, SCRATCH1, SZ_W, SZ_D);
			} else {
				dst = movzx_rr(dst, src_op.base, SCRATCH1, SZ_W, SZ_D);
			}
		} else {
			if (inst->op == M68K_MULS) {
				dst = movsx_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_W, SZ_D);
			} else {
				dst = movzx_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_W, SZ_D);
			}
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst_reg = dst_op.base;
			if (inst->op == M68K_MULS) {
				dst = movsx_rr(dst, dst_reg, dst_reg, SZ_W, SZ_D);
			} else {
				dst = movzx_rr(dst, dst_reg, dst_reg, SZ_W, SZ_D);
			}
		} else {
			dst_reg = SCRATCH2;
			if (inst->op == M68K_MULS) {
				dst = movsx_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH2, SZ_W, SZ_D);
			} else {
				dst = movzx_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH2, SZ_W, SZ_D);
			}
		}
		dst = imul_rr(dst, SCRATCH1, dst_reg, SZ_D);
		if (dst_op.mode == MODE_REG_DISPLACE8) {
			dst = mov_rrdisp8(dst, dst_reg, dst_op.base, dst_op.disp, SZ_D);
		}
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = cmp_ir(dst, 0, dst_reg, SZ_D);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		break;
	//case M68K_NBCD:
	case M68K_NEG:
		dst = cycles(dst, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = neg_r(dst, dst_op.base, inst->extra.size);
		} else {
			dst = neg_rdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_NEGX:
		dst = cycles(dst, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (dst_op.base == SCRATCH1) {
				dst = push_r(dst, SCRATCH2);
				dst = xor_rr(dst, SCRATCH2, SCRATCH2, inst->extra.size);
				dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
				dst = sbb_rr(dst, dst_op.base, SCRATCH2, inst->extra.size);
				dst = mov_rr(dst, SCRATCH2, dst_op.base, inst->extra.size);
				dst = pop_r(dst, SCRATCH2);
			} else {
				dst = xor_rr(dst, SCRATCH1, SCRATCH1, inst->extra.size);
				dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
				dst = sbb_rr(dst, dst_op.base, SCRATCH1, inst->extra.size);
				dst = mov_rr(dst, SCRATCH1, dst_op.base, inst->extra.size);
			}
		} else {
			dst = xor_rr(dst, SCRATCH1, SCRATCH1, inst->extra.size);
			dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
			dst = sbb_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH1, inst->extra.size);
			dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, inst->extra.size);
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = jcc(dst, CC_Z, dst+4);
		dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
		break;
	case M68K_NOP:
		dst = cycles(dst, BUS);
		break;
	case M68K_NOT:
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = not_r(dst, dst_op.base, inst->extra.size);
			dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
		} else {
			dst = not_rdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
			dst = cmp_irdisp8(dst, 0, dst_op.base, dst_op.disp, inst->extra.size);
		}

		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_OR:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = or_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = or_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = or_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = or_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = or_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ORI_CCR:
	case M68K_ORI_SR:
		dst = cycles(dst, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (inst->src.params.immed & 0x1) {
			dst = mov_ir(dst, 1, FLAG_C, SZ_B);
		}
		if (inst->src.params.immed & 0x2) {
			dst = mov_ir(dst, 1, FLAG_V, SZ_B);
		}
		if (inst->src.params.immed & 0x4) {
			dst = mov_ir(dst, 1, FLAG_Z, SZ_B);
		}
		if (inst->src.params.immed & 0x8) {
			dst = mov_ir(dst, 1, FLAG_N, SZ_B);
		}
		if (inst->src.params.immed & 0x10) {
			dst = mov_irind(dst, 1, CONTEXT, SZ_B);
		}
		if (inst->op == M68K_ORI_SR) {
			dst = or_irdisp8(dst, inst->src.params.immed >> 8, CONTEXT, offsetof(m68k_context, status), SZ_B);
			if (inst->src.params.immed & 0x700) {
				dst = call(dst, (uint8_t *)do_sync);
			}
		}
		break;
	case M68K_RESET:
		dst = call(dst, opts->save_context);
		dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
		dst = call(dst, (uint8_t *)print_regs_exit);
		break;
	case M68K_ROL:
	case M68K_ROR:
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		if (inst->src.addr_mode == MODE_UNUSED) {
			dst = cycles(dst, BUS);
			//Memory rotate
			if (inst->op == M68K_ROL) {
				dst = rol_ir(dst, 1, dst_op.base, inst->extra.size);
			} else {
				dst = ror_ir(dst, 1, dst_op.base, inst->extra.size);
			}
			dst = setcc_r(dst, CC_C, FLAG_C);
			dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = m68k_save_result(inst, dst, opts);
		} else {
			if (src_op.mode == MODE_IMMED) {
				dst = cycles(dst, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + src_op.disp*2);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						dst = rol_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
					} else {
						dst = ror_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						dst = rol_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = ror_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				dst = setcc_r(dst, CC_C, FLAG_C);
			} else {
				if (src_op.mode == MODE_REG_DIRECT) {
					if (src_op.base != SCRATCH1) {
						dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_B);
					}
				} else {
					dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_B);
				}
				dst = and_ir(dst, 63, SCRATCH1, SZ_D);
				zero_off = dst+1;
				dst = jcc(dst, CC_Z, dst+2);
				dst = add_rr(dst, SCRATCH1, CYCLES, SZ_D);
				dst = add_rr(dst, SCRATCH1, CYCLES, SZ_D);
				dst = cmp_ir(dst, 32, SCRATCH1, SZ_B);
				norm_off = dst+1;
				dst = jcc(dst, CC_L, dst+2);
				dst = sub_ir(dst, 32, SCRATCH1, SZ_B);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						dst = rol_ir(dst, 31, dst_op.base, inst->extra.size);
						dst = rol_ir(dst, 1, dst_op.base, inst->extra.size);
					} else {
						dst = ror_ir(dst, 31, dst_op.base, inst->extra.size);
						dst = ror_ir(dst, 1, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						dst = rol_irdisp8(dst, 31, dst_op.base, dst_op.disp, inst->extra.size);
						dst = rol_irdisp8(dst, 1, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = ror_irdisp8(dst, 31, dst_op.base, dst_op.disp, inst->extra.size);
						dst = ror_irdisp8(dst, 1, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				*norm_off = dst - (norm_off+1);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						dst = rol_clr(dst, dst_op.base, inst->extra.size);
					} else {
						dst = ror_clr(dst, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						dst = rol_clrdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = ror_clrdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				dst = setcc_r(dst, CC_C, FLAG_C);
				end_off = dst + 1;
				dst = jmp(dst, dst+2);
				*zero_off = dst - (zero_off+1);
				dst = mov_ir(dst, 0, FLAG_C, SZ_B);
				*end_off = dst - (end_off+1);
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
			} else {
				dst = cmp_irdisp8(dst, 0, dst_op.base, dst_op.disp, inst->extra.size);
			}
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		break;
	case M68K_ROXL:
	case M68K_ROXR:
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		if (inst->src.addr_mode == MODE_UNUSED) {
			dst = cycles(dst, BUS);
			//Memory rotate
			dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
			if (inst->op == M68K_ROXL) {
				dst = rcl_ir(dst, 1, dst_op.base, inst->extra.size);
			} else {
				dst = rcr_ir(dst, 1, dst_op.base, inst->extra.size);
			}
			dst = setcc_r(dst, CC_C, FLAG_C);
			dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
			dst = m68k_save_result(inst, dst, opts);
		} else {
			if (src_op.mode == MODE_IMMED) {
				dst = cycles(dst, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + src_op.disp*2);
				dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						dst = rcl_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
					} else {
						dst = rcr_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						dst = rcl_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = rcr_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				dst = setcc_r(dst, CC_C, FLAG_C);
				dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
			} else {
				if (src_op.mode == MODE_REG_DIRECT) {
					if (src_op.base != SCRATCH1) {
						dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_B);
					}
				} else {
					dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_B);
				}
				dst = and_ir(dst, 63, SCRATCH1, SZ_D);
				zero_off = dst+1;
				dst = jcc(dst, CC_Z, dst+2);
				dst = add_rr(dst, SCRATCH1, CYCLES, SZ_D);
				dst = add_rr(dst, SCRATCH1, CYCLES, SZ_D);
				dst = cmp_ir(dst, 32, SCRATCH1, SZ_B);
				norm_off = dst+1;
				dst = jcc(dst, CC_L, dst+2);
				dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						dst = rcl_ir(dst, 31, dst_op.base, inst->extra.size);
						dst = rcl_ir(dst, 1, dst_op.base, inst->extra.size);
					} else {
						dst = rcr_ir(dst, 31, dst_op.base, inst->extra.size);
						dst = rcr_ir(dst, 1, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						dst = rcl_irdisp8(dst, 31, dst_op.base, dst_op.disp, inst->extra.size);
						dst = rcl_irdisp8(dst, 1, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = rcr_irdisp8(dst, 31, dst_op.base, dst_op.disp, inst->extra.size);
						dst = rcr_irdisp8(dst, 1, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				dst = setcc_rind(dst, CC_C, CONTEXT);
				dst = sub_ir(dst, 32, SCRATCH1, SZ_B);
				*norm_off = dst - (norm_off+1);
				dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						dst = rcl_clr(dst, dst_op.base, inst->extra.size);
					} else {
						dst = rcr_clr(dst, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						dst = rcl_clrdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						dst = rcr_clrdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				dst = setcc_r(dst, CC_C, FLAG_C);
				dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
				end_off = dst + 1;
				dst = jmp(dst, dst+2);
				*zero_off = dst - (zero_off+1);
				//Carry flag is set to X flag when count is 0, this is different from ROR/ROL
				dst = mov_rindr(dst, CONTEXT, FLAG_C, SZ_B);
				*end_off = dst - (end_off+1);
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = cmp_ir(dst, 0, dst_op.base, inst->extra.size);
			} else {
				dst = cmp_irdisp8(dst, 0, dst_op.base, dst_op.disp, inst->extra.size);
			}
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
		}
		break;
	case M68K_RTE:
		//TODO: Trap if not in system mode
		//Read saved SR
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, opts->read_16);
		dst = add_ir(dst, 2, opts->aregs[7], SZ_D);
		dst = call(dst, (uint8_t *)set_sr);
		//Read saved PC
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, opts->read_32);
		dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
		//Check if we've switched to user mode and swap stack pointers if needed
		dst = bt_irdisp8(dst, 5, CONTEXT, offsetof(m68k_context, status), SZ_B);
		end_off = dst+1;
		dst = jcc(dst, CC_C, dst+2);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
		dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
		*end_off = dst - (end_off+1);
		//Get native address, sync components, recalculate integer points and jump to returned address
		dst = call(dst, (uint8_t *)m68k_native_addr_and_sync);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case M68K_RTR:
		//Read saved CCR
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, opts->read_16);
		dst = add_ir(dst, 2, opts->aregs[7], SZ_D);
		dst = call(dst, (uint8_t *)set_ccr);
		//Read saved PC
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, opts->read_32);
		dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
		//Get native address and jump to it
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = jmp_r(dst, SCRATCH1);
		break;
	case M68K_SBCD:
		if (src_op.base != SCRATCH2) {
			if (src_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, src_op.base, SCRATCH2, SZ_B);
			} else {
				dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_B);
			}
		}
		if (dst_op.base != SCRATCH1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, dst_op.base, SCRATCH1, SZ_B);
			} else {
				dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, SCRATCH1, SZ_B);
			}
		}
		dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
		dst = jcc(dst, CC_NC, dst+5);
		dst = sub_ir(dst, 1, SCRATCH1, SZ_B);
		dst = call(dst, (uint8_t *)bcd_sub);
		dst = mov_rr(dst, CH, FLAG_C, SZ_B);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = cmp_ir(dst, 0, SCRATCH1, SZ_B);
		dst = jcc(dst, CC_Z, dst+4);
		dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		if (dst_op.base != SCRATCH1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, SCRATCH1, dst_op.base, SZ_B);
			} else {
				dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_B);
			}
		}
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_STOP: {
		//TODO: Trap if not in system mode
		//manual says 4 cycles, but it has to be at least 8 since it's a 2-word instruction
		//possibly even 12 since that's how long MOVE to SR takes
		dst = cycles(dst, BUS*2);
		dst = mov_ir(dst, src_op.disp & 0x1, FLAG_C, SZ_B);
		dst = mov_ir(dst, (src_op.disp >> 1) & 0x1, FLAG_V, SZ_B);
		dst = mov_ir(dst, (src_op.disp >> 2) & 0x1, FLAG_Z, SZ_B);
		dst = mov_ir(dst, (src_op.disp >> 3) & 0x1, FLAG_N, SZ_B);
		dst = mov_irind(dst, (src_op.disp >> 4) & 0x1, CONTEXT, SZ_B);
		dst = mov_irdisp8(dst, (src_op.disp >> 8), CONTEXT, offsetof(m68k_context, status), SZ_B);
		if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
			//leave supervisor mode
			dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
			dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
		}
		uint8_t * loop_top = dst;
		dst = call(dst, (uint8_t *)do_sync);
    dst = cmp_rr(dst, LIMIT, CYCLES, SZ_D);
    uint8_t * normal_cycle_up = dst + 1;
    dst = jcc(dst, CC_A, dst+2);
    dst = cycles(dst, BUS);
    uint8_t * after_cycle_up = dst + 1;
    dst = jmp(dst, dst+2);
    *normal_cycle_up = dst - (normal_cycle_up + 1);
		dst = mov_rr(dst, LIMIT, CYCLES, SZ_D);
    *after_cycle_up = dst - (after_cycle_up+1);
		dst = cmp_rdisp8r(dst, CONTEXT, offsetof(m68k_context, int_cycle), CYCLES, SZ_D);
		dst = jcc(dst, CC_C, loop_top);
		break;
	}
	case M68K_SUB:
		size = inst->dst.addr_mode == MODE_AREG ? OPSIZE_LONG : inst->extra.size;
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sub_rr(dst, src_op.base, dst_op.base, size);
			} else {
				dst = sub_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = sub_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sub_ir(dst, src_op.disp, dst_op.base, size);
			} else {
				dst = sub_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, size);
			}
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			dst = setcc_r(dst, CC_C, FLAG_C);
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = setcc_r(dst, CC_O, FLAG_V);
			dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		}
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_SUBX:
		dst = cycles(dst, BUS);
		dst = bt_irdisp8(dst, 0, CONTEXT, 0, SZ_B);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sbb_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = sbb_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = sbb_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sbb_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = sbb_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = jcc(dst, CC_Z, dst+4);
		dst = mov_ir(dst, 0, FLAG_Z, SZ_B);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_SWAP:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = rol_ir(dst, 16, src_op.base, SZ_D);
			dst = cmp_ir(dst, 0, src_op.base, SZ_D);
		} else{
			dst = rol_irdisp8(dst, 16, src_op.base, src_op.disp, SZ_D);
			dst = cmp_irdisp8(dst, 0, src_op.base, src_op.disp, SZ_D);
		}

		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		break;
	//case M68K_TAS:
	case M68K_TRAP:
		dst = mov_ir(dst, src_op.disp + VECTOR_TRAP_0, SCRATCH2, SZ_D);
		dst = mov_ir(dst, inst->address+2, SCRATCH1, SZ_D);
		dst = jmp(dst, opts->trap);
		break;
	//case M68K_TRAPV:
	case M68K_TST:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = cmp_ir(dst, 0, src_op.base, inst->extra.size);
		} else { //M68000 doesn't support immedate operand for tst, so this must be MODE_REG_DISPLACE8
			dst = cmp_irdisp8(dst, 0, src_op.base, src_op.disp, inst->extra.size);
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		break;
	case M68K_UNLK:
		dst = cycles(dst, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, dst_op.base, opts->aregs[7], SZ_D);
		} else {
			dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, opts->aregs[7], SZ_D);
		}
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, opts->read_32);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, SCRATCH1, dst_op.base, SZ_D);
		} else {
			dst = mov_rrdisp8(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_D);
		}
		dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\ninstruction %d not yet implemented\n", inst->address, disasm_buf, inst->op);
		exit(1);
	}
	return dst;
}

uint8_t m68k_is_terminal(m68kinst * inst)
{
	return inst->op == M68K_RTS || inst->op == M68K_RTE || inst->op == M68K_RTR || inst->op == M68K_JMP
		|| inst->op == M68K_TRAP || inst->op == M68K_ILLEGAL || inst->op == M68K_INVALID || inst->op == M68K_RESET
		|| (inst->op == M68K_BCC && inst->extra.cond == COND_TRUE);
}

void m68k_handle_deferred(m68k_context * context)
{
	x86_68k_options * opts = context->options;
	process_deferred(&opts->deferred, context, (native_addr_func)get_native_from_context);
	if (opts->deferred) {
		translate_m68k_stream(opts->deferred->address, context);
	}
}

uint8_t * translate_m68k_stream(uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	x86_68k_options * opts = context->options;
	uint8_t * dst = opts->cur_code;
	uint8_t * dst_end = opts->code_end;
	address &= 0xFFFFFF;
	if(get_native_address(opts->native_code_map, address)) {
		return dst;
	}
	char disbuf[1024];
	uint16_t *encoded, *next;
	if ((address & 0xFFFFFF) < 0x400000) {
		encoded = context->mem_pointers[0] + (address & 0xFFFFFF)/2;
	} else if ((address & 0xFFFFFF) > 0xE00000) {
		encoded = context->mem_pointers[1] + (address  & 0xFFFF)/2;
	} else {
		printf("attempt to translate non-memory address: %X\n", address);
		exit(1);
	}
	do {
		if (opts->address_log) {
			fprintf(opts->address_log, "%X\n", address);
		}
		do {
			if (dst_end-dst < MAX_NATIVE_SIZE) {
				if (dst_end-dst < 5) {
					puts("out of code memory, not enough space for jmp to next chunk");
					exit(1);
				}
				size_t size = 1024*1024;
				opts->cur_code = alloc_code(&size);
				opts->code_end = opts->cur_code + size;
				jmp(dst, opts->cur_code);
				dst = opts->cur_code;
				dst_end = opts->code_end;
			}
			if (address >= 0x400000 && address < 0xE00000) {
				dst = xor_rr(dst, RDI, RDI, SZ_D);
				dst = call(dst, (uint8_t *)exit);
				break;
			}
			uint8_t * existing = get_native_address(opts->native_code_map, address);
			if (existing) {
				dst = jmp(dst, existing);
				break;
			}
			next = m68k_decode(encoded, &instbuf, address);
			if (instbuf.op == M68K_INVALID) {
				instbuf.src.params.immed = *encoded;
			}
			uint16_t m68k_size = (next-encoded)*2;
			address += m68k_size;
			encoded = next;
			//m68k_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", instbuf.address, disbuf);
			uint8_t * after = translate_m68k(dst, &instbuf, opts);
			map_native_address(context, instbuf.address, dst, m68k_size, after-dst);
			dst = after;
		} while(!m68k_is_terminal(&instbuf));
		process_deferred(&opts->deferred, context, (native_addr_func)get_native_from_context);
		if (opts->deferred) {
			address = opts->deferred->address;
			if ((address & 0xFFFFFF) < 0x400000) {
				encoded = context->mem_pointers[0] + (address & 0xFFFFFF)/2;
			} else if ((address & 0xFFFFFF) > 0xE00000) {
				encoded = context->mem_pointers[1] + (address  & 0xFFFF)/2;
			} else {
				printf("attempt to translate non-memory address: %X\n", address);
				exit(1);
			}
		} else {
			encoded = NULL;
		}
	} while(encoded != NULL);
	opts->cur_code = dst;
	return dst;
}

uint8_t * get_native_address_trans(m68k_context * context, uint32_t address)
{
	address &= 0xFFFFFF;
	uint8_t * ret = get_native_address(context->native_code_map, address);
	if (!ret) {
		translate_m68k_stream(address, context);
		ret = get_native_address(context->native_code_map, address);
	}
	return ret;
}

void * m68k_retranslate_inst(uint32_t address, m68k_context * context)
{
	x86_68k_options * opts = context->options;
	uint8_t orig_size = get_native_inst_size(opts, address);
	uint8_t * orig_start = get_native_address(context->native_code_map, address);
	uint32_t orig = address;
	address &= 0xFFFF;
	uint8_t * dst = opts->cur_code;
	uint8_t * dst_end = opts->code_end;
	uint16_t *after, *inst = context->mem_pointers[1] + address/2;
	m68kinst instbuf;
	after = m68k_decode(inst, &instbuf, orig);
	if (orig_size != MAX_NATIVE_SIZE) {
		if (dst_end - dst < 128) {
			size_t size = 1024*1024;
			dst = alloc_code(&size);
			opts->code_end = dst_end = dst + size;
			opts->cur_code = dst;
		}
		deferred_addr * orig_deferred = opts->deferred;
		uint8_t * native_end = translate_m68k(dst, &instbuf, opts);
		uint8_t is_terminal = m68k_is_terminal(&instbuf);
		if ((native_end - dst) <= orig_size) {
			uint8_t * native_next;
			if (!is_terminal) {
				native_next = get_native_address(context->native_code_map, orig + (after-inst)*2);
			}
			if (is_terminal || (native_next && ((native_next == orig_start + orig_size) || (orig_size - (native_end - dst)) > 5))) {
				remove_deferred_until(&opts->deferred, orig_deferred);
				native_end = translate_m68k(orig_start, &instbuf, opts);
				if (!is_terminal) {
					if (native_next == orig_start + orig_size && (native_next-native_end) < 2) {
						while (native_end < orig_start + orig_size) {
							*(native_end++) = 0x90; //NOP
						}
					} else {
						jmp(native_end, native_next);
					}
				}
				m68k_handle_deferred(context);
				return orig_start;
			}
		}

		map_native_address(context, instbuf.address, dst, (after-inst)*2, MAX_NATIVE_SIZE);
		opts->cur_code = dst+MAX_NATIVE_SIZE;
		jmp(orig_start, dst);
		if (!m68k_is_terminal(&instbuf)) {
			jmp(native_end, get_native_address_trans(context, orig + (after-inst)*2));
		}
		m68k_handle_deferred(context);
		return dst;
	} else {
		dst = translate_m68k(orig_start, &instbuf, opts);
		if (!m68k_is_terminal(&instbuf)) {
			dst = jmp(dst, get_native_address_trans(context, orig + (after-inst)*2));
		}
		m68k_handle_deferred(context);
		return orig_start;
	}
}

m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context)
{
	uint32_t inst_start = get_instruction_start(context->native_code_map, address | 0xFF0000);
	if (inst_start) {
		uint8_t * dst = get_native_address(context->native_code_map, inst_start);
		dst = mov_ir(dst, inst_start, SCRATCH2, SZ_D);
		x86_68k_options * options = context->options;
		if (!options->retrans_stub) {
			if (options->code_end - options->cur_code < 32) {
				size_t size = 1024*1024;
				options->cur_code = alloc_code(&size);
				options->code_end = options->cur_code + size;
			}
			uint8_t * rdst = options->retrans_stub = options->cur_code;
			rdst = call(rdst, options->save_context);
			rdst = push_r(rdst, CONTEXT);
			rdst = call(rdst, (uint8_t *)m68k_retranslate_inst);
			rdst = pop_r(rdst, CONTEXT);
			rdst = mov_rr(rdst, RAX, SCRATCH1, SZ_Q);
			rdst = call(rdst, options->load_context);
			rdst = jmp_r(rdst, SCRATCH1);
			options->cur_code = rdst;
		}
		dst = jmp(dst, options->retrans_stub);
	}
	return context;
}

void insert_breakpoint(m68k_context * context, uint32_t address, uint8_t * bp_handler)
{
	static uint8_t * bp_stub = NULL;
	uint8_t * native = get_native_address_trans(context, address);
	uint8_t * start_native = native;
	native = mov_ir(native, address, SCRATCH1, SZ_D);
	if (!bp_stub) {
		x86_68k_options * opts = context->options;
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
		dst = check_cycles_int(dst, address, opts);
		int check_int_size = dst-bp_stub;
		dst = bp_stub;

		//Save context and call breakpoint handler
		dst = call(dst, opts->save_context);
		dst = push_r(dst, SCRATCH1);
		dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
		dst = mov_rr(dst, SCRATCH1, RSI, SZ_D);
		dst = call(dst, bp_handler);
		dst = mov_rr(dst, RAX, CONTEXT, SZ_Q);
		//Restore context
		dst = call(dst, opts->load_context);
		dst = pop_r(dst, SCRATCH1);
		//do prologue stuff
		dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
		uint8_t * jmp_off = dst+1;
		dst = jcc(dst, CC_NC, dst + 7);
		dst = call(dst, opts->handle_cycle_limit_int);
		*jmp_off = dst - (jmp_off+1);
		//jump back to body of translated instruction
		dst = pop_r(dst, SCRATCH1);
		dst = add_ir(dst, check_int_size - (native-start_native), SCRATCH1, SZ_Q);
		dst = jmp_r(dst, SCRATCH1);
		opts->cur_code = dst;
	} else {
		native = call(native, bp_stub);
	}
}

void remove_breakpoint(m68k_context * context, uint32_t address)
{
	uint8_t * native = get_native_address(context->native_code_map, address);
	check_cycles_int(native, address, context->options);
}

void start_68k_context(m68k_context * context, uint32_t address)
{
	uint8_t * addr = get_native_address_trans(context, address);
	x86_68k_options * options = context->options;
	options->start_context(addr, context);
}

void m68k_reset(m68k_context * context)
{
	//TODO: Make this actually use the normal read functions
	context->aregs[7] = context->mem_pointers[0][0] << 16 | context->mem_pointers[0][1];
	uint32_t address = context->mem_pointers[0][2] << 16 | context->mem_pointers[0][3];
	start_68k_context(context, address);
}

typedef enum {
	READ_16,
	READ_8,
	WRITE_16,
	WRITE_8
} ftype;

uint8_t * gen_mem_fun(x86_68k_options * opts, memmap_chunk * memmap, uint32_t num_chunks, ftype fun_type)
{
	uint8_t * dst = opts->cur_code;
	uint8_t * start = dst;
	dst = check_cycles(dst);
	dst = cycles(dst, BUS);
	dst = and_ir(dst, 0xFFFFFF, SCRATCH1, SZ_D);
	uint8_t *lb_jcc = NULL, *ub_jcc = NULL;
	uint8_t is_write = fun_type == WRITE_16 || fun_type == WRITE_8;
	uint8_t adr_reg = is_write ? SCRATCH2 : SCRATCH1;
	uint16_t access_flag = is_write ? MMAP_WRITE : MMAP_READ;
	uint8_t size =  (fun_type == READ_16 || fun_type == WRITE_16) ? SZ_W : SZ_B;
	for (uint32_t chunk = 0; chunk < num_chunks; chunk++)
	{
		if (memmap[chunk].start > 0) {
			dst = cmp_ir(dst, memmap[chunk].start, adr_reg, SZ_D);
			lb_jcc = dst + 1;
			dst = jcc(dst, CC_C, dst+2);
		}
		if (memmap[chunk].end < 0x1000000) {
			dst = cmp_ir(dst, memmap[chunk].end, adr_reg, SZ_D);
			ub_jcc = dst + 1;
			dst = jcc(dst, CC_NC, dst+2);
		}

		if (memmap[chunk].mask != 0xFFFFFF) {
			dst = and_ir(dst, memmap[chunk].mask, adr_reg, SZ_D);
		}
		void * cfun;
		switch (fun_type)
		{
		case READ_16:
			cfun = memmap[chunk].read_16;
			break;
		case READ_8:
			cfun = memmap[chunk].read_8;
			break;
		case WRITE_16:
			cfun = memmap[chunk].write_16;
			break;
		case WRITE_8:
			cfun = memmap[chunk].write_8;
			break;
		default:
			cfun = NULL;
		}
		if(memmap[chunk].buffer && memmap[chunk].flags & access_flag) {
			if (memmap[chunk].flags & MMAP_PTR_IDX) {
				if (memmap[chunk].flags & MMAP_FUNC_NULL) {
					dst = cmp_irdisp8(dst, 0, CONTEXT, offsetof(m68k_context, mem_pointers) + sizeof(void*) * memmap[chunk].ptr_index, SZ_Q);
					uint8_t * not_null = dst+1;
					dst = jcc(dst, CC_NZ, dst+2);
					dst = call(dst, opts->save_context);
					if (is_write) {
						//SCRATCH2 is RDI, so no need to move it there
						dst = mov_rr(dst, SCRATCH1, RDX, size);
					} else {
						dst = push_r(dst, CONTEXT);
						dst = mov_rr(dst, SCRATCH1, RDI, SZ_D);
					}
					dst = test_ir(dst, 8, RSP, SZ_D);
					uint8_t *adjust_rsp = dst+1;
					dst = jcc(dst, CC_NZ, dst+2);
					dst = call(dst, cfun);
					uint8_t *no_adjust = dst+1;
					dst = jmp(dst, dst+2);
					*adjust_rsp = dst - (adjust_rsp + 1);
					dst = sub_ir(dst, 8, RSP, SZ_Q);
					dst = call(dst, cfun);
					dst = add_ir(dst, 8, RSP, SZ_Q);
					*no_adjust = dst - (no_adjust + 1);
					if (is_write) {
						dst = mov_rr(dst, RAX, CONTEXT, SZ_Q);
					} else {
						dst = pop_r(dst, CONTEXT);
						dst = mov_rr(dst, RAX, SCRATCH1, size);
					}
					dst = jmp(dst, opts->load_context);

					*not_null = dst - (not_null + 1);
				}
				if (size == SZ_B) {
					dst = xor_ir(dst, 1, adr_reg, SZ_D);
				}
				dst = add_rdisp8r(dst, CONTEXT, offsetof(m68k_context, mem_pointers) + sizeof(void*) * memmap[chunk].ptr_index, adr_reg, SZ_Q);
				if (is_write) {
					dst = mov_rrind(dst, SCRATCH1, SCRATCH2, size);

				} else {
					dst = mov_rindr(dst, SCRATCH1, SCRATCH1, size);
				}
			} else {
				uint8_t tmp_size = size;
				if (size == SZ_B) {
					if ((memmap[chunk].flags & MMAP_ONLY_ODD) || (memmap[chunk].flags & MMAP_ONLY_EVEN)) {
						dst = bt_ir(dst, 0, adr_reg, SZ_D);
						uint8_t * good_addr = dst + 1;
						dst = jcc(dst, (memmap[chunk].flags & MMAP_ONLY_ODD) ? CC_C : CC_NC, dst+2);
						if (!is_write) {
							dst = mov_ir(dst, 0xFF, SCRATCH1, SZ_B);
						}
						dst = retn(dst);
						*good_addr = dst - (good_addr + 1);
						dst = shr_ir(dst, 1, adr_reg, SZ_D);
					} else {
						dst = xor_ir(dst, 1, adr_reg, SZ_D);
					}
				} else if ((memmap[chunk].flags & MMAP_ONLY_ODD) || (memmap[chunk].flags & MMAP_ONLY_EVEN)) {
					tmp_size = SZ_B;
					dst = shr_ir(dst, 1, adr_reg, SZ_D);
					if ((memmap[chunk].flags & MMAP_ONLY_EVEN) && is_write) {
						dst = shr_ir(dst, 8, SCRATCH1, SZ_W);
					}
				}
				if ((int64_t)memmap[chunk].buffer <= 0x7FFFFFFF && (int64_t)memmap[chunk].buffer >= -2147483648) {
					if (is_write) {
						dst = mov_rrdisp32(dst, SCRATCH1, SCRATCH2, (int64_t)memmap[chunk].buffer, tmp_size);
					} else {
						dst = mov_rdisp32r(dst, SCRATCH1, (int64_t)memmap[chunk].buffer, SCRATCH1, tmp_size);
					}
				} else {
					if (is_write) {
						dst = push_r(dst, SCRATCH1);
						dst = mov_ir(dst, (int64_t)memmap[chunk].buffer, SCRATCH1, SZ_Q);
						dst = add_rr(dst, SCRATCH1, SCRATCH2, SZ_Q);
						dst = pop_r(dst, SCRATCH1);
						dst = mov_rrind(dst, SCRATCH1, SCRATCH2, tmp_size);
					} else {
						dst = mov_ir(dst, (int64_t)memmap[chunk].buffer, SCRATCH2, SZ_Q);
						dst = mov_rindexr(dst, SCRATCH2, SCRATCH1, 1, SCRATCH1, tmp_size);
					}
				}
				if (size != tmp_size && !is_write) {
					if (memmap[chunk].flags & MMAP_ONLY_EVEN) {
						dst = shl_ir(dst, 8, SCRATCH1, SZ_W);
						dst = mov_ir(dst, 0xFF, SCRATCH1, SZ_B);
					} else {
						dst = or_ir(dst, 0xFF00, SCRATCH1, SZ_W);
					}
				}
			}
			if (is_write && (memmap[chunk].flags & MMAP_CODE)) {
				dst = mov_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
				dst = shr_ir(dst, 11, SCRATCH1, SZ_D);
				dst = bt_rrdisp32(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, ram_code_flags), SZ_D);
				uint8_t * not_code = dst+1;
				dst = jcc(dst, CC_NC, dst+2);
				dst = call(dst, opts->save_context);
				dst = call(dst, (uint8_t *)m68k_handle_code_write);
				dst = mov_rr(dst, RAX, CONTEXT, SZ_Q);
				dst = call(dst, opts->load_context);
				*not_code = dst - (not_code+1);
			}
			dst = retn(dst);
		} else if (cfun) {
			dst = call(dst, opts->save_context);
			if (is_write) {
				//SCRATCH2 is RDI, so no need to move it there
				dst = mov_rr(dst, SCRATCH1, RDX, size);
			} else {
				dst = push_r(dst, CONTEXT);
				dst = mov_rr(dst, SCRATCH1, RDI, SZ_D);
			}
			dst = test_ir(dst, 8, RSP, SZ_D);
			uint8_t *adjust_rsp = dst+1;
			dst = jcc(dst, CC_NZ, dst+2);
			dst = call(dst, cfun);
			uint8_t *no_adjust = dst+1;
			dst = jmp(dst, dst+2);
			*adjust_rsp = dst - (adjust_rsp + 1);
			dst = sub_ir(dst, 8, RSP, SZ_Q);
			dst = call(dst, cfun);
			dst = add_ir(dst, 8, RSP, SZ_Q);
			*no_adjust = dst - (no_adjust+1);
			if (is_write) {
				dst = mov_rr(dst, RAX, CONTEXT, SZ_Q);
			} else {
				dst = pop_r(dst, CONTEXT);
				dst = mov_rr(dst, RAX, SCRATCH1, size);
			}
			dst = jmp(dst, opts->load_context);
		} else {
			//Not sure the best course of action here
			if (!is_write) {
				dst = mov_ir(dst, size == SZ_B ? 0xFF : 0xFFFF, SCRATCH1, size);
			}
			dst = retn(dst);
		}
		if (lb_jcc) {
			*lb_jcc = dst - (lb_jcc+1);
			lb_jcc = NULL;
		}
		if (ub_jcc) {
			*ub_jcc = dst - (ub_jcc+1);
			ub_jcc = NULL;
		}
	}
	if (!is_write) {
		dst = mov_ir(dst, size == SZ_B ? 0xFF : 0xFFFF, SCRATCH1, size);
	}
	dst = retn(dst);
	opts->cur_code = dst;
	return start;
}

void init_x86_68k_opts(x86_68k_options * opts, memmap_chunk * memmap, uint32_t num_chunks)
{
	memset(opts, 0, sizeof(*opts));
	for (int i = 0; i < 8; i++)
		opts->dregs[i] = opts->aregs[i] = -1;
	opts->dregs[0] = R10;
	opts->dregs[1] = R11;
	opts->dregs[2] = R12;
	opts->dregs[3] = R8;
	opts->aregs[0] = R13;
	opts->aregs[1] = R14;
	opts->aregs[2] = R9;
	opts->aregs[7] = R15;

	opts->flag_regs[0] = -1;
	opts->flag_regs[1] = RBX;
	opts->flag_regs[2] = RDX;
	opts->flag_regs[3] = BH;
	opts->flag_regs[4] = DH;
	opts->native_code_map = malloc(sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	memset(opts->native_code_map, 0, sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	opts->deferred = NULL;
	size_t size = 1024 * 1024;
	opts->cur_code = alloc_code(&size);
	opts->code_end = opts->cur_code + size;
	opts->ram_inst_sizes = malloc(sizeof(uint8_t *) * 64);
	memset(opts->ram_inst_sizes, 0, sizeof(uint8_t *) * 64);

	uint8_t * dst = opts->cur_code;

	opts->save_context = dst;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			dst = mov_rrdisp8(dst, opts->flag_regs[i], CONTEXT, offsetof(m68k_context, flags) + i, SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			dst = mov_rrdisp8(dst, opts->dregs[i], CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			dst = mov_rrdisp8(dst, opts->aregs[i], CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, SZ_D);
		}
	}
	dst = mov_rrdisp8(dst, CYCLES, CONTEXT, offsetof(m68k_context, current_cycle), SZ_D);
	dst = retn(dst);

	opts->load_context = dst;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, flags) + i, opts->flag_regs[i], SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, opts->dregs[i], SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, opts->aregs[i], SZ_D);
		}
	}
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, current_cycle), CYCLES, SZ_D);
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, target_cycle), LIMIT, SZ_D);
	dst = retn(dst);

	opts->start_context = (start_fun)dst;
	dst = push_r(dst, RBP);
	dst = push_r(dst, R12);
	dst = push_r(dst, R13);
	dst = push_r(dst, R14);
	dst = push_r(dst, R15);
	dst = call(dst, opts->load_context);
	dst = call_r(dst, RDI);
	dst = call(dst, opts->save_context);
	dst = pop_r(dst, R15);
	dst = pop_r(dst, R14);
	dst = pop_r(dst, R13);
	dst = pop_r(dst, R12);
	dst = pop_r(dst, RBP);
	dst = retn(dst);

	opts->cur_code = dst;

	opts->read_16 = gen_mem_fun(opts, memmap, num_chunks, READ_16);
	opts->read_8 = gen_mem_fun(opts, memmap, num_chunks, READ_8);
	opts->write_16 = gen_mem_fun(opts, memmap, num_chunks, WRITE_16);
	opts->write_8 = gen_mem_fun(opts, memmap, num_chunks, WRITE_8);

	dst = opts->cur_code;

	opts->read_32 = dst;
	dst = push_r(dst, SCRATCH1);
	dst = call(dst, opts->read_16);
	dst = mov_rr(dst, SCRATCH1, SCRATCH2, SZ_W);
	dst = pop_r(dst, SCRATCH1);
	dst = push_r(dst, SCRATCH2);
	dst = add_ir(dst, 2, SCRATCH1, SZ_D);
	dst = call(dst, opts->read_16);
	dst = pop_r(dst, SCRATCH2);
	dst = movzx_rr(dst, SCRATCH1, SCRATCH1, SZ_W, SZ_D);
	dst = shl_ir(dst, 16, SCRATCH2, SZ_D);
	dst = or_rr(dst, SCRATCH2, SCRATCH1, SZ_D);
	dst = retn(dst);

	opts->write_32_lowfirst = dst;
	dst = push_r(dst, SCRATCH2);
	dst = push_r(dst, SCRATCH1);
	dst = add_ir(dst, 2, SCRATCH2, SZ_D);
	dst = call(dst, opts->write_16);
	dst = pop_r(dst, SCRATCH1);
	dst = pop_r(dst, SCRATCH2);
	dst = shr_ir(dst, 16, SCRATCH1, SZ_D);
	dst = jmp(dst, opts->write_16);

	opts->write_32_highfirst = dst;
	dst = push_r(dst, SCRATCH1);
	dst = push_r(dst, SCRATCH2);
	dst = shr_ir(dst, 16, SCRATCH1, SZ_D);
	dst = call(dst, opts->write_16);
	dst = pop_r(dst, SCRATCH2);
	dst = pop_r(dst, SCRATCH1);
	dst = add_ir(dst, 2, SCRATCH2, SZ_D);
	dst = jmp(dst, opts->write_16);

	opts->handle_cycle_limit_int = dst;
	dst = cmp_rdisp8r(dst, CONTEXT, offsetof(m68k_context, int_cycle), CYCLES, SZ_D);
	uint8_t * do_int = dst+1;
	dst = jcc(dst, CC_NC, dst+2);
	dst = cmp_rdisp8r(dst, CONTEXT, offsetof(m68k_context, sync_cycle), CYCLES, SZ_D);
	uint8_t * skip_sync = dst+1;
	dst = jcc(dst, CC_C, dst+2);
	dst = call(dst, opts->save_context);
	dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
	dst = mov_rr(dst, SCRATCH1, RSI, SZ_D);
	dst = test_ir(dst, 8, RSP, SZ_D);
	uint8_t *adjust_rsp = dst+1;
	dst = jcc(dst, CC_NZ, dst+2);
	dst = call(dst, (uint8_t *)sync_components);
	uint8_t *no_adjust = dst+1;
	dst = jmp(dst, dst+2);
	*adjust_rsp = dst - (adjust_rsp + 1);
	dst = sub_ir(dst, 8, RSP, SZ_Q);
	dst = call(dst, (uint8_t *)sync_components);
	dst = add_ir(dst, 8, RSP, SZ_Q);
	*no_adjust = dst - (no_adjust+1);
	dst = mov_rr(dst, RAX, CONTEXT, SZ_Q);
	dst = jmp(dst, opts->load_context);
	*skip_sync = dst - (skip_sync+1);
	dst = retn(dst);
	*do_int = dst - (do_int+1);
	//set target cycle to sync cycle
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, sync_cycle), LIMIT, SZ_D);
	//swap USP and SSP if not already in supervisor mode
	dst = bt_irdisp8(dst, 5, CONTEXT, offsetof(m68k_context, status), SZ_B);
	uint8_t *already_supervisor = dst+1;
	dst = jcc(dst, CC_C, dst+2);
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SCRATCH2, SZ_D);
	dst = mov_rrdisp8(dst, opts->aregs[7], CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
	dst = mov_rr(dst, SCRATCH2, opts->aregs[7], SZ_D);
	*already_supervisor = dst - (already_supervisor+1);
	//save PC
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_32_lowfirst);
	//save status register
	dst = sub_ir(dst, 2, opts->aregs[7], SZ_D);
	dst = call(dst, (uint8_t *)get_sr);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_16);
	//update status register
	dst = and_irdisp8(dst, 0xF8, CONTEXT, offsetof(m68k_context, status), SZ_B);
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, int_num), SCRATCH1, SZ_B);
	dst = or_ir(dst, 0x20, SCRATCH1, SZ_B);
	dst = or_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, status), SZ_B);
	//calculate interrupt vector address
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, int_num), SCRATCH1, SZ_D);
	dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, int_ack), SZ_W);
	dst = shl_ir(dst, 2, SCRATCH1, SZ_D);
	dst = add_ir(dst, 0x60, SCRATCH1, SZ_D);
	dst = call(dst, opts->read_32);
	dst = call(dst, (uint8_t *)m68k_native_addr_and_sync);
	dst = cycles(dst, 24);
	//discard function return address
	dst = pop_r(dst, SCRATCH2);
	dst = jmp_r(dst, SCRATCH1);

	opts->trap = dst;
	dst = push_r(dst, SCRATCH2);
	//swap USP and SSP if not already in supervisor mode
	dst = bt_irdisp8(dst, 5, CONTEXT, offsetof(m68k_context, status), SZ_B);
	already_supervisor = dst+1;
	dst = jcc(dst, CC_C, dst+2);
	dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SCRATCH2, SZ_D);
	dst = mov_rrdisp8(dst, opts->aregs[7], CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
	dst = mov_rr(dst, SCRATCH2, opts->aregs[7], SZ_D);
	*already_supervisor = dst - (already_supervisor+1);
	//save PC
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_32_lowfirst);
	//save status register
	dst = sub_ir(dst, 2, opts->aregs[7], SZ_D);
	dst = call(dst, (uint8_t *)get_sr);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, opts->write_16);
	//set supervisor bit
	dst = or_irdisp8(dst, 0x20, CONTEXT, offsetof(m68k_context, status), SZ_B);
	//calculate vector address
	dst = pop_r(dst, SCRATCH1);
	dst = shl_ir(dst, 2, SCRATCH1, SZ_D);
	dst = call(dst, opts->read_32);
	dst = call(dst, (uint8_t *)m68k_native_addr_and_sync);
	dst = cycles(dst, 18);
	dst = jmp_r(dst, SCRATCH1);

	opts->cur_code = dst;
}

void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts)
{
	memset(context, 0, sizeof(m68k_context));
	context->native_code_map = native_code_map;
	context->options = opts;
	context->int_cycle = 0xFFFFFFFF;
	context->status = 0x27;
}

