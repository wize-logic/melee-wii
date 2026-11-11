/* The SDK's interrupt handlers (GX's PE token, PE finish and CP FIFO
 * handlers) on libogc's dispatcher. The SDK numbers the interrupts its own
 * way; libogc 3.1 (tuxedo) numbers them in the order of its dispatch.
 *
 * This file includes libogc's headers and not the SDK's: the SDK's types are
 * spelled out (an __OSInterrupt is an s16, a handler takes it and an
 * OSContext*). */

#include <stddef.h>
#include <stdint.h>

#include <tuxedo/interrupt.h>
#include <tuxedo/tick.h>

#include "melee_wii.h"

typedef int16_t SdkInterrupt;
typedef void (*SdkInterruptHandler)(SdkInterrupt interrupt, void* context);

#define SDK_INTERRUPT_COUNT 27

/* Index: the SDK's __OS_INTERRUPT_*; value: libogc's KIrqId */
static const uint8_t s_irq_of[SDK_INTERRUPT_COUNT] = {
    IRQ_MEM0,       IRQ_MEM1,      IRQ_MEM2,         IRQ_MEM3,
    IRQ_MEMADDRESS, IRQ_DSP_AI,    IRQ_DSP_ARAM,     IRQ_DSP_DSP,
    IRQ_AI,         IRQ_EXI0_EXI,  IRQ_EXI0_TC,      IRQ_EXI0_EXT,
    IRQ_EXI1_EXI,   IRQ_EXI1_TC,   IRQ_EXI1_EXT,     IRQ_EXI2_EXI,
    IRQ_EXI2_TC,    IRQ_PI_CP,     IRQ_PI_PETOKEN,   IRQ_PI_PEFINISH,
    IRQ_PI_SI,      IRQ_PI_DI,     IRQ_PI_RSW,       IRQ_PI_ERROR,
    IRQ_PI_VI,      IRQ_PI_DEBUG,  IRQ_PI_HSP,
};

static SdkInterruptHandler s_handlers[SDK_INTERRUPT_COUNT];

/* The OSContext the SDK's handlers are given: they only pass it back to
 * OSSetCurrentContext (os.c) */
static uint8_t s_context[0x2C8] __attribute__((aligned(8)));

/* A test (IRQ_AUDIT 1): the savestates' pages each handler writes, logged
 * (wii/src/mmu_track.c's audit; so_rollback_covers) */
#define IRQ_AUDIT 0
#if IRQ_AUDIT
void melee_wii_mmu_audit_begin(void);
int melee_wii_mmu_audit_end(uint32_t* out, int max);
int so_rollback_covers(uint32_t addr) __attribute__((weak));
void so_log(const char* fmt, ...) __attribute__((weak));
int so_rollback_covers_word(uint32_t addr) __attribute__((weak));
#define WATCH_PAGES 16
static uint32_t s_watch[WATCH_PAGES];
static int s_watch_count;
static uint32_t s_before[WATCH_PAGES][1024];
static uint32_t s_audit_seen[256];
static int s_audit_count;

static void audit_before(void)
{
    for (int w = 0; w < s_watch_count; w++) {
        const uint32_t* p = (const uint32_t*) s_watch[w];
        for (int i = 0; i < 1024; i++) {
            s_before[w][i] = p[i];
        }
    }
}

/* the words of the watched pages the handler changed, in the savestates;
 * new pages it marked changed watched from the next time */
static void audit_after(SdkInterrupt interrupt, const uint32_t* pages, int n)
{
    for (int w = 0; w < s_watch_count; w++) {
        const uint32_t* p = (const uint32_t*) s_watch[w];
        for (int i = 0; i < 1024; i++) {
            uint32_t addr = s_watch[w] + i * 4, key = addr | (uint32_t) (interrupt & 3), k;
            if (p[i] == s_before[w][i] || so_rollback_covers_word == NULL || !so_rollback_covers_word(addr)) {
                continue;
            }
            for (k = 0; k < (uint32_t) s_audit_count && s_audit_seen[k] != key; k++) {
            }
            if (k < (uint32_t) s_audit_count || s_audit_count >= 256) {
                continue;
            }
            s_audit_seen[s_audit_count++] = key;
            if (so_log != NULL) {
                so_log("irq audit: interrupt %d's handler changed %08x (in the savestates): %08x -> %08x",
                       (int) interrupt, (unsigned) addr, (unsigned) s_before[w][i], (unsigned) p[i]);
            }
        }
    }
    for (int i = 0; i < n; i++) {
        int k;
        if (so_rollback_covers == NULL || !so_rollback_covers(pages[i])) {
            continue;
        }
        for (k = 0; k < s_watch_count && s_watch[k] != pages[i]; k++) {
        }
        if (k == s_watch_count && s_watch_count < WATCH_PAGES) {
            s_watch[s_watch_count++] = pages[i];
            if (so_log != NULL) {
                so_log("irq audit: interrupt %d marked page %08x; watching it", (int) interrupt,
                       (unsigned) pages[i]);
            }
        }
    }
}
#endif

