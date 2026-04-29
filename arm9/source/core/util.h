/*
 * util.h -- Shared utility functions for maxtracker core.
 */

#ifndef MT_UTIL_H
#define MT_UTIL_H

#include <nds.h>

/* Clamp a signed 32-bit value to [lo, hi]. */
static inline s32 clamp_s32(s32 val, s32 lo, s32 hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

#endif /* MT_UTIL_H */
