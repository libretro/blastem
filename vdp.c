/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "vdp.h"
#include "blastem.h"
#include <stdlib.h>
#include <string.h>
#include "render.h"

#define NTSC_ACTIVE 225
#define PAL_ACTIVE 241
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define SCROLL_BUFFER_SIZE 32
#define SCROLL_BUFFER_MASK (SCROLL_BUFFER_SIZE-1)
#define SCROLL_BUFFER_DRAW (SCROLL_BUFFER_SIZE/2)

#define MCLKS_SLOT_H40  16
#define MCLKS_SLOT_H32  20
#define VINT_CYCLE_H40  (21*MCLKS_SLOT_H40+332+9*MCLKS_SLOT_H40) //21 slots before HSYNC, 16 during, 10 after
#define VINT_CYCLE_H32  ((33+20+7)*MCLKS_SLOT_H32)  //33 slots before HSYNC, 20 during, 7 after  TODO: confirm final number
#define HSYNC_SLOT_H40  21
#define MCLK_WEIRD_END  (HSYNC_SLOT_H40*MCLKS_SLOT_H40 + 332)
#define SLOT_WEIRD_END  (HSYNC_SLOT_H40+17)
#define HSYNC_END_H32   (33 * MCLKS_SLOT_H32)
#define HBLANK_CLEAR_H40 (MCLK_WEIRD_END+61*4)
#define HBLANK_CLEAR_H32 (HSYNC_END_H32 + 46*5)
#define FIFO_LATENCY    3

int32_t color_map[1 << 12];
uint8_t levels[] = {0, 27, 49, 71, 87, 103, 119, 130, 146, 157, 174, 190, 206, 228, 255};

uint8_t debug_base[][3] = {
	{127, 127, 127}, //BG
	{0, 0, 127},     //A
	{127, 0, 0},     //Window
	{0, 127, 0},     //B
	{127, 0, 127}    //Sprites
};

uint8_t color_map_init_done;

