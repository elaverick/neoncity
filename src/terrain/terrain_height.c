#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The urban relief at a point, its settlement's (m); 0 with the city on a datum. */
static double urban_relief(double x, double z) {
    return TERRAIN_URBAN_GRADED ? plan_settlement(plan_settlement_near(x, z)).relief_m : 0.0;
}

/* The most relief any settlement's ground has (m). */
static double urban_relief_max(void) {
    double r = 0.0;
    for (int k = 0; k < PLAN_SETTLEMENTS && TERRAIN_URBAN_GRADED; ++k) r = fmax(r, plan_settlement(k).relief_m);
    return r;
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

double terrain_land_amplitude(double edge) {
    return TERRAIN_LAND_NEAR_M + (TERRAIN_LAND_FAR_M - TERRAIN_LAND_NEAR_M) * smoothstepf(0.0f, (float)TERRAIN_LAND_RISE, (float)edge);
}

static double land_at(double x, double z, double edge, double spacing) {
    return terrain_land_amplitude(edge) * noise_fbm2_band(WORLD_SEED ^ 0x7E12u, x / TERRAIN_LAND_WAVE, z / TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES,
                                                          terrain_octaves(TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES, spacing));
}

double terrain_land(double x, double z) { return land_at(x, z, plan_city_edge(x, z), 0.0); }

/* Each field is evaluated only where its weight is non-zero; that skips work
 * without changing any height. *w gets the urban weight. */
static double height_lod(double x, double z, double spacing, double *w, double *relief) {
    double edge = plan_city_edge(x, z), h = 0.0;
    *w = plan_urban_weight(edge);
    *relief = *w > 0.0 ? urban_relief(x, z) : 0.0;
    if (*w < 1.0) h += (1.0 - *w) * land_at(x, z, edge, spacing);
    if (*w > 0.0 && *relief > 0.0) h += *w * *relief * urban_shape_lod(x, z, spacing);
    return h;
}

double terrain_height_lod(double x, double z, double spacing) {
    double w, relief;
    return height_lod(x, z, spacing, &w, &relief);
}

double terrain_height(double x, double z) { return terrain_height_lod(x, z, 0.0); }

/* height_lod's urban term at w = 1: 0 + 1 * relief * shape is relief * shape exactly. */
double terrain_urban_height(double x, double z) {
    double relief = urban_relief(x, z);
    return relief > 0.0 ? relief * urban_shape_lod(x, z, 0.0) : 0.0;
}

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
 * any settlement times that, so it lies between the extreme products. */
void terrain_urban_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0);
    double sc = terrain_urban_shape(cx, cz), reach = urban_shape_slope() * half;
    double slo = fmax(-1.0, sc - reach), shi = fmin(1.0, sc + reach), rlo = 1e300, rhi = 0.0;
    for (int k = 0; k < PLAN_SETTLEMENTS; ++k) {
        double r = TERRAIN_URBAN_GRADED ? plan_settlement(k).relief_m : 0.0;
        rlo = fmin(rlo, r); rhi = fmax(rhi, r);
    }
    *lo = fmin(rlo * slo, rhi * slo) - 1e-3;   /* rounding: the shape is a float */
    *hi = fmax(rlo * shi, rhi * shi) + 1e-3;
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

double terrain_surface_lod(double x, double z, double spacing, float *urban) {
    double w, relief, h = height_lod(x, z, spacing, &w, &relief);
    if (urban) *urban = (float)w;
    return h - terrain_urban_sink(spacing, relief) * w;
}

double terrain_urban_sink(double spacing, double relief) {
    return fmax(TERRAIN_URBAN_SINK_M, terrain_urban_lod_error(spacing, relief) + TERRAIN_URBAN_SINK_CLEAR_M);
}

double terrain_surface(double x, double z) { return terrain_surface_lod(x, z, 0.0, NULL); }

/* The height is a blend of urban (|h| <= its relief) and land (|h| <= its
 * amplitude, which grows with the edge measure), so it is bounded by whichever
 * of the two can reach into the rectangle. The edge measure over the
 * rectangle is bounded from its centre by CITY_EDGE_RATE. Leaving octaves out
 * (terrain_height_lod) only shrinks each fractal sum, so the bound holds at
 * every level of detail. */
void terrain_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0);
    double e = plan_city_edge(cx, cz), emin = e - CITY_EDGE_RATE * half, emax = e + CITY_EDGE_RATE * half;
    double a = 0.0;
    if (emin < 0.0) a = urban_relief_max();                                   /* somewhere w > 0 */
    if (emax > -CITY_BELT) a = fmax(a, terrain_land_amplitude(emax));         /* somewhere w < 1 */
    a += 1e-3;                                                                /* rounding */
    *lo = -a;
    *hi = a;
}
