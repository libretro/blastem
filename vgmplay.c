/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "render.h"
#include "ym2612.h"
#include "psg.h"
#include "config.h"
#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vgm.h"
#include "system.h"

#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

#define MCLKS_PER_68K 7
#define MCLKS_PER_YM  MCLKS_PER_68K
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)

system_header *current_system;

void handle_keydown(int keycode)
{
}

void handle_keyup(int keycode)
{
}

void handle_joydown(int joystick, int button)
{
}

void handle_joyup(int joystick, int button)
{
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
}

void handle_joy_axis(int joystick, int axis, int16_t value)
{
}

void handle_joy_added(int joystick)
{
}

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
}

void handle_mousedown(int mouse, int button)
{
}

void handle_mouseup(int mouse, int button)
{
}

int headless = 0;

#define CYCLE_LIMIT MCLKS_NTSC/60
#define MAX_SOUND_CYCLES 100000
tern_node * config;

void vgm_wait(ym2612_context * y_context, psg_context * p_context, uint32_t * current_cycle, uint32_t cycles)
{
	while (cycles > MAX_SOUND_CYCLES)
	{
		vgm_wait(y_context, p_context, current_cycle, MAX_SOUND_CYCLES);
		cycles -= MAX_SOUND_CYCLES;
	}
	*current_cycle += cycles;
	psg_run(p_context, *current_cycle);
	ym_run(y_context, *current_cycle);

	if (*current_cycle > CYCLE_LIMIT) {
		*current_cycle -= CYCLE_LIMIT;
		p_context->cycles -= CYCLE_LIMIT;
		y_context->current_cycle -= CYCLE_LIMIT;
		process_events();
	}
}

int main(int argc, char ** argv)
{
	set_exe_str(argv[0]);
	data_block *blocks = NULL;
	data_block *seek_block = NULL;
	uint32_t seek_offset;
	uint32_t block_offset;

	uint32_t fps = 60;
	config = load_config(argv[0]);
	render_init(320, 240, "vgm play", 0);

	uint32_t opts = 0;
	if (argc >= 3 && !strcmp(argv[2], "-y")) {
		opts |= YM_OPT_WAVE_LOG;
	}
	
	char * lowpass_cutoff_str = tern_find_path(config, "audio\0lowpass_cutoff\0", TVAL_PTR).ptrval;
	uint32_t lowpass_cutoff = lowpass_cutoff_str ? atoi(lowpass_cutoff_str) : 3390;

	ym2612_context y_context;
	ym_init(&y_context, MCLKS_NTSC, MCLKS_PER_YM, opts);

	psg_context p_context;
	psg_init(&p_context, MCLKS_NTSC, MCLKS_PER_PSG);

	FILE * f = fopen(argv[1], "rb");
	vgm_header header;
	fread(&header, sizeof(header), 1, f);
	if (header.version < 0x150 || !header.data_offset) {
		header.data_offset = 0xC;
	}
	fseek(f, header.data_offset + 0x34, SEEK_SET);
	uint32_t data_size = header.eof_offset + 4 - (header.data_offset + 0x34);
	uint8_t * data = malloc(data_size);
	fread(data, 1, data_size, f);
	fclose(f);

	uint32_t mclks_sample = MCLKS_NTSC / 44100;
	uint32_t loop_count = 2;

	uint8_t * end = data + data_size;
	uint8_t * cur = data;
	uint32_t current_cycle = 0;
	while (cur < end) {
		uint8_t cmd = *(cur++);
		switch(cmd)
		{
		case CMD_PSG_STEREO:
			//ignore for now
			cur++;
			break;
		case CMD_PSG:
			psg_write(&p_context, *(cur++));
			break;
		case CMD_YM2612_0:
			ym_address_write_part1(&y_context, *(cur++));
			ym_data_write(&y_context, *(cur++));
			break;
		case CMD_YM2612_1:
			ym_address_write_part2(&y_context, *(cur++));
			ym_data_write(&y_context, *(cur++));
			break;
		case CMD_WAIT: {
			uint32_t wait_time = *(cur++);
			wait_time |= *(cur++) << 8;
			wait_time *= mclks_sample;
			vgm_wait(&y_context, &p_context, &current_cycle, wait_time);
			break;
		}
		case CMD_WAIT_60:
			vgm_wait(&y_context, &p_context, &current_cycle, 735 * mclks_sample);
			break;
		case CMD_WAIT_50:
			vgm_wait(&y_context, &p_context, &current_cycle, 882 * mclks_sample);
			break;
		case CMD_END:
			if (header.loop_offset && --loop_count) {
				cur = data + header.loop_offset + 0x1C - (header.data_offset + 0x34);
			} else {
				//TODO: fade out
				return 0;
			}
			break;
		case CMD_DATA: {
			cur++; //skip compat command
			uint8_t data_type = *(cur++);
			uint32_t data_size = *(cur++);
			data_size |= *(cur++) << 8;
			data_size |= *(cur++) << 16;
			data_size |= *(cur++) << 24;
			if (data_type == DATA_YM2612_PCM) {
				data_block ** curblock = &blocks;
				while(*curblock)
				{
					curblock = &((*curblock)->next);
				}
				*curblock = malloc(sizeof(data_block));
				(*curblock)->size = data_size;
				(*curblock)->type = data_type;
				(*curblock)->data = cur;
				(*curblock)->next = NULL;
			} else {
				fprintf(stderr, "Skipping data block with unrecognized type %X\n", data_type);
			}
			cur += data_size;
			break;
		}
		case CMD_DATA_SEEK: {
			uint32_t new_offset = *(cur++);
			new_offset |= *(cur++) << 8;
			new_offset |= *(cur++) << 16;
			new_offset |= *(cur++) << 24;
			if (!seek_block || new_offset < seek_offset) {
				seek_block = blocks;
				seek_offset = 0;
				block_offset = 0;
			}
			while (seek_block && (seek_offset - block_offset + seek_block->size) < new_offset)
			{
				seek_offset += seek_block->size - block_offset;
				seek_block = seek_block->next;
				block_offset = 0;
			}
			block_offset += new_offset-seek_offset;
			seek_offset = new_offset;
			break;
		}

		default:
			if (cmd >= CMD_WAIT_SHORT && cmd < (CMD_WAIT_SHORT + 0x10)) {
				uint32_t wait_time = (cmd & 0xF) + 1;
				wait_time *= mclks_sample;
				vgm_wait(&y_context, &p_context, &current_cycle, wait_time);
			} else if (cmd >= CMD_YM2612_DAC && cmd < CMD_DAC_STREAM_SETUP) {
				if (seek_block) {
					ym_address_write_part1(&y_context, 0x2A);
					ym_data_write(&y_context, seek_block->data[block_offset++]);
					seek_offset++;
					if (block_offset > seek_block->size) {
						seek_block = seek_block->next;
						block_offset = 0;
					}
				} else {
					fputs("Encountered DAC write command but data seek pointer is invalid!\n", stderr);
				}
				uint32_t wait_time = (cmd & 0xF);
				if (wait_time)
				{
					wait_time *= mclks_sample;
					vgm_wait(&y_context, &p_context, &current_cycle, wait_time);
				}
			} else {
				fatal_error("unimplemented command: %X at offset %X\n", cmd, (unsigned int)(cur - data - 1));
			}
		}
	}
	return 0;
}
