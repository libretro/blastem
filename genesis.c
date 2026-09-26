/*
 Copyright 2013-2016 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "genesis.h"
#include "segacd.h"
#include "blastem.h"
#include "nor.h"
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include "render.h"
#include "gst.h"
#include "util.h"
#include "debug.h"
#include "gdb_remote.h"
#include "saves.h"
#include "bindings.h"
#include "jcart.h"
#include "radica.h"
#include "config.h"
#include "event_log.h"
#include "paths.h"
#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

uint32_t MCLKS_PER_68K;
#define MCLKS_PER_YM  7
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)
#define Z80_INT_PULSE_MCLKS 2573 //measured value is ~171.5 Z80 clocks
#define DEFAULT_SYNC_INTERVAL MCLKS_LINE
#define DEFAULT_LOWPASS_CUTOFF 3390

#define LINES_NTSC 262
#define LINES_PAL 313

#ifdef IS_LIB
#define MAX_SOUND_CYCLES (MCLKS_PER_YM*OPN2_NUM_OPERATORS*6*4)
#else
#define MAX_SOUND_CYCLES 100000
#endif

#ifdef NEW_Z80
#define Z80_CYCLE cycles
#define Z80_OPTS opts
#define z80_handle_code_write(...)
#else
#define Z80_CYCLE current_cycle
#define Z80_OPTS options
#endif
#ifdef NEW_CORE
#define int_num int_priority
#endif

void genesis_serialize(genesis_context *gen, serialize_buffer *buf, uint32_t m68k_pc, uint8_t all)
{
	if (all) {
		start_section(buf, SECTION_68000);
		m68k_serialize(gen->m68k, m68k_pc, buf);
		end_section(buf);

		if (gen->header.type == SYSTEM_GENESIS) {
			start_section(buf, SECTION_Z80);
			z80_serialize(gen->z80, buf);
			end_section(buf);
		}
	}

	start_section(buf, SECTION_VDP);
	vdp_serialize(gen->vdp, buf);
	end_section(buf);

	if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
		start_section(buf, SECTION_YM2612);
		ym_serialize(gen->ym, buf);
		end_section(buf);
	}

	start_section(buf, SECTION_PSG);
	psg_serialize(gen->psg, buf);
	end_section(buf);

	if (all) {
		start_section(buf, SECTION_GEN_BUS_ARBITER);
		save_int8(buf, gen->z80->reset);
		save_int8(buf, gen->z80->busreq);
		save_int16(buf, gen->z80_bank_reg);
		//I think the refresh logic may technically be part of the VDP, but whatever
		save_int32(buf, gen->last_sync_cycle);
		save_int32(buf, gen->refresh_counter);
		end_section(buf);

		if (gen->header.type == SYSTEM_GENESIS) {
			start_section(buf, SECTION_SEGA_IO_1);
			io_serialize(gen->io.ports, buf);
			end_section(buf);

			start_section(buf, SECTION_SEGA_IO_2);
			io_serialize(gen->io.ports + 1, buf);
			end_section(buf);

			start_section(buf, SECTION_SEGA_IO_EXT);
			io_serialize(gen->io.ports + 2, buf);
			end_section(buf);
		}

		start_section(buf, SECTION_MAIN_RAM);
		save_int8(buf, RAM_WORDS * 2 / 1024);
		save_buffer16(buf, gen->work_ram, RAM_WORDS);
		end_section(buf);

		if (gen->header.type == SYSTEM_GENESIS) {
			start_section(buf, SECTION_SOUND_RAM);
			save_int8(buf, Z80_RAM_BYTES / 1024);
			save_buffer8(buf, gen->zram, Z80_RAM_BYTES);
			end_section(buf);

			if (gen->version_reg & 0xF) {
				//only save TMSS info if it's present
				//that will allow a state saved on a model lacking TMSS
				//to be loaded on a model that has it
				start_section(buf, SECTION_TMSS);
				save_int8(buf, gen->tmss);
				save_buffer16(buf, gen->tmss_lock, 2);
				end_section(buf);
			}
		}

		cart_serialize(&gen->header, buf);
	}
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_serialize(cd, buf, all);
	}
}

static uint8_t *serialize(system_header *sys, size_t *size_out)
{
	genesis_context *gen = (genesis_context *)sys;
	uint32_t address;
#ifndef NEW_CORE
	if (gen->m68k->resume_pc) {
		
		gen->header.save_state = SERIALIZE_SLOT+1;
		while (!gen->serialize_tmp)
		{
			gen->m68k->target_cycle = gen->m68k->cycles + 1;
			resume_68k(gen->m68k);
		}
		if (size_out) {
			*size_out = gen->serialize_size;
		}
		uint8_t *ret = gen->serialize_tmp;
		gen->serialize_tmp = NULL;
		return ret;
	} else 
#endif
	{
		serialize_buffer state;
		init_serialize(&state);
		uint32_t address = read_word(4, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen, gen->m68k) << 16;
		address |= read_word(6, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen, gen->m68k);
		genesis_serialize(gen, &state, address, 1);
		if (size_out) {
			*size_out = state.size;
		}
		return state.data;
	}
}

static void ram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024 / 2;
	if (ram_size > RAM_WORDS) {
		fatal_error("State has a RAM size of %d bytes", ram_size * 2);
	}
	load_buffer16(buf, gen->work_ram, ram_size);
	m68k_invalidate_code_range(gen->m68k, 0xE00000, 0x1000000);
}

static void zram_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	uint32_t ram_size = load_int8(buf) * 1024;
	if (ram_size > Z80_RAM_BYTES) {
		fatal_error("State has a Z80 RAM size of %d bytes", ram_size);
	}
	load_buffer8(buf, gen->zram, ram_size);
	z80_invalidate_code_range(gen->z80, 0, 0x4000);
}

void gen_update_z80_bank_pointer(genesis_context *gen)
{
	if (gen->z80_bank_reg < 0x140) {
		gen->z80->mem_pointers[1] = get_native_pointer(gen->z80_bank_reg << 15, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen);
	} else {
		gen->z80->mem_pointers[1] = NULL;
	}
	z80_invalidate_code_range(gen->z80, 0x8000, 0xFFFF);
}

static void bus_arbiter_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	gen->z80->reset = load_int8(buf);
	gen->z80->busreq = load_int8(buf);
	gen->z80_bank_reg = load_int16(buf) & 0x1FF;
	if ((buf->size - buf->cur_pos) >= 2 * sizeof(uint32_t)) {
		gen->last_sync_cycle = load_int32(buf);
		gen->refresh_counter = load_int32(buf);
	} else {
		//save state is from an older version of BlastEm that lacks these fields
		//set them to reasonable values
		gen->last_sync_cycle = gen->m68k->cycles;
		gen->refresh_counter = 0;
	}
}

static void tmss_deserialize(deserialize_buffer *buf, void *vgen)
{
	genesis_context *gen = vgen;
	gen->tmss = load_int8(buf);
	load_buffer16(buf, gen->tmss_lock, 2);
}

static void adjust_int_cycle(m68k_context * context, vdp_context * v_context);
static void check_tmss_lock(genesis_context *gen);
static void toggle_tmss_rom(genesis_context *gen);
#ifndef NEW_CORE
#include "m68k_internal.h" //needed for get_native_address_trans, should be eliminated once handling of PC is cleaned up
#endif
void genesis_deserialize(deserialize_buffer *buf, genesis_context *gen)
{
	register_section_handler(buf, (section_handler){.fun = m68k_deserialize, .data = gen->m68k}, SECTION_68000);
	register_section_handler(buf, (section_handler){.fun = vdp_deserialize, .data = gen->vdp}, SECTION_VDP);
	register_section_handler(buf, (section_handler){.fun = psg_deserialize, .data = gen->psg}, SECTION_PSG);
	register_section_handler(buf, (section_handler){.fun = bus_arbiter_deserialize, .data = gen}, SECTION_GEN_BUS_ARBITER);
	register_section_handler(buf, (section_handler){.fun = ram_deserialize, .data = gen}, SECTION_MAIN_RAM);
	register_section_handler(buf, (section_handler){.fun = cart_deserialize, .data = gen}, SECTION_MAPPER);
	if (gen->header.type == SYSTEM_GENESIS) {
		register_section_handler(buf, (section_handler){.fun = z80_deserialize, .data = gen->z80}, SECTION_Z80);
		register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports}, SECTION_SEGA_IO_1);
		register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 1}, SECTION_SEGA_IO_2);
		register_section_handler(buf, (section_handler){.fun = io_deserialize, .data = gen->io.ports + 2}, SECTION_SEGA_IO_EXT);
		register_section_handler(buf, (section_handler){.fun = zram_deserialize, .data = gen}, SECTION_SOUND_RAM);
		register_section_handler(buf, (section_handler){.fun = tmss_deserialize, .data = gen}, SECTION_TMSS);
	}
	if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
		register_section_handler(buf, (section_handler){.fun = ym_deserialize, .data = gen->ym}, SECTION_YM2612);
	}
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		segacd_register_section_handlers(cd, buf);
	}
	uint8_t tmss_old = gen->tmss;
	uint8_t old_z80_clock_bit = gen->vdp->test_regs[1] & 1;
	gen->tmss = 0xFF;
	while (buf->cur_pos < buf->size)
	{
		load_section(buf);
	}
	if (gen->header.type == SYSTEM_GENESIS && (gen->version_reg & 0xF)) {
		if (gen->tmss == 0xFF) {
			//state lacked a TMSS section, assume that the game ROM is mapped in
			//and that the VDP is unlocked
			gen->tmss_lock[0] = 0x5345;
			gen->tmss_lock[1] = 0x4741;
			gen->tmss = 1;
		}
		if (gen->tmss != tmss_old) {
			toggle_tmss_rom(gen);
		}
		check_tmss_lock(gen);
	}
	gen_update_z80_bank_pointer(gen);
	adjust_int_cycle(gen->m68k, gen->vdp);
#ifndef NEW_CORE
	//HACK: Fix this once PC/IR is represented in a better way in 68K core
	//Would be better for this hack to live in side the 68K core itself, but it's better to do it
	//after RAM has been loaded to avoid any unnecessary retranslation
	gen->m68k->resume_pc = get_native_address_trans(gen->m68k, gen->m68k->last_prefetch_address);
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		cd->m68k->resume_pc = get_native_address_trans(cd->m68k, cd->m68k->last_prefetch_address);
	}
#endif
	free(buf->handlers);
	buf->handlers = NULL;
	uint8_t new_z80_clock_bit = gen->vdp->test_regs[1] & 1;
	if (old_z80_clock_bit != new_z80_clock_bit) {
		gen->z80->Z80_OPTS->gen.clock_divider = new_z80_clock_bit ? MCLKS_PER_YM : MCLKS_PER_Z80;
		gen->psg->clock_inc = new_z80_clock_bit ? MCLKS_PER_YM * 16 : MCLKS_PER_PSG;
		psg_adjust_master_clock(gen->psg, gen->master_clock);
		z80_invalidate_code_range(gen->z80, 0, 0x4000);
		z80_clock_divider_updated(gen->z80->Z80_OPTS);
	}
}

static void deserialize(system_header *sys, uint8_t *data, size_t size)
{
	genesis_context *gen = (genesis_context *)sys;
	deserialize_buffer buffer;
	init_deserialize(&buffer, data, size);
	genesis_deserialize(&buffer, gen);
}

uint16_t read_dma_value(uint32_t address)
{
	genesis_context *genesis = (genesis_context *)current_system;
	address *= 2;
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	if ((address >= 0xA00000 && address < 0xB00000) || (address >= 0xC00000 && address <= 0xE00000)) {
		return 0;
	}
	if (genesis->expansion) {
		segacd_context *cd = genesis->expansion;
		uint32_t word_ram = cd->base + 0x200000;
		uint32_t word_ram_end = cd->base + 0x240000;
		if (address >= word_ram && address < word_ram_end) {
			if (!cd->has_vdp_dma_value) {
#ifdef NEW_CORE
				cd->vdp_dma_value = genesis->m68k->prefetch;
#else
				cd->vdp_dma_value = read_word(genesis->m68k->last_prefetch_address, (void **)genesis->m68k->mem_pointers, &genesis->m68k->opts->gen, genesis->m68k);
#endif
				cd->has_vdp_dma_value = 1;
			}
			uint16_t ret = cd->vdp_dma_value;
			cd->vdp_dma_value = read_word(address, (void **)genesis->m68k->mem_pointers, &genesis->m68k->opts->gen, genesis->m68k);
			return ret;
		}
	}

	return read_word(address, (void **)genesis->m68k->mem_pointers, &genesis->m68k->opts->gen, genesis->m68k);
}

void vdp_dma_started(void)
{
	genesis_context *genesis = (genesis_context *)current_system;
	if (genesis->expansion) {
		segacd_context *cd = genesis->expansion;
		cd->has_vdp_dma_value = 0;
	}
}


static uint16_t get_open_bus_value(system_header *system)
{
	genesis_context *genesis = (genesis_context *)system;
#ifdef NEW_CORE
	return genesis->m68k->prefetch;
#else
	return read_dma_value(genesis->m68k->last_prefetch_address/2);
#endif
}

static void adjust_int_cycle(m68k_context * context, vdp_context * v_context)
{
	//static int old_int_cycle = CYCLE_NEVER;
	genesis_context *gen = context->system;
	if (context->sync_cycle - context->cycles > gen->max_cycles) {
		context->sync_cycle = context->cycles + gen->max_cycles;
	}
	context->int_cycle = CYCLE_NEVER;
	uint8_t mask = context->status & 0x7;
	if (mask < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if (mask < 4) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				next_hint = next_hint < context->cycles ? context->cycles : next_hint;
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 4;

				}
			}
			if (mask < 2 && (v_context->regs[REG_MODE_3] & BIT_EINT_EN) && gen->header.type == SYSTEM_GENESIS) {
				uint32_t next_eint_port0 = io_next_interrupt(gen->io.ports, context->cycles);
				uint32_t next_eint_port1 = io_next_interrupt(gen->io.ports + 1, context->cycles);
				uint32_t next_eint_port2 = io_next_interrupt(gen->io.ports + 2, context->cycles);
				uint32_t next_eint = next_eint_port0 < next_eint_port1
					? (next_eint_port0 < next_eint_port2 ? next_eint_port0 : next_eint_port2)
					: (next_eint_port1 < next_eint_port2 ? next_eint_port1 : next_eint_port2);
				if (next_eint != CYCLE_NEVER) {
					next_eint = next_eint < context->cycles ? context->cycles : next_eint;
					if (next_eint < context->int_cycle) {
						context->int_cycle = next_eint;
						context->int_num = 2;
					}
				}
			}
		}
	}
	if (context->int_cycle > context->cycles && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	/*if (context->int_cycle != old_int_cycle) {
		printf("int cycle changed to: %d, level: %d @ %d(%d), frame: %d, vcounter: %d, hslot: %d, mask: %d, hint_counter: %d\n", context->int_cycle, context->int_num, v_context->cycles, context->cycles, v_context->frame, v_context->vcounter, v_context->hslot, context->status & 0x7, v_context->hint_counter);
		old_int_cycle = context->int_cycle;
	}*/

	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->cycles + 1;
		return;
	}

	if (context->should_return || gen->header.enter_debugger || context->wp_hit) {
		context->sync_cycle = context->cycles;
	}

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	if (context->target_cycle < context->cycles) {
		//Changes to SR can result in an interrupt cycle that's in the past
		//This can cause issues with the implementation of STOP though
		context->target_cycle = context->cycles;
	}
	if (context->target_cycle == context->int_cycle) {
		//Currently delays from Z80 access and refresh are applied only when we sync
		//this can cause extra latency when it comes to interrupts
		//to prevent this code forces some extra synchronization in the period immediately before an interrupt
		if ((context->target_cycle - context->cycles) > gen->int_latency_prev1) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev1;
		} else if ((context->target_cycle - context->cycles) > gen->int_latency_prev2) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev2;
		} else {
			context->target_cycle = context->sync_cycle = context->cycles;
		}

	}
	/*printf("Cyc: %d, Trgt: %d, Int Cyc: %d, Int: %d, Mask: %X, V: %d, H: %d, HICount: %d, HReg: %d, Line: %d\n",
		context->cycles, context->target_cycle, context->int_cycle, context->int_num, (context->status & 0x7),
		v_context->regs[REG_MODE_2] & 0x20, v_context->regs[REG_MODE_1] & 0x10, v_context->hint_counter, v_context->regs[REG_HINT], v_context->cycles / MCLKS_LINE);*/
}

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

