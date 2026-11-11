/* The program's start: the console's services (wiilib's platform layer over
 * libogc), then the game's own main (gm/gmmain.c, built as melee_main) */

#include "melee_wii.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>

#include <wl/base/base.h>
#include <wl/platform/platform.h>
#include <wl/platform/stop.h>

/* libogc's sbrk takes MEM2 for newlib's malloc while this is set (its own
 * definition, weak, is 0) */
extern u32 MALLOC_MEM2;

int melee_main(void);

#ifdef MELEE_ORIG_CODE
/* launcher.c: what the player chose (Slippi's online mode, its codes), and
 * its start (wii/src/slippi/so_wii.c) */
void melee_wii_launch(void);
#endif

#ifdef MELEE_ORIG_CODE
/* The game as the decomp's MWCC build links it (wii/tools/orig_image.py): its
 * DOL's sections are where the GameCube has them; the port's main() clears
 * its zero-initialised ranges, has the SDK's entry points the port replaces
 * branch to the port's functions, and runs its static constructors */
typedef struct MeleePatch {
    u32 site;
    void* target;
} MeleePatch;
typedef struct MeleeTramp {
    u32 site;
    void* target;
    u32 size; /* the function's */
} MeleeTramp;
extern const MeleeTramp melee_tramp_table[];
extern const MeleePatch melee_call_table[];
extern const u32 melee_zero_table[][2];
/* the DOL's sections under the Wii's globals at 0x80003100: {address, size,
 * bytes}, copied there once libogc has started */
typedef struct MeleeLate {
    u32 address;
    u32 size;
    const void* bytes;
} MeleeLate;
extern const MeleeLate melee_late_table[];
extern void (*const __melee_ctors_start[])(void);
extern void (*const __melee_ctors_end[])(void);
extern u8 __melee_image_start[];
extern u8 __melee_image_end[];

/* Code the port puts in MEM1, in the game's reach (the island: MetroTRK's
 * code, which nothing calls; wii/tools/orig_image.py): the veneers below, and
 * Gecko codes' blocks (gecko.c) */
extern u32 __melee_island_start[];
extern u32 __melee_island_end[];
static u32* s_island = __melee_island_start;

void* melee_wii_island_alloc(u32 size)
{
    u32* p = s_island;
    size = (size + 3) & ~3u;
    if ((u8*) p + size > (u8*) __melee_island_end) {
        return NULL;
    }
    s_island = (u32*) ((u8*) p + size);
    return p;
}

/* Where the island's next block would go (fused.c: the code before it) */
u32* melee_wii_island_next(void)
{
    return s_island;
}

void melee_wii_watch_note(u32 addr, u32 len) __attribute__((weak)); /* diag_watch.c */

void melee_wii_put_branch(u32 site, void* target, u32 link)
{
    s32 disp = (s32) ((u32) target - site);
    if (disp < -0x2000000 || disp >= 0x2000000) {
        /* The port's code is in MEM2, out of a branch's reach: a veneer in
         * MEM1 jumps there; r12 is free at a call */
        u32* v = melee_wii_island_alloc(16);
        if (v == NULL) {
            melee_wii_fatal("Out of room for the veneers to the port's code");
        }
        u32 t = (u32) target;
        v[0] = 0x3D800000 | (((t + 0x8000) >> 16) & 0xFFFF); /* lis r12, t@ha */
        v[1] = 0x398C0000 | (t & 0xFFFF);                    /* addi r12, r12, t@l */
        v[2] = 0x7D8903A6;                                   /* mtctr r12 */
        v[3] = 0x4E800420;                                   /* bctr */
        DCFlushRange(v, 16);
        ICInvalidateRange(v, 16);
        disp = (s32) ((u32) v - site);
    }
    *(volatile u32*) site = 0x48000000 | ((u32) disp & 0x03FFFFFC) | link;
    if (melee_wii_watch_note != NULL) {
        melee_wii_watch_note(site, 4);
    }
}

#define put_branch melee_wii_put_branch

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* tramp.S: an SDK function the game calls that the port doesn't have */
void melee_wii_sdk_missing(const char* name)
{
    melee_wii_fatal("The game called %s, which the port doesn't have.", name);
}

