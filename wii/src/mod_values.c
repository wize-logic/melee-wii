/* 20XX's extras in the plain game and Slippi Online, for a player who has a
 * 20XX image (the launcher's "20XX menu" setting; the plan is
 * wii/docs/20xx.md): the image, and the player's values.
 *
 * Only what changes sound or picture is taken: the music playlists
 * (mod_songs.c) and the stages' looks (mod_stages.c), set from 20XX's own
 * menu (mod_menu.c), of which the rest is left out. Nothing of 20XX's is in
 * the port, and none of its code runs: its songs, stage files, names and
 * defaults are read from the player's image, and the game is given another
 * file when it looks one up (dvd.c's substitute).
 *
 * In 20XX an option is a word in its executable's data (1.02 addresses, where
 * the game's own debug menu was). Here each one the port acts on has a word
 * of the port's own, found by that address (melee_wii_mod_value): 20XX's menu
 * items point at theirs, and an item whose variable has none here is not
 * offered. They start as the image has them and are kept in
 * sd:/apps/melee/20xx.ini. */

#ifdef MELEE_ORIG_CODE

#include "melee_wii.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include <wl/platform/platform.h>

#include "mod_values.h"

/* dvd.c */
int melee_wii_disc2_open(const char* path);
void melee_wii_disc2_close(void);
unsigned int melee_wii_disc2_dol(void);
int melee_wii_disc2_read(u32 offset, void* buf, u32 len);
int melee_wii_disc2_entry(const char* path);
void melee_wii_disc_substitute(s32 (*fn)(const char* path));

#define VALUES_PATH "sd:/apps/melee/20xx.ini"

static const char* const PLAYLIST_KEYS[MOD_PLAYLISTS] = {
    "menu",        "yoshis_story",      "fountain",     "pokemon_stadium", "dream_land", "battlefield",
    "final_destination", "green_greens", "trophy",       "temple",          "global",
};
static const char* const SKIN_KEYS[MOD_SKINS] = {
    "pokemon_stadium", "dream_land", "fountain", "yoshis_story", "final_destination", "battlefield",
};

static u32 v_global, v_affects_menu, v_preview;
static u32 v_types[MOD_PLAYLISTS];
static u32 v_lists[MOD_PLAYLISTS * MOD_LIST_WORDS];
static u32 v_skins[MOD_SKINS];
static u32 v_fd_color[4];
static u32 v_music_off, v_alt_music_off, v_costumes;
static u32 v_shields[MOD_SHIELD_WORDS];
static const char* const SHIELD_KEYS[MOD_SHIELD_WORDS] = {
    "p4.blue", "p4.green", "p4.red", "p4.on", "p3.blue", "p3.green", "p3.red", "p3.on",
    "p2.blue", "p2.green", "p2.red", "p2.on", "p1.blue", "p1.green", "p1.red", "p1.on",
};
static const char* const FD_COLOR_KEYS[4] = { "blue", "green", "red", "on" };

/* The variables the port acts on. `limit`: a value's bound in the file (the
 * menu has its own, from 20XX's item) */
static const struct Group {
    u32 addr, words;
    u32* values;
    const char* key;          /* in the file; NULL: not kept */
    const char* const* names; /* key.<name> for each word */
    u32 limit;
} GROUPS[] = {
    { MOD_GLOBAL_ON, 1, &v_global, "global", NULL, 2 },
    { MOD_AFFECTS_MENU, 1, &v_affects_menu, "global_affects_menu", NULL, 2 },
    { MOD_TYPES, MOD_PLAYLISTS, v_types, "type", PLAYLIST_KEYS, 5 },
    { MOD_LISTS, MOD_PLAYLISTS * MOD_LIST_WORDS, v_lists, "songs", PLAYLIST_KEYS, 0x100 },
    { MOD_SKINS_ADDR, MOD_SKINS, v_skins, "skin", SKIN_KEYS, 36 },
    { MOD_FD_COLOR, 4, v_fd_color, "fd_color", FD_COLOR_KEYS, 256 },
    { MOD_MUSIC_OFF, 1, &v_music_off, "music_off", NULL, 2 },
    { MOD_ALT_MUSIC_OFF, 1, &v_alt_music_off, "alt_music_off", NULL, 2 },
    { MOD_SHIELDS, MOD_SHIELD_WORDS, v_shields, "shield", SHIELD_KEYS, 256 },
    { MOD_COSTUMES, 1, &v_costumes, "costumes", NULL, 2 },
    { MOD_SONG_PREVIEW, 1, &v_preview, NULL, NULL, 0x100 },
};
#define GROUP_COUNT (sizeof(GROUPS) / sizeof(GROUPS[0]))

