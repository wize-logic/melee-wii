/* The SDK's DVD on the player's own copy of the disc: a GameCube image
 * (.iso/.gcm) on the SD card or a USB drive, opened with wiilib's GameCube
 * disc reader (wl/gcm) and read through its file API (wl/platform).
 *
 * Reads are queued to an IO thread that runs above the game's threads; it is
 * woken, and a read's callback called, from interrupt context
 * (melee_wii_defer), as the SDK's DVD interrupt would. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <ctype.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <unistd.h>

#include <wl/base/base.h>
#include <wl/gcm/gcm.h>
#include <wl/platform/platform.h>

/* Where the image is looked for, in order */
static const char* const s_image_paths[] = {
    "sd:/apps/melee/game.iso",  "sd:/apps/melee/GALE01.iso",
    "sd:/games/GALE01.iso",     "sd:/games/melee.iso",
    "sd:/melee.iso",            "usb:/apps/melee/game.iso",
    "usb:/apps/melee/GALE01.iso", "usb:/games/GALE01.iso",
    "usb:/games/melee.iso",     "usb:/melee.iso",
};

static WlPlatformFile* s_file;
static WlGcm s_gcm;
static DVDDiskID s_disk_id ATTRIBUTE_ALIGN(32);
static const char* s_image_path;

/* The FST, as the SDK keeps it: 12-byte entries, then the names */
static const u8* s_fst;
static u32 s_entries;
static const char* s_names;
static BOOL s_dvdlog; /* tests: reads and lookups logged (dvdlog_read) */

/* A second image beside the game's: a 20XX image, whose songs the game may be
 * given in place of its own (mod_songs.c). Its files' entry numbers follow
 * the game's (s_entries + its own), and their offsets have the top bit set
 * (an image is under 2 GB), which is how a read knows its image. Nothing of
 * this exists until melee_wii_disc2_open. */
#define SECOND_IMAGE 0x80000000u
static WlPlatformFile* s_file2;
static WlGcm s_gcm2;
static s32 (*s_substitute)(const char* path);
static void (*s_after_read)(int second, u32 offset, void* buf, u32 len);
/* A stretch of the game's image whose bytes come from the second image
 * while *s_overlay_on (20XX's portraits, where the character select screen's
 * file has its own: mod_stages.c) */
static u32 s_overlay_from, s_overlay_to, s_overlay_other;
static const u32* s_overlay_on;

/* The queue of reads (linked by their command blocks) and the thread */
static DVDCommandBlock* s_head;
static DVDCommandBlock* s_tail;
static DVDCommandBlock* volatile s_current;
static volatile BOOL s_stopping; /* leaving: no more reads (melee_wii_dvd_stop) */
static DVDCommandBlock* s_done_head;
static DVDCommandBlock* s_done_tail;
static ogc_KThrQueue s_wait;
static MeleeWiiDefer s_kick;
static MeleeWiiDefer s_complete;
static ogc_lwp_t s_thread;
/* (stall.c's report) */
static OSTime s_current_start, s_last_done;
static u32 s_reads_queued, s_reads_done;
static u8 s_stack[32 * 1024] ATTRIBUTE_ALIGN(32);

#ifdef MELEE_ORIG_CODE
/* The SDK's dvdfs.c keeps the FST in statics (config/GALE01/symbols.txt:
 * FstStart, FstStringStart, MaxEntryNum) that only its own DVD code, which
 * the port replaces, sets. A mod's code may read them itself (20XX's file
 * lookup does), so they point at the port's copy. */
#define SDK_FST_START 0x804D727Cu
#define SDK_FST_STRING_START 0x804D7280u
#define SDK_MAX_ENTRY_NUM 0x804D7284u

static void publish_fst(void)
{
    *(volatile u32*) SDK_FST_START = (u32) s_fst;
    *(volatile u32*) SDK_FST_STRING_START = (u32) s_names;
    *(volatile u32*) SDK_MAX_ENTRY_NUM = s_entries;
}
#else
static void publish_fst(void) {}
#endif

static WlResult read_image(void* ctx, uint64_t off, void* buf, size_t n)
{
    return wl_platform_fs_read_at(ctx, off, buf, (uint32_t) n);
}

