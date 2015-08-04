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

#define NTSC_INACTIVE_START 224
#define PAL_INACTIVE_START 240
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define SCROLL_BUFFER_SIZE 32
#define SCROLL_BUFFER_MASK (SCROLL_BUFFER_SIZE-1)
#define SCROLL_BUFFER_DRAW (SCROLL_BUFFER_SIZE/2)

#define MCLKS_SLOT_H40  16
#define MCLKS_SLOT_H32  20
#define VINT_SLOT_H40  4 //21 slots before HSYNC, 16 during, 10 after
#define VINT_SLOT_H32  4  //old value was 23, but recent tests suggest the actual value is close to the H40 one
#define HSYNC_SLOT_H40  234
#define HSYNC_END_H40  (HSYNC_SLOT_H40+17)
#define HSYNC_END_H32   (33 * MCLKS_SLOT_H32)
#define HBLANK_START_H40 178 //should be 179 according to Nemesis, but 178 seems to fit slightly better with my test ROM results
#define HBLANK_END_H40  0 //should be 5.5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define HBLANK_START_H32 233 //should be 147 according to Nemesis which is very different from my test ROM result
#define HBLANK_END_H32 0 //should be 5 according to Nemesis, but 0 seems to fit better with my test ROM results
#define LINE_CHANGE_H40 165
#define LINE_CHANGE_H32 132
#define VBLANK_START_H40 (LINE_CHANGE_H40+2)
#define VBLANK_START_H32 (LINE_CHANGE_H32+2)
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

void init_vdp_context(vdp_context * context, uint8_t region_pal)
{
	memset(context, 0, sizeof(*context));
	context->vdpmem = malloc(VRAM_SIZE);
	memset(context->vdpmem, 0, VRAM_SIZE);
	/*
	*/
	if (headless) {
		context->oddbuf = context->framebuf = malloc(FRAMEBUF_ENTRIES * (32 / 8));
		memset(context->framebuf, 0, FRAMEBUF_ENTRIES * (32 / 8));
		context->evenbuf = malloc(FRAMEBUF_ENTRIES * (32 / 8));
		memset(context->evenbuf, 0, FRAMEBUF_ENTRIES * (32 / 8));
	} else {
		render_alloc_surfaces(context);
	}
	context->framebuf = context->oddbuf;
	context->linebuf = malloc(LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	memset(context->linebuf, 0, LINEBUF_SIZE + SCROLL_BUFFER_SIZE*2);
	context->tmp_buf_a = context->linebuf + LINEBUF_SIZE;
	context->tmp_buf_b = context->tmp_buf_a + SCROLL_BUFFER_SIZE;
	context->sprite_draws = MAX_DRAWS;
	context->fifo_write = 0;
	context->fifo_read = -1;

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
	if (region_pal) {
		context->flags2 |= FLAG2_REGION_PAL;
	}
}

int is_refresh(vdp_context * context, uint32_t slot)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		return slot == 250 || slot == 26 || slot == 59 || slot == 90 || slot == 122 || slot == 154;
	} else {
		//TODO: Figure out which slots are refresh when display is off in 32-cell mode
		//These numbers are guesses based on H40 numbers
		return slot == 243 || slot == 19 || slot == 51 || slot == 83 || slot == 115;
		//The numbers below are the refresh slots during active display
		//return (slot == 29 || slot == 61 || slot == 93 || slot == 125);
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
				if (context->linebuf[x] && (context->vdpmem[address] >> 4)) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
				context->linebuf[x] = (context->vdpmem[address] >> 4) | d->pal_priority;
			}
			x += dir;
			if (x >= 0 && x < 320 && !(context->linebuf[x] & 0xF)) {
				if (context->linebuf[x] && (context->vdpmem[address] & 0xF)) {
					context->flags2 |= FLAG2_SPRITE_COLLIDE;
				}
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
		printf("Sprite %d: X=%d(%d), Y=%d(%d), Width=%u, Height=%u, Link=%u, Pal=%u, Pri=%u, Pat=%X\n", current_index, x, x-128, y, y-128, width, height, link, pal, pri, pattern);
		current_index = link;
		count++;
	} while (current_index != 0 && count < 80);
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

const char * cd_name(uint8_t cd)
{
	switch (cd & 0xF)
	{
	case VRAM_READ:
		return "VRAM read";
	case VRAM_WRITE:
		return "VRAM write";
	case CRAM_WRITE:
		return "CRAM write";
	case VSRAM_READ:
		return "VSRAM read";
	case VSRAM_WRITE:
		return "VSRAM write";
	case VRAM_READ8:
		return "VRAM read (undocumented 8-bit mode)";
	default:
		return "invalid";
	}
}

