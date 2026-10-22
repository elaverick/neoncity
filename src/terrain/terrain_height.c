#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The urban relief at a point, its settlement's (m); 0 with the city on a datum. */
static double urban_relief(double x, double z) {
    return TERRAIN_URBAN_GRADED ? plan_settlement(plan_settlement_near(x, z)).relief_m : 0.0;
}

/* The most relief any settlement's ground has in the rectangle (m). */
static double urban_relief_max(double x0, double z0, double x1, double z1) {
    double lo, hi;
    plan_settlement_relief_in(x0, z0, x1, z1, &lo, &hi);
    return TERRAIN_URBAN_GRADED ? hi : 0.0;
}

/* Within this distance of the megacity's centre its own edge measure is under
 * TERRAIN_MACRO_FROM (the warp moves a point by at most sqrt(2) times the
 * octaves' amplitudes), so the planet's highlands (M * macro) are 0. */
#define LIFT_CLEAR (TERRAIN_MACRO_FROM + CITY_RADIUS - 1.4142135623730951 * (CITY_WARP1_M + CITY_WARP2_M) - 1.0)

static double home_distance(double x, double z) {
    DVec2 c = plan_town_centre(0, 0);
    return hypot(x - c.x, z - c.z);
}

/* The settlements' ground rises and falls with the planet's highlands, as the
 * land round them does (terrain_land_parts): M * macro, at full detail. */
static double urban_lift(double x, double z) {
    return home_distance(x, z) <= LIFT_CLEAR ? 0.0 : terrain_lift(x, z, plan_home_edge(x, z), 0.0);
}

int terrain_octaves(double wave, int octaves, double spacing) {
    int used = 0;
    while (used < octaves && ldexp(wave, -used) >= TERRAIN_LOD_SAMPLES_PER_WAVE * spacing) used++;
    return used;
}

static double urban_shape_lod(double x, double z, double spacing) {
    return noise_fbm2_band(WORLD_SEED ^ 0x7E11u, x / TERRAIN_URBAN_WAVE, z / TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES,
                           terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, spacing));
}

double terrain_urban_shape(double x, double z) { return urban_shape_lod(x, z, 0.0); }

/* Each field is evaluated only where its weight is non-zero; that skips work
 * without changing any height. *w gets the urban weight. The planet's
 * highlands (lift) are under the settlement's ground as under the land; round
 * the megacity they are 0, and there the sum is as it was before settlements
 * had them, operation for operation. The land reads the direction p, or
 * without one the chart's at (x, z). */
static double height_lod(double x, double z, const TerrainDir *p, double spacing, double *w, double *relief, int roads) {
    double home, edge = plan_city_edges(x, z, &home), h = 0.0, lift = 0.0;
    *w = plan_urban_weight(edge);
    *relief = *w > 0.0 ? urban_relief(x, z) : 0.0;
    if (*w < 1.0) {
        TerrainChart wc = terrain_world_chart();
        TerrainDir d = p ? *p : terrain_chart_dir(&wc, x, z);
        h += (1.0 - *w) * terrain_land_parts(d, edge, home, spacing, &lift);
    } else lift = terrain_lift(x, z, home, spacing);
    if (*w > 0.0) {
        if (lift == 0.0) { if (*relief > 0.0) h += *w * *relief * urban_shape_lod(x, z, spacing); }
        else h += *w * ((*relief > 0.0 ? *relief * urban_shape_lod(x, z, spacing) : 0.0) + lift);
    }
    /* The roads between the settlements: the ground takes them where the
     * land does, never under the city's own ground. */
    if (roads && *w < 1.0 && spacing <= PLAN_ROAD_LOD_SPACING) {
        double rh, wr = plan_road_ground(x, z, h, spacing, &rh);
        if (wr > 0.0) h += wr * (1.0 - *w) * (rh - h);
    }
    return h;
}

double terrain_height_lod(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, NULL, spacing, &w, &relief, 1);
}

double terrain_height(double x, double z) { return terrain_height_lod(x, z, 0.0); }

/* height_lod at w = 1: 0 + 1 * (relief * shape + lift) is exactly this. */
double terrain_urban_height(double x, double z) {
    double relief = urban_relief(x, z), lift = urban_lift(x, z);
    double h = relief > 0.0 ? relief * urban_shape_lod(x, z, 0.0) : 0.0;
    if (lift != 0.0) h += lift;
    return h;
}

double terrain_urban_lift(double x, double z) { return urban_lift(x, z); }

/* A bound on urban_shape's slope (per metre): value noise of lattice
 * wavelength L, mapped to [-1, 1], changes by at most 3 / L per metre along
 * each axis (noise_value2), so by 3 sqrt(2) / L in any direction; each octave
 * adds that times its weight. */
static double urban_shape_slope(void) {
    double wsum = 0.0, g = 0.0;
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) wsum += ldexp(1.0, -k);
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) g += 3.0 * sqrt(2.0) * ldexp(1.0, -k) / wsum / ldexp(TERRAIN_URBAN_WAVE, -k);
    return g;
}

/* urban_shape over the rectangle lies within its slope bound times the
 * half-diagonal of its value at the centre, and in [-1, 1] (a fractal sum of
 * unit amplitude). The height is a relief between the least and the most of
 * any settlement times that, so it lies between the extreme products; and
 * the lift within its own range (0 near the megacity). */
