/* A determinism test, built into the game only with MELEE_SYNCTEST (the Wii
 * build's option, or by hand into a GameCube build; gmscene.c includes it): every game frame in a
 * match, a hash of the fighters' state goes to the log, and every second the
 * state itself. Two builds that log the same lines computed the same game.
 * The random seed is set when the title screen and a match begin, so the
 * title's demo matches (computer against computer) play the same in both.
 * Online play runs the Wii build in lockstep with the GameCube's game, which
 * needs exactly this. */

#ifdef MELEE_SYNCTEST

#include <melee/ft/fighter.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/random.h>

#include <dolphin/os.h>
#include <dolphin/pad.h>

static int synctest_kind = -1;
static unsigned long synctest_frame_num;

static unsigned long synctest_mix(unsigned long h, unsigned long v)
{
    return (h ^ v) * 16777619UL;
}

static unsigned long synctest_f(float f)
{
    return *(unsigned long*) &f;
}

void synctest_scene(int kind)
{
    synctest_kind = kind;
    synctest_frame_num = 0;
    if (kind == 0 || kind == 2) {
        *HSD_RandSeedPtr = 0x12345678;
    }
    OSReport("SYNCS %d\n", kind);
}

/* controller 1, each game frame (baselib/controller.c): through the memory
 * card's prompts, A now and then */
static unsigned long synctest_pad_num;

void synctest_pad(PADStatus* status)
{
    synctest_pad_num++;
    if (synctest_kind == 42 && (synctest_pad_num % 60) < 4) {
        status[0].button |= PAD_BUTTON_A;
        status[0].analogA = 255;
        status[0].err = 0;
    }
}

/* baselib/random.c: each number drawn, in the frames around a difference */
void synctest_rand(unsigned long a, unsigned long b, unsigned long c)
{
    if (synctest_kind == 2 && synctest_frame_num + 1 >= 565 &&
        synctest_frame_num + 1 <= 580)
    {
        OSReport("SYNCR %lu %08lx %08lx %08lx %08lx\n", synctest_frame_num + 1, a, b,
                 c, *HSD_RandSeedPtr);
    }
}

void synctest_frame(void)
{
    unsigned long h = 2166136261UL;
    int slot;
    int index;
    int any = 0;
    int detail;

    synctest_frame_num++;
    detail = synctest_frame_num <= 40 || (synctest_frame_num % 60) == 0 ||
             (synctest_frame_num >= 570 && synctest_frame_num <= 578);
    for (slot = 0; slot < 6; slot++) {
        for (index = 0; index < 2; index++) {
            HSD_GObj* gobj = Player_GetEntityAtIndex(slot, index);
            Fighter* fp;
            if (gobj == NULL) {
                continue;
            }
            fp = GET_FIGHTER(gobj);
            if (fp == NULL) {
                continue;
            }
            any = 1;
            h = synctest_mix(h, (unsigned long) fp->motion_id);
            h = synctest_mix(h, synctest_f(fp->cur_pos.x));
            h = synctest_mix(h, synctest_f(fp->cur_pos.y));
            h = synctest_mix(h, synctest_f(fp->cur_pos.z));
            h = synctest_mix(h, synctest_f(fp->self_vel.x));
            h = synctest_mix(h, synctest_f(fp->self_vel.y));
            h = synctest_mix(h, synctest_f(fp->facing_dir));
            h = synctest_mix(h, synctest_f(fp->cur_anim_frame));
            h = synctest_mix(h, synctest_f(fp->dmg.x1830_percent));
            h = synctest_mix(h, (unsigned long) fp->ground_or_air);
            if (detail) {
                OSReport("SYNCD %lu %d.%d m%d p%08lx,%08lx v%08lx,%08lx "
                         "a%08lx d%08lx\n",
                         synctest_frame_num, slot, index, (int) fp->motion_id,
                         synctest_f(fp->cur_pos.x), synctest_f(fp->cur_pos.y),
                         synctest_f(fp->self_vel.x),
                         synctest_f(fp->self_vel.y),
                         synctest_f(fp->cur_anim_frame),
                         synctest_f(fp->dmg.x1830_percent));
            }
        }
    }
    if (any) {
        h = synctest_mix(h, *HSD_RandSeedPtr);
        OSReport("SYNC %d %lu %08lx\n", synctest_kind, synctest_frame_num, h);
    }
}

#endif
