/* Rollback for Slippi Online on the Wii: what Slippi Dolphin's device does
 * for its online codeset's rollback (EXI_DeviceSlippi.cpp, SlippiSavestate.cpp
 * in Slippi's Ishiiruka 3.6.4), on the console itself.
 *
 * Slippi's online code (netplay.gct) does the game's side: when an opponent's
 * input for a frame hasn't come, it plays the frame with a predicted one, and
 * asks the device to keep the game's state (0xB1, the frame about to run);
 * when the real input differs, it asks for that state back (0xB2) and plays
 * the frames again in one displayed frame. The device side is here:
 *
 *   - 0xB0's answer: go on (with what has come) unless an opponent is more
 *     than ROLLBACK_MAX_FRAMES behind, and keep in time with the opponent
 *     (at the start, skip frames while ahead; later, a frame now and then);
 *   - the savestates: the game's memory that the frames change, copied into
 *     slots in MEM2, the oldest reused.
 *
 * Which memory: the game's data and bss (less the sound, DSP and video
 * driver state, as Slippi leaves them: Slippi's own list), and the heaps the
 * match lives in, found from the game's own records at the game's start (the
 * port's arena is not where the GameCube's is: its addresses differ from
 * Slippi's list). The preserve blocks the game names with 0xB2 (its online
 * buffers, in the heap) keep their present contents. */

#include <malloc.h>
#include <stdio.h>
#include <unistd.h>

#include <gccore.h>

#include "so_client.h"
#include "so_gamelog.h"
#include "so_netplay.h"
#include "so_platform.h"
#include "so_rollback.h"
#include "so_util.h"
#include "string.h"

#define ROLLBACK_MAX_FRAMES 7
#define CAPTURES (ROLLBACK_MAX_FRAMES + 1)
#define MAX_SEGS 20000
#define MAX_RANGES 64
#define FRAME_US 16683
#define SYNC_INTERVAL 30

typedef struct {
    u32 addr, len;
} Range;

/* A piece of the state: within one 512-byte chunk of a page and one range */
typedef struct {
    u32 ea;
    u16 len;
    u8 tracked; /* the page's writes are recorded (wii/src/mmu_track.c) */
} Seg;

/* The savestates' memory: 512-byte buffers, each the copy of one piece (at
 * the piece's place in its chunk). Each piece has its current copy (the
 * state as of the last capture: the mirror) and that copy's hash; a capture
 * gives each piece that differs from it a new buffer for its copy, and keeps
 * the one it had (and its hash) as the capture's undo record: one copy a
 * changed piece, none of the mirror.
 *
 * What differs: the MMU says which pages were written (4 KiB), and of their
 * pieces, those whose hash is not the copy's (the copies, in MEM2, are slow
 * to read back). The game writes much of a page with what was there: of
 * some 920 KB of pages written between captures, 270 KB differed in 1 KB
 * chunks and 160 KB in 256-byte ones (Dolphin, Final Destination), so the
 * savestates take a third to a quarter of the room whole pages did. */
#define BUF_SIZE 512u
#define MAX_RECS 16384

typedef struct {
    u16 seg, buf; /* the piece, and its copy before the capture */
    u32 hash_hi, hash_lo; /* that copy's hash */
} Rec;

/* A savestate: the frame, and its undo records ([first, end) of the ring) */
typedef struct {
    s32 frame;
    u32 first, end;
} Capture;

/* This game's captures, summed (their averages logged at its end): the
 * written pages found, the bytes hashed in them, and each step's time */
static u32 s_written_pages;
static u64 s_hashed_bytes;
static struct {
    u64 pages_us, hash_us, copy_us, clear_us, total_us;
} s_sum, s_lsum; /* (the loads') */
static u64 s_load_hashed, s_load_extra;

static int s_enabled;
static Range s_ranges[MAX_RANGES];
static int s_range_count;
static u32 s_state_size;
static int s_ready; /* savestates set up for this game */
static int s_window; /* frames the game may run ahead */

static Seg* s_segs;
static int s_seg_count;
static u8* s_changed_page; /* scratch: per piece, changed (or to restore) */
static u8* s_area[2];      /* the buffers: two blocks of MEM2 */
static u32 s_area_bufs[2];
static u32 s_buf_count;
static u16* s_cur;  /* per piece: the buffer with its current copy */
static u64* s_hash; /* per piece: its current copy's hash */
static u64* s_live; /* scratch: per piece, the hash of the memory now */
static int* s_marked; /* scratch: the pieces to hash (mark_written, hash_list) */
/* The pieces by page: runs of pieces in one tracked page (or one piece not
 * tracked), in the pieces' order. A capture goes by these (some 1900) to
 * find the written pages' pieces (some 1800 of 15000), and from then on by
 * that list only: four passes over every piece cost it about a millisecond. */
typedef struct {
    u32 page;  /* the page's address */
    u32 first; /* its first piece */
    u16 count;
    u16 tracked;
} Run;
static Run* s_runs;
static int* s_extra; /* scratch: a load's pieces put back without hashing */
static int s_run_count;
static u16* s_free; /* the buffers not in use */
static u32 s_free_count;
static Rec* s_recs; /* the undo records: a ring */
static u32 s_rec_head, s_rec_tail; /* records ever written, ever dropped */
static Capture s_caps[CAPTURES];
static int s_cap_count;
/* The last loads (for the log when the opponent's checksum differs) */
#define LOAD_HISTORY 6
static struct {
    s32 target, newest; /* the frame gone back to, the newest capture then */
} s_load_hist[LOAD_HISTORY];
static u32 s_load_hist_count;

static int s_skip_left, s_skipping;
static int s_waits_in_time; /* the last "wait" of so_rollback_ready was the time sync's */
static struct {
    u32 captures, loads, missing, skips, stalls, dropped, checks, check_bad;
    u64 capture_bytes;
    u32 irq_on; /* captures and loads called with interrupts on */
    u32 scan_us, copy_us, clear_us; /* the longest capture's parts */
    u32 max_capture_us, max_load_us, max_capture_kb, max_load_kb;
} s_stats;

/* Big savestates and loads, logged as they come (a game that stops leaves
 * no end-of-game line): from 2 MB or 15 ms, each one bigger or longer than
 * the last logged, up to 32 a game */
#define BIG_KB 2048u
#define BIG_US 15000u
static u32 s_big_kb, s_big_us, s_big_count;

static void note_big(const char* what, s32 frame, u32 kb, u32 us)
{
    if ((kb < BIG_KB && us < BIG_US) || (kb <= s_big_kb && us <= s_big_us) || s_big_count >= 32) {
        return;
    }
    s_big_count++;
    s_big_kb = kb > s_big_kb ? kb : s_big_kb;
    s_big_us = us > s_big_us ? us : s_big_us;
    so_log("rollback: frame %d: a %s of %u KB took %u us", (int) frame, what, (unsigned) kb, (unsigned) us);
}

/* Every 10 s of a game (so_exi.c), for a game that stops */
void so_rollback_heartbeat(s32 frame)
{
    if (!s_ready) {
        return;
    }
    so_log("rollback: frame %d: %u savestates (longest %u us, most %u KB), %u loads (longest %u us, most %u KB) so "
           "far",
           (int) frame, (unsigned) s_stats.captures, (unsigned) s_stats.max_capture_us,
           (unsigned) s_stats.max_capture_kb, (unsigned) s_stats.loads, (unsigned) s_stats.max_load_us,
           (unsigned) s_stats.max_load_kb);
}

