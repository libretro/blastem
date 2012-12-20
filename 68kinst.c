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
			dst->params.immed = sign_extend16(ext);
			break;
		case 1:
			dst->addr_mode = MODE_ABSOLUTE;
			ext = *(++cur);
			dst->params.immed = ext << 16 | *(++cur);
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
				dst->params.immed = ext & 0xFF;
				break;
			case OPSIZE_WORD:
				dst->params.immed = ext;
				break;
			case OPSIZE_LONG:
				dst->params.immed = ext << 16 | *(++cur);
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

uint8_t m68k_reg_quick_field(uint16_t op)
{
	return (op >> 9) & 0x7;
}

uint16_t * m68k_decode(uint16_t * istream, m68kinst * decoded, uint32_t address)
{
	uint8_t optype = *istream >> 12;
	uint8_t size;
	uint8_t reg;
	uint8_t opmode;
	uint32_t immed;
	decoded->op = M68K_INVALID;
	decoded->src.addr_mode = decoded->dst.addr_mode = MODE_UNUSED;
	decoded->variant = VAR_NORMAL;
	decoded->address = address;
	switch(optype)
	{
	case BIT_MOVEP_IMMED:
		if (*istream & 0x100) {
			//BTST, BCHG, BCLR, BSET
			switch ((*istream >> 6) & 0x3)
			{
			case 0:
				decoded->op = M68K_BTST;
				break;
			case 1:
				decoded->op = M68K_BCHG;
				break;
			case 2:
				decoded->op = M68K_BCLR;
				break;
			case 3:
				decoded->op = M68K_BSET;
				break;
			}
			decoded->src.addr_mode = MODE_REG;
			decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
			decoded->extra.size = OPSIZE_BYTE;
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->dst));
			if (decoded->dst.addr_mode == MODE_REG) {
				decoded->extra.size = OPSIZE_LONG;
			}
		} else if ((*istream & 0xF00) == 0x800) {
			//BTST, BCHG, BCLR, BSET
			switch ((*istream >> 6) & 0x3)
			{
			case 0:
				decoded->op = M68K_BTST;
				break;
			case 1:
				decoded->op = M68K_BCHG;
				break;
			case 2:
				decoded->op = M68K_BCLR;
				break;
			case 3:
				decoded->op = M68K_BSET;
				break;
			}
			opmode = (*istream >> 3) & 0x7;
			reg = *istream & 0x7;
			decoded->src.addr_mode = MODE_IMMEDIATE_WORD;
			decoded->src.params.immed = *(++istream) & 0xFF;
			decoded->extra.size = OPSIZE_BYTE;
			istream = m68k_decode_op_ex(istream, opmode, reg, decoded->extra.size, &(decoded->dst));
			if (decoded->dst.addr_mode == MODE_REG) {
				decoded->extra.size = OPSIZE_LONG;
			}
		} else if ((*istream & 0xC0) == 0xC0) {
#ifdef M68020
			//CMP2, CHK2, CAS, CAS2, RTM, CALLM
#endif
		} else {
			switch ((*istream >> 9) & 0x7)
			{
			case 0:
				if ((*istream & 0xFF) == 0x3C) {
					decoded->op = M68K_ORI_CCR;
					decoded->extra.size = OPSIZE_BYTE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream) & 0xFF;
				} else if((*istream & 0xFF) == 0x7C) {
					decoded->op = M68K_ORI_SR;
					decoded->extra.size = OPSIZE_WORD;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream);
				} else {
					decoded->op = M68K_OR;
					decoded->variant = VAR_IMMEDIATE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->extra.size = size = (*istream >> 6) & 3;
					reg = *istream & 0x7;
					opmode = (*istream >> 3) & 0x7;
					switch (size)
					{
					case OPSIZE_BYTE:
						decoded->src.params.immed = *(++istream) & 0xFF;
						break;
					case OPSIZE_WORD:
						decoded->src.params.immed = *(++istream);
						break;
					case OPSIZE_LONG:
						immed = *(++istream);
						decoded->src.params.immed = immed << 16 | *(++istream);
						break;
					}
					istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				}
				break;
			case 1:
				//ANDI, ANDI to CCR, ANDI to SR
				if ((*istream & 0xFF) == 0x3C) {
					decoded->op = M68K_ANDI_CCR;
					decoded->extra.size = OPSIZE_BYTE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream) & 0xFF;
				} else if((*istream & 0xFF) == 0x7C) {
					decoded->op = M68K_ANDI_SR;
					decoded->extra.size = OPSIZE_WORD;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream);
				} else {
					decoded->op = M68K_AND;
					decoded->variant = VAR_IMMEDIATE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->extra.size = size = (*istream >> 6) & 3;
					reg = *istream & 0x7;
					opmode = (*istream >> 3) & 0x7;
					switch (size)
					{
					case OPSIZE_BYTE:
						decoded->src.params.immed = *(++istream) & 0xFF;
						break;
					case OPSIZE_WORD:
						decoded->src.params.immed = *(++istream);
						break;
					case OPSIZE_LONG:
						immed = *(++istream);
						decoded->src.params.immed = immed << 16 | *(++istream);
						break;
					}
					istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				}
				break;
			case 2:
				decoded->op = M68K_SUB;
				decoded->variant = VAR_IMMEDIATE;
				decoded->src.addr_mode = MODE_IMMEDIATE;
				decoded->extra.size = size = (*istream >> 6) & 3;
				reg = *istream & 0x7;
				opmode = (*istream >> 3) & 0x7;
				switch (size)
				{
				case OPSIZE_BYTE:
					decoded->src.params.immed = *(++istream) & 0xFF;
					break;
				case OPSIZE_WORD:
					decoded->src.params.immed = *(++istream);
					break;
				case OPSIZE_LONG:
					immed = *(++istream);
					decoded->src.params.immed = immed << 16 | *(++istream);
					break;
				}
				istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				break;
			case 3:
				decoded->op = M68K_ADD;
				decoded->variant = VAR_IMMEDIATE;
				decoded->src.addr_mode = MODE_IMMEDIATE;
				decoded->extra.size = size = (*istream >> 6) & 3;
				reg = *istream & 0x7;
				opmode = (*istream >> 3) & 0x7;
				switch (size)
				{
				case OPSIZE_BYTE:
					decoded->src.params.immed = *(++istream) & 0xFF;
					break;
				case OPSIZE_WORD:
					decoded->src.params.immed = *(++istream);
					break;
				case OPSIZE_LONG:
					immed = *(++istream);
					decoded->src.params.immed = immed << 16 | *(++istream);
					break;
				}
				istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				break;
			case 4:
				//BTST, BCHG, BCLR, BSET
				switch ((*istream >> 6) & 0x3)
				{
				case 0:
					decoded->op = M68K_BTST;
					break;
				case 1:
					decoded->op = M68K_BCHG;
					break;
				case 2:
					decoded->op = M68K_BCLR;
					break;
				case 3:
					decoded->op = M68K_BSET;
					break;
				}
				decoded->src.addr_mode = MODE_IMMEDIATE;
				decoded->src.params.immed = *(++istream) & 0xFF;
				istream = m68k_decode_op(istream, OPSIZE_BYTE, &(decoded->dst));
				break;
			case 5:
				//EORI, EORI to CCR, EORI to SR
				if ((*istream & 0xFF) == 0x3C) {
					decoded->op = M68K_EORI_CCR;
					decoded->extra.size = OPSIZE_BYTE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream) & 0xFF;
				} else if((*istream & 0xFF) == 0x7C) {
					decoded->op = M68K_EORI_SR;
					decoded->extra.size = OPSIZE_WORD;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->src.params.immed = *(++istream);
				} else {
					decoded->op = M68K_EOR;
					decoded->variant = VAR_IMMEDIATE;
					decoded->src.addr_mode = MODE_IMMEDIATE;
					decoded->extra.size = size = (*istream >> 6) & 3;
					reg = *istream & 0x7;
					opmode = (*istream >> 3) & 0x7;
					switch (size)
					{
					case OPSIZE_BYTE:
						decoded->src.params.immed = *(++istream) & 0xFF;
						break;
					case OPSIZE_WORD:
						decoded->src.params.immed = *(++istream);
						break;
					case OPSIZE_LONG:
						immed = *(++istream);
						decoded->src.params.immed = immed << 16 | *(++istream);
						break;
					}
					istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				}
				break;
			case 6:
				decoded->op = M68K_CMP;
				decoded->variant = VAR_IMMEDIATE;
				decoded->extra.size = (*istream >> 6) & 0x3;
				decoded->src.addr_mode = MODE_IMMEDIATE;
				reg = *istream & 0x7;
				opmode = (*istream >> 3) & 0x7;
				switch (decoded->extra.size)
				{
				case OPSIZE_BYTE:
					decoded->src.params.immed = *(++istream) & 0xFF;
					break;
				case OPSIZE_WORD:
					decoded->src.params.immed = *(++istream);
					break;
				case OPSIZE_LONG:
					immed = *(++istream);
					decoded->src.params.immed = (immed << 16) | *(++istream);
					break;
				}
				istream = m68k_decode_op_ex(istream, opmode, reg, size, &(decoded->dst));
				break;
			case 7:
				//MOVEP
				decoded->op = M68K_MOVEP;
				decoded->extra.size = *istream & 0x40 ? OPSIZE_LONG : OPSIZE_WORD;
				if (*istream & 0x80) {
					//memory dest
					decoded->src.addr_mode = MODE_REG;
					decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
					decoded->dst.addr_mode = MODE_AREG_DISPLACE;
					decoded->dst.params.regs.pri = *istream & 0x7;
				} else {
					//memory source
					decoded->dst.addr_mode = MODE_REG;
					decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
					decoded->src.addr_mode = MODE_AREG_DISPLACE;
					decoded->src.params.regs.pri = *istream & 0x7;
				}
				immed = *(++istream);
				
				break;
			}
		}
		break;
	case MOVE_BYTE:
	case MOVE_LONG:
	case MOVE_WORD:
		decoded->op = M68K_MOVE;
		decoded->extra.size = optype == MOVE_BYTE ? OPSIZE_BYTE : (optype == MOVE_WORD ? OPSIZE_WORD : OPSIZE_LONG);
		opmode = (*istream >> 6) & 0x7;
		reg = m68k_reg_quick_field(*istream);
		istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		istream = m68k_decode_op_ex(istream, opmode, reg, decoded->extra.size, &(decoded->dst));
		break;
	case MISC:
		
		if ((*istream & 0x1C0) == 0x1C0) {
			decoded->op = M68K_LEA;
			decoded->extra.size = OPSIZE_LONG;
			decoded->dst.addr_mode = MODE_AREG;
			decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		} else {
			if (*istream & 0x100) {
				decoded->op = M68K_CHK;
				if ((*istream & 0x180) == 0x180) {
					decoded->extra.size = OPSIZE_WORD;
				} else {
					//only on M68020+
#ifdef M68020
					decoded->extra.size = OPSIZE_LONG;
#else
					decoded->op = M68K_INVALID;
					break;
#endif
				}
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.addr_mode = m68k_reg_quick_field(*istream);
			} else {
				opmode = (*istream >> 3) & 0x7;
				if ((*istream & 0xB80) == 0x880 && opmode != MODE_REG && opmode != MODE_AREG) {
					//TODO: Check for invalid modes that are dependent on direction
					decoded->op = M68K_MOVEM;
					decoded->extra.size = *istream & 0x40 ? OPSIZE_LONG : OPSIZE_WORD;
					reg = *istream & 0x7;
					immed = *(++istream);
					if(*istream & 0x400) {
						decoded->dst.addr_mode = MODE_REG;
						decoded->dst.params.immed = immed;
						istream = m68k_decode_op_ex(istream, opmode, reg, decoded->extra.size, &(decoded->src));
					} else {
						decoded->src.addr_mode = MODE_REG;
						decoded->src.params.immed = immed;
						istream = m68k_decode_op_ex(istream, opmode, reg, decoded->extra.size, &(decoded->dst));
					}
				} else {
					optype = (*istream >> 9) & 0x7;
					size = (*istream >> 6) & 0x3;
					switch(optype)
					{
					case 0:
						//Move from SR or NEGX
						if (size == OPSIZE_INVALID) {
							decoded->op = M68K_MOVE_FROM_SR;
							size = OPSIZE_WORD;
						} else {
							decoded->op = M68K_NEGX;
						}
						decoded->extra.size = size;
						istream= m68k_decode_op(istream, size, &(decoded->dst));
						break;
					case 1:
						//MOVE from CCR or CLR
						if (size == OPSIZE_INVALID) {
#ifdef M68010
							decoded->op = M68K_MOVE_FROM_CCR;
							size = OPSIZE_WORD;
#else
							return istream+1;
#endif
						} else {
							decoded->op = M68K_CLR;
						}
						decoded->extra.size = size;
						istream= m68k_decode_op(istream, size, &(decoded->dst));
						break;
					case 2:
						//MOVE to CCR or NEG
						if (size == OPSIZE_INVALID) {
							decoded->op = M68K_MOVE_CCR;
							size = OPSIZE_WORD;
							istream= m68k_decode_op(istream, size, &(decoded->src));
						} else {
							decoded->op = M68K_NEG;
							istream= m68k_decode_op(istream, size, &(decoded->dst));
						}
						decoded->extra.size = size;
						break;
					case 3:
						//MOVE to SR or NOT
						if (size == OPSIZE_INVALID) {
							decoded->op = M68K_MOVE_SR;
							size = OPSIZE_WORD;
							istream= m68k_decode_op(istream, size, &(decoded->src));
						} else {
							decoded->op = M68K_NOT;
							istream= m68k_decode_op(istream, size, &(decoded->dst));
						}
						decoded->extra.size = size;
						break;
					case 4:
						//EXT, EXTB, LINK.l, NBCD, SWAP, BKPT, PEA
						switch((*istream >> 3) & 0x3F)
						{
						case 1:
#ifdef M68020
							decoded->op = M68K_LINK;
							decoded->extra.size = OPSIZE_LONG;
							reg = *istream & 0x7;
							immed = *(++istream) << 16;
							immed |= *(++istream);
#endif
							break;
						case 8:
							decoded->op = M68K_SWAP;
							decoded->src.addr_mode = MODE_REG;
							decoded->src.params.regs.pri = *istream & 0x7;
							decoded->extra.size = OPSIZE_WORD;
							break;
						case 9:
#ifdef M68010
							decoded->op = M68K_BKPT;
							decoded->src.addr_mode = MODE_IMMEDIATE;
							decoded->extra.size = OPSIZE_UNSIZED;
							decoded->src.params.immed = *istream & 0x7;
#endif
							break;
						case 0x10:
							decoded->op = M68K_EXT;
							decoded->src.addr_mode = MODE_REG;
							decoded->src.params.regs.pri = *istream & 0x7;
							decoded->extra.size = OPSIZE_WORD;
							break;
						case 0x18:
							decoded->op = M68K_EXT;
							decoded->src.addr_mode = MODE_REG;
							decoded->src.params.regs.pri = *istream & 0x7;
							decoded->extra.size = OPSIZE_LONG;
							break;
						case 0x38:
#ifdef M68020
#endif
							break;
						default:
							if (!(*istream & 0x1C0)) {
								decoded->op = M68K_NBCD;
								decoded->extra.size = OPSIZE_BYTE;
								istream = m68k_decode_op(istream, OPSIZE_BYTE, &(decoded->dst));
							} else if((*istream & 0x1C0) == 0x40) {
								decoded->op = M68K_PEA;
								decoded->extra.size = OPSIZE_LONG;
								istream = m68k_decode_op(istream, OPSIZE_LONG, &(decoded->dst));
							}
						}
						break;
					case 5:
						//BGND, ILLEGAL, TAS, TST
						optype = *istream & 0xFF;
						if (optype == 0xFA) {
							//BGND - CPU32 only
						} else if (optype == 0xFC) {
							decoded->op = M68K_ILLEGAL;
							decoded->extra.size = OPSIZE_UNSIZED;
						} else {
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
						//MULU, MULS, DIVU, DIVUL, DIVS, DIVSL
#ifdef M68020
						//TODO: Implement these for 68020+ support
#endif
						break;
					case 7:
						//TRAP, LINK.w, UNLNK, MOVE USP, RESET, NOP, STOP, RTE, RTD, RTS, TRAPV, RTR, MOVEC, JSR, JMP
						if (*istream & 0x80) {
							//JSR, JMP
							if (*istream & 0x40) {
								decoded->op = M68K_JMP;
							} else {
								decoded->op = M68K_JSR;
							}
							decoded->extra.size = OPSIZE_UNSIZED;
							istream = m68k_decode_op(istream, OPSIZE_UNSIZED, &(decoded->src));
						} else {
							//it would appear bit 6 needs to be set for it to be a valid instruction here
							switch((*istream >> 3) & 0x7)
							{
							case 0:
							case 1:
								//TRAP
								decoded->op = M68K_TRAP;
								decoded->extra.size = OPSIZE_UNSIZED;
								decoded->src.addr_mode = MODE_IMMEDIATE;
								decoded->src.params.immed = *istream & 0xF;
								break;
							case 2:
								//LINK.w
								decoded->op = M68K_LINK;
								decoded->extra.size = OPSIZE_WORD;
								decoded->src.addr_mode = MODE_AREG;
								decoded->src.params.regs.pri = *istream & 0x7;
								decoded->dst.addr_mode = MODE_IMMEDIATE;
								decoded->dst.params.immed = *(++istream);
								break;
							case 3:
								//UNLK
								decoded->op = M68K_UNLK;
								decoded->extra.size = OPSIZE_UNSIZED;
								decoded->dst.addr_mode = MODE_AREG;
								decoded->dst.params.regs.pri = *istream & 0x7;
								break;
							case 4:
							case 5:
								//MOVE USP
								decoded->op = M68K_MOVE_USP;
								if (*istream & 0x8) {
									decoded->dst.addr_mode = MODE_AREG;
									decoded->dst.params.regs.pri = *istream & 0x7;
								} else {
									decoded->src.addr_mode = MODE_AREG;
									decoded->src.params.regs.pri = *istream & 0x7;
								}
								break;
							case 6:
								decoded->extra.size = OPSIZE_UNSIZED;
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
									decoded->src.addr_mode = MODE_IMMEDIATE;
									decoded->src.params.immed =*(++istream);
									break;
								case 3:
									decoded->op = M68K_RTE;
									break;
								case 4:
#ifdef M68010
									decoded->op = M68K_RTD;
									decoded->src.addr_mode = MODE_IMMEDIATE;
									decoded->src.params.immed =*(++istream);
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
#ifdef M68010
#endif
								break;
							}
						}
						break;
					}
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
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = *istream & 0x7;
				decoded->src.params.immed = sign_extend16(*(++istream));
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
			immed = m68k_reg_quick_field(*istream);
			if (!immed) {
				immed = 8;
			}
			decoded->src.params.immed = immed;
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
		decoded->src.params.immed = immed;
		break;
	case MOVEQ:
		decoded->op = M68K_MOVE;
		decoded->variant = VAR_QUICK;
		decoded->extra.size = OPSIZE_LONG;
		decoded->src.addr_mode = MODE_IMMEDIATE;
		decoded->src.params.immed = sign_extend8(*istream & 0xFF);
		decoded->dst.addr_mode = MODE_REG;
		decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
		immed = *istream & 0xFF;
		break;
	case OR_DIV_SBCD:
		//for OR, if opmode bit 2 is 1, then src = Dn, dst = <ea>
		opmode = (*istream >> 6) & 0x7;
		size = opmode & 0x3;
		if (size == OPSIZE_INVALID || (opmode & 0x4 && !(*istream & 0x30))) {
			switch(opmode)
			{
			case 3:
				decoded->op = M68K_DIVU;
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = (*istream >> 9) & 0x7;
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
				break;
			case 4:
				decoded->op = M68K_SBCD;
				decoded->dst.addr_mode = decoded->src.addr_mode = *istream & 0x8 ? MODE_AREG_PREDEC : MODE_REG;
				decoded->src.params.regs.pri = *istream & 0x7;
				decoded->dst.params.regs.pri = (*istream >> 9) & 0x7;
				break;
			case 5:
	#ifdef M68020
	#endif
				break;
			case 6:
	#ifdef M68020
	#endif
				break;
			case 7:
				decoded->op = M68K_DIVS;
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = (*istream >> 9) & 0x7;
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
				break;		
			}
		} else {
			decoded->op = M68K_OR;
			decoded->extra.size = size;
			if (opmode & 0x4) {
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = (*istream >> 9) & 0x7;
				istream = m68k_decode_op(istream, size, &(decoded->dst));
			} else {
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = (*istream >> 9) & 0x7;
				istream = m68k_decode_op(istream, size, &(decoded->src));
			}
		}
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
					decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
					istream = m68k_decode_op(istream, OPSIZE_LONG, &(decoded->src));
				} else {
					decoded->extra.size = size;
					decoded->src.addr_mode = MODE_REG;
					decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
					istream = m68k_decode_op(istream, size, &(decoded->dst));
				}
			} else {
				//SUBX
				decoded->op = M68K_SUBX;
				decoded->extra.size = size;
				istream = m68k_decode_op(istream, size, &(decoded->src));
				decoded->dst.addr_mode = decoded->src.addr_mode;
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
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
			decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		}
		break;
	case RESERVED:
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
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
			} else {
				//EOR
				decoded->op = M68K_EOR;
				decoded->extra.size = size;
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
			}
		} else {
			//CMP
			decoded->extra.size = size;
			decoded->dst.addr_mode = MODE_REG;
			decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
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
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
			} else if(!(*istream & 0xF0)) {
				decoded->op = M68K_ABCD;
				decoded->extra.size = OPSIZE_BYTE;
				decoded->src.params.regs.pri = *istream & 0x7;
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
				decoded->dst.addr_mode = decoded->src.addr_mode = (*istream & 8) ? MODE_AREG_PREDEC : MODE_REG;
			} else if(!(*istream & 0x30)) {
				decoded->op = M68K_EXG;
				decoded->extra.size = OPSIZE_LONG;
				decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
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
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->dst));
			}
		} else {
			if ((*istream & 0xC0) == 0xC0) {
				decoded->op = M68K_MULU;
				decoded->extra.size = OPSIZE_WORD;
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->src));
			} else {
				decoded->op = M68K_AND;
				decoded->extra.size = (*istream >> 6);
				decoded->dst.addr_mode = MODE_REG;
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
				istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
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
					decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
					istream = m68k_decode_op(istream, OPSIZE_LONG, &(decoded->src));
				} else {
					decoded->extra.size = size;
					decoded->src.addr_mode = MODE_REG;
					decoded->src.params.regs.pri = m68k_reg_quick_field(*istream);
					istream = m68k_decode_op(istream, size, &(decoded->dst));
				}
			} else {
				//ADDX
				decoded->op = M68K_ADDX;
				//FIXME: Size is not technically correct
				decoded->extra.size = size;
				istream = m68k_decode_op(istream, size, &(decoded->src));
				decoded->dst.addr_mode = decoded->src.addr_mode;
				decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
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
			decoded->dst.params.regs.pri = m68k_reg_quick_field(*istream);
			istream = m68k_decode_op(istream, decoded->extra.size, &(decoded->src));
		}
		break;
	case SHIFT_ROTATE:
		if ((*istream & 0x8C0) == 0xC0) {
			switch((*istream >> 8) & 0x7)
			{
			case 0:
				decoded->op = M68K_ASR;
				break;
			case 1:
				decoded->op = M68K_ASL;
				break;
			case 2:
				decoded->op = M68K_LSR;
				break;
			case 3:
				decoded->op = M68K_LSL;
				break;
			case 4:
				decoded->op = M68K_ROXR;
				break;
			case 5:
				decoded->op = M68K_ROXL;
				break;
			case 6:
				decoded->op = M68K_ROR;
				break;
			case 7:
				decoded->op = M68K_ROL;
				break;
			}
			decoded->extra.size = OPSIZE_WORD;
			istream = m68k_decode_op(istream, OPSIZE_WORD, &(decoded->dst));
		} else if((*istream & 0xC0) != 0xC0) {
			switch(((*istream >> 2) & 0x6) | ((*istream >> 8) & 1))
			{
			case 0:
				decoded->op = M68K_ASR;
				break;
			case 1:
				decoded->op = M68K_ASL;
				break;
			case 2:
				decoded->op = M68K_LSR;
				break;
			case 3:
				decoded->op = M68K_LSL;
				break;
			case 4:
				decoded->op = M68K_ROXR;
				break;
			case 5:
				decoded->op = M68K_ROXL;
				break;
			case 6:
				decoded->op = M68K_ROR;
				break;
			case 7:
				decoded->op = M68K_ROL;
				break;
			}
			decoded->extra.size = (*istream >> 6) & 0x3;
			immed = (*istream >> 9) & 0x7;
			if (*istream & 0x20) {
				decoded->src.addr_mode = MODE_REG;
				decoded->src.params.regs.pri = immed;
			} else {
				decoded->src.addr_mode = MODE_IMMEDIATE;
				if (!immed) {
					immed = 8;
				}
				decoded->src.params.immed = immed;
			}
			decoded->dst.addr_mode = MODE_REG;
			decoded->dst.params.regs.pri = *istream & 0x7;
			
		} else {
#ifdef M68020
			//TODO: Implement bitfield instructions for M68020+ support
#endif
		}
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
	"andi",//ccr
	"andi",//sr
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
	"eori",//ccr
	"eori",//sr
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
	"move",//ccr
	"move",//from_sr
	"move",//sr
	"move",//usp
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
	"ori",//ccr
	"ori",//sr
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
	"unlk",
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

