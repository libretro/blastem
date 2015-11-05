/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "68kinst.h"
#include "m68k_core.h"
#include "z80_to_x86.h"
#include "mem.h"
#include "vdp.h"
#include "render.h"
#include "blastem.h"
#include "gdb_remote.h"
#include "gst.h"
#include "util.h"
#include "romdb.h"
#include "terminal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define BLASTEM_VERSION "0.3.X"

#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

#define MCLKS_PER_68K 7
#define MCLKS_PER_YM  MCLKS_PER_68K
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)
#define DEFAULT_SYNC_INTERVAL MCLKS_LINE

//TODO: Figure out the exact value for this
#define LINES_NTSC 262
#define LINES_PAL 312

#define MAX_SOUND_CYCLES 100000

#ifdef __ANDROID__
#define FULLSCREEN_DEFAULT 1
#else
#define FULLSCREEN_DEFAULT 0
#endif

uint16_t *cart;
uint16_t ram[RAM_WORDS];
uint8_t z80_ram[Z80_RAM_BYTES];

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
int frame_limit = 0;

tern_node * config;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

int load_smd_rom(long filesize, FILE * f)
{
	uint8_t block[SMD_BLOCK_SIZE];
	filesize -= SMD_HEADER_SIZE;
	fseek(f, SMD_HEADER_SIZE, SEEK_SET);

	uint16_t * dst = cart;
	int rom_size = filesize;
	while (filesize > 0) {
		fread(block, 1, SMD_BLOCK_SIZE, f);
		for (uint8_t *low = block, *high = (block+SMD_BLOCK_SIZE/2), *end = block+SMD_BLOCK_SIZE; high < end; high++, low++) {
			*(dst++) = *low << 8 | *high;
		}
		filesize -= SMD_BLOCK_SIZE;
	}
	return filesize;
}