/* Slippi's: the game's data sections 0-1, and 2-7 with the bss */
static const Range BACKUP_FIXED[] = {
    { 0x80005520, 0x80005940 - 0x80005520 },
    { 0x803B7240, 0x804DEC00 - 0x803B7240 },
};

/* Left out of those: Slippi's (sound, AX/DSP, synth, the video driver), and
 * the port's (marked W): the game's disc and ARAM requests (HSD's devcom:
 * its requests, buffers and file block) and the music stream's counters,
 * which run on the audio's time, not the game's frames. Rolled back with the
 * frames, a request in flight across a savestate was finished twice or never
 * (its busy flag, 0x804D77F5, back to 1 after its callback): no more reads,
 * the music looping its last second, the next screen's files never loading.
 * (On a PC, Slippi plays the music itself, the game reading nothing in a
 * match.) Sorted. */
static const Range EXCLUDE[] = {
    { 0x804031A0, 0x24 },   { 0x80407FB4, 0x34C },  { 0x80433C64, 0x1EE80 }, { 0x804A8D78, 0x17A68 },
    { 0x804C0980, 0x15F8 }, { 0x804C28E0, 0x399C },
    { 0x804C62A0, 0x80DC }, /* W: devComARQR, devComStatus, the requests' lists, buffers, fileinfo */
    { 0x804D6030, 0x4 },    { 0x804D603C, 0x4 },
    { 0x804D7218, 0x4 },    { 0x804D7228, 0x8 },    { 0x804D7474, 0x8 },     { 0x804D74F0, 0x50 },
    { 0x804D7548, 0x4 },    { 0x804D7558, 0x24 },   { 0x804D7580, 0xC },     { 0x804D759C, 0x4 },
    { 0x804D7720, 0x4 },
    { 0x804D773C, 0x4 },    /* W: the sound banks' moves in ARAM under way (sfxGroupDataReaddressCounter) */
    { 0x804D7740, 0x4 },    { 0x804D7744, 0x4 },     { 0x804D774C, 0x8 },
    { 0x804D7754, 0x4 },    { 0x804D7758, 0x8 },
    { 0x804D7760, 0x28 },   /* W: the music stream's (HSD_Synth_804D7760-7784) */
    { 0x804D7788, 0x10 },   { 0x804D77BC, 0x4 },
    { 0x804D77C8, 0x4 },    { 0x804D77D0, 0x4 },    { 0x804D77E0, 0x4 },
    { 0x804D77F0, 0x20 },   /* W: devcom's free list, flags, current requests */
    { 0x804DE358, 0x80 },
    { 0x804DE7F0, 0x10 },   { 0x804DE800, 0x70 },
};

/* Devcom's requests (36 bytes each, in the audio heap, which is kept): kept
 * across a load as they are, found from the lists' heads (left out above) */
#define DEVCOM_SIZE 0x24
#define DEVCOM_MAX 64

static int devcom_requests(u32* out)
{
    static const u32 heads[] = {
        0x804C6320, 0x804C6324, 0x804C6328, 0x804C632C, /* devComStatus */
        0x804C6330, 0x804C6334, 0x804C6338, 0x804C633C, /* HSD_DevCom_804C6330 */
        0x804D77F0, 0x804D77F8, 0x804D77FC, 0x804D7800, 0x804D7808, /* free list, dvdDC, 804D77FC, aramDC */
    };
    int n = 0;
    for (u32 h = 0; h < sizeof(heads) / sizeof(heads[0]); h++) {
        u32 dc = *(volatile u32*) heads[h];
        while (n < DEVCOM_MAX && dc >= 0x80000000u && dc < 0x81800000u - DEVCOM_SIZE && (dc & 3) == 0) {
            int seen = 0;
            for (int i = 0; i < n; i++) {
                seen |= out[i] == dc;
            }
            if (seen) {
                break;
            }
            out[n++] = dc;
            dc = *(volatile u32*) dc; /* next */
        }
    }
    return n;
}

/* The game's heap records (lb/lbheap.c, baselib/initialize.c) */
#define HSD_HEAP_LO 0x804D76B8u /* hsd_heap_next_arena_lo */
#define HSD_HEAP_HI 0x804D76BCu /* hsd_heap_next_arena_hi */

/* MEM2 as the savestates find it: the heap's free bytes, what is not yet
 * taken from the arena, and the largest block one allocation could get */
