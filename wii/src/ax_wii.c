/* The SDK's AX on the Wii's DSP: its sample addresses. The GameCube's DSP
 * reads a voice's samples out of ARAM; the Wii has none, and in Wii mode the
 * DSP's accelerator reads main memory instead, MEM2 above 0x10000000. The
 * game's ARAM is 16 MiB of MEM2 (ar.c), its addresses offsets into it: they
 * are moved into MEM2 here, after AX has copied them to the parameter blocks
 * the DSP reads, and the DSP's position is moved back where AX has copied it
 * to the game's voice (the game compares it with its own ARAM addresses).
 *
 * The SDK's __AXSyncPBs (AXVPB.c, built as __AXSyncPBs_sdk) does the copies;
 * this runs after it. Addresses below 16 MiB, in the voice's units, are the
 * game's, the DSP's are above: 2 x 0x10000000 nibbles for ADPCM, 0x08000000
 * samples for PCM16, 0x10000000 bytes for PCM8.
 *
 * Before it, one more thing the GameCube's disc never brought out: a voice
 * given its whole address block (AXSetVoiceAddr) and then one of its addresses
 * (AXSetVoiceCurrentAddr, EndAddr, LoopAddr, AXSetVoiceLoop) in the same 5 ms
 * frame gets only the later fields, the SDK's sync preferring them: its loop
 * flag and format never reach the DSP. A music stream does both, the header's
 * block as its header arrives and the first block's addresses as that block
 * arrives; from the GameCube's disc those were frames apart, from the SD card
 * they can be one, and the music then stops at the end of its first block (its
 * loop flag 0). The voice's own block holds each field's latest value, so it is
 * copied whole. */

#include "melee_wii.h"

#include <dolphin/ax.h>
#include <dolphin/os.h>

#include "__ax.h"

#define ARAM_SIZE (16 * 1024 * 1024)

#define FORMAT_ADPCM 0x00
#define FORMAT_PCM16 0x0A
#define FORMAT_PCM8 0x19

u8* melee_wii_aram(void);
void __AXSyncPBs_sdk(u32 lessDspCycles);

/* The offset of the DSP's addresses from the game's, and the end of the
 * game's, in a format's units */
static BOOL units(u16 format, u32 base, u32* offset, u32* limit)
{
    switch (format) {
    case FORMAT_ADPCM:
        *offset = base * 2;
        *limit = ARAM_SIZE * 2;
        return TRUE;
    case FORMAT_PCM16:
        *offset = base / 2;
        *limit = ARAM_SIZE / 2;
        return TRUE;
    case FORMAT_PCM8:
        *offset = base;
        *limit = ARAM_SIZE;
        return TRUE;
    default:
        return FALSE;
    }
}

static inline u32 get(const u16* hi)
{
    return ((u32) hi[0] << 16) | hi[1];
}

static inline void set(u16* hi, u32 v)
{
    hi[0] = v >> 16;
    hi[1] = (u16) v;
}

static void to_dsp(u16* field, u32 offset, u32 limit)
{
    u32 v = get(field);
    if (v < limit) {
        set(field, v + offset);
    }
}

static void from_dsp(u16* field, u32 offset)
{
    u32 v = get(field);
    if (v >= offset) {
        set(field, v - offset);
    }
}

#define SYNC_ADDR_FIELDS                                                       \
    (AX_SYNC_FLAG_COPYLOOP | AX_SYNC_FLAG_COPYLOOPADDR |                       \
     AX_SYNC_FLAG_COPYENDADDR | AX_SYNC_FLAG_COPYCURADDR)

void __AXSyncPBs(u32 lessDspCycles)
{
    static BOOL s_running;
    for (u32 priority = 1; priority < AX_PRIORITY_STACKS; priority++) {
        for (AXVPB* p = __AXGetStackHead(priority); p != NULL; p = p->next) {
            if ((p->sync & AX_SYNC_FLAG_COPYADDR) && (p->sync & SYNC_ADDR_FIELDS)) {
                p->sync &= ~SYNC_ADDR_FIELDS;
            }
        }
    }
    __AXSyncPBs_sdk(lessDspCycles);
    if (!s_running) {
        s_running = TRUE;
        OSReport("AX: the DSP runs (its first audio frame)\n");
    }

    u8* aram = melee_wii_aram();
    if (aram == NULL) {
        return;
    }
    u32 base = (u32) aram & 0x3FFFFFFF;
    u32 offset, limit;

    AXPB* pbs = __AXGetPBs();
    for (int i = 0; i < AX_MAX_VOICES; i++) {
        AXPBADDR* addr = &pbs[i].addr;
        if (units(addr->format, base, &offset, &limit)) {
            to_dsp(&addr->loopAddressHi, offset, limit);
            to_dsp(&addr->endAddressHi, offset, limit);
            to_dsp(&addr->currentAddressHi, offset, limit);
        }
    }
    for (u32 priority = 0; priority < 32; priority++) {
        for (AXVPB* p = __AXGetStackHead(priority); p != NULL; p = p->next) {
            AXPBADDR* addr = &p->pb.addr;
            if (units(addr->format, base, &offset, &limit)) {
                from_dsp(&addr->currentAddressHi, offset);
            }
        }
    }
    DCFlushRange(pbs, AX_MAX_VOICES * sizeof(AXPB));
}

