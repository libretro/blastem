#include "z80inst.h"
#include "z80_to_x86.h"
#include "mem.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t z80_ram[0x2000];
uint16_t cart[0x200000];

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	x86_z80_options opts;
	z80_context context;
	if (argc < 2) {
		fputs("usage: transz80 zrom [cartrom]\n", stderr);
		exit(1);
	}
	FILE * f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "unable to open file %s\n", argv[2]);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	fread(z80_ram, 1, filesize < sizeof(z80_ram) ? filesize : sizeof(z80_ram), f);
	fclose(f);
	if (argc > 2) {
		f = fopen(argv[2], "rb");
		if (!f) {
			fprintf(stderr, "unable to open file %s\n", argv[2]);
			exit(1);
		}
		fseek(f, 0, SEEK_END);
		filesize = ftell(f);
		fseek(f, 0, SEEK_SET);
		fread(cart, 1, filesize < sizeof(cart) ? filesize : sizeof(cart), f);
		fclose(f);
	}
	init_x86_z80_opts(&opts);
	init_z80_context(&context, &opts);
	//Z80 RAM
	context.mem_pointers[0] = z80_ram;
	context.sync_cycle = context.target_cycle = 0x7FFFFFFF;
	//cartridge/bank
	context.mem_pointers[1] = context.mem_pointers[2] = cart;
	z80_reset(&context);
	for(;;)
	{
		z80_run(&context);
	}
	return 0;
}
