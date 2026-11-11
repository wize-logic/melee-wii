/* slippi_online/so_exi.c
 * Slippi Dolphin's online EXI device (see so_exi.h). Function names in
 * comments refer to CEXISlippi in Slippi Dolphin's EXI_DeviceSlippi.cpp,
 * which this follows; byte layouts are in docs/EXI_DEVICE_SPEC.md (repository root).
 */
#define SO_FILE_ID "exi"
#include "so_exi.h"
#include "so_gamelog.h"
#include "so_rollback.h"
#include "so_alloc.h"
#include "so_client.h"
#include "so_files.h"
#include "so_netplay.h"
#include "so_platform.h"
#include "so_selftest.h"	/* so_encode_connect_code */
#include "so_text.h"
#include "so_util.h"
#include "string.h"

enum {
	CMD_ONLINE_INPUTS = 0xB0,
	CMD_CAPTURE_SAVESTATE = 0xB1,
	CMD_LOAD_SAVESTATE = 0xB2,
	CMD_GET_MATCH_STATE = 0xB3,
	CMD_FIND_OPPONENT = 0xB4,
	CMD_SET_MATCH_SELECTIONS = 0xB5,
	CMD_OPEN_LOGIN = 0xB6,
	CMD_LOGOUT = 0xB7,
	CMD_UPDATE = 0xB8,
	CMD_GET_ONLINE_STATUS = 0xB9,
	CMD_CLEANUP_CONNECTION = 0xBA,
	CMD_SEND_CHAT_MESSAGE = 0xBB,
	CMD_GET_NEW_SEED = 0xBC,
	CMD_REPORT_GAME = 0xBD,
	CMD_FETCH_CODE_SUGGESTION = 0xBE,
	CMD_OVERWRITE_SELECTIONS = 0xBF,
	CMD_GP_COMPLETE_STEP = 0xC0,
	CMD_GP_FETCH_STEP = 0xC1,
	CMD_REPORT_SET_COMPLETE = 0xC2,
	CMD_GET_PLAYER_SETTINGS = 0xC3,
	CMD_REPORT_MATCH_STATUS_UPDATE = 0xC4,
	CMD_LOG_MESSAGE = 0xD0,
	CMD_FILE_LENGTH = 0xD1,
	CMD_FILE_LOAD = 0xD2,
	CMD_GCT_LENGTH = 0xD3,
	CMD_GCT_LOAD = 0xD4,
	CMD_GET_DELAY = 0xD5,
	CMD_PLAY_MUSIC = 0xD6,
	CMD_STOP_MUSIC = 0xD7,
	CMD_CHANGE_MUSIC_VOLUME = 0xD8,
	CMD_PREMADE_TEXT_LENGTH = 0xE1,
	CMD_PREMADE_TEXT_LOAD = 0xE2,
	CMD_GET_RANK = 0xE3,
	CMD_FETCH_RANK = 0xE4,
	CMD_GET_RANK_VISIBILITY = 0xE5,
	/* Ours: Melee's crash report (tools/ppc/crash_report.s). */
	CMD_CRASH_REPORT = 0xDF,
	CMD_HEARTBEAT = 0xDE,	/* diagnostics (tools/ppc/heartbeat.s) */
	CMD_PROBE = 0xDD,	/* diagnostics (tools/ppc/probe_*.s) */
	CMD_TEXT_REPORT = 0xDA,	/* Melee's panics and OSReport (tools/ppc/text_report.s) */
	CMD_SET_DELAY = 0xDC,	/* Wii only: [step] -> [delay], the delay buttons (hidden: so_wii.c) */
	CMD_SET_RUMBLE = 0xDB,	/* Wii only: [0 off, 1 on, 0xFF read] -> [rumble], the online CSS's rumble switch */
};

/* ONLINE_INPUTS results (the ASM's RESP_*). */
enum { RESP_NORMAL = 1, RESP_SKIP = 2, RESP_DISCONNECTED = 3 };

/* SlippiMatchmaking::ProcessState, as the game reads it. */
enum { MM_IDLE = 0, MM_INITIALIZING = 1, MM_MATCHMAKING = 2, MM_OPPONENT_CONNECTING = 3, MM_CONNECTION_SUCCESS = 4,
	MM_ERROR = 5 };

#define RXB_SIZE 297
#define MSRB_SIZE 962
#define OSB_SIZE 42
#define PLAYER_SETTINGS_SIZE 3264
#define ROLLBACK_MAX_FRAMES 7
#define PAD_FULL_SIZE 12
#define MATCH_BLOCK_SIZE 0x138
#define STALL_LIMIT_US 7000000	/* Dolphin gives up after 420 stalled frames */

/* Payload sizes (bytes after the command byte): Dolphin's payload_sizes,
 * EXI_DeviceSlippi.h. 0 marks a command so_exi does not handle. */
static u16 payload_size(u8 cmd)
{
	switch (cmd) {
	case CMD_ONLINE_INPUTS: return 25;
	case CMD_CAPTURE_SAVESTATE:
	case CMD_LOAD_SAVESTATE: return 32;
	case CMD_FIND_OPPONENT: return 19;
	case CMD_SET_MATCH_SELECTIONS: return 9;
	case CMD_SEND_CHAT_MESSAGE: return 2;
	case CMD_REPORT_GAME: return 368;
	case CMD_FETCH_CODE_SUGGESTION: return 31;
	case CMD_OVERWRITE_SELECTIONS: return 14;
	case CMD_GP_COMPLETE_STEP: return 5;
	case CMD_GP_FETCH_STEP:
	case CMD_REPORT_SET_COMPLETE:
	case CMD_REPORT_MATCH_STATUS_UPDATE:
	case CMD_CHANGE_MUSIC_VOLUME: return 1;
	case CMD_LOG_MESSAGE: return 127;	/* the logf buffer: 128 bytes */
	case CMD_FILE_LENGTH:
	case CMD_FILE_LOAD: return 0x40;
	case CMD_GCT_LOAD: return 4;
	case CMD_PLAY_MUSIC: return 8;
	case CMD_PREMADE_TEXT_LENGTH:
	case CMD_PREMADE_TEXT_LOAD: return 2;
	case CMD_CRASH_REPORT:
	case CMD_PROBE: return 0x3F;
	case CMD_HEARTBEAT: return 0x5F;
	case CMD_SET_DELAY: return 1;
	case CMD_SET_RUMBLE: return 1;
	case CMD_TEXT_REPORT: return 0x7F;
	default: return 0;
	}
}

int so_exi_online_mode(void)
{
	return so_client_online_game();
}

int so_exi_is_online_command(u8 cmd)
{
	return (cmd >= CMD_ONLINE_INPUTS && cmd <= CMD_REPORT_MATCH_STATUS_UPDATE) ||
		(cmd >= CMD_LOG_MESSAGE && cmd <= CMD_CHANGE_MUSIC_VOLUME) ||
		(cmd >= CMD_PREMADE_TEXT_LENGTH && cmd <= CMD_GET_RANK_VISIBILITY) || cmd == CMD_CRASH_REPORT || cmd == CMD_HEARTBEAT ||
		cmd == CMD_PROBE || cmd == CMD_TEXT_REPORT || cmd == CMD_SET_DELAY ||
		cmd == CMD_SET_RUMBLE;
}

u32 so_exi_request_size(u8 cmd)
{
	return 1 + payload_size(cmd);
}

/* Dolphin's starting match block: a VS match, P1 red Falco vs P2 red Bowser
 * (P3/P4 Young Link, not playing) on Battlefield. It is patched, never rebuilt:
 * the game reads the last stage played from it after a game. */
static const u8 default_match_block[MATCH_BLOCK_SIZE] = {
	0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0xFF, 0xFF, 0x6E, 0x00,
	0x1F, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F,
	0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09,
	0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04,
	0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F,
	0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00,
	0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x15, 0x03, 0x04, 0x00, 0x00, 0xFF,
	0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
	0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00, 0x40, 0x00, 0x04,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09,
	0x00, 0x78, 0x00, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
};

/* Fallback stages: FoD, Pokemon Stadium, Yoshi's Story, Dream Land,
 * Battlefield, Final Destination. */
static const u16 default_stages[] = { 0x2, 0x3, 0x8, 0x1C, 0x1F, 0x20 };

#define MAX_STAGES 32

static struct {
	/* the answer for the next read (Dolphin's m_read_queue): small ones in
	 * `small`, the rest in `big`, from the online heap (none without the
	 * online client) */
	u8 small[48];
	u8 *big;
	const u8 *resp;
	u32 resp_len;
	int has_resp;
	/* 0xD2: the answer is this file, read from the drive by the read */
	int file_pending;
	char file[0x41];

	SoSelections local_sel;	/* local_selections */
	u8 mode;	/* last_search.mode */
	u8 match_block[MATCH_BLOCK_SIZE];
	u8 local_idx, remote_idx;	/* m_local_player_idx / m_remote_player_idx */
	int have_client;	/* Dolphin holds slippi_netplay */
	u16 allowed[MAX_STAGES];
	int allowed_count;
	u16 pool[MAX_STAGES];
	int pool_count;
	u8 sent_chat;	/* remote_sent_chat_message_id */

	s32 last_frame;	/* of the previous ONLINE_INPUTS */
	int dummies_sent;	/* the delay frames' empty pads, this game */
	int no_savestates;	/* this game started without its savestates */
	u64 stall_since_us;
	/* This game's netcode, logged when it ends: frames played, polls
	 * answered SKIP (each one a whole frame the game waited), frames that
	 * waited at all, the longest wait for one frame, and when it started. */
	/* What the last 0xB0 sends once the game has read its answer. */
	struct {
		int valid, has_pad, delay_pads, has_extra;
		u8 delay;
		SoLocalPad pad;
		SoLocalPad extra;	/* before `pad`: the frame a longer delay leaves without one */
	} pend;
	int answer_read;	/* the game read the last answer */

	struct {
		u32 frames, skips, held, longest, run;
		u64 start_us;
		u32 max_send_us, replay_writes;
		/* The first frames that waited for the opponent: frame, wait in ms. */
		u32 wait_frame[8], wait_ms[8];
		u32 waits_logged;
		u64 wait_since_us;
		/* The game's own frame pacing: the time between 0xB0 polls (one
		 * per frame: over 20 ms means it missed a refresh), and how long
		 * the device took to answer (with the client lock's wait). */
		u64 last_poll_us;
		u32 slow_polls, max_gap_us;
		u32 max_answer_us, slow_answers, max_lock_us;
		s32 heartbeat_frame;	/* the last frame so_rollback_heartbeat logged */
	} game;
	int initialised;