int m68k_disasm_op(m68k_op_info *decoded, char *dst, int need_comma)
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
	case MODE_AREG_DISPLACE:
		return sprintf(dst, "%s (a%d, %d)", c, decoded->params.regs.pri, decoded->params.regs.displacement);
	case MODE_IMMEDIATE:
	case MODE_IMMEDIATE_WORD:
		return sprintf(dst, (decoded->params.immed <= 128 ? "%s #%d" : "%s #$%X"), c, decoded->params.immed);
	case MODE_ABSOLUTE_SHORT:
		return sprintf(dst, "%s $%X.w", c, decoded->params.immed);
	case MODE_ABSOLUTE:
		return sprintf(dst, "%s $%X", c, decoded->params.immed);
	case MODE_PC_DISPLACE:
		return sprintf(dst, "%s (pc, %d)", c, decoded->params.regs.displacement);
	default:
		return 0;
	}
}

int m68k_disasm_movem_op(m68k_op_info *decoded, m68k_op_info *other, char *dst, int need_comma)
{
	int8_t dir, reg, bit, regnum, last=-1, lastreg, first=-1;
	char *rtype, *last_rtype;
	int oplen;
	if (decoded->addr_mode == MODE_REG) {
		if (other->addr_mode == MODE_AREG_PREDEC) {
			bit = 15;
			dir = -1;
		} else {
			reg = 0;
			bit = 1;
		}
		strcat(dst, " ");
		for (oplen = 1, reg=0; bit < 16 && bit > -1; bit += dir, reg++) {
			if (decoded->params.immed & (1 << bit)) {
				if (reg > 7) {
					rtype = "a";
					regnum = reg - 8;
				} else {
					rtype = "d";
					regnum = reg;
				}
				if (last >= 0 && last == regnum - 1 && lastreg == reg - 1) {
					last = regnum;
					lastreg = reg;
				} else if(last >= 0) {
					if (first != last) {
						oplen += sprintf(dst + oplen, "-%s%d/%s%d",last_rtype, last, rtype, regnum);
					} else {
						oplen += sprintf(dst + oplen, "/%s%d", rtype, regnum);
					}
					first = last = regnum;
					last_rtype = rtype;
					lastreg = reg;
				} else {
					oplen += sprintf(dst + oplen, "%s%d", rtype, regnum);
					first = last = regnum;
					last_rtype = rtype;
					lastreg = reg;
				}
			}
		}
		if (last >= 0 && last != first) {
			oplen += sprintf(dst + oplen, "-%s%d", last_rtype, last);
		}
		return oplen;
	} else {
		return m68k_disasm_op(decoded, dst, need_comma);
	}
}

