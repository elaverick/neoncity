#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

static double urban_relief(void) { return TERRAIN_URBAN_GRADED ? TERRAIN_URBAN_RELIEF_M : 0.0; }

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
static double height_lod(double x, double z, double spacing, double *w) {
    double edge = plan_city_edge(x, z), h = 0.0;
    *w = plan_urban_weight(edge);
    if (*w < 1.0) h += (1.0 - *w) * land_at(x, z, edge, spacing);
    if (*w > 0.0 && urban_relief() > 0.0) h += *w * urban_relief() * urban_shape_lod(x, z, spacing);
    return h;
}

double terrain_height_lod(double x, double z, double spacing) {
    double w;
    return height_lod(x, z, spacing, &w);
}

double terrain_height(double x, double z) { return terrain_height_lod(x, z, 0.0); }

/* height_lod's urban term at w = 1: 0 + 1 * relief * shape is relief * shape exactly. */
double terrain_urban_height(double x, double z) {
    return urban_relief() > 0.0 ? urban_relief() * urban_shape_lod(x, z, 0.0) : 0.0;
}

/* urban_shape is a fractal sum of unit amplitude: |shape| <= 1 everywhere. */
void terrain_urban_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    (void)x0; (void)z0; (void)x1; (void)z1;
    *lo = -urban_relief() - 1e-3;
    *hi = urban_relief() + 1e-3;
}

double terrain_surface_lod(double x, double z, double spacing, float *urban) {
    double w, h = height_lod(x, z, spacing, &w);
    if (urban) *urban = (float)w;
    return h - TERRAIN_URBAN_SINK_M * w;
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
    if (emin < 0.0) a = urban_relief();                                       /* somewhere w > 0 */
    if (emax > -CITY_BELT) a = fmax(a, terrain_land_amplitude(emax));         /* somewhere w < 1 */
    a += 1e-3;                                                                /* rounding */
    *lo = -a;
    *hi = a;
}
