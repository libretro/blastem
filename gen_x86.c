#include "gen_x86.h"
#include "68kinst.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define REX_RM_FIELD 0x1
#define REX_SIB_FIELD 0x2
#define REX_REG_FIELD 0x4
#define REX_QUAD 0x8

#define OP_ADD 0x00
#define OP_OR  0x08
#define PRE_2BYTE 0x0F
#define OP_ADC 0x10
#define OP_SBB 0x18
#define OP_AND 0x20
#define OP_SUB 0x28
#define OP_XOR 0x30
#define OP_CMP 0x38
#define PRE_REX 0x40
#define OP_PUSH 0x50
#define OP_POP 0x58
#define OP_MOVSXD 0x63
#define PRE_SIZE 0x66
#define OP_JCC 0x70
#define OP_IMMED_ARITH 0x80
#define OP_XCHG 0x86
#define OP_MOV 0x88
#define OP_XCHG_AX 0x90
#define OP_CDQ 0x99
#define OP_PUSHF 0x9C
#define OP_POPF 0x9D
#define OP_MOV_I8R 0xB0
#define OP_MOV_IR 0xB8
#define OP_SHIFTROT_IR 0xC0
#define OP_RETN 0xC3
#define OP_MOV_IEA 0xC6
#define OP_SHIFTROT_1 0xD0
#define OP_SHIFTROT_CL 0xD2
#define OP_LOOP 0xE2
#define OP_CALL 0xE8
#define OP_JMP 0xE9
#define OP_JMP_BYTE 0xEB
#define OP_NOT_NEG 0xF6
#define OP_SINGLE_EA 0xFF

#define OP2_JCC 0x80
#define OP2_SETCC 0x90
#define OP2_BT 0xA3
#define OP2_BTS 0xAB
#define OP2_IMUL 0xAF
#define OP2_BTR 0xB3
#define OP2_BTX_I 0xBA
#define OP2_BTC 0xBB
#define OP2_MOVSX 0xBE
#define OP2_MOVZX 0xB6

#define OP_EX_ADDI 0x0
#define OP_EX_ORI  0x1
#define OP_EX_ADCI 0x2
#define OP_EX_SBBI 0x3
#define OP_EX_ANDI 0x4
#define OP_EX_SUBI 0x5
#define OP_EX_XORI 0x6
#define OP_EX_CMPI 0x7

#define OP_EX_ROL 0x0
#define OP_EX_ROR 0x1
#define OP_EX_RCL 0x2
#define OP_EX_RCR 0x3
#define OP_EX_SHL 0x4
#define OP_EX_SHR 0x5
#define OP_EX_SAL 0x6 //identical to SHL
#define OP_EX_SAR 0x7

#define OP_EX_BT  0x4
#define OP_EX_BTS 0x5
#define OP_EX_BTR 0x6
#define OP_EX_BTC 0x7

#define OP_EX_TEST_I 0x0
#define OP_EX_NOT    0x2
#define OP_EX_NEG    0x3
#define OP_EX_MUL    0x4
#define OP_EX_IMUL   0x5
#define OP_EX_DIV    0x6
#define OP_EX_IDIV   0x7

#define OP_EX_INC     0x0
#define OP_EX_DEC     0x1
#define OP_EX_CALL_EA 0x2
#define OP_EX_JMP_EA  0x4
#define OP_EX_PUSH_EA 0x6

#define BIT_IMMED_RAX 0x4
#define BIT_DIR 0x2
#define BIT_SIZE 0x1


enum {
	X86_RAX = 0,
	X86_RCX,
	X86_RDX,
	X86_RBX,
	X86_RSP,
	X86_RBP,
	X86_RSI,
	X86_RDI,
	X86_AH=4,
	X86_CH,
	X86_DH,
	X86_BH,
	X86_R8=0,
	X86_R9,
	X86_R10,
	X86_R11,
	X86_R12,
	X86_R13,
	X86_R14,
	X86_R15
} x86_regs_enc;

