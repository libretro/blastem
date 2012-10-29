#include "68kinst.h"

void m68k_decode_op(uint16_t op, m68k_op_info *dst)
{
	uint8_t mode = (op >> 3) & 0x7;
	uint8_t reg = op & 0x7;
	dst->addr_mode = mode;
	switch(mode)
	{
	case MODE_REG:
	case MODE_AREG:
		dst->params.regs.pri = reg;
		break;
	case MODE_
	}
}

uint16_t * m68K_decode(uint16_t * istream, m68kinst * decoded)
{
	uint8_t optype = *istream >> 12;
	uint8_t size;
	uint8_t immed;
	switch(optype)
	{
	case BIT_MOVEP_IMMED:
		//TODO: Implement me
		break;
	case MOVE_BYTE:
	case MOVE_LONG:
	case MOVE_WORD:
		decoded->op = M68K_MOVE;
		decoded->extra.size = optype == MOVE_BYTE ? OPSIZE_BYTE : (optype == MOVE_WORD ? OPSIZE_WORD : OPSIZE_LONG);
		m68k_decode_op(*istream, &(decoded->src));
		m68k_decode_op(((*istream >> 9) & 0x7) | , &(decoded->dst));
		break;
	case MISC:
		//TODO: Implement me
		break;
	case QUICK_ARITH_LOOP:
		size = (*istream >> 6) & 3;
		if (size == 0x3) {
			//DBcc, TRAPcc or Scc
			decoded->extra.cond = (*istream >> 0x8) & 0xF;
			switch ((*istream >> 3) & 0x7)
			{
			case 1: //DBcc
				decoded->op = M68K_DBCC;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.regs.pri = *istream & 0x7;
				break;
			case 7: //TRAPcc
				decoded->op = M68K_TRAPCC;
				decoded->src.addr_mode = MODE_PC_INDIRECT_ABS_IMMED;
				decoded->src.regs.pri = MODE_IMMEDIATE;
				//TODO: Figure out what to do with OPMODE and optional extention words
				break;
			default: //Scc
				decoded->op = M68K_SCC;
				M68k_decode_op(*istream, &(decoded->dst));
				break;
			}
		} else {
			//ADDQ, SUBQ
			decoded->variant = VAR_QUICK;
			decoded->extra.size = size;
			decoded->src.addr_mode = MODE_PC_INDIRECT_ABS_IMMED;
			decoded->src.regs.pri = MODE_IMMEDIATE;
			immed = (*istream >> 9) & 0x7
			if (!immed) {
				immed = 8;
			}
			switch (size)
			{
			case OPSIZE_BYTE;
				decoded->src.params.u8 = immed;
				break;
			case OPSIZE_WORD:
				decoded->src.params.u16 = immed;
				break;
			case OPSIZE_LONG:
				decoded->src.params.u38 = immed;
				break;
			}
			if (*istream & 0x10) {
				decoded->op = M68K_SUB;
			} else {
				decoded->op = M68K_ADD;
			}
		}
		break;
	}
}
