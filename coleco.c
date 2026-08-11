#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "coleco.h"
#include "render.h"
#include "io.h"
#include "blastem.h"
#include "paths.h"
#include "debug.h"
#include "bindings.h"
#include "saves.h"
#include "media_file.h"

#ifdef NEW_Z80
#define Z80_CYCLE cycles
#define Z80_OPTS opts
#define z80_handle_code_write(...)
#else
#define Z80_CYCLE current_cycle
#define Z80_OPTS options
#endif

static void *coleco_select_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	coleco_context *coleco = z80->system;
	location &= 0xFF;
	coleco->controller_select = location >= 0xC0;
	return vcontext;
}

static uint8_t coleco_controller_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	coleco_context *coleco = z80->system;

	uint8_t index = coleco->controller_select ? 2 : 0;
	if (location & 2) {
		++index;
	}
	return coleco->controller_state[index];
}

static void update_interrupts(coleco_context *coleco)
{
#ifdef NEW_Z80
	if (coleco->z80->nmi_cycle == CYCLE_NEVER) {
#else
	if (coleco->z80->nmi_start == CYCLE_NEVER) {
#endif
		uint32_t nmi = vdp_next_vint(coleco->vdp);
		if (nmi != CYCLE_NEVER) {
			z80_assert_nmi(coleco->z80, nmi);
		}
	}
}

static void *coleco_vdp_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	coleco_context *coleco = z80->system;

	vdp_run_context_full(coleco->vdp, z80->Z80_CYCLE);
	if (location & 1) {
		vdp_control_port_write_pbc(coleco->vdp, value);
		update_interrupts(coleco);
	} else {
		vdp_data_port_write_pbc(coleco->vdp, value);
	}

	return vcontext;
}

static uint8_t coleco_vdp_read(uint32_t location, void *vcontext)
{
	z80_context *z80 = vcontext;
	coleco_context *coleco = z80->system;
	vdp_run_context(coleco->vdp, z80->Z80_CYCLE);
	if (location & 1) {
		uint8_t ret = vdp_control_port_read(coleco->vdp);
		coleco->vdp->flags2 &= ~(FLAG2_VINT_PENDING|FLAG2_HINT_PENDING);
		update_interrupts(coleco);
		return ret;
	} else {
		return vdp_data_port_read_pbc(coleco->vdp);
	}
}

static void *coleco_psg_write(uint32_t location, void *vcontext, uint8_t value)
{
	z80_context *z80 = vcontext;
	coleco_context *coleco = z80->system;

	psg_run(coleco->psg, z80->Z80_CYCLE);
	psg_write(coleco->psg, value);

	return vcontext;
}

void coleco_serialize(coleco_context *coleco, serialize_buffer *buf)
{
	start_section(buf, SECTION_Z80);
	z80_serialize(coleco->z80, buf);
	end_section(buf);

	start_section(buf, SECTION_VDP);
	vdp_serialize(coleco->vdp, buf);
	end_section(buf);

	start_section(buf, SECTION_PSG);
	psg_serialize(coleco->psg, buf);
	end_section(buf);

	start_section(buf, SECTION_MAIN_RAM);
	save_int8(buf, sizeof(coleco->ram) / 1024);
	save_buffer8(buf, coleco->ram, sizeof(coleco->ram));
	end_section(buf);

	start_section(buf, SECTION_COLECO_IO);
	save_buffer8(buf, coleco->controller_state, sizeof(coleco->controller_state));
	save_int8(buf, coleco->controller_select);
	end_section(buf);
}

static uint8_t *serialize(system_header *sys, size_t *size_out)
{
	coleco_context *coleco = (coleco_context *)sys;
	serialize_buffer state;
	init_serialize(&state);
	coleco_serialize(coleco, &state);
	if (size_out) {
		*size_out = state.size;
	}
	return state.data;
}

static void ram_deserialize(deserialize_buffer *buf, void *vcoleco)
{
	coleco_context *coleco = vcoleco;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > sizeof(coleco->ram)) {
		fatal_error("State has a RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, coleco->ram, ram_size);
}

static void controller_deserialize(deserialize_buffer *buf, void *vcoleco)
{
	coleco_context *coleco = vcoleco;
	load_buffer8(buf, coleco->controller_state, sizeof(coleco->controller_state));
	coleco->controller_select = load_int8(buf);
}

void coleco_deserialize(deserialize_buffer *buf, coleco_context *coleco)
{
	register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = coleco->z80}, SECTION_Z80);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = coleco->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = coleco->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = coleco}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = controller_deserialize, .data = coleco}, SECTION_COLECO_IO);
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	z80_invalidate_code_range(coleco->z80, 0x7000, 0x8000);
	free(buf->handlers);
	buf->handlers = NULL;
}

