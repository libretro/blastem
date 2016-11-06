#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "jag_video.h"
#include "jaguar.h"
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
		uint8_t c = cry >> 8 & 0xF;
		uint8_t r = cry >> 12;
		
		uint32_t red = cry_red[c < 7 ? 0 : c - 7][r];
		uint32_t green = cry_green[c][r < 8 ? r : 15 - r];
		uint32_t blue = cry_red[c < 7 ? 0 : c - 7][15-r];
		red = red * y / 255;
		blue = blue * y / 255;
		green = green * y / 255;
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

enum {
	OBJ_IDLE,
	OBJ_FETCH_DESC1,
	OBJ_FETCH_DESC2,
	OBJ_FETCH_DESC3,
	OBJ_PROCESS,
	OBJ_HEIGHT_WB,
	OBJ_REMAINDER_WB,
	OBJ_GPU_WAIT
};

enum {
	OBJ_BITMAP,
	OBJ_SCALED,
	OBJ_GPU,
	OBJ_BRANCH,
	OBJ_STOP
};

uint32_t jag_cycles_to_halfline(jag_video *context, uint32_t target)
{
	uint32_t cycles = context->regs[VID_HPERIOD] - (context->regs[VID_HCOUNT] & 0x3FF);
	uint32_t num_lines;
	if (context->regs[VID_VCOUNT] < target) {
		num_lines = target - 1 - context->regs[VID_VCOUNT];
	} else {
		num_lines = target + context->regs[VID_VPERIOD] - context->regs[VID_VCOUNT];
	}
	cycles += num_lines * context->regs[VID_HPERIOD];
	return cycles;
}

uint32_t jag_next_vid_interrupt(jag_video *context)
{
	if (context->regs[VID_VINT] > context->regs[VID_VPERIOD]) {
		return 0xFFFFFFF;
	}
	return context->cycles + jag_cycles_to_halfline(context, context->regs[VID_VINT]);
}