uint8_t * x86_rr_sizedir(uint8_t * out, uint16_t opcode, uint8_t src, uint8_t dst, uint8_t size)
{
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_B && dst >= RSP && dst <= RDI) {
		opcode |= BIT_DIR;
		tmp = dst;
		dst = src;
		src = tmp;
	}
	if (size == SZ_Q || src >= R8 || dst >= R8 || (size == SZ_B && src >= RSP && src <= RDI)) {
		*out = PRE_REX;
		if (src >= AH && src <= BH || dst >= AH && dst <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_REG_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (src >= AH && src <= BH) {
			src -= (AH-X86_AH);
		}
		if (dst >= AH && dst <= BH) {
			dst -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	if (opcode >= 0x100) {
		*(out++) = opcode >> 8;
		*(out++) = opcode;
	} else {
		*(out++) = opcode;
	}
	*(out++) = MODE_REG_DIRECT | dst | (src << 3);
	return out;
}

uint8_t * x86_rrdisp8_sizedir(uint8_t * out, uint16_t opcode, uint8_t reg, uint8_t base, int8_t disp, uint8_t size, uint8_t dir)
{
	//TODO: Deal with the fact that AH, BH, CH and DH can only be in the R/M param when there's a REX prefix
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || reg >= R8 || base >= R8 || (size == SZ_B && reg >= RSP && reg <= RDI)) {
		*out = PRE_REX;
		if (reg >= AH && reg <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (reg >= R8) {
			*out |= REX_REG_FIELD;
			reg -= (R8 - X86_R8);
		}
		if (base >= R8) {
			*out |= REX_RM_FIELD;
			base -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (reg >= AH && reg <= BH) {
			reg -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	opcode |= dir;
	if (opcode >= 0x100) {
		*(out++) = opcode >> 8;
		*(out++) = opcode;
	} else {
		*(out++) = opcode;
	}
	*(out++) = MODE_REG_DISPLACE8 | base | (reg << 3);
	if (base == RSP) {
		//add SIB byte, with no index and RSP as base
		*(out++) = (RSP << 3) | RSP;
	}
	*(out++) = disp;
	return out;
}

uint8_t * x86_rrdisp32_sizedir(uint8_t * out, uint16_t opcode, uint8_t reg, uint8_t base, int32_t disp, uint8_t size, uint8_t dir)
{
	//TODO: Deal with the fact that AH, BH, CH and DH can only be in the R/M param when there's a REX prefix
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || reg >= R8 || base >= R8 || (size == SZ_B && reg >= RSP && reg <= RDI)) {
		*out = PRE_REX;
		if (reg >= AH && reg <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (reg >= R8) {
			*out |= REX_REG_FIELD;
			reg -= (R8 - X86_R8);
		}
		if (base >= R8) {
			*out |= REX_RM_FIELD;
			base -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (reg >= AH && reg <= BH) {
			reg -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	opcode |= dir;
	if (opcode >= 0x100) {
		*(out++) = opcode >> 8;
		*(out++) = opcode;
	} else {
		*(out++) = opcode;
	}
	*(out++) = MODE_REG_DISPLACE32 | base | (reg << 3);
	if (base == RSP) {
		//add SIB byte, with no index and RSP as base
		*(out++) = (RSP << 3) | RSP;
	}
	*(out++) = disp;
	*(out++) = disp >> 8;
	*(out++) = disp >> 16;
	*(out++) = disp >> 24;
	return out;
}

uint8_t * x86_rrind_sizedir(uint8_t * out, uint8_t opcode, uint8_t reg, uint8_t base, uint8_t size, uint8_t dir)
{
	//TODO: Deal with the fact that AH, BH, CH and DH can only be in the R/M param when there's a REX prefix
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || reg >= R8 || base >= R8 || (size == SZ_B && reg >= RSP && reg <= RDI)) {
		*out = PRE_REX;
		if (reg >= AH && reg <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (reg >= R8) {
			*out |= REX_REG_FIELD;
			reg -= (R8 - X86_R8);
		}
		if (base >= R8) {
			*out |= REX_RM_FIELD;
			base -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (reg >= AH && reg <= BH) {
			reg -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode | dir;
	*(out++) = MODE_REG_INDIRECT | base | (reg << 3);
	if (base == RSP) {
		//add SIB byte, with no index and RSP as base
		*(out++) = (RSP << 3) | RSP;
	}
	return out;
}

uint8_t * x86_rrindex_sizedir(uint8_t * out, uint8_t opcode, uint8_t reg, uint8_t base, uint8_t index, uint8_t scale, uint8_t size, uint8_t dir)
{
	//TODO: Deal with the fact that AH, BH, CH and DH can only be in the R/M param when there's a REX prefix
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || reg >= R8 || base >= R8 || (size == SZ_B && reg >= RSP && reg <= RDI)) {
		*out = PRE_REX;
		if (reg >= AH && reg <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (reg >= R8) {
			*out |= REX_REG_FIELD;
			reg -= (R8 - X86_R8);
		}
		if (base >= R8) {
			*out |= REX_RM_FIELD;
			base -= (R8 - X86_R8);
		}
		if (index >= R8) {
			*out |= REX_SIB_FIELD;
			index -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (reg >= AH && reg <= BH) {
			reg -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode | dir;
	*(out++) = MODE_REG_INDIRECT | base | (RSP << 3);
	if (base == RSP) {
		if (scale == 4) {
			scale = 3;
		}
		*(out++) = scale << 6 | (index << 3) | base;
	}
	return out;
}

uint8_t * x86_r_size(uint8_t * out, uint8_t opcode, uint8_t opex, uint8_t dst, uint8_t size)
{
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8) {
		*out = PRE_REX;
		if (dst >= AH && dst <= BH) {
			fprintf(stderr, "attempt to use *H reg in an instruction requiring REX prefix. opcode = %X\n", opcode);
			exit(1);
		}
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (size == SZ_B) {
		if (dst >= AH && dst <= BH) {
			dst -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode;
	*(out++) = MODE_REG_DIRECT | dst | (opex << 3);
	return out;
}

uint8_t * x86_rdisp8_size(uint8_t * out, uint8_t opcode, uint8_t opex, uint8_t dst, int8_t disp, uint8_t size)
{
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (size != SZ_B) {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode;
	*(out++) = MODE_REG_DISPLACE8 | dst | (opex << 3);
	*(out++) = disp;
	return out;
}

uint8_t * x86_ir(uint8_t * out, uint8_t opcode, uint8_t op_ex, uint8_t al_opcode, int32_t val, uint8_t dst, uint8_t size)
{
	uint8_t sign_extend = 0;
	if ((size == SZ_D || size == SZ_Q) && val <= 0x7F && val >= -0x80) {
		sign_extend = 1;
		opcode |= BIT_DIR;
	}
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (dst == RAX && !sign_extend) {
		if (size != SZ_B) {
			al_opcode |= BIT_SIZE;
			if (size == SZ_Q) {
				*out = PRE_REX | REX_QUAD;
			}
		}
		*(out++) = al_opcode | BIT_IMMED_RAX;
	} else {
		if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
			*out = PRE_REX;
			if (size == SZ_Q) {
				*out |= REX_QUAD;
			}
			if (dst >= R8) {
				*out |= REX_RM_FIELD;
				dst -= (R8 - X86_R8);
			}
			out++;
		}
		if (dst >= AH && dst <= BH) {
			dst -= (AH-X86_AH);
		}
		if (size != SZ_B) {
			opcode |= BIT_SIZE;
		}
		*(out++) = opcode;
		*(out++) = MODE_REG_DIRECT | dst | (op_ex << 3);
	}
	*(out++) = val;
	if (size != SZ_B && !sign_extend) {
		val >>= 8;
		*(out++) = val;
		if (size != SZ_W) {
			val >>= 8;
			*(out++) = val;
			val >>= 8;
			*(out++) = val;
		}
	}
	return out;
}

uint8_t * x86_irdisp8(uint8_t * out, uint8_t opcode, uint8_t op_ex, int32_t val, uint8_t dst, int8_t disp, uint8_t size)
{
	uint8_t sign_extend = 0;
	if ((size == SZ_D || size == SZ_Q) && val <= 0x7F && val >= -0x80) {
		sign_extend = 1;
		opcode |= BIT_DIR;
	}
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}

	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (size != SZ_B) {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode;
	*(out++) = MODE_REG_DISPLACE8 | dst | (op_ex << 3);
	*(out++) = disp;
	*(out++) = val;
	if (size != SZ_B && !sign_extend) {
		val >>= 8;
		*(out++) = val;
		if (size != SZ_W) {
			val >>= 8;
			*(out++) = val;
			val >>= 8;
			*(out++) = val;
		}
	}
	return out;
}


uint8_t * x86_shiftrot_ir(uint8_t * out, uint8_t op_ex, uint8_t val, uint8_t dst, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}

	*(out++) = (val == 1 ? OP_SHIFTROT_1: OP_SHIFTROT_IR) | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DIRECT | dst | (op_ex << 3);
	if (val != 1) {
		*(out++) = val;
	}
	return out;
}

uint8_t * x86_shiftrot_irdisp8(uint8_t * out, uint8_t op_ex, uint8_t val, uint8_t dst, int8_t disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}

	*(out++) = (val == 1 ? OP_SHIFTROT_1: OP_SHIFTROT_IR) | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DISPLACE8 | dst | (op_ex << 3);
	*(out++) = disp;
	if (val != 1) {
		*(out++) = val;
	}
	return out;
}

uint8_t * x86_shiftrot_clr(uint8_t * out, uint8_t op_ex, uint8_t dst, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}

	*(out++) = OP_SHIFTROT_CL | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DIRECT | dst | (op_ex << 3);
	return out;
}

uint8_t * x86_shiftrot_clrdisp8(uint8_t * out, uint8_t op_ex, uint8_t dst, int8_t disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}

	*(out++) = OP_SHIFTROT_CL | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DISPLACE8 | dst | (op_ex << 3);
	*(out++) = disp;
	return out;
}

uint8_t * rol_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_ROL, val, dst, size);
}

uint8_t * ror_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_ROR, val, dst, size);
}

uint8_t * rcl_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_RCL, val, dst, size);
}

uint8_t * rcr_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_RCR, val, dst, size);
}

uint8_t * shl_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_SHL, val, dst, size);
}

uint8_t * shr_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_SHR, val, dst, size);
}

