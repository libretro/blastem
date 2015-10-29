/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "psg.h"
#include "render.h"
#include "blastem.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

FILE *blah;

void psg_init(psg_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t samples_frame)
{
	memset(context, 0, sizeof(*context));
	context->audio_buffer = malloc(sizeof(*context->audio_buffer) * samples_frame);
	context->back_buffer = malloc(sizeof(*context->audio_buffer) * samples_frame);
	context->clock_inc = clock_div;
	context->sample_rate = sample_rate;
	context->samples_frame = samples_frame;
	psg_adjust_master_clock(context, master_clock);
	for (int i = 0; i < 4; i++) {
		context->volume[i] = 0xF;
	}
	blah = fopen("psg.raw", "wb");
}

#define BUFFER_INC_RES 1000000000UL

void psg_adjust_master_clock(psg_context * context, uint32_t master_clock)
{
	uint64_t old_inc = context->buffer_inc;
	context->buffer_inc = ((BUFFER_INC_RES * (uint64_t)context->sample_rate) / (uint64_t)master_clock) * (uint64_t)context->clock_inc;
}

void psg_write(psg_context * context, uint8_t value)
{
	if (value & 0x80) {
		context->latch = value & 0x70;
		uint8_t channel = value >> 5 & 0x3;
		if (value & 0x10) {
			context->volume[channel] = value & 0xF;
		} else {
			if (channel == 3) {
				switch(value & 0x3)
				{
				case 0:
				case 1:
				case 2:
					context->counter_load[3] = 0x10 << (value & 0x3);
					context->noise_use_tone = 0;
					break;
				default:
					context->counter_load[3] = context->counter_load[2];
					context->noise_use_tone = 1;
				}
				context->noise_type = value & 0x4;
				context->lsfr = 0x8000;
			} else {
				context->counter_load[channel] = (context->counter_load[channel] & 0x3F0) | (value & 0xF);
				if (channel == 2 && context->noise_use_tone) {
					context->counter_load[3] = context->counter_load[2];
				}
			}
		}
	} else {
		if (!(context->latch & 0x10)) {
			uint8_t channel = context->latch >> 5 & 0x3;
			if (channel != 3) {
				context->counter_load[channel] = (value << 4 & 0x3F0) | (context->counter_load[channel] & 0xF);
				if (channel == 2 && context->noise_use_tone) {
					context->counter_load[3] = context->counter_load[2];
				}
			}
		}
	}
}

#define PSG_VOL_DIV 14

//table shamelessly swiped from PSG doc from smspower.org
int16_t volume_table[16] = {
	32767/PSG_VOL_DIV, 26028/PSG_VOL_DIV, 20675/PSG_VOL_DIV, 16422/PSG_VOL_DIV, 13045/PSG_VOL_DIV, 10362/PSG_VOL_DIV,
	8231/PSG_VOL_DIV, 6568/PSG_VOL_DIV, 5193/PSG_VOL_DIV, 4125/PSG_VOL_DIV, 3277/PSG_VOL_DIV, 2603/PSG_VOL_DIV,
	2067/PSG_VOL_DIV, 1642/PSG_VOL_DIV, 1304/PSG_VOL_DIV, 0
};

void psg_run(psg_context * context, uint32_t cycles)
{
	while (context->cycles < cycles) {
		for (int i = 0; i < 4; i++) {
			if (context->counters[i]) {
				context->counters[i] -= 1;
			}
			if (!context->counters[i]) {
				context->counters[i] = context->counter_load[i];
				context->output_state[i] = !context->output_state[i];
				if (i == 3 && context->output_state[i]) {
					context->noise_out = context->lsfr & 1;
					context->lsfr = (context->lsfr >> 1) | (context->lsfr << 15);
					if (context->noise_type) {
						//white noise
						if (context->lsfr & 0x40) {
							context->lsfr ^= 0x8000;
						}
					}
				}
			}
		}

		for (int i = 0; i < 3; i++) {
			if (context->output_state[i]) {
				context->accum += volume_table[context->volume[i]];
			}
		}
		if (context->noise_out) {
			context->accum += volume_table[context->volume[3]];
		}
		context->sample_count++;

		context->buffer_fraction += context->buffer_inc;
		if (context->buffer_fraction >= BUFFER_INC_RES) {
			context->buffer_fraction -= BUFFER_INC_RES;

			context->audio_buffer[context->buffer_pos++] = context->accum / context->sample_count;
			context->accum = context->sample_count = 0;
			if (context->buffer_pos == context->samples_frame) {
				fwrite(context->audio_buffer, 1, context->buffer_pos, blah);
				if (!headless) {
					render_wait_psg(context);
				}
			}
		}
		context->cycles += context->clock_inc;
	}
}

