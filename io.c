/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#endif
#include <string.h>
#include <stdlib.h>

#include "serialize.h"
#include "io.h"
#include "blastem.h"
#include "genesis.h"
#include "sms.h"
#include "render.h"
#include "util.h"
#include "menu.h"

#define CYCLE_NEVER 0xFFFFFFFF
#define MIN_POLL_INTERVAL 6840

const char * device_type_names[] = {
	"SMS gamepad",
	"3-button gamepad",
	"6-button gamepad",
	"Mega Mouse",
	"Saturn Keyboard",
	"XBAND Keyboard",
	"Menacer",
	"Justifier",
	"Sega multi-tap",
	"EA 4-way Play cable A",
	"EA 4-way Play cable B",
	"Sega Parallel Transfer Board",
	"Generic Device",
	"None"
};

enum {
	BIND_NONE,
	BIND_UI,
	BIND_GAMEPAD1,
	BIND_GAMEPAD2,
	BIND_GAMEPAD3,
	BIND_GAMEPAD4,
	BIND_GAMEPAD5,
	BIND_GAMEPAD6,
	BIND_GAMEPAD7,
	BIND_GAMEPAD8,
	BIND_MOUSE1,
	BIND_MOUSE2,
	BIND_MOUSE3,
	BIND_MOUSE4,
	BIND_MOUSE5,
	BIND_MOUSE6,
	BIND_MOUSE7,
	BIND_MOUSE8
};

typedef enum {
	UI_DEBUG_MODE_INC,
	UI_DEBUG_PAL_INC,
	UI_ENTER_DEBUGGER,
	UI_SAVE_STATE,
	UI_SET_SPEED,
	UI_NEXT_SPEED,
	UI_PREV_SPEED,
	UI_RELEASE_MOUSE,
	UI_TOGGLE_KEYBOARD_CAPTURE,
	UI_TOGGLE_FULLSCREEN,
	UI_SOFT_RESET,
	UI_RELOAD,
	UI_SMS_PAUSE,
	UI_SCREENSHOT,
	UI_EXIT
} ui_action;

typedef enum {
	MOUSE_NONE,     //mouse is ignored
	MOUSE_ABSOLUTE, //really only useful for menu ROM
	MOUSE_RELATIVE, //for full screen
	MOUSE_CAPTURE   //for windowed mode
} mouse_modes;


typedef struct {
	io_port *port;
	uint8_t bind_type;
	uint8_t subtype_a;
	uint8_t subtype_b;
	uint8_t value;
} keybinding;

typedef struct {
	keybinding bindings[4];
	uint8_t    state;
} joydpad;

typedef struct {
	keybinding positive;
	keybinding negative;
	int16_t    value;
} joyaxis;

typedef struct {
	keybinding *buttons;
	joydpad    *dpads;
	joyaxis    *axes;
	uint32_t   num_buttons; //number of entries in the buttons array, not necessarily the number of buttons on the device
	uint32_t   num_dpads;   //number of entries in the dpads array, not necessarily the number of dpads on the device
	uint32_t   num_axes;    //number of entries in the axes array, not necessarily the number of dpads on the device
} joystick;

typedef struct {
	io_port    *motion_port;
	keybinding buttons[MAX_MOUSE_BUTTONS];
	uint8_t    bind_type;
} mousebinding;

#define DEFAULT_JOYBUTTON_ALLOC 12

static sega_io *current_io;
static keybinding *bindings[0x10000];
static joystick joysticks[MAX_JOYSTICKS];
static mousebinding mice[MAX_MICE];
static io_port *keyboard_port;
const uint8_t dpadbits[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};

static void do_bind(keybinding *binding, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	binding->bind_type = bind_type;
	binding->subtype_a = subtype_a;
	binding->subtype_b = subtype_b;
	binding->value = value;
}

void bind_key(int keycode, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	int bucket = keycode >> 15 & 0xFFFF;
	if (!bindings[bucket]) {
		bindings[bucket] = malloc(sizeof(keybinding) * 0x8000);
		memset(bindings[bucket], 0, sizeof(keybinding) * 0x8000);
	}
	int idx = keycode & 0x7FFF;
	do_bind(bindings[bucket] + idx, bind_type, subtype_a, subtype_b, value);
}

void bind_button(int joystick, int button, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joysticks[joystick].buttons) {
		joysticks[joystick].num_buttons = button < DEFAULT_JOYBUTTON_ALLOC ? DEFAULT_JOYBUTTON_ALLOC : button + 1;
		joysticks[joystick].buttons = calloc(joysticks[joystick].num_buttons, sizeof(keybinding));
	} else if (joysticks[joystick].num_buttons <= button) {
		uint32_t old_capacity = joysticks[joystick].num_buttons;
		joysticks[joystick].num_buttons *= 2;
		joysticks[joystick].buttons = realloc(joysticks[joystick].buttons, sizeof(keybinding) * joysticks[joystick].num_buttons);
		memset(joysticks[joystick].buttons + old_capacity, 0, joysticks[joystick].num_buttons - old_capacity);
	}
	do_bind(joysticks[joystick].buttons + button, bind_type, subtype_a, subtype_b, value);
}

void bind_dpad(int joystick, int dpad, int direction, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joysticks[joystick].dpads) {
		//multiple D-pads/hats are not common, so don't allocate any extra space
		joysticks[joystick].dpads = calloc(dpad+1, sizeof(joydpad));
		joysticks[joystick].num_dpads = dpad+1;
	} else if (joysticks[joystick].num_dpads <= dpad) {
		uint32_t old_capacity = joysticks[joystick].num_dpads;
		joysticks[joystick].num_dpads *= 2;
		joysticks[joystick].dpads = realloc(joysticks[joystick].dpads, sizeof(joydpad) * joysticks[joystick].num_dpads);
		memset(joysticks[joystick].dpads + old_capacity, 0, (joysticks[joystick].num_dpads - old_capacity) * sizeof(joydpad));
	}
	for (int i = 0; i < 4; i ++) {
		if (dpadbits[i] & direction) {
			do_bind(joysticks[joystick].dpads[dpad].bindings + i, bind_type, subtype_a, subtype_b, value);
			break;
		}
	}
}

void bind_axis(int joystick, int axis, int positive, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joysticks[joystick].axes) {
		//typical gamepad has 4 axes
		joysticks[joystick].num_axes = axis+1 > 4 ? axis+1 : 4;
		joysticks[joystick].axes = calloc(joysticks[joystick].num_axes, sizeof(joyaxis));
	} else if (joysticks[joystick].num_axes <= axis) {
		uint32_t old_capacity = joysticks[joystick].num_axes;
		joysticks[joystick].num_axes *= 2;
		joysticks[joystick].axes = realloc(joysticks[joystick].axes, sizeof(joyaxis) * joysticks[joystick].num_axes);
		memset(joysticks[joystick].axes + old_capacity, 0, (joysticks[joystick].num_axes - old_capacity) * sizeof(joyaxis));
	}
	if (positive) {
		do_bind(&joysticks[joystick].axes[axis].positive, bind_type, subtype_a, subtype_b, value);
	} else {
		do_bind(&joysticks[joystick].axes[axis].negative, bind_type, subtype_a, subtype_b, value);
	}
}

void reset_joystick_bindings(int joystick)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (joysticks[joystick].buttons) {
		for (int i = 0; i < joysticks[joystick].num_buttons; i++)
		{
			joysticks[joystick].buttons[i].bind_type = BIND_NONE;
		}
	}
	if (joysticks[joystick].dpads) {
		for (int i = 0; i < joysticks[joystick].num_dpads; i++)
		{
			for (int dir = 0; dir < 4; dir++)
			{
				joysticks[joystick].dpads[i].bindings[dir].bind_type = BIND_NONE;
			}
		}
	}
	if (joysticks[joystick].axes) {
		for (int i = 0; i < joysticks[joystick].num_axes; i++)
		{
			joysticks[joystick].axes[i].positive.bind_type = BIND_NONE;
			joysticks[joystick].axes[i].negative.bind_type = BIND_NONE;
		}
	}
}

#define GAMEPAD_BUTTON(PRI_SLOT, SEC_SLOT, VALUE)  (PRI_SLOT << 12 | SEC_SLOT << 8 | VALUE)

#define DPAD_UP      GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_TH1, 0x01)
#define BUTTON_Z     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x01)
#define DPAD_DOWN    GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_TH1, 0x02)
#define BUTTON_Y     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x02)
#define DPAD_LEFT    GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x04)
#define BUTTON_X     GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x04)
#define DPAD_RIGHT   GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x08)
#define BUTTON_MODE  GAMEPAD_BUTTON(GAMEPAD_EXTRA, GAMEPAD_NONE, 0x08)
#define BUTTON_A     GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_NONE, 0x10)
#define BUTTON_B     GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x10)
#define BUTTON_START GAMEPAD_BUTTON(GAMEPAD_TH0, GAMEPAD_NONE, 0x20)
#define BUTTON_C     GAMEPAD_BUTTON(GAMEPAD_TH1, GAMEPAD_NONE, 0x20)

#define PSEUDO_BUTTON_MOTION 0xFFFF
#define MOUSE_LEFT           1
#define MOUSE_RIGHT          2
#define MOUSE_MIDDLE         4
#define MOUSE_START          8

