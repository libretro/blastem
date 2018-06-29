#include <stdint.h>
#include "backend.h"
#include "jagcpu.h"
#include "gen_x86.h"

void jag_check_resultwrite(jag_cpu_options *opts, uint8_t src, uint8_t dst)
{
	code_info *code = &opts->gen.code;
	cmp_ir(code, JAGCPU_NOREG, opts->resultreg, SZ_B);
	code_ptr no_result = code->cur + 1;
	jcc(code, CC_Z, no_result + 1);
	//save result to current bank
	movzx_rr(code, opts->resultreg, opts->scratch1, SZ_B, SZ_D);
	mov_rrindex(code, opts->result, opts->bankptr, opts->scratch1, 4, SZ_D);
	
	code_ptr writeback_penatly;
	if (dst == src) {
		//unclear if src and dst read can share a port if it's the same register
		//assume that is the case for now
		writeback_penalty = NULL;
	} else {
		cmp_ir(code, JAGCPU_NOREG, opts->writeback, SZ_B);
		code_ptr no_writeback_penalty = code->cur + 1;
		jcc(code, CC_Z, no_writeback_penalty + 1);
		cmp_ir(code, src, opts->writeback, SZ_B);
		code_ptr no_writeback_penalty2 = code->cur + 1;
		jcc(code, CC_Z, no_writeback_penalty2 + 1);
		cmp_ir(code, dst, opts->writeback, SZ_B);
		writeback_penalty = code->cur + 1;
		jcc(code, CC_NZ, writeback_penalty + 1);
		*no_writeback_penalty = code->cur - (no_writeback_penalty + 1);
		*no_writeback_penalty2 = code->cur - (no_writeback_penalty2 + 1);
	}
	cmp_ir(code, src, opts->resultreg, SZ_B);
	code_ptr no_result_penalty;
	if (dst == src) {
		no_result_penalty = code->cur+1;
		jcc(code, CC_NZ, no_result_penalty+1);
	} else {
		code_ptr result_penalty = code->cur + 1;
		jcc(code, CC_Z, result_penalty+1);
		cmp_ir(code, dst, opts->resultreg, SZ_B);
		no_result_penalty = code->cur+1;
		jcc(code, CC_NZ, no_result_penalty+1);
		*result_penalty = code->cur - (result_penalty + 1);
	}
	code_ptr penalty = code->cur;
	cycles(code, 1);
	*no_result_penalty = code->cur - (no_result_penalty + 1);
	code_ptr end = code->cur + 1;
	jmp(end + 1);
	//No result to save, but there could still be a writeback, source read conflict
	code_ptr end2 = NULL;
	
	cmp_ir(code, JAGCPU_NOREG, opts->writeback, SZ_B);
	code_ptr no_resultreg_move = code->cur + 1;
	jcc(code, CC_Z, no_resultreg_move+1);
	
	if (src != dst) {
		//unclear if src and dst read can share a port if it's the same register
		//assume that is the case for now
		cmp_ir(code, src, opts->writeback, SZ_B);
		end2 = code->cur + 1;
		jcc(code, CC_Z, end2+1);
		cmp_ir(code, src, opts->writeback, SZ_B);
		jcc(code, CC_NZ, penalty)
	}
	*end = code->cur - (end + 1);
	if (end2) {
		*end2 = code->cur - (end2 + 1);
	}
	*no_result = code->cur - (no_result + 1);
	mov_rr(code, opts->resultreg, opts->writeback, SZ_B);
	*no_resultreg_move = code->cur - (no_resultreg_move + 1);
}

void jag_check_resultwrite_noread(jag_cpu_options *opts)
{
	code_info *code = &opts->gen.code;
	cmp_ir(code, JAGCPU_NOREG, opts->resultreg, SZ_B);
	code_ptr no_result = code->cur + 1;
	jcc(code, CC_Z, no_result + 1);
	//save result to current bank
	movzx_rr(code, opts->resultreg, opts->scratch1, SZ_B, SZ_D);
	mov_rrindex(code, opts->result, opts->bankptr, opts->scratch1, 4, SZ_D);
	*no_result = code->cur - (no_result + 1);
	mov_rr(code, opts->resultreg, opts->writeback, SZ_B);
}

