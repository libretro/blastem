#include <stdlib.h>
#include <stdio.h>
#include "vdp.h"
#include "render.h"
#include "blastem.h"

//not used, but referenced by the renderer since it handles input
io_port gamepad_1;
io_port gamepad_2;

uint16_t read_dma_value(uint32_t address)
{
	return 0;
}

int main(int argc, char ** argv)
{
	if (argc < 2) {
		fprintf(stderr, "Usage: stateview FILENAME\n");
		exit(1);
	}
	FILE * state_file = fopen(argv[1], "rb");
	if (!state_file) {
		fprintf(stderr, "Failed to open %s\n", argv[1]);
		exit(1);
	}
	int width = 320;
	int height = 240;
	if (argc > 2) {
		width = atoi(argv[2]);
		if (argc > 3) {
			height = atoi(argv[3]);
		} else {
			height = (width/320) * 240;
		}
	}
	vdp_context context;
	init_vdp_context(&context);
	vdp_load_savestate(&context, state_file);
	vdp_run_to_vblank(&context);
	printf("Display %s\n", (context.regs[REG_MODE_2] & DISPLAY_ENABLE) ? "enabled" : "disabled");
    render_init(width, height);
    render_context(&context);
    render_wait_quit(&context);
    return 0;
}
