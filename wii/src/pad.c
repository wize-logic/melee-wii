/* The SDK's PAD on libogc's: the same serial interface, and PADStatus is
 * libogc's structure field for field.
 *
 * For testing without a controller (Dolphin run unattended): when
 * sd:/apps/melee/input.txt exists, controller 1 plays it back. Each line is
 *     FRAME BUTTONS [STICK_X STICK_Y [SUBSTICK_X SUBSTICK_Y]]
 * (FRAME in video frames since the start, BUTTONS the PAD_BUTTON_* bits in
 * hex, the sticks -128 to 127): from that frame on, controller 1 holds them,
 * on top of what a real one does, until the next line. A line "SCENE KIND"
 * waits for the game to enter that scene (gm/gm_1A3F.c reports it, as
 * melee_wii_scene): the frames after it count from there. '#' starts a
 * comment. */

#include "melee_wii.h"
#include "ogc_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <wl/platform/platform.h>

_Static_assert(sizeof(PADStatus) == 12, "PADStatus is libogc's");

#define SCRIPT_PATH "sd:/apps/melee/input.txt"
#define SCRIPT_MAX 1024

typedef struct Step {
    s32 scene; /* >= 0: wait for this scene, the rest unused */
    u32 frame;
    u16 buttons;
    s8 stick[4];
} Step;

static Step* s_script;
static u32 s_steps;
static u32 s_index;      /* the next step */
static u32 s_base;       /* the frame the frames count from */
static Step s_current;   /* what controller 1 holds */
static volatile s32 s_scene = -1;
static volatile u32 s_scene_frame;
static volatile u32 s_scene_count;
static u32 s_scene_seen;

/* The loads' count when the current scene began (dvd.c's record) */
static u32 s_scene_loads;

/* Called as the game enters a scene (mod_values.c) */
void (*melee_wii_scene_hook)(int kind);

/* How many scenes the game has entered (mod_songs.c: a song chosen for one) */
unsigned int melee_wii_scene_count(void)
{
    return s_scene_count;
}

unsigned int melee_wii_scene_first_load(void)
{
    return s_scene_loads;
}

/* 20XX's widescreen, for the launcher's Screen setting (launcher.c sets
 * this for a mod when the setting, or Wii Settings under Auto, says 16:9).
 * 20XX has its own widescreen code (Dan Salvato's, with Achilles's changes),
 * run by a value of its debug menu: 0 off, 1 the 16:9 picture. The menu's
 * values come back from its save as it starts, so the value is set once the
 * game is past that (its second and third scenes), and only where that code
 * is; the player can still turn it off there. */
int melee_wii_mod_wide;

#ifdef MELEE_ORIG_CODE
static void mod_widescreen(void)
{
    /* the code's start, where 20XX puts its own (MCC's functions' place):
     * lis r3, 0x8040; lwz r3, -0x5C30(r3); cmpwi r3, 0 (at 0x8033136C in
     * 5.0.2; its map file has another build's address) */
    volatile u32* value = (volatile u32*) 0x803FA3D0u;
    int found = 0;
    if (!melee_wii_mod_wide) {
        return;
    }
    for (volatile u32* code = (volatile u32*) 0x80331000u; code < (volatile u32*) 0x80332000u; code++) {
        if (code[0] == 0x3C608040u && code[1] == 0x8063A3D0u && code[2] == 0x2C030000u) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return;
    }
    if (*value == 0) {
        *value = 1;
        OSReport("20XX: its widescreen turned on (the launcher's Screen setting)\n");
    }
}
#else
static void mod_widescreen(void) {}
#endif

/* gm/gm_1A3F.c: the game enters a scene */
void melee_wii_scene(int kind)
{
    {
        const unsigned int *a, *l;
        s_scene_loads = melee_wii_dvd_loads(&a, &l);
    }
    s_scene = kind;
    s_scene_frame = VIGetRetraceCount();
    s_scene_count++;
    if (melee_wii_scene_hook != NULL) {
        melee_wii_scene_hook(kind);
    }
    if (s_scene_count == 2 || s_scene_count == 3) {
        mod_widescreen();
    }
    if (s_scene_count == 3) {
        melee_wii_log_sync_end(); /* the game is up: the log's thread will do */
    }
}

#ifdef MELEE_ORIG_CODE
/* The same, for the game as MWCC built it: gm_801A4014 looks the scene up as
 * it enters it, and that call comes here (wii/tools/orig_image.py --redirect) */
void* gm_FindGameSceneHandler(u8 kind);

void* melee_wii_find_scene(u8 kind)
{
    OSReport("scene: state 0 kind %d\n", (int) kind);
    melee_wii_scene(kind);
    return gm_FindGameSceneHandler(kind);
}
#endif

