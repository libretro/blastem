#include <stdint.h>
#include "68kinst.h"

#define NUM_MEM_AREAS 4
#define NATIVE_MAP_CHUNKS (32*1024)
#define NATIVE_CHUNK_SIZE ((16 * 1024 * 1024 / NATIVE_MAP_CHUNKS)/2)
#define INVALID_OFFSET 0xFFFF

typedef struct {
	uint8_t  *base;
	uint16_t *offsets;
} native_map_slot;

typedef struct deferred_addr {
	struct deferred_addr *next;
	uint8_t              *dest;
	uint32_t             address;
} deferred_addr;

typedef struct {
	uint32_t        flags;
	int8_t          dregs[8];
	int8_t          aregs[8];
	native_map_slot *native_code_map;
	deferred_addr   *deferred;
	
} x86_68k_options;

typedef struct {
	uint8_t         flags[5];
	uint8_t         status;
	uint16_t        reserved;
	uint32_t        dregs[8];
	uint32_t        aregs[8];
	uint32_t		target_cycle;
	uint32_t		current_cycle;
	uint16_t        *mem_pointers[NUM_MEM_AREAS];
	void            *next_context;
	uint16_t        value;
	native_map_slot *native_code_map;
	void            *options;
} m68k_context;

uint8_t * translate_m68k(uint8_t * dst, m68kinst * inst, x86_68k_options * opts);
uint8_t * translate_m68k_stream(uint8_t * dst, uint8_t * dst_end, uint32_t address, m68k_context * context);
void start_68k_context(m68k_context * context, uint32_t address);
void init_x86_68k_opts(x86_68k_options * opts);
void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts);
void m68k_reset(m68k_context * context);

