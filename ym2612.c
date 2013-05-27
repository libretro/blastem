#include <string.h>
#include <math.h>
#include <stdio.h>
#include "ym2612.h"

#define BUSY_CYCLES 17
#define TIMERA_UPDATE_PERIOD 144

#define REG_TIMERA_HIGH 0x03 // 0x24
#define REG_TIMERA_LOW  0x04 // 0x25
#define REG_TIMERB      0x05 // 0x26
#define REG_TIME_CTRL   0x06 // 0x27
#define REG_DAC         0x0A // 0x2A
#define REG_DAC_ENABLE  0x0B // 0x2B

//offset to add to "shared" regs when looking for them in Part I
#define REG_SHARED      0x10


#define REG_ALG_FEEDBACK (0xB0-0x30)
#define REG_ATTACK_KS    (0x50-0x30)
#define REG_DECAY_AM     (0x60-0x30)
#define REG_SUSTAIN_RATE (0x70-0x30)


#define BIT_TIMERA_ENABLE 0x1
#define BIT_TIMERB_ENABLE 0x2
#define BIT_TIMERA_OVEREN 0x4
#define BIT_TIMERB_OVEREN 0x8
#define BIT_TIMERA_RESET  0x10
#define BIT_TIMERB_RESET  0x20

#define BIT_STATUS_TIMERA 0x1
#define BIT_STATUS_TIMERB 0x2

enum {
	PHASE_ATTACK,
	PHASE_DECAY,
	PHASE_SUSTAIN,
	PHASE_RELEASE
};

uint8_t did_tbl_init = 0;
//According to Nemesis, real hardware only uses a 256 entry quarter sine table; however,
//memory is cheap so using a half sine table will probably save some cycles
//a full sine table would be nice, but negative numbers don't get along with log2
#define SINE_TABLE_SIZE 512
uint16_t sine_table[SINE_TABLE_SIZE];
//Similar deal here with the power table for log -> linear conversion
//According to Nemesis, real hardware only uses a 256 entry table for the fractional part
//and uses the whole part as a shift amount.
#define POW_TABLE_SIZE (1 << 13)
uint16_t pow_table[POW_TABLE_SIZE];


uint16_t round_fixed_point(double value, int dec_bits)
{
	return value * (1 << dec_bits) + 0.5;
}

void ym_init(ym2612_context * context)
{
	memset(context, 0, sizeof(*context));
	if (!did_tbl_init) {
		//populate sine table
		for (int32_t i = 0; i < 512; i++) {
			double sine = sin( ((double)(i*2+1) / SINE_TABLE_SIZE) * M_PI_2 );
			
			//table stores 4.8 fixed pointed representation of the base 2 log
			sine_table[i] = round_fixed_point(-log2(sine), 8);
		}
		//populate power table
		for (int32_t i = 0; i < POW_TABLE_SIZE; i++) {
			double linear = pow(2, -((double)((i & 0xFF)+1) / 256.0));
			int32_t tmp = round_fixed_point(linear, 11);
			int32_t shift = (i >> 8) - 2;
			if (shift < 0) {
				tmp <<= 0-shift;
			} else {
				tmp >>= shift;
			}
			pow_table[i] =  tmp;
		}
	}
}

void ym_run(ym2612_context * context, uint32_t to_cycle)
{
	for (; context->current_cycle < to_cycle; context->current_cycle += 6) {
		uint32_t update_cyc = context->current_cycle % 144;
		//Update timers at beginning of 144 cycle period
		if (!update_cyc && context->part1_regs[REG_TIME_CTRL] & BIT_TIMERA_ENABLE) {
			if (context->timer_a) {
				context->timer_a--;
			} else {
				if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERA_OVEREN) {
					context->status |= BIT_STATUS_TIMERA;
				}
				context->timer_a = (context->part1_regs[REG_TIMERA_HIGH] << 2) | (context->part1_regs[REG_TIMERA_LOW] & 0x3);
			}
			if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERB_ENABLE) {
				uint32_t b_cyc = (context->current_cycle / 144) % 16;
				if (!b_cyc) {
					if (context->timer_b) {
						context->timer_b--;
					} else {
						if (context->part1_regs[REG_TIME_CTRL] & BIT_TIMERB_OVEREN) {
							context->status |= BIT_STATUS_TIMERB;
						}
						context->timer_b = context->part1_regs[REG_TIMERB];
					}
				}
			}
		}
		//Update Envelope Generator
		if (update_cyc == 0 || update_cyc == 72) {
			uint32_t env_cyc = context->current_cycle / 72;
			uint32_t op = env_cyc % 24;
			env_cyc /= 24;
			uint8_t rate;
			switch(context->env_phase[op])
			{
			case PHASE_ATTACK:
				rate = (op < 3 ? context->part1_regs[REG_SHARED + REG_ATTACK_KS + op] : context->part2_regs[REG_ATTACK_KS + op - 3]) & 0x1F;
				break;
			case PHASE_DECAY:
				rate = (op < 3 ? context->part1_regs[REG_SHARED + REG_DECAY_AM + op] : context->part2_regs[REG_DECAY_AM + op - 3]) & 0x1F;
				break;
			case PHASE_SUSTAIN:
				rate = (op < 3 ? context->part1_regs[REG_SHARED + REG_SUSTAIN_RATE + op] : context->part2_regs[REG_SUSTAIN_RATE + op - 3]) & 0x1F;
				break;
			case PHASE_RELEASE:
				rate = (op < 3 ? context->part1_regs[REG_SHARED + REG_DECAY_AM + op] : context->part2_regs[REG_DECAY_AM + op - 3]) << 1 & 0x1E | 1;
				break;
			}
			if (rate) {
				//apply key scaling
				uint8_t shift = (op < 3 ? 
					context->part1_regs[REG_SHARED + REG_ATTACK_KS + op] : 
					context->part2_regs[REG_ATTACK_KS + op - 3]
				) >> 6;
				uint8_t ks = context->keycode[op] >> (3 - shift);
				rate = rate*2 + ks;
				if (rate > 63) {
					rate = 63;
				}
			}
		}
		//Update Phase Generator
		uint32_t channel = update_cyc / 24;
		if (channel != 6 || !(context->part1_regs[REG_DAC_ENABLE] & 0x80)) {
			uint32_t op = (update_cyc) / 6;
			uint8_t alg;
			if (op < 3) {
				alg = context->part1_regs[REG_SHARED + REG_ALG_FEEDBACK + op] & 0x7;
			} else {
				alg = context->part2_regs[REG_ALG_FEEDBACK + op-3] & 0x7;
			}
			context->phase_counter[op] += context->phase_inc[op];
			uint16_t phase = context->phase_counter[op] >> 10 & 0x3FF;
			//TODO: Modulate phase if necessary
			uint16_t output = pow_table[sine_table[phase & 0x1FF] + context->envelope[op]];
			if (phase & 0x200) {
				output = -output;
			}
			context->op_out[op] = output;
			//Update the channel output if we've updated all operators
			if (op % 4 == 3) {
				if (alg < 4) {
					context->channel_out[channel] = context->op_out[channel * 4 + 3];
				} else if(alg == 4) {
					context->channel_out[channel] = context->op_out[channel * 4 + 3] + context->op_out[channel * 4 + 1];
				} else {
					output = 0;
					for (uint32_t op = ((alg == 7) ? 0 : 1) + channel*4; op < (channel+1)*4; op++) {
						output += context->op_out[op];
					}
					context->channel_out[channel] = output;
				}
			}
		}
		
	}
	if (context->current_cycle >= context->write_cycle + BUSY_CYCLES) {
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

