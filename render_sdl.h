#ifndef RENDER_SDL_H_
#define RENDER_SDL_H_

#include <SDL.h>

SDL_Window *render_get_window(void);
typedef void (*ui_render_fun)(void);
typedef void (*event_handler)(SDL_Event *);
void render_update_display(void);
void render_set_ui_render_fun(ui_render_fun);
void render_set_event_handler(event_handler handler);
void render_set_gl_context_handlers(ui_render_fun destroy, ui_render_fun create);

#endif //RENDER_SDL_H_
