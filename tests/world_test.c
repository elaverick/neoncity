/* The world's settlements and the roads between them (src/plan/plan_world.c,
 * plan_roads.c).
 *
 * Settlements: the megacity is settlement 0, as it was; PLAN_COLOSSAL cities of
 * its size and the megacities between PLAN_MEGA_MIN_KM and PLAN_MEGA_MAX_KM
 * across; every one on land the sea can't reach, clear of canyons and
 * mountains, apart from the others, inside the world's reach; the edge
 * measure finds each, and each is built about as big as it was placed.
 *
 * Roads: every settlement on a continent joined to the rest of it; every road
 * between nodes, on land, clear of the other cities; junctions only where
 * three or more roads meet; freeways about FREEWAY_SHARE of the length, and
 * at every colossal city.
 *
 * world_test --fingerprint prints a hash of what was placed and built, to
 * compare two builds (or the game's floating point against the tests', or
 * world_test --threads 1 --fingerprint against any other number of threads);
 * world_test --map out.ppm [px] draws the world, world_test --local out.ppm
 * [km [x z]] the land and roads round a point (the megacity by default). */
#include "plan.h"
#include "terrain.h"
#include "city.h"
#include "hash.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static uint64_t fnv(const void *data, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* A settlement's built towns, and the size across of a disc of their area (km). */
static int built_towns(const PlanSettlement *s, double *km) {
    double r = s->radius + 2.0 * TOWN_SIZE;
    int i0 = (int)floor((s->centre.x - r) / TOWN_SIZE), i1 = (int)floor((s->centre.x + r) / TOWN_SIZE);
    int j0 = (int)floor((s->centre.z - r) / TOWN_SIZE), j1 = (int)floor((s->centre.z + r) / TOWN_SIZE);
    int n = 0;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            DVec2 c = plan_town_centre(i, j);
            if (dlen(dsub(c, s->centre)) <= r && plan_town_built(i, j)) n++;
        }
    *km = 2.0 * sqrt(n * TOWN_SIZE * TOWN_SIZE / 3.14159265358979) / 1000.0;
    return n;
}