static void* io_thread(void* arg);
void so_gamelog_event(int code, u32 us) __attribute__((weak));
static void kick(void* arg);
static void complete(void* arg);

static u32 be32(const u8* p)
{
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

static BOOL open_path(const char* path)
{
    WlPlatformFile* file;
    if (!wl_platform_fs_exists(path) || wl_platform_fs_open(path, WL_FILE_READ, &file) != WL_OK) {
        return FALSE;
    }
    WlResult r = wl_gcm_open_src(&s_gcm, read_image, NULL, file, wl_platform_fs_size(file));
    if (r != WL_OK) {
        OSReport("DVD: %s is not a GameCube image (%s)\n", path, wl_result_name(r));
        wl_platform_fs_close(file);
        return FALSE;
    }
    s_file = file;
    s_image_path = path;
    return TRUE;
}

/* The launcher (before the game's DVDInit): play this image instead (20XX);
 * 0, or -1 when it can't be opened */
int melee_wii_disc_use(const char* path)
{
    static char chosen[128];
    if (s_file != NULL) {
        wl_platform_fs_close(s_file);
        s_file = NULL;
        s_fst = NULL;
    }
    snprintf(chosen, sizeof(chosen), "%s", path);
    if (!open_path(chosen)) {
        return -1;
    }
    s_fst = s_gcm.fst;
    s_entries = s_gcm.entry_count;
    s_names = (const char*) s_fst + s_entries * 12;
    return 0;
}

static BOOL open_image(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(s_image_paths); i++) {
        const char* path = s_image_paths[i];
        if (strncmp(path, "usb:", 4) == 0 &&
            !wl_platform_fs_mounted(WL_DEVICE_USB))
        {
            if (wl_platform_fs_mount(WL_DEVICE_USB) != WL_OK) {
                break;
            }
        }
        if (open_path(path)) {
            return TRUE;
        }
    }
    return FALSE;
}

void DVDInit(void)
{
    static BOOL started;
    if (started) {
        return;
    }
    started = TRUE;
    /* (the launcher may have opened the image already) */
    if (s_file == NULL && !open_image()) {
        melee_wii_fatal(
            "No copy of the game was found.\n\n"
            "Put your own dump of Super Smash Bros. Melee (NTSC-U 1.02,\n"
            "GALE01) on the SD card or a USB drive as\n"
            "  /apps/melee/game.iso  or  /games/melee.iso");
    }
    if (wl_gcm_read(&s_gcm, 0, &s_disk_id, sizeof(s_disk_id)) != WL_OK ||
        memcmp(s_disk_id.gameName, "GALE", 4) != 0 ||
        memcmp(s_disk_id.company, "01", 2) != 0)
    {
        melee_wii_fatal("%s is %.6s, not Super Smash Bros. Melee (GALE01)",
                        s_image_path, s_disk_id.gameName);
    }
    if (s_disk_id.gameVersion != 2) {
        OSReport("DVD: %s is revision %d; the build is of revision 2 (1.02)\n",
                 s_image_path, s_disk_id.gameVersion);
    }
    s_fst = s_gcm.fst;
    s_entries = s_gcm.entry_count;
    s_names = (const char*) s_fst + s_entries * 12;
    publish_fst();
    s_dvdlog = wl_platform_fs_exists("sd:/apps/melee/dvdlog");
    OSReport("DVD: %s, %u FST entries\n", s_image_path, (unsigned) s_entries);

    s_kick.fn = kick;
    s_complete.fn = complete;
    LWP_CreateThread(&s_thread, io_thread, NULL, s_stack, sizeof(s_stack),
                     MELEE_WII_PRIO_IO);
}

/* dvdfs.c: the names compare without case, '/' separates, "." and ".." */
static s32 s_current_dir;

static BOOL is_dir(u32 entry)
{
    return s_fst[entry * 12] != 0;
}

static const char* name_of(u32 entry)
{
    return s_names + (be32(&s_fst[entry * 12]) & 0xFFFFFF);
}

/* Tests (sd:/apps/melee/dvdlog): each read and path lookup, logged */

