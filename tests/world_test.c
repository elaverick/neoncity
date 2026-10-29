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
 * up), world_test --chart out.ppm [px] the world chart within 5400 km,
 * world_test --local out.ppm [km [x z]] the land, the footprints and the roads
 * round a point (the megacity by default); world_test --list lists the
 * settlements, their areas, sizes and how much of a disc of their size each
 * keeps; world_test --neighbours lists the pairs of cities near each other
 * and how near each comes in the other's frame (test_neighbours).
 * world_test [--threads n] --earth <bake> ... builds the world on the Earth's
 * bake (src/terrain/terrain_earth.c); without one, on the procedural stand-in. */
#include "plan.h"
#include "terrain_internal.h"
#include "city.h"
#include "hash.h"
#include "thread.h"
#include "frames.h"
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

/* Settlement k's built towns, every stride-th each way, and the size across
 * of a disc of their area (km): in its own frame, where its city is laid out
 * and its towns are true to size. */
static int built_towns(int k, const PlanSettlement *s, double *km) {
    terrain_frame_bind(plan_frame_settlement(k));
    double r = s->radius + warp_of(s->radius) + 2.0 * TOWN_SIZE;
    int stride = 1 + (int)(r / 30000.0);
    int i0 = (int)floor((s->disc.x - r) / TOWN_SIZE), i1 = (int)floor((s->disc.x + r) / TOWN_SIZE);
    int j0 = (int)floor((s->disc.z - r) / TOWN_SIZE), j1 = (int)floor((s->disc.z + r) / TOWN_SIZE);
    int n = 0;
    for (int j = j0; j <= j1; j += stride)
        for (int i = i0; i <= i1; i += stride) {
            DVec2 c = plan_town_centre(i, j);
            if (dlen(dsub(c, s->disc)) <= r && plan_town_built(i, j)) n++;
        }
    terrain_frame_bind(NULL);
    *km = 2.0 * sqrt(n * stride * stride * TOWN_SIZE * TOWN_SIZE / 3.14159265358979) / 1000.0;
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
    int colossal = 0, label[PLAN_MAX_SETTLEMENTS], held[PLAN_MAX_SETTLEMENTS], nc = 0, roaded = 0, wetAll = 0, clipped = 0, grown = 0, lakeAll = 0, lakeNearK = 0;
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
        terrain_frame_bind(plan_frame_settlement(k));   /* its towns are its frame's */
        CHECK(tc.x == s.centre.x && tc.z == s.centre.z && plan_town_regional(s.town[0], s.town[1]), "settlement %d's centre isn't its regional town's", k);
        TerrainDir fc = terrain_world_dir(s.centre.x, s.centre.z);
        CHECK(sphere_m(fc, s.dir) < 1e-3 && plan_settlement_near(s.centre.x, s.centre.z) == k && plan_built_edge(s.centre.x, s.centre.z) < -CITY_BELT,
              "settlement %d's frame doesn't hold it at its centre", k);
        terrain_frame_bind(NULL);
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
        roaded += s.continent > 0;
        CHECK(k || s.continent > 0, "the megacity isn't on a continent");
        if (s.continent > 0) {   /* the continents by their numbers, which run into the thousands (every island) */
            int c = 0;
            while (c < nc && label[c] != s.continent) c++;
            if (c == nc) { label[nc] = s.continent; held[nc++] = 0; }
            held[c]++;
        }
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
        built_towns(k, &s, &km);
        if (s.radius >= r0 - 1.0 && km < kmLo) { kmLo = km; kmLoK = k; }
        kmHi = fmax(kmHi, km);
    }
    int biggest = 0;
    for (int c = 0; c < nc; ++c) biggest = held[c] > biggest ? held[c] : biggest;
    printf("  %d colossal, %d megacities; the farthest %.0f km from the megacity\n", colossal, ns - colossal, farthest / 1000.0);
    printf("  %d on %d continents (the largest holds %d)\n", roaded, nc, biggest);
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
    int offLand = 0, inCity = 0, inOwn = 0, badEnds = 0, gates = 0, badGates = 0;
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
            /* in the settlement's own frame, where its towns are and the gate was found */
            terrain_frame_bind(plan_frame_settlement(nd.settlement));
            const PlanRoad *Rs = plan_road(r);
            PlanGate g = plan_road_gate(r, end);
            DVec2 es = Rs->npts ? Rs->pts[end ? Rs->npts - 1 : 0] : dv(1e30, 1e30);
            if (!R->gate[end] || dlen(dsub(es, g.p)) > 1e-6) { badEnds++; terrain_frame_bind(NULL); continue; }
            /* on the boundary: built towns behind it, not in front */
            int ii, ij, oi, oj;
            DVec2 in = dsub(g.p, dscale(g.n, 2.0)), out = dadd(g.p, dscale(g.n, 2.0));
            plan_town_at(in.x, in.z, &ii, &ij);
            plan_town_at(out.x, out.z, &oi, &oj);
            gates++;
            if (!plan_town_built(ii, ij) || plan_town_built(oi, oj)) badGates++;
            if (fabs(R->prof[end ? R->nprof - 1 : 0] - terrain_height(g.p.x, g.p.z)) > 1e-3) badGates++;
            terrain_frame_bind(NULL);
        }
        double G = R->cls == PLAN_ROAD_FREEWAY ? PLAN_FREEWAY_GRADE : PLAN_TRUNK_GRADE;
        for (int k = 0; k + 1 < R->nprof; ++k) gradeOver = fmax(gradeOver, fabs(R->prof[k + 1] - R->prof[k]) / R->ds / G);
        double straight = sphere_m(a.dir, b.dir);
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
                    /* nor back in its own city's built towns past its gate (a gate is where it leaves them
                     * for the last time): but for a corner, rounded after the gates, clipping one at its edge */
                    double sp = R->s[k] + (R->s[k + 1] - R->s[k]) * q / 4.0, len = R->s[R->npts - 1];
                    if ((near == a.settlement && R->gate[0] && sp > 1000.0) || (near == b.settlement && R->gate[1] && sp < len - 1000.0)) {
                        /* its towns are its own frame's */
                        const TerrainFrame *nf = plan_frame_settlement(near);
                        double nx, nz;
                        terrain_dir_frame(nf, terrain_world_dir(p.x, p.z), &nx, &nz);
                        terrain_frame_bind(nf);
                        int ti, tj;
                        plan_town_at(nx, nz, &ti, &tj);
                        inOwn += plan_town_built(ti, tj);
                        terrain_frame_bind(NULL);
                    }
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
    printf("  %d points of road clip their own city's built towns past their gates (corners rounded at its edge)\n", inOwn);
    CHECK(inOwn < 50, "%d points of road back in its own city's built towns past its gate", inOwn);
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
            printf("  road %d out of the megacity (%s): --cam %.0f %.0f %.0f 0 %.0f / --cam %.0f %.0f %.0f -12 %.0f\n", r,
                   R->cls == PLAN_ROAD_FREEWAY ? "freeway" : "trunk road", g.p.x - 20.0 * g.n.x, g.p.z - 20.0 * g.n.z, yaw, terrain_height(g.p.x, g.p.z) + 4.0,
                   g.p.x - 600.0 * g.n.x, g.p.z - 600.0 * g.n.z, yaw, terrain_height(g.p.x, g.p.z) + 150.0);
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
    uint64_t hb = 0xcbf29ce484222325ULL;
    for (int r = 0; r < plan_road_count(); ++r) {
        const PlanRoad *R = plan_road(r);
        for (int b = 0; b < R->nbridges; ++b) {
            const PlanRoadBridge *B = &R->bridges[b];
            double v[5] = { B->s0, B->s1, B->w0, B->w1, B->deck };
            hb = fnv(v, sizeof v, hb);
            hb = fnv(&B->lake, 1, hb);
        }
        hb = fnv(R->prof, sizeof(float) * (size_t)R->nprof, hb);
    }
    uint64_t hc = 0xcbf29ce484222325ULL;
    for (int k = 0; k < plan_channel_count(); ++k) {
        const PlanChannel *c = plan_channel(k);
        hc = fnv(c->v, sizeof(DVec2) * (size_t)c->n, hc);
        hc = fnv(c->edge, (size_t)c->n, hc);
        hc = fnv(&c->built, 1, hc);
    }
    for (int k = 0; k < plan_bridge_count(); ++k) {
        const PlanBridge *b = plan_bridge(k);
        double v[6] = { b->p.x, b->p.z, b->d.x, b->d.z, b->s0, b->s1 };
        hc = fnv(v, sizeof v, hc);
        hc = fnv(&b->road, sizeof b->road, hc);
    }
    printf("world fingerprint\n");
    printf("  settlements %016llx\n", (unsigned long long)hs);
    printf("  roads       %016llx\n", (unsigned long long)hr);
    printf("  channels    %016llx\n", (unsigned long long)hc);
    printf("  bridges     %016llx\n", (unsigned long long)hb);
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