static void test_settlements(void) {
    PlanSettlement home = plan_settlement(0);
    DVec2 c0 = plan_town_centre(0, 0);
    CHECK(home.centre.x == c0.x && home.centre.z == c0.z && home.radius == CITY_RADIUS && home.relief_m == PLAN_RELIEF_MEGA_M
          && home.colossal && home.town[0] == 0 && home.town[1] == 0, "settlement 0 isn't the megacity as it was");
    int colossal = 0, continents[64] = { 0 };
    double gapMin = 1e300, seaMin = 1e300, reachMax = 0.0;
    double kmLo[2] = { 1e9, 1e9 }, kmHi[2] = { 0.0, 0.0 };
    for (int k = 0; k < PLAN_SETTLEMENTS; ++k) {
        PlanSettlement s = plan_settlement(k);
        colossal += s.colossal;
        CHECK(s.colossal == (k < PLAN_COLOSSAL), "settlement %d: colossal %d out of order", k, s.colossal);
        if (s.colossal) CHECK(s.radius == CITY_RADIUS, "colossal settlement %d has radius %.0f", k, s.radius);
        else CHECK(s.size_km >= PLAN_MEGA_MIN_KM && s.size_km <= PLAN_MEGA_MAX_KM && s.radius == plan_settlement_radius(s.size_km),
                   "megacity %d is %.2f km (radius %.0f)", k, s.size_km, s.radius);
        DVec2 tc = plan_town_centre(s.town[0], s.town[1]);
        CHECK(tc.x == s.centre.x && tc.z == s.centre.z && plan_town_regional(s.town[0], s.town[1]), "settlement %d's centre isn't its regional town's", k);
        double sea = plan_world_sea(s.centre.x, s.centre.z);
        if (k) seaMin = fmin(seaMin, sea);
        CHECK(k == 0 || sea >= 90.0, "settlement %d stands %.0f m above the sea to come", k, sea);
        CHECK(s.continent > 0, "settlement %d isn't on a continent", k);
        if (s.continent > 0 && s.continent < 64) continents[s.continent]++;
        reachMax = fmax(reachMax, dlen(s.centre));
        CHECK(dlen(s.centre) <= PLAN_WORLD_REACH, "settlement %d lies %.0f km out, past the world's reach", k, dlen(s.centre) / 1000.0);
        CHECK(plan_city_edge(s.centre.x, s.centre.z) < -CITY_BELT && plan_settlement_near(s.centre.x, s.centre.z) == k,
              "the edge measure doesn't find settlement %d (edge %.0f, near %d)", k, plan_city_edge(s.centre.x, s.centre.z),
              plan_settlement_near(s.centre.x, s.centre.z));
        TerrainLandform lf;
        terrain_landform(s.centre.x, s.centre.z, &lf);
        CHECK(k == 0 || (lf.canyon == 0.0f && lf.w[TERRAIN_MOUNTAINS] <= 0.35f), "settlement %d stands on a canyon or a mountain", k);
        for (int q = 0; q < k; ++q) {
            PlanSettlement o = plan_settlement(q);
            gapMin = fmin(gapMin, dlen(dsub(s.centre, o.centre)) - s.radius - o.radius);
        }
        double km;
        built_towns(&s, &km);
        kmLo[s.colossal] = fmin(kmLo[s.colossal], km);
        kmHi[s.colossal] = fmax(kmHi[s.colossal], km);
    }
    int nc = 0, biggest = 0;
    for (int k = 1; k < 64; ++k) if (continents[k]) { nc++; biggest = continents[k] > biggest ? continents[k] : biggest; }
    printf("settlements: %d colossal, %d megacities on %d continents (the largest holds %d); the farthest %.0f km out\n",
           colossal, PLAN_SETTLEMENTS - colossal, nc, biggest, reachMax / 1000.0);
    printf("  footprints at least %.0f km apart; centres at least %.0f m above the sea to come\n", gapMin / 1000.0, seaMin);
    printf("  built across (from their towns' area): colossal %.1f .. %.1f km, megacities %.1f .. %.1f km\n",
           kmLo[1], kmHi[1], kmLo[0], kmHi[0]);
    CHECK(colossal == PLAN_COLOSSAL, "%d colossal settlements, not %d", colossal, PLAN_COLOSSAL);
    CHECK(gapMin > 20000.0, "two footprints only %.1f km apart", gapMin / 1000.0);
    CHECK(kmLo[1] >= 17.0 && kmHi[1] <= 23.0, "colossal cities built %.1f .. %.1f km across", kmLo[1], kmHi[1]);
    CHECK(kmLo[0] >= 5.5 && kmHi[0] <= 13.5, "megacities built %.1f .. %.1f km across", kmLo[0], kmHi[0]);

    /* The edge measure: capped, continuous, and no faster than CITY_EDGE_RATE, near the settlements too. */
    Rng r = rng_make(77);
    double rateMax = 0.0, eMax = -1e300;
    for (int k = 0; k < 40000; ++k) {
        PlanSettlement s = plan_settlement(rng_int(&r, 0, PLAN_SETTLEMENTS - 1));
        float a = rng_float(&r), b = rng_float(&r), u = rng_float(&r), v = rng_float(&r);
        double rad = (s.radius + PLAN_EDGE_CAP + 5000.0) * sqrt(a), th = 6.2831853 * b;
        double x = s.centre.x + rad * cos(th), z = s.centre.z + rad * sin(th);
        double dx = (2.0 * u - 1.0) * 300.0, dz = (2.0 * v - 1.0) * 300.0, d = hypot(dx, dz);
        double e0 = plan_city_edge(x, z), e1 = plan_city_edge(x + dx, z + dz);
        eMax = fmax(eMax, e0);
        if (d > 1e-3) rateMax = fmax(rateMax, fabs(e1 - e0) / d);
    }
    printf("  edge measure near them: at most %.0f m, changing by at most %.2f m/m (bound %.2f)\n", eMax, rateMax, CITY_EDGE_RATE);
    CHECK(eMax <= PLAN_EDGE_CAP, "edge measure %.0f past its cap", eMax);
    CHECK(rateMax <= CITY_EDGE_RATE, "edge measure changes faster than CITY_EDGE_RATE: %.3f", rateMax);
}

static int uf_find(int *p, int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }

