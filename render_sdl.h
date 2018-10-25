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
SDL_Joystick *render_get_joystick(int index);
SDL_GameController *render_get_controller(int index);
int render_lookup_button(char *name);
int render_lookup_axis(char *name);

#endif //RENDER_SDL_H_
