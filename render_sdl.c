#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "render.h"

SDL_Surface *screen;

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
}

void render_context(vdp_context * context)
{
	uint8_t *buf_8;
	uint16_t *buf_16;
	uint32_t *buf_32; 
	uint8_t b,g,r;
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
    printf("w: %d, h: %d, repeat_x: %d, repeat_y: %d\n", screen->clip_rect.w, screen->clip_rect.h, repeat_x, repeat_y);
    switch (screen->format->BytesPerPixel) {
    case 2:
        buf_16 = (uint16_t *)screen->pixels;
        for (int y = 0; y < 240; y++) {
        	for (int i = 0; i < repeat_y; i++,buf_16 += screen->pitch/2) {
        		uint16_t *line = buf_16;
		    	for (int x = 0; x < 320; x++) {
		    		uint16_t gen_color = context->framebuf[y * 320 + x];
		    		b = ((gen_color >> 8) & 0xE) * 18;
		    		g = ((gen_color >> 4) & 0xE) * 18;
		    		r = (gen_color& 0xE) * 18;
		    		for (int j = 0; j < repeat_x; j++) {
		    			*(line++) = SDL_MapRGB(screen->format, r, g, b);
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
		    	for (int x = 0; x < 320; x++) {
		    		uint16_t gen_color = context->framebuf[y * 320 + x];
		    		b = ((gen_color >> 8) & 0xE) * 18;
		    		g = ((gen_color >> 4) & 0xE) * 18;
		    		r = (gen_color& 0xE) * 18;
		    		for (int j = 0; j < repeat_x; j++) {
		    			*(line++) = SDL_MapRGB(screen->format, r, g, b);
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

void render_wait_quit()
{
	SDL_Event event;
	while(SDL_WaitEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			return;
		}
	}
}

