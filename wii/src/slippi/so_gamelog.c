/* Diagnostics: a record of each online game's timing (the launcher's
 * "Diagnostics" setting), for finding what makes games run badly on someone's
 * console and connection: the log's lines at a game's end are totals, and
 * say nothing of when a frame was late or why.
 *
 * While a game runs, a record is kept in memory of each time the game asks
 * for a frame's inputs (0xB0: once a frame, more when it waits) and of each
 * pad packet of an opponent's; nothing is written and nothing is logged
 * then, so that the record changes nothing of what it measures (the SD card
 * and the network share IOS). At the game's end a thread writes them to
 * sd:/apps/melee/logs/diag/game-<date>T<time>.bin and logs their summary.
 * wii/tools/gamelog.py reads the file.
 *
 * The file (big-endian): "MWDIAG3\0"; the length of the text; the size of a
 * poll's record and their number; the same of the packets' and of the
 * events' (u32 each); the text ("key=value" lines: the game, its totals);
 * the polls; the packets; the events (what the console did beside the game:
 * a broadcast, the SD card's reads and writes, slow sends; and every 250 ms
 * the Wi-Fi signal).
 *
 * A poll's record holds what happened from that poll to the next: the
 * savestates and the load the game asked for with its answer, the video
 * retraces, the time the game spent waiting for one. */

#include <malloc.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

#include <gccore.h>

#include "so_gamelog.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"

/* (small: the record's memory is what MEM2 has left beside the savestates
 * and the game's ARAM, about 1 MB) */
typedef struct {
    u32 t_us;        /* the poll, from the game's start */
    u16 frame;       /* the frame asked for (its low 16 bits) */
    u8 answer;       /* SO_GAMELOG_* in the low 3 bits; above them the video's retraces (1 a frame when the game
                      * keeps up), at most 31 */
    s8 ahead;        /* the frame, less the newest frame of the opponent's inputs: over 0, frames predicted */
    u8 rolled;       /* low 4 bits: the frames played again (a load back by that many); high 4: savestates */
    u8 packets;      /* the opponent's pad packets */
    u8 save_100us;   /* the savestates' time, in 0.1 ms (as the next three, each at most 25.5 ms) */
    u8 load_100us;   /* the load's */
    u8 wait_100us;   /* waiting for a retrace: the frame's spare time */
    u8 answer_100us; /* the poll's own time (the client's lock) */
    u8 ping_ms;      /* the round trip to the opponent */
    s8 offset_ms;    /* how far ahead of the opponent in time */
} Poll;

typedef struct {
    u32 t_us;
    u16 frame;  /* the newest frame in the packet (its low 16 bits) */
    u8 added;   /* low 6 bits: the frames it brought that had not come; high 2: which opponent */
    u8 ping_ms;
} Packet;

/* Something the console did beside the game (so_gamelog_event) */
typedef struct {
    u32 t_us;       /* when it began */
    u8 code;
    u8 value;       /* 'W': the Wi-Fi signal (wii/src/wifi.c), else 0 */
    u16 took_100us; /* how long it took, in 0.1 ms */
} Event;

_Static_assert(sizeof(Poll) == 16 && sizeof(Packet) == 8 && sizeof(Event) == 8, "the records' sizes are the file's");

#define MAX_POLLS 36000u   /* 10 minutes */
#define MAX_PACKETS 40000u
#define MAX_EVENTS 12000u
#define SPARE (256u * 1024u) /* MEM2 left alone for the rest */
#define TEXT_SIZE 4096u
#define CHUNK (32u * 1024u)
#define FOLDER "sd:/apps/melee/logs/diag"

enum { IDLE, RECORDING, ENDED };

