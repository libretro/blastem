#ifndef SYSTEM_H_
#define SYSTEM_H_
#include <stdint.h>
#include "arena.h"
#include "romdb.h"

typedef struct system_header system_header;

typedef enum {
	SYSTEM_UNKNOWN,
	SYSTEM_GENESIS,
	SYSTEM_SMS,
	SYSTEM_JAGUAR
} system_type;

typedef enum {
	DEBUGGER_NATIVE,
	DEBUGGER_GDB
} debugger_type;

typedef void (*system_fun)(system_header *);
typedef void (*start_system_fun)(system_header *, char *);
typedef void (*speed_system_fun)(system_header *, uint32_t);

struct system_header {
	system_header     *next_context;
	start_system_fun  start_context;
	system_fun        resume_context;
	system_fun        load_save;
	system_fun        persist_save;
	system_fun        request_exit;
	system_fun        free_context;
	speed_system_fun  set_speed_percent;
	system_fun        inc_debug_mode;
	system_fun        inc_debug_pal;
	arena             *arena;
	char              *next_rom;
	char              *save_dir;
	uint8_t           enter_debugger;
	uint8_t           should_exit;
	uint8_t           save_state;
	debugger_type     debugger_type;
	system_type       type;
};

system_type detect_system_type(uint8_t *rom, long filesize);
system_header *alloc_config_system(system_type stype, void *rom, uint32_t rom_size, void *lock_on, uint32_t lock_on_size, uint32_t opts, uint8_t force_region, rom_info *info_out);

#endif //SYSTEM_H_