/* The bake's rivers (as wide as they are, or a pixel) and lakes' shores over the map. */
typedef struct WaterMap { Image *im; const MapCtx *mc; TerrainChart wc; } WaterMap;
static void map_water(void *ctx, TerrainDir a, TerrainDir b, double width, int kind) {
    const WaterMap *wm = (const WaterMap *)ctx;
    if (kind != 1 && kind != 2) return;
    double ax, az, bx, bz;
    terrain_dir_chart(&wm->wc, a, &ax, &az);
    terrain_dir_chart(&wm->wc, b, &bx, &bz);
    const MapCtx *mc = wm->mc;
    double x0 = (ax - mc->x0) / mc->m, y0 = (az - mc->z0) / mc->m, x1 = (bx - mc->x0) / mc->m, y1 = (bz - mc->z0) / mc->m;
    if (fmax(x0, x1) < -2.0 || fmin(x0, x1) > wm->im->w + 2.0 || fmax(y0, y1) < -2.0 || fmin(y0, y1) > wm->im->h + 2.0) return;
    const float water[3] = { 0.2f, 0.6f, 1.0f };
    line(wm->im, x0, y0, x1, y1, fmax(0.5 * width / mc->m, 0.5), water);
}

/* The square of side `ext` * 2 round (cx, cz), W px square, north (-z) up:
 * the land and the sea to come, the bake's rivers and lakes (blue), the
 * roads (freeways red, trunk roads yellow, their bridges white, junctions
 * white), the settlements (colossal red, the megacities orange, the megacity
 * white), each as its footprint where that is bigger than a dot. */
static int draw_map(const char *path, int W, double cx, double cz, double ext, int relief, int threads) {
    Image im = { W, W, (uint8_t *)calloc((size_t)W * W * 3, 1) };
    if (!im.px) return 1;
    MapCtx mc = { &im, cx - ext, cz - ext, 2.0 * ext / W, relief };
    thread_parallel_for(W, threads, map_row, &mc);
    WaterMap wm = { &im, &mc, terrain_world_chart() };
    terrain_earth_water_segments(map_water, &wm);
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
                     (R->pts[k + 1].z - mc.z0) / mc.m, w, R->bridge && R->bridge[k] >= 0 ? white : pass ? fw : trunk);
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

/* Road R's bridge whose span holds s (widened by `pad`), or -1. */
static int road_bridge_at(const PlanRoad *R, double s, double pad) {
    for (int b = 0; b < R->nbridges; ++b)
        if (s >= R->bridges[b].s0 - pad && s <= R->bridges[b].s1 + pad) return b;
    return -1;
}

