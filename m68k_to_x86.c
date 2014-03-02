/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gen_x86.h"
#include "m68k_core.h"
#include "68kinst.h"
#include "mem.h"
#include "backend.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define BUS 4
#define PREDEC_PENALTY 2

#define CYCLES RAX
#define LIMIT RBP
#define CONTEXT RSI
#define SCRATCH1 RCX

#ifdef X86_64
#define SCRATCH2 RDI
#else
#define SCRATCH2 RBX
#endif

enum {
	FLAG_X,
	FLAG_N,
	FLAG_Z,
	FLAG_V,
	FLAG_C
};

char disasm_buf[1024];

m68k_context * sync_components(m68k_context * context, uint32_t address);

void m68k_invalid();
void bcd_add();
void bcd_sub();


void set_flag(m68k_options * opts, uint8_t val, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_ir(&opts->gen.code, val, opts->flag_regs[flag], SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_irdisp(&opts->gen.code, val, opts->gen.context_reg, offset, SZ_B);
		} else {
			mov_irind(&opts->gen.code, val, opts->gen.context_reg, SZ_B);
		}
	}
}

void set_flag_cond(m68k_options *opts, uint8_t cond, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		setcc_r(&opts->gen.code, cond, opts->flag_regs[flag]);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			setcc_rdisp(&opts->gen.code, cond, opts->gen.context_reg, offset);
		} else {
			setcc_rind(&opts->gen.code, cond, opts->gen.context_reg);
		}
	}
}

void check_flag(m68k_options *opts, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		cmp_ir(&opts->gen.code, 0, opts->flag_regs[flag], SZ_B);
	} else {
		cmp_irdisp(&opts->gen.code, 0, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_rdispr(&opts->gen.code, opts->gen.context_reg, offset, reg, SZ_B);
		} else {
			mov_rindr(&opts->gen.code, opts->gen.context_reg, reg, SZ_B);
		}
	}
}

void reg_to_flag(m68k_options *opts, uint8_t reg, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		mov_rr(&opts->gen.code, reg, opts->flag_regs[flag], SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags) + flag;
		if (offset) {
			mov_rrdisp(&opts->gen.code, reg, opts->gen.context_reg, offset, SZ_B);
		} else {
			mov_rrind(&opts->gen.code, reg, opts->gen.context_reg, SZ_B);
		}
	}
}

void flag_to_flag(m68k_options *opts, uint8_t flag1, uint8_t flag2)
{
	code_info *code = &opts->gen.code;
	if (opts->flag_regs[flag1] >= 0 && opts->flag_regs[flag2] >= 0) {
		mov_rr(code, opts->flag_regs[flag1], opts->flag_regs[flag2], SZ_B);
	} else if(opts->flag_regs[flag1] >= 0) {
		mov_rrdisp(code, opts->flag_regs[flag1], opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	} else if (opts->flag_regs[flag2] >= 0) {
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->flag_regs[flag2], SZ_B);
	} else {
		push_r(code, opts->gen.scratch1);
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->gen.scratch1, SZ_B);
		mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
		pop_r(code, opts->gen.scratch1);
	}
}

void flag_to_carry(m68k_options * opts, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		bt_ir(&opts->gen.code, 0, opts->flag_regs[flag], SZ_B);
	} else {
		bt_irdisp(&opts->gen.code, 0, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void or_flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		or_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		or_rdispr(&opts->gen.code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, reg, SZ_B);
	}
}

void xor_flag_to_reg(m68k_options *opts, uint8_t flag, uint8_t reg)
{
	if (opts->flag_regs[flag] >= 0) {
		xor_rr(&opts->gen.code, opts->flag_regs[flag], reg, SZ_B);
	} else {
		xor_rdispr(&opts->gen.code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, reg, SZ_B);
	}
}

void xor_flag(m68k_options *opts, uint8_t val, uint8_t flag)
{
	if (opts->flag_regs[flag] >= 0) {
		xor_ir(&opts->gen.code, val, opts->flag_regs[flag], SZ_B);
	} else {
		xor_irdisp(&opts->gen.code, val, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, SZ_B);
	}
}

void cmp_flags(m68k_options *opts, uint8_t flag1, uint8_t flag2)
{
	code_info *code = &opts->gen.code;
	if (opts->flag_regs[flag1] >= 0 && opts->flag_regs[flag2] >= 0) {
		cmp_rr(code, opts->flag_regs[flag1], opts->flag_regs[flag2], SZ_B);
	} else if(opts->flag_regs[flag1] >= 0 || opts->flag_regs[flag2] >= 0) {
		if (opts->flag_regs[flag2] >= 0) {
			uint8_t tmp = flag1;
			flag1 = flag2;
			flag2 = tmp;
		}
		cmp_rrdisp(code, opts->flag_regs[flag1], opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	} else {
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag1, opts->gen.scratch1, SZ_B);
		cmp_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, flags) + flag2, SZ_B);
	}
}

int8_t native_reg(m68k_op_info * op, m68k_options * opts)
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

void m68k_read_size(m68k_options *opts, uint8_t size)
{
	switch (size)
	{
	case OPSIZE_BYTE:
		call(&opts->gen.code, opts->read_8);
		break;
	case OPSIZE_WORD:
		call(&opts->gen.code, opts->read_16);
		break;
	case OPSIZE_LONG:
		call(&opts->gen.code, opts->read_32);
		break;
	}
}

void m68k_write_size(m68k_options *opts, uint8_t size)
{
	switch (size)
	{
	case OPSIZE_BYTE:
		call(&opts->gen.code, opts->write_8);
		break;
	case OPSIZE_WORD:
		call(&opts->gen.code, opts->write_16);
		break;
	case OPSIZE_LONG:
		call(&opts->gen.code, opts->write_32_highfirst);
		break;
	}
}

void translate_m68k_src(m68kinst * inst, x86_ea * ea, m68k_options * opts)
{
	code_info *code = &opts->gen.code;
	int8_t reg = native_reg(&(inst->src), opts);
	uint8_t sec_reg;
	int32_t dec_amount,inc_amount;
	if (reg >= 0) {
		ea->mode = MODE_REG_DIRECT;
		if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
			movsx_rr(code, reg, opts->gen.scratch1, SZ_W, SZ_D);
			ea->base = opts->gen.scratch1;
		} else {
			ea->base = reg;
		}
		return;
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
			ea->base = opts->gen.context_reg;
			ea->disp = reg_offset(&(inst->src));
		} else {
			if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
				movsx_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_W, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, inst->extra.size);
			}
			ea->mode = MODE_REG_DIRECT;
			ea->base = opts->gen.scratch1;
			//we're explicitly handling the areg dest here, so we exit immediately
			return;
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->src.params.regs.pri == 7 ? 2 :1));
		cycles(&opts->gen, PREDEC_PENALTY);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			sub_ir(code, dec_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
		} else {
			sub_irdisp(code, dec_amount, opts->gen.context_reg, reg_offset(&(inst->src)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		m68k_read_size(opts, inst->extra.size);

		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->src.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				add_ir(code, inc_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
			} else {
				add_irdisp(code, inc_amount, opts->gen.context_reg, reg_offset(&(inst->src)), SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) ? opts->gen.scratch2 : opts->gen.scratch1;
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		m68k_read_size(opts, inst->extra.size);

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 6);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		m68k_read_size(opts, inst->extra.size);

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, BUS);
		mov_ir(code, inst->src.params.regs.displacement + inst->address+2, opts->gen.scratch1, SZ_D);
		m68k_read_size(opts, inst->extra.size);

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, 6);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		m68k_read_size(opts, inst->extra.size);

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		cycles(&opts->gen, inst->src.addr_mode == MODE_ABSOLUTE ? BUS*2 : BUS);
		mov_ir(code, inst->src.params.immed, opts->gen.scratch1, SZ_D);
		m68k_read_size(opts, inst->extra.size);

		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_IMMEDIATE:
	case MODE_IMMEDIATE_WORD:
		if (inst->variant != VAR_QUICK) {
			cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG && inst->src.addr_mode == MODE_IMMEDIATE) ? BUS*2 : BUS);
		}
		ea->mode = MODE_IMMED;
		ea->disp = inst->src.params.immed;
		if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD && ea->disp & 0x8000) {
			ea->disp |= 0xFFFF0000;
		}
		return;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	if (inst->dst.addr_mode == MODE_AREG && inst->extra.size == OPSIZE_WORD) {
		if (ea->mode == MODE_REG_DIRECT) {
			movsx_rr(code, ea->base, opts->gen.scratch1, SZ_W, SZ_D);
		} else {
			movsx_rdispr(code, ea->base, ea->disp, opts->gen.scratch1, SZ_W, SZ_D);
			ea->mode = MODE_REG_DIRECT;
		}
		ea->base = opts->gen.scratch1;
	}
}

void translate_m68k_dst(m68kinst * inst, x86_ea * ea, m68k_options * opts, uint8_t fake_read)
{
	code_info *code = &opts->gen.code;
	int8_t reg = native_reg(&(inst->dst), opts), sec_reg;
	int32_t dec_amount, inc_amount;
	if (reg >= 0) {
		ea->mode = MODE_REG_DIRECT;
		ea->base = reg;
		return;
	}
	switch (inst->dst.addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		ea->mode = MODE_REG_DISPLACE8;
		ea->base = opts->gen.context_reg;
		ea->disp = reg_offset(&(inst->dst));
		break;
	case MODE_AREG_PREDEC:
		if (inst->src.addr_mode == MODE_AREG_PREDEC) {
			push_r(code, opts->gen.scratch1);
		}
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			sub_ir(code, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			sub_irdisp(code, dec_amount, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (fake_read) {
			cycles(&opts->gen, inst->extra.size == OPSIZE_LONG ? 8 : 4);
		} else {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch1, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch1, SZ_D);
			}
			m68k_read_size(opts, inst->extra.size);
		}
		if (inst->src.addr_mode == MODE_AREG_PREDEC) {
			//restore src operand to opts->gen.scratch2
			pop_r(code, opts->gen.scratch2);
		} else {
			//save reg value in opts->gen.scratch2 so we can use it to save the result in memory later
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
			}
		}

		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				add_ir(code, inc_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				add_irdisp(code, inc_amount, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, fake_read ? BUS+(inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : BUS);
		reg = fake_read ? opts->gen.scratch2 : opts->gen.scratch1;
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], reg, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), reg, SZ_D);
		}
		add_ir(code, inst->dst.params.regs.displacement, reg, SZ_D);
		if (!fake_read) {
			push_r(code, opts->gen.scratch1);
			m68k_read_size(opts, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, fake_read ? (6 + inst->extra.size == OPSIZE_LONG ? 8 : 4) : 6);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), opts->gen.scratch1, SZ_D);
		}
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		if (fake_read) {
			mov_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
		} else {
			push_r(code, opts->gen.scratch1);
			m68k_read_size(opts, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, fake_read ? BUS+(inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : BUS);
		mov_ir(code, inst->dst.params.regs.displacement + inst->address+2, fake_read ? opts->gen.scratch2 : opts->gen.scratch1, SZ_D);
		if (!fake_read) {
			push_r(code, opts->gen.scratch1);
			m68k_read_size(opts, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, fake_read ? (6 + inst->extra.size == OPSIZE_LONG ? 8 : 4) : 6);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		if (fake_read) {
			mov_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
		} else {
			push_r(code, opts->gen.scratch1);
			m68k_read_size(opts, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//Add cycles for reading address from instruction stream
		cycles(&opts->gen, (inst->dst.addr_mode == MODE_ABSOLUTE ? BUS*2 : BUS) + (fake_read ? (inst->extra.size == OPSIZE_LONG ? BUS*2 : BUS) : 0));
		mov_ir(code, inst->dst.params.immed, fake_read ? opts->gen.scratch2 : opts->gen.scratch1, SZ_D);
		if (!fake_read) {
			push_r(code, opts->gen.scratch1);
			m68k_read_size(opts, inst->extra.size);
			pop_r(code, opts->gen.scratch2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = opts->gen.scratch1;
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		exit(1);
	}
}

void m68k_save_result(m68kinst * inst, m68k_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG) {
		if (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->src.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
			}
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			call(code, opts->write_8);
			break;
		case OPSIZE_WORD:
			call(code, opts->write_16);
			break;
		case OPSIZE_LONG:
			call(code, opts->write_32_lowfirst);
			break;
		}
	}
}

code_ptr get_native_address(native_map_slot * native_code_map, uint32_t address)
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

code_ptr get_native_from_context(m68k_context * context, uint32_t address)
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

void map_native_address(m68k_context * context, uint32_t address, code_ptr native_addr, uint8_t size, uint8_t native_size)
{
	native_map_slot * native_code_map = context->native_code_map;
	m68k_options * opts = context->options;
	address &= 0xFFFFFF;
	if (address > 0xE00000) {
		context->ram_code_flags[(address & 0xC000) >> 14] |= 1 << ((address & 0x3800) >> 11);
		if (((address & 0x3FFF) + size) & 0xC000) {
			context->ram_code_flags[((address+size) & 0xC000) >> 14] |= 1 << (((address+size) & 0x3800) >> 11);
		}
		uint32_t slot = (address & 0xFFFF)/1024;
		if (!opts->gen.ram_inst_sizes[slot]) {
			opts->gen.ram_inst_sizes[slot] = malloc(sizeof(uint8_t) * 512);
		}
		opts->gen.ram_inst_sizes[slot][((address & 0xFFFF)/2)%512] = native_size;
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

uint8_t get_native_inst_size(m68k_options * opts, uint32_t address)
{
	if (address < 0xE00000) {
		return 0;
	}
	uint32_t slot = (address & 0xFFFF)/1024;
	return opts->gen.ram_inst_sizes[slot][((address & 0xFFFF)/2)%512];
}

void translate_m68k_move(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t reg, flags_reg, sec_reg;
	uint8_t dir = 0;
	int32_t offset;
	int32_t inc_amount, dec_amount;
	x86_ea src;
	translate_m68k_src(inst, &src, opts);
	reg = native_reg(&(inst->dst), opts);
	if (inst->dst.addr_mode != MODE_AREG) {
		//update statically set flags
		set_flag(opts, 0, FLAG_V);
		set_flag(opts, 0, FLAG_C);
	}

	if (inst->dst.addr_mode != MODE_AREG) {
		if (src.mode == MODE_REG_DIRECT) {
			flags_reg = src.base;
		} else {
			if (reg >= 0) {
				flags_reg = reg;
			} else {
				if(src.mode == MODE_REG_DISPLACE8) {
					mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
				} else {
					mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
				}
				src.mode = MODE_REG_DIRECT;
				flags_reg = src.base = opts->gen.scratch1;
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
				mov_rr(code, src.base, reg, size);
			} else if (src.mode == MODE_REG_DISPLACE8) {
				mov_rdispr(code, src.base, src.disp, reg, size);
			} else {
				mov_ir(code, src.disp, reg, size);
			}
		} else if(src.mode == MODE_REG_DIRECT) {
			mov_rrdisp(code, src.base, opts->gen.context_reg, reg_offset(&(inst->dst)), size);
		} else {
			mov_irdisp(code, src.disp, opts->gen.context_reg, reg_offset(&(inst->dst)), size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			sub_ir(code, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			sub_irdisp(code, dec_amount, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);

		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : (inst->dst.params.regs.pri == 7 ? 2 : 1));
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				add_ir(code, inc_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				add_irdisp(code, inc_amount, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
			}
		}
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
		}
		add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 6);//TODO: Check to make sure this is correct
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
		}
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			}
		} else {
			if (src.base == opts->gen.scratch1) {
				push_r(code, opts->gen.scratch1);
			}
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
			if (src.base == opts->gen.scratch1) {
				pop_r(code, opts->gen.scratch1);
			}
		}
		if (inst->dst.params.regs.displacement) {
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, BUS);
		mov_ir(code, inst->dst.params.regs.displacement + inst->address+2, opts->gen.scratch2, SZ_D);
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, 6);//TODO: Check to make sure this is correct
		mov_ir(code, inst->address, opts->gen.scratch2, SZ_D);
		sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
		if (inst->dst.params.regs.sec & 1) {
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			}
		} else {
			if (src.base == opts->gen.scratch1) {
				push_r(code, opts->gen.scratch1);
			}
			if (inst->dst.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
			if (src.base == opts->gen.scratch1) {
				pop_r(code, opts->gen.scratch1);
			}
		}
		if (inst->dst.params.regs.displacement) {
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
		}
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		if (src.mode == MODE_REG_DIRECT) {
			if (src.base != opts->gen.scratch1) {
				mov_rr(code, src.base, opts->gen.scratch1, inst->extra.size);
			}
		} else if (src.mode == MODE_REG_DISPLACE8) {
			mov_rdispr(code, src.base, src.disp, opts->gen.scratch1, inst->extra.size);
		} else {
			mov_ir(code, src.disp, opts->gen.scratch1, inst->extra.size);
		}
		if (inst->dst.addr_mode == MODE_ABSOLUTE) {
			cycles(&opts->gen, BUS*2);
		} else {
			cycles(&opts->gen, BUS);
		}
		mov_ir(code, inst->dst.params.immed, opts->gen.scratch2, SZ_D);
		if (inst->dst.addr_mode != MODE_AREG) {
			cmp_ir(code, 0, flags_reg, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		m68k_write_size(opts, inst->extra.size);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (move dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		exit(1);
	}

	//add cycles for prefetch
	cycles(&opts->gen, BUS);
}

void translate_m68k_movem(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
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
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
			}
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			reg = opts->gen.scratch2;
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
			}
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
			}
			sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
			if (inst->dst.params.regs.sec & 1) {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						add_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						add_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
					}
				}
			} else {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
					}
				}
				add_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
			}
			if (inst->dst.params.regs.displacement) {
				add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
			}
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			mov_ir(code, inst->dst.params.regs.displacement + inst->address+2, opts->gen.scratch2, SZ_D);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			mov_ir(code, inst->address+2, opts->gen.scratch2, SZ_D);
			sec_reg = (inst->dst.params.regs.sec >> 1) & 0x7;
			if (inst->dst.params.regs.sec & 1) {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						add_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						add_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
					}
				}
			} else {
				if (inst->dst.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
					}
				}
				add_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
			}
			if (inst->dst.params.regs.displacement) {
				add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
			}
			break;
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			mov_ir(code, inst->dst.params.immed, opts->gen.scratch2, SZ_D);
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
		cycles(&opts->gen, early_cycles);
		for(bit=0; reg < 16 && reg >= 0; reg += dir, bit++) {
			if (inst->src.params.immed & (1 << bit)) {
				if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
					sub_ir(code, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch2, SZ_D);
				}
				push_r(code, opts->gen.scratch2);
				if (reg > 7) {
					if (opts->aregs[reg-8] >= 0) {
						mov_rr(code, opts->aregs[reg-8], opts->gen.scratch1, inst->extra.size);
					} else {
						mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * (reg-8), opts->gen.scratch1, inst->extra.size);
					}
				} else {
					if (opts->dregs[reg] >= 0) {
						mov_rr(code, opts->dregs[reg], opts->gen.scratch1, inst->extra.size);
					} else {
						mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * (reg), opts->gen.scratch1, inst->extra.size);
					}
				}
				if (inst->extra.size == OPSIZE_LONG) {
					call(code, opts->write_32_lowfirst);
				} else {
					call(code, opts->write_16);
				}
				pop_r(code, opts->gen.scratch2);
				if (inst->dst.addr_mode != MODE_AREG_PREDEC) {
					add_ir(code, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch2, SZ_D);
				}
			}
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				mov_rr(code, opts->gen.scratch2, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				mov_rrdisp(code, opts->gen.scratch2, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
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
				mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
			}
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			reg = opts->gen.scratch2;
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
			}
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
			}
			sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
			if (inst->src.params.regs.sec & 1) {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
					}
				}
			} else {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
					}
				}
				add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
			}
			if (inst->src.params.regs.displacement) {
				add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
			}
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			mov_ir(code, inst->src.params.regs.displacement + inst->address+2, opts->gen.scratch1, SZ_D);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
			sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
			if (inst->src.params.regs.sec & 1) {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
					} else {
						add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
					}
				}
			} else {
				if (inst->src.params.regs.sec & 0x10) {
					if (opts->aregs[sec_reg] >= 0) {
						movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
					}
				} else {
					if (opts->dregs[sec_reg] >= 0) {
						movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
					} else {
						movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
					}
				}
				add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
			}
			if (inst->src.params.regs.displacement) {
				add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
			}
			break;
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			mov_ir(code, inst->src.params.immed, opts->gen.scratch1, SZ_D);
			break;
		default:
			m68k_disasm(inst, disasm_buf);
			printf("%X: %s\naddress mode %d not implemented (movem src)\n", inst->address, disasm_buf, inst->src.addr_mode);
			exit(1);
		}
		cycles(&opts->gen, early_cycles);
		for(reg = 0; reg < 16; reg ++) {
			if (inst->dst.params.immed & (1 << reg)) {
				push_r(code, opts->gen.scratch1);
				if (inst->extra.size == OPSIZE_LONG) {
					call(code, opts->read_32);
				} else {
					call(code, opts->read_16);
				}
				if (inst->extra.size == OPSIZE_WORD) {
					movsx_rr(code, opts->gen.scratch1, opts->gen.scratch1, SZ_W, SZ_D);
				}
				if (reg > 7) {
					if (opts->aregs[reg-8] >= 0) {
						mov_rr(code, opts->gen.scratch1, opts->aregs[reg-8], SZ_D);
					} else {
						mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * (reg-8), SZ_D);
					}
				} else {
					if (opts->dregs[reg] >= 0) {
						mov_rr(code, opts->gen.scratch1, opts->dregs[reg], SZ_D);
					} else {
						mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * (reg), SZ_D);
					}
				}
				pop_r(code, opts->gen.scratch1);
				add_ir(code, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch1, SZ_D);
			}
		}
		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				mov_rr(code, opts->gen.scratch1, opts->aregs[inst->src.params.regs.pri], SZ_D);
			} else {
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->src)), SZ_D);
			}
		}
	}
	//prefetch
	cycles(&opts->gen, 4);
}

