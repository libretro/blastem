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
			fatal_error("unexpected } on line %d\n", *line);
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

tern_node *parse_config(char * config_data)
{
	int line = 1;
	return parse_config_int(&config_data, 0, &line);
}

tern_node *parse_config_file(char *config_path)
{
	tern_node * ret = NULL;
	FILE * config_file = fopen(config_path, "rb");
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

tern_node *parse_bundled_config(char *config_name)
{
	uint32_t confsize;
	char *confdata = read_bundled_file(config_name, &confsize);
	tern_node *ret = NULL;
	if (confdata) {
		confdata[confsize] = 0;
		ret = parse_config(confdata);
		free(confdata);
	}
	return ret;
}

tern_node *load_config()
{
	char const *confdir = get_config_dir();
	char *confpath = NULL;
	tern_node *ret;
	if (confdir) {
		confpath = alloc_concat(confdir, "/blastem.cfg");
		ret = parse_config_file(confpath);
		if (ret) {
			free(confpath);
			return ret;
		}
	}

	ret = parse_bundled_config("default.cfg");
	if (ret) {
		free(confpath);
		return ret;
	}

	if (confpath) {
		fatal_error("Failed to find a config file at %s or in the blastem executable directory\n", confpath);
	} else {
		fatal_error("Failed to find a config file in the BlastEm executable directory and the config directory path could not be determined\n");
	}
	//this will never get reached, but the compiler doesn't know that. Let's make it happy
	return NULL;
}
