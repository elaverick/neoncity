#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The land beyond the city (see terrain_internal.h): rolling ground, the
 * region's landforms and canyons, as pure functions of position. */

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }

/* --- The region ---------------------------------------------------------------
 * A warped frame, so neither the regions nor the belts run straight, and in it
 * the strike frame: u along the mountain belts, v across them. */
typedef struct Region {
    double wx, wz, u, v;
    float  w[TERRAIN_TYPES];
} Region;

/* --- The planet's highlands and basins ------------------------------------------
 * A fractal sum over the sphere, in [-1, 1], and how much of it the land
 * takes at an edge measure (M). */
static double macro_weight(double edge) {
    double t = (edge - TERRAIN_MACRO_FROM) / (TERRAIN_MACRO_FULL - TERRAIN_MACRO_FROM);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

static double macro_at(TerrainDir p, double spacing) {
    double k = PLANET_RADIUS / TERRAIN_MACRO_WAVE;
    return noise_fbm3_band(WORLD_SEED ^ 0x7E61u, p.x * k, p.y * k, p.z * k, TERRAIN_MACRO_OCTAVES,
                           terrain_octaves(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing));
}

/* M * macro at a point of the world's chart (p NULL) or of another chart;
 * its affinity bias uses the full sum, whatever the spacing, so the region's
 * weights don't depend on the level of detail. */
static void macro_of(double x, double z, const TerrainDir *p, double edge, double spacing, double *lift, double *bias) {
    *lift = *bias = 0.0;
    double mw = macro_weight(edge);
    if (mw <= 0.0) return;
    TerrainDir d = p ? *p : terrain_chart_dir(x, z);
    double full = macro_at(d, 0.0);
    int all = terrain_octaves(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing) == TERRAIN_MACRO_OCTAVES;
    *lift = mw * TERRAIN_MACRO_M * (all ? full : macro_at(d, spacing));
    *bias = mw * TERRAIN_MACRO_BIAS * full;
}

static void region_at(double x, double z, double bias, Region *r) {
    double ww = TERRAIN_WARP_WAVE;
    r->wx = x + TERRAIN_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E31u, x / ww, z / ww) - 1.0);
    r->wz = z + TERRAIN_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E32u, x / ww, z / ww) - 1.0);
    r->u = TERRAIN_STRIKE_X * r->wx + TERRAIN_STRIKE_Z * r->wz;
    r->v = -TERRAIN_STRIKE_Z * r->wx + TERRAIN_STRIKE_X * r->wz;
    const double rw = TERRAIN_REGION_WAVE;
    float a[TERRAIN_TYPES];
    a[TERRAIN_PLAINS] = (float)TERRAIN_PLAINS_BIAS;
    a[TERRAIN_HILLS] = noise_value2(WORLD_SEED ^ 0x7E33u, r->wx / rw, r->wz / rw);
    a[TERRAIN_MOUNTAINS] = noise_value2(WORLD_SEED ^ 0x7E34u, r->u / (rw * TERRAIN_BELT_ALONG), r->v / (rw * TERRAIN_BELT_ACROSS));
    a[TERRAIN_PLATEAU] = noise_value2(WORLD_SEED ^ 0x7E35u, r->wx / rw, r->wz / rw);
    if (bias != 0.0) {   /* highlands raise the mountains and plateaus, basins the plains */
        a[TERRAIN_PLAINS] -= (float)bias;
        a[TERRAIN_MOUNTAINS] += (float)bias;
        a[TERRAIN_PLATEAU] += (float)(0.5 * bias);
    }
    /* Each type's lead over the margin below the best, cubed: continuous, and
     * smooth where a type's weight reaches 0. The best always has the full
     * margin, so the sum never vanishes. */
    float best = a[0], sum = 0.0f, k[TERRAIN_TYPES];
    for (int t = 1; t < TERRAIN_TYPES; ++t) best = maxf(best, a[t]);
    for (int t = 0; t < TERRAIN_TYPES; ++t) {
        float lead = maxf(0.0f, a[t] - (best - (float)TERRAIN_REGION_MARGIN));
        k[t] = lead * lead * lead;
        sum += k[t];
    }
    for (int t = 0; t < TERRAIN_TYPES; ++t) r->w[t] = k[t] / sum;
}

/* Ridged fractal sum in [0, 1] over the strike frame (u along the belt, v
 * across, in units of TERRAIN_RIDGE_WAVE): each octave's value noise folded
 * about its middle and squared, so its crests are sharp, and weighted by the
 * octave before, so the fine ridges stand on the coarse ones and the valleys
 * stay smoother. The first two octaves, the main ridges, are stretched along
 * the strike; each finer one is turned a little further, so the spurs off
 * them run every way. Octave weights as noise_fbm2_band; only the first
 * `used` count. */
