#include <string.h>
#include <stdlib.h>
#include "render.h"
#include "system.h"
#include "io.h"
#include "blastem.h"
#include "saves.h"
#include "util.h"
#include "genesis.h"
#include "sms.h"
#include "menu.h"
#include "bindings.h"
#include "controller_info.h"
#ifndef DISABLE_NUKLEAR
#include "nuklear_ui/blastem_nuklear.h"
#endif

enum {
	BIND_NONE,
	BIND_UI,
	BIND_GAMEPAD,
	BIND_MOUSE
};

typedef enum {
	UI_DEBUG_MODE_INC,
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
	UI_VGM_LOG,
	UI_EXIT,
	UI_PLANE_DEBUG,
	UI_VRAM_DEBUG,
	UI_CRAM_DEBUG,
	UI_COMPOSITE_DEBUG
} ui_action;

typedef struct {
	uint8_t bind_type;
	uint8_t subtype_a;
	uint8_t subtype_b;
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
	keybinding buttons[MAX_MOUSE_BUTTONS];
	keybinding motion;
} mousebinding;

#define DEFAULT_JOYBUTTON_ALLOC 12
static keybinding *bindings[0x10000];
static joystick joysticks[MAX_JOYSTICKS];
static mousebinding mice[MAX_MICE];
const uint8_t dpadbits[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};

static void do_bind(keybinding *binding, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b)
{
	binding->bind_type = bind_type;
	binding->subtype_a = subtype_a;
	binding->subtype_b = subtype_b;
}

void bind_key(int keycode, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b)
{
	int bucket = keycode >> 15 & 0xFFFF;
	if (!bindings[bucket]) {
		bindings[bucket] = malloc(sizeof(keybinding) * 0x8000);
		memset(bindings[bucket], 0, sizeof(keybinding) * 0x8000);
	}
	int idx = keycode & 0x7FFF;
	do_bind(bindings[bucket] + idx, bind_type, subtype_a, subtype_b);
}

void bind_button(int joystick, int button, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b)
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
	do_bind(joysticks[joystick].buttons + button, bind_type, subtype_a, subtype_b);
}

void bind_dpad(int joystick, int dpad, int direction, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b)
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
			do_bind(joysticks[joystick].dpads[dpad].bindings + i, bind_type, subtype_a, subtype_b);
			break;
		}
	}
}

