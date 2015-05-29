/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef RENDER_H_
#define RENDER_H_

//TODO: Throw an ifdef in here once there's more than one renderer
#include <SDL.h>
#define RENDERKEY_UP      SDLK_UP
#define RENDERKEY_DOWN    SDLK_DOWN
#define RENDERKEY_LEFT    SDLK_LEFT
#define RENDERKEY_RIGHT   SDLK_RIGHT
#define RENDERKEY_ESC     SDLK_ESCAPE
#define RENDERKEY_LSHIFT  SDLK_LSHIFT
#define RENDERKEY_RSHIFT  SDLK_RSHIFT
#define RENDER_DPAD_UP    SDL_HAT_UP
#define RENDER_DPAD_DOWN  SDL_HAT_DOWN
#define RENDER_DPAD_LEFT  SDL_HAT_LEFT
#define RENDER_DPAD_RIGHT SDL_HAT_RIGHT

#define MAX_JOYSTICKS 8

#include "vdp.h"
#include "psg.h"
#include "ym2612.h"

typedef struct {
	void *oddbuf;
	void *evenbuf;
	int  stride;
} surface_info;

uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b);
void render_alloc_surfaces(vdp_context * context);
void render_init(int width, int height, char * title, uint32_t fps, uint8_t fullscreen);
void render_context(vdp_context * context);
void render_wait_quit(vdp_context * context);
void render_wait_psg(psg_context * context);
void render_wait_ym(ym2612_context * context);
int wait_render_frame(vdp_context * context, int frame_limit);
void render_fps(uint32_t fps);
uint32_t render_audio_buffer();
uint32_t render_sample_rate();
void render_debug_mode(uint8_t mode);
void render_debug_pal(uint8_t pal);
void process_events();
int render_joystick_num_buttons(int joystick);
int render_joystick_num_hats(int joystick);
int render_num_joysticks();
void process_events();



#endif //RENDER_H_

