/* so_platform.h for the Wii port: the online client runs on the PowerPC
 * beside the game (in Slippi Nintendont it runs on the Starlet, in IOS's
 * place). Threads, locks and events are libogc's; files are on the SD card
 * or a USB drive (libfat); the NAND through ISFS; the disc is the port's
 * (wii/src/dvd.c). */

#include <dirent.h>
#include <gccore.h>
#include <ogc/cond.h>
#include <malloc.h>
#include <ogc/isfs.h>
#include <ogc/lwp_watchdog.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <wl/base/base.h>

#include "so_gamelog.h"
#include "so_platform.h"
#include "so_util.h"
#include "string.h"

/* (wii/src/melee_wii.h's MELEE_WII_PRIO_IO, the port's I/O threads') */
#define SO_THREAD_PRIORITY 110

/* kernel.ld's network thread stack in Slippi Nintendont: here the thread
 * gets one from libogc (so_thread_start), so these only have to exist */
char __slippi_online_stack_addr, __slippi_online_stack_size;
char __slippi_online_diag_stack_addr, __slippi_online_diag_stack_size;
char __slippi_online_test_stack_addr, __slippi_online_test_stack_size;

volatile SoTrace so_traces[SO_THREAD_COUNT];
volatile u32 so_timer_failures;

static u32 s_random = 0x2545F491;
static char s_status[32];
static char s_root[96] = "sd:/apps/melee/slippi";
static mutex_t s_fs_lock = LWP_MUTEX_NULL;

/* Where the client's files are ("/slippi/..." in its paths): the
 * launcher sets the folder it found or made */
void so_platform_set_root(const char* root)
{
    strlcpy(s_root, root, sizeof(s_root));
}

const char* so_platform_root(void)
{
    return s_root;
}

/* "/slippi/x" -> <root>/x; other paths stay (they name a device) */
static void map_path(const char* path, char* full, int size)
{
    static const char prefix[] = "/slippi/";
    if (strncmp(path, prefix, sizeof(prefix) - 1) == 0) {
        so_snprintf(full, size, "%s/%s", s_root, path + sizeof(prefix) - 1);
    } else if (strcmp(path, "/slippi") == 0) {
        so_snprintf(full, size, "%s", s_root);
    } else {
        so_snprintf(full, size, "%s", path);
    }
}

/* ---- time ---------------------------------------------------------------- */

u32 so_ticks(void)
{
    return (u32) gettime();
}

u32 so_ticks_to_ms(u32 ticks)
{
    return ticks / (TB_TIMER_CLOCK);
}

u64 so_time_us(void)
{
    return ticks_to_microsecs(gettime());
}

u32 so_time_ms(void)
{
    return (u32) (so_time_us() / 1000);
}

