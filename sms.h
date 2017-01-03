#ifndef SMS_H_
#define SMS_H_

#include "system.h"
#include "vdp.h"
#include "psg.h"
#include "z80_to_x86.h"
#include "io.h"

#define SMS_RAM_SIZE (8*1024)

typedef struct {
	system_header header;
	z80_context   *z80;
	vdp_context   *vdp;
	psg_context   *psg;
	sega_io       io;
	uint8_t       *rom;
	uint32_t      rom_size;
	uint32_t      master_clock;
	uint32_t      normal_clock;
	uint8_t       should_return;
	uint8_t       ram[SMS_RAM_SIZE];
} sms_context;

sms_context *alloc_configure_sms(void *rom, uint32_t rom_size, void *extra_rom, uint32_t extra_rom_size, uint32_t opts, uint8_t force_region, rom_info *info_out);

#endif //SMS_H_
