/* The launcher's updates (update.c): a newer release of Melee for the Wii,
 * from its GitHub releases, checked in the background as the launcher opens
 * and installed when the player says so. */
#ifndef LAUNCHER_UPDATE_H
#define LAUNCHER_UPDATE_H

enum { UPDATE_IDLE, UPDATE_CHECKING, UPDATE_NONE, UPDATE_AVAILABLE, UPDATE_FAILED };

/* Brings the network up and asks GitHub for the latest release, on a thread
 * of its own */
void update_check_start(void);
int update_state(void);
/* UPDATE_AVAILABLE: its version ("1.2.0"); UPDATE_FAILED: why */
const char* update_version(void);
const char* update_error(void);
/* Before the game starts: waits up to `ms` for the check to finish, so it
 * is not bringing the network up when the online client does */
void update_wait(int ms);

/* Downloads the release, checks it, and writes its files over this card's
 * (the player's settings, saves and account are not in it). Returns 0, or
 * < 0 with `error` set. */
int update_install(void (*progress)(const char* what, int done, int total), char* error, int error_size);

#endif
