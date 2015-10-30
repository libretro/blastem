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
#include "util.h"
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

void m68k_save_result(m68kinst * inst, m68k_options * opts)
{
	code_info *code = &opts->gen.code;
	if (inst->dst.addr_mode != MODE_REG && inst->dst.addr_mode != MODE_AREG && inst->dst.addr_mode != MODE_UNUSED) {
		if (inst->dst.addr_mode == MODE_AREG_PREDEC && inst->src.addr_mode == MODE_AREG_PREDEC && inst->op != M68K_MOVE) {
			areg_to_native(opts, inst->dst.params.regs.pri, opts->gen.scratch2);
		}
		switch (inst->extra.size)
		{
		case OPSIZE_BYTE:
			call(code, opts->write_8);
			break;
		case OPSIZE_WORD:
			call(code, opts->write_16);
			break;
		case OPSIZE_LONG:
			call(code, opts->write_32_lowfirst);
			break;
		}
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
		fatal_error("%X: %s\naddress mode %d not implemented (lea src)\n", inst->address, disasm_buf, inst->src.addr_mode);
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
	code_ptr dest_addr = get_native_address(opts, address);
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
		fatal_error("%s\naddress mode %d not yet supported (%s)\n", disasm_buf, inst->src.addr_mode, is_jsr ? "jsr" : "jmp");
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

void translate_m68k_move_usp(m68k_options *opts, m68kinst *inst)
{
	cycles(&opts->gen, BUS);
	int8_t reg;
	if (inst->src.addr_mode == MODE_UNUSED) {
		reg = native_reg(&inst->dst, opts);
		if (reg < 0) {
			reg = opts->gen.scratch1;
		}
		areg_to_native(opts, 8, reg);
		if (reg == opts->gen.scratch1) {
			native_to_areg(opts, opts->gen.scratch1, inst->dst.params.regs.pri);
		}
	} else {
		reg = native_reg(&inst->src, opts);
		if (reg < 0) {
			reg = opts->gen.scratch1;
			areg_to_native(opts, inst->src.params.regs.pri, reg);
		}
		native_to_areg(opts, reg, 8);
	}
}

void translate_m68k_movem(m68k_options * opts, m68kinst * inst)
{
	code_info *code = &opts->gen.code;
	int8_t bit,reg,sec_reg;
	uint8_t early_cycles;
	if(inst->src.addr_mode == MODE_REG) {
		//reg to mem
		early_cycles = 8;
		int8_t dir;
		switch (inst->dst.addr_mode)
		{
		case MODE_AREG_INDIRECT:
		case MODE_AREG_PREDEC:
			areg_to_native(opts, inst->dst.params.regs.pri, opts->gen.scratch2);
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			calc_areg_displace(opts, &inst->dst, opts->gen.scratch2);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			calc_areg_index_disp8(opts, &inst->dst, opts->gen.scratch2);
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			ldi_native(opts, inst->dst.params.regs.displacement + inst->address+2, opts->gen.scratch2);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			ldi_native(opts, inst->address+2, opts->gen.scratch2);
			calc_index_disp8(opts, &inst->dst, opts->gen.scratch2);
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			ldi_native(opts, inst->dst.params.immed, opts->gen.scratch2);
			break;
		default:
			m68k_disasm(inst, disasm_buf);
			fatal_error("%X: %s\naddress mode %d not implemented (movem dst)\n", inst->address, disasm_buf, inst->dst.addr_mode);
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			reg = 15;
			dir = -1;
		} else {
			reg = 0;
			dir = 1;
		}
		cycles(&opts->gen, early_cycles);
		for(bit=0; reg < 16 && reg >= 0; reg += dir, bit++) {
			if (inst->src.params.immed & (1 << bit)) {
				if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
					subi_native(opts, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch2);
				}
				push_native(opts, opts->gen.scratch2);
				if (reg > 7) {
					areg_to_native(opts, reg-8, opts->gen.scratch1);
				} else {
					dreg_to_native(opts, reg, opts->gen.scratch1);
				}
				if (inst->extra.size == OPSIZE_LONG) {
					call(code, opts->write_32_lowfirst);
				} else {
					call(code, opts->write_16);
				}
				pop_native(opts, opts->gen.scratch2);
				if (inst->dst.addr_mode != MODE_AREG_PREDEC) {
					addi_native(opts, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch2);
				}
			}
		}
		if (inst->dst.addr_mode == MODE_AREG_PREDEC) {
			native_to_areg(opts, opts->gen.scratch2, inst->dst.params.regs.pri);
		}
	} else {
		//mem to reg
		early_cycles = 4;
		switch (inst->src.addr_mode)
		{
		case MODE_AREG_INDIRECT:
		case MODE_AREG_POSTINC:
			areg_to_native(opts, inst->src.params.regs.pri, opts->gen.scratch1);
			break;
		case MODE_AREG_DISPLACE:
			early_cycles += BUS;
			reg = opts->gen.scratch2;
			calc_areg_displace(opts, &inst->src, opts->gen.scratch1);
			break;
		case MODE_AREG_INDEX_DISP8:
			early_cycles += 6;
			calc_areg_index_disp8(opts, &inst->src, opts->gen.scratch1);
			break;
		case MODE_PC_DISPLACE:
			early_cycles += BUS;
			ldi_native(opts, inst->src.params.regs.displacement + inst->address+2, opts->gen.scratch1);
			break;
		case MODE_PC_INDEX_DISP8:
			early_cycles += 6;
			ldi_native(opts, inst->address+2, opts->gen.scratch1);
			calc_index_disp8(opts, &inst->src, opts->gen.scratch1);
			break;
		case MODE_ABSOLUTE:
			early_cycles += 4;
		case MODE_ABSOLUTE_SHORT:
			early_cycles += 4;
			ldi_native(opts, inst->src.params.immed, opts->gen.scratch1);
			break;
		default:
			m68k_disasm(inst, disasm_buf);
			fatal_error("%X: %s\naddress mode %d not implemented (movem src)\n", inst->address, disasm_buf, inst->src.addr_mode);
		}
		cycles(&opts->gen, early_cycles);
		for(reg = 0; reg < 16; reg ++) {
			if (inst->dst.params.immed & (1 << reg)) {
				push_native(opts, opts->gen.scratch1);
				if (inst->extra.size == OPSIZE_LONG) {
					call(code, opts->read_32);
				} else {
					call(code, opts->read_16);
				}
				if (inst->extra.size == OPSIZE_WORD) {
					sign_extend16_native(opts, opts->gen.scratch1);
				}
				if (reg > 7) {
					native_to_areg(opts, opts->gen.scratch1, reg-8);
				} else {
					native_to_dreg(opts, opts->gen.scratch1, reg);
				}
				pop_native(opts, opts->gen.scratch1);
				addi_native(opts, (inst->extra.size == OPSIZE_LONG) ? 4 : 2, opts->gen.scratch1);
			}
		}
		if (inst->src.addr_mode == MODE_AREG_POSTINC) {
			native_to_areg(opts, opts->gen.scratch1, inst->src.params.regs.pri);
		}
	}
	//prefetch
	cycles(&opts->gen, 4);
}

void translate_m68k_nop(m68k_options *opts, m68kinst *inst)
{
	cycles(&opts->gen, BUS);
}

void swap_ssp_usp(m68k_options * opts)
{
	areg_to_native(opts, 7, opts->gen.scratch2);
	areg_to_native(opts, 8, opts->aregs[7]);
	native_to_areg(opts, opts->gen.scratch2, 8);
}

void translate_m68k_reset(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	call(code, opts->gen.save_context);
	call_args(code, (code_ptr)print_regs_exit, 1, opts->gen.context_reg);
}

void translate_m68k_rte(m68k_options *opts, m68kinst *inst)
{
	code_info *code = &opts->gen.code;
	//TODO: Trap if not in system mode
	//Read saved SR
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_16);
	addi_areg(opts, 2, 7);
	call(code, opts->set_sr);
	//Read saved PC
	areg_to_native(opts, 7, opts->gen.scratch1);
	call(code, opts->read_32);
	addi_areg(opts, 4, 7);
	check_user_mode_swap_ssp_usp(opts);
	//Get native address, sync components, recalculate integer points and jump to returned address
	call(code, opts->native_addr_and_sync);
	jmp_r(code, opts->gen.scratch1);
}