static void z80_next_int_pulse(z80_context * z_context)
{
	genesis_context * gen = z_context->system;
#ifdef NEW_Z80
	z_context->int_cycle = vdp_next_vint_z80(gen->vdp);
	z_context->int_end_cycle = z_context->int_cycle + Z80_INT_PULSE_MCLKS;
	z_context->int_value = 0xFF;
	z80_sync_cycle(z_context, z_context->sync_cycle);
#else
	z_context->int_pulse_start = vdp_next_vint_z80(gen->vdp);
	z_context->int_pulse_end = z_context->int_pulse_start + Z80_INT_PULSE_MCLKS;
	z_context->im2_vector = 0xFF;
#endif
}

static void sync_z80(genesis_context *gen, uint32_t mclks)
{
	z80_context *z_context = gen->z80;
#ifndef NO_Z80
	if (z80_enabled) {
#ifdef NEW_Z80
		if (z_context->int_cycle == 0xFFFFFFFFU) {
			z80_next_int_pulse(z_context);
		}
#endif
		if (gen->enter_z80_debugger && !z_context->reset && !z_context->busack) {
			while (!z_context->pc) {
				z80_run(z_context, z_context->Z80_CYCLE + 4);
			}
			gen->enter_z80_debugger = 0;
#ifndef IS_LIB
			zdebugger(z_context, z_context->pc);
#endif
		}
		z80_run(z_context, mclks);
	} else
#endif
	{
		z_context->Z80_CYCLE = mclks;
	}
}

static void sync_sound(genesis_context * gen, uint32_t target)
{
	//printf("YM | Cycle: %d, bpos: %d, PSG | Cycle: %d, bpos: %d\n", gen->ym->current_cycle, gen->ym->buffer_pos, gen->psg->cycles, gen->psg->buffer_pos * 2);
	while (target > gen->psg->cycles && target - gen->psg->cycles > MAX_SOUND_CYCLES) {
		uint32_t cur_target = gen->psg->cycles + MAX_SOUND_CYCLES;
		//printf("Running PSG to cycle %d\n", cur_target);
		psg_run(gen->psg, cur_target);
		//printf("Running YM-2612 to cycle %d\n", cur_target);
		ym_run(gen->ym, cur_target);
		if (gen->expansion) {
			scd_run(gen->expansion, gen_cycle_to_scd(cur_target, gen));
		}
	}
	psg_run(gen->psg, target);
	ym_run(gen->ym, target);
	if (gen->expansion) {
		scd_run(gen->expansion, gen_cycle_to_scd(target, gen));
	}

	//printf("Target: %d, YM bufferpos: %d, PSG bufferpos: %d\n", target, gen->ym->buffer_pos, gen->psg->buffer_pos * 2);
}

#define REFRESH_INTERVAL 128
#define REFRESH_DELAY 2

//Advances the refresh counter by elapsed master cycles and returns the refresh
//delay incurred along the way. The delay cycles are themselves time in which the
//counter keeps running, so they have to be fed back into it. Doing that in a loop
//rather than in a single division is what makes the result independent of how
//large the chunks are that the caller happens to hand us; the interpreter and the
//JIT sync at different granularity, so anything chunk-dependent makes the two
//cores drift apart.
static uint32_t refresh_advance(genesis_context *gen, uint32_t elapsed)
{
	uint32_t interval = MCLKS_PER_68K * REFRESH_INTERVAL;
	uint32_t delay = 0;
	gen->refresh_counter += elapsed;
	while (gen->refresh_counter >= interval) {
		gen->refresh_counter -= interval;
		gen->refresh_counter += REFRESH_DELAY * MCLKS_PER_68K;
		delay += REFRESH_DELAY * MCLKS_PER_68K;
	}
	return delay;
}

void gen_update_refresh(m68k_context *context)
{
	genesis_context *gen = context->system;
	context->cycles += refresh_advance(gen, context->cycles - gen->last_sync_cycle);
	gen->last_sync_cycle = context->cycles;
}

void gen_update_refresh_free_access(m68k_context *context)
{
	genesis_context *gen = context->system;
	uint32_t before = context->cycles - 4*MCLKS_PER_68K;
	if (before < gen->last_sync_cycle) {
		return;
	}
	//Add refresh delays for any accesses that happened before the current one
	context->cycles += refresh_advance(gen, before - gen->last_sync_cycle);
	gen->last_sync_cycle = context->cycles;
	//advance refresh counter for the current access, but don't generate delays
	gen->refresh_counter += 4*MCLKS_PER_68K;
	gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
}

void gen_update_refresh_no_wait(m68k_context *context)
{
	uint32_t interval = MCLKS_PER_68K * REFRESH_INTERVAL;
	genesis_context *gen = context->system;
	gen->refresh_counter += context->cycles - gen->last_sync_cycle;
	gen->last_sync_cycle = context->cycles;
	gen->refresh_counter = gen->refresh_counter % interval;
}

#define ADJUST_BUFFER (8*MCLKS_LINE*313)
#define MAX_NO_ADJUST (UINT32_MAX-ADJUST_BUFFER)

static m68k_context *sync_components(m68k_context * context, uint32_t address)
{
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	z80_context * z_context = gen->z80;
	if (gen->bus_busy) {
		gen_update_refresh_no_wait(context);
	} else {
		gen_update_refresh(context);
	}

	uint32_t mclks = context->cycles;
	sync_z80(gen, mclks);
	sync_sound(gen, mclks);
	vdp_run_context(v_context, mclks);
	io_run(gen->io.ports, mclks);
	io_run(gen->io.ports + 1, mclks);
	io_run(gen->io.ports + 2, mclks);
	if (gen->expansion) {
		scd_run(gen->expansion, gen_cycle_to_scd(mclks, gen));
	}
	if (gen->mars) {
		s32x_run(gen->mars, mclks);
	}
	if (mclks >= gen->reset_cycle) {
		gen->reset_requested = 1;
		context->should_return = 1;
		gen->reset_cycle = CYCLE_NEVER;
	}
	if (v_context->frame != gen->last_frame) {
#ifndef IS_LIB
		if (gen->ym->scope) {
			scope_render(gen->ym->scope);
		}
#endif
		//printf("reached frame end %d | MCLK Cycles: %d, Target: %d, VDP cycles: %d, vcounter: %d, hslot: %d\n", gen->last_frame, mclks, gen->frame_end, v_context->cycles, v_context->vcounter, v_context->hslot);
		uint32_t elapsed = v_context->frame - gen->last_frame;
		gen->last_frame = v_context->frame;
		event_flush(mclks);
		gen->last_flush_cycle = mclks;
		if (gen->header.enter_debugger_frames) {
			if (elapsed >= gen->header.enter_debugger_frames) {
				gen->header.enter_debugger_frames = 0;
				gen->header.enter_debugger = 1;
			} else {
				gen->header.enter_debugger_frames -= elapsed;
			}
		}

		if(exit_after){
			if (elapsed >= exit_after) {
				exit(0);
			} else {
				exit_after -= elapsed;
			}
		}
		if (context->cycles > MAX_NO_ADJUST || (gen->mars && gen->mars->main->cycles > MAX_NO_ADJUST)) {
			uint32_t deduction = mclks - ADJUST_BUFFER;
			vdp_adjust_cycles(v_context, deduction);
			io_adjust_cycles(gen->io.ports, context->cycles, deduction);
			io_adjust_cycles(gen->io.ports+1, context->cycles, deduction);
			io_adjust_cycles(gen->io.ports+2, context->cycles, deduction);
			if (gen->mapper_type == MAPPER_JCART) {
				jcart_adjust_cycles(gen, deduction);
			}
			context->cycles -= deduction;
			z80_adjust_cycles(z_context, deduction);
			ym_adjust_cycles(gen->ym, deduction);
			if (gen->ym->vgm) {
				vgm_adjust_cycles(gen->ym->vgm, deduction);
			}
			gen->psg->cycles -= deduction;
			if (gen->reset_cycle != CYCLE_NEVER) {
				gen->reset_cycle -= deduction;
			}
			event_cycle_adjust(mclks, deduction);
			if (gen->expansion) {
				scd_adjust_cycle(gen->expansion, deduction);
			}
			if (gen->mars) {
				s32x_adjust_cycles(gen->mars, deduction);
			}
			gen->last_flush_cycle -= deduction;
			gen->last_sync_cycle -= deduction;
		}
		if (gen->header.frame_advance) {
			gen->header.frame_advance = 0;
			gen->header.paused = 1;
			system_request_exit(&gen->header, 1);
		}
	} else if (mclks - gen->last_flush_cycle > gen->soft_flush_cycles) {
		event_soft_flush(mclks);
		gen->last_flush_cycle = mclks;
	}
	gen->frame_end = vdp_cycles_to_frame_end(v_context);
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->cycles, v_context->vcounter, v_context->hslot);
	if (!address && (gen->header.enter_debugger || gen->header.save_state)) {
		context->sync_cycle = context->cycles + 1;
	}
	adjust_int_cycle(context, v_context);
	if (gen->reset_cycle < context->target_cycle) {
		context->target_cycle = gen->reset_cycle;
	}
	if (address) {
		if (gen->header.enter_debugger || context->wp_hit) {
			if (!context->wp_hit) {
				gen->header.enter_debugger = 0;
			}
#ifndef IS_LIB
			if (gen->header.debugger_type == DEBUGGER_NATIVE) {
				debugger(context, address);
			} else {
				gdb_debug_enter(context, address);
			}
#endif
		}
#ifdef NEW_Z80
		if (gen->header.save_state) {
#else
		if (gen->header.save_state && (z_context->pc || !z_context->native_pc || z_context->reset || !z_context->busreq)) {
#endif
			uint8_t slot = gen->header.save_state - 1;
			gen->header.save_state = 0;
#ifndef NEW_Z80
			if (z_context->native_pc && !z_context->reset) {
				//advance Z80 core to the start of an instruction
				while (!z_context->pc)
				{
					sync_z80(gen, z_context->current_cycle + MCLKS_PER_Z80);
				}
			}
#endif
			char *save_path = slot >= SERIALIZE_SLOT ? NULL : get_slot_name(&gen->header, slot, use_native_states ? "state" : "gst");
			if (use_native_states || slot >= SERIALIZE_SLOT) {
				serialize_buffer state;
				init_serialize(&state);
				genesis_serialize(gen, &state, address, slot != EVENTLOG_SLOT);
				if (slot == SERIALIZE_SLOT) {
					gen->serialize_tmp = state.data;
					gen->serialize_size = state.size;
					context->sync_cycle = context->cycles;
					context->should_return = 1;
				} else if (slot == EVENTLOG_SLOT) {
					event_state(context->cycles, &state);
				} else {
					save_to_file(&state, save_path);
					free(state.data);
				}
			} else {
				save_gst(gen, save_path, address);
			}
			if (slot != SERIALIZE_SLOT) {
				debug_message("Saved state to %s\n", save_path);
			}
			free(save_path);
		} else if(gen->header.save_state) {
			context->sync_cycle = context->cycles + 1;
		}
	}
#ifdef NEW_CORE
	if (context->target_cycle == context->cycles) {
		context->target_cycle++;
	}
#endif
	return context;
}

static void sync_sound_pico(genesis_context * gen, uint32_t target)
{
	while (target > gen->psg->cycles && target - gen->psg->cycles > MAX_SOUND_CYCLES)
	{
		uint32_t cur_target = gen->psg->cycles + MAX_SOUND_CYCLES;
		psg_run(gen->psg, cur_target);
		pico_pcm_run(gen->adpcm, cur_target);
		if (gen->ymz) {
			//FIXME: These have a separate crystal
			ymz263b_run(gen->ymz, cur_target);
			ymf262_run(gen->opl, cur_target);
		}
	}
	psg_run(gen->psg, target);
	pico_pcm_run(gen->adpcm, target);
	if (gen->ymz) {
		ymz263b_run(gen->ymz, target);
		ymf262_run(gen->opl, target);
	}
}

static void adjust_int_cycle_pico(m68k_context *context, vdp_context *v_context)
{
	genesis_context *gen = context->system;
	if (context->sync_cycle - context->cycles > gen->max_cycles) {
		context->sync_cycle = context->cycles + gen->max_cycles;
	}
	context->int_cycle = CYCLE_NEVER;
	uint8_t mask = context->status & 0x7;
	if (mask < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if (mask < 5) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				next_hint = next_hint < context->cycles ? context->cycles : next_hint;
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 5;

				}
			}
			if (mask < 4) {
				if (v_context->regs[REG_MODE_3] & BIT_EINT_EN) {
					uint32_t next_eint_port0 = io_next_interrupt(gen->io.ports, context->cycles);
					uint32_t next_eint_port1 = io_next_interrupt(gen->io.ports + 1, context->cycles);
					uint32_t next_eint_port2 = io_next_interrupt(gen->io.ports + 2, context->cycles);
					uint32_t next_eint = next_eint_port0 < next_eint_port1
						? (next_eint_port0 < next_eint_port2 ? next_eint_port0 : next_eint_port2)
						: (next_eint_port1 < next_eint_port2 ? next_eint_port1 : next_eint_port2);
					if (next_eint != CYCLE_NEVER) {
						next_eint = next_eint < context->cycles ? context->cycles : next_eint;
						if (next_eint < context->int_cycle) {
							context->int_cycle = next_eint;
							context->int_num = 2;
						}
					}
				}
				if (mask < 3) {
					uint32_t next_pcm_int = pico_pcm_next_int(gen->adpcm);
					if (next_pcm_int != CYCLE_NEVER && next_pcm_int < context->int_cycle) {
						context->int_cycle = next_pcm_int;
						context->int_num = 3;
					}
					
					if (mask < 2 && gen->ymz) {
						uint32_t ymz_int = ymz263b_next_int(gen->ymz);
						if (ymz_int < context->int_cycle) {
							context->int_cycle = ymz_int;
							context->int_num = 2;
						}
					}
				}
			}
		}
	}
	if (context->int_cycle > context->cycles && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	/*if (context->int_cycle != old_int_cycle) {
		printf("int cycle changed to: %d, level: %d @ %d(%d), frame: %d, vcounter: %d, hslot: %d, mask: %d, hint_counter: %d\n", context->int_cycle, context->int_num, v_context->cycles, context->cycles, v_context->frame, v_context->vcounter, v_context->hslot, context->status & 0x7, v_context->hint_counter);
		old_int_cycle = context->int_cycle;
	}*/

	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->cycles + 1;
		return;
	}

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	if (context->should_return || gen->header.enter_debugger || context->wp_hit) {
		context->target_cycle = context->cycles;
	} else if (context->target_cycle < context->cycles) {
		//Changes to SR can result in an interrupt cycle that's in the past
		//This can cause issues with the implementation of STOP though
		context->target_cycle = context->cycles;
	}
	if (context->target_cycle == context->int_cycle) {
		//Currently delays from Z80 access and refresh are applied only when we sync
		//this can cause extra latency when it comes to interrupts
		//to prevent this code forces some extra synchronization in the period immediately before an interrupt
		if ((context->target_cycle - context->cycles) > gen->int_latency_prev1) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev1;
		} else if ((context->target_cycle - context->cycles) > gen->int_latency_prev2) {
			context->target_cycle = context->sync_cycle = context->int_cycle - gen->int_latency_prev2;
		} else {
			context->target_cycle = context->sync_cycle = context->cycles;
		}

	}
#ifdef NEW_CORE
	if (context->target_cycle == context->cycles) {
		context->target_cycle++;
	}
#endif
}

