#ifndef GEN_X86_H_
#define GEN_X86_H_

#include <stdint.h>

enum {
	RAX = 0,
	RCX,
	RDX,
	RBX,
	RSP,
	RBP,
	RSI,
	RDI,
	AH,
	CH,
	DH,
	BH,
	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15
} x86_regs;

enum {
	CC_O = 0,
	CC_NO,
	CC_C,
	CC_NC,
	CC_Z,
	CC_NZ,
	CC_BE,
	CC_A,
	CC_S,
	CC_NS,
	CC_P,
	CC_NP,
	CC_L,
	CC_GE,
	CC_LE,
	CC_G
} x86_cc;

enum {
	SZ_B = 0,
	SZ_W,
	SZ_D,
	SZ_Q
} x86_size;

enum {
	MODE_REG_INDIRECT = 0,
	MODE_REG_INDEXED = 4,
	MODE_REG_DISPLACE8 = 0x40,
	MODE_REG_INDEXED_DISPLACE8 = 0x44,
	MODE_REG_DIPSLACE32 = 0x80,
	MODE_REG_INDEXED_DIPSLACE32 = 0x84,
	MODE_REG_DIRECT = 0xC0,
//"phony" mode
	MODE_IMMED = 0xFF
} x86_modes;


uint8_t * rol_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * ror_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * rcl_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * rcr_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * shl_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * shr_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * sar_ir(uint8_t * out, uint8_t val, uint8_t dst, uint8_t size);
uint8_t * rol_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * ror_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * rcl_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * rcr_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * shl_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * shr_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * sar_irdisp8(uint8_t * out, uint8_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * add_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * or_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * xor_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * and_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * sub_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * cmp_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * add_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * or_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * xor_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * and_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * sub_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * cmp_ir(uint8_t * out, int32_t val, uint8_t dst, uint8_t size);
uint8_t * add_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * or_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * xor_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * and_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * sub_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * cmp_irdisp8(uint8_t * out, int32_t val, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * add_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * add_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * or_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * or_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * xor_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * xor_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * and_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * and_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * sub_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * sub_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * cmp_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * cmp_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * mov_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * mov_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * mov_rrind(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_rindr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_ir(uint8_t * out, int64_t val, uint8_t dst, uint8_t size);
uint8_t * mov_irdisp8(uint8_t * out, int32_t val, uint8_t dst, int8_t disp, uint8_t size);
uint8_t * pushf(uint8_t * out);
uint8_t * popf(uint8_t * out);
uint8_t * push_r(uint8_t * out, uint8_t reg);
uint8_t * pop_r(uint8_t * out, uint8_t reg);
uint8_t * setcc_r(uint8_t * out, uint8_t cc, uint8_t dst);
uint8_t * setcc_rind(uint8_t * out, uint8_t cc, uint8_t dst);
uint8_t * jcc(uint8_t * out, uint8_t cc, uint8_t *dest);
uint8_t * jmp(uint8_t * out, uint8_t *dest);
uint8_t * call(uint8_t * out, uint8_t * fun);
uint8_t * retn(uint8_t * out);

#endif //GEN_X86_H_

