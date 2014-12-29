

invalid_msg:
	.asciz "Invalid instruction at %X\n"

	.global m68k_invalid
m68k_invalid:
	push %ecx
	push invalid_msg
	xor %eax, %eax
	call printf
	push $1
	call exit




