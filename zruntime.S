z_inccycles_io:
	cmp %edi, %ebp
	jnb do_limit
no_sync_io:
	add $4, %ebp
	ret
do_limit_io:
	cmp 112(%rsi), %ebp
	jb no_sync_io
	jmp sync_io

z_inccycles:
	cmp %edi, %ebp
	jnb do_limit
no_sync:
	add $3, %ebp
	ret
do_limit:
	cmp 112(%rsi), %ebp
	jb no_sync
sync_io:
	movw $0, 164(%rsi)
	call z80_save_context_scratch
	pop %rax /*return address in read/write func*/
	pop 104(%rsi) /*return address in native code*/
	sub $5, %rax /* adjust return addres to point to the call instruction that got us here */
	mov %rax, (%rsi)

	pop %r15 /* restore callee saved regsiters */
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
	ret /* return to caller of z80_run */

forced_sync:
	movw $0, 164(%rsi)
	call z80_save_context_scratch
	pop (%rsi) /*return address in read/write func*/
	pop 104(%rsi) /*return address in native code*/

	pop %r15 /* restore callee saved regsiters */
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
	ret /* return to caller of z80_run */

	.global z80_handle_cycle_limit_int
z80_handle_cycle_limit_int:
	cmp 116(%rsi), %ebp
	jb zskip_int
	mov 112(%rsi), %ebp /* set cycle limit to sync cycle */
	/* not sure this is really necessary now that IFF1 and IFF2 are geting cleared */
	movl $0xFFFFFFFF, 116(%rsi) /* make sure the interrupt doesn't fire more than once */
	/* disable interrupts */
	movb $0, 96(%rsi)
	movb $0, 97(%rsi)
	add $7, %ebp
	sub $2, %r9w
	mov %r9w, %r14w
	call z_inccycles
	push %r13
	call z80_write_byte_noinc
	pop %r13
	mov %r9w, %r14w
	add $1, %r14w
	shr $8, %r13w
	call z_inccycles
	call z80_write_byte_noinc
	pop %r14 /*dispose of return address */
	/* TODO: Support interrupt mode 0 and 2 */
	mov $0x38, %r13w
	call z80_native_addr
	jmp *%r13
zskip_int:
	cmp 112(%rsi), %ebp
	jb zskip_sync
mov %r13w, 164(%rsi)
	.global z80_do_sync
z80_do_sync:
	call z80_save_context
	pop (%rsi) /*return address in native code*/
	pop %r15 /* restore callee saved regsiters */
	pop %r14
	pop %r13
	pop %r12
	pop %rbp
	pop %rbx
zskip_sync:
	ret

	.global z80_halt
z80_halt:
	mov %edi, %r14d
	sub %ebp, %r14d
	and $0xFFFFFFFC, %r14d
	add %r14d, %ebp
	cmp %edi, %ebp
	jnb z80_handle_cycle_limit_int
	add $4, %ebp
	jmp z80_handle_cycle_limit_int

	.global z80_read_byte
z80_read_byte:
	call z_inccycles
z80_read_byte_noinc:
	cmp $0x4000, %r13w
	jb z80_read_ram
	cmp $0x8000, %r13w
	jae z80_read_bank
	cmp $0x6000, %r13w
	jb z80_read_ym2612
	/* TODO: Bank reg, YM-2612, PSG/VDP */
	mov $0xFF, %r13b
	ret
z80_read_ram:
	and $0x1FFF, %r13
	mov (%r11, %r13), %r13b
	ret
z80_read_bank:
	/* approximation of wait states for normal 68K bus access */
	add $3, %ebp
	push %rsi
	mov 144(%rsi), %rsi /* get system context pointer */
	cmpb $0, 120(%rsi) /* check bus busy flag */
	pop %rsi
	jne bus_busy
z80_read_bank_cont:
	and $0x7FFF, %r13
	cmp $0, %r12
	je slow_bank_read
	/* 68K memory is byte swapped */
	xor $1, %r13
	mov (%r12, %r13), %r13b
	ret
slow_bank_read:
	/* TODO: Call into C to implement this */
	ret
