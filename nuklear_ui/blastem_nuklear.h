#ifndef BLASTEM_NUKLEAR_H_
#define BLASTEM_NUKLEAR_H_

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#include <SDL_opengl.h>
#include "nuklear.h"
#include "nuklear_sdl_gles2.h"

void blastem_nuklear_init(uint8_t file_loaded);

#endif //BLASTEM_NUKLEAR_H_