void byteswap_rom(int filesize)
{
	for(unsigned short * cur = cart; cur - cart < filesize/2; ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
}

int load_rom(char * filename)
{
	uint8_t header[10];
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	if (sizeof(header) != fread(header, 1, sizeof(header), f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (header[1] == SMD_MAGIC1 && header[8] == SMD_MAGIC2 && header[9] == SMD_MAGIC3) {
		int i;
		for (i = 3; i < 8; i++) {
			if (header[i] != 0) {
				break;
			}
		}
		if (i == 8) {
			if (header[2]) {
				fatal_error("%s is a split SMD ROM which is not currently supported", filename);
			}
			return load_smd_rom(filesize, f);
		}
	}
	cart = malloc(nearest_pow2(filesize));
	if (filesize != fread(cart, 1, filesize, f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	fclose(f);
	return filesize;
}

uint16_t read_dma_value(uint32_t address)
{
	//addresses here are word addresses (i.e. bit 0 corresponds to A1), so no need to do div by 2
	if (address < 0x200000) {
		return cart[address];
	} else if(address >= 0x700000) {
		return ram[address & 0x7FFF];
	}
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	return 0;
}

void adjust_int_cycle(m68k_context * context, vdp_context * v_context)
{
	//static int old_int_cycle = CYCLE_NEVER;
	genesis_context *gen = context->system;
	if (context->sync_cycle - context->current_cycle > gen->max_cycles) {
		context->sync_cycle = context->current_cycle + gen->max_cycles;
	}
	context->int_cycle = CYCLE_NEVER;
	if ((context->status & 0x7) < 6) {
		uint32_t next_vint = vdp_next_vint(v_context);
		if (next_vint != CYCLE_NEVER) {
			context->int_cycle = next_vint;
			context->int_num = 6;
		}
		if ((context->status & 0x7) < 4) {
			uint32_t next_hint = vdp_next_hint(v_context);
			if (next_hint != CYCLE_NEVER) {
				if (next_hint < context->int_cycle) {
					context->int_cycle = next_hint;
					context->int_num = 4;

				}
			}
		}
	}
	if (context->int_cycle > context->current_cycle) {
		context->int_pending = 0;
	}
	/*if (context->int_cycle != old_int_cycle) {
		printf("int cycle changed to: %d, level: %d @ %d(%d), frame: %d, vcounter: %d, hslot: %d, mask: %d, hint_counter: %d\n", context->int_cycle, context->int_num, v_context->cycles, context->current_cycle, v_context->frame, v_context->vcounter, v_context->hslot, context->status & 0x7, v_context->hint_counter);
		old_int_cycle = context->int_cycle;
	}*/

	context->target_cycle = context->int_cycle < context->sync_cycle ? context->int_cycle : context->sync_cycle;
	/*printf("Cyc: %d, Trgt: %d, Int Cyc: %d, Int: %d, Mask: %X, V: %d, H: %d, HICount: %d, HReg: %d, Line: %d\n",
		context->current_cycle, context->target_cycle, context->int_cycle, context->int_num, (context->status & 0x7),
		v_context->regs[REG_MODE_2] & 0x20, v_context->regs[REG_MODE_1] & 0x10, v_context->hint_counter, v_context->regs[REG_HINT], v_context->cycles / MCLKS_LINE);*/
}

int break_on_sync = 0;
int save_state = 0;

//#define DO_DEBUG_PRINT
#ifdef DO_DEBUG_PRINT
#define dprintf printf
#define dputs puts
#else
#define dprintf
#define dputs
#endif

#define Z80_VINT_DURATION 128

void z80_next_int_pulse(z80_context * z_context)
{
		genesis_context * gen = z_context->system;
	z_context->int_pulse_start = vdp_next_vint_z80(gen->vdp);
	z_context->int_pulse_end = z_context->int_pulse_start + Z80_VINT_DURATION * MCLKS_PER_Z80;
			}

void sync_z80(z80_context * z_context, uint32_t mclks)
{
#ifndef NO_Z80
	if (z80_enabled) {
		z80_run(z_context, mclks);
	} else
#endif
	{
		z_context->current_cycle = mclks;
	}
}

void sync_sound(genesis_context * gen, uint32_t target)
{
	//printf("YM | Cycle: %d, bpos: %d, PSG | Cycle: %d, bpos: %d\n", gen->ym->current_cycle, gen->ym->buffer_pos, gen->psg->cycles, gen->psg->buffer_pos * 2);
	while (target > gen->psg->cycles && target - gen->psg->cycles > MAX_SOUND_CYCLES) {
		uint32_t cur_target = gen->psg->cycles + MAX_SOUND_CYCLES;
		//printf("Running PSG to cycle %d\n", cur_target);
		psg_run(gen->psg, cur_target);
		//printf("Running YM-2612 to cycle %d\n", cur_target);
		ym_run(gen->ym, cur_target);
	}
	psg_run(gen->psg, target);
	ym_run(gen->ym, target);

	//printf("Target: %d, YM bufferpos: %d, PSG bufferpos: %d\n", target, gen->ym->buffer_pos, gen->psg->buffer_pos * 2);
}

uint32_t last_frame_num;
m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	genesis_context * gen = context->system;
	vdp_context * v_context = gen->vdp;
	z80_context * z_context = gen->z80;
	uint32_t mclks = context->current_cycle;
	sync_z80(z_context, mclks);
	sync_sound(gen, mclks);
	vdp_run_context(v_context, mclks);
	if (v_context->frame != last_frame_num) {
		//printf("reached frame end %d | MCLK Cycles: %d, Target: %d, VDP cycles: %d, vcounter: %d, hslot: %d\n", last_frame_num, mclks, gen->frame_end, v_context->cycles, v_context->vcounter, v_context->hslot);
		last_frame_num = v_context->frame;

		if (!headless) {
			break_on_sync |= wait_render_frame(v_context, frame_limit);
		} else if(exit_after){
			--exit_after;
			if (!exit_after) {
				exit(0);
			}
		}

		vdp_adjust_cycles(v_context, mclks);
		io_adjust_cycles(gen->ports, context->current_cycle, mclks);
		io_adjust_cycles(gen->ports+1, context->current_cycle, mclks);
		io_adjust_cycles(gen->ports+2, context->current_cycle, mclks);
		context->current_cycle -= mclks;
		z80_adjust_cycles(z_context, mclks);
		gen->ym->current_cycle -= mclks;
		gen->psg->cycles -= mclks;
		if (gen->ym->write_cycle != CYCLE_NEVER) {
			gen->ym->write_cycle = gen->ym->write_cycle >= mclks ? gen->ym->write_cycle - mclks : 0;
		}
	}
	gen->frame_end = vdp_cycles_to_frame_end(v_context);
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
	if (context->int_ack) {
		//printf("acknowledging %d @ %d:%d, vcounter: %d, hslot: %d\n", context->int_ack, context->current_cycle, v_context->cycles, v_context->vcounter, v_context->hslot);
		vdp_int_ack(v_context, context->int_ack);
		context->int_ack = 0;
	}
	if (!address && (break_on_sync || save_state)) {
		context->sync_cycle = context->current_cycle + 1;
	}
	adjust_int_cycle(context, v_context);
	if (address) {
		if (break_on_sync) {
			break_on_sync = 0;
			debugger(context, address);
		}
		if (save_state && (z_context->pc || (!z_context->reset && !z_context->busreq))) {
			save_state = 0;
			//advance Z80 core to the start of an instruction
			while (!z_context->pc)
			{
				sync_z80(z_context, z_context->current_cycle + MCLKS_PER_Z80);
			}
			save_gst(gen, "savestate.gst", address);
			puts("Saved state to savestate.gst");
		} else if(save_state) {
			context->sync_cycle = context->current_cycle + 1;
		}
	}
	return context;
}

m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to write to address %X\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->current_cycle);
	sync_components(context, 0);
	vdp_context * v_context = context->video_context;
	genesis_context * gen = context->system;
	if (vdp_port < 0x10) {
		int blocked;
		uint32_t before_cycle = v_context->cycles;
		if (vdp_port < 4) {

			while (vdp_data_port_write(v_context, value) < 0) {
				while(v_context->flags & FLAG_DMA_RUN) {
					vdp_run_dma_done(v_context, gen->frame_end);
					if (v_context->cycles >= gen->frame_end) {
						context->current_cycle = v_context->cycles;
						gen->bus_busy = 1;
						sync_components(context, 0);
						gen->bus_busy = 0;
					}
				}
				//context->current_cycle = v_context->cycles;
			}
		} else if(vdp_port < 8) {
			blocked = vdp_control_port_write(v_context, value);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						vdp_run_dma_done(v_context, gen->frame_end);
						if (v_context->cycles >= gen->frame_end) {
							context->current_cycle = v_context->cycles;
							gen->bus_busy = 1;
							sync_components(context, 0);
							gen->bus_busy = 0;
						}
					}
					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value);
					} else {
						blocked = 0;
					}
				}
			} else {
				context->sync_cycle = gen->frame_end = vdp_cycles_to_frame_end(v_context);
				//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
				adjust_int_cycle(context, v_context);
			}
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
		if (v_context->cycles != before_cycle) {
			//printf("68K paused for %d (%d) cycles at cycle %d (%d) for write\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
			context->current_cycle = v_context->cycles;
			//Lock the Z80 out of the bus until the VDP access is complete
			gen->bus_busy = 1;
			sync_z80(gen->z80, v_context->cycles);
			gen->bus_busy = 0;
		}
	} else if (vdp_port < 0x18) {
		psg_write(gen->psg, value);
	} else {
		//TODO: Implement undocumented test register(s)
	}
	return context;
}

