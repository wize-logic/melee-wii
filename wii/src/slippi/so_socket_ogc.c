/* so_socket.h for the Wii port, on libogc's sockets (IOS's /dev/net/ip/top,
 * as Slippi Nintendont's kernel calls it directly).
 *
 * One thread makes every socket call, in turn: the datagrams queued to send,
 * then a poll of the socket (up to poll_ms) and a receive when one has come.
 * IOS socket calls blocking in several threads at once stalled and failed on
 * Wi-Fi (so_socket.h), and a
 * send there takes 13-18 ms, so nothing that calls in here waits for IOS:
 * sends are queued, and what arrives waits in a ring, with the wake event
 * signalled. The network comes up (DHCP, net_init) in the background:
 * so_local_ip is 0 until it has. */

#include <gccore.h>
#include <network.h>

#include "so_alloc.h"
#include "so_gamelog.h"
#include "so_socket.h"
#include "so_util.h"
#include "string.h"

/* IOS's poll flag (BSD's; <poll.h>'s POLLIN is not it) */
#define IOS_POLLIN 0x0001

/* The length of an IPv4 address as IOS takes it: 8 (its length, family,
 * port, address), not libogc's sizeof(struct sockaddr_in), 16. libogc's
 * net_bind puts 8 in itself; its net_sendto passes the length it is given
 * (and writes it into sin_len), and a Wii's IOS sends nothing to an address
 * of 16 (Dolphin's takes either). */
#define IOS_SOCKADDR_LEN 8

#define RX_SLOTS 32
#define TX_SLOTS 16

typedef struct {
    u32 addr;
    u16 port;
    u16 len;
    u64 arrived_us;
    u8 data[SO_DATAGRAM_MAX];
} RxSlot;

typedef struct {
    int aux;
    u32 addr;
    u16 port;
    u16 len;
    u8 data[SO_DATAGRAM_MAX];
} TxSlot;

static volatile int s_net_ready;
static volatile u32 s_ip;
static s32 s_sock = -1, s_aux = -1;
static volatile int s_want_port = -1; /* the port the I/O thread is to open */
static u16 s_port;
static SoEvent* s_wake;
static SoEvent s_io_event;
static RxSlot s_rx[RX_SLOTS];
static u32 s_rx_head, s_rx_tail;
static TxSlot s_tx[TX_SLOTS];
static u32 s_tx_head, s_tx_tail;
static SoMutex s_rx_lock, s_tx_lock, s_stats_lock;
static SoSocketStats s_stats;
static int s_idle_wanted;
static u32 s_poll_ms = 2;
static volatile s32 s_last_send_error;

/* Diagnostics: the last IOS sendto's error (0: none yet) */
int so_socket_last_send_error(void)
{
    return s_last_send_error;
}

static void bump(u32* counter)
{
    so_mutex_lock(&s_stats_lock);
    (*counter)++;
    so_mutex_unlock(&s_stats_lock);
}

/* so_socket_set_wifi's, for NetUpThread */
static volatile int s_wifi_known, s_wifi;
static u16 s_wifi_rateset = 0xFFFF;
static u8 s_wifi_retry = 0xFF;
int melee_wii_wifi_tune(void* config, int rateset, int retry) __attribute__((weak)); /* wii/src/wifi.c */
const char* melee_wii_wifi_report(void) __attribute__((weak));
int melee_wii_wifi_roam_guard(void) __attribute__((weak));       /* wii/src/wifi.c */
void melee_wii_fatal(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2))); /* wii/src/main.c */

/* The game's current scene, read the same way pad.c does: the low 16 bits of
 * the word rotated left 8 are (minor << 8) | major; online in-game is 0x0208.
 * Roaming is re-checked each time this LEAVES the in-game scene, i.e. a match
 * ended and the player is on the way back to character select -- so the check,
 * and any stop, happens between matches, never during one. */
#define SCENE_WORD 0x80479D30u
#define SCENE_ONLINE_IN_GAME 0x0208u
#define SCENE_POLL_US 250000u

