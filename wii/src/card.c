/* The SDK's memory card, in slot A, kept on the SD card: one file per save in
 * sd:/apps/melee/card, in the GCI format (the card's 64-byte directory entry,
 * then the file's blocks), named as Dolphin names them, so saves go between
 * the two either way. Slot B is empty.
 *
 * The card is read into memory by CARDInit; reads and writes are copies to and
 * from it (the game calls them with interrupts disabled), their callbacks come
 * from interrupt context (melee_wii_defer), and a thread of its own writes
 * what changed back to the SD card. The SDK's rules are kept where the game
 * can see them (its results, the file numbers, the status's offsets: these
 * are CARDStat.c's and CARDOpen.c's). */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/card.h>
#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <wl/base/base.h>
#include <wl/platform/platform.h>

static const char* s_card_dir = "sd:/apps/melee/card";
#define CARD_DIR s_card_dir

/* The launcher (before the game's CARDInit): 20XX keeps its saves apart */
void melee_wii_card_dir(const char* dir)
{
    s_card_dir = dir;
}
/* A 251-block card (16 Mbit): 256 blocks of 8 KiB, five the system's */
#define CARD_MEM_SIZE 16
#define CARD_SECTOR_SIZE 8192
#define CARD_USER_BLOCKS (CARD_MEM_SIZE * 16 - CARD_NUM_SYSTEM_BLOCK)
#define CARD_ENTRY_SIZE 0x40

_Static_assert(sizeof(CARDDir) == CARD_ENTRY_SIZE, "CARDDir is a GCI header");

#ifndef CARD_STAT_BANNER_C8
#define CARD_STAT_BANNER_C8 1
#define CARD_STAT_BANNER_RGB5A3 2
#define CARD_STAT_ICON_C8 1
#define CARD_STAT_ICON_RGB5A3 2
#endif
#define BANNER_WIDTH 96
#define BANNER_HEIGHT 32
#define ICON_WIDTH 32
#define ICON_HEIGHT 32
#define COMMENT_SIZE 64

typedef struct Slot {
    CARDDir ent;          /* gameName[0] 0xFF: free */
    u8* data;             /* ent.length blocks, in MEM2 */
    char path[96];        /* its file on the SD card, "" before its first write */
    volatile BOOL dirty;  /* to be written */
    volatile BOOL doomed; /* its file to be removed */
} Slot;

static Slot s_slots[CARD_MAX_FILE];
static char s_doomed_paths[CARD_MAX_FILE][96];
static BOOL s_present;
static BOOL s_mounted;
static volatile s32 s_xferred;
static u16 s_next_block = CARD_NUM_SYSTEM_BLOCK;

/* The callbacks, delivered from interrupt context */
typedef struct Pending {
    CARDCallback callback;
    s32 chan;
    s32 result;
    s32 bytes;
} Pending;

#define PENDING_MAX 16
static Pending s_pending[PENDING_MAX];
static u32 s_pending_head;
static u32 s_pending_tail;
static MeleeWiiDefer s_defer;

/* The writer thread */
static ogc_KThrQueue s_writer_wait;
static MeleeWiiDefer s_writer_kick;
static ogc_lwp_t s_writer;
static u8 s_writer_stack[16 * 1024] ATTRIBUTE_ALIGN(32);
static u8* s_write_buffer;

static void deliver(void* arg)
{
    (void) arg;
    while (s_pending_head != s_pending_tail) {
        Pending p = s_pending[s_pending_head % PENDING_MAX];
        s_pending_head++;
        s_xferred += p.bytes;
        if (p.callback != NULL) {
            p.callback(p.chan, p.result);
        }
    }
}

/* The result now; the callback, with it, from interrupt context */
static s32 complete(CARDCallback callback, s32 chan, s32 result, s32 bytes)
{
    BOOL enabled = OSDisableInterrupts();
    if (s_pending_tail - s_pending_head < PENDING_MAX) {
        Pending* p = &s_pending[s_pending_tail % PENDING_MAX];
        p->callback = callback;
        p->chan = chan;
        p->result = result;
        p->bytes = bytes;
        s_pending_tail++;
        s_defer.fn = deliver;
        melee_wii_defer(&s_defer);
    } else {
        OSReport("CARD: too many callbacks pending\n");
    }
    OSRestoreInterrupts(enabled);
    return result;
}

static void kick_writer(void* arg)
{
    (void) arg;
    KThrQueueUnblockAllByValue(&s_writer_wait, (uintptr_t) &s_writer_wait);
}

