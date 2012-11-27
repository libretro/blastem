#ifndef MEM_H_
#define MEM_H_

#include <stddef.h>

#define PAGE_SIZE 4096

void * alloc_code(size_t *size);

#endif //MEM_H_