/* The game's memset, __fill_mem and memcpy (MSL, at the start of its .init:
 * 0x80003100-0x80003244, then TRK's copies of them to 0x80003298) are where a
 * Wii keeps its own globals: the memory and IOS versions libogc reads, and
 * words IOS writes while it runs (a Wii's at 0x800031A0 landed in
 * __fill_mem's loop: an illegal instruction at the game's first big memset).
 * The port leaves that memory to the Wii and runs the three from a copy in
 * the island, every call to them moved there (libc_relocate). */
#define WII_LOW_LO 0x80003100u
#define WII_LOW_HI 0x80003298u
#define LIBC_LO 0x80003100u
#define LIBC_HI 0x80003244u

static const u8* s_init_bytes; /* the game's (or a mod's) .init, as its DOL has it */
static u32* s_libc;            /* the three, in the island */

/* Copies a section of the game's image to its place, but for the Wii's
 * globals */
static void copy_section(u32 address, const u8* bytes, u32 size)
{
    u32 end = address + size;
    if (address == WII_LOW_LO) {
        s_init_bytes = bytes;
    }
    if (address < WII_LOW_HI && end > WII_LOW_LO) {
        if (address < WII_LOW_LO) {
            memcpy((void*) address, bytes, WII_LOW_LO - address);
        }
        if (end > WII_LOW_HI) {
            memcpy((void*) WII_LOW_HI, bytes + (WII_LOW_HI - address), end - WII_LOW_HI);
        }
        return;
    }
    memcpy((void*) address, bytes, size);
}

/* The game's image as it starts: the sections below 0x80003F00 (late: over
 * libogc's low memory until now), and its zero-initialized data */
static void image_copy(void)
{
    for (const MeleeLate* l = melee_late_table; l->size != 0; l++) {
        copy_section(l->address, l->bytes, l->size);
    }
    for (int i = 0; melee_zero_table[i][1] != 0; i++) {
        memset((void*) melee_zero_table[i][0], 0,
               melee_zero_table[i][1] - melee_zero_table[i][0]);
    }
}

/* The game's (and a mod's) branches to the three, over [lo, hi): to the copy */
static int libc_retarget(u32 lo, u32 hi)
{
    int n = 0;
    for (u32 a = lo; a < hi; a += 4) {
        u32 w = *(volatile u32*) a;
        s32 d;
        u32 t;
        if ((w & 0xFC000002) != 0x48000000) { /* b, bl (relative) */
            continue;
        }
        d = (s32) (w & 0x03FFFFFC);
        if (d & 0x02000000) {
            d -= 0x04000000;
        }
        t = a + (u32) d;
        if (t < LIBC_LO || t >= LIBC_HI) {
            continue;
        }
        d = (s32) ((u32) s_libc + (t - LIBC_LO) - a);
        *(volatile u32*) a = 0x48000000 | ((u32) d & 0x03FFFFFC) | (w & 1);
        n++;
    }
    return n;
}

static void libc_relocate(void)
{
    int n;
    if (s_init_bytes == NULL) {
        return;
    }
    s_libc = melee_wii_island_alloc(LIBC_HI - LIBC_LO);
    if (s_libc == NULL) {
        melee_wii_fatal("Out of room for the game's memset and memcpy");
    }
    /* (their branches are among themselves: the copy runs anywhere) */
    memcpy(s_libc, s_init_bytes, LIBC_HI - LIBC_LO);
    DCFlushRange(s_libc, LIBC_HI - LIBC_LO);
    ICInvalidateRange(s_libc, LIBC_HI - LIBC_LO);
    /* the rest of .init (a mod's code may be there), then the game's text */
    n = libc_retarget(WII_LOW_HI, 0x80005520u) + libc_retarget(0x80005940u, 0x803B7240u);
    melee_wii_libc_guard();
    WL_LOG_I("melee", "memset and memcpy at %p (%d calls moved there)", s_libc, n);
}

/* Code the game loads with its data (Slippi's SlippiCSS.dat) has its calls
 * to the three made when it loads, to their old places: one branch word at
 * each of their starts (three of the Wii's words: the system watch thread
 * puts them back if IOS writes them), and a fall-back in the exception
 * handler (stall.c) for whatever still runs there */
static const u32 s_libc_entries[] = { 0x80003100u, 0x80003130u, 0x800031F4u };

