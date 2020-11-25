#define NK_IMPLEMENTATION
#define NK_SDL_GLES2_IMPLEMENTATION
#define NK_RAWFB_IMPLEMENTATION
#define RAWFB_RGBX_8888

#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include "blastem_nuklear.h"
#include "nuklear_rawfb.h"
#include "font.h"
#include "../render.h"
#include "../render_sdl.h"
#include "../util.h"
#include "../paths.h"
#include "../saves.h"
#include "../blastem.h"
#include "../config.h"
#include "../io.h"
#include "../png.h"
#include "../controller_info.h"
#include "../bindings.h"

static struct nk_context *context;
static struct rawfb_context *fb_context;

typedef struct
{
	uint32_t         *image_data;
	uint32_t         width, height;
	struct nk_image  ui;
} ui_image;

static ui_image **ui_images, *controller_360, *controller_ps4, 
	*controller_ps4_6b, *controller_wiiu, *controller_gen_6b;
static uint32_t num_ui_images, ui_image_storage;

typedef void (*view_fun)(struct nk_context *);
static view_fun current_view;
static view_fun *previous_views;
static uint32_t view_storage;
static uint32_t num_prev;
static struct nk_font *def_font;
static uint8_t config_dirty;

static void push_view(view_fun new_view)
{
	if (num_prev == view_storage) {
		view_storage = view_storage ? 2*view_storage : 2;
		previous_views = realloc(previous_views, view_storage*sizeof(view_fun));
	}
	previous_views[num_prev++] = current_view;
	current_view = new_view;
	context->input.selected_widget = 0;
}

static void pop_view()
{
	if (num_prev) {
		current_view = previous_views[--num_prev];
		context->input.selected_widget = 0;
	}
}

static void clear_view_stack()
{
	num_prev = 0;
}

void view_play(struct nk_context *context)
{
	
}

void view_file_browser(struct nk_context *context, uint8_t normal_open)
{
	static char *current_path;
	static dir_entry *entries;
	static size_t num_entries;
	static int32_t selected_entry = -1;
	static char **ext_list;
	static uint32_t num_exts;
	static uint8_t got_ext_list;
	if (!current_path) {
		get_initial_browse_path(&current_path);
	}
	if (!entries) {
		entries = get_dir_list(current_path, &num_entries);
		if (entries) {
			sort_dir_list(entries, num_entries);
		}
		if (!num_entries) {
			//get_dir_list can fail if the user doesn't have permission
			//for the current folder, make sure they can still navigate up
			free_dir_list(entries, num_entries);
			entries = calloc(1, sizeof(dir_entry));
			entries[0].name = strdup("..");
			entries[0].is_dir = 1;
			num_entries = 1;
		}
	}
	if (!got_ext_list) {
		ext_list = get_extension_list(config, &num_exts);
		got_ext_list = 1;
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Load ROM", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - context->style.font->height * 3, width - 60, 1);
		int32_t old_selected = selected_entry;
		char *title = alloc_concat("Select ROM: ", current_path);
		if (nk_group_begin(context, title, NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, context->style.font->height - 2, width-100, 1);
			for (int32_t i = 0; i < num_entries; i++)
			{
				if (entries[i].name[0] == '.' && entries[i].name[1] != '.') {
					continue;
				}
				if (num_exts && !entries[i].is_dir && !path_matches_extensions(entries[i].name, ext_list, num_exts)) {
					continue;
				}
				int selected = i == selected_entry;
				nk_selectable_label(context, entries[i].name, NK_TEXT_ALIGN_LEFT, &selected);
				if (selected) {
					selected_entry = i;
				} else if (i == selected_entry) {
					selected_entry = -1;
				}
			}
			nk_group_end(context);
		}
		free(title);
		nk_layout_row_static(context, context->style.font->height * 1.75, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (nk_button_label(context, "Open") || (old_selected >= 0 && selected_entry < 0)) {
			if (selected_entry < 0) {
				selected_entry = old_selected;
			}
			char *full_path = path_append(current_path, entries[selected_entry].name);
			if (entries[selected_entry].is_dir) {
				free(current_path);
				current_path = full_path;
				free_dir_list(entries, num_entries);
				entries = NULL;
			} else {
				if(normal_open) {
					if (current_system) {
						current_system->next_rom = full_path;
						current_system->request_exit(current_system);
					} else {
						init_system_with_media(full_path, SYSTEM_UNKNOWN);
						free(full_path);
					}
				} else {
					lockon_media(full_path);
					free(full_path);
				}
				clear_view_stack();
				show_play_view();
			}
			selected_entry = -1;
		}
		nk_end(context);
	}
}

void view_load(struct nk_context *context)
{
	view_file_browser(context, 1);
}

void view_lock_on(struct nk_context *context)
{
	view_file_browser(context, 0);
}

void view_about(struct nk_context *context)
{
	const char *lines[] = {
		"BlastEm v0.6.3-pre",
		"Copyright 2012-2019 Michael Pavone",
		"",
		"BlastEm is a high performance open source",
		"(GPLv3) Genesis/Megadrive emulator",
	};
	const uint32_t NUM_LINES = sizeof(lines)/sizeof(*lines);
	const char *thanks[] = {
		"Nemesis: Documentation and test ROMs",
		"Charles MacDonald: Documentation",
		"Eke-Eke: Documentation",
		"Sauraen: YM2612/YM2203 Die Analysis",
		"Alexey Khokholov: YM3438 Die Analysis",
		"Bart Trzynadlowski: Documentation",
		"KanedaFR: Hosting the best Sega forum",
		"Titan: Awesome demos and documentation",
		"flamewing: BCD info and test ROM",
		"r57shell: Opcode size test ROM",
		"micky: Testing",
		"Sasha: Testing",
		"lol-frank: Testing",
		"Sik: Testing",
		"Tim Lawrence : Testing",
		"ComradeOj: Testing",
		"Vladikcomper: Testing"
	};
	const uint32_t NUM_THANKS = sizeof(thanks)/sizeof(*thanks);
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "About", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, context->style.font->height, width-40, 1);
		for (uint32_t i = 0; i < NUM_LINES; i++)
		{
			nk_label(context, lines[i], NK_TEXT_LEFT);
		}
		nk_layout_row_static(context, height - (context->style.font->height * 2 + 20) - (context->style.font->height +4)*NUM_LINES, width-40, 1);
		if (nk_group_begin(context, "Special Thanks", NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, context->style.font->height, width - 80, 1);
			for (uint32_t i = 0; i < NUM_THANKS; i++)
			{
				nk_label(context, thanks[i], NK_TEXT_LEFT);
			}
			nk_group_end(context);
		}
		nk_layout_row_static(context, context->style.font->height * 1.75, width/3, 1);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}

typedef struct {
	const char *title;
	view_fun   next_view;
} menu_item;

static save_slot_info *slots;
static uint32_t num_slots, selected_slot;

void view_choose_state(struct nk_context *context, uint8_t is_load)
{
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Slot Picker", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - context->style.font->height * 3, width - 60, 1);
		if (nk_group_begin(context, "Select Save Slot", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, context->style.font->height - 2, width-100, 1);
			if (!slots) {
				slots = get_slot_info(current_system, &num_slots);
			}
			for (uint32_t i = 0; i < num_slots; i++)
			{
				int selected = i == selected_slot;
				nk_selectable_label(context, slots[i].desc, NK_TEXT_ALIGN_LEFT, &selected);
				if (selected && (slots[i].modification_time || !is_load)) {
					selected_slot = i;
				}
			}
			nk_group_end(context);
		}
		nk_layout_row_static(context, context->style.font->height * 1.75, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (is_load) {
			if (nk_button_label(context, "Load")) {
				current_system->load_state(current_system, selected_slot);
				show_play_view();
			}
		} else {
			if (nk_button_label(context, "Save")) {
				current_system->save_state = selected_slot + 1;
				show_play_view();
			}
		}
		nk_end(context);
	}
}

void view_save_state(struct nk_context *context)
{
	view_choose_state(context, 0);
}

void view_load_state(struct nk_context *context)
{
	view_choose_state(context, 1);
}

typedef void (*menu_handler)(uint32_t index);

static void menu(struct nk_context *context, uint32_t num_entries, const menu_item *items, menu_handler handler)
{
	const uint32_t button_height = context->style.font->height * 1.75;
	const uint32_t ideal_button_width = context->style.font->height * 10;
	const uint32_t button_space = 6;
	
	uint32_t width = render_width();
	uint32_t height = render_height();
	uint32_t top = height/2 - (button_height * num_entries)/2;
	uint32_t button_width = width > ideal_button_width ? ideal_button_width : width;
	uint32_t left = width/2 - button_width/2;
	
	nk_layout_space_begin(context, NK_STATIC, top + button_height * num_entries, num_entries);
	for (uint32_t i = 0; i < num_entries; i++)
	{
		nk_layout_space_push(context, nk_rect(left, top + i * button_height, button_width, button_height-button_space));
		if (nk_button_label(context, items[i].title)) {
			if (items[i].next_view) {
				push_view(items[i].next_view);
				if (current_view == view_save_state || current_view == view_load_state) {
					free_slot_info(slots);
					slots = NULL;
				} else if (current_view == view_play) {
					clear_view_stack();
					set_content_binding_state(1);
				}
			} else {
				handler(i);
			}
		}
	}
	nk_layout_space_end(context);
}

void binding_loop(char *key, tern_val val, uint8_t valtype, void *data)
{
	if (valtype != TVAL_PTR) {
		return;
	}
	tern_node **binding_lookup = data;
	*binding_lookup = tern_insert_ptr(*binding_lookup, val.ptrval, strdup(key));
}

static int32_t keycode;
static const char *set_binding;
static uint8_t bind_click_release, click;
char *set_label;
void binding_group(struct nk_context *context, char *name, const char **binds, const char **bind_names, uint32_t num_binds, tern_node *binding_lookup)
{
	nk_layout_row_static(context, (context->style.font->height + 4)*num_binds+context->style.font->height+30, render_width() - 80, 1);
	if (nk_group_begin(context, name, NK_WINDOW_TITLE)) {
		nk_layout_row_static(context, context->style.font->height, render_width()/2 - 80, 2);
		
		for (int i = 0; i < num_binds; i++)
		{
			char *label_alloc = bind_names ? NULL : path_extension(binds[i]);
			const char *label = label_alloc;
			if (!label) {
				label = bind_names ? bind_names[i] : binds[i];
			}
			nk_label(context, label, NK_TEXT_LEFT);
			if (nk_button_label(context, tern_find_ptr_default(binding_lookup, binds[i], "Not Set"))) {
				set_binding = binds[i];
				set_label = strdup(label);
				bind_click_release = 0;
				keycode = 0;
			}
			if (label_alloc) {
				free(label_alloc);
			}
		}
		nk_group_end(context);
	}
}

