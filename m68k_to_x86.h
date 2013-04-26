#ifndef M68K_TO_X86_H_
#define M68K_TO_X86_H_
#include <stdint.h>
#include <stdio.h>
#include "x86_backend.h"
//#include "68kinst.h"
struct m68kinst;

#define NUM_MEM_AREAS 4
#define NATIVE_MAP_CHUNKS (64*1024)
#define NATIVE_CHUNK_SIZE ((16 * 1024 * 1024 / NATIVE_MAP_CHUNKS)/2)
#define EXTENSION_WORD 0xFFFFFFFE
#define MAX_NATIVE_SIZE 255

#define OPT_NATIVE_CALL_STACK 0x1

typedef struct {
	uint32_t        flags;
	int8_t          dregs[8];
	int8_t          aregs[8];
	native_map_slot *native_code_map;
	deferred_addr   *deferred;
	uint8_t         *cur_code;
	uint8_t         *code_end;
	uint8_t         **ram_inst_sizes;
	FILE            *address_log;
} x86_68k_options;

typedef struct {
	uint8_t         flags[5];
	uint8_t         status;
	uint16_t        reserved;
	uint32_t        dregs[8];
	uint32_t        aregs[9];
	uint32_t		target_cycle; //cycle at which the next synchronization or interrupt occurs
	uint32_t		current_cycle;
	uint32_t        sync_cycle;
	uint32_t        int_cycle;
	uint32_t        int_num;
	uint16_t        *mem_pointers[NUM_MEM_AREAS];
	void            *next_context;
	uint16_t        value;
	
	native_map_slot *native_code_map;
	void            *options;
	uint8_t         ram_code_flags[32/8];
} m68k_context;

uint8_t * translate_m68k(uint8_t * dst, struct m68kinst * inst, x86_68k_options * opts);
uint8_t * translate_m68k_stream(uint32_t address, m68k_context * context);
void start_68k_context(m68k_context * context, uint32_t address);
void init_x86_68k_opts(x86_68k_options * opts);
void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts);
void m68k_reset(m68k_context * context);
void insert_breakpoint(m68k_context * context, uint32_t address, uint8_t * bp_handler);
void remove_breakpoint(m68k_context * context, uint32_t address);
m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context);

#endif //M68K_TO_X86_H_

