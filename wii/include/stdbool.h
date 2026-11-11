#ifndef MELEE_WII_STDBOOL_H
#define MELEE_WII_STDBOOL_H

/* The game's bool is MWCC's: an int (src/MSL/stdbool.h). Its structures,
 * and the .dat archives laid out by them, keep that size. */
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false 0
#define __bool_true_false_are_defined 1
#endif

#endif
