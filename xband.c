#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "romdb.h"
#include "genesis.h"
#include "tern.h"
#include "xband.h"
#include "util.h"

#define BIT_ROM_HI 4

uint8_t xband_detect(uint8_t *rom, uint32_t rom_size)
{
	//Internal ROM is 512KB, accept larger ones for overdumps and custom firmware
	if (rom_size < (512*1024)) {
		return 0;
	}
	
	//ROM has no standard header, but does have a jump at $100
	if (rom[0x100] != 0x4E || rom[0x101] != 0xF9) {
		return 0;
	}
	
	//product ID is all NULL
	for (int i = GAME_ID_OFF; i <= (GAME_ID_OFF + GAME_ID_LEN); i++)
	{
		if (rom[i]) {
			return 0;
		}
	}
	return 1;
}

static xband *get_xband(genesis_context *gen)
{
	if (!gen->extra) {
		gen->extra = gen->m68k->options->gen.memmap[0].buffer;
		gen->m68k->mem_pointers[2] = (uint16_t *)gen->save_storage;
	}
	return gen->extra;
}

static void update_control(genesis_context *gen, uint8_t value)
{
	xband *x = gen->extra;
	if ((x->control ^ value) & BIT_ROM_HI) {
		if (value & BIT_ROM_HI) {
			gen->m68k->mem_pointers[0] = (uint16_t *)gen->save_storage;
			gen->m68k->mem_pointers[1] = NULL;
			gen->m68k->mem_pointers[2] = x->cart_space;
			gen->m68k->mem_pointers[3] = x->cart_space - 0x100000;
		} else {
			gen->m68k->mem_pointers[0] = x->cart_space;
			gen->m68k->mem_pointers[1] = x->cart_space;
			gen->m68k->mem_pointers[2] = (uint16_t *)gen->save_storage;
			gen->m68k->mem_pointers[3] = NULL;
		}
		m68k_invalidate_code_range(gen->m68k, 0, 0x3BC000);
	}
	x->control = value;
}

static void *xband_write_b(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	xband *x = get_xband(gen);
	if (address == 0x181) {
		x->kill = value;
		printf("Write to \"soft\" kill register %X\n", value);
	} else if (address == 0x183) {
		update_control(gen, value);
		printf("Write to \"soft\" control register %X\n", value);
	} else if ((x->control & BIT_ROM_HI && address < 0x200000) || (address >= 0x200000 && !(x->control & BIT_ROM_HI))) {
		gen->save_storage[(address & 0xFFFF) ^ 1] = value;
		m68k_handle_code_write(address, m68k);
		//TODO: handle code at mirror addresses
	} else {
		printf("Unhandled write to cartridge area %X: %X\n", address, value);
	}
	return context;
}

static void *xband_write_hi_b(uint32_t address, void *context, uint8_t value)
{
	return xband_write_b(address | 0x200000, context, value);
}

static void *xband_write_w(uint32_t address, void *context, uint16_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	xband *x = get_xband(gen);
	if (address == 0x180 || address == 0x182) {
		return xband_write_b(address | 1, context, value);
	} else if ((x->control & BIT_ROM_HI && address < 0x200000) || (address >= 0x200000 && !(x->control & BIT_ROM_HI))) {
		gen->save_storage[address & 0xFFFE] = value;
		gen->save_storage[(address & 0xFFFE) | 1] = value >> 8;
		m68k_handle_code_write(address, m68k);
		//TODO: handle code at mirror addresses
		return context;
	}
	printf("Unhandled write to %X: %X\n", address, value);
	return context;
}

static void *xband_write_hi_w(uint32_t address, void *context, uint16_t value)
{
	return xband_write_w(address | 0x200000, context, value);
}

static uint16_t xband_read_w(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	xband *x = get_xband(gen);
	//TODO: actually do something intelligent here
	return x->cart_space[address >> 1];
}

static uint16_t xband_read_hi_w(uint32_t address, void *context)
{
	return xband_read_w(address | 0x200000, context);
}

static uint8_t xband_read_b(uint32_t address, void *context)
{
	uint16_t val = xband_read_w(address, context);
	return address & 1 ? val : val >> 8;
}

static uint8_t xband_read_hi_b(uint32_t address, void *context)
{
	return xband_read_b(address | 0x200000, context);
}

static void *xband_reg_write_b(uint32_t address, void *context, uint8_t value)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (!(address & 1)) {
		printf("Ignoring write to even address %X: %X\n", address, value);
		return context;
	}
	xband *x = get_xband(gen);
	if (address < 0x3BFE00) {
		uint32_t offset = (address - 0x3BC001) / 2;
		if (offset < XBAND_REGS) {
			x->regs[offset] = value;
			printf("Write to register %X: %X\n", address, value);
		} else {
			printf("Unhandled register write %X: %X\n", address, value);
		}
	} else {
		if (address == 0x3BFE01) {
			x->kill = value;
			printf("Write to kill register %X\n", value);
		} else if (address == 0x3BFE03) {
			update_control(gen, value);
			printf("Write to control register %X\n", value);
		} else {
			printf("Unhandled register write %X: %X\n", address, value);
		}
	}
	return context;
}

static void *xband_reg_write_w(uint32_t address, void *context, uint16_t value)
{
	return xband_reg_write_b(address | 1, context, value);
}

