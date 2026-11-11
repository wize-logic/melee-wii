/* slippi_online/so_config.h
 * Settings and the Slippi account, read once at start-up from the boot
 * drive:
 *
 *   /slippi/user.json    Slippi Launcher's user.json, copied as is
 *   /slippi/config.ini   optional key=value settings (README.md)
 *
 * The play key is a credential: it is only ever sent inside a create-ticket
 * to the configured matchmaking server, and never logged.
 */
#ifndef SO_CONFIG_H
#define SO_CONFIG_H

#include "global.h"

#define SO_USER_PATH   "/slippi/user.json"
#define SO_TEST_USER_PATH "/slippi/test-user.json"	/* the dummy test account */
#define SO_CONFIG_PATH "/slippi/config.ini"

/* The netplay version Slippi Dolphin (Ishiiruka) reports; the server only
 * pairs compatible versions. */
#define SO_APP_VERSION_DEFAULT "3.6.4"
#define SO_MM_HOST_DEFAULT "mm.slippi.gg"
#define SO_MM_PORT_DEFAULT 43113

typedef struct {
	char uid[64];
	char play_key[96];
	char connect_code[16];	/* "ABCD#123" */
	char display_name[64];	/* UTF-8 */
	char latest_version[24];
} SoUser;

typedef struct {
	char user_path[64];	/* the account file (tests point it at a dummy) */
	char mm_host[64];
	u16 mm_port;
	u16 local_port;	/* 0: random in 41000-50999, as Dolphin picks */
	char app_version[24];
	/* The online input delay in frames for the next game: the override, or
	 * what goes with the connection and the opponent's ping
	 * (so_config_auto_delay; so_client_match_delay keeps it) */
	u8 delay;
	u8 delay_override;	/* input_delay=1-9 (the launcher's setting); 0, auto: none */
	/* delay_adjust=1 (config.ini only, no launcher setting; off unless set): the automatic
	 * delay also follows the ping during a game, a frame at a time
	 * (so_exi.c: each change repeats or drops one input) */
	u8 delay_adjust;
	u32 lan_ip;	/* forced ipAddressLan; 0: this console's address */
	u32 log_host;	/* send log lines to this PC over UDP; 0: off */
	u16 log_port;
	u8 log_packets;	/* log every netplay packet (debugging) */
	/* Seconds after start-up with no traffic unless the game asks for a
	 * match (traffic during Melee's boot crashed it). */
	u8 boot_quiet_s;
	u8 status_beacon;	/* show the client state as the console nickname in
				 * Slippi's LAN broadcast (debugging) */
	/* 0: flush each pad ack at once, as Dolphin does. Otherwise hold it up
	 * to this long so it shares a datagram with our next pad (IOS sends are
	 * slow; see so_socket.h). SO_AUTO_U16 until so_config_apply_link. */
	u16 ack_delay_us;

	/* The network profile (so_config_apply_link). Settings left at "auto"
	 * follow the connection: the wired profile is the one tested for hours
	 * on the USB LAN adapter; the Wi-Fi profile spends fewer datagrams,
	 * since Wi-Fi's IOS driver sends far fewer per second (FEASIBILITY.md). */
	u8 connection;	/* SO_CONNECTION_*: auto (the Wii's settings), wired or wifi */
	u8 wifi;	/* resolved: 1 for the Wi-Fi profile */
	u8 net_io;	/* SO_NET_IO_*: socket threads, or one thread polling (so_socket.h) */
	/* 1: a pad packet leaves only once IOS has sent the previous datagram
	 * (each packet carries every pad not yet acked, so a held one loses
	 * nothing); 0: every frame, as Dolphin. SO_AUTO until resolved. */
	u8 pad_pacing;
	u8 poll_ms;	/* SO_NET_IO_POLL: longest wait in one IOS poll */
	/* On Wi-Fi, for this session (wii/src/wifi.c): the transmission rates
	 * (wifi_rate=wii, 54 or 1: SO_AUTO_U16, 0x0800 or 0x0001) and send
	 * retries (wifi_retries=wii or 1-15: SO_AUTO), as Wii Settings saved
	 * them unless set (the launcher's settings) */
	u16 wifi_rateset;
	u8 wifi_retry;
	/* Self-test (tests/README): Direct search and a synthetic match,
	 * started without the game. */
	u8 selftest;
	char selftest_code[16];
	u16 selftest_frames;
	u8 selftest_allow_real_mm;
	/* After the self-test: replay the asynchronous IOS sends that froze
	 * diagnostics builds 1-4 (without, then with the completion reply), to
	 * confirm the cause on hardware. */
	u8 selftest_async_probe;
	u8 selftest_stage_pause_s;	/* between the probe's stages (default 3) */
	u16 diag_step_pause_ms;	/* hold each announced start-up step (SO_STEP) */
	u8 selftest_memory_walk;	/* write and verify the free heap first */
	/* Experiment: a second probe round with the kernel's main thread at
	 * this priority instead of 0x50 (0: skip). */
	u8 diag_main_priority;
	/* The kernel main thread's priority while online (0: leave it at 0x50).
	 * At 0x50 its polling loop starves IOS's network module: round trips
	 * took 39 ms and sends up to 11 ms; at 0x30, 2.8 ms and 0.9 ms. */
	u8 online_main_priority;
	/* The main loop's sleep between passes while online, in microseconds
	 * (20 otherwise). With the loop above the network threads, Melee's EXI
	 * requests wait at most about this long, and the network threads and
	 * IOS have the CPU in between. */
	u16 online_loop_us;
	u8 chat;	/* quick chat in Direct (Slippi's default: on) */
	u8 rumble;	/* controller rumble in online play (default on; rumble.txt, set on the
		 * online CSS, wins over config.ini) */
	u8 test_drop_pct;	/* tests: drop this share of outgoing pad packets */
	u16 test_rx_delay_ms;	/* tests: hold each datagram received this long (latency) */
	u16 test_rx_stall_ms;	/* tests: every 3 s, hold what comes this long, then all at once (a lag spike) */
	u16 test_stage;	/* tests: the stage this side picks (its id; 0: as the game picks) */
	u16 test_slow_save_us;	/* tests: each savestate takes this much longer (a console's times in Dolphin) */
	u16 test_slow_load_us;	/* tests: each load takes this much longer */
} SoConfig;

