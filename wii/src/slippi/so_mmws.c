/* slippi_online/so_mmws.c
 * The matchmaking service session (see so_mmws.h). Its timings are the
 * Rust client's (client.rs's constants).
 */
#include <malloc.h>
#include <stdlib.h>

#include "so_mmws.h"
#include "so_json.h"
#include "so_platform.h"
#include "so_tls.h"
#include "so_util.h"
#include "so_ws.h"
#include "string.h"

#define CONNECT_ATTEMPTS 3
#define RETRY_DELAY_MS 1000
#define HANDSHAKE_MS 10000
#define POLL_MS 250
#define JOIN_TIMEOUT_MS 5000
#define PING_AFTER_MS 5000
#define LIVENESS_MS 15000
#define RESUME_WINDOW_MS 15000

#define JOIN_MAX 2048
#define RESULT_MAX SO_WS_MESSAGE_MAX

static SoMutex s_lock;
static SoEvent s_wake;
static int s_started;
static SoTls *s_tls;
static SoWs *s_ws;

/* the request (the lock held to touch) */
static volatile u32 s_session;	/* the newest session's id */
static volatile u32 s_cancelled;	/* the id of the last cancelled one */
static char s_host[64], s_path[64], s_join[JOIN_MAX];
static u32 s_addr;
static u16 s_port;
static int s_use_tls;

/* the outcome */
static volatile SoMmwsState s_state = SO_MMWS_IDLE;
static char *s_result;
static int s_result_len;
static char s_error[128];

static int stopped(u32 id)
{
	return s_session != id || s_cancelled == id;
}

static void set_state(u32 id, SoMmwsState state)
{
	so_mutex_lock(&s_lock);
	if (s_session == id)
		s_state = state;
	so_mutex_unlock(&s_lock);
}

static void set_failed(u32 id, const char *text)
{
	so_mutex_lock(&s_lock);
	if (s_session == id) {
		strlcpy(s_error, text, sizeof(s_error));
		s_state = SO_MMWS_FAILED;
	}
	so_mutex_unlock(&s_lock);
	so_log("mm: %s", text);
}

/* Sleeps up to `ms`, waking early when the session is stopped */
static void nap(u32 id, u32 ms)
{
	u32 start = so_time_ms();
	while (!stopped(id) && so_time_ms() - start < ms)
		so_sleep_us(50000);
}

enum { RUN_LOST, RUN_DONE, RUN_CONNECT_FAILED };

/* One connection: connect, upgrade, send `first`, then serve it until it is
 * lost or the session ends. */
static int run_connection(u32 id, const char *first, char *ticket, int ticket_size, char *token,
	int token_size)
{
	u32 opened, last_frame;
	int joined = 0, pinged = 0;
	if (so_tls_open(s_tls, s_host, s_addr, s_port, s_use_tls, HANDSHAKE_MS) < 0) {
		so_log("mm: connecting to the service failed (%s)", so_tls_error_text(s_tls));
		return RUN_CONNECT_FAILED;
	}
	if (so_ws_open(s_ws, s_tls, s_host, s_path, HANDSHAKE_MS) < 0 ||
		so_ws_send_text(s_ws, first, (u32)strlen(first)) < 0) {
		so_log("mm: the service's upgrade failed (%s)", so_tls_error_text(s_tls));
		so_tls_close(s_tls);
		return RUN_CONNECT_FAILED;
	}
	opened = last_frame = so_time_ms();
	for (;;) {
		int res;
		u32 now;
		if (stopped(id)) {
			static const char cancel[] = "{\"type\":\"cancel\"}";
			so_ws_send_text(s_ws, cancel, sizeof(cancel) - 1);
			so_ws_send_close(s_ws);
			so_tls_close(s_tls);
			return RUN_DONE;
		}
		res = so_ws_poll(s_ws, POLL_MS);
		now = so_time_ms();
		if (res == SO_WS_MESSAGE) {
			SoJson doc;
			char type[24];
			last_frame = now;
			pinged = 0;
			if (so_json_parse(&doc, s_ws->msg, s_ws->msg_len) < 0 || so_json_type(&doc) != SO_JSON_OBJECT)
				continue;	/* unreadable: ignored, as the Rust client does */
			so_json_get_string(&doc, "type", type, sizeof(type), "");
			if (strcmp(type, "joined") == 0) {
				so_json_get_string(&doc, "ticketId", ticket, ticket_size, "");
				so_json_get_string(&doc, "token", token, token_size, "");
				if (!joined)
					so_log("mm: in queue");
				joined = 1;
				set_state(id, SO_MMWS_QUEUED);
			} else if (strcmp(type, "status") == 0) {
				/* (seconds in queue: nothing to do) */
			} else if (strcmp(type, "match") == 0) {
				so_mutex_lock(&s_lock);
				if (s_session == id && s_result != NULL) {
					memcpy(s_result, s_ws->msg, s_ws->msg_len + 1);
					s_result_len = (int)s_ws->msg_len;
					s_state = SO_MMWS_MATCHED;
				}
				so_mutex_unlock(&s_lock);
				so_ws_send_close(s_ws);
				so_tls_close(s_tls);
				return RUN_DONE;
			} else if (strcmp(type, "error") == 0) {
				char text[128];
				so_json_get_string(&doc, "error", text, sizeof(text), "Received an error from the mm server");
				set_failed(id, text);
				so_ws_send_close(s_ws);
				so_tls_close(s_tls);
				return RUN_DONE;
			}
		} else if (res == SO_WS_FRAME) {
			last_frame = now;
			pinged = 0;
		} else if (res == SO_WS_CLOSED) {
			so_log("mm: the service closed the connection");
			break;
		}
		if (!joined && now - opened >= JOIN_TIMEOUT_MS) {
			if (!ticket[0]) {
				set_failed(id, "Failed to join mm queue");
				so_tls_close(s_tls);
				return RUN_DONE;
			}
			break;	/* a resume not answered: lost */
		}
		if (now - last_frame >= LIVENESS_MS)
			break;
		if (now - last_frame >= PING_AFTER_MS && !pinged) {
			static const char ping[] = "{\"type\":\"ping\"}";
			so_ws_send_text(s_ws, ping, sizeof(ping) - 1);
			pinged = 1;
		}
	}
	so_tls_close(s_tls);
	return RUN_LOST;
}