static void schedule_write(void)
{
    s_writer_kick.fn = kick_writer;
    melee_wii_defer(&s_writer_kick);
}

/* Dolphin's name for a GCI file: company-game-fileName.gci */
static void gci_path(const CARDDir* ent, char* out, size_t size)
{
    char name[CARD_FILENAME_MAX + 1];
    memcpy(name, ent->fileName, CARD_FILENAME_MAX);
    name[CARD_FILENAME_MAX] = '\0';
    for (char* c = name; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\' || *c == ':' || *c == '*' || *c == '?' ||
            *c == '"' || *c == '<' || *c == '>' || *c == '|' ||
            (unsigned char) *c < 0x20)
        {
            *c = '_';
        }
    }
    snprintf(out, size, "%s/%.2s-%.4s-%s.gci", CARD_DIR, (const char*) ent->company,
             (const char*) ent->gameName, name);
}

static void write_slot(int i)
{
    char path[96];
    u32 bytes;
    BOOL enabled = OSDisableInterrupts();
    Slot* slot = &s_slots[i];
    if (!slot->dirty || slot->ent.gameName[0] == 0xFF) {
        slot->dirty = FALSE;
        OSRestoreInterrupts(enabled);
        return;
    }
    slot->dirty = FALSE;
    bytes = (u32) slot->ent.length * CARD_SECTOR_SIZE;
    memcpy(s_write_buffer, &slot->ent, CARD_ENTRY_SIZE);
    memcpy(s_write_buffer + CARD_ENTRY_SIZE, slot->data, bytes);
    gci_path(&slot->ent, path, sizeof(path));
    BOOL renamed = slot->path[0] != '\0' && strcmp(slot->path, path) != 0;
    char old[96];
    strcpy(old, slot->path);
    strcpy(slot->path, path);
    OSRestoreInterrupts(enabled);

    if (renamed) {
        wl_platform_fs_remove(old);
    }
    WlPlatformFile* file;
    WlResult r = wl_platform_fs_open(path, WL_FILE_WRITE_NEW, &file);
    if (r == WL_OK) {
        r = wl_platform_fs_write(file, s_write_buffer, CARD_ENTRY_SIZE + bytes);
        if (r == WL_OK) {
            r = wl_platform_fs_flush(file);
        }
        wl_platform_fs_close(file);
    }
    if (r != WL_OK) {
        OSReport("CARD: writing %s failed (%s)\n", path, wl_result_name(r));
    } else {
        OSReport("CARD: wrote %s\n", path);
    }
}

static void* writer_main(void* arg)
{
    (void) arg;
    for (;;) {
        BOOL enabled = OSDisableInterrupts();
        BOOL work = FALSE;
        for (int i = 0; i < CARD_MAX_FILE && !work; i++) {
            work = s_slots[i].dirty || s_doomed_paths[i][0] != '\0';
        }
        if (!work) {
            KThrQueueBlock(&s_writer_wait, (uintptr_t) &s_writer_wait);
        }
        OSRestoreInterrupts(enabled);

        for (int i = 0; i < CARD_MAX_FILE; i++) {
            char doomed[96];
            enabled = OSDisableInterrupts();
            strcpy(doomed, s_doomed_paths[i]);
            s_doomed_paths[i][0] = '\0';
            OSRestoreInterrupts(enabled);
            if (doomed[0] != '\0') {
                wl_platform_fs_remove(doomed);
                OSReport("CARD: removed %s\n", doomed);
            }
            write_slot(i);
        }
    }
    return NULL;
}

