/* A game that stops: where its main thread is, and the hardware's state,
 * written to the log (melee_wii.h).
 *
 * The game shows each frame through VISetNextFrameBuffer (vi.c); the shims'
 * blocking calls mark the main thread's waits with their own stack frame
 * (its frames pace themselves by polling the retrace count, not by waiting,
 * so a wait that lasts is the news). The system watch thread
 * (wl_platform_system_watch_start, above every thread of the game's) checks
 * each tick: when no new frame has come for 5 s (from the game's start),
 * it logs what the main thread waits in, if anything, the call chain under
 * that wait (the stack's back chain, as the SDK's OSDumpContext walks it),
 * and the registers of the hardware a start-up or frame waits on (GX's FIFO,
 * the pixel engine, VI, the DSP, the audio interface), then writes the log
 * out at once. Once per stall: a new frame arms it again. It needs interrupts
 * on (a stop with them off leaves nothing, as before). */

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <tuxedo/ppc/context.h>
#include <stdio.h>

#include <wl/base/base.h>
#include <wl/platform/platform.h>

#include "melee_wii.h"

#define STALL_SECONDS 5
#define MAX_FRAMES 16

static volatile int s_armed;
static lwp_t s_main;
static volatile int s_kind;
static const void* volatile s_object;
static volatile u32 s_frame;
static volatile u64 s_since;
static volatile u64 s_last_end;
static volatile u32 s_waits;
static volatile u64 s_started;
static volatile u64 s_last_frame;
static volatile u32 s_frames;
static u32 s_reported = 0xFFFFFFFFu;

/* The VI's retraces the game has had (vi.c) */
u32 VIGetRetraceCount(void);

void melee_wii_stall_start(void)
{
    s_main = LWP_GetSelf();
    s_started = s_last_end = gettime();
    s_armed = 1;
}

void melee_wii_stall_frame(void)
{
    /* (both halves at once for the tick, an interrupt: read half-written, it
     * once gave "no new frame for 3002397489 s") */
    u32 level = IRQ_Disable();
    s_last_frame = gettime();
    s_frames++;
    IRQ_Restore(level);
}

/* wii/src/slippi/so_gamelog.c */
int so_gamelog_recording(void) __attribute__((weak));
void so_gamelog_video_wait(u32 us) __attribute__((weak));

void melee_wii_wait_begin(int kind, const void* object, void* frame)
{
    if (!s_armed || LWP_GetSelf() != s_main) {
        return;
    }
    s_object = object;
    s_frame = (u32) frame;
    s_since = gettime();
    s_kind = kind;
}

void melee_wii_wait_end(void)
{
    if (!s_armed || LWP_GetSelf() != s_main) {
        return;
    }
    s_kind = 0;
    s_last_end = gettime();
    s_waits++;
    if (so_gamelog_recording != NULL && so_gamelog_recording()) {
        /* (the Diagnostics setting: the main thread's waits are a frame's spare time) */
        so_gamelog_video_wait((u32) diff_usec(s_since, s_last_end));
    }
}

#define WAIT_SUSPENDED 100

void melee_wii_wait_suspend(unsigned int lwp, int suspended, void* by)
{
    if (!s_armed || (lwp_t) lwp != s_main) {
        return;
    }
    if (suspended) {
        s_object = by;
        s_frame = 0;
        s_since = gettime();
        s_kind = WAIT_SUSPENDED;
    } else if (s_kind == WAIT_SUSPENDED) {
        s_kind = 0;
        s_last_end = gettime();
        s_waits++;
    }
}

/* A stack frame's address, as the game's and the port's stacks have them */
static int stack_addr(u32 p)
{
    return (p & 7) == 0 && ((p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u));
}

static void report_calls(u32 frame)
{
    char line[200];
    int n = 0, len = 0;
    len += snprintf(line + len, sizeof(line) - len, "stall: its calls (innermost first):");
    for (u32 p = frame; stack_addr(p) && n < MAX_FRAMES; n++) {
        u32 next = ((volatile u32*) p)[0];
        len += snprintf(line + len, sizeof(line) - len, " %08x", (unsigned) ((volatile u32*) p)[1]);
        if (len >= (int) sizeof(line) - 10 || next <= p) {
            break;
        }
        p = next;
    }
    WL_LOG_W("melee", "%s", line);
}

