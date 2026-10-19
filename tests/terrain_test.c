/* Terrain and city footprint tests (src/terrain, plan_city_edge).
 *
 * The terrain is a pure function of position: the same point gives the same
 * height whichever order it is asked in. This checks that, then the shape
 * promises: heights are finite and within their amplitudes, the range query
 * really bounds them, the ground under the city stays gentle, the belt joins
 * city and land without a cliff, and the footprint is where it should be.
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
 * terrain_test --fingerprint prints a hash of the heights, the footprint and
 * some filled clipmaps instead, to compare what two builds generate. */
#include "terrain_internal.h"
#include "plan.h"
#include "city.h"
#include "hash.h"
#include "mathlib.h"
#include <float.h>
#include <time.h>

#define N_POINTS     20000
#define REGION       25000.0   /* points scattered this far round the city centre (m) */
#define URBAN_SLOPE  0.03      /* steepest urban grade allowed */
#define BELT_SLOPE   0.12      /* steepest ground in the belt */
#define LAND_SLOPE   1.25      /* steepest land: the bound the amplitudes and wavelengths allow */

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

static double urban_graded(double x, double z) { return TERRAIN_URBAN_RELIEF_M * terrain_urban_shape(x, z); }

/* --- Levels of detail and the clipmap -------------------------------------- */

#define WALK_STEPS   6000
#define CHECKPOINTS  12

/* Share of a fractal sum's amplitude a spacing leaves out: the dropped
 * octaves' weight over the total. */
static double dropped_fraction(double wave, int octaves, double spacing) {
    int used = terrain_octaves(wave, octaves, spacing);
    double drop = 0.0, total = 0.0;
    for (int k = 0; k < octaves; ++k) {
        total += ldexp(1.0, -k);
        if (k >= used) drop += ldexp(1.0, -k);
    }
    return drop / total;
}

static int same_octaves(double s0, double s1) {
    return terrain_octaves(TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES, s0) == terrain_octaves(TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES, s1) &&
           terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s0) == terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s1);
}

/* 6. Coarse spacings leave out fine detail only: within the range bounds, and
 *    no further from full detail than the dropped octaves can reach. */
static void test_lod_heights(void) {
    double worst[TERRAIN_CLIP_LEVELS] = { 0 }, relief = TERRAIN_URBAN_GRADED ? TERRAIN_URBAN_RELIEF_M : 0.0;
    for (int i = 0; i < N_POINTS; i += 4) {
        double x = g_x[i], z = g_z[i], h = terrain_height(x, z), h0 = terrain_height_lod(x, z, 0.0), rlo, rhi;
        CHECK(memcmp(&h, &h0, sizeof(h)) == 0, "terrain_height_lod at spacing 0 differs from terrain_height at (%.1f, %.1f)", x, z);
        double amp = terrain_land_amplitude(plan_city_edge(x, z));
        terrain_height_range(x, z, x, z, &rlo, &rhi);
        for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
            double s = terrain_clip_spacing(l), hl = terrain_height_lod(x, z, s);
            /* 1e-4 m: each fractal sum is rounded to float before scaling. */
            double bound = amp * dropped_fraction(TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES, s) +
                           relief * dropped_fraction(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s) + 1e-4;
            CHECK(hl >= rlo && hl <= rhi, "height %.3f at spacing %.0f m at (%.1f, %.1f) outside its range", hl, s, x, z);
            CHECK(fabs(hl - h) <= bound, "height at spacing %.0f m at (%.1f, %.1f) is %.3f m from full detail (bound %.3f)", s, x, z, fabs(hl - h), bound);
            if (fabs(hl - h) > worst[l]) worst[l] = fabs(hl - h);
        }
    }
    printf("levels of detail (octaves kept of land %d, urban %d; largest change from full detail):\n", TERRAIN_LAND_OCTAVES, TERRAIN_URBAN_OCTAVES);
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        double s = terrain_clip_spacing(l);
        printf("  level %d  %4.0f m  land %d  urban %d  %6.2f m\n", l, s, terrain_octaves(TERRAIN_LAND_WAVE, TERRAIN_LAND_OCTAVES, s),
               terrain_octaves(TERRAIN_URBAN_WAVE, TERRAIN_URBAN_OCTAVES, s), worst[l]);
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
                /* Where both levels keep the same octaves, the shared samples are the same ground. */
                if (same && !ox && !oz && memcmp(&t->h, &hc, sizeof(float)) != 0 && bad++ < 5)
                    CHECK(0, "level %d sample (%lld, %lld) is %.6f, its parent's %.6f", l, (long long)i, (long long)j, t->h, hc);
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
    CHECK(clip.finest == 0 && updates <= TERRAIN_CLIP_LEVELS, "fill from scratch took %d updates (finest ready level %d)", updates, clip.finest);
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
            /* Every level snaps at multiples of 512 m (2 x the coarsest spacing);
             * going there is a jump of up to 256 m. */
            line = 512.0 * floor(x / 512.0 + 0.5);
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
        CHECK(spent <= (budget > TERRAIN_CLIP_W ? budget : TERRAIN_CLIP_W), "update filled %d samples on a budget of %d", spent, budget);
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
    CHECK(worstRecovery <= TERRAIN_CLIP_LEVELS, "a jump took %d updates to fill in on the full budget", worstRecovery);
    CHECK(steadyMisses == 0, "%d of %d updates while moving on the full budget left a level not ready", steadyMisses, steadyUpdates);
    printf("  walk: %d updates, %d moves checked, %d jumps (filled in within %d updates), %d steady updates all ready,\n"
           "        at most %d samples an update, %d comparisons with a fresh fill; children sit %d-%d parent samples in\n",
           WALK_STEPS, moves, jumps, worstRecovery, steadyUpdates, maxSpent, compared, mMin, mMax);
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
    printf("  the city's ground over the drawn terrain by at least:");
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        printf(" %.2f", clear[l]);
        CHECK(clear[l] > 0.0, "level %d's terrain pokes %.3f m through the city's ground", l, -clear[l]);
    }
    printf(" m\n");
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

int main(int argc, char **argv) {
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

    /* 1. Reference pass, in order: finite and within the amplitudes. */
    double amax = fmax(TERRAIN_URBAN_RELIEF_M, TERRAIN_LAND_FAR_M) + 1e-3, lo = 1e9, hi = -1e9;
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
        CHECK(rhi <= (TERRAIN_URBAN_GRADED ? TERRAIN_URBAN_RELIEF_M : 0.0) + 0.01, "range at the centre is loose: [%.2f, %.2f]", rlo, rhi);
        printf("range of %d rectangles checked; a chunk at the centre: [%.3f, %.3f] m\n", rects, rlo, rhi);
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
        if (e > -CITY_BELT && e < 2000.0) { if (s > beltMax) beltMax = s; nBelt++; }
        if (s > landMax) landMax = s;
    }
    /* Points scattered over 50 km rarely land in the belt: walk across it too. */
    for (int a = 0; a < 720; ++a) {
        double th = a * (2.0 * PI_F / 720.0);
        for (double rad = 5000.0; rad <= 16000.0; rad += 25.0) {
            double x = c.x + rad * cos(th), z = c.z + rad * sin(th), e = plan_city_edge(x, z);
            if (e <= -CITY_BELT || e >= 2000.0) continue;
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

    test_lod_heights();
    test_clipmap(&r);
    test_select();

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all terrain tests passed (%d points)\n", N_POINTS);
    return 0;
}
