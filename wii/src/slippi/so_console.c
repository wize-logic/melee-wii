/* Slippi's console connection, as Slippi Nintendont serves it: Slippi
 * Launcher's "Console Mirroring" connects to the Wii on TCP 51441 and gets
 * each game's replay data as it is played (to mirror it in Dolphin, record
 * it, or relay it to spectators). The Wii says it is there with a
 * "SLIP_READY" datagram on UDP 20582 every 10 seconds while no one is
 * connected, so Slippi Launcher lists it.
 *
 * The protocol is Slippi Nintendont's (kernel/SlippiNetwork.c,
 * SlippiCommunication.c, SlippiNetworkBroadcast.c in Slippi Nintendont):
 * messages are a 4-byte big-endian size and a UBJSON object, {"type": 1,
 * "payload": {...}} for the handshake, 2 for replay data and 3 to keep the
 * connection alive. The recording stream (what Slippi's recording codes write
 * to the device, wii/src/exi.c) goes into a ring of 2.5 MB; each client reads
 * it from its own cursor, whole events at a time, so a client that comes back
 * (with its token) picks up where it was.
 *
 * One thread does it all: the network calls of the online client are on
 * their own thread (so_socket_ogc.c), and this one never waits in IOS (its
 * polls return at once; it sleeps between them), as IOS socket calls
 * blocking in several threads at once stalled on Wi-Fi. */

#include <gccore.h>
#include <malloc.h>
#include <network.h>

#include "so_console.h"
#include "so_gamelog.h"
#include "so_platform.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

#define IOS_POLLIN 0x0001

#define SERVER_PORT 51441
#define BROADCAST_PORT 20582
#define BROADCAST_US (10 * 1000000ull)
#define KEEPALIVE_US (500 * 1000ull)
#define HANDSHAKE_US (5 * 1000000ull)

#define RING_SIZE 0x280000u
#define MAX_TX 25000u
#define MSG_MAX 1024u

#define CMD_PAYLOADS 0x35
#define CMD_GAME_END 0x39

enum { MSG_HANDSHAKE = 1, MSG_REPLAY = 2, MSG_KEEPALIVE = 3 };

/* The recording stream: written by the game's EXI path */
static u8* s_ring;
static volatile u64 s_write;      /* bytes ever written */
static volatile u64 s_game_base;  /* where the current (or last) game starts */
static volatile int s_in_game;

/* The connected client */
static s32 s_server = -1, s_client = -1, s_beacon = -1;
static u64 s_cursor;
static u32 s_token, s_prev_token;
static u64 s_prev_cursor;
static int s_force_pos;
static u64 s_last_io, s_last_beacon;
static u16 s_sizes[256]; /* the client's game's payload sizes, from its 0x35 */
static int s_have_sizes;

static char s_nick[32];
static const char* s_version = "1.0.0";
static u8 s_tx[MAX_TX + 128] __attribute__((aligned(32)));
static u8 s_msg[MAX_TX + 256] __attribute__((aligned(32)));
static int s_started;

static u64 now_us(void)
{
    return so_time_us();
}

static u64 write_pos(void)
{
    u32 level;
    u64 pos;
    level = IRQ_Disable();
    pos = s_write;
    IRQ_Restore(level);
    return pos;
}

void so_console_data(const u8* data, u32 len)
{
    u32 at;
    if (s_ring == NULL || len == 0 || len > RING_SIZE) {
        return;
    }
    at = (u32) (s_write % RING_SIZE);
    if (at + len > RING_SIZE) {
        u32 first = RING_SIZE - at;
        memcpy(s_ring + at, data, first);
        memcpy(s_ring, data + first, len - first);
    } else {
        memcpy(s_ring + at, data, len);
    }
    if (data[0] == CMD_PAYLOADS) {
        s_game_base = s_write;
        s_in_game = 1;
    } else if (data[0] == CMD_GAME_END) {
        s_in_game = 0;
    }
    s_write += len;
}

static u8 ring_at(u64 pos)
{
    return s_ring[pos % RING_SIZE];
}

static void ring_copy(u8* out, u64 pos, u32 len)
{
    u32 at = (u32) (pos % RING_SIZE);
    if (at + len > RING_SIZE) {
        u32 first = RING_SIZE - at;
        memcpy(out, s_ring + at, first);
        memcpy(out + first, s_ring, len - first);
    } else {
        memcpy(out, s_ring + at, len);
    }
}

/* ---- UBJSON, as Slippi Nintendont writes it ------------------------------- */

typedef struct {
    u8* p;
    u8* start;
} Ubj;

static void ubj_u8(Ubj* u, u8 v)
{
    *u->p++ = v;
}

