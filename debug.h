#ifndef DEBUG_H_
#define DEBUG_H_

#include <stdint.h>
#include "m68k_core.h"
#include "z80_to_x86.h"

typedef struct disp_def {
	struct disp_def * next;
	char *            param;
	uint32_t          index;
	char              format_char;
} disp_def;

typedef struct bp_def {
	struct bp_def *next;
	char          *commands;
	uint32_t      address;
	uint32_t      index;
} bp_def;

bp_def ** find_breakpoint(bp_def ** cur, uint32_t address);
bp_def ** find_breakpoint_idx(bp_def ** cur, uint32_t index);
void add_display(disp_def ** head, uint32_t *index, char format_char, char * param);
void remove_display(disp_def ** head, uint32_t index);
void debugger(m68k_context * context, uint32_t address);
z80_context * zdebugger(z80_context * context, uint16_t address);

#endif //DEBUG_H_
