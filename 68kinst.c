#include "68kinst.h"
#include <string.h>
#include <stdio.h>

uint32_t sign_extend16(uint32_t val)
{
	return (val & 0x8000) ? val | 0xFFFF0000 : val;
}

uint32_t sign_extend8(uint32_t val)
{
	return (val & 0x80) ? val | 0xFFFFFF00 : val;
}

uint16_t *m68k_decode_op_ex(uint16_t *cur, uint8_t mode, uint8_t reg, uint8_t size, m68k_op_info *dst)
{
	uint16_t ext;
	dst->addr_mode = mode;
	switch(mode)
	{
	case MODE_REG:
	case MODE_AREG:
	case MODE_AREG_INDIRECT:
	case MODE_AREG_POSTINC:
	case MODE_AREG_PREDEC:
		dst->params.regs.pri = reg;
		break;
	case MODE_AREG_DISPLACE:
		ext = *(++cur);
		dst->params.regs.pri = reg;
		dst->params.regs.displacement = sign_extend16(ext);
		break;
	case MODE_AREG_INDEX_MEM:
		//TODO: implement me
		break;
	case MODE_PC_INDIRECT_ABS_IMMED:
		switch(reg)
		{
		case 0:
			dst->addr_mode = MODE_ABSOLUTE_SHORT;
			ext = *(++cur);
			dst->params.u32 = sign_extend16(ext);
			break;
		case 1:
			dst->addr_mode = MODE_ABSOLUTE;
			ext = *(++cur);
			dst->params.u32 = ext << 16 | *(++cur);
			break;
		case 2:
			dst->addr_mode = MODE_PC_DISPLACE;
			ext = *(++cur);
			dst->params.regs.displacement = sign_extend16(ext);
			break;
		case 4:
			dst->addr_mode = MODE_IMMEDIATE;
			ext = *(++cur);
			switch (size)
			{
			case OPSIZE_BYTE:
				dst->params.u8 = ext;
				break;
			case OPSIZE_WORD:
				dst->params.u16 = ext;
				break;
			case OPSIZE_LONG:
				dst->params.u32 = ext << 16 | *(++cur);
				break;
			}
			break;
		//TODO: implement the rest of these
		}
		break;
	}
	return cur;
}

uint16_t *m68k_decode_op(uint16_t *cur, uint8_t size, m68k_op_info *dst)
{
	uint8_t mode = (*cur >> 3) & 0x7;
	uint8_t reg = *cur & 0x7;
	return m68k_decode_op_ex(cur, mode, reg, size, dst);
}

void m68k_decode_cond(uint16_t op, m68kinst * decoded)
{
	decoded->extra.cond = (op >> 0x8) & 0xF;
}

uint8_t m68K_reg_quick_field(uint16_t op)
{
	return (op >> 9) & 0x7;
}

