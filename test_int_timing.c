/*
 Copyright 2017 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdio.h>
#include "vdp.h"

int headless = 1;

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return 0;
}

uint16_t read_dma_value(uint32_t address)
{
	return 0;
}

uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
	*pitch = 0;
	return NULL;
}

void render_framebuffer_updated(uint8_t which, int width)
{
}

void warning(char *format, ...)
{
}


int main(int argc, char **argv)
{
	vdp_context context;
	int ret = 0;
	init_vdp_context(&context, 0);
	vdp_control_port_write(&context, 0x8000 | BIT_PAL_SEL);
	vdp_control_port_write(&context, 0x8100 | BIT_DISP_EN | BIT_VINT_EN | BIT_MODE_5);
	puts("Testing H32 Mode");
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
	}
	vdp_int_ack(&context);
	uint32_t vint_cycle = vdp_next_vint(&context);
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
		uint32_t vint_cycle2 = vdp_next_vint(&context);
		if (vint_cycle2 != vint_cycle) {
			printf("VINT Cycle changed from %d to %d @ line %d, slot %d\n", vint_cycle, vint_cycle2, context.vcounter, context.hslot);;
			ret = 1;
			vint_cycle = vint_cycle2;
		}
	}
	vdp_int_ack(&context);
	puts("Testing H40 Mode");
	vdp_control_port_write(&context, 0x8C81);
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
	}
	vdp_int_ack(&context);
	vint_cycle = vdp_next_vint(&context);
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
		uint32_t vint_cycle2 = vdp_next_vint(&context);
		if (vint_cycle2 != vint_cycle) {
			printf("VINT Cycle changed from %d to %d @ line %d, slot %d\n", vint_cycle, vint_cycle2, context.vcounter, context.hslot);;
			ret = 1;
			vint_cycle = vint_cycle2;
		}
	}
	vdp_int_ack(&context);
	puts("Testing Mode 4");
	vdp_control_port_write(&context, 0x8C00);
	vdp_control_port_write(&context, 0x8100 | BIT_DISP_EN | BIT_VINT_EN);
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
	}
	context.flags2 &= ~FLAG2_VINT_PENDING;
	vint_cycle = vdp_next_vint(&context);
	while (!(context.flags2 & FLAG2_VINT_PENDING))
	{
		vdp_run_context(&context, context.cycles + 1);
		uint32_t vint_cycle2 = vdp_next_vint(&context);
		if (vint_cycle2 != vint_cycle) {
			printf("VINT Cycle changed from %d to %d @ line %d, slot %d\n", vint_cycle, vint_cycle2, context.vcounter, context.hslot);;
			ret = 1;
			vint_cycle = vint_cycle2;
		}
	}
	printf("Result: %s\n", ret ? "failure" : "success");
	return ret;
}