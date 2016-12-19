#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include "m68k_core.h"
#include "68kinst.h"
#include "jaguar.h"
#include "util.h"
#include "debug.h"
#include "config.h"
#include "render.h"

//BIOS Area Memory map
// 10 00 00 - 10 04 00 : Video mode/ Memory control registers
// 10 04 00 - 10 08 00 : CLUT
// 10 08 00 - 10 10 00 : Line buffer A
// 10 10 00 - 10 18 00 : Line buffer B
// 10 18 00 - 10 20 00 : Write Line buffer
// 10 21 00 - 10 30 00 : GPU/blitter registers
// 10 30 00 - 10 40 00 : GPU Local RAM (mirrored every 1K?)
// 11 00 00 - 11 00 40 : Timer/Clock registers
// 11 40 00 - 11 40 04 : Joystick Interface
// 11 A1 00 - 11 A1 52 : DSP/DAC/I2S Registers
// 11 B0 00 - 11 D0 00 : DSP Local RAM (8KB)
// 11 D0 00 - 11 E0 00 : Wave table ROM

int headless = 1;
tern_node * config;

void handle_keydown(int keycode, uint8_t scancode)
{
}

void handle_keyup(int keycode, uint8_t scancode)
{
}

void handle_joydown(int joystick, int button)
{
}

void handle_joyup(int joystick, int button)
{
}

void handle_joy_dpad(int joystick, int dpadnum, uint8_t value)
{
}

void handle_mousedown(int mouse, int button)
{
}

void handle_mouseup(int mouse, int button)
{
}

void handle_mouse_moved(int mouse, uint16_t x, uint16_t y, int16_t deltax, int16_t deltay)
{
}

void jag_update_m68k_int(jaguar_context *system)
{
	m68k_context *m68k = system->m68k;
	if (m68k->sync_cycle - m68k->current_cycle > system->max_cycles) {
		m68k->sync_cycle = m68k->current_cycle + system->max_cycles;
	}
	//TODO: Support other interrupt sources
	if (!system->cpu_int_control || (m68k->status & 0x7)) {
		m68k->int_cycle = CYCLE_NEVER;
	} else if(system->cpu_int_control & system->video->cpu_int_pending) {
		m68k->int_cycle = m68k->current_cycle;
		//supposedly all interrupts on the jaguar are "level 0" autovector interrupts
		//which I assume means they're abusing the "spurious interrupt" vector
		m68k->int_num = VECTOR_USER0 - VECTOR_SPURIOUS_INTERRUPT;
	} else {
		m68k->int_cycle = jag_next_vid_interrupt(system->video);
		m68k->int_num = VECTOR_USER0 - VECTOR_SPURIOUS_INTERRUPT;
	}
	
	if (m68k->int_cycle > m68k->current_cycle && m68k->int_pending == INT_PENDING_SR_CHANGE) {
		m68k->int_pending = INT_PENDING_NONE;
	}
	
	m68k->target_cycle = m68k->int_cycle < m68k->sync_cycle ? m68k->int_cycle : m68k->sync_cycle;
	if (m68k->should_return) {
		m68k->target_cycle = m68k->current_cycle;
	} else if (m68k->target_cycle < m68k->current_cycle) {
		//Changes to SR can result in an interrupt cycle that's in the past
		//This can cause issues with the implementation of STOP though
		m68k->target_cycle = m68k->current_cycle;
	}
}

