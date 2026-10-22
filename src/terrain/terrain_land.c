#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The land beyond the city (see terrain_internal.h): rolling ground, the
 * region's landforms and canyons, as pure functions of position. */

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }

#define PI_D 3.14159265358979323846

/* --- The region ---------------------------------------------------------------
 * A warped frame, so neither the regions nor the belts run straight, and in it
 * the strike frame: u along the mountain belts, v across them. */
typedef struct Region {
    double wx, wz, u, v;
    float  w[TERRAIN_TYPES];
} Region;

/* What the planet gives the land at a point (macro_of): the lift, M * macro;
 * and for the landforms, the stand-in's affinity bias (without a bake), or
 * the Earth's own affinities and their share of the region's, the ridges'
 * two frames either side of the strike and the blend between them, how much
 * of the mountains' height the data leaves them, the roughness, and how much
 * of the canyons' cut the sea floor leaves. Where M is 0 the landforms are
 * the region's own: no bias, no share, frame 0 alone and every scale 1. */
typedef struct Macro {
    double lift, bias, share;
    float  a[TERRAIN_TYPES];
    int    k0, k1;
    double kt, amp, rough, canyon;
} Macro;

/* The ridges' frames: the default strike (TERRAIN_STRIKE_*) turned towards
 * +z by k pi / TERRAIN_STRIKES, as (cos, sin) of the turn. */
static const double STRIKE_TURN[TERRAIN_STRIKES][2] = {
    { 1.0, 0.0 }, { 0.9238795325112867, 0.3826834323650898 }, { 0.7071067811865476, 0.7071067811865476 },
    { 0.3826834323650898, 0.9238795325112867 }, { 0.0, 1.0 }, { -0.3826834323650898, 0.9238795325112867 },
    { -0.7071067811865476, 0.7071067811865476 }, { -0.9238795325112867, 0.3826834323650898 },
};
_Static_assert(TERRAIN_STRIKES == 8, "STRIKE_TURN holds 8 frames");

/* --- The planet's highlands and basins ------------------------------------------
 * The Earth less the datum (terrain_internal.h), how much of it the land
 * takes at the megacity's edge measure (M), and how much of the Earth's own
 * detail at the nearest edge measure. The procedural stand-in is a fractal
 * sum over the sphere, in [-1, 1]. */