static void load(void)
{
    DIR* dir = opendir(CARD_DIR);
    struct dirent* d;
    int n = 0;
    if (dir == NULL) {
        return;
    }
    while ((d = readdir(dir)) != NULL) {
        size_t len = strlen(d->d_name);
        if (len < 5 || strcasecmp(d->d_name + len - 4, ".gci") != 0) {
            continue;
        }
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", CARD_DIR, d->d_name);
        WlPlatformFile* file;
        if (wl_platform_fs_open(path, WL_FILE_READ, &file) != WL_OK) {
            continue;
        }
        CARDDir ent;
        u64 size = wl_platform_fs_size(file);
        if (size < CARD_ENTRY_SIZE ||
            wl_platform_fs_read_at(file, 0, &ent, CARD_ENTRY_SIZE) != WL_OK ||
            size != CARD_ENTRY_SIZE + (u64) ent.length * CARD_SECTOR_SIZE ||
            ent.length == 0 || ent.gameName[0] == 0xFF || n >= CARD_MAX_FILE)
        {
            OSReport("CARD: %s is not a save, skipped\n", path);
            wl_platform_fs_close(file);
            continue;
        }
        Slot* slot = &s_slots[n];
        slot->data = memalign(32, ent.length * CARD_SECTOR_SIZE);
        if (slot->data == NULL ||
            wl_platform_fs_read_at(file, CARD_ENTRY_SIZE, slot->data,
                                   ent.length * CARD_SECTOR_SIZE) != WL_OK)
        {
            OSReport("CARD: %s could not be read, skipped\n", path);
            free(slot->data);
            slot->data = NULL;
            wl_platform_fs_close(file);
            continue;
        }
        wl_platform_fs_close(file);
        slot->ent = ent;
        slot->ent.startBlock = s_next_block;
        s_next_block += ent.length;
        strcpy(slot->path, path);
        OSReport("CARD: %s, %u blocks\n", path, (unsigned) ent.length);
        n++;
    }
    closedir(dir);
}

void CARDInit(void)
{
    static BOOL initialized;
    if (initialized) {
        return;
    }
    initialized = TRUE;
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        memset(&s_slots[i].ent, 0xFF, sizeof(CARDDir));
    }
    if (!wl_platform_fs_mounted(WL_DEVICE_SD)) {
        OSReport("CARD: no SD card: no memory card in slot A\n");
        return;
    }
    wl_platform_fs_mkdir("sd:/apps");
    wl_platform_fs_mkdir("sd:/apps/melee");
    wl_platform_fs_mkdir(CARD_DIR);
    s_write_buffer = memalign(32, CARD_ENTRY_SIZE + CARD_USER_BLOCKS * CARD_SECTOR_SIZE);
    if (s_write_buffer == NULL) {
        OSReport("CARD: no memory for the writer\n");
        return;
    }
    load();
    s_present = TRUE;
    LWP_CreateThread(&s_writer, writer_main, NULL, s_writer_stack,
                     sizeof(s_writer_stack), MELEE_WII_PRIO_IO - 10);
}

static s32 used_blocks(void)
{
    s32 used = 0;
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        if (s_slots[i].ent.gameName[0] != 0xFF) {
            used += s_slots[i].ent.length;
        }
    }
    return used;
}

static BOOL ok_chan(s32 chan)
{
    return chan == 0 && s_present;
}

/* CARDOpen.c's __CARDAccess and __CARDIsPublic */
static s32 access_entry(const CARDDir* ent)
{
    const DVDDiskID* id = DVDGetCurrentDiskID();
    if (ent->gameName[0] == 0xFF) {
        return CARD_RESULT_NOFILE;
    }
    if (memcmp(ent->gameName, id->gameName, 4) == 0 &&
        memcmp(ent->company, id->company, 2) == 0)
    {
        return CARD_RESULT_READY;
    }
    return CARD_RESULT_NOPERM;
}

/* CARDOpen.c's __CARDCompareFileName */
static BOOL same_name(const CARDDir* ent, const char* fileName)
{
    const char* entName = (const char*) ent->fileName;
    int n = CARD_FILENAME_MAX;
    while (--n >= 0) {
        char c1 = *entName++;
        char c2 = *fileName++;
        if (c1 != c2) {
            return FALSE;
        }
        if (c2 == '\0') {
            return TRUE;
        }
    }
    return *fileName == '\0';
}

static s32 find(const char* fileName)
{
    for (s32 i = 0; i < CARD_MAX_FILE; i++) {
        if (access_entry(&s_slots[i].ent) >= 0 &&
            same_name(&s_slots[i].ent, fileName))
        {
            return i;
        }
    }
    return -1;
}

BOOL CARDProbe(long chan)
{
    return ok_chan(chan);
}

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
    if (!ok_chan(chan)) {
        return CARD_RESULT_NOCARD;
    }
    if (memSize != NULL) {
        *memSize = CARD_MEM_SIZE;
    }
    if (sectorSize != NULL) {
        *sectorSize = CARD_SECTOR_SIZE;
    }
    return CARD_RESULT_READY;
}

s32 CARDMountAsync(s32 chan, void* workArea, CARDCallback detachCallback,
                   CARDCallback attachCallback)
{
    (void) workArea;
    (void) detachCallback;
    if (!ok_chan(chan)) {
        return CARD_RESULT_NOCARD;
    }
    s_mounted = TRUE;
    return complete(attachCallback, chan, CARD_RESULT_READY, 0);
}

