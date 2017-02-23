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

typedef struct {
	uint32_t     start;
	uint32_t     end;
	uint16_t     sda_write_mask;
	uint16_t     scl_mask;
	uint8_t      sda_read_bit;
} eeprom_map;

typedef struct {
	char        *buffer;
	uint32_t    size;
	uint16_t    address;
	uint8_t     host_sda;
	uint8_t     slave_sda;
	uint8_t     scl;
	uint8_t     state;
	uint8_t     counter;
	uint8_t     latch;
} eeprom_state;


typedef struct rom_info rom_info;

#include "backend.h"

struct rom_info {
	char          *name;
	memmap_chunk  *map;
	uint8_t       *save_buffer;
	eeprom_map    *eeprom_map;
	char          *port1_override;
	char          *port2_override;
	char          *ext_override;
	char          *mouse_mode;
	uint32_t      num_eeprom;
	uint32_t      map_chunks;
	uint32_t      save_size;
	uint32_t      save_mask;
	uint16_t      mapper_start_index;
	uint8_t       save_type;
	uint8_t       regions;
};

#define GAME_ID_OFF 0x183
#define GAME_ID_LEN 8

tern_node *load_rom_db();
rom_info configure_rom(tern_node *rom_db, void *vrom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, memmap_chunk const *base_map, uint32_t base_chunks);
rom_info configure_rom_heuristics(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks);
uint8_t translate_region_char(uint8_t c);
void eeprom_init(eeprom_state *state, uint8_t *buffer, uint32_t size);

#endif //ROMDB_H_
