#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"

#define GAME_ID_OFF 0x183
#define GAME_ID_LEN 8
#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)

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

rom_info configure_rom_heuristics(uint8_t *rom)
{
	rom_info info;
	info.name = get_header_name(rom);
	
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
	info.name = strdup(tern_find_ptr_default(entry, "name", "UNKNOWN"));
	return info;
}