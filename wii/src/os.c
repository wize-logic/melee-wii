/* The SDK's OS on the Wii: libogc owns the processor (exceptions, interrupts,
 * threads, the decrementer); this keeps the SDK's contracts above it. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/os.h>

#include <wl/base/base.h>

/* The game's arena: all of MEM1 that libogc leaves (newlib's malloc is in
 * MEM2: main.c's MALLOC_MEM2) */
static void* s_arena_lo;
static void* s_arena_hi;

static BOOL s_initialized;

/* MEM1's free memory is the game's arena, as on the GameCube: taken from
 * libogc at the start (main.c), before anything (the launcher, Slippi's
 * client) allocates, so newlib's malloc goes to MEM2 instead */
/* The game's arena: the GameCube's, from the end of the game's main stack
 * (wii/tools/orig_image.py's __melee_arena_lo: mods like 20XX branch into
 * the files they load at the addresses a GameCube gives them) up to libogc's
 * own MEM1 (its arena, which the linker script puts under the DSP's data at
 * the top of MEM1) */
extern char __melee_arena_lo[];
extern char __Arena1Lo[];

/* The SDK's own arena variables (OSArena.c's __OSArenaLo and __OSArenaHi, at
 * their places in the game's small data), kept as this file's are: the
 * game's code reads the arena through the functions below, but 20XX reads
 * __OSArenaHi itself (it keeps a match's players' records just under the
 * arena's top) and found 0 there: a store through -4 as a match began. */
#ifdef MELEE_ORIG_CODE
#define SDK_ARENA_LO ((void* volatile*) 0x804D5C10)
#define SDK_ARENA_HI ((void* volatile*) 0x804D7370)
static void arena_changed(void)
{
    *SDK_ARENA_LO = s_arena_lo;
    *SDK_ARENA_HI = s_arena_hi;
}
#else
static void arena_changed(void) {}
#endif

void melee_wii_reserve_arena(void)
{
    if (s_arena_hi != NULL) {
        return;
    }
    s_arena_lo = (void*) OSRoundUp32B(__melee_arena_lo);
    s_arena_hi = (void*) OSRoundDown32B(__Arena1Lo);
}

/* The launcher's frame buffers: the top of the game's arena to be (which
 * OSInit clears before the game has it) */
void* melee_wii_arena_scratch(unsigned int size)
{
    melee_wii_reserve_arena();
    return (void*) (((u32) s_arena_hi - size) & ~31u);
}

void OSInit(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = TRUE;

    /* The SDK's clocks are read from low memory (OS_BUS_CLOCK), where the
     * Wii's loader leaves its own */
    if (OS_BUS_CLOCK == 0) {
        __OSBusClock = 243000000;
        __OSCoreClock = 729000000;
    }

    melee_wii_reserve_arena();
    /* As the GameCube's boot leaves it: zeros. The game takes fields of its
     * fresh heap memory for zero in places (a texture's or display list's
     * address with stray bits in the GX register the Wii reads more of), and
     * what is left here (the port's staging copy, libogc's) is not */
    memset(s_arena_lo, 0, (u8*) s_arena_hi - (u8*) s_arena_lo);
    DCFlushRange(s_arena_lo, (u8*) s_arena_hi - (u8*) s_arena_lo);

    arena_changed();
    melee_wii_thread_init();
    OSReport("OSInit: arena %p-%p (%u KiB), bus %u Hz\n", s_arena_lo,
             s_arena_hi,
             (unsigned) (((u8*) s_arena_hi - (u8*) s_arena_lo) / 1024),
             (unsigned) OS_BUS_CLOCK);
}

#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define ROUND_DOWN(x, a) ((x) & ~((a) - 1))

/* OSArena.c */
void* OSGetArenaHi(void)
{
    return s_arena_hi;
}

void* OSGetArenaLo(void)
{
    return s_arena_lo;
}

void OSSetArenaHi(void* addr)
{
    s_arena_hi = addr;
    arena_changed();
}

void OSSetArenaLo(void* addr)
{
    s_arena_lo = addr;
    arena_changed();
}

