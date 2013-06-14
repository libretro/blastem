#ifndef BLASTEM_H_
#define BLASTEM_H_

#include <stdint.h>
#include "m68k_to_x86.h"
#include "z80_to_x86.h"
#include "ym2612.h"
#include "vdp.h"
#include "psg.h"

typedef struct {
	uint32_t th_counter;
	uint32_t timeout_cycle;
	uint8_t output;
	uint8_t control;
	uint8_t input[3];
} io_port;

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
	uint8_t        bank_regs[8];
} genesis_context;

#define GAMEPAD_TH0 0
#define GAMEPAD_TH1 1
#define GAMEPAD_EXTRA 2
#define GAMEPAD_NONE 0xF

extern io_port gamepad_1;
extern io_port gamepad_2;

void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction);
uint16_t read_dma_value(uint32_t address);
m68k_context * debugger(m68k_context * context, uint32_t address);
void handle_keydown(int keycode);
void handle_keyup(int keycode);

#endif //BLASTEM_H_