static const double RIDGE_TURN[8][2] = {   /* (cos, sin) of 0.9 k radians */
    { 1.0, 0.0 }, { 0.0, 0.0 }, { -0.2272020946930871, 0.9738476308781951 }, { -0.9040721420170612, 0.4273798802338298 },
    { -0.896758416334147, -0.44252044329485246 }, { -0.2107957994307797, -0.977530117665097 },
    { 0.6346928759426347, -0.7727644875559871 }, { 0.9998586363834151, 0.016813900484349713 },
};
_Static_assert(TERRAIN_RIDGE_OCTAVES <= 8, "RIDGE_TURN holds 8 octaves' turns");

static double ridged(uint32_t seed, double u, double v, int octaves, int used) {
    double sum = 0.0, wsum = 0.0, a = 1.0, f = 1.0, carry = 1.0;
    for (int k = 0; k < octaves; ++k) {
        if (k < used) {
            double x = u / TERRAIN_RIDGE_ALONG, z = v;
            if (k >= 2) {
                double c = RIDGE_TURN[k][0], s = RIDGE_TURN[k][1];
                x = c * u - s * v; z = s * u + c * v;
            }
            double n = 1.0 - fabs(2.0 * noise_value2(hash_combine(seed, (uint32_t)k), x * f + 17.31 * k, z * f - 41.77 * k) - 1.0);
            n *= n * carry;
            carry = fmin(1.0, 2.0 * n);
            sum += a * n;
        }
        wsum += a;
        a *= 0.5;
        f *= 2.0;
    }
    return sum / wsum;
}

static double fbm_lod(uint32_t seed, double x, double z, double wave, int octaves, double spacing) {
    return noise_fbm2_band(seed, x / wave, z / wave, octaves, terrain_octaves(wave, octaves, spacing));
}

/* The plateau's step: 0 below the mask's level, 1 a step above it; its
 * steepest, 1.5 / TERRAIN_MASK_STEP per unit of mask, in the middle. */
static double plateau_step(double mask) {
    return smoothstepf(0.0f, 1.0f, (float)((mask - TERRAIN_MASK_LEVEL) / TERRAIN_MASK_STEP));
}

/* Type t's heights at the point (m). */
static double type_height(int t, double x, double z, const Region *r, double spacing) {
    switch (t) {
    case TERRAIN_PLAINS:
        return TERRAIN_PLAINS_M * fbm_lod(WORLD_SEED ^ 0x7E41u, x, z, TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
    case TERRAIN_HILLS:
        return TERRAIN_HILLS_M * fbm_lod(WORLD_SEED ^ 0x7E42u, x, z, TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
    case TERRAIN_MOUNTAINS: {
        int used = terrain_octaves(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
        double ww = TERRAIN_RIDGE_WARP_WAVE;
        double u = r->u + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E46u, r->u / ww, r->v / ww) - 1.0);
        double v = r->v + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E47u, r->u / ww, r->v / ww) - 1.0);
        return TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M * ridged(WORLD_SEED ^ 0x7E43u, u / TERRAIN_RIDGE_WAVE, v / TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, used);
    }
    default: {
        double mask = fbm_lod(WORLD_SEED ^ 0x7E44u, x, z, TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES, spacing);
        return TERRAIN_PLATEAU_M * plateau_step(mask) + TERRAIN_TOP_M * fbm_lod(WORLD_SEED ^ 0x7E45u, x, z, TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES, spacing);
    }
    }
}

static double region_rise(double edge) {
    return smoothstepf((float)TERRAIN_REGION_FROM, (float)(TERRAIN_REGION_FROM + TERRAIN_LAND_RISE), (float)edge);
}

static double canyon_rise(double edge) {
    return smoothstepf((float)TERRAIN_CANYON_FROM, (float)TERRAIN_CANYON_FULL, (float)edge);
}

/* --- Canyons ------------------------------------------------------------------
 * Cell (i, j) of the canyon lattice holds one canyon or none: a meandering
 * centre line of CANYON_SEGS segments about a straight chord through a point
 * in the middle of the cell, two sine meanders across it. Its depth tapers to
 * nothing at both ends. Across the line: a level floor, then walls climbing
 * at TERRAIN_CANYON_WALL on average to the rim. A canyon's centre lies in
 * the middle 0.4 of its cell and it reaches less than 0.6 of a cell from it
 * (CANYON_REACH), so the 3x3 cells round a point hold every canyon that can
 * cut it. */
#define CANYON_SEGS 16

typedef struct Canyon {
    double cx, cz, dx, dz, half;   /* the chord: centre, unit direction, half its length */
    double depth, floor;           /* deepest (m), the floor's half-width (m) */
    double m1, f1, p1, m2, f2, p2; /* meanders: amplitude (m), cycles along the chord, phase */
} Canyon;

