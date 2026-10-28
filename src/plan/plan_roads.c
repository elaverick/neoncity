#include "plan_internal.h"
#include "terrain.h"
#include "hash.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The roads between the settlements (see plan.h), designed once the
 * settlements are placed, in these steps:
 *
 * 1. Candidates. The Delaunay triangulation of the settlements' centres:
 *    every pair of neighbours on one continent within CAND_REACH may be
 *    linked; beyond it, a colossal city's nearest until it has HUB_LINKS,
 *    and the shortest pair across each gap those leave, so a continent is
 *    whole however sparsely it is settled. Each candidate is
 *    routed over the coarse grid (below) for its cost.
 * 2. A minimum spanning tree of the candidates by cost joins each continent.
 * 3. Network design by a gravity model with preferential connection. Traffic
 *    between two settlements is their sizes' product over the square of their
 *    distance. A candidate is worth adding where the network makes its
 *    traffic go round, by how much and how much traffic, and the more so the
 *    more links its ends already have and the bigger they are (links gather
 *    at the hubs); the best is added, and again, until none is worth it or
 *    the budget is spent.
 * 4. Steiner junctions. Where two links leave a node less than 120 degrees
 *    apart they are shorter as a shared road to a junction at the triangle's
 *    Fermat point, then a branch to each; the best such saving is taken, and
 *    again, while any saves enough.
 * 5. Routing. Each link is routed over the coarse grid, then over the fine
 *    grid in a corridor round that route, then pulled straight wherever a
 *    straight line costs no more, so the road runs in long straights round
 *    the obstacles rather than along the grid. The fine grid costs each
 *    river and lake it crosses heavily, by the bridge's length.
 * 6. Where two roads cross, a junction: both are split there.
 * 7. Classes. The gravity model's traffic between every pair of settlements
 *    goes by the shortest route; the busiest links are freeways, up to
 *    FREEWAY_SHARE of the network's length, and every colossal city has one.
 *    A freeway doesn't end at a junction: the busiest road on from it is one
 *    too.
 * 8. Gates: a road ends at each settlement where it leaves the built towns,
 *    on the perimeter boulevard's centre line, its last stretch square to
 *    the boulevard.
 * 9. Corners are rounded, wider for freeways.
 * 10. Bridges: where a road crosses the bake's rivers and lakes, a span
 *    over the water's stretch and the embankments' spill slopes.
 * 11. Profiles: the ground along each road every PLAN_ROAD_STATION metres,
 *    smoothed, pinned to the city's ground at a gate and to one height at a
 *    junction, its grade held to the class's, raised over the water to each
 *    bridge's clearance; the ground blends to it (plan_road_ground) but for
 *    under the bridges, found through an index of ROAD_CELL squares.
 *
 * The roads are on the sphere. A link is routed and shaped in a chart of its
 * own (link_chart: the azimuthal-equidistant one about the midpoint of its
 * ends, north up), where over a link's length the chart's distances are the
 * sphere's to within a percent; the nodes, where links meet, are directions,
 * and whatever one link asks of another goes through them. Distances between
 * nodes are great circles, and the candidates are the sphere's Delaunay
 * triangulation (the plane's of the centres' stereographic projection).
 *
 * The grids, in each link's chart, cost a step by its length, more on rough
 * land: in the mountains, on hills and plateaus (the coarse grid, by the
 * region's landforms, worked out once over the sphere: plan_cube_*), up
 * steep grades and along or across canyons (the fine grid, from heights and
 * the canyons' centre lines), and across the water (the fine grid: the
 * bake's rivers and lakes). The sea to come, the lakes and other settlements
 * block them. Steps are costed in whole metres, so the game's fast floating
 * point and the tests route alike. Everything is a pure function of the
 * settlements and the terrain (without roads), on as many threads as the
 * caller gives, each link on its own.
 *
 * The city and the terrain are still drawn in the world's chart, so that is
 * where the roads are published (PlanRoad): each link's points taken into
 * it, no more than PUBLISH_STEP apart, so that a segment straight in either
 * chart is straight in the other to within millimetres. Lengths along them
 * are the sphere's. plan_road_ground measures its distances as the sphere's
 * too (metric_at): far from the chart's origin, where its distances across
 * are stretched, the ground is graded to the road's true width. Spatial
 * indices are of world chart squares, each thing filed as far round as the
 * chart's stretch there takes its reach (seg_squares). */

#define COARSE_CELL    4000.0   /* m */
#define FINE_CELL      1000.0
#define FINE_PER       4        /* fine cells along a coarse cell's side */
#define CORRIDOR       2        /* coarse cells either side of the coarse route the fine route may use */
#define CITY_CLEAR     1500.0   /* and this far outside other settlements' edges (m) */
#define CAND_REACH     1600000.0 /* candidates no longer than this (m) */
#define DETOUR_MIN     1.3      /* a candidate is worth adding where the network's route costs this many times more */
#define ADD_PER_CITY   0.55     /* links added by the gravity model, at most, per settlement */
#define ADD_SCORE_MIN  0.02
#define PREFER         0.5      /* preferential connection: the ends' degrees count to this power */
#define HUB_LINKS      3        /* a colossal city has at least this many links, candidates allowing */
#define GAP_TRIES      4        /* pairs across a continent's gaps tried, at most, for each settlement on one... */
#define GAP_BATCH      32       /* ...so many at a time */
#define STEINER_SAVE   0.06     /* a junction is made where it saves this share of the two links' length */
#define FREEWAY_SHARE  0.4
#define GRAVITY_D0     500000.0 /* the gravity model's distances in this unit, never under a fifth of it */
#define FREEWAY_RADIUS 1500.0   /* the corners' radius (m) */
#define TRUNK_RADIUS   500.0
#define ARC_STEP       60.0     /* a rounded corner's points at most this far apart (m) */
#define SPIKE_COS      (-0.94)  /* a corner turning back by more than about 160 degrees is a spike */
#define MAX_NODES      (PLAN_MAX_SETTLEMENTS + 1024)
#define MAX_LINKS      4096
#define GATE_STEP      250.0    /* m: a road's way out of its settlement is walked in these steps... */
#define GATE_APPROACH  300.0    /* ...and its last stretch is square to the boulevard for this long */
#define ROAD_CELL      512.0    /* the index's squares (m) */
#define ROAD_REACH     (PLAN_FREEWAY_HALF + PLAN_ROAD_SHOULDER + PLAN_ROAD_BATTER_MAX + 1.5 * PLAN_ROAD_LOD_SPACING)
#define MAX_RECORDS    1500000  /* a search gives up past this many cells... */
#define ROUTE_LIMIT    6.0      /* ...or costing more than this many times the straight line */
#define BRIDGE_FIXED   1000.0   /* a crossing of a river or a lake costs as much as this much road (m)... */
#define BRIDGE_PER_M   20.0     /* ...and this much for each metre of bridge */
#define BRIDGE_SIN_MIN 0.2      /* a crossing at a shallow angle costs as one at this sine (its bridge five times the river's width) */
#define WATER_CELL     250.0    /* the bridges' grid of the water near a road (m) */
#define WATER_STEP     5000.0   /* the water near a road is gathered every this far along it... */
#define WATER_RADIUS   4000.0   /* ...this far round */
#define WET_STEP       4.0      /* a crossing's stretch over the water and its banks is walked out in these steps (m)... */
#define WET_MAX        1500.0   /* ...this far either side at most */
#define WET_DEPTH      0.5      /* ground this far under the dry ground is the water's bed or bank (m) */
#define WET_JOIN       60.0     /* stretches this near each other are one bridge (m) */
#define JUNCTION_CLEAR 40.0     /* a bridge stays this far from a junction (where the other roads' surfaces are) (m) */
#define PUBLISH_STEP   400.0    /* a published road's points are no more than this far apart (m) */
#define BRIDGE_UNDER   0.3      /* the ground under a deck stands this far under its soffit, at least (m) */

/* Open addressing from int64 keys to ints; INT64_MIN marks an empty slot. */
typedef struct IMap { int64_t *keys; int *vals; int cap, n; } IMap;

typedef struct Link {
    int      a, b;          /* nodes */
    int64_t  cost;          /* the route's (or, before it is routed, the coarse route's) cost */
    int      cls;
    double   flow;
    TerrainChart ch;        /* its own chart: pts are in it */
    DVec2   *pts;           /* the route, from a to b (malloc'd) */
    int      npts;
    double  *s;
    int      ok;
    int      gate[2];       /* it ends at a gate at a, at b */
    PlanGate gp[2];
    float   *prof;          /* the profile (malloc'd) */
    int      nprof;
    double   ds;
    PlanRoadBridge *br;     /* its bridges, along it (malloc'd) */
    int      nbr;
    int     *segbr;         /* each segment's bridge, or -1 (malloc'd) */
} Link;

typedef struct Net {
    PlanNode node[MAX_NODES];
    int      nnodes;
    Link     link[MAX_LINKS];
    int      nlinks;
    PlanRoad road[MAX_LINKS];
    double   nodeH[MAX_NODES];   /* each junction's height, where its roads meet */
    /* the index: ROAD_CELL squares -> runs of a road's segments (build_index) */
    IMap     cells;              /* square -> its first run in idx */
    int     *idxCount;           /* by the same number: how many */
    int     *idx;                /* each run's road, first and last segment */
    float   *idxLo, *idxHi;      /* by a square's first entry: the least and most height of its roads' segments */
    float    lo, hi;             /* ...and of every road */
} Net;

static Net *g_net;

/* --- Small containers ---------------------------------------------------------------- */

static uint32_t key_hash(int64_t k) { uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ULL; return (uint32_t)(x >> 32) ^ (uint32_t)x; }
static int64_t cell_key(int i, int j) { return (int64_t)(((uint64_t)(uint32_t)i << 32) | (uint32_t)j); }

static void imap_init(IMap *m, int cap) {
    m->cap = cap; m->n = 0;
    m->keys = (int64_t *)malloc(sizeof(int64_t) * (size_t)cap);
    m->vals = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!m->keys || !m->vals) fatal("out of memory routing the roads");
    for (int k = 0; k < cap; ++k) m->keys[k] = INT64_MIN;
}
static void imap_free(IMap *m) { free(m->keys); free(m->vals); }
static int imap_get(const IMap *m, int64_t key) {
    for (uint32_t h = key_hash(key) & (uint32_t)(m->cap - 1);; h = (h + 1) & (uint32_t)(m->cap - 1)) {
        if (m->keys[h] == key) return m->vals[h];
        if (m->keys[h] == INT64_MIN) return -1;
    }
}
static void imap_put(IMap *m, int64_t key, int val) {
    if (2 * (m->n + 1) > m->cap) {
        IMap g;
        imap_init(&g, m->cap * 2);
        for (int k = 0; k < m->cap; ++k)
            if (m->keys[k] != INT64_MIN) imap_put(&g, m->keys[k], m->vals[k]);
        imap_free(m);
        *m = g;
    }
    uint32_t h = key_hash(key) & (uint32_t)(m->cap - 1);
    while (m->keys[h] != INT64_MIN && m->keys[h] != key) h = (h + 1) & (uint32_t)(m->cap - 1);
    if (m->keys[h] == INT64_MIN) m->n++;
    m->keys[h] = key;
    m->vals[h] = val;
}

static void *grow(void *p, int *cap, int need, size_t size) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 64;
    while (c < need) c *= 2;
    void *q = realloc(p, size * (size_t)c);
    if (!q) fatal("out of memory routing the roads");
    *cap = c;
    return q;
}

/* --- Charts ------------------------------------------------------------------------------- */

static TerrainChart g_wc;   /* the world's chart */

static TerrainDir dir_of(const TerrainChart *c, DVec2 p) { return terrain_chart_dir(c, p.x, p.z); }
static DVec2 in_chart(const TerrainChart *c, TerrainDir d) { DVec2 r; terrain_dir_chart(c, d, &r.x, &r.z); return r; }
static DVec2 world_of(TerrainDir d) { return in_chart(&g_wc, d); }
/* A point of chart c taken into chart to. */
static DVec2 rechart(const TerrainChart *c, const TerrainChart *to, DVec2 p) { return in_chart(to, dir_of(c, p)); }

static double sphere_m(TerrainDir a, TerrainDir b) {
    TerrainDir c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return PLANET_RADIUS * atan2(sqrt(c.x * c.x + c.y * c.y + c.z * c.z), a.x * b.x + a.y * b.y + a.z * b.z);
}

/* A link's chart: about the midpoint of the great circle between its ends. */
static TerrainChart link_chart(TerrainDir a, TerrainDir b) {
    TerrainDir m = { a.x + b.x, a.y + b.y, a.z + b.z };
    double l = sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
    if (l < 1e-9) m = a; else { m.x /= l; m.y /= l; m.z /= l; }
    return terrain_chart_about(m, 0.0);
}

/* How far the world's chart stretches distances at its (x, z): along the
 * line out from its origin not at all, across it by th / sin th, th the
 * angle out (held short of the antipode, where it is unbounded). */
static double stretch_at(DVec2 w) {
    double th = fmin(dlen(w) / PLANET_RADIUS, 3.1);
    return th > 1e-9 ? th / sin(th) : 1.0;
}

/* The sphere's metres at the world chart's (x, z): offsets from there along
 * the line out from the origin as they are, across it shrunk by sin th / th. */
typedef struct Metric { DVec2 r; double s; } Metric;
static Metric metric_at(DVec2 w) {
    Metric m = { dv(1.0, 0.0), 1.0 };
    double l = dlen(w);
    if (l > 0.0) { m.r = dscale(w, 1.0 / l); m.s = 1.0 / stretch_at(w); }
    return m;
}
static DVec2 metric_apply(const Metric *m, DVec2 d) {
    double a = ddot(d, m->r);
    DVec2 r = dscale(m->r, a);
    return dadd(r, dscale(dsub(d, r), m->s));
}

/* --- A* over a grid --------------------------------------------------------------------- */

/* A step's cost from cell a to its neighbour b, whole metres, or < 0 if it can't be taken. */
typedef int64_t (*StepCost)(void *ctx, int ai, int aj, int bi, int bj);

typedef struct Rec { int64_t g; int parent, i, j, closed; } Rec;
typedef struct HeapItem { int64_t f; int rec; } HeapItem;

typedef struct Search {
    IMap      map;
    Rec      *rec;
    int       nrec, caprec;
    HeapItem *heap;
    int       nheap, capheap;
} Search;

static int heap_less(HeapItem a, HeapItem b) { return a.f < b.f || (a.f == b.f && a.rec < b.rec); }

static void heap_push(Search *s, HeapItem it) {
    s->heap = (HeapItem *)grow(s->heap, &s->capheap, s->nheap + 1, sizeof(HeapItem));
    int k = s->nheap++;
    while (k > 0 && heap_less(it, s->heap[(k - 1) / 2])) { s->heap[k] = s->heap[(k - 1) / 2]; k = (k - 1) / 2; }
    s->heap[k] = it;
}

static HeapItem heap_pop(Search *s) {
    HeapItem top = s->heap[0], last = s->heap[--s->nheap];
    int k = 0;
    for (;;) {
        int c = 2 * k + 1;
        if (c >= s->nheap) break;
        if (c + 1 < s->nheap && heap_less(s->heap[c + 1], s->heap[c])) c++;
        if (!heap_less(s->heap[c], last)) break;
        s->heap[k] = s->heap[c];
        k = c;
    }
    if (s->nheap) s->heap[k] = last;
    return top;
}

/* Octile distance in cells, times the cell and eps: a lower bound on the cost
 * (every step costs at least its length) when eps is 1, and the search
 * weighted towards the goal above it. */
static int64_t heuristic(int i, int j, int gi, int gj, double cell, double eps) {
    double dx = fabs((double)(i - gi)), dz = fabs((double)(j - gj));
    return (int64_t)(eps * cell * (fmax(dx, dz) + 0.41421356237 * fmin(dx, dz)));
}

/* The cheapest path (eps 1) or one within eps of it from cell (si, sj) to
 * (gi, gj): its cells into *pi / *pj (malloc'd, start first). Returns its
 * cost, or -1 if there is none costing under `limit` within MAX_RECORDS cells. */
