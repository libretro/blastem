#include "68kinst.h"
#include "m68k_to_x86.h"
#include "mem.h"
#include "vdp.h"
#include "render.h"
#include "blastem.h"
#include <stdio.h>
#include <stdlib.h>

#define CARTRIDGE_WORDS 0x200000
#define RAM_WORDS 32 * 1024
#define Z80_RAM_BYTES 8 * 1024
#define MCLKS_PER_68K 7
//TODO: Figure out the exact value for this
#define MCLKS_PER_FRAME (MCLKS_LINE*262)
#define CYCLE_NEVER 0xFFFFFFFF

uint16_t cart[CARTRIDGE_WORDS];
uint16_t ram[RAM_WORDS];
uint8_t z80_ram[Z80_RAM_BYTES];

io_port gamepad_1;
io_port gamepad_2;

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif

#define SMD_HEADER_SIZE 512
#define SMD_MAGIC1 0x03
#define SMD_MAGIC2 0xAA
#define SMD_MAGIC3 0xBB
#define SMD_BLOCK_SIZE 0x4000

int load_smd_rom(long filesize, FILE * f)
{
	uint8_t block[SMD_BLOCK_SIZE];
	filesize -= SMD_HEADER_SIZE;
	fseek(f, SMD_HEADER_SIZE, SEEK_SET);
	
	uint16_t * dst = cart;
	while (filesize > 0) {
		fread(block, 1, SMD_BLOCK_SIZE, f);
		for (uint8_t *low = block, *high = (block+SMD_BLOCK_SIZE/2), *end = block+SMD_BLOCK_SIZE; high < end; high++, low++) {
			*(dst++) = *high << 8 | *low;
		}
		filesize -= SMD_BLOCK_SIZE;
	}
	return 1;
}

int load_rom(char * filename)
{
	uint8_t header[10];
	FILE * f = fopen(filename, "rb");
	if (!f) {
		return 0;
	}
	fread(header, 1, sizeof(header), f);
	fseek(f, 0, SEEK_END);
	long filesize = ftell(f);
	if (filesize/2 > CARTRIDGE_WORDS) {
		//carts bigger than 4MB not currently supported
		filesize = CARTRIDGE_WORDS*2;
	}
	fseek(f, 0, SEEK_SET);
	if (header[1] == SMD_MAGIC1 && header[8] == SMD_MAGIC2 && header[9] == SMD_MAGIC3) {
		int i;
		for (i = 3; i < 8; i++) {
			if (header[i] != 0) {
				break;
			}
		}
		if (i == 8) {
			if (header[2]) {
				fprintf(stderr, "%s is a split SMD ROM which is not currently supported", filename);
				exit(1);
			}
			return load_smd_rom(filesize, f);
		}
	}
	fread(cart, 2, filesize/2, f);
	fclose(f);
	for(unsigned short * cur = cart; cur - cart < (filesize/2); ++cur)
	{
		*cur = (*cur >> 8) | (*cur << 8);
	}
	//TODO: Mirror ROM
	return 1;
}

uint16_t read_dma_value(uint32_t address)
{
	//addresses here are word addresses (i.e. bit 0 corresponds to A1), so no need to do div by 2
	if (address < 0x200000) {
		return cart[address];
	} else if(address >= 0x700000) {
		return ram[address & 0x7FFF];
	}
	//TODO: Figure out what happens when you try to DMA from weird adresses like IO or banked Z80 area
	return 0;
}

#define VINT_CYCLE ((MCLKS_LINE * 226)/MCLKS_PER_68K)

