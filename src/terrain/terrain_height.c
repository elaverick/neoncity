#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

static double urban_relief(void) { return TERRAIN_URBAN_GRADED ? TERRAIN_URBAN_RELIEF_M : 0.0; }

double terrain_urban_shape(double x, double z) {
    return noise_fbm2(WORLD_SEED ^ 0x7E11u, x / TERRAIN_URBAN_WAVE, z / TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES);
}

double terrain_land_amplitude(double edge) {
    return TERRAIN_LAND_NEAR_M + (TERRAIN_LAND_FAR_M - TERRAIN_LAND_NEAR_M) * smoothstepf(0.0f, (float)TERRAIN_LAND_RISE, (float)edge);
}

static double land_at(double x, double z, double edge) {
    return terrain_land_amplitude(edge) * noise_fbm2(WORLD_SEED ^ 0x7E12u, x / TERRAIN_LAND_WAVE, z / TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES);
}

double terrain_land(double x, double z) { return land_at(x, z, plan_city_edge(x, z)); }

/* Each field is evaluated only where its weight is non-zero; that skips work
 * without changing any height. */
double terrain_height(double x, double z) {
    double edge = plan_city_edge(x, z);
    double w = plan_urban_weight(edge), h = 0.0;
    if (w < 1.0) h += (1.0 - w) * land_at(x, z, edge);
    if (w > 0.0 && urban_relief() > 0.0) h += w * urban_relief() * terrain_urban_shape(x, z);
    return h;
}

/* The height is a blend of urban (|h| <= its relief) and land (|h| <= its
 * amplitude, which grows with the edge measure), so it is bounded by whichever
 * of the two can reach into the rectangle. The edge measure over the
 * rectangle is bounded from its centre by CITY_EDGE_RATE. */
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
