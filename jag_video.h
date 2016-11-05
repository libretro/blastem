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
	uint64_t im_data;
	uint64_t prefetch;
	uint32_t cycles;
	uint32_t obj_start;
	uint32_t link;
	uint32_t data_address;
	uint32_t cur_address;
	uint32_t increment;
	uint32_t line_pitch;
	uint32_t lb_offset;
	int16_t  xpos;
	uint16_t ypos;
	uint16_t height;
	int16_t  hscale;
	int16_t  vscale;
	int16_t  hremainder;
	int16_t  remainder;
	uint8_t  bpp;
	uint8_t  line_phrases;
	uint8_t  state;
	uint8_t  type;
	uint8_t  im_bits;
	uint8_t  pal_offset;
	uint8_t  has_prefetch;
	uint8_t  hflip;
	uint8_t  addpixels;
	uint8_t  transparent;
	uint8_t  leftclip;
} object_processor;

typedef struct {
	void             *system;
	uint32_t         *output;
	uint32_t         output_pitch;
	uint16_t         regs[JAG_VIDEO_REGS];
	
	uint16_t         clut[256];
	uint16_t         line_buffer_a[LINEBUFFER_WORDS];
	uint16_t         line_buffer_b[LINEBUFFER_WORDS];
	uint16_t         *write_line_buffer;
	
	uint32_t         cycles;
	uint32_t         op_cycles;
	uint8_t          pclock_div;
	uint8_t          pclock_counter;
	uint8_t          mode;
	uint8_t          cpu_int_pending;
	
	object_processor op;
	
} jag_video;


jag_video *jag_video_init(void);
void jag_video_run(jag_video *context, uint32_t target_cycle);
void jag_video_reg_write(jag_video *context, uint32_t address, uint16_t value);
uint32_t jag_next_vid_interrupt(jag_video *context);

#endif //JAG_VIDEO_H_