void so_socket_set_wifi(int wifi, u16 rateset, u8 retry)
{
    s_wifi = wifi;
    s_wifi_rateset = rateset;
    s_wifi_retry = retry;
    s_wifi_known = 1;
}

/* Before net_init: the Wi-Fi connection's rates and retries for this
 * session, when config.ini names any (tests: wifi_rate, wifi_retries).
 * Without, nothing is asked of IOS: the connection is as Wii Settings saved
 * it, which leaves both to the Wi-Fi module. */
static void tune_wifi(void)
{
    void* config;
    for (int i = 0; i < 300 && !s_wifi_known; i++) {
        so_sleep_us(10000);
    }
    if (!s_wifi_known || !s_wifi || melee_wii_wifi_tune == NULL ||
        (s_wifi_rateset == 0xFFFF && s_wifi_retry == 0xFF))
    {
        return;
    }
    config = so_malloc(7008); /* (NCD's whole configuration: 7004 bytes) */
    if (config == NULL) {
        so_log("socket: no memory to read the Wi-Fi settings: left as saved");
        return;
    }
    melee_wii_wifi_tune(config, s_wifi_rateset == 0xFFFF ? -1 : s_wifi_rateset,
                        s_wifi_retry == 0xFF ? -1 : s_wifi_retry);
    so_free(config);
    so_log("socket: %s", melee_wii_wifi_report());
}

static u32 NetUpThread(void* unused)
{
    (void) unused;
    tune_wifi();
    for (;;) {
        s32 res = net_init();
        if (res >= 0) {
            break;
        }
        so_log("socket: the network is not up (%d); trying again", (int) res);
        so_sleep_us(2000000);
    }
    s_ip = net_gethostip();
    s_net_ready = 1;
    so_log("socket: the network is up, %u.%u.%u.%u", (unsigned) (s_ip >> 24),
           (unsigned) (s_ip >> 16) & 0xFF, (unsigned) (s_ip >> 8) & 0xFF, (unsigned) s_ip & 0xFF);
    so_event_signal(&s_io_event);

    /* Keep Wi-Fi roaming off for the whole session: the driver's ~10 s roaming
     * scan spikes latency, and it re-enables roaming on every (re)association,
     * so re-check it here on this (background) thread, never in the netplay
     * loop. If roaming is ever on and cannot be turned off, stop with an error
     * and send the player back to the Homebrew Channel rather than play with it
     * on. A wired link (guard returns -1) has nothing to do. */
    if (melee_wii_wifi_roam_guard == NULL) {
        return 0;
    }
    {
        u32 prev = 0;
        int apply = 1; /* turn roaming off now, then re-verify between matches */
        for (;;) {
            u32 scene = *(volatile u32*) SCENE_WORD;
            u32 mm = ((scene << 8) | (scene >> 24)) & 0xFFFF;
            int left_match = (prev == SCENE_ONLINE_IN_GAME && mm != SCENE_ONLINE_IN_GAME);
            prev = mm;
            if (apply || left_match) {
                int r = melee_wii_wifi_roam_guard();
                if (r < 0) {
                    return 0; /* no Wi-Fi driver (wired): nothing to guard */
                }
                if (r == 0) {
                    so_log("socket: Wi-Fi roaming is on and will not turn off -- stopping");
                    if (melee_wii_fatal != NULL) {
                        melee_wii_fatal("Wi-Fi roaming could not be turned off.\n\n"
                                        "Its 10-second scan spikes netplay latency, so\n"
                                        "Melee is stopping rather than play with it on.\n\n"
                                        "Returning to the Homebrew Channel.");
                    }
                    return 0; /* (melee_wii_fatal does not return) */
                }
                if (apply) {
                    so_log("socket: Wi-Fi roaming off (re-checked at character select)");
                    apply = 0;
                }
            }
            so_sleep_us(SCENE_POLL_US);
        }
    }
}

