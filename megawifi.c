#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "genesis.h"

enum {
	TX_IDLE,
	TX_LEN1,
	TX_LEN2,
	TX_PAYLOAD,
	TX_WAIT_ETX
};
#define STX 0x7E
#define ETX 0x7E

#define E(N) N
enum {
#include "mw_commands.c"
	CMD_ERROR = 255
};
#undef E
#define E(N) #N
static const char *cmd_names[] = {
#include "mw_commands.c"
	[255] = "CMD_ERROR"
};

enum {
	STATE_IDLE=1,
	STATE_AP_JOIN,
	STATE_SCAN,
	STATE_READY,
	STATE_TRANSPARENT
};

#define FLAG_ONLINE 

typedef struct {
	uint32_t transmit_bytes;
	uint32_t expected_bytes;
	uint32_t receive_bytes;
	uint32_t receive_read;
	uint16_t channel_flags;
	uint8_t  scratchpad;
	uint8_t  transmit_channel;
	uint8_t  transmit_state;
	uint8_t  module_state;
	uint8_t  flags;
	uint8_t  transmit_buffer[4096];
	uint8_t  receive_buffer[4096];
} megawifi;

static megawifi *get_megawifi(void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (!gen->extra) {
		gen->extra = calloc(1, sizeof(megawifi));
		((megawifi *)gen->extra)->module_state = STATE_IDLE;
	}
	return gen->extra;
}

static void mw_putc(megawifi *mw, uint8_t v)
{
	if (mw->receive_bytes == sizeof(mw->receive_buffer)) {
		return;
	}
	mw->receive_buffer[mw->receive_bytes++] = v;
}

static void mw_puts(megawifi *mw, char *s)
{
	uint32_t len = strlen(s);
	if ((mw->receive_bytes + len) > sizeof(mw->receive_buffer)) {
		return;
	}
	memcpy(mw->receive_buffer + mw->receive_bytes, s, len);
	mw->receive_bytes += len;
}

static void process_packet(megawifi *mw)
{
	if (mw->transmit_channel == 0) {
		uint32_t command = mw->transmit_buffer[0] << 8 | mw->transmit_buffer[1];
		uint32_t size = mw->transmit_buffer[2] << 8 | mw->transmit_buffer[3];
		if (size > mw->transmit_bytes - 4) {
			size = mw->transmit_bytes - 4;
		}
		mw->receive_read = mw->receive_bytes = 0;
		switch (command)
		{
		case CMD_VERSION:
			//LSD header
			mw_putc(mw, 0x7E);
			mw_putc(mw, 0);
			mw->receive_bytes += 1; //reserve space for LSB of len
			//cmd
			mw_putc(mw, 0);
			mw_putc(mw, CMD_OK);
			//length
			mw_putc(mw, 0);
			mw->receive_bytes += 1; //reserve space for LSB of len
			mw_putc(mw, 1);
			mw_putc(mw, 0);
			mw_puts(mw, "blastem");
			mw->receive_buffer[2] = mw->receive_bytes - 3;
			mw->receive_buffer[6] = mw->receive_bytes - 7;
			mw_putc(mw, 0x7E);
			break;
		case CMD_ECHO:
			mw->receive_bytes = mw->transmit_bytes;
			memcpy(mw->receive_buffer, mw->transmit_buffer, mw->transmit_bytes);
			break;
		case CMD_AP_JOIN:
			mw->module_state = STATE_READY;
			mw_putc(mw, 0x7E);
			mw_putc(mw, 0);
			mw_putc(mw, 4);
			//cmd
			mw_putc(mw, 0);
			mw_putc(mw, CMD_OK);
			//length
			mw_putc(mw, 0);
			mw_putc(mw, 0);
			mw_putc(mw, 0x7E);
			break;
		case CMD_SYS_STAT:
			//LSD header
			mw_putc(mw, 0x7E);
			mw_putc(mw, 0);
			mw_putc(mw, 8);
			//cmd
			mw_putc(mw, 0);
			mw_putc(mw, CMD_OK);
			//length
			mw_putc(mw, 0);
			mw_putc(mw, 4);
			mw_putc(mw, mw->module_state);
			mw_putc(mw, mw->flags);
			mw_putc(mw, mw->channel_flags >> 8);
			mw_putc(mw, mw->channel_flags);
			mw_putc(mw, 0x7E);
			break;
		default:
			printf("Unhandled MegaWiFi command %s(%d) with length %X\n", cmd_names[command], command, size);
			break;
		}
	} else {
		printf("Unhandled receive of MegaWiFi data on channel %d\n", mw->transmit_channel);
	}
	mw->transmit_bytes = mw->expected_bytes = 0;
}

void *megawifi_write_b(uint32_t address, void *context, uint8_t value)
{
	if (!(address & 1)) {
		return context;
	}
	megawifi *mw = get_megawifi(context);
	address = address >> 1 & 7;
	switch (address)
	{
	case 0:
		switch (mw->transmit_state)
		{
		case TX_IDLE:
			if (value == STX) {
				mw->transmit_state = TX_LEN1;
			}
			break;
		case TX_LEN1:
			mw->transmit_channel = value >> 4;
			mw->expected_bytes = value << 8 & 0xF00;
			mw->transmit_state = TX_LEN2;
			break;
		case TX_LEN2:
			mw->expected_bytes |= value;
			mw->transmit_state = TX_PAYLOAD;
			break;
		case TX_PAYLOAD:
			mw->transmit_buffer[mw->transmit_bytes++] = value;
			if (mw->transmit_bytes == mw->expected_bytes) {
				mw->transmit_state = TX_WAIT_ETX;
			}
			break;
		case TX_WAIT_ETX:
			if (value == ETX) {
				mw->transmit_state = TX_IDLE;
				process_packet(mw);
			}
			break;
		}
		break;
	case 7:
		mw->scratchpad = value;
		break;
	default:
		printf("Unhandled write to MegaWiFi UART register %X: %X\n", address, value);
	}
	return context;
}

void *megawifi_write_w(uint32_t address, void *context, uint16_t value)
{
	return megawifi_write_b(address | 1, context, value);
}

uint8_t megawifi_read_b(uint32_t address, void *context)
{
	
	if (!(address & 1)) {
		return 0xFF;
	}
	megawifi *mw = get_megawifi(context);
	address = address >> 1 & 7;
	switch (address)
	{
	case 0:
		if (mw->receive_read < mw->receive_bytes) {
			return mw->receive_buffer[mw->receive_read++];
		}
		return 0xFF;
	case 5:
		//line status
		return 0x60 | (mw->receive_read < mw->receive_bytes);
	case 7:
		return mw->scratchpad;
	default:
		printf("Unhandled read from MegaWiFi UART register %X\n", address);
		return 0xFF;
	}
}

uint16_t megawifi_read_w(uint32_t address, void *context)
{
	return 0xFF00 | megawifi_read_b(address | 1, context);
}
