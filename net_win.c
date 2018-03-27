#include "net.h"

uint8_t get_host_address(iface_info *out)
{
	out->ip[0] = 127;
	out->ip[1] = 0;
	out->ip[2] = 0;
	out->ip[3] = 1;
	out->net_mask[0] = 255;
	out->net_mask[0] = 255;
	out->net_mask[0] = 255;
	out->net_mask[0] = 0;
	return 1;
}
