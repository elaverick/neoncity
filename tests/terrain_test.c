/* Terrain and city footprint tests (src/terrain, plan_city_edge).
 *
 * The terrain is a pure function of position: the same point gives the same
 * height whichever order it is asked in. This checks that, then the shape
 * promises: heights are finite and within their amplitudes, the range query
 * really bounds them, the ground under the city stays gentle, the belt joins
 * city and land without a cliff, and the footprint is where it should be.
 *
 * Then the region's landforms: weights that blend, every type present, each
 * with its own character, belts along their strike, sharp escarpments, and
 * continuous canyons.
 *
 * Then levels of detail and the clipmap: coarse spacings drop only fine
 * detail; filled samples are the ground at their level, each level's parent
 * heights are its parent's own samples, windows nest, the blend into the
 * parent ends exactly at each window's edge, a move fills exactly the newly
 * exposed samples within its budget, and a clipmap that followed any camera
 * path holds what one filled from scratch at the same camera holds, block
 * bounds included.
 *
 * Then the screen-space error's choice of levels: coarser over flat ground
 * and from high up, steady at a threshold, and never letting the terrain
 * through the city's ground.
 *
 * Then the planet: the chart, the cube-sphere's patches and their seams, the
 * ground over the whole sphere, the bend and horizon culling; then the
 * patches' meshes and the selection of which to draw (terrain_test --patches
 * runs only those).
 *
 * terrain_test --fingerprint prints a hash of the heights, the footprint and
 * some filled clipmaps instead, to compare what two builds generate;
 * terrain_test --map out.ppm [km] draws the land round the city (see map);
 * terrain_test --planet-map out.ppm [px] the whole planet (see planet_map). */
#include "terrain_internal.h"
#include "plan.h"
#include "city.h"
#include "hash.h"
#include "mathlib.h"
#include "thread.h"
#include <float.h>
#include <time.h>

#define N_POINTS     20000
#define REGION       25000.0   /* points scattered this far round the city centre (m) */
#define URBAN_SLOPE  0.03      /* steepest urban grade allowed */
#define BELT_SLOPE   0.12      /* steepest ground in the belt */
#define LAND_SLOPE   3.0       /* steepest land: escarpments and canyon walls are cliffs, up to about 70 deg */

static double g_x[N_POINTS], g_z[N_POINTS], g_ref[N_POINTS];
static int g_failures;

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static uint64_t fnv(const void *data, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* Steepest slope at a point, by central differences 1 m either side. */
static double slope_of(double (*f)(double, double), double x, double z) {
    double gx = 0.5 * (f(x + 1.0, z) - f(x - 1.0, z)), gz = 0.5 * (f(x, z + 1.0) - f(x, z - 1.0));
    return sqrt(gx * gx + gz * gz);
}

/* The urban relief's amplitude at a point: its settlement's (m). */
static double relief_at(double x, double z) { return TERRAIN_URBAN_GRADED ? plan_settlement(plan_settlement_near(x, z)).relief_m : 0.0; }
/* The city's ground: its settlement's relief, on the planet's highlands (0 round the megacity). */
static double urban_graded(double x, double z) {
    double h = relief_at(x, z) * terrain_urban_shape(x, z), lift = terrain_urban_lift(x, z);
    if (lift != 0.0) h += lift;
    return h;
}

/* --- Landforms -------------------------------------------------------------- */

#define LF_SPAN 60000.0   /* landforms surveyed this far round the city centre (m) */

/* 5b. The region's landforms: weights that blend, every type present round
 *     the city, each with its own character where it holds the land alone,
 *     mountain belts running along their strike, plateaus with sharp
 *     escarpments, and canyons cut continuously, no deeper than their deepest. */
static void test_landforms(Rng *r) {
    DVec2 c = plan_town_centre(0, 0);
    const double full = TERRAIN_REGION_FROM + TERRAIN_LAND_RISE;   /* the region's landforms at full height beyond this edge measure */
    int lead[TERRAIN_TYPES] = { 0 }, n = 0, badW = 0;
    double slopeSum[TERRAIN_TYPES] = { 0 }, slopeMax[TERRAIN_TYPES] = { 0 };
    int slopeN[TERRAIN_TYPES] = { 0 };
    for (double z = c.z - LF_SPAN; z <= c.z + LF_SPAN; z += 500.0)
        for (double x = c.x - LF_SPAN; x <= c.x + LF_SPAN; x += 500.0) {
            TerrainLandform lf;
            terrain_landform(x, z, &lf);
            double sum = 0.0;
            for (int t = 0; t < TERRAIN_TYPES; ++t) {
                if (lf.w[t] < 0.0f || lf.w[t] > 1.0f) badW++;
                sum += lf.w[t];
                if (lf.w[t] > 0.5f) lead[t]++;
            }
            if (fabs(sum - 1.0) > 1e-5 || lf.canyon < 0.0f || lf.canyon > TERRAIN_CANYON_DEEP_M) badW++;
            n++;
            if (plan_city_edge(x, z) < full || lf.canyon > 0.0f) continue;
            for (int t = 0; t < TERRAIN_TYPES; ++t)
                if (lf.w[t] > 0.95f) {
                    double s = slope_of(terrain_height, x, z);
                    slopeSum[t] += s; slopeN[t]++;
                    slopeMax[t] = fmax(slopeMax[t], s);
                }
        }
    static const char *names[TERRAIN_TYPES] = { "plains", "hills", "mountains", "plateaus" };
    printf("landforms over %.0f km square round the centre: leading", 2.0 * LF_SPAN / 1000.0);
    for (int t = 0; t < TERRAIN_TYPES; ++t) printf(" %s %.0f%%", names[t], 100.0 * lead[t] / n);
    printf("\n  mean (steepest) slope where each holds the land alone:");
    for (int t = 0; t < TERRAIN_TYPES; ++t) printf(" %s %.1f%% (%.0f%%)", names[t], 100.0 * slopeSum[t] / fmax(slopeN[t], 1), 100.0 * slopeMax[t]);
    printf("\n");
    CHECK(badW == 0, "%d landform weights out of [0, 1], not summing to 1, or canyons out of depth", badW);
    for (int t = 0; t < TERRAIN_TYPES; ++t) {
        CHECK(lead[t] > n / 20, "%s lead only %d of %d points round the city", names[t], lead[t], n);
        CHECK(slopeN[t] > 20, "%s hold the land alone at only %d points", names[t], slopeN[t]);
    }
    double mean[TERRAIN_TYPES];
    for (int t = 0; t < TERRAIN_TYPES; ++t) mean[t] = slopeSum[t] / fmax(slopeN[t], 1);
    CHECK(mean[TERRAIN_PLAINS] < 0.03 && mean[TERRAIN_PLAINS] < mean[TERRAIN_HILLS] && mean[TERRAIN_HILLS] < mean[TERRAIN_MOUNTAINS],
          "slopes don't rise from plains (%.1f%%) through hills (%.1f%%) to mountains (%.1f%%)", 100.0 * mean[0], 100.0 * mean[1], 100.0 * mean[2]);
    CHECK(slopeMax[TERRAIN_PLATEAU] > 1.0, "the plateaus' escarpments are smoothed away: steepest %.0f%%", 100.0 * slopeMax[TERRAIN_PLATEAU]);

    /* Belts: the mountains' weight holds about as long along the strike as across it, times the stretch. */
    double along = 0.0, across = 0.0;
    int runsA = 0, runsC = 0;
    for (int k = 0; k < 200; ++k) {
        float u0 = rng_float(r);
        float v0 = rng_float(r);
        double ox = c.x + (2.0 * u0 - 1.0) * LF_SPAN, oz = c.z + (2.0 * v0 - 1.0) * LF_SPAN;
        for (int dir = 0; dir < 2; ++dir) {
            double dx = dir ? -TERRAIN_STRIKE_Z : TERRAIN_STRIKE_X, dz = dir ? TERRAIN_STRIKE_X : TERRAIN_STRIKE_Z;
            int in = 0, len = 0;
            for (int s = 0; s <= 400; ++s) {
                TerrainLandform lf;
                terrain_landform(ox + dx * s * 250.0, oz + dz * s * 250.0, &lf);
                int now = lf.w[TERRAIN_MOUNTAINS] > 0.5f;
                if (now) len++;
                if (in && (!now || s == 400)) { if (dir) { across += len; runsC++; } else { along += len; runsA++; } len = 0; }
                in = now;
            }
        }
    }
    along /= fmax(runsA, 1); across /= fmax(runsC, 1);
    printf("  mountain belts run %.1f km along the strike, %.1f km across it\n", along * 0.25, across * 0.25);
    CHECK(along > 1.5 * across, "mountain belts don't run along their strike: %.1f km along, %.1f km across", along * 0.25, across * 0.25);

    /* Canyons: continuous (their cut changes no faster than their walls climb), and present. */
    double worst = 0.0, deepest = 0.0;
    for (int k = 0; k < 40000; ++k) {
        float u0 = rng_float(r);
        float v0 = rng_float(r);
        float a = rng_float(r);
        double x = c.x + (2.0 * u0 - 1.0) * LF_SPAN, z = c.z + (2.0 * v0 - 1.0) * LF_SPAN;
        double d0 = terrain_canyon_depth(x, z), d1 = terrain_canyon_depth(x + cos(6.2832 * a), z + sin(6.2832 * a));
        worst = fmax(worst, fabs(d1 - d0));
        deepest = fmax(deepest, d0);
    }
    for (double z = c.z - LF_SPAN; z <= c.z + LF_SPAN; z += 100.0)   /* along lines, to find the floors */
        deepest = fmax(deepest, terrain_canyon_depth(c.x + 0.37 * LF_SPAN, z));
    printf("  canyons cut up to %.0f m deep, changing by at most %.2f m per metre\n", deepest, worst);
    CHECK(worst <= 1.5 * TERRAIN_CANYON_WALL + 0.01, "a canyon's cut jumps by %.2f m over a metre", worst);
    CHECK(deepest > 150.0 && deepest <= TERRAIN_CANYON_DEEP_M, "canyons cut %.0f m at most round the city", deepest);
}

/* The first point, scanning out from the city centre, where landform `type`
 * holds the land alone at full height, no canyon cuts, and the ground is at
 * least `slope` steep. */
static int find_landform(int type, double slope, double *ox, double *oz) {
    DVec2 c = plan_town_centre(0, 0);
    for (double r = 8000.0; r <= LF_SPAN; r += 500.0)
        for (int a = 0; a < 360; a += 3) {
            double x = c.x + r * cos(a * (PI_F / 180.0)), z = c.z + r * sin(a * (PI_F / 180.0));
            if (plan_city_edge(x, z) < TERRAIN_REGION_FROM + TERRAIN_LAND_RISE) continue;
            TerrainLandform lf;
            terrain_landform(x, z, &lf);
            if (lf.w[type] < 0.98f || lf.canyon > 0.0f || slope_of(terrain_height, x, z) < slope) continue;
            *ox = x; *oz = z;
            return 1;
        }
    return 0;
}

/* --- Levels of detail and the clipmap -------------------------------------- */

#define WALK_STEPS   6000
#define CHECKPOINTS  12

static int same_octaves(double s0, double s1) { return terrain_lod_key(s0) == terrain_lod_key(s1); }

/* 6. Coarse spacings leave out fine detail only: within the range bounds, and
 *    no further from full detail than the dropped octaves can reach. */
static void test_lod_heights(void) {
    double worst[TERRAIN_CLIP_LEVELS] = { 0 };
    for (int i = 0; i < N_POINTS; i += 4) {
        double x = g_x[i], z = g_z[i], h = terrain_height(x, z), h0 = terrain_height_lod(x, z, 0.0), rlo, rhi, relief = relief_at(x, z);
        CHECK(memcmp(&h, &h0, sizeof(h)) == 0, "terrain_height_lod at spacing 0 differs from terrain_height at (%.1f, %.1f)", x, z);
        terrain_height_range(x, z, x, z, &rlo, &rhi);
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
            double s = terrain_clip_spacing(l), hl = terrain_height_lod(x, z, s);
            /* 1e-3 m: each fractal sum is rounded to float before scaling. */
            double bound = terrain_land_lod_error(x, z, s) + relief * terrain_dropped(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s) +
                           terrain_road_lod_error(x, z, s) + 1e-3;
            CHECK(hl >= rlo && hl <= rhi, "height %.3f at spacing %.0f m at (%.1f, %.1f) outside its range", hl, s, x, z);
            CHECK(fabs(hl - h) <= bound, "height at spacing %.0f m at (%.1f, %.1f) is %.3f m from full detail (bound %.3f)", s, x, z, fabs(hl - h), bound);
            if (fabs(hl - h) > worst[l]) worst[l] = fabs(hl - h);
        }
    }
    printf("levels of detail (octaves kept of urban %d, base %d, hills %d, ridges %d; largest change from full detail):\n",
           TERRAIN_URBAN_OCTAVES, TERRAIN_BASE_OCTAVES, TERRAIN_HILLS_OCTAVES, TERRAIN_RIDGE_OCTAVES);
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        double s = terrain_clip_spacing(l);
        printf("  level %d  %4.0f m  urban %d  base %d  hills %d  ridges %d  %6.2f m\n", l, s, terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s),
               terrain_octaves(TERRAIN_BASE_WAVE, TERRAIN_BASE_OCTAVES, s), terrain_octaves(TERRAIN_HILLS_WAVE, TERRAIN_HILLS_OCTAVES, s),
               terrain_octaves(TERRAIN_RIDGE_WAVE, TERRAIN_RIDGE_OCTAVES, s), worst[l]);
    }
}

