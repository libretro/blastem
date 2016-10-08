#ifndef JAG_VIDEO_H_
#define JAG_VIDEO_H_

#define JAG_VIDEO_REGS 0x2E
#define LINEBUFFER_WORDS 720

typedef struct {
	uint16_t regs[JAG_VIDEO_REGS];
	
	uint16_t     clut[256];
	uint16_t     line_buffer_a[LINEBUFFER_WORDS];
	uint16_t     line_buffer_b[LINEBUFFER_WORDS];
	uint16_t     *write_line_buffer;
	uint16_t     *read_line_buffer;
	
	uint32_t cycles;
} jag_video;

jag_video *jag_video_init(void);
void jag_video_run(jag_video *context, uint32_t target_cycle);

#endif //JAG_VIDEO_H_