void rom0_write_16(uint32_t address, jaguar_context *system, uint16_t value)
{
	//TODO: Use write_latch and write_pending to turn two 16-bit writes into a 32-bit one
	//Documentation heavily suggests that writes to most registers should be 32-bits wide
	if (address < 0x100000 || address >= 0x120000) {
		//Boot ROM
		fprintf(stderr, "Invalid write to Boot ROM - %X:%X\n", address, value);
		return;
	}
	if (address < 0x103000) {
		if (address < 0x101000) {
			if (address < 0x100400) {
				//Video mode / Memory control registers
				switch(address & 0x3FE)
				{
				case 0:
					if (((value ^ system->memcon1) & 1) || !system->memcon_written) {
						uint16_t **mem_pointers = system->m68k->mem_pointers;
						int rom = value & 1 ? 4 : 1;
						int ram0 = value & 1 ? 0 : 6;
						int ram1 = value & 1 ? 2 : 4;
						mem_pointers[ram0] = mem_pointers[ram0 + 1] = system->dram;
						//these are probably open bus, but mirror DRAM for now
						mem_pointers[ram1] = mem_pointers[ram1 + 1] = system->dram;
						
						mem_pointers[rom] = system->cart;
						mem_pointers[rom + 1] = system->cart + ((0x200000 & (system->cart_size-1)) >> 1);
						mem_pointers[rom + 2] = system->cart + ((0x400000 & (system->cart_size-1)) >> 1);
						system->memcon_written = 1;
						printf("MEMCON1 write - ROMHI: %d\n", value & 1);
						switch (system->memcon1 >> 3 & 3)
						{
						case 0:
							system->rom_cycles = 10;
							break;
						case 1:
							system->rom_cycles = 8;
							break;
						case 2:
							system->rom_cycles = 6;
							break;
						case 3:
							system->rom_cycles = 5;
							break;
						}
						//TODO: invalidate code cache
					}
					system->memcon1 = value;
					break;
				case 2:
					system->memcon2 = value;
					break;
				case 0xE0:
					system->cpu_int_control = value & 0x1F;
					system->video->cpu_int_pending &= ~(value >> 8);
					printf("INT1 write: %X @ %d - int_pending: %X, int_control: %X\n", value, system->m68k->current_cycle, system->video->cpu_int_pending, system->cpu_int_control);
					jag_update_m68k_int(system);
					//TODO: apply mask to int pending fields on other components once they are implemented
					break;
				case 0xE2:
					//no real handling of bus conflicts presently, so this doesn't really need to do anything yet
					printf("INT2 write: %X\n", value);
					break;
				default:
					jag_video_reg_write(system->video, address, value);
					jag_update_m68k_int(system);
					break;
				}
			} else if (address < 0x100800) {
				//CLUT
				address = address >> 1 & 255;
				system->video->clut[address] = value;
			} else {
				//Line buffer A
				address = address >> 1 & 0x3FF;
				if (address < LINEBUFFER_WORDS) {
					system->video->line_buffer_a[address] = value;
				}
			}
		} else if (address < 0x101800) {
			//Line buffer B
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				system->video->line_buffer_b[address] = value;
			}
		} else if (address < 0x102100) {
			//Write Line Buffer
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				system->video->write_line_buffer[address] = value;
			}
		} else {
			//GPU/Blitter registers
			if (address < 0x102200) {
				fprintf(stderr, "Unhandled write to GPU registers %X: %X\n", address, value);
				if (address == 0x102116 && (value & 1)) {
					FILE *f = fopen("gpu.bin", "wb");
					uint8_t buf[4];
					for (int i = 0; i < GPU_RAM_BYTES/sizeof(uint32_t); i++)
					{
						buf[0] = system->gpu_local[i] >> 24;
						buf[1] = system->gpu_local[i] >> 16;
						buf[2] = system->gpu_local[i] >> 8;
						buf[3] = system->gpu_local[i];
						fwrite(buf, 1, sizeof(buf), f);
					}
					fclose(f);
				}
			} else {
				fprintf(stderr, "Unhandled write to Blitter registers %X: %X\n", address, value);
			}
		}
	} else if (address < 0x11A100) {
		if (address < 0x110000) {
			//GPU Local RAM
				uint32_t offset = address >> 2 & (GPU_RAM_BYTES / sizeof(uint32_t) - 1);
			uint32_t value32 = value;
			if (address & 2) {
				system->gpu_local[offset] &= 0xFFFF0000;
			} else {
				system->gpu_local[offset] &= 0x0000FFFF;
				value32 = value32 << 16;
			}
			system->gpu_local[offset] |= value32;
		} else if (address < 0x114000) {
			//timer clock registers
			fprintf(stderr, "Unhandled write to timer/clock registers - %X:%X\n", address, value);
		} else {
			//joystick interface
			fprintf(stderr, "Unhandled write to joystick interface - %X:%X\n", address, value);
		}
	} else if (address < 0x11B000) {
		//DSP/DAC/I2S Registers
		fprintf(stderr, "Unhandled write to DSP/DAC/I2S registers - %X:%X\n", address, value);
	} else if (address < 0x11D000) {
		//DSP local RAM
		uint32_t offset = address >> 2 & (DSP_RAM_BYTES / sizeof(uint32_t) - 1);
		uint32_t value32 = value;
		if (address & 2) {
			system->dsp_local[offset] &= 0xFFFF0000;
		} else {
			system->dsp_local[offset] &= 0x0000FFFF;
			value32 = value32 << 16;
		}
		system->gpu_local[offset] |= value32;
	} else {
		//Wave table ROM
		fprintf(stderr, "Invalid write to wave table ROM - %X:%X\n", address, value);
	}
}

