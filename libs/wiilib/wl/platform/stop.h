// SPDX-License-Identifier: MIT
// The stop screen: what a program shows when it cannot go on.
#pragma once

#include "wl/base/base.h"

// Writes the message to the log and to libogc's text console on a framebuffer of its own, and
// waits for START on a GameCube controller (or the reset or power button) to leave for the
// loader (wl_platform_exit with WL_EXIT_LOADER). The first stop wins: a second one only waits.
WL_NORETURN void wl_platform_stop_screen(const char *fmt, ...) WL_PRINTF_FMT(1, 2);