static void deserialize(system_header *sys, uint8_t *data, size_t size)
{
	coleco_context *coleco = (coleco_context *)sys;
	deserialize_buffer buffer;
	init_deserialize(&buffer, data, size);
	coleco_deserialize(&buffer, coleco);
}

static void save_state(coleco_context *coleco, uint8_t slot)
{
	char *save_path = get_slot_name(&coleco->header, slot, "state");
	serialize_buffer state;
	init_serialize(&state);
	coleco_serialize(coleco, &state);
	save_to_file(&state, save_path);
	printf("Saved state to %s\n", save_path);
	free(save_path);
	free(state.data);
}

static uint8_t load_state_path(coleco_context *coleco, char *path)
{
	deserialize_buffer state;
	uint8_t ret;
	if ((ret = load_from_file(&state, path))) {
		coleco_deserialize(&state, coleco);
		free(state.data);
		printf("Loaded %s\n", path);
	}
	return ret;
}

static uint8_t load_state(system_header *system, uint8_t slot)
{
	coleco_context *coleco = (coleco_context *)system;
	char *statepath = get_slot_name(system, slot, "state");
	uint8_t ret;
#ifndef NEW_Z80
	if (!coleco->z80->native_pc) {
		ret = get_modification_time(statepath) != 0;
		if (ret) {
			system->delayed_load_slot = slot + 1;
		}
		goto done;

	}
#endif
	ret = load_state_path(coleco, statepath);
done:
	free(statepath);
	return ret;
}

static void run_coleco(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	uint32_t target_cycle = coleco->z80->Z80_CYCLE + 3420*16;
	render_set_video_standard(VID_NTSC);

	while (!coleco->should_return) {
		if (system->delayed_load_slot) {
			load_state(system, system->delayed_load_slot - 1);
			system->delayed_load_slot = 0;

		}
		if (coleco->vdp->frame != coleco->last_frame) {
#ifndef IS_LIB
			if (coleco->psg->scope) {
				scope_render(coleco->psg->scope);
			}
#endif
			uint32_t elapsed = coleco->vdp->frame - coleco->last_frame;
			coleco->last_frame = coleco->vdp->frame;
			if (system->enter_debugger_frames) {
				if (elapsed >= system->enter_debugger_frames) {
					system->enter_debugger_frames = 0;
					system->enter_debugger = 1;
				} else {
					system->enter_debugger_frames -= elapsed;
				}
			}

			if(exit_after){
				if (elapsed >= exit_after) {
					exit(0);
				} else {
					exit_after -= elapsed;
				}
			}
		}
		if (system->enter_debugger && coleco->z80->pc) {
			system->enter_debugger = 0;
#ifndef IS_LIB
			zdebugger(coleco->z80, coleco->z80->pc);
#endif
		}
		if (system->enter_debugger) {
			target_cycle = coleco->z80->Z80_CYCLE + 1;
		}
		update_interrupts(coleco);
		z80_run(coleco->z80, target_cycle);
		if (coleco->z80->reset) {
			z80_clear_reset(coleco->z80, coleco->z80->Z80_CYCLE + 3*15);
		}
		target_cycle = coleco->z80->Z80_CYCLE;
		vdp_run_context(coleco->vdp, target_cycle);
		psg_run(coleco->psg, target_cycle);
		if (system->save_state) {
			while (!coleco->z80->pc) {
				//advance Z80 to an instruction boundary
				z80_run(coleco->z80, coleco->z80->Z80_CYCLE + 1);
			}
			save_state(coleco, system->save_state - 1);
			system->save_state = 0;
		}

		target_cycle += 3420*16;
		if (target_cycle > 0x40000000) {
			uint32_t adjust = coleco->z80->Z80_CYCLE - 3420*262*2;
			z80_adjust_cycles(coleco->z80, adjust);
			vdp_adjust_cycles(coleco->vdp, adjust);
			coleco->psg->cycles -= adjust;
			target_cycle -= adjust;
		}
	}
	if (coleco->header.force_release || render_should_release_on_exit()) {
		bindings_release_capture();
		vdp_release_framebuffer(coleco->vdp);
		render_pause_source(coleco->psg->audio);
	}
	coleco->should_return = 0;
}

static void resume_coleco(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	if (coleco->header.force_release || render_should_release_on_exit()) {
		coleco->header.force_release = 0;
		bindings_reacquire_capture();
		vdp_reacquire_framebuffer(coleco->vdp);
		render_resume_source(coleco->psg->audio);
	}
	run_coleco(system);
}