static int64_t astar(int si, int sj, int gi, int gj, double cell, double eps, int64_t limit, StepCost cost, void *ctx, int **pi, int **pj, int *np) {
    Search s;
    memset(&s, 0, sizeof(s));
    imap_init(&s.map, 1 << 12);
    s.rec = (Rec *)grow(NULL, &s.caprec, 1, sizeof(Rec));
    s.rec[0] = (Rec){ 0, -1, si, sj, 0 };
    s.nrec = 1;
    imap_put(&s.map, cell_key(si, sj), 0);
    heap_push(&s, (HeapItem){ heuristic(si, sj, gi, gj, cell, eps), 0 });
    int found = -1;
    while (s.nheap) {
        HeapItem it = heap_pop(&s);
        Rec *r = &s.rec[it.rec];
        if (r->closed) continue;
        r->closed = 1;
        if (r->i == gi && r->j == gj) { found = it.rec; break; }
        if (s.nrec > MAX_RECORDS) break;
        int ri = r->i, rj = r->j;
        int64_t rg = r->g;
        for (int d = 0; d < 8; ++d) {
            static const int DI[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DJ[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            int ni = ri + DI[d], nj = rj + DJ[d];
            int64_t c = cost(ctx, ri, rj, ni, nj);
            if (c < 0) continue;
            int64_t g = rg + c;
            if (g + heuristic(ni, nj, gi, gj, cell, 1.0) > limit) continue;
            int idx = imap_get(&s.map, cell_key(ni, nj));
            if (idx >= 0) {
                if (s.rec[idx].closed || s.rec[idx].g <= g) continue;
                s.rec[idx].g = g;
                s.rec[idx].parent = it.rec;
            } else {
                s.rec = (Rec *)grow(s.rec, &s.caprec, s.nrec + 1, sizeof(Rec));
                idx = s.nrec++;
                s.rec[idx] = (Rec){ g, it.rec, ni, nj, 0 };
                imap_put(&s.map, cell_key(ni, nj), idx);
            }
            heap_push(&s, (HeapItem){ g + heuristic(ni, nj, gi, gj, cell, eps), idx });
        }
    }
    int64_t total = -1;
    *np = 0; *pi = *pj = NULL;
    if (found >= 0) {
        int n = 0;
        for (int k = found; k >= 0; k = s.rec[k].parent) n++;
        *pi = (int *)malloc(sizeof(int) * (size_t)n);
        *pj = (int *)malloc(sizeof(int) * (size_t)n);
        if (!*pi || !*pj) fatal("out of memory routing the roads");
        int k = found;
        for (int q = n - 1; q >= 0; --q, k = s.rec[k].parent) { (*pi)[q] = s.rec[k].i; (*pj)[q] = s.rec[k].j; }
        *np = n;
        total = s.rec[found].g;
    }
    imap_free(&s.map);
    free(s.rec);
    free(s.heap);
    return total;
}

/* --- What a route may cross ---------------------------------------------------------------- */

/* Whether a road may pass p (w in the world's chart) on its way between
 * settlements `from` and `to` (-1 for a junction): above the sea to come and
 * clear of every other settlement. Inside its own settlements' footprints the
 * ground is the city's, kept off the sea by its coast (plan_world.c), so
 * there it may pass however low the land. */
static int passable(TerrainDir p, DVec2 w, int from, int to) {
    double e = plan_city_edge(w.x, w.z);
    if (e >= 0.0 && !plan_road_dry_dir(p)) return 0;
    if (e < CITY_CLEAR) {
        int k = plan_settlement_near(w.x, w.z);
        if (k != from && k != to) return 0;
    }
    return 1;
}

/* The coarse grid: each cell's cost per metre of a step, from the region's
 * landforms, worked out once over the sphere (plan_cube_*, nodes COARSE_CELL
 * apart at most; a face's rows on the threads) and kept as a byte:
 * 1 + v / COARSE_STEPS, or COARSE_SEA where no road may go (the sea, a lake)
 * and COARSE_CITY within CITY_CLEAR of a settlement, where only its own
 * roads may (inside its footprint, however low the land: passable). A
 * link's coarse cells, COARSE_CELL squares of its chart, read the node
 * nearest their centres. */
#define COARSE_STEPS  50.0f
#define COARSE_SEA    255
#define COARSE_CITY   254
#define COARSE_DEEP   (-1000.0)   /* the sea grid's margin this far under is the deep ocean (m) */

static uint8_t   *g_coarse;
static int        g_coarse_n;

static void coarse_row(void *ctx, int r) {
    (void)ctx;
    const int n = g_coarse_n, f = r / n, j = r % n;
    for (int i = 0; i < n; ++i) {
        TerrainDir p = plan_cube_dir(n, f, i, j);
        uint8_t v = COARSE_SEA;
        /* the deep ocean, which no footprint reaches (they stop short of the shore) */
        if (plan_world_sea_dir(p) < COARSE_DEEP) { g_coarse[plan_cube_index(n, f, i, j)] = v; continue; }
        DVec2 w = world_of(p);
        double e = plan_city_edge(w.x, w.z);
        if (e < 0.0) v = COARSE_CITY;
        else if (plan_road_dry_dir(p)) {
            if (e < CITY_CLEAR) v = COARSE_CITY;
            else if (terrain_earth_shore(p) < 0.0) v = COARSE_SEA;   /* a lake */
            else {
                TerrainLandform lf;
                terrain_landform_dir(p, &lf);
                float c = 4.0f * lf.w[TERRAIN_MOUNTAINS] + 1.5f * lf.w[TERRAIN_PLATEAU] + 0.6f * lf.w[TERRAIN_HILLS] + (lf.canyon > 0.0f ? 0.3f : 0.0f);
                v = (uint8_t)fminf(c * COARSE_STEPS + 0.5f, COARSE_CITY - 1.0f);
            }
        }
        g_coarse[plan_cube_index(n, f, i, j)] = v;
    }
}

/* A coarse route's search: in chart ch, between settlements from and to
 * (-1 for a junction), from cell (si, sj) to (gi, gj); what it has read of
 * the grid, by cell (its factor in 1/1024ths, -1024 for none) and by corner
 * (whether a diagonal step through it stays on land). */
typedef struct Coarse { const TerrainChart *ch; int from, to, gi, gj, si, sj; IMap memo, corner; } Coarse;

/* Coarse cell (i, j)'s cost per metre, or -1 where this route can't go. */
static float coarse_factor(Coarse *c, int i, int j) {
    int64_t key = cell_key(i, j);
    int m = imap_get(&c->memo, key);
    if (m != -1) return m / 1024.0f;   /* -1 is never a factor's: they are -1024 or 1024 and more */
    TerrainDir p = dir_of(c->ch, dv((i + 0.5) * COARSE_CELL, (j + 0.5) * COARSE_CELL));
    double fi, fj;
    int f = plan_cube_at(g_coarse_n, p, &fi, &fj);
    uint8_t v = g_coarse[plan_cube_index(g_coarse_n, f, (int)(fi + 0.5), (int)(fj + 0.5))];
    int end = (i == c->si && j == c->sj) || (i == c->gi && j == c->gj);
    float r;
    if (v == COARSE_SEA) r = end ? 1.0f : -1.0f;
    else if (v == COARSE_CITY) {
        DVec2 w = world_of(p);
        int k = plan_settlement_near(w.x, w.z);
        r = end || k == c->from || k == c->to ? 1.0f : -1.0f;
    } else r = 1.0f + v / COARSE_STEPS;
    imap_put(&c->memo, key, (int)(r * 1024.0f));
    return r;
}

static int coarse_corner_dry(Coarse *c, int i, int j) {
    int64_t key = cell_key(i, j);
    int m = imap_get(&c->corner, key);
    if (m >= 0) return m;
    m = plan_road_dry_dir(dir_of(c->ch, dv(i * COARSE_CELL, j * COARSE_CELL)));
    imap_put(&c->corner, key, m);
    return m;
}

static int64_t coarse_step(void *ctx, int ai, int aj, int bi, int bj) {
    Coarse *c = (Coarse *)ctx;
    float fa = coarse_factor(c, ai, aj), fb = coarse_factor(c, bi, bj);
    if (fa < 0.0f || fb < 0.0f) return -1;
    double len = (ai != bi && aj != bj ? 1.4142135623730951 : 1.0) * COARSE_CELL;
    /* a diagonal step mustn't jump a strait */
    if (ai != bi && aj != bj && !coarse_corner_dry(c, ai > bi ? ai : bi, aj > bj ? aj : bj)) return -1;
    return (int64_t)(len * 0.5 * (fa + fb) + 0.5);
}

static int coarse_of(double v) { return (int)floor(v / COARSE_CELL); }
static int fine_of(double v) { return (int)floor(v / FINE_CELL); }

/* The water near a route: the bake's rivers, with their widths, and its
 * lakes' shores, in the route's chart, each segment filed under every square
 * of a grid its bounds touch (a river's widened by its half width), so a
 * line crosses, or comes into a river's water from, only segments filed in
 * the squares it passes through (water_hits walks them). */
typedef struct WaterSeg { DVec2 a, b; double w; int kind; } WaterSeg;
typedef struct WaterHit { double t, w, sin; uint32_t id; int kind, near; } WaterHit;
typedef struct WaterGrid {
    const TerrainChart *ch;
    double      cell;
    const IMap *corridor;   /* only the squares in these coarse cells (FINE_PER squares a side), or NULL for all */
    IMap        seen;       /* the bake's segment -> its own */
    WaterSeg   *seg;
    uint32_t   *id;
    int         nseg, capseg, capid;
    IMap        head;       /* square -> its first entry */
    int        *next, *which;
    int         nent, capnext, capwhich;
    WaterHit   *hit;        /* water_hits' */
    int         nhit, caphit;
} WaterGrid;

static void water_init(WaterGrid *g, const TerrainChart *ch, double cell, const IMap *corridor) {
    memset(g, 0, sizeof(*g));
    g->ch = ch;
    g->cell = cell;
    g->corridor = corridor;
    imap_init(&g->seen, 1 << 10);
    imap_init(&g->head, 1 << 10);
}

static void water_free(WaterGrid *g) {
    imap_free(&g->seen);
    imap_free(&g->head);
    free(g->seg); free(g->id); free(g->next); free(g->which); free(g->hit);
}

static void water_gather(void *ctx, uint32_t k, TerrainDir a, TerrainDir b, double width, int kind) {
    WaterGrid *g = (WaterGrid *)ctx;
    if (imap_get(&g->seen, (int64_t)k) >= 0) return;
    imap_put(&g->seen, (int64_t)k, g->nseg);
    WaterSeg sg;
    sg.a = in_chart(g->ch, a);
    sg.b = in_chart(g->ch, b);
    sg.w = width;
    sg.kind = kind;
    int e = g->nseg++;
    g->seg = (WaterSeg *)grow(g->seg, &g->capseg, g->nseg, sizeof(WaterSeg));
    g->id = (uint32_t *)grow(g->id, &g->capid, g->nseg, sizeof(uint32_t));
    g->seg[e] = sg;
    g->id[e] = k;
    double m = (kind == 1 ? 0.5 * width : 0.0) + 1.0;
    int i0 = (int)floor((fmin(sg.a.x, sg.b.x) - m) / g->cell), i1 = (int)floor((fmax(sg.a.x, sg.b.x) + m) / g->cell);
    int j0 = (int)floor((fmin(sg.a.z, sg.b.z) - m) / g->cell), j1 = (int)floor((fmax(sg.a.z, sg.b.z) + m) / g->cell);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            if (g->corridor && imap_get(g->corridor, cell_key((int)floor((double)i / FINE_PER), (int)floor((double)j / FINE_PER))) < 0) continue;
            int n = g->nent++;
            g->next = (int *)grow(g->next, &g->capnext, g->nent, sizeof(int));
            g->which = (int *)grow(g->which, &g->capwhich, g->nent, sizeof(int));
            int64_t key = cell_key(i, j);
            g->next[n] = imap_get(&g->head, key);
            g->which[n] = e;
            imap_put(&g->head, key, n);
        }
}

/* The water within `radius` of p, gathered into the grid. */
static void water_near(WaterGrid *g, DVec2 p, double radius) {
    terrain_earth_water_near(dir_of(g->ch, p), radius, (1u << 1) | (1u << 2), water_gather, g);
}

static int hit_by_id(const void *pa, const void *pb) {
    const WaterHit *a = (const WaterHit *)pa, *b = (const WaterHit *)pb;
    if (a->id != b->id) return a->id < b->id ? -1 : 1;
    return a->t < b->t ? -1 : a->t > b->t ? 1 : 0;
}
static int hit_by_t(const void *pa, const void *pb) {
    const WaterHit *a = (const WaterHit *)pa, *b = (const WaterHit *)pb;
    if (a->t != b->t) return a->t < b->t ? -1 : 1;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

/* The distance from p to segment a b, and how far along it the nearest point is (0 to 1). */
static double near_dist(DVec2 p, DVec2 a, DVec2 b, double *t) {
    DVec2 ab = dsub(b, a);
    double l2 = ddot(ab, ab), u = l2 > 0.0 ? ddot(dsub(p, a), ab) / l2 : 0.0;
    u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
    if (t) *t = u;
    return dlen(dsub(p, dadd(a, dscale(ab, u))));
}

/* Whether p lies in a river's water (within its half width of its line). */
static int water_in_river(const WaterGrid *g, DVec2 p) {
    for (int k = imap_get(&g->head, cell_key((int)floor(p.x / g->cell), (int)floor(p.z / g->cell))); k >= 0; k = g->next[k]) {
        const WaterSeg *sg = &g->seg[g->which[k]];
        if (sg->kind == 1 && near_dist(p, sg->a, sg->b, NULL) < 0.5 * sg->w) return 1;
    }
    return 0;
}

/* The water's segments the line p q crosses, each once (t along it, 0 to 1,
 * and the crossing's sine), in order along it, into g->hit; returns how
 * many. With `near`, also the rivers it comes into the water of without
 * crossing their lines (sine 0, `near` set), at its nearest to each. The
 * squares it passes through are walked one by one. A segment's end counts
 * as the next one's start, so a run is crossed once there. */
static int water_hits(WaterGrid *g, DVec2 p, DVec2 q, int near) {
    g->nhit = 0;
    if (!g->nseg) return 0;
    const double c = g->cell;
    int i = (int)floor(p.x / c), j = (int)floor(p.z / c), gi = (int)floor(q.x / c), gj = (int)floor(q.z / c);
    DVec2 r = dsub(q, p);
    double rl = dlen(r);
    if (rl < 1e-9) return 0;
    int si = r.x > 0.0 ? 1 : -1, sj = r.z > 0.0 ? 1 : -1;
    double tdx = fabs(r.x) > 1e-12 ? c / fabs(r.x) : 1e300, tdz = fabs(r.z) > 1e-12 ? c / fabs(r.z) : 1e300;
    double tx = fabs(r.x) > 1e-12 ? (si > 0 ? (i + 1) * c - p.x : p.x - i * c) / fabs(r.x) : 1e300;
    double tz = fabs(r.z) > 1e-12 ? (sj > 0 ? (j + 1) * c - p.z : p.z - j * c) / fabs(r.z) : 1e300;
    for (int guard = abs(gi - i) + abs(gj - j) + 2; guard > 0; --guard) {
        for (int k = imap_get(&g->head, cell_key(i, j)); k >= 0; k = g->next[k]) {
            int e = g->which[k];
            const WaterSeg *sg = &g->seg[e];
            DVec2 sv = dsub(sg->b, sg->a);
            double den = dcross(r, sv);
            DVec2 qp = dsub(sg->a, p);
            double t = fabs(den) < 1e-9 ? -1.0 : dcross(qp, sv) / den, u = fabs(den) < 1e-9 ? -1.0 : dcross(qp, r) / den;
            if (t < 0.0 || t > 1.0 || u < 0.0 || u >= 1.0) {
                if (!near || sg->kind != 1) continue;
                /* the nearest the two segments come: from an end of one to the other */
                double ta, tb, ua, ub, d0 = near_dist(sg->a, p, q, &ta), d1 = near_dist(sg->b, p, q, &tb);
                double d2 = near_dist(p, sg->a, sg->b, &ua), d3 = near_dist(q, sg->a, sg->b, &ub), d = fmin(fmin(d0, d1), fmin(d2, d3));
                if (d >= 0.5 * sg->w) continue;
                t = d == d0 ? ta : d == d1 ? tb : d == d2 ? 0.0 : 1.0;
                g->hit = (WaterHit *)grow(g->hit, &g->caphit, g->nhit + 1, sizeof(WaterHit));
                g->hit[g->nhit++] = (WaterHit){ t, sg->w, 0.0, g->id[e], sg->kind, 1 };
                continue;
            }
            g->hit = (WaterHit *)grow(g->hit, &g->caphit, g->nhit + 1, sizeof(WaterHit));
            g->hit[g->nhit++] = (WaterHit){ t, sg->w, fabs(den) / (rl * dlen(sv)), g->id[e], sg->kind, 0 };
        }
        if (i == gi && j == gj) break;
        if (tx < tz) { tx += tdx; i += si; } else { tz += tdz; j += sj; }
    }
    if (g->nhit > 1) {
        qsort(g->hit, (size_t)g->nhit, sizeof(WaterHit), hit_by_id);
        int m = 1;
        for (int k = 1; k < g->nhit; ++k)
            if (g->hit[k].id != g->hit[m - 1].id) g->hit[m++] = g->hit[k];
        g->nhit = m;
        qsort(g->hit, (size_t)g->nhit, sizeof(WaterHit), hit_by_t);
    }
    return g->nhit;
}

/* What the water costs a road along p q: each river it crosses BRIDGE_FIXED
 * and BRIDGE_PER_M for each metre of its bridge (the river's width over the
 * crossing's sine, which counts as no less than BRIDGE_SIN_MIN); each
 * stretch over a lake, between the shores it crosses taken in pairs (p and q
 * are on land), the same by its length. */
static int64_t water_cost(WaterGrid *g, DVec2 p, DVec2 q) {
    int n = water_hits(g, p, q, 0);
    if (!n) return 0;
    double len = dlen(dsub(q, p)), c = 0.0, in = -1.0;
    for (int k = 0; k < n; ++k) {
        const WaterHit *h = &g->hit[k];
        if (h->kind == 1) c += BRIDGE_FIXED + BRIDGE_PER_M * h->w / fmax(h->sin, BRIDGE_SIN_MIN);
        else if (in < 0.0) in = h->t;
        else { c += BRIDGE_FIXED + BRIDGE_PER_M * (h->t - in) * len; in = -1.0; }
    }
    if (in >= 0.0) c += BRIDGE_FIXED + BRIDGE_PER_M * (1.0 - in) * len;
    return (int64_t)(c + 0.5);
}

/* The fine grid: heights for grades, the mountains, canyons along and across.
 * A cell is open on land (1) or, as a last resort (Fine.wet), in a river's
 * water or a lake (2). */
typedef struct FineCell { float h, mountain, canyon; int open; } FineCell;

typedef struct Fine {
    const TerrainChart *ch;
    int       from, to;
    int       wet;          /* the water's cells are open too, each metre across them a bridge's */
    IMap      corridor;     /* coarse cells the route may use */
    IMap      memo;
    FineCell *cell;
    int       n, cap;
    int       si, sj, gi, gj;
    /* the canyons near the corridor, and their segments by fine cell */
    TerrainCanyon *canyon;
    int       ncanyon, capcanyon;
    IMap      seghead;      /* fine cell -> first entry */
    int      *segnext, *segcanyon, *segk;
    int       nseg, capseg, capseg2, capseg3;
    WaterGrid water;        /* the rivers and lakes near the corridor, by fine cell */
} Fine;

static const FineCell *fine_cell(Fine *f, int i, int j) {
    int64_t key = cell_key(i, j);
    int k = imap_get(&f->memo, key);
    if (k >= 0) return &f->cell[k];
    double x = (i + 0.5) * FINE_CELL, z = (j + 0.5) * FINE_CELL;
    FineCell c = { 0.0f, 0.0f, 0.0f, 0 };
    int end = (i == f->si && j == f->sj) || (i == f->gi && j == f->gj);
    int inside = imap_get(&f->corridor, cell_key((int)floor((double)i / FINE_PER), (int)floor((double)j / FINE_PER))) >= 0;
    TerrainDir p = dir_of(f->ch, dv(x, z));
    int water = -1;
    if (end || (inside && passable(p, world_of(p), f->from, f->to) &&
                (!(water = water_in_river(&f->water, dv(x, z)) || terrain_earth_shore(p) < 0.0) || f->wet))) {   /* not in a river's water or a lake */
        TerrainLandform lf;
        terrain_landform_dir(p, &lf);
        c.h = (float)terrain_height_dry_dir(p, 250.0);
        c.mountain = lf.w[TERRAIN_MOUNTAINS];
        c.canyon = lf.canyon;
        c.open = water > 0 ? 2 : 1;
    }
    f->cell = (FineCell *)grow(f->cell, &f->cap, f->n + 1, sizeof(FineCell));
    f->cell[f->n] = c;
    imap_put(&f->memo, key, f->n);
    return &f->cell[f->n++];
}

/* How deep the canyons are where the segment p-q crosses their centre lines (m). */
static double crossing_depth(const Fine *f, DVec2 p, DVec2 q, int ci, int cj, int di, int dj) {
    double deepest = 0.0;
    DVec2 r = dsub(q, p);
    for (int pass = 0; pass < 2; ++pass) {
        int k = imap_get(&f->seghead, cell_key(pass ? di : ci, pass ? dj : cj));
        for (; k >= 0; k = f->segnext[k]) {
            const TerrainCanyon *cy = &f->canyon[f->segcanyon[k]];
            int s = f->segk[k];
            DVec2 a = dv(cy->x[s], cy->z[s]), sv = dv(cy->x[s + 1] - cy->x[s], cy->z[s + 1] - cy->z[s]);
            double den = dcross(r, sv);
            if (fabs(den) < 1e-9) continue;
            DVec2 qp = dsub(a, p);
            double t = dcross(qp, sv) / den, u = dcross(qp, r) / den;
            if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0) continue;
            deepest = fmax(deepest, cy->depth[s] + (cy->depth[s + 1] - cy->depth[s]) * u);
        }
    }
    return deepest;
}

/* A step's cost: its length, more in the mountains, up a grade beyond 3% (and
 * steeply more beyond 8%), along a canyon, a great deal across one any
 * deeper than a road could simply dip through, and over the water's cells as
 * a bridge. */
static int64_t step_cost(const FineCell *a, const FineCell *b, double len, double crossing) {
    double g = fabs((double)b->h - a->h) / len;
    double c = len * (1.0 + 1.25 * (a->mountain + b->mountain));
    c += len * (30.0 * fmax(0.0, g - 0.03) + 1500.0 * fmax(0.0, g - 0.08) * fmax(0.0, g - 0.08));
    c += len * 0.05 * (a->canyon + b->canyon);
    c += len * 0.5 * BRIDGE_PER_M * ((a->open == 2) + (b->open == 2));
    if (crossing > 10.0) c += 300.0 * crossing;
    return (int64_t)(c + 0.5);
}

static int64_t fine_step(void *ctx, int ai, int aj, int bi, int bj) {
    Fine *f = (Fine *)ctx;
    const FineCell *a = fine_cell(f, ai, aj);
    if (!a->open) return -1;
    FineCell ca = *a;   /* fine_cell may move the array */
    const FineCell *b = fine_cell(f, bi, bj);
    if (!b->open) return -1;
    double len = (ai != bi && aj != bj ? 1.4142135623730951 : 1.0) * FINE_CELL;
    DVec2 p = dv((ai + 0.5) * FINE_CELL, (aj + 0.5) * FINE_CELL), q = dv((bi + 0.5) * FINE_CELL, (bj + 0.5) * FINE_CELL);
    return step_cost(&ca, b, len, crossing_depth(f, p, q, ai, aj, bi, bj)) + water_cost(&f->water, p, q);
}

/* The canyons near the coarse route, their segments filed by every fine cell
 * their bounds touch (a segment crossing a step crosses it in one of the
 * step's two cells). */
static void file_canyons(Fine *f, const int *ci, const int *cj, int n) {
    IMap seen;
    imap_init(&seen, 256);
    TerrainCanyon near[64];
    for (int k = 0; k < n; k += 2) {
        double x = (ci[k] + 0.5) * COARSE_CELL, z = (cj[k] + 0.5) * COARSE_CELL;
        int m = terrain_canyons_near_in(f->ch, x, z, (CORRIDOR + 2) * COARSE_CELL, near, 64);
        for (int q = 0; q < m; ++q) {
            if (imap_get(&seen, (int64_t)near[q].id) >= 0) continue;
            imap_put(&seen, (int64_t)near[q].id, 1);
            f->canyon = (TerrainCanyon *)grow(f->canyon, &f->capcanyon, f->ncanyon + 1, sizeof(TerrainCanyon));
            f->canyon[f->ncanyon++] = near[q];
        }
    }
    imap_free(&seen);
    for (int c = 0; c < f->ncanyon; ++c) {
        const TerrainCanyon *cy = &f->canyon[c];
        for (int s = 0; s + 1 < TERRAIN_CANYON_POINTS; ++s) {
            if (cy->depth[s] <= 10.0f && cy->depth[s + 1] <= 10.0f) continue;   /* too shallow to cost a crossing */
            int i0 = fine_of(fmin(cy->x[s], cy->x[s + 1]) - 1.0), i1 = fine_of(fmax(cy->x[s], cy->x[s + 1]) + 1.0);
            int j0 = fine_of(fmin(cy->z[s], cy->z[s + 1]) - 1.0), j1 = fine_of(fmax(cy->z[s], cy->z[s + 1]) + 1.0);
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    int e = f->nseg++;
                    f->segnext = (int *)grow(f->segnext, &f->capseg, f->nseg, sizeof(int));
                    f->segcanyon = (int *)grow(f->segcanyon, &f->capseg2, f->nseg, sizeof(int));
                    f->segk = (int *)grow(f->segk, &f->capseg3, f->nseg, sizeof(int));
                    int64_t key = cell_key(i, j);
                    f->segnext[e] = imap_get(&f->seghead, key);
                    f->segcanyon[e] = c;
                    f->segk[e] = s;
                    imap_put(&f->seghead, key, e);
                }
        }
    }
}

