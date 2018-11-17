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
#include "genesis.h"
#include "config.h"


uint16_t read_dma_value(uint32_t address)
{
	return 0;
}

m68k_context *m68k_handle_code_write(uint32_t address, m68k_context *context)
{
	return NULL;
}

z80_context *z80_handle_code_write(uint32_t address, z80_context *context)
{
	return NULL;
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

void handle_keydown(int keycode, uint8_t scancode)
{
}

void handle_keyup(int keycode, uint8_t scancode)
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

void handle_joy_axis(int joystick, int axis, int16_t value)
{
}

void handle_joy_added(int joystick)
{
}

void handle_mousedown(int mouse, int button)
{
}

void handle_mouseup(int mouse, int button)
{
}

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
}

void controller_add_mappings()
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

	render_init(width, height, "GST State Viewer", 0);
	vdp_context *context = init_vdp_context(0);
	vdp_load_gst(context, state_file);
	vdp_run_to_vblank(context);
	vdp_print_sprite_table(context);
	printf("Display %s\n", (context->regs[REG_MODE_2] & DISPLAY_ENABLE) ? "enabled" : "disabled");
	if (!(context->regs[REG_MODE_2] & DISPLAY_ENABLE)) {
		puts("Forcing display on");
		vdp_control_port_write(context, 0x8000 | REG_MODE_2 << 8 | context->regs[REG_MODE_2] | DISPLAY_ENABLE);
	}
    render_wait_quit(context);
    return 0;
}
