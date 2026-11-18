/* slippi_online/so_entropy.h
 * Unpredictable bytes, for what an opponent or a server must not guess: the
 * TLS client's seed, STUN transaction ids, WebSocket keys and masks, and
 * the random bits Slippi 3.7.0 puts in each online pad (they decide some
 * moves' outcomes, so an opponent who could predict them could steer them).
 *
 * The Wii has no random source the PowerPC can read, so the pool is fed
 * timings the CPU cannot repeat: the timebase (40.5 MHz) at network
 * arrivals, IOS call durations, controller polls and frames, and the
 * console's own identity at the start. They go into a SHA-256 pool;
 * so_entropy_bytes draws from an HMAC-DRBG (BearSSL's) reseeded from it.
 * so_random() is a plain xorshift whose state a few outputs give away: it
 * is never used for these.
 */
#ifndef SO_ENTROPY_H
#define SO_ENTROPY_H

#include "global.h"

/* Seeds the pool (idempotent; any thread). */
void so_entropy_init(void);

/* Mixes in a sample (any thread, interrupt handlers included: it only
 * writes to a ring, losing a sample at worst). */
void so_entropy_add_u32(u32 sample);
void so_entropy_add(const void *data, u32 len);

/* Fills `out` (any thread but an interrupt handler). */
void so_entropy_bytes(void *out, u32 len);
u8 so_entropy_byte(void);

#endif