	SoExiStats stats;
} x;

/* wii/src/overlay.c (the Wii build with the game's own code) */
void melee_wii_ping_text(int ms) __attribute__((weak));
/* wii/src/mod_fighters.c (20XX's costumes are this console's player's only):
 * that player's character and costume in the match made, or -1 while none is */
void melee_wii_costumes_match(int character, int costume) __attribute__((weak));

static void init_once(void)
{
	if (x.initialised)
		return;
	memcpy(x.match_block, default_match_block, sizeof(x.match_block));
	memcpy(x.allowed, default_stages, sizeof(default_stages));
	x.allowed_count = sizeof(default_stages) / sizeof(default_stages[0]);
	x.remote_idx = 1;
	x.initialised = 1;
}

/* A zeroed answer of `len` bytes, or NULL (and no answer) when a big one
 * has no buffer. */
static u8 *answer(u32 len)
{
	u8 *b = x.small;
	x.file_pending = 0;
	if (len > sizeof(x.small)) {
		if (!x.big)
			x.big = (u8 *)so_malloc(PLAYER_SETTINGS_SIZE);
		b = len <= PLAYER_SETTINGS_SIZE ? x.big : NULL;
	}
	x.has_resp = b != NULL;
	if (!b)
		return NULL;
	memset(b, 0, len);
	x.resp = b;
	x.resp_len = len;
	return b;
}

/* SlippiPlayerSelections::Reset (keeps the alt stage mode). */
static void reset_selections(SoSelections *s)
{
	u8 alt = s->alt_stage_mode;
	memset(s, 0, sizeof(*s));
	s->alt_stage_mode = alt;
}

/* SlippiPlayerSelections::Merge */
static void merge_selections(SoSelections *into, const SoSelections *s)
{
	into->rng_offset = s->rng_offset;
	if (s->is_stage_selected) {
		into->stage_id = s->stage_id;
		into->is_stage_selected = 1;
		into->alt_stage_mode = s->alt_stage_mode;
	}
	if (s->is_character_selected) {
		into->character_id = s->character_id;
		into->character_color = s->character_color;
		into->team_id = s->team_id;
		into->is_character_selected = 1;
	}
}

/* getRandomStage: draws from the pool without replacement. */
static u16 random_stage(void)
{
	int i;
	u16 stage;
	if (x.pool_count == 0) {
		for (i = 0; i < x.allowed_count; i++)
			x.pool[i] = x.allowed[i];
		x.pool_count = x.allowed_count;
	}
	if (x.pool_count == 0)
		return 0x1F;
	i = (int)(so_random() % (u32)x.pool_count);
	stage = x.pool[i];
	x.pool[i] = x.pool[--x.pool_count];
	return stage;
}

/* The match players are known once matchmaking has paired us. */
static const SoMatchResult *current_match(void)
{
	SoClientState st = so_client_state();
	if (st == SO_CLIENT_CONNECTING || st == SO_CLIENT_CONNECTED || st == SO_CLIENT_DISCONNECTED)
		return so_client_match();
	return NULL;
}

/* False without an account or a network: the client (and its lock) may
 * not exist, the game's online modes stay locked, and nothing here touches
 * the client. */
static int client_up(void)
{
	return so_client_state() != SO_CLIENT_OFF;
}

/* handleConnectionCleanup */
static void cleanup(void)
{
	if (client_up())
		so_client_cancel();
	reset_selections(&x.local_sel);
	x.pool_count = 0;
	x.have_client = 0;
	x.sent_chat = 0;
	x.last_frame = 0;	/* the next frame 1 starts a game */
}

/* ---- the input delay changed during a game ---------------------------------
 *
 * The delay_adjust setting (config.ini's delay_adjust=1: off unless set, and
 * not in the launcher, kept for later; only with the automatic delay).
 * Slippi fixes the delay for a game: its game code (TriggerSendInput, at
 * HSD_PadRenewRawStatus) keeps the last `delay` pads in a ring in its data
 * (the ODB), each frame playing the ring's oldest and putting the pad just
 * read in its place, and tells the device the delay with each pad, which the
 * device sends the opponent as frame + delay's. To change it by a frame, the
 * ring, the delay and what the opponent is sent change together, in the
 * answer to one pad (frame F, delay D; P(n) the pad read at frame n; the
 * frames F..F+D-1 already have P(F-D)..P(F-1), sent and not to be changed):
 *
 * - longer: frame F+D has no pad (P(F) is now frame F+D+1's). It gets
 *   P(F-1) again: the ring becomes P(F-D)..P(F-1), P(F-1), and the opponent
 *   is sent P(F-1) as frame F+D's before P(F).
 * - shorter: P(F) has no frame (frame F+D-1 has P(F-1)). It is dropped: the
 *   ring becomes P(F-D)..P(F-2), the pad the game is about to put in it (its
 *   copy of what it sent, P(F)) is made P(F-1), and the opponent is sent
 *   nothing for this frame.
 *
 * So each change repeats or drops one frame of the player's input, and
 * moves this side's pads a frame against the opponent's: Slippi's time sync
 * then has one side wait a frame (this one after a longer delay, the
 * opponent after a shorter). Hence few changes: the ping is the median of
 * the last 3 s of a game's readings, a change needs it 5 ms past the
 * delay's limit (so_config_auto_delay: 50 and 100 ms) for 3 s, and none
 * comes within 10 s of the game's start or of the last.
 *
 * The offsets are the codeset's (netplay.gct, as Slippi's
 * Online.s: ODB_FRAME 3, ODB_DELAY_FRAMES 33, ODB_DELAY_BUFFER_INDEX 34,
 * ODB_DELAY_BUFFER 35, ODB_TXB_ADDR 215; a pad report 12 bytes, the pad at
 * 14 in what the game sends). Everything is checked against the pad being
 * answered before a byte is written; anything not as expected turns the
 * setting off for the session, with a line in the log. */
#define ODB_POINTER 0x804D6CBCu	/* (r13 - 18916: the game's pointer to its ODB) */
#define ODB_FRAME 3
#define ODB_DELAY 33
#define ODB_DELAY_INDEX 34
#define ODB_DELAY_RING 35
#define ODB_TXB_ADDR 215
#define ODB_ROLLBACK_ACTIVE 223
#define GAME_PAD_SIZE 12
#define GAME_DELAY_MAX 15
#define TXB_PAD 14
#define ADJ_PINGS 180	/* 3 s of played frames */
#define ADJ_EVERY 30	/* frames between looks at the ping */
#define ADJ_RUNS 6	/* looks in a row that want the same change: 3 s */
#define ADJ_QUIET 600	/* frames from the game's start, and between changes */
#define ADJ_MARGIN_MS 5

static struct {
	u8 ping_ms[ADJ_PINGS];
	u32 pings;
	s8 want;	/* the change the last looks wanted: -1, 0, +1 */
	u8 runs;
	s32 last_change;	/* frame */
	s32 last_play;	/* the last frame answered "play" */
	SoLocalPad last_pad;	/* and the newest pad given to the network */
	int have_last;
	u32 changes;
	u32 median_ms;
	int off;	/* the game's data was not as expected: no more this session */
} s_adj;

static int game_ptr(u32 a, u32 len)
{
	return a >= 0x80000000u && a + len <= 0x81800000u;
}

/* The median of the game's last pings (ms, each at most 255) */
static u32 adj_median(void)
{
	u16 counts[256];
	u32 n = s_adj.pings < ADJ_PINGS ? s_adj.pings : ADJ_PINGS, i, seen = 0;
	memset(counts, 0, sizeof(counts));
	for (i = 0; i < n; i++)
		counts[s_adj.ping_ms[i]]++;
	for (i = 0; i < 256; i++) {
		seen += counts[i];
		if (seen * 2 > n)
			break;
	}
	return i;
}

/* A played frame's ping noted; +1 or -1 when the delay should change now */
static int adj_wanted(s32 frame, u8 delay)
{
	const SoConfig *cfg = so_client_config();
	u32 ping = so_np_max_ping_us() / 1000;
	int base, tier, want = 0;
	if (ping != 0) {
		s_adj.ping_ms[s_adj.pings % ADJ_PINGS] = (u8)(ping > 255 ? 255 : ping);
		s_adj.pings++;
	}
	if (cfg == NULL || !cfg->delay_adjust || cfg->delay_override || s_adj.off || !so_rollback_active())
		return 0;
	if (frame % ADJ_EVERY != 0 || s_adj.pings < ADJ_PINGS)
		return 0;
	s_adj.median_ms = adj_median();
	base = so_config_auto_delay(cfg->wifi, 0);	/* the least: the delay up to 50 ms */
	tier = (int)delay - base;
	if (tier < 2 && s_adj.median_ms > (u32)(tier <= 0 ? 50 : 100) + ADJ_MARGIN_MS)
		want = 1;
	else if (tier > 0 && s_adj.median_ms + ADJ_MARGIN_MS < (u32)(tier == 1 ? 50 : 100))
		want = -1;
	if (want == 0 || want != s_adj.want) {
		s_adj.want = (s8)want;
		s_adj.runs = want != 0;
		return 0;
	}
	if (++s_adj.runs < ADJ_RUNS || frame < ADJ_QUIET || frame - s_adj.last_change < ADJ_QUIET)
		return 0;
	return want;
}

static void adj_fail(const char *what, s32 frame)
{
	s_adj.off = 1;
	so_log("game: the delay is not changed during games this session: %s (frame %d)", what, (int)frame);
}

/* Changes the game's delay by `step` as it plays `frame` (the answer to its
 * pad is "play"; the client lock held). Returns 1 when done; 0 with nothing
 * written. */
