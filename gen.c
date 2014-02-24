#include <stdio.h>
#include <stdlib.h>
#include "gen.h"
#include "mem.h"

void init_code_info(code_info *code)
{
	size_t size = CODE_ALLOC_SIZE;
	code->cur = alloc_code(&size);
	if (!code->cur) {
		fputs("Failed to allocate memory for generated code\n", stderr);
		exit(1);
	}
	code->last = code->cur + size/sizeof(code_word) - RESERVE_WORDS;
}
