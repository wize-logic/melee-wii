/* so_platform.h on Linux, for the host tests: only what the matchmaking,
 * TLS and update code uses */
#define _GNU_SOURCE
#include <netdb.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "so_platform.h"
#include "so_util.h"

volatile SoTrace so_traces[SO_THREAD_COUNT];
volatile u32 so_timer_failures;

u64 so_time_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}
u32 so_time_ms(void) { return (u32)(so_time_us() / 1000); }
void so_sleep_us(u32 us) { usleep(us); }
u32 so_random(void) { return (u32)random(); }
u64 so_timebase(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}
u32 so_unix_time(void) { return (u32)time(NULL); }
void so_platform_entropy_seed(u8 *out, u32 len)
{
	FILE *f = fopen("/dev/urandom", "rb");
	if (!f || fread(out, 1, len, f) != len)
		abort();
	fclose(f);
}

/* (SoMutex's queue holds the pthread mutex's index) */
static pthread_mutex_t s_mutexes[64];
static int s_mutex_count;
int so_mutex_init(SoMutex *m)
{
	m->queue = s_mutex_count++;
	pthread_mutex_init(&s_mutexes[m->queue], NULL);
	return 0;
}
void so_mutex_lock(SoMutex *m) { pthread_mutex_lock(&s_mutexes[m->queue]); }
void so_mutex_unlock(SoMutex *m) { pthread_mutex_unlock(&s_mutexes[m->queue]); }

typedef struct { u32 (*entry)(void *); void *arg; } Start;
static void *thread_main(void *p)
{
	Start s = *(Start *)p;
	free(p);
	s.entry(s.arg);
	return NULL;
}
s32 so_thread_start(u32 (*entry)(void *), void *arg, void *stack_top, u32 stack_size, u32 priority)
{
	pthread_t t;
	Start *s = malloc(sizeof(*s));
	(void)stack_top; (void)stack_size; (void)priority;
	s->entry = entry;
	s->arg = arg;
	if (pthread_create(&t, NULL, thread_main, s) != 0)
		return -1;
	pthread_detach(t);
	return 1;
}

void so_log(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	so_vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("[log] %s\n", line);
	fflush(stdout);
}

/* (Linux's DNS needs nothing noted: so_resolve below always works) */
void so_socket_note_net_up(void)
{
}

int so_resolve(const char *host, u32 *addr)
{
	struct addrinfo hints, *res;
	if (so_parse_ipv4(host, addr) == 0)
		return 0;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(host, NULL, &hints, &res) != 0)
		return -2;
	*addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(res);
	return 0;
}

/* SoEvent's queue holds the index of a condition variable */
static struct { pthread_mutex_t m; pthread_cond_t c; int set; } s_events[16];
static int s_event_count;
int so_event_init(SoEvent *e)
{
	e->queue = s_event_count++;
	pthread_mutex_init(&s_events[e->queue].m, NULL);
	pthread_cond_init(&s_events[e->queue].c, NULL);
	return 0;
}
void so_event_signal(SoEvent *e)
{
	pthread_mutex_lock(&s_events[e->queue].m);
	s_events[e->queue].set = 1;
	pthread_cond_signal(&s_events[e->queue].c);
	pthread_mutex_unlock(&s_events[e->queue].m);
}
int so_event_wait(SoEvent *e, u32 timeout_us)
{
	struct timespec ts;
	int got;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += timeout_us / 1000000;
	ts.tv_nsec += (long)(timeout_us % 1000000) * 1000;
	if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
	pthread_mutex_lock(&s_events[e->queue].m);
	while (!s_events[e->queue].set)
		if (pthread_cond_timedwait(&s_events[e->queue].c, &s_events[e->queue].m, &ts) != 0)
			break;
	got = s_events[e->queue].set;
	s_events[e->queue].set = 0;
	pthread_mutex_unlock(&s_events[e->queue].m);
	return got;
}

int so_read_file(const char *path, void *buf, u32 size)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f)
		return -1;
	n = fread(buf, 1, size, f);
	fclose(f);
	return (int)n;
}
