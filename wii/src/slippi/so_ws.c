/* slippi_online/so_ws.c
 * The WebSocket client (see so_ws.h).
 */
#include "so_ws.h"
#include "so_entropy.h"
#include "so_platform.h"
#include "so_util.h"
#include "bearssl.h"
#include "string.h"

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

enum { OP_CONT = 0x0, OP_TEXT = 0x1, OP_BINARY = 0x2, OP_CLOSE = 0x8, OP_PING = 0x9, OP_PONG = 0xA };

void so_base64(const u8 *in, u32 len, char *out)
{
	static const char abc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	u32 i, o = 0;
	for (i = 0; i + 2 < len; i += 3) {
		u32 v = ((u32)in[i] << 16) | ((u32)in[i + 1] << 8) | in[i + 2];
		out[o++] = abc[v >> 18];
		out[o++] = abc[(v >> 12) & 63];
		out[o++] = abc[(v >> 6) & 63];
		out[o++] = abc[v & 63];
	}
	if (i < len) {
		u32 v = (u32)in[i] << 16;
		if (i + 1 < len)
			v |= (u32)in[i + 1] << 8;
		out[o++] = abc[v >> 18];
		out[o++] = abc[(v >> 12) & 63];
		out[o++] = i + 1 < len ? abc[(v >> 6) & 63] : '=';
		out[o++] = '=';
	}
	out[o] = '\0';
}

/* Case-insensitive search for a header's value in the response head */
static const char *header(const char *head, const char *name, char *out, int size)
{
	const char *p = head;
	int n = (int)strlen(name);
	while ((p = strchr(p, '\n')) != NULL) {
		int i;
		p++;
		for (i = 0; i < n; i++) {
			char a = p[i], b = name[i];
			if (a >= 'A' && a <= 'Z')
				a += 32;
			if (b >= 'A' && b <= 'Z')
				b += 32;
			if (a != b)
				break;
		}
		if (i == n && p[n] == ':') {
			const char *v = p + n + 1;
			int len = 0;
			while (*v == ' ' || *v == '\t')
				v++;
			while (v[len] && v[len] != '\r' && v[len] != '\n' && len < size - 1) {
				out[len] = v[len];
				len++;
			}
			out[len] = '\0';
			return out;
		}
	}
	return NULL;
}

int so_ws_open(SoWs *ws, SoTls *tls, const char *host, const char *path, u32 timeout_ms)
{
	u8 key_raw[16], digest[20];
	char key[25], expect[29], got[64], request[384];
	char *head = (char *)ws->rx;
	u32 deadline = so_time_ms() + timeout_ms;
	int len;
	br_sha1_context sha;

	ws->tls = tls;
	ws->rx_len = 0;
	ws->msg_len = 0;
	ws->msg_open = 0;
	ws->closed = 0;
	so_entropy_bytes(key_raw, sizeof(key_raw));
	so_base64(key_raw, sizeof(key_raw), key);
	len = so_snprintf(request, sizeof(request),
		"GET %s HTTP/1.1\r\nHost: %s\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
		"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n",
		path, host, key);
	if (len >= (int)sizeof(request) - 1 || so_tls_write(tls, request, (u32)len) < 0)
		return -1;

	/* the answer's head, up to the empty line */
	for (;;) {
		char *end;
		int n;
		s32 left = (s32)(deadline - so_time_ms());
		if (left <= 0 || ws->rx_len >= sizeof(ws->rx) - 1)
			return -2;
		n = so_tls_read(tls, ws->rx + ws->rx_len, sizeof(ws->rx) - 1 - ws->rx_len, (u32)left);
		if (n < 0)
			return -3;
		ws->rx_len += (u32)n;
		ws->rx[ws->rx_len] = '\0';
		end = strstr(head, "\r\n\r\n");
		if (end != NULL) {
			u32 used = (u32)(end + 4 - head);
			if (strncmp(head, "HTTP/1.1 101", 12) != 0) {
				so_log("ws: no upgrade: %.40s", head);
				return -4;
			}
			br_sha1_init(&sha);
			br_sha1_update(&sha, key, strlen(key));
			br_sha1_update(&sha, WS_GUID, sizeof(WS_GUID) - 1);
			br_sha1_out(&sha, digest);
			so_base64(digest, sizeof(digest), expect);
			if (header(head, "Sec-WebSocket-Accept", got, sizeof(got)) == NULL || strcmp(got, expect) != 0) {
				so_log("ws: wrong Sec-WebSocket-Accept");
				return -5;
			}
			/* frames that came with the head stay */
			memmove(ws->rx, ws->rx + used, ws->rx_len - used);
			ws->rx_len -= used;
			return 0;
		}
	}
}