void bind_gamepad(int keycode, int gamepadnum, int button)
{

	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_key(keycode, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_button_gamepad(int joystick, int joybutton, int gamepadnum, int padbutton)
{
	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_button(joystick, joybutton, bind_type, padbutton >> 12, padbutton >> 8 & 0xF, padbutton & 0xFF);
}

void bind_dpad_gamepad(int joystick, int dpad, uint8_t direction, int gamepadnum, int button)
{
	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_dpad(joystick, dpad, direction, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_axis_gamepad(int joystick, int axis, uint8_t positive, int gamepadnum, int button)
{
	if (gamepadnum < 1 || gamepadnum > 8) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_axis(joystick, axis, positive, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_ui(int keycode, ui_action action, uint8_t param)
{
	bind_key(keycode, BIND_UI, action, 0, param);
}

void bind_button_ui(int joystick, int joybutton, ui_action action, uint8_t param)
{
	bind_button(joystick, joybutton, BIND_UI, action, 0, param);
}

void bind_dpad_ui(int joystick, int dpad, uint8_t direction, ui_action action, uint8_t param)
{
	bind_dpad(joystick, dpad, direction, BIND_UI, action, 0, param);
}

void bind_axis_ui(int joystick, int axis, uint8_t positive, ui_action action, uint8_t param)
{
	bind_axis(joystick, axis, positive, BIND_UI, action, 0, param);
}

void handle_binding_down(keybinding * binding)
{
	if (binding->bind_type >= BIND_GAMEPAD1 && binding->bind_type <= BIND_GAMEPAD8)
	{
		if (binding->subtype_a <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_a] |= binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_b] |= binding->value;
		}
	}
	else if (binding->bind_type >= BIND_MOUSE1 && binding->bind_type <= BIND_MOUSE8)
	{
		if (binding->port) {
			binding->port->input[0] |= binding->value;
		}
	}
}

void store_key_event(uint16_t code)
{
	if (keyboard_port && keyboard_port->device.keyboard.write_pos != keyboard_port->device.keyboard.read_pos) {
		//there's room in the buffer, record this event
		keyboard_port->device.keyboard.events[keyboard_port->device.keyboard.write_pos] = code;
		if (keyboard_port->device.keyboard.read_pos == 0xFF) {
			//ring buffer was empty, update read_pos to indicate there is now data
			keyboard_port->device.keyboard.read_pos = keyboard_port->device.keyboard.write_pos;
		}
		keyboard_port->device.keyboard.write_pos = (keyboard_port->device.keyboard.write_pos + 1) & 7;
	}
}

void handle_keydown(int keycode, uint8_t scancode)
{
	int bucket = keycode >> 15 & 0xFFFF;
	int idx = keycode & 0x7FFF;
	keybinding * binding = bindings[bucket] ? bindings[bucket] + idx : NULL;
	if (binding && (!current_io->keyboard_captured || (binding->bind_type == BIND_UI && binding->subtype_a == UI_TOGGLE_KEYBOARD_CAPTURE))) {
		handle_binding_down(binding);
	} else if (current_io->keyboard_captured) {
		store_key_event(scancode);
	}
}

void handle_joydown(int joystick, int button)
{
	if (joystick >= MAX_JOYSTICKS || button >= joysticks[joystick].num_buttons) {
		return;
	}
	keybinding * binding = joysticks[joystick].buttons + button;
	handle_binding_down(binding);
}

void handle_mousedown(int mouse, int button)
{
	if (current_io->mouse_mode == MOUSE_CAPTURE && !current_io->mouse_captured) {
		current_io->mouse_captured = 1;
		render_relative_mouse(1);
		return;
	}
	if (mouse >= MAX_MICE || button > MAX_MOUSE_BUTTONS || button <= 0) {
		return;
	}
	keybinding * binding = mice[mouse].buttons + button - 1;
	handle_binding_down(binding);
}

uint8_t ui_debug_mode = 0;
uint8_t ui_debug_pal = 0;

int current_speed = 0;
int num_speeds = 1;
uint32_t * speeds = NULL;

uint8_t is_keyboard(io_port *port)
{
	return port->device_type == IO_SATURN_KEYBOARD || port->device_type == IO_XBAND_KEYBOARD;
}

uint8_t keyboard_connected(sega_io *io)
{
	return is_keyboard(io->ports) || is_keyboard(io->ports+1) || is_keyboard(io->ports+2);
}

#ifdef _WIN32
#define localtime_r(a,b) localtime(a)
#endif

void handle_binding_up(keybinding * binding)
{
	switch(binding->bind_type)
	{
	case BIND_GAMEPAD1:
	case BIND_GAMEPAD2:
	case BIND_GAMEPAD3:
	case BIND_GAMEPAD4:
	case BIND_GAMEPAD5:
	case BIND_GAMEPAD6:
	case BIND_GAMEPAD7:
	case BIND_GAMEPAD8:
		if (binding->subtype_a <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_a] &= ~binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA && binding->port) {
			binding->port->input[binding->subtype_b] &= ~binding->value;
		}
		break;
	case BIND_MOUSE1:
	case BIND_MOUSE2:
	case BIND_MOUSE3:
	case BIND_MOUSE4:
	case BIND_MOUSE5:
	case BIND_MOUSE6:
	case BIND_MOUSE7:
	case BIND_MOUSE8:
		if (binding->port) {
			binding->port->input[0] &= ~binding->value;
		}
		break;
	case BIND_UI:
		switch (binding->subtype_a)
		{
		case UI_DEBUG_MODE_INC:
			current_system->inc_debug_mode(current_system);
			break;
		case UI_DEBUG_PAL_INC:
			current_system->inc_debug_pal(current_system);
			break;
		case UI_ENTER_DEBUGGER:
			current_system->enter_debugger = 1;
			break;
		case UI_SAVE_STATE:
			current_system->save_state = QUICK_SAVE_SLOT+1;
			break;
		case UI_NEXT_SPEED:
			current_speed++;
			if (current_speed >= num_speeds) {
				current_speed = 0;
			}
			printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
			current_system->set_speed_percent(current_system, speeds[current_speed]);
			break;
		case UI_PREV_SPEED:
			current_speed--;
			if (current_speed < 0) {
				current_speed = num_speeds - 1;
			}
			printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
			current_system->set_speed_percent(current_system, speeds[current_speed]);
			break;
		case UI_SET_SPEED:
			if (binding->value < num_speeds) {
				current_speed = binding->value;
				printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
				current_system->set_speed_percent(current_system, speeds[current_speed]);
			} else {
				printf("Setting speed to %d\n", speeds[current_speed]);
				current_system->set_speed_percent(current_system, speeds[current_speed]);
			}
			break;
		case UI_RELEASE_MOUSE:
			if (current_io->mouse_captured) {
				current_io->mouse_captured = 0;
				render_relative_mouse(0);
			}
			break;
		case UI_TOGGLE_KEYBOARD_CAPTURE:
			if (keyboard_connected(current_io)) {
				current_io->keyboard_captured = !current_io->keyboard_captured;
			}
			break;
		case UI_TOGGLE_FULLSCREEN:
			render_toggle_fullscreen();
			break;
		case UI_SOFT_RESET:
			current_system->soft_reset(current_system);
			break;
		case UI_RELOAD:
			reload_media();
			break;
		case UI_SMS_PAUSE:
			if (current_system->type == SYSTEM_SMS) {
				sms_context *sms = (sms_context *)current_system;
				vdp_pbc_pause(sms->vdp);
			}
			break;
		case UI_SCREENSHOT: {
			char *screenshot_base = tern_find_path(config, "ui\0screenshot_path\0", TVAL_PTR).ptrval;
			if (!screenshot_base) {
				screenshot_base = "$HOME";
			}
			tern_node *vars = tern_insert_ptr(NULL, "HOME", get_home_dir());
			vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
			screenshot_base = replace_vars(screenshot_base, vars, 1);
			tern_free(vars);
			time_t now = time(NULL);
			struct tm local_store;
			char fname_part[256];
			char *template = tern_find_path(config, "ui\0screenshot_template\0", TVAL_PTR).ptrval;
			if (!template) {
				template = "blastem_%c.ppm";
			}
			strftime(fname_part, sizeof(fname_part), template, localtime_r(&now, &local_store));
			char const *parts[] = {screenshot_base, PATH_SEP, fname_part};
			char *path = alloc_concat_m(3, parts);
			free(screenshot_base);
			render_save_screenshot(path);
			break;
		}
		case UI_EXIT:
			current_system->request_exit(current_system);
			if (current_system->type == SYSTEM_GENESIS) {
				genesis_context *gen = (genesis_context *)current_system;
				if (gen->extra) {
					//TODO: More robust mechanism for detecting menu
					menu_context *menu = gen->extra;
					menu->external_game_load = 1;
				}
			}
			break;
		}
		break;
	}
}

void handle_keyup(int keycode, uint8_t scancode)
{
	int bucket = keycode >> 15 & 0xFFFF;
	int idx = keycode & 0x7FFF;
	keybinding * binding = bindings[bucket] ? bindings[bucket] + idx : NULL;
	if (binding && (!current_io->keyboard_captured || (binding->bind_type == BIND_UI && binding->subtype_a == UI_TOGGLE_KEYBOARD_CAPTURE))) {
		handle_binding_up(binding);
	} else if (current_io->keyboard_captured) {
		store_key_event(0xF000 | scancode);
	}
}

void handle_joyup(int joystick, int button)
{
	if (joystick >= MAX_JOYSTICKS  || button >= joysticks[joystick].num_buttons) {
		return;
	}
	keybinding * binding = joysticks[joystick].buttons + button;
	handle_binding_up(binding);
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS  || dpadnum >= joysticks[joystick].num_dpads) {
		return;
	}
	joydpad * dpad = joysticks[joystick].dpads + dpadnum;
	uint8_t newdown = (value ^ dpad->state) & value;
	uint8_t newup = ((~value) ^ (~dpad->state)) & (~value);
	dpad->state = value;
	for (int i = 0; i < 4; i++) {
		if (newdown & dpadbits[i]) {
			handle_binding_down(dpad->bindings + i);
		} else if(newup & dpadbits[i]) {
			handle_binding_up(dpad->bindings + i);
		}
	}
}

#define JOY_AXIS_THRESHOLD 2000

void handle_joy_axis(int joystick, int axis, int16_t value)
{
	if (joystick >= MAX_JOYSTICKS  || axis >= joysticks[joystick].num_axes) {
		return;
	}
	joyaxis *jaxis = joysticks[joystick].axes + axis;
	int old_active = abs(jaxis->value) > JOY_AXIS_THRESHOLD;
	int new_active = abs(value) > JOY_AXIS_THRESHOLD;
	int old_pos = jaxis->value > 0;
	int new_pos = value > 0;
	jaxis->value = value;
	if (old_active && (!new_active || old_pos != new_pos)) {
		//previously activated direction is no longer active
		handle_binding_up(old_pos ? &jaxis->positive : &jaxis->negative);
	}
	if (new_active && (!old_active || old_pos != new_pos)) {
		//previously unactivated direction is now active
		handle_binding_down(new_pos ? &jaxis->positive : &jaxis->negative);
	}
}

void handle_mouseup(int mouse, int button)
{
	if (mouse >= MAX_MICE || button > MAX_MOUSE_BUTTONS || button <= 0) {
		return;
	}
	keybinding * binding = mice[mouse].buttons + button - 1;
	handle_binding_up(binding);
}

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
	if (mouse >= MAX_MICE || !mice[mouse].motion_port) {
		return;
	}
	switch(current_io->mouse_mode)
	{
	case MOUSE_NONE:
		break;
	case MOUSE_ABSOLUTE: {
		float scale_x = (render_emulated_width() * 2.0f) / ((float)render_width());
		float scale_y = (render_emulated_height() * 2.0f) / ((float)render_height());
		int32_t adj_x = x * scale_x + 2 * render_overscan_left() - 2 * BORDER_LEFT;
		int32_t adj_y = y * scale_y + 2 * render_overscan_top() - 4;
		if (adj_x >= 0 && adj_y >= 0) {
			mice[mouse].motion_port->device.mouse.cur_x = adj_x;
			mice[mouse].motion_port->device.mouse.cur_y = adj_y;
		}
		break;
	}
	case MOUSE_RELATIVE: {
		mice[mouse].motion_port->device.mouse.cur_x += deltax;
		mice[mouse].motion_port->device.mouse.cur_y += deltay;
		break;
	}
	case MOUSE_CAPTURE: {
		if (current_io->mouse_captured) {
			mice[mouse].motion_port->device.mouse.cur_x += deltax;
			mice[mouse].motion_port->device.mouse.cur_y += deltay;
		}
		break;
	}
	}
}

int parse_binding_target(char * target, tern_node * padbuttons, tern_node *mousebuttons, int * ui_out, int * padnum_out, int * padbutton_out)
{
	const int gpadslen = strlen("gamepads.");
	const int mouselen = strlen("mouse.");
	if (!strncmp(target, "gamepads.", gpadslen)) {
		if (target[gpadslen] >= '1' && target[gpadslen] <= '8') {
			int padnum = target[gpadslen] - '0';
			int button = tern_find_int(padbuttons, target + gpadslen + 1, 0);
			if (button) {
				*padnum_out = padnum;
				*padbutton_out = button;
				return BIND_GAMEPAD1;
			} else {
				if (target[gpadslen+1]) {
					warning("Gamepad mapping string '%s' refers to an invalid button '%s'\n", target, target + gpadslen + 1);
				} else {
					warning("Gamepad mapping string '%s' has no button component\n", target);
				}
			}
		} else {
			warning("Gamepad mapping string '%s' refers to an invalid gamepad number %c\n", target, target[gpadslen]);
		}
	} else if(!strncmp(target, "mouse.", mouselen)) {
		if (target[mouselen] >= '1' && target[mouselen] <= '8') {
			int mousenum = target[mouselen] - '0';
			int button = tern_find_int(mousebuttons, target + mouselen + 1, 0);
			if (button) {
				*padnum_out = mousenum;
				*padbutton_out = button;
				return BIND_MOUSE1;
			} else {
				if (target[mouselen+1]) {
					warning("Mouse mapping string '%s' refers to an invalid button '%s'\n", target, target + mouselen + 1);
				} else {
					warning("Mouse mapping string '%s' has no button component\n", target);
				}
			}
		} else {
			warning("Gamepad mapping string '%s' refers to an invalid mouse number %c\n", target, target[mouselen]);
		}
	} else if(!strncmp(target, "ui.", strlen("ui."))) {
		*padbutton_out = 0;
		if (!strcmp(target + 3, "vdp_debug_mode")) {
			*ui_out = UI_DEBUG_MODE_INC;
		} else if(!strcmp(target + 3, "vdp_debug_pal")) {
			*ui_out = UI_DEBUG_PAL_INC;
		} else if(!strcmp(target + 3, "enter_debugger")) {
			*ui_out = UI_ENTER_DEBUGGER;
		} else if(!strcmp(target + 3, "save_state")) {
			*ui_out = UI_SAVE_STATE;
		} else if(!strncmp(target + 3, "set_speed.", strlen("set_speed."))) {
			*ui_out = UI_SET_SPEED;
			*padbutton_out = atoi(target + 3 + strlen("set_speed."));
		} else if(!strcmp(target + 3, "next_speed")) {
			*ui_out = UI_NEXT_SPEED;
		} else if(!strcmp(target + 3, "prev_speed")) {
			*ui_out = UI_PREV_SPEED;
		} else if(!strcmp(target + 3, "release_mouse")) {
			*ui_out = UI_RELEASE_MOUSE;
		} else if(!strcmp(target + 3, "toggle_keyboard_captured")) {
			*ui_out = UI_TOGGLE_KEYBOARD_CAPTURE;
		} else if (!strcmp(target + 3, "toggle_fullscreen")) {
			*ui_out = UI_TOGGLE_FULLSCREEN;
		} else if (!strcmp(target + 3, "soft_reset")) {
			*ui_out = UI_SOFT_RESET;
		} else if (!strcmp(target + 3, "reload")) {
			*ui_out = UI_RELOAD;
		} else if (!strcmp(target + 3, "sms_pause")) {
			*ui_out = UI_SMS_PAUSE;
		} else if (!strcmp(target + 3, "screenshot")) {
			*ui_out = UI_SCREENSHOT;
		} else if(!strcmp(target + 3, "exit")) {
			*ui_out = UI_EXIT;
		} else {
			warning("Unreconized UI binding type %s\n", target);
			return 0;
		}
		return BIND_UI;
	} else {
		warning("Unrecognized binding type %s\n", target);
	}
	return 0;
}

void process_keys(tern_node * cur, tern_node * special, tern_node * padbuttons, tern_node *mousebuttons, char * prefix)
{
	char * curstr = NULL;
	int len;
	if (!cur) {
		return;
	}
	char onec[2];
	if (prefix) {
		len = strlen(prefix);
		curstr = malloc(len + 2);
		memcpy(curstr, prefix, len);
	} else {
		curstr = onec;
		len = 0;
	}
	curstr[len] = cur->el;
	curstr[len+1] = 0;
	if (cur->el) {
		process_keys(cur->straight.next, special, padbuttons, mousebuttons, curstr);
	} else {
		int keycode = tern_find_int(special, curstr, 0);
		if (!keycode) {
			keycode = curstr[0];
			if (curstr[1] != 0) {
				warning("%s is not recognized as a key identifier, truncating to %c\n", curstr, curstr[0]);
			}
		}
		char * target = cur->straight.value.ptrval;
		int ui_func, padnum, button;
		int bindtype = parse_binding_target(target, padbuttons, mousebuttons, &ui_func, &padnum, &button);
		if (bindtype == BIND_GAMEPAD1) {
			bind_gamepad(keycode, padnum, button);
		} else if(bindtype == BIND_UI) {
			bind_ui(keycode, ui_func, button);
		}
	}
	process_keys(cur->left, special, padbuttons, mousebuttons, prefix);
	process_keys(cur->right, special, padbuttons, mousebuttons, prefix);
	if (curstr && len) {
		free(curstr);
	}
}

void process_speeds(tern_node * cur, char * prefix)
{
	char * curstr = NULL;
	int len;
	if (!cur) {
		return;
	}
	char onec[2];
	if (prefix) {
		len = strlen(prefix);
		curstr = malloc(len + 2);
		memcpy(curstr, prefix, len);
	} else {
		curstr = onec;
		len = 0;
	}
	curstr[len] = cur->el;
	curstr[len+1] = 0;
	if (cur->el) {
		process_speeds(cur->straight.next, curstr);
	} else {
		char *end;
		long speed_index = strtol(curstr, &end, 10);
		if (speed_index < 0 || end == curstr || *end) {
			warning("%s is not a valid speed index", curstr);
		} else {
			if (speed_index >= num_speeds) {
				speeds = realloc(speeds, sizeof(uint32_t) * (speed_index+1));
				for(; num_speeds < speed_index + 1; num_speeds++) {
					speeds[num_speeds] = 0;
				}
			}
			speeds[speed_index] = atoi(cur->straight.value.ptrval);
			if (speeds[speed_index] < 1) {
				warning("%s is not a valid speed percentage, setting speed %d to 100", cur->straight.value.ptrval, speed_index);
				speeds[speed_index] = 100;
			}
		}
	}
	process_speeds(cur->left, prefix);
	process_speeds(cur->right, prefix);
	if (curstr && len) {
		free(curstr);
	}
}

void process_device(char * device_type, io_port * port)
{
	port->device_type = IO_NONE;
	if (!device_type)
	{
		return;
	}

	const int gamepad_len = strlen("gamepad");
	const int mouse_len = strlen("mouse");
	if (!strncmp(device_type, "gamepad", gamepad_len))
	{
		if (
			(device_type[gamepad_len] != '3' && device_type[gamepad_len] != '6' && device_type[gamepad_len] != '2')
			|| device_type[gamepad_len+1] != '.' || device_type[gamepad_len+2] < '1'
			|| device_type[gamepad_len+2] > '8' || device_type[gamepad_len+3] != 0
		) {
			warning("%s is not a valid gamepad type\n", device_type);
		} else if (device_type[gamepad_len] == '3') {
			port->device_type = IO_GAMEPAD3;
		} else if (device_type[gamepad_len] == '2') {
			port->device_type = IO_GAMEPAD2;
		} else {
			port->device_type = IO_GAMEPAD6;
		}
		port->device.pad.gamepad_num = device_type[gamepad_len+2] - '1';
	} else if(!strncmp(device_type, "mouse", mouse_len)) {
		port->device_type = IO_MOUSE;
		port->device.mouse.mouse_num = device_type[mouse_len+1] - '1';
		port->device.mouse.last_read_x = 0;
		port->device.mouse.last_read_y = 0;
		port->device.mouse.cur_x = 0;
		port->device.mouse.cur_y = 0;
		port->device.mouse.latched_x = 0;
		port->device.mouse.latched_y = 0;
		port->device.mouse.ready_cycle = CYCLE_NEVER;
		port->device.mouse.tr_counter = 0;
	} else if(!strcmp(device_type, "saturn keyboard")) {
		port->device_type = IO_SATURN_KEYBOARD;
		port->device.keyboard.read_pos = 0xFF;
		port->device.keyboard.write_pos = 0;
	} else if(!strcmp(device_type, "xband keyboard")) {
		port->device_type = IO_XBAND_KEYBOARD;
		port->device.keyboard.read_pos = 0xFF;
		port->device.keyboard.write_pos = 0;
	} else if(!strcmp(device_type, "sega_parallel")) {
		port->device_type = IO_SEGA_PARALLEL;
		port->device.stream.data_fd = -1;
		port->device.stream.listen_fd = -1;
	} else if(!strcmp(device_type, "generic")) {
		port->device_type = IO_GENERIC;
		port->device.stream.data_fd = -1;
		port->device.stream.listen_fd = -1;
	}
}

char * io_name(int i)
{
	switch (i)
	{
	case 0:
		return "1";
	case 1:
		return "2";
	case 2:
		return "EXT";
	default:
		return "invalid";
	}
}

static char * sockfile_name;
static void cleanup_sockfile()
{
	unlink(sockfile_name);
}

void setup_io_devices(tern_node * config, rom_info *rom, sega_io *io)
{
	current_io = io;
	io_port * ports = current_io->ports;
	tern_node *io_nodes = tern_find_path(config, "io\0devices\0", TVAL_NODE).ptrval;
	char * io_1 = rom->port1_override ? rom->port1_override : io_nodes ? tern_find_ptr(io_nodes, "1") : NULL;
	char * io_2 = rom->port2_override ? rom->port2_override : io_nodes ? tern_find_ptr(io_nodes, "2") : NULL;
	char * io_ext = rom->ext_override ? rom->ext_override : io_nodes ? tern_find_ptr(io_nodes, "ext") : NULL;

	process_device(io_1, ports);
	process_device(io_2, ports+1);
	process_device(io_ext, ports+2);

	if (ports[0].device_type == IO_MOUSE || ports[1].device_type == IO_MOUSE || ports[2].device_type == IO_MOUSE) {
		if (render_fullscreen()) {
				current_io->mouse_mode = MOUSE_RELATIVE;
				render_relative_mouse(1);
		} else {
			if (rom->mouse_mode && !strcmp(rom->mouse_mode, "absolute")) {
				current_io->mouse_mode = MOUSE_ABSOLUTE;
			} else {
				current_io->mouse_mode = MOUSE_CAPTURE;
			}
		}
	} else {
		current_io->mouse_mode = MOUSE_NONE;
	}

	for (int i = 0; i < 3; i++)
	{
#ifndef _WIN32
		if (ports[i].device_type == IO_SEGA_PARALLEL)
		{
			char *pipe_name = tern_find_path(config, "io\0parallel_pipe\0", TVAL_PTR).ptrval;
			if (!pipe_name)
			{
				warning("IO port %s is configured to use the sega parallel board, but no paralell_pipe is set!\n", io_name(i));
				ports[i].device_type = IO_NONE;
			} else {
				printf("IO port: %s connected to device '%s' with pipe name: %s\n", io_name(i), device_type_names[ports[i].device_type], pipe_name);
				if (!strcmp("stdin", pipe_name))
				{
					ports[i].device.stream.data_fd = STDIN_FILENO;
				} else {
					if (mkfifo(pipe_name, 0666) && errno != EEXIST)
					{
						warning("Failed to create fifo %s for Sega parallel board emulation: %d %s\n", pipe_name, errno, strerror(errno));
						ports[i].device_type = IO_NONE;
					} else {
						ports[i].device.stream.data_fd = open(pipe_name, O_NONBLOCK | O_RDONLY);
						if (ports[i].device.stream.data_fd == -1)
						{
							warning("Failed to open fifo %s for Sega parallel board emulation: %d %s\n", pipe_name, errno, strerror(errno));
							ports[i].device_type = IO_NONE;
						}
					}
				}
			}
		} else if (ports[i].device_type == IO_GENERIC) {
			char *sock_name = tern_find_path(config, "io\0socket\0", TVAL_PTR).ptrval;
			if (!sock_name)
			{
				warning("IO port %s is configured to use generic IO, but no socket is set!\n", io_name(i));
				ports[i].device_type = IO_NONE;
			} else {
				printf("IO port: %s connected to device '%s' with socket name: %s\n", io_name(i), device_type_names[ports[i].device_type], sock_name);
				ports[i].device.stream.data_fd = -1;
				ports[i].device.stream.listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
				size_t pathlen = strlen(sock_name);
				size_t addrlen = offsetof(struct sockaddr_un, sun_path) + pathlen + 1;
				struct sockaddr_un *saddr = malloc(addrlen);
				saddr->sun_family = AF_UNIX;
				memcpy(saddr->sun_path, sock_name, pathlen+1);
				if (bind(ports[i].device.stream.listen_fd, (struct sockaddr *)saddr, addrlen))
				{
					warning("Failed to bind socket for IO Port %s to path %s: %d %s\n", io_name(i), sock_name, errno, strerror(errno));
					goto cleanup_sock;
				}
				if (listen(ports[i].device.stream.listen_fd, 1))
				{
					warning("Failed to listen on socket for IO Port %s: %d %s\n", io_name(i), errno, strerror(errno));
					goto cleanup_sockfile;
				}
				sockfile_name = sock_name;
				atexit(cleanup_sockfile);
				continue;
cleanup_sockfile:
				unlink(sock_name);
cleanup_sock:
				close(ports[i].device.stream.listen_fd);
				ports[i].device_type = IO_NONE;
			}
		} else
#endif
		if (ports[i].device_type == IO_GAMEPAD3 || ports[i].device_type == IO_GAMEPAD6 || ports[i].device_type == IO_GAMEPAD2) {
			printf("IO port %s connected to gamepad #%d with type '%s'\n", io_name(i), ports[i].device.pad.gamepad_num + 1, device_type_names[ports[i].device_type]);
		} else {
			printf("IO port %s connected to device '%s'\n", io_name(i), device_type_names[ports[i].device_type]);
		}
	}
}

void map_bindings(io_port *ports, keybinding *bindings, int numbindings)
{
	for (int i = 0; i < numbindings; i++)
	{
		if (bindings[i].bind_type >= BIND_GAMEPAD1 && bindings[i].bind_type <= BIND_GAMEPAD8)
		{
			int num = bindings[i].bind_type - BIND_GAMEPAD1;
			for (int j = 0; j < 3; j++)
			{
				if ((ports[j].device_type == IO_GAMEPAD3
					 || ports[j].device_type == IO_GAMEPAD6
					 || ports[j].device_type == IO_GAMEPAD2)
					 && ports[j].device.pad.gamepad_num == num
				)
				{
					memset(ports[j].input, 0, sizeof(ports[j].input));
					bindings[i].port = ports + j;
					break;
				}
			}
		}
		else if (bindings[i].bind_type >= BIND_MOUSE1 && bindings[i].bind_type <= BIND_MOUSE8)
		{
			int num = bindings[i].bind_type - BIND_MOUSE1;
			for (int j = 0; j < 3; j++)
			{
				if (ports[j].device_type == IO_MOUSE && ports[j].device.mouse.mouse_num == num)
				{
					memset(ports[j].input, 0, sizeof(ports[j].input));
					bindings[i].port = ports + j;
					break;
				}
			}
		}
	}
}

typedef struct {
	tern_node *padbuttons;
	tern_node *mousebuttons;
	int       mouseidx;
} pmb_state;

void process_mouse_button(char *buttonstr, tern_val value, uint8_t valtype, void *data)
{
	pmb_state *state = data;
	int buttonnum = atoi(buttonstr);
	if (buttonnum < 1 || buttonnum > MAX_MOUSE_BUTTONS) {
		warning("Mouse button %s is out of the supported range of 1-8\n", buttonstr);
		return;
	}
	if (valtype != TVAL_PTR) {
		warning("Mouse button %s is not a scalar value!\n", buttonstr);
		return;
	}
	buttonnum--;
	int ui_func, devicenum, button;
	int bindtype = parse_binding_target(value.ptrval, state->padbuttons, state->mousebuttons, &ui_func, &devicenum, &button);
	switch (bindtype)
	{
	case BIND_UI:
		mice[state->mouseidx].buttons[buttonnum].subtype_a = ui_func;
		break;
	case BIND_GAMEPAD1:
		mice[state->mouseidx].buttons[buttonnum].subtype_a = button >> 12;
		mice[state->mouseidx].buttons[buttonnum].subtype_b = button >> 8 & 0xF;
		mice[state->mouseidx].buttons[buttonnum].value = button & 0xFF;
		break;
	case BIND_MOUSE1:
		mice[state->mouseidx].buttons[buttonnum].value = button & 0xFF;
		break;
	}
	if (bindtype != BIND_UI) {
		bindtype += devicenum-1;
	}
	mice[state->mouseidx].buttons[buttonnum].bind_type = bindtype;

}

void process_mouse(char *mousenum, tern_val value, uint8_t valtype, void *data)
{
	tern_node **buttonmaps = data;
	if (valtype != TVAL_NODE) {
		warning("Binding for mouse %s is a scalar!\n", mousenum);
		return;
	}
	tern_node *mousedef = value.ptrval;
	tern_node *padbuttons = buttonmaps[0];
	tern_node *mousebuttons = buttonmaps[1];

	int mouseidx = atoi(mousenum);
	if (mouseidx < 0 || mouseidx >= MAX_MICE) {
		warning("Mouse numbers must be between 0 and %d, but %d is not\n", MAX_MICE, mouseidx);
		return;
	}
	char *motion = tern_find_ptr(mousedef, "motion");
	if (motion) {
		int ui_func,devicenum,button;
		int bindtype = parse_binding_target(motion, padbuttons, mousebuttons, &ui_func, &devicenum, &button);
		if (bindtype != BIND_UI) {
			bindtype += devicenum-1;
		}
		if (button == PSEUDO_BUTTON_MOTION) {
			mice[mouseidx].bind_type = bindtype;
		} else {
			warning("Mouse motion can't be bound to target %s\n", motion);
		}
	}
	tern_node *buttons = tern_find_path(mousedef, "buttons\0\0", TVAL_NODE).ptrval;
	if (buttons) {
		pmb_state state = {padbuttons, mousebuttons, mouseidx};
		tern_foreach(buttons, process_mouse_button, &state);
	}
}

typedef struct {
	int       padnum;
	tern_node *padbuttons;
	tern_node *mousebuttons;
} pad_button_state;


static long map_warning_pad = -1;
void process_pad_button(char *key, tern_val val, uint8_t valtype, void *data)
{
	pad_button_state *state = data;
	int hostpadnum = state->padnum;
	int ui_func, padnum, button;
	if (valtype != TVAL_PTR) {
		warning("Pad button %s has a non-scalar value\n", key);
		return;
	}
	int bindtype = parse_binding_target(val.ptrval, state->padbuttons, state->mousebuttons, &ui_func, &padnum, &button);
	char *end;
	long hostbutton = strtol(key, &end, 10);
	if (*end) {
		//key is not a valid base 10 integer
		hostbutton = render_translate_input_name(hostpadnum, key, 0);
		if (hostbutton < 0) {
			if (hostbutton == RENDER_INVALID_NAME) {
				warning("%s is not a valid gamepad input name\n", key);
			} else if (hostbutton == RENDER_NOT_MAPPED && hostpadnum != map_warning_pad) {
				warning("No SDL 2 mapping exists for input %s on gamepad %d\n", key, hostpadnum);
				map_warning_pad = hostpadnum;
			}
			return;
		}
		if (hostbutton & RENDER_DPAD_BIT) {
			if (bindtype == BIND_GAMEPAD1) {
				bind_dpad_gamepad(hostpadnum, render_dpad_part(hostbutton), render_direction_part(hostbutton), padnum, button);
			} else {
				bind_dpad_ui(hostpadnum, render_dpad_part(hostbutton), render_direction_part(hostbutton), ui_func, button);
			}
			return;
		} else if (hostbutton & RENDER_AXIS_BIT) {
			if (bindtype == BIND_GAMEPAD1) {
				bind_axis_gamepad(hostpadnum, render_axis_part(hostbutton), 1, padnum, button);
			} else {
				bind_axis_ui(hostpadnum, render_axis_part(hostbutton), 1, padnum, button);
			}
			return;
		}
	}
	if (bindtype == BIND_GAMEPAD1) {
		bind_button_gamepad(hostpadnum, hostbutton, padnum, button);
	} else if (bindtype == BIND_UI) {
		bind_button_ui(hostpadnum, hostbutton, ui_func, button);
	}
}

void process_pad_axis(char *key, tern_val val, uint8_t valtype, void *data)
{
	key = strdup(key);
	pad_button_state *state = data;
	int hostpadnum = state->padnum;
	int ui_func, padnum, button;
	if (valtype != TVAL_PTR) {
		warning("Mapping for axis %s has a non-scalar value", key);
		return;
	}
	int bindtype = parse_binding_target(val.ptrval, state->padbuttons, state->mousebuttons, &ui_func, &padnum, &button);
	char *modifier = strchr(key, '.');
	int positive = 1;
	if (modifier) {
		*modifier = 0;
		modifier++;
		if (!strcmp("negative", modifier)) {
			positive = 0;
		} else if(strcmp("positive", modifier)) {
			warning("Invalid axis modifier %s for axis %s on pad %d\n", modifier, key, hostpadnum);
		}
	}
	char *end;
	long axis = strtol(key, &end, 10);
	if (*end) {
		//key is not a valid base 10 integer
		axis = render_translate_input_name(hostpadnum, key, 1);
		if (axis < 0) {
			if (axis == RENDER_INVALID_NAME) {
				warning("%s is not a valid gamepad input name\n", key);
			} else if (axis == RENDER_NOT_MAPPED && hostpadnum != map_warning_pad) {
				warning("No SDL 2 mapping exists for input %s on gamepad %d\n", key, hostpadnum);
				map_warning_pad = hostpadnum;
			}
			goto done;
		}
		if (axis & RENDER_DPAD_BIT) {
			if (bindtype == BIND_GAMEPAD1) {
				bind_dpad_gamepad(hostpadnum, render_dpad_part(axis), render_direction_part(axis), padnum, button);
			} else {
				bind_dpad_ui(hostpadnum, render_dpad_part(axis), render_direction_part(axis), ui_func, button);
			}
			goto done;
		} else if (axis & RENDER_AXIS_BIT) {
			axis = render_axis_part(axis);
		} else {
			if (bindtype == BIND_GAMEPAD1) {
				bind_button_gamepad(hostpadnum, axis, padnum, button);
			} else if (bindtype == BIND_UI) {
				bind_button_ui(hostpadnum, axis, ui_func, button);
			}
			goto done;
		}
	}
	if (bindtype == BIND_GAMEPAD1) {
		bind_axis_gamepad(hostpadnum, axis, positive, padnum, button);
	} else {
		bind_axis_ui(hostpadnum, axis, positive, ui_func, button);
	}
done:
	free(key);
	return;
}

static tern_node *get_pad_buttons()
{
	static tern_node *padbuttons;
	if (!padbuttons) {
		padbuttons = tern_insert_int(NULL, ".up", DPAD_UP);
		padbuttons = tern_insert_int(padbuttons, ".down", DPAD_DOWN);
		padbuttons = tern_insert_int(padbuttons, ".left", DPAD_LEFT);
		padbuttons = tern_insert_int(padbuttons, ".right", DPAD_RIGHT);
		padbuttons = tern_insert_int(padbuttons, ".a", BUTTON_A);
		padbuttons = tern_insert_int(padbuttons, ".b", BUTTON_B);
		padbuttons = tern_insert_int(padbuttons, ".c", BUTTON_C);
		padbuttons = tern_insert_int(padbuttons, ".x", BUTTON_X);
		padbuttons = tern_insert_int(padbuttons, ".y", BUTTON_Y);
		padbuttons = tern_insert_int(padbuttons, ".z", BUTTON_Z);
		padbuttons = tern_insert_int(padbuttons, ".start", BUTTON_START);
		padbuttons = tern_insert_int(padbuttons, ".mode", BUTTON_MODE);
	}
	return padbuttons;
}

static tern_node *get_mouse_buttons()
{
	static tern_node *mousebuttons;
	if (!mousebuttons) {
		mousebuttons = tern_insert_int(NULL, ".left", MOUSE_LEFT);
		mousebuttons = tern_insert_int(mousebuttons, ".middle", MOUSE_MIDDLE);
		mousebuttons = tern_insert_int(mousebuttons, ".right", MOUSE_RIGHT);
		mousebuttons = tern_insert_int(mousebuttons, ".start", MOUSE_START);
		mousebuttons = tern_insert_int(mousebuttons, ".motion", PSEUDO_BUTTON_MOTION);
	}
	return mousebuttons;
}

void handle_joy_added(int joystick)
{
	if (joystick > MAX_JOYSTICKS) {
		return;
	}
	tern_node * pads = tern_find_path(config, "bindings\0pads\0", TVAL_NODE).ptrval;
	if (pads) {
		char numstr[11];
		sprintf(numstr, "%d", joystick);
		tern_node * pad = tern_find_node(pads, numstr);
		if (pad) {
			tern_node * dpad_node = tern_find_node(pad, "dpads");
			if (dpad_node) {
				for (int dpad = 0; dpad < 10; dpad++)
				{
					numstr[0] = dpad + '0';
					numstr[1] = 0;
					tern_node * pad_dpad = tern_find_node(dpad_node, numstr);
					char * dirs[] = {"up", "down", "left", "right"};
					int dirnums[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};
					for (int dir = 0; dir < sizeof(dirs)/sizeof(dirs[0]); dir++) {
						char * target = tern_find_ptr(pad_dpad, dirs[dir]);
						if (target) {
							int ui_func, padnum, button;
							int bindtype = parse_binding_target(target, get_pad_buttons(), get_mouse_buttons(), &ui_func, &padnum, &button);
							if (bindtype == BIND_GAMEPAD1) {
								bind_dpad_gamepad(joystick, dpad, dirnums[dir], padnum, button);
							} else if (bindtype == BIND_UI) {
								bind_dpad_ui(joystick, dpad, dirnums[dir], ui_func, button);
							}
						}
					}
				}
			}
			tern_node *button_node = tern_find_node(pad, "buttons");
			if (button_node) {
				pad_button_state state = {
					.padnum = joystick,
					.padbuttons = get_pad_buttons(),
					.mousebuttons = get_mouse_buttons()
				};
				tern_foreach(button_node, process_pad_button, &state);
			}
			tern_node *axes_node = tern_find_node(pad, "axes");
			if (axes_node) {
				pad_button_state state = {
					.padnum = joystick,
					.padbuttons = get_pad_buttons(),
					.mousebuttons = get_mouse_buttons()
				};
				tern_foreach(axes_node, process_pad_axis, &state);
			}
			if (current_io) {
				if (joysticks[joystick].buttons) {
					map_bindings(current_io->ports, joysticks[joystick].buttons, joysticks[joystick].num_buttons);
				}
				if (joysticks[joystick].dpads)
				{
					for (uint32_t i = 0; i < joysticks[joystick].num_dpads; i++)
					{
						map_bindings(current_io->ports, joysticks[joystick].dpads[i].bindings, 4);
					}
				}
				if (joysticks[joystick].axes) {
					for (uint32_t i = 0; i < joysticks[joystick].num_axes; i++)
					{
						map_bindings(current_io->ports, &joysticks[joystick].axes[i].positive, 1);
						map_bindings(current_io->ports, &joysticks[joystick].axes[i].negative, 1);
					}
				}
			}
		}
	}
	
}

void set_keybindings(sega_io *io)
{
	static uint8_t already_done;
	if (already_done) {
		map_all_bindings(io);
		return;
	}
	already_done = 1;
	io_port *ports = io->ports;
	tern_node * special = tern_insert_int(NULL, "up", RENDERKEY_UP);
	special = tern_insert_int(special, "down", RENDERKEY_DOWN);
	special = tern_insert_int(special, "left", RENDERKEY_LEFT);
	special = tern_insert_int(special, "right", RENDERKEY_RIGHT);
	special = tern_insert_int(special, "enter", '\r');
	special = tern_insert_int(special, "space", ' ');
	special = tern_insert_int(special, "tab", '\t');
	special = tern_insert_int(special, "backspace", '\b');
	special = tern_insert_int(special, "esc", RENDERKEY_ESC);
	special = tern_insert_int(special, "delete", RENDERKEY_DEL);
	special = tern_insert_int(special, "lshift", RENDERKEY_LSHIFT);
	special = tern_insert_int(special, "rshift", RENDERKEY_RSHIFT);
	special = tern_insert_int(special, "lctrl", RENDERKEY_LCTRL);
	special = tern_insert_int(special, "rctrl", RENDERKEY_RCTRL);
	special = tern_insert_int(special, "lalt", RENDERKEY_LALT);
	special = tern_insert_int(special, "ralt", RENDERKEY_RALT);
	special = tern_insert_int(special, "home", RENDERKEY_HOME);
	special = tern_insert_int(special, "end", RENDERKEY_END);
	special = tern_insert_int(special, "pageup", RENDERKEY_PAGEUP);
	special = tern_insert_int(special, "pagedown", RENDERKEY_PAGEDOWN);
	special = tern_insert_int(special, "f1", RENDERKEY_F1);
	special = tern_insert_int(special, "f2", RENDERKEY_F2);
	special = tern_insert_int(special, "f3", RENDERKEY_F3);
	special = tern_insert_int(special, "f4", RENDERKEY_F4);
	special = tern_insert_int(special, "f5", RENDERKEY_F5);
	special = tern_insert_int(special, "f6", RENDERKEY_F6);
	special = tern_insert_int(special, "f7", RENDERKEY_F7);
	special = tern_insert_int(special, "f8", RENDERKEY_F8);
	special = tern_insert_int(special, "f9", RENDERKEY_F9);
	special = tern_insert_int(special, "f10", RENDERKEY_F10);
	special = tern_insert_int(special, "f11", RENDERKEY_F11);
	special = tern_insert_int(special, "f12", RENDERKEY_F12);
	special = tern_insert_int(special, "select", RENDERKEY_SELECT);
	special = tern_insert_int(special, "play", RENDERKEY_PLAY);
	special = tern_insert_int(special, "search", RENDERKEY_SEARCH);
	special = tern_insert_int(special, "back", RENDERKEY_BACK);

	tern_node *padbuttons = get_pad_buttons();

	tern_node *mousebuttons = get_mouse_buttons();
	
	tern_node * keys = tern_find_path(config, "bindings\0keys\0", TVAL_NODE).ptrval;
	process_keys(keys, special, padbuttons, mousebuttons, NULL);
	char numstr[] = "00";
	tern_node * pads = tern_find_path(config, "bindings\0pads\0", TVAL_NODE).ptrval;
	if (pads) {
		for (int i = 0; i < MAX_JOYSTICKS; i++)
		{

			if (i < 10) {
				numstr[0] = i + '0';
				numstr[1] = 0;
			} else {
				numstr[0] = i/10 + '0';
				numstr[1] = i%10 + '0';
			}
			
		}
	}
	memset(mice, 0, sizeof(mice));
	tern_node * mice = tern_find_path(config, "bindings\0mice\0", TVAL_NODE).ptrval;
	if (mice) {
		tern_node *buttonmaps[2] = {padbuttons, mousebuttons};
		tern_foreach(mice, process_mouse, buttonmaps);
	}
	tern_node * speed_nodes = tern_find_path(config, "clocks\0speeds\0", TVAL_NODE).ptrval;
	speeds = malloc(sizeof(uint32_t));
	speeds[0] = 100;
	process_speeds(speed_nodes, NULL);
	for (int i = 0; i < num_speeds; i++)
	{
		if (!speeds[i]) {
			warning("Speed index %d was not set to a valid percentage!", i);
			speeds[i] = 100;
		}
	}
	map_all_bindings(io);
}

void map_all_bindings(sega_io *io)
{
	current_io = io;
	io_port *ports = io->ports;
	
	for (int bucket = 0; bucket < 0x10000; bucket++)
	{
		if (bindings[bucket])
		{
			map_bindings(ports, bindings[bucket], 0x8000);
		}
	}
	for (int stick = 0; stick < MAX_JOYSTICKS; stick++)
	{
		if (joysticks[stick].buttons) {
			map_bindings(ports, joysticks[stick].buttons, joysticks[stick].num_buttons);
		}
		if (joysticks[stick].dpads)
		{
			for (uint32_t i = 0; i < joysticks[stick].num_dpads; i++)
			{
				map_bindings(ports, joysticks[stick].dpads[i].bindings, 4);
			}
		}
		for (uint32_t i = 0; i < joysticks[stick].num_axes; i++)
		{
			map_bindings(current_io->ports, &joysticks[stick].axes[i].positive, 1);
			map_bindings(current_io->ports, &joysticks[stick].axes[i].negative, 1);
		}
	}
	for (int mouse = 0; mouse < MAX_MICE; mouse++)
	{
		if (mice[mouse].bind_type >= BIND_MOUSE1 && mice[mouse].bind_type <= BIND_MOUSE8) {
			int num = mice[mouse].bind_type - BIND_MOUSE1;
			for (int j = 0; j < 3; j++)
			{
				if (ports[j].device_type == IO_MOUSE && ports[j].device.mouse.mouse_num == num)
				{
					memset(ports[j].input, 0, sizeof(ports[j].input));
					mice[mouse].motion_port = ports + j;
					break;
				}
			}
		}
		map_bindings(ports, mice[mouse].buttons, MAX_MOUSE_BUTTONS);
	}
	keyboard_port = NULL;
	for (int i = 0; i < 3; i++)
	{
		if (ports[i].device_type == IO_SATURN_KEYBOARD || ports[i].device_type == IO_XBAND_KEYBOARD) {
			keyboard_port = ports + i;
			break;
		}
	}
	//not really related to the intention of this function, but the best place to do this currently
	if (speeds[0] != 100) {
		current_system->set_speed_percent(current_system, speeds[0]);
	}
}

#define TH 0x40
#define TR 0x20
#define TH_TIMEOUT 56000

void mouse_check_ready(io_port *port, uint32_t current_cycle)
{
	if (current_cycle >= port->device.mouse.ready_cycle) {
		port->device.mouse.tr_counter++;
		port->device.mouse.ready_cycle = CYCLE_NEVER;
		if (port->device.mouse.tr_counter == 3) {
			port->device.mouse.latched_x = port->device.mouse.cur_x;
			port->device.mouse.latched_y = port->device.mouse.cur_y;
			if (current_io->mouse_mode == MOUSE_ABSOLUTE) {
				//avoid overflow in absolute mode
				int deltax = port->device.mouse.latched_x - port->device.mouse.last_read_x;
				if (abs(deltax) > 255) {
					port->device.mouse.latched_x = port->device.mouse.last_read_x + (deltax > 0 ? 255 : -255);
				}
				int deltay = port->device.mouse.latched_y - port->device.mouse.last_read_y;
				if (abs(deltay) > 255) {
					port->device.mouse.latched_y = port->device.mouse.last_read_y + (deltay > 0 ? 255 : -255);
				}
			}
		}
	}
}

uint32_t last_poll_cycle;
void io_adjust_cycles(io_port * port, uint32_t current_cycle, uint32_t deduction)
{
	/*uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("adjust_cycles | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, current_cycle);
	}*/
	if (port->device_type == IO_GAMEPAD6)
	{
		if (current_cycle >= port->device.pad.timeout_cycle)
		{
			port->device.pad.th_counter = 0;
		} else {
			port->device.pad.timeout_cycle -= deduction;
		}
	} else if (port->device_type == IO_MOUSE) {
		mouse_check_ready(port, current_cycle);
		if (port->device.mouse.ready_cycle != CYCLE_NEVER) {
			port->device.mouse.ready_cycle -= deduction;
		}
	}
	for (int i = 0; i < 8; i++)
	{
		if (port->slow_rise_start[i] != CYCLE_NEVER) {
			if (port->slow_rise_start[i] >= deduction) {
				port->slow_rise_start[i] -= deduction;
			} else {
				port->slow_rise_start[i] = CYCLE_NEVER;
			}
		}
	}
	if (last_poll_cycle >= deduction) {
		last_poll_cycle -= deduction;
	} else {
		last_poll_cycle = 0;
	}
}

#ifndef _WIN32
static void wait_for_connection(io_port * port)
{
	if (port->device.stream.data_fd == -1)
	{
		puts("Waiting for socket connection...");
		port->device.stream.data_fd = accept(port->device.stream.listen_fd, NULL, NULL);
		fcntl(port->device.stream.data_fd, F_SETFL, O_NONBLOCK | O_RDWR);
	}
}

static void service_pipe(io_port * port)
{
	uint8_t value;
	int numRead = read(port->device.stream.data_fd, &value, sizeof(value));
	if (numRead > 0)
	{
		port->input[IO_TH0] = (value & 0xF) | 0x10;
		port->input[IO_TH1] = (value >> 4) | 0x10;
	} else if(numRead == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		warning("Error reading pipe for IO port: %d %s\n", errno, strerror(errno));
	}
}

static void service_socket(io_port *port)
{
	uint8_t buf[32];
	uint8_t blocking = 0;
	int numRead = 0;
	while (numRead <= 0)
	{
		numRead = recv(port->device.stream.data_fd, buf, sizeof(buf), 0);
		if (numRead > 0)
		{
			port->input[IO_TH0] = buf[numRead-1];
			if (port->input[IO_STATE] == IO_READ_PENDING)
			{
				port->input[IO_STATE] = IO_READ;
				if (blocking)
				{
					//pending read satisfied, back to non-blocking mode
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR | O_NONBLOCK);
				}
			} else if (port->input[IO_STATE] == IO_WRITTEN) {
				port->input[IO_STATE] = IO_READ;
			}
		} else if (numRead == 0) {
			port->device.stream.data_fd = -1;
			wait_for_connection(port);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			warning("Error reading from socket for IO port: %d %s\n", errno, strerror(errno));
			close(port->device.stream.data_fd);
			wait_for_connection(port);
		} else if (port->input[IO_STATE] == IO_READ_PENDING) {
			//clear the nonblocking flag so the next read will block
			if (!blocking)
			{
				fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR);
				blocking = 1;
			}
		} else {
			//no new data, but that's ok
			break;
		}
	}

	if (port->input[IO_STATE] == IO_WRITE_PENDING)
	{
		uint8_t value = port->output & port->control;
		int written = 0;
		blocking = 0;
		while (written <= 0)
		{
			send(port->device.stream.data_fd, &value, sizeof(value), 0);
			if (written > 0)
			{
				port->input[IO_STATE] = IO_WRITTEN;
				if (blocking)
				{
					//pending write satisfied, back to non-blocking mode
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR | O_NONBLOCK);
				}
			} else if (written == 0) {
				port->device.stream.data_fd = -1;
				wait_for_connection(port);
			} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
				warning("Error writing to socket for IO port: %d %s\n", errno, strerror(errno));
				close(port->device.stream.data_fd);
				wait_for_connection(port);
			} else {
				//clear the nonblocking flag so the next write will block
				if (!blocking)
				{
					fcntl(port->device.stream.data_fd, F_SETFL, O_RDWR);
					blocking = 1;
				}
			}
		}
	}
}
#endif

