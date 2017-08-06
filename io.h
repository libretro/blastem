/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef IO_H_
#define IO_H_
#include <stdint.h>
#include "tern.h"
#include "romdb.h"
#include "serialize.h"

enum {
	IO_GAMEPAD2,
	IO_GAMEPAD3,
	IO_GAMEPAD6,
	IO_MOUSE,
	IO_SATURN_KEYBOARD,
	IO_XBAND_KEYBOARD,
	IO_MENACER,
	IO_JUSTIFIER,
	IO_SEGA_MULTI,
	IO_EA_MULTI_A,
	IO_EA_MULTI_B,
	IO_SEGA_PARALLEL,
	IO_GENERIC,
	IO_NONE
};

typedef struct {
	union {
		struct {
			uint32_t timeout_cycle;
			uint16_t th_counter;
			uint16_t gamepad_num;
		} pad;
		struct {
			int data_fd;
			int listen_fd;
		} stream;
		struct {
			uint32_t ready_cycle;
			uint16_t last_read_x;
			uint16_t last_read_y;
			uint16_t cur_x;
			uint16_t cur_y;
			uint16_t latched_x;
			uint16_t latched_y;
			uint8_t  tr_counter;
			uint8_t  mouse_num;
		} mouse;
		struct {
			uint16_t events[8];
			uint8_t  read_pos;
			uint8_t  write_pos;
			uint8_t  tr_counter;
			uint8_t  mode;
			uint8_t  cmd;
		} keyboard;
	} device;
	uint8_t  output;
	uint8_t  control;
	uint8_t  input[3];
	uint32_t slow_rise_start[8];
	uint8_t  serial_out;
	uint8_t  serial_in;
	uint8_t  serial_ctrl;
	uint8_t  device_type;
} io_port;

typedef struct {
	io_port	ports[3];
	uint8_t mouse_mode;
	uint8_t mouse_captured;
	uint8_t keyboard_captured;
} sega_io;

#define GAMEPAD_TH0 0
#define GAMEPAD_TH1 1
#define GAMEPAD_EXTRA 2
#define GAMEPAD_NONE 0xF

#define IO_TH0 0
#define IO_TH1 1
#define IO_STATE 2

enum {
	IO_WRITE_PENDING,
	IO_WRITTEN,
	IO_READ_PENDING,
	IO_READ
};

typedef struct genesis_context genesis_context;

void set_keybindings(sega_io *io);
void map_all_bindings(sega_io *io);
void setup_io_devices(tern_node * config, rom_info *rom, sega_io *io);
void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction);
void io_control_write(io_port *port, uint8_t value, uint32_t current_cycle);
void io_data_write(io_port * pad, uint8_t value, uint32_t current_cycle);
uint8_t io_data_read(io_port * pad, uint32_t current_cycle);
void handle_keydown(int keycode, uint8_t scancode);
void handle_keyup(int keycode, uint8_t scancode);
void handle_joydown(int joystick, int button);
void handle_joyup(int joystick, int button);
void handle_joy_dpad(int joystick, int dpad, uint8_t state);
void handle_joy_axis(int joystick, int axis, int16_t value);
void handle_joy_added(int joystick);
void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay);
void handle_mousedown(int mouse, int button);
void handle_mouseup(int mouse, int button);
void io_serialize(io_port *port, serialize_buffer *buf);
void io_deserialize(deserialize_buffer *buf, void *vport);

#endif //IO_H_

