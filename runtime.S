
	.global handle_cycle_limit
handle_cycle_limit:
	cmp 84(%rsi), %eax
	jb skip_sync
	call m68k_save_context
	mov %rsi, %rdi
	call sync_components
	mov %rax, %rsi
	call m68k_load_context
skip_sync:
	ret

	.global handle_cycle_limit_int
handle_cycle_limit_int:
	cmp 88(%rsi), %eax
	jb skip_int
	push %rcx
	/* call print_int_dbg */
	/* swap USP and SSP if not already in supervisor mode */
	bt $5, 5(%rsi)
	jc already_supervisor
	mov 72(%rsi), %edi
	mov %r15d, 72(%rsi)
	mov %edi, %r15d
already_supervisor:
	/* save status register on stack */
	sub $2, %r15d
	mov %r15d, %edi
	call get_sr
	call m68k_write_word
	/* update status register */
	andb $0xF8, 5(%rsi)
	mov 92(%rsi), %cl
	or $0x20, %cl
	or %cl, 5(%rsi)
	/* save PC */
	sub $4, %r15d
	mov %r15d, %edi
	pop %rcx
	call m68k_write_long_lowfirst
	/* calculate interrupt vector address */
	mov 92(%rsi), %ecx
	shl $2, %ecx
	add $0x60, %ecx
	call m68k_read_long_scratch1
	call m68k_native_addr_and_sync
	add $24, %eax
	/* discard function return address */
	pop %rdi
	jmp *%rcx
skip_int:
	ret
	
int_dbg_msg:
	.asciz "Executing Interrupt!"
print_int_dbg:
	call m68k_save_context
	push %rsi
	lea int_dbg_msg(%rip), %rdi
	call puts
	pop %rsi
	call m68k_load_context
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

do_vdp_port_write:
	call m68k_save_context
	mov %rcx, %rdx
	call vdp_port_write
	mov %rax, %rsi
	call m68k_load_context
	ret

do_vdp_port_read:
	mov %ecx, %edi
	call m68k_save_context
	call vdp_port_read
	mov %rax, %rsi
	call m68k_load_context
	mov 136(%rsi), %cx
	ret
	
do_io_write:
	call m68k_save_context
	and $0x1FFF, %edi
	mov %ecx, %edx
	call io_write
	mov %rax, %rsi
	call m68k_load_context
	ret
do_io_read:
	mov %ecx, %edi
	and $0x1FFF, %edi
	call m68k_save_context
	call io_read
	mov %rax, %rsi
	call m68k_load_context
	mov 136(%rsi), %cl
	ret

do_io_write_w:
	call m68k_save_context
	and $0x1FFF, %edi
	mov %ecx, %edx
	call io_write_w
	mov %rax, %rsi
	call m68k_load_context
	ret
do_io_read_w:
	mov %ecx, %edi
	and $0x1FFF, %edi
	call m68k_save_context
	call io_read_w
	mov %rax, %rsi
	call m68k_load_context
	mov 136(%rsi), %cx
	ret
	
bad_access_msg:
	.asciz "Program tried to access illegal 68K address %X\n"
	
	.global m68k_write_word
	.global try_fifo_write
m68k_write_word:
	call inccycles
	and $0xFFFFFF, %rdi
	cmp $0x400000, %edi
	jbe cart_w
	cmp $0xE00000, %edi
	jae workram_w
	cmp $0xC00000, %edi
	jae vdp_psg_w
	cmp $0xA10000, %edi
	jb not_io_w
	cmp $0xA12000, %edi
	jae not_io_w
	jmp do_io_write_w
not_io_w:
	ret
workram_w:
	and $0xFFFF, %rdi
	mov %cx, (%r9, %rdi)
	ret
cart_w:
	mov %cx, (%r8, %rdi)
	ret
vdp_psg_w:
	test $0x2700E0, %edi
	jnz crash
	and $0x1F, %edi
	cmp $4, %edi
	jb try_fifo_write
	jmp do_vdp_port_write
try_fifo_write:
	push %rdx
	push %rbx
	/* fetch VDP context pointer from 68K context */
	mov 128(%rsi), %rdx
	/* get fifo_cur and compare it to fifo_end */
	mov (%rdx), %rbx
	cmp %rbx, 8(%rdx)
	/* bail out if fifo is full */
	je fifo_fallback
	/* populate FIFO entry */
	mov %cx, 4(%rbx) /* value */
	movb $0, 6(%rbx) /* partial */
	mov %eax, %ecx
	shl $3, %ecx /* multiply by 68K cycle by 7 to get MCLK cycle */
	sub %eax, %ecx
	mov %ecx, (%rbx) /* cycle */
	/* update fifo_cur and store back in 68K context */
	add $8, %rbx
	mov %rbx, (%rdx)
	/* clear pending flag */
	andb $0xEF, 19(%rdx)
	pop %rbx
	pop %rdx
	ret