void* OSAllocFromArenaLo(u32 size, u32 align)
{
    u8* ptr = (u8*) ROUND_UP((u32) s_arena_lo, align);
    s_arena_lo = (void*) ROUND_UP((u32) (ptr + size), align);
    arena_changed();
    return ptr;
}

void* OSAllocFromArenaHi(u32 size, u32 align)
{
    u8* arena = (u8*) ROUND_DOWN((u32) s_arena_hi, align);
    u8* ptr = (u8*) ROUND_DOWN((u32) (arena - size), align);
    s_arena_hi = ptr;
    arena_changed();
    return ptr;
}

/* OSMemory.c: a retail console's 24 MiB, not a development kit's 48 */
u32 OSGetPhysicalMemSize(void)
{
    return 24 * 1024 * 1024;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return 24 * 1024 * 1024;
}

/* OSError.c. Lines are assembled until their newline: the game reports a
 * line in pieces */
static char s_report[512];
static size_t s_report_len;

static void report(const char* msg, va_list args)
{
    BOOL enabled = OSDisableInterrupts();
    BOOL logged = FALSE;
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), msg, args);
    if (n < 0) {
        n = 0;
    } else if (n >= (int) sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n' || s_report_len == sizeof(s_report) - 1) {
            s_report[s_report_len] = '\0';
            if (strncmp(s_report, "SYNC", 4) == 0) {
                /* the determinism test's lines (gm/synctest.inc): every one,
                 * straight out (the log drops lines when they come faster
                 * than its writer) */
                SYS_Report("%s\n", s_report);
            } else {
                wl_log_write(WL_LOG_LEVEL_INFO, "melee", "%s", s_report);
                logged = TRUE;
            }
            s_report_len = 0;
            if (c == '\n') {
                continue;
            }
        }
        s_report[s_report_len++] = c;
    }
    OSRestoreInterrupts(enabled);
    /* (on the card at once while the game starts: with interrupts on again,
     * so a boot that stops leaves its last line) */
    if (logged) {
        melee_wii_log_point();
    }
}

void OSReport(char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    report(msg, args);
    va_end(args);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    char buf[256];
    va_list args;
    OSDisableInterrupts();
    va_start(args, msg);
    vsnprintf(buf, sizeof(buf), msg, args);
    va_end(args);
    wl_log_write(WL_LOG_LEVEL_ERROR, "melee", "OSPanic in \"%s\" on line %d: %s",
                 file, line, buf);
    melee_wii_fatal("OSPanic in \"%s\" on line %d:\n%s", file, line, buf);
}

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    /* libogc's exception handlers keep them */
    (void) error;
    (void) handler;
    return NULL;
}

/* OSInterrupt.c: MSR[EE]. The game's own too (cmake/Wii.cmake: --override),
 * in assembly that uses only what the SDK's does and r0, r12, cr0 (code
 * patched in calls them from assembly). Each turn of interrupts off or on is noted
 * with its caller in a ring (melee_wii_irq_ring: {caller, kind}; kinds:
 * 1 OSDisableInterrupts turned them off, 2 OSRestoreInterrupts on, 3
 * OSRestoreInterrupts off, 4 OSEnableInterrupts on; 0x1xx a Slippi command,
 * so_exi.c), for wii/src/stall.c should the game's frame find them left
 * off. */
volatile u32 melee_wii_irq_ring[128][2];
volatile u32 melee_wii_irq_ring_next __attribute__((section(".sdata"))) = 0;

void melee_wii_irq_mark(u32 kind)
{
    u32 msr, i;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" ::"r"(msr & ~0x8000u) : "memory");
    i = melee_wii_irq_ring_next++ % 128;
    melee_wii_irq_ring[i][0] = (u32) __builtin_return_address(0);
    melee_wii_irq_ring[i][1] = kind;
    __asm__ volatile("mtmsr %0" ::"r"(msr) : "memory");
}

