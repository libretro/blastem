/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "system.h"
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
#include "menu.h"

#define BLASTEM_VERSION "0.5.2-pre"

#ifdef __ANDROID__
#define FULLSCREEN_DEFAULT 1
#else
#define FULLSCREEN_DEFAULT 0
#endif

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
int frame_limit = 0;
uint8_t use_native_states = 1;

tern_node * config;

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

int load_smd_rom(long filesize, FILE * f, void **buffer)
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

uint32_t load_rom(char * filename, void **dst, system_type *stype)
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
			if (stype) {
				*stype = SYSTEM_GENESIS;
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
system_header *current_system;
system_header *menu_system;
system_header *game_system;
void persist_save()
{
	if (!game_system) {
		return;
	}
	game_system->persist_save(game_system);
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

static char *get_save_dir(system_media *media)
{
	char *savedir_template = tern_find_path(config, "ui\0save_path\0", TVAL_PTR).ptrval;
	if (!savedir_template) {
		savedir_template = "$USERDATA/blastem/$ROMNAME";
	}
	tern_node *vars = tern_insert_ptr(NULL, "ROMNAME", media->name);
	vars = tern_insert_ptr(vars, "HOME", get_home_dir());
	vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
	vars = tern_insert_ptr(vars, "USERDATA", (char *)get_userdata_dir());
	char *save_dir = replace_vars(savedir_template, vars, 1);
	tern_free(vars);
	if (!ensure_dir_exists(save_dir)) {
		warning("Failed to create save directory %s\n", save_dir);
	}
	return save_dir;
}

void setup_saves(system_media *media, rom_info *info, system_header *context)
{
	static uint8_t persist_save_registered;
	char *save_dir = get_save_dir(info->is_save_lock_on ? media->chain : media);
	char const *parts[] = {save_dir, PATH_SEP, info->save_type == SAVE_I2C ? "save.eeprom" : info->save_type == SAVE_NOR ? "save.nor" : "save.sram"};
	free(save_filename);
	save_filename = alloc_concat_m(3, parts);
	if (info->is_save_lock_on) {
		//initial save dir was calculated based on lock-on cartridge because that's where the save device is
		//save directory used for save states should still be located in the normal place
		free(save_dir);
		save_dir = get_save_dir(media);
	}
	if (use_native_states || context->type != SYSTEM_GENESIS) {
		parts[2] = "quicksave.state";
	} else {
		parts[2] = "quicksave.gst";
	}
	free(save_state_path);
	save_state_path = alloc_concat_m(3, parts);
	context->save_dir = save_dir;
	if (info->save_type != SAVE_NONE) {
		context->load_save(context);
		if (!persist_save_registered) {
			atexit(persist_save);
			persist_save_registered = 1;
		}
	}
}

static void on_drag_drop(const char *filename)
{
	if (current_system->next_rom) {
		free(current_system->next_rom);
	}
	current_system->next_rom = strdup(filename);
	current_system->request_exit(current_system);
	if (menu_system && menu_system->type == SYSTEM_GENESIS) {
		genesis_context *gen = (genesis_context *)menu_system;
		if (gen->extra) {
			menu_context *menu = gen->extra;
			menu->external_game_load = 1;
		} else {
			puts("No extra");
		}
	} else {
		puts("no menu");
	}
}

static system_media cart, lock_on;
void reload_media(void)
{
	if (current_system->next_rom) {
		free(current_system->next_rom);
	}
	char const *parts[] = {
		cart.dir, PATH_SEP, cart.name, ".", cart.extension
	};
	char const **start = parts[0] ? parts : parts + 2;
	int num_parts = parts[0] ? 5 : 3;
	if (!parts[4]) {
		num_parts--;
	}
	current_system->next_rom = alloc_concat_m(num_parts, start);
	current_system->request_exit(current_system);
}

void lockon_media(char *lock_on_path)
{
	reload_media();
	cart.chain = &lock_on;
	free(lock_on.dir);
	free(lock_on.name);
	free(lock_on.extension);
	lock_on.dir = path_dirname(lock_on_path);
	lock_on.name = basename_no_extension(lock_on_path);
	lock_on.extension = path_extension(lock_on_path);
	lock_on.size = load_rom(lock_on_path, &lock_on.buffer, NULL);
}

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
	config = load_config();
	int width = -1;
	int height = -1;
	int debug = 0;
	uint32_t opts = 0;
	int loaded = 0;
	system_type stype = SYSTEM_UNKNOWN, force_stype = SYSTEM_UNKNOWN;
	uint8_t force_region = 0;
	char * romfname = NULL;
	char * statefile = NULL;
	debugger_type dtype = DEBUGGER_NATIVE;
	uint8_t start_in_debugger = 0;
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
				start_in_debugger = 1;
				//allow debugging the menu
				if (argv[i][2] == 'm') {
					debug_target = 1;
				}
				break;
			case 'D':
				gdb_remote_init();
				dtype = DEBUGGER_GDB;
				start_in_debugger = 1;
				break;
			case 'f':
				fullscreen = !fullscreen;
				break;
			case 'g':
				use_gl = 0;
				break;
			case 'l':
				opts |= OPT_ADDRESS_LOG;
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
			case 'm':
				i++;
				if (i >= argc) {
					fatal_error("-r must be followed by a machine type (sms, gen or jag)\n");
				}
				if (!strcmp("sms", argv[i])) {
					stype = force_stype = SYSTEM_SMS;
				} else if (!strcmp("gen", argv[i])) {
					stype = force_stype = SYSTEM_GENESIS;
				} else if (!strcmp("jag", argv[i])) {
					stype = force_stype = SYSTEM_JAGUAR;
				} else {
					fatal_error("Unrecognized machine type %s\n", argv[i]);
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
				opts |= YM_OPT_WAVE_LOG;
				break;
			case 'o': {
				i++;
				if (i >= argc) {
					fatal_error("-o must be followed by a lock on cartridge filename\n");
				}
				lock_on.size = load_rom(argv[i], &lock_on.buffer, NULL);
				if (!lock_on.size) {
					fatal_error("Failed to load lock on cartridge %s\n", argv[i]);
				}
				lock_on.name = basename_no_extension(argv[i]);
				lock_on.extension = path_extension(argv[i]);
				cart.chain = &lock_on;
				break;
			}
			case 'h':
				info_message(
					"Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n"
					"Options:\n"
					"	-h          Print this help text\n"
					"	-r (J|U|E)  Force region to Japan, US or Europe respectively\n"
					"	-m MACHINE  Force emulated machine type to MACHINE. Valid values are:\n"
					"                   sms - Sega Master System/Mark III\n"
					"                   gen - Sega Genesis/Megadrive\n"
					"                   jag - Atari Jaguar\n"
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
			if (!(cart.size = load_rom(argv[i], &cart.buffer, stype == SYSTEM_UNKNOWN ? &stype : NULL))) {
				fatal_error("Failed to open %s for reading\n", argv[i]);
			}
			cart.dir = path_dirname(argv[i]);
			cart.name = basename_no_extension(argv[i]);
			cart.extension = path_extension(argv[i]);
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
		romfname = tern_find_path(config, "ui\0rom\0", TVAL_PTR).ptrval;
		if (!romfname) {
			romfname = "menu.bin";
		}
		if (is_absolute_path(romfname)) {
			if (!(cart.size = load_rom(romfname, &cart.buffer, &stype))) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
		} else {
			cart.buffer = (uint16_t *)read_bundled_file(romfname, &cart.size);
			if (!cart.buffer) {
				fatal_error("Failed to open UI ROM %s for reading", romfname);
			}
			uint32_t rom_size = nearest_pow2(cart.size);
			if (rom_size > cart.size) {
				cart.buffer = realloc(cart.buffer, rom_size);
				cart.size = rom_size;
			}
		}
		//force system detection, value on command line is only for games not the menu
		stype = detect_system_type(&cart);
		cart.dir = path_dirname(romfname);
		cart.name = basename_no_extension(romfname);
		cart.extension = path_extension(romfname);
		loaded = 1;
	}
	
	int def_width = 0, def_height = 0;
	char *config_width = tern_find_path(config, "video\0width\0", TVAL_PTR).ptrval;
	if (config_width) {
		def_width = atoi(config_width);
	}
	if (!def_width) {
		def_width = 640;
	}
	char *config_height = tern_find_path(config, "video\0height\0", TVAL_PTR).ptrval;
	if (config_height) {
		def_height = atoi(config_height);
	}
	if (!def_height) {
		def_height = -1;
	}
	width = width < 1 ? def_width : width;
	height = height < 1 ? def_height : height;

	char *config_fullscreen = tern_find_path(config, "video\0fullscreen\0", TVAL_PTR).ptrval;
	if (config_fullscreen && !strcmp("on", config_fullscreen)) {
		fullscreen = !fullscreen;
	}
	if (!headless) {
		render_init(width, height, "BlastEm", fullscreen);
		render_set_drag_drop_handler(on_drag_drop);
	}

	if (stype == SYSTEM_UNKNOWN) {
		stype = detect_system_type(&cart);
	}
	if (stype == SYSTEM_UNKNOWN) {
		fatal_error("Failed to detect system type for %s\n", romfname);
	}
	rom_info info;
	current_system = alloc_config_system(stype, &cart, menu ? 0 : opts, force_region, &info);
	if (!current_system) {
		fatal_error("Failed to configure emulated machine for %s\n", romfname);
	}
	char *state_format = tern_find_path(config, "ui\0state_format\0", TVAL_PTR).ptrval;
	if (state_format && !strcmp(state_format, "gst")) {
		use_native_states = 0;
	} else if (state_format && strcmp(state_format, "native")) {
		warning("%s is not a valid value for the ui.state_format setting. Valid values are gst and native\n", state_format);
	}
	setup_saves(&cart, &info, current_system);
	update_title(info.name);
	if (menu) {
		menu_system = current_system;
	} else {
		game_system = current_system;
	}

	current_system->debugger_type = dtype;
	current_system->enter_debugger = start_in_debugger && menu == debug_target;
	current_system->start_context(current_system,  menu ? NULL : statefile);
	for(;;)
	{
		if (current_system->should_exit) {
			break;
		}
		if (current_system->next_rom) {
			char *next_rom = current_system->next_rom;
			current_system->next_rom = NULL;
			if (game_system) {
				game_system->persist_save(game_system);
				//swap to game context arena and mark all allocated pages in it free
				if (menu) {
					current_system->arena = set_current_arena(game_system->arena);
				}
				mark_all_free();
				game_system->free_context(game_system);
			} else {
				//start a new arena and save old one in suspended genesis context
				current_system->arena = start_new_arena();
			}
			if (!(cart.size = load_rom(next_rom, &cart.buffer, &stype))) {
				fatal_error("Failed to open %s for reading\n", next_rom);
			}
			free(cart.dir);
			free(cart.name);
			free(cart.extension);
			cart.dir = path_dirname(next_rom);
			cart.name = basename_no_extension(next_rom);
			cart.extension = path_extension(next_rom);
			stype = force_stype;
			if (stype == SYSTEM_UNKNOWN) {
				stype = detect_system_type(&cart);
			}
			if (stype == SYSTEM_UNKNOWN) {
				fatal_error("Failed to detect system type for %s\n", next_rom);
			}
			//allocate new system context
			game_system = alloc_config_system(stype, &cart, opts,force_region, &info);
			if (!game_system) {
				fatal_error("Failed to configure emulated machine for %s\n", next_rom);
			}
			if (menu_system) {
				menu_system->next_context = game_system;
			}
			game_system->next_context = menu_system;
			setup_saves(&cart, &info, game_system);
			update_title(info.name);
			free(next_rom);
			menu = 0;
			current_system = game_system;
			current_system->debugger_type = dtype;
			current_system->enter_debugger = start_in_debugger && menu == debug_target;
			current_system->start_context(current_system, statefile);
		} else if (menu && game_system) {
			current_system->arena = set_current_arena(game_system->arena);
			current_system = game_system;
			menu = 0;
			current_system->resume_context(current_system);
		} else if (!menu && menu_system) {
			current_system->arena = set_current_arena(menu_system->arena);
			current_system = menu_system;
			menu = 1;
			current_system->resume_context(current_system);
		} else {
			break;
		}
	}

	return 0;
}