void terrain_urban_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0);
    double sc = terrain_urban_shape(cx, cz), reach = urban_shape_slope() * half;
    double slo = fmax(-1.0, sc - reach), shi = fmin(1.0, sc + reach), rlo, rhi;
    plan_settlement_relief_in(x0, z0, x1, z1, &rlo, &rhi);
    if (!TERRAIN_URBAN_GRADED) rlo = rhi = 0.0;
    *lo = fmin(rlo * slo, rhi * slo) - 1e-3;   /* rounding: the shape is a float */
    *hi = fmax(rlo * shi, rhi * shi) + 1e-3;
    if (home_distance(cx, cz) + half > LIFT_CLEAR) {   /* 0 inside LIFT_CLEAR */
        double llo, lhi;
        terrain_urban_lift_range(x0, z0, x1, z1, &llo, &lhi);
        *lo += llo;
        *hi += lhi;
    }
}

/* How far the urban relief drawn at `spacing` (interpolated linearly over the
 * level's triangles) can stray from the full relief, the city's ground. An
 * octave left out counts in full. A kept one, of amplitude a and lattice
 * wavelength L, is value noise eased by a smoothstep, whose second derivative
 * is at most 12 a / L^2; linear interpolation over a cell of diagonal D strays
 * by at most that times D^2 / 8. Half as much again, for the bilinear easing's
 * cross terms. */
double terrain_urban_lod_error(double spacing, double relief) {
    if (spacing <= 0.0 || relief <= 0.0) return 0.0;
    int used = terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, spacing);
    double wsum = 0.0, err = 0.0, D2 = 2.0 * spacing * spacing;
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) wsum += ldexp(1.0, -k);
    for (int k = 0; k < TERRAIN_URBAN_OCTAVES; ++k) {
        double a = relief * ldexp(1.0, -k) / wsum, L = ldexp(TERRAIN_URBAN_WAVE, -k);
        err += k < used ? 1.5 * 12.0 * a / (L * L) * D2 / 8.0 : a;
    }
    return err;
}

static double surface_lod(double x, double z, const TerrainDir *p, double spacing, float *urban) {
    double w, relief, h = height_lod(x, z, p, spacing, &w, &relief, 1);
    if (urban) *urban = (float)w;
    return h - terrain_urban_sink(spacing, relief) * w;
}

double terrain_surface_lod(double x, double z, double spacing, float *urban) { return surface_lod(x, z, NULL, spacing, urban); }

/* The land at p itself; the settlements, their ground and the roads at its
 * place in the world's chart. */
double terrain_height_dir(TerrainDir p, double spacing) {
    TerrainChart wc = terrain_world_chart();
    double x, z, w, relief;
    terrain_dir_chart(&wc, p, &x, &z);
    return height_lod(x, z, &p, spacing, &w, &relief, 1);
}

double terrain_surface_dir(TerrainDir p, double spacing) {
    TerrainChart wc = terrain_world_chart();
    double x, z;
    terrain_dir_chart(&wc, p, &x, &z);
    return surface_lod(x, z, &p, spacing, NULL);
}

double terrain_urban_sink(double spacing, double relief) {
    return fmax(TERRAIN_URBAN_SINK_M, terrain_urban_lod_error(spacing, relief) + TERRAIN_URBAN_SINK_CLEAR_M);
}

double terrain_surface(double x, double z) { return terrain_surface_lod(x, z, 0.0, NULL); }

/* The height is a blend of urban (|h| <= its relief, on the lift) and land
 * (within terrain_land_range, which only widens with the edge measures), so
 * it is bounded by whichever of the two can reach into the rectangle. The
 * edge measures over the rectangle are bounded from its centre by
 * CITY_EDGE_RATE. Leaving octaves out (terrain_height_lod) only shrinks each
 * fractal sum, so the bound holds at every level of detail. A road blends the
 * ground towards its own height, which the plan bounds (plan_road_range). */
void terrain_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0), home;
    double e = plan_city_edges(cx, cz, &home), emin = e - CITY_EDGE_RATE * half, emax = e + CITY_EDGE_RATE * half;
    double hmax = home + CITY_EDGE_RATE * half;
    double l = 0.0, h = 0.0;
    if (emin < 0.0) {                                                        /* somewhere w > 0 */
        double lift = terrain_lift_bound(hmax), relief = urban_relief_max(x0, z0, x1, z1);
        l = -relief - lift; h = relief + lift;
    }
    if (emax > -CITY_BELT) {                                                 /* somewhere w < 1 */
        double ll, lh;
        terrain_land_range(emax, hmax, &ll, &lh);
        l = fmin(l, ll); h = fmax(h, lh);
        if (plan_road_range(x0, z0, x1, z1, &ll, &lh)) { l = fmin(l, ll); h = fmax(h, lh); }
    }
    *lo = l - 1e-3;                                                          /* rounding */
    *hi = h + 1e-3;
}

double terrain_height_bare(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, NULL, spacing, &w, &relief, 0);
}

/* The roads move the ground at a spacing by wr (1 - w) (rh - g) from the bare
 * ground g there; the difference from full detail is the bare grounds'
 * difference (terrain_land_lod_error's) and at most the two moves. */
double terrain_road_lod_error(double x, double z, double spacing) {
    double e = 0.0;
    for (int k = 0; k < 2; ++k) {
        double sp = k ? spacing : 0.0, w, relief, rh, g = height_lod(x, z, NULL, sp, &w, &relief, 0);
        if (w >= 1.0 || sp > PLAN_ROAD_LOD_SPACING) continue;
        double wr = plan_road_ground(x, z, g, sp, &rh);
        e += wr * (1.0 - w) * fabs(rh - g);
    }
    return e;
}