static int adj_apply(s32 frame, u8 delay, int step)
{
	u8 old[GAME_DELAY_MAX][GAME_PAD_SIZE];
	u32 odb = so_game_word(ODB_POINTER), txb;
	volatile u8 *o, *t;
	int d = delay, n = d + step, i, k;
	if (n < 1 || n > 9)
		return 0;
	if (!s_adj.have_last || s_adj.last_play != frame - 1 || s_adj.last_pad.frame != frame + d - 1)
		return 0;	/* (a frame asked again, or waited for: another time) */
	if (!game_ptr(odb, 256)) {
		adj_fail("no game data", frame);
		return 0;
	}
	o = (volatile u8 *)odb;
	if (o[ODB_ROLLBACK_ACTIVE] != 0)
		return 0;	/* (a pad asked for with a rollback to come: another time) */
	txb = so_game_word(odb + ODB_TXB_ADDR);
	if (o[ODB_DELAY] != d || o[ODB_DELAY_INDEX] >= d || so_game_word(odb + ODB_FRAME) != (u32)frame ||
			!game_ptr(txb, 32)) {
		adj_fail("the game's data is not laid out as expected", frame);
		return 0;
	}
	t = (volatile u8 *)txb;
	if (t[0] != CMD_ONLINE_INPUTS || so_game_word(txb + 1) != (u32)frame || t[13] != d) {
		adj_fail("the game's pad buffer is not as expected", frame);
		return 0;
	}
	/* the ring, oldest first: P(F-D)..P(F-1) */
	for (k = 0; k < d; k++)
		for (i = 0; i < GAME_PAD_SIZE; i++)
			old[k][i] = o[ODB_DELAY_RING + ((o[ODB_DELAY_INDEX] + k) % d) * GAME_PAD_SIZE + i];
	if (memcmp(old[d - 1], s_adj.last_pad.pad, SO_PAD_SIZE) != 0) {
		adj_fail("the game's newest pad is not the one last sent", frame);
		return 0;
	}
	for (k = 0; k < (step > 0 ? d : d - 1); k++)
		for (i = 0; i < GAME_PAD_SIZE; i++)
			o[ODB_DELAY_RING + k * GAME_PAD_SIZE + i] = old[k][i];
	if (step > 0) {
		for (i = 0; i < GAME_PAD_SIZE; i++)
			o[ODB_DELAY_RING + d * GAME_PAD_SIZE + i] = old[d - 1][i];
	} else {
		for (i = 0; i < GAME_PAD_SIZE; i++)
			t[TXB_PAD + i] = old[d - 1][i];
	}
	o[ODB_DELAY_INDEX] = 0;
	o[ODB_DELAY] = (u8)n;
	s_adj.last_change = frame;
	s_adj.want = 0;
	s_adj.runs = 0;
	s_adj.changes++;
	so_log("game: delay %d -> %d at frame %d (ping %u ms over the last 3 s): one input %s", d, n, (int)frame,
		(unsigned)s_adj.median_ms, step > 0 ? "repeated" : "dropped");
	so_gamelog_mark('A', n);
	return 1;
}

/* ---- 0xB0 ONLINE_INPUTS (handleOnlineInputs, lockstep) -------------------- */

/* The game's checksums of its finished frames (Slippi's desync check): ours,
 * by frame, as the game sends them */
#define SUMS 128
static struct {
	s32 frame[SUMS];
	u32 sum[SUMS];
	int logged;
	s32 late_frame;	/* the last of the opponent's taken as the frame before's */
	u32 late;
} s_sums;

/* The opponent's latest checksum and its frame, for the game.
 *
 * Slippi's game code sends, with the number of its newest finished frame, the
 * checksum it finds for that frame; when it finds none, the last one sent
 * stays in its buffer: the frame before's, under the new frame's number. A
 * Slippi Dolphin host did that twice in 14 games (2026-11-09): its checksum
 * "for frame F" was, in all 32 bits, that of the state before frame F - 1,
 * worked out again from the Wii's replay, while the Wii's was frame F's, and
 * the two replays were the same on every frame. The Wii's game compared the
 * two, showed "Desync Risk" in one match and ended the other with "DESYNC
 * DETECTED".
 *
 * So a checksum of the opponent's that is not ours for its frame but is ours
 * for the frame before is given to the game as that frame's, which it is. A
 * real desync does not pass for one: the opponent's state would have to be
 * ours of a frame earlier in every value the checksum covers. */
static void remote_sum(int remote, s32 *frame, u32 *sum)
{
	s32 f;
	u32 s;
	so_np_remote_checksum(remote, &f, &s);
	if (f > 1 && s_sums.frame[f % SUMS] == f && s_sums.sum[f % SUMS] != s &&
	    s_sums.frame[(f - 1) % SUMS] == f - 1 && s_sums.sum[(f - 1) % SUMS] == s) {
		if (s_sums.late_frame != f) {
			s_sums.late_frame = f;
			s_sums.late++;
		}
		f--;
	}
	*frame = f;
	*sum = s;
}

/* The opponent's answer block: frame numbers, checksums and up to 7 pads per
 * remote, newest first (prepareOpponentInputs). The client lock is held.
 *
 * The 7 are the oldest the game hasn't finalized (from `finalized` on), as
 * Slippi Dolphin sends them (its queue cut at the finalized frame, read from
 * the oldest): not the newest 7. After a burst of the opponent's inputs (more
 * than 7 frames at once, after a lag spike), the newest 7 left out frames the
 * game still needed: it kept its predictions for them, and the consoles
 * desynced (after the longest rollbacks). The rest come in the next answers. */
static void fill_remote_inputs(u8 *r, s32 frame, s32 finalized, int remotes)
{
	s32 nums[3], smallest;
	int i, k;
	r[1] = (u8)remotes;
	for (i = 0; i < 3; i++) {
		nums[i] = frame;
		if (i < remotes) {
			s32 head = so_np_remote_head_frame(i), cf;
			u32 cs;
			remote_sum(i, &cf, &cs);
			so_put_u32(r + 0x02 + i * 8, (u32)cf);
			so_put_u32(r + 0x06 + i * 8, cs);
			if (finalized > 0 && head > finalized + ROLLBACK_MAX_FRAMES - 1)
				head = finalized + ROLLBACK_MAX_FRAMES - 1;
			nums[i] = head < frame ? head : frame;
		}
		so_put_u32(r + 0x1A + i * 4, (u32)nums[i]);
	}
	smallest = nums[0];
	for (i = 1; i < 3; i++)
		if (nums[i] < smallest)
			smallest = nums[i];
	so_put_u32(r + 0x26, (u32)smallest);
	for (i = 0; i < remotes; i++) {
		u8 *pads = r + 0x2A + i * ROLLBACK_MAX_FRAMES * PAD_FULL_SIZE;
		for (k = 0; k < ROLLBACK_MAX_FRAMES; k++)
			so_np_remote_pad(i, nums[i] - k, pads + k * PAD_FULL_SIZE);	/* 8 bytes; the rest stays 0 */
	}
}

/* fused.c: Slippi Dolphin's rounding of the fused multiply-adds */
int melee_wii_fused_patch_gct(unsigned char *gct, unsigned int len, unsigned int load_address) __attribute__((weak));
void melee_wii_fused_report(void) __attribute__((weak));

/* wii/src/stall.c: a step number on the screen (where a hang stopped) */
void melee_wii_screen_mark(unsigned int n) __attribute__((weak));
#define MARK(n) (melee_wii_screen_mark ? melee_wii_screen_mark(n) : (void) 0)

static u32 s_reentries; /* (reentered_b0, below) */
/* The raw pad queue (HSD's PadLibData raw reads: size, -, index, count) */
#define RAW_PADS_ADDR 0x804C1F78u
static u32 s_lag_skips, s_lag_run;
/* (the diagnostics' record of a game, so_gamelog.c) */
static u8 s_game_delay;
static SoNpStats s_game_np;

/* One line per game: how much the lockstep waited for the opponent. */
static void log_game(void)
{
	u64 us;
	u32 expected;
	so_rollback_end_game();
	if (x.game.frames >= 300 && melee_wii_fused_report != NULL)
		melee_wii_fused_report();
	if (x.game.frames < 300) {	/* the results screen's polls, not a game */
		so_gamelog_cancel();
		memset(&x.game, 0, sizeof(x.game));
		return;
	}
	us = so_time_us() - x.game.start_us;
	expected = (u32)(us / SO_FRAME_US);
	so_client_lock();
	if (so_gamelog_recording()) {
		/* (the diagnostics' record: the game, and what the lines below say) */
		const SoSelections *sel = so_np_local_selections();
		SoNpStats np;
		int i;
		so_np_stats(&np);
		so_gamelog_note("frames=%u ms=%u expected=%u waited=%u held=%u longest=%u", (unsigned)x.game.frames,
			(unsigned)(us / 1000), (unsigned)expected, (unsigned)x.game.skips, (unsigned)x.game.held,
			(unsigned)x.game.longest);
		so_gamelog_note("delay_changes=%u", (unsigned)s_adj.changes);
		so_gamelog_note("delay=%u wifi=%d players=%d port=%d", (unsigned)s_game_delay,
			so_client_config() != NULL ? so_client_config()->wifi : -1, so_np_remote_count() + 1,
			so_np_local_player_idx() + 1);
		if (sel != NULL)
			so_gamelog_note("character=%u stage=%u", (unsigned)sel->character_id, (unsigned)sel->stage_id);
		for (i = 0; i < so_np_remote_count(); i++) {
			sel = so_np_remote_selections(i);
			if (sel != NULL)
				so_gamelog_note("opponent%d_character=%u stage=%u", i + 1, (unsigned)sel->character_id,
					(unsigned)sel->stage_id);
		}
		so_gamelog_note("pad_packets sent=%u received=%u stale=%u bad=%u resends=%u acks sent=%u received=%u",
			(unsigned)(np.pad_packets_sent - s_game_np.pad_packets_sent),
			(unsigned)(np.pad_packets_received - s_game_np.pad_packets_received),
			(unsigned)(np.pad_packets_stale - s_game_np.pad_packets_stale),
			(unsigned)(np.pad_packets_bad - s_game_np.pad_packets_bad),
			(unsigned)(np.resends - s_game_np.resends), (unsigned)(np.acks_sent - s_game_np.acks_sent),
			(unsigned)(np.acks_received - s_game_np.acks_received));
		so_gamelog_note("slow_polls=%u max_gap_us=%u max_answer_us=%u slow_answers=%u max_lock_us=%u pad_reads_waited=%u",
			(unsigned)x.game.slow_polls, (unsigned)x.game.max_gap_us, (unsigned)x.game.max_answer_us,
			(unsigned)x.game.slow_answers, (unsigned)x.game.max_lock_us, (unsigned)s_lag_skips);
	}
	so_log("game over: %u frames in %u ms (%u at 59.94 fps); waited %u frames (%u frames waited, longest %u), ping %u us",
		(unsigned)x.game.frames, (unsigned)(us / 1000), (unsigned)expected, (unsigned)x.game.skips,
		(unsigned)x.game.held, (unsigned)x.game.longest, (unsigned)so_np_take_avg_ping_us());
	so_log("game pacing: %u polls over 20 ms apart (longest %u us); answers: longest %u us, %u over 1 ms, lock wait up to %u us",
		(unsigned)x.game.slow_polls, (unsigned)x.game.max_gap_us, (unsigned)x.game.max_answer_us,
		(unsigned)x.game.slow_answers, (unsigned)x.game.max_lock_us);
	so_log("game traffic: pads waited up to %u us for the network thread; %u replay transfers",
		(unsigned)so_client_take_pad_latency_us(), (unsigned)x.game.replay_writes);
	if (s_reentries)
		so_log("game: Slippi's input code entered again %u times (inner calls skipped)", (unsigned)s_reentries);
	if (s_lag_skips)
		so_log("game: %u pad reads waited for the game to take the last one", (unsigned)s_lag_skips);
	if (s_sums.late)
		so_log("game: %u of the opponent's checksums came under the next frame's number (the last at frame %d)",
			(unsigned)s_sums.late, (int)s_sums.late_frame);
	if (x.game.waits_logged) {
		char line[150];
		int len = so_snprintf(line, sizeof(line), "game waits (frame:ms):"), i;
		for (i = 0; i < (int)x.game.waits_logged; i++)
			len += so_snprintf(line + len, sizeof(line) - len, " %u:%u", (unsigned)x.game.wait_frame[i],
				(unsigned)x.game.wait_ms[i]);
		so_log("%s", line);
	}
	so_client_unlock();
	so_gamelog_end();
	memset(&x.game, 0, sizeof(x.game));
}

