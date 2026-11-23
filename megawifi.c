#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#define WINVER 0x501
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/param.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include "genesis.h"
#include "net.h"
#include "util.h"
#include "paths.h"

#if defined(_WIN32) || defined(__APPLE__)
#  if BYTE_ORDER == LITTLE_ENDIAN
#define htobe64(val)   ((((uint64_t)htonl((val)&0xFFFFFFFF))<<32) | htonl((val)>>32))
#  else
#define htobe64(val)	(val)
#  endif
#endif

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

enum mw_state {
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

// TCP/UDP address message
struct mw_addr_msg {
	char dst_port[6];
	char src_port[6];
	uint8_t channel;
	char host[];
};

#define FLAG_ONLINE 

#define NUM_AP_CFGS    3
#define NUM_GAMERTAGS  3
#define SSID_MAXLEN    32
#define PASS_MAXLEN    64
#define IP_CFG_LEN     20 //ip, mask, gateway, dns1, dns2
#define NTP_POOL_MAXLEN 144
#define SERVER_URL_MAXLEN 64
#define WIFI_ADV_LEN   24
#define GAMERTAG_LEN   964
#define FLASH_SIZE     0x200000 //user partition of the module flash
#define FLASH_SECT_LEN 0x1000
#define DEF_CFG_MAGIC  0xFEAA5501
#define PHY_11B        1
#define PHY_11BG       3
#define PHY_11BGN      7

//The module's non-volatile configuration. The firmware keeps it in RAM and
//only writes it to flash on NV_CFG_SAVE and FACTORY_RESET, so this does too.
typedef struct {
	char     ssid[NUM_AP_CFGS][SSID_MAXLEN];
	char     pass[NUM_AP_CFGS][PASS_MAXLEN];
	uint8_t  phy[NUM_AP_CFGS];
	uint8_t  ip_cfg[NUM_AP_CFGS][IP_CFG_LEN];
	uint8_t  default_ap; //0xFF when there is none
	uint8_t  ntp_pool_len[2];
	uint8_t  ntp_pool[NTP_POOL_MAXLEN];
	char     server_url[SERVER_URL_MAXLEN];
	uint8_t  wifi_adv[WIFI_ADV_LEN];
	uint8_t  gamertag[NUM_GAMERTAGS][GAMERTAG_LEN];
} mw_config;

#define CONFIG_MAGIC "BEMWCFG1"

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
	struct sockaddr_in remote_addr[15];	// Needed for UDP sockets
	char     *storage_base; //module config and flash files are this plus an extension
	mw_config config;
} megawifi;

static char *storage_prefix;

void megawifi_set_storage_prefix(const char *prefix)
{
	free(storage_prefix);
	storage_prefix = prefix ? strdup(prefix) : NULL;
}

static char *storage_path(megawifi *mw, const char *ext)
{
	if (!mw->storage_base) {
		return NULL;
	}
	return alloc_concat(mw->storage_base, ext);
}

static void default_config(mw_config *cfg)
{
	static const char ntp_pool[] = "GMT\0" "0.pool.ntp.org\0" "1.pool.ntp.org\0" "2.pool.ntp.org\0";
	//qos, ampdu rx, rx ba win, rx ampdu buf num, rx ampdu buf len, rx max single pkt len,
	//rx buf len, amsdu rx, rx buf num, rx pkt num, left continuous rx buf num, tx buf num
	static const uint8_t wifi_adv[WIFI_ADV_LEN] = {
		1, 1, 6, 5, 0, 0, 0x01, 0x00, 0, 0, 0x06, 0x40, 0, 0, 0x06, 0x40,
		0, 16, 7, 4, 6, 0, 0, 0
	};
	memset(cfg, 0, sizeof(*cfg));
	//A game can only get online through a configured access point, so slot 0
	//holds the network the host is already on and it is the default one.
	strcpy(cfg->ssid[0], "BlastEm");
	for (int i = 0; i < NUM_AP_CFGS; i++) {
		cfg->phy[i] = PHY_11BGN;
	}
	cfg->default_ap = 0;
	uint16_t pool_len = sizeof(ntp_pool);
	memcpy(cfg->ntp_pool, ntp_pool, pool_len);
	cfg->ntp_pool_len[0] = pool_len >> 8;
	cfg->ntp_pool_len[1] = pool_len;
	strcpy(cfg->server_url, "doragasu.com");
	memcpy(cfg->wifi_adv, wifi_adv, sizeof(wifi_adv));
	for (int i = 0; i < NUM_GAMERTAGS; i++) {
		uint8_t *tag = cfg->gamertag[i];
		tag[3] = i + 1; //id, big endian
		snprintf((char *)tag + 4, 32, "doragasu on Blastem!");
		snprintf((char *)tag + 36, 32, "My cool password");
		snprintf((char *)tag + 68, 32, "All your WiFi are belong to me!");
		//telegram token, avatar tiles and palette stay zeroed
	}
}

