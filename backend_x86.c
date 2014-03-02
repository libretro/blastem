#include "backend.h"
#include "gen_x86.h"

void cycles(cpu_options *opts, uint32_t num)
{
	add_ir(&opts->code, num, opts->cycles, SZ_D);
}

void check_cycles_int(cpu_options *opts, uint32_t address)
{
	code_info *code = &opts->code;
	cmp_rr(code, opts->cycles, opts->limit, SZ_D);
	code_ptr jmp_off = code->cur+1;
	jcc(code, CC_NC, jmp_off+1);
	mov_ir(code, address, opts->scratch1, SZ_D);
	call(code, opts->handle_cycle_limit_int);
	*jmp_off = code->cur - (jmp_off+1);
}

void check_cycles(cpu_options * opts)
{
	code_info *code = &opts->code;
	cmp_rr(code, opts->cycles, opts->limit, SZ_D);
	check_alloc_code(code, MAX_INST_LEN*2);
	code_ptr jmp_off = code->cur+1;
	jcc(code, CC_NC, jmp_off+1);
	call(code, opts->handle_cycle_limit);
	*jmp_off = code->cur - (jmp_off+1);
}
