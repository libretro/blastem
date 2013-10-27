/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "mem.h"

/*
void * alloc_code(size_t *size)
{
	*size += PAGE_SIZE - (*size & (PAGE_SIZE - 1));
	return mmap(NULL, *size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
*/

/*
void * alloc_code(size_t *size)
{
	char * ret = malloc(*size);
	char * base = (char *)(((intptr_t)ret) & (~(PAGE_SIZE-1)));
	mprotect(base, (ret + *size) - base, PROT_EXEC | PROT_READ | PROT_WRITE);
	return ret;
}
*/

void * alloc_code(size_t *size)
{
	*size += PAGE_SIZE - (*size & (PAGE_SIZE - 1));
	void * ret = sbrk(*size);
	if (ret == ((void *)-1)) {
		return NULL;
	}
	mprotect(ret, *size, PROT_EXEC | PROT_READ | PROT_WRITE);
	return ret;
}