/* The first frame where our checksum and the opponent's differ is logged (the
 * game shows the desync later, when it sees it), with the rollbacks before
 * it */
static void check_sums(s32 frame, int remotes)
{
	int i;
	for (i = 0; i < remotes && !s_sums.logged; i++) {
		s32 cf;
		u32 cs;
		remote_sum(i, &cf, &cs);
		if (cf > 0 && s_sums.frame[cf % SUMS] == cf && s_sums.sum[cf % SUMS] != cs) {
			char what[200];
			so_rollback_describe(what, sizeof(what));
			so_log("desync: frame %d's checksum %08x here, %08x the opponent's (now at frame %d); %s", (int)cf,
			       (unsigned)s_sums.sum[cf % SUMS], (unsigned)cs, (int)frame, what);
			s_sums.logged = 1;
		}
	}
}

/* Slippi's input code (TriggerSendInput, at HSD_PadRenewRawStatus) is not
 * reentrant: it reads its frame (ODB_FRAME) on entry and stores it plus one
 * on exit. Two callers reach it: the VI retrace callback (the lag reduction
 * codes' pad renew), and, after a rollback, the main loop (the renew the
 * retrace skipped during it: ODB_SHOULD_FORCE_PAD_RENEW). Slippi's guard
 * (PreventPadAlarmDuringRollback) holds the retrace's only while the
 * rollback is active, so a retrace during the main loop's forced renew runs
 * the code inside itself: two inputs for one frame, one frame counted (the
 * game's frame counter a frame ahead of the codes' from then: every input a
 * frame off, a desync). In Dolphin an EXI command takes no emulated time and
 * the retrace almost never lands there; here 0xB0's answer runs with
 * interrupts off, and a retrace that came meanwhile runs the moment the
 * game turns them back on, between the outer call's write and read.
 *
 * So the inner call is told to skip (its code exits before taking an input)
 * and the outer call reads its own answer: a 0xB0 written before the last
 * one's answer was read (nested), or one for the frame the last answer let
 * go on (after the outer's read, before its exit). */
static u8 s_last_b0[RXB_SIZE];	/* the last 0xB0 answer */
static s32 s_last_b0_frame;
static int s_last_b0_normal;
static u8 s_outer_b0[RXB_SIZE];	/* a nested call's outer answer, for its read */
static int s_restore_outer;
static u64 s_last_b0_us;	/* when the last 0xB0 was answered */
static s32 s_again_frame;	/* the last frame a reentry after the read was skipped on */
static u32 s_reentries;

static void on_online_inputs(const u8 *p)
{
	s32 frame = (s32)so_get_u32(p + 1), finalized = (s32)so_get_u32(p + 5);
	u32 checksum = so_get_u32(p + 9);
	u8 delay = p[13];
	u8 *r = answer(RXB_SIZE);
	int remotes, i, ready = 1, why = SO_GAMELOG_PLAY;
	s32 log_head = frame, log_offset = 0;
	u32 log_ping = 0;

	u64 t_in = so_time_us(), t_lock;
	x.stats.b0_polls++;
	if (x.game.last_poll_us && frame > 1) {
		u32 gap = (u32)(t_in - x.game.last_poll_us);
		if (gap > 20000)
			x.game.slow_polls++;
		if (gap > x.game.max_gap_us)
			x.game.max_gap_us = gap;
	}
	x.game.last_poll_us = t_in;
	if (!r)
		return;
	if (!client_up()) {
		r[0] = RESP_DISCONNECTED;
		return;
	}
	/* Game start, once per game: lockstep may poll frame 1 several times. */
	if (frame == 1 && x.last_frame != 1) {
		MARK(50);
		log_game();	/* a game that ended without a report */
		MARK(51);
		so_client_lock();
		so_np_start_game();
		so_client_unlock();
		MARK(52);
		so_rollback_start_game();
		MARK(53);
		s_reentries = 0;
		s_lag_skips = s_lag_run = 0;
		s_again_frame = 0;
		/* (online play is rollback only: a game without savestates ends) */
		x.no_savestates = so_rollback_enabled() && !so_rollback_active();
		memset(&s_sums, 0, sizeof(s_sums));
		reset_selections(&x.local_sel);
		x.dummies_sent = 0;
		x.stall_since_us = 0;
		x.game.start_us = so_time_us();
		s_game_delay = delay;
		{
			int off = s_adj.off;
			memset(&s_adj, 0, sizeof(s_adj));
			s_adj.off = off;
		}
		so_client_lock();
		so_np_stats(&s_game_np);
		so_client_unlock();
		so_gamelog_start();
		{
			/* the delay the game plays at, and how it was chosen */
			u32 ping_ms;
			int samples, automatic = so_client_delay_info(&ping_ms, &samples);
			if (automatic)
				so_log("game: delay %u (automatic: ping %u ms from %d readings before the game)%s",
					(unsigned)delay, (unsigned)ping_ms, samples,
					delay != so_client_config()->delay ? "; NOT the one last given to the game" : "");
			else
				so_log("game: delay %u (the launcher's setting)", (unsigned)delay);
			so_gamelog_note("delay_choice=%s connect_ping_ms=%u readings=%d given=%u",
				automatic ? "auto" : "override", (unsigned)ping_ms, samples,
				(unsigned)so_client_config()->delay);
		}
	}
	x.last_frame = frame;

	so_client_lock();
	t_lock = so_time_us() - t_in;
	if (t_lock > x.game.max_lock_us)
		x.game.max_lock_us = (u32)t_lock;
	if (so_np_status() != SO_NP_CONNECTED) {
		so_client_unlock();
		r[0] = RESP_DISCONNECTED;
		return;
	}
	if (x.no_savestates) {
		/* Online play is rollback only: a game whose savestates could not
		 * be set up (so_rollback.c logs why) ends, with the reason on the
		 * character select, rather than play on delay-based. (Not the polls
		 * after a game's end, its savestates let go: those go on as before.) */
		x.no_savestates = 0;
		so_log("exi: no savestates for this game; ending it");
		so_np_force_disconnect(0);
		so_client_fail("Rollback could not start on this Wii (see the log)");
		so_client_unlock();
		r[0] = RESP_DISCONNECTED;
		return;
	}
	remotes = so_np_remote_count();
	if (so_rollback_enabled()) {
		/* Rollback: the game goes on with what has come, predicting the
		 * rest, up to ROLLBACK_MAX_FRAMES ahead of the opponent; and stays
		 * in time with it (so_rollback.c) */
		ready = so_rollback_ready(frame, finalized, remotes);
		if (!ready)
			why = so_rollback_waits_in_time() ? SO_GAMELOG_WAIT_SYNC : SO_GAMELOG_WAIT_INPUTS;
	} else {
		for (i = 0; i < remotes; i++)
			if (so_np_remote_head_frame(i) < frame)
				ready = 0;
		if (!ready)
			why = SO_GAMELOG_WAIT_INPUTS;
	}
	if (so_gamelog_recording()) {
		for (i = 0; i < remotes; i++) {
			s32 head = so_np_remote_head_frame(i);
			if (i == 0 || head < log_head)
				log_head = head;
		}
		log_offset = so_np_time_offset_us();
		log_ping = so_np_max_ping_us();
	}

	/* The game a frame behind its input timer (a capture each predicted
	 * frame and the frames played again take real time here, none in
	 * Dolphin): the last pad read not yet taken by the game. Slippi's codes
	 * expect one read at a time (the raw pad queue's rewind after a load,
	 * which UCF's dash and turn read: desyncs there), so this read waits,
	 * as the timer would on a Dolphin running slow. At most 3 in a row. */
	if (ready && frame > 1 && so_rollback_active()) {
		u8 waiting = *(volatile u8 *)(RAW_PADS_ADDR + 3);
		if (waiting >= 1 && s_lag_run < 3) {
			ready = 0;
			why = SO_GAMELOG_WAIT_PAD;
			s_lag_run++;
			s_lag_skips++;
		} else {
			s_lag_run = 0;
		}
	}

	if (frame == 1 && !x.dummies_sent) {
		/* sent with this frame's (after the answer), so the opponent need not wait */
		x.pend.delay_pads = 1;
		x.pend.delay = delay;
		x.pend.valid = 1;
		x.dummies_sent = 1;
	}

	if (!ready) {
		/* Only resend what is not acked: the game polls this frame again. */
		u64 now = so_time_us();
		x.pend.valid = 1;	/* a resend, after the answer */
		r[0] = RESP_SKIP;
		x.stats.b0_skips++;
		x.game.skips++;
		if (x.game.run++ == 0) {
			x.game.held++;
			x.game.wait_since_us = now;
		}
		if (x.game.run > x.game.longest)
			x.game.longest = x.game.run;
		if (!x.stall_since_us) {
			x.stall_since_us = now;
		} else if (now - x.stall_since_us > STALL_LIMIT_US) {
			so_log("exi: no input from the opponent for 7 s; disconnecting");
			so_np_force_disconnect(0);
			r[0] = RESP_DISCONNECTED;
		}
	} else {
		SoLocalPad pad;
		/* (delay_adjust: the delay a frame longer or shorter from this pad on) */
		int step = adj_wanted(frame, delay);
		if (step != 0 && !adj_apply(frame, delay, step))
			step = 0;
		if (step > 0) {
			/* the frame left without a pad: the last one's again */
			x.pend.extra = s_adj.last_pad;
			x.pend.extra.frame++;
			x.pend.has_extra = 1;
			s_adj.last_pad = x.pend.extra;
			delay++;
		}
		pad.frame = frame + delay;
		pad.checksum_frame = finalized;
		pad.checksum = checksum;
		memcpy(pad.pad, p + 14, SO_PAD_SIZE);
		if (step >= 0) {
			x.pend.pad = pad;	/* sent after the answer */
			x.pend.has_pad = 1;
			s_adj.last_pad = pad;
			s_adj.have_last = 1;
		}	/* (shorter: this pad has no frame, and is dropped) */
		s_adj.last_play = frame;
		x.pend.valid = 1;
		r[0] = RESP_NORMAL;
		if (frame % 600 == 0 && frame != x.game.heartbeat_frame) {
			x.game.heartbeat_frame = frame;
			so_rollback_heartbeat(frame);
		}
		/* the ping, in the codeset's delay text, twice a second */
		if (melee_wii_ping_text != NULL && frame % 30 == 0 && so_rollback_active()) {
			u32 ping = so_np_max_ping_us();
			if (ping)
				melee_wii_ping_text((int)((ping + 500) / 1000));
		}
		x.stats.b0_frames++;
		x.game.frames++;
		if (x.game.run && x.game.waits_logged < 8) {
			x.game.wait_frame[x.game.waits_logged] = (u32)frame;
			x.game.wait_ms[x.game.waits_logged] = (u32)((so_time_us() - x.game.wait_since_us) / 1000);
			x.game.waits_logged++;
		}
		x.game.run = 0;
		x.stall_since_us = 0;
	}
	if (finalized > 0) {
		s_sums.frame[finalized % SUMS] = finalized;
		s_sums.sum[finalized % SUMS] = checksum;
	}
	fill_remote_inputs(r, frame, finalized, remotes);
	check_sums(frame, remotes);
	so_client_unlock();
	memcpy(s_last_b0, r, RXB_SIZE);
	s_last_b0_frame = frame;
	s_last_b0_normal = r[0] == RESP_NORMAL;
	s_last_b0_us = so_time_us();
	{
		u32 took = (u32)(so_time_us() - t_in);
		if (took > x.game.max_answer_us)
			x.game.max_answer_us = took;
		if (took > 1000)
			x.game.slow_answers++;
		so_gamelog_poll(t_in, frame, r[0] == RESP_NORMAL ? SO_GAMELOG_PLAY : r[0] == RESP_SKIP ? why : SO_GAMELOG_OTHER,
			log_head, log_offset, log_ping, took);
	}
}

