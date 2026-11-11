/* Modded Melee on its own disc image (mods.c): 20XX Hack Pack and the like */
#ifndef LAUNCHER_MODS_H
#define LAUNCHER_MODS_H

#include <gccore.h>
#include <malloc.h>

typedef struct ModDisc {
    char path[160];
    char title[64]; /* the disc's name */
    u32 dol_offset, dol_size;
    int is_20xx;
} ModDisc;

/* The modded images there are (at most `max`) */
int mods_scan(ModDisc* out, int max);

/* Plays this one: its image as the game's disc, its executable in the game's
 * place, its own saves. 0, or < 0 with `error` saying why. Before the game
 * starts; then no Slippi. */
int mods_start(const ModDisc* m, char* error, int error_size);

#endif
