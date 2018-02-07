#include <windows.h>
#include <shlobj.h>
#include "../paths.h"
#include "../util.h"

char *default_font_path(void)
{
	NONCLIENTMETRICSA metrics = {
		.cbSize = sizeof(metrics)
	};
	char *pref_name = NULL;
	if (SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
		pref_name = metrics.lfCaptionFont.lfFaceName;
	}
	char windows[MAX_PATH];
	SHGetFolderPathA(NULL, CSIDL_WINDOWS, NULL, 0, windows);
	char *fonts = path_append(windows, "Fonts");
	size_t num_entries;
	char *preferred = NULL, *tahoma = NULL, *arial = NULL;
	dir_entry *entries = get_dir_list(fonts, &num_entries);
	for (size_t i = 0; i < num_entries; i++)
	{
		if (entries[i].is_dir) {
			continue;
		}
		char *ext = path_extension(entries[i].name);
		if (!ext || strcasecmp(ext, "ttf")) {
			//not a truetype font, ignore
			free(ext);
			continue;
		}
		free(ext);
		char *base = basename_no_extension(entries[i].name);
		if (!strcasecmp(base, pref_name)) {
			preferred = entries[i].name;
			free(base);
			break;
		} else if (!strcasecmp(base, "tahoma")) {
			tahoma = entries[i].name;
		} else if (!strcasecmp(base, "arial")) {
			arial = entries[i].name;
		}
		free(base);
	}
	char *path = NULL;
	if (preferred) {
		path = path_append(fonts, preferred);
	} else if(tahoma) {
		path = path_append(fonts, tahoma);
	} else if(arial) {
		path = path_append(fonts, arial);
	}
	free(fonts);
	free_dir_list(entries, num_entries);
	return path;
}