m68k_context * vdp_port_write_b(uint32_t vdp_port, m68k_context * context, uint8_t value)
{
	return vdp_port_write(vdp_port, context, vdp_port < 0x10 ? value | value << 8 : ((vdp_port & 1) ? value : 0));
}

void * z80_vdp_port_write(uint32_t vdp_port, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	vdp_port &= 0xFF;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to write to Z80 address %X\n", 0x7F00 | vdp_port);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->current_cycle);
		if (vdp_port < 4) {
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_control_port_write(gen->vdp, value << 8 | value);
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
	} else if (vdp_port < 0x18) {
		sync_sound(gen, context->current_cycle);
		psg_write(gen->psg, value);
	} else {
		vdp_test_port_write(gen->vdp, value);
	}
	return context;
}

uint16_t vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	if (vdp_port & 0x2700E0) {
		fatal_error("machine freeze due to read from address %X\n", 0xC00000 | vdp_port);
	}
	vdp_port &= 0x1F;
	uint16_t value;
	sync_components(context, 0);
	vdp_context * v_context = context->video_context;
	uint32_t before_cycle = v_context->cycles;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			value = vdp_data_port_read(v_context);
		} else if(vdp_port < 8) {
			value = vdp_control_port_read(v_context);
		} else {
			value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", value, v_context->cycles);
		}
	} else if (vdp_port < 0x18){
		fatal_error("Illegal read from PSG  port %X\n", vdp_port);
	} else {
		value = vdp_test_port_read(v_context);
	}
	if (v_context->cycles != before_cycle) {
		//printf("68K paused for %d (%d) cycles at cycle %d (%d) for read\n", v_context->cycles - context->current_cycle, v_context->cycles - before_cycle, context->current_cycle, before_cycle);
		context->current_cycle = v_context->cycles;
		//Lock the Z80 out of the bus until the VDP access is complete
		genesis_context *gen = context->system;
		gen->bus_busy = 1;
		sync_z80(gen->z80, v_context->cycles);
		gen->bus_busy = 0;
	}
	return value;
}