static m68k_context* sync_components_pico(m68k_context * context, uint32_t address)
{
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	if (gen->bus_busy) {
		gen_update_refresh_no_wait(context);
	} else {
		gen_update_refresh(context);
	}

	uint32_t mclks = context->cycles;
	sync_sound_pico(gen, mclks);
	vdp_run_context(v_context, mclks);
	if (mclks >= gen->reset_cycle) {
		gen->reset_requested = 1;
		context->should_return = 1;
		gen->reset_cycle = CYCLE_NEVER;
	}
	if (v_context->frame != gen->last_frame) {
#ifndef IS_LIB
		if (gen->psg->scope) {
			scope_render(gen->psg->scope);
		}
#endif
		//printf("reached frame end %d | MCLK Cycles: %d, Target: %d, VDP cycles: %d, vcounter: %d, hslot: %d\n", gen->last_frame, mclks, gen->frame_end, v_context->cycles, v_context->vcounter, v_context->hslot);
		uint32_t elapsed = v_context->frame - gen->last_frame;
		gen->last_frame = v_context->frame;
		event_flush(mclks);
		gen->last_flush_cycle = mclks;
		if (gen->header.enter_debugger_frames) {
			if (elapsed >= gen->header.enter_debugger_frames) {
				gen->header.enter_debugger_frames = 0;
				gen->header.enter_debugger = 1;
			} else {
				gen->header.enter_debugger_frames -= elapsed;
			}
		}

		if(exit_after){
			if (elapsed >= exit_after) {
				exit(0);
			} else {
				exit_after -= elapsed;
			}
		}
		if (context->cycles > MAX_NO_ADJUST) {
			uint32_t deduction = mclks - ADJUST_BUFFER;
			vdp_adjust_cycles(v_context, deduction);
			if (gen->mapper_type == MAPPER_JCART) {
				jcart_adjust_cycles(gen, deduction);
			}
			context->cycles -= deduction;
			if (gen->psg->vgm) {
				vgm_adjust_cycles(gen->psg->vgm, deduction);
			}
			gen->psg->cycles -= deduction;
			gen->adpcm->cycle -= deduction;
			if (gen->ymz) {
				gen->ymz->cycle -= deduction;
				ymf262_adjust_cycles(gen->opl, deduction);
			}
			if (gen->reset_cycle != CYCLE_NEVER) {
				gen->reset_cycle -= deduction;
			}
			event_cycle_adjust(mclks, deduction);
			if (gen->expansion) {
				scd_adjust_cycle(gen->expansion, deduction);
			}
			if (gen->mars) {
				s32x_adjust_cycles(gen->mars, deduction);
			}
			gen->last_flush_cycle -= deduction;
		}
	} else if (mclks - gen->last_flush_cycle > gen->soft_flush_cycles) {
		event_soft_flush(mclks);
		gen->last_flush_cycle = mclks;
	}
	gen->frame_end = vdp_cycles_to_frame_end(v_context);
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->cycles, v_context->vcounter, v_context->hslot);
	if (!address && (gen->header.enter_debugger || gen->header.save_state)) {
		context->sync_cycle = context->cycles + 1;
	}
	adjust_int_cycle_pico(context, v_context);
	if (gen->reset_cycle < context->target_cycle) {
		context->target_cycle = gen->reset_cycle;
	}
	if (address) {
		if (gen->header.enter_debugger || context->wp_hit) {
			if (!context->wp_hit) {
				gen->header.enter_debugger = 0;
			}
#ifndef IS_LIB
			if (gen->header.debugger_type == DEBUGGER_NATIVE) {
				debugger(context, address);
			} else {
				gdb_debug_enter(context, address);
			}
#endif
		}
		if (gen->header.save_state) {
			uint8_t slot = gen->header.save_state - 1;
			gen->header.save_state = 0;
			char *save_path = slot >= SERIALIZE_SLOT ? NULL : get_slot_name(&gen->header, slot, use_native_states ? "state" : "gst");
			if (use_native_states || slot >= SERIALIZE_SLOT) {
				serialize_buffer state;
				init_serialize(&state);
				genesis_serialize(gen, &state, address, slot != EVENTLOG_SLOT);
				if (slot == SERIALIZE_SLOT) {
					gen->serialize_tmp = state.data;
					gen->serialize_size = state.size;
					context->sync_cycle = context->cycles;
					context->should_return = 1;
				} else if (slot == EVENTLOG_SLOT) {
					event_state(context->cycles, &state);
				} else {
					save_to_file(&state, save_path);
					free(state.data);
				}
			} else {
				save_gst(gen, save_path, address);
			}
			if (slot != SERIALIZE_SLOT) {
				debug_message("Saved state to %s\n", save_path);
			}
			free(save_path);
		} else if(gen->header.save_state) {
			context->sync_cycle = context->cycles + 1;
		}
	}
	return context;
}

static m68k_context *int_ack(m68k_context *context)
{
	genesis_context * gen = context->system;
	if ((gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) || context->int_num > 3) {
		vdp_context * v_context = gen->vdp;
		//printf("acknowledging %d @ %d:%d, vcounter: %d, hslot: %d\n", context->int_ack, context->cycles, v_context->cycles, v_context->vcounter, v_context->hslot);
		vdp_run_context(v_context, context->cycles);
		vdp_int_ack(v_context);
	}

	//the Genesis responds to these exclusively with !VPA which means its a slow
	//6800 operation. documentation says these can take between 10 and 19 cycles.
	//actual results measurements seem to suggest it's actually between 9 and 18
	//Base 68K core has added 4 cycles for a normal int ack cycle already
	//We add 5 + the current cycle count (in 68K cycles) mod 10 to simulate the
	//additional variable delay from the use of the 6800 cycle
	uint32_t cycle_count = context->cycles / context->opts->gen.clock_divider;
	context->cycles += 5 + (cycle_count % 10);

	return context;
}

static void enter_68k_debugger_immediately(void *context)
{
	genesis_context *gen = context;
	gen->header.enter_debugger = 1;
	if (gen->m68k->sync_cycle > gen->m68k->cycles + 1) {
		gen->m68k->sync_cycle = gen->m68k->cycles + 1;
	}
	if (gen->m68k->target_cycle > gen->m68k->sync_cycle) {
		gen->m68k->target_cycle = gen->m68k->sync_cycle;
	}
}

static void enter_z80_debugger_immediately(void *context)
{
	genesis_context *gen = context;
	gen->enter_z80_debugger;
	if (gen->z80->sync_cycle > gen->z80->Z80_CYCLE + 1) {
		gen->z80->sync_cycle = gen->z80->Z80_CYCLE + 1;
	}
#ifndef NEW_Z80
	if (gen->z80->target_cycle > gen->z80->sync_cycle) {
		gen->z80->target_cycle = gen->z80->sync_cycle;
	}
#endif
}


static m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if (vdp_port & 0x2700E0) {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Write to unmapped part of VDP memory region, address: %X\n", 0xC00000 | vdp_port);
	}
	if (!gen->vdp_unlocked) {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "VDP write to %X without TMSS unlock\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->cycles);

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	gen_update_refresh_free_access(context);

	if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
		sync_components_pico(context, 0);
	} else {
		sync_components(context, 0);
	}
	vdp_context *v_context = gen->vdp;
	uint32_t before_cycle = v_context->cycles;
	uint8_t did_dma = 0;
	if (vdp_port < 0x10) {
		int blocked;
		if (vdp_port < 4) {
			vdp_data_port_write(v_context, value);
		} else if(vdp_port < 8) {
			vdp_run_context_full(v_context, context->cycles);
			before_cycle = v_context->cycles;
			blocked = vdp_control_port_write(v_context, value, context->cycles);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						did_dma = 1;
						vdp_run_dma_done(v_context, gen->frame_end);
						if (v_context->cycles >= gen->frame_end) {
							uint32_t cycle_diff = v_context->cycles - context->cycles;
							uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
							if (m68k_cycle_diff < cycle_diff) {
								m68k_cycle_diff += MCLKS_PER_68K;
							}
							context->cycles += m68k_cycle_diff;
							gen->bus_busy = 1;
							if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
								sync_components_pico(context, 0);
							} else {
								sync_components(context, 0);
							}
							gen->bus_busy = 0;
						}
					}

					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value, context->cycles);
					} else {
						blocked = 0;
					}
				}
			} else {
				context->sync_cycle = gen->frame_end = vdp_cycles_to_frame_end(v_context);
				//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->cycles, v_context->vcounter, v_context->hslot);
				if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
					adjust_int_cycle_pico(context, v_context);
				} else {
					adjust_int_cycle(context, v_context);
				}
			}
		} else {
			machine_freeze(config, enter_68k_debugger_immediately, gen, "Illegal write to HV Counter port %X\n", vdp_port);
		}
		if (v_context->cycles != before_cycle) {
			//printf("68K paused for %d (%d) cycles at cycle %d (%d) for write\n", v_context->cycles - context->cycles, v_context->cycles - before_cycle, context->cycles, before_cycle);
			uint32_t cycle_diff = v_context->cycles - context->cycles;
			uint32_t m68k_cycle_diff = (cycle_diff / MCLKS_PER_68K) * MCLKS_PER_68K;
			if (m68k_cycle_diff < cycle_diff) {
				m68k_cycle_diff += MCLKS_PER_68K;
			}
			context->cycles += m68k_cycle_diff;
			if (gen->header.type == SYSTEM_GENESIS) {
				//Lock the Z80 out of the bus until the VDP access is complete
				gen->bus_busy = 1;
				sync_z80(gen, v_context->cycles);
				gen->bus_busy = 0;
			}
		}
	} else if (vdp_port < 0x18) {
		psg_write(gen->psg, value);
	} else if (vdp_port < 0x1C) {
		vdp_test_port_select(gen->vdp, value);
	} else {
		uint8_t old_z80_clock_bit = gen->vdp->test_regs[1] & 1;
		vdp_test_port_write(gen->vdp, value);
		uint8_t new_z80_clock_bit = gen->vdp->test_regs[1] & 1;
		if (old_z80_clock_bit != new_z80_clock_bit) {
			gen->z80->Z80_OPTS->gen.clock_divider = new_z80_clock_bit ? MCLKS_PER_YM : MCLKS_PER_Z80;
			gen->psg->clock_inc = new_z80_clock_bit ? MCLKS_PER_YM * 16 : MCLKS_PER_PSG;
			psg_adjust_master_clock(gen->psg, gen->master_clock);
			z80_invalidate_code_range(gen->z80, 0, 0x4000);
			z80_clock_divider_updated(gen->z80->Z80_OPTS);
		}
	}

	if (did_dma) {
		gen->refresh_counter = 0;
		gen->last_sync_cycle = context->cycles;
	} else {
		//refresh may have happened while we were waiting on the VDP,
		//so advance refresh_counter but don't add any delays
		gen_update_refresh_no_wait(context);
	}
	return context;
}

static m68k_context * vdp_port_write_b(uint32_t vdp_port, m68k_context * context, uint8_t value)
{
	return vdp_port_write(vdp_port, context, vdp_port < 0x10 ? value | value << 8 : ((vdp_port & 1) ? value : 0));
}

static void * z80_vdp_port_write(uint32_t vdp_port, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	vdp_port &= 0xFF;
	if (vdp_port & 0xE0) {
		machine_freeze(config, enter_z80_debugger_immediately, gen, "Z80 write to unmapped part of VDP memory region, address %X\n", 0x7F00 | vdp_port);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		if (vdp_port < 4) {
			vdp_run_context(gen->vdp, context->Z80_CYCLE);
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_run_context_full(gen->vdp, context->Z80_CYCLE);
			vdp_control_port_write(gen->vdp, value << 8 | value, context->Z80_CYCLE);
		} else {
			machine_freeze(config, enter_z80_debugger_immediately, gen, "Illegal Z80 write to HV Counter port %X\n", vdp_port);
		}
	} else if (vdp_port < 0x18) {
		sync_sound(gen, context->Z80_CYCLE);
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value << 8 | value);
	}
	return context;
}

static uint16_t vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	genesis_context *gen = context->system;
	if (vdp_port & 0x2700E0) {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Read from unmapped part of VDP memory region, address %X\n", 0xC00000 | vdp_port);
		gen->header.enter_debugger = 1;
		if (gen->m68k->sync_cycle > gen->m68k->cycles + 1) {
			gen->m68k->sync_cycle = gen->m68k->cycles + 1;
		}
		if (gen->m68k->target_cycle > gen->m68k->sync_cycle) {
			gen->m68k->target_cycle = gen->m68k->sync_cycle;
		}
	}
	if (!gen->vdp_unlocked) {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "VDP read from %X without TMSS unlock\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	uint16_t value;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during a VDP access
	gen_update_refresh_free_access(context);

	if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
		sync_components_pico(context, 0);
	} else {
		sync_components(context, 0);
	}
	vdp_context * v_context = gen->vdp;
	uint32_t before_cycle = context->cycles;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			value = vdp_data_port_read(v_context, &context->cycles, MCLKS_PER_68K);
		} else if(vdp_port < 8) {
			value = vdp_control_port_read(v_context);
		} else {
			value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", value, v_context->cycles);
		}
	} else if (vdp_port < 0x18){
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Illegal read from PSG  port %X\n", vdp_port);
	} else {
		value = get_open_bus_value(&gen->header);
	}
	if (context->cycles != before_cycle) {
		//printf("68K paused for %d (%d) cycles at cycle %d (%d) for read\n", v_context->cycles - context->cycles, v_context->cycles - before_cycle, context->cycles, before_cycle);
		//Lock the Z80 out of the bus until the VDP access is complete
		genesis_context *gen = context->system;
		if (gen->header.type == SYSTEM_GENESIS) {
			gen->bus_busy = 1;
			sync_z80(gen, context->cycles);
			gen->bus_busy = 0;
		}
	}

	//refresh may have happened while we were waiting on the VDP,
	//so advance refresh_counter but don't add any delays
	gen_update_refresh_no_wait(context);
	return value;
}

static uint8_t vdp_port_read_b(uint32_t vdp_port, m68k_context * context)
{
	uint16_t value = vdp_port_read(vdp_port, context);
	if (vdp_port & 1) {
		return value;
	} else {
		return value >> 8;
	}
}

static uint8_t z80_vdp_port_read(uint32_t vdp_port, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	if (vdp_port & 0xE0) {
		machine_freeze(config, enter_z80_debugger_immediately, gen, "Z80 read from unmapped part of VDP memory region, address %X\n", 0x7F00 | vdp_port);
	}
	//VDP access goes over the 68K bus like a bank area access
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->cycles += 8 * MCLKS_PER_68K;


	vdp_port &= 0x1F;
	uint16_t ret;
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->Z80_CYCLE);
		uint32_t before = context->Z80_CYCLE;
		if (vdp_port < 4) {
			ret = vdp_data_port_read(gen->vdp, &context->Z80_CYCLE, MCLKS_PER_Z80);
		} else if (vdp_port < 8) {
			ret = vdp_control_port_read(gen->vdp);
		} else {
			ret = vdp_hv_counter_read(gen->vdp);
		}
		if (context->Z80_CYCLE != before) {
			gen->m68k->cycles += context->Z80_CYCLE - before;
		}
	} else {
		//TODO: Figure out the correct value today
		ret = 0xFFFF;
	}
	return vdp_port & 1 ? ret : ret >> 8;
}

//TODO: Move this inside the system context
static uint32_t zram_counter = 0;

static m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
	gen_update_refresh_free_access(context);

	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->cycles += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->cycles)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				gen->zram[location & 0x1FFF] = value;
#ifndef NO_Z80
				z80_handle_code_write(location & 0x1FFF, gen->z80);
