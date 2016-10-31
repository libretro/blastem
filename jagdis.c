/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "tern.h"
#include "util.h"
#include "jagcpu.h"

uint8_t visited[(16*1024*1024)/16];
uint16_t label[(16*1024*1024)/8];

void fatal_error(char *format, ...)
{
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}


void visit(uint32_t address)
{
	address &= 0xFFFFFF;
	visited[address/16] |= 1 << ((address / 2) % 8);
}

void reference(uint32_t address)
{
	address &= 0xFFFFFF;
	//printf("referenced: %X\n", address);
	label[address/16] |= 1 << (address % 16);
}

uint8_t is_visited(uint32_t address)
{
	address &= 0xFFFFFF;
	return visited[address/16] & (1 << ((address / 2) % 8));
}

uint16_t is_label(uint32_t address)
{
	address &= 0xFFFFFF;
	return label[address/16] & (1 << (address % 16));
}

typedef struct {
	uint32_t num_labels;
	uint32_t storage;
	char     *labels[];
} label_names;

tern_node * add_label(tern_node * head, char * name, uint32_t address)
{
	char key[MAX_INT_KEY_SIZE];
	address &= 0xFFFFFF;
	reference(address);
	tern_int_key(address, key);
	label_names * names = tern_find_ptr(head, key);
	if (names)
	{
		if (names->num_labels == names->storage)
		{
			names->storage = names->storage + (names->storage >> 1);
			names = realloc(names, sizeof(label_names) + names->storage * sizeof(char *));
		}
	} else {
		names = malloc(sizeof(label_names) + 4 * sizeof(char *));
		names->num_labels = 0;
		names->storage = 4;
		head = tern_insert_ptr(head, key, names);
	}
	names->labels[names->num_labels++] = strdup(name);
	return head;
}

typedef struct deferred {
	uint32_t address;
	struct deferred *next;
} deferred;

deferred * defer(uint32_t address, deferred * next)
{
	if (is_visited(address) || address & 1) {
		return next;
	}
	//printf("deferring %X\n", address);
	deferred * d = malloc(sizeof(deferred));
	d->address = address;
	d->next = next;
	return d;
}

void check_reference(uint16_t inst, uint32_t address, uint8_t is_gpu)
{
	if (jag_opcode(inst, is_gpu) == JAG_JR) {
		reference(jag_jr_dest(inst, address));
	}
}

char * strip_ws(char * text)
{
	while (*text && (!isprint(*text) || isblank(*text)))
	{
		text++;
	}
	char * ret = text;
	text = ret + strlen(ret) - 1;
	while (text > ret && (!isprint(*text) || isblank(*text)))
	{
		*text = 0;
		text--;
	}
	return ret;
}