uint8_t * sar_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_ir(out, OP_EX_SAR, val, dst, size);
}

uint8_t * rol_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_ROL, val, dst_base, disp, size);
}

uint8_t * ror_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_ROR, val, dst_base, disp, size);
}

uint8_t * rcl_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_RCL, val, dst_base, disp, size);
}

uint8_t * rcr_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_RCR, val, dst_base, disp, size);
}

uint8_t * shl_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_SHL, val, dst_base, disp, size);
}

uint8_t * shr_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_SHR, val, dst_base, disp, size);
}

uint8_t * sar_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_irdisp8(out, OP_EX_SAR, val, dst_base, disp, size);
}

uint8_t * rol_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_ROL, dst, size);
}

uint8_t * ror_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_ROR, dst, size);
}

uint8_t * rcl_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_RCL, dst, size);
}

uint8_t * rcr_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_RCR, dst, size);
}

uint8_t * shl_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_SHL, dst, size);
}

uint8_t * shr_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_SHR, dst, size);
}

uint8_t * sar_clr(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_shiftrot_clr(out, OP_EX_SAR, dst, size);
}

uint8_t * rol_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_ROL, dst_base, disp, size);
}

uint8_t * ror_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_ROR, dst_base, disp, size);
}

uint8_t * rcl_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_RCL, dst_base, disp, size);
}

