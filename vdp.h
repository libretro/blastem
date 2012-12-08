#ifndef VDP_H_
#define VDP_H_

#include <stdint.h>
#include <stdio.h>

#define VDP_REGS 24
#define CRAM_SIZE 64
#define VSRAM_SIZE 40
#define VRAM_SIZE (64*1024)
#define LINEBUF_SIZE 320
#define FRAMEBUF_ENTRIES 320*224
#define FRAMEBUF_SIZE (FRAMEBUF_ENTRIES*sizeof(uint16_t))
#define MAX_DRAWS 40
#define MAX_SPRITES_LINE 20

enum {
	REG_MODE_1=0,
	REG_MODE_2,
	REG_SCROLL_A,
	REG_WINDOW,
	REG_SCROLL_B,
	REG_SAT,
	REG_BG_COLOR,
	REG_HINT=0xA,
	REG_MODE_3,
	REG_MODE_4,
	REG_HSCROLL,
	REG_AUTOINC=0xF,
	REG_SCROLL,
	REG_WINDOW_H,
	REG_WINDOW_V
} vdp_regs;

typedef struct {
	uint16_t address;
	int16_t x_pos;
	uint8_t pal_priority;
	uint8_t h_flip;
} sprite_draw;

typedef struct {
	uint8_t size;
	uint8_t index;
	int16_t y;
} sprite_info;

typedef struct {
	//cycle count in MCLKs
	uint32_t    cycles;
	uint8_t     *vdpmem;
	//stores 2-bit palette + 4-bit palette index + priority for current sprite line
	uint8_t     *linebuf;
	//stores 12-bit color + shadow/highlight bits
	uint16_t    *framebuf;
	uint16_t    cram[CRAM_SIZE];
	uint16_t    vsram[VSRAM_SIZE];
	uint8_t     latched_mode;
	uint16_t    hscroll_a;
	uint16_t    hscroll_b;
	uint8_t	    sprite_index;
	uint8_t     sprite_draws;
	uint8_t     slot_counter;
	uint8_t     regs[VDP_REGS];
	sprite_draw sprite_draw_list[MAX_DRAWS];
	sprite_info sprite_info_list[MAX_SPRITES_LINE];
	uint16_t    col_1;
	uint16_t    col_2;
	uint8_t     v_offset;
	uint8_t     *tmp_buf_a;
	uint8_t     *tmp_buf_b;
} vdp_context;

void init_vdp_context(vdp_context * context);
void vdp_run_context(vdp_context * context, uint32_t target_cycles);
//runs from current cycle count to VBLANK for the current mode, returns ending cycle count
uint32_t vdp_run_to_vblank(vdp_context * context);
void vdp_load_savestate(vdp_context * context, FILE * state_file);

#endif //VDP_H_
