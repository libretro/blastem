

invalid_msg:
	.asciz "Invalid instruction at %X\n"

	.global _m68k_invalid
_m68k_invalid:
	push %ecx
	push invalid_msg
	xor %eax, %eax
	call _printf
	push $1
	call _exit

	.global _bcd_add
_bcd_add:
	xchg %eax, %edi

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

	mov %edi, %eax
	ret

	.global _bcd_sub
_bcd_sub:
	xchg %eax, %edi

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

	mov %edi, %eax
	ret



