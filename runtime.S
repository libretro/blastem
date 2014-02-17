

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

	.global bcd_add
bcd_add:
	xchg %rax, %rdi

	mov %cl, %ch
	mov %al, %ah
	and $0xF, %ch
	and $0xF, %ah
	and $0xF0, %cl
	and $0xF0, %al
	add %ah, %ch
	cmp $10, %ch
	jb no_adjust
	add $6, %ch
no_adjust:
	add %ch, %al
	add %al, %cl
	mov $0, %ch
	jc def_adjust
	cmp $0xA0, %cl
	jb no_adjust_h
def_adjust:
	add $0x60, %cl
	mov $1, %ch
no_adjust_h:

	mov %rdi, %rax
	ret

	.global bcd_sub
bcd_sub:
	xchg %rax, %rdi

	mov %cl, %ch
	mov %al, %ah
	and $0xF, %ch
	and $0xF, %ah
	and $0xF0, %cl
	and $0xF0, %al
	sub %ah, %ch
	cmp $10, %ch
	jb no_adjusts
	sub $6, %ch
no_adjusts:
	add %ch, %cl
	sub %al, %cl
	mov $0, %ch
	jc def_adjusts
	cmp $0xA0, %cl
	jb no_adjust_hs
def_adjusts:
	sub $0x60, %cl
	mov $1, %ch
no_adjust_hs:

	mov %rdi, %rax
	ret

	.global get_sr
get_sr:
	mov 5(%rsi), %cl
	shl $8, %cx
	mov (%rsi), %cl
	shl $1, %cl
	or %bl, %cl
	shl $1, %cl
	or %dl, %cl
	shl $1, %cl
	or %bh, %cl
	shl $1, %cl
	or %dh, %cl
	ret

	.global set_sr
set_sr:
	mov %cl, %dh
	and $1, %dh
	shr $1, %cl
	mov %cl, %bh
	and $1, %bh
	shr $1, %cl
	mov %cl, %dl
	and $1, %dl
	shr $1, %cl
	mov %cl, %bl
	and $1, %bl
	shr $1, %cl
	and $1, %cl
	mov %cl, (%rsi)
	shr $8, %cx
	mov %cl, 5(%rsi)
	ret

	.global set_ccr
set_ccr:
	mov %cl, %dh
	and $1, %dh
	shr $1, %cl
	mov %cl, %bh
	and $1, %bh
	shr $1, %cl
	mov %cl, %dl
	and $1, %dl
	shr $1, %cl
	mov %cl, %bl
	and $1, %bl
	shr $1, %cl
	and $1, %cl
	mov %cl, (%rsi)
	ret

