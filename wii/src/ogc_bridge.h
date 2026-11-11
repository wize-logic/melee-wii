#ifndef MELEE_WII_OGC_BRIDGE_H
#define MELEE_WII_OGC_BRIDGE_H

/* The libogc calls of the shims that implement the SDK's API. Those shims
 * include the SDK's headers, whose types (GXColor, Mtx, PADStatus, BOOL...)
 * libogc's own headers define again: the calls are declared here with the
 * types they have in libogc 3.1 (u32 is unsigned int on both sides). */

#include <stddef.h>
#include <stdint.h>

/* ogc/lwp.h */
typedef uint32_t ogc_lwp_t;
int32_t LWP_CreateThread(ogc_lwp_t* thethread, void* (*entry)(void*), void* arg,
                         void* stackbase, uint32_t stack_size, uint8_t prio);
int32_t LWP_SuspendThread(ogc_lwp_t thethread);
int32_t LWP_ResumeThread(ogc_lwp_t thethread);
ogc_lwp_t LWP_GetSelf(void);
void LWP_SetThreadPriority(ogc_lwp_t thethread, uint32_t prio);
void LWP_YieldThread(void);

/* ogc/mutex.h, ogc/cond.h */
typedef uint32_t ogc_mutex_t;
typedef uint32_t ogc_cond_t;
int32_t LWP_MutexInit(ogc_mutex_t* mutex, int use_recursive);
int32_t LWP_MutexLock(ogc_mutex_t mutex);
int32_t LWP_MutexUnlock(ogc_mutex_t mutex);
int32_t LWP_CondInit(ogc_cond_t* cond);
int32_t LWP_CondWait(ogc_cond_t cond, ogc_mutex_t mutex);
int32_t LWP_CondSignal(ogc_cond_t cond);

/* tuxedo/thread.h: a queue of threads, woken by the token they wait on */
typedef struct ogc_KThrQueue {
    void* next;
    void* prev;
} ogc_KThrQueue;
uintptr_t KThrQueueBlock(ogc_KThrQueue* q, uintptr_t token);
void KThrQueueUnblockAllByValue(ogc_KThrQueue* q, uintptr_t ref);

/* ogc/system.h */
struct timespec;
typedef uint32_t ogc_syswd_t;
typedef void (*ogc_alarmcallback)(ogc_syswd_t alarm, void* cb_arg);
int32_t SYS_CreateAlarm(ogc_syswd_t* thealarm);
int32_t SYS_SetAlarm(ogc_syswd_t thealarm, const struct timespec* tp,
                     ogc_alarmcallback cb, void* cbarg);
int32_t SYS_SetPeriodicAlarm(ogc_syswd_t thealarm,
                             const struct timespec* tp_start,
                             const struct timespec* tp_period,
                             ogc_alarmcallback cb, void* cbarg);
int32_t SYS_CancelAlarm(ogc_syswd_t thealarm);
void* SYS_GetArena1Lo(void);
void* SYS_GetArena1Hi(void);
void SYS_SetArena1Lo(void* newLo);
void SYS_SetArena1Hi(void* newHi);
uint32_t SYS_ResetButtonDown(void);

/* ogc/video.h (GXRModeObj is the SDK's GXRenderModeObj, field for field) */
typedef void (*ogc_VIRetraceCallback)(uint32_t retraceCnt);
void VIDEO_Init(void);
void VIDEO_Configure(void* rmode);
void VIDEO_Flush(void);
void VIDEO_SetBlack(int black);
uint32_t VIDEO_GetNextField(void);
uint32_t VIDEO_GetCurrentTvMode(void);
void VIDEO_WaitVSync(void);
void VIDEO_SetNextFramebuffer(void* fb);
ogc_VIRetraceCallback VIDEO_SetPreRetraceCallback(ogc_VIRetraceCallback cb);
ogc_VIRetraceCallback VIDEO_SetPostRetraceCallback(ogc_VIRetraceCallback cb);
uint32_t VIDEO_HaveComponentCable(void);

/* ogc/pad.h (PADStatus is the SDK's, field for field) */
uint32_t PAD_Init(void);
uint32_t PAD_Read(void* status);
uint32_t PAD_Reset(uint32_t mask);
uint32_t PAD_Recalibrate(uint32_t mask);
void PAD_ControlMotor(int32_t chan, uint32_t cmd);

/* ogc/si.h: called as each poll of the controllers completes */
typedef void (*ogc_RDSTHandler)(uint32_t irq, void* ctx);
uint32_t SI_RegisterPollingHandler(ogc_RDSTHandler handler);
uint32_t SI_UnregisterPollingHandler(ogc_RDSTHandler handler);

/* ogc/conf.h */
int32_t CONF_GetSoundMode(void);
int32_t CONF_GetProgressiveScan(void);
int32_t CONF_GetAspectRatio(void); /* CONF_ASPECT_16_9 is 1 */


/* system.h */
void SYS_Report(const char* fmt, ...);

#endif
