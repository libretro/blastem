#include "gen_x86.h"
#include "m68k_to_x86.h"


#define BUS 4
#define CYCLES RAX
#define LIMIT RBP
#define SCRATCH RCX
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

uint8_t * cycles(uint8_t * dst, uint32_t num)
{
	dst = add_i32r(dst, num, CYCLES);
}

uint8_t * check_cycles(uint8_t * dst) 	Ivds
{
	dst = cmp_rr(dst, CYCLES, LIMIT, SZ_D);
	dst = jcc(dst, CC_G, 5);
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

uint8_t * translate_m68k_ea(m68k_op_info * op, x86_ea * dst, uint8_t * out, x86_68k_options * opts)
{
	int8_t reg = native_reg(op, opts);
	if (reg >= 0) {
		dst->mode = MODE_REG_DIRECT;
		dst->base = reg;
		return;
	}
	switch (op->addr_mode)
	{
	case MODE_REG:
	case MODE_AREG:
		dst->mode = MODE_DISPLACE8;
		dst->base = CONTEXT;
		dst->disp = (op->addr_mode = MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs)) + 4 * op->params.regs.pri;
		break;
	case MODE_AREG_INDIRECT:
		
		break;
	}
}

uint8_t * translate_m68k(uint8_t * dst, m68kinst * inst, x86_68k_options * opts)
{
	int8_t reg_a, reg_b, flags_reg;
	uint8_t dir = 0;
	int32_t offset;
	switch(inst->op)
	{
	case M68K_ABCD:
	case M68K_ADD:
	case M68K_ADDX:
	case M68K_AND:
	case M68K_ANDI_CCR:
	case M68K_ANDI_SR:
	case M68K_ASL:
	case M68K_ASR:
	case M68K_BCC:
	case M68K_BCHG:
	case M68K_BCLR:
	case M68K_BSET:
	case M68K_BSR:
	case M68K_BTST:
	case M68K_CHK:
	case M68K_CLR:
	case M68K_CMP:
	case M68K_DBCC:
	case M68K_DIVS:
	case M68K_DIVU:
	case M68K_EOR:
	case M68K_EORI_CCR:
	case M68K_EORI_SR:
	case M68K_EXG:
	case M68K_EXT:
	case M68K_ILLEGAL:
	case M68K_JMP:
	case M68K_JSR:
	case M68K_LEA:
	case M68K_LINK:
	case M68K_LSL:
	case M68K_LSR:
	case M68K_MOVE:
		
		if ((inst->src.addr_mode == MODE_REG || inst->src.addr_mode == MODE_AREG || (inst->src.addr_mode == MODE_IMMEDIATE && inst->src.variant == VAR_QUICK)) && (inst->dst.addr_mode == MODE_REG || inst->dst.addr_mode == MODE_AREG)) {
			dst = cycles(dst, BUS);
			reg_a = native_reg(&(inst->src), opts);
			reg_b = native_reg(&(inst->dst), opts);
			dst = cycles(dst, BUS);
			if (reg_a >= 0 && reg_b >= 0) {
				dst = mov_rr(dst, reg_a, reg_b, inst->extra.size);
				flags_reg = reg_b;
			} else if(reg_a >= 0) {
				offset = inst->dst.addr_mode == MODE_REG ? offsetof(m68k_context, dregs) : offsetof(m68k_context, aregs);
				dst = mov_rrdisp8(dst, reg_a, CONTEXT, offset + 4 * inst->dst.params.regs.pri, inst->extra.size);
				flags_reg = reg_a;
			} else if(reg_b >= 0) {
				if (inst->src.addr_mode == MODE_REG) {
					dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, dregs) + 4 * inst->src.params.regs.pri, reg_b, inst->extra.size);
				} else if(inst->src.addr_mode == MODE_AREG) {
					dst = mov_rdisp8r(dst, CONTEXT, offsetof(m68k_context, aregs) + 4 * inst->src.params.regs.pri, reg_b, inst->extra.size);
				} else {
					dst = mov_i32r(dst, inst->src.params.u32, reg_b);
				}
				flags_reg = reg_b;
			} else {
				
			}
			dst = mov_i8r(dst, 0, FLAG_V);
			dst = mov_i8r(dst, 0, FLAG_C);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				dst = cmp_i8r(dst, 0, reg_b, SZ_B);
				break;
			case OPSIZE_WORD:
				dst = cmp_i8r(dst, 0, reg_b, SZ_W);
				break;
			case OPSIZE_LONG:
				dst = cmp_i8r(dst, 0, reg_b, SZ_D);
				break;
			}
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = check_cycles(dst);
		}
		
		if (reg_a >= 0 && reg_b >= 0) {
			dst = cycles(dst, BUS);
			dst = mov_rr(dst, reg_a, reg_b, inst->extra.size);
			dst = mov_i8r(dst, 0, FLAG_V);
			dst = mov_i8r(dst, 0, FLAG_C);
			switch (inst->extra.size)
			{
			case OPSIZE_BYTE:
				dst = cmp_i8r(dst, 0, reg_b, SZ_B);
				break;
			case OPSIZE_WORD:
				dst = cmp_i8r(dst, 0, reg_b, SZ_W);
				break;
			case OPSIZE_LONG:
				dst = cmp_i8r(dst, 0, reg_b, SZ_D);
				break;
			}
			dst = setcc_r(dst, CC_Z, FLAG_Z);
			dst = setcc_r(dst, CC_S, FLAG_N);
			dst = check_cycles(dst);
		} else if(reg_a >= 0 || reg_b >= 0) {
			if (reg_a >= 0) {
				switch (inst->dst.addr_mode)
				{
				case MODE_REG:
					dst = cycles(dst, BUS);
					dst = mov_rr(dst, reg_a, reg_b, inst->extra.size);
					dst = check_cycles(dst);
					break;
				case MODE_AREG:
					break;
				}
			} else {
			}
		}
		break;
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
	case M68K_NOP:
	case M68K_NOT:
	case M68K_OR:
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
	case M68K_RTS:
	case M68K_SBCD:
	case M68K_SCC:
	case M68K_STOP:
	case M68K_SUB:
	case M68K_SUBX:
	case M68K_SWAP:
	case M68K_TAS:
	case M68K_TRAP:
	case M68K_TRAPV:
	case M68K_TST:
	case M68K_UNLK:
	case M68K_INVALID:
		break;
	}
}

