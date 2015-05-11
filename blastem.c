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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLASTEM_VERSION "0.2.0"

#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

#define MCLKS_PER_68K 7
#define MCLKS_PER_YM  MCLKS_PER_68K
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)

//TODO: Figure out the exact value for this
#define LINES_NTSC 262
#define LINES_PAL 312

#define MAX_SOUND_CYCLES 100000

uint16_t cart[CARTRIDGE_WORDS];
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
	while (filesize > 0) {
		fread(block, 1, SMD_BLOCK_SIZE, f);
		for (uint8_t *low = block, *high = (block+SMD_BLOCK_SIZE/2), *end = block+SMD_BLOCK_SIZE; high < end; high++, low++) {
			*(dst++) = *high << 8 | *low;
		}
		filesize -= SMD_BLOCK_SIZE;
	}
	return 1;
}

int load_rom(char * filename)
{
	uint8_t header[10];
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	fread(header, 1, sizeof(header), f);
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	if (filesize/2 > CARTRIDGE_WORDS) {
		//carts bigger than 4MB not currently supported
		filesize = CARTRIDGE_WORDS*2;
	}
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
				fprintf(stderr, "%s is a split SMD ROM which is not currently supported", filename);
				exit(1);
			}
			return load_smd_rom(filesize, f);
		}
	}
	fread(cart, 2, filesize/2, f);
	fclose(f);
	for(unsigned short * cur = cart; cur - cart < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	//TODO: Mirror ROM
	return 1;
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
		gen->frame_end = vdp_cycles_to_frame_end(v_context);
	} else {
		gen->frame_end = vdp_cycles_to_frame_end(v_context);
	}
	context->sync_cycle = gen->frame_end;
	//printf("Set sync cycle to: %d @ %d, vcounter: %d, hslot: %d\n", context->sync_cycle, context->current_cycle, v_context->vcounter, v_context->hslot);
	if (context->int_ack) {
		vdp_int_ack(v_context, context->int_ack);
		context->int_ack = 0;
	}
	adjust_int_cycle(context, v_context);
	if (address) {
		if (break_on_sync) {
			break_on_sync = 0;
			debugger(context, address);
		}
		if (save_state) {
			save_state = 0;
			//advance Z80 core to the start of an instruction
			while (!z_context->pc)
			{
				sync_z80(z_context, z_context->current_cycle + MCLKS_PER_Z80);
			}
			save_gst(gen, "savestate.gst", address);
		}
	}
	return context;
}

m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	if (vdp_port & 0x2700E0) {
		printf("machine freeze due to write to address %X\n", 0xC00000 | vdp_port);
		exit(1);
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
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
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
		printf("machine freeze due to write to Z80 address %X\n", 0x7F00 | vdp_port);
		exit(1);
	}
	if (vdp_port < 0x10) {
		//These probably won't currently interact well with the 68K accessing the VDP
		vdp_run_context(gen->vdp, context->current_cycle);
		if (vdp_port < 4) {
			vdp_data_port_write(gen->vdp, value << 8 | value);
		} else if (vdp_port < 8) {
			vdp_control_port_write(gen->vdp, value << 8 | value);
		} else {
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
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
		printf("machine freeze due to read from address %X\n", 0xC00000 | vdp_port);
		exit(1);
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
		printf("Illegal read from PSG  port %X\n", vdp_port);
		exit(1);
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
		printf("machine freeze due to read from Z80 address %X\n", 0x7F00 | vdp_port);
		exit(1);
	}
	genesis_context * gen = context->system;
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
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
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
				printf("68K write to unhandled Z80 address %X\n", location);
				exit(1);
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
	if (context->bank_reg < 0x80) {
		genesis_context *gen = context->system;
		context->mem_pointers[1] = get_native_pointer(context->bank_reg << 15, (void **)gen->m68k->mem_pointers, &gen->m68k->options->gen);
	} else {
		context->mem_pointers[1] = NULL;
	}

	return context;
}

