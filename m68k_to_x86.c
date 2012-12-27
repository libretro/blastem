#include "gen_x86.h"
#include "m68k_to_x86.h"
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

typedef struct {
	int32_t disp;
	uint8_t mode;
	uint8_t base;
	uint8_t index;
	uint8_t cycles;
} x86_ea;

void handle_cycle_limit_int();
void m68k_read_word_scratch1();
void m68k_read_long_scratch1();
void m68k_read_byte_scratch1();
void m68k_write_word();
void m68k_write_long_lowfirst();
void m68k_write_long_highfirst();
void m68k_write_byte();
void m68k_save_context();
void m68k_modified_ret_addr();
void m68k_native_addr();
void m68k_native_addr_and_sync();
void set_sr();
void set_ccr();
void get_sr();
void m68k_start_context(uint8_t * addr, m68k_context * context);

uint8_t * cycles(uint8_t * dst, uint32_t num)
{
	dst = add_ir(dst, num, CYCLES, SZ_D);
}

uint8_t * check_cycles_int(uint8_t * dst, uint32_t address)
{
	dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
	uint8_t * jmp_off = dst+1;
	dst = jcc(dst, CC_NC, dst + 7);
	dst = mov_ir(dst, address, SCRATCH1, SZ_D);
	dst = call(dst, (uint8_t *)handle_cycle_limit_int);
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
	printf("XNVZC\n%d%d%d%d%d\n", context->flags[0], context->flags[1], context->flags[2], context->flags[3], context->flags[4]);
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
		ea->base = reg;
		return out;
	}
	switch (inst->src.addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		//We only get one memory parameter, so if the dst operand is a register in memory,
		//we need to copy this to a temp register first
		reg = native_reg(&(inst->dst), opts);
		if (reg >= 0 || inst->dst.addr_mode == MODE_UNUSED || (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode == MODE_AREG) 
		    || inst->op == M68K_EXG) {
			
			ea->mode = MODE_REG_DISPLACE8;
			ea->base = CONTEXT;
			ea->disp = reg_offset(&(inst->src));
		} else {
			out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, inst->extra.size);
			ea->mode = MODE_REG_DIRECT;
			ea->base = SCRATCH1;
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
		out = cycles(out, PREDEC_PENALTY);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = sub_ir(out, inc_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
		} else {
			out = sub_irdisp8(out, inc_amount, CONTEXT, reg_offset(&(inst->src)), SZ_D);
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
			out = call(out, (char *)m68k_read_byte_scratch1);
			break;
		case OPSIZE_WORD:
			out = call(out, (char *)m68k_read_word_scratch1);
			break;
		case OPSIZE_LONG:
			out = call(out, (char *)m68k_read_long_scratch1);
			break;
		}
		
		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				out = add_ir(out, inc_amount, opts->aregs[inst->src.params.regs.pri], SZ_D);
			} else {
				out = add_irdisp8(out, inc_amount, CONTEXT, reg_offset(&(inst->src)), SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
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
			out = call(out, (char *)m68k_read_byte_scratch1);
			break;
		case OPSIZE_WORD:
			out = call(out, (char *)m68k_read_word_scratch1);
			break;
		case OPSIZE_LONG:
			out = call(out, (char *)m68k_read_long_scratch1);
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
		break;
	case MODE_PC_DISPLACE:
		out = cycles(out, BUS);
		out = mov_ir(out, inst->src.params.regs.displacement + inst->address+2, SCRATCH1, SZ_D);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, (char *)m68k_read_byte_scratch1);
			break;
		case OPSIZE_WORD:
			out = call(out, (char *)m68k_read_word_scratch1);
			break;
		case OPSIZE_LONG:
			out = call(out, (char *)m68k_read_long_scratch1);
			break;
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_PC_INDEX_DISP8:
		out = cycles(out, 6);
		out = mov_ir(out, inst->address, SCRATCH1, SZ_D);
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
			out = call(out, (char *)m68k_read_byte_scratch1);
			break;
		case OPSIZE_WORD:
			out = call(out, (char *)m68k_read_word_scratch1);
			break;
		case OPSIZE_LONG:
			out = call(out, (char *)m68k_read_long_scratch1);
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
		break;
	default:
		printf("address mode %d not implemented (src)\n", inst->src.addr_mode);
		exit(1);
	}
	return out;
}

