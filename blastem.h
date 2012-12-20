#ifndef BLASTEM_H_
#define BLASTEM_H_

typedef struct {
	uint32_t th_counter;
	uint32_t timeout_cycle;
	uint8_t output;
	uint8_t control;
	uint8_t input[3];
} io_port;

#define GAMEPAD_TH0 0
#define GAMEPAD_TH1 1
#define GAMEPAD_EXTRA 2

extern io_port gamepad_1;
extern io_port gamepad_2;

void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction);

#endif //BLASTEM_H_

