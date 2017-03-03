#ifndef REALTEC_H_
#define REALTEC_H_

uint8_t realtec_detect(uint8_t *rom, uint32_t rom_size);
rom_info realtec_configure_rom(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks);

#endif //REALTEC_H_