uint8_t * translate_m68k_dst(m68kinst * inst, x86_ea * ea, uint8_t * out, x86_68k_options * opts, uint8_t fake_read)
{
	int8_t reg = native_reg(&(inst->dst), opts);
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
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
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
				out = call(out, (char *)m68k_read_byte_scratch1);
				break;
			case OPSIZE_WORD:
				out = call(out, (char *)m68k_read_word_scratch1);
				break;
			case OPSIZE_LONG:
				out = call(out, (char *)m68k_read_long_scratch1);
				break;
			}
		}
		//save reg value in SCRATCH2 so we can use it to save the result in memory later
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT, reg_offset(&(inst->dst)), SCRATCH2, SZ_D);
		}
		
		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
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
				out = call(out, (char *)m68k_read_byte_scratch1);
				break;
			case OPSIZE_WORD:
				out = call(out, (char *)m68k_read_word_scratch1);
				break;
			case OPSIZE_LONG:
				out = call(out, (char *)m68k_read_long_scratch1);
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
				out = call(out, (char *)m68k_read_byte_scratch1);
				break;
			case OPSIZE_WORD:
				out = call(out, (char *)m68k_read_word_scratch1);
				break;
			case OPSIZE_LONG:
				out = call(out, (char *)m68k_read_long_scratch1);
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
				out = call(out, (char *)m68k_read_byte_scratch1);
				break;
			case OPSIZE_WORD:
				out = call(out, (char *)m68k_read_word_scratch1);
				break;
			case OPSIZE_LONG:
				out = call(out, (char *)m68k_read_long_scratch1);
				break;
			}
			out = pop_r(out, SCRATCH2);
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	default:
		printf("address mode %d not implemented (dst)\n", inst->dst.addr_mode);
		exit(1);
	}
	return out;
}

uint8_t * m68k_save_result(m68kinst * inst, uint8_t * out, x86_68k_options * opts)
{
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG) {
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			out = call(out, (char *)m68k_write_byte);
			break;
		case OPSIZE_WORD:
			out = call(out, (char *)m68k_write_word);
			break;
		case OPSIZE_LONG:
			out = call(out, (char *)m68k_write_long_lowfirst);
			break;
		}
	}
	return out;
}

uint8_t * get_native_address(native_map_slot * native_code_map, uint32_t address)
{
	address &= 0xFFFFFF;
	if (address > 0x400000) {
		printf("get_native_address: %X\n", address);
	}
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		return NULL;
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET) {
		return NULL;
	}
	return native_code_map[chunk].base + native_code_map[chunk].offsets[offset];
}

deferred_addr * defer_address(deferred_addr * old_head, uint32_t address, uint8_t *dest)
{
	deferred_addr * new_head = malloc(sizeof(deferred_addr));
	new_head->next = old_head;
	new_head->address = address & 0xFFFFFF;
	new_head->dest = dest;
	return new_head;
}

void process_deferred(x86_68k_options * opts)
{
	deferred_addr * cur = opts->deferred;
	deferred_addr **last_next = &(opts->deferred);
	while(cur)
	{
		uint8_t * native = get_native_address(opts->native_code_map, cur->address);
		if (native) {
			int32_t disp = native - (cur->dest + 4);
			uint8_t * out = cur->dest;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*out = disp;
			*last_next = cur->next;
			free(cur);
			cur = *last_next;
		} else {
			last_next = &(cur->next);
			cur = cur->next;
		}
	}
}

