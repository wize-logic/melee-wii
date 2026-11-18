/* slippi_online/so_entropy.c
 * The entropy pool and its generator (see so_entropy.h).
 */
#include "so_entropy.h"
#include "so_platform.h"
#include "bearssl.h"
#include "string.h"

#define RING 256	/* samples waiting for the pool */
#define RESEED_BYTES 4096	/* bytes drawn between reseeds */

static volatile u32 s_ring[RING];
static volatile u32 s_ring_head;
static u32 s_ring_tail;

static br_hmac_drbg_context s_drbg;
static SoMutex s_lock;
static volatile int s_ready;
static u32 s_drawn;
static u8 s_spare[32];	/* so_entropy_byte's buffer */
static u32 s_spare_left;

void so_entropy_add_u32(u32 sample)
{
	u32 h = s_ring_head;
	/* the timebase's low bits move fastest: keep them with the sample */
	s_ring[h % RING] = sample ^ ((u32)so_timebase() << 7);
	s_ring_head = h + 1;
}

void so_entropy_add(const void *data, u32 len)
{
	const u8 *p = (const u8 *)data;
	while (len >= 4) {
		so_entropy_add_u32(((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]);
		p += 4;
		len -= 4;
	}
	if (len) {
		u32 v = 0;
		while (len--)
			v = (v << 8) | *p++;
		so_entropy_add_u32(v);
	}
}

/* Folds the waiting samples into the generator (the lock is held). */
static void reseed(void)
{
	u32 buf[RING + 2];
	u32 n = 0, head = s_ring_head;
	u64 tb;
	while (s_ring_tail != head && n < RING) {
		buf[n++] = s_ring[s_ring_tail % RING];
		s_ring_tail++;
	}
	tb = so_timebase();
	buf[n++] = (u32)(tb >> 32);
	buf[n++] = (u32)tb;
	br_hmac_drbg_update(&s_drbg, buf, n * 4);
	s_drawn = 0;
}

void so_entropy_init(void)
{
	u8 seed[64];
	u32 i;
	if (s_ready)
		return;
	if (so_mutex_init(&s_lock) < 0)
		return;
	/* the console's identity and the time, then the timing of a few
	 * thousand timebase reads (cache and bus jitter) */
	so_platform_entropy_seed(seed, sizeof(seed));
	br_hmac_drbg_init(&s_drbg, &br_sha256_vtable, seed, sizeof(seed));
	for (i = 0; i < 64; i++) {
		u64 a = so_timebase(), b;
		volatile u32 spin = 0;
		while (spin < 50 + (i & 7))
			spin++;
		b = so_timebase();
		so_entropy_add_u32((u32)(b - a) ^ (u32)a);
	}
	reseed();
	memset(seed, 0, sizeof(seed));
	s_ready = 1;
}

void so_entropy_bytes(void *out, u32 len)
{
	if (!s_ready)
		so_entropy_init();
	so_mutex_lock(&s_lock);
	if (s_drawn >= RESEED_BYTES || s_ring_head - s_ring_tail >= RING / 4)
		reseed();
	{
		/* each draw also takes the time it is made */
		u64 tb = so_timebase();
		br_hmac_drbg_update(&s_drbg, &tb, sizeof(tb));
	}
	br_hmac_drbg_generate(&s_drbg, out, len);
	s_drawn += len;
	so_mutex_unlock(&s_lock);
}

u8 so_entropy_byte(void)
{
	u8 b;
	if (!s_ready)
		so_entropy_init();
	so_mutex_lock(&s_lock);
	if (s_spare_left == 0) {
		u64 tb = so_timebase();
		br_hmac_drbg_update(&s_drbg, &tb, sizeof(tb));
		br_hmac_drbg_generate(&s_drbg, s_spare, sizeof(s_spare));
		s_drawn += sizeof(s_spare);
		s_spare_left = sizeof(s_spare);
	}
	b = s_spare[--s_spare_left];
	s_spare[s_spare_left] = 0;
	so_mutex_unlock(&s_lock);
	return b;
}
