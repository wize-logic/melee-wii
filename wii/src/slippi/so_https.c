/* slippi_online/so_https.c
 * HTTPS GET (see so_https.h).
 */
#include <malloc.h>
#include <stdlib.h>

#include "so_https.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_tls.h"
#include "so_util.h"
#include "string.h"

#define REDIRECTS 5
#define TIMEOUT_MS 15000
#define HEAD_MAX 8192

typedef struct {
	SoTls *tls;
	u8 buf[4096];	/* received, not yet used */
	u32 have, used;
} Conn;

/* The next bytes of the stream; < 0 when it ended or failed */
static int fill(Conn *c)
{
	int n;
	if (c->used < c->have)
		return 0;
	n = so_tls_read(c->tls, c->buf, sizeof(c->buf), TIMEOUT_MS);
	if (n <= 0)
		return -1;
	c->have = (u32)n;
	c->used = 0;
	return 0;
}

static int read_exact(Conn *c, u8 *out, u32 len)
{
	while (len) {
		u32 take;
		if (fill(c) < 0)
			return -1;
		take = c->have - c->used < len ? c->have - c->used : len;
		memcpy(out, c->buf + c->used, take);
		c->used += take;
		out += take;
		len -= take;
	}
	return 0;
}

/* A line up to CRLF, without it */
static int read_line(Conn *c, char *out, int size)
{
	int n = 0;
	for (;;) {
		char ch;
		if (fill(c) < 0)
			return -1;
		ch = (char)c->buf[c->used++];
		if (ch == '\n') {
			if (n > 0 && out[n - 1] == '\r')
				n--;
			out[n] = '\0';
			return n;
		}
		if (n < size - 1)
			out[n++] = ch;
	}
}

static int lower_eq(const char *a, const char *b, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (x >= 'A' && x <= 'Z')
			x += 32;
		if (y >= 'A' && y <= 'Z')
			y += 32;
		if (x != y)
			return 0;
	}
	return 1;
}

static int split_url(const char *url, char *host, int host_size, char *path, int path_size)
{
	const char *p, *slash;
	int n;
	if (strncmp(url, "https://", 8) != 0)
		return -1;
	p = url + 8;
	slash = strchr(p, '/');
	n = slash ? (int)(slash - p) : (int)strlen(p);
	if (n <= 0 || n >= host_size)
		return -1;
	memcpy(host, p, n);
	host[n] = '\0';
	strlcpy(path, slash ? slash : "/", path_size);
	return (int)strlen(slash ? slash : "/") < path_size ? 0 : -1;
}