static char *get_key_name(int32_t keycode)
{
	char *name = NULL;
	if (keycode > ' ' && keycode < 0x80) {
		//key corresponds to a printable non-whitespace character
		name = malloc(2);
		name[0] = keycode;
		name[1] = 0;
	} else {
		switch (keycode)
		{
		case RENDERKEY_UP: name = "up"; break;
		case RENDERKEY_DOWN: name = "down"; break;
		case RENDERKEY_LEFT: name = "left"; break;
		case RENDERKEY_RIGHT: name = "right"; break;
		case '\r': name = "enter"; break;
		case ' ': name = "space"; break;
		case '\t': name = "tab"; break;
		case '\b': name = "backspace"; break;
		case RENDERKEY_ESC: name = "esc"; break;
		case RENDERKEY_DEL: name = "delete"; break;
		case RENDERKEY_LSHIFT: name = "lshift"; break;
		case RENDERKEY_RSHIFT: name = "rshift"; break;
		case RENDERKEY_LCTRL: name = "lctrl"; break;
		case RENDERKEY_RCTRL: name = "rctrl"; break;
		case RENDERKEY_LALT: name = "lalt"; break;
		case RENDERKEY_RALT: name = "ralt"; break;
		case RENDERKEY_HOME: name = "home"; break;
		case RENDERKEY_END: name = "end"; break;
		case RENDERKEY_PAGEUP: name = "pageup"; break;
		case RENDERKEY_PAGEDOWN: name = "pagedown"; break;
		case RENDERKEY_F1: name = "f1"; break;
		case RENDERKEY_F2: name = "f2"; break;
		case RENDERKEY_F3: name = "f3"; break;
		case RENDERKEY_F4: name = "f4"; break;
		case RENDERKEY_F5: name = "f5"; break;
		case RENDERKEY_F6: name = "f6"; break;
		case RENDERKEY_F7: name = "f7"; break;
		case RENDERKEY_F8: name = "f8"; break;
		case RENDERKEY_F9: name = "f9"; break;
		case RENDERKEY_F10: name = "f10"; break;
		case RENDERKEY_F11: name = "f11"; break;
		case RENDERKEY_F12: name = "f12"; break;
		case RENDERKEY_SELECT: name = "select"; break;
		case RENDERKEY_PLAY: name = "play"; break;
		case RENDERKEY_SEARCH: name = "search"; break;
		case RENDERKEY_BACK: name = "back"; break;
		case RENDERKEY_NP0: name = "np0"; break;
		case RENDERKEY_NP1: name = "np1"; break;
		case RENDERKEY_NP2: name = "np2"; break;
		case RENDERKEY_NP3: name = "np3"; break;
		case RENDERKEY_NP4: name = "np4"; break;
		case RENDERKEY_NP5: name = "np5"; break;
		case RENDERKEY_NP6: name = "np6"; break;
		case RENDERKEY_NP7: name = "np7"; break;
		case RENDERKEY_NP8: name = "np8"; break;
		case RENDERKEY_NP9: name = "np9"; break;
		case RENDERKEY_NP_DIV: name = "np/"; break;
		case RENDERKEY_NP_MUL: name = "np*"; break;
		case RENDERKEY_NP_MIN: name = "np-"; break;
		case RENDERKEY_NP_PLUS: name = "np+"; break;
		case RENDERKEY_NP_ENTER: name = "npenter"; break;
		case RENDERKEY_NP_STOP: name = "np."; break;
		}
		if (name) {
			name = strdup(name);
		}
	}
	return name;
}

void view_key_bindings(struct nk_context *context)
{
	const char *controller1_binds[] = {
		"gamepads.1.up", "gamepads.1.down", "gamepads.1.left", "gamepads.1.right",
		"gamepads.1.a", "gamepads.1.b", "gamepads.1.c",
		"gamepads.1.x", "gamepads.1.y", "gamepads.1.z",
		"gamepads.1.start", "gamepads.1.mode"
	};
	const char *controller2_binds[] = {
		"gamepads.2.up", "gamepads.2.down", "gamepads.2.left", "gamepads.2.right",
		"gamepads.2.a", "gamepads.2.b", "gamepads.2.c",
		"gamepads.2.x", "gamepads.2.y", "gamepads.2.z",
		"gamepads.2.start", "gamepads.2.mode"
	};
	const char *general_binds[] = {
		"ui.exit", "ui.save_state", "ui.toggle_fullscreen", "ui.soft_reset", "ui.reload",
		"ui.screenshot", "ui.vgm_log", "ui.sms_pause", "ui.toggle_keyboard_cpatured", "ui.release_mouse"
	};
	const char *general_names[] = {
		"Show Menu", "Quick Save", "Toggle Fullscreen", "Soft Reset", "Reload Media",
		"Internal Screenshot", "Toggle VGM Log", "SMS Pause", "Capture Keyboard", "Release Mouse"
	};
	const char *speed_binds[] = {
		"ui.next_speed", "ui.prev_speed",
		"ui.set_speed.0", "ui.set_speed.1", "ui.set_speed.2" ,"ui.set_speed.3", "ui.set_speed.4",
		"ui.set_speed.5", "ui.set_speed.6", "ui.set_speed.7" ,"ui.set_speed.8", "ui.set_speed.9",
	};
	const char *speed_names[] = {
		"Next", "Previous",
		"Default Speed", "Set Speed 1", "Set Speed 2", "Set Speed 3", "Set Speed 4",
		"Set Speed 5", "Set Speed 6", "Set Speed 7", "Set Speed 8", "Set Speed 9"
	};
	const char *debug_binds[] = {
		"ui.enter_debugger", "ui.plane_debug", "ui.vram_debug", "ui.cram_debug",
		"ui.compositing_debug", "ui.vdp_debug_mode"
	};
	const char *debug_names[] = {
		"CPU Debugger", "Plane Debugger", "VRAM Debugger", "CRAM Debugger", 
		"Layer Debugger", "Cycle Mode/Pal"
	};
	const uint32_t NUM_C1_BINDS = sizeof(controller1_binds)/sizeof(*controller1_binds);
	const uint32_t NUM_C2_BINDS = sizeof(controller2_binds)/sizeof(*controller2_binds);
	const uint32_t NUM_SPEED_BINDS = sizeof(speed_binds)/sizeof(*speed_binds);
	const uint32_t NUM_GEN_BINDS = sizeof(general_binds)/sizeof(*general_binds);
	const uint32_t NUM_DBG_BINDS = sizeof(debug_binds)/sizeof(*debug_binds);
	static tern_node *binding_lookup;
	if (!binding_lookup) {
		tern_node *bindings = tern_find_path(config, "bindings\0keys\0", TVAL_NODE).ptrval;
		if (bindings) {
			tern_foreach(bindings, binding_loop, &binding_lookup);
		}
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Keyboard Bindings", nk_rect(0, 0, width, height), 0)) {
		binding_group(context, "Controller 1", controller1_binds, NULL, NUM_C1_BINDS, binding_lookup);
		binding_group(context, "Controller 2", controller2_binds, NULL, NUM_C2_BINDS, binding_lookup);
		binding_group(context, "General", general_binds, general_names, NUM_GEN_BINDS, binding_lookup);
		binding_group(context, "Speed Control", speed_binds, speed_names, NUM_SPEED_BINDS, binding_lookup);
		binding_group(context, "Debug", debug_binds, debug_names, NUM_DBG_BINDS, binding_lookup);
		nk_layout_row_static(context, context->style.font->height * 1.1333, (render_width() - 80) / 2, 1);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
	if (set_binding && nk_begin(context, "Set Binding", nk_rect(width/4, height/4, width/2/*width*3/4*/, height/2), NK_WINDOW_TITLE | NK_WINDOW_BORDER)) {
		nk_layout_row_static(context, 30, width/2-30, 1);
		nk_label(context, "Press new key for", NK_TEXT_CENTERED);
		nk_label(context, set_label, NK_TEXT_CENTERED);
		if (nk_button_label(context, "Cancel") && bind_click_release) {
			free(set_label);
			set_binding = set_label = NULL;
		} else if (keycode) {
			char *name = get_key_name(keycode);
			if (name) {
				uint32_t prefix_len = strlen("bindings") + strlen("keys") + 2;
				char * old = tern_find_ptr(binding_lookup, set_binding);
				if (old) {
					uint32_t suffix_len = strlen(old) + 1;
					char *old_path = malloc(prefix_len + suffix_len + 1);
					memcpy(old_path, "bindings\0keys\0", prefix_len);
					memcpy(old_path + prefix_len, old, suffix_len);
					old_path[prefix_len + suffix_len] = 0;
					tern_val old_val;
					if (tern_delete_path(&config, old_path, &old_val) == TVAL_PTR) {
						free(old_val.ptrval);
					}
				}
				uint32_t suffix_len = strlen(name) + 1;
				char *path = malloc(prefix_len + suffix_len + 1);
				memcpy(path, "bindings\0keys\0", prefix_len);
				memcpy(path + prefix_len, name, suffix_len);
				path[prefix_len + suffix_len] = 0;
				
				config_dirty = 1;
				config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(set_binding)}, TVAL_PTR);
				free(path);
				free(name);
				tern_free(binding_lookup);
				binding_lookup = NULL;
			}
			free(set_label);
			set_binding = set_label = NULL;
		} else if (!click) {
			bind_click_release = 1;
		}
		nk_end(context);
	}
}

static int selected_controller;
static controller_info selected_controller_info;
//#define MIN_BIND_BOX_WIDTH 140
#define MAX_BIND_BOX_WIDTH 350

#define AXIS       0x40000000
#define STICKDIR   0x30000000
#define LEFTSTICK  0x10000000
#define RIGHTSTICK 0x20000000
enum {
	UP,DOWN,RIGHT,LEFT,NUM_AXIS_DIRS
};

static char * config_ps_names[] = {
	[SDL_CONTROLLER_BUTTON_A] = "cross",
	[SDL_CONTROLLER_BUTTON_B] = "circle",
	[SDL_CONTROLLER_BUTTON_X] = "square",
	[SDL_CONTROLLER_BUTTON_Y] = "triangle",
	[SDL_CONTROLLER_BUTTON_BACK] = "share",
	[SDL_CONTROLLER_BUTTON_START] = "options",
	[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "l1",
	[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "r1",
	[SDL_CONTROLLER_BUTTON_LEFTSTICK] = "l3",
	[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "r3",
};

typedef struct {	
	const char *button_binds[SDL_CONTROLLER_BUTTON_MAX];
	const char *left_stick[NUM_AXIS_DIRS];
	const char *right_stick[NUM_AXIS_DIRS];
	const char *triggers[2];
} pad_bind_config;

static const char **current_bind_dest;

const char *translate_binding_option(const char *option)
{
	static tern_node *conf_names;
	if (!conf_names) {
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.up", "Pad Up");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.down", "Pad Down");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.left", "Pad Left");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.right", "Pad Right");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.a", "Pad A");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.b", "Pad B");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.c", "Pad C");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.x", "Pad X");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.y", "Pad Y");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.z", "Pad Z");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.start", "Pad Start");
		conf_names = tern_insert_ptr(conf_names, "gamepads.n.mode", "Pad Mode");
		conf_names = tern_insert_ptr(conf_names, "ui.release_mouse", "Release Mouse");
		conf_names = tern_insert_ptr(conf_names, "ui.vdp_debug_mode", "VDP Debug Mode");
		conf_names = tern_insert_ptr(conf_names, "ui.vdp_debug_pal", "VDP Debug Palette");
		conf_names = tern_insert_ptr(conf_names, "ui.enter_debugger", "Enter CPU Debugger");
		conf_names = tern_insert_ptr(conf_names, "ui.screenshot", "Take Screenshot");
		conf_names = tern_insert_ptr(conf_names, "ui.vgm_log", "Toggle VGM Log");
		conf_names = tern_insert_ptr(conf_names, "ui.exit", "Show Menu");
		conf_names = tern_insert_ptr(conf_names, "ui.save_state", "Quick Save");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.0", "Set Speed 0");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.1", "Set Speed 1");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.2", "Set Speed 2");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.3", "Set Speed 3");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.4", "Set Speed 4");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.5", "Set Speed 5");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.6", "Set Speed 6");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.7", "Set Speed 7");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.8", "Set Speed 8");
		conf_names = tern_insert_ptr(conf_names, "ui.set_speed.9", "Set Speed 9");
		conf_names = tern_insert_ptr(conf_names, "ui.next_speed", "Next Speed");
		conf_names = tern_insert_ptr(conf_names, "ui.prev_speed", "Prev. Speed");
		conf_names = tern_insert_ptr(conf_names, "ui.toggle_fullscreen", "Toggle Fullscreen");
		conf_names = tern_insert_ptr(conf_names, "ui.soft_reset", "Soft Reset");
		conf_names = tern_insert_ptr(conf_names, "ui.reload", "Reload ROM");
		conf_names = tern_insert_ptr(conf_names, "ui.sms_pause", "SMS Pause");
		conf_names = tern_insert_ptr(conf_names, "ui.toggle_keyboard_captured", "Toggle Keyboard Capture");
	}
	return tern_find_ptr_default(conf_names, option, (void *)option);
}