void map_native_address(native_map_slot * native_code_map, uint32_t address, uint8_t * native_addr)
{
	address &= 0xFFFFFF;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		native_code_map[chunk].base = native_addr;
		native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	native_code_map[chunk].offsets[offset] = native_addr-native_code_map[chunk].base;
}

uint8_t * translate_m68k_move(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t reg, flags_reg;
	uint8_t dir = 0;
	int32_t offset;
	int32_t inc_amount, dec_amount;
	x86_ea src;
	dst = translate_m68k_src(inst, &src, dst, opts);
	reg = native_reg(&(inst->dst), opts);
	//update statically set flags
	dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	dst = mov_ir(dst, 0, FLAG_C, SZ_B);
	
	if (src.mode == MODE_REG_DIRECT) {
		flags_reg = src.base;
	} else {
		if (reg >= 0) {
			flags_reg = reg;
		} else {
			dst = mov_ir(dst, src.disp, SCRATCH1, SZ_D);
			src.mode = MODE_REG_DIRECT;
			flags_reg = src.base = SCRATCH1;
		}
	}
	switch(inst->dst.addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		if (reg >= 0) {
			if (src.mode == MODE_REG_DIRECT) {
				dst = mov_rr(dst, src.base, reg, inst->extra.size);
			} else if (src.mode == MODE_REG_DISPLACE8) {
				dst = mov_rdisp8r(dst, src.base, src.disp, reg, inst->extra.size);
			} else {
				dst = mov_ir(dst, src.disp, reg, inst->extra.size);
			}
		} else if(src.mode == MODE_REG_DIRECT) {
			dst = mov_rrdisp8(dst, src.base, CONTEXT, reg_offset(&(inst->dst)), inst->extra.size);
		} else {
			dst = mov_irdisp8(dst, src.disp, CONTEXT, reg_offset(&(inst->dst)), inst->extra.size);
		}
		dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
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
		dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, (char *)m68k_write_byte);
			break;
		case OPSIZE_WORD:
			dst = call(dst, (char *)m68k_write_word);
			break;
		case OPSIZE_LONG:
			dst = call(dst, (char *)m68k_write_long_highfirst);
			break;
		}
		if (inst->dst.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
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
		dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, (char *)m68k_write_byte);
			break;
		case OPSIZE_WORD:
			dst = call(dst, (char *)m68k_write_word);
			break;
		case OPSIZE_LONG:
			dst = call(dst, (char *)m68k_write_long_highfirst);
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
		dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, (char *)m68k_write_byte);
			break;
		case OPSIZE_WORD:
			dst = call(dst, (char *)m68k_write_word);
			break;
		case OPSIZE_LONG:
			dst = call(dst, (char *)m68k_write_long_highfirst);
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
		dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			dst = call(dst, (char *)m68k_write_byte);
			break;
		case OPSIZE_WORD:
			dst = call(dst, (char *)m68k_write_word);
			break;
		case OPSIZE_LONG:
			dst = call(dst, (char *)m68k_write_long_highfirst);
			break;
		}
		break;
	default:
		printf("address mode %d not implemented (move dst)\n", inst->dst.addr_mode);
		exit(1);
	}

	//add cycles for prefetch
	dst = cycles(dst, BUS);
	return dst;
}

