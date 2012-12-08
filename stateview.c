#include <stdlib.h>
#include <stdio.h>
#include "vdp.h"
#include "render.h"

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
	vdp_context context;
	init_vdp_context(&context);
	vdp_load_savestate(&context, state_file);
	vdp_run_to_vblank(&context);
    render_init();
    render_context(&context);
    render_wait_quit();
    return 0;
}