static int in_window(const int64_t o[2], int64_t i, int64_t j) {
    return i >= o[0] && i < o[0] + TERRAIN_CLIP_W && j >= o[1] && j < o[1] + TERRAIN_CLIP_W;
}

/* Every sample of level l's window against the drawn surface and its parent's own
 * samples. Needs l and every coarser level ready. */
static void check_contents(const TerrainClipmap *c, int l) {
    const TerrainClipLevel *lv = &c->level[l], *pv = l + 1 < TERRAIN_CLIP_LEVELS ? &c->level[l + 1] : NULL;
    double s = lv->spacing;
    int same = pv && same_octaves(s, pv->spacing), bad = 0;
    for (int64_t j = lv->origin[1]; j < lv->origin[1] + TERRAIN_CLIP_W; ++j)
        for (int64_t i = lv->origin[0]; i < lv->origin[0] + TERRAIN_CLIP_W; ++i) {
            const TerrainSample *t = terrain_clip_sample(lv, i, j);
            float urban;
            float h = (float)terrain_surface_lod((double)i * s, (double)j * s, s, &urban), hc = h;
            if (pv) {
                int ox = i % 2 != 0, oz = j % 2 != 0;
                int64_t a = (i - ox) / 2, b = (j - oz) / 2;
                float p00 = terrain_clip_sample(pv, a, b)->h;
                if (!ox && !oz)     hc = p00;
                else if (ox && !oz) hc = 0.5f * (p00 + terrain_clip_sample(pv, a + 1, b)->h);
                else if (!ox && oz) hc = 0.5f * (p00 + terrain_clip_sample(pv, a, b + 1)->h);
                else                hc = 0.5f * (p00 + terrain_clip_sample(pv, a + 1, b + 1)->h);
                /* Where both levels keep the same octaves, the shared samples are
                 * the same ground (each level then sinks it under the city by its own
                 * terrain_urban_lod_error, so the drawn heights may differ there). */
                double gl = terrain_height_lod((double)i * s, (double)j * s, s), gp = terrain_height_lod((double)i * s, (double)j * s, pv->spacing);
                if (same && !ox && !oz && gl != gp && bad++ < 5)
                    CHECK(0, "level %d sample (%lld, %lld) is ground %.6f, its parent's %.6f", l, (long long)i, (long long)j, gl, gp);
            }
            double gx = (terrain_surface_lod((double)(i + 1) * s, (double)j * s, s, NULL) - terrain_surface_lod((double)(i - 1) * s, (double)j * s, s, NULL)) / (2.0 * s);
            double gz = (terrain_surface_lod((double)i * s, (double)(j + 1) * s, s, NULL) - terrain_surface_lod((double)i * s, (double)(j - 1) * s, s, NULL)) / (2.0 * s);
            double inv = 1.0 / sqrt(1.0 + gx * gx + gz * gz);
            int ok = memcmp(&t->h, &h, sizeof(float)) == 0 && memcmp(&t->hc, &hc, sizeof(float)) == 0 &&
                     fabs(t->nx + gx * inv) <= 1e-6 && fabs(t->nz + gz * inv) <= 1e-6 && t->urban == urban &&
                     urban == plan_urban((double)i * s, (double)j * s);
            if (!ok && bad++ < 5)
                CHECK(0, "level %d sample (%lld, %lld) holds h %.6f hc %.6f n (%.5f, %.5f) urban %.4f, wants %.6f %.6f (%.5f, %.5f) %.4f", l,
                      (long long)i, (long long)j, t->h, t->hc, t->nx, t->nz, t->urban, h, hc, -gx * inv, -gz * inv, urban);
        }
}

/* Each ready level inside its ready parent, edges on the parent's samples,
 * and the blend: 1 on every edge sample, 0 wherever the child is drawn. */
static void check_nesting_and_morph(const TerrainClipmap *c, int *marginMin, int *marginMax) {
    for (int l = c->finest; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *lv = &c->level[l];
        double s = lv->spacing;
        int64_t o0 = lv->origin[0], o1 = lv->origin[1], e0 = o0 + TERRAIN_CLIP_W - 1, e1 = o1 + TERRAIN_CLIP_W - 1;
        int edgeBad = 0, innerBad = 0;
        for (int64_t k = 0; k < TERRAIN_CLIP_W; ++k) {
            double edge[4][2] = { { (double)(o0 + k) * s, (double)o1 * s }, { (double)(o0 + k) * s, (double)e1 * s },
                                  { (double)o0 * s, (double)(o1 + k) * s }, { (double)e0 * s, (double)(o1 + k) * s } };
            for (int q = 0; q < 4; ++q)
                if (terrain_clip_morph(c, l, edge[q][0], edge[q][1]) != 1.0f) edgeBad++;
        }
        CHECK(edgeBad == 0, "level %d: blend short of 1 at %d edge samples", l, edgeBad);
        if (l + 1 < TERRAIN_CLIP_LEVELS) {
            const TerrainClipLevel *pv = &c->level[l + 1];
            CHECK(o0 % 2 == 0 && o1 % 2 == 0, "level %d origin (%lld, %lld) is not on its parent's samples", l, (long long)o0, (long long)o1);
            int64_t m[4] = { o0 / 2 - pv->origin[0], o1 / 2 - pv->origin[1],
                             pv->origin[0] + TERRAIN_CLIP_W - 1 - e0 / 2, pv->origin[1] + TERRAIN_CLIP_W - 1 - e1 / 2 };
            for (int q = 0; q < 4; ++q) {
                CHECK(m[q] >= 1, "level %d pokes out of its parent (margin %lld)", l, (long long)m[q]);
                if (m[q] < *marginMin) *marginMin = (int)m[q];
                if (m[q] > *marginMax) *marginMax = (int)m[q];
            }
        }
        if (l > c->finest) {
            const TerrainClipLevel *cv = &c->level[l - 1];
            double cs = cv->spacing;
            for (int64_t j = cv->origin[1]; j < cv->origin[1] + TERRAIN_CLIP_W; ++j)
                for (int64_t i = cv->origin[0]; i < cv->origin[0] + TERRAIN_CLIP_W; ++i)
                    if (terrain_clip_morph(c, l, (double)i * cs, (double)j * cs) != 0.0f) innerBad++;
            CHECK(innerBad == 0, "level %d blends into its parent at %d points where level %d is drawn", l, innerBad, l - 1);
        }
    }
}

/* The slots a level's update wrote are exactly the samples of its new window
 * its old one lacked (all of it if there was none), each written once. */
static void check_dirty(const TerrainClipLevel *lv, int l, int wasFilled, const int64_t was[2]) {
    static int count[TERRAIN_CLIP_N][TERRAIN_CLIP_N];
    memset(count, 0, sizeof(count));
    for (int k = 0; k < lv->n_dirty; ++k) {
        TerrainRect r = lv->dirty[k];
        if (!(0 <= r.x0 && r.x0 < r.x1 && r.x1 <= TERRAIN_CLIP_N && 0 <= r.z0 && r.z0 < r.z1 && r.z1 <= TERRAIN_CLIP_N)) {
            CHECK(0, "level %d dirty rectangle [%d,%d) x [%d,%d) is not inside storage", l, r.x0, r.x1, r.z0, r.z1);
            return;
        }
        for (int z = r.z0; z < r.z1; ++z)
            for (int x = r.x0; x < r.x1; ++x) count[z][x]++;
    }
    int bad = 0, written = 0;
    for (int64_t j = lv->origin[1]; j < lv->origin[1] + TERRAIN_CLIP_W; ++j)
        for (int64_t i = lv->origin[0]; i < lv->origin[0] + TERRAIN_CLIP_W; ++i) {
            int *n = &count[terrain_clip_slot(j)][terrain_clip_slot(i)];
            int want = !wasFilled || !in_window(was, i, j);
            if (*n != want) bad++;
            written += want;
            *n = 0;
        }
    for (int z = 0; z < TERRAIN_CLIP_N; ++z)
        for (int x = 0; x < TERRAIN_CLIP_N; ++x)
            if (count[z][x]) bad++;
    CHECK(bad == 0, "level %d: %d slots written wrongly moving (%lld, %lld) -> (%lld, %lld) (%d samples exposed)", l, bad,
          (long long)was[0], (long long)was[1], (long long)lv->origin[0], (long long)lv->origin[1], written);
}

/* Each storage block of a filled level bounds exactly the window's samples
 * in its slots, however the level got there. */
static void check_blocks(const TerrainClipLevel *lv, int l) {
    static TerrainBlock want[TERRAIN_CLIP_NB * TERRAIN_CLIP_NB];
    for (int k = 0; k < TERRAIN_CLIP_NB * TERRAIN_CLIP_NB; ++k) want[k] = (TerrainBlock){ FLT_MAX, -FLT_MAX, 0.0f };
    for (int64_t j = lv->origin[1]; j < lv->origin[1] + TERRAIN_CLIP_W; ++j)
        for (int64_t i = lv->origin[0]; i < lv->origin[0] + TERRAIN_CLIP_W; ++i) {
            const TerrainSample *t = terrain_clip_sample(lv, i, j);
            TerrainBlock *b = &want[(terrain_clip_slot(j) / TERRAIN_CLIP_BLOCK) * TERRAIN_CLIP_NB + terrain_clip_slot(i) / TERRAIN_CLIP_BLOCK];
            b->lo = fminf(b->lo, fminf(t->h, t->hc));
            b->hi = fmaxf(b->hi, fmaxf(t->h, t->hc));
            b->err = fmaxf(b->err, fabsf(t->h - t->hc));
        }
    int bad = 0;
    for (int k = 0; k < TERRAIN_CLIP_NB * TERRAIN_CLIP_NB; ++k)
        if (memcmp(&want[k], &lv->blocks[k], sizeof(TerrainBlock)) != 0 && bad++ < 3)
            CHECK(0, "level %d block %d bounds [%.4f, %.4f] err %.5f, its samples [%.4f, %.4f] err %.5f", l, k,
                  lv->blocks[k].lo, lv->blocks[k].hi, lv->blocks[k].err, want[k].lo, want[k].hi, want[k].err);
}

/* Updates a fill from scratch (or a jump) may take on the full budget: the
 * whole clipmap's heights, plus a quarter for the bands' aprons and the rows
 * that don't fit what an update has left. */
static int jump_bound(void) {
    int total = 0;
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) total += terrain_clip_fill_cost(l, TERRAIN_CLIP_W, TERRAIN_CLIP_W);
    return (int)ceil(1.25 * total / TERRAIN_CLIP_BUDGET) + 1;
}

static int fill_all(TerrainClipmap *c, double x, double z) {
    int updates = 0;
    do {
        terrain_clip_update(c, x, z, TERRAIN_CLIP_BUDGET);
        updates++;
    } while (c->finest > 0 && updates < 1000);
    return updates;
}

/* Two clipmaps at the same camera hold the same windows, sample for sample. */
static int same_clipmaps(const TerrainClipmap *a, const TerrainClipmap *b) {
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *la = &a->level[l], *lb = &b->level[l];
        if (la->origin[0] != lb->origin[0] || la->origin[1] != lb->origin[1]) return 0;
        for (int64_t j = la->origin[1]; j < la->origin[1] + TERRAIN_CLIP_W; ++j)
            for (int64_t i = la->origin[0]; i < la->origin[0] + TERRAIN_CLIP_W; ++i)
                if (memcmp(terrain_clip_sample(la, i, j), terrain_clip_sample(lb, i, j), sizeof(TerrainSample)) != 0) return 0;
    }
    return 1;
}