void translate_m68k_clr(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	set_flag(opts, 0, FLAG_N);
	set_flag(opts, 0, FLAG_V);
	set_flag(opts, 0, FLAG_C);
	set_flag(opts, 1, FLAG_Z);
	int8_t reg = native_reg(&(inst->dst), opts);
	if (reg >= 0) {
		cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 6 : 4));
		xor_rr(code, reg, reg, inst->extra.size);
		return;
	}
	x86_ea dst_op;
	translate_m68k_dst(inst, &dst_op, opts, 1);
	if (dst_op.mode == MODE_REG_DIRECT) {
		xor_rr(code, dst_op.base, dst_op.base, inst->extra.size);
	} else {
		mov_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_ext(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	x86_ea dst_op;
	uint8_t dst_size = inst->extra.size;
	inst->extra.size--;
	translate_m68k_dst(inst, &dst_op, opts, 0);
	if (dst_op.mode == MODE_REG_DIRECT) {
		movsx_rr(code, dst_op.base, dst_op.base, inst->extra.size, dst_size);
		cmp_ir(code, 0, dst_op.base, dst_size);
	} else {
		movsx_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch1, inst->extra.size, dst_size);
		cmp_ir(code, 0, opts->gen.scratch1, dst_size);
		mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, dst_size);
	}
	inst->extra.size = dst_size;
	set_flag(opts, 0, FLAG_V);
	set_flag(opts, 0, FLAG_C);
	set_flag_cond(opts, CC_Z, FLAG_Z);
	set_flag_cond(opts, CC_S, FLAG_N);
	//M68K EXT only operates on registers so no need for a call to save result here
}

void translate_m68k_lea(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t dst_reg = native_reg(&(inst->dst), opts), sec_reg;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		cycles(&opts->gen, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			if (dst_reg >= 0) {
				mov_rr(code, opts->aregs[inst->src.params.regs.pri], dst_reg, SZ_D);
			} else {
				mov_rrdisp(code, opts->aregs[inst->src.params.regs.pri], opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
		} else {
			if (dst_reg >= 0) {
				mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, dst_reg, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, opts->gen.scratch1, SZ_D);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
		}
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, 8);
		if (dst_reg >= 0) {
			if (inst->src.params.regs.pri != inst->dst.params.regs.pri) {
				if (opts->aregs[inst->src.params.regs.pri] >= 0) {
					mov_rr(code, opts->aregs[inst->src.params.regs.pri], dst_reg, SZ_D);
				} else {
					mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), dst_reg, SZ_D);
				}
			}
			add_ir(code, inst->src.params.regs.displacement, dst_reg, SZ_D);
		} else {
			if (inst->src.params.regs.pri != inst->dst.params.regs.pri) {
				if (opts->aregs[inst->src.params.regs.pri] >= 0) {
					mov_rrdisp(code, opts->aregs[inst->src.params.regs.pri], opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
				} else {
					mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
					mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
				}
			}
			add_irdisp(code, inst->src.params.regs.displacement, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 12);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch2, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch2, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch2, SZ_D);
		}
		if (dst_reg >= 0) {
			mov_rr(code, opts->gen.scratch2, dst_reg, SZ_D);
		} else {
			mov_rrdisp(code, opts->gen.scratch2, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, 8);
		if (dst_reg >= 0) {
			mov_ir(code, inst->src.params.regs.displacement + inst->address+2, dst_reg, SZ_D);
		} else {
			mov_irdisp(code, inst->src.params.regs.displacement + inst->address+2, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		if (dst_reg >= 0) {
			mov_rr(code, opts->gen.scratch1, dst_reg, SZ_D);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		cycles(&opts->gen, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		if (dst_reg >= 0) {
			mov_ir(code, inst->src.params.immed, dst_reg, SZ_D);
		} else {
			mov_irdisp(code, inst->src.params.immed, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_D);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
}

void translate_m68k_pea(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	uint8_t sec_reg;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		cycles(&opts->gen, BUS);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, opts->gen.scratch1, SZ_D);
		}
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, 8);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 6);//TODO: Check to make sure this is correct
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, 8);
		mov_ir(code, inst->src.params.regs.displacement + inst->address+2, opts->gen.scratch1, SZ_D);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		cycles(&opts->gen, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		mov_ir(code, inst->src.params.immed, opts->gen.scratch1, SZ_D);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	sub_ir(code, 4, opts->aregs[7], SZ_D);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_32_lowfirst);
}

void translate_m68k_bsr(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + (inst->variant == VAR_BYTE ? 2 : 4);
	//TODO: Add cycles in the right place relative to pushing the return address on the stack
	cycles(&opts->gen, 10);
	mov_ir(code, after, opts->gen.scratch1, SZ_D);
	sub_ir(code, 4, opts->aregs[7], SZ_D);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_32_highfirst);
	code_ptr dest_addr = get_native_address(opts->gen.native_code_map, (inst->address+2) + disp);
	if (!dest_addr) {
		opts->gen.deferred = defer_address(opts->gen.deferred, (inst->address+2) + disp, code->cur + 1);
		//dummy address to be replaced later
		dest_addr = code->cur + 256;
	}
	jmp(code, dest_addr);
}

uint8_t m68k_eval_cond(m68k_options * opts, uint8_t cc)
{
	uint8_t cond = CC_NZ;
	switch (cc)
	{
	case COND_HIGH:
		cond = CC_Z;
	case COND_LOW_SAME:
		flag_to_reg(opts, FLAG_Z, opts->gen.scratch1);
		or_flag_to_reg(opts, FLAG_C, opts->gen.scratch1);
		break;
	case COND_CARRY_CLR:
		cond = CC_Z;
	case COND_CARRY_SET:
		check_flag(opts, FLAG_C);
		break;
	case COND_NOT_EQ:
		cond = CC_Z;
	case COND_EQ:
		check_flag(opts, FLAG_Z);
		break;
	case COND_OVERF_CLR:
		cond = CC_Z;
	case COND_OVERF_SET:
		check_flag(opts, FLAG_V);
		break;
	case COND_PLUS:
		cond = CC_Z;
	case COND_MINUS:
		check_flag(opts, FLAG_N);
		break;
	case COND_GREATER_EQ:
		cond = CC_Z;
	case COND_LESS:
		cmp_flags(opts, FLAG_N, FLAG_V);
		break;
	case COND_GREATER:
		cond = CC_Z;
	case COND_LESS_EQ:
		flag_to_reg(opts, FLAG_V, opts->gen.scratch1);
		xor_flag_to_reg(opts, FLAG_N, opts->gen.scratch1);
		or_flag_to_reg(opts, FLAG_Z, opts->gen.scratch1);
		break;
	}
	return cond;
}

void translate_m68k_bcc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	cycles(&opts->gen, 10);//TODO: Adjust this for branch not taken case
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + 2;
	code_ptr dest_addr = get_native_address(opts->gen.native_code_map, after + disp);
	if (inst->extra.cond == COND_TRUE) {
		if (!dest_addr) {
			opts->gen.deferred = defer_address(opts->gen.deferred, after + disp, code->cur + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = code->cur + 256;
		}
		jmp(code, dest_addr);
	} else {
		uint8_t cond = m68k_eval_cond(opts, inst->extra.cond);
		if (!dest_addr) {
			opts->gen.deferred = defer_address(opts->gen.deferred, after + disp, code->cur + 2);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = code->cur + 256;
		}
		jcc(code, cond, dest_addr);
	}
}

void translate_m68k_scc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	uint8_t cond = inst->extra.cond;
	x86_ea dst_op;
	inst->extra.size = OPSIZE_BYTE;
	translate_m68k_dst(inst, &dst_op, opts, 1);
	if (cond == COND_TRUE || cond == COND_FALSE) {
		if ((inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG) && inst->extra.cond == COND_TRUE) {
			cycles(&opts->gen, 6);
		} else {
			cycles(&opts->gen, BUS);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, cond == COND_TRUE ? 0xFF : 0, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, cond == COND_TRUE ? 0xFF : 0, dst_op.base, dst_op.disp, SZ_B);
		}
	} else {
		uint8_t cc = m68k_eval_cond(opts, cond);
		check_alloc_code(code, 6*MAX_INST_LEN);
		code_ptr true_off = code->cur + 1;
		jcc(code, cc, code->cur+2);
		cycles(&opts->gen, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, 0, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, 0, dst_op.base, dst_op.disp, SZ_B);
		}
		code_ptr end_off = code->cur+1;
		jmp(code, code->cur+2);
		*true_off = code->cur - (true_off+1);
		cycles(&opts->gen, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_ir(code, 0xFF, dst_op.base, SZ_B);
		} else {
			mov_irdisp(code, 0xFF, dst_op.base, dst_op.disp, SZ_B);
		}
		*end_off = code->cur - (end_off+1);
	}
	m68k_save_result(inst, opts);
}

