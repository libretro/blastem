#include "z80_to_x86.h"
#include "gen_x86.h"

#define MODE_UNUSED (MODE_IMMED-1)

#define ZCYCLES RBP
#define SCRATCH1 R13
#define SCRATCH2 R14

void z80_read_byte();
void z80_read_word();

uint8_t z80_size(z80_inst * inst)
{
	uint8_t reg = (inst->reg & 0x1F);
	if (reg != Z80_UNUSED &&) {
		return reg < Z80_BC ? SZ_B : SZ_W;
	}
	//TODO: Handle any necessary special cases
	return SZ_B;
}

uint8_t * zcylces(dst, uint32_t num_cycles)
{
	return add_ir(dst, num_cycles, ZCYCLES, SZ_D);
}

uint8_t * translate_z80_reg(z80_inst * inst, x86_ea * ea, uint8_t * dst, x86_z80_options * opts)
{
	if (inst->reg == Z80_USE_IMMED) {
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
	} else if ((inst->reg & 0x1F) == Z80_UNUSED) {
		ea->mode = MODE_UNUSED;
	} else {
		ea->mode = MODE_REG;
		if (inst->reg == Z80_IYH) {
			ea->base = opts->regs[Z80_IYL];
			dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
		} else {
			ea->base = opts->regs[inst->reg]
		}
	}
	return dst;
}

uint8_t * save_z80_reg(uint8_t * dst, z80_inst * inst, x86_z80_options * opts)
{
	if (inst->reg == Z80_IYH) {
		dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
	}
	return dst;
}