void vdp_print_reg_explain(vdp_context * context)
{
	char * hscroll[] = {"full", "7-line", "cell", "line"};
	printf("**Mode Group**\n"
	       "00: %.2X | H-ints %s, Pal Select %d, HVC latch %s, Display gen %s\n"
	       "01: %.2X | Display %s, V-ints %s, Height: %d, Mode %d\n"
	       "0B: %.2X | E-ints %s, V-Scroll: %s, H-Scroll: %s\n"
	       "0C: %.2X | Width: %d, Shadow/Highlight: %s\n",
	       context->regs[REG_MODE_1], context->regs[REG_MODE_1] & BIT_HINT_EN ? "enabled" : "disabled", (context->regs[REG_MODE_1] & BIT_PAL_SEL) != 0,
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
	       context->regs[REG_SAT], (context->regs[REG_SAT] & (context->regs[REG_MODE_4] & BIT_H40 ? 0x7E : 0x7F)) << 9,
	       context->regs[REG_HSCROLL], (context->regs[REG_HSCROLL] & 0x3F) << 10);
	char * sizes[] = {"32", "64", "invalid", "128"};
	printf("\n**Misc Group**\n"
	       "07: %.2X | Backdrop Color: $%X\n"
	       "0A: %.2X | H-Int Counter: %u\n"
	       "0F: %.2X | Auto-increment: $%X\n"
	       "10: %.2X | Scroll A/B Size: %sx%s\n",
	       context->regs[REG_BG_COLOR], context->regs[REG_BG_COLOR],
	       context->regs[REG_HINT], context->regs[REG_HINT],
	       context->regs[REG_AUTOINC], context->regs[REG_AUTOINC],
	       context->regs[REG_SCROLL], sizes[context->regs[REG_SCROLL] & 0x3], sizes[context->regs[REG_SCROLL] >> 4 & 0x3]);
	char * src_types[] = {"68K", "68K", "Copy", "Fill"};
	printf("\n**DMA Group**\n"
	       "13: %.2X |\n"
		   "14: %.2X | DMA Length: $%.4X words\n"
		   "15: %.2X |\n"
		   "16: %.2X |\n"
		   "17: %.2X | DMA Source Address: $%.6X, Type: %s\n",
		   context->regs[REG_DMALEN_L],
		   context->regs[REG_DMALEN_H], context->regs[REG_DMALEN_H] << 8 | context->regs[REG_DMALEN_L],
		   context->regs[REG_DMASRC_L],
		   context->regs[REG_DMASRC_M],
		   context->regs[REG_DMASRC_H],
		       context->regs[REG_DMASRC_H] << 17 | context->regs[REG_DMASRC_M] << 9 | context->regs[REG_DMASRC_L] << 1,
			   src_types[context->regs[REG_DMASRC_H] >> 6 & 3]);
	printf("\n**Internal Group**\n"
	       "Address: %X\n"
	       "CD:      %X - %s\n"
	       "Pending: %s\n"
		   "VCounter: %d\n"
		   "HCounter: %d\n",
	       context->address, context->cd, cd_name(context->cd), (context->flags & FLAG_PENDING) ? "true" : "false",
		   context->vcounter, context->hslot*2);

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
		if (context->regs[REG_MODE_4] & BIT_H40) {
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
				context->vdpmem[start->address ^ 1] = start->partial == 2 ? start->value >> 8 : start->value;
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
			write_cram(context, start->address, start->partial == 2 ? context->fifo[context->fifo_write].value : start->value);
			break;
		}
		case VSRAM_WRITE:
			if (((start->address/2) & 63) < VSRAM_SIZE) {
				//printf("VSRAM Write: %X to %X @ vcounter: %d, hslot: %d, cycle: %d\n", start->value, context->address, context->vcounter, context->hslot, context->cycles);
				context->vsram[(start->address/2) & 63] = start->partial == 2 ? context->fifo[context->fifo_write].value : start->value;
			}

			break;
		}
		context->fifo_read = (context->fifo_read+1) & (FIFO_SIZE-1);
		if (context->fifo_read == context->fifo_write) {
			context->fifo_read = -1;
		}
		context->flags &= ~FLAG_UNUSED_SLOT;
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
	fifo_entry * cur = NULL;
	switch(context->regs[REG_DMASRC_H] & 0xC0)
	{
	//68K -> VDP
	case 0:
	case 0x40:
		if (!slot || !is_refresh(context, slot-1)) {
			cur = context->fifo + context->fifo_write;
			cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
			cur->address = context->address;
			cur->value = read_dma_value((context->regs[REG_DMASRC_H] << 16) | (context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L]);
			cur->cd = context->cd;
			cur->partial = 0;
			if (context->fifo_read < 0) {
				context->fifo_read = context->fifo_write;
			}
			context->fifo_write = (context->fifo_write + 1) & (FIFO_SIZE-1);
		}
		break;
	//Copy
	case 0xC0:
		if (context->flags & FLAG_UNUSED_SLOT && context->fifo_read < 0) {
			//TODO: Fix this to not use the FIFO at all once read-caching is properly implemented
			context->fifo_read = (context->fifo_write-1) & (FIFO_SIZE-1);
			cur = context->fifo + context->fifo_read;
			cur->cycle = context->cycles;
			cur->address = context->address;
			cur->partial = 1;
			cur->value = context->vdpmem[(context->regs[REG_DMASRC_M] << 8) | context->regs[REG_DMASRC_L] ^ 1] | (cur->value & 0xFF00);
			cur->cd = VRAM_WRITE;
			context->flags &= ~FLAG_UNUSED_SLOT;
		}
		break;
	case 0x80:
		if (context->fifo_read < 0) {
			context->fifo_read = (context->fifo_write-1) & (FIFO_SIZE-1);
			cur = context->fifo + context->fifo_read;
			cur->cycle = context->cycles;
			cur->address = context->address;
			cur->partial = 2;
		}
		break;
	}

	if (cur) {
		context->regs[REG_DMASRC_L] += 1;
		if (!context->regs[REG_DMASRC_L]) {
			context->regs[REG_DMASRC_M] += 1;
		}
		context->address += context->regs[REG_AUTOINC];
		uint16_t dma_len = ((context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L]) - 1;
		context->regs[REG_DMALEN_H] = dma_len >> 8;
		context->regs[REG_DMALEN_L] = dma_len;
		if (!dma_len) {
			//printf("DMA end at cycle %d, frame: %d, vcounter: %d, hslot: %d\n", context->cycles, context->frame, context->vcounter, context->hslot);
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
			if (context->regs[REG_MODE_4] & BIT_H40) {
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
	//TODO: Further research on vscroll latch behavior and the "first column bug"
	if (!column) {
		if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
			if (context->regs[REG_MODE_4] & BIT_H40) {
				//Based on observed behavior documented by Eke-Eke, I'm guessing the VDP
				//ends up fetching the last value on the VSRAM bus in the H40 case
				//getting the last latched value should be close enough for now
				if (!vsram_off) {
					context->vscroll_latch[0] = context->vscroll_latch[1];
				}
			} else {
				//supposedly it's always forced to 0 in the H32 case
				context->vscroll_latch[0] = context->vscroll_latch[1] = 0;
			}
		} else {
			context->vscroll_latch[vsram_off] = context->vsram[vsram_off];
		}
	} else if (context->regs[REG_MODE_3] & BIT_VSCROLL) {
		context->vscroll_latch[vsram_off] = context->vsram[column - 2 + vsram_off];
	}
	vscroll &= context->vscroll_latch[vsram_off] + line;
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
	uint32_t *dst;
	uint8_t *sprite_buf,  *plane_a, *plane_b;
	int plane_a_off, plane_b_off;
	if (col)
	{
		col-=2;
		dst = context->framebuf;
		dst += line * 320 + col * 8;
		if (context->debug < 2) {
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
					plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
					plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
					uint8_t pixel = context->regs[REG_BG_COLOR];
					uint32_t *colors = context->colors;
					src = DBG_SRC_BG;
					if (*plane_b & 0xF) {
						pixel = *plane_b;
						src = DBG_SRC_B;
					}
					uint8_t intensity = *plane_b & BUF_BIT_PRIORITY;
					if (*plane_a & 0xF && (*plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						pixel = *plane_a;
						src = DBG_SRC_A;
					}
					intensity |= *plane_a & BUF_BIT_PRIORITY;
					if (*sprite_buf & 0xF && (*sprite_buf & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						if ((*sprite_buf & 0x3F) == 0x3E) {
							intensity += BUF_BIT_PRIORITY;
						} else if ((*sprite_buf & 0x3F) == 0x3F) {
							intensity = 0;
						} else {
							pixel = *sprite_buf;
							src = DBG_SRC_S;
							if ((pixel & 0xF) == 0xE) {
								intensity = BUF_BIT_PRIORITY;
							} else {
								intensity |= pixel & BUF_BIT_PRIORITY;
							}
						}
					}
					if (!intensity) {
						src |= DBG_SHADOW;
						colors += CRAM_SIZE;
					} else if (intensity ==  BUF_BIT_PRIORITY*2) {
						src |= DBG_HILIGHT;
						colors += CRAM_SIZE*2;
					}
					
					uint32_t outpixel;
					if (context->debug) {
						outpixel = context->debugcolors[src];
					} else {
						outpixel = colors[pixel & 0x3F];
					}
					*(dst++) = outpixel;
				}
			} else {
				for (int i = 0; i < 16; ++plane_a_off, ++plane_b_off, ++sprite_buf, ++i) {
					plane_a = context->tmp_buf_a + (plane_a_off & SCROLL_BUFFER_MASK);
					plane_b = context->tmp_buf_b + (plane_b_off & SCROLL_BUFFER_MASK);
					uint8_t pixel = context->regs[REG_BG_COLOR];
					src = DBG_SRC_BG;
					if (*plane_b & 0xF) {
						pixel = *plane_b;
						src = DBG_SRC_B;
					}
					if (*plane_a & 0xF && (*plane_a & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						pixel = *plane_a;
						src = DBG_SRC_A;
					}
					if (*sprite_buf & 0xF && (*sprite_buf & BUF_BIT_PRIORITY) >= (pixel & BUF_BIT_PRIORITY)) {
						pixel = *sprite_buf;
						src = DBG_SRC_S;
					}
					uint32_t outpixel;
					if (context->debug) {
						outpixel = context->debugcolors[src];
					} else {
						outpixel = context->colors[pixel & 0x3F];
					}
					*(dst++) = outpixel;
				}
			}
		} else if (context->debug == 2) {
			if (col < 32) {
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 1];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 2];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
				*(dst++) = context->colors[col * 2 + 3];
			} else if (col == 32 || line >= 192) {
				for (int32_t i = 0; i < 16; i ++) {
					*(dst++) = 0;
				}
			} else {
				for (int32_t i = 0; i < 16; i ++) {
					*(dst++) = context->colors[line / 3 + (col - 34) * 0x20];
				}
			}
		} else {
			uint32_t base = (context->debug - 3) * 0x200;
			uint32_t cell = base + (line / 8) * (context->regs[REG_MODE_4] & BIT_H40 ? 40 : 32) + col;
			uint32_t address = (cell * 32 + (line % 8) * 4) & 0xFFFF;
			for (int32_t i = 0; i < 4; i ++) {
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] >> 4)];
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] & 0xF)];
				address++;
			}
			cell++;
			address = (cell * 32 + (line % 8) * 4) & 0xFFFF;
			for (int32_t i = 0; i < 4; i ++) {
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] >> 4)];
				*(dst++) = context->colors[(context->debug_pal << 4) | (context->vdpmem[address] & 0xF)];
				address++;
			}
		}
	}
	context->buf_a_off = (context->buf_a_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
	context->buf_b_off = (context->buf_b_off + SCROLL_BUFFER_DRAW) & SCROLL_BUFFER_MASK;
}