int main(int argc, char ** argv)
{
	long filesize;
	unsigned short *filebuf;
	char disbuf[1024];
	unsigned short * cur;
	deferred *def = NULL, *tmpd;
	uint32_t start = 0xFFFFFFFF;

	uint8_t labels = 0, addr = 0, only = 0, vos = 0, reset = 0;
	tern_node * named_labels = NULL;

	uint32_t address_off = 0xFFFFFFFF, address_end;
	uint8_t is_gpu = 1;
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
			case 'r':
				reset = 1;
				break;
			case 's':
				opt++;
				if (opt >= argc) {
					fputs("-s must be followed by an offset\n", stderr);
					exit(1);
				}
				address_off = strtol(argv[opt], NULL, 0);
				break;
			case 'p':
				opt++;
				if (opt >= argc) {
					fputs("-p must be followed by a starting PC value\n", stderr);
					exit(1);
				}
				start = strtol(argv[opt], NULL, 0);
				break;
			case 'd':
				is_gpu = 0;
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
						char *end;
						uint32_t address = strtol(disbuf, &end, 16);
						if (address) {
							def = defer(address, def);
							reference(address);
							if (*end == '=') {
								named_labels = add_label(named_labels, strip_ws(end+1), address);
							}
						}
					}
				}
			}
		} else {
			char *end;
			uint32_t address = strtol(argv[opt], &end, 16);
			def = defer(address, def);
			reference(address);
			if (*end == '=') {
				named_labels = add_label(named_labels, end+1, address);
			}
		}
	}
	if (address_off == 0xFFFFFFFF) {
		address_off = is_gpu ? 0xF03000 : 0xF1B000;
	}
	if (start == 0xFFFFFFFF) {
		start = address_off;
	}
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);

	char int_key[MAX_INT_KEY_SIZE];
	address_end = address_off + filesize;
	filebuf = malloc(filesize);
	if (fread(filebuf, 2, filesize/2, f) != filesize/2)
	{
		fprintf(stderr, "Failure while reading file %s\n", argv[1]);
	}
	fclose(f);
	for(cur = filebuf; cur - filebuf < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	named_labels = add_label(named_labels, "start", start);
	if (!def || !only) {
		def = defer(start, def);
	}
		
	uint16_t *encoded, *next;
	uint32_t size, tmp_addr;
	uint32_t address;
	while(def) {
		do {
			encoded = NULL;
			address = def->address;
			if (!is_visited(address)) {
				encoded = filebuf + (address - address_off)/2;
			}
			tmpd = def;
			def = def->next;
			free(tmpd);
		} while(def && encoded == NULL);
		if (!encoded) {
			break;
		}
		for(;;) {
			if (address > address_end || address < address_off) {
				break;
			}
			visit(address);
			uint16_t inst = *encoded;
			uint32_t inst_address = address;
			check_reference(inst, address, is_gpu);
			uint16_t opcode = jag_opcode(inst, is_gpu);
			if (opcode == JAG_MOVEI) {
				address += 6;
				encoded += 3;
			} else {
				address += 2;
				encoded++;
			}
			

			if (opcode == JAG_JR || opcode == JAG_JUMP) {
				if (!(jag_reg2(inst) & 0xF)) {
					//unconditional jump
					if (opcode == JAG_JR) {
						address = jag_jr_dest(inst, inst_address);
						reference(address);
						if (is_visited(address)) {
							break;
						}
					} else {
						break;
					}
				} else if (opcode == JAG_JR) {
					uint32_t dest = jag_jr_dest(inst, inst_address);
					reference(dest);
					def = defer(dest, def);
				}
			}
		}
	}
	if (labels) {
		for (address = 0; address < address_off; address++) {
			if (is_label(address)) {
				printf("ADR_%X equ $%X\n", address, address);
			}
		}
		for (address = filesize; address < (16*1024*1024); address++) {
			char key[MAX_INT_KEY_SIZE];
			tern_int_key(address, key);
			label_names *names = tern_find_ptr(named_labels, key);
			if (names) {
				for (int i = 0; i < names->num_labels; i++)
				{
					printf("%s equ $%X\n", names->labels[i], address);
				}
			} else if (is_label(address)) {
				printf("ADR_%X equ $%X\n", address, address);
			}
		}
		puts("");
	}
	for (address = address_off; address < address_end; address+=2) {
		if (is_visited(address)) {
			encoded = filebuf + (address-address_off)/2;
			jag_cpu_disasm(&encoded, address, disbuf, is_gpu, labels);
			if (labels) {
				char keybuf[MAX_INT_KEY_SIZE];
				label_names * names = tern_find_ptr(named_labels, tern_int_key(address, keybuf));
				if (names)
				{
					for (int i = 0; i < names->num_labels; i++)
					{
						printf("%s:\n", names->labels[i]);
					}
				} else if (is_label(address)) {
					printf("ADR_%X:\n", address);
				}
				if (addr) {
					printf("\t%s\t;%X\n", disbuf, address);
				} else {
					printf("\t%s\n", disbuf);
				}
			} else {
				printf("%X: %s\n", address, disbuf);
			}
		}
	}
	return 0;
}
