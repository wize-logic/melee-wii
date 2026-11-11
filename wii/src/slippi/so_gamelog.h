/* Diagnostics (so_gamelog.c): a record of each online game's timing, for
 * finding what makes a game run badly on someone's console and connection.
 * The launcher's "Diagnostics" setting; off, every call here returns at
 * once. */
#ifndef SO_GAMELOG_H
#define SO_GAMELOG_H

#include "global.h"

/* What 0xB0 answered */
enum {
	SO_GAMELOG_PLAY,	/* the game plays the frame */
	SO_GAMELOG_WAIT_INPUTS,	/* waits: the opponent's inputs are too far behind */
	SO_GAMELOG_WAIT_SYNC,	/* waits: ahead of the opponent in time */
	SO_GAMELOG_WAIT_PAD,	/* waits: the game has not taken the last pad read */
	SO_GAMELOG_OTHER,	/* disconnected, no savestates */
};

/* so_wii.c: takes the record's memory (as Slippi starts) */
void so_gamelog_enable(int on);
/* 1 while a game is recorded */
int so_gamelog_recording(void);

/* so_exi.c: a game's start (0xB0 for frame 1), each 0xB0, the game's end
 * (written to the card by a thread) or a game too short to keep */
void so_gamelog_start(void);
void so_gamelog_poll(u64 now_us, s32 frame, int answer, s32 head, s32 offset_us, u32 ping_us, u32 answer_us);
void so_gamelog_end(void);
void so_gamelog_cancel(void);
/* A line of the record's text ("key=value"), while a game is recorded */
void so_gamelog_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* so_rollback.c: a savestate, and a load back to `frame` */
void so_gamelog_save(u32 us, u32 kb);
void so_gamelog_load(u32 us, s32 frame);
/* so_netplay.c: a pad packet of an opponent's, its newest frame and how many
 * frames it brought that had not come (the client lock held) */
void so_gamelog_packet(u64 now_us, int remote, s32 frame, int added, u32 ping_us);
/* wii/src/vi.c: a retrace (from its interrupt). wii/src/stall.c: how long the
 * game's main thread waited (asleep until the retrace or the next pad read:
 * the frame's spare time) */
void so_gamelog_retrace(void);
void so_gamelog_video_wait(u32 us);
/* Something the console did beside the game that took `us` and may hold the
 * network or the game (any thread): `code` is
 *   'B' the console connection's broadcast (so_console.c)
 *   'D' a read of the disc image, 'R' a write of the replay (both the SD card)
 *   'L' a line of the log (written to the SD card within 2 s)
 *   'T' a datagram's send that took 5 ms or more, 'V' a receive that did */
void so_gamelog_event(int code, u32 us);
/* A moment with a value: 'A' the input delay changed during the game, to
 * `value` frames (so_exi.c) */
void so_gamelog_mark(int code, int value);

#endif
