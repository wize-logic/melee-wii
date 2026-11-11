/* Big copies for rollback's savestates (wii/src/slippi/so_rollback.c): by
 * the CPU's DMA engine between memory and the locked cache, or by the CPU.
 *
 * A CPU copy waits out each line's miss in turn (on the console, about 170
 * MB/s from MEM1 into MEM2). The locked cache's DMA moves 4 KiB at a time
 * at the bus's speed: each piece goes into the locked cache and back out to
 * its place, the commands queued in order (a store from a buffer of the
 * locked cache comes after the load into it). The DMA works on whole lines
 * of memory and doesn't see the data cache: the source's lines are written
 * out of it first (dcbst) and the destination's dropped (dcbi); a piece's
 * partial lines at either end are copied by the CPU.
 *
 * melee_wii_copy_check (as the savestates' memory is taken) copies with
 * both, checks the DMA's copy byte for byte and times both, and keeps the
 * faster that is right. */

#include <string.h>

#include <gccore.h>
#include <ogc/cache.h>
#include <ogc/lwp_watchdog.h>

#include <wl/base/base.h>

#include "melee_wii.h"

#define LC_START 0xE0000000u
#define LC_CHUNK 4096u
#define LC_BUFS 4 /* of the locked cache's 16 KiB */

static int s_dma_save; /* copies into MEM2 (savestates) by the DMA */
static int s_dma_load; /* copies out of MEM2 (loads) by the DMA */
static int s_lc_on;  /* the locked cache on (between begin and end) */
static u32 s_next;   /* the locked cache's next buffer */

static u32 hid2(void)
{
    u32 v;
    __asm__ volatile("mfspr %0, 920" : "=r"(v));
    return v;
}

static int s_stuck; /* the DMA's queue once didn't empty: never again (copy_end says so) */

/* Until no more than `n` commands are queued (at most 20 ms: interrupts are
 * off, and a stuck queue must not hang the console) */
static void queue_wait(u32 n)
{
    u64 until = 0;
    while (((hid2() >> 4) & 0xF) > n) {
        u64 now = gettime();
        if (until == 0) {
            until = now + millisecs_to_ticks(20);
        } else if (now > until) {
            s_stuck = 1;
            return;
        }
    }
}

/* A command: `lines` (1-128) between memory at `mem` and the locked cache at
 * `lc`; to the locked cache when `load`. The memory's address is physical:
 * MEM2's 0x9... is 0x1... (libogc's LCLoadBlocks, which keeps 28 bits, has
 * MEM1's addresses in mind) */
static void dma(u32 mem, u32 lc, u32 lines, int load)
{
    u32 n = lines & 0x7F; /* (128 as 0) */
    queue_wait(14);
    __asm__ volatile("mtspr 922, %0" ::"r"((mem & 0x3FFFFFE0u) | (n >> 2)));
    __asm__ volatile("mtspr 923, %0" ::"r"(lc | ((n & 3) << 2) | (load ? 0x12u : 0x02u)) : "memory");
}

/* The CPU's copy: by cache lines, the next ones asked for ahead (dcbt), the
 * destination's lines zeroed in the cache (dcbz) rather than read */
/* How far ahead the copy asks for lines, into MEM2 and out of it (set by
 * melee_wii_copy_check: the fastest of a few on this console) */
static u32 s_ahead_save = 128, s_ahead_load = 128;

static void cpu_copy_ahead(u8* dst, const u8* src, u32 len, u32 ahead);

static void cpu_copy(u8* dst, const u8* src, u32 len)
{
    cpu_copy_ahead(dst, src, len, (u32) dst >= 0x90000000u ? s_ahead_save : s_ahead_load);
}

static void cpu_copy_ahead(u8* dst, const u8* src, u32 len, u32 ahead)
{
    u32 head = (32 - ((u32) dst & 31)) & 31;
    if (head > len) {
        head = len;
    }
    memcpy(dst, src, head);
    dst += head;
    src += head;
    len -= head;
    if (((u32) src & 7) == 0) {
        while (len >= 32) {
            const double* s = (const double*) src;
            double* d = (double*) dst;
            double a, b, c, e;
            __asm__ volatile("dcbt 0, %0" ::"r"(src + ahead));
            a = s[0];
            b = s[1];
            c = s[2];
            e = s[3];
            __asm__ volatile("dcbz 0, %0" ::"r"(dst) : "memory");
            d[0] = a;
            d[1] = b;
            d[2] = c;
            d[3] = e;
            dst += 32;
            src += 32;
            len -= 32;
        }
    }
    memcpy(dst, src, len);
}