static void load_config(megawifi *mw)
{
	default_config(&mw->config);
	char *path = storage_path(mw, ".mwcfg");
	if (!path) {
		return;
	}
	FILE *f = fopen(path, "rb");
	free(path);
	if (!f) {
		return;
	}
	char magic[sizeof(CONFIG_MAGIC) - 1];
	mw_config cfg;
	if (fread(magic, 1, sizeof(magic), f) == sizeof(magic) && !memcmp(magic, CONFIG_MAGIC, sizeof(magic))
		&& fread(&cfg, 1, sizeof(cfg), f) == sizeof(cfg)
	) {
		mw->config = cfg;
	}
	fclose(f);
}

static uint8_t save_config(megawifi *mw)
{
	char *path = storage_path(mw, ".mwcfg");
	if (!path) {
		return 0;
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		warning("Failed to save MegaWiFi configuration to %s\n", path);
		free(path);
		return 0;
	}
	free(path);
	uint8_t ok = fwrite(CONFIG_MAGIC, 1, sizeof(CONFIG_MAGIC) - 1, f) == sizeof(CONFIG_MAGIC) - 1
		&& fwrite(&mw->config, 1, sizeof(mw->config), f) == sizeof(mw->config);
	return fclose(f) == 0 && ok;
}

static megawifi *get_megawifi(void *context)
{
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (!gen->extra) {
		socket_init();
		gen->extra = calloc(1, sizeof(megawifi));
		megawifi *mw = gen->extra;
		mw->module_state = STATE_IDLE;
		mw->flags = 0xE0; // cfg_ok, dt_ok, online
		for (int i = 0; i < 15; i++) {
			mw->sock_fds[i] = -1;
		}
		if (storage_prefix) {
			mw->storage_base = strdup(storage_prefix);
		} else if (gen->header.save_dir) {
			mw->storage_base = path_append(gen->header.save_dir, "megawifi");
		}
		load_config(mw);
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

static void mw_puts(megawifi *mw, const char *s)
{
	size_t len = strlen(s);
	mw_copy(mw, (uint8_t*)s, len);
}

static void udp_recv(megawifi *mw, uint8_t idx)
{
	ssize_t recvd;
	int s = mw->sock_fds[idx];
	struct sockaddr_in remote;
	socklen_t addr_len = sizeof(struct sockaddr_in);

	if (mw->remote_addr[idx].sin_addr.s_addr != htonl(INADDR_ANY)) {
		// Receive only from specified address
		recvd = recvfrom(s, (char*)mw->receive_buffer + 3, MAX_RECV_SIZE, 0,
				(struct sockaddr*)&remote, &addr_len);
		if (recvd > 0) {
			if (remote.sin_addr.s_addr != mw->remote_addr[idx].sin_addr.s_addr) {
				printf("Discarding UDP packet from unknown addr %s:%d\n",
						inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));
				recvd = 0;
			}
		}
	} else {
		// Reuse mode, data is preceded by remote IPv4 and port
		recvd = recvfrom(s, (char*)mw->receive_buffer + 9, MAX_RECV_SIZE - 6,
				0, (struct sockaddr*)&remote, &addr_len);
		if (recvd > 0) {
			mw->receive_buffer[3] = remote.sin_addr.s_addr;
			mw->receive_buffer[4] = remote.sin_addr.s_addr>>8;
			mw->receive_buffer[5] = remote.sin_addr.s_addr>>16;
			mw->receive_buffer[6] = remote.sin_addr.s_addr>>24;
			mw->receive_buffer[7] = remote.sin_port;
			mw->receive_buffer[8] = remote.sin_port>>8;
			recvd += 6;
		}
	}

	if (recvd > 0) {
		mw_putc(mw, STX);
		mw_putc(mw, (recvd >> 8) | ((idx+1) << 4));
		mw_putc(mw, recvd);
		mw->receive_bytes += recvd;
		mw_putc(mw, ETX);
		//should this set the channel flag?
	} else if (recvd < 0 && !socket_error_is_wouldblock()) {
		socket_close(mw->sock_fds[idx]);
		mw->channel_state[idx] = SOCKST_NONE;
		mw->channel_flags |= 1 << (idx + 1);
	}
}

static void udp_send(megawifi *mw, uint8_t idx)
{
	struct sockaddr_in remote;
	int s = mw->sock_fds[idx];
	int sent;
	char *data = (char*)mw->transmit_buffer;

	if (mw->remote_addr[idx].sin_addr.s_addr != htonl(INADDR_ANY)) {
		sent = sendto(s, data, mw->transmit_bytes, 0, (struct sockaddr*)&mw->remote_addr[idx],
				sizeof(struct sockaddr_in));
	} else {
		// Reuse mode, extract address from leading bytes
		// NOTE: mw->remote_addr[idx].sin_addr.s_addr == INADDR_ANY
		remote.sin_addr.s_addr = *((int32_t*)data);
		remote.sin_port = *((int16_t*)(data + 4));
		remote.sin_family = AF_INET;
		memset(remote.sin_zero, 0, sizeof(remote.sin_zero));
		sent = sendto(s, data + 6, mw->transmit_bytes - 6, 0, (struct sockaddr*)&remote,
				sizeof(struct sockaddr_in)) + 6;
	}
	if (sent < 0 && !socket_error_is_wouldblock()) {
		socket_close(s);
		mw->sock_fds[idx] = -1;
		mw->channel_state[idx] = SOCKST_NONE;
		mw->channel_flags |= 1 << (idx + 1);
	} else if (sent < mw->transmit_bytes) {
		//TODO: save this data somewhere so it can be sent in poll_socket
		printf("Sent %d bytes on channel %d, but %d were requested\n", sent, idx + 1, mw->transmit_bytes);
	}
}

static void poll_socket(megawifi *mw, uint8_t channel)
{
	if (mw->sock_fds[channel] < 0) {
		return;
	}
	if (mw->channel_state[channel] == SOCKST_TCP_LISTEN) {
		int res = accept(mw->sock_fds[channel], NULL, NULL);
		if (res >= 0) {
			socket_close(mw->sock_fds[channel]);
			socket_blocking(res, 0);
			mw->sock_fds[channel] = res;
			mw->channel_state[channel] = SOCKST_TCP_EST;
			mw->channel_flags |= 1 << (channel + 1);
		} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
			socket_close(mw->sock_fds[channel]);
			mw->channel_state[channel] = SOCKST_NONE;
			mw->channel_flags |= 1 << (channel + 1);
		}
	} else if (mw->channel_state[channel] == SOCKST_TCP_EST && mw->receive_bytes < (sizeof(mw->receive_buffer) - 4)) {
		size_t max = sizeof(mw->receive_buffer) - 4 - mw->receive_bytes;
		if (max > MAX_RECV_SIZE) {
			max = MAX_RECV_SIZE;
		}
		int bytes = recv(mw->sock_fds[channel], (char*)(mw->receive_buffer + mw->receive_bytes + 3), max, 0);
		if (bytes > 0) {
			mw_putc(mw, STX);
			mw_putc(mw, bytes >> 8 | (channel+1) << 4);
			mw_putc(mw, bytes);
			mw->receive_bytes += bytes;
			mw_putc(mw, ETX);
			//should this set the channel flag?
		} else if (bytes < 0 && !socket_error_is_wouldblock()) {
			socket_close(mw->sock_fds[channel]);
			mw->channel_state[channel] = SOCKST_NONE;
			mw->channel_flags |= 1 << (channel + 1);
		}
	} else if (mw->channel_state[channel] == SOCKST_UDP_READY && !mw->receive_bytes) {
		udp_recv(mw, channel);
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

static uint32_t read_be32(const uint8_t *src)
{
	return (uint32_t)src[0] << 24 | src[1] << 16 | src[2] << 8 | src[3];
}

static void reply_status(megawifi *mw, uint8_t ok)
{
	start_reply(mw, ok ? CMD_OK : CMD_ERROR);
	end_reply(mw);
}

static void cmd_ap_cfg(megawifi *mw, uint32_t size)
{
	uint8_t *data = mw->transmit_buffer + 4;
	uint8_t slot = data[0];
	uint8_t phy = data[1];
	if (size < 2 + SSID_MAXLEN + PASS_MAXLEN || slot >= NUM_AP_CFGS
		|| (phy != PHY_11B && phy != PHY_11BG && phy != PHY_11BGN)
	) {
		reply_status(mw, 0);
		return;
	}
	mw->config.phy[slot] = phy;
	memcpy(mw->config.ssid[slot], data + 2, SSID_MAXLEN);
	memcpy(mw->config.pass[slot], data + 2 + SSID_MAXLEN, PASS_MAXLEN);
	mw->config.default_ap = slot;
	reply_status(mw, 1);
}

static void cmd_ap_cfg_get(megawifi *mw)
{
	uint8_t slot = mw->transmit_buffer[4];
	if (slot >= NUM_AP_CFGS) {
		reply_status(mw, 0);
		return;
	}
	start_reply(mw, CMD_OK);
	mw_putc(mw, slot);
	mw_putc(mw, mw->config.phy[slot]);
	mw_copy(mw, (uint8_t*)mw->config.ssid[slot], SSID_MAXLEN);
	mw_copy(mw, (uint8_t*)mw->config.pass[slot], PASS_MAXLEN);
	end_reply(mw);
}

static void cmd_ap_scan(megawifi *mw)
{
	//There is a single network to be found: the host's own connection
	static const char ssid[] = "BlastEm";
	start_reply(mw, CMD_OK);
	mw_putc(mw, 1); //number of access points
	mw_putc(mw, 0); //auth mode: open
	mw_putc(mw, 1); //channel
	mw_putc(mw, (uint8_t)-40); //RSSI in dBm
	mw_putc(mw, sizeof(ssid) - 1);
	mw_puts(mw, ssid);
	end_reply(mw);
}

static void cmd_ip_cfg(megawifi *mw, uint32_t size)
{
	uint8_t slot = mw->transmit_buffer[4];
	if (size < 4 + IP_CFG_LEN || slot >= NUM_AP_CFGS) {
		reply_status(mw, 0);
		return;
	}
	memcpy(mw->config.ip_cfg[slot], mw->transmit_buffer + 8, IP_CFG_LEN);
	reply_status(mw, 1);
}

static void cmd_ip_cfg_get(megawifi *mw)
{
	uint8_t slot = mw->transmit_buffer[4];
	if (slot >= NUM_AP_CFGS) {
		reply_status(mw, 0);
		return;
	}
	start_reply(mw, CMD_OK);
	mw_putc(mw, slot);
	mw_set(mw, 0, 3);
	mw_copy(mw, mw->config.ip_cfg[slot], IP_CFG_LEN);
	end_reply(mw);
}

static void cmd_sntp_cfg(megawifi *mw, uint32_t size)
{
	//timezone, then at least one server, each one null terminated, then an empty string
	uint8_t *data = mw->transmit_buffer + 4;
	uint32_t tokens = 0, pos = 0;
	while (pos < size && data[pos])
	{
		uint8_t *end = memchr(data + pos, 0, size - pos);
		if (!end) {
			break;
		}
		if (!tokens && end - (data + pos) < 3) {
			//timezone is at least 3 characters long
			break;
		}
		tokens++;
		pos = end - data + 1;
	}
	if (tokens < 2 || pos >= size || data[pos] || pos + 1 != size || size > NTP_POOL_MAXLEN) {
		reply_status(mw, 0);
		return;
	}
	memcpy(mw->config.ntp_pool, data, size);
	mw->config.ntp_pool_len[0] = size >> 8;
	mw->config.ntp_pool_len[1] = size;
	reply_status(mw, 1);
}

static void cmd_sntp_cfg_get(megawifi *mw)
{
	uint16_t len = mw->config.ntp_pool_len[0] << 8 | mw->config.ntp_pool_len[1];
	if (len > NTP_POOL_MAXLEN) {
		len = NTP_POOL_MAXLEN;
	}
	start_reply(mw, CMD_OK);
	mw_copy(mw, mw->config.ntp_pool, len);
	end_reply(mw);
}

//The user partition of the module's flash lives in a file of its own, created
//erased on first use. Programming can only clear bits, like on the real chip.
static FILE *open_flash(megawifi *mw)
{
	char *path = storage_path(mw, ".mwflash");
	if (!path) {
		return NULL;
	}
	FILE *f = fopen(path, "r+b");
	if (!f) {
		f = fopen(path, "w+b");
		if (f) {
			uint8_t erased[FLASH_SECT_LEN];
			memset(erased, 0xFF, sizeof(erased));
			for (uint32_t i = 0; i < FLASH_SIZE / FLASH_SECT_LEN; i++)
			{
				if (fwrite(erased, 1, sizeof(erased), f) != sizeof(erased)) {
					fclose(f);
					f = NULL;
					break;
				}
			}
		}
		if (!f) {
			warning("Failed to create MegaWiFi flash file %s\n", path);
		}
	}
	free(path);
	return f;
}

static uint8_t flash_read(megawifi *mw, uint32_t addr, uint32_t len, uint8_t *dst)
{
	if (addr >= FLASH_SIZE || len > FLASH_SIZE - addr) {
		return 0;
	}
	FILE *f = open_flash(mw);
	if (!f) {
		return 0;
	}
	uint8_t ok = !fseek(f, addr, SEEK_SET) && fread(dst, 1, len, f) == len;
	fclose(f);
	return ok;
}

static uint8_t flash_write(megawifi *mw, uint32_t addr, uint32_t len, const uint8_t *src, uint8_t erase)
{
	if (addr >= FLASH_SIZE || len > FLASH_SIZE - addr) {
		return 0;
	}
	FILE *f = open_flash(mw);
	if (!f) {
		return 0;
	}
	uint8_t buf[FLASH_SECT_LEN];
	uint8_t ok = !fseek(f, addr, SEEK_SET) && fread(buf, 1, len, f) == len;
	if (ok) {
		for (uint32_t i = 0; i < len; i++)
		{
			buf[i] = erase ? 0xFF : buf[i] & src[i];
		}
		ok = !fseek(f, addr, SEEK_SET) && fwrite(buf, 1, len, f) == len;
	}
	return fclose(f) == 0 && ok;
}

static void cmd_flash_write(megawifi *mw, uint32_t size)
{
	uint8_t *data = mw->transmit_buffer + 4;
	if (size < 4) {
		reply_status(mw, 0);
		return;
	}
	uint32_t addr = read_be32(data);
	reply_status(mw, flash_write(mw, addr, size - 4, data + 4, 0));
}

static void cmd_flash_read(megawifi *mw)
{
	uint8_t *data = mw->transmit_buffer + 4;
	uint32_t addr = read_be32(data);
	uint16_t len = data[4] << 8 | data[5];
	uint8_t buf[MAX_RECV_SIZE];
	if (len > sizeof(buf) || !flash_read(mw, addr, len, buf)) {
		reply_status(mw, 0);
		return;
	}
	start_reply(mw, CMD_OK);
	mw_copy(mw, buf, len);
	end_reply(mw);
}

static void cmd_flash_erase(megawifi *mw)
{
	uint16_t sector = mw->transmit_buffer[4] << 8 | mw->transmit_buffer[5];
	reply_status(mw, flash_write(mw, sector * FLASH_SECT_LEN, FLASH_SECT_LEN, NULL, 1));
}

static void cmd_gamertag_set(megawifi *mw, uint32_t size)
{
	//slot, 3 reserved bytes, gamertag
	uint8_t slot = mw->transmit_buffer[4];
	if (size != 4 + GAMERTAG_LEN || slot >= NUM_GAMERTAGS) {
		reply_status(mw, 0);
		return;
	}
	memcpy(mw->config.gamertag[slot], mw->transmit_buffer + 8, GAMERTAG_LEN);
	reply_status(mw, 1);
}

static void cmd_server_url_set(megawifi *mw, uint32_t size)
{
	char *url = (char *)mw->transmit_buffer + 4;
	if (!size || size > SERVER_URL_MAXLEN || !memchr(url, 0, size)) {
		reply_status(mw, 0);
		return;
	}
	strcpy(mw->config.server_url, url);
	reply_status(mw, 1);
}

static void cmd_wifi_adv_set(megawifi *mw, uint32_t size)
{
	uint8_t *adv = mw->transmit_buffer + 4;
	//left continuous rx buf num, rx ba win, rx buf num, rx pkt num and tx buf num
	//have to be in range, and the block ack window needs AMPDU
	if (size < WIFI_ADV_LEN || adv[19] > 16 || adv[2] > 16
		|| adv[17] < 14 || adv[17] > 28 || adv[18] < 4 || adv[18] > 16
		|| adv[20] < 4 || adv[20] > 16 || (!adv[1] && adv[2])
	) {
		reply_status(mw, 0);
		return;
	}
	memcpy(mw->config.wifi_adv, adv, WIFI_ADV_LEN);
	reply_status(mw, 1);
}

static void cmd_tcp_con(megawifi *mw, uint32_t size)
{
	struct mw_addr_msg *addr = (struct mw_addr_msg*)(mw->transmit_buffer + 4);
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	int s;
	int err;

	uint8_t channel = addr->channel;
	if (!channel || channel > 15 || mw->sock_fds[channel - 1] >= 0) {
		start_reply(mw, CMD_ERROR);
		end_reply(mw);
		return;
	}
	channel--;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
#ifndef _WIN32
	hints.ai_flags = AI_NUMERICSERV;
#endif
	hints.ai_socktype = SOCK_STREAM;

	if ((err = getaddrinfo(addr->host, addr->dst_port, &hints, &res)) != 0) {
		printf("getaddrinfo failed: %s\n", gai_strerror(err));
		start_reply(mw, CMD_ERROR);
		end_reply(mw);
		return;
	}

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) {
		goto err;
	}

	// Should this be handled in a separate thread to avoid blocking emulation?
	if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
		goto err;
	}

	socket_blocking(s, 0);
	mw->sock_fds[channel] = s;
	mw->channel_state[channel] = SOCKST_TCP_EST;
	mw->channel_flags |= 1 << (channel + 1);
	printf("Connection established on ch %d with %s:%s\n", channel + 1,
			addr->host, addr->dst_port);

	if (res) {
		freeaddrinfo(res);
	}
	start_reply(mw, CMD_OK);
	end_reply(mw);
	return;

err:
	freeaddrinfo(res);
	printf("Connection to %s:%s failed, %s\n", addr->host, addr->dst_port, strerror(errno));
	start_reply(mw, CMD_ERROR);
	end_reply(mw);
}

