/* 20XX's stage looks in the plain game and Slippi Online (mod_values.c).
 *
 * In 20XX a look is a whole stage file, chosen by its name's last letters
 * (GrNBa.3at for Battlefield's third). Here the game is given that file when
 * it looks its own up, but only a file that is the original stage with
 * another look: 20XX's files differ in more than textures (its own default
 * Battlefield has other collision bounds), and online both sides must play
 * the same stage. stage_check.c compares each with the original, once: the
 * verdicts are kept in sd:/apps/melee/20xx-stages.txt, by each file's place
 * and size in the image (delete it to have them made again).
 *
 * 20XX gives some looks their own songs, by numbers the game doesn't have, in
 * the stage's rows of its parameters. The game gets the original's rows: the
 * check returns them, and they are put over what is read from the file.
 *
 * Final Destination's custom colour is done the same way, as 20XX does it as
 * it loads the file: the red, green and blue of two materials, at fixed
 * places of a Final Destination file of the original's size (the game's own
 * or a look's). 20XX's other stage options are not taken: "Disable FD BG
 * transitions" and "BF rainbow water BG" change how many random numbers the
 * stage draws, which a match's two sides must draw alike, and "Rainbow FD"
 * writes at an address worked out from 20XX's own memory layout. */

#ifdef MELEE_ORIG_CODE

#include "melee_wii.h"

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>

#include <wl/platform/platform.h>

#include "mod_values.h"
#include "stage_check.h"

/* dvd.c */
int melee_wii_disc_file(const char* name, u32* offset, u32* size);
int melee_wii_disc_read(u32 offset, void* buf, u32 len);
int melee_wii_disc2_file(const char* path, u32* offset, u32* size);
int melee_wii_disc2_read(u32 offset, void* buf, u32 len);
int melee_wii_disc2_entry(const char* path);
void melee_wii_disc_after_read(void (*fn)(int second, u32 offset, void* buf, u32 len));
void melee_wii_disc2_each(void (*fn)(const char* name, u32 offset, u32 size));
void melee_wii_disc_overlay(u32 from, u32 to, u32 other, const u32* on);

#define VERDICTS_PATH "sd:/apps/melee/20xx-stages.txt"
#define LOOKS 15      /* 0 the game's own, 1 to 14 a file each */
#define LOOK_RANDOM 15
#define MAX_PATCHES 2
/* A look's file may be this much bigger than the stage's: the stage's memory
 * is the game's, and has only so much to spare */
#define MAX_GROWTH (256u * 1024u)

/* In the order of 20XX's variables (MOD_SKINS_ADDR); a look's file has its
 * number (0-9, a-e) for the %c */
static const struct {
    const char* original;
    const char* look;
    const char* name;
} STAGES[MOD_SKINS] = {
    { "GrPs.usd", "GrP%c.usd", "Pokemon Stadium" }, { "GrOp.dat", "GrOp.%cat", "Dream Land" },
    { "GrIz.dat", "GrIz.%cat", "Fountain of Dreams" }, { "GrSt.dat", "GrSt.%cat", "Yoshi's Story" },
    { "GrNLa.dat", "GrNLa.%cat", "Final Destination" }, { "GrNBa.dat", "GrNBa.%cat", "Battlefield" },
};

typedef struct Look {
    u32 offset, size; /* in the 20XX image; size 0: no such file */
    u8 known;         /* a verdict */
    u8 ok;
    u8 patch_count;
    StagePatch patches[MAX_PATCHES];
} Look;

/* Final Destination: its place among the stages, its file's size, and where
 * its two materials' colours are in it */
#define STAGE_FD 4
#define FD_FILE_SIZE 0x95335u
static const u32 FD_COLORS[2] = { 0x7974, 0x7B84 };