fifo_fallback:
	pop %rbx
	pop %rdx
	jmp do_vdp_port_write
crash:
	mov %edi, %esi
	lea bad_access_msg(%rip), %rdi
	call printf
	mov $1, %rdi
	call exit

	.global m68k_write_byte
m68k_write_byte:
	call inccycles
	and $0xFFFFFF, %rdi
	cmp $0x400000, %edi
	jbe cart_wb
	cmp $0xE00000, %edi
	jae workram_wb
	cmp $0xC00000, %edi
	jae vdp_psg_wb
	cmp $0xA10000, %edi
	jb not_io_wb
	cmp $0xA12000, %edi
	jae not_io_wb
	jmp do_io_write
not_io_wb:
	ret
workram_wb:
	/* deal with byte swapping */
	xor $1, %edi
	and $0xFFFF, %rdi
	mov %cl, (%r9, %rdi)
	ret
cart_wb:
	/* deal with byte swapping */
	xor $1, %edi
	mov %cl, (%r8, %rdi)
	ret
vdp_psg_wb:
	push %rdx
	mov %cl, %dl
	and $0xFF, %cx
	shl $8, %dx
	or %dx, %cx
	pop %rdx
	jmp vdp_psg_w

	.global m68k_write_long_lowfirst
m68k_write_long_lowfirst:
	push %rdi
	push %rcx
	add $2, %edi
	call m68k_write_word
	pop %rcx
	pop %rdi
	shr $16, %ecx
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
	
inccycles:
	cmp %rbp, %rax
	jnb do_limit
	add $4, %rax
	ret
do_limit:
	push %rcx
	push %rdi
	call handle_cycle_limit
	pop %rdi
	pop %rcx
	add $4, %rax
	ret

	.global m68k_read_word_scratch1
m68k_read_word_scratch1:
	call inccycles
	and $0xFFFFFF, %rcx
	cmp $0x400000, %ecx
	jbe cart
	cmp $0xE00000, %ecx
	jae workram
	cmp $0xC00000, %ecx
	jae vdp_psg
	cmp $0xA10000, %ecx
	jb not_io
	cmp $0xA12000, %ecx
	jae not_io
	call do_io_read_w
	ret
not_io:
	xor %cx, %cx
	dec %cx
	ret
workram:
	and $0xFFFF, %rcx
	mov (%r9, %rcx), %cx
	ret
vdp_psg:
	test $0x2700E0, %ecx
	jnz crash
	and $0x1F, %ecx
	jmp do_vdp_port_read
cart:
	mov (%r8, %rcx), %cx
	ret
	
	.global m68k_read_long_scratch1
m68k_read_long_scratch1:
	push %rcx
	call m68k_read_word_scratch1
	mov %cx, %di
	pop %rcx
	add $2, %ecx
	push %rdi
	call m68k_read_word_scratch1
	pop %rdi
	and $0xFFFF, %ecx
	shl $16, %edi
	or %edi, %ecx
	ret

	.global m68k_read_byte_scratch1
m68k_read_byte_scratch1:
	call inccycles
	and $0xFFFFFF, %rcx
	cmp $0x400000, %ecx
	jbe cart_b
	cmp $0xE00000, %ecx
	jae workram_b
	cmp $0xA10000, %ecx
	jb not_io_b
	cmp $0xA12000, %ecx
	jae not_io_b
	jmp do_io_read
not_io_b:
	xor %cl, %cl
	dec %cl
	ret
workram_b:
	/* deal with byte swapping */
	xor $1, %ecx
	and $0xFFFF, %rcx
	mov (%r9, %rcx), %cl
	ret
cart_b:
	/* deal with byte swapping */
	xor $1, %ecx
	mov (%r8, %rcx), %cl
	ret
	
ret_addr_msg:
	.asciz "Program modified return address on stack: found %X, expected %X\n"
	
	.global m68k_modified_ret_addr
m68k_modified_ret_addr:
	lea ret_addr_msg(%rip), %rdi
	mov %rcx, %rsi
	mov 8(%rsp), %rdx
	xor %rax, %rax
	call printf
	mov $1, %rdi
	call exit

dyn_addr_msg:
	.asciz "Program needs dynamically calculated native address\n"
	
	.global m68k_native_addr_and_sync
m68k_native_addr_and_sync:
	call m68k_save_context
	push %rcx
	mov %rsi, %rdi
	call sync_components
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
	mov %eax, 80(%rsi) /* current cycle count */
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
	mov 76(%rsi), %ebp /* target cycle count */
	mov 80(%rsi), %eax /* current cycle count */
	mov 96(%rsi), %r8d /* cartridge address */
	mov 104(%rsi), %r9d /* work ram address */
	ret

	.global m68k_start_context
m68k_start_context:
	call m68k_load_context
	jmp *%rdi
