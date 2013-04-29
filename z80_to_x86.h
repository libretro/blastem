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
	uint8_t *       cur_code;
	uint8_t *       code_end;
	deferred_addr * deferred;
	uint32_t        flags;
	int8_t          regs[Z80_UNUSED];
} x86_z80_options;

typedef struct {
	void *            native_pc;
	uint16_t          sp;
	uint8_t           flags[ZF_NUM];
	uint16_t          bank_reg;
	uint8_t           regs[Z80_A+1];
	uint8_t           im;
	uint8_t           alt_regs[Z80_A+1];
	uint32_t          target_cycle;
	uint32_t          current_cycle;
	uint8_t           alt_flags[ZF_NUM];
	uint8_t *         mem_pointers[ZNUM_MEM_AREAS];
	uint8_t           iff1;
	uint8_t           iff2;
	uint16_t          scratch1;
	uint16_t          scratch2;
	void *            extra_pc;
	uint32_t          sync_cycle;
	uint32_t          int_cycle;
	native_map_slot * static_code_map;
	native_map_slot * banked_code_map;
	void *            options;
	void *            next_context;
} z80_context;

void translate_z80_stream(z80_context * context, uint32_t address);
void init_x86_z80_opts(x86_z80_options * options);
void init_z80_context(z80_context * context, x86_z80_options * options);
uint8_t * z80_get_native_address(z80_context * context, uint32_t address);
void z80_run(z80_context * context);
void z80_reset(z80_context * context);

#endif //Z80_TO_X86_H_

