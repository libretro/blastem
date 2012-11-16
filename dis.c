#include "68kinst.h"
#include <stdio.h>
#include <stdlib.h>

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
	for(cur = filebuf; (cur - filebuf) < (filesize/2); )
	{
		//printf("cur: %p: %x\n", cur, *cur);
		unsigned short * start = cur;
		cur = m68K_decode(cur, &instbuf);
		m68k_disasm(&instbuf, disbuf);
		printf("%lX: %s\n", (start - filebuf)*2, disbuf);
	}
	return 0;
}
