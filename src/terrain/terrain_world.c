#include "terrain_internal.h"
#include "plan.h"

/* The planet (see terrain.h): the chart that world X/Z is, the cube-sphere
 * that addresses the surface, the bend and the horizon over it. Every
 * function is pure; directions are computed in double. */

/* Where the chart's origin lies: the Earth bake's origin, the megacity's
 * place on the Earth (terrain_earth_chart). Without a bake, face 0 (+x), at
 * equal-angle (u, v) = (0.31, 0.95). That puts the face's +v edge (the seam
 * with face 4, +z) about 250 km from the city, within sight from aircraft
 * height, so the seams get looked at. */
#define PI 3.14159265358979323846

#define CHART_FACE 0
#define CHART_U    0.31
#define CHART_V    0.95

/* The cube-sphere that addresses the surface, its patches, and latitude and
 * longitude live in terrain_cube.c, which the Earth bake links on its own. */

/* --- The charts ------------------------------------------------------------------- */

/* East and north at o, both turned clockwise by the heading for x and -z;
 * z = x cross o makes the frame (x, up, z) right-handed. At a pole east is
 * undefined: no chart is centred on one. */
TerrainChart terrain_chart_about(TerrainDir o, double heading_deg) {
    TerrainDir east = unit3((TerrainDir){ -o.y, o.x, 0.0 });   /* the pole (+z) cross o */
    TerrainDir north = cross3(o, east);
    double h = heading_deg * (PI / 180.0), c = cos(h), s = sin(h);
    TerrainChart ch;
    ch.o = o;
    ch.ex = add3(scale3(east, c), scale3(north, -s));
    ch.ez = cross3(ch.ex, o);
    return ch;
}

TerrainChart terrain_world_chart(void) {
    TerrainChart c;
    if (terrain_earth_chart(&c)) return c;
    return terrain_chart_about(terrain_face_dir(CHART_FACE, CHART_U, CHART_V), 0.0);
}

TerrainDir terrain_chart_dir(const TerrainChart *c, double x, double z) {
    double s = hypot(x, z);
    if (s == 0.0) return c->o;
    double th = s / PLANET_RADIUS, k = sin(th) / s;
    return add3(scale3(c->o, cos(th)), scale3(add3(scale3(c->ex, x), scale3(c->ez, z)), k));
}

/* p's components along the chart's x and z, (a, b), have length st, the
 * sine of its angle th from the origin; its place is (a, b) times th R / st. */
void terrain_dir_chart(const TerrainChart *c, TerrainDir p, double *x, double *z) {
    double a = dot3(p, c->ex), b = dot3(p, c->ez), st = hypot(a, b), th = atan2(st, dot3(p, c->o));
    *x = *z = 0.0;
    if (st > 0.0) {
        double k = PLANET_RADIUS * th / st;
        *x = a * k;
        *z = b * k;
    }
}

TerrainDir terrain_chart_point(const TerrainChart *c, double x, double y, double z) {
    return scale3(terrain_chart_dir(c, x, z), PLANET_RADIUS + y);
}

/* The chart is polar about the origin: moving dr out from it turns the
 * direction by dr / R towards it, and moving across it by ds turns it by
 * ds sin(theta) / (theta R). The chart's x direction mixes the two. */
void terrain_chart_frame(const TerrainChart *c, double x, double z, TerrainDir *ax, TerrainDir *up, TerrainDir *az) {
    *up = terrain_chart_dir(c, x, z);
    double s = hypot(x, z);
    TerrainDir dx = c->ex;
    if (s > 0.0) {
        double ux = x / s, uz = z / s, th = s / PLANET_RADIUS;
        TerrainDir t = add3(scale3(c->ex, ux), scale3(c->ez, uz)), n = add3(scale3(c->ex, -uz), scale3(c->ez, ux));
        TerrainDir radial = add3(scale3(c->o, -sin(th)), scale3(t, cos(th)));
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
 * strays from 1 by under theta^2 / 6 + |h| / R; |h| / R < 0.002 for |h| under
 * 12.7 km, the Earth's deepest trench and highest peak less any datum. */
void terrain_bend_bounds(float mn[3], float mx[3]) {
    const double R = PLANET_RADIUS;
    double sx = fmax(fabs(mn[0]), fabs(mx[0])), sz = fmax(fabs(mn[2]), fabs(mx[2])), s = hypot(sx, sz);
    double drop = s * s / (2.0 * R) * 1.002 + 0.01, grow = s * (s * s / (6.0 * R * R) + 0.002) + 0.01;
    mn[1] -= (float)drop;
    mn[0] -= (float)grow; mx[0] += (float)grow;
    mn[2] -= (float)grow; mx[2] += (float)grow;
}

/* --- The lowest ground ------------------------------------------------------------ */

/* The land's own bound, and the city's ground at its lowest: a settlement's
 * relief down, sunk by at most that relief again and the clearance
 * (terrain_urban_sink, every octave left out). */
double terrain_planet_lowest(void) {
    double lo, hi, rlo, relief;
    terrain_land_range(1e9, &lo, &hi);
    plan_settlement_relief_range(&rlo, &relief);
    return fmin(lo, -2.0 * relief - TERRAIN_URBAN_SINK_CLEAR_M - terrain_lift_bound()) - 1.0;
}

/* --- Where the sea will be ----------------------------------------------------------
 * The planet has no water yet. When it does, the sea will fill the Earth's
 * own oceans, up to SEA_MARGIN_M above sea level (without a bake, the basins
 * of the stand-in below SEA_LEVEL_MACRO). The margin is the Earth read at the
 * lift's spacing, as coarse as the plan's sea grid; the shore as finely as
 * its caller asks. */
#define SEA_MARGIN_M    20.0
#define SEA_LEVEL_MACRO (-0.12)

double terrain_shore(TerrainDir p, double spacing) {
    return terrain_earth_loaded() ? terrain_earth_height(p, spacing) : TERRAIN_MACRO_M * (terrain_macro(p, 0.0) - SEA_LEVEL_MACRO);
}

double terrain_sea_margin_dir(TerrainDir p) {
    return terrain_shore(p, TERRAIN_LIFT_SPACING) - (terrain_earth_loaded() ? SEA_MARGIN_M : 0.0);
}

double terrain_sea_margin(double x, double z) {
    TerrainChart wc = terrain_world_chart();
    return terrain_sea_margin_dir(terrain_chart_dir(&wc, x, z));
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