static void fine_free(Fine *f) {
    imap_free(&f->corridor);
    imap_free(&f->memo);
    imap_free(&f->seghead);
    free(f->cell); free(f->canyon); free(f->segnext); free(f->segcanyon); free(f->segk);
    water_free(&f->water);
}

/* The cost of the straight line p-q over the fine grid: each cell it passes
 * through, stepped along it, the canyons it crosses and the water; -1 where
 * it leaves the open cells. */
static int64_t line_cost(Fine *f, DVec2 p, DVec2 q) {
    double len = dlen(dsub(q, p));
    int steps = (int)ceil(len / (0.5 * FINE_CELL));
    if (steps < 1) steps = 1;
    int64_t total = 0;
    FineCell prev = *fine_cell(f, fine_of(p.x), fine_of(p.z));
    if (!prev.open) return -1;
    DVec2 a = p;
    for (int k = 1; k <= steps; ++k) {
        DVec2 b = dadd(p, dscale(dsub(q, p), (double)k / steps));
        const FineCell *c = fine_cell(f, fine_of(b.x), fine_of(b.z));
        if (!c->open) return -1;
        FineCell cur = *c;
        double cross = crossing_depth(f, a, b, fine_of(a.x), fine_of(a.z), fine_of(b.x), fine_of(b.z));
        total += step_cost(&prev, &cur, len / steps, cross);
        prev = cur;
        a = b;
    }
    return total + water_cost(&f->water, p, q);
}

/* --- Routing a link ---------------------------------------------------------------------------- */

static TerrainDir node_dir(const Net *net, int k) { return net->node[k].dir; }
/* Whether a junction at p would stand in or beside the bake's rivers and
 * lakes, within JUNCTION_DRY of their water: a road's bridges stop short of
 * its ends, so its junctions are on dry land. */
#define JUNCTION_DRY 50.0
static int junction_wet(TerrainDir p) {
    TerrainEarthWater w;
    return terrain_earth_water(p, &w) && !w.stream && w.edge < JUNCTION_DRY;
}
static int node_settlement(const Net *net, int k) { return net->node[k].settlement; }
/* Node k in chart c. */
static DVec2 node_in(const Net *net, int k, const TerrainChart *c) { return in_chart(c, net->node[k].dir); }
static double node_m(const Net *net, int a, int b) { return sphere_m(net->node[a].dir, net->node[b].dir); }

/* The coarse route's cost between two nodes in chart ch (and its cells, if
 * wanted), or -1 (also if it would cost more than `limit` times the straight
 * line, when that is above 0). */
static int64_t coarse_route(const Net *net, int a, int b, const TerrainChart *ch, double limitFactor, int **ci, int **cj, int *n) {
    Coarse c;
    c.ch = ch;
    c.from = node_settlement(net, a);
    c.to = node_settlement(net, b);
    DVec2 pa = node_in(net, a, ch), pb = node_in(net, b, ch);
    c.si = coarse_of(pa.x); c.sj = coarse_of(pa.z);
    c.gi = coarse_of(pb.x); c.gj = coarse_of(pb.z);
    imap_init(&c.memo, 1 << 12);
    imap_init(&c.corner, 1 << 10);
    int *pi, *pj, np;
    int64_t limit = limitFactor > 0.0 ? (int64_t)(limitFactor * dlen(dsub(pb, pa))) + (int64_t)(4.0 * COARSE_CELL) : INT64_MAX / 4;
    int64_t cost = astar(c.si, c.sj, c.gi, c.gj, COARSE_CELL, 1.4, limit, coarse_step, &c, &pi, &pj, &np);
    imap_free(&c.memo);
    imap_free(&c.corner);
    if (ci) { *ci = pi; *cj = pj; *n = np; }
    else { free(pi); free(pj); }
    return cost;
}

/* Pulls the fine route straight: from each kept point, on to a later point
 * (within PULL_REACH) the straight line to which costs no more than the
 * route between them, and no more than PULL_SLACK over it: as far as
 * doubling the reach keeps finding such points, then bisecting between the
 * last that did and the first that didn't. */
#define PULL_REACH 64
#define PULL_SLACK 1.03
static int pull_ok(Fine *f, const DVec2 *p, const int64_t *g, int i, int j) {
    int64_t c = line_cost(f, p[i], p[j]);
    return c >= 0 && (double)c <= PULL_SLACK * (double)(g[j] - g[i]) + 1.0;
}

static int pull_straight(Fine *f, const DVec2 *p, const int64_t *g, int n, DVec2 *out) {
    int m = 0, i = 0;
    out[m++] = p[0];
    while (i < n - 1) {
        int hi = i + PULL_REACH < n - 1 ? i + PULL_REACH : n - 1, good = i + 1, bad = hi + 1;
        for (int span = 2; i + span <= hi; span *= 2) {
            if (pull_ok(f, p, g, i, i + span)) good = i + span;
            else { bad = i + span; break; }
        }
        if (bad == hi + 1 && good < hi) {
            if (pull_ok(f, p, g, i, hi)) good = hi;
            else bad = hi;
        }
        while (bad - good > 1) {
            int mid = (good + bad) / 2;
            if (pull_ok(f, p, g, i, mid)) good = mid;
            else bad = mid;
        }
        out[m++] = p[good];
        i = good;
    }
    return m;
}

typedef struct RouteJob { Net *net; int *links; int n; int coarseOnly; } RouteJob;

/* Routes link k in its chart: coarse, then fine within the corridor round it
 * (widened if that finds nothing, and at last with the water open: a river
 * too wide for a fine cell, a lake's long arm), pulled straight. Its points
 * run from the centre of a's cell to b's, ending at the nodes themselves. */
static void route_link(Net *net, Link *L) {
    int *ci, *cj, nc;
    L->ok = 0;
    L->ch = link_chart(node_dir(net, L->a), node_dir(net, L->b));
    int64_t cc = coarse_route(net, L->a, L->b, &L->ch, 0.0, &ci, &cj, &nc);
    if (cc < 0) return;
    DVec2 pa = node_in(net, L->a, &L->ch), pb = node_in(net, L->b, &L->ch);
    for (int widen = 0; widen < 4 && !L->ok; ++widen) {
        Fine f;
        memset(&f, 0, sizeof(f));
        f.ch = &L->ch;
        f.wet = widen == 3;
        f.from = node_settlement(net, L->a);
        f.to = node_settlement(net, L->b);
        f.si = fine_of(pa.x); f.sj = fine_of(pa.z);
        f.gi = fine_of(pb.x); f.gj = fine_of(pb.z);
        imap_init(&f.corridor, 1 << 10);
        imap_init(&f.memo, 1 << 14);
        imap_init(&f.seghead, 1 << 12);
        int r = CORRIDOR << (widen < 3 ? widen : 1);
        for (int k = 0; k < nc; ++k)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) imap_put(&f.corridor, cell_key(ci[k] + di, cj[k] + dj), 1);
        file_canyons(&f, ci, cj, nc);
        water_init(&f.water, &L->ch, FINE_CELL, &f.corridor);
        for (int k = 0; k < nc; k = k + 2 < nc || k == nc - 1 ? k + 2 : nc - 1)
            water_near(&f.water, dv((ci[k] + 0.5) * COARSE_CELL, (cj[k] + 0.5) * COARSE_CELL), (r + 1.5) * COARSE_CELL);
        int *pi, *pj, np;
        int64_t cost = astar(f.si, f.sj, f.gi, f.gj, FINE_CELL, 1.5, INT64_MAX / 4, fine_step, &f, &pi, &pj, &np);
        if (cost >= 0) {
            DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(np + 2));
            int64_t *g = (int64_t *)malloc(sizeof(int64_t) * (size_t)(np + 2));
            if (!p || !g) fatal("out of memory routing the roads");
            for (int k = 0; k < np; ++k) p[k] = dv((pi[k] + 0.5) * FINE_CELL, (pj[k] + 0.5) * FINE_CELL);
            /* the cost along the route, for pulling it straight */
            g[0] = 0;
            for (int k = 1; k < np; ++k) {
                int64_t c = fine_step(&f, pi[k - 1], pj[k - 1], pi[k], pj[k]);
                g[k] = g[k - 1] + (c < 0 ? 0 : c);
            }
            p[0] = pa;
            p[np - 1] = pb;
            DVec2 *out = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(np + 2));
            if (!out) fatal("out of memory routing the roads");
            int m = np > 1 ? pull_straight(&f, p, g, np, out) : 0;
            if (np == 1) { out[0] = pa; out[1] = pb; m = 2; }
            L->pts = out;
            L->npts = m;
            L->cost = cost;
            L->ok = 1;
            free(p); free(g); free(pi); free(pj);
        }
        fine_free(&f);
    }
    free(ci); free(cj);
}

static void route_job(void *ctx, int i) {
    RouteJob *j = (RouteJob *)ctx;
    Link *L = &j->net->link[j->links[i]];
    if (j->coarseOnly) {
        TerrainChart ch = link_chart(node_dir(j->net, L->a), node_dir(j->net, L->b));
        L->cost = coarse_route(j->net, L->a, L->b, &ch, ROUTE_LIMIT, NULL, NULL, NULL);
        L->ok = L->cost >= 0;
    } else {
        route_link(j->net, L);
    }
}

/* Routes the links (each on its own, so the order doesn't change any route),
 * the longest first so that no long one is left running alone at the end. */
static void route_all(Net *net, int *links, int n, int coarseOnly, int threads) {
    int *order = (int *)malloc(sizeof(int) * (size_t)n);
    double *len = (double *)malloc(sizeof(double) * (size_t)n);
    if (!order || !len) fatal("out of memory routing the roads");
    for (int k = 0; k < n; ++k) {
        const Link *L = &net->link[links[k]];
        len[k] = node_m(net, L->a, L->b);
        order[k] = links[k];
        for (int q = k; q > 0 && len[q] > len[q - 1]; --q) {
            double t = len[q]; len[q] = len[q - 1]; len[q - 1] = t;
            int o = order[q]; order[q] = order[q - 1]; order[q - 1] = o;
        }
    }
    RouteJob j = { net, order, n, coarseOnly };
    thread_parallel_for(n, threads, route_job, &j);
    free(order);
    free(len);
}

/* --- Candidates: the Delaunay triangulation ------------------------------------------------------ */

typedef struct Tri { int a, b, c; double cx, cz, r2; } Tri;

static Tri make_tri(const DVec2 *p, int a, int b, int c) {
    Tri t = { a, b, c, 0, 0, 0 };
    double ax = p[a].x, az = p[a].z, bx = p[b].x, bz = p[b].z, cx = p[c].x, cz = p[c].z;
    double d = 2.0 * (ax * (bz - cz) + bx * (cz - az) + cx * (az - bz));
    double a2 = ax * ax + az * az, b2 = bx * bx + bz * bz, c2 = cx * cx + cz * cz;
    t.cx = (a2 * (bz - cz) + b2 * (cz - az) + c2 * (az - bz)) / d;
    t.cz = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
    t.r2 = (ax - t.cx) * (ax - t.cx) + (az - t.cz) * (az - t.cz);
    return t;
}

/* Bowyer-Watson over the centres of settlements ids[0 .. n) as the sphere's
 * stereographic projection from its south pole (where there are none), which
 * keeps circles circles: the plane's triangulation is the sphere's but for
 * the triangles round the pole. The pairs of neighbours (settlements i < j)
 * into pairs. */
static int delaunay(const int *ids, int n, int (*pairs)[2], int maxPairs) {
    DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(n + 3));
    int cap = 8 * n + 16, nt = 0;
    Tri *t = (Tri *)malloc(sizeof(Tri) * (size_t)cap);
    int (*edges)[2] = malloc(sizeof(int[2]) * (size_t)cap * 3);
    if (!p || !t || !edges) fatal("out of memory triangulating the settlements");
    for (int k = 0; k < n; ++k) {
        TerrainDir d = plan_settlement(ids[k]).dir;
        p[k] = dv(PLANET_RADIUS * d.x / (1.0 + d.z), PLANET_RADIUS * d.y / (1.0 + d.z));
    }
    const double big = 1e8;
    p[n] = dv(-big, -big); p[n + 1] = dv(big * 3.0, -big); p[n + 2] = dv(-big, big * 3.0);
    t[nt++] = make_tri(p, n, n + 1, n + 2);
    for (int k = 0; k < n; ++k) {
        int ne = 0;
        for (int q = 0; q < nt;) {
            double dx = p[k].x - t[q].cx, dz = p[k].z - t[q].cz;
            if (dx * dx + dz * dz < t[q].r2) {
                int v[3] = { t[q].a, t[q].b, t[q].c };
                for (int e = 0; e < 3; ++e) {
                    int a = v[e], b = v[(e + 1) % 3], dup = -1;
                    for (int s = 0; s < ne; ++s)
                        if ((edges[s][0] == b && edges[s][1] == a) || (edges[s][0] == a && edges[s][1] == b)) dup = s;
                    if (dup >= 0) { edges[dup][0] = edges[ne - 1][0]; edges[dup][1] = edges[ne - 1][1]; ne--; }
                    else { edges[ne][0] = a; edges[ne][1] = b; ne++; }
                }
                t[q] = t[--nt];
            } else {
                q++;
            }
        }
        for (int e = 0; e < ne; ++e) {
            if (nt >= cap) fatal("the settlements' triangulation outgrew its room");
            t[nt++] = make_tri(p, edges[e][0], edges[e][1], k);
        }
    }
    int np = 0;
    for (int q = 0; q < nt; ++q) {
        int v[3] = { t[q].a, t[q].b, t[q].c };
        for (int e = 0; e < 3; ++e) {
            int a = v[e], b = v[(e + 1) % 3];
            if (a >= n || b >= n) continue;
            a = ids[a]; b = ids[b];
            int lo = a < b ? a : b, hi = a < b ? b : a, dup = 0;
            for (int s = 0; s < np && !dup; ++s) dup = pairs[s][0] == lo && pairs[s][1] == hi;
            if (!dup && np < maxPairs) { pairs[np][0] = lo; pairs[np][1] = hi; np++; }
        }
    }
    free(p); free(t); free(edges);
    return np;
}

/* --- Network design ------------------------------------------------------------------------- */

static double settlement_weight(int k) { double km = plan_settlement(k).size_km; return km * km / 100.0; }

/* The gravity model's traffic between settlements i and j. */
static double gravity(int i, int j) {
    double d = sphere_m(plan_settlement(i).dir, plan_settlement(j).dir) / GRAVITY_D0;
    d = d < 0.2 ? 0.2 : d;
    return settlement_weight(i) * settlement_weight(j) / (d * d);
}

typedef struct Cand { int a, b; int64_t cost; int used; } Cand;