/* One request: 0 with the body, 1 with a redirect in `location`, < 0 */
static int request(SoTls *tls, const char *url, const char *accept, const char *user_agent, u32 max, u8 **body,
	u32 *len, char *location, int location_size, SoHttpsProgress progress, void *ctx, char *error, int error_size)
{
	static Conn conn;
	char host[128], path[1024], line[1024];
	char *req;
	u32 addr, content_length = 0, got = 0, cap;
	int status = 0, chunked = 0, have_length = 0, n;
	u8 *out;

	if (split_url(url, host, sizeof(host), path, sizeof(path)) < 0) {
		so_snprintf(error, error_size, "bad address: %.60s", url);
		return -1;
	}
	if (so_resolve(host, &addr) < 0) {
		so_snprintf(error, error_size, "could not find %s (DNS)", host);
		return -1;
	}
	if (so_tls_open(tls, host, addr, 443, 1, TIMEOUT_MS) < 0) {
		so_snprintf(error, error_size, "%s: %s", host, so_tls_error_text(tls));
		return -1;
	}
	req = (char *)malloc(2048);
	if (!req) {
		so_tls_close(tls);
		so_snprintf(error, error_size, "no memory");
		return -1;
	}
	n = so_snprintf(req, 2048,
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: %s\r\nAccept: %s\r\nAccept-Encoding: identity\r\n"
		"Connection: close\r\n\r\n",
		path, host, user_agent, accept);
	if (n >= 2047 || so_tls_write(tls, req, (u32)n) < 0) {
		free(req);
		so_tls_close(tls);
		so_snprintf(error, error_size, "%s: could not send the request", host);
		return -1;
	}
	free(req);

	conn.tls = tls;
	conn.have = conn.used = 0;
	if (read_line(&conn, line, sizeof(line)) < 0 || strncmp(line, "HTTP/1.", 7) != 0) {
		so_tls_close(tls);
		so_snprintf(error, error_size, "%s: no answer", host);
		return -1;
	}
	status = (int)so_parse_long(line + 9, NULL);
	location[0] = '\0';
	for (;;) {
		n = read_line(&conn, line, sizeof(line));
		if (n < 0) {
			so_tls_close(tls);
			so_snprintf(error, error_size, "%s: the answer was cut off", host);
			return -1;
		}
		if (n == 0)
			break;
		if (lower_eq(line, "content-length:", 15)) {
			content_length = (u32)so_parse_long(line + 15 + (line[15] == ' '), NULL);
			have_length = 1;
		} else if (lower_eq(line, "transfer-encoding:", 18) && strstr(line, "chunked")) {
			chunked = 1;
		} else if (lower_eq(line, "location:", 9)) {
			const char *v = line + 9;
			while (*v == ' ')
				v++;
			strlcpy(location, v, location_size);
		}
	}
	if (status >= 300 && status < 400 && location[0]) {
		so_tls_close(tls);
		return 1;
	}
	if (status != 200) {
		so_tls_close(tls);
		so_snprintf(error, error_size, "%s answered %d", host, status);
		return -1;
	}
	if (have_length && content_length > max) {
		so_tls_close(tls);
		so_snprintf(error, error_size, "%s: the file is too big (%u bytes)", host, (unsigned)content_length);
		return -1;
	}
	cap = have_length ? content_length : 64 * 1024;
	out = (u8 *)memalign(32, cap + 1);
	if (!out) {
		so_tls_close(tls);
		so_snprintf(error, error_size, "no memory for %u bytes", (unsigned)cap);
		return -1;
	}
	for (;;) {
		u32 part;
		if (chunked) {
			if (read_line(&conn, line, sizeof(line)) < 0)
				break;
			part = (u32)strtoul(line, NULL, 16);
			if (part == 0) {
				have_length = 1;	/* the last chunk: done */
				content_length = got;
				break;
			}
		} else if (have_length) {
			if (got >= content_length)
				break;
			part = content_length - got < 16384 ? content_length - got : 16384;
		} else {
			if (fill(&conn) < 0) {
				have_length = 1;	/* closed: the end of the body */
				content_length = got;
				break;
			}
			part = conn.have - conn.used;
		}
		if (got + part > max) {
			free(out);
			so_tls_close(tls);
			so_snprintf(error, error_size, "%s: the file is too big", host);
			return -1;
		}
		if (got + part > cap) {
			u32 bigger = cap * 2 > got + part ? cap * 2 : got + part;
			u8 *more = (u8 *)memalign(32, bigger + 1);
			if (!more) {
				free(out);
				so_tls_close(tls);
				so_snprintf(error, error_size, "no memory");
				return -1;
			}
			memcpy(more, out, got);
			free(out);
			out = more;
			cap = bigger;
		}
		if (read_exact(&conn, out + got, part) < 0)
			break;
		got += part;
		if (chunked)
			read_line(&conn, line, sizeof(line));	/* the CRLF after the chunk */
		if (progress)
			progress(got, have_length && !chunked ? content_length : 0, ctx);
	}
	so_tls_close(tls);
	if (!have_length || got != content_length) {
		free(out);
		so_snprintf(error, error_size, "%s: the download was cut off", host);
		return -1;
	}
	out[got] = 0;
	*body = out;
	*len = got;
	return 0;
}

int so_https_get(const char *url, const char *accept, const char *user_agent, u32 max, u8 **body, u32 *len,
	SoHttpsProgress progress, void *ctx, char *error, int error_size)
{
	char *where = (char *)malloc(1024), *next = (char *)malloc(1024);
	SoTls *tls = (SoTls *)memalign(32, sizeof(SoTls));
	int i, res = -1;
	*body = NULL;
	*len = 0;
	if (!where || !next || !tls) {
		so_snprintf(error, error_size, "no memory");
		goto done;
	}
	strlcpy(where, url, 1024);
	for (i = 0; i <= REDIRECTS; i++) {
		res = request(tls, where, accept, user_agent, max, body, len, next, 1024, progress, ctx, error, error_size);
		if (res != 1)
			goto done;
		strlcpy(where, next, 1024);
	}
	so_snprintf(error, error_size, "too many redirects");
	res = -1;
done:
	free(where);
	free(next);
	free(tls);
	return res < 0 ? -1 : 0;
}
