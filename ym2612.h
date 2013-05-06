#ifndef YM2612_H_
#define YM2612_H_

#include <stdint.h>

#define NUM_SHARED_REGS (0x30-0x21)
#define NUM_PART_REGS (0xB7-0x30)

typedef struct {
	uint32_t current_cycle;
	uint32_t write_cycle;
	uint8_t  *selected_reg;
	uint16_t timer_a;
	uint8_t  timer_b;
	uint8_t  reg_num;
	uint8_t  status;
	uint8_t  part1_regs[NUM_SHARED_REGS+NUM_PART_REGS];
	uint8_t  part2_regs[NUM_PART_REGS];
} ym2612_context;

void ym_init(ym2612_context * context);
void ym_run(ym2612_context * context, uint32_t to_cycle);
void ym_address_write_part1(ym2612_context * context, uint8_t address);
void ym_address_write_part2(ym2612_context * context, uint8_t address);
void ym_data_write(ym2612_context * context, uint8_t value);
uint8_t ym_read_status(ym2612_context * context);

#endif //YM2612_H_