static int cand_by_cost(const void *pa, const void *pb) {
    const Cand *a = (const Cand *)pa, *b = (const Cand *)pb;
    if (a->cost != b->cost) return a->cost < b->cost ? -1 : 1;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    return a->b < b->b ? -1 : a->b > b->b ? 1 : 0;
}

static int uf_find(int *parent, int x) {
    while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
    return x;
}

/* All-pairs network costs between settlements (n x n, row major), by
 * Dijkstra from each over the links in use (with a plain O(n^2) scan: n is
 * a few hundred). */
static void network_costs(int n, const Cand *c, int nc, double *D) {
    double *w = (double *)malloc(sizeof(double) * (size_t)n * n);
    char *done = (char *)malloc((size_t)n);
    if (!w || !done) fatal("out of memory designing the roads");
    for (int k = 0; k < n * n; ++k) w[k] = 1e300;
    for (int k = 0; k < nc; ++k)
        if (c[k].used) {
            double v = (double)c[k].cost;
            if (v < w[c[k].a * n + c[k].b]) w[c[k].a * n + c[k].b] = w[c[k].b * n + c[k].a] = v;
        }
    for (int s = 0; s < n; ++s) {
        double *d = &D[(size_t)s * n];
        for (int k = 0; k < n; ++k) { d[k] = 1e300; done[k] = 0; }
        d[s] = 0.0;
        for (;;) {
            int u = -1;
            for (int k = 0; k < n; ++k)
                if (!done[k] && d[k] < 1e299 && (u < 0 || d[k] < d[u])) u = k;
            if (u < 0) break;
            done[u] = 1;
            for (int v = 0; v < n; ++v)
                if (w[u * n + v] < 1e299 && d[u] + w[u * n + v] < d[v]) d[v] = d[u] + w[u * n + v];
        }
    }
    free(w);
    free(done);
}

/* The links: a spanning tree of the candidates per continent, then those the
 * gravity model finds worth adding. */
static int design(Cand *c, int nc) {
    const int n = plan_settlement_count();
    qsort(c, (size_t)nc, sizeof(Cand), cand_by_cost);
    int parent[PLAN_MAX_SETTLEMENTS], deg[PLAN_MAX_SETTLEMENTS] = { 0 }, roaded = 0;
    for (int k = 0; k < n; ++k) roaded += plan_settlement(k).continent > 0;
    for (int k = 0; k < n; ++k) parent[k] = k;
    int used = 0;
    for (int k = 0; k < nc; ++k) {
        if (c[k].cost < 0) continue;
        int ra = uf_find(parent, c[k].a), rb = uf_find(parent, c[k].b);
        if (ra == rb) continue;
        parent[ra] = rb;
        c[k].used = 1;
        deg[c[k].a]++; deg[c[k].b]++;
        used++;
    }
    double *D = (double *)malloc(sizeof(double) * (size_t)n * n);
    if (!D) fatal("out of memory designing the roads");
    network_costs(n, c, nc, D);
    int budget = (int)(ADD_PER_CITY * roaded), added = 0;
    while (added < budget) {
        int best = -1;
        double bestScore = ADD_SCORE_MIN;
        for (int k = 0; k < nc; ++k) {
            if (c[k].used || c[k].cost <= 0) continue;
            double net = D[c[k].a * n + c[k].b], direct = (double)c[k].cost;
            if (net < DETOUR_MIN * direct) continue;
            double pref = pow((1.0 + deg[c[k].a]) * (1.0 + deg[c[k].b]), PREFER);
            double score = gravity(c[k].a, c[k].b) * (fmin(net, 4.0 * direct) / direct - 1.0) * pref;
            score = floor(score * 65536.0 + 0.5) / 65536.0;   /* quantised: see the top */
            if (score > bestScore) { bestScore = score; best = k; }
        }
        if (best < 0) break;
        c[best].used = 1;
        deg[c[best].a]++; deg[c[best].b]++;
        used++;
        added++;
        /* the new link shortens whatever is cheaper through it */
        int u = c[best].a, v = c[best].b;
        double w = (double)c[best].cost;
        for (int i = 0; i < n; ++i) {
            double iu = D[i * n + u], iv = D[i * n + v];
            if (iu >= 1e299 && iv >= 1e299) continue;
            for (int j = 0; j < n; ++j) {
                double t = fmin(iu + w + D[v * n + j], iv + w + D[u * n + j]);
                if (t < D[i * n + j]) D[i * n + j] = t;
            }
        }
    }
    free(D);
    /* A colossal city is a hub: at least HUB_LINKS links, its cheapest
     * candidates first, where it has them. */
    for (int s = 0; s < n; ++s)
        for (int k = 0; k < nc && deg[s] < HUB_LINKS && plan_settlement(s).colossal; ++k) {
            if (c[k].used || c[k].cost < 0 || (c[k].a != s && c[k].b != s)) continue;
            c[k].used = 1;
            deg[c[k].a]++; deg[c[k].b]++;
            used++;
        }
    return used;
}

/* The Fermat point of a triangle (the point the sum of whose distances to the
 * corners is least), by Weiszfeld's iteration from the centroid. */
static DVec2 fermat(DVec2 a, DVec2 b, DVec2 c) {
    DVec2 p = dscale(dadd(dadd(a, b), c), 1.0 / 3.0);
    for (int it = 0; it < 200; ++it) {
        double wa = 1.0 / fmax(dlen(dsub(p, a)), 1.0), wb = 1.0 / fmax(dlen(dsub(p, b)), 1.0), wc = 1.0 / fmax(dlen(dsub(p, c)), 1.0);
        DVec2 q = dscale(dadd(dadd(dscale(a, wa), dscale(b, wb)), dscale(c, wc)), 1.0 / (wa + wb + wc));
        if (dlen(dsub(q, p)) < 1.0) { p = q; break; }
        p = q;
    }
    return p;
}

static int add_node(Net *net, TerrainDir d, int settlement) {
    if (net->nnodes >= MAX_NODES) fatal("the road network outgrew its nodes");
    net->node[net->nnodes].dir = d;
    net->node[net->nnodes].p = world_of(d);
    net->node[net->nnodes].settlement = settlement;
    return net->nnodes++;
}

static int add_link(Net *net, int a, int b) {
    if (net->nlinks >= MAX_LINKS) fatal("the road network outgrew its links");
    Link *L = &net->link[net->nlinks];
    memset(L, 0, sizeof(*L));
    L->a = a; L->b = b;
    return net->nlinks++;
}

/* Junctions where two links leave a node close together (step 4): the best
 * saving first, while any saves STEINER_SAVE of their length. Each node's
 * triangles are worked out in a chart about it. */
static void steiner(Net *net) {
    int *start = (int *)malloc(sizeof(int) * (MAX_NODES + 1)), *incidence = (int *)malloc(sizeof(int) * 2 * MAX_LINKS);
    if (!start || !incidence) fatal("out of memory joining the roads");
    for (int round = 0; round < 4 * plan_settlement_count(); ++round) {
        /* each node's links, in link order */
        for (int v = 0; v <= net->nnodes; ++v) start[v] = 0;
        for (int k = 0; k < net->nlinks; ++k) { start[net->link[k].a + 1]++; start[net->link[k].b + 1]++; }
        for (int v = 0; v < net->nnodes; ++v) start[v + 1] += start[v];
        for (int k = 0; k < net->nlinks; ++k) { incidence[start[net->link[k].a]++] = k; incidence[start[net->link[k].b]++] = k; }
        for (int v = net->nnodes; v > 0; --v) start[v] = start[v - 1];
        start[0] = 0;
        double bestSave = 0.0;
        int bl1 = -1, bl2 = -1, bv = -1;
        TerrainDir bf = { 0.0, 0.0, 0.0 };
        for (int v = 0; v < net->nnodes; ++v) {
            int ni = 0, *inc = &incidence[start[v]];
            for (int k = start[v]; k < start[v + 1]; ++k) ni++;
            /* merging two of a node's roads takes one from it: a junction must
             * keep three, a colossal city its HUB_LINKS */
            if (net->node[v].settlement < 0 && ni < 4) continue;
            if (net->node[v].settlement >= 0 && plan_settlement(net->node[v].settlement).colossal && ni <= HUB_LINKS) continue;
            TerrainChart cv = terrain_chart_about(node_dir(net, v), 0.0);
            for (int x = 0; x < ni; ++x)
                for (int y = x + 1; y < ni; ++y) {
                    const Link *l1 = &net->link[inc[x]], *l2 = &net->link[inc[y]];
                    int a = l1->a == v ? l1->b : l1->a, b = l2->a == v ? l2->b : l2->a;
                    if (a == b) continue;
                    DVec2 pv = node_in(net, v, &cv), pa = node_in(net, a, &cv), pb = node_in(net, b, &cv);
                    DVec2 ua = dnorm(dsub(pa, pv)), ub = dnorm(dsub(pb, pv));
                    if (ddot(ua, ub) <= -0.5) continue;   /* 120 degrees or more apart: no junction helps */
                    DVec2 f = fermat(pv, pa, pb);
                    double before = dlen(dsub(pa, pv)) + dlen(dsub(pb, pv));
                    double save = before - (dlen(dsub(f, pv)) + dlen(dsub(f, pa)) + dlen(dsub(f, pb)));
                    save = floor(save + 0.5);
                    if (save < STEINER_SAVE * before || save <= bestSave) continue;
                    TerrainDir fd = dir_of(&cv, f);
                    DVec2 fw = world_of(fd);
                    if (!plan_road_dry_dir(fd) || plan_city_edge(fw.x, fw.z) < 2.0 * CITY_CLEAR || junction_wet(fd)) continue;
                    bestSave = save; bl1 = inc[x]; bl2 = inc[y]; bv = v; bf = fd;
                }
        }
        if (bl1 < 0) break;
        Link *l1 = &net->link[bl1], *l2 = &net->link[bl2];
        int a = l1->a == bv ? l1->b : l1->a, b = l2->a == bv ? l2->b : l2->a;
        int j = add_node(net, bf, -1);
        /* the first link becomes the shared stem, the second a branch; one more branch */
        l1->a = bv; l1->b = j;
        l2->a = j; l2->b = b;
        add_link(net, j, a);
    }
    free(start);
    free(incidence);
}

/* --- Crossings ------------------------------------------------------------------------------- */

static double link_length(const Link *L) {
    double s = 0.0;
    for (int k = 0; k + 1 < L->npts; ++k) s += dlen(dsub(L->pts[k + 1], L->pts[k]));
    return s;
}

/* How far along from each end a link is still its settlement's there: up to
 * the farthest (from that end) it comes within CITY_CLEAR of that
 * settlement's footprint, sampled every SPAN_STEP; nothing at a junction.
 * Out of a city 60-150 km across a route may leave round a bay or a range
 * and come back in; until it leaves for the last time it is the city's (its
 * gate cuts it there), so no junction is made on that stretch. *s0 from the
 * start, *s1 from the end (m). */
#define SPAN_STEP 500.0
static void end_spans(const Net *net, const Link *L, double *s0, double *s1) {
    int sa = net->node[L->a].settlement, sb = net->node[L->b].settlement;
    double first = -1.0, s = 0.0, at = 0.0;
    *s0 = *s1 = 0.0;
    if (sa < 0 && sb < 0) return;
    for (int k = 0; k + 1 < L->npts; ++k) {
        double len = dlen(dsub(L->pts[k + 1], L->pts[k]));
        for (; at <= s + len; at += SPAN_STEP) {
            DVec2 p = dadd(L->pts[k], dscale(dsub(L->pts[k + 1], L->pts[k]), len > 0.0 ? (at - s) / len : 0.0));
            DVec2 w = world_of(dir_of(&L->ch, p));
            if (plan_city_edge(w.x, w.z) >= CITY_CLEAR) continue;
            int near = plan_settlement_near(w.x, w.z);
            if (near == sa) *s0 = at + SPAN_STEP;
            if (near == sb && first < 0.0) first = at;
        }
        s += len;
    }
    if (first >= 0.0) *s1 = s - first + SPAN_STEP;
}

typedef struct Cross { int link, seg; double t; int node; } Cross;

static int cross_cmp(const void *pa, const void *pb) {
    const Cross *a = (const Cross *)pa, *b = (const Cross *)pb;
    if (a->link != b->link) return a->link - b->link;
    if (a->seg != b->seg) return a->seg - b->seg;
    return a->t < b->t ? -1 : a->t > b->t ? 1 : 0;
}

static int key_cmp(const void *pa, const void *pb) {
    int64_t a = *(const int64_t *)pa, b = *(const int64_t *)pb;
    return a < b ? -1 : a > b;
}

/* The world chart's squares of side `cell` that segment a-b of chart ch
 * comes within `reach` of (m on the sphere), each once and sorted, into
 * *keys (grown as needed); returns how many. Stepped along no more than a
 * quarter square apart in the world's chart, each step filed as far round as
 * the chart's stretch there takes the reach and half a step. */
static int seg_squares(const TerrainChart *ch, DVec2 a, DVec2 b, double reach, double cell, int64_t **keys, int *cap) {
    DVec2 wa = world_of(dir_of(ch, a)), wb = world_of(dir_of(ch, b));
    double len = dlen(dsub(b, a)), k = 1.25 * fmax(stretch_at(wa), stretch_at(wb));
    int steps = (int)ceil(len * k / (0.25 * cell)) + 1, n = 0;
    for (int q = 0; q <= steps; ++q) {
        DVec2 w = q == 0 ? wa : q == steps ? wb : world_of(dir_of(ch, dadd(a, dscale(dsub(b, a), (double)q / steps))));
        double pad = 1.01 * (reach + 0.5 * len / steps) * stretch_at(w) + 1.0;
        int i0 = (int)floor((w.x - pad) / cell), i1 = (int)floor((w.x + pad) / cell);
        int j0 = (int)floor((w.z - pad) / cell), j1 = (int)floor((w.z + pad) / cell);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                int64_t key = cell_key(i, j);
                if (n > 0 && (*keys)[n - 1] == key) continue;
                *keys = (int64_t *)grow(*keys, cap, n + 1, sizeof(int64_t));
                (*keys)[n++] = key;
            }
    }
    qsort(*keys, (size_t)n, sizeof(int64_t), key_cmp);
    int m = 0;
    for (int q = 0; q < n; ++q)
        if (m == 0 || (*keys)[q] != (*keys)[m - 1]) (*keys)[m++] = (*keys)[q];
    return m;
}

static int key_i(int64_t key) { return (int)(uint32_t)((uint64_t)key >> 32); }
static int key_j(int64_t key) { return (int)(uint32_t)key; }

static void split_links(Net *net, Cross *x, int nx);

/* Step 6: wherever two links' centre lines cross, away from their ends and
 * their settlements' stretches (end_spans), a junction; each link crossed is
 * split into pieces there. Segments are filed by the world chart's squares,
 * and a pair is crossed in the first's chart. */
static void split_crossings(Net *net) {
    const double CELL = 20000.0, END_CLEAR = 3000.0;
    IMap grid;
    imap_init(&grid, 1 << 14);
    int *next = NULL, *lk = NULL, *sg = NULL, cn = 0, c1 = 0, c2 = 0, c3 = 0, capk = 0;
    int64_t *keys = NULL;
    double (*span)[2] = malloc(sizeof(double[2]) * (size_t)(net->nlinks + 1)), **pre = malloc(sizeof(double *) * (size_t)(net->nlinks + 1));
    if (!span || !pre) fatal("out of memory crossing the roads");
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        end_spans(net, L, &span[l][0], &span[l][1]);
        pre[l] = (double *)malloc(sizeof(double) * (size_t)L->npts);
        if (!pre[l]) fatal("out of memory crossing the roads");
        pre[l][0] = 0.0;
        for (int k = 1; k < L->npts; ++k) pre[l][k] = pre[l][k - 1] + dlen(dsub(L->pts[k], L->pts[k - 1]));
        span[l][1] = pre[l][L->npts - 1] - span[l][1];   /* from here on: the stretch between them, [s0, s1] */
    }
#define IN_SPAN(l, k, t) (pre[l][k] + (t) * (pre[l][(k) + 1] - pre[l][k]) < span[l][0] || pre[l][k] + (t) * (pre[l][(k) + 1] - pre[l][k]) > span[l][1])
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        for (int s = 0; s + 1 < L->npts; ++s) {
            if (pre[l][s + 1] < span[l][0] || pre[l][s] > span[l][1]) continue;
            int nk = seg_squares(&L->ch, L->pts[s], L->pts[s + 1], 0.0, CELL, &keys, &capk);
            for (int q = 0; q < nk; ++q) {
                next = (int *)grow(next, &c1, cn + 1, sizeof(int));
                lk = (int *)grow(lk, &c2, cn + 1, sizeof(int));
                sg = (int *)grow(sg, &c3, cn + 1, sizeof(int));
                next[cn] = imap_get(&grid, keys[q]);
                lk[cn] = l; sg[cn] = s;
                imap_put(&grid, keys[q], cn++);
            }
        }
    }
    Cross *x = NULL;
    int nx = 0, cx = 0;
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        DVec2 ends[4];
        for (int q = 0; q < 4; ++q) ends[q] = node_in(net, q == 0 ? L->a : q == 1 ? L->b : 0, &L->ch);
        for (int s = 0; s + 1 < L->npts; ++s) {
            if (pre[l][s + 1] < span[l][0] || pre[l][s] > span[l][1]) continue;
            DVec2 a = L->pts[s], b = L->pts[s + 1], r = dsub(b, a);
            int nk = seg_squares(&L->ch, a, b, 0.0, CELL, &keys, &capk);
            for (int q = 0; q < nk; ++q)
                for (int e = imap_get(&grid, keys[q]); e >= 0; e = next[e]) {
                    if (lk[e] <= l) continue;   /* each pair once, from the lower link */
                    const Link *M = &net->link[lk[e]];
                    DVec2 c = rechart(&M->ch, &L->ch, M->pts[sg[e]]), sv = dsub(rechart(&M->ch, &L->ch, M->pts[sg[e] + 1]), c);
                    double den = dcross(r, sv);
                    if (fabs(den) < 1e-9) continue;
                    DVec2 qp = dsub(c, a);
                    double t = dcross(qp, sv) / den, u = dcross(qp, r) / den;
                    if (t < 0.0 || t >= 1.0 || u < 0.0 || u >= 1.0) continue;
                    if (IN_SPAN(l, s, t) || IN_SPAN(lk[e], sg[e], u)) continue;   /* a settlement's: its gate cuts it off */
                    TerrainDir pd = dir_of(&L->ch, dadd(a, dscale(r, t)));
                    DVec2 pw = world_of(pd), p = dadd(a, dscale(r, t));
                    /* once, in the square holding the crossing (both segments are filed there) */
                    if ((int)floor(pw.x / CELL) != key_i(keys[q]) || (int)floor(pw.z / CELL) != key_j(keys[q])) continue;
                    /* not where they share an end (they meet there anyway) */
                    ends[2] = node_in(net, M->a, &L->ch);
                    ends[3] = node_in(net, M->b, &L->ch);
                    int nearEnd = 0;
                    for (int k = 0; k < 4; ++k) nearEnd |= dlen(dsub(p, ends[k])) < END_CLEAR;
                    /* nor inside a settlement: both roads end there, and its gates cut them off before they meet */
                    if (nearEnd || plan_city_edge(pw.x, pw.z) < CITY_CLEAR || junction_wet(pd)) continue;
                    x = (Cross *)grow(x, &cx, nx + 2, sizeof(Cross));
                    int node = add_node(net, pd, -1);
                    x[nx++] = (Cross){ l, s, t, node };
                    x[nx++] = (Cross){ lk[e], sg[e], u, node };
                }
        }
    }
