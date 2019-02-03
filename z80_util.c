
void z80_read_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	context->scratch1 = read_byte(context->scratch1, NULL, &context->opts->gen, context);
}

void z80_write_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	write_byte(context->scratch2, context->scratch1, NULL, &context->opts->gen, context);
}

void z80_io_read8(z80_context *context)
{
	uint32_t tmp_mask = context->opts->gen.address_mask;
	memmap_chunk const *tmp_map = context->opts->gen.memmap;
	uint32_t tmp_chunks = context->opts->gen.memmap_chunks;
	
	context->opts->gen.address_mask = context->io_mask;
	context->opts->gen.memmap = context->io_map;
	context->opts->gen.memmap_chunks = context->io_chunks;
	
	context->scratch1 = read_byte(context->scratch1, NULL, &context->opts->gen, context);
	
	context->opts->gen.address_mask = tmp_mask;
	context->opts->gen.memmap = tmp_map;
	context->opts->gen.memmap_chunks = tmp_chunks;
}

void z80_io_write8(z80_context *context)
{
	uint32_t tmp_mask = context->opts->gen.address_mask;
	memmap_chunk const *tmp_map = context->opts->gen.memmap;
	uint32_t tmp_chunks = context->opts->gen.memmap_chunks;
	
	context->opts->gen.address_mask = context->io_mask;
	context->opts->gen.memmap = context->io_map;
	context->opts->gen.memmap_chunks = context->io_chunks;
	
	write_byte(context->scratch2, context->scratch1, NULL, &context->opts->gen, context);
	
	context->opts->gen.address_mask = tmp_mask;
	context->opts->gen.memmap = tmp_map;
	context->opts->gen.memmap_chunks = tmp_chunks;
}