// SPDX-License-Identifier: MIT
// Files on the SD card and a USB drive (platform.h), over libfat and newlib's stdio.
#include <errno.h>
#include <fat.h>
#include <gccore.h>
#include <ogc/usbstorage.h>
#include <sdcard/wiisd_io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wl/platform/platform.h"

// IOS 58 has the USB 2.0 driver
#define WL_USB_IOS 58

struct WlPlatformFile {
    FILE *f;
    uint64_t size;
    bool used;
};

static WlPlatformFile g_files[WL_FS_MAX_FILES];
static bool g_mounted[2];
static bool g_ios_done;

WlResult wl_platform_fs_ios_init(uint32_t *out_ios_version) {
    WlResult r = WL_OK;
    if (!g_ios_done) {
        g_ios_done = true;
        s32 v = IOS_GetVersion();
        if (v != WL_USB_IOS && (g_mounted[WL_DEVICE_SD] || g_mounted[WL_DEVICE_USB])) {
            // a reload closes every IOS handle, the drivers' open devices with them
            WL_LOG_W("FS", "IOS %d running and a device mounted: no reload to IOS %d", (int)v, WL_USB_IOS);
            r = WL_ERR_STATE;
        } else if (v != WL_USB_IOS) {
            WL_LOG_I("FS", "IOS %d running, reloading IOS %d for USB 2.0", (int)v, WL_USB_IOS);
            if (IOS_ReloadIOS(WL_USB_IOS) < 0) {
                WL_LOG_W("FS", "IOS %d reload failed, staying on IOS %d", WL_USB_IOS, (int)v);
                r = WL_ERR_FAILED;
            }
        }
    } else if (IOS_GetVersion() != WL_USB_IOS) {
        r = WL_ERR_FAILED;
    }
    if (out_ios_version != NULL) {
        *out_ios_version = (uint32_t)IOS_GetVersion();
    }
    return r;
}

static const char *device_name(WlDevice device) {
    return device == WL_DEVICE_SD ? "sd" : "usb";
}

WlResult wl_platform_fs_mount(WlDevice device) {
    if ((unsigned)device > 1u) {
        return WL_ERR_ARG;
    }
    if (g_mounted[device]) {
        return WL_OK;
    }
    const DISC_INTERFACE *disc = device == WL_DEVICE_SD ? &__io_wiisd : &__io_usbstorage;
    if (!fatMountSimple(device_name(device), disc)) {
        WL_LOG_I("FS", "%s: not present", device_name(device));
        return WL_ERR_NOT_FOUND;
    }
    g_mounted[device] = true;
    WL_LOG_I("FS", "%s: mounted", device_name(device));
    return WL_OK;
}

bool wl_platform_fs_mounted(WlDevice device) {
    return (unsigned)device <= 1u && g_mounted[device];
}

void wl_platform_fs_unmount(WlDevice device) {
    if ((unsigned)device > 1u || !g_mounted[device]) {
        return;
    }
    char name[8];
    snprintf(name, sizeof name, "%s:", device_name(device));
    fatUnmount(name);
    g_mounted[device] = false;
}

// The device a path names, or -1 without a known prefix
static int path_device(const char *path) {
    if (strncmp(path, "sd:/", 4) == 0) {
        return WL_DEVICE_SD;
    }
    if (strncmp(path, "usb:/", 5) == 0) {
        return WL_DEVICE_USB;
    }
    return -1;
}

WlResult wl_platform_fs_open(const char *path, WlPlatformFileMode mode, WlPlatformFile **out) {
    int device = path_device(path);
    if (device < 0 || out == NULL) {
        return WL_ERR_ARG;
    }
    if (!g_mounted[device]) {
        return WL_ERR_STATE;
    }
    WlPlatformFile *file = NULL;
    for (uint32_t i = 0; i < WL_FS_MAX_FILES; i++) {
        if (!g_files[i].used) {
            file = &g_files[i];
            break;
        }
    }
    if (file == NULL) {
        return WL_ERR_LIMIT;
    }
    const char *how = mode == WL_FILE_READ ? "rb" : mode == WL_FILE_WRITE_NEW ? "wb" : "r+b";
    FILE *f = fopen(path, how);
    if (f == NULL && mode == WL_FILE_READ_WRITE) {
        f = fopen(path, "w+b");
    }
    if (f == NULL) {
        return errno == ENOENT ? WL_ERR_NOT_FOUND : WL_ERR_IO;
    }
    struct stat st;
    file->f = f;
    file->size = fstat(fileno(f), &st) == 0 ? (uint64_t)st.st_size : 0u;
    file->used = true;
    *out = file;
    return WL_OK;
}

WlResult wl_platform_fs_read_at(WlPlatformFile *file, uint64_t offset, void *dst, uint32_t size) {
    if (file == NULL || !file->used || offset > 0xFFFFFFFFu) {
        return WL_ERR_ARG;
    }
    if (offset + size > file->size) {
        return WL_ERR_RANGE;
    }
    if (fseeko(file->f, (off_t)offset, SEEK_SET) != 0) {
        return WL_ERR_IO;
    }
    return fread(dst, 1, size, file->f) == size ? WL_OK : WL_ERR_IO;
}

WlResult wl_platform_fs_write(WlPlatformFile *file, const void *src, uint32_t size) {
    if (file == NULL || !file->used) {
        return WL_ERR_ARG;
    }
    if (fwrite(src, 1, size, file->f) != size) {
        return WL_ERR_IO;
    }
    long at = ftell(file->f);
    if (at > 0 && (uint64_t)at > file->size) {
        file->size = (uint64_t)at;
    }
    return WL_OK;
}

WlResult wl_platform_fs_flush(WlPlatformFile *file) {
    if (file == NULL || !file->used) {
        return WL_ERR_ARG;
    }
    if (fflush(file->f) != 0) {
        return WL_ERR_IO;
    }
    return fsync(fileno(file->f)) == 0 ? WL_OK : WL_ERR_IO;
}

uint64_t wl_platform_fs_size(const WlPlatformFile *file) {
    return file != NULL ? file->size : 0u;
}

void wl_platform_fs_close(WlPlatformFile *file) {
    if (file == NULL || !file->used) {
        return;
    }
    fclose(file->f);
    file->f = NULL;
    file->used = false;
}

WlResult wl_platform_fs_remove(const char *path) {
    int device = path_device(path);
    if (device < 0) {
        return WL_ERR_ARG;
    }
    if (!g_mounted[device]) {
        return WL_ERR_STATE;
    }
    if (unlink(path) == 0) {
        return WL_OK;
    }
    return errno == ENOENT ? WL_ERR_NOT_FOUND : WL_ERR_IO;
}

WlResult wl_platform_fs_mkdir(const char *path) {
    int device = path_device(path);
    if (device < 0) {
        return WL_ERR_ARG;
    }
    if (!g_mounted[device]) {
        return WL_ERR_STATE;
    }
    if (mkdir(path, 0777) == 0 || errno == EEXIST) {
        return WL_OK;
    }
    return WL_ERR_IO;
}

bool wl_platform_fs_exists(const char *path) {
    int device = path_device(path);
    struct stat st;
    return device >= 0 && g_mounted[device] && stat(path, &st) == 0;
}
