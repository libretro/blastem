/*
 Copyright 2014 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef GEN_ARM_H_
#define GEN_ARM_H_

#include <stdint.h>
#include "gen.h"

#define SET_COND 0x100000u
#define NO_COND  0u

#define CC_FIELD_SHIFT 28

#define CC_EQ 0x0u
#define CC_NE (0x1u << CC_FIELD_SHIFT)
#define CC_CS (0x2u << CC_FIELD_SHIFT)
#define CC_CC (0x3u << CC_FIELD_SHIFT)
#define CC_MI (0x4u << CC_FIELD_SHIFT)
#define CC_PL (0x5u << CC_FIELD_SHIFT)
#define CC_VS (0x6u << CC_FIELD_SHIFT)
#define CC_VC (0x7u << CC_FIELD_SHIFT)
#define CC_HI (0x8u << CC_FIELD_SHIFT)
#define CC_LS (0x9u << CC_FIELD_SHIFT)
#define CC_GE (0xAu << CC_FIELD_SHIFT)
#define CC_LT (0xBu << CC_FIELD_SHIFT)
#define CC_GT (0xCu << CC_FIELD_SHIFT)
#define CC_LE (0xDu << CC_FIELD_SHIFT)
#define CC_AL (0xEu << CC_FIELD_SHIFT)

#define INVALID_IMMED 0xFFFFFFFFu
#define CODE_OK 0u

enum {
	r0,
	r1,
	r2,
	r3,
	r4,
	r5,
	r6,
	r7,
	r8,
	r9,
	r10,
	r11,
	r12,
	sp,
	lr,
	pc
};

#define R0  0x1
#define R1  0x2
#define R2  0x4
#define R3  0x8
#define R4  0x10
#define R5  0x20
#define R6  0x40
#define R7  0x80
#define R8  0x100
#define R9  0x200
#define R10 0x400
#define R11 0x800
#define R12 0x1000
#define SP  0x2000
#define LR  0x4000
#define PC  0x8000

uint32_t and(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t andi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t and_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t andi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t eor(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t eori(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t eor_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t eori_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t sub(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t subi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t sub_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t subi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t rsb(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t rsbi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t rsb_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t rsbi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t add(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t addi(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t add_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t addi_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t adc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t adci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t adc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t adci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t sbc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t sbci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t sbc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t sbci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t rsc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t rsci(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t rsc_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t rsci_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t tst(code_info *code, uint32_t src1, uint32_t src2);
uint32_t tsti(code_info *code, uint32_t src1, uint32_t immed);
uint32_t tst_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc);
uint32_t tsti_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc);
uint32_t teq(code_info *code, uint32_t src1, uint32_t src2);
uint32_t teqi(code_info *code, uint32_t src1, uint32_t immed);
uint32_t teq_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc);
uint32_t teqi_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc);
uint32_t cmp(code_info *code, uint32_t src1, uint32_t src2);
uint32_t cmpi(code_info *code, uint32_t src1, uint32_t immed);
uint32_t cmp_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc);
uint32_t cmpi_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc);
uint32_t cmn(code_info *code, uint32_t src1, uint32_t src2);
uint32_t cmni(code_info *code, uint32_t src1, uint32_t immed);
uint32_t cmn_cc(code_info *code, uint32_t src1, uint32_t src2, uint32_t cc);
uint32_t cmni_cc(code_info *code, uint32_t src1, uint32_t immed, uint32_t cc);
uint32_t orr(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t orri(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t orr_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t orri_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t mov(code_info *code, uint32_t dst, uint32_t src2, uint32_t set_cond);
uint32_t movi(code_info *code, uint32_t dst, uint32_t immed, uint32_t set_cond);
uint32_t mov_cc(code_info *code, uint32_t dst, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t movi_cc(code_info *code, uint32_t dst, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t bic(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t set_cond);
uint32_t bici(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t set_cond);
uint32_t bic_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t bici_cc(code_info *code, uint32_t dst, uint32_t src1, uint32_t immed, uint32_t cc, uint32_t set_cond);
uint32_t mvn(code_info *code, uint32_t dst, uint32_t src2, uint32_t set_cond);
uint32_t mvni(code_info *code, uint32_t dst, uint32_t immed, uint32_t set_cond);
uint32_t mvn_cc(code_info *code, uint32_t dst, uint32_t src2, uint32_t cc, uint32_t set_cond);
uint32_t mvni_cc(code_info *code, uint32_t dst, uint32_t immed, uint32_t cc, uint32_t set_cond);

uint32_t b(code_info *code, uint32_t *dst);
uint32_t b_cc(code_info *code, uint32_t *dst, uint32_t cc);
uint32_t bl(code_info *code, uint32_t *dst);
uint32_t bl_cc(code_info *code, uint32_t *dst, uint32_t cc);
uint32_t bx(code_info *code, uint32_t dst);
uint32_t bx_cc(code_info *code, uint32_t dst, uint32_t cc);

uint32_t push(code_info *code, uint32_t reg);
uint32_t push_cc(code_info *code, uint32_t reg, uint32_t cc);
uint32_t pushm(code_info *code, uint32_t reglist);
uint32_t pushm_cc(code_info *code, uint32_t reglist, uint32_t cc);
uint32_t pop(code_info *code, uint32_t reg);
uint32_t pop_cc(code_info *code, uint32_t reg, uint32_t cc);
uint32_t popm(code_info *code, uint32_t reglist);
uint32_t popm_cc(code_info *code, uint32_t reglist, uint32_t cc);
uint32_t ldr_cc(code_info *code, uint32_t dst, uint32_t base, int32_t offset, uint32_t cc);
uint32_t ldr(code_info *code, uint32_t rst, uint32_t base, int32_t offset);
uint32_t str_cc(code_info *code, uint32_t src, uint32_t base, int32_t offset, uint32_t cc);
uint32_t str(code_info *code, uint32_t src, uint32_t base, int32_t offset);

#endif //GEN_ARM_H_