/* 20XX's costumes: for a character's costume file (PlFxNr.dat) up to two
 * others, of its two sets (PlFxNr.lat, PlFxNr.rat), which a player chooses
 * on the character select screen with L and R, as in 20XX (mod_fighters.c).
 * The game gets the one chosen when it looks its own up, if it passed the
 * same check: a costume file holds the character's skeleton, which its
 * hitboxes hang on, and that must be the original's. In 20XX the choice is a
 * letter of the file's name in the game's table of costumes, so it is the
 * costume's, whoever plays it; here too (and no two players have the same
 * costume). It lasts until the game is left. */
#define COSTUMES 160
#define COSTUME_SETS 2
static const char* const SET_EXT[COSTUME_SETS] = { ".lat", ".rat" };
typedef struct Costume {
    char name[8]; /* "PlFxNr" */
    struct {
        u32 offset, size;
        u8 known, ok;
    } set[COSTUME_SETS];
    u8 chosen; /* 0 the game's own, 1 or 2 the set */
} Costume;
static Costume s_costumes[COSTUMES];
static u32 s_costume_count;

static Look s_looks[MOD_SKINS][LOOKS];
static u32 s_fd_offset, s_fd_size; /* the game's own, in its image */
static u32 s_random[MOD_SKINS]; /* the look "random" is, until the next menu or character select */
static void (*s_progress)(const char* name, int done, int total);

void melee_wii_stages_progress(void (*fn)(const char* name, int done, int total))
{
    s_progress = fn;
}

static void look_name(char* out, size_t size, u32 stage, u32 look)
{
    snprintf(out, size, STAGES[stage].look, (int) (look < 10 ? '0' + look : 'a' + look - 10));
}

int melee_wii_stages_allowed(u32 stage, u32 look)
{
    if (stage >= MOD_SKINS) {
        return 0;
    }
    if (look == 0 || look == LOOK_RANDOM) {
        return 1;
    }
    return look < LOOKS && s_looks[stage][look].ok;
}

/* ---- the verdicts' file --------------------------------------------------
 * A line a file: its name, its place and size in the image, 1 or 0, then
 * the song rows to put back (where in the file: 40 hex digits), or why not */

static char s_text[24 * 1024];

static void read_verdicts(void)
{
    WlPlatformFile* file;
    u32 size;
    char* line = s_text;
    if (!wl_platform_fs_exists(VERDICTS_PATH) || wl_platform_fs_open(VERDICTS_PATH, WL_FILE_READ, &file) != WL_OK) {
        return;
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
        char name[16], rest[160];
        unsigned offset, bytes, ok;
        *end = '\0';
        rest[0] = '\0';
        if (line[0] != '#' && sscanf(line, "%15s %x %x %u %159[^\n]", name, &offset, &bytes, &ok, rest) >= 4) {
            for (u32 c = 0; c < s_costume_count; c++) {
                for (u32 k = 0; k < COSTUME_SETS; k++) {
                    if (strncmp(name, s_costumes[c].name, 6) == 0 && strcmp(name + 6, SET_EXT[k]) == 0 &&
                        s_costumes[c].set[k].size != 0 && s_costumes[c].set[k].offset == offset &&
                        s_costumes[c].set[k].size == bytes)
                    {
                        s_costumes[c].set[k].known = 1;
                        s_costumes[c].set[k].ok = ok == 1;
                    }
                }
            }
            for (u32 s = 0; s < MOD_SKINS; s++) {
                for (u32 k = 1; k < LOOKS; k++) {
                    Look* look = &s_looks[s][k];
                    char want[16];
                    const char* p = rest;
                    look_name(want, sizeof(want), s, k);
                    if (strcmp(want, name) != 0 || look->size == 0 || look->offset != offset || look->size != bytes) {
                        continue;
                    }
                    look->known = 1;
                    look->ok = ok == 1;
                    look->patch_count = 0;
                    while (look->ok && look->patch_count < MAX_PATCHES) {
                        unsigned at;
                        int used = 0;
                        StagePatch* patch = &look->patches[look->patch_count];
                        if (sscanf(p, " %x:%n", &at, &used) < 1 || used == 0 || strlen(p + used) < STAGE_PATCH_BYTES * 2) {
                            break;
                        }
                        p += used;
                        for (int i = 0; i < STAGE_PATCH_BYTES; i++) {
                            unsigned byte = 0;
                            sscanf(p + i * 2, "%2x", &byte);
                            patch->bytes[i] = (u8) byte;
                        }
                        p += STAGE_PATCH_BYTES * 2;
                        patch->offset = at;
                        look->patch_count++;
                    }
                }
            }
        }
        line = next;
    }
}