static volatile int s_wanted;  /* the setting is on */
static volatile int s_enabled; /* and the record's memory is there */
static u32 s_max_polls, s_max_packets;
static volatile int s_state;
static Poll* s_polls;
static Packet* s_packets;
static Event* s_events;
static volatile u32 s_event_count;
static u32 s_max_events, s_lost_events;
static char* s_text;
static u8* s_head; /* (aligned, for the SD driver) */
#define HEAD_BYTES 36 /* the file's header (write_file) */
#define HEAD_SIZE 64  /* its buffer: whole cache lines */
static volatile u32 s_poll_count, s_packet_count;
static u32 s_text_len;
static u32 s_lost_polls, s_lost_packets;
static u64 s_start_us;
static time_t s_start_time;
static volatile u32 s_retraces;
static int s_started;
static u32 s_unrecorded; /* the polls of a game begun while the last one's record was being written */
int melee_wii_dvd_busy(void) __attribute__((weak)); /* wii/src/dvd.c */
u8* melee_wii_aram(void) __attribute__((weak));     /* wii/src/ar.c: the game's ARAM, once it has it */
int melee_wii_wifi_signal(void) __attribute__((weak));           /* wii/src/wifi.c */
const char* melee_wii_wifi_report(void) __attribute__((weak));
const char* melee_wii_wc24_status(void) __attribute__((weak));
#define SIGNAL_EVERY_US 250000u
static u64 s_signal_us;
static void add_event(int code, int value, u32 us, u64 now);

static u8 clamp8(u32 v)
{
    return v > 0xFF ? 0xFF : (u8) v;
}

static void add8(u8* to, u32 v)
{
    *to = clamp8((u32) *to + v);
}

int so_gamelog_recording(void)
{
    return s_state == RECORDING;
}

/* ---- the game's end: the summary and the file (the thread) --------------- */

static void line(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char* fmt, ...)
{
    char text[240];
    va_list ap;
    va_start(ap, fmt);
    so_vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    so_log("diag: %s", text);
    if (s_text_len + strlen(text) + 10 < TEXT_SIZE) {
        s_text_len += (u32) so_snprintf(s_text + s_text_len, (int) (TEXT_SIZE - s_text_len), "summary=%s\n", text);
    }
}

static int list(char* out, int size, const u32* counts, int n, int from)
{
    int len = 0;
    out[0] = '\0';
    for (int i = 0; i < n && len < size - 16; i++) {
        if (counts[i] != 0) {
            len += so_snprintf(out + len, size - len, " %d%s:%u", i + from, i == n - 1 ? "+" : "", (unsigned) counts[i]);
        }
    }
    return len;
}

