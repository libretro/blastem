
	.global handle_cycle_limit
	.global do_sync
handle_cycle_limit:
	cmp 84(%rsi), %eax
	jb skip_sync
do_sync:
	push %rcx
	push %rdi
	call m68k_save_context
	mov %rsi, %rdi
	xor %esi, %esi
	test $8, %esp
	jnz adjust_rsp
	call sync_components
	jmp done_adjust
adjust_rsp:
	sub $8, %rsp
	call sync_components
	add $8, %rsp
done_adjust:
	mov %rax, %rsi
	call m68k_load_context
	pop %rdi
	pop %rcx
skip_sync:
	ret

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

	.global m68k_native_addr_and_sync
m68k_native_addr_and_sync:
	call m68k_save_context
	push %rcx
	mov %rsi, %rdi
	xor %esi, %esi
	test $8, %rsp
	jnz adjust_rsp_na
	call sync_components
	jmp no_adjust_rsp_na
adjust_rsp_na:
	sub $8, %rsp
	call sync_components
	add $8, %rsp
no_adjust_rsp_na:
	pop %rsi
	push %rax
	mov %rax, %rdi
	call get_native_address_trans
	mov %rax, %rcx
	pop %rsi
	call m68k_load_context
	ret

	.global m68k_native_addr
m68k_native_addr:
	call m68k_save_context
	push %rsi
	mov %rsi, %rdi
	mov %ecx, %esi
	call get_native_address_trans
	mov %rax, %rcx
	pop %rsi
	call m68k_load_context
	ret

	.global m68k_retrans_stub
m68k_retrans_stub:
	call m68k_save_context
	push %rsi
	call m68k_retranslate_inst
	pop %rsi
	mov %rax, %rcx
	call m68k_load_context
	jmp *%rcx

	.global m68k_save_context
m68k_save_context:
	mov %bl, 1(%rsi) /* N Flag */
	mov %dl, 2(%rsi) /* Z flag */
	mov %bh, 3(%rsi) /* V flag */
	mov %dh, 4(%rsi) /* C flag */
	mov %r10d, 8(%rsi) /* d0 */
	mov %r11d, 12(%rsi) /* d1 */
	mov %r12d, 16(%rsi) /* d2 */
	mov %r8d,  20(%rsi) /* d3 */
	mov %r13d, 40(%rsi) /* a0 */
	mov %r14d, 44(%rsi) /* a1 */
	mov %r9d,  48(%rsi) /* a2 */
	mov %r15d, 68(%rsi) /* a7 */
	mov %eax, 80(%rsi) /* current cycle count */
	ret

	.global m68k_load_context
m68k_load_context:
	mov 1(%rsi), %bl /* N Flag */
	mov 2(%rsi), %dl /* Z flag */
	mov 3(%rsi), %bh /* V flag */
	mov 4(%rsi), %dh /* C flag */
	mov 8(%rsi), %r10d /* d0 */
	mov 12(%rsi), %r11d /* d1 */
	mov 16(%rsi), %r12d /* d2 */
	mov 20(%rsi), %r8d  /* d3 */
	mov 40(%rsi), %r13d /* a0 */
	mov 44(%rsi), %r14d /* a1 */
	mov 48(%rsi), %r9d  /* a2 */
	mov 68(%rsi), %r15d /* a7 */
	mov 76(%rsi), %ebp /* target cycle count */
	mov 80(%rsi), %eax /* current cycle count */
	ret