m68k_context * sync_components(m68k_context * context)
{
	//TODO: Handle sync targets smaller than a single frame
	vdp_context * v_context = context->next_context;
	uint32_t mclks = context->current_cycle * MCLKS_PER_68K;
	if (mclks >= MCLKS_PER_FRAME) {
		//printf("reached frame end | 68K Cycles: %d, MCLK Cycles: %d\n", context->current_cycle, mclks);
		vdp_run_context(v_context, MCLKS_PER_FRAME);
		wait_render_frame(v_context);
		mclks -= MCLKS_PER_FRAME;
		vdp_adjust_cycles(v_context, MCLKS_PER_FRAME);
		io_adjust_cycles(&gamepad_1, context->current_cycle, MCLKS_PER_FRAME/MCLKS_PER_68K);
		io_adjust_cycles(&gamepad_2, context->current_cycle, MCLKS_PER_FRAME/MCLKS_PER_68K);
		context->current_cycle -= MCLKS_PER_FRAME/MCLKS_PER_68K;
		if (mclks) {
			vdp_run_context(v_context, mclks);
		}
		if (v_context->regs[REG_MODE_2] & 0x20 && ((context->status & 0x7) < 6)) {
			if (context->int_cycle > VINT_CYCLE) {
				context->int_cycle = VINT_CYCLE;
				context->int_num = 6;
				if (context->int_cycle < context->sync_cycle) {
					context->target_cycle = context->int_cycle;
				}
			}
		} else {
			context->int_cycle = 0xFFFFFFFF;
			context->target_cycle = context->sync_cycle;
		}
	} else {
		//printf("running VDP for %d cycles\n", mclks - v_context->cycles);
		vdp_run_context(v_context, mclks);
		if (v_context->regs[REG_MODE_2] & 0x20 && ((context->status & 0x7) < 6)) {
			if (context->int_cycle > VINT_CYCLE) {
				context->int_cycle = VINT_CYCLE;
				context->int_num = 6;
				if (context->int_cycle < context->sync_cycle && context->int_cycle < context->current_cycle) {
					context->target_cycle = context->int_cycle;
				}
			}
			if (context->int_cycle <= context->current_cycle) {
				context->int_cycle = CYCLE_NEVER;
				context->target_cycle = context->sync_cycle;
			}
		} else {
			context->int_cycle = CYCLE_NEVER;
			context->target_cycle = context->sync_cycle;
		}
	}
	return context;
}

m68k_context * vdp_port_write(uint32_t vdp_port, m68k_context * context, uint16_t value)
{
	//printf("vdp_port write: %X, value: %X, cycle: %d\n", vdp_port, value, context->current_cycle);
	sync_components(context);
	vdp_context * v_context = context->next_context;
	if (vdp_port < 0x10) {
		int blocked;
		if (vdp_port < 4) {
			while (vdp_data_port_write(v_context, value) < 0) {
				while(v_context->flags & FLAG_DMA_RUN) {
					vdp_run_dma_done(v_context, MCLKS_PER_FRAME);
					if (v_context->cycles >= MCLKS_PER_FRAME) {
						wait_render_frame(v_context);
						vdp_adjust_cycles(v_context, MCLKS_PER_FRAME);
						io_adjust_cycles(&gamepad_1, v_context->cycles/MCLKS_PER_68K, MCLKS_PER_FRAME/MCLKS_PER_68K);
						io_adjust_cycles(&gamepad_2, v_context->cycles/MCLKS_PER_68K, MCLKS_PER_FRAME/MCLKS_PER_68K);
					}
				}
				context->current_cycle = v_context->cycles / MCLKS_PER_68K;
			}
		} else if(vdp_port < 8) {
			blocked = vdp_control_port_write(v_context, value);
			if (blocked) {
				while (blocked) {
					while(v_context->flags & FLAG_DMA_RUN) {
						vdp_run_dma_done(v_context, MCLKS_PER_FRAME);
						if (v_context->cycles >= MCLKS_PER_FRAME) {
							wait_render_frame(v_context);
							vdp_adjust_cycles(v_context, MCLKS_PER_FRAME);
							io_adjust_cycles(&gamepad_1, v_context->cycles/MCLKS_PER_68K, MCLKS_PER_FRAME/MCLKS_PER_68K);
							io_adjust_cycles(&gamepad_2, v_context->cycles/MCLKS_PER_68K, MCLKS_PER_FRAME/MCLKS_PER_68K);
						}
					}
					if (blocked < 0) {
						blocked = vdp_control_port_write(v_context, value);
					} else {
						blocked = 0;
					}
				}
				context->current_cycle = v_context->cycles / MCLKS_PER_68K;
			} else {
				if (v_context->regs[REG_MODE_2] & 0x20 && ((context->status & 0x7) < 6)) {
					if (context->int_cycle > VINT_CYCLE) {
						context->int_cycle = VINT_CYCLE;
						context->int_num = 6;
						if (context->int_cycle < context->sync_cycle) {
							context->target_cycle = context->int_cycle;
						}
					}
				} else {
					context->int_cycle = 0xFFFFFFFF;
					context->target_cycle = context->sync_cycle;
				}
			}
		} else {
			printf("Illegal write to HV Counter port %X\n", vdp_port);
			exit(1);
		}
		context->current_cycle = v_context->cycles/MCLKS_PER_68K;
	} else if (vdp_port < 0x18) {
		//TODO: Implement PSG
	} else {
		//TODO: Implement undocumented test register(s)
	}
	return context;
}

