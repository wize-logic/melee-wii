/* The launcher: what runs between the port's start and the game's. It reads
 * the player's settings (launcher_settings.c), shows the launcher's screen
 * (launcher/ui.c) unless told to skip it, makes sure what Slippi Online
 * needs is there (its folder, the account, its files), and starts Slippi
 * (wii/src/slippi/so_wii.c). */

#include "launcher.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <gccore.h>

#include <wl/base/base.h>
#include <wl/platform/platform.h>

#include "launcher/mods.h"
#include "melee_wii.h"
#include "slippi/so_wii.h"

static int exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* The Diagnostics setting's folder (slippi/so_gamelog.c writes a record of
 * each online game there, about 1 MB a game): made if need be, and only its
 * newest DIAG_KEPT records kept (their names are their dates, so the oldest
 * is the first by name). */
#define DIAG_DIR LAUNCHER_DIR "/logs/diag"
#define DIAG_KEPT 40

static void diagnostics_folder(void)
{
    mkdir(LAUNCHER_DIR "/logs", 0777);
    mkdir(DIAG_DIR, 0777);
    for (int pass = 0; pass < 200; pass++) {
        char oldest[64] = "", path[128];
        int count = 0;
        struct dirent* e;
        DIR* dir = opendir(DIAG_DIR);
        if (dir == NULL) {
            return;
        }
        while ((e = readdir(dir)) != NULL) {
            size_t len = strlen(e->d_name);
            if (strncmp(e->d_name, "game-", 5) != 0 || len < 9 || len >= sizeof(oldest) ||
                strcmp(e->d_name + len - 4, ".bin") != 0)
            {
                continue;
            }
            count++;
            if (oldest[0] == '\0' || strcmp(e->d_name, oldest) < 0) {
                snprintf(oldest, sizeof(oldest), "%s", e->d_name);
            }
        }
        closedir(dir);
        if (count < DIAG_KEPT) {
            return;
        }
        snprintf(path, sizeof(path), DIAG_DIR "/%s", oldest);
        if (remove(path) != 0) {
            return;
        }
    }
}

/* Slippi's folder, made if need be. Returns 0, or < 0 when the account
 * (user.json, Slippi Launcher's, copied as is) is not in it. */
int launcher_slippi_folder(void)
{
    char user[128];
    mkdir(LAUNCHER_DIR, 0777);
    mkdir(LAUNCHER_SLIPPI, 0777);
    mkdir(LAUNCHER_SLIPPI "/files", 0777);
    mkdir(LAUNCHER_SLIPPI "/replays", 0777);
    snprintf(user, sizeof(user), "%s/user.json", LAUNCHER_SLIPPI);
    return exists(user) ? 0 : -1;
}

/* The player's cheats (offline): a Gecko .gct, run as a code handler would
 * (wii/src/gecko.c) */
