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

void handle_cycle_limit();
void m68k_read_word_scratch1();
void m68k_read_long_scratch1();
void m68k_read_byte_scratch1();
void m68k_write_word();
void m68k_write_long_lowfirst();
void m68k_write_long_highfirst();
void m68k_write_byte();
void m68k_save_context();
void m68k_modified_ret_addr();
void m68k_start_context(uint8_t * addr, m68k_context * context);

uint8_t * cycles(uint8_t * dst, uint32_t num)
{
	dst = add_ir(dst, num, CYCLES, SZ_D);
}

uint8_t * check_cycles(uint8_t * dst)
{
	dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
	dst = jcc(dst, CC_G, dst+7);
	dst = call(dst, (char *)handle_cycle_limit);
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

void print_regs_exit(m68k_context * context)
{
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
			ea->disp = (inst->src.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->src.params.regs.pri;
		} else {
			out = mov_rdisp8r(out, CONTEXT, (inst->src.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->src.params.regs.pri, SCRATCH1, inst->extra.size);
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
			out = sub_irdisp8(out, inc_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SZ_D);
		}
		out = check_cycles(out);
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:	
		if (opts->aregs[inst->src.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->src.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SCRATCH1, SZ_D);
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
				out = add_irdisp8(out, inc_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, SZ_D);
			}
		}
		ea->mode = MODE_REG_DIRECT;
		ea->base = SCRATCH1;
		break;
	case MODE_IMMEDIATE:
		if (inst->variant != VAR_QUICK) {
			if (inst->extra.size == OPSIZE_LONG) {
				out = cycles(out, BUS);
				out = check_cycles(out);
			}
			out = cycles(out, BUS);
			out = check_cycles(out);
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

uint8_t * translate_m68k_dst(m68kinst * inst, x86_ea * ea, uint8_t * out, x86_68k_options * opts)
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
		ea->disp = (inst->dst.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->dst.params.regs.pri;
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = sub_ir(out, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			out = sub_irdisp8(out, dec_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH1, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT,  offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SCRATCH1, SZ_D);
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
		//save reg value in SCRATCH2 so we can use it to save the result in memory later
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			out = mov_rr(out, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			out = mov_rdisp8r(out, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SCRATCH2, SZ_D);
		}
		
		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			inc_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
			if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
				out = add_ir(out, inc_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
			} else {
				out = add_irdisp8(out, inc_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
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
			printf("Native dest: %p, Offset address: %p, displacement: %X\n", native, cur->dest, disp);
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
	//FIXME: This probably isn't going to work with real code in a lot of cases, no guarantee that
	//all the code in 1KB block is going to be translated at the same time
	address &= 0xFFFFFF;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		native_code_map[chunk].base = native_addr;
		native_code_map[chunk].offsets = malloc(sizeof(uint16_t) * NATIVE_CHUNK_SIZE);
		memset(native_code_map[chunk].offsets, 0xFF, sizeof(uint16_t) * NATIVE_CHUNK_SIZE);
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
	if (src.mode == MODE_REG_DIRECT) {
		flags_reg = src.base;
	} else {
		if (reg >= 0) {
			flags_reg = reg;
		} else {
			printf("moving %d to temp register %d\n", src.disp, SCRATCH1);
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
			printf("mov_rrdisp8 from reg %d to offset %d from reg %d (%d)\n", src.base, (int)(inst->dst.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->dst.params.regs.pri, CONTEXT, inst->dst.params.regs.pri);
			dst = mov_rrdisp8(dst, src.base, CONTEXT, (inst->dst.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->dst.params.regs.pri, inst->extra.size);
		} else {
			dst = mov_irdisp8(dst, src.disp, CONTEXT, (inst->dst.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * inst->dst.params.regs.pri, inst->extra.size);
		}
		break;
	case MODE_AREG_PREDEC:
		dec_amount = inst->extra.size == OPSIZE_WORD ? 2 : (inst->extra.size == OPSIZE_LONG ? 4 : 1);
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = sub_ir(dst, dec_amount, opts->aregs[inst->dst.params.regs.pri], SZ_D);
		} else {
			dst = sub_irdisp8(dst, dec_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
		}
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
		if (opts->aregs[inst->dst.params.regs.pri] >= 0) {
			dst = mov_rr(dst, opts->aregs[inst->dst.params.regs.pri], SCRATCH2, SZ_D);
		} else {
			dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SCRATCH2, SZ_D);
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
				dst = add_irdisp8(dst, inc_amount, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
			}
		}
		break;
	default:
		printf("address mode %d not implemented (move dst)\n", inst->dst.addr_mode);
		exit(1);
	}

	//add cycles for prefetch
	dst = cycles(dst, BUS);
	//update flags
	dst = mov_ir(dst, 0, FLAG_V, SZ_B);
	dst = mov_ir(dst, 0, FLAG_C, SZ_B);
	dst = cmp_ir(dst, 0, flags_reg, inst->extra.size);
	dst = setcc_r(dst, CC_Z, FLAG_Z);
	dst = setcc_r(dst, CC_S, FLAG_N);
	dst = check_cycles(dst);
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
        dst = check_cycles(dst);
        break;
    case MODE_ABSOLUTE:
        dst = cycles(dst, BUS);
        dst = check_cycles(dst);
    case MODE_ABSOLUTE_SHORT:
        dst = cycles(dst, BUS);
        dst = check_cycles(dst);
        dst = cycles(dst, BUS);
        if (dst_reg >= 0) {
            dst = mov_ir(dst, inst->src.params.immed, dst_reg, SZ_D);
        } else {
            dst = mov_irdisp8(dst, inst->src.params.immed, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->dst.params.regs.pri, SZ_D);
        }
        dst = check_cycles(dst);
        break;
    }
	return dst;
}

uint8_t * translate_m68k_bsr(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	//TODO: Add cycles
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + 2;
	dst = mov_ir(dst, after, SCRATCH1, SZ_D);
	dst = push_r(dst, SCRATCH1);
	dst = sub_ir(dst, 4, opts->aregs[7], SZ_D);
	dst = mov_rr(dst, opts->aregs[7], SCRATCH2, SZ_D);
	dst = call(dst, (char *)m68k_write_long_highfirst);
	printf("bsr@%X: after=%X, disp=%X, dest=%X\n", inst->address, after, disp, after+disp);
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
	printf("bcc@%X: after=%X, disp=%X, dest=%X\n", inst->address, after, disp, after+disp);
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
	dst = check_cycles(dst);
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
	dst = check_cycles(dst);
}

uint8_t * translate_m68k(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	map_native_address(opts->native_code_map, inst->address, dst);
	if (inst->op == M68K_MOVE) {
		return translate_m68k_move(dst, inst, opts);
	} else if(inst->op == M68K_LEA) {
		return translate_m68k_lea(dst, inst, opts);
	} else if(inst->op == M68K_BSR) {
		return translate_m68k_bsr(dst, inst, opts);
	} else if(inst->op == M68K_BCC) {
		return translate_m68k_bcc(dst, inst, opts);
	} else if(inst->op == M68K_RTS) {
		return translate_m68k_rts(dst, inst, opts);
	} else if(inst->op == M68K_DBCC) {
		return translate_m68k_dbcc(dst, inst, opts);
	}
	x86_ea src_op, dst_op;
	if (inst->src.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_src(inst, &src_op, dst, opts);
	}
	if (inst->dst.addr_mode != MODE_UNUSED) {
		dst = translate_m68k_dst(inst, &dst_op, dst, opts);
	}
	switch(inst->op)
	{
	case M68K_ABCD:
		break;
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
		dst = check_cycles(dst);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ADDX:
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
		dst = check_cycles(dst);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ANDI_CCR:
	case M68K_ANDI_SR:
	case M68K_ASL:
	case M68K_LSL:
	case M68K_ASR:
	case M68K_LSR:
	case M68K_BCHG:
	case M68K_BCLR:
	case M68K_BSET:
	case M68K_BTST:
	case M68K_CHK:
	case M68K_CLR:
		break;
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
		dst = check_cycles(dst);
		break;
	case M68K_DIVS:
	case M68K_DIVU:
		break;
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
		dst = check_cycles(dst);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_EORI_CCR:
	case M68K_EORI_SR:
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
	    dst = check_cycles(dst);
	    break;
	case M68K_EXT:
		break;
	case M68K_ILLEGAL:
		dst = call(dst, (uint8_t *)m68k_save_context);
		dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
		dst = call(dst, (uint8_t *)print_regs_exit);
		break;
	case M68K_JMP:
	case M68K_JSR:
	case M68K_LEA:
	case M68K_LINK:
	case M68K_MOVE_CCR:
	case M68K_MOVE_FROM_SR:
	case M68K_MOVE_SR:
	case M68K_MOVE_USP:
	case M68K_MOVEM:
	case M68K_MOVEP:
	case M68K_MULS:
	case M68K_MULU:
	case M68K_NBCD:
	case M68K_NEG:
	case M68K_NEGX:
		break;
	case M68K_NOP:
		dst = cycles(dst, BUS);
		dst = check_cycles(dst);
		break;
	case M68K_NOT:
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
		dst = check_cycles(dst);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_ORI_CCR:
	case M68K_ORI_SR:
	case M68K_PEA:
	case M68K_RESET:
	case M68K_ROL:
	case M68K_ROR:
	case M68K_ROXL:
	case M68K_ROXR:
	case M68K_RTE:
	case M68K_RTR:
	case M68K_SBCD:
	case M68K_SCC:
	case M68K_STOP:
		break;
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
		dst = check_cycles(dst);
		dst = m68k_save_result(inst, dst, opts);
		break;
	case M68K_SUBX:
		break;
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
		dst = check_cycles(dst);
		break;
	case M68K_TAS:
	case M68K_TRAP:
	case M68K_TRAPV:
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
		dst = check_cycles(dst);
		break;
	case M68K_UNLK:
	case M68K_INVALID:
		break;
	}
	return dst;
}

uint8_t * translate_m68k_stream(uint8_t * dst, uint8_t * dst_end, uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	x86_68k_options * opts = context->options;
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
}