uint32_t const h40_hsync_cycles[] = {19, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 18, 20, 20, 20, 19};

void vdp_advance_line(vdp_context *context)
{
	context->vcounter++;
	context->vcounter &= 0x1FF;
	if (context->flags2 & FLAG2_REGION_PAL) {
		if (context->latched_mode & BIT_PAL) {
			if (context->vcounter == 0x10B) {
				context->vcounter = 0x1D2;
			}
		} else if (context->vcounter == 0x103){
			context->vcounter = 0x1CA;
		}
	} else if (!(context->latched_mode & BIT_PAL) &&  context->vcounter == 0xEB) {
		context->vcounter = 0x1E5;
	}
	
	if (context->vcounter > (context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START)) {
		context->hint_counter = context->regs[REG_HINT];
	} else if (context->hint_counter) {
		context->hint_counter--;
	} else {
		context->flags2 |= FLAG2_HINT_PENDING;
		context->pending_hint_start = context->cycles;
		context->hint_counter = context->regs[REG_HINT];
	}
}

#define CHECK_ONLY if (context->cycles >= target_cycles) { return; }
#define CHECK_LIMIT if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); } context->hslot++; context->cycles += slot_cycles; CHECK_ONLY

#define COLUMN_RENDER_BLOCK(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+1):\
		external_slot(context);\
		CHECK_LIMIT\
	case (startcyc+2):\
		render_map_1(context);\
		CHECK_LIMIT\
	case (startcyc+3):\
		render_map_2(context);\
		CHECK_LIMIT\
	case (startcyc+4):\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+5):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+6):\
		render_map_3(context);\
		CHECK_LIMIT\
	case (startcyc+7):\
		render_map_output(context->vcounter, column, context);\
		CHECK_LIMIT