void translate_m68k_jmp_jsr(m68k_options * opts, m68kinst * inst)
{
	uint8_t is_jsr = inst->op == M68K_JSR;
	code_info *code = &opts->gen.code;
	code_ptr dest_addr;
	uint8_t sec_reg;
	uint32_t after;
	uint32_t m68k_addr;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		cycles(&opts->gen, BUS*2);
		if (is_jsr) {
			mov_ir(code, inst->address + 2, opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, opts->gen.scratch1, SZ_D);
		}
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS*2);
		if (is_jsr) {
			mov_ir(code, inst->address + 4, opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, opts->gen.scratch1, SZ_D);
		}
		add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);//TODO: CHeck that this is correct
		if (is_jsr) {
			mov_ir(code, inst->address + 4, opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg,  reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			//32-bit index register
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			//16-bit index register
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_PC_DISPLACE:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		cycles(&opts->gen, 10);
		if (is_jsr) {
			mov_ir(code, inst->address + 4, opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		m68k_addr = inst->src.params.regs.displacement + inst->address + 2;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->gen.native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->gen.deferred = defer_address(opts->gen.deferred, m68k_addr, code->cur + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = code->cur + 256;
			}
			jmp(code, dest_addr);
		} else {
			mov_ir(code, m68k_addr, opts->gen.scratch1, SZ_D);
			call(code, opts->native_addr);
			jmp_r(code, opts->gen.scratch1);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);//TODO: CHeck that this is correct
		if (is_jsr) {
			mov_ir(code, inst->address + 4, opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		sec_reg = (inst->src.params.regs.sec >> 1) & 0x7;
		if (inst->src.params.regs.sec & 1) {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					add_rr(code, opts->aregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					add_rr(code, opts->dregs[sec_reg], opts->gen.scratch1, SZ_D);
				} else {
					add_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch1, SZ_D);
				}
			}
		} else {
			if (inst->src.params.regs.sec & 0x10) {
				if (opts->aregs[sec_reg] >= 0) {
					movsx_rr(code, opts->aregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			} else {
				if (opts->dregs[sec_reg] >= 0) {
					movsx_rr(code, opts->dregs[sec_reg], opts->gen.scratch2, SZ_W, SZ_D);
				} else {
					movsx_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t)*sec_reg, opts->gen.scratch2, SZ_W, SZ_D);
				}
			}
			add_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		cycles(&opts->gen, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		if (is_jsr) {
			mov_ir(code, inst->address + (inst->src.addr_mode == MODE_ABSOLUTE ? 6 : 4), opts->gen.scratch1, SZ_D);
			sub_ir(code, 4, opts->aregs[7], SZ_D);
			mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
			call(code, opts->write_32_highfirst);
		}
		m68k_addr = inst->src.params.immed;
		if ((m68k_addr & 0xFFFFFF) < 0x400000) {
			dest_addr = get_native_address(opts->gen.native_code_map, m68k_addr);
			if (!dest_addr) {
				opts->gen.deferred = defer_address(opts->gen.deferred, m68k_addr, code->cur + 1);
				//dummy address to be replaced later, make sure it generates a 4-byte displacement
				dest_addr = code->cur + 256;
			}
			jmp(code, dest_addr);
		} else {
			mov_ir(code, m68k_addr, opts->gen.scratch1, SZ_D);
			call(code, opts->native_addr);
			jmp_r(code, opts->gen.scratch1);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%s\naddress mode %d not yet supported (%s)\n", disasm_buf, inst->src.addr_mode, is_jsr ? "jsr" : "jmp");
		exit(1);
	}
}

void translate_m68k_rts(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	//TODO: Add cycles
	mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
	add_ir(code, 4, opts->aregs[7], SZ_D);
	call(code, opts->read_32);
	call(code, opts->native_addr);
	jmp_r(code, opts->gen.scratch1);
}

void translate_m68k_dbcc(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	//best case duration
	cycles(&opts->gen, 10);
	code_ptr skip_loc = NULL;
	//TODO: Check if COND_TRUE technically valid here even though
	//it's basically a slow NOP
	if (inst->extra.cond != COND_FALSE) {
		uint8_t cond = m68k_eval_cond(opts, inst->extra.cond);
		check_alloc_code(code, 6*MAX_INST_LEN);
		skip_loc = code->cur + 1;
		jcc(code, cond, code->cur + 2);
	}
	if (opts->dregs[inst->dst.params.regs.pri] >= 0) {
		sub_ir(code, 1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
		cmp_ir(code, -1, opts->dregs[inst->dst.params.regs.pri], SZ_W);
	} else {
		sub_irdisp(code, 1, opts->gen.context_reg, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
		cmp_irdisp(code, -1, opts->gen.context_reg, offsetof(m68k_context, dregs) + 4 * inst->dst.params.regs.pri, SZ_W);
	}
	code_ptr loop_end_loc = code->cur + 1;
	jcc(code, CC_Z, code->cur + 2);
	uint32_t after = inst->address + 2;
	code_ptr dest_addr = get_native_address(opts->gen.native_code_map, after + inst->src.params.immed);
	if (!dest_addr) {
		opts->gen.deferred = defer_address(opts->gen.deferred, after + inst->src.params.immed, code->cur + 1);
		//dummy address to be replaced later, make sure it generates a 4-byte displacement
		dest_addr = code->cur + 256;
	}
	jmp(code, dest_addr);
	*loop_end_loc = code->cur - (loop_end_loc+1);
	if (skip_loc) {
		cycles(&opts->gen, 2);
		*skip_loc = code->cur - (skip_loc+1);
		cycles(&opts->gen, 2);
	} else {
		cycles(&opts->gen, 4);
	}
}

void translate_m68k_link(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t reg = native_reg(&(inst->src), opts);
	//compensate for displacement word
	cycles(&opts->gen, BUS);
	sub_ir(code, 4, opts->aregs[7], SZ_D);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	if (reg >= 0) {
		mov_rr(code, reg, opts->gen.scratch1, SZ_D);
	} else {
		mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
	}
	call(code, opts->write_32_highfirst);
	if (reg >= 0) {
		mov_rr(code, opts->aregs[7], reg, SZ_D);
	} else {
		mov_rrdisp(code, opts->aregs[7], opts->gen.context_reg, reg_offset(&(inst->src)), SZ_D);
	}
	add_ir(code, inst->dst.params.immed, opts->aregs[7], SZ_D);
	//prefetch
	cycles(&opts->gen, BUS);
}

void translate_m68k_movep(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t reg;
	cycles(&opts->gen, BUS*2);
	if (inst->src.addr_mode == MODE_REG) {
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->dst.params.regs.pri], opts->gen.scratch2, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->dst)), opts->gen.scratch2, SZ_D);
		}
		if (inst->dst.params.regs.displacement) {
			add_ir(code, inst->dst.params.regs.displacement, opts->gen.scratch2, SZ_D);
		}
		reg = native_reg(&(inst->src), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				mov_rr(code, reg, opts->gen.scratch1, SZ_D);
				shr_ir(code, 24, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch2);
				call(code, opts->write_8);
				pop_r(code, opts->gen.scratch2);
				mov_rr(code, reg, opts->gen.scratch1, SZ_D);
				shr_ir(code, 16, opts->gen.scratch1, SZ_D);

			} else {
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+3, opts->gen.scratch1, SZ_B);
				push_r(code, opts->gen.scratch2);
				call(code, opts->write_8);
				pop_r(code, opts->gen.scratch2);
				mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+2, opts->gen.scratch1, SZ_B);
			}
			add_ir(code, 2, opts->gen.scratch2, SZ_D);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			add_ir(code, 2, opts->gen.scratch2, SZ_D);
		}
		if (reg >= 0) {
			mov_rr(code, reg, opts->gen.scratch1, SZ_W);
			shr_ir(code, 8, opts->gen.scratch1, SZ_W);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			mov_rr(code, reg, opts->gen.scratch1, SZ_W);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src))+1, opts->gen.scratch1, SZ_B);
			push_r(code, opts->gen.scratch2);
			call(code, opts->write_8);
			pop_r(code, opts->gen.scratch2);
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_B);
		}
		add_ir(code, 2, opts->gen.scratch2, SZ_D);
		call(code, opts->write_8);
	} else {
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			mov_rr(code, opts->aregs[inst->src.params.regs.pri], opts->gen.scratch1, SZ_D);
		} else {
			mov_rdispr(code, opts->gen.context_reg, reg_offset(&(inst->src)), opts->gen.scratch1, SZ_D);
		}
		if (inst->src.params.regs.displacement) {
			add_ir(code, inst->src.params.regs.displacement, opts->gen.scratch1, SZ_D);
		}
		reg = native_reg(&(inst->dst), opts);
		if (inst->extra.size == OPSIZE_LONG) {
			if (reg >= 0) {
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				shl_ir(code, 24, opts->gen.scratch1, SZ_D);
				mov_rr(code, opts->gen.scratch1, reg, SZ_D);
				pop_r(code, opts->gen.scratch1);
				add_ir(code, 2, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				shl_ir(code, 16, opts->gen.scratch1, SZ_D);
				or_rr(code, opts->gen.scratch1, reg, SZ_D);
			} else {
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+3, SZ_B);
				pop_r(code, opts->gen.scratch1);
				add_ir(code, 2, opts->gen.scratch1, SZ_D);
				push_r(code, opts->gen.scratch1);
				call(code, opts->read_8);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+2, SZ_B);
			}
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
		}
		push_r(code, opts->gen.scratch1);
		call(code, opts->read_8);
		if (reg >= 0) {

			shl_ir(code, 8, opts->gen.scratch1, SZ_W);
			mov_rr(code, opts->gen.scratch1, reg, SZ_W);
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
			call(code, opts->read_8);
			mov_rr(code, opts->gen.scratch1, reg, SZ_B);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst))+1, SZ_B);
			pop_r(code, opts->gen.scratch1);
			add_ir(code, 2, opts->gen.scratch1, SZ_D);
			call(code, opts->read_8);
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, reg_offset(&(inst->dst)), SZ_B);
		}
	}
}

void translate_m68k_cmp(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	uint8_t size = inst->extra.size;
	x86_ea src_op, dst_op;
	translate_m68k_src(inst, &src_op, opts);
	if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
		push_r(code, opts->gen.scratch1);
		translate_m68k_dst(inst, &dst_op, opts, 0);
		pop_r(code, opts->gen.scratch2);
		src_op.base = opts->gen.scratch2;
	} else {
		translate_m68k_dst(inst, &dst_op, opts, 0);
		if (inst->dst.addr_mode == MODE_AREG && size == OPSIZE_WORD) {
			size = OPSIZE_LONG;
		}
	}
	cycles(&opts->gen, BUS);
	if (src_op.mode == MODE_REG_DIRECT) {
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_rr(code, src_op.base, dst_op.base, size);
		} else {
			cmp_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
		}
	} else if (src_op.mode == MODE_REG_DISPLACE8) {
		cmp_rdispr(code, src_op.base, src_op.disp, dst_op.base, size);
	} else {
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, src_op.disp, dst_op.base, size);
		} else {
			cmp_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, size);
		}
	}
	set_flag_cond(opts, CC_C, FLAG_C);
	set_flag_cond(opts, CC_Z, FLAG_Z);
	set_flag_cond(opts, CC_S, FLAG_N);
	set_flag_cond(opts, CC_O, FLAG_V);
}

typedef void (*shift_ir_t)(code_info *code, uint8_t val, uint8_t dst, uint8_t size);
typedef void (*shift_irdisp_t)(code_info *code, uint8_t val, uint8_t dst_base, int32_t disp, uint8_t size);
typedef void (*shift_clr_t)(code_info *code, uint8_t dst, uint8_t size);
typedef void (*shift_clrdisp_t)(code_info *code, uint8_t dst_base, int32_t disp, uint8_t size);

void translate_shift(m68k_options * opts, m68kinst * inst, x86_ea *src_op, x86_ea * dst_op, shift_ir_t shift_ir, shift_irdisp_t shift_irdisp, shift_clr_t shift_clr, shift_clrdisp_t shift_clrdisp, shift_ir_t special, shift_irdisp_t special_disp)
{
	code_info *code = &opts->gen.code;
	code_ptr end_off = NULL;
	code_ptr nz_off = NULL;
	code_ptr z_off = NULL;
	if (inst->src.addr_mode == MODE_UNUSED) {
		cycles(&opts->gen, BUS);
		//Memory shift
		shift_ir(code, 1, dst_op->base, SZ_W);
	} else {
		cycles(&opts->gen, inst->extra.size == OPSIZE_LONG ? 8 : 6);
		if (src_op->mode == MODE_IMMED) {
			if (src_op->disp != 1 && inst->op == M68K_ASL) {
				set_flag(opts, 0, FLAG_V);
				for (int i = 0; i < src_op->disp; i++) {
					if (dst_op->mode == MODE_REG_DIRECT) {
						shift_ir(code, 1, dst_op->base, inst->extra.size);
					} else {
						shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}
					check_alloc_code(code, 2*MAX_INST_LEN);
					code_ptr after_flag_set = code->cur + 1;
					jcc(code, CC_NO, code->cur + 2);
					set_flag(opts, 1, FLAG_V);
					*after_flag_set = code->cur - (after_flag_set+1);
				}
			} else {
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_ir(code, src_op->disp, dst_op->base, inst->extra.size);
				} else {
					shift_irdisp(code, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
				}
				set_flag_cond(opts, CC_O, FLAG_V);
			}
		} else {
			if (src_op->base != RCX) {
				if (src_op->mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op->base, RCX, SZ_B);
				} else {
					mov_rdispr(code, src_op->base, src_op->disp, RCX, SZ_B);
				}

			}
			and_ir(code, 63, RCX, SZ_D);
			check_alloc_code(code, 7*MAX_INST_LEN);
			nz_off = code->cur + 1;
			jcc(code, CC_NZ, code->cur + 2);
			//Flag behavior for shift count of 0 is different for x86 than 68K
			if (dst_op->mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op->base, inst->extra.size);
			} else {
				cmp_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
			}
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			set_flag(opts, 0, FLAG_C);
			//For other instructions, this flag will be set below
			if (inst->op == M68K_ASL) {
				set_flag(opts, 0, FLAG_V);
			}
			z_off = code->cur + 1;
			jmp(code, code->cur + 2);
			*nz_off = code->cur - (nz_off + 1);
			//add 2 cycles for every bit shifted
			add_rr(code, RCX, CYCLES, SZ_D);
			add_rr(code, RCX, CYCLES, SZ_D);
			if (inst->op == M68K_ASL) {
				//ASL has Overflow flag behavior that depends on all of the bits shifted through the MSB
				//Easiest way to deal with this is to shift one bit at a time
				set_flag(opts, 0, FLAG_V);
				check_alloc_code(code, 5*MAX_INST_LEN);
				code_ptr loop_start = code->cur;
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_ir(code, 1, dst_op->base, inst->extra.size);
				} else {
					shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
				}
				code_ptr after_flag_set = code->cur + 1;
				jcc(code, CC_NO, code->cur + 2);
				set_flag(opts, 1, FLAG_V);
				*after_flag_set = code->cur - (after_flag_set+1);
				loop(code, loop_start);
			} else {
				//x86 shifts modulo 32 for operand sizes less than 64-bits
				//but M68K shifts modulo 64, so we need to check for large shifts here
				cmp_ir(code, 32, RCX, SZ_B);
				check_alloc_code(code, 14*MAX_INST_LEN);
				code_ptr norm_shift_off = code->cur + 1;
				jcc(code, CC_L, code->cur + 2);
				if (special) {
					code_ptr after_flag_set = NULL;
					if (inst->extra.size == OPSIZE_LONG) {
						code_ptr neq_32_off = code->cur + 1;
						jcc(code, CC_NZ, code->cur + 2);

						//set the carry bit to the lsb
						if (dst_op->mode == MODE_REG_DIRECT) {
							special(code, 1, dst_op->base, SZ_D);
						} else {
							special_disp(code, 1, dst_op->base, dst_op->disp, SZ_D);
						}
						set_flag_cond(opts, CC_C, FLAG_C);
						after_flag_set = code->cur + 1;
						jmp(code, code->cur + 2);
						*neq_32_off = code->cur - (neq_32_off+1);
					}
					set_flag(opts, 0, FLAG_C);
					if (after_flag_set) {
						*after_flag_set = code->cur - (after_flag_set+1);
					}
					set_flag(opts, 1, FLAG_Z);
					set_flag(opts, 0, FLAG_N);
					if (dst_op->mode == MODE_REG_DIRECT) {
						xor_rr(code, dst_op->base, dst_op->base, inst->extra.size);
					} else {
						mov_irdisp(code, 0, dst_op->base, dst_op->disp, inst->extra.size);
					}
				} else {
					if (dst_op->mode == MODE_REG_DIRECT) {
						shift_ir(code, 31, dst_op->base, inst->extra.size);
						shift_ir(code, 1, dst_op->base, inst->extra.size);
					} else {
						shift_irdisp(code, 31, dst_op->base, dst_op->disp, inst->extra.size);
						shift_irdisp(code, 1, dst_op->base, dst_op->disp, inst->extra.size);
					}

				}
				end_off = code->cur + 1;
				jmp(code, code->cur + 2);
				*norm_shift_off = code->cur - (norm_shift_off+1);
				if (dst_op->mode == MODE_REG_DIRECT) {
					shift_clr(code, dst_op->base, inst->extra.size);
				} else {
					shift_clrdisp(code, dst_op->base, dst_op->disp, inst->extra.size);
				}
			}
		}

	}
	if (!special && end_off) {
		*end_off = code->cur - (end_off + 1);
	}
	set_flag_cond(opts, CC_C, FLAG_C);
	set_flag_cond(opts, CC_Z, FLAG_Z);
	set_flag_cond(opts, CC_S, FLAG_N);
	if (special && end_off) {
		*end_off = code->cur - (end_off + 1);
	}
	//set X flag to same as C flag
	if (opts->flag_regs[FLAG_C] >= 0) {
		flag_to_flag(opts, FLAG_C, FLAG_X);
	} else {
		set_flag_cond(opts, CC_C, FLAG_X);
	}
	if (z_off) {
		*z_off = code->cur - (z_off + 1);
	}
	if (inst->op != M68K_ASL) {
		set_flag(opts, 0, FLAG_V);
	}
	if (inst->src.addr_mode == MODE_UNUSED) {
		m68k_save_result(inst, opts);
	}
}

