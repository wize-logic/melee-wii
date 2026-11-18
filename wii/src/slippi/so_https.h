/* slippi_online/so_https.h
 * HTTPS GET over so_tls, for the launcher's updates (GitHub's API and its
 * release downloads): redirects followed, chunked or sized bodies, the body
 * in memory. Blocks the calling thread.
 */
#ifndef SO_HTTPS_H
#define SO_HTTPS_H

#include "global.h"

/* Called as the body comes in (total 0 when the server gave no length) */
typedef void (*SoHttpsProgress)(u32 done, u32 total, void *ctx);

/* GETs `url` (https://host/path) with `accept` as its Accept header.
 * Returns 0 with *body (malloc'd, NUL after its *len bytes; free it) or
 * < 0 with `error` set. A body over `max` bytes is refused. */
int so_https_get(const char *url, const char *accept, const char *user_agent, u32 max, u8 **body, u32 *len,
	SoHttpsProgress progress, void *ctx, char *error, int error_size);

#endif