static void cmd_close(megawifi *mw)
{
	int channel = mw->transmit_buffer[4] - 1;

	if (channel >= 15 || mw->sock_fds[channel] < 0) {
		start_reply(mw, CMD_ERROR);
		end_reply(mw);
		return;
	}

	socket_close(mw->sock_fds[channel]);
	mw->sock_fds[channel] = -1;
	mw->channel_state[channel] = SOCKST_NONE;
	mw->channel_flags |= 1 << (channel + 1);
	start_reply(mw, CMD_OK);
	end_reply(mw);
}

static void cmd_udp_set(megawifi *mw)
{
	struct mw_addr_msg *addr = (struct mw_addr_msg*)(mw->transmit_buffer + 4);
	unsigned int local_port, remote_port;
	int s;
	struct addrinfo *raddr;
	struct addrinfo hints;
	struct sockaddr_in local;
	int err;

	uint8_t channel = addr->channel;
	if (!channel || channel > 15 || mw->sock_fds[channel - 1] >= 0) {
		goto err;
	}
	channel--;
	local_port = atoi(addr->src_port);
	remote_port = atoi(addr->dst_port);

	if ((s = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
		printf("Datagram socket creation failed\n");
		goto err;
	}

	memset(local.sin_zero, 0, sizeof(local.sin_zero));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = htons(local_port);
	if (remote_port && addr->host[0]) {
		// Communication with remote peer
		printf("Set UDP ch %d, port %d to addr %s:%d\n", addr->channel,
				local_port, addr->host, remote_port);

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
#ifndef _WIN32
		hints.ai_flags = AI_NUMERICSERV;
#endif
		hints.ai_socktype = SOCK_DGRAM;

		if ((err = getaddrinfo(addr->host, addr->dst_port, &hints, &raddr)) != 0) {
			printf("getaddrinfo failed: %s\n", gai_strerror(err));
			goto err;
		}
		mw->remote_addr[channel] = *((struct sockaddr_in*)raddr->ai_addr);
		freeaddrinfo(raddr);
	} else if (local_port) {
		// Server in reuse mode
		printf("Set UDP ch %d, src port %d\n", addr->channel, local_port);
		mw->remote_addr[channel] = local;
	} else {
		printf("Invalid UDP socket data\n");
		goto err;
	}

	if (bind(s, (struct sockaddr*)&local, sizeof(struct sockaddr_in)) < 0) {
		printf("bind to port %d failed\n", local_port);
		goto err;
	}

	socket_blocking(s, 0);
	mw->sock_fds[channel] = s;
	mw->channel_state[channel] = SOCKST_UDP_READY;
	mw->channel_flags |= 1 << (channel + 1);

	start_reply(mw, CMD_OK);
	end_reply(mw);

	return;

err:
	start_reply(mw, CMD_ERROR);
	end_reply(mw);
}

static void cmd_gamertag_get(megawifi *mw)
{
	uint8_t slot = mw->transmit_buffer[4];
	if (slot >= NUM_GAMERTAGS) {
		reply_status(mw, 0);
		return;
	}
	start_reply(mw, CMD_OK);
	mw_copy(mw, mw->config.gamertag[slot], GAMERTAG_LEN);
	end_reply(mw);
}

static void cmd_hrng_get(megawifi *mw)
{
	uint16_t len = (mw->transmit_buffer[4]<<8) + mw->transmit_buffer[5];
	if (len > (MAX_RECV_SIZE - 4)) {
		start_reply(mw, CMD_ERROR);
		end_reply(mw);
		return;
	}
	// Pseudo-random, but who cares
	start_reply(mw, CMD_OK);
	srand(time(NULL));
	for (uint16_t i = 0; i < len; i++) {
		mw_putc(mw, rand());
	}
	end_reply(mw);
}

static void cmd_datetime(megawifi *mw)
{
	start_reply(mw, CMD_OK);
#ifdef _WIN32
	__time64_t t = _time64(NULL);
	int64_t t_be = htobe64(t);
	mw_copy(mw, (uint8_t*)&t_be, sizeof(int64_t));
	mw_puts(mw, _ctime64(&t));
#else
	time_t t = time(NULL);
	int64_t t_be = htobe64(t);
	mw_copy(mw, (uint8_t*)&t_be, sizeof(int64_t));
	mw_puts(mw, ctime(&t));
#endif

	mw_putc(mw, '\0');
	end_reply(mw);
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
		mw_putc(mw, '\0');
		end_reply(mw);
		break;
	case CMD_ECHO:
		mw->receive_bytes = mw->transmit_bytes;
		memcpy(mw->receive_buffer, mw->transmit_buffer, mw->transmit_bytes);
		break;
	case CMD_AP_SCAN:
		cmd_ap_scan(mw);
		break;
	case CMD_AP_CFG:
		cmd_ap_cfg(mw, size);
		break;
	case CMD_AP_CFG_GET:
		cmd_ap_cfg_get(mw);
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
	case CMD_IP_CFG:
		cmd_ip_cfg(mw, size);
		break;
	case CMD_IP_CFG_GET:
		cmd_ip_cfg_get(mw);
		break;
	case CMD_DEF_AP_CFG:
		if (mw->transmit_buffer[4] < NUM_AP_CFGS) {
			mw->config.default_ap = mw->transmit_buffer[4];
			reply_status(mw, 1);
		} else {
			reply_status(mw, 0);
		}
		break;
	case CMD_DEF_AP_CFG_GET:
		start_reply(mw, CMD_OK);
		mw_putc(mw, mw->config.default_ap);
		end_reply(mw);
		break;
	case CMD_AP_JOIN: {
		uint8_t slot = mw->transmit_buffer[4];
		if (slot >= NUM_AP_CFGS || !mw->config.ssid[slot][0]) {
			reply_status(mw, 0);
			break;
		}
		mw->module_state = STATE_READY;
		reply_status(mw, 1);
		break;
	}
	case CMD_AP_LEAVE:
		for (int i = 0; i < 15; i++)
		{
			if (mw->sock_fds[i] >= 0) {
				socket_close(mw->sock_fds[i]);
				mw->sock_fds[i] = -1;
				mw->channel_state[i] = SOCKST_NONE;
			}
		}
		mw->module_state = STATE_IDLE;
		reply_status(mw, 1);
		break;
	case CMD_TCP_CON:
		cmd_tcp_con(mw, size);
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
			socket_close(mw->sock_fds[channel]);
		}
		mw->sock_fds[channel] = socket(AF_INET, SOCK_STREAM, 0);
		if (mw->sock_fds[channel] < 0) {
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		int value = 1;
		setsockopt(mw->sock_fds[channel], SOL_SOCKET, SO_REUSEADDR, (char*)&value, sizeof(value));
		struct sockaddr_in bind_addr;
		memset(&bind_addr, 0, sizeof(bind_addr));
		bind_addr.sin_family = AF_INET;
		bind_addr.sin_port = htons(mw->transmit_buffer[8] << 8 | mw->transmit_buffer[9]);
		if (bind(mw->sock_fds[channel], (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
			socket_close(mw->sock_fds[channel]);
			mw->sock_fds[channel] = -1;
			start_reply(mw, CMD_ERROR);
			end_reply(mw);
			break;
		}
		int res = listen(mw->sock_fds[channel], 2);
		start_reply(mw, res ? CMD_ERROR : CMD_OK);
		if (res) {
			socket_close(mw->sock_fds[channel]);
			mw->sock_fds[channel] = -1;
		} else {
			mw->channel_flags |= 1 << (channel + 1);
			mw->channel_state[channel] = SOCKST_TCP_LISTEN;
			socket_blocking(mw->sock_fds[channel], 0);
		}
		end_reply(mw);
		break;
	}
	case CMD_CLOSE:
		cmd_close(mw);
		break;
	case CMD_UDP_SET:
		cmd_udp_set(mw);
		break;
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
	case CMD_SNTP_CFG:
		cmd_sntp_cfg(mw, size);
		break;
	case CMD_SNTP_CFG_GET:
		cmd_sntp_cfg_get(mw);
		break;
	case CMD_DATETIME:
		cmd_datetime(mw);
		break;
	case CMD_FLASH_WRITE:
		cmd_flash_write(mw, size);
		break;
	case CMD_FLASH_READ:
		cmd_flash_read(mw);
		break;
	case CMD_FLASH_ERASE:
		cmd_flash_erase(mw);
		break;
	case CMD_FLASH_ID:
		//Winbond W25Q32, the 4 MiB part on ESP-12 modules
		start_reply(mw, CMD_OK);
		mw_putc(mw, 0x40);
		mw_putc(mw, 0x16);
		mw_putc(mw, 0xEF);
		end_reply(mw);
		break;
	case CMD_DEF_CFG_SET: {
		uint8_t *data = mw->transmit_buffer + 4;
		if (size == 4 && read_be32(data) == DEF_CFG_MAGIC) {
			default_config(&mw->config);
			reply_status(mw, save_config(mw));
		} else {
			reply_status(mw, 0);
		}
		break;
	}
	case CMD_BSSID_GET: {
		//locally administered address, last byte tells station and AP interfaces apart
		static const uint8_t mac[] = {0x02, 0xB1, 0xA5, 0x7E, 0x40, 0x00};
		start_reply(mw, CMD_OK);
		mw_copy(mw, mac, sizeof(mac) - 1);
		mw_putc(mw, mac[5] | (mw->transmit_buffer[4] & 1));
		end_reply(mw);
		break;
	}
	case CMD_GAMERTAG_SET:
		cmd_gamertag_set(mw, size);
		break;
	case CMD_SYS_STAT:
		poll_all_sockets(mw);
		start_reply(mw, CMD_OK);
		mw_putc(mw, mw->module_state);
		mw_putc(mw, mw->flags);
		mw_putc(mw, mw->channel_flags >> 8);
		mw_putc(mw, mw->channel_flags);
		end_reply(mw);
		break;
	case CMD_GAMERTAG_GET:
		cmd_gamertag_get(mw);
		break;
	case CMD_LOG:
		start_reply(mw, CMD_OK);
		puts((char*)&mw->transmit_buffer[4]);
		end_reply(mw);
		break;
	case CMD_HRNG_GET:
		cmd_hrng_get(mw);
		break;
	case CMD_FACTORY_RESET:
		default_config(&mw->config);
		reply_status(mw, save_config(mw));
		break;
	case CMD_SERVER_URL_GET:
		start_reply(mw, CMD_OK);
		mw_puts(mw, mw->config.server_url);
		mw_putc(mw,'\0');
		end_reply(mw);
		break;
	case CMD_SERVER_URL_SET:
		cmd_server_url_set(mw, size);
		break;
	case CMD_WIFI_ADV_GET:
		start_reply(mw, CMD_OK);
		mw_copy(mw, mw->config.wifi_adv, WIFI_ADV_LEN);
		end_reply(mw);
		break;
	case CMD_WIFI_ADV_SET:
		cmd_wifi_adv_set(mw, size);
		break;
	case CMD_NV_CFG_SAVE:
		reply_status(mw, save_config(mw));
		break;
	case CMD_UPGRADE_LIST:
		//no firmware upgrades on offer
		reply_status(mw, 1);
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
		if (sock_fd >= 0 && channel_state == SOCKST_TCP_EST) {
			int sent = send(sock_fd, (char*)mw->transmit_buffer, mw->transmit_bytes, 0);
			if (sent < 0 && !socket_error_is_wouldblock()) {
				socket_close(sock_fd);
				mw->sock_fds[channel] = -1;
				mw->channel_state[channel] = SOCKST_NONE;
				mw->channel_flags |= 1 << mw->transmit_channel;
			} else if (sent < mw->transmit_bytes) {
				//TODO: save this data somewhere so it can be sent in poll_socket
				printf("Sent %d bytes on channel %d, but %d were requested\n", sent, mw->transmit_channel, mw->transmit_bytes);
			}
		} else if (sock_fd >= 0 && channel_state == SOCKST_UDP_READY) {
			udp_send(mw, channel);
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
