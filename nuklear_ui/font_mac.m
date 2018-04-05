#import <AppKit/AppKit.h>
#include <stddef.h>
#include "../paths.h"
#include "../util.h"
#include "sfnt.h"

static sfnt_table *find_font_in_dir(char *path, char *prefix, const char *ps_name)
{
	size_t num_entries;
	dir_entry *entries = get_dir_list(path, &num_entries);
	size_t prefix_len = prefix ? strlen(prefix) : 0;
	sfnt_table *selected = NULL;
	for (size_t i = 0; i < num_entries && !selected; i++)
	{
		char *ext = path_extension(entries[i].name);
		if (!ext || (strcasecmp(ext, "ttf") && strcasecmp(ext, "ttc") && strcasecmp(ext, "dfont"))) {
			//not a truetype font, ignore
			printf("Skipping %s because of its extension\n", entries[i].name);
			free(ext);
			continue;
		}
		free(ext);
		if (!prefix || !strncasecmp(entries[i].name, prefix, prefix_len)) {
			char *full_path = path_append(path, entries[i].name);
			FILE *f = fopen(full_path, "rb");
			if (f)
			{
				long font_size = file_size(f);
				uint8_t *blob = malloc(font_size);
				if (font_size == fread(blob, 1, font_size, f))
				{
					sfnt_container *sfnt = load_sfnt(blob, font_size);
					if (sfnt) {
						printf("Examining font file %s\n", entries[i].name);
						for (uint8_t j = 0; j < sfnt->num_fonts && !selected; j++)
						{
							char *cur_ps = sfnt_name(sfnt->tables + j, SFNT_POSTSCRIPT);
							printf("\t%s\n", cur_ps);
							if (!strcmp(cur_ps, ps_name)) {
								selected = sfnt->tables + j;
							}
							free(cur_ps);
						}
					} else {
						printf("Failed to load %s as sfnt containern\n", entries[i].name);
						free(blob);
					}
				} else {
					free(blob);
				}
				fclose(f);
			}
			free(full_path);
		}
	}
	return selected;
}

static sfnt_table *find_font_by_ps_name(const char*ps_name, uint8_t exhaustive)
{
	const unsigned char *prefix_start = (const unsigned char *)ps_name;
	while(*prefix_start && (
		*prefix_start < '0' || 
		(*prefix_start > 'z' && *prefix_start <= 0x80) || 
		(*prefix_start > 'Z' && *prefix_start < 'a') || 
		(*prefix_start > '9' && *prefix_start < 'A')
	))
	{
		prefix_start++;
	}
	if (!*prefix_start) {
		//Didn't find a suitable starting character, just start from the beginning
		prefix_start = (const unsigned char *)ps_name;
	}
	const unsigned char *prefix_end = (const unsigned char *)prefix_start + 1;
	while (*prefix_end && *prefix_end >= 'a')
	{
		prefix_end++;
	}
	char *prefix = malloc(prefix_end - prefix_start + 1);
	memcpy(prefix, prefix_start, prefix_end - prefix_start);
	prefix[prefix_end-prefix_start] = 0;
	//check /Library/Fonts first
	sfnt_table *selected = find_font_in_dir("/Library/Fonts", (char *)prefix, ps_name);
	if (!selected) {
		selected = find_font_in_dir("/System/Library/Fonts", (char *)prefix, ps_name);
	}
	if (exhaustive) {
		if (!selected) {
			puts("Check using prefix failed, exhaustively checking fonts");
			selected = find_font_in_dir("/Library/Fonts", NULL, ps_name);
		}
		if (!selected) {
			selected = find_font_in_dir("/System/Library/Fonts", NULL, ps_name);
		}
	}
	free(prefix);
	return selected;
}

uint8_t *default_font(uint32_t *size_out)
{
	NSFont *sys = [NSFont systemFontOfSize:0];
	NSString *name = [sys fontName];
	sfnt_table *selected = find_font_by_ps_name([name UTF8String], 1);
	if (!selected) {
		selected = find_font_by_ps_name(".HelveticaNeueDeskInterface-Regular", 0);
	}
	if (!selected) {
		selected = find_font_by_ps_name(".LucidaGrandeUI", 0);
	}
	
	if (!selected) {
		fatal_error("Failed to find system font %s\n", [name UTF8String]);
	}
	return sfnt_flatten(selected, size_out);
}

