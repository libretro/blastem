/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gen_x86.h"
#include "m68k_core.h"
#include <stdio.h>
#include <stddef.h>

int main(int argc, char ** argv)
{
	uint8_t foo[512];
	uint8_t *cur = foo, *end;
	cur = mov_rr(cur, RAX, RBX, SZ_B);
	cur = mov_rr(cur, RCX, RDX, SZ_B);
	cur = mov_rr(cur, R8, R9, SZ_B);
	cur = mov_rr(cur, R8, RAX, SZ_B);
	cur = mov_rr(cur, RAX, RBX, SZ_W);
	cur = mov_rr(cur, R11, R12, SZ_W);
	cur = mov_rr(cur, RAX, RBX, SZ_D);
	cur = mov_rr(cur, RAX, RBX, SZ_Q);
	cur = mov_ir(cur, 5, RAX, SZ_D);
	cur = mov_ir(cur, 3, R8, SZ_D);
	cur = mov_ir(cur, 4, RSP, SZ_B);
	cur = add_rr(cur, RAX, RBX, SZ_D);
	cur = add_ir(cur, 5, RAX, SZ_B);
	cur = add_ir(cur, 5, RBX, SZ_B);
	cur = add_ir(cur, 5, RBP, SZ_B);
	cur = pushf(cur);
	cur = popf(cur);
	cur = setcc_r(cur, CC_S, RBX);
	cur = setcc_r(cur, CC_Z, RDX);
	cur = setcc_r(cur, CC_O, BH);
	cur = setcc_r(cur, CC_C, DH);
	cur = setcc_rind(cur, CC_C, RSI);
	cur = mov_rrdisp8(cur, RCX, RSI, offsetof(m68k_context, dregs) + 4 * sizeof(uint32_t), SZ_D);
	cur = mov_rdisp8r(cur, RSI, offsetof(m68k_context, dregs) + 5 * sizeof(uint32_t), RCX, SZ_D);
	cur = mov_rrind(cur, DH, RSI, SZ_B);
	cur = jcc(cur, CC_NZ, -2);
	cur = jcc(cur, CC_Z, 0);
	cur = jcc(cur, CC_LE, 0x7CA);
	for (end = cur, cur = foo; cur != end; cur++) {
		printf(" %X", *cur);
	}
	puts("");
	return 0;
}
