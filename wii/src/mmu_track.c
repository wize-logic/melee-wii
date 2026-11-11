/* Which 4 KiB pages of MEM1 the game writes (for rollback's savestates,
 * wii/src/slippi/so_rollback.c), from the CPU's own page table.
 *
 * libogc maps MEM1 with a block address translation (DBAT0: 0x80000000, 256
 * MiB), which keeps no record of writes. While tracking, DBAT0 covers only
 * the first 2 MiB (most of the game's code), and the rest of MEM1
 * (0x80200000-0x817FFFFF, the game's data from 0x803B7240 on with it: copied
 * whole each savestate when it was in the block) is mapped page by page
 * through a hashed page
 * table, to the same memory, cached as before. The PowerPC 750 sets a page's
 * Changed bit in its table entry on the first store to it; tracking reads
 * those bits and clears them (and the TLB's copies). Instruction fetches keep
 * IBAT0; DMA (GX, the DSP, the disc's reads into memory) works on physical
 * addresses and is untouched, but it sets no Changed bit: such writes are
 * marked by their callers (melee_wii_mmu_touch).
 *
 * The table is 64 KiB (1024 groups of 8 entries) for the 5632 pages: each
 * page goes in its primary group (or its secondary), filled in once.
 *
 * The table is read and written through the cache, each line flushed
 * (dcbf) around it: right whether the CPU's table search updates the
 * Changed bits in the cache or in memory. (Read through the uncached
 * mapping, it missed changes on the console that Dolphin, with no cache,
 * shows: savestates without them, and the game crashed at the first load.) */

#include <malloc.h>
#include <string.h>

#include <gccore.h>

#include <wl/base/base.h>

#include "melee_wii.h"

#define TRACK_LO 0x80200000u
#define TRACK_HI 0x81800000u
#define PAGES ((TRACK_HI - TRACK_LO) >> 12)
#define HTAB_SIZE 0x10000u
#define VSID 0x5A5u /* segment 8's (0x8xxxxxxx), any value */
#define DBAT0U_2MB (0x80000000u | (0x0Fu << 2) | 3u) /* BEPI 0x80000000, BL 2 MiB, Vs, Vp */

static u32* s_htab;      /* 64 KiB, its own alignment */
static volatile u32* s_pte[PAGES]; /* each page's entry (its first word) */
static u32 s_dbat0u, s_dbat0l, s_sr8;
#define GPU_RANGES 16
static struct {
    u32 addr, len;
} s_gpu[GPU_RANGES];
static u32 s_gpu_count, s_gpu_next;
static int s_on;

static u32 mfspr_dbat0u(void)
{
    u32 v;
    __asm__ volatile("mfspr %0, 536" : "=r"(v));
    return v;
}

static u32 mfspr_dbat0l(void)
{
    u32 v;
    __asm__ volatile("mfspr %0, 537" : "=r"(v));
    return v;
}

static void tlb_flush_all(void)
{
    /* tlbie with each of the 64 congruence classes (EA bits 14-19) clears
     * both ways of each: the whole TLB */
    for (u32 ea = 0; ea < 64 * 0x1000; ea += 0x1000) {
        __asm__ volatile("tlbie %0" ::"r"(ea) : "memory");
    }
    __asm__ volatile("sync" ::: "memory");
}

/* An entry for the page at `ea` (to physical `pa`) in its primary group, or
 * its secondary when full; 0, or -1 when both are */