static uint64_t hash_clipmap(const TerrainClipmap *c, uint64_t h) {
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *lv = &c->level[l];
        h = fnv(lv->origin, sizeof(lv->origin), h);
        for (int64_t j = lv->origin[1]; j < lv->origin[1] + TERRAIN_CLIP_W; ++j)
            for (int64_t i = lv->origin[0]; i < lv->origin[0] + TERRAIN_CLIP_W; ++i) {
                const TerrainSample *t = terrain_clip_sample(lv, i, j);
                h = fnv(&t->h, sizeof(float), h);
                h = fnv(&t->hc, sizeof(float), h);
                h = fnv(&t->nx, sizeof(float), h);
                h = fnv(&t->nz, sizeof(float), h);
                h = fnv(&t->urban, sizeof(float), h);
            }
    }
    return h;
}

/* 7-10. The clipmap: a fill from scratch, then a long camera walk. */
static void test_clipmap(Rng *r) {
    DVec2 c0 = plan_town_centre(0, 0);
    TerrainClipmap clip, fresh;
    terrain_clip_init(&clip);
    terrain_clip_init(&fresh);

    /* 7. From scratch at the city's edge: every level ready, every sample right. */
    double cx = c0.x + 9300.0, cz = c0.z - 2100.0;
    clock_t t0 = clock();
    int updates = fill_all(&clip, cx, cz);
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;
    CHECK(clip.finest == 0 && updates <= jump_bound(), "fill from scratch took %d updates (finest ready level %d)", updates, clip.finest);
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        check_contents(&clip, l);
        check_blocks(&clip.level[l], l);
    }
    int mMin = 1 << 30, mMax = 0;
    check_nesting_and_morph(&clip, &mMin, &mMax);
    printf("clipmap: %d levels of %d^2 samples, %.0f m to %.0f m apart, %.1f km across\n", TERRAIN_CLIP_LEVELS, TERRAIN_CLIP_W,
           terrain_clip_spacing(0), terrain_clip_spacing(TERRAIN_CLIP_LEVELS - 1), (TERRAIN_CLIP_W - 1) * terrain_clip_spacing(TERRAIN_CLIP_LEVELS - 1) / 1000.0);
    printf("  filled from scratch in %d updates, %.0f ms (%.0f ns a sample)\n", updates, ms,
           1e6 * ms / ((double)TERRAIN_CLIP_LEVELS * TERRAIN_CLIP_W * TERRAIN_CLIP_W));

    /* 8-10. A walk: strolls, sprints, flights, dithering across a snap line and
     *       jumps, under full and starved budgets. After each update: the budget
     *       held, ready levels are contiguous from the coarsest, every move that
     *       started and finished in the update wrote exactly what it exposed,
     *       and idle levels wrote nothing. At checkpoints, the clipmap matches
     *       one filled from scratch at the same camera. */
    double x = cx, z = cz, heading = 0.0, line = 0.0;
    int mode = 0, budget = TERRAIN_CLIP_BUDGET, maxSpent = 0, steadyMisses = 0, steadyUpdates = 0, compared = 0, jumps = 0, worstRecovery = 0;
    int catching = 0, timing = 0, since = 0, moves = 0;
    for (int step = 0; step < WALK_STEPS; ++step) {
        if (step % 250 == 0) {
            mode = rng_int(r, 0, 4);
            int b = rng_int(r, 0, 3);
            budget = b == 0 ? TERRAIN_CLIP_W : b == 1 ? 2500 : TERRAIN_CLIP_BUDGET;
            /* Every level snaps at multiples of twice the coarsest spacing;
             * going there is a jump of up to one coarsest spacing. */
            const double snap = 2.0 * terrain_clip_spacing(TERRAIN_CLIP_LEVELS - 1);
            line = snap * floor(x / snap + 0.5);
            if (mode == 3) catching = 1;
            if (mode == 4) {   /* jump 3 to 40 km, kept within 30 km of the city */
                float a = rng_float(r);
                float d = rng_float(r);
                x += cos(a * 2.0 * PI_F) * (3000.0 + 37000.0 * d);
                z += sin(a * 2.0 * PI_F) * (3000.0 + 37000.0 * d);
                if (hypot(x - c0.x, z - c0.z) > 30000.0) { x = c0.x + (x - c0.x) * 0.25; z = c0.z + (z - c0.z) * 0.25; }
                jumps++;
                timing = budget == TERRAIN_CLIP_BUDGET && clip.finest == 0;   /* time jumps from a settled clipmap */
                catching = 1;
                since = 0;
            }
        }
        float u = rng_float(r);
        heading += (u - 0.5) * 0.3;
        double speed = mode == 0 ? 0.03 : mode == 1 ? 0.3 : mode == 2 ? 5.0 : 0.0;
        x += cos(heading) * speed;
        z += sin(heading) * speed;
        if (mode == 3) x = line + (step & 1 ? 0.6 : -0.6);   /* across every level's snap line each step */

        int wasFilled[TERRAIN_CLIP_LEVELS], wasIdle[TERRAIN_CLIP_LEVELS];
        int64_t was[TERRAIN_CLIP_LEVELS][2];
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
            wasFilled[l] = clip.level[l].filled;
            wasIdle[l] = clip.level[l].n_pending == 0;
            was[l][0] = clip.level[l].origin[0];
            was[l][1] = clip.level[l].origin[1];
        }
        int spent = terrain_clip_update(&clip, x, z, budget);
        const int row = terrain_clip_fill_cost(0, TERRAIN_CLIP_W, 1);
        CHECK(spent <= (budget > row ? budget : row), "update evaluated %d heights on a budget of %d", spent, budget);
        if (spent > maxSpent) maxSpent = spent;
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
            const TerrainClipLevel *lv = &clip.level[l];
            if (l >= clip.finest) CHECK(lv->ready, "level %d is not ready but finest is %d", l, clip.finest);
            int moved = !wasFilled[l] || was[l][0] != lv->origin[0] || was[l][1] != lv->origin[1];
            if (wasIdle[l] && lv->filled && moved) { check_dirty(lv, l, wasFilled[l], was[l]); moves++; }
            if (wasIdle[l] && wasFilled[l] && lv->filled && !moved) CHECK(lv->n_dirty == 0, "level %d wrote %d rectangles without moving", l, lv->n_dirty);
        }
        if (clip.finest > 0) CHECK(!clip.level[clip.finest - 1].ready, "finest %d is not the finest ready level", clip.finest);
        /* A starved budget lets levels fall behind; on the full budget they
         * catch up, and then must keep up with any motion short of a jump. */
        if (budget != TERRAIN_CLIP_BUDGET) {
            catching = 1;
            timing = 0;
        } else if (catching) {
            since++;
            if (clip.finest == 0) {
                if (timing && since > worstRecovery) worstRecovery = since;
                catching = timing = 0;
            }
        } else {
            steadyUpdates++;
            if (clip.finest != 0) steadyMisses++;
        }

        if ((step + 1) % (WALK_STEPS / CHECKPOINTS) == 0) {
            fill_all(&clip, x, z);   /* let a starved budget catch up */
            terrain_clip_free(&fresh);
            terrain_clip_init(&fresh);
            fill_all(&fresh, x, z);
            CHECK(same_clipmaps(&clip, &fresh), "after %d steps the clipmap differs from one filled from scratch at (%.1f, %.1f)", step + 1, x, z);
            for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) check_blocks(&clip.level[l], l);
            check_nesting_and_morph(&clip, &mMin, &mMax);
            catching = timing = 0;
            compared++;
        }
    }
    CHECK(worstRecovery <= jump_bound(), "a jump took %d updates to fill in on the full budget (bound %d)", worstRecovery, jump_bound());
    CHECK(steadyMisses == 0, "%d of %d updates while moving on the full budget left a level not ready", steadyMisses, steadyUpdates);
    printf("  walk: %d updates, %d moves checked, %d jumps (filled in within %d updates), %d steady updates all ready,\n"
           "        at most %d heights an update, %d comparisons with a fresh fill; children sit %d-%d parent samples in\n",
           WALK_STEPS, moves, jumps, worstRecovery, steadyUpdates, maxSpent, compared, mMin, mMax);

    /* 10b. Levels the selection won't draw soon are left alone: with want at
     *      LAZY_WANT, a flight leaves the levels finer than LAZY_WANT - 1 where
     *      they were, writing nothing, while the rest keep up. Asked for again,
     *      they catch up to what a fresh fill holds. */
    enum { LAZY_WANT = 4 };
    fill_all(&clip, x, z);
    int64_t kept[TERRAIN_CLIP_LEVELS][2];
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) { kept[l][0] = clip.level[l].origin[0]; kept[l][1] = clip.level[l].origin[1]; }
    int lazyWrites = 0, lazyMisses = 0;
    clip.want = LAZY_WANT;
    for (int step = 0; step < 200; ++step) {
        x += 6.0;
        terrain_clip_update(&clip, x, z, TERRAIN_CLIP_BUDGET);
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
            if (l < LAZY_WANT - 1) lazyWrites += clip.level[l].n_dirty;
            else lazyMisses += !clip.level[l].ready;
        }
    }
    int lazyKept = 1;
    for (int l = 0; l < LAZY_WANT - 1; ++l) lazyKept &= clip.level[l].origin[0] == kept[l][0] && clip.level[l].origin[1] == kept[l][1];
    CHECK(lazyWrites == 0 && lazyKept, "levels finer than %d moved (%d rectangles written) while want was %d", LAZY_WANT - 1, lazyWrites, LAZY_WANT);
    CHECK(lazyMisses == 0, "levels %d and coarser fell behind %d times while finer ones were left", LAZY_WANT - 1, lazyMisses);
    CHECK(clip.finest == LAZY_WANT - 1, "with want %d the finest ready level is %d", LAZY_WANT, clip.finest);
    clip.want = 0;
    int caught = fill_all(&clip, x, z);
    terrain_clip_free(&fresh);
    terrain_clip_init(&fresh);
    fill_all(&fresh, x, z);
    CHECK(same_clipmaps(&clip, &fresh), "the levels left alone didn't catch up to a fresh fill");
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) check_blocks(&clip.level[l], l);
    printf("  a 1.2 km flight with level %d wanted left levels 0-%d alone; asked for again, they caught up in %d updates\n",
           LAZY_WANT, LAZY_WANT - 2, caught);
    terrain_clip_free(&clip);
    terrain_clip_free(&fresh);
}

/* --- Screen-space error --------------------------------------------------- */

/* The game's view: 1080 rows, fovy 72 degrees. */
static double sse_k(void) { return 1080.0 / (2.0 * tan(36.0 * PI_F / 180.0)); }

/* Cameras the selection is checked at, round the city centre (m). */
static const double SSE_CAMS[3][2] = { { 0.0, 0.0 }, { 9300.0, -2100.0 }, { -21000.0, 14500.0 } };
static const char  *SSE_NAMES[3] = { "centre", "edge", "land" };
static const double SSE_HEIGHTS[5] = { 1.7, 10.0, 60.0, 300.0, 1500.0 };   /* above the ground */

/* The finest level drawn for a camera at height y over a filled clipmap,
 * from a selection that starts from the finest level. */
static int select_fresh(TerrainClipmap *c, double y) {
    c->want = 0;
    return terrain_clip_select(c, y, sse_k());
}

/* Where the city stands (plan_urban 1), the drawn terrain at every level must
 * stay under its lowest ground, the asphalt 3 cm under city_ground_y,
 * whatever the morph: the samples (h and hc) and the middles of the cells'
 * diagonals, where the triangles bend. Returns the least clearance (m). */
static double urban_clearance(const TerrainClipLevel *lv) {
    double s = lv->spacing, least = 1e9;
    for (int64_t j = lv->origin[1]; j + 1 < lv->origin[1] + TERRAIN_CLIP_W; ++j)
        for (int64_t i = lv->origin[0]; i + 1 < lv->origin[0] + TERRAIN_CLIP_W; ++i) {
            const TerrainSample *a = terrain_clip_sample(lv, i, j), *b = terrain_clip_sample(lv, i + 1, j + 1);
            if (a->urban < 1.0f || b->urban < 1.0f) continue;
            double x = (double)i * s, z = (double)j * s;
            least = fmin(least, terrain_urban_height(x, z) - 0.03 - fmax(a->h, a->hc));
            least = fmin(least, terrain_urban_height(x + 0.5 * s, z + 0.5 * s) - 0.03 - 0.5 * ((double)a->h + b->h));
            least = fmin(least, terrain_urban_height(x + 0.5 * s, z + 0.5 * s) - 0.03 - 0.5 * ((double)a->hc + b->hc));
        }
    return least;
}

/* The drawn surface of level l at world (x, z): its samples, each blended into
 * its parent by the morph as the vertex shader does, interpolated over the
 * triangle holding the point (cells split along (i, j)-(i + 1, j + 1)). */