static void write_verdicts(void)
{
    WlPlatformFile* file;
    size_t n = 0;
#define PUT(...) (n += (size_t) snprintf(s_text + n, n < sizeof(s_text) ? sizeof(s_text) - n : 0, __VA_ARGS__))
    PUT("# 20XX's stage files that are the original stages with another look (1), and not (0).\n"
        "# Made by the port from your images; delete it to have them checked again.\n");
    for (u32 s = 0; s < MOD_SKINS; s++) {
        for (u32 k = 1; k < LOOKS; k++) {
            const Look* look = &s_looks[s][k];
            char name[16];
            if (look->size == 0 || !look->known) {
                continue;
            }
            look_name(name, sizeof(name), s, k);
            PUT("%s %x %x %u", name, (unsigned) look->offset, (unsigned) look->size, look->ok);
            for (u32 p = 0; p < look->patch_count; p++) {
                PUT(" %x:", look->patches[p].offset);
                for (int i = 0; i < STAGE_PATCH_BYTES; i++) {
                    PUT("%02x", look->patches[p].bytes[i]);
                }
            }
            PUT("\n");
        }
    }
    for (u32 c = 0; c < s_costume_count; c++) {
        for (u32 k = 0; k < COSTUME_SETS; k++) {
            if (s_costumes[c].set[k].size != 0 && s_costumes[c].set[k].known) {
                PUT("%s%s %x %x %u\n", s_costumes[c].name, SET_EXT[k], (unsigned) s_costumes[c].set[k].offset,
                    (unsigned) s_costumes[c].set[k].size, s_costumes[c].set[k].ok);
            }
        }
    }
#undef PUT
    if (n >= sizeof(s_text) || wl_platform_fs_open(VERDICTS_PATH, WL_FILE_WRITE_NEW, &file) != WL_OK) {
        OSReport("20XX stages: %s could not be written\n", VERDICTS_PATH);
        return;
    }
    wl_platform_fs_write(file, s_text, (u32) n);
    wl_platform_fs_close(file);
}

/* ---- the check ----------------------------------------------------------- */

static void check_stage(u32 stage, int* done, int total)
{
    u32 offset, size;
    u8* original = NULL;
    if (melee_wii_disc_file(STAGES[stage].original, &offset, &size) == 0 && size != 0 && size < 16u * 1024 * 1024) {
        original = memalign(32, (size + 31) & ~31u);
        if (original != NULL && melee_wii_disc_read(offset, original, size) < 0) {
            free(original);
            original = NULL;
        }
    }
    for (u32 k = 1; k < LOOKS; k++) {
        Look* look = &s_looks[stage][k];
        char name[16];
        const char* why = "the original could not be read";
        u8* modded;
        int count = 0;
        if (look->size == 0 || look->known) {
            continue;
        }
        look_name(name, sizeof(name), stage, k);
        if (s_progress != NULL) {
            s_progress(name, *done, total);
        }
        (*done)++;
        look->known = 1;
        look->ok = 0;
        look->patch_count = 0;
        if (original == NULL) {
            /* (not kept: checked again next time) */
            look->known = 0;
        } else if (look->size > size + MAX_GROWTH) {
            why = "too big for the stage's memory";
        } else if ((modded = memalign(32, (look->size + 31) & ~31u)) == NULL) {
            why = "no memory";
            look->known = 0;
        } else {
            if (melee_wii_disc2_read(look->offset, modded, look->size) < 0) {
                why = "it could not be read";
                look->known = 0;
            } else if (stage_check(original, size, modded, look->size, look->patches, MAX_PATCHES, &count, &why) == 0) {
                look->ok = 1;
                look->patch_count = (u8) count;
            }
            free(modded);
        }
        OSReport("20XX stages: %s %s%s\n", name, look->ok ? "is the stage with another look" : "is left out: ",
                 look->ok ? "" : why);
    }
    free(original);
}