uint16_t rom0_read_16(uint32_t address, jaguar_context *system)
{
	if (address < 0x100000 || address >= 0x120000) {
		//Boot ROM
		address = address >> 1 & ((system->bios_size >> 1) - 1);
		return system->bios[address];
	}
	if (address < 0x103000) {
		if (address < 0x101000) {
			if (address < 0x100400) {
				//Video mode / Memory control registers
				switch (address & 0x3FE)
				{
				case 0xE0:
					puts("INT1 read");
					//TODO: Bitwise or with cpu_int_pending fields from other components once implemented
					return system->video->cpu_int_pending;
					break;
				default:
					fprintf(stderr, "Unhandled read from video mode/memory control registers - %X\n", address);
				}
			} else if (address < 0x100800) {
				//CLUT
				address = address >> 1 & 255;
				return system->video->clut[address];
			} else {
				//Line buffer A
				address = address >> 1 & 0x3FF;
				if (address < LINEBUFFER_WORDS) {
					return system->video->line_buffer_a[address];
				}
			}
		} else if (address < 0x101800) {
			//Line buffer B
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				return system->video->line_buffer_b[address];
			}
		} else if (address < 0x102100) {
			//Write Line Buffer
			address = address >> 1 & 0x3FF;
			if (address < LINEBUFFER_WORDS) {
				return system->video->write_line_buffer[address];
			}
		} else {
			//GPU/Blitter registers
			if (address < 0x102200) {
				fprintf(stderr, "Unhandled read from GPU registers %X\n", address);
			} else {
				fprintf(stderr, "Unhandled read from Blitter registers %X\n", address);
			}
		}
	} else if (address < 0x11A100) {
		if (address < 0x110000) {
			//GPU Local RAM
			uint32_t offset = address >> 2 & (GPU_RAM_BYTES / sizeof(uint32_t) - 1);
			if (address & 2) {
				return system->gpu_local[offset];
			} else {
				return system->gpu_local[offset] >> 16;
			}
		} else if (address < 0x114000) {
			//timer clock registers
			fprintf(stderr, "Unhandled read from timer/clock registers - %X\n", address);
		} else {
			//joystick interface
			fprintf(stderr, "Unhandled read from joystick interface - %X\n", address);
		}
	} else if (address < 0x11B000) {
		//DSP/DAC/I2S Registers
		fprintf(stderr, "Unhandled read from DSP/DAC/I2S registers - %X\n", address);
	} else if (address < 0x11D000) {
		//DSP local RAM
		uint32_t offset = address >> 2 & (DSP_RAM_BYTES / sizeof(uint32_t) - 1);
		if (address & 2) {
				return system->dsp_local[offset];
			} else {
				return system->dsp_local[offset] >> 16;
			}
	} else {
		//Wave table ROM
		fprintf(stderr, "Unhandled read from wave table ROM - %X\n", address);
	}
	return 0xFFFF;
}

uint64_t rom0_read_64(uint32_t address, jaguar_context *system)
{
	address &= 0x1FFFFF;
	uint64_t high = rom0_read_16(address, system);
	uint64_t highmid = rom0_read_16(address+2, system);
	uint64_t lowmid = rom0_read_16(address+4, system);
	uint64_t low = rom0_read_16(address+6, system);
	return high << 48 | highmid << 32 | lowmid << 16 | low;
}

void rom0_write_64(uint32_t address, jaguar_context *system, uint64_t val)
{
	address &= 0x1FFFFF;
	rom0_write_16(address, system, val >> 48);
	rom0_write_16(address+2, system, val >> 32);
	rom0_write_16(address+4, system, val >> 16);
	rom0_write_16(address+6, system, val);
}