#define COLUMN_RENDER_BLOCK_REFRESH(column, startcyc) \
	case startcyc:\
		read_map_scroll_a(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+1):\
		/* refresh, no don't run dma src */\
		context->hslot++;\
		context->cycles += slot_cycles;\
		CHECK_ONLY\
	case (startcyc+2):\
		render_map_1(context);\
		CHECK_LIMIT\
	case (startcyc+3):\
		render_map_2(context);\
		CHECK_LIMIT\
	case (startcyc+4):\
		read_map_scroll_b(column, context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+5):\
		read_sprite_x(context->vcounter, context);\
		CHECK_LIMIT\
	case (startcyc+6):\
		render_map_3(context);\
		CHECK_LIMIT\
	case (startcyc+7):\
		render_map_output(context->vcounter, column, context);\
		if (column == 40 || (column == 32 && startcyc == 124)) {\
			vdp_advance_line(context);\
		}\
		CHECK_LIMIT
		
#define SPRITE_RENDER_H40(slot) \
	case slot:\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); } \
		if (slot == 182) {\
			context->hslot = 229;\
			context->cycles += slot_cycles;\
		} else {\
			context->hslot++;\
			if (slot >= HSYNC_SLOT_H40 && slot < HSYNC_END_H40) {\
				context->cycles += h40_hsync_cycles[slot - HSYNC_SLOT_H40];\
			} else {\
				context->cycles += slot_cycles;\
			}\
		}\
		CHECK_ONLY
		
#define SPRITE_RENDER_H32(slot) \
	case slot:\
		render_sprite_cells( context);\
		scan_sprite_table(context->vcounter, context);\
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); } \
		if (slot == 147) {\
			context->hslot = 233;\
		} else {\
			context->hslot++;\
		}\
		context->cycles += slot_cycles;\
		CHECK_ONLY
			

void vdp_h40(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H40;
	switch(context->hslot)
	{
	for (;;)
	{
	case 165:
		external_slot(context);
		CHECK_LIMIT
	case 166:
		external_slot(context);
		CHECK_LIMIT
	//sprite render to line buffer starts
	case 167:
		context->cur_slot = MAX_DRAWS-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 168:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 169:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 170:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
			if (context->vcounter == (context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START)) {
				context->hslot++;
				context->cycles += slot_cycles;
				return;
			}
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 171:
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE;
		render_sprite_cells( context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(172)
	SPRITE_RENDER_H40(173)
	SPRITE_RENDER_H40(174)
	SPRITE_RENDER_H40(175)
	SPRITE_RENDER_H40(176)
	SPRITE_RENDER_H40(177)
	SPRITE_RENDER_H40(178)
	SPRITE_RENDER_H40(179)
	SPRITE_RENDER_H40(180)
	SPRITE_RENDER_H40(181)
	SPRITE_RENDER_H40(182)
	SPRITE_RENDER_H40(229)
	SPRITE_RENDER_H40(230)
	SPRITE_RENDER_H40(231)
	SPRITE_RENDER_H40(232)
	SPRITE_RENDER_H40(233)
	//!HSYNC asserted
	SPRITE_RENDER_H40(234)
	SPRITE_RENDER_H40(235)
	SPRITE_RENDER_H40(236)
	SPRITE_RENDER_H40(237)
	SPRITE_RENDER_H40(238)
	SPRITE_RENDER_H40(239)
	SPRITE_RENDER_H40(240)
	SPRITE_RENDER_H40(241)
	SPRITE_RENDER_H40(242)
	SPRITE_RENDER_H40(243)
	SPRITE_RENDER_H40(244)
	SPRITE_RENDER_H40(245)
	SPRITE_RENDER_H40(246)
	SPRITE_RENDER_H40(247)
	case 248:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); }
		context->hslot++;
		context->cycles += h40_hsync_cycles[14];
		CHECK_ONLY
	//!HSYNC high
	SPRITE_RENDER_H40(249)
	SPRITE_RENDER_H40(250)
	SPRITE_RENDER_H40(251)
	SPRITE_RENDER_H40(252)
	case 253:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(254)
	case 255:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); }
		context->hslot = 0;
		context->cycles += slot_cycles;
		CHECK_ONLY
	case 0:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 1:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H40(2)
	case 3:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 4:
		if (context->vcounter == (context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START)) {
			context->flags2 |= FLAG2_VINT_PENDING;
			context->pending_vint_start = context->cycles;
		}
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE-1;
		context->sprite_draws = MAX_DRAWS;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 5)
	COLUMN_RENDER_BLOCK(4, 13)
	COLUMN_RENDER_BLOCK(6, 21)
	COLUMN_RENDER_BLOCK_REFRESH(8, 29)
	COLUMN_RENDER_BLOCK(10, 37)
	COLUMN_RENDER_BLOCK(12, 45)
	COLUMN_RENDER_BLOCK(14, 53)
	COLUMN_RENDER_BLOCK_REFRESH(16, 61)
	COLUMN_RENDER_BLOCK(18, 69)
	COLUMN_RENDER_BLOCK(20, 77)
	COLUMN_RENDER_BLOCK(22, 85)
	COLUMN_RENDER_BLOCK_REFRESH(24, 93)
	COLUMN_RENDER_BLOCK(26, 101)
	COLUMN_RENDER_BLOCK(28, 109)
	COLUMN_RENDER_BLOCK(30, 117)
	COLUMN_RENDER_BLOCK_REFRESH(32, 125)
	COLUMN_RENDER_BLOCK(34, 133)
	COLUMN_RENDER_BLOCK(36, 141)
	COLUMN_RENDER_BLOCK(38, 149)
	COLUMN_RENDER_BLOCK_REFRESH(40, 157)
	}
	default:
		context->hslot++;
		context->cycles += slot_cycles;
		return;
	}
}