/* Appends `src` cut to `chars` code points. */
static void utf8_append(char *dst, int size, const char *src, int chars)
{
	int len = (int)strlen(dst);
	const u8 *p = (const u8 *)src;
	while (*p && chars > 0) {
		int step = 1;
		if ((*p & 0xE0) == 0xC0)
			step = 2;
		else if ((*p & 0xF0) == 0xE0)
			step = 3;
		else if ((*p & 0xF8) == 0xF0)
			step = 4;
		if (len + step >= size)
			break;
		while (step-- && *p)
			dst[len++] = (char)*p++;
		chars--;
	}
	dst[len] = '\0';
}

/* ---- 0xB3 GET_MATCH_STATE (prepareOnlineMatchState) ------------------------ */

static u8 mm_state(void)
{
	switch (so_client_state()) {
	case SO_CLIENT_SEARCHING: return MM_MATCHMAKING;
	case SO_CLIENT_CONNECTING: return MM_OPPONENT_CONNECTING;
	case SO_CLIENT_CONNECTED:
	case SO_CLIENT_DISCONNECTED: return MM_CONNECTION_SUCCESS;
	case SO_CLIENT_ERROR: return MM_ERROR;
	default: return MM_IDLE;
	}
}

static u16 shade_key(const u8 *b, int slot)
{
	u8 c = b[0x60 + slot * 0x24];
	return (u16)(((c == 0x13 ? 0x12 : c) << 8) | b[0x63 + slot * 0x24]);
}

/* Writes the per-game fields into the match block once both players have
 * picked. The client lock is held. */
static u32 build_game(int remotes, u8 *alt_out)
{
	const SoSelections *by_port[SO_REMOTE_MAX + 1] = { NULL };
	const SoSelections *lps = so_np_local_selections();
	u16 stage = 0x1F;
	u8 alt = 0, *b = x.match_block;
	u32 rng;
	int i, j;

	if (lps->player_idx <= SO_REMOTE_MAX)
		by_port[lps->player_idx] = lps;
	for (i = 0; i < remotes; i++) {
		const SoSelections *s = so_np_remote_selections(i);
		if (s->player_idx <= SO_REMOTE_MAX)
			by_port[s->player_idx] = s;
	}
	for (i = 0; i <= remotes; i++) {
		if (by_port[i] && by_port[i]->is_stage_selected) {
			stage = by_port[i]->stage_id;
			alt = by_port[i]->alt_stage_mode;
			break;
		}
	}
	rng = so_np_is_decider() ? lps->rng_offset : so_np_remote_selections(0)->rng_offset;

	for (i = 0; i <= remotes; i++) {
		const SoSelections *s = by_port[i];
		if (!s || !s->is_character_selected)
			continue;
		b[0x60 + i * 0x24] = s->character_id;
		b[0x63 + i * 0x24] = s->character_color;
		b[0x67 + i * 0x24] = 0;
		b[0x69 + i * 0x24] = 0;	/* team: Teams only */
	}
	/* Shades: each human slot gets the number of earlier slots with the same
	 * character (Sheik counting as Zelda) and colour. */
	for (i = 0; i < 4; i++) {
		u8 shade = 0;
		if (b[0x61 + i * 0x24] != 0)
			continue;
		for (j = 0; j < i; j++)
			if (b[0x61 + j * 0x24] == 0 && shade_key(b, j) == shade_key(b, i))
				shade++;
		b[0x67 + i * 0x24] = shade;
	}
	b[0x08] = 0;	/* not Teams */
	b[0x61 + 2 * 0x24] = remotes >= 2 ? 0 : 3;
	b[0x61 + 3 * 0x24] = remotes >= 3 ? 0 : 3;
	so_put_u16(b + 0x0E, stage);
	if (x.mode == SO_MODE_DIRECT)
		b[0x02] &= 0xF7;	/* pause allowed */
	else
		b[0x02] |= 0x08;
	if (x.mode != SO_MODE_DIRECT && x.mode != SO_MODE_TEAMS)
		alt = 0;
	for (i = 0; i < 4; i++) {
		b[0x62 + i * 0x24] = 4;	/* stocks */
		so_put_u16(b + 0x70 + i * 0x24, 0);	/* percent */
	}
	if (melee_wii_costumes_match != NULL) {
		if (lps->player_idx <= SO_REMOTE_MAX)
			melee_wii_costumes_match(b[0x60 + lps->player_idx * 0x24], b[0x63 + lps->player_idx * 0x24]);
		else
			melee_wii_costumes_match(0xFF, 0);	/* (no slot of ours: nobody's) */
	}
	*alt_out = alt;
	return rng;
}

