#ifndef Z80_TO_X86_H_
#define Z80_TO_X86_H_
#include "z80inst.h"
#include "x86_backend.h"

#define ZNUM_MEM_AREAS 4

enum {
	ZF_C = 0,
	ZF_N,
	ZF_PV,
	ZF_H,
	ZF_Z,
	ZF_S,
	ZF_NUM
};

typedef struct {
	uint32_t flags;
	int8_t   regs[Z80_UNUSED];
} x86_z80_options;

typedef struct {
	void *            native_pc;
	uint16_t          sp;
	uint8_t           flags[ZF_NUM];
	uint16_t          bank_reg;
	uint8_t           regs[Z80_A+1];
	uint8_t           alt_regs[Z80_A+1];
	uint8_t *         mem_pointers[ZNUM_MEM_AREAS];
	native_map_slot * native_code_map;
	void *            options
	void *            next_context;
} z80_context;

void translate_z80_stream(z80_context * context, uint16_t address);

#endif //Z80_TO_X86_H_

