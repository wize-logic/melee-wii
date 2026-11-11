// SPDX-License-Identifier: MIT
// The platform layer, as much of it as Super Smash Bros. Melee's Wii port uses: the system's
// buttons and leaving, files on the SD card and a USB drive, the log, and the stop screen.
// Only wl/platform/wii includes <ogc/*.h>.
#pragma once

#include "wl/base/base.h"

// ---------------------------------------------------------------------------------------------
// System buttons and leaving
// ---------------------------------------------------------------------------------------------

// Installs the reset and power button callbacks (interrupt context: they only latch a bit).
void wl_platform_system_init(void);

typedef enum {
    WL_EXIT_LOADER = 0, // return to the Homebrew Channel (or whatever loaded the program)
    WL_EXIT_WII_MENU,
    WL_EXIT_POWER_OFF
} WlExitTarget;

enum { WL_SYS_RESET = 1u << 0, WL_SYS_POWER = 1u << 1 };

// The buttons pressed since the last call (WL_SYS_*), cleared.
uint32_t wl_platform_system_events(void);

// Flushes the log, unmounts the SD card and the USB drive (their caches written out) and leaves.
// WL_EXIT_LOADER powers off when no loader left a stub to return to (Dolphin booting the DOL).
WL_NORETURN void wl_platform_exit(WlExitTarget target);

// A function wl_platform_exit runs first, once, on the thread that leaves, before its own steps:
// the program's chance to stop what it started (DMA, threads reading the card). NULL for none.
void wl_platform_set_exit_hook(void (*hook)(void));

// Starts a thread that looks at the buttons every `period_ms` and leaves: power with
// WL_EXIT_POWER_OFF, reset with WL_EXIT_LOADER. `tick` (may be NULL) runs on that thread between
// looks. While it runs the program must not call wl_platform_system_events.
WlResult wl_platform_system_watch_start(uint32_t period_ms, void (*tick)(void *user), void *user);

// ---------------------------------------------------------------------------------------------
// Files (SD and USB through libfat). Blocking: call from an IO thread, except small reads at boot.
// ---------------------------------------------------------------------------------------------

typedef enum { WL_DEVICE_SD = 0, WL_DEVICE_USB } WlDevice;

// IOS 58 gives USB 2.0. Reloads only when another IOS runs, and only before SD or USB is
// mounted (a reload drops every IOS handle). Writes the IOS version running afterwards to
// *out_ios_version (may be NULL). WL_ERR_FAILED when the reload failed.
WlResult wl_platform_fs_ios_init(uint32_t *out_ios_version);

// Mounts `sd:/` or `usb:/`. Idempotent. WL_ERR_NOT_FOUND when nothing is inserted.
WlResult wl_platform_fs_mount(WlDevice device);
bool wl_platform_fs_mounted(WlDevice device);
// Unmounts (flushes first). No-op when not mounted.
void wl_platform_fs_unmount(WlDevice device);

typedef struct WlPlatformFile WlPlatformFile;

typedef enum { WL_FILE_READ = 0, WL_FILE_WRITE_NEW, WL_FILE_READ_WRITE } WlPlatformFileMode;

#define WL_FS_MAX_FILES 16u

// Paths with a device prefix ("sd:/apps/x/y"). WL_ERR_NOT_FOUND, WL_ERR_STATE (not mounted),
// WL_ERR_LIMIT (no handle free), WL_ERR_IO. A handle is used by one thread at a time.
WlResult wl_platform_fs_open(const char *path, WlPlatformFileMode mode, WlPlatformFile **out);
// Exact read: all `size` bytes at `offset`, or WL_ERR_RANGE when the file ends first, WL_ERR_IO.
WlResult wl_platform_fs_read_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size);
// Appends at the current position.
WlResult wl_platform_fs_write(WlPlatformFile *file, const void *src, uint32_t size);
WlResult wl_platform_fs_flush(WlPlatformFile *file);
uint64_t wl_platform_fs_size(const WlPlatformFile *file);
void wl_platform_fs_close(WlPlatformFile *file);
// Removes a file. WL_ERR_NOT_FOUND when there is none.
WlResult wl_platform_fs_remove(const char *path);
// Creates one directory level; an existing directory is WL_OK.
WlResult wl_platform_fs_mkdir(const char *path);
// True when `path` names an existing file or directory.
bool wl_platform_fs_exists(const char *path);

// ---------------------------------------------------------------------------------------------
// Logging: the EXI UART (Dolphin's OSREPORT log) and a file on the SD card
// ---------------------------------------------------------------------------------------------

// Installs the sink with wl_log_set_sink. Idempotent. `sd_dir` (e.g. "sd:/apps/myapp/logs")
// enables the file <sd_dir>/log.txt (the four runs before it kept as log-1.txt to log-4.txt); NULL or no card keeps it off. Lines are
// buffered in memory (the sink may be called with interrupts disabled) and written by a thread
// of their own every second, and by wl_platform_log_flush.
void wl_platform_log_init(const char *sd_dir);
void wl_platform_log_flush(void);
// Flushes and closes the file.
void wl_platform_log_shutdown(void);