#define BIT_SUPERVISOR 5

void translate_m68k(m68k_options * opts, m68kinst * inst)
{
	code_ptr end_off, zero_off, norm_off;
	uint8_t dst_reg;
	code_info *code = &opts->gen.code;
	check_cycles_int(&opts->gen, inst->address);
	if (inst->op == M68K_MOVE) {
		return translate_m68k_move(opts, inst);
	} else if(inst->op == M68K_LEA) {
		return translate_m68k_lea(opts, inst);
	} else if(inst->op == M68K_PEA) {
		return translate_m68k_pea(opts, inst);
	} else if(inst->op == M68K_BSR) {
		return translate_m68k_bsr(opts, inst);
	} else if(inst->op == M68K_BCC) {
		return translate_m68k_bcc(opts, inst);
	} else if(inst->op == M68K_JMP) {
		return translate_m68k_jmp_jsr(opts, inst);
	} else if(inst->op == M68K_JSR) {
		return translate_m68k_jmp_jsr(opts, inst);
	} else if(inst->op == M68K_RTS) {
		return translate_m68k_rts(opts, inst);
	} else if(inst->op == M68K_DBCC) {
		return translate_m68k_dbcc(opts, inst);
	} else if(inst->op == M68K_CLR) {
		return translate_m68k_clr(opts, inst);
	} else if(inst->op == M68K_MOVEM) {
		return translate_m68k_movem(opts, inst);
	} else if(inst->op == M68K_LINK) {
		return translate_m68k_link(opts, inst);
	} else if(inst->op == M68K_EXT) {
		return translate_m68k_ext(opts, inst);
	} else if(inst->op == M68K_SCC) {
		return translate_m68k_scc(opts, inst);
	} else if(inst->op == M68K_MOVEP) {
		return translate_m68k_movep(opts, inst);
	} else if(inst->op == M68K_INVALID) {
		if (inst->src.params.immed == 0x7100) {
			return retn(code);
		}
		mov_ir(code, inst->address, opts->gen.scratch1, SZ_D);
		return call(code, (code_ptr)m68k_invalid);
	} else if(inst->op == M68K_CMP) {
		return translate_m68k_cmp(opts, inst);
	}
	x86_ea src_op, dst_op;
	if (inst->src.addr_mode != MODE_UNUSED) {
		translate_m68k_src(inst, &src_op, opts);
	}
	if (inst->dst.addr_mode != MODE_UNUSED) {
		translate_m68k_dst(inst, &dst_op, opts, 0);
	}
	uint8_t size;
	switch(inst->op)
	{
	case M68K_ABCD:
		if (src_op.base != opts->gen.scratch2) {
			if (src_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, src_op.base, opts->gen.scratch2, SZ_B);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch2, SZ_B);
			}
		}
		if (dst_op.base != opts->gen.scratch1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, dst_op.base, opts->gen.scratch1, SZ_B);
			} else {
				mov_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch1, SZ_B);
			}
		}
		flag_to_carry(opts, FLAG_X);
		jcc(code, CC_NC, code->cur + 5);
		add_ir(code, 1, opts->gen.scratch1, SZ_B);
		call(code, (code_ptr)bcd_add);
		reg_to_flag(opts, CH, FLAG_C);
		reg_to_flag(opts, CH, FLAG_X);
		cmp_ir(code, 0, opts->gen.scratch1, SZ_B);
		jcc(code, CC_Z, code->cur + 4);
		set_flag(opts, 0, FLAG_Z);
		if (dst_op.base != opts->gen.scratch1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, opts->gen.scratch1, dst_op.base, SZ_B);
			} else {
				mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
			}
		}
		m68k_save_result(inst, opts);
		break;
	case M68K_ADD:
		cycles(&opts->gen, BUS);
		size = inst->dst.addr_mode == MODE_AREG ? OPSIZE_LONG : inst->extra.size;
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				add_rr(code, src_op.base, dst_op.base, size);
			} else {
				add_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			add_rdispr(code, src_op.base, src_op.disp, dst_op.base, size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				add_ir(code, src_op.disp, dst_op.base, size);
			} else {
				add_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, size);
			}
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			set_flag_cond(opts, CC_C, FLAG_C);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			set_flag_cond(opts, CC_O, FLAG_V);
			if (opts->flag_regs[FLAG_C] >= 0) {
				flag_to_flag(opts, FLAG_C, FLAG_X);
			} else {
				set_flag_cond(opts, CC_C, FLAG_X);
			}
		}
		m68k_save_result(inst, opts);
		break;
	case M68K_ADDX: {
		cycles(&opts->gen, BUS);
		flag_to_carry(opts, FLAG_X);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				adc_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else {
				adc_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			adc_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				adc_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				adc_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		set_flag_cond(opts, CC_C, FLAG_C);

		check_alloc_code(code, 2*MAX_INST_LEN);
		code_ptr after_flag_set = code->cur + 1;
		jcc(code, CC_Z, code->cur + 2);
		set_flag(opts, 0, FLAG_Z);
		*after_flag_set = code->cur - (after_flag_set+1);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag_cond(opts, CC_O, FLAG_V);
		if (opts->flag_regs[FLAG_C] >= 0) {
			flag_to_flag(opts, FLAG_C, FLAG_X);
		} else {
			set_flag_cond(opts, CC_C, FLAG_X);
		}
		m68k_save_result(inst, opts);
		break;
	}
	case M68K_AND:
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				and_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else {
				and_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			and_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				and_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				and_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		m68k_save_result(inst, opts);
		break;
	case M68K_ANDI_CCR:
	case M68K_ANDI_SR:
		cycles(&opts->gen, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (!(inst->src.params.immed & 0x1)) {
			set_flag(opts, 0, FLAG_C);
		}
		if (!(inst->src.params.immed & 0x2)) {
			set_flag(opts, 0, FLAG_V);
		}
		if (!(inst->src.params.immed & 0x4)) {
			set_flag(opts, 0, FLAG_Z);
		}
		if (!(inst->src.params.immed & 0x8)) {
			set_flag(opts, 0, FLAG_N);
		}
		if (!(inst->src.params.immed & 0x10)) {
			set_flag(opts, 0, FLAG_X);
		}
		if (inst->op == M68K_ANDI_SR) {
			and_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
			if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
				//leave supervisor mode
				mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_B);
				mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_B);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_B);
			}
			if (inst->src.params.immed & 0x700) {
				call(code, opts->do_sync);
			}
		}
		break;
	case M68K_ASL:
	case M68K_LSL:
		translate_shift(opts, inst, &src_op, &dst_op, shl_ir, shl_irdisp, shl_clr, shl_clrdisp, shr_ir, shr_irdisp);
		break;
	case M68K_ASR:
		translate_shift(opts, inst, &src_op, &dst_op, sar_ir, sar_irdisp, sar_clr, sar_clrdisp, NULL, NULL);
		break;
	case M68K_LSR:
		translate_shift(opts, inst, &src_op, &dst_op, shr_ir, shr_irdisp, shr_clr, shr_clrdisp, shl_ir, shl_irdisp);
		break;
	case M68K_BCHG:
	case M68K_BCLR:
	case M68K_BSET:
	case M68K_BTST:
		cycles(&opts->gen, inst->extra.size == OPSIZE_BYTE ? 4 : (
			inst->op == M68K_BTST ? 6 : (inst->op == M68K_BCLR ? 10 : 8))
		);
		if (src_op.mode == MODE_IMMED) {
			if (inst->extra.size == OPSIZE_BYTE) {
				src_op.disp &= 0x7;
			}
			if (inst->op == M68K_BTST) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					bt_ir(code, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					bt_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else if (inst->op == M68K_BSET) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					bts_ir(code, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					bts_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else if (inst->op == M68K_BCLR) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					btr_ir(code, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					btr_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					btc_ir(code, src_op.disp, dst_op.base, inst->extra.size);
				} else {
					btc_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
				}
			}
		} else {
			if (src_op.mode == MODE_REG_DISPLACE8 || (inst->dst.addr_mode != MODE_REG && src_op.base != opts->gen.scratch1 && src_op.base != opts->gen.scratch2)) {
				if (dst_op.base == opts->gen.scratch1) {
					push_r(code, opts->gen.scratch2);
					if (src_op.mode == MODE_REG_DIRECT) {
						mov_rr(code, src_op.base, opts->gen.scratch2, SZ_B);
					} else {
						mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch2, SZ_B);
					}
					src_op.base = opts->gen.scratch2;
				} else {
					if (src_op.mode == MODE_REG_DIRECT) {
						mov_rr(code, src_op.base, opts->gen.scratch1, SZ_B);
					} else {
						mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
					}
					src_op.base = opts->gen.scratch1;
				}
			}
			uint8_t size = inst->extra.size;
			if (dst_op.mode == MODE_REG_DISPLACE8) {
				if (src_op.base != opts->gen.scratch1 && src_op.base != opts->gen.scratch2) {
					if (src_op.mode == MODE_REG_DIRECT) {
						mov_rr(code, src_op.base, opts->gen.scratch1, SZ_D);
					} else {
						mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_D);
						src_op.mode = MODE_REG_DIRECT;
					}
					src_op.base = opts->gen.scratch1;
				}
				//b### with register destination is modulo 32
				//x86 with a memory destination isn't modulo anything
				//so use an and here to force the value to be modulo 32
				and_ir(code, 31, opts->gen.scratch1, SZ_D);
			} else if(inst->dst.addr_mode != MODE_REG) {
				//b### with memory destination is modulo 8
				//x86-64 doesn't support 8-bit bit operations
				//so we fake it by forcing the bit number to be modulo 8
				and_ir(code, 7, src_op.base, SZ_D);
				size = SZ_D;
			}
			if (inst->op == M68K_BTST) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					bt_rr(code, src_op.base, dst_op.base, size);
				} else {
					bt_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else if (inst->op == M68K_BSET) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					bts_rr(code, src_op.base, dst_op.base, size);
				} else {
					bts_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else if (inst->op == M68K_BCLR) {
				if (dst_op.mode == MODE_REG_DIRECT) {
					btr_rr(code, src_op.base, dst_op.base, size);
				} else {
					btr_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
				}
			} else {
				if (dst_op.mode == MODE_REG_DIRECT) {
					btc_rr(code, src_op.base, dst_op.base, size);
				} else {
					btc_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
				}
			}
			if (src_op.base == opts->gen.scratch2) {
				pop_r(code, opts->gen.scratch2);
			}
		}
		//x86 sets the carry flag to the value of the bit tested
		//68K sets the zero flag to the complement of the bit tested
		set_flag_cond(opts, CC_NC, FLAG_Z);
		if (inst->op != M68K_BTST) {
			m68k_save_result(inst, opts);
		}
		break;
	case M68K_CHK:
	{
		cycles(&opts->gen, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, dst_op.base, inst->extra.size);
		} else {
			cmp_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
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
		//make sure we won't start a new chunk in the middle of these branches
		check_alloc_code(code, MAX_INST_LEN * 11);
		code_ptr passed = code->cur + 1;
		jcc(code, CC_GE, code->cur + 2);
		set_flag(opts, 1, FLAG_N);
		mov_ir(code, VECTOR_CHK, opts->gen.scratch2, SZ_D);
		mov_ir(code, inst->address+isize, opts->gen.scratch1, SZ_D);
		jmp(code, opts->trap);
		*passed = code->cur - (passed+1);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (src_op.mode == MODE_REG_DIRECT) {
				cmp_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else if(src_op.mode == MODE_REG_DISPLACE8) {
				cmp_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				cmp_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			}
		} else if(dst_op.mode == MODE_REG_DISPLACE8) {
			if (src_op.mode == MODE_REG_DIRECT) {
				cmp_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			} else {
				cmp_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		passed = code->cur + 1;
		jcc(code, CC_LE, code->cur + 2);
		set_flag(opts, 0, FLAG_N);
		mov_ir(code, VECTOR_CHK, opts->gen.scratch2, SZ_D);
		mov_ir(code, inst->address+isize, opts->gen.scratch1, SZ_D);
		jmp(code, opts->trap);
		*passed = code->cur - (passed+1);
		cycles(&opts->gen, 4);
		break;
	}
	case M68K_DIVS:
	case M68K_DIVU:
	{
		check_alloc_code(code, MAX_NATIVE_SIZE);
		//TODO: cycle exact division
		cycles(&opts->gen, inst->op == M68K_DIVS ? 158 : 140);
		set_flag(opts, 0, FLAG_C);
		push_r(code, RDX);
		push_r(code, RAX);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, RAX, SZ_D);
		} else {
			mov_rdispr(code, dst_op.base, dst_op.disp, RAX, SZ_D);
		}
		if (src_op.mode == MODE_IMMED) {
			mov_ir(code, (src_op.disp & 0x8000) && inst->op == M68K_DIVS ? src_op.disp | 0xFFFF0000 : src_op.disp, opts->gen.scratch2, SZ_D);
		} else if (src_op.mode == MODE_REG_DIRECT) {
			if (inst->op == M68K_DIVS) {
				movsx_rr(code, src_op.base, opts->gen.scratch2, SZ_W, SZ_D);
			} else {
				movzx_rr(code, src_op.base, opts->gen.scratch2, SZ_W, SZ_D);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			if (inst->op == M68K_DIVS) {
				movsx_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch2, SZ_W, SZ_D);
			} else {
				movzx_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch2, SZ_W, SZ_D);
			}
		}
		cmp_ir(code, 0, opts->gen.scratch2, SZ_D);
		check_alloc_code(code, 6*MAX_INST_LEN);
		code_ptr not_zero = code->cur + 1;
		jcc(code, CC_NZ, code->cur + 2);
		pop_r(code, RAX);
		pop_r(code, RDX);
		mov_ir(code, VECTOR_INT_DIV_ZERO, opts->gen.scratch2, SZ_D);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		jmp(code, opts->trap);
		*not_zero = code->cur - (not_zero+1);
		if (inst->op == M68K_DIVS) {
			cdq(code);
		} else {
			xor_rr(code, RDX, RDX, SZ_D);
		}
		if (inst->op == M68K_DIVS) {
			idiv_r(code, opts->gen.scratch2, SZ_D);
		} else {
			div_r(code, opts->gen.scratch2, SZ_D);
		}
		code_ptr skip_sec_check;
		if (inst->op == M68K_DIVS) {
			cmp_ir(code, 0x8000, RAX, SZ_D);
			skip_sec_check = code->cur + 1;
			jcc(code, CC_GE, code->cur + 2);
			cmp_ir(code, -0x8000, RAX, SZ_D);
			norm_off = code->cur + 1;
			jcc(code, CC_L, code->cur + 2);
		} else {
			cmp_ir(code, 0x10000, RAX, SZ_D);
			norm_off = code->cur + 1;
			jcc(code, CC_NC, code->cur + 2);
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, RDX, dst_op.base, SZ_W);
			shl_ir(code, 16, dst_op.base, SZ_D);
			mov_rr(code, RAX, dst_op.base, SZ_W);
		} else {
			mov_rrdisp(code, RDX, dst_op.base, dst_op.disp, SZ_W);
			shl_irdisp(code, 16, dst_op.base, dst_op.disp, SZ_D);
			mov_rrdisp(code, RAX, dst_op.base, dst_op.disp, SZ_W);
		}
		cmp_ir(code, 0, RAX, SZ_W);
		pop_r(code, RAX);
		pop_r(code, RDX);
		set_flag(opts, 0, FLAG_V);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		end_off = code->cur + 1;
		jmp(code, code->cur + 2);
		*norm_off = code->cur - (norm_off + 1);
		if (inst->op == M68K_DIVS) {
			*skip_sec_check = code->cur - (skip_sec_check+1);
		}
		pop_r(code, RAX);
		pop_r(code, RDX);
		set_flag(opts, 1, FLAG_V);
		*end_off = code->cur - (end_off + 1);
		break;
	}
	case M68K_EOR:
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				xor_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else {
				xor_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			xor_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				xor_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				xor_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		m68k_save_result(inst, opts);
		break;
	case M68K_EORI_CCR:
	case M68K_EORI_SR:
		cycles(&opts->gen, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (inst->src.params.immed & 0x1) {
			xor_flag(opts, 1, FLAG_C);
		}
		if (inst->src.params.immed & 0x2) {
			xor_flag(opts, 1, FLAG_V);
		}
		if (inst->src.params.immed & 0x4) {
			xor_flag(opts, 1, FLAG_Z);
		}
		if (inst->src.params.immed & 0x8) {
			xor_flag(opts, 1, FLAG_N);
		}
		if (inst->src.params.immed & 0x10) {
			xor_flag(opts, 1, FLAG_X);
		}
		if (inst->op == M68K_ORI_SR) {
			xor_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
			if (inst->src.params.immed & 0x700) {
				call(code, opts->do_sync);
			}
		}
		break;
	case M68K_EXG:
		cycles(&opts->gen, 6);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, opts->gen.scratch2, SZ_D);
			if (src_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, src_op.base, dst_op.base, SZ_D);
				mov_rr(code, opts->gen.scratch2, src_op.base, SZ_D);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, dst_op.base, SZ_D);
				mov_rrdisp(code, opts->gen.scratch2, src_op.base, src_op.disp, SZ_D);
			}
		} else {
			mov_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch2, SZ_D);
			if (src_op.mode == MODE_REG_DIRECT) {
				mov_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, SZ_D);
				mov_rr(code, opts->gen.scratch2, src_op.base, SZ_D);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_D);
				mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_D);
				mov_rrdisp(code, opts->gen.scratch2, src_op.base, src_op.disp, SZ_D);
			}
		}
		break;
	case M68K_ILLEGAL:
		call(code, opts->gen.save_context);