static uint8_t controller_binding_changed;
static void bind_option_group(struct nk_context *context, char *name, const char **options, uint32_t num_options)
{
	float margin = context->style.font->height * 2;
	nk_layout_row_static(context, (context->style.font->height + 3) * ((num_options + 2) / 3) + context->style.font->height*2.1, render_width() - margin, 1);
	if (nk_group_begin(context, name, NK_WINDOW_TITLE|NK_WINDOW_NO_SCROLLBAR)) {
		nk_layout_row_static(context, context->style.font->height, (render_width() - margin - context->style.font->height) / 3, 3);
		for (int i = 0; i < num_options; i++)
		{
			if (nk_button_label(context, translate_binding_option(options[i]))) {
				*current_bind_dest = options[i];
				controller_binding_changed = 1;
				pop_view();
			}
		}
		nk_group_end(context);
	}
}

static void view_button_binding(struct nk_context *context)
{
	static const char *pad_opts[] = {
		"gamepads.n.up",
		"gamepads.n.down",
		"gamepads.n.left",
		"gamepads.n.right",
		"gamepads.n.a",
		"gamepads.n.b",
		"gamepads.n.c",
		"gamepads.n.x",
		"gamepads.n.y",
		"gamepads.n.z",
		"gamepads.n.start",
		"gamepads.n.mode"
	};
	static const char *system_buttons[] = {
		"ui.soft_reset",
		"ui.reload",
		"ui.sms_pause"
	};
	static const char *emu_control[] = {
		"ui.save_state",
		"ui.exit",
		"ui.toggle_fullscreen",
		"ui.screenshot",
		"ui.release_mouse",
		"ui.toggle_keyboard_captured"
	};
	static const char *debugger[] = {
		"ui.vdp_debug_mode",
		"ui.vdp_debug_pal",
		"ui.enter_debugger"
	};
	static const char *speeds[] = {
		"ui.next_speed",
		"ui.prev_speed",
		"ui.set_speed.0",
		"ui.set_speed.1",
		"ui.set_speed.2",
		"ui.set_speed.3",
		"ui.set_speed.4",
		"ui.set_speed.5",
		"ui.set_speed.6",
		"ui.set_speed.7",
		"ui.set_speed.8",
		"ui.set_speed.9"
	};
		
	if (nk_begin(context, "Button Binding", nk_rect(0, 0, render_width(), render_height()), 0)) {
		bind_option_group(context, "Controller Buttons", pad_opts, sizeof(pad_opts)/sizeof(*pad_opts));
		bind_option_group(context, "System Buttons", system_buttons, sizeof(system_buttons)/sizeof(*system_buttons));
		bind_option_group(context, "Emulator Control", emu_control, sizeof(emu_control)/sizeof(*emu_control));
		bind_option_group(context, "Debugging", debugger, sizeof(debugger)/sizeof(*debugger));
		bind_option_group(context, "Speed Control", speeds, sizeof(speeds)/sizeof(*speeds));
		
		nk_layout_row_static(context, context->style.font->height, (render_width() - 80)/4, 1);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}

static void binding_box(struct nk_context *context, pad_bind_config *bindings, char *name, float x, float y, float width, int num_binds, int *binds)
{
	const struct nk_user_font *font = context->style.font;
	float row_height = font->height * 2;
	
	char const **labels = calloc(sizeof(char *), num_binds);
	char const ***conf_vals = calloc(sizeof(char *), num_binds);
	float max_width = 0.0f;
	
	int skipped = 0;
	for (int i = 0; i < num_binds; i++)
	{
		if (binds[i] & AXIS) {
			labels[i] = get_axis_label(&selected_controller_info, binds[i] & ~AXIS);
			conf_vals[i] = &bindings->triggers[(binds[i] & ~AXIS) - SDL_CONTROLLER_AXIS_TRIGGERLEFT];
		} else if (binds[i] & STICKDIR) {
			static char const * dirs[] = {"Up", "Down", "Right", "Left"};
			labels[i] = dirs[binds[i] & 3];
			conf_vals[i] = &(binds[i] & LEFTSTICK ? bindings->left_stick : bindings->right_stick)[binds[i] & 3];
		} else {
			labels[i] = get_button_label(&selected_controller_info, binds[i]);
			conf_vals[i] = &bindings->button_binds[binds[i]];
		}
		if (!labels[i]) {
			skipped++;
			continue;
		}
		float lb_width = font->width(font->userdata, font->height, labels[i], strlen(labels[i]));
		max_width = max_width < lb_width ? lb_width : max_width;
	}
	nk_layout_space_push(context, nk_rect(x, y, width, (num_binds - skipped) * (row_height + 4) + 4));
	nk_group_begin(context, name, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR);
	
	float widths[] = {max_width + 3, width - (max_width + 6)};
	nk_layout_row(context, NK_STATIC, row_height, 2, widths);
	for (int i = 0; i < num_binds; i++)
	{
		if (!labels[i]) {
			continue;
		}
		nk_label(context, labels[i], NK_TEXT_LEFT);
		const char *name = *conf_vals[i] ? translate_binding_option(*conf_vals[i]) : "None";
		if (nk_button_label(context, name)) {
			current_bind_dest = conf_vals[i];
			push_view(view_button_binding);
		}
	}
	free(labels);
	free(conf_vals);
	nk_group_end(context);
}

static void button_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	pad_bind_config *bindings = data;
	if (valtype != TVAL_PTR) {
		return;
	}
	int button = render_lookup_button(key);
	if (button != SDL_CONTROLLER_BUTTON_INVALID) {
		bindings->button_binds[button] = val.ptrval;
	}
}

static void axis_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	pad_bind_config *bindings = data;
	if (valtype != TVAL_PTR) {
		return;
	}
	int axis;
	uint8_t is_negative = 0;
	char *period = strchr(key, '.');
	if (period) {
		char *tmp = malloc(period-key + 1);
		memcpy(tmp, key, period-key);
		tmp[period-key] = 0;
		axis = render_lookup_axis(tmp);
		free(tmp);
		is_negative = strcmp(period+1, "negative") == 0;
	} else {
		axis = render_lookup_axis(key);
	}
	switch (axis)
	{
	case SDL_CONTROLLER_AXIS_LEFTX:
	case SDL_CONTROLLER_AXIS_LEFTY:
		bindings->left_stick[(axis - SDL_CONTROLLER_AXIS_LEFTX) * 2 + is_negative] = val.ptrval;
		break;
	case SDL_CONTROLLER_AXIS_RIGHTX:
	case SDL_CONTROLLER_AXIS_RIGHTY:
		bindings->right_stick[(axis - SDL_CONTROLLER_AXIS_RIGHTX) * 2 + is_negative] = val.ptrval;
		break;
	case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
	case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
		bindings->triggers[axis-SDL_CONTROLLER_AXIS_TRIGGERLEFT] = val.ptrval;
		break;
	}
}

enum {
	SIMILAR_CONTROLLERS,
	IDENTICAL_CONTROLLERS,
	BY_INDEX,
	DEFAULT,
	NUM_DEST_TYPES
};

//it would be cleaner to generate this algorithmically for 4th and up,
//but BlastEm only supports 8 controllers currently so it's not worth the effort
static const char *by_index_names[] = {
	"Use for 1st controller",
	"Use for 2nd controller",
	"Use for 3rd controller",
	"Use for 4th controller",
	"Use for 5th controller",
	"Use for 6th controller",
	"Use for 7th controller",
	"Use for 8th controller",
};

static void save_stick_binds(char *axes_key, size_t axes_key_size, const char **bindings, char *prefix)
{
	for (int i = 0; i < NUM_AXIS_DIRS; i++)
	{
		char axis = (i / 2) ? 'x' : 'y';
		char *suffix = (i % 2) ? ".negative" : ".positive";
		size_t prefix_len = strlen(prefix), suffix_len = strlen(suffix);
		size_t full_key_size = axes_key_size + prefix_len + 1 + suffix_len + 2;
		char *full_key = malloc(full_key_size);
		memcpy(full_key, axes_key, axes_key_size);
		memcpy(full_key + axes_key_size, prefix, prefix_len);
		full_key[axes_key_size+prefix_len] = axis;
		memcpy(full_key + axes_key_size + prefix_len + 1, suffix, suffix_len  +1);
		full_key[axes_key_size + prefix_len + 1 + suffix_len + 1] = 0;
		
		if (bindings[i]) {
			tern_insert_path(config, full_key, (tern_val){.ptrval = strdup(bindings[i])}, TVAL_PTR);
		} else {
			tern_val prev_val;
			uint8_t prev_type = tern_delete_path(&config, full_key, &prev_val);
			if (prev_type == TVAL_PTR) {
				free(prev_val.ptrval);
			}
		}
		
		free(full_key);
	}
}

