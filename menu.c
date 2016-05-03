#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "blastem.h"
#include "menu.h"
#include "backend.h"
#include "util.h"
#include "gst.h"
#include "m68k_internal.h" //needed for get_native_address_trans, should be eliminated once handling of PC is cleaned up


uint16_t menu_read_w(uint32_t address, void * context)
{
	//This should return the status of the last request with 0
	//meaning either the request is complete or no request is pending
	//in the current implementation, the operations happen instantly
	//in emulated time so we can always return 0
	return 0;
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
#endif

void * menu_write_w(uint32_t address, void * context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	menu_context *menu = gen->extra;
	if (!menu) {
		gen->extra = menu = calloc(1, sizeof(menu_context));
		menu->curpath = tern_find_path(config, "ui\0initial_path\0").ptrval;
		if (menu->curpath) {
			menu->curpath = strdup(menu->curpath);
		} else {
#ifdef __ANDROID__
			menu->curpath = strdup(get_external_storage_path());
#else
			menu->curpath = strdup(get_home_dir());
#endif
		}
	}
	if (menu->state) {
		uint32_t dst = menu->latch << 16 | value;
		switch (address >> 2)
		{
		case 0: {
			size_t num_entries;
			dir_entry *entries = get_dir_list(menu->curpath, &num_entries);
			if (entries) {
				qsort(entries, num_entries, sizeof(dir_entry), menu_dir_sort);
			} else {
				warning("Failed to open directory %s: %s\n", menu->curpath, strerror(errno));
			}
			uint8_t *dest;
			for (size_t i = 0; i < num_entries; i++)
			{
				dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
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
			//terminate list
			dest = get_native_pointer(dst, (void **)m68k->mem_pointers, &m68k->options->gen);
			if (dest) {
				*dest = dest[1] = 0;
				free_dir_list(entries, num_entries);
			}
			break;
		}
		case 1: {
			char buf[4096];
			copy_string_from_guest(m68k, dst, buf, sizeof(buf));
			if (!strcmp(buf, "..")) {
				size_t len = strlen(menu->curpath);
				while (len > 1) {
					--len;
					if (is_path_sep(menu->curpath[len])) {
						menu->curpath[len] = 0;
						break;
					}
				}
			} else {
				char *tmp = menu->curpath;
				char const *pieces[] = {menu->curpath, PATH_SEP, buf};
				menu->curpath = alloc_concat_m(3, pieces);
				free(tmp);
			}
			break;
		}
		case 2: {
			char buf[4096];
			copy_string_from_guest(m68k, dst, buf, sizeof(buf));
			char const *pieces[] = {menu->curpath, PATH_SEP, buf};
			gen->next_rom = alloc_concat_m(3, pieces);
			m68k->should_return = 1;
			break;
		}
		case 3: {
			switch (dst)
			{
			case 1:
				m68k->should_return = 1;
				gen->should_exit = 1;
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
			if (gen->next_context && gen->next_context->save_dir) {
				char *end = buffer + SAVE_INFO_BUFFER_SIZE;
				char slotfile[] = "slot_0.gst";
				char const * parts[3] = {gen->next_context->save_dir, PATH_SEP, slotfile};
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
						cur += snprintf(cur, end-cur, "Slot %d - EMPTY", i);
					}
					//advance past the null terminator for this entry
					cur++;
				}
				if (cur < end) {
					parts[2] = "quicksave.gst";
					fname = alloc_concat_m(3, parts);
					modtime = get_modification_time(fname);
					free(fname);
					if (modtime) {
						cur += strftime(cur, end-cur, "Quick  - %c", localtime_r(&modtime, &ltime));
					} else if ((end-cur) > strlen("Quick  - EMPTY")){
						cur += strlen(strcpy(cur, "Quick  - EMPTY"));
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
		case 5:
			//save state
			if (gen->next_context) {
				gen->next_context->save_state = dst + 1;
			}
			m68k->should_return = 1;
			break;
		case 6:
			//load state
			if (gen->next_context && gen->next_context->save_dir) {
				char numslotname[] = "slot_0.gst";
				char *slotname;
				if (dst == QUICK_SAVE_SLOT) {
					slotname = "quicksave.gst";
				} else {
					numslotname[5] = '0' + dst;
					slotname = numslotname;
				}
				char const *parts[] = {gen->next_context->save_dir, PATH_SEP, slotname};
				char *gstpath = alloc_concat_m(3, parts);
				uint32_t pc = load_gst(gen->next_context, gstpath);
				free(gstpath);
				if (!pc) {
					break;
				}
				gen->next_context->m68k->resume_pc = get_native_address_trans(gen->next_context->m68k, pc);
			}
			m68k->should_return = 1;
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
	if (m68k->should_return) {
		m68k->target_cycle = m68k->current_cycle;
	}

	return context;
}