static void start_coleco(system_header *system, char *statefile)
{
	coleco_context *coleco = (coleco_context *)system;

	z80_assert_reset(coleco->z80, 0);
	z80_clear_reset(coleco->z80, 128*15);

	if (statefile) {
		load_state_path(coleco, statefile);
	}

	if (system->enter_debugger) {
		system->enter_debugger = 0;
#ifndef IS_LIB
		zinsert_breakpoint(coleco->z80, coleco->z80->pc, (uint8_t *)zdebugger);
#endif
	}

	run_coleco(system);
}

static void free_coleco(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	vdp_free(coleco->vdp);
	z80_options_free(coleco->z80->Z80_OPTS);
	free(coleco->z80);
	psg_free(coleco->psg);
	free(coleco->rom);
	free(coleco->header.info.map);
	free(coleco->header.info.name);
	free(coleco);
}

static void soft_reset(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	z80_assert_reset(coleco->z80, coleco->z80->Z80_CYCLE);
#ifndef NEW_Z80
	coleco->z80->target_cycle = coleco->z80->sync_cycle = coleco->z80->Z80_CYCLE;
#endif
}


static void set_speed_percent(system_header * system, uint32_t percent)
{
	coleco_context *coleco = (coleco_context *)system;
	uint32_t old_clock = coleco->master_clock;
	coleco->master_clock = ((uint64_t)coleco->normal_clock * (uint64_t)percent) / 100;

	psg_adjust_master_clock(coleco->psg, coleco->master_clock);
}

static uint16_t get_open_bus_value(system_header *system)
{
	return 0xFFFF;
}

static void request_exit(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	coleco->should_return = 1;
#ifndef NEW_Z80
	coleco->z80->target_cycle = coleco->z80->sync_cycle = coleco->z80->Z80_CYCLE;
#endif
}

static uint8_t button_map[] = {
	[DPAD_UP] = 1,
	[DPAD_DOWN] = 4,
	[DPAD_LEFT] = 8,
	[DPAD_RIGHT] = 2,
	[BUTTON_A] = 0x40,
	[BUTTON_B] = 0x40,
	[BUTTON_C] = 9, //*
	[BUTTON_START] = 6, //#
	[BUTTON_X] = 0xD, //1
	[BUTTON_Y] = 0x7, //2
	[BUTTON_Z] = 0xC, //3
	[BUTTON_MODE] = 0x2 //4
};

static void gamepad_down(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	if (gamepad_num > 2) {
		return;
	}
	coleco_context *coleco = (coleco_context *)system;
	uint8_t index = gamepad_num - 1;
	if (button < BUTTON_B) {
		index += 2;
	}
	if (button > BUTTON_B) {
		coleco->controller_state[index] &= 0xF0;
		coleco->controller_state[index] |= button_map[button];
	} else {
		coleco->controller_state[index] &= ~button_map[button];
	}

}

static void gamepad_up(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	if (gamepad_num > 2) {
		return;
	}
	coleco_context *coleco = (coleco_context *)system;
	uint8_t index = gamepad_num - 1;
	if (button < BUTTON_B) {
		index += 2;
	}
	if (button > BUTTON_B) {
		coleco->controller_state[index] |= 0xF;
	} else {
		coleco->controller_state[index] |= button_map[button];
	}
}


static void config_updated(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	//sample rate may have changed
	psg_adjust_master_clock(coleco->psg, coleco->master_clock);
}

static void inc_debug_mode(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	vdp_inc_debug_mode(coleco->vdp);
}

static void toggle_debug_view(system_header *system, uint8_t debug_view)
{
#ifndef IS_LIB
	coleco_context *coleco = (coleco_context *)system;
	if (debug_view < DEBUG_OSCILLOSCOPE) {
		vdp_toggle_debug_view(coleco->vdp, debug_view);
	} else if (debug_view == DEBUG_OSCILLOSCOPE) {
		if (coleco->psg->scope) {
			oscilloscope *scope = coleco->psg->scope;
			coleco->psg->scope = NULL;
			scope_close(scope);
		} else {
			oscilloscope *scope = create_oscilloscope();
			psg_enable_scope(coleco->psg, scope, coleco->normal_clock);
		}
	}
#endif
}

static void load_save(system_header *system)
{
	//Unclear if any Coleco carts have non-volatile memory
	//but we need a dummy implementation so the save directory gets setup
}

static void persist_save(system_header *system)
{
}

static vdp_context *get_vdp(system_header *system)
{
	coleco_context *coleco = (coleco_context *)system;
	return coleco->vdp;
}

