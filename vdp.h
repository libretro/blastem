/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef VDP_H_
#define VDP_H_

#include <stdint.h>
#include <stdio.h>

#define VDP_REGS 24
#define CRAM_SIZE 64
#define VSRAM_SIZE 40
#define VRAM_SIZE (64*1024)
#define LINEBUF_SIZE 320
#define FRAMEBUF_ENTRIES (320+27)*(240+27) //PAL active display + full border
#define MAX_DRAWS 40
#define MAX_DRAWS_H32 32
#define MAX_SPRITES_LINE 20
#define MAX_SPRITES_LINE_H32 16
#define MAX_SPRITES_FRAME 80
#define MAX_SPRITES_FRAME_H32 64

#define FBUF_SHADOW 0x0001
#define FBUF_HILIGHT 0x0010
#define DBG_SHADOW 0x10
#define DBG_HILIGHT 0x20
#define DBG_PRIORITY 0x8
#define DBG_SRC_MASK 0x7
#define DBG_SRC_A 0x1
#define DBG_SRC_W 0x2
#define DBG_SRC_B 0x3
#define DBG_SRC_S 0x4
#define DBG_SRC_BG 0x0

#define MCLKS_LINE 3420

#define FLAG_DOT_OFLOW     0x01
#define FLAG_CAN_MASK      0x02
#define FLAG_MASKED        0x04
#define FLAG_WINDOW        0x08
#define FLAG_PENDING       0x10
#define FLAG_UNUSED_SLOT   0x20
#define FLAG_DMA_RUN       0x40
#define FLAG_DMA_PROG      0x80

#define FLAG2_VINT_PENDING   0x01
#define FLAG2_HINT_PENDING   0x02
#define FLAG2_READ_PENDING   0x04
#define FLAG2_SPRITE_COLLIDE 0x08

#define DISPLAY_ENABLE 0x40

enum {
	REG_MODE_1=0,
	REG_MODE_2,
	REG_SCROLL_A,
	REG_WINDOW,
	REG_SCROLL_B,
	REG_SAT,
	REG_BG_COLOR=7,
	REG_HINT=0xA,
	REG_MODE_3,
	REG_MODE_4,
	REG_HSCROLL,
	REG_AUTOINC=0xF,
	REG_SCROLL,
	REG_WINDOW_H,
	REG_WINDOW_V,
	REG_DMALEN_L,
	REG_DMALEN_H,
	REG_DMASRC_L,
	REG_DMASRC_M,
	REG_DMASRC_H
} vdp_regs;

//Mode reg 1
#define BIT_HINT_EN    0x10
#define BIT_PAL_SEL    0x04
#define BIT_HVC_LATCH  0x02
#define BIT_DISP_DIS   0x01

//Mode reg 2
#define BIT_DISP_EN    0x40
#define BIT_VINT_EN    0x20
#define BIT_DMA_ENABLE 0x10
#define BIT_PAL        0x08
#define BIT_MODE_5     0x04

//Mode reg 3
#define BIT_EINT_EN    0x10
#define BIT_VSCROLL    0x04

//Mode reg 4
#define BIT_H40        0x01
#define BIT_HILIGHT    0x8
#define BIT_DOUBLE_RES 0x4
#define BIT_INTERLACE  0x2

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

#define FIFO_SIZE 4

typedef struct {
	uint32_t cycle;
	uint16_t address;
	uint16_t value;
	uint8_t  cd;
	uint8_t  partial;
} fifo_entry;

typedef struct {
	fifo_entry  fifo[FIFO_SIZE];
	int32_t     fifo_write;
	int32_t     fifo_read;
	uint16_t    address;
	uint8_t     cd;
	uint8_t	    flags;
	uint8_t     regs[VDP_REGS];
	//cycle count in MCLKs
	uint32_t    cycles;
	uint8_t     *vdpmem;
	//stores 2-bit palette + 4-bit palette index + priority for current sprite line
	uint8_t     *linebuf;
	//stores 12-bit color + shadow/highlight bits
	void        *framebuf;
	void        *oddbuf;
	void        *evenbuf;
	uint16_t    cram[CRAM_SIZE];
	uint32_t    colors[CRAM_SIZE*3];
	uint32_t    debugcolors[1 << (3 + 1 + 1 + 1)];//3 bits for source, 1 bit for priority, 1 bit for shadow, 1 bit for hilight
	uint16_t    vsram[VSRAM_SIZE];
	uint16_t    vcounter;
	uint16_t    hslot; //hcounter/2
	uint16_t    hscroll_a;
	uint16_t    hscroll_b;
	uint8_t     latched_mode;
	uint8_t	    sprite_index;
	uint8_t     sprite_draws;
	int8_t      slot_counter;
	int8_t      cur_slot;
	sprite_draw sprite_draw_list[MAX_DRAWS];
	sprite_info sprite_info_list[MAX_SPRITES_LINE];
	uint16_t    col_1;
	uint16_t    col_2;
	uint16_t    hv_latch;
	uint8_t     v_offset;
	uint8_t     dma_cd;
	uint8_t     hint_counter;
	uint8_t     flags2;
	uint8_t     double_res;
	uint8_t     b32;
	uint8_t     buf_a_off;
	uint8_t     buf_b_off;
	uint8_t     debug;
	uint8_t     *tmp_buf_a;
	uint8_t     *tmp_buf_b;
} vdp_context;

void init_vdp_context(vdp_context * context);
void vdp_run_context(vdp_context * context, uint32_t target_cycles);
//runs from current cycle count to VBLANK for the current mode, returns ending cycle count
uint32_t vdp_run_to_vblank(vdp_context * context);
//runs until the target cycle is reached or the current DMA operation has completed, whicever comes first
void vdp_run_dma_done(vdp_context * context, uint32_t target_cycles);
uint8_t vdp_load_gst(vdp_context * context, FILE * state_file);
uint8_t vdp_save_gst(vdp_context * context, FILE * outfile);
int vdp_control_port_write(vdp_context * context, uint16_t value);
int vdp_data_port_write(vdp_context * context, uint16_t value);
void vdp_test_port_write(vdp_context * context, uint16_t value);
uint16_t vdp_control_port_read(vdp_context * context);
uint16_t vdp_data_port_read(vdp_context * context);
uint16_t vdp_hv_counter_read(vdp_context * context);
uint16_t vdp_test_port_read(vdp_context * context);
void vdp_adjust_cycles(vdp_context * context, uint32_t deduction);
uint32_t vdp_next_hint(vdp_context * context);
uint32_t vdp_next_vint(vdp_context * context);
uint32_t vdp_next_vint_z80(vdp_context * context);
void vdp_int_ack(vdp_context * context, uint16_t int_num);
void vdp_print_sprite_table(vdp_context * context);
void vdp_print_reg_explain(vdp_context * context);
void latch_mode(vdp_context * context);

extern int32_t color_map[1 << 12];

#endif //VDP_H_
