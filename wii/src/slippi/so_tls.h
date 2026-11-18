/* slippi_online/so_tls.h
 * A TLS 1.2 client stream (BearSSL, libs/bearssl) over so_tcp, or plain
 * TCP for test servers. For one thread at a time, which it may block: the
 * matchmaking session's thread (so_mmws.c) and the launcher's updates.
 *
 * Certificates are checked against so_tls_ta.c's roots and the host name.
 * Their dates against the Wii's clock, or, when that is unset or older,
 * against this build's date: a Wii whose clock was never set still
 * connects, and a certificate that expired before the build is refused.
 *
 * Suites: ECDHE with ECDSA or RSA certificates, AES-GCM or ChaCha20-Poly1305
 * (what matchmaking.slippi.gg, GitHub and Let's Encrypt's hosts offer).
 */
#ifndef SO_TLS_H
#define SO_TLS_H

#include "global.h"
#include "bearssl.h"

#define SO_TLS_OBUF 1024	/* the most we send in one record */

typedef struct {
	br_ssl_client_context sc;
	br_x509_minimal_context xc;
	s32 sock;
	int tls;	/* 0: plain TCP */
	int failed;
	int last_error;	/* BearSSL's error code, or the socket's (< 0) */
	u8 ibuf[BR_SSL_BUFSIZE_INPUT];
	u8 obuf[SO_TLS_OBUF + 85];
} SoTls;

extern const br_x509_trust_anchor so_tls_anchors[];
extern const unsigned so_tls_anchor_count;

/* Connects to addr:port and, with `tls`, completes the handshake for
 * `host`. Returns 0, or < 0 (t->last_error says why). */
int so_tls_open(SoTls *t, const char *host, u32 addr, u16 port, int tls, u32 timeout_ms);
/* Sends everything; returns 0 or < 0. */
int so_tls_write(SoTls *t, const void *data, u32 len);
/* Returns the bytes read (> 0), 0 when none came within `timeout_ms`, or
 * < 0 when the stream closed or failed. */
int so_tls_read(SoTls *t, void *buf, u32 size, u32 timeout_ms);
/* Sends TLS's close and closes the socket. */
void so_tls_close(SoTls *t);

/* A short text for t->last_error (logs). */
const char *so_tls_error_text(const SoTls *t);

#endif