/* The farthest any canyon reaches from its centre: chord, meanders, floor and walls. */
#define CANYON_REACH (10000.0 + 1200.0 + 300.0 + 90.0 + TERRAIN_CANYON_DEEP_M / TERRAIN_CANYON_WALL)
_Static_assert(CANYON_REACH < 0.6 * TERRAIN_CANYON_CELL, "a canyon must stay within its cell's neighbours");

/* Cell (i, j)'s canyon, if it has one that can reach (x, z). */
static int canyon_of(int i, int j, double x, double z, Canyon *c) {
    uint32_t h = hash2i(WORLD_SEED ^ 0x7E51u, i, j);
    if (hf(h, 0) >= (float)TERRAIN_CANYON_CHANCE) return 0;
    c->cx = (i + 0.3 + 0.4 * hf(h, 1)) * TERRAIN_CANYON_CELL;
    c->cz = (j + 0.3 + 0.4 * hf(h, 2)) * TERRAIN_CANYON_CELL;
    if ((x - c->cx) * (x - c->cx) + (z - c->cz) * (z - c->cz) >= CANYON_REACH * CANYON_REACH) return 0;
    double a = 2.0 * PI_F * hf(h, 3);
    c->dx = cos(a); c->dz = sin(a);
    c->half = 6000.0 + 4000.0 * hf(h, 4);
    c->depth = 180.0 + (TERRAIN_CANYON_DEEP_M - 180.0) * hf(h, 5);
    c->floor = 40.0 + 50.0 * hf(h, 6);
    c->m1 = 600.0 + 600.0 * hf(h, 7); c->f1 = 1.5 + hf(h, 8); c->p1 = 2.0 * PI_F * hf(h, 9);
    c->m2 = 150.0 + 150.0 * hf(h, 10); c->f2 = 4.0 + 2.0 * hf(h, 11); c->p2 = 2.0 * PI_F * hf(h, 12);
    return 1;
}

/* How deep canyon c cuts at (x, z): the deepest of its segments' cuts, each
 * from the nearest point on the segment, so the cut is continuous. */
static double canyon_cut(const Canyon *c, double x, double z) {
    double reach = c->half + c->m1 + c->m2 + c->floor + c->depth / TERRAIN_CANYON_WALL;
    double rx = x - c->cx, rz = z - c->cz;
    if (rx * rx + rz * rz >= reach * reach) return 0.0;
    /* the meanders' sines along the vertices, by turning each a step at a time */
    double d1 = 2.0 * PI_F * c->f1 / CANYON_SEGS, d2 = 2.0 * PI_F * c->f2 / CANYON_SEGS;
    double s1 = sin(c->p1), c1 = cos(c->p1), sd1 = sin(d1), cd1 = cos(d1);
    double s2 = sin(c->p2), c2 = cos(c->p2), sd2 = sin(d2), cd2 = cos(d2);
    double px = 0.0, pz = 0.0, cut = 0.0;
    for (int k = 0; k <= CANYON_SEGS; ++k) {
        double t = (double)k / CANYON_SEGS, s = 2.0 * t - 1.0, off = c->m1 * s1 + c->m2 * s2, n;
        n = s1 * cd1 + c1 * sd1; c1 = c1 * cd1 - s1 * sd1; s1 = n;
        n = s2 * cd2 + c2 * sd2; c2 = c2 * cd2 - s2 * sd2; s2 = n;
        double qx = c->cx + c->dx * c->half * s - c->dz * off, qz = c->cz + c->dz * c->half * s + c->dx * off;
        if (k > 0) {
            double ex = qx - px, ez = qz - pz, l2 = ex * ex + ez * ez;
            double u = l2 > 0.0 ? ((x - px) * ex + (z - pz) * ez) / l2 : 0.0;
            u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
            double d = hypot(x - px - u * ex, z - pz - u * ez), tt = (k - 1 + u) / CANYON_SEGS;
            double deep = c->depth * smoothstepf(0.0f, 0.18f, (float)tt) * smoothstepf(1.0f, 0.82f, (float)tt);
            double rim = c->floor + deep / TERRAIN_CANYON_WALL;
            if (d < rim) cut = fmax(cut, deep * (1.0 - smoothstepf((float)c->floor, (float)rim, (float)d)));
        }
        px = qx; pz = qz;
    }
    return cut;
}

double terrain_canyon_depth(double x, double z) {
    int i0 = (int)floor(x / TERRAIN_CANYON_CELL), j0 = (int)floor(z / TERRAIN_CANYON_CELL);
    double cut = 0.0;
    Canyon c;
    for (int j = j0 - 1; j <= j0 + 1; ++j)
        for (int i = i0 - 1; i <= i0 + 1; ++i)
            if (canyon_of(i, j, x, z, &c)) cut = fmax(cut, canyon_cut(&c, x, z));
    return cut;
}