static void load_cheats(void)
{
    static const char* const PATHS[] = {
        LAUNCHER_DIR "/codes/GALE01.gct", "sd:/codes/GALE01.gct", "usb:/codes/GALE01.gct",
    };
    for (size_t i = 0; i < sizeof(PATHS) / sizeof(PATHS[0]); i++) {
        FILE* f = fopen(PATHS[i], "rb");
        long size;
        u8* gct;
        if (f == NULL) {
            continue;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        gct = size > 0 && size < 512 * 1024 ? malloc((size_t) size) : NULL;
        if (gct != NULL && fread(gct, 1, (size_t) size, f) == (size_t) size) {
            WL_LOG_I("launcher", "cheats: %s", PATHS[i]);
            if (melee_wii_gecko_cheats(gct, (u32) size) < 0) {
                WL_LOG_E("launcher", "cheats: could not apply %s", PATHS[i]);
            }
        }
        free(gct);
        fclose(f);
        return;
    }
    WL_LOG_I("launcher", "cheats: no codes/GALE01.gct");
}

/* 20XX's menu, music and stage looks in the plain game and Slippi Online
 * (mod_values.c), from the player's own 20XX image: the one last played, or
 * the first there is. `progress`: the launcher's screen, while it checks the
 * image's stage files (the first time). Once: the screen calls it as it
 * leaves, and melee_wii_launch when the screen was skipped. */
void launcher_mod_start(const LauncherSettings* s, void (*progress)(const char* name, int done, int total))
{
    int melee_wii_mod_start(const char* image);
    void melee_wii_stages_progress(void (*fn)(const char* name, int done, int total));
    static int started;
    ModDisc mods[4];
    int n, pick = -1, res;
    if (started || !s->xx_menu || s->mode == LAUNCH_MOD) {
        return;
    }
    started = 1;
    n = mods_scan(mods, 4);
    for (int i = 0; i < n; i++) {
        if (mods[i].is_20xx && (pick < 0 || strcmp(mods[i].path, s->mod_path) == 0)) {
            pick = i;
        }
    }
    if (pick < 0) {
        WL_LOG_I("launcher", "20XX menu: on, but there is no 20XX image");
        return;
    }
    melee_wii_stages_progress(progress);
    res = melee_wii_mod_start(mods[pick].path);
    melee_wii_stages_progress(NULL);
    if (res < 0) {
        WL_LOG_E("launcher", "20XX menu: %s can't be used (%d)", mods[pick].path, res);
    }
}

/* launcher/ui.c: the screen; returns with the settings to start with */
int launcher_ui(LauncherSettings* settings);

void melee_wii_launch(void)
{
    LauncherSettings s;
    SoWiiSettings so;
    int res;

    LauncherSettings before;
    /* A USB drive, for a mod's image or cheats on it: the disc's
     * own lookup (dvd.c) mounts it only when the SD card has no image */
    wl_platform_fs_mount(WL_DEVICE_USB);
    launcher_settings_load(&s, LAUNCHER_SETTINGS);
    before = s;
    /* written only when changed: the fewer writes to the card the better */
    if (launcher_ui(&s) == 0 && memcmp(&before, &s, sizeof(s)) != 0) {
        launcher_settings_save(&s, LAUNCHER_SETTINGS);
    }

    if (s.mode == LAUNCH_MOD) {
        /* a modded Melee on its own image (20XX), apart from Slippi */
        ModDisc mods[4];
        char error[200];
        int n = mods_scan(mods, 4), i;
        for (i = 0; i < n && strcmp(mods[i].path, s.mod_path) != 0; i++) {
        }
        if (i == n) {
            melee_wii_fatal("%s is gone: pick the game again in the launcher.", s.mod_path);
        }
        if (mods_start(&mods[i], error, sizeof(error)) < 0) {
            melee_wii_fatal("%s", error);
        }
        WL_LOG_I("launcher", "starting %s (%s)", mods[i].title, mods[i].path);
        {
            /* (pad.c: 20XX's own widescreen for the Screen setting) */
            extern int melee_wii_mod_wide;
            melee_wii_mod_wide = s.screen == SO_WII_SCREEN_WIDE ||
                                 (s.screen == LAUNCH_SCREEN_AUTO && CONF_GetAspectRatio() == CONF_ASPECT_16_9);
        }
        if (s.cheats) {
            load_cheats();
        }
        return;
    }

    memset(&so, 0, sizeof(so));
    so.online = s.mode == LAUNCH_ONLINE;
    snprintf(so.root, sizeof(so.root), "%s", LAUNCHER_SLIPPI);
    so.controller_fix = s.controller_fix;
    so.pal = s.pal;
    so.mods = s.mods;
    so.stages = s.stages;
    so.gameplay = s.gameplay;
    so.screen = s.screen != LAUNCH_SCREEN_AUTO           ? s.screen
                : CONF_GetAspectRatio() == CONF_ASPECT_16_9 ? SO_WII_SCREEN_WIDE
                                                            : SO_WII_SCREEN_OFF;
    so.safety = s.safety;
    so.replays = s.replays;
    so.console = s.console;
    so.diagnostics = s.diagnostics;
    if (so.online && so.diagnostics) {
        diagnostics_folder();
    }
    if (CONF_GetNickName((u8*) so.nickname) <= 0 || so.nickname[0] == '\0') {
        snprintf(so.nickname, sizeof(so.nickname), "Wii");
    }
    if (so.online && launcher_slippi_folder() < 0) {
        melee_wii_fatal("Slippi Online needs your Slippi account: copy user.json from\n"
                        "Slippi Launcher into %s on the SD card.", LAUNCHER_SLIPPI);
    }
    WL_LOG_I("launcher", "starting %s", so.online ? "Slippi Online" : "Melee");
    res = so_wii_start(&so);
    if (res < 0) {
        melee_wii_fatal("Slippi Online did not start (%d).", res);
    }
    launcher_mod_start(&s, NULL);
    if (so.online) {
        /* (mod_menu.c: "Extra" on Slippi's Online Play screen, for 20XX's menu) */
        void melee_wii_menu_extra(void);
        melee_wii_menu_extra();
    }
    if (so.online) {
        /* the lockstep with Slippi Dolphin: its rounding (fused.c) */
        void melee_wii_fused_install(void);
        melee_wii_fused_install();
    }
    if (!so.online && s.cheats) {
        load_cheats();
    }
}