static void test_roads(void) {
    int nr = plan_road_count(), nn = plan_node_count();
    int *parent = (int *)malloc(sizeof(int) * (size_t)nn), *deg = (int *)calloc((size_t)nn, sizeof(int));
    int fwAt[PLAN_SETTLEMENTS] = { 0 };
    for (int k = 0; k < nn; ++k) parent[k] = k;
    double total = 0.0, freeway = 0.0, detourMax = 0.0, seaMin = 1e300;
    int offLand = 0, inCity = 0, badEnds = 0;
    for (int r = 0; r < nr; ++r) {
        const PlanRoad *R = plan_road(r);
        PlanNode a = plan_node(R->from), b = plan_node(R->to);
        deg[R->from]++; deg[R->to]++;
        parent[uf_find(parent, R->from)] = uf_find(parent, R->to);
        double len = R->s[R->npts - 1];
        total += len;
        if (R->cls == PLAN_ROAD_FREEWAY) {
            freeway += len;
            if (a.settlement >= 0) fwAt[a.settlement] = 1;
            if (b.settlement >= 0) fwAt[b.settlement] = 1;
        }
        if (dlen(dsub(R->pts[0], a.p)) > 1.0 || dlen(dsub(R->pts[R->npts - 1], b.p)) > 1.0) badEnds++;
        double straight = dlen(dsub(b.p, a.p));
        if (straight > 50000.0) detourMax = fmax(detourMax, len / straight);
        for (int k = 0; k + 1 < R->npts; ++k)
            for (int q = 0; q < 4; ++q) {
                DVec2 p = dadd(R->pts[k], dscale(dsub(R->pts[k + 1], R->pts[k]), q / 4.0));
                double sea = plan_world_sea(p.x, p.z);
                seaMin = fmin(seaMin, sea);
                if (sea <= 0.0) offLand++;
                if (plan_city_edge(p.x, p.z) < 0.0) {
                    int near = plan_settlement_near(p.x, p.z);
                    if (near != a.settlement && near != b.settlement) inCity++;
                }
            }
    }
    int junctions = 0, deadJunctions = 0, splitContinents = 0, colossalNoFw = 0;
    for (int k = 0; k < nn; ++k)
        if (plan_node(k).settlement < 0) {
            junctions++;
            if (deg[k] < 3) deadJunctions++;
        }
    for (int s = 0; s < PLAN_SETTLEMENTS; ++s) {
        for (int t = s + 1; t < PLAN_SETTLEMENTS; ++t)
            if (plan_settlement(s).continent == plan_settlement(t).continent && uf_find(parent, s) != uf_find(parent, t)) { splitContinents++; break; }
        if (plan_settlement(s).colossal && !fwAt[s]) colossalNoFw++;
    }
    printf("roads: %d between %d nodes (%d junctions), %.0f km in all, %.0f%% freeway; the worst detour %.2f times the straight line\n",
           nr, nn, junctions, total / 1000.0, 100.0 * freeway / total, detourMax);
    printf("  the lowest point %.0f m above the sea to come; settlement 0 has %d roads out\n", seaMin, deg[0]);
    CHECK(splitContinents == 0, "%d settlements not joined to the rest of their continent", splitContinents);
    CHECK(offLand == 0, "%d points of road where the sea will be", offLand);
    CHECK(inCity == 0, "%d points of road inside other settlements", inCity);
    CHECK(badEnds == 0, "%d roads not ending at their nodes", badEnds);
    CHECK(deadJunctions == 0, "%d junctions where fewer than three roads meet", deadJunctions);
    CHECK(colossalNoFw == 0, "%d colossal cities without a freeway", colossalNoFw);
    CHECK(freeway > 0.25 * total && freeway < 0.7 * total, "freeways %.0f%% of the network", 100.0 * freeway / total);
    CHECK(deg[0] >= 2, "only %d roads out of the megacity", deg[0]);
    free(parent);
    free(deg);
}