s32 CARDUnmount(s32 chan)
{
    if (!ok_chan(chan)) {
        return CARD_RESULT_NOCARD;
    }
    s_mounted = FALSE;
    return CARD_RESULT_READY;
}

static s32 mounted(s32 chan)
{
    if (!ok_chan(chan)) {
        return CARD_RESULT_NOCARD;
    }
    return s_mounted ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
}

s32 CARDCheckAsync(s32 chan, CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

s32 CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    BOOL enabled = OSDisableInterrupts();
    s32 files = 0;
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        if (s_slots[i].ent.gameName[0] == 0xFF) {
            files++;
        }
    }
    if (byteNotUsed != NULL) {
        *byteNotUsed = (CARD_USER_BLOCKS - used_blocks()) * CARD_SECTOR_SIZE;
    }
    if (filesNotUsed != NULL) {
        *filesNotUsed = files;
    }
    OSRestoreInterrupts(enabled);
    return CARD_RESULT_READY;
}

s32 CARDFormatAsync(s32 chan, CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    BOOL enabled = OSDisableInterrupts();
    for (int i = 0; i < CARD_MAX_FILE; i++) {
        Slot* slot = &s_slots[i];
        if (slot->path[0] != '\0') {
            strcpy(s_doomed_paths[i], slot->path);
        }
        free(slot->data);
        slot->data = NULL;
        slot->path[0] = '\0';
        slot->dirty = FALSE;
        memset(&slot->ent, 0xFF, sizeof(CARDDir));
    }
    OSRestoreInterrupts(enabled);
    schedule_write();
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

static void fill_info(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    fileInfo->chan = chan;
    fileInfo->fileNo = fileNo;
    fileInfo->offset = 0;
    fileInfo->length = (s32) s_slots[fileNo].ent.length * CARD_SECTOR_SIZE;
    fileInfo->iBlock = s_slots[fileNo].ent.startBlock;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    BOOL enabled = OSDisableInterrupts();
    s32 fileNo = find(fileName);
    if (fileNo >= 0) {
        fill_info(chan, fileNo, fileInfo);
        r = CARD_RESULT_READY;
    } else {
        r = CARD_RESULT_NOFILE;
    }
    OSRestoreInterrupts(enabled);
    return r;
}

s32 CARDFastOpen(s32 chan, s32 fileNo, CARDFileInfo* fileInfo)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    BOOL enabled = OSDisableInterrupts();
    r = access_entry(&s_slots[fileNo].ent);
    if (r == CARD_RESULT_NOPERM && (s_slots[fileNo].ent.permission & CARD_ATTR_PUBLIC)) {
        r = CARD_RESULT_READY;
    }
    if (r >= 0) {
        fill_info(chan, fileNo, fileInfo);
    }
    OSRestoreInterrupts(enabled);
    return r;
}

s32 CARDClose(CARDFileInfo* fileInfo)
{
    fileInfo->chan = -1;
    return CARD_RESULT_READY;
}

