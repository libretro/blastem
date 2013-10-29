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

#define MAX_NEST 30 //way more than I'll ever need

tern_node * parse_config(char * config_data)
{
	char *state, *curline;
	char *prefix = NULL;
	int nest_level = 0;
	char * prefix_parts[MAX_NEST];
	int line = 1;
	tern_node * head = NULL;
	while ((curline = strtok_r(config_data, "\n", &state)))
	{
		config_data = NULL;
		curline = strip_ws(curline);
		int len = strlen(curline);
		if (!len) {
			continue;
		}
		if (curline[0] == '#') {
			continue;
		}
		if (curline[0] == '}') {
			if (!nest_level) {
				fprintf(stderr, "unexpected } on line %d\n", line);
				exit(1);
			}
			if (prefix) {
				free(prefix);
				prefix = NULL;
			}
			nest_level--;
			curline = strip_ws(curline+1);
		}
		char * end = curline + len - 1;
		if (*end == '{') {
			*end = 0;
			curline = strip_ws(curline);
			prefix_parts[nest_level++] = curline;
			if (prefix) {
				free(prefix);
				prefix = NULL;
			}
		} else {
			if (nest_level && !prefix) {
				prefix = alloc_concat_m(nest_level, prefix_parts);
			}
			char * val = strip_ws(split_keyval(curline));
			char * key = curline;
			if (*key) {
				if (prefix) {
					key = alloc_concat(prefix, key);
				}
				head = tern_insert_ptr(head, key, strdup(val));
				if (prefix) {
					free(key);
				}
			}
		}
	}
	if (prefix) {
		free(prefix);
	}
	return head;
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
	char * config_data = malloc(config_size);
	if (fread(config_data, 1, config_size, config_file) != config_size) {
		goto config_read_fail;
	}
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
	char * home = getenv("HOME");
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

