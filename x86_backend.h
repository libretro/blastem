#ifndef X86_BACKEND_H_
#define X86_BACKEND_H_

typedef struct {
	int32_t disp;
	uint8_t mode;
	uint8_t base;
	uint8_t index;
	uint8_t cycles;
} x86_ea;

typedef struct {
	uint8_t  *base;
	int32_t  *offsets;
} native_map_slot;

#endif //X86_BACKEND_H_

