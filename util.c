#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#ifdef __ANDROID__
#include <android/log.h>
#define info_puts(msg) __android_log_write(ANDROID_LOG_INFO, "BlastEm", msg)
#define warning_puts(msg) __android_log_write(ANDROID_LOG_WARN, "BlastEm", msg)
#define fatal_puts(msg) __android_log_write(ANDROID_LOG_FATAL, "BlastEm", msg)

#define info_printf(msg, args) __android_log_vprint(ANDROID_LOG_INFO, "BlastEm", msg, args)
#define warning_printf(msg, args) __android_log_vprint(ANDROID_LOG_WARN, "BlastEm", msg, args)
#define fatal_printf(msg, args) __android_log_vprint(ANDROID_LOG_FATAL, "BlastEm", msg, args)
#else
#define info_puts(msg) fputs(msg, stdout);
#define warning_puts(msg) fputs(msg, stderr);
#define fatal_puts(msg) fputs(msg, stderr);

#define info_printf(msg, args) vprintf(msg, args)
#define warning_printf(msg, args) vfprintf(stderr, msg, args)
#define fatal_printf(msg, args) vfprintf(stderr, msg, args)
#endif

#include "blastem.h" //for headless global
#include "render.h" //for render_errorbox
#include "util.h"

char * alloc_concat(char const * first, char const * second)
{
	int flen = strlen(first);
	int slen = strlen(second);
	char * ret = malloc(flen + slen + 1);
	memcpy(ret, first, flen);
	memcpy(ret+flen, second, slen+1);
	return ret;
}

char * alloc_concat_m(int num_parts, char const ** parts)
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

typedef struct {
	uint32_t start;
	uint32_t end;
	char *value;
} var_pos;

char *replace_vars(char *base, tern_node *vars, uint8_t allow_env)
{
	uint32_t num_vars = 0;
	for (char *cur = base; *cur; ++cur)
	{
		//TODO: Support escaping $ and allow brace syntax
		if (*cur == '$') {
			num_vars++;
		}
	}
	var_pos *positions = calloc(num_vars, sizeof(var_pos));
	num_vars = 0;
	uint8_t in_var = 0;
	uint32_t max_var_len = 0;
	for (char *cur = base; *cur; ++cur)
	{
		if (in_var) {
			if (!(*cur == '_' || isalnum(*cur))) {
				positions[num_vars].end = cur-base;
				if (positions[num_vars].end - positions[num_vars].start > max_var_len) {
					max_var_len = positions[num_vars].end - positions[num_vars].start;
				}
				num_vars++;
				in_var = 0;
			}
		} else if (*cur == '$') {
			positions[num_vars].start = cur-base+1;
			in_var = 1;
		}
	}
	if (in_var) {
		positions[num_vars].end = strlen(base);
		if (positions[num_vars].end - positions[num_vars].start > max_var_len) {
			max_var_len = positions[num_vars].end - positions[num_vars].start;
		}
		num_vars++;
	}
	char *varname = malloc(max_var_len+1);
	uint32_t total_len = 0;
	uint32_t cur = 0;
	for (uint32_t i = 0; i < num_vars; i++)
	{
		total_len += (positions[i].start - 1) - cur;
		cur = positions[i].start;
		memcpy(varname, base + positions[i].start, positions[i].end-positions[i].start);
		varname[positions[i].end-positions[i].start] = 0;
		positions[i].value = tern_find_ptr(vars, varname);
		if (!positions[i].value && allow_env) {
			positions[i].value = getenv(varname);
		}
		if (positions[i].value) {
			total_len += strlen(positions[i].value);
		}
	}
	total_len += strlen(base+cur);
	free(varname);
	char *output = malloc(total_len+1);
	cur = 0;
	char *curout = output;
	for (uint32_t i = 0; i < num_vars; i++)
	{
		if (positions[i].start-1 > cur) {
			memcpy(curout, base + cur, (positions[i].start-1) - cur);
			curout += (positions[i].start-1) - cur;
		}
		if (positions[i].value) {
			strcpy(curout, positions[i].value);
			curout += strlen(curout);
		}
		cur = positions[i].end;
	};
	if (base[cur]) {
		strcpy(curout, base+cur);
	} else {
		*curout = 0;
	}
	free(positions);
	return output;
}