void init_vdp_context(vdp_context * context)
{
	memset(context, 0, sizeof(*context));
	context->vdpmem = malloc(VRAM_SIZE);
	memset(context->vdpmem, 0, VRAM_SIZE);
	context->oddbuf = context->framebuf = malloc(FRAMEBUF_ENTRIES * (render_depth() / 8));
	memset(context->framebuf, 0, FRAMEBUF_ENTRIES * (render_depth() / 8));
	context->evenbuf = malloc(FRAMEBUF_ENTRIES * (render_depth() / 8));
	memset(context->evenbuf, 0, FRAMEBUF_ENTRIES * (render_depth() / 8));
	context->linebuf = malloc(LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	memset(context->linebuf, 0, LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	context->tmp_buf_a = context->linebuf + LINEBUF_SIZE;
	context->tmp_buf_b = context->tmp_buf_a + SCROLL_BUFFER_SIZE;
	context->sprite_draws = MAX_DRAWS;
	context->fifo_write = 0;
	context->fifo_read = -1;
	context->b32 = render_depth() == 32;
	if (!color_map_init_done) {
		uint8_t b,g,r;
		for (uint16_t color = 0; color < (1 << 12); color++) {
			if (color & FBUF_SHADOW) {
				b = levels[(color >> 9) & 0x7];
				g = levels[(color >> 5) & 0x7];
				r = levels[(color >> 1) & 0x7];
			} else if(color & FBUF_HILIGHT) {
				b = levels[((color >> 9) & 0x7) + 7];
				g = levels[((color >> 5) & 0x7) + 7];
				r = levels[((color >> 1) & 0x7) + 7];
			} else {
				b = levels[(color >> 8) & 0xE];
				g = levels[(color >> 4) & 0xE];
				r = levels[color & 0xE];
			}
			color_map[color] = render_map_color(r, g, b);
		}
		color_map_init_done = 1;
	}
	for (uint8_t color = 0; color < (1 << (3 + 1 + 1 + 1)); color++)
	{
		uint8_t src = color & DBG_SRC_MASK;
		if (src > DBG_SRC_S) {
			context->debugcolors[color] = 0;
		} else {
			uint8_t r,g,b;
			b = debug_base[src][0];
			g = debug_base[src][1];
			r = debug_base[src][2];
			if (color & DBG_PRIORITY)
			{
				if (b) {
					b += 48;
				}
				if (g) {
					g += 48;
				}
				if (r) {
					r += 48;
				}
			}
			if (color & DBG_SHADOW) {
				b /= 2;
				g /= 2;
				r /=2 ;
			}
			if (color & DBG_HILIGHT) {
				if (b) {
					b += 72;
				}
				if (g) {
					g += 72;
				}
				if (r) {
					r += 72;
				}
			}
			context->debugcolors[color] = render_map_color(r, g, b);
		}
	}
}

int is_refresh(vdp_context * context, uint32_t slot)
{
	if (context->latched_mode & BIT_H40) {
		return (slot == 37 || slot == 69 || slot == 102 || slot == 133 || slot == 165 || slot == 197 || slot >= 210);
	} else {
		//TODO: Figure out which slots are refresh when display is off in 32-cell mode
		//These numbers are guesses based on H40 numbers
		return (slot == 24 || slot == 56 || slot == 88 || slot == 120 || slot == 152);
		//The numbers below are the refresh slots during active display
		//return (slot == 66 || slot == 98 || slot == 130 || slot == 162);
	}
}

void render_sprite_cells(vdp_context * context)
{
	if (context->cur_slot >= context->sprite_draws) {
		sprite_draw * d = context->sprite_draw_list + context->cur_slot;

		uint16_t dir;
		int16_t x;
		if (d->h_flip) {
			x = d->x_pos + 7;
			dir = -1;
		} else {
			x = d->x_pos;
			dir = 1;
		}
		//printf("Draw Slot %d of %d, Rendering sprite cell from %X to x: %d\n", context->cur_slot, context->sprite_draws, d->address, x);
		context->cur_slot--;
		for (uint16_t address = d->address; address != ((d->address+4) & 0xFFFF); address++) {
			if (x >= 0 && x < 320 && !(context->linebuf[x] & 0xF)) {
				context->linebuf[x] = (context->vdpmem[address] >> 4) | d->pal_priority;
			}
			x += dir;
			if (x >= 0 && x < 320 && !(context->linebuf[x] & 0xF)) {
				context->linebuf[x] = (context->vdpmem[address] & 0xF)  | d->pal_priority;
			}
			x += dir;
		}
	}
}

void vdp_print_sprite_table(vdp_context * context)
{
	uint16_t sat_address = (context->regs[REG_SAT] & 0x7F) << 9;
	uint16_t current_index = 0;
	uint8_t count = 0;
	do {
		uint16_t address = current_index * 8 + sat_address;
		uint8_t height = ((context->vdpmem[address+2] & 0x3) + 1) * 8;
		uint8_t width = (((context->vdpmem[address+2]  >> 2) & 0x3) + 1) * 8;
		int16_t y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) & 0x1FF;
		int16_t x = ((context->vdpmem[address+ 6] & 0x3) << 8 | context->vdpmem[address + 7]) & 0x1FF;
		uint16_t link = context->vdpmem[address+3] & 0x7F;
		uint8_t pal = context->vdpmem[address + 4] >> 5 & 0x3;
		uint8_t pri = context->vdpmem[address + 4] >> 7;
		uint16_t pattern = ((context->vdpmem[address + 4] << 8 | context->vdpmem[address + 5]) & 0x7FF) << 5;
		//printf("Sprite %d: X=%d(%d), Y=%d(%d), Width=%u, Height=%u, Link=%u, Pal=%u, Pri=%u, Pat=%X\n", current_index, x, x-128, y, y-128, width, height, link, pal, pri, pattern);
		current_index = link;
		count++;
	} while (current_index != 0 && count < 80);
}

void vdp_print_reg_explain(vdp_context * context)
{
	char * hscroll[] = {"full", "7-line", "cell", "line"};
	printf("**Mode Group**\n"
	       "00: %.2X | H-ints %s, Pal Select %d, HVC latch %s, Display gen %s\n"
	       "01: %.2X | Display %s, V-ints %s, Height: %d, Mode %d\n"
	       "0B: %.2X | E-ints %s, V-Scroll: %s, H-Scroll: %s\n"
	       "0C: %.2X | Width: %d, Shadow/Highlight: %s\n",
	       context->regs[REG_MODE_1], context->regs[REG_MODE_1] & BIT_HINT_EN ? "enabled" : "disabled", context->regs[REG_MODE_1] & BIT_PAL_SEL != 0,
	           context->regs[REG_MODE_1] & BIT_HVC_LATCH ? "enabled" : "disabled", context->regs[REG_MODE_1] & BIT_DISP_DIS ? "disabled" : "enabled",
	       context->regs[REG_MODE_2], context->regs[REG_MODE_2] & BIT_DISP_EN ? "enabled" : "disabled", context->regs[REG_MODE_2] & BIT_VINT_EN ? "enabled" : "disabled",
	           context->regs[REG_MODE_2] & BIT_PAL ? 30 : 28, context->regs[REG_MODE_2] & BIT_MODE_5 ? 5 : 4,
	       context->regs[REG_MODE_3], context->regs[REG_MODE_3] & BIT_EINT_EN ? "enabled" : "disabled", context->regs[REG_MODE_3] & BIT_VSCROLL ? "2 cell" : "full",
	           hscroll[context->regs[REG_MODE_3] & 0x3],
	       context->regs[REG_MODE_4], context->regs[REG_MODE_4] & BIT_H40 ? 40 : 32, context->regs[REG_MODE_4] & BIT_HILIGHT ? "enabled" : "disabled");
	printf("\n**Table Group**\n"
	       "02: %.2X | Scroll A Name Table:    $%.4X\n"
	       "03: %.2X | Window Name Table:      $%.4X\n"
	       "04: %.2X | Scroll B Name Table:    $%.4X\n"
	       "05: %.2X | Sprite Attribute Table: $%.4X\n"
	       "0D: %.2X | HScroll Data Table:     $%.4X\n",
	       context->regs[REG_SCROLL_A], (context->regs[REG_SCROLL_A] & 0x38) << 10,
	       context->regs[REG_WINDOW], (context->regs[REG_WINDOW] & (context->regs[REG_MODE_4] & BIT_H40 ? 0x3C : 0x3E)) << 10,
	       context->regs[REG_SCROLL_B], (context->regs[REG_SCROLL_B] & 0x7) << 13,
	       context->regs[REG_SAT], (context->regs[REG_SAT] & (context->regs[REG_MODE_4] & BIT_H40 ? 0x3E : 0x3F)) << 9,
	       context->regs[REG_HSCROLL], (context->regs[REG_HSCROLL] & 0x1F) << 10);
	char * sizes[] = {"32", "64", "invalid", "128"};
	printf("\n**Misc Group**\n"
	       "07: %.2X | Backdrop Color: $%X\n"
	       "0A: %.2X | H-Int Counter: %u\n"
	       "0F: %.2X | Auto-increment: $%X\n"
	       "10: %.2X | Scroll A/B Size: %sx%s\n",
	       context->regs[REG_BG_COLOR], context->regs[REG_BG_COLOR] & 0x3F,
	       context->regs[REG_HINT], context->regs[REG_HINT],
	       context->regs[REG_AUTOINC], context->regs[REG_AUTOINC],
	       context->regs[REG_SCROLL], sizes[context->regs[REG_SCROLL] & 0x3], sizes[context->regs[REG_SCROLL] >> 4 & 0x3]);
	printf("\n**Internal Group**\n"
	       "Address: %X\n"
	       "CD:      %X\n"
	       "Pending: %s\n",
	       context->address, context->cd, (context->flags & FLAG_PENDING) ? "true" : "false");

	//TODO: Window Group, DMA Group
}

void scan_sprite_table(uint32_t line, vdp_context * context)
{
	if (context->sprite_index && context->slot_counter) {
		line += 1;
		line &= 0xFF;
		uint16_t ymask, ymin;
		uint8_t height_mult;
		if (context->double_res) {
			line *= 2;
			if (context->framebuf != context->oddbuf) {
				line++;
			}
			ymask = 0x3FF;
			ymin = 256;
			height_mult = 16;
		} else {
			ymask = 0x1FF;
			ymin = 128;
			height_mult = 8;
		}
		context->sprite_index &= 0x7F;
		if (context->latched_mode & BIT_H40) {
			if (context->sprite_index >= MAX_SPRITES_FRAME) {
				context->sprite_index = 0;
				return;
			}
		} else if(context->sprite_index >= MAX_SPRITES_FRAME_H32) {
			context->sprite_index = 0;
			return;
		}
		//TODO: Read from SAT cache rather than from VRAM
		uint16_t sat_address = (context->regs[REG_SAT] & 0x7F) << 9;
		uint16_t address = context->sprite_index * 8 + sat_address;
		line += ymin;
		uint16_t y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) & ymask;
		uint8_t height = ((context->vdpmem[address+2] & 0x3) + 1) * height_mult;
		//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
		if (y <= line && line < (y + height)) {
			//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
			context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
			context->sprite_info_list[context->slot_counter].index = context->sprite_index;
			context->sprite_info_list[context->slot_counter].y = y-ymin;
		}
		context->sprite_index = context->vdpmem[address+3] & 0x7F;
		if (context->sprite_index && context->slot_counter)
		{
			address = context->sprite_index * 8 + sat_address;
			y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) & ymask;
			height = ((context->vdpmem[address+2] & 0x3) + 1) * height_mult;
			//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
			if (y <= line && line < (y + height)) {
				//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
				context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
				context->sprite_info_list[context->slot_counter].index = context->sprite_index;
				context->sprite_info_list[context->slot_counter].y = y-ymin;
			}
			context->sprite_index = context->vdpmem[address+3] & 0x7F;
		}
	}
}

