#include <windows.h>
#include <shlobj.h>
#include <string.h>
#include "../paths.h"
#include "../util.h"
#include "sfnt.h"

uint8_t *default_font(uint32_t *size_out)
{
	static const char *thin[] = {"Thin", NULL};
	static const char *extra_light[] = {"ExtraLight", "UltraLight", NULL};
	static const char *light[] = {"Light", NULL};
	static const char *regular[] = {"Regular", "Normal", "Book", NULL};
	static const char *medium[] = {"Medium", NULL};
	static const char *semi_bold[] = {"SemiBold", "DemiBold", NULL};
	static const char *bold[] = {"Bold", NULL};
	static const char *extra_bold[] = {"ExtraBold", "UltraBold", NULL};
	static const char *heavy[] = {"Heavy", "Black", NULL};
	static const char **weight_to_subfamilies[] = {
		NULL,
		thin,
		extra_light,
		light,
		regular,
		medium,
		semi_bold,
		bold,
		extra_bold,
		heavy
	};

	NONCLIENTMETRICSA metrics = {
		.cbSize = sizeof(metrics)
	};
	char *pref_name = NULL, *pref_prefix = NULL;
	const char **pref_sub_families;
	if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
		pref_name = metrics.lfMenuFont.lfFaceName;
		int32_t weight = metrics.lfMenuFont.lfWeight / 100;
		if (weight < 1 || weight > 9) {
			weight = 4;
		}
		pref_sub_families = weight_to_subfamilies[weight];
	}
	if (pref_name) {
		uint32_t prefix_len = 0;
		while (pref_name[prefix_len] && pref_name[prefix_len] != ' ')
		{
			prefix_len++;
		}
		pref_prefix = malloc(prefix_len + 1);
		memcpy(pref_prefix, pref_name, prefix_len);
		pref_prefix[prefix_len] = 0;
	}
	sfnt_table *selected = NULL;
	char windows[MAX_PATH];
	SHGetFolderPathA(NULL, CSIDL_WINDOWS, NULL, 0, windows);
	char *fonts = path_append(windows, "Fonts");
	size_t num_entries;
	char *tahoma = NULL, *arial = NULL;
	dir_entry *entries = get_dir_list(fonts, &num_entries);
	char *path = NULL;
	for (size_t i = 0; i < num_entries; i++)
	{
		if (entries[i].is_dir) {
			continue;
		}
		char *ext = path_extension(entries[i].name);
		if (!ext || (strcasecmp(ext, "ttf") && strcasecmp(ext, "ttc") && strcasecmp(ext, "dfont"))) {
			//not a truetype font, ignore
			free(ext);
			continue;
		}
		free(ext);
		char *base = basename_no_extension(entries[i].name);
		if (pref_prefix && !strncasecmp(base, pref_prefix, 6)) {
			path = path_append(fonts, entries[i].name);
			FILE *f = fopen(path, "rb");
			if (f)
			{
				long font_size = file_size(f);
				uint8_t *blob = malloc(font_size);
				if (font_size == fread(blob, 1, font_size, f))
				{
					sfnt_container *sfnt = load_sfnt(blob, font_size);
					if (sfnt) {
						selected = sfnt_subfamily_by_names(sfnt, pref_sub_families);
						if (!selected) {
							sfnt_free(sfnt);
						}
					} else {
						free(blob);
					}
				} else {
					free(blob);
				}
				fclose(f);
			}
			free(path);
			free(base);
			if (selected) {
				printf("Found preferred font in %s\n", entries[i].name);
				break;
			}
		} else if (!strcasecmp(base, "tahoma")) {
			tahoma = entries[i].name;
		} else if (!strcasecmp(base, "arial")) {
			arial = entries[i].name;
		}
		free(base);
	}
	if (!selected) {
		path = NULL;
		if (tahoma) {
			path = path_append(fonts, tahoma);
		} else if (arial) {
			path = path_append(fonts, arial);
		}
		if (path) {
			FILE *f = fopen(path, "rb");
			if (f)
			{
				long font_size = file_size(f);
				uint8_t *blob = malloc(font_size);
				if (font_size == fread(blob, 1, font_size, f))
				{
					sfnt_container *sfnt = load_sfnt(blob, font_size);
					if (sfnt) {
						selected = sfnt->tables;
					} else {
						free(blob);
					}
				}
				fclose(f);
			}
			free(path);
		}
	}
	free(pref_prefix);
	free(fonts);
	free_dir_list(entries, num_entries);
	if (selected) {
		return sfnt_flatten(selected, size_out);
	}
	return NULL;
}