m68k_context * vdp_port_read(uint32_t vdp_port, m68k_context * context)
{
	sync_components(context);
	vdp_context * v_context = context->next_context;
	if (vdp_port < 0x10) {
		if (vdp_port < 4) {
			context->value = vdp_data_port_read(v_context);
		} else if(vdp_port < 8) {
			context->value = vdp_control_port_read(v_context);
		} else {
			context->value = vdp_hv_counter_read(v_context);
			//printf("HV Counter: %X at cycle %d\n", context->value, v_context->cycles);
		}
		context->current_cycle = v_context->cycles/MCLKS_PER_68K;
	} else {
		printf("Illegal read from PSG or test register port %X\n", vdp_port);
		exit(1);
	}
	return context;
}

#define TH 0x40
#define TH_TIMEOUT 8000
#define Z80_ACK_DELAY 3 //TODO: Calculate this on the fly based on how synced up the Z80 and 68K clocks are

uint8_t reset = 1;
uint8_t busreq = 0;
uint8_t busack = 0;
uint32_t busack_cycle = CYCLE_NEVER;
uint8_t new_busack = 0;

void io_adjust_cycles(io_port * pad, uint32_t current_cycle, uint32_t deduction)
{
	/*uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("adjust_cycles | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, current_cycle);
	}*/
	if (current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	} else {
		pad->timeout_cycle -= deduction;
	}
	if (busack_cycle < CYCLE_NEVER && current_cycle < busack_cycle) {
		busack_cycle -= deduction;
	}
}

void io_data_write(io_port * pad, m68k_context * context, uint8_t value)
{
	if (pad->control & TH) {
		//check if TH has changed
		if ((pad->output & TH) ^ (value & TH)) {
			if (context->current_cycle >= pad->timeout_cycle) {
				pad->th_counter = 0;
			}
			if (!(value & TH)) {
				pad->th_counter++;
			}
			pad->timeout_cycle = context->current_cycle + TH_TIMEOUT;
		}
	}
	pad->output = value;
}