__asm__(".text\n"
        /* r4: the caller, r0: the kind; r3 and r5 kept; uses r12 */
        "irq_event:\n"
        "\tstwu 1, -16(1)\n"
        "\tstw 5, 8(1)\n"
        "\tlwz 12, melee_wii_irq_ring_next@sda21(0)\n"
        "\taddi 5, 12, 1\n"
        "\tstw 5, melee_wii_irq_ring_next@sda21(0)\n"
        "\trlwinm 12, 12, 3, 22, 28\n"
        "\tlis 5, melee_wii_irq_ring@ha\n"
        "\taddi 5, 5, melee_wii_irq_ring@l\n"
        "\tadd 12, 12, 5\n"
        "\tstw 4, 0(12)\n"
        "\tstw 0, 4(12)\n"
        "\tlwz 5, 8(1)\n"
        "\taddi 1, 1, 16\n"
        "\tblr\n"

        "\t.globl OSDisableInterrupts\n"
        "\t.type OSDisableInterrupts, @function\n"
        "OSDisableInterrupts:\n"
        "\tmfmsr 3\n"
        "\trlwinm 4, 3, 0, 17, 15\n"
        "\tmtmsr 4\n"
        "\trlwinm 3, 3, 17, 31, 31\n"
        "\tcmpwi 3, 0\n"
        "\tbeqlr\n"
        "\tmflr 4\n"
        "\tli 0, 1\n"
        "\tb irq_event\n"
        "\t.size OSDisableInterrupts, .-OSDisableInterrupts\n"

        "\t.globl OSEnableInterrupts\n"
        "\t.type OSEnableInterrupts, @function\n"
        "OSEnableInterrupts:\n"
        "\tmfmsr 3\n"
        "\tori 4, 3, 0x8000\n"
        "\tmtmsr 4\n"
        "\trlwinm 3, 3, 17, 31, 31\n"
        "\tcmpwi 3, 0\n"
        "\tbnelr\n"
        "\tmflr 4\n"
        "\tli 0, 4\n"
        "\tb irq_event\n"
        "\t.size OSEnableInterrupts, .-OSEnableInterrupts\n"

        "\t.globl OSRestoreInterrupts\n"
        "\t.type OSRestoreInterrupts, @function\n"
        "OSRestoreInterrupts:\n"
        "\tcmpwi 3, 0\n"
        "\tmfmsr 4\n"
        "\tbeq 1f\n"
        "\tori 5, 4, 0x8000\n"
        "\tmtmsr 5\n"
        "\trlwinm 3, 4, 17, 31, 31\n"
        "\tcmpwi 3, 0\n"
        "\tbnelr\n"
        "\tmflr 4\n"
        "\tli 0, 2\n"
        "\tb irq_event\n"
        "1:\trlwinm 5, 4, 0, 17, 15\n"
        "\tmtmsr 5\n"
        "\trlwinm 3, 4, 17, 31, 31\n"
        "\tcmpwi 3, 0\n"
        "\tbeqlr\n"
        "\tmflr 4\n"
        "\tli 0, 3\n"
        "\tb irq_event\n"
        "\t.size OSRestoreInterrupts, .-OSRestoreInterrupts\n");

/* OSTime.c's assembly: the time base */
OSTime OSGetTime(void)
{
    u32 hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    return ((OSTime) hi << 32) | lo;
}

OSTick OSGetTick(void)
{
    u32 lo;
    __asm__ volatile("mftb %0" : "=r"(lo));
    return lo;
}

/* OSContext.c: the contexts the SDK switches its floating-point registers
 * with. libogc saves a thread's registers itself; these only keep the
 * pointers the SDK's callers hand back. */
static OSContext s_context;
static OSContext* s_current_context = &s_context;

OSContext* OSGetCurrentContext(void)
{
    return s_current_context;
}

void OSSetCurrentContext(OSContext* context)
{
    s_current_context = context;
}

void OSClearContext(OSContext* context)
{
    context->mode = 0;
    context->state = 0;
}

u32 OSSaveContext(OSContext* context)
{
    memset(context, 0, sizeof(*context));
    __asm__ volatile("mflr %0" : "=r"(context->lr));
    __asm__ volatile("mr %0, 1" : "=r"(context->gpr[1]));
    __asm__ volatile("mr %0, 2" : "=r"(context->gpr[2]));
    __asm__ volatile("mr %0, 13" : "=r"(context->gpr[13]));
    context->srr0 = context->lr;
    return 0;
}

