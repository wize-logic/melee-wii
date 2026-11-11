/* Wii Remotes with a Classic Controller (or Classic Controller Pro) as
 * GameCube controllers, for Wiis without GameCube ports (and for players who
 * like them): the Remote that is player N plays as controller N whenever no
 * GameCube controller is plugged in there (wii/src/pad.c asks).
 *
 * The buttons as the Classic's face says (A, B, X, Y; + is Start), the left
 * stick as the control stick and the right one as the C-stick, at a GameCube
 * stick's reach; L and R analog (a Pro's, which are digital, all or nothing),
 * ZR as Z and ZL as a full L. The sticks come from libogc's angle and
 * magnitude, on a circle.
 *
 * HOME, on any Remote or its Classic Controller, held for a second leaves
 * for the Homebrew Channel as the console's RESET button does: the system
 * watch thread (main.c's tick) asks here and leaves as it does for RESET. */

#include <math.h>
#include <string.h>

#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <wiiuse/wpad.h>

/* A GameCube stick's full reach, as Melee reads it (its gate's corners are
 * further, its sides nearer) */
#define STICK_FULL 100.0f
#define CLASSIC_TRIGGER_FULL 31

#define HOME_HOLD_MS 1000

static int s_started;
static int s_home_down;
static u64 s_home_since;
static volatile int s_home_leave;

static void js_to_xy(const struct joystick_t* js, s8 out[2])
{
    float rad = js->ang * (3.14159265f / 180.0f);
    float m = js->mag < 0.0f ? 0.0f : (js->mag > 1.0f ? 1.0f : js->mag);
    out[0] = (s8) lroundf(m * sinf(rad) * STICK_FULL);
    out[1] = (s8) lroundf(m * cosf(rad) * STICK_FULL);
}

static u8 trigger(u8 raw)
{
    return raw >= CLASSIC_TRIGGER_FULL ? 255 : (u8) (raw * 255 / CLASSIC_TRIGGER_FULL);
}

void melee_wii_classic_init(void)
{
    if (!s_started) {
        s_started = WPAD_Init() == WPAD_ERR_NONE;
    }
}

/* HOME held on any Remote (or its Classic Controller's): the time it has
 * been down, not a count of scans, which a game replaying frames makes more
 * of */
static void scan_home(void)
{
    int down = 0;
    for (int chan = 0; chan < 4; chan++) {
        u32 type;
        const WPADData* d;
        if (WPAD_Probe(chan, &type) != WPAD_ERR_NONE) {
            continue;
        }
        d = WPAD_Data(chan);
        if (d == NULL || d->err != WPAD_ERR_NONE) {
            continue;
        }
        if ((d->btns_h & WPAD_BUTTON_HOME) != 0 ||
            (type == WPAD_EXP_CLASSIC && (d->btns_h & WPAD_CLASSIC_BUTTON_HOME) != 0))
        {
            down = 1;
        }
    }
    if (!down) {
        s_home_down = 0;
    } else if (!s_home_down) {
        s_home_down = 1;
        s_home_since = gettime();
    } else if (diff_msec(s_home_since, gettime()) >= HOME_HOLD_MS) {
        s_home_leave = 1;
    }
}

/* Once a frame, before the reads */
void melee_wii_classic_scan(void)
{
    if (s_started) {
        WPAD_ScanPads();
        scan_home();
    }
}

/* HOME has been held for a second (the system watch thread asks) */
int melee_wii_home_held(void)
{
    return s_home_leave;
}

/* Controller `chan`'s state from Remote `chan`'s Classic Controller: 1, or 0
 * when there is none. buttons: PAD_BUTTON_* / PAD_TRIGGER_*; sticks: main x,
 * y, C x, y; triggers: L, R (0-255) */
int melee_wii_classic_read(int chan, u16* buttons, s8 sticks[4], u8 triggers[2])
{
    u32 type;
    const WPADData* d;
    u32 b;
    u16 out = 0;
    if (!s_started || WPAD_Probe(chan, &type) != WPAD_ERR_NONE || type != WPAD_EXP_CLASSIC) {
        return 0;
    }
    d = WPAD_Data(chan);
    if (d == NULL || d->err != WPAD_ERR_NONE) {
        return 0;
    }
    b = d->btns_h;
    if (b & WPAD_CLASSIC_BUTTON_A) out |= PAD_BUTTON_A;
    if (b & WPAD_CLASSIC_BUTTON_B) out |= PAD_BUTTON_B;
    if (b & WPAD_CLASSIC_BUTTON_X) out |= PAD_BUTTON_X;
    if (b & WPAD_CLASSIC_BUTTON_Y) out |= PAD_BUTTON_Y;
    if (b & WPAD_CLASSIC_BUTTON_PLUS) out |= PAD_BUTTON_START;
    if (b & WPAD_CLASSIC_BUTTON_UP) out |= PAD_BUTTON_UP;
    if (b & WPAD_CLASSIC_BUTTON_DOWN) out |= PAD_BUTTON_DOWN;
    if (b & WPAD_CLASSIC_BUTTON_LEFT) out |= PAD_BUTTON_LEFT;
    if (b & WPAD_CLASSIC_BUTTON_RIGHT) out |= PAD_BUTTON_RIGHT;
    if (b & WPAD_CLASSIC_BUTTON_ZR) out |= PAD_TRIGGER_Z;
    js_to_xy(&d->exp.classic.ljs, sticks);
    js_to_xy(&d->exp.classic.rjs, sticks + 2);
    triggers[0] = trigger(d->exp.classic.ls_raw);
    triggers[1] = trigger(d->exp.classic.rs_raw);
    if (b & (WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL)) {
        out |= PAD_TRIGGER_L;
        triggers[0] = 255;
    }
    if (b & WPAD_CLASSIC_BUTTON_FULL_R) {
        out |= PAD_TRIGGER_R;
        triggers[1] = 255;
    }
    *buttons = out;
    return 1;
}
