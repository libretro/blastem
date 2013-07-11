#include <stdlib.h>
#include <stdio.h>
#include "render.h"
#include "blastem.h"
#include "io.h"

SDL_Surface *screen;
uint8_t render_dbg = 0;
uint8_t debug_pal = 0;

uint32_t last_frame = 0;

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

SDL_Joystick * joysticks[MAX_JOYSTICKS];
int num_joysticks;

int render_num_joysticks()
{
	return num_joysticks;
}

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return SDL_MapRGB(screen->format, r, g, b);
}

uint8_t render_depth()
{
	return screen->format->BytesPerPixel * 8;
}

void render_init(int width, int height, char * title, uint32_t fps)
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
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
    if (screen->format->BytesPerPixel != 2 && screen->format->BytesPerPixel != 4) {
    	fprintf(stderr, "BlastEm requires a 16-bit or 32-bit surface, SDL returned a %d-bit surface\n", screen->format->BytesPerPixel * 8);
    	exit(1);
    }
    SDL_WM_SetCaption(title, title);
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
    desired.samples = 2048;//1024;
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
    num_joysticks = SDL_NumJoysticks();
    if (num_joysticks > MAX_JOYSTICKS) {
    	num_joysticks = MAX_JOYSTICKS;
    }
    for (int i = 0; i < num_joysticks; i++) {
    	printf("Joystick %d: %s\n", i, SDL_JoystickName(i));
    	SDL_Joystick * joy = joysticks[i] = SDL_JoystickOpen(i);
    	if (joy) {
    		printf("\tNum Axes: %d\n\tNum Buttons: %d\n\tNum Hats: %d\n", SDL_JoystickNumAxes(joy), SDL_JoystickNumButtons(joy), SDL_JoystickNumHats(joy));
    	}
    }
    SDL_JoystickEventState(SDL_ENABLE);
}

uint32_t blankbuf[320*240];

void render_context(vdp_context * context)
{
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
    
    if (screen->format->BytesPerPixel == 2) {
    	uint16_t *otherbuf = (context->regs[REG_MODE_4] & BIT_INTERLACE) ? context->evenbuf : (uint16_t *)blankbuf;
    	uint16_t * oddbuf = context->oddbuf;
    	buf_16 = (uint16_t *)screen->pixels;
    	for (int y = 0; y < 240; y++) {
    		for (int i = 0; i < repeat_y; i++,buf_16 += screen->pitch/2) {
        		uint16_t *line = buf_16;
        		uint16_t *src_line = (i & othermask ? otherbuf : oddbuf) + y * 320;
		    	for (int x = 0; x < 320; x++) {
		    		uint16_t color = *(src_line++);
		    		for (int j = 0; j < repeat_x; j++) {
		    			*(line++) = color;
		    		}
		    	}
		    }
    	}
    } else {
    	uint32_t *otherbuf = (context->regs[REG_MODE_4] & BIT_INTERLACE) ? context->evenbuf : (uint32_t *)blankbuf;
    	uint32_t * oddbuf = context->oddbuf;
    	buf_32 = (uint32_t *)screen->pixels;
    	for (int y = 0; y < 240; y++) {
    		for (int i = 0; i < repeat_y; i++,buf_32 += screen->pitch/4) {
        		uint32_t *line = buf_32;
        		uint32_t *src_line = (i & othermask ? otherbuf : oddbuf) + y * 320;
		    	for (int x = 0; x < 320; x++) {
		    		uint32_t color = *(src_line++);
		    		for (int j = 0; j < repeat_x; j++) {
		    			*(line++) = color;
		    		}
		    	}
		    }
    	}
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

int render_joystick_num_buttons(int joystick)
{
	if (joystick >= num_joysticks) {
		return 0;
	}
	return SDL_JoystickNumButtons(joysticks[joystick]);
}

int render_joystick_num_hats(int joystick)
{
	if (joystick >= num_joysticks) {
		return 0;
	}
	return SDL_JoystickNumHats(joysticks[joystick]);
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
	case SDL_JOYBUTTONDOWN:
		handle_joydown(event->jbutton.which, event->jbutton.button);
		break;
	case SDL_JOYBUTTONUP:
		handle_joyup(event->jbutton.which, event->jbutton.button);
		break;
	case SDL_JOYHATMOTION:
		handle_joy_dpad(event->jbutton.which, event->jhat.hat, event->jhat.value);
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


