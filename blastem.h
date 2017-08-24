#ifndef BLASTEM_H_
#define BLASTEM_H_

#include "tern.h"
#include "system.h"

extern int headless;
extern int exit_after;
extern int z80_enabled;
extern int frame_limit;

extern tern_node * config;
extern system_header *current_system;

extern char *save_state_path;
extern char *save_filename;
extern uint8_t use_native_states;
#define QUICK_SAVE_SLOT 10
void reload_media(void);
void lockon_media(char *lock_on_path);

#endif //BLASTEM_H_
