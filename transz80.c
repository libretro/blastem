#include "z80inst.h"
#include "z80_to_x86.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t z80_ram[0x2000];

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	x86_z80_options opts;
	z80_context context;
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	fread(z80_ram, 1, filesize < sizeof(z80_ram) ? filesize : sizeof(z80_ram), f);
	fclose(f);
	init_x86_z80_opts(&opts);
	init_z80_context(&context, &opts);
	//cartridge ROM
	context.mem_pointers[0] = z80_ram;
	context.sync_cycle = context.target_cycle = 0x7FFFFFFF;
	//work RAM
	context.mem_pointers[1] = context.mem_pointers[2] = NULL;
	z80_reset(&context);
	for(;;)
	{
		z80_run(&context);
	}
	return 0;
}