static int send_frame(SoWs *ws, int opcode, const void *payload, u32 len)
{
	u8 head[14], mask[4];
	u8 chunk[256];
	const u8 *p = (const u8 *)payload;
	u32 n = 0, i, done;
	if (ws->closed)
		return -1;
	head[n++] = (u8)(0x80 | opcode);
	if (len < 126) {
		head[n++] = (u8)(0x80 | len);
	} else if (len < 65536) {
		head[n++] = 0x80 | 126;
		head[n++] = (u8)(len >> 8);
		head[n++] = (u8)len;
	} else {
		head[n++] = 0x80 | 127;
		head[n++] = 0;
		head[n++] = 0;
		head[n++] = 0;
		head[n++] = 0;
		head[n++] = (u8)(len >> 24);
		head[n++] = (u8)(len >> 16);
		head[n++] = (u8)(len >> 8);
		head[n++] = (u8)len;
	}
	so_entropy_bytes(mask, 4);
	memcpy(head + n, mask, 4);
	n += 4;
	/* the head and the masked payload in pieces (one TLS record each
	 * write; the engine coalesces nothing across them) */
	if (len + n <= sizeof(chunk)) {
		memcpy(chunk, head, n);
		for (i = 0; i < len; i++)
			chunk[n + i] = p[i] ^ mask[i & 3];
		return so_tls_write(ws->tls, chunk, n + len);
	}
	if (so_tls_write(ws->tls, head, n) < 0)
		return -1;
	for (done = 0; done < len;) {
		u32 take = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
		for (i = 0; i < take; i++)
			chunk[i] = p[done + i] ^ mask[(done + i) & 3];
		if (so_tls_write(ws->tls, chunk, take) < 0)
			return -1;
		done += take;
	}
	return 0;
}

int so_ws_send_text(SoWs *ws, const char *text, u32 len)
{
	return send_frame(ws, OP_TEXT, text, len);
}

void so_ws_send_close(SoWs *ws)
{
	static const u8 normal[2] = { 0x03, 0xE8 };	/* 1000 */
	send_frame(ws, OP_CLOSE, normal, 2);
	ws->closed = 1;
}

/* Parses one whole frame at the front of rx: its result (above), or
 * SO_WS_NOTHING when rx holds no whole frame yet. */
static int take_frame(SoWs *ws)
{
	u8 *b = ws->rx;
	u32 hlen = 2, plen, i;
	int fin, op, masked;
	u8 mask[4] = { 0, 0, 0, 0 };
	int result = SO_WS_FRAME;
	if (ws->rx_len < 2)
		return SO_WS_NOTHING;
	fin = b[0] & 0x80;
	op = b[0] & 0x0F;
	masked = b[1] & 0x80;
	plen = b[1] & 0x7F;
	if (plen == 126) {
		if (ws->rx_len < 4)
			return SO_WS_NOTHING;
		plen = ((u32)b[2] << 8) | b[3];
		hlen = 4;
	} else if (plen == 127) {
		if (ws->rx_len < 10)
			return SO_WS_NOTHING;
		if (b[2] | b[3] | b[4] | b[5])
			return SO_WS_CLOSED;	/* over 4 GB */
		plen = ((u32)b[6] << 24) | ((u32)b[7] << 16) | ((u32)b[8] << 8) | b[9];
		hlen = 10;
	}
	if (masked) {
		if (ws->rx_len < hlen + 4)
			return SO_WS_NOTHING;
		memcpy(mask, b + hlen, 4);
		hlen += 4;
	}
	if (plen > SO_WS_MESSAGE_MAX)
		return SO_WS_CLOSED;	/* bigger than any message of the service */
	if (ws->rx_len < hlen + plen)
		return SO_WS_NOTHING;
	if (masked)
		for (i = 0; i < plen; i++)
			b[hlen + i] ^= mask[i & 3];

	switch (op) {
	case OP_TEXT:
	case OP_BINARY:
	case OP_CONT:
		if (op != OP_CONT) {
			ws->msg_len = 0;
			ws->msg_open = op == OP_TEXT;
		}
		if (ws->msg_open) {
			if (ws->msg_len + plen > SO_WS_MESSAGE_MAX)
				return SO_WS_CLOSED;
			memcpy(ws->msg + ws->msg_len, b + hlen, plen);
			ws->msg_len += plen;
			if (fin) {
				ws->msg[ws->msg_len] = '\0';
				ws->msg_open = 0;
				result = SO_WS_MESSAGE;
			}
		}
		break;
	case OP_PING:
		send_frame(ws, OP_PONG, b + hlen, plen);
		break;
	case OP_CLOSE:
		if (!ws->closed)
			so_ws_send_close(ws);
		result = SO_WS_CLOSED;
		break;
	default:
		break;	/* pong, or something new: only life */
	}
	memmove(ws->rx, ws->rx + hlen + plen, ws->rx_len - hlen - plen);
	ws->rx_len -= hlen + plen;
	return result;
}

int so_ws_poll(SoWs *ws, u32 timeout_ms)
{
	u32 deadline = so_time_ms() + timeout_ms;
	for (;;) {
		int res = take_frame(ws), n;
		s32 left;
		if (res != SO_WS_NOTHING) {
			if (res == SO_WS_CLOSED)
				ws->closed = 1;
			return res;
		}
		if (ws->rx_len >= sizeof(ws->rx))
			return SO_WS_CLOSED;
		left = (s32)(deadline - so_time_ms());
		if (left < 0)
			left = 0;
		n = so_tls_read(ws->tls, ws->rx + ws->rx_len, sizeof(ws->rx) - ws->rx_len, (u32)left);
		if (n < 0) {
			ws->closed = 1;
			return SO_WS_CLOSED;
		}
		if (n == 0)
			return SO_WS_NOTHING;
		ws->rx_len += (u32)n;
	}
}