void io_data_read(io_port * pad, m68k_context * context)
{
	uint8_t control = pad->control | 0x80;
	uint8_t th = control & pad->output;
	uint8_t input;
	if (context->current_cycle >= pad->timeout_cycle) {
		pad->th_counter = 0;
	}
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf("io_data_read | control: %X, TH: %X, GAMEPAD_TH0: %X, GAMEPAD_TH1: %X, TH Counter: %d, Timeout: %d, Cycle: %d\n", control, th, pad->input[GAMEPAD_TH0], pad->input[GAMEPAD_TH1], pad->th_counter,pad->timeout_cycle, context->current_cycle);
	}*/
	if (th) {
		if (pad->th_counter == 2) {
			input = pad->input[GAMEPAD_EXTRA];
		} else {
			input = pad->input[GAMEPAD_TH1];
		}
	} else {
		if (pad->th_counter == 2) {
			input = pad->input[GAMEPAD_TH0] | 0xF;
		} else if(pad->th_counter == 3) {
			input = pad->input[GAMEPAD_TH0]  & 0x30;
		} else {
			input = pad->input[GAMEPAD_TH0] | 0xC;
		}
	}
	context->value = ((~input) & (~control)) | (pad->output & control);
	/*if (pad->input[GAMEPAD_TH0] || pad->input[GAMEPAD_TH1]) {
		printf ("value: %X\n", context->value);
	}*/
}

m68k_context * io_write(uint32_t location, m68k_context * context, uint8_t value)
{
	if (location < 0x10000) {
		if (busack_cycle > context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				z80_ram[location & 0x1FFF] = value;
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(&gamepad_1, context, value);
				break;
			case 0x2:
				io_data_write(&gamepad_2, context, value);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				gamepad_1.control = value;
				break;
			case 0x5:
				gamepad_2.control = value;
				break;
			}
		} else {
			if (location == 0x1100) {
				if (busack_cycle > context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				if (value & 1) {
					busreq = 1;
					if(!reset) {
						busack_cycle = context->current_cycle + Z80_ACK_DELAY;
						new_busack = 0;
					}
				} else {
					busreq = 0;
					busack_cycle = CYCLE_NEVER;
					busack = 1;
				}
			} else if (location == 0x1200) {
				if (value & 1) {
					if (reset && busreq) {
						new_busack = 0;
						busack_cycle = context->current_cycle + Z80_ACK_DELAY;
					}
					reset = 0;
				} else {
					reset = 1;
				}
			}
		}
	}
	return context;
}

m68k_context * io_write_w(uint32_t location, m68k_context * context, uint16_t value)
{
	if (location < 0x10000) {
		if (busack_cycle > context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				z80_ram[location & 0x1FFE] = value >> 8;
			}
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x1:
				io_data_write(&gamepad_1, context, value);
				break;
			case 0x2:
				io_data_write(&gamepad_2, context, value);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				gamepad_1.control = value;
				break;
			case 0x5:
				gamepad_2.control = value;
				break;
			}
		} else {
			//printf("IO Write of %X to %X @ %d\n", value, location, context->current_cycle);
			if (location == 0x1100) {
				if (busack_cycle > context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				if (value & 0x100) {
					busreq = 1;
					if(!reset) {
						busack_cycle = context->current_cycle + Z80_ACK_DELAY;
						new_busack = 0;
					}
				} else {
					busreq = 0;
					busack_cycle = CYCLE_NEVER;
					busack = 1;
				}
			} else if (location == 0x1200) {
				if (value & 0x100) {
					if (reset && busreq) {
						new_busack = 0;
						busack_cycle = context->current_cycle + Z80_ACK_DELAY;
					}
					reset = 0;
				} else {
					reset = 1;
				}
			}
		}
	}
	return context;
}

#define USA 0x80
#define JAP 0x00
#define EUR 0xC0
#define NO_DISK 0x20
uint8_t version_reg = NO_DISK | USA;

m68k_context * io_read(uint32_t location, m68k_context * context)
{
	if (location < 0x10000) {
		if (busack_cycle > context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				context->value = z80_ram[location & 0x1FFF];
			} else {
				context->value = 0xFF;
			}
		} else {
			context->value = 0xFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				//Not sure about the other bits
				context->value = version_reg;
				break;
			case 0x1:
				io_data_read(&gamepad_1, context);
				break;
			case 0x2:
				io_data_read(&gamepad_2, context);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				context->value = gamepad_1.control;
				break;
			case 0x5:
				context->value = gamepad_2.control;
				break;
			}
		} else {
			if (location == 0x1100) {
				if (busack_cycle > context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				context->value = reset || busack;
				//printf("Byte read of BUSREQ returned %d @ %d (reset: %d, busack: %d)\n", context->value, context->current_cycle, reset, busack);
			} else if (location == 0x1200) {
				context->value = !reset;
			} else {
				printf("Byte read of unknown IO location: %X\n", location);
			}
		}
	}
	return context;
}