static void on_get_match_state(void)
{
	const SoUser *user = so_client_user();
	const SoMatchResult *m;
	u8 *r, state, local_ready, remote_ready = 0, chat_id = 0, chat_player = 0, sent = 0, alt = 0;
	u8 delay = so_client_config()->delay;
	u32 rng = 0;
	int remotes = 0, built = 0, i;
	char error[128];

	for (;;) {
		r = answer(MSRB_SIZE);
		if (!r)
			return;
		state = mm_state();
		local_ready = x.local_sel.is_character_selected;
		remote_ready = 0;
		if (state != MM_CONNECTION_SUCCESS) {
			x.have_client = 0;
			break;
		}
		m = so_client_match();
		x.local_idx = m->local_player_index;
		if (!x.have_client) {
			/* First answer since connecting: the stages this match allows,
			 * and our selections (made before the connection) go out. */
			x.have_client = 1;
			x.allowed_count = 0;
			for (i = 0; i < m->stage_count && i < MAX_STAGES; i++)
				x.allowed[x.allowed_count++] = m->stages[i];
			if (!x.allowed_count) {
				memcpy(x.allowed, default_stages, sizeof(default_stages));
				x.allowed_count = sizeof(default_stages) / sizeof(default_stages[0]);
			}
			x.pool_count = 0;
			x.local_sel.stage_id = random_stage();
			if (so_client_config()->test_stage) {
				x.local_sel.stage_id = so_client_config()->test_stage;
				x.local_sel.is_stage_selected = 1;
			}
			so_client_lock();
			so_np_set_selections(&x.local_sel);
			so_client_unlock();
		}
		so_client_lock();
		remotes = so_np_remote_count();
		{
			int connected = so_np_status() == SO_NP_CONNECTED, active = 0;
			for (i = 0; i < remotes; i++)
				active += so_np_player_active(so_np_remote_player_idx(i));
			if (!connected || active != remotes) {
				so_client_unlock();
				cleanup();	/* the opponent left: end the lobby */
				continue;	/* and answer for the new state */
			}
		}
		remote_ready = 1;
		for (i = 0; i < remotes; i++)
			if (!so_np_remote_selections(i)->is_character_selected)
				remote_ready = 0;
		if (remotes == 1) {
			x.local_idx = so_np_is_decider() ? 0 : 1;
			x.remote_idx = so_np_is_decider() ? 1 : 0;
		}
		/* Chat: the message we sent, else one received (one-shot each). */
		sent = x.sent_chat;
		x.sent_chat = 0;
		if (!sent) {
			u8 from;
			chat_id = (u8)so_np_take_chat(&from);
			if (chat_id && chat_id != SO_CHAT_DISABLED && !so_client_config()->chat) {
				so_np_send_chat(SO_CHAT_DISABLED);
				chat_id = 0;
			}
		}
		if (remotes == 1)
			chat_player = sent ? x.local_idx : x.remote_idx;
		if (local_ready && remote_ready) {
			rng = build_game(remotes, &alt);
			built = 1;
		}
		/* (the game takes the delay from this answer as a match starts) */
		delay = so_client_match_delay();
		so_client_unlock();
		break;
	}

	if (!built && melee_wii_costumes_match != NULL)
		melee_wii_costumes_match(-1, 0);	/* no match yet: build_game says when */

	r[0x000] = state;
	r[0x001] = local_ready;
	r[0x002] = remote_ready;
	r[0x003] = x.local_idx;
	r[0x004] = x.remote_idx;

	/* Always: mode, timer and items. */
	x.match_block[0x00] = 0x32;	/* stock mode, timer counting down */
	x.match_block[0x03] = 0x4C;	/* score UI hidden */
	so_put_u32(x.match_block + 0x10, 8 * 60);
	m = current_match();
	x.match_block[0x0B] = 0xFF;	/* items off */
	{
		u32 items = m ? m->items : 0;
		u32 hi = 0xF8000000u, lo = 0x0F000000u;
		if (items) {
			/* items << 28 across the 64-bit field */
			hi |= items >> 4;
			lo |= items << 28;
			x.match_block[0x0B] = 3;
		}
		so_put_u32(x.match_block + 0x23, hi);
		so_put_u32(x.match_block + 0x27, lo);
	}

	so_put_u32(r + 0x005, rng);
	r[0x009] = delay;
	r[0x00A] = sent;
	r[0x00B] = chat_id;
	r[0x00C] = chat_player;
	/* 0x00D, 0x00E: ranks (Ranked only) */
	if (x.have_client)
		so_game_string(user->display_name, 15, r + 0x00F);
	if (m) {
		char opponents[64] = "";
		for (i = 0; i < 4 && i < m->player_count; i++) {
			so_game_string(m->players[i].display_name, 15, r + 0x02E + i * 31);
			so_game_connect_code(m->players[i].connect_code, r + 0x0C9 + i * 10);
			strlcpy((char *)r + 0x0F1 + i * 29, m->players[i].uid, 29);
		}
		/* The opponents, as the game names them: the other human players'
		 * names, each cut to share 15 characters, joined with '/'. */
		{
			const char *names[4];
			int n = 0, each;
			for (i = 0; i < 4 && i < m->player_count; i++)
				if (i != x.local_idx && x.match_block[0x61 + i * 0x24] == 0 && m->players[i].display_name[0])
					names[n++] = m->players[i].display_name;
			each = n ? (15 - (n - 1)) / n : 0;
			for (i = 0; i < n; i++) {
				if (i)
					strlcpy(opponents + strlen(opponents), "/", sizeof(opponents) - strlen(opponents));
				utf8_append(opponents, sizeof(opponents), names[i], each);
			}
		}
		so_game_string(opponents, 15, r + 0x0AA);
		strlcpy((char *)r + 0x38E, m->match_id, 51);
	}
	error[0] = '\0';
	if (client_up())
		so_client_error(error, sizeof(error));
	so_game_string(error, 120, r + 0x165);
	memcpy(r + 0x256, x.match_block, MATCH_BLOCK_SIZE);
	r[0x3C1] = alt;
}

/* ---- the other commands --------------------------------------------------- */

static void remember_code(const u8 *code, int len);
static void load_history(void);
static volatile int history_loaded;

/* startFindMatch */
static void on_find_opponent(const u8 *p)
{
	SoSearch search;
	int len = 0;
	memset(&search, 0, sizeof(search));
	search.mode = p[1];
	while (len < 18 && p[2 + len])
		len++;	/* the code, cut at its first 0 */
	if (len > (int)sizeof(search.connect_code))
		len = sizeof(search.connect_code);
	memcpy(search.connect_code, p + 2, len);
	search.connect_code_len = (u8)len;
	if (search.mode == 2) {	/* ONLINE_MODE_DIRECT */
		if (!history_loaded)
			load_history();
		remember_code(search.connect_code, len);
	}
	x.mode = search.mode;
	x.have_client = 0;
	x.last_frame = 0;
	if (!client_up())
		return;
	so_log("exi: find opponent (mode %u)", (unsigned)search.mode);
	so_client_find_match(&search);
}

/* setMatchSelections */
static void on_set_selections(const u8 *p)
{
	SoSelections s;
	u8 option = p[7];
	memset(&s, 0, sizeof(s));
	s.team_id = p[1];
	s.character_id = p[2];
	s.character_color = p[3];
	s.is_character_selected = p[4] != 0;
	s.stage_id = so_get_u16(p + 5);
	s.is_stage_selected = option == 1 || option == 3;
	if (option == 3)
		s.stage_id = random_stage();
	if (x.have_client && so_client_config()->test_stage) {
		s.stage_id = so_client_config()->test_stage;
		s.is_stage_selected = 1;
	}
	s.alt_stage_mode = p[9];
	s.rng_offset = so_random() % 0xFFFF;
	merge_selections(&x.local_sel, &s);
	if (x.have_client) {
		so_client_lock();
		if (so_np_status() == SO_NP_CONNECTED)
			so_np_set_selections(&x.local_sel);
		so_client_unlock();
	}
}

static void on_online_status(void)
{
	const SoUser *user = so_client_user();
	u8 *r = answer(OSB_SIZE);
	if (so_client_state() == SO_CLIENT_OFF)
		return;	/* not logged in: the online modes stay locked */
	r[0] = 1;
	so_game_string(user->display_name, 15, r + 0x01);
	so_game_connect_code(user->connect_code, r + 0x20);
}

static void on_send_chat(const u8 *p)
{
	if (!x.have_client || !so_client_config()->chat)
		return;
	so_client_lock();
	if (so_np_status() == SO_NP_CONNECTED) {
		x.sent_chat = p[1];
		so_np_send_chat(p[1]);
	}
	so_client_unlock();
}

/* The direct-code history behind connect-code autocomplete (0xBE), most
 * recent first: the codes searched in Direct this session (raw Shift-JIS, 2
 * bytes a character, as 0xB4 sends them), then selftest_code. Slippi Dolphin
 * keeps it in direct-codes.json; this one lasts until the console is off. */
#define HISTORY_MAX 8
static struct {
	u8 code[16];
	u8 len;	/* bytes */
} history[HISTORY_MAX + 1];
static int history_count;
static volatile int history_dirty, history_loaded;

/* SO_DIRECT_CODES_PATH: "SLDC", a count, then per code its length in bytes
 * and 16 bytes of Shift-JIS, most recent first. */
#define SO_DIRECT_CODES_PATH "/slippi/direct-codes.bin"
#define HISTORY_FILE_SIZE (5 + HISTORY_MAX * 17)

static void load_history(void)
{
	u8 buf[HISTORY_FILE_SIZE];
	int n, i;
	history_loaded = 1;
	n = so_read_file(SO_DIRECT_CODES_PATH, buf, sizeof(buf));
	if (n < 5 || memcmp(buf, "SLDC", 4) != 0)
		return;
	for (i = 0; i < buf[4] && i < HISTORY_MAX && 5 + (i + 1) * 17 <= n; i++) {
		const u8 *e = buf + 5 + i * 17;
		if (e[0] == 0 || e[0] > 16 || e[0] % 2)
			break;
		history[i].len = e[0];
		memcpy(history[i].code, e + 1, 16);
		history_count = i + 1;
	}
	so_log("exi: %d direct codes remembered", history_count);
}

void so_exi_save_history(void)
{
	u8 buf[HISTORY_FILE_SIZE];
	int i, count;
	if (!history_dirty)
		return;
	history_dirty = 0;
	count = history_count;
	memset(buf, 0, sizeof(buf));
	memcpy(buf, "SLDC", 4);
	buf[4] = (u8)count;
	for (i = 0; i < count; i++) {
		buf[5 + i * 17] = history[i].len;
		memcpy(buf + 6 + i * 17, history[i].code, 16);
	}
	if (so_write_file(SO_DIRECT_CODES_PATH, buf, 5 + count * 17) < 0)
		so_log("exi: could not save the direct codes");
}

static void remember_code(const u8 *code, int len)
{
	int i, j;
	if (len <= 0 || len > 16 || len % 2)
		return;
	for (i = 0; i < history_count; i++)
		if (history[i].len == len && memcmp(history[i].code, code, len) == 0)
			break;
	if (i == history_count && history_count < HISTORY_MAX)
		history_count++;
	if (i == history_count)
		i = history_count - 1;	/* full: drop the oldest */
	for (j = i; j > 0; j--)
		history[j] = history[j - 1];
	memset(history[0].code, 0, sizeof(history[0].code));
	memcpy(history[0].code, code, len);
	history[0].len = (u8)len;
	history_dirty = 1;
}

/* The history with selftest_code last (unless already in it); returns the count. */
static int code_list(void)
{
	if (!history_loaded)
		load_history();
	const SoConfig *c = so_client_config();
	u8 code[16];
	int n = c && c->selftest_code[0] ? so_encode_connect_code(c->selftest_code, code, sizeof(code)) : -1, i;
	for (i = 0; n > 0 && i < history_count; i++)
		if (history[i].len == n && memcmp(history[i].code, code, n) == 0)
			n = -1;
	if (n > 0) {
		memcpy(history[history_count].code, code, n);
		history[history_count].len = (u8)n;
		return history_count + 1;
	}
	return history_count;
}

/* handleNameEntryLoad: the first code from `index` on (or back, scrolling
 * newer) that starts with the typed characters. */
