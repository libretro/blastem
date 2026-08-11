#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cd_graphics.h"
#include "genesis.h"
#include "paths.h"
#include "debug.h"
#include "gdb_remote.h"
#include "blastem.h"
#include "cdimage.h"
#include "media_file.h"

#ifdef DO_DEBUG_PRINT
#define dprintf printf
#else
#define dprintf
#endif

#define SCD_MCLKS 50000000
#define SCD_PERIPH_RESET_CLKS (SCD_MCLKS / 10)
#define TIMER_TICK_CLKS 1536/*1792*/

//TODO: do some logic analyzer captuers to get actual values
#define REFRESH_INTERVAL 259
#define REFRESH_DELAY 2

#ifdef NEW_CORE
#define int_num int_priority
#endif

enum {
	GA_SUB_CPU_CTRL,
	GA_MEM_MODE,
	GA_CDC_CTRL,
	GA_CDC_REG_DATA,
	GA_CDC_HOST_DATA,
	GA_CDC_DMA_ADDR,
	GA_STOP_WATCH,
	GA_COMM_FLAG,
	GA_COMM_CMD0,
	GA_COMM_CMD1,
	GA_COMM_CMD2,
	GA_COMM_CMD3,
	GA_COMM_CMD4,
	GA_COMM_CMD5,
	GA_COMM_CMD6,
	GA_COMM_CMD7,
	GA_COMM_STATUS0,
	GA_COMM_STATUS1,
	GA_COMM_STATUS2,
	GA_COMM_STATUS3,
	GA_COMM_STATUS4,
	GA_COMM_STATUS5,
	GA_COMM_STATUS6,
	GA_COMM_STATUS7,
	GA_TIMER,
	GA_INT_MASK,
	GA_CDD_FADER,
	GA_CDD_CTRL,
	GA_CDD_STATUS0,
	GA_CDD_STATUS1,
	GA_CDD_STATUS2,
	GA_CDD_STATUS3,
	GA_CDD_STATUS4,
	GA_CDD_CMD0,
	GA_CDD_CMD1,
	GA_CDD_CMD2,
	GA_CDD_CMD3,
	GA_CDD_CMD4,
	GA_FONT_COLOR,
	GA_FONT_BITS,
	GA_FONT_DATA0,
	GA_FONT_DATA1,
	GA_FONT_DATA2,
	GA_FONT_DATA3,
	GA_SUBCODE_START = 0x80,
	GA_SUBCODE_MIRROR = 0xC0,

	GA_HINT_VECTOR = GA_CDC_REG_DATA
};
//GA_SUB_CPU_CTRL
#define BIT_IEN2       0x8000
#define BIT_IFL2       0x0100
#define BIT_LEDG       0x0200
#define BIT_LEDR       0x0100
#define BIT_SBRQ       0x0002
#define BIT_SRES       0x0001
#define BIT_PRES       0x0001
//GA_MEM_MODE
#define MASK_PROG_BANK 0x00C0
#define BIT_OVERWRITE  0x0010
#define BIT_UNDERWRITE 0x0008
#define MASK_PRIORITY  (BIT_OVERWRITE|BIT_UNDERWRITE)
#define BIT_MEM_MODE   0x0004
#define BIT_DMNA       0x0002
#define BIT_RET        0x0001

//GA_CDC_CTRL
#define BIT_EDT        0x8000
#define BIT_DSR        0x4000

//GA_CDD_CTRL
#define BIT_MUTE       0x0100

enum {
	DST_MAIN_CPU = 2,
	DST_SUB_CPU,
	DST_PCM_RAM,
	DST_PROG_RAM,
	DST_WORD_RAM = 7
};

//GA_INT_MASK
#define BIT_MASK_IEN1  0x0002
#define BIT_MASK_IEN2  0x0004
#define BIT_MASK_IEN3  0x0008
#define BIT_MASK_IEN4  0x0010
#define BIT_MASK_IEN5  0x0020
#define BIT_MASK_IEN6  0x0040

//GA_CDD_CTRL
#define BIT_HOCK       0x0004

static void *prog_ram_wp_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	//if (!(cd->gate_array[GA_MEM_MODE] & (1 << ((address >> 9) + 8)))) {
	if (address >= ((cd->gate_array[GA_MEM_MODE] & 0xFF00) << 1)) {
		cd->prog_ram[address >> 1] = value;
		m68k_invalidate_code_range(m68k, address, address + 2);
	}
	return vcontext;
}

static void *prog_ram_wp_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	if (address >= ((cd->gate_array[GA_MEM_MODE] & 0xFF00) << 1)) {
		((uint8_t *)cd->prog_ram)[address ^ 1] = value;
		m68k_invalidate_code_range(m68k, address, address + 1);
	}
	return vcontext;
}

static uint16_t word_ram_2M_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	uint16_t* bank = m68k->mem_pointers[1];
	if (!bank) {
		return 0xFFFF;
	}
	uint16_t raw = bank[address >> 1 & ~1];
	if (address & 2) {
		return (raw & 0xF) | (raw << 4 & 0xF00);
	} else {
		return (raw >> 4 & 0xF00) | (raw >> 8 & 0xF);
	}
}

static uint8_t word_ram_2M_read8(uint32_t address, void *vcontext)
{
	uint16_t word = word_ram_2M_read16(address, vcontext);
	if (address & 1) {
		return word;
	}
	return word >> 8;
}

static void *word_ram_2M_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	if (!(cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE)) {
		//TODO: Confirm this first write goes through (seemed like it in initial testing)
		if (address & 1) {
			address >>= 1;
			cd->word_ram[address] &= 0xFF00;
			cd->word_ram[address] |= value;
		} else {
		address >>= 1;
			cd->word_ram[address] &= 0x00FF;
			cd->word_ram[address] |= value << 8;
		}
		m68k_invalidate_code_range(cd->genesis->m68k, cd->base + 0x200000 + (address & ~1), cd->base + 0x200000 + (address & ~1) + 1);
		cd->sub_paused_wordram = 1;
		m68k->sync_cycle = m68k->target_cycle = m68k->cycles;
		m68k->should_return = 1;
	} else {
		value &= 0xF;
		uint16_t priority = cd->gate_array[GA_MEM_MODE] & MASK_PRIORITY;

		if (priority == BIT_OVERWRITE && !value) {
			return vcontext;
		}
		if (priority == BIT_UNDERWRITE) {
			if (!value) {
				return vcontext;
			}
			uint8_t old = word_ram_2M_read8(address, vcontext);
			if (old) {
				return vcontext;
			}
		}
		uint16_t* bank = m68k->mem_pointers[1];
		if (!bank) {
			return vcontext;
		}
		uint16_t raw = bank[address >> 1 & ~1];
		uint16_t shift = ((address & 3) * 4);
		raw &= ~(0xF000 >> shift);
		raw |= value << (12 - shift);
		bank[address >> 1 & ~1] = raw;
	}
	return vcontext;
}


static void *word_ram_2M_write16(uint32_t address, void *vcontext, uint16_t value)
{
	word_ram_2M_write8(address, vcontext, value >> 8);
	return word_ram_2M_write8(address + 1, vcontext, value);
}

static uint16_t word_ram_1M_read16(uint32_t address, void *vcontext)
{
	//TODO: check behavior for these on hardware
	return 0;
}

static uint8_t word_ram_1M_read8(uint32_t address, void *vcontext)
{
	return 0;
}

static void *word_ram_1M_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *word_ram_1M_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}


static uint16_t unmapped_prog_read16(uint32_t address, void *vcontext)
{
	return 0xFFFF;
}

static uint8_t unmapped_prog_read8(uint32_t address, void *vcontext)
{
	return 0xFF;
}

static void *unmapped_prog_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return vcontext;
}

static void *unmapped_prog_write8(uint32_t address, void *vcontext, uint8_t value)
{
	return vcontext;
}

static uint16_t unmapped_word_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		return cd->word_ram[address + cd->bank_toggle];
	} else {
		return 0xFFFF;
	}
}

static uint8_t unmapped_word_read8(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		if (address & 1) {
			return cd->word_ram[(address & ~1) + cd->bank_toggle];
		} else {
			return cd->word_ram[address + cd->bank_toggle] >> 8;
		}
	} else {
		return 0xFF;
	}
}

static void *unmapped_word_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		cd->word_ram[address + cd->bank_toggle] = value;
		m68k_invalidate_code_range(m68k, cd->base + 0x200000 + address, cd->base + 0x200000 + address + 1);
	}
	return vcontext;
}

static void *unmapped_word_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		if (address & 1) {
			uint32_t offset = (address & ~1) + cd->bank_toggle;
			cd->word_ram[offset] &= 0xFF00;
			cd->word_ram[offset] |= value;
		} else {
			cd->word_ram[address + cd->bank_toggle] &= 0xFF;
			cd->word_ram[address + cd->bank_toggle] |= value << 8;
		}
		m68k_invalidate_code_range(m68k, cd->base + 0x200000 + (address & ~1), cd->base + 0x200000 + (address & ~1) + 1);
	}
	return vcontext;
}

