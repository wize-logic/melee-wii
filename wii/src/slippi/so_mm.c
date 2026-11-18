/* slippi_online/so_mm.c
 * Matchmaking client state machine (see so_mm.h). STUN as
 * SlippiMatchmaking::startMatchmaking does it: each server up to 3 times,
 * 500 ms each, until two have answered; the first to answer gets the
 * keepalives.
 */
#define SO_FILE_ID "mm"
#include "so_mm.h"
#include "so_alloc.h"
#include "so_entropy.h"
#include "so_mmws.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

#define STUN_TRIES 3
#define STUN_WAIT_MS 500
#define STUN_KEEPALIVE_MS 15000
#define JOIN_MAX 2048

typedef struct {
	SoMmState state;
	u16 port;
	char error[128];
	char latest_version[24];
	SoMatchResult result;
	/* the search */
	SoConfig config;
	SoUser user;
	SoSearch search;
	SoMmServers servers;
	char lan[32];
	/* STUN */
	int stun_server;	/* the one being asked */
	int stun_try;
	u32 stun_sent_ms;
	u8 txid[12];
	SoStunResult stun;
	int keepalive_server;	/* -1: none resolved */
	u32 keepalive_ms;
} Mm;

/* On the online heap (the kernel's data region is nearly full), from the
 * first search on. */
static Mm *mmp;
#define mm (*mmp)

static void enter(SoMmState state)
{
	mm.state = state;
}

static void fail(const char *text)
{
	strlcpy(mm.error, text, sizeof(mm.error));
	so_log("mm: %s", text);
	so_mmws_cancel();
	enter(SO_MM_ERROR);
}

static void stun_send(int server)
{
	u8 req[SO_STUN_REQUEST_LEN];
	so_entropy_bytes(mm.txid, sizeof(mm.txid));
	so_stun_request(req, mm.txid);
	so_socket_send(mm.servers.stun_addr[server], mm.servers.stun_port[server], req, sizeof(req));
	mm.stun_sent_ms = so_time_ms();
}

/* Reads what has arrived on the socket: STUN answers count, anything else
 * (an early netplay packet) is dropped, as while Dolphin's ENet host waits */
static void drain(void)
{
	u8 buf[SO_DATAGRAM_MAX];
	u32 addr;
	u16 port;
	int n;
	while ((n = so_socket_receive(&addr, &port, buf, sizeof(buf), NULL)) > 0) {
		u32 mapped;
		u16 mapped_port;
		if (mm.state != SO_MM_STUN || so_stun_parse(buf, (u32)n, mm.txid, &mapped, &mapped_port) < 0)
			continue;
		{
			int k = mm.stun.answered[0] ? 1 : 0;
			char ip[16];
			mm.stun.answered[k] = 1;
			mm.stun.addr[k] = mapped;
			mm.stun.port[k] = mapped_port;
			so_format_ipv4(mapped, ip, sizeof(ip));
			so_log("mm: STUN server %d sees us at %s:%u", mm.stun_server, ip, (unsigned)mapped_port);
			if (k == 0)
				mm.keepalive_server = mm.stun_server;
			/* the next server, for the second answer */
			mm.stun_server++;
			mm.stun_try = 0;
			mm.stun_sent_ms = 0;
			memset(mm.txid, 0, sizeof(mm.txid));
		}
	}
}

static void start_session(void)
{
	static char join[JOIN_MAX];
	int len;
	if (mm.stun.answered[0] == 0)
		so_log("mm: no STUN server answered; the service will use the address it sees");
	else
		so_log("mm: STUN: %d of 2 servers answered", mm.stun.answered[1] ? 2 : 1);
	len = so_mm_build_join(join, sizeof(join), &mm.config, &mm.user, &mm.search, mm.port, mm.lan, &mm.stun);
	if (len < 0) {
		fail("Failed to build mm ticket");
		return;
	}
	if (so_mmws_start(mm.config.mm_host, mm.servers.mm_addr, mm.config.mm_port, mm.config.mm_path,
			mm.config.mm_tls, join) < 0) {
		memset(join, 0, sizeof(join));	/* it holds the play key */
		fail("Failed to create mm client");
		return;
	}
	memset(join, 0, sizeof(join));
	memset(&mm.user, 0, sizeof(mm.user));
	mm.keepalive_ms = so_time_ms();
	enter(SO_MM_CONNECTING);
}

