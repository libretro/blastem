/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "z80inst.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

uint8_t visited[(64*1024)/8];
uint8_t label[(64*1024)/8];

void fatal_error(char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}

void visit(uint16_t address)
{
	visited[address/8] |= 1 << (address % 8);
}

void reference(uint16_t address)
{
	//printf("referenced: %X\n", address);
	label[address/8] |= 1 << (address % 8);
}

uint8_t is_visited(uint16_t address)
{
	return visited[address/8] & (1 << (address % 8));
}

uint8_t is_label(uint16_t address)
{
	return label[address/8] & (1 << (address % 8));
}

typedef struct deferred {
	uint16_t address;
	struct deferred *next;
} deferred;

deferred * defer(uint16_t address, deferred * next)
{
	if (is_visited(address)) {
		return next;
	}
	//printf("deferring %X\n", address);
	deferred * d = malloc(sizeof(deferred));
	d->address = address;
	d->next = next;
	return d;
}

uint8_t labels = 0;
uint8_t addr = 0;
uint8_t only = 0;

int main(int argc, char ** argv)
{
	long filesize;
	uint8_t *filebuf;
	char disbuf[1024];
	z80inst instbuf;
	uint8_t * cur;
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	filebuf = malloc(filesize);
	fread(filebuf, 1, filesize, f);
	fclose(f);
	deferred *def = NULL, *tmpd;
	uint16_t offset = 0;
	for(uint8_t opt = 2; opt < argc; ++opt) {
		if (argv[opt][0] == '-') {
			FILE * address_log;
			switch (argv[opt][1])
			{
			case 'l':
				labels = 1;
				break;
			case 'a':
				addr = 1;
				break;
			case 'o':
				only = 1;
				break;
			case 'f':
				opt++;
				if (opt >= argc) {
					fputs("-f must be followed by a filename\n", stderr);
					exit(1);
				}
				address_log = fopen(argv[opt], "r");
				if (!address_log) {
					fprintf(stderr, "Failed to open %s for reading\n", argv[opt]);
					exit(1);
				}
				while (fgets(disbuf, sizeof(disbuf), address_log)) {
				 	if (disbuf[0]) {
						uint16_t address = strtol(disbuf, NULL, 16);
						if (address) {
							def = defer(address, def);
							reference(address);
						}
					}
				}
				fclose(address_log);
				break;
			case 's':
				opt++;
				if (opt >= argc) {
					fputs("-s must be followed by a start offset in hex\n", stderr);
					exit(1);
				}
				offset = strtol(argv[opt], NULL, 16);
				break;
			}
		} else {
			uint16_t address = strtol(argv[opt], NULL, 16);
			def = defer(address, def);
			reference(address);
		}
	}
	uint16_t start = offset;
	uint8_t *encoded, *next;
	uint32_t size;
	if (!def || !only) {
		def = defer(start, def);
	}
	uint16_t address;
	while(def) {
		do {
			encoded = NULL;
			address = def->address;
			if (!is_visited(address)) {
				encoded = filebuf + address - offset;
			}
			tmpd = def;
			def = def->next;
			free(tmpd);
		} while(def && encoded == NULL);
		if (!encoded) {
			break;
		}
		for(;;) {
			if ((address - offset) > filesize || is_visited(address) || address < offset) {
				break;
			}
			visit(address);
			next = z80_decode(encoded, &instbuf);
			address += (next-encoded);
			encoded = next;
			
			//z80_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", address, disbuf);
			switch (instbuf.op)
			{
			case Z80_JR:
				address += instbuf.immed;
				encoded = filebuf + address - offset;
				break;
			case Z80_JRCC:
				reference(address + instbuf.immed);
				def = defer(address + instbuf.immed, def);
				break;
			case Z80_JP:
				address = instbuf.immed;
				encoded = filebuf + address - offset;
				break;
			case Z80_JPCC:
			case Z80_CALL:
			case Z80_CALLCC:
			case Z80_RST:
				reference(instbuf.immed);
				def = defer(instbuf.immed, def);
				break;
			default:
				if (z80_is_terminal(&instbuf)) {
					address = filesize + 1;
				}
			}
		}
	}
	if (labels) {
		for (address = filesize; address < (64*1024); address++) {
			if (is_label(address)) {
				printf("ADR_%X equ $%X\n", address, address);
			}
		}
		puts("");
	}
	for (address = offset; address < filesize + offset; address++) {
		if (is_visited(address)) {
			encoded = filebuf + address - offset;
			z80_decode(encoded, &instbuf);
			if (labels) {
				/*m68k_disasm_labels(&instbuf, disbuf);
				if (is_label(instbuf.address)) {
					printf("ADR_%X:\n", instbuf.address);
				}
				if (addr) {
					printf("\t%s\t;%X\n", disbuf, instbuf.address);
				} else {
					printf("\t%s\n", disbuf);
				}*/
			} else {
				z80_disasm(&instbuf, disbuf, address);
				printf("%X: %s\n", address, disbuf);
			}
		}
	}
	return 0;
}