static int insert(u32 ea, u32 pa, int idx)
{
    u32 pageidx = (ea >> 12) & 0xFFFF;
    u32 hash = (VSID & 0x7FFFF) ^ pageidx;
    u32 api = (ea >> 22) & 0x3F;
    for (int h = 0; h < 2; h++) {
        u32 group = ((h ? ~hash : hash) & 0x3FF) << 6; /* 1024 groups of 64 bytes */
        u32* pteg = (u32*) ((u8*) s_htab + group);
        for (int k = 0; k < 8; k++) {
            u32* pte = pteg + k * 2;
            if (pte[0] & 0x80000000u) {
                continue;
            }
            /* word 1: the page, R and C clear, WIMG 0 (cached), PP 2 (read/write) */
            pte[1] = (pa & 0xFFFFF000u) | 0x2;
            __asm__ volatile("eieio" ::: "memory");
            /* word 0: valid, VSID, H, API */
            pte[0] = 0x80000000u | (VSID << 7) | ((u32) h << 6) | api;
            s_pte[idx] = pte;
            return 0;
        }
    }
    return -1;
}

int melee_wii_mmu_track_start(void)
{
    u32 level, htab_pa;
    if (s_on) {
        return 0;
    }
    if (s_htab == NULL) {
        s_htab = memalign(HTAB_SIZE, HTAB_SIZE);
        if (s_htab == NULL) {
            return -1;
        }
    }
    memset(s_htab, 0, HTAB_SIZE);
    s_gpu_count = s_gpu_next = 0;
    for (u32 i = 0; i < PAGES; i++) {
        u32 ea = TRACK_LO + (i << 12);
        if (insert(ea, ea & 0x3FFFFFFFu, (int) i) < 0) {
            WL_LOG_E("mmu", "no room in the page table for %08x", (unsigned) ea);
            return -2;
        }
    }
    DCFlushRange(s_htab, HTAB_SIZE);
    htab_pa = (u32) s_htab & 0x3FFFFFFFu;

    level = IRQ_Disable();
    s_dbat0u = mfspr_dbat0u();
    s_dbat0l = mfspr_dbat0l();
    __asm__ volatile("mfsr %0, 8" : "=r"(s_sr8));
    __asm__ volatile("sync\n\tmtspr 25, %0\n\tisync" ::"r"(htab_pa & 0xFFFF0000u) : "memory"); /* SDR1, mask 0 */
    __asm__ volatile("mtsr 8, %0\n\tisync" ::"r"(VSID) : "memory");
    tlb_flush_all();
    /* DBAT0 down to the first 2 MiB: the rest of MEM1 goes by the table */
    __asm__ volatile("sync\n\tmtspr 536, %0\n\tisync" ::"r"(DBAT0U_2MB) : "memory");
    s_on = 1;
    IRQ_Restore(level);
    WL_LOG_I("mmu", "tracking MEM1 %08x-%08x by page (DBAT0 was %08x/%08x, SR8 %08x)", TRACK_LO, TRACK_HI,
             (unsigned) s_dbat0u, (unsigned) s_dbat0l, (unsigned) s_sr8);
    return 0;
}

void melee_wii_mmu_track_stop(void)
{
    u32 level;
    if (!s_on) {
        return;
    }
    level = IRQ_Disable();
    __asm__ volatile("sync\n\tmtspr 536, %0\n\tisync" ::"r"(s_dbat0u) : "memory");
    __asm__ volatile("mtsr 8, %0\n\tisync" ::"r"(s_sr8) : "memory");
    tlb_flush_all();
    s_on = 0;
    IRQ_Restore(level);
}

static void line_flush(volatile const void* p)
{
    __asm__ volatile("dcbf 0, %0" ::"r"(p) : "memory");
}

/* Before reading the Changed bits: no line of the table left in the cache
 * (the memory then has the table search's updates, wherever it made them) */
void melee_wii_mmu_refresh(void)
{
    if (!s_on) {
        return;
    }
    for (u32 o = 0; o < HTAB_SIZE; o += 32) {
        line_flush((const u8*) s_htab + o);
    }
    __asm__ volatile("sync" ::: "memory");
}

int melee_wii_mmu_tracking(void)
{
    return s_on;
}

/* Is the page at `ea` in the tracked part of MEM1 (while tracking)? */
int melee_wii_mmu_tracks(u32 ea)
{
    return s_on && ea >= TRACK_LO && ea < TRACK_HI;
}