const int mouse_delays[] = {112*7, 120*7, 96*7, 132*7, 104*7, 96*7, 112*7, 96*7};

enum {
	KB_SETUP,
	KB_READ,
	KB_WRITE
};

void io_control_write(io_port *port, uint8_t value, uint32_t current_cycle)
{
	uint8_t changes = value ^ port->control;
	if (changes) {
		for (int i = 0; i < 8; i++)
		{
			if (!(value & 1 << i) && !(port->output & 1 << i)) {
				//port switched from output to input and the output value was 0
				//since there is a weak pull-up on input pins, this will lead
				//to a slow rise from 0 to 1 if the pin isn't being externally driven
				port->slow_rise_start[i] = current_cycle;
			} else {
				port->slow_rise_start[i] = CYCLE_NEVER;
			}
		}
		port->control = value;
	}
}

void io_data_write(io_port * port, uint8_t value, uint32_t current_cycle)
{
	uint8_t old_output = (port->control & port->output) | (~port->control & 0xFF);
	uint8_t output = (port->control & value) | (~port->control & 0xFF);
	switch (port->device_type)
	{
	case IO_GAMEPAD6:
		//check if TH has changed
		if ((old_output & TH) ^ (output & TH)) {
			if (current_cycle >= port->device.pad.timeout_cycle) {
				port->device.pad.th_counter = 0;
			}
			if ((output & TH)) {
				port->device.pad.th_counter++;
			}
			port->device.pad.timeout_cycle = current_cycle + TH_TIMEOUT;
		}
		break;
	case IO_MOUSE:
		mouse_check_ready(port, current_cycle);
		if (output & TH) {
			//request is over or mouse is being reset
			if (port->device.mouse.tr_counter) {
				//request is over
				port->device.mouse.last_read_x = port->device.mouse.latched_x;
				port->device.mouse.last_read_y = port->device.mouse.latched_y;
			}
			port->device.mouse.tr_counter = 0;
			port->device.mouse.ready_cycle = CYCLE_NEVER;
		} else {
			if ((output & TR) != (old_output & TR)) {
				int delay_index = port->device.mouse.tr_counter >= sizeof(mouse_delays) ? sizeof(mouse_delays)-1 : port->device.mouse.tr_counter;
				port->device.mouse.ready_cycle = current_cycle + mouse_delays[delay_index];
			}
		}
		break;
	case IO_SATURN_KEYBOARD:
		if (output & TH) {
			//request is over
			if (port->device.keyboard.tr_counter >= 10 && port->device.keyboard.read_pos != 0xFF) {
				//remove scan code from buffer
				port->device.keyboard.read_pos++;
				port->device.keyboard.read_pos &= 7;
				if (port->device.keyboard.read_pos == port->device.keyboard.write_pos) {
					port->device.keyboard.read_pos = 0xFF;
				}
			}
			port->device.keyboard.tr_counter = 0;
		} else {
			if ((output & TR) != (old_output & TR)) {
				port->device.keyboard.tr_counter++;
			}
		}
		break;
	case IO_XBAND_KEYBOARD:
		if (output & TH) {
			//request is over
			if (
				port->device.keyboard.mode == KB_READ && port->device.keyboard.tr_counter > 6
				&& (port->device.keyboard.tr_counter & 1)
			) {
				if (port->device.keyboard.events[port->device.keyboard.read_pos] & 0xFF00) {
					port->device.keyboard.events[port->device.keyboard.read_pos] &= 0xFF;
				} else {
					port->device.keyboard.read_pos++;
					port->device.keyboard.read_pos &= 7;
					if (port->device.keyboard.read_pos == port->device.keyboard.write_pos) {
						port->device.keyboard.read_pos = 0xFF;
					}
				}
			}
			port->device.keyboard.tr_counter = 0;
			port->device.keyboard.mode = KB_SETUP;
		} else {
			if ((output & TR) != (old_output & TR)) {
				port->device.keyboard.tr_counter++;
				if (port->device.keyboard.tr_counter == 2) {
					port->device.keyboard.mode = (output & 0xF) ? KB_READ : KB_WRITE;
				} else if (port->device.keyboard.mode == KB_WRITE) {
					switch (port->device.keyboard.tr_counter)
					{
					case 3:
						//host writes 0b0001
						break;
					case 4:
						//host writes 0b0000
						break;
					case 5:
						//host writes 0b0000
						break;
					case 6:
						port->device.keyboard.cmd = output << 4;
						break;
					case 7:
						port->device.keyboard.cmd |= output & 0xF;
						//TODO: actually do something with the command
						break;
					}
				} else if (
					port->device.keyboard.mode == KB_READ && port->device.keyboard.tr_counter > 7
					&& !(port->device.keyboard.tr_counter & 1)
				) {
					
					if (port->device.keyboard.events[port->device.keyboard.read_pos] & 0xFF00) {
						port->device.keyboard.events[port->device.keyboard.read_pos] &= 0xFF;
					} else {
						port->device.keyboard.read_pos++;
						port->device.keyboard.read_pos &= 7;
						if (port->device.keyboard.read_pos == port->device.keyboard.write_pos) {
							port->device.keyboard.read_pos = 0xFF;
						}
					}
				}
			}
		}
		break;
#ifndef _WIN32
	case IO_GENERIC:
		wait_for_connection(port);
		port->input[IO_STATE] = IO_WRITE_PENDING;
		service_socket(port);
		break;
#endif
	}
	port->output = value;

}

