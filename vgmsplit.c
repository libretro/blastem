/*
 Copyright 2015 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "ym2612.h"
#include "vgm.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_CHANNELS 10
#define DAC_CHANNEL 5
#define PSG_BASE 6
#define SAMPLE_THRESHOLD 100

uint32_t total_delay;
void accum_wait(uint32_t *delays, uint32_t samples)
{
	total_delay += samples;
	for (int i = 0; i < OUT_CHANNELS; i++)
	{
		delays[i] += samples;
	}
}

void write_wait(uint8_t **out_buffer, uint32_t *delay)
{
	while (*delay >= 65535)
	{
		*((*out_buffer)++) = CMD_WAIT;
		*((*out_buffer)++) = 0xFF;
		*((*out_buffer)++) = 0xFF;
		*delay -= 65535;
	}
	if (*delay) {
		if (*delay == 735) {
			*((*out_buffer)++) = CMD_WAIT_60;
		} else if (*delay > 735 && *delay <= 751) {
			*((*out_buffer)++) = CMD_WAIT_60;
			*((*out_buffer)++) = CMD_WAIT_SHORT + *delay - 736;
		} else if (*delay == 882) {
			*((*out_buffer)++) = CMD_WAIT_50;
		} else if (*delay > 882 && *delay <= 898) {
			*((*out_buffer)++) = CMD_WAIT_50;
			*((*out_buffer)++) = CMD_WAIT_SHORT + *delay - 882;
		} else if (*delay <= 16) {
			*((*out_buffer)++) = CMD_WAIT_SHORT + *delay - 1;
		} else {
			*((*out_buffer)++) = CMD_WAIT;
			*((*out_buffer)++) = *delay;
			*((*out_buffer)++) = *delay >> 8;
		}
		*delay = 0;
	}
}

int main(int argc, char ** argv)
{
	data_block *blocks = NULL;
	data_block *seek_block = NULL;
	uint32_t seek_offset;
	uint32_t block_offset;


	FILE * f = fopen(argv[1], "rb");
	vgm_header header;
	size_t bytes = fread(&header, 1, sizeof(header), f);
	if (bytes != sizeof(header)) {
		fputs("Error reading file\n", stderr);
		exit(1);
	}
	if (header.version < 0x150 || !header.data_offset) {
		header.data_offset = 0xC;
	}
	fseek(f, header.data_offset + 0x34, SEEK_SET);
	uint32_t data_size = header.eof_offset + 4 - (header.data_offset + 0x34);
	uint8_t * data = malloc(data_size);
	data_size = fread(data, 1, data_size, f);
	fclose(f);
	uint8_t *buffers[OUT_CHANNELS];
	uint8_t *out_pos[OUT_CHANNELS];
	uint8_t has_real_data[OUT_CHANNELS];
	uint32_t delay[OUT_CHANNELS];

	buffers[0] = malloc(data_size * OUT_CHANNELS);
	out_pos[0] = buffers[0];
	has_real_data[0] = 0;
	delay[0] = 0;
	for (int i = 1; i < OUT_CHANNELS; i++)
	{
		buffers[i] = buffers[i-1] + data_size;
		out_pos[i] = buffers[i];
		has_real_data[i] = 0;
		delay[i] = 0;
	}

	uint8_t * end = data + data_size;
	uint8_t * cur = data;
	uint32_t current_cycle = 0;
	uint8_t psg_latch = 0;
	uint8_t param,reg;
	uint8_t channel;
	uint32_t sample_count = 0;
	uint8_t last_cmd;
	while (cur < end) {
		uint8_t cmd = *(cur++);
		switch(cmd)
		{
		case CMD_PSG_STEREO:
			//ignore for now
			cur++;
			break;
		case CMD_PSG:
			param = *(cur++);
			if (param & 0x80) {
				psg_latch = param;
				channel = param >> 5 & 3;
			} else {
				channel = psg_latch >> 5 & 3;
			}
			write_wait(out_pos + PSG_BASE+channel, delay + PSG_BASE+channel);
			*(out_pos[PSG_BASE+channel]++) = cmd;
			*(out_pos[PSG_BASE+channel]++) = param;
			has_real_data[PSG_BASE+channel] = 1;
			break;
		case CMD_YM2612_0:
			reg = *(cur++);
			param = *(cur++);
			if (reg < REG_KEY_ONOFF) {
				for (int i = 0; i < 6; i++)
				{
					write_wait(out_pos + i, delay + i);
					*(out_pos[i]++) = cmd;
					*(out_pos[i]++) = reg;
					*(out_pos[i]++) = param;
				}
				break;
			} else if(reg == REG_DAC || reg == REG_DAC_ENABLE) {
				if (reg == REG_DAC) {
					sample_count++;
				}
				channel = DAC_CHANNEL;
			} else if(reg == REG_KEY_ONOFF) {
				channel = param & 7;
				if (channel > 2) {
					channel--;
				}
				if (param & 0xF0) {
					has_real_data[channel] = 1;
				}
			} else if (reg >= REG_FNUM_LOW_CH3 && reg < REG_ALG_FEEDBACK) {
				channel = 2;
			} else {
				channel = 255;
			}
		case CMD_YM2612_1:
			if (cmd == CMD_YM2612_1) {
				reg = *(cur++);
				param = *(cur++);
				channel = 255;
			}
			if (channel >= PSG_BASE) {
				if (reg >= REG_DETUNE_MULT && reg < REG_FNUM_LOW) {
					channel = (cmd == CMD_YM2612_0 ? 0 : 3) + (reg & 0xC >> 2);
				} else if ((reg >= REG_FNUM_LOW && reg < REG_FNUM_LOW_CH3) || (reg >= REG_ALG_FEEDBACK && reg < 0xC0)) {
					channel = (cmd == CMD_YM2612_0 ? 0 : 3) + (reg & 0x3);
				} else {
					fprintf(stderr, "WARNING: Skipping nrecognized write to register %X on part %d\n", reg, (cmd == CMD_YM2612_0 ? 1 : 2));
				}
			}
			if (channel < PSG_BASE) {
				write_wait(out_pos + channel, delay + channel);
				*(out_pos[channel]++) = cmd;
				*(out_pos[channel]++) = reg;
				*(out_pos[channel]++) = param;
			}
			break;
		case CMD_WAIT: {
			uint32_t wait_time = *(cur++);
			wait_time |= *(cur++) << 8;
			accum_wait(delay, wait_time);
			break;
		}
		case CMD_WAIT_60:
			accum_wait(delay, 735);
			break;
		case CMD_WAIT_50:
			accum_wait(delay, 882);
			break;
		case CMD_END:
			for (int i = 0; i < OUT_CHANNELS; i++)
			{
				write_wait(out_pos + i, delay + i);
				*(out_pos[i]++) = cmd;
			}
			cur = end;
			break;
		case CMD_DATA: {
			uint8_t * start = cur - 1;
			cur++; //skip compat command
			uint8_t data_type = *(cur++);
			uint32_t data_size = *(cur++);
			data_size |= *(cur++) << 8;
			data_size |= *(cur++) << 16;
			data_size |= *(cur++) << 24;
			if (cur + data_size > end) {
				data_size = end - cur;
			}
			cur += data_size;
			if (data_type == DATA_YM2612_PCM) {
				write_wait(out_pos + DAC_CHANNEL, delay + DAC_CHANNEL);
				memcpy(out_pos[DAC_CHANNEL], start, cur-start);
				out_pos[DAC_CHANNEL] += cur-start;
			} else {
				fprintf(stderr, "WARNING: Skipping data block with unrecognized type %X\n", data_type);
			}
			break;
		}
		case CMD_DATA_SEEK: {
			write_wait(out_pos + DAC_CHANNEL, delay + DAC_CHANNEL);
			memcpy(out_pos[DAC_CHANNEL], cur-1, 5);
			out_pos[DAC_CHANNEL] += 5;
			cur += 4;
			break;
		}

		default:
			if (cmd >= CMD_WAIT_SHORT && cmd < (CMD_WAIT_SHORT + 0x10)) {
				accum_wait(delay, (cmd & 0xF) + 1);
			} else if (cmd >= CMD_YM2612_DAC && cmd < CMD_DAC_STREAM_SETUP) {
				write_wait(out_pos + DAC_CHANNEL, delay + DAC_CHANNEL);
				*(out_pos[DAC_CHANNEL]++) = cmd;
				for (int i = 0; i < OUT_CHANNELS; i++)
				{
					if (i != DAC_CHANNEL)
					{
						delay[i] += cmd & 0xF;
					}
				}
				sample_count++;
			} else {
				fprintf(stderr, "unimplemented command: %X at offset %X, last valid command was %X\n", cmd, (unsigned int)(cur - data - 1), last_cmd);
				exit(1);
			}
		}
		last_cmd = cmd;
	}
	if (sample_count > SAMPLE_THRESHOLD) {
		has_real_data[DAC_CHANNEL] = 1;
	}
	for (int i = 0; i < OUT_CHANNELS; i++)
	{
		if (has_real_data[i]) {
			char fname[11];
			sprintf(fname, i < PSG_BASE ? "ym_%d.vgm" : "psg_%d.vgm", i < PSG_BASE ? i : i - PSG_BASE);
			f = fopen(fname, "wb");
			if (!f) {
				fprintf(stderr, "Failed to open %s for writing\n", fname);
				exit(1);
			}
			data_size = out_pos[i] - buffers[i];
			header.eof_offset = (header.data_offset + 0x34) + data_size - 4;
			header.gd3_offset = 0;
			fwrite(&header, 1, sizeof(header), f);
			fseek(f, header.data_offset + 0x34, SEEK_SET);
			fwrite(buffers[i], 1, data_size, f);
			fclose(f);
		}
	}
	printf("total_delay: %d\n", total_delay);
	return 0;
}
