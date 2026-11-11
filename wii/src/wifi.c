/* The Wii's network services beside the sockets, as the Revolution SDK's
 * RevoEX libraries call them (nwc24, ncd, wd; their IOS commands read from
 * RevoEX 2.3's libraries):
 *
 * - WiiConnect24's background scheduler (/dev/net/kd/request), paused while
 *   the game runs. On a Wii with WiiConnect24 on it sends and downloads every
 *   1-10 minutes during a game, which the SDK's manual says can worsen
 *   "network communication response" and NAND access (NWC24SuspendScheduler,
 *   WiiConnect24 Programming Manual, 8.1).
 * - The Wi-Fi signal: the level of the last frame the Wi-Fi module received,
 *   which IOS keeps at 0x800031A0 (WDGetRadioLevel(NULL) reads it there; no
 *   IOS call).
 * - The Wi-Fi connection's transmission rates and send retries
 *   (/dev/net/ncd/manage), changed for this session only, before the network
 *   starts (NCDSetIfConfig: the settings saved in NAND stay as they are). */

#include <gccore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* usleep (the ioctl probe's timeout wait) */

#include <wl/base/base.h>
#include <wl/platform/platform.h>

#include "melee_wii.h"

#define KD_PATH "/dev/net/kd/request"
#define KD_SUSPEND_SCHEDULER 1
#define KD_RESUME_SCHEDULER 3
#define KD_LOCK_SOCKET 8
#define KD_UNLOCK_SOCKET 9

#define NCD_PATH "/dev/net/ncd/manage"
#define NCD_GET_CONFIG 3 /* the settings in use (IOS's copy) */
#define NCD_SET_CONFIG 4 /* the settings in use, until IOS reloads (not NAND's) */
#define NCD_PROFILES 3
#define NCD_PROFILE_SIZE 2332
#define NCD_HEADER_SIZE 8
#define NCD_FLAG_IS_WIRED 0x01
#define NCD_FLAG_ENABLE 0x80
#define NCD_NETIF 0x7C0 /* a profile's NCDWirelessProfile: u16 rateset, u8 configMethod, u8 retryLimit */

#define SIGNAL_ADDR 0x800031A0u /* u16 (its low byte: the signal), u8 1 when there is one */

static u32 s_suspend_out[8] __attribute__((aligned(32)));
static volatile int s_suspend_state; /* 0 not asked, 1 asked, 2 paused, -1 failed */
static volatile s32 s_suspend_result;
static char s_report[200];
static char s_wc24_text[64] = "WiiConnect24 scheduler: not paused yet";

/* ---- WiiConnect24's scheduler --------------------------------------------- */

/* One of KD's commands that take nothing; its first word back (a count, or
 * an NWC24 error), or IOS's error */
static s32 kd_command(int command)
{
    static u32 out[8] __attribute__((aligned(32)));
    s32 fd = IOS_Open(KD_PATH, 0);
    s32 res;
    if (fd < 0) {
        return fd;
    }
    memset(out, 0, sizeof(out));
    res = IOS_Ioctl(fd, command, NULL, 0, out, sizeof(out));
    IOS_Close(fd);
    return res < 0 ? res : (s32) out[0];
}

static s32 suspend_done(s32 result, void* fd)
{
    s32 previous = (s32) s_suspend_out[0];
    if (result < 0 || previous < 0) {
        s_suspend_result = result < 0 ? result : previous;
        s_suspend_state = -1;
    } else {
        s_suspend_result = previous;
        s_suspend_state = 2;
    }
    IOS_CloseAsync((s32) fd, NULL, NULL);
    return 0;
}

/* main.c, at start-up: asks IOS to pause the scheduler and goes on (if a
 * send or download is under way, the pause comes when it has finished:
 * seconds, at worst) */
void melee_wii_wc24_suspend(void)
{
    s32 fd;
    if (s_suspend_state != 0) {
        return;
    }
    fd = IOS_Open(KD_PATH, 0);
    if (fd < 0) {
        s_suspend_state = -1;
        s_suspend_result = fd;
        WL_LOG_W("melee", "wc24: could not open %s (%d): WiiConnect24's scheduler left as it is", KD_PATH, (int) fd);
        return;
    }
    memset(s_suspend_out, 0, sizeof(s_suspend_out));
    s_suspend_state = 1;
    if (IOS_IoctlAsync(fd, KD_SUSPEND_SCHEDULER, NULL, 0, s_suspend_out, sizeof(s_suspend_out), suspend_done,
                       (void*) fd) < 0)
    {
        s_suspend_state = -1;
        IOS_Close(fd);
        WL_LOG_W("melee", "wc24: could not ask for the scheduler's pause");
    }
}

