#ifndef MELEE_WII_SDK_COMPAT_H
#define MELEE_WII_SDK_COMPAT_H

/* Included before every SDK source built for the Wii (cmake/Wii.cmake): the
 * MWCC intrinsics they use, and C for the assembly helpers that
 * wii/tools/strip_asm.py leaves out of them. */

static inline void __sync(void) { __asm__ volatile("sync" ::: "memory"); }
static inline void __eieio(void) { __asm__ volatile("eieio" ::: "memory"); }
static inline void __isync(void) { __asm__ volatile("isync" ::: "memory"); }

static inline int __cntlzw(unsigned int x)
{
    int n;
    __asm__("cntlzw %0, %1" : "=r"(n) : "r"(x));
    return n;
}

/* GXTransform.c's matrix writers: the same words, in the same order, into the
 * write-gather pipe */
static inline void WriteMTXPS4x3(float mtx[3][4], volatile float* dest)
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
            *dest = mtx[r][c];
        }
    }
}

static inline void WriteMTXPS3x3from3x4(float mtx[3][4], volatile float* dest)
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            *dest = mtx[r][c];
        }
    }
}

static inline void WriteMTXPS3x3(float mtx[3][3], volatile float* dest)
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            *dest = mtx[r][c];
        }
    }
}

static inline void WriteMTXPS4x2(float mtx[2][4], volatile float* dest)
{
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 4; c++) {
            *dest = mtx[r][c];
        }
    }
}

/* OSTime.c's __OSSetTime: the time base is libogc's, and not the game's to set */
static inline void __SetTime(long long time) { (void) time; }

/* MWCC's declaration syntax, in the SDK's private headers */
#define __declspec(x)
#define asm

#endif