/* The whole lines [src, src + len), both 32-byte aligned, by the DMA */
static void dma_lines(u8* dst, const u8* src, u32 len)
{
    while (len > 0) {
        u32 n = len < LC_CHUNK ? len : LC_CHUNK;
        u32 lc = LC_START + (s_next++ % LC_BUFS) * LC_CHUNK;
        for (u32 o = 0; o < n; o += 32) {
            __asm__ volatile("dcbst 0, %0" ::"r"(src + o));
            __asm__ volatile("dcbi 0, %0" ::"r"(dst + o) : "memory");
        }
        __asm__ volatile("sync" ::: "memory");
        dma((u32) src, lc, n / 32, 1);
        dma((u32) dst, lc, n / 32, 0);
        dst += n;
        src += n;
        len -= n;
    }
}

void melee_wii_copy_begin(void)
{
    if ((s_dma_save || s_dma_load) && !s_stuck && !s_lc_on) {
        LCEnable();
        s_lc_on = 1;
    }
}

int melee_wii_copy_end(void)
{
    if (!s_lc_on) {
        return 0;
    }
    /* one line more, after the rest: once it is taken off the queue, all
     * before it are done; then a moment for it */
    dma((u32) &s_next & ~31u, LC_START + LC_BUFS * LC_CHUNK - 32, 1, 1);
    queue_wait(0);
    {
        u64 until = gettime() + microsecs_to_ticks(4);
        while (gettime() < until) {
        }
    }
    LCDisable();
    s_lc_on = 0;
    if (s_stuck) {
        /* the copies since begin may be unfinished: the caller does them
         * again (by the CPU from now on) */
        s_dma_save = s_dma_load = 0;
        return -1;
    }
    return 0;
}

/* [src, src + len) to dst; (dst & 31) == (src & 31). Between begin and end. */
void melee_wii_copy(void* dst, const void* src, u32 len)
{
    u8* d = dst;
    const u8* s = src;
    u32 head, body;
    if (!s_lc_on || len < 256 || !((u32) dst >= 0x90000000u ? s_dma_save : s_dma_load)) {
        cpu_copy(d, s, len);
        return;
    }
    head = (32 - ((u32) s & 31)) & 31;
    body = (len - head) & ~31u;
    memcpy(d, s, head);
    dma_lines(d + head, s + head, body);
    memcpy(d + head + body, s + head + body, len - head - body);
}

/* The locked cache as a window for reading memory (rollback's hashes of
 * the pieces written since the last savestate: read by the CPU a line at a
 * time they took 5.5 ms of a savestate's 7 on a console). Each piece (whole
 * lines, at most 512 bytes) is brought into the next of 16 slots by the
 * DMA, a few pieces ahead of the CPU, which reads it there at the cache's
 * speed. A command off the DMA's queue may still be running: a piece is
 * there once the next command is off it too, or the queue is empty and a
 * moment has passed. Between begin and melee_wii_copy_end (which says
 * whether the DMA got stuck: then the hashes are to be done again by the
 * CPU). */
#define WINDOW_SLOT 512u
#define WINDOW_SLOTS 16u
static u32 s_window_issued;

int melee_wii_window_begin(void)
{
    if (!s_dma_save || s_stuck) {
        return 0;
    }
    melee_wii_copy_begin();
    s_window_issued = 0;
    return s_lc_on;
}

/* Fetch number melee_wii_window_count() - 1: `len` (whole lines) of `src`,
 * to be read at the address returned once melee_wii_window_wait says */
const void* melee_wii_window_fetch(const void* src, u32 len)
{
    u32 lc = LC_START + (s_window_issued++ % WINDOW_SLOTS) * WINDOW_SLOT;
    for (u32 o = 0; o < len; o += 32) {
        __asm__ volatile("dcbst 0, %0" ::"r"((const u8*) src + o));
    }
    __asm__ volatile("sync" ::: "memory");
    dma((u32) src, lc, len / 32, 1);
    return (const void*) lc;
}

