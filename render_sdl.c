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
uint32_t frame_delay = 1000/60;

int16_t * current_psg = NULL;
int16_t * current_ym = NULL;

uint32_t buffer_samples, sample_rate;
uint32_t missing_count;

SDL_mutex * audio_mutex;
SDL_cond * audio_ready;
SDL_cond * psg_cond;
SDL_cond * ym_cond;
uint8_t quitting = 0;

void audio_callback(void * userdata, uint8_t *byte_stream, int len)
{
	//puts("audio_callback");
	int16_t * stream = (int16_t *)byte_stream;
	int samples = len/(sizeof(int16_t)*2);
	int16_t * psg_buf, * ym_buf;
	uint8_t local_quit;
	SDL_LockMutex(audio_mutex);
		psg_buf = NULL;
		ym_buf = NULL;
		do {
			if (!psg_buf) {
				psg_buf = current_psg;
				current_psg = NULL;
				SDL_CondSignal(psg_cond);
			}
			if (!ym_buf) {
				ym_buf = current_ym;
				current_ym = NULL;
				SDL_CondSignal(ym_cond);
			}
			if (!quitting && (!psg_buf || !ym_buf)) {
				SDL_CondWait(audio_ready, audio_mutex);
			}
		} while(!quitting && (!psg_buf || !ym_buf));

		local_quit = quitting;
	SDL_UnlockMutex(audio_mutex);
	if (!local_quit) {
		for (int i = 0; i < samples; i++) {
			*(stream++) = psg_buf[i] + *(ym_buf++);
			*(stream++) = psg_buf[i] + *(ym_buf++);
		}
	}
}

void render_close_audio()
{
	SDL_LockMutex(audio_mutex);
		quitting = 1;
		SDL_CondSignal(audio_ready);
	SDL_UnlockMutex(audio_mutex);
	SDL_CloseAudio();
}

void render_init(int width, int height, char * title, uint32_t fps)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    atexit(render_close_audio);
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
    SDL_WM_SetCaption(title, title);
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
    for (int i = 0; i < 100; i++) {
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
    
    frame_delay = 1000/fps;
    
    audio_mutex = SDL_CreateMutex();
    psg_cond = SDL_CreateCond();
    ym_cond = SDL_CreateCond();
    audio_ready = SDL_CreateCond();
    
    SDL_AudioSpec desired, actual;
    desired.freq = 48000;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = audio_callback;
    desired.userdata = NULL;
    
    if (SDL_OpenAudio(&desired, &actual) < 0) {
    	fprintf(stderr, "Unable to open SDL audio: %s\n", SDL_GetError());
    	exit(1);
    }
    buffer_samples = actual.samples;
    sample_rate = actual.freq;
    printf("Initialized audio at frequency %d with a %d sample buffer\n", actual.freq, actual.samples);
    SDL_PauseAudio(0);
}

uint16_t blankbuf[320*240];

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
    int othermask = repeat_y >> 1;
    uint16_t *otherbuf = (context->regs[REG_MODE_4] & BIT_INTERLACE) ? context->evenbuf : blankbuf;
    switch (screen->format->BytesPerPixel) {
    case 2:
        buf_16 = (uint16_t *)screen->pixels;
        for (int y = 0; y < 240; y++) {
        	for (int i = 0; i < repeat_y; i++,buf_16 += screen->pitch/2) {
        		uint16_t *line = buf_16;
        		uint16_t *src_line = (i & othermask ? otherbuf : context->oddbuf) + y * 320;
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
		    		uint16_t gen_color = context->oddbuf[y * 320 + x];
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
	    		uint16_t *src_line = (i & othermask ? otherbuf : context->oddbuf) + y * 320;
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
						uint16_t gen_color = context->oddbuf[y * 320 + x];
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
    if (context->regs[REG_MODE_4] & BIT_INTERLACE)
    {
    	context->framebuf = context->framebuf == context->oddbuf ? context->evenbuf : context->oddbuf;
    }
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

void render_debug_mode(uint8_t mode)
{
	if (mode < 4) {
		render_dbg = mode;
	}
}

void render_debug_pal(uint8_t pal)
{
	if (pal < 4) {
		debug_pal = pal;
	}
}

int32_t handle_event(SDL_Event *event)
{
	switch (event->type) {
	case SDL_KEYDOWN:
		handle_keydown(event->key.keysym.sym);
		break;
	case SDL_KEYUP:
		handle_keyup(event->key.keysym.sym);
		break;
	case SDL_QUIT:
		puts("");
		exit(0);
	}
	return 0;
}

uint32_t frame_counter = 0;
uint32_t start = 0;
int wait_render_frame(vdp_context * context, int frame_limit)
{
	SDL_Event event;
	int ret = 0;
	while(SDL_PollEvent(&event)) {
		ret = handle_event(&event);
	}
	if (frame_limit) {
		//TODO: Adjust frame delay so we actually get 60 FPS rather than 62.5 FPS
		uint32_t current = SDL_GetTicks();
		uint32_t desired = last_frame + frame_delay;
		if (current < desired) {
			uint32_t delay = last_frame + frame_delay - current;
			if (delay > min_delay) {
				SDL_Delay((delay/min_delay)*min_delay);
			}
			while ((desired) >= SDL_GetTicks()) {
			}
		}
	}
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

void process_events()
{
	SDL_Event event;
	while(SDL_PollEvent(&event)) {
		handle_event(&event);
	}
}

void render_wait_psg(psg_context * context)
{
	SDL_LockMutex(audio_mutex);
		while (current_psg != NULL) {
			SDL_CondWait(psg_cond, audio_mutex);
		}
		current_psg = context->audio_buffer;
		SDL_CondSignal(audio_ready);

		context->audio_buffer = context->back_buffer;
		context->back_buffer = current_psg;
	SDL_UnlockMutex(audio_mutex);
	context->buffer_pos = 0;
}

void render_wait_ym(ym2612_context * context)
{
	SDL_LockMutex(audio_mutex);
		while (current_ym != NULL) {
			SDL_CondWait(ym_cond, audio_mutex);
		}
		current_ym = context->audio_buffer;
		SDL_CondSignal(audio_ready);

		context->audio_buffer = context->back_buffer;
		context->back_buffer = current_ym;
	SDL_UnlockMutex(audio_mutex);
	context->buffer_pos = 0;
}

void render_fps(uint32_t fps)
{
	frame_delay = 1000/fps;
}

uint32_t render_audio_buffer()
{
	return buffer_samples;
}

uint32_t render_sample_rate()
{
	return sample_rate;
}