uint8_t vdp_port_read_b(uint32_t vdp_port, m68k_context * context)
{
	uint16_t value = vdp_port_read(vdp_port, context);
	if (vdp_port & 1) {
		return value;
	} else {
		return value >> 8;
	}
}

uint8_t z80_vdp_port_read(uint32_t vdp_port, void * vcontext)
{
	z80_context * context = vcontext;
	if (vdp_port & 0xE0) {
		fatal_error("machine freeze due to read from Z80 address %X\n", 0x7F00 | vdp_port);
	}
	genesis_context * gen = context->system;
	//VDP access goes over the 68K bus like a bank area access
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;


	vdp_port &= 0x1F;
	uint16_t ret;
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->current_cycle);
		if (vdp_port < 4) {
			ret = vdp_data_port_read(gen->vdp);
		} else if (vdp_port < 8) {
			ret = vdp_control_port_read(gen->vdp);
		} else {
			fatal_error("Illegal write to HV Counter port %X\n", vdp_port);
		}
	} else {
		//TODO: Figure out the correct value today
		ret = 0xFFFF;
	}
	return vdp_port & 1 ? ret : ret >> 8;
}

uint32_t zram_counter = 0;

m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				z80_ram[location & 0x1FFF] = value;
#ifndef NO_Z80
				z80_handle_code_write(location & 0x1FFF, gen->z80);
#endif
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				if (location & 1) {
					ym_data_write(gen->ym, value);
				} else if(location & 2) {
					ym_address_write_part2(gen->ym, value);
				} else {
					ym_address_write_part1(gen->ym, value);
				}
			} else if (location == 0x6000) {
				gen->z80->bank_reg = (gen->z80->bank_reg >> 1 | value << 8) & 0x1FF;
				if (gen->z80->bank_reg < 0x80) {
					gen->z80->mem_pointers[1] = (gen->z80->bank_reg << 15) + ((char *)gen->z80->mem_pointers[2]);
				} else {
					gen->z80->mem_pointers[1] = NULL;
				}
			} else {
				fatal_error("68K write to unhandled Z80 address %X\n", location);
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(gen->ports, value, context->current_cycle);
				break;
			case 0x2:
				io_data_write(gen->ports+1, value, context->current_cycle);
				break;
			case 0x3:
				io_data_write(gen->ports+2, value, context->current_cycle);
				break;
			case 0x4:
				gen->ports[0].control = value;
				break;
			case 0x5:
				gen->ports[1].control = value;
				break;
			case 0x6:
				gen->ports[2].control = value;
				break;
			}
		} else {
			if (location == 0x1100) {
				if (value & 1) {
					dputs("bus requesting Z80");
					if (z80_enabled) {
						z80_assert_busreq(gen->z80, context->current_cycle);
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
						z80_clear_busreq(gen->z80, context->current_cycle);
					} else {
						gen->z80->busack = 0;
					}
				}
			} else if (location == 0x1200) {
				sync_z80(gen->z80, context->current_cycle);
				if (value & 1) {
					if (z80_enabled) {
						z80_clear_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 0;
					}
				} else {
					if (z80_enabled) {
						z80_assert_reset(gen->z80, context->current_cycle);
					} else {
						gen->z80->reset = 1;
					}
				}
			}
		}
	}
	return context;
}

