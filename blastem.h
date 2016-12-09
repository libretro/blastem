#ifndef BLASTEM_H_
#define BLASTEM_H_

#include "tern.h"

extern int headless;
extern int exit_after;
extern int z80_enabled;
extern int frame_limit;

extern tern_node * config;

extern char *save_state_path;
#define QUICK_SAVE_SLOT 10

#endif //BLASTEM_H_
