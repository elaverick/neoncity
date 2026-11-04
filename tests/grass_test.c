/* The grass's rules (src/grass/grass.c, grass_rules.h).
 *
 * The numbers first: every species' density scale is at most 1, so the
 * retention never clamps and the density formula can hold. The clumps' mean
 * edgeness is what grass_rules.h says (GRASS_EDGE_MEAN, which the formula
 * rests on). Then the density: blades per m^2 counted over a wide area, for
 * every species and at part suitability, within 3% of grass_expected. Then
 * nothing where the suitability is 0 (bare ground, snow, steep, the city's
 * ground, roads), on made-up sites and over the real land round the city.
 * Then determinism: the same roots and blades whatever order they are asked
 * in, and clumps that run on across a patch's side. Then the pattern: no
 * peak in the roots' spectrum at their grid's, the patches' or the clumps'
 * frequencies (a visible grid would make one); clumps whose roots' heights
 * differ from one clump to the next, a lawn's less than a meadow's.
 *
 * grass_test --gpu evaluates the rules in shaders/grass/rules.glsl on the GPU
 * (rules_test.comp) over random cases and compares every answer with C's;
 * grass_test --fingerprint prints a hash of a region's blades, to compare
 * builds; grass_test --density-map out.ppm [km [x z]] draws the effective
 * density over the land (see density_map). --earth <bake> names the Earth
 * bake to load (the test bake by default). */
#include "grass.h"
#include "terrain_internal.h"
#include "plan.h"
#include "hash.h"
#include "thread.h"
#include "vkcore.h"
#include <float.h>

static const uint32_t SPV_rules_test[] = {
#include "grass/rules_test.comp.inc"
};

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static const char *const NAMES[GRASS_SPECIES] = { "meadow", "reeds", "lawn", "rough" };

static GrassSite site_land(float grass, float mud, float wet) {
    GrassSite s = { 0 };
    s.grass = grass; s.mud = mud; s.wet = wet; s.ny = 1.0f; s.road = (float)GRASS_ROAD_FAR;
    return s;
}
static GrassSite site_city(float maintained) {
    GrassSite s = site_land(1.0f, 0.0f, 0.0f);
    s.city = 1; s.urban = 1.0f; s.maintained = maintained;
    return s;
}

/* --- the numbers ----------------------------------------------------------------- */

static void test_numbers(void) {
    printf("species   target/m2  roots/m2  tuft  clump mean  density scale\n");
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        float d = grass_density_scale(k);
        printf("%-8s  %9.0f  %8.2f  %4.1f  %10.3f  %13.3f\n", NAMES[k], grass_target(k), grass_roots_m2(k), grass_tuft_mean(k), grass_clump_mean(k), d);
        CHECK(d > 0.0f && d <= 1.0f, "%s's density scale %.3f is past 1: too few roots or blades per tuft for its target", NAMES[k], d);
    }
}

/* The mean edgeness over uniformly spread points, for each species' clumps
 * (it shouldn't depend on their size): GRASS_EDGE_MEAN. */
static void test_edge_mean(void) {
    Rng r = rng_make(77);
    double sum = 0.0;
    long n = 0;
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        double ks = 0.0;
        const int N = 400000;
        for (int q = 0; q < N; ++q) {
            int32_t px = (int32_t)(rng_u32(&r) % 20000u) - 10000, pz = (int32_t)(rng_u32(&r) % 20000u) - 10000;
            float x = rng_float(&r) * (float)GRASS_PATCH_M, z = rng_float(&r) * (float)GRASS_PATCH_M;
            GrassClump c;
            grass_clump(k, px, pz, x, z, &c);
            ks += c.edge;
        }
        printf("%s clumps' mean edgeness %.4f\n", NAMES[k], ks / N);
        sum += ks;
        n += N;
    }
    double mean = sum / n;
    printf("mean edgeness %.4f (GRASS_EDGE_MEAN %.4f)\n", mean, GRASS_EDGE_MEAN);
    CHECK(fabs(mean - GRASS_EDGE_MEAN) < 0.002, "the clumps' mean edgeness is %.4f, not GRASS_EDGE_MEAN %.4f: set it so", mean, GRASS_EDGE_MEAN);
}

/* --- density --------------------------------------------------------------------- */

typedef struct Count { long roots, blades; } Count;

/* Every kept root and blade of species k over side x side patches from (px0, pz0), the ground s everywhere. */
static Count count_region(int k, int32_t px0, int32_t pz0, int side, const GrassSite *s) {
    Count c = { 0, 0 };
    int n = grass_grid(k);
    for (int pz = pz0; pz < pz0 + side; ++pz)
        for (int px = px0; px < px0 + side; ++px)
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    GrassCandidate cand;
                    GrassRoot root;
                    grass_candidate(k, px, pz, i, j, &cand);
                    if (!grass_keep(&cand, s, &root)) continue;
                    c.roots++;
                    c.blades += root.blades;
                }
    return c;
}