s32 CARDCreateAsync(s32 chan, const char* fileName, u32 size,
                    CARDFileInfo* fileInfo, CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    if (strlen(fileName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    if (size == 0 || size % CARD_SECTOR_SIZE != 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    u16 blocks = size / CARD_SECTOR_SIZE;
    BOOL enabled = OSDisableInterrupts();
    if (find(fileName) >= 0) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_EXIST;
    }
    s32 fileNo = -1;
    for (s32 i = 0; i < CARD_MAX_FILE; i++) {
        if (s_slots[i].ent.gameName[0] == 0xFF) {
            fileNo = i;
            break;
        }
    }
    if (fileNo < 0) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_NOENT;
    }
    if (used_blocks() + blocks > CARD_USER_BLOCKS) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_INSSPACE;
    }
    OSRestoreInterrupts(enabled);

    u8* data = memalign(32, size);
    if (data == NULL) {
        return CARD_RESULT_INSSPACE;
    }
    memset(data, 0, size);

    enabled = OSDisableInterrupts();
    Slot* slot = &s_slots[fileNo];
    const DVDDiskID* id = DVDGetCurrentDiskID();
    CARDDir* ent = &slot->ent;
    memset(ent, 0, sizeof(*ent));
    memcpy(ent->gameName, id->gameName, 4);
    memcpy(ent->company, id->company, 2);
    ent->_padding0 = 0xFF;
    ent->_padding1[0] = ent->_padding1[1] = 0xFF;
    strncpy((char*) ent->fileName, fileName, CARD_FILENAME_MAX);
    ent->time = (u32) OSTicksToSeconds(OSGetTime());
    ent->iconAddr = 0xFFFFFFFF;
    ent->commentAddr = 0xFFFFFFFF;
    ent->iconSpeed = CARD_STAT_SPEED_FAST;
    ent->permission = CARD_ATTR_PUBLIC;
    ent->startBlock = s_next_block;
    s_next_block += blocks;
    ent->length = blocks;
    slot->data = data;
    slot->path[0] = '\0';
    slot->dirty = TRUE;
    fill_info(chan, fileNo, fileInfo);
    OSRestoreInterrupts(enabled);
    schedule_write();
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

static s32 transfer(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset,
                    CARDCallback callback, BOOL write)
{
    s32 chan = fileInfo->chan;
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    s32 fileNo = fileInfo->fileNo;
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE || length < 0 || offset < 0) {
        return CARD_RESULT_FATAL_ERROR;
    }
    Slot* slot = &s_slots[fileNo];
    if (slot->ent.gameName[0] == 0xFF) {
        return CARD_RESULT_NOFILE;
    }
    if (offset + length > (s32) slot->ent.length * CARD_SECTOR_SIZE) {
        return CARD_RESULT_LIMIT;
    }
    if (write) {
        memcpy(slot->data + offset, buf, length);
        slot->ent.time = (u32) OSTicksToSeconds(OSGetTime());
        slot->dirty = TRUE;
        schedule_write();
    } else {
        memcpy(buf, slot->data + offset, length);
        /* as the card's DMA would leave it: in memory */
        DCFlushRange(buf, length);
    }
    fileInfo->offset = offset + length;
    return complete(callback, chan, CARD_RESULT_READY, length);
}

s32 CARDReadAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset,
                  CARDCallback callback)
{
    return transfer(fileInfo, buf, length, offset, callback, FALSE);
}

s32 CARDWriteAsync(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset,
                   CARDCallback callback)
{
    return transfer(fileInfo, buf, length, offset, callback, TRUE);
}

s32 CARDDeleteAsync(s32 chan, char* fileName, CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    BOOL enabled = OSDisableInterrupts();
    s32 fileNo = find(fileName);
    if (fileNo < 0) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_NOFILE;
    }
    Slot* slot = &s_slots[fileNo];
    if (slot->path[0] != '\0') {
        strcpy(s_doomed_paths[fileNo], slot->path);
    }
    free(slot->data);
    slot->data = NULL;
    slot->path[0] = '\0';
    slot->dirty = FALSE;
    memset(&slot->ent, 0xFF, sizeof(CARDDir));
    OSRestoreInterrupts(enabled);
    schedule_write();
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

s32 CARDRenameAsync(s32 chan, const char* oldName, const char* newName,
                    CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    if (strlen(newName) > CARD_FILENAME_MAX) {
        return CARD_RESULT_NAMETOOLONG;
    }
    BOOL enabled = OSDisableInterrupts();
    s32 fileNo = find(oldName);
    if (fileNo < 0) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_NOFILE;
    }
    if (find(newName) >= 0) {
        OSRestoreInterrupts(enabled);
        return CARD_RESULT_EXIST;
    }
    CARDDir* ent = &s_slots[fileNo].ent;
    memset(ent->fileName, 0, CARD_FILENAME_MAX);
    strncpy((char*) ent->fileName, newName, CARD_FILENAME_MAX);
    ent->time = (u32) OSTicksToSeconds(OSGetTime());
    s_slots[fileNo].dirty = TRUE;
    OSRestoreInterrupts(enabled);
    schedule_write();
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

