/* The world's settlements and the roads between them (src/plan/plan_world.c,
 * plan_roads.c).
 *
 * Settlements: the megacity is settlement 0, its core at town (0, 0) in the
 * world's chart; the rest are the list's cities in rank order, colossal to
 * PLAN_COLOSSAL_RANK; each in its own chart about its centre, its town's;
 * each sized from its real urban area, the bigger the area the bigger the
 * city, from PLAN_CITY_MIN_KM to PLAN_CITY_MAX_KM; none of their footprints
 * on the sea or within PLAN_COAST_M of it, none meeting another; the edge
 * measure finds each, and each is built about as big as its area makes it.
 *
 * Roads: every settlement on a continent joined to the rest of it; every road
 * between nodes, on land, clear of the other cities; junctions only where
 * three or more roads meet; freeways about FREEWAY_SHARE of the length, and
 * at every colossal city. Each ends at its settlements at a gate on the
 * boundary of the built towns; its profile keeps to its grade and meets the
 * city's ground there; the ground is the road along its centre and falls
 * away from it no steeper than a batter.
 *
 * world_test --fingerprint prints a hash of what was placed and built, to
 * compare two builds (or the game's floating point against the tests', or
 * world_test --threads 1 --fingerprint against any other number of threads);
 * world_test --map out.ppm [px] draws the planet (longitude across, latitude
 * up), world_test --chart out.ppm [px] the roads' reach in the world chart,
 * world_test --local out.ppm [km [x z]] the land, the footprints and the roads
 * round a point (the megacity by default); world_test --list lists the
 * settlements, their areas, sizes and how much of a disc of their size each
 * keeps.
 * world_test [--threads n] --earth <bake> ... builds the world on the Earth's
 * bake (src/terrain/terrain_earth.c); without one, on the procedural stand-in. */
#include "plan.h"
#include "terrain_internal.h"
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

static double warp_of(double r) { return 1.4142135623730951 * (CITY_WARP1_M + CITY_WARP2_M) * r / CITY_RADIUS; }

/* A settlement's built towns, every stride-th each way, and the size across
 * of a disc of their area over the sphere (km): the world chart stretches the
 * land across a line from its origin by th / sin th, th the angle out. */
static int built_towns(const PlanSettlement *s, double *km) {
    double th = dlen(s->disc) / PLANET_RADIUS, f = th > 1e-9 ? sin(th) / th : 1.0;
    double r = (s->radius + warp_of(s->radius) + 2.0 * TOWN_SIZE) / f;
    int stride = 1 + (int)(r / 30000.0);
    int i0 = (int)floor((s->disc.x - r) / TOWN_SIZE), i1 = (int)floor((s->disc.x + r) / TOWN_SIZE);
    int j0 = (int)floor((s->disc.z - r) / TOWN_SIZE), j1 = (int)floor((s->disc.z + r) / TOWN_SIZE);
    int n = 0;
    for (int j = j0; j <= j1; j += stride)
        for (int i = i0; i <= i1; i += stride) {
            DVec2 c = plan_town_centre(i, j);
            if (dlen(dsub(c, s->disc)) <= r && plan_town_built(i, j)) n++;
        }
    *km = 2.0 * sqrt(f * n * stride * stride * TOWN_SIZE * TOWN_SIZE / 3.14159265358979) / 1000.0;
    return n;
}

static double sphere_m(TerrainDir a, TerrainDir b) {
    TerrainDir c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return PLANET_RADIUS * atan2(sqrt(dot3(c, c)), dot3(a, b));
}

/* A settlement's footprint sampled every `step` m over its own chart, about
 * its disc (the megacity's chart is the world's): how many samples lie on the
 * sea, how many in a lake and the nearest any comes to a lake's shore (m),
 * and its area (m^2). The coast is cut on a grid (plan_world.c, CUT_CELL), so
 * between its nodes in the lowlands the interpolated Earth may dip a little
 * below the sea; deeper than WET_DIP_M is the sea. */