static double macro_weight(double edge) {
    double t = (edge - TERRAIN_MACRO_FROM) / (TERRAIN_MACRO_FULL - TERRAIN_MACRO_FROM);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

static double detail_rise(double edge) {
    return smoothstepf((float)TERRAIN_DETAIL_FROM, (float)TERRAIN_DETAIL_FULL, (float)edge);
}

static double macro_at(TerrainDir p, double spacing) {
    double k = PLANET_RADIUS / TERRAIN_MACRO_WAVE;
    return noise_fbm3_band(WORLD_SEED ^ 0x7E61u, p.x * k, p.y * k, p.z * k, TERRAIN_MACRO_OCTAVES,
                           terrain_octaves(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing));
}

double terrain_macro(TerrainDir p, double spacing) { return macro_at(p, spacing); }

/* M grows with the megacity's measure, and the Earth lies within its extremes. */
double terrain_lift_bound(double home) {
    double lo, hi, grade, e0 = terrain_earth_datum();
    terrain_earth_extremes(&lo, &hi, &grade);
    return macro_weight(home) * fmax(hi - e0, e0 - lo);
}

/* The Earth's affinity for each type, in [0, 1], from the relief round a
 * point and its elevation at the lift's spacing (m): much relief, mountains;
 * some, hills; high and level, plateaus; low and level, plains. Where the
 * hints override the landform, the override takes its share. */
static void forms_of(const TerrainEarthLand *el, double elev, float a[TERRAIN_TYPES]) {
    float r = (float)el->relief, high = smoothstepf(700.0f, 1500.0f, (float)elev), f[TERRAIN_TYPES];
    f[TERRAIN_MOUNTAINS] = smoothstepf(400.0f, 1100.0f, r);
    f[TERRAIN_HILLS] = smoothstepf(100.0f, 300.0f, r) * (1.0f - smoothstepf(700.0f, 1300.0f, r));
    f[TERRAIN_PLATEAU] = high * (1.0f - smoothstepf(250.0f, 650.0f, r));
    f[TERRAIN_PLAINS] = (1.0f - smoothstepf(150.0f, 400.0f, r)) * (1.0f - high);
    double o = 0.0;
    for (int t = 0; t < TERRAIN_TYPES; ++t) o += el->hint[t];
    for (int t = 0; t < TERRAIN_TYPES; ++t) a[t] = (float)((1.0 - o) * f[t] + el->hint[t]);
}

int terrain_earth_forms(TerrainDir p, float a[TERRAIN_TYPES]) {
    TerrainEarthLand el;
    if (!terrain_earth_land(p, &el)) return 0;
    forms_of(&el, terrain_earth_height(p, TERRAIN_LIFT_SPACING), a);
    return 1;
}

/* The ridges' strike at (x, z) of the world's chart, direction d: the
 * data's, turned from east and north into the chart's x and z there, blended
 * with the default strike by M times its coherence as doubled angles (a
 * strike has no sense); then the two frames either side of it. */
static void strike_frames(double x, double z, TerrainDir d, double mw, const TerrainEarthLand *el, Macro *m) {
    double w = mw * hypot(el->strike_e, el->strike_n), l = hypot(d.x, d.y);
    if (w <= 0.0 || l < 1e-9) return;   /* no strike, or at a pole */
    TerrainDir ax, up, az, east = { -d.y / l, d.x / l, 0.0 }, north = cross3(d, east);
    terrain_chart_frame(x, z, &ax, &up, &az);
    double half = 0.5 * atan2(el->strike_n, el->strike_e);
    TerrainDir s = add3(scale3(east, cos(half)), scale3(north, sin(half)));
    double a = 2.0 * atan2(dot3(s, az), dot3(s, ax)), a0 = 2.0 * atan2(TERRAIN_STRIKE_Z, TERRAIN_STRIKE_X);
    double cx = w * cos(a) + (1.0 - w) * cos(a0), cz = w * sin(a) + (1.0 - w) * sin(a0);
    if (cx == 0.0 && cz == 0.0) return;
    double turn = fmod(0.5 * (atan2(cz, cx) - a0), PI_D);   /* from the default strike towards +z */
    if (turn < 0.0) turn += PI_D;
    double q = turn * (TERRAIN_STRIKES / PI_D);
    int k = (int)floor(q);
    if (k >= TERRAIN_STRIKES) return;
    m->k0 = k;
    m->k1 = (k + 1) % TERRAIN_STRIKES;
    m->kt = smoothstepf(0.0f, 1.0f, (float)(q - k));
}

/* M * macro at a point of the world's chart (p NULL) or of another chart:
 * the Earth at TERRAIN_LIFT_SPACING (or coarser, at a coarser spacing), and
 * by the detail's rise at the edge measure `edge` the Earth at the spacing
 * itself. M reads the megacity's own edge measure, `home` (plan_city_edges).
 * With `forms`, the landforms' part too (see Macro): it reads the Earth at
 * the lift's spacing and the landform grid, whatever the spacing, so the
 * region's weights and scales don't depend on the level of detail. The data's
 * strike is read in the world's chart only; another chart keeps the default. */
static void macro_of(double x, double z, const TerrainDir *p, double edge, double home, double spacing, int forms, Macro *m) {
    m->lift = m->bias = m->share = 0.0;
    m->k0 = m->k1 = 0;
    m->kt = 0.0;
    m->amp = m->rough = m->canyon = 1.0;
    double mw = macro_weight(home);
    if (mw <= 0.0) return;
    TerrainDir d = p ? *p : terrain_chart_dir(x, z);
    double coarse = terrain_earth_height(d, fmax(spacing, TERRAIN_LIFT_SPACING)), r = detail_rise(edge);
    double l = coarse - terrain_earth_datum();
    if (r > 0.0) l += r * (terrain_earth_height(d, spacing) - coarse);
    m->lift = mw * l;
    if (!forms) return;
    TerrainEarthLand el;
    if (!terrain_earth_land(d, &el)) {
        m->bias = mw * TERRAIN_MACRO_BIAS * terrain_macro(d, 0.0);
        return;
    }
    double elev = spacing <= TERRAIN_LIFT_SPACING ? coarse : terrain_earth_height(d, TERRAIN_LIFT_SPACING);
    forms_of(&el, elev, m->a);
    m->share = mw * TERRAIN_DATA_SHARE;
    m->amp = 1.0 - mw * (1.0 - TERRAIN_RIDGE_KEEP) * smoothstepf((float)TERRAIN_KEEP_FROM_M, (float)TERRAIN_KEEP_FULL_M, (float)el.relief);
    m->rough = 1.0 + mw * (el.rough - 1.0);
    m->canyon = 1.0 - mw * (1.0 - smoothstepf((float)TERRAIN_SEA_FLOOR_M, 0.0f, (float)elev));
    if (!p) strike_frames(x, z, d, mw, &el, m);
}

static void region_at(double x, double z, const Macro *m, Region *r) {
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
    if (m->share > 0.0) {   /* the Earth's landforms take their share */
        for (int t = 0; t < TERRAIN_TYPES; ++t) a[t] = (float)((1.0 - m->share) * a[t] + m->share * m->a[t]);
    } else if (m->bias != 0.0) {   /* the stand-in's highlands raise the mountains and plateaus, basins the plains */
        a[TERRAIN_PLAINS] -= (float)m->bias;
        a[TERRAIN_MOUNTAINS] += (float)m->bias;
        a[TERRAIN_PLATEAU] += (float)(0.5 * m->bias);
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

/* A mountain belt's heights in ridge frame k: the region's warped frame
 * turned to that strike (frame 0 is the region's own u, v). */
static double belt_height(const Region *r, int k, double spacing) {
    double c = TERRAIN_STRIKE_X * STRIKE_TURN[k][0] - TERRAIN_STRIKE_Z * STRIKE_TURN[k][1];
    double s = TERRAIN_STRIKE_Z * STRIKE_TURN[k][0] + TERRAIN_STRIKE_X * STRIKE_TURN[k][1];
    double ru = c * r->wx + s * r->wz, rv = -s * r->wx + c * r->wz;
    int used = terrain_octaves(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
    double ww = TERRAIN_RIDGE_WARP_WAVE;
    double u = ru + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E46u, ru / ww, rv / ww) - 1.0);
    double v = rv + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value2(WORLD_SEED ^ 0x7E47u, ru / ww, rv / ww) - 1.0);
    return TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M * ridged(WORLD_SEED ^ 0x7E43u, u / TERRAIN_RIDGE_WAVE, v / TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, used);
}

/* Type t's heights at the point (m). The mountains blend the ridge frames
 * either side of the strike, scaled by what the data leaves them. */
static double type_height(int t, double x, double z, const Region *r, const Macro *m, double spacing) {
    switch (t) {
    case TERRAIN_PLAINS:
        return TERRAIN_PLAINS_M * fbm_lod(WORLD_SEED ^ 0x7E41u, x, z, TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
    case TERRAIN_HILLS:
        return TERRAIN_HILLS_M * fbm_lod(WORLD_SEED ^ 0x7E42u, x, z, TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
    case TERRAIN_MOUNTAINS: {
        double h = (1.0 - m->kt) * belt_height(r, m->k0, spacing);
        if (m->kt > 0.0) h += m->kt * belt_height(r, m->k1, spacing);
        return m->amp * h;
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
_Static_assert(CANYON_SEGS + 1 == TERRAIN_CANYON_POINTS, "a canyon's centre line has a point at each end of each segment");

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

/* Canyon c's centre line, as canyon_cut walks it. */
static void canyon_line(const Canyon *c, TerrainCanyon *out) {
    double d1 = 2.0 * PI_F * c->f1 / CANYON_SEGS, d2 = 2.0 * PI_F * c->f2 / CANYON_SEGS;
    double s1 = sin(c->p1), c1 = cos(c->p1), sd1 = sin(d1), cd1 = cos(d1);
    double s2 = sin(c->p2), c2 = cos(c->p2), sd2 = sin(d2), cd2 = cos(d2);
    for (int k = 0; k <= CANYON_SEGS; ++k) {
        double t = (double)k / CANYON_SEGS, sg = 2.0 * t - 1.0, off = c->m1 * s1 + c->m2 * s2, n;
        n = s1 * cd1 + c1 * sd1; c1 = c1 * cd1 - s1 * sd1; s1 = n;
        n = s2 * cd2 + c2 * sd2; c2 = c2 * cd2 - s2 * sd2; s2 = n;
        out->x[k] = c->cx + c->dx * c->half * sg - c->dz * off;
        out->z[k] = c->cz + c->dz * c->half * sg + c->dx * off;
        out->depth[k] = (float)(c->depth * smoothstepf(0.0f, 0.18f, (float)t) * smoothstepf(1.0f, 0.82f, (float)t));
    }
    out->rim = (float)(c->floor + c->depth / TERRAIN_CANYON_WALL);
}

int terrain_canyons_near(double x, double z, double r, TerrainCanyon *out, int max) {
    const double C = TERRAIN_CANYON_CELL, reach = r * 1.4142135623730951 + CANYON_REACH;
    int i0 = (int)floor((x - r - 0.6 * C) / C), i1 = (int)floor((x + r + 0.6 * C) / C);
    int j0 = (int)floor((z - r - 0.6 * C) / C), j1 = (int)floor((z + r + 0.6 * C) / C);
    int n = 0;
    Canyon c;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1 && n < max; ++i) {
            uint32_t h = hash2i(WORLD_SEED ^ 0x7E51u, i, j);
            if (hf(h, 0) >= (float)TERRAIN_CANYON_CHANCE) continue;
            double cx = (i + 0.3 + 0.4 * hf(h, 1)) * C, cz = (j + 0.3 + 0.4 * hf(h, 2)) * C;
            if (hypot(x - cx, z - cz) >= reach) continue;
            canyon_of(i, j, cx, cz, &c);   /* asked at its own centre, it is always found */
            canyon_line(&c, &out[n]);
            out[n++].id = h;
        }
    return n;
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

/* The land, and M * macro in it in *lift. */
static double land_parts(double x, double z, const TerrainDir *p, double edge, double home, double spacing, double *lift) {
    double h = TERRAIN_BASE_M * fbm_lod(WORLD_SEED ^ 0x7E12u, x, z, TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing);
    double e = region_rise(edge), c = canyon_rise(edge);
    Macro m;
    macro_of(x, z, p, edge, home, spacing, e > 0.0 || c > 0.0, &m);
    *lift = m.lift;
    h += *lift;
    if (e > 0.0) {
        Region r;
        region_at(x, z, &m, &r);
        double sum = 0.0;
        for (int t = 0; t < TERRAIN_TYPES; ++t)
            if (r.w[t] > 0.0f) sum += r.w[t] * type_height(t, x, z, &r, &m, spacing);
        h += e * m.rough * sum;
    }
    if (c > 0.0) h -= c * m.canyon * terrain_canyon_depth(x, z);
    return h;
}

static double land_lod(double x, double z, const TerrainDir *p, double edge, double home, double spacing) {
    double lift;
    return land_parts(x, z, p, edge, home, spacing, &lift);
}

double terrain_land_parts(double x, double z, double edge, double home, double spacing, double *lift) {
    return land_parts(x, z, NULL, edge, home, spacing, lift);
}
double terrain_land_lod(double x, double z, double edge, double home, double spacing) { return land_lod(x, z, NULL, edge, home, spacing); }
double terrain_land_at(double x, double z, TerrainDir p, double edge, double spacing) { return land_lod(x, z, &p, edge, edge, spacing); }

double terrain_land(double x, double z) {
    double home, edge = plan_city_edges(x, z, &home);
    return terrain_land_lod(x, z, edge, home, 0.0);
}

/* Where plan_urban is above 0 the edge measure is below 0, and the detail's
 * rise 0. */
double terrain_lift(double x, double z, double home, double spacing) {
    Macro m;
    macro_of(x, z, NULL, -CITY_BELT, home, spacing, 0, &m);
    return m.lift;
}

/* Over the rectangle the megacity's measure strays from its value at the
 * centre by at most CITY_EDGE_RATE times the half-diagonal, and no point of
 * it is further from the centre on the sphere than in the chart (whose
 * scale is never under 1), so the Earth there lies within its range over
 * that cap; M is at least 0, so the lift lies between the extreme products. */
void terrain_urban_lift_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0), h = plan_home_edge(cx, cz);
    double wlo = macro_weight(h - CITY_EDGE_RATE * half), whi = macro_weight(h + CITY_EDGE_RATE * half), elo, ehi;
    *lo = *hi = 0.0;
    if (whi <= 0.0) return;
    terrain_earth_range_at(terrain_chart_dir(cx, cz), half, TERRAIN_LIFT_SPACING, &elo, &ehi);
    elo -= terrain_earth_datum();
    ehi -= terrain_earth_datum();
    *lo = fmin(wlo * elo, whi * elo) - 1e-6;   /* rounding */
    *hi = fmax(wlo * ehi, whi * ehi) + 1e-6;
}

void terrain_landform(double x, double z, TerrainLandform *out) {
    Region r;
    Macro m;
    macro_of(x, z, NULL, 0.0, plan_home_edge(x, z), 0.0, 1, &m);
    region_at(x, z, &m, &r);
    for (int t = 0; t < TERRAIN_TYPES; ++t) out->w[t] = r.w[t];
    out->canyon = (float)terrain_canyon_depth(x, z);
}

/* Base, macro, the types and canyons each within their reach: the hills
 * reach lowest, the mountains highest (the data only scales them down), the
 * canyons cut at most their deepest; the roughness scales the types by at
 * most its largest; E and C only grow with the edge measure, M with the
 * megacity's. The lift blends the Earth at two spacings, each within its
 * extremes. */
void terrain_land_range(double edge, double home, double *lo, double *hi) {
    double e = region_rise(edge), m = macro_weight(home), elo, ehi, grade, e0 = terrain_earth_datum(), g = terrain_earth_rough_max();
    terrain_earth_extremes(&elo, &ehi, &grade);
    *lo = m * fmin(0.0, elo - e0) - (TERRAIN_BASE_M + e * g * TERRAIN_HILLS_M + canyon_rise(edge) * TERRAIN_CANYON_DEEP_M);
    *hi = m * fmax(0.0, ehi - e0) + TERRAIN_BASE_M + e * g * (TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M);
}

/* Leaving octaves out of a fractal sum moves it by at most their share of its
 * weight (each octave's noise is bounded), the same for the ridged sum (each
 * octave adds 0 .. its weight, and the carry only reaches later octaves), and
 * for a blend of two ridge frames, by at most the larger, times what the data
 * leaves the mountains. The plateau's mask moves the step through its slope.
 * The region's weights, the scales and the canyons don't depend on the
 * spacing. The lift's coarse Earth is its
 * full detail out to TERRAIN_LIFT_SPACING, and beyond within the Earth's
 * bounds at both spacings of the full detail; its fine Earth within the
 * bound at the spacing. */
double terrain_land_lod_error(double x, double z, double spacing) {
    double err = TERRAIN_BASE_M * terrain_dropped(TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing);
    double home, edge = plan_city_edges(x, z, &home), e = region_rise(edge), mw = macro_weight(home);
    if (mw > 0.0) {
        TerrainDir d = terrain_chart_dir(x, z);
        double r = detail_rise(edge), fine = terrain_earth_lod_error(d, spacing);
        double coarse = spacing > TERRAIN_LIFT_SPACING ? fine + terrain_earth_lod_error(d, TERRAIN_LIFT_SPACING) : 0.0;
        err += mw * ((1.0 - r) * coarse + r * fine);
    }
    if (e > 0.0) {
        Region r;
        Macro m;
        macro_of(x, z, NULL, edge, home, 0.0, 1, &m);
        region_at(x, z, &m, &r);
        double t[TERRAIN_TYPES];
        t[TERRAIN_PLAINS] = TERRAIN_PLAINS_M * terrain_dropped(TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
        t[TERRAIN_HILLS] = TERRAIN_HILLS_M * terrain_dropped(TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
        t[TERRAIN_MOUNTAINS] = m.amp * TERRAIN_RIDGE_M * terrain_dropped(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
        t[TERRAIN_PLATEAU] = TERRAIN_PLATEAU_M * 1.5 / TERRAIN_MASK_STEP * terrain_dropped(TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES, spacing) +
                             TERRAIN_TOP_M * terrain_dropped(TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES, spacing);
        for (int k = 0; k < TERRAIN_TYPES; ++k) err += e * m.rough * r.w[k] * t[k];
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

uint64_t terrain_lod_key(double spacing) {
    static const struct { double wave; int octaves; } sums[] = {
        { TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES }, { TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES },
        { TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES }, { TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES },
        { TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES }, { TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES },
        { TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES }, { TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES },
    };
    uint64_t key = 0;
    for (size_t k = 0; k < sizeof(sums) / sizeof(sums[0]); ++k) key = key * 16u + (uint64_t)terrain_octaves(sums[k].wave, sums[k].octaves, spacing);
    return key << 32 | terrain_earth_lod_key(spacing);
}
