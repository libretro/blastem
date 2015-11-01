/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef M68K_CORE_H_
#define M68K_CORE_H_
#include <stdint.h>
#include <stdio.h>
#include "backend.h"
//#include "68kinst.h"
struct m68kinst;

#define NUM_MEM_AREAS 8
#define NATIVE_MAP_CHUNKS (64*1024)
#define NATIVE_CHUNK_SIZE ((16 * 1024 * 1024 / NATIVE_MAP_CHUNKS)/2)
#define MAX_NATIVE_SIZE 255

#define M68K_OPT_BROKEN_READ_MODIFY 1

typedef void (*start_fun)(uint8_t * addr, void * context);

typedef struct {
	cpu_options     gen;

	int8_t          dregs[8];
	int8_t          aregs[8];
	int8_t			flag_regs[5];
	FILE            *address_log;
	code_ptr        read_16;
	code_ptr        write_16;
	code_ptr        read_8;
	code_ptr        write_8;
	code_ptr        read_32;
	code_ptr        write_32_lowfirst;
	code_ptr        write_32_highfirst;
	code_ptr        do_sync;
	code_ptr        trap;
	code_ptr        odd_address;
	start_fun       start_context;
	code_ptr        retrans_stub;
	code_ptr        native_addr;
	code_ptr        native_addr_and_sync;
	code_ptr		get_sr;
	code_ptr		set_sr;
	code_ptr		set_ccr;
} m68k_options;

typedef struct {
	uint8_t         flags[5];
	uint8_t         status;
	uint16_t        int_ack;
	uint32_t        dregs[8];
	uint32_t        aregs[9];
	uint32_t		target_cycle; //cycle at which the next synchronization or interrupt occurs
	uint32_t		current_cycle;
	uint32_t        sync_cycle;
	uint32_t        int_cycle;
	uint32_t        int_num;
	uint16_t        *mem_pointers[NUM_MEM_AREAS];
	void            *video_context;

	native_map_slot *native_code_map;
	m68k_options    *options;
	void            *system;
	uint8_t         int_pending;
	uint8_t         ram_code_flags[];
} m68k_context;

void translate_m68k(m68k_options * opts, struct m68kinst * inst);
void translate_m68k_stream(uint32_t address, m68k_context * context);
void start_68k_context(m68k_context * context, uint32_t address);
void init_m68k_opts(m68k_options * opts, memmap_chunk * memmap, uint32_t num_chunks, uint32_t clock_divider);
m68k_context * init_68k_context(m68k_options * opts);
void m68k_reset(m68k_context * context);
void insert_breakpoint(m68k_context * context, uint32_t address, uint8_t * bp_handler);
void remove_breakpoint(m68k_context * context, uint32_t address);
m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context);
uint32_t get_instruction_start(native_map_slot * native_code_map, uint32_t address);

#endif //M68K_CORE_H_

