#ifndef SEGA_MAPPER_H_
#define SEGA_MAPPER_H_

uint16_t read_sram_w(uint32_t address, m68k_context * context);
uint8_t read_sram_b(uint32_t address, m68k_context * context);
m68k_context * write_sram_area_w(uint32_t address, m68k_context * context, uint16_t value);
m68k_context * write_sram_area_b(uint32_t address, m68k_context * context, uint8_t value);
m68k_context * write_bank_reg_w(uint32_t address, m68k_context * context, uint16_t value);
m68k_context * write_bank_reg_b(uint32_t address, m68k_context * context, uint8_t value);

#endif //SEGA_MAPPER_H_