void read_sprite_x(uint32_t line, vdp_context * context)
{
	if (context->cur_slot >= context->slot_counter) {
		if (context->sprite_draws) {
			line += 1;
			line &= 0xFF;
			//in tiles
			uint8_t width = ((context->sprite_info_list[context->cur_slot].size >> 2) & 0x3) + 1;
			//in pixels
			uint8_t height = ((context->sprite_info_list[context->cur_slot].size & 0x3) + 1) * 8;
			if (context->double_res) {
				line *= 2;
				if (context->framebuf != context->oddbuf) {
					line++;
				}
				height *= 2;
			}
			uint16_t att_addr = ((context->regs[REG_SAT] & 0x7F) << 9) + context->sprite_info_list[context->cur_slot].index * 8 + 4;
			uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];
			uint8_t pal_priority = (tileinfo >> 9) & 0x70;
			uint8_t row;
			if (tileinfo & MAP_BIT_V_FLIP) {
				row = (context->sprite_info_list[context->cur_slot].y + height - 1) - line;
			} else {
				row = line-context->sprite_info_list[context->cur_slot].y;
			}
			uint16_t address;
			if (context->double_res) {
				address = ((tileinfo & 0x3FF) << 6) + row * 4;
			} else {
				address = ((tileinfo & 0x7FF) << 5) + row * 4;
			}
			int16_t x = ((context->vdpmem[att_addr+ 2] & 0x3) << 8 | context->vdpmem[att_addr + 3]) & 0x1FF;
			if (x) {
				context->flags |= FLAG_CAN_MASK;
			} else if(context->flags & (FLAG_CAN_MASK | FLAG_DOT_OFLOW)) {
				context->flags |= FLAG_MASKED;
			}

			context->flags &= ~FLAG_DOT_OFLOW;
			int16_t i;
			if (context->flags & FLAG_MASKED) {
				for (i=0; i < width && context->sprite_draws; i++) {
					--context->sprite_draws;
					context->sprite_draw_list[context->sprite_draws].x_pos = -128;
				}
			} else {
				x -= 128;
				int16_t base_x = x;
				int16_t dir;
				if (tileinfo & MAP_BIT_H_FLIP) {
					x += (width-1) * 8;
					dir = -8;
				} else {
					dir = 8;
				}
				//printf("Sprite %d | x: %d, y: %d, width: %d, height: %d, pal_priority: %X, row: %d, tile addr: %X\n", context->sprite_info_list[context->cur_slot].index, x, context->sprite_info_list[context->cur_slot].y, width, height, pal_priority, row, address);
				for (i=0; i < width && context->sprite_draws; i++, x += dir) {
					--context->sprite_draws;
					context->sprite_draw_list[context->sprite_draws].address = address + i * height * 4;
					context->sprite_draw_list[context->sprite_draws].x_pos = x;
					context->sprite_draw_list[context->sprite_draws].pal_priority = pal_priority;
					context->sprite_draw_list[context->sprite_draws].h_flip = (tileinfo & MAP_BIT_H_FLIP) ? 1 : 0;
				}
			}
			if (i < width) {
				context->flags |= FLAG_DOT_OFLOW;
			}
			context->cur_slot--;
		} else {
			context->flags |= FLAG_DOT_OFLOW;
		}
	}
}

void write_cram(vdp_context * context, uint16_t address, uint16_t value)
{
	uint16_t addr = (address/2) & (CRAM_SIZE-1);
	context->cram[addr] = value;
	context->colors[addr] = color_map[value & 0xEEE];
	context->colors[addr + CRAM_SIZE] = color_map[(value & 0xEEE) | FBUF_SHADOW];
	context->colors[addr + CRAM_SIZE*2] = color_map[(value & 0xEEE) | FBUF_HILIGHT];
}

#define VRAM_READ 0 //0000
#define VRAM_WRITE 1 //0001
//2 would trigger register write 0010
#define CRAM_WRITE 3 //0011
#define VSRAM_READ 4 //0100
#define VSRAM_WRITE 5//0101
//6 would trigger regsiter write 0110
//7 is a mystery
#define CRAM_READ 8  //1000
//9 is also a mystery //1001
//A would trigger register write 1010
//B is a mystery 1011
#define VRAM_READ8 0xC //1100
//D is a mystery 1101
//E would trigger register write 1110
//F is a mystery 1111
#define DMA_START 0x20

void external_slot(vdp_context * context)
{
	fifo_entry * start = context->fifo + context->fifo_read;
	/*if (context->flags2 & FLAG2_READ_PENDING) {
		context->flags2 &= ~FLAG2_READ_PENDING;
		context->flags |= FLAG_UNUSED_SLOT;
		return;
	}*/
	if (context->fifo_read >= 0 && start->cycle <= context->cycles) {
		switch (start->cd & 0xF)
		{
		case VRAM_WRITE:
			if (start->partial) {
				//printf("VRAM Write: %X to %X at %d (line %d, slot %d)\n", start->value, start->address ^ 1, context->cycles, context->cycles/MCLKS_LINE, (context->cycles%MCLKS_LINE)/16);
				context->last_fifo_val = start->value;
				context->vdpmem[start->address ^ 1] = start->value;
			} else {
				//printf("VRAM Write High: %X to %X at %d (line %d, slot %d)\n", start->value >> 8, start->address, context->cycles, context->cycles/MCLKS_LINE, (context->cycles%MCLKS_LINE)/16);
				context->vdpmem[start->address] = start->value >> 8;
				start->partial = 1;
				//skip auto-increment and removal of entry from fifo
				return;
			}
			break;
		case CRAM_WRITE: {
			//printf("CRAM Write | %X to %X\n", start->value, (start->address/2) & (CRAM_SIZE-1));
			write_cram(context, start->address, start->value);
			context->last_fifo_val = start->value;
			break;
		}
		case VSRAM_WRITE:
			if (((start->address/2) & 63) < VSRAM_SIZE) {
				//printf("VSRAM Write: %X to %X\n", start->value, context->address);
				context->vsram[(start->address/2) & 63] = start->value;
				context->last_fifo_val = start->value;
			}

			break;
		}
		context->fifo_read = (context->fifo_read+1) & (FIFO_SIZE-1);
		if (context->fifo_read == context->fifo_write) {
			context->fifo_read = -1;
		}
	} else {
		context->flags |= FLAG_UNUSED_SLOT;
	}
}

void run_dma_src(vdp_context * context, uint32_t slot)
{
	//TODO: Figure out what happens if CD bit 4 is not set in DMA copy mode
	//TODO: Figure out what happens when CD:0-3 is not set to a write mode in DMA operations
	//TODO: Figure out what happens if DMA gets disabled part way through a DMA fill or DMA copy
	if (context->fifo_write == context->fifo_read) {
		return;
	}
	uint16_t read_val;
	uint8_t ran_source = 0, partial = 0;
	uint16_t dma_len;
	switch(context->regs[REG_DMASRC_H] & 0xC0)
	{
	//68K -> VDP
	case 0:
	case 0x40:
		if (!slot || !is_refresh(context, slot-1)) {
			read_val = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
			ran_source = 1;
		}
		break;
	//Copy
	case 0xC0:
		if (context->flags & FLAG_UNUSED_SLOT) {
			switch(context->dma_cd & 0xF)
			{
			case VRAM_WRITE:
				read_val = context->vdpmem[(context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]];
				break;
			case CRAM_WRITE:
				read_val = context->cram[context->regs[REG_DMASRC_L] & (CRAM_SIZE-1)];
				break;
			case VSRAM_WRITE:
				if ((context->regs[REG_DMASRC_L] & 63) < VSRAM_SIZE) {
					read_val = context->vsram[context->regs[REG_DMASRC_L] & 63];
				} else {
					read_val = 0;
				}
				break;
			}
			ran_source = 1;
			context->flags &= ~FLAG_UNUSED_SLOT;
		}
		break;
	case 0x80:
		read_val = (context->cd & 0xF) == VRAM_WRITE ? context->last_write_val >> 8 : context->last_write_val;
		partial = 1;
		ran_source = 1;
		break;
	}

	if (ran_source) {
		fifo_entry * cur = context->fifo + context->fifo_write;
		cur->cycle = context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
		cur->address = context->address;
		cur->value = read_val;
		cur->cd = context->cd;
		cur->partial = partial;
		if (context->fifo_read < 0) {
			context->fifo_read = context->fifo_write;
		}
		context->fifo_write = (context->fifo_write+1) & (FIFO_SIZE-1);
		context->regs[REG_DMASRC_L] += 1;
		if (!context->regs[REG_DMASRC_L]) {
			context->regs[REG_DMASRC_M] += 1;
		}
		context->address += context->regs[REG_AUTOINC];
		dma_len = ((context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L]) - 1;
		context->regs[REG_DMALEN_H] = dma_len >> 8;
		context->regs[REG_DMALEN_L] = dma_len;
		if (!dma_len) {
			//printf("DMA end at cycle %d\n", context->cycles);
			context->flags &= ~FLAG_DMA_RUN;
			context->cd &= 0xF;
		}
	}
}