/* Is the page at `ea` changed since its bit was last cleared? (1 also for a
 * page out of the tracked range: no record, so changed.) After
 * melee_wii_mmu_refresh, with nothing stored meanwhile. */
int melee_wii_mmu_page_changed(u32 ea)
{
    if (!s_on || ea < TRACK_LO || ea >= TRACK_HI) {
        return 1;
    }
    return (s_pte[(ea - TRACK_LO) >> 12][1] & 0x80) != 0;
}

/* The Changed bits of the pages over [lo, hi) cleared, the table's words
 * written out; the TLB not yet (tlb_after). Counts them in *cleared, the
 * first in *first. */
static void clear_bits(u32 lo, u32 hi, u32* cleared, u32* first)
{
    if (lo < TRACK_LO) {
        lo = TRACK_LO;
    }
    if (hi > TRACK_HI) {
        hi = TRACK_HI;
    }
    for (u32 ea = lo & ~0xFFFu; ea < hi; ea += 0x1000) {
        volatile u32* pte = s_pte[(ea - TRACK_LO) >> 12];
        u32 w = pte[1];
        if (w & 0x80) {
            pte[1] = w & ~0x80u;
            line_flush(pte);
            if ((*cleared)++ == 0) {
                *first = ea;
            }
        }
    }
}

/* Then the TLB's copies, which would let stores on without setting the bits
 * again: one page's entry, or all 64 classes at once (a sync and a tlbie for
 * each page took milliseconds a savestate) */
static void tlb_after(u32 cleared, u32 first)
{
    if (cleared == 0) {
        return;
    }
    __asm__ volatile("sync" ::: "memory");
    if (cleared == 1) {
        __asm__ volatile("tlbie %0\n\tsync" ::"r"(first) : "memory");
    } else {
        tlb_flush_all();
    }
}

/* Clears the Changed bits of the pages over [lo, hi): stores to them are
 * recorded again from now. After melee_wii_mmu_refresh. */
void melee_wii_mmu_clear(u32 lo, u32 hi)
{
    u32 cleared = 0, first = 0;
    if (!s_on) {
        return;
    }
    clear_bits(lo, hi, &cleared, &first);
    tlb_after(cleared, first);
}

/* The same over `n` ranges (address and length pairs), the TLB once */
void melee_wii_mmu_clear_ranges(const u32* pairs, int n)
{
    u32 cleared = 0, first = 0;
    if (!s_on) {
        return;
    }
    for (int i = 0; i < n; i++) {
        clear_bits(pairs[2 * i], pairs[2 * i] + pairs[2 * i + 1], &cleared, &first);
    }
    tlb_after(cleared, first);
}

/* Writes the CPU didn't make (DMA into memory): marks the pages changed */
void melee_wii_mmu_touch(unsigned int addr, unsigned int len)
{
    addr |= 0x80000000u;
    addr &= 0x8FFFFFFFu;
    if (!s_on || len == 0) {
        return;
    }
    u32 level = IRQ_Disable();
    for (u32 ea = addr & ~0xFFFu; ea < addr + len && ea < TRACK_HI; ea += 0x1000) {
        if (ea >= TRACK_LO) {
            volatile u32* pte = s_pte[(ea - TRACK_LO) >> 12];
            line_flush(pte);
            __asm__ volatile("sync" ::: "memory");
            pte[1] |= 0x80;
            line_flush(pte);
        }
    }
    __asm__ volatile("sync" ::: "memory");
    IRQ_Restore(level);
}

/* Memory the GPU writes (texture copies: the characters' shadows): noted for
 * rollback's log, not marked: the savestates leave them out (drawn again
 * each frame, and read by nothing but the GPU) */