static void load_script(void)
{
    static BOOL loaded;
    WlPlatformFile* file;
    if (loaded) {
        return;
    }
    loaded = TRUE;
    if (!wl_platform_fs_exists(SCRIPT_PATH) ||
        wl_platform_fs_open(SCRIPT_PATH, WL_FILE_READ, &file) != WL_OK)
    {
        return;
    }
    u32 size = (u32) wl_platform_fs_size(file);
    char* text = malloc(size + 1);
    s_script = malloc(sizeof(Step) * SCRIPT_MAX);
    if (text == NULL || s_script == NULL ||
        wl_platform_fs_read_at(file, 0, text, size) != WL_OK)
    {
        wl_platform_fs_close(file);
        free(text);
        return;
    }
    wl_platform_fs_close(file);
    text[size] = '\0';
    for (char* line = text; line != NULL && *line != '\0' && s_steps < SCRIPT_MAX;) {
        char* next = line;
        while (*next != '\0' && *next != '\n') {
            next++;
        }
        if (*next == '\n') {
            *next++ = '\0';
        }
        unsigned frame, buttons;
        int sx = 0, sy = 0, cx = 0, cy = 0, kind;
        if (line[0] != '#' && sscanf(line, "SCENE %d", &kind) == 1) {
            Step* step = &s_script[s_steps++];
            memset(step, 0, sizeof(*step));
            step->scene = kind;
        } else if (line[0] != '#' &&
            sscanf(line, "%u %x %d %d %d %d", &frame, &buttons, &sx, &sy, &cx, &cy) >= 2)
        {
            Step* step = &s_script[s_steps++];
            step->scene = -1;
            step->frame = frame;
            step->buttons = (u16) buttons;
            step->stick[0] = (s8) sx;
            step->stick[1] = (s8) sy;
            step->stick[2] = (s8) cx;
            step->stick[3] = (s8) cy;
        }
        line = next;
    }
    free(text);
    OSReport("PAD: controller 1 plays %s (%u steps)\n", SCRIPT_PATH, (unsigned) s_steps);
}

static void play_script(PADStatus* status)
{
    u32 frame = VIGetRetraceCount();
    while (s_index < s_steps) {
        const Step* next = &s_script[s_index];
        if (next->scene >= 0) {
            /* a scene entered since the last one this script saw */
            if (s_scene_count == s_scene_seen || s_scene != next->scene) {
                s_scene_seen = s_scene_count;
                break;
            }
            s_scene_seen = s_scene_count;
            s_base = s_scene_frame;
            s_index++;
            OSReport("PAD: script synced to scene %d at frame %u\n", (int) next->scene,
                     (unsigned) s_base);
        } else if (frame - s_base >= next->frame) {
            s_current = *next;
            s_index++;
        } else {
            break;
        }
    }
    const Step* step = &s_current;
    PADStatus* pad = &status[0];
    if (pad->err != PAD_ERR_NONE) {
        memset(pad, 0, sizeof(*pad));
        pad->err = PAD_ERR_NONE;
    }
    pad->button |= step->buttons;
    if (step->stick[0] != 0 || step->stick[1] != 0) {
        pad->stickX = step->stick[0];
        pad->stickY = step->stick[1];
    }
    if (step->stick[2] != 0 || step->stick[3] != 0) {
        pad->substickX = step->stick[2];
        pad->substickY = step->stick[3];
    }
    if (step->buttons & PAD_BUTTON_A) {
        pad->analogA = 255;
    }
    if (step->buttons & PAD_BUTTON_B) {
        pad->analogB = 255;
    }
    if (step->buttons & PAD_TRIGGER_L) {
        pad->triggerLeft = 255;
    }
    if (step->buttons & PAD_TRIGGER_R) {
        pad->triggerRight = 255;
    }
}

/* classic.c: Wii Remotes with Classic Controllers, as controllers */
void melee_wii_classic_init(void);
void melee_wii_classic_scan(void);
int melee_wii_classic_read(int chan, u16* buttons, s8 sticks[4], u8 triggers[2]);

BOOL PADInit(void)
{
    PAD_Init();
    melee_wii_classic_init();
    load_script();
    return TRUE;
}

/* A port without a GameCube controller takes the Classic Controller of the
 * Remote with its number, if there is one */
