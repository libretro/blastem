/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef PSG_CONTEXT_H_
#define PSG_CONTEXT_H_

#include <stdint.h>
#include "serialize.h"

typedef struct {
	int16_t  *audio_buffer;
	int16_t  *back_buffer;
	uint64_t buffer_fraction;
	uint64_t buffer_inc;
	uint32_t buffer_pos;
	uint32_t clock_inc;
	uint32_t cycles;
	uint32_t sample_rate;
	uint32_t samples_frame;
	int32_t lowpass_alpha;
	uint16_t lsfr;
	uint16_t counter_load[4];
	uint16_t counters[4];
	int16_t  accum;
	int16_t  last_sample;
	uint8_t  volume[4];
	uint8_t  output_state[4];
	uint8_t  noise_out;
	uint8_t  noise_use_tone;
	uint8_t  noise_type;
	uint8_t  latch;
} psg_context;


void psg_init(psg_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t samples_frame, uint32_t lowpass_cutoff);
void psg_free(psg_context *context);
void psg_adjust_master_clock(psg_context * context, uint32_t master_clock);
void psg_write(psg_context * context, uint8_t value);
void psg_run(psg_context * context, uint32_t cycles);
void psg_serialize(psg_context *context, serialize_buffer *buf);
void psg_deserialize(deserialize_buffer *buf, void *vcontext);

#endif //PSG_CONTEXT_H_