#define WET_DIP_M 1.0
static void footprint_sample(int k, double step, int *wet, int *lake, double *lake_near, double *area) {
    PlanSettlement s = plan_settlement(k);
    TerrainChart wc = terrain_world_chart();
    double reach = s.radius + warp_of(s.radius), ou = k ? 0.0 : s.disc.x, ov = k ? 0.0 : s.disc.z;
    int n = (int)ceil(reach / step);
    *wet = *lake = 0;
    *lake_near = 1e30;
    *area = 0.0;
    for (int j = -n; j <= n; ++j)
        for (int i = -n; i <= n; ++i) {
            double u = i * step, v = j * step, x, z;
            if (hypot(u, v) > reach) continue;
            TerrainDir d = terrain_chart_dir(&s.chart, ou + u, ov + v);
            terrain_dir_chart(&wc, d, &x, &z);
            if (plan_city_edge(x, z) >= 0.0 || plan_settlement_near(x, z) != k) continue;
            *area += step * step;
            double sh = terrain_shore(d, 500.0);
            if (sh < -WET_DIP_M) (*wet)++;
            double ls = terrain_earth_shore(d);
            if (ls < 0.0) (*lake)++;
            if (ls < *lake_near) *lake_near = ls;
        }
}

/* Where its footprint's disc stands, in the planet frame. */
static TerrainDir disc_dir(int k) {
    PlanSettlement s = plan_settlement(k);
    TerrainChart wc = terrain_world_chart();
    return k ? s.dir : terrain_chart_dir(&wc, s.disc.x, s.disc.z);
}

