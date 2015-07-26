/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __MINGW64_VERSION_MAJOR
#define MINGW_W64_VERSION (__MINGW64_VERSION_MAJOR * 1000 + __MINGW64_VERSION_MINOR)
#else
#define MINGW_W64_VERSION 0
#endif

#if defined(_WIN32) && (MINGW_W64_VERSION < 3003)
char * strtok_r(char * input, char * sep, char ** state)
{
	if (input) {
		*state = input;
	}
	char * ret = *state;
	while (**state && **state != *sep)
	{
		++*state;
	}
	if (**state)
	{
		**state = 0;
		++*state;
		return ret;
	}
	return NULL;
}
#endif

tern_node * parse_config_int(char **state, int started, int *line)
{
	char *config_data, *curline;
	tern_node * head = NULL;
	config_data = started ? NULL : *state;
	while ((curline = strtok_r(config_data, "\n", state)))
	{
		
		config_data = NULL;
		curline = strip_ws(curline);
		int len = strlen(curline);
		if (!len) {
			*line = *line + 1;
			continue;
		}
		if (curline[0] == '#') {
			*line = *line + 1;
			continue;
		}
		if (curline[0] == '}') {
			if (started) {
				return head;
			}
			fprintf(stderr, "unexpected } on line %d\n", *line);
			exit(1);
		}
		
		char * end = curline + len - 1;
		if (*end == '{') {
			*end = 0;
			curline = strip_ws(curline);
			*line = *line + 1;
			head = tern_insert_node(head, curline, parse_config_int(state, 1, line));
		} else {
			char * val = strip_ws(split_keyval(curline));
			char * key = curline;
			if (*val) {
				head = tern_insert_ptr(head, key, strdup(val));
			} else {
				fprintf(stderr, "Key %s is missing a value on line %d\n", key, *line);
			}
			*line = *line + 1;
		}
	}
	return head;
}

tern_node * parse_config(char * config_data)
{
	int line = 1;
	return parse_config_int(&config_data, 0, &line);
}

tern_node * parse_config_file(char * config_path)
{
	tern_node * ret = NULL;
	FILE * config_file = fopen(config_path, "r");
	if (!config_file) {
		goto open_fail;
	}
	long config_size = file_size(config_file);
	if (!config_size) {
		goto config_empty;
	}
	char * config_data = malloc(config_size+1);
	if (fread(config_data, 1, config_size, config_file) != config_size) {
		goto config_read_fail;
	}
	config_data[config_size] = '\0';

	ret = parse_config(config_data);
config_read_fail:
	free(config_data);
config_empty:
	fclose(config_file);
open_fail:
	return ret;
}

tern_node * load_config()
{
	char * exe_dir;
	char * home = get_home_dir();
	if (!home) {
		goto load_in_app_dir;
	}
	char * path = alloc_concat(home, "/.config/blastem/blastem.cfg");
	tern_node * ret = parse_config_file(path);
	if (ret) {
		goto success;
	}
	free(path);
load_in_app_dir:
	exe_dir = get_exe_dir();
	if (!exe_dir) {
		goto no_config;
	}
	path = alloc_concat(exe_dir, "/default.cfg");
	ret = parse_config_file(path);
	free(path);
success:
	if (ret) {
		return ret;
	}
no_config:
	fputs("Failed to find a config file in ~/.config/blastem/blastem.cfg or in the blastem executable directory\n", stderr);
	exit(1);
}