code_ptr get_native_address(m68k_options *opts, uint32_t address)
{
	native_map_slot * native_code_map = opts->gen.native_code_map;
	address &= opts->gen.address_mask;
	if (address & 1) {
		return opts->odd_address;
	}
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
	return get_native_address(context->options, address);
}

uint32_t get_instruction_start(native_map_slot * native_code_map, uint32_t address)
{
	//FIXME: Use opts->gen.address_mask
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
	address &= opts->gen.address_mask;
	uint32_t meta_off = 0;
	//TODO: Refactor part of this loop into some kind of get_ram_chunk function
	for (int i = 0; i < opts->gen.memmap_chunks; i++) {
		if (address >= opts->gen.memmap[i].start && address < opts->gen.memmap[i].end) {
			if ((opts->gen.memmap[i].flags & (MMAP_WRITE | MMAP_CODE)) == (MMAP_WRITE | MMAP_CODE)) {
				uint32_t masked = (address & opts->gen.memmap[i].mask);
				uint32_t final_off = masked + meta_off;
				uint32_t ram_flags_off = final_off >> (opts->gen.ram_flags_shift + 3);
				context->ram_code_flags[ram_flags_off] |= 1 << ((final_off >> opts->gen.ram_flags_shift) & 7);

				uint32_t slot = final_off / 1024;
				if (!opts->gen.ram_inst_sizes[slot]) {
					opts->gen.ram_inst_sizes[slot] = malloc(sizeof(uint8_t) * 512);
				}
				opts->gen.ram_inst_sizes[slot][(final_off/2) & 511] = native_size;

				//TODO: Deal with case in which end of instruction is in a different memory chunk
				masked = (address + size - 1) & opts->gen.memmap[i].mask;
				final_off = masked + meta_off;
				ram_flags_off = final_off >> (opts->gen.ram_flags_shift + 3);
				context->ram_code_flags[ram_flags_off] |= 1 << ((final_off >> opts->gen.ram_flags_shift) & 7);
			}
			break;
		} else if ((opts->gen.memmap[i].flags & (MMAP_WRITE | MMAP_CODE)) == (MMAP_WRITE | MMAP_CODE)) {
			uint32_t size = chunk_size(&opts->gen, opts->gen.memmap + i);
			meta_off += size;
		}
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
		address &= opts->gen.address_mask >> 1;
		chunk = address / NATIVE_CHUNK_SIZE;
		offset = address % NATIVE_CHUNK_SIZE;
		if (!native_code_map[chunk].base) {
			native_code_map[chunk].base = native_addr;
			native_code_map[chunk].offsets = malloc(sizeof(int32_t) * NATIVE_CHUNK_SIZE);
			memset(native_code_map[chunk].offsets, 0xFF, sizeof(int32_t) * NATIVE_CHUNK_SIZE);
		}
		if (native_code_map[chunk].offsets[offset] == INVALID_OFFSET) {
			//TODO: Better handling of overlapping instructions
			native_code_map[chunk].offsets[offset] = EXTENSION_WORD;
		}
	}
}