static uint32_t cell_image_translate_address(uint32_t address)
{
	uint32_t word_of_cell = address & 2;
	if (address < 0x10000) {
		//64x32 cell view
		uint32_t line_of_column = address & 0x3FC;
		uint32_t column = address & 0xFC00;
		address = (line_of_column << 6) | (column >> 8) | word_of_cell;
	} else if (address < 0x18000) {
		//64x16 cell view
		uint32_t line_of_column = address & 0x1FC;
		uint32_t column = address & 0x7E00;
		address = 0x10000 | (line_of_column << 6) | (column >> 7) | word_of_cell;
	} else if (address < 0x1C000) {
		//64x8 cell view
		uint32_t line_of_column = address & 0x00FC;
		uint32_t column = address & 0x3F00;
		address = 0x18000 | (line_of_column << 6) | (column >> 6) | word_of_cell;
	} else {
		//64x4 cell view
		uint32_t line_of_column = address & 0x007C;
		uint32_t column = address & 0x1F80;
		address &= 0x1E000;
		address |= (line_of_column << 6) | (column >> 5) | word_of_cell;
	}
	return address;
}

static uint16_t cell_image_read16(uint32_t address, void *vcontext)
{
	address = cell_image_translate_address(address);
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (!(cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE)) {
		return 0xFFFF;
	}
	return cd->word_ram[address + cd->bank_toggle];
}

static uint8_t cell_image_read8(uint32_t address, void *vcontext)
{
	uint16_t word = cell_image_read16(address & 0xFFFFFE, vcontext);
	if (address & 1) {
		return word;
	}
	return word >> 8;
}

static void *cell_image_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		address = cell_image_translate_address(address);
		cd->word_ram[address + cd->bank_toggle] = value;
		m68k_invalidate_code_range(m68k, cd->base + 0x200000 + address, cd->base + 0x200000 + address + 1);
	}
	return vcontext;
}

static void *cell_image_write8(uint32_t address, void *vcontext, uint8_t value)
{
	uint32_t byte = address & 1;
	address = cell_image_translate_address(address);
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		if (byte) {
			cd->word_ram[address + cd->bank_toggle] &= 0xFF00;
			cd->word_ram[address + cd->bank_toggle] |= value;
		} else {
			cd->word_ram[address + cd->bank_toggle] &= 0x00FF;
			cd->word_ram[address + cd->bank_toggle] |= value << 8;
		}
		m68k_invalidate_code_range(m68k, cd->base + 0x200000 + address, cd->base + 0x200000 + address + 1);
	}
	return vcontext;
}

static void cdd_run(segacd_context *cd, uint32_t cycle)
{
	cdd_mcu_run(&cd->cdd, cycle, cd->gate_array + GA_CDD_CTRL, &cd->cdc, &cd->fader);
	lc8951_run(&cd->cdc, cycle);
}

static uint8_t pcm_read8(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	if (address & 1) {
		//need to run CD drive because there may be a PCM DMA underway
		cdd_run(cd, m68k->cycles);
		rf5c164_run(&cd->pcm, m68k->cycles);
		return rf5c164_read(&cd->pcm, address >> 1);
	} else {
		return 0xFF;
	}
}

static uint16_t pcm_read16(uint32_t address, void *vcontext)
{
	return 0xFF00 | pcm_read8(address+1, vcontext);
}

static void *pcm_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	if (address & 1) {
		//need to run CD drive because there may be a PCM DMA underway
		cdd_run(cd, m68k->cycles);
		rf5c164_run(&cd->pcm, m68k->cycles);
		while ((cd->pcm.flags & 0x81) == 1) {
			//not sounding, but pending write
			//DMA write conflict presumably adds wait states
			m68k->cycles += 4;
			rf5c164_run(&cd->pcm, m68k->cycles);
		}
		rf5c164_write(&cd->pcm, address >> 1, value);
	}
	return vcontext;
}

static void *pcm_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return pcm_write8(address+1, vcontext, value);
}

static uint16_t cart_area_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
#ifdef NEW_CORE
	uint16_t open_bus = m68k->prefetch;
#else
	uint16_t open_bus = read_word(m68k->last_prefetch_address, (void **)m68k->mem_pointers, &m68k->opts->gen, m68k);
#endif
	if (cd->bram_cart_id > 7) {
		// No cart, just return open bus
		return open_bus;
	}
	address &= 0x3FFFFF;
	if (address < 0x200000) {
		if (address < 0x100000) {
			return (open_bus & 0xFF00) | cd->bram_cart_id;
		}
		return open_bus;
	} else {
		address &= 0x1FFFFF;
		uint32_t end = 0x2000 << (1 + cd->bram_cart_id);
		if (address >= end) {
			return open_bus;
		}
		return (open_bus & 0xFF00) | cd->bram_cart[address >> 1];
	}
}

static uint8_t cart_area_read8(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (!(address & 1) || cd->bram_cart_id > 7) {
		//open bus
#ifdef NEW_CORE
		return (address & 1) ? m68k->prefetch : m68k->prefetch >> 8;
#else
		return read_byte(m68k->last_prefetch_address | (address & 1), (void **)m68k->mem_pointers, &m68k->opts->gen, m68k);
#endif
	}
	address &= 0x3FFFFF;
	if (address < 0x200000) {
		if (address < 0x100000) {
			return cd->bram_cart_id;
		}
#ifdef NEW_CORE
		return m68k->prefetch;
#else
		return read_byte(m68k->last_prefetch_address | 1, (void **)m68k->mem_pointers, &m68k->opts->gen, m68k);
#endif
	} else {
		address &= 0x1FFFFF;
		uint32_t end = 0x2000 << (1 + cd->bram_cart_id);
		if (address >= end) {
#ifdef NEW_CORE
			return m68k->prefetch;
#else
			return read_byte(m68k->last_prefetch_address | 1, (void **)m68k->mem_pointers, &m68k->opts->gen, m68k);
#endif
		}
		return cd->bram_cart[address >> 1];
	}
}

static void *cart_area_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	if (!(address & 1) || cd->bram_cart_id > 7 || address < 0x600000) {
		return vcontext;
	}
	address &= 0x1FFFFF;
	uint32_t end = 0x2000 << (1 + cd->bram_cart_id);
	if (address < end && cd->bram_cart_write_enabled) {
		cd->bram_cart[address >> 1] = value;
	}
	if (address == 0x1FFFFF || (cd->bram_cart_id < 7 && address > 0x100000)) {
		cd->bram_cart_write_enabled = value & 1;
	}
	return vcontext;
}

static void *cart_area_write16(uint32_t address, void *vcontext, uint16_t value)
{
	return cart_area_write8(address | 1, vcontext, value);
}

static void timers_run(segacd_context *cd, uint32_t cycle)
{
	if (cycle <= cd->stopwatch_cycle) {
		return;
	}
	uint32_t ticks = (cycle - cd->stopwatch_cycle) / TIMER_TICK_CLKS;
	cd->stopwatch_cycle += ticks * TIMER_TICK_CLKS;
	cd->gate_array[GA_STOP_WATCH] += ticks;
	cd->gate_array[GA_STOP_WATCH] &= 0xFFF;
	if (ticks && !cd->timer_value) {
		--ticks;
		cd->timer_value = cd->gate_array[GA_TIMER];
	}
	if (ticks && cd->timer_value) {
		while (ticks >= (cd->timer_value + 1)) {
			ticks -= cd->timer_value + 1;
			cd->timer_value = cd->gate_array[GA_TIMER];
			cd->timer_pending = 1;
		}
		cd->timer_value -= ticks;
		if (!cd->timer_value) {
			cd->timer_pending = 1;
		}
	}
}

static uint32_t next_timer_int(segacd_context *cd)
{
	if (cd->timer_pending) {
		return cd->m68k->cycles;
	}
	if (cd->timer_value) {
		return cd->stopwatch_cycle + TIMER_TICK_CLKS * cd->timer_value;
	}
	if (cd->gate_array[GA_TIMER]) {
		return cd->stopwatch_cycle + TIMER_TICK_CLKS * (cd->gate_array[GA_TIMER] + 1);
	}
	return CYCLE_NEVER;
}

