/* A measurement for rollback (test builds only, -DMELEE_DIRTY_DIAG): how much
 * of the memory Slippi's savestates copy changes from frame to frame in a VS
 * match. Every engine tick (in the game's loop) it hashes Slippi's regions in 256-byte
 * blocks and logs, every 10 seconds of a match: the blocks changed per frame
 * (mean, most), the most changed over any 8 frames (a rollback's window), and
 * the blocks changed at all since the match started, per region. */

#ifdef MELEE_DIRTY_DIAG

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include <wl/base/base.h>

#include "melee_wii.h"

#define BLOCK 256u
#define WINDOW 8

typedef struct {
    const char* name;
    u32 lo, hi;
} Region;

#define REGIONS 5
static Region s_regions[REGIONS] = {
    { "sections 0-1", 0x80005520, 0x80005940 },
    { "data and bss", 0x803B7240, 0x804DEC00 },
    { "arena below the heap", 0, 0 },
    { "heap", 0, 0 },
    { "arena above the heap", 0, 0x81800000 },
};
extern char __Arena1Lo[];
static u8* s_file;        /* each block: 1 when a disc/ARAM read filled it */

static u32* s_hash;       /* each block's last hash */
static u32* s_changed_at; /* the frame it last changed (0: never this match) */
static u32 s_blocks;
static u32 s_frame, s_in_match;
static u32 s_sum, s_most, s_most_window;

static u32 hash_block(const u32* p)
{
    u32 h = 0x9E3779B9;
    for (u32 i = 0; i < BLOCK / 4; i += 4) {
        h = (h ^ p[i]) * 0x01000193u;
        h = (h ^ p[i + 1]) * 0x01000193u;
        h = (h ^ p[i + 2]) * 0x01000193u;
        h = (h ^ p[i + 3]) * 0x01000193u;
    }
    return h;
}

/* The runs of changed blocks of region `r` (gaps under 4 KB joined) */
static void runs(int r, u32 base)
{
    u32 n = (s_regions[r].hi - s_regions[r].lo) / BLOCK, start = 0, last = 0, have = 0, shown = 0;
    for (u32 i = 0; i <= n && shown < 24; i++) {
        int changed = i < n && s_changed_at[base + i] != 0;
        if (changed) {
            if (!have) {
                start = i;
                have = 1;
            }
            last = i;
        } else if (have && (i == n || i - last > 4096 / BLOCK)) {
            WL_LOG_I("dirty", "    run %08x-%08x (%u KB)", (unsigned) (s_regions[r].lo + start * BLOCK),
                     (unsigned) (s_regions[r].lo + (last + 1) * BLOCK), (unsigned) ((last + 1 - start) * BLOCK / 1024));
            have = 0;
            shown++;
        }
    }
}

static void layout(void)
{
    const u32* fb = (const u32*) 0x804C0948;
    const u8* h = (const u8*) 0x80431FA0;
    unsigned int alo = 0, ahi = 0;
    melee_wii_audio_heap(&alo, &ahi);
    WL_LOG_I("dirty", "audio heap %08x-%08x; arena start %08x", alo, ahi, (unsigned) __Arena1Lo);
    WL_LOG_I("dirty", "XFBs %08x %08x %08x; lb arena %08x-%08x", (unsigned) fb[0], (unsigned) fb[1], (unsigned) fb[2],
             (unsigned) *(const u32*) h, (unsigned) *(const u32*) (h + 4));
    for (int k = 0; k < 6; k++) {
        const u8* e = h + 0x10 + k * 0x1C;
        WL_LOG_I("dirty", "lb heap %d: %08x + %08x type %d status %d", k, (unsigned) *(const u32*) (e + 8),
                 (unsigned) *(const u32*) (e + 12), (int) *(const s32*) (e + 16), (int) *(const s32*) (e + 24));
    }
}