void OSSaveFPUContext(OSContext* context)
{
    (void) context;
}

void OSLoadFPUContext(OSContext* context)
{
    (void) context;
}

/* OSReset.c: the reset and power buttons are wiilib's watcher thread's */
unsigned long OSGetResetCode(void)
{
    return 0;
}

BOOL OSGetResetSwitchState(void)
{
    return FALSE;
}

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
    OSReport("OSResetSystem(%d, 0x%x, %d): leaving\n", reset,
             (unsigned) resetCode, forceMenu);
    melee_wii_exit();
}

/* OSRtc.c: the console's settings stand for the GameCube's SRAM */
static u32 s_progressive = (u32) -1;
static u32 s_sound_mode = (u32) -1;

u32 OSGetProgressiveMode(void)
{
    if (s_progressive == (u32) -1) {
        s_progressive = CONF_GetProgressiveScan() > 0 ? 1 : 0;
    }
    return s_progressive;
}

void OSSetProgressiveMode(u32 on)
{
    s_progressive = on ? 1 : 0;
}

u32 OSGetSoundMode(void)
{
    if (s_sound_mode == (u32) -1) {
        /* CONF_SOUND_MONO is 0 */
        s_sound_mode = CONF_GetSoundMode() == 0 ? 0 : 1;
    }
    return s_sound_mode;
}

void OSSetSoundMode(u32 mode)
{
    s_sound_mode = mode ? 1 : 0;
}

/* base/PPCArch.c */
u32 PPCMfmsr(void)
{
    u32 v;
    __asm__ volatile("mfmsr %0" : "=r"(v));
    return v;
}

void PPCMtmsr(u32 v)
{
    __asm__ volatile("mtmsr %0" ::"r"(v) : "memory");
}

u32 PPCMfhid2(void)
{
    u32 v;
    __asm__ volatile("mfspr %0, 920" : "=r"(v));
    return v;
}

void PPCMthid2(u32 v)
{
    __asm__ volatile("mtspr 920, %0" ::"r"(v) : "memory");
}

u32 PPCMfwpar(void)
{
    u32 v;
    __asm__ volatile("sync; mfspr %0, 921" : "=r"(v)::"memory");
    return v;
}

void PPCMtwpar(u32 v)
{
    __asm__ volatile("mtspr 921, %0" ::"r"(v) : "memory");
}

/* The SDK's is a system call whose handler syncs; libogc's system call is
 * its own */
void PPCSync(void)
{
    __asm__ volatile("sync; isync" ::: "memory");
}

/* db.c */
BOOL DBIsDebuggerPresent(void)
{
    return FALSE;
}

/* OSExi.c: GXInit's reset of the EXI probes */
void EXIProbeReset(void) {}


#ifdef MELEE_ORIG_CODE
/* HSD_OSInit's heaps (wii/tools/orig_image.py --redirect): the audio heap,
 * then the first main heap; their bounds, for the rollback's savestates
 * (wii/src/slippi/so_rollback_ranges.c) */
static u32 s_hsd_heaps[2][2];
static int s_hsd_heap_count;

OSHeapHandle melee_wii_create_heap(void* lo, void* hi)
{
    if (s_hsd_heap_count < 2) {
        s_hsd_heaps[s_hsd_heap_count][0] = (u32) lo;
        s_hsd_heaps[s_hsd_heap_count][1] = (u32) hi;
        s_hsd_heap_count++;
    }
    return OSCreateHeap(lo, hi);
}

int melee_wii_audio_heap(unsigned int* lo, unsigned int* hi)
{
    if (s_hsd_heap_count < 1) {
        return -1;
    }
    *lo = s_hsd_heaps[0][0];
    *hi = s_hsd_heaps[0][1];
    return 0;
}

/* __ppc_eabi_init.c's, in .init (which the port's image leaves out): code
 * written at run time made fetchable. The game doesn't call it once it has
 * started; 20XX does, after loading code of its own. */
void __flush_cache(void* address, unsigned int size)
{
    DCStoreRange(address, size);
    ICInvalidateRange(address, size);
}
#endif