static void calculate_target_cycle(m68k_context * context)
{
	segacd_context *cd = context->system;
	context->int_cycle = CYCLE_NEVER;
	uint8_t mask = context->status & 0x7;
	uint32_t cdc_cycle = CYCLE_NEVER;
	if (mask < 6) {
		if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN6) {
			uint32_t subcode_cycle = cd->cdd.subcode_int_pending ? context->cycles : cd->cdd.next_subcode_int_cycle;
			if (subcode_cycle != CYCLE_NEVER) {
				context->int_cycle = subcode_cycle;
				context->int_num = 6;
			}
		}
		if (mask < 5) {
			if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN5) {
				cdc_cycle = lc8951_next_interrupt(&cd->cdc);
#ifdef NEW_CORE
				//should this maybe happen with the old core too?
				if (cdc_cycle == cd->cdc.cycle) {
					cdc_cycle = context->cycles;
				}
#endif
				//CDC interrupts only generated on falling edge of !INT signal
				if (cd->cdc_int_ack) {
					if (cdc_cycle > cd->cdc.cycle) {
						cd->cdc_int_ack = 0;
					} else {
						cdc_cycle = CYCLE_NEVER;
					}
				}
				if (cdc_cycle < context->int_cycle) {
					context->int_cycle = cdc_cycle;
					context->int_num = 5;
				}
			}
			if (mask < 4) {
				if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN4) {
					uint32_t cdd_cycle = cd->cdd.int_pending ? context->cycles : cd->cdd.next_int_cycle;
					if (cdd_cycle < context->int_cycle) {
						context->int_cycle = cdd_cycle;
						context->int_num = 4;
					}
				}
				if (mask < 3) {
					uint32_t next_timer;
					if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN3) {
						uint32_t next_timer_cycle = next_timer_int(cd);
						if (next_timer_cycle < context->int_cycle) {
							context->int_cycle = next_timer_cycle;
							context->int_num = 3;
						}
					}
					if (mask < 2) {
						if (cd->int2_cycle < context->int_cycle && (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN2)) {
							context->int_cycle = cd->int2_cycle;
							context->int_num = 2;
						}
						if (mask < 1) {
							if (cd->graphics_int_cycle < context->int_cycle && (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN1)) {
								context->int_cycle = cd->graphics_int_cycle;
								context->int_num = 1;
							}
						}
					}
				}
			}
		}
	}
	if (context->int_cycle > context->cycles && context->int_pending == INT_PENDING_SR_CHANGE) {
		context->int_pending = INT_PENDING_NONE;
	}
	if (context->cycles >= context->sync_cycle) {
		context->should_return = 1;
		context->target_cycle = context->cycles + 1;
		return;
	}
	if (context->status & M68K_STATUS_TRACE || context->trace_pending) {
		context->target_cycle = context->cycles + 1;
		return;
	}
	context->target_cycle = context->sync_cycle < context->int_cycle ? context->sync_cycle : context->int_cycle;
	if (context->int_cycle == cdc_cycle && context->int_num == 5) {
		uint32_t before = cdc_cycle - cd->m68k->opts->gen.clock_divider * 158; //divs worst case
		if (before < context->target_cycle) {
			while (before <= context->cycles) {
				before += cd->cdc.clock_step;
			}
			if (before < context->target_cycle) {
				context->target_cycle = context->sync_cycle = before;
			}
		}
	}
	if (context->target_cycle <= context->cycles) {
		context->target_cycle = context->cycles + 1;
	}
}

static uint16_t sub_gate_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t before_cycle = m68k->cycles - m68k->opts->gen.clock_divider * 4;
	if (before_cycle >= cd->last_refresh_cycle) {
		uint32_t num_refresh = (before_cycle - cd->last_refresh_cycle) / REFRESH_INTERVAL;
		uint32_t num_full = (m68k->cycles - cd->last_refresh_cycle) / REFRESH_INTERVAL;
		cd->last_refresh_cycle = cd->last_refresh_cycle + num_full * REFRESH_INTERVAL;
		m68k->cycles += num_refresh * REFRESH_DELAY;
	}
	
	
	uint32_t reg = address >> 1;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL: {
		uint16_t value = cd->gate_array[reg] & 0xFFFE;
		if (cd->periph_reset_cycle == CYCLE_NEVER || (m68k->cycles - cd->periph_reset_cycle) > SCD_PERIPH_RESET_CLKS) {
			value |= BIT_PRES;
		}
		return value;
	}
	case GA_MEM_MODE:
		return cd->gate_array[reg] & 0xFF1F;
	case GA_CDC_CTRL:
		cdd_run(cd, m68k->cycles);
		return cd->gate_array[reg] | cd->cdc.ar;
	case GA_CDC_REG_DATA:
		cdd_run(cd, m68k->cycles);
		return lc8951_reg_read(&cd->cdc);
	case GA_CDC_HOST_DATA: {
		cdd_run(cd, m68k->cycles);
		uint16_t dst = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
		if (dst == DST_SUB_CPU) {
			if (cd->gate_array[GA_CDC_CTRL] & BIT_DSR) {
				cd->gate_array[GA_CDC_CTRL] &= ~BIT_DSR;
				lc8951_resume_transfer(&cd->cdc);
			}
			calculate_target_cycle(cd->m68k);

		}
		return cd->gate_array[reg];
	}
	case GA_STOP_WATCH:
	case GA_TIMER:
		timers_run(cd, m68k->cycles);
		return cd->gate_array[reg];
	case GA_CDD_STATUS0:
	case GA_CDD_STATUS1:
	case GA_CDD_STATUS2:
	case GA_CDD_STATUS3:
	case GA_CDD_STATUS4:
		cdd_run(cd, m68k->cycles);
		return cd->gate_array[reg];
		break;
	case GA_FONT_DATA0:
	case GA_FONT_DATA1:
	case GA_FONT_DATA2:
	case GA_FONT_DATA3: {
		uint16_t shift = 4 * (3 - (reg - GA_FONT_DATA0));
		uint16_t value = 0;
		uint16_t fg = cd->gate_array[GA_FONT_COLOR] >> 4;
		uint16_t bg = cd->gate_array[GA_FONT_COLOR] & 0xF;
		for (int i = 0; i < 4; i++) {
			uint16_t pixel = 0;
			if (cd->gate_array[GA_FONT_BITS] & 1 << (shift + i)) {
				pixel = fg;
			} else {
				pixel = bg;
			}
			value |= pixel << (i * 4);
		}
		return value;
	}
	case GA_STAMP_SIZE:
	case GA_IMAGE_BUFFER_LINES:
		//these two have bits that change based on graphics operations
		cd_graphics_run(cd, m68k->cycles);
		return cd->gate_array[reg];
	case GA_TRACE_VECTOR_BASE:
		//write only
		return 0xFFFF;
	default:
		if (reg >= GA_SUBCODE_MIRROR) {
			return cd->gate_array[GA_SUBCODE_START + (reg & 0x3F)];
		}
		return cd->gate_array[reg];
	}
}

static uint8_t sub_gate_read8(uint32_t address, void *vcontext)
{
	uint16_t val = sub_gate_read16(address, vcontext);
	return address & 1 ? val : val >> 8;
}

