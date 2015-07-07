#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "romdb.h"
#include "util.h"
#include "blastem.h"

#define TITLE_START 0x150
#define TITLE_END (TITLE_START+48)
#define GAME_ID_OFF 0x183
#define GAME_ID_LEN 8
#define ROM_END   0x1A4
#define RAM_ID    0x1B0
#define RAM_FLAGS 0x1B2
#define RAM_START 0x1B4
#define RAM_END   0x1B8
#define REGION_START 0x1F0

uint16_t read_sram_w(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address] << 8 | gen->save_storage[address+1];
	case RAM_FLAG_EVEN:
		return gen->save_storage[address >> 1] << 8 | 0xFF;
	case RAM_FLAG_ODD:
		return gen->save_storage[address >> 1] | 0xFF00;
	}
	return 0xFFFF;//We should never get here
}

uint8_t read_sram_b(uint32_t address, m68k_context * context)
{
	genesis_context * gen = context->system;
	address &= gen->save_ram_mask;
	switch(gen->save_type)
	{
	case RAM_FLAG_BOTH:
		return gen->save_storage[address];
	case RAM_FLAG_EVEN:
		if (address & 1) {
			return 0xFF;
		} else {
			return gen->save_storage[address >> 1];
		}
	case RAM_FLAG_ODD:
		if (address & 1) {
			return gen->save_storage[address >> 1];
		} else {
			return 0xFF;
		}
	}
	return 0xFF;//We should never get here
}

m68k_context * write_sram_area_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value >> 8;
			gen->save_storage[address+1] = value;
			break;
		case RAM_FLAG_EVEN:
			gen->save_storage[address >> 1] = value >> 8;
			break;
		case RAM_FLAG_ODD:
			gen->save_storage[address >> 1] = value;
			break;
		}
	}
	return context;
}

m68k_context * write_sram_area_b(uint32_t address, m68k_context * context, uint8_t value)
{
	genesis_context * gen = context->system;
	if ((gen->bank_regs[0] & 0x3) == 1) {
		address &= gen->save_ram_mask;
		switch(gen->save_type)
		{
		case RAM_FLAG_BOTH:
			gen->save_storage[address] = value;
			break;
		case RAM_FLAG_EVEN:
			if (!(address & 1)) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		case RAM_FLAG_ODD:
			if (address & 1) {
				gen->save_storage[address >> 1] = value;
			}
			break;
		}
	}
	return context;
}

m68k_context * write_bank_reg_w(uint32_t address, m68k_context * context, uint16_t value)
{
	genesis_context * gen = context->system;
	address &= 0xE;
	address >>= 1;
	gen->bank_regs[address] = value;
	if (!address) {
		if (value & 1) {
			context->mem_pointers[2] = NULL;
		} else {
			context->mem_pointers[2] = cart + 0x200000/2;
		}
	}
	return context;
}

m68k_context * write_bank_reg_b(uint32_t address, m68k_context * context, uint8_t value)
{
	if (address & 1) {
		genesis_context * gen = context->system;
		address &= 0xE;
		address >>= 1;
		gen->bank_regs[address] = value;
		if (!address) {
			if (value & 1) {
				context->mem_pointers[2] = NULL;
			} else {
				context->mem_pointers[2] = cart + 0x200000/2;
			}
		}
	}
	return context;
}

tern_node *load_rom_db()
{
	char *exe_dir = get_exe_dir();
	if (!exe_dir) {
		fputs("Failed to find executable path\n", stderr);
		exit(1);
	}
	char *path = alloc_concat(exe_dir, "/rom.db");
	tern_node *db = parse_config_file(path);
	free(path);
	if (!db) {
		fputs("Failed to load ROM DB\n", stderr);
	}
	return db;
}

char *get_header_name(uint8_t *rom)
{
	uint8_t *last = rom + TITLE_END - 1;
	uint8_t *src = rom + TITLE_START;
	
	while (last > src && (*last <=  0x20 || *last >= 0x80))
	{
		last--;
	}
	if (last == src) {
		//TODO: Use other name field
		return strdup("UNKNOWN");
	} else {
		last++;
		char *ret = malloc(last - (rom + TITLE_START) + 1);
		uint8_t *dst;
		for (dst = ret; src < last; src++)
		{
			if (*src >= 0x20 && *src < 0x80) {
				*(dst++) = *src;
			}
		}
		*dst = 0;
		return ret;
	}
}

char *region_chars = "UB4JEA";
uint8_t region_bits[] = {REGION_U, REGION_U, REGION_U, REGION_J, REGION_E, REGION_E};

uint8_t translate_region_char(uint8_t c)
{
	for (int i = 0; i < sizeof(region_bits); i++)
	{
		if (c == region_chars[i]) {
			return region_bits[i];
		}
	}
	return 0;
}

uint8_t get_header_regions(uint8_t *rom)
{
	uint8_t regions = 0;
	for (int i = 0; i < 3; i++)
	{
		regions |= translate_region_char(rom[REGION_START + i]);
	}
	return regions;
}

uint32_t get_u32be(uint8_t *data)
{
	return *data << 24 | data[1] << 16 | data[2] << 8 | data[3];
}