static s32 open_udp(u16 port)
{
    struct sockaddr_in name;
    s32 s = net_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s < 0) {
        return s;
    }
    if (port != 0) {
        memset(&name, 0, sizeof(name));
        name.sin_family = AF_INET;
        name.sin_len = IOS_SOCKADDR_LEN;
        name.sin_port = htons(port);
        name.sin_addr.s_addr = INADDR_ANY;
        if (net_bind(s, (struct sockaddr*) &name, IOS_SOCKADDR_LEN) < 0) {
            net_close(s);
            return -2;
        }
    }
    return s;
}

static void send_slot(TxSlot* slot)
{
    struct sockaddr_in to;
    s32 s = slot->aux ? s_aux : s_sock;
    s32 n;
    u64 start;
    if (s < 0 && slot->aux) {
        s = s_aux = open_udp(0);
    }
    if (s < 0) {
        bump(&s_stats.send_errors);
        return;
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_len = IOS_SOCKADDR_LEN;
    to.sin_addr.s_addr = htonl(slot->addr);
    to.sin_port = htons(slot->port);
    start = so_time_us();
    n = net_sendto(s, slot->data, slot->len, 0, (struct sockaddr*) &to, IOS_SOCKADDR_LEN);
    so_mutex_lock(&s_stats_lock);
    if (n < 0) {
        s_last_send_error = n;
        s_stats.send_errors++;
    } else {
        u32 us = (u32) (so_time_us() - start);
        if (us >= 5000) {
            so_gamelog_event('T', us);
        }
        s_stats.sent++;
        s_stats.send_us_sum += us;
        if (us > s_stats.send_us_max) {
            s_stats.send_us_max = us;
        }
    }
    so_mutex_unlock(&s_stats_lock);
}

static void receive_one(void)
{
    static u8 buf[SO_DATAGRAM_MAX] __attribute__((aligned(32)));
    struct sockaddr_in from;
    u32 from_len = sizeof(from);
    u64 start = so_time_us();
    s32 n = net_recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*) &from, &from_len);
    if (so_time_us() - start >= 5000) {
        so_gamelog_event('V', (u32) (so_time_us() - start));
    }
    if (n < 0) {
        bump(&s_stats.receive_errors);
        return;
    }
    so_mutex_lock(&s_rx_lock);
    if (s_rx_head - s_rx_tail >= RX_SLOTS) {
        s_stats.receive_dropped++;
    } else {
        RxSlot* slot = &s_rx[s_rx_head % RX_SLOTS];
        slot->addr = ntohl(from.sin_addr.s_addr);
        slot->port = ntohs(from.sin_port);
        slot->len = (u16) n;
        slot->arrived_us = so_time_us();
        memcpy(slot->data, buf, n);
        s_rx_head++;
        s_stats.received++;
    }
    so_mutex_unlock(&s_rx_lock);
    if (s_wake != NULL) {
        so_event_signal(s_wake);
    }
}

static u32 IoThread(void* unused)
{
    (void) unused;
    for (;;) {
        TxSlot* slot = NULL;
        int drained = 0;
        if (!s_net_ready) {
            so_event_wait(&s_io_event, 100000);
            continue;
        }
        /* open, reopen or close the socket, as asked */
        if (s_want_port >= 0 && (s_sock < 0 || s_port != (u16) s_want_port)) {
            if (s_sock >= 0) {
                net_close(s_sock);
                s_sock = -1;
            }
            s_sock = open_udp((u16) s_want_port);
            s_port = (u16) s_want_port;
            if (s_sock < 0) {
                so_log("socket: no socket on port %u (%d)", (unsigned) s_port, (int) s_sock);
                s_want_port = -1;
            }
        } else if (s_want_port < 0 && s_sock >= 0) {
            net_close(s_sock);
            s_sock = -1;
        }
        /* what is queued */
        so_mutex_lock(&s_tx_lock);
        if (s_tx_tail != s_tx_head) {
            slot = &s_tx[s_tx_tail % TX_SLOTS];
        }
        so_mutex_unlock(&s_tx_lock);
        if (slot != NULL) {
            send_slot(slot);
            so_mutex_lock(&s_tx_lock);
            s_tx_tail++;
            drained = s_tx_tail == s_tx_head && s_idle_wanted;
            if (drained) {
                s_idle_wanted = 0;
            }
            so_mutex_unlock(&s_tx_lock);
            if (drained && s_wake != NULL) {
                so_event_signal(s_wake);
            }
            continue;
        }
        /* then what has come */
        if (s_sock >= 0) {
            struct pollsd pfd;
            pfd.socket = s_sock;
            pfd.events = IOS_POLLIN;
            pfd.revents = 0;
            if (net_poll(&pfd, 1, (s32) s_poll_ms) > 0 && (pfd.revents & IOS_POLLIN)) {
                receive_one();
            }
        } else {
            so_event_wait(&s_io_event, 20000);
        }
    }
    return 0;
}