void bind_axis(int joystick, int axis, int positive, uint8_t bind_type, uint8_t subtype_a, uint8_t subtype_b)
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
		do_bind(&joysticks[joystick].axes[axis].positive, bind_type, subtype_a, subtype_b);
	} else {
		do_bind(&joysticks[joystick].axes[axis].negative, bind_type, subtype_a, subtype_b);
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

static uint8_t content_binds_enabled = 1;
void set_content_binding_state(uint8_t enabled)
{
	content_binds_enabled = enabled;
}

void handle_binding_down(keybinding * binding)
{
	if (!current_system) {
		return;
	}
	if (binding->bind_type == BIND_GAMEPAD && current_system && current_system->gamepad_down)
	{
		current_system->gamepad_down(current_system, binding->subtype_a, binding->subtype_b);
	}
	else if (binding->bind_type == BIND_MOUSE && current_system && current_system->mouse_down)
	{
		current_system->mouse_down(current_system, binding->subtype_a, binding->subtype_b);
	}
}

static uint8_t keyboard_captured;
void handle_keydown(int keycode, uint8_t scancode)
{
	int bucket = keycode >> 15 & 0xFFFF;
	int idx = keycode & 0x7FFF;
	keybinding * binding = bindings[bucket] ? bindings[bucket] + idx : NULL;
	if (binding && (!keyboard_captured || (binding->bind_type == BIND_UI && binding->subtype_a == UI_TOGGLE_KEYBOARD_CAPTURE))) {
		handle_binding_down(binding);
	} else if (keyboard_captured && current_system && current_system->keyboard_down) {
		current_system->keyboard_down(current_system, scancode);
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

static uint8_t mouse_mode = MOUSE_NONE;
static uint8_t mouse_captured;
void handle_mousedown(int mouse, int button)
{
	if (mouse_mode == MOUSE_CAPTURE && !mouse_captured) {
		mouse_captured = 1;
		render_relative_mouse(1);
		return;
	}
	if (mouse >= MAX_MICE || button > MAX_MOUSE_BUTTONS || button <= 0) {
		return;
	}
	keybinding * binding = mice[mouse].buttons + button - 1;
	handle_binding_down(binding);
}

static int current_speed = 0;
static int num_speeds = 1;
static uint32_t * speeds = NULL;

static uint8_t mouse_captured;

#ifdef _WIN32
#define localtime_r(a,b) localtime(a)
#endif

char *get_content_config_path(char *config_path, char *config_template, char *default_name)
{
	char *base = tern_find_path(config, config_path, TVAL_PTR).ptrval;
	if (!base) {
		base = "$HOME";
	}
	const system_media *media = current_media();
	tern_node *vars = tern_insert_ptr(NULL, "HOME", get_home_dir());
	vars = tern_insert_ptr(vars, "EXEDIR", get_exe_dir());
	vars = tern_insert_ptr(vars, "USERDATA", (char *)get_userdata_dir());
	vars = tern_insert_ptr(vars, "ROMNAME", media->name);
	vars = tern_insert_ptr(vars, "ROMDIR", media->dir);
	base = replace_vars(base, vars, 1);
	tern_free(vars);
	ensure_dir_exists(base);
	time_t now = time(NULL);
	struct tm local_store;
	char fname_part[256];
	char *template = tern_find_path(config, config_template, TVAL_PTR).ptrval;
	if (template) {
		vars = tern_insert_ptr(NULL, "ROMNAME", media->name);
		template = replace_vars(template, vars, 0);
	} else {
		template = strdup(default_name);
	}
	strftime(fname_part, sizeof(fname_part), template, localtime_r(&now, &local_store));
	char const *parts[] = {base, PATH_SEP, fname_part};
	char *path = alloc_concat_m(3, parts);
	free(base);
	free(template);
	return path;
}

void handle_binding_up(keybinding * binding)
{
	uint8_t allow_content_binds = content_binds_enabled && current_system;
	switch(binding->bind_type)
	{
	case BIND_GAMEPAD:
		if (allow_content_binds && current_system->gamepad_up) {
			current_system->gamepad_up(current_system, binding->subtype_a, binding->subtype_b);
		}
		break;
	case BIND_MOUSE:
		if (allow_content_binds && current_system->mouse_up) {
			current_system->mouse_up(current_system, binding->subtype_a, binding->subtype_b);
		}
		break;
	case BIND_UI:
		switch (binding->subtype_a)
		{
		case UI_DEBUG_MODE_INC:
			if (allow_content_binds) {
				current_system->inc_debug_mode(current_system);
			}
			break;
		case UI_ENTER_DEBUGGER:
			if (allow_content_binds) {
				current_system->enter_debugger = 1;
			}
			break;
		case UI_SAVE_STATE:
			if (allow_content_binds) {
				current_system->save_state = QUICK_SAVE_SLOT+1;
			}
			break;
		case UI_NEXT_SPEED:
			if (allow_content_binds) {
				current_speed++;
				if (current_speed >= num_speeds) {
					current_speed = 0;
				}
				printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
				current_system->set_speed_percent(current_system, speeds[current_speed]);
			}
			break;
		case UI_PREV_SPEED:
			if (allow_content_binds) {
				current_speed--;
				if (current_speed < 0) {
					current_speed = num_speeds - 1;
				}
				printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
				current_system->set_speed_percent(current_system, speeds[current_speed]);
			}
			break;
		case UI_SET_SPEED:
			if (allow_content_binds) {
				if (binding->subtype_b < num_speeds) {
					current_speed = binding->subtype_b;
					printf("Setting speed to %d: %d\n", current_speed, speeds[current_speed]);
					current_system->set_speed_percent(current_system, speeds[current_speed]);
				} else {
					printf("Setting speed to %d\n", speeds[current_speed]);
					current_system->set_speed_percent(current_system, speeds[current_speed]);
				}
			}
			break;
		case UI_RELEASE_MOUSE:
			if (mouse_captured) {
				mouse_captured = 0;
				render_relative_mouse(0);
			}
			break;
		case UI_TOGGLE_KEYBOARD_CAPTURE:
			if (allow_content_binds && current_system->has_keyboard) {
				keyboard_captured = !keyboard_captured;
			}
			break;
		case UI_TOGGLE_FULLSCREEN:
			render_toggle_fullscreen();
			break;
		case UI_SOFT_RESET:
			if (allow_content_binds) {
				current_system->soft_reset(current_system);
			}
			break;
		case UI_RELOAD:
			if (allow_content_binds) {
				reload_media();
			}
			break;
		case UI_SMS_PAUSE:
			if (allow_content_binds && current_system->gamepad_down) {
				current_system->gamepad_down(current_system, GAMEPAD_MAIN_UNIT, MAIN_UNIT_PAUSE);
			}
			break;
		case UI_SCREENSHOT:
			if (allow_content_binds) {
				char *path = get_content_config_path("ui\0screenshot_path\0", "ui\0screenshot_template\0", "blastem_%c.ppm");
				render_save_screenshot(path);
			}
			break;
		case UI_VGM_LOG:
			if (allow_content_binds && current_system->start_vgm_log) {
				if (current_system->vgm_logging) {
					current_system->stop_vgm_log(current_system);
				} else {
					char *path = get_content_config_path("ui\0vgm_path\0", "ui\0vgm_template\0", "blastem_%c.vgm");
					current_system->start_vgm_log(current_system, path);
					free(path);
				}
			}
			break;
		case UI_EXIT:
#ifndef DISABLE_NUKLEAR
			if (is_nuklear_active()) {
				show_pause_menu();
			} else {
#endif
			system_request_exit(current_system, 1);
			if (current_system->type == SYSTEM_GENESIS) {
				genesis_context *gen = (genesis_context *)current_system;
				if (gen->extra) {
					//TODO: More robust mechanism for detecting menu
					menu_context *menu = gen->extra;
					menu->external_game_load = 1;
				}
			}
#ifndef DISABLE_NUKLEAR
			}
#endif
			break;
		case UI_PLANE_DEBUG: 
		case UI_VRAM_DEBUG: 
		case UI_CRAM_DEBUG:
		case UI_COMPOSITE_DEBUG:
			if (allow_content_binds) {
				vdp_context *vdp = NULL;
				if (current_system->type == SYSTEM_GENESIS) {
					genesis_context *gen = (genesis_context *)current_system;
					vdp = gen->vdp;
				} else if (current_system->type == SYSTEM_SMS) {
					sms_context *sms = (sms_context *)current_system;
					vdp = sms->vdp;
				}
				if (vdp) {
					uint8_t debug_type;
					switch(binding->subtype_a)
					{
					case UI_PLANE_DEBUG: debug_type = VDP_DEBUG_PLANE; break;
					case UI_VRAM_DEBUG: debug_type = VDP_DEBUG_VRAM; break;
					case UI_CRAM_DEBUG: debug_type = VDP_DEBUG_CRAM; break;
					case UI_COMPOSITE_DEBUG: debug_type = VDP_DEBUG_COMPOSITE; break;
					default: return;
					}
					vdp_toggle_debug_view(vdp, debug_type);
				}
				break;
			}
		}
		break;
	}
}

void handle_keyup(int keycode, uint8_t scancode)
{
	int bucket = keycode >> 15 & 0xFFFF;
	int idx = keycode & 0x7FFF;
	keybinding * binding = bindings[bucket] ? bindings[bucket] + idx : NULL;
	if (binding && (!keyboard_captured || (binding->bind_type == BIND_UI && binding->subtype_a == UI_TOGGLE_KEYBOARD_CAPTURE))) {
		handle_binding_up(binding);
	} else if (keyboard_captured && current_system && current_system->keyboard_up) {
		current_system->keyboard_up(current_system, scancode);
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

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
	if (mouse >= MAX_MICE || !current_system) {
		return;
	}
	if (mice[mouse].motion.bind_type == BIND_MOUSE && mice[mouse].motion.subtype_b == PSEUDO_BUTTON_MOTION) {
		uint8_t target_mouse = mice[mouse].motion.subtype_a;
		switch(mouse_mode)
		{
		case MOUSE_NONE:
			break;
		case MOUSE_ABSOLUTE: {
			if (current_system->mouse_motion_absolute) {
				float scale_x = (render_emulated_width() * 2.0f) / ((float)render_width());
				float scale_y = (render_emulated_height() * 2.0f) / ((float)render_height());
				int32_t adj_x = x * scale_x + 2 * render_overscan_left() - 2 * BORDER_LEFT;
				int32_t adj_y = y * scale_y + 2 * render_overscan_top() - 4;
				
				current_system->mouse_motion_absolute(current_system, target_mouse, adj_x, adj_y);
			}
			break;
		}
		case MOUSE_RELATIVE: {
			if (current_system->mouse_motion_relative) {
				current_system->mouse_motion_relative(current_system, target_mouse, deltax, deltay);
			}
			break;
		}
		case MOUSE_CAPTURE: {
			if (mouse_captured && current_system->mouse_motion_relative) {
				current_system->mouse_motion_relative(current_system, target_mouse, deltax, deltay);
			}
			break;
		}
		}
	} else {
		handle_binding_up(&mice[mouse].motion);
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

void bindings_release_capture(void)
{
	if (mouse_mode == MOUSE_RELATIVE || (mouse_mode == MOUSE_CAPTURE && mouse_captured)) {
		render_relative_mouse(0);
	}
	keyboard_captured = 0;
}

void bindings_reacquire_capture(void)
{
	if (mouse_mode == MOUSE_RELATIVE || (mouse_mode == MOUSE_CAPTURE && mouse_captured)) {
		render_relative_mouse(1);
	}
}

int parse_binding_target(int device_num, char * target, tern_node * padbuttons, tern_node *mousebuttons, uint8_t * subtype_a, uint8_t * subtype_b)
{
	const int gpadslen = strlen("gamepads.");
	const int mouselen = strlen("mouse.");
	if (startswith(target, "gamepads.")) {
		int padnum = target[gpadslen] == 'n' ? device_num + 1 : target[gpadslen] - '0';
		if (padnum >= 1 && padnum <= 8) {
			int button = tern_find_int(padbuttons, target + gpadslen + 1, 0);
			if (button) {
				*subtype_a = padnum;
				*subtype_b = button;
				return BIND_GAMEPAD;
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
	} else if(startswith(target, "mouse.")) {
		int mousenum = target[mouselen] == 'n' ? device_num + 1 : target[mouselen] - '0';
		if (mousenum >= 1 && mousenum <= 8) {
			int button = tern_find_int(mousebuttons, target + mouselen + 1, 0);
			if (button) {
				*subtype_a = mousenum;
				*subtype_b = button;
				return BIND_MOUSE;
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
	} else if(startswith(target, "ui.")) {
		if (!strcmp(target + 3, "vdp_debug_mode")) {
			*subtype_a = UI_DEBUG_MODE_INC;
		} else if(!strcmp(target + 3, "vdp_debug_pal")) {
			//legacy binding, ignore
			return 0;
		} else if(!strcmp(target + 3, "enter_debugger")) {
			*subtype_a = UI_ENTER_DEBUGGER;
		} else if(!strcmp(target + 3, "save_state")) {
			*subtype_a = UI_SAVE_STATE;
		} else if(startswith(target + 3, "set_speed.")) {
			*subtype_a = UI_SET_SPEED;
			*subtype_b = atoi(target + 3 + strlen("set_speed."));
		} else if(!strcmp(target + 3, "next_speed")) {
			*subtype_a = UI_NEXT_SPEED;
		} else if(!strcmp(target + 3, "prev_speed")) {
			*subtype_a = UI_PREV_SPEED;
		} else if(!strcmp(target + 3, "release_mouse")) {
			*subtype_a = UI_RELEASE_MOUSE;
		} else if(!strcmp(target + 3, "toggle_keyboard_captured")) {
			*subtype_a = UI_TOGGLE_KEYBOARD_CAPTURE;
		} else if (!strcmp(target + 3, "toggle_fullscreen")) {
			*subtype_a = UI_TOGGLE_FULLSCREEN;
		} else if (!strcmp(target + 3, "soft_reset")) {
			*subtype_a = UI_SOFT_RESET;
		} else if (!strcmp(target + 3, "reload")) {
			*subtype_a = UI_RELOAD;
		} else if (!strcmp(target + 3, "sms_pause")) {
			*subtype_a = UI_SMS_PAUSE;
		} else if (!strcmp(target + 3, "screenshot")) {
			*subtype_a = UI_SCREENSHOT;
		} else if (!strcmp(target + 3, "vgm_log")) {
			*subtype_a = UI_VGM_LOG;
		} else if(!strcmp(target + 3, "exit")) {
			*subtype_a = UI_EXIT;
		} else if (!strcmp(target + 3, "plane_debug")) {
			*subtype_a = UI_PLANE_DEBUG;
		} else if (!strcmp(target + 3, "vram_debug")) {
			*subtype_a = UI_VRAM_DEBUG;
		} else if (!strcmp(target + 3, "cram_debug")) {
			*subtype_a = UI_CRAM_DEBUG;
		} else if (!strcmp(target + 3, "compositing_debug")) {
			*subtype_a = UI_COMPOSITE_DEBUG;
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
		uint8_t subtype_a = 0, subtype_b = 0;
		int bindtype = parse_binding_target(0, target, padbuttons, mousebuttons, &subtype_a, &subtype_b);
		bind_key(keycode, bindtype, subtype_a, subtype_b);
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
	uint8_t subtype_a = 0, subtype_b = 0;
	int bindtype = parse_binding_target(state->mouseidx, value.ptrval, state->padbuttons, state->mousebuttons, &subtype_a, &subtype_b);
	mice[state->mouseidx].buttons[buttonnum].bind_type = bindtype;
	mice[state->mouseidx].buttons[buttonnum].subtype_a = subtype_a;
	mice[state->mouseidx].buttons[buttonnum].subtype_b = subtype_b;
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
		uint8_t subtype_a = 0, subtype_b = 0;
		int bindtype = parse_binding_target(mouseidx, motion, padbuttons, mousebuttons, &subtype_a, &subtype_b);
		mice[mouseidx].motion.bind_type = bindtype;
		mice[mouseidx].motion.subtype_a = subtype_a;
		mice[mouseidx].motion.subtype_b = subtype_b;
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
	if (valtype != TVAL_PTR) {
		warning("Pad button %s has a non-scalar value\n", key);
		return;
	}
	uint8_t subtype_a = 0, subtype_b = 0;
	int bindtype = parse_binding_target(hostpadnum, val.ptrval, state->padbuttons, state->mousebuttons, &subtype_a, &subtype_b);
	char *end;
	long hostbutton = strtol(key, &end, 10);
	if (*end) {
		//key is not a valid base 10 integer
		hostbutton = render_translate_input_name(hostpadnum, key, 0);
		if (hostbutton < 0) {
			if (hostbutton == RENDER_INVALID_NAME) {
				warning("%s is not a valid gamepad input name\n", key);
			} else if (hostbutton == RENDER_NOT_MAPPED && hostpadnum != map_warning_pad) {
				debug_message("No SDL 2 mapping exists for input %s on gamepad %d\n", key, hostpadnum);
				map_warning_pad = hostpadnum;
			}
			return;
		}
		if (hostbutton & RENDER_DPAD_BIT) {
			bind_dpad(hostpadnum, render_dpad_part(hostbutton), render_direction_part(hostbutton), bindtype, subtype_a, subtype_b);
			return;
		} else if (hostbutton & RENDER_AXIS_BIT) {
			bind_axis(hostpadnum, render_axis_part(hostbutton), hostbutton & RENDER_AXIS_POS, bindtype, subtype_a, subtype_b);
			return;
		}
	}
	bind_button(hostpadnum, hostbutton, bindtype, subtype_a, subtype_b);
}

void process_pad_axis(char *key, tern_val val, uint8_t valtype, void *data)
{
	key = strdup(key);
	pad_button_state *state = data;
	int hostpadnum = state->padnum;
	if (valtype != TVAL_PTR) {
		warning("Mapping for axis %s has a non-scalar value", key);
		return;
	}
	uint8_t subtype_a = 0, subtype_b = 0;
	int bindtype = parse_binding_target(hostpadnum, val.ptrval, state->padbuttons, state->mousebuttons, &subtype_a, &subtype_b);
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
				debug_message("No SDL 2 mapping exists for input %s on gamepad %d\n", key, hostpadnum);
				map_warning_pad = hostpadnum;
			}
			goto done;
		}
		if (axis & RENDER_DPAD_BIT) {
			bind_dpad(hostpadnum, render_dpad_part(axis), render_direction_part(axis), bindtype, subtype_a, subtype_b);
			goto done;
		} else if (axis & RENDER_AXIS_BIT) {
			axis = render_axis_part(axis);
		} else {
			bind_button(hostpadnum, axis, bindtype, subtype_a, subtype_b);
			goto done;
		}
	}
	bind_axis(hostpadnum, axis, positive, bindtype, subtype_a, subtype_b);
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

tern_node *get_binding_node_for_pad(int padnum)
{
	if (padnum > MAX_JOYSTICKS) {
		return NULL;
	}
	tern_node * pads = tern_find_path(config, "bindings\0pads\0", TVAL_NODE).ptrval;
	if (!pads) {
		return NULL;
	}
	char numstr[11];
	sprintf(numstr, "%d", padnum);
	tern_node * pad = tern_find_node(pads, numstr);
	if (!pad) {
		char *type_id = render_joystick_type_id(padnum);
		pad = tern_find_node(pads, type_id);
		free(type_id);
	}
	if (!pad) {
		controller_info info = get_controller_info(padnum);
		char *key = make_controller_type_key(&info);
		pad = tern_find_node(pads, key);
		free(key);
	}
	if (!pad) {
		pad = tern_find_node(pads, "default");
	}
	return pad;
}

void handle_joy_added(int joystick)
{
	tern_node *pad = get_binding_node_for_pad(joystick);
	if (!pad) {
		return;
	}
	tern_node * dpad_node = tern_find_node(pad, "dpads");
	if (dpad_node) {
		for (int dpad = 0; dpad < 10; dpad++)
		{
			char numstr[2] = {dpad + '0', 0};
			tern_node * pad_dpad = tern_find_node(dpad_node, numstr);
			char * dirs[] = {"up", "down", "left", "right"};
			char *render_dirs[] = {"dpup", "dpdown", "dpleft", "dpright"};
			int dirnums[] = {RENDER_DPAD_UP, RENDER_DPAD_DOWN, RENDER_DPAD_LEFT, RENDER_DPAD_RIGHT};
			for (int dir = 0; dir < sizeof(dirs)/sizeof(dirs[0]); dir++) {
				char * target = tern_find_ptr(pad_dpad, dirs[dir]);
				if (target) {
					uint8_t subtype_a = 0, subtype_b = 0;
					int bindtype = parse_binding_target(joystick, target, get_pad_buttons(), get_mouse_buttons(), &subtype_a, &subtype_b);
					int32_t hostbutton = dpad >0 ? -1 : render_translate_input_name(joystick, render_dirs[dir], 0);
					if (hostbutton < 0) {
						//assume this is a raw dpad mapping
						bind_dpad(joystick, dpad, dirnums[dir], bindtype, subtype_a, subtype_b);
					} else if (hostbutton & RENDER_DPAD_BIT) {
						bind_dpad(joystick, render_dpad_part(hostbutton), render_direction_part(hostbutton), bindtype, subtype_a, subtype_b);
					} else if (hostbutton & RENDER_AXIS_BIT) {
						//SDL2 knows internally whether this should be a positive or negative binding, but doesn't expose that externally
						//for now I'll just assume that any controller with axes for a d-pad has these mapped the "sane" way
						bind_axis(joystick, render_axis_part(hostbutton), dir == 1 || dir == 3 ? 1 : 0, bindtype, subtype_a, subtype_b);
					} else {
						bind_button(joystick, hostbutton, bindtype, subtype_a, subtype_b);
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
}

//only handles keyboards and mice as gamepads are handled on hotplug events
void set_bindings(void)
{
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
	special = tern_insert_int(special, "np0", RENDERKEY_NP0);
	special = tern_insert_int(special, "np1", RENDERKEY_NP1);
	special = tern_insert_int(special, "np2", RENDERKEY_NP2);
	special = tern_insert_int(special, "np3", RENDERKEY_NP3);
	special = tern_insert_int(special, "np4", RENDERKEY_NP4);
	special = tern_insert_int(special, "np5", RENDERKEY_NP5);
	special = tern_insert_int(special, "np6", RENDERKEY_NP6);
	special = tern_insert_int(special, "np7", RENDERKEY_NP7);
	special = tern_insert_int(special, "np8", RENDERKEY_NP8);
	special = tern_insert_int(special, "np9", RENDERKEY_NP9);
	special = tern_insert_int(special, "np/", RENDERKEY_NP_DIV);
	special = tern_insert_int(special, "np*", RENDERKEY_NP_MUL);
	special = tern_insert_int(special, "np-", RENDERKEY_NP_MIN);
	special = tern_insert_int(special, "np+", RENDERKEY_NP_PLUS);
	special = tern_insert_int(special, "npenter", RENDERKEY_NP_ENTER);
	special = tern_insert_int(special, "np.", RENDERKEY_NP_STOP);

	tern_node *padbuttons = get_pad_buttons();

	tern_node *mousebuttons = get_mouse_buttons();
	
	tern_node * keys = tern_find_path(config, "bindings\0keys\0", TVAL_NODE).ptrval;
	process_keys(keys, special, padbuttons, mousebuttons, NULL);
	tern_free(special);
	
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
}

void bindings_set_mouse_mode(uint8_t mode)
{
	mouse_mode = mode;
	if (mode == MOUSE_RELATIVE) {
		render_relative_mouse(1);
	}
}