static u32 classic_ports(PADStatus* status)
{
    u32 present = 0;
    melee_wii_classic_scan();
    for (int chan = 0; chan < 4; chan++) {
        PADStatus* pad = &status[chan];
        u16 buttons;
        s8 sticks[4];
        u8 triggers[2];
        if (pad->err != PAD_ERR_NO_CONTROLLER ||
            !melee_wii_classic_read(chan, &buttons, sticks, triggers))
        {
            continue;
        }
        memset(pad, 0, sizeof(*pad));
        pad->button = buttons;
        pad->stickX = sticks[0];
        pad->stickY = sticks[1];
        pad->substickX = sticks[2];
        pad->substickY = sticks[3];
        pad->triggerLeft = triggers[0];
        pad->triggerRight = triggers[1];
        pad->err = PAD_ERR_NONE;
        present |= 0x80000000u >> chan;
    }
    return present;
}

u32 PADRead(PADStatus* status)
{
    u32 r = PAD_Read(status);
    r |= classic_ports(status);
    if (s_steps != 0) {
        play_script(status);
    }
    return r;
}

int PADReset(unsigned long mask)
{
    return PAD_Reset(mask) != 0;
}

BOOL PADRecalibrate(u32 mask)
{
    return PAD_Recalibrate(mask) != 0;
}

/* wii/src/slippi/so_wii.c: in Slippi Online, the rumble switch (1 on, 0
 * off), or < 0 off line */
int so_wii_rumble(void);

/* Slippi's HandleRumble (online codeset, at the GameCube's PADControlMotor):
 * in an online game, the local player's rumble goes to the controller the
 * player plays with (the player's port can differ online), the opponents'
 * nowhere; and the online character select screen's rumble switch */
#define SCENE_CONTROLLER 0x80479D30u
#define SCENE_ONLINE_IN_GAME 0x0208
#define SLIPPI_ODB_POINTER (0x804DB6A0u - 0x49E4u)

void PADControlMotor(s32 chan, u32 command)
{
    int rumble = so_wii_rumble();
    if (rumble >= 0) {
        u32 scene = *(volatile u32*) SCENE_CONTROLLER;
        u32 minor_major = ((scene << 8) | (scene >> 24)) & 0xFFFF;
        if (minor_major == SCENE_ONLINE_IN_GAME) {
            const volatile u8* odb = *(u8* volatile*) SLIPPI_ODB_POINTER;
            if (odb != NULL) {
                if (chan != odb[0]) {
                    return; /* an opponent's */
                }
                chan = odb[2]; /* the local player's controller */
            }
        }
        if (rumble == 0 && command == PAD_MOTOR_RUMBLE) {
            command = PAD_MOTOR_STOP;
        }
    }
    PAD_ControlMotor(chan, command);
}

/* libogc samples at its own rate; the game asks for the SDK's default */
void PADSetSamplingRate(unsigned long msec)
{
    (void) msec;
}

void PADSetSpec(u32 spec)
{
    (void) spec;
}

/* OSSerial.c's polling handlers, called as each poll of the controllers
 * completes. The game itself registers none; a mod's code can, through the
 * SDK's PADSetSamplingCallback (20XX's lag reduction counts the frame's polls
 * with one). libogc keeps its own the same way: one handler of the port's
 * there calls the game's, at most four as the SDK keeps. */
#define POLL_HANDLERS 4
static __OSInterruptHandler s_poll_handlers[POLL_HANDLERS];
static OSContext s_poll_context;

static void poll_handlers(uint32_t irq, void* ctx)
{
    (void) irq;
    (void) ctx;
    for (int i = 0; i < POLL_HANDLERS; i++) {
        if (s_poll_handlers[i] != NULL) {
            s_poll_handlers[i](__OS_INTERRUPT_PI_SI, &s_poll_context);
        }
    }
}

BOOL SIRegisterPollingHandler(__OSInterruptHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    BOOL done = FALSE;
    int count = 0;
    for (int i = 0; i < POLL_HANDLERS; i++) {
        if (s_poll_handlers[i] == handler) {
            done = TRUE;
        }
        count += s_poll_handlers[i] != NULL;
    }
    for (int i = 0; i < POLL_HANDLERS && !done; i++) {
        if (s_poll_handlers[i] == NULL) {
            s_poll_handlers[i] = handler;
            done = TRUE;
            if (count == 0) {
                SI_RegisterPollingHandler(poll_handlers);
            }
        }
    }
    OSRestoreInterrupts(enabled);
    return done;
}

BOOL SIUnregisterPollingHandler(__OSInterruptHandler handler)
{
    BOOL enabled = OSDisableInterrupts();
    BOOL done = FALSE;
    int left = 0;
    for (int i = 0; i < POLL_HANDLERS; i++) {
        if (s_poll_handlers[i] == handler) {
            s_poll_handlers[i] = NULL;
            done = TRUE;
        }
        left += s_poll_handlers[i] != NULL;
    }
    if (done && left == 0) {
        SI_UnregisterPollingHandler(poll_handlers);
    }
    OSRestoreInterrupts(enabled);
    return done;
}
