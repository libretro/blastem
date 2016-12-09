/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "68kinst.h"
#include "m68k_core.h"
#include "z80_to_x86.h"
#include "mem.h"
#include "vdp.h"
#include "render.h"
#include "genesis.h"
#include "gdb_remote.h"
#include "gst.h"
#include "util.h"
#include "romdb.h"
#include "terminal.h"
#include "arena.h"
#include "config.h"

#define BLASTEM_VERSION "0.4.1"

#ifdef __ANDROID__
#define FULLSCREEN_DEFAULT 1
#else
#define FULLSCREEN_DEFAULT 0
#endif

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
int frame_limit = 0;

tern_node * config;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

int load_smd_rom(long filesize, FILE * f, uint16_t **buffer)
{
	uint8_t block[SMD_BLOCK_SIZE];
	filesize -= SMD_HEADER_SIZE;
	fseek(f, SMD_HEADER_SIZE, SEEK_SET);

	uint16_t *dst = *buffer = malloc(nearest_pow2(filesize));
	int rom_size = filesize;
	while (filesize > 0) {
		fread(block, 1, SMD_BLOCK_SIZE, f);
		for (uint8_t *low = block, *high = (block+SMD_BLOCK_SIZE/2), *end = block+SMD_BLOCK_SIZE; high < end; high++, low++) {
			*(dst++) = *low << 8 | *high;
		}
		filesize -= SMD_BLOCK_SIZE;
	}
	return rom_size;
}