/* A costume's files of 20XX's two sets against the game's own */
static void check_costume(Costume* costume, int* done, int total)
{
    char name[16];
    u32 offset, size;
    u8* original = NULL;
    if (costume->set[0].known >= (costume->set[0].size != 0) && costume->set[1].known >= (costume->set[1].size != 0)) {
        return;
    }
    snprintf(name, sizeof(name), "%.6s.dat", costume->name);
    if (melee_wii_disc_file(name, &offset, &size) == 0 && size != 0 && size < 16u * 1024 * 1024) {
        original = memalign(32, (size + 31) & ~31u);
        if (original != NULL && melee_wii_disc_read(offset, original, size) < 0) {
            free(original);
            original = NULL;
        }
    }
    for (u32 k = 0; k < COSTUME_SETS; k++) {
        const char* why = "the game has no such costume";
        StagePatch none[1];
        int count = 0;
        u8* modded;
        if (costume->set[k].size == 0 || costume->set[k].known) {
            continue;
        }
        snprintf(name, sizeof(name), "%.6s%s", costume->name, SET_EXT[k]);
        if (s_progress != NULL) {
            s_progress(name, *done, total);
        }
        (*done)++;
        costume->set[k].known = 1;
        costume->set[k].ok = 0;
        if (original == NULL) {
            /* (kept: there is nothing to give it in place of) */
        } else if (costume->set[k].size > size + MAX_GROWTH) {
            why = "too big for the character's memory";
        } else if ((modded = memalign(32, (costume->set[k].size + 31) & ~31u)) == NULL) {
            why = "no memory";
            costume->set[k].known = 0;
        } else {
            if (melee_wii_disc2_read(costume->set[k].offset, modded, costume->set[k].size) < 0) {
                why = "it could not be read";
                costume->set[k].known = 0;
            } else if (stage_check(original, size, modded, costume->set[k].size, none, 0, &count, &why) == 0) {
                costume->set[k].ok = 1;
            }
            free(modded);
        }
        if (!costume->set[k].ok) {
            OSReport("20XX costumes: %s is left out: %s\n", name, why);
        }
    }
    free(original);
}

/* (melee_wii_disc2_each) a file of the 20XX image: a costume of one of its
 * sets? */
static void costume_file(const char* name, u32 offset, u32 size)
{
    u32 k, c;
    if (strlen(name) != 10 || name[0] != 'P' || name[1] != 'l' || size == 0) {
        return;
    }
    for (k = 0; k < COSTUME_SETS && strcmp(name + 6, SET_EXT[k]) != 0; k++) {
    }
    if (k == COSTUME_SETS) {
        return;
    }
    for (c = 0; c < s_costume_count && strncmp(s_costumes[c].name, name, 6) != 0; c++) {
    }
    if (c == s_costume_count) {
        if (s_costume_count == COSTUMES) {
            return;
        }
        memset(&s_costumes[c], 0, sizeof(s_costumes[c]));
        memcpy(s_costumes[c].name, name, 6);
        s_costume_count++;
    }
    s_costumes[c].set[k].offset = offset;
    s_costumes[c].set[k].size = size;
}

