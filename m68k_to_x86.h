#include <stdint.h>

typedef struct {
	uint32_t flags;
	int8_t   dregs[8];
	int8_t   aregs[8];
} x86_68k_options;

typedef struct {
	uint8_t  flags[5];
	uint8_t  status;
	uint16_t reserved;
	uint32_t dregs[8];
	uint32_t aregs[8];
} m68k_context;

