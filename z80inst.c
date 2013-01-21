#include "z80inst.h"
#include <string.h>
#include <stdio.h>

z80inst z80_tbl_a[256] = {
	//0
	{Z80_NOP, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_BC, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT | Z80_DIR, Z80_BC, 0},
	{Z80_INC, Z80_BC, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_B, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_B, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_B, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RLC, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_EX, Z80_AF, Z80_REG, Z80_AF, 0},
	{Z80_ADD, Z80_HL, Z80_REG, Z80_BC, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT, Z80_BC, 0},
	{Z80_DEC, Z80_BC, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_C, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_C, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_C, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RRC, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	//1
	{Z80_DJNZ, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_DE, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT | Z80_DIR, Z80_DE, 0},
	{Z80_INC, Z80_DE, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_D, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_D, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_D, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RL, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JR, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_ADD, Z80_HL, Z80_REG, Z80_DE, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT, Z80_DE, 0},
	{Z80_DEC, Z80_DE, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_E, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_E, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_E, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RR, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	//2
	{Z80_JRCC, Z80_CC_NZ, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_HL, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_HL, Z80_IMMED_INDIRECT | Z80_DIR, Z80_UNUSED, 0},
	{Z80_INC, Z80_HL, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_H, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_H, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_H, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_DAA, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JRCC, Z80_CC_Z, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_ADD, Z80_HL, Z80_REG, Z80_HL, 0},
	{Z80_LD, Z80_HL, Z80_IMMED_INDIRECT, Z80_IMMED, 0},
	{Z80_DEC, Z80_HL, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_L, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_L, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_L, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_CPL, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	//3
	{Z80_JRCC, Z80_CC_NC, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_SP, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_LD, Z80_A, Z80_IMMED_INDIRECT | Z80_DIR, Z80_UNUSED, 0},
	{Z80_INC, Z80_SP, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_UNUSED, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_DEC, Z80_UNUSED, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_USE_IMMED, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_SCF, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JRCC, Z80_CC_C, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_ADD, Z80_HL, Z80_REG, Z80_SP, 0},
	{Z80_LD, Z80_A, Z80_IMMED_INDIRECT, Z80_IMMED, 0},
	{Z80_DEC, Z80_SP, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_INC, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_DEC, Z80_A, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_CCF, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	//4
	{Z80_LD, Z80_B, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_B, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_B, Z80_REG, Z80_A, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_C, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_C, Z80_REG, Z80_A, 0},
	//5
	{Z80_LD, Z80_D, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_D, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_D, Z80_REG, Z80_A, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_E, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_E, Z80_REG, Z80_A, 0},
	//6
	{Z80_LD, Z80_H, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_H, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_H, Z80_REG, Z80_A, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_L, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_L, Z80_REG, Z80_A, 0},
	//7
	{Z80_LD, Z80_B, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_C, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_D, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_E, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_H, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_L, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_HALT, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT | Z80_DIR, Z80_HL, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_LD, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_LD, Z80_A, Z80_REG, Z80_A, 0},
	//8
	{Z80_ADD, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_ADD, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_ADD, Z80_A, Z80_REG, Z80_A, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_ADC, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_ADC, Z80_A, Z80_REG, Z80_A, 0},
	//9
	{Z80_SUB, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_SUB, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_SUB, Z80_A, Z80_REG, Z80_A, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_SBC, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_SBC, Z80_A, Z80_REG, Z80_A, 0},
	//A
	{Z80_AND, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_AND, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_AND, Z80_A, Z80_REG, Z80_A, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_XOR, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_XOR, Z80_A, Z80_REG, Z80_A, 0},
	//B
	{Z80_OR, Z80_A, Z80_REG, Z80_B, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_C, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_D, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_E, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_H, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_L, 0},
	{Z80_OR, Z80_A, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_OR, Z80_A, Z80_REG, Z80_A, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_B, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_C, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_D, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_E, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_H, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_L, 0},
	{Z80_OR, Z80_CP, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_OR, Z80_CP, Z80_REG, Z80_A, 0},
	//C
	{Z80_RETCC, Z80_CC_NZ, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_POP, Z80_BC, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_NZ, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_JP, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_CALLCC, Z80_CC_NZ, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_PUSH, Z80_BC, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_ADD, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x0},
	{Z80_RETCC, Z80_CC_Z, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_RET, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_Z, Z80_IMMED, Z80_UNUSED, 0},
	{0, 0, 0, 0, 0},//BITS Prefix
	{Z80_CALLCC, Z80_CC_Z, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_CALL, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_ADC, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x8},
	//D
	{Z80_RETCC, Z80_CC_NC, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_POP, Z80_DE, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_NC, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_OUT, Z80_A, Z80_IMMED_INDIRECT, Z80_UNUSED, 0},
	{Z80_CALLCC, Z80_CC_NC, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_PUSH, Z80_DE, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_SUB, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x10},
	{Z80_RETCC, Z80_CC_C, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_EXX, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_C, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_IN, Z80_A, Z80_IMMED_INDIRECT, Z80_UNUSED, 0},
	{Z80_CALLCC, Z80_CC_C, Z80_IMMED, Z80_UNUSED, 0},
	{0, 0, 0, 0, 0},//IX Prefix
	{Z80_SBC, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x18},
	//E
	{Z80_RETCC, Z80_CC_PO, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_POP, Z80_HL, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_PO, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_EX, Z80_HL, Z80_REG_INDIRECT | Z80_DIR, Z80_SP, 0},
	{Z80_CALLCC, Z80_CC_PO, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_PUSH, Z80_HL, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_AND, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x20},
	{Z80_RETCC, Z80_CC_PE, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JP, Z80_UNUSED, Z80_REG_INDIRECT, Z80_HL, 0},
	{Z80_JPCC, Z80_CC_PE, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_EX, Z80_DE, Z80_REG, Z80_HL, 0},
	{Z80_CALLCC, Z80_CC_PE, Z80_IMMED, Z80_UNUSED, 0},
	{0, 0, 0, 0, 0},//EXTD Prefix
	{Z80_XOR, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x28},
	//F
	{Z80_RETCC, Z80_CC_P, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_POP, Z80_AF, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_JPCC, Z80_CC_P, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_DI, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_CALLCC, Z80_CC_P, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_PUSH, Z80_AF, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_OR, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x30},
	{Z80_RETCC, Z80_CC_M, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_LD, Z80_SP, Z80_REG, Z80_HL, 0},
	{Z80_JPCC, Z80_CC_M, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_EI, Z80_UNUSED, Z80_UNUSED, Z80_UNUSED, 0},
	{Z80_CALLCC, Z80_CC_M, Z80_IMMED, Z80_UNUSED, 0},
	{0, 0, 0, 0, 0},//IY Prefix
	{Z80_CP, Z80_A, Z80_IMMED, Z80_UNUSED, 0},
	{Z80_RST, Z80_UNUSED, Z80_IMMED, Z80_UNUSED, 0x38}
};

uint8_t * z80_decode(uint8_t * istream, z80inst * decoded)
{
	if (*istream == 0xCB) {
	} else if (*istream == 0xDD) {
	} else if (*istream == 0xED) {
	} else if (*istream == 0xFD) {
	} else {
		memcpy(decoded, z80_tbl_a + *istream, sizeof(z80inst));
		if (decoded->addr_mode == Z80_IMMED && decoded->op != Z80_RST) {
			decoded->immed = *(++istream);
			if (decoded->reg >= Z80_BC) {
				decoded->immed |= *(++istream) << 8;
			} else if (decoded->immed & 0x80) {
				decoded->immed |= 0xFF00;
			}
		} else if (decoded->addr_mode == Z80_IMMED_INDIRECT) {
			decoded->immed = *(++istream);
			if (decoded->op != Z80_OUT && decoded->op != Z80_IN) {
				decoded->immed |= *(++istream) << 8;
			}
		} else if (decoded->reg == Z80_USE_IMMED) {
			decoded->immed = *(++istream);
		}
	}
	return istream+1;
}

char *z80_mnemonics[Z80_OTDR+1] = {
	"ld",
	"push",
	"pop",
	"ex",
	"exx",
	"ldi",
	"ldir",
	"ldd",
	"lddr",
	"cpi",
	"cpir",
	"cpd",
	"cpdr",
	"add",
	"adc",
	"sub",
	"sbc",
	"and",
	"or",
	"xor",
	"cp",
	"inc",
	"dec",
	"daa",
	"cpl",
	"neg",
	"ccf",
	"scf",
	"nop",
	"halt",
	"di",
	"ei",
	"im",
	"rlc",
	"rl",
	"rrc",
	"rr",
	"sl",
	"rld",
	"rrd",
	"bit",
	"set",
	"res",
	"jp",
	"jp",
	"jr",
	"jr",
	"djnz",
	"call",
	"call",
	"ret",
	"ret",
	"reti",
	"retn",
	"rst",
	"in",
	"ini",
	"inir",
	"indr",
	"out",
	"outi",
	"otir",
	"outd",
	"otdr"
};

char * z80_regs[Z80_USE_IMMED] = {
	"b",
	"c",
	"d",
	"e",
	"h",
	"l",
	"",
	"a",
	"bc",
	"de",
	"hl",
	"sp",
	"af",
};

char * z80_conditions[Z80_CC_M+1] = {
	"nz",
	"z",
	"nc",
	"c",
	"po",
	"pe",
	"p",
	"m"
};

int z80_disasm(z80inst * decoded, char * dst)
{
	int len = sprintf(dst, "%s", z80_mnemonics[decoded->op]);
	if (decoded->addr_mode & Z80_DIR) {
		switch (decoded->addr_mode)
		{
		case Z80_REG:
			len += sprintf(dst+len,  " %s", z80_regs[decoded->ea_reg]);
			break;
		case Z80_REG_INDIRECT:
			len += sprintf(dst+len,  " (%s)", z80_regs[decoded->ea_reg]);
			break;
		case Z80_IMMED:
			len += sprintf(dst+len, " %d", decoded->immed);
			break;
		case Z80_IMMED_INDIRECT:
			len += sprintf(dst+len, " (%d)", decoded->immed);
			break;
		}
		if (decoded->reg != Z80_UNUSED) {
			if (decoded->op == Z80_JRCC || decoded->op == Z80_JPCC || decoded->op == Z80_CALLCC || decoded->op == Z80_RETCC) {
				len += sprintf(dst+len,  "%s %s", decoded->reg == Z80_UNUSED ? "" : "," , z80_conditions[decoded->reg]);
			} else {
				len += sprintf(dst+len,  "%s %s", decoded->reg == Z80_UNUSED ? "" : "," , z80_regs[decoded->reg]);
			}
		}
	} else {
		if (decoded->reg != Z80_UNUSED) {
			if (decoded->op == Z80_JRCC || decoded->op == Z80_JPCC || decoded->op == Z80_CALLCC || decoded->op == Z80_RETCC) {
				len += sprintf(dst+len,  " %s", z80_conditions[decoded->reg]);
			} else {
				len += sprintf(dst+len,  " %s", z80_regs[decoded->reg]);
			}
		}
		switch (decoded->addr_mode)
		{
		case Z80_REG:
			len += sprintf(dst+len,  "%s %s", decoded->reg == Z80_UNUSED ? "" : "," , z80_regs[decoded->ea_reg]);
			break;
		case Z80_REG_INDIRECT:
			len += sprintf(dst+len,  "%s (%s)", decoded->reg == Z80_UNUSED ? "" : "," , z80_regs[decoded->ea_reg]);
			break;
		case Z80_IMMED:
			len += sprintf(dst+len, "%s %d", decoded->reg == Z80_UNUSED ? "" : "," , decoded->immed);
			break;
		case Z80_IMMED_INDIRECT:
			len += sprintf(dst+len, "%s (%d)", decoded->reg == Z80_UNUSED ? "" : "," , decoded->immed);
			break;
		}
	}
	return len;
}

