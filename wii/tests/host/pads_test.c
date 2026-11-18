/* Host test: so_pads.c's 9-byte pads and 3.7.0's drop-report helpers */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "so_pads.h"

static void pad_for(int frame, unsigned char *p) { for (int i = 0; i < SO_PAD_SIZE; i++) p[i] = (unsigned char)(frame * 16 + i); }

int main(void)
{
	SoRemotePads q; unsigned char pkt[20 * SO_PAD_SIZE], got[SO_PAD_SIZE], want[SO_PAD_SIZE], rep[64 * SO_PAD_SIZE];
	s32 newest; int n, f;
	so_remote_pads_reset(&q);
	/* a pad packet for frames 1..5, newest first */
	for (f = 5; f >= 1; f--) pad_for(f, pkt + (5 - f) * SO_PAD_SIZE);
	assert(so_remote_pads_add(&q, 5, pkt, 5 * SO_PAD_SIZE) == 5);
	for (f = 1; f <= 5; f++) { assert(so_remote_pads_get(&q, f, got) == 0); pad_for(f, want); assert(!memcmp(got, want, SO_PAD_SIZE)); }
	/* a report naming frames up to 9: 6..9 added from it */
	for (f = 9; f >= 2; f--) pad_for(f, rep + (9 - f) * SO_PAD_SIZE);
	assert(so_remote_pads_add_report(&q, 9, 8, rep) == 4);
	for (f = 6; f <= 9; f++) { assert(so_remote_pads_get(&q, f, got) == 0); pad_for(f, want); assert(!memcmp(got, want, SO_PAD_SIZE)); }
	/* an older report adds nothing; one too far ahead is refused */
	assert(so_remote_pads_add_report(&q, 8, 1, rep) == 0);
	assert(so_remote_pads_add_report(&q, 9 + 129, 1, rep) == -1);
	/* the report we would send: newest first */
	n = so_remote_pads_report(&q, rep, 64, &newest);
	assert(n == 9 && newest == 9);
	pad_for(9, want); assert(!memcmp(rep, want, SO_PAD_SIZE));
	pad_for(1, want); assert(!memcmp(rep + 8 * SO_PAD_SIZE, want, SO_PAD_SIZE));
	/* settled: the last input repeats */
	so_remote_pads_fill(&q, 12);
	pad_for(9, want);
	for (f = 10; f <= 12; f++) { assert(so_remote_pads_get(&q, f, got) == 0); assert(!memcmp(got, want, SO_PAD_SIZE)); }
	/* an empty queue fills from a zero pad at frame 0 */
	so_remote_pads_reset(&q);
	so_remote_pads_fill(&q, 3);
	memset(want, 0, sizeof(want));
	for (f = 0; f <= 3; f++) { assert(so_remote_pads_get(&q, f, got) == 0); assert(!memcmp(got, want, SO_PAD_SIZE)); }
	/* a report missing frames repeats the head */
	so_remote_pads_reset(&q);
	pad_for(1, pkt); assert(so_remote_pads_add(&q, 1, pkt, SO_PAD_SIZE) == 1);
	pad_for(4, rep);
	assert(so_remote_pads_add_report(&q, 4, 1, rep) == 3);
	pad_for(1, want); assert(so_remote_pads_get(&q, 2, got) == 0 && !memcmp(got, want, SO_PAD_SIZE));
	pad_for(4, want); assert(so_remote_pads_get(&q, 4, got) == 0 && !memcmp(got, want, SO_PAD_SIZE));
	printf("pads: all checks passed\n");
	return 0;
}
