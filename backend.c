/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "backend.h"
#include <stdlib.h>

deferred_addr * defer_address(deferred_addr * old_head, uint32_t address, uint8_t *dest)
{
	deferred_addr * new_head = malloc(sizeof(deferred_addr));
	new_head->next = old_head;
	new_head->address = address & 0xFFFFFF;
	new_head->dest = dest;
	return new_head;
}

void remove_deferred_until(deferred_addr **head_ptr, deferred_addr * remove_to)
{
	for(deferred_addr *cur = *head_ptr; cur && cur != remove_to; cur = *head_ptr)
	{
		*head_ptr = cur->next;
		free(cur);
	}
}

void process_deferred(deferred_addr ** head_ptr, void * context, native_addr_func get_native)
{
	deferred_addr * cur = *head_ptr;
	deferred_addr **last_next = head_ptr;
	while(cur)
	{
		code_ptr native = get_native(context, cur->address);//get_native_address(opts->native_code_map, cur->address);
		if (native) {
			int32_t disp = native - (cur->dest + 4);
			code_ptr out = cur->dest;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*(out++) = disp;
			disp >>= 8;
			*out = disp;
			*last_next = cur->next;
			free(cur);
			cur = *last_next;
		} else {
			last_next = &(cur->next);
			cur = cur->next;
		}
	}
}

void * get_native_pointer(uint32_t address, void ** mem_pointers, cpu_options * opts)
{
	memmap_chunk const * memmap = opts->memmap;
	address &= opts->address_mask;
	for (uint32_t chunk = 0; chunk < opts->memmap_chunks; chunk++)
	{
		if (address >= memmap[chunk].start && address < memmap[chunk].end) {
			if (!(memmap[chunk].flags & MMAP_READ)) {
				return NULL;
			}
			uint8_t * base = memmap[chunk].flags & MMAP_PTR_IDX
				? mem_pointers[memmap[chunk].ptr_index]
				: memmap[chunk].buffer;
			if (!base) {
				return NULL;
			}
			return base + (address & memmap[chunk].mask);
		}
	}
	return NULL;
}
