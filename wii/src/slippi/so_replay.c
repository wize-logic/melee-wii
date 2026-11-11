/* Replays (.slp): what Slippi's recording codes send the device each game,
 * kept on the SD card as Slippi Dolphin and Slippi Nintendont keep them:
 * <Slippi's folder>/replays/Game_YYYYMMDDTHHMMSS.slp.
 *
 * The codes write the game's events over EXI (wii/src/exi.c hands them here):
 * 0x35, the payload sizes of every event, then 0x36 (the game's start), each
 * frame's events, and 0x39 (its end). A replay is those bytes as they came,
 * in Slippi's UBJSON container ({"raw": [bytes], "metadata": {...}}).
 *
 * A whole game doesn't fit in memory beside the savestates (8 MB of MEM2
 * for a long game; a game holding it lost its rollbacks), so the file is
 * written as the game runs, as Slippi Nintendont writes it: the game's EXI
 * path only appends to a ring, and a thread empties the ring into the file
 * every 100 ms. At the game's end the thread adds the metadata and writes
 * the length of "raw" (until then 0, which Slippi reads as a game still
 * being written). */

#include <malloc.h>
#include <time.h>

#include "so_gamelog.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"

#define CMD_PAYLOADS 0x35
#define CMD_GAME_END 0x39
#define RING_SIZE (1024 * 1024) /* (a power of 2) about 40 s of a game: the thread's slack */
#define STAGE_SIZE (32 * 1024)
#define WRITER_PERIOD_US 100000

enum { IDLE, RECORDING, ENDED };

static u8* s_ring;
static u8* s_stage;                /* (aligned, for the SD driver) */
static volatile int s_state;       /* EXI path: IDLE -> RECORDING -> ENDED; the thread: ENDED -> IDLE */
static volatile u32 s_head;        /* this game's bytes so far (the EXI path's) */
static volatile u32 s_tail;        /* of them, those written (the thread's) */
static volatile int s_broken;      /* the ring overflowed: the replay stops there */
static u16 s_sizes[256];           /* each event's payload size, from 0x35 */
static u32 s_carry;                /* of the last event, the bytes still to come in later writes */
static u32 s_writes;               /* this game's EXI writes (the log's) */
static int s_started;
static volatile int s_enabled = 1;
static int s_slow_logged;          /* (the writer's) */
int melee_wii_dvd_busy(void) __attribute__((weak)); /* wii/src/dvd.c */

/* The memory is taken here, as Slippi starts (not with interrupts off, at a
 * game's start) */
void so_replay_enable(int on)
{
    s_enabled = on;
    if (on && s_ring == NULL) {
        s_ring = memalign(32, RING_SIZE);
        s_stage = memalign(32, STAGE_SIZE);
        if (s_ring == NULL || s_stage == NULL) {
            so_log("replay: no memory for the buffers (%u KB): no replays",
                   (unsigned) ((RING_SIZE + STAGE_SIZE) / 1024));
            free(s_ring);
            free(s_stage);
            s_ring = NULL;
            s_stage = NULL;
        }
    }
}

static const u8 s_head_bytes[] = { '{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l' };
#define RAW_AT (sizeof(s_head_bytes) + 4)

