/* Is a modded stage file the original stage with another look?
 * (stage_check.c) */
#ifndef MELEE_WII_STAGE_CHECK_H
#define MELEE_WII_STAGE_CHECK_H

/* The songs of one of the stage's rows, where the modded file has others:
 * the original's bytes, and where they go in the modded file */
#define STAGE_PATCH_BYTES 20
typedef struct StagePatch {
    unsigned int offset;
    unsigned char bytes[STAGE_PATCH_BYTES];
} StagePatch;

/* 0 when `modded` is `original` with another look (then *patch_count
 * patches, at most patch_max); -1 when not, with *why saying what differs.
 * Both are whole files in memory. */
int stage_check(const unsigned char* original, unsigned int original_size, const unsigned char* modded,
                unsigned int modded_size, StagePatch* patches, int patch_max, int* patch_count, const char** why);

#endif
