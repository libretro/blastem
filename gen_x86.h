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

uint8_t * add_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * or_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * xor_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * and_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * sub_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * cmp_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * add_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * or_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * xor_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * and_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * sub_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * cmp_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * add_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * or_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * xor_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * and_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * sub_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * cmp_i32r(uint8_t * out, int32_t val, uint8_t dst);
uint8_t * mov_rr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_rrdisp8(uint8_t * out, uint8_t src, uint8_t dst_base, int8_t disp, uint8_t size);
uint8_t * mov_rdisp8r(uint8_t * out, uint8_t src_base, int8_t disp, uint8_t dst, uint8_t size);
uint8_t * mov_rrind(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_rindr(uint8_t * out, uint8_t src, uint8_t dst, uint8_t size);
uint8_t * mov_i8r(uint8_t * out, uint8_t val, uint8_t dst);
uint8_t * mov_i16r(uint8_t * out, uint16_t val, uint8_t dst);
uint8_t * mov_i32r(uint8_t * out, uint32_t val, uint8_t dst);
uint8_t * pushf(uint8_t * out);
uint8_t * popf(uint8_t * out);
uint8_t * push_r(uint8_t * out, uint8_t reg);
uint8_t * pop_r(uint8_t * out, uint8_t reg);
uint8_t * setcc_r(uint8_t * out, uint8_t cc, uint8_t dst);
uint8_t * setcc_rind(uint8_t * out, uint8_t cc, uint8_t dst);
uint8_t * jcc(uint8_t * out, uint8_t cc, int32_t disp);
uint8_t * call(uint8_t * out, uint8_t * fun);
uint8_t * retn(uint8_t * out);

#endif //GEN_X86_H_