void melee_wii_libc_guard(void)
{
    if (s_libc == NULL) {
        return;
    }
    for (unsigned i = 0; i < sizeof(s_libc_entries) / sizeof(s_libc_entries[0]); i++) {
        u32 a = s_libc_entries[i];
        u32 want = 0x48000000u | (((u32) s_libc + (a - LIBC_LO) - a) & 0x03FFFFFCu);
        if (*(volatile u32*) a != want) {
            *(volatile u32*) a = want;
            DCFlushRange((void*) (a & ~31u), 32);
            ICInvalidateRange((void*) (a & ~31u), 32);
        }
    }
}

/* Where code that runs at `pc` (in the three's old place) runs now, or 0 */
unsigned int melee_wii_libc_moved(unsigned int pc)
{
    if (s_libc == NULL || pc < LIBC_LO || pc >= LIBC_HI) {
        return 0;
    }
    return (u32) s_libc + (pc - LIBC_LO);
}

/* Gecko codes call the three by address (lis rX, 0x8000; ori or addi rX, rX,
 * low): those pairs, moved to the copy; how many */
int melee_wii_libc_fix_code(unsigned int* words, unsigned int count)
{
    int n = 0;
    if (s_libc == NULL) {
        return 0;
    }
    for (u32 i = 0; i + 1 < count; i++) {
        u32 w = words[i];
        u32 rx;
        if ((w & 0xFC1FFFFFu) != 0x3C008000u) { /* lis rX, 0x8000 */
            continue;
        }
        rx = (w >> 21) & 31;
        for (u32 j = 1; j <= 3 && i + j < count; j++) {
            u32 w2 = words[i + j], op = w2 >> 26, t, nt;
            if ((op != 24 && op != 14) || ((w2 >> 21) & 31) != rx || ((w2 >> 16) & 31) != rx) {
                continue;
            }
            t = 0x80000000u | (w2 & 0xFFFF);
            if ((op == 14 && (w2 & 0x8000)) || t < LIBC_LO || t >= LIBC_HI) {
                break;
            }
            nt = (u32) s_libc + (t - LIBC_LO);
            words[i] = (w & 0xFFFF0000u) | (op == 24 ? nt >> 16 : (nt + 0x8000) >> 16);
            words[i + j] = (w2 & 0xFFFF0000u) | (nt & 0xFFFF);
            n++;
            break;
        }
    }
    return n;
}

/* The game's own words where the port puts its branches (each trampoline's
 * 16 bytes, each call's word), kept at the start: a mod whose code is there
 * (20XX's in THPDec and MCC, which the port replaces) keeps it */
static u32* s_tramp_orig;
static u32* s_call_orig;
static int s_mod_loaded;

static void keep_originals(void)
{
    int n = 0, m = 0;
    for (const MeleeTramp* p = melee_tramp_table; p->site != 0; p++) {
        n++;
    }
    for (const MeleePatch* p = melee_call_table; p->site != 0; p++) {
        m++;
    }
    s_tramp_orig = malloc(n * 16 + 4);
    s_call_orig = malloc(m * 4 + 4);
    if (s_tramp_orig != NULL) {
        int i = 0;
        for (const MeleeTramp* p = melee_tramp_table; p->site != 0; p++, i++) {
            memcpy(&s_tramp_orig[i * 4], (const void*) p->site, p->size >= 16 ? 16 : 4);
        }
    }
    if (s_call_orig != NULL) {
        int i = 0;
        for (const MeleePatch* p = melee_call_table; p->site != 0; p++, i++) {
            s_call_orig[i] = *(const u32*) p->site;
        }
    }
}

/* After a mod's DOL: is the site still the game's own? */
static int site_vanilla(const u32* orig, u32 site, u32 bytes)
{
    return !s_mod_loaded || orig == NULL || memcmp(orig, (const void*) site, bytes) == 0;
}

/* The port's changes to the game's code: the SDK functions it replaces jump
 * to its own, and the calls it takes over */
