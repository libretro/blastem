#ifndef SYSTEM_H_
#define SYSTEM_H_
#include <stdint.h>

typedef struct system_header system_header;
typedef struct system_media system_media;

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
typedef uint16_t (*system_fun_r16)(system_header *);
typedef void (*system_str_fun)(system_header *, char *);
typedef uint8_t (*system_str_fun_r8)(system_header *, char *);
typedef void (*speed_system_fun)(system_header *, uint32_t);
typedef uint8_t (*system_u8_fun_r8)(system_header *, uint8_t);

#include "arena.h"
#include "romdb.h"

struct system_header {
	system_header     *next_context;
	system_str_fun    start_context;
	system_fun        resume_context;
	system_fun        load_save;
	system_fun        persist_save;
	system_u8_fun_r8  load_state;
	system_fun        request_exit;
	system_fun        soft_reset;
	system_fun        free_context;
	system_fun_r16    get_open_bus_value;
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

struct system_media {
	void         *buffer;
	char         *dir;
	char         *name;
	char         *extension;
	system_media *chain;
	uint32_t     size;
};

#define OPT_ADDRESS_LOG (1U << 31U)

system_type detect_system_type(system_media *media);
system_header *alloc_config_system(system_type stype, system_media *media, uint32_t opts, uint8_t force_region, rom_info *info_out);

#endif //SYSTEM_H_