uint8_t * translate_m68k_movem(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t bit,reg;
	uint8_t early_cycles;
	if(inst->src.addr_mode == MODE_REG) {
		//reg to mem
		early_cycles = 8;
		int8_t dir;
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			reg = 15;
			dir = -1;
		} else {
			reg = 0;
		}
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
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			dst = mov_ir(dst, inst->dst.params.immed, SCRATCH2, SZ_D);
			break;
		default:
			printf("address mode %d not implemented (movem dst)\n", inst->dst.addr_mode);
			exit(1);
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
					dst = call(dst, (uint8_t *)m68k_write_long_lowfirst);
				} else {
					dst = call(dst, (uint8_t *)m68k_write_word);
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
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			dst = mov_ir(dst, inst->src.params.immed, SCRATCH1, SZ_D);
			break;
		default:
			printf("address mode %d not implemented (movem src)\n", inst->src.addr_mode);
			exit(1);
		}
		dst = cycles(dst, early_cycles);
		for(reg = 0; reg < 16; reg ++) {
			if (inst->dst.params.immed & (1 << reg)) {
				dst = push_r(dst, SCRATCH1);
				if (inst->extra.size == OPSIZE_LONG) {
					dst = call(dst, (uint8_t *)m68k_read_long_scratch1);
				} else {
					dst = call(dst, (uint8_t *)m68k_read_word_scratch1);
				}
				if (reg > 7) {
					if (opts->aregs[reg-8] >= 0) {
						dst = mov_rr(dst, SCRATCH1, opts->aregs[reg-8], inst->extra.size);
					} else {
						dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * (reg-8), inst->extra.size);
					}
				} else {
					if (opts->dregs[reg] >= 0) {
						dst = mov_rr(dst, SCRATCH1, opts->dregs[reg], inst->extra.size);
					} else {
						dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, offsetof(m68k_context, dregs) + sizeof(uint32_t) * (reg), inst->extra.size);
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

uint8_t * translate_m68k_lea(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t dst_reg = native_reg(&(inst->dst), opts);
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
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], dst_reg, SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), dst_reg, SZ_D);
			}
			dst = add_ir(dst, inst->src.params.regs.displacement, dst_reg, SZ_D);
		} else {
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				dst = mov_rrdisp8(dst, opts->aregs[inst->src.params.regs.pri], CONTEXT, reg_offset(&(inst->dst)), SZ_D);
			} else {
				dst = mov_rdisp8r(dst, CONTEXT, reg_offset(&(inst->src)), SCRATCH1, SZ_D);
				dst = mov_rrdisp8(dst, SCRATCH1, CONTEXT, reg_offset(&(inst->dst)), SZ_D);
			}
			dst = add_irdisp8(dst, inst->src.params.regs.displacement, CONTEXT, reg_offset(&(inst->src)), SZ_D);
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
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		dst = cycles(dst, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		if (dst_reg >= 0) {
			dst = mov_ir(dst, inst->src.params.immed, dst_reg, SZ_D);
		} else {
			dst = mov_irdisp8(dst, inst->src.params.immed, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
		}
		break;
	default:
		printf("address mode %d not implemented (lea src)\n", inst->src.addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * translate_m68k_bsr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + 2;
	//TODO: Add cycles in the right place relative to pushing the return address on the stack
	dst = cycles(dst, 10);
	dst = mov_ir(dst, after, SCRATCH1, SZ_D);
	dst = push_r(dst, SCRATCH1);
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, (char *)m68k_write_long_highfirst);
	uint8_t * dest_addr = get_native_address(opts->native_code_map, after + disp);
	if (!dest_addr) {
		opts->deferred = defer_address(opts->deferred, after + disp, dst + 1);
		//dummy address to be replaced later
		dest_addr = dst + 5;
	}
	dst = call(dst, (char *)dest_addr);
	//would add_ir(dst, 8, RSP, SZ_Q) be faster here?
	dst = pop_r(dst, SCRATCH1);
	return dst;
}

uint8_t * translate_m68k_bcc(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	//TODO: Add cycles
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

uint8_t * translate_m68k_jmp(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * dest_addr;
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
	case MODE_PC_DISPLACE:
		dst = cycles(dst, 10);
		dest_addr = get_native_address(opts->native_code_map, inst->src.params.regs.displacement + inst->address + 2);
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, inst->src.params.immed, dst + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 256;
		}
		dst = jmp(dst, dest_addr);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		dst = cycles(dst, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		dest_addr = get_native_address(opts->native_code_map, inst->src.params.immed);
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, inst->src.params.immed, dst + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 256;
		}
		dst = jmp(dst, dest_addr);
		break;
	default:
		printf("address mode %d not yet supported (jmp)\n", inst->src.addr_mode);
	}
	return dst;
}