static void image_patch(void)
{
    int i = 0, kept = 0;
    libc_relocate();
    for (const MeleeTramp* p = melee_tramp_table; p->site != 0; p++, i++) {
        if (!site_vanilla(s_tramp_orig ? &s_tramp_orig[i * 4] : NULL, p->site, p->size >= 16 ? 16 : 4)) {
            kept++;
            continue;
        }
        if (p->size >= 16) {
            /* the jump in full at the function's start: lis r12; addi r12;
             * mtctr r12; bctr (r12 is free at a call) */
            volatile u32* v = (volatile u32*) p->site;
            u32 t = (u32) p->target;
            v[0] = 0x3D800000 | (((t + 0x8000) >> 16) & 0xFFFF);
            v[1] = 0x398C0000 | (t & 0xFFFF);
            v[2] = 0x7D8903A6;
            v[3] = 0x4E800420;
        } else {
            put_branch(p->site, p->target, 0);
        }
    }
    i = 0;
    for (const MeleePatch* p = melee_call_table; p->site != 0; p++, i++) {
        if (!site_vanilla(s_call_orig ? &s_call_orig[i] : NULL, p->site, 4)) {
            kept++;
            continue;
        }
        put_branch(p->site, p->target, 1);
    }
    if (kept != 0) {
        WL_LOG_I("melee", "the mod's code is where %d of the port's branches go: kept", kept);
    }
    DCFlushRange(__melee_image_start, __melee_image_end - __melee_image_start);
    ICInvalidateRange(__melee_image_start, __melee_image_end - __melee_image_start);
}

/* The launcher, for a modded game on its own disc (20XX): that disc's
 * executable instead of the game's, before the game starts. A mod's DOL is
 * the 1.02 DOL changed in place (its code in the game's unused regions), so
 * its sections are where the game's are; the port's changes go on top again
 * (a mod's changes to the SDK functions the port replaces are left out, as
 * Gecko codes' are). Returns 0, or < 0 for a DOL that doesn't fit. */
static void orig_ctors(void);
void melee_wii_watch_start(void);

int melee_wii_image_load_dol(const u8* dol, u32 size)
{
    if (size < 0x100) {
        return -1;
    }
    for (int i = 0; i < 18; i++) {
        u32 off = be32(dol + i * 4), addr = be32(dol + 0x48 + i * 4), len = be32(dol + 0x90 + i * 4);
        if (len == 0) {
            continue;
        }
        if (off + len > size || addr < 0x80003100 || addr + len > (u32) __melee_image_end) {
            WL_LOG_E("melee", "the DOL's section %d (%08x, %u bytes) is outside the game's image",
                     i, (unsigned) addr, (unsigned) len);
            return -2;
        }
    }
    image_copy();
    s_mod_loaded = 1;
    for (int i = 0; i < 18; i++) {
        u32 off = be32(dol + i * 4), addr = be32(dol + 0x48 + i * 4), len = be32(dol + 0x90 + i * 4);
        if (len == 0) {
            continue;
        }
        copy_section(addr, dol + off, len);
    }
    image_patch();
    orig_ctors(); /* (the data they set up is the DOL's now) */
    return 0;
}

static void orig_boot(void)
{
    image_copy();
    keep_originals();
    image_patch();

    /* The paired singles and their quantized loads, as the GameCube's start
     * (__OSPSInit) leaves them: HID2's PSE and LSQE, every GQR 0 */
    u32 hid2;
    __asm__ volatile("mfspr %0, 920" : "=r"(hid2));
    hid2 |= 0xA0000000;
    __asm__ volatile("mtspr 920, %0" ::"r"(hid2));
    __asm__ volatile("mtspr 912, %0\n\tmtspr 913, %0\n\tmtspr 914, %0\n\t"
                     "mtspr 915, %0\n\tmtspr 916, %0\n\tmtspr 917, %0\n\t"
                     "mtspr 918, %0\n\tmtspr 919, %0\n\tisync" ::"r"(0));
}

static void orig_ctors(void)
{

    for (void (*const* f)(void) = __melee_ctors_start; f < __melee_ctors_end; f++) {
        if (*f != NULL) {
            (*f)();
        }
    }
}
#else
/* The game's zero-initialized data, laid out as on the GameCube
 * (wii/tools/data_layout.py): outside the .bss that libogc's start clears */
extern u8 __melee_bss_start[];
extern u8 __melee_bss_end[];

/* MSL/trigf.c's table, which MWCC's start-up code fills (the static
 * constructors in .ctors) */