int m68k_disasm(m68kinst * decoded, char * dst)
{
	int ret,op1len;
	uint8_t size;
	char * special_op = "CCR";
	switch (decoded->op)
	{
	case M68K_BCC:
	case M68K_DBCC:
	case M68K_SCC:
		ret = strlen(mnemonics[decoded->op]) - 2;
		memcpy(dst, mnemonics[decoded->op], ret);
		dst[ret] = 0;
		strcpy(dst+ret, cond_mnem[decoded->extra.cond]);
		ret = strlen(dst);
		if (decoded->op != M68K_SCC) {
			if (decoded->op == M68K_DBCC) {
				ret += sprintf(dst+ret, " d%d, #%d <%X>", decoded->dst.params.regs.pri, decoded->src.params.immed, decoded->address + 2 + decoded->src.params.immed);
			} else {
				ret += sprintf(dst+ret, " #%d <%X>", decoded->src.params.immed, decoded->address + 2 + decoded->src.params.immed);
			}
			return ret;
		}
		break;
	case M68K_BSR:
		ret = sprintf(dst, "bsr%s #%d <%X>", decoded->variant == VAR_BYTE ? ".s" : "", decoded->src.params.immed, decoded->address + 2 + decoded->src.params.immed);
		return ret;
	case M68K_MOVE_FROM_SR:
		ret = sprintf(dst, "%s", mnemonics[decoded->op]);
		ret += sprintf(dst + ret, " SR");
		ret += m68k_disasm_op(&(decoded->dst), dst + ret, 1);
		return ret;
	case M68K_ANDI_SR:
	case M68K_EORI_SR:
	case M68K_MOVE_SR:
	case M68K_ORI_SR:
		special_op = "SR";
	case M68K_ANDI_CCR:
	case M68K_EORI_CCR:
	case M68K_MOVE_CCR:
	case M68K_ORI_CCR:
		ret = sprintf(dst, "%s", mnemonics[decoded->op]);
		ret += m68k_disasm_op(&(decoded->src), dst + ret, 0);
		ret += sprintf(dst + ret, ", %s", special_op);
		return ret;
	case M68K_MOVE_USP:
		ret = sprintf(dst, "%s", mnemonics[decoded->op]);
		if (decoded->src.addr_mode != MODE_UNUSED) {
			ret += m68k_disasm_op(&(decoded->src), dst + ret, 0);
			ret += sprintf(dst + ret, ", USP");
		} else {
			ret += sprintf(dst + ret, "USP, ");
			ret += m68k_disasm_op(&(decoded->dst), dst + ret, 0);
		}
		return ret;
	default:
		size = decoded->extra.size;
		ret = sprintf(dst, "%s%s%s", 
				mnemonics[decoded->op], 
				decoded->variant == VAR_QUICK ? "q" : (decoded->variant == VAR_IMMEDIATE ? "i" : ""), 
				size == OPSIZE_BYTE ? ".b" : (size == OPSIZE_WORD ? ".w" : (size == OPSIZE_LONG ? ".l" : "")));
	}
	if (decoded->op == M68K_MOVEM) {
		op1len = m68k_disasm_movem_op(&(decoded->src), &(decoded->dst), dst + ret, 0);
		ret += op1len;
		ret += m68k_disasm_movem_op(&(decoded->dst), &(decoded->src), dst + ret, op1len);
	} else {
		op1len = m68k_disasm_op(&(decoded->src), dst + ret, 0);
		ret += op1len;
		ret += m68k_disasm_op(&(decoded->dst), dst + ret, op1len);
	}
	return ret;
}