uint8_t * translate_m68k_jsr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * dest_addr;
	uint32_t after;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		dst = cycles(dst, BUS*2);
		dst = mov_ir(dst, inst->address + 8, SCRATCH1, SZ_D);
		dst = push_r(dst, SCRATCH1);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, (char *)m68k_write_long_highfirst);
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
		}
		dst = call(dst, (uint8_t *)m68k_native_addr);
		dst = call_r(dst, SCRATCH1);
		//would add_ir(dst, 8, RSP, SZ_Q) be faster here?
		dst = pop_r(dst, SCRATCH1);
		break;
	case MODE_PC_DISPLACE:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		dst = cycles(dst, 10);
		dst = mov_ir(dst, inst->address + 8, SCRATCH1, SZ_D);
		dst = push_r(dst, SCRATCH1);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, (char *)m68k_write_long_highfirst);
		dest_addr = get_native_address(opts->native_code_map, inst->src.params.regs.displacement + inst->address + 2);
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, inst->src.params.immed, dst + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 5;
		}
		dst = call(dst, (char *)dest_addr);
		//would add_ir(dst, 8, RSP, SZ_Q) be faster here?
		dst = pop_r(dst, SCRATCH1);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		dst = cycles(dst, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		dst = mov_ir(dst, inst->address + 8, SCRATCH1, SZ_D);
		dst = push_r(dst, SCRATCH1);
		dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = call(dst, (char *)m68k_write_long_highfirst);
		dest_addr = get_native_address(opts->native_code_map, inst->src.params.immed);
		if (!dest_addr) {
			opts->deferred = defer_address(opts->deferred, inst->src.params.immed, dst + 1);
			//dummy address to be replaced later, make sure it generates a 4-byte displacement
			dest_addr = dst + 5;
		}
		dst = call(dst, (char *)dest_addr);
		//would add_ir(dst, 8, RSP, SZ_Q) be faster here?
		dst = pop_r(dst, SCRATCH1);
		break;
	default:
		printf("address mode %d not yet supported (jsr)\n", inst->src.addr_mode);
	}
	return dst;
}

