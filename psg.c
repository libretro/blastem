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
#include <math.h>
void psg_init(psg_context * context, uint32_t master_clock, uint32_t clock_div)
{
	memset(context, 0, sizeof(*context));
	context->audio = render_audio_source(master_clock, clock_div, 1);
	context->clock_inc = clock_div;
	for (int i = 0; i < 4; i++) {
		context->volume[i] = 0xF;
	}
}

void psg_free(psg_context *context)
{
	render_free_source(context->audio);
	free(context);
}

void psg_adjust_master_clock(psg_context * context, uint32_t master_clock)
{
	render_audio_adjust_clock(context->audio, master_clock, context->clock_inc);
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
static int16_t volume_table[16] = {
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

		int16_t accum = 0;
		
		for (int i = 0; i < 3; i++) {
			if (context->output_state[i]) {
				accum += volume_table[context->volume[i]];
			}
		}
		if (context->noise_out) {
			accum += volume_table[context->volume[3]];
		}
		
		render_put_mono_sample(context->audio, accum);

		context->cycles += context->clock_inc;
	}
}

void psg_serialize(psg_context *context, serialize_buffer *buf)
{
	save_int16(buf, context->lsfr);
	save_buffer16(buf, context->counter_load, 4);
	save_buffer16(buf, context->counters, 4);
	save_buffer8(buf, context->volume, 4);
	uint8_t output_state = context->output_state[0] << 3 | context->output_state[1] << 2
		| context->output_state[2] << 1 | context->output_state[3]
		| context->noise_use_tone << 4;
	save_int8(buf, output_state);
	save_int8(buf, context->noise_type);
	save_int8(buf, context->latch);
	save_int32(buf, context->cycles);
}

void psg_deserialize(deserialize_buffer *buf, void *vcontext)
{
	psg_context *context = vcontext;
	context->lsfr = load_int16(buf);
	load_buffer16(buf, context->counter_load, 4);
	load_buffer16(buf, context->counters, 4);
	load_buffer8(buf, context->volume, 4);
	uint8_t output_state = load_int8(buf);
	context->output_state[0] = output_state >> 3 & 1;
	context->output_state[1] = output_state >> 2 & 1;
	context->output_state[2] = output_state >> 1 & 1;
	context->output_state[3] = output_state & 1;
	context->noise_use_tone = output_state >> 4 & 1;
	context->noise_type = load_int8(buf);
	context->latch = load_int8(buf);
	context->cycles = load_int32(buf);
}