static double drawn_height(const TerrainClipmap *c, int l, double x, double z) {
    const TerrainClipLevel *lv = &c->level[l];
    double s = lv->spacing, fi = x / s, fj = z / s;
    int64_t i = (int64_t)floor(fi), j = (int64_t)floor(fj);
    double u = fi - (double)i, v = fj - (double)j, d[2][2];
    for (int b = 0; b < 2; ++b)
        for (int a = 0; a < 2; ++a) {
            const TerrainSample *t = terrain_clip_sample(lv, i + a, j + b);
            double m = terrain_clip_morph(c, l, (double)(i + a) * s, (double)(j + b) * s);
            d[b][a] = t->h + m * ((double)t->hc - t->h);
        }
    return u >= v ? d[0][0] * (1.0 - u) + d[0][1] * (u - v) + d[1][1] * v
                  : d[0][0] * (1.0 - v) + d[1][0] * (v - u) + d[1][1] * u;
}

/* How far the drawn ground strays from what the player stands on beyond the
 * city (terrain_surface, full detail) within a few metres of the camera, for
 * the level the screen-space error draws at street level. */
static double feet_error(const TerrainClipmap *c, int l, double x, double z) {
    double worst = 0.0;
    for (int b = -6; b <= 6; ++b)
        for (int a = -6; a <= 6; ++a) {
            double px = x + 0.5 * a, pz = z + 0.5 * b;
            worst = fmax(worst, fabs(drawn_height(c, l, px, pz) - terrain_surface(px, pz)));
        }
    return worst;
}
#define FEET_TOLERANCE 0.03   /* m: what the drawn ground may stray from the feet's */

/* 11. Which levels the screen-space error draws: fine levels dropped over the
 *     city's flat ground and from high cameras; the city's ground over the
 *     terrain whichever level is drawn under it; and the hysteresis holding
 *     the choice while the camera hovers at a threshold. */
static void test_select(void) {
    DVec2 c0 = plan_town_centre(0, 0);
    TerrainClipmap clip;
    double clear[TERRAIN_CLIP_LEVELS];
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) clear[l] = 1e9;
    printf("screen-space error (K %.0f px, refine over %.1f px, coarsen under %.1f px): finest level drawn\n", sse_k(), TERRAIN_SSE_REFINE, TERRAIN_SSE_COARSEN);
    printf("          ");
    for (int h = 0; h < 5; ++h) printf("  %6.1f m", SSE_HEIGHTS[h]);
    printf("\n");
    for (int k = 0; k < 3; ++k) {
        double x = c0.x + SSE_CAMS[k][0], z = c0.z + SSE_CAMS[k][1], ground = terrain_height(x, z);
        terrain_clip_init(&clip);
        fill_all(&clip, x, z);
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) clear[l] = fmin(clear[l], urban_clearance(&clip.level[l]));
        int want[5];
        printf("  %-7s ", SSE_NAMES[k]);
        for (int h = 0; h < 5; ++h) {
            want[h] = select_fresh(&clip, ground + SSE_HEIGHTS[h]);
            printf("  %8d", want[h]);
            /* Selecting again from where it settled changes nothing. */
            CHECK(terrain_clip_select(&clip, ground + SSE_HEIGHTS[h], sse_k()) == want[h], "%s at %.1f m: a second selection moved", SSE_NAMES[k], SSE_HEIGHTS[h]);
        }
        printf("\n");
        CHECK(want[4] > want[0], "%s: a camera %.0f m up draws level %d, as fine as at street level", SSE_NAMES[k], SSE_HEIGHTS[4], want[4]);
        if (k == 0) CHECK(want[0] >= 2, "the city centre at street level draws level %d: its flat ground needs no metre-scale levels", want[0]);
        /* 11b. The ground under the feet: at street level the drawn level stays
         *      within FEET_TOLERANCE of what the player walks on, 3 m round. */
        int street = select_fresh(&clip, ground + SSE_HEIGHTS[0]);
        double feet = feet_error(&clip, street, x, z);
        CHECK(feet <= FEET_TOLERANCE, "%s: the drawn ground (level %d) strays %.3f m from the feet's", SSE_NAMES[k], street, feet);
        printf("          at street level the drawn ground is within %.1f mm of the feet's\n", 1000.0 * feet);

        /* Hysteresis: climb in 2% steps to the first switch, then hover
         * either side of it. */
        double y = SSE_HEIGHTS[0];
        int w0 = select_fresh(&clip, ground + y), w = w0;
        while (w == w0 && y < 5000.0) {
            y *= 1.02;
            w = terrain_clip_select(&clip, ground + y, sse_k());
        }
        CHECK(w > w0, "%s: climbing to %.0f m never coarsened from level %d", SSE_NAMES[k], y, w0);
        int flips = 0;
        for (int step = 0; step < 50; ++step) {
            int now = terrain_clip_select(&clip, ground + (step & 1 ? y : y / 1.02), sse_k());
            flips += now != w;
            w = now;
        }
        CHECK(flips == 0, "%s: hovering at %.1f m, the level switched %d times", SSE_NAMES[k], y, flips);
        printf("          level %d to %d at %.1f m up, held while hovering 2%% either side\n", w0, w, y);
        terrain_clip_free(&clip);
    }
    /* 11c. Different landforms get different detail (design doc 6.1): over
     *      mountains and at an escarpment the selection keeps finer levels
     *      than over plains, the camera as high above each. */
    {
        static const char *names[3] = { "plains", "ridges", "escarpment" };
        static const double heights[2] = { 60.0, 300.0 };
        int want[3][2];
        for (int k = 0; k < 3; ++k) {
            double x, z;
            if (!find_landform(k == 0 ? TERRAIN_PLAINS : k == 1 ? TERRAIN_MOUNTAINS : TERRAIN_PLATEAU, k == 2 ? 1.0 : 0.0, &x, &z)) {
                CHECK(0, "no %s found round the city", names[k]);
                return;
            }
            double ground = terrain_height(x, z);
            terrain_clip_init(&clip);
            fill_all(&clip, x, z);
            for (int h = 0; h < 2; ++h) want[k][h] = select_fresh(&clip, ground + heights[h]);
            terrain_clip_free(&clip);
        }
        printf("  by landform, %.0f m and %.0f m up:", heights[0], heights[1]);
        for (int k = 0; k < 3; ++k) printf(" %s %d, %d;", names[k], want[k][0], want[k][1]);
        printf("\n");
        for (int h = 0; h < 2; ++h)
            CHECK(want[1][h] < want[0][h] && want[2][h] < want[0][h], "%.0f m up, ridges (level %d) and an escarpment (%d) aren't drawn finer than plains (%d)",
                  heights[h], want[1][h], want[2][h], want[0][h]);
    }
    printf("  the city's ground over the drawn terrain by at least:");
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        printf(" %.2f", clear[l]);
        CHECK(clear[l] > 0.0, "level %d's terrain pokes %.3f m through the city's ground", l, -clear[l]);
    }
    printf(" m\n");
}

/* --- Level transitions ------------------------------------------------------ */

/* The finest surface drawn as a fractional level: level l with its morph at
 * least m shows a surface between its own (l) and its parent's (l + 1). */
static double fade_surface(const TerrainFade *f) { return f->shown + terrain_fade_morph(f, f->shown); }

/* Steps f towards target at 60 frames a second until it draws it, up to
 * `limit` frames. Checks every frame that the surface moves the target's way,
 * by no more than the fastest blend allows, and is never finer than the
 * finest ready level. Returns the frames taken. */
static int fade_to(TerrainFade *f, const TerrainClipmap *c, int target, int limit, const char *what) {
    const double dt = 1.0 / 60.0, fastest = (double)TERRAIN_CLIP_LEVELS / TERRAIN_FADE_TIME * dt + 1e-9;
    int frames = 0, bad = 0;
    double was = fade_surface(f);
    while ((f->shown != target || f->level >= 0) && frames < limit) {
        terrain_fade_step(f, c, target, dt);
        frames++;
        double now = fade_surface(f), step = now - was;
        if (fabs(step) > fastest || (target < was ? step > 1e-9 : step < -1e-9) || f->shown < c->finest) bad++;
        was = now;
    }
    CHECK(bad == 0, "%s: the drawn surface jumped, turned back or went finer than ready on %d frames", what, bad);
    CHECK(f->shown == target && f->level < 0, "%s: after %d frames level %d is drawn (fading %d), not %d", what, frames, f->shown, f->level, target);
    return frames;
}

/* 12. Level transitions: the drawn level follows the selection a level at a
 *     time, each blend starting and ending on the surface drawn before and
 *     after it, faster the further behind it is; a blend the selection
 *     reverses turns round where it is; and a jump goes straight to it. */
static void test_fade(void) {
    DVec2 c0 = plan_town_centre(0, 0);
    TerrainClipmap clip;
    terrain_clip_init(&clip);
    fill_all(&clip, c0.x + SSE_CAMS[2][0], c0.z + SSE_CAMS[2][1]);
    TerrainFade f;
    terrain_fade_init(&f);
    terrain_fade_step(&f, &clip, TERRAIN_CLIP_LEVELS - 1, 0.0);
    CHECK(f.shown == TERRAIN_CLIP_LEVELS - 1 && f.level < 0, "the first step draws level %d (fading %d)", f.shown, f.level);

    /* A fast descent: the selection drops from the coarsest level to level 1. */
    int down = fade_to(&f, &clip, 1, 600, "descending");
    int up = fade_to(&f, &clip, TERRAIN_CLIP_LEVELS - 1, 600, "climbing");
    double bound = 0.0;
    for (int b = 1; b < TERRAIN_CLIP_LEVELS - 1; ++b) bound += TERRAIN_FADE_TIME / b;
    CHECK(down <= (int)(bound * 60.0) + TERRAIN_CLIP_LEVELS, "descending 7 levels took %d frames", down);

    /* Turning round: half way into level 7, the selection wants 8 again. */
    for (int k = 0; k < 9; ++k) terrain_fade_step(&f, &clip, TERRAIN_CLIP_LEVELS - 2, 1.0 / 60.0);
    double before = fade_surface(&f);
    terrain_fade_step(&f, &clip, TERRAIN_CLIP_LEVELS - 1, 0.0);
    CHECK(fabs(fade_surface(&f) - before) < 1e-9, "a reversed blend jumped from %.3f to %.3f", before, fade_surface(&f));
    int back = fade_to(&f, &clip, TERRAIN_CLIP_LEVELS - 1, 600, "turning round");

    /* A jump: the levels drawn are no longer ready. */
    fade_to(&f, &clip, 2, 600, "descending again");
    clip.finest = 5;
    terrain_fade_step(&f, &clip, 5, 1.0 / 60.0);
    CHECK(f.shown == 5 && f.level < 0, "after a jump level %d is drawn (fading %d), not the ready 5", f.shown, f.level);
    printf("level transitions (%.1f s a level): 8 to 1 in %d frames, back in %d, a reversal half way in %d\n",
           TERRAIN_FADE_TIME, down, up, back);
    terrain_clip_free(&clip);
}

/* terrain_test --map out.ppm [km]: the land round the city centre, `km` across
 * (60 by default), as a relief map 800 pixels square: lit from the north-west,
 * tinted by the leading landform (plains green, hills ochre, mountains grey,
 * plateaus red), canyons blue, the city dark. North (-z) is up. */
