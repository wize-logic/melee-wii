/* Where newlib and the game's C library (MSL) differ in what the game relies on */

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

int __real_vsnprintf(char* str, size_t size, const char* format, va_list args);

/* The game formats into its buffers with a size of -1 (hsd_3A64.c,
 * textlib.c): MSL takes that as "no limit", newlib refuses any size past
 * INT_MAX and writes nothing */
int __wrap_vsnprintf(char* str, size_t size, const char* format, va_list args)
{
    if (size > INT_MAX) {
        size = INT_MAX;
    }
    return __real_vsnprintf(str, size, format, args);
}
