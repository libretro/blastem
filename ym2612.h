/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef YM2612_H_
#define YM2612_H_

#include <stdint.h>
#include <stdio.h>

#define NUM_PART_REGS (0xB7-0x30)
#define NUM_CHANNELS 6
#define NUM_OPERATORS (4*NUM_CHANNELS)

#define YM_OPT_WAVE_LOG 1

typedef struct {
	uint32_t phase_inc;
	uint32_t phase_counter;
	uint16_t envelope;
	int16_t  output;
	uint16_t total_level;
	uint16_t sustain_level;
	uint8_t  rates[4];
	uint8_t  key_scaling;
	uint8_t  multiple;
	uint8_t  detune;
	uint8_t  am;
	uint8_t  env_phase;
} ym_operator;

typedef struct {
	FILE *   logfile;
	uint16_t fnum;
	int16_t  output;
	int16_t  op1_old;
	uint8_t  block_fnum_latch;
	uint8_t  block;
	uint8_t  keycode;
	uint8_t  algorithm;
	uint8_t  feedback;
	uint8_t  ams;
	uint8_t  pms;
	uint8_t  lr;
} ym_channel;

typedef struct {
	uint16_t fnum;
	uint8_t  block;
	uint8_t  block_fnum_latch;
	uint8_t  keycode;
} ym_supp;

#define YM_PART1_START 0x21
#define YM_PART2_START 0x30
#define YM_REG_END     0xB8
#define YM_PART1_REGS (YM_REG_END-YM_PART1_START)
#define YM_PART2_REGS (YM_REG_END-YM_PART2_START)

typedef struct {
    int16_t     *audio_buffer;
    int16_t     *back_buffer;
    uint64_t    buffer_fraction;
    uint64_t    buffer_inc;
    uint32_t    clock_inc;
    uint32_t    buffer_pos;
	uint32_t    sample_rate;
    uint32_t    sample_limit;
	uint32_t    current_cycle;
	//TODO: Condense the next two fields into one
	uint32_t    write_cycle;
	uint32_t    busy_cycles;
	ym_operator operators[NUM_OPERATORS];
	ym_channel  channels[NUM_CHANNELS];
	uint16_t    timer_a;
	uint16_t    timer_a_load;
	uint16_t    env_counter;
	ym_supp     ch3_supp[3];
	uint8_t     timer_b;
	uint8_t     sub_timer_b;
	uint8_t     timer_b_load;
	uint8_t     ch3_mode;
	uint8_t     current_op;
	uint8_t     current_env_op;

	uint8_t     timer_control;
	uint8_t     dac_enable;
	uint8_t     lfo_enable;
	uint8_t     lfo_freq;
	uint8_t     lfo_counter;
	uint8_t     lfo_am_step;
	uint8_t     lfo_pm_step;
	uint8_t     status;
	uint8_t     selected_reg;
	uint8_t     selected_part;
	uint8_t     part1_regs[YM_PART1_REGS];
	uint8_t     part2_regs[YM_PART2_REGS];
} ym2612_context;

void ym_init(ym2612_context * context, uint32_t sample_rate, uint32_t master_clock, uint32_t clock_div, uint32_t sample_limit, uint32_t options);
void ym_adjust_master_clock(ym2612_context * context, uint32_t master_clock);
void ym_run(ym2612_context * context, uint32_t to_cycle);
void ym_address_write_part1(ym2612_context * context, uint8_t address);
void ym_address_write_part2(ym2612_context * context, uint8_t address);
void ym_data_write(ym2612_context * context, uint8_t value);
uint8_t ym_read_status(ym2612_context * context);
uint8_t ym_load_gst(ym2612_context * context, FILE * gstfile);
uint8_t ym_save_gst(ym2612_context * context, FILE * gstfile);
void ym_print_channel_info(ym2612_context *context, int channel);

#endif //YM2612_H_

