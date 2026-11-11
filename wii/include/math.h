/* newlib's math.h, with the functions MWCC's MSL defines in its headers made
 * MSL's (melee_wii/gekko.h): the game and the SDK were built on MSL, and their
 * results have to be the same bit for bit. sinf, cosf, tanf, logf and frexp
 * are MSL's own (src/MSL, built with the game); atanf, atan2f, acosf, asinf,
 * expf and powf are the game's. */

#include_next <math.h>

#ifndef MELEE_WII_MATH_H
#define MELEE_WII_MATH_H

#include <melee_wii/gekko.h>

#undef sqrtf
#define sqrtf(x) melee_sqrtf(x)
#undef fmodf
#define fmodf(a, b) melee_fmodf(a, b)

#endif