static void report(void)
{
    u32 b = 0;
    WL_LOG_I("dirty", "%u frames: per frame mean %u KB, most %u KB; most in %d frames %u KB", (unsigned) s_frame,
             (unsigned) (s_sum / (s_frame ? s_frame : 1) * BLOCK / 1024), (unsigned) (s_most * BLOCK / 1024),
             WINDOW, (unsigned) (s_most_window * BLOCK / 1024));
    for (int r = 0; r < REGIONS; r++) {
        u32 n = (s_regions[r].hi - s_regions[r].lo) / BLOCK, ever = 0, file = 0, file_changed = 0;
        for (u32 i = 0; i < n; i++) {
            ever += s_changed_at[b + i] != 0;
            file += s_file[b + i];
            file_changed += s_file[b + i] && s_changed_at[b + i] != 0;
        }
        WL_LOG_I("dirty", "  %s %08x-%08x: %u of %u KB changed; file data %u KB, of it changed %u KB",
                 s_regions[r].name, (unsigned) s_regions[r].lo, (unsigned) s_regions[r].hi,
                 (unsigned) (ever * BLOCK / 1024), (unsigned) (n * BLOCK / 1024), (unsigned) (file * BLOCK / 1024),
                 (unsigned) (file_changed * BLOCK / 1024));
        if (r >= 2) {
            runs(r, b);
        }
        b += n;
    }
}

static void tick(void);

/* The engine tick's start (0x801A4DE4, where Slippi's savestates are taken):
 * a block in the island that saves the volatile registers, calls tick(),
 * restores them, runs the instruction it replaced and goes back */
#define TICK_SITE 0x801A4DE4u
void* melee_wii_island_alloc(unsigned int size);
void melee_wii_put_branch(unsigned int site, void* target, unsigned int link);

static void install(void)
{
    static const u32 save[] = {
        0x9421FF70, /* stwu r1,-0x90(r1) */
        0x90010010, 0x90610014, 0x90810018, 0x90A1001C, 0x90C10020, 0x90E10024, 0x91010028,
        0x9121002C, 0x91410030, 0x91610034, 0x91810038, /* stw r0, r3-r12 */
        0x7C0802A6, 0x9001003C, /* mflr r0; stw r0,0x3C */
        0x7C000026, 0x90010040, /* mfcr r0; stw r0,0x40 */
        0x7C0902A6, 0x90010044, /* mfctr r0; stw r0,0x44 */
        0x7C0102A6, 0x90010048, /* mfxer r0; stw r0,0x48 */
    };
    static const u32 restore[] = {
        0x80010048, 0x7C0103A6, 0x80010044, 0x7C0903A6, 0x80010040, 0x7C0FF120,
        0x8001003C, 0x7C0803A6,
        0x80010010, 0x80610014, 0x80810018, 0x80A1001C, 0x80C10020, 0x80E10024, 0x81010028,
        0x8121002C, 0x81410030, 0x81610034, 0x81810038,
        0x38210090, /* addi r1,r1,0x90 */
    };
    u32 n = sizeof(save) / 4 + 4 + sizeof(restore) / 4 + 2, k = 0, t = (u32) tick;
    u32* b = melee_wii_island_alloc(n * 4);
    if (b == NULL) {
        return;
    }
    memcpy(b, save, sizeof(save));
    k = sizeof(save) / 4;
    b[k++] = 0x3D800000 | (((t + 0x8000) >> 16) & 0xFFFF); /* lis r12 */
    b[k++] = 0x398C0000 | (t & 0xFFFF);                    /* addi r12 */
    b[k++] = 0x7D8903A6;                                   /* mtctr r12 */
    b[k++] = 0x4E800421;                                   /* bctrl */
    memcpy(b + k, restore, sizeof(restore));
    k += sizeof(restore) / 4;
    {
        /* the replaced instruction (a relative branch, moved, keeps its target) */
        u32 insn = *(volatile u32*) TICK_SITE;
        if ((insn & 0xFC000002) == 0x48000000) {
            s32 li = (s32) (insn << 6) >> 6 & ~3;
            u32 target = TICK_SITE + (u32) li;
            s32 disp = (s32) (target - (u32) &b[k]);
            insn = (insn & 0xFC000003) | ((u32) disp & 0x03FFFFFC);
        }
        b[k++] = insn;
    }
    {
        s32 disp = (s32) ((TICK_SITE + 4) - (u32) &b[k]);
        b[k++] = 0x48000000 | ((u32) disp & 0x03FFFFFC);
    }
    DCFlushRange(b, n * 4);
    ICInvalidateRange(b, n * 4);
    melee_wii_put_branch(TICK_SITE, b, 0);
    DCFlushRange((void*) (TICK_SITE & ~31u), 32);
    ICInvalidateRange((void*) (TICK_SITE & ~31u), 32);
    WL_LOG_I("dirty", "probe at the engine tick");
}