void add_memmap_header(rom_info *info, uint8_t *rom, uint32_t size, memmap_chunk const *base_map, int base_chunks)
{
	if (rom[RAM_ID] == 'R' && rom[RAM_ID+1] == 'A') {
		uint32_t rom_end = get_u32be(rom + ROM_END) + 1;
		uint32_t ram_start = get_u32be(rom + RAM_START);
		uint32_t ram_end = get_u32be(rom + RAM_END);
		uint32_t ram_flags = info->save_type = rom[RAM_FLAGS] & RAM_FLAG_MASK;
		ram_start &= 0xFFFFFE;
		ram_end |= 1;
		info->save_mask = ram_end - ram_start;
		uint32_t size = info->save_mask + 1;
		if (ram_flags != RAM_FLAG_BOTH) {
			size /= 2;
		}
		info->save_size = size;
		info->save_buffer = malloc(size);
		
		info->map_chunks = base_chunks + (ram_start >= rom_end ? 2 : 3);
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk)*2);
		memcpy(info->map+2, base_map, sizeof(memmap_chunk) * base_chunks);
		
		if (ram_start >= rom_end) {
			info->map[0].end = rom_end;
			//TODO: ROM mirroring
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;
			
			info->map[1].start = ram_start;
			info->map[1].mask = info->save_mask;
			info->map[1].end = ram_end + 1;
			info->map[1].flags = MMAP_READ | MMAP_WRITE;
			
			if (ram_flags == RAM_FLAG_ODD) {
				info->map[1].flags |= MMAP_ONLY_ODD;
			} else if (ram_flags == RAM_FLAG_EVEN) {
				info->map[1].flags |= MMAP_ONLY_EVEN;
			}
			info->map[1].buffer = info->save_buffer;
		} else {
			//Assume the standard Sega mapper
			info->map[0].end = 0x200000;
			info->map[0].mask = 0xFFFFFF;
			info->map[0].flags = MMAP_READ;
			info->map[0].buffer = rom;
			
			info->map[1].start = 0x200000;
			info->map[1].end = 0x400000;
			info->map[1].mask = 0x1FFFFF;
			info->map[1].flags = MMAP_READ | MMAP_PTR_IDX | MMAP_FUNC_NULL;
			info->map[1].ptr_index = 2;
			info->map[1].read_16 = (read_16_fun)read_sram_w;//these will only be called when mem_pointers[2] == NULL
			info->map[1].read_8 = (read_8_fun)read_sram_b;
			info->map[1].write_16 = (write_16_fun)write_sram_area_w;//these will be called all writes to the area
			info->map[1].write_8 = (write_8_fun)write_sram_area_b;
			info->map[1].buffer = cart + 0x200000;
			
			memmap_chunk *last = info->map + info->map_chunks - 1;
			memset(last, 0, sizeof(memmap_chunk));
			last->start = 0xA13000;
			last->end = 0xA13100;
			last->mask = 0xFF;
			last->write_16 = (write_16_fun)write_bank_reg_w;
			last->write_8 = (write_8_fun)write_bank_reg_b;
		}
	} else {
		info->map_chunks = base_chunks + 1;
		info->map = malloc(sizeof(memmap_chunk) * info->map_chunks);
		memset(info->map, 0, sizeof(memmap_chunk));
		memcpy(info->map+1, base_map, sizeof(memmap_chunk) * base_chunks);
		
		info->map[0].end = 0x400000;
		info->map[0].mask = 0xFFFFFF;
		info->map[0].flags = MMAP_READ;
		info->map[0].buffer = rom;
		info->save_type = SAVE_NONE;
	}
}

rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	info.name = get_header_name(rom);
	info.regions = get_header_regions(rom);
	add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	return info;
}

rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	uint8_t product_id[GAME_ID_LEN+1];
	uint8_t *rom = vrom;
	product_id[GAME_ID_LEN] = 0;
	for (int i = 0; i < GAME_ID_LEN; i++)
	{
		if (rom[GAME_ID_OFF + i] <= ' ') {
			product_id[i] = 0;
			break;
		}
		product_id[i] = rom[GAME_ID_OFF + i];
		
	}
	tern_node * entry = tern_find_prefix(rom_db, product_id);
	if (!entry) {
		return configure_rom_heuristics(rom, rom_size, base_map, base_chunks);
	}
	rom_info info;
	info.name = tern_find_ptr(entry, "name");
	if (info.name) {
		info.name = strdup(info.name);
	} else {
		info.name = get_header_name(rom);
	}
	
	char *dbreg = tern_find_ptr(entry, "regions");
	info.regions = 0;
	if (dbreg) {
		while (*dbreg != 0)
		{
			info.regions |= translate_region_char(*(dbreg++));
		}
	}
	if (!info.regions) {
		info.regions = get_header_regions(rom);
	}
	
	tern_node *map = tern_find_prefix(entry, "map");
	if (map) {
		uint32_t map_count = tern_count(map);
		if (map_count) {
			
		} else {
			add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
		}
	} else {
		add_memmap_header(&info, rom, rom_size, base_map, base_chunks);
	}
	
	return info;
}