static void density_case(const char *what, const GrassSite *s, int32_t px0, int32_t pz0, int side) {
    double area = (double)side * side * GRASS_PATCH_M * GRASS_PATCH_M, blades = 0.0;
    float w[GRASS_SPECIES];
    grass_species(s, w);
    for (int k = 0; k < GRASS_SPECIES; ++k)
        if (w[k] > 0.0f) blades += (double)count_region(k, px0, pz0, side, s).blades;
    double got = blades / area, want = grass_expected(s);
    printf("%-22s %7.2f blades/m2 (formula %7.2f, %+.2f%%) over %.0f m2\n", what, got, want, want > 0 ? 100.0 * (got / want - 1.0) : 0.0, area);
    CHECK(want > 0.0 && fabs(got / want - 1.0) < 0.03, "%s: %.2f blades per m^2 where the formula gives %.2f", what, got, want);
}

static void test_density(void) {
    GrassSite meadow = site_land(1.0f, 0.0f, 0.0f), half = site_land(0.5f, 0.0f, 0.0f);
    GrassSite reeds = site_land(0.5f, 0.5f, 1.0f), bank = site_land(0.8f, 0.1f, 0.9f);
    GrassSite lawn = site_city(1.0f), rough = site_city(0.0f), mixed = site_city(0.5f);
    float w[GRASS_SPECIES];
    grass_species(&reeds, w);
    CHECK(w[GRASS_REEDS] == 1.0f, "mud with grass by the water isn't all reeds (%.2f)", w[GRASS_REEDS]);
    grass_species(&bank, w);
    printf("bank site: suitability %.2f, reeds %.2f\n", grass_suitability(&bank), w[GRASS_REEDS]);
    density_case("meadow", &meadow, 12345, -6789, 32);
    density_case("meadow, grass 0.5", &half, -40000, 512, 32);
    density_case("reeds", &reeds, 7, 7, 40);
    density_case("bank (meadow + reeds)", &bank, -3, 99999, 32);
    density_case("lawn", &lawn, 100, 100, 24);
    density_case("rough", &rough, -100, 2, 32);
    density_case("lawn and rough", &mixed, 555, -555, 24);
}

/* --- where nothing grows --------------------------------------------------------- */

static long kept_in_patch(const GrassSite *s) {
    long kept = 0;
    for (int k = 0; k < GRASS_SPECIES; ++k) kept += count_region(k, 3, -4, 1, s).roots;
    return kept;
}

static void zero_case(const char *what, const GrassSite *s) {
    float suit = grass_suitability(s);
    long kept = kept_in_patch(s);
    CHECK(suit == 0.0f && kept == 0, "%s: suitability %.3f, %ld roots kept", what, suit, kept);
}

static void test_zero(void) {
    GrassSite s = site_land(0.0f, 0.0f, 0.3f);
    zero_case("bare ground (rock, sand, soil)", &s);
    s = site_land(1.0f, 0.0f, 0.0f); s.snow = (float)GRASS_SNOW_NONE;
    zero_case("under the snow", &s);
    s = site_land(1.0f, 0.0f, 0.0f); s.ny = (float)GRASS_SLOPE_NONE;
    zero_case("steep (46 degrees)", &s);
    s = site_land(1.0f, 0.0f, 0.0f); s.urban = (float)GRASS_URBAN_NONE;
    zero_case("the city's ground", &s);
    s = site_land(1.0f, 0.0f, 0.0f); s.road = (float)GRASS_ROAD_NONE;
    zero_case("a road's gravel", &s);
    s = site_land(1.0f, 0.0f, 0.0f); s.road = -3.0f;
    zero_case("a road's asphalt", &s);
    s = site_land(0.0f, 0.0f, 0.0f); s.city = 1;
    zero_case("the city's non-grass", &s);
    s = site_city(1.0f);
    CHECK(grass_suitability(&s) == 1.0f, "the city's own grass (urban 1) isn't wholly suitable: %.3f", grass_suitability(&s));
    s = site_land(1.0f, 0.0f, 0.0f);
    CHECK(grass_suitability(&s) == 1.0f, "level grass isn't wholly suitable: %.3f", grass_suitability(&s));
    /* part ways: between the ends, and less than the grass alone */
    s.ny = 0.76f; float a = grass_suitability(&s);
    s.ny = 1.0f; s.snow = 0.3f; float b = grass_suitability(&s);
    s.snow = 0.0f; s.road = 1.6f; float c = grass_suitability(&s);
    CHECK(a > 0.0f && a < 1.0f && b > 0.0f && b < 1.0f && c > 0.0f && c < 1.0f, "no thinning at a slope, snow or a road's edge: %.2f %.2f %.2f", a, b, c);
}

/* The real land round the city: the rules on terrain_cover. */
static double g_road_x0, g_road_z0, g_road_x1, g_road_z1;
typedef struct Seg { double ax, az, bx, bz, half; } Seg;
static Seg *g_segs;
static int g_nsegs;

/* The roads' segments near the rectangle (for road_edge). */
static void roads_near(double x0, double z0, double x1, double z1) {
    free(g_segs);
    g_segs = NULL; g_nsegs = 0;
    int cap = 0;
    g_road_x0 = x0; g_road_z0 = z0; g_road_x1 = x1; g_road_z1 = z1;
    for (int k = 0; k < plan_road_count(); ++k) {
        const PlanRoad *rd = plan_road(k);
        for (int p = 0; p + 1 < rd->npts; ++p) {
            DVec2 a = rd->pts[p], b = rd->pts[p + 1];
            double pad = rd->half + 20.0;
            if (fmax(a.x, b.x) < x0 - pad || fmin(a.x, b.x) > x1 + pad || fmax(a.z, b.z) < z0 - pad || fmin(a.z, b.z) > z1 + pad) continue;
            if (g_nsegs == cap) { cap = cap ? 2 * cap : 256; g_segs = (Seg *)realloc(g_segs, (size_t)cap * sizeof(Seg)); }
            g_segs[g_nsegs++] = (Seg){ a.x, a.z, b.x, b.z, rd->half };
        }
    }
}