#undef IN_SPAN
    for (int l = 0; l < net->nlinks; ++l) free(pre[l]);
    free(pre); free(span);
    imap_free(&grid);
    free(next); free(lk); free(sg); free(keys);
    split_links(net, x, nx);
    free(x);
}

/* --- Roads alongside each other --------------------------------------------------------------
 * Routed each on its own, two links can run side by side for miles (through
 * one gap in the mountains, along one coast), crossing and recrossing. Taken
 * longest first, a link that runs within MERGE_D of a link already taken for
 * MERGE_MIN or more joins it there instead: it is cut where the run begins
 * and ends, each end meets the other road at a junction, and the stretch
 * between goes. Where it shares an end node with the road it joins, and has
 * kept beside it all the way from that end, the stretch from the end goes too
 * (that road serves it). Stretches inside the settlements' clearance don't
 * count: roads gather at a city. */
#define MERGE_SAMPLE 250.0
#define MERGE_D      1500.0
#define MERGE_MIN    4000.0
#define MERGE_CELL   4000.0

typedef struct SegGrid { IMap head; int *next, *link, *seg; int n, c1, c2, c3, capk; int64_t *keys; } SegGrid;

/* Files link l's segments but those wholly within its settlements' stretches (end_spans). */
static void seggrid_add(const Net *net, SegGrid *g, const Link *L, int l) {
    double s0, s1, at = 0.0, len = link_length(L);
    end_spans(net, L, &s0, &s1);
    for (int s = 0; s + 1 < L->npts; ++s) {
        double sl = dlen(dsub(L->pts[s + 1], L->pts[s]));
        at += sl;
        if (at < s0 || at - sl > len - s1) continue;
        int nk = seg_squares(&L->ch, L->pts[s], L->pts[s + 1], MERGE_D, MERGE_CELL, &g->keys, &g->capk);
        for (int q = 0; q < nk; ++q) {
            int e = g->n++;
            g->next = (int *)grow(g->next, &g->c1, g->n, sizeof(int));
            g->link = (int *)grow(g->link, &g->c2, g->n, sizeof(int));
            g->seg = (int *)grow(g->seg, &g->c3, g->n, sizeof(int));
            g->next[e] = imap_get(&g->head, g->keys[q]);
            g->link[e] = l;
            g->seg[e] = s;
            imap_put(&g->head, g->keys[q], e);
        }
    }
}

/* The nearest point to the direction p on the links filed (only link `only`
 * if >= 0; not link `skip`), within MERGE_D, measured in each link's chart:
 * its link, segment and place along it. */
static double seggrid_nearest(const Net *net, const SegGrid *g, TerrainDir pd, int skip, int only, int *link, int *seg, double *t) {
    double best = MERGE_D;
    *link = -1;
    DVec2 w = world_of(pd), pl = dv(0.0, 0.0);
    int inL = -1;   /* the link whose chart pl is p in */
    for (int e = imap_get(&g->head, cell_key((int)floor(w.x / MERGE_CELL), (int)floor(w.z / MERGE_CELL))); e >= 0; e = g->next[e]) {
        if (g->link[e] == skip || (only >= 0 && g->link[e] != only)) continue;
        const Link *L = &net->link[g->link[e]];
        if (inL != g->link[e]) { pl = in_chart(&L->ch, pd); inL = g->link[e]; }
        DVec2 p = pl, a = L->pts[g->seg[e]], ab = dsub(L->pts[g->seg[e] + 1], a);
        double l2 = ddot(ab, ab), u = l2 > 0.0 ? ddot(dsub(p, a), ab) / l2 : 0.0;
        u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
        double d = dlen(dsub(p, dadd(a, dscale(ab, u))));
        if (d < best || (d == best && *link >= 0 && g->link[e] < *link)) { best = d; *link = g->link[e]; *seg = g->seg[e]; *t = u; }
    }
    return best;
}

/* A point a distance along a link: its segment, and where on it. */
typedef struct OnLink { DVec2 p; int seg; double t; } OnLink;

static OnLink along(const Link *L, double s) {
    OnLink o = { L->pts[0], 0, 0.0 };
    for (int k = 0; k + 1 < L->npts; ++k) {
        double len = dlen(dsub(L->pts[k + 1], L->pts[k]));
        if (s <= len || k + 2 == L->npts) {
            o.seg = k;
            o.t = len > 0.0 ? fmin(fmax(s / len, 0.0), 1.0) : 0.0;
            o.p = dadd(L->pts[k], dscale(dsub(L->pts[k + 1], L->pts[k]), o.t));
            return o;
        }
        s -= len;
    }
    return o;
}

/* The piece of L from a to b (a before b along it), with `head` before it
 * and `tail` after it if given. */
static int piece(const Link *L, OnLink a, OnLink b, const DVec2 *head, const DVec2 *tail, DVec2 **out) {
    int cap = b.seg - a.seg + 5, n = 0;
    DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)cap);
    if (!p) fatal("out of memory joining the roads");
    if (head) p[n++] = *head;
    p[n++] = a.p;
    for (int v = a.seg + 1; v <= b.seg; ++v) p[n++] = L->pts[v];
    p[n++] = b.p;
    if (tail) p[n++] = *tail;
    /* no repeated points, but always both ends */
    int m = 1;
    for (int k = 1; k < n; ++k)
        if (dlen(dsub(p[k], p[m - 1])) > 0.5) p[m++] = p[k];
    if (m == 1) p[m++] = p[n - 1];
    *out = p;
    return m;
}

static int len_desc(const void *pa, const void *pb, const double *len) {
    int a = *(const int *)pa, b = *(const int *)pb;
    if (len[a] != len[b]) return len[a] > len[b] ? -1 : 1;
    return a - b;
}

static void merge_parallel(Net *net) {
    const int n0 = net->nlinks;
    double *len = (double *)malloc(sizeof(double) * MAX_LINKS);
    int *order = (int *)malloc(sizeof(int) * (size_t)n0);
    if (!len || !order) fatal("out of memory joining the roads");
    for (int l = 0; l < n0; ++l) { len[l] = net->link[l].ok ? link_length(&net->link[l]) : 0.0; order[l] = l; }
    for (int i = 1; i < n0; ++i)   /* longest first: an insertion sort, so ties stay in link order */
        for (int j = i; j > 0 && len_desc(&order[j], &order[j - 1], len) < 0; --j) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    SegGrid g;
    memset(&g, 0, sizeof(g));
    imap_init(&g.head, 1 << 14);
    Cross *cut = NULL;
    int ncut = 0, ccut = 0;
    int *nearL = NULL, *nearS = NULL, cn1 = 0, cn2 = 0;
    double *nearT = NULL;
    TerrainDir *at = NULL;   /* B's samples */
    int cn3 = 0, cn4 = 0;
    for (int oi = 0; oi < n0; ++oi) {
        int l = order[oi];
        Link B = net->link[l];
        if (!B.ok) continue;
        double L = len[l];
        int ns = (int)ceil(L / MERGE_SAMPLE) + 1;
        nearL = (int *)grow(nearL, &cn1, ns, sizeof(int));
        nearS = (int *)grow(nearS, &cn2, ns, sizeof(int));
        nearT = (double *)grow(nearT, &cn3, ns, sizeof(double));
        at = (TerrainDir *)grow(at, &cn4, ns, sizeof(TerrainDir));
        double s0, s1;
        end_spans(net, &B, &s0, &s1);
        for (int k = 0; k < ns; ++k) {
            at[k] = dir_of(&B.ch, along(&B, L * k / (ns - 1)).p);
            DVec2 w = world_of(at[k]);
            nearL[k] = -1;
            if (plan_city_edge(w.x, w.z) < CITY_CLEAR || L * k / (ns - 1) < s0 || L * k / (ns - 1) > L - s1) { nearL[k] = -2; continue; }
            seggrid_nearest(net, &g, at[k], l, -1, &nearL[k], &nearS[k], &nearT[k]);
            if (nearL[k] >= 0) {   /* where a run starts or ends is a junction: not over the water */
                const Link *LA = &net->link[nearL[k]];
                DVec2 q = dadd(LA->pts[nearS[k]], dscale(dsub(LA->pts[nearS[k] + 1], LA->pts[nearS[k]]), nearT[k]));
                if (junction_wet(dir_of(&LA->ch, q))) nearL[k] = -1;
            }
        }
        /* runs alongside one link, long enough */
        int runs[64][2], nruns = 0;
        for (int k = 0; k < ns && nruns < 64;) {
            if (nearL[k] < 0) { k++; continue; }
            int e = k;
            while (e + 1 < ns && nearL[e + 1] == nearL[k]) e++;
            if ((e - k) * (L / (ns - 1)) >= MERGE_MIN) { runs[nruns][0] = k; runs[nruns][1] = e; nruns++; }
            k = e + 1;
        }
        if (!nruns) { seggrid_add(net, &g, &net->link[l], l); continue; }
        /* The pieces between the runs: piece r from B's start or where run
         * r - 1 leaves its road, to B's end or where run r meets its road. A
         * first or last piece from an end node it shares with the run's road,
         * beside that road all the way, goes (the road serves it). A junction
         * is made only where a kept piece meets a road. */
        int kept = 0;
        for (int r = 0; r <= nruns; ++r) {
            int startK = r > 0 ? runs[r - 1][1] : 0, endK = r < nruns ? runs[r][0] : ns - 1;
            int A = r < nruns ? nearL[runs[r][0]] : nearL[runs[r - 1][0]], end = r < nruns ? B.a : B.b;
            int drop = (r == 0 || r == nruns) && (net->link[A].a == end || net->link[A].b == end);
            for (int k = r == 0 ? 0 : startK + 1; k <= (r == 0 ? endK - 1 : ns - 1) && drop; ++k) {
                if (nearL[k] == -2) continue;
                int ll, ss;
                double tt;
                seggrid_nearest(net, &g, at[k], l, A, &ll, &ss, &tt);
                drop = ll == A;
            }
            if (drop) continue;
            int from = B.a, to = B.b, haveHead = r > 0, haveTail = r < nruns;
            DVec2 head = dv(0, 0), tail = dv(0, 0);
            for (int side = 0; side < 2; ++side) {
                if (side == 0 ? !haveHead : !haveTail) continue;
                int k = side == 0 ? startK : endK, AL = nearL[side == 0 ? runs[r - 1][0] : runs[r][0]];
                const Link *LA = &net->link[AL];
                TerrainDir qd = dir_of(&LA->ch, dadd(LA->pts[nearS[k]], dscale(dsub(LA->pts[nearS[k] + 1], LA->pts[nearS[k]]), nearT[k])));
                DVec2 q = in_chart(&B.ch, qd);
                int j = add_node(net, qd, -1);
                cut = (Cross *)grow(cut, &ccut, ncut + 1, sizeof(Cross));
                cut[ncut++] = (Cross){ AL, nearS[k], nearT[k], j };
                if (side == 0) { from = j; head = q; } else { to = j; tail = q; }
            }
            OnLink a = startK == 0 ? (OnLink){ B.pts[0], 0, 0.0 } : along(&B, L * startK / (ns - 1));
            OnLink b = endK == ns - 1 ? (OnLink){ B.pts[B.npts - 1], B.npts - 2, 1.0 } : along(&B, L * endK / (ns - 1));
            DVec2 *pts;
            int np = piece(&B, a, b, haveHead ? &head : NULL, haveTail ? &tail : NULL, &pts);
            int idx = kept++ == 0 ? l : add_link(net, from, to);
            Link *P = &net->link[idx];
            P->a = from; P->b = to; P->ch = B.ch; P->pts = pts; P->npts = np; P->ok = 1;
            P->cost = (int64_t)((double)B.cost * link_length(P) / fmax(L, 1.0) + 0.5);
            seggrid_add(net, &g, P, idx);
        }
        if (!kept) { net->link[l].ok = 0; net->link[l].pts = NULL; }   /* wholly beside other roads */
        free(B.pts);
    }
    imap_free(&g.head);
    free(g.next); free(g.link); free(g.seg); free(g.keys);
    free(nearL); free(nearS); free(nearT); free(at);
    free(len); free(order);
    split_links(net, cut, ncut);
    free(cut);
}

/* Splits links at the cuts (each a junction node at a point on a link's
 * segment), each link at all of its cuts in order along it; sorts the cuts. */
static void split_links(Net *net, Cross *x, int nx) {
    if (!nx) return;
    qsort(x, (size_t)nx, sizeof(Cross), cross_cmp);
    for (int k = 0; k < nx;) {
        int l = x[k].link, e = k;
        while (e < nx && x[e].link == l) e++;
        Link old = net->link[l];
        int from = old.a, seg0 = 0;
        DVec2 start = old.pts[0];
        double oldLen = fmax(link_length(&old), 1.0);
        for (int q = k; q <= e; ++q) {
            int to = q < e ? x[q].node : old.b, seg1 = q < e ? x[q].seg : old.npts - 2;
            DVec2 end = q < e ? node_in(net, x[q].node, &old.ch) : old.pts[old.npts - 1];
            int idx = q == k ? l : add_link(net, from, to);
            Link *L = &net->link[idx];
            int np = seg1 - seg0 + 2;
            DVec2 *pts = (DVec2 *)malloc(sizeof(DVec2) * (size_t)np);
            if (!pts) fatal("out of memory splitting the roads");
            pts[0] = start;
            for (int v = seg0 + 1; v <= seg1; ++v) pts[v - seg0] = old.pts[v];
            pts[np - 1] = end;
            L->a = from; L->b = to; L->ch = old.ch; L->pts = pts; L->npts = np; L->ok = 1;
            L->cost = (int64_t)((double)old.cost * link_length(L) / oldLen + 0.5);
            from = to; start = end; seg0 = seg1;
        }
        free(old.pts);
        k = e;
    }
}

/* --- Tidying the junctions -----------------------------------------------------------------------
 * Merging can leave a junction with fewer than three roads: one with a
 * single road is a spur to nowhere and goes, with its road; one with two is
 * no junction, and its roads become one. Then the nodes no road reaches go
 * (the settlements stay: they come first, and keep their numbers). */

static void tidy_junctions(Net *net) {
    /* A link a few metres long joins two nodes at one place: they become one
     * (a settlement stays itself), and the link goes. */
    for (int l = 0; l < net->nlinks; ++l) {
        Link *L = &net->link[l];
        if (!L->ok || link_length(L) >= 5.0) continue;
        int keep = L->a, gone = L->b;
        if (net->node[gone].settlement >= 0) { keep = L->b; gone = L->a; }
        L->ok = 0;
        if (keep == gone || net->node[gone].settlement >= 0) continue;
        for (int k = 0; k < net->nlinks; ++k) {
            Link *M = &net->link[k];
            if (!M->ok) continue;
            if (M->a == gone) { M->a = keep; M->pts[0] = node_in(net, keep, &M->ch); }
            if (M->b == gone) { M->b = keep; M->pts[M->npts - 1] = node_in(net, keep, &M->ch); }
        }
    }
    int *deg = (int *)calloc((size_t)net->nnodes, sizeof(int));
    if (!deg) fatal("out of memory tidying the roads");
    for (int l = 0; l < net->nlinks; ++l)
        if (net->link[l].ok) { deg[net->link[l].a]++; deg[net->link[l].b]++; }
    for (int changed = 1; changed;) {
        changed = 0;
        for (int v = plan_settlement_count(); v < net->nnodes; ++v) {
            if (deg[v] != 1 && deg[v] != 2) continue;
            int l1 = -1, l2 = -1;
            for (int l = 0; l < net->nlinks; ++l)
                if (net->link[l].ok && (net->link[l].a == v || net->link[l].b == v)) { if (l1 < 0) l1 = l; else l2 = l; }
            Link *A = &net->link[l1];
            if (deg[v] == 1 || A->a == A->b) {   /* a spur, or a loop back to v */
                deg[A->a]--; deg[A->b]--;
                A->ok = 0;
                changed = 1;
                continue;
            }
            Link *B = &net->link[l2];
            int x = A->a == v ? A->b : A->a, y = B->a == v ? B->b : B->a;
            /* A's points ending at v, then B's from v on, in a chart about the two's ends */
            DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(A->npts + B->npts));
            if (!p) fatal("out of memory tidying the roads");
            TerrainChart ch = link_chart(node_dir(net, x), node_dir(net, y));
            int n = 0;
            for (int k = 0; k < A->npts; ++k) p[n++] = rechart(&A->ch, &ch, A->pts[A->a == v ? A->npts - 1 - k : k]);
            for (int k = 1; k < B->npts; ++k) p[n++] = rechart(&B->ch, &ch, B->pts[B->a == v ? k : B->npts - 1 - k]);
            free(A->pts);
            free(B->pts);
            A->ch = ch; A->pts = p; A->npts = n; A->a = x; A->b = y; A->cost += B->cost;
            B->ok = 0; B->pts = NULL;
            deg[v] = 0;
            changed = 1;
        }
    }
    /* the links still in use, then the nodes */
    int w = 0;
    for (int l = 0; l < net->nlinks; ++l)
        if (net->link[l].ok) net->link[w++] = net->link[l];
    net->nlinks = w;
    int *renum = (int *)malloc(sizeof(int) * (size_t)net->nnodes), m = 0;
    if (!renum) fatal("out of memory tidying the roads");
    for (int v = 0; v < net->nnodes; ++v) {
        renum[v] = v < plan_settlement_count() || deg[v] > 0 ? m : -1;
        if (renum[v] >= 0) net->node[m++] = net->node[v];
    }
    net->nnodes = m;
    for (int l = 0; l < net->nlinks; ++l) { net->link[l].a = renum[net->link[l].a]; net->link[l].b = renum[net->link[l].b]; }
    free(renum);
    free(deg);
}

/* --- Classes ---------------------------------------------------------------------------------- */

/* Step 7: every pair of settlements' traffic along the cheapest route between
 * them, then the busiest links freeways. */