static void trampoline(KIrqId id, void* user)
{
    (void) id;
    SdkInterrupt interrupt = (SdkInterrupt) (uintptr_t) user;
    SdkInterruptHandler handler = s_handlers[interrupt];
    if (handler != NULL) {
#if IRQ_AUDIT
        uint32_t pages[32];
        int n;
        melee_wii_mmu_audit_begin();
        audit_before();
        handler(interrupt, s_context);
        n = melee_wii_mmu_audit_end(pages, 32);
        audit_after(interrupt, pages, n);
#else
        handler(interrupt, s_context);
#endif
    }
}

SdkInterruptHandler __OSSetInterruptHandler(SdkInterrupt interrupt,
                                            SdkInterruptHandler handler)
{
    if (interrupt < 0 || interrupt >= SDK_INTERRUPT_COUNT) {
        return NULL;
    }
    SdkInterruptHandler old = s_handlers[interrupt];
    s_handlers[interrupt] = handler;
    KIrqSet((KIrqId) s_irq_of[interrupt], handler != NULL ? trampoline : NULL,
            (void*) (uintptr_t) interrupt);
    return old;
}

/* The audio's two interrupts (the AI DMA's block boundary and the DSP's
 * mail), handled now when they are pending and enabled, as the dispatcher
 * would once interrupts are back on. For code the game calls with its
 * interrupts off for longer than an audio block's 5 ms (rollback's
 * savestates and loads, one after the other): the handlers set the DMA's
 * next block and start the DSP on the one after, which waiting would leave
 * to a block played twice. The interrupts are off: the handlers acknowledge
 * their own, so the dispatcher finds nothing left. Returns how many ran. */
#define DSPCSR (*(volatile uint16_t*) 0xCC00500A)
#define DSPCSR_AIDINT 0x0008
#define DSPCSR_AIDINTMSK 0x0010
#define DSPCSR_DSPINT 0x0080
#define DSPCSR_DSPINTMSK 0x0100
#define SDK_INTERRUPT_DSP_AI 5
#define SDK_INTERRUPT_DSP_DSP 7

int melee_wii_irq_service_audio(void)
{
    int ran = 0;
    for (int pass = 0; pass < 3; pass++) {
        int any = 0;
        uint16_t csr = DSPCSR;
        if ((csr & DSPCSR_AIDINT) && (csr & DSPCSR_AIDINTMSK) && s_handlers[SDK_INTERRUPT_DSP_AI] != NULL) {
            s_handlers[SDK_INTERRUPT_DSP_AI](SDK_INTERRUPT_DSP_AI, s_context);
            any++;
        }
        csr = DSPCSR;
        if ((csr & DSPCSR_DSPINT) && (csr & DSPCSR_DSPINTMSK) && s_handlers[SDK_INTERRUPT_DSP_DSP] != NULL) {
            s_handlers[SDK_INTERRUPT_DSP_DSP](SDK_INTERRUPT_DSP_DSP, s_context);
            any++;
        }
        if (any == 0) {
            break;
        }
        ran += any;
    }
    return ran;
}

SdkInterruptHandler __OSGetInterruptHandler(SdkInterrupt interrupt)
{
    if (interrupt < 0 || interrupt >= SDK_INTERRUPT_COUNT) {
        return NULL;
    }
    return s_handlers[interrupt];
}

/* An SDK mask (0x80000000 >> its number) as libogc's (PPCBIT(KIrqId)) */
static uint32_t ogc_mask(uint32_t sdk_mask)
{
    uint32_t mask = 0;
    for (int i = 0; i < SDK_INTERRUPT_COUNT; i++) {
        if (sdk_mask & (0x80000000u >> i)) {
            mask |= IRQMASK(s_irq_of[i]);
        }
    }
    return mask;
}

static uint32_t s_masked;

uint32_t __OSMaskInterrupts(uint32_t mask)
{
    uint32_t old = s_masked;
    s_masked |= mask;
    KIrqDisable(ogc_mask(mask));
    return old;
}

uint32_t __OSUnmaskInterrupts(uint32_t mask)
{
    uint32_t old = s_masked;
    s_masked &= ~mask;
    KIrqEnable(ogc_mask(mask));
    return old;
}

/* melee_wii.h's deferred work */
_Static_assert(sizeof(((MeleeWiiDefer*) 0)->task) >= sizeof(KTickTask),
               "MeleeWiiDefer holds a KTickTask");

static void defer_run(KTickTask* task)
{
    MeleeWiiDefer* defer = (MeleeWiiDefer*) task;
    defer->armed = 0;
    defer->fn(defer->arg);
}

void melee_wii_defer(MeleeWiiDefer* defer)
{
    PPCIrqState level = PPCIrqLockByMsr();
    if (!defer->armed) {
        defer->armed = 1;
        /* a microsecond on: the decrementer's interrupt, not this call */
        KTickTaskStart((KTickTask*) defer->task, defer_run, PPCUsToTicks(1), 0);
    }
    PPCIrqUnlockByMsr(level);
}