/* How far beyond the nearest road's paved edge (x, z) is: GRASS_ROAD_FAR if none is within 20 m. */
static float road_edge(double x, double z) {
    double best = GRASS_ROAD_FAR;
    for (int q = 0; q < g_nsegs; ++q) {
        const Seg *s = &g_segs[q];
        double ex = s->bx - s->ax, ez = s->bz - s->az, l2 = fmax(ex * ex + ez * ez, 1e-9);
        double t = ((x - s->ax) * ex + (z - s->az) * ez) / l2;
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
        double dx = x - (s->ax + t * ex), dz = z - (s->az + t * ez), e = sqrt(dx * dx + dz * dz) - s->half;
        if (e < best) best = e;
    }
    return (float)best;
}

/* The rules' site at world (x, z), the ground sampled every `spacing` metres. */
static GrassSite site_at(double x, double z, double spacing) {
    TerrainCover c;
    terrain_cover(x, z, spacing, &c);
    double s = spacing > 0.0 ? spacing : 1.0;
    double gx = (terrain_surface_lod(x + s, z, spacing, NULL) - terrain_surface_lod(x - s, z, spacing, NULL)) / (2.0 * s);
    double gz = (terrain_surface_lod(x, z + s, spacing, NULL) - terrain_surface_lod(x, z - s, spacing, NULL)) / (2.0 * s);
    GrassSite g = { 0 };
    g.grass = c.w[TERRAIN_COVER_GRASS]; g.mud = c.w[TERRAIN_COVER_MUD];
    g.snow = c.snow; g.wet = c.wet;
    g.ny = (float)(1.0 / sqrt(1.0 + gx * gx + gz * gz));
    g.urban = plan_urban(x, z);
    g.road = road_edge(x, z);
    return g;
}

/* Round the boulevard at the eastern freeway gate (city_test --grass's
 * views): the city, its edge and the land beyond. Half the points anywhere
 * within GATE_REACH of it, half within 25 m of a road's centre line. */
#define GATE_X 64598.0
#define GATE_Z 11010.0
#define GATE_REACH 15000.0
static void test_real(void) {
    roads_near(GATE_X - GATE_REACH, GATE_Z - GATE_REACH, GATE_X + GATE_REACH, GATE_Z + GATE_REACH);
    printf("%d road segments within %.0f km of the eastern gate (of %d roads)\n", g_nsegs, GATE_REACH / 1000.0, plan_road_count());
    Rng r = rng_make(9001);
    int n = 20000, some = 0, bad = 0, reeds = 0, roads = 0, city = 0, onRoad = 0;
    double sum = 0.0, dens = 0.0;
    for (int q = 0; q < n; ++q) {
        float u = rng_float(&r), v = rng_float(&r), t = rng_float(&r);
        double x = GATE_X + (2.0 * u - 1.0) * GATE_REACH, z = GATE_Z + (2.0 * v - 1.0) * GATE_REACH;
        if (q % 2 && g_nsegs) {   /* by a road */
            const Seg *g = &g_segs[rng_u32(&r) % (uint32_t)g_nsegs];
            double ex = g->bx - g->ax, ez = g->bz - g->az, l = fmax(sqrt(ex * ex + ez * ez), 1e-6), off = 50.0 * (t - 0.5);
            x = g->ax + u * ex - ez / l * off;
            z = g->az + u * ez + ex / l * off;
        }
        GrassSite s = site_at(x, z, 1.0);
        float suit = grass_suitability(&s), w[GRASS_SPECIES];
        grass_species(&s, w);
        if (s.road < (float)GRASS_ROAD_FULL) roads++;
        if (s.road <= (float)GRASS_ROAD_NONE && s.urban < (float)GRASS_URBAN_NONE) onRoad++;
        if (s.urban >= (float)GRASS_URBAN_NONE) city++;
        int none = s.grass + s.mud <= 0.0f || s.snow >= (float)GRASS_SNOW_NONE || s.urban >= (float)GRASS_URBAN_NONE ||
                   s.ny <= (float)GRASS_SLOPE_NONE || s.road <= (float)GRASS_ROAD_NONE;
        if (none && suit != 0.0f) {
            if (bad < 5) printf("  grass %.4f mud %.4f snow %.3f urban %.3f ny %.3f road %.2f: suitability %.4f\n", s.grass, s.mud, s.snow, s.urban, s.ny, s.road, suit);
            bad++;
        }
        if (suit > 0.0f) { some++; sum += suit; if (w[GRASS_REEDS] > 0.5f) reeds++; }
        dens += grass_expected(&s);
    }
    printf("round the eastern gate: %.1f%% the city's ground, %d points on a road or its gravel beyond it; grass grows on %.1f%% "
           "(mean suitability there %.2f; reeds on %.1f%% of it), %.0f blades/m2 on average; %d points by a road\n",
           100.0 * city / n, onRoad, 100.0 * some / n, some ? sum / some : 0.0, some ? 100.0 * reeds / some : 0.0, dens / n, roads);
    CHECK(bad == 0, "%d points of the land where the rules say nothing grows have grass", bad);
    CHECK(some > n / 10, "grass grows on only %d of %d points round the gate", some, n);
    CHECK(onRoad > n / 20, "only %d points on the land's roads: the road rule isn't tested", onRoad);
}