static void *sub_gate_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t before_cycle = m68k->cycles - m68k->opts->gen.clock_divider * 4;
	if (before_cycle >= cd->last_refresh_cycle) {
		uint32_t num_refresh = (before_cycle - cd->last_refresh_cycle) / REFRESH_INTERVAL;
		uint32_t num_full = (m68k->cycles - cd->last_refresh_cycle) / REFRESH_INTERVAL;
		cd->last_refresh_cycle = cd->last_refresh_cycle + num_full * REFRESH_INTERVAL;
		m68k->cycles += num_refresh * REFRESH_DELAY;
	}
	
	uint32_t reg = address >> 1;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL:
		cd->gate_array[reg] &= 0xF0;
		cd->gate_array[reg] |= value & (BIT_LEDG|BIT_LEDR);
		if (value & BIT_PRES) {
			cd->periph_reset_cycle = m68k->cycles;
		}
		break;
	case GA_MEM_MODE: {
		uint16_t changed = value ^ cd->gate_array[reg];
		uint8_t old_main_has_word2m = cd->main_has_word2m;
		if (value & BIT_RET) {
			cd->main_has_word2m = 1;
		}
		uint8_t old_bank_toggle = cd->bank_toggle;
		cd->bank_toggle = value & BIT_RET;
		genesis_context *gen = cd->genesis;
		cd->gate_array[reg] &= 0xFFC0;
		if (changed & BIT_MEM_MODE) {
			cd->main_swap_request = cd->bank_toggle && !old_bank_toggle;
			if (value & BIT_MEM_MODE) {
				//switch to 1M mode
				gen->m68k->mem_pointers[cd->memptr_start_index + 1] = NULL; //(value & BIT_RET) ? cd->word_ram + 0x10000 : cd->word_ram;
				gen->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
				m68k->mem_pointers[0] = NULL;
				m68k->mem_pointers[1] = cd->bank_toggle ? cd->word_ram : cd->word_ram + 1;
				cd->gate_array[reg] |= value & (MASK_PRIORITY|BIT_RET|BIT_MEM_MODE);
				if (cd->main_swap_request) {
					cd->gate_array[reg] |= BIT_DMNA;
				}
			} else {
				//switch to 2M mode
				if (cd->main_has_word2m) {
					//Main CPU will have word ram
					gen->m68k->mem_pointers[cd->memptr_start_index + 1] = cd->word_ram;
					gen->m68k->mem_pointers[cd->memptr_start_index + 2] = cd->word_ram + 0x10000;
					m68k->mem_pointers[0] = NULL;
					m68k->mem_pointers[1] = NULL;
				} else {
					//sub cpu will have word ram
					gen->m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
					gen->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
					m68k->mem_pointers[0] = cd->word_ram;
					m68k->mem_pointers[1] = NULL;
				}
				cd->gate_array[reg] |= value & (MASK_PRIORITY|BIT_MEM_MODE);
				cd->gate_array[reg] |= cd->main_has_word2m ? BIT_RET : BIT_DMNA;
			}
			m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
			m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
		} else if (value & BIT_MEM_MODE) {
			//1M mode
			if (old_bank_toggle != cd->bank_toggle) {
				m68k->mem_pointers[1] = (value & BIT_RET) ? cd->word_ram : cd->word_ram + 1;
				m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
				cd->main_swap_request = 0;
			}
			cd->gate_array[reg] |= value & (MASK_PRIORITY|BIT_RET|BIT_MEM_MODE);
			if (cd->main_swap_request) {
				cd->gate_array[reg] |= BIT_DMNA;
			}
		} else {
			//2M mode
			if (old_main_has_word2m != cd->main_has_word2m) {
				gen->m68k->mem_pointers[cd->memptr_start_index + 1] = cd->word_ram;
				gen->m68k->mem_pointers[cd->memptr_start_index + 2] = cd->word_ram + 0x10000;
				m68k->mem_pointers[0] = NULL;
				m68k_invalidate_code_range(gen->m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(m68k, 0x080000, 0x0E0000);
			}
			cd->gate_array[reg] |= value & MASK_PRIORITY;
			cd->gate_array[reg] |= cd->main_has_word2m ? BIT_RET : BIT_DMNA;
		}
		break;
	}
	case GA_CDC_CTRL: {
		cdd_run(cd, m68k->cycles);
		lc8951_ar_write(&cd->cdc, value);
		//cd->gate_array[reg] &= 0xC000;
		uint16_t old_dest = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
		//clears both EDT and DSR
		cd->gate_array[reg] = value & 0x0700;
		uint16_t dest = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
		if (dest != old_dest) {
			if (dest == DST_PCM_RAM) {
				lc8951_set_dma_multiple(&cd->cdc, 21);
			} else {
				lc8951_set_dma_multiple(&cd->cdc, 6);
			}
			if ((old_dest < DST_MAIN_CPU || old_dest == 6) && dest >= DST_MAIN_CPU && dest != 6) {
				lc8951_resume_transfer(&cd->cdc);
			}
			calculate_target_cycle(m68k);
		}
		cd->gate_array[GA_CDC_DMA_ADDR] = 0;
		cd->cdc_dst_low = 0;
		break;
	}
	case GA_CDC_REG_DATA:
		cdd_run(cd, m68k->cycles);
		dprintf("CDC write %X: %X @ %u\n", cd->cdc.ar, value, m68k->cycles);
		if (cd->cdc.ar == 6) {
			//this next bit needs hardware confirmation
			cd->cdc_dst_low = 0;
		}
		lc8951_reg_write(&cd->cdc, value);
		if (!lc8951_dtbsy_state(&cd->cdc)) {
			//new transfer has started, this clears EDT
			cd->gate_array[GA_CDC_CTRL] &= ~BIT_EDT;
			//DSR does not seem to be cleared on hardware
			//but doing this seems to fix Penn & Teller's Smoke and Mirrors
			//needs more research
			cd->gate_array[GA_CDC_CTRL] &= ~BIT_DSR;
		}
		calculate_target_cycle(m68k);
		break;
	case GA_CDC_HOST_DATA:
		//writes to this register have the same side effects as reads
		sub_gate_read16(address, vcontext);
		break;
	case GA_CDC_DMA_ADDR:
		cdd_run(cd, m68k->cycles);
		cd->gate_array[reg] = value;
		cd->cdc_dst_low = 0;
		break;
	case GA_STOP_WATCH:
		//docs say you should only write zero to reset
		//mcd-verificator comments suggest any value will reset
		timers_run(cd, m68k->cycles);
		cd->gate_array[reg] = 0;
		break;
	case GA_COMM_FLAG:
		cd->gate_array[reg] &= 0xFF00;
		cd->gate_array[reg] |= value & 0xFF;
		break;
	case GA_COMM_STATUS0:
	case GA_COMM_STATUS1:
	case GA_COMM_STATUS2:
	case GA_COMM_STATUS3:
	case GA_COMM_STATUS4:
	case GA_COMM_STATUS5:
	case GA_COMM_STATUS6:
	case GA_COMM_STATUS7:
		//no effects for these other than saving the value
		cd->gate_array[reg] = value;
		break;
	case GA_TIMER:
		timers_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0xFF;
		cd->timer_value = 0;
		calculate_target_cycle(m68k);
		break;
	case GA_INT_MASK:
		if (!(cd->gate_array[reg] & BIT_MASK_IEN6)) {
			//subcode interrupts can't be made pending when they are disabled in this reg
			cd->cdd.subcode_int_pending = 0;
		}
		cd->gate_array[reg] = value & (BIT_MASK_IEN6|BIT_MASK_IEN5|BIT_MASK_IEN4|BIT_MASK_IEN3|BIT_MASK_IEN2|BIT_MASK_IEN1);
		calculate_target_cycle(m68k);
		break;
	case GA_CDD_FADER:
		cdd_run(cd, m68k->cycles);
		value &= 0x7FFF;
		cdd_fader_attenuation_write(&cd->fader, value);
		cd->gate_array[reg] &= 0x8000;
		cd->gate_array[reg] |= value;
		break;
	case GA_CDD_CTRL: {
		cdd_run(cd, m68k->cycles);
		uint16_t changed = cd->gate_array[reg] ^ value;
		if (changed & BIT_HOCK) {
			cd->gate_array[reg] &= ~BIT_HOCK;
			cd->gate_array[reg] |= value & BIT_HOCK;
			if (value & BIT_HOCK) {
				cdd_hock_enabled(&cd->cdd);
			} else {
				cdd_hock_disabled(&cd->cdd);
				cd->gate_array[reg] |= BIT_MUTE;
			}
			calculate_target_cycle(m68k);
		}
		break;
	}
	case GA_CDD_CMD0:
	case GA_CDD_CMD1:
	case GA_CDD_CMD2:
	case GA_CDD_CMD3:
		cdd_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0x0F0F;
		break;
	case GA_CDD_CMD4:
		cdd_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0x0F0F;
		cdd_mcu_start_cmd_recv(&cd->cdd, cd->gate_array + GA_CDD_CTRL);
		break;
	case GA_FONT_COLOR:
		cd->gate_array[reg] = value & 0xFF;
		break;
	case GA_FONT_BITS:
		cd->gate_array[reg] = value;
		break;
	case GA_STAMP_SIZE:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] &= BIT_GRON;
		cd->gate_array[reg] |= value & (BIT_SMS|BIT_STS|BIT_RPT);
		break;
	case GA_STAMP_MAP_BASE:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0xFFE0;
		break;
	case GA_IMAGE_BUFFER_VCELLS:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0x1F;
		break;
	case GA_IMAGE_BUFFER_START:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0xFFF8;
		break;
	case GA_IMAGE_BUFFER_OFFSET:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0x3F;
		break;
	case GA_IMAGE_BUFFER_HDOTS:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0x1FF;
		break;
	case GA_IMAGE_BUFFER_LINES:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0xFF;
		break;
	case GA_TRACE_VECTOR_BASE:
		cd_graphics_run(cd, m68k->cycles);
		cd->gate_array[reg] = value & 0xFFFE;
		cd_graphics_start(cd);
		break;
	default:
		printf("Unhandled gate array write %X:%X\n", address, value);
	}
	return vcontext;
}

static void *sub_gate_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	segacd_context *cd = m68k->system;
	uint32_t reg = (address & 0x1FF) >> 1;
	uint16_t value16;
	switch (address >> 1)
	{
	case GA_MEM_MODE:
	case GA_CDC_HOST_DATA:
	case GA_CDC_DMA_ADDR:
	case GA_STOP_WATCH:
	case GA_COMM_FLAG:
	case GA_TIMER:
	case GA_CDD_FADER:
	case GA_FONT_COLOR:
		//these registers treat all writes as word-wide
		value16 = value | (value << 8);
		break;
	case GA_CDC_CTRL:
		if (address & 1) {
			lc8951_ar_write(&cd->cdc, value);
			return vcontext;
		} else {
			value16 = cd->cdc.ar | (value << 8);
		}
		break;
	case GA_CDD_CMD4:
		if (!address) {
			//byte write to $FF804A should not trigger transfer
			cdd_run(cd, m68k->cycles);
			cd->gate_array[reg] &= 0x0F;
			cd->gate_array[reg] |= (value << 8 & 0x0F00);
			return vcontext;
		}
		//intentional fallthrough for $FF804B
	default:
		if (address & 1) {
			value16 = cd->gate_array[reg] & 0xFF00 | value;
		} else {
			value16 = cd->gate_array[reg] & 0xFF | (value << 8);
		}
	}
	return sub_gate_write16(address, vcontext, value16);
}

