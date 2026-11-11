#include "random.h"

static u32 seed = 1;
u32* HSD_RandSeedPtr = &seed;

#ifdef MELEE_SYNCTEST
/* gm/synctest.inc: who draws a number, in the frames it logs */
extern void synctest_rand(u32 a, u32 b, u32 c);
#define SYNCTEST_RAND()                                                       \
    {                                                                         \
        register u32 a, b, c;                                                 \
        asm {                                                                 \
            mflr a;                                                           \
            lwz c, 0(r1);                                                     \
            lwz c, 0(c);                                                      \
            lwz b, 4(c);                                                      \
            lwz c, 0(c);                                                      \
            lwz c, 4(c)                                                       \
        }                                                                     \
        synctest_rand(a, b, c);                                               \
    }
#else
#define SYNCTEST_RAND()
#endif

s32 HSD_Rand(void)
{
    SYNCTEST_RAND();
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    return *HSD_RandSeedPtr >> 0x10;
}

f32 HSD_Randf(void)
{
    SYNCTEST_RAND();
    *HSD_RandSeedPtr = *HSD_RandSeedPtr * 214013 + 2531011;
    return (f32) (*HSD_RandSeedPtr >> 0x10) / (1 << 16);
}

s32 HSD_Randi(s32 max_val)
{
    return max_val * HSD_Rand() / (1 << 16);
}

void _HSD_RandForgetMemory(void* low, void* high)
{
    if (low <= (void*) HSD_RandSeedPtr && (void*) HSD_RandSeedPtr < high) {
        HSD_RandSeedPtr = &seed;
    }
}
