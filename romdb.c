#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"

#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)
#define GAME_ID_OFF 0x183
#define GAME_ID_LEN 8
#define REGION_START 0x1F0

tern_node *load_rom_db()
{
	char *exe_dir = get_exe_dir();
	if (!exe_dir) {
		fputs("Failed to find executable path\n", stderr);
		exit(1);
	}
	char *path = alloc_concat(exe_dir, "/rom.db");
	tern_node *db = parse_config_file(path);
	free(path);
	if (!db) {
		fputs("Failed to load ROM DB\n", stderr);
	}
	return db;
}

char *get_header_name(uint8_t *rom)
{
	uint8_t *last = rom + TITLE_END - 1;
	uint8_t *src = rom + TITLE_START;
	
	while (last > src && (*last <=  0x20 || *last >= 0x80))
	{
		last--;
	}
	if (last == src) {
		//TODO: Use other name field
		return strdup("UNKNOWN");
	} else {
		last++;
		char *ret = malloc(last - (rom + TITLE_START) + 1);
		uint8_t *dst;
		for (dst = ret; src < last; src++)
		{
			if (*src >= 0x20 && *src < 0x80) {
				*(dst++) = *src;
			}
		}
		*dst = 0;
		return ret;
	}
}

char *region_chars = "UB4JEA";
uint8_t region_bits[] = {REGION_U, REGION_U, REGION_U, REGION_J, REGION_E, REGION_E};

uint8_t translate_region_char(uint8_t c)
{
	for (int i = 0; i < sizeof(region_bits); i++)
	{
		if (c == region_chars[i]) {
			return region_bits[i];
		}
	}
	return 0;
}

uint8_t get_header_regions(uint8_t *rom)
{
	uint8_t regions = 0;
	for (int i = 0; i < 3; i++)
	{
		regions |= translate_region_char(rom[REGION_START + i]);
	}
	return regions;
}


rom_info configure_rom_heuristics(uint8_t *rom)
{
	rom_info info;
	info.name = get_header_name(rom);
	info.regions = get_header_regions(rom);
	return info;
}

rom_info configure_rom(tern_node *rom_db, void *vrom)
{
	uint8_t product_id[GAME_ID_LEN+1];
	uint8_t *rom = vrom;
	product_id[GAME_ID_LEN] = 0;
	for (int i = 0; i < GAME_ID_LEN; i++)
	{
		if (rom[GAME_ID_OFF + i] <= ' ') {
			product_id[i] = 0;
			break;
		}
		product_id[i] = rom[GAME_ID_OFF + i];
		
	}
	tern_node * entry = tern_find_prefix(rom_db, product_id);
	if (!entry) {
		return configure_rom_heuristics(rom);
	}
	rom_info info;
	info.name = tern_find_ptr(entry, "name");
	if (info.name) {
		info.name = strdup(info.name);
	} else {
		info.name = get_header_name(rom);
	}
	
	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	if (!info.regions) {
		info.regions = get_header_regions(rom);
	}
	return info;
}