#endif
			} else if (location < 0x6000) {
				sync_sound(gen, context->cycles);
				if (location & 1) {
					ym_data_write(gen->ym, value);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value);
				} else {
					ym_address_write_part1(gen->ym, value);
				}
			} else if (location == 0x6000) {
				gen->z80_bank_reg = (gen->z80_bank_reg >> 1 | value << 8) & 0x1FF;
				if (gen->z80_bank_reg < 0x80) {
					gen->z80->mem_pointers[1] = (gen->z80_bank_reg << 15) + ((char *)gen->z80->mem_pointers[2]);
				} else {
					gen->z80->mem_pointers[1] = NULL;
				}
			} else {
				fatal_error("68K write to unhandled Z80 address %X\n", location);
			}
		}
	} else {
		if (location < 0x10100) {
			switch(location >> 1 & 0xFF)
			{
			case 0x1:
				io_data_write(gen->io.ports, value, context->cycles);
				break;
			case 0x2:
				io_data_write(gen->io.ports+1, value, context->cycles);
				break;
			case 0x3:
				io_data_write(gen->io.ports+2, value, context->cycles);
				break;
			case 0x4:
				io_control_write(gen->io.ports, value, context->cycles);
				break;
			case 0x5:
				io_control_write(gen->io.ports+1, value, context->cycles);
				break;
			case 0x6:
				io_control_write(gen->io.ports+2, value, context->cycles);
				break;
			case 0x7:
				io_tx_write(gen->io.ports, value, context->cycles);
				break;
			case 0x8:
			case 0xB:
			case 0xE:
				//serial input port is not writeable
				break;
			case 0x9:
				io_sctrl_write(gen->io.ports, value, context->cycles);
				gen->io.ports[0].serial_ctrl = value;
				break;
			case 0xA:
				io_tx_write(gen->io.ports + 1, value, context->cycles);
				break;
			case 0xC:
				io_sctrl_write(gen->io.ports + 1, value, context->cycles);
				break;
			case 0xD:
				io_tx_write(gen->io.ports + 2, value, context->cycles);
				break;
			case 0xF:
				io_sctrl_write(gen->io.ports + 2, value, context->cycles);
				break;
			}
		} else {
			uint32_t masked = location & 0xFFF00;
			if (masked == 0x11100) {
				if (value & 1) {
					dputs("bus requesting Z80");
					if (z80_enabled) {
						z80_assert_busreq(gen->z80, context->cycles);
					} else {
						gen->z80->busack = 1;
					}
				} else {
					if (gen->z80->busreq) {
						dputs("releasing z80 bus");
						#ifdef DO_DEBUG_PRINT
						char fname[20];
						sprintf(fname, "zram-%d", zram_counter++);
						FILE * f = fopen(fname, "wb");
						fwrite(z80_ram, 1, sizeof(z80_ram), f);
						fclose(f);
						#endif
					}
					if (z80_enabled) {
						z80_clear_busreq(gen->z80, context->cycles);
					} else {
						gen->z80->busack = 0;
					}
				}
			} else if (masked == 0x11200) {
				sync_z80(gen, context->cycles);
				if (value & 1) {
					if (z80_enabled) {
						z80_clear_reset(gen->z80, context->cycles);
					} else {
						gen->z80->reset = 0;
					}
				} else {
					if (z80_enabled) {
						z80_assert_reset(gen->z80, context->cycles);
					} else {
						gen->z80->reset = 1;
					}
					ym_reset(gen->ym);
				}
			} else if (masked != 0x11300 && masked != 0x11000) {
				machine_freeze(config, enter_68k_debugger_immediately, gen, "68K write to unmapped address %X\n", location | 0xA00000);
			}
		}
	}

	//no refresh delays during IO access
	gen_update_refresh_no_wait(context);
	return context;
}

static m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	if (location < 0x10000 || (location & 0x1FFF) >= 0x100) {
		return io_write(location, context, value >> 8);
	} else {
		return io_write(location, context, value);
	}
}

static void* pico_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	printf("Pico IO write.b %X - %X\n", location, value);
	return vcontext;
}

static void* pico_io_write_w(uint32_t location, void *vcontext, uint16_t value)
{
	uint32_t port = location & 0xFE;
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	if (port == 0x10) {
		sync_sound_pico(gen, context->cycles);
		pico_pcm_data_write(gen->adpcm, value);
		printf("PICO ADPCM Data: %04X\n", value);
		if (context->int_num == 3) {
			adjust_int_cycle_pico(context, gen->vdp);
		}
	} else if (port == 0x12) {
		sync_sound_pico(gen, context->cycles);
		printf("PICO ADPCM Control: %04X\n", value);
		pico_pcm_ctrl_write(gen->adpcm, value);
		adjust_int_cycle_pico(context, gen->vdp);
	} else {
		return pico_io_write(location, vcontext, value);
	}
	return vcontext;
}

#define FOREIGN 0x80
#define HZ50 0x40
#define USA FOREIGN
#define JAP 0x00
#define EUR (HZ50|FOREIGN)
#define NO_DISK 0x20

static uint8_t io_read(uint32_t location, m68k_context * context)
{
	uint8_t value;
	genesis_context *gen = context->system;

	//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
	gen_update_refresh_free_access(context);

	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->cycles += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->cycles)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = gen->zram[location & 0x1FFF];
			} else if (location < 0x6000) {
				sync_sound(gen, context->cycles);
				value = ym_read_status(gen->ym, context->cycles, location);
			} else if (location < 0x7F00) {
				value = 0xFF;
			} else {
				machine_freeze(config, enter_68k_debugger_immediately, gen, "Read of Z80 VDP memory window by 68K: %X\n", location | 0xA00000);
				value = 0xFF;
			}
		} else {
			uint16_t word = get_open_bus_value(&gen->header);
			value = location & 1 ? word : word >> 8;
		}
	} else {
		if (location < 0x10100) {
			switch(location >> 1 & 0xFF)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				value = gen->version_reg;
				break;
			case 0x1:
				value = io_data_read(gen->io.ports, context->cycles);
				break;
			case 0x2:
				value = io_data_read(gen->io.ports+1, context->cycles);
				break;
			case 0x3:
				value = io_data_read(gen->io.ports+2, context->cycles);
				break;
			case 0x4:
				value = gen->io.ports[0].control;
				break;
			case 0x5:
				value = gen->io.ports[1].control;
				break;
			case 0x6:
				value = gen->io.ports[2].control;
				break;
			case 0x7:
				value = gen->io.ports[0].serial_out;
				break;
			case 0x8:
				value = io_rx_read(gen->io.ports, context->cycles);
				break;
			case 0x9:
				value = io_sctrl_read(gen->io.ports, context->cycles);
				break;
			case 0xA:
				value = gen->io.ports[1].serial_out;
				break;
			case 0xB:
				value = io_rx_read(gen->io.ports + 1, context->cycles);
				break;
			case 0xC:
				value = io_sctrl_read(gen->io.ports + 1, context->cycles);
				break;
			case 0xD:
				value = gen->io.ports[2].serial_out;
				break;
			case 0xE:
				value = io_rx_read(gen->io.ports + 2, context->cycles);
				break;
			case 0xF:
				value = io_sctrl_read(gen->io.ports + 2, context->cycles);
				break;
			default:
				value = get_open_bus_value(&gen->header) >> 8;
			}
		} else {
			uint32_t masked = location & 0xFFF01;
			if (masked == 0x11100) {
				value = z80_enabled ? !z80_get_busack(gen->z80, context->cycles) : !gen->z80->busack;
				value |= (get_open_bus_value(&gen->header) >> 8) & 0xFE;
				dprintf("Byte read of BUSREQ returned %d @ %d (reset: %d)\n", value, context->cycles, gen->z80->reset);
			} else if (masked == 0x11200) {
				value = !gen->z80->reset;
				value |= (get_open_bus_value(&gen->header) >> 8) & 0xFE;
			} else if (masked == 0x11300 || masked == 0x11000) {
				//A11300 is apparently completely unused
				//A11000 is the memory control register which I am assuming is write only
				value = get_open_bus_value(&gen->header) >> 8;
			} else if (masked == 0x11001 || masked == 0x11101 || masked == 0x11201 || masked == 0x11301) {
				return get_open_bus_value(&gen->header);
			} else {
				location |= 0xA00000;
				machine_freeze(config, enter_68k_debugger_immediately, gen, "68K read of unmapped IO location %X\n", location);
				value = 0xFF;
			}
		}
	}

	//no refresh delays during IO access
	gen_update_refresh_no_wait(context);
	return value;
}

static uint16_t io_read_w(uint32_t location, m68k_context * context)
{
	genesis_context *gen = context->system;
	uint16_t value = io_read(location, context);
	if (location < 0x10000 || (location & 0x1FFF) < 0x100) {
		value = value | (value << 8);
	} else {
		value <<= 8;
		value |= get_open_bus_value(&gen->header) & 0xFF;
	}
	return value;
}

static uint8_t pico_io_read(uint32_t location, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	uint16_t tmp;
	switch(location >> 1 & 0x7F)
	{
	case 0:
		return gen->version_reg;
	case 1:
		return gen->pico_button_state;
	case 2:
		return gen->pico_pen_x >> 8;
	case 3:
		return gen->pico_pen_x;
	case 4:
		return gen->pico_pen_y >> 8;
	case 5:
		return gen->pico_pen_y;
	case 6:
		return gen->pico_page;
	case 7:
		//Copera titles seem to expect bit 0 to be 0 on a Copera and 1 on a Pico
		if (gen->header.type == SYSTEM_PICO) {
			return 1;
		} else {
			return 0;
		}
	case 8:
		//printf("uPD7759 data read @ %u\n", m68k->cycles);
		sync_sound_pico(gen, m68k->cycles);
		tmp = pico_pcm_data_read(gen->adpcm);
		return (location & 1) ? tmp : tmp >> 8;
	case 9:
		//printf("uPD7759 contro/status read @ %u\n", m68k->cycles);
		sync_sound_pico(gen, m68k->cycles);
		tmp = pico_pcm_ctrl_read(gen->adpcm);
		return (location & 1) ? tmp : tmp >> 8;
		return 0;
	default:
		printf("Unknown Pico IO read %X @ %u\n", location, m68k->cycles);
		return 0xFF;
	}
}

static uint16_t pico_io_read_w(uint32_t location, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	uint32_t port = location & 0xFE;
	if (port == 0x10) {
		sync_sound_pico(gen, m68k->cycles);
		return pico_pcm_data_read(gen->adpcm);
	} else if (port == 0x12) {
		sync_sound_pico(gen, m68k->cycles);
		return pico_pcm_ctrl_read(gen->adpcm);
	}
	uint16_t value = pico_io_read(location, vcontext);
	return value | (value << 8);
}

static uint16_t copera_io_read_w(uint32_t location, void *vcontext)
{
	printf("Unhandled Copera 16-bit read %X\n", location);
	return 0xFFFF;
}

static uint8_t copera_io_read(uint32_t location, void *vcontext)
{
	uint8_t ret;
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	//FIXME: Copera sound hardware has a separate clock
	switch (location & 0xFF)
	{
	case 1:
	case 5:
		ymz263b_run(gen->ymz, m68k->cycles);
		ret = ymz263b_status_read(gen->ymz);
		printf("Copera YMZ263 Status read - %X: %X\n", location, ret);
		adjust_int_cycle_pico(gen->m68k, gen->vdp);
		return ret;
	case 3:
	case 7:
		ymz263b_run(gen->ymz, m68k->cycles);
		ret = ymz263b_data_read(gen->ymz, location & 4);
		printf("Copera YMZ263 Data read - %X: %X\n", location, ret);
		return ret;
	case 0x28:
		ret = ymf262_read_status(gen->opl, m68k->cycles, 0);
		printf("Copera YMF262 Status read - %X: %X\n", location, ret);
		return ret;
	default:
		printf("Unhandled Copera 8-bit read %X\n", location);
		return 0xFF;
	}
}

static void* copera_io_write_w(uint32_t location, void *vcontext, uint16_t value)
{
	printf("Unhandled Copera 16-bit write %X: %X\n", location, value);
	return vcontext;
}

static void* copera_io_write(uint32_t location, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	//FIXME: Copera sound hardware has a separate clock
	switch (location & 0xFF)
	{
	case 1:
	case 5:
		ymz263b_run(gen->ymz, m68k->cycles);
		printf("Copera YMZ263 Address write - %X: %X\n", location, value);
		ymz263b_address_write(gen->ymz, value);
		break;
	case 3:
	case 7:
		ymz263b_run(gen->ymz, m68k->cycles);
		printf("Copera YMZ263 Channel #%d Data write - %X: %X\n", ((location & 4) >> 2) + 1, location, value);
		ymz263b_data_write(gen->ymz, location & 4, value);
		adjust_int_cycle_pico(gen->m68k, gen->vdp);
		break;
	case 0x24:
	case 0x34:
		printf("Copera YMF262 Address Part #%d write - %X: %X\n", ((location >> 4) & 1) + 1, location, value);
		ymf262_run(gen->opl, m68k->cycles);
		if (location & 0x10) {
			ymf262_address_write_part2(gen->opl, value);
		} else {
			ymf262_address_write_part1(gen->opl, value);
		}
		break;
	case 0x28:
		printf("Copera YMF262 Data write - %X: %X\n", location, value);
		ymf262_run(gen->opl, m68k->cycles);
		ymf262_data_write(gen->opl, value);
		break;
	case 0x40:
		//Bit 4 = SCI
		//Bit 5 = DIN
		//Bit 6 = A0
		//Possible Bit 0-3 are the same but for the other YM7128B
		printf("Copera YM7128B Write - %X: %X\n", location, value);
		break;
	default:
		printf("Unhandled Copera 8-bit write %X: %X\n", location, value);
	}
	return vcontext;
}

static void * z80_write_ym(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->Z80_CYCLE);
	if (location & 1) {
		ym_data_write(gen->ym, value);
	} else if (location & 2) {
		ym_address_write_part2(gen->ym, value);
	} else {
		ym_address_write_part1(gen->ym, value);
	}
	return context;
}

static uint8_t z80_read_ym(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->Z80_CYCLE);
	return ym_read_status(gen->ym, context->Z80_CYCLE, location);
}

static uint8_t z80_read_bank(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->Z80_CYCLE = gen->m68k->cycles;
	}
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->cycles += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	if (context->mem_pointers[1]) {
		return context->mem_pointers[1][location ^ 1];
	}
	uint32_t address = gen->z80_bank_reg << 15 | location;
	if (address >= 0xC00000 && address < 0xE00000) {
		return z80_vdp_port_read(location & 0xFF, context);
	} else if (address >= 0xA10000 && address <= 0xA10001) {
		//Apparently version reg can be read through Z80 banked area
		//TODO: Check rest of IO region addresses
		return gen->version_reg;
	} else if (gen->mars && address >= 0xA15100 && address <= 0xA15400) {
		uint32_t prev_cycles = gen->m68k->cycles;
		uint8_t ret = s32x_68k_read_b(address, gen->m68k);
		if (gen->m68k->cycles != prev_cycles) {
			context->Z80_CYCLE += gen->m68k->cycles - prev_cycles;
		}
		return ret;
	} else {
		fprintf(stderr, "Unhandled read by Z80 from address %X through banked memory area (%X)\n", address, gen->z80_bank_reg << 15);
	}
	return 0;
}

static void *z80_write_bank(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->Z80_CYCLE = gen->m68k->cycles;
	}
	//typical delay from bus arbitration
	context->Z80_CYCLE += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->cycles += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	uint32_t address = gen->z80_bank_reg << 15 | location;
	if (address >= 0xE00000) {
		address &= 0xFFFF;
		((uint8_t *)gen->work_ram)[address ^ 1] = value;
	} else if (address >= 0xC00000) {
		z80_vdp_port_write(location & 0xFF, context, value);
	} else if (gen->mars && address >= 0xA15100 && address <= 0xA15400) {
		uint32_t prev_cycles = gen->m68k->cycles;
		s32x_68k_write_b(address, gen->m68k, value);
		if (gen->m68k->cycles != prev_cycles) {
			context->Z80_CYCLE += gen->m68k->cycles - prev_cycles;
		}
	} else {
		fprintf(stderr, "Unhandled write by Z80 to address %X through banked memory area\n", address);
	}
	return context;
}

static void *z80_write_bank_reg(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;

	gen->z80_bank_reg = (gen->z80_bank_reg >> 1 | value << 8) & 0x1FF;
	gen_update_z80_bank_pointer(context->system);

	return context;
}