uint16_t * m68K_decode(uint16_t * istream, m68kinst * decoded)
{
	uint8_t optype = *istream >> 12;
	uint8_t size;
	uint32_t immed;
	decoded->op = M68K_INVALID;
	decoded->src.addr_mode = decoded->dst.addr_mode = MODE_UNUSED;
	decoded->variant = VAR_NORMAL;
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
		istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		istream = m68k_decode_op_ex(istream, (*istream >> 6) & 0x7, m68K_reg_quick_field(*istream), decoded->extra.size, &(decoded->dst));
		break;
	case MISC:
		
		if ((*istream & 0x1C0) == 0x1C0) {
			decoded->op = M68K_LEA;
			decoded->extra.size = OPSIZE_LONG;
			decoded->dst.addr_mode = MODE_AREG;
			decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		} else {
			if (*istream & 0x100) {
				decoded->op = M68K_CHK;
				if ((*istream & 0x180) == 0x180) {
					decoded->extra.size = OPSIZE_WORD;
				} else {
					//only on M68020+
					decoded->extra.size = OPSIZE_LONG;
				}
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.addr_mode = m68K_reg_quick_field(*istream);
			} else {
				optype = (*istream >> 9) & 0x7;
				switch(optype)
				{
				case 0:
					//Move from SR or NEGX
					break;
				case 1:
					//MOVE from CCR or CLR
					break;
				case 2:
					//MOVE to CCR or NEG
					break;
				case 3:
					//MOVE to SR or NOT
					break;
				case 4:
					//EXT, EXTB, LINK.l, NBCD, SWAP, BKPT, PEA, MOVEM
					break;
				case 5:
					//BGND, ILLEGAL, TAS, TST
					optype = *istream & 0xFF;
					if (optype == 0xFA) {
						//BGND - CPU32 only
					} else if (optype == 0xFC) {
						decoded->op = M68K_ILLEGAL;
					} else {
						size = (*istream & 0xC0) >> 6;
						if (size == OPSIZE_INVALID) {
							decoded->op = M68K_TAS;
						} else {
							decoded->op = M68K_TST;
							decoded->extra.size = size;
							istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
						}
					}
					break;	
				case 6:
					//MULU, MULS, DIVU, DIVUL, DIVS, DIVSL, MOVEM
					break;
				case 7:
					//TRAP, LINK.w, UNLNK, MOVE USP, RESET, NOP, STOP, RTE, RTD, RTS, TRAPV, RTR, MOVEC, JSR, JMP
					if (*istream & 0x80) {
						//JSR, JMP
					} else {
						//it would appear bit 6 needs to be set for it to be a valid instruction here
						switch((*istream >> 3) & 0x7)
						{
						case 0:
						case 1:
							//TRAP
							break;
						case 2:
							//LINK.w
							break;
						case 3:
							//UNLNK
							break;
						case 4:
						case 5:
							//MOVE USP
							break;
						case 6:
							switch(*istream & 0x7)
							{
							case 0:
								decoded->op = M68K_RESET;
								break;
							case 1:
								decoded->op = M68K_NOP;
								break;
							case 2:
								decoded->op = M68K_STOP;
								decoded->extra.size = OPSIZE_WORD;
								decoded->src.addr_mode = MODE_IMMEDIATE;
								decoded->src.params.u16 =*(++istream);
								break;
							case 3:
								decoded->op = M68K_RTE;
								break;
							case 4:
#ifdef M68010
								decoded->op = M68K_RTD;
								decoded->extra.size = OPSIZE_WORD;
								decoded->src.addr_mode = MODE_IMMEDIATE;
								decoded->src.params.u16 =*(++istream);
#endif
								break;
							case 5:
								decoded->op = M68K_RTS;
								break;
							case 6:
								decoded->op = M68K_TRAPV;
								break;
							case 7:
								decoded->op = M68K_RTR;
								break;
							}
							break;
						case 7:
							//MOVEC
							break;
						}
					}
					break;
				}
			}
		}
		break;
	case QUICK_ARITH_LOOP:
		size = (*istream >> 6) & 3;
		if (size == 0x3) {
			//DBcc, TRAPcc or Scc
			m68k_decode_cond(*istream, decoded);
			switch ((*istream >> 3) & 0x7)
			{
			case 1: //DBcc
				decoded->op = M68K_DBCC;
				decoded->src.addr_mode = MODE_IMMEDIATE;
				decoded->src.params.u16 = *(++istream);
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = *istream & 0x7;
				break;
			case 7: //TRAPcc
#ifdef M68020
				decoded->op = M68K_TRAPCC;
				decoded->src.addr_mode = MODE_IMMEDIATE;
				//TODO: Figure out what to do with OPMODE and optional extention words
#endif
				break;
			default: //Scc
				decoded->op = M68K_SCC;
				istream = m68k_decode_op(istream, OPSIZE_BYTE, &(decoded->dst));
				break;
			}
		} else {
			//ADDQ, SUBQ
			decoded->variant = VAR_QUICK;
			decoded->extra.size = size;
			decoded->src.addr_mode = MODE_IMMEDIATE;
			istream = m68k_decode_op(istream, size, &(decoded->dst));
			immed = m68K_reg_quick_field(*istream);
			if (!immed) {
				immed = 8;
			}
			switch (size)
			{
			case OPSIZE_BYTE:
				decoded->src.params.u8 = immed;
				break;
			case OPSIZE_WORD:
				decoded->src.params.u16 = immed;
				break;
			case OPSIZE_LONG:
				decoded->src.params.u32 = immed;
				break;
			}
			if (*istream & 0x100) {
				decoded->op = M68K_SUB;
			} else {
				decoded->op = M68K_ADD;
			}
		}
		break;
	case BRANCH:
		m68k_decode_cond(*istream, decoded);
		decoded->op = decoded->extra.cond == COND_FALSE ? M68K_BSR : M68K_BCC;
		decoded->src.addr_mode = MODE_IMMEDIATE;
		immed = *istream & 0xFF;
		if (immed == 0) {
			decoded->variant = VAR_WORD;
			immed = *(++istream);
			immed = sign_extend16(immed);
		} else if (immed == 0xFF) {
			decoded->variant = VAR_LONG;
			immed = *(++istream) << 16;
			immed |= *(++istream);
		} else {
			decoded->variant = VAR_BYTE;
			immed = sign_extend8(immed);
		}
		decoded->src.params.u32 = immed;
		break;
	case MOVEQ:
		decoded->op = M68K_MOVE;
		decoded->variant = VAR_QUICK;
		decoded->src.addr_mode = MODE_IMMEDIATE;
		decoded->src.params.u32 = sign_extend8(*istream & 0xFF);
		decoded->dst.addr_mode = MODE_REG;
		decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
		immed = *istream & 0xFF;
		break;
	case OR_DIV_SBCD:
		//TODO: Implement me
		break;
	case SUB_SUBX:
		size = *istream >> 6 & 0x3;
		decoded->op = M68K_SUB;
		if (*istream & 0x100) {
			//<ea> destination, SUBA.l or SUBX
			if (*istream & 0x6) {
				if (size == OPSIZE_INVALID) {
					//SUBA.l
					decoded->extra.size = OPSIZE_LONG;
					decoded->dst.addr_mode = MODE_AREG;
					istream = m68k_decode_op(istream, OPSIZE_LONG, &(decoded->src));
				} else {
					decoded->extra.size = size;
					decoded->src.addr_mode = MODE_REG;
					istream = m68k_decode_op(istream, size, &(decoded->dst));
				}
			} else {
				//SUBX
				decoded->op = M68K_SUBX;
				decoded->extra.size = size;
				istream = m68k_decode_op(istream, size, &(decoded->src));
				decoded->dst.addr_mode = decoded->src.addr_mode;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			}
		} else {
			if (size == OPSIZE_INVALID) {
				//SUBA.w
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_AREG;
			} else {
				decoded->extra.size = size;
				decoded->dst.addr_mode = MODE_REG;
			}
			decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		}
		break;
	case RESERVED:
		//TODO: implement me
		break;
	case CMP_XOR:
		size = *istream >> 6 & 0x3;
		decoded->op = M68K_CMP;
		if (*istream & 0x100) {
			//CMPM or EOR
			istream = m68k_decode_op(istream, size, &(decoded->dst));
			if (decoded->src.addr_mode == MODE_AREG) {
				//CMPM
				decoded->src.addr_mode = decoded->dst.addr_mode = MODE_AREG_POSTINC;
				decoded->src.params.regs.pri = decoded->dst.params.regs.pri;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			} else {
				//EOR
				decoded->op = M68K_EOR;
				decoded->extra.size = size;
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = m68K_reg_quick_field(*istream);
			}
		} else {
			//CMP
			decoded->extra.size = size;
			decoded->dst.addr_mode = MODE_REG;
			decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, size, &(decoded->src));
		}
		break;
	case AND_MUL_ABCD_EXG:
		//page 575 for summary
		//EXG opmodes:
		//01000 -data regs
		//01001 -addr regs
		//10001 -one of each
		//AND opmodes:
		//operand order bit + 2 size bits (00 - 10)
		//no address register direct addressing
		//data register direct not allowed when <ea> is the source (operand order bit of 1)
		if (*istream & 0x100) {
			if ((*istream & 0xC0) == 0xC0) {
				decoded->op = M68K_MULS;
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
			} else if(!(*istream & 0xF0)) {
				decoded->op = M68K_ABCD;
				decoded->extra.size = OPSIZE_BYTE;
				decoded->src.params.regs.pri = *istream & 0x7;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
				decoded->dst.addr_mode = decoded->src.addr_mode = (*istream & 8) ? MODE_AREG_PREDEC : MODE_REG;
			} else if(!(*istream & 0x30)) {
				decoded->op = M68K_EXG;
				decoded->extra.size = OPSIZE_LONG;
				decoded->src.params.regs.pri = m68K_reg_quick_field(*istream);
				decoded->dst.params.regs.pri = *istream & 0x7;
				if (*istream & 0x8) {
					if (*istream & 0x80) {
						decoded->src.addr_mode = MODE_REG;
						decoded->dst.addr_mode = MODE_AREG;
					} else {
						decoded->src.addr_mode = decoded->dst.addr_mode = MODE_AREG;
					}
				} else {
					decoded->src.addr_mode = decoded->dst.addr_mode = MODE_REG;
				}
			} else {
				decoded->op = M68K_AND;
				decoded->extra.size = (*istream >> 6);
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
			}
		} else {
			if ((*istream & 0xC0) == 0xC0) {
				decoded->op = M68K_MULU;
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
			} else {
				decoded->op = M68K_AND;
				decoded->extra.size = (*istream >> 6);
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = m68K_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->dst));
			}
		}
		break;
	case ADD_ADDX:
		size = *istream >> 6 & 0x3;
		decoded->op = M68K_ADD;
		if (*istream & 0x100) {
			//<ea> destination, ADDA.l or ADDX
			if (*istream & 0x6) {
				if (size == OPSIZE_INVALID) {
					//ADDA.l
					decoded->extra.size = OPSIZE_LONG;
					decoded->dst.addr_mode = MODE_AREG;
					istream = m68k_decode_op(istream, OPSIZE_LONG, &(decoded->src));
				} else {
					decoded->extra.size = size;
					decoded->src.addr_mode = MODE_REG;
					istream = m68k_decode_op(istream, size, &(decoded->dst));
				}
			} else {
				//ADDX
				decoded->op = M68K_ADDX;
				//FIXME: Size is not technically correct
				decoded->extra.size = size;
				istream = m68k_decode_op(istream, size, &(decoded->src));
				decoded->dst.addr_mode = decoded->src.addr_mode;
				decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			}
		} else {
			if (size == OPSIZE_INVALID) {
				//ADDA.w
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_AREG;
			} else {
				decoded->extra.size = size;
				decoded->dst.addr_mode = MODE_REG;
			}
			decoded->dst.params.regs.pri = m68K_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		}
		break;
	case SHIFT_ROTATE:
		//TODO: Implement me
		break;
	case COPROC:
		//TODO: Implement me
		break;
	}
	return istream+1;
}

