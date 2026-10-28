#pragma once
/* The water section the Earth tools write (tools/earth_bake.c's layout, and
 * tools/earth_streams.c's file, which repeats it): lines as runs of points
 * on the sphere, listed in B x B buckets a face, each bucket every segment
 * within the reach (plus a river's half width) of it, and whether its centre
 * lies in a lake. The engine reads it in src/terrain/terrain_earth.c. */
#include "terrain.h"

#include <stdint.h>
#include <stdio.h>

typedef struct Water {
    int       B;                    /* buckets a side per face; 0 without water */
    float     reach;
    uint32_t  V, E, rivers, rings;
    float    *xyz;                  /* V x 3 */
    uint16_t *width;
    uint8_t  *kind, *flags;
    uint32_t *start;                /* 6 B B + 1 */
    uint8_t  *inside;               /* 6 B B */
    uint32_t *entry;                /* E */
} Water;

/* The lakes' rings among the points, for the buckets' insides: ring r is
 * points first[r] .. first[r] + count[r] - 1, and ll holds every point's
 * longitude (unwrapped along its ring) and latitude, V x 2. */
typedef struct WaterRings {
    uint32_t       n;
    const uint32_t *first, *count;
    const double   *ll;
} WaterRings;

TerrainDir water_point(const Water *w, uint32_t k);
/* The distance (chord, on the unit sphere) from p to the arc a b: to the
 * point of the chord nearest p, taken out to the sphere. */
double water_arc_dist(TerrainDir p, TerrainDir a, TerrainDir b);
/* Douglas-Peucker on directions, to tol (radians): marks the points kept.
 * stack holds 4 n ints. */
void   water_simplify(const TerrainDir *p, int n, double tol, unsigned char *keep, int *stack);
/* Lists w's segments (the points' flags) in B x B buckets a face within
 * `reach` (m) of each, and tests the buckets' centres against the rings
 * (none without them). Returns 0 if the entries overflow. */
int    water_index(Water *w, int B, double reach, const WaterRings *rings, int threads);
/* The section after the header: the points, start, inside and entry, as the
 * bake's layout gives them. water_read takes B, V and E as set. */
int    water_write(FILE *f, const Water *w);
int    water_read(FILE *f, Water *w);
void   water_free(Water *w);
/* How many buckets list lines, the most any lists, and the section's bytes. */
void   water_stats(const Water *w, size_t *used, size_t *most, size_t *wet, double *mb);