m68k_context * io_read_w(uint32_t location, m68k_context * context)
{
	if (location < 0x10000) {
		if (busack_cycle > context->current_cycle) {
			busack = new_busack;
			busack_cycle = CYCLE_NEVER;
		}
		if (!(busack || reset)) {
			location &= 0x7FFF;
			if (location < 0x4000) {
				context->value = z80_ram[location & 0x1FFE];
				context->value |= context->value << 8;
			} else {
				context->value = 0xFFFF;
			}
		} else {
			context->value = 0xFFFF;
		}
	} else {
		location &= 0x1FFF;
		if (location < 0x100) {
			switch(location/2)
			{
			case 0x0:
				//version bits should be 0 for now since we're not emulating TMSS
				//Not sure about the other bits
				context->value = 0;
				break;
			case 0x1:
				io_data_read(&gamepad_1, context);
				break;
			case 0x2:
				io_data_read(&gamepad_2, context);
				break;
			case 0x3://PORT C Data
				break;
			case 0x4:
				context->value = gamepad_1.control;
				break;
			case 0x5:
				context->value = gamepad_2.control;
				break;
			case 0x6:
				//PORT C Control
				context->value = 0;
				break;
			}
			context->value = context->value | (context->value << 8);
			//printf("Word read to %X returned %d\n", location, context->value);
		} else {
			if (location == 0x1100) {
				if (busack_cycle > context->current_cycle) {
					busack = new_busack;
					busack_cycle = CYCLE_NEVER;
				}
				context->value = (reset || busack) << 8;
				//printf("Word read of BUSREQ returned %d\n", context->value);
			} else if (location == 0x1200) {
				context->value = (!reset) << 8;
			} else {
				printf("Word read of unknown IO location: %X\n", location);
			}
		}
	}
	return context;
}

int main(int argc, char ** argv)
{
	if (argc < 2) {
		fputs("Usage: blastem FILENAME\n", stderr);
		return 1;
	}
	if(!load_rom(argv[1])) {
		fprintf(stderr, "Failed to open %s for reading\n", argv[1]);
		return 1;
	}
	int width = 320;
	int height = 240;
	if (argc > 2) {
		width = atoi(argv[2]);
		if (argc > 3) {
			height = atoi(argv[3]);
		} else {
			height = (width/320) * 240;
		}
	}
	render_init(width, height);
	
	x86_68k_options opts;
	m68k_context context;
	vdp_context v_context;
	
	init_x86_68k_opts(&opts);
	init_68k_context(&context, opts.native_code_map, &opts);
	init_vdp_context(&v_context);
	context.next_context = &v_context;
	//cartridge ROM
	context.mem_pointers[0] = cart;
	context.target_cycle = context.sync_cycle = MCLKS_PER_FRAME/MCLKS_PER_68K;
	//work RAM
	context.mem_pointers[1] = ram;
	uint32_t address;
	/*address = cart[0x68/2] << 16 | cart[0x6A/2];
	translate_m68k_stream(address, &context);
	address = cart[0x70/2] << 16 | cart[0x72/2];
	translate_m68k_stream(address, &context);
	address = cart[0x78/2] << 16 | cart[0x7A/2];
	translate_m68k_stream(address, &context);*/
	address = cart[2] << 16 | cart[3];
	translate_m68k_stream(address, &context);
	m68k_reset(&context);
	return 0;
}
