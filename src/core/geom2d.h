#pragma once
#include <math.h>

/* World-space 2D points and vectors in double precision (x east, z south),
 * as the plan and the chunk generators use them. */
typedef struct DVec2 { double x, z; } DVec2;

static inline DVec2  dv(double x, double z) { DVec2 r = { x, z }; return r; }
static inline DVec2  dsub(DVec2 a, DVec2 b) { return dv(a.x - b.x, a.z - b.z); }
static inline DVec2  dadd(DVec2 a, DVec2 b) { return dv(a.x + b.x, a.z + b.z); }
static inline DVec2  dscale(DVec2 a, double s) { return dv(a.x * s, a.z * s); }
static inline double ddot(DVec2 a, DVec2 b) { return a.x * b.x + a.z * b.z; }
static inline double dcross(DVec2 a, DVec2 b) { return a.x * b.z - a.z * b.x; }
static inline double dlen(DVec2 a) { return sqrt(a.x * a.x + a.z * a.z); }
static inline DVec2  dnorm(DVec2 a) { double l = dlen(a); return l > 1e-12 ? dv(a.x / l, a.z / l) : dv(1, 0); }

/* Distance from p to the segment a-b. */
static inline double seg_dist(DVec2 p, DVec2 a, DVec2 b) {
    DVec2 ab = dsub(b, a);
    double l2 = ddot(ab, ab);
    double t = l2 > 1e-12 ? ddot(dsub(p, a), ab) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return dlen(dsub(p, dadd(a, dscale(ab, t))));
}