static void dvdlog_read(u32 offset, u32 length, u32 addr)
{
    const char* name = "?";
    if ((offset & SECOND_IMAGE) && s_file2 != NULL) {
        OSReport("dvdlog: (the second image) +0x%x, 0x%x bytes -> %08x\n", (unsigned) (offset & ~SECOND_IMAGE),
                 (unsigned) length, (unsigned) addr);
        return;
    }
    for (u32 e = 1; e < s_entries; e++) {
        u32 pos = s_fst[e * 12 + 4] << 24 | s_fst[e * 12 + 5] << 16 | s_fst[e * 12 + 6] << 8 | s_fst[e * 12 + 7];
        u32 len = s_fst[e * 12 + 8] << 24 | s_fst[e * 12 + 9] << 16 | s_fst[e * 12 + 10] << 8 | s_fst[e * 12 + 11];
        if (s_fst[e * 12] == 0 && offset >= pos && offset < pos + len) {
            name = name_of(e);
            offset -= pos;
            break;
        }
    }
    OSReport("dvdlog: %s +0x%x, 0x%x bytes -> %08x\n", name, (unsigned) offset, (unsigned) length,
             (unsigned) addr);
}

static BOOL same_name(const char* path, size_t len, const char* name)
{
    for (size_t i = 0; i < len; i++) {
        if (name[i] == '\0' ||
            tolower((unsigned char) path[i]) != tolower((unsigned char) name[i]))
        {
            return FALSE;
        }
    }
    return name[len] == '\0';
}

static s32 find_path(const u8* fst, u32 entries, u32 dir, const char* path);

s32 DVDConvertPathToEntrynum(const char* path)
{
    s32 entry = s_substitute != NULL ? s_substitute(path) : -1;
    if (entry < 0) {
        entry = s_fst != NULL ? find_path(s_fst, s_entries, s_current_dir, path) : -1;
    }
    if (s_dvdlog) {
        OSReport("dvdlog: \"%s\" is entry %d\n", path != NULL ? path : "(null)", (int) entry);
    }
    if (entry < 0) {
        OSReport("DVD: no file \"%s\" on the disc\n", path != NULL ? path : "(null)");
    }
    return entry;
}