static void ubj_count(Ubj* u, u32 n)
{
    if (n < 256) {
        ubj_u8(u, 'U');
        ubj_u8(u, (u8) n);
    } else if (n < 32768) {
        ubj_u8(u, 'I');
        ubj_u8(u, (u8) (n >> 8));
        ubj_u8(u, (u8) n);
    } else {
        ubj_u8(u, 'l');
        so_put_u32(u->p, n);
        u->p += 4;
    }
}

static void ubj_key(Ubj* u, const char* key)
{
    u32 n = (u32) strlen(key);
    ubj_count(u, n);
    memcpy(u->p, key, n);
    u->p += n;
}

static void ubj_string(Ubj* u, const char* s)
{
    ubj_u8(u, 'S');
    ubj_key(u, s);
}

static void ubj_bytes(Ubj* u, const void* data, u32 n)
{
    ubj_u8(u, '[');
    ubj_u8(u, '$');
    ubj_u8(u, 'U');
    ubj_u8(u, '#');
    ubj_count(u, n);
    memcpy(u->p, data, n);
    u->p += n;
}

static void msg_begin(Ubj* u, int type)
{
    u->start = s_msg;
    u->p = s_msg + 4;
    ubj_u8(u, '{');
    ubj_key(u, "type");
    ubj_u8(u, 'U');
    ubj_u8(u, (u8) type);
}

static int msg_send(Ubj* u, int objects)
{
    u32 size, sent = 0;
    while (objects-- > 0) {
        ubj_u8(u, '}');
    }
    size = (u32) (u->p - u->start);
    so_put_u32(u->start, size - 4);
    while (sent < size) {
        s32 res = net_send(s_client, u->start + sent, size - sent, 0);
        if (res <= 0) {
            return -1;
        }
        sent += (u32) res;
    }
    s_last_io = now_us();
    return 0;
}

/* ---- the client ---------------------------------------------------------- */

static void drop_client(void)
{
    if (s_client >= 0) {
        so_log("console: %s disconnected", "Slippi Launcher");
        net_close(s_client);
    }
    s_prev_token = s_token;
    s_prev_cursor = s_cursor;
    s_client = -1;
    s_last_beacon = 0;
}

static int wait_readable(s32 sock, s32 timeout_ms)
{
    struct pollsd pd;
    s32 res;
    pd.socket = sock;
    pd.events = IOS_POLLIN;
    pd.revents = 0;
    res = net_poll(&pd, 1, timeout_ms);
    return res > 0 && (pd.revents & IOS_POLLIN);
}

/* The handshake's value after `key`: a byte array (n bytes), or a bool */
static const u8* find_key(const u8* msg, u32 len, const char* key)
{
    u32 n = (u32) strlen(key);
    for (u32 i = 0; i + n < len; i++) {
        if (memcmp(msg + i, key, n) == 0) {
            return msg + i + n;
        }
    }
    return NULL;
}

static int read_bytes(const u8* v, const u8* end, u8* out, u32 n)
{
    /* [$U#U<n> (or #i<n>) then n bytes */
    if (v == NULL || v + 6 + n > end || v[0] != '[' || v[1] != '$' || v[2] != 'U' || v[3] != '#' ||
        (v[4] != 'U' && v[4] != 'i') || v[5] != n)
    {
        return -1;
    }
    memcpy(out, v + 6, n);
    return 0;
}