char * mnemonics[] = {
	"abcd",
	"add",
	"addx",
	"and",
	"andi_ccr",
	"andi_sr",
	"asl",
	"asr",
	"bcc",
	"bchg",
	"bclr",
	"bset",
	"bsr",
	"btst",
	"chk",
	"clr",
	"cmp",
	"dbcc",
	"divs",
	"divu",
	"eor",
	"eori_ccr",
	"eori_sr",
	"exg",
	"ext",
	"illegal",
	"jmp",
	"jsr",
	"lea",
	"link",
	"lsl",
	"lsr",
	"move",
	"move_ccr",
	"move_from_sr",
	"move_sr",
	"move_usp",
	"movem",
	"movep",
	"muls",
	"mulu",
	"nbcd",
	"neg",
	"negx",
	"nop",
	"not",
	"or",
	"ori_ccr",
	"ori_sr",
	"pea",
	"reset",
	"rol",
	"ror",
	"roxl",
	"roxr",
	"rte",
	"rtr",
	"rts",
	"sbcd",
	"scc",
	"stop",
	"sub",
	"subx",
	"swap",
	"tas",
	"trap",
	"trapv",
	"tst",
	"unlnk",
	"invalid"
};

char * cond_mnem[] = {
	"ra",
	"f",
	"hi",
	"ls",
	"cc",
	"cs",
	"ne",
	"eq",
	"vc",
	"vs",
	"pl",
	"mi",
	"ge",
	"lt",
	"gt",
	"le"
};

