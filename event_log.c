#ifdef _WIN32
#define WINVER 0x501
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/tcp.h>
#endif

#include <errno.h>
#include "event_log.h"
#include "util.h"
#include "blastem.h"
#include "saves.h"

enum {
	CMD_GAMEPAD_DOWN,
	CMD_GAMEPAD_UP,
};

static uint8_t active, fully_active;
static FILE *event_file;
static serialize_buffer buffer;

static const char el_ident[] = "BLSTEL\x02\x00";
static uint32_t last;
void event_log_file(char *fname)
{
	event_file = fopen(fname, "wb");
	if (!event_file) {
		warning("Failed to open event file %s for writing\n", fname);
		return;
	}
	fwrite(el_ident, 1, sizeof(el_ident) - 1, event_file);
	init_serialize(&buffer);
	active = fully_active = 1;
	last = 0;
}

static int listen_sock, remotes[7];
static int num_remotes;
void event_log_tcp(char *address, char *port)
{
	struct addrinfo request, *result;
	socket_init();
	memset(&request, 0, sizeof(request));
	request.ai_family = AF_INET;
	request.ai_socktype = SOCK_STREAM;
	request.ai_flags = AI_PASSIVE;
	getaddrinfo(address, port, &request, &result);
	
	listen_sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (listen_sock < 0) {
		warning("Failed to open event log listen socket on %s:%s\n", address, port);
		goto cleanup_address;
	}
	int param = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&param, sizeof(param));
	if (bind(listen_sock, result->ai_addr, result->ai_addrlen) < 0) {
		warning("Failed to bind event log listen socket on %s:%s\n", address, port);
		socket_close(listen_sock);
		goto cleanup_address;
	}
	if (listen(listen_sock, 3) < 0) {
		warning("Failed to listen for event log remotes on %s:%s\n", address, port);
		socket_close(listen_sock);
		goto cleanup_address;
	}
	socket_blocking(listen_sock, 0);
	active = 1;
cleanup_address:
	freeaddrinfo(result);
}

static uint8_t *system_start;
static size_t system_start_size;
void event_system_start(system_type stype, vid_std video_std, char *name)
{
	if (!active) {
		return;
	}
	save_int8(&buffer, stype);
	save_int8(&buffer, video_std);
	size_t name_len = strlen(name);
	if (name_len > 255) {
		name_len = 255;
	}
	save_int8(&buffer, name_len);
	save_buffer8(&buffer, name, strlen(name));
	if (!fully_active) {
		system_start = malloc(buffer.size);
		system_start_size = buffer.size;
		memcpy(system_start, buffer.data, buffer.size);
		buffer.size = 0;
	}
}

//header formats
//Single byte: 4 bit type, 4 bit delta (16-31)
//Three Byte: 8 bit type, 16-bit delta
//Four byte: 8-bit type, 24-bit signed delta
#define FORMAT_3BYTE 0xE0
#define FORMAT_4BYTE 0xF0
static void event_header(uint8_t type, uint32_t cycle)
{
	uint32_t delta = cycle - last;
	if (delta > 65535) {
		save_int8(&buffer, FORMAT_4BYTE | type);
		save_int8(&buffer, delta >> 16);
		save_int16(&buffer, delta);
	} else if (delta >= 16 && delta < 32) {
		save_int8(&buffer, type << 4 | (delta - 16));
	} else {
		save_int8(&buffer, FORMAT_3BYTE | type);
		save_int16(&buffer, delta);
	}
}

void event_cycle_adjust(uint32_t cycle, uint32_t deduction)
{
	if (!fully_active) {
		return;
	}
	event_header(EVENT_ADJUST, cycle);
	last = cycle - deduction;
	save_int32(&buffer, deduction);
}

static size_t remote_send_progress[7];
static uint8_t remote_needs_state[7];
static void flush_socket(void)
{
	int remote = accept(listen_sock, NULL, NULL);
	if (remote != -1) {
		if (num_remotes == 7) {
			socket_close(remote);
		} else {
			printf("remote %d connected\n", num_remotes);
			remotes[num_remotes] = remote;
			remote_needs_state[num_remotes++] = 1;
			current_system->save_state = EVENTLOG_SLOT + 1;
		}
	}
	size_t min_progress = 0;
	for (int i = 0; i < num_remotes; i++) {
		int sent = 1;
		if (remote_needs_state[i]) {
			remote_send_progress[i] = buffer.size;
		} else {
			uint8_t buffer[1500];
			int bytes = recv(remotes[i], buffer, sizeof(buffer), 0);
			for (int j = 0; j < bytes; j++)
			{
				uint8_t cmd = buffer[j];
				switch(cmd)
				{
				case CMD_GAMEPAD_DOWN:
				case CMD_GAMEPAD_UP: {
					++j;
					if (j < bytes) {
						uint8_t button = buffer[j];
						uint8_t pad = (button >> 5) + i + 1;
						button &= 0x1F;
						if (cmd == CMD_GAMEPAD_DOWN) {
							current_system->gamepad_down(current_system, pad, button);
						} else {
							current_system->gamepad_up(current_system, pad, button);
						}
					} else {
						warning("Received incomplete command %X\n", cmd);
					}
					break;
				}
				default:
					warning("Unrecognized remote command %X\n", cmd);
					j = bytes;
				}
			}
		}
		while (sent && buffer.size - remote_send_progress[i])
		{
			sent = send(remotes[i], buffer.data + remote_send_progress[i], buffer.size - remote_send_progress[i], 0);
			if (sent >= 0) {
				remote_send_progress[i] += sent;
			} else if (socket_error_is_wouldblock()) {
				socket_close(remotes[i]);
				remotes[i] = remotes[num_remotes-1];
				remote_send_progress[i] = remote_send_progress[num_remotes-1];
				remote_needs_state[i] = remote_needs_state[num_remotes-1];
				num_remotes--;
				i--;
				break;
			}
			if (remote_send_progress[i] > min_progress) {
				min_progress = remote_send_progress[i];
			}
		}
	}
	if (min_progress == buffer.size) {
		buffer.size = 0;
		memset(remote_send_progress, 0, sizeof(remote_send_progress));
	}
}