static uint8_t can_main_access_prog(segacd_context *cd)
{
	//TODO: use actual busack
	return cd->busreq || !cd->reset;
}

static uint8_t handle_cdc_byte(void *vsys, uint8_t value)
{
	segacd_context *cd = vsys;
	if (cd->gate_array[GA_CDC_CTRL] & BIT_DSR) {
		//host reg is already full, pause transfer
		return 0;
	}
	if (cd->cdc.cycle == cd->cdc.transfer_end) {
		cd->gate_array[GA_CDC_CTRL] |= BIT_EDT;
		dprintf("EDT set at %u\n", cd->cdc.cycle);
	}
	uint16_t dest = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
	if (!(cd->cdc_dst_low & 1)) {
		cd->gate_array[GA_CDC_HOST_DATA] &= 0xFF;
		cd->gate_array[GA_CDC_HOST_DATA] |= value << 8;
		cd->cdc_dst_low++;
		if (dest != DST_PCM_RAM) {
			//PCM RAM writes a byte at a time
			return 1;
		}
	} else {
		cd->gate_array[GA_CDC_HOST_DATA] &= 0xFF00;
		cd->gate_array[GA_CDC_HOST_DATA] |= value;
	}

	uint32_t dma_addr = cd->gate_array[GA_CDC_DMA_ADDR] << 3;
	dma_addr |= cd->cdc_dst_low;
	switch (dest)
	{
	case DST_MAIN_CPU:
	case DST_SUB_CPU:
		cd->cdc_dst_low = 0;
		cd->gate_array[GA_CDC_CTRL] |= BIT_DSR;
		break;
	case DST_PCM_RAM:
		dma_addr &= (1 << 13) - 1;
		rf5c164_run(&cd->pcm, cd->cdc.cycle);
		while ((cd->pcm.flags & 0x81) == 1) {
			//not sounding, but pending write
			//DMA write conflict with CPU
			rf5c164_run(&cd->pcm, cd->pcm.cycle + 4);
		}
		rf5c164_write(&cd->pcm, 0x1000 | (dma_addr >> 1), value);
		dma_addr += 2;
		cd->cdc_dst_low = dma_addr & 7;
		cd->gate_array[GA_CDC_DMA_ADDR] = dma_addr >> 3;
		//TODO: determine actual main CPU penalty
		cd->m68k->cycles += 2 * cd->m68k->opts->gen.bus_cycles;
		break;
	case DST_PROG_RAM:
		if (can_main_access_prog(cd)) {
			return 0;
		}
		cd->prog_ram[dma_addr >> 1] = cd->gate_array[GA_CDC_HOST_DATA];
		m68k_invalidate_code_range(cd->m68k, dma_addr - 1, dma_addr + 1);
		dma_addr++;
		cd->cdc_dst_low = dma_addr & 7;
		cd->gate_array[GA_CDC_DMA_ADDR] = dma_addr >> 3;
		//TODO: determine actual main CPU penalty
		cd->m68k->cycles += 2 * cd->m68k->opts->gen.bus_cycles;
		break;
	case DST_WORD_RAM:
		if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
			//1M mode, write to bank assigned to Sub CPU

			uint32_t masked = dma_addr & (1 << 17) - 2;
			cd->m68k->mem_pointers[1][masked] = cd->gate_array[GA_CDC_HOST_DATA];
			m68k_invalidate_code_range(cd->m68k, 0x0C0000 + masked - 1, 0x0C0000 + masked + 1);
		} else {
			//2M mode, check if Sub CPU has access
			if (cd->main_has_word2m) {
				return 0;
			} else {
				cd_graphics_run(cd, cd->cdc.cycle);
				dma_addr &= (1 << 18) - 1;
				cd->word_ram[dma_addr >> 1] = cd->gate_array[GA_CDC_HOST_DATA];
				m68k_invalidate_code_range(cd->m68k, 0x080000 + dma_addr, 0x080000 + dma_addr + 1);
			}
		}
		dma_addr++;
		cd->cdc_dst_low = dma_addr & 7;
		cd->gate_array[GA_CDC_DMA_ADDR] = dma_addr >> 3;
		break;
	default:
		return 0;
	}
	return 1;
}

static void scd_peripherals_run(segacd_context *cd, uint32_t cycle)
{
	timers_run(cd, cycle);
	cdd_run(cd, cycle);
	cd_graphics_run(cd, cycle);
	rf5c164_run(&cd->pcm, cycle);
}

static m68k_context *sync_components(m68k_context * context, uint32_t address)
{
	segacd_context *cd = context->system;

	uint32_t num_refresh = (context->cycles - cd->last_refresh_cycle) / REFRESH_INTERVAL;
	cd->last_refresh_cycle = cd->last_refresh_cycle + num_refresh * REFRESH_INTERVAL;
	context->cycles += num_refresh * REFRESH_DELAY;

	scd_peripherals_run(cd, context->cycles);
	if (address) {
		if (cd->enter_debugger || context->wp_hit) {
			genesis_context *gen = cd->genesis;
			cd->enter_debugger = 0;
#ifndef IS_LIB
			if (gen->header.debugger_type == DEBUGGER_NATIVE) {
				debugger(context, address);
			} else {
				gdb_debug_enter(context, address);
			}
#endif
		}
		cd->m68k_pc = address;
	}
	calculate_target_cycle(context);
	return context;
}

static m68k_context *int_ack(m68k_context *context)
{	
	segacd_context *cd = context->system;
	scd_peripherals_run(cd, context->cycles);
	switch (context->int_pending)
	{
	case 1:
		cd->graphics_int_cycle = CYCLE_NEVER;
		break;
	case 2:
		cd->int2_cycle = CYCLE_NEVER;
		break;
	case 3:
		cd->timer_pending = 0;
		break;
	case 4:
		cd->cdd.int_pending = 0;
		break;
	case 5:
		cd->cdc_int_ack = 1;
		break;
	case 6:
		cd->cdd.subcode_int_pending = 0;
		break;
	}
	//the Sega CD responds to these exclusively with !VPA which means its a slow
	//6800 operation. documentation says these can take between 10 and 19 cycles.
	//actual results measurements seem to suggest it's actually between 9 and 18
	//Base 68K core has added 4 cycles for a normal int ack cycle already
	//We add 5 + the current cycle count (in 68K cycles) mod 10 to simulate the
	//additional variable delay from the use of the 6800 cycle
	uint32_t cycle_count = context->cycles / context->opts->gen.clock_divider;
	context->cycles += 5 + (cycle_count % 10);
	
	return context;
}

void scd_run(segacd_context *cd, uint32_t cycle)
{
	uint8_t m68k_run = !can_main_access_prog(cd);
	while (cycle > cd->m68k->cycles) {
		if (m68k_run && !cd->sub_paused_wordram) {
			uint32_t num_refresh = (cd->m68k->cycles - cd->last_refresh_cycle) / REFRESH_INTERVAL;
			cd->last_refresh_cycle = cd->last_refresh_cycle + num_refresh * REFRESH_INTERVAL;
			cd->m68k->cycles += num_refresh * REFRESH_DELAY;


			cd->m68k->sync_cycle = cd->enter_debugger ? cd->m68k->cycles + 1 : cycle;
			if (cd->need_reset) {
				cd->need_reset = 0;
				m68k_reset(cd->m68k);
			} else {
				calculate_target_cycle(cd->m68k);
				resume_68k(cd->m68k);
			}
		} else {
			cd->m68k->cycles = cycle;
			cd->last_refresh_cycle = cycle;
		}
		scd_peripherals_run(cd, cd->m68k->cycles);
	}

}

uint32_t gen_cycle_to_scd(uint32_t cycle, genesis_context *gen)
{
	return ((uint64_t)cycle) * ((uint64_t)SCD_MCLKS) / ((uint64_t)gen->normal_clock);
}

void scd_adjust_cycle(segacd_context *cd, uint32_t deduction)
{
	deduction = gen_cycle_to_scd(deduction, cd->genesis);
	cd->m68k->cycles -= deduction;
	cd->stopwatch_cycle -= deduction;
	if (deduction >= cd->int2_cycle) {
		cd->int2_cycle = 0;
	} else if (cd->int2_cycle != CYCLE_NEVER) {
		cd->int2_cycle -= deduction;
	}
	if (deduction >= cd->periph_reset_cycle) {
		cd->periph_reset_cycle = CYCLE_NEVER;
	} else if (cd->periph_reset_cycle != CYCLE_NEVER) {
		cd->periph_reset_cycle -= deduction;
	}
	cdd_mcu_adjust_cycle(&cd->cdd, deduction);
	lc8951_adjust_cycles(&cd->cdc, deduction);
	cd->graphics_cycle -= deduction;
	if (cd->graphics_int_cycle != CYCLE_NEVER) {
		if (cd->graphics_int_cycle > deduction) {
			cd->graphics_int_cycle -= deduction;
		} else {
			cd->graphics_int_cycle = 0;
		}
	}
	if (deduction >= cd->last_refresh_cycle) {
		cd->last_refresh_cycle -= deduction;
	} else {
		cd->last_refresh_cycle = 0;
	}
	cd->pcm.cycle -= deduction;
}

