/* slippi_online/so_mmws.h
 * The matchmaking service session (Slippi Dolphin 3.7.0's Rust
 * MatchmakingClient, slippi-rust-extensions matchmaking/src/client.rs): a
 * WebSocket to wss://matchmaking.slippi.gg/v1/queue on a thread of its own,
 * which may block. so_mm.c starts it with the join message, polls its state
 * from the network thread, and takes the match result.
 *
 * The conversation: join -> joined (a ticket id and token) -> status ...
 * -> match | error. A ping after 5 s without a frame, the connection lost
 * after 15 s; lost with a ticket, it resumes (resume + ticket + token) for
 * up to 15 s; a fresh connection is tried 3 times, 1 s apart.
 */
#ifndef SO_MMWS_H
#define SO_MMWS_H

#include "global.h"

typedef enum {
	SO_MMWS_IDLE = 0,
	SO_MMWS_CONNECTING = 1,
	SO_MMWS_QUEUED = 2,
	SO_MMWS_MATCHED = 3,
	SO_MMWS_FAILED = 4,
} SoMmwsState;

/* Starts a session (ending any other): `addr` is the service's resolved
 * IPv4 address, `join` the join message. Returns 0, or < 0 when the thread
 * or its memory could not be had. */
int so_mmws_start(const char *host, u32 addr, u16 port, const char *path, int tls, const char *join);
/* Leaves the queue (never waits). */
void so_mmws_cancel(void);

SoMmwsState so_mmws_state(void);
/* SO_MMWS_MATCHED: copies the match message (once); returns its length or
 * < 0. */
int so_mmws_take_result(char *out, int size);
/* SO_MMWS_FAILED: the text for the player. */
void so_mmws_error(char *out, int size);

#endif