uint64_t jag_read_phrase(jaguar_context *system, uint32_t address, uint32_t *cycles)
{
	if (!system->memcon_written) {
		//unsure of timing, but presumably at least 2 32-bit reads 
		//which presumably take a minimum of 1 cycle
		//reality probably depends on the exact area read
		//docs seem to imply some areas only 16-bits wide whereas others are 32-bit
		*cycles += 2;
		return rom0_read_64(address, system);
	}
	uint16_t *src;
	if (system->memcon1 & 1) {
		if (address < 0x800000) {
			src = system->dram + (address >> 1 & (DRAM_WORDS - 1));
			//DRAM is 64-bits wide, but sounds like an access is still at least two cycles
			*cycles += 2;
		} else if (address < 0xE00000) {
			//cart is slow and only 32-bits wide
			*cycles += 2 * (system->rom_cycles);
			src = system->cart + (address >> 1 & (system->cart_size - 1));
		} else {
			*cycles += 2;
			return rom0_read_64(address, system);
		}
	} else if (address > 0x800000) {
		src = system->dram + (address >> 1 & (DRAM_WORDS - 1));
		//DRAM is 64-bits wide, but sounds like an access is still at least two cycles
		*cycles += 2;
	} else if (address > 0x200000) {
		//cart is slow and only 32-bits wide
		*cycles += 2 * (system->rom_cycles);
		src = system->cart + (address >> 1 & (system->cart_size - 1));
	} else {
		*cycles += 2;
		return rom0_read_64(address, system);
	}
	uint64_t high = src[0];
	uint64_t highmid = src[1];
	uint64_t lowmid = src[2];
	uint64_t low = src[3];
	return high << 48 | highmid << 32 | lowmid << 16 | low;
}

uint32_t jag_write_phrase(jaguar_context *system, uint32_t address, uint64_t val)
{
	if (!system->memcon_written) {
		//unsure of timing, but presumably at least 2 32-bit reads 
		//which presumably take a minimum of 1 cycle
		//reality probably depends on the exact area read
		//docs seem to imply some areas only 16-bits wide whereas others are 32-bit
		rom0_write_64(address, system, val);
		return 2;
	}
	uint16_t *dst;
	uint32_t cycles;
	if (system->memcon1 & 1) {
		if (address < 0x800000) {
			dst = system->dram + (address >> 1 & (DRAM_WORDS - 1));
			//DRAM is 64-bits wide, but sounds like an access is still at least two cycles
			cycles = 2;
		} else if (address < 0xE00000) {
			dst = system->cart + (address >> 1 & (system->cart_size - 1));
			//cart is slow and only 32-bits wide
			cycles = 2 * (system->rom_cycles);
		} else {
			rom0_write_64(address, system, val);
			return 2;
		}
	} else if (address > 0x800000) {
		dst = system->dram + (address >> 1 & (DRAM_WORDS - 1));
		//DRAM is 64-bits wide, but sounds like an access is still at least two cycles
		cycles = 2;
	} else if (address > 0x200000) {
		dst = system->cart + (address >> 1 & (system->cart_size - 1));
		//cart is slow and only 32-bits wide
		cycles = 2 * (system->rom_cycles);
	} else {
		rom0_write_64(address, system, val);
		return 2;
	}
	dst[0] = val >> 48;
	dst[1] = val >> 32;
	dst[2] = val >> 16;
	dst[3] = val;
	return cycles;
}

m68k_context * sync_components(m68k_context * context, uint32_t address)
{
	jaguar_context *system = context->system;
	jag_video_run(system->video, context->current_cycle);
	jag_update_m68k_int(system);
	if (context->current_cycle > 0x10000000) {
		context->current_cycle -= 0x10000000;
		system->video->cycles -= 0x10000000;
	}
	if (context->int_ack) {
		context->int_ack = 0;
		//hack until 68K core more properly supports non-autovector interrupts
		context->status |= 1;
	}
	jag_update_m68k_int(system);
	return context;
}


void *rom0_write_m68k(uint32_t address, void *context, uint16_t value)
{
	sync_components(context, 0);
	rom0_write_16(address, ((m68k_context *)context)->system, value);
	return context;
}

uint16_t rom0_read_m68k(uint32_t address, void *context)
{
	sync_components(context, 0);
	return rom0_read_16(address, ((m68k_context *)context)->system);
}