uint8_t get_scancode_bytes(io_port *port)
{
	if (port->device.keyboard.read_pos == 0xFF) {
		return 0;
	}
	uint8_t bytes = 0, read_pos = port->device.keyboard.read_pos;
	do {
		bytes += port->device.keyboard.events[read_pos] & 0xFF00 ? 2 : 1;
		read_pos++;
		read_pos &= 7;
	} while (read_pos != port->device.keyboard.write_pos);
	
	return bytes;
}

#define SLOW_RISE_DEVICE (30*7)
#define SLOW_RISE_INPUT (12*7)

static uint8_t get_output_value(io_port *port, uint32_t current_cycle, uint32_t slow_rise_delay)
{
	uint8_t output = (port->control | 0x80) & port->output;
	for (int i = 0; i < 8; i++)
	{
		if (!(port->control & 1 << i)) {
			if (port->slow_rise_start[i] != CYCLE_NEVER) {
				if (current_cycle - port->slow_rise_start[i] >= slow_rise_delay) {
					output |= 1 << i;
				}
			} else {
				output |= 1 << i;
			}
		}
	}
	return output;
}

uint8_t io_data_read(io_port * port, uint32_t current_cycle)
{
	uint8_t output = get_output_value(port, current_cycle, SLOW_RISE_DEVICE);
	uint8_t control = port->control | 0x80;
	uint8_t th = output & 0x40;
	uint8_t input;
	uint8_t device_driven;
	if (current_cycle - last_poll_cycle > MIN_POLL_INTERVAL) {
		process_events();
		last_poll_cycle = current_cycle;
	}
	switch (port->device_type)
	{
	case IO_GAMEPAD2:
		input = ~port->input[GAMEPAD_TH1];
		device_driven = 0x3F;
		break;
	case IO_GAMEPAD3:
	{
		input = port->input[th ? GAMEPAD_TH1 : GAMEPAD_TH0];
		if (!th) {
			input |= 0xC;
		}
		//controller output is logically inverted
		input = ~input;
		device_driven = 0x3F;
		break;
	}
	case IO_GAMEPAD6:
	{
		if (current_cycle >= port->device.pad.timeout_cycle) {
			port->device.pad.th_counter = 0;
		}
		/*if (port->input[GAMEPAD_TH0] || port->input[GAMEPAD_TH1]) {
			printf("io_data_read | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, port->input[GAMEPAD_TH0], port->input[GAMEPAD_TH1], port->th_counter,port->timeout_cycle, context->current_cycle);
		}*/
		if (th) {
			if (port->device.pad.th_counter == 3) {
				input = port->input[GAMEPAD_EXTRA];
			} else {
				input = port->input[GAMEPAD_TH1];
			}
		} else {
			if (port->device.pad.th_counter == 2) {
				input = port->input[GAMEPAD_TH0] | 0xF;
			} else if(port->device.pad.th_counter == 3) {
				input = port->input[GAMEPAD_TH0]  & 0x30;
			} else {
				input = port->input[GAMEPAD_TH0] | 0xC;
			}
		}
		//controller output is logically inverted
		input = ~input;
		device_driven = 0x3F;
		break;
	}
	case IO_MOUSE:
	{
		mouse_check_ready(port, current_cycle);
		uint8_t tr = output & TR;
		if (th) {
			if (tr) {
				input = 0x10;
			} else {
				input = 0;
			}
		} else {

			int16_t delta_x = port->device.mouse.latched_x - port->device.mouse.last_read_x;
			int16_t delta_y = port->device.mouse.last_read_y - port->device.mouse.latched_y;
			switch (port->device.mouse.tr_counter)
			{
			case 0:
				input = 0xB;
				break;
			case 1:
			case 2:
				input = 0xF;
				break;
			case 3:
				input = 0;
				if (delta_y > 255 || delta_y < -255) {
					input |= 8;
				}
				if (delta_x > 255 || delta_x < -255) {
					input |= 4;
				}
				if (delta_y < 0) {
					input |= 2;
				}
				if (delta_x < 0) {
					input |= 1;
				}
				break;
			case 4:
				input = port->input[0];
				break;
			case 5:
				input = delta_x >> 4 & 0xF;
				break;
			case 6:
				input = delta_x & 0xF;
				break;
			case 7:
				input = delta_y >> 4 & 0xF;
				break;
			case 8:
			default:
				input = delta_y & 0xF;
				break;
			}
			input |= ((port->device.mouse.tr_counter & 1) == 0) << 4;
		}
		device_driven = 0x1F;
		break;
	}
	case IO_SATURN_KEYBOARD:
	{
		if (th) {
			input = 0x11;
		} else {
			uint8_t tr = output & TR;
			uint16_t code = port->device.keyboard.read_pos == 0xFF ? 0 
				: port->device.keyboard.events[port->device.keyboard.read_pos];
			switch (port->device.keyboard.tr_counter)
			{
			case 0:
				input = 1;
				break;
			case 1:
				//Saturn peripheral ID
				input = 3;
				break;
			case 2:
				//data size
				input = 4;
				break;
			case 3:
				//d-pad
				//TODO: set these based on keyboard state
				input = 0xF;
				break;
			case 4:
				//Start ABC
				//TODO: set these based on keyboard state
				input = 0xF;
				break;
			case 5:
				//R XYZ
				//TODO: set these based on keyboard state
				input = 0xF;
				break;
			case 6:
				//L and KBID
				//TODO: set L based on keyboard state
				input = 0x8;
				break;
			case 7:
				//Capslock, Numlock, Scrolllock
				//TODO: set these based on keyboard state
				input = 0;
				break;
			case 8:
				input = 6;
				if (code & 0xFF00) {
					//break
					input |= 1;
				} else if (code) {
					input |= 8;
				}
				break;
			case 9:
				input = code >> 4 & 0xF;
				break;
			case 10:
				input = code & 0xF;
				break;
			case 11:
				input = 0;
				break;
			default:
				input = 1;
				break;
			}
			input |= ((port->device.keyboard.tr_counter & 1) == 0) << 4;
		}
		device_driven = 0x1F;
		break;
	}
	case IO_XBAND_KEYBOARD:
	{
		if (th) {
			input = 0x1C;
		} else {
			uint8_t size;
			if (port->device.keyboard.mode == KB_SETUP || port->device.keyboard.mode == KB_READ) {
				switch (port->device.keyboard.tr_counter)
				{
				case 0:
					input = 0x3;
					break;
				case 1:
					input = 0x6;
					break;
				case 2:
					//This is where thoe host indicates a read or write
					//presumably, the keyboard only outputs this if the host
					//is not already driving the data bus low
					input = 0x9;
					break;
				case 3:
					size = get_scancode_bytes(port);
					if (size) {
						++size;
					}
					if (size > 15) {
						size = 15;
					}
					input = size;
					break;
				case 4:
				case 5:
					//always send packet type 0 for now
					input = 0;
					break;
				default:
					if (port->device.keyboard.read_pos == 0xFF) {
						//we've run out of bytes
						input = 0;
					} else if (port->device.keyboard.events[port->device.keyboard.read_pos] & 0xFF00) {
						if (port->device.keyboard.tr_counter & 1) {
							input = port->device.keyboard.events[port->device.keyboard.read_pos] >> 8 & 0xF;
						} else {
							input = port->device.keyboard.events[port->device.keyboard.read_pos] >> 12;
						}
					} else {
						if (port->device.keyboard.tr_counter & 1) {
							input = port->device.keyboard.events[port->device.keyboard.read_pos] & 0xF;
						} else {
							input = port->device.keyboard.events[port->device.keyboard.read_pos] >> 4;
						}
					}
					break;
				}
			} else {
				input = 0xF;
			}
			input |= ((port->device.keyboard.tr_counter & 1) == 0) << 4;
		}
		//this is not strictly correct at all times, but good enough for now
		device_driven = 0x1F;
		break;
	}
#ifndef _WIN32
	case IO_SEGA_PARALLEL:
		if (!th)
		{
			service_pipe(port);
		}
		input = port->input[th ? IO_TH1 : IO_TH0];
		device_driven = 0x3F;
		break;
	case IO_GENERIC:
		if (port->input[IO_TH0] & 0x80 && port->input[IO_STATE] == IO_WRITTEN)
		{
			//device requested a blocking read after writes
			port->input[IO_STATE] = IO_READ_PENDING;
		}
		service_socket(port);
		input = port->input[IO_TH0];
		device_driven = 0x7F;
		break;
#endif
	default:
		input = 0;
		device_driven = 0;
		break;
	}
	uint8_t value = (input & (~control) & device_driven) | (port->output & control);
	//deal with pins that are configured as inputs, but not being actively driven by the device
	uint8_t floating = (~device_driven) & (~control);
	if (floating) {
		value |= get_output_value(port, current_cycle, SLOW_RISE_INPUT) & floating;
	}
	/*if (port->input[GAMEPAD_TH0] || port->input[GAMEPAD_TH1]) {
		printf ("value: %X\n", value);
	}*/
	return value;
}