static u32 cp32(u32 off)
{
    volatile u16* cp = (volatile u16*) 0xCC000000u;
    return ((u32) cp[(off + 2) / 2] << 16) | cp[off / 2];
}

static void report_hardware(void)
{
    volatile u32* pi = (volatile u32*) 0xCC003000u;
    volatile u16* cp = (volatile u16*) 0xCC000000u;
    volatile u16* pe = (volatile u16*) 0xCC001000u;
    volatile u16* vi = (volatile u16*) 0xCC002000u;
    volatile u16* dsp = (volatile u16*) 0xCC005000u;
    volatile u32* ai = (volatile u32*) 0xCD006C00u;
    WL_LOG_W("melee", "stall: PI cause %08x mask %08x, its FIFO %08x-%08x write %08x",
             (unsigned) pi[0], (unsigned) pi[1], (unsigned) pi[3], (unsigned) pi[4], (unsigned) pi[5]);
    WL_LOG_W("melee",
             "stall: CP status %04x control %04x, FIFO %08x-%08x, marks %08x/%08x, distance %08x, "
             "write %08x read %08x break %08x",
             (unsigned) cp[0], (unsigned) cp[1], (unsigned) cp32(0x20), (unsigned) cp32(0x24),
             (unsigned) cp32(0x28), (unsigned) cp32(0x2C), (unsigned) cp32(0x30), (unsigned) cp32(0x34),
             (unsigned) cp32(0x38), (unsigned) cp32(0x3C));
    /* (the mailboxes' high halves only: reading a low half takes the mail) */
    WL_LOG_W("melee",
             "stall: PE interrupts %04x token %04x; VI config %04x, interrupts %04x%04x %04x%04x; "
             "DSP control %04x, mail to %04x from %04x; AI control %08x count %08x",
             (unsigned) pe[5], (unsigned) pe[7], (unsigned) vi[1], (unsigned) vi[0x18], (unsigned) vi[0x19],
             (unsigned) vi[0x1A], (unsigned) vi[0x1B], (unsigned) dsp[5], (unsigned) dsp[0], (unsigned) dsp[2],
             (unsigned) ai[0], (unsigned) ai[2]);
}

static void align_note(void);

static void irq_stack_note(void);
static void dsp_note(void);
static void irq_left_note(void);

void melee_wii_stall_tick(void* user)
{
    align_note();
    irq_stack_note();
    dsp_note();
    irq_left_note();
    melee_wii_libc_guard();
    u64 now, last;
    u32 seconds;
    int kind;
    (void) user;
    if (!s_armed || s_reported == s_frames) {
        return;
    }
    now = gettime();
    last = s_frames != 0 ? s_last_frame : s_started;
    if (now <= last) {
        return;
    }
    seconds = ticks_to_secs(now - last);
    if (seconds < STALL_SECONDS) {
        return;
    }
    s_reported = s_frames;
    kind = s_kind;
    WL_LOG_W("melee", "stall: no new frame for %u s (%u frames shown so far)", (unsigned) seconds,
             (unsigned) s_frames);
    seconds = ticks_to_secs(now - (kind != 0 ? s_since : s_last_end));
    switch (kind) {
    case 0: {
        /* where it runs: its registers as the switch to this thread saved
         * them (a KThread starts with its context; an lwp_t is its address
         * inverted) */
        const PPCContext* c = (const PPCContext*) ~(u32) s_main;
        WL_LOG_W("melee", "stall: the game's main thread is in no wait (its last ended %u s ago), at %08x, lr %08x, "
                 "msr %08x",
                 (unsigned) seconds, (unsigned) c->pc, (unsigned) c->lr, (unsigned) c->msr);
        report_calls(c->gpr[1]);
        break;
    }
    case MELEE_WII_WAIT_SLEEP:
        WL_LOG_W("melee", "stall: the game's main thread has waited %u s in OSSleepThread (queue %p)",
                 (unsigned) seconds, s_object);
        break;
    case MELEE_WII_WAIT_RETRACE:
        WL_LOG_W("melee", "stall: the game's main thread has waited %u s for the VI's retrace",
                 (unsigned) seconds);
        break;
    case WAIT_SUSPENDED:
        WL_LOG_W("melee", "stall: the game's main thread has been suspended %u s (by %p)", (unsigned) seconds,
                 s_object);
        break;
    default:
        break;
    }
    if (kind != 0 && kind != WAIT_SUSPENDED) {
        report_calls(s_frame);
    }
    report_hardware();
    {
        void melee_wii_dvd_report(void);
        melee_wii_dvd_report();
    }
    WL_LOG_W("melee", "stall: %u waits so far, %u retraces; sound bank moves under way %d", (unsigned) s_waits,
             (unsigned) VIGetRetraceCount(), *(volatile int*) 0x804D773Cu /* sfxGroupDataReaddressCounter */);
    wl_platform_log_flush();
}

