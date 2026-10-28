#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The land beyond the city (see terrain_internal.h): rolling ground, the
 * region's landforms and canyons, as pure functions of the direction on the
 * planet. Only the edge measures, which read the settlements, are taken in
 * the world's chart. */

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }

#define PI_D 3.14159265358979323846

static double comp(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

static TerrainDir axis_dir(int k) {
    TerrainDir e = { k == 0, k == 1, k == 2 };
    return e;
}

/* p along cube face f's outward normal: the face is where this is largest. */
static double face_comp(TerrainDir p, int f) {
    double c = comp(p, f / 2);
    return (f & 1) ? -c : c;
}

/* p's equal-angle coordinate on face f (terrain_dir_face) along its u (k 1)
 * or v (k 2), given c = face_comp(p, f) > 0; past the face's edges too. */
static double face_coord(TerrainDir p, int f, int k, double c) {
    return atan(comp(p, (f / 2 + k) % 3) / c) * (4.0 / PI_D);
}

/* 0 at t <= -1 to 1 at t >= 1, smoothly. */
static double blend11(double t) {
    t = 0.5 + 0.5 * t;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

/* --- The frames --------------------------------------------------------------
 * The landforms are noise in three dimensions at the point R p (m), so they
 * need no chart, and the isotropic fields read it as it is. The mountains run
 * along a strike, which only a fixed frame can carry: a frame turning with
 * the point would shear the noise by R times its turn. So each cube face is
 * cut into FRAME_TILES x FRAME_TILES tiles, each with the tangent frame at
 * its centre (n up, u along the face's u, v = n x u), and the belts and
 * ridges are drawn in the frames of the tiles round a point, blended over
 * FRAME_BLEND_M either side of the tiles' edges and of the faces' seams. A
 * tile's plane strays at most 16 degrees from the sphere under it, which
 * shortens a belt's stretch a little there but never turns it. */
#define FRAME_TILES   4
#define FRAME_BLEND_M 8000.0
#define FRAME_MAX     12   /* three faces, two by two tiles each: more than ever meet */

typedef struct Frame {
    TerrainDir n, u, v;       /* the tile's frame */
    double wt;                /* its weight at the point; the frames' weights sum to 1 */
    double wu, wv, wn;        /* the region's warped point in it (m) */
    int    k0, k1;            /* the ridges' frames either side of the strike... */
    double kt;                /* ...and the blend between them */
    float  w[TERRAIN_TYPES];  /* the region's weights in it */
} Frame;

static void tile_frame(int f, int i, int j, Frame *fr) {
    double w = 2.0 / FRAME_TILES;
    fr->n = terrain_face_dir(f, -1.0 + (i + 0.5) * w, -1.0 + (j + 0.5) * w);
    TerrainDir e = axis_dir((f / 2 + 1) % 3);
    fr->u = unit3(sub3(e, scale3(fr->n, dot3(e, fr->n))));
    fr->v = cross3(fr->n, fr->u);
}

/* The tiles along one of a face's axes at coordinate u, with their weights:
 * one, or two within hw tiles of the edge between them. Past the face's own
 * edges the last tile stands (the faces' seams blend the faces). */
static int tile_axis(double u, double hw, int idx[2], double w[2]) {
    double t = (u + 1.0) * 0.5 * FRAME_TILES;
    int i = (int)floor(t), k = (int)floor(t + 0.5);
    idx[0] = i < 0 ? 0 : i >= FRAME_TILES ? FRAME_TILES - 1 : i;
    w[0] = 1.0;
    if (k < 1 || k >= FRAME_TILES || fabs(t - k) >= hw) return 1;
    double s = blend11((t - k) / hw);
    idx[0] = k - 1; w[0] = 1.0 - s;
    idx[1] = k;     w[1] = s;
    return 2;
}

/* The frames round p, their weights summing to 1. A face's weight is the
 * product of its lead over each other face's component, blended over
 * FRAME_BLEND_M (across a seam the two components part at sqrt 2 / R a
 * metre); the face p is on leads every other, so its weight is never 0. */
static int frames_at(TerrainDir p, Frame out[FRAME_MAX]) {
    const double fb = 1.4142135623730951 * FRAME_BLEND_M / PLANET_RADIUS;
    const double tb = FRAME_BLEND_M / PLANET_RADIUS * (4.0 / PI_D) * 0.5 * FRAME_TILES;
    double c[6], sum = 0.0;
    for (int f = 0; f < 6; ++f) c[f] = face_comp(p, f);
    int n = 0;
    for (int f = 0; f < 6; ++f) {
        if (c[f] <= 0.0) continue;
        double wf = 1.0;
        for (int g = 0; g < 6 && wf > 0.0; ++g)
            if (g != f) wf *= blend11((c[f] - c[g]) / fb);
        if (wf <= 0.0) continue;
        int ti[2], tj[2];
        double wi[2], wj[2];
        int ni = tile_axis(face_coord(p, f, 1, c[f]), tb, ti, wi), nj = tile_axis(face_coord(p, f, 2, c[f]), tb, tj, wj);
        for (int b = 0; b < nj; ++b)
            for (int a = 0; a < ni; ++a) {
                Frame *fr = &out[n++];
                tile_frame(f, ti[a], tj[b], fr);
                fr->wt = wf * wi[a] * wj[b];
                sum += fr->wt;
            }
    }
    for (int k = 0; k < n; ++k) out[k].wt /= sum;
    return n;
}

/* The default strike at p, in the frame leading there, along the sphere. */
TerrainDir terrain_frame_strike(TerrainDir p) {
    Frame fr[FRAME_MAX];
    int n = frames_at(p, fr), best = 0;
    for (int k = 1; k < n; ++k)
        if (fr[k].wt > fr[best].wt) best = k;
    TerrainDir s = add3(scale3(fr[best].u, TERRAIN_STRIKE_X), scale3(fr[best].v, TERRAIN_STRIKE_Z));
    return unit3(sub3(s, scale3(p, dot3(s, p))));
}

/* --- The region ----------------------------------------------------------------
 * A warped place for the point, so neither the regions nor the belts run
 * straight; in each frame the strike's: u along the mountain belts, v across
 * them. */
typedef struct Region {
    TerrainDir P, W;         /* the point, R p, and its warped place (m) */
    float hills, plateau;    /* the isotropic affinities */
} Region;

/* What the planet gives the land at a point (macro_of): the lift, macro;
 * and for the landforms, the stand-in's affinity bias (without a bake), or
 * the Earth's own affinities and their share of the region's, the data's
 * strike along the sphere and its weight (its coherence), how much of the
 * mountains' height the data leaves them, the roughness, and how much of the
 * canyons' cut the sea floor leaves. */
typedef struct Macro {
    double lift, bias, share;
    float  a[TERRAIN_TYPES];
    TerrainDir strike;
    double sw, amp, rough, canyon;
} Macro;

/* The ridges' frames: the default strike (TERRAIN_STRIKE_*) turned towards
 * +v by k pi / TERRAIN_STRIKES, as (cos, sin) of the turn. */
static const double STRIKE_TURN[TERRAIN_STRIKES][2] = {
    { 1.0, 0.0 }, { 0.9238795325112867, 0.3826834323650898 }, { 0.7071067811865476, 0.7071067811865476 },
    { 0.3826834323650898, 0.9238795325112867 }, { 0.0, 1.0 }, { -0.3826834323650898, 0.9238795325112867 },
    { -0.7071067811865476, 0.7071067811865476 }, { -0.9238795325112867, 0.3826834323650898 },
};
_Static_assert(TERRAIN_STRIKES == 8, "STRIKE_TURN holds 8 frames");

/* --- The planet's highlands and basins ------------------------------------------
 * The Earth less the datum (terrain_internal.h), and how much of the Earth's
 * own detail at the nearest edge measure. The procedural stand-in is a
 * fractal sum over the sphere, in [-1, 1]. */
static double detail_rise(double edge) {
    return smoothstepf((float)TERRAIN_DETAIL_FROM, (float)TERRAIN_DETAIL_FULL, (float)edge);
}

static double macro_at(TerrainDir p, double spacing) {
    double k = PLANET_RADIUS / TERRAIN_MACRO_WAVE;
    return noise_fbm3_band(WORLD_SEED ^ 0x7E61u, p.x * k, p.y * k, p.z * k, TERRAIN_MACRO_OCTAVES,
                           terrain_octaves(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing));
}

double terrain_macro(TerrainDir p, double spacing) { return macro_at(p, spacing); }

/* The Earth lies within its extremes. */
double terrain_lift_bound(void) {
    double lo, hi, grade, e0 = terrain_earth_datum();
    terrain_earth_extremes(&lo, &hi, &grade);
    return fmax(hi - e0, e0 - lo);
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

/* The data's strike at d, from its east and north, along the sphere, and
 * its weight, its coherence (none at a pole). */
static void data_strike(TerrainDir d, const TerrainEarthLand *el, Macro *m) {
    double w = hypot(el->strike_e, el->strike_n), l = hypot(d.x, d.y);
    if (w <= 0.0 || l < 1e-9) return;
    TerrainDir east = { -d.y / l, d.x / l, 0.0 }, north = cross3(d, east);
    double half = 0.5 * atan2(el->strike_n, el->strike_e);
    m->strike = add3(scale3(east, cos(half)), scale3(north, sin(half)));
    m->sw = w;
}

/* The ridges' two frames either side of the strike in frame f, and the
 * blend between them: the data's strike carried along p into the frame's
 * plane, blended with the default by its weight as doubled angles (a strike
 * has no sense). Without data, frame 0, the default. */
static void frame_strike(Frame *f, TerrainDir p, const Macro *m) {
    f->k0 = f->k1 = 0;
    f->kt = 0.0;
    double w = m->sw;
    if (w <= 0.0) return;
    TerrainDir q = sub3(m->strike, scale3(p, dot3(m->strike, f->n) / dot3(p, f->n)));
    double a = 2.0 * atan2(dot3(q, f->v), dot3(q, f->u)), a0 = 2.0 * atan2(TERRAIN_STRIKE_Z, TERRAIN_STRIKE_X);
    double cx = w * cos(a) + (1.0 - w) * cos(a0), cz = w * sin(a) + (1.0 - w) * sin(a0);
    if (cx == 0.0 && cz == 0.0) return;
    double turn = fmod(0.5 * (atan2(cz, cx) - a0), PI_D);   /* from the default strike towards +v */
    if (turn < 0.0) turn += PI_D;
    double q8 = turn * (TERRAIN_STRIKES / PI_D);
    int k = (int)floor(q8);
    if (k >= TERRAIN_STRIKES) return;
    f->k0 = k;
    f->k1 = (k + 1) % TERRAIN_STRIKES;
    f->kt = smoothstepf(0.0f, 1.0f, (float)(q8 - k));
}

/* macro at direction d: the Earth at TERRAIN_LIFT_SPACING (or coarser, at a
 * coarser spacing), and by the detail's rise at the edge measure `edge` the
 * Earth at the spacing itself. With `forms`, the landforms' part too (see
 * Macro): it reads the Earth at the lift's spacing and the landform grid,
 * whatever the spacing, so the region's weights and scales don't depend on
 * the level of detail. */
static void macro_of(TerrainDir d, double edge, double spacing, int forms, Macro *m) {
    m->lift = m->bias = m->share = m->sw = 0.0;
    m->amp = m->rough = m->canyon = 1.0;
    double coarse = terrain_earth_height(d, fmax(spacing, TERRAIN_LIFT_SPACING)), r = detail_rise(edge);
    double l = coarse - terrain_earth_datum();
    if (r > 0.0) l += r * (terrain_earth_height(d, spacing) - coarse);
    m->lift = l;
    if (!forms) return;
    TerrainEarthLand el;
    if (!terrain_earth_land(d, &el)) {
        m->bias = TERRAIN_MACRO_BIAS * terrain_macro(d, 0.0);
        return;
    }
    double elev = spacing <= TERRAIN_LIFT_SPACING ? coarse : terrain_earth_height(d, TERRAIN_LIFT_SPACING);
    forms_of(&el, elev, m->a);
    m->share = TERRAIN_DATA_SHARE;
    m->amp = 1.0 - (1.0 - TERRAIN_RIDGE_KEEP) * smoothstepf((float)TERRAIN_KEEP_FROM_M, (float)TERRAIN_KEEP_FULL_M, (float)el.relief);
    m->rough = el.rough;
    m->canyon = smoothstepf((float)TERRAIN_SEA_FLOOR_M, 0.0f, (float)elev);
    data_strike(d, &el, m);
}

static void region_at(TerrainDir p, Region *r) {
    const double ww = TERRAIN_WARP_WAVE, rw = TERRAIN_REGION_WAVE;
    r->P = scale3(p, PLANET_RADIUS);
    double x = r->P.x / ww, y = r->P.y / ww, z = r->P.z / ww;
    r->W.x = r->P.x + TERRAIN_WARP_M * (2.0 * noise_value3(WORLD_SEED ^ 0x7E31u, x, y, z) - 1.0);
    r->W.y = r->P.y + TERRAIN_WARP_M * (2.0 * noise_value3(WORLD_SEED ^ 0x7E32u, x, y, z) - 1.0);
    r->W.z = r->P.z + TERRAIN_WARP_M * (2.0 * noise_value3(WORLD_SEED ^ 0x7E36u, x, y, z) - 1.0);
    r->hills = noise_value3(WORLD_SEED ^ 0x7E33u, r->W.x / rw, r->W.y / rw, r->W.z / rw);
    r->plateau = noise_value3(WORLD_SEED ^ 0x7E35u, r->W.x / rw, r->W.y / rw, r->W.z / rw);
}

/* The region's weights in frame f: the mountains' affinity stretched along
 * the default strike there, the rest the same in every frame. */
static void region_in(const Region *r, const Macro *m, TerrainDir p, Frame *f) {
    const double rw = TERRAIN_REGION_WAVE;
    f->wu = dot3(r->W, f->u);
    f->wv = dot3(r->W, f->v);
    f->wn = dot3(r->W, f->n);
    frame_strike(f, p, m);
    double su = TERRAIN_STRIKE_X * f->wu + TERRAIN_STRIKE_Z * f->wv, sv = -TERRAIN_STRIKE_Z * f->wu + TERRAIN_STRIKE_X * f->wv;
    float a[TERRAIN_TYPES];
    a[TERRAIN_PLAINS] = (float)TERRAIN_PLAINS_BIAS;
    a[TERRAIN_HILLS] = r->hills;
    a[TERRAIN_MOUNTAINS] = noise_value3(WORLD_SEED ^ 0x7E34u, su / (rw * TERRAIN_BELT_ALONG), f->wn / rw, sv / (rw * TERRAIN_BELT_ACROSS));
    a[TERRAIN_PLATEAU] = r->plateau;
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
    for (int t = 0; t < TERRAIN_TYPES; ++t) f->w[t] = k[t] / sum;
}

/* The frames round p and the region's weights in each. */
static int region_frames(TerrainDir p, const Macro *m, Region *r, Frame fr[FRAME_MAX]) {
    region_at(p, r);
    int n = frames_at(p, fr);
    for (int k = 0; k < n; ++k) region_in(r, m, p, &fr[k]);
    return n;
}

/* Ridged fractal sum in [0, 1] over the strike frame (u along the belt, v
 * across, n up the tile, in units of TERRAIN_RIDGE_WAVE): each octave's value
 * noise folded about its middle and squared, so its crests are sharp, and
 * weighted by the octave before, so the fine ridges stand on the coarse ones
 * and the valleys stay smoother. The first two octaves, the main ridges, are
 * stretched along the strike; each finer one is turned a little further about
 * n, so the spurs off them run every way. Octave weights as
 * noise_fbm3_band; only the first `used` count. */
static const double RIDGE_TURN[8][2] = {   /* (cos, sin) of 0.9 k radians */
    { 1.0, 0.0 }, { 0.0, 0.0 }, { -0.2272020946930871, 0.9738476308781951 }, { -0.9040721420170612, 0.4273798802338298 },
    { -0.896758416334147, -0.44252044329485246 }, { -0.2107957994307797, -0.977530117665097 },
    { 0.6346928759426347, -0.7727644875559871 }, { 0.9998586363834151, 0.016813900484349713 },
};
_Static_assert(TERRAIN_RIDGE_OCTAVES <= 8, "RIDGE_TURN holds 8 octaves' turns");

static double ridged(uint32_t seed, double u, double n, double v, int octaves, int used) {
    double sum = 0.0, wsum = 0.0, a = 1.0, f = 1.0, carry = 1.0;
    for (int k = 0; k < octaves; ++k) {
        if (k < used) {
            double x = u / TERRAIN_RIDGE_ALONG, z = v;
            if (k >= 2) {
                double c = RIDGE_TURN[k][0], s = RIDGE_TURN[k][1];
                x = c * u - s * v; z = s * u + c * v;
            }
            double q = 1.0 - fabs(2.0 * noise_value3(hash_combine(seed, (uint32_t)k), x * f + 17.31 * k, n * f + 29.53 * k, z * f - 41.77 * k) - 1.0);
            q *= q * carry;
            carry = fmin(1.0, 2.0 * q);
            sum += a * q;
        }
        wsum += a;
        a *= 0.5;
        f *= 2.0;
    }
    return sum / wsum;
}

static double fbm_lod(uint32_t seed, TerrainDir P, double wave, int octaves, double spacing) {
    return noise_fbm3_band(seed, P.x / wave, P.y / wave, P.z / wave, octaves, terrain_octaves(wave, octaves, spacing));
}

/* The plateau's step: 0 below the mask's level, 1 a step above it; its
 * steepest, 1.5 / TERRAIN_MASK_STEP per unit of mask, in the middle. */
static double plateau_step(double mask) {
    return smoothstepf(0.0f, 1.0f, (float)((mask - TERRAIN_MASK_LEVEL) / TERRAIN_MASK_STEP));
}

/* A mountain belt's heights in ridge frame k of tile frame f: the region's
 * warped point turned to that strike (k 0 is the default strike). */
static double belt_height(const Frame *f, int k, double spacing) {
    double c = TERRAIN_STRIKE_X * STRIKE_TURN[k][0] - TERRAIN_STRIKE_Z * STRIKE_TURN[k][1];
    double s = TERRAIN_STRIKE_Z * STRIKE_TURN[k][0] + TERRAIN_STRIKE_X * STRIKE_TURN[k][1];
    double ru = c * f->wu + s * f->wv, rv = -s * f->wu + c * f->wv, rn = f->wn;
    int used = terrain_octaves(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
    double ww = TERRAIN_RIDGE_WARP_WAVE;
    double u = ru + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value3(WORLD_SEED ^ 0x7E46u, ru / ww, rn / ww, rv / ww) - 1.0);
    double v = rv + TERRAIN_RIDGE_WARP_M * (2.0 * noise_value3(WORLD_SEED ^ 0x7E47u, ru / ww, rn / ww, rv / ww) - 1.0);
    return TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M * ridged(WORLD_SEED ^ 0x7E43u, u / TERRAIN_RIDGE_WAVE, rn / TERRAIN_RIDGE_WAVE, v / TERRAIN_RIDGE_WAVE,
                                                       TERRAIN_RIDGE_OCTAVES, used);
}

/* The mountains in frame f: the ridge frames either side of the strike
 * blended, scaled by what the data leaves them. */
static double mountain_height(const Frame *f, const Macro *m, double spacing) {
    double h = (1.0 - f->kt) * belt_height(f, f->k0, spacing);
    if (f->kt > 0.0) h += f->kt * belt_height(f, f->k1, spacing);
    return m->amp * h;
}

/* Type t's heights at the point (m), for every type but the mountains: the
 * same in every frame. */
static double type_height(int t, TerrainDir P, double spacing) {
    switch (t) {
    case TERRAIN_PLAINS:
        return TERRAIN_PLAINS_M * fbm_lod(WORLD_SEED ^ 0x7E41u, P, TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
    case TERRAIN_HILLS:
        return TERRAIN_HILLS_M * fbm_lod(WORLD_SEED ^ 0x7E42u, P, TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
    default: {
        double mask = fbm_lod(WORLD_SEED ^ 0x7E44u, P, TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES, spacing);
        return TERRAIN_PLATEAU_M * plateau_step(mask) + TERRAIN_TOP_M * fbm_lod(WORLD_SEED ^ 0x7E45u, P, TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES, spacing);
    }
    }
}

/* The landforms' height: in each frame the types blended by the region's
 * weights there, the frames by theirs. Only a type with weight is
 * evaluated, and the isotropic ones once. */
static double forms_height(const Region *r, const Frame *fr, int n, const Macro *m, double spacing) {
    double iso[TERRAIN_TYPES], sum = 0.0;
    int have = 0;
    for (int k = 0; k < n; ++k) {
        double s = 0.0;
        for (int t = 0; t < TERRAIN_TYPES; ++t) {
            if (fr[k].w[t] <= 0.0f) continue;
            double h;
            if (t == TERRAIN_MOUNTAINS) h = mountain_height(&fr[k], m, spacing);
            else {
                if (!(have >> t & 1)) { iso[t] = type_height(t, r->P, spacing); have |= 1 << t; }
                h = iso[t];
            }
            s += fr[k].w[t] * h;
        }
        sum += fr[k].wt * s;
    }
    return sum;
}

static double region_rise(double edge) {
    return smoothstepf((float)TERRAIN_REGION_FROM, (float)(TERRAIN_REGION_FROM + TERRAIN_LAND_RISE), (float)edge);
}

static double canyon_rise(double edge) {
    return smoothstepf((float)TERRAIN_CANYON_FROM, (float)TERRAIN_CANYON_FULL, (float)edge);
}

/* --- Canyons ------------------------------------------------------------------
 * Each cube face is cut into CANYON_SIDE x CANYON_SIDE cells, equal in angle,
 * TERRAIN_CANYON_CELL across at the face's middle, and cell (f, i, j) holds
 * one canyon or none. It is drawn in its own chart, about a point in the
 * middle 0.4 of the cell with x along the face's u (over a canyon's reach the
 * chart is the sphere to a micrometre): a meandering centre line of
 * CANYON_SEGS segments about a straight chord through the chart's origin, two
 * sine meanders across it. Its depth tapers to nothing at both ends. Across
 * the line: a level floor, then walls climbing at TERRAIN_CANYON_WALL on
 * average to the rim. A lookup checks every cell, on every face, whose
 * canyon's centre can lie within CANYON_REACH. */
#define CANYON_SEGS 16
#define CANYON_SIDE ((int)(0.5 * PI_D * PLANET_RADIUS / TERRAIN_CANYON_CELL + 0.5))
_Static_assert(CANYON_SEGS + 1 == TERRAIN_CANYON_POINTS, "a canyon's centre line has a point at each end of each segment");

typedef struct Canyon {
    TerrainChart chart;            /* about its centre */
    double dx, dz, half;           /* the chord: unit direction, half its length */
    double depth, floor;           /* deepest (m), the floor's half-width (m) */
    double m1, f1, p1, m2, f2, p2; /* meanders: amplitude (m), cycles along the chord, phase */
} Canyon;

/* The farthest any canyon reaches from its centre: chord, meanders, floor and walls. */
#define CANYON_REACH (10000.0 + 1200.0 + 300.0 + 90.0 + TERRAIN_CANYON_DEEP_M / TERRAIN_CANYON_WALL)

/* The cells of face f whose canyons' centres can lie within `reach` (m) of
 * p. A centre on the face has its component at least 1 / sqrt 3, so p is
 * within reach only if its own is that less the reach's angle m; and along
 * the way the face's coordinates change by at most 4 / (pi R c) a metre,
 * with c the least component on the way. */
static int canyon_cells(TerrainDir p, int f, double reach, int *i0, int *i1, int *j0, int *j1) {
    double c = face_comp(p, f), m = reach / PLANET_RADIUS;
    if (c < 0.5773502691896258 - m) return 0;
    double du = (4.0 / PI_D) * m / (c - m) * 1.001, u = face_coord(p, f, 1, c), v = face_coord(p, f, 2, c);
    const int N = CANYON_SIDE;
    *i0 = (int)floor((u - du + 1.0) * 0.5 * N); *i1 = (int)floor((u + du + 1.0) * 0.5 * N);
    *j0 = (int)floor((v - du + 1.0) * 0.5 * N); *j1 = (int)floor((v + du + 1.0) * 0.5 * N);
    *i0 = *i0 < 0 ? 0 : *i0; *i1 = *i1 >= N ? N - 1 : *i1;
    *j0 = *j0 < 0 ? 0 : *j0; *j1 = *j1 >= N ? N - 1 : *j1;
    return *i0 <= *i1 && *j0 <= *j1;
}

static uint32_t canyon_hash(int f, int i, int j) { return hash3i(WORLD_SEED ^ 0x7E51u, f, i, j); }

/* Cell (f, i, j)'s canyon's centre, given its hash. */
static TerrainDir canyon_centre(int f, int i, int j, uint32_t h) {
    double w = 2.0 / CANYON_SIDE;
    return terrain_face_dir(f, -1.0 + (i + 0.3 + 0.4 * hf(h, 1)) * w, -1.0 + (j + 0.3 + 0.4 * hf(h, 2)) * w);
}

/* Cell (f, i, j)'s canyon, if it has one that can reach p, and p in its
 * chart at (*x, *z). */
static int canyon_of(int f, int i, int j, TerrainDir p, Canyon *c, double *x, double *z) {
    uint32_t h = canyon_hash(f, i, j);
    if (hf(h, 0) >= (float)TERRAIN_CANYON_CHANCE) return 0;
    TerrainDir o = canyon_centre(f, i, j, h);
    const double m = CANYON_REACH / PLANET_RADIUS;
    if (dot3(p, o) < 1.0 - 0.5 * m * m) return 0;   /* beyond the reach: cos m is more */
    TerrainDir e = axis_dir((f / 2 + 1) % 3);
    c->chart.o = o;
    c->chart.ex = unit3(sub3(e, scale3(o, dot3(e, o))));
    c->chart.ez = cross3(c->chart.ex, o);
    terrain_dir_chart(&c->chart, p, x, z);
    if (*x * *x + *z * *z >= CANYON_REACH * CANYON_REACH) return 0;
    double a = 2.0 * PI_F * hf(h, 3);
    c->dx = cos(a); c->dz = sin(a);
    c->half = 6000.0 + 4000.0 * hf(h, 4);
    c->depth = 180.0 + (TERRAIN_CANYON_DEEP_M - 180.0) * hf(h, 5);
    c->floor = 40.0 + 50.0 * hf(h, 6);
    c->m1 = 600.0 + 600.0 * hf(h, 7); c->f1 = 1.5 + hf(h, 8); c->p1 = 2.0 * PI_F * hf(h, 9);
    c->m2 = 150.0 + 150.0 * hf(h, 10); c->f2 = 4.0 + 2.0 * hf(h, 11); c->p2 = 2.0 * PI_F * hf(h, 12);
    return 1;
}

/* How deep canyon c cuts at (x, z) of its chart: the deepest of its
 * segments' cuts, each from the nearest point on the segment, so the cut is
 * continuous. */
static double canyon_cut(const Canyon *c, double x, double z) {
    double reach = c->half + c->m1 + c->m2 + c->floor + c->depth / TERRAIN_CANYON_WALL;
    if (x * x + z * z >= reach * reach) return 0.0;
    /* the meanders' sines along the vertices, by turning each a step at a time */
    double d1 = 2.0 * PI_F * c->f1 / CANYON_SEGS, d2 = 2.0 * PI_F * c->f2 / CANYON_SEGS;
    double s1 = sin(c->p1), c1 = cos(c->p1), sd1 = sin(d1), cd1 = cos(d1);
    double s2 = sin(c->p2), c2 = cos(c->p2), sd2 = sin(d2), cd2 = cos(d2);
    double px = 0.0, pz = 0.0, cut = 0.0;
    for (int k = 0; k <= CANYON_SEGS; ++k) {
        double t = (double)k / CANYON_SEGS, s = 2.0 * t - 1.0, off = c->m1 * s1 + c->m2 * s2, n;
        n = s1 * cd1 + c1 * sd1; c1 = c1 * cd1 - s1 * sd1; s1 = n;
        n = s2 * cd2 + c2 * sd2; c2 = c2 * cd2 - s2 * sd2; s2 = n;
        double qx = c->dx * c->half * s - c->dz * off, qz = c->dz * c->half * s + c->dx * off;
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

/* Canyon c's centre line, as canyon_cut walks it, in the chart wc. */
static void canyon_line(const Canyon *c, const TerrainChart *wc, TerrainCanyon *out) {
    double d1 = 2.0 * PI_F * c->f1 / CANYON_SEGS, d2 = 2.0 * PI_F * c->f2 / CANYON_SEGS;
    double s1 = sin(c->p1), c1 = cos(c->p1), sd1 = sin(d1), cd1 = cos(d1);
    double s2 = sin(c->p2), c2 = cos(c->p2), sd2 = sin(d2), cd2 = cos(d2);
    for (int k = 0; k <= CANYON_SEGS; ++k) {
        double t = (double)k / CANYON_SEGS, sg = 2.0 * t - 1.0, off = c->m1 * s1 + c->m2 * s2, n;
        n = s1 * cd1 + c1 * sd1; c1 = c1 * cd1 - s1 * sd1; s1 = n;
        n = s2 * cd2 + c2 * sd2; c2 = c2 * cd2 - s2 * sd2; s2 = n;
        TerrainDir q = terrain_chart_dir(&c->chart, c->dx * c->half * sg - c->dz * off, c->dz * c->half * sg + c->dx * off);
        terrain_dir_chart(wc, q, &out->x[k], &out->z[k]);
        out->depth[k] = (float)(c->depth * smoothstepf(0.0f, 0.18f, (float)t) * smoothstepf(1.0f, 0.82f, (float)t));
    }
    out->rim = (float)(c->floor + c->depth / TERRAIN_CANYON_WALL);
}

int terrain_canyons_near(double x, double z, double r, TerrainCanyon *out, int max) {
    TerrainChart wc = terrain_world_chart();
    TerrainDir p = terrain_chart_dir(&wc, x, z);
    double reach = r * 1.4142135623730951 + CANYON_REACH, cx, cz;
    int n = 0, i0, i1, j0, j1;
    Canyon c;
    for (int f = 0; f < 6; ++f) {
        if (!canyon_cells(p, f, reach, &i0, &i1, &j0, &j1)) continue;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1 && n < max; ++i) {
                uint32_t h = canyon_hash(f, i, j);
                if (hf(h, 0) >= (float)TERRAIN_CANYON_CHANCE) continue;
                TerrainDir o = canyon_centre(f, i, j, h);
                double d = dot3(p, o);
                if (acos(d > 1.0 ? 1.0 : d) * PLANET_RADIUS >= reach) continue;
                canyon_of(f, i, j, o, &c, &cx, &cz);   /* asked at its own centre, it is always found */
                canyon_line(&c, &wc, &out[n]);
                out[n++].id = h;
            }
    }
    return n;
}

static double canyon_depth(TerrainDir p) {
    double cut = 0.0, x, z;
    int i0, i1, j0, j1;
    Canyon c;
    for (int f = 0; f < 6; ++f) {
        if (!canyon_cells(p, f, CANYON_REACH, &i0, &i1, &j0, &j1)) continue;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (canyon_of(f, i, j, p, &c, &x, &z)) cut = fmax(cut, canyon_cut(&c, x, z));
    }
    return cut;
}

double terrain_canyon_depth(double x, double z) {
    TerrainChart wc = terrain_world_chart();
    return canyon_depth(terrain_chart_dir(&wc, x, z));
}

/* --- The land -------------------------------------------------------------- */

/* The procedural land's share at a signed distance s (m) from the real
 * water's edge (terrain_earth_valley): none in the water, all of it from
 * TERRAIN_VALLEY_M out, so the real rivers and lakes lie on the Earth's own
 * valley floors and shores. */
static double valley_rise(double s) { return smoothstepf(0.0f, (float)TERRAIN_VALLEY_M, (float)s); }

double terrain_land_parts_at(TerrainDir d, double edge, double spacing, double valley, double *lift) {
    double v = valley_rise(valley);
    double h = v > 0.0 ? v * TERRAIN_BASE_M * fbm_lod(WORLD_SEED ^ 0x7E12u, scale3(d, PLANET_RADIUS), TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing) : 0.0;
    double e = v * region_rise(edge), c = v * canyon_rise(edge);
    Macro m;
    macro_of(d, edge, spacing, e > 0.0 || c > 0.0, &m);
    *lift = m.lift;
    h += *lift;
    if (e > 0.0) {
        Region r;
        Frame fr[FRAME_MAX];
        int n = region_frames(d, &m, &r, fr);
        h += e * m.rough * forms_height(&r, fr, n, &m, spacing);
    }
    if (c > 0.0) h -= c * m.canyon * canyon_depth(d);
    return h;
}

double terrain_land_parts(TerrainDir d, double edge, double spacing, double *lift) {
    return terrain_land_parts_at(d, edge, spacing, terrain_earth_valley(d), lift);
}

/* Where plan_urban is above 0 the edge measure is below 0, and the detail's
 * rise 0. */
double terrain_lift(double x, double z, double spacing) {
    TerrainChart wc = terrain_world_chart();
    Macro m;
    macro_of(terrain_chart_dir(&wc, x, z), -CITY_BELT, spacing, 0, &m);
    return m.lift;
}

/* No point of the rectangle is further from its centre on the sphere than
 * in the chart (whose scale is never under 1), so the Earth there lies
 * within its range over that cap. */
void terrain_urban_lift_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), half = 0.5 * hypot(x1 - x0, z1 - z0), elo, ehi;
    TerrainChart wc = terrain_world_chart();
    terrain_earth_range_at(terrain_chart_dir(&wc, cx, cz), half, TERRAIN_LIFT_SPACING, &elo, &ehi);
    *lo = elo - terrain_earth_datum() - 1e-6;   /* rounding */
    *hi = ehi - terrain_earth_datum() + 1e-6;
}

void terrain_landform(double x, double z, TerrainLandform *out) {
    TerrainChart wc = terrain_world_chart();
    TerrainDir d = terrain_chart_dir(&wc, x, z);
    Macro m;
    Region r;
    Frame fr[FRAME_MAX];
    macro_of(d, 0.0, 0.0, 1, &m);
    int n = region_frames(d, &m, &r, fr);
    for (int t = 0; t < TERRAIN_TYPES; ++t) {
        double w = 0.0;
        for (int k = 0; k < n; ++k) w += fr[k].wt * fr[k].w[t];
        out->w[t] = (float)w;
    }
    out->canyon = (float)canyon_depth(d);
}

/* Base, macro, the types and canyons each within their reach: the hills
 * reach lowest, the mountains highest (the data only scales them down), the
 * canyons cut at most their deepest; the roughness scales the types by at
 * most its largest; E and C only grow with the edge measure. The lift
 * blends the Earth at two spacings, each within its extremes; the frames
 * blend the landforms, each within the same reach. */
void terrain_land_range(double edge, double *lo, double *hi) {
    double e = region_rise(edge), elo, ehi, grade, e0 = terrain_earth_datum(), g = terrain_earth_rough_max();
    terrain_earth_extremes(&elo, &ehi, &grade);
    *lo = fmin(0.0, elo - e0) - (TERRAIN_BASE_M + e * g * TERRAIN_HILLS_M + canyon_rise(edge) * TERRAIN_CANYON_DEEP_M);
    *hi = fmax(0.0, ehi - e0) + TERRAIN_BASE_M + e * g * (TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M);
}

/* Leaving octaves out of a fractal sum moves it by at most their share of its
 * weight (each octave's noise is bounded), the same for the ridged sum (each
 * octave adds 0 .. its weight, and the carry only reaches later octaves), and
 * for a blend of two ridge frames, by at most the larger, times what the data
 * leaves the mountains. The plateau's mask moves the step through its slope.
 * The region's weights, the frames', the scales and the canyons don't depend
 * on the spacing. The lift's coarse Earth is its full detail out to
 * TERRAIN_LIFT_SPACING, and beyond within the Earth's bounds at both
 * spacings of the full detail; its fine Earth within the bound at the
 * spacing. */
double terrain_land_lod_error(double x, double z, double spacing) {
    double err = TERRAIN_BASE_M * terrain_dropped(TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, spacing);
    double edge = plan_city_edge(x, z), e = region_rise(edge), rise = detail_rise(edge);
    TerrainChart wc = terrain_world_chart();
    TerrainDir d = terrain_chart_dir(&wc, x, z);
    double fine = terrain_earth_lod_error(d, spacing);
    double coarse = spacing > TERRAIN_LIFT_SPACING ? fine + terrain_earth_lod_error(d, TERRAIN_LIFT_SPACING) : 0.0;
    err += (1.0 - rise) * coarse + rise * fine;
    if (e > 0.0) {
        Region r;
        Macro m;
        Frame fr[FRAME_MAX];
        macro_of(d, edge, 0.0, 1, &m);
        int n = region_frames(d, &m, &r, fr);
        double t[TERRAIN_TYPES];
        t[TERRAIN_PLAINS] = TERRAIN_PLAINS_M * terrain_dropped(TERRAIN_PLAINS_WAVE, TERRAIN_PLAINS_OCTAVES, spacing);
        t[TERRAIN_HILLS] = TERRAIN_HILLS_M * terrain_dropped(TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, spacing);
        t[TERRAIN_MOUNTAINS] = m.amp * TERRAIN_RIDGE_M * terrain_dropped(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, spacing);
        t[TERRAIN_PLATEAU] = TERRAIN_PLATEAU_M * 1.5 / TERRAIN_MASK_STEP * terrain_dropped(TERRAIN_MASK_WAVE, TERRAIN_MASK_OCTAVES, spacing) +
                             TERRAIN_TOP_M * terrain_dropped(TERRAIN_TOP_WAVE, TERRAIN_TOP_OCTAVES, spacing);
        for (int f = 0; f < n; ++f)
            for (int k = 0; k < TERRAIN_TYPES; ++k) err += e * m.rough * fr[f].wt * fr[f].w[k] * t[k];
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