void jag_check_resultwrite_singleread(jag_cpu options *opts, uint8_t reg)
{
	code_info *code = &opts->gen.code;
	cmp_ir(code, JAGCPU_NOREG, opts->resultreg, SZ_B);
	code_ptr no_result = code->cur + 1;
	jcc(code, CC_Z, no_result + 1);
	//save result to current bank
	movzx_rr(code, opts->resultreg, opts->scratch1, SZ_B, SZ_D);
	mov_rrindex(code, opts->result, opts->bankptr, opts->scratch1, 4, SZ_D);
	
	//check for a scoreboard delay
	cmp_ir(code, reg, opts->resultreg, SZ_B);
	code_ptr no_delay = code-.cur + 1;
	jcc(code, CC_NZ, no_delay + 1);
	ccylces(code, 1);
	*no_delay = code->cur - (no_delay + 1);
	*no_result = code->cur - (no_result + 1);
	mov_rr(code, opts->resultreg, opts->writeback, SZ_B);
}

void translate_jag_quickimmed(jag_cpu_options *opts, uint32_t address, uint16_t opcode, uint32_t value, uint16_t dest)
{
	jag_check_resultwrite_singleread(opts, dest);
	switch (opcode)
	{
	case ADDQ:
		break;
	case ADDQT:
		break;
	}
}


uint16_t *translate_jag_inst(uint16_t *stream, jag_cpu_options *opts, uint32_t address)
{
	uint16_t inst = *stream;
	++stream;
	uint16_t opcode = jag_opcode(inst, opts->is_gpu);
	check_cycles_int(&opts->gen, address);
	code_info *code = &opts->gen.code;
	switch (opcode)
	{
	case JAG_MOVEI: {
		uint32_t value = *stream;
		++stream;
		value |= *stream << 16;
		++stream;
		
		jag_check_resultwrite_noread(opts);
		mov_ir(code, value, opts->result, SZ_D);
		mov_ir(code, jag_reg2(inst), opts->resultreg, SZ_B);
		break;
		}
	case JAG_MOVE: {
		uint8_t src = jag_reg1(inst), dst = jag_reg2(inst);
		jag_check_resultwrite_singleread(opts, src);
		//move has a shorter pipeline than normal instructions
		if (src != dst) {
			mov_rdispr(code, opts->bankptr, src * 4, opts->gen.scratch1, SZ_D);
			mov_rrdisp(code, opts->gen.scratch1, opts->bankptr, dst * 4, SZ_D);
		}
		mov_ir(code, dst, opts->writeback, SZ_B);
		mov_ir(code, JAGCPU_NOREG, opts->resultreg, SZ_B);
		break;
		}
	case JAG_MOVEQ: {
		uint8_t dst = jag_reg2(inst);
		jag_check_resultwrite_noread(opts);
		//moveq has a shorter pipeline than normal instructions
		mov_irdisp(code, jag_quick(inst), opts->bankptr, dst * 4, SZ_D);
		mov_ir(code, dst, opts->writeback, SZ_B);
		mov_ir(code, JAGCPU_NOREG, opts->resultreg, SZ_B);
		}
		break;
	case JAG_JR: {
		jag_check_resultwrite_noread(opts);
		//TODO: Pipeline stalls on flag readiness
		uint16_t cond = jag_reg2(inst);
		if (jag_is_always_false(cond)) {
		} else {
			int32_t offset = jag_quick(inst);
			if (offset & 0x10) {
				offset = -16 + (offset & 0xF);
			}
		}
		
		}
		break;
	default:
		if (is_quick_1_32_opcode(opcode, opts->is_gpu)) {
			translate_jag_quickimmed(opts, address, jag_quick(inst), jag_reg2(inst));
		} else if (is_quick_0_31_opcode(opcode)) {
			translate_jag_quickimmed(opts, address, jag_reg1(inst), jag_reg2(inst));
		} else if (is_single_source(opcode, opts->is_gpu)) {
			translate_jag_single_source(opts, address, jag_reg2(isnt));
		} else {
			translate_jag_normal(opts, address, jag_reg1(inst), jag_reg2(inst));
		}
	}
	return stream;
}