static uint16_t main_gate_read16(uint32_t address, void *vcontext)
{
	m68k_context *m68k = vcontext;
	gen_update_refresh_free_access(m68k);
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t scd_cycle = gen_cycle_to_scd(m68k->cycles, gen);
	scd_run(cd, scd_cycle);
	uint32_t offset = (address & 0x1FF) >> 1;
	switch (offset)
	{
	case GA_SUB_CPU_CTRL: {
		uint16_t value = 0;
		if (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN2) {
			value |= BIT_IEN2;
		}
		if (cd->int2_cycle != CYCLE_NEVER) {
			value |= BIT_IFL2;
		}
		if (can_main_access_prog(cd)) {
			value |= BIT_SBRQ;
		}
		if (cd->reset) {
			value |= BIT_SRES;
		}
		return value;
	}
	case GA_MEM_MODE:
		//Main CPU can't read priority mode bits
		return cd->gate_array[offset] & 0xFFE7;
	case GA_HINT_VECTOR:
		return cd->rom_mut[0x72/2];
	case GA_CDC_HOST_DATA: {
		uint16_t dst = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
		if (dst == DST_MAIN_CPU) {
			if (cd->gate_array[GA_CDC_CTRL] & BIT_DSR) {
				cd->gate_array[GA_CDC_CTRL] &= ~BIT_DSR;
				lc8951_resume_transfer(&cd->cdc);
			} else {
				dprintf("Read of CDC host data with DSR clear at %u\n", scd_cycle);
			}
			calculate_target_cycle(cd->m68k);
		}
		return cd->gate_array[offset];
	}
	case GA_CDC_DMA_ADDR:
		//TODO: open bus maybe?
		return 0xFFFF;
	default:
		if (offset < GA_TIMER) {
			return cd->gate_array[offset];
		}
		//TODO: open bus maybe?
		return 0xFFFF;
	}
}

static uint8_t main_gate_read8(uint32_t address, void *vcontext)
{
	uint16_t val = main_gate_read16(address & 0xFE, vcontext);
	return address & 1 ? val : val >> 8;
}

static void *main_gate_write16(uint32_t address, void *vcontext, uint16_t value)
{
	m68k_context *m68k = vcontext;
	gen_update_refresh_free_access(m68k);
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t scd_cycle = gen_cycle_to_scd(m68k->cycles, gen);
	uint32_t reg = (address & 0x1FF) >> 1;
	if (reg != GA_SUB_CPU_CTRL) {
		scd_run(cd, scd_cycle);
	}
	switch (reg)
	{
	case GA_SUB_CPU_CTRL: {
		if ((value & BIT_IFL2) && (cd->gate_array[GA_INT_MASK] & BIT_MASK_IEN2)) {
			if (cd->int2_cycle != CYCLE_NEVER) {
				scd_run(cd, scd_cycle - 4 * cd->m68k->opts->gen.clock_divider);
				while (cd->int2_cycle != CYCLE_NEVER && cd->m68k->cycles < scd_cycle) {
					scd_run(cd, cd->m68k->cycles + cd->m68k->opts->gen.clock_divider);
				}
			}
			cd->int2_cycle = scd_cycle;
			
		}
		scd_run(cd, scd_cycle);
		uint8_t old_access = can_main_access_prog(cd);
		cd->busreq = value & BIT_SBRQ;
		uint8_t old_reset = cd->reset;
		cd->reset = value & BIT_SRES;
		if (cd->reset && !old_reset) {
			cd->need_reset = 1;
		}
		/*cd->gate_array[reg] &= 0x7FFF;
		cd->gate_array[reg] |= value & 0x8000;*/
		uint8_t new_access = can_main_access_prog(cd);
		uint32_t bank = cd->gate_array[GA_MEM_MODE] >> 6 & 0x3;
		if (new_access) {
			if (!old_access) {
				m68k->mem_pointers[cd->memptr_start_index] = cd->prog_ram + bank * 0x10000;
				m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
			}
		} else if (old_access) {
			m68k->mem_pointers[cd->memptr_start_index] = NULL;
			m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
			m68k_invalidate_code_range(cd->m68k, bank * 0x20000, (bank + 1) * 0x20000);
			uint16_t dst = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
			if (dst == DST_PROG_RAM) {
				lc8951_resume_transfer(&cd->cdc);
			}
		}
		break;
	}
	case GA_MEM_MODE: {
		uint16_t changed = cd->gate_array[reg] ^ value;
		//Main CPU can't write priority mode bits, MODE or RET
		cd->gate_array[reg] &= 0x001F;
		cd->gate_array[reg] |= value & 0xFFC0;
		if ((cd->gate_array[reg] & BIT_MEM_MODE)) {
			//1M mode
			if (!(value & BIT_DMNA)) {
				cd->gate_array[reg] |= BIT_DMNA;
				cd->main_swap_request = 1;
			} else {
				cd->main_has_word2m = 0;
			}
		} else {
			//2M mode
			if (changed & value & BIT_DMNA) {
				cd->gate_array[reg] |= BIT_DMNA;
				m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
				m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
				cd->m68k->mem_pointers[0] = cd->word_ram;
				cd->gate_array[reg] &= ~BIT_RET;
				cd->main_has_word2m = 0;
				if (cd->sub_paused_wordram) {
					cd->sub_paused_wordram = 0;
				}

				uint16_t dst = cd->gate_array[GA_CDC_CTRL] >> 8 & 0x7;
				if (dst == DST_WORD_RAM) {
					lc8951_resume_transfer(&cd->cdc);
				}

				m68k_invalidate_code_range(m68k, cd->base + 0x200000, cd->base + 0x240000);
				m68k_invalidate_code_range(cd->m68k, 0x080000, 0x0C0000);
			}
		}
		if (changed & MASK_PROG_BANK && can_main_access_prog(cd)) {
			uint32_t bank = cd->gate_array[GA_MEM_MODE] >> 6 & 0x3;
			m68k->mem_pointers[cd->memptr_start_index] = cd->prog_ram + bank * 0x10000;
			m68k_invalidate_code_range(m68k, cd->base + 0x220000, cd->base + 0x240000);
		}
		break;
	}
	case GA_HINT_VECTOR:
		cd->rom_mut[0x72/2] = value;
		break;
	case GA_CDC_HOST_DATA:
		//writes to this register have the same side effects as reads
		main_gate_read16(address, vcontext);
		break;
	case GA_COMM_FLAG:
		//Main CPU can only write the upper byte;
		cd->gate_array[reg] &= 0xFF;
		cd->gate_array[reg] |= value & 0xFF00;
		break;
	case GA_COMM_CMD0:
	case GA_COMM_CMD1:
	case GA_COMM_CMD2:
	case GA_COMM_CMD3:
	case GA_COMM_CMD4:
	case GA_COMM_CMD5:
	case GA_COMM_CMD6:
	case GA_COMM_CMD7:
		//no effects for these other than saving the value
		cd->gate_array[reg] = value;
		break;
	default:
		dprintf("Unhandled gate array write %X:%X\n", address, value);
	}
	return vcontext;
}

static void *main_gate_write8(uint32_t address, void *vcontext, uint8_t value)
{
	m68k_context *m68k = vcontext;
	genesis_context *gen = m68k->system;
	segacd_context *cd = gen->expansion;
	uint32_t reg = (address & 0x1FF) >> 1;
	uint16_t value16;
	switch (reg)
	{
	case GA_SUB_CPU_CTRL:
		if (address & 1) {
			value16 = value;
		} else {
			value16 = value << 8;
			if (cd->reset) {
				value16 |= BIT_SRES;
			}
			if (cd->busreq) {
				value16 |= BIT_SBRQ;
			}
		}
		break;
	case GA_HINT_VECTOR:
	case GA_COMM_FLAG:
		//writes to these regs are always treated as word wide
		value16 = value | (value << 8);
		break;
	default:
		if (address & 1) {
			value16 = cd->gate_array[reg] & 0xFF00 | value;
		} else {
			value16 = cd->gate_array[reg] & 0xFF | (value << 8);
		}
	}
	return main_gate_write16(address, vcontext, value16);
}

uint8_t laseractive_regs[256];

static uint16_t laseractive_read16(uint32_t address, void *vcontext)
{
	printf("LaserActive 16-bit register read %X\n", address);
	return 0xFFFF;
}

static uint8_t laseractive_read8(uint32_t address, void *vcontext)
{
	printf("LaserActive 8-bit register read %X\n", address);
	if (address == 0xFDFE81) {
		return 0x80 | (laseractive_regs[0x41] & 1);
	} else if (address >= 0xFDFE41 && address < 0xFDFE80 && (address & 1)) {
		return laseractive_regs[address & 0xFF];
	}
	return 0xFF;
}

