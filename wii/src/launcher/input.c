/* The launcher's controls: any GameCube controller, and Wii Remotes (with or
 * without a Classic Controller), as presses: the directions repeat while
 * held. The Wii Remote's stack is shut down when the launcher is done: the
 * game reads GameCube controllers only. */

#include "input.h"

#include <gccore.h>
#include <wiiuse/wpad.h>

#define REPEAT_FIRST 22 /* frames before a held direction repeats */
#define REPEAT_NEXT 6

static u32 s_held_prev;
static int s_repeat;
static int s_wpad;

void input_init(void)
{
    PAD_Init();
    s_wpad = WPAD_Init() == WPAD_ERR_NONE;
    if (s_wpad) {
        /* the pointer, in a 640x480 screen's pixels */
        WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
        WPAD_SetVRes(WPAD_CHAN_ALL, 640, 480);
    }
}

/* Where the first Wii Remote that points at the screen points (640x480; the
 * last input_poll's), or 0 when none does */
int input_pointer(int* x, int* y)
{
    if (!s_wpad) {
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        WPADData* d = WPAD_Data(i);
        if (d != NULL && d->err == WPAD_ERR_NONE && d->ir.valid) {
            *x = (int) d->ir.x;
            *y = (int) d->ir.y;
            return 1;
        }
    }
    return 0;
}

/* The game starts: the Wii Remotes stay connected, for the Classic
 * Controllers the game takes as controllers (wii/src/classic.c) */
void input_hand_over(void)
{
}

void input_shutdown(void)
{
    if (s_wpad) {
        WPAD_Shutdown();
        s_wpad = 0;
    }
}

static u32 held_now(void)
{
    u32 held = 0;
    PAD_ScanPads();
    for (int i = 0; i < 4; i++) {
        u16 b = PAD_ButtonsHeld(i);
        s8 sx = PAD_StickX(i), sy = PAD_StickY(i);
        if ((b & PAD_BUTTON_UP) || sy > 50) held |= IN_UP;
        if ((b & PAD_BUTTON_DOWN) || sy < -50) held |= IN_DOWN;
        if ((b & PAD_BUTTON_LEFT) || sx < -50) held |= IN_LEFT;
        if ((b & PAD_BUTTON_RIGHT) || sx > 50) held |= IN_RIGHT;
        if (b & PAD_BUTTON_A) held |= IN_A;
        if (b & PAD_BUTTON_B) held |= IN_B;
        if (b & PAD_BUTTON_X) held |= IN_X;
        if (b & PAD_BUTTON_Y) held |= IN_Y;
        if (b & PAD_TRIGGER_Z) held |= IN_Z;
        if (b & PAD_BUTTON_START) held |= IN_START;
    }
    if (s_wpad) {
        WPAD_ScanPads();
        for (int i = 0; i < 4; i++) {
            u32 b = WPAD_ButtonsHeld(i);
            /* the Wii Remote held sideways: its D-pad turned */
            if (b & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_UP)) held |= IN_UP;
            if (b & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_DOWN)) held |= IN_DOWN;
            if (b & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_LEFT)) held |= IN_LEFT;
            if (b & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_RIGHT)) held |= IN_RIGHT;
            if (b & (WPAD_BUTTON_2 | WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) held |= IN_A;
            if (b & (WPAD_BUTTON_1 | WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) held |= IN_B;
            if (b & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) held |= IN_X;
            if (b & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) held |= IN_Z;
            if (b & WPAD_CLASSIC_BUTTON_Y) held |= IN_Y;
            if (b & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) held |= IN_HOME;
        }
    }
    return held;
}

u32 input_held(void)
{
    return s_held_prev;
}

u32 input_poll(void)
{
    u32 held = held_now();
    u32 pressed = held & ~s_held_prev;
    u32 dirs = held & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (dirs && (held & s_held_prev & dirs)) {
        if (++s_repeat >= REPEAT_FIRST) {
            pressed |= dirs;
            s_repeat = REPEAT_FIRST - REPEAT_NEXT;
        }
    } else {
        s_repeat = 0;
    }
    s_held_prev = held;
    return pressed;
}