u32* melee_wii_mod_value(u32 addr)
{
    for (u32 g = 0; g < GROUP_COUNT; g++) {
        u32 off = addr - GROUPS[g].addr;
        if (addr >= GROUPS[g].addr && off < GROUPS[g].words * 4 && (off & 3) == 0) {
            /* (a playlist's words past its songs are its name) */
            if (GROUPS[g].addr == MOD_LISTS && (off / 4) % MOD_LIST_WORDS >= MOD_SLOTS) {
                return NULL;
            }
            return &GROUPS[g].values[off / 4];
        }
    }
    return NULL;
}

/* ---- 20XX's executable, by address --------------------------------------- */

static u8 s_dol_header[0x100];

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

int melee_wii_mod_dol_read(u32 addr, void* buf, u32 len)
{
    for (int i = 0; i < 18; i++) {
        u32 off = be32(s_dol_header + i * 4);
        u32 at = be32(s_dol_header + 0x48 + i * 4);
        u32 size = be32(s_dol_header + 0x90 + i * 4);
        if (size != 0 && addr >= at && addr - at <= size && len <= size - (addr - at)) {
            return melee_wii_disc2_read(melee_wii_disc2_dol() + off + (addr - at), buf, len);
        }
    }
    return -1;
}

/* ---- the file ------------------------------------------------------------
 * "key=value" lines:
 *   global=1                 the global playlist, for every stage's song
 *   global_affects_menu=1    and for the menu's
 *   type.<playlist>=N        0 the first song, 1 one of the list, 2 one of
 *                            the game's stage songs, 3 one of the custom
 *                            songs, 4 one of either
 *   songs.<playlist>=31,32   20XX's song numbers (hex: 31 is audio/31.hps)
 *   skin.<stage>=N           the stage's look: 0 the game's own, 15 any
 *   fd_color.on=1, fd_color.red=, .green=, .blue=   Final Destination's colour
 *   music_off=1, alt_music_off=1                    no music; no second songs
 *   shield.p1.on=1, shield.p1.red=, .green=, .blue= a player's shield colour
 *   costumes=1               20XX's costumes, with L and R on the character select
 * <playlist>: menu, yoshis_story, fountain, pokemon_stadium, dream_land,
 * battlefield, final_destination, green_greens, trophy, temple, global
 * <stage>: battlefield, final_destination, yoshis_story, fountain,
 * dream_land, pokemon_stadium */

static void value_line(char* line)
{
    char* eq = strchr(line, '=');
    char* dot;
    if (line[0] == '#' || line[0] == ';' || eq == NULL) {
        return;
    }
    *eq++ = '\0';
    dot = strchr(line, '.');
    if (dot != NULL) {
        *dot++ = '\0';
    }
    for (u32 g = 0; g < GROUP_COUNT; g++) {
        const struct Group* group = &GROUPS[g];
        u32 index = 0;
        if (group->key == NULL || strcmp(line, group->key) != 0 || (dot != NULL) != (group->names != NULL)) {
            continue;
        }
        if (group->names != NULL) {
            u32 names = group->addr == MOD_LISTS ? MOD_PLAYLISTS : group->words;
            while (index < names && strcmp(dot, group->names[index]) != 0) {
                index++;
            }
            if (index == names) {
                return;
            }
        }
        if (group->addr == MOD_LISTS) {
            u32* songs = &group->values[index * MOD_LIST_WORDS];
            u32 n = 0;
            while (n < MOD_SLOTS) {
                char* end;
                u32 song = (u32) strtoul(eq, &end, 16);
                if (end == eq || song == 0 || song >= group->limit) {
                    break;
                }
                songs[n++] = song;
                eq = end + strspn(end, ", \t");
            }
            while (n < MOD_SLOTS) {
                songs[n++] = 0;
            }
        } else {
            u32 value = (u32) strtoul(eq, NULL, 10);
            group->values[index] = value < group->limit ? value : 0;
        }
        return;
    }
}

static char s_text[6144];

/* 1 when there is a file */
static int load_values(void)
{
    WlPlatformFile* file;
    u32 size;
    char* line = s_text;
    if (!wl_platform_fs_exists(VALUES_PATH) || wl_platform_fs_open(VALUES_PATH, WL_FILE_READ, &file) != WL_OK) {
        return 0;
    }
    size = (u32) wl_platform_fs_size(file);
    if (size > sizeof(s_text) - 1) {
        size = sizeof(s_text) - 1;
    }
    if (wl_platform_fs_read_at(file, 0, s_text, size) != WL_OK) {
        size = 0;
    }
    wl_platform_fs_close(file);
    s_text[size] = '\0';
    while (*line != '\0') {
        char* end = line + strcspn(line, "\r\n");
        char* next = end + strspn(end, "\r\n");
        *end = '\0';
        value_line(line);
        line = next;
    }
    return 1;
}