void __sinit_trigf_c(void);
#endif

/* The boot's log, line by line: the log's own thread writes once a second,
 * so a boot that stops would lose its last lines. Until the game's first
 * scenes, the main thread's lines are written at once (with its interrupts
 * on: the write takes a lock). */
static lwp_t s_main_thread = LWP_THREAD_NULL;
static volatile int s_log_sync = 1;

void melee_wii_log_point(void)
{
    u32 msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    if (s_log_sync && (msr & 0x8000) && LWP_GetSelf() == s_main_thread) {
        wl_platform_log_flush();
    }
}

void melee_wii_log_sync_end(void)
{
    melee_wii_log_point();
    s_log_sync = 0;
}

void melee_wii_exit(void)
{
    wl_platform_exit(WL_EXIT_LOADER);
}

/* Leaving (the reset or power button, the game's OSResetSystem, a stop
 * screen): what the game set going is stopped first, before the platform's
 * own steps (the log written out, the card unmounted) and the jump back to
 * the loader. Left alone, the audio interface's DMA plays its last 5 ms over
 * and over and the DSP goes on running AX while the Homebrew Channel loads;
 * a disc read would go on from the SD card as it is unmounted; rollback's
 * page table would still be mapping MEM1; and Slippi's socket open in IOS.
 * The loader's stub (0x80001800, "STUBHAXX", where libogc's exit jumps) is
 * put back as it was at the start, should anything have written over it. */
#define LOADER_STUB 0x80001800u
#define LOADER_STUB_SIZE 0x1800u /* up to the game's image, 0x80003100 */
static u8 s_loader_stub[LOADER_STUB_SIZE] __attribute__((aligned(32)));
static int s_have_loader_stub;

void AIStopDMA(void);
void melee_wii_dvd_stop(void);
void so_socket_close(void) __attribute__((weak));

static void keep_loader_stub(void)
{
    if (memcmp((const void*) (LOADER_STUB + 4), "STUBHAXX", 8) == 0) {
        memcpy(s_loader_stub, (const void*) LOADER_STUB, LOADER_STUB_SIZE);
        s_have_loader_stub = 1;
    }
}

/* Tests (sd:/apps/melee/exittest): a loader's stub as the Homebrew Channel
 * leaves one, here a branch to itself, and the reset button's exit 20 s on,
 * from the watcher's thread as the button's: where the CPU ends up (Dolphin's
 * debugger, dolphin_run.py --backtrace) shows whether the exit gets to it */
static u32 s_exit_test;

int melee_wii_home_held(void); /* classic.c */

static void exit_test_tick(void* user)
{
    melee_wii_stall_tick(user);
    if (melee_wii_home_held()) {
        /* a Wii Remote's HOME held for a second: as the reset button */
        WL_LOG_I("melee", "HOME held: leaving");
        wl_platform_exit(WL_EXIT_LOADER);
    }
    if (s_exit_test != 0 && ++s_exit_test == 1250) {
        WL_LOG_I("melee", "exittest: leaving as the reset button does");
        wl_platform_exit(WL_EXIT_LOADER);
    }
}

static void exit_test_stub(void)
{
    *(volatile u32*) LOADER_STUB = 0x48000000u; /* b . */
    memcpy((void*) (LOADER_STUB + 4), "STUBHAXX", 8);
    DCFlushRange((void*) LOADER_STUB, 32);
    ICInvalidateRange((void*) LOADER_STUB, 32);
    keep_loader_stub();
    s_exit_test = 1;
}

static void before_exit(void)
{
    volatile u16* dsp_control = (volatile u16*) 0xCC00500A;
    AIStopDMA();
    /* the DSP halted (its interrupt bits, which a 1 clears, left alone) */
    *dsp_control = (u16) ((*dsp_control & ~0x00A8u) | 0x0004u);
    melee_wii_mmu_track_stop();
    if (so_socket_close != NULL) {
        so_socket_close();
    }
    melee_wii_wc24_resume();
    melee_wii_dvd_stop();
    if (s_have_loader_stub && memcmp((const void*) LOADER_STUB, s_loader_stub, LOADER_STUB_SIZE) != 0) {
        WL_LOG_W("melee", "the loader's stub was written over: put back");
        memcpy((void*) LOADER_STUB, s_loader_stub, LOADER_STUB_SIZE);
        DCFlushRange((void*) LOADER_STUB, LOADER_STUB_SIZE);
        ICInvalidateRange((void*) LOADER_STUB, LOADER_STUB_SIZE);
    }
}

