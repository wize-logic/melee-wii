/* What the game links against but the Wii has no use for: the development
 * kit's host link (MCC, FIO), and MSL's leftovers */

#include "melee_wii.h"

#include <dolphin/types.h>
#include <dolphin/mcc.h>

#ifndef MELEE_ORIG_CODE
/* MSL's float.c: a NaN and an infinity, as words (MSL's own objects have them
 * with MELEE_ORIG_CODE) */
int MSL_TrigF_80400770[] = { 0x7FFFFFFF };
int MSL_TrigF_80400774[] = { 0x7F800000 };
#endif

/* MWCC's linker: the main thread's stack, which db_ reports on */
unsigned char _stack_addr[4];
unsigned char _stack_end[4];

int MCCInit(enum MCC_EXI exiChannel, u8 timeout,
            MCC_CBSysEvent callbackSysEvent)
{
    (void) exiChannel;
    (void) timeout;
    (void) callbackSysEvent;
    return 0;
}

void MCCExit(void) {}

int MCCEnumDevices(MCC_CBEnumDevices callbackEnumDevices)
{
    (void) callbackEnumDevices;
    return 0;
}

u8 MCCGetFreeBlocks(enum MCC_MODE mode)
{
    (void) mode;
    return 0;
}

u8 MCCGetLastError(void)
{
    return 0;
}

int MCCGetConnectionStatus(enum MCC_CHANNEL chID, enum MCC_CONNECT* connect)
{
    (void) chID;
    (void) connect;
    return 0;
}

int MCCNotify(enum MCC_CHANNEL chID, u32 notify)
{
    (void) chID;
    (void) notify;
    return 0;
}

int MCCOpen(enum MCC_CHANNEL chID, u8 blockSize, MCC_CBEvent callbackEvent)
{
    (void) chID;
    (void) blockSize;
    (void) callbackEvent;
    return 0;
}

int MCCClose(enum MCC_CHANNEL chID)
{
    (void) chID;
    return 0;
}

int MCCRead(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
            enum MCC_SYNC_STATE async)
{
    (void) chID;
    (void) offset;
    (void) data;
    (void) size;
    (void) async;
    return 0;
}

int MCCWrite(enum MCC_CHANNEL chID, u32 offset, void* data, long size,
             enum MCC_SYNC_STATE async)
{
    (void) chID;
    (void) offset;
    (void) data;
    (void) size;
    (void) async;
    return 0;
}

int MCCStreamOpen(enum MCC_CHANNEL chID, u8 blockSize)
{
    (void) chID;
    (void) blockSize;
    return 0;
}

int FIOInit(enum MCC_EXI exiChannel, enum MCC_CHANNEL chID, u8 blockSize)
{
    (void) exiChannel;
    (void) chID;
    (void) blockSize;
    return 0;
}

void FIOExit(void) {}

int FIOQuery(void)
{
    return 0;
}

int FIOFopen(const char* filename, u32 mode)
{
    (void) filename;
    (void) mode;
    return -1;
}

int FIOFclose(int handle)
{
    (void) handle;
    return 0;
}

u32 FIOFwrite(int handle, void* data, u32 size)
{
    (void) handle;
    (void) data;
    (void) size;
    return 0;
}