void event_log(uint8_t type, uint32_t cycle, uint8_t size, uint8_t *payload)
{
	if (!fully_active) {
		return;
	}
	event_header(type, cycle);
	last = cycle;
	save_buffer8(&buffer, payload, size);
	if (listen_sock && buffer.size > 1280) {
		flush_socket();
	}
}

static uint32_t last_word_address;
void event_vram_word(uint32_t cycle, uint32_t address, uint16_t value)
{
	uint32_t delta = address - last_word_address;
	if (delta < 256) {
		uint8_t buffer[3] = {delta, value >> 8, value};
		event_log(EVENT_VRAM_WORD_DELTA, cycle, sizeof(buffer), buffer);
	} else {
		uint8_t buffer[5] = {address >> 16, address >> 8, address, value >> 8, value};
		event_log(EVENT_VRAM_WORD, cycle, sizeof(buffer), buffer);
	}
	last_word_address = address;
}

static uint32_t last_byte_address;
void event_vram_byte(uint32_t cycle, uint16_t address, uint8_t byte, uint8_t auto_inc)
{
	uint32_t delta = address - last_byte_address;
	if (delta == 1) {
		event_log(EVENT_VRAM_BYTE_ONE, cycle, sizeof(byte), &byte);
	} else if (delta == auto_inc) {
		event_log(EVENT_VRAM_BYTE_AUTO, cycle, sizeof(byte), &byte);
	} else if (delta < 256) {
		uint8_t buffer[2] = {delta, byte};
		event_log(EVENT_VRAM_BYTE_DELTA, cycle, sizeof(buffer), buffer);
	} else {
		uint8_t buffer[3] = {address >> 8, address, byte};
		event_log(EVENT_VRAM_BYTE, cycle, sizeof(buffer), buffer);
	}
	last_byte_address = address;
}

static size_t send_all(int sock, uint8_t *data, size_t size, int flags)
{
	size_t total = 0, sent = 1;
	while(sent > 0 && total < size)
	{
		sent = send(sock, data + total, size - total, flags);
		if (sent > 0) {
			total += sent;
		}
	}
	return total;
}

