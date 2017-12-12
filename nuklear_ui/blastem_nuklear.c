#define NK_IMPLEMENTATION
#define NK_SDL_GLES2_IMPLEMENTATION

#include <stdlib.h>
#include "blastem_nuklear.h"
#include "font.h"
#include "../render.h"
#include "../render_sdl.h"
#include "../util.h"
#include "../paths.h"
#include "../saves.h"
#include "../blastem.h"
#include "../config.h"
#include "../io.h"

static struct nk_context *context;

typedef void (*view_fun)(struct nk_context *);
static view_fun current_view;
static view_fun *previous_views;
static uint32_t view_storage;
static uint32_t num_prev;

static void push_view(view_fun new_view)
{
	if (num_prev == view_storage) {
		view_storage = view_storage ? 2*view_storage : 2;
		previous_views = realloc(previous_views, view_storage*sizeof(view_fun));
	}
	previous_views[num_prev++] = current_view;
	current_view = new_view;
}

static void pop_view()
{
	if (num_prev) {
		current_view = previous_views[--num_prev];
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
	static uint32_t selected_entry;
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
	}
	if (!got_ext_list) {
		ext_list = get_extension_list(config, &num_exts);
		got_ext_list = 1;
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Load ROM", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - 100, width - 60, 1);
		if (nk_group_begin(context, "Select ROM", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, 28, width-100, 1);
			for (uint32_t i = 0; i < num_entries; i++)
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
				}
			}
			nk_group_end(context);
		}
		nk_layout_row_static(context, 52, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (nk_button_label(context, "Open")) {
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
				current_view = view_play;
			}
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
		nk_layout_row_static(context, height - 100, width - 60, 1);
		if (nk_group_begin(context, "Select Save Slot", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, 28, width-100, 1);
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
		nk_layout_row_static(context, 52, width > 600 ? 300 : width / 2, 2);
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		if (is_load) {
			if (nk_button_label(context, "Load")) {
				current_system->load_state(current_system, selected_slot);
				current_view = view_play;
			}
		} else {
			if (nk_button_label(context, "Save")) {
				current_system->save_state = selected_slot + 1;
				current_view = view_play;
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

static void menu(struct nk_context *context, uint32_t num_entries, const menu_item *items)
{
	const uint32_t button_height = 52;
	const uint32_t ideal_button_width = 300;
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
			push_view(items[i].next_view);
			if (!current_view) {
				exit(0);
			}
			if (current_view == view_save_state || current_view == view_load_state) {
				free_slot_info(slots);
				slots = NULL;
			}
		}
	}
	nk_layout_space_end(context);
}

void view_key_bindings(struct nk_context *context)
{
	
}
void view_controllers(struct nk_context *context)
{
	
}

void settings_toggle(struct nk_context *context, char *label, char *path, uint8_t def)
{
	uint8_t curval = !strcmp("on", tern_find_path_default(config, path, (tern_val){.ptrval = def ? "on": "off"}, TVAL_PTR).ptrval);
	nk_label(context, label, NK_TEXT_LEFT);
	uint8_t newval = nk_check_label(context, "", curval);
	if (newval != curval) {
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
	nk_edit_string(context, NK_EDIT_SIMPLE, buffer, &len, sizeof(buffer)-1, nk_filter_decimal);
	buffer[len] = 0;
	if (strcmp(buffer, curstr)) {
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(buffer)}, TVAL_PTR);
	}
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
					progs = realloc(progs, sizeof(progs) * prog_storage);
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
	shader_dir = path_append(get_exe_dir(), "shaders");
	entries = get_dir_list(shader_dir, &num_entries);
	progs = get_shader_progs(entries, num_entries, progs, &num_progs, &prog_storage);
	*num_out = num_progs;
	return progs;
}

void view_video_settings(struct nk_context *context)
{
	static shader_prog *progs;
	static char **prog_names;
	static uint32_t num_progs;
	static uint32_t selected_prog;
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
	if (nk_begin(context, "Video Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, 30, width > 300 ? 300 : width, 2);
		settings_toggle(context, "Fullscreen", "video\0fullscreen\0", 0);
		settings_toggle(context, "Open GL", "video\0gl\0", 1);
		settings_toggle(context, "Scanlines", "video\0scanlines\0", 0);
		settings_int_input(context, "Windowed Width", "video\0width\0", "640");
		nk_label(context, "Shader", NK_TEXT_LEFT);
		uint32_t next_selected = nk_combo(context, (const char **)prog_names, num_progs, selected_prog, 30, nk_vec2(300, 300));
		if (next_selected != selected_prog) {
			selected_prog = next_selected;
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
		config = tern_insert_path(config, path, (tern_val){.ptrval = strdup(options[next])}, TVAL_PTR);
	}
	return next;
}

int32_t settings_dropdown(struct nk_context *context, char *label, const char **options, uint32_t num_options, int32_t current, char *path)
{
	return settings_dropdown_ex(context, label, options, options, num_options, current, path);
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
	const uint32_t num_rates = sizeof(rates)/sizeof(*rates);
	const uint32_t num_sizes = sizeof(sizes)/sizeof(*sizes);
	static int32_t selected_rate = -1;
	static int32_t selected_size = -1;
	if (selected_rate < 0 || selected_size < 0) {
		selected_rate = find_match(rates, num_rates, "autio\0rate\0", "48000");
		selected_size = find_match(sizes, num_sizes, "audio\0buffer\0", "512");
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Audio Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, 30, width > 300 ? 300 : width, 2);
		selected_rate = settings_dropdown(context, "Rate in Hz", rates, num_rates, selected_rate, "audio\0rate\0");
		selected_size = settings_dropdown(context, "Buffer Samples", sizes, num_sizes, selected_size, "audio\0buffer\0");
		settings_int_input(context, "Lowpass Cutoff Hz", "audio\0lowpass_cutoff\0", "3390");
		if (nk_button_label(context, "Back")) {
			pop_view();
		}
		nk_end(context);
	}
}
void view_system_settings(struct nk_context *context)
{
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
	const char *formats[] = {
		"native",
		"gst"
	};
	const uint32_t num_formats = sizeof(formats)/sizeof(*formats);
	int32_t selected_format = -1;
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
		"gamepad2.1",
		"gamepad3.1",
		"gamepad6.1",
		"mouse",
		"saturn keyboard",
		"xband keyboard"
	};
	const char *io_opts_2[] = {
		"gamepad2.2",
		"gamepad3.2",
		"gamepad6.2",
		"mouse",
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
	if (nk_begin(context, "System Settings", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, 30, width > 300 ? 300 : width, 2);
		settings_int_property(context, "68000 Clock Divider", "", "clocks\0m68k_divider\0", 7, 1, 53);
		settings_toggle(context, "Remember ROM Path", "ui\0remember_path\0", 1);
		selected_region = settings_dropdown_ex(context, "Default Region", region_codes, regions, num_regions, selected_region, "system\0default_region\0");
		selected_format = settings_dropdown(context, "Save State Format", formats, num_formats, selected_format, "ui\0state_format\0");
		selected_init = settings_dropdown(context, "Initial RAM Value", ram_inits, num_inits, selected_init, "system\0ram_init\0");
		selected_io_1 = settings_dropdown_ex(context, "IO Port 1 Device", io_opts_1, device_type_names, num_io, selected_io_1, "io\0devices\0""1\0");
		selected_io_2 = settings_dropdown_ex(context, "IO Port 2 Device", io_opts_2, device_type_names, num_io, selected_io_2, "io\0devices\0""2\0");
		if (nk_button_label(context, "Back")) {
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
		{"Back", view_back}
	};
	
	const uint32_t num_buttons = 6;
	if (nk_begin(context, "Settings Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
		nk_end(context);
	}
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
	
	const uint32_t num_buttons = 3;
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
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
	
	const uint32_t num_buttons = 3;
	if (nk_begin(context, "Main Menu", nk_rect(0, 0, render_width(), render_height()), 0)) {
		menu(context, sizeof(items)/sizeof(*items), items);
		nk_end(context);
	}
}

void blastem_nuklear_render(void)
{
	nk_input_end(context);
	current_view(context);
	nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);
	nk_input_begin(context);
}

void ui_idle_loop(void)
{
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
}
static void handle_event(SDL_Event *event)
{
	nk_sdl_handle_event(event);
}

static void context_destroyed(void)
{
	nk_sdl_device_destroy();
}
static void context_created(void)
{
	nk_sdl_device_create();
	struct nk_font_atlas *atlas;
	nk_sdl_font_stash_begin(&atlas);
	char *font = default_font_path();
	if (!font) {
		fatal_error("Failed to find default font path\n");
	}
	struct nk_font *def_font = nk_font_atlas_add_from_file(atlas, font, 30, NULL);
	nk_sdl_font_stash_end();
	nk_style_set_font(context, &def_font->handle);
}

void show_pause_menu(void)
{
	context->style.window.background = nk_rgba(0, 0, 0, 128);
	context->style.window.fixed_background = nk_style_item_color(nk_rgba(0, 0, 0, 128));
	current_view = view_pause;
	current_system->request_exit(current_system);
}

static uint8_t active;
uint8_t is_nuklear_active(void)
{
	return active;
}

uint8_t is_nuklear_available(void)
{
	if (!render_has_gl()) {
		//currently no fallback if GL2 unavailable
		return 0;
	}
	char *style = tern_find_path(config, "ui\0style\0", TVAL_PTR).ptrval;
	if (!style) {
		return 1;
	}
	return strcmp(style, "rom") != 0;
}

void blastem_nuklear_init(uint8_t file_loaded)
{
	context = nk_sdl_init(render_get_window());
	
	struct nk_font_atlas *atlas;
	nk_sdl_font_stash_begin(&atlas);
	char *font = default_font_path();
	if (!font) {
		fatal_error("Failed to find default font path\n");
	}
	struct nk_font *def_font = nk_font_atlas_add_from_file(atlas, font, 30, NULL);
	nk_sdl_font_stash_end();
	nk_style_set_font(context, &def_font->handle);
	current_view = file_loaded ? view_play : view_menu;
	render_set_ui_render_fun(blastem_nuklear_render);
	render_set_event_handler(handle_event);
	render_set_gl_context_handlers(context_destroyed, context_created);
	active = 1;
	ui_idle_loop();
}