#define SO_AUTO 0xFF
#define SO_AUTO_U16 0xFFFF

/* The same values as SoLink (so_link.h). */
enum { SO_CONNECTION_AUTO = 0, SO_CONNECTION_WIRED = 1, SO_CONNECTION_WIFI = 2 };
enum { SO_NET_IO_AUTO = 0, SO_NET_IO_THREADS = 1, SO_NET_IO_POLL = 2 };

/* Wi-Fi profile values */
#define SO_WIFI_ACK_DELAY_US 16000	/* about a frame: acks ride with our next pad */

void so_config_defaults(SoConfig *config);
/* Resolves the settings left at auto, for `detected` (a SoLink: what the
 * Wii's network settings say) unless `connection` names one. An unknown
 * connection gets the wired profile. */
void so_config_apply_link(SoConfig *config, int detected);

/* The input delay that goes with a connection and a round trip to the
 * opponent: on Wi-Fi 3 frames up to 50 ms, 4 up to 100 and 5 over; wired,
 * one fewer each. */
u8 so_config_auto_delay(int wifi, u32 ping_ms);
/* Applies config.ini text; unknown keys and bad values are logged and
 * skipped. Comment lines start with '#' or ';' (connect codes contain '#',
 * so only whole lines are comments). */
void so_config_parse(SoConfig *config, const char *text, u32 len);

/* Parses user.json; returns 0 when uid, playKey and connectCode are present. */
int so_user_parse(SoUser *user, const char *text, u32 len);

/* Reads both files from the boot drive. Returns 0 when user.json is usable;
 * the settings fall back to defaults either way. */
int so_config_load(SoConfig *config, SoUser *user);

/* True when the matchmaking host is Slippi's real server. */
int so_config_is_real_mm(const SoConfig *config);

#endif