uint16_t read_sram_w(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_flags)
	{
	case RAM_FLAG_BOTH:
		return gen->save_ram[address] << 8 | gen->save_ram[address+1];
	case RAM_FLAG_EVEN:
		return gen->save_ram[address >> 1] << 8 | 0xFF;
	case RAM_FLAG_ODD:
		return gen->save_ram[address >> 1] | 0xFF00;
	}
	return 0xFFFF;//We should never get here
}

uint8_t read_sram_b(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_flags)
	{
	case RAM_FLAG_BOTH:
		return gen->save_ram[address];
	case RAM_FLAG_EVEN:
		if (address & 1) {
			return 0xFF;
		} else {
			return gen->save_ram[address >> 1];
		}
	case RAM_FLAG_ODD:
		if (address & 1) {
			return gen->save_ram[address >> 1];
		} else {
			return 0xFF;
		}
	}
	return 0xFF;//We should never get here
}

m68k_context * write_sram_area_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_flags)
		{
		case RAM_FLAG_BOTH:
			gen->save_ram[address] = value >> 8;
			gen->save_ram[address+1] = value;
			break;
		case RAM_FLAG_EVEN:
			gen->save_ram[address >> 1] = value >> 8;
			break;
		case RAM_FLAG_ODD:
			gen->save_ram[address >> 1] = value;
			break;
		}
	}
	return context;
}

m68k_context * write_sram_area_b(uint32_t address, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_flags)
		{
		case RAM_FLAG_BOTH:
			gen->save_ram[address] = value;
			break;
		case RAM_FLAG_EVEN:
			if (!(address & 1)) {
				gen->save_ram[address >> 1] = value;
			}
			break;
		case RAM_FLAG_ODD:
			if (address & 1) {
				gen->save_ram[address >> 1] = value;
			}
			break;
		}
	}
	return context;
}

m68k_context * write_bank_reg_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	address &= 0xE;
	address >>= 1;
	gen->bank_regs[address] = value;
	if (!address) {
		if (value & 1) {
			context->mem_pointers[2] = NULL;
		} else {
			context->mem_pointers[2] = cart + 0x200000/2;
		}
	}
	return context;
}

