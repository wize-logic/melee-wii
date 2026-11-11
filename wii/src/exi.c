/* The SDK's EXI, for the one device the game talks to over it on the Wii:
 * Slippi's (slot B: channel 1, device 0), which Slippi's online codeset uses
 * (FN_EXITransferBuffer: attach, lock, select, one DMA, sync, deselect,
 * unlock, detach, with interrupts off). wii/src/slippi's client stands in for
 * it, as Slippi Dolphin's EXI device and Slippi Nintendont's kernel do. Every
 * other device is absent: the memory cards are the port's own (card.c), and
 * the clock and settings the OS's (os.c). */

#include "melee_wii.h"

#include <dolphin/exi.h>
#include <dolphin/os.h>

#include <string.h>

/* wii/src/slippi/so_exi.h (its types are libogc's) */
void so_exi_write(const unsigned char* request, unsigned int dma_len);
unsigned int so_exi_read(unsigned char* out, unsigned int len);
void so_exi_after_ack(void);
void so_exi_other_write(void);
void so_replay_data(const unsigned char* data, unsigned int len);
void so_console_data(const unsigned char* data, unsigned int len);
int so_exi_is_online_command(unsigned char cmd);

#define SLIPPI_CHANNEL 1
#define SLIPPI_DEVICE 0

#define EXI_READ 0
#define EXI_WRITE 1

typedef struct Channel {
    BOOL attached;
    BOOL locked;
    BOOL selected;
    u32 device;
    EXICallback exi_callback;
    EXICallback tc_callback;
} Channel;

static Channel s_channels[3];
static BOOL s_slippi; /* Slippi's device is there (melee_wii_exi_slippi) */

void melee_wii_exi_slippi(BOOL present)
{
    s_slippi = present;
}

static BOOL slippi(s32 channel)
{
    return s_slippi && channel == SLIPPI_CHANNEL && s_channels[channel].selected &&
           s_channels[channel].device == SLIPPI_DEVICE;
}

void EXIInit(void) {}

EXICallback EXISetExiCallback(s32 channel, EXICallback callback)
{
    EXICallback old;
    if (channel < 0 || channel > 2) {
        return NULL;
    }
    old = s_channels[channel].exi_callback;
    s_channels[channel].exi_callback = callback;
    return old;
}

BOOL EXIProbe(s32 channel)
{
    return s_slippi && channel == SLIPPI_CHANNEL;
}

s32 EXIProbeEx(s32 channel)
{
    return EXIProbe(channel) ? 1 : 0;
}

BOOL EXIAttach(s32 channel, EXICallback callback)
{
    if (!EXIProbe(channel)) {
        return FALSE;
    }
    s_channels[channel].attached = TRUE;
    s_channels[channel].exi_callback = callback;
    return TRUE;
}

BOOL EXIDetach(s32 channel)
{
    if (channel < 0 || channel > 2) {
        return FALSE;
    }
    s_channels[channel].attached = FALSE;
    return TRUE;
}

BOOL EXILock(s32 channel, u32 device, EXICallback callback)
{
    (void) callback;
    if (channel < 0 || channel > 2 || s_channels[channel].locked) {
        return FALSE;
    }
    s_channels[channel].locked = TRUE;
    s_channels[channel].device = device;
    return TRUE;
}

BOOL EXIUnlock(s32 channel)
{
    if (channel < 0 || channel > 2) {
        return FALSE;
    }
    s_channels[channel].locked = FALSE;
    return TRUE;
}

BOOL EXISelect(s32 channel, u32 device, u32 frequency)
{
    (void) frequency;
    if (channel < 0 || channel > 2 || !s_slippi || channel != SLIPPI_CHANNEL ||
        device != SLIPPI_DEVICE)
    {
        return FALSE;
    }
    s_channels[channel].selected = TRUE;
    s_channels[channel].device = device;
    return TRUE;
}

BOOL EXIDeselect(s32 channel)
{
    if (channel < 0 || channel > 2) {
        return FALSE;
    }
    s_channels[channel].selected = FALSE;
    return TRUE;
}

/* One transfer: a write is one command for the device (the codeset writes
 * each on its own, and the device reads its payload from memory: 0xC2 and
 * the file commands carry more than the transfer's length); a read takes the
 * answer it prepared, which the game then invalidates in its cache (so it
 * goes to memory here). */
static BOOL transfer(s32 channel, void* buffer, s32 length, u32 type)
{
    if (!slippi(channel) || length <= 0) {
        return FALSE;
    }
    if (type == EXI_WRITE) {
        const unsigned char* request = buffer;
        if (so_exi_is_online_command(request[0])) {
            so_exi_write(request, (unsigned int) length);
        } else {
            /* replay data (Slippi's recording): kept as a .slp
             * (wii/src/slippi/so_replay.c), and sent to Slippi Launcher's
             * console mirroring (so_console.c) */
            so_exi_other_write();
            so_replay_data(request, (unsigned int) length);
            so_console_data(request, (unsigned int) length);
        }
    } else {
        so_exi_read((unsigned char*) buffer, (unsigned int) length);
        DCFlushRange(buffer, (u32) length);
        so_exi_after_ack();
    }
    return TRUE;
}

BOOL EXIImm(s32 channel, void* buffer, s32 length, u32 type, EXICallback callback)
{
    BOOL ok = transfer(channel, buffer, length, type);
    s_channels[channel < 0 || channel > 2 ? 0 : channel].tc_callback = callback;
    return ok;
}

BOOL EXIImmEx(s32 channel, void* buffer, s32 length, u32 type)
{
    return transfer(channel, buffer, length, type);
}

BOOL EXIDma(s32 channel, void* buffer, s32 length, u32 type, EXICallback callback)
{
    BOOL ok = transfer(channel, buffer, length, type);
    s_channels[channel < 0 || channel > 2 ? 0 : channel].tc_callback = callback;
    return ok;
}

/* The transfers above are done when they return */
BOOL EXISync(s32 channel)
{
    EXICallback callback;
    if (channel < 0 || channel > 2) {
        return FALSE;
    }
    callback = s_channels[channel].tc_callback;
    s_channels[channel].tc_callback = NULL;
    if (callback != NULL) {
        callback(channel, NULL);
    }
    return TRUE;
}

u32 EXIGetState(s32 channel)
{
    u32 state = 0;
    if (channel < 0 || channel > 2) {
        return 0;
    }
    if (s_channels[channel].attached) {
        state |= 8;
    }
    if (s_channels[channel].locked) {
        state |= 0x10;
    }
    return state;
}

s32 EXIGetID(s32 channel, u32 device, u32* id)
{
    (void) device;
    if (!EXIProbe(channel)) {
        return 0;
    }
    *id = 0; /* (Slippi's device has no ID to report) */
    return 1;
}
