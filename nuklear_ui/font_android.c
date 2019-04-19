#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include "../util.h"
#include "../paths.h"
#include "sfnt.h"

typedef enum {
	STATE_DEFAULT,
	STATE_DECL,
	STATE_COMMENT,
	STATE_TAG,
	STATE_PRE_ATTRIB,
	STATE_ATTRIB,
	STATE_PRE_VALUE,
	STATE_VALUE
} parse_state;

#define DEFAULT_WEIGHT 400

char *default_font_path(void)
{
	//Would probably be better to call into Java for this, but this should do for now
	FILE *f = fopen("/system/etc/fonts.xml", "rb");
	if (!f) {
		return NULL;
	}
	long size = file_size(f);
	char *font_xml = malloc(size+1);
	if (size != fread(font_xml, 1, size, f)) {
		free(font_xml);
		fclose(f);
		return NULL;
	}
	fclose(f);
	font_xml[size] = 0;
	
	char *last_tag = NULL, *last_attrib = NULL, *last_value = NULL;
	uint8_t last_style_was_normal = 0;
	char *capture_best = NULL;
	char *best = NULL;
	int best_weight_diff = INT_MAX;
	int last_weight = INT_MAX;
	parse_state state = STATE_DEFAULT;
	for(char *cur = font_xml; *cur; ++cur) {
		switch (state)
		{
		case STATE_DEFAULT:
			if (*cur == '<' && cur[1]) {
				cur++;
				switch(*cur)
				{
				case '?':
					state = STATE_DECL;
					break;
				case '!':
					if (cur[1] == '-' && cur[2] == '-') {
						state = STATE_COMMENT;
						cur++;
					} else {
						debug_message("Invalid comment\n");
						cur = font_xml + size - 1;
					}
					break;
				default:
					if (capture_best) {
						cur[-1] = 0;
						best = strip_ws(capture_best);
						capture_best = NULL;
						best_weight_diff = abs(last_weight - DEFAULT_WEIGHT);
						debug_message("Found candidate %s with weight %d\n", best, last_weight);
					}
					state = STATE_TAG;
					last_tag = cur;
					last_attrib = NULL;
					last_value = NULL;
					last_weight = INT_MAX;
					break;
				}
			}
			break;
		case STATE_DECL:
			if (*cur == '?' && cur[1] == '>') {
				cur++;
				state = STATE_DEFAULT;
			}
			break;
		case STATE_COMMENT:
			if (*cur == '-' && cur[1] == '-' && cur[2] == '>') {
				cur += 2;
				state = STATE_DEFAULT;
			}
			break;
		case STATE_TAG:
			if (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r') {
				*cur = 0;
				state = STATE_PRE_ATTRIB;
			} else if (*cur == '>') {
				*cur = 0;
				state = STATE_DEFAULT;
			}
			break;
		case STATE_PRE_ATTRIB:
			if (!(*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) {
				if (*cur == '>') {
					state = STATE_DEFAULT;
					if (last_style_was_normal && abs(last_weight - DEFAULT_WEIGHT) < best_weight_diff) {
						capture_best = cur + 1;
					} else if (best && !strcmp("/family", last_tag)) {
						debug_message("found family close tag, stopping search\n");
						cur = font_xml + size - 1;
					}
				} else {
					last_attrib = cur;
					state = STATE_ATTRIB;
				}
			}
			break;
		case STATE_ATTRIB:
			if (*cur == '=') {
				*cur = 0;
				state = STATE_PRE_VALUE;
			} else if (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r') {
				*cur = 0;
			}
			break;
		case STATE_PRE_VALUE:
			if (*cur == '"') {
				state = STATE_VALUE;
				last_value = cur + 1;
			}
			break;
		case STATE_VALUE:
			if (*cur == '"') {
				*cur = 0;
				state = STATE_PRE_ATTRIB;
				if (!strcmp("weight", last_attrib)) {
					last_weight = atoi(last_value);
				} else if (!strcmp("style", last_attrib)) {
					last_style_was_normal = !strcmp("normal", last_value);
				}
			}
			break;
		}
	}
	if (best) {
		best = path_append("/system/fonts", best);
	}
	free(font_xml);
	return best;
}

static uint8_t *try_load_font(char *path, uint32_t *size_out)
{
	debug_message("Trying to load font %s\n", path);
	FILE *f = fopen(path, "rb");
	free(path);
	if (!f) {
		return NULL;
	}
	long size = file_size(f);
	uint8_t *buffer = malloc(size);
	if (size != fread(buffer, 1, size, f)) {
		fclose(f);
		return NULL;
	}
	fclose(f);
	sfnt_container *sfnt = load_sfnt(buffer, size);
	if (!sfnt) {
		free(buffer);
		return NULL;
	}
	return sfnt_flatten(sfnt->tables, size_out);
}

uint8_t *default_font(uint32_t *size_out)
{
	char *path = default_font_path();
	if (!path) {
		goto error;
	}
	uint8_t *ret = try_load_font(path, size_out);
	if (ret) {
		return ret;
	}
error:
	//try some likely suspects if we failed to parse fonts.xml or failed to find the indicated font
	ret = try_load_font("/system/fonts/Roboto-Regular.ttf", size_out);
	if (!ret) {
		ret = try_load_font("/system/fonts/DroidSans.ttf", size_out);
	}
	return ret;
}