static void test_settlements(void) {
    const int ns = plan_settlement_count();
    PlanSettlement home = plan_settlement(0);
    DVec2 c0 = plan_town_centre(0, 0);
    TerrainChart wc = terrain_world_chart();
    CHECK(home.centre.x == c0.x && home.centre.z == c0.z && home.relief_m == plan_settlement_relief(home.radius)
          && home.colossal && home.town[0] == 0 && home.town[1] == 0, "settlement 0's core isn't town (0, 0)");
    CHECK(plan_home_edge(c0.x, c0.z) <= -(CITY_BELT + CITY_FRINGE), "the megacity's core is only %.0f m inside its edge", -plan_home_edge(c0.x, c0.z));
    CHECK(memcmp(&home.chart, &wc, sizeof wc) == 0, "settlement 0's chart isn't the world's");
    CHECK(!terrain_earth_loaded() || home.rank > 0, "no city of the list is the megacity, on the Earth");
    printf("settlements: %d; the megacity is %s (rank %d)\n", ns, plan_settlement_name(0), home.rank);
    int colossal = 0, continents[64] = { 0 }, inReach = 0, roaded = 0, wetAll = 0, clipped = 0, grown = 0, lakeAll = 0, lakeNearK = 0;
    double lakeNear = 1e30;
    double gapMin = 1e300, gradeMax = 0.0, gradeSum = 0.0, keptMin = 1e300, farthest = 0.0;
    double kmLo = 1e9, kmHi = 0.0, sizeLo = 1e9, sizeHi = 0.0;
    int keptMinK = 0, gradeMaxK = 0, kmLoK = 0, shrunk = 0;
    for (int k = 0; k < ns; ++k) {
        PlanSettlement s = plan_settlement(k);
        colossal += s.colossal;
        if (k) {
            CHECK(s.colossal == (s.rank <= PLAN_COLOSSAL_RANK), "settlement %d (rank %d): colossal %d", k, s.rank, s.colossal);
            CHECK(k == 1 || s.rank > plan_settlement(k - 1).rank, "settlement %d (rank %d) out of rank order", k, s.rank);
            CHECK(s.disc.x == s.centre.x && s.disc.z == s.centre.z, "settlement %d's disc isn't about its centre", k);
        }
        CHECK(s.size_km >= PLAN_CITY_MIN_KM - 1e-9 && s.size_km <= PLAN_CITY_MAX_KM + 1e-9, "settlement %d is %.2f km", k, s.size_km);
        sizeLo = fmin(sizeLo, s.size_km);
        sizeHi = fmax(sizeHi, s.size_km);
        /* the bigger the real city, the bigger it is built */
        for (int q = 0; q < k; ++q) {
            PlanSettlement o = plan_settlement(q);
            CHECK(!(s.area_km2 > o.area_km2 && s.size_km < o.size_km) && !(s.area_km2 < o.area_km2 && s.size_km > o.size_km),
                  "%s (%.0f km2) is built %.1f km across, %s (%.0f km2) %.1f km", plan_settlement_name(k), s.area_km2, s.size_km,
                  plan_settlement_name(q), o.area_km2, o.size_km);
        }
        double r0 = plan_settlement_radius(s.size_km);
        CHECK(s.radius <= 2.0 * r0 + 1e-6 && s.radius >= 3000.0, "settlement %d's radius %.0f m, for %.1f km", k, s.radius, s.size_km);
        grown += s.radius > r0 + 1.0;
        shrunk += s.radius < r0 - 1.0;
        DVec2 tc = plan_town_centre(s.town[0], s.town[1]);
        CHECK(tc.x == s.centre.x && tc.z == s.centre.z && plan_town_regional(s.town[0], s.town[1]), "settlement %d's centre isn't its regional town's", k);
        TerrainDir dc = terrain_chart_dir(&wc, s.centre.x, s.centre.z);
        CHECK(sphere_m(dc, s.dir) < 1e-3 && (k == 0 || sphere_m(s.chart.o, s.dir) < 1e-3), "settlement %d's direction or chart isn't its centre's", k);
        farthest = fmax(farthest, sphere_m(s.dir, home.dir));
        /* the Earth's tilt under it, as coarsely as its ground reads it */
        double sea = terrain_sea_margin_dir(s.dir), tilt = 0.0;
        for (int q = 0; k && q < 16; ++q) {
            double rr = s.radius + 2000.0, a = 6.283185307179586 * q / 16.0;
            tilt = fmax(tilt, fabs(terrain_sea_margin_dir(terrain_chart_dir(&s.chart, rr * cos(a), rr * sin(a))) - sea) / rr);
        }
        if (tilt > gradeMax) { gradeMax = tilt; gradeMaxK = k; }
        gradeSum += tilt;
        if (dlen(s.centre) <= PLAN_ROAD_REACH - CITY_RADIUS) { inReach++; roaded += s.continent > 0; }
        else CHECK(s.continent == 0, "settlement %d lies past the roads' reach, but on continent %d", k, s.continent);
        CHECK(k || s.continent > 0, "the megacity isn't on a continent");
        if (s.continent > 0 && s.continent < 64) continents[s.continent]++;
        CHECK(plan_city_edge(s.centre.x, s.centre.z) < -CITY_BELT && plan_settlement_near(s.centre.x, s.centre.z) == k,
              "the edge measure doesn't find settlement %d (edge %.0f, near %d)", k, plan_city_edge(s.centre.x, s.centre.z),
              plan_settlement_near(s.centre.x, s.centre.z));
        for (int q = 0; q < k; ++q) {
            PlanSettlement o = plan_settlement(q);
            gapMin = fmin(gapMin, sphere_m(disc_dir(k), disc_dir(q)) - s.radius - o.radius - 2.0 * fmax(warp_of(s.radius), warp_of(o.radius)));
        }
        /* on land, clear of the coast and the lakes, and as big as its area makes it */
        int wet, lake;
        double area, lake_near;
        footprint_sample(k, 1000.0, &wet, &lake, &lake_near, &area);
        lakeAll += lake;
        if (lake_near < lakeNear) { lakeNear = lake_near; lakeNearK = k; }
        CHECK(lake == 0, "%d samples of settlement %d (%s) in a lake", lake, k, plan_settlement_name(k));
        double kept = area / (3.14159265358979 * r0 * r0);
        if (kept < keptMin) { keptMin = kept; keptMinK = k; }
        clipped += kept < 0.97;
        wetAll += wet;
        CHECK(wet == 0, "%d samples of settlement %d (%s) on the sea or its margin", wet, k, plan_settlement_name(k));
        double km;
        built_towns(&s, &km);
        if (s.radius >= r0 - 1.0 && km < kmLo) { kmLo = km; kmLoK = k; }
        kmHi = fmax(kmHi, km);
    }
    int nc = 0, biggest = 0;
    for (int k = 1; k < 64; ++k) if (continents[k]) { nc++; biggest = continents[k] > biggest ? continents[k] : biggest; }
    printf("  %d colossal, %d megacities; the farthest %.0f km from the megacity\n", colossal, ns - colossal, farthest / 1000.0);
    printf("  %d within the roads' reach, %d of them on %d continents (the largest holds %d)\n", inReach, roaded, nc, biggest);
    printf("  footprints at least %.1f km apart (warps allowed for); the Earth tilts under them %.2f%% at most (%s), %.2f%% on average\n",
           gapMin / 1000.0, 100.0 * gradeMax, plan_settlement_name(gradeMaxK), 100.0 * gradeSum / fmax(ns - 1, 1));
    printf("  %d short of a disc of their size by 3%% or more, %d grown to keep their area, %d shrunk for a neighbour; the least kept %.0f%% (%s)\n",
           clipped, grown, shrunk, 100.0 * keptMin, plan_settlement_name(keptMinK));
    printf("  sized %.1f .. %.1f km across; built across (from their towns' area, but the shrunk) %.1f (%s) .. %.1f km\n",
           sizeLo, sizeHi, kmLo, plan_settlement_name(kmLoK), kmHi);
    CHECK(gapMin > 0.0, "two footprints only %.1f km apart, warps allowed for", gapMin / 1000.0);
    CHECK(wetAll == 0, "%d footprint samples on the sea or within its margin", wetAll);
    if (lakeNear < 1e29) printf("  footprints come within %.0f m of a lake's shore at the nearest (%s; %d samples in one)\n", lakeNear, plan_settlement_name(lakeNearK), lakeAll);
    CHECK(kmLo >= 0.8 * PLAN_CITY_MIN_KM && kmHi <= 1.4 * PLAN_CITY_MAX_KM, "cities built %.1f .. %.1f km across", kmLo, kmHi);

    /* The edge measure: capped, continuous, and no faster than CITY_EDGE_RATE, near the settlements too. */
    Rng r = rng_make(77);
    double rateMax = 0.0, eMax = -1e300;
    for (int k = 0; k < 40000; ++k) {
        PlanSettlement s = plan_settlement(rng_int(&r, 0, ns - 1));
        float a = rng_float(&r), b = rng_float(&r), u = rng_float(&r), v = rng_float(&r);
        double rad = (s.radius + PLAN_EDGE_CAP + 5000.0) * sqrt(a), th = 6.2831853 * b;
        double x = s.disc.x + rad * cos(th), z = s.disc.z + rad * sin(th);
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
    int fwAt[PLAN_MAX_SETTLEMENTS] = { 0 }, ns = plan_settlement_count();
    /* the sea: where no road may go and the Earth's shore is more than
     * WET_DIP_M below it (the narrow straits the bake barely resolves, such as
     * New York's Narrows, dip less: the bridges are to come) */
    TerrainChart wc = terrain_world_chart();
    for (int k = 0; k < nn; ++k) parent[k] = k;
    double total = 0.0, freeway = 0.0, detourMax = 0.0, seaMin = 1e300;
    int offLand = 0, inCity = 0, badEnds = 0, gates = 0, badGates = 0;
    double gradeOver = 0.0;
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
        for (int end = 0; end < 2; ++end) {
            PlanNode nd = end ? b : a;
            DVec2 e = R->pts[end ? R->npts - 1 : 0];
            if (nd.settlement < 0) { badEnds += dlen(dsub(e, nd.p)) > 1.0 || R->gate[end]; continue; }
            PlanGate g = plan_road_gate(r, end);
            if (!R->gate[end] || dlen(dsub(e, g.p)) > 1e-6) { badEnds++; continue; }
            /* on the boundary: built towns behind it, not in front */
            int ii, ij, oi, oj;
            DVec2 in = dsub(g.p, dscale(g.n, 2.0)), out = dadd(g.p, dscale(g.n, 2.0));
            plan_town_at(in.x, in.z, &ii, &ij);
            plan_town_at(out.x, out.z, &oi, &oj);
            gates++;
            if (!plan_town_built(ii, ij) || plan_town_built(oi, oj)) badGates++;
            if (fabs(R->prof[end ? R->nprof - 1 : 0] - terrain_height(g.p.x, g.p.z)) > 1e-3) badGates++;
        }
        double G = R->cls == PLAN_ROAD_FREEWAY ? PLAN_FREEWAY_GRADE : PLAN_TRUNK_GRADE;
        for (int k = 0; k + 1 < R->nprof; ++k) gradeOver = fmax(gradeOver, fabs(R->prof[k + 1] - R->prof[k]) / R->ds / G);
        double straight = dlen(dsub(b.p, a.p));
        if (straight > 50000.0) detourMax = fmax(detourMax, len / straight);
        for (int k = 0; k + 1 < R->npts; ++k)
            for (int q = 0; q < 4; ++q) {
                DVec2 p = dadd(R->pts[k], dscale(dsub(R->pts[k + 1], R->pts[k]), q / 4.0));
                double sea = plan_world_sea(p.x, p.z);
                seaMin = fmin(seaMin, sea);
                if (sea <= 0.0 && !plan_road_dry(p.x, p.z) && terrain_shore(terrain_chart_dir(&wc, p.x, p.z), 2000.0) < -WET_DIP_M) offLand++;
                if (plan_city_edge(p.x, p.z) < 0.0) {
                    int near = plan_settlement_near(p.x, p.z);
                    if (near != a.settlement && near != b.settlement) inCity++;
                }
            }
    }
    int junctions = 0, deadJunctions = 0, splitContinents = 0, colossalNoFw = 0, apart[2] = { -1, -1 };
    for (int k = 0; k < nn; ++k)
        if (plan_node(k).settlement < 0) {
            junctions++;
            if (deg[k] < 3) deadJunctions++;
        }
    for (int s = 0; s < ns; ++s) {
        for (int t = s + 1; t < ns && plan_settlement(s).continent > 0; ++t)
            if (plan_settlement(s).continent == plan_settlement(t).continent && uf_find(parent, s) != uf_find(parent, t)) {
                if (!splitContinents++) { apart[0] = s; apart[1] = t; }
                break;
            }
        if (plan_settlement(s).colossal && deg[s] && !fwAt[s]) colossalNoFw++;   /* alone on its island, it has no roads */
    }
    printf("roads: %d between %d nodes (%d junctions), %.0f km in all, %.0f%% freeway; the worst detour %.2f times the straight line\n",
           nr, nn, junctions, total / 1000.0, 100.0 * freeway / total, detourMax);
    printf("  the lowest point %.0f m above the sea to come; settlement 0 has %d roads out\n", seaMin, deg[0]);
    printf("  %d gates; the steepest grade %.2f of its class's\n", gates, gradeOver);
    CHECK(badGates == 0, "%d gates off the built towns' boundary or the city's ground", badGates);
    CHECK(gradeOver <= 1.01, "a road %.2f times as steep as its class allows", gradeOver);
    CHECK(splitContinents == 0, "%d settlements not joined to the rest of their continent (%s and %s, %d and %d roads; the second at %.0f %.0f)",
          splitContinents, plan_settlement_name(apart[0]), plan_settlement_name(apart[1]), apart[0] >= 0 ? deg[apart[0]] : 0,
          apart[1] >= 0 ? deg[apart[1]] : 0, plan_settlement(apart[1]).centre.x, plan_settlement(apart[1]).centre.z);
    CHECK(offLand == 0, "%d points of road where the sea will be", offLand);
    CHECK(inCity == 0, "%d points of road inside other settlements", inCity);
    CHECK(badEnds == 0, "%d roads not ending at their nodes", badEnds);
    CHECK(deadJunctions == 0, "%d junctions where fewer than three roads meet", deadJunctions);
    CHECK(colossalNoFw == 0, "%d colossal cities without a freeway", colossalNoFw);
    CHECK(freeway > 0.25 * total && freeway < 0.7 * total, "freeways %.0f%% of the network", 100.0 * freeway / total);
    /* on the Earth, by the sea with its neighbours all one way, its roads may share one way out */
    CHECK(deg[0] >= (terrain_earth_loaded() ? 1 : 2), "only %d roads out of the megacity", deg[0]);
    /* views out along the megacity's roads, for NeonCity --cam: from the boulevard, and from 150 m up behind it */
    for (int r = 0; r < nr; ++r)
        for (int end = 0; end < 2; ++end) {
            const PlanRoad *R = plan_road(r);
            if (!R->gate[end] || plan_node(end ? R->to : R->from).settlement != 0) continue;
            PlanGate g = plan_road_gate(r, end);
            double yaw = atan2(g.n.x, -g.n.z) * 57.29577951;
            if (yaw < 0.0) yaw += 360.0;
            printf("  road %d out of the megacity (%s): --cam %.0f %.0f %.0f 0 4 / --cam %.0f %.0f %.0f -12 150\n", r,
                   R->cls == PLAN_ROAD_FREEWAY ? "freeway" : "trunk road", g.p.x - 20.0 * g.n.x, g.p.z - 20.0 * g.n.z, yaw,
                   g.p.x - 600.0 * g.n.x, g.p.z - 600.0 * g.n.z, yaw);
            /* and 15 km out along it, beside it, looking on along it */
            double s = end ? R->s[R->npts - 1] - 15000.0 : 15000.0, dir = end ? -1.0 : 1.0;
            int sg = 0;
            while (sg + 2 < R->npts && R->s[sg + 1] < s) sg++;
            DVec2 a = R->pts[sg], d = dsub(R->pts[sg + 1], a);
            double t = (s - R->s[sg]) / fmax(R->s[sg + 1] - R->s[sg], 1e-9);
            DVec2 p = dadd(a, dscale(d, t)), u = dscale(dnorm(d), dir);
            double ay = atan2(u.x, -u.z) * 57.29577951;
            printf("    15 km out: --cam %.0f %.0f %.0f -8 %.0f\n", p.x - 30.0 * u.z, p.z + 30.0 * u.x, ay < 0.0 ? ay + 360.0 : ay,
                   plan_road_height(r, s) + 40.0);
        }
    free(parent);
    free(deg);
}

