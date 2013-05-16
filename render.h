#ifndef RENDER_SDL_H_
#define RENDER_SDL_H_

#include "vdp.h"
void render_init(int width, int height, char * title);
void render_context(vdp_context * context);
void render_wait_quit(vdp_context * context);
int wait_render_frame(vdp_context * context, int frame_limit);
void render_fps(uint32_t fps);

#endif //RENDER_SDL_H_