void melee_wii_fatal(const char* fmt, ...)
{
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    wl_platform_stop_screen("Super Smash Bros. Melee\n\n%s", msg);
}

/* MSL/trigf.c's table, which MWCC's start-up code fills (the static
 * constructors in .ctors) */
void __sinit_trigf_c(void);

/* The GPU's command FIFO reset, as libogc's GX_AbortFrame does it, before the
 * game's GXInit, which takes the GPU as a GameCube's start leaves it: on a
 * Wii, the program before (the Homebrew Channel draws with GX) may not */
static void gx_reset(void)
{
    volatile u32* pi = (volatile u32*) 0xCC003000u;
    u64 start;
    pi[6] = 1;
    start = gettime();
    while (gettime() - start < 50) {
    }
    pi[6] = 0;
    start = gettime();
    while (gettime() - start < 5) {
    }
}

int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    melee_wii_reserve_arena();

#ifndef MELEE_ORIG_CODE
    memset(__melee_bss_start, 0, __melee_bss_end - __melee_bss_start);
    DCFlushRange(__melee_bss_start, __melee_bss_end - __melee_bss_start);
    __sinit_trigf_c();
#endif

    /* newlib's malloc in MEM2: MEM1 is the game's arena, as on the GameCube */
    MALLOC_MEM2 = 1;

    keep_loader_stub();
    wl_platform_fs_ios_init(NULL);
    wl_platform_system_init();
    wl_platform_set_exit_hook(before_exit);
    wl_platform_fs_mount(WL_DEVICE_SD);
    wl_platform_log_init("sd:/apps/melee/logs");
    s_main_thread = LWP_GetSelf();
    if (wl_platform_fs_exists("sd:/apps/melee/exittest")) {
        exit_test_stub();
    }
    wl_platform_system_watch_start(16, exit_test_tick, NULL);
    melee_wii_crash_install();
    /* WiiConnect24's background sends and downloads held off while the game
     * runs (wifi.c) */
    melee_wii_wc24_suspend();

    WL_LOG_I("melee", "Super Smash Bros. Melee for the Wii");
    {
        void melee_wii_fp_fingerprint(void) __attribute__((weak));
        if (melee_wii_fp_fingerprint != NULL) {
            melee_wii_fp_fingerprint();
        }
    }

#ifdef MELEE_ORIG_CODE
    /* (nothing above touches the game's image) */
    orig_boot();
    WL_LOG_I("melee", "the game's image is ready");
    melee_wii_watch_start(); /* (test builds: diag_watch.c) */
    {
        /* tests: the rollback's page tracking (mmu_track.c) */
        void melee_wii_mmu_selftest(void);
        if (wl_platform_fs_exists("sd:/apps/melee/mmutest")) {
            melee_wii_mmu_selftest();
        }
    }
    melee_wii_log_point();
    orig_ctors();
    melee_wii_fpscr();
    melee_wii_launch(); /* (it may load a mod's DOL in the game's place) */
    WL_LOG_I("melee", "starting the game");
    melee_wii_log_point();
#endif
    gx_reset();
    melee_wii_stall_start();
#ifdef MELEE_ORIG_CODE
    if (wl_platform_fs_exists("sd:/apps/melee/lowmemtest")) {
        /* tests: the Wii's globals' memory written over, as IOS does, with
         * traps, after the game's code there (memcpy, the rest of
         * __fill_mem) */
        for (u32 a = 0x80003160u; a < WII_LOW_HI; a += 4) {
            *(volatile u32*) a = 0x7FE00008u;
        }
        DCFlushRange((void*) 0x80003160u, WII_LOW_HI - 0x80003160u);
        ICInvalidateRange((void*) 0x80003160u, WII_LOW_HI - 0x80003160u);
        WL_LOG_I("melee", "lowmemtest: traps at 80003160-%08x", (unsigned) WII_LOW_HI);
    }
#endif
    WL_LOG_I("melee", "%s", melee_wii_wc24_status());
    melee_main();
    melee_wii_exit();
}