static void on_code_suggestion(const u8 *p)
{
	u8 *r = answer(30);
	u8 len = p[0x19], dir = p[0x1E];
	u32 index = so_get_u32(p + 0x1A), start;
	int count = code_list(), found = -1, i, k;
	if (len > 8)
		len = 8;
	if (dir == 1)
		index++;
	else if (dir == 2)
		index--;
	else if (dir == 3)
		index = 0;
	for (start = index; found < 0 && start < (u32)count;) {
		int ok = history[start].len / 2 >= len;
		for (k = 0; ok && k < len; k++)
			ok = history[start].code[2 * k] == p[1 + 3 * k] && history[start].code[2 * k + 1] == p[2 + 3 * k];
		if (ok)
			found = (int)start;
		else if (dir == 2)
			start--;	/* unsigned: below 0 ends the walk */
		else
			start++;
	}
	if (found < 0) {
		/* not found: the typed text, echoed */
		memcpy(r + 1, p + 1, 3 * len);
		r[0x19] = len;
		memcpy(r + 0x1A, p + 0x1A, 4);
		return;
	}
	r[0] = 1;
	for (i = 0; i < history[found].len / 2 && i < 8; i++) {
		r[1 + 3 * i] = history[found].code[2 * i];
		r[2 + 3 * i] = history[found].code[2 * i + 1];
	}
	r[0x19] = (u8)(history[found].len / 2);
	so_put_u32(r + 0x1A, (u32)found);
}

/* handleGetPlayerSettings: every player's 16 quick-chat messages. */
static void on_player_settings(void)
{
	static const char *const defaults[16] = {
		"ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
		"sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later", "bad connection",
	};
	const SoMatchResult *m = current_match();
	u8 *r = answer(PLAYER_SETTINGS_SIZE);
	int i, j;
	if (!r)
		return;
	for (i = 0; i < 4; i++) {
		const SoMmPlayer *p = NULL;
		int k;
		for (k = 0; m && k < m->player_count; k++)
			if (m->players[k].port == i + 1 && m->players[k].has_chat_messages)
				p = &m->players[k];
		for (j = 0; j < 16; j++)
			so_game_string(p ? p->chat_messages[j] : defaults[j], 25, r + i * 816 + j * 51);
	}
}

static void on_premade_text(const u8 *p, int load)
{
	u8 text[400];
	const SoMatchResult *m = current_match();
	const char *name = "";
	int id = p[1], n;
	if (id >= 1 && id <= 4 && m && id - 1 < m->player_count)
		name = m->players[id - 1].display_name;
	n = so_premade_text(id, p[2], name, text, sizeof(text));
	if (n < 0) {
		so_log("exi: premade text %d (param 0x%02x) unknown", id, p[2]);
		n = 0;
	}
	if (load) {
		u8 *r = answer((u32)n);
		if (r)
			memcpy(r, text, n);
	} else {
		so_put_u32(answer(4), (u32)n);
	}
}

/* Diagnostics: the last file the game asked for, and the last archives it
 * parsed (tools/ppc/probe_archive.s), shown when it panics. */
static char last_file[40];
static struct {
	char file[40];
	u32 data, length, caller, header[3];
} archives[4];
static u32 archive_count;

static void log_archive(u32 i)
{
	so_log("ARCHIVE %u: after %s: data %08x length %u (header size %u data %u relocs %u) from %08x", (unsigned)i,
		archives[i % 4].file, (unsigned)archives[i % 4].data, (unsigned)archives[i % 4].length,
		(unsigned)archives[i % 4].header[0], (unsigned)archives[i % 4].header[1], (unsigned)archives[i % 4].header[2],
		(unsigned)archives[i % 4].caller);
}

static void on_file(const u8 *p, int load)
{
	char name[0x41];
	u32 len;
	memcpy(name, p + 1, 0x40);
	name[0x40] = '\0';
	len = so_files_size(name);
	strlcpy(last_file, name, sizeof(last_file));
	if (!load) {
		so_put_u32(answer(4), len);
		if (len)
			so_log("exi: serving %s (%u bytes)", name, (unsigned)len);
		else if (x.stats.disc_files++ < 64)
			so_log("exi: %s from the disc", name);
	} else if (len) {
		answer(0);
		strlcpy(x.file, name, sizeof(x.file));
		x.file_pending = 1;
		x.resp_len = len;
	} else {
		answer(0);
	}
}

int melee_wii_libc_fix_code(unsigned int *words, unsigned int count); /* wii/src/main.c */

/* The codeset's "Delay: 2f" in a game's corner (its HUD codes at 0x802f666c
 * and 0x802f6694) blanked: the delay goes with the connection and the
 * opponent's ping, and the port shows the ping in that text instead
 * (wii/src/overlay.c). Its other messages
 * (Desync Risk, DESYNC DETECTED...) are text of their own and stay. */
static int hide_delay_text(u8 *gct, u32 len)
{
	static const char text[] = "Delay: %df";
	int n = 0;
	u32 i;
	for (i = 0; i + sizeof(text) <= len; i++)
		if (!memcmp(gct + i, text, sizeof(text))) {
			gct[i] = ' ';
			gct[i + 1] = 0;
			n++;
		}
	return n;
}

static void on_gct(int load, u32 address)
{
	const u8 *gct;
	u32 len = so_files_gct(&gct);
	if (!load) {
		so_put_u32(answer(4), len);
	} else {
		/* The bootloader writes its buffer's address after the command. */
		int linked;
		if (len)	/* (its calls to the game's memset and memcpy: where the port runs them) */
			melee_wii_libc_fix_code((unsigned int *)gct, len / 4);
		if (len && !hide_delay_text((u8 *)gct, len))
			so_log("exi: the codeset's delay text not found");
		if (len && melee_wii_fused_patch_gct != NULL)	/* (fused.c: Slippi Dolphin's rounding) */
			melee_wii_fused_patch_gct((u8 *)gct, len, address);
		linked = len ? so_gct_link((u8 *)gct, len, address) : 0;
		answer(0);
		x.resp = gct;
		x.resp_len = len;
		so_log("exi: codeset loaded (%u bytes) for %08x, %d hooks linked", (unsigned)len, (unsigned)address, linked);
	}
}

/* Hands what the last 0xB0 queued to the network thread (so_exi.h,
 * so_exi_after_ack): the kernel's main thread does no network I/O. */
static void send_pending(void)
{
	if (!x.pend.valid)
		return;
	if (x.pend.delay_pads) {
		/* empty pads for frames 1..delay: the first input is for frame
		 * 1 + delay (handleSendInputs) */
		SoLocalPad pad;
		int f;
		for (f = 1; f <= x.pend.delay; f++) {
			memset(&pad, 0, sizeof(pad));
			pad.frame = f;
			so_client_queue_pad(&pad);
		}
	}
	if (x.pend.has_extra)
		so_client_queue_pad(&x.pend.extra);
	so_client_queue_pad(x.pend.has_pad ? &x.pend.pad : NULL);
	memset(&x.pend, 0, sizeof(x.pend));
}

void so_exi_after_ack(void)
{
	if (x.pend.valid && x.answer_read)
		send_pending();
}

void so_exi_other_write(void)
{
	x.has_resp = 0;
	x.file_pending = 0;
	x.game.replay_writes++;
}

/* The last four commands, the newest in the low byte (wii/src/stall.c's
 * note of interrupts left off) */
static volatile u32 s_last_cmds;
void melee_wii_irq_mark(u32 kind); /* wii/src/os.c */

u32 so_exi_last_commands(void)
{
	return s_last_cmds;
}

/* A 0xB0 that reentered Slippi's input code (above): answered SKIP with the
 * outer call's answer otherwise, and nothing queued. Returns 1 if so. */
static int reentered_b0(const u8 *request, u8 prev_cmd, int prev_read, int prev_answer)
{
	s32 frame = (s32)so_get_u32(request + 1);
	int nested = prev_cmd == CMD_ONLINE_INPUTS && !prev_read && prev_answer;
	/* (the game also asks for a frame again after its "go on", legitimately,
	 * after a rollback; a reentry comes while the outer call still runs:
	 * within a few hundred us of its answer. Once per frame, so that a
	 * misjudged one costs one frame, never a freeze) */
	int again = prev_read && s_last_b0_normal && frame == s_last_b0_frame && frame > 1 &&
		(u32)(so_time_us() - s_last_b0_us) < 3000 && s_again_frame != frame;
	u8 *r;
	if (!nested && !again)
		return 0;
	if (nested)
		memcpy(s_outer_b0, x.resp, RXB_SIZE);
	r = answer(RXB_SIZE);
	if (!r)
		return 0;
	memcpy(r, nested ? s_outer_b0 : s_last_b0, RXB_SIZE);
	r[0] = RESP_SKIP;
	s_restore_outer = nested;
	if (again)
		s_again_frame = frame;
	s_reentries++; /* (the game's summary has the count) */
	return 1;
}