/* The ground with the roads in it: the road's surface along its centre (but
 * under a bridge), and a slope no steeper than a batter across it. */
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
        if (road_bridge_at(R, s, 1.0) >= 0) continue;   /* the water's, under a bridge */
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
    /* How far the roads stand above or below the bare land: the cuttings and embankments, off the bridges (tunnels to come) */
    double total = 0.0, over10 = 0.0, over30 = 0.0, deepest = 0.0;
    for (int r = 0; r < plan_road_count(); ++r) {
        const PlanRoad *R = plan_road(r);
        for (int k = 0; k < R->nprof; k += 4) {
            double s = k * R->ds;
            int sg = 0;
            while (sg + 2 < R->npts && R->s[sg + 1] < s) sg++;
            double t = (s - R->s[sg]) / fmax(R->s[sg + 1] - R->s[sg], 1e-9);
            DVec2 p = dadd(R->pts[sg], dscale(dsub(R->pts[sg + 1], R->pts[sg]), fmin(fmax(t, 0.0), 1.0)));
            if (plan_urban(p.x, p.z) > 0.0f || road_bridge_at(R, s, 0.0) >= 0) continue;
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

/* --- The roads' bridges ----------------------------------------------------------------- */

/* Along each road every BRIDGE_PROBE metres outside the cities: where the
 * point is in a river or a lake (not a stream: a culvert), it is on a
 * bridge's water stretch; and over every water stretch the road stands at its
 * deck's clearance and over the ground, which is the water's. */
#define BRIDGE_PROBE 25.0
typedef struct BridgeProbe { int wet, culverts, low, buried, beside; double over; } BridgeProbe;
static void bridge_probe_job(void *ctx, int r) {
    BridgeProbe *out = &((BridgeProbe *)ctx)[r];
    const PlanRoad *R = plan_road(r);
    TerrainChart wc = terrain_world_chart();
    memset(out, 0, sizeof *out);
    for (int sg = 0; sg + 1 < R->npts; ++sg) {
        double l = R->s[sg + 1] - R->s[sg];
        for (double u = fmod(BRIDGE_PROBE - fmod(R->s[sg], BRIDGE_PROBE), BRIDGE_PROBE); u < l; u += BRIDGE_PROBE) {
            double s = R->s[sg] + u;
            DVec2 p = dadd(R->pts[sg], dscale(dsub(R->pts[sg + 1], R->pts[sg]), u / l));
            if (plan_urban(p.x, p.z) > 0.0f) continue;
            int b = R->bridge ? R->bridge[sg] : -1;
            TerrainEarthWater w;
            if (b < 0 && terrain_earth_water(terrain_chart_dir(&wc, p.x, p.z), &w) && w.edge < 0.0) {
                if (w.stream) out->culverts++;
                else out->wet++;
            }
            if (b < 0) continue;
            const PlanRoadBridge *B = &R->bridges[b];
            double h = plan_road_height(r, s), g = terrain_height(p.x, p.z);
            if (s >= B->w0 && s <= B->w1) {
                if (h < B->deck - 0.01) out->low++;
                out->over = fmax(out->over, h - g);
            }
            if (g > h + 0.01) {   /* the deck in the ground: but where another road beside it weighs more */
                if (road_beside(R, p, s)) out->beside++;
                else out->buried++;
            }
        }
    }
}

static void test_road_bridges(int threads) {
    int nr = plan_road_count(), nb = 0, lakes = 0;
    double span = 0.0, longest = 0.0;
    int lr = -1, lb = -1;
    for (int r = 0; r < nr; ++r) {
        const PlanRoad *R = plan_road(r);
        for (int b = 0; b < R->nbridges; ++b) {
            const PlanRoadBridge *B = &R->bridges[b];
            nb++;
            lakes += B->lake;
            span += B->s1 - B->s0;
            if (B->s1 - B->s0 > longest) { longest = B->s1 - B->s0; lr = r; lb = b; }
        }
    }
    BridgeProbe *pr = (BridgeProbe *)calloc((size_t)(nr > 0 ? nr : 1), sizeof(BridgeProbe));
    if (!pr) return;
    thread_parallel_for(nr, threads, bridge_probe_job, pr);
    BridgeProbe all = { 0 };
    for (int r = 0; r < nr; ++r) {
        all.wet += pr[r].wet; all.culverts += pr[r].culverts; all.low += pr[r].low; all.buried += pr[r].buried; all.beside += pr[r].beside;
        all.over = fmax(all.over, pr[r].over);
    }
    free(pr);
    printf("roads' bridges: %d (%d over lakes), %.1f km of span, the longest %.0f m", nb, lakes, span / 1000.0, longest);
    if (lr >= 0) {
        const PlanRoad *R = plan_road(lr);
        DVec2 m = dscale(dadd(R->pts[0], R->pts[0]), 0.5);
        for (int sg = 0; sg + 1 < R->npts; ++sg)
            if (R->bridge[sg] == lb) { m = R->pts[sg]; break; }
        printf(" (at %.0f %.0f)", m.x, m.z);
    }
    printf("\n  every %.0f m along the roads: %d points in a river or lake on no bridge, %d in a stream (culverts);"
           " %d under a deck's clearance, %d with the ground over the deck (%d more beside another road); the highest deck %.0f m over the ground\n",
           BRIDGE_PROBE, all.wet, all.culverts, all.low, all.buried, all.beside, all.over);
    /* views of the bridges nearest the megacity, for NeonCity --cam: from the road 60 m back, and from beside it down by the water */
    DVec2 c0 = plan_settlement(0).centre;
    int near[2][2] = { { -1, -1 }, { -1, -1 } };
    double nd[2] = { 1e300, 1e300 };
    for (int r = 0; r < nr; ++r) {
        const PlanRoad *R = plan_road(r);
        for (int b = 0; b < R->nbridges; ++b) {
            DVec2 p = R->pts[0];
            for (int sg = 0; sg + 1 < R->npts; ++sg)
                if (R->bridge[sg] == b) { p = R->pts[sg]; break; }
            double d = dlen(dsub(p, c0));
            for (int q = 0; q < 2; ++q) {
                if (d >= nd[q]) continue;
                if (q == 0) { nd[1] = nd[0]; near[1][0] = near[0][0]; near[1][1] = near[0][1]; }
                nd[q] = d; near[q][0] = r; near[q][1] = b;
                break;
            }
        }
    }
    for (int q = 0; q < 2; ++q) {
        if (near[q][0] < 0) continue;
        const PlanRoad *R = plan_road(near[q][0]);
        const PlanRoadBridge *B = &R->bridges[near[q][1]];
        int sg = 0;
        double s = B->s0 - 60.0;
        while (sg + 2 < R->npts && R->s[sg + 1] < fmax(s, 0.0)) sg++;
        DVec2 d = dnorm(dsub(R->pts[sg + 1], R->pts[sg])), n = dv(-d.z, d.x);
        DVec2 at = dadd(R->pts[sg], dscale(d, fmax(s, 0.0) - R->s[sg]));
        double yaw = atan2(d.x, -d.z) * 57.29577951;
        if (yaw < 0.0) yaw += 360.0;
        double mid = 0.5 * (B->w0 + B->w1);
        int sm = sg;
        while (sm + 2 < R->npts && R->s[sm + 1] < mid) sm++;
        DVec2 pm = dadd(R->pts[sm], dscale(dnorm(dsub(R->pts[sm + 1], R->pts[sm])), mid - R->s[sm]));
        DVec2 side = dadd(pm, dscale(n, 80.0));
        double syaw = atan2(-n.x, n.z) * 57.29577951;
        if (syaw < 0.0) syaw += 360.0;
        printf("  a bridge %.0f km from the megacity's centre (%s, %.0f m, over %s): --cam %.0f %.0f %.0f -3 %.0f / --cam %.0f %.0f %.0f 4 %.0f\n",
               nd[q] / 1000.0, R->cls == PLAN_ROAD_FREEWAY ? "freeway" : "trunk road", B->s1 - B->s0, B->lake ? "a lake" : "a river",
               at.x, at.z, yaw, plan_road_height(near[q][0], fmax(s, 0.0)) + 2.0, side.x, side.z, syaw, terrain_height(side.x, side.z) + 3.0);
    }
    CHECK(all.wet == 0, "%d points of road in a river or a lake on no bridge", all.wet);
    CHECK(all.low == 0, "%d points of road over the water under their bridge's clearance", all.low);
    CHECK(all.buried == 0, "%d points of a bridge's deck under the ground", all.buried);
}

/* --- The rivers through the cities ---------------------------------------------------- */

/* The area of convex p inside convex channel c (m^2). */
static double overlap_area(const Poly *p, const PlanChannel *c) {
    double area = 0.0;
    for (int i = 1; i + 1 < c->n; ++i) area += dcross(dsub(c->v[i], c->v[0]), dsub(c->v[i + 1], c->v[0]));
    double o = area >= 0.0 ? 1.0 : -1.0;
    Poly q = *p;
    RoadAttr none;
    memset(&none, 0, sizeof none);
    for (int i = 0; i < c->n && q.n; ++i) {
        DVec2 a = c->v[i], e = dsub(c->v[(i + 1) % c->n], a);
        poly_clip(&q, a, dv(o * e.z, -o * e.x), none, &q);
    }
    return q.n ? poly_area(&q) : 0.0;
}

/* How much of segment a b lies inside convex channel c, shrunk by `in` (m). */
static double inside_length(DVec2 a, DVec2 b, const PlanChannel *c, double in) {
    double area = 0.0;
    for (int i = 1; i + 1 < c->n; ++i) area += dcross(dsub(c->v[i], c->v[0]), dsub(c->v[i + 1], c->v[0]));
    double o = area >= 0.0 ? 1.0 : -1.0, lo = 0.0, hi = 1.0;
    DVec2 d = dsub(b, a);
    for (int i = 0; i < c->n; ++i) {
        DVec2 v = c->v[i], e = dsub(c->v[(i + 1) % c->n], v);
        double l = dlen(e);
        if (l < 1e-9) continue;
        double k0 = o * dcross(e, dsub(a, v)) / l - in, k1 = o * dcross(e, d) / l;   /* inside: k0 + t k1 >= 0 */
        if (fabs(k1) < 1e-12) { if (k0 < 0.0) return 0.0; continue; }
        double t = -k0 / k1;
        if (k1 > 0.0) lo = fmax(lo, t); else hi = fmin(hi, t);
    }
    return hi > lo ? (hi - lo) * dlen(d) : 0.0;
}

/* Where a river runs through a built town: no block stands on its channel,
 * no local street crosses it, and every arterial and boulevard that does is
 * a bridge. Read round every built piece of channel (every stride-th). */
static void test_channels(void) {
    int built = 0, belt = 0, home = 0, home_bridges = 0;
    double built_km = 0.0, belt_km = 0.0;
    for (int k = 0; k < plan_channel_count(); ++k) {
        const PlanChannel *c = plan_channel(k);
        double l = dlen(dsub(c->b, c->a)) / 1000.0;
        if (c->built) { built++; built_km += l; } else { belt++; belt_km += l; }
        if (c->built && plan_settlement_near(c->a.x, c->a.z) == 0) home++;
    }
    for (int k = 0; k < plan_bridge_count(); ++k) {
        const PlanBridge *b = plan_bridge(k);
        if (plan_settlement_near(b->p.x, b->p.z) == 0) home_bridges++;
    }
    printf("rivers through the cities: %d pieces of channel across the towns (%.0f km), %d out in their belts (%.0f km), %d bridges;"
           " the megacity %d pieces, %d bridges\n", built, built_km, belt, belt_km, plan_bridge_count(), home, home_bridges);
    /* views of the bridges nearest the megacity's core, for NeonCity --cam: from a deck's sidewalk out over its parapet, and from 60 m up */
    DVec2 c0 = plan_town_centre(0, 0);
    int near[3] = { -1, -1, -1 };
    for (int k = 0; k < plan_bridge_count(); ++k) {
        double d = dlen(dsub(plan_bridge(k)->p, c0));
        for (int q = 0; q < 3; ++q) {
            if (near[q] >= 0 && d >= dlen(dsub(plan_bridge(near[q])->p, c0))) continue;
            for (int m = 2; m > q; --m) near[m] = near[m - 1];
            near[q] = k;
            break;
        }
    }
    for (int q = 0; q < 3; ++q) {
        if (near[q] < 0) continue;
        const PlanBridge *b = plan_bridge(near[q]);
        DVec2 n = dv(-b->d.z, b->d.x), at = dadd(b->p, dscale(n, b->half + 0.5 * b->walk));
        double yaw = atan2(n.x, -n.z) * 57.29577951;
        if (yaw < 0.0) yaw += 360.0;
        printf("  a bridge %.0f m from the core (class %d, deck %.0f m): --cam %.0f %.0f %.0f -20 / --cam %.0f %.0f %.0f -35 60\n", dlen(dsub(b->p, c0)),
               b->cls, b->s1 - b->s0, at.x, at.z, yaw, at.x - 60.0 * n.x, at.z - 60.0 * n.z, yaw);
    }
    /* ...and of the piece of channel nearest it: from its promenade along it, and from 40 m up behind */
    {
        int best = -1;
        double bd = 1e300;
        for (int k = 0; k < plan_channel_count(); ++k) {
            const PlanChannel *c = plan_channel(k);
            double d = dlen(dsub(dscale(dadd(c->a, c->b), 0.5), c0));
            if (c->built && dlen(dsub(c->b, c->a)) > 150.0 && d < bd) { bd = d; best = k; }
        }
        if (best >= 0) {
            const PlanChannel *c = plan_channel(best);
            DVec2 u = dnorm(dsub(c->b, c->a)), n = dv(-u.z, u.x), m = dscale(dadd(c->a, c->b), 0.5);
            DVec2 at = dadd(dsub(m, dscale(u, 40.0)), dscale(n, c->hw + 2.0));
            double yaw = atan2(u.x, -u.z) * 57.29577951;
            if (yaw < 0.0) yaw += 360.0;
            printf("  a channel %.0f m from the core (%.0f m wide): --cam %.0f %.0f %.0f -12 / --cam %.0f %.0f %.0f -30 40\n", bd, 2.0 * c->hw,
                   at.x, at.z, yaw, m.x - 90.0 * u.x, m.z - 90.0 * u.z, yaw);
        }
    }
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) return;
    int stride = 1 + built / 600, checked = 0, on_block = 0, crossed = 0, unbridged = 0;
    for (int k = 0, n = 0; k < plan_channel_count(); ++k) {
        const PlanChannel *c = plan_channel(k);
        if (!c->built || n++ % stride) continue;
        terrain_frame_bind(plan_frame_settlement(plan_channel_settlement(k)));   /* laid out in its city's frame */
        plan_area(c->x0 - 30.0, c->z0 - 30.0, c->x1 + 30.0, c->z1 + 30.0, pa);
        terrain_frame_bind(NULL);
        checked++;
        for (int q = 0; q < pa->nchannels; ++q) {
            const PlanChannel *h = &pa->channels[q];
            if (!h->built) continue;
            for (int b = 0; b < pa->nblocks; ++b) {
                double a = overlap_area(&pa->blocks[b].curb, h);
                if (a > 0.5 && on_block++ < 5) CHECK(0, "a block at (%.0f, %.0f) stands %.1f m2 on a channel", pa->blocks[b].x0, pa->blocks[b].z0, a);
            }
            for (int r = 0; r < pa->nroads; ++r) {
                const Road *rd = &pa->roads[r];
                double len = inside_length(rd->a, rd->b, h, 0.5);
                if (len <= 0.5) continue;
                if (rd->at.cls == ROAD_LOCAL) { if (crossed++ < 5) CHECK(0, "a local street at (%.0f, %.0f) crosses a channel for %.1f m", rd->a.x, rd->a.z, len); continue; }
                int bridged = 0;
                for (int g = 0; g < pa->nbridges; ++g) bridged |= pa->bridges[g].road == rd->at.id;
                if (!bridged && unbridged++ < 5) CHECK(0, "a road (class %d) at (%.0f, %.0f) crosses a channel for %.1f m on no bridge", rd->at.cls, rd->a.x, rd->a.z, len);
            }
        }
    }
    printf("  round %d of them: %d blocks on a channel, %d local streets across one, %d roads across one on no bridge\n", checked, on_block, crossed, unbridged);
    free(pa);
}


/* --- A flight between the cities -------------------------------------------------------------
 * The camera flies from the megacity to the city nearest FLIGHT_KM away (and
 * at least that far) and back, along the great circle, FLIGHT_UP above the
 * sphere, the active frame chosen as the game chooses it (src/app/frames.c).
 * At every change of frame nothing may jump: the camera's place on the
 * planet and the way it faces stay put, and the ground round it, read in the
 * old frame and in the new at the same places, is the same to within
 * FLIGHT_POP. In every frame, the bend (terrain_bend: chart offsets taken as
 * the sphere's) stays within FLIGHT_BEND of the truth at the clipmap's edge;
 * and the far city is drawn in its own frame, the camera there in its
 * streets. */
#define FLIGHT_KM   5000.0
#define FLIGHT_UP   3000.0      /* m above the sphere */
#define FLIGHT_STEP 2000.0      /* m */
#define FLIGHT_POP  0.05        /* m */
#define FLIGHT_BEND 10.0        /* m, at FLIGHT_EDGE */
#define FLIGHT_EDGE 32000.0     /* the clipmap's reach from the camera (m) */

static TerrainDir unitd(TerrainDir a) { double l = sqrt(dot3(a, a)); TerrainDir r = { a.x / l, a.y / l, a.z / l }; return r; }

/* The camera at direction p, facing along the great circle towards q, in the active frame. */
static void flight_place(Player *pl, TerrainDir p, TerrainDir q) {
    const TerrainFrame *f = terrain_frame();
    terrain_dir_frame(f, p, &pl->feet[0], &pl->feet[2]);
    pl->feet[1] = FLIGHT_UP;
    TerrainDir ax, up, az;
    terrain_frame_axes(f, pl->feet[0], pl->feet[2], &ax, &up, &az);
    TerrainDir t = { q.x - dot3(q, p) * p.x, q.y - dot3(q, p) * p.y, q.z - dot3(q, p) * p.z };
    pl->yaw = (float)atan2(dot3(t, ax), -dot3(t, az));
    pl->vel[0] = pl->vel[1] = pl->vel[2] = 0.0f;
    pl->eye[0] = pl->feet[0]; pl->eye[1] = pl->feet[1]; pl->eye[2] = pl->feet[2];
    pl->fwd = v3(sinf(pl->yaw), 0.0f, -cosf(pl->yaw));
    pl->right = v3(cosf(pl->yaw), 0.0f, sinf(pl->yaw));
    pl->up = v3(0.0f, 1.0f, 0.0f);
}

/* The way the camera faces, on the planet. */
static TerrainDir flight_heading(const Player *pl) {
    TerrainDir ax, up, az;
    terrain_frame_axes(terrain_frame(), pl->feet[0], pl->feet[2], &ax, &up, &az);
    double sy = sin(pl->yaw), cy = cos(pl->yaw);
    TerrainDir h = { sy * ax.x - cy * az.x, sy * ax.y - cy * az.y, sy * ax.z - cy * az.z };
    return h;
}

/* The worst of terrain_bend against the truth, round the camera at FLIGHT_EDGE. */
static double flight_bend(const Player *pl) {
    double cx = pl->feet[0], cz = pl->feet[2], cy = pl->feet[1], worst = 0.0;
    TerrainDir c = scale3(terrain_world_dir(cx, cz), PLANET_RADIUS + cy), ax, up, az;
    terrain_frame_axes(terrain_frame(), cx, cz, &ax, &up, &az);
    for (int q = 0; q < 16; ++q) {
        double a = 6.283185307179586 * q / 16.0, dx = FLIGHT_EDGE * cos(a), dz = FLIGHT_EDGE * sin(a);
        TerrainDir P = scale3(terrain_world_dir(cx + dx, cz + dz), PLANET_RADIUS), d = { P.x - c.x, P.y - c.y, P.z - c.z };
        float b[3];
        terrain_bend((float)dx, (float)dz, 0.0f, (float)cy, b);
        double e = hypot(hypot(b[0] - dot3(d, ax), b[1] - dot3(d, up)), b[2] - dot3(d, az));
        worst = fmax(worst, e);
    }
    return worst;
}

/* The ground round the camera (at it, and 100 m, 1 km and 10 km out every
 * way), read through the frame: the planet-frame places and their heights. */
#define FLIGHT_PROBES 25
static void flight_ground(const Player *pl, TerrainDir *at, double *h) {
    const double rings[3] = { 100.0, 1000.0, 10000.0 };
    int n = 0;
    for (int r = -1; r < 3; ++r)
        for (int q = 0; q < (r < 0 ? 1 : 8); ++q) {
            double a = 6.283185307179586 * q / 8.0, s = r < 0 ? 0.0 : rings[r];
            double x = pl->feet[0] + s * cos(a), z = pl->feet[2] + s * sin(a);
            at[n] = terrain_world_dir(x, z);
            h[n++] = terrain_height(x, z);
        }
}

static void test_flight(void) {
    PlanSettlement home = plan_settlement(0);
    int far = -1;
    double farD = 1e300;
    for (int k = 1; k < plan_settlement_count(); ++k) {
        double d = sphere_m(plan_settlement(k).dir, home.dir);
        if (d >= FLIGHT_KM * 1000.0 && d < farD) { farD = d; far = k; }
    }
    if (far < 0) { printf("flight: no city %.0f km from the megacity\n", FLIGHT_KM); return; }
    Frames fr;
    frames_init(&fr, 0);
    Player pl;
    memset(&pl, 0, sizeof pl);
    TerrainDir ends[2] = { terrain_world_dir(plan_town_centre(0, 0).x, plan_town_centre(0, 0).z), plan_settlement(far).dir };
    int switches = 0, travel = 0, cities = 0, steps = 0, inFar = 0, lineages = 0, unforeseen = 0;
    double posJump = 0.0, turn = 0.0, pop = 0.0, bendTravel = 0.0, bendCity = 0.0;
    char popAt[160] = "";
    for (int leg = 0; leg < 2; ++leg) {
        TerrainDir a = ends[leg], b = ends[1 - leg];
        double len = sphere_m(a, b);
        int n = (int)ceil(len / FLIGHT_STEP);
        for (int s = 0; s <= n; ++s, ++steps) {
            double t = len * s / n / PLANET_RADIUS, th = len / PLANET_RADIUS;
            TerrainDir p = unitd(add3(scale3(a, sin(th - t) / sin(th)), scale3(b, sin(t) / sin(th))));
            flight_place(&pl, p, b);
            const TerrainFrame *was = terrain_frame(), *ahead = fr.ahead;
            TerrainDir h0 = flight_heading(&pl), g0[FLIGHT_PROBES];
            double gh0[FLIGHT_PROBES];
            flight_ground(&pl, g0, gh0);
            if (!frames_update(&fr, &pl)) continue;
            switches++;
            if (fr.active->lineage != was->lineage) {   /* the renderer made ready in it beforehand */
                lineages++;
                if (ahead != fr.active) unforeseen++;
            }
            if (fr.active->settlement < 0) travel++; else cities++;
            if (fr.active->settlement == far) inFar = 1;
            /* the same place and the same way */
            TerrainDir p1 = terrain_world_dir(pl.feet[0], pl.feet[2]), h1 = flight_heading(&pl);
            posJump = fmax(posJump, PLANET_RADIUS * sqrt(dot3(sub3(p1, p), sub3(p1, p))));
            turn = fmax(turn, acos(fmin(1.0, dot3(unitd(h0), unitd(h1)))));
            /* the same ground at the same places */
            for (int k = 0; k < FLIGHT_PROBES; ++k) {
                double x, z;
                terrain_dir_world(g0[k], &x, &z);
                double e = fabs(terrain_height(x, z) - gh0[k]);
                if (e > pop) {
                    pop = e;
                    double lat, lon;
                    terrain_dir_latlon(g0[k], &lat, &lon);
                    snprintf(popAt, sizeof popAt, "%.4f %.4f, frame %u to %u", lat, lon, was->id, fr.active->id);
                }
            }
            double bend = flight_bend(&pl);
            if (fr.active->settlement < 0) bendTravel = fmax(bendTravel, bend); else bendCity = fmax(bendCity, bend);
        }
    }
    /* at the far city: in its frame, in its streets */
    terrain_frame_bind(plan_frame_settlement(far));
    double sx, sz;
    float syaw;
    city_spawn(&sx, &sz, &syaw);
    int built = plan_built_edge(sx, sz) < CITY_BUILT_EDGE;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    int roads = 0, blocks = 0;
    if (pa) { plan_area(sx - 200.0, sz - 200.0, sx + 200.0, sz + 200.0, pa); roads = pa->nroads; blocks = pa->nblocks; free(pa); }
    Player at;
    memset(&at, 0, sizeof at);
    at.feet[0] = sx; at.feet[2] = sz; at.feet[1] = terrain_height(sx, sz) + 2.0;
    double bendFar = flight_bend(&at);
    terrain_frame_bind(NULL);
    frames_shutdown(&fr);
    terrain_frame_activate(NULL);
    printf("flight: the megacity to %s (%.0f km) and back, %d steps, %d changes of frame (%d into travel frames, %d into cities'),\n"
           "  %d into another lineage, %d of them not named ahead the step before\n",
           plan_settlement_name(far), farD / 1000.0, steps, switches, travel, cities, lineages, unforeseen);
    printf("  at a change the camera moved %.2e m and turned %.2e rad at most; the ground round it moved %.5f m at most%s%s%s\n",
           posJump, turn, pop, pop > 0.0 ? " (at " : "", pop > 0.0 ? popAt : "", pop > 0.0 ? ")" : "");
    printf("  the bend at %.0f km strays %.2f m at most in a travel frame, %.2f m in a city's; %.2f m at %s's spawn\n",
           FLIGHT_EDGE / 1000.0, bendTravel, bendCity, bendFar, plan_settlement_name(far));
    printf("  at %s's spawn (its frame): %d roads, %d blocks within 200 m\n", plan_settlement_name(far), roads, blocks);
    CHECK(inFar, "the flight never worked in %s's frame", plan_settlement_name(far));
    CHECK(unforeseen == 0, "%d changes into another lineage weren't named ahead", unforeseen);
    CHECK(posJump < 1e-3 && turn < 1e-6, "a change of frame moved the camera %.2e m or turned it %.2e rad", posJump, turn);
    CHECK(pop <= FLIGHT_POP, "a change of frame moved the ground %.3f m (%s)", pop, popAt);
    CHECK(bendTravel <= FLIGHT_BEND && bendCity <= FLIGHT_BEND && bendFar <= FLIGHT_BEND, "the bend strays %.2f / %.2f / %.2f m", bendTravel, bendCity, bendFar);
    CHECK(built && roads > 0 && blocks > 0, "%s's spawn isn't in its built streets", plan_settlement_name(far));
}

/* Settlement k's built towns that have an unbuilt one beside them, by their
 * centres in its own frame: how far a point outside its built towns lies
 * from them is how far it lies from these. */
typedef struct TownEdge { int n; DVec2 *c; } TownEdge;
static TownEdge town_edge(int k) {
    TownEdge e = { 0, NULL };
    PlanSettlement s = plan_settlement(k);
    terrain_frame_bind(plan_frame_settlement(k));
    double r = plan_settlement_reach(k) + TOWN_SIZE;
    int i0 = (int)floor((s.disc.x - r) / TOWN_SIZE), i1 = (int)floor((s.disc.x + r) / TOWN_SIZE);
    int j0 = (int)floor((s.disc.z - r) / TOWN_SIZE), j1 = (int)floor((s.disc.z + r) / TOWN_SIZE);
    int w = i1 - i0 + 3, d = j1 - j0 + 3, cap = 0;
    unsigned char *built = (unsigned char *)calloc((size_t)w * d, 1);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) built[(j - j0 + 1) * w + (i - i0 + 1)] = (unsigned char)plan_town_built(i, j);
    for (int j = 1; j < d - 1; ++j)
        for (int i = 1; i < w - 1; ++i) {
            const unsigned char *b = &built[j * w + i];
            if (!b[0] || (b[-1] && b[1] && b[-w] && b[w])) continue;
            if (e.n == cap) { cap = cap ? 2 * cap : 256; e.c = (DVec2 *)realloc(e.c, sizeof(DVec2) * cap); }
            e.c[e.n++] = plan_town_centre(i0 + i - 1, j0 + j - 1);
        }
    free(built);
    terrain_frame_bind(NULL);
    return e;
}