static int map(const char *path, double km) {
    const int W = 800;
    DVec2 c = plan_town_centre(0, 0);
    double px = km * 1000.0 / W;
    uint8_t *img = (uint8_t *)malloc((size_t)W * W * 3);
    double *row = (double *)malloc(sizeof(double) * (W + 1) * 2);
    if (!img || !row) return 1;
    static const float tint[TERRAIN_TYPES][3] = { { 0.55f, 0.75f, 0.45f }, { 0.85f, 0.72f, 0.45f }, { 0.75f, 0.75f, 0.8f }, { 0.9f, 0.5f, 0.4f } };
    double lo = 1e9, hi = -1e9;
    for (int j = 0; j < W; ++j) {
        for (int k = 0; k <= W; ++k) {
            double x = c.x + (k - W / 2) * px;
            row[k] = terrain_height(x, c.z + (j - W / 2) * px);
            row[W + 1 + k] = terrain_height(x, c.z + (j + 1 - W / 2) * px);
        }
        for (int i = 0; i < W; ++i) {
            double x = c.x + (i - W / 2) * px, z = c.z + (j - W / 2) * px, h = row[i];
            double gx = (row[i + 1] - h) / px, gz = (row[W + 1 + i] - h) / px;
            float shade = clampf(0.75f + (float)(0.7 * (gx + gz) / sqrt(1.0 + gx * gx + gz * gz)) * -1.0f, 0.15f, 1.3f);
            TerrainLandform lf;
            terrain_landform(x, z, &lf);
            float col[3] = { 0.0f, 0.0f, 0.0f };
            for (int t = 0; t < TERRAIN_TYPES; ++t)
                for (int q = 0; q < 3; ++q) col[q] += lf.w[t] * tint[t][q];
            float e = (float)plan_city_edge(x, z), city = e < 0.0f ? 1.0f : 0.0f;
            float wet = clampf(lf.canyon / 100.0f, 0.0f, 1.0f) * smoothstepf((float)TERRAIN_CANYON_FROM, (float)TERRAIN_CANYON_FULL, e);
            float hgt = 0.7f + 0.6f * clampf((float)(h / (TERRAIN_LAND_TOP_M - TERRAIN_MACRO_M)), -0.5f, 1.0f);
            for (int q = 0; q < 3; ++q) {
                float v = col[q] * shade * hgt;
                v = v * (1.0f - wet) + (q == 2 ? 0.8f : 0.3f) * shade * wet;
                v = v * (1.0f - 0.7f * city);
                img[((size_t)j * W + i) * 3 + q] = (uint8_t)clampf(v * 255.0f, 0.0f, 255.0f);
            }
            lo = fmin(lo, h); hi = fmax(hi, h);
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", W, W);
    fwrite(img, 1, (size_t)W * W * 3, f);
    fclose(f);
    printf("map of %.0f km round the centre: heights %.0f .. %.0f m\n", km, lo, hi);
    free(img);
    free(row);
    return 0;
}

/* terrain_test --planet-map out.ppm [px]: the whole planet as six faces of
 * px a side (3 across, 2 down: faces 0-2 then 3-5), each pixel the ground
 * at the pixel's spacing, shaded from the north-west; the city in red. */
static int planet_map(const char *path, int F) {
    const int W = 3 * F, H = 2 * F;
    double sp = 0.5 * 3.14159265358979323846 / F * PLANET_RADIUS, lo = 1e9, hi = -1e9;
    uint8_t *img = (uint8_t *)calloc((size_t)W * H * 3, 1);
    double *hgt = (double *)malloc(sizeof(double) * (size_t)(F + 1) * (F + 1));
    if (!img || !hgt) return 1;
    double cu, cv;
    int cf = terrain_dir_face(terrain_chart_dir(0.0, 0.0), &cu, &cv);
    for (int f = 0; f < 6; ++f) {
        for (int j = 0; j <= F; ++j)
            for (int i = 0; i <= F; ++i) {
                double h = terrain_planet_height_lod(terrain_face_dir(f, -1.0 + 2.0 * (i + 0.5) / F, -1.0 + 2.0 * (j + 0.5) / F), sp);
                hgt[j * (F + 1) + i] = h;
                lo = fmin(lo, h); hi = fmax(hi, h);
            }
        for (int j = 0; j < F; ++j)
            for (int i = 0; i < F; ++i) {
                double h = hgt[j * (F + 1) + i], gx = (hgt[j * (F + 1) + i + 1] - h) / sp, gz = (hgt[(j + 1) * (F + 1) + i] - h) / sp;
                float shade = clampf(1.0f - (float)(40.0 * (gx + gz)), 0.4f, 1.4f);
                float t = clampf((float)((h + 1500.0) / 4000.0), 0.0f, 1.0f);
                float col[3] = { 0.35f + 0.5f * t, 0.5f + 0.25f * t - 0.2f * t * t, 0.3f + 0.3f * t * t };
                if (f == cf && fabs(-1.0 + 2.0 * (i + 0.5) / F - cu) < 3.0 / F && fabs(-1.0 + 2.0 * (j + 0.5) / F - cv) < 3.0 / F) {
                    col[0] = 1.0f; col[1] = col[2] = 0.1f; shade = 1.0f;
                }
                size_t px = ((size_t)((f / 3) * F + (F - 1 - j)) * W + (size_t)((f % 3) * F + i)) * 3;
                for (int q = 0; q < 3; ++q) img[px + q] = (uint8_t)clampf(col[q] * shade * 255.0f, 0.0f, 255.0f);
            }
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) return 1;
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, (size_t)W * H * 3, fp);
    fclose(fp);
    printf("planet map, %d px a face (%.0f km a pixel): heights %.0f .. %.0f m; the city on face %d\n", F, sp / 1000.0, lo, hi, cf);
    free(img);
    free(hgt);
    return 0;
}

static int fingerprint(void) {
    DVec2 c = plan_town_centre(0, 0);
    uint64_t hh = 0xcbf29ce484222325ULL, he = hh;
    int n = 0;
    for (int j = -100; j <= 100; ++j)
        for (int i = -100; i <= 100; ++i, ++n) {
            double x = c.x + i * 250.0, z = c.z + j * 250.0;
            double h = terrain_height(x, z), e = plan_city_edge(x, z);
            hh = fnv(&h, sizeof(h), hh);
            he = fnv(&e, sizeof(e), he);
        }
    for (int i = 0; i < N_POINTS; ++i, ++n) {
        double h = terrain_height(g_x[i], g_z[i]), e = plan_city_edge(g_x[i], g_z[i]);
        hh = fnv(&h, sizeof(h), hh);
        he = fnv(&e, sizeof(e), he);
    }
    /* Clipmaps filled at the centre, the edge and out on the land. */
    uint64_t hc = 0xcbf29ce484222325ULL;
    const double cams[3][2] = { { 0.0, 0.0 }, { 9300.0, -2100.0 }, { -21000.0, 14500.0 } };
    for (int k = 0; k < 3; ++k) {
        TerrainClipmap clip;
        terrain_clip_init(&clip);
        fill_all(&clip, c.x + cams[k][0], c.z + cams[k][1]);
        hc = hash_clipmap(&clip, hc);
        terrain_clip_free(&clip);
    }
    printf("fingerprint over %d points and 3 clipmaps\n", n);
    printf("  height     %016llx\n", (unsigned long long)hh);
    printf("  edge       %016llx\n", (unsigned long long)he);
    printf("  clipmap    %016llx\n", (unsigned long long)hc);
    return 0;
}

/* --- The planet --------------------------------------------------------------- */

static double pdot(TerrainDir a, TerrainDir b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static TerrainDir psub(TerrainDir a, TerrainDir b) { TerrainDir r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static double pdist(TerrainDir a, TerrainDir b) { TerrainDir d = psub(a, b); return sqrt(pdot(d, d)); }
static int psame(TerrainDir a, TerrainDir b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
static int pcmp(const void *pa, const void *pb) {
    const TerrainDir *a = (const TerrainDir *)pa, *b = (const TerrainDir *)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    return a->z < b->z ? -1 : a->z > b->z;
}
static TerrainDir prand(Rng *r) {   /* uniform on the sphere */
    float u = rng_float(r);
    float v = rng_float(r);
    double z = 2.0 * u - 1.0, a = 2.0 * 3.14159265358979323846 * v, s = sqrt(fmax(0.0, 1.0 - z * z));
    TerrainDir p = { s * cos(a), s * sin(a), z };
    return p;
}
static int same_patch(TerrainPatch a, TerrainPatch b) { return a.face == b.face && a.level == b.level && a.i == b.i && a.j == b.j; }

/* Whether the segment from a to b passes through the ball of radius rad round the origin. */
static int segment_blocked(TerrainDir a, TerrainDir b, double rad) {
    TerrainDir d = psub(b, a);
    double t = -pdot(a, d) / pdot(d, d);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    TerrainDir q = { a.x + t * d.x, a.y + t * d.y, a.z + t * d.z };
    return pdot(q, q) < rad * rad;
}

/* 9. The planet: the chart is a true azimuthal-equidistant projection that
 *    round-trips; the cube-sphere round-trips, its patches nest and meet across
 *    every seam bit for bit, and their neighbours are mutual; the ground on the
 *    planet is the chart's round the city and continuous out to the antipode;
 *    the bend matches the exact geometry; horizon culling only hides what is
 *    hidden. */
static void test_planet(Rng *r) {
    const double R = PLANET_RADIUS, PI_D = 3.14159265358979323846;
    TerrainDir o = terrain_chart_dir(0.0, 0.0);

    /* The chart: distance from the origin is arc length, and back again. */
    double chartErr = 0.0, arcErr = 0.0;
    for (int k = 0; k < 20000; ++k) {
        float u = rng_float(r);
        float v = rng_float(r);
        double s = (k % 4 == 0 ? 0.999 * PI_D * R : 2.0e5) * u, a = 2.0 * PI_D * v;
        double x = s * cos(a), z = s * sin(a), bx, bz;
        TerrainDir p = terrain_chart_dir(x, z);
        terrain_dir_chart(p, &bx, &bz);
        double tol = 1e-9 * s + 1e-5;
        chartErr = fmax(chartErr, hypot(bx - x, bz - z) / fmax(1.0, s));
        CHECK(fabs(pdot(p, p) - 1.0) < 1e-14, "chart direction at (%.0f, %.0f) isn't unit", x, z);
        CHECK(hypot(bx - x, bz - z) <= tol, "chart at (%.1f, %.1f) comes back as (%.6f, %.6f)", x, z, bx, bz);
        double arc = acos(fmin(1.0, pdot(p, o))) * R;
        if (s > 1000.0) arcErr = fmax(arcErr, fabs(arc - s) / s);
    }
    printf("planet: chart round trip within %.1e of the distance, arc length within %.1e\n", chartErr, arcErr);
    CHECK(arcErr < 1e-7, "chart distance isn't arc length (%.2e)", arcErr);

    /* The chart's local frame is orthonormal, right-handed, and up is the direction. */
    for (int k = 0; k < 200; ++k) {
        float u = rng_float(r);
        float v = rng_float(r);
        double s = 3.0e6 * u, a = 2.0 * PI_D * v, x = s * cos(a), z = s * sin(a);
        TerrainDir ax, up, az, cr;
        terrain_chart_frame(x, z, &ax, &up, &az);
        cr.x = ax.y * up.z - ax.z * up.y; cr.y = ax.z * up.x - ax.x * up.z; cr.z = ax.x * up.y - ax.y * up.x;
        CHECK(fabs(pdot(ax, up)) < 1e-12 && fabs(pdot(az, up)) < 1e-12 && fabs(pdot(ax, az)) < 1e-12 && pdist(cr, az) < 1e-12 &&
              pdist(up, terrain_chart_dir(x, z)) < 1e-15, "chart frame at (%.0f, %.0f) isn't orthonormal", x, z);
        /* x along the chart's x: a metre's step there moves the direction along ax */
        TerrainDir d = psub(terrain_chart_dir(x + 1.0, z), terrain_chart_dir(x - 1.0, z));
        double along = pdot(d, ax) / sqrt(pdot(d, d));
        CHECK(along > 1.0 - 1e-6, "chart frame's x at (%.0f, %.0f) isn't the chart's x (cos %.9f)", x, z, along);
    }

    /* The planet frame is geocentric and right-handed: +x through (0, 0), +y
     * through (0, 90 E), +z through the north pole; latitude and longitude
     * round-trip; and the chart's x and z at its origin are east and south. */
    {
        TerrainDir ex = { 1.0, 0.0, 0.0 }, ey = { 0.0, 1.0, 0.0 }, ez = { 0.0, 0.0, 1.0 };
        TerrainDir px = terrain_latlon_dir(0.0, 0.0), py = terrain_latlon_dir(0.0, 90.0), pz = terrain_latlon_dir(90.0, 0.0);
        CHECK(pdist(px, ex) < 1e-15 && pdist(py, ey) < 1e-15 && pdist(pz, ez) < 1e-15,
              "the planet frame's axes aren't (0, 0), (0, 90 E) and the north pole");
        TerrainDir xy = { px.y * py.z - px.z * py.y, px.z * py.x - px.x * py.z, px.x * py.y - px.y * py.x };
        CHECK(pdist(xy, pz) < 1e-15, "the planet frame isn't right-handed");
        double lat, lon;
        terrain_dir_latlon(ez, &lat, &lon);
        CHECK(fabs(lat - 90.0) < 1e-12 && lon == 0.0, "the north pole isn't at latitude 90, longitude 0 (%.9f, %.9f)", lat, lon);
        for (int k = 0; k < 20000; ++k) {
            TerrainDir p = prand(r);
            terrain_dir_latlon(p, &lat, &lon);
            TerrainDir q = terrain_latlon_dir(lat, lon);
            CHECK(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0 && pdist(p, q) < 1e-14,
                  "latitude and longitude round trip: (%.6f, %.6f) is %.2e away", lat, lon, pdist(p, q));
        }
        TerrainDir o = terrain_chart_dir(0.0, 0.0), ax, up, az;
        terrain_chart_frame(0.0, 0.0, &ax, &up, &az);
        terrain_dir_latlon(o, &lat, &lon);
        TerrainDir east = psub(terrain_latlon_dir(lat, lon + 1e-5), terrain_latlon_dir(lat, lon - 1e-5));
        TerrainDir north = psub(terrain_latlon_dir(lat + 1e-5, lon), terrain_latlon_dir(lat - 1e-5, lon));
        CHECK(pdot(east, ax) / sqrt(pdot(east, east)) > 1.0 - 1e-8 && pdot(north, az) / sqrt(pdot(north, north)) < -(1.0 - 1e-8),
              "the chart's x and z at its origin (%.4f, %.4f) aren't east and south", lat, lon);
    }

    /* The cube-sphere round-trips. */
    for (int k = 0; k < 20000; ++k) {
        TerrainDir p = prand(r);
        double u, v;
        int f = terrain_dir_face(p, &u, &v);
        TerrainDir q = terrain_face_dir(f, u, v);
        CHECK(f >= 0 && f < 6 && fabs(u) <= 1.0 && fabs(v) <= 1.0 && pdist(p, q) < 1e-14, "face round trip: face %d (%.6f, %.6f) is %.2e away", f, u, v, pdist(p, q));
        TerrainPatch t = terrain_patch_at(p, 12);
        CHECK(t.face == f, "patch_at put a direction on face %d, not %d", t.face, f);
    }

    /* Patches nest: a vertex of a patch is the same direction through its children. */
    const int G = 8;
    for (int k = 0; k < 2000; ++k) {
        TerrainPatch t = terrain_patch_at(prand(r), k % 10);
        int gi = rng_int(r, 0, G), gj = rng_int(r, 0, G), ck = (gi * 2 > G) + 2 * (gj * 2 > G);
        TerrainPatch c = terrain_patch_child(t, ck);
        TerrainDir a = terrain_patch_dir(t, G, gi, gj), b = terrain_patch_dir(c, G, 2 * gi - (ck & 1) * G, 2 * gj - (ck >> 1) * G);
        CHECK(psame(a, b), "vertex (%d, %d) of patch %d/%d/%lld,%lld differs through its child", gi, gj, t.face, t.level,
              (long long)t.i, (long long)t.j);
        CHECK(same_patch(terrain_patch_parent(c), t), "a child's parent isn't the patch");
        /* the same grid line at another cell count */
        CHECK(psame(a, terrain_patch_dir(t, 2 * G, 2 * gi, 2 * gj)), "vertex of patch differs at twice the cells");
    }

    /* Seams: every edge vertex of every patch at a level is the same direction,
     * bit for bit, as the vertex of the patch across the edge; nothing lands
     * nearly, but not exactly, on another. */
    for (int level = 0; level <= 3; ++level) {
        int per = 1 << level, n = 6 * per * per * 4 * (G + 1), m = 0;
        TerrainDir *pts = (TerrainDir *)malloc(sizeof(TerrainDir) * (size_t)n);
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j < per; ++j)
                for (int i = 0; i < per; ++i) {
                    TerrainPatch t = { f, level, i, j };
                    for (int g = 0; g <= G; ++g) {
                        pts[m++] = terrain_patch_dir(t, G, g, 0);
                        pts[m++] = terrain_patch_dir(t, G, g, G);
                        pts[m++] = terrain_patch_dir(t, G, 0, g);
                        pts[m++] = terrain_patch_dir(t, G, G, g);
                    }
                }
        qsort(pts, (size_t)m, sizeof(TerrainDir), pcmp);
        int alone = 0, near = 0;
        for (int a = 0; a < m;) {
            int b = a + 1;
            while (b < m && psame(pts[a], pts[b])) b++;
            if (b - a < 2) alone++;
            if (b < m && pdist(pts[a], pts[b]) < 1e-9) near++;
            a = b;
        }
        CHECK(alone == 0 && near == 0, "level %d seams: %d edge vertices with no twin, %d near misses", level, alone, near);
        free(pts);
    }

    /* Neighbours: same level, never the patch itself, and mutual. */
    for (int k = 0; k < 4000; ++k) {
        TerrainPatch t = terrain_patch_at(prand(r), k % 12);
        for (int side = 0; side < 4; ++side) {
            TerrainPatch nb = terrain_patch_neighbour(t, side);
            int back = 0;
            for (int s2 = 0; s2 < 4; ++s2) back |= same_patch(terrain_patch_neighbour(nb, s2), t);
            CHECK(nb.level == t.level && !same_patch(nb, t) && back, "patch %d/%d/%lld,%lld side %d: neighbour %d/%lld,%lld isn't mutual",
                  t.face, t.level, (long long)t.i, (long long)t.j, side, nb.face, (long long)nb.i, (long long)nb.j);
        }
    }

    /* The ground on the planet: the chart's round the city, bit for bit, and
     * continuous everywhere, through the blend and at the antipode. */
    const double spacings[3] = { 0.0, 64.0, 4096.0 };
    double lowest = terrain_planet_lowest(), seen = 1e9, jump = 0.0;
    for (int k = 0; k < 6000; ++k) {
        TerrainDir p = prand(r);
        double sp = spacings[k % 3], x, z;
        if (k % 5 == 0) {   /* near the city */
            float u = rng_float(r);
            float v = rng_float(r);
            p = terrain_chart_dir((2.0 * u - 1.0) * 40000.0, (2.0 * v - 1.0) * 40000.0);
        }
        terrain_dir_chart(p, &x, &z);
        double h = terrain_planet_height_lod(p, sp), sf = terrain_planet_surface_lod(p, sp);
        if (acos(fmin(1.0, pdot(p, o))) <= 0.4 * PI_D) {
            double hc = terrain_height_lod(x, z, sp), sc = terrain_surface_lod(x, z, sp, NULL);
            CHECK(memcmp(&h, &hc, sizeof(h)) == 0 && memcmp(&sf, &sc, sizeof(sf)) == 0, "planet ground at chart (%.0f, %.0f) isn't the chart's", x, z);
        }
        CHECK(isfinite(h) && h >= lowest && sf >= lowest, "planet ground %.1f (surface %.1f) under the lowest, %.1f", h, sf, lowest);
        seen = fmin(seen, fmin(h, sf));
        /* a step of 2 m over the sphere moves it by no more than the land's steepest */
        TerrainDir ax, up, az;
        terrain_chart_frame(x, z, &ax, &up, &az);
        (void)up;
        TerrainDir q = { p.x + ax.x * 2.0 / R, p.y + ax.y * 2.0 / R, p.z + ax.z * 2.0 / R };
        double len = sqrt(pdot(q, q));
        q.x /= len; q.y /= len; q.z /= len;
        jump = fmax(jump, fabs(terrain_planet_height_lod(q, sp) - h) / 2.0);
    }
    for (int k = 0; k < 2000; ++k) {   /* round the antipode, where the chart's polar */
        float u = rng_float(r);
        float v = rng_float(r);
        double s = PI_D * R - 50.0 * u, a = 2.0 * PI_D * v, b = a + 0.5 * PI_D;
        TerrainDir p = terrain_chart_dir(s * cos(a), s * sin(a)), q = terrain_chart_dir(s * cos(b), s * sin(b));
        double d = pdist(p, q) * R;
        if (d > 1e-3) jump = fmax(jump, fabs(terrain_planet_height_lod(p, 0.0) - terrain_planet_height_lod(q, 0.0)) / d);
    }
    printf("planet ground: lowest %.1f m (bound %.1f), steepest step %.2f\n", seen, lowest, jump);
    CHECK(jump <= LAND_SLOPE, "planet ground jumps: slope %.2f", jump);

    /* The bend against the exact geometry: exact from the chart's origin, and
     * from a camera off it, the chart's distortion. */
    const double offs[4] = { 0.0, 10000.0, 50000.0, 200000.0 };
    for (int oc = 0; oc < 4; ++oc) {
        double cx = offs[oc] * 0.6, cz = -offs[oc] * 0.8, worst = 0.0, worstNear = 0.0;
        for (int k = 0; k < 4000; ++k) {
            float u = rng_float(r);
            float v = rng_float(r);
            float w = rng_float(r);
            float y = rng_float(r);
            double s = (k % 2 ? 32000.0 : 500000.0) * u, a = 2.0 * PI_D * v, h = -500.0 + 3000.0 * w, cy = 1.7 + 20000.0 * y;
            double dx = s * cos(a), dz = s * sin(a);
            TerrainDir ax, up, az;
            terrain_chart_frame(cx, cz, &ax, &up, &az);
            TerrainDir rel = psub(terrain_chart_point(cx + dx, h, cz + dz), terrain_chart_point(cx, cy, cz));
            float b[3];
            terrain_bend((float)dx, (float)dz, (float)h, (float)cy, b);
            double e = sqrt((b[0] - pdot(rel, ax)) * (b[0] - pdot(rel, ax)) + (b[1] - pdot(rel, up)) * (b[1] - pdot(rel, up)) +
                            (b[2] - pdot(rel, az)) * (b[2] - pdot(rel, az)));
            if (s <= 32000.0) worstNear = fmax(worstNear, e);
            else worst = fmax(worst, e / s);
        }
        printf("bend from %3.0f km off the origin: within %.4f m to 32 km, %.1e of the distance to 500 km\n", offs[oc] / 1000.0, worstNear, worst);
        /* Measured: 6 mm, 9 cm, 0.7 m and 7.4 m to 32 km; at 32 km a pixel is about 40 m. */
        const double nearBound[4] = { 0.01, 0.15, 1.0, 10.0 };
        CHECK(worstNear < nearBound[oc], "bend from %.0f km off the chart's origin strays %.4f m", offs[oc] / 1000.0, worstNear);
        if (oc == 0) CHECK(worst < 1e-6, "bend from the chart's origin strays %.2e of the distance far off", worst);
    }

    /* The bent image of a box lies inside its widened bounds. */
    for (int k = 0; k < 4000; ++k) {
        float u = rng_float(r);
        float v = rng_float(r);
        float w = rng_float(r);
        float y = rng_float(r);
        double s = 40000.0 * u, a = 2.0 * PI_D * v, cy = 1.7 + 20000.0 * y;
        float mn[3] = { (float)(s * cos(a)), (float)(-600.0 + 3000.0 * w - cy), (float)(s * sin(a)) }, mx[3];
        float ext = (float)(1.0 + 4000.0 * u * w);
        mx[0] = mn[0] + ext; mx[1] = mn[1] + 0.3f * ext; mx[2] = mn[2] + 0.7f * ext;
        float bmn[3] = { mn[0], mn[1], mn[2] }, bmx[3] = { mx[0], mx[1], mx[2] };
        terrain_bend_bounds(bmn, bmx);
        for (int q = 0; q < 16; ++q) {
            float fx = rng_float(r);
            float fy = rng_float(r);
            float fz = rng_float(r);
            float px = mn[0] + fx * (mx[0] - mn[0]), py = mn[1] + fy * (mx[1] - mn[1]), pz = mn[2] + fz * (mx[2] - mn[2]), b[3];
            if (q < 8) { px = q & 1 ? mx[0] : mn[0]; py = q & 2 ? mx[1] : mn[1]; pz = q & 4 ? mx[2] : mn[2]; }
            terrain_bend(px, pz, py + (float)cy, (float)cy, b);
            CHECK(b[0] >= bmn[0] && b[0] <= bmx[0] && b[1] >= bmn[1] && b[1] <= bmx[1] && b[2] >= bmn[2] && b[2] <= bmx[2],
                  "bent point (%.1f, %.1f, %.1f) outside its box's bent bounds", b[0], b[1], b[2]);
        }
    }

    /* Horizon culling hides only what's behind the planet, and does hide the
     * far side of it. */
    double occ = R + lowest;
    int hidden = 0, farHidden = 0, farN = 0;
    for (int k = 0; k < 20000; ++k) {
        float u = rng_float(r);
        float v = rng_float(r);
        float w = rng_float(r);
        float y = rng_float(r);
        float q = rng_float(r);
        double cs = 300000.0 * u, ca = 2.0 * PI_D * v, alt = 2.0 + 20000.0 * w * w;
        TerrainDir cam = terrain_chart_point(cs * cos(ca), alt, cs * sin(ca));
        double bs = cs + 2.0e6 * y, ba = ca + (q - 0.5), rad = 10.0 + 50000.0 * u * w;
        float hh = rng_float(r);
        TerrainDir centre = terrain_chart_point(bs * cos(ba), -500.0 + 4000.0 * hh, bs * sin(ba));
        int hid = terrain_horizon_hidden(cam, centre, rad, occ);
        if (hid) {
            hidden++;
            for (int s = 0; s < 32; ++s) {   /* the centre, and points on its surface */
                TerrainDir d = s ? prand(r) : (TerrainDir){ 0.0, 0.0, 0.0 };
                TerrainDir pt = { centre.x + d.x * rad, centre.y + d.y * rad, centre.z + d.z * rad };
                CHECK(segment_blocked(cam, pt, occ), "a ball reported hidden can be seen (camera %.0f m up, ball %.0f km off)", alt, pdist(cam, centre) / 1000.0);
            }
        }
        /* a small ball on the ground well past the horizon must be hidden */
        double horizon = sqrt(2.0 * R * (alt - lowest)) + sqrt(2.0 * R * (4000.0 - lowest));
        if (rad < 200.0 && pdist(cam, centre) > 1.5 * horizon) { farN++; farHidden += hid; }
    }
    printf("horizon: %d of 20000 balls hidden, %d of %d well past the horizon\n", hidden, farHidden, farN);
    CHECK(farN > 0 && farHidden == farN, "horizon culling missed %d of %d balls well past the horizon", farN - farHidden, farN);
}

/* --- Planet patches ----------------------------------------------------------------- */

static TerrainDir vabs(const TerrainPatchMesh *m, int k, int morphed) {
    const TerrainPatchVertex *v = &m->v[k];
    TerrainDir p = { m->info.centre.x + v->pos[0], m->info.centre.y + v->pos[1], m->info.centre.z + v->pos[2] };
    if (morphed) { p.x += v->morph[0]; p.y += v->morph[1]; p.z += v->morph[2]; }
    return p;
}

/* A view for the selection: a camera at world (x, y, z) looking along yaw and
 * pitch (degrees, as the player's), 1600 x 900 at 72 degrees, far plane far
 * enough for the horizon. */
static TerrainPlanetView planet_view(double x, double y, double z, float yawDeg, float pitchDeg) {
    TerrainPlanetView v;
    memset(&v, 0, sizeof(v));
    v.cam[0] = x; v.cam[1] = y; v.cam[2] = z;
    float yaw = yawDeg * PI_F / 180.0f, pitch = pitchDeg * PI_F / 180.0f, fovy = 72.0f * PI_F / 180.0f;
    vec3 fwd = v3(cosf(pitch) * sinf(yaw), sinf(pitch), -cosf(pitch) * cosf(yaw));
    vec3 right = v3_norm(v3_cross(fwd, v3(0.0f, 1.0f, 0.0f)));
    vec3 up = v3_cross(right, fwd);
    mat4 vp = mat4_mul(mat4_perspective_rz(fovy, 16.0f / 9.0f, 0.08f, 1.0e6f), mat4_view_rot(right, up, fwd));
    v.frustum = frustum_from_matrix(vp);
    v.K = 900.0 / (2.0 * tan(0.5 * fovy));
    return v;
}

/* Selects and fills until nothing more is wanted (or `rounds` run out), as
 * the renderer would over frames. Returns the rounds taken; *fills gets the
 * patches filled. */
static int settle(TerrainPlanet *tp, const TerrainPlanetView *v, TerrainPatchMesh *scratch, int rounds, int *fills) {
    int k = 0;
    for (; k < rounds; ++k) {
        terrain_planet_select(tp, v);
        if (tp->nwant == 0) break;
        for (int w = 0; w < tp->nwant; ++w) {
            terrain_patch_fill(tp->want[w], scratch);
            if (terrain_planet_insert(tp, &scratch->info) >= 0 && fills) (*fills)++;
        }
    }
    return k;
}

static uint64_t hash_draws(const TerrainPlanet *tp) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int k = 0; k < tp->ndraw; ++k) {
        TerrainPatch id = tp->slot[tp->draw[k].slot].info.id;
        h = fnv(&id, sizeof(id), h);
    }
    return h;
}

/* 10. The planet's patches: meshes are pure functions of the patch, meet
 *     their neighbours across every seam, morph onto their parent exactly, and
 *     their err bounds where their children's vertices leave them; the
 *     selection is bounded, steady, never draws a patch with its ancestor, and
 *     refines where it matters. */
static void test_planet_patches(Rng *r) {
    TerrainPatchMesh *a = (TerrainPatchMesh *)malloc(sizeof(TerrainPatchMesh) * 8);
    TerrainPatchMesh *top = (TerrainPatchMesh *)malloc(sizeof(TerrainPatchMesh) * 6);
    TerrainPlanet *tp = (TerrainPlanet *)malloc(sizeof(TerrainPlanet));
    if (!a || !top || !tp) { CHECK(0, "out of memory"); return; }
    const int PSIDE = TERRAIN_PATCH_SIDE, PCELL = TERRAIN_PATCH_CELLS;

    /* Patches near the city and elsewhere, some across a face seam. */
    TerrainPatch picks[12];
    for (int k = 0; k < 12; ++k) {
        TerrainDir p = k < 6 ? terrain_chart_dir((k - 3) * 30000.0, (k % 3 - 1) * 40000.0) : prand(r);
        picks[k] = terrain_patch_at(p, 4 + k % 7);
    }
    picks[11] = terrain_patch_at(terrain_face_dir(0, 0.3, 0.99999), 9);   /* on face 0 against face 4 */
    double seamWorst = 0.0, morphWorst = 0.0, errSlack = 1e30;
    clock_t t0 = clock();
    int evals = 0, nfill = 0;
    for (int k = 0; k < 12; ++k) {
        TerrainPatch t = picks[k];
        evals += terrain_patch_fill(t, &a[0]);
        terrain_patch_fill(t, &a[1]);
        nfill++;
        /* (the info and the vertices; the struct's tail padding is never written) */
        CHECK(memcmp(&a[0].info, &a[1].info, sizeof(TerrainPatchInfo)) == 0 && memcmp(a[0].v, a[1].v, sizeof(a[0].v)) == 0, "patch %d/%d/%lld,%lld fills differently twice", t.face, t.level, (long long)t.i, (long long)t.j);
        CHECK(isfinite(a[0].info.err) && a[0].info.err >= 0.0f && a[0].info.radius > 0.0f, "patch err %.3f radius %.1f", a[0].info.err, a[0].info.radius);
        /* every vertex within the radius */
        for (int v = 0; v < TERRAIN_PATCH_VERTS; ++v) {
            TerrainDir q = vabs(&a[0], v, 0);
            CHECK(pdist(q, a[0].info.centre) <= a[0].info.radius, "a vertex lies outside its patch's radius");
        }
        /* seams: each edge vertex is a vertex of the neighbour across it */
        for (int side = 0; side < 4; ++side) {
            TerrainPatch nb = terrain_patch_neighbour(t, side);
            terrain_patch_fill(nb, &a[2]);
            for (int e = 0; e < PSIDE; ++e) {
                int gi = side < 2 ? (side == 0 ? 0 : PCELL) : e, gj = side < 2 ? e : (side == 2 ? 0 : PCELL);
                TerrainDir q = vabs(&a[0], gj * PSIDE + gi, 0);
                double best = 1e30;
                for (int s2 = 0; s2 < 4; ++s2)
                    for (int e2 = 0; e2 < PSIDE; ++e2) {
                        int hi = s2 < 2 ? (s2 == 0 ? 0 : PCELL) : e2, hj = s2 < 2 ? e2 : (s2 == 2 ? 0 : PCELL);
                        best = fmin(best, pdist(q, vabs(&a[2], hj * PSIDE + hi, 0)));
                    }
                seamWorst = fmax(seamWorst, best / a[0].info.radius);
            }
        }
        /* children: at full morph each is its parent's surface; and its vertices
         * at the parent's cell centres are within the parent's err */
        for (int c = 0; c < 4; ++c) {
            TerrainPatchMesh *ch = &a[3 + c];
            terrain_patch_fill(terrain_patch_child(t, c), ch);
            int oi = (c & 1) * PCELL / 2, oj = (c >> 1) * PCELL / 2;
            for (int gj = 0; gj <= PCELL; ++gj)
                for (int gi = 0; gi <= PCELL; ++gi) {
                    int pi = oi + gi / 2, pj = oj + gj / 2, di = gi & 1, dj = gj & 1;
                    TerrainDir pa = vabs(&a[0], pj * PSIDE + pi, 0), pb = vabs(&a[0], (pj + dj) * PSIDE + pi + di, 0);
                    TerrainDir want = { 0.5 * (pa.x + pb.x), 0.5 * (pa.y + pb.y), 0.5 * (pa.z + pb.z) };
                    TerrainDir got = vabs(ch, gj * PSIDE + gi, 1);
                    morphWorst = fmax(morphWorst, pdist(got, want) / a[0].info.radius);
                    if (di && dj) errSlack = fmin(errSlack, a[0].info.err - pdist(vabs(ch, gj * PSIDE + gi, 0), want));
                }
        }
    }
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / (nfill * 26);
    printf("planet patches: %d heights a fill, about %.2f ms; seams within %.1e of the radius, morph %.1e; err's least slack %.3f m\n",
           evals / 12, ms, seamWorst, morphWorst, errSlack);
    CHECK(seamWorst < 1e-6, "patch seams open by %.2e of the radius", seamWorst);
    CHECK(morphWorst < 1e-6, "a child fully morphed is %.2e of the radius off its parent's surface", morphWorst);
    CHECK(errSlack > -0.01, "a child's vertex leaves its parent's surface by more than the parent's err (%.3f m)", -errSlack);

    /* The selection, from the street, a tower, and aircraft height. */
    terrain_planet_init(tp, top);
    TerrainPlanetView v0 = planet_view(0.0, 2.0, 0.0, 90.0f, 0.0f);
    terrain_planet_select(tp, &v0);
    CHECK(tp->ndraw > 0 && tp->ndraw <= 6, "with only the top patches filled, %d are drawn", tp->ndraw);
    const struct { double x, y, z; float yaw, pitch; const char *name; double win; } views[] = {
        { 0.0, 2.0, 0.0, 90.0f, 0.0f, "street", 1 }, { 12500.0, 300.0, 0.0, 90.0f, -2.0f, "edge, 300 m", 1 },
        { 30000.0, 2000.0, -20000.0, 45.0f, -10.0f, "land, 2 km", 1 }, { 0.0, 10000.0, 0.0, 120.0f, -20.0f, "10 km", 1 },
        { 0.0, 20000.0, 0.0, 300.0f, -45.0f, "20 km, down", 1 }, { 0.0, 20000.0, 0.0, 30.0f, 0.0f, "20 km, level", 0 },
    };
    for (int k = 0; k < (int)(sizeof(views) / sizeof(views[0])); ++k) {
        TerrainPlanetView v = planet_view(views[k].x, views[k].y, views[k].z, views[k].yaw, views[k].pitch);
        if (views[k].win) {   /* a clipmap window as the renderer's: 64.5 km round the camera */
            v.window = 1;
            v.win[0] = views[k].x - 32000.0; v.win[1] = views[k].z - 32000.0;
            v.win[2] = views[k].x + 32000.0; v.win[3] = views[k].z + 32000.0;
        }
        int fills = 0;
        int rounds = settle(tp, &v, &a[0], 400, &fills);
        CHECK(tp->nwant == 0, "%s: still wanting %d patches after %d rounds", views[k].name, tp->nwant, rounds);
        CHECK(tp->ndraw < TERRAIN_PLANET_MAX_DRAWS, "%s: %d patches drawn, the most there's room for", views[k].name, tp->ndraw);
        int lv[TERRAIN_PATCH_LEVELS] = { 0 }, hole = 0, lmin = 99, lmax = 0;
        for (int d = 0; d < tp->ndraw; ++d) {
            const TerrainPlanetSlot *sl = &tp->slot[tp->draw[d].slot];
            CHECK(sl->used, "a drawn patch's slot is empty");
            int L = sl->info.id.level;
            lv[L]++; lmin = L < lmin ? L : lmin; lmax = L > lmax ? L : lmax;
            hole += tp->draw[d].hole;
            /* no ancestor drawn too */
            for (TerrainPatch p = sl->info.id; p.level > 0;) {
                p = terrain_patch_parent(p);
                for (int e = 0; e < tp->ndraw; ++e)
                    CHECK(!same_patch(tp->slot[tp->draw[e].slot].info.id, p), "%s: a patch is drawn with its ancestor", views[k].name);
            }
        }
        /* steady: the same view again draws the same, and a camera dithering
         * by half a metre each frame changes it at most once */
        uint64_t h0 = hash_draws(tp);
        terrain_planet_select(tp, &v);
        CHECK(hash_draws(tp) == h0 && tp->nwant == 0, "%s: the same view selects differently", views[k].name);
        int changes = 0;
        uint64_t prev = h0;
        for (int f = 0; f < 40; ++f) {
            TerrainPlanetView vd = v;
            vd.cam[0] += f & 1 ? 0.5 : -0.5;
            settle(tp, &vd, &a[0], 10, NULL);
            uint64_t hd = hash_draws(tp);
            changes += hd != prev;
            prev = hd;
        }
        CHECK(changes <= 2, "%s: a dithering camera changes the patches %d times", views[k].name, changes);
        printf("  %-13s %4d drawn (levels %d-%d, %d cut by the window), %d culled, %d past the horizon, %d inside; %d filled in %d rounds\n",
               views[k].name, tp->ndraw, lmin, lmax, hole, tp->culled, tp->hidden, tp->inside, fills, rounds);
    }
    free(a);
    free(top);
    free(tp);
}

int main(int argc, char **argv) {
    plan_world_init(cpu_count());
    /* Points round the city and its surroundings, plus some far away. */
    DVec2 c = plan_town_centre(0, 0);
    Rng r = rng_make(4321);
    for (int i = 0; i < N_POINTS; ++i) {
        double span = i % 10 == 0 ? 1.0e8 : REGION;
        float u = rng_float(&r);
        float v = rng_float(&r);
        g_x[i] = c.x + (2.0 * u - 1.0) * span;
        g_z[i] = c.z + (2.0 * v - 1.0) * span;
    }
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();
    if (argc > 1 && strcmp(argv[1], "--patches") == 0) { test_planet_patches(&r); return g_failures != 0; }
    if (argc > 2 && strcmp(argv[1], "--map") == 0) return map(argv[2], argc > 3 ? atof(argv[3]) : 60.0);
    if (argc > 2 && strcmp(argv[1], "--planet-map") == 0) return planet_map(argv[2], argc > 3 ? atoi(argv[3]) : 256);

    /* 1. Reference pass, in order: finite and within the amplitudes. */
    double amax = fmax(PLAN_RELIEF_MAX_M, fmax(TERRAIN_LAND_TOP_M, TERRAIN_LAND_DEEP_M)) + 1e-3, lo = 1e9, hi = -1e9;
    for (int i = 0; i < N_POINTS; ++i) {
        double h = terrain_height(g_x[i], g_z[i]);
        g_ref[i] = h;
        CHECK(isfinite(h) && fabs(h) <= amax, "height %.3f at (%.1f, %.1f) out of range", h, g_x[i], g_z[i]);
        if (h < lo) lo = h;
        if (h > hi) hi = h;
    }
    printf("heights over %d points: %.1f .. %.1f m\n", N_POINTS, lo, hi);

    /* 2. Shuffled order, interleaved with unrelated queries, must match bit for bit. */
    int order[N_POINTS];
    for (int i = 0; i < N_POINTS; ++i) order[i] = i;
    for (int i = N_POINTS - 1; i > 0; --i) {
        int j = rng_int(&r, 0, i), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (int k = 0; k < N_POINTS; ++k) {
        int i = order[k];
        terrain_height(g_x[i] + 37.0, g_z[i] - 11.0);
        double h = terrain_height(g_x[i], g_z[i]);
        CHECK(memcmp(&h, &g_ref[i], sizeof(h)) == 0, "height at (%.1f, %.1f) differs when asked in another order", g_x[i], g_z[i]);
    }

    /* 3. The range query bounds every height in its rectangle, at every size. */
    int rects = 0;
    for (int k = 0; k < 2000; ++k) {
        int i = rng_int(&r, 0, N_POINTS - 1);
        float sz = rng_float(&r);
        double w = pow(10.0, 6.0 * sz);   /* 1 m .. 1000 km */
        double x0 = g_x[i], z0 = g_z[i], x1 = x0 + w, z1 = z0 + w * 0.7, rlo, rhi;
        terrain_height_range(x0, z0, x1, z1, &rlo, &rhi);
        for (int sj = 0; sj <= 8; ++sj)
            for (int si = 0; si <= 8; ++si) {
                double x = x0 + (x1 - x0) * si / 8.0, z = z0 + (z1 - z0) * sj / 8.0, h = terrain_height(x, z);
                CHECK(h >= rlo && h <= rhi, "height %.3f at (%.1f, %.1f) outside its range [%.3f, %.3f]", h, x, z, rlo, rhi);
            }
        rects++;
    }
    {   /* ...and is tight under the city: a chunk at the centre gets only the urban relief. */
        double rlo, rhi;
        terrain_height_range(c.x, c.z, c.x + CHUNK_SIZE, c.z + CHUNK_SIZE, &rlo, &rhi);
        CHECK(rhi <= relief_at(c.x, c.z) + 0.01, "range at the centre is loose: [%.2f, %.2f]", rlo, rhi);
        printf("range of %d rectangles checked; a chunk at the centre: [%.3f, %.3f] m\n", rects, rlo, rhi);
    }
    {   /* The city's ground has its own range, bounding it at every size and tight at a chunk's. */
        double widest = 0.0;
        for (int k = 0; k < 2000; ++k) {
            int i = rng_int(&r, 0, N_POINTS - 1);
            float sz = rng_float(&r);
            double w = pow(10.0, 5.0 * sz);   /* 1 m .. 100 km */
            double x0 = g_x[i], z0 = g_z[i], x1 = x0 + w, z1 = z0 + w * 0.7, rlo, rhi;
            terrain_urban_height_range(x0, z0, x1, z1, &rlo, &rhi);
            for (int sj = 0; sj <= 8; ++sj)
                for (int si = 0; si <= 8; ++si) {
                    double x = x0 + (x1 - x0) * si / 8.0, z = z0 + (z1 - z0) * sj / 8.0, h = terrain_urban_height(x, z);
                    CHECK(h >= rlo && h <= rhi, "city ground %.3f at (%.1f, %.1f) outside its range [%.3f, %.3f]", h, x, z, rlo, rhi);
                }
        }
        for (int k = 0; k < 400; ++k) {
            int i = rng_int(&r, 0, N_POINTS - 1);
            double x0 = floor(g_x[i] / CHUNK_SIZE) * CHUNK_SIZE, z0 = floor(g_z[i] / CHUNK_SIZE) * CHUNK_SIZE, rlo, rhi;
            terrain_urban_height_range(x0, z0, x0 + CHUNK_SIZE, z0 + CHUNK_SIZE, &rlo, &rhi);
            /* far off, the planet's highlands under it, by their slope bound */
            if (terrain_urban_lift(x0, z0) != 0.0) rhi -= 2.0 * TERRAIN_LIFT_SLOPE * 0.5 * hypot(CHUNK_SIZE, CHUNK_SIZE);
            widest = fmax(widest, rhi - rlo);
        }
        printf("city ground's range over a chunk: at most %.2f m wide (less the highlands' slope bound far off)\n", widest);
        /* With the megacity's 10 m of relief: its slope bound, 1.9%, across a chunk's diagonal, against 20 m for +-relief. */
        CHECK(widest <= 3.0, "city ground's range over a chunk is loose: %.2f m", widest);
    }

    /* 4. Slopes: gentle under the city, no cliff across the belt, bounded on the land. */
    double urbanMax = 0.0, beltMax = 0.0, landMax = 0.0;
    int nUrban = 0, nBelt = 0;
    for (int i = 0; i < N_POINTS; ++i) {
        double x = g_x[i], z = g_z[i], e = plan_city_edge(x, z);
        if (e <= -CITY_BELT) {
            double s = slope_of(urban_graded, x, z);
            if (s > urbanMax) urbanMax = s;
            CHECK(terrain_height(x, z) == (TERRAIN_URBAN_GRADED ? urban_graded(x, z) : 0.0), "height at (%.1f, %.1f) inside the city isn't the urban ground", x, z);
            CHECK(terrain_urban_height(x, z) == terrain_height(x, z), "the city's ground at (%.1f, %.1f) inside the city isn't the terrain", x, z);
            nUrban++;
        }
        /* The city's ground is the urban relief everywhere, even where the land rises past it. */
        CHECK(terrain_urban_height(x, z) == (TERRAIN_URBAN_GRADED ? urban_graded(x, z) : 0.0), "the city's ground at (%.1f, %.1f) isn't the urban relief", x, z);
        double s = slope_of(terrain_height, x, z);
        if (e > -CITY_BELT && e < TERRAIN_REGION_FROM) { if (s > beltMax) beltMax = s; nBelt++; }
        if (s > landMax) landMax = s;
    }
    /* Points scattered over 50 km rarely land in the belt: walk across it too,
     * out to where the region's landforms begin. */
    for (int a = 0; a < 720; ++a) {
        double th = a * (2.0 * PI_F / 720.0);
        for (double rad = 5000.0; rad <= 16000.0; rad += 25.0) {
            double x = c.x + rad * cos(th), z = c.z + rad * sin(th), e = plan_city_edge(x, z);
            if (e <= -CITY_BELT || e >= TERRAIN_REGION_FROM) continue;
            double s = slope_of(terrain_height, x, z);
            if (s > beltMax) beltMax = s;
            nBelt++;
        }
    }
    printf("steepest slope: urban %.2f%% (graded, %d points), belt %.2f%% (%d points), anywhere %.1f%%\n",
           100.0 * urbanMax, nUrban, 100.0 * beltMax, nBelt, 100.0 * landMax);
    CHECK(nUrban > 100 && nBelt > 1000, "too few points inside the city (%d) or the belt (%d)", nUrban, nBelt);
    CHECK(urbanMax <= URBAN_SLOPE, "urban ground too steep: %.2f%%", 100.0 * urbanMax);
    CHECK(beltMax <= BELT_SLOPE, "belt too steep: %.2f%%", 100.0 * beltMax);
    CHECK(landMax <= LAND_SLOPE, "land too steep: %.1f%%", 100.0 * landMax);

    /* 5. The footprint: the spawn is deep in the city, everything 15 km out is
     *    beyond it, everything 5 km out is inside; the edge measure keeps to its rate. */
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    CHECK(plan_urban(sx, sz) == 1.0f, "spawn (%.1f, %.1f) is not deep in the city (edge %.0f m)", sx, sz, plan_city_edge(sx, sz));
    double rmin = 1e9, rmax = 0.0;
    for (int a = 0; a < 720; ++a) {
        double th = a * (2.0 * PI_F / 720.0), dx = cos(th), dz = sin(th);
        CHECK(plan_urban(c.x + 5000.0 * dx, c.z + 5000.0 * dz) == 1.0f, "5 km out at %.1f deg is not in the city", th * 57.2958);
        CHECK(plan_urban(c.x + 15000.0 * dx, c.z + 15000.0 * dz) == 0.0f, "15 km out at %.1f deg is still in the city", th * 57.2958);
        double rad = 5000.0;   /* first crossing of the edge, to the nearest 10 m */
        while (rad < 15000.0 && plan_city_edge(c.x + rad * dx, c.z + rad * dz) < 0.0) rad += 10.0;
        if (rad < rmin) rmin = rad;
        if (rad > rmax) rmax = rad;
    }
    printf("city edge %.1f .. %.1f km from the centre\n", rmin / 1000.0, rmax / 1000.0);
    double rateMax = 0.0;
    for (int k = 0; k < 20000; ++k) {
        int i = rng_int(&r, 0, N_POINTS - 1);
        float u = rng_float(&r);
        float v = rng_float(&r);
        double dx = (2.0 * u - 1.0) * 300.0, dz = (2.0 * v - 1.0) * 300.0, d = hypot(dx, dz);
        if (d < 1e-3) continue;
        double rate = fabs(plan_city_edge(g_x[i] + dx, g_z[i] + dz) - plan_city_edge(g_x[i], g_z[i])) / d;
        if (rate > rateMax) rateMax = rate;
    }
    printf("edge measure changes by at most %.2f m/m (bound %.2f)\n", rateMax, CITY_EDGE_RATE);
    CHECK(rateMax <= CITY_EDGE_RATE, "edge measure changes faster than CITY_EDGE_RATE: %.3f", rateMax);

    /* The settlements' relief: the megacity's exactly, more for smaller ones, never past the most. */
    CHECK(plan_settlement_relief(CITY_RADIUS) == PLAN_RELIEF_MEGA_M, "a settlement of CITY_RADIUS has %.3f m of relief", plan_settlement_relief(CITY_RADIUS));
    for (double rad = 250.0, prev = 1e9; rad <= 4.0 * CITY_RADIUS; rad *= 1.25) {
        double rl = plan_settlement_relief(rad);
        CHECK(rl <= prev && rl <= PLAN_RELIEF_MAX_M && rl > 0.0, "relief %.2f m at radius %.0f m: not falling with size, or past the most", rl, rad);
        prev = rl;
    }
    CHECK(plan_settlement_near(c.x, c.z) == 0 && plan_settlement(0).relief_m == PLAN_RELIEF_MEGA_M, "the megacity isn't settlement 0, of PLAN_RELIEF_MEGA_M");

    test_landforms(&r);
    test_lod_heights();
    test_clipmap(&r);
    test_select();
    test_fade();
    test_planet(&r);
    test_planet_patches(&r);

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all terrain tests passed (%d points)\n", N_POINTS);
    return 0;
}
