/* Modded Melee on its own disc image (20XX Hack Pack and the like), apart
 * from Slippi: the images on the SD card or a USB drive that are Melee 1.02
 * (GALE01, revision 2) with another executable than the game's. The port
 * plays one by loading its executable over the game's
 * (melee_wii_image_load_dol, main.c) and its files from its image.
 *
 * Where they are looked for: /games, /apps/melee and the root of the SD card
 * and of a USB drive (when one is mounted), files ending in .iso or .gcm. Only
 * each image's header and its executable's are read. */

#include "mods.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wl/platform/platform.h>

static const char* const DIRS[] = { "sd:/games", "sd:/apps/melee", "sd:/", "usb:/games", "usb:/apps/melee", "usb:/" };

/* The 1.02 executable's header (Start.dol: its sections' offsets,
 * addresses and sizes) and the disc's name: a mod changes one or the other
 * (20XX adds to the executable and names its disc) */
static const u8 VANILLA_DOL_HEADER[0x100] = {
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x25, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3B, 0x3E, 0x20,
    0x00, 0x3B, 0x3F, 0xC0, 0x00, 0x3B, 0x42, 0x40, 0x00, 0x3B, 0x42, 0x60, 0x00, 0x3B, 0x42, 0x80,
    0x00, 0x3B, 0x68, 0x40, 0x00, 0x42, 0xE6, 0xC0, 0x00, 0x43, 0x13, 0xC0, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x31, 0x00, 0x80, 0x00, 0x59, 0x40,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x55, 0x20, 0x80, 0x00, 0x56, 0xC0, 0x80, 0x3B, 0x72, 0x40,
    0x80, 0x3B, 0x72, 0x60, 0x80, 0x3B, 0x72, 0x80, 0x80, 0x3B, 0x98, 0x40, 0x80, 0x4D, 0x36, 0xA0,
    0x80, 0x4D, 0x79, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x24, 0x20, 0x00, 0x3B, 0x19, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xA0,
    0x00, 0x00, 0x02, 0x80, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x25, 0xC0,
    0x00, 0x07, 0x7E, 0x80, 0x00, 0x00, 0x2D, 0x00, 0x00, 0x00, 0x72, 0x20, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x43, 0x16, 0xC0, 0x00, 0x0A, 0x63, 0x09,
    0x80, 0x00, 0x52, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
#define VANILLA_TITLE "Super Smash Bros Melee"

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static int has_suffix(const char* name, const char* suffix)
{
    size_t n = strlen(name), m = strlen(suffix);
    if (n < m) {
        return 0;
    }
    for (size_t i = 0; i < m; i++) {
        if (tolower((unsigned char) name[n - m + i]) != suffix[i]) {
            return 0;
        }
    }
    return 1;
}

static int contains_nocase(const char* s, const char* what)
{
    size_t m = strlen(what);
    for (; *s; s++) {
        size_t i = 0;
        while (i < m && s[i] && tolower((unsigned char) s[i]) == tolower((unsigned char) what[i])) {
            i++;
        }
        if (i == m) {
            return 1;
        }
    }
    return 0;
}

/* The executable's size, from its header (the end of its furthest section) */
static u32 dol_size(const u8* hdr)
{
    u32 end = 0x100;
    for (int i = 0; i < 18; i++) {
        u32 off = be32(hdr + i * 4), len = be32(hdr + 0x90 + i * 4);
        if (len != 0 && off + len > end) {
            end = off + len;
        }
    }
    return end;
}

/* Is it Melee 1.02 with another executable? Fills `m`; 1 if so */
static int examine(const char* path, ModDisc* m)
{
    u8 hdr[0x440];
    u8 dol[0x100];
    FILE* f = fopen(path, "rb");
    int ok = 0;
    if (f == NULL) {
        return 0;
    }
    if (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && memcmp(hdr, "GALE01", 6) == 0 && hdr[7] == 2) {
        u32 dol_off = be32(hdr + 0x420);
        if (fseek(f, (long) dol_off, SEEK_SET) == 0 && fread(dol, 1, sizeof(dol), f) == sizeof(dol)) {
            u32 size = dol_size(dol);
            m->dol_offset = dol_off;
            m->dol_size = size;
            /* a vanilla image is the plain game: not a mod */
            ok = memcmp(dol, VANILLA_DOL_HEADER, sizeof(dol)) != 0 ||
                 strncmp((const char*) hdr + 0x20, VANILLA_TITLE, 0x40) != 0;
            snprintf(m->path, sizeof(m->path), "%s", path);
            snprintf(m->title, sizeof(m->title), "%.60s", (const char*) hdr + 0x20);
            m->is_20xx = contains_nocase(m->title, "20XX") || contains_nocase(path, "20XX");
        }
    }
    fclose(f);
    return ok;
}

int mods_scan(ModDisc* out, int max)
{
    int n = 0;
    for (size_t d = 0; d < sizeof(DIRS) / sizeof(DIRS[0]) && n < max; d++) {
        DIR* dir;
        struct dirent* e;
        if (strncmp(DIRS[d], "usb:", 4) == 0 && !wl_platform_fs_mounted(WL_DEVICE_USB)) {
            continue;
        }
        dir = opendir(DIRS[d]);
        if (dir == NULL) {
            continue;
        }
        while ((e = readdir(dir)) != NULL && n < max) {
            char path[160];
            if (!has_suffix(e->d_name, ".iso") && !has_suffix(e->d_name, ".gcm")) {
                continue;
            }
            const char* sep = DIRS[d][strlen(DIRS[d]) - 1] == '/' ? "" : "/";
            if (snprintf(path, sizeof(path), "%s%s%s", DIRS[d], sep, e->d_name) >= (int) sizeof(path)) {
                continue; /* too long a name */
            }
            if (examine(path, &out[n])) {
                n++;
            }
        }
        closedir(dir);
    }
    return n;
}

/* wii/src/dvd.c, main.c, card.c */
int melee_wii_disc_use(const char* path);
int melee_wii_disc_read(unsigned int offset, void* buf, unsigned int len);
int melee_wii_image_load_dol(const u8* dol, u32 size);
void melee_wii_card_dir(const char* dir);

int mods_start(const ModDisc* m, char* error, int error_size)
{
    u8* dol;
    int res;
    if (melee_wii_disc_use(m->path) < 0) {
        snprintf(error, error_size, "%s could not be opened.", m->path);
        return -1;
    }
    dol = memalign(32, (m->dol_size + 31) & ~31u);
    if (dol == NULL) {
        snprintf(error, error_size, "No memory for the executable (%u bytes).", (unsigned) m->dol_size);
        return -2;
    }
    if (melee_wii_disc_read(m->dol_offset, dol, m->dol_size) < 0) {
        free(dol);
        snprintf(error, error_size, "The executable could not be read from %s.", m->path);
        return -3;
    }
    res = melee_wii_image_load_dol(dol, m->dol_size);
    free(dol);
    if (res < 0) {
        snprintf(error, error_size, "%s's executable doesn't fit the game's (%d): is it a 1.02 mod?", m->path,
                 res);
        return -4;
    }
    /* its saves apart from Melee's (20XX keeps its settings in the save) */
    melee_wii_card_dir(m->is_20xx ? "sd:/apps/melee/card20xx" : "sd:/apps/melee/cardmod");
    return 0;
}