void vdp_h32(vdp_context * context, uint32_t target_cycles)
{
	uint16_t address;
	uint32_t mask;
	uint32_t const slot_cycles = MCLKS_SLOT_H32;
	switch(context->hslot)
	{
	for (;;)
	{
	case 132:
		external_slot(context);
		CHECK_LIMIT
	case 133:
		external_slot(context);
		CHECK_LIMIT
	//sprite render to line buffer starts
	case 134:
		context->cur_slot = MAX_DRAWS_H32-1;
		memset(context->linebuf, 0, LINEBUF_SIZE);
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 135:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 136:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
		}
		CHECK_LIMIT
	case 137:
		if (context->vcounter == 0x1FF) {
			external_slot(context);
		} else {
			render_sprite_cells(context);
			if (context->vcounter == (context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START)) {
				context->hslot++;
				context->cycles += slot_cycles;
				return;
			}
		}
		CHECK_LIMIT
	//sprite attribute table scan starts
	case 138:
		context->sprite_index = 0x80;
		context->slot_counter = MAX_SPRITES_LINE_H32;
		render_sprite_cells( context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(139)
	SPRITE_RENDER_H32(140)
	SPRITE_RENDER_H32(141)
	SPRITE_RENDER_H32(142)
	SPRITE_RENDER_H32(143)
	SPRITE_RENDER_H32(144)
	SPRITE_RENDER_H32(145)
	SPRITE_RENDER_H32(146)
	SPRITE_RENDER_H32(147)
	case 233:
		external_slot(context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(234)
	SPRITE_RENDER_H32(235)
	SPRITE_RENDER_H32(236)
	SPRITE_RENDER_H32(237)
	SPRITE_RENDER_H32(238)
	//HSYNC start
	SPRITE_RENDER_H32(239)
	SPRITE_RENDER_H32(240)
	SPRITE_RENDER_H32(241)
	SPRITE_RENDER_H32(242)
	SPRITE_RENDER_H32(243)
	SPRITE_RENDER_H32(244)
	SPRITE_RENDER_H32(245)
	case 246:
		external_slot(context);
		CHECK_LIMIT
	case 247:
		address = (context->regs[REG_HSCROLL] & 0x3F) << 10;
		mask = 0;
		if (context->regs[REG_MODE_3] & 0x2) {
			mask |= 0xF8;
		}
		if (context->regs[REG_MODE_3] & 0x1) {
			mask |= 0x7;
		}
		address += (context->vcounter & mask) * 4;
		context->hscroll_a = context->vdpmem[address] << 8 | context->vdpmem[address+1];
		context->hscroll_b = context->vdpmem[address+2] << 8 | context->vdpmem[address+3];
		//printf("%d: HScroll A: %d, HScroll B: %d\n", context->vcounter, context->hscroll_a, context->hscroll_b);
		CHECK_LIMIT
	SPRITE_RENDER_H32(248)
	SPRITE_RENDER_H32(249)
	SPRITE_RENDER_H32(250)
	SPRITE_RENDER_H32(251)
	//!HSYNC high
	case 252:
		read_map_scroll_a(0, context->vcounter, context);
		CHECK_LIMIT
	SPRITE_RENDER_H32(253)
	case 254:
		render_map_1(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 255:
		render_map_2(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		if (context->flags & FLAG_DMA_RUN) { run_dma_src(context, context->hslot); }
		context->cycles += slot_cycles;
		context->hslot = 0;
		CHECK_ONLY
	case 0:
		read_map_scroll_b(0, context->vcounter, context);
		CHECK_LIMIT
	case 1:
		render_sprite_cells(context);
		scan_sprite_table(context->vcounter, context);
		CHECK_LIMIT
	case 2:
		render_map_3(context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		CHECK_LIMIT
	case 3:
		render_map_output(context->vcounter, 0, context);
		scan_sprite_table(context->vcounter, context);//Just a guess
		//reverse context slot counter so it counts the number of sprite slots
		//filled rather than the number of available slots
		//context->slot_counter = MAX_SPRITES_LINE - context->slot_counter;
		context->cur_slot = MAX_SPRITES_LINE_H32-1;
		context->sprite_draws = MAX_DRAWS_H32;
		context->flags &= (~FLAG_CAN_MASK & ~FLAG_MASKED);
		CHECK_LIMIT
	COLUMN_RENDER_BLOCK(2, 4)
	COLUMN_RENDER_BLOCK(4, 12)
	COLUMN_RENDER_BLOCK(6, 20)
	COLUMN_RENDER_BLOCK_REFRESH(8, 28)
	COLUMN_RENDER_BLOCK(10, 36)
	COLUMN_RENDER_BLOCK(12, 44)
	COLUMN_RENDER_BLOCK(14, 52)
	COLUMN_RENDER_BLOCK_REFRESH(16, 60)
	COLUMN_RENDER_BLOCK(18, 68)
	COLUMN_RENDER_BLOCK(20, 76)
	COLUMN_RENDER_BLOCK(22, 84)
	COLUMN_RENDER_BLOCK_REFRESH(24, 92)
	COLUMN_RENDER_BLOCK(26, 100)
	COLUMN_RENDER_BLOCK(28, 108)
	COLUMN_RENDER_BLOCK(30, 116)
	COLUMN_RENDER_BLOCK_REFRESH(32, 124)
	}
	default:
		context->hslot++;
		context->cycles += MCLKS_SLOT_H32;
	}
}

void latch_mode(vdp_context * context)
{
	context->latched_mode = context->regs[REG_MODE_2] & BIT_PAL;
}

void check_render_bg(vdp_context * context, int32_t line, uint32_t slot)
{
	int starti = -1;
	if (context->regs[REG_MODE_4] & BIT_H40) {
		if (slot >= 12 && slot < 172) {
			uint32_t x = (slot-12)*2;
			starti = line * 320 + x;
		}
	} else {
		if (slot >= 11 && slot < 139) {
			uint32_t x = (slot-11)*2;
			starti = line * 320 + x;
		}
	}
	if (starti >= 0) {
		uint32_t color = context->colors[context->regs[REG_BG_COLOR]];
		uint32_t * start = context->framebuf;
		start += starti;
		for (int i = 0; i < 2; i++) {
			*(start++) = color;
		}
	}
}


void vdp_run_context(vdp_context * context, uint32_t target_cycles)
{
	while(context->cycles < target_cycles)
	{
		uint32_t inactive_start = context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START;
		//line 0x1FF is basically active even though it's not displayed
		uint8_t active_slot = context->vcounter < inactive_start || context->vcounter == 0x1FF;
		uint8_t is_h40 = context->regs[REG_MODE_4] & BIT_H40;
		if (context->vcounter == inactive_start) {
			if (is_h40) {
				//the first inactive line behaves as an active one for the first 4 slots
				if (context->hslot > LINE_CHANGE_H40 && context->hslot < 171) {
					active_slot = 1;
				}
			} else {
				//the first inactive line behaves as an active one for the first few slots
				if (context->hslot > LINE_CHANGE_H32 && context->hslot < 138) {
					active_slot = 1;
				}
			}
		}
		if (context->regs[REG_MODE_2] & DISPLAY_ENABLE && active_slot) {
			if (is_h40) {
				vdp_h40(context, target_cycles);
			} else {
				vdp_h32(context, target_cycles);
			}
		} else {
			if (is_h40) {
				if (context->hslot == 167) {
					context->cur_slot = MAX_DRAWS-1;
					memset(context->linebuf, 0, LINEBUF_SIZE);
				} else if (context->hslot == 171) {
					context->sprite_index = 0x80;
					context->slot_counter = MAX_SPRITES_LINE;
				}
			} else {
				if (context->hslot == 134) {
					context->cur_slot = MAX_DRAWS_H32-1;
					memset(context->linebuf, 0, LINEBUF_SIZE);
				} else if (context->hslot == 138) {
					context->sprite_index = 0x80;
					context->slot_counter = MAX_SPRITES_LINE_H32;
				}
			}
			if(context->vcounter == inactive_start) {
				uint32_t intslot = context->regs[REG_MODE_4] & BIT_H40 ? VINT_SLOT_H40 :  VINT_SLOT_H32;
				if (context->hslot == intslot) {
					context->flags2 |= FLAG2_VINT_PENDING;
					context->pending_vint_start = context->cycles;
				}
			}
			uint32_t inccycles;
			if (is_h40) {
				if (context->hslot < HSYNC_SLOT_H40 || context->hslot >= HSYNC_END_H40) {
					inccycles = MCLKS_SLOT_H40;
				} else {
					inccycles = h40_hsync_cycles[context->hslot-HSYNC_SLOT_H40];
				}
			} else {
				inccycles = MCLKS_SLOT_H32;
			}
			if (!is_refresh(context, context->hslot)) {
				external_slot(context);
			}
			if (context->vcounter < inactive_start) {
				check_render_bg(context, context->vcounter, context->hslot);
			}
			if (context->flags & FLAG_DMA_RUN && !is_refresh(context, context->hslot)) {
				run_dma_src(context, context->hslot);
			}
			context->cycles += inccycles;
			context->hslot++;
			context->hslot &= 0xFF;
			if (is_h40) {
				if (context->hslot == LINE_CHANGE_H40) {
					vdp_advance_line(context);
					if (context->vcounter == (inactive_start + 8)) {
						context->frame++;
					}
				} else if (context->hslot == 183) {
					context->hslot = 229;
				}
			} else {
				if (context->hslot == LINE_CHANGE_H32) {
					vdp_advance_line(context);
					if (context->vcounter == (inactive_start + 8)) {
						context->frame++;
					}
				} else if (context->hslot == 148) {
					context->hslot = 233;
				}
			}
		}
	}
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t target_cycles = ((context->latched_mode & BIT_PAL) ? PAL_INACTIVE_START : NTSC_INACTIVE_START) * MCLKS_LINE;
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
		uint32_t min_dma_complete = dmalen * (context->regs[REG_MODE_4] & BIT_H40 ? 16 : 20);
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
				//printf("DMA start (length: %X) at cycle %d, frame: %d, vcounter: %d, hslot: %d\n", (context->regs[REG_DMALEN_H] << 8) | context->regs[REG_DMALEN_L], context->cycles, context->frame, context->vcounter, context->hslot);
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
				if (reg == REG_MODE_1 && (value & BIT_HVC_LATCH) && !(context->regs[reg] & BIT_HVC_LATCH)) {
					context->hv_latch = vdp_hv_counter_read(context);
				}
				if (reg == REG_BG_COLOR) {
					value &= 0x3F;
				}
				/*if (reg == REG_MODE_4 && ((value ^ context->regs[reg]) & BIT_H40)) {
					printf("Mode changed from H%d to H%d @ %d, frame: %d\n", context->regs[reg] & BIT_H40 ? 40 : 32, value & BIT_H40 ? 40 : 32, context->cycles, context->frame);
				}*/
				context->regs[reg] = value;
				if (reg == REG_MODE_4) {
					context->double_res = (value & (BIT_INTERLACE | BIT_DOUBLE_RES)) == (BIT_INTERLACE | BIT_DOUBLE_RES);
					if (!context->double_res) {
						context->framebuf = context->oddbuf;
					}
					}
				context->cd &= 0x3C;
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
		vdp_run_context(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	fifo_entry * cur = context->fifo + context->fifo_write;
	cur->cycle = context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20)*FIFO_LATENCY;
	cur->address = context->address;
	cur->value = value;
	if (context->cd & 0x20 && (context->regs[REG_DMASRC_H] & 0xC0) == 0x80 && (context->regs[REG_MODE_2] & BIT_DMA_ENABLE)) {
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
	if (context->flags & FLAG_DOT_OFLOW) {
		value |= 0x40;
	}
	if (context->flags2 & FLAG2_SPRITE_COLLIDE) {
		value |= 0x20;
		//TODO: Test when this is actually cleared
		context->flags2 &= ~FLAG2_SPRITE_COLLIDE;
	}
	if ((context->regs[REG_MODE_4] & BIT_INTERLACE) && context->framebuf == context->oddbuf) {
		value |= 0x10;
	}
	uint32_t line= context->vcounter;
	uint32_t slot = context->hslot;
	uint32_t inactive_start = (context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START);
	if (
		(
			line > inactive_start
			&& line < 0x1FF
		)
		|| (line == inactive_start 
			&& (
				slot >= (context->regs[REG_MODE_4] & BIT_H40 ? VBLANK_START_H40 : VBLANK_START_H32)
				|| slot < (context->regs[REG_MODE_4] & BIT_H40 ? LINE_CHANGE_H40 : LINE_CHANGE_H32)
			)
		)
		|| (line == 0x1FF
			&& slot < (context->regs[REG_MODE_4] & BIT_H40 ? VBLANK_START_H40 : VBLANK_START_H32))
			&& slot >= (context->regs[REG_MODE_4] & BIT_H40 ? LINE_CHANGE_H40 : LINE_CHANGE_H32)
		|| !(context->regs[REG_MODE_2] & BIT_DISP_EN)
	) {
		value |= 0x8;
	}
	if (context->regs[REG_MODE_4] & BIT_H40) {
		if (slot < HBLANK_END_H40 || slot > HBLANK_START_H40) {
			value |= 0x4;
		}
	} else {
		if (slot < HBLANK_END_H32 || slot > HBLANK_START_H32) {
			value |= 0x4;
		}
	}
	if (context->flags & FLAG_DMA_RUN) {
		value |= 0x2;
	}
	if (context->flags2 & FLAG2_REGION_PAL) {
		value |= 0x1;
	}
	//printf("status read at cycle %d returned %X\n", context->cycles, value);
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
		vdp_run_context(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
	}
	uint16_t value = 0;
	switch (context->cd & 0xF)
	{
	case VRAM_READ:
		value = context->vdpmem[context->address & 0xFFFE] << 8;
		context->flags &= ~FLAG_UNUSED_SLOT;
		context->flags2 |= FLAG2_READ_PENDING;
		while (!(context->flags & FLAG_UNUSED_SLOT)) {
			vdp_run_context(context, context->cycles + ((context->regs[REG_MODE_4] & BIT_H40) ? 16 : 20));
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
	case VSRAM_READ: {
		uint16_t address = (context->address /2) & 63;
		if (address >= VSRAM_SIZE) {
			address = 0;
		}
		value = context->vsram[address] & VSRAM_BITS;
		value |= context->fifo[context->fifo_write].value & VSRAM_DIRTY_BITS;
		break;
		}
	}
	context->address += context->regs[REG_AUTOINC];
	return value;
}

uint16_t vdp_hv_counter_read(vdp_context * context)
{
	if (context->regs[REG_MODE_1] & BIT_HVC_LATCH) {
		return context->hv_latch;
	}
	uint32_t line= context->vcounter & 0xFF;
	uint32_t linecyc = context->hslot;
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
	if (context->pending_vint_start >= deduction) {
		context->pending_vint_start -= deduction;
	} else {
		context->pending_vint_start = 0;
	}
	if (context->pending_hint_start >= deduction) {
		context->pending_hint_start -= deduction;
	} else {
		context->pending_hint_start = 0;
	}
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

uint32_t vdp_cycles_hslot_wrap_h40(vdp_context * context)
{
	if (context->hslot < 183) {
		return MCLKS_LINE - context->hslot * MCLKS_SLOT_H40;
	} else if (context->hslot < HSYNC_END_H40) {
		uint32_t before_hsync = context->hslot < HSYNC_SLOT_H40 ? (HSYNC_SLOT_H40 - context->hslot) * MCLKS_SLOT_H40 : 0;
		uint32_t hsync = 0;
		for (int i = context->hslot <= HSYNC_SLOT_H40 ? 0 : context->hslot - HSYNC_SLOT_H40; i < sizeof(h40_hsync_cycles)/sizeof(uint32_t); i++)
		{
			hsync += h40_hsync_cycles[i];
		}
		uint32_t after_hsync = (256- HSYNC_END_H40) * MCLKS_SLOT_H40;
		return before_hsync + hsync + after_hsync;
	} else {
		return (256-context->hslot) * MCLKS_SLOT_H40;
	}
}

uint32_t vdp_cycles_next_line(vdp_context * context)
{
	if (context->regs[REG_MODE_4] & BIT_H40) {
		if (context->hslot < LINE_CHANGE_H40) {
			return (LINE_CHANGE_H40 - context->hslot) * MCLKS_SLOT_H40;
		} else {
			return vdp_cycles_hslot_wrap_h40(context) + LINE_CHANGE_H40 * MCLKS_SLOT_H40;
		}
	} else {
		if (context->hslot < LINE_CHANGE_H32) {
			return (LINE_CHANGE_H32 - context->hslot) * MCLKS_SLOT_H32;
		} else if (context->hslot < 148) {
			return MCLKS_LINE - (context->hslot - LINE_CHANGE_H32) * MCLKS_SLOT_H32;
		} else {
			return (256-context->hslot + LINE_CHANGE_H32) * MCLKS_SLOT_H32;
		}
	}
}

uint32_t vdp_cycles_to_line(vdp_context * context, uint32_t target)
{
	uint32_t jump_start, jump_dst;
	if (context->flags2 & FLAG2_REGION_PAL) {
		if (context->latched_mode & BIT_PAL) {
			jump_start = 0x10B;
			jump_dst = 0x1D2;
		} else {
			jump_start = 0x103;
			jump_dst = 0x1CA;
		}
	} else {
		if (context->latched_mode & BIT_PAL) {
			jump_start = 0;
			jump_dst = 0;
		} else {
			jump_start = 0xEB;
			jump_dst = 0x1E5;
		}
	}
	uint32_t lines;
	if (context->vcounter < target) {
		if (target < jump_start) {
			lines = target - context->vcounter;
		} else {
			lines = jump_start - context->vcounter + target - jump_dst;
		}
	} else {
		if (context->vcounter < jump_start) {
			lines = jump_start - context->vcounter + 512 - jump_dst;
		} else {
			lines = 512 - context->vcounter;
		}
		if (target < jump_start) {
			lines += target;
		} else {
			lines += jump_start + target - jump_dst;
		}
	}
	return MCLKS_LINE * (lines - 1) + vdp_cycles_next_line(context);
}

uint32_t vdp_frame_end_line(vdp_context * context)
{
	uint32_t frame_end;
	if (context->flags2 & FLAG2_REGION_PAL) {
		if (context->latched_mode & BIT_PAL) {
			frame_end = PAL_INACTIVE_START + 8;
		} else {
			frame_end = NTSC_INACTIVE_START + 8;
		}
	} else {
		if (context->latched_mode & BIT_PAL) {
			frame_end = 512;
		} else {
			frame_end = NTSC_INACTIVE_START + 8;
		}
	}
	return frame_end;
}

uint32_t vdp_cycles_to_frame_end(vdp_context * context)
{
	return context->cycles + vdp_cycles_to_line(context, vdp_frame_end_line(context));
}

uint32_t vdp_next_hint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_1] & BIT_HINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_HINT_PENDING) {
		return context->pending_hint_start;
	}
	uint32_t inactive_start = context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START;
	uint32_t hint_line;
	if (context->vcounter + context->hint_counter >= inactive_start) {
		if (context->regs[REG_HINT] > inactive_start) {
			return 0xFFFFFFFF;
		}
		hint_line = context->regs[REG_HINT];
	} else {
		hint_line = context->vcounter + context->hint_counter + 1;
	}

	return context->cycles + vdp_cycles_to_line(context, hint_line);
}

uint32_t vdp_next_vint(vdp_context * context)
{
	if (!(context->regs[REG_MODE_2] & BIT_VINT_EN)) {
		return 0xFFFFFFFF;
	}
	if (context->flags2 & FLAG2_VINT_PENDING) {
		return context->pending_vint_start;
	}


	return vdp_next_vint_z80(context);
}

uint32_t vdp_next_vint_z80(vdp_context * context)
{
	uint32_t inactive_start = context->latched_mode & BIT_PAL ? PAL_INACTIVE_START : NTSC_INACTIVE_START;
	if (context->vcounter == inactive_start) {
		if (context->regs[REG_MODE_4] & BIT_H40) {
			if (context->hslot >= LINE_CHANGE_H40) {
				return context->cycles + vdp_cycles_hslot_wrap_h40(context) + VINT_SLOT_H40 * MCLKS_SLOT_H40;
			} else if (context->hslot <= VINT_SLOT_H40) {
				return context->cycles + (VINT_SLOT_H40 - context->hslot) * MCLKS_SLOT_H40;
			}
		} else {
			if (context->hslot >= LINE_CHANGE_H32) {
				if (context->hslot < 148) {
					return context->cycles + (VINT_SLOT_H32 + 148 - context->hslot + 256 - 233) * MCLKS_SLOT_H32;
				} else {
					return context->cycles + (VINT_SLOT_H32 + 256 - context->hslot) * MCLKS_SLOT_H32;
				}
			} else if (context->hslot <= VINT_SLOT_H32) {
				return context->cycles + (VINT_SLOT_H32 - context->hslot) * MCLKS_SLOT_H32;
			}
		}
	}
	int32_t cycles_to_vint = vdp_cycles_to_line(context, inactive_start);
	if (context->regs[REG_MODE_4] & BIT_H40) {
		cycles_to_vint += MCLKS_LINE - (LINE_CHANGE_H40 - VINT_SLOT_H40) * MCLKS_SLOT_H40;
	} else {
		cycles_to_vint += (VINT_SLOT_H32 + 148 - LINE_CHANGE_H32 + 256 - 233) * MCLKS_SLOT_H32;
	}
	return context->cycles + cycles_to_vint;
}

void vdp_int_ack(vdp_context * context, uint16_t int_num)
{
	if (int_num == 6) {
		context->flags2 &= ~FLAG2_VINT_PENDING;
	} else if(int_num ==4) {
		context->flags2 &= ~FLAG2_HINT_PENDING;
	}
}

