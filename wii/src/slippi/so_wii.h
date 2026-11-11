/* The Wii port's start of Slippi (so_wii.c): the settings the launcher
 * (wii/src/launcher.c) hands over. Plain ints, so both libogc's and the
 * SDK's sides can include it. */
#ifndef SO_WII_H
#define SO_WII_H

enum { SO_WII_FIX_OFF, SO_WII_UCF, SO_WII_UCF_STEALTH };
enum { SO_WII_MODS_OFF, SO_WII_MODS_STEALTH, SO_WII_MODS_TOURNAMENT, SO_WII_MODS_FRIENDLIES };
enum { SO_WII_STAGES_OFF, SO_WII_STAGES_STADIUM, SO_WII_STAGES_ALL };
enum { SO_WII_GAMEPLAY_OFF, SO_WII_GAMEPLAY_LGL, SO_WII_GAMEPLAY_WOBBLING, SO_WII_GAMEPLAY_BOTH };
enum { SO_WII_SCREEN_OFF, SO_WII_SCREEN_WIDE, SO_WII_SCREEN_WIDE_SHUTTERS };

typedef struct SoWiiSettings {
    int online;         /* Slippi Online (else Melee, with the codes below) */
    char root[96];      /* Slippi's folder ("sd:/apps/melee/slippi") */
    int controller_fix; /* SO_WII_FIX_*, offline (online has UCF) */
    int pal;            /* PAL balance, offline */
    int mods;           /* SO_WII_MODS_*, offline */
    int stages;         /* SO_WII_STAGES_*, offline */
    int gameplay;       /* SO_WII_GAMEPLAY_*, offline */
    int screen;         /* SO_WII_SCREEN_* */
    int safety;         /* Slippi's safety codes, offline */
    int replays;        /* a .slp of each game in <root>/replays */
    int console;        /* Slippi Launcher's console mirroring */
    int diagnostics;    /* a record of each online game's timing (so_gamelog.c) */
    char nickname[32];  /* how Slippi Launcher lists the Wii */
} SoWiiSettings;

/* Starts what the settings ask for; 0, or < 0 when the online client or
 * its codes could not start */
int so_wii_start(const SoWiiSettings* settings);
const SoWiiSettings* so_wii_settings(void);

#endif