#define WINDOW_RIGHT 0x80
#define WINDOW_DOWN  0x80

void read_map_scroll(uint16_t column, uint16_t vsram_off, uint32_t line, uint16_t address, uint16_t hscroll_val, vdp_context * context)
{
	uint16_t window_line_shift, v_offset_mask, vscroll_shift;
	if (context->double_res) {
		line *= 2;
		if (context->framebuf != context->oddbuf) {
			line++;
		}
		window_line_shift = 4;
		v_offset_mask = 0xF;
		vscroll_shift = 4;
	} else {
		window_line_shift = 3;
		v_offset_mask = 0x7;
		vscroll_shift = 3;
	}
	if (!vsram_off) {
		uint16_t left_col, right_col;
		if (context->regs[REG_WINDOW_H] & WINDOW_RIGHT) {
			left_col = (context->regs[REG_WINDOW_H] & 0x1F) * 2;
			right_col = 42;
		} else {
			left_col = 0;
			right_col = (context->regs[REG_WINDOW_H] & 0x1F) * 2;
			if (right_col) {
				right_col += 2;
			}
		}
		uint16_t top_line, bottom_line;
		if (context->regs[REG_WINDOW_V] & WINDOW_DOWN) {
			top_line = (context->regs[REG_WINDOW_V] & 0x1F) << window_line_shift;
			bottom_line = context->double_res ? 481 : 241;
		} else {
			top_line = 0;
			bottom_line = (context->regs[REG_WINDOW_V] & 0x1F) << window_line_shift;
		}
		if ((column >= left_col && column < right_col) || (line >= top_line && line < bottom_line)) {
			uint16_t address = context->regs[REG_WINDOW] << 10;
			uint16_t line_offset, offset, mask;
			if (context->latched_mode & BIT_H40) {
				address &= 0xF000;
				line_offset = (((line) >> vscroll_shift) * 64 * 2) & 0xFFF;
				mask = 0x7F;

			} else {
				address &= 0xF800;
				line_offset = (((line) >> vscroll_shift) * 32 * 2) & 0xFFF;
				mask = 0x3F;
			}
			if (context->double_res) {
				mask <<= 1;
				mask |= 1;
			}
			offset = address + line_offset + (((column - 2) * 2) & mask);
			context->col_1 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			//printf("Window | top: %d, bot: %d, left: %d, right: %d, base: %X, line: %X offset: %X, tile: %X, reg: %X\n", top_line, bottom_line, left_col, right_col, address, line_offset, offset, ((context->col_1 & 0x3FF) << 5), context->regs[REG_WINDOW]);
			offset = address + line_offset + (((column - 1) * 2) & mask);
			context->col_2 = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
			context->v_offset = (line) & v_offset_mask;
			context->flags |= FLAG_WINDOW;
			return;
		}
		context->flags &= ~FLAG_WINDOW;
	}
	uint16_t vscroll;
	switch(context->regs[REG_SCROLL] & 0x30)
	{
	case 0:
		vscroll = 0xFF;
		break;
	case 0x10:
		vscroll = 0x1FF;
		break;
	case 0x20:
		//TODO: Verify this behavior
		vscroll = 0;
		break;
	case 0x30:
		vscroll = 0x3FF;
		break;
	}
	if (context->double_res) {
		vscroll <<= 1;
		vscroll |= 1;
	}
	vscroll &= (context->vsram[(context->regs[REG_MODE_3] & BIT_VSCROLL ? (column-2)&63 : 0) + vsram_off] + line);
	context->v_offset = vscroll & v_offset_mask;
	//printf("%s | line %d, vsram: %d, vscroll: %d, v_offset: %d\n",(vsram_off ? "B" : "A"), line, context->vsram[context->regs[REG_MODE_3] & 0x4 ? column : 0], vscroll, context->v_offset);
	vscroll >>= vscroll_shift;
	uint16_t hscroll_mask;
	uint16_t v_mul;
	switch(context->regs[REG_SCROLL] & 0x3)
	{
	case 0:
		hscroll_mask = 0x1F;
		v_mul = 64;
		break;
	case 0x1:
		hscroll_mask = 0x3F;
		v_mul = 128;
		break;
	case 0x2:
		//TODO: Verify this behavior
		hscroll_mask = 0;
		v_mul = 0;
		break;
	case 0x3:
		hscroll_mask = 0x7F;
		v_mul = 256;
		break;
	}
	uint16_t hscroll, offset;
	for (int i = 0; i < 2; i++) {
		hscroll = (column - 2 + i - ((hscroll_val/8) & 0xFFFE)) & hscroll_mask;
		offset = address + ((vscroll * v_mul + hscroll*2) & 0x1FFF);
		//printf("%s | line: %d, col: %d, x: %d, hs_mask %X, scr reg: %X, tbl addr: %X\n", (vsram_off ? "B" : "A"), line, (column-2+i), hscroll, hscroll_mask, context->regs[REG_SCROLL], offset);
		uint16_t col_val = (context->vdpmem[offset] << 8) | context->vdpmem[offset+1];
		if (i) {
			context->col_2 = col_val;
		} else {
			context->col_1 = col_val;
		}
	}
}

void read_map_scroll_a(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 0, line, (context->regs[REG_SCROLL_A] & 0x38) << 10, context->hscroll_a, context);
}

void read_map_scroll_b(uint16_t column, uint32_t line, vdp_context * context)
{
	read_map_scroll(column, 1, line, (context->regs[REG_SCROLL_B] & 0x7) << 13, context->hscroll_b, context);
}

void render_map(uint16_t col, uint8_t * tmp_buf, uint8_t offset, vdp_context * context)
{
	uint16_t address;
	uint8_t shift, add;
	if (context->double_res) {
		address = ((col & 0x3FF) << 6);
		shift = 1;
		add = context->framebuf != context->oddbuf ? 1 : 0;
	} else {
		address = ((col & 0x7FF) << 5);
		shift = 0;
		add = 0;
	}
	if (col & MAP_BIT_V_FLIP) {
		address +=  28 - 4 * context->v_offset/*((context->v_offset << shift) + add)*/;
	} else {
		address += 4 * context->v_offset/*((context->v_offset << shift) + add)*/;
	}
	uint16_t pal_priority = (col >> 9) & 0x70;
	int32_t dir;
	if (col & MAP_BIT_H_FLIP) {
		offset += 7;
		offset &= SCROLL_BUFFER_MASK;
		dir = -1;
	} else {
		dir = 1;
	}
	for (uint32_t i=0; i < 4; i++, address++)
	{
		tmp_buf[offset] = pal_priority | (context->vdpmem[address] >> 4);
		offset += dir;
		offset &= SCROLL_BUFFER_MASK;
		tmp_buf[offset] = pal_priority | (context->vdpmem[address] & 0xF);
		offset += dir;
		offset &= SCROLL_BUFFER_MASK;
	}
}

void render_map_1(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_a, context->buf_a_off, context);
}

void render_map_2(vdp_context * context)
{
	render_map(context->col_2, context->tmp_buf_a, context->buf_a_off+8, context);
}

void render_map_3(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_b, context->buf_b_off, context);
}

