/* Host test: TLS + WebSocket to the matchmaking service, a join with a
 * made-up account, and whatever the service answers (an error is
 * expected). Usage: ws_probe [host [path]] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "so_entropy.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_tls.h"
#include "so_ws.h"

int main(int argc, char **argv)
{
	const char *host = argc > 1 ? argv[1] : "matchmaking.slippi.gg";
	const char *path = argc > 2 ? argv[2] : "/v1/queue";
	static SoTls tls;
	static SoWs ws;
	u32 addr, t0;
	int res, i;
	const char *join = "{\"type\":\"join\",\"user\":{\"uid\":\"test\",\"playKey\":\"test\","
		"\"connectCode\":\"TEST#000\",\"displayName\":\"test\"},\"search\":{\"mode\":1,\"connectCode\":[]},"
		"\"appVersion\":\"3.7.0\",\"port\":41000,\"ipAddressLan\":\"\",\"natType\":\"unknown\"}";
	so_entropy_init();
	if (so_resolve(host, &addr) < 0) {
		printf("resolve failed\n");
		return 1;
	}
	t0 = so_time_ms();
	res = so_tls_open(&tls, host, addr, 443, 1, 10000);
	printf("tls open: %d (%s) in %u ms, suite %04x\n", res, so_tls_error_text(&tls), so_time_ms() - t0,
		tls.sc.eng.session.cipher_suite);
	if (res < 0)
		return 1;
	res = so_ws_open(&ws, &tls, host, path, 10000);
	printf("ws open: %d\n", res);
	if (res < 0)
		return 1;
	so_ws_send_text(&ws, join, (u32)strlen(join));
	for (i = 0; i < 20; i++) {
		res = so_ws_poll(&ws, 1000);
		if (res == SO_WS_MESSAGE)
			printf("message: %s\n", ws.msg);
		else if (res == SO_WS_FRAME)
			printf("frame\n");
		else if (res == SO_WS_CLOSED) {
			printf("closed\n");
			break;
		}
	}
	so_tls_close(&tls);
	return 0;
}
