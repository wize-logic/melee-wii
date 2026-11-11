#ifndef METROTRK_INTRINSICS_H
#define METROTRK_INTRINSICS_H

#include <stddef.h>

void __sync(void);
void __isync(void);
int __cntlzw(unsigned int);
#if defined(TARGET_WII) && !defined(__MWERKS__)
#include <melee_wii/gekko.h>
#else
float sqrtf__Ff(float);
float __fnmsubs(float, float, float);
double __fabs(double);
float __fabsf(float);
double __frsqrte(double);
#endif
int __rlwinm(int, int, int, int);
int __rlwimi(int, int, int, int, int);

void* __memcpy(void* dst, const void* src, size_t n);

#endif
