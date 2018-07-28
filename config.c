/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include "util.h"
#include "paths.h"
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

static tern_node * parse_config_int(char **state, int started, int *line)
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

typedef struct {
	char     *buf;
	uint32_t capacity;
	uint32_t size;
	uint32_t indent;
} serialize_state;

static void ensure_buf_capacity(uint32_t ensure, serialize_state *state)
{
	if (ensure + state->size > state->capacity) {
		state->capacity = state->capacity * 2;
		state->buf = realloc(state->buf, state->capacity);
	}
}

static void indent(serialize_state *state)
{
	memset(state->buf + state->size, '\t', state->indent);
	state->size += state->indent;
}

static void serialize_config_int(tern_node *config, serialize_state *state);

static void serialize_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	serialize_state *state = data;
	uint32_t keylen = strlen(key);
	uint32_t vallen = 0;
	if (valtype == TVAL_PTR) {
		vallen = strlen(val.ptrval);
	}
	ensure_buf_capacity(state->indent + keylen + 2 + vallen, state);
	state->buf[state->size++] = '\n';
	indent(state);
	memcpy(state->buf + state->size, key, keylen);
	state->size += keylen;
	state->buf[state->size++] = ' ';
	if (valtype == TVAL_PTR) {
		memcpy(state->buf + state->size, val.ptrval, vallen);
		state->size += vallen;
	} else {
		serialize_config_int(val.ptrval, state);
	}
}

static void serialize_config_int(tern_node *config, serialize_state *state)
{
	ensure_buf_capacity(1, state);
	state->buf[state->size++] = '{';
	state->indent++;
	
	tern_foreach(config, serialize_iter, state);

	--state->indent;
	ensure_buf_capacity(2 + state->indent, state);
	state->buf[state->size++] = '\n';
	indent(state);
	state->buf[state->size++] = '}';
}

char *serialize_config(tern_node *config, uint32_t *size_out)
{
	serialize_state state = {
		.size = 0,
		.capacity = 1024,
		.indent = 0
	};
	state.buf = malloc(state.capacity);
	tern_foreach(config, serialize_iter, &state);
	//serialize_config_int(config, &state);
	*size_out = state.size;
	return state.buf;
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

uint8_t serialize_config_file(tern_node *config, char *path)
{
	FILE *f = fopen(path, "w");
	if (!f) {
		return 0;
	}
	uint32_t buf_size;
	char *buffer = serialize_config(config, &buf_size);
	uint8_t ret = buf_size == fwrite(buffer, 1, buf_size, f);
	free(buffer);
	fclose(f);
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

tern_node *load_overrideable_config(char *name, char *bundled_name)
{
	char const *confdir = get_config_dir();
	char *confpath = NULL;
	tern_node *ret;
	if (confdir) {
		confpath = path_append(confdir, name);
		ret = parse_config_file(confpath);
		if (ret) {
			free(confpath);
			return ret;
		}
	}

	ret = parse_bundled_config(bundled_name);
	if (ret) {
		free(confpath);
		return ret;
	}
	return NULL;
}

tern_node *load_config()
{
	char const *confdir = get_config_dir();
	char *confpath = NULL;
	tern_node *ret = load_overrideable_config("blastem.cfg", "default.cfg");
	if (confdir) {
		confpath = path_append(confdir, "blastem.cfg");
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

	if (get_config_dir()) {
		fatal_error("Failed to find a config file at %s or in the blastem executable directory\n", get_config_dir());
	} else {
		fatal_error("Failed to find a config file in the BlastEm executable directory and the config directory path could not be determined\n");
	}
	//this will never get reached, but the compiler doesn't know that. Let's make it happy
	return NULL;
}

void persist_config_at(tern_node *config, char *fname)
{
	char const *confdir = get_config_dir();
	if (!confdir) {
		fatal_error("Failed to locate config file directory\n");
	}
	ensure_dir_exists(confdir);
	char *confpath = path_append(confdir, fname);
	if (!serialize_config_file(config, confpath)) {
		fatal_error("Failed to write config to %s\n", confpath);
	}
	free(confpath);
}

void persist_config(tern_node *config)
{
	persist_config_at(config, "blastem.cfg");
}

char **get_extension_list(tern_node *config, uint32_t *num_exts_out)
{
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
	*num_exts_out = num_exts;
	return ext_list;
}

#define DEFAULT_LOWPASS_CUTOFF 3390
uint32_t get_lowpass_cutoff(tern_node *config)
{
	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0", TVAL_PTR).ptrval;
	return lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : DEFAULT_LOWPASS_CUTOFF;
}
