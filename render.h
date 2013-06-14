#ifndef RENDER_H_
#define RENDER_H_

#include "vdp.h"
#include "psg.h"
#include "ym2612.h"
void render_init(int width, int height, char * title, uint32_t fps);
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

//TODO: Throw an ifdef in here once there's more than one renderer
#include <SDL.h>
#define RENDERKEY_UP    SDLK_UP
#define RENDERKEY_DOWN  SDLK_DOWN
#define RENDERKEY_LEFT  SDLK_LEFT
#define RENDERKEY_RIGHT SDLK_RIGHT

#endif //RENDER_H_