int so_socket_init(void)
{
    static int started;
    if (started) {
        return 0;
    }
    if (so_mutex_init(&s_rx_lock) < 0 || so_mutex_init(&s_tx_lock) < 0 ||
        so_mutex_init(&s_stats_lock) < 0 || so_event_init(&s_io_event) < 0)
    {
        return -1;
    }
    if (so_thread_start(NetUpThread, NULL, NULL, 16 * 1024, 0) < 0 ||
        so_thread_start(IoThread, NULL, NULL, 32 * 1024, 0) < 0)
    {
        return -2;
    }
    started = 1;
    return 0;
}

void so_socket_set_io_mode(int mode, u32 poll_ms)
{
    (void) mode; /* one thread, polling, either way */
    s_poll_ms = poll_ms ? poll_ms : 2;
}

int so_socket_send_idle(void)
{
    int idle;
    so_mutex_lock(&s_tx_lock);
    idle = s_tx_head == s_tx_tail;
    if (!idle) {
        s_idle_wanted = 1;
    }
    so_mutex_unlock(&s_tx_lock);
    return idle;
}

void so_socket_set_wake(SoEvent* wake)
{
    s_wake = wake;
}

int so_socket_open(u16 port)
{
    u32 waited = 0;
    so_socket_flush();
    s_want_port = port;
    so_event_signal(&s_io_event);
    /* the I/O thread opens it: wait a little for it */
    while ((s_sock < 0 || s_port != port) && s_want_port >= 0 && waited < 2000) {
        so_sleep_us(1000);
        waited++;
    }
    return s_sock >= 0 && s_port == port ? 0 : -1;
}

void so_socket_flush(void)
{
    so_mutex_lock(&s_rx_lock);
    s_rx_tail = s_rx_head;
    so_mutex_unlock(&s_rx_lock);
}

void so_socket_close(void)
{
    s_want_port = -1;
    so_event_signal(&s_io_event);
}

int so_socket_is_open(void)
{
    return s_sock >= 0 && s_want_port >= 0;
}

u16 so_socket_port(void)
{
    return s_port;
}

static int queue(int aux, u32 addr, u16 port, const SoSlice* slices, int count)
{
    TxSlot* slot;
    u32 len = 0, depth;
    int i;
    so_mutex_lock(&s_tx_lock);
    depth = s_tx_head - s_tx_tail;
    if (depth >= TX_SLOTS) {
        so_mutex_unlock(&s_tx_lock);
        bump(&s_stats.send_dropped);
        return -2;
    }
    slot = &s_tx[s_tx_head % TX_SLOTS];
    for (i = 0; i < count; i++) {
        if (len + slices[i].len > sizeof(slot->data)) {
            so_mutex_unlock(&s_tx_lock);
            return -1;
        }
        memcpy(slot->data + len, slices[i].data, slices[i].len);
        len += slices[i].len;
    }
    slot->aux = aux;
    slot->addr = addr;
    slot->port = port;
    slot->len = (u16) len;
    s_tx_head++;
    so_mutex_unlock(&s_tx_lock);
    so_mutex_lock(&s_stats_lock);
    if (depth + 1 > s_stats.send_queue_max) {
        s_stats.send_queue_max = depth + 1;
    }
    so_mutex_unlock(&s_stats_lock);
    so_event_signal(&s_io_event);
    return (int) len;
}