int m68K_disasm_op(m68k_op_info *decoded, uint8_t size, char *dst, int need_comma)
{
	char * c = need_comma ? "," : "";
	switch(decoded->addr_mode)
	{
	case MODE_REG:
		return sprintf(dst, "%s d%d", c, decoded->params.regs.pri);
	case MODE_AREG:
		return sprintf(dst, "%s a%d", c, decoded->params.regs.pri);
	case MODE_AREG_INDIRECT:
		return sprintf(dst, "%s (a%d)", c, decoded->params.regs.pri);
	case MODE_AREG_POSTINC:
		return sprintf(dst, "%s (a%d)+", c, decoded->params.regs.pri);
	case MODE_AREG_PREDEC:
		return sprintf(dst, "%s -(a%d)", c, decoded->params.regs.pri);
	case MODE_IMMEDIATE:
		return sprintf(dst, "%s #%d", c, size == OPSIZE_LONG ? decoded->params.u32 : (size == OPSIZE_WORD ? decoded->params.u16 : decoded->params.u8));
	default:
		return 0;
	}
}

int m68k_disasm(m68kinst * decoded, char * dst)
{
	int ret,op1len;
	uint8_t size;
	if (decoded->op == M68K_BCC || decoded->op == M68K_DBCC || decoded->op == M68K_SCC) {
		ret = strlen(mnemonics[decoded->op]) - 2;
		memcpy(dst, mnemonics[decoded->op], ret);
		dst[ret] = 0;
		strcat(dst, cond_mnem[decoded->extra.cond]);
		ret = strlen(dst);
		size = decoded->op = M68K_BCC ? OPSIZE_LONG : OPSIZE_WORD;
	} else if (decoded->op == M68K_BSR) {
		size = OPSIZE_LONG;
		ret = sprintf(dst, "bsr%s", decoded->variant == VAR_BYTE ? ".s" : "");
	} else {
		size = decoded->extra.size;
		ret = sprintf(dst, "%s%s.%c", 
				mnemonics[decoded->op], 
				decoded->variant == VAR_QUICK ? "q" : "", 
				decoded->extra.size == OPSIZE_BYTE ? 'b' : (size == OPSIZE_WORD ? 'w' : 'l'));
	}
	op1len = m68K_disasm_op(&(decoded->src), size, dst + ret, 0);
	ret += op1len;
	ret += m68K_disasm_op(&(decoded->dst), size, dst + ret, op1len);
	return ret;
}