static void summary(void)
{
    static const u32 EDGE[5] = { 17500, 21000, 34000, 50000, 0xFFFFFFFFu };
    u32 n = s_poll_count, plays = 0, waits[5] = { 0, 0, 0, 0, 0 }, gaps[5] = { 0, 0, 0, 0, 0 };
    u32 late = 0, tight = 0, ahead[9], rolled[9], saves = 0, loads = 0, longest_gap = 0, packet_gaps = 0, stale = 0;
    u32 ping_low = 0xFFFFFFFFu, ping_high = 0, pings = 0, longest_packet_gap = 0, rolled_frames = 0;
    u64 gap_sum = 0, spare_sum = 0, save_sum = 0, load_sum = 0, ping_sum = 0;
    char a[160], b[160];
    memset(ahead, 0, sizeof(ahead));
    memset(rolled, 0, sizeof(rolled));
    for (u32 i = 0; i < n; i++) {
        const Poll* p = &s_polls[i];
        u32 answer = p->answer & 7, retraces = p->answer >> 3, back = p->rolled & 15, saved = p->rolled >> 4;
        waits[answer < 5 ? answer : 4]++;
        if (answer != SO_GAMELOG_PLAY) {
            continue;
        }
        plays++;
        if (i + 1 < n) {
            u32 gap = s_polls[i + 1].t_us - p->t_us, k = 0;
            while (gap > EDGE[k]) {
                k++;
            }
            gaps[k]++;
            gap_sum += gap;
            longest_gap = gap > longest_gap ? gap : longest_gap;
        }
        late += retraces > 1;
        tight += p->wait_100us < 10;
        spare_sum += p->wait_100us * 100u;
        ahead[p->ahead <= 0 ? 0 : p->ahead > 8 ? 8 : p->ahead]++;
        if (back != 0) {
            rolled[back > 8 ? 8 : back]++;
            rolled_frames += back;
            loads++;
            load_sum += p->load_100us * 100u;
        }
        saves += saved;
        save_sum += p->save_100us * 100u;
        if (p->ping_ms != 0) {
            u32 ping = p->ping_ms;
            ping_low = ping < ping_low ? ping : ping_low;
            ping_high = ping > ping_high ? ping : ping_high;
            ping_sum += ping;
            pings++;
        }
    }
    for (u32 i = 0; i < s_packet_count; i++) {
        stale += (s_packets[i].added & 63) == 0;
        if (i != 0) {
            u32 gap = s_packets[i].t_us - s_packets[i - 1].t_us;
            packet_gaps += gap > 34000;
            longest_packet_gap = gap > longest_packet_gap ? gap : longest_packet_gap;
        }
    }
    if (plays == 0) {
        return;
    }
    if (s_event_count > s_max_events) {
        s_lost_events = s_event_count - s_max_events;
    }
    line("%u frames in %u polls (waits: %u for inputs, %u in time with the opponent, %u for the pad, %u other)%s",
         (unsigned) plays, (unsigned) n, (unsigned) waits[1], (unsigned) waits[2], (unsigned) waits[3],
         (unsigned) waits[4], s_lost_polls ? "; the record was full before the end" : "");
    line("a frame to the next: %u us on average, longest %u; %u up to 17.5 ms, %u to 21, %u to 34, %u to 50, %u over; "
         "the picture a retrace late in %u",
         (unsigned) (gap_sum / plays), (unsigned) longest_gap, (unsigned) gaps[0], (unsigned) gaps[1],
         (unsigned) gaps[2], (unsigned) gaps[3], (unsigned) gaps[4], (unsigned) late);
    line("a frame's spare time: %u us on average, under 1 ms in %u; %u savestates of %u us, %u loads of %u us, %u "
         "frames played again",
         (unsigned) (spare_sum / plays), (unsigned) tight, (unsigned) saves,
         (unsigned) (saves ? save_sum / saves : 0), (unsigned) loads, (unsigned) (loads ? load_sum / loads : 0),
         (unsigned) rolled_frames);
    list(a, sizeof(a), ahead, 9, 0);
    list(b, sizeof(b), rolled, 9, 0);
    line("frames ahead of the opponent's inputs (frames:times):%s; played again:%s", a, b[0] ? b : " none");
    line("ping %u to %u ms, %u on average; the opponent's packets: %u, %u with nothing new, %u over 34 ms "
         "after the one before (longest %u ms)%s",
         (unsigned) (pings ? ping_low : 0), (unsigned) ping_high, (unsigned) (pings ? ping_sum / pings : 0),
         (unsigned) s_packet_count, (unsigned) stale,
         (unsigned) packet_gaps, (unsigned) (longest_packet_gap / 1000),
         s_lost_packets ? "; more came than the record holds" : "");
    {
        /* what the console did beside the game: how often and the longest, by kind */
        static const char CODES[] = "BDRLTV";
        char text[200];
        int len = 0;
        u32 count = s_event_count < s_max_events ? s_event_count : s_max_events;
        text[0] = '\0';
        for (u32 k = 0; CODES[k] != '\0'; k++) {
            u32 times = 0, longest = 0;
            for (u32 i = 0; i < count; i++) {
                if (s_events[i].code == (u8) CODES[k]) {
                    times++;
                    longest = s_events[i].took_100us > longest ? s_events[i].took_100us : longest;
                }
            }
            if (times != 0) {
                len += so_snprintf(text + len, (int) sizeof(text) - len, " %c:%u (longest %u.%u ms)", CODES[k],
                                   (unsigned) times, (unsigned) (longest / 10), (unsigned) (longest % 10));
            }
        }
        line("beside the game (B broadcast, D disc read, R replay write, L log line, T slow send, V slow receive):%s%s",
             text[0] ? text : " nothing", s_lost_events ? "; more than the record holds" : "");
    }
    {
        /* the Wi-Fi signal (as a signed byte: dBm, by the SDK's levels) */
        u32 count = s_event_count < s_max_events ? s_event_count : s_max_events, samples = 0;
        s32 low = 127, high = -128, sum = 0;
        for (u32 i = 0; i < count; i++) {
            if (s_events[i].code == 'W') {
                s32 dbm = (s8) s_events[i].value;
                low = dbm < low ? dbm : low;
                high = dbm > high ? dbm : high;
                sum += dbm;
                samples++;
            }
        }
        if (samples != 0) {
            line("Wi-Fi signal %d to %d dBm, %d on average (%u readings)", (int) low, (int) high,
                 (int) (sum / (s32) samples), (unsigned) samples);
        }
    }
}

