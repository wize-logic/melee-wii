/* so_tcp.h on POSIX sockets, for the host tests */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "so_tcp.h"

s32 so_tcp_connect(u32 addr, u16 port)
{
	struct sockaddr_in to;
	int one = 1, s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(port);
	to.sin_addr.s_addr = addr;
	if (connect(s, (struct sockaddr *)&to, sizeof(to)) < 0) {
		close(s);
		return -2;
	}
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return s;
}
s32 so_tcp_send(s32 sock, const void *data, u32 len)
{
	u32 sent = 0;
	while (sent < len) {
		ssize_t r = send(sock, (const char *)data + sent, len - sent, MSG_NOSIGNAL);
		if (r <= 0)
			return -1;
		sent += (u32)r;
	}
	return (s32)len;
}
s32 so_tcp_recv(s32 sock, void *buf, u32 size, u32 timeout_ms)
{
	struct pollfd pd = { sock, POLLIN, 0 };
	int r = poll(&pd, 1, (int)timeout_ms);
	ssize_t n;
	if (r < 0)
		return -1;
	if (r == 0)
		return 0;
	n = recv(sock, buf, size, 0);
	return n > 0 ? (s32)n : -1;
}
void so_tcp_close(s32 sock)
{
	if (sock >= 0)
		close(sock);
}