void event_state(uint32_t cycle, serialize_buffer *state)
{
	if (!fully_active) {
		last = cycle;
	}
	uint8_t header[] = {
		EVENT_STATE << 4, last >> 24, last >> 16, last >> 8, last,
		last_word_address >> 16, last_word_address >> 8, last_word_address,
		last_byte_address >> 8, last_byte_address,
		state->size >> 16, state->size >> 8, state->size
	};
	for (int i = 0; i < num_remotes; i++)
	{
		if (remote_needs_state[i]) {
			if(
				send_all(remotes[i], system_start, system_start_size, 0) == system_start_size
				&& send_all(remotes[i], header, sizeof(header), 0) == sizeof(header)
				&& send_all(remotes[i], state->data, state->size, 0) == state->size
			) {
				remote_send_progress[i] = buffer.size;
				remote_needs_state[i] = 0;
				socket_blocking(remotes[i], 0);
				int flag = 1;
				setsockopt(remotes[i], IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
				fully_active = 1;
			} else {
				socket_close(remotes[i]);
				remotes[i] = remotes[num_remotes-1];
				remote_send_progress[i] = remote_send_progress[num_remotes-1];
				remote_needs_state[i] = remote_needs_state[num_remotes-1];
				num_remotes--;
				i--;
			}
		}
	}
}

void event_flush(uint32_t cycle)
{
	if (!active) {
		return;
	}
	if (fully_active) {
		event_log(EVENT_FLUSH, cycle, 0, NULL);
	}
	if (event_file) {
		fwrite(buffer.data, 1, buffer.size, event_file);
		fflush(event_file);
		buffer.size = 0;
	} else if (listen_sock) {
		flush_socket();
	}
}

void init_event_reader(event_reader *reader, uint8_t *data, size_t size)
{
	reader->socket = 0;
	reader->last_cycle = 0;
	init_deserialize(&reader->buffer, data, size);
}

void init_event_reader_tcp(event_reader *reader, char *address, char *port)
{
	struct addrinfo request, *result;
	socket_init();
	memset(&request, 0, sizeof(request));
	request.ai_family = AF_INET;
	request.ai_socktype = SOCK_STREAM;
	request.ai_flags = AI_PASSIVE;
	getaddrinfo(address, port, &request, &result);
	
	reader->socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
	if (reader->socket < 0) {
		fatal_error("Failed to create socket for event log connection to %s:%s\n", address, port);
	}
	if (connect(reader->socket, result->ai_addr, result->ai_addrlen) < 0) {
		fatal_error("Failed to connect to %s:%s for event log stream\n", address, port);
	}
	
	reader->storage = 512 * 1024;
	reader->last_cycle = 0;
	init_deserialize(&reader->buffer, malloc(reader->storage), reader->storage);
	reader->buffer.size = 0;
	while(reader->buffer.size < 3 || reader->buffer.size < 3 + reader->buffer.data[2])
	{
		int bytes = recv(reader->socket, reader->buffer.data + reader->buffer.size, reader->storage - reader->buffer.size, 0);
		if (bytes < 0) {
			fatal_error("Failed to receive system init from %s:%s\n", address, port);
		}
		reader->buffer.size += bytes;
	}
	socket_blocking(reader->socket, 0);
	int flag = 1;
	setsockopt(reader->socket, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
}

uint8_t reader_next_event(event_reader *reader, uint32_t *cycle_out)
{
	if (reader->socket) {
		uint8_t blocking = 0;
		if (reader->buffer.size - reader->buffer.cur_pos < 9) {
			//set back to block mode
			socket_blocking(reader->socket, 1);
			blocking = 1;
		}
		if (reader->storage - (reader->buffer.size - reader->buffer.cur_pos) < 128 * 1024) {
			reader->storage *= 2;
			uint8_t *new_buf = malloc(reader->storage);
			memcpy(new_buf, reader->buffer.data + reader->buffer.cur_pos, reader->buffer.size - reader->buffer.cur_pos);
			free(reader->buffer.data);
			reader->buffer.data = new_buf;
			reader->buffer.size -= reader->buffer.cur_pos;
			reader->buffer.cur_pos = 0;
		} else if (reader->buffer.cur_pos >= reader->buffer.size/2 && reader->buffer.size >= reader->storage/2) {
			memmove(reader->buffer.data, reader->buffer.data + reader->buffer.cur_pos, reader->buffer.size - reader->buffer.cur_pos);
			reader->buffer.size -= reader->buffer.cur_pos;
			reader->buffer.cur_pos = 0;
		}
		int bytes = 128;
		while (bytes > 127 && reader->buffer.size < reader->storage)
		{
			bytes = recv(reader->socket, reader->buffer.data + reader->buffer.size, reader->storage - reader->buffer.size, 0);
			if (bytes >= 0) {
				reader->buffer.size += bytes;
				if (blocking && reader->buffer.size - reader->buffer.cur_pos >= 9) {
					socket_blocking(reader->socket, 0);
				}
			} else if (!socket_error_is_wouldblock()) {
				printf("Connection closed, error = %X\n", socket_last_error());
			}
		}
	}
	uint8_t header = load_int8(&reader->buffer);
	uint8_t ret;
	uint32_t delta;
	if ((header & 0xF0) < FORMAT_3BYTE) {
		delta = (header & 0xF) + 16;
		ret = header >> 4;
	} else if ((header & 0xF0) == FORMAT_3BYTE) {
		delta = load_int16(&reader->buffer);
		ret = header & 0xF;
	} else {
		delta = load_int8(&reader->buffer) << 16;
		//sign extend 24-bit delta to 32-bit
		if (delta & 0x800000) {
			delta |= 0xFF000000;
		}
		delta |= load_int16(&reader->buffer);
		ret = header & 0xF;
	}
	*cycle_out = reader->last_cycle + delta;
	reader->last_cycle = *cycle_out;
	if (ret == EVENT_ADJUST) {
		size_t old_pos = reader->buffer.cur_pos;
		uint32_t adjust = load_int32(&reader->buffer);
		reader->buffer.cur_pos = old_pos;
		reader->last_cycle -= adjust;
	} else if (ret == EVENT_STATE) {
		reader->last_cycle = load_int32(&reader->buffer);
		reader->last_word_address = load_int8(&reader->buffer) << 16;
		reader->last_word_address |= load_int16(&reader->buffer);
		reader->last_byte_address = load_int16(&reader->buffer);
	}
	return ret;
}

uint8_t reader_system_type(event_reader *reader)
{
	return load_int8(&reader->buffer);
}

void reader_send_gamepad_event(event_reader *reader, uint8_t pad, uint8_t button, uint8_t down)
{
	uint8_t buffer[] = {down ? CMD_GAMEPAD_DOWN : CMD_GAMEPAD_UP, pad << 5 | button};
	//TODO: Deal with the fact that we're not in blocking mode so this may not actually send all
	//if the buffer is full
	send_all(reader->socket, buffer, sizeof(buffer), 0);
}