static uint16_t unused_read(uint32_t location, void *vcontext)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	if (location >= 0x800000) {
		//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
		gen->refresh_counter += context->cycles - 4*MCLKS_PER_68K - gen->last_sync_cycle;
		context->cycles += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
		gen->refresh_counter += 4*MCLKS_PER_68K;
		gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
		gen->last_sync_cycle = context->cycles;
	}

	if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//Only called if the cart/exp doesn't have a more specific handler for this region
		return get_open_bus_value(&gen->header);
	} else if (location == 0xA14000 || location == 0xA14002) {
		if (gen->version_reg & 0xF) {
			return gen->tmss_lock[location >> 1 & 1];
		} else {
			machine_freeze(config, enter_68k_debugger_immediately, gen, "68K read from TMSS lock when TMSS is not present %X\n", location);
			return 0xFFFF;
		}
	} else if (location == 0xA14100) {
		if (gen->version_reg & 0xF) {
			return get_open_bus_value(&gen->header);
		} else {
			machine_freeze(config, enter_68k_debugger_immediately, gen, "68K read from TMSS control when TMSS is not present %X\n", location);
			return 0xFFFF;
		}
	} else {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Unmapped read from %X\n", location);
		return 0xFFFF;
	}
}

static uint8_t unused_read_b(uint32_t location, void *vcontext)
{
	uint16_t v = unused_read(location & 0xFFFFFE, vcontext);
	if (location & 1) {
		return v;
	} else {
		return v >> 8;
	}
}

static void check_tmss_lock(genesis_context *gen)
{
	gen->vdp_unlocked = gen->tmss_lock[0] == 0x5345 && gen->tmss_lock[1] == 0x4741;
}

static void toggle_tmss_rom(genesis_context *gen)
{
	m68k_context *context = gen->m68k;
	for (int i = 0; i < NUM_MEM_AREAS; i++)
	{
		uint16_t *tmp = context->mem_pointers[i];
		context->mem_pointers[i] = gen->tmss_pointers[i];
		gen->tmss_pointers[i] = tmp;
	}
	m68k_invalidate_code_range(context, 0, 0x400000);
}

static void *unused_write(uint32_t location, void *vcontext, uint16_t value)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	if (location >= 0x800000) {
		//do refresh check here so we can avoid adding a penalty for a refresh that happens during an IO area access
		gen->refresh_counter += context->cycles - 4*MCLKS_PER_68K - gen->last_sync_cycle;
		context->cycles += REFRESH_DELAY * MCLKS_PER_68K * (gen->refresh_counter / (MCLKS_PER_68K * REFRESH_INTERVAL));
		gen->refresh_counter += 4*MCLKS_PER_68K;
		gen->refresh_counter = gen->refresh_counter % (MCLKS_PER_68K * REFRESH_INTERVAL);
		gen->last_sync_cycle = context->cycles;
	}

	uint8_t has_tmss = gen->version_reg & 0xF;
	if (has_tmss && (location == 0xA14000 || location == 0xA14002)) {
		gen->tmss_lock[location >> 1 & 1] = value;
		check_tmss_lock(gen);
	} else if (has_tmss && location == 0xA14100) {
		value &= 1;
		if (gen->tmss != value) {
			gen->tmss = value;
			toggle_tmss_rom(gen);
		}
	} else if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//these writes are ignored when no relevant hardware is present
	} else {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Unmapped write to %X\n", location);
	}
	return vcontext;
}

static void *unused_write_b(uint32_t location, void *vcontext, uint8_t value)
{
	m68k_context *context = vcontext;
	genesis_context *gen = context->system;
	uint8_t has_tmss = gen->version_reg & 0xF;
	if (has_tmss && location >= 0xA14000 && location <= 0xA14003) {
		uint32_t offset = location >> 1 & 1;
		if (location & 1) {
			gen->tmss_lock[offset] &= 0xFF00;
			gen->tmss_lock[offset] |= value;
		} else {
			gen->tmss_lock[offset] &= 0xFF;
			gen->tmss_lock[offset] |= value << 8;
		}
		check_tmss_lock(gen);
	} else if (has_tmss && (location == 0xA14100 || location == 0xA14101)) {
		if (location & 1) {
			value &= 1;
			if (gen->tmss != value) {
				gen->tmss = value;
				toggle_tmss_rom(gen);
			}
		}
	} else if (location < 0x800000 || (location >= 0xA13000 && location < 0xA13100) || (location >= 0xA12000 && location < 0xA12100)) {
		//these writes are ignored when no relevant hardware is present
	} else {
		machine_freeze(config, enter_68k_debugger_immediately, gen, "Unmapped byte write to %X\n", location);
	}
	return vcontext;
}

static void set_speed_percent(system_header * system, uint32_t percent)
{
	genesis_context *context = (genesis_context *)system;
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;
	if (context->header.type != SYSTEM_PICO && context->header.type != SYSTEM_COPERA) {
		while (context->ym->current_cycle != context->psg->cycles) {
			sync_sound(context, context->psg->cycles + MCLKS_PER_PSG);
		}
		if (context->expansion) {
			segacd_context *cd = context->expansion;
			segacd_set_speed_percent(cd, percent);
		}
		if (context->mars) {
			s32x_set_speed(context->mars, context->master_clock);
		}
		ym_adjust_master_clock(context->ym, context->master_clock);
	} else {
		while (context->adpcm->cycle != context->psg->cycles) {
			sync_sound_pico(context, context->psg->cycles + MCLKS_PER_PSG);
		}
		if (context->ymz) {
			ymz263b_adjust_master_clock(context->ymz, context->master_clock);
			ymf262_adjust_master_clock(context->opl, context->master_clock);
		}
		pico_pcm_adjust_master_clock(context->adpcm, context->master_clock);
	}
	psg_adjust_master_clock(context->psg, context->master_clock);
}

void set_region(genesis_context *gen, rom_info *info, uint8_t region)
{
	if (!region) {
		char * def_region = tern_find_path_default(config, "system\0default_region\0", (tern_val){.ptrval = "U"}, TVAL_PTR).ptrval;
		char * force_region = tern_find_path_default(config, "system\0force_region\0", (tern_val){.ptrval = "off"}, TVAL_PTR).ptrval;
		
		if (!info->regions || (info->regions & translate_region_char(toupper(*def_region))) || !strcmp(force_region, "on")) {
			region = translate_region_char(toupper(*def_region));
		} else {
			region = info->regions;
		}
	}
	uint8_t is_50hz = 0;
	if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
		if (region & REGION_E) {
			is_50hz = 1;
			gen->version_reg = 0x20;
		} else if (region & REGION_J) {
			gen->version_reg = 0;
		} else {
			gen->version_reg = 0x40;
		}
	} else {
		if (region & REGION_E) {
			gen->version_reg = NO_DISK | EUR;
			is_50hz = 1; 
		} else if (region & REGION_J) {
			gen->version_reg = NO_DISK | JAP;
		} else {
			gen->version_reg = NO_DISK | USA;
		}
	}

	if (is_50hz) {
		gen->normal_clock = MCLKS_PAL;
		gen->soft_flush_cycles = MCLKS_LINE * 313 / 3 + 2;
	} else {
		gen->normal_clock = MCLKS_NTSC;
		gen->soft_flush_cycles = MCLKS_LINE * 262 / 3 + 2;
	}
	gen->master_clock = gen->normal_clock;
}

static uint8_t load_state(system_header *system, uint8_t slot)
{
	genesis_context *gen = (genesis_context *)system;
	char *statepath = get_slot_name(system, slot, "state");
	deserialize_buffer state;
	uint32_t pc = 0;
	uint8_t ret;
#ifndef NEW_CORE
	if (!gen->m68k->resume_pc) {
		system->delayed_load_slot = slot + 1;
		gen->m68k->should_return = 1;
		ret = get_modification_time(statepath) != 0;
		if (!ret) {
			strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
			ret = get_modification_time(statepath) != 0;
		}
		goto done;
	}
#endif
	if (load_from_file(&state, statepath)) {
		genesis_deserialize(&state, gen);
		free(state.data);
		ret = 1;
	} else {
		strcpy(statepath + strlen(statepath)-strlen("state"), "gst");
		pc = load_gst(gen, statepath);
		ret = pc != 0;
	}
	if (ret) {
		debug_message("Loaded state from %s\n", statepath);
	}
done:
	free(statepath);
	return ret;
}

static void handle_reset_requests(genesis_context *gen)
{
	while (gen->reset_requested || gen->header.delayed_load_slot)
	{
		if (gen->reset_requested) {
			gen->reset_requested = 0;
			gen->m68k->should_return = 0;
			if (gen->header.type == SYSTEM_GENESIS) {
				z80_assert_reset(gen->z80, gen->m68k->cycles);
				z80_clear_busreq(gen->z80, gen->m68k->cycles);
			}
			if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
				ym_reset(gen->ym);
			}
			if (gen->mapper_type == MAPPER_RADICA) {
				radica_reset(gen);
			}
			//Is there any sort of VDP reset?
			m68k_reset(gen->m68k);
		}
		if (gen->header.delayed_load_slot) {
			load_state(&gen->header, gen->header.delayed_load_slot - 1);
			gen->header.delayed_load_slot = 0;
			resume_68k(gen->m68k);
		}
	}
	if (gen->header.force_release || render_should_release_on_exit()) {
		bindings_release_capture();
		vdp_release_framebuffer(gen->vdp);
		if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
			render_pause_source(gen->ym->audio);
		}
		render_pause_source(gen->psg->audio);
	}
}

static void start_genesis(system_header *system, char *statefile)
{
	genesis_context *gen = (genesis_context *)system;
	if (statefile) {
		//first try loading as a native format savestate
		deserialize_buffer state;
		uint32_t pc;
		if (load_from_file(&state, statefile)) {
			genesis_deserialize(&state, gen);
			free(state.data);
#ifdef NEW_CORE
			pc = gen->m68k->pc;
#else
			//HACK
			pc = gen->m68k->last_prefetch_address;
#endif
		} else {
			pc = load_gst(gen, statefile);
			if (!pc) {
				fatal_error("Failed to load save state %s\n", statefile);
			}
		}
		printf("Loaded %s\n", statefile);
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
#ifndef IS_LIB
			insert_breakpoint(gen->m68k, pc, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
#endif
		}
		if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
			adjust_int_cycle_pico(gen->m68k, gen->vdp);
		} else {
			adjust_int_cycle(gen->m68k, gen->vdp);
		}
		start_68k_context(gen->m68k, pc);
	} else {
		if (gen->header.enter_debugger) {
			gen->header.enter_debugger = 0;
#ifndef IS_LIB
			uint32_t address = read_word(4, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen, gen->m68k) << 16
				| read_word(6, (void **)gen->m68k->mem_pointers, &gen->m68k->opts->gen, gen->m68k);
			insert_breakpoint(gen->m68k, address, gen->header.debugger_type == DEBUGGER_NATIVE ? debugger : gdb_debug_enter);
#endif
		}
		m68k_reset(gen->m68k);
	}
#ifdef NEW_CORE
	while (!gen->m68k->should_return) {
		if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
			sync_components_pico(gen->m68k, gen->m68k->pc);
		} else {
			sync_components(gen->m68k, gen->m68k->pc);
		}
		m68k_execute(gen->m68k, gen->m68k->target_cycle);
	}
	gen->m68k->should_return = 0;
#endif
	handle_reset_requests(gen);
	return;
}

static void resume_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->header.force_release || render_should_release_on_exit()) {
		gen->header.force_release = 0;
		if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
			render_set_video_standard((gen->version_reg & 0x60) == 0x20 ? VID_PAL : VID_NTSC);
		} else {
			render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
		}
		bindings_reacquire_capture();
		vdp_reacquire_framebuffer(gen->vdp);
		if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
			render_resume_source(gen->ym->audio);
		}
		render_resume_source(gen->psg->audio);
	}
#ifdef NEW_CORE
	while (!gen->m68k->should_return) {
		if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
			sync_components_pico(gen->m68k, gen->m68k->pc);
		} else {
			sync_components(gen->m68k, gen->m68k->pc);
		}
		m68k_execute(gen->m68k, gen->m68k->target_cycle);
	}
	gen->m68k->should_return = 0;
#else
	resume_68k(gen->m68k);
#endif
	handle_reset_requests(gen);
}

static void inc_debug_mode(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	vdp_inc_debug_mode(gen->vdp);
}

static void request_exit(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	gen->m68k->target_cycle = gen->m68k->cycles;
	gen->m68k->should_return = 1;
}

static void persist_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	FILE *f;
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		char *bram_name = path_append(system->save_dir, "internal.bram");
		f = fopen(bram_name, "wb");
		if (f) {
			fwrite(cd->bram, 1, 8 * 1024, f);
			fclose(f);
			printf("Saved internal BRAM to %s\n", bram_name);
		}
		free(bram_name);
		if (cd->bram_cart_id < 8) {
			bram_name = path_append(system->save_dir, "cart.bram");
			f = fopen(bram_name, "wb");
			if (f) {
				long configured_size = 0x2000 << cd->bram_cart_id;
				fwrite(cd->bram_cart, 1, configured_size, f);
				fclose(f);
				printf("Saved BRAM cart to %s\n", bram_name);
			}
			free(bram_name);
		}
	}
	if (gen->save_type == SAVE_NONE) {
		return;
	}
	f = fopen(save_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open %s file %s for writing\n", save_type_name(gen->save_type), save_filename);
		return;
	}
	if (gen->save_type == RAM_FLAG_BOTH) {
		byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
	}
	fwrite(gen->save_storage, 1, gen->save_size, f);
	if (gen->save_type == RAM_FLAG_BOTH) {
		byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
	}
	fclose(f);
	printf("Saved %s to %s\n", save_type_name(gen->save_type), save_filename);
}

static void load_save(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	FILE * f = fopen(save_filename, "rb");
	if (f) {
		uint32_t read = fread(gen->save_storage, 1, gen->save_size, f);
		fclose(f);
		if (read > 0) {
			if (gen->save_type == RAM_FLAG_BOTH) {
				byteswap_rom(gen->save_size, (uint16_t *)gen->save_storage);
			}
			printf("Loaded %s from %s\n", save_type_name(gen->save_type), save_filename);
		}
	}
	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		char *bram_name = path_append(system->save_dir, "internal.bram");
		f = fopen(bram_name, "rb");
		if (f) {
			uint32_t read = fread(cd->bram, 1, 8 * 1024, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded internal BRAM from %s\n", bram_name);
			}
		} else {
			segacd_format_bram(cd->bram, 8 * 1024);
		}
		free(bram_name);
		bram_name = path_append(system->save_dir, "cart.bram");
		f = fopen(bram_name, "rb");
		long configured_size = 0x2000 << cd->bram_cart_id;
		if (f) {
			long existing_size = nearest_pow2(file_size(f));
			if (existing_size > 1 * 1024 * 1024) {
				existing_size = 1 * 1024 * 1024;
			}
			if (existing_size != configured_size) {
				if (existing_size > configured_size) {
					free(cd->bram_cart);
					cd->bram_cart = calloc(existing_size, 1);
				}
				cd->bram_cart_id = 0;
				while (existing_size > (0x2000 <<cd->bram_cart_id)) {
					cd->bram_cart_id++;
				}
			}
			uint32_t read = fread(cd->bram_cart, 1, existing_size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded BRAM cart from %s\n", bram_name);
			}
		} else {
			segacd_format_bram(cd->bram_cart, configured_size);
		}
		free(bram_name);
	}
}

static void soft_reset(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->reset_cycle == CYCLE_NEVER) {
		double random = (double)rand()/(double)RAND_MAX;
		gen->reset_cycle = gen->m68k->cycles + random * MCLKS_LINE * (gen->version_reg & HZ50 ? LINES_PAL : LINES_NTSC);
		if (gen->reset_cycle < gen->m68k->target_cycle) {
			gen->m68k->target_cycle = gen->reset_cycle;
		}
	}
}

static void free_genesis(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->expansion) {
		free_segacd(gen->expansion);
	}
	if (gen->mars) {
		free_32x(gen->mars);
	}
	vdp_free(gen->vdp);
	memmap_chunk *map = (memmap_chunk *)gen->m68k->opts->gen.memmap;
	m68k_options_free(gen->m68k->opts);
	aligned_free(gen->cart);
	free(gen->m68k);
	free(gen->work_ram);
	if (gen->header.type == SYSTEM_GENESIS) {
		z80_options_free(gen->z80->Z80_OPTS);
		free(gen->z80);
		free(gen->zram);
	}
	if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
		pico_pcm_free(gen->adpcm);
		free(gen->adpcm);
		if (gen->ymz) {
			ymz263b_free(gen->ymz);
			free(gen->ymz);
			ymf262_free(gen->opl);
		}
	} else {
		ym_free(gen->ym);
	}
	psg_free(gen->psg);
	free(gen->header.save_dir);
	free_rom_info(&gen->header.info);
	free(gen->lock_on);
	if (gen->save_type != SAVE_NONE && gen->mapper_type != MAPPER_SEGA_MED_V2) {
		free(gen->save_storage);
	}
	free(map);
	free(gen);
}