void render_map_output(uint32_t line, int32_t col, vdp_context * context)
{
	if (line >= 240) {
		return;
	}
	render_map(context->col_2, context->tmp_buf_b, context->buf_b_off+8, context);
	uint16_t *dst;
	uint32_t *dst32;
	uint8_t *sprite_buf,  *plane_a, *plane_b;
	int plane_a_off, plane_b_off;
	if (col)
	{
		col-=2;
		if (context->b32) {
			dst32 = context->framebuf;
			dst32 += line * 320 + col * 8;
		} else {
			dst = context->framebuf;
			dst += line * 320 + col * 8;
		}
		sprite_buf = context->linebuf + col * 8;
		uint8_t a_src, src;
		if (context->flags & FLAG_WINDOW) {
			plane_a_off = context->buf_a_off;
			a_src = DBG_SRC_W;
		} else {
			plane_a_off = context->buf_a_off - (context->hscroll_a & 0xF);
			a_src = DBG_SRC_A;
		}
		plane_b_off = context->buf_b_off - (context->hscroll_b & 0xF);
		//printf("A | tmp_buf offset: %d\n", 8 - (context->hscroll_a & 0x7));

		if (context->regs[REG_MODE_4] & BIT_HILIGHT) {
			for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i) {
				uint8_t pixel;
				plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
				plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
				uint32_t * colors = context->colors;
				src = 0;
				uint8_t sprite_color = *sprite_buf & 0x3F;
				if (sprite_color == 0x3E || sprite_color == 0x3F) {
					if (sprite_color == 0x3F) {
						colors += CRAM_SIZE;
						src = DBG_SHADOW;
					} else {
						colors += CRAM_SIZE*2;
						src = DBG_HILIGHT;
					}
					if (*plane_a & BUF_BIT_PRIORITY && *plane_a & 0xF) {
						pixel = *plane_a;
						src |= a_src;
					} else if (*plane_b & BUF_BIT_PRIORITY && *plane_b & 0xF) {
						pixel = *plane_b;
						src |= DBG_SRC_B;
					} else if (*plane_a & 0xF) {
						pixel = *plane_a;
						src |= a_src;
					} else if (*plane_b & 0xF){
						pixel = *plane_b;
						src |= DBG_SRC_B;
					} else {
						pixel = context->regs[REG_BG_COLOR] & 0x3F;
						src |= DBG_SRC_BG;
					}
				} else {
					if (*sprite_buf & BUF_BIT_PRIORITY && *sprite_buf & 0xF) {
						pixel = *sprite_buf;
						src = DBG_SRC_S;
					} else if (*plane_a & BUF_BIT_PRIORITY && *plane_a & 0xF) {
						pixel = *plane_a;
						src = a_src;
					} else if (*plane_b & BUF_BIT_PRIORITY && *plane_b & 0xF) {
						pixel = *plane_b;
						src = DBG_SRC_B;
					} else {
						if (!(*plane_a & BUF_BIT_PRIORITY || *plane_a & BUF_BIT_PRIORITY)) {
							colors += CRAM_SIZE;
							src = DBG_SHADOW;
						}
						if (*sprite_buf & 0xF) {
							pixel = *sprite_buf;
							if (*sprite_buf & 0xF == 0xE) {
								colors = context->colors;
								src = DBG_SRC_S;
							} else {
								src |= DBG_SRC_S;
							}
						} else if (*plane_a & 0xF) {
							pixel = *plane_a;
							src |= a_src;
						} else if (*plane_b & 0xF){
							pixel = *plane_b;
							src |= DBG_SRC_B;
						} else {
							pixel = context->regs[REG_BG_COLOR] & 0x3F;
							src |= DBG_SRC_BG;
						}
					}
				}
				pixel &= 0x3F;
				uint32_t outpixel;
				if (context->debug) {
					outpixel = context->debugcolors[src];
				} else {
					outpixel = colors[pixel];
				}
				if (context->b32) {
					*(dst32++) = outpixel;
				} else {
					*(dst++) = outpixel;
				}
				//*dst = (context->cram[pixel & 0x3F] & 0xEEE) | ((pixel & BUF_BIT_PRIORITY) ? FBUF_BIT_PRIORITY : 0) | src;
			}
		} else {
			for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i) {
				uint8_t pixel;
				src = 0;
				plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
				plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
				if (*sprite_buf & BUF_BIT_PRIORITY && *sprite_buf & 0xF) {
					pixel = *sprite_buf;
					src = DBG_SRC_S;
				} else if (*plane_a & BUF_BIT_PRIORITY && *plane_a & 0xF) {
					pixel = *plane_a;
					src = a_src;
				} else if (*plane_b & BUF_BIT_PRIORITY && *plane_b & 0xF) {
					pixel = *plane_b;
					src = DBG_SRC_B;
				} else if (*sprite_buf & 0xF) {
					pixel = *sprite_buf;
					src = DBG_SRC_S;
				} else if (*plane_a & 0xF) {
					pixel = *plane_a;
					src = a_src;
				} else if (*plane_b & 0xF){
					pixel = *plane_b;
					src = DBG_SRC_B;
				} else {
					pixel = context->regs[REG_BG_COLOR] & 0x3F;
					src = DBG_SRC_BG;
				}
				uint32_t outpixel;
				if (context->debug) {
					outpixel = context->debugcolors[src];
				} else {
					outpixel = context->colors[pixel & 0x3F];
				}
				if (context->b32) {
					*(dst32++) = outpixel;
				} else {
					*(dst++) = outpixel;
				}
				//*dst = (context->cram[pixel & 0x3F] & 0xEEE) | ((pixel & BUF_BIT_PRIORITY) ? FBUF_BIT_PRIORITY : 0) | src;
			}
		}
	} else {
		//dst = context->framebuf + line * 320;
		//sprite_buf = context->linebuf + col * 8;
		//plane_a = context->tmp_buf_a + 16 - (context->hscroll_a & 0x7);
		//plane_b = context->tmp_buf_b + 16 - (context->hscroll_b & 0x7);
		//end = dst + 8;
	}
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

#define COLUMN_RENDER_BLOCK(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, line, context);\
		break;\
	case (startcyc+1):\
		external_slot(context);\
		break;\
	case (startcyc+2):\
		render_map_1(context);\
		break;\
	case (startcyc+3):\
		render_map_2(context);\
		break;\
	case (startcyc+4):\
		read_map_scroll_b(column, line, context);\
		break;\
	case (startcyc+5):\
		read_sprite_x(line, context);\
		break;\
	case (startcyc+6):\
		render_map_3(context);\
		break;\
	case (startcyc+7):\
		render_map_output(line, column, context);\
		break;

#define COLUMN_RENDER_BLOCK_REFRESH(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, line, context);\
		break;\
	case (startcyc+1):\
		break;\
	case (startcyc+2):\
		render_map_1(context);\
		break;\
	case (startcyc+3):\
		render_map_2(context);\
		break;\
	case (startcyc+4):\
		read_map_scroll_b(column, line, context);\
		break;\
	case (startcyc+5):\
		read_sprite_x(line, context);\
		break;\
	case (startcyc+6):\
		render_map_3(context);\
		break;\
	case (startcyc+7):\
		render_map_output(line, column, context);\
		break;

