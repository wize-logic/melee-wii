/* The launcher (launcher.c): before the game starts, a screen to choose how
 * (Melee, or Slippi Online) with its settings, the checks Slippi Online needs
 * (the disc, the account, Slippi's files), and Slippi's start. */
#ifndef MELEE_WII_LAUNCHER_H
#define MELEE_WII_LAUNCHER_H

#include <stddef.h>

#define MELEE_WII_VERSION "1.0.0"

enum { LAUNCH_MELEE, LAUNCH_ONLINE, LAUNCH_MOD };

/* LauncherSettings.screen: SO_WII_SCREEN_* (so_wii.h), or Auto: Slippi's
 * widescreen code when Wii Settings say 16:9 */
#define LAUNCH_SCREEN_AUTO 3

/* sd:/apps/melee/settings.ini. The online rumble and connection are
 * Slippi's own files (rumble.txt, config.ini in its folder); the character
 * select screen writes rumble.txt too. */
typedef struct LauncherSettings {
    int mode;           /* LAUNCH_* */
    int skip;           /* start the last mode at once (hold B for the menu: ui.c) */
    int controller_fix; /* SO_WII_FIX_* (so_wii.h): the rest, offline */
    int pal;
    int mods;
    int stages;
    int gameplay;
    int screen;
    int safety;
    int cheats;  /* the player's Gecko codes (codes/GALE01.gct), offline */
    int replays; /* a .slp of each game (Slippi's recording; offline too) */
    int console; /* Slippi Launcher's console mirroring (TCP 51441) */
    int xx_menu; /* 20XX's menu in Melee and Slippi Online, from a 20XX image (mod_values.c) */
    int diagnostics; /* a record of each online game's timing in logs/diag (slippi/so_gamelog.c) */
    char mod_path[160]; /* LAUNCH_MOD: the modded image (20XX), launcher/mods.c */
} LauncherSettings;

void launcher_mod_start(const LauncherSettings* s, void (*progress)(const char* name, int done, int total));
void launcher_settings_default(LauncherSettings* s);
int launcher_settings_load(LauncherSettings* s, const char* path);
int launcher_settings_save(const LauncherSettings* s, const char* path);

#define LAUNCHER_DIR "sd:/apps/melee"
#define LAUNCHER_SETTINGS LAUNCHER_DIR "/settings.ini"
#define LAUNCHER_SLIPPI LAUNCHER_DIR "/slippi"

#endif