static void gamepad_down(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_gamepad_down(&gen->io, gamepad_num, button);
	if (gen->mapper_type == MAPPER_JCART) {
		jcart_gamepad_down(gen, gamepad_num, button);
	}
}

static void gamepad_up(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_gamepad_up(&gen->io, gamepad_num, button);
	if (gen->mapper_type == MAPPER_JCART) {
		jcart_gamepad_up(gen, gamepad_num, button);
	}
}

static void pico_update_page(genesis_context *gen)
{
#ifndef IS_LIB
	uint8_t page_num = 0;
	uint8_t page = gen->pico_page;
	while (page)
	{
		page_num++;
		page >>= 1;
	}
	render_clear_window(gen->pico_story_window, 236, 205, 27);
	int x, width;
	if (page_num) {
		x = 0;
		width = 640;
	} else {
		render_fill_rect(gen->pico_story_window, 50, 29, 96, 0, 0, 320, 320);
		x = 320;
		width = 320;
	}
	if (gen->pico_story_pages[page_num] == 0xFF && page_num == 6 && gen->pico_story_pages[page_num - 1]) {
		//repeat last page if storybook doesn't have a full 6 pages
		//hack to deal with storybooks that skip page 5
		page_num--;
	}
	if (gen->pico_story_pages[page_num] != 0xFF) {
		render_draw_image(gen->pico_story_window, gen->pico_story_pages[page_num], x, 0, width, 320);
	} else {
		uint8_t grey = page * (255 / 5);
		render_fill_rect(gen->pico_story_window, grey, grey, grey, x, 0, width, 320);
	}
	render_window_refresh(gen->pico_story_window);
#endif
}

static void gamepad_down_pico(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	if (gamepad_num != 1) {
		return;
	}
	//TODO: storyware display
	if (button == BUTTON_C) {
		gen->pico_page <<= 1;
		gen->pico_page |= 1;
		gen->pico_page &= 0x3F;
		pico_update_page(gen);
	} else if (button == BUTTON_Z) {
		gen->pico_page >>= 1;
		pico_update_page(gen);
	} else if (button < BUTTON_B) {
		gen->pico_button_state &= ~(1 << (button - 1));
	}
}

static void gamepad_up_pico(system_header *system, uint8_t gamepad_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	if (gamepad_num != 1) {
		return;
	}
	if (button < BUTTON_B) {
		gen->pico_button_state |= 1 << (button - 1);
	}
	return;
}

static void mouse_down(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_down(&gen->io, mouse_num, button);
}

static void mouse_up(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_up(&gen->io, mouse_num, button);
}

static void mouse_down_pico(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	if (button == MOUSE_LEFT) {
		gen->pico_button_state &= ~0x80;
	}
}

static void mouse_up_pico(system_header *system, uint8_t mouse_num, uint8_t button)
{
	genesis_context *gen = (genesis_context *)system;
	if (button == MOUSE_LEFT) {
		gen->pico_button_state |= 0x80;
	}
}

static void mouse_motion_absolute(system_header *system, uint8_t mouse_num, uint16_t x, uint16_t y)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_motion_absolute(&gen->io, mouse_num, x, y);
}

static void mouse_motion_relative(system_header *system, uint8_t mouse_num, int32_t x, int32_t y)
{
	genesis_context *gen = (genesis_context *)system;
	io_mouse_motion_relative(&gen->io, mouse_num, x, y);
}

static void mouse_motion_absolute_pico(system_header *system, uint8_t mouse_num, uint16_t x, uint16_t y)
{
	genesis_context *gen = (genesis_context *)system;
	//FIXME: coordinate translation feels a little off for storybook area
	//TODO: limit to mouse motion on emulated storyware/drawing area
	if (y < 24) {
		y = 24;
	}
	gen->pico_pen_x = x * (0x17D - 0x3C) / 640 + 0x3C;
	if (y < 320) {
		gen->pico_pen_y = (y-24) * (0x3F4-0x2F8) / (320-24) + 0x2F8;
	} else {
		gen->pico_pen_y = (y - 320) * (0x2f8-0x1FC) / 320 + 0x1FC;
	}
}

static void mouse_motion_relative_pico(system_header *system, uint8_t mouse_num, int32_t x, int32_t y)
{
	genesis_context *gen = (genesis_context *)system;
	//TODO: scale properly
	//TODO: limit to mouse motion on emulated storyware/drawing area
	gen->pico_pen_x += x;
	gen->pico_pen_y += y;
}

static void keyboard_down(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	io_keyboard_down(&gen->io, scancode);
}

static void keyboard_up(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	io_keyboard_up(&gen->io, scancode);
}

static void keyboard_down_pico(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	//TODO: Keyboard Pico emulation
}

static void keyboard_up_pico(system_header *system, uint8_t scancode)
{
	genesis_context *gen = (genesis_context *)system;
	//TODO: Keyboard Pico emulation
}

static void set_audio_config(genesis_context *gen)
{
	char *config_gain;
	config_gain = tern_find_path(config, "audio\0psg_gain\0", TVAL_PTR).ptrval;
	render_audio_source_gaindb(gen->psg->audio, config_gain ? atof(config_gain) : 0.0f);
	if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
		config_gain = tern_find_path(config, "audio\0fm_gain\0", TVAL_PTR).ptrval;
		render_audio_source_gaindb(gen->ym->audio, config_gain ? atof(config_gain) : 0.0f);

		char *config_dac = tern_find_path_default(config, "audio\0fm_dac\0", (tern_val){.ptrval="auto"}, TVAL_PTR).ptrval;
		uint8_t zero_offset = 0;
		if (!strcmp(config_dac, "auto")) {
			zero_offset = gen->ym->status_address_mask != 0;
		} else if (!strcmp(config_dac, "zero_offset")) {
			zero_offset = 1;
		}
		ym_enable_zero_offset(gen->ym, zero_offset);
	}

	if (gen->expansion) {
		segacd_context *cd = gen->expansion;
		config_gain = tern_find_path(config, "audio\0rf5c164_gain\0", TVAL_PTR).ptrval;
		render_audio_source_gaindb(cd->pcm.audio, config_gain ? atof(config_gain) : -6.0f);
		config_gain = tern_find_path(config, "audio\0cdda_gain\0", TVAL_PTR).ptrval;
		render_audio_source_gaindb(cd->fader.audio, config_gain ? atof(config_gain) : -9.5f);
	}
}

static void config_updated(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	if (gen->header.type == SYSTEM_GENESIS) {
		setup_io_devices(config, &system->info, &gen->io);
	}
	set_audio_config(gen);
	//sample rate may have changed
	if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
		ym_adjust_master_clock(gen->ym, gen->master_clock);
	}
	psg_adjust_master_clock(gen->psg, gen->master_clock);
	if (gen->expansion) {
		segacd_config_updated(gen->expansion);
	}
}

static void start_vgm_log(system_header *system, char *filename)
{
	genesis_context *gen = (genesis_context *)system;
	vgm_writer *vgm = vgm_write_open(filename, gen->version_reg & HZ50 ? 50 : 60, gen->normal_clock, gen->m68k->cycles);
	if (vgm) {
		printf("Started logging VGM to %s\n", filename);
		if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
			sync_sound(gen, vgm->last_cycle);
			ym_vgm_log(gen->ym, gen->normal_clock, vgm);
		} else {
			sync_sound_pico(gen, vgm->last_cycle);
			if (gen->opl) {
				ymf262_vgm_log(gen->opl, gen->normal_clock, vgm);
			}
		}
		psg_vgm_log(gen->psg, gen->normal_clock, vgm);
		gen->header.vgm_logging = 1;
	} else {
		printf("Failed to start logging to %s\n", filename);
	}
}

static void stop_vgm_log(system_header *system)
{
	puts("Stopped VGM log");
	genesis_context *gen = (genesis_context *)system;
	vgm_close(gen->psg->vgm);
	if (gen->header.type != SYSTEM_PICO && gen->header.type != SYSTEM_COPERA) {
		gen->ym->vgm = NULL;
	}
	gen->psg->vgm = NULL;
	gen->header.vgm_logging = 0;
}

static void toggle_debug_view(system_header *system, uint8_t debug_view)
{
#ifndef IS_LIB
	genesis_context *gen = (genesis_context *)system;
	if (debug_view < DEBUG_OSCILLOSCOPE) {
		vdp_toggle_debug_view(gen->vdp, debug_view);
	} else if (debug_view == DEBUG_OSCILLOSCOPE) {
		if (gen->psg->scope) {
			oscilloscope *scope = gen->psg->scope;
			if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
				gen->adpcm->scope = NULL;
			} else {
				gen->ym->scope = NULL;
			}
			gen->psg->scope = NULL;
			if (gen->expansion) {
				segacd_context *cd = gen->expansion;
				cd->pcm.scope = NULL;
			}
			if (gen->mars) {
				gen->mars->scope = NULL;
			}
			scope_close(scope);
		} else {
			oscilloscope *scope = create_oscilloscope();
			if (gen->header.type == SYSTEM_PICO || gen->header.type == SYSTEM_COPERA) {
				pico_pcm_enable_scope(gen->adpcm, scope, gen->normal_clock);
			} else {
				ym_enable_scope(gen->ym, scope, gen->normal_clock);
			}
			psg_enable_scope(gen->psg, scope, gen->normal_clock);
			if (gen->expansion) {
				segacd_context *cd = gen->expansion;
				rf5c164_enable_scope(&cd->pcm, scope);
			}
			if (gen->mars) {
				s32x_enable_scope(gen->mars, scope, gen->normal_clock);
			}
		}
	} else if (debug_view == DEBUG_CD_GRAPHICS && gen->expansion) {
		scd_toggle_graphics_debug(gen->expansion);
	}
#endif
}

static vdp_context *get_vdp(system_header *system)
{
	genesis_context *gen = (genesis_context *)system;
	return gen->vdp;
}

static void *tmss_rom_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_write_16(address, context, value);
	}

	return context;
}

static void *tmss_rom_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_write_8(address, context, value);
	}

	return context;
}

static uint16_t tmss_rom_read_16(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_read_16(address, context);
	}
	return ((uint16_t *)gen->tmss_buffer)[address >> 1];
}

static uint8_t tmss_rom_read_8(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		return gen->tmss_read_8(address, context);
	}
#ifdef BLASTEM_BIG_ENDIAN
	return gen->tmss_buffer[address];
#else
	return gen->tmss_buffer[address ^ 1];
#endif
}

static void *tmss_word_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		address += gen->tmss_write_offset;
		uint16_t *dest = get_native_pointer(address, (void **)m68k->mem_pointers, &m68k->opts->gen);
		*dest = value;
		m68k_handle_code_write(address, m68k);
	}

	return context;
}

static void *tmss_word_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		address += gen->tmss_write_offset;
		uint8_t *dest = get_native_pointer(address & ~1, (void **)m68k->mem_pointers, &m68k->opts->gen);
#ifdef BLASTEM_BIG_ENDIAN
		dest[address & 1] = value;
#else
		dest[address & 1 ^ 1] = value;
#endif
		m68k_handle_code_write(address & ~1, m68k);
	}

	return context;
}

static void *tmss_odd_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->opts->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static void *tmss_odd_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss && (address & 1)) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->opts->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static void *tmss_even_write_16(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->opts->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value >> 8;
	}
	return context;
}

static void *tmss_even_write_8(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (gen->tmss && !(address & 1)) {
		memmap_chunk const *chunk = find_map_chunk(address + gen->tmss_write_offset, &m68k->opts->gen, 0, NULL);
		address >>= 1;
		uint8_t *base = (uint8_t *)m68k->mem_pointers[chunk->ptr_index];
		base[address] = value;
	}
	return context;
}

static genesis_context *shared_init(uint32_t system_opts, rom_info *rom, uint8_t force_region)
{
	static memmap_chunk z80_map[] = {
		{ 0x0000, 0x4000,  0x1FFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
		{ 0x8000, 0x10000, 0x7FFF, .read_8 = z80_read_bank, .write_8 = z80_write_bank},
		{ 0x4000, 0x6000,  0x0003, .read_8 = z80_read_ym, .write_8 = z80_write_ym},
		{ 0x6000, 0x6100,  0xFFFF, .write_8 = z80_write_bank_reg},
		{ 0x7F00, 0x8000,  0x00FF, .read_8 = z80_vdp_port_read, .write_8 = z80_vdp_port_write}
	};

	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}

	genesis_context *gen = calloc(1, sizeof(genesis_context));
	gen->header.set_speed_percent = set_speed_percent;
	gen->header.start_context = start_genesis;
	gen->header.resume_context = resume_genesis;
	gen->header.load_save = load_save;
	gen->header.persist_save = persist_save;
	gen->header.load_state = load_state;
	gen->header.soft_reset = soft_reset;
	gen->header.free_context = free_genesis;
	gen->header.get_open_bus_value = get_open_bus_value;
	gen->header.request_exit = request_exit;
	gen->header.inc_debug_mode = inc_debug_mode;
	gen->header.gamepad_down = gamepad_down;
	gen->header.gamepad_up = gamepad_up;
	gen->header.mouse_down = mouse_down;
	gen->header.mouse_up = mouse_up;
	gen->header.mouse_motion_absolute = mouse_motion_absolute;
	gen->header.mouse_motion_relative = mouse_motion_relative;
	gen->header.keyboard_down = keyboard_down;
	gen->header.keyboard_up = keyboard_up;
	gen->header.config_updated = config_updated;
	gen->header.serialize = serialize;
	gen->header.deserialize = deserialize;
	gen->header.start_vgm_log = start_vgm_log;
	gen->header.stop_vgm_log = stop_vgm_log;
	gen->header.toggle_debug_view = toggle_debug_view;
	gen->header.get_vdp = get_vdp;
	gen->header.type = SYSTEM_GENESIS;
	gen->header.info = *rom;
	set_region(gen, rom, force_region);
	tern_node *model = get_model(config, SYSTEM_GENESIS);
	uint8_t tmss = !strcmp(tern_find_ptr_default(model, "tmss", "off"), "on");
	if (tmss) {
		gen->version_reg |= 1;
	} else {
		gen->vdp_unlocked = 1;
	}

	uint8_t max_vsram = !strcmp(tern_find_ptr_default(model, "vsram", "40"), "64");
	gen->vdp = init_vdp_context(gen->version_reg & 0x40, max_vsram, VDP_GENESIS);
	gen->vdp->system = &gen->header;
	gen->frame_end = vdp_cycles_to_frame_end(gen->vdp);
	char * config_cycles = tern_find_path(config, "clocks\0max_cycles\0", TVAL_PTR).ptrval;
	gen->max_cycles = config_cycles ? atoi(config_cycles) : DEFAULT_SYNC_INTERVAL;
	gen->int_latency_prev1 = MCLKS_PER_68K * 32;
	gen->int_latency_prev2 = MCLKS_PER_68K * 16;

	render_set_video_standard((gen->version_reg & HZ50) ? VID_PAL : VID_NTSC);
	event_system_start(SYSTEM_GENESIS, (gen->version_reg & HZ50) ? VID_PAL : VID_NTSC, rom->name);

	gen->ym = malloc(sizeof(ym2612_context));
	char *fm = tern_find_ptr_default(model, "fm", "discrete 2612");
	if (!strcmp(fm + strlen(fm) -4, "3834")) {
		system_opts |= YM_OPT_3834;
	}
	ym_init(gen->ym, gen->master_clock, MCLKS_PER_YM, system_opts);

	gen->psg = malloc(sizeof(psg_context));
	psg_init(gen->psg, gen->master_clock, MCLKS_PER_PSG);

	z80_map[0].buffer = gen->zram = calloc(1, Z80_RAM_BYTES);
#ifndef NO_Z80
	z80_options *z_opts = malloc(sizeof(z80_options));
	init_z80_opts(z_opts, z80_map, 5, NULL, 0, MCLKS_PER_Z80, 0xFFFF);
	gen->z80 = init_z80_context(z_opts);
#ifndef NEW_Z80
	gen->z80->next_int_pulse = z80_next_int_pulse;
#endif
	z80_assert_reset(gen->z80, 0);
#else
	gen->z80 = calloc(1, sizeof(z80_context));
#endif

	gen->z80->system = gen;
	gen->z80->mem_pointers[0] = gen->zram;
	gen->z80->mem_pointers[1] = gen->z80->mem_pointers[2] = NULL;

	gen->work_ram = calloc(2, RAM_WORDS);
	if (!strcmp("random", tern_find_path_default(config, "system\0ram_init\0", (tern_val){.ptrval = "zero"}, TVAL_PTR).ptrval))
	{
		srand(time(NULL));
		for (int i = 0; i < RAM_WORDS; i++)
		{
			gen->work_ram[i] = rand();
		}
		for (int i = 0; i < Z80_RAM_BYTES; i++)
		{
			gen->zram[i] = rand();
		}
		for (int i = 0; i < VRAM_SIZE; i++)
		{
			gen->vdp->vdpmem[i] = rand();
		}
		for (int i = 0; i < SAT_CACHE_SIZE; i++)
		{
			gen->vdp->sat_cache[i] = rand();
		}
		for (int i = 0; i < CRAM_SIZE; i++)
		{
			write_cram_internal(gen->vdp, i, rand());
		}
		for (int i = 0; i < gen->vdp->vsram_size; i++)
		{
			gen->vdp->vsram[i] = rand();
		}
	}

	return gen;
}

