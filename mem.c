#include <sys/mman.h>
#include <stddef.h>
#include "mem.h"

void * alloc_code(size_t *size)
{
	*size += PAGE_SIZE - (*size & (PAGE_SIZE - 1));
	return mmap(NULL, *size, PROT_EXEC | PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