static void *laseractive_write16(uint32_t address, void *vcontext, uint16_t value)
{
	printf("LaserActive 16-bit register write %X: %X\n", address, value);
	return vcontext;
}

static void *laseractive_write8(uint32_t address, void *vcontext, uint8_t value)
{
	printf("LaserActive 8-bit register write %X: %X\n", address, value);
	laseractive_regs[address & 0xFF] = value;
	return vcontext;
}

segacd_context *alloc_configure_segacd(system_media *media, uint32_t opts, uint8_t force_region, rom_info *info)
{
	static memmap_chunk sub_cpu_map[] = {
		{0x000000, 0x01FF00, 0xFFFFFF, .flags=MMAP_READ | MMAP_CODE, .write_16 = prog_ram_wp_write16, .write_8 = prog_ram_wp_write8},
		{0x01FF00, 0x080000, 0xFFFFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE},
		{0x080000, 0x0C0000, 0x03FFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE | MMAP_PTR_IDX | MMAP_FUNC_NULL, .ptr_index = 0,
			.read_16 = word_ram_2M_read16, .write_16 = word_ram_2M_write16, .read_8 = word_ram_2M_read8, .write_8 = word_ram_2M_write8},
		{0x0C0000, 0x0E0000, 0x01FFFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_CODE | MMAP_PTR_IDX | MMAP_FUNC_NULL, .ptr_index = 1,
			.read_16 = word_ram_1M_read16, .write_16 = word_ram_1M_write16, .read_8 = word_ram_1M_read8, .write_8 = word_ram_1M_write8, .shift = 1},
		{0xFE0000, 0xFF0000, 0x003FFF, .flags=MMAP_READ | MMAP_WRITE | MMAP_ONLY_ODD},
		{0xFF0000, 0xFF8000, 0x003FFF, .read_16 = pcm_read16, .write_16 = pcm_write16, .read_8 = pcm_read8, .write_8 = pcm_write8},
		{0xFF8000, 0xFF8200, 0x0001FF, .read_16 = sub_gate_read16, .write_16 = sub_gate_write16, .read_8 = sub_gate_read8, .write_8 = sub_gate_write8},
		{0xFD0000, 0xFE0000, 0xFFFFFF, .read_16 = laseractive_read16, .write_16 = laseractive_write16, .read_8 = laseractive_read8, .write_8 = laseractive_write8}
	};

	segacd_context *cd = calloc(sizeof(segacd_context), 1);
	cd->speed_percent = 100;
	uint32_t firmware_size;
	uint8_t region = force_region;
	if (!region) {
		char * def_region = tern_find_path_default(config, "system\0default_region\0", (tern_val){.ptrval = "U"}, TVAL_PTR).ptrval;
		if (!info->regions || (info->regions & translate_region_char(toupper(*def_region)))) {
			region = translate_region_char(toupper(*def_region));
		} else {
			region = info->regions;
		}
	}
	const char *key;
	if (region & REGION_E) {
		key = "system\0scd_bios_eu\0";
	} else if (region & REGION_J) {
		key = "system\0scd_bios_jp\0";
	} else {
		key = "system\0scd_bios_us\0";
	}
	char *bios_path = tern_find_path_default(config, key, (tern_val){.ptrval = "cdbios.bin"}, TVAL_PTR).ptrval;
	if (media_path_is_external(bios_path)) {
		media_file *f = media_fopen(bios_path, "rb");
		if (f) {
			long to_read = media_file_size(f);
			cd->rom = malloc(to_read);
			firmware_size = media_fread(cd->rom, 1, to_read, f);
			if (!firmware_size) {
				free(cd->rom);
				cd->rom = NULL;
			}
			media_fclose(f);
		}
	} else {
		cd->rom = (uint16_t *)read_bundled_file(bios_path, &firmware_size);
	}
	if (!cd->rom) {
		fatal_error("Failed to load Sega CD BIOS from %s\n", bios_path);
	}
	uint32_t adjusted_size = nearest_pow2(firmware_size);
	if (adjusted_size != firmware_size) {
		cd->rom = realloc(cd->rom, adjusted_size);
	}
	cd->rom_mut = malloc(adjusted_size);
	byteswap_rom(adjusted_size, cd->rom);
	memcpy(cd->rom_mut, cd->rom, adjusted_size);
	cd->rom_mut[0x72/2] = 0xFFFF;

	cd->prog_ram = calloc(512*1024, 1);
	cd->word_ram = calloc(256*1024, 1);
	cd->bram = calloc(8*1024, 1);
	char *bram_size_id = tern_find_path_default(config, "system\0bram_cart_size_id\0", (tern_val){.ptrval = "4"}, TVAL_PTR).ptrval;
	cd->bram_cart_id = 0xFF;
	cd->bram_cart = NULL;
	if (strcmp(bram_size_id, "none")) {
		char *end;
		long id = strtol(bram_size_id, &end, 10);
		if (end != bram_size_id && id < 8) {
			cd->bram_cart_id = id;
			cd->bram_cart = calloc(0x2000 << id, 1);
		}
	}

	sub_cpu_map[0].buffer = sub_cpu_map[1].buffer = cd->prog_ram;
	sub_cpu_map[4].buffer = cd->bram;
	m68k_options *mopts = malloc(sizeof(m68k_options));
	init_m68k_opts(mopts, sub_cpu_map, sizeof(sub_cpu_map) / sizeof(*sub_cpu_map), 4, sync_components, int_ack);
	cd->m68k = init_68k_context(mopts, NULL);
	cd->m68k->system = cd;
	cd->int2_cycle = CYCLE_NEVER;
	cd->busreq = 1;
	cd->busack = 1;
	cd->need_reset = 1;
	cd->reset = 1; //active low, so reset is not active on start
	cd->memptr_start_index = 0;
	cd->gate_array[1] = 1;
	cd->gate_array[GA_CDD_CTRL] = BIT_MUTE; //Data/mute flag is set on start
	cd->main_has_word2m = 1;
	lc8951_init(&cd->cdc, handle_cdc_byte, cd);
	if (media->chain && media->type != MEDIA_CDROM) {
		media = media->chain;
	}
	cdd_mcu_init(&cd->cdd, media);
	cd_graphics_init(cd);
	cdd_fader_init(&cd->fader);
	rf5c164_init(&cd->pcm, SCD_MCLKS, 4);
	return cd;
}

void free_segacd(segacd_context *cd)
{
	cdd_fader_deinit(&cd->fader);
	rf5c164_deinit(&cd->pcm);
	m68k_options_free(cd->m68k->opts);
	free(cd->m68k);
	free(cd->bram);
	free(cd->word_ram);
	free(cd->prog_ram);
	free(cd->rom_mut);
	cdimage_free(cd->cdd.media);
}

void segacd_serialize(segacd_context *cd, serialize_buffer *buf, uint8_t all)
{
	if (all) {
		start_section(buf, SECTION_SUB_68000);
		m68k_serialize(cd->m68k, cd->m68k_pc, buf);
		end_section(buf);

		start_section(buf, SECTION_GATE_ARRAY);
		save_buffer16(buf, cd->gate_array, sizeof(cd->gate_array)/sizeof(*cd->gate_array));
		save_buffer16(buf, cd->prog_ram, 256*1024);
		save_buffer16(buf, cd->word_ram, 128*1024);
		save_int16(buf, cd->rom_mut[0x72/2]);
		save_int32(buf, cd->stopwatch_cycle);
		save_int32(buf, cd->int2_cycle);
		save_int32(buf, cd->graphics_int_cycle);
		save_int32(buf, cd->periph_reset_cycle);
		save_int32(buf, cd->last_refresh_cycle);
		save_int32(buf, cd->graphics_cycle);
		save_int32(buf, cd->base);
		save_int32(buf, cd->graphics_x);
		save_int32(buf, cd->graphics_y);
		save_int32(buf, cd->graphics_dx);
		save_int32(buf, cd->graphics_dy);
		save_int32(buf, cd->graphics_dst_x);
		save_buffer8(buf, cd->graphics_pixels, sizeof(cd->graphics_pixels));
		save_int8(buf, cd->timer_pending);
		save_int8(buf, cd->timer_value);
		save_int8(buf, cd->busreq);
		save_int8(buf, cd->reset);
		save_int8(buf, cd->need_reset);
		save_int8(buf, cd->cdc_dst_low);
		save_int8(buf, cd->cdc_int_ack);
		save_int8(buf, cd->graphics_step);
		save_int8(buf, cd->graphics_dst_y);
		save_int8(buf, cd->main_has_word2m);
		save_int8(buf, cd->main_swap_request);
		save_int8(buf, cd->bank_toggle);
		save_int8(buf, cd->sub_paused_wordram);
		save_int8(buf, cd->bram_cart_write_enabled);
		end_section(buf);

		start_section(buf, SECTION_CDD_MCU);
		cdd_mcu_serialize(&cd->cdd, buf);
		end_section(buf);

		start_section(buf, SECTION_LC8951);
		lc8951_serialize(&cd->cdc, buf);
		end_section(buf);

		start_section(buf, SECTION_CDROM);
		cdimage_serialize(cd->cdd.media, buf);
		end_section(buf);
	}
	start_section(buf, SECTION_RF5C164);
	rf5c164_serialize(&cd->pcm, buf);
	end_section(buf);

	start_section(buf, SECTION_CDD_FADER);
	cdd_fader_serialize(&cd->fader, buf);
	end_section(buf);
}

