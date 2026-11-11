/* The SDK's VI on libogc's VIDEO: libogc programs the Wii's video encoder,
 * which the GameCube's VI library knows nothing of. GXRenderModeObj and
 * libogc's GXRModeObj are the same structure. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <dolphin/gx/GXStruct.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

static BOOL s_initialized;
static volatile u32 s_retrace_count;
static VIRetraceCallback s_pre_retrace;

/* wii/src/gecko.c: the player's cheats, each frame (when there) */
void melee_wii_gecko_frame(void) __attribute__((weak));
/* wii/src/slippi/so_gamelog.c: the launcher's Diagnostics, while an online
 * game is recorded */
void so_gamelog_retrace(void) __attribute__((weak));
/* wii/src/diag_dirty.c, in test builds */
void melee_wii_diag_frame(void) __attribute__((weak));

/* The end of the GameCube's retrace handler (__VIRetraceHandler): it wakes the
 * threads waiting in its queue. The port's VIWaitForRetrace waits in libogc
 * and the game sleeps in that queue nowhere else, but a mod's code can: 20XX's
 * lag reduction sleeps in it until the frame's second poll of the
 * controllers, and puts a step of its own before the wake-up, in place of the
 * instruction there (a branch at 0x8034EB60 instead of "addi r3, r13,
 * retraceQueue"): with lag reduction on, when no poll came since the last
 * retrace, "not polled this frame" (and a timing value of its own); then the
 * frame's count of polls back to 0. Its poll callback counts them
 * (pad.c, SIRegisterPollingHandler). */
#define RETRACE_QUEUE ((OSThreadQueue*) 0x804D7428)
#define RETRACE_WAKE_SITE 0x8034EB60u
#define X20XX_LAG_STEP 0x80331584u
#define X20XX_LAG_ON 0x80228914u
#define X20XX_POLLS 0x804DA0D0u     /* r2 - 22800 */
#define X20XX_POLLED 0x804DA0D4u    /* r2 - 22796 */
#define X20XX_TIMING 0x80432A42u

static BOOL x20xx_lag_step(void)
{
    static const u32 start[4] = { 0x3DE08023, 0x81EF8914, 0x2C0F0000, 0x41A20030 };
    const volatile u32* site = (const volatile u32*) RETRACE_WAKE_SITE;
    const volatile u32* step = (const volatile u32*) X20XX_LAG_STEP;
    if (*site != (0x48000000u | ((X20XX_LAG_STEP - RETRACE_WAKE_SITE) & 0x03FFFFFCu))) {
        return FALSE;
    }
    for (int i = 0; i < 4; i++) {
        if (step[i] != start[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void retrace_end(void)
{
    if (x20xx_lag_step() && *(volatile u32*) X20XX_LAG_ON != 0) {
        if (*(volatile s32*) X20XX_POLLS < 1) {
            *(volatile u32*) X20XX_POLLED = 0;
            *(volatile u16*) X20XX_TIMING = 0x95FC;
        }
        *(volatile s32*) X20XX_POLLS = 0;
    }
    OSWakeupThread(RETRACE_QUEUE);
}

/* libogc 3.1 counts no retraces: the count is kept here, before the game's
 * own pre-retrace callback */
static void pre_retrace(u32 count)
{
    (void) count;
    s_retrace_count++;
    if (melee_wii_gecko_frame != NULL) {
        melee_wii_gecko_frame();
    }
    if (melee_wii_diag_frame != NULL) {
        melee_wii_diag_frame();
    }
    if (so_gamelog_retrace != NULL) {
        so_gamelog_retrace();
    }
    if (s_pre_retrace != NULL) {
        s_pre_retrace(s_retrace_count);
    }
    retrace_end();
}

void VIInit(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = TRUE;
    VIDEO_Init();
    VIDEO_SetPreRetraceCallback(pre_retrace);
}

/* The Wii's 16:9 setting: the picture over 704 pixels of the line, centred
 * (a 16:9 TV's clean aperture; the interface widens the 640-pixel frame
 * buffer to it), as Wii programs show 16:9; the game's own modes are 640.
 * The game's object is left as it is. */
void VIConfigure(GXRenderModeObj* rm)
{
    GXRenderModeObj mode = *rm;
    if (CONF_GetAspectRatio() == 1) {
        mode.viWidth = 704;
        mode.viXOrigin = (720 - 704) / 2;
    }
    VIDEO_Configure(&mode);
}

void VIFlush(void)
{
    VIDEO_Flush();
}

void VISetNextFrameBuffer(void* fb)
{
    VIDEO_SetNextFramebuffer(fb);
    melee_wii_stall_frame();
}

void VISetBlack(BOOL black)
{
    VIDEO_SetBlack(black != FALSE);
}

void VIWaitForRetrace(void)
{
    melee_wii_wait_begin(MELEE_WII_WAIT_RETRACE, NULL, __builtin_frame_address(0));
    VIDEO_WaitVSync();
    melee_wii_wait_end();
}

u32 VIGetRetraceCount(void)
{
    return s_retrace_count;
}

u32 VIGetNextField(void)
{
    return VIDEO_GetNextField();
}

u32 VIGetTvFormat(void)
{
    /* The game is the NTSC release: VI_NTSC, VI_PAL, VI_MPAL are numbered
     * alike in libogc; a Wii set to EURGB60 shows the NTSC timing */
    u32 mode = VIDEO_GetCurrentTvMode();
    return mode <= 2 ? mode : 0;
}

u32 VIGetDTVStatus(void)
{
    return VIDEO_HaveComponentCable();
}

/* The boot's question (gm_801A4510 asks "Enable progressive scan display?"
 * with a component cable, when progressive scan was chosen before or B is
 * held), answered by the Wii's own setting, as Wii games go by it: with the
 * Wii set to 480p and a component cable, the game is put in 480p as its
 * "Yes" does it (gm_Scene_ProgScan_OnFrame) and boots on without asking. B
 * held at the start still asks. (wii/tools/orig_image.py --redirect: this
 * call only) */
#define GAME_PROGRESSIVE 0x8046B0F8u /* gmMainLib_8046B0F0.progressive */
#define LAUNCH_BUTTONS 0x804D6B30u   /* db_gameLaunchButtonState */
#define LAUNCH_B 0x0200u

void gmMainLib_8015F500(void);

u32 melee_wii_boot_dtv_status(void)
{
    u32 cable = VIDEO_HaveComponentCable();
    if (cable && CONF_GetProgressiveScan() > 0 && !(*(volatile u16*) LAUNCH_BUTTONS & LAUNCH_B)) {
        *(volatile u8*) GAME_PROGRESSIVE = 1;
        gmMainLib_8015F500();
        OSSetProgressiveMode(1);
        return 0;
    }
    return cable;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = s_pre_retrace;
    s_pre_retrace = cb;
    OSRestoreInterrupts(enabled);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    return (VIRetraceCallback) VIDEO_SetPostRetraceCallback(
        (ogc_VIRetraceCallback) cb);
}