/* CARDStat.c's UpdateIconOffsets */
static void icon_offsets(const CARDDir* ent, CARDStat* stat)
{
    u32 offset = ent->iconAddr;
    BOOL iconTlut = FALSE;
    if (offset == 0xFFFFFFFF) {
        stat->bannerFormat = 0;
        stat->iconFormat = 0;
        stat->iconSpeed = 0;
        offset = 0;
    }
    switch (ent->bannerFormat & 3) {
    case CARD_STAT_BANNER_C8:
        stat->offsetBanner = offset;
        offset += BANNER_WIDTH * BANNER_HEIGHT;
        stat->offsetBannerTlut = offset;
        offset += 2 * 256;
        break;
    case CARD_STAT_BANNER_RGB5A3:
        stat->offsetBanner = offset;
        offset += 2 * BANNER_WIDTH * BANNER_HEIGHT;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    default:
        stat->offsetBanner = 0xFFFFFFFF;
        stat->offsetBannerTlut = 0xFFFFFFFF;
        break;
    }
    for (int i = 0; i < CARD_ICON_MAX; i++) {
        switch ((ent->iconFormat >> (2 * i)) & 3) {
        case CARD_STAT_ICON_C8:
            stat->offsetIcon[i] = offset;
            offset += ICON_WIDTH * ICON_HEIGHT;
            iconTlut = TRUE;
            break;
        case CARD_STAT_ICON_RGB5A3:
            stat->offsetIcon[i] = offset;
            offset += 2 * ICON_WIDTH * ICON_HEIGHT;
            break;
        default:
            stat->offsetIcon[i] = 0xFFFFFFFF;
            break;
        }
    }
    if (iconTlut) {
        stat->offsetIconTlut = offset;
        offset += 2 * 256;
    } else {
        stat->offsetIconTlut = 0xFFFFFFFF;
    }
    stat->offsetData = offset;
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE) {
        return CARD_RESULT_FATAL_ERROR;
    }
    BOOL enabled = OSDisableInterrupts();
    const CARDDir* ent = &s_slots[fileNo].ent;
    r = access_entry(ent);
    if (r == CARD_RESULT_NOPERM) {
        r = (ent->permission & CARD_ATTR_PUBLIC) ? CARD_RESULT_READY
                                                 : CARD_RESULT_NOPERM;
    }
    if (r >= 0) {
        memcpy(stat->gameName, ent->gameName, sizeof(stat->gameName));
        memcpy(stat->company, ent->company, sizeof(stat->company));
        stat->length = (u32) ent->length * CARD_SECTOR_SIZE;
        memcpy(stat->fileName, ent->fileName, CARD_FILENAME_MAX);
        stat->time = ent->time;
        stat->bannerFormat = ent->bannerFormat;
        stat->iconAddr = ent->iconAddr;
        stat->iconFormat = ent->iconFormat;
        stat->iconSpeed = ent->iconSpeed;
        stat->commentAddr = ent->commentAddr;
        icon_offsets(ent, stat);
    }
    OSRestoreInterrupts(enabled);
    return r;
}

s32 CARDSetStatusAsync(s32 chan, s32 fileNo, CARDStat* stat,
                       CARDCallback callback)
{
    s32 r = mounted(chan);
    if (r < 0) {
        return r;
    }
    if (fileNo < 0 || fileNo >= CARD_MAX_FILE ||
        (stat->iconAddr != 0xFFFFFFFF && CARD_READ_SIZE <= stat->iconAddr) ||
        (stat->commentAddr != 0xFFFFFFFF &&
         CARD_SYSTEM_BLOCK_SIZE - COMMENT_SIZE <
             stat->commentAddr % CARD_SYSTEM_BLOCK_SIZE))
    {
        return CARD_RESULT_FATAL_ERROR;
    }
    BOOL enabled = OSDisableInterrupts();
    CARDDir* ent = &s_slots[fileNo].ent;
    r = access_entry(ent);
    if (r < 0) {
        OSRestoreInterrupts(enabled);
        return r;
    }
    ent->bannerFormat = stat->bannerFormat;
    ent->iconAddr = stat->iconAddr;
    ent->iconFormat = stat->iconFormat;
    ent->iconSpeed = stat->iconSpeed;
    ent->commentAddr = stat->commentAddr;
    icon_offsets(ent, stat);
    if (ent->iconAddr == 0xFFFFFFFF) {
        ent->iconSpeed = (ent->iconSpeed & ~3) | CARD_STAT_SPEED_FAST;
    }
    ent->time = (u32) OSTicksToSeconds(OSGetTime());
    s_slots[fileNo].dirty = TRUE;
    OSRestoreInterrupts(enabled);
    schedule_write();
    return complete(callback, chan, CARD_RESULT_READY, 0);
}

long CARDGetXferredBytes(long chan)
{
    (void) chan;
    return s_xferred;
}

/* The synchronous forms: the transfer is done when the call returns */
long CARDRead(struct CARDFileInfo* fileInfo, void* buf, long length, long offset)
{
    return transfer(fileInfo, buf, length, offset, NULL, FALSE);
}

s32 CARDWrite(CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset)
{
    return transfer(fileInfo, buf, length, offset, NULL, TRUE);
}
