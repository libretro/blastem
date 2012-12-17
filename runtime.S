
	.global handle_cycle_limit
handle_cycle_limit:
	ret
	
	.global m68k_write_word
m68k_write_word:
	and $0xFFFFFF, %rdi
	cmp $0x400000, %edi
	jle cart_w
	cmp $0xE00000, %edi
	jge workram_w
	jmp inccycles
workram_w:
	and $0xFFFF, %rdi
	mov %cx, (%r9, %rdi)
	jmp inccycles
cart_w:
	mov %cx, (%r8, %rdi)
	jmp inccycles

	.global m68k_write_byte
m68k_write_byte:
	and $0xFFFFFF, %rdi
	/* deal with byte swapping */
	xor $1, %edi
	cmp $0x400000, %edi
	jle cart_wb
	cmp $0xE00000, %edi
	jge workram_wb
	jmp inccycles
workram_wb:
	and $0xFFFF, %rdi
	mov %cl, (%r9, %rdi)
	jmp inccycles
cart_wb:
	mov %cl, (%r8, %rdi)
	jmp inccycles

	.global m68k_write_long_lowfirst
m68k_write_long_lowfirst:
	push %rdi
	add $2, %edi
	call m68k_write_word
	shr $16, %ecx
	pop %rdi
	jmp m68k_write_word

	.global m68k_write_long_highfirst
m68k_write_long_highfirst:
	push %rdi
	push %rcx
	shr $16, %ecx
	call m68k_write_word
	pop %rcx
	pop %rdi
	add $2, %rdi
	jmp m68k_write_word

	.global m68k_read_word_scratch1
m68k_read_word_scratch1:
	and $0xFFFFFF, %rcx
	cmp $0x400000, %ecx
	jle cart
	cmp $0xE00000, %ecx
	jge workram
	xor %cx, %cx
	jmp inccycles
workram:
	and $0xFFFF, %rcx
	mov (%r9, %rcx), %cx
	jmp inccycles
cart:
	mov (%r8, %rcx), %cx
inccycles:
	add $4, %rax
	cmp %rbp, %rax
	jge sync
	ret
sync:
	ret
	
	.global m68k_read_long_scratch1
m68k_read_long_scratch1:
	push %rcx
	call m68k_read_word_scratch1
	mov %cx, %di
	pop %rcx
	add $2, %ecx
	call m68k_read_word_scratch1
	and $0xFFFF, %ecx
	shl $16, %edi
	or %edi, %ecx
	ret

	.global m68k_read_byte_scratch1
m68k_read_byte_scratch1:
	and $0xFFFFFF, %rcx
	/* deal with byte swapping */
	xor $1, %ecx
	cmp $0x400000, %ecx
	jle cart_b
	cmp $0xE00000, %ecx
	jge workram_b
	xor %cl, %cl
	jmp inccycles
workram_b:
	and $0xFFFF, %rcx
	mov (%r9, %rcx), %cl
	jmp inccycles
cart_b:
	mov (%r8, %rcx), %cl
	jmp inccycles
	
ret_addr_msg:
	.asciz "Program modified return address on stack: found %X, expected %X\n"
	
	.global m68k_modified_ret_addr
m68k_modified_ret_addr:
	lea ret_addr_msg(%rip), %rdi
	mov %rcx, %rsi
	mov 8(%rsp), %rdx
	call printf
	mov $1, %rdi
	call exit

dyn_addr_msg:
	.asciz "Program needs dynamically calculated native address\n"
	
	.global m68k_native_addr
m68k_native_addr:
	lea dyn_addr_msg(%rip), %rdi
	call puts
	mov $1, %rdi
	call exit

	.global m68k_save_context
m68k_save_context:
	mov %bl, 1(%rsi) /* N Flag */
	mov %bh, 2(%rsi) /* V flag */
	mov %dl, 3(%rsi) /* Z flag */
	mov %dh, 4(%rsi) /* C flag */
	mov %r10d, 8(%rsi) /* d0 */
	mov %r11d, 12(%rsi) /* d1 */
	mov %r12d, 16(%rsi) /* d2 */
	mov %r13d, 40(%rsi) /* a0 */
	mov %r14d, 44(%rsi) /* a1 */
	mov %r15d, 68(%rsi) /* a7 */
	ret

	.global m68k_load_context
m68k_load_context:
	mov 1(%rsi), %bl /* N Flag */
	mov 2(%rsi), %bh /* V flag */
	mov 3(%rsi), %dl /* Z flag */
	mov 4(%rsi), %dh /* C flag */
	mov 8(%rsi), %r10d /* d0 */
	mov 12(%rsi), %r11d /* d1 */
	mov 16(%rsi), %r12d /* d2 */
	mov 40(%rsi), %r13d /* a0 */
	mov 44(%rsi), %r14d /* a1 */
	mov 68(%rsi), %r15d /* a7 */
	mov 72(%rsi), %ebp /* target cycle count */
	mov 76(%rsi), %eax /* current cycle count */
	mov 80(%rsi), %r8d /* cartridge address */
	mov 88(%rsi), %r9d /* work ram address */
	ret

	.global m68k_start_context
m68k_start_context:
	call m68k_load_context
	jmp *%rdi
