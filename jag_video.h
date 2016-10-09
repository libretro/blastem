#ifndef JAG_VIDEO_H_
#define JAG_VIDEO_H_

enum {
	VID_HCOUNT,
	VID_VCOUNT,
	VID_HLPEN,
	VID_VLPEN,
	VID_REG_C,
	VID_REG_E,
	VID_OBJ0,
	VID_OBJ1,
	VID_OBJ2,
	VID_OBJ3,
	VID_REG_18,
	VID_REG_1A,
	VID_REG_1C,
	VID_REG_1E,
	VID_OBJLIST1,
	VID_OBJLIST2,
	VID_REG_24,
	VID_OBJFLAG,
	VID_VMODE,
	VID_BORDER_RG,
	VID_BORDER_B,
	VID_HPERIOD,
	VID_HBLANK_BEGIN,
	VID_HBLANK_END,
	VID_HSYNC,
	VID_HVSYNC,
	VID_HDISP_BEGIN1,
	VID_HDISP_BEGIN2,
	VID_HDISP_END,
	VID_VPERIOD,
	VID_VBLANK_BEGIN,
	VID_VBLANK_END,
	VID_VSYNC,
	VID_VDISP_BEGIN,
	VID_VDISP_END,
	VID_VEQUAL_BEGIN,
	VID_VEQUAL_END,
	VID_VINT,
	VID_PIT0,
	VID_PIT1,
	VID_HEQUAL_END,
	VID_REG_56,
	VID_BGCOLOR,
	JAG_VIDEO_REGS
};
#define LINEBUFFER_WORDS 720

typedef struct {
	uint16_t regs[JAG_VIDEO_REGS];
	
	uint16_t     clut[256];
	uint16_t     line_buffer_a[LINEBUFFER_WORDS];
	uint16_t     line_buffer_b[LINEBUFFER_WORDS];
	uint16_t     *write_line_buffer;
	uint16_t     *read_line_buffer;
	
	uint32_t     cycles;
	uint8_t      pclock_div;
	uint8_t      pclock_counter;
	uint8_t      mode;
} jag_video;


jag_video *jag_video_init(void);
void jag_video_run(jag_video *context, uint32_t target_cycle);
void jag_video_reg_write(jag_video *context, uint32_t address, uint16_t value);

#endif //JAG_VIDEO_H_