void melee_wii_mmu_touch_gpu(unsigned int addr, unsigned int len)
{
    u32 level;
    if (!s_on || len == 0) {
        return;
    }
    level = IRQ_Disable();
    for (u32 i = 0; i < s_gpu_count; i++) {
        if (s_gpu[i].addr == addr && s_gpu[i].len >= len) {
            IRQ_Restore(level);
            return;
        }
    }
    s_gpu[s_gpu_next].addr = addr;
    s_gpu[s_gpu_next].len = len;
    s_gpu_next = (s_gpu_next + 1) % GPU_RANGES;
    if (s_gpu_count < GPU_RANGES) {
        s_gpu_count++;
    }
    IRQ_Restore(level);
}

/* Is the page at `ea` one a texture copy writes? (rollback's statistics) */
int melee_wii_mmu_gpu_page(unsigned int ea)
{
    for (u32 i = 0; i < s_gpu_count; i++) {
        if (ea < s_gpu[i].addr + s_gpu[i].len && (s_gpu[i].addr & ~0xFFFu) <= ea) {
            return 1;
        }
    }
    return 0;
}

/* The texture copies' memory, for the log */
int melee_wii_mmu_gpu_ranges(unsigned int* addr, unsigned int* len, int max)
{
    int n = 0;
    for (u32 i = 0; i < s_gpu_count && n < max; i++, n++) {
        addr[n] = s_gpu[i].addr;
        len[n] = s_gpu[i].len;
    }
    return n;
}

/* A check of the tracking (main.c, when sd:/apps/melee/mmutest exists): a
 * page written shows changed, a cleared one not, the memory still reads and
 * writes through the table; then tracking off. Logs the outcome. */
void melee_wii_mmu_selftest(void)
{
    volatile u32* p = (volatile u32*) 0x81000040u; /* the game's arena, not in use yet */
    u32 before, ok = 1;
    if (melee_wii_mmu_track_start() < 0) {
        WL_LOG_E("mmu", "selftest: no tracking");
        return;
    }
    melee_wii_mmu_refresh();
    melee_wii_mmu_clear(0x81000000u, 0x81002000u);
    ok &= !melee_wii_mmu_page_changed(0x81000000u);
    before = p[0];
    p[0] = 0x12345678;
    __asm__ volatile("sync" ::: "memory");
    ok &= p[0] == 0x12345678;
    melee_wii_mmu_refresh();
    ok &= melee_wii_mmu_page_changed(0x81000000u);
    ok &= !melee_wii_mmu_page_changed(0x81001000u);
    melee_wii_mmu_clear(0x81000000u, 0x81002000u);
    ok &= !melee_wii_mmu_page_changed(0x81000000u);
    p[0] = before;
    __asm__ volatile("sync" ::: "memory");
    melee_wii_mmu_refresh();
    ok &= melee_wii_mmu_page_changed(0x81000000u);
    /* the uncached mirror sees the same memory */
    ok &= *(volatile u32*) 0xC1000040u == before;
    melee_wii_mmu_track_stop();
    WL_LOG_I("mmu", "selftest: %s", ok ? "PASS" : "FAIL");
}

/* ---- a test: which pages an interrupt's handler writes (IRQ_AUDIT in
 * os_irq.c): the Changed bits before and after it, the pages that became
 * changed meanwhile. In the handler, interrupts off. */
static u32 s_audit_bits[PAGES / 32];

void melee_wii_mmu_audit_begin(void)
{
    if (!s_on) {
        return;
    }
    melee_wii_mmu_refresh();
    for (u32 i = 0; i < PAGES; i++) {
        if (i % 32 == 0) {
            s_audit_bits[i / 32] = 0;
        }
        if (s_pte[i][1] & 0x80) {
            s_audit_bits[i / 32] |= 1u << (i % 32);
        }
    }
}

int melee_wii_mmu_audit_end(u32* out, int max)
{
    int n = 0;
    if (!s_on) {
        return 0;
    }
    melee_wii_mmu_refresh();
    for (u32 i = 0; i < PAGES && n < max; i++) {
        if ((s_pte[i][1] & 0x80) && !(s_audit_bits[i / 32] & (1u << (i % 32)))) {
            out[n++] = TRACK_LO + (i << 12);
        }
    }
    return n;
}