/* --- determinism ----------------------------------------------------------------- */

static uint32_t hash_blade(uint32_t h, const GrassBlade *b) {
    uint32_t w[12];
    memcpy(w, &b->x, sizeof(float)); memcpy(w + 1, &b->z, sizeof(float)); memcpy(w + 2, &b->height, sizeof(float));
    memcpy(w + 3, &b->width, sizeof(float)); memcpy(w + 4, &b->facing, sizeof(float)); memcpy(w + 5, &b->leanX, sizeof(float));
    memcpy(w + 6, &b->leanZ, sizeof(float)); memcpy(w + 7, &b->lean, sizeof(float)); memcpy(w + 8, &b->bend, sizeof(float));
    memcpy(w + 9, &b->phase, sizeof(float)); memcpy(w + 10, &b->tint, sizeof(float)); w[11] = b->seed;
    for (int i = 0; i < 12; ++i) h = hash_combine(h, w[i]);
    return h;
}

/* A hash of every root and blade of species k in patch (px, pz), its cells visited in `order` (0 rows, 1 backwards, 2 columns). */
static uint32_t patch_hash(int k, int32_t px, int32_t pz, const GrassSite *s, int order) {
    int n = grass_grid(k);
    uint32_t *cell = (uint32_t *)calloc((size_t)n * n, sizeof(uint32_t));
    for (int q = 0; q < n * n; ++q) {
        int idx = order == 0 ? q : order == 1 ? n * n - 1 - q : (q % n) * n + q / n;
        int i = idx % n, j = idx / n;
        GrassCandidate cand;
        GrassRoot root;
        grass_candidate(k, px, pz, i, j, &cand);
        uint32_t h = cand.id;
        if (grass_keep(&cand, s, &root)) {
            uint32_t hb = 0;
            memcpy(&hb, &root.height, sizeof(float));
            h = hash_combine(h, hb);
            for (int b = 0; b < root.blades; ++b) {
                GrassBlade bl;
                grass_blade(&root, b, &bl);
                h = hash_blade(h, &bl);
            }
        }
        cell[idx] = h;
    }
    uint32_t h = 0x1234u;
    for (int q = 0; q < n * n; ++q) h = hash_combine(h, cell[q]);
    free(cell);
    return h;
}

static void test_determinism(void) {
    GrassSite s = site_land(0.8f, 0.2f, 0.85f), c = site_city(0.5f);
    int same = 1;
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        const GrassSite *g = k >= GRASS_LAWN ? &c : &s;
        uint32_t a = patch_hash(k, -77, 1234, g, 0), b = patch_hash(k, -77, 1234, g, 1), d = patch_hash(k, -77, 1234, g, 2);
        uint32_t e = patch_hash(k, -77, 1234, g, 0);
        if (a != b || a != d || a != e) same = 0;
    }
    CHECK(same, "a patch's roots and blades depend on the order they are asked for");

    /* clumps run on across a patch's side: the same clump from either patch */
    Rng r = rng_make(5);
    int seams = 0, tries = 4000;
    for (int q = 0; q < tries; ++q) {
        int k = (int)(rng_u32(&r) % GRASS_SPECIES);
        int32_t px = (int32_t)(rng_u32(&r) % 1000u) - 500, pz = (int32_t)(rng_u32(&r) % 1000u) - 500;
        float z = rng_float(&r) * (float)GRASS_PATCH_M, e = 0.01f + 0.3f * rng_float(&r);
        GrassClump a, b;
        grass_clump(k, px, pz, (float)GRASS_PATCH_M - e, z, &a);
        grass_clump(k, px + 1, pz, -e, z, &b);
        if (a.id != b.id) seams++;
    }
    CHECK(seams == 0, "clumps break at a patch's side: %d of %d points just across it are in different clumps from each side", seams, tries);
}

/* --- pattern --------------------------------------------------------------------- */

/* The kept roots of species k over side x side patches, world positions relative to the region's corner. */
static int roots_region(int k, int32_t px0, int32_t pz0, int side, const GrassSite *s, double **xs, double **zs, float **hs, uint32_t **cl) {
    int n = grass_grid(k), cap = 1 << 16, m = 0;
    *xs = (double *)malloc(cap * sizeof(double)); *zs = (double *)malloc(cap * sizeof(double));
    *hs = (float *)malloc(cap * sizeof(float)); *cl = (uint32_t *)malloc(cap * sizeof(uint32_t));
    for (int pz = pz0; pz < pz0 + side; ++pz)
        for (int px = px0; px < px0 + side; ++px)
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    GrassCandidate cand;
                    GrassRoot root;
                    grass_candidate(k, px, pz, i, j, &cand);
                    if (!grass_keep(&cand, s, &root)) continue;
                    if (m == cap) {
                        cap *= 2;
                        *xs = (double *)realloc(*xs, cap * sizeof(double)); *zs = (double *)realloc(*zs, cap * sizeof(double));
                        *hs = (float *)realloc(*hs, cap * sizeof(float)); *cl = (uint32_t *)realloc(*cl, cap * sizeof(uint32_t));
                    }
                    (*xs)[m] = (px - px0) * GRASS_PATCH_M + cand.x;
                    (*zs)[m] = (pz - pz0) * GRASS_PATCH_M + cand.z;
                    (*hs)[m] = root.height;
                    (*cl)[m] = root.clump.id;
                    m++;
                }
    return m;
}

