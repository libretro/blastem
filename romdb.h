#ifndef ROMDB_H_
#define ROMDB_H_

#define REGION_J 1
#define REGION_U 2
#define REGION_E 4

#define RAM_FLAG_ODD  0x18
#define RAM_FLAG_EVEN 0x10
#define RAM_FLAG_BOTH 0x00
#define RAM_FLAG_MASK RAM_FLAG_ODD
#define SAVE_I2C      0x01
#define SAVE_NONE     0xFF

#include "tern.h"
#include "backend.h"

typedef struct {
	char          *name;
	memmap_chunk  *map;
	uint8_t       *save_buffer;
	uint32_t      map_chunks;
	uint32_t      save_size;
	uint32_t      save_mask;
	uint8_t       save_type;
	uint8_t       regions;
} rom_info;

tern_node *load_rom_db();
rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks);
rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks);
uint8_t translate_region_char(uint8_t c);

#endif //ROMDB_H_
