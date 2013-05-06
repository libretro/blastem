#include <string.h>
#include "ym2612.h"

#define BUSY_CYCLES 17
#define TIMERA_UPDATE_PERIOD 144

#define REG_TIMERA_HIGH 0x3 // 0x24
#define REG_TIMERA_LOW 0x4  // 0x25
#define REG_TIMERB 0x5      // 0x26
#define REG_TIME_CTRL 0x6   // 0x27

#define BIT_TIMERA_ENABLE 0x1
#define BIT_TIMERB_ENABLE 0x2
#define BIT_TIMERA_OVEREN 0x4
#define BIT_TIMERB_OVEREN 0x8
#define BIT_TIMERA_RESET  0x10
#define BIT_TIMERB_RESET  0x20

#define BIT_STATUS_TIMERA 0x1
#define BIT_STATUS_TIMERB 0x2

void ym_init(ym2612_context * context)
{
	memset(context, 0, sizeof(*context));
}

void ym_run(ym2612_context * context, uint32_t to_cycle)
{
	uint32_t delta = to_cycle - context->current_cycle;
	//Timers won't be perfect with this, but it's good enough for now
	//once actual FM emulation is in place the timers should just be 
	//decremented/reloaded on the appropriate ticks
	uint32_t timer_delta = to_cycle / TIMERA_UPDATE_PERIOD;
	if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERA_ENABLE) {
		if (timer_delta > context->timer_a) {
			if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERA_OVEREN) {
				context->status |= BIT_STATUS_TIMERA;
			}
			uint32_t rem_delta = timer_delta - (context->timer_a+1);
			uint16_t timer_val = (context->part1_regs[REG_TIMERA_HIGH] << 2) | (context->part1_regs[REG_TIMERA_LOW] & 0x3);
			context->timer_a = timer_val - (rem_delta % (timer_val + 1));
		} else {
			context->timer_a -= timer_delta;
		}
	}
	timer_delta /= 16; //Timer B runs at 1/16th the speed of Timer A
	if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERB_ENABLE) {
		if (timer_delta > context->timer_b) {
			if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERB_OVEREN) {
				context->status |= BIT_STATUS_TIMERB;
			}
			uint32_t rem_delta = timer_delta - (context->timer_b+1);
			uint8_t timer_val = context->part1_regs[REG_TIMERB];
			context->timer_b = timer_val - (rem_delta % (timer_val + 1));
		} else {
			context->timer_a -= timer_delta;
		}
	}
	context->current_cycle = to_cycle;
	if (to_cycle >= context->write_cycle + BUSY_CYCLES) {
		context->status &= 0x7F;
	}
}

void ym_address_write_part1(ym2612_context * context, uint8_t address)
{
	if (address >= 0x21 && address < 0xB7) {
		context->selected_reg = context->part1_regs + address - 0x21;
	} else {
		context->selected_reg = NULL;
	}
}

void ym_address_write_part2(ym2612_context * context, uint8_t address)
{
	if (address >= 0x30 && address < 0xB7) {
		context->selected_reg = context->part1_regs + address - 0x30;
	} else {
		context->selected_reg = NULL;
	}
}

void ym_data_write(ym2612_context * context, uint8_t value)
{
	if (context->selected_reg && !(context->status & 0x80)) {
		*context->selected_reg = value;
		context->write_cycle = context->current_cycle;
		context->selected_reg = NULL;//TODO: Verify this
	}
}

uint8_t ym_read_status(ym2612_context * context)
{
	return context->status;
}

