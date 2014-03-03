/*
 Copyright 2014 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef M68K_INTERNAL_H_
#define M68K_INTERNAL_H_

#include "68kinst.h"

//functions implemented in host CPU specfic file
void translate_out_of_bounds(code_info *code);
void check_code_prologue(code_info *code);

//functions implemented in m68k_core.c
int8_t native_reg(m68k_op_info * op, m68k_options * opts);
size_t reg_offset(m68k_op_info *op);
void print_regs_exit(m68k_context * context);
void m68k_read_size(m68k_options *opts, uint8_t size);
void m68k_write_size(m68k_options *opts, uint8_t size);
code_ptr get_native_address(native_map_slot * native_code_map, uint32_t address);
void map_native_address(m68k_context * context, uint32_t address, code_ptr native_addr, uint8_t size, uint8_t native_size);
uint8_t get_native_inst_size(m68k_options * opts, uint32_t address);
uint8_t m68k_is_terminal(m68kinst * inst);
void m68k_handle_deferred(m68k_context * context);
code_ptr get_native_address_trans(m68k_context * context, uint32_t address);

#endif //M68K_INTERNAL_H_