uint8_t * rcr_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_RCR, dst_base, disp, size);
}

uint8_t * shl_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_SHL, dst_base, disp, size);
}

uint8_t * shr_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_SHR, dst_base, disp, size);
}

uint8_t * sar_clrdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_shiftrot_clrdisp8(out, OP_EX_SAR, dst_base, disp, size);
}

uint8_t * add_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_ADD, src, dst, size);
}

uint8_t * add_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_ADDI, OP_ADD, val, dst, size);
}

uint8_t * add_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_ADDI, val, dst_base, disp, size);
}

uint8_t * add_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_ADD, src, dst_base, disp, size, 0);
}

uint8_t * add_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_ADD, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * adc_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_ADC, src, dst, size);
}

uint8_t * adc_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_ADCI, OP_ADC, val, dst, size);
}

uint8_t * adc_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_ADCI, val, dst_base, disp, size);
}

uint8_t * adc_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_ADC, src, dst_base, disp, size, 0);
}

uint8_t * adc_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_ADC, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * or_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_OR, src, dst, size);
}
uint8_t * or_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_ORI, OP_OR, val, dst, size);
}

uint8_t * or_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_ORI, val, dst_base, disp, size);
}

uint8_t * or_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_OR, src, dst_base, disp, size, 0);
}

uint8_t * or_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_OR, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * and_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_AND, src, dst, size);
}

uint8_t * and_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_ANDI, OP_AND, val, dst, size);
}

uint8_t * and_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_ANDI, val, dst_base, disp, size);
}

uint8_t * and_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_AND, src, dst_base, disp, size, 0);
}

uint8_t * and_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_AND, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * xor_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_XOR, src, dst, size);
}

uint8_t * xor_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_XORI, OP_XOR, val, dst, size);
}

uint8_t * xor_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_XORI, val, dst_base, disp, size);
}

uint8_t * xor_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_XOR, src, dst_base, disp, size, 0);
}

uint8_t * xor_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_XOR, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * sub_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_SUB, src, dst, size);
}