uint8_t * translate_m68k_rts(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	//TODO: Add cycles
	dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
	dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = call(dst, (char *)m68k_read_long_scratch1);
	dst = cmp_rdisp8r(dst, RSP, 8, SCRATCH1, SZ_D);
	dst = jcc(dst, CC_NZ, dst+3);
	dst = retn(dst);
	dst = jmp(dst, (char *)m68k_modified_ret_addr);
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
	dst = call(dst, (char *)m68k_write_long_highfirst);
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

typedef uint8_t * (*shift_ir_t)(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
typedef uint8_t * (*shift_irdisp8_t)(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
typedef uint8_t * (*shift_clr_t)(uint8_t * out, uint8_t dst, uint8_t size);
typedef uint8_t * (*shift_clrdisp8_t)(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size);

uint8_t * translate_shift(uint8_t * dst, m68kinst * inst, x86_ea *src_op, x86_ea * dst_op, x86_68k_options * opts, shift_ir_t shift_ir, shift_irdisp8_t shift_irdisp8, shift_clr_t shift_clr, shift_clrdisp8_t shift_clrdisp8, shift_ir_t special, shift_irdisp8_t special_disp8)
{
	uint8_t * end_off = NULL;
	if (inst->src.addr_mode == MODE_UNUSED) {
		dst = cycles(dst, BUS);
		//Memory shift
		dst = shift_ir(dst, 1, dst_op->base, SZ_W);
	} else {
		dst = cycles(dst, inst->extra.size == OPSIZE_LONG ? 8 : 6);
		if (src_op->mode == MODE_IMMED) {
			if (dst_op->mode == MODE_REG_DIRECT) {
				dst = shift_ir(dst, src_op->disp, dst_op->base, inst->extra.size);
			} else {
				dst = shift_irdisp8(dst, src_op->disp, dst_op->base, dst_op->disp, inst->extra.size);
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
			//add 2 cycles for every bit shifted
			dst = add_rr(dst, RCX, CYCLES, SZ_D);
			dst = add_rr(dst, RCX, CYCLES, SZ_D);
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
	if (!special && end_off) {
		*end_off = dst - (end_off + 1);
	}
	dst = setcc_r(dst, CC_C, FLAG_C);
	dst = setcc_r(dst, CC_Z, FLAG_Z);
	dst = setcc_r(dst, CC_S, FLAG_N);
	if (special && end_off) {
		*end_off = dst - (end_off + 1);
	}
	dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	//set X flag to same as C flag
	dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
	if (inst->src.addr_mode == MODE_UNUSED) {
		dst = m68k_save_result(inst, dst, opts);
	}
	return dst;
}

#define BIT_SUPERVISOR 5

uint8_t * translate_m68k(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	uint8_t * end_off;
	map_native_address(opts->native_code_map, inst->address, dst);
	dst = check_cycles_int(dst, inst->address);
	if (inst->op == M68K_MOVE) {
		return translate_m68k_move(dst, inst, opts);
	} else if(inst->op == M68K_LEA) {
		return translate_m68k_lea(dst, inst, opts);
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
	}
	x86_ea src_op, dst_op;
	if (inst->src.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_src(inst, &src_op, dst, opts);
	}
	if (inst->dst.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_dst(inst, &dst_op, dst, opts, 0);
	}
	switch(inst->op)
	{
	//case M68K_ABCD:
	//	break;
	case M68K_ADD:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = add_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = add_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = add_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = add_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = add_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	//case M68K_ADDX:
	//	break;
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
	/*case M68K_BCHG:
	case M68K_BCLR:
	case M68K_BSET:
		break;*/
	case M68K_BTST:
		dst = cycles(dst, inst->extra.size == OPSIZE_BYTE ? 4 : 6);
		if (src_op.mode == MODE_IMMED) {
			if (inst->extra.size == OPSIZE_BYTE) {
				src_op.disp &= 0x7;
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = bt_ir(dst, src_op.disp, dst_op.base, SZ_D);
			} else {
				dst = bt_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, SZ_D);
			}
		} else {
			if (src_op.mode == MODE_REG_DISPLACE8) {
				if (dst_op.base == SCRATCH1) {
					dst = push_r(dst, SCRATCH2);
					dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH2, SZ_B);
					src_op.base = SCRATCH1;
				} else {
					dst = mov_rdisp8r(dst, src_op.base, src_op.disp, SCRATCH1, SZ_B);
					src_op.base = SCRATCH1;
				}
			}
			if (inst->extra.size == OPSIZE_BYTE) {
				dst = and_ir(dst, 0x7, src_op.base, SZ_B);
			}
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = bt_rr(dst, src_op.base, dst_op.base, SZ_D);
			} else {
				dst = bt_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, SZ_D);
			}
		}
		//x86 sets the carry flag to the value of the bit tested
		//68K sets the zero flag to the complement of the bit tested
		dst = setcc_r(dst, CC_NC, FLAG_Z);
		if (src_op.base == SCRATCH2) {
			dst = pop_r(dst, SCRATCH2);
		}
		break;
	/*case M68K_CHK:
		break;*/
	case M68K_CMP:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = cmp_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = cmp_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = cmp_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = cmp_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = cmp_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		break;
	/*case M68K_DIVS:
	case M68K_DIVU:
		break;*/
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
	/*case M68K_EORI_CCR:
	case M68K_EORI_SR:*/
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
	//case M68K_EXT:
	//	break;
	case M68K_ILLEGAL:
		dst = call(dst, (uint8_t *)m68k_save_context);
		dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
		dst = call(dst, (uint8_t *)print_regs_exit);
		break;
	/*case M68K_MOVE_FROM_SR:
		break;*/
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
	/*case M68K_MOVEP:
	case M68K_MULS:
	case M68K_MULU:
	case M68K_NBCD:*/
	case M68K_NEG:
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = neg_r(dst, dst_op.base, inst->extra.size);
		} else {
			dst = not_rdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	/*case M68K_NEGX:
		break;*/
	case M68K_NOP:
		dst = cycles(dst, BUS);
		break;
	case M68K_NOT:
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = not_r(dst, dst_op.base, inst->extra.size);
		} else {
			dst = not_rdisp8(dst, dst_op.base, dst_op.disp, inst->extra.size);
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
	/*case M68K_ORI_CCR:
	case M68K_ORI_SR:
	case M68K_PEA:
	case M68K_RESET:
	case M68K_ROL:
	case M68K_ROR:
	case M68K_ROXL:
	case M68K_ROXR:*/
	case M68K_RTE:
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, (uint8_t *)m68k_read_long_scratch1);
		dst = push_r(dst, SCRATCH1);
		dst = add_ir(dst, 4, opts->aregs[7], SZ_D);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, (uint8_t *)m68k_read_word_scratch1);
		dst = add_ir(dst, 2, opts->aregs[7], SZ_D);
		dst = call(dst, (uint8_t *)set_sr);
		dst = pop_r(dst, SCRATCH1);
		dst = bt_irdisp8(dst, 5, CONTEXT, offsetof(m68k_context, status), SZ_B);
		end_off = dst+1;
		dst = jcc(dst, CC_NC, dst+2);
		dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
		dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, opts->aregs[7], SZ_D);
		dst = mov_rrdisp8(dst, SCRATCH2, CONTEXT, offsetof(m68k_context, aregs) + sizeof(uint32_t) * 8, SZ_D);
		*end_off = dst - (end_off+1);
		dst = call(dst, (uint8_t *)m68k_native_addr_and_sync);
		dst = jmp_r(dst, SCRATCH1);
		break;
	/*case M68K_RTR:
	case M68K_SBCD:
	case M68K_SCC:
	case M68K_STOP:
		break;*/
	case M68K_SUB:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sub_rr(dst, src_op.base, dst_op.base, inst->extra.size);
			} else {
				dst = sub_rrdisp8(dst, src_op.base, dst_op.base, dst_op.disp, inst->extra.size);
			}
		} else if (src_op.mode == MODE_REG_DISPLACE8) {
			dst = sub_rdisp8r(dst, src_op.base, src_op.disp, dst_op.base, inst->extra.size);
		} else {
			if (dst_op.mode == MODE_REG_DIRECT) {
				dst = sub_ir(dst, src_op.disp, dst_op.base, inst->extra.size);
			} else {
				dst = sub_irdisp8(dst, src_op.disp, dst_op.base, dst_op.disp, inst->extra.size);
			}
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		dst = mov_rrind(dst, FLAG_C, CONTEXT, SZ_B);
		dst = m68k_save_result(inst, dst, opts);
		break;
	//case M68K_SUBX:
	//	break;
	case M68K_SWAP:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = rol_ir(dst, 16, src_op.base, inst->extra.size);
		} else{
			dst = rol_irdisp8(dst, 16, src_op.base, src_op.disp, inst->extra.size);
		}
		dst = mov_ir(dst, 0, FLAG_C, SZ_B);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = mov_ir(dst, 0, FLAG_V, SZ_B);
		break;
	/*case M68K_TAS:
	case M68K_TRAP:
	case M68K_TRAPV:*/
	case M68K_TST:
		dst = cycles(dst, BUS);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = cmp_ir(dst, 0, src_op.base, inst->extra.size);
		} else { //M68000 doesn't support immedate operand for tst, so this must be MODE_REG_DISPLACE8
			dst = cmp_irdisp8(dst, 0, src_op.base, src_op.disp, inst->extra.size);
		}
		dst = setcc_r(dst, CC_C, FLAG_C);
		dst = setcc_r(dst, CC_Z, FLAG_Z);
		dst = setcc_r(dst, CC_S, FLAG_N);
		dst = setcc_r(dst, CC_O, FLAG_V);
		break;
	case M68K_UNLK:
		dst = cycles(dst, BUS);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, dst_op.base, opts->aregs[7], SZ_D);
		} else {
			dst = mov_rdisp8r(dst, dst_op.base, dst_op.disp, opts->aregs[7], SZ_D);
		}
		dst = mov_rr(dst, opts->aregs[7], SCRATCH1, SZ_D);
		dst = call(dst, (uint8_t *)m68k_read_long_scratch1);
		if (dst_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, SCRATCH1, dst_op.base, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, SCRATCH1, dst_op.base, dst_op.disp, SZ_D);
		}
		break;
	/*case M68K_INVALID:
		break;*/
	default:
		printf("instruction %d not yet implemented\n", inst->op);
		exit(1);
	}
	return dst;
}