static int fingerprint(void) {
    uint64_t hs = 0xcbf29ce484222325ULL;
    for (int k = 0; k < PLAN_SETTLEMENTS; ++k) {
        PlanSettlement s = plan_settlement(k);
        double v[4] = { s.centre.x, s.centre.z, s.radius, s.relief_m };
        int iv[4] = { s.town[0], s.town[1], s.colossal, s.continent };
        hs = fnv(v, sizeof(v), hs);
        hs = fnv(iv, sizeof(iv), hs);
    }
    uint64_t hr = 0xcbf29ce484222325ULL;
    for (int r = 0; r < plan_road_count(); ++r) {
        const PlanRoad *R = plan_road(r);
        int iv[3] = { R->from, R->to, R->cls };
        hr = fnv(iv, sizeof(iv), hr);
        hr = fnv(R->pts, sizeof(DVec2) * (size_t)R->npts, hr);
    }
    printf("world fingerprint\n");
    printf("  settlements %016llx\n", (unsigned long long)hs);
    printf("  roads       %016llx\n", (unsigned long long)hr);
    return 0;
}

/* --- The map --------------------------------------------------------------------------- */

typedef struct Image { int w, h; uint8_t *px; } Image;

static void put(Image *im, int x, int y, const float c[3]) {
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) return;
    for (int q = 0; q < 3; ++q) im->px[((size_t)y * im->w + x) * 3 + q] = (uint8_t)clampf(c[q] * 255.0f, 0.0f, 255.0f);
}

static void disc(Image *im, double cx, double cy, double r, const float c[3]) {
    for (int y = (int)floor(cy - r); y <= (int)ceil(cy + r); ++y)
        for (int x = (int)floor(cx - r); x <= (int)ceil(cx + r); ++x)
            if ((x + 0.5 - cx) * (x + 0.5 - cx) + (y + 0.5 - cy) * (y + 0.5 - cy) <= r * r) put(im, x, y, c);
}

static void line(Image *im, double x0, double y0, double x1, double y1, double r, const float c[3]) {
    if (fmax(x0, x1) < -r || fmax(y0, y1) < -r || fmin(x0, x1) > im->w + r || fmin(y0, y1) > im->h + r) return;
    int n = (int)ceil(hypot(x1 - x0, y1 - y0) * 2.0) + 1;
    for (int k = 0; k <= n; ++k) disc(im, x0 + (x1 - x0) * k / n, y0 + (y1 - y0) * k / n, r, c);
}

static int save(const Image *im, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->px, 1, (size_t)im->w * im->h * 3, f);
    fclose(f);
    return 0;
}

typedef struct MapCtx { Image *im; double x0, z0, m; int relief; } MapCtx;

/* The land, tinted by its leading landform and shaded by its height; the sea to come blue. */
static void map_row(void *ctx, int j) {
    MapCtx *mc = (MapCtx *)ctx;
    static const float tint[TERRAIN_TYPES][3] = { { 0.55f, 0.75f, 0.45f }, { 0.85f, 0.72f, 0.45f }, { 0.75f, 0.75f, 0.8f }, { 0.9f, 0.5f, 0.4f } };
    for (int i = 0; i < mc->im->w; ++i) {
        double x = mc->x0 + (i + 0.5) * mc->m, z = mc->z0 + (j + 0.5) * mc->m;
        double sea = plan_world_sea(x, z);
        float col[3];
        if (sea <= 0.0) {
            float d = clampf((float)(-sea / 1500.0), 0.0f, 1.0f);
            col[0] = 0.10f - 0.06f * d; col[1] = 0.22f - 0.12f * d; col[2] = 0.42f - 0.15f * d;
            if (sea < -1e8) { col[0] = col[1] = col[2] = 0.05f; }
        } else {
            TerrainLandform lf;
            terrain_landform(x, z, &lf);
            double h = terrain_height_lod(x, z, mc->m);
            float k = 0.55f + 0.5f * clampf((float)(h / 2000.0), -0.3f, 1.0f);
            if (mc->relief) {   /* lit from the north-west */
                double gx = (terrain_height_lod(x + mc->m, z, mc->m) - h) / mc->m, gz = (terrain_height_lod(x, z + mc->m, mc->m) - h) / mc->m;
                k *= clampf(0.85f - (float)(1.2 * (gx + gz) / sqrt(1.0 + gx * gx + gz * gz)), 0.25f, 1.4f);
                if (lf.canyon > 20.0f) k *= 0.6f;
            }
            for (int q = 0; q < 3; ++q) {
                col[q] = 0.0f;
                for (int t = 0; t < TERRAIN_TYPES; ++t) col[q] += lf.w[t] * tint[t][q];
                col[q] *= k;
            }
            if (sea < 100.0) { col[0] *= 0.85f; col[1] *= 0.9f; }
        }
        put(mc->im, i, j, col);
    }
}

