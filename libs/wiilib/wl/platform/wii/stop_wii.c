// SPDX-License-Identifier: MIT
// The stop screen (stop.h): libogc's text console on a framebuffer of its own; waits for START on a GameCube controller, or the reset or
// power button, and leaves for the loader.
#include <gccore.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

#include "wl/platform/platform.h"
#include "wl/platform/stop.h"

WL_NORETURN void wl_platform_stop_screen(const char *fmt, ...) {
    static atomic_flag stopping = ATOMIC_FLAG_INIT;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    WL_LOG_E("STOP", "%s", msg);
    wl_platform_log_flush();
    if (atomic_flag_test_and_set(&stopping)) {
        for (;;) {
            VIDEO_WaitVSync();
        }
    }

    // the video as libogc starts it (the program's own may not have started), a framebuffer the
    // program's frames cannot cover
    VIDEO_Init();
    GXRModeObj *mode = VIDEO_GetPreferredMode(NULL);
    void *fb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(mode));
    VIDEO_Configure(mode);
    console_init(fb, 20, 20, mode->fbWidth, mode->xfbHeight, mode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_SetNextFramebuffer(fb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    printf("\n\n%s\n\nPress START (or RESET) to return to the loader.\n", msg);

    PAD_Init();
    for (;;) {
        PAD_ScanPads();
        bool leave = false;
        for (int i = 0; i < PAD_CHANMAX; i++) {
            leave |= (PAD_ButtonsDown(i) & PAD_BUTTON_START) != 0u;
        }
        if (leave || wl_platform_system_events() != 0u) {
            wl_platform_exit(WL_EXIT_LOADER);
        }
        VIDEO_WaitVSync();
    }
}