uint8_t * sub_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_SUBI, OP_SUB, val, dst, size);
}

uint8_t * sub_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_SUBI, val, dst_base, disp, size);
}

uint8_t * sub_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_SUB, src, dst_base, disp, size, 0);
}

uint8_t * sub_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_SUB, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * sbb_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_SBB, src, dst, size);
}

uint8_t * sbb_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_SBBI, OP_SBB, val, dst, size);
}

uint8_t * sbb_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_SBBI, val, dst_base, disp, size);
}

uint8_t * sbb_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_SBB, src, dst_base, disp, size, 0);
}

uint8_t * sbb_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_SBB, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * cmp_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_CMP, src, dst, size);
}

uint8_t * cmp_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	return x86_ir(out, OP_IMMED_ARITH, OP_EX_CMPI, OP_CMP, val, dst, size);
}

uint8_t * cmp_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_irdisp8(out, OP_IMMED_ARITH, OP_EX_CMPI, val, dst_base, disp, size);
}

uint8_t * cmp_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_CMP, src, dst_base, disp, size, 0);
}

uint8_t * cmp_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_CMP, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * imul_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP2_IMUL | (PRE_2BYTE << 8), dst, src, size);
}

uint8_t * imul_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP2_IMUL | (PRE_2BYTE << 8), dst, src_base, disp, size, 0);
}

uint8_t * not_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_NOT, dst, size);
}

uint8_t * neg_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_NEG, dst, size);
}

uint8_t * not_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_NOT, dst_base, disp, size);
}

uint8_t * neg_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_NEG, dst_base, disp, size);
}

uint8_t * mul_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_MUL, dst, size);
}

uint8_t * imul_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_IMUL, dst, size);
}

uint8_t * div_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_DIV, dst, size);
}

uint8_t * idiv_r(uint8_t * out, uint8_t dst, uint8_t size)
{
	return x86_r_size(out, OP_NOT_NEG, OP_EX_IDIV, dst, size);
}

uint8_t * mul_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_MUL, dst_base, disp, size);
}

uint8_t * imul_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_IMUL, dst_base, disp, size);
}

uint8_t * div_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_DIV, dst_base, disp, size);
}

uint8_t * idiv_rdisp8(uint8_t * out, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rdisp8_size(out, OP_NOT_NEG, OP_EX_IDIV, dst_base, disp, size);
}

uint8_t * mov_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rr_sizedir(out, OP_MOV, src, dst, size);
}

uint8_t * mov_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_MOV, src, dst_base, disp, size, 0);
}

uint8_t * mov_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp8_sizedir(out, OP_MOV, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * mov_rrdisp32(uint8_t * out, uint8_t src, uint8_t dst_base, int32_t disp, uint8_t size)
{
	return x86_rrdisp32_sizedir(out, OP_MOV, src, dst_base, disp, size, 0);
}

uint8_t * mov_rdisp32r(uint8_t * out, uint8_t src_base, int32_t disp, uint8_t dst, uint8_t size)
{
	return x86_rrdisp32_sizedir(out, OP_MOV, dst, src_base, disp, size, BIT_DIR);
}

uint8_t * mov_rrind(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rrind_sizedir(out, OP_MOV, src, dst, size, 0);
}

uint8_t * mov_rindr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return x86_rrind_sizedir(out, OP_MOV, dst, src, size, BIT_DIR);
}

uint8_t * mov_rrindex(uint8_t * out, uint8_t src, uint8_t dst_base, uint8_t dst_index, uint8_t scale, uint8_t size)
{
	return x86_rrindex_sizedir(out, OP_MOV, src, dst_base, dst_index, scale, size, 0);
}

uint8_t * mov_rindexr(uint8_t * out, uint8_t src_base, uint8_t src_index, uint8_t scale, uint8_t dst, uint8_t size)
{
	return x86_rrindex_sizedir(out, OP_MOV, dst, src_base, src_index, scale, size, BIT_DIR);
}