/* Around a long time with interrupts off (rollback's savestates, which the
 * game asks for with its interrupts off): the audio DMA plays a block, then
 * the block whose address is set, again and again until AX's interrupt sets
 * the next: one 5 ms block over and over is a buzz. Held, the address set
 * is silence's; released before the block ends, it is set back (nothing
 * missed), else it is left for AX's interrupt to set the next. */
#define AI_REGS ((volatile u16*) 0xCC005000)
#define AI_DSPCSR 5
#define AI_DSPCSR_AIDINT 0x0008
#define AI_START_HI 24
#define AI_START_LO 25
#define AI_LEN 27

static u8 s_silence[4096] ATTRIBUTE_ALIGN(32);
static u16 s_held_hi, s_held_lo;
static int s_hold_depth;
static BOOL s_swapped; /* silence's address set by the hold */

void melee_wii_audio_hold(void)
{
    BOOL level = OSDisableInterrupts();
    if (s_hold_depth++ == 0) {
        u16 len = AI_REGS[AI_LEN];
        s_swapped = (len & 0x8000) && (len & 0x7FFF) <= sizeof(s_silence) / 32;
        if (s_swapped) {
            u32 pa = (u32) s_silence & 0x3FFFFFFF;
            s_held_hi = AI_REGS[AI_START_HI];
            s_held_lo = AI_REGS[AI_START_LO];
            AI_REGS[AI_START_HI] = (s_held_hi & 0xE000) | (pa >> 16);
            AI_REGS[AI_START_LO] = (s_held_lo & 0x001F) | (pa & 0xFFE0);
        }
    }
    OSRestoreInterrupts(level);
}

void melee_wii_audio_release(void)
{
    BOOL level = OSDisableInterrupts();
    if (s_hold_depth > 0 && --s_hold_depth == 0 && s_swapped) {
        s_swapped = FALSE;
        if (!(AI_REGS[AI_DSPCSR] & AI_DSPCSR_AIDINT)) {
            AI_REGS[AI_START_HI] = s_held_hi;
            AI_REGS[AI_START_LO] = s_held_lo;
        }
    }
    OSRestoreInterrupts(level);
}

/* A rollback's savestates and loads, 3 to 4 ms each with interrupts off
 * and one straight after the other in a frame that goes back: held as
 * above, each one that a block's start fell in played silence in a good
 * block's place (3 in 5 savestates, 3 in 4 loads: the clicks of a
 * rollback, 2026-11-10). Instead:
 *
 * - service, before and after each: the audio's interrupts are handled
 *   there and then when pending (os_irq.c), so AX sets the next block and
 *   starts the DSP on the one after without waiting for the game to turn
 *   its interrupts back on, a savestate later.
 * - guard, between the steps of one: nothing while no block has begun
 *   unhandled (the block playing is the right one, and the next is set
 *   once the handler runs, within the 5 ms). Once one has, the address
 *   set is still its own: silence is set in its place, for the case that
 *   this one runs past the block's end too (longer than 5 ms in all),
 *   where the block would play twice; the service after sets the real
 *   next block over it when it comes in time. */
static u32 s_serviced, s_guarded, s_silent;

void melee_wii_audio_guard(void)
{
    u16 len = AI_REGS[AI_LEN];
    if (s_hold_depth != 0 || s_swapped || !(AI_REGS[AI_DSPCSR] & AI_DSPCSR_AIDINT)) {
        return;
    }
    if ((len & 0x8000) && (len & 0x7FFF) <= sizeof(s_silence) / 32) {
        u32 pa = (u32) s_silence & 0x3FFFFFFF;
        s_held_hi = AI_REGS[AI_START_HI];
        s_held_lo = AI_REGS[AI_START_LO];
        AI_REGS[AI_START_HI] = (s_held_hi & 0xE000) | (pa >> 16);
        AI_REGS[AI_START_LO] = (s_held_lo & 0x001F) | (pa & 0xFFE0);
        s_swapped = TRUE;
        s_guarded++;
    }
}

void melee_wii_audio_service(void)
{
    if (s_hold_depth != 0) {
        return;
    }
    s_serviced += (u32) melee_wii_irq_service_audio();
    if (s_swapped) {
        /* (AX's handler sets the next block over the silence; when it has
         * not, the DSP's frame not yet done, silence stays: the block that
         * would have played twice) */
        u32 pa = (u32) s_silence & 0x3FFFFFFF;
        if ((AI_REGS[AI_START_HI] & 0x1FFF) == (pa >> 16) && (AI_REGS[AI_START_LO] & 0xFFE0) == (pa & 0xFFE0)) {
            s_silent++;
        }
        s_swapped = FALSE;
    }
}

void melee_wii_audio_counts(unsigned int* serviced, unsigned int* guarded, unsigned int* silent)
{
    *serviced = s_serviced;
    *guarded = s_guarded;
    *silent = s_silent;
}