/* main.c, before leaving: the scheduler as it was */
void melee_wii_wc24_resume(void)
{
    if (s_suspend_state == 2) {
        kd_command(KD_RESUME_SCHEDULER);
        s_suspend_state = 0;
    }
}

/* What became of the pause, for the log and the Diagnostics record */
const char* melee_wii_wc24_status(void)
{
    switch (s_suspend_state) {
    case 2:
        snprintf(s_wc24_text, sizeof(s_wc24_text), "WiiConnect24 scheduler: paused (%s)",
                 s_suspend_result == 0 ? "it was running" : "it was paused already");
        break;
    case 1:
        snprintf(s_wc24_text, sizeof(s_wc24_text), "WiiConnect24 scheduler: pause asked, not done yet");
        break;
    case -1:
        snprintf(s_wc24_text, sizeof(s_wc24_text), "WiiConnect24 scheduler: not paused (error %d)",
                 (int) s_suspend_result);
        break;
    default:
        break;
    }
    return s_wc24_text;
}

/* ---- the signal ------------------------------------------------------------ */

/* The signal of the last frame the Wi-Fi module received, 0-255 (the SDK's
 * levels: 3 from 196, 2 from 181, 1 from 171: as a signed byte, dBm, -60,
 * -75 and -85), or -1 when there is none (wired, or nothing received yet) */
int melee_wii_wifi_signal(void)
{
    volatile u8* p = (volatile u8*) SIGNAL_ADDR;
    DCInvalidateRange((void*) SIGNAL_ADDR, 32);
    return p[2] != 0 ? p[1] : -1;
}

/* ---- the connection's rates and retries ------------------------------------ */

static u16 get_u16(const u8* p)
{
    return (u16) ((p[0] << 8) | p[1]);
}

/* so_socket_ogc.c, before the network starts (net_init): sets the
 * transmission rates (a bit for each: 0x0001 1 Mb/s ... 0x0800 54 Mb/s; 0:
 * the driver's choice) and the send retries (0: the driver's) of every
 * enabled Wi-Fi profile for this session, -1 for either leaving it as
 * saved. `config` is 7004 bytes (32-byte aligned) the call may use. Returns
 * 0, or < 0 with nothing changed; either way the report says what was done
 * (melee_wii_wifi_report). */
int melee_wii_wifi_tune(void* config, int rateset, int retry)
{
    static ioctlv vec[2] __attribute__((aligned(32)));
    static u32 result[8] __attribute__((aligned(32)));
    u8* buf = config;
    char before[64] = "", after[64] = "";
    int changed = 0, wireless = 0, len = 0;
    s32 fd, res, lock;

    fd = IOS_Open(NCD_PATH, 0);
    if (fd < 0) {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: could not open %s (%d)", NCD_PATH, (int) fd);
        return -1;
    }
    /* (as NCDSetIfConfig: WiiConnect24's socket use held off meanwhile) */
    lock = kd_command(KD_LOCK_SOCKET);
    memset(result, 0, sizeof(result));
    vec[0].data = buf;
    vec[0].len = NCD_HEADER_SIZE + NCD_PROFILES * NCD_PROFILE_SIZE;
    vec[1].data = result;
    vec[1].len = sizeof(result);
    res = IOS_Ioctlv(fd, NCD_GET_CONFIG, 0, 2, vec);
    if (res < 0 || (s32) result[0] != 0) {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: could not read them (%d, %d)", (int) res,
                 (int) result[0]);
        res = -2;
        goto done;
    }
    for (int i = 0; i < NCD_PROFILES; i++) {
        u8* profile = buf + NCD_HEADER_SIZE + i * NCD_PROFILE_SIZE;
        u8* netif = profile + NCD_NETIF;
        if (!(profile[0] & NCD_FLAG_ENABLE) || (profile[0] & NCD_FLAG_IS_WIRED)) {
            continue;
        }
        wireless++;
        len += snprintf(before + len, sizeof(before) - len, "%s%d: rates %04x, retries %u", len ? "; " : "", i + 1,
                        get_u16(netif), netif[3]);
        if (rateset >= 0 && get_u16(netif) != (u16) rateset) {
            netif[0] = (u8) (rateset >> 8);
            netif[1] = (u8) rateset;
            changed = 1;
        }
        if (retry >= 0 && netif[3] != (u8) retry) {
            netif[3] = (u8) retry;
            changed = 1;
        }
        snprintf(after, sizeof(after), "rates %04x, retries %u", get_u16(netif), netif[3]);
    }
    if (wireless == 0) {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: no enabled Wi-Fi connection");
        res = 0;
        goto done;
    }
    if (!changed) {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: as saved (connection %s)", before);
        res = 0;
        goto done;
    }
    memset(result, 0, sizeof(result));
    vec[0].data = buf;
    vec[0].len = NCD_HEADER_SIZE + NCD_PROFILES * NCD_PROFILE_SIZE;
    vec[1].data = result;
    vec[1].len = sizeof(result);
    res = IOS_Ioctlv(fd, NCD_SET_CONFIG, 1, 1, vec);
    if (res < 0 || (s32) result[0] != 0) {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: could not change them (%d, %d; connection %s)",
                 (int) res, (int) result[0], before);
        res = -3;
    } else {
        snprintf(s_report, sizeof(s_report), "Wi-Fi settings: %s for this session (saved: connection %s)", after,
                 before);
        res = 0;
    }
