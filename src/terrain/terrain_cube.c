#include "terrain.h"
#include <math.h>

/* The cube-sphere (see terrain.h, "The planet"): the six equal-angle faces
 * that address the planet's surface, the patches on them, and latitude and
 * longitude in the planet frame. Every function is pure, in double, and
 * depends on nothing but this file: the Earth bake (tools/earth_bake.c) links
 * it on its own, so the bake and the engine evaluate every direction bit for
 * bit the same. Nothing here may reach the chart, the city or the ground. */

#define PI 3.14159265358979323846

static double comp(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

/* --- The faces ---------------------------------------------------------------- */

/* tan(u pi / 4), exactly +-1 at the face's edges (tan(pi / 4) rounds below
 * 1) and odd, so the faces either side of an edge agree on it. */
static double quarter_tan(double u) {
    if (u == 1.0 || u == -1.0) return u;
    return copysign(tan(0.25 * PI * fabs(u)), u);
}

TerrainDir terrain_face_dir(int face, double u, double v) {
    int a = face / 2;
    double c[3];
    c[a] = (face & 1) ? -1.0 : 1.0;
    c[(a + 1) % 3] = quarter_tan(u);
    c[(a + 2) % 3] = quarter_tan(v);
    double len = sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    TerrainDir p = { c[0] / len, c[1] / len, c[2] / len };
    return p;
}

int terrain_dir_face(TerrainDir p, double *u, double *v) {
    double ax = fabs(p.x), ay = fabs(p.y), az = fabs(p.z);
    int a = ax >= ay && ax >= az ? 0 : ay >= az ? 1 : 2;
    double n = fabs(comp(p, a));
    *u = atan(comp(p, (a + 1) % 3) / n) * (4.0 / PI);
    *v = atan(comp(p, (a + 2) % 3) / n) * (4.0 / PI);
    return 2 * a + (comp(p, a) < 0.0);
}

/* --- Patches ------------------------------------------------------------------ */

/* Grid line k of n across a face: (2k - n) / n, which is exact in the
 * integers and correctly rounded, so the same line asked for at any level or
 * through either face gives the same double. */
static double grid_coord(int64_t k, int64_t n) { return (double)(2 * k - n) / (double)n; }

TerrainDir terrain_patch_dir(TerrainPatch t, int cells, int gi, int gj) {
    int64_t n = (int64_t)cells << t.level;
    return terrain_face_dir(t.face, grid_coord(t.i * cells + gi, n), grid_coord(t.j * cells + gj, n));
}

TerrainPatch terrain_patch_at(TerrainDir p, int level) {
    double u, v;
    TerrainPatch t = { terrain_dir_face(p, &u, &v), level, 0, 0 };
    int64_t n = (int64_t)1 << level;
    t.i = (int64_t)floor((u + 1.0) * 0.5 * (double)n);
    t.j = (int64_t)floor((v + 1.0) * 0.5 * (double)n);
    t.i = t.i < 0 ? 0 : t.i >= n ? n - 1 : t.i;
    t.j = t.j < 0 ? 0 : t.j >= n ? n - 1 : t.j;
    return t;
}

TerrainPatch terrain_patch_parent(TerrainPatch t) {
    TerrainPatch r = { t.face, t.level - 1, t.i >> 1, t.j >> 1 };
    return r;
}

TerrainPatch terrain_patch_child(TerrainPatch t, int k) {
    TerrainPatch r = { t.face, t.level + 1, 2 * t.i + (k & 1), 2 * t.j + (k >> 1) };
    return r;
}

/* A quarter of a patch past the middle of the side: on this face or, past
 * its edge, the next face's patch of the same size (the faces are equal
 * squares, so patches of a level line up across every seam). */
TerrainPatch terrain_patch_neighbour(TerrainPatch t, int side) {
    double n = ldexp(1.0, t.level), w = 2.0 / n;
    double u = -1.0 + (t.i + 0.5) * w, v = -1.0 + (t.j + 0.5) * w;
    double step = 0.75 * w;
    if (side == 0) u -= step; else if (side == 1) u += step; else if (side == 2) v -= step; else v += step;
    return terrain_patch_at(terrain_face_dir(t.face, u, v), t.level);
}

/* --- Latitude and longitude --------------------------------------------------- */

TerrainDir terrain_latlon_dir(double lat_deg, double lon_deg) {
    double lat = lat_deg * (PI / 180.0), lon = lon_deg * (PI / 180.0);
    double c = cos(lat);
    TerrainDir p = { c * cos(lon), c * sin(lon), sin(lat) };
    return p;
}

void terrain_dir_latlon(TerrainDir p, double *lat_deg, double *lon_deg) {
    *lat_deg = atan2(p.z, hypot(p.x, p.y)) * (180.0 / PI);
    *lon_deg = (p.x == 0.0 && p.y == 0.0) ? 0.0 : atan2(p.y, p.x) * (180.0 / PI);
}
