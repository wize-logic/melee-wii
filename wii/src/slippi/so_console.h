/* Slippi's console connection (so_console.c): Slippi Launcher's console
 * mirroring, on TCP 51441, with the Wii's "SLIP_READY" beacon. */
#ifndef SO_CONSOLE_H
#define SO_CONSOLE_H

#include "global.h"

/* Starts the server (and the network, if need be); `nickname` is how Slippi
 * Launcher lists the Wii */
int so_console_start(const char* nickname, const char* version);

/* What Slippi's recording codes write to the device (wii/src/exi.c): from
 * the game's EXI path; never waits */
void so_console_data(const u8* data, u32 len);

#endif
