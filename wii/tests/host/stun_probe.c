/* Host test: so_stun_request/so_stun_parse against the public STUN servers */
#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "so_entropy.h"
#include "so_mm.h"
#include "so_socket.h"
#include "so_util.h"

int main(void)
{
	static const char *hosts[] = { "stun.l.google.com", "stun1.l.google.com", "stun.cloudflare.com", "global.stun.twilio.com" };
	static const u16 ports[] = { 19302, 19302, 3478, 3478 };
	struct sockaddr_in me = { 0 };
	int s = socket(AF_INET, SOCK_DGRAM, 0), i, ok = 0;
	me.sin_family = AF_INET;
	me.sin_port = htons(41234);
	bind(s, (struct sockaddr *)&me, sizeof(me));
	so_entropy_init();
	for (i = 0; i < 4; i++) {
		u8 req[SO_STUN_REQUEST_LEN], txid[12], buf[512];
		u32 addr, mapped;
		u16 mport;
		struct sockaddr_in to = { 0 };
		struct pollfd pd = { s, POLLIN, 0 };
		char ip[16];
		if (so_resolve(hosts[i], &addr) < 0) { printf("%s: no DNS\n", hosts[i]); continue; }
		so_entropy_bytes(txid, 12);
		so_stun_request(req, txid);
		to.sin_family = AF_INET; to.sin_port = htons(ports[i]); to.sin_addr.s_addr = addr;
		sendto(s, req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to));
		if (poll(&pd, 1, 1500) <= 0) { printf("%s: no answer\n", hosts[i]); continue; }
		{
			int n = (int)recv(s, buf, sizeof(buf), 0);
			if (so_stun_parse(buf, (u32)n, txid, &mapped, &mport) == 0) {
				so_format_ipv4(mapped, ip, sizeof(ip));
				printf("%s: %s:%u\n", hosts[i], ip, (unsigned)mport);
				ok++;
			} else printf("%s: unparsable (%d bytes)\n", hosts[i], n);
		}
	}
	return ok ? 0 : 1;
}
