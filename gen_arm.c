/*
 Copyright 2014 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gen_arm.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>

#define OP_FIELD_SHIFT 21u

//Data processing format instructions
#define OP_AND 0x0u
#define OP_EOR (0x1u << OP_FIELD_SHIFT)
#define OP_SUB (0x2u << OP_FIELD_SHIFT)
#define OP_RSB (0x3u << OP_FIELD_SHIFT)
#define OP_ADD (0x4u << OP_FIELD_SHIFT)
#define OP_ADC (0x5u << OP_FIELD_SHIFT)
#define OP_SBC (0x6u << OP_FIELD_SHIFT)
#define OP_RSC (0x7u << OP_FIELD_SHIFT)
#define OP_TST (0x8u << OP_FIELD_SHIFT)
#define OP_TEQ (0x9u << OP_FIELD_SHIFT)
#define OP_CMP (0xAu << OP_FIELD_SHIFT)
#define OP_CMN (0xBu << OP_FIELD_SHIFT)
#define OP_ORR (0xCu << OP_FIELD_SHIFT)
#define OP_MOV (0xDu << OP_FIELD_SHIFT)
#define OP_BIC (0xEu << OP_FIELD_SHIFT)
#define OP_MVN (0xFu << OP_FIELD_SHIFT)

//branch instructions
#define OP_B  0xA000000u
#define OP_BL 0xB000000u
#define OP_BX 0x12FFF10u

//load/store
#define OP_STR   0x4000000u
#define OP_LDR   0x4100000u
#define OP_STM   0x8000000u
#define OP_LDM   0x8100000u
#define POST_IND 0u
#define PRE_IND  0x1000000u
#define DIR_DOWN 0u
#define DIR_UP   0x0800000u
#define SZ_W     0u
#define SZ_B     0x0400000u
#define WRITE_B  0x0200000u
#define OFF_IMM  0u
#define OFF_REG  0x2000000u

#define PUSH     (OP_STR | PRE_IND | OFF_IMM | SZ_W | WRITE_B | DIR_DOWN | sizeof(uint32_t) | (sp << 16))
#define POP      (OP_LDR | POST_IND | OFF_IMM | SZ_W | DIR_UP | sizeof(uint32_t) | (sp << 16))
#define PUSHM     (OP_STM | PRE_IND | SZ_W | WRITE_B | DIR_DOWN | (sp << 16))
#define POPM      (OP_LDM | POST_IND | SZ_W | WRITE_B | DIR_UP | (sp << 16))

#define IMMED    0x2000000u
#define REG      0u


uint32_t make_immed(uint32_t val)
{
	uint32_t rot_amount = 0;
	for (; rot_amount < 0x20; rot_amount += 2)
	{
		uint32_t test_mask = ~(0xFF << rot_amount | 0xFF >> (32-rot_amount));
		if (!(test_mask & val)) {
			return val << rot_amount | val >> (32-rot_amount) | rot_amount << 7;
		}
	}
	return INVALID_IMMED;
}

void check_alloc_code(code_info *code)
{
	if (code->cur == code->last) {
		size_t size = CODE_ALLOC_SIZE;
		uint32_t *next_code = alloc_code(&size);
		if (!next_code) {
			fatal_error("Failed to allocate memory for generated code\n");
		}
		if (next_code = code->last + RESERVE_WORDS) {
			//new chunk is contiguous with the current one
			code->last = next_code + size/sizeof(code_word) - RESERVE_WORDS;
		} else {
			uint32_t * from = code->cur + 2;
			if (next_code - from < 0x400000 || from - next_code <= 0x400000) {
				*from = CC_AL | OP_B | ((next_code - from) & 0xFFFFFF);
			} else {
				//push r0 onto the stack
				*(from++) = CC_AL | PUSH;
				uint32_t immed = make_immed((uint32_t)next_code);
				if (immed == INVALID_IMMED) {
					//Load target into r0 from word after next instruction into register 0
					*(from++) = CC_AL | OP_LDR | OFF_IMM | DIR_DOWN | PRE_IND | SZ_W | (pc << 16) | 4;
					from[1] = (uint32_t)next_code;
				} else {
					//Load target into r0
					*(from++) = CC_AL | OP_MOV | IMMED | NO_COND | immed;
				}
				//branch to address in r0
				*from = CC_AL | OP_BX;
				code->last = next_code + size/sizeof(code_word) - RESERVE_WORDS;
				//pop r0
				*(next_code++) = CC_AL | POP;
				code->cur = next_code;
			}
		}
	}
}

uint32_t data_proc(code_info *code, uint32_t cond, uint32_t op, uint32_t set_cond, uint32_t dst, uint32_t src1, uint32_t src2)
{
	check_alloc_code(code);
	*(code->cur++) = cond | op | set_cond | (src1 << 16) | (dst << 12) | src2;

	return CODE_OK;
}

uint32_t data_proci(code_info *code, uint32_t cond, uint32_t op, uint32_t set_cond, uint32_t dst, uint32_t src1, uint32_t immed)
{
	immed = make_immed(immed);
	if (immed == INVALID_IMMED) {
		return immed;
	}
	return data_proc(code, cond, op | IMMED, set_cond, dst, src1, immed);
}

//TODO: support shifted register for op2

uint32_t and(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_AND, set_cond, dst, src1, src2);
}

uint32_t andi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_AND, set_cond, dst, src1, immed);
}

uint32_t and_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_AND, set_cond, dst, src1, src2);
}

uint32_t andi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_AND, set_cond, dst, src1, immed);
}

uint32_t eor(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_EOR, set_cond, dst, src1, src2);
}

uint32_t eori(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_EOR, set_cond, dst, src1, immed);
}

uint32_t eor_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_EOR, set_cond, dst, src1, src2);
}

uint32_t eori_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_EOR, set_cond, dst, src1, immed);
}

uint32_t sub(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_SUB, set_cond, dst, src1, src2);
}

uint32_t subi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_SUB, set_cond, dst, src1, immed);
}

uint32_t sub_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_SUB, set_cond, dst, src1, src2);
}

uint32_t subi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_SUB, set_cond, dst, src1, immed);
}

uint32_t rsb(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_RSB, set_cond, dst, src1, src2);
}

uint32_t rsbi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_RSB, set_cond, dst, src1, immed);
}

uint32_t rsb_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_RSB, set_cond, dst, src1, src2);
}

uint32_t rsbi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_RSB, set_cond, dst, src1, immed);
}

uint32_t add(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_ADD, set_cond, dst, src1, src2);
}

uint32_t addi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_ADD, set_cond, dst, src1, immed);
}

uint32_t add_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_ADD, set_cond, dst, src1, src2);
}

uint32_t addi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_ADD, set_cond, dst, src1, immed);
}

uint32_t adc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_ADC, set_cond, dst, src1, src2);
}

uint32_t adci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_ADC, set_cond, dst, src1, immed);
}

uint32_t adc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_ADC, set_cond, dst, src1, src2);
}

uint32_t adci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_ADC, set_cond, dst, src1, immed);
}

uint32_t sbc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_SBC, set_cond, dst, src1, src2);
}

uint32_t sbci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_SBC, set_cond, dst, src1, immed);
}

uint32_t sbc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_SBC, set_cond, dst, src1, src2);
}

uint32_t sbci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_SBC, set_cond, dst, src1, immed);
}

uint32_t rsc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_RSC, set_cond, dst, src1, src2);
}

uint32_t rsci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_RSC, set_cond, dst, src1, immed);
}

uint32_t rsc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_RSC, set_cond, dst, src1, src2);
}

uint32_t rsci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_RSC, set_cond, dst, src1, immed);
}

uint32_t tst(code_info *code, uint32_t src1, uint32_t src2)
{
	return data_proc(code, CC_AL, OP_TST, SET_COND, r0, src1, src2);
}

uint32_t tsti(code_info *code, uint32_t src1, uint32_t immed)
{
	return data_proci(code, CC_AL, OP_TST, SET_COND, r0, src1, immed);
}

uint32_t tst_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc)
{
	return data_proc(code, cc, OP_TST, SET_COND, r0, src1, src2);
}

uint32_t tsti_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc)
{
	return data_proci(code, cc, OP_TST, SET_COND, r0, src1, immed);
}

uint32_t teq(code_info *code, uint32_t src1, uint32_t src2)
{
	return data_proc(code, CC_AL, OP_TEQ, SET_COND, r0, src1, src2);
}

uint32_t teqi(code_info *code, uint32_t src1, uint32_t immed)
{
	return data_proci(code, CC_AL, OP_TEQ, SET_COND, r0, src1, immed);
}

uint32_t teq_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc)
{
	return data_proc(code, cc, OP_TEQ, SET_COND, r0, src1, src2);
}

uint32_t teqi_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc)
{
	return data_proci(code, cc, OP_TEQ, SET_COND, r0, src1, immed);
}

uint32_t cmp(code_info *code, uint32_t src1, uint32_t src2)
{
	return data_proc(code, CC_AL, OP_CMP, SET_COND, r0, src1, src2);
}

uint32_t cmpi(code_info *code, uint32_t src1, uint32_t immed)
{
	return data_proci(code, CC_AL, OP_CMP, SET_COND, r0, src1, immed);
}

uint32_t cmp_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc)
{
	return data_proc(code, cc, OP_CMP, SET_COND, r0, src1, src2);
}

uint32_t cmpi_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc)
{
	return data_proci(code, cc, OP_CMP, SET_COND, r0, src1, immed);
}

uint32_t cmn(code_info *code, uint32_t src1, uint32_t src2)
{
	return data_proc(code, CC_AL, OP_CMN, SET_COND, r0, src1, src2);
}

uint32_t cmni(code_info *code, uint32_t src1, uint32_t immed)
{
	return data_proci(code, CC_AL, OP_CMN, SET_COND, r0, src1, immed);
}

uint32_t cmn_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc)
{
	return data_proc(code, cc, OP_CMN, SET_COND, r0, src1, src2);
}

uint32_t cmni_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc)
{
	return data_proci(code, cc, OP_CMN, SET_COND, r0, src1, immed);
}

uint32_t orr(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_ORR, set_cond, dst, src1, src2);
}

uint32_t orri(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_ORR, set_cond, dst, src1, immed);
}

uint32_t orr_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_ORR, set_cond, dst, src1, src2);
}

uint32_t orri_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_ORR, set_cond, dst, src1, immed);
}

uint32_t mov(code_info *code, uint32_t dst, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_MOV, set_cond, dst, 0, src2);
}

uint32_t movi(code_info *code, uint32_t dst, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_MOV, set_cond, dst, 0, immed);
}

uint32_t mov_cc(code_info *code, uint32_t dst, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_MOV, set_cond, dst, 0, src2);
}

uint32_t movi_cc(code_info *code, uint32_t dst, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_MOV, set_cond, dst, 0, immed);
}

uint32_t bic(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_BIC, set_cond, dst, src1, src2);
}

uint32_t bici(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_BIC, set_cond, dst, src1, immed);
}

uint32_t bic_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_BIC, set_cond, dst, src1, src2);
}

uint32_t bici_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_BIC, set_cond, dst, src1, immed);
}

uint32_t mvn(code_info *code, uint32_t dst, uint32_t src2, uint32_t set_cond)
{
	return data_proc(code, CC_AL, OP_MVN, set_cond, dst, 0, src2);
}

uint32_t mvni(code_info *code, uint32_t dst, uint32_t immed, uint32_t set_cond)
{
	return data_proci(code, CC_AL, OP_MVN, set_cond, dst, 0, immed);
}

uint32_t mvn_cc(code_info *code, uint32_t dst, uint32_t src2, uint32_t cc, uint32_t set_cond)
{
	return data_proc(code, cc, OP_MVN, set_cond, dst, 0, src2);
}

uint32_t mvni_cc(code_info *code, uint32_t dst, uint32_t immed, uint32_t cc, uint32_t set_cond)
{
	return data_proci(code, cc, OP_MVN, set_cond, dst, 0, immed);
}

uint32_t branchi(code_info *code, uint32_t cc, uint32_t op, uint32_t *dst)
{
	uint32_t * from = code->cur + 2;
	if (dst - from >= 0x400000 && from - dst > 0x400000) {
		return INVALID_IMMED;
	}
	check_alloc_code(code);
	*(code->cur++) = cc | op | ((dst - from) & 0xFFFFFF);
	return CODE_OK;
}

uint32_t b(code_info *code, uint32_t *dst)
{
	return branchi(code, CC_AL, OP_B, dst);
}

uint32_t b_cc(code_info *code, uint32_t *dst, uint32_t cc)
{
	return branchi(code, cc, OP_B, dst);
}

uint32_t bl(code_info *code, uint32_t *dst)
{
	return branchi(code, CC_AL, OP_BL, dst);
}

uint32_t bl_cc(code_info *code, uint32_t *dst, uint32_t cc)
{
	return branchi(code, cc, OP_BL, dst);
}

uint32_t bx(code_info *code, uint32_t dst)
{
	check_alloc_code(code);
	*(code->cur++) = CC_AL | OP_BX | dst;
	return CODE_OK;
}

uint32_t bx_cc(code_info *code, uint32_t dst, uint32_t cc)
{
	check_alloc_code(code);
	*(code->cur++) = cc | OP_BX | dst;
	return CODE_OK;
}

uint32_t push(code_info *code, uint32_t reg)
{
	check_alloc_code(code);
	*(code->cur++) = CC_AL | PUSH | reg << 12;
	return CODE_OK;
}

uint32_t push_cc(code_info *code, uint32_t reg, uint32_t cc)
{
	check_alloc_code(code);
	*(code->cur++) = cc | PUSH | reg << 12;
	return CODE_OK;
}

uint32_t pushm(code_info *code, uint32_t reglist)
{
	check_alloc_code(code);
	*(code->cur++) = CC_AL | PUSHM | reglist;
	return CODE_OK;
}

uint32_t pushm_cc(code_info *code, uint32_t reglist, uint32_t cc)
{
	check_alloc_code(code);
	*(code->cur++) = cc | PUSHM | reglist;
	return CODE_OK;
}

uint32_t pop(code_info *code, uint32_t reg)
{
	check_alloc_code(code);
	*(code->cur++) = CC_AL | POP | reg << 12;
	return CODE_OK;
}

uint32_t pop_cc(code_info *code, uint32_t reg, uint32_t cc)
{
	check_alloc_code(code);
	*(code->cur++) = cc | POP | reg << 12;
	return CODE_OK;
}

uint32_t popm(code_info *code, uint32_t reglist)
{
	check_alloc_code(code);
	*(code->cur++) = CC_AL | POPM | reglist;
	return CODE_OK;
}

uint32_t popm_cc(code_info *code, uint32_t reglist, uint32_t cc)
{
	check_alloc_code(code);
	*(code->cur++) = cc | POPM | reglist;
	return CODE_OK;
}

uint32_t load_store_immoff(code_info *code, uint32_t op, uint32_t dst, uint32_t base, int32_t offset, uint32_t cc)
{
	if (offset >= 0x1000 || offset <= -0x1000) {
		return INVALID_IMMED;
	}
	check_alloc_code(code);
	uint32_t instruction = cc | op | POST_IND | OFF_IMM | SZ_W | base << 16 | dst << 12;
	if (offset >= 0) {
		instruction |= offset | DIR_UP;
	} else {
		instruction |= (-offset) | DIR_DOWN;
	}
	*(code->cur++) = instruction;
	return CODE_OK;
}

uint32_t ldr_cc(code_info *code, uint32_t dst, uint32_t base, int32_t offset, uint32_t cc)
{
	return load_store_immoff(code, OP_LDR, dst, base, offset, cc);
}

uint32_t ldr(code_info *code, uint32_t dst, uint32_t base, int32_t offset)
{
	return ldr_cc(code, dst, base, offset, CC_AL);
}

uint32_t str_cc(code_info *code, uint32_t src, uint32_t base, int32_t offset, uint32_t cc)
{
	return load_store_immoff(code, OP_STR, src, base, offset, cc);
}

uint32_t str(code_info *code, uint32_t src, uint32_t base, int32_t offset)
{
	return str_cc(code, src, base, offset, CC_AL);
}