/* world_test --list: each settlement's rank, name, real area, size, radius
 * against its size's, the share of a disc of its size it covers and how far
 * its disc stands off its core, and which way (degrees east of north). */
static int list_settlements(void) {
    printf("  k rank %-28s %8s %7s %8s %6s %6s %7s %5s\n", "name", "km2", "km", "radius", "of r0", "kept", "off km", "way");
    for (int k = 0; k < plan_settlement_count(); ++k) {
        PlanSettlement s = plan_settlement(k);
        double r0 = plan_settlement_radius(s.size_km), area;
        DVec2 off = dsub(s.disc, s.centre);
        double way = fmod(atan2(off.x, -off.z) * 57.29577951308232 + 360.0, 360.0);   /* the chart's x east, z south */
        int wet, lake;
        double lake_near;
        footprint_sample(k, 1000.0, &wet, &lake, &lake_near, &area);
        printf("%3d %4d %-28.28s %8.0f %7.1f %8.0f %6.2f %6.2f %7.1f %5.0f\n", k, s.rank, plan_settlement_name(k), s.area_km2, s.size_km,
               s.radius, s.radius / r0, area / (3.14159265358979 * r0 * r0), dlen(off) / 1000.0, way);
    }
    return 0;
}

static int fingerprint(void) {
    uint64_t hs = 0xcbf29ce484222325ULL;
    for (int k = 0; k < plan_settlement_count(); ++k) {
        PlanSettlement s = plan_settlement(k);
        double v[7] = { s.centre.x, s.centre.z, s.radius, s.relief_m, s.disc.x, s.disc.z, s.size_km };
        int iv[5] = { s.town[0], s.town[1], s.colossal, s.continent, s.rank };
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
        if (mc->relief) {   /* the settlements' footprints, the ground showing through */
            double e = plan_city_edge(x, z);
            float u = e <= -CITY_BELT ? 0.6f : e < 0.0 ? 0.6f * (float)(-e / CITY_BELT) : 0.0f;
            static const float city[3] = { 0.9f, 0.3f, 0.8f };
            for (int q = 0; q < 3; ++q) col[q] += u * (city[q] - col[q]);
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
    for (int k = plan_settlement_count() - 1; k >= 0; --k) {
        PlanSettlement s = plan_settlement(k);
        double px = (s.centre.x - mc.x0) / mc.m, py = (s.centre.z - mc.z0) / mc.m;
        double size = (s.colossal ? 4.0 : 2.5) * fmin(scale, 1.5) + 0.5;
        disc(&im, px, py, relief ? size : fmax(size, s.radius / mc.m), k == 0 ? white : s.colossal ? red : orange);
    }
    int rc = save(&im, path);
    printf("map, %d px, %.2f km a pixel\n", W, mc.m / 1000.0);
    free(im.px);
    return rc;
}

/* The planet, longitude across from 180 W and latitude up, W px by W / 2:
 * the Earth's land shaded by its height and the sea to come blue, the roads
 * (freeways red, trunk roads yellow) and the settlements (colossal red, the
 * megacities orange, the megacity white). */
typedef struct PlanetCtx { Image *im; } PlanetCtx;

static void planet_row(void *ctx, int j) {
    PlanetCtx *pc = (PlanetCtx *)ctx;
    double lat = 90.0 - (j + 0.5) * 180.0 / pc->im->h;
    for (int i = 0; i < pc->im->w; ++i) {
        double lon = -180.0 + (i + 0.5) * 360.0 / pc->im->w;
        double h = terrain_shore(terrain_latlon_dir(lat, lon), 20000.0);
        float col[3];
        if (h < 0.0) {
            float d = clampf((float)(-h / 6000.0), 0.0f, 1.0f);
            col[0] = 0.10f - 0.06f * d; col[1] = 0.22f - 0.12f * d; col[2] = 0.45f - 0.2f * d;
        } else {
            float t = clampf((float)(h / 3000.0), 0.0f, 1.0f);
            col[0] = 0.35f + 0.45f * t; col[1] = 0.55f + 0.15f * t; col[2] = 0.3f + 0.4f * t;
        }
        put(pc->im, i, j, col);
    }
}

static void planet_xy(const Image *im, TerrainDir d, double *x, double *y) {
    double lat, lon;
    terrain_dir_latlon(d, &lat, &lon);
    *x = (lon + 180.0) / 360.0 * im->w;
    *y = (90.0 - lat) / 180.0 * im->h;
}

static int draw_planet(const char *path, int W, int threads) {
    Image im = { W, W / 2, (uint8_t *)calloc((size_t)W * (W / 2) * 3, 1) };
    if (!im.px) return 1;
    PlanetCtx pc = { &im };
    thread_parallel_for(im.h, threads, planet_row, &pc);
    TerrainChart wc = terrain_world_chart();
    const float red[3] = { 0.95f, 0.15f, 0.1f }, orange[3] = { 1.0f, 0.65f, 0.1f }, white[3] = { 1.0f, 1.0f, 1.0f };
    const float fw[3] = { 0.9f, 0.1f, 0.1f }, trunk[3] = { 1.0f, 0.9f, 0.2f };
    double scale = W / 1600.0;
    for (int pass = 0; pass < 2; ++pass)
        for (int r = 0; r < plan_road_count(); ++r) {
            const PlanRoad *R = plan_road(r);
            if ((R->cls == PLAN_ROAD_FREEWAY) != pass) continue;
            double px = 0.0, py = 0.0;
            for (int k = 0; k < R->npts; k += 8) {
                double x, y;
                planet_xy(&im, terrain_chart_dir(&wc, R->pts[k].x, R->pts[k].z), &x, &y);
                if (k && fabs(x - px) < 0.5 * im.w) line(&im, px, py, x, y, 0.6 * fmin(scale, 1.5), pass ? fw : trunk);
                px = x; py = y;
            }
        }
    for (int k = plan_settlement_count() - 1; k >= 0; --k) {
        PlanSettlement s = plan_settlement(k);
        double x, y;
        planet_xy(&im, s.dir, &x, &y);
        disc(&im, x, y, (k == 0 ? 3.5 : s.colossal ? 2.8 : 1.8) * fmin(scale, 2.0), k == 0 ? white : s.colossal ? red : orange);
    }
    int rc = save(&im, path);
    printf("planet map, %d x %d px\n", im.w, im.h);
    free(im.px);
    return rc;
}

/* Whether another road's centre line, or this one's further along (a hairpin),
 * passes within both roads' reach of p, s along R, where the ground takes
 * whichever weighs more there. */
static int road_beside(const PlanRoad *R, DVec2 p, double s) {
    const double reach = 2.0 * (PLAN_FREEWAY_HALF + PLAN_ROAD_SHOULDER + PLAN_ROAD_BATTER_MAX);
    for (int q = 0; q < plan_road_count(); ++q) {
        const PlanRoad *O = plan_road(q);
        for (int k = 0; k + 1 < O->npts; ++k) {
            if (O == R && O->s[k + 1] > s - 4.0 * reach && O->s[k] < s + 4.0 * reach) continue;
            DVec2 a = O->pts[k], d = dsub(O->pts[k + 1], a);
            double t = fmin(fmax(ddot(dsub(p, a), d) / fmax(ddot(d, d), 1e-9), 0.0), 1.0);
            if (dlen(dsub(p, dadd(a, dscale(d, t)))) < reach) return 1;
        }
    }
    return 0;
}

/* The ground with the roads in it: the road's surface along its centre, and
 * a slope no steeper than a batter across it. */
static void test_road_ground(void) {
    Rng r = rng_make(99);
    double offCentre = 0.0, steepest = 0.0;
    int n = 0, beside = 0;
    for (int k = 0; k < 4000; ++k) {
        const PlanRoad *R = plan_road(rng_int(&r, 0, plan_road_count() - 1));
        float u = rng_float(&r), v = rng_float(&r);
        double s = u * R->s[R->npts - 1];
        int sg = 0;
        while (sg + 2 < R->npts && R->s[sg + 1] < s) sg++;
        double t = (s - R->s[sg]) / fmax(R->s[sg + 1] - R->s[sg], 1e-9);
        DVec2 a = R->pts[sg], d = dsub(R->pts[sg + 1], a), p = dadd(a, dscale(d, t)), nrm = dnorm(dv(-d.z, d.x));
        if (plan_urban(p.x, p.z) > 0.0f) continue;   /* the city's ground stands there */
        double h = terrain_height(p.x, p.z), rh = plan_road_height((int)(R - plan_road(0)), s);
        /* where two roads' reaches overlap the higher weight wins: skip near
         * junctions, and count where another road runs beside this one */
        int nearNode = 0;
        for (int e = 0; e < 2; ++e) nearNode |= dlen(dsub(p, plan_node(e ? R->to : R->from).p)) < 2000.0 || s < 2000.0 || s > R->s[R->npts - 1] - 2000.0;
        if (nearNode) continue;
        if (road_beside(R, p, s)) { beside++; continue; }
        offCentre = fmax(offCentre, fabs(h - rh));
        /* across: out to the batter's farthest */
        double off = (2.0 * v - 1.0) * (R->half + PLAN_ROAD_SHOULDER + PLAN_ROAD_BATTER_MAX);
        DVec2 q = dadd(p, dscale(nrm, off)), q2 = dadd(q, dscale(nrm, 0.5));
        steepest = fmax(steepest, fabs(terrain_height(q2.x, q2.z) - terrain_height(q.x, q.z)) / 0.5);
        n++;
    }
    printf("  the ground on %d points along the roads: within %.3f m of the road's surface; across, at most %.2f m/m"
           " (%d more beside another road or itself, away from a junction)\n", n, offCentre, steepest, beside);
    /* How far the roads stand above or below the bare land: the cuttings and embankments (bridges and tunnels to come) */
    double total = 0.0, over10 = 0.0, over30 = 0.0, deepest = 0.0;
    for (int r = 0; r < plan_road_count(); ++r) {
        const PlanRoad *R = plan_road(r);
        for (int k = 0; k < R->nprof; k += 4) {
            double s = k * R->ds;
            int sg = 0;
            while (sg + 2 < R->npts && R->s[sg + 1] < s) sg++;
            double t = (s - R->s[sg]) / fmax(R->s[sg + 1] - R->s[sg], 1e-9);
            DVec2 p = dadd(R->pts[sg], dscale(dsub(R->pts[sg + 1], R->pts[sg]), fmin(fmax(t, 0.0), 1.0)));
            if (plan_urban(p.x, p.z) > 0.0f) continue;
            double d = fabs(R->prof[k] - terrain_height_bare(p.x, p.z, 0.0)), step = 4.0 * R->ds;
            total += step;
            if (d > 10.0) over10 += step;
            if (d > 30.0) over30 += step;
            deepest = fmax(deepest, d);
        }
    }
    printf("  cut or fill: over 10 m on %.1f%% of the roads, over 30 m on %.2f%%, at most %.0f m\n", 100.0 * over10 / total, 100.0 * over30 / total, deepest);
    CHECK(offCentre <= 0.01, "the ground strays %.3f m from a road's surface", offCentre);
}

int main(int argc, char **argv) {
    int threads = cpu_count();
    if (argc > 2 && strcmp(argv[1], "--threads") == 0) {   /* the world must come out the same on any number */
        threads = atoi(argv[2]);
        argv += 2;
        argc -= 2;
    }
    if (argc > 2 && strcmp(argv[1], "--earth") == 0) {   /* the Earth's bake; without one, the procedural stand-in */
        if (!terrain_earth_load(argv[2])) { printf("FAIL: the bake %s didn't load\n", argv[2]); return 1; }
        argv += 2;
        argc -= 2;
    }
    clock_t t0 = clock();
    plan_world_init(threads);
    printf("world built on %d threads in %.2f s\n", threads, (double)(clock() - t0) / CLOCKS_PER_SEC);
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();
    if (argc > 1 && strcmp(argv[1], "--list") == 0) return list_settlements();
    if (argc > 2 && strcmp(argv[1], "--map") == 0) return draw_planet(argv[2], argc > 3 ? atoi(argv[3]) : 1600, threads);
    if (argc > 2 && strcmp(argv[1], "--chart") == 0) return draw_map(argv[2], argc > 3 ? atoi(argv[3]) : 1600, 0.0, 0.0, PLAN_ROAD_REACH * 1.08, 0, threads);
    if (argc > 2 && strcmp(argv[1], "--local") == 0) {
        DVec2 c = plan_town_centre(0, 0);
        double km = argc > 3 ? atof(argv[3]) : 300.0;
        if (argc > 5) c = dv(atof(argv[4]), atof(argv[5]));
        return draw_map(argv[2], 1200, c.x, c.z, km * 500.0, 1, threads);
    }

    test_settlements();
    test_roads();
    test_road_ground();

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all world tests passed\n");
    return 0;
}
