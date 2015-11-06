#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "blastem.h" //for headless global
#include "render.h" //for render_errorbox
#include "util.h"

char * alloc_concat(char * first, char * second)
{
	int flen = strlen(first);
	int slen = strlen(second);
	char * ret = malloc(flen + slen + 1);
	memcpy(ret, first, flen);
	memcpy(ret+flen, second, slen+1);
	return ret;
}

char * alloc_concat_m(int num_parts, char ** parts)
{
	int total = 0;
	for (int i = 0; i < num_parts; i++) {
		total += strlen(parts[i]);
	}
	char * ret = malloc(total + 1);
	*ret = 0;
	for (int i = 0; i < num_parts; i++) {
		strcat(ret, parts[i]);
	}
	return ret;
}

long file_size(FILE * f)
{
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);
	return fsize;
}

char * strip_ws(char * text)
{
	while (*text && (!isprint(*text) || isblank(*text)))
	{
		text++;
	}
	char * ret = text;
	text = ret + strlen(ret) - 1;
	while (text > ret && (!isprint(*text) || isblank(*text)))
	{
		*text = 0;
		text--;
	}
	return ret;
}

char * split_keyval(char * text)
{
	while (*text && !isblank(*text))
	{
		text++;
	}
	if (!*text) {
		return text;
	}
	*text = 0;
	return text+1;
}

uint32_t nearest_pow2(uint32_t val)
{
	uint32_t ret = 1;
	while (ret < val)
	{
		ret = ret << 1;
	}
	return ret;
}

static char * exe_str;

void set_exe_str(char * str)
{
	exe_str = str;
}

void fatal_error(char *format, ...)
{
	va_list args;
	va_start(args, format);
	if (!headless) {
		//take a guess at the final size
		size_t size = strlen(format) * 2;
		char *buf = malloc(size);
		size_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size) {
			actual++;
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		fputs(buf, stderr);
		render_errorbox("Fatal Error", buf);
		free(buf);
	} else {
		vfprintf(stderr, format, args);
	}
	va_end(args);
	exit(1);
}

void warning(char *format, ...)
{
	va_list args;
	va_start(args, format);
#ifndef _WIN32
	if (headless || (isatty(STDERR_FILENO) && isatty(STDIN_FILENO))) {
		vfprintf(stderr, format, args);
	} else {
#endif
		size_t size = strlen(format) * 2;
		char *buf = malloc(size);
		size_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size) {
			actual++;
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		fputs(buf, stderr);
		render_infobox("BlastEm Info", buf);
		free(buf);
#ifndef _WIN32
	}
#endif
	va_end(args);
}

void info_message(char *format, ...)
{
	va_list args;
	va_start(args, format);
#ifndef _WIN32
	if (headless || (isatty(STDOUT_FILENO) && isatty(STDIN_FILENO))) {
		vprintf(format, args);
	} else {
#endif
		size_t size = strlen(format) * 2;
		char *buf = malloc(size);
		size_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size) {
			actual++;
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		fputs(buf, stdout);
		render_infobox("BlastEm Info", buf);
		free(buf);
#ifndef _WIN32
	}
#endif
	va_end(args);
}

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>

char * get_home_dir()
{
	static char path[MAX_PATH];
	SHGetFolderPathA(NULL, CSIDL_PROFILE, NULL, 0, path);
	return path;
}

char * get_exe_dir()
{
	static char path[MAX_PATH];
	HMODULE module = GetModuleHandleA(NULL);
	GetModuleFileNameA(module, path, MAX_PATH);

	int pathsize = strlen(path);
	for(char * cur = path + pathsize - 1; cur != path; cur--)
	{
		if (*cur == '\\') {
			*cur = 0;
			break;
		}
	}
	return path;
}

#else

char * get_home_dir()
{
	return getenv("HOME");
}

char * readlink_alloc(char * path)
{
	char * linktext = NULL;
	ssize_t linksize = 512;
	ssize_t cursize = 0;
	do {
		if (linksize > cursize) {
			cursize = linksize;
			if (linktext) {
				free(linktext);
			}
		}
		linktext = malloc(cursize);
		linksize = readlink(path, linktext, cursize-1);
		if (linksize == -1) {
			perror("readlink");
			free(linktext);
			return NULL;
		}
	} while ((linksize+1) > cursize);
	linktext[linksize] = 0;
	return linktext;
}

char * get_exe_dir()
{
	static char * exe_dir;
	if (!exe_dir) {
		char * cur;
#ifdef HAS_PROC
		char * linktext = readlink_alloc("/proc/self/exe");
		if (!linktext) {
			goto fallback;
		}
		int linksize = strlen(linktext);
		for(cur = linktext + linksize - 1; cur != linktext; cur--)
		{
			if (*cur == '/') {
				*cur = 0;
				break;
			}
		}
		if (cur == linktext) {
			free(linktext);
fallback:
#endif
			if (!exe_str) {
				fputs("/proc/self/exe is not available and set_exe_str was not called!", stderr);
			}
			int pathsize = strlen(exe_str);
			for(cur = exe_str + pathsize - 1; cur != exe_str; cur--)
			{
				if (*cur == '/') {
					exe_dir = malloc(cur-exe_str+1);
					memcpy(exe_dir, exe_str, cur-exe_str);
					exe_dir[cur-exe_str] = 0;
					break;
				}
			}
#ifdef HAS_PROC
		} else {
			exe_dir = linktext;
		}
#endif
	}
	return exe_dir;
}
#include <dirent.h>

dir_entry *get_dir_list(char *path, size_t *numret)
{
	DIR *d = opendir(path);
	if (!d) {
		if (numret) {
			*numret = 0;
		}
		return NULL;
	}
	size_t storage = 64;
	dir_entry *ret = malloc(sizeof(dir_entry) * storage);
	size_t pos = 0;
	struct dirent* entry;
	while (entry = readdir(d))
	{
		if (entry->d_type != DT_REG && entry->d_type != DT_LNK && entry->d_type != DT_DIR) {
			continue;
		}
		if (pos == storage) {
			storage = storage * 2;
			ret = realloc(ret, sizeof(dir_entry) * storage);
		}
		ret[pos].name = strdup(entry->d_name);
		ret[pos++].is_dir = entry->d_type == DT_DIR;
	}
	if (numret) {
		*numret = pos;
	}
	return ret;
}

void free_dir_list(dir_entry *list, size_t numentries)
{
	for (size_t i = 0; i < numentries; i++)
	{
		free(list[i].name);
	}
	free(list);
}

#endif
