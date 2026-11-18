/* Host test: the whole session thread (so_mmws.c) with a made-up account:
 * connects, joins, gets the service's error */
#include <stdio.h>
#include <string.h>
#include "so_config.h"
#include "so_entropy.h"
#include "so_mm.h"
#include "so_mmws.h"
#include "so_platform.h"
#include "so_socket.h"

int main(void)
{
	SoConfig config;
	SoUser user;
	SoSearch search;
	SoStunResult stun;
	char join[2048], err[128], latest[24];
	u32 addr, t0;
	int len;
	SoMmwsState st = SO_MMWS_IDLE, last = -1;
	so_config_defaults(&config);
	memset(&user, 0, sizeof(user));
	strcpy(user.uid, "test"); strcpy(user.play_key, "test"); strcpy(user.connect_code, "TEST#000"); strcpy(user.display_name, "test");
	memset(&search, 0, sizeof(search));
	search.mode = 1;
	memset(&stun, 0, sizeof(stun));
	stun.answered[0] = 1; stun.addr[0] = 0x01020304; stun.port[0] = 41000;
	len = so_mm_build_join(join, sizeof(join), &config, &user, &search, 41000, "192.168.1.5:41000", &stun);
	printf("join (%d): %s\n", len, join);
	so_entropy_init();
	so_resolve(config.mm_host, &addr);
	so_mmws_start(config.mm_host, addr, config.mm_port, config.mm_path, config.mm_tls, join);
	t0 = so_time_ms();
	while (so_time_ms() - t0 < 30000) {
		st = so_mmws_state();
		if (st != last) { printf("state %d at %u ms\n", st, so_time_ms() - t0); last = st; }
		if (st == SO_MMWS_FAILED || st == SO_MMWS_MATCHED || (st == SO_MMWS_IDLE && so_time_ms() - t0 > 1000)) break;
		so_sleep_us(50000);
	}
	so_mmws_error(err, sizeof(err), latest, sizeof(latest));
	printf("error: %s, latest: %s\n", err, latest);
	return st == SO_MMWS_FAILED ? 0 : 1;
}