static void run_session(u32 id)
{
	char ticket[96] = "", token[256] = "", resume[512];
	u32 lost_deadline = 0;
	int attempts = 0;
	set_state(id, SO_MMWS_CONNECTING);
	so_log("mm: connecting to %s%s", s_host, s_path);
	while (!stopped(id)) {
		const char *first = s_join;
		int res;
		if (ticket[0]) {
			so_snprintf(resume, sizeof(resume), "{\"type\":\"resume\",\"ticketId\":\"%s\",\"token\":\"%s\"}",
				ticket, token);
			first = resume;
		}
		res = run_connection(id, first, ticket, sizeof(ticket), token, sizeof(token));
		if (res == RUN_DONE)
			break;
		if (!ticket[0]) {
			if (res == RUN_LOST) {
				set_failed(id, "Lost connection to the mm server");
				break;
			}
			if (++attempts >= CONNECT_ATTEMPTS) {
				set_failed(id, "Failed to connect to mm server");
				break;
			}
		} else {
			if (lost_deadline == 0) {
				lost_deadline = so_time_ms() + RESUME_WINDOW_MS;
				so_log("mm: connection lost; resuming");
			}
			if ((s32)(so_time_ms() - lost_deadline) >= 0) {
				set_failed(id, "Lost connection to the mm server");
				break;
			}
		}
		nap(id, RETRY_DELAY_MS);
	}
	so_mutex_lock(&s_lock);
	if (s_session == id && s_state != SO_MMWS_MATCHED && s_state != SO_MMWS_FAILED)
		s_state = SO_MMWS_IDLE;
	so_mutex_unlock(&s_lock);
}

static u32 SessionThread(void *unused)
{
	u32 done = 0;
	(void)unused;
	for (;;) {
		u32 id = s_session;
		if (id == done || s_cancelled == id) {
			so_event_wait(&s_wake, 500000);
			continue;
		}
		done = id;
		run_session(id);
	}
	return 0;
}

int so_mmws_start(const char *host, u32 addr, u16 port, const char *path, int tls, const char *join)
{
	if (!s_started) {
		/* (the first search: well after the game's ARAM is taken) */
		s_tls = (SoTls *)memalign(32, sizeof(SoTls));
		s_ws = (SoWs *)malloc(sizeof(SoWs));
		s_result = (char *)malloc(RESULT_MAX + 1);
		if (!s_tls || !s_ws || !s_result || so_mutex_init(&s_lock) < 0 || so_event_init(&s_wake) < 0) {
			free(s_tls);
			free(s_ws);
			free(s_result);
			s_tls = NULL;
			s_ws = NULL;
			s_result = NULL;
			return -1;
		}
		if (so_thread_start(SessionThread, NULL, NULL, 64 * 1024, 0) < 0)
			return -2;
		s_started = 1;
	}
	if (strlen(join) >= sizeof(s_join))
		return -3;
	so_mutex_lock(&s_lock);
	strlcpy(s_host, host, sizeof(s_host));
	strlcpy(s_path, path, sizeof(s_path));
	strlcpy(s_join, join, sizeof(s_join));
	s_addr = addr;
	s_port = port;
	s_use_tls = tls;
	s_result_len = -1;
	s_error[0] = '\0';
	s_state = SO_MMWS_CONNECTING;
	s_session++;
	so_mutex_unlock(&s_lock);
	so_event_signal(&s_wake);
	return 0;
}

void so_mmws_cancel(void)
{
	if (!s_started)
		return;
	so_mutex_lock(&s_lock);
	s_cancelled = s_session;
	if (s_state == SO_MMWS_CONNECTING || s_state == SO_MMWS_QUEUED)
		s_state = SO_MMWS_IDLE;
	so_mutex_unlock(&s_lock);
	so_event_signal(&s_wake);
}

SoMmwsState so_mmws_state(void)
{
	return s_state;
}

int so_mmws_take_result(char *out, int size)
{
	int len;
	so_mutex_lock(&s_lock);
	len = s_result_len;
	if (len >= 0 && len < size)
		memcpy(out, s_result, (u32)len + 1);
	else
		len = -1;
	s_result_len = -1;
	so_mutex_unlock(&s_lock);
	return len;
}

void so_mmws_error(char *out, int size)
{
	so_mutex_lock(&s_lock);
	strlcpy(out, s_error[0] ? s_error : "Matchmaking was cancelled", size);
	so_mutex_unlock(&s_lock);
}