static u32 largest_block(void)
{
    u32 lo = 0, hi = 64u * 1024 * 1024;
    while (hi - lo > 64 * 1024) {
        u32 mid = lo + (hi - lo) / 2;
        void *p = memalign(32, mid);
        if (p != NULL) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static void log_memory(const char *when)
{
    struct mallinfo mi = mallinfo();
    u32 top = (u32) sbrk(0), end = (u32) SYS_GetArena2Hi();
    u32 lo = largest_block();
    so_log("rollback: MEM2 %s: %u KB free in the heap, %u KB not taken yet (%08x-%08x), largest block %u KB", when,
           (unsigned) (mi.fordblks / 1024), (unsigned) ((end > top ? end - top : 0) / 1024), (unsigned) top,
           (unsigned) end, (unsigned) (lo / 1024));
}

/* The savestates' memory (AREA_0 + AREA_1 of MEM2, two blocks to find room
 * for), taken once as Slippi Online starts with rollback chosen, before the
 * game's ARAM and the files kept in RAM split the heap, and kept: at a
 * game's start, as the Wii's MEM2 is by then, no 13 MB block may be left */
#define AREA_0 (8u * 1024 * 1024)
#define AREA_1 (10u * 1024 * 1024)

/* wii/src/copy.c, wii/src/ax_wii.c */
void melee_wii_copy_check(void* dst, const void* src, unsigned int len);
int melee_wii_copy_by_dma(void);
void melee_wii_copy_begin(void);
void melee_wii_copy(void* dst, const void* src, unsigned int len);
int melee_wii_copy_end(void);
void melee_wii_audio_hold(void);
void melee_wii_audio_release(void);
void melee_wii_audio_service(void);
void melee_wii_audio_guard(void);
void melee_wii_audio_counts(unsigned int* serviced, unsigned int* guarded, unsigned int* silent);

static void hash_check(void);

static void reserve(void)
{
    if (s_area[0] == NULL) {
        s_area[0] = memalign(32, AREA_0);
    }
    if (s_area[1] == NULL) {
        s_area[1] = memalign(32, AREA_1);
    }
    if (s_changed_page == NULL) {
        s_segs = malloc(MAX_SEGS * sizeof(Seg));
        s_hash = malloc(MAX_SEGS * sizeof(u64));
        s_live = malloc(MAX_SEGS * sizeof(u64));
        s_marked = malloc(MAX_SEGS * sizeof(int));
        s_runs = malloc(MAX_SEGS * sizeof(Run));
        s_extra = malloc(MAX_SEGS * sizeof(int));
        s_changed_page = malloc(MAX_SEGS);
        s_cur = malloc(MAX_SEGS * sizeof(u16));
        s_recs = malloc(MAX_RECS * sizeof(Rec));
        s_free = malloc((AREA_0 + AREA_1) / BUF_SIZE * sizeof(u16));
    }
    if (s_area[0] == NULL || s_area[1] == NULL || s_changed_page == NULL || s_cur == NULL || s_recs == NULL ||
        s_free == NULL || s_segs == NULL || s_hash == NULL || s_live == NULL || s_marked == NULL || s_runs == NULL || s_extra == NULL) {
        log_memory("(no room for the savestates)");
        return;
    }
    if (s_buf_count == 0) {
        s_area_bufs[0] = AREA_0 / BUF_SIZE;
        s_area_bufs[1] = AREA_1 / BUF_SIZE;
        s_buf_count = s_area_bufs[0] + s_area_bufs[1];
        /* the copies: by the locked cache's DMA if right and faster here
         * (the game's data into the buffers) */
        melee_wii_audio_hold();
        melee_wii_copy_check(s_area[1], (const void*) 0x803B7240, 512 * 1024);
        hash_check();
        melee_wii_audio_release();
        log_memory("after the savestates' memory");
    }
}

static u8* buf_at(u32 b)
{
    return b < s_area_bufs[0] ? s_area[0] + b * BUF_SIZE : s_area[1] + (b - s_area_bufs[0]) * BUF_SIZE;
}

/* A piece's copy in buffer `b` */
static u8* copy_of(const Seg* g, u32 b)
{
    return buf_at(b) + (g->ea & (BUF_SIZE - 1));
}

/* A piece's memory's hash, 64 bits: two chains (the even words' and the odd
 * ones') of rotate(h ^ word) * an odd constant, mixed into each other after
 * every line of 32 bytes and at the end. Every step is one-to-one on the
 * pair, so a change of one word always shows. The rotation is what the
 * first hash here lacked: a product by an odd constant keeps a change of
 * the top bit in the top bit, and so did its two sums for a pair of them
 * an even number of words apart, so two floats changing sign at once (a
 * turn: the facing direction, a speed) left that hash as it was half the
 * time; the savestate then kept the old values, and a rollback brought
 * them back (a desync, with nothing else wrong). By lines of 32 bytes, each
 * asked for ahead. The constants fit in mulli's 16 bits: the 750 multiplies
 * by them in 2 or 3 cycles. */
#define HASH_K0 0x6B43u
#define HASH_K1 0x7F4Bu
#define HASH_ROTL(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
#define HASH_A(w) (a = HASH_ROTL(a ^ (w), 15) * HASH_K0)
#define HASH_B(w) (b = HASH_ROTL(b ^ (w), 13) * HASH_K1)
#define HASH_MIX() (a += HASH_ROTL(b, 7), b ^= a)

static u64 hash_at(const u8* at, u32 len, int ahead)
{
    const u32* p = (const u32*) at;
    u32 n = len >> 2, i = 0;
    u32 a = 0x811C9DC5u, b = 0x01000193u ^ len;
    if ((((u32) at | len) & 31) == 0) {
        for (; i < n; i += 8) {
            const u32* q = p + i;
            if (ahead) {
                __asm__ volatile("dcbt 0, %0" ::"r"(q + 16));
            }
            HASH_A(q[0]);
            HASH_B(q[1]);
            HASH_A(q[2]);
            HASH_B(q[3]);
            HASH_A(q[4]);
            HASH_B(q[5]);
            HASH_A(q[6]);
            HASH_B(q[7]);
            HASH_MIX();
        }
    } else {
        /* (the same for whole lines) */
        for (; i < n; i++) {
            if (i & 1) {
                HASH_B(p[i]);
            } else {
                HASH_A(p[i]);
            }
            if ((i & 7) == 7) {
                HASH_MIX();
            }
        }
        for (i = n << 2; i < len; i++) {
            HASH_A((u32) at[i]);
        }
    }
    HASH_MIX();
    a ^= a >> 15;
    b ^= b >> 13;
    return ((u64) a << 32) | b;
}

static u64 piece_hash(const Seg* g)
{
    return hash_at((const u8*) g->ea, g->len, 1);
}

/* wii/src/copy.c: the locked cache as a window for reading memory */
int melee_wii_window_begin(void);
const void* melee_wii_window_fetch(const void* src, u32 len);
void melee_wii_window_wait(u32 index);

/* s_live[i]: the hash of each of the `count` pieces listed in s_marked (in
 * order), those of whole lines read through the locked cache (WINDOW_AHEAD
 * pieces fetched ahead of the one hashed), the rest by the CPU; all by the
 * CPU if the DMA got stuck */
#define WINDOW_AHEAD 6

static int s_hash_by_window; /* hash_check: faster on this console */

static void hash_list(int count)
{
    if (count == 0) {
        return;
    }
    if (s_hash_by_window && melee_wii_window_begin()) {
        struct {
            int k;          /* which marked piece */
            const void* at; /* its slot */
            u32 fetch;      /* its fetch's number */
        } q[WINDOW_AHEAD + 1];
        u32 head = 0, tail = 0, issued = 0;
        int next = 0; /* the next marked piece to fetch */
        for (int k = 0; k < count; k++) {
            const Seg* g = &s_segs[s_marked[k]];
            while ((tail - head < WINDOW_AHEAD || next <= k) && next < count) {
                const Seg* h = &s_segs[s_marked[next]];
                if (((h->ea | h->len) & 31) == 0) {
                    q[tail % (WINDOW_AHEAD + 1)].k = next;
                    q[tail % (WINDOW_AHEAD + 1)].at = melee_wii_window_fetch((const void*) h->ea, h->len);
                    q[tail % (WINDOW_AHEAD + 1)].fetch = issued++;
                    tail++;
                }
                next++;
            }
            if (head != tail && q[head % (WINDOW_AHEAD + 1)].k == k) {
                melee_wii_window_wait(q[head % (WINDOW_AHEAD + 1)].fetch);
                s_live[s_marked[k]] = hash_at((const u8*) q[head % (WINDOW_AHEAD + 1)].at, g->len, 0);
                head++;
            } else {
                s_live[s_marked[k]] = piece_hash(g); /* (not whole lines) */
            }
        }
        if (melee_wii_copy_end() == 0) {
            return;
        }
    }
    for (int k = 0; k < count; k++) {
        s_live[s_marked[k]] = piece_hash(&s_segs[s_marked[k]]);
    }
}

/* As the savestates' memory is taken: 512 KB of the game's data hashed in
 * pieces by the CPU and through the locked cache, timed (interrupts off),
 * and the faster kept, if both came out the same */
static void hash_check(void)
{
    const u8* data = (const u8*) 0x803B7240;
    u32 n = 512 * 1024 / BUF_SIZE, level, cpu_us, window_us = 0, same = 1;
    u64 sum_cpu = 0, sum_window = 0, t;
    level = IRQ_Disable();
    t = so_time_us();
    for (u32 k = 0; k < n; k++) {
        sum_cpu += hash_at(data + k * BUF_SIZE, BUF_SIZE, 1);
    }
    cpu_us = (u32) (so_time_us() - t);
    if (melee_wii_window_begin()) {
        const void* at[WINDOW_AHEAD + 1];
        t = so_time_us();
        for (u32 k = 0; k < n + WINDOW_AHEAD; k++) {
            if (k < n) {
                at[k % (WINDOW_AHEAD + 1)] = melee_wii_window_fetch(data + k * BUF_SIZE, BUF_SIZE);
            }
            if (k >= WINDOW_AHEAD) {
                u32 j = k - WINDOW_AHEAD;
                melee_wii_window_wait(j);
                sum_window += hash_at((const u8*) at[j % (WINDOW_AHEAD + 1)], BUF_SIZE, 0);
            }
        }
        same = melee_wii_copy_end() == 0 && sum_window == sum_cpu;
        window_us = (u32) (so_time_us() - t);
    }
    IRQ_Restore(level);
    s_hash_by_window = window_us != 0 && same && window_us < cpu_us;
    so_log("rollback: hashing 512 KB: %u us by the CPU, %u us through the locked cache%s; by the %s", (unsigned) cpu_us,
           (unsigned) window_us, same ? "" : " (NOT the same: not used)",
           s_hash_by_window ? "locked cache" : "CPU");
}

/* What the game's start takes from MEM2 after Slippi Online's: the game's
 * ARAM (16 MiB, ar.c), the files kept in RAM (6 MB), and some room */
#define AFTER_RESERVE (24u * 1024 * 1024)

void so_rollback_set(int on)
{
    s_enabled = on;
    if (!on) {
        return;
    }
    if (largest_block() >= AREA_0 + AREA_1 + AFTER_RESERVE) {
        reserve();
    } else {
        log_memory("as Slippi Online starts (the savestates' memory left for the game's start)");
    }
}

int so_rollback_enabled(void)
{
    return s_enabled;
}

/* This game's savestates are set up (so_rollback_start_game) */
int so_rollback_active(void)
{
    return s_ready;
}

/* wii/src/mmu_track.c: the pages the game writes */
int melee_wii_mmu_track_start(void);
void melee_wii_mmu_track_stop(void);
int melee_wii_mmu_tracking(void);
int melee_wii_mmu_tracks(u32 ea);
int melee_wii_mmu_page_changed(u32 ea);
/* wii/src/stall.c: a step number on the screen (where a hang stopped) */
void melee_wii_screen_mark(unsigned int n) __attribute__((weak));
#define MARK(n) (melee_wii_screen_mark ? melee_wii_screen_mark(n) : (void) 0)
void melee_wii_mmu_clear_ranges(const u32* pairs, int n);
void melee_wii_mmu_refresh(void);
int melee_wii_mmu_gpu_page(unsigned int ea);
int melee_wii_mmu_gpu_ranges(unsigned int* addr, unsigned int* len, int max);

/* What the captures copied, by 64 KiB of MEM1, by texture copies and by
 * pieces that are not tracked (copied at every capture): the log at a
 * game's end says where a savestate's size goes */
#define REGIONS (0x01800000u >> 16)
static u32 s_region_bytes[REGIONS];
static u32 s_gpu_bytes, s_untracked_bytes;
static void note_piece(const Seg* g)
{
    u32 r = (g->ea - 0x80000000u) >> 16;
    if (r < REGIONS) {
        s_region_bytes[r] += g->len;
    }
    if (!g->tracked) {
        s_untracked_bytes += g->len;
    } else if (melee_wii_mmu_gpu_page(g->ea)) {
        s_gpu_bytes += g->len;
    }
}

static void log_regions(void)
{
    char text[400];
    int used = 0;
    u32 n = s_stats.captures ? s_stats.captures : 1;
    unsigned int ga[8], gl[8];
    int gn = melee_wii_mmu_gpu_ranges(ga, gl, 8);
    so_log("rollback: a savestate's average: %u KB, %u KB of it texture copies, %u KB pieces not tracked",
           (unsigned) (s_stats.capture_bytes / n / 1024), (unsigned) (s_gpu_bytes / n / 1024),
           (unsigned) (s_untracked_bytes / n / 1024));
    for (int k = 0; k < 16; k++) {
        u32 best = 0, at = 0;
        for (u32 r = 0; r < REGIONS; r++) {
            if (s_region_bytes[r] > best) {
                best = s_region_bytes[r];
                at = r;
            }
        }
        if (best == 0 || used > (int) sizeof(text) - 32) {
            break;
        }
        used += snprintf(text + used, sizeof(text) - used, " %08x:%u", (unsigned) (0x80000000u + (at << 16)),
                         (unsigned) (best / n / 1024));
        s_region_bytes[at] = 0;
    }
    so_log("rollback: KB a savestate by 64 KB of MEM1:%s", text);
    used = 0;
    text[0] = 0;
    for (int i = 0; i < gn; i++) {
        used += snprintf(text + used, sizeof(text) - used, " %08x+%x", ga[i], gl[i]);
    }
    so_log("rollback: texture copies at%s", gn ? text : " (none)");
}

static void save_all(void);

/* ---- the memory a savestate keeps ---------------------------------------- */

static void add_range(u32 addr, u32 len)
{
    if (len == 0 || s_range_count >= MAX_RANGES) {
        return;
    }
    s_ranges[s_range_count].addr = addr;
    s_ranges[s_range_count].len = len;
    s_range_count++;
}

/* `r` less the excluded blocks, in order (they are sorted) */
static void add_less_excluded(Range r)
{
    u32 at = r.addr, end = r.addr + r.len;
    for (u32 i = 0; i < sizeof(EXCLUDE) / sizeof(EXCLUDE[0]); i++) {
        u32 lo = EXCLUDE[i].addr, hi = lo + EXCLUDE[i].len;
        if (hi <= at || lo >= end) {
            continue;
        }
        if (lo > at) {
            add_range(at, lo - at);
        }
        at = hi;
    }
    if (at < end) {
        add_range(at, end - at);
    }
}

static void build_ranges(void)
{
    u32 lo = *(volatile u32*) HSD_HEAP_LO, hi = *(volatile u32*) HSD_HEAP_HI;
    s_range_count = 0;
    for (u32 i = 0; i < sizeof(BACKUP_FIXED) / sizeof(BACKUP_FIXED[0]); i++) {
        add_less_excluded(BACKUP_FIXED[i]);
    }
    /* the match's heap (where the fighters, the stage's objects and the
     * game's state live) */
    if (lo >= 0x80000000u && hi > lo && hi <= 0x81800000u) {
        add_range(lo, hi - lo);
    }
    so_rollback_extra_ranges(add_range);
    s_state_size = 0;
    for (int i = 0; i < s_range_count; i++) {
        s_state_size += s_ranges[i].len;
    }
}

/* ---- the pieces ---------------------------------------------------------- */

/* The state's pieces: each range cut at the chunks' bounds; tracked when the
 * page is in the MMU's tracked part of MEM1. Each given a buffer for its
 * copy. */
static int build_segments(void)
{
    s_seg_count = 0;
    for (int i = 0; i < s_range_count; i++) {
        u32 a = s_ranges[i].addr, end = a + s_ranges[i].len;
        while (a < end) {
            u32 chunk_end = (a & ~(BUF_SIZE - 1)) + BUF_SIZE;
            u32 n = (chunk_end < end ? chunk_end : end) - a;
            Seg* g;
            if (s_seg_count >= MAX_SEGS) {
                return -1;
            }
            g = &s_segs[s_seg_count++];
            g->ea = a;
            g->len = (u16) n;
            g->tracked = (u8) melee_wii_mmu_tracks(a);
            a += n;
        }
    }
    /* each piece's copy, and room for a capture of the most of them */
    if ((u32) s_seg_count + 2048 > s_buf_count) {
        return -1;
    }
    s_free_count = 0;
    for (u32 b = s_buf_count; b-- > 0;) {
        s_free[s_free_count++] = (u16) b;
    }
    for (int i = 0; i < s_seg_count; i++) {
        s_cur[i] = s_free[--s_free_count];
    }
    s_run_count = 0;
    for (int i = 0; i < s_seg_count; i++) {
        const Seg* g = &s_segs[i];
        Run* r = s_run_count > 0 ? &s_runs[s_run_count - 1] : NULL;
        if (r != NULL && g->tracked && r->tracked && r->page == (g->ea & ~0xFFFu) && r->first + r->count == (u32) i) {
            r->count++;
            continue;
        }
        r = &s_runs[s_run_count++];
        r->page = g->ea & ~0xFFFu;
        r->first = (u32) i;
        r->count = 1;
        r->tracked = g->tracked;
    }
    return 0;
}

/* Is `addr` in the savestates (this game's)? (a test's: os_irq.c) */
int so_rollback_covers(u32 addr)
{
    for (int i = 0; s_ready && i < s_range_count; i++) {
        if (addr < s_ranges[i].addr + s_ranges[i].len && s_ranges[i].addr < addr + 0x1000) {
            return 1;
        }
    }
    return 0;
}

int so_rollback_covers_word(u32 addr)
{
    for (int i = 0; s_ready && i < s_range_count; i++) {
        if (addr >= s_ranges[i].addr && addr < s_ranges[i].addr + s_ranges[i].len) {
            return 1;
        }
    }
    return 0;
}

static void clear_changed(void)
{
    melee_wii_mmu_clear_ranges((const u32*) s_ranges, s_range_count); /* (Range: address, length) */
}

/* s_changed_page[i]: each piece's page written since the last clear; those
 * pieces listed in s_marked, in order (returns how many) */
static int mark_written(void)
{
    int count = 0;
    memset(s_changed_page, 0, (size_t) s_seg_count);
    for (int k = 0; k < s_run_count; k++) {
        const Run* r = &s_runs[k];
        if (r->tracked) {
            if (!melee_wii_mmu_page_changed(r->page)) {
                continue;
            }
            s_written_pages++;
        }
        for (u32 i = r->first; i < r->first + r->count; i++) {
            s_changed_page[i] = 1;
            s_marked[count++] = (int) i;
        }
    }
    return count;
}

/* ---- a game ------------------------------------------------------------- */

/* The GPU's command FIFO (PI's base and end): a load writing over commands
 * the GPU has yet to read would leave it reading garbage. Logged once. */
static void gpu_fifo_check(void)
{
    static int logged;
    volatile const u32* pi = (volatile const u32*) 0xCC003000;
    u32 base = (pi[3] & 0x1FFFFFE0u) | 0x80000000u, end = (pi[4] & 0x1FFFFFE0u) | 0x80000000u;
    if (logged++) {
        return;
    }
    for (int i = 0; i < s_range_count; i++) {
        if (s_ranges[i].addr < end && base < s_ranges[i].addr + s_ranges[i].len) {
            so_log("rollback: the GPU's FIFO %08x-%08x IS in the savestates (%08x+%x)", (unsigned) base,
                   (unsigned) end, (unsigned) s_ranges[i].addr, (unsigned) s_ranges[i].len);
            return;
        }
    }
    so_log("rollback: the GPU's FIFO %08x-%08x is not in the savestates", (unsigned) base, (unsigned) end);
}

void so_rollback_start_game(void)
{
    so_rollback_end_game();
    memset(&s_stats, 0, sizeof(s_stats));
    s_written_pages = 0;
    s_hashed_bytes = 0;
    memset(&s_sum, 0, sizeof(s_sum));
    memset(&s_lsum, 0, sizeof(s_lsum));
    s_load_hashed = s_load_extra = 0;
    s_load_hist_count = 0;
    s_skip_left = s_skipping = 0;
    s_big_kb = s_big_us = s_big_count = 0;
    memset(s_region_bytes, 0, sizeof(s_region_bytes));
    s_gpu_bytes = s_untracked_bytes = 0;
    if (!s_enabled) {
        return;
    }
    MARK(10);
    build_ranges();
    MARK(11);
    if (melee_wii_mmu_track_start() < 0) {
        so_log("rollback: no page tracking; no rollback this game");
        return;
    }
    MARK(12);
    reserve();
    MARK(13);
    if (s_buf_count == 0 || build_segments() < 0) {
        so_log("rollback: no memory for the savestates (%u KB in %d pieces); no rollback this game",
               (unsigned) (s_state_size / 1024), s_seg_count);
        so_rollback_end_game();
        return;
    }
    /* the copies: the state now; writes recorded from now */
    MARK(14);
    melee_wii_audio_hold();
    MARK(15);
    save_all();
    MARK(19);
    melee_wii_mmu_refresh();
    MARK(20);
    clear_changed();
    melee_wii_audio_release();
    MARK(21);
    s_rec_head = s_rec_tail = 0;
    s_cap_count = 0;
    s_ready = 1;
    s_window = ROLLBACK_MAX_FRAMES;
    gpu_fifo_check();
    {
        /* the game thread's floating point modes (FPSCR: rounding, non-IEEE
         * mode; HID2: paired singles), as the GameCube would have them */
        union {
            double d;
            u32 w[2];
        } f;
        u32 hid2, msr;
        __asm__ volatile("mffs %0" : "=f"(f.d));
        __asm__ volatile("mfspr %0, 920" : "=r"(hid2));
        __asm__ volatile("mfmsr %0" : "=r"(msr));
        so_log("rollback: the game's FPSCR %08x, HID2 %08x, MSR %08x", (unsigned) f.w[1], (unsigned) hid2,
               (unsigned) msr);
    }
    so_log("rollback: %d ranges, %u KB a state in %d pieces, %u KB for its copies and the undo records; copies by "
           "%s",
           s_range_count, (unsigned) (s_state_size / 1024), s_seg_count, (unsigned) (s_buf_count * BUF_SIZE / 1024),
           (const char*[]){ "the CPU", "the DMA into MEM2, the CPU out", "the CPU into MEM2, the DMA out",
                            "the DMA" }[melee_wii_copy_by_dma() & 3]);
}

void so_rollback_end_game(void)
{
    if (s_ready) {
        so_log("rollback: %u savestates (longest %u us: %u scanning, %u copying, %u clearing; most %u KB), "
               "%u loads (longest %u us, most %u KB), %u missing, %u dropped; %u time-sync skips, %u stalls; "
               "%u called with interrupts on",
               (unsigned) s_stats.captures, (unsigned) s_stats.max_capture_us, (unsigned) s_stats.scan_us,
               (unsigned) s_stats.copy_us, (unsigned) s_stats.clear_us, (unsigned) s_stats.max_capture_kb,
               (unsigned) s_stats.loads, (unsigned) s_stats.max_load_us, (unsigned) s_stats.max_load_kb,
               (unsigned) s_stats.missing, (unsigned) s_stats.dropped, (unsigned) s_stats.skips,
               (unsigned) s_stats.stalls, (unsigned) s_stats.irq_on);
        if (s_stats.captures) {
            u32 n = s_stats.captures;
            so_log("rollback: a savestate's average: %u us (%u finding the written pages, %u hashing them, %u "
                   "copying, %u clearing); %u pages written, %u KB hashed",
                   (unsigned) (s_sum.total_us / n), (unsigned) (s_sum.pages_us / n), (unsigned) (s_sum.hash_us / n),
                   (unsigned) (s_sum.copy_us / n), (unsigned) (s_sum.clear_us / n), (unsigned) (s_written_pages / n),
                   (unsigned) (s_hashed_bytes / n / 1024));
        }
        if (s_stats.loads) {
            u32 n = s_stats.loads;
            so_log("rollback: a load's average: %u us (%u finding its pieces, %u hashing, %u copying, %u the rest); %u "
                   "pieces hashed, %u put back unhashed",
                   (unsigned) (s_lsum.total_us / n), (unsigned) (s_lsum.pages_us / n), (unsigned) (s_lsum.hash_us / n),
                   (unsigned) (s_lsum.copy_us / n),
                   (unsigned) ((s_lsum.total_us - s_lsum.pages_us - s_lsum.hash_us - s_lsum.copy_us) / n),
                   (unsigned) (s_load_hashed / n), (unsigned) (s_load_extra / n));
        }
        log_regions();
        {
            /* the audio through the savestates and loads (ax_wii.c), this game */
            static unsigned int last[3];
            unsigned int now[3];
            melee_wii_audio_counts(&now[0], &now[1], &now[2]);
            so_log("rollback: the audio's interrupts handled inside savestates and loads: %u; a block begun inside "
                   "one %u times, %u of them played silence after it",
                   now[0] - last[0], now[1] - last[1], now[2] - last[2]);
            so_gamelog_note("audio_serviced=%u audio_guarded=%u audio_silent=%u", now[0] - last[0], now[1] - last[1],
                            now[2] - last[2]);
            last[0] = now[0];
            last[1] = now[1];
            last[2] = now[2];
        }
        so_gamelog_note("savestates=%u longest_us=%u most_kb=%u loads=%u longest_us=%u most_kb=%u missing=%u "
                        "dropped=%u time_sync_skips=%u stalls=%u",
                        (unsigned) s_stats.captures, (unsigned) s_stats.max_capture_us,
                        (unsigned) s_stats.max_capture_kb, (unsigned) s_stats.loads, (unsigned) s_stats.max_load_us,
                        (unsigned) s_stats.max_load_kb, (unsigned) s_stats.missing, (unsigned) s_stats.dropped,
                        (unsigned) s_stats.skips, (unsigned) s_stats.stalls);
        if (s_stats.captures) {
            u32 n = s_stats.captures;
            so_gamelog_note("savestate_us=%u pages_us=%u hash_us=%u copy_us=%u clear_us=%u pages=%u hashed_kb=%u",
                            (unsigned) (s_sum.total_us / n), (unsigned) (s_sum.pages_us / n),
                            (unsigned) (s_sum.hash_us / n), (unsigned) (s_sum.copy_us / n),
                            (unsigned) (s_sum.clear_us / n), (unsigned) (s_written_pages / n),
                            (unsigned) (s_hashed_bytes / n / 1024));
        }
        if (s_stats.loads) {
            u32 n = s_stats.loads;
            so_gamelog_note("load_us=%u pieces_us=%u hash_us=%u copy_us=%u hashed=%u unhashed=%u",
                            (unsigned) (s_lsum.total_us / n), (unsigned) (s_lsum.pages_us / n),
                            (unsigned) (s_lsum.hash_us / n), (unsigned) (s_lsum.copy_us / n),
                            (unsigned) (s_load_hashed / n), (unsigned) (s_load_extra / n));
        }
    }
    melee_wii_mmu_track_stop();
    /* (the savestates' memory kept for the next game) */
    s_ready = 0;
    s_cap_count = 0;
}


/* Why so_rollback_ready last said to wait: 1 to stay in time with the
 * opponent, 0 for the opponent's inputs */
int so_rollback_waits_in_time(void)
{
    return s_waits_in_time;
}

/* 0xB0: 1 for the game to play `frame`, 0 to wait (the client lock held) */
int so_rollback_ready(s32 frame, s32 finalized, int remotes)
{
    int need = 0;
    if (!s_ready) {
        /* no savestates: the polls after a game's end (one that started
         * without them is ended, so_exi.c): as the opponent's inputs come */
        for (int i = 0; i < remotes; i++) {
            if (so_np_remote_head_frame(i) < frame) {
                return 0;
            }
        }
        return 1;
    }
    for (int i = 0; i < remotes; i++) {
        s32 head = so_np_remote_head_frame(i);
        /* Dolphin's hasEnoughNewInputs */
        if (head - finalized < frame - finalized - s_window) {
            need = 1;
        }
    }
    s_waits_in_time = 0;
    if (need) {
        s_stats.stalls++;
        return 0;
    }
    /* Time sync (Dolphin's shouldSkipOnlineFrame): every 30 frames, if ahead
     * of the opponent, wait some frames: up to 5 at the start (by 10 ms),
     * later one (by two frames and 10 ms; the PC side slows down for less) */
    if (frame % SYNC_INTERVAL == 0 && !s_skipping) {
        s32 offset = so_np_time_offset_us();
        s32 limit = frame <= 120 ? 10000 : 2 * FRAME_US + 10000;
        if (offset > limit) {
            int most = frame <= 120 ? 5 : 1;
            s_skip_left = (offset - 10000) / FRAME_US + 1;
            if (s_skip_left > most) {
                s_skip_left = most;
            }
            s_skipping = 1;
        }
    }
    if (s_skip_left > 0) {
        s_skip_left--;
        s_stats.skips++;
        s_waits_in_time = 1;
        return 0;
    }
    s_skipping = 0;
    return 1;
}

/* ---- the copies and the undo records ------------------------------------- */

static void save_all(void)
{
    int again = 0;
    MARK(16);
    for (int i = 0; i < s_seg_count; i++) {
        s_hash[i] = piece_hash(&s_segs[i]);
    }
    MARK(17);
    do {
        melee_wii_copy_begin();
        for (int i = 0; i < s_seg_count; i++) {
            const Seg* g = &s_segs[i];
            melee_wii_copy(copy_of(g, s_cur[i]), (const u8*) g->ea, g->len);
        }
        /* (the DMA stuck: again, by the CPU) */
        MARK(18);
    } while (melee_wii_copy_end() != 0 && !again++);
}

/* The records [first, end): their buffers back to the free ones */
static void free_records(u32 first, u32 end)
{
    for (u32 r = first; r != end; r++) {
        s_free[s_free_count++] = s_recs[r % MAX_RECS].buf;
    }
}

/* The oldest capture goes (its records are never needed: they lead to
 * before it) */
static void drop_oldest(void)
{
    if (s_cap_count == 0) {
        return;
    }
    free_records(s_caps[0].first, s_caps[0].end);
    s_rec_tail = s_caps[0].end;
    memmove(&s_caps[0], &s_caps[1], (s_cap_count - 1) * sizeof(Capture));
    s_cap_count--;
    s_stats.dropped++;
}

static int room_for(u32 n)
{
    return s_free_count >= n && MAX_RECS - (s_rec_head - s_rec_tail) >= n;
}

/* A check of the savestates on the console (a hitch of some 30 ms each): the
 * pieces must be as their copies: those not marked changed before a capture
 * (the page tracking), and all of them after a capture or a load (the copies
 * themselves). After the first three captures and loads of a game, and every
 * 100th capture. Logs those that are not. */
#define CHECK_EVERY 100
/* (off: on the console every check came out right, and each one stalled the
 * game some 70 ms) */
#define CHECKS 0

static void check_pieces(const char* when, int unchanged_only)
{
    u32 bad = 0, checked = 0;
    for (int i = 0; i < s_seg_count; i++) {
        const Seg* g = &s_segs[i];
        if (unchanged_only && s_changed_page[i]) {
            continue;
        }
        checked++;
        if (memcmp((const void*) g->ea, copy_of(g, s_cur[i]), g->len) != 0) {
            if (bad < 3) {
                const u8* a = (const u8*) g->ea;
                const u8* b = copy_of(g, s_cur[i]);
                u32 o = 0, n = 0;
                for (u32 k = 0; k < g->len; k++) {
                    if (a[k] != b[k]) {
                        if (n++ == 0) {
                            o = k;
                        }
                    }
                }
                so_log("rollback check: %08x+%x (%s): %u bytes differ, first at +%x: %08x now, %08x in the copy",
                       (unsigned) g->ea, (unsigned) g->len, g->tracked ? "tracked" : "untracked", (unsigned) n,
                       (unsigned) o, (unsigned) so_get_u32(a + (o & ~3u)), (unsigned) so_get_u32(b + (o & ~3u)));
            }
            bad++;
        }
    }
    s_stats.checks++;
    s_stats.check_bad += bad;
    so_log("rollback check (%s): %u of %u pieces differ from their copies", when, (unsigned) bad, (unsigned) checked);
}

/* Tests: a console's times in Dolphin (busy, interrupts off as they are) */
static void test_slow(u32 us)
{
    u64 until = so_time_us() + us;
    while (us != 0 && so_time_us() < until) {
    }
}

/* 0xB1: keep the state the game is in before playing `frame` */
void so_savestate_capture(s32 frame)
{
    u64 t, t_pages, t_scan, t_copy, t_clear;
    u32 took, bytes = 0, n = 0;
    int keep_records, cand;
    Capture* c;
    u32 level;
    if (!s_ready) {
        return;
    }
    /* All at once, as on Dolphin, where the emulated machine stands still
     * meanwhile: the audio's interrupt (every 5 ms; the game's synth runs
     * in it) would otherwise change the game's sound state in the middle of
     * the copies, a capture keeping a torn state and a load leaving the
     * synth's lists half the old and half the new. */
    level = IRQ_Disable();
    s_stats.irq_on += level != 0;
    /* (the audio's interrupts due by now handled first: ax_wii.c) */
    melee_wii_audio_service();
    t = so_time_us();
    /* the pieces changed since the last capture: in the pages written, those
     * whose hash is not their copy's. (Not the GPU's texture copies, the
     * characters' shadows: drawn again each frame, and nothing reads them
     * but the GPU.) */
    melee_wii_mmu_refresh();
    cand = mark_written();
    t_pages = so_time_us();
    melee_wii_audio_guard();
    hash_list(cand);
    for (int k = 0; k < cand; k++) {
        int i = s_marked[k];
        s_hashed_bytes += s_segs[i].len;
        s_changed_page[i] = s_live[i] != s_hash[i];
        n += s_changed_page[i];
    }
    if (CHECKS && s_stats.captures % CHECK_EVERY == CHECK_EVERY - 1 && s_stats.checks < 20) {
        check_pieces("unmarked before a capture", 1);
    }
    t_scan = so_time_us();
    melee_wii_audio_guard();
    if (s_cap_count >= CAPTURES) {
        drop_oldest();
    }
    while (s_cap_count > 0 && !room_for(n)) {
        drop_oldest();
    }
    /* the first capture's records lead to before it: none needed (nor when
     * too much changed for the buffers: no going back past this) */
    keep_records = s_cap_count > 0 && room_for(n);
    if (!keep_records) {
        while (s_cap_count > 0) {
            drop_oldest();
        }
    }
    c = &s_caps[s_cap_count++];
    c->frame = frame;
    c->first = s_rec_head;
    melee_wii_copy_begin();
    for (int k = 0; k < cand; k++) {
        int i = s_marked[k];
        const Seg* g = &s_segs[i];
        if (!s_changed_page[i]) {
            continue;
        }
        if (keep_records) {
            Rec* r = &s_recs[s_rec_head++ % MAX_RECS];
            r->seg = (u16) i;
            r->buf = s_cur[i];
            r->hash_hi = (u32) (s_hash[i] >> 32);
            r->hash_lo = (u32) s_hash[i];
            s_cur[i] = s_free[--s_free_count];
        }
        s_hash[i] = s_live[i];
        melee_wii_copy(copy_of(g, s_cur[i]), (const u8*) g->ea, g->len);
        bytes += g->len;
        note_piece(g);
    }
    s_stats.capture_bytes += bytes;
    if (melee_wii_copy_end() != 0) {
        /* the DMA stuck: the same copies again, by the CPU */
        for (int k = 0; k < cand; k++) {
            int i = s_marked[k];
            if (s_changed_page[i]) {
                melee_wii_copy(copy_of(&s_segs[i], s_cur[i]), (const u8*) s_segs[i].ea, s_segs[i].len);
            }
        }
    }
    t_copy = so_time_us();
    melee_wii_audio_guard();
    test_slow(so_client_config()->test_slow_save_us);
    c->end = s_rec_head;
    if (!keep_records) {
        s_rec_tail = s_rec_head;
    }
    clear_changed();
    t_clear = so_time_us();
    if (CHECKS && s_stats.captures < 3) {
        check_pieces("after a capture", 0);
    }
    took = (u32) (so_time_us() - t);
    melee_wii_audio_service();
    IRQ_Restore(level);
    s_sum.pages_us += (u32) (t_pages - t);
    s_sum.hash_us += (u32) (t_scan - t_pages);
    s_sum.copy_us += (u32) (t_copy - t_scan);
    s_sum.clear_us += (u32) (t_clear - t_copy);
    s_sum.total_us += took;
    if (took > s_stats.max_capture_us) {
        s_stats.max_capture_us = took;
        s_stats.scan_us = (u32) (t_scan - t);
        s_stats.copy_us = (u32) (t_copy - t_scan);
        s_stats.clear_us = (u32) (t + took - t_copy);
    }
    note_big("savestate", frame, bytes / 1024, took);
    if (bytes / 1024 > s_stats.max_capture_kb) {
        s_stats.max_capture_kb = bytes / 1024;
    }
    so_gamelog_save(took, bytes / 1024);
    s_stats.captures++;
}

int so_rollback_describe(char* out, int size)
{
    int len = so_snprintf(out, size, "%u loads, %u savestates, %u missing; the last (back to <- from):",
                          (unsigned) s_stats.loads, (unsigned) s_stats.captures, (unsigned) s_stats.missing);
    for (u32 i = 0; i < LOAD_HISTORY && i < s_load_hist_count && len < size; i++) {
        u32 k = (s_load_hist_count - 1 - i) % LOAD_HISTORY;
        len += so_snprintf(out + len, size - len, " %d<-%d", (int) s_load_hist[k].target,
                           (int) s_load_hist[k].newest);
    }
    return len;
}

/* 0xB2: back to the state before `frame`, but for the preserve blocks
 * (`preserve`: big-endian address and length pairs, ended by 0) */
void so_savestate_load(s32 frame, const u8* preserve)
{
    static u8 kept[8192] __attribute__((aligned(32)));
    static u32 dc_addr[DEVCOM_MAX];
    static u8 dc_kept[DEVCOM_MAX][DEVCOM_SIZE];
    int dc_count;
    u32 addr[4], len[4], used = 0, bytes = 0;
    int blocks = 0, target = -1, hashed, extra = 0;
    u64 t_pages, t_hash, t_copy;
    u64 t;
    u32 took, level;
    if (!s_ready) {
        return;
    }
    for (int i = 0; i < s_cap_count; i++) {
        if (s_caps[i].frame == frame) {
            target = i;
        }
    }
    if (target < 0) {
        if (s_stats.missing++ < 4) {
            so_log("rollback: no savestate for frame %d", (int) frame);
        }
        return;
    }
    s_load_hist[s_load_hist_count % LOAD_HISTORY].target = frame;
    s_load_hist[s_load_hist_count % LOAD_HISTORY].newest = s_caps[s_cap_count - 1].frame;
    s_load_hist_count++;
    level = IRQ_Disable(); /* (as a capture) */
    s_stats.irq_on += level != 0;
    /* (the audio's interrupts due by now handled first: ax_wii.c) */
    melee_wii_audio_service();
    t = so_time_us();
    while (blocks < 4) {
        u32 a = so_get_u32(preserve + blocks * 8), l = so_get_u32(preserve + blocks * 8 + 4);
        if (s_stats.loads == 0 && a != 0) {
            so_log("rollback: the game keeps %08x+%x across loads", (unsigned) a, (unsigned) l);
        }
        if (a == 0 || used + l > sizeof(kept)) {
            if (a != 0) {
                so_log("rollback: NOT kept (no room): %08x+%x", (unsigned) a, (unsigned) l);
            }
            break;
        }
        memcpy(kept + used, (const void*) a, l);
        addr[blocks] = a;
        len[blocks] = l;
        used += (l + 31) & ~31u;
        blocks++;
    }
    dc_count = devcom_requests(dc_addr);
    for (int i = 0; i < dc_count; i++) {
        memcpy(dc_kept[i], (const void*) dc_addr[i], DEVCOM_SIZE);
    }
    /* the pieces to put back: those changed since the last capture (their
     * current copies are as of it), and those the captures after the
     * target changed, each capture undone newest first (its records' copies
     * the pieces' current again) */
    melee_wii_mmu_refresh();
    {
        u32 counted = s_written_pages;
        hashed = mark_written(); /* (in s_marked) */
        s_written_pages = counted; /* (the captures' count) */
    }
    /* the records' pieces changed since the target for sure: put back
     * without asking the hashes (s_extra, each once; s_changed_page 2) */
    for (int k = s_cap_count - 1; k > target; k--) {
        for (u32 r = s_caps[k].first; r != s_caps[k].end; r++) {
            const Rec* rec = &s_recs[r % MAX_RECS];
            s_free[s_free_count++] = s_cur[rec->seg];
            s_cur[rec->seg] = rec->buf;
            s_hash[rec->seg] = ((u64) rec->hash_hi << 32) | rec->hash_lo;
            if (s_changed_page[rec->seg] != 2) {
                s_changed_page[rec->seg] = 2;
                s_extra[extra++] = rec->seg;
            }
        }
    }
    t_pages = so_time_us();
    melee_wii_audio_guard();
    /* of the written pages' other pieces, the ones the memory doesn't hold
     * already (by the hashes) */
    {
        int left = 0;
        for (int k = 0; k < hashed; k++) {
            if (s_changed_page[s_marked[k]] == 1) {
                s_marked[left++] = s_marked[k];
            }
        }
        hashed = left;
    }
    hash_list(hashed);
    for (int k = 0; k < hashed; k++) {
        int i = s_marked[k];
        if (s_live[i] == s_hash[i]) {
            s_changed_page[i] = 0;
        }
    }
    t_hash = so_time_us();
    melee_wii_audio_guard();
    for (int again = 0; again < 2; again++) {
        melee_wii_copy_begin();
        for (int k = 0; k < hashed + extra; k++) {
            int i = k < hashed ? s_marked[k] : s_extra[k - hashed];
            const Seg* g = &s_segs[i];
            if (s_changed_page[i]) {
                melee_wii_copy((u8*) g->ea, copy_of(g, s_cur[i]), g->len);
                bytes += again ? 0 : g->len;
            }
        }
        if (melee_wii_copy_end() == 0) {
            break;
        }
        /* (the DMA stuck: the same copies again, by the CPU) */
    }
    t_copy = so_time_us();
    melee_wii_audio_guard();
    test_slow(so_client_config()->test_slow_load_us);
    /* Dolphin frees them all: the frames played again are kept again; the
     * current copies are the target's state */
    for (int k = 0; k <= target; k++) {
        free_records(s_caps[k].first, s_caps[k].end);
    }
    s_cap_count = 0;
    s_rec_tail = s_rec_head;
    /* (the table again: the CPU's stores since, the ends of pieces) */
    melee_wii_mmu_refresh();
    clear_changed();
    if (CHECKS && s_stats.loads < 3) {
        check_pieces("after a load", 0);
    }
    /* the game's online buffers keep what they have now (and count as
     * changed: the next capture keeps them) */
    used = 0;
    for (int i = 0; i < blocks; i++) {
        memcpy((void*) addr[i], kept + used, len[i]);
        used += (len[i] + 31) & ~31u;
    }
    for (int i = 0; i < dc_count; i++) {
        memcpy((void*) dc_addr[i], dc_kept[i], DEVCOM_SIZE);
    }
    took = (u32) (so_time_us() - t);
    melee_wii_audio_service();
    IRQ_Restore(level);
    s_lsum.pages_us += (u32) (t_pages - t);
    s_lsum.hash_us += (u32) (t_hash - t_pages);
    s_lsum.copy_us += (u32) (t_copy - t_hash);
    s_lsum.total_us += took;
    s_load_hashed += (u32) hashed;
    s_load_extra += (u32) extra;
    if (took > s_stats.max_load_us) {
        s_stats.max_load_us = took;
    }
    if (bytes / 1024 > s_stats.max_load_kb) {
        s_stats.max_load_kb = bytes / 1024;
    }
    note_big("load", frame, bytes / 1024, took);
    so_gamelog_load(took, frame);
    s_stats.loads++;
}

/* ---- a check of the savestates (test builds: wii/src/diag_dirty.c calls
 * this at each engine tick of a VS match) ----------------------------------
 * Tick 10: each piece's checksum noted, then a capture; 11 and 12: captures;
 * 13: back to 10, and every piece must be as noted. Logs the pieces that are
 * not. Offline, with no Slippi: the game goes on from tick 10's state. */
static u32* s_test_sums;

static u32 piece_sum(const Seg* g)
{
    const u8* p = (const u8*) g->ea;
    u32 h = 0x811C9DC5u;
    for (u32 i = 0; i < g->len; i++) {
        h = (h ^ p[i]) * 0x01000193u;
    }
    return h;
}

void so_rollback_selftest_tick(int tick)
{
    static const u8 no_preserve[8] = { 0 };
    if (tick == 1) {
        s_enabled = 1;
        so_rollback_start_game();
        return;
    }
    if (!s_ready) {
        return;
    }
    u32 level = IRQ_Disable(); /* as Slippi's capture and load run: nothing else writes meanwhile */
    if (tick == 10) {
        s_test_sums = malloc(s_seg_count * 4);
        if (s_test_sums == NULL) {
            IRQ_Restore(level);
            return;
        }
        for (int i = 0; i < s_seg_count; i++) {
            s_test_sums[i] = piece_sum(&s_segs[i]);
        }
        so_savestate_capture(10);
    } else if (tick == 11 || tick == 12) {
        so_savestate_capture(tick);
    } else if (tick == 13 && s_test_sums != NULL) {
        int bad = 0, bad_tracked = 0;
        so_savestate_load(10, no_preserve);
        for (int i = 0; i < s_seg_count; i++) {
            if (piece_sum(&s_segs[i]) != s_test_sums[i]) {
                if (bad < 12) {
                    so_log("rollback test: piece %d %08x+%x differs (%s)", i, (unsigned) s_segs[i].ea,
                           (unsigned) s_segs[i].len, s_segs[i].tracked ? "tracked" : "untracked");
                }
                bad++;
                bad_tracked += s_segs[i].tracked;
            }
        }
        so_log("rollback test: back to tick 10: %d of %d pieces differ (%d tracked): %s", bad, s_seg_count,
               bad_tracked, bad ? "FAIL" : "PASS");
        free(s_test_sums);
        s_test_sums = NULL;
    }
    IRQ_Restore(level);
}