void vdp_h40(uint32_t line, uint32_t linecyc, vdp_context * context)
{
	uint16_t address;
	uint32_t mask;
	switch(linecyc)
	{
	//sprite render to line buffer starts
	case 0:
		context->cur_slot = MAX_DRAWS-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
	case 1:
	case 2:
	case 3:
		if (line == 0xFF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		break;
	//sprite attribute table scan starts
	case 4:
		render_sprite_cells( context);
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE;
		scan_sprite_table(line, context);
		break;
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
	case 14:
	case 15:
	case 16:
	case 17:
	case 18:
	case 19:
	case 20:
	//!HSYNC asserted
	case 21:
	case 22:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 23:
		external_slot(context);
		break;
	case 24:
	case 25:
	case 26:
	case 27:
	case 28:
	case 29:
	case 30:
	case 31:
	case 32:
	case 33:
	case 34:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 35:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		line &= mask;
		address += line * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", line, context->hscroll_a, context->hscroll_b);
		break;
	case 36:
	//!HSYNC high
	case 37:
	case 38:
	case 39:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 40:
		read_map_scroll_a(0, line, context);
		break;
	case 41:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 42:
		render_map_1(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 43:
		render_map_2(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 44:
		read_map_scroll_b(0, line, context);
		break;
	case 45:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 46:
		render_map_3(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 47:
		render_map_output(line, 0, context);
		scan_sprite_table(line, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE-1;
		context->sprite_draws = MAX_DRAWS;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		break;
	COLUMN_RENDER_BLOCK(2, 48)
	COLUMN_RENDER_BLOCK(4, 56)
	COLUMN_RENDER_BLOCK(6, 64)
	COLUMN_RENDER_BLOCK_REFRESH(8, 72)
	COLUMN_RENDER_BLOCK(10, 80)
	COLUMN_RENDER_BLOCK(12, 88)
	COLUMN_RENDER_BLOCK(14, 96)
	COLUMN_RENDER_BLOCK_REFRESH(16, 104)
	COLUMN_RENDER_BLOCK(18, 112)
	COLUMN_RENDER_BLOCK(20, 120)
	COLUMN_RENDER_BLOCK(22, 128)
	COLUMN_RENDER_BLOCK_REFRESH(24, 136)
	COLUMN_RENDER_BLOCK(26, 144)
	COLUMN_RENDER_BLOCK(28, 152)
	COLUMN_RENDER_BLOCK(30, 160)
	COLUMN_RENDER_BLOCK_REFRESH(32, 168)
	COLUMN_RENDER_BLOCK(34, 176)
	COLUMN_RENDER_BLOCK(36, 184)
	COLUMN_RENDER_BLOCK(38, 192)
	COLUMN_RENDER_BLOCK_REFRESH(40, 200)
	case 208:
	case 209:
		external_slot(context);
		break;
	default:
		//leftovers from HSYNC clock change nonsense
		break;
	}
}

void vdp_h32(uint32_t line, uint32_t linecyc, vdp_context * context)
{
	uint16_t address;
	uint32_t mask;
	switch(linecyc)
	{
	//sprite render to line buffer starts
	case 0:
		context->cur_slot = MAX_DRAWS_H32-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
	case 1:
	case 2:
	case 3:
		if (line == 0xFF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		break;
	//sprite attribute table scan starts
	case 4:
		render_sprite_cells( context);
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE_H32;
		scan_sprite_table(line, context);
		break;
	case 5:
	case 6:
	case 7:
	case 8:
	case 9:
	case 10:
	case 11:
	case 12:
	case 13:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
	case 14:
		external_slot(context);
		break;
	case 15:
	case 16:
	case 17:
	case 18:
	case 19:
	//HSYNC start
	case 20:
	case 21:
	case 22:
	case 23:
	case 24:
	case 25:
	case 26:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 27:
		external_slot(context);
		break;
	case 28:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		line &= mask;
		address += line * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", line, context->hscroll_a, context->hscroll_b);
		break;
	case 29:
	case 30:
	case 31:
	case 32:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	//!HSYNC high
	case 33:
		read_map_scroll_a(0, line, context);
		break;
	case 34:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 35:
		render_map_1(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 36:
		render_map_2(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 37:
		read_map_scroll_b(0, line, context);
		break;
	case 38:
		render_sprite_cells(context);
		scan_sprite_table(line, context);
		break;
	case 39:
		render_map_3(context);
		scan_sprite_table(line, context);//Just a guess
		break;
	case 40:
		render_map_output(line, 0, context);
		scan_sprite_table(line, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE_H32-1;
		context->sprite_draws = MAX_DRAWS_H32;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		break;
	COLUMN_RENDER_BLOCK(2, 41)
	COLUMN_RENDER_BLOCK(4, 49)
	COLUMN_RENDER_BLOCK(6, 57)
	COLUMN_RENDER_BLOCK_REFRESH(8, 65)
	COLUMN_RENDER_BLOCK(10, 73)
	COLUMN_RENDER_BLOCK(12, 81)
	COLUMN_RENDER_BLOCK(14, 89)
	COLUMN_RENDER_BLOCK_REFRESH(16, 97)
	COLUMN_RENDER_BLOCK(18, 105)
	COLUMN_RENDER_BLOCK(20, 113)
	COLUMN_RENDER_BLOCK(22, 121)
	COLUMN_RENDER_BLOCK_REFRESH(24, 129)
	COLUMN_RENDER_BLOCK(26, 137)
	COLUMN_RENDER_BLOCK(28, 145)
	COLUMN_RENDER_BLOCK(30, 153)
	COLUMN_RENDER_BLOCK_REFRESH(32, 161)
	case 169:
	case 170:
		external_slot(context);
		break;
	}
}
void latch_mode(vdp_context * context)
{
	context->latched_mode = (context->regs[REG_MODE_4] & 0x81) | (context->regs[REG_MODE_2] & BIT_PAL);
}

void check_render_bg(vdp_context * context, int32_t line, uint32_t slot)
{
	if (line > 0) {
		line -= 1;
		int starti = -1;
		if (context->latched_mode & BIT_H40) {
			if (slot >= 55 && slot < 210) {
				uint32_t x = (slot-55)*2;
				starti = line * 320 + x;
			} else if (slot < 5) {
				uint32_t x = (slot + 155)*2;
				starti = (line-1)*320 + x;
			}
		} else {
			if (slot >= 48 && slot < 171) {
				uint32_t x = (slot-48)*2;
				starti = line * 320 + x;
			} else if (slot < 5) {
				uint32_t x = (slot + 123)*2;
				starti = (line-1)*320 + x;
			}
		}
		if (starti >= 0) {
			if (context->b32) {
				uint32_t color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
				uint32_t * start = context->framebuf;
				start += starti;
				for (int i = 0; i < 2; i++) {
					*(start++) = color;
				}
			} else {
				uint16_t color = context->colors[context->regs[REG_BG_COLOR] & 0x3F];
				uint16_t * start = context->framebuf;
				start += starti;
				for (int i = 0; i < 2; i++) {
					*(start++) = color;
				}
			}
		}
	}
}

void vdp_run_context(vdp_context * context, uint32_t target_cycles)
{
	while(context->cycles < target_cycles)
	{
		context->flags &= ~FLAG_UNUSED_SLOT;
		uint32_t line = context->cycles / MCLKS_LINE;
		uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
		if (!context->cycles) {
			latch_mode(context);
		}
		uint32_t linecyc = context->cycles % MCLKS_LINE;
		if (linecyc == 0) {
			if (line <= 1 || line >= active_lines) {
				context->hint_counter = context->regs[REG_HINT];
			} else if (context->hint_counter) {
				context->hint_counter--;
			} else {
				context->flags2 |= FLAG2_HINT_PENDING;
				context->hint_counter = context->regs[REG_HINT];
			}
		} else if(line == active_lines) {
			uint32_t intcyc = context->latched_mode & BIT_H40 ? VINT_CYCLE_H40 :  VINT_CYCLE_H32;
			if (linecyc == intcyc) {
				context->flags2 |= FLAG2_VINT_PENDING;
			}
		}
		uint32_t inccycles, slot;
		if (context->latched_mode & BIT_H40){
			if (linecyc < MCLKS_SLOT_H40*HSYNC_SLOT_H40) {
				slot = linecyc/MCLKS_SLOT_H40;
				inccycles = MCLKS_SLOT_H40;
			} else if(linecyc < MCLK_WEIRD_END) {
				switch(linecyc-(MCLKS_SLOT_H40*HSYNC_SLOT_H40))
				{
				case 0:
					inccycles = 19;
					slot = 0;
					break;
				case 19:
					slot = 1;
					inccycles = 20;
					break;
				case 39:
					slot = 2;
					inccycles = 20;
					break;
				case 59:
					slot = 3;
					inccycles = 20;
					break;
				case 79:
					slot = 4;
					inccycles = 18;
					break;
				case 97:
					slot = 5;
					inccycles = 20;
					break;
				case 117:
					slot = 6;
					inccycles = 20;
					break;
				case 137:
					slot = 7;
					inccycles = 20;
					break;
				case 157:
					slot = 8;
					inccycles = 18;
					break;
				case 175:
					slot = 9;
					inccycles = 20;
					break;
				case 195:
					slot = 10;
					inccycles = 20;
					break;
				case 215:
					slot = 11;
					inccycles = 20;
					break;
				case 235:
					slot = 12;
					inccycles = 18;
					break;
				case 253:
					slot = 13;
					inccycles = 20;
					break;
				case 273:
					slot = 14;
					inccycles = 20;
					break;
				case 293:
					slot = 15;
					inccycles = 20;
					break;
				case 313:
					slot = 16;
					inccycles = 19;
					break;
				default:
					fprintf(stderr, "cycles after weirdness %d\n", linecyc-(MCLKS_SLOT_H40*HSYNC_SLOT_H40));
					exit(1);
				}
				slot += HSYNC_SLOT_H40;
			} else {
				slot = (linecyc-MCLK_WEIRD_END)/MCLKS_SLOT_H40 + SLOT_WEIRD_END;
				inccycles = MCLKS_SLOT_H40;
			}
		} else {
			inccycles = MCLKS_SLOT_H32;
			slot = linecyc/MCLKS_SLOT_H32;
		}
		if ((line < active_lines || (line == active_lines && linecyc < (context->latched_mode & BIT_H40 ? 64 : 80))) && context->regs[REG_MODE_2] & DISPLAY_ENABLE) {
			//first sort-of active line is treated as 255 internally
			//it's used for gathering sprite info for line
			line = (line - 1) & 0xFF;

			//Convert to slot number
			if (context->latched_mode & BIT_H40){
				vdp_h40(line, slot, context);
			} else {
				vdp_h32(line, slot, context);
			}
		} else {
			if (!is_refresh(context, slot)) {
				external_slot(context);
			}
			if (line < active_lines) {
				check_render_bg(context, line, slot);
			}
		}
		if (context->flags & FLAG_DMA_RUN && !is_refresh(context, slot)) {
			run_dma_src(context, slot);
		}
		context->cycles += inccycles;
	}
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t target_cycles = ((context->latched_mode & BIT_PAL) ? PAL_ACTIVE : NTSC_ACTIVE) * MCLKS_LINE;
	vdp_run_context(context, target_cycles);
	return context->cycles;
}

void vdp_run_dma_done(vdp_context * context, uint32_t target_cycles)
{
	for(;;) {
		uint32_t dmalen = (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L];
		if (!dmalen) {
			dmalen = 0x10000;
		}
		uint32_t min_dma_complete = dmalen * (context->latched_mode & BIT_H40 ? 16 : 20);
		if ((context->regs[REG_DMASRC_H] & 0xC0) == 0xC0 || (context->cd & 0xF) == VRAM_WRITE) {
			//DMA copies take twice as long to complete since they require a read and a write
			//DMA Fills and transfers to VRAM also take twice as long as it requires 2 writes for a single word
			min_dma_complete *= 2;
		}
		min_dma_complete += context->cycles;
		if (target_cycles < min_dma_complete) {
			vdp_run_context(context, target_cycles);
			return;
		} else {
			vdp_run_context(context, min_dma_complete);
			if (!(context->flags & FLAG_DMA_RUN)) {
				return;
			}
		}
	}
}

int vdp_control_port_write(vdp_context * context, uint16_t value)
{
	//printf("control port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_DMA_RUN) {
		return -1;
	}
	if (context->flags & FLAG_PENDING) {
		context->address = (context->address & 0x3FFF) | (value << 14);
		context->cd = (context->cd & 0x3) | ((value >> 2) & 0x3C);
		context->flags &= ~FLAG_PENDING;
		//printf("New Address: %X, New CD: %X\n", context->address, context->cd);
		if (context->cd & 0x20 && (context->regs[REG_MODE_2] & BIT_DMA_ENABLE)) {
			//
			if((context->regs[REG_DMASRC_H] & 0xC0) != 0x80) {
				//DMA copy or 68K -> VDP, transfer starts immediately
				context->flags |= FLAG_DMA_RUN;
				context->dma_cd = context->cd;
				//printf("DMA start at cycle %d\n", context->cycles);
				if (!(context->regs[REG_DMASRC_H] & 0x80)) {
					//printf("DMA Address: %X, New CD: %X, Source: %X, Length: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_H] << 17) | (context->regs[REG_DMASRC_M] << 9) | (context->regs[REG_DMASRC_L] << 1), context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L]);
					return 1;
				} else {
					//printf("DMA Copy Address: %X, New CD: %X, Source: %X\n", context->address, context->cd, (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
				}
			} else {
				//printf("DMA Fill Address: %X, New CD: %X\n", context->address, context->cd);
			}
		}
	} else {
		if ((value & 0xC000) == 0x8000) {
			//Register write
			uint8_t reg = (value >> 8) & 0x1F;
			if (reg < (context->regs[REG_MODE_2] & BIT_MODE_5 ? VDP_REGS : 0xA)) {
				//printf("register %d set to %X\n", reg, value & 0xFF);
				context->regs[reg] = value;
				if (reg == REG_MODE_2) {
					//printf("Display is now %s\n", (context->regs[REG_MODE_2] & DISPLAY_ENABLE) ? "enabled" : "disabled");
				}
				if (reg == REG_MODE_4) {
					context->double_res = (value & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
					if (!context->double_res) {
						context->framebuf = context->oddbuf;
					}
				}
			}
		} else {
			context->flags |= FLAG_PENDING;
			context->address = (context->address &0xC000) | (value & 0x3FFF);
			context->cd = (context->cd &0x3C) | (value >> 14);
		}
	}
	return 0;
}

int vdp_data_port_write(vdp_context * context, uint16_t value)
{
	//printf("data port write: %X at %d\n", value, context->cycles);
	if (context->flags & FLAG_DMA_RUN && (context->regs[REG_DMASRC_H] & 0xC0) != 0x80) {
		return -1;
	}
	context->flags &= ~FLAG_PENDING;
	/*if (context->fifo_cur == context->fifo_end) {
		printf("FIFO full, waiting for space before next write at cycle %X\n", context->cycles);
	}*/
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80) {
		context->flags &= ~FLAG_DMA_RUN;
	}
	while (context->fifo_write == context->fifo_read) {
		vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	context->last_write_val = value;
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80) {
		context->flags |= FLAG_DMA_RUN;
	}
	cur->cd = context->cd;
	cur->partial = 0;
	if (context->fifo_read < 0) {
		context->fifo_read = context->fifo_write;
	}
	context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
	context->address += context->regs[REG_AUTOINC];
	return 0;
}

void vdp_test_port_write(vdp_context * context, uint16_t value)
{
	//TODO: implement test register
}

uint16_t vdp_control_port_read(vdp_context * context)
{
	context->flags &= ~FLAG_PENDING;
	uint16_t value = 0x3400;
	if (context->fifo_read < 0) {
		value |= 0x200;
	}
	if (context->fifo_read == context->fifo_write) {
		value |= 0x100;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		value |= 0x80;
	}
	if ((context->regs[REG_MODE_4] & BIT_INTERLACE) && context->framebuf == context->oddbuf) {
		value |= 0x10;
	}
	uint32_t line= context->cycles / MCLKS_LINE;
	uint32_t linecyc = context->cycles % MCLKS_LINE;
	if (line >= (context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE)) {
		value |= 0x8;
	}
	if (linecyc < (context->latched_mode & BIT_H40 ? HBLANK_CLEAR_H40 : HBLANK_CLEAR_H32)) {
		value |= 0x4;
	}
	if (context->flags & FLAG_DMA_RUN) {
		value |= 0x2;
	}
	if (context->latched_mode & BIT_PAL) {//Not sure about this, need to verify
		value |= 0x1;
	}
	//printf("status read at cycle %d returned %X\n", context->cycles, value);
	//TODO: Sprite overflow, sprite collision, odd frame flag
	return value;
}

#define CRAM_BITS 0xEEE
#define VSRAM_BITS 0x7FF
#define VSRAM_DIRTY_BITS 0xF800

uint16_t vdp_data_port_read(vdp_context * context)
{
	context->flags &= ~FLAG_PENDING;
	if (context->cd & 1) {
		return 0;
	}
	//Not sure if the FIFO should be drained before processing a read or not, but it would make sense
	context->flags &= ~FLAG_UNUSED_SLOT;
	//context->flags2 |= FLAG2_READ_PENDING;
	while (!(context->flags & FLAG_UNUSED_SLOT)) {
		vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
	}
	uint16_t value = 0;
	switch (context->cd & 0xF)
	{
	case VRAM_READ:
		value = context->vdpmem[context->address & 0xFFFE] << 8;
		context->flags &= ~FLAG_UNUSED_SLOT;
		context->flags2 |= FLAG2_READ_PENDING;
		while (!(context->flags & FLAG_UNUSED_SLOT)) {
			vdp_run_context(context, context->cycles + ((context->latched_mode & BIT_H40) ? 16 : 20));
		}
		value |= context->vdpmem[context->address | 1];
		break;
	case VRAM_READ8:
		value = context->vdpmem[context->address ^ 1];
		value |= context->fifo[context->fifo_write].value & 0xFF00;
		break;
	case CRAM_READ:
		value = context->cram[(context->address/2) & (CRAM_SIZE-1)] & CRAM_BITS;
		value |= context->fifo[context->fifo_write].value & ~CRAM_BITS;
		break;
	case VSRAM_READ:
		if (((context->address / 2) & 63) < VSRAM_SIZE) {
			value = context->vsram[(context->address / 2) & 63] & VSRAM_BITS;
			value |= context->fifo[context->fifo_write].value & VSRAM_DIRTY_BITS;
		}
		break;
	}
	context->address += context->regs[REG_AUTOINC];
	return value;
}

uint16_t vdp_hv_counter_read(vdp_context * context)
{
	//TODO: deal with clock adjustemnts handled in vdp_run_context
	uint32_t line= context->cycles / MCLKS_LINE;
	if (!line) {
		line = 0xFF;
	} else {
		line--;
		if (line > 0xEA) {
			line = (line + 0xFA) & 0xFF;
		}
	}
	uint32_t linecyc = context->cycles % MCLKS_LINE;
	if (context->latched_mode & BIT_H40) {
		uint32_t slot;
		if (linecyc < MCLKS_SLOT_H40*HSYNC_SLOT_H40) {
			slot = linecyc/MCLKS_SLOT_H40;
		} else if(linecyc < MCLK_WEIRD_END) {
			switch(linecyc-(MCLKS_SLOT_H40*HSYNC_SLOT_H40))
			{
			case 0:
				slot = 0;
				break;
			case 19:
				slot = 1;
				break;
			case 39:
				slot = 2;
				break;
			case 59:
				slot = 2;
				break;
			case 79:
				slot = 3;
				break;
			case 97:
				slot = 4;
				break;
			case 117:
				slot = 5;
				break;
			case 137:
				slot = 6;
				break;
			case 157:
				slot = 7;
				break;
			case 175:
				slot = 8;
				break;
			case 195:
				slot = 9;
				break;
			case 215:
				slot = 11;
				break;
			case 235:
				slot = 12;
				break;
			case 253:
				slot = 13;
				break;
			case 273:
				slot = 14;
				break;
			case 293:
				slot = 15;
				break;
			case 313:
				slot = 16;
				break;
			default:
				fprintf(stderr, "cycles after weirdness %d\n", linecyc-(MCLKS_SLOT_H40*HSYNC_SLOT_H40));
				exit(1);
			}
			slot += HSYNC_SLOT_H40;
		} else {
			slot = (linecyc-MCLK_WEIRD_END)/MCLKS_SLOT_H40 + SLOT_WEIRD_END;
		}
		linecyc = slot * 2;
		if (linecyc >= 86) {
			linecyc -= 86;
		} else {
			linecyc += 334;
		}
		if (linecyc > 0x16C) {
			linecyc += 92;
		}
	} else {
		linecyc /= 10;
		if (linecyc >= 74) {
			linecyc -= 74;
		} else {
			linecyc += 268;
		}
		if (linecyc > 0x127) {
			linecyc += 170;
		}
	}
	linecyc &= 0xFF;
	if (context->double_res) {
		line <<= 1;
		if (line & 0x100) {
			line |= 1;
		}
	}
	return (line << 8) | linecyc;
}

uint16_t vdp_test_port_read(vdp_context * context)
{
	//TODO: Find out what actually gets returned here
	return 0xFFFF;
}

void vdp_adjust_cycles(vdp_context * context, uint32_t deduction)
{
	context->cycles -= deduction;
	if (context->fifo_read >= 0) {
		int32_t idx = context->fifo_read;
		do {
			if (context->fifo[idx].cycle >= deduction) {
				context->fifo[idx].cycle -= deduction;
			} else {
				context->fifo[idx].cycle = 0;
			}
			idx = (idx+1) & (FIFO_SIZE-1);
		} while(idx != context->fifo_write);
	}
}

uint32_t vdp_next_hint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_1] & BIT_HINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_HINT_PENDING) {
		return context->cycles;
	}
	uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
	uint32_t line = context->cycles / MCLKS_LINE;
	if (line >= active_lines) {
		return 0xFFFFFFFF;
	}
	uint32_t linecyc = context->cycles % MCLKS_LINE;
	uint32_t hcycle = context->cycles + context->hint_counter * MCLKS_LINE + MCLKS_LINE - linecyc;
	if (!line) {
		hcycle += MCLKS_LINE;
	}
	return hcycle;
}

uint32_t vdp_next_vint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_2] & BIT_VINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		return context->cycles;
	}
	uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
	uint32_t vcycle =  MCLKS_LINE * active_lines;
	if (context->latched_mode & BIT_H40) {
		vcycle += VINT_CYCLE_H40;
	} else {
		vcycle += VINT_CYCLE_H32;
	}
	if (vcycle < context->cycles) {
		return 0xFFFFFFFF;
	}
	return vcycle;
}

uint32_t vdp_next_vint_z80(vdp_context * context)
{
	uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
	uint32_t vcycle =  MCLKS_LINE * active_lines;
	if (context->latched_mode & BIT_H40) {
		vcycle += VINT_CYCLE_H40;
	} else {
		vcycle += VINT_CYCLE_H32;
	}
	return vcycle;
}

void vdp_int_ack(vdp_context * context, uint16_t int_num)
{
	if (int_num == 6) {
		context->flags2 &= ~FLAG2_VINT_PENDING;
	} else if(int_num ==4) {
		context->flags2 &= ~FLAG2_HINT_PENDING;
	}
}