int so_socket_send(u32 addr, u16 port, const void* data, u32 len)
{
    SoSlice slice;
    slice.data = data;
    slice.len = len;
    return so_socket_sendv(addr, port, &slice, 1);
}

int so_socket_sendv(u32 addr, u16 port, const SoSlice* slices, int count)
{
    if (!so_socket_is_open()) {
        return -1;
    }
    return queue(0, addr, port, slices, count);
}

int so_socket_send_aux(u32 addr, u16 port, const void* data, u32 len)
{
    SoSlice slice;
    slice.data = data;
    slice.len = len;
    return queue(1, addr, port, &slice, 1);
}

static u32 s_test_rx_delay_us;
static u32 s_test_rx_stall_us;

void so_socket_set_test_rx_stall_us(u32 us)
{
    s_test_rx_stall_us = us;
}

void so_socket_set_test_rx_delay_us(u32 us)
{
    s_test_rx_delay_us = us;
    if (us != 0) {
        so_log("socket: TEST: what comes is held %u ms", (unsigned) (us / 1000));
    }
}

/* Is there a datagram to take (the test delay passed)? The rx lock held */
static int deliverable_locked(void)
{
    if (s_rx_tail == s_rx_head) {
        return 0;
    }
    if (s_test_rx_stall_us != 0 && so_time_us() % 3000000 < s_test_rx_stall_us) {
        return 0; /* (the test's lag spike: held, then all at once) */
    }
    return s_test_rx_delay_us == 0 || so_time_us() - s_rx[s_rx_tail % RX_SLOTS].arrived_us >= s_test_rx_delay_us;
}

int so_socket_receive(u32* addr, u16* port, void* buf, u32 size, u64* arrived_us)
{
    int n = 0;
    so_mutex_lock(&s_rx_lock);
    if (deliverable_locked()) {
        RxSlot* slot = &s_rx[s_rx_tail % RX_SLOTS];
        n = slot->len < size ? slot->len : (int) size;
        memcpy(buf, slot->data, n);
        *addr = slot->addr;
        *port = slot->port;
        if (arrived_us != NULL) {
            *arrived_us = slot->arrived_us;
        }
        s_rx_tail++;
    }
    so_mutex_unlock(&s_rx_lock);
    return n;
}

int so_socket_pending(void)
{
    int pending;
    so_mutex_lock(&s_rx_lock);
    pending = deliverable_locked();
    so_mutex_unlock(&s_rx_lock);
    return pending;
}

int so_socket_wait(u32 timeout_us)
{
    u64 deadline = so_time_us() + timeout_us;
    while (!so_socket_pending() && s_wake != NULL && so_time_us() < deadline) {
        u32 left = (u32) (deadline - so_time_us());
        /* (tests: a datagram held back comes due without a wake) */
        if (s_test_rx_delay_us != 0 && left > 2000) {
            left = 2000;
        }
        so_event_wait(s_wake, left);
    }
    return so_socket_pending();
}

int so_resolve(const char* host, u32* addr)
{
    struct hostent* he;
    if (so_parse_ipv4(host, addr) == 0) {
        return 0;
    }
    if (!s_net_ready) {
        return -1;
    }
    he = net_gethostbyname(host);
    if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL) {
        return -2;
    }
    *addr = ntohl(*(u32*) he->h_addr_list[0]);
    return 0;
}

/* Waits for the network (so_console.c's thread) */
int so_net_wait_ready(void)
{
    while (!s_net_ready) {
        so_sleep_us(100000);
    }
    return 0;
}

u32 so_local_ip(void)
{
    return s_net_ready ? s_ip : 0;
}

void so_socket_stats(SoSocketStats* out)
{
    so_mutex_lock(&s_stats_lock);
    *out = s_stats;
    so_mutex_unlock(&s_stats_lock);
}

void so_socket_stats_peek(SoSocketStats* out)
{
    *out = s_stats;
}

int so_socket_experiment_async_send(u32 addr, u16 port, const void* data, u32 len, int reply)
{
    (void) reply;
    return so_socket_send(addr, port, data, len);
}
