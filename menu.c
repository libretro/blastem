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
					if (menu->curpath[len] == '/') {
						menu->curpath[len] = 0;
						break;
					}
				}
			} else {
				char *pieces[] = {menu->curpath, "/", buf};
				menu->curpath = alloc_concat_m(3, pieces);
				free(pieces[0]);
			}
			break;
		}
		case 2: {
			char buf[4096];
			copy_string_from_guest(m68k, dst, buf, sizeof(buf));
			char *pieces[] = {menu->curpath, "/", buf};
			gen->next_rom = alloc_concat_m(3, pieces);
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

	return context;
}