m68k_context * write_bank_reg_b(uint32_t address, m68k_context * context, uint8_t value)
{
	if (address & 1) {
		genesis_context * gen = context->system;
		address &= 0xE;
		address >>= 1;
		gen->bank_regs[address] = value;
		if (!address) {
			if (value & 1) {
				context->mem_pointers[2] = NULL;
			} else {
				context->mem_pointers[2] = cart + 0x200000/2;
			}
		}
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

#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define MAX_MAP_CHUNKS (4+7+1)
#define RAM_FLAG_MASK 0x1800

const memmap_chunk static_map[] = {
		{0,        0x400000,  0xFFFFFF, 0, MMAP_READ,                          cart,
		           NULL,          NULL,         NULL,            NULL},
		{0xE00000, 0x1000000, 0xFFFF,   0, MMAP_READ | MMAP_WRITE | MMAP_CODE, ram,
		           NULL,          NULL,         NULL,            NULL},
		{0xC00000, 0xE00000,  0x1FFFFF, 0, 0,                                  NULL,
		           (read_16_fun)vdp_port_read,  (write_16_fun)vdp_port_write,
		           (read_8_fun)vdp_port_read_b, (write_8_fun)vdp_port_write_b},
		{0xA00000, 0xA12000,  0x1FFFF,  0, 0,                                  NULL,
		           (read_16_fun)io_read_w,      (write_16_fun)io_write_w,
		           (read_8_fun)io_read,         (write_8_fun)io_write}
	};

char * sram_filename;
genesis_context * genesis;
void save_sram()
{
	FILE * f = fopen(sram_filename, "wb");
	if (!f) {
		fprintf(stderr, "Failed to open SRAM file %s for writing\n", sram_filename);
		return;
	}
	uint32_t size = genesis->save_ram_mask+1;
	if (genesis->save_flags != RAM_FLAG_BOTH) {
		size/= 2;
	}
	fwrite(genesis->save_ram, 1, size, f);
	fclose(f);
	printf("Saved SRAM to %s\n", sram_filename);
}

void init_run_cpu(genesis_context * gen, FILE * address_log, char * statefile, uint8_t * debugger)
{
	m68k_options opts;
	memmap_chunk memmap[MAX_MAP_CHUNKS];
	uint32_t num_chunks;
	void * initial_mapped = NULL;
	gen->save_ram = NULL;
	//TODO: Handle carts larger than 4MB
	//TODO: Handle non-standard mappers
	uint32_t size;
	if ((cart[RAM_ID/2] & 0xFF) == 'A' && (cart[RAM_ID/2] >> 8) == 'R') {
		//Cart has save RAM
		uint32_t rom_end = ((cart[ROM_END/2] << 16) | cart[ROM_END/2+1]) + 1;
		uint32_t ram_start = (cart[RAM_START/2] << 16) | cart[RAM_START/2+1];
		uint32_t ram_end = (cart[RAM_END/2] << 16) | cart[RAM_END/2+1];
		uint16_t ram_flags = cart[RAM_FLAGS/2];
		gen->save_flags = ram_flags & RAM_FLAG_MASK;
		memset(memmap, 0, sizeof(memmap_chunk)*2);
		if (ram_start >= rom_end) {
			memmap[0].end = rom_end;
			memmap[0].mask = 0xFFFFFF;
			memmap[0].flags = MMAP_READ;
			memmap[0].buffer = cart;

			ram_start &= 0xFFFFFE;
			ram_end |= 1;
			memmap[1].start = ram_start;
			gen->save_ram_mask = memmap[1].mask = ram_end-ram_start;
			ram_end += 1;
			memmap[1].end = ram_end;
			memmap[1].flags = MMAP_READ | MMAP_WRITE;
			size = ram_end-ram_start;
			if ((ram_flags & RAM_FLAG_MASK) == RAM_FLAG_ODD) {
				memmap[1].flags |= MMAP_ONLY_ODD;
				size /= 2;
			} else if((ram_flags & RAM_FLAG_MASK) == RAM_FLAG_EVEN) {
				memmap[1].flags |= MMAP_ONLY_EVEN;
				size /= 2;
			}
			memmap[1].buffer = gen->save_ram = malloc(size);

			memcpy(memmap+2, static_map+1, sizeof(static_map)-sizeof(static_map[0]));
			num_chunks = sizeof(static_map)/sizeof(memmap_chunk)+1;
		} else {
			//Assume the standard Sega mapper for now
			memmap[0].end = 0x200000;
			memmap[0].mask = 0xFFFFFF;
			memmap[0].flags = MMAP_READ;
			memmap[0].buffer = cart;

			memmap[1].start = 0x200000;
			memmap[1].end = 0x400000;
			memmap[1].mask = 0x1FFFFF;
			ram_start &= 0xFFFFFE;
			ram_end |= 1;
			gen->save_ram_mask = ram_end-ram_start;
			memmap[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
			memmap[1].ptr_index = 2;
			memmap[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
			memmap[1].read_8 = (read_8_fun)read_sram_b;
			memmap[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
			memmap[1].write_8 = (write_8_fun)write_sram_area_b;
			memcpy(memmap+2, static_map+1, sizeof(static_map)-sizeof(static_map[0]));
			num_chunks = sizeof(static_map)/sizeof(memmap_chunk)+1;
			memset(memmap+num_chunks, 0, sizeof(memmap[num_chunks]));
			memmap[num_chunks].start = 0xA13000;
			memmap[num_chunks].end = 0xA13100;
			memmap[num_chunks].mask = 0xFF;
			memmap[num_chunks].write_16 = (write_16_fun)write_bank_reg_w;
			memmap[num_chunks].write_8 = (write_8_fun)write_bank_reg_b;
			num_chunks++;
			ram_end++;
			size = ram_end-ram_start;
			if ((ram_flags & RAM_FLAG_MASK) != RAM_FLAG_BOTH) {
				size /= 2;
			}
			gen->save_ram = malloc(size);
			memmap[1].buffer = initial_mapped = cart + 0x200000/2;
		}
	} else {
		memcpy(memmap, static_map, sizeof(static_map));
		num_chunks = sizeof(static_map)/sizeof(memmap_chunk);
	}
	if (gen->save_ram) {
		memset(gen->save_ram, 0, size);
		FILE * f = fopen(sram_filename, "rb");
		if (f) {
			uint32_t read = fread(gen->save_ram, 1, size, f);
			fclose(f);
			if (read > 0) {
				printf("Loaded SRAM from %s\n", sram_filename);
			}
		}
		atexit(save_sram);
	}
	init_m68k_opts(&opts, memmap, num_chunks, MCLKS_PER_68K);
	opts.address_log = address_log;
	m68k_context *context = init_68k_context(&opts);
	gen->m68k = context;

	context->video_context = gen->vdp;
	context->system = gen;
	//cartridge ROM
	context->mem_pointers[0] = cart;
	context->target_cycle = context->sync_cycle = gen->frame_end > gen->max_cycles ? gen->frame_end : gen->max_cycles;
	//work RAM
	context->mem_pointers[1] = ram;
	//save RAM/map
	context->mem_pointers[2] = initial_mapped;
	context->mem_pointers[3] = (uint16_t *)gen->save_ram;
	uint32_t address;
	address = cart[2] << 16 | cart[3];
	translate_m68k_stream(address, context);
	if (statefile) {
		uint32_t pc = load_gst(gen, statefile);
		if (!pc) {
			fprintf(stderr, "Failed to load save state %s\n", statefile);
			exit(1);
		}
		printf("Loaded %s\n", statefile);
		if (debugger) {
			insert_breakpoint(context, pc, debugger);
		}
		adjust_int_cycle(gen->m68k, gen->vdp);
		start_68k_context(context, pc);
	} else {
		if (debugger) {
			insert_breakpoint(context, address, debugger);
		}
		m68k_reset(context);
	}
}

char title[64];

#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)

void update_title()
{
	uint16_t *last = cart + TITLE_END/2 - 1;
	while(last > cart + TITLE_START/2 && *last == 0x2020)
	{
		last--;
	}
	uint16_t *start = cart + TITLE_START/2;
	char *cur = title;
	char last_char = ' ';
	for (; start != last; start++)
	{
		if ((last_char != ' ' || (*start >> 8) != ' ') && (*start >> 8) < 0x80) {
			*(cur++) = *start >> 8;
			last_char = *start >> 8;
		}
		if (last_char != ' ' || (*start & 0xFF) != ' ' && (*start & 0xFF) < 0x80) {
			*(cur++) = *start;
			last_char = *start & 0xFF;
		}
	}
	*(cur++) = *start >> 8;
	if ((*start & 0xFF) != ' ') {
		*(cur++) = *start;
	}
	strcpy(cur, " - BlastEm");
}

#define REGION_START 0x1F0

int detect_specific_region(char region)
{
	return (cart[REGION_START/2] & 0xFF) == region || (cart[REGION_START/2] >> 8) == region || (cart[REGION_START/2+1] & 0xFF) == region;
}

void detect_region()
{
	if (detect_specific_region('U')|| detect_specific_region('B') || detect_specific_region('4')) {
		version_reg = NO_DISK | USA;
	} else if (detect_specific_region('J')) {
		version_reg = NO_DISK | JAP;
	} else if (detect_specific_region('E') || detect_specific_region('A')) {
		version_reg = NO_DISK | EUR;
	} else {
		char * def_region = tern_find_ptr(config, "default_region");
		if (def_region) {
			switch(*def_region)
			{
			case 'j':
			case 'J':
				version_reg = NO_DISK | JAP;
				break;
			case 'u':
			case 'U':
				version_reg = NO_DISK | USA;
				break;
			case 'e':
			case 'E':
				version_reg = NO_DISK | EUR;
				break;
			}
		}
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
	if (argc < 2) {
		fputs("Usage: blastem [OPTIONS] ROMFILE [WIDTH] [HEIGHT]\n", stderr);
		return 1;
	}
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
	uint8_t * debuggerfun = NULL;
	uint8_t fullscreen = 0, use_gl = 1;
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			switch(argv[i][1]) {
			case 'b':
				i++;
				if (i >= argc) {
					fputs("-b must be followed by a frame count\n", stderr);
					return 1;
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
				fullscreen = 1;
				break;
			case 'g':
				use_gl = 0;
				break;
			case 'l':
				address_log = fopen("address.log", "w");
				break;
			case 'v':
				printf("blastem %s\n", BLASTEM_VERSION);
				return 0;
				break;
			case 'n':
				z80_enabled = 0;
				break;
			case 'r':
				i++;
				if (i >= argc) {
					fputs("-r must be followed by region (J, U or E)\n", stderr);
					return 1;
				}
				switch (argv[i][0])
				{
				case 'j':
				case 'J':
					force_version = NO_DISK | JAP;
					break;
				case 'u':
				case 'U':
					force_version = NO_DISK | USA;
					break;
				case 'e':
				case 'E':
					force_version = NO_DISK | EUR;
					break;
				default:
					fprintf(stderr, "'%c' is not a valid region character for the -r option\n", argv[i][0]);
					return 1;
				}
				break;
			case 's':
				i++;
				if (i >= argc) {
					fputs("-s must be followed by a savestate filename\n", stderr);
					return 1;
				}
				statefile = argv[i];
				break;
			case 'y':
				ym_log = 1;
				break;
			case 'h':
				puts(
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
				fprintf(stderr, "Unrecognized switch %s\n", argv[i]);
				return 1;
			}
		} else if (!loaded) {
			if(!load_rom(argv[i])) {
				fprintf(stderr, "Failed to open %s for reading\n", argv[i]);
				return 1;
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
		fputs("You must specify a ROM filename!\n", stderr);
		return 1;
	}
	if (force_version) {
		version_reg = force_version;
	} else {
		detect_region();
	}
	update_title();
	int def_width = 0;
	char *config_width = tern_find_ptr(config, "videowidth");
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
		render_init(width, height, title, fps, fullscreen, use_gl);
	}
	vdp_context v_context;
	genesis_context gen;
	memset(&gen, 0, sizeof(gen));
	gen.master_clock = gen.normal_clock = fps == 60 ? MCLKS_NTSC : MCLKS_PAL;

	init_vdp_context(&v_context, version_reg & 0x40);
	gen.frame_end = vdp_cycles_to_frame_end(&v_context);
	char * config_cycles = tern_find_ptr(config, "clocksmax_cycles");
	gen.max_cycles = config_cycles ? atoi(config_cycles) : 10000000;

	ym2612_context y_context;
	ym_init(&y_context, render_sample_rate(), gen.master_clock, MCLKS_PER_YM, render_audio_buffer(), ym_log ? YM_OPT_WAVE_LOG : 0);

	psg_context p_context;
	psg_init(&p_context, render_sample_rate(), gen.master_clock, MCLKS_PER_PSG, render_audio_buffer());

	z80_context z_context;
#ifndef NO_Z80
	z80_options z_opts;
	init_z80_opts(&z_opts, z80_map, 5, MCLKS_PER_Z80);
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
	genesis = &gen;
	setup_io_devices(config, gen.ports);

	int fname_size = strlen(romfname);
	sram_filename = malloc(fname_size+6);
	memcpy(sram_filename, romfname, fname_size);
	int i;
	for (i = fname_size-1; fname_size >= 0; --i) {
		if (sram_filename[i] == '.') {
			strcpy(sram_filename + i + 1, "sram");
			break;
		}
	}
	if (i < 0) {
		strcpy(sram_filename + fname_size, ".sram");
	}
	set_keybindings(gen.ports);

	init_run_cpu(&gen, address_log, statefile, debuggerfun);
	return 0;
}