int load_rom(char * filename, uint16_t **dst)
{
	uint8_t header[10];
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	if (sizeof(header) != fread(header, 1, sizeof(header), f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (header[1] == SMD_MAGIC1 && header[8] == SMD_MAGIC2 && header[9] == SMD_MAGIC3) {
		int i;
		for (i = 3; i < 8; i++) {
			if (header[i] != 0) {
				break;
			}
		}
		if (i == 8) {
			if (header[2]) {
				fatal_error("%s is a split SMD ROM which is not currently supported", filename);
			}
			return load_smd_rom(filesize, f, dst);
		}
	}
	*dst = malloc(nearest_pow2(filesize));
	if (filesize != fread(*dst, 1, filesize, f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	fclose(f);
	return filesize;
}



int break_on_sync = 0;
char *save_state_path;





char * save_filename;
genesis_context *genesis;
genesis_context *menu_context;
genesis_context *game_context;
void persist_save()
{
	if (!game_context) {
		return;
	}
	FILE * f = fopen(save_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open %s file %s for writing\n", game_context->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
		return;
	}
	fwrite(game_context->save_storage, 1, game_context->save_size, f);
	fclose(f);
	printf("Saved %s to %s\n", game_context->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
}



char *title;
void update_title(char *rom_name)
{
	if (title) {
		free(title);
		title = NULL;
	}
	title = alloc_concat(rom_name, " - BlastEm");
	render_update_caption(title);
}

void setup_saves(char *fname, rom_info *info, genesis_context *context)
{
	char * barename = basename_no_extension(fname);
	char const * parts[3] = {get_save_dir(), PATH_SEP, barename};
	char *save_dir = alloc_concat_m(3, parts);
	if (!ensure_dir_exists(save_dir)) {
		warning("Failed to create save directory %s\n", save_dir);
	}
	parts[0] = save_dir;
	parts[2] = info->save_type == SAVE_I2C ? "save.eeprom" : "save.sram";
	free(save_filename);
	save_filename = alloc_concat_m(3, parts);
	parts[2] = "quicksave.gst";
	free(save_state_path);
	save_state_path = alloc_concat_m(3, parts);
	context->save_dir = save_dir;
	free(barename);
	if (info->save_type != SAVE_NONE) {
		FILE * f = fopen(save_filename, "rb");
		if (f) {
			uint32_t read = fread(context->save_storage, 1, info->save_size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded %s from %s\n", info->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
			}
		}
		atexit(persist_save);
	}
}

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
	config = load_config();
	int width = -1;
	int height = -1;
	int debug = 0;
	int ym_log = 0;
	int loaded = 0;
	uint8_t force_region = 0;
	char * romfname = NULL;
	FILE *address_log = NULL;
	char * statefile = NULL;
	int rom_size, lock_on_size;
	uint16_t *cart = NULL, *lock_on = NULL;
	uint8_t * debuggerfun = NULL;
	uint8_t fullscreen = FULLSCREEN_DEFAULT, use_gl = 1;
	uint8_t debug_target = 0;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'b':
				i++;
				if (i >= argc) {
					fatal_error("-b must be followed by a frame count\n");
				}
				headless = 1;
				exit_after = atoi(argv[i]);
				break;
			case 'd':
				debuggerfun = (uint8_t *)debugger;
				//allow debugging the menu
				if (argv[i][2] == 'm') {
					debug_target = 1;
				}
				break;
			case 'D':
				gdb_remote_init();
				debuggerfun = (uint8_t *)gdb_debug_enter;
				break;
			case 'f':
				fullscreen = !fullscreen;
				break;
			case 'g':
				use_gl = 0;
				break;
			case 'l':
				address_log = fopen("address.log", "w");
				break;
			case 'v':
				info_message("blastem %s\n", BLASTEM_VERSION);
				return 0;
				break;
			case 'n':
				z80_enabled = 0;
				break;
			case 'r':
				i++;
				if (i >= argc) {
					fatal_error("-r must be followed by region (J, U or E)\n");
				}
				force_region = translate_region_char(toupper(argv[i][0]));
				if (!force_region) {
					fatal_error("'%c' is not a valid region character for the -r option\n", argv[i][0]);
				}
				break;
			case 's':
				i++;
				if (i >= argc) {
					fatal_error("-s must be followed by a savestate filename\n");
				}
				statefile = argv[i];
				break;
			case 't':
				force_no_terminal();
				break;
			case 'y':
				ym_log = 1;
				break;
			case 'o': {
				i++;
				if (i >= argc) {
					fatal_error("-o must be followed by a lock on cartridge filename\n");
				}
				lock_on_size = load_rom(argv[i], &lock_on);
				if (!lock_on_size) {
					fatal_error("Failed to load lock on cartridge %s\n", argv[i]);
				}
				break;
			}
			case 'h':
				info_message(
					"Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n"
					"Options:\n"
					"	-h          Print this help text\n"
					"	-r (J|U|E)  Force region to Japan, US or Europe respectively\n"
					"	-f          Toggles fullscreen mode\n"
					"	-g          Disable OpenGL rendering\n"
					"	-s FILE     Load a GST format savestate from FILE\n"
					"	-o FILE     Load FILE as a lock-on cartridge\n"
					"	-d          Enter debugger on startup\n"
					"	-n          Disable Z80\n"
					"	-v          Display version number and exit\n"
					"	-l          Log 68K code addresses (useful for assemblers)\n"
					"	-y          Log individual YM-2612 channels to WAVE files\n"
				);
				return 0;
			default:
				fatal_error("Unrecognized switch %s\n", argv[i]);
			}
		} else if (!loaded) {
			if (!(rom_size = load_rom(argv[i], &cart))) {
				fatal_error("Failed to open %s for reading\n", argv[i]);
			}
			romfname = argv[i];
			loaded = 1;
		} else if (width < 0) {
			width = atoi(argv[i]);
		} else if (height < 0) {
			height = atoi(argv[i]);
		}
	}
	uint8_t menu = !loaded;
	if (!loaded) {
		//load menu
		romfname = tern_find_path(config, "ui\0rom\0").ptrval;
		if (!romfname) {
			romfname = "menu.bin";
		}
		if (is_absolute_path(romfname)) {
			if (!(rom_size = load_rom(romfname, &cart))) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
		} else {
			long fsize;
			cart = (uint16_t *)read_bundled_file(romfname, &fsize);
			if (!cart) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
			rom_size = nearest_pow2(fsize);
			if (rom_size > fsize) {
				cart = realloc(cart, rom_size);
			}
		}

		loaded = 1;
	}
	
	int def_width = 0;
	char *config_width = tern_find_path(config, "video\0width\0").ptrval;
	if (config_width) {
		def_width = atoi(config_width);
	}
	if (!def_width) {
		def_width = 640;
	}
	width = width < 320 ? def_width : width;
	height = height < 240 ? (width/320) * 240 : height;

	char *config_fullscreen = tern_find_path(config, "video\0fullscreen\0").ptrval;
	if (config_fullscreen && !strcmp("on", config_fullscreen)) {
		fullscreen = !fullscreen;
	}
	if (!headless) {
		render_init(width, height, "BlastEm", fullscreen);
	}

	rom_info info;
	uint32_t ym_opts = (ym_log && !menu) ? YM_OPT_WAVE_LOG : 0;
	genesis = alloc_config_genesis(cart, rom_size, lock_on, lock_on_size, ym_opts, force_region, &info);
	setup_saves(romfname, &info, genesis);
	update_title(info.name);
	if (menu) {
		menu_context = genesis;
	} else {
		genesis->m68k->options->address_log = address_log;
		game_context = genesis;
	}

	set_keybindings(genesis->ports);
	start_genesis(genesis, menu ? NULL : statefile, menu == debug_target ? debuggerfun : NULL);
	for(;;)
	{
		if (genesis->should_exit) {
			break;
		}
		if (menu && menu_context->next_rom) {
			if (game_context) {
				if (game_context->save_type != SAVE_NONE) {
					genesis = game_context;
					persist_save();
					genesis = menu_context;
				}
				//swap to game context arena and mark all allocated pages in it free
				genesis->arena = set_current_arena(game_context->arena);
				mark_all_free();
				free_genesis(game_context);
			} else {
				//start a new arena and save old one in suspended genesis context
				genesis->arena = start_new_arena();
			}
			if (!(rom_size = load_rom(menu_context->next_rom, &cart))) {
				fatal_error("Failed to open %s for reading\n", menu_context->next_rom);
			}
			//allocate new genesis context
			game_context = alloc_config_genesis(cart, rom_size, lock_on, lock_on_size, ym_opts,force_region, &info);
			menu_context->next_context = game_context;
			game_context->next_context = menu_context;
			setup_saves(menu_context->next_rom, &info, game_context);
			update_title(info.name);
			free(menu_context->next_rom);
			menu_context->next_rom = NULL;
			menu = 0;
			genesis = game_context;
			genesis->m68k->options->address_log = address_log;
			map_all_bindings(genesis->ports);
			start_genesis(genesis, statefile, menu == debug_target ? debuggerfun : NULL);
		} else if (menu && game_context) {
			genesis->arena = set_current_arena(game_context->arena);
			genesis = game_context;
			menu = 0;
			map_all_bindings(genesis->ports);
			resume_68k(genesis->m68k);
		} else if (!menu && menu_context) {
			genesis->arena = set_current_arena(menu_context->arena);
			genesis = menu_context;
			menu = 1;
			map_all_bindings(genesis->ports);
			resume_68k(genesis->m68k);
		} else {
			break;
		}
	}

	return 0;
}
