/* slippi_online/so_mm.h
 * Matchmaking client: Slippi Dolphin's SlippiMatchmaking (Ishiiruka v3.7.0)
 * as a non-blocking state machine.
 *
 * First STUN, on the session's UDP socket (the port netplay will play on):
 * binding requests to public STUN servers give this console's address and
 * port as the internet sees them, and from two servers whether the NAT
 * keeps one port ("cone") or not ("symmetric"). Then the matchmaking
 * service session (so_mmws.c, a WebSocket on a thread of its own) joins the
 * queue with them; while queued a STUN keepalive every 15 s holds the NAT's
 * mapping open. The match message gives the players and their addresses.
 *
 * Driven from the network thread: so_mm_poll() never blocks. Not
 * thread-safe; the client serialises access.
 */
#ifndef SO_MM_H
#define SO_MM_H

#include "global.h"
#include "so_config.h"

/* Dolphin's OnlinePlayMode. */
enum {
	SO_MODE_RANKED = 0,
	SO_MODE_UNRANKED = 1,
	SO_MODE_DIRECT = 2,
	SO_MODE_TEAMS = 3,
	SO_MODE_PARTY = 4,
};

#define SO_MAX_PLAYERS 4
#define SO_CONNECT_CODE_MAX 24	/* game bytes: 8 full-width Shift-JIS chars + slack */
#define SO_CHAT_MESSAGES 16
#define SO_CHAT_TEXT_MAX 32
#define SO_MAX_STAGES 32

typedef struct {
	u8 mode;
	/* The connect code exactly as the game hands it over (full-width
	 * Shift-JIS from the name entry screen); sent as a byte array. */
	u8 connect_code[SO_CONNECT_CODE_MAX];
	u8 connect_code_len;
} SoSearch;

typedef struct {
	char uid[64];
	char display_name[64];
	char connect_code[16];
	u8 port;	/* 1-4 */
	u8 is_local;
	u8 is_bot;
	u8 has_chat_messages;	/* 0: use the defaults */
	char chat_messages[SO_CHAT_MESSAGES][SO_CHAT_TEXT_MAX];
	u32 addr;	/* remote players: where to connect (LAN or external) */
	u16 udp_port;
} SoMmPlayer;

typedef struct {
	char match_id[96];
	u8 player_count;
	SoMmPlayer players[SO_MAX_PLAYERS];	/* in the server's order (by port) */
	u8 local_player_index;	/* port - 1 */
	u8 is_host;	/* the "decider" */
	u8 stage_count;
	u16 stages[SO_MAX_STAGES];
	u32 items;
} SoMatchResult;

typedef enum {
	SO_MM_IDLE,
	SO_MM_STUN,	/* asking the STUN servers */
	SO_MM_CONNECTING,	/* the service session is connecting */
	SO_MM_SEARCHING,	/* in the queue */
	SO_MM_MATCHED,	/* result ready; the socket is free for netplay */
	SO_MM_ERROR,
} SoMmState;

#define SO_STUN_SERVERS 4

/* Where to reach the service and the STUN servers (so_client.c resolves
 * them, with its lock dropped: DNS blocks). */
typedef struct {
	u32 mm_addr;
	u32 stun_addr[SO_STUN_SERVERS];
	u16 stun_port[SO_STUN_SERVERS];
	int stun_count;
} SoMmServers;

/* Starts a search on the open session socket bound to `port`. Returns 0,
 * or < 0 (with an error message). */
int so_mm_start(const SoConfig *config, const SoUser *user, const SoSearch *search, const SoMmServers *servers,
	u16 port);
void so_mm_poll(void);
/* Leaves the queue. */
void so_mm_stop(void);

SoMmState so_mm_state(void);
/* The error text shown to the player (the service's own text when it sent
 * one), valid in SO_MM_ERROR. */
const char *so_mm_error(void);
/* NULL before the first search. */
const SoMatchResult *so_mm_result(void);
u16 so_mm_port(void);

/* ---- messages (so_mm_msg.c; no networking, unit-tested) ------------------ */

/* What the STUN servers said */
typedef struct {
	int answered[2];
	u32 addr[2];
	u16 port[2];
} SoStunResult;

/* Writes the join message. `lan_address` is "a.b.c.d:port" or "".
 * Returns the length, or -1 when it does not fit. */
int so_mm_build_join(char *out, int size, const SoConfig *config, const SoUser *user, const SoSearch *search,
	u16 local_port, const char *lan_address, const SoStunResult *stun);

#define SO_STUN_REQUEST_LEN 20
void so_stun_request(u8 out[SO_STUN_REQUEST_LEN], const u8 txid[12]);
/* Reads a binding success response for `txid`: the mapped address (network
 * byte order) and port. Returns 0, or < 0 when it is not one. */
int so_stun_parse(const u8 *msg, u32 len, const u8 txid[12], u32 *addr, u16 *port);

/* Parses the match message into `result`. Returns 0, or < 0 with `error`
 * set. */
int so_mm_parse_match(const char *text, u32 len, SoMatchResult *result, char *error, int error_size);

#endif