m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	if (location < 0x10000 || (location & 0x1FFF) >= 0x100) {
		return io_write(location, context, value >> 8);
	} else {
		return io_write(location, context, value);
	}
}

#define USA 0x80
#define JAP 0x00
#define EUR 0xC0
#define NO_DISK 0x20
uint8_t version_reg = NO_DISK | USA;

uint8_t io_read(uint32_t location, m68k_context * context)
{
	uint8_t value;
	genesis_context *gen = context->system;
	if (location < 0x10000) {
		//Access to Z80 memory incurs a one 68K cycle wait state
		context->current_cycle += MCLKS_PER_68K;
		if (!z80_enabled || z80_get_busack(gen->z80, context->current_cycle)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				value = z80_ram[location & 0x1FFF];
			} else if (location < 0x6000) {
				sync_sound(gen, context->current_cycle);
				value = ym_read_status(gen->ym);
			} else {
				value = 0xFF;
			}
		} else {
			value = 0xFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				value = version_reg;
				break;
			case 0x1:
				value = io_data_read(gen->ports, context->current_cycle);
				break;
			case 0x2:
				value = io_data_read(gen->ports+1, context->current_cycle);
				break;
			case 0x3:
				value = io_data_read(gen->ports+2, context->current_cycle);
				break;
			case 0x4:
				value = gen->ports[0].control;
				break;
			case 0x5:
				value = gen->ports[1].control;
				break;
			case 0x6:
				value = gen->ports[2].control;
				break;
			default:
				value = 0xFF;
			}
		} else {
			if (location == 0x1100) {
				value = z80_enabled ? !z80_get_busack(gen->z80, context->current_cycle) : !gen->z80->busack;
				//TODO: actual pre-fetch emulation
				value |= 0x4E;
				dprintf("Byte read of BUSREQ returned %d @ %d (reset: %d)\n", value, context->current_cycle, gen->z80->reset);
			} else if (location == 0x1200) {
				value = !gen->z80->reset;
			} else {
				value = 0xFF;
				printf("Byte read of unknown IO location: %X\n", location);
			}
		}
	}
	return value;
}

uint16_t io_read_w(uint32_t location, m68k_context * context)
{
	uint16_t value = io_read(location, context);
	if (location < 0x10000 || (location & 0x1FFF) < 0x100) {
		value = value | (value << 8);
	} else {
		value <<= 8;
		//TODO: actual pre-fetch emulation
		value |= 0x73;
	}
	return value;
}

void * z80_write_ym(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->current_cycle);
	if (location & 1) {
		ym_data_write(gen->ym, value);
	} else if (location & 2) {
		ym_address_write_part2(gen->ym, value);
	} else {
		ym_address_write_part1(gen->ym, value);
	}
	return context;
}

uint8_t z80_read_ym(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context * gen = context->system;
	sync_sound(gen, context->current_cycle);
	return ym_read_status(gen->ym);
}

uint8_t z80_read_bank(uint32_t location, void * vcontext)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->current_cycle = context->sync_cycle;
	}
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	if (context->mem_pointers[1]) {
		return context->mem_pointers[1][location ^ 1];
	}
	uint32_t address = context->bank_reg << 15 | location;
	if (address >= 0xC00000 && address < 0xE00000) {
		return z80_vdp_port_read(location & 0xFF, context);
	} else {
		fprintf(stderr, "Unhandled read by Z80 from address %X through banked memory area (%X)\n", address, context->bank_reg << 15);
	}
	return 0;
}

void *z80_write_bank(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;
	genesis_context *gen = context->system;
	if (gen->bus_busy) {
		context->current_cycle = context->sync_cycle;
	}
	//typical delay from bus arbitration
	context->current_cycle += 3 * MCLKS_PER_Z80;
	//TODO: add cycle for an access right after a previous one
	//TODO: Below cycle time is an estimate based on the time between 68K !BG goes low and Z80 !MREQ goes high
	//      Needs a new logic analyzer capture to get the actual delay on the 68K side
	gen->m68k->current_cycle += 8 * MCLKS_PER_68K;

	location &= 0x7FFF;
	uint32_t address = context->bank_reg << 15 | location;
	if (address >= 0xE00000) {
		address &= 0xFFFF;
		((uint8_t *)ram)[address ^ 1] = value;
	} else if (address >= 0xC00000) {
		z80_vdp_port_write(location & 0xFF, context, value);
	} else {
		fprintf(stderr, "Unhandled write by Z80 to address %X through banked memory area\n", address);
	}
	return context;
}