void io_serialize(io_port *port, serialize_buffer *buf)
{
	save_int8(buf, port->output);
	save_int8(buf, port->control);
	save_int8(buf, port->serial_out);
	save_int8(buf, port->serial_in);
	save_int8(buf, port->serial_ctrl);
	save_int8(buf, port->device_type);
	save_buffer32(buf, port->slow_rise_start, 8);
	switch (port->device_type)
	{
	case IO_GAMEPAD6:
		save_int32(buf, port->device.pad.timeout_cycle);
		save_int16(buf, port->device.pad.th_counter);
		break;
	case IO_MOUSE:
		save_int32(buf, port->device.mouse.ready_cycle);
		save_int16(buf, port->device.mouse.last_read_x);
		save_int16(buf, port->device.mouse.last_read_y);
		save_int16(buf, port->device.mouse.latched_x);
		save_int16(buf, port->device.mouse.latched_y);
		save_int8(buf, port->device.mouse.tr_counter);
		break;
	case IO_SATURN_KEYBOARD:
	case IO_XBAND_KEYBOARD:
		save_int8(buf, port->device.keyboard.tr_counter);
		if (port->device_type == IO_XBAND_KEYBOARD) {
			save_int8(buf, port->device.keyboard.mode);
			save_int8(buf, port->device.keyboard.cmd);
		}
		break;
	}
}