void op_run(jag_video *context)
{
	while (context->op.cycles < context->cycles)
	{
		switch (context->op.state)
		{
		case OBJ_IDLE:
		case OBJ_GPU_WAIT:
			context->op.cycles = context->cycles;
			break;
		case OBJ_FETCH_DESC1: {
			uint32_t address = context->regs[VID_OBJLIST1] | context->regs[VID_OBJLIST2] << 16;
			uint64_t val = jag_read_phrase(context->system, address, &context->op.cycles);
			address += 8;
				
			context->regs[VID_OBJ0] = val >> 48;
			context->regs[VID_OBJ1] = val >> 32;
			context->regs[VID_OBJ2] = val >> 16;
			context->regs[VID_OBJ3] = val;
			context->op.type = val & 7;
			context->op.has_prefetch = 0;
			uint16_t ypos = val >> 3 & 0x7FF;
			switch (context->op.type)
			{
			case OBJ_BITMAP:
			case OBJ_SCALED: {
				uint16_t height = val >> 14 & 0x7FF;
				uint32_t link = (address & 0xC00007) | (val >> 21 & 0x3FFFF8);
				if ((ypos == 0x7FF || context->regs[VID_VCOUNT] >= ypos) && height) {
					context->op.state = OBJ_FETCH_DESC2;
					context->op.obj_start = address - 8;
					context->op.ypos = ypos;
					context->op.height = height;
					context->op.link = link;
					context->op.data_address = val >> 40 & 0xFFFFF8;
					context->op.cur_address = context->op.data_address;
				} else {
					//object is not visible on this line, advance to next object
					address = link;
				}
				break;
			}
			case OBJ_GPU:
				context->op.state = OBJ_GPU_WAIT;
				break;
			case OBJ_BRANCH: {
				uint8_t branch;
				switch(val >> 14 & 7)
				{
				case 0:
					branch = ypos == context->regs[VID_VCOUNT] || ypos == 0x7FF;
					break;
				case 1:
					branch = ypos > context->regs[VID_VCOUNT];
					break;
				case 2:
					branch = ypos < context->regs[VID_VCOUNT];
					break;
				case 3:
					branch = context->regs[VID_OBJFLAG] & 1;
					break;
				case 4:
					branch = (context->regs[VID_HCOUNT] & 0x400) != 0;
					break;
				default:
					branch = 0;
					fprintf(stderr, "Invalid branch CC type %d in object at %X\n", (int)(val >> 14 & 7), address-8);
					break;
				}
				if (branch) {
					address &= 0xC00007;
					address |= val >> 21 & 0x3FFFF8;
				}
			}
			case OBJ_STOP:
				//TODO: trigger interrupt
				context->op.state = OBJ_IDLE;
				break;
			}
			context->regs[VID_OBJLIST1] = address;
			context->regs[VID_OBJLIST2] = address >> 16;
			break;
		}
		case OBJ_FETCH_DESC2: {
			uint32_t address = context->regs[VID_OBJLIST1] | context->regs[VID_OBJLIST2] << 16;
			uint64_t val = jag_read_phrase(context->system, address, &context->op.cycles);
			address += 8;
			
			context->op.xpos = val & 0xFFF;
			if (context->op.xpos & 0x800) {
				context->op.xpos |= 0xF000;
			}
			context->op.increment = (val >> 15 & 0x7) * 8;
			context->op.bpp = 1 << (val >> 12 & 7);
			if (context->op.bpp == 32) {
				context->op.bpp = 24;
			}
			context->op.line_pitch = (val >> 18 & 0x3FF) * 8;
			if (context->op.bpp < 8) {
				context->op.pal_offset = val >> 37;
				if (context->op.bpp == 4) {
					context->op.pal_offset &= 0xF0;
				} else if(context->op.bpp == 2) {
					context->op.pal_offset &= 0xFC;
				} else {
					context->op.pal_offset &= 0xFE;
				}
			} else {
				context->op.pal_offset = 0;
			}
			context->op.line_phrases = val >> 28 & 0x3FF;
			context->op.hflip = (val & (1UL << 45)) != 0;
			context->op.addpixels = (val & (1UL << 46)) != 0;
			context->op.transparent = (val & (1UL << 47)) != 0;
			//TODO: do something with RELEASE flag
			context->op.leftclip = val >> 49;
			if (context->op.type == OBJ_SCALED) {
				context->op.state = OBJ_FETCH_DESC3;
				switch (context->op.bpp)
				{
				case 1:
					context->op.leftclip &= 0x3F;
					
					break;
				//documentation isn't clear exactly how this works for higher bpp values
				case 2:
					context->op.leftclip &= 0x3E;
					break;
				case 4:
					context->op.leftclip &= 0x3C;
					break;
				case 8:
					context->op.leftclip &= 0x38;
					break;
				case 16:
					context->op.leftclip &= 0x30;
					break;
				default:
					context->op.leftclip = 0x20;
					break;
				}
			} else {
				context->op.state = OBJ_PROCESS;
				address = context->op.link;
				switch (context->op.bpp)
				{
				case 1:
					context->op.leftclip &= 0x3E;
					break;
				case 2:
					context->op.leftclip &= 0x3C;
					break;
				//values for 4bpp and up are sort of a guess
				case 4:
					context->op.leftclip &= 0x38;
					break;
				case 8:
					context->op.leftclip &= 0x30;
					break;
				case 16:
					context->op.leftclip &= 0x20;
					break;
				default:
					context->op.leftclip = 0;
					break;
				}
			}
			if (context->op.xpos < 0) {
				int16_t pixels_per_phrase = 64 / context->op.bpp;
				int16_t clip = -context->op.xpos / pixels_per_phrase;
				int16_t rem = -context->op.xpos % pixels_per_phrase;
				if (clip >= context->op.line_phrases) {
					context->op.line_phrases = 0;
				} else {
					context->op.line_phrases -= clip;
					context->op.leftclip += rem * context->op.bpp;
					if (context->op.leftclip >= 64) {
						context->op.line_phrases--;
						context->op.leftclip -= 64;
					}
					
				}
			} else if (context->op.bpp < 32){
				context->op.lb_offset = context->op.xpos;
			} else {
				context->op.lb_offset = context->op.xpos * 2;
			}
			if (context->op.lb_offset >= LINEBUFFER_WORDS || !context->op.line_phrases) {
				//ignore objects that are completely offscreen
				//not sure if that's how the hardware does it, but it would make sense
				context->op.state = OBJ_FETCH_DESC1;
				address = context->op.link;
			}
			context->regs[VID_OBJLIST1] = address;
			context->regs[VID_OBJLIST2] = address >> 16;
			break;
		}
		case OBJ_FETCH_DESC3: {
			uint32_t address = context->regs[VID_OBJLIST1] | context->regs[VID_OBJLIST2] << 16;
			uint64_t val = jag_read_phrase(context->system, address, &context->op.cycles);
			
			context->op.state = OBJ_PROCESS;
			context->op.hscale = val & 0xFF;;
			context->op.hremainder = val & 0xFF;
			context->op.vscale = val >> 8 & 0xFF;
			context->op.remainder = val >> 16 & 0xFF;
			
			context->regs[VID_OBJLIST1] = context->op.link;
			context->regs[VID_OBJLIST2] = context->op.link >> 16;
			break;
		}
		case OBJ_PROCESS: {
			uint32_t proc_cycles = 0;
			if (!context->op.has_prefetch && context->op.line_phrases) {
				context->op.prefetch = jag_read_phrase(context->system, context->op.cur_address, &proc_cycles);
				context->op.cur_address += context->op.increment;
				context->op.has_prefetch = 1;
				context->op.line_phrases--;
			}
			if (!proc_cycles) {
				//run at least one cycle of writes even if we didn't spend any time reading
				proc_cycles = 1;
			}
			while (proc_cycles)
			{
				if (context->op.im_bits) {
					uint32_t val = context->op.im_data >> (context->op.im_bits - context->op.bpp);
					val &= (1 << context->op.bpp) - 1;
					if (val || !context->op.transparent)
					{
						if (context->op.bpp < 16) {
							val = context->clut[val + context->op.pal_offset];
						}
						if (context->op.bpp == 32) {
							context->write_line_buffer[context->op.lb_offset++] = val >> 16;
						}
						context->write_line_buffer[context->op.lb_offset++] = val;
					} else {
						context->op.lb_offset += context->op.bpp == 32 ? 2 : 1;
					}
					if (context->op.type == OBJ_SCALED) {
						context->op.hremainder -= 0x20;
						while (context->op.hremainder <= 0 && context->op.im_bits) {
							context->op.im_bits -= context->op.bpp;
							context->op.hremainder += context->op.hscale;
						}
					} else {
						context->op.im_bits -= context->op.bpp;
					}
				}
				if (context->op.im_bits && context->op.bpp < 32 && context->op.type == OBJ_BITMAP && context->op.lb_offset < LINEBUFFER_WORDS) {
					uint32_t val = context->op.im_data >> (context->op.im_bits - context->op.bpp);
					val &= (1 << context->op.bpp) - 1;
					if (val || !context->op.transparent)
					{
						val = context->clut[val + context->op.pal_offset];
						context->write_line_buffer[context->op.lb_offset] = val;
					}
					context->op.lb_offset++;
					context->op.im_bits -= context->op.bpp;
				}
				context->op_cycles++;
				proc_cycles--;
			}
			if (!context->op.im_bits && context->op.has_prefetch) {
				context->op.im_data = context->op.prefetch;
				context->op.has_prefetch = 0;
				//docs say this is supposed to be a value in pixels
				//but given the "significant" bits part I'm guessing
				//this is actually how many bits are pre-shifted off
				//the first phrase read in a line
				context->op.im_bits = 64 - context->op.leftclip;
				context->op.leftclip = 0;
			}
			if (context->op.lb_offset == LINEBUFFER_WORDS || (!context->op.im_bits && !context->op.line_phrases)) {
				context->op.state = OBJ_HEIGHT_WB;
			}
			break;
		}
		case OBJ_HEIGHT_WB: {
			if (context->op.type == OBJ_BITMAP) {
				context->op.height--;
				context->op.data_address += context->op.line_pitch;
				context->op.state = OBJ_FETCH_DESC1;
			} else {
				context->op.remainder -= 0x20;
				context->op.state = OBJ_REMAINDER_WB;
				while (context->op.height && context->op.remainder <= 0) {
					context->op.height--;
					context->op.remainder += context->op.vscale;
					context->op.data_address += context->op.line_pitch;
				}
			}
			uint64_t val = context->op.type | context->op.ypos << 3  | context->op.height << 14
				| ((uint64_t)context->op.link & 0x3FFFF8) << 21 | ((uint64_t)context->op.data_address) << 40;
			context->op.cycles += jag_write_phrase(context->system, context->op.obj_start, val);
			break;
		}
		case OBJ_REMAINDER_WB: {
			uint64_t val = context->op.hscale | context->op.vscale << 8 | context->op.remainder << 16;
			context->op.cycles += jag_write_phrase(context->system, context->op.obj_start+16, val);
			context->op.state = OBJ_FETCH_DESC1;
			break;
		}
		}
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
				
				//kick off object processor
				context->op.state = OBJ_FETCH_DESC1;
			} else if (context->regs[VID_HCOUNT] == context->regs[VID_HDISP_END]) {
				//stob object processor
				context->op.state = OBJ_IDLE;
			}
			
			context->cycles++;
			op_run(context);
			
			//advance counters
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
					if (context->regs[VID_VCOUNT] == context->regs[VID_VINT]) {
						context->cpu_int_pending |= BIT_CPU_VID_INT_ENABLED;
					}
				}
			} else {
				context->regs[VID_HCOUNT]++;
			}
			
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