void *rom0_write_m68k_b(uint32_t address, void *context, uint8_t value)
{
	sync_components(context, 0);
	//seems unlikely these areas support byte access
	uint16_t value16 = value;
	value16 |= value16 << 8;
	rom0_write_16(address, ((m68k_context *)context)->system, value16);
	return context;
}

uint8_t rom0_read_m68k_b(uint32_t address, void *context)
{
	sync_components(context, 0);
	uint16_t value = rom0_read_16(address, ((m68k_context *)context)->system);
	if (address & 1) {
		return value;
	}
	return value >> 8;
}

m68k_context *handle_m68k_reset(m68k_context *context)
{
	puts("M68K executed RESET");
	return context;
}

jaguar_context *init_jaguar(uint16_t *bios, uint32_t bios_size, uint16_t *cart, uint32_t cart_size)
{
	jaguar_context *system = calloc(1, sizeof(jaguar_context));
	system->bios = bios;
	system->bios_size = bios_size;
	system->cart = cart;
	system->cart_size = cart_size;
	//TODO: Figure out a better default for this and make it configurable
	system->max_cycles = 3000;

	memmap_chunk *jag_m68k_map = calloc(8, sizeof(memmap_chunk));
	for (uint32_t start = 0, index=0; index < 8; index++, start += 0x200000)
	{
		jag_m68k_map[index].start = start;
		jag_m68k_map[index].end = start + 0x200000;
		jag_m68k_map[index].mask = index ? 0x1FFFFF : 0xFFFFFF;
		jag_m68k_map[index].aux_mask = bios_size - 1;
		jag_m68k_map[index].ptr_index = index;
		jag_m68k_map[index].flags = MMAP_READ | MMAP_WRITE | MMAP_PTR_IDX | MMAP_FUNC_NULL | MMAP_AUX_BUFF | MMAP_CODE;
		jag_m68k_map[index].buffer = bios;
		jag_m68k_map[index].read_16 = rom0_read_m68k;
		jag_m68k_map[index].read_8 = rom0_read_m68k_b;
		jag_m68k_map[index].write_16 = rom0_write_m68k;
		jag_m68k_map[index].write_8 = rom0_write_m68k_b;
	}
	m68k_options *opts = malloc(sizeof(m68k_options));
	init_m68k_opts(opts, jag_m68k_map, 8, 2);
	system->m68k = init_68k_context(opts, handle_m68k_reset);
	system->m68k->sync_cycle = system->max_cycles;
	system->m68k->system = system;
	system->video = jag_video_init();
	system->video->system = system;
	return system;
}

//modified copy of the version in blastem.c
uint16_t *load_rom(char * filename, uint32_t *size)
{
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	fseek(f, 0, SEEK_SET);
	
	*size = nearest_pow2(filesize);
	uint16_t *cart = malloc(*size);
	if (filesize != fread(cart, 1, filesize, f)) {
		fatal_error("Error reading from %s\n", filename);
	}
	filesize = (filesize + 1) & ~1L;
	for (long i = 0; i < filesize; i+=2)
	{
		long index = i >> 1;
		cart[index] = cart[index] >> 8 | cart[index] << 8;
	}
	while (filesize < *size)
	{
		cart[filesize / 2] = 0xFFFF;
		filesize += 2;
	}
	fclose(f);
	return cart;
}

//temporary main function until I clean up blastem.c
int main(int argc, char **argv)
{
	if (argc < 3) {
		fputs("Usage: blastjag BIOS ROM\n", stderr);
		return 1;
	}
	set_exe_str(argv[0]);
	config = load_config(argv[0]);
	uint32_t bios_size;
	uint16_t *bios = load_rom(argv[1], &bios_size);
	if (!bios_size) {
		fatal_error("Failed to read BIOS from %s\n", argv[1]);
	}
	uint32_t cart_size;
	uint16_t *cart = load_rom(argv[2], &cart_size);
	if (!bios_size) {
		fatal_error("Failed to read cart from %s\n", argv[2]);
	}
	jaguar_context *system = init_jaguar(bios, bios_size, cart, cart_size);
	render_init(640, 480, "BlastJag", 0);
	m68k_reset(system->m68k);
	return 0;
}
