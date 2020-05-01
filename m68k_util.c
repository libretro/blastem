#include <string.h>

void m68k_read_8(m68k_context *context)
{
	context->cycles += 4 * context->opts->gen.clock_divider;
	context->scratch1 = read_byte(context->scratch1, context->mem_pointers, &context->opts->gen, context);
}

void m68k_read_16(m68k_context *context)
{
	context->cycles += 4 * context->opts->gen.clock_divider;
	context->scratch1 = read_word(context->scratch1, context->mem_pointers, &context->opts->gen, context);
}

void m68k_write_8(m68k_context *context)
{
	context->cycles += 4 * context->opts->gen.clock_divider;
	write_byte(context->scratch2, context->scratch1, context->mem_pointers, &context->opts->gen, context);
}

void m68k_write_16(m68k_context *context)
{
	context->cycles += 4 * context->opts->gen.clock_divider;
	write_word(context->scratch2, context->scratch1, context->mem_pointers, &context->opts->gen, context);
}

void m68k_sync_cycle(m68k_context *context, uint32_t target_cycle)
{
	//TODO: interrupt stuff
	context->sync_cycle = target_cycle;
}

void init_m68k_opts(m68k_options *opts, memmap_chunk * memmap, uint32_t num_chunks, uint32_t clock_divider)
{
	memset(opts, 0, sizeof(*opts));
	opts->gen.memmap = memmap;
	opts->gen.memmap_chunks = num_chunks;
	opts->gen.address_mask = 0xFFFFFF;
	opts->gen.byte_swap = 1;
	opts->gen.max_address = 0x1000000;
	opts->gen.bus_cycles = 4;
	opts->gen.clock_divider = clock_divider;
}

m68k_context *init_68k_context(m68k_options * opts, m68k_reset_handler reset_handler)
{
	m68k_context *context = calloc(1, sizeof(m68k_context));
	context->opts = opts;
	context->reset_handler = reset_handler;
	context->int_cycle = 0xFFFFFFFFU;
	return context;
}

void m68k_reset(m68k_context *context)
{
	//read initial SP
	context->scratch1 = 0;
	m68k_read_16(context);
	context->aregs[7] = context->scratch1 << 16;
	context->scratch1 = 2;
	m68k_read_16(context);
	context->aregs[7] |= context->scratch1;
	
	//read initial PC
	context->scratch1 = 4;
	m68k_read_16(context);
	context->pc = context->scratch1 << 16;
	context->scratch1 = 6;
	m68k_read_16(context);
	context->pc |= context->scratch1;
	
	context->scratch1 = context->pc;
	m68k_read_16(context);
	context->prefetch = context->scratch1;
	context->pc += 2;
	
	context->status = 0x27;
}

void m68k_print_regs(m68k_context *context)
{
	printf("XNZVC\n%d%d%d%d%d\n", context->xflag != 0, context->nflag != 0, context->zflag != 0, context->vflag != 0, context->cflag != 0);
	for (int i = 0; i < 8; i++) {
		printf("d%d: %X\n", i, context->dregs[i]);
	}
	for (int i = 0; i < 8; i++) {
		printf("a%d: %X\n", i, context->aregs[i]);
	}
}
