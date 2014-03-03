#ifndef GEN_H_
#define GEN_H_
#include <stdint.h>

#if defined(X86_64) || defined(X86_32)
typedef uint8_t code_word;
#define RESERVE_WORDS 5 //opcode + 4-byte displacement
#else
typedef uint32_t code_word;
#define RESERVE_WORDS 4 //1 push + 1 ldr + 1bx + 1 constant
#endif
typedef code_word * code_ptr;
#define CODE_ALLOC_SIZE (1024*1024)

typedef struct {
	code_ptr cur;
	code_ptr last;
} code_info;

void init_code_info(code_info *code);
void call(code_info *code, code_ptr fun);
void jmp(code_info *code, code_ptr dest);

#endif //GEN_H_
