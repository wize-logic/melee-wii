#ifndef MELEE_WII_GEKKO_H
#define MELEE_WII_GEKKO_H

/* MWCC's floating-point intrinsics, as the instructions they stand for. The
 * game's results have to be the GameCube's bit for bit (online play runs a
 * Wii and a GameCube or Dolphin in lockstep), so the estimates (frsqrte, fres)
 * and the fused multiply-adds are the CPU's own, not C approximations of
 * them. */

static inline double __frsqrte(double x)
{
    double r;
    __asm__("frsqrte %0, %1" : "=f"(r) : "f"(x));
    return r;
}

static inline float __fres(float x)
{
    float r;
    __asm__("fres %0, %1" : "=f"(r) : "f"(x));
    return r;
}

static inline double __fabs(double x)
{
    double r;
    __asm__("fabs %0, %1" : "=f"(r) : "f"(x));
    return r;
}

static inline float __fabsf(float x)
{
    float r;
    __asm__("fabs %0, %1" : "=f"(r) : "f"(x));
    return r;
}

static inline double __fnabs(double x)
{
    double r;
    __asm__("fnabs %0, %1" : "=f"(r) : "f"(x));
    return r;
}

static inline double __fmadd(double a, double c, double b)
{
    double r;
    __asm__("fmadd %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline double __fmsub(double a, double c, double b)
{
    double r;
    __asm__("fmsub %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline double __fnmadd(double a, double c, double b)
{
    double r;
    __asm__("fnmadd %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline double __fnmsub(double a, double c, double b)
{
    double r;
    __asm__("fnmsub %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline float __fmadds(float a, float c, float b)
{
    float r;
    __asm__("fmadds %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline float __fmsubs(float a, float c, float b)
{
    float r;
    __asm__("fmsubs %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline float __fnmadds(float a, float c, float b)
{
    float r;
    __asm__("fnmadds %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline float __fnmsubs(float a, float c, float b)
{
    float r;
    __asm__("fnmsubs %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(c), "f"(b));
    return r;
}

static inline double __fsel(double a, double b, double c)
{
    double r;
    __asm__("fsel %0, %1, %2, %3" : "=f"(r) : "f"(a), "f"(b), "f"(c));
    return r;
}

/* MSL's sqrtf (MSL/math_ppc.h), which MWCC inlines: three Newton steps from
 * the estimate, in double, rounded once to float. Compiled with each caller's
 * contraction (the game fuses "3.0 - g * g * x"; the SDK doesn't) as MWCC
 * compiles it. */
static inline float melee_sqrtf(float x)
{
    volatile float y;
    if (x > 0.0f) {
        double guess = __frsqrte((double) x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        y = (float) (x * guess);
        return y;
    }
    return x;
}

static inline float melee_sqrtf_accurate(float x)
{
    volatile float y;
    if (x > 0.0f) {
        double guess = __frsqrte((double) x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        guess = 0.5 * guess * (3.0 - guess * guess * x);
        y = (float) (x * guess);
        return y;
    }
    return x;
}

/* MSL's fmodf (MSL/math.h) */
static inline float melee_fmodf(float a, float b)
{
    long long quotient;
    if (__fabsf(b) > __fabsf(a)) {
        return a;
    }
    quotient = a / b;
    return a - b * quotient;
}

#endif