coleco_context *alloc_configure_coleco(system_media *media)
{
	coleco_context *coleco = calloc(1, sizeof(coleco_context));
	char *bios_path = tern_find_path_default(config, "system\0coleco_bios_path\0", (tern_val){.ptrval = "colecovision_bios.col"}, TVAL_PTR).ptrval;
	if (media_path_is_external(bios_path)) {
		media_file *f = media_fopen(bios_path, "rb");
		if (f) {
			media_fread(coleco->bios, 1, sizeof(coleco->bios), f);
			media_fclose(f);
		} else {
			warning("Failed to open Colecovision BIOS from %s\n", bios_path);
		}

	} else {
		uint32_t bios_size;
		char *tmp = read_bundled_file(bios_path, &bios_size);
		if (tmp) {
			if (bios_size > sizeof(coleco->bios)) {
				bios_size = sizeof(coleco->bios);
			}
			memcpy(coleco->bios, tmp, bios_size);
			free(tmp);
		} else {
			warning("Failed to open Colecovision BIOS from %s\n", bios_path);
		}

	}

	coleco->rom = media->buffer;
	coleco->rom_size = media->size;
	const memmap_chunk map[] = {
		{0x0000, 0x2000, sizeof(coleco->bios)-1, .flags = MMAP_READ, .buffer = coleco->bios},
		{0x8000, 0x10000, nearest_pow2(coleco->rom_size)-1, .flags = MMAP_READ, .buffer = coleco->rom},
		{0x7000, 0x8000, sizeof(coleco->ram)-1, .flags = MMAP_READ|MMAP_WRITE|MMAP_CODE, .buffer = coleco->ram}
	};
	static const memmap_chunk io_map[] = {
		{0x80, 0xA0, 0xFF, .write_8 = coleco_select_write},
		{0xA0, 0xC0, 0xFF, .read_8 = coleco_vdp_read, .write_8 = coleco_vdp_write},
		{0xC0, 0xE0, 0xFF, .write_8 = coleco_select_write},
		{0xE0, 0x100, 0xFF, .read_8 = coleco_controller_read, .write_8 = coleco_psg_write}
	};
	uint32_t rom_size = coleco->header.info.rom_size;
	z80_options *zopts = malloc(sizeof(z80_options));
	uint32_t num_chunks = sizeof(map)/sizeof(*map);
	memmap_chunk *heap_map = calloc(num_chunks, sizeof(memmap_chunk));
	memcpy(heap_map, map, sizeof(map));

	//Colecovision appears to use a 7.15909 MHz crystal with a /2 divider, but /15 works better with my VDP implementation
	init_z80_opts(zopts, heap_map, num_chunks, io_map, sizeof(io_map)/sizeof(*io_map), 15, 0xFF);
	coleco->z80 = init_z80_context(zopts);
	coleco->z80->system = coleco;
	coleco->normal_clock = coleco->master_clock = 53693175;

	coleco->psg = malloc(sizeof(psg_context));
	psg_init(coleco->psg, coleco->master_clock, 15*16);

	coleco->vdp = init_vdp_context(0, 0, VDP_TMS9918A);
	coleco->vdp->system = &coleco->header;

	memset(coleco->controller_state, 0xFF, sizeof(coleco->controller_state));

	coleco->header.info.save_type = SAVE_NONE;
	coleco->header.info.map = heap_map;
	//TODO: copy name from header if present
	coleco->header.info.name = strdup(media->name);


	coleco->header.has_keyboard = 0;
	coleco->header.set_speed_percent = set_speed_percent;
	coleco->header.start_context = start_coleco;
	coleco->header.resume_context = resume_coleco;
	coleco->header.load_save = load_save;
	coleco->header.persist_save = persist_save;
	coleco->header.load_state = load_state;
	coleco->header.free_context = free_coleco;
	coleco->header.get_open_bus_value = get_open_bus_value;
	coleco->header.request_exit = request_exit;
	coleco->header.soft_reset = soft_reset;
	coleco->header.inc_debug_mode = inc_debug_mode;
	coleco->header.gamepad_down = gamepad_down;
	coleco->header.gamepad_up = gamepad_up;
	//coleco->header.keyboard_down = keyboard_down;
	//coleco->header.keyboard_up = keyboard_up;
	coleco->header.config_updated = config_updated;
	coleco->header.serialize = serialize;
	coleco->header.deserialize = deserialize;
	coleco->header.toggle_debug_view = toggle_debug_view;
	coleco->header.get_vdp = get_vdp;
	coleco->header.type = SYSTEM_COLECOVISION;

	return coleco;
}