static void classify(Net *net) {
    const int nn = net->nnodes, nl = net->nlinks;
    double *d = (double *)malloc(sizeof(double) * (size_t)nn);
    int *via = (int *)malloc(sizeof(int) * (size_t)nn), *done = (int *)malloc(sizeof(int) * (size_t)nn);
    int *order = (int *)malloc(sizeof(int) * (size_t)nl);
    if (!d || !via || !done || !order) fatal("out of memory classing the roads");
    for (int l = 0; l < nl; ++l) net->link[l].flow = 0.0;
    const int ns = plan_settlement_count();
    for (int s = 0; s < ns; ++s) {
        for (int k = 0; k < nn; ++k) { d[k] = 1e300; via[k] = -1; done[k] = 0; }
        d[s] = 0.0;
        for (;;) {
            int u = -1;
            for (int k = 0; k < nn; ++k)
                if (!done[k] && d[k] < 1e299 && (u < 0 || d[k] < d[u])) u = k;
            if (u < 0) break;
            done[u] = 1;
            for (int l = 0; l < nl; ++l) {
                const Link *L = &net->link[l];
                if (!L->ok || (L->a != u && L->b != u)) continue;
                int v = L->a == u ? L->b : L->a;
                double c = d[u] + (double)L->cost;
                if (c < d[v]) { d[v] = c; via[v] = l; }
            }
        }
        for (int t = s + 1; t < ns; ++t) {
            if (d[t] >= 1e299) continue;
            double T = gravity(s, t);
            for (int v = t; v != s;) {
                Link *L = &net->link[via[v]];
                L->flow += T;
                v = L->a == v ? L->b : L->a;
            }
        }
    }
    for (int l = 0; l < nl; ++l) order[l] = l;
    for (int i = 1; i < nl; ++i)   /* busiest first; insertion sort keeps equal flows in link order */
        for (int j = i; j > 0 && net->link[order[j]].flow > net->link[order[j - 1]].flow; --j) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    double total = 0.0, fw = 0.0;
    for (int l = 0; l < nl; ++l) { net->link[l].cls = PLAN_ROAD_TRUNK; if (net->link[l].ok) total += link_length(&net->link[l]); }
    for (int k = 0; k < nl && fw < FREEWAY_SHARE * total; ++k) {
        Link *L = &net->link[order[k]];
        if (!L->ok) continue;
        L->cls = PLAN_ROAD_FREEWAY;
        fw += link_length(L);
    }
    /* every colossal city on a freeway; no freeway ending at a junction */
    for (int changed = 1, round = 0; changed && round < 64; ++round) {
        changed = 0;
        for (int v = 0; v < nn; ++v) {
            int nf = 0, best = -1, any = 0;
            for (int k = 0; k < nl; ++k) {
                const Link *L = &net->link[order[k]];
                if (!L->ok || (L->a != v && L->b != v)) continue;
                any = 1;
                if (L->cls == PLAN_ROAD_FREEWAY) nf++;
                else if (best < 0) best = order[k];
            }
            int s = net->node[v].settlement;
            int want = (s >= 0 && plan_settlement(s).colossal && nf == 0 && any) || (s < 0 && nf == 1);
            if (want && best >= 0) { net->link[best].cls = PLAN_ROAD_FREEWAY; changed = 1; }
        }
    }
    free(d); free(via); free(done); free(order);
}

/* --- Corners ----------------------------------------------------------------------------------- */

/* Step 8: each corner of the pulled route rounded to the class's radius, or
 * less where its legs are short; the points along it then carry the
 * distance along. */
static void round_corners(Link *L) {
    double R = L->cls == PLAN_ROAD_FREEWAY ? FREEWAY_RADIUS : TRUNK_RADIUS;
    /* A corner turning back on itself (a route out to a point and back along
     * the same line) has no arc: the point goes, until none is left; so does
     * a point repeated. */
    for (int k = 1, m; k + 1 < L->npts; k = m > 1 ? m - 1 : 1) {
        DVec2 d1 = dsub(L->pts[k], L->pts[k - 1]), d2 = dsub(L->pts[k + 1], L->pts[k]);
        if (dlen(d1) > 1e-6 && dlen(d2) > 1e-6 && ddot(dnorm(d1), dnorm(d2)) > SPIKE_COS) { m = k + 2; continue; }
        memmove(&L->pts[k], &L->pts[k + 1], sizeof(DVec2) * (size_t)(L->npts - k - 1));
        L->npts--;
        m = k;
    }
    int cap = 0, n = 0;
    DVec2 *out = NULL;
    out = (DVec2 *)grow(out, &cap, 1, sizeof(DVec2));
    out[n++] = L->pts[0];
    for (int k = 1; k + 1 < L->npts; ++k) {
        DVec2 p = L->pts[k - 1], v = L->pts[k], q = L->pts[k + 1];
        DVec2 u1 = dnorm(dsub(v, p)), u2 = dnorm(dsub(q, v));
        double turn = acos(fmax(-1.0, fmin(1.0, ddot(u1, u2))));
        if (turn < 1e-3) { out = (DVec2 *)grow(out, &cap, n + 1, sizeof(DVec2)); out[n++] = v; continue; }
        double room = 0.45 * fmin(dlen(dsub(v, p)), dlen(dsub(q, v)));
        double r = fmin(R, room / tan(0.5 * turn)), tl = r * tan(0.5 * turn);
        DVec2 t1 = dsub(v, dscale(u1, tl)), t2 = dadd(v, dscale(u2, tl));
        double side = dcross(u1, u2) > 0.0 ? 1.0 : -1.0;
        DVec2 nrm = dv(-u1.z * side, u1.x * side), centre = dadd(t1, dscale(nrm, r));
        int steps = (int)ceil(r * turn / ARC_STEP);
        if (steps < 1) steps = 1;
        double a0 = atan2(t1.z - centre.z, t1.x - centre.x);
        out = (DVec2 *)grow(out, &cap, n + steps + 1, sizeof(DVec2));
        for (int s = 0; s <= steps; ++s) {
            double a = a0 + side * turn * s / steps;
            out[n++] = s == 0 ? t1 : s == steps ? t2 : dv(centre.x + r * cos(a), centre.z + r * sin(a));
        }
    }
    out = (DVec2 *)grow(out, &cap, n + 1, sizeof(DVec2));
    out[n++] = L->pts[L->npts - 1];
    free(L->pts);
    L->pts = out;
    L->npts = n;
    L->s = (double *)malloc(sizeof(double) * (size_t)n);
    if (!L->s) fatal("out of memory rounding the roads");
    L->s[0] = 0.0;
    for (int k = 1; k < n; ++k) L->s[k] = L->s[k - 1] + dlen(dsub(out[k], out[k - 1]));
}

/* --- Gates ------------------------------------------------------------------------------------ */

/* Whether the town holding world (x, z) is built, remembered in `cache`. */
static int built_at(IMap *cache, DVec2 w) {
    int i, j;
    plan_town_at(w.x, w.z, &i, &j);
    int64_t key = cell_key(i, j);
    int v = imap_get(cache, key);
    if (v < 0) { v = plan_town_built(i, j); imap_put(cache, key, v); }
    return v;
}

/* The point s along a polyline p of n points. */
static DVec2 at_s(const DVec2 *p, int n, double s) {
    for (int k = 0; k + 1 < n; ++k) {
        double len = dlen(dsub(p[k + 1], p[k]));
        if (s <= len || k + 2 == n) return dadd(p[k], dscale(dsub(p[k + 1], p[k]), len > 0.0 ? fmin(s / len, 1.0) : 0.0));
        s -= len;
    }
    return p[0];
}

/* The point s along polyline p of chart ch, in the world's chart. */
static DVec2 world_at_s(const TerrainChart *ch, const DVec2 *p, int n, double s) { return world_of(dir_of(ch, at_s(p, n, s))); }

/* A road running out from settlement k's centre along p (n points of chart
 * ch): where it leaves k's built towns for the last time, on the boulevard
 * between the last built town and the next (the towns are the world chart's,
 * so the gate is found there); its points from there into *out (malloc'd,
 * in ch), the first GATE_APPROACH of them (less if that would reach a built
 * town) straight out along the boulevard's normal. Returns the new count, or
 * 0 if it never leaves (then p stands). The whole road is walked: it may
 * leave a city 60-150 km across into a hole the cut made (a range, a lake)
 * and come back in beyond; other settlements' towns don't count. */
static int gate_out(int k, const TerrainChart *ch, const DVec2 *p, int n, IMap *cache, PlanGate *g, DVec2 **out) {
    double len = 0.0;
    for (int q = 0; q + 1 < n; ++q) len += dlen(dsub(p[q + 1], p[q]));
    double sIn = -1.0;
    for (double s = 0.0; s <= len; s += GATE_STEP) {
        DVec2 q = world_at_s(ch, p, n, s);
        if (built_at(cache, q) && plan_settlement_near(q.x, q.z) == k) sIn = s;
    }
    if (sIn < 0.0 || sIn + GATE_STEP > len) return 0;
    double lo = sIn, hi = sIn + GATE_STEP;
    for (int it = 0; it < 20; ++it) {
        double mid = 0.5 * (lo + hi);
        if (built_at(cache, world_at_s(ch, p, n, mid))) lo = mid; else hi = mid;
    }
    DVec2 qi = world_at_s(ch, p, n, lo), qo = world_at_s(ch, p, n, hi);
    int ii, ij, oi, oj;
    plan_town_at(qi.x, qi.z, &ii, &ij);
    plan_town_at(qo.x, qo.z, &oi, &oj);
    DVec2 ci = plan_town_centre(ii, ij), co = plan_town_centre(oi, oj);
    DVec2 nrm = dnorm(dsub(co, ci)), mid = dscale(dadd(ci, co), 0.5);
    DVec2 gp = world_at_s(ch, p, n, 0.5 * (lo + hi));
    gp = dsub(gp, dscale(nrm, ddot(dsub(gp, mid), nrm)));   /* onto the bisector: the boulevard's centre line */
    double approach = GATE_APPROACH;
    DVec2 a = dadd(gp, dscale(nrm, approach));
    while (approach > 40.0 && built_at(cache, a)) { approach *= 0.5; a = dadd(gp, dscale(nrm, approach)); }
    DVec2 gl = in_chart(ch, terrain_chart_dir(&g_wc, gp.x, gp.z)), al = in_chart(ch, terrain_chart_dir(&g_wc, a.x, a.z));
    /* on along the route from the first point well clear of the gate */
    int m = 0;
    double sk = 0.0;
    for (; m < n; ++m) {
        if (m > 0) sk += dlen(dsub(p[m], p[m - 1]));
        if (sk > hi && dlen(dsub(p[m], gl)) >= 2.0 * approach && !built_at(cache, world_of(dir_of(ch, p[m])))) break;
    }
    DVec2 *o = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(n - m + 3));
    if (!o) fatal("out of memory making the roads' gates");
    int c = 0;
    o[c++] = gl;
    o[c++] = al;
    if (m >= n) o[c++] = p[n - 1];
    for (int k = m; k < n; ++k) o[c++] = p[k];
    g->p = gp;
    g->n = nrm;
    *out = o;
    return c;
}

static void reverse(DVec2 *p, int n) {
    for (int k = 0; k < n / 2; ++k) { DVec2 t = p[k]; p[k] = p[n - 1 - k]; p[n - 1 - k] = t; }
}

static void gate_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    IMap cache;
    imap_init(&cache, 256);
    for (int end = 0; end < 2; ++end) {
        int v = end ? L->b : L->a;
        L->gate[end] = 0;
        if (net->node[v].settlement < 0) continue;
        if (end) reverse(L->pts, L->npts);
        DVec2 *o;
        PlanGate g;
        int n = gate_out(net->node[v].settlement, &L->ch, L->pts, L->npts, &cache, &g, &o);
        if (n) {
            free(L->pts);
            L->pts = o;
            L->npts = n;
            g.half = L->cls == PLAN_ROAD_FREEWAY ? PLAN_FREEWAY_HALF : PLAN_TRUNK_HALF;
            g.road = l;
            L->gp[end] = g;
            L->gate[end] = 1;
        }
        if (end) reverse(L->pts, L->npts);
    }
    imap_free(&cache);
}

/* --- Profiles ---------------------------------------------------------------------------------- */

/* The ground's height for the roads to follow: without them (they aren't in
 * the terrain while it is asked), its finest detail left out. */
static double road_ground(TerrainDir p) { return terrain_height_dry_dir(p, 10.0); }

/* First pass: the ground along the road every station, smoothed over the
 * class's window, into L->prof. */
static void profile_ground_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    double len = L->s[L->npts - 1];
    int n = (int)ceil(len / PLAN_ROAD_STATION) + 1;
    if (n < 2) n = 2;
    double ds = len / (n - 1);
    float *g = (float *)malloc(sizeof(float) * (size_t)n), *r = (float *)malloc(sizeof(float) * (size_t)n);
    if (!g || !r) fatal("out of memory grading the roads");
    int seg = 0;
    for (int k = 0; k < n; ++k) {
        double s = k * ds;
        while (seg + 2 < L->npts && L->s[seg + 1] < s) seg++;
        double sl = L->s[seg + 1] - L->s[seg], t = sl > 0.0 ? (s - L->s[seg]) / sl : 0.0;
        g[k] = (float)road_ground(dir_of(&L->ch, dadd(L->pts[seg], dscale(dsub(L->pts[seg + 1], L->pts[seg]), fmin(fmax(t, 0.0), 1.0)))));
    }
    int W = (int)ceil((L->cls == PLAN_ROAD_FREEWAY ? 300.0 : 150.0) / ds);
    for (int k = 0; k < n; ++k) {
        double sum = 0.0;
        int c = 0;
        for (int q = k - W; q <= k + W; ++q)
            if (q >= 0 && q < n) { sum += g[q]; c++; }
        r[k] = (float)(sum / c);
    }
    free(g);
    L->prof = r;
    L->nprof = n;
    L->ds = ds;
}

static double link_grade(const Link *L) { return L->cls == PLAN_ROAD_FREEWAY ? PLAN_FREEWAY_GRADE : PLAN_TRUNK_GRADE; }

/* Holds r (n heights ds apart) to grade G, r[0] at h0 and r[n - 1] at h1: in
 * one pass from the start, each height as near its own as both the step from
 * the last and the band from which both ends can still be reached allow.
 * Where the ends are too far apart for the grade, a straight line. */
static void hold_grade(float *r, int n, double ds, double G, double h0, double h1) {
    double L = ds * (n - 1), step = G * ds;
    r[0] = (float)h0;
    for (int k = 1; k < n; ++k) {
        double s = k * ds;
        double lo = fmax(h0 - G * s, h1 - G * (L - s)), hi = fmin(h0 + G * s, h1 + G * (L - s));
        if (lo > hi) lo = hi = h0 + (h1 - h0) * s / L;
        lo = fmax(lo, (double)r[k - 1] - step);
        hi = fmin(hi, (double)r[k - 1] + step);
        if (lo > hi) lo = hi = 0.5 * (lo + hi);
        r[k] = (float)fmin(fmax((double)r[k], lo), hi);
    }
    r[n - 1] = (float)h1;
}

/* hold_grade from the end, when `back`: the pass follows the ground until
 * the grade won't let it, so the end it starts from keeps to the ground. */
static void hold_grade_from(float *r, int n, double ds, double G, double h0, double h1, int back) {
    if (!back) { hold_grade(r, n, ds, G, h0, h1); return; }
    for (int k = 0; k < n / 2; ++k) { float t = r[k]; r[k] = r[n - 1 - k]; r[n - 1 - k] = t; }
    hold_grade(r, n, ds, G, h1, h0);
    for (int k = 0; k < n / 2; ++k) { float t = r[k]; r[k] = r[n - 1 - k]; r[n - 1 - k] = t; }
}

/* Second pass: pinned to its ends' heights, its grade held, then short
 * vertical curves (an average, its grade held again). The grade is held
 * from a gate outwards: beyond its city a hill too steep for the grade is
 * climbed in a cutting, not come down from onto the city's belt on an
 * embankment. */
static void profile_grade_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    float *r = L->prof;
    int n = L->nprof;
    double ds = L->ds, len = ds * (n - 1), G = link_grade(L);
    int back = L->gate[1] && !L->gate[0];
    double h0 = L->gate[0] ? terrain_height(L->gp[0].p.x, L->gp[0].p.z) : net->nodeH[L->a];
    double h1 = L->gate[1] ? terrain_height(L->gp[1].p.x, L->gp[1].p.z) : net->nodeH[L->b];
    /* drawn to the ends' heights over as long as half the grade takes */
    double e0 = h0 - r[0], e1 = h1 - r[n - 1];
    double l0 = fmax(200.0, fabs(e0) / (0.5 * G)), l1 = fmax(200.0, fabs(e1) / (0.5 * G));
    for (int k = 0; k < n; ++k) {
        double s = k * ds;
        r[k] += (float)(e0 * fmax(0.0, 1.0 - s / l0) + e1 * fmax(0.0, 1.0 - (len - s) / l1));
    }
    hold_grade_from(r, n, ds, G, h0, h1, back);
    int V = (int)ceil(100.0 / ds);
    float *g = (float *)malloc(sizeof(float) * (size_t)n);
    if (!g) fatal("out of memory grading the roads");
    memcpy(g, r, sizeof(float) * (size_t)n);
    for (int k = 1; k < n - 1; ++k) {
        int w = V < k ? V : k;
        if (n - 1 - k < w) w = n - 1 - k;
        double sum = 0.0;
        for (int q = k - w; q <= k + w; ++q) sum += g[q];
        r[k] = (float)(sum / (2 * w + 1));
    }
    free(g);
    hold_grade_from(r, n, ds, G, h0, h1, back);
    /* over the water, each bridge's deck at its clearance, the grade held
     * down from it on either side (the approaches' embankments); never so
     * high that an end would have to move */
    if (L->nbr) {
        for (int b = 0; b < L->nbr; ++b) {
            PlanRoadBridge *B = &L->br[b];
            double h = fmin(B->deck, fmin(h0 + G * fmax(0.0, B->w0 - ds), h1 + G * fmax(0.0, len - B->w1 - ds)));
            B->deck = (float)h;
            for (int k = 0; k < n; ++k)
                if (k * ds > B->w0 - ds && k * ds < B->w1 + ds) r[k] = fmaxf(r[k], (float)h);
        }
        float step = (float)(G * ds);
        for (int k = 1; k < n; ++k) r[k] = fmaxf(r[k], r[k - 1] - step);
        for (int k = n - 2; k >= 0; --k) r[k] = fmaxf(r[k], r[k + 1] - step);
        r[0] = (float)h0;
        r[n - 1] = (float)h1;
    }
}

/* Where roads meet at a junction they meet at one height: the mean of their
 * smoothed ground there; then nudged together, a few times over, wherever a
 * link is too short for its grade to climb between its ends' heights (a gate's
 * height, its city's ground, stays put). */
