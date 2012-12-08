#include <SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include "render.h"

SDL_Surface *screen;

void render_init()
{
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Unable to init SDL: %s\n", SDL_GetError());
        exit(1);
    }
    atexit(SDL_Quit);
    screen = SDL_SetVideoMode(320, 240, 32, SDL_SWSURFACE | SDL_ANYFORMAT);
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
    switch (screen->format->BytesPerPixel) {
    case 2:
        buf_16 = (uint16_t *)screen->pixels;
        for (int y = 0; y < 240; y++, buf_16 += (screen->pitch/2 - 320)) {
        	for (int x = 0; x < 320; x++, buf_16++) {
        		uint16_t gen_color = context->framebuf[y * 320 + x];
        		b = ((gen_color >> 8) & 0xE) * 18;
        		g = ((gen_color >> 4) & 0xE) * 18;
        		r = (gen_color& 0xE) * 18;
        		*buf_16 = SDL_MapRGB(screen->format, r, g, b);
        	}
        }
    	break;
    case 3:
        buf_8 = (uint8_t *)screen->pixels;
        for (int y = 0; y < 240; y++, buf_8 += (screen->pitch - 320)) {
        	for (int x = 0; x < 320; x++, buf_8 += 3) {
        		uint16_t gen_color = context->framebuf[y * 320 + x];
        		b = ((gen_color >> 8) & 0xE) * 18;
        		g = ((gen_color >> 4) & 0xE) * 18;
        		r = (gen_color& 0xE) * 18;
				*(buf_8+screen->format->Rshift/8) = r;
				*(buf_8+screen->format->Gshift/8) = g;
				*(buf_8+screen->format->Bshift/8) = b;
        	}
        }
    	break;
    case 4:
        buf_32 = (uint32_t *)screen->pixels;
        for (int y = 0; y < 224; y++, buf_32 += (screen->pitch/4 - 320)) {
        	for (int x = 0; x < 320; x++, buf_32++) {
        		uint16_t gen_color = context->framebuf[y * 320 + x];
        		b = ((gen_color >> 8) & 0xE) * 18;
        		g = ((gen_color >> 4) & 0xE) * 18;
        		r = (gen_color& 0xE) * 18;
        		*buf_32 = SDL_MapRGB(screen->format, r, g, b);
        	}
        }
        for (int y = 224; y < 240; y++, buf_32 += (screen->pitch/4 - 320)) {
        	for (int x = 0; x < 320; x++, buf_32++) {
        		uint16_t gen_color = context->cram[x/10 + ((y-224)/8)*32];
        		b = ((gen_color >> 8) & 0xE) * 18;
        		g = ((gen_color >> 4) & 0xE) * 18;
        		r = (gen_color& 0xE) * 18;
        		*buf_32 = SDL_MapRGB(screen->format, r, g, b);
        	}
        }
    	break;
    }
    if ( SDL_MUSTLOCK(screen) ) {
        SDL_UnlockSurface(screen);
    }
    SDL_UpdateRect(screen, 0, 0, 320, 240);
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