uint8_t get_native_inst_size(m68k_options * opts, uint32_t address)
{
	address &= opts->gen.address_mask;
	uint32_t meta_off = 0;
	for (int i = 0; i < opts->gen.memmap_chunks; i++) {
		if (address >= opts->gen.memmap[i].start && address < opts->gen.memmap[i].end) {
			if ((opts->gen.memmap[i].flags & (MMAP_WRITE | MMAP_CODE)) != (MMAP_WRITE | MMAP_CODE)) {
				return 0;
			}
			meta_off += address & opts->gen.memmap[i].mask;
			break;
		} else if ((opts->gen.memmap[i].flags & (MMAP_WRITE | MMAP_CODE)) == (MMAP_WRITE | MMAP_CODE)) {
			uint32_t size = chunk_size(&opts->gen, opts->gen.memmap + i);
			meta_off += size;
		}
	}
	uint32_t slot = meta_off/1024;
	return opts->gen.ram_inst_sizes[slot][(meta_off/2)%512];
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

typedef enum {
	RAW_FUNC = 1,
	BINARY_ARITH,
	UNARY_ARITH,
	OP_FUNC
} impl_type;

typedef void (*raw_fun)(m68k_options * opts, m68kinst *inst);
typedef void (*op_fun)(m68k_options * opts, m68kinst *inst, host_ea *src_op, host_ea *dst_op);

typedef struct {
	union {
		raw_fun  raw;
		uint32_t flag_mask;
		op_fun   op;
	} impl;
	impl_type itype;
} impl_info;

#define RAW_IMPL(inst, fun)     [inst] = { .impl = { .raw = fun }, .itype = RAW_FUNC }
#define OP_IMPL(inst, fun)      [inst] = { .impl = { .op = fun }, .itype = OP_FUNC }
#define UNARY_IMPL(inst, mask)  [inst] = { .impl = { .flag_mask = mask }, .itype = UNARY_ARITH }
#define BINARY_IMPL(inst, mask) [inst] = { .impl = { .flag_mask = mask}, .itype = BINARY_ARITH }

impl_info m68k_impls[] = {
	//math
	BINARY_IMPL(M68K_ADD, X|N|Z|V|C),
	BINARY_IMPL(M68K_SUB, X|N|Z|V|C),
	//z flag is special cased for ADDX/SUBX
	BINARY_IMPL(M68K_ADDX, X|N|V|C),
	BINARY_IMPL(M68K_SUBX, X|N|V|C),
	OP_IMPL(M68K_ABCD, translate_m68k_abcd_sbcd),
	OP_IMPL(M68K_SBCD, translate_m68k_abcd_sbcd),
	OP_IMPL(M68K_NBCD, translate_m68k_abcd_sbcd),
	BINARY_IMPL(M68K_AND, N|Z|V0|C0),
	BINARY_IMPL(M68K_EOR, N|Z|V0|C0),
	BINARY_IMPL(M68K_OR, N|Z|V0|C0),
	RAW_IMPL(M68K_CMP, translate_m68k_cmp),
	OP_IMPL(M68K_DIVS, translate_m68k_div),
	OP_IMPL(M68K_DIVU, translate_m68k_div),
	OP_IMPL(M68K_MULS, translate_m68k_mul),
	OP_IMPL(M68K_MULU, translate_m68k_mul),
	RAW_IMPL(M68K_EXT, translate_m68k_ext),
	UNARY_IMPL(M68K_NEG, X|N|Z|V|C),
	OP_IMPL(M68K_NEGX, translate_m68k_negx),
	UNARY_IMPL(M68K_NOT, N|Z|V|C),
	UNARY_IMPL(M68K_TST, N|Z|V0|C0),

	//shift/rotate
	OP_IMPL(M68K_ASL, translate_m68k_sl),
	OP_IMPL(M68K_LSL, translate_m68k_sl),
	OP_IMPL(M68K_ASR, translate_m68k_asr),
	OP_IMPL(M68K_LSR, translate_m68k_lsr),
	OP_IMPL(M68K_ROL, translate_m68k_rot),
	OP_IMPL(M68K_ROR, translate_m68k_rot),
	OP_IMPL(M68K_ROXL, translate_m68k_rot),
	OP_IMPL(M68K_ROXR, translate_m68k_rot),
	UNARY_IMPL(M68K_SWAP, N|Z|V0|C0),

	//bit
	OP_IMPL(M68K_BCHG, translate_m68k_bit),
	OP_IMPL(M68K_BCLR, translate_m68k_bit),
	OP_IMPL(M68K_BSET, translate_m68k_bit),
	OP_IMPL(M68K_BTST, translate_m68k_bit),

	//data movement
	RAW_IMPL(M68K_MOVE, translate_m68k_move),
	RAW_IMPL(M68K_MOVEM, translate_m68k_movem),
	RAW_IMPL(M68K_MOVEP, translate_m68k_movep),
	RAW_IMPL(M68K_MOVE_USP, translate_m68k_move_usp),
	RAW_IMPL(M68K_LEA, translate_m68k_lea_pea),
	RAW_IMPL(M68K_PEA, translate_m68k_lea_pea),
	RAW_IMPL(M68K_CLR, translate_m68k_clr),
	OP_IMPL(M68K_EXG, translate_m68k_exg),
	RAW_IMPL(M68K_SCC, translate_m68k_scc),

	//function calls and branches
	RAW_IMPL(M68K_BCC, translate_m68k_bcc),
	RAW_IMPL(M68K_BSR, translate_m68k_bsr),
	RAW_IMPL(M68K_DBCC, translate_m68k_dbcc),
	RAW_IMPL(M68K_JMP, translate_m68k_jmp_jsr),
	RAW_IMPL(M68K_JSR, translate_m68k_jmp_jsr),
	RAW_IMPL(M68K_RTS, translate_m68k_rts),
	RAW_IMPL(M68K_RTE, translate_m68k_rte),
	RAW_IMPL(M68K_RTR, translate_m68k_rtr),
	RAW_IMPL(M68K_LINK, translate_m68k_link),
	RAW_IMPL(M68K_UNLK, translate_m68k_unlk),

	//SR/CCR stuff
	RAW_IMPL(M68K_ANDI_CCR, translate_m68k_andi_ori_ccr_sr),
	RAW_IMPL(M68K_ANDI_SR, translate_m68k_andi_ori_ccr_sr),
	RAW_IMPL(M68K_EORI_CCR, translate_m68k_eori_ccr_sr),
	RAW_IMPL(M68K_EORI_SR, translate_m68k_eori_ccr_sr),
	RAW_IMPL(M68K_ORI_CCR, translate_m68k_andi_ori_ccr_sr),
	RAW_IMPL(M68K_ORI_SR, translate_m68k_andi_ori_ccr_sr),
	OP_IMPL(M68K_MOVE_CCR, translate_m68k_move_ccr_sr),
	OP_IMPL(M68K_MOVE_SR, translate_m68k_move_ccr_sr),
	OP_IMPL(M68K_MOVE_FROM_SR, translate_m68k_move_from_sr),
	RAW_IMPL(M68K_STOP, translate_m68k_stop),

	//traps
	OP_IMPL(M68K_CHK, translate_m68k_chk),
	RAW_IMPL(M68K_TRAP, translate_m68k_trap),
	RAW_IMPL(M68K_TRAPV, translate_m68k_trapv),
	RAW_IMPL(M68K_ILLEGAL, translate_m68k_illegal),
	RAW_IMPL(M68K_INVALID, translate_m68k_invalid),

	//misc
	RAW_IMPL(M68K_NOP, translate_m68k_nop),
	RAW_IMPL(M68K_RESET, translate_m68k_reset),
	RAW_IMPL(M68K_TAS, translate_m68k_tas),
};

void translate_m68k(m68k_options * opts, m68kinst * inst)
{
	check_cycles_int(&opts->gen, inst->address);
	//log_address(&opts->gen, inst->address, "M68K: %X @ %d\n");
	impl_info * info = m68k_impls + inst->op;
	if (info->itype == RAW_FUNC) {
		info->impl.raw(opts, inst);
		return;
	}

	host_ea src_op, dst_op;
	if (inst->src.addr_mode != MODE_UNUSED) {
		translate_m68k_op(inst, &src_op, opts, 0);
	}
	if (inst->dst.addr_mode != MODE_UNUSED) {
		translate_m68k_op(inst, &dst_op, opts, 1);
	}
	if (info->itype == OP_FUNC) {
		info->impl.op(opts, inst, &src_op, &dst_op);
	} else if (info->itype == BINARY_ARITH) {
		translate_m68k_arith(opts, inst, info->impl.flag_mask, &src_op, &dst_op);
	} else if (info->itype == UNARY_ARITH) {
		translate_m68k_unary(opts, inst, info->impl.flag_mask, inst->dst.addr_mode != MODE_UNUSED ? &dst_op : &src_op);
	} else {
		m68k_disasm(inst, disasm_buf);
		fatal_error("%X: %s\ninstruction %d not yet implemented\n", inst->address, disasm_buf, inst->op);
	}
}

void translate_m68k_stream(uint32_t address, m68k_context * context)
{
	m68kinst instbuf;
	m68k_options * opts = context->options;
	code_info *code = &opts->gen.code;
	if(get_native_address(opts, address)) {
		return;
	}
	uint16_t *encoded, *next;
	do {
		if (opts->address_log) {
			fprintf(opts->address_log, "%X\n", address);
			fflush(opts->address_log);
		}
		do {
			if (address & 1) {
				break;
			}
			encoded = get_native_pointer(address, (void **)context->mem_pointers, &opts->gen);
			if (!encoded) {
				map_native_address(context, address, code->cur, 2, 1);
				translate_out_of_bounds(code);
				break;
			}
			code_ptr existing = get_native_address(opts, address);
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
			//char disbuf[1024];
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
		}
	} while(opts->gen.deferred);
}

void * m68k_retranslate_inst(uint32_t address, m68k_context * context)
{
	m68k_options * opts = context->options;
	code_info *code = &opts->gen.code;
	uint8_t orig_size = get_native_inst_size(opts, address);
	code_ptr orig_start = get_native_address(context->options, address);
	uint32_t orig = address;
	code_info orig_code;
	orig_code.cur = orig_start;
	orig_code.last = orig_start + orig_size + 5;
	uint16_t *after, *inst = get_native_pointer(address, (void **)context->mem_pointers, &opts->gen);
	m68kinst instbuf;
	after = m68k_decode(inst, &instbuf, orig);
	if (orig_size != MAX_NATIVE_SIZE) {
		deferred_addr * orig_deferred = opts->gen.deferred;

		//make sure we have enough code space for the max size instruction
		check_alloc_code(code, MAX_NATIVE_SIZE);
		code_ptr native_start = code->cur;
		translate_m68k(opts, &instbuf);
		code_ptr native_end = code->cur;
		/*uint8_t is_terminal = m68k_is_terminal(&instbuf);
		if ((native_end - native_start) <= orig_size) {
			code_ptr native_next;
			if (!is_terminal) {
				native_next = get_native_address(context->native_code_map, orig + (after-inst)*2);
			}
			if (is_terminal || (native_next && ((native_next == orig_start + orig_size) || (orig_size - (native_end - native_start)) > 5))) {
				printf("Using original location: %p\n", orig_code.cur);
				remove_deferred_until(&opts->gen.deferred, orig_deferred);
				code_info tmp;
				tmp.cur = code->cur;
				tmp.last = code->last;
				code->cur = orig_code.cur;
				code->last = orig_code.last;
				translate_m68k(opts, &instbuf);
				native_end = orig_code.cur = code->cur;
				code->cur = tmp.cur;
				code->last = tmp.last;
				if (!is_terminal) {
					nop_fill_or_jmp_next(&orig_code, orig_start + orig_size, native_next);
				}
				m68k_handle_deferred(context);
				return orig_start;
			}
		}*/

		map_native_address(context, instbuf.address, native_start, (after-inst)*2, MAX_NATIVE_SIZE);

		jmp(&orig_code, native_start);
		if (!m68k_is_terminal(&instbuf)) {
			code_ptr native_end = code->cur;
			code->cur = native_start + MAX_NATIVE_SIZE;
			code_ptr rest = get_native_address_trans(context, orig + (after-inst)*2);
			code_ptr tmp = code->cur;
			code->cur = native_end;
			jmp(code, rest);
			code->cur = tmp;
		} else {
			code->cur = native_start + MAX_NATIVE_SIZE;
		}
		m68k_handle_deferred(context);
		return native_start;
	} else {
		code_info tmp = *code;
		*code = orig_code;
		translate_m68k(opts, &instbuf);
		orig_code = *code;
		*code = tmp;
		if (!m68k_is_terminal(&instbuf)) {
			jmp(&orig_code, get_native_address_trans(context, orig + (after-inst)*2));
		}
		m68k_handle_deferred(context);
		return orig_start;
	}
}

code_ptr get_native_address_trans(m68k_context * context, uint32_t address)
{
	code_ptr ret = get_native_address(context->options, address);
	if (!ret) {
		translate_m68k_stream(address, context);
		ret = get_native_address(context->options, address);
	}
	return ret;
}

void remove_breakpoint(m68k_context * context, uint32_t address)
{
	code_ptr native = get_native_address(context->options, address);
	code_info tmp = context->options->gen.code;
	context->options->gen.code.cur = native;
	context->options->gen.code.last = native + MAX_NATIVE_SIZE;
	check_cycles_int(&context->options->gen, address);
	context->options->gen.code = tmp;
}

void start_68k_context(m68k_context * context, uint32_t address)
{
	code_ptr addr = get_native_address_trans(context, address);
	m68k_options * options = context->options;
	options->start_context(addr, context);
}

void m68k_reset(m68k_context * context)
{
	//TODO: Actually execute the M68K reset vector rather than simulating some of its behavior
	uint16_t *reset_vec = get_native_pointer(0, (void **)context->mem_pointers, &context->options->gen);
	context->aregs[7] = reset_vec[0] << 16 | reset_vec[1];
	uint32_t address = reset_vec[2] << 16 | reset_vec[3];
	start_68k_context(context, address);
}


m68k_context * init_68k_context(m68k_options * opts)
{
	size_t ctx_size = sizeof(m68k_context) + ram_size(&opts->gen) / (1 << opts->gen.ram_flags_shift) / 8;
	m68k_context * context = malloc(ctx_size);
	memset(context, 0, ctx_size);
	context->native_code_map = opts->gen.native_code_map;
	context->options = opts;
	context->int_cycle = CYCLE_NEVER;
	context->status = 0x27;
	return context;
}