uint8_t * mov_ir(uint8_t * out, int64_t val, uint8_t dst, uint8_t size)
{	
	uint8_t sign_extend = 0;
	if (size == SZ_Q && val <= 0x7FFFFFFF && val >= -2147483648) {
		sign_extend = 1;
	}
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}
	if (size == SZ_B) {
		*(out++) = OP_MOV_I8R | dst;
	} else if (size == SZ_Q && sign_extend) {
		*(out++) = OP_MOV_IEA | BIT_SIZE;
		*(out++) = MODE_REG_DIRECT | dst;
	} else {
		*(out++) = OP_MOV_IR | dst;
	}
	*(out++) = val;
	if (size != SZ_B) {
		val >>= 8;
		*(out++) = val;
		if (size != SZ_W) {
			val >>= 8;
			*(out++) = val;
			val >>= 8;
			*(out++) = val;
			if (size == SZ_Q && !sign_extend) {
				val >>= 8;
				*(out++) = val;
				val >>= 8;
				*(out++) = val;
				val >>= 8;
				*(out++) = val;
				val >>= 8;
				*(out++) = val;
			}
		}
	}
	return out;
}

uint8_t * mov_irdisp8(uint8_t * out, int32_t val, uint8_t dst, int8_t disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}
	*(out++) = OP_MOV_IEA | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DISPLACE8 | dst;
	*(out++) = disp;

	*(out++) = val;
	if (size != SZ_B) {
		val >>= 8;
		*(out++) = val;
		if (size != SZ_W) {
			val >>= 8;
			*(out++) = val;
			val >>= 8;
			*(out++) = val;
		}
	}
	return out;
}

uint8_t * mov_irind(uint8_t * out, int32_t val, uint8_t dst, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || (size == SZ_B && dst >= RSP && dst <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (dst >= AH && dst <= BH) {
		dst -= (AH-X86_AH);
	}
	*(out++) = OP_MOV_IEA | (size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_INDIRECT | dst;

	*(out++) = val;
	if (size != SZ_B) {
		val >>= 8;
		*(out++) = val;
		if (size != SZ_W) {
			val >>= 8;
			*(out++) = val;
			val >>= 8;
			*(out++) = val;
		}
	}
	return out;
}

uint8_t * movsx_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t src_size, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || src >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_RM_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_REG_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (src_size == SZ_D) {
		*(out++) = OP_MOVSXD;
	} else {
		*(out++) = PRE_2BYTE;
		*(out++) = OP2_MOVSX | (src_size == SZ_B ? 0 : BIT_SIZE);
	}
	*(out++) = MODE_REG_DIRECT | src | (dst << 3);
	return out;
}

uint8_t * movsx_rdisp8r(uint8_t * out, uint8_t src, int8_t disp, uint8_t dst, uint8_t src_size, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || src >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_RM_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_REG_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	if (src_size == SZ_D) {
		*(out++) = OP_MOVSXD;
	} else {
		*(out++) = PRE_2BYTE;
		*(out++) = OP2_MOVSX | (src_size == SZ_B ? 0 : BIT_SIZE);
	}
	*(out++) = MODE_REG_DISPLACE8 | src | (dst << 3);
	*(out++) = disp;
	return out;
}

uint8_t * movzx_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t src_size, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || src >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_RM_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_REG_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_MOVZX | (src_size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DIRECT | src | (dst << 3);
	return out;
}

uint8_t * movzx_rdisp8r(uint8_t * out, uint8_t src, int8_t disp, uint8_t dst, uint8_t src_size, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8 || src >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_RM_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_REG_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_MOVZX | (src_size == SZ_B ? 0 : BIT_SIZE);
	*(out++) = MODE_REG_DISPLACE8 | src | (dst << 3);
	*(out++) = disp;
	return out;
}

uint8_t * xchg_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	//TODO: Use OP_XCHG_AX when one of the registers is AX, EAX or RAX
	uint8_t tmp;
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_B && dst >= RSP && dst <= RDI) {
		tmp = dst;
		dst = src;
		src = tmp;
	}
	if (size == SZ_Q || src >= R8 || dst >= R8 || (size == SZ_B && src >= RSP && src <= RDI)) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_REG_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	uint8_t opcode = OP_XCHG;
	if (size == SZ_B) {
		if (src >= AH && src <= BH) {
			src -= (AH-X86_AH);
		}
		if (dst >= AH && dst <= BH) {
			dst -= (AH-X86_AH);
		}
	} else {
		opcode |= BIT_SIZE;
	}
	*(out++) = opcode;
	*(out++) = MODE_REG_DIRECT | dst | (src << 3);
	return out;
}

uint8_t * pushf(uint8_t * out)
{
	*(out++) = OP_PUSHF;
	return out;
}

uint8_t * popf(uint8_t * out)
{
	*(out++) = OP_POPF;
	return out;
}

uint8_t * push_r(uint8_t * out, uint8_t reg)
{
	if (reg >= R8) {
		*(out++) = PRE_REX | REX_RM_FIELD;
		reg -= R8 - X86_R8;
	}
	*(out++) = OP_PUSH | reg;
	return out;
}

