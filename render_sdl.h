#ifndef RENDER_SDL_H_
#define RENDER_SDL_H_

#include <SDL.h>

SDL_Window *render_get_window(void);
typedef void (*event_handler)(SDL_Event *);
void render_update_display(void);
void render_set_event_handler(event_handler handler);
SDL_Joystick *render_get_joystick(int index);
SDL_GameController *render_get_controller(int index);
int render_lookup_button(char *name);
int render_lookup_axis(char *name);
void render_enable_gamepad_events(uint8_t enabled);

#endif //RENDER_SDL_H_