/* The character select screen's portraits. 20XX's show a costume with its
 * other two beside it ("tri-CSPs"), which is how a player sees what L and R
 * will give. They are in its MnSlChr.0sd where the game's MnSlChr.usd has
 * its own: 118 pictures with their palettes, one after the other, at the
 * same place in both files (the rest of 20XX's file is its own screen, its
 * code and its menu). With 20XX's costumes on, the game reads that stretch
 * from 20XX's file. Both files are looked at first: a picture's description
 * at each end of the list must be the one known. */
#define PORTRAITS_FROM 0x4CA40u
#define PORTRAITS_TO 0x33CE40u

static void portraits_start(void)
{
    static const u32 AT[2] = { 0x34C304u, 0x3975D8u };
    static const u8 IS[2][12] = { { 0x00, 0x11, 0xF0, 0x20, 0x00, 0x88, 0x00, 0xBC, 0x00, 0x00, 0x00, 0x09 },
                                  { 0x00, 0x30, 0x9E, 0x20, 0x00, 0x88, 0x00, 0xBC, 0x00, 0x00, 0x00, 0x09 } };
    u32 own, own_size, theirs, theirs_size;
    if (melee_wii_disc_file("MnSlChr.usd", &own, &own_size) < 0 ||
        melee_wii_disc2_file("/MnSlChr.0sd", &theirs, &theirs_size) < 0 || own_size < AT[1] + 12 ||
        theirs_size < AT[1] + 12)
    {
        return;
    }
    for (u32 i = 0; i < 2; i++) {
        u8 a[32] ATTRIBUTE_ALIGN(32), b[32] ATTRIBUTE_ALIGN(32);
        if (melee_wii_disc_read(own + AT[i], a, 12) < 0 || melee_wii_disc2_read(theirs + AT[i], b, 12) < 0 ||
            memcmp(a, IS[i], 12) != 0 || memcmp(b, IS[i], 12) != 0)
        {
            OSReport("20XX costumes: the character select screen's files are not the ones known: its own portraits\n");
            return;
        }
    }
    melee_wii_disc_overlay(own + PORTRAITS_FROM, own + PORTRAITS_TO, theirs + PORTRAITS_FROM,
                           melee_wii_mod_value(MOD_COSTUMES));
    OSReport("20XX costumes: 20XX's portraits on the character select screen while they are on\n");
}

/* ---- the game's side ----------------------------------------------------- */