void io_deserialize(deserialize_buffer *buf, void *vport)
{
	io_port *port = vport;
	port->output = load_int8(buf);
	port->control = load_int8(buf);
	port->serial_out = load_int8(buf);
	port->serial_in = load_int8(buf);
	port->serial_ctrl = load_int8(buf);
	uint8_t device_type = load_int8(buf);
	if (device_type != port->device_type) {
		warning("Loaded save state has a different device type from the current configuration");
		return;
	}
	switch (port->device_type)
	{
	case IO_GAMEPAD6:
		port->device.pad.timeout_cycle = load_int32(buf);
		port->device.pad.th_counter = load_int16(buf);
		break;
	case IO_MOUSE:
		port->device.mouse.ready_cycle = load_int32(buf);
		port->device.mouse.last_read_x = load_int16(buf);
		port->device.mouse.last_read_y = load_int16(buf);
		port->device.mouse.latched_x = load_int16(buf);
		port->device.mouse.latched_y = load_int16(buf);
		port->device.mouse.tr_counter = load_int8(buf);
		break;
	case IO_SATURN_KEYBOARD:
	case IO_XBAND_KEYBOARD:
		port->device.keyboard.tr_counter = load_int8(buf);
		if (port->device_type == IO_XBAND_KEYBOARD) {
			port->device.keyboard.mode = load_int8(buf);
			port->device.keyboard.cmd = load_int8(buf);
		}
		break;
	}
}
