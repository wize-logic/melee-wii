/* Test builds only (-DMELEE_WATCH_DIAG): a thread that compares the game's
 * code (0x80005940-0x803B7240) with its state after the boot, through the
 * uncached mirror (memory itself), from the boot on; logs the first change:
 * where, what, when (retraces), the scene, the last loads. */
#ifdef MELEE_WATCH_DIAG
#include <malloc.h>
#include <string.h>

#include <gccore.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>

#include <wl/base/base.h>

#include "melee_wii.h"

#define LO 0x80005940u
#define HI 0x803B7240u
#define STEP (16 * 1024 / 4)

static u32* s_copy;
static lwp_t s_thread;
static u8 s_stack[16 * 1024] __attribute__((aligned(32)));

extern u32 __melee_island_start[];
extern u32 __melee_island_end[];

static void* watch(void* arg)
{
    const volatile u32* mem = (const volatile u32*) (LO | 0x40000000u);
    u32 n = (HI - LO) / 4, at = 0, reports = 0, last = 0;
    u32 isl_lo = ((u32) __melee_island_start - LO) / 4, isl_hi = ((u32) __melee_island_end - LO) / 4;
    (void) arg;
    for (;;) {
        u32 end = at + STEP > n ? n : at + STEP;
        for (u32 i = at; i < end; i++) {
            if (i >= isl_lo && i < isl_hi) {
                continue; /* the port's island */
            }
            if (mem[i] != s_copy[i]) {
                if (reports < 40 && (i != last + 1)) {
                    const unsigned int *a, *l;
                    u32 cnt = melee_wii_dvd_loads(&a, &l);
                    reports++;
                    WL_LOG_E("watch", "CODE CHANGED at %08x: %08x -> %08x (ms %u, scene %08x, last load %08x+%x)",
                             (unsigned) (LO + i * 4), (unsigned) s_copy[i], (unsigned) mem[i],
                             (unsigned) (gettime() / TB_TIMER_CLOCK), (unsigned) *(volatile u32*) 0x80479D30,
                             cnt ? a[(cnt - 1) % 1024] : 0, cnt ? l[(cnt - 1) % 1024] : 0);
                    melee_wii_log_point();
                }
                last = i;
                s_copy[i] = mem[i];
            }
        }
        at = end >= n ? 0 : end;
        {
            static int shown;
            if (!shown && (*(volatile u32*) 0x80479D30 >> 24) == 2) {
                shown = 1;
                for (u32 k = 0; k < 256; k += 8) {
                    const volatile u32* w = (const volatile u32*) (0xC01C8C00u + k * 4);
                    WL_LOG_E("watch", "dump %08x: %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned) (0x801C8C00u + k * 4),
                             w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
                melee_wii_log_point();
            }
        }
        usleep(1000);
    }
    return NULL;
}

/* The port's own writes to the game's code (Gecko codes, branches): the
 * snapshot follows them */
void melee_wii_watch_note(u32 addr, u32 len)
{
    if (s_copy == NULL || addr + len <= LO || addr >= HI) {
        return;
    }
    if (addr < LO) {
        len -= LO - addr;
        addr = LO;
    }
    if (addr + len > HI) {
        len = HI - addr;
    }
    {
        u32 a = addr & ~3u, e = (addr + len + 3) & ~3u;
        for (; a < e; a += 4) {
            s_copy[(a - LO) / 4] = *(const volatile u32*) (a | 0x40000000u);
        }
    }
}

/* main.c, right after the game's image is set up */
void melee_wii_watch_start(void)
{
    s_copy = memalign(32, HI - LO);
    if (s_copy == NULL) {
        return;
    }
    memcpy(s_copy, (const void*) (LO | 0x40000000u), HI - LO);
    LWP_CreateThread(&s_thread, watch, NULL, s_stack, sizeof(s_stack), 100);
    WL_LOG_I("watch", "watching the game's code from the boot");
}
#else
void melee_wii_watch_start(void)
{
}
#endif
