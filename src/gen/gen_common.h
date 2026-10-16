#pragma once
#include "building.h"
#include <stdarg.h>

/* Shared by the generators (src/gen/building.c, interior.c, arcology.c). */

/* Append to a decision log, truncating at its capacity. A NULL log, or one
 * without a buffer, is silently ignored. */
void blog_vprintf(BLog *log, const char *fmt, va_list ap);
void blog_printf(BLog *log, const char *fmt, ...);

/* (x, z) turned by q quarter turns: q = 1 takes +x to +z. The planners turn
 * the building frame by -q into their planning frame, so that the entrance
 * faces -z, and by q back out of it. */
static inline void quarter_turn(int q, float x, float z, float *ox, float *oz) {
    switch (q & 3) {
    case 0:  *ox = x;  *oz = z;  break;
    case 1:  *ox = -z; *oz = x;  break;
    case 2:  *ox = -x; *oz = -z; break;
    default: *ox = z;  *oz = -x; break;
    }
}

/* A building-frame outline or footprint in a planning frame turned by q. */
void loop_to_frame(int q, const BLoop *in, BLoop *out);
void foot_to_frame(int q, const BFoot *in, BFoot *out);