#ifdef X86_64
		mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
#else
		push_r(code, opts->gen.context_reg);
#endif
		call(code, (code_ptr)print_regs_exit);
		break;
	case M68K_MOVE_FROM_SR:
		//TODO: Trap if not in system mode
		call(code, opts->get_sr);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, opts->gen.scratch1, dst_op.base, SZ_W);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_W);
		}
		m68k_save_result(inst, opts);
		break;
	case M68K_MOVE_CCR:
	case M68K_MOVE_SR:
		//TODO: Privilege check for MOVE to SR
		if (src_op.mode == MODE_IMMED) {
			set_flag(opts, src_op.disp & 0x1, FLAG_C);
			set_flag(opts, (src_op.disp >> 1) & 0x1, FLAG_V);
			set_flag(opts, (src_op.disp >> 2) & 0x1, FLAG_Z);
			set_flag(opts, (src_op.disp >> 3) & 0x1, FLAG_N);
			set_flag(opts, (src_op.disp >> 4) & 0x1, FLAG_X);
			if (inst->op == M68K_MOVE_SR) {
				mov_irdisp(code, (src_op.disp >> 8), opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
				if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
					//leave supervisor mode
					mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
					mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
					mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
				}
				call(code, opts->do_sync);
			}
			cycles(&opts->gen, 12);
		} else {
			if (src_op.base != opts->gen.scratch1) {
				if (src_op.mode == MODE_REG_DIRECT) {
					mov_rr(code, src_op.base, opts->gen.scratch1, SZ_W);
				} else {
					mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_W);
				}
			}
			call(code, inst->op == M68K_MOVE_SR ? opts->set_sr : opts->set_ccr);
			cycles(&opts->gen, 12);

		}
		break;
	case M68K_MOVE_USP:
		cycles(&opts->gen, BUS);
		//TODO: Trap if not in supervisor mode
		//bt_irdisp(code, BIT_SUPERVISOR, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		if (inst->src.addr_mode == MODE_UNUSED) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, dst_op.base, SZ_D);
			} else {
				mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->gen.scratch1, SZ_D);
				mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_D);
			}
		} else {
			if (src_op.mode == MODE_REG_DIRECT) {
				mov_rrdisp(code, src_op.base, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_D);
				mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
			}
		}
		break;
	//case M68K_MOVEP:
	case M68K_MULS:
	case M68K_MULU:
		cycles(&opts->gen, 70); //TODO: Calculate the actual value based on the value of the <ea> parameter
		if (src_op.mode == MODE_IMMED) {
			mov_ir(code, inst->op == M68K_MULU ? (src_op.disp & 0xFFFF) : ((src_op.disp & 0x8000) ? src_op.disp | 0xFFFF0000 : src_op.disp), opts->gen.scratch1, SZ_D);
		} else if (src_op.mode == MODE_REG_DIRECT) {
			if (inst->op == M68K_MULS) {
				movsx_rr(code, src_op.base, opts->gen.scratch1, SZ_W, SZ_D);
			} else {
				movzx_rr(code, src_op.base, opts->gen.scratch1, SZ_W, SZ_D);
			}
		} else {
			if (inst->op == M68K_MULS) {
				movsx_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_W, SZ_D);
			} else {
				movzx_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_W, SZ_D);
			}
		}
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst_reg = dst_op.base;
			if (inst->op == M68K_MULS) {
				movsx_rr(code, dst_reg, dst_reg, SZ_W, SZ_D);
			} else {
				movzx_rr(code, dst_reg, dst_reg, SZ_W, SZ_D);
			}
		} else {
			dst_reg = opts->gen.scratch2;
			if (inst->op == M68K_MULS) {
				movsx_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch2, SZ_W, SZ_D);
			} else {
				movzx_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch2, SZ_W, SZ_D);
			}
		}
		imul_rr(code, opts->gen.scratch1, dst_reg, SZ_D);
		if (dst_op.mode == MODE_REG_DISPLACE8) {
			mov_rrdisp(code, dst_reg, dst_op.base, dst_op.disp, SZ_D);
		}
		set_flag(opts, 0, FLAG_V);
		set_flag(opts, 0, FLAG_C);
		cmp_ir(code, 0, dst_reg, SZ_D);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		break;
	//case M68K_NBCD:
	case M68K_NEG:
		cycles(&opts->gen, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			neg_r(code, dst_op.base, inst->extra.size);
		} else {
			neg_rdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
		}
		set_flag_cond(opts, CC_C, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag_cond(opts, CC_O, FLAG_V);
		if (opts->flag_regs[FLAG_C] >= 0) {
			flag_to_flag(opts, FLAG_C, FLAG_X);
		} else {
			set_flag_cond(opts, CC_C, FLAG_X);
		}
		m68k_save_result(inst, opts);
		break;
	case M68K_NEGX: {
		cycles(&opts->gen, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			if (dst_op.base == opts->gen.scratch1) {
				push_r(code, opts->gen.scratch2);
				xor_rr(code, opts->gen.scratch2, opts->gen.scratch2, inst->extra.size);
				flag_to_carry(opts, FLAG_X);
				sbb_rr(code, dst_op.base, opts->gen.scratch2, inst->extra.size);
				mov_rr(code, opts->gen.scratch2, dst_op.base, inst->extra.size);
				pop_r(code, opts->gen.scratch2);
			} else {
				xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, inst->extra.size);
				flag_to_carry(opts, FLAG_X);
				sbb_rr(code, dst_op.base, opts->gen.scratch1, inst->extra.size);
				mov_rr(code, opts->gen.scratch1, dst_op.base, inst->extra.size);
			}
		} else {
			xor_rr(code, opts->gen.scratch1, opts->gen.scratch1, inst->extra.size);
			flag_to_carry(opts, FLAG_X);
			sbb_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch1, inst->extra.size);
			mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, inst->extra.size);
		}
		set_flag_cond(opts, CC_C, FLAG_C);
		code_ptr after_flag_set = code->cur + 1;
		jcc(code, CC_Z, code->cur + 2);
		set_flag(opts, 0, FLAG_Z);
		*after_flag_set = code->cur - (after_flag_set+1);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag_cond(opts, CC_O, FLAG_V);
		if (opts->flag_regs[FLAG_C] >= 0) {
			flag_to_flag(opts, FLAG_C, FLAG_X);
		} else {
			set_flag_cond(opts, CC_C, FLAG_X);
		}
		m68k_save_result(inst, opts);
		break;
	}
	case M68K_NOP:
		cycles(&opts->gen, BUS);
		break;
	case M68K_NOT:
		if (dst_op.mode == MODE_REG_DIRECT) {
			not_r(code, dst_op.base, inst->extra.size);
			cmp_ir(code, 0, dst_op.base, inst->extra.size);
		} else {
			not_rdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
			cmp_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
		}

		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		m68k_save_result(inst, opts);
		break;
	case M68K_OR:
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				or_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else {
				or_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			or_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				or_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				or_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		m68k_save_result(inst, opts);
		break;
	case M68K_ORI_CCR:
	case M68K_ORI_SR:
		cycles(&opts->gen, 20);
		//TODO: If ANDI to SR, trap if not in supervisor mode
		if (inst->src.params.immed & 0x1) {
			set_flag(opts, 1, FLAG_C);
		}
		if (inst->src.params.immed & 0x2) {
			set_flag(opts, 1, FLAG_V);
		}
		if (inst->src.params.immed & 0x4) {
			set_flag(opts, 1, FLAG_Z);
		}
		if (inst->src.params.immed & 0x8) {
			set_flag(opts, 1, FLAG_N);
		}
		if (inst->src.params.immed & 0x10) {
			set_flag(opts, 1, FLAG_X);
		}
		if (inst->op == M68K_ORI_SR) {
			or_irdisp(code, inst->src.params.immed >> 8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
			if (inst->src.params.immed & 0x700) {
				call(code, opts->do_sync);
			}
		}
		break;
	case M68K_RESET:
		call(code, opts->gen.save_context);
#ifdef X86_64
		mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
#else
		push_r(code, opts->gen.context_reg);
#endif
		call(code, (code_ptr)print_regs_exit);
		break;
	case M68K_ROL:
	case M68K_ROR:
		set_flag(opts, 0, FLAG_V);
		if (inst->src.addr_mode == MODE_UNUSED) {
			cycles(&opts->gen, BUS);
			//Memory rotate
			if (inst->op == M68K_ROL) {
				rol_ir(code, 1, dst_op.base, inst->extra.size);
			} else {
				ror_ir(code, 1, dst_op.base, inst->extra.size);
			}
			set_flag_cond(opts, CC_C, FLAG_C);
			cmp_ir(code, 0, dst_op.base, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			m68k_save_result(inst, opts);
		} else {
			if (src_op.mode == MODE_IMMED) {
				cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + src_op.disp*2);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						rol_ir(code, src_op.disp, dst_op.base, inst->extra.size);
					} else {
						ror_ir(code, src_op.disp, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						rol_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						ror_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				set_flag_cond(opts, CC_C, FLAG_C);
			} else {
				if (src_op.mode == MODE_REG_DIRECT) {
					if (src_op.base != opts->gen.scratch1) {
						mov_rr(code, src_op.base, opts->gen.scratch1, SZ_B);
					}
				} else {
					mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
				}
				and_ir(code, 63, opts->gen.scratch1, SZ_D);
				zero_off = code->cur + 1;
				jcc(code, CC_Z, code->cur + 2);
				add_rr(code, opts->gen.scratch1, CYCLES, SZ_D);
				add_rr(code, opts->gen.scratch1, CYCLES, SZ_D);
				cmp_ir(code, 32, opts->gen.scratch1, SZ_B);
				norm_off = code->cur + 1;
				jcc(code, CC_L, code->cur + 2);
				sub_ir(code, 32, opts->gen.scratch1, SZ_B);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						rol_ir(code, 31, dst_op.base, inst->extra.size);
						rol_ir(code, 1, dst_op.base, inst->extra.size);
					} else {
						ror_ir(code, 31, dst_op.base, inst->extra.size);
						ror_ir(code, 1, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						rol_irdisp(code, 31, dst_op.base, dst_op.disp, inst->extra.size);
						rol_irdisp(code, 1, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						ror_irdisp(code, 31, dst_op.base, dst_op.disp, inst->extra.size);
						ror_irdisp(code, 1, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				*norm_off = code->cur - (norm_off+1);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROL) {
						rol_clr(code, dst_op.base, inst->extra.size);
					} else {
						ror_clr(code, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROL) {
						rol_clrdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						ror_clrdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				set_flag_cond(opts, CC_C, FLAG_C);
				end_off = code->cur + 1;
				jmp(code, code->cur + 2);
				*zero_off = code->cur - (zero_off+1);
				set_flag(opts, 0, FLAG_C);
				*end_off = code->cur - (end_off+1);
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, inst->extra.size);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
			}
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		break;
	case M68K_ROXL:
	case M68K_ROXR:
		set_flag(opts, 0, FLAG_V);
		if (inst->src.addr_mode == MODE_UNUSED) {
			cycles(&opts->gen, BUS);
			//Memory rotate
			flag_to_carry(opts, FLAG_X);
			if (inst->op == M68K_ROXL) {
				rcl_ir(code, 1, dst_op.base, inst->extra.size);
			} else {
				rcr_ir(code, 1, dst_op.base, inst->extra.size);
			}
			set_flag_cond(opts, CC_C, FLAG_C);
			if (opts->flag_regs[FLAG_C] < 0) {
				set_flag_cond(opts, CC_C, FLAG_X);
			}
			cmp_ir(code, 0, dst_op.base, inst->extra.size);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			if (opts->flag_regs[FLAG_C] >= 0) {
				flag_to_flag(opts, FLAG_C, FLAG_X);
			}
			m68k_save_result(inst, opts);
		} else {
			if (src_op.mode == MODE_IMMED) {
				cycles(&opts->gen, (inst->extra.size == OPSIZE_LONG ? 8 : 6) + src_op.disp*2);
				flag_to_carry(opts, FLAG_X);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						rcl_ir(code, src_op.disp, dst_op.base, inst->extra.size);
					} else {
						rcr_ir(code, src_op.disp, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						rcl_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						rcr_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				set_flag_cond(opts, CC_C, FLAG_C);
				if (opts->flag_regs[FLAG_C] >= 0) {
					flag_to_flag(opts, FLAG_C, FLAG_X);
				} else {
					set_flag_cond(opts, CC_C, FLAG_X);
				}
			} else {
				if (src_op.mode == MODE_REG_DIRECT) {
					if (src_op.base != opts->gen.scratch1) {
						mov_rr(code, src_op.base, opts->gen.scratch1, SZ_B);
					}
				} else {
					mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch1, SZ_B);
				}
				and_ir(code, 63, opts->gen.scratch1, SZ_D);
				zero_off = code->cur + 1;
				jcc(code, CC_Z, code->cur + 2);
				add_rr(code, opts->gen.scratch1, CYCLES, SZ_D);
				add_rr(code, opts->gen.scratch1, CYCLES, SZ_D);
				cmp_ir(code, 32, opts->gen.scratch1, SZ_B);
				norm_off = code->cur + 1;
				jcc(code, CC_L, code->cur + 2);
				flag_to_carry(opts, FLAG_X);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						rcl_ir(code, 31, dst_op.base, inst->extra.size);
						rcl_ir(code, 1, dst_op.base, inst->extra.size);
					} else {
						rcr_ir(code, 31, dst_op.base, inst->extra.size);
						rcr_ir(code, 1, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						rcl_irdisp(code, 31, dst_op.base, dst_op.disp, inst->extra.size);
						rcl_irdisp(code, 1, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						rcr_irdisp(code, 31, dst_op.base, dst_op.disp, inst->extra.size);
						rcr_irdisp(code, 1, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				set_flag_cond(opts, CC_C, FLAG_X);
				sub_ir(code, 32, opts->gen.scratch1, SZ_B);
				*norm_off = code->cur - (norm_off+1);
				flag_to_carry(opts, FLAG_X);
				if (dst_op.mode == MODE_REG_DIRECT) {
					if (inst->op == M68K_ROXL) {
						rcl_clr(code, dst_op.base, inst->extra.size);
					} else {
						rcr_clr(code, dst_op.base, inst->extra.size);
					}
				} else {
					if (inst->op == M68K_ROXL) {
						rcl_clrdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
					} else {
						rcr_clrdisp(code, dst_op.base, dst_op.disp, inst->extra.size);
					}
				}
				set_flag_cond(opts, CC_C, FLAG_C);
				if (opts->flag_regs[FLAG_C] >= 0) {
					flag_to_flag(opts, FLAG_C, FLAG_X);
				} else {
					set_flag_cond(opts, CC_C, FLAG_X);
				}
				end_off = code->cur + 1;
				jmp(code, code->cur + 2);
				*zero_off = code->cur - (zero_off+1);
				//Carry flag is set to X flag when count is 0, this is different from ROR/ROL
				flag_to_flag(opts, FLAG_X, FLAG_C);
				*end_off = code->cur - (end_off+1);
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				cmp_ir(code, 0, dst_op.base, inst->extra.size);
			} else {
				cmp_irdisp(code, 0, dst_op.base, dst_op.disp, inst->extra.size);
			}
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
		}
		break;
	case M68K_RTE:
		//TODO: Trap if not in system mode
		//Read saved SR
		mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
		call(code, opts->read_16);
		add_ir(code, 2, opts->aregs[7], SZ_D);
		call(code, opts->set_sr);
		//Read saved PC
		mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
		call(code, opts->read_32);
		add_ir(code, 4, opts->aregs[7], SZ_D);
		//Check if we've switched to user mode and swap stack pointers if needed
		bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		end_off = code->cur + 1;
		jcc(code, CC_C, code->cur + 2);
		mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
		mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
		mov_rrdisp(code, opts->gen.scratch2, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
		*end_off = code->cur - (end_off+1);
		//Get native address, sync components, recalculate integer points and jump to returned address
		call(code, opts->native_addr_and_sync);
		jmp_r(code, opts->gen.scratch1);
		break;
	case M68K_RTR:
		//Read saved CCR
		mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
		call(code, opts->read_16);
		add_ir(code, 2, opts->aregs[7], SZ_D);
		call(code, opts->set_ccr);
		//Read saved PC
		mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
		call(code, opts->read_32);
		add_ir(code, 4, opts->aregs[7], SZ_D);
		//Get native address and jump to it
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case M68K_SBCD: {
		if (src_op.base != opts->gen.scratch2) {
			if (src_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, src_op.base, opts->gen.scratch2, SZ_B);
			} else {
				mov_rdispr(code, src_op.base, src_op.disp, opts->gen.scratch2, SZ_B);
			}
		}
		if (dst_op.base != opts->gen.scratch1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, dst_op.base, opts->gen.scratch1, SZ_B);
			} else {
				mov_rdispr(code, dst_op.base, dst_op.disp, opts->gen.scratch1, SZ_B);
			}
		}
		flag_to_carry(opts, FLAG_X);
		jcc(code, CC_NC, code->cur + 5);
		sub_ir(code, 1, opts->gen.scratch1, SZ_B);
		call(code, (code_ptr)bcd_sub);
		reg_to_flag(opts, CH, FLAG_C);
		reg_to_flag(opts, CH, FLAG_X);
		cmp_ir(code, 0, opts->gen.scratch1, SZ_B);
		code_ptr after_flag_set = code->cur+1;
		jcc(code, CC_Z, code->cur + 2);
		set_flag(opts, 0, FLAG_Z);
		*after_flag_set = code->cur - (after_flag_set+1);
		if (dst_op.base != opts->gen.scratch1) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				mov_rr(code, opts->gen.scratch1, dst_op.base, SZ_B);
			} else {
				mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_B);
			}
		}
		m68k_save_result(inst, opts);
		break;
	}
	case M68K_STOP: {
		//TODO: Trap if not in system mode
		//manual says 4 cycles, but it has to be at least 8 since it's a 2-word instruction
		//possibly even 12 since that's how long MOVE to SR takes
		cycles(&opts->gen, BUS*2);
		set_flag(opts, src_op.disp & 0x1, FLAG_C);
		set_flag(opts, (src_op.disp >> 1) & 0x1, FLAG_V);
		set_flag(opts, (src_op.disp >> 2) & 0x1, FLAG_Z);
		set_flag(opts, (src_op.disp >> 3) & 0x1, FLAG_N);
		set_flag(opts, (src_op.disp >> 4) & 0x1, FLAG_X);
		mov_irdisp(code, (src_op.disp >> 8), opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
		if (!((inst->src.params.immed >> 8) & (1 << BIT_SUPERVISOR))) {
			//leave supervisor mode
			mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
			mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
		}
		code_ptr loop_top = code->cur;
		call(code, opts->do_sync);
		cmp_rr(code, opts->gen.limit, opts->gen.cycles, SZ_D);
		code_ptr normal_cycle_up = code->cur + 1;
		jcc(code, CC_A, code->cur + 2);
		cycles(&opts->gen, BUS);
		code_ptr after_cycle_up = code->cur + 1;
		jmp(code, code->cur + 2);
		*normal_cycle_up = code->cur - (normal_cycle_up + 1);
		mov_rr(code, opts->gen.limit, opts->gen.cycles, SZ_D);
		*after_cycle_up = code->cur - (after_cycle_up+1);
		cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_cycle), opts->gen.cycles, SZ_D);
		jcc(code, CC_C, loop_top);
		break;
	}
	case M68K_SUB:
		size = inst->dst.addr_mode == MODE_AREG ? OPSIZE_LONG : inst->extra.size;
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				sub_rr(code, src_op.base, dst_op.base, size);
			} else {
				sub_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			sub_rdispr(code, src_op.base, src_op.disp, dst_op.base, size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				sub_ir(code, src_op.disp, dst_op.base, size);
			} else {
				sub_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, size);
			}
		}
		if (inst->dst.addr_mode != MODE_AREG) {
			set_flag_cond(opts, CC_C, FLAG_C);
			set_flag_cond(opts, CC_Z, FLAG_Z);
			set_flag_cond(opts, CC_S, FLAG_N);
			set_flag_cond(opts, CC_O, FLAG_V);
			if (opts->flag_regs[FLAG_C] >= 0) {
				flag_to_flag(opts, FLAG_C, FLAG_X);
			} else {
				set_flag_cond(opts, CC_C, FLAG_X);
			}
		}
		m68k_save_result(inst, opts);
		break;
	case M68K_SUBX: {
		cycles(&opts->gen, BUS);
		flag_to_carry(opts, FLAG_X);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				sbb_rr(code, src_op.base, dst_op.base, inst->extra.size);
			} else {
				sbb_rrdisp(code, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			sbb_rdispr(code, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				sbb_ir(code, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				sbb_irdisp(code, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		set_flag_cond(opts, CC_C, FLAG_C);
		if (opts->flag_regs[FLAG_C] < 0) {
			set_flag_cond(opts, CC_C, FLAG_X);
		}
		code_ptr after_flag_set = code->cur + 1;
		jcc(code, CC_Z, code->cur + 2);
		set_flag(opts, 0, FLAG_Z);
		*after_flag_set = code->cur - (after_flag_set+1);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag_cond(opts, CC_O, FLAG_V);
		if (opts->flag_regs[FLAG_C] >= 0) {
			flag_to_flag(opts, FLAG_C, FLAG_X);
		}
		m68k_save_result(inst, opts);
		break;
	}
	case M68K_SWAP:
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			rol_ir(code, 16, src_op.base, SZ_D);
			cmp_ir(code, 0, src_op.base, SZ_D);
		} else{
			rol_irdisp(code, 16, src_op.base, src_op.disp, SZ_D);
			cmp_irdisp(code, 0, src_op.base, src_op.disp, SZ_D);
		}

		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		break;
	//case M68K_TAS:
	case M68K_TRAP:
		mov_ir(code, src_op.disp + VECTOR_TRAP_0, opts->gen.scratch2, SZ_D);
		mov_ir(code, inst->address+2, opts->gen.scratch1, SZ_D);
		jmp(code, opts->trap);
		break;
	//case M68K_TRAPV:
	case M68K_TST:
		cycles(&opts->gen, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			cmp_ir(code, 0, src_op.base, inst->extra.size);
		} else { //M68000 doesn't support immedate operand for tst, so this must be MODE_REG_DISPLACE8
			cmp_irdisp(code, 0, src_op.base, src_op.disp, inst->extra.size);
		}
		set_flag(opts, 0, FLAG_C);
		set_flag_cond(opts, CC_Z, FLAG_Z);
		set_flag_cond(opts, CC_S, FLAG_N);
		set_flag(opts, 0, FLAG_V);
		break;
	case M68K_UNLK:
		cycles(&opts->gen, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, dst_op.base, opts->aregs[7], SZ_D);
		} else {
			mov_rdispr(code, dst_op.base, dst_op.disp, opts->aregs[7], SZ_D);
		}
		mov_rr(code, opts->aregs[7], opts->gen.scratch1, SZ_D);
		call(code, opts->read_32);
		if (dst_op.mode == MODE_REG_DIRECT) {
			mov_rr(code, opts->gen.scratch1, dst_op.base, SZ_D);
		} else {
			mov_rrdisp(code, opts->gen.scratch1, dst_op.base, dst_op.disp, SZ_D);
		}
		add_ir(code, 4, opts->aregs[7], SZ_D);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\ninstruction %d not yet implemented\n", inst->address, disasm_buf, inst->op);
		exit(1);
	}
}

uint8_t m68k_is_terminal(m68kinst * inst)
{
	return inst->op == M68K_RTS || inst->op == M68K_RTE || inst->op == M68K_RTR || inst->op == M68K_JMP
		|| inst->op == M68K_TRAP || inst->op == M68K_ILLEGAL || inst->op == M68K_INVALID || inst->op == M68K_RESET
		|| (inst->op == M68K_BCC && inst->extra.cond == COND_TRUE);
}

void m68k_handle_deferred(m68k_context * context)
{
	m68k_options * opts = context->options;
	process_deferred(&opts->gen.deferred, context, (native_addr_func)get_native_from_context);
	if (opts->gen.deferred) {
		translate_m68k_stream(opts->gen.deferred->address, context);
	}
}

void translate_m68k_stream(uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	m68k_options * opts = context->options;
	code_info *code = &opts->gen.code;
	address &= 0xFFFFFF;
	if(get_native_address(opts->gen.native_code_map, address)) {
		return;
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
			if (address >= 0x400000 && address < 0xE00000) {
				xor_rr(code, RDI, RDI, SZ_D);
#ifdef X86_32
				push_r(code, RDI);
#endif
				call(code, (code_ptr)exit);
				break;
			}
			code_ptr existing = get_native_address(opts->gen.native_code_map, address);
			if (existing) {
				jmp(code, existing);
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

			//make sure the beginning of the code for an instruction is contiguous
			check_alloc_code(code, MAX_INST_LEN*4);
			code_ptr start = code->cur;
			translate_m68k(opts, &instbuf);
			code_ptr after = code->cur;
			map_native_address(context, instbuf.address, start, m68k_size, after-start);
			after;
		} while(!m68k_is_terminal(&instbuf));
		process_deferred(&opts->gen.deferred, context, (native_addr_func)get_native_from_context);
		if (opts->gen.deferred) {
			address = opts->gen.deferred->address;
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
}

code_ptr get_native_address_trans(m68k_context * context, uint32_t address)
{
	address &= 0xFFFFFF;
	code_ptr ret = get_native_address(context->native_code_map, address);
	if (!ret) {
		translate_m68k_stream(address, context);
		ret = get_native_address(context->native_code_map, address);
	}
	return ret;
}

void * m68k_retranslate_inst(uint32_t address, m68k_context * context)
{
	m68k_options * opts = context->options;
	code_info *code = &opts->gen.code;
	uint8_t orig_size = get_native_inst_size(opts, address);
	code_ptr orig_start = get_native_address(context->native_code_map, address);
	uint32_t orig = address;
	code_info orig_code;
	orig_code.cur = orig_start;
	orig_code.last = orig_start + orig_size + 5;
	address &= 0xFFFF;
	uint16_t *after, *inst = context->mem_pointers[1] + address/2;
	m68kinst instbuf;
	after = m68k_decode(inst, &instbuf, orig);
	if (orig_size != MAX_NATIVE_SIZE) {
		deferred_addr * orig_deferred = opts->gen.deferred;

		//make sure the beginning of the code for an instruction is contiguous
		check_alloc_code(code, MAX_INST_LEN*4);
		code_ptr native_start = code->cur;
		translate_m68k(opts, &instbuf);
		code_ptr native_end = code->cur;
		uint8_t is_terminal = m68k_is_terminal(&instbuf);
		if ((native_end - native_start) <= orig_size) {
			code_ptr native_next;
			if (!is_terminal) {
				native_next = get_native_address(context->native_code_map, orig + (after-inst)*2);
			}
			if (is_terminal || (native_next && ((native_next == orig_start + orig_size) || (orig_size - (native_end - native_start)) > 5))) {
				remove_deferred_until(&opts->gen.deferred, orig_deferred);
				code_info tmp;
				tmp.cur = code->cur;
				tmp.last = code->last;
				code->cur = orig_code.cur;
				code->last = orig_code.last;
				translate_m68k(opts, &instbuf);
				native_end = orig_code.cur = code->cur;
				code->cur = tmp.cur;
				code->last = tmp.last;
				if (!is_terminal) {
					if (native_next == orig_start + orig_size && (native_next-native_end) < 2) {
						while (orig_code.cur < orig_start + orig_size) {
							*(orig_code.cur++) = 0x90; //NOP
						}
					} else {
						jmp(&orig_code, native_next);
					}
				}
				m68k_handle_deferred(context);
				return orig_start;
			}
		}

		map_native_address(context, instbuf.address, native_start, (after-inst)*2, MAX_NATIVE_SIZE);

		jmp(&orig_code, native_start);
		if (!m68k_is_terminal(&instbuf)) {
			code_ptr native_end = code->cur;
			code->cur = native_start + MAX_NATIVE_SIZE;
			code_ptr rest = get_native_address_trans(context, orig + (after-inst)*2);
			code_ptr tmp = code->cur;
			code->cur = native_end;
			jmp(code, rest);
			code->cur = tmp;
		} else {
			code->cur = native_start + MAX_NATIVE_SIZE;
		}
		m68k_handle_deferred(context);
		return native_start;
	} else {
		code_info tmp;
		tmp.cur = code->cur;
		tmp.last = code->last;
		code->cur = orig_code.cur;
		code->last = orig_code.last;
		translate_m68k(opts, &instbuf);
		if (!m68k_is_terminal(&instbuf)) {
			jmp(code, get_native_address_trans(context, orig + (after-inst)*2));
		}
		code->cur = tmp.cur;
		code->last = tmp.last;
		m68k_handle_deferred(context);
		return orig_start;
	}
}

m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context)
{
	uint32_t inst_start = get_instruction_start(context->native_code_map, address | 0xFF0000);
	if (inst_start) {
		m68k_options * options = context->options;
		code_info *code = &options->gen.code;
		code_ptr dst = get_native_address(context->native_code_map, inst_start);
		code_info orig;
		orig.cur = dst;
		orig.last = dst + 128;
		mov_ir(&orig, inst_start, options->gen.scratch2, SZ_D);

		if (!options->retrans_stub) {
			options->retrans_stub = code->cur;
			call(code, options->gen.save_context);
			push_r(code, options->gen.context_reg);
#ifdef X86_32
			push_r(code, options->gen.context_reg);
			push_r(code, options->gen.scratch2);
#endif
			call(code, (code_ptr)m68k_retranslate_inst);
#ifdef X86_32
			add_ir(code, 8, RSP, SZ_D);
#endif
			pop_r(code, options->gen.context_reg);
			mov_rr(code, RAX, options->gen.scratch1, SZ_PTR);
			call(code, options->gen.load_context);
			jmp_r(code, options->gen.scratch1);
		}
		jmp(&orig, options->retrans_stub);
	}
	return context;
}

void insert_breakpoint(m68k_context * context, uint32_t address, code_ptr bp_handler)
{
	static code_ptr bp_stub = NULL;
	m68k_options * opts = context->options;
	code_info native;
	native.cur = get_native_address_trans(context, address);
	native.last = native.cur + 128;
	code_ptr start_native = native.cur;
	mov_ir(&native, address, opts->gen.scratch1, SZ_D);
	if (!bp_stub) {
		code_info *code = &opts->gen.code;
		check_alloc_code(code, 5);
		bp_stub = code->cur;
		call(&native, bp_stub);

		//Calculate length of prologue
		check_cycles_int(&opts->gen, address);
		int check_int_size = code->cur-bp_stub;
		code->cur = bp_stub;

		//Save context and call breakpoint handler
		call(code, opts->gen.save_context);
		push_r(code, opts->gen.scratch1);
#ifdef X86_64
		mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
		mov_rr(code, opts->gen.scratch1, RSI, SZ_D);
#else
		push_r(code, opts->gen.scratch1);
		push_r(code, opts->gen.context_reg);
#endif
		call(code, bp_handler);
#ifdef X86_32
		add_ir(code, 8, RSP, SZ_D);
#endif
		mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
		//Restore context
		call(code, opts->gen.load_context);
		pop_r(code, opts->gen.scratch1);
		//do prologue stuff
		cmp_rr(code, opts->gen.cycles, opts->gen.limit, SZ_D);
		code_ptr jmp_off = code->cur + 1;
		jcc(code, CC_NC, code->cur + 7);
		call(code, opts->gen.handle_cycle_limit_int);
		*jmp_off = code->cur - (jmp_off+1);
		//jump back to body of translated instruction
		pop_r(code, opts->gen.scratch1);
		add_ir(code, check_int_size - (native.cur-start_native), opts->gen.scratch1, SZ_PTR);
		jmp_r(code, opts->gen.scratch1);
	} else {
		call(&native, bp_stub);
	}
}

void remove_breakpoint(m68k_context * context, uint32_t address)
{
	code_ptr native = get_native_address(context->native_code_map, address);
	check_cycles_int(context->options, address);
}

void start_68k_context(m68k_context * context, uint32_t address)
{
	code_ptr addr = get_native_address_trans(context, address);
	m68k_options * options = context->options;
	options->start_context(addr, context);
}

void m68k_reset(m68k_context * context)
{
	//TODO: Make this actually use the normal read functions
	context->aregs[7] = context->mem_pointers[0][0] << 16 | context->mem_pointers[0][1];
	uint32_t address = context->mem_pointers[0][2] << 16 | context->mem_pointers[0][3];
	start_68k_context(context, address);
}

code_ptr gen_mem_fun(cpu_options * opts, memmap_chunk * memmap, uint32_t num_chunks, ftype fun_type)
{
	code_info *code = &opts->code;
	code_ptr start = code->cur;
	check_cycles(opts);
	cycles(opts, BUS);
	and_ir(code, 0xFFFFFF, opts->scratch1, SZ_D);
	code_ptr lb_jcc = NULL, ub_jcc = NULL;
	uint8_t is_write = fun_type == WRITE_16 || fun_type == WRITE_8;
	uint8_t adr_reg = is_write ? opts->scratch2 : opts->scratch1;
	uint16_t access_flag = is_write ? MMAP_WRITE : MMAP_READ;
	uint8_t size =  (fun_type == READ_16 || fun_type == WRITE_16) ? SZ_W : SZ_B;
	for (uint32_t chunk = 0; chunk < num_chunks; chunk++)
	{
		if (memmap[chunk].start > 0) {
			cmp_ir(code, memmap[chunk].start, adr_reg, SZ_D);
			lb_jcc = code->cur + 1;
			jcc(code, CC_C, code->cur + 2);
		}
		if (memmap[chunk].end < 0x1000000) {
			cmp_ir(code, memmap[chunk].end, adr_reg, SZ_D);
			ub_jcc = code->cur + 1;
			jcc(code, CC_NC, code->cur + 2);
		}

		if (memmap[chunk].mask != 0xFFFFFF) {
			and_ir(code, memmap[chunk].mask, adr_reg, SZ_D);
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
					cmp_irdisp(code, 0, opts->context_reg, offsetof(m68k_context, mem_pointers) + sizeof(void*) * memmap[chunk].ptr_index, SZ_PTR);
					code_ptr not_null = code->cur + 1;
					jcc(code, CC_NZ, code->cur + 2);
					call(code, opts->save_context);
#ifdef X86_64
					if (is_write) {
						if (opts->scratch2 != RDI) {
							mov_rr(code, opts->scratch2, RDI, SZ_D);
						}
						mov_rr(code, opts->scratch1, RDX, size);
					} else {
						push_r(code, opts->context_reg);
						mov_rr(code, opts->scratch1, RDI, SZ_D);
					}
					test_ir(code, 8, RSP, SZ_D);
					code_ptr adjust_rsp = code->cur + 1;
					jcc(code, CC_NZ, code->cur + 2);
					call(code, cfun);
					code_ptr no_adjust = code->cur + 1;
					jmp(code, code->cur + 2);
					*adjust_rsp = code->cur - (adjust_rsp + 1);
					sub_ir(code, 8, RSP, SZ_PTR);
					call(code, cfun);
					add_ir(code, 8, RSP, SZ_PTR);
					*no_adjust = code->cur - (no_adjust + 1);
#else
					if (is_write) {
						push_r(code, opts->scratch1);
					} else {
						push_r(code, opts->context_reg);//save opts->context_reg for later
					}
					push_r(code, opts->context_reg);
					push_r(code, is_write ? opts->scratch2 : opts->scratch1);
					call(code, cfun);
					add_ir(code, is_write ? 12 : 8, RSP, SZ_D);
#endif
					if (is_write) {
						mov_rr(code, RAX, opts->context_reg, SZ_PTR);
					} else {
						pop_r(code, opts->context_reg);
						mov_rr(code, RAX, opts->scratch1, size);
					}
					jmp(code, opts->load_context);

					*not_null = code->cur - (not_null + 1);
				}
				if (size == SZ_B) {
					xor_ir(code, 1, adr_reg, SZ_D);
				}
				add_rdispr(code, opts->context_reg, offsetof(m68k_context, mem_pointers) + sizeof(void*) * memmap[chunk].ptr_index, adr_reg, SZ_PTR);
				if (is_write) {
					mov_rrind(code, opts->scratch1, opts->scratch2, size);

				} else {
					mov_rindr(code, opts->scratch1, opts->scratch1, size);
				}
			} else {
				uint8_t tmp_size = size;
				if (size == SZ_B) {
					if ((memmap[chunk].flags & MMAP_ONLY_ODD) || (memmap[chunk].flags & MMAP_ONLY_EVEN)) {
						bt_ir(code, 0, adr_reg, SZ_D);
						code_ptr good_addr = code->cur + 1;
						jcc(code, (memmap[chunk].flags & MMAP_ONLY_ODD) ? CC_C : CC_NC, code->cur + 2);
						if (!is_write) {
							mov_ir(code, 0xFF, opts->scratch1, SZ_B);
						}
						retn(code);
						*good_addr = code->cur - (good_addr + 1);
						shr_ir(code, 1, adr_reg, SZ_D);
					} else {
						xor_ir(code, 1, adr_reg, SZ_D);
					}
				} else if ((memmap[chunk].flags & MMAP_ONLY_ODD) || (memmap[chunk].flags & MMAP_ONLY_EVEN)) {
					tmp_size = SZ_B;
					shr_ir(code, 1, adr_reg, SZ_D);
					if ((memmap[chunk].flags & MMAP_ONLY_EVEN) && is_write) {
						shr_ir(code, 8, opts->scratch1, SZ_W);
					}
				}
				if ((intptr_t)memmap[chunk].buffer <= 0x7FFFFFFF && (intptr_t)memmap[chunk].buffer >= -2147483648) {
					if (is_write) {
						mov_rrdisp(code, opts->scratch1, opts->scratch2, (intptr_t)memmap[chunk].buffer, tmp_size);
					} else {
						mov_rdispr(code, opts->scratch1, (intptr_t)memmap[chunk].buffer, opts->scratch1, tmp_size);
					}
				} else {
					if (is_write) {
						push_r(code, opts->scratch1);
						mov_ir(code, (intptr_t)memmap[chunk].buffer, opts->scratch1, SZ_PTR);
						add_rr(code, opts->scratch1, opts->scratch2, SZ_PTR);
						pop_r(code, opts->scratch1);
						mov_rrind(code, opts->scratch1, opts->scratch2, tmp_size);
					} else {
						mov_ir(code, (intptr_t)memmap[chunk].buffer, opts->scratch2, SZ_PTR);
						mov_rindexr(code, opts->scratch2, opts->scratch1, 1, opts->scratch1, tmp_size);
					}
				}
				if (size != tmp_size && !is_write) {
					if (memmap[chunk].flags & MMAP_ONLY_EVEN) {
						shl_ir(code, 8, opts->scratch1, SZ_W);
						mov_ir(code, 0xFF, opts->scratch1, SZ_B);
					} else {
						or_ir(code, 0xFF00, opts->scratch1, SZ_W);
					}
				}
			}
			if (is_write && (memmap[chunk].flags & MMAP_CODE)) {
				mov_rr(code, opts->scratch2, opts->scratch1, SZ_D);
				shr_ir(code, 11, opts->scratch1, SZ_D);
				bt_rrdisp(code, opts->scratch1, opts->context_reg, offsetof(m68k_context, ram_code_flags), SZ_D);
				code_ptr not_code = code->cur + 1;
				jcc(code, CC_NC, code->cur + 2);
				call(code, opts->save_context);
#ifdef X86_32
				push_r(code, opts->context_reg);
				push_r(code, opts->scratch2);
#endif
				call(code, (code_ptr)m68k_handle_code_write);
#ifdef X86_32
				add_ir(code, 8, RSP, SZ_D);
#endif
				mov_rr(code, RAX, opts->context_reg, SZ_PTR);
				call(code, opts->load_context);
				*not_code = code->cur - (not_code+1);
			}
			retn(code);
		} else if (cfun) {
			call(code, opts->save_context);
#ifdef X86_64
			if (is_write) {
				if (opts->scratch2 != RDI) {
					mov_rr(code, opts->scratch2, RDI, SZ_D);
				}
				mov_rr(code, opts->scratch1, RDX, size);
			} else {
				push_r(code, opts->context_reg);
				mov_rr(code, opts->scratch1, RDI, SZ_D);
			}
			test_ir(code, 8, RSP, SZ_D);
			code_ptr adjust_rsp = code->cur + 1;
			jcc(code, CC_NZ, code->cur + 2);
			call(code, cfun);
			code_ptr no_adjust = code->cur + 1;
			jmp(code, code->cur + 2);
			*adjust_rsp = code->cur - (adjust_rsp + 1);
			sub_ir(code, 8, RSP, SZ_PTR);
			call(code, cfun);
			add_ir(code, 8, RSP, SZ_PTR);
			*no_adjust = code->cur - (no_adjust+1);
#else
			if (is_write) {
				push_r(code, opts->scratch1);
			} else {
				push_r(code, opts->context_reg);//save opts->context_reg for later
			}
			push_r(code, opts->context_reg);
			push_r(code, is_write ? opts->scratch2 : opts->scratch1);
			call(code, cfun);
			add_ir(code, is_write ? 12 : 8, RSP, SZ_D);
#endif
			if (is_write) {
				mov_rr(code, RAX, opts->context_reg, SZ_PTR);
			} else {
				pop_r(code, opts->context_reg);
				mov_rr(code, RAX, opts->scratch1, size);
			}
			jmp(code, opts->load_context);
		} else {
			//Not sure the best course of action here
			if (!is_write) {
				mov_ir(code, size == SZ_B ? 0xFF : 0xFFFF, opts->scratch1, size);
			}
			retn(code);
		}
		if (lb_jcc) {
			*lb_jcc = code->cur - (lb_jcc+1);
			lb_jcc = NULL;
		}
		if (ub_jcc) {
			*ub_jcc = code->cur - (ub_jcc+1);
			ub_jcc = NULL;
		}
	}
	if (!is_write) {
		mov_ir(code, size == SZ_B ? 0xFF : 0xFFFF, opts->scratch1, size);
	}
	retn(code);
	return start;
}

void init_x86_68k_opts(m68k_options * opts, memmap_chunk * memmap, uint32_t num_chunks)
{
	memset(opts, 0, sizeof(*opts));
	for (int i = 0; i < 8; i++)
	{
		opts->dregs[i] = opts->aregs[i] = -1;
	}
#ifdef X86_64
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

	opts->gen.scratch2 = RDI;
#else
	opts->dregs[0] = RDX;
	opts->aregs[7] = RDI;

	for (int i = 0; i < 5; i++)
	{
		opts->flag_regs[i] = -1;
	}
	opts->gen.scratch2 = RBX;
#endif
	opts->gen.context_reg = RSI;
	opts->gen.cycles = RAX;
	opts->gen.limit = RBP;
	opts->gen.scratch1 = RCX;


	opts->gen.native_code_map = malloc(sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	memset(opts->gen.native_code_map, 0, sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	opts->gen.deferred = NULL;
	opts->gen.ram_inst_sizes = malloc(sizeof(uint8_t *) * 64);
	memset(opts->gen.ram_inst_sizes, 0, sizeof(uint8_t *) * 64);

	code_info *code = &opts->gen.code;
	init_code_info(code);

	opts->gen.save_context = code->cur;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			mov_rrdisp(code, opts->flag_regs[i], opts->gen.context_reg, offsetof(m68k_context, flags) + i, SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			mov_rrdisp(code, opts->dregs[i], opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			mov_rrdisp(code, opts->aregs[i], opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, SZ_D);
		}
	}
	mov_rrdisp(code, opts->gen.cycles, opts->gen.context_reg, offsetof(m68k_context, current_cycle), SZ_D);
	retn(code);

	opts->gen.load_context = code->cur;
	for (int i = 0; i < 5; i++)
		if (opts->flag_regs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + i, opts->flag_regs[i], SZ_B);
		}
	for (int i = 0; i < 8; i++)
	{
		if (opts->dregs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, dregs) + sizeof(uint32_t) * i, opts->dregs[i], SZ_D);
		}
		if (opts->aregs[i] >= 0) {
			mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * i, opts->aregs[i], SZ_D);
		}
	}
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, current_cycle), CYCLES, SZ_D);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, target_cycle), LIMIT, SZ_D);
	retn(code);

	opts->start_context = (start_fun)code->cur;