/* ---- a step number on the screen ------------------------------------------

   A hang with interrupts off (the console's bus locked, say) writes no log
   and runs no watchdog, but the video interface keeps showing its frame. So
   the longest section with interrupts off (rollback's setup as a match
   starts: so_exi.c's 50-53, so_rollback.c's 10-21) puts a step number at the
   top left of the frame shown, written into it directly (uncached): the next
   frame drawn covers it, so it stays only when the console stops there. (A
   console froze twice as an online match started, silent, its reset button
   dead; nothing in the log.) */
void melee_wii_screen_mark(unsigned int n)
{
    static const u8 font[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
        { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    };
    volatile u16* vi = (volatile u16*) 0xCC002000u;
    u32 fields[2], stride = (u32) (vi[0x48 / 2] & 0xFF) * 16; /* (HSW's STD: a field's line, in 16 bytes) */
    u8 digit[2] = { (u8) ((n / 10) % 10), (u8) (n % 10) };
    fields[0] = ((u32) vi[0x1C / 2] << 16) | vi[0x1C / 2 + 1];
    fields[1] = ((u32) vi[0x24 / 2] << 16) | vi[0x24 / 2 + 1];
    if (stride < 640 || stride > 4096) {
        return;
    }
    for (int f = 0; f < 2; f++) {
        u32 v = fields[f], addr = (v & 0x00FFFFFFu) << ((v & 0x10000000u) ? 5 : 0);
        if (addr < 0x100000u || addr + 16 * stride > 0x01800000u) {
            continue;
        }
        for (u32 row = 0; row < 14; row++) {
            volatile u32* line = (volatile u32*) (0xC0000000u | (addr + (16 + row) * stride)) + 16;
            for (u32 w = 0; w < 20; w++) {
                u32 white = 0;
                if (row >= 2 && row < 12) {
                    u32 fr = (row - 2) / 2, d = w < 10 ? 0 : 1, x = w - d * 10;
                    if (x >= 2 && x < 8) {
                        white = (font[digit[d]][fr] >> (2 - (x - 2) / 2)) & 1;
                    }
                }
                line[w] = white ? 0xEB80EB80u : 0x10801080u; /* (two pixels: YUYV) */
            }
        }
    }
}

/* ---- a crash ------------------------------------------------------------ */

/* An exception nothing handles goes to libogc's panic function (tuxedo's
 * PPCExcptCurPanicFn), whose screen shows the registers and waits for
 * RESET; while the game starts, the screen is black (the launcher's last
 * VIDEO_SetBlack), so a crash there looked like a hang. This one keeps the
 * crash, then has the crashed thread go on in crash_report, on a stack of its
 * own with interrupts on (the exception's return takes the context it is
 * given): the crash in the log, written out, then libogc's screen, shown. */

#include <string.h>
#include <tuxedo/ppc/exception.h>

extern char _SDA_BASE_[];
extern char _SDA2_BASE_[];

static PPCExcptPanicFn s_ogc_panic;
static volatile u32 s_crashes;
static unsigned s_crash_id;
static PPCContext s_crash;
static u32 s_crash_dar, s_crash_dsisr, s_crash_thread;
static u8 s_crash_stack[32 * 1024] __attribute__((aligned(32)));

static int code_addr(u32 p)
{
    return (p & 3) == 0 && ((p >= 0x80000000u && p < 0x81800000u) || (p >= 0x90000000u && p < 0x94000000u));
}

/* The game's arena's top (os.c): MEM1, which the video interface reads */
void* melee_wii_arena_scratch(unsigned int size);

/* A screen with the crash, in the launcher's video mode (a TV took that one),
 * on a frame buffer in MEM1, with interrupts on (libogc's own crash screen
 * programs the video interface with them off: a Wii lost the TV's signal);
 * RESET or POWER (the system watch thread) leaves */
static void crash_screen(const char* name, const PPCContext* c)
{
    static GXRModeObj wide;
    GXRModeObj* mode = VIDEO_GetPreferredMode(NULL);
    u32 size = VIDEO_GetFrameBufferSize(mode);
    if (CONF_GetAspectRatio() == CONF_ASPECT_16_9) {
        wide = *mode;
        wide.viWidth = 704;
        wide.viXOrigin = (VI_MAX_WIDTH_NTSC - 704) / 2;
        mode = &wide;
    }
    void* fb = MEM_K0_TO_K1(melee_wii_arena_scratch(size));
    VIDEO_Configure(mode);
    VIDEO_ClearFrameBuffer(mode, fb, COLOR_BLACK);
    console_init(fb, 40, 40, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    printf("\n Super Smash Bros. Melee stopped: a crash.\n\n");
    printf(" %s exception at %08x (lr %08x)\n", name, (unsigned) c->pc, (unsigned) c->lr);
    printf(" data address %08x\n\n", (unsigned) s_crash_dar);
    printf(" The details are in apps/melee/logs/log.txt on the SD card.\n\n");
    printf(" Press RESET to go back to the Homebrew Channel.\n");
    VIDEO_SetNextFramebuffer(fb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    for (;;) {
        VIDEO_WaitVSync();
    }
}

static void crash_report(void)
{
    static const char* const names[] = {
        "?", "reset", "machine check", "DSI (data access)", "ISI (instruction fetch)", "external interrupt",
        "alignment", "program (illegal instruction, trap or privileged)", "FPU unavailable", "decrementer",
        "?", "?", "system call", "trace", "?", "performance monitor",
    };
    const PPCContext* c = &s_crash;
    const char* name = s_crash_id < sizeof(names) / sizeof(names[0]) ? names[s_crash_id] : "?";
    WL_LOG_E("melee", "crash: %s exception (%u) at %08x, lr %08x, srr1 %08x, in %s", name, s_crash_id,
             (unsigned) c->pc, (unsigned) c->lr, (unsigned) c->msr,
             s_armed && s_crash_thread == (u32) s_main ? "the game's main thread" : "another thread");
    WL_LOG_E("melee", "crash: data address %08x, DSISR %08x", (unsigned) s_crash_dar, (unsigned) s_crash_dsisr);
    if (code_addr(c->pc - 8) && code_addr(c->pc + 8)) {
        const volatile u32* p = (const volatile u32*) (c->pc - 8);
        WL_LOG_E("melee", "crash: code there: %08x %08x [%08x] %08x %08x", (unsigned) p[0], (unsigned) p[1],
                 (unsigned) p[2], (unsigned) p[3], (unsigned) p[4]);
    }
    for (int i = 0; i < 32; i += 8) {
        WL_LOG_E("melee", "crash: r%-2d %08x %08x %08x %08x %08x %08x %08x %08x", i, (unsigned) c->gpr[i],
                 (unsigned) c->gpr[i + 1], (unsigned) c->gpr[i + 2], (unsigned) c->gpr[i + 3],
                 (unsigned) c->gpr[i + 4], (unsigned) c->gpr[i + 5], (unsigned) c->gpr[i + 6],
                 (unsigned) c->gpr[i + 7]);
    }
    WL_LOG_E("melee", "crash: cr %08x ctr %08x xer %08x", (unsigned) c->cr, (unsigned) c->ctr, (unsigned) c->xer);
    report_calls(c->gpr[1]);
    report_hardware();
    wl_platform_log_flush();
    crash_screen(name, c);
}

/* ---- floating-point loads and stores at an address that isn't word-aligned

   The 750 family doesn't do them: it takes an alignment exception and leaves
   the access to software (IBM's 750GX/GL manuals; Linux's align.c does it).
   Dolphin does them, and code made for it has some (Slippi's online codes:
   floats at +0xA and +0xE of an 18-byte heap block, as the online character
   select starts). Done here, through an aligned word: the register's value
   stored there by the same instruction on it, then copied byte by byte, or
   the other way. The exception came from the instruction itself, so its
   thread has the FPU (its registers live), with interrupts off. */

static volatile u32 s_align_fixes, s_align_first_pc, s_align_first_dar, s_align_logged;
static u32 s_align_stub[2] __attribute__((aligned(32)));
static u8 s_align_word[8] __attribute__((aligned(8)));

/* Runs `insn` (with rA r3, displacement 0) on the aligned word */
static void align_run(u32 insn)
{
    u32 msr;
    s_align_stub[0] = insn;
    s_align_stub[1] = 0x4E800020; /* blr */
    __asm__ volatile("dcbst 0,%0\n\tsync\n\ticbi 0,%0\n\tsync\n\tisync" ::"r"(s_align_stub) : "memory");
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0\n\tisync" ::"r"(msr | 0x2000u) : "memory"); /* FP */
    ((void (*)(void*)) s_align_stub)(s_align_word);
    __asm__ volatile("mtmsr %0\n\tisync" ::"r"(msr) : "memory");
}

static int align_fixup(PPCContext* ctx, u32 dar)
{
    u32 insn, op, frt, ra, size, store, update, run;
    volatile u8* ea = (volatile u8*) dar;
    if (!code_addr(ctx->pc)) {
        return 0;
    }
    insn = *(const volatile u32*) ctx->pc;
    op = insn >> 26;
    frt = (insn >> 21) & 31;
    ra = (insn >> 16) & 31;
    if (op >= 48 && op <= 55) { /* lfs lfsu lfd lfdu stfs stfsu stfd stfdu */
        size = (op & 2) ? 8 : 4;
        store = op >= 52;
        update = op & 1;
        run = ((op & ~1u) << 26) | (frt << 21) | (3u << 16);
    } else if (op == 31) {
        u32 xo = (insn >> 1) & 0x3FF;
        switch (xo) {
        case 535: case 567: size = 4; store = 0; break; /* lfsx lfsux */
        case 599: case 631: size = 8; store = 0; break; /* lfdx lfdux */
        case 663: case 695: size = 4; store = 1; break; /* stfsx stfsux */
        case 727: case 759: size = 8; store = 1; break; /* stfdx stfdux */
        case 983: size = 4; store = 1; break;           /* stfiwx */
        default: return 0;
        }
        update = xo != 983 && (xo & 32) != 0;
        /* the D form of the same access, on r3 */
        if (xo == 983) {
            run = (31u << 26) | (frt << 21) | (3u << 11) | (983u << 1); /* stfiwx frt, 0, r3 */
        } else {
            u32 d_op = size == 4 ? (store ? 52u : 48u) : (store ? 54u : 50u);
            run = (d_op << 26) | (frt << 21) | (3u << 16);
        }
    } else {
        return 0;
    }
    if (store) {
        align_run(run);
        for (u32 i = 0; i < size; i++) {
            ea[i] = s_align_word[i];
        }
    } else {
        for (u32 i = 0; i < size; i++) {
            s_align_word[i] = ea[i];
        }
        align_run(run);
    }
    if (update && ra != 0) {
        ctx->gpr[ra] = dar;
    }
    ctx->pc += 4;
    if (s_align_fixes++ == 0) {
        s_align_first_pc = ctx->pc - 4;
        s_align_first_dar = dar;
    }
    return 1;
}

static volatile u32 s_libc_redirects;

static void panic(unsigned exid, PPCContext* ctx)
{
    {
        /* the game's memset or memcpy called at its old place (main.c) */
        u32 to = melee_wii_libc_moved(ctx->pc);
        if (to != 0 && (exid == PPC_EXCPT_UNDEF || exid == PPC_EXCPT_ISI || exid == PPC_EXCPT_DSI)) {
            ctx->pc = to;
            s_libc_redirects++;
            return;
        }
    }
    if (exid == PPC_EXCPT_ALIGN) {
        u32 dar;
        __asm__ volatile("mfspr %0, 19" : "=r"(dar));
        if (align_fixup(ctx, dar)) {
            return;
        }
    }
    if (s_crashes++ != 0 || s_ogc_panic == NULL) {
        if (s_ogc_panic != NULL) {
            s_ogc_panic(exid, ctx);
        }
        return;
    }
    __asm__ volatile("mfspr %0, 19" : "=r"(s_crash_dar));
    __asm__ volatile("mfspr %0, 18" : "=r"(s_crash_dsisr));
    s_crash_id = exid;
    s_crash_thread = (u32) LWP_GetSelf();
    /* (the exception's frame has the integer registers only) */
    memcpy(&s_crash, ctx, offsetof(PPCContext, fpscr));
    ctx->pc = (u32) crash_report;
    ctx->lr = 0;
    ctx->gpr[1] = ((u32) s_crash_stack + sizeof(s_crash_stack) - 64) & ~15u;
    ctx->gpr[2] = (u32) _SDA2_BASE_;
    ctx->gpr[13] = (u32) _SDA_BASE_;
    ctx->msr |= 0x8000u | 0x2000u | 0x2u; /* EE, FP, RI */
}

/* ---- the interrupts' stack

   libogc's kernel runs every interrupt handler on one stack of its own,
   16 KB, just above the main thread's (default_stacks.c). The game's audio
   runs in its interrupt (AX's frame, the synth and its sequencer, the
   reverbs), which on the GameCube ran on the stack of whatever thread it
   interrupted; one that went past those 16 KB would write over the top of
   the main thread's stack. This one is 64 KB (libogc's pointer to it is
   weak), filled with a pattern: the watch thread notes how deep the
   interrupts have gone, and whether past its end. */
#define IRQ_STACK_SIZE (64 * 1024)
#define IRQ_STACK_FILL 0x53544B21u
static u32 s_irq_stack[IRQ_STACK_SIZE / 4] __attribute__((aligned(32)));
void* __ppc_excpt_sp = (u8*) s_irq_stack + IRQ_STACK_SIZE;
static u32 s_irq_deepest, s_irq_noted_kb, s_irq_ticks, s_irq_over;

static void irq_stack_fill(void)
{
    u32 level = IRQ_Disable(); /* (nothing on it meanwhile) */
    for (u32 i = 0; i < IRQ_STACK_SIZE / 4 - 128; i++) {
        s_irq_stack[i] = IRQ_STACK_FILL;
    }
    IRQ_Restore(level);
}

static void irq_stack_note(void)
{
    u32 i = 0, depth;
    if (++s_irq_ticks % 32 != 0) {
        return;
    }
    while (i < IRQ_STACK_SIZE / 4 && s_irq_stack[i] == IRQ_STACK_FILL) {
        i++;
    }
    if (i == 0 && !s_irq_over) {
        s_irq_over = 1;
        WL_LOG_E("melee", "irq stack: the interrupts went past the end of their %u KB stack",
                 (unsigned) (IRQ_STACK_SIZE / 1024));
    }
    depth = IRQ_STACK_SIZE - i * 4;
    if (depth > s_irq_deepest) {
        s_irq_deepest = depth;
        /* (logged from 8 KB, at each new 2 KB) */
        if (depth >= 8 * 1024 && depth / 2048 > s_irq_noted_kb / 2) {
            s_irq_noted_kb = depth / 1024;
            WL_LOG_I("melee", "irq stack: the interrupts have used up to %u bytes of their stack (%u KB; "
                     "libogc's was 16 KB)", (unsigned) depth, (unsigned) (IRQ_STACK_SIZE / 1024));
        }
    }
}

/* ---- the DSP's mailboxes

   The SDK's waits on them spin with interrupts off: AX's frame (in the
   audio's interrupt) waits for the DSP to take each mail, the DSP's
   interrupt handler for the DSP's mail. A DSP that stopped would leave the
   CPU there for good: the console frozen, the audio looping its last 5 ms,
   the reset button dead, the log silent. The SDK's AXOut.c and dsp_task.c
   ask here instead (cmake/Wii.cmake): a wait gives up after 50 ms (2 ms
   once one has), noted for the log by the watch thread. Only the high
   halves are read: reading a low half takes the mail. */
#define DSP_REGS ((volatile u16*) 0xCC005000)
static u64 s_dsp_since;
static int s_dsp_waiting;
static volatile u32 s_dsp_gave_up, s_dsp_noted, s_dsp_where;
static volatile u16 s_dsp_csr, s_dsp_to, s_dsp_from;

static int dsp_wait(int busy, u32 where)
{
    u64 now;
    if (!busy) {
        s_dsp_waiting = 0;
        return 0;
    }
    now = gettime();
    if (!s_dsp_waiting) {
        s_dsp_waiting = 1;
        s_dsp_since = now;
        return 1;
    }
    if (ticks_to_millisecs(now - s_dsp_since) < (s_dsp_gave_up != 0 ? 2u : 50u)) {
        return 1;
    }
    s_dsp_waiting = 0;
    if (s_dsp_gave_up++ == 0) {
        s_dsp_where = where;
        s_dsp_csr = DSP_REGS[5];
        s_dsp_to = DSP_REGS[0];
        s_dsp_from = DSP_REGS[2];
    }
    return 0;
}

/* DSPCheckMailToDSP: 1 while the DSP has not taken the last mail */
u32 melee_wii_dsp_mail_to_dsp(void)
{
    return (u32) dsp_wait((DSP_REGS[0] & 0x8000) != 0, (u32) __builtin_return_address(0));
}

/* DSPCheckMailFromDSP: 1 once the DSP has sent mail */
u32 melee_wii_dsp_mail_from_dsp(void)
{
    return (u32) !dsp_wait((DSP_REGS[2] & 0x8000) == 0, (u32) __builtin_return_address(0));
}

static void dsp_note(void)
{
    u32 n = s_dsp_gave_up;
    if (n != 0 && s_dsp_noted == 0) {
        s_dsp_noted = n;
        WL_LOG_E("melee", "dsp: the DSP answered no mail for 50 ms (the wait at %08x; DSP control %04x, mail to "
                 "it %04x...., from it %04x....); later waits give up after 2 ms",
                 (unsigned) s_dsp_where, (unsigned) s_dsp_csr, (unsigned) s_dsp_to, (unsigned) s_dsp_from);
    } else if (n >= s_dsp_noted * 16 && n > s_dsp_noted) {
        s_dsp_noted = n;
        WL_LOG_E("melee", "dsp: %u waits given up so far", (unsigned) n);
    }
}

/* ---- interrupts left off

   The game's frame waits for the controllers' next poll in a loop
   (gmscene.c: lb_80019894, the pads' queue count, until one has come, from
   0x801A4DAC), which interrupts fill: never with them off. Were they left
   off by something before, it would wait there for good: the console
   frozen, the audio looping its last 5 ms, the reset button dead, the log
   silent. The count's own OSDisableInterrupts comes here (cmake/Wii.cmake,
   --redirect): called from that wait, it says they were on, so its
   OSRestoreInterrupts turns them on, and notes it for the log (the watch
   thread). Slippi's code also calls the count, with them off on purpose
   (its frame hook, after a load): left as they are. */
#define FRAME_WAIT_RETURN 0x801A4DB0u
static volatile u32 s_irq_left_off, s_irq_left_noted, s_irq_left_frames;
/* the ring of interrupts' turns (os.c) as it was then, oldest first */
extern volatile u32 melee_wii_irq_ring[128][2], melee_wii_irq_ring_next;
static u32 s_irq_left_ring[128][2];

static int stack_word(const u32* sp)
{
    return sp != NULL && ((u32) sp & 7) == 0 && (u32) sp >= 0x80000000u && (u32) sp < 0x94000000u;
}

/* lb_80019894's caller: up the back chain from this frame to lb_80019894's,
 * then its caller's, where lb_80019894 saved its LR (MWCC's prologue) */
static u32 __attribute__((noinline)) count_caller(const u32* sp)
{
    for (int k = 0; k < 2; k++) {
        if (!stack_word(sp)) {
            return 0;
        }
        sp = (const u32*) sp[0];
    }
    return stack_word(sp) ? sp[1] : 0;
}
static volatile u32 s_irq_other_caller;

BOOL melee_wii_frame_wait_irq_off(void)
{
    u32 msr;
    __asm__ volatile("mfmsr %0" : "=r"(msr));
    __asm__ volatile("mtmsr %0" ::"r"(msr & ~0x8000u) : "memory");
    if (msr & 0x8000u) {
        return TRUE;
    }
    {
        u32 caller = count_caller((const u32*) __builtin_frame_address(0));
        if (caller != FRAME_WAIT_RETURN) {
            if (s_irq_other_caller == 0) {
                s_irq_other_caller = caller;
            }
            return FALSE;
        }
    }
    if (s_irq_left_off++ == 0) {
        u32 next = melee_wii_irq_ring_next;
        s_irq_left_frames = s_frames;
        for (u32 i = 0; i < 128; i++) {
            s_irq_left_ring[i][0] = melee_wii_irq_ring[(next + i) % 128][0];
            s_irq_left_ring[i][1] = melee_wii_irq_ring[(next + i) % 128][1];
        }
    }
    return TRUE;
}

static void irq_left_note(void)
{
    static int other_noted;
    u32 n = s_irq_left_off;
    if (s_irq_other_caller != 0 && !other_noted) {
        other_noted = 1;
        WL_LOG_I("melee", "irq: the pads' count called with interrupts off, from %08x (Slippi's code: left as "
                 "they are)", (unsigned) s_irq_other_caller);
    }
    if (n != s_irq_left_noted && (s_irq_left_noted == 0 || n >= s_irq_left_noted * 16)) {
        int first = s_irq_left_noted == 0;
        s_irq_left_noted = n;
        WL_LOG_E("melee", "irq: the frame's wait found interrupts off %u time(s) (the first after %u frames "
                 "shown) and turned them on again",
                 (unsigned) n, (unsigned) s_irq_left_frames);
        if (first) {
            /* the last turns before: caller/kind (1 disable off, 2 restore
             * on, 3 restore off, 4 enable on, 1xx Slippi's command xx) */
            for (int i = 128 - 48; i < 128; i += 8) {
                WL_LOG_E("melee", "irq:   %08x/%x %08x/%x %08x/%x %08x/%x %08x/%x %08x/%x %08x/%x %08x/%x",
                         (unsigned) s_irq_left_ring[i][0], (unsigned) s_irq_left_ring[i][1],
                         (unsigned) s_irq_left_ring[i + 1][0], (unsigned) s_irq_left_ring[i + 1][1],
                         (unsigned) s_irq_left_ring[i + 2][0], (unsigned) s_irq_left_ring[i + 2][1],
                         (unsigned) s_irq_left_ring[i + 3][0], (unsigned) s_irq_left_ring[i + 3][1],
                         (unsigned) s_irq_left_ring[i + 4][0], (unsigned) s_irq_left_ring[i + 4][1],
                         (unsigned) s_irq_left_ring[i + 5][0], (unsigned) s_irq_left_ring[i + 5][1],
                         (unsigned) s_irq_left_ring[i + 6][0], (unsigned) s_irq_left_ring[i + 6][1],
                         (unsigned) s_irq_left_ring[i + 7][0], (unsigned) s_irq_left_ring[i + 7][1]);
            }
        }
    }
}

void melee_wii_crash_install(void)
{
    if (s_ogc_panic == NULL) {
        s_ogc_panic = PPCExcptCurPanicFn;
        PPCExcptCurPanicFn = panic;
        irq_stack_fill();
    }
}

/* The fix-ups, in the log once (the watch thread: the exception can't log) */
static void align_note(void)
{
    if (s_align_fixes != 0 && !s_align_logged) {
        s_align_logged = 1;
        WL_LOG_I("melee", "align: a floating-point access at %08x to %08x (not word-aligned) done in software, "
                 "as the 750 needs; such accesses are done that way from now on",
                 (unsigned) s_align_first_pc, (unsigned) s_align_first_dar);
    }
}