/* The structure factor |sum w exp(i k.x)|^2 / sum w^2 at wave vector (kx,
 * kz) of points in a square `side` metres across, windowed (Hann: w =
 * sin^2 across x times sin^2 across z) so the square's edges leak nothing
 * into frequencies near the patches' (the square is whole patches): about 1
 * for scattered points, N for a lattice's. */
static double g_side;
static double structure(const double *xs, const double *zs, int m, double kx, double kz) {
    double re = 0.0, im = 0.0, ww = 0.0, f = 3.14159265358979 / g_side;
    for (int q = 0; q < m; ++q) {
        double sx = sin(f * xs[q]), sz = sin(f * zs[q]), w = sx * sx * sz * sz, a = kx * xs[q] + kz * zs[q];
        re += w * cos(a); im += w * sin(a); ww += w * w;
    }
    return (re * re + im * im) / ww;
}

/* The spectrum at a grid's frequencies (spacing `cell`: the lattice's first
 * eight reciprocal vectors, where a grid of any kind would peak at once) and
 * its level nearby (each 0.87 and 1.13 times, and turned 0.1 rad either
 * way), added into peak[0] and near[0]. Summed over the vectors and several
 * regions, so a single frequency's noise (the structure factor of scattered
 * points is exponentially distributed) can't pass for a peak. */
#define SPECTRA 1
static void grid_spectrum(const double *xs, const double *zs, int m, double cell, double peak[SPECTRA], double near[SPECTRA]) {
    const double K = 2.0 * 3.14159265358979 / cell;
    static const int V[8][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 }, { 2, 0 }, { 0, 2 }, { 2, 1 }, { 1, 2 } };
    for (int v = 0; v < 8; ++v) {
        double kx = K * V[v][0], kz = K * V[v][1];
        double pk = structure(xs, zs, m, kx, kz);
        peak[0] += pk;
        static const double F[4] = { 0.87, 1.13, 1.0, 1.0 }, A[4] = { 0.0, 0.0, 0.1, -0.1 };
        for (int f = 0; f < 4; ++f) {
            double c = cos(A[f]), sn = sin(A[f]), fx = kx * F[f], fz = kz * F[f];
            double nk = structure(xs, zs, m, c * fx - sn * fz, sn * fx + c * fz);
            near[0] += nk / 4.0;
        }
    }
}
/* The peak over its level. */
static double spectrum_peak(const double peak[SPECTRA], const double near[SPECTRA]) { return peak[0] / near[0]; }

static void test_pattern(void) {
    GrassSite land = site_land(1.0f, 0.0f, 0.0f), lawnS = site_city(1.0f), roughS = site_city(0.0f);
    double between[GRASS_SPECIES] = { 0 };
    static const int CL[GRASS_SPECIES] = { GRASS_CLUMP_LIST };
    for (int k = 0; k < GRASS_SPECIES; ++k) {
        const GrassSite *s = k == GRASS_LAWN ? &lawnS : k == GRASS_ROUGH ? &roughS : &land;
        if (k == GRASS_REEDS) continue;
        /* the spectra summed over a dozen regions (one's is too noisy to judge a peak by: 12 regions of 8
         * vectors put the ratio's noise near 0.12) */
        double pr[SPECTRA] = { 0 }, nr[SPECTRA] = { 0 }, pp[SPECTRA] = { 0 }, np[SPECTRA] = { 0 }, pc[SPECTRA] = { 0 }, nc[SPECTRA] = { 0 };
        int side = k == GRASS_LAWN ? 3 : 4, roots = 0;
        g_side = side * GRASS_PATCH_M;
        double mm = 0.0, mm2 = 0.0, within = 0.0;
        int used = 0;
        for (int region = 0; region < 12; ++region) {
            double *xs, *zs;
            float *hs;
            uint32_t *cl;
            int m = roots_region(k, 2000 + 97 * region, -3000 - 131 * region, side, s, &xs, &zs, &hs, &cl);
            roots += m;
            grid_spectrum(xs, zs, m, GRASS_PATCH_M / grass_grid(k), pr, nr);
            grid_spectrum(xs, zs, m, GRASS_PATCH_M, pp, np);
            grid_spectrum(xs, zs, m, GRASS_PATCH_M / CL[k], pc, nc);

            /* clumps' mean heights: their spread against what the roots' own spread would give alone */
            typedef struct Acc { uint32_t id; double s, s2; int n; } Acc;
            int cap = 4096;
            Acc *acc = (Acc *)calloc(cap, sizeof(Acc));
            for (int q = 0; q < m; ++q) {
                uint32_t slot = hash_u32(cl[q]) % (uint32_t)cap;
                while (acc[slot].n && acc[slot].id != cl[q]) slot = (slot + 1) % (uint32_t)cap;
                if (!acc[slot].n) acc[slot].id = cl[q];
                acc[slot].s += hs[q]; acc[slot].s2 += (double)hs[q] * hs[q]; acc[slot].n++;
            }
            for (int q = 0; q < cap; ++q) {
                if (acc[q].n < 20) continue;
                double mean = acc[q].s / acc[q].n;
                mm += mean; mm2 += mean * mean;
                within += (acc[q].s2 / acc[q].n - mean * mean) / acc[q].n;
                used++;
            }
            free(acc); free(xs); free(zs); free(hs); free(cl);
        }
        double r = spectrum_peak(pr, nr), p = spectrum_peak(pp, np), c = spectrum_peak(pc, nc);
        printf("%s: %d roots; the spectrum's peak over its level nearby at the root grid's frequency %.2f, the patches' %.2f, the clumps' %.2f\n",
               NAMES[k], roots, r, p, c);
        CHECK(r < 1.5 && p < 1.5 && c < 1.5,"%s's roots show a grid: peaks %.2f (roots), %.2f (patches), %.2f (clumps)", NAMES[k], r, p, c);
        double mu = mm / used, varB = mm2 / used - mu * mu, varW = within / used;
        between[k] = sqrt(varB) / mu;
        printf("  %d clumps: their mean heights vary by %.1f%%, %.1f times what the roots' own spread alone would give\n",
               used, 100.0 * between[k], sqrt(varB / varW));
        if (k != GRASS_LAWN) CHECK(varB > 9.0 * varW, "%s's clumps don't differ: between %.2e, within %.2e", NAMES[k], varB, varW);
    }
    CHECK(between[GRASS_LAWN] < 0.5 * between[GRASS_MEADOW], "a lawn's clumps (%.3f) aren't more even than a meadow's (%.3f)",
          between[GRASS_LAWN], between[GRASS_MEADOW]);
}

