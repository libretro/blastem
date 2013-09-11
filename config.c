/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
			linktext = NULL;
		}
	} while (linksize > cursize);
	return linktext;
}

tern_node * load_config(char * expath)
{
	char * linktext;
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
	
	linktext = readlink_alloc("/proc/self/exe");
	if (!linktext) {
		goto link_prob;
	}
	char * cur;
	int linksize = strlen(linktext);
	for(cur = linktext + linksize - 1; cur != linktext; cur--)
	{
		if (*cur == '/') {
			*cur = 0;
			break;
		}
	}
	if (cur == linktext) {
		goto link_prob;
	}
	path = alloc_concat(linktext, "/default.cfg");
	ret = parse_config_file(path);
success:
	return ret;
link_prob:
	if (linktext) {
		free(linktext);
	}
no_proc:
	//TODO: Fall back to using expath if /proc is not available
	fputs("Failed to find a config file in ~/.config/blastem/blastem.cfg or in the blastem executable directory\n", stderr);
	exit(1);
}

