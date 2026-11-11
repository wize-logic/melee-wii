/* 20XX's music playlists in Slippi Online (mod_values.c).
 *
 * 20XX does it with a patch at the start of the game's music function, which
 * rewrites the file name of the song the game is about to play and picks at
 * random with the game's own generator (HSD_Randi). Neither is done here:
 * the game is given another file when it looks its song up, and the choice
 * is made with a generator of the port's own, so that nothing a match
 * depends on is touched and an opponent's game, which plays its own music,
 * stays in step.
 *
 * What it takes from 20XX's executable, besides the playlists (mod_values.h):
 * the song names. The game's 99 names are at the same addresses in both
 * executables; where the game has "ystory.hps" 20XX has "27.hps", its own
 * number for that song. A song 20XX left its name (the fanfares, the
 * hammer's) has no number and is never replaced, by the global playlist
 * either (20XX's own replaces whatever the music function is asked for). */

#ifdef MELEE_ORIG_CODE

#include "melee_wii.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/os.h>

#include "mod_values.h"

/* dvd.c */
int melee_wii_disc2_entry(const char* path);
int melee_wii_disc_entry(const char* path);

/* The game's: the stage loaded (gr/ground.c's stage_info), whose parameters
 * (+0x6B0) have a row for each stage the file serves (at +0xB0, how many at
 * +0xB4; 0x64 bytes each): the stage, its song and its second song, the
 * same for sudden death */
#define STAGE_PARAM (*(u8* volatile*) (0x8049E6C8u + 0x6B0u))
#define ROW_SIZE 0x64u
#define SCENE_GAME 2

#define NAMES_LO 0x803BBDDCu /* "1p_qk.hps" */
#define NAMES_HI 0x803BC314u /* past "testnz.hps" */
#define SONG_NAMES 99

/* 20XX's song numbers: the game's own are 1 to 0x30 (the stages' from 0x18),
 * the custom ones follow */
#define FIRST_STAGE_SONG 0x18u
#define STAGE_SONGS 0x19u
#define FIRST_CUSTOM 0x31u
/* The game's numbers of the menu's two songs (menu01, menu3) */
#define GAME_MENU_1 0x34u
#define GAME_MENU_2 0x36u

enum { TYPE_SINGLE, TYPE_RANDOM_LIST, TYPE_RANDOM_GAME, TYPE_RANDOM_CUSTOM, TYPE_RANDOM_BOTH };

static const char* const PLAYLIST_NAMES[MOD_PLAYLISTS] = {
    "menu",    "Yoshi's Story",     "Fountain of Dreams", "Pokemon Stadium", "Dream Land", "Battlefield",
    "Final Destination", "Green Greens", "trophy",    "Hyrule Temple",   "global",
};

static char s_names[NAMES_HI - NAMES_LO + 4]; /* 20XX's, where the game has its own */
static u32 s_owner[MOD_PLAYLISTS];
static u32 s_custom;  /* how many custom songs the image has */
static u32 s_preview; /* the menu's: this song, whatever is asked for */
static int s_scene = -1; /* the scene the game is in */
static struct {
    BOOL have;
    u32 scene, song;
} s_pick[MOD_PLAYLISTS];

