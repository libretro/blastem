#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "genesis.h"
#include "menu.h"
#include "backend.h"
#include "util.h"
#include "gst.h"
#include "paths.h"
#include "saves.h"
#include "config.h"

static menu_context *get_menu(genesis_context *gen)
{
	menu_context *menu = gen->extra;
	if (!menu) {
		gen->extra = menu = calloc(1, sizeof(menu_context));
		get_initial_browse_path(&menu->curpath);
	}
	return menu;
}

uint16_t menu_read_w(uint32_t address, void * vcontext)
{
	if ((address >> 1) == 14) {
		m68k_context *context = vcontext;
		menu_context *menu = get_menu(context->system);
		uint16_t value = menu->external_game_load;
		if (value) {
			printf("Read: %X\n", value);
		}
		menu->external_game_load = 0;
		return value;
	} else {
		//This should return the status of the last request with 0
		//meaning either the request is complete or no request is pending
		//in the current implementation, the operations happen instantly
		//in emulated time so we can always return 0
		return 0;
	}
}

void copy_string_from_guest(m68k_context *m68k, uint32_t guest_addr, char *buf, size_t maxchars)
{
	char *cur;
	char *src = NULL;
	for (cur = buf; cur < buf+maxchars; cur+=2, guest_addr+=2, src+=2)
	{
		if (!src || !(guest_addr & 0xFFFF)) {
			//we may have walked off the end of a memory block, get a fresh native pointer
			src = get_native_pointer(guest_addr, (void **)m68k->mem_pointers, &m68k->options->gen);
			if (!src) {
				break;
			}
		}
		*cur = src[1];
		cur[1] = *src;
		if (!*src || !src[1]) {
			break;
		}
	}
	//make sure we terminate the string even if we did not hit a null terminator in the source
	buf[maxchars-1] = 0;
}

void copy_to_guest(m68k_context *m68k, uint32_t guest_addr, char *src, size_t tocopy)
{
	char *dst = NULL;
	for (char *cur = src; cur < src+tocopy; cur+=2, guest_addr+=2, dst+=2)
	{
		if (!dst || !(guest_addr & 0xFFFF)) {
			//we may have walked off the end of a memory block, get a fresh native pointer
			dst = get_native_pointer(guest_addr, (void **)m68k->mem_pointers, &m68k->options->gen);
			if (!dst) {
				break;
			}
		}
		dst[1] = *cur;
		*dst = cur[1];
	}
}

#define SAVE_INFO_BUFFER_SIZE (11*40)

uint32_t copy_dir_entry_to_guest(uint32_t dst, m68k_context *m68k, char *name, uint8_t is_dir)
{
	uint8_t *dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
	if (!dest) {
		return 0;
	}
	*(dest++) = is_dir;
	*(dest++) = 1;
	dst += 2;
	uint8_t term = 0;
	for (char *cpos = name; *cpos; cpos++)
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
	return dst;
}
#ifdef _WIN32
#include <windows.h>
#endif
void * menu_write_w(uint32_t address, void * context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	menu_context *menu = get_menu(gen);
	if (menu->state) {
		uint32_t dst = menu->latch << 16 | value;
		switch (address >> 2)
		{
		case 0: {
			size_t num_entries;
			dir_entry *entries = get_dir_list(menu->curpath, &num_entries);
			if (entries) {
				sort_dir_list(entries, num_entries);
			} else {
				warning("Failed to open directory %s: %s\n", menu->curpath, strerror(errno));
				entries = malloc(sizeof(dir_entry));
				entries->name = strdup("..");
				entries->is_dir = 1;
				num_entries = 1;
			}
			uint32_t num_exts;
			char **ext_list = get_extension_list(config, &num_exts);
			for (size_t i = 0; dst && i < num_entries; i++)
			{
				if (num_exts && !entries[i].is_dir) {
					if (!path_matches_extensions(entries[i].name, ext_list, num_exts)) {
						continue;
					}
				}
				dst = copy_dir_entry_to_guest(dst,  m68k, entries[i].name, entries[i].is_dir);
			}
			free(ext_list);
			//terminate list
			uint8_t *dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
			if (dest) {
				*dest = dest[1] = 0;
			}
			free_dir_list(entries, num_entries);
			break;
		}
		case 1: {
			char buf[4096];
			copy_string_from_guest(m68k, dst, buf, sizeof(buf));
			buf[sizeof(buf)-1] = 0;
			char *tmp = menu->curpath;
			menu->curpath = path_append(tmp, buf);
			free(tmp);
			break;
		}
		case 2:
		case 8: {
			char buf[4096];
			copy_string_from_guest(m68k, dst, buf, sizeof(buf));
			char const *pieces[] = {menu->curpath, PATH_SEP, buf};
			char *selected = alloc_concat_m(3, pieces);
			if ((address >> 2) == 2) {
				gen->header.next_rom = selected;
				m68k->should_return = 1;
			} else {
				lockon_media(selected);
				free(selected);
			}
			break;
		}
		case 3: {
			switch (dst)
			{
			case 1:
				m68k->should_return = 1;
				gen->header.should_exit = 1;
				break;
			case 2:
				m68k->should_return = 1;
				break;
			}
			
			break;
		}
		case 4: {
			char *buffer = malloc(SAVE_INFO_BUFFER_SIZE);
			char *cur = buffer;
			if (gen->header.next_context && gen->header.next_context->save_dir) {
				char *end = buffer + SAVE_INFO_BUFFER_SIZE;
				uint32_t num_slots;
				save_slot_info *slots = get_slot_info(gen->header.next_context, &num_slots);
				for (uint32_t i = 0; i < num_slots; i++)
				{
					size_t desc_len = strlen(slots[i].desc) + 1;//+1 for string terminator
					char *after = cur + desc_len + 1;//+1 for list terminator
					if (after > cur) {
						desc_len -= after - cur;
					}
					memcpy(cur, slots[i].desc, desc_len);
					cur = after;
				}
				*cur = 0;//terminate list
			} else {
				*(cur++) = 0;
				*(cur++) = 0;
			}
			copy_to_guest(m68k, dst, buffer, cur-buffer);
			break;
		}
		case 5:
			//save state
			if (gen->header.next_context) {
				gen->header.next_context->save_state = dst + 1;
			}
			m68k->should_return = 1;
			break;
		case 6:
			//load state
			if (gen->header.next_context && gen->header.next_context->save_dir) {
				if (!gen->header.next_context->load_state(gen->header.next_context, dst)) {
					break;
				}
			}
			m68k->should_return = 1;
			break;
		case 7: 
			//read only port
			break;
		default:
			fprintf(stderr, "WARNING: write to undefined menu port %X\n", address);
		}
		menu->state = 0;
	} else {
		menu->latch = value;
		menu->state = 1;
	}
	if (m68k->should_return) {
		m68k->target_cycle = m68k->current_cycle;
	}

	return context;
}
