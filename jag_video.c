#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "jag_video.h"
#include "render.h"

enum {
	VMODE_CRY,
	VMODE_RGB24,
	VMODE_DIRECT16,
	VMODE_RGB16,
	VMODE_VARIABLE
};

#define BIT_TBGEN 1

char *vmode_names[] = {
	"CRY",
	"RGB24",
	"RGB16",
	"DIRECT16",
	"VARIABLE"
};

static uint8_t cry_red[9][16] = {
	{0, 34, 68, 102, 135, 169, 203, 237, 255, 255, 255, 255, 255, 255, 255, 255},
	{0, 34, 68, 102, 135, 169, 203, 230, 247, 255, 255, 255, 255, 255, 255, 255},
	{0, 34, 68, 102, 135, 170, 183, 197, 214, 235, 255, 255, 255, 255, 255, 255},
	{0, 34, 68, 102, 130, 141, 153, 164, 181, 204, 227, 249, 255, 255, 255, 255},
	{0, 34, 68, 95,  104, 113, 122, 131, 148, 173, 198, 223, 248, 255, 255, 255},
	{0, 34, 64, 71,  78,  85,  91,  98,  115, 143, 170, 197, 224, 252, 255, 255},
	{0, 34, 43, 47,  52,  56,  61,  65,  82,  112, 141, 171, 200, 230, 255, 255},
	{0, 19, 21, 23,  26,  28,  30,  32,  49,  81,  113, 145, 177, 208, 240, 255},
	{0, 0,  0,  0,   0,   0,   0,   0,   17,  51,  85,  119, 153, 187, 221, 255}
};

static uint8_t cry_green[16][8] = {
	{0,   0,   0,   0,   0,   0,   0,   0},
	{17,  19,  21,  23,  26,  28,  30,  32},
	{34,  38,  43,  47,  52,  56,  61,  65},
	{51,  57,  64,  71,  78,  85,  91,  98},
	{68,  77,  86,  95,  104, 113, 122, 131},
	{85,  96,  107, 119, 130, 141, 153, 164},
	{102, 115, 129, 142, 156, 170, 183, 197},
	{119, 134, 150, 166, 182, 198, 214, 230},
	{136, 154, 172, 190, 208, 226, 244, 255},
	{153, 173, 193, 214, 234, 255, 255, 255},
	{170, 192, 215, 238, 255, 255, 255, 255},
	{187, 211, 236, 255, 255, 255, 255, 255},
	{204, 231, 255, 255, 255, 255, 255, 255},
	{221, 250, 255, 255, 255, 255, 255, 255},
	{238, 255, 255, 255, 255, 255, 255, 255},
	{255, 255, 255, 255, 255, 255, 255, 255},
};

static uint32_t table_cry[0x10000];
static uint32_t table_rgb[0x10000];
static uint32_t table_variable[0x10000];

static uint32_t cry_to_rgb(uint16_t cry)
{
	uint32_t y = cry & 0xFF;
	if (y) {
		uint8_t c = cry >> 12;
		uint8_t r = cry >> 8 & 0xF;
		
		uint32_t red = cry_red[c < 7 ? 0 : c - 7][r];
		uint32_t green = cry_green[c][r < 8 ? r : 15 - r];
		uint32_t blue = cry_red[c < 7 ? 0 : c - 7][15-r];
		red = red * 255 / y;
		blue = blue * 255 / y;
		green = green * 255 / y;
		return render_map_color(red, green, blue);
	} else {
		return render_map_color(0, 0, 0);
	}
}

static uint32_t rgb16_to_rgb(uint16_t rgb)
{
	return render_map_color(
		rgb >> 8 & 0xF8,
		rgb << 2 & 0xFC,
		rgb >> 4 & 0xF8
	);
}

jag_video *jag_video_init(void)
{
	static uint8_t table_init_done = 0;
	if (!table_init_done) {
		for (int i = 0; i < 0x10000; i++)
		{
			table_cry[i] = cry_to_rgb(i);
			table_rgb[i] = rgb16_to_rgb(i);
			table_variable[i] = i & 1 ? rgb16_to_rgb(i & 0xFFFE) : cry_to_rgb(i);
		}
		table_init_done = 1;
	}
	return calloc(1, sizeof(jag_video));
}

