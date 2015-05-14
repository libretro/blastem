/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef BLASTEM_H_
#define BLASTEM_H_

#include <stdint.h>
#include "m68k_core.h"
#include "z80_to_x86.h"
#include "ym2612.h"
#include "vdp.h"
#include "psg.h"
#include "io.h"
#include "config.h"

#define RAM_FLAG_ODD  0x1800
#define RAM_FLAG_EVEN 0x1000
#define RAM_FLAG_BOTH 0x0000

typedef struct {
	m68k_context   *m68k;
	z80_context    *z80;
	vdp_context    *vdp;
	ym2612_context *ym;
	psg_context    *psg;
	uint8_t        *save_ram;
	uint32_t       save_ram_mask;
	uint32_t       save_flags;
	uint32_t       master_clock; //Current master clock value
	uint32_t       normal_clock; //Normal master clock (used to restore master clock after turbo mode)
	uint32_t       frame_end;
	uint32_t       max_cycles;
	uint8_t        bank_regs[8];
	io_port        ports[3];
	uint8_t        bus_busy;
} genesis_context;

extern genesis_context * genesis;
extern int headless;
extern int break_on_sync;
extern int save_state;
extern tern_node * config;

#define CARTRIDGE_WORDS 0x200000
#define RAM_WORDS 32 * 1024
#define Z80_RAM_BYTES 8 * 1024

extern uint16_t cart[CARTRIDGE_WORDS];
extern uint16_t ram[RAM_WORDS];
extern uint8_t z80_ram[Z80_RAM_BYTES];

uint16_t read_dma_value(uint32_t address);
m68k_context * sync_components(m68k_context *context, uint32_t address);
m68k_context * debugger(m68k_context * context, uint32_t address);
void set_speed_percent(genesis_context * context, uint32_t percent);

#endif //BLASTEM_H_

