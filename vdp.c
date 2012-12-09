#include "vdp.h"
#include <stdlib.h>
#include <string.h>

#define MCLKS_LINE 3420
#define NTSC_ACTIVE 225
#define PAL_ACTIVE 241
#define BUF_BIT_PRIORITY 0x40
#define MAP_BIT_PRIORITY 0x8000
#define MAP_BIT_H_FLIP 0x800
#define MAP_BIT_V_FLIP 0x1000

#define BIT_PAL 0x8
#define BIT_H40 0x1

void init_vdp_context(vdp_context * context)
{
	memset(context, 0, sizeof(context));
	context->vdpmem = malloc(VRAM_SIZE);
	context->framebuf = malloc(FRAMEBUF_SIZE);
	context->linebuf = malloc(LINEBUF_SIZE + 48);
	context->tmp_buf_a = context->linebuf + LINEBUF_SIZE;
	context->tmp_buf_b = context->tmp_buf_a + 24;
	context->sprite_draws = MAX_DRAWS;
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
		for (uint16_t address = d->address; address < d->address+4; address++) {
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

void scan_sprite_table(uint32_t line, vdp_context * context)
{
	if (context->sprite_index && context->slot_counter) {
		line += 1;
		line &= 0xFF;
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
		int16_t y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) - 128;
		uint8_t height = ((context->vdpmem[address+2] & 0x3) + 1) * 8;
		//printf("Sprite %d | y: %d, height: %d\n", context->sprite_index, y, height);
		if (y <= line && line < (y + height)) {
			//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
			context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
			context->sprite_info_list[context->slot_counter].index = context->sprite_index;
			context->sprite_info_list[context->slot_counter].y = y;
		}
		context->sprite_index = context->vdpmem[address+3] & 0x7F;
		if (context->sprite_index && context->slot_counter)
		{
			address = context->sprite_index * 8 + sat_address;
			y = ((context->vdpmem[address] & 0x3) << 8 | context->vdpmem[address+1]) - 128;
			height = ((context->vdpmem[address+2] & 0x3) + 1) * 8;
			if (y <= line && line < (y + height)) {
				//printf("Sprite %d at y: %d with height %d is on line %d\n", context->sprite_index, y, height, line);
				context->sprite_info_list[--(context->slot_counter)].size = context->vdpmem[address+2];
				context->sprite_info_list[context->slot_counter].index = context->sprite_index;
				context->sprite_info_list[context->slot_counter].y = y;
			}
			context->sprite_index = context->vdpmem[address+3] & 0x7F;
		}
	}
}

#define FLAG_DOT_OFLOW 0x1
#define FLAG_CAN_MASK  0x2
#define FLAG_MASKED    0x4
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
			uint16_t att_addr = ((context->regs[REG_SAT] & 0x7F) << 9) + context->sprite_info_list[context->cur_slot].index * 8 + 4;
			uint16_t tileinfo = (context->vdpmem[att_addr] << 8) | context->vdpmem[att_addr+1];		
			uint8_t pal_priority = (tileinfo >> 9) & 0x70;
			uint8_t row;
			if (tileinfo & MAP_BIT_V_FLIP) {
				row = (context->sprite_info_list[context->cur_slot].y + height - 1) - line;
			} else {
				row = line-context->sprite_info_list[context->cur_slot].y;
			}
			uint16_t address = ((tileinfo & 0x7FF) << 5) + row * 4;
			int16_t x = ((context->vdpmem[att_addr+ 2] & 0x3) << 8) | context->vdpmem[att_addr + 3];
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

void external_slot(vdp_context * context)
{
	//TODO: Implement me
}

void read_map_scroll(uint16_t column, uint16_t vsram_off, uint32_t line, uint16_t address, uint16_t hscroll_val, vdp_context * context)
{
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
	vscroll &= (context->vsram[(context->regs[REG_MODE_3] & 0x4 ? column : 0) + vsram_off] + line);
	context->v_offset = vscroll & 0x7;
	//printf("%s | line %d, vsram: %d, vscroll: %d, v_offset: %d\n",(vsram_off ? "B" : "A"), line, context->vsram[context->regs[REG_MODE_3] & 0x4 ? column : 0], vscroll, context->v_offset);
	vscroll /= 8;
	uint16_t hscroll_mask;
	uint16_t v_mul;
	switch(context->regs[REG_SCROLL] & 0x3)
	{
	case 0:
		hscroll_mask = 0xF8;
		v_mul = 64;
		break;
	case 0x1:
		hscroll_mask = 0x1F8;
		v_mul = 128;
		break;
	case 0x2:
		//TODO: Verify this behavior
		hscroll_mask = 0;
		v_mul = 0;
		break;
	case 0x3:
		hscroll_mask = 0x3F8;
		v_mul = 256;
		break;
	}
	uint16_t hscroll, offset;
	for (int i = 0; i < 2; i++) {
		hscroll = ((column - 2 + i) * 8 - hscroll_val) & hscroll_mask;
		offset = address + ((vscroll * v_mul + hscroll/4) & 0x1FFF);
		//printf("%s | line: %d, col: %d, x: %d, hs_mask %X, v_mul: %d, scr reg: %X, tbl addr: %X\n", (vsram_off ? "B" : "A"), line, (column-(2-i)), hscroll, hscroll_mask, v_mul, context->regs[REG_SCROLL], offset);
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

void render_map(uint16_t col, uint8_t * tmp_buf, vdp_context * context)
{
	uint16_t address = ((col & 0x3FF) << 5);
	if (col & MAP_BIT_V_FLIP) {
		address +=  24 - 4 * context->v_offset;
	} else {
		address += 4 * context->v_offset;
	}
	uint16_t pal_priority = (col >> 9) & 0x70;
	int32_t dir;
	if (col & MAP_BIT_H_FLIP) {
		tmp_buf += 7;
		dir = -1;
	} else {
		dir = 1;
	}
	for (uint32_t i=0; i < 4; i++, address++)
	{
		*tmp_buf = pal_priority | (context->vdpmem[address] >> 4);
		tmp_buf += dir;
		*tmp_buf = pal_priority | (context->vdpmem[address] & 0xF);
		tmp_buf += dir;
	}
}

void render_map_1(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_a+8, context);
}

void render_map_2(vdp_context * context)
{
	render_map(context->col_2, context->tmp_buf_a+16, context);
}

void render_map_3(vdp_context * context)
{
	render_map(context->col_1, context->tmp_buf_b+8, context);
}

void render_map_output(uint32_t line, int32_t col, vdp_context * context)
{
	if (line >= 240) {
		return;
	}
	render_map(context->col_2, context->tmp_buf_b+16, context);
	uint16_t *dst, *end;
	uint8_t *sprite_buf, *plane_a, *plane_b;
	if (col)
	{
		col-=2;
		dst = context->framebuf + line * 320 + col * 8;
		sprite_buf = context->linebuf + col * 8;
		plane_a = context->tmp_buf_a + 8 - (context->hscroll_a & 0x7);
		plane_b = context->tmp_buf_b + 8 - (context->hscroll_b & 0x7);
		end = dst + 16;
		//printf("A | tmp_buf offset: %d\n", 8 - (context->hscroll_a & 0x7));
		for (; dst < end; ++plane_a, ++plane_b, ++sprite_buf, ++dst) {
			uint8_t pixel;
			if (*sprite_buf & BUF_BIT_PRIORITY && *sprite_buf & 0xF) {
				pixel = *sprite_buf;
			} else if (*plane_a & BUF_BIT_PRIORITY && *plane_a & 0xF) {
				pixel = *plane_a;
			} else if (*plane_b & BUF_BIT_PRIORITY && *plane_b & 0xF) {
				pixel = *plane_b;
			} else if (*sprite_buf & 0xF) {
				pixel = *sprite_buf;
			} else if (*plane_a & 0xF) {
				pixel = *plane_a;
			} else if (*plane_b & 0xF){
				pixel = *plane_b;
			} else {
				pixel = context->regs[REG_BG_COLOR] & 0x3F;
			}
			*dst = context->cram[pixel & 0x3F] | ((pixel & BUF_BIT_PRIORITY) ? 0x1000 : 0);
		}
	} else {
		//dst = context->framebuf + line * 320;
		//sprite_buf = context->linebuf + col * 8;
		//plane_a = context->tmp_buf_a + 16 - (context->hscroll_a & 0x7);
		//plane_b = context->tmp_buf_b + 16 - (context->hscroll_b & 0x7);
		//end = dst + 8;
	}
	
	uint16_t remaining = context->hscroll_a & 0x7;
	memcpy(context->tmp_buf_a + 8 - remaining, context->tmp_buf_a + 24 - remaining, remaining);
	remaining = context->hscroll_b & 0x7;
	memcpy(context->tmp_buf_b + 8 - remaining, context->tmp_buf_b + 24 - remaining, remaining);
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
		render_sprite_cells(context);
		break;
	case 1:
	case 2:
	case 3:
		render_sprite_cells(context);
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
		render_sprite_cells(context);
		break;
	case 1:
	case 2:
	case 3:
		render_sprite_cells(context);
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

void vdp_run_context(vdp_context * context, uint32_t target_cycles)
{
	while(context->cycles < target_cycles)
	{
		uint32_t line = context->cycles / MCLKS_LINE;
		uint32_t active_lines = context->latched_mode & BIT_PAL ? PAL_ACTIVE : NTSC_ACTIVE;
		if (line < active_lines) {
			if (!line) {
				latch_mode(context);
			}
			//first sort-of active line is treated as 255 internally
			//it's used for gathering sprite info for line 
			line = (line - 1) & 0xFF;
			uint32_t linecyc = context->cycles % MCLKS_LINE;
			
			//Convert to slot number
			if (context->latched_mode & BIT_H40){
				//TODO: Deal with nasty clock switching during HBLANK
				linecyc = linecyc/16;
				context->cycles += 16;
				vdp_h40(line, linecyc, context);
			} else {
				linecyc = linecyc/20;
				context->cycles += 20;
				vdp_h32(line, linecyc, context);
			}
		} else {
			//TODO: Empty FIFO
		}
	}
}

uint32_t vdp_run_to_vblank(vdp_context * context)
{
	uint32_t target_cycles = ((context->latched_mode & BIT_PAL) ? PAL_ACTIVE : NTSC_ACTIVE) * MCLKS_LINE;
	vdp_run_context(context, target_cycles);
	return context->cycles;
}

#define GST_VDP_REGS 0xFA
#define GST_VDP_MEM 0x12478

void vdp_load_savestate(vdp_context * context, FILE * state_file)
{
	uint8_t tmp_buf[CRAM_SIZE*2];
	fseek(state_file, GST_VDP_REGS, SEEK_SET);
	fread(context->regs, 1, VDP_REGS, state_file);
	latch_mode(context);
	fread(tmp_buf, 1, sizeof(tmp_buf), state_file);
	for (int i = 0; i < CRAM_SIZE; i++) {
		context->cram[i] = (tmp_buf[i*2+1] << 8) | tmp_buf[i*2];
	}
	fread(tmp_buf, 2, VSRAM_SIZE, state_file);
	for (int i = 0; i < VSRAM_SIZE; i++) {
		context->vsram[i] = (tmp_buf[i*2+1] << 8) | tmp_buf[i*2];
	}
	fseek(state_file, GST_VDP_MEM, SEEK_SET);
	fread(context->vdpmem, 1, VRAM_SIZE, state_file);
}
