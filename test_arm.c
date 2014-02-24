#include <stdio.h>
#include "gen_arm.h"

typedef int32_t (*fib_fun)(int32_t);

int main(int arc, char **argv)
{
	code_info code;
	init_code_info(&code);
	uint32_t *fib = code.cur;
	subi(&code, r0, r0, 2, SET_COND);
	movi_cc(&code, r0, 1, NO_COND, CC_LT);
	bx_cc(&code, lr, CC_LT);
	pushm(&code, LR | R4);
	mov(&code, r4, r0, NO_COND);
	bl(&code, fib);
	mov(&code, r1, r0, NO_COND);
	addi(&code, r0, r4, 1, NO_COND);
	mov(&code, r4, r1, NO_COND);
	bl(&code, fib);
	add(&code, r0, r4, r0, NO_COND);
	popm(&code, LR | R4);
	bx(&code, lr);

	fib_fun fibc = (fib_fun)fib;
	printf("fib(10): %d\n", fibc(10));

	return 0;
}