bus_busy:
	cmp %ebp, %edi
	jbe no_adjust
	mov %edi, %ebp
no_adjust:
	call forced_sync
	jmp z80_read_bank_cont
z80_read_ym2612:
	call z80_save_context
	mov %r13w, %di
	push %rsi
	test $8, %rsp
	jnz 0f
	call z80_read_ym
	jmp 1f
0:
	sub $8, %rsp
	call z80_read_ym
	add $8, %rsp
1:
	pop %rsi
	mov %al, %r13b
	call z80_load_context
	ret

	.global z80_write_byte
z80_write_byte:
	call z_inccycles
z80_write_byte_noinc:
	cmp $0x4000, %r14w
	jb z80_write_ram
	cmp $0x8000, %r14w
	jae z80_write_bank
	cmp $0x6000, %r14w
	jb z80_write_ym2612
	cmp $0x6100, %r14w
	jb z80_write_bank_reg
	cmp $0x7F00, %r14w
	jae z80_write_vdp
	ret
z80_write_ram:
	and $0x1FFF, %r14
	mov %r13b, (%r11, %r14)
	mov %r14d, %r13d
	shr $7, %r13d
	bt %r13d, 152(%rsi)
	jnc not_code
	call z80_save_context
	mov %r14d, %edi
	call z80_handle_code_write
	mov %rax, %rsi
	call z80_load_context
not_code:
	ret
z80_write_bank:
slow_bank_write:
	/* approximation of wait states for 68K bus access */
	add $3, %ebp
	/* TODO: Call into C to implement this */
	ret
z80_write_ym2612:
	and $0x3, %r14w
	call z80_save_context
	mov %r14w, %di
	mov %r13b, %dl
	test $8, %rsp
	jnz 0f
	call z80_write_ym
	jmp 1f
0:
	sub $8, %rsp
	call z80_write_ym
	add $8, %rsp
1:
	mov %rax, %rsi
	jmp z80_load_context
z80_write_bank_reg:
	and $1, %r13w
	shr %r15w
	shl $8, %r13w
	xor %r12, %r12
	or %r13w, %r15w
	and $0x1FF, %r15w
	cmp $0x80, %r15w
	jb update_bank_ptr
	ret
update_bank_ptr:
	mov %r15w, %r12w
	shl $15, %r12
	add 80(%rsi), %r12
	ret
z80_write_vdp:
	and $0xFF, %r14w
	call z80_save_context
	mov %r14w, %di
	mov %r13b, %dl
	test $8, %rsp
	jnz 0f
	call z80_vdp_port_write
	jmp 1f
0:
	sub $8, %rsp
	call z80_vdp_port_write
	add $8, %rsp
1:
	mov %rax, %rsi
	jmp z80_load_context

	.global z80_read_word
z80_read_word:
	call z_inccycles
	cmp $0x8000, %r13w
	jae z80_read_bank_word
	push %r13
	call z80_read_byte_noinc
	mov %r13b, %r14b
	pop %r13
	inc %r13
	call z_inccycles
	call z80_read_byte_noinc
	shl $8, %r13w
	mov %r14b, %r13b
	ret

z80_read_bank_word:
	add $3, %ebp /* first read typically has 3 wait states */
	push %rsi
	mov 144(%rsi), %rsi /* get system context pointer */
	cmpb $0, 120(%rsi) /* check bus busy flag */
	pop %rsi
	jne bus_busy_word
z80_read_bank_word_cont:
	push %r13
	call z80_read_bank_cont
	mov %r13b, %r14b
	pop %r13
	inc %r13
	call z_inccycles
	add $4, %ebp /* second read typically has 4 wait states */
	push %rsi
	mov 144(%rsi), %rsi /* get system context pointer */
	cmpb $0, 120(%rsi) /* check bus busy flag */
	pop %rsi
	jne bus_busy_word2
z80_read_bank_word_cont2:
	call z80_read_bank_cont
	shl $8, %r13w
	mov %r14b, %r13b
	ret

bus_busy_word:
	cmp %ebp, %edi
	jb no_adjust_word
	mov %edi, %ebp