static pad_bind_config *bindings;
static void handle_dest_clicked(uint32_t dest)
{
	char key_buf[12];
	char *key;
	switch (dest)
	{
	case SIMILAR_CONTROLLERS:
		key = make_controller_type_key(&selected_controller_info);
		break;
	case IDENTICAL_CONTROLLERS:
		key = render_joystick_type_id(selected_controller);
		break;
	case BY_INDEX:
		snprintf(key_buf, sizeof(key_buf), "%d", selected_controller);
		key = key_buf;
		break;
	default:
		key = "default";
		break;
	}
	static const char base_path[] = "bindings\0pads";
	size_t pad_key_size = sizeof(base_path) + strlen(key) + 1;
	char *pad_key = malloc(pad_key_size);
	memcpy(pad_key, base_path, sizeof(base_path));
	strcpy(pad_key + sizeof(base_path), key);
	static const char dpad_base[] = "dpads\0""0";
	size_t dpad_key_size = pad_key_size + sizeof(dpad_base);
	char *dpad_key = malloc(dpad_key_size);
	memcpy(dpad_key, pad_key, pad_key_size);
	memcpy(dpad_key + pad_key_size, dpad_base, sizeof(dpad_base));
	static const char button_base[] = "buttons";
	size_t button_key_size = pad_key_size + sizeof(button_base);
	char *button_key = malloc(button_key_size);
	memcpy(button_key, pad_key, pad_key_size);
	memcpy(button_key + pad_key_size, button_base, sizeof(button_base));
	
	char *final_key;
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		char *base;
		const char *suffix;
		size_t base_key_len;
		if ( i < SDL_CONTROLLER_BUTTON_DPAD_UP) {
			suffix = SDL_GameControllerGetStringForButton(i);
			base_key_len = button_key_size;
			base = button_key;
			
			
		} else {
			static const char *dir_keys[] = {"up", "down", "left", "right"};
			suffix = dir_keys[i - SDL_CONTROLLER_BUTTON_DPAD_UP];
			base = dpad_key;
			base_key_len = dpad_key_size;
		}
		size_t suffix_len = strlen(suffix);
		final_key = malloc(base_key_len + suffix_len + 2);
		memcpy(final_key, base, base_key_len);
		memcpy(final_key + base_key_len, suffix, suffix_len + 1);
		final_key[base_key_len + suffix_len + 1] = 0;
		if (bindings->button_binds[i]) {
			tern_insert_path(config, final_key, (tern_val){.ptrval = strdup(bindings->button_binds[i])}, TVAL_PTR);
		} else {
			tern_val prev_val;
			uint8_t prev_type = tern_delete_path(&config, final_key, &prev_val);
			if (prev_type == TVAL_PTR) {
				free(prev_val.ptrval);
			}
		}
		free(final_key);
	}
	free(button_key);
	free(dpad_key);
	
	static const char axes_base[] = "axes";
	size_t axes_key_size = pad_key_size + sizeof(axes_base);
	char *axes_key = malloc(axes_key_size);
	memcpy(axes_key, pad_key, pad_key_size);
	memcpy(axes_key + pad_key_size, axes_base, sizeof(axes_base));
	
	save_stick_binds(axes_key, axes_key_size,bindings->left_stick, "left");
	save_stick_binds(axes_key, axes_key_size,bindings->right_stick, "right");
	for (int i = SDL_CONTROLLER_AXIS_TRIGGERLEFT; i < SDL_CONTROLLER_AXIS_MAX; i++)
	{
		const char *suffix = SDL_GameControllerGetStringForAxis(i);
		size_t suffix_len = strlen(suffix);
		final_key = malloc(axes_key_size + suffix_len + 2);
		memcpy(final_key, axes_key, axes_key_size);
		memcpy(final_key + axes_key_size, suffix, suffix_len + 1);
		final_key[axes_key_size + suffix_len + 1] = 0;
		if (bindings->triggers[i - SDL_CONTROLLER_AXIS_TRIGGERLEFT]) {
			tern_insert_path(config, final_key, (tern_val){.ptrval = strdup(bindings->triggers[i - SDL_CONTROLLER_AXIS_TRIGGERLEFT])}, TVAL_PTR);
		} else {
			tern_val prev_val;
			uint8_t prev_type = tern_delete_path(&config, final_key, &prev_val);
			if (prev_type == TVAL_PTR) {
				free(prev_val.ptrval);
			}
		}
		free(final_key);
	}
	free(axes_key);
	
	free(pad_key);
	if (dest == SIMILAR_CONTROLLERS) {
		free(key);
	}
	pop_view();
	config_dirty = 1;
}

void view_select_binding_dest(struct nk_context *context)
{
	static menu_item options[NUM_DEST_TYPES];
	options[IDENTICAL_CONTROLLERS].title = "Use for identical controllers";
	options[DEFAULT].title = "Use as default";
	options[BY_INDEX].title = by_index_names[selected_controller];
	options[SIMILAR_CONTROLLERS].title = make_human_readable_type_name(&selected_controller_info);
	
	if (nk_begin(context, "Select Binding Dest", nk_rect(0, 0, render_width(), render_height()), NK_WINDOW_NO_SCROLLBAR)) {
		menu(context, NUM_DEST_TYPES, options, handle_dest_clicked);
		nk_end(context);
	}
	free((char *)options[SIMILAR_CONTROLLERS].title);
}

static ui_image *select_best_image(controller_info *info)
{
	if (info->variant != VARIANT_NORMAL || info->type == TYPE_SEGA) {
		if (info->type == TYPE_PSX) {
			return controller_ps4_6b;
		} else {
			return controller_gen_6b;
		}
	} else if (info->type == TYPE_PSX) {
		return controller_ps4;
	} else if (info->type == TYPE_NINTENDO) {
		return controller_wiiu;
	} else {
		return controller_360;
	}
}

void view_controller_bindings(struct nk_context *context)
{
	if (nk_begin(context, "Controller Bindings", nk_rect(0, 0, render_width(), render_height()), NK_WINDOW_NO_SCROLLBAR)) {
		if (!bindings) {
			bindings = calloc(1, sizeof(*bindings));
			tern_node *pad = get_binding_node_for_pad(selected_controller);
			if (pad) {
				tern_foreach(tern_find_node(pad, "buttons"), button_iter, bindings);
				tern_foreach(tern_find_node(pad, "axes"), axis_iter, bindings);
				tern_node *dpad = tern_find_path(pad, "dpads\0" "0\0", TVAL_NODE).ptrval;
				const char *dir_keys[] = {"up", "down", "right", "left"};
				const int button_idx[] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_LEFT};
				for (int i = 0; i < NUM_AXIS_DIRS; i++)
				{
					bindings->button_binds[button_idx[i]] = tern_find_ptr(dpad, dir_keys[i]);
				}
			}
		}
	
		float orig_height = def_font->handle.height;
		def_font->handle.height *= 0.5f;
		
		uint32_t avail_height = render_height() - 2 * orig_height;
		float desired_width = render_width() * 0.5f, desired_height = avail_height * 0.5f;
		ui_image *controller_image = select_best_image(&selected_controller_info);
		
		float controller_ratio = (float)controller_image->width / (float)controller_image->height;
		
		const struct nk_user_font *font = context->style.font;
		int MIN_BIND_BOX_WIDTH = font->width(font->userdata, font->height, "Right", strlen("Right"))
			+ def_font->handle.width(font->userdata, font->height, "Internal Screenshot", strlen("Internal Screenshot"));
		
		if (render_width() - desired_width < 2.5f*MIN_BIND_BOX_WIDTH) {
			desired_width = render_width() - 2.5f*MIN_BIND_BOX_WIDTH;
		}
		
		if (desired_width / desired_height > controller_ratio) {
			desired_width = desired_height * controller_ratio;
		} else {
			desired_height = desired_width / controller_ratio;
		}
		float img_left = render_width() / 2.0f - desired_width / 2.0f;
		float img_top = avail_height / 2.0f - desired_height / 2.0f;
		float img_right = img_left + desired_width;
		float img_bot = img_top + desired_height;
		nk_layout_space_begin(context, NK_STATIC, avail_height, INT_MAX);
		nk_layout_space_push(context, nk_rect(img_left, img_top, desired_width, desired_height));
		nk_image(context, controller_image->ui);
		
		float bind_box_width = (render_width() - img_right) * 0.8f;
		if (bind_box_width < MIN_BIND_BOX_WIDTH) {
			bind_box_width = render_width() - img_right;
			if (bind_box_width > MIN_BIND_BOX_WIDTH) {
				bind_box_width = MIN_BIND_BOX_WIDTH;
			}
		} else if (bind_box_width > MAX_BIND_BOX_WIDTH) {
			bind_box_width = MAX_BIND_BOX_WIDTH;
		}
		float bind_box_left;
		if (bind_box_width >= (render_width() - img_right)) {
			bind_box_left = img_right;
		} else {
			bind_box_left = img_right + (render_width() - img_right) / 2.0f - bind_box_width / 2.0f;
		}
		
		if (selected_controller_info.variant == VARIANT_NORMAL) {
			binding_box(context, bindings, "Action Buttons", bind_box_left, img_top, bind_box_width, 4, (int[]){
				SDL_CONTROLLER_BUTTON_A,
				SDL_CONTROLLER_BUTTON_B,
				SDL_CONTROLLER_BUTTON_X,
				SDL_CONTROLLER_BUTTON_Y
			});
		} else {
			binding_box(context, bindings, "Action Buttons", bind_box_left, img_top, bind_box_width, 6, (int[]){
				SDL_CONTROLLER_BUTTON_A,
				SDL_CONTROLLER_BUTTON_B,
				selected_controller_info.variant == VARIANT_6B_RIGHT ? AXIS | SDL_CONTROLLER_AXIS_TRIGGERRIGHT : SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
				SDL_CONTROLLER_BUTTON_X,
				SDL_CONTROLLER_BUTTON_Y,
				selected_controller_info.variant == VARIANT_6B_RIGHT ? SDL_CONTROLLER_BUTTON_RIGHTSHOULDER : SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
			});
		}
		
		if (selected_controller_info.variant == VARIANT_NORMAL) {
			binding_box(context, bindings, "Right Shoulder", bind_box_left, font->height/2, bind_box_width, 2, (int[]){
				SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
				AXIS | SDL_CONTROLLER_AXIS_TRIGGERRIGHT
			});
		} else {
			binding_box(context, bindings, "Right Shoulder", bind_box_left, font->height/2, bind_box_width,
				selected_controller_info.variant == VARIANT_6B_BUMPERS ? 1 : 2, 
				(int[]){
				selected_controller_info.variant == VARIANT_6B_RIGHT ? SDL_CONTROLLER_BUTTON_LEFTSHOULDER : AXIS | SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
				AXIS | SDL_CONTROLLER_AXIS_TRIGGERLEFT
			});
		}
		
		binding_box(context, bindings, "Misc Buttons", (render_width() - bind_box_width) / 2, font->height/2, bind_box_width, 3, (int[]){
			SDL_CONTROLLER_BUTTON_BACK,
			SDL_CONTROLLER_BUTTON_GUIDE,
			SDL_CONTROLLER_BUTTON_START
		});
		
		if (selected_controller_info.variant == VARIANT_NORMAL)
		{
			binding_box(context, bindings, "Right Stick", img_right - desired_width/3, img_bot, bind_box_width, 5, (int[]){
				RIGHTSTICK | UP,
				RIGHTSTICK | DOWN,
				RIGHTSTICK | LEFT,
				RIGHTSTICK | RIGHT,
				SDL_CONTROLLER_BUTTON_RIGHTSTICK
			});
		}
		
		bind_box_left -= img_right;
		float dpad_left, dpad_top;
		if (selected_controller_info.variant == VARIANT_NORMAL)
		{
			binding_box(context, bindings, "Left Stick", bind_box_left, img_top, bind_box_width, 5, (int[]){
				LEFTSTICK | UP,
				LEFTSTICK | DOWN,
				LEFTSTICK | LEFT,
				LEFTSTICK | RIGHT,
				SDL_CONTROLLER_BUTTON_LEFTSTICK
			});
			dpad_left = img_left - desired_width/6;
			dpad_top = img_bot + font->height * 1.5;
		} else {
			dpad_left = bind_box_left;
			dpad_top = img_top;
		}
		
		if (selected_controller_info.variant == VARIANT_NORMAL) {
			binding_box(context, bindings, "Left Shoulder", bind_box_left, font->height/2, bind_box_width, 2, (int[]){
				SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
				AXIS | SDL_CONTROLLER_AXIS_TRIGGERLEFT
			});
		} else {
			binding_box(context, bindings, "Left Shoulder", bind_box_left, font->height/2, bind_box_width, 
				selected_controller_info.variant == VARIANT_6B_BUMPERS ? 1 : 2, 
				(int[]){
				selected_controller_info.variant == VARIANT_6B_RIGHT ? SDL_CONTROLLER_BUTTON_LEFTSTICK : AXIS | SDL_CONTROLLER_AXIS_TRIGGERLEFT,
				SDL_CONTROLLER_BUTTON_RIGHTSTICK
			});
		}
		
		binding_box(context, bindings, "D-pad", dpad_left, dpad_top, bind_box_width, 4, (int[]){
			SDL_CONTROLLER_BUTTON_DPAD_UP,
			SDL_CONTROLLER_BUTTON_DPAD_DOWN,
			SDL_CONTROLLER_BUTTON_DPAD_LEFT,
			SDL_CONTROLLER_BUTTON_DPAD_RIGHT
		});
		
		nk_layout_space_end(context);
		
		def_font->handle.height = orig_height;
		nk_layout_row_static(context, orig_height + 4, (render_width() - 2*orig_height) / 4, 1);
		if (nk_button_label(context, controller_binding_changed ? "Save" : "Back")) {
			pop_view();
			if (controller_binding_changed) {
				push_view(view_select_binding_dest);
			}
		}
		nk_end(context);
	}
}