/* --- fingerprint ----------------------------------------------------------------- */

static int fingerprint(void) {
    GrassSite s = site_land(0.7f, 0.3f, 0.9f), c = site_city(0.4f);
    uint32_t h = 0;
    for (int k = 0; k < GRASS_SPECIES; ++k)
        for (int pz = -1; pz <= 1; ++pz)
            for (int px = -1; px <= 1; ++px) h = hash_combine(h, patch_hash(k, 4096 + px, -8192 + pz, k >= GRASS_LAWN ? &c : &s, 0));
    printf("grass fingerprint %08x\n", h);
    return 0;
}

/* --- density map ----------------------------------------------------------------- */

/* grass_test --density-map out.ppm [km [x z]]: the effective blades per m^2
 * round (x, z) (by default the city's edge and the land past the eastern freeway gate), `km`
 * across (4 by default), 800 pixels square, each pixel the land's site
 * sampled at its size: dark blue none, through green to yellow at 250 and
 * more, cyan where reeds grow, the city's ground dark grey. North (-z) is up. */
typedef struct DMap { double cx, cz, px; int W; uint8_t *img; } DMap;
static void dmap_row(void *ctx, int j) {
    const DMap *m = (const DMap *)ctx;
    for (int i = 0; i < m->W; ++i) {
        double x = m->cx + (i - m->W / 2) * m->px, z = m->cz + (j - m->W / 2) * m->px;
        GrassSite s = site_at(x, z, m->px);
        float w[GRASS_SPECIES], d = grass_expected(&s) / 250.0f;
        grass_species(&s, w);
        float c[3];
        if (s.urban >= (float)GRASS_URBAN_NONE) { c[0] = c[1] = c[2] = 0.12f; }
        else if (d <= 0.0f) { c[0] = 0.02f; c[1] = 0.02f; c[2] = 0.12f; }
        else {
            static const float A[3] = { 0.05f, 0.10f, 0.45f }, B[3] = { 0.15f, 0.65f, 0.15f }, C[3] = { 1.0f, 0.9f, 0.1f }, R[3] = { 0.1f, 0.8f, 0.9f };
            for (int q = 0; q < 3; ++q) {
                float v = d < 0.5f ? A[q] + (B[q] - A[q]) * d / 0.5f : B[q] + (C[q] - B[q]) * fminf((d - 0.5f) / 0.5f, 1.0f);
                c[q] = v + (R[q] - v) * 0.7f * w[GRASS_REEDS];
            }
        }
        for (int q = 0; q < 3; ++q) m->img[((size_t)j * m->W + i) * 3 + q] = (uint8_t)(fminf(fmaxf(c[q], 0.0f), 1.0f) * 255.0f);
    }
}

static int density_map(const char *path, double km, double cx, double cz) {
    DMap m;
    m.W = 800; m.cx = cx; m.cz = cz; m.px = km * 1000.0 / m.W;
    m.img = (uint8_t *)malloc((size_t)m.W * m.W * 3);
    if (!m.img) return 1;
    double h = 0.5 * km * 1000.0;
    roads_near(cx - h, cz - h, cx + h, cz + h);
    thread_parallel_for(m.W, cpu_count(), dmap_row, &m);
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", m.W, m.W);
    fwrite(m.img, 1, (size_t)m.W * m.W * 3, f);
    fclose(f);
    printf("density map of %.1f km round (%.0f, %.0f), sampled every %.1f m\n", km, cx, cz, m.px);
    free(m.img);
    return 0;
}

/* --- the GPU's rules against C's ------------------------------------------------- */

