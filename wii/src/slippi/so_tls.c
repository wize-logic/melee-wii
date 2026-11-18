/* slippi_online/so_tls.c
 * The TLS client (see so_tls.h).
 */
#include "so_tls.h"
#include "so_entropy.h"
#include "so_platform.h"
#include "so_tcp.h"
#include "so_util.h"
#include "string.h"

/* 2026-11-18 00:00 UTC: no certificate in use may have expired before this
 * build (a Wii's clock is often unset) */
#define SO_TLS_BUILD_TIME 1794960000u
#define DAYS_TO_1970 719528u	/* BearSSL counts days from 0000-01-01 */

static void client_init(SoTls *t)
{
	static const u16 suites[] = {
		BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
		BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
		BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
		BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
		BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
		BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	};
	br_ssl_client_context *cc = &t->sc;
	br_ssl_engine_context *eng = &cc->eng;
	br_x509_minimal_context *xc = &t->xc;
	u32 now;

	br_ssl_client_zero(cc);
	br_ssl_engine_set_versions(eng, BR_TLS12, BR_TLS12);
	br_x509_minimal_init(xc, &br_sha256_vtable, so_tls_anchors, so_tls_anchor_count);
	br_ssl_engine_set_suites(eng, suites, sizeof(suites) / sizeof(suites[0]));
	br_ssl_engine_set_default_rsavrfy(eng);
	br_ssl_engine_set_default_ecdsa(eng);
	br_x509_minimal_set_rsa(xc, br_ssl_engine_get_rsavrfy(eng));
	br_x509_minimal_set_ecdsa(xc, br_ssl_engine_get_ec(eng), br_ssl_engine_get_ecdsa(eng));
	br_ssl_engine_set_hash(eng, br_sha1_ID, &br_sha1_vtable);
	br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
	br_ssl_engine_set_hash(eng, br_sha384_ID, &br_sha384_vtable);
	br_ssl_engine_set_hash(eng, br_sha512_ID, &br_sha512_vtable);
	br_x509_minimal_set_hash(xc, br_sha1_ID, &br_sha1_vtable);
	br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
	br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
	br_x509_minimal_set_hash(xc, br_sha512_ID, &br_sha512_vtable);
	br_ssl_engine_set_x509(eng, &xc->vtable);
	br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
	br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
	br_ssl_engine_set_default_aes_gcm(eng);
	br_ssl_engine_set_default_chapol(eng);
	br_ssl_engine_set_buffers_bidi(eng, t->ibuf, sizeof(t->ibuf), t->obuf, sizeof(t->obuf));

	now = so_unix_time();
	if (now < SO_TLS_BUILD_TIME)
		now = SO_TLS_BUILD_TIME;
	br_x509_minimal_set_time(xc, DAYS_TO_1970 + now / 86400, now % 86400);
}

static int fail(SoTls *t, int error)
{
	t->failed = 1;
	t->last_error = error;
	return -1;
}

/* Runs the engine until it can do `want` (BR_SSL_SENDAPP or RECVAPP):
 * 0 when it can, 1 when `deadline` passed first, < 0 when it closed. */
static int pump(SoTls *t, unsigned want, u32 deadline)
{
	br_ssl_engine_context *eng = &t->sc.eng;
	for (;;) {
		unsigned st = br_ssl_engine_current_state(eng);
		unsigned char *buf;
		size_t len;
		if (st & BR_SSL_CLOSED) {
			int err = br_ssl_engine_last_error(eng);
			return fail(t, err ? err : -1);
		}
		if (st & BR_SSL_SENDREC) {
			buf = br_ssl_engine_sendrec_buf(eng, &len);
			if (so_tcp_send(t->sock, buf, (u32)len) < 0)
				return fail(t, -2);
			br_ssl_engine_sendrec_ack(eng, len);
			continue;
		}
		if (st & want)
			return 0;
		if (st & BR_SSL_RECVREC) {
			s32 left = (s32)(deadline - so_time_ms()), n;
			if (left < 0)
				left = 0;	/* (still one look at what has arrived) */
			buf = br_ssl_engine_recvrec_buf(eng, &len);
			n = so_tcp_recv(t->sock, buf, (u32)len, (u32)left);
			if (n < 0)
				return fail(t, -3);
			if (n == 0)
				return 1;
			br_ssl_engine_recvrec_ack(eng, (size_t)n);
			continue;
		}
		/* waiting for the other side with nothing to read: give it time */
		if ((s32)(deadline - so_time_ms()) <= 0)
			return 1;
		so_sleep_us(1000);
	}
}