uint8_t * translate_m68k_stream(uint8_t * dst, uint8_t * dst_end, uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	x86_68k_options * opts = context->options;
	if(get_native_address(opts->native_code_map, address)) {
		return dst;
	}
	char disbuf[1024];
	uint16_t *encoded = context->mem_pointers[0] + address/2, *next;
	do {
		do {
			if (dst_end-dst < 128) {
				puts("out of code memory");
				exit(1);
			}
			next = m68k_decode(encoded, &instbuf, address);
			address += (next-encoded)*2;
			encoded = next;
			m68k_disasm(&instbuf, disbuf);
			printf("%X: %s\n", instbuf.address, disbuf);
			dst = translate_m68k(dst, &instbuf, opts);
		} while(instbuf.op != M68K_ILLEGAL && instbuf.op != M68K_RTS && instbuf.op != M68K_RTE && !(instbuf.op == M68K_BCC && instbuf.extra.cond == COND_TRUE) && instbuf.op != M68K_JMP);
		process_deferred(opts);
		if (opts->deferred) {
			address = opts->deferred->address;
			encoded = context->mem_pointers[0] + address/2;
		} else {
			encoded = NULL;
		}
	} while(encoded != NULL);
	return dst;
}

void start_68k_context(m68k_context * context, uint32_t address)
{
	uint8_t * addr = get_native_address(context->native_code_map, address);
	m68k_start_context(addr, context);
}

void m68k_reset(m68k_context * context)
{
	//TODO: Make this actually use the normal read functions
	context->aregs[7] = context->mem_pointers[0][0] << 16 | context->mem_pointers[0][1];
	uint32_t address = context->mem_pointers[0][2] << 16 | context->mem_pointers[0][3];
	start_68k_context(context, address);
}

void init_x86_68k_opts(x86_68k_options * opts)
{
	opts->flags = 0;
	for (int i = 0; i < 8; i++)
		opts->dregs[i] = opts->aregs[i] = -1;
	opts->dregs[0] = R10;
	opts->dregs[1] = R11;
	opts->dregs[2] = R12;
	opts->aregs[0] = R13;
	opts->aregs[1] = R14;
	opts->aregs[7] = R15;
	opts->native_code_map = malloc(sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	memset(opts->native_code_map, 0, sizeof(native_map_slot) * NATIVE_MAP_CHUNKS);
	opts->deferred = NULL;
}

void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts)
{
	memset(context, 0, sizeof(m68k_context));
	context->native_code_map = native_code_map;
	context->options = opts;
	context->int_cycle = 0xFFFFFFFF;
}