static int current_button;
static int current_axis;
static int button_pressed, last_button;
static int hat_moved, hat_value, last_hat, last_hat_value;
static int axis_moved, axis_value, last_axis, last_axis_value;
static char *mapping_string;
static size_t mapping_pos;

static void start_mapping(void)
{
	const char *name;
	mapping_string[mapping_pos++] = ',';
	if (current_button != SDL_CONTROLLER_BUTTON_MAX) {
		name = SDL_GameControllerGetStringForButton(current_button);
	} else {
		name = SDL_GameControllerGetStringForAxis(current_axis);
	}
	size_t namesz = strlen(name);
	memcpy(mapping_string + mapping_pos, name, namesz);
	mapping_pos += namesz;
	mapping_string[mapping_pos++] = ':';
}

static uint8_t initial_controller_config;
#define QUIET_FRAMES 9
static void view_controller_mappings(struct nk_context *context)
{
	char buffer[512];
	static int quiet, button_a = -1, button_a_axis = -1;
	uint8_t added_mapping = 0;
	if (nk_begin(context, "Controllers", nk_rect(0, 0, render_width(), render_height()), NK_WINDOW_NO_SCROLLBAR)) {
		
		nk_layout_space_begin(context, NK_STATIC, render_height() - context->style.font->height, 3);
		
		if (current_button < SDL_CONTROLLER_BUTTON_MAX) {
			snprintf(buffer, sizeof(buffer), "Press Button %s", get_button_label(&selected_controller_info, current_button));
		} else {
			snprintf(buffer, sizeof(buffer), "Move Axis %s", get_axis_label(&selected_controller_info, current_axis));
		}
		
		float height = context->style.font->height * 1.25;
		float top = render_height()/2 - 1.5 * height;
		float width = render_width() - context->style.font->height;
		
		nk_layout_space_push(context, nk_rect(0, top, width, height));
		nk_label(context, buffer, NK_TEXT_CENTERED);
		if (current_button > SDL_CONTROLLER_BUTTON_B) {
			nk_layout_space_push(context, nk_rect(0, top + height, width, height));
			nk_label(context, "OR", NK_TEXT_CENTERED);
		
			nk_layout_space_push(context, nk_rect(0, top + 2.0 * height, width, height));
			snprintf(buffer, sizeof(buffer), "Press Button %s to skip", get_button_label(&selected_controller_info, SDL_CONTROLLER_BUTTON_A));
			nk_label(context, buffer, NK_TEXT_CENTERED);
		}
		
		nk_layout_space_end(context);
		if (quiet) {
			--quiet;
		} else {
			if (button_pressed >= 0 && button_pressed != last_button) {
				if (current_button <= SDL_CONTROLLER_BUTTON_B || button_pressed != button_a) {
					start_mapping();
					mapping_string[mapping_pos++] = 'b';
					if (button_pressed > 9) {
						mapping_string[mapping_pos++] = '0' + button_pressed / 10;
					}
					mapping_string[mapping_pos++] = '0' + button_pressed % 10;
					last_button = button_pressed;
					if (current_button == SDL_CONTROLLER_BUTTON_A) {
						button_a = button_pressed;
					}
				}
				added_mapping = 1;
			} else if (hat_moved >= 0 && hat_value && (hat_moved != last_hat || hat_value != last_hat_value)) {
				start_mapping();
				mapping_string[mapping_pos++] = 'h';
				mapping_string[mapping_pos++] = '0' + hat_moved;
				mapping_string[mapping_pos++] = '.';
				mapping_string[mapping_pos++] = '0' + hat_value;
				added_mapping = 1;
				
				last_hat = hat_moved;
				last_hat_value = hat_value;
			} else if (axis_moved >= 0 && abs(axis_value) > 1000 && (
					axis_moved != last_axis || (
						axis_value/abs(axis_value) != last_axis_value/abs(axis_value) && current_button >= SDL_CONTROLLER_BUTTON_DPAD_UP
					)
				)) {
				if (current_button <= SDL_CONTROLLER_BUTTON_B || axis_moved != button_a_axis) {
					start_mapping();
					if (current_button >= SDL_CONTROLLER_BUTTON_DPAD_UP) {
						mapping_string[mapping_pos++] = axis_value >= 0 ? '+' : '-';
					}
					mapping_string[mapping_pos++] = 'a';
					if (axis_moved > 9) {
						mapping_string[mapping_pos++] = '0' + axis_moved / 10;
					}
					mapping_string[mapping_pos++] = '0' + axis_moved % 10;
					last_axis = axis_moved;
					last_axis_value = axis_value;
				}
				added_mapping = 1;
			}
		}
			
		while (added_mapping) {
			quiet = QUIET_FRAMES;
			if (current_button < SDL_CONTROLLER_BUTTON_MAX) {
				current_button++;
				if (current_button == SDL_CONTROLLER_BUTTON_MAX) {
					current_axis = 0;
					if (get_axis_label(&selected_controller_info, current_axis)) {
						added_mapping = 0;
					}
				} else if (get_button_label(&selected_controller_info, current_button)) {
					added_mapping = 0;
				}
			} else {
				current_axis++;
				if (current_axis == SDL_CONTROLLER_AXIS_MAX) {
					button_a = -1;
					button_a_axis = -1;
					mapping_string[mapping_pos] = 0;
					save_controller_mapping(selected_controller, mapping_string);
					free(mapping_string);
					pop_view();
					if (initial_controller_config) {
						push_view(view_controller_bindings);
						controller_binding_changed = 0;
					}
					added_mapping = 0;
				} else if (get_axis_label(&selected_controller_info, current_axis)) {
					added_mapping = 0;
				}
			}
		}
		button_pressed = -1;
		hat_moved = -1;
		axis_moved = -1;
		nk_end(context);
	}
}

static void show_mapping_view(void)
{
	current_button = SDL_CONTROLLER_BUTTON_A;
	button_pressed = -1;
	last_button = -1;
	last_hat = -1;
	axis_moved = -1;
	last_axis = -1;
	last_axis_value = 0;
	SDL_Joystick *joy = render_get_joystick(selected_controller);
	const char *name = SDL_JoystickName(joy);
	size_t namesz = strlen(name);
	mapping_string = malloc(512 + namesz);
	for (mapping_pos = 0; mapping_pos < namesz; mapping_pos++)
	{
		char c = name[mapping_pos];
		if (c == ',' || c == '\n' || c == '\r') {
			c = ' ';
		}
		mapping_string[mapping_pos] = c;
	}
	
	push_view(view_controller_mappings);
}