static int handshake(s32 sock)
{
    u8 buf[MSG_MAX];
    u32 size = 0, have = 0;
    u64 until = now_us() + HANDSHAKE_US;
    u8 cursor_be[8], token_be[4];
    const u8* v;
    u64 cursor;
    u32 token;
    int fresh, realtime;
    u64 pos;
    Ubj u;

    while (now_us() < until && (size == 0 || have < size + 4)) {
        s32 res;
        if (!wait_readable(sock, 0)) {
            so_sleep_us(10000);
            continue;
        }
        res = net_recv(sock, buf + have, (size == 0 ? 4 : size + 4) - have, 0);
        if (res <= 0) {
            return -1;
        }
        have += (u32) res;
        if (size == 0 && have >= 4) {
            size = so_get_u32(buf);
            if (size == 0 || size + 4 > sizeof(buf)) {
                return -2;
            }
        }
    }
    if (size == 0 || have < size + 4) {
        return -3;
    }
    if (buf[4] != '{' || (v = find_key(buf + 4, size, "type")) == NULL ||
        (v[0] != 'U' && v[0] != 'i') || v[1] != MSG_HANDSHAKE)
    {
        return -4;
    }
    memset(cursor_be, 0, sizeof(cursor_be));
    memset(token_be, 0, sizeof(token_be));
    read_bytes(find_key(buf + 4, size, "cursor"), buf + 4 + size, cursor_be, 8);
    read_bytes(find_key(buf + 4, size, "clientToken"), buf + 4 + size, token_be, 4);
    v = find_key(buf + 4, size, "isRealtime");
    realtime = v != NULL && v[0] == 'T';
    cursor = ((u64) so_get_u32(cursor_be) << 32) | so_get_u32(cursor_be + 4);
    token = so_get_u32(token_be);

    /* The cursor: a client that comes back (its token) may go on where it
     * was, within the current game (or the last one, if it ended since);
     * any other starts at the current game's start, or at what comes next */
    pos = write_pos();
    fresh = token == 0 || token != s_prev_token;
    if (fresh) {
        cursor = s_in_game ? s_game_base : pos;
        do {
            token = (u32) (now_us() * 2654435761u) ^ (u32) (now_us() >> 17);
        } while (token == 0 || token == s_prev_token);
    } else if (!s_in_game) {
        if (cursor < s_game_base || cursor > pos) {
            cursor = pos;
        }
    } else if (cursor < s_game_base || cursor > pos) {
        cursor = s_game_base;
    }
    if (pos - cursor >= RING_SIZE) {
        cursor = pos;
    }
    s_client = sock;
    s_cursor = cursor;
    s_token = token;
    s_force_pos = 0;
    s_have_sizes = 0;
    if (realtime) {
        int one = 1;
        net_setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    so_put_u32(token_be, token);
    so_put_u32(cursor_be, (u32) (cursor >> 32));
    so_put_u32(cursor_be + 4, (u32) cursor);
    msg_begin(&u, MSG_HANDSHAKE);
    ubj_key(&u, "payload");
    ubj_u8(&u, '{');
    ubj_key(&u, "nick");
    ubj_string(&u, s_nick);
    ubj_key(&u, "nintendontVersion");
    ubj_string(&u, s_version);
    ubj_key(&u, "clientToken");
    ubj_bytes(&u, token_be, 4);
    ubj_key(&u, "pos");
    ubj_bytes(&u, cursor_be, 8);
    if (msg_send(&u, 2) < 0) {
        return -5;
    }
    so_log("console: Slippi Launcher connected (%s, %s)", fresh ? "new" : "back again",
           realtime ? "realtime" : "buffered");
    return 0;
}

/* The client's game's payload sizes, from the 0x35 at `pos` */
static void read_sizes(u64 pos)
{
    u8 table[256 * 3 + 2];
    u32 n = ring_at(pos + 1);
    memset(s_sizes, 0, sizeof(s_sizes));
    ring_copy(table, pos, n + 1 < sizeof(table) ? n + 1 : sizeof(table));
    for (u32 i = 2; i + 3 <= n + 1; i += 3) {
        s_sizes[table[i]] = (u16) ((table[i + 1] << 8) | table[i + 2]);
    }
    s_sizes[CMD_PAYLOADS] = (u16) n;
    s_have_sizes = 1;
}

/* Sends what the client has yet to get, whole events, up to MAX_TX */
static int send_replay(void)
{
    u64 pos = write_pos(), from = s_cursor, at = s_cursor;
    u32 len;
    u8 cursor_be[8], next_be[8];
    Ubj u;

    if (pos - at >= RING_SIZE) {
        /* too far behind: the data is gone; on from what comes next */
        so_log("console: the client fell behind; skipping ahead");
        s_cursor = pos;
        s_force_pos = 1;
        s_have_sizes = 0;
        return 0;
    }
    while (at < pos) {
        u8 cmd = ring_at(at);
        u32 size;
        if (cmd == CMD_PAYLOADS) {
            if (at != from) {
                break; /* a new game: in a message of its own */
            }
            read_sizes(at);
        }
        if (!s_have_sizes || (size = s_sizes[cmd]) == 0) {
            /* not the start of an event we know: on to what comes next */
            so_log("console: lost the event stream at %02x; skipping ahead", cmd);
            if (at == from) {
                s_cursor = pos;
                s_force_pos = 1;
                return 0;
            }
            break;
        }
        if (at + 1 + size > pos || (u32) (at + 1 + size - from) > MAX_TX) {
            break;
        }
        at += 1 + size;
        if (cmd == CMD_GAME_END) {
            break;
        }
    }
    len = (u32) (at - from);
    if (len == 0) {
        return 0;
    }
    ring_copy(s_tx, from, len);
    if (write_pos() - from >= RING_SIZE) {
        return 0; /* overwritten while copied: the next call skips ahead */
    }
    so_put_u32(cursor_be, (u32) (from >> 32));
    so_put_u32(cursor_be + 4, (u32) from);
    so_put_u32(next_be, (u32) (at >> 32));
    so_put_u32(next_be + 4, (u32) at);
    msg_begin(&u, MSG_REPLAY);
    ubj_key(&u, "payload");
    ubj_u8(&u, '{');
    ubj_key(&u, "pos");
    ubj_bytes(&u, cursor_be, 8);
    ubj_key(&u, "nextPos");
    ubj_bytes(&u, next_be, 8);
    ubj_key(&u, "forcePos");
    ubj_u8(&u, s_force_pos ? 'T' : 'F');
    ubj_key(&u, "data");
    ubj_bytes(&u, s_tx, len);
    if (msg_send(&u, 2) < 0) {
        return -1;
    }
    s_cursor = at;
    s_force_pos = 0;
    return 1;
}

static int keep_alive(void)
{
    Ubj u;
    if (now_us() - s_last_io < KEEPALIVE_US) {
        return 0;
    }
    msg_begin(&u, MSG_KEEPALIVE);
    return msg_send(&u, 1);
}

/* ---- the server ---------------------------------------------------------- */

static s32 open_server(void)
{
    struct sockaddr_in name;
    s32 s = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s < 0) {
        return s;
    }
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_len = 8; /* (IOS's length of an IPv4 address: so_socket_ogc.c) */
    name.sin_port = htons(SERVER_PORT);
    name.sin_addr.s_addr = INADDR_ANY;
    {
        s32 res = net_bind(s, (struct sockaddr*) &name, sizeof(name));
        if (res >= 0) {
            res = net_listen(s, 1);
        }
        if (res < 0) {
            static int logged;
            if (!logged) {
                logged = 1;
                so_log("console: no server on port %d (%d); trying again now and then", SERVER_PORT,
                       (int) res);
            }
            net_close(s);
            return res;
        }
    }
    return s;
}

