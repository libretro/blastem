#ifndef ROMDB_H_
#define ROMDB_H_

#define REGION_J 1
#define REGION_U 2
#define REGION_E 4

#include "tern.h"
#include "backend.h"

typedef struct {
	char         *name;
	memmap_chunk *map;
	uint8_t      regions;
} rom_info;

tern_node *load_rom_db();
rom_info configure_rom(tern_node *rom_db, void *vrom);

#endif //ROMDB_H_
