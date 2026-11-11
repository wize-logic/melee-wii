/* The SDK's alarms on libogc's. libogc calls an alarm's function from the
 * decrementer's interrupt, as the SDK does. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <time.h>

#include <dolphin/os.h>
#include <dolphin/os/OSAlarm.h>

#define MAX_ALARMS 32

typedef struct AlarmSlot {
    OSAlarm* alarm;
    ogc_syswd_t wd;
} AlarmSlot;

static AlarmSlot s_slots[MAX_ALARMS];

void OSInitAlarm(void) {}

void OSCreateAlarm(OSAlarm* alarm)
{
    alarm->handler = NULL;
    alarm->tag = 0;
}

/* The libogc alarm of an OSAlarm, made at its first use. One per address:
 * the game's alarms are static. */
static AlarmSlot* slot_of(OSAlarm* alarm, BOOL create)
{
    AlarmSlot* free_slot = NULL;
    for (int i = 0; i < MAX_ALARMS; i++) {
        if (s_slots[i].alarm == alarm) {
            return &s_slots[i];
        }
        if (s_slots[i].alarm == NULL && free_slot == NULL) {
            free_slot = &s_slots[i];
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    if (SYS_CreateAlarm(&free_slot->wd) < 0) {
        OSReport("OSSetAlarm: SYS_CreateAlarm failed\n");
        return NULL;
    }
    free_slot->alarm = alarm;
    return free_slot;
}

static void fire(ogc_syswd_t wd, void* arg)
{
    (void) wd;
    OSAlarm* alarm = arg;
    OSAlarmHandler handler = alarm->handler;
    if (alarm->period == 0) {
        alarm->handler = NULL;
    }
    if (handler != NULL) {
        handler(alarm, OSGetCurrentContext());
    }
}

static struct timespec to_timespec(OSTime ticks)
{
    struct timespec ts;
    u64 ns = (u64) ticks * 1000000000u / OS_TIMER_CLOCK;
    if (ticks <= 0) {
        ns = 1000; /* at once: libogc ignores a zero time */
    }
    ts.tv_sec = ns / 1000000000u;
    ts.tv_nsec = ns % 1000000000u;
    return ts;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    AlarmSlot* slot = slot_of(alarm, TRUE);
    alarm->handler = handler;
    alarm->period = 0;
    alarm->start = OSGetTime();
    alarm->fire = alarm->start + tick;
    if (slot != NULL) {
        struct timespec ts = to_timespec(tick);
        SYS_SetAlarm(slot->wd, &ts, fire, alarm);
    }
    OSRestoreInterrupts(enabled);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                        OSAlarmHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    AlarmSlot* slot = slot_of(alarm, TRUE);
    alarm->handler = handler;
    alarm->period = period;
    alarm->start = start;
    if (slot != NULL) {
        /* The first at the next multiple of the period after start */
        OSTime now = OSGetTime();
        OSTime first = start;
        if (first <= now && period > 0) {
            first = start + ((now - start) / period + 1) * period;
        }
        alarm->fire = first;
        struct timespec ts_start = to_timespec(first - now);
        struct timespec ts_period = to_timespec(period);
        SYS_SetPeriodicAlarm(slot->wd, &ts_start, &ts_period, fire, alarm);
    }
    OSRestoreInterrupts(enabled);
}

void OSCancelAlarm(OSAlarm* alarm)
{
    BOOL enabled = OSDisableInterrupts();
    AlarmSlot* slot = slot_of(alarm, FALSE);
    if (slot != NULL) {
        SYS_CancelAlarm(slot->wd);
    }
    alarm->handler = NULL;
    OSRestoreInterrupts(enabled);
}