static void* open_replay(char* name, u32 name_size, struct tm* t)
{
    void* file;
    so_snprintf(name, name_size, "/slippi/replays/Game_%04d%02d%02dT%02d%02d%02d.slp",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    file = so_file_create(name);
    if (file == NULL) {
        so_log("replay: could not create %s", name);
        return NULL;
    }
    memcpy(s_stage, s_head_bytes, sizeof(s_head_bytes));
    so_put_u32(s_stage + sizeof(s_head_bytes), 0); /* (a game still being written) */
    if (so_file_write(file, s_stage, RAW_AT) < 0) {
        so_log("replay: could not write %s", name);
        so_file_close(file);
        return NULL;
    }
    return file;
}

/* "metadata": {"startAt": "...", "playedOn": "wii"}, the closing brace, and
 * the length of "raw" */
static int close_replay(void* file, u32 len, struct tm* t)
{
    char meta[160];
    int n, ok;
    n = so_snprintf(meta + 10, sizeof(meta) - 10,
                    "%c%cstartAtSU%c%04d-%02d-%02dT%02d:%02d:%02dZU%cplayedOnSU%cwii}}", 'U', 7, 20,
                    t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, 8,
                    3);
    memcpy(meta, "U\x08metadata{", 10);
    memcpy(s_stage, meta, 10 + (u32) n);
    ok = so_file_write(file, s_stage, 10 + (u32) n) == 0;
    so_put_u32(s_stage, len);
    ok = ok && so_file_write_at(file, sizeof(s_head_bytes), s_stage, 4) == 0;
    so_file_close(file);
    return ok;
}

static u32 WriterThread(void* unused)
{
    void* file = NULL;
    int failed = 0;
    char name[96];
    struct tm start_time;
    (void) unused;
    for (;;) {
        int state;
        u32 head;
        so_sleep_us(WRITER_PERIOD_US);
        state = s_state; /* (before the head: the EXI path moves the head, then ends) */
        if (state == IDLE) {
            continue;
        }
        head = s_head;
        /* Off the SD card while the game reads its disc (a load, the music
         * stream: the image is on the card too), unless the ring is half
         * full: a freeze as a match loaded, its file just made */
        if (melee_wii_dvd_busy != NULL && melee_wii_dvd_busy() && state != ENDED &&
            head - s_tail < RING_SIZE / 2) {
            continue;
        }
        if (file == NULL && !failed) {
            time_t now = time(NULL);
            start_time = *gmtime(&now);
            s_slow_logged = 0;
            file = open_replay(name, sizeof(name), &start_time);
            failed = file == NULL;
        }
        while (s_tail != head) {
            u32 at = s_tail & (RING_SIZE - 1);
            u32 n = head - s_tail;
            if (n > STAGE_SIZE) {
                n = STAGE_SIZE;
            }
            if (n > RING_SIZE - at) {
                n = RING_SIZE - at;
            }
            if (!failed) {
                u64 t0;
                int bad;
                memcpy(s_stage, s_ring + at, n);
                t0 = so_time_us();
                bad = so_file_write(file, s_stage, n) < 0;
                {
                    /* the SD card and the network share IOS: a slow write can hold the game's packets */
                    u32 took = (u32) (so_time_us() - t0);
                    so_gamelog_event('R', took);
                    if (took > 50000 && s_slow_logged++ < 8) {
                        so_log("replay: writing %u bytes at %u took %u ms", (unsigned) n, (unsigned) s_tail,
                               (unsigned) (took / 1000));
                    }
                }
                if (bad) {
                    so_log("replay: could not write %s at %u", name, (unsigned) s_tail);
                    so_file_close(file);
                    file = NULL;
                    failed = 1;
                }
            }
            s_tail += n;
        }
        if (state == ENDED) {
            if (file != NULL) {
                if (close_replay(file, head, &start_time)) {
                    so_log("replay: %s (%u bytes%s)", name, (unsigned) (RAW_AT + head),
                           s_broken ? "; the game's rest was lost: the thread fell behind" : "");
                } else {
                    so_log("replay: could not finish %s", name);
                }
            }
            file = NULL;
            failed = 0;
            s_state = IDLE;
        }
    }
    return 0;
}

static void start(void)
{
    if (s_started) {
        return;
    }
    s_started = 1;
    so_thread_start(WriterThread, NULL, NULL, 0, 0);
}

static void stop_recording(void)
{
    s_state = ENDED;
    s_carry = 0;
}

/* The events of one EXI write (wii/src/exi.c): from the game's EXI path, with
 * interrupts off; never waits, takes no lock */
void so_replay_data(const u8* data, u32 len)
{
    u32 at, ring_at, first;
    if (!s_enabled || len == 0 || s_ring == NULL) {
        return;
    }
    if (data[0] == CMD_PAYLOADS && (s_state != RECORDING || s_carry == 0)) {
        /* a new game: the sizes, then the events */
        u32 table = data[1];
        if (s_state == RECORDING) {
            so_log("replay: a game's end never came (%u bytes in %u writes)", (unsigned) s_head,
                   (unsigned) s_writes);
            stop_recording(); /* (that game's file is finished as it is) */
        }
        if (s_state != IDLE) {
            so_log("replay: the last game's file isn't finished yet: no replay of this one");
            return;
        }
        memset(s_sizes, 0, sizeof(s_sizes));
        for (u32 i = 0; i + 3 <= table - 1 && 2 + i + 2 < len; i += 3) {
            s_sizes[data[2 + i]] = (u16) ((data[3 + i] << 8) | data[4 + i]);
        }
        s_sizes[CMD_PAYLOADS] = (u16) table;
        start();
        s_head = 0;
        s_tail = 0;
        s_broken = 0;
        s_carry = 0;
        s_writes = 0;
        s_state = RECORDING;
    }
    if (s_state != RECORDING) {
        return;
    }
    if (s_head + len - s_tail > RING_SIZE) {
        so_log("replay: the writer fell %u bytes behind: the replay stops at %u bytes",
               (unsigned) (s_head - s_tail), (unsigned) s_head);
        s_broken = 1;
        stop_recording();
        return;
    }
    ring_at = s_head & (RING_SIZE - 1);
    first = len < RING_SIZE - ring_at ? len : RING_SIZE - ring_at;
    memcpy(s_ring + ring_at, data, first);
    memcpy(s_ring, data + first, len - first);
    s_writes++;
    /* The events, from where the last write's ended (an event may straddle
     * writes): at the game's end the replay is done */
    at = s_carry;
    while (at < len) {
        u8 cmd = data[at];
        if (cmd == CMD_GAME_END) {
            s_head += len; /* (the game end's payload is in this write) */
            stop_recording();
            return;
        }
        if (s_sizes[cmd] == 0 && cmd != CMD_PAYLOADS) {
            so_log("replay: event %02x (write %u, +%u) has no size: the replay stops there", cmd,
                   (unsigned) s_writes, (unsigned) at);
            stop_recording();
            return;
        }
        at += 1 + s_sizes[cmd];
    }
    s_head += len;
    s_carry = at - len;
}