/* The entry of `path` in an FST, from the directory `dir` */
static s32 find_path(const u8* fst, u32 entries, u32 dir, const char* path)
{
    const char* names = (const char*) fst + entries * 12;
    if (path == NULL) {
        return -1;
    }
    while (*path != '\0') {
        if (*path == '/') {
            dir = 0;
            path++;
            continue;
        }
        size_t len = 0;
        while (path[len] != '\0' && path[len] != '/') {
            len++;
        }
        if (len == 1 && path[0] == '.') {
            /* stays */
        } else if (len == 2 && path[0] == '.' && path[1] == '.') {
            dir = be32(&fst[dir * 12 + 4]);
        } else {
            u32 found = 0;
            u32 end = be32(&fst[dir * 12 + 8]);
            for (u32 e = dir + 1; e < end && e < entries;
                 e = fst[e * 12] != 0 ? be32(&fst[e * 12 + 8]) : e + 1)
            {
                if (same_name(path, len, names + (be32(&fst[e * 12]) & 0xFFFFFF))) {
                    found = e;
                    break;
                }
            }
            if (found == 0) {
                return -1;
            }
            if (path[len] == '\0') {
                return found;
            }
            if (fst[found * 12] == 0) {
                return -1;
            }
            dir = found;
        }
        path += len;
        if (*path == '/') {
            path++;
        }
    }
    return dir;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo)
{
    if (s_file2 != NULL && entrynum > 0 && (u32) entrynum >= s_entries) {
        u32 e = (u32) entrynum - s_entries;
        if (e == 0 || e >= s_gcm2.entry_count || s_gcm2.fst[e * 12] != 0) {
            return FALSE;
        }
        fileInfo->startAddr = SECOND_IMAGE | be32(&s_gcm2.fst[e * 12 + 4]);
        fileInfo->length = be32(&s_gcm2.fst[e * 12 + 8]);
        fileInfo->callback = NULL;
        fileInfo->cb.state = DVD_STATE_END;
        return TRUE;
    }
    if (entrynum <= 0 || (u32) entrynum >= s_entries || is_dir(entrynum)) {
        return FALSE;
    }
    fileInfo->startAddr = be32(&s_fst[entrynum * 12 + 4]);
    fileInfo->length = be32(&s_fst[entrynum * 12 + 8]);
    fileInfo->callback = NULL;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fileInfo)
{
    s32 entry = DVDConvertPathToEntrynum(fileName);
    return entry >= 0 && DVDFastOpen(entry, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo)
{
    (void) fileInfo;
    return TRUE;
}

static void dvd_callback(s32 result, DVDCommandBlock* block)
{
    DVDFileInfo* fileInfo = (DVDFileInfo*) block;
    if (fileInfo->callback != NULL) {
        fileInfo->callback(result, fileInfo);
    }
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length,
                      s32 offset, DVDCallback callback, s32 prio)
{
    (void) prio;
    DVDCommandBlock* block = &fileInfo->cb;
    fileInfo->callback = callback;
    block->next = NULL;
    block->prev = NULL;
    block->command = 1; /* read */
    block->state = DVD_STATE_WAITING;
    block->offset = fileInfo->startAddr + offset;
    block->length = length;
    block->addr = addr;
    block->currTransferSize = 0;
    block->transferredSize = 0;
    block->callback = dvd_callback;

    BOOL enabled = OSDisableInterrupts();
    if (s_tail != NULL) {
        s_tail->next = block;
        block->prev = s_tail;
    } else {
        s_head = block;
    }
    s_tail = block;
    s_reads_queued++;
    melee_wii_defer(&s_kick);
    OSRestoreInterrupts(enabled);
    return TRUE;
}

/* Interrupt context: the IO thread, woken here, runs once it returns */
static void kick(void* arg)
{
    (void) arg;
    KThrQueueUnblockAllByValue(&s_wait, (uintptr_t) &s_head);
}

/* Interrupt context: the finished reads' callbacks */
static void complete(void* arg)
{
    (void) arg;
    while (s_done_head != NULL) {
        DVDCommandBlock* block = s_done_head;
        s_done_head = block->next;
        if (s_done_head == NULL) {
            s_done_tail = NULL;
        }
        block->next = NULL;
        s32 result = block->state == DVD_STATE_END ? (s32) block->length
                                                    : DVD_RESULT_FATAL_ERROR;
        if (block->callback != NULL) {
            block->callback(result, block);
        }
    }
}

/* The last reads into memory (where and how much): for the rollback
 * measurement (diag_dirty.c), which tells file data from the rest */
#define LOAD_RING 1024
static u32 s_load_addr[LOAD_RING], s_load_len[LOAD_RING];
static u32 s_load_count;

void melee_wii_note_load(u32 addr, u32 len)
{
    /* never into the game's code (0x80003100-0x803B7240): a load there is a
     * bug, said at once */
    u32 a = (addr & 0x3FFFFFFFu) | 0x80000000u;
    if (a < 0x803B7240u && a + len > 0x80003100u) {
        OSReport("LOAD INTO THE GAME'S CODE: %08x + %x\n", (unsigned) addr, (unsigned) len);
    }
    s_load_addr[s_load_count % LOAD_RING] = addr;
    s_load_len[s_load_count % LOAD_RING] = len;
    s_load_count++;
}

u32 melee_wii_dvd_loads(const u32** addr, const u32** len)
{
    *addr = s_load_addr;
    *len = s_load_len;
    return s_load_count;
}

static void* io_thread(void* arg)
{
    (void) arg;
    for (;;) {
        BOOL enabled = OSDisableInterrupts();
        while (s_head == NULL || s_stopping) {
            KThrQueueBlock(&s_wait, (uintptr_t) &s_head);
        }
        DVDCommandBlock* block = s_head;
        s_head = block->next;
        if (s_head == NULL) {
            s_tail = NULL;
        } else {
            s_head->prev = NULL;
        }
        block->next = NULL;
        block->state = DVD_STATE_BUSY;
        s_current = block;
        s_current_start = OSGetTime();
        OSRestoreInterrupts(enabled);

        u32 length = block->length;
        u32 offset = block->offset;
        WlPlatformFile* file = s_file;
        u64 size = s_gcm.size;
        if (s_file2 != NULL && (offset & SECOND_IMAGE)) {
            offset &= ~SECOND_IMAGE;
            file = s_file2;
            size = s_gcm2.size;
        }
        if ((u64) offset + length > size) {
            length = offset < size ? (u32) (size - offset) : 0;
        }
        WlResult r = WL_OK;
        if (length != 0 && file == s_file && s_file2 != NULL && s_overlay_on != NULL && *s_overlay_on != 0 &&
            offset < s_overlay_to && offset + length > s_overlay_from)
        {
            /* in three parts: the game's own before the stretch, the second
             * image's for it, the game's own after */
            u32 from = offset > s_overlay_from ? offset : s_overlay_from;
            u32 to = offset + length < s_overlay_to ? offset + length : s_overlay_to;
            if (from > offset) {
                r = wl_platform_fs_read_at(s_file, offset, block->addr, from - offset);
            }
            if (r == WL_OK) {
                r = wl_platform_fs_read_at(s_file2, s_overlay_other + (from - s_overlay_from),
                                           (u8*) block->addr + (from - offset), to - from);
            }
            if (r == WL_OK && offset + length > to) {
                r = wl_platform_fs_read_at(s_file, to, (u8*) block->addr + (to - offset), offset + length - to);
            }
        } else if (length != 0) {
            r = wl_platform_fs_read_at(file, offset, block->addr, length);
        }
        if (so_gamelog_event != NULL) {
            /* (slippi/so_gamelog.c: the Diagnostics setting's record of an online game) */
            so_gamelog_event('D', (u32) OSTicksToMicroseconds(OSGetTime() - s_current_start));
        }
        if (r == WL_OK && s_after_read != NULL) {
            s_after_read(file != s_file, offset, block->addr, length);
        }
        /* The SDK's DMA writes memory under the cache: the game may
         * invalidate the lines over the buffer after the read */
        DCFlushRange(block->addr, block->length);
        melee_wii_note_load((u32) block->addr, block->length);
        if (s_dvdlog) {
            dvdlog_read(block->offset, block->length, (u32) block->addr);
        }
        /* (IOS's DMA sets no page's Changed bit: rollback's tracking) */
        melee_wii_mmu_touch((u32) block->addr, block->length);

        if (r != WL_OK) {
            OSReport("DVD: read of 0x%x bytes at 0x%x failed (%s)\n",
                     (unsigned) block->length, (unsigned) block->offset,
                     wl_result_name(r));
        }
        enabled = OSDisableInterrupts();
        s_current = NULL;
        s_reads_done++;
        s_last_done = OSGetTime();
        if (r == WL_OK) {
            block->transferredSize = block->length;
            block->state = DVD_STATE_END;
        } else {
            block->state = DVD_STATE_FATAL_ERROR;
        }
        if (s_done_tail != NULL) {
            s_done_tail->next = block;
        } else {
            s_done_head = block;
        }
        s_done_tail = block;
        melee_wii_defer(&s_complete);
        OSRestoreInterrupts(enabled);
    }
    return NULL;
}

/* Leaving (main.c's melee_wii_before_exit): the read in progress finished,
 * no other started, so that the card can be unmounted under no read */
void melee_wii_dvd_stop(void)
{
    s_stopping = TRUE;
    for (int i = 0; i < 100 && s_current != NULL; i++) {
        usleep(10000);
    }
}

/* stall.c: where the reads are (a game that waits on one forever) */
/* Whether the game's disc reads are under way (or one ended in the last
 * 200 ms: a load comes as many reads): the replay writer keeps off the SD
 * card meanwhile (wii/src/slippi/so_replay.c) */
int melee_wii_dvd_busy(void)
{
    return s_reads_queued != s_reads_done || (s_last_done && OSGetTime() - s_last_done < OSMillisecondsToTicks(200));
}

void melee_wii_dvd_report(void)
{
    BOOL enabled = OSDisableInterrupts();
    DVDCommandBlock* head = s_head;
    DVDCommandBlock* current = s_current;
    DVDCommandBlock* done = s_done_head;
    u32 queued = s_reads_queued, finished = s_reads_done, waiting = 0;
    OSTime now = OSGetTime(), start = s_current_start, last = s_last_done;
    int kick = s_kick.armed, complete_armed = s_complete.armed;
    for (DVDCommandBlock* b = head; b != NULL && waiting < 64; b = b->next) {
        waiting++;
    }
    OSRestoreInterrupts(enabled);
    OSReport("stall: DVD: %u reads queued, %u done (the last %u ms ago); %u waiting (first %p), done but not "
             "delivered %p; kick %s, delivery %s\n",
             (unsigned) queued, (unsigned) finished, (unsigned) OSTicksToMilliseconds(now - last),
             (unsigned) waiting, head, done, kick ? "pending" : "idle", complete_armed ? "pending" : "idle");
    if (current != NULL) {
        /* its thread's registers as its last switch away saved them
         * (tuxedo's PPCContext: pc, msr, cr, lr, ctr, xer, gpr[]) */
        const volatile u32* ctx = (const volatile u32*) ~(u32) s_thread;
        char calls[120];
        int len = 0, n = 0;
        calls[0] = 0;
        for (u32 p = ctx[6 + 1]; n < 8 && (p & 7) == 0 && p >= 0x80000000u && p < 0x94000000u; n++) {
            u32 next = ((const volatile u32*) p)[0];
            len += snprintf(calls + len, sizeof(calls) - len, " %08x", (unsigned) ((const volatile u32*) p)[1]);
            if (next <= p) {
                break;
            }
            p = next;
        }
        OSReport("stall: DVD: reading 0x%x bytes at 0x%x into %p for %u ms; its thread at %08x, lr %08x, calls%s\n",
                 (unsigned) current->length, (unsigned) current->offset, current->addr,
                 (unsigned) OSTicksToMilliseconds(now - start), (unsigned) ctx[0], (unsigned) ctx[3], calls);
    }
}

long DVDGetDriveStatus(void)
{
    BOOL enabled = OSDisableInterrupts();
    long status =
        (s_head != NULL || s_current != NULL || s_done_head != NULL)
            ? DVD_STATE_BUSY
            : DVD_STATE_END;
    OSRestoreInterrupts(enabled);
    return status;
}

long DVDGetCommandBlockStatus(DVDCommandBlock* block)
{
    return block->state;
}

BOOL DVDCheckDisk(void)
{
    return TRUE;
}

DVDDiskID* DVDGetCurrentDiskID(void)
{
    return &s_disk_id;
}

/* wii/src/slippi: bytes of the disc image (the online mode keeps some of its
 * files in memory) */
int melee_wii_disc_read(u32 offset, void* buf, u32 len)
{
    if (s_file == NULL || (u64) offset + len > s_gcm.size) {
        return -1;
    }
    return wl_platform_fs_read_at(s_file, offset, buf, len) == WL_OK ? 0 : -1;
}

/* The launcher (before the game's DVDInit): the image, if there is one,
 * whether it is the game, and its files. 0: the game (id: 6 characters,
 * revision); -1: no image; -2: an image of another game */
int melee_wii_disc_open(char* path, int path_size, char* id, int* revision)
{
    if (s_file == NULL && !open_image()) {
        return -1;
    }
    if (wl_gcm_read(&s_gcm, 0, &s_disk_id, sizeof(s_disk_id)) != WL_OK) {
        return -1;
    }
    snprintf(path, path_size, "%s", s_image_path);
    memcpy(id, s_disk_id.gameName, 4);
    memcpy(id + 4, s_disk_id.company, 2);
    id[6] = '\0';
    *revision = s_disk_id.gameVersion;
    if (s_fst == NULL) {
        s_fst = s_gcm.fst;
        s_entries = s_gcm.entry_count;
        s_names = (const char*) s_fst + s_entries * 12;
    }
    return memcmp(id, "GALE01", 6) == 0 ? 0 : -2;
}

/* A file of the disc's root by name: its offset and size; 0, or -1 */
int melee_wii_disc_file(const char* name, u32* offset, u32* size)
{
    char path[64];
    s32 entry;
    snprintf(path, sizeof(path), "/%s", name);
    entry = s_fst != NULL ? find_path(s_fst, s_entries, 0, path) : -1;
    if (entry <= 0 || is_dir(entry)) {
        return -1;
    }
    *offset = be32(&s_fst[entry * 12 + 4]);
    *size = be32(&s_fst[entry * 12 + 8]);
    return 0;
}

unsigned long long melee_wii_disc_size(void)
{
    return s_file != NULL ? s_gcm.size : 0;
}

/* The game's own file at this path (not what the substitute would give), or
 * -1 */
int melee_wii_disc_entry(const char* path)
{
    s32 e = s_fst != NULL ? find_path(s_fst, s_entries, 0, path) : -1;
    return e > 0 && !is_dir(e) ? (int) e : -1;
}

/* ---- the second image (mod_songs.c) -------------------------------------- */

/* Opens it; 0, or -1 */
int melee_wii_disc2_open(const char* path)
{
    WlPlatformFile* file;
    if (s_file2 != NULL) {
        return 0;
    }
    if (!wl_platform_fs_exists(path) || wl_platform_fs_open(path, WL_FILE_READ, &file) != WL_OK) {
        return -1;
    }
    if (wl_gcm_open_src(&s_gcm2, read_image, NULL, file, wl_platform_fs_size(file)) != WL_OK ||
        s_gcm2.size >= SECOND_IMAGE)
    {
        wl_platform_fs_close(file);
        return -1;
    }
    s_file2 = file;
    return 0;
}

void melee_wii_disc2_close(void)
{
    if (s_file2 != NULL) {
        s_substitute = NULL;
        s_after_read = NULL;
        s_overlay_on = NULL;
        wl_platform_fs_close(s_file2);
        wl_gcm_close(&s_gcm2);
        s_file2 = NULL;
    }
}

/* Its executable's offset, and a read of the image; 0, or -1 */
unsigned int melee_wii_disc2_dol(void)
{
    return s_file2 != NULL ? s_gcm2.dol_off : 0;
}

int melee_wii_disc2_read(u32 offset, void* buf, u32 len)
{
    if (s_file2 == NULL || (u64) offset + len > s_gcm2.size) {
        return -1;
    }
    return wl_platform_fs_read_at(s_file2, offset, buf, len) == WL_OK ? 0 : -1;
}

/* The entry number the game can open one of its files by (a path from its
 * root), or -1 */
int melee_wii_disc2_entry(const char* path)
{
    s32 e;
    if (s_file2 == NULL || s_entries == 0) {
        return -1;
    }
    e = find_path(s_gcm2.fst, s_gcm2.entry_count, 0, path);
    return e > 0 && s_gcm2.fst[e * 12] == 0 ? (int) (s_entries + (u32) e) : -1;
}

/* One of its files by path: its offset and size; 0, or -1 */
int melee_wii_disc2_file(const char* path, u32* offset, u32* size)
{
    s32 e = s_file2 != NULL ? find_path(s_gcm2.fst, s_gcm2.entry_count, 0, path) : -1;
    if (e <= 0 || s_gcm2.fst[e * 12] != 0) {
        return -1;
    }
    *offset = be32(&s_gcm2.fst[e * 12 + 4]);
    *size = be32(&s_gcm2.fst[e * 12 + 8]);
    return 0;
}

/* Calls `fn` with each of its files: its name (without its folders), offset
 * and size */
void melee_wii_disc2_each(void (*fn)(const char* name, u32 offset, u32 size))
{
    const char* names;
    if (s_file2 == NULL) {
        return;
    }
    names = (const char*) s_gcm2.fst + s_gcm2.entry_count * 12;
    for (u32 e = 1; e < s_gcm2.entry_count; e++) {
        const u8* entry = &s_gcm2.fst[e * 12];
        if (entry[0] == 0) {
            fn(names + (be32(entry) & 0xFFFFFF), be32(entry + 4), be32(entry + 8));
        }
    }
}

/* The game's image's bytes from `from` to `to` come from the second image's
 * at `other` on, while *on */
void melee_wii_disc_overlay(u32 from, u32 to, u32 other, const u32* on)
{
    s_overlay_on = NULL;
    s_overlay_from = from;
    s_overlay_to = to;
    s_overlay_other = other;
    s_overlay_on = on;
}

/* Called with what a read brought (of the game's image, or of the second),
 * before the game has it (the IO thread) */
void melee_wii_disc_after_read(void (*fn)(int second, u32 offset, void* buf, u32 len))
{
    s_after_read = fn;
}

/* Asked for each path the game looks up: the entry to give it instead (one
 * of melee_wii_disc2_entry's), or -1 for the game's own file */
void melee_wii_disc_substitute(s32 (*fn)(const char* path))
{
    s_substitute = fn;
}