/* --- The land -------------------------------------------------------------- */

static double land_lod(double x, double z, const TerrainDir *p, double edge, double spacing) {
    double h = TERRAIN_BASE_M * fbm_lod(WORLD_SEED ^ 0x7E12u, x, z, TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing);
    double e = region_rise(edge), c = canyon_rise(edge), lift, bias;
    macro_of(x, z, p, edge, spacing, &lift, &bias);
    h += lift;
    if (e > 0.0) {
        Region r;
        region_at(x, z, bias, &r);
        double sum = 0.0;
        for (int t = 0; t < TERRAIN_TYPES; ++t)
            if (r.w[t] > 0.0f) sum += r.w[t] * type_height(t, x, z, &r, spacing);
        h += e * sum;
    }
    if (c > 0.0) h -= c * terrain_canyon_depth(x, z);
    return h;
}

double terrain_land_lod(double x, double z, double edge, double spacing) { return land_lod(x, z, NULL, edge, spacing); }
double terrain_land_at(double x, double z, TerrainDir p, double edge, double spacing) { return land_lod(x, z, &p, edge, spacing); }

double terrain_land(double x, double z) { return terrain_land_lod(x, z, plan_city_edge(x, z), 0.0); }

void terrain_landform(double x, double z, TerrainLandform *out) {
    Region r;
    double lift, bias;
    macro_of(x, z, NULL, plan_city_edge(x, z), 0.0, &lift, &bias);
    region_at(x, z, bias, &r);
    for (int t = 0; t < TERRAIN_TYPES; ++t) out->w[t] = r.w[t];
    out->canyon = (float)terrain_canyon_depth(x, z);
}

/* Base, macro, the types and canyons each within their reach: the hills
 * reach lowest, the mountains highest, the canyons cut at most their deepest;
 * M, E and C only grow with the edge measure. */
void terrain_land_range(double edge, double *lo, double *hi) {
    double e = region_rise(edge), m = macro_weight(edge) * TERRAIN_MACRO_M;
    *lo = -(TERRAIN_BASE_M + m + e * TERRAIN_HILLS_M + canyon_rise(edge) * TERRAIN_CANYON_DEEP_M);
    *hi = TERRAIN_BASE_M + m + e * (TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M);
}

/* Leaving octaves out of a fractal sum moves it by at most their share of its
 * weight (each octave's noise is bounded), the same for the ridged sum (each
 * octave adds 0 .. its weight, and the carry only reaches later octaves). The
 * plateau's mask moves the step through its slope. The region's weights and
 * the canyons don't depend on the spacing. */
double terrain_land_lod_error(double x, double z, double spacing) {
    double err = TERRAIN_BASE_M * terrain_dropped(TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing);
    double edge = plan_city_edge(x, z), e = region_rise(edge), lift, bias;
    err += macro_weight(edge) * TERRAIN_MACRO_M * terrain_dropped(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing);
    if (e > 0.0) {
        Region r;
        macro_of(x, z, NULL, edge, 0.0, &lift, &bias);
        region_at(x, z, bias, &r);
        double t[TERRAIN_TYPES];
        t[TERRAIN_PLAINS] = TERRAIN_PLAINS_M * terrain_dropped(TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
        t[TERRAIN_HILLS] = TERRAIN_HILLS_M * terrain_dropped(TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
        t[TERRAIN_MOUNTAINS] = TERRAIN_RIDGE_M * terrain_dropped(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
        t[TERRAIN_PLATEAU] = TERRAIN_PLATEAU_M * 1.5 / TERRAIN_MASK_STEP * terrain_dropped(TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES, spacing) +
                             TERRAIN_TOP_M * terrain_dropped(TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES, spacing);
        for (int k = 0; k < TERRAIN_TYPES; ++k) err += e * r.w[k] * t[k];
    }
    return err;
}

double terrain_dropped(double wave, int octaves, double spacing) {
    int used = terrain_octaves(wave, octaves, spacing);
    double drop = 0.0, total = 0.0;
    for (int k = 0; k < octaves; ++k) {
        total += ldexp(1.0, -k);
        if (k >= used) drop += ldexp(1.0, -k);
    }
    return drop / total;
}

uint32_t terrain_lod_key(double spacing) {
    static const struct { double wave; int octaves; } sums[] = {
        { TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES }, { TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES },
        { TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES }, { TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES },
        { TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES }, { TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES },
        { TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES }, { TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES },
    };
    uint32_t key = 0;
    for (size_t k = 0; k < sizeof(sums) / sizeof(sums[0]); ++k) key = key * 16u + (uint32_t)terrain_octaves(sums[k].wave, sums[k].octaves, spacing);
    return key;
}