static void gate_array_deserialize(deserialize_buffer *buf, void *vcd)
{
	segacd_context *cd = vcd;
	load_buffer16(buf, cd->gate_array, sizeof(cd->gate_array)/sizeof(*cd->gate_array));
	load_buffer16(buf, cd->prog_ram, 256*1024);
	load_buffer16(buf, cd->word_ram, 128*1024);
	cd->rom_mut[0x72/2] = load_int16(buf);
	cd->stopwatch_cycle = load_int32(buf);
	cd->int2_cycle = load_int32(buf);
	cd->graphics_int_cycle = load_int32(buf);
	cd->periph_reset_cycle = load_int32(buf);
	cd->last_refresh_cycle = load_int32(buf);
	cd->graphics_cycle = load_int32(buf);
	cd->base = load_int32(buf);
	cd->graphics_x = load_int32(buf);
	cd->graphics_y = load_int32(buf);
	cd->graphics_dx = load_int32(buf);
	cd->graphics_dy = load_int32(buf);
	cd->graphics_dst_x = load_int32(buf);
	load_buffer8(buf, cd->graphics_pixels, sizeof(cd->graphics_pixels));
	cd->timer_pending = load_int8(buf);
	cd->timer_value = load_int8(buf);
	cd->busreq = load_int8(buf);
	cd->reset = load_int8(buf);
	cd->need_reset = load_int8(buf);
	cd->cdc_dst_low = load_int8(buf);
	cd->cdc_int_ack = load_int8(buf);
	cd->graphics_step = load_int8(buf);
	cd->graphics_dst_y = load_int8(buf);
	cd->main_has_word2m = load_int8(buf);
	cd->main_swap_request = load_int8(buf);
	cd->bank_toggle = load_int8(buf);
	cd->sub_paused_wordram = load_int8(buf);
	cd->bram_cart_write_enabled = load_int8(buf);

	if (cd->gate_array[GA_MEM_MODE] & BIT_MEM_MODE) {
		//1M mode
		cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
		cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
		cd->m68k->mem_pointers[0] = NULL;
		cd->m68k->mem_pointers[1] = cd->bank_toggle ? cd->word_ram : cd->word_ram + 1;
	} else {
		//2M mode
		if (cd->main_has_word2m) {
			//main CPU has word ram
			cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 1] = cd->word_ram;
			cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 2] = cd->word_ram + 0x10000;
			cd->m68k->mem_pointers[0] = NULL;
			cd->m68k->mem_pointers[1] = NULL;
		} else {
			//sub cpu has word ram
			cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 1] = NULL;
			cd->genesis->m68k->mem_pointers[cd->memptr_start_index + 2] = NULL;
			cd->m68k->mem_pointers[0] = cd->word_ram;
			cd->m68k->mem_pointers[1] = NULL;
		}
	}

	m68k_invalidate_code_range(cd->m68k, 0, 0x0E0000);
	m68k_invalidate_code_range(cd->genesis->m68k, cd->base + 0x200000, cd->base + 0x240000);
}

void segacd_register_section_handlers(segacd_context *cd, deserialize_buffer *buf)
{
	register_section_handler(buf, (section_handler){.fun = m68k_deserialize, .data = cd->m68k}, SECTION_SUB_68000);
	register_section_handler(buf, (section_handler){.fun = gate_array_deserialize, .data = cd}, SECTION_GATE_ARRAY);
	register_section_handler(buf, (section_handler){.fun = cdd_mcu_deserialize, .data = &cd->cdd}, SECTION_CDD_MCU);
	register_section_handler(buf, (section_handler){.fun = lc8951_deserialize, .data = &cd->cdc}, SECTION_LC8951);
	register_section_handler(buf, (section_handler){.fun = rf5c164_deserialize, .data = &cd->pcm}, SECTION_RF5C164);
	register_section_handler(buf, (section_handler){.fun = cdd_fader_deserialize, .data = &cd->fader}, SECTION_CDD_FADER);
	register_section_handler(buf, (section_handler){.fun = cdimage_deserialize, .data = cd->cdd.media}, SECTION_CDROM);
}

memmap_chunk *segacd_main_cpu_map(segacd_context *cd, uint8_t cart_boot, uint32_t *num_chunks)
{
	static memmap_chunk main_cpu_map[] = {
		{0x000000, 0x020000, 0x01FFFF, .flags=MMAP_READ},
		{0x020000, 0x040000, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 0,
			.read_16 = unmapped_prog_read16, .write_16 = unmapped_prog_write16, .read_8 = unmapped_prog_read8, .write_8 = unmapped_prog_write8},
		{0x040000, 0x060000, 0x01FFFF, .flags=MMAP_READ}, //first ROM alias
		//TODO: additional ROM/prog RAM aliases
		{0x200000, 0x220000, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 1,
			.read_16 = unmapped_word_read16, .write_16 = unmapped_word_write16, .read_8 = unmapped_word_read8, .write_8 = unmapped_word_write8},
		{0x220000, 0x240000, 0x01FFFF, .flags=MMAP_READ|MMAP_WRITE|MMAP_PTR_IDX|MMAP_FUNC_NULL|MMAP_CODE, .ptr_index = 2,
			.read_16 = cell_image_read16, .write_16 = cell_image_write16, .read_8 = cell_image_read8, .write_8 = cell_image_write8},
		{0xA12000, 0xA13000, 0xFFFFFF, .read_16 = main_gate_read16, .write_16 = main_gate_write16, .read_8 = main_gate_read8, .write_8 = main_gate_write8},
		{0x400000, 0x800000, 0xFFFFFF, .read_16 = cart_area_read16, .write_16 = cart_area_write16, .read_8 = cart_area_read8, .write_8 = cart_area_write8}
	};
	*num_chunks = sizeof(main_cpu_map) / sizeof(*main_cpu_map);
	if (cart_boot) {
		(*num_chunks)--;
	}
	for (int i = 0; i < *num_chunks; i++)
	{
		if (main_cpu_map[i].start < 0x400000) {
			if (cart_boot) {
				main_cpu_map[i].start  |= 0x400000;
				main_cpu_map[i].end  |= 0x400000;
			} else {
				main_cpu_map[i].start  &= 0x3FFFFF;
				main_cpu_map[i].end  &= 0x3FFFFF;
			}
		}
	}
	main_cpu_map[0].buffer = cd->rom_mut;
	main_cpu_map[2].buffer = cd->rom;
	main_cpu_map[1].buffer = cd->prog_ram;
	main_cpu_map[3].buffer = cd->word_ram;
	main_cpu_map[4].buffer = cd->word_ram + 0x10000;
	return main_cpu_map;
}

void segacd_set_speed_percent(segacd_context *cd, uint32_t percent)
{
	uint32_t scd_cycle = gen_cycle_to_scd(cd->genesis->ym->current_cycle, cd->genesis);
	scd_run(cd, scd_cycle);
	cd->speed_percent = percent;
	uint32_t new_clock = ((uint64_t)SCD_MCLKS * (uint64_t)cd->speed_percent) / 100;
	rf5c164_adjust_master_clock(&cd->pcm, new_clock);
	cdd_fader_set_speed_percent(&cd->fader, percent);
}

void segacd_config_updated(segacd_context *cd)
{
	//sample rate may have changed
	uint32_t new_clock = ((uint64_t)SCD_MCLKS * (uint64_t)cd->speed_percent) / 100;
	rf5c164_adjust_master_clock(&cd->pcm, new_clock);
	cdd_fader_set_speed_percent(&cd->fader, cd->speed_percent);
}

static uint8_t *copy_chars(uint8_t *dst, uint8_t *str)
{
	size_t len = strlen(str);
	memcpy(dst, str, len);
	return dst + len;
}

void segacd_format_bram(uint8_t *buffer, size_t size)
{
	memset(buffer, 0, size);
	uint16_t free_blocks = (size / 64) - 3;
	uint8_t *cur = buffer + size - 0x40;
	cur = copy_chars(cur, "___________");
	cur += 4;
	*(cur++) = 0x40;
	for (int i = 0; i < 4; i++)
	{
		*(cur++) = free_blocks >> 8;
		*(cur++) = free_blocks;
	}
	cur += 8;
	cur = copy_chars(cur, "SEGA_CD_ROM");
	++cur;
	*(cur++) = 1;
	cur += 3;
	copy_chars(cur, "RAM_CARTRIDGE___");
}