static BOOL same_nocase(const char* a, const char* b)
{
    while (*a != '\0' && tolower((unsigned char) *a) == tolower((unsigned char) *b)) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static u32 random_below(u32 n)
{
    static u32 x; /* (the port's own: never the game's random numbers) */
    if (x == 0) {
        x = (u32) OSGetTime() | 1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return n != 0 ? x % n : 0;
}

static void roll(void)
{
    for (u32 s = 0; s < MOD_SKINS; s++) {
        u32 allowed[LOOKS], n = 0;
        for (u32 k = 0; k < LOOKS; k++) {
            if (melee_wii_stages_allowed(s, k)) {
                allowed[n++] = k;
            }
        }
        s_random[s] = allowed[random_below(n)];
    }
}

/* mod_fighters.c: L or R on a costume. The costume of this file ("PlFxNr.dat")
 * becomes the set's, or the game's own again if it was the set's: the set it
 * now is (0: the game's own), or -1 when 20XX has no such costume that can
 * be played. */
int melee_wii_costumes_toggle(const char* file, u32 set)
{
    if (set < 1 || set > COSTUME_SETS || file == NULL) {
        return -1;
    }
    for (u32 c = 0; c < s_costume_count; c++) {
        if (strncmp(s_costumes[c].name, file, 6) == 0) {
            if (!s_costumes[c].set[set - 1].ok) {
                return -1;
            }
            s_costumes[c].chosen = s_costumes[c].chosen == set ? 0 : (u8) set;
            return s_costumes[c].chosen;
        }
    }
    return -1;
}

/* The set this file's costume now is (0: the game's own) */
int melee_wii_costumes_chosen(const char* file)
{
    for (u32 c = 0; file != NULL && c < s_costume_count; c++) {
        if (strncmp(s_costumes[c].name, file, 6) == 0) {
            u32 set = s_costumes[c].chosen;
            return set != 0 && s_costumes[c].set[set - 1].ok ? (int) set : 0;
        }
    }
    return 0;
}

/* A second character's costume follows the first's (Nana's, Sheik's) */
void melee_wii_costumes_set(const char* file, u32 set)
{
    for (u32 c = 0; file != NULL && set <= COSTUME_SETS && c < s_costume_count; c++) {
        if (strncmp(s_costumes[c].name, file, 6) == 0) {
            s_costumes[c].chosen = set != 0 && s_costumes[c].set[set - 1].ok ? (u8) set : 0;
        }
    }
}

/* pad.c: the game enters a scene. "Random" is drawn again at the menu and
 * the character select, never between a stage's two lookups (as it is
 * loaded ahead, and as the match starts). */
void melee_wii_stages_scene(int kind)
{
    if (kind == 1 || kind == 8) {
        roll();
    }
}

/* The game looks a file up (mod_values.c, for dvd.c). For a stage whose look
 * is another, the entry of that look's file in the 20XX image. */
s32 melee_wii_stages_substitute(const char* path)
{
    const char* base;
    if (path == NULL) {
        return -1;
    }
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    if (base[0] == 'P' && base[1] == 'l' && strlen(base) == 10 && same_nocase(base + 6, ".dat")) {
        /* a costume: 20XX's of the set chosen for it */
        u32 on = *melee_wii_mod_value(MOD_COSTUMES);
        for (u32 c = 0; on != 0 && c < s_costume_count; c++) {
            char name[16];
            u32 set = s_costumes[c].chosen;
            if (strncmp(s_costumes[c].name, base, 6) != 0) {
                continue;
            }
            if (set == 0 || set > COSTUME_SETS || !s_costumes[c].set[set - 1].ok) {
                return -1;
            }
            snprintf(name, sizeof(name), "/%.6s%s", s_costumes[c].name, SET_EXT[set - 1]);
            OSReport("20XX costumes: %s in place of %s\n", name + 1, base);
            return melee_wii_disc2_entry(name);
        }
        return -1;
    }
    if ((base[0] != 'G' && base[0] != 'g') || (base[1] != 'r' && base[1] != 'R')) {
        return -1;
    }
    for (u32 s = 0; s < MOD_SKINS; s++) {
        char name[20];
        u32 look;
        int entry;
        if (!same_nocase(base, STAGES[s].original)) {
            continue;
        }
        look = *melee_wii_mod_value(MOD_SKINS_ADDR + s * 4);
        if (look == LOOK_RANDOM) {
            look = s_random[s];
        }
        if (look == 0 || look >= LOOKS || !s_looks[s][look].ok) {
            return -1;
        }
        name[0] = '/';
        look_name(name + 1, sizeof(name) - 1, s, look);
        entry = melee_wii_disc2_entry(name);
        if (entry >= 0) {
            OSReport("20XX stages: %s in place of %s\n", name + 1, base);
        }
        return entry;
    }
    return -1;
}

/* Final Destination's colour over what was read of one of its files (at
 * `file` in its image) */
static void fd_color(u32 file, u32 offset, void* buf, u32 len)
{
    const u32* color = melee_wii_mod_value(MOD_FD_COLOR); /* blue, green, red, on */
    if (color[3] == 0) {
        return;
    }
    for (u32 m = 0; m < 2; m++) {
        for (u32 i = 0; i < 3; i++) {
            u32 at = file + FD_COLORS[m] + i;
            if (at >= offset && at < offset + len) {
                ((u8*) buf)[at - offset] = (u8) color[2 - i];
            }
        }
    }
}

/* dvd.c, as bytes of an image are read: the original's song rows over a
 * look's own, and Final Destination's colour */
static void after_read(int second, u32 offset, void* buf, u32 len)
{
    if (!second) {
        if (s_fd_size == FD_FILE_SIZE && offset < s_fd_offset + s_fd_size && offset + len > s_fd_offset) {
            fd_color(s_fd_offset, offset, buf, len);
        }
        return;
    }
    for (u32 s = 0; s < MOD_SKINS; s++) {
        for (u32 k = 1; k < LOOKS; k++) {
            const Look* look = &s_looks[s][k];
            if (!look->ok || offset >= look->offset + look->size || offset + len <= look->offset)
            {
                continue;
            }
            for (u32 p = 0; p < look->patch_count; p++) {
                u32 at = look->offset + look->patches[p].offset;
                for (u32 i = 0; i < STAGE_PATCH_BYTES; i++) {
                    if (at + i >= offset && at + i < offset + len) {
                        ((u8*) buf)[at + i - offset] = look->patches[p].bytes[i];
                    }
                }
            }
            if (s == STAGE_FD && look->size == FD_FILE_SIZE) {
                fd_color(look->offset, offset, buf, len);
            }
        }
    }
}

/* mod_values.c, once the image is open: which looks there are, and their
 * verdicts, from the file or made now */
void melee_wii_stages_start(void)
{
    int todo = 0, done = 0, ok = 0, files = 0;
    memset(s_looks, 0, sizeof(s_looks));
    for (u32 s = 0; s < MOD_SKINS; s++) {
        for (u32 k = 1; k < LOOKS; k++) {
            char name[20];
            name[0] = '/';
            look_name(name + 1, sizeof(name) - 1, s, k);
            if (melee_wii_disc2_file(name, &s_looks[s][k].offset, &s_looks[s][k].size) < 0) {
                s_looks[s][k].size = 0;
            }
            files += s_looks[s][k].size != 0;
        }
    }
    s_costume_count = 0;
    melee_wii_disc2_each(costume_file);
    read_verdicts();
    for (u32 s = 0; s < MOD_SKINS; s++) {
        for (u32 k = 1; k < LOOKS; k++) {
            todo += s_looks[s][k].size != 0 && !s_looks[s][k].known;
        }
    }
    for (u32 c = 0; c < s_costume_count; c++) {
        for (u32 k = 0; k < COSTUME_SETS; k++) {
            todo += s_costumes[c].set[k].size != 0 && !s_costumes[c].set[k].known;
        }
    }
    if (todo != 0) {
        OSTime start = OSGetTime();
        for (u32 s = 0; s < MOD_SKINS; s++) {
            check_stage(s, &done, todo);
        }
        for (u32 c = 0; c < s_costume_count; c++) {
            check_costume(&s_costumes[c], &done, todo);
        }
        write_verdicts();
        OSReport("20XX stages: %d files checked in %u ms\n", todo,
                 (unsigned) OSTicksToMilliseconds(OSGetTime() - start));
    }
    for (u32 s = 0; s < MOD_SKINS; s++) {
        for (u32 k = 1; k < LOOKS; k++) {
            ok += s_looks[s][k].ok;
        }
    }
    roll();
    if (melee_wii_disc_file(STAGES[STAGE_FD].original, &s_fd_offset, &s_fd_size) < 0) {
        s_fd_size = 0;
    }
    melee_wii_disc_after_read(after_read);
    OSReport("20XX stages: %d of the image's %d looks can be played\n", ok, files);
    ok = files = 0;
    for (u32 c = 0; c < s_costume_count; c++) {
        for (u32 k = 0; k < COSTUME_SETS; k++) {
            files += s_costumes[c].set[k].size != 0;
            ok += s_costumes[c].set[k].ok;
        }
    }
    OSReport("20XX costumes: %d of the image's %d can be played\n", ok, files);
    portraits_start();
}

#endif