static BOOL same_nocase(const char* a, const char* b)
{
    while (*a != '\0' && tolower((unsigned char) *a) == tolower((unsigned char) *b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* The names 20XX has where the game has its 99: each must be a song's */
static BOOL names_ok(void)
{
    u32 a = 0, count = 0;
    while (a < NAMES_HI - NAMES_LO) {
        size_t len = strlen(&s_names[a]);
        if (len == 0) {
            a += 4;
            continue;
        }
        if (len < 5 || len > 15 || !same_nocase(&s_names[a + len - 4], ".hps")) {
            return FALSE;
        }
        a += (len + 4) & ~3u;
        count++;
    }
    return count == SONG_NAMES;
}

u32 melee_wii_songs_count(void)
{
    return FIRST_CUSTOM + s_custom;
}

void melee_wii_songs_preview(u32 song)
{
    s_preview = song;
}

/* The port's own random numbers: never the game's, which a match's two sides
 * must draw alike */
static u32 random_below(u32 n)
{
    static u32 x;
    if (x == 0) {
        x = (u32) OSGetTime() | 1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return n != 0 ? x % n : 0;
}

/* A playlist's song. It keeps it for the scene: the game asks again within a
 * match (after the hammer's song, or as a rollback plays a frame again) and
 * must get the same one. */
static u32 pick(int k)
{
    const u32* songs = melee_wii_mod_value(MOD_LISTS + (u32) k * MOD_LIST_WORDS * 4);
    u32 scene = melee_wii_scene_count();
    u32 n = 0, song;
    if (s_pick[k].have && s_pick[k].scene == scene) {
        return s_pick[k].song;
    }
    while (n < MOD_SLOTS && songs[n] != 0) {
        n++;
    }
    switch (*melee_wii_mod_value(MOD_TYPES + (u32) k * 4)) {
    case TYPE_RANDOM_LIST:
        song = n != 0 ? songs[random_below(n)] : 0;
        break;
    case TYPE_RANDOM_GAME:
        song = FIRST_STAGE_SONG + random_below(STAGE_SONGS);
        break;
    case TYPE_RANDOM_CUSTOM:
        song = s_custom != 0 ? FIRST_CUSTOM + random_below(s_custom) : songs[0];
        break;
    case TYPE_RANDOM_BOTH:
        song = FIRST_STAGE_SONG + random_below(STAGE_SONGS + s_custom);
        break;
    default:
        song = songs[0];
        break;
    }
    if (song == 0 || song >= melee_wii_songs_count()) {
        song = 1; /* (as 20XX: "none" is the menu's first song) */
    }
    s_pick[k].have = TRUE;
    s_pick[k].scene = scene;
    s_pick[k].song = song;
    return song;
}

/* The game's song of this file name: its name's address and its number */
static BOOL find_song(const char* name, u32* at, u32* number)
{
    u32 a = NAMES_LO, n = 0;
    while (a < NAMES_HI) {
        const char* s = (const char*) a;
        size_t len = strnlen(s, NAMES_HI - a);
        if (len == 0) {
            a += 4;
            continue;
        }
        if (len < NAMES_HI - a && same_nocase(s, name)) {
            *at = a;
            *number = n;
            return TRUE;
        }
        a += (len + 4) & ~3u;
        n++;
    }
    return FALSE;
}

void melee_wii_songs_scene(int kind)
{
    s_scene = kind;
}

/* The name's address of the game's song of this number, or 0 */
static u32 song_name(u32 number)
{
    u32 a = NAMES_LO, n = 0;
    while (a < NAMES_HI) {
        size_t len = strnlen((const char*) a, NAMES_HI - a);
        if (len == 0) {
            a += 4;
            continue;
        }
        if (n == number) {
            return a;
        }
        a += (len + 4) & ~3u;
        n++;
    }
    return 0;
}

/* "Disable alternate music": the first song of the stage being played whose
 * second song this is, or -1. The game has drawn which of the two to play
 * (from its random numbers, as the opponent's game does: that is left
 * alone); only the file it gets is the other. By the file's name alone a
 * second song can't be told from another stage's first (Venom's is
 * Corneria's second), so this goes by the loaded stage's rows, in a match. */
static int first_song_of(u32 number)
{
    const u8* param = STAGE_PARAM;
    const u8* rows;
    u32 count;
    if (s_scene != SCENE_GAME || (u32) param < 0x80003100u || (u32) param >= 0x817FFF00u) {
        return -1;
    }
    memcpy(&rows, param + 0xB0, 4);
    memcpy(&count, param + 0xB4, 4);
    if ((u32) rows < 0x80003100u || (u32) rows >= 0x817F0000u || count > 16) {
        return -1;
    }
    for (u32 i = 0; i < count; i++) {
        u32 songs[4]; /* the song, the second, sudden death's, its second */
        memcpy(songs, rows + i * ROW_SIZE + 4, sizeof(songs));
        for (u32 k = 0; k < 4; k += 2) {
            if (songs[k + 1] == number && songs[k] != number && songs[k] < SONG_NAMES) {
                return (int) songs[k];
            }
        }
    }
    return -1;
}

/* lbAudioAx_80023F28's call of fn_80023ED4 (cmake/Wii.cmake's --redirect):
 * the game starts a song. "Disable all music": it doesn't. */
#define fn_80023ED4 ((int (*)(const char*, int, int)) 0x80023ED4u)

int melee_wii_song_play(const char* path, int vol, int arg2)
{
    const u32* off = melee_wii_mod_value(MOD_MUSIC_OFF);
    if (off != NULL && *off != 0 && s_names[0] != '\0' && s_preview == 0) {
        return 0;
    }
    return fn_80023ED4(path, vol, arg2);
}

/* The game looks a file up (mod_values.c, for dvd.c). For a song a playlist
 * replaces, the entry of the playlist's song in the 20XX image. */
s32 melee_wii_songs_substitute(const char* path)
{
    static u32 s_missing; /* (said once each) */
    char other[64];
    const char* base;
    const char* own;
    size_t len;
    u32 at, number, song;
    int k = -1, entry, first;
    BOOL moved = FALSE; /* a second song, played as the stage's first */
    if (path == NULL) {
        return -1;
    }
    len = strlen(path);
    if (len < 5 || len >= sizeof(other) - 8 || !same_nocase(path + len - 4, ".hps")) {
        return -1;
    }
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    if (!find_song(base, &at, &number)) {
        return -1;
    }
    if (s_preview == 0 && *melee_wii_mod_value(MOD_ALT_MUSIC_OFF) != 0 && (first = first_song_of(number)) >= 0 &&
        song_name((u32) first) != 0)
    {
        number = (u32) first;
        at = song_name(number);
        moved = TRUE;
    }
    own = &s_names[at - NAMES_LO];
    if (same_nocase(own, (const char*) at)) {
        /* not one of 20XX's numbered songs: the game's own file */
        goto own_file;
    }
    if (s_preview != 0) {
        song = s_preview;
    } else {
        if (*melee_wii_mod_value(MOD_GLOBAL_ON) != 0 &&
            ((number != GAME_MENU_1 && number != GAME_MENU_2) || *melee_wii_mod_value(MOD_AFFECTS_MENU) != 0))
        {
            k = MOD_GLOBAL_PLAYLIST;
        } else if (number == GAME_MENU_1 || number == GAME_MENU_2) {
            /* (the game goes from one of its two menu songs to the other;
             * 20XX's menu playlist has the first's name only, and the second
             * would play as it is between the playlist's songs) */
            k = 0;
        } else {
            for (int i = 1; i < MOD_GLOBAL_PLAYLIST; i++) {
                if (s_owner[i] == at) {
                    k = i;
                }
            }
        }
        if (k < 0) {
            goto own_file;
        }
        if (k == 0 && number == GAME_MENU_2 && *melee_wii_mod_value(MOD_TYPES) == TYPE_SINGLE &&
            *melee_wii_mod_value(MOD_LISTS) == 1)
        {
            goto own_file; /* the menu playlist as 20XX has it: the game's two menu songs as they are */
        }
        song = pick(k);
    }
    snprintf(other, sizeof(other), "%.*s%02X.hps", (int) (base - path), path, (unsigned) song);
    if (same_nocase(other + (base - path), own)) {
        goto own_file; /* the song itself */
    }
    entry = melee_wii_disc2_entry(other);
    if (entry < 0) {
        if (s_missing != song) {
            s_missing = song;
            OSReport("20XX songs: the image has no %s\n", other);
        }
        return -1;
    }
    OSReport("20XX songs: %s in place of %s (%s)\n", other + (base - path), base,
             k >= 0 ? PLAYLIST_NAMES[k] : "the menu's preview");
    return entry;

own_file:
    /* the game's own file: the one it asked for, or the stage's first song's */
    if (!moved) {
        return -1;
    }
    snprintf(other, sizeof(other), "%.*s%s", (int) (base - path), path, (const char*) at);
    entry = melee_wii_disc_entry(other);
    if (entry >= 0) {
        OSReport("20XX songs: %s in place of %s (the stage's first song)\n", (const char*) at, base);
    }
    return entry;
}

/* mod_values.c, as the image is opened: 0, or -1 when its executable isn't
 * the one this knows */
int melee_wii_songs_start(void)
{
    if (melee_wii_mod_dol_read(NAMES_LO, s_names, NAMES_HI - NAMES_LO) < 0 || !names_ok() ||
        melee_wii_mod_dol_read(MOD_OWNERS, s_owner, sizeof(s_owner)) < 0)
    {
        return -1;
    }
    for (int k = 0; k < MOD_PLAYLISTS; k++) {
        if (s_owner[k] < NAMES_LO || s_owner[k] >= NAMES_HI) {
            return -1;
        }
    }
    for (s_custom = 0; FIRST_CUSTOM + s_custom < 0xFF; s_custom++) {
        char path[24];
        snprintf(path, sizeof(path), "/audio/%02X.hps", (unsigned) (FIRST_CUSTOM + s_custom));
        if (melee_wii_disc2_entry(path) < 0) {
            break;
        }
    }
    s_preview = 0;
    OSReport("20XX songs: %u custom songs\n", (unsigned) s_custom);
    return 0;
}

#endif
