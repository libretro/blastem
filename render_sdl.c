#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "render.h"
#include "blastem.h"

SDL_Surface *screen;
uint8_t render_dbg = 0;
uint8_t debug_pal = 0;

uint32_t last_frame = 0;

int32_t color_map[1 << 12];
uint8_t levels[] = {0, 27, 49, 71, 87, 103, 119, 130, 146, 157, 174, 190, 206, 228, 255};

uint32_t min_delay;

void render_init(int width, int height)
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    printf("width: %d, height: %d\n", width, height);
    screen = SDL_SetVideoMode(width, height, 32, SDL_SWSURFACE | SDL_ANYFORMAT);
    if (!screen) {
    	fprintf(stderr, "Unable to get SDL surface: %s\n", SDL_GetError());
        exit(1);
    }
    if (screen->format->BytesPerPixel < 2) {
    	fprintf(stderr, "BlastEm requires at least a 16-bit surface, SDL returned a %d-bit surface\n", screen->format->BytesPerPixel * 8);
    	exit(1);
    }
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
		color_map[color] = SDL_MapRGB(screen->format, r, g, b);
    }
    min_delay = 0;
    for (int i = 0; i < 20; i++) {
    	uint32_t start = SDL_GetTicks();
    	SDL_Delay(1);
    	uint32_t delay = SDL_GetTicks()-start;
    	if (delay > min_delay) {
    		min_delay = delay;
    	}
    }
    if (!min_delay) {
    	min_delay = 1;
    }
    printf("minimum delay: %d\n", min_delay);
}

void render_context(vdp_context * context)
{
	uint8_t *buf_8;
	uint16_t *buf_16;
	uint32_t *buf_32; 
	uint8_t b,g,r;
	last_frame = SDL_GetTicks();
	if (SDL_MUSTLOCK(screen)) {
		if (SDL_LockSurface(screen) < 0) {
			return;
		}
    }
    uint16_t repeat_x = screen->clip_rect.w / 320;
    uint16_t repeat_y = screen->clip_rect.h / 240;
    if (repeat_x > repeat_y) {
    	repeat_x = repeat_y;
    } else {
    	repeat_y = repeat_x;
    }
    switch (screen->format->BytesPerPixel) {
    case 2:
        buf_16 = (uint16_t *)screen->pixels;
        for (int y = 0; y < 240; y++) {
        	for (int i = 0; i < repeat_y; i++,buf_16 += screen->pitch/2) {
        		uint16_t *line = buf_16;
        		uint16_t *src_line = context->framebuf + y * 320;
		    	for (int x = 0; x < 320; x++) {
		    		uint16_t color = color_map[*(src_line++) & 0xFFF];
		    		for (int j = 0; j < repeat_x; j++) {
		    			*(line++) = color;
		    		}
		    	}
		    }
        }
    	break;
    case 3:
        buf_8 = (uint8_t *)screen->pixels;
        for (int y = 0; y < 240; y++) {
        	for (int i = 0; i < repeat_y; i++,buf_8 += screen->pitch) {
        		uint8_t *line = buf_8;
		    	for (int x = 0; x < 320; x++) {
		    		uint16_t gen_color = context->framebuf[y * 320 + x];
		    		b = ((gen_color >> 8) & 0xE) * 18;
		    		g = ((gen_color >> 4) & 0xE) * 18;
		    		r = (gen_color& 0xE) * 18;
		    		for (int j = 0; j < repeat_x; j++) {
						*(buf_8+screen->format->Rshift/8) = r;
						*(buf_8+screen->format->Gshift/8) = g;
						*(buf_8+screen->format->Bshift/8) = b;
						buf_8 += 3;
					}
		    	}
		    }
        }
    	break;
    case 4:
        buf_32 = (uint32_t *)screen->pixels;

	    for (int y = 0; y < 240; y++) {
	    	for (int i = 0; i < repeat_y; i++,buf_32 += screen->pitch/4) {
	    		uint32_t *line = buf_32;
	    		uint16_t *src_line = context->framebuf + y * 320;
		    	for (int x = 0; x < 320; x++) {
		    		uint32_t color;
		    		if (!render_dbg) {
		    			color = color_map[*(src_line++) & 0xFFF];
					} else if(render_dbg == 2) {
						color = color_map[context->cram[(y/30)*8 + x/40]];
					} else if(render_dbg == 3) {
						if (x & 1) {
							color = color_map[context->cram[ (debug_pal << 4) | (context->vdpmem[(x/8)*32 + (y/8)*32*40 + (x%8)/2 + (y%8)*4] & 0xF) ]];
						} else {
							color = color_map[context->cram[ (debug_pal << 4) | (context->vdpmem[(x/8)*32 + (y/8)*32*40 + (x%8)/2 + (y%8)*4] >> 4) ]];
						}
					}else {
						uint16_t gen_color = context->framebuf[y * 320 + x];
						r = g = b = 0;
						switch(gen_color & FBUF_SRC_MASK)
						{
						case FBUF_SRC_A:
							g = 127;
							break;
						case FBUF_SRC_W:
							g = 127;
							b = 127;
							break;
						case FBUF_SRC_B:
							b = 127;
							break;
						case FBUF_SRC_S:
							r = 127;
							break;
						case FBUF_SRC_BG:
							r = 127;
							b = 127;
						}
						if (gen_color & FBUF_BIT_PRIORITY) {
							b *= 2;
							g *= 2;
							r *= 2;
						}
						if (gen_color & FBUF_SHADOW) {
							b /= 2;
							g /= 2;
							r /= 2;
						} else if(gen_color & FBUF_HILIGHT) {
							b = b ? b : 64;
							g = g ? g : 64;
							r = r ? r : 64;
						}
						color = SDL_MapRGB(screen->format, r, g, b);
					}
					for (int j = 0; j < repeat_x; j++) {
						*(line++) = color;
					}
				}
	    	}
	    }
		break;
	}
    if ( SDL_MUSTLOCK(screen) ) {
        SDL_UnlockSurface(screen);
    }
    SDL_UpdateRect(screen, 0, 0, screen->clip_rect.w, screen->clip_rect.h);
}