static int write_all(void* file, const void* data, u32 size)
{
    const u8* p = data;
    while (size != 0) {
        u32 n = size > CHUNK ? CHUNK : size;
        if (so_file_write(file, p, n) < 0) {
            return -1;
        }
        p += n;
        size -= n;
        so_sleep_us(2000); /* (the game's own reads of the card in between) */
    }
    return 0;
}

static void write_file(void)
{
    char name[96];
    struct tm* t = gmtime(&s_start_time);
    void* file;
    u32 polls = s_poll_count * sizeof(Poll), packets = s_packet_count * sizeof(Packet);
    u32 events = (s_event_count < s_max_events ? s_event_count : s_max_events) * sizeof(Event);
    int ok;
    so_snprintf(name, sizeof(name), FOLDER "/game-%04d%02d%02dT%02d%02d%02d.bin", t->tm_year + 1900, t->tm_mon + 1,
                t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    file = so_file_create(name);
    if (file == NULL) {
        so_log("diag: could not create %s", name);
        return;
    }
    memcpy(s_head, "MWDIAG3", 8);
    so_put_u32(s_head + 8, s_text_len);
    so_put_u32(s_head + 12, sizeof(Poll));
    so_put_u32(s_head + 16, s_poll_count);
    so_put_u32(s_head + 20, sizeof(Packet));
    so_put_u32(s_head + 24, s_packet_count);
    so_put_u32(s_head + 28, sizeof(Event));
    so_put_u32(s_head + 32, events / sizeof(Event));
    ok = so_file_write(file, s_head, HEAD_BYTES) == 0 && write_all(file, s_text, s_text_len) == 0 &&
         write_all(file, s_polls, polls) == 0 && write_all(file, s_packets, packets) == 0 &&
         write_all(file, s_events, events) == 0;
    so_file_close(file);
    if (ok) {
        so_log("diag: %s (%u bytes)", name, (unsigned) (36 + s_text_len + polls + packets + events));
    } else {
        so_log("diag: could not write %s", name);
    }
}

/* The record's memory, taken once the game has its own: the game's ARAM is
 * 16 MiB of MEM2 taken as the game starts, with little to spare beside the
 * savestates (with the record's taken first, a console with 20XX's menu on
 * had no 16 MiB left: "ARInit: no 16 MiB of MEM2"). After it the record
 * goes where there is room, smaller if need be, or there is none. */
static void take_memory(void)
{
    struct mallinfo before = mallinfo();
    u32 top = (u32) sbrk(0), end = (u32) SYS_GetArena2Hi();
    for (u32 part = 4; part >= 1 && !s_enabled; part--) {
        void* spare;
        s_max_polls = MAX_POLLS / 4 * part;
        s_max_packets = MAX_PACKETS / 4 * part;
        s_max_events = MAX_EVENTS / 4 * part;
        s_polls = memalign(32, s_max_polls * sizeof(Poll));
        s_packets = memalign(32, s_max_packets * sizeof(Packet));
        s_events = memalign(32, s_max_events * sizeof(Event));
        s_text = memalign(32, TEXT_SIZE);
        s_head = memalign(32, HEAD_SIZE);
        spare = memalign(32, SPARE); /* (what the game's other parts may still need) */
        free(spare);
        if (s_polls != NULL && s_packets != NULL && s_events != NULL && s_text != NULL && s_head != NULL &&
            spare != NULL)
        {
            s_enabled = 1;
        } else {
            free(s_polls);
            free(s_packets);
            free(s_events);
            s_events = NULL;
            free(s_text);
            free(s_head);
            s_polls = NULL;
            s_packets = NULL;
        }
    }
    if (s_enabled) {
        so_log("diag: on: each online game's record goes to " FOLDER " (%u KB kept for it: %u minutes of a game; "
               "MEM2 had %u KB free in the heap and %u KB not taken)",
               (unsigned) ((s_max_polls * sizeof(Poll) + s_max_packets * sizeof(Packet) + s_max_events * sizeof(Event)) /
                           1024),
               (unsigned) (s_max_polls / 3600), (unsigned) (before.fordblks / 1024),
               (unsigned) ((end > top ? end - top : 0) / 1024));
    } else {
        so_log("diag: no memory for a game's record (%u KB at the least; MEM2 has %u KB free in the heap and %u KB "
               "not taken): no diagnostics",
               (unsigned) ((MAX_POLLS * sizeof(Poll) + MAX_PACKETS * sizeof(Packet)) / 4 / 1024),
               (unsigned) (before.fordblks / 1024), (unsigned) ((end > top ? end - top : 0) / 1024));
        s_wanted = 0;
    }
}

static u32 WriterThread(void* unused)
{
    (void) unused;
    for (;;) {
        so_sleep_us(200000);
        if (s_wanted && !s_enabled && (melee_wii_aram == NULL || melee_wii_aram() != NULL)) {
            take_memory();
        }
        if (s_state != ENDED) {
            continue;
        }
        /* (the screen after the game loads from the card: after it, for at
         * most 10 s) */
        for (int i = 0; i < 50 && melee_wii_dvd_busy != NULL && melee_wii_dvd_busy(); i++) {
            so_sleep_us(200000);
        }
        summary();
        write_file();
        s_state = IDLE;
    }
    return 0;
}

/* ---- the game's side ------------------------------------------------------ */

void so_gamelog_enable(int on)
{
    s_wanted = on;
    if (on && !s_started) {
        s_started = 1;
        so_thread_start(WriterThread, NULL, NULL, 0, 0); /* (takes the memory, later) */
    }
}

void so_gamelog_start(void)
{
    if (!s_enabled) {
        return;
    }
    if (s_state == ENDED) {
        s_unrecorded = 1; /* (said if it is a game: the results screen polls too) */
        return;
    }
    s_unrecorded = 0;
    s_poll_count = 0;
    s_packet_count = 0;
    s_event_count = 0;
    s_lost_polls = s_lost_packets = s_lost_events = 0;
    s_text_len = 0;
    s_retraces = 0;
    s_start_us = so_time_us();
    s_start_time = time(NULL);
    s_signal_us = 0;
    s_state = RECORDING;
    if (melee_wii_wc24_status != NULL) {
        so_gamelog_note("wc24=%s", melee_wii_wc24_status());
    }
    if (melee_wii_wifi_report != NULL && melee_wii_wifi_report()[0] != '\0') {
        so_gamelog_note("wifi=%s", melee_wii_wifi_report());
    }
}

void so_gamelog_note(const char* fmt, ...)
{
    va_list ap;
    int n;
    if (s_state != RECORDING || s_text_len + 200 > TEXT_SIZE) {
        return;
    }
    va_start(ap, fmt);
    n = so_vsnprintf(s_text + s_text_len, 190, fmt, ap);
    va_end(ap);
    s_text_len += (u32) (n > 189 ? 189 : n < 0 ? 0 : n);
    s_text[s_text_len++] = '\n';
}

static Poll* current(void)
{
    u32 n = s_poll_count;
    return s_state == RECORDING && n != 0 ? &s_polls[n - 1] : NULL;
}

void so_gamelog_poll(u64 now_us, s32 frame, int answer, s32 head, s32 offset_us, u32 ping_us, u32 answer_us)
{
    Poll* p;
    Poll* last = current();
    s32 ahead = frame - head;
    u32 level, retraces;
    if (s_state != RECORDING) {
        if (s_unrecorded != 0 && s_unrecorded++ == 300) {
            so_log("diag: the last game's record was not written yet as this one began: none of this game");
        }
        return;
    }
    level = IRQ_Disable();
    retraces = s_retraces;
    s_retraces = 0;
    IRQ_Restore(level);
    if (last != NULL) {
        last->answer = (u8) ((last->answer & 7) | ((retraces > 31 ? 31 : retraces) << 3));
    }
    if (s_poll_count >= s_max_polls) {
        s_lost_polls++;
        return;
    }
    p = &s_polls[s_poll_count];
    memset(p, 0, sizeof(*p));
    p->t_us = (u32) (now_us - s_start_us);
    p->frame = (u16) frame;
    p->answer = (u8) (answer & 7);
    p->ahead = ahead > 127 ? 127 : ahead < -128 ? -128 : (s8) ahead;
    p->ping_ms = clamp8((ping_us + 500) / 1000);
    offset_us /= 1000;
    p->offset_ms = offset_us > 127 ? 127 : offset_us < -128 ? -128 : (s8) offset_us;
    p->answer_100us = clamp8(answer_us / 100);
    s_poll_count++;
    if (melee_wii_wifi_signal != NULL && now_us - s_signal_us >= SIGNAL_EVERY_US) {
        int signal = melee_wii_wifi_signal();
        s_signal_us = now_us;
        if (signal >= 0) {
            add_event('W', signal, 0, now_us);
        }
    }
}

void so_gamelog_end(void)
{
    if (s_state == RECORDING) {
        s_state = ENDED;
    }
}

void so_gamelog_cancel(void)
{
    if (s_state == RECORDING) {
        s_state = IDLE;
    }
}

void so_gamelog_save(u32 us, u32 kb)
{
    Poll* p = current();
    (void) kb;
    if (p != NULL) {
        if ((p->rolled >> 4) < 15) {
            p->rolled += 16;
        }
        add8(&p->save_100us, (us + 50) / 100);
    }
}

void so_gamelog_load(u32 us, s32 frame)
{
    Poll* p = current();
    if (p != NULL) {
        /* (the poll's frame is the one about to be played: its low 16 bits) */
        s32 back = (s32) (u16) (p->frame - (u16) frame);
        add8(&p->load_100us, (us + 50) / 100);
        p->rolled = (u8) ((p->rolled & 0xF0) | (back > 15 ? 15 : back));
    }
}

void so_gamelog_packet(u64 now_us, int remote, s32 frame, int added, u32 ping_us)
{
    Poll* p = current();
    Packet* k;
    if (s_state != RECORDING) {
        return;
    }
    if (p != NULL) {
        p->packets += p->packets < 255;
    }
    if (s_packet_count >= s_max_packets) {
        s_lost_packets++;
        return;
    }
    k = &s_packets[s_packet_count];
    k->t_us = (u32) (now_us - s_start_us);
    k->frame = (u16) frame;
    k->added = (u8) ((added > 63 ? 63 : added < 0 ? 0 : added) | ((remote & 3) << 6));
    k->ping_ms = clamp8((ping_us + 500) / 1000);
    s_packet_count++;
}

static void add_event(int code, int value, u32 us, u64 now)
{
    u32 level, at;
    Event* e;
    level = IRQ_Disable(); /* (any thread) */
    at = s_event_count++;
    IRQ_Restore(level);
    if (at >= s_max_events) {
        return; /* (counted: the file says how many there were) */
    }
    e = &s_events[at];
    e->t_us = (u32) (now - us - s_start_us);
    e->code = (u8) code;
    e->value = (u8) value;
    e->took_100us = us / 100 > 0xFFFF ? 0xFFFF : (u16) (us / 100);
}

void so_gamelog_event(int code, u32 us)
{
    if (s_state != RECORDING) {
        return;
    }
    add_event(code, 0, us, so_time_us());
}

void so_gamelog_mark(int code, int value)
{
    if (s_state != RECORDING) {
        return;
    }
    add_event(code, value, 0, so_time_us());
}

void so_gamelog_retrace(void)
{
    if (s_state == RECORDING) {
        s_retraces++;
    }
}

void so_gamelog_video_wait(u32 us)
{
    Poll* p = current();
    if (p != NULL) {
        add8(&p->wait_100us, (us + 50) / 100);
    }
}