done:
    if (lock >= 0) {
        kd_command(KD_UNLOCK_SOCKET);
    }
    IOS_Close(fd);
    return res;
}

const char* melee_wii_wifi_report(void)
{
    return s_report;
}

/* ---- the Wi-Fi driver (/dev/wl): turning roaming off ----------------------- */

/* The Broadcom Wi-Fi driver (IOS's WL module) roams: about every 10 s, while
 * associated, it scans for a better access point, and each scan stalls the
 * connection long enough to spike netplay latency. The driver has one switch
 * for it, the "roam_off" iovar (a byte in its wlc; there is no finer roam iovar
 * in this 4.30.47.3 build). A title cannot reach that byte in memory (IOS keeps
 * the WL image out of the processor's view), but it CAN command the driver: open
 * /dev/wl0 (the node WD opens inside IOS; a PPC title may open it too) and send
 * the Broadcom WLC command WLC_SET_VAR "roam_off" = 1.
 *
 * The catch is the command format (reverse-engineered from WD): IOS_Ioctl
 * number 1 carries a WLC command, but the input is an IOS "iobuf", not a flat
 * buffer -- one or more nodes, each {next at +0x00, data pointer at +0x10, u16
 * length at +0x16}. The module gathers the nodes' data into [cmd, _, len at +8,
 * _,_,_, payload at +0x18] and runs the command, writing any result back into
 * the node data. We hand-build ONE node whose data pointer is the physical
 * address of a payload right after it, and drive it asynchronously with a
 * timeout and a settle delay (a wrong format once hung IOS; a rushed back-to-
 * back reuse of the buffer once raced and crashed -- the delay fixes that).
 * melee_wii_wifi_roam_guard() below reads roam_off and, if it is on, turns it
 * off and verifies; the netplay thread calls it after the connection is up and
 * keeps it off (so_socket_ogc.c). */

#define WL_PATH "/dev/wl0"

/* The driver's command interface, as WD uses it: IOS_Ioctl number 1 carries a
 * Broadcom WLC command, but the input is an IOS "iobuf", not a flat buffer --
 * one or more nodes, each {next at +0x00, data pointer at +0x10, u16 length at
 * +0x16}. The module gathers the nodes' data into [cmd, _, len at +8, _,_,_,
 * payload at +0x18] and runs the WLC command, writing results back into the
 * node data. A flat buffer is walked as a node and its bytes followed as
 * pointers -- which is the hang we hit. Here we hand-build ONE valid node whose
 * data pointer is the physical address of a payload right after it. */
#define WL_IOCTL_WLC 1       /* IOS_Ioctl number for a WLC command */
#define WLC_GET_MAGIC 0      /* simplest WLC command: returns 0x14E41726 */
#define WLC_GET_VAR 0x106    /* read an iovar by name (payload: name, then value back) */
#define WLC_SET_VAR 0x107    /* write an iovar (payload: name\0 then the value) */
#define WLC_IOCTL_MAGIC 0x14E41726u
#define VIRT_TO_PHYS(x) ((u32) (x) & 0x3FFFFFFFu) /* 0x9xxxxxxx EA -> 0x1xxxxxxx physical */