/* How far direction p lies from settlement k's built towns (e its town_edge;
 * p outside them), in k's frame (m). */
static double built_away(int k, const TownEdge *e, TerrainDir p) {
    double x, z, best = 1e300;
    terrain_dir_frame(plan_frame_settlement(k), p, &x, &z);
    for (int t = 0; t < e->n; ++t) {
        double dx = fmax(fabs(x - e->c[t].x) - 0.5 * TOWN_SIZE, 0.0), dz = fmax(fabs(z - e->c[t].z) - 0.5 * TOWN_SIZE, 0.0);
        best = fmin(best, dx * dx + dz * dz);
    }
    return sqrt(best);
}

/* Neighbours: every pair of cities whose discs come within twice
 * PLAN_FRAME_NEAR, swept on a NEIGHBOUR_STEP grid round them with the game's
 * rule for which frame the camera is in (frames.c: a city's while within
 * PLAN_FRAME_NEAR of its disc, kept to FRAME_STAY beyond, left for a nearer
 * one FRAME_SWITCH_MARGIN nearer). Where the camera may be in one's frame:
 * how near the other's built towns come (which that frame doesn't lay out).
 * Where it changes from one's frame to the other's: how near the built towns
 * of the one it leaves are. Within the proxies' reach (TERRAIN_DISTANCE) a
 * neighbour is drawn as its proxies, made in its own frame
 * (src/render/proxy_gpu.c), but its chunks only in its own: so both must be
 * beyond the chunks' reach (VIEW_DISTANCE), else a neighbour's streets in
 * view would be boxes, or the city left would turn to boxes as the frame
 * changes. `list` prints every pair, and a view from the nearest approach. */
