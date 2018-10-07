
void svp_prog_read_16(svp_context *context)
{
	uint16_t address = context->scratch1 >> 1;
	context->scratch1 = context->rom[address];
}
