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
#include "m68k_internal.h" //needed for get_native_address_trans, should be eliminated once handling of PC is cleaned up

static menu_context *persist_path_menu;
static void persist_path(void)
{
	char const *parts[] = {get_userdata_dir(), PATH_SEP, "sticky_path"};
	char *pathfname = alloc_concat_m(3, parts);
	FILE *f = fopen(pathfname, "wb");
	if (f) {
		if (fwrite(persist_path_menu->curpath, 1, strlen(persist_path_menu->curpath), f) != strlen(persist_path_menu->curpath)) {
			warning("Failed to save menu path");
		}
		fclose(f);
	} else {
		warning("Failed to save menu path: Could not open %s for writing\n", pathfname);
		
	}
	free(pathfname);
}

static menu_context *get_menu(genesis_context *gen)
{
	menu_context *menu = gen->extra;
	if (!menu) {
		gen->extra = menu = calloc(1, sizeof(menu_context));
		menu->curpath = NULL;
		char *remember_path = tern_find_path(config, "ui\0remember_path\0", TVAL_PTR).ptrval;
		if (!remember_path || !strcmp("on", remember_path)) {
			char const *parts[] = {get_userdata_dir(), PATH_SEP, "sticky_path"};
			char *pathfname = alloc_concat_m(3, parts);
			FILE *f = fopen(pathfname, "rb");
			if (f) {
				long pathsize = file_size(f);
				if (pathsize > 0) {
					menu->curpath = malloc(pathsize + 1);
					if (fread(menu->curpath, 1, pathsize, f) != pathsize) {
						warning("Error restoring saved menu path");
						free(menu->curpath);
						menu->curpath = NULL;
					} else {
						menu->curpath[pathsize] = 0;
					}
				}
				fclose(f);
			}
			free(pathfname);
			if (!persist_path_menu) {
				atexit(persist_path);
			}
			persist_path_menu = menu;
		}
		if (!menu->curpath) {
			menu->curpath = tern_find_path(config, "ui\0initial_path\0", TVAL_PTR).ptrval;
		}
		if (!menu->curpath){
#ifdef __ANDROID__
			menu->curpath = get_external_storage_path();
#else
			menu->curpath = "$HOME";
#endif
		}
		tern_node *vars = tern_insert_ptr(NULL, "HOME", get_home_dir());
		vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
		menu->curpath = replace_vars(menu->curpath, vars, 1);
		tern_free(vars);
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

int menu_dir_sort(const void *a, const void *b)
{
	const dir_entry *da, *db;
	da = a;
	db = b;
	if (da->is_dir != db->is_dir) {
		return db->is_dir - da->is_dir;
	}
	return strcasecmp(((dir_entry *)a)->name, ((dir_entry *)b)->name);
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

#ifdef __ANDROID__
#include <SDL.h>
#include <jni.h>
char *get_external_storage_path()
{
	static char *ret;
	if (ret) {
		return ret;
	}
	JNIEnv *env = SDL_AndroidGetJNIEnv();
	if ((*env)->PushLocalFrame(env, 8) < 0) {
		return NULL;
	}

	jclass Environment = (*env)->FindClass(env, "android/os/Environment");
	jmethodID getExternalStorageDirectory =
		(*env)->GetStaticMethodID(env, Environment, "getExternalStorageDirectory", "()Ljava/io/File;");
	jobject file = (*env)->CallStaticObjectMethod(env, Environment, getExternalStorageDirectory);
	if (!file) {
		goto cleanup;
	}

	jmethodID getAbsolutePath = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, file),
		"getAbsolutePath", "()Ljava/lang/String;");
	jstring path = (*env)->CallObjectMethod(env, file, getAbsolutePath);

	char const *tmp = (*env)->GetStringUTFChars(env, path, NULL);
	ret = strdup(tmp);
	(*env)->ReleaseStringUTFChars(env, path, tmp);

cleanup:
	(*env)->PopLocalFrame(env, NULL);
	return ret;
}
#endif

#ifdef _WIN32
#define localtime_r(a,b) localtime(a)
//windows inclues seem not to like certain single letter defines from m68k_internal.h
//get rid of them here
#undef X
#undef N
#undef Z
#undef V
#undef C
#include <windows.h>
#endif

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
#ifdef _WIN32
			//handle virtual "drives" directory
			if (menu->curpath[0] == PATH_SEP[0]) {
				char drivestrings[4096];
				if (sizeof(drivestrings) >= GetLogicalDriveStrings(sizeof(drivestrings), drivestrings)) {
					for (char *cur = drivestrings; *cur; cur += strlen(cur) + 1)
					{
						dst = copy_dir_entry_to_guest(dst, m68k, cur, 1);
					}
				}
				//terminate list
				uint8_t *dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
				if (dest) {
					*dest = dest[1] = 0;
				}
				break;
			}
#endif
			size_t num_entries;
			dir_entry *entries = get_dir_list(menu->curpath, &num_entries);
			if (entries) {
				qsort(entries, num_entries, sizeof(dir_entry), menu_dir_sort);
			} else {
				warning("Failed to open directory %s: %s\n", menu->curpath, strerror(errno));
				entries = malloc(sizeof(dir_entry));
				entries->name = strdup("..");
				entries->is_dir = 1;
				num_entries = 1;
			}
#ifdef _WIN32
			if (menu->curpath[1] == ':' && !menu->curpath[2]) {
				//Add fake .. entry to allow navigation to virtual "drives" directory
				dst = copy_dir_entry_to_guest(dst, m68k, "..", 1);
			}
#endif
			char *ext_filter = strdup(tern_find_path_default(config, "ui\0extensions\0", (tern_val){.ptrval = "bin gen md smd sms gg"}, TVAL_PTR).ptrval);
			uint32_t num_exts = 0, ext_storage = 5;
			char **ext_list = malloc(sizeof(char *) * ext_storage);
			char *cur_filter = ext_filter;
			while (*cur_filter)
			{
				if (num_exts == ext_storage) {
					ext_storage *= 2;
					ext_list = realloc(ext_list, sizeof(char *) * ext_storage);
				}
				ext_list[num_exts++] = cur_filter;
				cur_filter = split_keyval(cur_filter);
			}
			for (size_t i = 0; dst && i < num_entries; i++)
			{
				if (num_exts && !entries[i].is_dir) {
					char *ext = path_extension(entries[i].name);
					if (!ext) {
						continue;
					}
					uint32_t extidx;
					for (extidx = 0; extidx < num_exts; extidx++)
					{
						if (!strcasecmp(ext, ext_list[extidx])) {
							break;
						}
					}
					if (extidx == num_exts) {
						continue;
					}
				}
				dst = copy_dir_entry_to_guest(dst,  m68k, entries[i].name, entries[i].is_dir);
			}
			free(ext_filter);
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
			if (!strcmp(buf, "..")) {
#ifdef _WIN32
				if (menu->curpath[1] == ':' && !menu->curpath[2]) {
					menu->curpath[0] = PATH_SEP[0];
					menu->curpath[1] = 0;
					break;
				}
#endif
				size_t len = strlen(menu->curpath);
				while (len > 0) {
					--len;
					if (is_path_sep(menu->curpath[len])) {
						if (!len) {
							//special handling for /
							menu->curpath[len+1] = 0;
						} else {
							menu->curpath[len] = 0;
						}
						break;
					}
				}
			} else {
				char *tmp = menu->curpath;
#ifdef _WIN32
				if (menu->curpath[0] == PATH_SEP[0] && !menu->curpath[1]) {
					menu->curpath = strdup(buf);
				} else
#endif
				if (is_path_sep(menu->curpath[strlen(menu->curpath) - 1])) {
					menu->curpath = alloc_concat(menu->curpath, buf);
				} else {
					char const *pieces[] = {menu->curpath, PATH_SEP, buf};
					menu->curpath = alloc_concat_m(3, pieces);
				}
				free(tmp);
			}
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
				char slotfile[] = "slot_0.state";
				char slotfilegst[] = "slot_0.gst";
				char const * parts[3] = {gen->header.next_context->save_dir, PATH_SEP, slotfile};
				char const * partsgst[3] = {gen->header.next_context->save_dir, PATH_SEP, slotfilegst};
				struct tm ltime;
				char *fname;
				time_t modtime;
				for (int i = 0; i < 10 && cur < end; i++)
				{
					slotfile[5] = i + '0';
					fname = alloc_concat_m(3, parts);
					modtime = get_modification_time(fname);
					free(fname);
					if (modtime) {
						cur += snprintf(cur, end-cur, "Slot %d - ", i);
						cur += strftime(cur, end-cur, "%c", localtime_r(&modtime, &ltime));
						
					} else {
						slotfilegst[5] = i + '0';
						fname = alloc_concat_m(3, partsgst);
						modtime = get_modification_time(fname);
						free(fname);
						if (modtime) {
							cur += snprintf(cur, end-cur, "Slot %d - ", i);
							cur += strftime(cur, end-cur, "%c", localtime_r(&modtime, &ltime));
						} else {
							cur += snprintf(cur, end-cur, "Slot %d - EMPTY", i);
						}
					}
					//advance past the null terminator for this entry
					cur++;
				}
				if (cur < end) {
					parts[2] = "quicksave.state";
					fname = alloc_concat_m(3, parts);
					modtime = get_modification_time(fname);
					free(fname);
					if (modtime) {
						cur += strftime(cur, end-cur, "Quick  - %c", localtime_r(&modtime, &ltime));
					} else {
						parts[2] = "quicksave.gst";
						fname = alloc_concat_m(3, parts);
						modtime = get_modification_time(fname);
						free(fname);
						if (modtime) {
							cur += strftime(cur, end-cur, "Quick  - %c", localtime_r(&modtime, &ltime));
						} else if ((end-cur) > strlen("Quick  - EMPTY")){
							cur += strlen(strcpy(cur, "Quick  - EMPTY"));
						}
					}
					//advance past the null terminator for this entry
					cur++;
					if (cur < end) {
						//terminate the list
						*(cur++) = 0;
					}
				}
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
				}/*
				char numslotname[] = "slot_0.state";
				char *slotname;
				if (dst == QUICK_SAVE_SLOT) {
					slotname = "quicksave.state";
				} else {
					numslotname[5] = '0' + dst;
					slotname = numslotname;
				}
				char const *parts[] = {gen->header.next_context->save_dir, PATH_SEP, slotname};
				char *statepath = alloc_concat_m(3, parts);
				gen->header.next_context->load_state
				genesis_context *next = (genesis_context *)gen->header.next_context;
				deserialize_buffer state;
				uint32_t pc = 0;
				if (load_from_file(&state, statepath)) {
					genesis_deserialize(&state, next);
					free(state.data);
					//HACK
					pc = next->m68k->last_prefetch_address;
				} else {
					strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
					pc = load_gst(next, statepath);
				}
				free(statepath);
				if (!pc) {
					break;
				}
				next->m68k->resume_pc = get_native_address_trans(next->m68k, pc);
				*/
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
