#ifndef RENDER_SDL_H_
#define RENDER_SDL_H_

#include "vdp.h"
void render_init();
void render_context(vdp_context * context);
void render_wait_quit();

#endif //RENDER_SDL_H_