uint8_t * pop_r(uint8_t * out, uint8_t reg)
{
	if (reg >= R8) {
		*(out++) = PRE_REX | REX_RM_FIELD;
		reg -= R8 - X86_R8;
	}
	*(out++) = OP_POP | reg;
	return out;
}

uint8_t * setcc_r(uint8_t * out, uint8_t cc, uint8_t dst)
{
	if (dst >= R8) {
		*(out++) = PRE_REX | REX_RM_FIELD;
		dst -= R8 - X86_R8;
	} else if (dst >= RSP && dst <= RDI) {
		*(out++) = PRE_REX;
	} else if (dst >= AH && dst <= BH) {
		dst -= AH - X86_AH;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_SETCC | cc;
	*(out++) = MODE_REG_DIRECT | dst;
	return out;
}

uint8_t * setcc_rind(uint8_t * out, uint8_t cc, uint8_t dst)
{
	if (dst >= R8) {
		*(out++) = PRE_REX | REX_RM_FIELD;
		dst -= R8 - X86_R8;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_SETCC | cc;
	*(out++) = MODE_REG_INDIRECT | dst;
	return out;
}

uint8_t * setcc_rdisp8(uint8_t * out, uint8_t cc, uint8_t dst, int8_t disp)
{
	if (dst >= R8) {
		*(out++) = PRE_REX | REX_RM_FIELD;
		dst -= R8 - X86_R8;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_SETCC | cc;
	*(out++) = MODE_REG_DISPLACE8 | dst;
	*(out++) = disp;
	return out;
}

uint8_t * bit_rr(uint8_t * out, uint8_t op2, uint8_t src, uint8_t dst, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || src >= R8 || dst >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_REG_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = op2;
	*(out++) = MODE_REG_DIRECT | dst | (src << 3);
	return out;
}

uint8_t * bit_rrdisp8(uint8_t * out, uint8_t op2, uint8_t src, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || src >= R8 || dst_base >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_REG_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst_base >= R8) {
			*out |= REX_RM_FIELD;
			dst_base -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = op2;
	*(out++) = MODE_REG_DISPLACE8 | dst_base | (src << 3);
	*(out++) = dst_disp;
	return out;
}

uint8_t * bit_rrdisp32(uint8_t * out, uint8_t op2, uint8_t src, uint8_t dst_base, int32_t dst_disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || src >= R8 || dst_base >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (src >= R8) {
			*out |= REX_REG_FIELD;
			src -= (R8 - X86_R8);
		}
		if (dst_base >= R8) {
			*out |= REX_RM_FIELD;
			dst_base -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = op2;
	*(out++) = MODE_REG_DISPLACE32 | dst_base | (src << 3);
	*(out++) = dst_disp;
	*(out++) = dst_disp >> 8;
	*(out++) = dst_disp >> 16;
	*(out++) = dst_disp >> 24;
	return out;
}

uint8_t * bit_ir(uint8_t * out, uint8_t op_ex, uint8_t val, uint8_t dst, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst >= R8) {
			*out |= REX_RM_FIELD;
			dst -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_BTX_I;
	*(out++) = MODE_REG_DIRECT | dst | (op_ex << 3);
	*(out++) = val;
	return out;
}

uint8_t * bit_irdisp8(uint8_t * out, uint8_t op_ex, uint8_t val, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	if (size == SZ_W) {
		*(out++) = PRE_SIZE;
	}
	if (size == SZ_Q || dst_base >= R8) {
		*out = PRE_REX;
		if (size == SZ_Q) {
			*out |= REX_QUAD;
		}
		if (dst_base >= R8) {
			*out |= REX_RM_FIELD;
			dst_base -= (R8 - X86_R8);
		}
		out++;
	}
	*(out++) = PRE_2BYTE;
	*(out++) = OP2_BTX_I;
	*(out++) = MODE_REG_DISPLACE8 | dst_base | (op_ex << 3);
	*(out++) = dst_disp;
	*(out++) = val;
	return out;
}

uint8_t * bt_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return bit_rr(out, OP2_BT, src, dst, size);
}

uint8_t * bt_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_rrdisp8(out, OP2_BT, src, dst_base, dst_disp, size);
}

uint8_t * bt_rrdisp32(uint8_t * out, uint8_t src, uint8_t dst_base, int32_t dst_disp, uint8_t size)
{
	return bit_rrdisp32(out, OP2_BT, src, dst_base, dst_disp, size);
}

uint8_t * bt_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return bit_ir(out, OP_EX_BT, val, dst, size);
}

uint8_t * bt_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_irdisp8(out, OP_EX_BT, val, dst_base, dst_disp, size);
}

