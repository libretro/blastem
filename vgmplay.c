#include "render.h"
#include "ym2612.h"
#include "psg.h"
#include <stdint.h>
#include <stdio.h>

#define MCLKS_NTSC 53693175
#define MCLKS_PAL  53203395

#define MCLKS_PER_68K 7
#define MCLKS_PER_YM  MCLKS_PER_68K
#define MCLKS_PER_Z80 15
#define MCLKS_PER_PSG (MCLKS_PER_Z80*16)

#pragma pack(push, 1)
typedef struct {
	char     ident[4];
	uint32_t eof_offset;
	uint32_t version;
	uint32_t sn76489_clk;
	uint32_t ym2413_clk;
	uint32_t gd3_offset;
	uint32_t num_samples;
	uint32_t loop_offset;
	uint32_t loop_count;
	uint32_t rate;
	uint16_t sn76489_fb;
	uint8_t  sn76489_shift;
	uint8_t  sn76489_flags;
	uint32_t ym2612_clk;
	uint32_t ym2151_clk;
	uint32_t data_offset;
	uint32_t sega_pcm_clk;
	uint32_t sega_pcm_reg;
} vgm_header;

enum {
	CMD_PSG_STEREO = 0x4F,
	CMD_PSG,
	CMD_YM2413,
	CMD_YM2612_0,
	CMD_YM2612_1,
	CMD_YM2151,
	CMD_YM2203,
	CMD_YM2608_0,
	CMD_YM2608_1,
	CMD_YM2610_0,
	CMD_YM2610_1,
	CMD_YM3812,
	CMD_YM3526,
	CMD_Y8950,
	CMD_YMZ280B,
	CMD_YMF262_0,
	CMD_YMF262_1,
	CMD_WAIT = 0x61,
	CMD_WAIT_60,
	CMD_WAIT_50,
	CMD_END = 0x66,
	CMD_DATA,
	CMD_PCM_WRITE,
	CMD_WAIT_SHORT = 0x70,
	CMD_YM2612_DAC = 0x80
};

#pragma pack(pop)

void handle_keydown(int keycode)
{
}

void handle_keyup(int keycode)
{
}

#define CYCLE_LIMIT MCLKS_NTSC/60

void wait(ym2612_context * y_context, psg_context * p_context, uint32_t * current_cycle, uint32_t cycles)
{
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
	uint32_t fps = 60;
	render_init(320, 240, "vgm play", 60);
	
	
	ym2612_context y_context;
	ym_init(&y_context, render_sample_rate(), MCLKS_NTSC, MCLKS_PER_YM, render_audio_buffer(), 0);
	
	psg_context p_context;
	psg_init(&p_context, render_sample_rate(), MCLKS_NTSC, MCLKS_PER_PSG, render_audio_buffer());
	
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
			wait(&y_context, &p_context, &current_cycle, wait_time);
			break;
		}
		case CMD_WAIT_60:
			wait(&y_context, &p_context, &current_cycle, 735 * mclks_sample);
			break;
		case CMD_WAIT_50:
			wait(&y_context, &p_context, &current_cycle, 882 * mclks_sample);
			break;
		case CMD_END:
			return 0;
		default:
			if (cmd >= CMD_WAIT_SHORT && cmd < (CMD_WAIT_SHORT + 0x10)) {
				uint32_t wait_time = (cmd & 0xF) + 1;
				wait_time *= mclks_sample;
				wait(&y_context, &p_context, &current_cycle, wait_time);
			} else {
				printf("unimplemented command: %X at offset %X\n", cmd, cur - data - 1);
				exit(1);
			}
		}
	}
	return 0;
}
