/* Terrain and city footprint tests (src/terrain, plan_city_edge).
 *
 * The terrain is a pure function of position: the same point gives the same
 * height whichever order it is asked in. This checks that, then the shape
 * promises: heights are finite and within their amplitudes, the range query
 * really bounds them, the ground under the city stays gentle, the belt joins
 * city and land without a cliff, and the footprint is where it should be.
 *
 * terrain_test --fingerprint prints a hash of the heights and the footprint
 * over a fixed set of points instead, to compare what two builds generate. */
#include "terrain_internal.h"
#include "plan.h"
#include "city.h"
#include "hash.h"
#include "mathlib.h"

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
    printf("fingerprint over %d points\n", n);
    printf("  height     %016llx\n", (unsigned long long)hh);
    printf("  edge       %016llx\n", (unsigned long long)he);
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
            nUrban++;
        }
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

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all terrain tests passed (%d points)\n", N_POINTS);
    return 0;
}
