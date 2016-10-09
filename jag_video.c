#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "jag_video.h"

enum {
	VMODE_CRY,
	VMODE_RGB24,
	VMODE_DIRECT16,
	VMODE_RGB16,
	VMODE_VARIABLE
};

char *vmode_names[] = {
	"CRY",
	"RGB16",
	"DIRECT16",
	"VARIABLE"
};

jag_video *jag_video_init(void)
{
	return calloc(1, sizeof(jag_video));
}

void jag_video_run(jag_video *context, uint32_t target_cycle)
{
	context->cycles = target_cycle;
	
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
			printf("Mode %s, pixel clock divider: %d, time base generation: %s\n", vmode_names[context->mode], context->pclock_div, value & 1 ? "enabled" : "disabled");
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