#ifdef X86_64
	if (opts->gen.scratch2 != RDI) {
		mov_rr(code, RDI, opts->gen.scratch2, SZ_PTR);
	}
	//save callee save registers
	push_r(code, RBP);
	push_r(code, R12);
	push_r(code, R13);
	push_r(code, R14);
	push_r(code, R15);
#else
	//save callee save registers
	push_r(code, RBP);
	push_r(code, RBX);
	push_r(code, RSI);
	push_r(code, RDI);

	mov_rdispr(code, RSP, 20, opts->gen.scratch2, SZ_D);
	mov_rdispr(code, RSP, 24, opts->gen.context_reg, SZ_D);
#endif
	call(code, opts->gen.load_context);
	call_r(code, opts->gen.scratch2);
	call(code, opts->gen.save_context);
#ifdef X86_64
	//restore callee save registers
	pop_r(code, R15);
	pop_r(code, R14);
	pop_r(code, R13);
	pop_r(code, R12);
	pop_r(code, RBP);
#else
	pop_r(code, RDI);
	pop_r(code, RSI);
	pop_r(code, RBX);
	pop_r(code, RBP);
#endif
	retn(code);

	opts->native_addr = code->cur;
	call(code, opts->gen.save_context);
	push_r(code, opts->gen.context_reg);