/* The square of side `ext` * 2 round (cx, cz), W px square, north (-z) up:
 * the land and the sea to come, the roads (freeways red, trunk roads yellow,
 * junctions white), the settlements (colossal red, the megacities orange, the
 * megacity white), each as its footprint where that is bigger than a dot. */
static int draw_map(const char *path, int W, double cx, double cz, double ext, int relief, int threads) {
    Image im = { W, W, (uint8_t *)calloc((size_t)W * W * 3, 1) };
    if (!im.px) return 1;
    MapCtx mc = { &im, cx - ext, cz - ext, 2.0 * ext / W, relief };
    thread_parallel_for(W, threads, map_row, &mc);
    const float red[3] = { 0.95f, 0.15f, 0.1f }, orange[3] = { 1.0f, 0.65f, 0.1f }, white[3] = { 1.0f, 1.0f, 1.0f };
    const float fw[3] = { 0.9f, 0.1f, 0.1f }, trunk[3] = { 1.0f, 0.9f, 0.2f };
    double scale = W / 1600.0;
    for (int pass = 0; pass < 2; ++pass)
        for (int r = 0; r < plan_road_count(); ++r) {
            const PlanRoad *R = plan_road(r);
            if ((R->cls == PLAN_ROAD_FREEWAY) != pass) continue;
            double w = fmax((R->cls == PLAN_ROAD_FREEWAY ? 1.3 : 0.7) * fmin(scale, 1.5), R->half / mc.m);
            for (int k = 0; k + 1 < R->npts; ++k)
                line(&im, (R->pts[k].x - mc.x0) / mc.m, (R->pts[k].z - mc.z0) / mc.m, (R->pts[k + 1].x - mc.x0) / mc.m,
                     (R->pts[k + 1].z - mc.z0) / mc.m, w, pass ? fw : trunk);
        }
    for (int k = 0; k < plan_node_count(); ++k) {
        PlanNode n = plan_node(k);
        if (n.settlement < 0) disc(&im, (n.p.x - mc.x0) / mc.m, (n.p.z - mc.z0) / mc.m, 1.6 * fmin(scale, 1.5), white);
    }
    for (int k = PLAN_SETTLEMENTS - 1; k >= 0; --k) {
        PlanSettlement s = plan_settlement(k);
        double px = (s.centre.x - mc.x0) / mc.m, py = (s.centre.z - mc.z0) / mc.m;
        disc(&im, px, py, fmax((s.colossal ? 4.0 : 2.5) * fmin(scale, 1.5) + 0.5, s.radius / mc.m), k == 0 ? white : s.colossal ? red : orange);
    }
    int rc = save(&im, path);
    printf("map, %d px, %.2f km a pixel\n", W, mc.m / 1000.0);
    free(im.px);
    return rc;
}

int main(int argc, char **argv) {
    int threads = cpu_count();
    if (argc > 2 && strcmp(argv[1], "--threads") == 0) {   /* the world must come out the same on any number */
        threads = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    }
    clock_t t0 = clock();
    plan_world_init(threads);
    printf("world built on %d threads in %.2f s\n", threads, (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();
    if (argc > 2 && strcmp(argv[1], "--map") == 0) return draw_map(argv[2], argc > 3 ? atoi(argv[3]) : 1600, 0.0, 0.0, PLAN_WORLD_REACH * 1.08, 0, threads);
    if (argc > 2 && strcmp(argv[1], "--local") == 0) {
        DVec2 c = plan_town_centre(0, 0);
        double km = argc > 3 ? atof(argv[3]) : 300.0;
        if (argc > 5) c = dv(atof(argv[4]), atof(argv[5]));
        return draw_map(argv[2], 1200, c.x, c.z, km * 500.0, 1, threads);
    }

    test_settlements();
    test_roads();

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all world tests passed\n");
    return 0;
}