uint8_t * translate_z80_ea(z80_inst * inst, x86_ea * ea, uint8_t * dst, x86_z80_options * opts, uint8_t read, uint8_t modify)
{
	uint8_t size, reg, areg;
	ea->mode = MODE_REG;
	areg = read ? SCRATCH1 : SCRATCH2;
	switch(inst->addr_mode & 0x1F)
	{
	case Z80_REG:
		if (inst->ea_reg == Z80_IYH) {
			ea->base = opts->regs[Z80_IYL];
			dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
		} else {
			ea->base = opts->regs[inst->ea_reg];
		}
		break;
	case Z80_REG_INDIRECT:
		dst = mov_rr(dst, opts->regs[inst->ea_reg], areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		ea->base = SCRATCH1;
		break;
	case Z80_IMMED:
		ea->mode = MODE_IMMED;
		ea->disp = inst->immed;
		break;
	case Z80_IMMED_INDIRECT:
		dst = mov_ir(dst, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		ea->base = SCRATCH1;
		break;
	case Z80_IX_INDEXED:
	case Z80_IY_INDEXED:
		reg = opts->regs[inst->addr_mode == Z80_IX_INDEXED ? Z80_IX : Z80_IY];
		dst = mov_rr(dst, reg, areg, SZ_W);
		dst = add_ir(dst, inst->immed, areg, SZ_W);
		size = z80_size(inst);
		if (read) {
			if (modify) {
				dst = push_r(dst, SCRATCH1);
			}
			if (size == SZ_B) {
				dst = call(dst, (uint8_t *)z80_read_byte);
			} else {
				dst = call(dst, (uint8_t *)z80_read_word);
			}
			if (modify) {
				dst = pop_r(dst, SCRATCH2);
			}
		}
		break;
	case Z80_UNUSED:
		ea->mode = MODE_UNUSED:
		break;
	default:
		fprintf(stderr, "Unrecognized Z80 addressing mode %d\n", inst->addr_mode);
		exit(1);
	}
	return dst;
}

uint8_t * z80_save_ea(uint8_t * dst, z80_inst * inst, x86_z80_options * opts)
{
	if (inst->addr_mode == Z80_REG_DIRECT && inst->ea_reg == Z80_IYH) {
		dst = ror_ir(dst, 8, opts->regs[Z80_IY], SZ_W);
	}
	return dst;
}

uint8_t * z80_save_result(uint8_t * dst, z80_inst * inst)
{
	if (z80_size(inst). == SZ_B) {
		dst = call(dst, (uint8_t *)z80_write_byte);
	} else {
		dst = call(dst, (uint8_t *)z80_write_word);
	}
	return dst;
}

enum {
	DONT_READ=0,
	READ
};

enum {
	DONT_MODIFY=0,
	MODIFY
};

uint8_t zf_off(uint8_t flag)
{
	return offsetof(z80_context, flags) + flag;
}

uint8_t * translate_z80_inst(z80_inst * inst, uint8_t * dst, x86_z80_options * opts)
{
	uint32_t cycles;
	x86_ea src_op, dst_op;
	switch(inst->op)
	{
	case Z80_LD:
		if (inst->addr_mode & Z80_DIR) {
			dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
			dst = translate_z80_reg(inst, &dst_op, dst, opts);
		} else {
			dst = translate_z80_reg(inst, &src_op, dst, opts);
			dst = translate_z80_ea(inst, &dst_op, dst, opts, DONT_READ, MODIFY);
		}
		if (ea_op.mode == MODE_REG_DIRECT) {
			dst = mov_rr(dst, ea_op.base, reg_op.base, z80_size(inst));
		} else {
			dst = mov_ir(dst, ea_op.disp, reg_op.base, z80_size(inst));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		if (!(inst->addr_mode & Z80_DIR)) {
			dst = z80_save_result(dst, inst, opts);
		}
		break;
	case Z80_PUSH:
		dst = zcycles(dst, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 9 : 5);
		dst = sub_ir(dst, opts->regs[Z80_SP], SZ_W);
		dst = translate_z80_reg(inst, &src_op, dst, opts);
		dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_W);
		dst = call(dst, z80_write_word);
		break;
	case Z80_POP:
		dst = zcycles(dst, (inst->reg == Z80_IX || inst->reg == Z80_IY) ? 8 : 4);
		dst = sub_ir(dst, opts->regs[Z80_SP], SZ_W);
		dst = translate_z80_reg(inst, &src_op, dst, opts);
		dst = mov_rr(dst, src_op.base, SCRATCH1, SZ_W);
		dst = call(dst, z80_write_word);
		break;
	/*case Z80_EX:
	case Z80_EXX:
	case Z80_LDI:
	case Z80_LDIR:
	case Z80_LDD:
	case Z80_LDDR:
	case Z80_CPI:
	case Z80_CPIR:
	case Z80_CPD:
	case Z80_CPDR:
		break;*/
	case Z80_ADD:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_INDIRECT || inst->addr_mdoe == Z80_IY_INDIRECT) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 4;
		}
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = add_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = add_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N));
		//TODO: Implement half-carry flag
		if (z80_size(inst) == SZ_B) {
			dst = setcc_rdisp8(dst, CC_O, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	/*case Z80_ADC:
		break;*/
	case Z80_SUB:
		cycles = 4;
		if (inst->addr_mode == Z80_IX_INDIRECT || inst->addr_mdoe == Z80_IY_INDIRECT) {
			cycles += 12;
		} else if(inst->addr_mode == Z80_IMMED) {
			cycles += 3;
		}
		dst = zcycles(dst, cycles);
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		dst = translate_z80_ea(inst, &src_op, dst, opts, READ, DONT_MODIFY);
		if (src_op.mode == MODE_REG_DIRECT) {
			dst = sub_rr(dst, src_op.base, dst_op.base, z80_size(inst));
		} else {
			dst = sub_ir(dst, src_op.disp, dst_op.base, z80_size(inst));
		}
		dst = setcc_rdisp8(dst, CC_C, CONTEXT, zf_off(ZF_C));
		dst = mov_irdisp8(dst, 1, CONTEXT, zf_off(ZF_N));
		dst = setcc_rdisp8(dst, CC_O, zf_off(ZF_PV));
		//TODO: Implement half-carry flag
		dst = setcc_rdisp8(dst, CC_Z, zf_off(ZF_Z));
		dst = setcc_rdisp8(dst, CC_S, zf_off(ZF_S)
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	/*case Z80_SBC:
	case Z80_AND:
	case Z80_OR:
	case Z80_XOR:
	case Z80_CP:*/
	case Z80_INC:
		cycles = 4;
		if (inst->reg == Z80_IX || inst->reg == Z80_IY) {
			cycles += 6;
		} else if(z80_size(inst) == SZ_W) {
			cycles += 2;
		} else if(inst->reg == Z80_IXH || inst->reg == Z80_IXL || inst->reg == Z80_IYH || inst->reg == Z80_IYL || inst->addr_mode == Z80_IX_DISPLACE || inst->addr_mode == Z80_IY_DISPLACE) {
			cycles += 4;
		}
		dst = translate_z80_reg(inst, &dst_op, dst, opts);
		if (dst_op.mode == MODE_UNUSED) {
			dst = translate_z80_ea(inst, &dst_op, dst, opts, READ, MODIFY);
		}
		dst = add_ir(dst, 1, dst_op.base, z80_size(inst));
		if (z80_size(inst) == SZ_B) {
			dst = mov_irdisp8(dst, 0, CONTEXT, zf_off(ZF_N));
			//TODO: Implement half-carry flag
			dst = setcc_rdisp8(dst, CC_O, zf_off(ZF_PV));
			dst = setcc_rdisp8(dst, CC_Z, zf_off(ZF_Z));
			dst = setcc_rdisp8(dst, CC_S, zf_off(ZF_S));
		}
		dst = z80_save_reg(dst, inst, opts);
		dst = z80_save_ea(dst, inst, opts);
		break;
	/*case Z80_DEC:
		break;
	case Z80_DAA:
	case Z80_CPL:
	case Z80_NEG:
	case Z80_CCF:
	case Z80_SCF:*/
	case Z80_NOP:
		if (inst->immed == 42) {
			dst = call(dst, (uint8_t *)z80_save_context);
			dst = mov_rr(dst, CONTEXT, RDI, SZ_Q);
			dst = jmp(dst, (uint8_t *)z80_print_regs_exit);
		} else {
			dst = zcycles(dst, 4 * inst->immed);
		}
		break;
	/*case Z80_HALT:
	case Z80_DI:
	case Z80_EI:
	case Z80_IM:
	case Z80_RLC:
	case Z80_RL:
	case Z80_RRC:
	case Z80_RR:
	case Z80_SLA:
	case Z80_SRA:
	case Z80_SLL:
	case Z80_SRL:
	case Z80_RLD:
	case Z80_RRD:
	case Z80_BIT:
	case Z80_SET:
	case Z80_RES:
	case Z80_JP:
	case Z80_JPCC:
	case Z80_JR:
	case Z80_JRCC:
	case Z80_DJNZ:
	case Z80_CALL:
	case Z80_CALLCC:
	case Z80_RET:
	case Z80_RETCC:
	case Z80_RETI:
	case Z80_RETN:
	case Z80_RST:
	case Z80_IN:
	case Z80_INI:
	case Z80_INIR:
	case Z80_IND:
	case Z80_INDR:
	case Z80_OUT:
	case Z80_OUTI:
	case Z80_OTIR:
	case Z80_OUTD:
	case Z80_OTDR:*/
	default:
		fprintf(stderr, "unimplemented instruction: %d\n", inst->op);
		exit(1);
	}
}

void translate_z80_stream(z80_context * context, uint16_t address)
{
}
