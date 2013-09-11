/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef X86_BACKEND_H_
#define X86_BACKEND_H_

#include <stdint.h>

#define INVALID_OFFSET 0xFFFFFFFF
#define EXTENSION_WORD 0xFFFFFFFE

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


#define MMAP_READ      0x01
#define MMAP_WRITE     0x02
#define MMAP_CODE      0x04
#define MMAP_PTR_IDX   0x08
#define MMAP_ONLY_ODD  0x10
#define MMAP_ONLY_EVEN 0x20
#define MMAP_FUNC_NULL 0x40

typedef uint16_t (*read_16_fun)(uint32_t address, void * context);
typedef uint8_t (*read_8_fun)(uint32_t address, void * context);
typedef void * (*write_16_fun)(uint32_t address, void * context, uint16_t value);
typedef void * (*write_8_fun)(uint32_t address, void * context, uint8_t value);

typedef struct {
	uint32_t     start;
	uint32_t     end;
	uint32_t     mask;
	uint16_t     ptr_index;
	uint16_t     flags;
	void *       buffer;
	read_16_fun  read_16;
	write_16_fun write_16;
	read_8_fun   read_8;
	write_8_fun  write_8;
} memmap_chunk;

typedef uint8_t * (*native_addr_func)(void * context, uint32_t address);

deferred_addr * defer_address(deferred_addr * old_head, uint32_t address, uint8_t *dest);
void remove_deferred_until(deferred_addr **head_ptr, deferred_addr * remove_to);
void process_deferred(deferred_addr ** head_ptr, void * context, native_addr_func get_native);

#endif //X86_BACKEND_H_

