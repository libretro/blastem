/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef IO_H_
#define IO_H_
#include <stdint.h>

typedef struct {
	uint32_t th_counter;
	uint32_t timeout_cycle;
	uint8_t output;
	uint8_t control;
	uint8_t input[3];
} io_port;

#define GAMEPAD_TH0 0
#define GAMEPAD_TH1 1
#define GAMEPAD_EXTRA 2
#define GAMEPAD_NONE 0xF

void set_keybindings();
void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction);
void io_data_write(io_port * pad, uint8_t value, uint32_t current_cycle);
uint8_t io_data_read(io_port * pad, uint32_t current_cycle);
void handle_keydown(int keycode);
void handle_keyup(int keycode);
void handle_joydown(int joystick, int button);
void handle_joyup(int joystick, int button);
void handle_joy_dpad(int joystick, int dpad, uint8_t state);

#endif //IO_H_

