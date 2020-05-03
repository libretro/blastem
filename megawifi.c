#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#define WINVER 0x501
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include "genesis.h"
#include "net.h"

enum {
	TX_IDLE,
	TX_LEN1,
	TX_LEN2,
	TX_PAYLOAD,
	TX_WAIT_ETX
};
#define STX 0x7E
#define ETX 0x7E
#define MAX_RECV_SIZE 1460

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

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
	STATE_IDLE=1,
	STATE_AP_JOIN,
	STATE_SCAN,
	STATE_READY,
	STATE_TRANSPARENT
};

enum {
	SOCKST_NONE = 0,
	SOCKST_TCP_LISTEN,
	SOCKST_TCP_EST,
	SOCKST_UDP_READY
};


#define FLAG_ONLINE 

typedef struct {
	uint32_t transmit_bytes;
	uint32_t expected_bytes;
	uint32_t receive_bytes;
	uint32_t receive_read;
	int      sock_fds[15];
	uint16_t channel_flags;
	uint8_t  channel_state[15];
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
		megawifi *mw = gen->extra;
		mw->module_state = STATE_IDLE;
		for (int i = 0; i < 15; i++)
		{
			mw->sock_fds[i] = -1;
		}
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

static void mw_set(megawifi *mw, uint8_t val, uint32_t count)
{
	if (count + mw->receive_bytes > sizeof(mw->receive_buffer)) {
		count = sizeof(mw->receive_buffer) - mw->receive_bytes;
	}
	memset(mw->receive_buffer + mw->receive_bytes, val, count);
	mw->receive_bytes += count;
}

static void mw_copy(megawifi *mw, const uint8_t *src, uint32_t count)
{
	if (count + mw->receive_bytes > sizeof(mw->receive_buffer)) {
		count = sizeof(mw->receive_buffer) - mw->receive_bytes;
	}
	memcpy(mw->receive_buffer + mw->receive_bytes, src, count);
	mw->receive_bytes += count;
}

static void mw_putraw(megawifi *mw, const char *data, size_t len)
{
	if ((mw->receive_bytes + len) > sizeof(mw->receive_buffer)) {
		return;
	}
	memcpy(mw->receive_buffer + mw->receive_bytes, data, len);
	mw->receive_bytes += len;
}

static void mw_puts(megawifi *mw, const char *s)
{
	size_t len = strlen(s);
	mw_putraw(mw, s, len);
}

static void poll_socket(megawifi *mw, uint8_t channel)
{
	if (mw->sock_fds[channel] < 0) {
		return;
	}
	if (mw->channel_state[channel] == SOCKST_TCP_LISTEN) {
		int res = accept(mw->sock_fds[channel], NULL, NULL);
		if (res >= 0) {
			close(mw->sock_fds[channel]);
#ifndef _WIN32
//FIXME: Set nonblocking on Windows too
			fcntl(res, F_SETFL, O_NONBLOCK);
#endif
			mw->sock_fds[channel] = res;
			mw->channel_state[channel] = SOCKST_TCP_EST;
			mw->channel_flags |= 1 << (channel + 1);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			close(mw->sock_fds[channel]);
			mw->channel_state[channel] = SOCKST_NONE;
			mw->channel_flags |= 1 << (channel + 1);
		}
	} else if (mw->channel_state[channel] == SOCKST_TCP_EST && mw->receive_bytes < sizeof(mw->receive_buffer) - 4) {
		size_t max = sizeof(mw->receive_buffer) - 4 - mw->receive_bytes;
		if (max > MAX_RECV_SIZE) {
			max = MAX_RECV_SIZE;
		}
		int bytes = recv(mw->sock_fds[channel], mw->receive_buffer + mw->receive_bytes + 3, max, 0);
		if (bytes > 0) {
			mw_putc(mw, STX);
			mw_putc(mw, bytes >> 8 | (channel+1) << 4);
			mw_putc(mw, bytes);
			mw->receive_bytes += bytes;
			mw_putc(mw, ETX);
			//should this set the channel flag?
		} else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			close(mw->sock_fds[channel]);
			mw->channel_state[channel] = SOCKST_NONE;
			mw->channel_flags |= 1 << (channel + 1);
		}
	}
}

static void poll_all_sockets(megawifi *mw)
{
	for (int i = 0; i < 15; i++)
	{
		poll_socket(mw, i);
	}
}

static void start_reply(megawifi *mw, uint8_t cmd)
{
	mw_putc(mw, STX);
	//reserve space for length
	mw->receive_bytes += 2;
	//cmd
	mw_putc(mw, 0);
	mw_putc(mw, cmd);
	//reserve space for length
	mw->receive_bytes += 2;
}

static void end_reply(megawifi *mw)
{
	uint32_t len = mw->receive_bytes - 3;
	//LSD packet length
	mw->receive_buffer[1] = len >> 8;
	mw->receive_buffer[2] = len;
	//command length
	len -= 4;
	mw->receive_buffer[5] = len >> 8;
	mw->receive_buffer[6] = len;
	mw_putc(mw, ETX);
}