#define GPU_IN  16
#define GPU_OUT 32

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static float bitsf(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* rules_test.comp's case: species, patch, cell, the site, a blade index. */
static void gpu_case(Rng *r, uint32_t *in) {
    int k = (int)(rng_u32(r) % GRASS_SPECIES);
    int n = grass_grid(k);
    memset(in, 0, GPU_IN * sizeof(uint32_t));
    in[0] = (uint32_t)k;
    in[1] = (uint32_t)((int32_t)(rng_u32(r) % 400000u) - 200000);
    in[2] = (uint32_t)((int32_t)(rng_u32(r) % 400000u) - 200000);
    in[3] = rng_u32(r) % (uint32_t)n;
    in[4] = rng_u32(r) % (uint32_t)n;
    float g = rng_float(r), mud = 0.5f * rng_float(r) * (1.0f - g);
    float snow = rng_float(r) < 0.7f ? 0.0f : rng_float(r);
    float wet = rng_float(r), ny = 0.6f + 0.4f * rng_float(r);
    float urban = rng_float(r) < 0.7f ? 0.0f : rng_float(r);
    float road = rng_float(r) < 0.5f ? (float)GRASS_ROAD_FAR : 3.0f * rng_float(r);
    float maint = rng_float(r);
    float v[8] = { g, mud, snow, wet, ny, urban, road, maint };
    for (int q = 0; q < 8; ++q) in[5 + q] = fbits(v[q]);
    in[13] = k >= GRASS_LAWN ? 1u : 0u;
    in[14] = rng_u32(r) % 10u;
}

/* C's answer to a case, as rules_test.comp lays it out. */
static void cpu_answer(const uint32_t *in, uint32_t *out) {
    memset(out, 0, GPU_OUT * sizeof(uint32_t));
    int k = (int)in[0];
    GrassSite s;
    s.grass = bitsf(in[5]); s.mud = bitsf(in[6]); s.snow = bitsf(in[7]); s.wet = bitsf(in[8]);
    s.ny = bitsf(in[9]); s.urban = bitsf(in[10]); s.road = bitsf(in[11]); s.maintained = bitsf(in[12]);
    s.city = (int)in[13];
    out[0] = fbits(grass_suitability(&s));
    float w[GRASS_SPECIES];
    grass_species(&s, w);
    for (int q = 0; q < GRASS_SPECIES; ++q) out[1 + q] = fbits(w[q]);
    out[5] = fbits(grass_expected(&s));
    GrassCandidate c;
    grass_candidate(k, (int32_t)in[1], (int32_t)in[2], (int)in[3], (int)in[4], &c);
    out[6] = c.id; out[7] = fbits(c.x); out[8] = fbits(c.z);
    GrassClump cl;
    grass_clump(k, c.px, c.pz, c.x, c.z, &cl);
    out[12] = cl.id;
    out[13] = fbits(cl.edge); out[14] = fbits(cl.density); out[15] = fbits(cl.height);
    out[16] = fbits(cl.lean); out[17] = fbits(cl.stiffness); out[18] = fbits(cl.tint); out[19] = (uint32_t)cl.bare;
    GrassRoot root;
    if (!grass_keep(&c, &s, &root)) return;
    out[9] = 1;
    out[10] = fbits(root.height);
    out[11] = (uint32_t)root.blades;
    GrassBlade b;
    int bi = (int)in[14] < root.blades - 1 ? (int)in[14] : root.blades - 1;
    grass_blade(&root, bi, &b);
    float bf[11] = { b.x, b.z, b.height, b.width, b.facing, b.leanX, b.leanZ, b.lean, b.bend, b.phase, b.tint };
    for (int q = 0; q < 11; ++q) out[20 + q] = fbits(bf[q]);
    out[31] = b.seed;
}

static int gpu_check(int validation) {
    const uint32_t N = 1u << 16;
    VkCore vk;
    vk_init_headless(&vk, validation != 0, false);
    printf("GPU: %s\n", vk.props.deviceName);
    VkDevice d = vk.device;
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    Buffer bin = vk_buffer(&vk, (VkDeviceSize)N * GPU_IN * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
    Buffer bout = vk_buffer(&vk, (VkDeviceSize)N * GPU_OUT * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
    uint32_t *in = (uint32_t *)bin.mapped;
    Rng r = rng_make(2468);
    for (uint32_t c = 0; c < N; ++c) gpu_case(&r, in + (size_t)c * GPU_IN);

    VkDescriptorSetLayoutBinding b[2] = { 0 };
    for (int i = 0; i < 2; ++i) {
        b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 2; li.pBindings = b;
    VkDescriptorSetLayout dsl;
    VK_CHECK(vkCreateDescriptorSetLayout(d, &li, NULL, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VkPipelineLayout pl;
    VK_CHECK(vkCreatePipelineLayout(d, &pli, NULL, &pl));
    VkShaderModule sm = vk_shader(&vk, SPV_rules_test, sizeof(SPV_rules_test));
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = sm; ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline pipe;
    VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VkDescriptorPool dp;
    VK_CHECK(vkCreateDescriptorPool(d, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    VK_CHECK(vkAllocateDescriptorSets(d, &dai, &set));
    VkDescriptorBufferInfo bi[2] = { { bin.buf, 0, VK_WHOLE_SIZE }, { bout.buf, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    for (int i = 0; i < 2; ++i) {
        wr[i].dstSet = set; wr[i].dstBinding = (uint32_t)i; wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wr[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(d, 2, wr, 0, NULL);

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.queueFamilyIndex = vk.queueFamily;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(d, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(d, &cai, &cmd));
    VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bgi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bgi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &N);
    vkCmdDispatch(cmd, (N + 63) / 64, 1, 1);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK_CHECK(vkCreateFence(d, &fi, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX));

    /* Compare. Ids, counts and seeds exactly; floats within a small tolerance
     * (the GPU's division, square root and trigonometry aren't IEEE's
     * correctly rounded ones); and a decision (kept, a clump's bare spot) may
     * only differ where a draw sits within that tolerance of its threshold. */
    const uint32_t *gpu = (const uint32_t *)bout.mapped;
    long keptBoth = 0, keptDiffer = 0, idDiffer = 0, floatBad = 0, clumpDiffer = 0;
    double worst = 0.0;
    int worstField = -1;
    static const int IS_FLOAT[GPU_OUT] = { 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
    for (uint32_t c = 0; c < N; ++c) {
        uint32_t want[GPU_OUT];
        const uint32_t *got = gpu + (size_t)c * GPU_OUT;
        cpu_answer(in + (size_t)c * GPU_IN, want);
        if (got[6] != want[6]) idDiffer++;
        if (got[12] != want[12]) { clumpDiffer++; continue; }   /* a point on a clump's border: nearer one seed on each */
        if (got[9] != want[9]) { keptDiffer++; continue; }
        if (want[9]) keptBoth++;
        int last = want[9] ? GPU_OUT : 20;
        for (int q = 0; q < last; ++q) {
            if (q == 9) continue;
            if (!IS_FLOAT[q]) {
                if (got[q] != want[q]) { floatBad++; if (worstField < 0) worstField = q; }
                continue;
            }
            double a = bitsf(got[q]), e = bitsf(want[q]), diff = fabs(a - e) / fmax(1.0, fabs(e));
            if (q == 16 || q == 24) diff = fmin(diff, fabs(fabs(a - e) - 6.28318531) / 6.3);   /* angles: either side of 2 pi */
            if (diff > worst) { worst = diff; worstField = q; }
            if (diff > 1e-4) floatBad++;
        }
    }
    printf("%u cases: %ld roots kept by both; differing: %ld kept, %ld clumps, %ld ids, %ld values (worst %.2e, field %d)\n",
           N, keptBoth, keptDiffer, clumpDiffer, idDiffer, floatBad, worst, worstField);
    CHECK(idDiffer == 0, "%ld roots' identities differ between the GPU and C", idDiffer);
    CHECK(keptDiffer <= (long)N / 2000 && clumpDiffer <= (long)N / 2000, "the GPU and C disagree on %ld roots kept and %ld clumps of %u",
          keptDiffer, clumpDiffer, N);
    CHECK(floatBad == 0, "%ld values differ between the GPU and C (worst %.2e in field %d)", floatBad, worst, worstField);
    CHECK(keptBoth > (long)N / 20, "too few roots kept (%ld) to compare the blades", keptBoth);

    vkDestroyFence(d, fence, NULL);
    vkDestroyCommandPool(d, pool, NULL);
    vkDestroyDescriptorPool(d, dp, NULL);
    vkDestroyPipeline(d, pipe, NULL);
    vkDestroyShaderModule(d, sm, NULL);
    vkDestroyPipelineLayout(d, pl, NULL);
    vkDestroyDescriptorSetLayout(d, dsl, NULL);
    vk_buffer_destroy(&vk, &bin);
    vk_buffer_destroy(&vk, &bout);
    vk_shutdown(&vk);
    return g_failures != 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--gpu") == 0) {
        gpu_check(argc > 2 && strcmp(argv[2], "--validation") == 0);
        printf(g_failures ? "grass_test --gpu: %d failure(s)\n" : "grass_test --gpu: the GPU's rules are C's\n", g_failures);
        return g_failures != 0;
    }
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();

    const char *earth = NULL;   /* "none": the procedural stand-in */
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--earth") != 0) continue;
        earth = argv[i + 1];
        for (int k = i; k + 2 < argc; ++k) argv[k] = argv[k + 2];
        argc -= 2;
        break;
    }
    if (!earth) {
        static const char *const candidates[] = { "tests/data/earth_test.bake", "../tests/data/earth_test.bake" };
        for (size_t k = 0; k < 2 && !earth; ++k) { FILE *f = fopen(candidates[k], "rb"); if (f) { fclose(f); earth = candidates[k]; } }
    }
    if (earth && strcmp(earth, "none") != 0) terrain_earth_load(earth);
    plan_world_init(cpu_count());

    if (argc > 2 && strcmp(argv[1], "--density-map") == 0)
        return density_map(argv[2], argc > 3 ? atof(argv[3]) : 4.0, argc > 5 ? atof(argv[4]) : 65600.0, argc > 5 ? atof(argv[5]) : 12100.0);

    test_numbers();
    test_edge_mean();
    test_density();
    test_zero();
    test_real();
    test_determinism();
    test_pattern();
    printf(g_failures ? "grass_test: %d failure(s)\n" : "grass_test: all passed\n", g_failures);
    return g_failures != 0;
}