void *z80_write_bank_reg(uint32_t location, void * vcontext, uint8_t value)
{
	z80_context * context = vcontext;

	context->bank_reg = (context->bank_reg >> 1 | value << 8) & 0x1FF;
	if (context->bank_reg < 0x100) {
		genesis_context *gen = context->system;
		context->mem_pointers[1] = get_native_pointer(context->bank_reg << 15, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen);
	} else {
		context->mem_pointers[1] = NULL;
	}

	return context;
}

void set_speed_percent(genesis_context * context, uint32_t percent)
{
	uint32_t old_clock = context->master_clock;
	context->master_clock = ((uint64_t)context->normal_clock * (uint64_t)percent) / 100;
	while (context->ym->current_cycle != context->psg->cycles) {
		sync_sound(context, context->psg->cycles + MCLKS_PER_PSG);
}
	ym_adjust_master_clock(context->ym, context->master_clock);
	psg_adjust_master_clock(context->psg, context->master_clock);
}

const memmap_chunk base_map[] = {
		{0xE00000, 0x1000000, 0xFFFF,   0, MMAP_READ | MMAP_WRITE | MMAP_CODE, ram,
		           NULL,          NULL,         NULL,            NULL},
		{0xC00000, 0xE00000,  0x1FFFFF, 0, 0,                                  NULL,
		           (read_16_fun)vdp_port_read,  (write_16_fun)vdp_port_write,
		           (read_8_fun)vdp_port_read_b, (write_8_fun)vdp_port_write_b},
		{0xA00000, 0xA12000,  0x1FFFF,  0, 0,                                  NULL,
		           (read_16_fun)io_read_w,      (write_16_fun)io_write_w,
		           (read_8_fun)io_read,         (write_8_fun)io_write}
	};

