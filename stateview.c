/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdlib.h>
#include <stdio.h>
#include "vdp.h"
#include "render.h"
#include "util.h"
#include "blastem.h"

//not used, but referenced by the renderer since it handles input
io_port gamepad_1;
io_port gamepad_2;
uint8_t reset = 1;
uint8_t busreq = 0;

//uint16_t ram[RAM_WORDS];
uint8_t z80_ram[Z80_RAM_BYTES];

uint16_t read_dma_value(uint32_t address)
{
	return 0;
}

void ym_data_write(ym2612_context * context, uint8_t value)
{
}

void ym_address_write_part1(ym2612_context * context, uint8_t address)
{
}

void ym_address_write_part2(ym2612_context * context, uint8_t address)
{
}

void handle_keydown(int keycode)
{
}

void handle_keyup(int keycode)
{
}

void handle_joydown(int joystick, int button)
{
}

void handle_joyup(int joystick, int button)
{
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
}

tern_node * config;
int headless = 0;

int main(int argc, char ** argv)
{
	if (argc < 2) {
		fatal_error("Usage: stateview FILENAME\n");
	}
	FILE * state_file = fopen(argv[1], "rb");
	if (!state_file) {
		fatal_error("Failed to open %s\n", argv[1]);
	}
	set_exe_str(argv[0]);
	config = load_config(argv[0]);
	int width = -1;
	int height = -1;
	if (argc > 2) {
		width = atoi(argv[2]);
		if (argc > 3) {
			height = atoi(argv[3]);
		}
	}
	int def_width = 0;
	char *config_width = tern_find_ptr(config, "videowidth");
	if (config_width) {
		def_width = atoi(config_width);
	}
	if (!def_width) {
		def_width = 640;
	}
	width = width < 320 ? def_width : width;
	height = height < 240 ? (width/320) * 240 : height;

	vdp_context context;
	render_init(width, height, "GST State Viewer", 60, 0);
	init_vdp_context(&context, 0);
	vdp_load_gst(&context, state_file);
	vdp_run_to_vblank(&context);
	vdp_print_sprite_table(&context);
	printf("Display %s\n", (context.regs[REG_MODE_2] & DISPLAY_ENABLE) ? "enabled" : "disabled");
    render_context(&context);
    render_wait_quit(&context);
    return 0;
}
