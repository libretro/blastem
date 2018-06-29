/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "68kinst.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include "vos_program_module.h"
#include "tern.h"
#include "util.h"

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

void check_reference(m68kinst * inst, m68k_op_info * op)
{
	switch(op->addr_mode)
	{
	case MODE_PC_DISPLACE:
		reference(inst->address + 2 + op->params.regs.displacement);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		reference(op->params.immed);
		break;
	}
}

int label_fun(char *dst, uint32_t address, void * data)
{
	tern_node * labels = data;
	char key[MAX_INT_KEY_SIZE];
	label_names * names = tern_find_ptr(labels, tern_int_key(address & 0xFFFFFF, key));
	if (names)
	{
		return sprintf(dst, "%s", names->labels[0]);
	} else {
		return m68k_default_label_fun(dst, address, NULL);
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
	m68kinst instbuf;
	unsigned short * cur;
	deferred *def = NULL, *tmpd;

	uint8_t labels = 0, addr = 0, only = 0, vos = 0, reset = 0;
	tern_node * named_labels = NULL;

	uint32_t address_off = 0, address_end;
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
			case 'v':
				vos = 1;
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
				fclose(address_log);
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
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);

	char int_key[MAX_INT_KEY_SIZE];
	if (vos)
	{
		vos_program_module header;
		vos_read_header(f, &header);
		vos_read_alloc_module_map(f, &header);
		address_off = header.user_boundary;
		address_end = address_off + filesize - 0x1000;
		def = defer(header.main_entry_link.code_address, def);
		named_labels = add_label(named_labels, "main_entry_link", header.main_entry_link.code_address);
		for (int i = 0; i < header.n_modules; i++)
		{
			if (!reset || header.module_map_entries[i].code_address != header.user_boundary)
			{
				def = defer(header.module_map_entries[i].code_address, def);
			}
			named_labels = add_label(named_labels, header.module_map_entries[i].name.str, header.module_map_entries[i].code_address);
		}
		fseek(f, 0x1000, SEEK_SET);
		filebuf = malloc(filesize - 0x1000);
		if (fread(filebuf, 2, (filesize - 0x1000)/2, f) != (filesize - 0x1000)/2)
		{
			fprintf(stderr, "Failure while reading file %s\n", argv[1]);
		}
		fclose(f);
		for(cur = filebuf; cur - filebuf < ((filesize - 0x1000)/2); ++cur)
		{
			*cur = (*cur >> 8) | (*cur << 8);
		}
		if (reset)
		{
			def = defer(filebuf[2] << 16 | filebuf[3], def);
			named_labels = add_label(named_labels, "reset", filebuf[2] << 16 | filebuf[3]);
		}
	} else {
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
		uint32_t start = filebuf[2] << 16 | filebuf[3];
		uint32_t int_2 = filebuf[0x68/2] << 16 | filebuf[0x6A/2];
		uint32_t int_4 = filebuf[0x70/2] << 16 | filebuf[0x72/2];
		uint32_t int_6 = filebuf[0x78/2] << 16 | filebuf[0x7A/2];
		named_labels = add_label(named_labels, "start", start);
		named_labels = add_label(named_labels, "int_2", int_2);
		named_labels = add_label(named_labels, "int_4", int_4);
		named_labels = add_label(named_labels, "int_6", int_6);
		if (!def || !only) {
			def = defer(start, def);
			def = defer(int_2, def);
			def = defer(int_4, def);
			def = defer(int_6, def);
		}
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
			next = m68k_decode(encoded, &instbuf, address);
			address += (next-encoded)*2;
			encoded = next;
			//m68k_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", instbuf.address, disbuf);
			check_reference(&instbuf, &(instbuf.src));
			check_reference(&instbuf, &(instbuf.dst));
			if (instbuf.op == M68K_ILLEGAL || instbuf.op == M68K_RTS || instbuf.op == M68K_RTE || instbuf.op == M68K_INVALID) {
				break;
			}
			if (instbuf.op == M68K_BCC || instbuf.op == M68K_DBCC || instbuf.op == M68K_BSR) {
				if (instbuf.op == M68K_BCC && instbuf.extra.cond == COND_TRUE) {
					address = instbuf.address + 2 + instbuf.src.params.immed;
					encoded = filebuf + (address - address_off)/2;
					reference(address);
					if (is_visited(address)) {
						break;
					}
				} else {
					tmp_addr = instbuf.address + 2 + instbuf.src.params.immed;
					reference(tmp_addr);
					def = defer(tmp_addr, def);
				}
			} else if(instbuf.op == M68K_JMP) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					address = instbuf.src.params.immed;
					encoded = filebuf + (address - address_off)/2;
					if (is_visited(address)) {
						break;
					}
				} else if (instbuf.src.addr_mode == MODE_PC_DISPLACE) {
					address = instbuf.src.params.regs.displacement + instbuf.address + 2;
					encoded = filebuf + (address - address_off)/2;
					if (is_visited(address)) {
						break;
					}
				} else {
					break;
				}
			} else if(instbuf.op == M68K_JSR) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					def = defer(instbuf.src.params.immed, def);
				} else if (instbuf.src.addr_mode == MODE_PC_DISPLACE) {
					def = defer(instbuf.src.params.regs.displacement + instbuf.address + 2, def);
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
			m68k_decode(encoded, &instbuf, address);
			if (labels) {
				m68k_disasm_labels(&instbuf, disbuf, label_fun, named_labels);
				char keybuf[MAX_INT_KEY_SIZE];
				label_names * names = tern_find_ptr(named_labels, tern_int_key(address, keybuf));
				if (names)
				{
					for (int i = 0; i < names->num_labels; i++)
					{
						printf("%s:\n", names->labels[i]);
					}
				} else if (is_label(instbuf.address)) {
					printf("ADR_%X:\n", instbuf.address);
				}
				if (addr) {
					printf("\t%s\t;%X\n", disbuf, instbuf.address);
				} else {
					printf("\t%s\n", disbuf);
				}
			} else {
				m68k_disasm(&instbuf, disbuf);
				printf("%X: %s\n", instbuf.address, disbuf);
			}
		}
	}
	return 0;
}