static genesis_context *shared_init_gen(rom_info info, void *lock_on, uint32_t lock_on_size, uint32_t ym_opts, uint8_t force_region)
{
	void *rom = info.rom;
	uint32_t rom_size = info.rom_size;
#ifndef BLASTEM_BIG_ENDIAN
	byteswap_rom(nearest_pow2(rom_size), rom);
	if (lock_on) {
		byteswap_rom(nearest_pow2(lock_on_size), lock_on);
	}
#endif
	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}
	genesis_context *gen = shared_init(ym_opts, &info, force_region);
	gen->z80->mem_pointers[1] = gen->z80->mem_pointers[2] = rom;

	gen->cart = rom;
	gen->lock_on = lock_on;

	setup_io_devices(config, &info, &gen->io);
	gen->header.has_keyboard = io_has_keyboard(&gen->io);
	gen->mapper_type = info.mapper_type;
	gen->save_type = info.save_type;
	if (gen->save_type != SAVE_NONE) {
		gen->save_ram_mask = info.save_mask;
		gen->save_size = info.save_size;
		gen->save_storage = info.save_buffer;
		gen->header.info.save_buffer = info.save_buffer = NULL;
		gen->eeprom_map = info.eeprom_map;
		gen->num_eeprom = info.num_eeprom;
		if (gen->save_type == SAVE_I2C) {
			eeprom_init(&gen->eeprom, gen->save_storage, gen->save_size);
		} else if (gen->save_type == SAVE_NOR) {
			memcpy(&gen->nor, info.nor, sizeof(gen->nor));
			//nor_flash_init(&gen->nor, gen->save_storage, gen->save_size, info.save_page_size, info.save_product_id, info.save_bus);
		}
	} else {
		gen->save_storage = NULL;
	}

	gen->mapper_start_index = info.mapper_start_index;

	tern_node *model = get_model(config, SYSTEM_GENESIS);
	uint8_t tmss = !strcmp(tern_find_ptr_default(model, "tmss", "off"), "on");

	//This must happen before we generate memory access functions in init_m68k_opts
	uint8_t next_ptr_index = 0;
	uint32_t tmss_min_alloc = 16 * 1024;
	for (int i = 0; i < info.map_chunks; i++)
	{
		if (info.map[i].start == 0xE00000) {
			info.map[i].buffer = gen->work_ram;
			if (!tmss) {
				break;
			}
		}
		if (info.map[i].flags & MMAP_PTR_IDX && info.map[i].ptr_index >= next_ptr_index) {
			next_ptr_index = info.map[i].ptr_index + 1;
		}
		if (info.map[i].start < 0x400000 && info.map[i].read_16 != unused_read) {
			uint32_t highest_offset = (info.map[i].end & info.map[i].mask) + 1;
			if (highest_offset > tmss_min_alloc) {
				tmss_min_alloc = highest_offset;
			}
		}
	}
	if (tmss) {
		char *tmss_path = tern_find_path_default(config, "system\0tmss_path\0", (tern_val){.ptrval = "tmss.md"}, TVAL_PTR).ptrval;
		uint8_t *buffer = malloc(tmss_min_alloc);
		uint32_t tmss_size;
		if (is_absolute_path(tmss_path)) {
			FILE *f = fopen(tmss_path, "rb");
			if (!f) {
				fatal_error("Configured to use a model with TMSS, but failed to load the TMSS ROM from %s\n", tmss_path);
			}
			tmss_size = fread(buffer, 1, tmss_min_alloc, f);
			fclose(f);
		} else {
			char *tmp = read_bundled_file(tmss_path, &tmss_size);
			if (!tmp) {
				fatal_error("Configured to use a model with TMSS, but failed to load the TMSS ROM from %s\n", tmss_path);
			}
			memcpy(buffer, tmp, tmss_size);
			free(tmp);
		}
		for (uint32_t padded = nearest_pow2(tmss_size); tmss_size < padded; tmss_size++)
		{
			buffer[tmss_size] = 0xFF;
		}
#ifndef BLASTEM_BIG_ENDIAN
		byteswap_rom(tmss_size, (uint16_t *)buffer);
#endif
		//mirror TMSS ROM until we fill up to tmss_min_alloc
		for (uint32_t dst = tmss_size; dst < tmss_min_alloc; dst += tmss_size)
		{
			memcpy(buffer + dst, buffer, dst + tmss_size > tmss_min_alloc ? tmss_min_alloc - dst : tmss_size);
		}
		//modify mappings for ROM space to point to the TMSS ROM and fixup flags to allow switching back and forth
		//WARNING: This code makes some pretty big assumptions about the kinds of map chunks it will encounter
		for (int i = 0; i < info.map_chunks; i++)
		{
			if (info.map[i].start < 0x400000 && info.map[i].read_16 != unused_read) {
				if (info.map[i].flags == MMAP_READ) {
					//Normal ROM
					info.map[i].flags |= MMAP_PTR_IDX | MMAP_CODE;
					info.map[i].ptr_index = next_ptr_index++;
					if (info.map[i].ptr_index >= NUM_MEM_AREAS) {
						fatal_error("Too many memmap chunks with MMAP_PTR_IDX after TMSS remap\n");
					}
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
				} else if (info.map[i].flags & MMAP_PTR_IDX) {
					//Sega mapper page or multi-game mapper
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (info.map[i].write_16) {
						if (!gen->tmss_write_16) {
							gen->tmss_write_16 = info.map[i].write_16;
							gen->tmss_write_8 = info.map[i].write_8;
							info.map[i].write_16 = tmss_rom_write_16;
							info.map[i].write_8 = tmss_rom_write_8;
						} else if (gen->tmss_write_16 == info.map[i].write_16) {
							info.map[i].write_16 = tmss_rom_write_16;
							info.map[i].write_8 = tmss_rom_write_8;
						} else {
							warning("Chunk starting at %X has a write function, but we've already stored a different one for TMSS remap\n", info.map[i].start);
						}
					}
				} else if ((info.map[i].flags & (MMAP_READ | MMAP_WRITE)) == (MMAP_READ | MMAP_WRITE)) {
					//RAM or SRAM
					info.map[i].flags |= MMAP_PTR_IDX;
					info.map[i].ptr_index = next_ptr_index++;
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (!gen->tmss_write_offset || gen->tmss_write_offset == info.map[i].start) {
						gen->tmss_write_offset = info.map[i].start;
						info.map[i].flags &= ~MMAP_WRITE;
						if (info.map[i].flags & MMAP_ONLY_ODD) {
							info.map[i].write_16 = tmss_odd_write_16;
							info.map[i].write_8 = tmss_odd_write_8;
						} else if (info.map[i].flags & MMAP_ONLY_EVEN) {
							info.map[i].write_16 = tmss_even_write_16;
							info.map[i].write_8 = tmss_even_write_8;
						} else {
							info.map[i].write_16 = tmss_word_write_16;
							info.map[i].write_8 = tmss_word_write_8;
						}
					} else {
						warning("Could not remap writes for chunk starting at %X for TMSS because write_offset is %X\n", info.map[i].start, gen->tmss_write_offset);
					}
				} else if (info.map[i].flags & MMAP_READ_CODE) {
					//NOR flash
					info.map[i].flags |= MMAP_PTR_IDX;
					info.map[i].ptr_index = next_ptr_index++;
					if (info.map[i].ptr_index >= NUM_MEM_AREAS) {
						fatal_error("Too many memmap chunks with MMAP_PTR_IDX after TMSS remap\n");
					}
					gen->tmss_pointers[info.map[i].ptr_index] = info.map[i].buffer;
					info.map[i].buffer = buffer + (info.map[i].start & ~info.map[i].mask & (tmss_size - 1));
					if (!gen->tmss_write_16) {
						gen->tmss_write_16 = info.map[i].write_16;
						gen->tmss_write_8 = info.map[i].write_8;
						gen->tmss_read_16 = info.map[i].read_16;
						gen->tmss_read_8 = info.map[i].read_8;
						info.map[i].write_16 = tmss_rom_write_16;
						info.map[i].write_8 = tmss_rom_write_8;
						info.map[i].read_16 = tmss_rom_read_16;
						info.map[i].read_8 = tmss_rom_read_8;
					} else if (gen->tmss_write_16 == info.map[i].write_16) {
						info.map[i].write_16 = tmss_rom_write_16;
						info.map[i].write_8 = tmss_rom_write_8;
						info.map[i].read_16 = tmss_rom_read_16;
						info.map[i].read_8 = tmss_rom_read_8;
					} else {
						warning("Chunk starting at %X has a write function, but we've already stored a different one for TMSS remap\n", info.map[i].start);
					}
				} else {
					warning("Didn't remap chunk starting at %X for TMSS because it has flags %X\n", info.map[i].start, info.map[i].flags);
				}
			}
		}
		gen->tmss_buffer = buffer;
	}
	memmap_chunk* map = info.map;
	uint32_t map_chunks = info.map_chunks;
	uint8_t cd_attached = current_media()->chain && current_media()->chain->type == MEDIA_CDROM;
	if (info.wants_cd && !cd_attached && !segacd_bios_available(force_region, &info)) {
		//a cartridge that can use a Sega CD but does not need one; without the
		//BIOS it runs as a plain cartridge, the way it does on a console with no
		//Sega CD attached, rather than not at all
		warning("%s can use a Sega CD, but the Sega CD BIOS could not be loaded; running it without one\n", info.name);
		info.wants_cd = gen->header.info.wants_cd = 0;
	}
	if (info.wants_cd || cd_attached) {
		gen->header.type = SYSTEM_SEGACD;
		segacd_context *cd = alloc_configure_segacd((system_media *)current_media(), 0, force_region, &info);
		gen->expansion = cd;
		gen->version_reg &= ~NO_DISK;
		cd->genesis = gen;
		uint32_t cd_chunks;
		memmap_chunk *cd_map = segacd_main_cpu_map(gen->expansion, 1, &cd_chunks);
		map_chunks = cd_chunks + info.map_chunks;
		map = calloc(map_chunks, sizeof(memmap_chunk));
		memcpy(map, info.map, sizeof(memmap_chunk) * (info.map_chunks - 1));
		memcpy(map + info.map_chunks - 1, cd_map, sizeof(memmap_chunk) * cd_chunks);
		memcpy(map + map_chunks - 1, info.map + info.map_chunks - 1, sizeof(memmap_chunk));
		free(info.map);
		int max_ptr_index = -1;
		for (int i = 0; i < info.map_chunks - 1; i++)
		{
			if (map[i].flags & MMAP_PTR_IDX && map[i].ptr_index > max_ptr_index) {
				max_ptr_index = map[i].ptr_index;
			}
		}
		cd->memptr_start_index = max_ptr_index + 1;
		for (int i = info.map_chunks - 1; i < map_chunks - 1; i++)
		{
			if (map[i].flags & MMAP_PTR_IDX) {
				map[i].ptr_index += cd->memptr_start_index;
			}
		}
		cd->base = 0x400000;
		cd->m68k->opts->address_log = (ym_opts & OPT_ADDRESS_LOG) ? fopen("address_sub.log", "w") : NULL;
	}
	info.map = gen->header.info.map = NULL;

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, map_chunks, MCLKS_PER_68K, sync_components, int_ack);
	if (!strcmp(tern_find_ptr_default(model, "tas", "broken"), "broken")) {
		opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	}
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (ym_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;

	//This must happen after the 68K context has been allocated
	for (int i = 0; i < map_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}

	if (gen->mapper_type == MAPPER_SEGA) {
		//initialize bank registers
		for (int i = 1; i < sizeof(gen->bank_regs); i++)
		{
			gen->bank_regs[i] = i;
		}
	} else if (gen->mapper_type == MAPPER_RADICA) {
		radica_reset(gen);
	}
	gen->reset_cycle = CYCLE_NEVER;

	set_audio_config(gen);
	return gen;
}

static memmap_chunk base_map[] = {
	{0xE00000, 0x1000000, 0xFFFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
	{0xC00000, 0xE00000,  0x1FFFFF, .read_16 = (read_16_fun)vdp_port_read,  .write_16 =(write_16_fun)vdp_port_write,
			   .read_8 = (read_8_fun)vdp_port_read_b, .write_8 = (write_8_fun)vdp_port_write_b},
	{0xA00000, 0xA12000,  0x1FFFF,  .read_16 = (read_16_fun)io_read_w, .write_16 = (write_16_fun)io_write_w,
			   .read_8 = (read_8_fun)io_read, .write_8 = (write_8_fun)io_write},
	{0x000000, 0xFFFFFF, 0xFFFFFF, .read_16 = (read_16_fun)unused_read, .write_16 = unused_write,
			   .read_8 = (read_8_fun)unused_read_b, .write_8 = (write_8_fun)unused_write_b}
};
const size_t base_chunks = sizeof(base_map)/sizeof(*base_map);
genesis_context *alloc_config_genesis(void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t ym_opts, uint8_t force_region)
{
	tern_node *rom_db = get_rom_db();
	rom_info info = configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, base_map, base_chunks);
	return shared_init_gen(info, lock_on, lock_on_size, ym_opts, force_region);
}

genesis_context *alloc_gen_cdboot_shared(system_media *media, uint32_t system_opts, uint8_t force_region)
{
	tern_node *rom_db = get_rom_db();
	rom_info info = configure_rom(rom_db, media->buffer, media->size, NULL, 0, base_map, base_chunks);
	if (media->size > 0x20B) {
		//Use a byte in the security code region that's unique across all 3 regions
		//since it's more reliable than the official header field for this
		uint8_t *bytes = media->buffer;
		switch (bytes[0x20B])
		{
		case 0x7A:
			info.regions = REGION_U;
			break;
		case 0xA1:
			info.regions = REGION_J;
			break;
		case 0x64:
			info.regions = REGION_E;
			break;
		}
	}

	segacd_context *cd = alloc_configure_segacd(media, system_opts, force_region, &info);
	genesis_context *gen = shared_init(system_opts, &info, force_region);
	gen->cart = gen->lock_on = NULL;
	gen->save_storage = NULL;
	gen->save_type = SAVE_NONE;
	gen->version_reg &= ~NO_DISK;

	gen->expansion = cd;
	cd->m68k->opts->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address_sub.log", "w") : NULL;
	gen->version_reg &= ~NO_DISK;
	cd->genesis = gen;
	setup_io_devices(config, &info, &gen->io);

	set_audio_config(gen);
	return gen;
}

