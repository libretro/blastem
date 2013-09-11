/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "io.h"
#include "blastem.h"
#include "render.h"

enum {
	BIND_NONE,
	BIND_GAMEPAD1,
	BIND_GAMEPAD2,
	BIND_UI
};

typedef enum {
	UI_DEBUG_MODE_INC,
	UI_DEBUG_PAL_INC,
	UI_ENTER_DEBUGGER,
  UI_SAVE_STATE,
	UI_EXIT
} ui_action;

typedef struct {
	uint8_t bind_type;
	uint8_t subtype_a;
	uint8_t subtype_b;
	uint8_t value;
} keybinding;

typedef struct {
	keybinding bindings[4];
	uint8_t    state;
} joydpad;

keybinding * bindings[256];
keybinding * joybindings[MAX_JOYSTICKS];
joydpad * joydpads[MAX_JOYSTICKS];
const uint8_t dpadbits[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};

void bind_key(int keycode, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		bindings[bucket] = malloc(sizeof(keybinding) * 256);
		memset(bindings[bucket], 0, sizeof(keybinding) * 256);
	}
	int idx = keycode & 0xFF;
	bindings[bucket][idx].bind_type = bind_type;
	bindings[bucket][idx].subtype_a = subtype_a;
	bindings[bucket][idx].subtype_b = subtype_b;
	bindings[bucket][idx].value = value;
}

void bind_button(int joystick, int button, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joybindings[joystick]) {
		int num = render_joystick_num_buttons(joystick);
		if (!num) {
			return;
		}
		joybindings[joystick] = malloc(sizeof(keybinding)*num);
		memset(joybindings[joystick], 0, sizeof(keybinding)*num);
	}
	joybindings[joystick][button].bind_type = bind_type;
	joybindings[joystick][button].subtype_a = subtype_a;
	joybindings[joystick][button].subtype_b = subtype_b;
	joybindings[joystick][button].value = value;
}