static uint8_t xband_reg_read_b(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (!(address & 1)) {
		printf("Read from even address %X\n", address);
		return gen->header.get_open_bus_value(&gen->header) >> 8;
	}
	xband *x = get_xband(gen);
	if (address < 0x3BFE00) {
		uint32_t offset = (address - 0x3BC001) / 2;
		if (offset < XBAND_REGS) {
			return x->regs[offset];
		} else {
			printf("Unhandled register read from address %X\n", address);
			return 0x5D;
		}
	} else {
		if (address == 0x3BFE01) {
			return x->kill;
		} else if (address == 0x3BFE03) {
			return x->control;
		} else {
			printf("Unhandled register read from address %X\n", address);
			return 0x5D;
		}
	}
}

static uint16_t xband_reg_read_w(uint32_t address, void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	uint16_t value = xband_reg_read_b(address | 1, context);
	value |= gen->header.get_open_bus_value(&gen->header) & 0xFF00;
	return value;
}

rom_info xband_configure_rom(tern_node *rom_db, void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	if (lock_on && lock_on_size) {
		rom_info lock_on_info = configure_rom(rom_db, lock_on, lock_on_size, NULL, 0, base_map, base_chunks);
		info.name = alloc_concat("XBAND - ", lock_on_info.name);
		info.regions = lock_on_info.regions;
		//TODO: Move this to a shared function in romdbc.h
		free(lock_on_info.name);
		if (lock_on_info.save_type != SAVE_NONE) {
			free(lock_on_info.save_buffer);
			if (lock_on_info.save_type == SAVE_I2C) {
				free(lock_on_info.eeprom_map);
			}
		}
		free(lock_on_info.map);
		free(lock_on_info.port1_override);
		free(lock_on_info.port2_override);
		free(lock_on_info.ext_override);
		free(lock_on_info.mouse_mode);
	} else {
		info.name = strdup("XBAND");
		info.regions = REGION_J|REGION_U|REGION_E;
	}
	info.save_size = 64*1024;
	info.save_buffer = malloc(info.save_size);
	info.save_mask = info.save_size-1;
	info.save_type = RAM_FLAG_BOTH;
	info.port1_override = info.port2_override = info.ext_override = info.mouse_mode = NULL;
	info.eeprom_map = NULL;
	info.num_eeprom = 0;
	xband *x = calloc(sizeof(xband), 1);
	rom_size = nearest_pow2(rom_size);
	for (int i = 0; (i + rom_size) <= sizeof(x->cart_space) / 2; i += rom_size)
	{
		memcpy(x->cart_space + i/2, rom, rom_size);
	}
	if (lock_on && lock_on_size >= 0x200) {
		memcpy(x->cart_space + 0x80, ((uint16_t *)lock_on) + 0x80, 0x100);
	}
	byteswap_rom(0x400000, x->cart_space);
	
	info.map_chunks = base_chunks + 5;
	info.map = calloc(sizeof(memmap_chunk), info.map_chunks);
	info.map[0].mask = 0xFFFFFF;
	info.map[0].aux_mask = 0xFFFFFF;
	info.map[0].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_AUX_BUFF;
	info.map[0].start = 0;
	info.map[0].end = 0x10000;
	info.map[0].ptr_index = 0;
	info.map[0].buffer = x->cart_space;
	info.map[0].write_16 = xband_write_w;
	info.map[0].write_8 = xband_write_b;
	info.map[0].read_16 = xband_read_w;
	info.map[0].read_8 = xband_read_b;
	info.map[1].mask = 0xFFFFFF;
	info.map[1].aux_mask = 0xFFFFFF;
	info.map[1].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_AUX_BUFF;
	info.map[1].start = 0x10000;
	info.map[1].end = 0x200000;
	info.map[1].ptr_index = 1;
	info.map[1].buffer = x->cart_space;
	info.map[1].write_16 = xband_write_w;
	info.map[1].write_8 = xband_write_b;
	info.map[1].read_16 = xband_read_w;
	info.map[1].read_8 = xband_read_b;
	info.map[2].mask = 0xFFFF;
	info.map[2].aux_mask = 0xFFFF;
	info.map[2].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX|MMAP_FUNC_NULL;
	info.map[2].start = 0x200000;
	info.map[2].end = 0x210000;
	info.map[2].ptr_index = 2;
	info.map[2].buffer = NULL;
	info.map[2].write_16 = xband_write_hi_w;
	info.map[2].write_8 = xband_write_hi_b;
	info.map[2].read_16 = xband_read_hi_w;
	info.map[2].read_8 = xband_read_hi_b;
	info.map[3].mask = 0xFFFFFF;
	info.map[3].aux_mask = 0xFFFFFF;
	info.map[3].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX|MMAP_FUNC_NULL;
	info.map[3].start = 0x210000;
	info.map[3].end = 0x3BC000;
	info.map[3].ptr_index = 3;
	info.map[3].buffer = NULL;
	info.map[3].write_16 = xband_write_w;
	info.map[3].write_8 = xband_write_b;
	info.map[3].read_16 = xband_read_w;
	info.map[3].read_8 = xband_read_b;
	info.map[4].mask = 0xFFFFFF;
	info.map[4].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX|MMAP_FUNC_NULL;
	info.map[4].start = 0x3BC000;
	info.map[4].end = 0x3C0000;
	info.map[4].ptr_index = 4;
	info.map[4].write_16 = xband_reg_write_w;
	info.map[4].write_8 = xband_reg_write_b;
	info.map[4].read_16 = xband_reg_read_w;
	info.map[4].read_8 = xband_reg_read_b;
	memcpy(info.map + 5, base_map, base_chunks * sizeof(memmap_chunk));
	
	return info;
}