#ifdef X86_64
	mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR); //move context to 1st arg reg
	mov_rr(code, opts->gen.scratch1, RSI, SZ_D); //move address to 2nd arg reg
#else
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.context_reg);
#endif
	call(code, (code_ptr)get_native_address_trans);
#ifdef X86_32
	add_ir(code, 8, RSP, SZ_D);
#endif
	mov_rr(code, RAX, opts->gen.scratch1, SZ_PTR); //move result to scratch reg
	pop_r(code, opts->gen.context_reg);
	call(code, opts->gen.load_context);
	retn(code);

	opts->native_addr_and_sync = code->cur;
	call(code, opts->gen.save_context);
	push_r(code, opts->gen.scratch1);
#ifdef X86_64
	mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
	xor_rr(code, RSI, RSI, SZ_D);
	test_ir(code, 8, RSP, SZ_PTR); //check stack alignment
	code_ptr do_adjust_rsp = code->cur + 1;
	jcc(code, CC_NZ, code->cur + 2);
	call(code, (code_ptr)sync_components);
	code_ptr no_adjust_rsp = code->cur + 1;
	jmp(code, code->cur + 2);
	*do_adjust_rsp = code->cur - (do_adjust_rsp+1);
	sub_ir(code, 8, RSP, SZ_PTR);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_PTR);
	*no_adjust_rsp = code->cur - (no_adjust_rsp+1);
	pop_r(code, RSI);
	push_r(code, RAX);
	mov_rr(code, RAX, RDI, SZ_PTR);
	call(code, (code_ptr)get_native_address_trans);