static void process_command(megawifi *mw)
{
	uint32_t command = mw->transmit_buffer[0] << 8 | mw->transmit_buffer[1];
	uint32_t size = mw->transmit_buffer[2] << 8 | mw->transmit_buffer[3];
	if (size > mw->transmit_bytes - 4) {
		size = mw->transmit_bytes - 4;
	}
	int orig_receive_bytes = mw->receive_bytes;
	switch (command)
	{
	case CMD_VERSION:
		start_reply(mw, CMD_OK);
		mw_putc(mw, 1);
		mw_putc(mw, 3);
		mw_putc(mw, 0);
		mw_puts(mw, "blastem");
		end_reply(mw);
		break;
	case CMD_ECHO:
		mw->receive_bytes = mw->transmit_bytes;
		memcpy(mw->receive_buffer, mw->transmit_buffer, mw->transmit_bytes);
		break;
	case CMD_IP_CURRENT: {
		iface_info i;
		if (get_host_address(&i)) {
			start_reply(mw, CMD_OK);
			//config number and reserved bytes
			mw_set(mw, 0, 4);
			//ip
			mw_copy(mw, i.ip, sizeof(i.ip));
			//net mask
			mw_copy(mw, i.net_mask, sizeof(i.net_mask));
			//gateway guess
			mw_putc(mw, i.ip[0] & i.net_mask[0]);
			mw_putc(mw, i.ip[1] & i.net_mask[1]);
			mw_putc(mw, i.ip[2] & i.net_mask[2]);
			mw_putc(mw, (i.ip[3] & i.net_mask[3]) + 1);
			//dns
			static const uint8_t localhost[] = {127,0,0,1};
			mw_copy(mw, localhost, sizeof(localhost));
			mw_copy(mw, localhost, sizeof(localhost));
			
		} else {
			start_reply(mw, CMD_ERROR);
		}
		end_reply(mw);
		break;
	}
	case CMD_AP_JOIN:
		mw->module_state = STATE_READY;
		start_reply(mw, CMD_OK);
		end_reply(mw);
		break;
	case CMD_TCP_BIND:{
		if (size < 7){
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		uint8_t channel = mw->transmit_buffer[10];
		if (!channel || channel > 15) {
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		channel--;
		if (mw->sock_fds[channel] >= 0) {
			close(mw->sock_fds[channel]);
		}
		mw->sock_fds[channel] = socket(AF_INET, SOCK_STREAM, 0);
		if (mw->sock_fds[channel] < 0) {
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		int value = 1;
		setsockopt(mw->sock_fds[channel], SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
		struct sockaddr_in bind_addr;
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_port = htons(mw->transmit_buffer[8] << 8 | mw->transmit_buffer[9]);
		if (bind(mw->sock_fds[channel], (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
			close(mw->sock_fds[channel]);
			mw->sock_fds[channel] = -1;
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		int res = listen(mw->sock_fds[channel], 2);
		start_reply(mw, res ? CMD_ERROR : CMD_OK);
		if (res) {
			close(mw->sock_fds[channel]);
			mw->sock_fds[channel] = -1;
		} else {
			mw->channel_flags |= 1 << (channel + 1);
			mw->channel_state[channel] = SOCKST_TCP_LISTEN;
#ifndef _WIN32
//FIXME: Set nonblocking on Windows too
			fcntl(mw->sock_fds[channel], F_SETFL, O_NONBLOCK);
#endif
		}
		end_reply(mw);
		break;
	}
	case CMD_SOCK_STAT: {
		uint8_t channel = mw->transmit_buffer[4];
		if (!channel || channel > 15) {
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		mw->channel_flags &= ~(1 << channel);
		channel--;
		poll_socket(mw, channel);
		start_reply(mw, CMD_OK);
		mw_putc(mw, mw->channel_state[channel]);
		end_reply(mw);
		break;
	}
	case CMD_SYS_STAT:
		poll_all_sockets(mw);
		start_reply(mw, CMD_OK);
		mw_putc(mw, mw->module_state);
		mw_putc(mw, mw->flags);
		mw_putc(mw, mw->channel_flags >> 8);
		mw_putc(mw, mw->channel_flags);
		end_reply(mw);
		break;
	default:
		printf("Unhandled MegaWiFi command %s(%d) with length %X\n", cmd_names[command], command, size);
		break;
	}
}

static void process_packet(megawifi *mw)
{
	if (mw->transmit_channel == 0) {
		process_command(mw);
	} else {
		uint8_t channel = mw->transmit_channel - 1;
		int channel_state = mw->channel_state[channel];
		int sock_fd = mw->sock_fds[channel];
		// TODO Handle UDP type sockets
		if (sock_fd >= 0 && channel_state == SOCKST_TCP_EST) {
			int sent = send(sock_fd, mw->transmit_buffer, mw->transmit_bytes, MSG_NOSIGNAL);
			if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
				close(sock_fd);
				mw->sock_fds[channel] = -1;
				mw->channel_state[channel] = SOCKST_NONE;
				mw->channel_flags |= 1 << mw->transmit_channel;
			} else if (sent < mw->transmit_bytes) {
				//TODO: save this data somewhere so it can be sent in poll_socket
				printf("Sent %d bytes on channel %d, but %d were requested\n", sent, mw->transmit_channel, mw->transmit_bytes);
			}
		} else {
			printf("Unhandled receive of MegaWiFi data on channel %d\n", mw->transmit_channel);
		}
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
		poll_all_sockets(mw);
		if (mw->receive_read < mw->receive_bytes) {
			uint8_t ret = mw->receive_buffer[mw->receive_read++];
			if (mw->receive_read == mw->receive_bytes) {
				mw->receive_read = mw->receive_bytes = 0;
			}
			return ret;
		}
		return 0xFF;
	case 5:
		poll_all_sockets(mw);
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
