/* Rollback (so_rollback.c): Slippi Dolphin's side of its online codeset's
 * rollback, on the Wii. Off: the lockstep (delay-based) play of so_exi.c. */
#ifndef SO_ROLLBACK_H
#define SO_ROLLBACK_H

#include "global.h"

void so_rollback_set(int on);
int so_rollback_enabled(void);
int so_rollback_active(void);
void so_rollback_heartbeat(s32 frame);

/* A game's start (0xB0 for frame 1) and end */
void so_rollback_start_game(void);
void so_rollback_end_game(void);

/* 0xB0: 1 for the game to play `frame`, 0 for it to wait */
int so_rollback_ready(s32 frame, s32 finalized, int remotes);
int so_rollback_waits_in_time(void);

/* 0xB1, 0xB2 */
void so_savestate_capture(s32 frame);
void so_savestate_load(s32 frame, const u8* preserve);
/* The rollbacks so far, for the log */
int so_rollback_describe(char* out, int size);

/* More memory for the savestates than the data, bss and the match's heap
 * (so_rollback_ranges.c): calls add(addr, len) for each */
void so_rollback_extra_ranges(void (*add)(u32 addr, u32 len));

#endif
