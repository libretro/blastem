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

static struct nk_context *context;

typedef void (*view_fun)(struct nk_context *);
static view_fun current_view;
static view_fun previous_view;

void view_play(struct nk_context *context)
{
	
}

void view_load(struct nk_context *context)
{
	static char *current_path;
	static dir_entry *entries;
	static size_t num_entries;
	static uint32_t selected_entry;
	get_initial_browse_path(&current_path);
	if (!entries) {
		entries = get_dir_list(current_path, &num_entries);
	}
	uint32_t width = render_width();
	uint32_t height = render_height();
	if (nk_begin(context, "Load ROM", nk_rect(0, 0, width, height), 0)) {
		nk_layout_row_static(context, height - 100, width - 60, 1);
		if (nk_group_begin(context, "Select ROM", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
			nk_layout_row_static(context, 28, width-100, 1);
			for (uint32_t i = 0; i < num_entries; i++)
			{
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
			current_view = previous_view;
		}
		if (nk_button_label(context, "Open")) {
			char const *pieces[] = {current_path, PATH_SEP, entries[selected_entry].name};
			if (entries[selected_entry].is_dir) {
				char *old = current_path;
				current_path = alloc_concat_m(3, pieces);
				free(old);
				free_dir_list(entries, num_entries);
				entries = NULL;
			} else {
				current_system->next_rom =  alloc_concat_m(3, pieces);
				current_system->request_exit(current_system);
				current_view = view_play;
			}
		}
		nk_end(context);
	}
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
			current_view = previous_view;
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
			previous_view = current_view;
			current_view = items[i].next_view;
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

void view_pause(struct nk_context *context)
{
	static menu_item items[] = {
		{"Resume", view_play},
		{"Load ROM", view_load},
		{"Save State", view_save_state},
		{"Load State", view_load_state},
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

void idle_loop(void)
{
	while (current_view != view_play)
	{
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
}

static uint8_t active;
uint8_t is_nuklear_active(void)
{
	return active;
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
	idle_loop();
}