/* Until fetch number `index` is in its slot */
void melee_wii_window_wait(u32 index)
{
    u64 until = 0;
    for (;;) {
        u32 queued = (hid2() >> 4) & 0xF;
        u64 now;
        if (s_window_issued - queued >= index + 2) {
            return;
        }
        if (queued == 0) {
            u64 moment = gettime() + microsecs_to_ticks(2);
            while (gettime() < moment) {
            }
            return;
        }
        now = gettime();
        if (until == 0) {
            until = now + millisecs_to_ticks(20);
        } else if (now > until) {
            s_stuck = 1;
            return;
        }
    }
}

/* Bit 0: savestates' copies by the DMA; bit 1: loads' */
int melee_wii_copy_by_dma(void)
{
    return (s_dma_save ? 1 : 0) | (s_dma_load ? 2 : 0);
}

/* Times the copies of `len` of `src` (the game's) into `dst` (scratch, in
 * MEM2) and back (the same bytes over themselves), by the CPU and by the
 * locked cache's DMA, interrupts off, and keeps for each way the faster
 * that is right. On a console a load (out of MEM2) by the CPU took 2.5
 * times a savestate's time (4179 against 1708 us for 512 KB: MEM2's reads),
 * and so most of the 10 ms the audio lost to each rollback. A DMA copy back
 * that came out wrong is put right at once by the CPU, from `dst`. */
static u32 dma_time(u8* to, const u8* from, u32 len)
{
    u64 t;
    s_lc_on = 0;
    LCEnable();
    s_lc_on = 1;
    t = gettime();
    dma_lines(to, from, len);
    melee_wii_copy_end();
    return (u32) ticks_to_microsecs(gettime() - t);
}

void melee_wii_copy_check(void* dst, const void* src, u32 len)
{
    static const u32 aheads[] = { 128, 256 };
    u32 save_us[2], load_us[2], save_dma_us, load_dma_us, level, best_save = 0, best_load = 0;
    int save_ok, load_ok;
    s_dma_save = s_dma_load = 0;
    len &= ~31u;
    level = IRQ_Disable();
    for (u32 i = 0; i < 2; i++) {
        u64 t = gettime();
        cpu_copy_ahead(dst, src, len, aheads[i]);
        save_us[i] = (u32) ticks_to_microsecs(gettime() - t);
        t = gettime();
        cpu_copy_ahead((u8*) src, dst, len, aheads[i]);
        load_us[i] = (u32) ticks_to_microsecs(gettime() - t);
        best_save = save_us[i] < save_us[best_save] ? i : best_save;
        best_load = load_us[i] < load_us[best_load] ? i : best_load;
    }
    s_ahead_save = aheads[best_save];
    s_ahead_load = aheads[best_load];
    /* the DMA: into the scratch (cleared first, to see it arrive), then back */
    memset(dst, 0x5A, len);
    DCFlushRange(dst, len);
    save_dma_us = dma_time(dst, src, len);
    save_ok = !s_stuck && memcmp(dst, src, len) == 0;
    if (!save_ok) {
        cpu_copy(dst, src, len);
    }
    load_dma_us = dma_time((u8*) src, dst, len);
    load_ok = !s_stuck && memcmp(src, dst, len) == 0;
    if (!load_ok) {
        cpu_copy((u8*) src, dst, len);
    }
    IRQ_Restore(level);
    s_dma_save = save_ok && save_dma_us < save_us[best_save];
    s_dma_load = load_ok && load_dma_us < load_us[best_load];
    WL_LOG_I("copy", "%u KB into MEM2: CPU %u us (lines asked for %u ahead), DMA %u us%s; out of MEM2: CPU %u us "
             "(%u ahead), DMA %u us%s; savestates by the %s, loads by the %s",
             (unsigned) (len / 1024), (unsigned) save_us[best_save], (unsigned) s_ahead_save,
             (unsigned) save_dma_us, save_ok ? "" : " (WRONG)", (unsigned) load_us[best_load],
             (unsigned) s_ahead_load, (unsigned) load_dma_us, load_ok ? "" : " (WRONG)",
             s_dma_save ? "DMA" : "CPU", s_dma_load ? "DMA" : "CPU");
}
