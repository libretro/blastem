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
	UI_ENTER_DEBUGGER
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
			render_debug_mode(ui_debug_mode);
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

void set_keybindings()
{
	bind_gamepad(RENDERKEY_UP, 1, DPAD_UP);
	bind_gamepad(RENDERKEY_DOWN, 1, DPAD_DOWN);
	bind_gamepad(RENDERKEY_LEFT, 1, DPAD_LEFT);
	bind_gamepad(RENDERKEY_RIGHT, 1, DPAD_RIGHT);
	bind_gamepad('a', 1, BUTTON_A);
	bind_gamepad('s', 1, BUTTON_B);
	bind_gamepad('d', 1, BUTTON_C);
	bind_gamepad('q', 1, BUTTON_X);
	bind_gamepad('w', 1, BUTTON_Y);
	bind_gamepad('e', 1, BUTTON_Z);
	bind_gamepad('\r', 1, BUTTON_START);
	bind_gamepad('f', 1, BUTTON_MODE);
	bind_ui('[', UI_DEBUG_MODE_INC);
	bind_ui(']', UI_DEBUG_PAL_INC);
	bind_ui('u', UI_ENTER_DEBUGGER);
	
	bind_dpad_gamepad(0, 0, RENDER_DPAD_UP, 2, DPAD_UP);
	bind_dpad_gamepad(0, 0, RENDER_DPAD_DOWN, 2, DPAD_DOWN);
	bind_dpad_gamepad(0, 0, RENDER_DPAD_LEFT, 2, DPAD_LEFT);
	bind_dpad_gamepad(0, 0, RENDER_DPAD_RIGHT, 2, DPAD_RIGHT);
	bind_button_gamepad(0, 0, 2, BUTTON_A);
	bind_button_gamepad(0, 1, 2, BUTTON_B);
	bind_button_gamepad(0, 2, 2, BUTTON_C);
	bind_button_gamepad(0, 3, 2, BUTTON_X);
	bind_button_gamepad(0, 4, 2, BUTTON_Y);
	bind_button_gamepad(0, 5, 2, BUTTON_Z);
	bind_button_gamepad(0, 6, 2, BUTTON_START);
	bind_button_gamepad(0, 7, 2, BUTTON_MODE);
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


