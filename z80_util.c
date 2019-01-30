
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
}

void z80_io_write8(z80_context *context)
{
}