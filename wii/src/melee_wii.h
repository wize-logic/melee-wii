#ifndef MELEE_WII_H
#define MELEE_WII_H

/* What the Wii port's shims share (wii/src) */

/* os_thread.c: the main thread's OSThread; from OSInit */
void melee_wii_thread_init(void);

/* main.c: leaves for the loader (the Homebrew Channel) */
__attribute__((noreturn)) void melee_wii_exit(void);
/* main.c: a stop screen with the message, then leaves */
__attribute__((noreturn, format(printf, 1, 2))) void
melee_wii_fatal(const char* fmt, ...);

/* Work deferred to interrupt context (a tick task on the decrementer, as
 * libogc's alarms are). The SDK delivers its DVD and ARAM completions from
 * interrupts, which a thread that disabled them holds off; libogc switches
 * to a woken thread at once, whatever the waker's interrupts. So the shims
 * wake their threads and call the game's callbacks from here: never while
 * the game's thread has interrupts disabled. Armed once until it runs; `fn`
 * runs with interrupts disabled. */
typedef struct MeleeWiiDefer {
    unsigned long long task[3]; /* tuxedo's KTickTask */
    void (*fn)(void* arg);
    void* arg;
    volatile int armed;
} MeleeWiiDefer;

void melee_wii_defer(MeleeWiiDefer* defer);

/* pad.c: the game enters a scene (gm/gm_1A3F.c), for the test input script */
void melee_wii_scene(int kind);

/* stall.c: a report in the log, by the system watch thread, when the game
 * shows no new frame for long, with where its main thread waits (the shims'
 * blocking calls mark it, with their own stack frame for the call chain):
 * what a game that stops on a Wii, with no debugger, leaves */
enum {
    MELEE_WII_WAIT_SLEEP = 1, /* OSSleepThread: `object` its queue */
    MELEE_WII_WAIT_RETRACE,   /* VIWaitForRetrace */
};
void melee_wii_wait_begin(int kind, const void* object, void* frame);
void melee_wii_wait_end(void);
/* OSSuspendThread/OSResumeThread of a thread (its LWP), `by` the caller */
void melee_wii_wait_suspend(unsigned int lwp, int suspended, void* by);
void melee_wii_stall_start(void); /* main.c: the game starts (main thread) */
void melee_wii_stall_frame(void); /* vi.c: the game shows a new frame */
void melee_wii_stall_tick(void* user); /* the system watch thread's tick */
/* stall.c: a crash (an exception nothing handles) written to the log, then
 * libogc's crash screen, shown (main.c, at the start) */
void melee_wii_crash_install(void);

/* The priorities of the shims' worker threads (libogc's, 127 highest; the
 * main thread runs at 64): above the game's, like the interrupts they stand
 * for */
#define MELEE_WII_PRIO_IO 100


/* The floating-point unit as the GameCube's SDK runs the game: IEEE mode
 * (denormals kept; libogc starts with FPSCR[NI], which flushes them to zero),
 * round to nearest, no exceptions. The game's results have to be the
 * GameCube's bit for bit (online play), so every thread that runs its code
 * starts with this. */
static inline void melee_wii_fpscr(void)
{
    double zero = 0.0;
    __asm__ volatile("mtfsf 255, %0" ::"f"(zero));
}


/* main.c (MELEE_ORIG_CODE): code in MEM1 within the game's reach, and a
 * branch (link: bl) at a site of the game's code to a function anywhere */
void* melee_wii_island_alloc(unsigned int size);
/* main.c (MELEE_ORIG_CODE): Gecko code's calls by address to the game's
 * memset and memcpy (where a Wii keeps its globals) moved to where the port
 * runs them; how many */
int melee_wii_libc_fix_code(unsigned int* words, unsigned int count);
/* main.c: the branches at the three's old starts, put back if changed; and
 * where code that runs at their old place runs now (0: not there) */
void melee_wii_libc_guard(void);
unsigned int melee_wii_libc_moved(unsigned int pc);
void melee_wii_put_branch(unsigned int site, void* target, unsigned int link);

