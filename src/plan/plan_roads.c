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
 *    the obstacles rather than along the grid.
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
 * 10. Profiles: the ground along each road every PLAN_ROAD_STATION metres,
 *    smoothed, pinned to the city's ground at a gate and to one height at a
 *    junction, its grade held to the class's; the ground blends to it
 *    (plan_road_ground), found through an index of ROAD_CELL squares.
 *
 * The grids (in the world's chart) cost a step by its length, more on rough
 * land: in the mountains, on hills and plateaus (the coarse grid, by the
 * region's landforms), up steep grades and along or across canyons (the fine
 * grid, from heights and the canyons' centre lines). The sea to come, the
 * world's edge and other settlements block them. Steps are costed in whole
 * metres, so the game's fast floating point and the tests route alike.
 * Everything is a pure function of the settlements and the terrain (without
 * roads), on as many threads as the caller gives, each link on its own. */

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
#define STEINER_SAVE   0.06     /* a junction is made where it saves this share of the two links' length */
#define FREEWAY_SHARE  0.4
#define GRAVITY_D0     500000.0 /* the gravity model's distances in this unit, never under a fifth of it */
#define FREEWAY_RADIUS 1500.0   /* the corners' radius (m) */
#define TRUNK_RADIUS   500.0
#define ARC_STEP       60.0     /* a rounded corner's points at most this far apart (m) */
#define MAX_NODES      (PLAN_SETTLEMENTS + 1024)
#define MAX_LINKS      4096
#define GATE_STEP      250.0    /* m: a road's way out of its settlement is walked in these steps... */
#define GATE_APPROACH  300.0    /* ...and its last stretch is square to the boulevard for this long */
#define ROAD_CELL      512.0    /* the index's squares (m) */
#define ROAD_REACH     (PLAN_FREEWAY_HALF + PLAN_ROAD_SHOULDER + PLAN_ROAD_BATTER_MAX + 1.5 * PLAN_ROAD_LOD_SPACING)
#define MAX_RECORDS    1500000  /* a search gives up past this many cells... */
#define ROUTE_LIMIT    6.0      /* ...or costing more than this many times the straight line */

/* Open addressing from int64 keys to ints; INT64_MIN marks an empty slot. */
typedef struct IMap { int64_t *keys; int *vals; int cap, n; } IMap;

typedef struct Link {
    int      a, b;          /* nodes */
    int64_t  cost;          /* the route's (or, before it is routed, the coarse route's) cost */
    int      cls;
    double   flow;
    DVec2   *pts;           /* the route, from a to b (malloc'd) */
    int      npts;
    double  *s;
    int      ok;
    int      gate[2];       /* it ends at a gate at a, at b */
    PlanGate gp[2];
    float   *prof;          /* the profile (malloc'd) */
    int      nprof;
    double   ds;
} Link;

typedef struct Net {
    PlanNode node[MAX_NODES];
    int      nnodes;
    Link     link[MAX_LINKS];
    int      nlinks;
    PlanRoad road[MAX_LINKS];
    double   nodeH[MAX_NODES];   /* each junction's height, where its roads meet */
    /* the index: ROAD_CELL squares -> runs of (road, segment) in idx */
    IMap     cells;              /* square -> its first entry in idx */
    int     *idxCount;           /* by the same number: how many */
    int     *idx;                /* road, segment, road, segment... */
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

/* Whether a road may pass (x, z) on its way between settlements `from` and
 * `to` (-1 for a junction): above the sea to come, inside the world's reach,
 * and clear of every other settlement. */
static int passable(double x, double z, int from, int to) {
    if (plan_world_sea(x, z) < PLAN_ROAD_SEA_M) return 0;
    if (plan_city_edge(x, z) < CITY_CLEAR) {
        int k = plan_settlement_near(x, z);
        if (k != from && k != to) return 0;
    }
    return 1;
}

/* The coarse grid: each cell's cost per metre of a step, from the region's
 * landforms, worked out once for the whole world's reach (its rows on the
 * threads) and kept as a byte: 1 + v / COARSE_STEPS, or COARSE_SEA where no
 * road may go and COARSE_CITY within CITY_CLEAR of a settlement, where only
 * its own roads may. */
#define COARSE_EXTENT (PLAN_WORLD_REACH + 300000.0)
#define COARSE_I0     ((int)floor(-COARSE_EXTENT / COARSE_CELL))
#define COARSE_SIDE   (2 * -COARSE_I0)
#define COARSE_STEPS  50.0f
#define COARSE_SEA    255
#define COARSE_CITY   254

static uint8_t *g_coarse;

static void coarse_row(void *ctx, int r) {
    (void)ctx;
    for (int q = 0; q < COARSE_SIDE; ++q) {
        double x = (COARSE_I0 + q + 0.5) * COARSE_CELL, z = (COARSE_I0 + r + 0.5) * COARSE_CELL;
        uint8_t v = COARSE_SEA;
        if (plan_world_sea(x, z) >= PLAN_ROAD_SEA_M) {
            if (plan_city_edge(x, z) < CITY_CLEAR) v = COARSE_CITY;
            else {
                TerrainLandform lf;
                terrain_landform(x, z, &lf);
                float f = 4.0f * lf.w[TERRAIN_MOUNTAINS] + 1.5f * lf.w[TERRAIN_PLATEAU] + 0.6f * lf.w[TERRAIN_HILLS] + (lf.canyon > 0.0f ? 0.3f : 0.0f);
                v = (uint8_t)fminf(f * COARSE_STEPS + 0.5f, COARSE_CITY - 1.0f);
            }
        }
        g_coarse[(size_t)r * COARSE_SIDE + q] = v;
    }
}

typedef struct Coarse { int from, to, gi, gj, si, sj; } Coarse;

/* Coarse cell (i, j)'s cost per metre, or -1 where this route can't go. */
static float coarse_factor(const Coarse *c, int i, int j) {
    int q = i - COARSE_I0, r = j - COARSE_I0;
    if (q < 0 || r < 0 || q >= COARSE_SIDE || r >= COARSE_SIDE) return -1.0f;
    uint8_t v = g_coarse[(size_t)r * COARSE_SIDE + q];
    int end = (i == c->si && j == c->sj) || (i == c->gi && j == c->gj);
    if (v == COARSE_SEA) return end ? 1.0f : -1.0f;
    if (v == COARSE_CITY) {
        int k = plan_settlement_near((i + 0.5) * COARSE_CELL, (j + 0.5) * COARSE_CELL);
        return end || k == c->from || k == c->to ? 1.0f : -1.0f;
    }
    return 1.0f + v / COARSE_STEPS;
}

static int64_t coarse_step(void *ctx, int ai, int aj, int bi, int bj) {
    Coarse *c = (Coarse *)ctx;
    float fa = coarse_factor(c, ai, aj), fb = coarse_factor(c, bi, bj);
    if (fa < 0.0f || fb < 0.0f) return -1;
    double len = (ai != bi && aj != bj ? 1.4142135623730951 : 1.0) * COARSE_CELL;
    /* a diagonal step mustn't jump a strait */
    if (ai != bi && aj != bj && plan_world_sea((ai + bi + 1) * 0.5 * COARSE_CELL, (aj + bj + 1) * 0.5 * COARSE_CELL) < PLAN_ROAD_SEA_M) return -1;
    return (int64_t)(len * 0.5 * (fa + fb) + 0.5);
}

static int coarse_of(double v) { return (int)floor(v / COARSE_CELL); }
static int fine_of(double v) { return (int)floor(v / FINE_CELL); }

/* The fine grid: heights for grades, the mountains, canyons along and across. */
typedef struct FineCell { float h, mountain, canyon; int open; } FineCell;

typedef struct Fine {
    int       from, to;
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
} Fine;

static const FineCell *fine_cell(Fine *f, int i, int j) {
    int64_t key = cell_key(i, j);
    int k = imap_get(&f->memo, key);
    if (k >= 0) return &f->cell[k];
    double x = (i + 0.5) * FINE_CELL, z = (j + 0.5) * FINE_CELL;
    FineCell c = { 0.0f, 0.0f, 0.0f, 0 };
    int end = (i == f->si && j == f->sj) || (i == f->gi && j == f->gj);
    int inside = imap_get(&f->corridor, cell_key((int)floor((double)i / FINE_PER), (int)floor((double)j / FINE_PER))) >= 0;
    if (end || (inside && passable(x, z, f->from, f->to))) {
        TerrainLandform lf;
        terrain_landform(x, z, &lf);
        c.h = (float)terrain_height_lod(x, z, 250.0);
        c.mountain = lf.w[TERRAIN_MOUNTAINS];
        c.canyon = lf.canyon;
        c.open = 1;
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
 * steeply more beyond 8%), along a canyon, and a great deal across one any
 * deeper than a road could simply dip through. */
static int64_t step_cost(const FineCell *a, const FineCell *b, double len, double crossing) {
    double g = fabs((double)b->h - a->h) / len;
    double c = len * (1.0 + 1.25 * (a->mountain + b->mountain));
    c += len * (30.0 * fmax(0.0, g - 0.03) + 1500.0 * fmax(0.0, g - 0.08) * fmax(0.0, g - 0.08));
    c += len * 0.05 * (a->canyon + b->canyon);
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
    return step_cost(&ca, b, len, crossing_depth(f, p, q, ai, aj, bi, bj));
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
        int m = terrain_canyons_near(x, z, (CORRIDOR + 2) * COARSE_CELL, near, 64);
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
}

/* The cost of the straight line p-q over the fine grid: each cell it passes
 * through, stepped along it, and the canyons it crosses; -1 where it leaves
 * the open cells. */
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
    return total;
}

/* --- Routing a link ---------------------------------------------------------------------------- */

static DVec2 node_p(const Net *net, int k) { return net->node[k].p; }
static int node_settlement(const Net *net, int k) { return net->node[k].settlement; }

/* The coarse route's cost between two nodes (and its cells, if wanted), or -1
 * (also if it would cost more than `limit` times the straight line, when that
 * is above 0). */
static int64_t coarse_route(const Net *net, int a, int b, double limitFactor, int **ci, int **cj, int *n) {
    Coarse c;
    c.from = node_settlement(net, a);
    c.to = node_settlement(net, b);
    DVec2 pa = node_p(net, a), pb = node_p(net, b);
    c.si = coarse_of(pa.x); c.sj = coarse_of(pa.z);
    c.gi = coarse_of(pb.x); c.gj = coarse_of(pb.z);
    int *pi, *pj, np;
    int64_t limit = limitFactor > 0.0 ? (int64_t)(limitFactor * dlen(dsub(pb, pa))) + (int64_t)(4.0 * COARSE_CELL) : INT64_MAX / 4;
    int64_t cost = astar(c.si, c.sj, c.gi, c.gj, COARSE_CELL, 1.4, limit, coarse_step, &c, &pi, &pj, &np);
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

/* Routes link k: coarse, then fine within the corridor round it (widened if
 * that finds nothing), pulled straight. Its points run from the centre of a's
 * cell to b's, ending at the nodes themselves. */
static void route_link(Net *net, Link *L) {
    int *ci, *cj, nc;
    L->ok = 0;
    int64_t cc = coarse_route(net, L->a, L->b, 0.0, &ci, &cj, &nc);
    if (cc < 0) return;
    DVec2 pa = node_p(net, L->a), pb = node_p(net, L->b);
    for (int widen = 0; widen < 3 && !L->ok; ++widen) {
        Fine f;
        memset(&f, 0, sizeof(f));
        f.from = node_settlement(net, L->a);
        f.to = node_settlement(net, L->b);
        f.si = fine_of(pa.x); f.sj = fine_of(pa.z);
        f.gi = fine_of(pb.x); f.gj = fine_of(pb.z);
        imap_init(&f.corridor, 1 << 10);
        imap_init(&f.memo, 1 << 14);
        imap_init(&f.seghead, 1 << 12);
        int r = CORRIDOR << widen;
        for (int k = 0; k < nc; ++k)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) imap_put(&f.corridor, cell_key(ci[k] + di, cj[k] + dj), 1);
        file_canyons(&f, ci, cj, nc);
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
        L->cost = coarse_route(j->net, L->a, L->b, ROUTE_LIMIT, NULL, NULL, NULL);
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
        len[k] = dlen(dsub(node_p(net, L->b), node_p(net, L->a)));
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

/* Bowyer-Watson over the settlements' centres; the pairs of neighbours (i < j) into pairs. */
static int delaunay(int n, int (*pairs)[2], int maxPairs) {
    DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(n + 3));
    int cap = 8 * n + 16, nt = 0;
    Tri *t = (Tri *)malloc(sizeof(Tri) * (size_t)cap);
    int (*edges)[2] = malloc(sizeof(int[2]) * (size_t)cap * 3);
    if (!p || !t || !edges) fatal("out of memory triangulating the settlements");
    for (int k = 0; k < n; ++k) p[k] = plan_settlement(k).centre;
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
    double d = dlen(dsub(plan_settlement(i).centre, plan_settlement(j).centre)) / GRAVITY_D0;
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
    const int n = PLAN_SETTLEMENTS;
    qsort(c, (size_t)nc, sizeof(Cand), cand_by_cost);
    int parent[PLAN_SETTLEMENTS], deg[PLAN_SETTLEMENTS] = { 0 };
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
    int budget = (int)(ADD_PER_CITY * n), added = 0;
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
    for (int s = 0; s < PLAN_COLOSSAL; ++s)
        for (int k = 0; k < nc && deg[s] < HUB_LINKS; ++k) {
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

static int add_node(Net *net, DVec2 p, int settlement) {
    if (net->nnodes >= MAX_NODES) fatal("the road network outgrew its nodes");
    net->node[net->nnodes].p = p;
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
 * saving first, while any saves STEINER_SAVE of their length. */
static void steiner(Net *net) {
    int *start = (int *)malloc(sizeof(int) * (MAX_NODES + 1)), *incidence = (int *)malloc(sizeof(int) * 2 * MAX_LINKS);
    if (!start || !incidence) fatal("out of memory joining the roads");
    for (int round = 0; round < 4 * PLAN_SETTLEMENTS; ++round) {
        /* each node's links, in link order */
        for (int v = 0; v <= net->nnodes; ++v) start[v] = 0;
        for (int k = 0; k < net->nlinks; ++k) { start[net->link[k].a + 1]++; start[net->link[k].b + 1]++; }
        for (int v = 0; v < net->nnodes; ++v) start[v + 1] += start[v];
        for (int k = 0; k < net->nlinks; ++k) { incidence[start[net->link[k].a]++] = k; incidence[start[net->link[k].b]++] = k; }
        for (int v = net->nnodes; v > 0; --v) start[v] = start[v - 1];
        start[0] = 0;
        double bestSave = 0.0;
        int bl1 = -1, bl2 = -1, bv = -1;
        DVec2 bf = dv(0, 0);
        for (int v = 0; v < net->nnodes; ++v) {
            int ni = 0, *inc = &incidence[start[v]];
            for (int k = start[v]; k < start[v + 1]; ++k) ni++;
            /* merging two of a node's roads takes one from it: a junction must
             * keep three, a colossal city its HUB_LINKS */
            if (net->node[v].settlement < 0 && ni < 4) continue;
            if (net->node[v].settlement >= 0 && net->node[v].settlement < PLAN_COLOSSAL && ni <= HUB_LINKS) continue;
            for (int x = 0; x < ni; ++x)
                for (int y = x + 1; y < ni; ++y) {
                    const Link *l1 = &net->link[inc[x]], *l2 = &net->link[inc[y]];
                    int a = l1->a == v ? l1->b : l1->a, b = l2->a == v ? l2->b : l2->a;
                    if (a == b) continue;
                    DVec2 pv = node_p(net, v), pa = node_p(net, a), pb = node_p(net, b);
                    DVec2 ua = dnorm(dsub(pa, pv)), ub = dnorm(dsub(pb, pv));
                    if (ddot(ua, ub) <= -0.5) continue;   /* 120 degrees or more apart: no junction helps */
                    DVec2 f = fermat(pv, pa, pb);
                    double before = dlen(dsub(pa, pv)) + dlen(dsub(pb, pv));
                    double save = before - (dlen(dsub(f, pv)) + dlen(dsub(f, pa)) + dlen(dsub(f, pb)));
                    save = floor(save + 0.5);
                    if (save < STEINER_SAVE * before || save <= bestSave) continue;
                    if (plan_world_sea(f.x, f.z) < PLAN_ROAD_SEA_M || plan_city_edge(f.x, f.z) < 2.0 * CITY_CLEAR) continue;
                    bestSave = save; bl1 = inc[x]; bl2 = inc[y]; bv = v; bf = f;
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

typedef struct Cross { int link, seg; double t; DVec2 p; int node; } Cross;

static int cross_cmp(const void *pa, const void *pb) {
    const Cross *a = (const Cross *)pa, *b = (const Cross *)pb;
    if (a->link != b->link) return a->link - b->link;
    if (a->seg != b->seg) return a->seg - b->seg;
    return a->t < b->t ? -1 : a->t > b->t ? 1 : 0;
}

static void split_links(Net *net, Cross *x, int nx);

/* Step 6: wherever two links' centre lines cross, away from their ends, a
 * junction; each link crossed is split into pieces there. */
static void split_crossings(Net *net) {
    const double CELL = 20000.0, END_CLEAR = 3000.0;
    IMap grid;
    imap_init(&grid, 1 << 14);
    int *next = NULL, *lk = NULL, *sg = NULL, cn = 0, c1 = 0, c2 = 0, c3 = 0;
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        for (int s = 0; s + 1 < L->npts; ++s) {
            DVec2 a = L->pts[s], b = L->pts[s + 1];
            for (int j = (int)floor(fmin(a.z, b.z) / CELL); j <= (int)floor(fmax(a.z, b.z) / CELL); ++j)
                for (int i = (int)floor(fmin(a.x, b.x) / CELL); i <= (int)floor(fmax(a.x, b.x) / CELL); ++i) {
                    next = (int *)grow(next, &c1, cn + 1, sizeof(int));
                    lk = (int *)grow(lk, &c2, cn + 1, sizeof(int));
                    sg = (int *)grow(sg, &c3, cn + 1, sizeof(int));
                    next[cn] = imap_get(&grid, cell_key(i, j));
                    lk[cn] = l; sg[cn] = s;
                    imap_put(&grid, cell_key(i, j), cn++);
                }
        }
    }
    Cross *x = NULL;
    int nx = 0, cx = 0;
    for (int l = 0; l < net->nlinks; ++l) {
        const Link *L = &net->link[l];
        for (int s = 0; s + 1 < L->npts; ++s) {
            DVec2 a = L->pts[s], b = L->pts[s + 1], r = dsub(b, a);
            for (int j = (int)floor(fmin(a.z, b.z) / CELL); j <= (int)floor(fmax(a.z, b.z) / CELL); ++j)
                for (int i = (int)floor(fmin(a.x, b.x) / CELL); i <= (int)floor(fmax(a.x, b.x) / CELL); ++i)
                    for (int e = imap_get(&grid, cell_key(i, j)); e >= 0; e = next[e]) {
                        if (lk[e] <= l) continue;   /* each pair once, from the lower link */
                        const Link *M = &net->link[lk[e]];
                        DVec2 c = M->pts[sg[e]], sv = dsub(M->pts[sg[e] + 1], c);
                        double den = dcross(r, sv);
                        if (fabs(den) < 1e-9) continue;
                        DVec2 qp = dsub(c, a);
                        double t = dcross(qp, sv) / den, u = dcross(qp, r) / den;
                        if (t < 0.0 || t >= 1.0 || u < 0.0 || u >= 1.0) continue;
                        DVec2 p = dadd(a, dscale(r, t));
                        /* once, in the cell holding the crossing (both segments are filed there) */
                        if ((int)floor(p.x / CELL) != i || (int)floor(p.z / CELL) != j) continue;
                        /* not where they share an end (they meet there anyway) */
                        int ends[4] = { L->a, L->b, M->a, M->b }, nearEnd = 0;
                        for (int q = 0; q < 4; ++q) nearEnd |= dlen(dsub(p, node_p(net, ends[q]))) < END_CLEAR;
                        /* nor inside a settlement: both roads end there, and its gates cut them off before they meet */
                        if (nearEnd || plan_city_edge(p.x, p.z) < CITY_CLEAR) continue;
                        x = (Cross *)grow(x, &cx, nx + 2, sizeof(Cross));
                        int node = add_node(net, p, -1);
                        x[nx++] = (Cross){ l, s, t, p, node };
                        x[nx++] = (Cross){ lk[e], sg[e], u, p, node };
                    }
        }
    }
    imap_free(&grid);
    free(next); free(lk); free(sg);
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

typedef struct SegGrid { IMap head; int *next, *link, *seg; int n, c1, c2, c3; } SegGrid;

static void seggrid_add(SegGrid *g, const Link *L, int l) {
    for (int s = 0; s + 1 < L->npts; ++s) {
        DVec2 a = L->pts[s], b = L->pts[s + 1];
        int i0 = (int)floor((fmin(a.x, b.x) - MERGE_D) / MERGE_CELL), i1 = (int)floor((fmax(a.x, b.x) + MERGE_D) / MERGE_CELL);
        int j0 = (int)floor((fmin(a.z, b.z) - MERGE_D) / MERGE_CELL), j1 = (int)floor((fmax(a.z, b.z) + MERGE_D) / MERGE_CELL);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                int e = g->n++;
                g->next = (int *)grow(g->next, &g->c1, g->n, sizeof(int));
                g->link = (int *)grow(g->link, &g->c2, g->n, sizeof(int));
                g->seg = (int *)grow(g->seg, &g->c3, g->n, sizeof(int));
                g->next[e] = imap_get(&g->head, cell_key(i, j));
                g->link[e] = l;
                g->seg[e] = s;
                imap_put(&g->head, cell_key(i, j), e);
            }
    }
}

/* The nearest point to p on the links filed (only link `only` if >= 0; not
 * link `skip`), within MERGE_D: its link, segment and place along it. */
static double seggrid_nearest(const Net *net, const SegGrid *g, DVec2 p, int skip, int only, int *link, int *seg, double *t) {
    double best = MERGE_D;
    *link = -1;
    for (int e = imap_get(&g->head, cell_key((int)floor(p.x / MERGE_CELL), (int)floor(p.z / MERGE_CELL))); e >= 0; e = g->next[e]) {
        if (g->link[e] == skip || (only >= 0 && g->link[e] != only)) continue;
        const Link *L = &net->link[g->link[e]];
        DVec2 a = L->pts[g->seg[e]], ab = dsub(L->pts[g->seg[e] + 1], a);
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
    int cn3 = 0;
    for (int oi = 0; oi < n0; ++oi) {
        int l = order[oi];
        Link B = net->link[l];
        if (!B.ok) continue;
        double L = len[l];
        int ns = (int)ceil(L / MERGE_SAMPLE) + 1;
        nearL = (int *)grow(nearL, &cn1, ns, sizeof(int));
        nearS = (int *)grow(nearS, &cn2, ns, sizeof(int));
        nearT = (double *)grow(nearT, &cn3, ns, sizeof(double));
        for (int k = 0; k < ns; ++k) {
            OnLink o = along(&B, L * k / (ns - 1));
            nearL[k] = -1;
            if (plan_city_edge(o.p.x, o.p.z) < CITY_CLEAR) { nearL[k] = -2; continue; }
            seggrid_nearest(net, &g, o.p, l, -1, &nearL[k], &nearS[k], &nearT[k]);
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
        if (!nruns) { seggrid_add(&g, &net->link[l], l); continue; }
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
                seggrid_nearest(net, &g, along(&B, L * k / (ns - 1)).p, l, A, &ll, &ss, &tt);
                drop = ll == A;
            }
            if (drop) continue;
            int from = B.a, to = B.b, haveHead = r > 0, haveTail = r < nruns;
            DVec2 head = dv(0, 0), tail = dv(0, 0);
            for (int side = 0; side < 2; ++side) {
                if (side == 0 ? !haveHead : !haveTail) continue;
                int k = side == 0 ? startK : endK, AL = nearL[side == 0 ? runs[r - 1][0] : runs[r][0]];
                const Link *LA = &net->link[AL];
                DVec2 q = dadd(LA->pts[nearS[k]], dscale(dsub(LA->pts[nearS[k] + 1], LA->pts[nearS[k]]), nearT[k]));
                int j = add_node(net, q, -1);
                cut = (Cross *)grow(cut, &ccut, ncut + 1, sizeof(Cross));
                cut[ncut++] = (Cross){ AL, nearS[k], nearT[k], q, j };
                if (side == 0) { from = j; head = q; } else { to = j; tail = q; }
            }
            OnLink a = startK == 0 ? (OnLink){ B.pts[0], 0, 0.0 } : along(&B, L * startK / (ns - 1));
            OnLink b = endK == ns - 1 ? (OnLink){ B.pts[B.npts - 1], B.npts - 2, 1.0 } : along(&B, L * endK / (ns - 1));
            DVec2 *pts;
            int np = piece(&B, a, b, haveHead ? &head : NULL, haveTail ? &tail : NULL, &pts);
            int idx = kept++ == 0 ? l : add_link(net, from, to);
            Link *P = &net->link[idx];
            P->a = from; P->b = to; P->pts = pts; P->npts = np; P->ok = 1;
            P->cost = (int64_t)((double)B.cost * link_length(P) / fmax(L, 1.0) + 0.5);
            seggrid_add(&g, P, idx);
        }
        if (!kept) { net->link[l].ok = 0; net->link[l].pts = NULL; }   /* wholly beside other roads */
        free(B.pts);
    }
    imap_free(&g.head);
    free(g.next); free(g.link); free(g.seg);
    free(nearL); free(nearS); free(nearT);
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
            DVec2 end = q < e ? x[q].p : old.pts[old.npts - 1];
            int idx = q == k ? l : add_link(net, from, to);
            Link *L = &net->link[idx];
            int np = seg1 - seg0 + 2;
            DVec2 *pts = (DVec2 *)malloc(sizeof(DVec2) * (size_t)np);
            if (!pts) fatal("out of memory splitting the roads");
            pts[0] = start;
            for (int v = seg0 + 1; v <= seg1; ++v) pts[v - seg0] = old.pts[v];
            pts[np - 1] = end;
            L->a = from; L->b = to; L->pts = pts; L->npts = np; L->ok = 1;
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
            if (M->a == gone) { M->a = keep; M->pts[0] = net->node[keep].p; }
            if (M->b == gone) { M->b = keep; M->pts[M->npts - 1] = net->node[keep].p; }
        }
    }
    int *deg = (int *)calloc((size_t)net->nnodes, sizeof(int));
    if (!deg) fatal("out of memory tidying the roads");
    for (int l = 0; l < net->nlinks; ++l)
        if (net->link[l].ok) { deg[net->link[l].a]++; deg[net->link[l].b]++; }
    for (int changed = 1; changed;) {
        changed = 0;
        for (int v = PLAN_SETTLEMENTS; v < net->nnodes; ++v) {
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
            /* A's points ending at v, then B's from v on */
            DVec2 *p = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(A->npts + B->npts));
            if (!p) fatal("out of memory tidying the roads");
            int n = 0;
            for (int k = 0; k < A->npts; ++k) p[n++] = A->pts[A->a == v ? A->npts - 1 - k : k];
            for (int k = 1; k < B->npts; ++k) p[n++] = B->pts[B->a == v ? k : B->npts - 1 - k];
            free(A->pts);
            free(B->pts);
            A->pts = p; A->npts = n; A->a = x; A->b = y; A->cost += B->cost;
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
        renum[v] = v < PLAN_SETTLEMENTS || deg[v] > 0 ? m : -1;
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
    for (int s = 0; s < PLAN_SETTLEMENTS; ++s) {
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
        for (int t = s + 1; t < PLAN_SETTLEMENTS; ++t) {
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

/* Whether the town holding (x, z) is built, remembered in `cache`. */
static int built_at(IMap *cache, double x, double z) {
    int i, j;
    plan_town_at(x, z, &i, &j);
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

/* A road running out from settlement k's centre along p (n points): where it
 * leaves the built towns for the last time, on the boulevard between the
 * last built town and the next; its points from there into *out (malloc'd),
 * the first GATE_APPROACH of them (less if that would reach a built town)
 * straight out along the boulevard's normal. Returns the new count, or 0 if
 * it never leaves (then p stands). */
static int gate_out(const DVec2 *p, int n, IMap *cache, PlanGate *g, DVec2 **out) {
    double len = 0.0;
    for (int k = 0; k + 1 < n; ++k) len += dlen(dsub(p[k + 1], p[k]));
    double sIn = -1.0;
    for (double s = 0.0; s <= len; s += GATE_STEP) {
        DVec2 q = at_s(p, n, s);
        if (built_at(cache, q.x, q.z)) sIn = s;
        else if (plan_city_edge(q.x, q.z) > 0.0 && sIn >= 0.0) break;   /* past the footprint: no more built towns */
    }
    if (sIn < 0.0 || sIn + GATE_STEP > len) return 0;
    double lo = sIn, hi = sIn + GATE_STEP;
    for (int it = 0; it < 20; ++it) {
        double mid = 0.5 * (lo + hi);
        DVec2 q = at_s(p, n, mid);
        if (built_at(cache, q.x, q.z)) lo = mid; else hi = mid;
    }
    DVec2 qi = at_s(p, n, lo), qo = at_s(p, n, hi);
    int ii, ij, oi, oj;
    plan_town_at(qi.x, qi.z, &ii, &ij);
    plan_town_at(qo.x, qo.z, &oi, &oj);
    DVec2 ci = plan_town_centre(ii, ij), co = plan_town_centre(oi, oj);
    DVec2 nrm = dnorm(dsub(co, ci)), mid = dscale(dadd(ci, co), 0.5);
    DVec2 gp = at_s(p, n, 0.5 * (lo + hi));
    gp = dsub(gp, dscale(nrm, ddot(dsub(gp, mid), nrm)));   /* onto the bisector: the boulevard's centre line */
    double approach = GATE_APPROACH;
    DVec2 a = dadd(gp, dscale(nrm, approach));
    while (approach > 40.0 && built_at(cache, a.x, a.z)) { approach *= 0.5; a = dadd(gp, dscale(nrm, approach)); }
    /* on along the route from the first point well clear of the gate */
    int m = 0;
    double sk = 0.0;
    for (; m < n; ++m) {
        if (m > 0) sk += dlen(dsub(p[m], p[m - 1]));
        if (sk > hi && dlen(dsub(p[m], gp)) >= 2.0 * approach && !built_at(cache, p[m].x, p[m].z)) break;
    }
    DVec2 *o = (DVec2 *)malloc(sizeof(DVec2) * (size_t)(n - m + 3));
    if (!o) fatal("out of memory making the roads' gates");
    int c = 0;
    o[c++] = gp;
    o[c++] = a;
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
        int n = gate_out(L->pts, L->npts, &cache, &g, &o);
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
static double road_ground(DVec2 p) { return terrain_height_lod(p.x, p.z, 10.0); }

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
        g[k] = (float)road_ground(dadd(L->pts[seg], dscale(dsub(L->pts[seg + 1], L->pts[seg]), fmin(fmax(t, 0.0), 1.0))));
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
    double h0 = L->gate[0] ? terrain_height(L->pts[0].x, L->pts[0].z) : net->nodeH[L->a];
    double h1 = L->gate[1] ? terrain_height(L->pts[L->npts - 1].x, L->pts[L->npts - 1].z) : net->nodeH[L->b];
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
        net->nodeH[v] = cnt[v] ? net->nodeH[v] / cnt[v] : road_ground(net->node[v].p);
    for (int it = 0; it < 32; ++it)
        for (int l = 0; l < net->nlinks; ++l) {
            const Link *L = &net->link[l];
            double ha = L->gate[0] ? terrain_height(L->pts[0].x, L->pts[0].z) : net->nodeH[L->a];
            double hb = L->gate[1] ? terrain_height(L->pts[L->npts - 1].x, L->pts[L->npts - 1].z) : net->nodeH[L->b];
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

/* --- The index ---------------------------------------------------------------------------------- */

typedef struct IdxEntry { int64_t cell; int road, seg; } IdxEntry;

static int idx_cmp(const void *pa, const void *pb) {
    const IdxEntry *a = (const IdxEntry *)pa, *b = (const IdxEntry *)pb;
    if (a->cell != b->cell) return a->cell < b->cell ? -1 : 1;
    if (a->road != b->road) return a->road - b->road;
    return a->seg - b->seg;
}

/* Every segment of every road filed under each square within ROAD_REACH of
 * it, stepping along it (a long straight would touch far too many squares
 * by its bounds). */
static void build_index(Net *net) {
    IdxEntry *e = NULL;
    int ne = 0, ce = 0;
    for (int r = 0; r < net->nlinks; ++r) {
        const Link *L = &net->link[r];
        for (int sg = 0; sg + 1 < L->npts; ++sg) {
            DVec2 a = L->pts[sg], b = L->pts[sg + 1];
            int steps = (int)ceil(dlen(dsub(b, a)) / (0.5 * ROAD_CELL)) + 1;
            int64_t recent[16];   /* the squares this segment was last filed in: the next steps mostly reach them again */
            int nrecent = 0, wr = 0;
            for (int k = 0; k <= steps; ++k) {
                DVec2 q = dadd(a, dscale(dsub(b, a), (double)k / steps));
                int i0 = (int)floor((q.x - ROAD_REACH) / ROAD_CELL), i1 = (int)floor((q.x + ROAD_REACH) / ROAD_CELL);
                int j0 = (int)floor((q.z - ROAD_REACH) / ROAD_CELL), j1 = (int)floor((q.z + ROAD_REACH) / ROAD_CELL);
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        int64_t key = cell_key(i, j), seen = 0;
                        for (int t = 0; t < nrecent && !seen; ++t) seen = recent[t] == key;
                        if (seen) continue;
                        recent[wr++ & 15] = key;
                        if (nrecent < 16) nrecent++;
                        e = (IdxEntry *)grow(e, &ce, ne + 1, sizeof(IdxEntry));
                        e[ne++] = (IdxEntry){ key, r, sg };
                    }
            }
        }
    }
    qsort(e, (size_t)ne, sizeof(IdxEntry), idx_cmp);
    /* each segment's least and most height: the profile's stations over it and either side */
    int *segBase = (int *)malloc(sizeof(int) * (size_t)(net->nlinks + 1)), nseg = 0;
    if (!segBase) fatal("out of memory indexing the roads");
    for (int r = 0; r < net->nlinks; ++r) { segBase[r] = nseg; nseg += net->link[r].npts - 1; }
    float *segLo = (float *)malloc(sizeof(float) * (size_t)(nseg + 1)), *segHi = (float *)malloc(sizeof(float) * (size_t)(nseg + 1));
    if (!segLo || !segHi) fatal("out of memory indexing the roads");
    net->lo = 1e30f; net->hi = -1e30f;
    for (int r = 0; r < net->nlinks; ++r) {
        const Link *L = &net->link[r];
        for (int sg = 0; sg + 1 < L->npts; ++sg) {
            int k0 = (int)floor(L->s[sg] / L->ds), k1 = (int)ceil(L->s[sg + 1] / L->ds);
            float lo = 1e30f, hi = -1e30f;
            for (int k = k0 < 0 ? 0 : k0; k <= k1 && k < L->nprof; ++k) { lo = fminf(lo, L->prof[k]); hi = fmaxf(hi, L->prof[k]); }
            segLo[segBase[r] + sg] = lo;
            segHi[segBase[r] + sg] = hi;
            net->lo = fminf(net->lo, lo); net->hi = fmaxf(net->hi, hi);
        }
    }
    imap_init(&net->cells, 1 << 16);
    net->idx = (int *)malloc(sizeof(int) * 2 * (size_t)(ne + 1));
    net->idxCount = (int *)malloc(sizeof(int) * (size_t)(ne + 1));
    net->idxLo = (float *)malloc(sizeof(float) * (size_t)(ne + 1));
    net->idxHi = (float *)malloc(sizeof(float) * (size_t)(ne + 1));
    if (!net->idx || !net->idxCount || !net->idxLo || !net->idxHi) fatal("out of memory indexing the roads");
    int n = 0, cells = 0;
    for (int k = 0; k < ne; ++k) {
        if (k > 0 && e[k].cell == e[k - 1].cell && e[k].road == e[k - 1].road && e[k].seg == e[k - 1].seg) continue;
        if (k == 0 || e[k].cell != e[k - 1].cell) {
            imap_put(&net->cells, e[k].cell, n);
            net->idxCount[n] = 0;
            net->idxLo[n] = 1e30f;
            net->idxHi[n] = -1e30f;
            cells++;
        }
        int first = imap_get(&net->cells, e[k].cell), sg = segBase[e[k].road] + e[k].seg;
        net->idx[2 * n] = e[k].road;
        net->idx[2 * n + 1] = e[k].seg;
        net->idxCount[first]++;
        net->idxLo[first] = fminf(net->idxLo[first], segLo[sg]);
        net->idxHi[first] = fmaxf(net->idxHi[first], segHi[sg]);
        n++;
    }
    free(e);
    free(segBase); free(segLo); free(segHi);
}

/* --- The whole network --------------------------------------------------------------------------- */

void plan_roads_build(int threads) {
    Net *net = (Net *)calloc(1, sizeof(Net));
    if (!net) fatal("out of memory building the roads");
    for (int k = 0; k < PLAN_SETTLEMENTS; ++k) add_node(net, plan_settlement(k).centre, k);

    /* the coarse grid, for every route */
    g_coarse = (uint8_t *)malloc((size_t)COARSE_SIDE * COARSE_SIDE);
    if (!g_coarse) fatal("out of memory building the roads");
    thread_parallel_for(COARSE_SIDE, threads, coarse_row, NULL);

    /* 1. candidates, routed coarsely for their costs */
    int (*pairs)[2] = malloc(sizeof(int[2]) * 8 * PLAN_SETTLEMENTS);
    if (!pairs) fatal("out of memory building the roads");
    int np = delaunay(PLAN_SETTLEMENTS, pairs, 8 * PLAN_SETTLEMENTS), nc = 0;
    Cand *cand = (Cand *)calloc((size_t)np, sizeof(Cand));
    {
        /* those longer than CAND_REACH only where the shorter leave their ends
         * apart: the shortest across each gap, until every continent is whole */
        int *parent = (int *)malloc(sizeof(int) * PLAN_SETTLEMENTS);
        double *len = (double *)malloc(sizeof(double) * (size_t)np);
        if (!cand || !parent || !len) fatal("out of memory building the roads");
        int ncand[PLAN_SETTLEMENTS] = { 0 };
        for (int v = 0; v < PLAN_SETTLEMENTS; ++v) parent[v] = v;
        for (int k = 0; k < np; ++k) {
            PlanSettlement a = plan_settlement(pairs[k][0]), b = plan_settlement(pairs[k][1]);
            len[k] = a.continent != b.continent ? -1.0 : dlen(dsub(a.centre, b.centre));
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
                        if (pass == 1 ? (a < PLAN_COLOSSAL && ncand[a] < HUB_LINKS) || (b < PLAN_COLOSSAL && ncand[b] < HUB_LINKS)
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
    thread_parallel_for(net->nlinks, threads, profile_ground_job, net);
    junction_heights(net);
    thread_parallel_for(net->nlinks, threads, profile_grade_job, net);
    build_index(net);
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

/* The nearest road's reach over (x, z): its surface where (x, z) is on it or
 * its shoulders, then a batter sloping to the ground, a smoothstep across
 * twice the cut or fill (between the batter's least and greatest), widened
 * by the spacing so coarse samples still see it. Where reaches overlap, the
 * strongest. */
double plan_road_ground(double x, double z, double ground, double spacing, double *road_h) {
    const Net *net = g_net;
    if (!net || spacing > PLAN_ROAD_LOD_SPACING) return 0.0;
    int first = imap_get(&net->cells, cell_key((int)floor(x / ROAD_CELL), (int)floor(z / ROAD_CELL)));
    if (first < 0) return 0.0;
    double best = 0.0, bestD = 1e300;
    DVec2 p = dv(x, z);
    for (int k = first; k < first + net->idxCount[first]; ++k) {
        const PlanRoad *R = &net->road[net->idx[2 * k]];
        int sg = net->idx[2 * k + 1];
        DVec2 a = R->pts[sg], ab = dsub(R->pts[sg + 1], a);
        double l2 = ddot(ab, ab), t = l2 > 0.0 ? ddot(dsub(p, a), ab) / l2 : 0.0;
        t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
        double d = dlen(dsub(p, dadd(a, dscale(ab, t)))), core = R->half + PLAN_ROAD_SHOULDER;
        if (d >= core + PLAN_ROAD_BATTER_MAX + 1.5 * spacing) continue;
        double h = profile_at(R, R->s[sg] + t * sqrt(l2));
        double batter = fmin(fmax(2.0 * fabs(ground - h), PLAN_ROAD_BATTER_MIN), PLAN_ROAD_BATTER_MAX) + 1.5 * spacing;
        double w = 1.0;
        if (d > core) {
            double u = fmin((d - core) / batter, 1.0);
            w = 1.0 - u * u * (3.0 - 2.0 * u);
        }
        if (w > best || (w == best && w > 0.0 && d < bestD)) { best = w; bestD = d; *road_h = h; }
    }
    return best;
}
