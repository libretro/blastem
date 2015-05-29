#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "vdp.h"

int headless = 1;
uint16_t read_dma_value(uint32_t address)
{
	return 0;
}

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return 0;
}

void render_alloc_surfaces(vdp_context * context)
{
	context->oddbuf = context->framebuf = malloc(512 * 256 * 4 * 2);
	memset(context->oddbuf, 0, 512 * 256 * 4 * 2);
	context->evenbuf = ((char *)context->oddbuf) + 512 * 256 * 4;
}

int check_hint_time(vdp_context * v_context)
{
	uint32_t orig_hint_cycle = vdp_next_hint(v_context);
	uint32_t cur_hint_cycle;
	printf("hint cycle is %d at vcounter: %d, hslot: %d\n", orig_hint_cycle, v_context->vcounter, v_context->hslot);
	int res = 1;
	while ((cur_hint_cycle = vdp_next_hint(v_context)) > v_context->cycles)
	{
		if (cur_hint_cycle != orig_hint_cycle) {
			fprintf(stderr, "ERROR: hint cycle changed to %d at vcounter: %d, hslot: %d\n", cur_hint_cycle, v_context->vcounter, v_context->hslot);
			orig_hint_cycle = cur_hint_cycle;
			res = 0;
		}
		vdp_run_context(v_context, v_context->cycles + 1);
	}
	printf("hint fired at cycle: %d, vcounter: %d, hslot: %d\n", cur_hint_cycle, v_context->vcounter, v_context->hslot);
	vdp_int_ack(v_context, 4);
	return res;
}


int main(int argc, char ** argv)
{
	vdp_context v_context;
	init_vdp_context(&v_context, 0);
	vdp_control_port_write(&v_context, 0x8144);
	vdp_control_port_write(&v_context, 0x8C81);
	vdp_control_port_write(&v_context, 0x8A7F);
	vdp_control_port_write(&v_context, 0x8014);
	v_context.hint_counter = 0x7F;
	v_context.vcounter = 128;
	v_context.hslot = 165;
	//check single shot behavior
	int res = check_hint_time(&v_context);
	//check every line behavior
	while (v_context.vcounter < 225)
	{
		vdp_run_context(&v_context, v_context.cycles + 1);
	}
	vdp_control_port_write(&v_context, 0x8A00);
	int hint_count = 0;
	while (res && v_context.vcounter != 224)
	{
		res = res && check_hint_time(&v_context);
		hint_count++;
	}
	if (res && hint_count != 225) {
		fprintf(stderr, "ERROR: hint count should be 225 but was %d instead\n", hint_count);
		res = 0;
	}
	return 0;
}