static void view_controller_variant(struct nk_context *context)
{
	uint8_t selected = 0;
	if (nk_begin(context, "Controller Type", nk_rect(0, 0, render_width(), render_height()), 0)) {
		nk_layout_row_static(context, context->style.font->height*1.25, render_width() - context->style.font->height * 2, 1);
		nk_label(context, "", NK_TEXT_CENTERED);
		nk_label(context, "Select the layout that", NK_TEXT_CENTERED);
		nk_label(context, "best matches your controller", NK_TEXT_CENTERED);
		nk_label(context, "", NK_TEXT_CENTERED);
		if (selected_controller_info.subtype == SUBTYPE_GENESIS) {
			if (nk_button_label(context, "3 button")) {
				selected_controller_info.variant = VARIANT_3BUTTON;
				selected = 1;
			}
			if (nk_button_label(context, "Standard 6 button")) {
				selected_controller_info.variant = VARIANT_6B_BUMPERS;
				selected = 1;
			}
			if (nk_button_label(context, "6 button with 2 shoulder buttons")) {
				selected_controller_info.variant = VARIANT_8BUTTON;
				selected = 1;
			}
		} else {
			if (nk_button_label(context, "4 face buttons")) {
				selected_controller_info.variant = VARIANT_NORMAL;
				selected = 1;
			}
			char buffer[512];
			snprintf(buffer, sizeof(buffer), "6 face buttons including %s and %s", 
				get_button_label(&selected_controller_info, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER), 
				get_axis_label(&selected_controller_info, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
			);
			if (nk_button_label(context, buffer)) {
				selected_controller_info.variant = VARIANT_6B_RIGHT;
				selected = 1;
			}
			snprintf(buffer, sizeof(buffer), "6 face buttons including %s and %s", 
				get_button_label(&selected_controller_info, SDL_CONTROLLER_BUTTON_LEFTSHOULDER), 
				get_button_label(&selected_controller_info, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
			);
			if (nk_button_label(context, buffer)) {
				selected_controller_info.variant = VARIANT_6B_BUMPERS;
				selected = 1;
			}
		}
		nk_end(context);
	}
	if (selected) {
		save_controller_info(selected_controller, &selected_controller_info);
		pop_view();
		if (initial_controller_config) {
			SDL_GameController *controller = render_get_controller(selected_controller);
			if (controller) {
				push_view(view_controller_bindings);
				controller_binding_changed = 0;
				SDL_GameControllerClose(controller);
			} else {
				show_mapping_view();
			}
		}
	}
}

static void controller_type_group(struct nk_context *context, char *name, int type_id, int first_subtype_id, const char **types, uint32_t num_types)
{
	nk_layout_row_static(context, (context->style.font->height + 3) * num_types + context->style.font->height, render_width() - 80, 1);
	if (nk_group_begin(context, name, NK_WINDOW_TITLE)) {
		nk_layout_row_static(context, context->style.font->height, render_width()/2 - 80, 2);
		for (int i = 0; i < num_types; i++)
		{
			if (nk_button_label(context, types[i])) {
				selected_controller_info.type = type_id;
				selected_controller_info.subtype = first_subtype_id + i;
				pop_view();
				if (selected_controller_info.subtype == SUBTYPE_SATURN) {
					selected_controller_info.variant = VARIANT_6B_BUMPERS;
					save_controller_info(selected_controller, &selected_controller_info);
					if (initial_controller_config) {
						SDL_GameController *controller = render_get_controller(selected_controller);
						if (controller) {
							push_view(view_controller_bindings);
							controller_binding_changed = 0;
							SDL_GameControllerClose(controller);
						} else {
							show_mapping_view();
						}
					}
				} else {
					push_view(view_controller_variant);
				}
			}
		}
		nk_group_end(context);
	}
}

void view_controller_type(struct nk_context *context)
{
	if (nk_begin(context, "Controller Type", nk_rect(0, 0, render_width(), render_height()), 0)) {
		controller_type_group(context, "Xbox", TYPE_XBOX, SUBTYPE_XBOX, (const char *[]){
			"Original", "Xbox 360", "Xbox One"
		}, 3);
		controller_type_group(context, "Playstation", TYPE_PSX, SUBTYPE_PS3, (const char *[]){
			"PS3", "PS4"
		}, 2);
		controller_type_group(context, "Sega", TYPE_SEGA, SUBTYPE_GENESIS, (const char *[]){
			"Genesis", "Saturn"
		}, 2);
		controller_type_group(context, "Nintendo", TYPE_NINTENDO, SUBTYPE_WIIU, (const char *[]){
			"WiiU", "Switch"
		}, 2);
		nk_end(context);
	}
}

void view_controllers(struct nk_context *context)
{
	if (nk_begin(context, "Controllers", nk_rect(0, 0, render_width(), render_height()), NK_WINDOW_NO_SCROLLBAR)) {
		int height = (render_height() - 2*context->style.font->height) / 5;
		int inner_height = height - context->style.window.spacing.y;
		const struct nk_user_font *font = context->style.font;
		int bindings_width = font->width(font->userdata, font->height, "Bindings", strlen("Bindings")) + context->style.button.padding.x * 2;
		int remap_width = font->width(font->userdata, font->height, "Remap", strlen("Remap")) + context->style.button.padding.x * 2;
		int change_type_width = font->width(font->userdata, font->height, "Change Type", strlen("Change Type")) + context->style.button.padding.x * 2;
		int total = bindings_width + remap_width + change_type_width;
		float bindings_ratio = (float)bindings_width / total;
		float remap_ratio = (float)remap_width / total;
		float change_type_ratio = (float)change_type_width / total;
		
		
		uint8_t found_controller = 0;
		for (int i = 0; i < MAX_JOYSTICKS; i++)
		{
			SDL_Joystick *joy = render_get_joystick(i);
			if (joy) {
				found_controller = 1;
				controller_info info = get_controller_info(i);
				ui_image *controller_image = select_best_image(&info);
				int image_width = inner_height * controller_image->width / controller_image->height;
				nk_layout_space_begin(context, NK_STATIC, height, INT_MAX);
				nk_layout_space_push(context, nk_rect(context->style.font->height / 2, 0, image_width, inner_height));
				if (info.type == TYPE_UNKNOWN || info.type == TYPE_GENERIC_MAPPING) {
					nk_label(context, "?", NK_TEXT_CENTERED);
				} else {
					nk_image(context, controller_image->ui);
				}
				int button_start = image_width + context->style.font->height;
				int button_area_width = render_width() - image_width - 2 * context->style.font->height;
				
				nk_layout_space_push(context, nk_rect(button_start, 0, button_area_width, inner_height/2));
				nk_label(context, info.name, NK_TEXT_CENTERED);
				const struct nk_user_font *font = context->style.font;
				if (info.type == TYPE_UNKNOWN || info.type == TYPE_GENERIC_MAPPING) {
					int button_width = font->width(font->userdata, font->height, "Configure", strlen("Configure"));
					nk_layout_space_push(context, nk_rect(button_start, height/2, button_width, inner_height/2));
					if (nk_button_label(context, "Configure")) {
						selected_controller = i;
						selected_controller_info = info;
						initial_controller_config = 1;
						push_view(view_controller_type);
					}
				} else {
					button_area_width -= 2 * context->style.window.spacing.x;
					bindings_width = bindings_ratio * button_area_width;
					nk_layout_space_push(context, nk_rect(button_start, height/2, bindings_width, inner_height/2));
					if (nk_button_label(context, "Bindings")) {
						selected_controller = i;
						selected_controller_info = info;
						push_view(view_controller_bindings);
						controller_binding_changed = 0;
					}
					button_start += bindings_width + context->style.window.spacing.x;
					remap_width = remap_ratio * button_area_width;
					nk_layout_space_push(context, nk_rect(button_start, height/2, remap_width, inner_height/2));
					if (nk_button_label(context, "Remap")) {
						selected_controller = i;
						selected_controller_info = info;
						initial_controller_config = 0;
						show_mapping_view();
					}
					button_start += remap_width + context->style.window.spacing.x;
					change_type_width = change_type_ratio * button_area_width;
					nk_layout_space_push(context, nk_rect(button_start, height/2, change_type_width, inner_height/2));
					if (nk_button_label(context, "Change Type")) {
						selected_controller = i;
						selected_controller_info = info;
						initial_controller_config = 0;
						push_view(view_controller_type);
					}
				}
				//nk_layout_row_end(context);
			}
		}
		if (!found_controller) {
			nk_layout_row_static(context, context->style.font->height, render_width() - 2 * context->style.font->height, 1);
			nk_label(context, "No controllers detected", NK_TEXT_CENTERED);
		}
		nk_layout_row_static(context, context->style.font->height, (render_width() - 2 * context->style.font->height) / 2, 2);
		nk_label(context, "", NK_TEXT_LEFT);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}

void settings_toggle(struct nk_context *context, char *label, char *path, uint8_t def)
{
	uint8_t curval = !strcmp("on", tern_find_path_default(config, path, (tern_val){.ptrval = def ? "on": "off"}, TVAL_PTR).ptrval);
	nk_label(context, label, NK_TEXT_LEFT);
	uint8_t newval = nk_check_label(context, "", curval);
	if (newval != curval) {
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(newval ? "on" : "off")}, TVAL_PTR);
	}
}

void settings_int_input(struct nk_context *context, char *label, char *path, char *def)
{
	char buffer[12];
	nk_label(context, label, NK_TEXT_LEFT);
	uint32_t curval;
	char *curstr = tern_find_path_default(config, path, (tern_val){.ptrval = def}, TVAL_PTR).ptrval;
	uint32_t len = strlen(curstr);
	if (len > 11) {
		len = 11;
	}
	memcpy(buffer, curstr, len);
	memset(buffer+len, 0, sizeof(buffer)-len);
	nk_edit_string(context, NK_EDIT_SIMPLE, buffer, &len, sizeof(buffer)-1, nk_filter_decimal);
	buffer[len] = 0;
	if (strcmp(buffer, curstr)) {
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
}

void settings_string(struct nk_context *context, char *label, char *path, char *def)
{
	nk_label(context, label, NK_TEXT_LEFT);
	char *curstr = tern_find_path_default(config, path, (tern_val){.ptrval = def}, TVAL_PTR).ptrval;
	uint32_t len = strlen(curstr);
	uint32_t buffer_len = len > 100 ? len + 1 : 101;
	char *buffer = malloc(buffer_len);
	memcpy(buffer, curstr, len);
	memset(buffer+len, 0, buffer_len-len);
	nk_edit_string(context, NK_EDIT_SIMPLE, buffer, &len, buffer_len-1, nk_filter_default);
	buffer[len] = 0;
	if (strcmp(buffer, curstr)) {
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
	free(buffer);
}

void settings_int_property(struct nk_context *context, char *label, char *name, char *path, int def, int min, int max)
{
	char *curstr = tern_find_path(config, path, TVAL_PTR).ptrval;
	int curval = curstr ? atoi(curstr) : def;
	nk_label(context, label, NK_TEXT_LEFT);
	int val = curval;
	nk_property_int(context, name, min, &val, max, 1, 1.0f);
	if (val != curval) {
		char buffer[12];
		sprintf(buffer, "%d", val);
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
}

void settings_float_property(struct nk_context *context, char *label, char *name, char *path, float def, float min, float max, float step)
{
	char *curstr = tern_find_path(config, path, TVAL_PTR).ptrval;
	float curval = curstr ? atof(curstr) : def;
	nk_label(context, label, NK_TEXT_LEFT);
	float val = curval;
	nk_property_float(context, name, min, &val, max, step, step);
	if (val != curval) {
		char buffer[64];
		sprintf(buffer, "%f", val);
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
}

typedef struct {
	char *fragment;
	char *vertex;
} shader_prog;

shader_prog *get_shader_progs(dir_entry *entries, size_t num_entries, shader_prog *progs, uint32_t *num_existing, uint32_t *storage)
{
	uint32_t num_progs = *num_existing;
	uint32_t prog_storage = *storage;
	uint32_t starting = num_progs;
	
	for (uint32_t i = 0; i < num_entries; i++) {
		if (entries[i].is_dir) {
			continue;
		}
		char *no_ext = basename_no_extension(entries[i].name);
		uint32_t len = strlen(no_ext);
		if (no_ext[len-1] == 'f' && no_ext[len-2] == '.') {
			uint8_t dupe = 0;;
			for (uint32_t j = 0; j < starting; j++) {
				if (!strcmp(entries[i].name, progs[j].fragment)) {
					dupe = 1;
					break;
				}
			}
			if (!dupe) {
				if (num_progs == prog_storage) {
					prog_storage = prog_storage ? prog_storage*2 : 4;
					progs = realloc(progs, sizeof(*progs) * prog_storage);
				}
				progs[num_progs].vertex = NULL;
				progs[num_progs++].fragment = strdup(entries[i].name); 
			}
		}
		free(no_ext);
	}
	
	for (uint32_t i = 0; i < num_entries; i++) {
		if (entries[i].is_dir) {
			continue;
		}
		char *no_ext = basename_no_extension(entries[i].name);
		uint32_t len = strlen(no_ext);
		if (no_ext[len-1] == 'v' && no_ext[len-2] == '.') {
			for (uint32_t j = 0; j < num_progs; j++) {
				if (!strncmp(no_ext, progs[j].fragment, len-1) && progs[j].fragment[len-1] == 'f' && progs[j].fragment[len] == '.') {
					progs[j].vertex = strdup(entries[i].name);
				}
			}
		}
		free(no_ext);
	}
	free_dir_list(entries, num_entries);
	*num_existing = num_progs;
	*storage = prog_storage;
	return progs;
}

shader_prog *get_shader_list(uint32_t *num_out)
{
	char *shader_dir = path_append(get_config_dir(), "shaders");
	size_t num_entries;
	dir_entry *entries = get_dir_list(shader_dir, &num_entries);
	free(shader_dir);
	shader_prog *progs;
	uint32_t num_progs = 0, prog_storage;
	if (num_entries) {
		progs = calloc(num_entries, sizeof(shader_prog));
		prog_storage = num_entries;
		progs = get_shader_progs(entries, num_entries, progs, &num_progs, &prog_storage);
	} else {
		progs = NULL;
		prog_storage = 0;
	}
#ifdef DATA_PATH
	shader_dir = path_append(DATA_PATH, "shaders");
#else
	shader_dir = path_append(get_exe_dir(), "shaders");
#endif
	entries = get_dir_list(shader_dir, &num_entries);
	free(shader_dir);
	progs = get_shader_progs(entries, num_entries, progs, &num_progs, &prog_storage);
	*num_out = num_progs;
	return progs;
}

int32_t find_match(const char **options, uint32_t num_options, char *path, char *def)
{
	char *setting = tern_find_path_default(config, path, (tern_val){.ptrval = def}, TVAL_PTR).ptrval;
	int32_t selected = -1;
	for (uint32_t i = 0; i < num_options; i++)
	{
		if (!strcmp(setting, options[i])) {
			selected = i;
			break;
		}
	}
	if (selected == -1) {
		for (uint32_t i = 0; i < num_options; i++)
		{
			if (!strcmp(def, options[i])) {
				selected = i;
				break;
			}
		}
	}
	return selected;
}

int32_t settings_dropdown_ex(struct nk_context *context, char *label, const char **options, const char **opt_display, uint32_t num_options, int32_t current, char *path)
{
	nk_label(context, label, NK_TEXT_LEFT);
	int32_t next = nk_combo(context, opt_display, num_options, current, 30, nk_vec2(300, 300));
	if (next != current) {
		config_dirty = 1;
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(options[next])}, TVAL_PTR);
	}
	return next;
}

int32_t settings_dropdown(struct nk_context *context, char *label, const char **options, uint32_t num_options, int32_t current, char *path)
{
	return settings_dropdown_ex(context, label, options, options, num_options, current, path);
}

void view_video_settings(struct nk_context *context)
{
	const char *vsync_opts[] = {"on", "off", "tear"};
	const char *vsync_opt_names[] = {
		"On",
		"Off",
		"On, tear if late"
	};
	const uint32_t num_vsync_opts = sizeof(vsync_opts)/sizeof(*vsync_opts);
	static shader_prog *progs;
	static char **prog_names;
	static uint32_t num_progs;
	static uint32_t selected_prog;
	static int32_t selected_vsync = -1;
	if (selected_vsync < 0) {
		selected_vsync = find_match(vsync_opts, num_vsync_opts, "video\0vsync\0", "off");
	}
	if(!progs) {
		progs = get_shader_list(&num_progs);
		prog_names = calloc(num_progs, sizeof(char*));
		for (uint32_t i = 0; i < num_progs; i++)
		{
			prog_names[i] = basename_no_extension(progs[i].fragment);;
			uint32_t len = strlen(prog_names[i]);
			if (len > 2) {
				prog_names[i][len-2] = 0;
			}
			if (!progs[i].vertex) {
				progs[i].vertex = strdup("default.v.glsl");
			}
			if (!strcmp(
				progs[i].fragment,
				tern_find_path_default(config, "video\0fragment_shader\0", (tern_val){.ptrval = "default.f.glsl"}, TVAL_PTR).ptrval
			)) {
				selected_prog = i;
			}
		}
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	uint32_t desired_width = context->style.font->height * 10;
	if (desired_width > width) {
		desired_width = width;
	}
	if (nk_begin(context, "Video Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, context->style.font->height, desired_width, 2);
		settings_toggle(context, "Fullscreen", "video\0fullscreen\0", 0);
		settings_toggle(context, "Open GL", "video\0gl\0", 1);
		settings_toggle(context, "Scanlines", "video\0scanlines\0", 0);
		selected_vsync = settings_dropdown_ex(context, "VSync", vsync_opts, vsync_opt_names, num_vsync_opts, selected_vsync, "video\0vsync\0");
		settings_int_input(context, "Windowed Width", "video\0width\0", "640");
		nk_label(context, "Shader", NK_TEXT_LEFT);
		uint32_t next_selected = nk_combo(context, (const char **)prog_names, num_progs, selected_prog, context->style.font->height, nk_vec2(desired_width, desired_width));
		if (next_selected != selected_prog) {
			selected_prog = next_selected;
			config_dirty = 1;
			config = tern_insert_path(config, "video\0fragment_shader\0", (tern_val){.ptrval = strdup(progs[next_selected].fragment)}, TVAL_PTR);
			config = tern_insert_path(config, "video\0vertex_shader\0", (tern_val){.ptrval = strdup(progs[next_selected].vertex)}, TVAL_PTR);
		}
		settings_int_property(context, "NTSC Overscan", "Top", "video\0ntsc\0overscan\0top\0", 2, 0, 32);
		settings_int_property(context, "", "Bottom", "video\0ntsc\0overscan\0bottom\0", 17, 0, 32);
		settings_int_property(context, "", "Left", "video\0ntsc\0overscan\0left\0", 13, 0, 32);
		settings_int_property(context, "", "Right", "video\0ntsc\0overscan\0right\0", 14, 0, 32);
		settings_int_property(context, "PAL Overscan", "Top", "video\0pal\0overscan\0top\0", 2, 0, 32);
		settings_int_property(context, "", "Bottom", "video\0pal\0overscan\0bottom\0", 17, 0, 32);
		settings_int_property(context, "", "Left", "video\0pal\0overscan\0left\0", 13, 0, 32);
		settings_int_property(context, "", "Right", "video\0pal\0overscan\0right\0", 14, 0, 32);
		
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}

void view_audio_settings(struct nk_context *context)
{
	const char *rates[] = {
		"192000",
		"96000",
		"48000",
		"44100",
		"22050"
	};
	const char *sizes[] = {
		"1024",
		"512",
		"256",
		"128",
		"64"
	};
	const char *dac[] = {
		"zero_offset",
		"linear"
	};
	const char *dac_desc[] = {
		"Zero Offset",
		"Linear"
	};
	const uint32_t num_rates = sizeof(rates)/sizeof(*rates);
	const uint32_t num_sizes = sizeof(sizes)/sizeof(*sizes);
	const uint32_t num_dacs = sizeof(dac)/sizeof(*dac);
	static int32_t selected_rate = -1;
	static int32_t selected_size = -1;
	static int32_t selected_dac = -1;
	if (selected_rate < 0 || selected_size < 0 || selected_dac < 0) {
		selected_rate = find_match(rates, num_rates, "autio\0rate\0", "48000");
		selected_size = find_match(sizes, num_sizes, "audio\0buffer\0", "512");
		selected_dac = find_match(dac, num_dacs, "audio\0fm_dac\0", "zero_offset");
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	uint32_t desired_width = context->style.font->height * 10;
	if (desired_width > width) {
		desired_width = width;
	}
	if (nk_begin(context, "Audio Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, context->style.font->height , desired_width, 2);
		selected_rate = settings_dropdown(context, "Rate in Hz", rates, num_rates, selected_rate, "audio\0rate\0");
		selected_size = settings_dropdown(context, "Buffer Samples", sizes, num_sizes, selected_size, "audio\0buffer\0");
		settings_int_input(context, "Lowpass Cutoff Hz", "audio\0lowpass_cutoff\0", "3390");
		settings_float_property(context, "Gain (dB)", "Overall", "audio\0gain\0", 0, -30.0f, 30.0f, 0.5f);
		settings_float_property(context, "", "FM", "audio\0fm_gain\0", 0, -30.0f, 30.0f, 0.5f);
		settings_float_property(context, "", "PSG", "audio\0psg_gain\0", 0, -30.0f, 30.0f, 0.5f);
		selected_dac = settings_dropdown_ex(context, "FM DAC", dac, dac_desc, num_dacs, selected_dac, "audio\0fm_dac\0");
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}
typedef struct {
	const char **models;
	const char **names;
	uint32_t   num_models;
	uint32_t   storage;
} model_foreach_state;
void model_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	if (valtype != TVAL_NODE) {
		return;
	}
	model_foreach_state *state = data;
	if (state->num_models == state->storage) {
		state->storage *= 2;
		state->models = realloc(state->models, state->storage * sizeof(char *));
		state->names = realloc(state->names, state->storage * sizeof(char *));
	}
	char *def = strdup(key);
	state->models[state->num_models] = def;
	state->names[state->num_models++] = tern_find_ptr_default(val.ptrval, "name", def);
}

typedef struct {
	const char **models;
	const char **names;
} models;

models get_models(uint32_t *num_out)
{
	tern_node *systems = get_systems_config();
	model_foreach_state state = {
		.models = calloc(4, sizeof(char *)),
		.names = calloc(4, sizeof(char *)),
		.num_models = 0,
		.storage = 4
	};
	tern_foreach(systems, model_iter, &state);
	*num_out = state.num_models;
	return (models){
		.models = state.models,
		.names = state.names
	};
}

void view_system_settings(struct nk_context *context)
{
	const char *sync_opts[] = {
		"video",
		"audio"
	};
	const uint32_t num_sync_opts = sizeof(sync_opts)/sizeof(*sync_opts);
	static int32_t selected_sync = -1;
	if (selected_sync < 0) {
		selected_sync = find_match(sync_opts, num_sync_opts, "system\0sync_source\0", "video");
	}
	const char *regions[] = {
		"J - Japan",
		"U - Americas",
		"E - Europe"
	};
	const char *region_codes[] = {"J", "U", "E"};
	const uint32_t num_regions = sizeof(regions)/sizeof(*regions);
	static int32_t selected_region = -1;
	if (selected_region < 0) {
		selected_region = find_match(region_codes, num_regions, "system\0default_region\0", "U");
	}
	static const char **model_opts;
	static const char **model_names;
	static uint32_t num_models;
	if (!model_opts) {
		models m = get_models(&num_models);
		model_opts = m.models;
		model_names = m.names;
	}
	static int32_t selected_model = -1;
	if (selected_model < 0) {
		selected_model = find_match(model_opts, num_models, "system\0model\0", "md1va3");
	}
	
	const char *formats[] = {
		"native",
		"gst"
	};
	const uint32_t num_formats = sizeof(formats)/sizeof(*formats);
	static int32_t selected_format = -1;
	if (selected_format < 0) {
		selected_format = find_match(formats, num_formats, "ui\0state_format\0", "native");
	}
	const char *ram_inits[] = {
		"zero",
		"random"
	};
	const uint32_t num_inits = sizeof(ram_inits)/sizeof(*ram_inits);
	static int32_t selected_init = -1;
	if (selected_init < 0) {
		selected_init = find_match(ram_inits, num_inits, "system\0ram_init\0", "zero");
	}
	const char *io_opts_1[] = {
		"none",
		"gamepad2.1",
		"gamepad3.1",
		"gamepad6.1",
		"mouse.1",
		"saturn keyboard",
		"xband keyboard"
	};
	const char *io_opts_2[] = {
		"none",
		"gamepad2.2",
		"gamepad3.2",
		"gamepad6.2",
		"mouse.1",
		"saturn keyboard",
		"xband keyboard"
	};
	static int32_t selected_io_1 = -1;
	static int32_t selected_io_2 = -1;
	const uint32_t num_io = sizeof(io_opts_1)/sizeof(*io_opts_1);
	if (selected_io_1 < 0 || selected_io_2 < 0) {
		selected_io_1 = find_match(io_opts_1, num_io, "io\0devices\0""1\0", "gamepad6.1");
		selected_io_2 = find_match(io_opts_2, num_io, "io\0devices\0""2\0", "gamepad6.2");
	}
	
	uint32_t width = render_width();
	uint32_t height = render_height();
	uint32_t desired_width = context->style.font->height * 10;
	if (nk_begin(context, "System Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, context->style.font->height, desired_width, 2);
		
		selected_model = settings_dropdown_ex(context, "Model", model_opts, model_names, num_models, selected_model, "system\0model\0");
		selected_io_1 = settings_dropdown_ex(context, "IO Port 1 Device", io_opts_1, device_type_names, num_io, selected_io_1, "io\0devices\0""1\0");
		selected_io_2 = settings_dropdown_ex(context, "IO Port 2 Device", io_opts_2, device_type_names, num_io, selected_io_2, "io\0devices\0""2\0");
		selected_region = settings_dropdown_ex(context, "Default Region", region_codes, regions, num_regions, selected_region, "system\0default_region\0");
		selected_sync = settings_dropdown(context, "Sync Source", sync_opts, num_sync_opts, selected_sync, "system\0sync_source\0");
		settings_int_property(context, "68000 Clock Divider", "", "clocks\0m68k_divider\0", 7, 1, 53);
		selected_format = settings_dropdown(context, "Save State Format", formats, num_formats, selected_format, "ui\0state_format\0");
		selected_init = settings_dropdown(context, "Initial RAM Value", ram_inits, num_inits, selected_init, "system\0ram_init\0");
		settings_toggle(context, "Remember ROM Path", "ui\0remember_path\0", 1);
		settings_toggle(context, "Save config with EXE", "ui\0config_in_exe_dir\0", 0);
		settings_string(context, "Game Save Path", "ui\0save_path\0", "$USERDATA/blastem/$ROMNAME");
		
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}

void view_confirm_reset(struct nk_context *context)
{
	if (nk_begin(context, "Reset Confirm", nk_rect(0, 0, render_width(), render_height()), 0)) {
		uint32_t desired_width = context->style.font->height * 20;
		nk_layout_row_static(context, context->style.font->height, desired_width, 1);
		nk_label(context, "This will reset all settings and controller", NK_TEXT_LEFT);
		nk_label(context, "mappings back to the defaults.", NK_TEXT_LEFT);
		nk_label(context, "Are you sure you want to proceed?", NK_TEXT_LEFT);
		nk_layout_row_static(context, context->style.font->height * 1.5, desired_width / 2, 2);
		if (nk_button_label(context, "Maybe not")) {
			pop_view();
		}
		if (nk_button_label(context, "Yep, delete it all")) {
			delete_custom_config();
			config = load_config();
			delete_controller_info();
			config_dirty = 1;
			pop_view();
		}
		nk_end(context);
	}
}

void view_back(struct nk_context *context)
{
	pop_view();
	pop_view();
	current_view(context);
}

void view_settings(struct nk_context *context)
{
	static menu_item items[] = {
		{"Key Bindings", view_key_bindings},
		{"Controllers", view_controllers},
		{"Video", view_video_settings},
		{"Audio", view_audio_settings},
		{"System", view_system_settings},
		{"Reset to Defaults", view_confirm_reset},
		{"Back", view_back}
	};
	
	if (nk_begin(context, "Settings Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items, NULL);
		nk_end(context);
	}
}

void exit_handler(uint32_t index)
{
	exit(0);
}

void view_pause(struct nk_context *context)
{
	static menu_item items[] = {
		{"Resume", view_play},
		{"Load ROM", view_load},
		{"Lock On", view_lock_on},
		{"Save State", view_save_state},
		{"Load State", view_load_state},
		{"Settings", view_settings},
		{"Exit", NULL}
	};
	
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items, exit_handler);
		nk_end(context);
	}
}

void view_menu(struct nk_context *context)
{
	static menu_item items[] = {
		{"Load ROM", view_load},
		{"Settings", view_settings},
		{"About", view_about},
		{"Exit", NULL}
	};
	
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items, exit_handler);
		nk_end(context);
	}
}

void blastem_nuklear_render(void)
{
	if (current_view != view_play) {
		nk_input_end(context);
		current_view(context);
		if (fb_context) {
			fb_context->fb.pixels = render_get_framebuffer(FRAMEBUFFER_UI, &fb_context->fb.pitch);
			nk_rawfb_render(fb_context, nk_rgb(0,0,0), 0);
			render_framebuffer_updated(FRAMEBUFFER_UI, render_width());
		} else {
#ifndef DISABLE_OPENGL
			nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
#endif
		}
		nk_input_begin(context);
	}
}

void ui_idle_loop(void)
{
	render_enable_gamepad_events(1);
	const uint32_t MIN_UI_DELAY = 15;
	static uint32_t last;
	while (current_view != view_play)
	{
		uint32_t current = render_elapsed_ms();
		if ((current - last) < MIN_UI_DELAY) {
			render_sleep_ms(MIN_UI_DELAY - (current - last) - 1);
		}
		last = current;
		render_update_display();
	}
	if (config_dirty) {
		apply_updated_config();
		persist_config(config);
		config_dirty = 0;
	}
	render_enable_gamepad_events(0);
}
static void handle_event(SDL_Event *event)
{
	if (event->type == SDL_KEYDOWN) {
		keycode = event->key.keysym.sym;
	}
	else if (event->type == SDL_JOYBUTTONDOWN) {
		button_pressed = event->jbutton.button;
	}
	else if (event->type == SDL_JOYHATMOTION) {
		hat_moved = event->jhat.hat;
		hat_value = event->jhat.value;
	}
	else if (event->type == SDL_JOYAXISMOTION) {
		if (event->jaxis.axis == axis_moved || abs(event->jaxis.value) > abs(axis_value) || abs(event->jaxis.value) > 1000) {
			axis_moved = event->jaxis.axis;
			axis_value = event->jaxis.value;
		}
	} else if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == 0) {
		click = 1;
	} else if (event->type == SDL_MOUSEBUTTONUP && event->button.button == 0) {
		click = 0;
	}
	nk_sdl_handle_event(event);
}

static void context_destroyed(void)
{
	if (context)
	{
		nk_sdl_shutdown();
		context = NULL;
	}
}

static void fb_resize(void)
{
	nk_rawfb_resize_fb(fb_context, NULL, render_width(), render_height(), 0);
}

#ifndef DISABLE_OPENGL
static struct nk_image load_image_texture(uint32_t *buf, uint32_t width, uint32_t height)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#ifdef USE_GLES
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf);
#else
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, buf);
#endif
	return nk_image_id((int)tex);
}
#endif

static struct nk_image load_image_rawfb(uint32_t *buf, uint32_t width, uint32_t height)
{
	struct rawfb_image *fbimg = calloc(1, sizeof(struct rawfb_image));
	fbimg->pixels = buf;
	fbimg->pitch = width * sizeof(uint32_t);
	fbimg->w = width;
	fbimg->h = height;
	fbimg->format = NK_FONT_ATLAS_RGBA32;
	return nk_image_ptr(fbimg);
}

static void texture_init(void)
{
	struct nk_font_atlas *atlas;
	if (fb_context) {
		nk_rawfb_font_stash_begin(fb_context, &atlas);
	} else {
#ifndef DISABLE_OPENGL
		nk_sdl_font_stash_begin(&atlas);
#endif
	}
	uint32_t font_size;
	uint8_t *font = default_font(&font_size);
	if (!font) {
		fatal_error("Failed to find default font path\n");
	}
	def_font = nk_font_atlas_add_from_memory(atlas, font, font_size, render_height() / 16, NULL);
	free(font);
	if (fb_context) {
		nk_rawfb_font_stash_end(fb_context);
	} else {
#ifndef DISABLE_OPENGL
		nk_sdl_font_stash_end();
#endif
	}
	nk_style_set_font(context, &def_font->handle);
	for (uint32_t i = 0; i < num_ui_images; i++)
	{
#ifndef DISABLE_OPENGL
		if (fb_context) {
#endif
			ui_images[i]->ui = load_image_rawfb(ui_images[i]->image_data, ui_images[i]->width, ui_images[i]->height);
#ifndef DISABLE_OPENGL
		} else {
			ui_images[i]->ui = load_image_texture(ui_images[i]->image_data, ui_images[i]->width, ui_images[i]->height);
		}
#endif
	}
}

static void style_init(void)
{
	context->style.checkbox.padding.x = render_height() / 120;
	context->style.checkbox.padding.y = render_height() / 120;
	context->style.checkbox.border = render_height() / 240;
	context->style.checkbox.cursor_normal.type = NK_STYLE_ITEM_COLOR;
	context->style.checkbox.cursor_normal.data.color = (struct nk_color){
		.r = 255, .g = 128, .b = 0, .a = 255
	};
	context->style.checkbox.cursor_hover = context->style.checkbox.cursor_normal;
	context->style.property.inc_button.text_hover = (struct nk_color){
		.r = 255, .g = 128, .b = 0, .a = 255
	};
	context->style.property.dec_button.text_hover = context->style.property.inc_button.text_hover;
	context->style.combo.button.text_hover = context->style.property.inc_button.text_hover;
}

static void context_created(void)
{
	context = nk_sdl_init(render_get_window());
	nk_sdl_device_create();
	style_init();
	texture_init();
}

void show_pause_menu(void)
{
	if (current_view == view_play) {
		set_content_binding_state(0);
		context->style.window.background = nk_rgba(0, 0, 0, 128);
		context->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 128));
		current_view = view_pause;
		context->input.selected_widget = 0;
		system_request_exit(current_system, 1);
	} else if (current_system && !set_binding) {
		clear_view_stack();
		show_play_view();
	}
}

void show_play_view(void)
{
	set_content_binding_state(1);
	current_view = view_play;
	context->input.selected_widget = 0;
}

static uint8_t active;
uint8_t is_nuklear_active(void)
{
	return active;
}

uint8_t is_nuklear_available(void)
{
	/*if (!render_has_gl()) {
		//currently no fallback if GL2 unavailable
		return 0;
	}*/
	char *style = tern_find_path(config, "ui\0style\0", TVAL_PTR).ptrval;
	if (!style) {
		return 1;
	}
	return strcmp(style, "rom") != 0;
}

static void persist_config_exit(void)
{
	if (config_dirty) {
		persist_config(config);
	}
}

ui_image *load_ui_image(char *name)
{
	uint32_t buf_size;
	uint8_t *buf = (uint8_t *)read_bundled_file(name, &buf_size);
	if (buf) {
		num_ui_images++;
		if (num_ui_images > ui_image_storage) {
			ui_image_storage = (ui_image_storage + 1) * 2;
			ui_images = realloc(ui_images, ui_image_storage * sizeof(*ui_images));
		}
		ui_image *this_image = ui_images[num_ui_images-1] = calloc(1, sizeof(ui_image));
		this_image->image_data = load_png(buf, buf_size, &this_image->width, &this_image->height);
#ifdef USE_GLES
		uint32_t *cur = this_image->image_data;
		for (int i = 0; i < this_image->width*this_image->height; i++, cur++)
		{
			uint32_t pixel = *cur;
			*cur = (pixel & 0xFF00FF00) | (pixel << 16 & 0xFF0000) | (pixel >> 16 & 0xFF);
		}
#endif
		free(buf);
		if (!this_image->image_data) {
			num_ui_images--;
			free(this_image);
			return NULL;
		}
		return this_image;
	} else {
		return NULL;
	}
}

void blastem_nuklear_init(uint8_t file_loaded)
{
	context = nk_sdl_init(render_get_window());
#ifndef DISABLE_OPENGL
	if (render_has_gl()) {
		nk_sdl_device_create();
	} else {
#endif
		fb_context = nk_rawfb_init(NULL, context, render_width(), render_height(), 0);
		render_set_ui_fb_resize_handler(fb_resize);
#ifndef DISABLE_OPENGL
	}
#endif
	style_init();
	
	controller_360 = load_ui_image("images/360.png");
	controller_ps4 = load_ui_image("images/ps4.png");
	controller_ps4_6b = load_ui_image("images/ps4_6b.png");
	controller_wiiu = load_ui_image("images/wiiu.png");
	controller_gen_6b = load_ui_image("images/genesis_6b.png");
	
	texture_init();
	
	if (file_loaded) {
		current_view = view_play;
	} else {
		current_view = view_menu;
		set_content_binding_state(0);
	}
	render_set_ui_render_fun(blastem_nuklear_render);
	render_set_event_handler(handle_event);
	render_set_gl_context_handlers(context_destroyed, context_created);
	
	atexit(persist_config_exit);
	
	active = 1;
	ui_idle_loop();
}