void so_sleep_us(u32 us)
{
    if (us == 0) {
        LWP_YieldThread();
        return;
    }
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

u32 so_random(void)
{
    u32 x = s_random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_random = x;
}

/* ---- locks and events ---------------------------------------------------- */

int so_mutex_init(SoMutex* m)
{
    mutex_t id;
    if (LWP_MutexInit(&id, false) != 0) {
        return -1;
    }
    m->queue = (s32) id;
    return 0;
}

void so_mutex_lock(SoMutex* m)
{
    LWP_MutexLock((mutex_t) m->queue);
}

void so_mutex_unlock(SoMutex* m)
{
    LWP_MutexUnlock((mutex_t) m->queue);
}

/* An event: a flag, under a mutex, with a condition to wait on (the mutex
 * in slot[0], the condition in slot[1] of the IOS layout's slots) */
int so_event_init(SoEvent* e)
{
    mutex_t mtx;
    cond_t cond;
    if (LWP_MutexInit(&mtx, false) != 0 || LWP_CondInit(&cond) != 0) {
        return -1;
    }
    e->queue = (s32) mtx;
    e->slots[0] = (u32) cond;
    e->pending = 0;
    return 0;
}

void so_event_signal(SoEvent* e)
{
    LWP_MutexLock((mutex_t) e->queue);
    e->pending = 1;
    LWP_CondSignal((cond_t) e->slots[0]);
    LWP_MutexUnlock((mutex_t) e->queue);
}

int so_event_wait(SoEvent* e, u32 timeout_us)
{
    int signalled;
    LWP_MutexLock((mutex_t) e->queue);
    if (!e->pending) {
        if (timeout_us == 0) {
            while (!e->pending) {
                LWP_CondWait((cond_t) e->slots[0], (mutex_t) e->queue);
            }
        } else {
            struct timespec ts = { timeout_us / 1000000, (timeout_us % 1000000) * 1000 };
            LWP_CondTimedWait((cond_t) e->slots[0], (mutex_t) e->queue, &ts);
        }
    }
    signalled = e->pending != 0;
    e->pending = 0;
    LWP_MutexUnlock((mutex_t) e->queue);
    return signalled;
}

/* ---- threads ------------------------------------------------------------- */

typedef struct {
    u32 (*entry)(void*);
    void* arg;
} Launch;

static void* launch(void* p)
{
    Launch l = *(Launch*) p;
    free(p);
    return (void*) (uintptr_t) l.entry(l.arg);
}

/* IOS's priorities (higher runs first, the client's at 0x78) become
 * libogc's: above the game's own threads, so the network keeps up while the
 * game waits on it */
s32 so_thread_start(u32 (*entry)(void*), void* arg, void* stack_top, u32 stack_size,
                    u32 priority)
{
    lwp_t thread;
    Launch* l = malloc(sizeof(*l));
    (void) stack_top;
    (void) stack_size; /* (the address of a linker symbol in the kernel) */
    (void) priority;
    if (l == NULL) {
        return -1;
    }
    l->entry = entry;
    l->arg = arg;
    if (LWP_CreateThread(&thread, launch, l, NULL, 64 * 1024, SO_THREAD_PRIORITY) != 0)
    {
        free(l);
        return -1;
    }
    return (s32) thread;
}

void so_capture_main_thread(void) {}

int so_set_main_thread_priority(int priority)
{
    (void) priority;
    return 0;
}

/* ---- log ----------------------------------------------------------------- */

void melee_wii_log_point(void); /* wii/src/main.c */

void so_log(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    so_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    wl_log_write(WL_LOG_LEVEL_INFO, "slippi", "%s", line);
    melee_wii_log_point();
    so_gamelog_event('L', 0);
}

int so_log_take(char* out, int size)
{
    (void) out;
    (void) size;
    return 0; /* written to the port's log as they come */
}

void so_set_status(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    so_vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

const char* so_status(void)
{
    return s_status;
}

/* ---- files --------------------------------------------------------------- */

void so_fs_lock(void)
{
    if (s_fs_lock != LWP_MUTEX_NULL) {
        LWP_MutexLock(s_fs_lock);
    }
}

void so_fs_unlock(void)
{
    if (s_fs_lock != LWP_MUTEX_NULL) {
        LWP_MutexUnlock(s_fs_lock);
    }
}

int so_read_file(const char* path, void* buf, u32 size)
{
    char full[200];
    FILE* f;
    int n;
    map_path(path, full, sizeof(full));
    so_fs_lock();
    f = fopen(full, "rb");
    if (f == NULL) {
        so_fs_unlock();
        return -1;
    }
    n = (int) fread(buf, 1, size, f);
    fclose(f);
    so_fs_unlock();
    return n;
}

int so_write_file(const char* path, const void* buf, u32 size)
{
    char full[200];
    FILE* f;
    int ok;
    map_path(path, full, sizeof(full));
    so_fs_lock();
    f = fopen(full, "wb");
    if (f == NULL) {
        so_fs_unlock();
        return -1;
    }
    ok = fwrite(buf, 1, size, f) == size;
    fclose(f);
    so_fs_unlock();
    return ok ? 0 : -2;
}

int so_read_file_bounced(const char* path, u8* out, u32 size, u8* bounce, u32 bounce_size)
{
    (void) bounce;
    (void) bounce_size;
    return so_read_file(path, out, size);
}

void* so_file_open(const char* path, int* error)
{
    char full[200];
    FILE* f;
    map_path(path, full, sizeof(full));
    so_fs_lock();
    f = fopen(full, "rb");
    so_fs_unlock();
    *error = f != NULL ? 0 : 4; /* (FatFS's FR_NO_FILE) */
    return f;
}

int so_file_read_at(void* file, u32 offset, u8* out, u32 size, u8* bounce, u32 bounce_size)
{
    int n;
    (void) bounce;
    (void) bounce_size;
    so_fs_lock();
    if (fseek((FILE*) file, (long) offset, SEEK_SET) != 0) {
        so_fs_unlock();
        return -1001;
    }
    n = (int) fread(out, 1, size, (FILE*) file);
    so_fs_unlock();
    return n;
}

void* so_file_create(const char* path)
{
    char full[200];
    FILE* f;
    map_path(path, full, sizeof(full));
    so_fs_lock();
    f = fopen(full, "wb");
    so_fs_unlock();
    return f;
}

int so_file_write(void* file, const void* buf, u32 size)
{
    int ok;
    so_fs_lock();
    ok = fwrite(buf, 1, size, (FILE*) file) == size;
    so_fs_unlock();
    return ok ? 0 : -1;
}

int so_file_write_at(void* file, u32 offset, const void* buf, u32 size)
{
    int ok;
    so_fs_lock();
    ok = fseek((FILE*) file, (long) offset, SEEK_SET) == 0 && fwrite(buf, 1, size, (FILE*) file) == size &&
         fseek((FILE*) file, 0, SEEK_END) == 0;
    so_fs_unlock();
    return ok ? 0 : -1;
}

void so_file_close(void* file)
{
    so_fs_lock();
    fclose((FILE*) file);
    so_fs_unlock();
}

int so_list_dir(const char* path, void (*fn)(const char* name, u32 size, void* ctx), void* ctx)
{
    char full[200];
    char file[300];
    DIR* dir;
    struct dirent* entry;
    int n = 0;
    map_path(path, full, sizeof(full));
    so_fs_lock();
    dir = opendir(full);
    if (dir == NULL) {
        so_fs_unlock();
        return -1;
    }
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        if (entry->d_name[0] == '.') {
            continue;
        }
        so_snprintf(file, sizeof(file), "%s/%s", full, entry->d_name);
        if (stat(file, &st) != 0 || S_ISDIR(st.st_mode)) {
            continue;
        }
        fn(entry->d_name, (u32) st.st_size, ctx);
        n++;
    }
    closedir(dir);
    so_fs_unlock();
    return n;
}

int so_read_nand(const char* path, void* buf, u32 size)
{
    static u8 bounce[8192] __attribute__((aligned(32))); /* (config.dat's three slots: 0x1B5C) */
    s32 fd;
    s32 n;
    if (size > sizeof(bounce)) {
        size = sizeof(bounce);
    }
    if (ISFS_Initialize() < 0) {
        return -1;
    }
    fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0) {
        return -1;
    }
    n = ISFS_Read(fd, bounce, size);
    ISFS_Close(fd);
    if (n > 0) {
        memcpy(buf, bounce, n);
    }
    return n;
}