int so_mm_start(const SoConfig *config, const SoUser *user, const SoSearch *search, const SoMmServers *servers,
	u16 port)
{
	u32 local;
	char ip[16];
	if (!mmp) {
		mmp = (Mm *)so_malloc(sizeof(Mm));
		if (!mmp)
			return -1;
		memset(mmp, 0, sizeof(Mm));
	}
	so_mm_stop();
	memset(&mm.result, 0, sizeof(mm.result));
	memset(&mm.stun, 0, sizeof(mm.stun));
	mm.error[0] = '\0';
	mm.latest_version[0] = '\0';
	mm.port = port;
	mm.config = *config;
	mm.user = *user;
	mm.search = *search;
	mm.servers = *servers;
	mm.stun_server = 0;
	mm.stun_try = 0;
	mm.stun_sent_ms = 0;
	mm.keepalive_server = servers->stun_count > 0 ? 0 : -1;
	so_entropy_init();

	/* The LAN address lets an opponent behind the same external IP reach
	 * us directly. */
	local = config->lan_ip ? config->lan_ip : so_local_ip();
	so_format_ipv4(local, ip, sizeof(ip));
	if (local)
		so_snprintf(mm.lan, sizeof(mm.lan), "%s:%u", ip, (unsigned)port);
	else
		mm.lan[0] = '\0';
	so_log("mm: searching (mode %d) from port %u, LAN address %s", search->mode, (unsigned)port,
		local ? mm.lan : "unknown");
	SO_PHASE("mm stun");
	enter(SO_MM_STUN);
	return 0;
}

static void poll_stun(void)
{
	drain();
	if (mm.state != SO_MM_STUN)
		return;
	if (mm.stun.answered[1] || mm.stun_server >= mm.servers.stun_count) {
		start_session();
		return;
	}
	if (mm.stun_sent_ms != 0 && so_time_ms() - mm.stun_sent_ms < STUN_WAIT_MS)
		return;
	if (mm.stun_try >= STUN_TRIES) {
		/* this one is silent: the next */
		mm.stun_server++;
		mm.stun_try = 0;
		mm.stun_sent_ms = 0;
		if (mm.stun_server >= mm.servers.stun_count) {
			start_session();
			return;
		}
	}
	stun_send(mm.stun_server);
	mm.stun_try++;
}

static void poll_session(void)
{
	SoMmwsState st = so_mmws_state();
	drain();
	if ((st == SO_MMWS_CONNECTING || st == SO_MMWS_QUEUED) && mm.keepalive_server >= 0 &&
		so_time_ms() - mm.keepalive_ms >= STUN_KEEPALIVE_MS) {
		u8 req[SO_STUN_REQUEST_LEN], txid[12];
		so_entropy_bytes(txid, sizeof(txid));
		so_stun_request(req, txid);
		so_socket_send(mm.servers.stun_addr[mm.keepalive_server], mm.servers.stun_port[mm.keepalive_server],
			req, sizeof(req));
		mm.keepalive_ms = so_time_ms();
	}
	switch (st) {
	case SO_MMWS_QUEUED:
		if (mm.state != SO_MM_SEARCHING) {
			SO_PHASE("mm in queue");
			enter(SO_MM_SEARCHING);
		}
		break;
	case SO_MMWS_MATCHED: {
		static char text[8192 + 1];
		const SoMmPlayer *local;
		const SoMatchResult *r = &mm.result;
		int len = so_mmws_take_result(text, sizeof(text));
		if (len < 0) {
			fail("Match result was missing");
			break;
		}
		if (so_mm_parse_match(text, (u32)len, &mm.result, mm.error, sizeof(mm.error)) < 0) {
			fail(mm.error);
			break;
		}
		local = &r->players[r->local_player_index];
		so_log("mm: matched (%s): %d players, we are port %d%s", r->match_id, r->player_count, local->port,
			r->is_host ? ", decider" : "");
		SO_PHASE("mm matched");
		/* the replies to the last keepalive must not reach netplay */
		so_socket_flush();
		enter(SO_MM_MATCHED);
		break;
	}
	case SO_MMWS_FAILED: {
		char text[128];
		so_mmws_error(text, sizeof(text), mm.latest_version, sizeof(mm.latest_version));
		fail(text);
		break;
	}
	case SO_MMWS_IDLE:
		fail("Matchmaking was cancelled");
		break;
	default:
		break;
	}
}

void so_mm_poll(void)
{
	if (!mmp)
		return;
	switch (mm.state) {
	case SO_MM_STUN:
		poll_stun();
		break;
	case SO_MM_CONNECTING:
	case SO_MM_SEARCHING:
		poll_session();
		break;
	default:
		break;
	}
}

void so_mm_stop(void)
{
	if (!mmp)
		return;
	if (mm.state == SO_MM_CONNECTING || mm.state == SO_MM_SEARCHING)
		so_mmws_cancel();
	enter(SO_MM_IDLE);
}

SoMmState so_mm_state(void)
{
	return mmp ? mm.state : SO_MM_IDLE;
}

const char *so_mm_error(void)
{
	return mmp ? mm.error : "Failed to create mm client";
}

const SoMatchResult *so_mm_result(void)
{
	return mmp ? &mm.result : NULL;
}

const char *so_mm_latest_version(void)
{
	return mmp ? mm.latest_version : "";
}

u16 so_mm_port(void)
{
	return mmp ? mm.port : 0;
}