#else
	//TODO: Add support for pushing a constant in gen_x86
	xor_rr(code, RAX, RAX, SZ_D);
	push_r(code, RAX);
	push_r(code, opts->gen.context_reg);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_D);
	pop_r(code, RSI); //restore saved address from opts->gen.scratch1
	push_r(code, RAX); //save context pointer for later
	push_r(code, RSI); //2nd arg -- address
	push_r(code, RAX); //1st arg -- context pointer
	call(code, (code_ptr)get_native_address_trans);
	add_ir(code, 8, RSP, SZ_D);
#endif

	mov_rr(code, RAX, opts->gen.scratch1, SZ_PTR); //move result to scratch reg
	pop_r(code, opts->gen.context_reg);
	call(code, opts->gen.load_context);
	retn(code);

	opts->gen.handle_cycle_limit = code->cur;
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), CYCLES, SZ_D);
	code_ptr skip_sync = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	opts->do_sync = code->cur;
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	call(code, opts->gen.save_context);
#ifdef X86_64
	mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
	xor_rr(code, RSI, RSI, SZ_D);
	test_ir(code, 8, RSP, SZ_D);
	code_ptr adjust_rsp = code->cur + 1;
	jcc(code, CC_NZ, code->cur + 2);
	call(code, (code_ptr)sync_components);
	code_ptr no_adjust = code->cur + 1;
	jmp(code, code->cur + 2);
	*adjust_rsp = code->cur - (adjust_rsp + 1);
	sub_ir(code, 8, RSP, SZ_PTR);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_PTR);
	*no_adjust = code->cur - (no_adjust+1);
#else
	//TODO: Add support for pushing a constant in gen_x86
	xor_rr(code, RAX, RAX, SZ_D);
	push_r(code, RAX);
	push_r(code, opts->gen.context_reg);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_D);
#endif
	mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
	call(code, opts->gen.load_context);
	pop_r(code, opts->gen.scratch2);
	pop_r(code, opts->gen.scratch1);
	*skip_sync = code->cur - (skip_sync+1);
	retn(code);

	opts->read_16 = gen_mem_fun(&opts->gen, memmap, num_chunks, READ_16);
	opts->read_8 = gen_mem_fun(&opts->gen, memmap, num_chunks, READ_8);
	opts->write_16 = gen_mem_fun(&opts->gen, memmap, num_chunks, WRITE_16);
	opts->write_8 = gen_mem_fun(&opts->gen, memmap, num_chunks, WRITE_8);

	opts->read_32 = code->cur;
	push_r(code, opts->gen.scratch1);
	call(code, opts->read_16);
	mov_rr(code, opts->gen.scratch1, opts->gen.scratch2, SZ_W);
	pop_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	add_ir(code, 2, opts->gen.scratch1, SZ_D);
	call(code, opts->read_16);
	pop_r(code, opts->gen.scratch2);
	movzx_rr(code, opts->gen.scratch1, opts->gen.scratch1, SZ_W, SZ_D);
	shl_ir(code, 16, opts->gen.scratch2, SZ_D);
	or_rr(code, opts->gen.scratch2, opts->gen.scratch1, SZ_D);
	retn(code);

	opts->write_32_lowfirst = code->cur;
	push_r(code, opts->gen.scratch2);
	push_r(code, opts->gen.scratch1);
	add_ir(code, 2, opts->gen.scratch2, SZ_D);
	call(code, opts->write_16);
	pop_r(code, opts->gen.scratch1);
	pop_r(code, opts->gen.scratch2);
	shr_ir(code, 16, opts->gen.scratch1, SZ_D);
	jmp(code, opts->write_16);

	opts->write_32_highfirst = code->cur;
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.scratch2);
	shr_ir(code, 16, opts->gen.scratch1, SZ_D);
	call(code, opts->write_16);
	pop_r(code, opts->gen.scratch2);
	pop_r(code, opts->gen.scratch1);
	add_ir(code, 2, opts->gen.scratch2, SZ_D);
	jmp(code, opts->write_16);

	opts->get_sr = code->cur;
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, status), opts->gen.scratch1, SZ_B);
	shl_ir(code, 8, opts->gen.scratch1, SZ_W);
	if (opts->flag_regs[FLAG_X] >= 0) {
		mov_rr(code, opts->flag_regs[FLAG_X], opts->gen.scratch1, SZ_B);
	} else {
		int8_t offset = offsetof(m68k_context, flags);
		if (offset) {
			mov_rdispr(code, opts->gen.context_reg, offset, opts->gen.scratch1, SZ_B);
		} else {
			mov_rindr(code, opts->gen.context_reg, opts->gen.scratch1, SZ_B);
		}
	}
	for (int flag = FLAG_N; flag <= FLAG_C; flag++)
	{
		shl_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			or_rr(code, opts->flag_regs[flag], opts->gen.scratch1, SZ_B);
		} else {
			or_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, flags) + flag, opts->gen.scratch1, SZ_B);
		}
	}
	retn(code);

	opts->set_sr = code->cur;
	for (int flag = FLAG_C; flag >= FLAG_X; flag--)
	{
		rcr_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			setcc_r(code, CC_C, opts->flag_regs[flag]);
		} else {
			int8_t offset = offsetof(m68k_context, flags) + flag;
			if (offset) {
				setcc_rdisp(code, CC_C, opts->gen.context_reg, offset);
			} else {
				setcc_rind(code, CC_C, opts->gen.context_reg);
			}
		}
	}
	shr_ir(code, 8, opts->gen.scratch1, SZ_W);
	mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	retn(code);

	opts->set_ccr = code->cur;
	for (int flag = FLAG_C; flag >= FLAG_X; flag--)
	{
		rcr_ir(code, 1, opts->gen.scratch1, SZ_B);
		if (opts->flag_regs[flag] >= 0) {
			setcc_r(code, CC_C, opts->flag_regs[flag]);
		} else {
			int8_t offset = offsetof(m68k_context, flags) + flag;
			if (offset) {
				setcc_rdisp(code, CC_C, opts->gen.context_reg, offset);
			} else {
				setcc_rind(code, CC_C, opts->gen.context_reg);
			}
		}
	}
	retn(code);

	opts->gen.handle_cycle_limit_int = code->cur;
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_cycle), CYCLES, SZ_D);
	code_ptr do_int = code->cur + 1;
	jcc(code, CC_NC, code->cur + 2);
	cmp_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), CYCLES, SZ_D);
	skip_sync = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	call(code, opts->gen.save_context);
#ifdef X86_64
	mov_rr(code, opts->gen.context_reg, RDI, SZ_PTR);
	mov_rr(code, opts->gen.scratch1, RSI, SZ_D);
	test_ir(code, 8, RSP, SZ_D);
	adjust_rsp = code->cur + 1;
	jcc(code, CC_NZ, code->cur + 2);
	call(code, (code_ptr)sync_components);
	no_adjust = code->cur + 1;
	jmp(code, code->cur + 2);
	*adjust_rsp = code->cur - (adjust_rsp + 1);
	sub_ir(code, 8, RSP, SZ_PTR);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_PTR);
	*no_adjust = code->cur - (no_adjust+1);
#else
	push_r(code, opts->gen.scratch1);
	push_r(code, opts->gen.context_reg);
	call(code, (code_ptr)sync_components);
	add_ir(code, 8, RSP, SZ_D);
#endif
	mov_rr(code, RAX, opts->gen.context_reg, SZ_PTR);
	jmp(code, opts->gen.load_context);
	*skip_sync = code->cur - (skip_sync+1);
	retn(code);
	*do_int = code->cur - (do_int+1);
	//set target cycle to sync cycle
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, sync_cycle), LIMIT, SZ_D);
	//swap USP and SSP if not already in supervisor mode
	bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	code_ptr already_supervisor = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->gen.scratch2, SZ_D);
	mov_rrdisp(code, opts->aregs[7], opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
	mov_rr(code, opts->gen.scratch2, opts->aregs[7], SZ_D);
	*already_supervisor = code->cur - (already_supervisor+1);
	//save PC
	sub_ir(code, 4, opts->aregs[7], SZ_D);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_32_lowfirst);
	//save status register
	sub_ir(code, 2, opts->aregs[7], SZ_D);
	call(code, opts->get_sr);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_16);
	//update status register
	and_irdisp(code, 0xF8, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_num), opts->gen.scratch1, SZ_B);
	or_ir(code, 0x20, opts->gen.scratch1, SZ_B);
	or_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	//calculate interrupt vector address
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, int_num), opts->gen.scratch1, SZ_D);
	mov_rrdisp(code, opts->gen.scratch1, opts->gen.context_reg, offsetof(m68k_context, int_ack), SZ_W);
	shl_ir(code, 2, opts->gen.scratch1, SZ_D);
	add_ir(code, 0x60, opts->gen.scratch1, SZ_D);
	call(code, opts->read_32);
	call(code, opts->native_addr_and_sync);
	cycles(&opts->gen, 24);
	//discard function return address
	pop_r(code, opts->gen.scratch2);
	jmp_r(code, opts->gen.scratch1);

	opts->trap = code->cur;
	push_r(code, opts->gen.scratch2);
	//swap USP and SSP if not already in supervisor mode
	bt_irdisp(code, 5, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	already_supervisor = code->cur + 1;
	jcc(code, CC_C, code->cur + 2);
	mov_rdispr(code, opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->gen.scratch2, SZ_D);
	mov_rrdisp(code, opts->aregs[7], opts->gen.context_reg, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
	mov_rr(code, opts->gen.scratch2, opts->aregs[7], SZ_D);
	*already_supervisor = code->cur - (already_supervisor+1);
	//save PC
	sub_ir(code, 4, opts->aregs[7], SZ_D);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_32_lowfirst);
	//save status register
	sub_ir(code, 2, opts->aregs[7], SZ_D);
	call(code, opts->get_sr);
	mov_rr(code, opts->aregs[7], opts->gen.scratch2, SZ_D);
	call(code, opts->write_16);
	//set supervisor bit
	or_irdisp(code, 0x20, opts->gen.context_reg, offsetof(m68k_context, status), SZ_B);
	//calculate vector address
	pop_r(code, opts->gen.scratch1);
	shl_ir(code, 2, opts->gen.scratch1, SZ_D);
	call(code, opts->read_32);
	call(code, opts->native_addr_and_sync);
	cycles(&opts->gen, 18);
	jmp_r(code, opts->gen.scratch1);
}

void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts)
{
	memset(context, 0, sizeof(m68k_context));
	context->native_code_map = native_code_map;
	context->options = opts;
	context->int_cycle = 0xFFFFFFFF;
	context->status = 0x27;
}