/* ---- the game and the disc ----------------------------------------------- */

u32 so_game_word(u32 address)
{
    if (address < 0x80000000 || address >= 0x81800000) {
        return 0;
    }
    return *(volatile u32*) address;
}

void so_exi_debug(u32 out[9])
{
    memset(out, 0, 9 * sizeof(u32));
}

/* wii/src/dvd.c: bytes of the disc image */
int melee_wii_disc_read(u32 offset, void* buf, u32 len);

int so_disc_read(u32 offset, void* buf, u32 len)
{
    return melee_wii_disc_read(offset, buf, len);
}

void* so_platform_file_cache(u32* size)
{
    static void* cache;
    *size = SO_FILE_CACHE_SIZE;
    if (cache == NULL) {
        cache = memalign(32, SO_FILE_CACHE_SIZE); /* (MEM2: newlib's malloc is there) */
    }
    if (cache == NULL) {
        *size = 0;
    }
    return cache;
}

void* so_platform_arena(u32* size)
{
    *size = 512 * 1024;
    return memalign(32, *size);
}

int so_platform_init(void)
{
    if (s_fs_lock == LWP_MUTEX_NULL) {
        LWP_MutexInit(&s_fs_lock, true);
    }
    s_random ^= (u32) gettime() ^ ((u32) (gettime() >> 32) << 16);
    return 0;
}
