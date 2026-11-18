/* slippi_online/so_ws.h
 * A WebSocket client (RFC 6455) over so_tls: the upgrade, masked frames
 * out, text messages in (continuations joined), pings answered. For the
 * matchmaking service, whose messages are JSON in text frames.
 */
#ifndef SO_WS_H
#define SO_WS_H

#include "global.h"
#include "so_tls.h"

#define SO_WS_MESSAGE_MAX 8192	/* a match result with four players' chat: ~3 KB */

typedef struct {
	SoTls *tls;
	u8 rx[SO_WS_MESSAGE_MAX + 16];	/* frame bytes not yet parsed */
	u32 rx_len;
	char msg[SO_WS_MESSAGE_MAX + 1];	/* the message being put together */
	u32 msg_len;
	int msg_open;	/* a fragmented message is in progress */
	int closed;
} SoWs;

/* Sends the upgrade for ws(s)://host/path over an open stream and checks
 * the answer. Returns 0, or < 0. */
int so_ws_open(SoWs *ws, SoTls *tls, const char *host, const char *path, u32 timeout_ms);
/* Sends one text message. Returns 0 or < 0. */
int so_ws_send_text(SoWs *ws, const char *text, u32 len);
/* Sends a Close frame (code 1000). */
void so_ws_send_close(SoWs *ws);

enum {
	SO_WS_NOTHING = 0,	/* nothing came within the timeout */
	SO_WS_MESSAGE = 1,	/* a whole text message: ws->msg, ws->msg_len */
	SO_WS_FRAME = 2,	/* another frame (ping, pong, binary): only life */
	SO_WS_CLOSED = -1,	/* a Close frame, the stream ended or failed */
};
/* Waits up to `timeout_ms` for the next frame. */
int so_ws_poll(SoWs *ws, u32 timeout_ms);

/* base64 of `len` bytes into `out` (NUL-terminated; room 4*ceil(len/3)+1) */
void so_base64(const u8 *in, u32 len, char *out);

#endif