void bind_dpad(int joystick, int dpad, int direction, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b, uint8_t value)
{
	if (joystick >= MAX_JOYSTICKS) {
		return;
	}
	if (!joydpads[joystick]) {
		int num = render_joystick_num_hats(joystick);
		if (!num) {
			return;
		}
		joydpads[joystick] = malloc(sizeof(joydpad)*num);
		memset(joydpads[joystick], 0, sizeof(joydpad)*num);
	}
	for (int i = 0; i < 4; i ++) {
		if (dpadbits[i] & direction) {
			joydpads[joystick][dpad].bindings[i].bind_type = bind_type;
			joydpads[joystick][dpad].bindings[i].subtype_a = subtype_a;
			joydpads[joystick][dpad].bindings[i].subtype_b = subtype_b;
			joydpads[joystick][dpad].bindings[i].value = value;
			break;
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

void bind_gamepad(int keycode, int gamepadnum, int button)
{

	if (gamepadnum < 1 || gamepadnum > 2) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_key(keycode, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_button_gamepad(int joystick, int joybutton, int gamepadnum, int padbutton)
{
	if (gamepadnum < 1 || gamepadnum > 2) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_button(joystick, joybutton, bind_type, padbutton >> 12, padbutton >> 8 & 0xF, padbutton & 0xFF);
}

void bind_dpad_gamepad(int joystick, int dpad, uint8_t direction, int gamepadnum, int button)
{
	if (gamepadnum < 1 || gamepadnum > 2) {
		return;
	}
	uint8_t bind_type = gamepadnum - 1 + BIND_GAMEPAD1;
	bind_dpad(joystick, dpad, direction, bind_type, button >> 12, button >> 8 & 0xF, button & 0xFF);
}

void bind_ui(int keycode, ui_action action)
{
	bind_key(keycode, BIND_UI, action, 0, 0);
}

void handle_binding_down(keybinding * binding)
{
	switch(binding->bind_type)
	{
	case BIND_GAMEPAD1:
	case BIND_GAMEPAD2:
		if (binding->subtype_a <= GAMEPAD_EXTRA) {
			genesis->ports[binding->bind_type - BIND_GAMEPAD1].input[binding->subtype_a] |= binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA) {
			genesis->ports[binding->bind_type - BIND_GAMEPAD1].input[binding->subtype_b] |= binding->value;
		}
		break;
	}
}

void handle_keydown(int keycode)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		return;
	}
	int idx = keycode & 0xFF;
	keybinding * binding = bindings[bucket] + idx;
	handle_binding_down(binding);
}

void handle_joydown(int joystick, int button)
{
	if (!joybindings[joystick]) {
		return;
	}
	keybinding * binding = joybindings[joystick] + button;
	handle_binding_down(binding);
}

uint8_t ui_debug_mode = 0;
uint8_t ui_debug_pal = 0;

void handle_binding_up(keybinding * binding)
{
	switch(binding->bind_type)
	{
	case BIND_GAMEPAD1:
	case BIND_GAMEPAD2:
		if (binding->subtype_a <= GAMEPAD_EXTRA) {
			genesis->ports[binding->bind_type - BIND_GAMEPAD1].input[binding->subtype_a] &= ~binding->value;
		}
		if (binding->subtype_b <= GAMEPAD_EXTRA) {
			genesis->ports[binding->bind_type - BIND_GAMEPAD1].input[binding->subtype_b] &= ~binding->value;
		}
		break;
	case BIND_UI:
		switch (binding->subtype_a)
		{
		case UI_DEBUG_MODE_INC:
			ui_debug_mode++;
			if (ui_debug_mode == 4) {
				ui_debug_mode = 0;
			}
			genesis->vdp->debug = ui_debug_mode;
			break;
		case UI_DEBUG_PAL_INC:
			ui_debug_pal++;
			if (ui_debug_pal == 4) {
				ui_debug_pal = 0;
			}
			render_debug_pal(ui_debug_pal);
			break;
		case UI_ENTER_DEBUGGER:
			break_on_sync = 1;
			break;
		case UI_SAVE_STATE:
			save_state = 1;
			break;
		case UI_EXIT:
			exit(0);
		}
		break;
	}
}

void handle_keyup(int keycode)
{
	int bucket = keycode >> 8 & 0xFF;
	if (!bindings[bucket]) {
		return;
	}
	int idx = keycode & 0xFF;
	keybinding * binding = bindings[bucket] + idx;
	handle_binding_up(binding);
}

void handle_joyup(int joystick, int button)
{
	if (!joybindings[joystick]) {
		return;
	}
	keybinding * binding = joybindings[joystick] + button;
	handle_binding_up(binding);
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
	if (!joydpads[joystick]) {
		return;
	}
	joydpad * dpad = joydpads[joystick] + dpadnum;
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

int parse_binding_target(char * target, tern_node * padbuttons, int * ui_out, int * padnum_out, int * padbutton_out)
{
	int gpadslen = strlen("gamepads.");
	if (!memcmp(target, "gamepads.", gpadslen)) {
		if (target[gpadslen] >= '1' && target[gpadslen] <= '8') {
			int padnum = target[gpadslen] - '0';
			int button = tern_find_int(padbuttons, target + gpadslen + 1, 0);
			if (button) {
				*padnum_out = padnum;
				*padbutton_out = button;
				return 1;
			} else {
				if (target[gpadslen+1]) {
					fprintf(stderr, "Gamepad mapping string '%s' refers to an invalid button '%s'\n", target, target + gpadslen + 1);
				} else {
					fprintf(stderr, "Gamepad mapping string '%s' has no button component\n", target);
				}
			}
		} else {
			fprintf(stderr, "Gamepad mapping string '%s' refers to an invalid gamepad number %c\n", target, target[gpadslen]);
		}
	} else if(!memcmp(target, "ui.", strlen("ui."))) {
		if (!strcmp(target + 3, "vdp_debug_mode")) {
			*ui_out = UI_DEBUG_MODE_INC;
		} else if(!strcmp(target + 3, "vdp_debug_pal")) {
			*ui_out = UI_DEBUG_PAL_INC;
		} else if(!strcmp(target + 3, "enter_debugger")) {
			*ui_out = UI_ENTER_DEBUGGER;
		} else if(!strcmp(target + 3, "save_state")) {
			*ui_out = UI_SAVE_STATE;
		} else if(!strcmp(target + 3, "exit")) {
			*ui_out = UI_EXIT;
		} else {
			fprintf(stderr, "Unreconized UI binding type %s\n", target);
			return 0;
		}
		return 2;
	} else {
		fprintf(stderr, "Unrecognized binding type %s\n", target);
	}
	return 0;
}

void process_keys(tern_node * cur, tern_node * special, tern_node * padbuttons, char * prefix)
{
	char * curstr;
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
	if (cur->el) {
		curstr[len+1] = 0;
		process_keys(cur->straight.next, special, padbuttons, curstr);
	} else {
		int keycode = tern_find_int(special, curstr, 0);
		if (!keycode) {
			keycode = curstr[0];
			if (curstr[1] != 0) {
				fprintf(stderr, "%s is not recognized as a key identifier, truncating to %c\n", curstr, curstr[0]);
			}
		}
		char * target = cur->straight.value.ptrval;
		int ui_func, padnum, button;
		int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
		if (bindtype == 1) {
			bind_gamepad(keycode, padnum, button);
		} else if(bindtype == 2) {
			bind_ui(keycode, ui_func);
		}
	}
	process_keys(cur->left, special, padbuttons, prefix);
	process_keys(cur->right, special, padbuttons, prefix);
}

void set_keybindings()
{
	tern_node * special = tern_insert_int(NULL, "up", RENDERKEY_UP);
	special = tern_insert_int(special, "down", RENDERKEY_DOWN);
	special = tern_insert_int(special, "left", RENDERKEY_LEFT);
	special = tern_insert_int(special, "right", RENDERKEY_RIGHT);
	special = tern_insert_int(special, "enter", '\r');
		special = tern_insert_int(special, "esc", RENDERKEY_ESC);

	tern_node * padbuttons = tern_insert_int(NULL, ".up", DPAD_UP);
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

	tern_node * keys = tern_find_prefix(config, "bindingskeys");
	process_keys(keys, special, padbuttons, NULL);
	char prefix[] = "bindingspads00";
	for (int i = 0; i < 100 && i < render_num_joysticks(); i++)
	{
		if (i < 10) {
			prefix[strlen("bindingspads")] = i + '0';
			prefix[strlen("bindingspads")+1] = 0;
		} else {
			prefix[strlen("bindingspads")] = i/10 + '0';
			prefix[strlen("bindingspads")+1] = i%10 + '0';
		}
		tern_node * pad = tern_find_prefix(config, prefix);
		if (pad) {
			char dprefix[] = "dpads0";
			for (int dpad = 0; dpad < 10 && dpad < render_joystick_num_hats(i); dpad++)
			{
				dprefix[strlen("dpads")] = dpad + '0';
				tern_node * pad_dpad = tern_find_prefix(pad, dprefix);
				char * dirs[] = {"up", "down", "left", "right"};
				int dirnums[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};
				for (int dir = 0; dir < sizeof(dirs)/sizeof(dirs[0]); dir++) {
					char * target = tern_find_ptr(pad_dpad, dirs[dir]);
					if (target) {
						int ui_func, padnum, button;
						int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
						if (bindtype == 1) {
							bind_dpad_gamepad(i, dpad, dirnums[dir], padnum, button);
						}
						//TODO: Handle UI bindings
					}
				}
			}
			char bprefix[] = "buttons00";
			for (int but = 0; but < 100 && but < render_joystick_num_buttons(i); but++)
			{
				if (but < 10) {
					bprefix[strlen("buttons")] = but + '0';
					bprefix[strlen("buttons")+1] = 0;
				} else {
					bprefix[strlen("buttons")] = but/10 + '0';
					bprefix[strlen("buttons")+1] = but%10 + '0';
				}
				char * target = tern_find_ptr(pad, bprefix);
				if (target) {
					int ui_func, padnum, button;
					int bindtype = parse_binding_target(target, padbuttons, &ui_func, &padnum, &button);
					if (bindtype == 1) {
						bind_button_gamepad(i, but, padnum, button);
					}
					//TODO: Handle UI bindings
				}
			}
		}
	}
}

#define TH 0x40
#define TH_TIMEOUT 8000

void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction)
{
	/*uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("adjust_cycles | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, current_cycle);
	}*/
	if (current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	} else {
		pad->timeout_cycle -= deduction;
	}
}

void io_data_write(io_port * pad, uint8_t value, uint32_t current_cycle)
{
	if (pad->control & TH) {
		//check if TH has changed
		if ((pad->output & TH) ^ (value & TH)) {
			if (current_cycle >= pad->timeout_cycle) {
				pad->th_counter = 0;
			}
			if (!(value & TH)) {
				pad->th_counter++;
			}
			pad->timeout_cycle = current_cycle + TH_TIMEOUT;
		}
	}
	pad->output = value;
}

uint8_t io_data_read(io_port * pad, uint32_t current_cycle)
{
	uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	uint8_t input;
	if (current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	}
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("io_data_read | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, context->current_cycle);
	}*/
	if (th) {
		if (pad->th_counter == 3) {
			input = pad->input[GAMEPAD_EXTRA];
		} else {
			input = pad->input[GAMEPAD_TH1];
		}
	} else {
		if (pad->th_counter == 3) {
			input = pad->input[GAMEPAD_TH0] | 0xF;
		} else if(pad->th_counter == 4) {
			input = pad->input[GAMEPAD_TH0]  & 0x30;
		} else {
			input = pad->input[GAMEPAD_TH0] | 0xC;
		}
	}
	uint8_t value = ((~input) & (~control)) | (pad->output & control);
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf ("value: %X\n", value);
	}*/
	return value;
}