static void junction_heights(Net *net) {
    int *cnt = (int *)calloc((size_t)net->nnodes, sizeof(int));
    if (!cnt) fatal("out of memory grading the roads");
    for (int v = 0; v < net->nnodes; ++v) net->nodeH[v] = 0.0;
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        if (!L->gate[0]) { net->nodeH[L->a] += L->prof[0]; cnt[L->a]++; }
        if (!L->gate[1]) { net->nodeH[L->b] += L->prof[L->nprof - 1]; cnt[L->b]++; }
    }
    for (int v = 0; v < net->nnodes; ++v)
        net->nodeH[v] = cnt[v] ? net->nodeH[v] / cnt[v] : road_ground(net->node[v].dir);
    for (int it = 0; it < 32; ++it)
        for (int l = 0; l < net->nlinks; ++l) {
            const Link *L = &net->link[l];
            double ha = L->gate[0] ? terrain_height(L->gp[0].p.x, L->gp[0].p.z) : net->nodeH[L->a];
            double hb = L->gate[1] ? terrain_height(L->gp[1].p.x, L->gp[1].p.z) : net->nodeH[L->b];
            double reach = 0.8 * link_grade(L) * L->ds * (L->nprof - 1), d = hb - ha, excess = fabs(d) - reach;
            if (excess <= 0.0 || (L->gate[0] && L->gate[1])) continue;
            double sgn = d > 0.0 ? 1.0 : -1.0;
            if (L->gate[0]) net->nodeH[L->b] -= sgn * excess;
            else if (L->gate[1]) net->nodeH[L->a] += sgn * excess;
            else { net->nodeH[L->a] += 0.5 * sgn * excess; net->nodeH[L->b] -= 0.5 * sgn * excess; }
        }
    free(cnt);
}

static double profile_at(const PlanRoad *R, double s) {
    if (!(R->ds > 0.0)) return R->prof[0];
    double f = s / R->ds;
    if (f <= 0.0) return R->prof[0];
    if (f >= R->nprof - 1) return R->prof[R->nprof - 1];
    int k = (int)f;
    return R->prof[k] + (R->prof[k + 1] - R->prof[k]) * (f - k);
}

/* --- Bridges ------------------------------------------------------------------------------------- */

/* The point s along link L, in its chart. */
static DVec2 link_at(const Link *L, double s) {
    int lo = 0, hi = L->npts - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (L->s[mid] <= s) lo = mid; else hi = mid;
    }
    double sl = L->s[hi] - L->s[lo], t = sl > 0.0 ? (s - L->s[lo]) / sl : 0.0;
    return dadd(L->pts[lo], dscale(dsub(L->pts[hi], L->pts[lo]), fmin(fmax(t, 0.0), 1.0)));
}

static double link_profile(const Link *L, double s) {
    double f = s / L->ds;
    if (f <= 0.0) return L->prof[0];
    if (f >= L->nprof - 1) return L->prof[L->nprof - 1];
    int k = (int)f;
    return L->prof[k] + (L->prof[k + 1] - L->prof[k]) * (f - k);
}

/* Whether chart ch's p is on a city's ground (its urban weight over 0). */
static int urban_in(const TerrainChart *ch, DVec2 p) {
    DVec2 w = world_of(dir_of(ch, p));
    return plan_urban(w.x, w.z) > 0.0f;
}

/* Whether the ground at chart ch's p is the water's: in the water or within
 * a metre of it, or more than WET_DEPTH under the dry ground (a bed or a
 * bank carved into it); the dry ground into *dry. While the roads are being
 * built the terrain has none of them. Never on a city's ground, which the
 * roads don't move and the water carves no bed in: there the rivers run in
 * the city's channels or are buried. */
static int wet_at(const TerrainChart *ch, DVec2 p, double *dry) {
    TerrainDir d = dir_of(ch, p);
    TerrainEarthWater w;
    *dry = road_ground(d);
    if (urban_in(ch, p)) return 0;
    if (terrain_earth_water(d, &w) && w.edge < 1.0) return 1;
    return terrain_height_dir(d, 10.0) < *dry - WET_DEPTH;
}

/* A bridge's clearance over the water's level, to its deck's surface: more
 * over a wider river, most over a lake (m). */
static double bridge_clear(double w, int lake) { return (lake ? 8.0 : 4.0 + fmin(w, 300.0) / 50.0) + PLAN_ROAD_DECK_DEPTH; }

typedef struct Wet { double a, b, w, lv; int lake; } Wet;
static int wet_cmp(const void *pa, const void *pb) {
    const Wet *a = (const Wet *)pa, *b = (const Wet *)pb;
    return a->a < b->a ? -1 : a->a > b->a ? 1 : 0;
}

/* Step 10, first part: where link l crosses the bake's rivers (their centre
 * lines) and lakes (between the shores it crosses), each crossing walked out
 * along the road while the ground is the water's (wet_at), up to WET_MAX
 * either side; those within WET_JOIN of each other one bridge, its deck over
 * the water's level (the dry ground's least along the stretch) by its
 * clearance. The spans come once the profile is known (bridge_span_job). */
static void bridge_find_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    L->br = NULL; L->nbr = 0; L->segbr = NULL;
    if (!terrain_earth_loaded()) return;
    double len = L->s[L->npts - 1];
    WaterGrid g;
    water_init(&g, &L->ch, WATER_CELL, NULL);
    for (double s = 0.0;; s += WATER_STEP) {
        water_near(&g, link_at(L, fmin(s, len)), WATER_RADIUS);
        if (s >= len) break;
    }
    Wet *wet = NULL;
    int nw = 0, capw = 0;
    double in = -1.0;
    for (int k = 0; k + 1 < L->npts; ++k) {
        int n = water_hits(&g, L->pts[k], L->pts[k + 1], 1);
        double sl = L->s[k + 1] - L->s[k];
        for (int h = 0; h < n; ++h) {
            const WaterHit *wh = &g.hit[h];
            if (wh->t >= 1.0 && k + 2 < L->npts) continue;   /* the next segment's start */
            double s = L->s[k] + wh->t * sl;
            if (wh->kind == 2 && in < 0.0) { in = s; continue; }
            double a = wh->kind == 1 ? s : in;
            if (wh->kind == 2) in = -1.0;   /* out of the lake (a river's line may run through one) */
            /* none on a city's ground (wet_at) */
            if (urban_in(&L->ch, link_at(L, a)) || urban_in(&L->ch, link_at(L, 0.5 * (a + s))) || urban_in(&L->ch, link_at(L, s))) continue;
            wet = (Wet *)grow(wet, &capw, nw + 1, sizeof(Wet));
            wet[nw++] = (Wet){ a, s, wh->kind == 1 ? wh->w : 0.0, 0.0, wh->kind == 2 };
        }
    }
    water_free(&g);
    if (!nw) return;
    for (int k = 0; k < nw; ++k) {
        double d, lo = wet[k].a, hi = wet[k].b, lv = 1e30;
        for (double s = wet[k].a; s >= fmax(0.0, wet[k].a - WET_MAX); s -= WET_STEP) {
            int w = wet_at(&L->ch, link_at(L, s), &d);
            lv = fmin(lv, d);
            if (!w) break;
            lo = s - WET_STEP;
        }
        for (double s = wet[k].b; s <= fmin(len, wet[k].b + WET_MAX); s += WET_STEP) {
            int w = wet_at(&L->ch, link_at(L, s), &d);
            lv = fmin(lv, d);
            if (!w) break;
            hi = s + WET_STEP;
        }
        if (wet[k].lake)   /* the dry ground over a lake (the elevation's flat water) */
            for (double s = wet[k].a; s <= wet[k].b; s += fmax(WET_STEP, (wet[k].b - wet[k].a) / 16.0)) lv = fmin(lv, road_ground(dir_of(&L->ch, link_at(L, s))));
        wet[k].a = fmax(lo, L->gate[0] ? 1.0 : JUNCTION_CLEAR);
        wet[k].b = fmin(hi, len - (L->gate[1] ? 1.0 : JUNCTION_CLEAR));
        wet[k].lv = lv;
    }
    qsort(wet, (size_t)nw, sizeof(Wet), wet_cmp);
    L->br = (PlanRoadBridge *)calloc((size_t)nw, sizeof(PlanRoadBridge));
    if (!L->br) fatal("out of memory bridging the roads");
    for (int k = 0; k < nw; ++k) {
        if (wet[k].b - wet[k].a < 1.0) continue;
        double deck = wet[k].lv + bridge_clear(wet[k].w, wet[k].lake);
        PlanRoadBridge *B = L->nbr ? &L->br[L->nbr - 1] : NULL;
        if (B && wet[k].a - B->w1 < WET_JOIN) {
            B->w1 = fmax(B->w1, wet[k].b);
            B->deck = fmaxf(B->deck, (float)deck);
            B->lake |= (uint8_t)wet[k].lake;
            continue;
        }
        B = &L->br[L->nbr++];
        B->w0 = wet[k].a; B->w1 = wet[k].b;
        B->deck = (float)deck;
        B->lake = (uint8_t)wet[k].lake;
    }
    free(wet);
}

/* Step 10, second part, once the profile is raised: each span reaches out
 * past its water's stretch as far as the embankment's spill slope takes to
 * come down from the road to the dry ground there (twice the height, as the
 * batter: PLAN_ROAD_BATTER_MIN to _MAX); spans that meet are one. The spans'
 * ends become points of the road, so each segment is under a bridge or not. */
static void bridge_span_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    if (!L->nbr) return;
    double len = L->s[L->npts - 1];
    int m = 0;
    for (int b = 0; b < L->nbr; ++b) {
        PlanRoadBridge B = L->br[b];
        double g0 = road_ground(dir_of(&L->ch, link_at(L, B.w0))), g1 = road_ground(dir_of(&L->ch, link_at(L, B.w1)));
        double sp0 = fmin(fmax(2.0 * (link_profile(L, B.w0) - g0), PLAN_ROAD_BATTER_MIN), PLAN_ROAD_BATTER_MAX);
        double sp1 = fmin(fmax(2.0 * (link_profile(L, B.w1) - g1), PLAN_ROAD_BATTER_MIN), PLAN_ROAD_BATTER_MAX);
        B.s0 = fmax(B.w0 - sp0, L->gate[0] ? 0.5 : fmin(JUNCTION_CLEAR, B.w0));
        B.s1 = fmin(B.w1 + sp1, len - (L->gate[1] ? 0.5 : fmin(JUNCTION_CLEAR, len - B.w1)));
        if (m && B.s0 <= L->br[m - 1].s1) {
            PlanRoadBridge *P = &L->br[m - 1];
            P->s1 = fmax(P->s1, B.s1); P->w1 = fmax(P->w1, B.w1);
            P->deck = fmaxf(P->deck, B.deck);
            P->lake |= B.lake;
            continue;
        }
        L->br[m++] = B;
    }
    L->nbr = m;
    /* the spans' ends into the points */
    int cap = L->npts + 2 * m, n = 0;
    DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)cap);
    double *s = (double *)malloc(sizeof(double) * (size_t)cap);
    if (!p || !s) fatal("out of memory bridging the roads");
    int e = 0;
    for (int k = 0; k < L->npts; ++k) {
        while (e < 2 * m) {
            double c = e & 1 ? L->br[e >> 1].s1 : L->br[e >> 1].s0;
            if (c >= L->s[k] - 1e-6) break;
            if (k > 0 && c > L->s[k - 1] + 1e-6) { p[n] = link_at(L, c); s[n++] = c; }
            e++;
        }
        while (e < 2 * m && fabs((e & 1 ? L->br[e >> 1].s1 : L->br[e >> 1].s0) - L->s[k]) <= 1e-6) e++;
        p[n] = L->pts[k]; s[n++] = L->s[k];
    }
    free(L->pts); free(L->s);
    L->pts = p; L->s = s; L->npts = n;
    L->segbr = (int *)malloc(sizeof(int) * (size_t)(n > 1 ? n - 1 : 1));
    if (!L->segbr) fatal("out of memory bridging the roads");
    for (int k = 0, b = 0; k + 1 < n; ++k) {
        double mid = 0.5 * (s[k] + s[k + 1]);
        while (b < m && L->br[b].s1 < mid) b++;
        L->segbr[k] = b < m && L->br[b].s0 < mid ? b : -1;
    }
}

/* --- Publishing ------------------------------------------------------------------------------ */

/* Link l's points taken into the world's chart, no more than PUBLISH_STEP
 * apart: each segment cut into equal pieces, under its bridge if it is, the
 * distance along them its own; its ends the gates' or the nodes' points
 * exactly. From here on the link's points are the world chart's. */
static void publish_job(void *ctx, int l) {
    Net *net = (Net *)ctx;
    Link *L = &net->link[l];
    int n = 1;
    for (int k = 0; k + 1 < L->npts; ++k) n += 1 + (int)((L->s[k + 1] - L->s[k]) / PUBLISH_STEP);
    DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)n);
    double *s = (double *)malloc(sizeof(double) * (size_t)n);
    int *sb = L->segbr ? (int *)malloc(sizeof(int) * (size_t)(n - 1)) : NULL;
    if (!p || !s || (L->segbr && !sb)) fatal("out of memory publishing the roads");
    int m = 0;
    p[m] = L->gate[0] ? L->gp[0].p : net->node[L->a].p;
    s[m++] = 0.0;
    for (int k = 0; k + 1 < L->npts; ++k) {
        int pieces = 1 + (int)((L->s[k + 1] - L->s[k]) / PUBLISH_STEP);
        for (int q = 1; q <= pieces; ++q) {
            double t = (double)q / pieces;
            if (sb) sb[m - 1] = L->segbr[k];
            s[m] = q == pieces ? L->s[k + 1] : L->s[k] + t * (L->s[k + 1] - L->s[k]);
            p[m++] = world_of(dir_of(&L->ch, q == pieces ? L->pts[k + 1] : dadd(L->pts[k], dscale(dsub(L->pts[k + 1], L->pts[k]), t))));
        }
    }
    p[m - 1] = L->gate[1] ? L->gp[1].p : net->node[L->b].p;
    free(L->pts); free(L->s); free(L->segbr);
    L->pts = p; L->s = s; L->npts = m; L->segbr = sb;
}

/* --- The index ---------------------------------------------------------------------------------- */

/* A run of road's segments seg0 to seg1 that all reach one square. */
typedef struct IdxRun { int64_t cell; int road, seg0, seg1; float lo, hi; } IdxRun;
typedef struct IdxRuns { IdxRun *run; int n, cap; } IdxRuns;
typedef struct IndexJob { const Net *net; IdxRuns *runs; } IndexJob;

static int run_cmp(const void *pa, const void *pb) {
    const IdxRun *a = (const IdxRun *)pa, *b = (const IdxRun *)pb;
    if (a->cell != b->cell) return a->cell < b->cell ? -1 : 1;
    if (a->road != b->road) return a->road - b->road;
    return a->seg0 - b->seg0;
}

/* How far either way along the world chart's x and z ROAD_REACH on the
 * sphere reaches at w: across the line out from its origin the chart
 * stretches it, along that line not. */
static DVec2 reach_at(DVec2 w) {
    Metric m = metric_at(w);
    double k = 1.0 / m.s;
    return dv(ROAD_REACH * sqrt(m.r.x * m.r.x + k * k * m.r.z * m.r.z), ROAD_REACH * sqrt(m.r.z * m.r.z + k * k * m.r.x * m.r.x));
}

/* Road r's segments (in the world's chart: they are short, PUBLISH_STEP at
 * most on the sphere) filed under each square their bounds, widened by the
 * reach, touch: consecutive segments in one square as one run; each with
 * the least and most height of its segments (the profile's stations over
 * them and either side). */
static void index_job(void *ctx, int r) {
    IndexJob *j = (IndexJob *)ctx;
    const Link *L = &j->net->link[r];
    IdxRuns *out = &j->runs[r];
    IMap open;   /* square -> its latest run */
    imap_init(&open, 1 << 10);
    for (int sg = 0; sg + 1 < L->npts; ++sg) {
        DVec2 a = L->pts[sg], b = L->pts[sg + 1], ra = reach_at(a), rb = reach_at(b);
        double hx = 1.01 * fmax(ra.x, rb.x) + 1.0, hz = 1.01 * fmax(ra.z, rb.z) + 1.0;
        int k0 = (int)floor(L->s[sg] / L->ds), k1 = (int)ceil(L->s[sg + 1] / L->ds);
        float lo = 1e30f, hi = -1e30f;
        for (int k = k0 < 0 ? 0 : k0; k <= k1 && k < L->nprof; ++k) { lo = fminf(lo, L->prof[k]); hi = fmaxf(hi, L->prof[k]); }
        int i0 = (int)floor((fmin(a.x, b.x) - hx) / ROAD_CELL), i1 = (int)floor((fmax(a.x, b.x) + hx) / ROAD_CELL);
        int j0 = (int)floor((fmin(a.z, b.z) - hz) / ROAD_CELL), j1 = (int)floor((fmax(a.z, b.z) + hz) / ROAD_CELL);
        for (int jj = j0; jj <= j1; ++jj)
            for (int ii = i0; ii <= i1; ++ii) {
                int64_t key = cell_key(ii, jj);
                int k = imap_get(&open, key);
                if (k >= 0 && out->run[k].seg1 == sg - 1) {
                    IdxRun *u = &out->run[k];
                    u->seg1 = sg; u->lo = fminf(u->lo, lo); u->hi = fmaxf(u->hi, hi);
                    continue;
                }
                out->run = (IdxRun *)grow(out->run, &out->cap, out->n + 1, sizeof(IdxRun));
                out->run[out->n] = (IdxRun){ key, r, sg, sg, lo, hi };
                imap_put(&open, key, out->n++);
            }
    }
    imap_free(&open);
}

/* Every road's runs, by square: the runs of a square together, by road and
 * segment, each square's first in `cells`, with how many and the least and
 * most height of their segments. */
static void build_index(Net *net, int threads) {
    IdxRuns *runs = (IdxRuns *)calloc((size_t)(net->nlinks + 1), sizeof(IdxRuns));
    if (!runs) fatal("out of memory indexing the roads");
    IndexJob job = { net, runs };
    thread_parallel_for(net->nlinks, threads, index_job, &job);
    int ne = 0;
    for (int r = 0; r < net->nlinks; ++r) ne += runs[r].n;
    IdxRun *e = (IdxRun *)malloc(sizeof(IdxRun) * (size_t)(ne + 1));
    if (!e) fatal("out of memory indexing the roads");
    ne = 0;
    for (int r = 0; r < net->nlinks; ++r) {
        memcpy(&e[ne], runs[r].run, sizeof(IdxRun) * (size_t)runs[r].n);
        ne += runs[r].n;
        free(runs[r].run);
    }
    free(runs);
    qsort(e, (size_t)ne, sizeof(IdxRun), run_cmp);
    imap_init(&net->cells, 1 << 16);
    net->idx = (int *)malloc(sizeof(int) * 3 * (size_t)(ne + 1));
    net->idxCount = (int *)malloc(sizeof(int) * (size_t)(ne + 1));
    net->idxLo = (float *)malloc(sizeof(float) * (size_t)(ne + 1));
    net->idxHi = (float *)malloc(sizeof(float) * (size_t)(ne + 1));
    if (!net->idx || !net->idxCount || !net->idxLo || !net->idxHi) fatal("out of memory indexing the roads");
    net->lo = 1e30f; net->hi = -1e30f;
    int first = -1;
    for (int k = 0; k < ne; ++k) {
        if (k == 0 || e[k].cell != e[k - 1].cell) {
            first = k;
            imap_put(&net->cells, e[k].cell, k);
            net->idxCount[k] = 0;
            net->idxLo[k] = 1e30f;
            net->idxHi[k] = -1e30f;
        }
        net->idx[3 * k] = e[k].road;
        net->idx[3 * k + 1] = e[k].seg0;
        net->idx[3 * k + 2] = e[k].seg1;
        net->idxCount[first]++;
        net->idxLo[first] = fminf(net->idxLo[first], e[k].lo);
        net->idxHi[first] = fmaxf(net->idxHi[first], e[k].hi);
        net->lo = fminf(net->lo, e[k].lo); net->hi = fmaxf(net->hi, e[k].hi);
    }
    free(e);
}