char * save_filename;
genesis_context * genesis;
void persist_save()
{
	FILE * f = fopen(save_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open %s file %s for writing\n", genesis->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
		return;
	}
	fwrite(genesis->save_storage, 1, genesis->save_size, f);
	fclose(f);
	printf("Saved %s to %s\n", genesis->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
}

void init_run_cpu(genesis_context * gen, rom_info *rom, FILE * address_log, char * statefile, uint8_t * debugger)
{
	m68k_options opts;

	gen->save_type = rom->save_type;
	if (gen->save_type != SAVE_NONE) {
		gen->save_ram_mask = rom->save_mask;
		gen->save_size = rom->save_size;
		gen->save_storage = rom->save_buffer;
		gen->eeprom_map = rom->eeprom_map;
		gen->num_eeprom = rom->num_eeprom;
		FILE * f = fopen(save_filename, "rb");
		if (f) {
			uint32_t read = fread(gen->save_storage, 1, rom->save_size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded %s from %s\n", rom->save_type == SAVE_I2C ? "EEPROM" : "SRAM", save_filename);
			}
		}
		atexit(persist_save);
		if (gen->save_type == SAVE_I2C) {
			eeprom_init(&gen->eeprom, gen->save_storage, gen->save_size);
		}
	} else {
		gen->save_storage = NULL;
	}

	init_m68k_opts(&opts, rom->map, rom->map_chunks, MCLKS_PER_68K);
	opts.address_log = address_log;
	opts.gen.flags |= M68K_OPT_BROKEN_READ_MODIFY;
	m68k_context *context = init_68k_context(&opts);
	gen->m68k = context;

	context->video_context = gen->vdp;
	context->system = gen;
	for (int i = 0; i < rom->map_chunks; i++)
	{
		if (rom->map[i].flags & MMAP_PTR_IDX) {
			context->mem_pointers[rom->map[i].ptr_index] = rom->map[i].buffer;
		}
	}

	if (statefile) {
		uint32_t pc = load_gst(gen, statefile);
		if (!pc) {
			fatal_error("Failed to load save state %s\n", statefile);
		}
		printf("Loaded %s\n", statefile);
		if (debugger) {
			insert_breakpoint(context, pc, debugger);
		}
		adjust_int_cycle(gen->m68k, gen->vdp);
		start_68k_context(context, pc);
	} else {
		if (debugger) {
			uint32_t address = cart[2] << 16 | cart[3];
			insert_breakpoint(context, address, debugger);
		}
		m68k_reset(context);
	}
}

char *title;

void update_title(char *rom_name)
{
	if (title) {
		free(title);
		title = NULL;
	}
	title = alloc_concat(rom_name, " - BlastEm");
}

void set_region(rom_info *info, uint8_t region)
{
	if (!region) {
		char * def_region = tern_find_ptr(config, "default_region");
		if (def_region && (!info->regions || (info->regions & translate_region_char(toupper(*def_region))))) {
			region = translate_region_char(toupper(*def_region));
		} else {
			region = info->regions;
		}
	}
	if (region & REGION_E) {
		version_reg = NO_DISK | EUR;
	} else if (region & REGION_J) {
		version_reg = NO_DISK | JAP;
	} else {
		version_reg = NO_DISK | USA;
	}
}

#ifndef NO_Z80
const memmap_chunk z80_map[] = {
	{ 0x0000, 0x4000,  0x1FFF, 0, MMAP_READ | MMAP_WRITE | MMAP_CODE, z80_ram, NULL, NULL, NULL,              NULL },
	{ 0x8000, 0x10000, 0x7FFF, 0, 0,                                  NULL,    NULL, NULL, z80_read_bank,     z80_write_bank},
	{ 0x4000, 0x6000,  0x0003, 0, 0,                                  NULL,    NULL, NULL, z80_read_ym,       z80_write_ym},
	{ 0x6000, 0x6100,  0xFFFF, 0, 0,                                  NULL,    NULL, NULL, NULL,              z80_write_bank_reg},
	{ 0x7F00, 0x8000,  0x00FF, 0, 0,                                  NULL,    NULL, NULL, z80_vdp_port_read, z80_vdp_port_write}
};
#endif

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
	config = load_config();
	int width = -1;
	int height = -1;
	int debug = 0;
	int ym_log = 0;
	int loaded = 0;
	uint8_t force_version = 0;
	char * romfname = NULL;
	FILE *address_log = NULL;
	char * statefile = NULL;
	int rom_size;
	uint8_t * debuggerfun = NULL;
	uint8_t fullscreen = FULLSCREEN_DEFAULT, use_gl = 1;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'b':
				i++;
				if (i >= argc) {
					fatal_error("-b must be followed by a frame count\n");
				}
				headless = 1;
				exit_after = atoi(argv[i]);
				break;
			case 'd':
				debuggerfun = (uint8_t *)debugger;
				break;
			case 'D':
				gdb_remote_init();
				debuggerfun = (uint8_t *)gdb_debug_enter;
				break;
			case 'f':
				fullscreen = !fullscreen;
				break;
			case 'g':
				use_gl = 0;
				break;
			case 'l':
				address_log = fopen("address.log", "w");
				break;
			case 'v':
				info_message("blastem %s\n", BLASTEM_VERSION);
				return 0;
				break;
			case 'n':
				z80_enabled = 0;
				break;
			case 'r':
				i++;
				if (i >= argc) {
					fatal_error("-r must be followed by region (J, U or E)\n");
				}
				force_version = translate_region_char(toupper(argv[i][0]));
				if (!force_version) {
					fatal_error("'%c' is not a valid region character for the -r option\n", argv[i][0]);
				}
				break;
			case 's':
				i++;
				if (i >= argc) {
					fatal_error("-s must be followed by a savestate filename\n");
				}
				statefile = argv[i];
				break;
			case 't':
				force_no_terminal();
				break;
			case 'y':
				ym_log = 1;
				break;
			case 'h':
				info_message(
					"Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n"
					"Options:\n"
					"	-h          Print this help text\n"
					"	-r (J|U|E)  Force region to Japan, US or Europe respectively\n"
					"	-f          Start in fullscreen mode\n"
					"	-g          Disable OpenGL rendering\n"
					"	-s FILE     Load a GST format savestate from FILE\n"
					"	-d          Enter debugger on startup\n"
					"	-n          Disable Z80\n"
					"	-v          Display version number and exit\n"
					"	-l          Log 68K code addresses (useful for assemblers)\n"
					"	-y          Log individual YM-2612 channels to WAVE files\n"
				);
				return 0;
			default:
				fatal_error("Unrecognized switch %s\n", argv[i]);
			}
		} else if (!loaded) {
			if (!(rom_size = load_rom(argv[i]))) {
				fatal_error("Failed to open %s for reading\n", argv[i]);
			}
			romfname = argv[i];
			loaded = 1;
		} else if (width < 0) {
			width = atoi(argv[i]);
		} else if (height < 0) {
			height = atoi(argv[i]);
		}
	}
	if (!loaded) {
#ifdef __ANDROID__
		//Temporary hack until UI is in place
		if (!(rom_size = load_rom("/mnt/sdcard/rom.bin"))) {
			fatal_error("Failed to open /mnt/sdcard/rom.bin for reading");
			
		}
		romfname = "/mnt/sdcard/rom.bin";
		loaded = 1;
#else
		fatal_error("Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n");
#endif
	}
	tern_node *rom_db = load_rom_db();
	rom_info info = configure_rom(rom_db, cart, rom_size, base_map, sizeof(base_map)/sizeof(base_map[0]));
	byteswap_rom(rom_size);
	set_region(&info, force_version);
	update_title(info.name);
	int def_width = 0;
	char *config_width = tern_find_path(config, "video\0width\0").ptrval;
	if (config_width) {
		def_width = atoi(config_width);
	}
	if (!def_width) {
		def_width = 640;
	}
	width = width < 320 ? def_width : width;
	height = height < 240 ? (width/320) * 240 : height;
	uint32_t fps = 60;
	if (version_reg & 0x40) {
		fps = 50;
	}
	if (!headless) {
		render_init(width, height, title, fps, fullscreen);
	}
	vdp_context v_context;
	genesis_context gen;
	memset(&gen, 0, sizeof(gen));
	gen.master_clock = gen.normal_clock = fps == 60 ? MCLKS_NTSC : MCLKS_PAL;

	init_vdp_context(&v_context, version_reg & 0x40);
	gen.frame_end = vdp_cycles_to_frame_end(&v_context);
	char * config_cycles = tern_find_path(config, "clocks\0max_cycles\0").ptrval;
	gen.max_cycles = config_cycles ? atoi(config_cycles) : DEFAULT_SYNC_INTERVAL;

	ym2612_context y_context;
	ym_init(&y_context, render_sample_rate(), gen.master_clock, MCLKS_PER_YM, render_audio_buffer(), ym_log ? YM_OPT_WAVE_LOG : 0);

	psg_context p_context;
	psg_init(&p_context, render_sample_rate(), gen.master_clock, MCLKS_PER_PSG, render_audio_buffer());

	z80_context z_context;