static void copy_16(uint32_t *dst, uint32_t len, uint16_t *linebuffer, uint32_t *table)
{
	for (; len; len--, dst++, linebuffer++)
	{
		*dst = table[*linebuffer];
	}
}

static void copy_linebuffer(jag_video *context, uint16_t *linebuffer)
{
	if (!context->output) {
		return;
	}
	uint32_t *dst = context->output;
	uint32_t len;
	if (context->regs[VID_HCOUNT] == context->regs[VID_HDISP_BEGIN1]) {
		if (
			context->regs[VID_HDISP_BEGIN2] == context->regs[VID_HDISP_BEGIN1] 
			|| context->regs[VID_HDISP_BEGIN2] > (context->regs[VID_HPERIOD] | 0x400)
		) {
			//only one line buffer per line, so copy the previous line in its entirety
			len = context->regs[VID_HDISP_END] - 0x400 + context->regs[VID_HPERIOD] - context->regs[VID_HDISP_BEGIN1] + 2;
		} else {
			//copy the second half of the previous line
			if (context->regs[VID_HDISP_BEGIN2] & 0x400) {
				//BEGIN2 is after the HCOUNT jump
				dst += context->regs[VID_HPERIOD] - context->regs[VID_HDISP_BEGIN1] 
					+ context->regs[VID_HDISP_BEGIN2] - 0x400 + 1;
				len = context->regs[VID_HDISP_END] - context->regs[VID_HDISP_BEGIN2] + 1;
			} else {
				//BEGIN2 is before the HCOUNT jump
				dst += context->regs[VID_HDISP_BEGIN2] - context->regs[VID_HDISP_BEGIN1];
				len = context->regs[VID_HDISP_END] + context->regs[VID_HPERIOD] - context->regs[VID_HDISP_BEGIN2] + 2;
			}
		}
		context->output += context->output_pitch / sizeof(uint32_t);
	} else {
		//copy the first half of the current line
		if (context->regs[VID_HDISP_BEGIN2] & 0x400) {
			//BEGIN2 is after the HCOUNT jump
			len = context->regs[VID_HDISP_BEGIN2] - 0x400 + context->regs[VID_HPERIOD] - context->regs[VID_HDISP_BEGIN1] + 1;
		} else {
			//BEGIN2 is before the HCOUNT jump
			len = context->regs[VID_HDISP_BEGIN2] - context->regs[VID_HDISP_BEGIN1];
		}
	}
	len /= context->pclock_div;
	switch (context->mode)
	{
	case VMODE_CRY:
		copy_16(dst, len, linebuffer, table_cry);
		break;
	case VMODE_RGB24:
		//TODO: Implement me
		break;
	case VMODE_DIRECT16:
		//TODO: Implement this once I better understand what would happen on hardware with composite output
		break;
	case VMODE_RGB16:
		copy_16(dst, len, linebuffer, table_rgb);
		break;
	case VMODE_VARIABLE:
		copy_16(dst, len, linebuffer, table_variable);
		break;
	}
}