no_adjust_word:
	call forced_sync
	jmp z80_read_bank_word_cont

foofoo:
	jmp foofoo

bus_busy_word2:
	cmp %ebp, %edi
	jb no_adjust_word2
	mov %edi, %ebp
no_adjust_word2:
	call forced_sync
	jmp z80_read_bank_word_cont2
blahblah:
	jmp blahblah

	.global z80_write_word_highfirst
z80_write_word_highfirst:
	call z_inccycles
	push %r14
	push %r13
	add $1, %r14w
	shr $8, %r13w
	call z80_write_byte_noinc
	pop %r13
	pop %r14
	call z_inccycles
	call z80_write_byte_noinc
	ret

	.global z80_write_word_lowfirst
z80_write_word_lowfirst:
	call z_inccycles
	push %r14
	push %r13
	call z80_write_byte_noinc
	pop %r13
	pop %r14
	add $1, %r14w
	shr $8, %r13w
	call z_inccycles
	call z80_write_byte_noinc
	ret

	.global z80_io_read
z80_io_read:
	call z_inccycles_io
	/* genesis Z80 has no IO port hardware and always returns FF */
	mov $0xFF, %r13
	ret

	.global z80_io_write
z80_io_write:
	call z_inccycles_io
	/* genesis Z80 has no IO port hardware and writes have no effect */
	ret

	.global z80_retrans_stub
z80_retrans_stub:
	pop %r14
	call z80_save_context
	/* adjust for mov and call instructions */
	sub $11, %r14
	mov %r13d, %edi
	mov %r14, %rdx
	push %rsi
	call z80_retranslate_inst
	pop %rsi
	mov %rax, %r13
	call z80_load_context
	jmp *%r13

	.global z80_native_addr
z80_native_addr:
	call z80_save_context
	push %rsi
	mov %rsi, %rdi
	movzx %r13w, %esi
	call z80_get_native_address_trans
	mov %rax, %r13
	pop %rsi
	call z80_load_context
	ret

z80_save_context_scratch:
	mov %r13w, 98(%rsi)  /* scratch1 */
	mov %r14w, 100(%rsi) /* scratch2 */

	.global z80_save_context
z80_save_context:
	mov %r9w, 8(%rsi)    /* SP */
	mov %r15w, 16(%rsi)  /* bank register */
	mov %bx, 18(%rsi)    /* BC */
	mov %cx, 20(%rsi)    /* DE */
	mov %ax, 22(%rsi)    /* HL */
	mov %dx, 24(%rsi)    /* IX */
	mov %r8w, 26(%rsi)   /* IY */
	mov %r10b, 30(%rsi)  /* A */
	mov %edi, 48(%rsi)   /* target_cycle */
	mov %ebp, 52(%rsi)   /* current_cycle */
	mov %r12, 72(%rsi)   /* cartridge bank pointer */
	ret


z80_load_context_scratch:
	mov 98(%rsi), %r13w  /* scratch1 */
	mov 100(%rsi), %r14w /* scratch2 */

	.global z80_load_context
z80_load_context:
	mov 8(%rsi), %r9w    /* SP */
	mov 16(%rsi), %r15w  /* bank register */
	mov 18(%rsi), %bx    /* BC */
	mov 20(%rsi), %cx    /* DE */
	mov 22(%rsi), %ax    /* HL */
	mov 24(%rsi), %dx    /* IX */
	mov 26(%rsi), %r8w   /* IY */
	mov 30(%rsi), %r10b  /* A */
	mov 48(%rsi), %edi   /* target_cycle */
	mov 52(%rsi), %ebp   /* current_cycle */
	mov 64(%rsi), %r11	 /* z80 RAM */
	mov 72(%rsi), %r12   /* cartridge bank pointer */
	ret

	.global z80_run
z80_run:
	push %rbx
	push %rbp
	push %r12
	push %r13
	push %r14
	push %r15
	mov %rdi, %rsi
	call z80_load_context_scratch
	cmpq $0, 104(%rsi)
	je no_extra
	push 104(%rsi)
	movq $0, 104(%rsi)
no_extra:
	jmp *(%rsi)

