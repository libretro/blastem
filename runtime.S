

invalid_msg:
	.asciz "Invalid instruction at %X\n"

	.global m68k_invalid
m68k_invalid:
	lea invalid_msg(%rip), %rdi
	mov %ecx, %esi
	xor %rax, %rax
	call printf
	mov $1, %rdi
	call exit



