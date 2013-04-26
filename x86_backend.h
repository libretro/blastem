#ifndef X86_BACKEND_H_
#define X86_BACKEND_H_

#include <stdint.h>

#define INVALID_OFFSET 0xFFFFFFFF

typedef struct {
	int32_t disp;
	uint8_t mode;
	uint8_t base;
	uint8_t index;
	uint8_t cycles;
} x86_ea;

typedef struct {
	uint8_t  *base;
	int32_t  *offsets;
} native_map_slot;

typedef struct deferred_addr {
	struct deferred_addr *next;
	uint8_t              *dest;
	uint32_t             address;
} deferred_addr;

typedef uint8_t * (*native_addr_func)(void * context, uint32_t address);

deferred_addr * defer_address(deferred_addr * old_head, uint32_t address, uint8_t *dest);
void process_deferred(deferred_addr ** head_ptr, void * context, native_addr_func get_native);

#endif //X86_BACKEND_H_

