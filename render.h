#ifndef RENDER_SDL_H_
#define RENDER_SDL_H_

#include "vdp.h"
#include "psg.h"
void render_init(int width, int height, char * title, uint32_t fps);
void render_context(vdp_context * context);
void render_wait_quit(vdp_context * context);
void render_wait_audio(psg_context * context);
int wait_render_frame(vdp_context * context, int frame_limit);
void render_fps(uint32_t fps);
uint32_t render_audio_buffer();
uint32_t render_sample_rate();

#endif //RENDER_SDL_H_