/* wii/src/vi.c, at each retrace: puts the probe in, once */
void melee_wii_diag_frame(void)
{
    static int done;
    if (!done) {
        done = 1;
        install();
    }
}

void so_rollback_selftest_tick(int tick); /* wii/src/slippi/so_rollback.c */

static void tick(void)
{
    u32 scene = *(volatile u32*) 0x80479D30;
    {
        /* the savestates' check: ticks counted from the match's start */
        static int t;
        int in = ((scene >> 24) == 2) && (scene & 0xFF) == 2;
        t = in ? t + 1 : 0;
        if (in && t <= 13) {
            so_rollback_selftest_tick(t);
        }
        return; /* (no dirty measuring in this build) */
    }
    int in_match = ((scene >> 24) == 2 || (scene >> 24) == 8) && (scene & 0xFF) == 2; /* VS or online, in game */
    if (!in_match) {
        if (s_in_match) {
            report();
            s_in_match = 0;
        }
        return;
    }
    if (!s_in_match) {
        u32 lo = *(volatile u32*) 0x804D76B8, hi = *(volatile u32*) 0x804D76BC;
        s_regions[2].lo = ((u32) __Arena1Lo + BLOCK - 1) & ~(BLOCK - 1);
        s_regions[2].hi = lo & ~(BLOCK - 1);
        s_regions[3].lo = lo & ~(BLOCK - 1);
        s_regions[3].hi = hi & ~(BLOCK - 1);
        s_regions[4].lo = hi & ~(BLOCK - 1);
        s_blocks = 0;
        for (int r = 0; r < REGIONS; r++) {
            s_blocks += (s_regions[r].hi - s_regions[r].lo) / BLOCK;
        }
        free(s_hash);
        free(s_changed_at);
        free(s_file);
        s_hash = malloc(s_blocks * 4);
        s_changed_at = calloc(s_blocks, 4);
        s_file = calloc(s_blocks, 1);
        if (s_hash == NULL || s_changed_at == NULL || s_file == NULL) {
            return;
        }
        {
            /* the blocks the last reads filled */
            const unsigned int *la, *ll;
            u32 count = melee_wii_dvd_loads(&la, &ll), first = melee_wii_scene_first_load();
            if (count - first > 1024) {
                first = count - 1024;
            }
            WL_LOG_I("dirty", "this scene's loads: %u", (unsigned) (count - first));
            for (u32 k = first; k < count; k++) {
                u32 a = la[k % 1024], e = a + ll[k % 1024], base = 0;
                for (int r = 0; r < REGIONS; r++) {
                    u32 rl = s_regions[r].lo, rh = s_regions[r].hi;
                    for (u32 x = (a > rl ? a : rl) & ~(BLOCK - 1); x < e && x < rh; x += BLOCK) {
                        if (x >= rl) {
                            s_file[base + (x - rl) / BLOCK] = 1;
                        }
                    }
                    base += (rh - rl) / BLOCK;
                }
            }
        }
        WL_LOG_I("dirty", "match: heap %08x-%08x, %u blocks", (unsigned) lo, (unsigned) hi, (unsigned) s_blocks);
        layout();
        s_frame = s_sum = s_most = s_most_window = 0;
        s_in_match = 1;
    }
    u32 b = 0, changed = 0, window = 0;
    s_frame++;
    for (int r = 0; r < REGIONS; r++) {
        for (u32 a = s_regions[r].lo; a < s_regions[r].hi; a += BLOCK, b++) {
            u32 h = hash_block((const u32*) a);
            if (s_frame > 1 && h != s_hash[b]) {
                s_changed_at[b] = s_frame;
                changed++;
            }
            s_hash[b] = h;
            if (s_changed_at[b] != 0 && s_frame - s_changed_at[b] < WINDOW) {
                window++;
            }
        }
    }
    if (s_frame > 1) {
        s_sum += changed;
        if (changed > s_most) {
            s_most = changed;
        }
        if (window > s_most_window) {
            s_most_window = window;
        }
    }
    if (s_frame % 600 == 0) {
        report();
    }
}

#endif