/* node (0x20) then payload (0x40); the module reads the payload by its physical
 * address, so this must be one physically-contiguous, cache-aligned block */
static u8 s_wl_io[0x60] __attribute__((aligned(32)));
static volatile int s_wl_async_done;
static volatile s32 s_wl_async_res;

static s32 wl_async_cb(s32 result, void* ud)
{
    (void) ud;
    s_wl_async_res = result;
    s_wl_async_done = 1;
    return 0;
}

/* Run WLC command `cmd` through a hand-built single-node iobuf. `name` is the
 * iovar name for GET_VAR/SET_VAR (NULL otherwise); for SET_VAR `setval` >= 0 is
 * written as the value byte right after name\0 (any non-zero sets a bool iovar);
 * -1 means no value (GET). `inner` is the payload length. Returns the async
 * result (0 ok, -200 timeout, -100 submit failed). The reply, for GET, is left
 * in the payload at s_wl_io + 0x20 + 0x18. */
static s32 wl_do(s32 fd, u32 cmd, u32 inner, const char* name, int setval)
{
    u8* node = s_wl_io;
    u8* data = s_wl_io + 0x20;
    int waited;

    memset(s_wl_io, 0, sizeof(s_wl_io));
    *(u32*) (node + 0x00) = 0;                    /* next = NULL */
    *(u32*) (node + 0x10) = VIRT_TO_PHYS(data);   /* data pointer (physical) */
    *(u16*) (node + 0x16) = (u16) (0x18 + inner); /* node data length */
    *(u32*) (data + 0x00) = cmd;                  /* WLC command */
    *(u32*) (data + 0x08) = inner;                /* payload length */
    if (name != NULL) {
        strcpy((char*) (data + 0x18), name);      /* iovar name */
        if (setval >= 0) {
            /* value goes right after name\0 (no bsscfg index for an unprefixed
             * iovar); any non-zero sets a bool iovar */
            data[0x18 + strlen(name) + 1] = (u8) setval;
        }
    }
    DCFlushRange(s_wl_io, sizeof(s_wl_io));

    s_wl_async_done = 0;
    s_wl_async_res = 0;
    if (IOS_IoctlAsync(fd, WL_IOCTL_WLC, s_wl_io, sizeof(s_wl_io), NULL, 0, wl_async_cb, NULL) < 0) {
        return -100;
    }
    for (waited = 0; waited < 2000 && !s_wl_async_done; waited++) {
        usleep(1000);
    }
    if (!s_wl_async_done) {
        return -200;
    }
    DCInvalidateRange(s_wl_io, sizeof(s_wl_io));
    /* settle before the buffer may be reused: the reply's write-back and IOS's
     * own book-keeping finish after the callback fires; reusing s_wl_io at once
     * raced and crashed IOS. 20 ms is far more than it needs. */
    usleep(20000);
    return s_wl_async_res;
}

/* Read roam_off, turning it on-off (off) if it is on, and verify. The driver
 * resets roam_off to 0 (roaming on) on every (re)association, so this is meant
 * to be called again and again while connected. Returns:
 *    1  roaming is off (it already was, or we turned it off and confirmed it);
 *    0  roaming is on and could not be turned off (the caller should stop);
 *   -1  the driver could not be reached (no Wi-Fi: a wired link, say).
 * The ioctls are spaced (wl_do settles after each), so this is safe to call,
 * but it may block up to a few seconds on a stuck driver -- run it off the
 * latency-sensitive path (a worker thread), never in the netplay loop. */
int melee_wii_wifi_roam_guard(void)
{
    const volatile u32* d = (const volatile u32*) (s_wl_io + 0x20); /* payload words */
    s32 fd, r;

    fd = IOS_Open(WL_PATH, 0);
    if (fd < 0) {
        return -1;
    }
    r = wl_do(fd, WLC_GET_VAR, 0x20, "roam_off", -1);
    if (r == 0 && d[0x18 / 4] != 0) { /* already off */
        IOS_Close(fd);
        return 1;
    }
    /* on, or the read failed: turn it off and read it back */
    wl_do(fd, WLC_SET_VAR, 0x0D, "roam_off", 1); /* name(9) + value(4); any non-zero = off */
    r = wl_do(fd, WLC_GET_VAR, 0x20, "roam_off", -1);
    IOS_Close(fd);
    return (r == 0 && d[0x18 / 4] != 0) ? 1 : 0;
}