#ifndef NO_Z80
	z80_options z_opts;
	init_z80_opts(&z_opts, z80_map, 5, NULL, 0, MCLKS_PER_Z80);
	init_z80_context(&z_context, &z_opts);
	z80_assert_reset(&z_context, 0);
#endif

	z_context.system = &gen;
	z_context.mem_pointers[0] = z80_ram;
	z_context.mem_pointers[1] = z_context.mem_pointers[2] = (uint8_t *)cart;

	gen.z80 = &z_context;
	gen.vdp = &v_context;
	gen.ym = &y_context;
	gen.psg = &p_context;
	gen.work_ram = ram;
	gen.zram = z80_ram;
	genesis = &gen;
	setup_io_devices(config, gen.ports);

	int fname_size = strlen(romfname);
	char * ext = info.save_type == SAVE_I2C ? "eeprom" : "sram";
	save_filename = malloc(fname_size+strlen(ext) + 2);
	memcpy(save_filename, romfname, fname_size);
	int i;
	for (i = fname_size-1; fname_size >= 0; --i) {
		if (save_filename[i] == '.') {
			strcpy(save_filename + i + 1, ext);
			break;
		}
	}
	if (i < 0) {
		save_filename[fname_size] = '.';
		strcpy(save_filename + fname_size + 1, ext);
	}
	set_keybindings(gen.ports);

	init_run_cpu(&gen, &info, address_log, statefile, debuggerfun);
	return 0;
}