static void beacon(void)
{
    struct {
        char cmd[10];
        u8 mac[6];
        char nick[32];
    } __attribute__((packed)) msg;
    static u8 mac[6];
    static int have_mac;
    struct sockaddr_in to;

    if (s_last_beacon != 0 && now_us() - s_last_beacon < BROADCAST_US) {
        return;
    }
    s_last_beacon = now_us();
    if (s_beacon < 0) {
        int one = 1;
        s_beacon = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (s_beacon < 0) {
            return;
        }
        net_setsockopt(s_beacon, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    }
    if (!have_mac) {
        have_mac = net_get_mac_address(mac) >= 0;
    }
    memset(&msg, 0, sizeof(msg));
    memcpy(msg.cmd, "SLIP_READY", 10);
    memcpy(msg.mac, mac, 6);
    memcpy(msg.nick, s_nick, strnlen(s_nick, sizeof(msg.nick)));
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_len = 8;
    to.sin_port = htons(BROADCAST_PORT);
    to.sin_addr.s_addr = 0xFFFFFFFF;
    {
        u64 start = now_us();
        net_sendto(s_beacon, &msg, sizeof(msg), 0, (struct sockaddr*) &to, 8);
        so_gamelog_event('B', (u32) (now_us() - start));
    }
}

int so_net_wait_ready(void); /* so_socket_ogc.c */

static u32 ConsoleThread(void* unused)
{
    int retries = 0;
    (void) unused;
    so_net_wait_ready();
    so_log("console: waiting for Slippi Launcher on port %d", SERVER_PORT);
    for (;;) {
        if (s_server < 0) {
            if (retries >= 10) {
                so_sleep_us(1000000);
                continue;
            }
            s_server = open_server();
            if (s_server < 0) {
                retries++;
                so_sleep_us(1000000);
                continue;
            }
            retries = 0;
        }
        if (s_client < 0) {
            beacon();
            if (!wait_readable(s_server, 0)) {
                so_sleep_us(100000);
            } else {
                struct sockaddr_in peer;
                socklen_t peer_len = sizeof(peer);
                s32 sock;
                memset(&peer, 0, sizeof(peer));
                /* (libogc's accept wants the address's room) */
                sock = net_accept(s_server, (struct sockaddr*) &peer, &peer_len);
                if (sock < 0) {
                    net_close(s_server);
                    s_server = -1;
                    continue;
                }
                int res = handshake(sock);
                if (res < 0) {
                    so_log("console: no handshake from the client (%d)", res);
                    net_close(sock);
                    s_client = -1;
                }
            }
            continue;
        }
        {
            int sent = send_replay();
            if (sent < 0 || keep_alive() < 0) {
                drop_client();
                continue;
            }
            if (sent == 0) {
                so_sleep_us(2000);
            }
        }
    }
    return 0;
}

int so_console_start(const char* nickname, const char* version)
{
    if (s_started) {
        return 0;
    }
    s_ring = memalign(32, RING_SIZE);
    if (s_ring == NULL) {
        return -1;
    }
    so_snprintf(s_nick, sizeof(s_nick), "%s", nickname != NULL && nickname[0] ? nickname : "Wii");
    if (version != NULL) {
        s_version = version;
    }
    if (so_socket_init() < 0 || so_thread_start(ConsoleThread, NULL, NULL, 32 * 1024, 0) < 0) {
        return -2;
    }
    s_started = 1;
    return 0;
}
