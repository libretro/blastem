/*
 Copyright 2014 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "m68k_core.h"
#include "m68k_internal.h"
#include "68kinst.h"
#include "backend.h"
#include "gen.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

char disasm_buf[1024];

int8_t native_reg(m68k_op_info * op, m68k_options * opts)
{
	if (op->addr_mode == MODE_REG) {
		return opts->dregs[op->params.regs.pri];
	}
	if (op->addr_mode == MODE_AREG) {
		return opts->aregs[op->params.regs.pri];
	}
	return -1;
}

size_t dreg_offset(uint8_t reg)
{
	return offsetof(m68k_context, dregs) + sizeof(uint32_t) * reg;
}

size_t areg_offset(uint8_t reg)
{
	return offsetof(m68k_context, aregs) + sizeof(uint32_t) * reg;
}

//must be called with an m68k_op_info that uses a register
size_t reg_offset(m68k_op_info *op)
{
	return op->addr_mode == MODE_REG ? dreg_offset(op->params.regs.pri) : areg_offset(op->params.regs.pri);
}

void print_regs_exit(m68k_context * context)
{
	printf("XNZVC\n%d%d%d%d%d\n", context->flags[0], context->flags[1], context->flags[2], context->flags[3], context->flags[4]);
	for (int i = 0; i < 8; i++) {
		printf("d%d: %X\n", i, context->dregs[i]);
	}
	for (int i = 0; i < 8; i++) {
		printf("a%d: %X\n", i, context->aregs[i]);
	}
	exit(0);
}

void m68k_read_size(m68k_options *opts, uint8_t size)
{
	switch (size)
	{
	case OPSIZE_BYTE:
		call(&opts->gen.code, opts->read_8);
		break;
	case OPSIZE_WORD:
		call(&opts->gen.code, opts->read_16);
		break;
	case OPSIZE_LONG:
		call(&opts->gen.code, opts->read_32);
		break;
	}
}

void m68k_write_size(m68k_options *opts, uint8_t size)
{
	switch (size)
	{
	case OPSIZE_BYTE:
		call(&opts->gen.code, opts->write_8);
		break;
	case OPSIZE_WORD:
		call(&opts->gen.code, opts->write_16);
		break;
	case OPSIZE_LONG:
		call(&opts->gen.code, opts->write_32_highfirst);
		break;
	}
}

void translate_m68k_lea_pea(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t dst_reg = inst->op == M68K_PEA ? opts->gen.scratch1 : native_reg(&(inst->dst), opts);
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		cycles(&opts->gen, BUS);
		if (dst_reg >= 0) {
			areg_to_native(opts, inst->src.params.regs.pri, dst_reg);
		} else {
			if (opts->aregs[inst->src.params.regs.pri] >= 0) {
				native_to_areg(opts, opts->aregs[inst->src.params.regs.pri], inst->dst.params.regs.pri);
			} else {
				areg_to_native(opts, inst->src.params.regs.pri, opts->gen.scratch1);
				native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
			}
		}
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, 8);
		calc_areg_displace(opts, &inst->src, dst_reg >= 0 ? dst_reg : opts->gen.scratch1);
		if (dst_reg < 0) {
			native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
		}
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, 12);
		if (dst_reg < 0 || inst->dst.params.regs.pri == inst->src.params.regs.pri || inst->dst.params.regs.pri == (inst->src.params.regs.sec >> 1 & 0x7)) {
			dst_reg = opts->gen.scratch1;
		}
		calc_areg_index_disp8(opts, &inst->src, dst_reg);
		if (dst_reg == opts->gen.scratch1 && inst->op != M68K_PEA) {
			native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
		}
		break;
	case MODE_PC_DISPLACE:
		cycles(&opts->gen, 8);
		if (inst->op == M68K_PEA) {
			ldi_native(opts, inst->src.params.regs.displacement + inst->address+2, dst_reg);
		} else {
			ldi_areg(opts, inst->src.params.regs.displacement + inst->address+2, inst->dst.params.regs.pri);
		}
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);
		if (dst_reg < 0 || inst->dst.params.regs.pri == (inst->src.params.regs.sec >> 1 & 0x7)) {
			dst_reg = opts->gen.scratch1;
		}
		ldi_native(opts, inst->address+2, dst_reg);
		calc_index_disp8(opts, &inst->src, dst_reg);
		if (dst_reg == opts->gen.scratch1 && inst->op != M68K_PEA) {
			native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
		}
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		cycles(&opts->gen, (inst->src.addr_mode == MODE_ABSOLUTE) ? BUS * 3 : BUS * 2);
		if (inst->op == M68K_PEA) {
			ldi_native(opts, inst->src.params.immed, dst_reg);
		} else {
			ldi_areg(opts, inst->src.params.immed, inst->dst.params.regs.pri);
		}
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		exit(1);
	}
	if (inst->op == M68K_PEA) {
		subi_areg(opts, 4, 7);
		areg_to_native(opts, 7, opts->gen.scratch2);
		call(code, opts->write_32_lowfirst);
	}
}

void push_const(m68k_options *opts, int32_t value)
{
	ldi_native(opts, value, opts->gen.scratch1);
	subi_areg(opts, 4, 7);
	areg_to_native(opts, 7, opts->gen.scratch2);
	call(&opts->gen.code, opts->write_32_highfirst);
}

void jump_m68k_abs(m68k_options * opts, uint32_t address)
{
	code_info *code = &opts->gen.code;
	code_ptr dest_addr = get_native_address(opts->gen.native_code_map, address);
	if (!dest_addr) {
		opts->gen.deferred = defer_address(opts->gen.deferred, address, code->cur + 1);
		//dummy address to be replaced later, make sure it generates a 4-byte displacement
		dest_addr = code->cur + 256;
	}
	jmp(code, dest_addr);
	//this used to call opts->native_addr for destinations in RAM, but that shouldn't be needed
	//since instruction retranslation patches the original native instruction location
}

void translate_m68k_bsr(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int32_t disp = inst->src.params.immed;
	uint32_t after = inst->address + (inst->variant == VAR_BYTE ? 2 : 4);
	//TODO: Add cycles in the right place relative to pushing the return address on the stack
	cycles(&opts->gen, 10);
	push_const(opts, after);
	jump_m68k_abs(opts, inst->address + 2 + disp);
}

void translate_m68k_jmp_jsr(m68k_options * opts, m68kinst * inst)
{
	uint8_t is_jsr = inst->op == M68K_JSR;
	code_info *code = &opts->gen.code;
	code_ptr dest_addr;
	uint8_t sec_reg;
	uint32_t after;
	uint32_t m68k_addr;
	switch(inst->src.addr_mode)
	{
	case MODE_AREG_INDIRECT:
		cycles(&opts->gen, BUS*2);
		if (is_jsr) {
			push_const(opts, inst->address+2);
		}
		areg_to_native(opts, inst->src.params.regs.pri, opts->gen.scratch1);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_AREG_DISPLACE:
		cycles(&opts->gen, BUS*2);
		if (is_jsr) {
			push_const(opts, inst->address+4);
		}
		calc_areg_displace(opts, &inst->src, opts->gen.scratch1);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_AREG_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);//TODO: CHeck that this is correct
		if (is_jsr) {
			push_const(opts, inst->address+4);
		}
		calc_areg_index_disp8(opts, &inst->src, opts->gen.scratch1);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_PC_DISPLACE:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		cycles(&opts->gen, 10);
		if (is_jsr) {
			push_const(opts, inst->address+4);
		}
		jump_m68k_abs(opts, inst->src.params.regs.displacement + inst->address + 2);
		break;
	case MODE_PC_INDEX_DISP8:
		cycles(&opts->gen, BUS*3);//TODO: CHeck that this is correct
		if (is_jsr) {
			push_const(opts, inst->address+4);
		}
		ldi_native(opts, inst->address+2, opts->gen.scratch1);
		calc_index_disp8(opts, &inst->src, opts->gen.scratch1);
		call(code, opts->native_addr);
		jmp_r(code, opts->gen.scratch1);
		break;
	case MODE_ABSOLUTE:
	case MODE_ABSOLUTE_SHORT:
		//TODO: Add cycles in the right place relative to pushing the return address on the stack
		cycles(&opts->gen, inst->src.addr_mode == MODE_ABSOLUTE ? 12 : 10);
		if (is_jsr) {
			push_const(opts, inst->address + (inst->src.addr_mode == MODE_ABSOLUTE ? 6 : 4));
		}
		jump_m68k_abs(opts, inst->src.params.immed);
		break;
	default:
		m68k_disasm(inst, disasm_buf);
		printf("%s\naddress mode %d not yet supported (%s)\n", disasm_buf, inst->src.addr_mode, is_jsr ? "jsr" : "jmp");
		exit(1);
	}
}

void translate_m68k_unlk(m68k_options * opts, m68kinst * inst)
{
	cycles(&opts->gen, BUS);
	areg_to_native(opts, inst->dst.params.regs.pri, opts->aregs[7]);
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(&opts->gen.code, opts->read_32);
	native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
	addi_areg(opts, 4, 7);
}

void translate_m68k_link(m68k_options * opts, m68kinst * inst)
{
	//compensate for displacement word
	cycles(&opts->gen, BUS);
	subi_areg(opts, 4, 7);
	areg_to_native(opts, 7, opts->gen.scratch2);
	areg_to_native(opts, inst->src.params.regs.pri, opts->gen.scratch1);
	call(&opts->gen.code, opts->write_32_highfirst);
	native_to_areg(opts, opts->aregs[7], inst->src.params.regs.pri);
	addi_areg(opts, inst->dst.params.immed, 7);
	//prefetch
	cycles(&opts->gen, BUS);
}

void translate_m68k_rts(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	//TODO: Add cycles
	areg_to_native(opts, 7, opts->gen.scratch1);
	addi_areg(opts, 4, 7);
	call(code, opts->read_32);
	call(code, opts->native_addr);
	jmp_r(code, opts->gen.scratch1);
}

void translate_m68k_rtr(m68k_options *opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	//Read saved CCR
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_16);
	addi_areg(opts, 2, 7);
	call(code, opts->set_ccr);
	//Read saved PC
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_32);
	addi_areg(opts, 4, 7);
	//Get native address and jump to it
	call(code, opts->native_addr);
	jmp_r(code, opts->gen.scratch1);
}

void translate_m68k_trap(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	ldi_native(opts, inst->src.params.immed + VECTOR_TRAP_0, opts->gen.scratch2);
	ldi_native(opts, inst->address+2, opts->gen.scratch1);
	jmp(code, opts->trap);
}

void swap_ssp_usp(m68k_options * opts)
{
	areg_to_native(opts, 7, opts->gen.scratch2);
	areg_to_native(opts, 8, opts->aregs[7]);
	native_to_areg(opts, opts->gen.scratch2, 8);
}

code_ptr get_native_address(native_map_slot * native_code_map, uint32_t address)
{
	address &= 0xFFFFFF;
	address /= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		return NULL;
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET || native_code_map[chunk].offsets[offset] == EXTENSION_WORD) {
		return NULL;
	}
	return native_code_map[chunk].base + native_code_map[chunk].offsets[offset];
}

code_ptr get_native_from_context(m68k_context * context, uint32_t address)
{
	return get_native_address(context->native_code_map, address);
}

uint32_t get_instruction_start(native_map_slot * native_code_map, uint32_t address)
{
	address &= 0xFFFFFF;
	address /= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		return 0;
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET) {
		return 0;
	}
	while (native_code_map[chunk].offsets[offset] == EXTENSION_WORD) {
		--address;
		chunk = address / NATIVE_CHUNK_SIZE;
		offset = address % NATIVE_CHUNK_SIZE;
	}
	return address*2;
}

void map_native_address(m68k_context * context, uint32_t address, code_ptr native_addr, uint8_t size, uint8_t native_size)
{
	native_map_slot * native_code_map = context->native_code_map;
	m68k_options * opts = context->options;
	address &= 0xFFFFFF;
	if (address > 0xE00000) {
		context->ram_code_flags[(address & 0xC000) >> 14] |= 1 << ((address & 0x3800) >> 11);
		if (((address & 0x3FFF) + size) & 0xC000) {
			context->ram_code_flags[((address+size) & 0xC000) >> 14] |= 1 << (((address+size) & 0x3800) >> 11);
		}
		uint32_t slot = (address & 0xFFFF)/1024;
		if (!opts->gen.ram_inst_sizes[slot]) {
			opts->gen.ram_inst_sizes[slot] = malloc(sizeof(uint8_t) * 512);
		}
		opts->gen.ram_inst_sizes[slot][((address & 0xFFFF)/2)%512] = native_size;
	}
	address/= 2;
	uint32_t chunk = address / NATIVE_CHUNK_SIZE;
	if (!native_code_map[chunk].base) {
		native_code_map[chunk].base = native_addr;
		native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
	}
	uint32_t offset = address % NATIVE_CHUNK_SIZE;
	native_code_map[chunk].offsets[offset] = native_addr-native_code_map[chunk].base;
	for(address++,size-=2; size; address++,size-=2) {
		chunk = address / NATIVE_CHUNK_SIZE;
		offset = address % NATIVE_CHUNK_SIZE;
		if (!native_code_map[chunk].base) {
			native_code_map[chunk].base = native_addr;
			native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
			memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		}
		native_code_map[chunk].offsets[offset] = EXTENSION_WORD;
	}
}

uint8_t get_native_inst_size(m68k_options * opts, uint32_t address)
{
	if (address < 0xE00000) {
		return 0;
	}
	uint32_t slot = (address & 0xFFFF)/1024;
	return opts->gen.ram_inst_sizes[slot][((address & 0xFFFF)/2)%512];
}

uint8_t m68k_is_terminal(m68kinst * inst)
{
	return inst->op == M68K_RTS || inst->op == M68K_RTE || inst->op == M68K_RTR || inst->op == M68K_JMP
		|| inst->op == M68K_TRAP || inst->op == M68K_ILLEGAL || inst->op == M68K_INVALID || inst->op == M68K_RESET
		|| (inst->op == M68K_BCC && inst->extra.cond == COND_TRUE);
}

void m68k_handle_deferred(m68k_context * context)
{
	m68k_options * opts = context->options;
	process_deferred(&opts->gen.deferred, context, (native_addr_func)get_native_from_context);
	if (opts->gen.deferred) {
		translate_m68k_stream(opts->gen.deferred->address, context);
	}
}

void translate_m68k_stream(uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	m68k_options * opts = context->options;
	code_info *code = &opts->gen.code;
	address &= 0xFFFFFF;
	if(get_native_address(opts->gen.native_code_map, address)) {
		return;
	}
	char disbuf[1024];
	uint16_t *encoded, *next;
	if ((address & 0xFFFFFF) < 0x400000) {
		encoded = context->mem_pointers[0] + (address & 0xFFFFFF)/2;
	} else if ((address & 0xFFFFFF) > 0xE00000) {
		encoded = context->mem_pointers[1] + (address  & 0xFFFF)/2;
	} else {
		printf("attempt to translate non-memory address: %X\n", address);
		exit(1);
	}
	do {
		if (opts->address_log) {
			fprintf(opts->address_log, "%X\n", address);
		}
		do {
			if (address >= 0x400000 && address < 0xE00000) {
				translate_out_of_bounds(code);
				break;
			}
			code_ptr existing = get_native_address(opts->gen.native_code_map, address);
			if (existing) {
				jmp(code, existing);
				break;
			}
			next = m68k_decode(encoded, &instbuf, address);
			if (instbuf.op == M68K_INVALID) {
				instbuf.src.params.immed = *encoded;
			}
			uint16_t m68k_size = (next-encoded)*2;
			address += m68k_size;
			encoded = next;
			//m68k_disasm(&instbuf, disbuf);
			//printf("%X: %s\n", instbuf.address, disbuf);

			//make sure the beginning of the code for an instruction is contiguous
			check_code_prologue(code);
			code_ptr start = code->cur;
			translate_m68k(opts, &instbuf);
			code_ptr after = code->cur;
			map_native_address(context, instbuf.address, start, m68k_size, after-start);
		} while(!m68k_is_terminal(&instbuf));
		process_deferred(&opts->gen.deferred, context, (native_addr_func)get_native_from_context);
		if (opts->gen.deferred) {
			address = opts->gen.deferred->address;
			if ((address & 0xFFFFFF) < 0x400000) {
				encoded = context->mem_pointers[0] + (address & 0xFFFFFF)/2;
			} else if ((address & 0xFFFFFF) > 0xE00000) {
				encoded = context->mem_pointers[1] + (address  & 0xFFFF)/2;
			} else {
				printf("attempt to translate non-memory address: %X\n", address);
				exit(1);
			}
		} else {
			encoded = NULL;
		}
	} while(encoded != NULL);
}

code_ptr get_native_address_trans(m68k_context * context, uint32_t address)
{
	address &= 0xFFFFFF;
	code_ptr ret = get_native_address(context->native_code_map, address);
	if (!ret) {
		translate_m68k_stream(address, context);
		ret = get_native_address(context->native_code_map, address);
	}
	return ret;
}

void remove_breakpoint(m68k_context * context, uint32_t address)
{
	code_ptr native = get_native_address(context->native_code_map, address);
	check_cycles_int(context->options, address);
}

void start_68k_context(m68k_context * context, uint32_t address)
{
	code_ptr addr = get_native_address_trans(context, address);
	m68k_options * options = context->options;
	options->start_context(addr, context);
}

void m68k_reset(m68k_context * context)
{
	//TODO: Make this actually use the normal read functions
	context->aregs[7] = context->mem_pointers[0][0] << 16 | context->mem_pointers[0][1];
	uint32_t address = context->mem_pointers[0][2] << 16 | context->mem_pointers[0][3];
	start_68k_context(context, address);
}


void init_68k_context(m68k_context * context, native_map_slot * native_code_map, void * opts)
{
	memset(context, 0, sizeof(m68k_context));
	context->native_code_map = native_code_map;
	context->options = opts;
	context->int_cycle = 0xFFFFFFFF;
	context->status = 0x27;
}