void jag_video_run(jag_video *context, uint32_t target_cycle)
{
	if (context->regs[VID_VMODE] & BIT_TBGEN) {
		while (context->cycles < target_cycle)
		{
			//TODO: Optimize this to not actually increment one step at a time
			if (
				(
					context->regs[VID_HCOUNT] == context->regs[VID_HDISP_BEGIN1]
					|| context->regs[VID_HCOUNT] == context->regs[VID_HDISP_BEGIN2]
				)
				&& context->regs[VID_VCOUNT] >= context->regs[VID_VDISP_BEGIN]
				&& context->regs[VID_VCOUNT] < context->regs[VID_VDISP_END]
			) {
				//swap linebuffers, render linebuffer to framebuffer and kick off object processor
				if (context->write_line_buffer == context->line_buffer_a) {
					context->write_line_buffer = context->line_buffer_b;
					copy_linebuffer(context, context->line_buffer_a);
				} else {
					context->write_line_buffer = context->line_buffer_a;
					copy_linebuffer(context, context->line_buffer_b);
				}
				//clear new write line buffer with background color
				for (int i = 0; i  < LINEBUFFER_WORDS; i++)
				{
					context->write_line_buffer[i] = context->regs[VID_BGCOLOR];
				}
				
				//TODO: kick off object processor
			}
			
			if (
				!context->output 
				&& context->regs[VID_VCOUNT] == context->regs[VID_VDISP_BEGIN]
				&& context->regs[VID_HCOUNT] == context->regs[VID_HDISP_BEGIN1]
			) {
				context->output = render_get_framebuffer(FRAMEBUFFER_ODD, &context->output_pitch);
			} else if (context->output && context->regs[VID_VCOUNT] >= context->regs[VID_VDISP_END]) {
				int width = (context->regs[VID_HPERIOD] - context->regs[VID_HDISP_BEGIN1] 
				+ context->regs[VID_HDISP_END] - 1024 + 2) / context->pclock_div;
				render_framebuffer_updated(FRAMEBUFFER_ODD, width);
				context->output = NULL;
			}
			
			if ((context->regs[VID_HCOUNT] & 0x3FF) == context->regs[VID_HPERIOD]) {
				//reset bottom 10 bits to zero, flip the 11th bit which represents which half of the line we're on
				context->regs[VID_HCOUNT] = (context->regs[VID_HCOUNT] & 0x400) ^ 0x400;
				//increment half-line counter
				if (context->regs[VID_VCOUNT] == context->regs[VID_VPERIOD]) {
					context->regs[VID_VCOUNT] = 0;
				} else {
					context->regs[VID_VCOUNT]++;
				}
			} else {
				context->regs[VID_HCOUNT]++;
			}
			context->cycles++;
		}
	} else {
		context->cycles = target_cycle;
	}
}

static uint8_t is_reg_writeable(uint32_t address)
{
	return address < VID_HLPEN || address >= VID_OBJLIST1;
}

void jag_video_reg_write(jag_video *context, uint32_t address, uint16_t value)
{
	uint32_t reg = (address >> 1 & 0x7F) - 2;
	if (reg < JAG_VIDEO_REGS && is_reg_writeable(reg)) {
		context->regs[reg] = value;
		if (reg == VID_VMODE) {
			context->pclock_div = (value >> 9 & 7) + 1;
			context->pclock_counter = 0;
			if (value & 0x10) {
				context->mode = VMODE_VARIABLE;
			} else {
				context->mode = value >> 1 & 3;
			}
			printf("Mode %s, pixel clock divider: %d, time base generation: %s\n", vmode_names[context->mode], context->pclock_div, value & BIT_TBGEN ? "enabled" : "disabled");
		}
		switch (reg)
		{
		case VID_OBJLIST1:
			printf("Object List Pointer 1: %X\n", value);
			break;
		case VID_OBJLIST2:
			printf("Object List Pointer 2: %X\n", value);
			break;
		case VID_HPERIOD:
			printf("Horizontal period: %d\n", value & 0x3FF);
			break;
		case VID_HBLANK_BEGIN:
			printf("horizontal blanking begin: %d\n", value & 0x7FF);
			break;
		case VID_HBLANK_END:
			printf("horizontal blanking end: %d\n", value & 0x7FF);
			break;
		case VID_HSYNC:
			printf("horizontal sync start: %d\n", value & 0x7FF);
			break;
		case VID_HDISP_BEGIN1:
			printf("horizontal display begin 1: %d\n", value & 0x7FF);
			break;
		case VID_HDISP_BEGIN2:
			printf("horizontal display begin 2: %d\n", value & 0x7FF);
			break;
		case VID_HDISP_END:
			printf("horizontal display end: %d\n", value & 0x7FF);
			break;
		case VID_VPERIOD:
			printf("Vertical period: %d\n", value & 0x7FF);
			break;
		case VID_VBLANK_BEGIN:
			printf("vertical blanking begin: %d\n", value & 0x7FF);
			break;
		case VID_VBLANK_END:
			printf("vertical blanking end: %d\n", value & 0x7FF);
			break;
		case VID_VSYNC:
			printf("vertical sync start: %d\n", value & 0x7FF);
			break;
		case VID_VDISP_BEGIN:
			printf("vertical display begin: %d\n", value & 0x7FF);
			break;
		case VID_VDISP_END:
			printf("vertical display end: %d\n", value & 0x7FF);
			break;
		}
	} else {
		fprintf(stderr, "Write to invalid video/object processor register %X:%X\n", address, value);
	}
}
