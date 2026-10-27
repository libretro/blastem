#ifndef NET_H_
#define NET_H_
#include <stdint.h>

#ifdef IS_LIB
//RetroArch's libretro-common has its own socket_init and
//socket_close. A static link (the emscripten build) has
//no version script to keep ours private, so the two
//would collide.
#define socket_init blastem_socket_init
#define socket_close blastem_socket_close
#endif

typedef struct {
	uint8_t ip[4];
	uint8_t net_mask[4];
} iface_info;

uint8_t get_host_address(iface_info *out);
//Initializes the socket library on platforms that need it
void socket_init(void);
//Sets a sockt to blocking or non-blocking mode
int socket_blocking(int sock, int should_block);
//Close a socket
void socket_close(int sock);
//Return the last error on a socket operation
int socket_last_error(void);
//Returns if the last socket error was EAGAIN/EWOULDBLOCK
int socket_error_is_wouldblock(void);

#endif //NET_H_