genesis_context *alloc_config_genesis_cdboot(system_media *media, uint32_t system_opts, uint8_t force_region)
{
	genesis_context *gen = alloc_gen_cdboot_shared(media, system_opts, force_region);
	uint32_t cd_chunks;
	memmap_chunk *cd_map = segacd_main_cpu_map(gen->expansion, 0, &cd_chunks);
	memmap_chunk *map = malloc(sizeof(memmap_chunk) * (cd_chunks + base_chunks));
	memcpy(map, cd_map, sizeof(memmap_chunk) * cd_chunks);
	memcpy(map + cd_chunks, base_map, sizeof(memmap_chunk) * base_chunks);
	map[cd_chunks].buffer = gen->work_ram;
	uint32_t num_chunks = cd_chunks + base_chunks;

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, num_chunks, MCLKS_PER_68K, sync_components, int_ack);
	//TODO: make this configurable
	opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;

	//This must happen after the 68K context has been allocated
	for (int i = 0; i < num_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}
	gen->header.type = SYSTEM_SEGACD;
	return gen;
}

static memmap_chunk base_map_32x[] = {
	{0xE00000, 0x1000000, 0xFFFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
	{0xC00000, 0xE00000,  0x1FFFFF, .read_16 = (read_16_fun)vdp_port_read,  .write_16 =(write_16_fun)vdp_port_write,
			   .read_8 = (read_8_fun)vdp_port_read_b, .write_8 = (write_8_fun)vdp_port_write_b},
	{0x840000, 0x860000, 0xFFFFFF, .read_16 = s32x_fb_read_w, .write_16 = s32x_fb_write_w,
			   .read_8 = s32x_fb_read_b, .write_8 = s32x_fb_write_b},
	{0x860000, 0x880000, 0xFFFFFF, .read_16 = s32x_fb_read_w, .write_16 = s32x_overwrite_write_w,
			   .read_8 = s32x_fb_read_b, .write_8 = s32x_overwrite_write_b},
	{0xA15100, 0xA15400, 0xFFFFFF, .read_16 = s32x_68k_read, .write_16 = s32x_68k_write,
			   .read_8 = s32x_68k_read_b, .write_8 = s32x_68k_write_b},
	{0xA00000, 0xA12000,  0x1FFFF,  .read_16 = (read_16_fun)io_read_w, .write_16 = (write_16_fun)io_write_w,
			   .read_8 = (read_8_fun)io_read, .write_8 = (write_8_fun)io_write},
	{0xA130EC, 0xA130F0, 0x3, .flags = MMAP_READ, .buffer = "AMSR"},
	{0x000000, 0xFFFFFF, 0xFFFFFF, .read_16 = (read_16_fun)unused_read, .write_16 = unused_write,
			   .read_8 = (read_8_fun)unused_read_b, .write_8 = (write_8_fun)unused_write_b}
};
const size_t s32x_base_chunks = sizeof(base_map_32x)/sizeof(*base_map_32x);

genesis_context *alloc_genesis_32x(system_media *media, uint32_t opts, uint8_t force_region)
{
	tern_node *rom_db = get_rom_db();
	rom_info info = configure_rom_32x(rom_db, media->buffer, media->size, 
		media->chain ? media->chain->buffer : NULL, media->chain ? media->chain->size : 0, base_map_32x, s32x_base_chunks
	);
	genesis_context *gen = shared_init_gen(info, media->chain ? media->chain->buffer : NULL, media->chain ? media->chain->size : 0, opts, force_region);
	gen->mars = alloc_32x(media, gen->version_reg & HZ50, 0);
	gen->header.type = SYSTEM_32X;
	gen->vdp->s32x_vid = &gen->mars->video;
	if (gen->vdp->renderer) {
		gen->vdp->renderer->s32x_vid = &gen->mars->video;
	}
	gen->m68k->mem_pointers[2] = gen->m68k->mem_pointers[3] = NULL;
	return gen;
}

genesis_context *alloc_genesis_32x_cdboot(system_media *media, uint32_t system_opts, uint8_t force_region)
{
	genesis_context *gen = alloc_gen_cdboot_shared(media, system_opts, force_region);
	uint32_t cd_chunks;
	memmap_chunk *cd_map = segacd_main_cpu_map(gen->expansion, 0, &cd_chunks);
	uint32_t num_chunks = cd_chunks + s32x_base_chunks;
	memmap_chunk *map = calloc(num_chunks, sizeof(memmap_chunk));
	memcpy(map, cd_map, sizeof(memmap_chunk) * cd_chunks);
	memcpy(map + cd_chunks, base_map_32x, sizeof(memmap_chunk) * s32x_base_chunks);
	map[cd_chunks].buffer = gen->work_ram;

	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, num_chunks, MCLKS_PER_68K, sync_components, int_ack);
	//TODO: make this configurable
	opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (system_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;

	gen->mars = alloc_32x(media, gen->version_reg & HZ50, 1);
	gen->vdp->s32x_vid = &gen->mars->video;
	
	//This must happen after the 68K context has been allocated
	for (int i = 0; i < num_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}
	gen->header.type = SYSTEM_32XCD;
	return gen;
}

static memmap_chunk pico_base_map[] = {
	{0xE00000, 0x1000000, 0xFFFF, .flags = MMAP_READ | MMAP_WRITE | MMAP_CODE},
	{0xC00000, 0xE00000,  0x1FFFFF, .read_16 = (read_16_fun)vdp_port_read,  .write_16 =(write_16_fun)vdp_port_write,
			   .read_8 = (read_8_fun)vdp_port_read_b, .write_8 = (write_8_fun)vdp_port_write_b},
	{0x800000, 0x900000,  0xFFFFFF, .read_16 = pico_io_read_w, .write_16 = pico_io_write_w,
			   .read_8 = pico_io_read, .write_8 = pico_io_write},
	{0xB00000, 0xC00000,  0xFFFFFF, .read_16 = copera_io_read_w, .write_16 = copera_io_write_w, 
	           .read_8 = copera_io_read, .write_8 = copera_io_write}
};
const size_t pico_base_chunks = sizeof(pico_base_map)/sizeof(*pico_base_map);

genesis_context* alloc_config_pico(void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t ym_opts, uint8_t force_region, system_type stype)
{
	tern_node *rom_db = get_rom_db();
	uint32_t chunks = pico_base_chunks;
	if (stype == SYSTEM_PICO) {
		chunks--;
	}
	rom_info info = configure_rom(rom_db, rom, rom_size, lock_on, lock_on_size, pico_base_map, chunks);
	rom = info.rom;
	rom_size = info.rom_size;
#ifndef BLASTEM_BIG_ENDIAN
	byteswap_rom(nearest_pow2(rom_size), rom);
	if (lock_on) {
		byteswap_rom(nearest_pow2(lock_on_size), lock_on);
	}
#endif
	char *m68k_divider = tern_find_path(config, "clocks\0m68k_divider\0", TVAL_PTR).ptrval;
	if (!m68k_divider) {
		m68k_divider = "7";
	}
	MCLKS_PER_68K = atoi(m68k_divider);
	if (!MCLKS_PER_68K) {
		MCLKS_PER_68K = 7;
	}
	genesis_context *gen = calloc(1, sizeof(genesis_context));
	gen->header.set_speed_percent = set_speed_percent;
	gen->header.start_context = start_genesis;
	gen->header.resume_context = resume_genesis;
	gen->header.load_save = load_save;
	gen->header.persist_save = persist_save;
	gen->header.load_state = load_state;
	gen->header.soft_reset = soft_reset;
	gen->header.free_context = free_genesis;
	gen->header.get_open_bus_value = get_open_bus_value;
	gen->header.request_exit = request_exit;
	gen->header.inc_debug_mode = inc_debug_mode;
	gen->header.gamepad_down = gamepad_down_pico;
	gen->header.gamepad_up = gamepad_up_pico;
	gen->header.mouse_down = mouse_down_pico;
	gen->header.mouse_up = mouse_up_pico;
	gen->header.mouse_motion_absolute = mouse_motion_absolute_pico;
	gen->header.mouse_motion_relative = mouse_motion_relative_pico;
	gen->header.keyboard_down = keyboard_down_pico;
	gen->header.keyboard_up = keyboard_up_pico;
	gen->header.config_updated = config_updated;
	gen->header.serialize = serialize;
	gen->header.deserialize = deserialize;
	gen->header.start_vgm_log = start_vgm_log;
	gen->header.stop_vgm_log = stop_vgm_log;
	gen->header.toggle_debug_view = toggle_debug_view;
	gen->header.get_vdp = get_vdp;
	gen->header.type = stype;
	gen->header.info = info;
	set_region(gen, &info, force_region);
	gen->vdp_unlocked = 1;
	gen->pico_button_state = 0xFF;
	
	gen->vdp = init_vdp_context((gen->version_reg & 0x60) == 0x20, 40, VDP_GENESIS);
	gen->vdp->system = &gen->header;
	gen->frame_end = vdp_cycles_to_frame_end(gen->vdp);
	char * config_cycles = tern_find_path(config, "clocks\0max_cycles\0", TVAL_PTR).ptrval;
	gen->max_cycles = config_cycles ? atoi(config_cycles) : DEFAULT_SYNC_INTERVAL;
	gen->int_latency_prev1 = MCLKS_PER_68K * 32;
	gen->int_latency_prev2 = MCLKS_PER_68K * 16;
	
	render_set_video_standard((gen->version_reg & 0x60) == 0x20 ? VID_PAL : VID_NTSC);
	
	gen->psg = calloc(1, sizeof(psg_context));
	psg_init(gen->psg, gen->master_clock, MCLKS_PER_PSG);
	
	gen->adpcm = calloc(1, sizeof(pico_pcm));
	pico_pcm_init(gen->adpcm, gen->master_clock, 42);
	
	if (stype == SYSTEM_COPERA) {
		gen->ymz = calloc(1, sizeof(*gen->ymz));
		//This divider is just a guess, PCB diagram in MAME shows no other crystal
		//Datasheet says the typical clock is 16.9344 MHz
		//Master clock / 3 is 17.897725 MHz which is reasonably close
		//FIXME: Copera does actually have a separate crystal for these
		ymz263b_init(gen->ymz, gen->master_clock, 3);
		gen->opl = calloc(1, sizeof(*gen->opl));
		ymf262_init(gen->opl, gen->master_clock, 3, ym_opts);
	}
	
	gen->work_ram = calloc(2, RAM_WORDS);
	if (!strcmp("random", tern_find_path_default(config, "system\0ram_init\0", (tern_val){.ptrval = "zero"}, TVAL_PTR).ptrval))
	{
		srand(time(NULL));
		for (int i = 0; i < RAM_WORDS; i++)
		{
			gen->work_ram[i] = rand();
		}
		for (int i = 0; i < VRAM_SIZE; i++)
		{
			gen->vdp->vdpmem[i] = rand();
		}
		for (int i = 0; i < SAT_CACHE_SIZE; i++)
		{
			gen->vdp->sat_cache[i] = rand();
		}
		for (int i = 0; i < CRAM_SIZE; i++)
		{
			write_cram_internal(gen->vdp, i, rand());
		}
		for (int i = 0; i < gen->vdp->vsram_size; i++)
		{
			gen->vdp->vsram[i] = rand();
		}
	}
	gen->cart = rom;
	gen->lock_on = lock_on;
	gen->header.has_keyboard = 0; //TODO: Keyboard Pico emulation
	gen->mapper_type = info.mapper_type;
	gen->save_type = info.save_type;
	if (gen->save_type != SAVE_NONE) {
		gen->save_ram_mask = info.save_mask;
		gen->save_size = info.save_size;
		gen->save_storage = info.save_buffer;
		gen->header.info.save_buffer = info.save_buffer = NULL;
		gen->eeprom_map = info.eeprom_map;
		gen->num_eeprom = info.num_eeprom;
		if (gen->save_type == SAVE_I2C) {
			eeprom_init(&gen->eeprom, gen->save_storage, gen->save_size);
		} else if (gen->save_type == SAVE_NOR) {
			memcpy(&gen->nor, info.nor, sizeof(gen->nor));
			//nor_flash_init(&gen->nor, gen->save_storage, gen->save_size, info.save_page_size, info.save_product_id, info.save_bus);
		}
	} else {
		gen->save_storage = NULL;
	}
	
	gen->mapper_start_index = info.mapper_start_index;
	//This must happen before we generate memory access functions in init_m68k_opts
	for (int i = 0; i < info.map_chunks; i++)
	{
		if (info.map[i].start == 0xE00000) {
			info.map[i].buffer = gen->work_ram;
			break;
		}
	}
	
	memmap_chunk* map = info.map;
	uint32_t map_chunks = info.map_chunks;
	info.map = gen->header.info.map = NULL;
	
	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, map, map_chunks, MCLKS_PER_68K, sync_components_pico, int_ack);
	//TODO: Pico model selection
	//if (!strcmp(tern_find_ptr_default(model, "tas", "broken"), "broken")) {
		opts->gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	//}
	gen->m68k = init_68k_context(opts, NULL);
	gen->m68k->system = gen;
	opts->address_log = (ym_opts & OPT_ADDRESS_LOG) ? fopen("address.log", "w") : NULL;
	
	//This must happen after the 68K context has been allocated
	for (int i = 0; i < map_chunks; i++)
	{
		if (map[i].flags & MMAP_PTR_IDX) {
			gen->m68k->mem_pointers[map[i].ptr_index] = map[i].buffer;
		}
	}

	if (gen->mapper_type == MAPPER_SEGA) {
		//initialize bank registers
		for (int i = 1; i < sizeof(gen->bank_regs); i++)
		{
			gen->bank_regs[i] = i;
		}
	} else if (gen->mapper_type == MAPPER_RADICA) {
		radica_reset(gen);
	}
	gen->reset_cycle = CYCLE_NEVER;

	set_audio_config(gen);
	bindings_set_mouse_mode(MOUSE_ABSOLUTE);
	memset(gen->pico_story_pages, 0xFF, sizeof(gen->pico_story_pages));
#ifndef IS_LIB
	gen->pico_story_window = render_create_window("Pico Storybook & Pad", 640, 640, NULL);
	char *manifest_name = alloc_concat(current_media()->name, ".manifest");
	char *manifest_str = load_media_subfile(current_media(), manifest_name, NULL);
	tern_node *manifest = NULL;
	if (manifest_str) {
		manifest = parse_config(manifest_str);
		if (!manifest) {
			printf("Failed to parse manifest %s\n", manifest_name);
		}
	} else {
		printf("Manifest file %s not found\n", manifest_name);
	}
	
	tern_node *pages = tern_find_node(manifest, "pages");
	if (!pages && manifest) {
		printf("No pages key in %s\n", manifest_name);
	}
	char numkey[18];
	for (int i = 0; i < 7; i++) {
		uint8_t *img_data;
		uint32_t img_size;
		if (pages) {
			sprintf(numkey, "%d", i);
			char *page_path = tern_find_ptr(pages, numkey);
			if (page_path) {
				printf("page %d: %s\n", i, page_path);
			} else {
				continue;
			}
			img_data = load_media_subfile(current_media(), page_path, &img_size);
			if (!img_data) {
				printf("Failed to load image file for page %d from %s\n", i, page_path);
				continue;
			}
		} else {
			sprintf(numkey, "_%d.png", i);
			char *img_path = alloc_concat(current_media()->name, numkey);
			img_data = load_media_subfile(current_media(), img_path, &img_size);
			if (!img_data) {
				sprintf(numkey, "_%d.PNG", i);
				char *img_path_loud = alloc_concat(current_media()->name, numkey);
				img_data = load_media_subfile(current_media(), img_path_loud, &img_size);
				if (!img_data) {
					printf("Failed to load image file for page %d from %s or %s\n", i, img_path, img_path_loud);
					free(img_path);
					free(img_path_loud);
					continue;
				}
				free(img_path_loud);
			}
			free(img_path);
		}
		
		gen->pico_story_pages[i] = render_static_image(gen->pico_story_window, img_data, img_size);
		if (gen->pico_story_pages[i] == 0xFF) {
			printf("Failed to decode image for page %d\n", i);
		}
		free(img_data);
	}
	
	free(manifest_name);
	tern_free(manifest);
	pico_update_page(gen);
#endif
	return gen;
}
