#include <string.h>

void z80_read_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	uint8_t *fast = context->fastmem[context->scratch1 >> 10];
	if (fast) {
		context->scratch1 = fast[context->scratch1 & 0x3FF];
	} else {
		context->scratch1 = read_byte(context->scratch1, NULL, &context->opts->gen, context);
	}
}

void z80_write_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	uint8_t *fast = context->fastmem[context->scratch2 >> 10];
	if (fast) {
		fast[context->scratch2 & 0x3FF] = context->scratch1;
	} else {
		write_byte(context->scratch2, context->scratch1, NULL, &context->opts->gen, context);
	}
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

//quick hack until I get a chance to change which init method these get passed to
static memmap_chunk const * tmp_io_chunks;
static uint32_t tmp_num_io_chunks, tmp_io_mask;
void init_z80_opts(z80_options * options, memmap_chunk const * chunks, uint32_t num_chunks, memmap_chunk const * io_chunks, uint32_t num_io_chunks, uint32_t clock_divider, uint32_t io_address_mask)
{
	memset(options, 0, sizeof(*options));
	options->gen.memmap = chunks;
	options->gen.memmap_chunks = num_chunks;
	options->gen.address_mask = 0xFFFF;
	options->gen.max_address = 0xFFFF;
	options->gen.clock_divider = clock_divider;
	tmp_io_chunks = io_chunks;
	tmp_num_io_chunks = num_io_chunks;
	tmp_io_mask = io_address_mask;
}

z80_context * init_z80_context(z80_options *options)
{
	z80_context *context = calloc(1, sizeof(z80_context));
	context->opts = options;
	context->io_map = (memmap_chunk *)tmp_io_chunks;
	context->io_chunks = tmp_num_io_chunks;
	context->io_mask = tmp_io_mask;
	for(uint32_t address = 0; address < 0x10000; address+=1024)
	{
		uint8_t *start = get_native_pointer(address, NULL, &options->gen);
		if (start) {
			uint8_t *end = get_native_pointer(address + 1023, NULL, &options->gen);
			if (end && end - start == 1023) {
				context->fastmem[address >> 10] = start;
			}
		}
	}
	return context;
}