#define NEIGHBOUR_STEP 1000.0
static double beyond_disc(int k, TerrainDir p) { return sphere_m(p, disc_dir(k)) - plan_settlement_reach(k); }
static void test_neighbours(int list) {
    int n = plan_settlement_count(), pairs = 0, seen = 0;
    double leaveMin = 1e300, otherMin = 1e300;
    char leaveAt[96] = "", otherAt[96] = "";
    for (int k = 0; k < n; ++k)
        for (int q = k + 1; q < n; ++q) {
            double len = sphere_m(disc_dir(k), disc_dir(q));
            if (len - plan_settlement_reach(k) - plan_settlement_reach(q) > 2.0 * PLAN_FRAME_NEAR) continue;
            pairs++;
            double pairLeave = 1e300, pairOther = 1e300, view[3] = { 0.0, 0.0, 0.0 };
            int viewIn = k;
            TownEdge edge[2] = { town_edge(k), town_edge(q) };
            TerrainDir mid = unitd(add3(disc_dir(k), disc_dir(q)));
            TerrainChart ch = terrain_chart_about(mid, 0.0);
            double r = fmax(plan_settlement_reach(k), plan_settlement_reach(q)) + PLAN_FRAME_NEAR + FRAME_STAY;
            int m = (int)ceil(r / NEIGHBOUR_STEP);
            for (int j = -m; j <= m; ++j)
                for (int i = -m; i <= m; ++i) {
                    double x = i * NEIGHBOUR_STEP, z = j * NEIGHBOUR_STEP;
                    if (hypot(x, z) > r) continue;
                    TerrainDir p = terrain_chart_dir(&ch, x, z);
                    for (int side = 0; side < 2; ++side) {
                        int in = side ? q : k, other = side ? k : q;   /* the camera in in's frame */
                        double bi = beyond_disc(in, p), bo = beyond_disc(other, p);
                        if (bi >= PLAN_FRAME_NEAR + FRAME_STAY) continue;
                        int stays = !(bo < PLAN_FRAME_NEAR && bo < bi - FRAME_SWITCH_MARGIN);
                        int changes = !stays && bo > bi - FRAME_SWITCH_MARGIN - NEIGHBOUR_STEP;
                        double a = stays && bo < pairOther ? built_away(other, &edge[1 - side], p) : 1e300;
                        if (a < pairOther) {   /* a view from there in in's frame, towards the other's disc */
                            pairOther = a;
                            viewIn = in;
                            const TerrainFrame *f = plan_frame_settlement(in);
                            TerrainDir o = disc_dir(other), fx, fu, fz;
                            terrain_dir_frame(f, p, &view[0], &view[1]);
                            terrain_frame_axes(f, view[0], view[1], &fx, &fu, &fz);
                            TerrainDir h = { o.x - dot3(o, p) * p.x, o.y - dot3(o, p) * p.y, o.z - dot3(o, p) * p.z };
                            view[2] = atan2(dot3(h, fx), -dot3(h, fz)) * 180.0 / 3.14159265358979;
                        }
                        if (changes) pairLeave = fmin(pairLeave, built_away(in, &edge[side], p));
                    }
                }
            free(edge[0].c);
            free(edge[1].c);
            if (list)
                printf("  %-24s %-24s discs %5.1f km apart: the frame changes %5.1f km from the city left at the nearest; the other's towns %5.1f km off"
                       " (--city %d --cam %.0f %.0f %.0f)\n",
                       plan_settlement_name(k), plan_settlement_name(q), (len - plan_settlement_reach(k) - plan_settlement_reach(q)) / 1000.0,
                       pairLeave / 1000.0, pairOther / 1000.0, viewIn, view[0], view[1], view[2]);
            if (pairLeave < leaveMin) { leaveMin = pairLeave; snprintf(leaveAt, sizeof leaveAt, "%s / %s", plan_settlement_name(k), plan_settlement_name(q)); }
            if (pairOther < otherMin) { otherMin = pairOther; snprintf(otherAt, sizeof otherAt, "%s / %s", plan_settlement_name(k), plan_settlement_name(q)); }
            if (pairOther < TERRAIN_DISTANCE) seen++;
        }
    printf("neighbours: %d pairs within %.0f km; a city's frame changes to another's %.1f km from the city left at the nearest (%s);\n"
           "  in %d pairs one's towns come within the proxies' %.0f km while in the other's frame (drawn as its proxies), %.1f km at the nearest (%s)\n",
           pairs, 2.0 * PLAN_FRAME_NEAR / 1000.0, leaveMin / 1000.0, leaveAt, seen, TERRAIN_DISTANCE / 1000.0, otherMin / 1000.0, otherAt);
    CHECK(otherMin > VIEW_DISTANCE, "%s: one's built towns come within %.1f km while in the other's frame", otherAt, otherMin / 1000.0);
    CHECK(leaveMin > VIEW_DISTANCE, "%s: the frame changes %.1f km from the built towns of the city left", leaveAt, leaveMin / 1000.0);
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
    if (argc > 1 && strcmp(argv[1], "--neighbours") == 0) { test_neighbours(1); return g_failures != 0; }
    if (argc > 2 && strcmp(argv[1], "--map") == 0) return draw_planet(argv[2], argc > 3 ? atoi(argv[3]) : 1600, threads);
    if (argc > 2 && strcmp(argv[1], "--chart") == 0) return draw_map(argv[2], argc > 3 ? atoi(argv[3]) : 1600, 0.0, 0.0, 5400000.0, 0, threads);
    if (argc > 2 && strcmp(argv[1], "--local") == 0) {
        DVec2 c = plan_town_centre(0, 0);
        double km = argc > 3 ? atof(argv[3]) : 300.0;
        if (argc > 5) c = dv(atof(argv[4]), atof(argv[5]));
        return draw_map(argv[2], 1200, c.x, c.z, km * 500.0, 1, threads);
    }

    test_settlements();
    test_roads();
    test_road_ground();
    test_road_bridges(threads);
    test_channels();
    test_flight();
    test_neighbours(0);

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all world tests passed\n");
    return 0;
}
