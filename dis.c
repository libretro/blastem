#include "68kinst.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t visited[(16*1024*1024)/16];

void visit(uint32_t address)
{
	address &= 0xFFFFFF;
	visited[address/16] |= 1 << ((address / 2) % 8);
}

uint8_t is_visited(uint32_t address)
{
	address &= 0xFFFFFF;
	return visited[address/16] & (1 << ((address / 2) % 8));
}

typedef struct deferred {
	uint32_t address;
	struct deferred *next;
} deferred;

deferred * defer(uint32_t address, deferred * next)
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

#define SIMPLE 0

int main(int argc, char ** argv)
{
	long filesize;
	unsigned short *filebuf;
	char disbuf[1024];
	m68kinst instbuf;
	unsigned short * cur;
	FILE * f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END);
	filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	filebuf = malloc(filesize);
	fread(filebuf, 2, filesize/2, f);
	fclose(f);
	for(cur = filebuf; cur - filebuf < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	uint32_t address = filebuf[2] << 16 | filebuf[3], tmp_addr;
	#if !SIMPLE
	uint16_t *encoded, *next;
	uint32_t size;
	deferred *def = NULL, *tmpd;
	def = defer(address, def);
	def = defer(filebuf[0x68/2] << 16 | filebuf[0x6A/2], def);
	def = defer(filebuf[0x70/2] << 16 | filebuf[0x72/2], def);
	def = defer(filebuf[0x78/2] << 16 | filebuf[0x7A/2], def);
	while(def) {
		do {
			encoded = NULL;
			address = def->address;
			if (!is_visited(address)) {
				encoded = filebuf + address/2;
			}
			tmpd = def;
			def = def->next;
			free(tmpd);
		} while(def && encoded == NULL);
		if (!encoded) {
			break;
		}
		for(;;) {
			if (address > filesize) {
				break;
			}
			visit(address);
			next = m68k_decode(encoded, &instbuf, address);
			address += (next-encoded)*2;
			encoded = next;
			//m68k_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", instbuf.address, disbuf);
			if (instbuf.op == M68K_ILLEGAL || instbuf.op == M68K_RTS || instbuf.op == M68K_RTE) {
				break;
			} else if (instbuf.op == M68K_BCC || instbuf.op == M68K_DBCC || instbuf.op == M68K_BSR) {
				if (instbuf.op == M68K_BCC && instbuf.extra.cond == COND_TRUE) {
					address = instbuf.address + 2 + instbuf.src.params.immed;
					encoded = filebuf + address/2;
					if (is_visited(address)) {
						break;
					}
				} else {
					tmp_addr = instbuf.address + 2 + instbuf.src.params.immed;
					def = defer(tmp_addr, def);
				}
			} else if(instbuf.op == M68K_JMP) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					address = instbuf.src.params.immed;
					encoded = filebuf + address/2;
					if (is_visited(address)) {
						break;
					}
				} else {
					break;
				}
			} else if(instbuf.op == M68K_JSR) {
				if (instbuf.src.addr_mode == MODE_ABSOLUTE || instbuf.src.addr_mode == MODE_ABSOLUTE_SHORT) {
					def = defer(instbuf.src.params.immed, def);
				}
			}
		}
	}
	for (address = 0; address < filesize; address+=2) {
		if (is_visited(address)) {
			encoded = filebuf + address/2;
			m68k_decode(encoded, &instbuf, address);
			m68k_disasm(&instbuf, disbuf);
			printf("%X: %s\n", instbuf.address, disbuf);
		}
	}
	#else
	for(cur = filebuf + 0x100; (cur - filebuf) < (filesize/2); )
	{
		unsigned short * start = cur;
		cur = m68k_decode(cur, &instbuf, (start - filebuf)*2);
		m68k_disasm(&instbuf, disbuf);
		printf("%X: %s\n", instbuf.address, disbuf);
	}
	#endif
	return 0;
}
