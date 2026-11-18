/* slippi_online/so_tcp_ogc.c
 * so_tcp.h on libogc's IOS sockets.
 */
#include <gccore.h>
#include <network.h>

#include "so_entropy.h"
#include "so_platform.h"
#include "so_tcp.h"
#include "string.h"

#define POLL_SLEEP_US 5000
/* IOS's poll flags (Dolphin's WII_Socket.h: RDNORM, ERR, HUP) */
#define IOS_POLLIN 0x0001
#define IOS_POLLERR 0x0020
#define IOS_POLLHUP 0x0040

s32 so_tcp_connect(u32 addr, u16 port)
{
	struct sockaddr_in to;
	s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP), res;
	u64 start = so_timebase();
	int one = 1;
	if (s < 0)
		return s;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_len = 8;	/* (IOS's length of an IPv4 address: so_socket_ogc.c) */
	to.sin_port = htons(port);
	to.sin_addr.s_addr = addr;
	res = net_connect(s, (struct sockaddr *)&to, 8);
	so_entropy_add_u32((u32)(so_timebase() - start));
	if (res < 0) {
		net_close(s);
		return res;
	}
	net_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return s;
}

s32 so_tcp_send(s32 sock, const void *data, u32 len)
{
	const u8 *p = (const u8 *)data;
	u32 sent = 0;
	while (sent < len) {
		u64 start = so_timebase();
		s32 res = net_send(sock, p + sent, len - sent, 0);
		so_entropy_add_u32((u32)(so_timebase() - start));
		if (res <= 0)
			return res < 0 ? res : -1;
		sent += (u32)res;
	}
	return (s32)len;
}

s32 so_tcp_recv(s32 sock, void *buf, u32 size, u32 timeout_ms)
{
	u32 start = so_time_ms();
	for (;;) {
		struct pollsd pd;
		s32 res;
		pd.socket = sock;
		pd.events = IOS_POLLIN;
		pd.revents = 0;
		res = net_poll(&pd, 1, 0);
		if (res < 0)
			return res;
		if (res > 0 && (pd.revents & (IOS_POLLIN | IOS_POLLHUP | IOS_POLLERR))) {
			res = net_recv(sock, buf, size, 0);
			so_entropy_add_u32((u32)so_timebase());
			return res > 0 ? res : -1;	/* 0: the other side closed */
		}
		if (so_time_ms() - start >= timeout_ms)
			return 0;
		so_sleep_us(POLL_SLEEP_US);
	}
}

void so_tcp_close(s32 sock)
{
	if (sock >= 0)
		net_close(sock);
}
