/* The savestates' memory besides the game's data, bss and the match's heap
 * (so_rollback.c): what Slippi keeps as its "unknown region" (0x8065C000-
 * 0x8071B000 on the GameCube). In the game's arena that is:
 *
 *   - OSAlloc's heap table (HeapArray: each heap's free and allocated lists),
 *     which OSInitAlloc puts just below the audio heap. A heap's contents
 *     rolled back without its table leave the two at odds (the next
 *     allocation fails: HSD_MemAlloc's assertion);
 *   - HSD's audio heap, the Seq heap and the start of the Stay heap (its
 *     memory manager's records): 0xBE3C0 bytes from the audio heap's start,
 *     as on the GameCube;
 *   - what lies below the audio heap in Slippi's region: online, Slippi's
 *     codeset (loaded before the heaps are made), with the data its codes
 *     keep in it.
 *
 * The port's arena starts elsewhere (and online, Slippi's codeset comes
 * before these), so they are found from the game's records: the table from
 * OSAlloc's own variables, the audio heap from its creation (wii/src/os.c). */

#include "so_rollback.h"

#define SLIPPI_REGION_LO 0x8065C000u /* Slippi's "unknown region" (its savestates' third range) */
#define SPAN 0xBE3C0u /* 0x8071B000 - 0x8065CC40, the GameCube's audio heap start */
#define HEAP_ARRAY 0x804D7360u /* OSAlloc.c's HeapArray (.sbss) */
#define NUM_HEAPS 0x804D7364u  /* and NumHeaps */
#define HEAP_DESC_SIZE 12u    /* size, free, allocated */

int melee_wii_audio_heap(unsigned int* lo, unsigned int* hi); /* wii/src/os.c */

void so_rollback_extra_ranges(void (*add)(u32 addr, u32 len))
{
    unsigned int lo, hi;
    u32 table = *(volatile u32*) HEAP_ARRAY;
    s32 count = *(volatile s32*) NUM_HEAPS;
    if (table >= 0x80000000u && table < 0x81800000u && count > 0 && count <= 64) {
        add(table, (u32) count * HEAP_DESC_SIZE);
    }
    if (melee_wii_audio_heap(&lo, &hi) == 0 && hi > lo) {
        add(lo, SPAN);
        /* Slippi's region starts below the audio heap, at 0x8065C000: online,
         * Slippi's codeset is there (0x8065CC80), loaded before the heaps are
         * made, and Dolphin rolls it back with the rest (the codes keep data
         * of their own in it). The same here, up to the heap table or the
         * audio heap, above the GPU's FIFO (which ends in it on the Wii: a
         * load must never write over commands the GPU has yet to read). */
        {
            volatile const u32* pi = (volatile const u32*) 0xCC003000;
            u32 fifo_base = (pi[3] & 0x1FFFFFE0u) | 0x80000000u, fifo_end = (pi[4] & 0x1FFFFFE0u) | 0x80000000u;
            u32 gap_lo = SLIPPI_REGION_LO, gap_hi = lo;
            if (fifo_end > gap_lo && fifo_base < lo) {
                gap_lo = (fifo_end + 32 + 31) & ~31u; /* (the end register holds the last 32 bytes' address) */
            }
            if (table == gap_lo) {
                gap_lo = (table + (u32) count * HEAP_DESC_SIZE + 31) & ~31u; /* (the table is a range of its own) */
            } else if (table > gap_lo && table < gap_hi) {
                gap_hi = table;
            }
            if (gap_hi > gap_lo && gap_hi - gap_lo < 0x200000u) {
                add(gap_lo, gap_hi - gap_lo);
            }
        }
    }
}
