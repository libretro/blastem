/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef BLASTEM_H_
#define BLASTEM_H_

#include <stdint.h>
#include "m68k_to_x86.h"
#include "z80_to_x86.h"
#include "ym2612.h"
#include "vdp.h"
#include "psg.h"
#include "io.h"
#include "config.h"

#define RAM_FLAG_ODD  0x1800
#define RAM_FLAG_EVEN 0x1000
#define RAM_FLAG_BOTH 0x0000

#define CYCLE_NEVER 0xFFFFFFFF

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
	uint8_t        bank_regs[8];
	io_port        ports[3];
} genesis_context;

extern genesis_context * genesis;
extern int break_on_sync;
extern int save_state;
extern tern_node * config;
extern uint8_t busreq;
extern uint8_t reset;

uint16_t read_dma_value(uint32_t address);
m68k_context * debugger(m68k_context * context, uint32_t address);
void set_speed_percent(genesis_context * context, uint32_t percent);

#endif //BLASTEM_H_