void byteswap_rom(int filesize, uint16_t *cart)
{
	for(uint16_t *cur = cart; cur - cart < filesize/2; ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
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

void bin_to_hex(uint8_t *output, uint8_t *input, uint64_t size)
{
	while (size)
	{
		uint8_t digit = *input >> 4;
		digit += digit > 9 ? 'a' - 0xa : '0';
		*(output++) = digit;
		digit = *(input++) & 0xF;
		digit += digit > 9 ? 'a' - 0xa : '0';
		*(output++) = digit;
		size--;
	}
	*(output++) = 0;
}

char is_path_sep(char c)
{
#ifdef _WIN32
	if (c == '\\') {
		return 1;
	}
#endif
	return c == '/';
}

char is_absolute_path(char *path)
{
#ifdef _WIN32
	if (path[1] == ':' && is_path_sep(path[2]) && isalpha(path[0])) {
		return 1;
	}
#endif
	return is_path_sep(path[0]);
}

char * basename_no_extension(char *path)
{
	char *lastdot = NULL;
	char *lastslash = NULL;
	char *cur;
	for (cur = path; *cur; cur++)
	{
		if (*cur == '.') {
			lastdot = cur;
		} else if (is_path_sep(*cur)) {
			lastslash = cur + 1;
		}
	}
	if (!lastdot) {
		lastdot = cur;
	}
	if (!lastslash) {
		lastslash = path;
	}
	char *barename = malloc(lastdot-lastslash+1);
	memcpy(barename, lastslash, lastdot-lastslash);
	barename[lastdot-lastslash] = 0;
	
	return barename;
}

char *path_extension(char *path)
{
	char *lastdot = NULL;
	char *lastslash = NULL;
	char *cur;
	for (cur = path; *cur; cur++)
	{
		if (*cur == '.') {
			lastdot = cur;
		} else if (is_path_sep(*cur)) {
			lastslash = cur + 1;
		}
	}
	if (!lastdot || (lastslash && lastslash > lastdot)) {
		//no extension
		return NULL;
	}
	return strdup(lastdot+1);
}

char * path_dirname(char *path)
{
	char *lastslash = NULL;
	char *cur;
	for (cur = path; *cur; cur++)
	{
		if (is_path_sep(*cur)) {
			lastslash = cur;
		}
	}
	if (!lastslash) {
		return NULL;
	}
	char *dir = malloc(lastslash-path+1);
	memcpy(dir, path, lastslash-path);
	dir[lastslash-path] = 0;
	
	return dir;
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
		int32_t size = strlen(format) * 2;
		char *buf = malloc(size);
		int32_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size || actual < 0) {
			if (actual < 0) {
				//seems on windows, vsnprintf is returning -1 when the buffer is too small
				//since we don't know the proper size, a generous multiplier will hopefully suffice
				actual = size * 4;
			} else {
				actual++;
			}
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		fatal_puts(buf);
		render_errorbox("Fatal Error", buf);
		free(buf);
	} else {
		fatal_printf(format, args);
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
		warning_printf(format, args);
	} else {
#endif
		int32_t size = strlen(format) * 2;
		char *buf = malloc(size);
		int32_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size || actual < 0) {
			if (actual < 0) {
				//seems on windows, vsnprintf is returning -1 when the buffer is too small
				//since we don't know the proper size, a generous multiplier will hopefully suffice
				actual = size * 4;
			} else {
				actual++;
			}
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		warning_puts(buf);
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
		info_printf(format, args);
	} else {
#endif
		int32_t size = strlen(format) * 2;
		char *buf = malloc(size);
		int32_t actual = vsnprintf(buf, size, format, args);
		if (actual >= size || actual < 0) {
			if (actual < 0) {
				//seems on windows, vsnprintf is returning -1 when the buffer is too small
				//since we don't know the proper size, a generous multiplier will hopefully suffice
				actual = size * 4;
			} else {
				actual++;
			}
			free(buf);
			buf = malloc(actual);
			va_end(args);
			va_start(args, format);
			vsnprintf(buf, actual, format, args);
		}
		info_puts(buf);
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

dir_entry *get_dir_list(char *path, size_t *numret)
{
	HANDLE dir;
	WIN32_FIND_DATA file;
	char *pattern = alloc_concat(path, "/*.*");
	dir = FindFirstFile(pattern, &file);
	free(pattern);
	if (dir == INVALID_HANDLE_VALUE) {
		if (numret) {
			*numret = 0;
		}
		return NULL;
	}
	
	size_t storage = 64;
	dir_entry *ret = malloc(sizeof(dir_entry) * storage);
	size_t pos = 0;
	
	do {
		if (pos == storage) {
			storage = storage * 2;
			ret = realloc(ret, sizeof(dir_entry) * storage);
		}
		ret[pos].name = strdup(file.cFileName);
		ret[pos++].is_dir = (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	} while (FindNextFile(dir, &file));
	
	FindClose(dir);
	if (numret) {
		*numret = pos;
	}
	return ret;
}

time_t get_modification_time(char *path)
{
	HANDLE results;
	WIN32_FIND_DATA file;
	results = FindFirstFile(path, &file);
	if (results == INVALID_HANDLE_VALUE) {
		return 0;
	}
	FindClose(results);
	uint64_t wintime = ((uint64_t)file.ftLastWriteTime.dwHighDateTime) << 32 | file.ftLastWriteTime.dwLowDateTime;
	//convert to seconds
	wintime /= 10000000;
	//adjust for difference between Windows and Unix Epoch
	wintime -= 11644473600LL;
	return (time_t)wintime;
}

int ensure_dir_exists(char *path)
{
	if (CreateDirectory(path, NULL)) {
		return 1;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		return 1;
	}
	if (GetLastError() != ERROR_PATH_NOT_FOUND) {
		warning("CreateDirectory failed with unexpected error code %X\n", GetLastError());
		return 0;
	}
	char *parent = strdup(path);
	//Windows technically supports both native and Unix-style path separators
	//so search for both
	char *sep = strrchr(parent, '\\');
	char *osep = strrchr(parent, '/');
	if (osep && (!sep || osep > sep)) {
		sep = osep;
	}
	if (!sep || sep == parent) {
		//relative path, but for some reason we failed
		return 0;
	}
	*sep = 0;
	if (!ensure_dir_exists(parent)) {
		free(parent);
		return 0;
	}
	free(parent);
	return CreateDirectory(path, NULL);
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
			if (is_path_sep(*cur)) {
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
				if (is_path_sep(*cur)) {
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

time_t get_modification_time(char *path)
{
	struct stat st;
	if (stat(path, &st)) {
		return 0;
	}
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec;
#else
	//Android's Bionic doesn't support the new style so we'll use the old one instead
	return st.st_mtime;
#endif
}

int ensure_dir_exists(char *path)
{
	struct stat st;
	if (stat(path, &st)) {
		if (errno == ENOENT) {
			char *parent = strdup(path);
			char *sep = strrchr(parent, '/');
			if (sep && sep != parent) {
				*sep = 0;
				if (!ensure_dir_exists(parent)) {
					free(parent);
					return 0;
				}
				free(parent);
			}
			return mkdir(path, 0777) == 0;
		} else {
			char buf[80];
			strerror_r(errno, buf, sizeof(buf));
			warning("stat failed with error: %s", buf);
			return 0;
		}
	}
	return S_ISDIR(st.st_mode);
}

#endif

void free_dir_list(dir_entry *list, size_t numentries)
{
	for (size_t i = 0; i < numentries; i++)
	{
		free(list[i].name);
	}
	free(list);
}

#ifdef __ANDROID__

#include <SDL.h>
char *read_bundled_file(char *name, uint32_t *sizeret)
{
	SDL_RWops *rw = SDL_RWFromFile(name, "rb");
	if (!rw) {
		if (sizeret) {
			*sizeret = -1;
		}
		return NULL;
	}

	long fsize = rw->size(rw);
	if (sizeret) {
		*sizeret = fsize;
	}
	char *ret;
	if (fsize) {
		ret = malloc(fsize);
		if (SDL_RWread(rw, ret, 1, fsize) != fsize) {
			free(ret);
			ret = NULL;
		}
	} else {
		ret = NULL;
	}
	SDL_RWclose(rw);
	return ret;
}

char const *get_config_dir()
{
	return SDL_AndroidGetInternalStoragePath();
}

char const *get_userdata_dir()
{
	return SDL_AndroidGetInternalStoragePath();
}

#else

char *read_bundled_file(char *name, uint32_t *sizeret)
{
	char *exe_dir = get_exe_dir();
	if (!exe_dir) {
		if (sizeret) {
			*sizeret = -1;
		}
		return NULL;
	}
	char const *pieces[] = {exe_dir, PATH_SEP, name};
	char *path = alloc_concat_m(3, pieces);
	FILE *f = fopen(path, "rb");
	free(path);
	if (!f) {
		if (sizeret) {
			*sizeret = -1;
		}
		return NULL;
	}

	long fsize = file_size(f);
	if (sizeret) {
		*sizeret = fsize;
	}
	char *ret;
	if (fsize) {
		//reserve an extra byte in case caller wants
		//to null terminate the data
		ret = malloc(fsize+1);
		if (fread(ret, 1, fsize, f) != fsize) {
			free(ret);
			ret = NULL;
		}
	} else {
		ret = NULL;
	}
	return ret;
}


#ifdef _WIN32
char const *get_userdata_dir()
{
	static char path[MAX_PATH];
	if (S_OK == SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, path))
	{
		return path;
	}
	return NULL;
}

char const *get_config_dir()
{
	return get_userdata_dir();
}
#define CONFIG_PREFIX ""
#define SAVE_PREFIX ""

#else

#define CONFIG_PREFIX "/.config"
#define USERDATA_SUFFIX "/.local/share"

char const *get_config_dir()
{
	static char* confdir;
	if (!confdir) {
		char const *base = get_home_dir();
		if (base) {
			confdir = alloc_concat(base, CONFIG_PREFIX PATH_SEP "blastem");
		}
	}
	return confdir;
}

char const *get_userdata_dir()
{
	static char* savedir;
	if (!savedir) {
		char const *base = get_home_dir();
		if (base) {
			savedir = alloc_concat(base, USERDATA_SUFFIX);
		}
	}
	return savedir;
}


#endif



#endif