/* The menu, as it is left with a value changed: 0, or -1 */
int melee_wii_mod_save(void)
{
    WlPlatformFile* file;
    size_t n = 0;
    WlResult r;
#define PUT(...) (n += (size_t) snprintf(s_text + n, n < sizeof(s_text) ? sizeof(s_text) - n : 0, __VA_ARGS__))
    PUT("# 20XX's options in Melee and Slippi Online (wii/README.md)\n");
    for (u32 g = 0; g < GROUP_COUNT; g++) {
        const struct Group* group = &GROUPS[g];
        if (group->key == NULL) {
            continue;
        }
        if (group->addr == MOD_LISTS) {
            for (u32 k = 0; k < MOD_PLAYLISTS; k++) {
                const u32* songs = &group->values[k * MOD_LIST_WORDS];
                PUT("%s.%s=", group->key, group->names[k]);
                for (u32 i = 0; i < MOD_SLOTS && songs[i] != 0; i++) {
                    PUT(i == 0 ? "%02X" : ",%02X", (unsigned) songs[i]);
                }
                PUT("\n");
            }
        } else if (group->names != NULL) {
            for (u32 i = 0; i < group->words; i++) {
                PUT("%s.%s=%u\n", group->key, group->names[i], (unsigned) group->values[i]);
            }
        } else {
            PUT("%s=%u\n", group->key, (unsigned) group->values[0]);
        }
    }
#undef PUT
    if (n >= sizeof(s_text) || wl_platform_fs_open(VALUES_PATH, WL_FILE_WRITE_NEW, &file) != WL_OK) {
        OSReport("20XX menu: %s could not be written\n", VALUES_PATH);
        return -1;
    }
    r = wl_platform_fs_write(file, s_text, (u32) n);
    wl_platform_fs_close(file);
    OSReport("20XX menu: %s %s\n", VALUES_PATH, r == WL_OK ? "written" : "could not be written");
    return r == WL_OK ? 0 : -1;
}

/* ---- the start ----------------------------------------------------------- */

static void scene(int kind)
{
    melee_wii_stages_scene(kind);
    melee_wii_songs_scene(kind);
    melee_wii_menu_scene(kind);
}

static s32 substitute(const char* path)
{
    s32 entry = melee_wii_songs_substitute(path);
    return entry >= 0 ? entry : melee_wii_stages_substitute(path);
}

/* The launcher, with the setting on: opens the 20XX image and reads what the
 * port takes from it. 0, or < 0 (and nothing is changed): -1 the image can't
 * be opened, -2 it isn't 20XX, -3 its executable isn't the one this knows. */
int melee_wii_mod_start(const char* image)
{
    static BOOL started;
    int have_values;
    if (started) {
        return 0;
    }
    if (melee_wii_disc2_open(image) < 0) {
        return -1;
    }
    /* (its menu is in this file: wii/tools/mod_menu.py) */
    if (melee_wii_disc2_entry("/MnSlChr.0sd") < 0) {
        melee_wii_disc2_close();
        return -2;
    }
    if (melee_wii_disc2_read(melee_wii_disc2_dol(), s_dol_header, sizeof(s_dol_header)) < 0) {
        melee_wii_disc2_close();
        return -3;
    }
    for (u32 g = 0; g < GROUP_COUNT; g++) {
        const struct Group* group = &GROUPS[g];
        if (group->addr >= 0xFFFF0000u) {
            continue; /* (the port's own: no default in 20XX's executable) */
        }
        /* (both big-endian: the words as they are) */
        if (melee_wii_mod_dol_read(group->addr, group->values, group->words * 4) < 0) {
            melee_wii_disc2_close();
            return -3;
        }
        for (u32 i = 0; i < group->words; i++) {
            if (melee_wii_mod_value(group->addr + i * 4) != NULL && group->values[i] >= group->limit) {
                melee_wii_disc2_close();
                return -3;
            }
        }
    }
    if (melee_wii_songs_start() < 0) {
        melee_wii_disc2_close();
        return -3;
    }
    v_preview = 0;
    have_values = load_values();
    melee_wii_stages_start();
    melee_wii_fighters_start();
    melee_wii_scene_hook = scene;
    melee_wii_disc_substitute(substitute);
    started = TRUE;
    OSReport("20XX menu: %s; %s\n", image, have_values ? VALUES_PATH : "no " VALUES_PATH " (20XX's defaults)");
    melee_wii_menu_start();
    return 0;
}

#endif
