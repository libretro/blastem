/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/

#include "mem.h"
#include <windows.h>

void * alloc_code(size_t *size)
{
	*size += PAGE_SIZE - (*size & (PAGE_SIZE - 1));

	return VirtualAlloc(NULL, *size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}
