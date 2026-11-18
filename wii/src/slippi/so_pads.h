/* slippi_online/so_pads.h
 * The bookkeeping behind Slippi's pad exchange, without any networking, so it
 * can be unit-tested on a PC. Each piece mirrors a part of Slippi Dolphin's
 * SlippiNetplayClient (Ishiiruka v3.7.0):
 *
 *   SoLocalPads   localPadQueue: our pads not yet acked by every peer, sent
 *                 newest first in each pad packet.
 *   SoRemotePads  remotePadQueue: one peer's pads by frame. The newest frame
 *                 is the "head"; a packet adds the frames after it.
 *   SoAckTimers   ackTimers: send times of pad packets, matched to acks for
 *                 the ping.
 *   SoTimeSync    frameOffsetData: how far ahead of a peer we run, from the
 *                 arrival times of its pad packets.
 *
 * Frames are Melee's online frame numbers (the first game frame is 1).
 */
#ifndef SO_PADS_H
#define SO_PADS_H

#include "global.h"

/* Bytes of one pad on the wire (3.7.0's SLIPPI_PAD_WIRE_SIZE): the 8 input
 * bytes, then the random bits the game mixes into the RNG seed */
#define SO_PAD_SIZE 9
#define SO_PAD_INPUT_SIZE 8
#define SO_PADS_MAX_PER_PACKET 128	/* Dolphin rejects packets with more */
#define SO_FRAME_US 16683	/* one frame at 59.94 Hz */

/* ---- local pads ---------------------------------------------------------- */

#define SO_LOCAL_PAD_RING 256	/* > 128 un-acked frames + slack */

typedef struct {
	s32 frame;
	s32 checksum_frame;
	u32 checksum;
	u8 pad[SO_PAD_SIZE];
} SoLocalPad;

typedef struct {
	SoLocalPad ring[SO_LOCAL_PAD_RING];
	u32 head;	/* index after the newest pad */
	u32 count;
} SoLocalPads;

void so_local_pads_reset(SoLocalPads *q);
/* Queues the pad for `frame` (frames must increase). */
void so_local_pads_push(SoLocalPads *q, const SoLocalPad *pad);
/* Drops pads older than `min_acked_frame`, which is first raised to
 * newest - 128 so a peer that stops acking cannot grow the queue. */
void so_local_pads_drop_acked(SoLocalPads *q, s32 min_acked_frame);
const SoLocalPad *so_local_pads_newest(const SoLocalPads *q);
/* The i-th newest pad (0 = newest). */
const SoLocalPad *so_local_pads_at(const SoLocalPads *q, u32 i);

/* Writes a 0x80 pad packet (message id included) with every queued pad,
 * newest first; returns its length, 0 when the queue is empty, or -1 when
 * `size` is too small. */
int so_local_pads_encode(const SoLocalPads *q, u8 player_idx, u8 *out, int size);

/* ---- remote pads --------------------------------------------------------- */

#define SO_REMOTE_PAD_RING 256

typedef struct {
	u8 pads[SO_REMOTE_PAD_RING][SO_PAD_SIZE];
	s32 head_frame;	/* newest queued frame; 0 when empty */
	u32 count;	/* frames held, at most the ring size */
} SoRemotePads;

void so_remote_pads_reset(SoRemotePads *q);

/* Adds the pads of one pad packet: `newest_frame` and `pads` as received
 * (newest first, `pad_bytes` bytes). Returns the number of new frames
 * (Dolphin's inputsToCopy; 0 for an old packet, which must not be acked),
 * or -1 when the packet is too short for that many frames or claims more
 * than 128. */
int so_remote_pads_add(SoRemotePads *q, s32 newest_frame, const u8 *pads, u32 pad_bytes);

/* Copies the pad for `frame`; returns 0, or -1 if it has not arrived (or
 * has left the ring). */
int so_remote_pads_get(const SoRemotePads *q, s32 frame, u8 *out);

/* A dropped player's inputs (3.7.0's player drop reports, so_netplay.c):
 *
 * so_remote_pads_add_report: the report's `count` pads, newest first, the
 * newest for `last_frame`: the frames after the head are added. Returns
 * the frames added, or -1 when the report is more than 128 frames ahead
 * (AddPlayerDropReportInputs). A frame the report lacks repeats the head.
 *
 * so_remote_pads_fill: repeats the newest pad up to `frame`, from a zero
 * pad at frame 0 when there is none (FillDroppedPlayerInputs).
 *
 * so_remote_pads_report: writes up to `max` of the newest pads, newest
 * first; returns how many, and the newest one's frame (0 for none). */
int so_remote_pads_add_report(SoRemotePads *q, s32 last_frame, u8 count, const u8 *pads);
void so_remote_pads_fill(SoRemotePads *q, s32 frame);
int so_remote_pads_report(const SoRemotePads *q, u8 *out, int max, s32 *newest_frame);

/* ---- ack timers ---------------------------------------------------------- */

#define SO_ACK_TIMER_RING 256

typedef struct {
	s32 frame[SO_ACK_TIMER_RING];
	u64 sent_us[SO_ACK_TIMER_RING];
	u32 head, count;
} SoAckTimers;

void so_ack_timers_reset(SoAckTimers *t);
void so_ack_timers_push(SoAckTimers *t, s32 frame, u64 sent_us);
/* Returns the round trip for an ack of `frame`, or -1 when that frame's send
 * time is not known; older entries are discarded either way. */
s64 so_ack_timers_ack(SoAckTimers *t, s32 frame, u64 now_us);

/* ---- time sync ----------------------------------------------------------- */

#define SO_TIME_SYNC_SAMPLES 30	/* SLIPPI_ONLINE_LOCKSTEP_INTERVAL */

typedef struct {
	s32 samples[SO_TIME_SYNC_SAMPLES];
	u32 count, next;
} SoTimeSync;

void so_time_sync_reset(SoTimeSync *s);
void so_time_sync_add(SoTimeSync *s, s32 offset_us);
/* The mean of the middle third of the samples (Dolphin's
 * CalcTimeOffsetUs); `*valid` is 0 when there are too few samples. */
s32 so_time_sync_offset(const SoTimeSync *s, int *valid);

/* The offset sample for one received pad packet, as in OnData: positive when
 * we run ahead of the peer. `local_frame`/`local_sent_us` are our newest
 * sent frame and its send time; `ping_us` is the latest round trip. */
s32 so_time_sync_sample(u64 arrived_us, u32 ping_us, s32 remote_frame, s32 local_frame, u64 local_sent_us);

#endif