uint8_t * bts_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return bit_rr(out, OP2_BTS, src, dst, size);
}

uint8_t * bts_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_rrdisp8(out, OP2_BTS, src, dst_base, dst_disp, size);
}

uint8_t * bts_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return bit_ir(out, OP_EX_BTS, val, dst, size);
}

uint8_t * bts_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_irdisp8(out, OP_EX_BTS, val, dst_base, dst_disp, size);
}

uint8_t * btr_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return bit_rr(out, OP2_BTR, src, dst, size);
}

uint8_t * btr_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_rrdisp8(out, OP2_BTR, src, dst_base, dst_disp, size);
}

uint8_t * btr_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return bit_ir(out, OP_EX_BTR, val, dst, size);
}

uint8_t * btr_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_irdisp8(out, OP_EX_BTR, val, dst_base, dst_disp, size);
}

uint8_t * btc_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size)
{
	return bit_rr(out, OP2_BTC, src, dst, size);
}

uint8_t * btc_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_rrdisp8(out, OP2_BTC, src, dst_base, dst_disp, size);
}

uint8_t * btc_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size)
{
	return bit_ir(out, OP_EX_BTC, val, dst, size);
}

uint8_t * btc_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t dst_disp, uint8_t size)
{
	return bit_irdisp8(out, OP_EX_BTC, val, dst_base, dst_disp, size);
}

uint8_t * jcc(uint8_t * out, uint8_t cc, uint8_t * dest)
{
	ptrdiff_t disp = dest-(out+2);
	if (disp <= 0x7F && disp >= -0x80) {
		*(out++) = OP_JCC | cc;
		*(out++) = disp;
	} else {
		disp = dest-(out+6);
		if (disp <= 0x7FFFFFFF && disp >= -2147483648) {
			*(out++) = PRE_2BYTE;
			*(out++) = OP2_JCC | cc;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
		} else {
			printf("%p - %p = %lX\n", dest, out + 6, disp);
			return NULL;
		}
	}
	return out;
}

uint8_t * jmp(uint8_t * out, uint8_t * dest)
{
	ptrdiff_t disp = dest-(out+2);
	if (disp <= 0x7F && disp >= -0x80) {
		*(out++) = OP_JMP_BYTE;
		*(out++) = disp;
	} else {
		disp = dest-(out+5);
		if (disp <= 0x7FFFFFFF && disp >= -2147483648) {
			*(out++) = OP_JMP;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
		} else {
			printf("%p - %p = %lX\n", dest, out + 6, disp);
			return NULL;
		}
	}
	return out;
}

uint8_t * jmp_r(uint8_t * out, uint8_t dst)
{
	if (dst >= R8) {
		dst -= R8 - X86_R8;
		*(out++) = PRE_REX | REX_RM_FIELD;
	}
	*(out++) = OP_SINGLE_EA;
	*(out++) = MODE_REG_DIRECT | dst | (OP_EX_JMP_EA << 3);
	return out;
}

uint8_t * call(uint8_t * out, uint8_t * fun)
{
	ptrdiff_t disp = fun-(out+5);
	if (disp <= 0x7FFFFFFF && disp >= -2147483648) {
		*(out++) = OP_CALL;
		*(out++) = disp;
		disp >>= 8;
		*(out++) = disp;
		disp >>= 8;
		*(out++) = disp;
		disp >>= 8;
		*(out++) = disp;
	} else {
		//TODO: Implement far call???
		printf("%p - %p = %lX\n", fun, out + 5, disp);
		return NULL;
	}
	return out;
}

uint8_t * call_r(uint8_t * out, uint8_t dst)
{
	*(out++) = OP_SINGLE_EA;
	*(out++) = MODE_REG_DIRECT | dst | (OP_EX_CALL_EA << 3);
	return out;
}

uint8_t * retn(uint8_t * out)
{
	*(out++) = OP_RETN;
	return out;
}

uint8_t * cdq(uint8_t * out)
{
	*(out++) = OP_CDQ;
	return out;
}

uint8_t * loop(uint8_t * out, uint8_t * dst)
{
	ptrdiff_t disp = dst-(out+2);
	*(out++) = OP_LOOP;
	*(out++) = disp;
	return out;
}
