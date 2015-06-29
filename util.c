#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

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

static char * exe_str;

void set_exe_str(char * str)
{
	exe_str = str;
}

#ifdef _WIN32
#include "Shlobj.h"
#include "Windows.h"

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
#ifndef HAS_PROC
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
#ifndef HAS_PROC
		} else {
			exe_dir = linktext;
		}
#endif
	}
	return exe_dir;
}

#endif
