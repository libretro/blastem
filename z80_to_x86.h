/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef Z80_TO_X86_H_
#define Z80_TO_X86_H_
#include "z80inst.h"
#include "backend.h"

#define ZNUM_MEM_AREAS 4
#define ZMAX_NATIVE_SIZE 128

enum {
	ZF_C = 0,
	ZF_N,
	ZF_PV,
	ZF_H,
	ZF_Z,
	ZF_S,
	ZF_NUM
};

typedef void (*z80_run_fun)(void * context);

typedef struct {
	cpu_options     gen;
	code_ptr        save_context_scratch;
	code_ptr        load_context_scratch;
	code_ptr        native_addr;
	code_ptr        retrans_stub;
	code_ptr        do_sync;
	code_ptr        read_8;
	code_ptr        write_8;
	code_ptr        read_8_noinc;
	code_ptr        write_8_noinc;
	code_ptr        read_16;
	code_ptr        write_16_highfirst;
	code_ptr        write_16_lowfirst;
	code_ptr		read_io;
	code_ptr		write_io;

	uint32_t        flags;
	int8_t          regs[Z80_UNUSED];
	z80_run_fun     run;
} z80_options;

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
	z80_options *     options;
	void *            system;
	uint8_t           ram_code_flags[(8 * 1024)/128/8];
	uint32_t          int_enable_cycle;
	uint16_t          pc;
	uint32_t          int_pulse_start;
	uint32_t          int_pulse_end;
	uint8_t           breakpoint_flags[(16 * 1024)/sizeof(uint8_t)];
	uint8_t *         bp_handler;
	uint8_t *         bp_stub;
	uint8_t *         interp_code[256];
	uint8_t           reset;
	uint8_t           busreq;
	uint8_t           busack;
} z80_context;

void translate_z80_stream(z80_context * context, uint32_t address);
void init_z80_opts(z80_options * options, memmap_chunk const * chunks, uint32_t num_chunks, uint32_t clock_divider);
void init_z80_context(z80_context * context, z80_options * options);
code_ptr z80_get_native_address(z80_context * context, uint32_t address);
code_ptr z80_get_native_address_trans(z80_context * context, uint32_t address);
z80_context * z80_handle_code_write(uint32_t address, z80_context * context);
void z80_reset(z80_context * context);
void zinsert_breakpoint(z80_context * context, uint16_t address, uint8_t * bp_handler);
void zremove_breakpoint(z80_context * context, uint16_t address);
void z80_run(z80_context * context, uint32_t target_cycle);
void z80_assert_reset(z80_context * context, uint32_t cycle);
void z80_clear_reset(z80_context * context, uint32_t cycle);
void z80_assert_busreq(z80_context * context, uint32_t cycle);
void z80_clear_busreq(z80_context * context, uint32_t cycle);
uint8_t z80_get_busack(z80_context * context, uint32_t cycle);
void z80_adjust_cycles(z80_context * context, uint32_t deduction);
//to be provided by system code
void z80_next_int_pulse(z80_context * z_context);

#endif //Z80_TO_X86_H_

