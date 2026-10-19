#include "terrain_internal.h"
#include "plan.h"

/* The planet (see terrain.h): the chart that world X/Z is, the cube-sphere
 * that addresses the surface, and the ground and horizon over it. Every
 * function is pure; directions are computed in double. */

/* Where the chart's origin lies: face 0 (+x), at equal-angle (u, v) =
 * (0.31, 0.95). That puts the face's +v edge (the seam with face 4, +z)
 * about 250 km from the city, within sight from aircraft height, so the
 * seams get looked at. */
#define PI 3.14159265358979323846

#define CHART_FACE 0
#define CHART_U    0.31
#define CHART_V    0.95

/* Over the hemisphere round the origin the ground is the chart's own. From
 * CHART_BLEND_FROM radians out to CHART_BLEND_TO it blends into a second
 * chart's, centred on the antipode, where the first chart's singularity is.
 * The second chart's land is offset by ANTIPODE_OFFSET_M so it isn't the
 * first's land mirrored. */
#define CHART_BLEND_FROM  (0.40 * PI)
#define CHART_BLEND_TO    (0.60 * PI)
#define ANTIPODE_OFFSET_M 3.1e6

static double comp(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

/* --- The cube-sphere ------------------------------------------------------------ */

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

/* --- The chart -------------------------------------------------------------------- */

/* The chart's origin o and its x and z directions there: x east, z south. */
static void chart_basis(TerrainDir *ex, TerrainDir *o, TerrainDir *ez) {
    *o = terrain_face_dir(CHART_FACE, CHART_U, CHART_V);
    TerrainDir east = { -o->y, o->x, 0.0 };   /* the pole (+z) cross o */
    *ex = unit3(east);
    *ez = cross3(*ex, *o);
}

TerrainDir terrain_chart_dir(double x, double z) {
    TerrainDir ex, o, ez;
    chart_basis(&ex, &o, &ez);
    double s = hypot(x, z);
    if (s == 0.0) return o;
    double th = s / PLANET_RADIUS, k = sin(th) / s;
    return add3(scale3(o, cos(th)), scale3(add3(scale3(ex, x), scale3(ez, z)), k));
}

/* p in the chart: (a, b) its components along the chart's x and z, st their
 * length (the sine of its angle th from the origin). The one computation of
 * it, so the planet's ground asks the chart for exactly the point
 * terrain_dir_chart gives. */
typedef struct ChartPos { double x, z, a, b, st, th; } ChartPos;
static ChartPos chart_pos(TerrainDir p) {
    TerrainDir ex, o, ez;
    chart_basis(&ex, &o, &ez);
    ChartPos c = { 0.0, 0.0, dot3(p, ex), dot3(p, ez), 0.0, 0.0 };
    c.st = hypot(c.a, c.b);
    c.th = atan2(c.st, dot3(p, o));
    if (c.st > 0.0) {
        double k = PLANET_RADIUS * c.th / c.st;
        c.x = c.a * k;
        c.z = c.b * k;
    }
    return c;
}

void terrain_dir_chart(TerrainDir p, double *x, double *z) {
    ChartPos c = chart_pos(p);
    *x = c.x;
    *z = c.z;
}

TerrainDir terrain_chart_point(double x, double y, double z) {
    return scale3(terrain_chart_dir(x, z), PLANET_RADIUS + y);
}

/* The chart is polar about the origin: moving dr out from it turns the
 * direction by dr / R towards it, and moving across it by ds turns it by
 * ds sin(theta) / (theta R). The chart's x direction mixes the two. */
void terrain_chart_frame(double x, double z, TerrainDir *ax, TerrainDir *up, TerrainDir *az) {
    TerrainDir ex, o, ez;
    chart_basis(&ex, &o, &ez);
    *up = terrain_chart_dir(x, z);
    double s = hypot(x, z);
    TerrainDir dx = ex;
    if (s > 0.0) {
        double ux = x / s, uz = z / s, th = s / PLANET_RADIUS;
        TerrainDir t = add3(scale3(ex, ux), scale3(ez, uz)), n = add3(scale3(ex, -uz), scale3(ez, ux));
        TerrainDir radial = add3(scale3(o, -sin(th)), scale3(t, cos(th)));
        dx = add3(scale3(radial, ux), scale3(n, -uz * sin(th) / th));
    }
    dx = add3(dx, scale3(*up, -dot3(dx, *up)));
    *ax = unit3(dx);
    *az = cross3(*ax, *up);
}

/* (R + h) times the direction turned theta = s / R from the camera's, less
 * (R + cam_y) times the camera's: across, (R + h) sin(theta), which is
 * s (1 + h / R) sinc(theta); up, (R + h) cos(theta) - (R + cam_y), which is
 * (h - cam_y) - (R + h) 2 sin^2(theta / 2). Both forms keep float's relative
 * precision, as R + h - R would not. */
void terrain_bend(float dx, float dz, float h, float cam_y, float out[3]) {
    const float R = (float)PLANET_RADIUS;
    float s = sqrtf(dx * dx + dz * dz), th = s / R;
    float sinc = th > 1e-3f ? sinf(th) / th : 1.0f - th * th / 6.0f;
    float k = (1.0f + h / R) * sinc, hs = sinf(0.5f * th);
    out[0] = dx * k;
    out[1] = (h - cam_y) - (R + h) * 2.0f * hs * hs;
    out[2] = dz * k;
}

/* The farthest corner off sets the most the box can fall and spread: the
 * drop (R + h) 2 sin^2(theta / 2) is under s^2 (1 + h / R) / 2R, and across, k
 * strays from 1 by under theta^2 / 6 + h / R; h / R < 0.001 for h < 6 km. */
void terrain_bend_bounds(float mn[3], float mx[3]) {
    const double R = PLANET_RADIUS;
    double sx = fmax(fabs(mn[0]), fabs(mx[0])), sz = fmax(fabs(mn[2]), fabs(mx[2])), s = hypot(sx, sz);
    double drop = s * s / (2.0 * R) * 1.001 + 0.01, grow = s * (s * s / (6.0 * R * R) + 0.001) + 0.01;
    mn[1] -= (float)drop;
    mn[0] -= (float)grow; mx[0] += (float)grow;
    mn[2] -= (float)grow; mx[2] += (float)grow;
}

/* --- The ground on the planet ----------------------------------------------------- */

static double planet_lod(TerrainDir p, double spacing, int surface) {
    ChartPos c = chart_pos(p);
    double near = 0.0;
    if (c.th < CHART_BLEND_TO) {
        if (surface) near = terrain_surface_lod(c.x, c.z, spacing, NULL);
        else near = terrain_height_lod(c.x, c.z, spacing);
        if (c.th <= CHART_BLEND_FROM) return near;
    }
    /* The antipode's chart: centred on -o, with x along ex and z along -ez
     * (a right-handed frame there). Nothing of the city reaches this far. */
    double xb = 0.0, zb = 0.0;
    if (c.st > 0.0) {
        double k = PLANET_RADIUS * (PI - c.th) / c.st;
        xb = c.a * k;
        zb = -c.b * k;
    }
    double far = terrain_land_at(xb + ANTIPODE_OFFSET_M, zb + ANTIPODE_OFFSET_M, p, 1e9, spacing);
    if (c.th >= CHART_BLEND_TO) return far;
    double t = (c.th - CHART_BLEND_FROM) / (CHART_BLEND_TO - CHART_BLEND_FROM), w = t * t * (3.0 - 2.0 * t);
    return near + w * (far - near);
}

double terrain_planet_height_lod(TerrainDir p, double spacing) { return planet_lod(p, spacing, 0); }
double terrain_planet_surface_lod(TerrainDir p, double spacing) { return planet_lod(p, spacing, 1); }

/* The land's own bound, and the city's ground at its lowest: a settlement's
 * relief down, sunk by at most that relief again and the clearance
 * (terrain_urban_sink, every octave left out). */
double terrain_planet_lowest(void) {
    double lo, hi, rlo, relief;
    terrain_land_range(1e9, 1e9, &lo, &hi);
    plan_settlement_relief_range(&rlo, &relief);
    return fmin(lo, -2.0 * relief - TERRAIN_URBAN_SINK_CLEAR_M - terrain_lift_bound(1e9)) - 1.0;
}

/* --- Where the sea will be ----------------------------------------------------------
 * The planet has no water yet. When it does, the sea will fill the basins of
 * the macro field below SEA_LEVEL_MACRO, and the whole far side of the planet
 * from the chart's origin, beyond SEA_OUTER_FROM: the world ocean, until a
 * settlement there can have a chart of its own (plan.h, PLAN_WORLD_REACH). */
#define SEA_LEVEL_MACRO (-0.12)
#define SEA_OUTER_FROM  (0.35 * PI)
#define SEA_OUTER_TO    (0.39 * PI)
#define SEA_OUTER_M     4000.0

double terrain_sea_margin(double x, double z) {
    double th = hypot(x, z) / PLANET_RADIUS;
    if (th >= SEA_OUTER_TO) return -SEA_OUTER_M;
    double m = TERRAIN_MACRO_M * (terrain_macro(terrain_chart_dir(x, z), 0.0) - SEA_LEVEL_MACRO);
    if (th > SEA_OUTER_FROM) {
        double t = (th - SEA_OUTER_FROM) / (SEA_OUTER_TO - SEA_OUTER_FROM);
        m -= SEA_OUTER_M * t * t * (3.0 - 2.0 * t);
    }
    return m;
}

/* --- The horizon ------------------------------------------------------------------- */

/* Seen from the camera C, the occluding sphere fills a cone round -C of half
 * angle asin(occluder / |C|), and a sight line inside that cone enters the
 * sphere at its near cap, which lies on the camera's side of the plane
 * through the cone's circle of tangency, x . C^ = occluder^2 / |C|. So a ball
 * wholly inside the cone and wholly beyond that plane is behind the sphere. */
int terrain_horizon_hidden(TerrainDir cam, TerrainDir centre, double radius, double occluder) {
    double dc = sqrt(dot3(cam, cam));
    if (dc <= occluder) return 0;
    TerrainDir c = scale3(cam, 1.0 / dc);
    if (dot3(centre, c) + radius > occluder * occluder / dc) return 0;
    TerrainDir w = add3(centre, scale3(cam, -1.0));
    double L = sqrt(dot3(w, w));
    if (L <= radius) return 0;
    double cosb = -dot3(w, c) / L;
    double beta = acos(cosb > 1.0 ? 1.0 : cosb < -1.0 ? -1.0 : cosb);
    return beta + asin(radius / L) <= asin(occluder / dc);
}