void render_wait_quit(vdp_context * context)
{
	SDL_Event event;
	while(SDL_WaitEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
			if (event.key.keysym.sym == SDLK_LEFTBRACKET) {
				render_dbg++;
				if (render_dbg == 4) {
					render_dbg = 0;
				}
				render_context(context);
			} else if(event.key.keysym.sym ==  SDLK_RIGHTBRACKET) {
				debug_pal++;
				if (debug_pal == 4) {
					debug_pal = 0;
				}
			}
			break;
		case SDL_QUIT:
			return;
		}
	}
}

#define DPAD_UP      0x01
#define BUTTON_Z     0x01
#define DPAD_DOWN    0x02
#define BUTTON_Y     0x02
#define DPAD_LEFT    0x04
#define BUTTON_X     0x04
#define DPAD_RIGHT   0x08
#define BUTTON_MODE  0x08
#define BUTTON_A     0x10
#define BUTTON_B     0x10
#define BUTTON_START 0x20
#define BUTTON_C     0x20

#define FRAME_DELAY 16
#define MIN_DELAY 5
uint32_t frame_counter = 0;
uint32_t start = 0;
extern uint8_t z80_ram[];
int wait_render_frame(vdp_context * context)
{
	FILE * outfile;
	SDL_Event event;
	int ret = 0;
	while(SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_KEYDOWN:
			switch(event.key.keysym.sym)
			{
			case SDLK_LEFTBRACKET:
				render_dbg++;
				if (render_dbg == 4) {
					render_dbg = 0;
				}
				break;
			case SDLK_RIGHTBRACKET:
				debug_pal++;
				if (debug_pal == 4) {
					debug_pal = 0;
				}
				break;
			case SDLK_t:
				outfile = fopen("state.gst", "wb");
				fwrite("GST\0\0\0\xE0\x40", 1, 8, outfile);
				vdp_save_state(context, outfile);
				fclose(outfile);
				puts("state saved to state.gst");
				break;
			case SDLK_u:
				ret = 1;
				break;
			case SDLK_RETURN:
				gamepad_1.input[GAMEPAD_TH0] |= BUTTON_START;
				break;
			case SDLK_UP:
				gamepad_1.input[GAMEPAD_TH0] |= DPAD_UP;
				gamepad_1.input[GAMEPAD_TH1] |= DPAD_UP;
				break;
			case SDLK_DOWN:
				gamepad_1.input[GAMEPAD_TH0] |= DPAD_DOWN;
				gamepad_1.input[GAMEPAD_TH1] |= DPAD_DOWN;
				break;
			case SDLK_LEFT:
				gamepad_1.input[GAMEPAD_TH1] |= DPAD_LEFT;
				break;
			case SDLK_RIGHT:
				gamepad_1.input[GAMEPAD_TH1] |= DPAD_RIGHT;
				break;
			case SDLK_a:
				gamepad_1.input[GAMEPAD_TH0] |= BUTTON_A;
				//printf("BUTTON_A Dn | GAMEPAD_TH0: %X\n", gamepad_1.input[GAMEPAD_TH0]);
				break;
			case SDLK_s:
				gamepad_1.input[GAMEPAD_TH1] |= BUTTON_B;
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_B;
				break;
			case SDLK_d:
				gamepad_1.input[GAMEPAD_TH1] |= BUTTON_C;
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_C;
				break;
			case SDLK_q:
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_X;
				break;
			case SDLK_w:
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_Y;
				break;
			case SDLK_e:
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_Z;
				break;
			case SDLK_f:
				gamepad_1.input[GAMEPAD_EXTRA] |= BUTTON_MODE;
				break;
			}
			break;
		case SDL_KEYUP:
			switch(event.key.keysym.sym)
			{
			case SDLK_RETURN:
				gamepad_1.input[GAMEPAD_TH0] &= ~BUTTON_START;
				break;
			case SDLK_UP:
				gamepad_1.input[GAMEPAD_TH0] &= ~DPAD_UP;
				gamepad_1.input[GAMEPAD_TH1] &= ~DPAD_UP;
				break;
			case SDLK_DOWN:
				gamepad_1.input[GAMEPAD_TH0] &= ~DPAD_DOWN;
				gamepad_1.input[GAMEPAD_TH1] &= ~DPAD_DOWN;
				break;
			case SDLK_LEFT:
				gamepad_1.input[GAMEPAD_TH1] &= ~DPAD_LEFT;
				break;
			case SDLK_RIGHT:
				gamepad_1.input[GAMEPAD_TH1] &= ~DPAD_RIGHT;
				break;
			case SDLK_a:
				gamepad_1.input[GAMEPAD_TH0] &= ~BUTTON_A;
				//printf("BUTTON_A Up | GAMEPAD_TH0: %X\n", gamepad_1.input[GAMEPAD_TH0]);
				break;
			case SDLK_s:
				gamepad_1.input[GAMEPAD_TH1] &= ~BUTTON_B;
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_B;
				break;
			case SDLK_d:
				gamepad_1.input[GAMEPAD_TH1] &= ~BUTTON_C;
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_C;
				break;
			case SDLK_q:
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_X;
				break;
			case SDLK_w:
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_Y;
				break;
			case SDLK_e:
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_Z;
				break;
			case SDLK_f:
				gamepad_1.input[GAMEPAD_EXTRA] &= ~BUTTON_MODE;
				break;
			case SDLK_z:{
				FILE * f = fopen("zram.bin", "wb");
				fwrite(z80_ram, 1, 8 * 1024, f);
				fclose(f);
				break;
				}
			}
			break;
		case SDL_QUIT:
			puts("");
			exit(0);
		}
	}
	//TODO: Adjust frame delay so we actually get 60 FPS rather than 62.5 FPS
	/*uint32_t current = SDL_GetTicks();
	uint32_t desired = last_frame + FRAME_DELAY;
	if (current < desired) {
		uint32_t delay = last_frame + FRAME_DELAY - current;
		//TODO: Calculate MIN_DELAY at runtime
		if (delay > MIN_DELAY) {
			SDL_Delay((delay/MIN_DELAY)*MIN_DELAY);
		}
		while ((desired) >= SDL_GetTicks()) {
		}
	}*/
	render_context(context);
	
	
	//TODO: Figure out why this causes segfaults
	/*frame_counter++;
	if ((last_frame - start) > 1000) {
		if (start && (last_frame-start)) {
			printf("\r%f fps", ((float)frame_counter) / (((float)(last_frame-start)) / 1000.0));
			fflush(stdout);
		}
		start = last_frame;
		frame_counter = 0;
	}*/
	return ret;
}