void so_exi_write(const u8 *request, u32 dma_len)
{
	static u8 seen[32];
	u8 cmd = request[0], prev_cmd = x.stats.last_command;
	int prev_read = x.answer_read;
	int prev_answer = x.has_resp && !x.file_pending && x.resp == x.big && x.resp_len == RXB_SIZE;
	s_last_cmds = (s_last_cmds << 8) | cmd;
	melee_wii_irq_mark(0x100u | cmd);
	init_once();
	if (x.pend.valid)
		send_pending();	/* the game wrote again without reading the answer */
	x.answer_read = 0;
	x.stats.commands++;
	x.stats.last_command = cmd;
	if (!(seen[cmd >> 3] & (1 << (cmd & 7)))) {
		seen[cmd >> 3] |= (u8)(1 << (cmd & 7));
		so_log("exi: first 0x%02X (%u bytes)", cmd, (unsigned)dma_len);
	}
	/* A command without an answer leaves none behind (Dolphin's read queue
	 * keeps the old one, but nothing reads it). */
	x.has_resp = 0;
	x.file_pending = 0;
	switch (cmd) {
	case CMD_ONLINE_INPUTS:
		if (!reentered_b0(request, prev_cmd, prev_read, prev_answer))
			on_online_inputs(request);
		break;
	case CMD_CAPTURE_SAVESTATE:
		if (so_rollback_enabled()) {
			so_savestate_capture((s32)so_get_u32(request + 1));
			break;
		}
		/* fall through: lockstep never predicts, so these should never come */
	case CMD_LOAD_SAVESTATE:
		if (cmd == CMD_LOAD_SAVESTATE && so_rollback_enabled()) {
			so_savestate_load((s32)so_get_u32(request + 1), request + 5);
			break;
		}
		if (x.stats.savestate_requests++ < 4)
			so_log("exi: unexpected savestate command 0x%02x (frame %d)", cmd, (int)so_get_u32(request + 1));
		break;
	case CMD_GET_MATCH_STATE:
		on_get_match_state();
		break;
	case CMD_FIND_OPPONENT:
		on_find_opponent(request);
		break;
	case CMD_SET_MATCH_SELECTIONS:
		on_set_selections(request);
		break;
	case CMD_GET_ONLINE_STATUS:
		on_online_status();
		break;
	case CMD_CLEANUP_CONNECTION:
		cleanup();
		break;
	case CMD_SEND_CHAT_MESSAGE:
		on_send_chat(request);
		break;
	case CMD_GET_NEW_SEED:
		so_put_u32(answer(4), so_random());
		break;
	case CMD_FETCH_CODE_SUGGESTION:
		on_code_suggestion(request);
		break;
	case CMD_GP_FETCH_STEP:
		answer(6);	/* not found (Ranked only) */
		break;
	case CMD_GET_PLAYER_SETTINGS:
		on_player_settings();
		break;
	case CMD_LOG_MESSAGE: {
		char text[126];
		memcpy(text, request + 3, sizeof(text) - 1);
		text[sizeof(text) - 1] = '\0';
		so_log("game: %s", text);
		break;
	}
	case CMD_FILE_LENGTH:
	case CMD_FILE_LOAD:
		on_file(request, cmd == CMD_FILE_LOAD);
		break;
	case CMD_GCT_LENGTH:
	case CMD_GCT_LOAD:
		on_gct(cmd == CMD_GCT_LOAD, cmd == CMD_GCT_LOAD ? so_get_u32(request + 1) : 0);
		break;
	case CMD_CRASH_REPORT: {
		const u8 *p = request;
		x.stats.crash_srr0 = so_get_u32(p + 0x08);
		x.stats.crash_dar = so_get_u32(p + 0x10);
		so_log("CRASH: exception %u at %08x (srr1 %08x), data address %08x, dsisr %08x, lr %08x, sp %08x",
			(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08), (unsigned)so_get_u32(p + 0x0C),
			(unsigned)so_get_u32(p + 0x10), (unsigned)so_get_u32(p + 0x14), (unsigned)so_get_u32(p + 0x18),
			(unsigned)so_get_u32(p + 0x1C));
		so_log("CRASH: stack %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)so_get_u32(p + 0x20),
			(unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x28), (unsigned)so_get_u32(p + 0x2C),
			(unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34), (unsigned)so_get_u32(p + 0x38),
			(unsigned)so_get_u32(p + 0x3C));
		break;
	}
	case CMD_TEXT_REPORT: {
		static u32 reports;
		const u8 *p = request;
		char a[49], b[65];
		u8 kind = p[1];
		int i;
		memcpy(a, p + 0x10, 48);
		a[48] = '\0';
		memcpy(b, p + 0x40, 64);
		b[64] = '\0';
		for (i = 0; a[i]; i++)
			if (a[i] == '\n' || a[i] == '\r')
				a[i] = ' ';
		for (i = 0; b[i]; i++)
			if (b[i] == '\n' || b[i] == '\r')
				b[i] = ' ';
		if (kind == 1 || kind == 2) {
			u32 n;
			for (n = archive_count > 4 ? archive_count - 4 : 0; n < archive_count; n++)
				log_archive(n);
			x.stats.panics++;
			so_log("PANIC (%s) %s:%d from %08x: %s", kind == 1 ? "HSD_Panic" : "OSPanic", a,
				(int)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08), b);
		} else if (reports++ < 60) {
			so_log("OSReport from %08x: %s | %s", (unsigned)so_get_u32(p + 0x08), a, b);
		}
		break;
	}
	case CMD_PROBE: {
		static u32 probes, last_us, bad;
		const u8 *p = request;
		if (p[1] == 2) {
			u32 i = archive_count++ % 4;
			strlcpy(archives[i].file, last_file, sizeof(archives[i].file));
			archives[i].data = so_get_u32(p + 0x08);
			archives[i].length = so_get_u32(p + 0x0C);
			archives[i].caller = so_get_u32(p + 0x10);
			archives[i].header[0] = so_get_u32(p + 0x18);
			archives[i].header[1] = so_get_u32(p + 0x1C);
			archives[i].header[2] = so_get_u32(p + 0x20);
			/* An archive starts with its own size: flag any that does not match. */
			if (archives[i].header[0] != archives[i].length)
				log_archive(archive_count - 1);
			break;
		}
		u32 us = so_get_u32(p + 0x14);
		s32 n_us = (s32)so_get_u32(p + 0x1C), n_jp = (s32)so_get_u32(p + 0x18);
		probes++;
		/* The first ones, every change of table, and the first bad ones. */
		if (probes <= 6 || us != last_us || ((n_us < 0 || n_jp < 0) && bad++ < 6))
			so_log("PROBE %u: toy(%d, %u) from %08x: jp %08x n%d [0]=%08x | us %08x n%d [0]=%08x [1]=%08x | r13 %08x",
				(unsigned)probes, (int)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x10), (int)n_jp,
				(unsigned)so_get_u32(p + 0x28), (unsigned)us, (int)n_us, (unsigned)so_get_u32(p + 0x20),
				(unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x2C));
		last_us = us;
		break;
	}
	case CMD_HEARTBEAT: {
		/* tools/ppc/heartbeat.s: logged when the main thread moved, and
		 * every 16th. */
		static u32 last_main_pc;
		const u8 *p = request;
		u32 main_pc = so_get_u32(p + 0x20), tag = so_get_u32(p + 0x2C);
		if (tag >> 24 == 'S') {
			/* tools/ppc/stall.s: the game loop overran a frame */
			so_log("STALL vi %u logic %u: %s %u retraces | cur %08x main st %u pc %08x lr %08x",
				(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x28), (tag >> 16 & 0xFF) ? "ended after" : "for",
				(unsigned)(tag & 0xFFFF), (unsigned)so_get_u32(p + 0x18), (unsigned)(so_get_u32(p + 0x1C) >> 16),
				(unsigned)main_pc, (unsigned)so_get_u32(p + 0x24));
			so_log("STALL   at %08x lr %08x < %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x40), (unsigned)so_get_u32(p + 0x44),
				(unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x4C), (unsigned)so_get_u32(p + 0x50),
				(unsigned)so_get_u32(p + 0x54), (unsigned)so_get_u32(p + 0x58), (unsigned)so_get_u32(p + 0x5C));
			break;
		}
		x.stats.heartbeats++;
		x.stats.heartbeat_pc = main_pc;
		if (main_pc != last_main_pc || x.stats.heartbeats % 16 == 1) {
			so_log("PPC vi %u scene %08x | main st %u pc %08x lr %08x < %08x %08x %08x %08x %08x %08x",
				(unsigned)so_get_u32(p + 0x04), (unsigned)so_get_u32(p + 0x14), (unsigned)(so_get_u32(p + 0x1C) >> 16),
				(unsigned)main_pc, (unsigned)so_get_u32(p + 0x24), (unsigned)so_get_u32(p + 0x40),
				(unsigned)so_get_u32(p + 0x44), (unsigned)so_get_u32(p + 0x48), (unsigned)so_get_u32(p + 0x4C),
				(unsigned)so_get_u32(p + 0x50), (unsigned)so_get_u32(p + 0x54));
			so_log("PPC   irq at %08x lr %08x < %08x %08x | current thread %08x", (unsigned)so_get_u32(p + 0x08),
				(unsigned)so_get_u32(p + 0x0C), (unsigned)so_get_u32(p + 0x30), (unsigned)so_get_u32(p + 0x34),
				(unsigned)so_get_u32(p + 0x18));
		}
		last_main_pc = main_pc;
		break;
	}
	case CMD_GET_DELAY:
		answer(2)[0] = 1;	/* a real answer: no extra delay offline */
		break;
	case CMD_SET_RUMBLE:	/* 0xFF: just the setting */
		answer(1)[0] = (u8)so_client_set_rumble(request[1]);
		break;
	case CMD_SET_DELAY:	/* the delay goes with the connection and the ping: no steps */
		answer(1)[0] = so_client_config()->delay;
		break;
	case CMD_PREMADE_TEXT_LENGTH:
	case CMD_PREMADE_TEXT_LOAD:
		on_premade_text(request, cmd == CMD_PREMADE_TEXT_LOAD);
		break;
	case CMD_GET_RANK:
		answer(16)[1] = 1;	/* hidden, fetched, unranked */
		break;
	case CMD_REPORT_GAME:
		log_game();	/* the game is over */
		break;
	case CMD_OPEN_LOGIN:
	case CMD_LOGOUT:
	case CMD_UPDATE:
	case CMD_OVERWRITE_SELECTIONS:
	case CMD_GP_COMPLETE_STEP:
	case CMD_REPORT_SET_COMPLETE:
	case CMD_REPORT_MATCH_STATUS_UPDATE:
	case CMD_PLAY_MUSIC:
	case CMD_STOP_MUSIC:
	case CMD_CHANGE_MUSIC_VOLUME:
	case CMD_FETCH_RANK:
	case CMD_GET_RANK_VISIBILITY:
		break;	/* nothing to do on a console (see the spec) */
	default:
		x.stats.unknown++;
		break;
	}
}

u32 so_exi_read(u8 *out, u32 len)
{
	u32 n;
	x.stats.reads++;
	x.answer_read = 1;
	if (!x.has_resp)
		return 0;	/* Dolphin writes nothing then */
	if (x.file_pending) {
		/* Into the game's buffer, a piece at a time: these files reach 2 MB. */
		u64 start = so_time_us();
		n = so_files_read(x.file, out, len);
		memset(out + n, 0, len - n);
		/* An archive starts with its own length: a cheap check of the read. */
		x.stats.file_loads++;
		so_log("exi: loaded %s at %p: %u of %u bytes (read %u) in %u ms, header %08x", x.file, out, (unsigned)n,
			(unsigned)x.resp_len, (unsigned)len, (unsigned)((so_time_us() - start) / 1000),
			(unsigned)(n >= 4 ? so_get_u32(out) : 0));
		return len;
	}
	n = x.resp_len < len ? x.resp_len : len;
	memcpy(out, x.resp, n);
	memset(out + n, 0, len - n);
	if (s_restore_outer) {
		/* the inner call has its answer: the outer call's is next */
		s_restore_outer = 0;
		memcpy((u8 *)x.resp, s_outer_b0, RXB_SIZE);
		x.answer_read = 0;
	}
	if (x.stats.last_command == CMD_GCT_LOAD)
		so_log("exi: codeset at %p (read %u)", out, (unsigned)len);
	return len;
}

void so_exi_stats(SoExiStats *out)
{
	*out = x.stats;
}