int so_tls_open(SoTls *t, const char *host, u32 addr, u16 port, int tls, u32 timeout_ms)
{
	u8 seed[32];
	u32 deadline = so_time_ms() + timeout_ms;
	int res;
	t->sock = -1;
	t->tls = tls;
	t->failed = 0;
	t->last_error = 0;
	if (tls)
		client_init(t);
	t->sock = so_tcp_connect(addr, port);
	if (t->sock < 0)
		return fail(t, t->sock);
	if (!tls)
		return 0;
	so_entropy_bytes(seed, sizeof(seed));
	br_ssl_engine_inject_entropy(&t->sc.eng, seed, sizeof(seed));
	memset(seed, 0, sizeof(seed));
	if (!br_ssl_client_reset(&t->sc, host, 0)) {
		res = fail(t, br_ssl_engine_last_error(&t->sc.eng));
		so_tcp_close(t->sock);
		t->sock = -1;
		return res;
	}
	res = pump(t, BR_SSL_SENDAPP, deadline);
	if (res != 0) {
		if (res > 0)
			fail(t, -4);	/* timed out */
		so_tcp_close(t->sock);
		t->sock = -1;
		return -1;
	}
	return 0;
}

int so_tls_write(SoTls *t, const void *data, u32 len)
{
	br_ssl_engine_context *eng = &t->sc.eng;
	const u8 *p = (const u8 *)data;
	if (t->sock < 0 || t->failed)
		return -1;
	if (!t->tls)
		return so_tcp_send(t->sock, data, len) < 0 ? fail(t, -2) : 0;
	while (len) {
		unsigned char *buf;
		size_t room;
		if (pump(t, BR_SSL_SENDAPP, so_time_ms() + 10000) != 0)
			return t->failed ? -1 : fail(t, -4);
		buf = br_ssl_engine_sendapp_buf(eng, &room);
		if (room > len)
			room = len;
		memcpy(buf, p, room);
		br_ssl_engine_sendapp_ack(eng, room);
		p += room;
		len -= (u32)room;
	}
	br_ssl_engine_flush(eng, 0);
	/* push the records out now (pump returns once they are sent and the
	 * engine could take more) */
	return pump(t, BR_SSL_SENDAPP, so_time_ms() + 10000) < 0 ? -1 : 0;
}

int so_tls_read(SoTls *t, void *buf, u32 size, u32 timeout_ms)
{
	br_ssl_engine_context *eng = &t->sc.eng;
	unsigned char *app;
	size_t avail;
	int res;
	if (t->sock < 0 || t->failed)
		return -1;
	if (!t->tls) {
		s32 n = so_tcp_recv(t->sock, buf, size, timeout_ms);
		return n < 0 ? fail(t, -3) : n;
	}
	res = pump(t, BR_SSL_RECVAPP, so_time_ms() + timeout_ms);
	if (res < 0)
		return -1;
	if (res > 0)
		return 0;
	app = br_ssl_engine_recvapp_buf(eng, &avail);
	if (avail > size)
		avail = size;
	memcpy(buf, app, avail);
	br_ssl_engine_recvapp_ack(eng, avail);
	return (int)avail;
}

void so_tls_close(SoTls *t)
{
	if (t->sock < 0)
		return;
	if (t->tls && !t->failed) {
		br_ssl_engine_close(&t->sc.eng);
		/* send the close_notify; the other side's answer is not awaited */
		{
			unsigned st = br_ssl_engine_current_state(&t->sc.eng);
			if (st & BR_SSL_SENDREC) {
				size_t len;
				unsigned char *buf = br_ssl_engine_sendrec_buf(&t->sc.eng, &len);
				so_tcp_send(t->sock, buf, (u32)len);
				br_ssl_engine_sendrec_ack(&t->sc.eng, len);
			}
		}
	}
	so_tcp_close(t->sock);
	t->sock = -1;
}

const char *so_tls_error_text(const SoTls *t)
{
	static char text[32];
	switch (t->last_error) {
	case 0: return "no error";
	case -1: return "closed";
	case -2: return "send failed";
	case -3: return "connection lost";
	case -4: return "timed out";
	case BR_ERR_X509_EXPIRED: return "certificate expired (check the Wii's clock)";
	case BR_ERR_X509_NOT_TRUSTED: return "certificate not trusted";
	case BR_ERR_X509_BAD_SERVER_NAME: return "certificate for another host";
	case BR_ERR_BAD_VERSION: return "server refused TLS 1.2";
	case BR_ERR_BAD_CIPHER_SUITE: return "no cipher suite in common";
	default:
		if (t->last_error < 0)
			so_snprintf(text, sizeof(text), "socket error %d", t->last_error);
		else
			so_snprintf(text, sizeof(text), "TLS error %d", t->last_error);
		return text;
	}
}
