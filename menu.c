#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "blastem.h"
#include "menu.h"
#include "backend.h"
#include "util.h"


uint16_t menu_read_w(uint32_t address, void * context)
{
	//This should return the status of the last request with 0
	//meaning either the request is complete or no request is pending
	//in the current implementation, the operations happen instantly
	//in emulated time so we can always return 0
	return 0;
}

void * menu_write_w(uint32_t address, void * context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	menu_context *menu = gen->extra;
	if (!menu) {
		gen->extra = menu = calloc(1, sizeof(menu_context));
		menu->curpath = strdup(get_home_dir());
	}
	if (menu->state) {
		uint32_t dst = menu->latch << 16 | value;
		switch (address >> 2)
		{
		case 0: {
			size_t num_entries;
			dir_entry *entries = get_dir_list(menu->curpath, &num_entries);
			for (size_t i = 0; i < num_entries; i++)
			{
				uint8_t *dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
				if (!dest) {
					break;
				}
				*(dest++) = entries[i].is_dir;
				*(dest++) = 1;
				dst += 2;
				uint8_t term = 0;
				for (char *cpos = entries[i].name; *cpos; cpos++)
				{
					dest[1] = *cpos;
					dest[0] = cpos[1];
					if (cpos[1]) {
						cpos++;
					} else {
						term = 1;
					}
					dst += 2;
					if (!(dst & 0xFFFF)) {
						//we may have walked off the end of a memory block, get a fresh native pointer
						dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
						if (!dest) {
							break;
						}
					} else {
						dest += 2;
					}
				}
				if (!term) {
					*(dest++) = 0;
					*dest = 0;
					dst += 2;
				}
			}
			free_dir_list(entries, num_entries);
			break;
		}
		default:
			fprintf(stderr, "WARNING: write to undefined menu port %X\n", address);
		}
		menu->state = 0;
	} else {
		menu->latch = value;
		menu->state = 1;
	}

	return context;
}