/* --- The whole network --------------------------------------------------------------------------- */

void plan_roads_build(int threads) {
    Net *net = (Net *)calloc(1, sizeof(Net));
    if (!net) fatal("out of memory building the roads");
    const int ns = plan_settlement_count();
    g_wc = terrain_world_chart();
    for (int k = 0; k < ns; ++k) net->node[add_node(net, plan_settlement(k).dir, k)].p = plan_settlement(k).centre;

    /* the coarse grid, for every route */
    g_coarse_n = plan_cube_side(COARSE_CELL);
    g_coarse = (uint8_t *)malloc(6 * (size_t)g_coarse_n * g_coarse_n);
    if (!g_coarse) fatal("out of memory building the roads");
    thread_parallel_for(6 * g_coarse_n, threads, coarse_row, NULL);

    /* 1. candidates, routed coarsely for their costs */
    /* between the settlements on a continent */
    int (*pairs)[2] = malloc(sizeof(int[2]) * 8 * PLAN_MAX_SETTLEMENTS);
    int ids[PLAN_MAX_SETTLEMENTS], nids = 0;
    if (!pairs) fatal("out of memory building the roads");
    for (int k = 0; k < ns; ++k)
        if (plan_settlement(k).continent > 0) ids[nids++] = k;
    int np = delaunay(ids, nids, pairs, 8 * PLAN_MAX_SETTLEMENTS), nc = 0;
    int capc = np > 0 ? np : 1;
    Cand *cand = (Cand *)calloc((size_t)capc, sizeof(Cand));
    {
        /* those longer than CAND_REACH only where the shorter leave their ends
         * apart: the shortest across each gap, until every continent is whole */
        int *parent = (int *)malloc(sizeof(int) * PLAN_MAX_SETTLEMENTS);
        double *len = (double *)malloc(sizeof(double) * (size_t)np);
        if (!cand || !parent || !len) fatal("out of memory building the roads");
        int ncand[PLAN_MAX_SETTLEMENTS] = { 0 };
        for (int v = 0; v < ns; ++v) parent[v] = v;
        for (int k = 0; k < np; ++k) {
            PlanSettlement a = plan_settlement(pairs[k][0]), b = plan_settlement(pairs[k][1]);
            len[k] = a.continent != b.continent ? -1.0 : sphere_m(a.dir, b.dir);
        }
        /* pass 0 takes every pair within CAND_REACH; pass 1 the shortest
         * longer one at a colossal city with fewer than HUB_LINKS, again;
         * pass 2 the shortest longer one across a gap, again */
        for (int pass = 0; pass < 3; ++pass)
            for (int k0 = 0; k0 < np; ) {
                int best = -1;
                if (pass == 0) { best = len[k0] >= 0.0 && len[k0] <= CAND_REACH ? k0 : -1; k0++; }
                else {
                    for (int k = 0; k < np; ++k) {
                        int a = pairs[k][0], b = pairs[k][1];
                        if (len[k] < 0.0 || (best >= 0 && len[k] >= len[best])) continue;
                        if (pass == 1 ? (plan_settlement(a).colossal && ncand[a] < HUB_LINKS) || (plan_settlement(b).colossal && ncand[b] < HUB_LINKS)
                                      : uf_find(parent, a) != uf_find(parent, b)) best = k;
                    }
                    if (best < 0) break;
                }
                if (best < 0) continue;
                int a = pairs[best][0], b = pairs[best][1];
                len[best] = -1.0;   /* taken */
                cand[nc++] = (Cand){ a, b, 0, add_link(net, a, b) };
                ncand[a]++; ncand[b]++;
                parent[uf_find(parent, a)] = uf_find(parent, b);
            }
        free(parent);
        free(len);
    }
    {
        int *ls = (int *)malloc(sizeof(int) * (size_t)nc);
        for (int k = 0; k < nc; ++k) ls[k] = cand[k].used;
        route_all(net, ls, nc, 1, threads);
        for (int k = 0; k < nc; ++k) { cand[k].cost = net->link[ls[k]].ok ? net->link[ls[k]].cost : -1; cand[k].used = 0; }
        free(ls);
        /* a settlement the limit leaves with no candidate at all (rough land
         * all round it) has its candidates routed again without one */
        int routed[PLAN_MAX_SETTLEMENTS] = { 0 };
        for (int k = 0; k < nc; ++k)
            if (cand[k].cost >= 0) { routed[cand[k].a] = 1; routed[cand[k].b] = 1; }
        for (int k = 0; k < nc; ++k)
            if (cand[k].cost < 0 && (!routed[cand[k].a] || !routed[cand[k].b])) {
                TerrainChart ch = link_chart(node_dir(net, cand[k].a), node_dir(net, cand[k].b));
                cand[k].cost = coarse_route(net, cand[k].a, cand[k].b, &ch, 0.0, NULL, NULL, NULL);
            }
        /* Gaps the routed candidates leave in a continent (those across a
         * strait no road crosses, the land round it too long a way for the
         * limit): the nearest pairs of settlements across them not yet
         * tried, GAP_BATCH at a time on the threads, routed, again, until
         * each continent is whole or GAP_TRIES pairs for each settlement on
         * one have been tried. */
        int *parent = (int *)malloc(sizeof(int) * PLAN_MAX_SETTLEMENTS), on = 0, triedAll = 0;
        uint8_t *tried = (uint8_t *)calloc((size_t)ns * ns, 1);
        if (!parent || !tried) fatal("out of memory building the roads");
        for (int k = 0; k < nc; ++k) tried[cand[k].a * ns + cand[k].b] = tried[cand[k].b * ns + cand[k].a] = 1;
        for (int k = 0; k < ns; ++k) on += plan_settlement(k).continent > 0;
        while (triedAll < GAP_TRIES * on) {
            for (int v = 0; v < ns; ++v) parent[v] = v;
            for (int k = 0; k < nc; ++k)
                if (cand[k].cost >= 0) parent[uf_find(parent, cand[k].a)] = uf_find(parent, cand[k].b);
            int pa[GAP_BATCH], pb[GAP_BATCH], npair = 0;
            double pd[GAP_BATCH];
            for (int a = 0; a < ns; ++a) {
                int ca = plan_settlement(a).continent;
                if (ca <= 0) continue;
                for (int b = a + 1; b < ns; ++b) {
                    if (plan_settlement(b).continent != ca || tried[a * ns + b] || uf_find(parent, a) == uf_find(parent, b)) continue;
                    double d = node_m(net, a, b);
                    if (npair == GAP_BATCH && d >= pd[npair - 1]) continue;
                    int q = npair < GAP_BATCH ? npair++ : npair - 1;   /* in order, nearest first */
                    for (; q > 0 && pd[q - 1] > d; --q) { pa[q] = pa[q - 1]; pb[q] = pb[q - 1]; pd[q] = pd[q - 1]; }
                    pa[q] = a; pb[q] = b; pd[q] = d;
                }
            }
            if (!npair) break;
            int ls[GAP_BATCH], base = net->nlinks;
            for (int q = 0; q < npair; ++q) {
                tried[pa[q] * ns + pb[q]] = tried[pb[q] * ns + pa[q]] = 1;
                ls[q] = add_link(net, pa[q], pb[q]);
            }
            route_all(net, ls, npair, 1, threads);
            for (int q = 0; q < npair; ++q) {
                cand = (Cand *)grow(cand, &capc, nc + 1, sizeof(Cand));
                cand[nc++] = (Cand){ pa[q], pb[q], net->link[ls[q]].ok ? net->link[ls[q]].cost : -1, 0 };
            }
            net->nlinks = base;
            triedAll += npair;
        }
        free(parent);
        free(tried);
    }
    /* 2-3. the spanning tree and the gravity model's additions */
    design(cand, nc);
    net->nlinks = 0;
    for (int k = 0; k < nc; ++k)
        if (cand[k].used) add_link(net, cand[k].a, cand[k].b);
    free(pairs);
    /* 4. junctions */
    steiner(net);
    /* 5. routes */
    {
        int *ls = (int *)malloc(sizeof(int) * (size_t)net->nlinks), n = net->nlinks;
        for (int k = 0; k < n; ++k) ls[k] = k;
        route_all(net, ls, n, 0, threads);
        free(ls);
    }
    /* A link that found no route can leave a continent in two: the cheapest
     * candidates across the gap, until it is whole (cand is cheapest first). */
    {
        int *parent = (int *)malloc(sizeof(int) * MAX_NODES);
        if (!parent) fatal("out of memory building the roads");
        for (int v = 0; v < MAX_NODES; ++v) parent[v] = v;
        for (int l = 0; l < net->nlinks; ++l)
            if (net->link[l].ok) parent[uf_find(parent, net->link[l].a)] = uf_find(parent, net->link[l].b);
        for (int k = 0; k < nc; ++k) {
            if (cand[k].cost < 0 || uf_find(parent, cand[k].a) == uf_find(parent, cand[k].b)) continue;
            int l = add_link(net, cand[k].a, cand[k].b);
            route_link(net, &net->link[l]);
            if (net->link[l].ok) parent[uf_find(parent, cand[k].a)] = uf_find(parent, cand[k].b);
        }
        free(parent);
    }
    free(cand);
    /* 6-8 */
    merge_parallel(net);
    split_crossings(net);
    tidy_junctions(net);
    classify(net);
    thread_parallel_for(net->nlinks, threads, gate_job, net);
    for (int l = 0; l < net->nlinks; ++l) round_corners(&net->link[l]);
    free(g_coarse);
    g_coarse = NULL;
    thread_parallel_for(net->nlinks, threads, bridge_find_job, net);
    thread_parallel_for(net->nlinks, threads, profile_ground_job, net);
    junction_heights(net);
    thread_parallel_for(net->nlinks, threads, profile_grade_job, net);
    thread_parallel_for(net->nlinks, threads, bridge_span_job, net);
    thread_parallel_for(net->nlinks, threads, publish_job, net);
    build_index(net, threads);
    /* the public view */
    int nr = 0;
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        if (!L->ok) continue;
        PlanRoad *r = &net->road[nr++];
        r->from = L->a; r->to = L->b; r->cls = L->cls;
        r->half = L->cls == PLAN_ROAD_FREEWAY ? PLAN_FREEWAY_HALF : PLAN_TRUNK_HALF;
        r->flow = L->flow;
        r->npts = L->npts; r->pts = L->pts; r->s = L->s;
        r->gate[0] = L->gate[0]; r->gate[1] = L->gate[1];
        r->nprof = L->nprof; r->ds = L->ds; r->prof = L->prof;
        r->nbridges = L->nbr; r->bridges = L->br; r->bridge = L->segbr;
    }
    net->nlinks = nr;   /* road k is link k from here on: compact the links to match */
    {
        int w = 0;
        for (int l = 0; w < nr; ++l)
            if (net->link[l].ok) net->link[w++] = net->link[l];
    }
    g_net = net;
}

int plan_node_count(void) { plan_settlement(0); return g_net ? g_net->nnodes : 0; }
PlanNode plan_node(int k) { plan_settlement(0); return g_net->node[k]; }
int plan_road_count(void) { plan_settlement(0); return g_net ? g_net->nlinks : 0; }
const PlanRoad *plan_road(int k) { plan_settlement(0); return &g_net->road[k]; }

PlanGate plan_road_gate(int k, int end) { plan_settlement(0); return g_net->link[k].gp[end ? 1 : 0]; }

double plan_road_height(int k, double s) { plan_settlement(0); return profile_at(&g_net->road[k], s); }

/* The squares the rectangle touches, or all the roads if it touches too many to look at. */
int plan_road_range(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    const Net *net = g_net;
    if (!net || net->nlinks == 0) return 0;
    double i0 = floor(x0 / ROAD_CELL), i1 = floor(x1 / ROAD_CELL), j0 = floor(z0 / ROAD_CELL), j1 = floor(z1 / ROAD_CELL);
    if ((i1 - i0 + 1.0) * (j1 - j0 + 1.0) > 4096.0) { *lo = net->lo; *hi = net->hi; return 1; }
    int any = 0;
    for (int j = (int)j0; j <= (int)j1; ++j)
        for (int i = (int)i0; i <= (int)i1; ++i) {
            int first = imap_get(&net->cells, cell_key(i, j));
            if (first < 0) continue;
            if (!any) { *lo = net->idxLo[first]; *hi = net->idxHi[first]; any = 1; }
            else { *lo = fmin(*lo, net->idxLo[first]); *hi = fmax(*hi, net->idxHi[first]); }
        }
    return any;
}

/* The bridges any of whose segments come within `pad` of the rectangle, each
 * once, in the order the index files them. */
int plan_road_bridges_in(double x0, double z0, double x1, double z1, double pad, PlanRoadSpan *out, int max) {
    const Net *net = g_net;
    if (!net || net->nlinks == 0) return 0;
    int n = 0;
    double k = 1.01 * stretch_at(dv(0.5 * (x0 + x1), 0.5 * (z0 + z1)));   /* the sphere's metres as the chart's there */
    for (int j = (int)floor(z0 / ROAD_CELL); j <= (int)floor(z1 / ROAD_CELL); ++j)
        for (int i = (int)floor(x0 / ROAD_CELL); i <= (int)floor(x1 / ROAD_CELL); ++i) {
            int first = imap_get(&net->cells, cell_key(i, j));
            if (first < 0) continue;
            for (int e = first; e < first + net->idxCount[first]; ++e) {
                int r = net->idx[3 * e];
                const PlanRoad *R = &net->road[r];
                if (!R->bridge) continue;
                for (int sg = net->idx[3 * e + 1]; sg <= net->idx[3 * e + 2]; ++sg) {
                    int b = R->bridge[sg];
                    if (b < 0) continue;
                    DVec2 a = R->pts[sg], c = R->pts[sg + 1];
                    double m = (pad + R->half) * k;
                    if (fmax(a.x, c.x) < x0 - m || fmin(a.x, c.x) > x1 + m || fmax(a.z, c.z) < z0 - m || fmin(a.z, c.z) > z1 + m) continue;
                    int seen = 0;
                    for (int q = 0; q < n && !seen; ++q) seen = out[q].road == r && out[q].bridge == b;
                    if (!seen && n < max) out[n++] = (PlanRoadSpan){ r, b };
                }
            }
        }
    return n;
}

/* The nearest road's reach over (x, z): its surface where (x, z) is on it or
 * its shoulders, then a batter sloping to the ground, a smoothstep across
 * twice the cut or fill (between the batter's least and greatest), widened
 * by the spacing so coarse samples still see it. Under a bridge, the water's
 * stretch is left as it is, and the embankment's spill slope comes in from
 * each end of the span (never wider for the spacing); ground there that
 * would stand above the deck's soffit is cut down under it. Where reaches
 * overlap, the strongest. Distances are the sphere's: the world chart's
 * offsets from (x, z) taken through its metric there (metric_at). */
double plan_road_ground(double x, double z, double ground, double spacing, double *road_h) {
    const Net *net = g_net;
    if (!net || spacing > PLAN_ROAD_LOD_SPACING) return 0.0;
    int first = imap_get(&net->cells, cell_key((int)floor(x / ROAD_CELL), (int)floor(z / ROAD_CELL)));
    if (first < 0) return 0.0;
    double best = 0.0, bestD = 1e300;
    DVec2 p = dv(x, z);
    Metric mt = metric_at(p);
    for (int e = first; e < first + net->idxCount[first]; ++e) {
        const PlanRoad *R = &net->road[net->idx[3 * e]];
        for (int sg = net->idx[3 * e + 1]; sg <= net->idx[3 * e + 2]; ++sg) {
            DVec2 a = metric_apply(&mt, dsub(R->pts[sg], p)), ab = dsub(metric_apply(&mt, dsub(R->pts[sg + 1], p)), a);
            double l2 = ddot(ab, ab), t = l2 > 0.0 ? -ddot(a, ab) / l2 : 0.0, sl = R->s[sg + 1] - R->s[sg];
            if (R->bridge && (t < 0.0 || t > 1.0)) {
                /* past a segment's end, nothing reaches round into a span, nor a
                 * bridge's segment out of it: under the deck is the bridge's,
                 * before it the road's. Round a corner the road is no longer
                 * along the segment's line, so a span as far back along the road
                 * as the point lies past the end, and off the line, counts. */
                if (R->bridge[sg] >= 0) continue;
                double tc = t < 0.0 ? 0.0 : 1.0, se = R->s[sg] + tc * sl;
                double past = fabs(t - tc) * sl + dlen(dadd(a, dscale(ab, tc)));
                double lo = t < 0.0 ? se - past : se, hi = t < 0.0 ? se : se + past;
                int into = 0;
                for (int q = 0; q < R->nbridges && !into; ++q) into = R->bridges[q].s0 < hi && R->bridges[q].s1 > lo;
                if (into) continue;
            }
            t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
            double d = dlen(dadd(a, dscale(ab, t))), core = R->half + PLAN_ROAD_SHOULDER;
            if (d >= core + PLAN_ROAD_BATTER_MAX + 1.5 * spacing) continue;
            double sr = R->s[sg] + t * sl, h = profile_at(R, sr);
            double batter = fmin(fmax(2.0 * fabs(ground - h), PLAN_ROAD_BATTER_MIN), PLAN_ROAD_BATTER_MAX) + 1.5 * spacing;
            double over = d - core;
            int b = R->bridge ? R->bridge[sg] : -1;
            if (b >= 0) {
                /* under a bridge the ground is the water's: the embankment's
                 * spill slope comes in from each end of the span as from the
                 * road's edge, never over the water's stretch; but a bank
                 * standing up into the span is cut down under the soffit */
                const PlanRoadBridge *B = &R->bridges[b];
                double soffit = h - PLAN_ROAD_DECK_DEPTH - BRIDGE_UNDER;
                if (ground > soffit) {
                    h = soffit;
                    batter = fmin(fmax(2.0 * (ground - h), PLAN_ROAD_BATTER_MIN), PLAN_ROAD_BATTER_MAX) + 1.5 * spacing;
                } else {
                    int first = sr < 0.5 * (B->w0 + B->w1);
                    over = fmax(over, first ? sr - B->s0 : B->s1 - sr);
                    batter = fmin(batter, first ? B->w0 - B->s0 : B->s1 - B->w1);
                    if (over >= batter) continue;
                }
            }
            double w = 1.0;
            if (over > 0.0) {
                double u = fmin(over / batter, 1.0);
                w = 1.0 - u * u * (3.0 - 2.0 * u);
            }
            if (w > best || (w == best && w > 0.0 && d < bestD)) { best = w; bestD = d; *road_h = h; }
        }
    }
    return best;
}