/* gecko.c (MELEE_ORIG_CODE): Gecko codes put in once (Slippi's), and the
 * player's cheats, run each frame */
int melee_wii_gecko_apply(const unsigned char* gct, unsigned int len);
int melee_wii_gecko_cheats(const unsigned char* gct, unsigned int len);



/* main.c: the boot's log written at once (until the first scenes), and its
 * end */
void melee_wii_log_point(void);
void melee_wii_log_sync_end(void);

/* dvd.c: where the game's file data went (disc and ARAM reads), the last
 * ones: for the rollback measurement */
void melee_wii_note_load(unsigned int addr, unsigned int len);
unsigned int melee_wii_dvd_loads(const unsigned int** addr, const unsigned int** len);
/* mmu_track.c: memory written other than by the CPU, for rollback's page
 * tracking */
void melee_wii_mmu_touch(unsigned int addr, unsigned int len);
/* mmu_track.c: memory the GPU writes (texture copies), noted for rollback's
 * log (not marked: the savestates leave it out) */
void melee_wii_mmu_touch_gpu(unsigned int addr, unsigned int len);
/* mmu_track.c: the page table off, MEM1 by its block mapping again */
void melee_wii_mmu_track_stop(void);
/* copy.c: rollback's big copies, by the locked cache's DMA or the CPU:
 * (dst & 31) == (src & 31); melee_wii_copy between begin and end */
void melee_wii_copy_check(void* dst, const void* src, unsigned int len);
int melee_wii_copy_by_dma(void);
void melee_wii_copy_begin(void);
void melee_wii_copy(void* dst, const void* src, unsigned int len);
int melee_wii_copy_end(void); /* nonzero: the copies since begin are to be done again */
/* ax_wii.c: around a long time with interrupts off (a savestate's), the
 * audio DMA's next block silence rather than the last one again */
void melee_wii_audio_hold(void);
void melee_wii_audio_release(void);
/* ax_wii.c: for a shorter time with interrupts off, one after the other (a
 * rollback's load and savestates): the audio's pending interrupts handled
 * now (service, before and after each), and inside one, silence set as the
 * next block only once a block has begun unhandled (guard): no block is
 * lost unless two begin within one */
void melee_wii_audio_service(void);
void melee_wii_audio_guard(void);
void melee_wii_audio_counts(unsigned int* serviced, unsigned int* guarded, unsigned int* silent);
/* os_irq.c: the AI DMA's and the DSP's handlers run now if their interrupts
 * are pending (interrupts off); how many ran */
int melee_wii_irq_service_audio(void);
/* pad.c: that count when the current scene began */
unsigned int melee_wii_scene_first_load(void);
unsigned int melee_wii_scene_count(void);
extern void (*melee_wii_scene_hook)(int kind);

/* os.c: the bounds of HSD's audio heap (HSD_OSInit's first heap); 0, or -1
 * before it is made */
int melee_wii_audio_heap(unsigned int* lo, unsigned int* hi);

/* os.c: MEM1's free memory, kept for the game's arena (main.c, first) */
void melee_wii_reserve_arena(void);

/* wifi.c: WiiConnect24's scheduler paused while the game runs (main.c), the
 * Wi-Fi signal, and the Wi-Fi connection's rates and retries for this
 * session (so_socket_ogc.c, before the network starts) */
void melee_wii_wc24_suspend(void);
void melee_wii_wc24_resume(void);
const char* melee_wii_wc24_status(void);
int melee_wii_wifi_signal(void);
int melee_wii_wifi_tune(void* config, int rateset, int retry);
const char* melee_wii_wifi_report(void);

/* wifi.c: turn the Wi-Fi driver's roaming off (its ~10 s roam scan spikes
 * netplay latency), by commanding the driver over /dev/wl0. Ensures roam_off is
 * set and verified; returns 1 (off), 0 (on and could not be turned off), -1 (no
 * Wi-Fi driver, e.g. a wired link). The driver resets it on each (re)assoc, so
 * call it again while connected; run it off the netplay loop (it may block a
 * few seconds). so_socket_ogc.c's net thread keeps roaming off this way. */
int melee_wii_wifi_roam_guard(void);

#endif
