/* earth_streams: derives the land's small streams from the Earth bake and
 * the procedural land, for the engine to load beside the bake.
 *
 *   earth_streams <bake> <out.streams> [--stage <stage.r16> | --runoff-const L]
 *                 [--cell M] [--min-flow Q] [--halo M] [--tolerance M] [--threads T]
 *   earth_streams --info <bake> <streams>
 *
 * The ground is the land with the settlements on it (terrain_ground_bare_dir:
 * the Earth, the region's landforms and canyons, the cities' ground, but
 * neither the roads, which the streams pass under, nor the water's own beds),
 * the settlements placed first (plan_world_init), read on each cube face's
 * equal-angle grid of `cell` metres (default 500) at that spacing, with a
 * halo of `halo` metres (default 30 km) past the face's edges. Inside a city
 * the streams run on through its ground, buried: the engine carves none
 * there. A cell is sea where terrain_shore is below 0, water where the bake's
 * rivers and lakes come within half a cell (terrain_earth_water), and land
 * otherwise. Every land cell drains to a neighbour (of eight) by a
 * priority flood from the sea, the water and the grid's border: the lowest
 * open cell is taken next, and each neighbour it reaches drains to it, its
 * level raised to the taker's if it lay lower (a pit fills to its spill), so
 * the levels never rise along the way down. Each cell sheds its area times
 * the runoff where it lies (the project's runoff layer, staged beside the
 * stage as <stage>.runoff, litres a second per km2; or --runoff-const), and
 * the flow is summed down the drainage. A stream is every land cell of at
 * least `min-flow` m3/s (default 0.5). Its links run from each head, each
 * junction and each cell a stream enters from the halo, down to the next
 * junction, the sea, the water, or the halo (where the next face's link
 * starts: the ends are joined across the seams); corner-cut twice (Chaikin),
 * the ends kept, then thinned to within `tolerance` metres (default 60) and
 * BED_TOL of the bed. An end at the sea is moved to the coast (terrain_shore
 * 0), at the water to its edge. The width is 7 sqrt(flow) m, as the
 * project's rivers'.
 *
 * The beds. Along the streams' drainage tree the water's level is fitted to
 * the land at the stream's cells, never rising downstream, with the least
 * total |cut| + |fill| (an L1 isotonic regression on the tree, solved
 * leaf to root with mergeable heaps: tree_fit); the bed is that level less
 * the channel's depth, DEPTH_K flow^DEPTH_EXP. The bake's rivers get theirs
 * the same way, the land read at each point, along their reaches, each
 * reach's end running on into the nearest other reach that starts within
 * REACH_JOIN_M of it. A lake's level is the lowest land on its shore, its
 * every ring's (each ring is told an outer ring or a hole by its winding
 * and the side the water lies on, and the holes follow their outer ring);
 * buckets inside a lake that no shore comes near take its level from the
 * buckets round them.
 *
 * File layout, all little-endian (f32 float), the header 512 bytes:
 *      0  char[8] "NCSTREAM"     8  u32 version (1)    12  u32 the grid's cells a face side
 *     16  f32 cell (m)          20  f32 the least flow (m3/s)   24  f32 width per root of flow (7)
 *     28  u32 probes P (24)     32  char[64] the bake's stage sha256 (its header's at 128)
 *     96  u32 buckets a side B 100  f32 reach (m)      104  u32 points V   108  u32 bucket entries E
 *    112  u32 links           116  u32 0
 *    120  P x 16 bytes: f32 x, y, z (a direction on land), f32 the land there at `cell` (world Y, m),
 *         so the engine can tell when the land has changed since
 *   then the bake's water section (src/terrain/terrain_earth.c, tools/earth_water.c): V points of kind
 *   3, each link a run, the buckets (no lakes: inside is 0), then f32 bed[V] (world Y, m) and
 *   f32 flow[V] (m3/s);
 *   then the plan's probes, the header's count at 116: f32 x 4 each, the first the
 *   settlements' count and three 0s, the rest a direction and plan_city_edge there, round
 *   every settlement's disc, so the engine can tell when the settlements have changed;
 *   then u32 the bake's water points Vb and f32 bed[Vb] (a river point's bed, a lake point's
 *   lake's level; world Y, m); then u32 L, u32 bucket[L] (ascending: face, then row, then
 *   column) and f32 level[L], the lakes' levels in the buckets inside them that list no shore.
 *
 * The same bake, stage and options give the same bytes, whatever the thread count. */
#include "terrain_internal.h"
#include "plan.h"
#include "thread.h"
#include "earth_water.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI            3.14159265358979323846
#define MAGIC         "NCSTREAM"
#define VERSION       2
#define HEADER        512
#define PROBES        24
#define WIDTH_K       7.0       /* m per root of m3/s, as earth.py's WATER_WIDTH_K */
#define BED_TOL       2.0       /* m: thinning keeps the bed within this */
#define SMOOTH        2         /* rounds of corner cutting */
#define BUCKETS       1024      /* a face side, as the bake's (9.8 km) */
#define REACH_M       2000.0    /* as the bake's */
#define SEAM_JOIN     1.5       /* cells: a link's end in the halo joins the next face's start this near */
#define DEPTH_K       0.27      /* a channel's depth (m) is this times its flow (m3/s) to the... */
#define DEPTH_EXP     0.39      /* ...this (Leopold and Maddock's hydraulic geometry) */
#define PLAN_PROBES   8         /* plan probes round each settlement's disc, besides its centre */
#define REACH_JOIN_M  1500.0    /* a reach's end runs on into the nearest other reach starting this near (HydroRIVERS steps 15" cells) */
#define SIDE_M        10.0      /* a lake's ring is told outer or hole by the water this far to the left of its longest side */

enum { LAND = 0, SEA = 1, WATER = 2 };
enum { END_JUNCTION = 0, END_SEA, END_WATER, END_SEAM, END_GRID, END_KINDS };
static const char *END_NAME[END_KINDS] = { "junction", "sea", "water", "face seam", "grid edge" };
static const int DI[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, DJ[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
#define NONE  0xFFu       /* a land cell not yet reached */
#define OFF   8u          /* drains off the grid */

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "earth_streams: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

static void put_u32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_f32(unsigned char *p, float v) { uint32_t u; memcpy(&u, &v, 4); put_u32(p, u); }
static uint32_t get_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static double   get_f64(const unsigned char *p) { uint64_t u = (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; double v; memcpy(&v, &u, 8); return v; }
static float    get_f32(const unsigned char *p) { uint32_t u = get_u32(p); float v; memcpy(&v, &u, 4); return v; }

/* --- The runoff -------------------------------------------------------------------------- */

typedef struct Runoff {
    int    W, H;
    double lon0, lat0, cell, constant;   /* constant >= 0: everywhere that, no grid */
    float *v;                            /* L/s/km2, rows north to south, NaN without data */
} Runoff;

/* The staged runoff named by the stage's sidecar. */
static int runoff_read(Runoff *r, const char *stage) {
    char side[1100], line[1024], key[64], val[960], file[960] = "";
    snprintf(side, sizeof side, "%s.txt", stage);
    FILE *f = fopen(side, "r");
    if (!f) { fprintf(stderr, "earth_streams: cannot open %s\n", side); return 0; }
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%63s %959s", key, val) == 2 && !strcmp(key, "runoff_file")) snprintf(file, sizeof file, "%s", val);
    fclose(f);
    if (!file[0]) { fprintf(stderr, "earth_streams: %s names no runoff_file (earth.py runoff, then stage)\n", side); return 0; }
    char path[2100];
    const char *slash = strrchr(stage, '/'), *bs = strrchr(stage, '\\');
    if (bs > slash) slash = bs;
    snprintf(path, sizeof path, "%.*s%s", slash ? (int)(slash - stage + 1) : 0, stage, file);
    f = fopen(path, "rb");
    unsigned char hd[44];
    if (!f || fread(hd, 1, 44, f) != 44 || memcmp(hd, "NCRUNOF1", 8) != 0 || get_u32(hd + 8) != 1) {
        fprintf(stderr, "earth_streams: %s isn't a staged runoff (NCRUNOF1 version 1)\n", path);
        if (f) fclose(f);
        return 0;
    }
    r->W = (int)get_u32(hd + 12); r->H = (int)get_u32(hd + 16);
    r->lon0 = get_f64(hd + 20); r->lat0 = get_f64(hd + 28); r->cell = get_f64(hd + 36);
    r->constant = -1.0;
    size_t n = (size_t)r->W * r->H;
    r->v = (float *)xmalloc(n * sizeof(float));
    unsigned char *buf = (unsigned char *)xmalloc(n * 4);
    int ok = fread(buf, 4, n, f) == n && fgetc(f) == EOF;
    fclose(f);
    for (size_t k = 0; k < n; ++k) r->v[k] = get_f32(buf + 4 * k);
    free(buf);
    if (!ok || r->W <= 0 || r->H <= 0 || fabs(r->W * r->cell - 360.0) > 1e-6) { fprintf(stderr, "earth_streams: %s is truncated or not global\n", path); return 0; }
    printf("runoff: %s, %d x %d cells of %g degrees\n", path, r->W, r->H, r->cell);
    return 1;
}

/* L/s/km2 at (lat, lon): bilinear between the cells' centres, over the
 * corners that have data (0 where none does). */
static double runoff_at(const Runoff *r, double lat, double lon) {
    if (r->constant >= 0.0) return r->constant;
    double x = (lon - r->lon0) / r->cell - 0.5, y = (r->lat0 - lat) / r->cell - 0.5;
    int x0 = (int)floor(x), y0 = (int)floor(y);
    double fx = x - x0, fy = y - y0, sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 4; ++k) {
        int xi = x0 + (k & 1), yi = y0 + (k >> 1);
        xi = ((xi % r->W) + r->W) % r->W;
        yi = yi < 0 ? 0 : yi >= r->H ? r->H - 1 : yi;
        float v = r->v[(size_t)yi * r->W + xi];
        if (isnan(v)) continue;
        double w = ((k & 1) ? fx : 1.0 - fx) * ((k >> 1) ? fy : 1.0 - fy);
        sum += w * v; wsum += w;
    }
    return wsum > 1e-9 ? sum / wsum : 0.0;
}

/* --- A face's grid ----------------------------------------------------------------------- */

typedef struct Grid {
    int      face, n, H, G;      /* cells a face side, the halo, the grid's side (n + 2 H) */
    double   cell;
    float   *lv;                 /* G G: the land's height, then the flood's level (world Y, m) */
    float   *q;                  /* G G: the cell's own flow, then all of it from upstream (m3/s) */
    uint8_t *cls, *rcv, *inc;    /* G G: LAND / SEA / WATER; the neighbour it drains to (0..7, OFF, NONE); streams in (& 15), one from the halo (16) */
    const Runoff *runoff;
} Grid;

static TerrainDir cell_dir(const Grid *g, int i, int j) {
    double w = 2.0 / g->n;
    return terrain_face_dir(g->face, -1.0 + (i - g->H + 0.5) * w, -1.0 + (j - g->H + 0.5) * w);
}

static int in_core(const Grid *g, int i, int j) { return i >= g->H && j >= g->H && i < g->H + g->n && j < g->H + g->n; }

/* The cell's area on the sphere (km2): the equal-angle face's area element
 * R^2 (pi/4)^2 (1 + a^2)(1 + b^2) / (1 + a^2 + b^2)^1.5 per du dv. */
static double cell_area(const Grid *g, int i, int j) {
    double w = 2.0 / g->n, u = -1.0 + (i - g->H + 0.5) * w, v = -1.0 + (j - g->H + 0.5) * w;
    double a = tan(0.25 * PI * u), b = tan(0.25 * PI * v), k = 0.25 * PI * w * PLANET_RADIUS;
    return k * k * (1.0 + a * a) * (1.0 + b * b) / pow(1.0 + a * a + b * b, 1.5) / 1e6;
}

static void classify_task(void *ctx, int j) {
    Grid *g = (Grid *)ctx;
    for (int i = 0; i < g->G; ++i) {
        size_t c = (size_t)j * g->G + i;
        TerrainDir d = cell_dir(g, i, j);
        g->rcv[c] = NONE; g->inc[c] = 0; g->q[c] = 0.0f; g->lv[c] = 0.0f;
        if (terrain_shore(d, g->cell) < 0.0) { g->cls[c] = SEA; continue; }
        TerrainEarthWater w;
        /* the edge reads the reach beyond it, so a coarse grid's half cell is held within it */
        if (terrain_earth_water(d, &w) && w.edge < fmin(0.5 * g->cell, 0.5 * w.reach)) { g->cls[c] = WATER; continue; }
        double lift, lat, lon;
        g->cls[c] = LAND;
        (void)lift;
        g->lv[c] = (float)terrain_ground_bare_dir(d, g->cell);
        terrain_dir_latlon(d, &lat, &lon);
        g->q[c] = (float)(cell_area(g, i, j) * runoff_at(g->runoff, lat, lon) / 1000.0);
    }
}

/* --- The flood ----------------------------------------------------------------------------
 * A binary heap of (level, cell), the lower first and ties by cell, and a
 * queue for the cells a pit fills, taken before the heap: Barnes, Lehman
 * and Mulla's priority flood (2014). */
typedef struct Heap { float *key; uint32_t *id; size_t n, cap; } Heap;

static int heap_less(const Heap *h, size_t a, size_t b) { return h->key[a] < h->key[b] || (h->key[a] == h->key[b] && h->id[a] < h->id[b]); }

static void heap_swap(Heap *h, size_t a, size_t b) {
    float k = h->key[a]; h->key[a] = h->key[b]; h->key[b] = k;
    uint32_t i = h->id[a]; h->id[a] = h->id[b]; h->id[b] = i;
}

static void heap_push(Heap *h, float key, uint32_t id) {
    if (h->n == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 1 << 20;
        h->key = (float *)realloc(h->key, h->cap * sizeof(float)); h->id = (uint32_t *)realloc(h->id, h->cap * sizeof(uint32_t));
        if (!h->key || !h->id) { fprintf(stderr, "earth_streams: out of memory for the flood\n"); exit(1); }
    }
    size_t k = h->n++;
    h->key[k] = key; h->id[k] = id;
    while (k > 0 && heap_less(h, k, (k - 1) / 2)) { heap_swap(h, k, (k - 1) / 2); k = (k - 1) / 2; }
}

static uint32_t heap_pop(Heap *h) {
    uint32_t top = h->id[0];
    h->key[0] = h->key[--h->n]; h->id[0] = h->id[h->n];
    size_t k = 0;
    for (;;) {
        size_t l = 2 * k + 1, r = l + 1, m = k;
        if (l < h->n && heap_less(h, l, m)) m = l;
        if (r < h->n && heap_less(h, r, m)) m = r;
        if (m == k) break;
        heap_swap(h, k, m);
        k = m;
    }
    return top;
}

/* Floods the grid: every land cell's receiver and level; the land cells in
 * the order taken (each after the one it drains to). Returns how many. */
static size_t flood(Grid *g, uint32_t *order) {
    int G = g->G;
    Heap h = { 0 };
    uint32_t *pit = order;   /* the pit queue runs through order itself: a cell is queued once and taken in turn */
    size_t taken = 0, queued = 0;
    long off[8];
    for (int k = 0; k < 8; ++k) off[k] = (long)DJ[k] * G + DI[k];
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            size_t c = (size_t)j * G + i;
            if (g->cls[c] != LAND) continue;
            if (i == 0 || j == 0 || i == G - 1 || j == G - 1) { g->rcv[c] = OFF; heap_push(&h, g->lv[c], (uint32_t)c); continue; }
            for (int k = 0; k < 8; ++k)
                if (g->cls[c + off[k]] != LAND) { g->rcv[c] = (uint8_t)k; heap_push(&h, g->lv[c], (uint32_t)c); break; }
        }
    /* order[0, taken) is the cells taken; order[taken, queued) the pit queue's waiting cells */
    while (h.n > 0 || taken < queued) {
        uint32_t c;
        if (taken < queued) c = pit[taken];
        else { c = heap_pop(&h); order[queued++] = c; }
        ++taken;
        float level = g->lv[c];
        int ci = (int)(c % G), cj = (int)(c / G);
        for (int k = 0; k < 8; ++k) {
            int ni = ci + DI[k], nj = cj + DJ[k];
            if (ni < 0 || nj < 0 || ni >= G || nj >= G) continue;
            size_t n = c + off[k];
            if (g->cls[n] != LAND || g->rcv[n] != NONE) continue;
            g->rcv[n] = (uint8_t)((k + 4) & 7);
            if (g->lv[n] <= level) { g->lv[n] = level; pit[queued++] = (uint32_t)n; }
            else heap_push(&h, g->lv[n], (uint32_t)n);
        }
    }
    free(h.key); free(h.id);
    return taken;
}

/* --- The links ------------------------------------------------------------------------------ */

typedef struct Pt { TerrainDir d; double bed, flow; } Pt;

typedef struct Link {
    uint32_t first, count;   /* into the points */
    int      face, end;      /* END_* */
} Link;

typedef struct Links {
    Pt     *pt;
    Link   *link;
    size_t  np, cp, nl, cl;
    size_t  ends[END_KINDS];
    double  carve_sum, carve_max, fill_sum, fill_max;   /* the land at the stream's cells against its bed (m) */
    size_t  cells, ponded;                              /* stream cells, and those more than 2 m under their bed */
} Links;

static void add_pt(Links *L, Pt p) {
    if (L->np == L->cp) {
        L->cp = L->cp ? 2 * L->cp : 1 << 20;
        L->pt = (Pt *)realloc(L->pt, L->cp * sizeof(Pt));
        if (!L->pt) { fprintf(stderr, "earth_streams: out of memory for the points\n"); exit(1); }
    }
    L->pt[L->np++] = p;
}

static int is_start(const Grid *g, size_t c) { return (g->inc[c] & 15) != 1 || (g->inc[c] & 16); }

/* Corner cutting on a run of points, the ends kept: directions, beds and flows alike. */
static int chaikin(Pt *p, int n, Pt *tmp) {
    for (int round = 0; round < SMOOTH && n >= 3; ++round) {
        int m = 0;
        tmp[m++] = p[0];
        for (int k = 0; k + 1 < n; ++k) {
            for (int s = 0; s < 2; ++s) {
                if ((k == 0 && s == 0) || (k == n - 2 && s == 1)) continue;
                double a = s == 0 ? 0.75 : 0.25, b = 1.0 - a;
                Pt q;
                q.d = unit3(add3(scale3(p[k].d, a), scale3(p[k + 1].d, b)));
                q.bed = a * p[k].bed + b * p[k + 1].bed;
                q.flow = a * p[k].flow + b * p[k + 1].flow;
                tmp[m++] = q;
            }
        }
        tmp[m++] = p[n - 1];
        memcpy(p, tmp, (size_t)m * sizeof(Pt));
        n = m;
    }
    return n;
}

/* The point where f changes sign between a (f >= 0) and b (f < 0), along the arc. */
static TerrainDir bisect(TerrainDir a, TerrainDir b, double (*f)(TerrainDir, double), double cell) {
    for (int k = 0; k < 40; ++k) {
        TerrainDir m = unit3(add3(a, b));
        if (f(m, cell) >= 0.0) a = m; else b = m;
    }
    return unit3(add3(a, b));
}

static double shore_f(TerrainDir p, double cell) { return terrain_shore(p, cell); }
static double water_f(TerrainDir p, double cell) { TerrainEarthWater w; (void)cell; terrain_earth_water(p, &w); return w.edge; }

/* A link's end at the water moved to the water's edge: along the last
 * segment if it crosses, else down the edge's gradient (the edge is a
 * distance, so a step of its value reaches it). */
static TerrainDir to_edge(TerrainDir a, TerrainDir b, double cell) {
    if (water_f(a, cell) < 0.0) return b;
    if (water_f(b, cell) < 0.0) return bisect(a, b, water_f, cell);
    TerrainDir p = b, e, nn;
    for (int it = 0; it < 6; ++it) {
        double v = water_f(p, cell);
        if (v < 0.5) return p;
        TerrainDir up = { 0, 0, 1 };
        e = unit3(cross3(up, p));
        nn = cross3(p, e);
        double h = 5.0 / PLANET_RADIUS;
        double ge = (water_f(unit3(add3(p, scale3(e, h))), cell) - water_f(unit3(sub3(p, scale3(e, h))), cell)) / 10.0;
        double gn = (water_f(unit3(add3(p, scale3(nn, h))), cell) - water_f(unit3(sub3(p, scale3(nn, h))), cell)) / 10.0;
        double gg = ge * ge + gn * gn;
        if (gg < 0.25) break;
        p = unit3(add3(p, scale3(add3(scale3(e, ge), scale3(nn, gn)), -(v + 0.5) / gg / PLANET_RADIUS)));
    }
    return water_f(p, cell) < 1.0 ? p : b;
}

/* Thins a run to within tol (radians) across and BED_TOL of the bed between
 * the points kept (Douglas-Peucker on both): marks the points kept. */
static void thin(const Pt *p, int n, double tol, unsigned char *keep, int *stack, double *along) {
    along[0] = 0.0;
    for (int k = 1; k < n; ++k) along[k] = along[k - 1] + sqrt(dot3(sub3(p[k].d, p[k - 1].d), sub3(p[k].d, p[k - 1].d)));
    memset(keep, 0, (size_t)n);
    keep[0] = keep[n - 1] = 1;
    int top = 0;
    stack[top++] = 0; stack[top++] = n - 1;
    while (top > 0) {
        int j = stack[--top], i = stack[--top];
        if (j <= i + 1) continue;
        double worst = 1.0; int at = -1;
        for (int k = i + 1; k < j; ++k) {
            double t = along[j] > along[i] ? (along[k] - along[i]) / (along[j] - along[i]) : 0.0;
            double e = fmax(water_arc_dist(p[k].d, p[i].d, p[j].d) / tol, fabs(p[k].bed - (p[i].bed + t * (p[j].bed - p[i].bed))) / BED_TOL);
            if (e > worst) { worst = e; at = k; }
        }
        if (at >= 0) { keep[at] = 1; stack[top++] = i; stack[top++] = at; stack[top++] = at; stack[top++] = j; }
    }
}

typedef struct Scratch { Pt *run, *tmp; unsigned char *keep; int *stack; double *along; size_t cap; } Scratch;

static void scratch_fit(Scratch *s, size_t n) {
    size_t need = 4 * n + 8;   /* corner cutting doubles a run twice */
    if (need <= s->cap) return;
    free(s->run); free(s->tmp); free(s->keep); free(s->stack); free(s->along);
    s->cap = need * 2;
    s->run = (Pt *)xmalloc(s->cap * sizeof(Pt)); s->tmp = (Pt *)xmalloc(s->cap * sizeof(Pt));
    s->keep = (unsigned char *)xmalloc(s->cap); s->stack = (int *)xmalloc(4 * s->cap * sizeof(int)); s->along = (double *)xmalloc(s->cap * sizeof(double));
}

/* Every link that starts in the face's core, from each start down. */
static void trace(const Grid *g, Links *L, double tol, Scratch *s) {
    int G = g->G;
    long off[8];
    for (int k = 0; k < 8; ++k) off[k] = (long)DJ[k] * G + DI[k];
    size_t *cells = NULL, ccap = 0;
    for (int j = g->H; j < g->H + g->n; ++j)
        for (int i = g->H; i < g->H + g->n; ++i) {
            size_t c = (size_t)j * G + i;
            if (g->cls[c] != LAND || !(g->inc[c] & 32) || !is_start(g, c)) continue;
            size_t n = 0;
            int end;
            for (size_t at = c;;) {
                if (n + 1 >= ccap) { ccap = ccap ? 2 * ccap : 4096; cells = (size_t *)realloc(cells, ccap * sizeof(size_t)); if (!cells) exit(1); }
                cells[n++] = at;
                if (g->rcv[at] == OFF) { end = END_GRID; break; }
                size_t next = at + off[g->rcv[at]];
                int ni = (int)(next % G), nj = (int)(next / G);
                if (g->cls[next] != LAND) { cells[n++] = next; end = g->cls[next] == SEA ? END_SEA : END_WATER; break; }
                if (!in_core(g, ni, nj)) { cells[n++] = next; end = END_SEAM; break; }
                if (is_start(g, next)) { cells[n++] = next; end = END_JUNCTION; break; }
                at = next;
            }
            scratch_fit(s, n);
            for (size_t k = 0; k < n; ++k) {
                size_t a = cells[k];
                Pt p;
                p.d = cell_dir(g, (int)(a % G), (int)(a / G));
                int land = g->cls[a] == LAND;
                p.bed = land ? g->lv[a] : s->run[k - 1].bed;
                p.flow = land ? g->q[a] : s->run[k - 1].flow;
                s->run[k] = p;
                if (land && k + 1 < n) {   /* the land at the cell's centre against the bed */
                    double h = terrain_ground_bare_dir(p.d, g->cell), dz = h - p.bed;
                    L->cells++;
                    if (dz > 0.0) { L->carve_sum += dz; L->carve_max = fmax(L->carve_max, dz); }
                    else { L->fill_sum -= dz; L->fill_max = fmax(L->fill_max, -dz); L->ponded += dz < -2.0; }
                }
            }
            int m = chaikin(s->run, (int)n, s->tmp);
            Pt *r = s->run;
            for (int k = 1; k < m; ++k) {   /* never rising, never thinning, downstream */
                if (r[k].bed > r[k - 1].bed) r[k].bed = r[k - 1].bed;
                if (r[k].flow < r[k - 1].flow) r[k].flow = r[k - 1].flow;
            }
            if (m >= 2 && end == END_SEA && shore_f(r[m - 2].d, g->cell) >= 0.0) r[m - 1].d = bisect(r[m - 2].d, r[m - 1].d, shore_f, g->cell);
            if (m >= 2 && end == END_WATER) r[m - 1].d = to_edge(r[m - 2].d, r[m - 1].d, g->cell);
            thin(r, m, tol, s->keep, s->stack, s->along);
            if (L->nl == L->cl) {
                L->cl = L->cl ? 2 * L->cl : 1 << 16;
                L->link = (Link *)realloc(L->link, L->cl * sizeof(Link));
                if (!L->link) { fprintf(stderr, "earth_streams: out of memory for the links\n"); exit(1); }
            }
            Link *lk = &L->link[L->nl++];
            lk->first = (uint32_t)L->np; lk->face = g->face; lk->end = end;
            for (int k = 0; k < m; ++k) if (s->keep[k]) add_pt(L, r[k]);
            lk->count = (uint32_t)(L->np - lk->first);
            L->ends[end]++;
        }
    free(cells);
}

/* Marks the streams and counts how many flow into each: bit 32 is "a stream". */
static void mark_streams(Grid *g, double min_flow) {
    int G = g->G;
    long off[8];
    for (int k = 0; k < 8; ++k) off[k] = (long)DJ[k] * G + DI[k];
    for (size_t c = 0; c < (size_t)G * G; ++c)
        if (g->cls[c] == LAND && g->q[c] >= min_flow) g->inc[c] |= 32;
    for (int j = 0; j < G; ++j)
        for (int i = 0; i < G; ++i) {
            size_t c = (size_t)j * G + i;
            if (!(g->inc[c] & 32) || g->rcv[c] >= OFF) continue;
            size_t n = c + off[g->rcv[c]];
            if (g->cls[n] != LAND) continue;
            if ((g->inc[n] & 15) < 15) g->inc[n]++;
            if (!in_core(g, i, j)) g->inc[n] |= 16;
        }
}

/* --- The beds -------------------------------------------------------------------------------
 * An L1 isotonic regression on a forest: levels x minimising the sum of
 * |x - h| over the nodes, with no node's x below its parent's (the parent is
 * downstream). Leaf to root, each node's cost as a function of its own level
 * is convex and piecewise linear; what its parent sees of it, the least cost
 * at that level or above, is flat to the left of its least point and kept as
 * the multiset of the points where its slope steps up by one (a leftist
 * heap, the least on top). A node's own cost adds h twice (slope -1 to +1)
 * to the melded heaps of its children; its least point is then the heap's
 * least, which is taken out once to flatten the left. Root to leaf, a node's
 * level is the larger of its parent's and its own least point. */
typedef struct LHeap { float *key; int32_t *l, *r; uint8_t *rank; size_t n, cap; } LHeap;

static int32_t lh_new(LHeap *h, float key) {
    if (h->n == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 1 << 16;
        h->key = (float *)realloc(h->key, h->cap * sizeof(float)); h->l = (int32_t *)realloc(h->l, h->cap * sizeof(int32_t));
        h->r = (int32_t *)realloc(h->r, h->cap * sizeof(int32_t)); h->rank = (uint8_t *)realloc(h->rank, h->cap);
        if (!h->key || !h->l || !h->r || !h->rank) { fprintf(stderr, "earth_streams: out of memory for the beds\n"); exit(1); }
    }
    int32_t k = (int32_t)h->n++;
    h->key[k] = key; h->l[k] = h->r[k] = -1; h->rank[k] = 1;
    return k;
}

static int lh_rank(const LHeap *h, int32_t a) { return a < 0 ? 0 : h->rank[a]; }

/* The right spines are at most log n long, so the recursion is shallow. */
static int32_t lh_meld(LHeap *h, int32_t a, int32_t b) {
    if (a < 0) return b;
    if (b < 0) return a;
    if (h->key[b] < h->key[a]) { int32_t t = a; a = b; b = t; }
    h->r[a] = lh_meld(h, h->r[a], b);
    if (lh_rank(h, h->l[a]) < lh_rank(h, h->r[a])) { int32_t t = h->l[a]; h->l[a] = h->r[a]; h->r[a] = t; }
    int rk = lh_rank(h, h->r[a]) + 1;
    h->rank[a] = (uint8_t)(rk > 255 ? 255 : rk);
    return a;
}

/* Levels x[n] fitted to h[n]: parent[i] is i's parent or -1 (a root), and
 * up[0 .. m) lists the nodes fitted, each after all its children (the rest
 * are left as they are). */
static void tree_fit(size_t n, const int32_t *parent, const float *h, const uint32_t *up, size_t m, float *x) {
    LHeap hp;
    memset(&hp, 0, sizeof hp);
    int32_t *root = (int32_t *)xmalloc(n * sizeof(int32_t));
    float *least = (float *)xmalloc(n * sizeof(float));
    for (size_t i = 0; i < n; ++i) root[i] = -1;
    for (size_t k = 0; k < m; ++k) {
        uint32_t i = up[k];
        int32_t r = lh_meld(&hp, root[i], lh_meld(&hp, lh_new(&hp, h[i]), lh_new(&hp, h[i])));
        least[i] = hp.key[r];
        r = lh_meld(&hp, hp.l[r], hp.r[r]);
        if (parent[i] >= 0) root[parent[i]] = lh_meld(&hp, root[parent[i]], r);
    }
    for (size_t k = m; k-- > 0;) {
        uint32_t i = up[k];
        x[i] = parent[i] >= 0 && x[parent[i]] > least[i] ? x[parent[i]] : least[i];
    }
    free(hp.key); free(hp.l); free(hp.r); free(hp.rank); free(root); free(least);
}

static double channel_depth(double flow) { return DEPTH_K * pow(flow > 0.0 ? flow : 0.0, DEPTH_EXP); }

/* How the fitted levels sit against the land (m). */
typedef struct FitStats { double cut, fill, cut_max, fill_max; size_t n, over2; } FitStats;

static void fit_stats(FitStats *s, double h, double x) {
    double d = h - x;
    s->n++;
    if (d >= 0.0) { s->cut += d; s->cut_max = fmax(s->cut_max, d); }
    else { s->fill -= d; s->fill_max = fmax(s->fill_max, -d); }
    s->over2 += fabs(d) > 2.0;
}

static void fit_print(const char *what, const FitStats *s) {
    printf("%s: %zu fitted against the land: cut %.2f m on average (at most %.0f), filled %.2f m (at most %.0f); %.1f%% more than 2 m off\n",
           what, s->n, s->cut / fmax(1.0, (double)s->n), s->cut_max, s->fill / fmax(1.0, (double)s->n), s->fill_max, 100.0 * s->over2 / fmax(1.0, (double)s->n));
}

typedef struct LandCtx { const TerrainDir *d; float *h; double cell; } LandCtx;

static void land_task(void *ctx, int k) {
    LandCtx *c = (LandCtx *)ctx;
    for (int i = 0; i < 1024; ++i) {
        size_t at = (size_t)k * 1024 + i;
        if (c->d[at].x == 0.0 && c->d[at].y == 0.0 && c->d[at].z == 0.0) continue;
        c->h[at] = (float)terrain_ground_bare_dir(c->d[at], c->cell);
    }
}

/* The land at n directions (zero ones skipped), on threads. */
static void land_at(TerrainDir *d, size_t n, float *h, double cell, int threads) {
    size_t padded = (n + 1023) / 1024 * 1024;
    TerrainDir *dd = d;
    if (padded != n) {
        dd = (TerrainDir *)xmalloc(padded * sizeof(TerrainDir));
        memcpy(dd, d, n * sizeof(TerrainDir));
        memset(dd + n, 0, (padded - n) * sizeof(TerrainDir));
    }
    float *hh = (float *)xmalloc(padded * sizeof(float));
    LandCtx c = { dd, hh, cell };
    thread_parallel_for((int)(padded / 1024), threads, land_task, &c);
    memcpy(h, hh, n * sizeof(float));
    if (dd != d) free(dd);
    free(hh);
}

/* The face's streams' beds: the stream cells taken in the flood's order (each
 * after the one it drains to), fitted upstream first, and each stream cell's
 * level in g->lv set to its bed. order is free to use as each cell's index. */
static void fit_streams(Grid *g, uint32_t *order, size_t land, int threads, FitStats *st) {
    int G = g->G;
    long off[8];
    for (int k = 0; k < 8; ++k) off[k] = (long)DJ[k] * G + DI[k];
    size_t n = 0;
    for (size_t k = 0; k < land; ++k) n += (g->inc[order[k]] & 32) != 0;
    uint32_t *cell = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t)), *up = (uint32_t *)xmalloc((n + 1) * sizeof(uint32_t));
    int32_t *parent = (int32_t *)xmalloc((n + 1) * sizeof(int32_t));
    float *h = (float *)xmalloc((n + 1) * sizeof(float)), *x = (float *)xmalloc((n + 1) * sizeof(float));
    TerrainDir *d = (TerrainDir *)xmalloc((n + 1) * sizeof(TerrainDir));
    size_t m = 0;
    for (size_t k = 0; k < land; ++k)
        if (g->inc[order[k]] & 32) cell[m++] = order[k];
    for (size_t i = 0; i < n; ++i) {   /* order now: each stream cell's index */
        order[cell[i]] = (uint32_t)i;
        d[i] = cell_dir(g, (int)(cell[i] % G), (int)(cell[i] / G));
    }
    for (size_t i = 0; i < n; ++i) {
        size_t c = cell[i];
        parent[i] = -1;
        if (g->rcv[c] < OFF) {
            size_t nx = c + off[g->rcv[c]];
            if (g->cls[nx] == LAND && (g->inc[nx] & 32)) parent[i] = (int32_t)order[nx];
        }
        up[i] = (uint32_t)(n - 1 - i);   /* downstream first, so backwards is upstream first */
    }
    land_at(d, n, h, g->cell, threads);
    tree_fit(n, parent, h, up, n, x);
    for (size_t i = 0; i < n; ++i) {
        if (in_core(g, (int)(cell[i] % G), (int)(cell[i] / G))) fit_stats(st, h[i], x[i]);
        g->lv[cell[i]] = (float)(x[i] - channel_depth(g->q[cell[i]]));
    }
    free(cell); free(up); free(parent); free(h); free(x); free(d);
}

/* --- The bake's water's beds ------------------------------------------------------------------ */

static uint64_t pos_key(TerrainDir d, double q, int dx, int dy, int dz);

typedef struct WaterBeds {
    uint32_t  V;
    float    *bed;                 /* V */
    uint32_t  nlevels, *bucket;    /* the lakes' levels in buckets inside them listing no shore */
    float    *level;
    size_t    lakes, holes, reaches, joined;
    FitStats  rivers;
} WaterBeds;


/* The bucket of face f, column i, row j, past the face's edges onto the next. */
static int64_t bucket_at(int B, int f, int i, int j) {
    if (i >= 0 && j >= 0 && i < B && j < B) return ((int64_t)f * B + j) * B + i;
    double u, v;
    int g = terrain_dir_face(terrain_face_dir(f, -1.0 + (2.0 * i + 1.0) / B, -1.0 + (2.0 * j + 1.0) / B), &u, &v);
    int a = (int)floor((u + 1.0) * 0.5 * B), b = (int)floor((v + 1.0) * 0.5 * B);
    a = a < 0 ? 0 : a >= B ? B - 1 : a;
    b = b < 0 ? 0 : b >= B ? B - 1 : b;
    return ((int64_t)g * B + b) * B + a;
}

static int water_beds(WaterBeds *wb, double cell, int threads) {
    memset(wb, 0, sizeof *wb);
    const float *xyz;
    const uint16_t *width;
    const uint8_t *kind, *flags, *inside;
    const uint32_t *start, *entry;
    uint32_t V = terrain_earth_water_points(&xyz, &width, &kind, &flags);
    int B = terrain_earth_water_buckets(&inside, &start, &entry);
    wb->V = V;
    if (!V) return 1;
    wb->bed = (float *)xmalloc(V * sizeof(float));
    TerrainDir *d = (TerrainDir *)xmalloc(V * sizeof(TerrainDir));
    float *h = (float *)xmalloc(V * sizeof(float)), *x = (float *)xmalloc(V * sizeof(float));
    for (uint32_t k = 0; k < V; ++k) {
        TerrainDir p = { xyz[3 * k], xyz[3 * k + 1], xyz[3 * k + 2] };
        d[k] = unit3(p);
        wb->bed[k] = 0.0f;
    }
    land_at(d, V, h, cell, threads);

    /* the rivers' tree: along each reach, and from its end into the reach that starts there */
    int32_t *parent = (int32_t *)xmalloc(V * sizeof(int32_t));
    uint32_t *nkids = (uint32_t *)calloc(V, sizeof(uint32_t)), *up = (uint32_t *)xmalloc(V * sizeof(uint32_t));
    size_t size = 1;
    while (size < 2 * (size_t)V) size <<= 1;
    uint32_t *head = (uint32_t *)xmalloc(size * sizeof(uint32_t)), *next = (uint32_t *)xmalloc(V * sizeof(uint32_t));
    if (!nkids) { fprintf(stderr, "earth_streams: out of memory for the beds\n"); return 0; }
    memset(head, 0xFF, size * sizeof(uint32_t));
    double jq = REACH_JOIN_M / PLANET_RADIUS;
    for (uint32_t k = 0; k < V; ++k) {
        parent[k] = -1;
        int first = k == 0 || !(flags[k - 1] & 1);
        if (kind[k] != 1 || !first) continue;
        size_t b = pos_key(d[k], jq, 0, 0, 0) & (size - 1);
        next[k] = head[b]; head[b] = k;
        wb->reaches++;
    }
    for (uint32_t k = 0, own = 0; k < V; ++k) {
        if (k == 0 || !(flags[k - 1] & 1)) own = k;   /* the reach's own start */
        if (kind[k] != 1) continue;
        if (flags[k] & 1) { parent[k] = (int32_t)(k + 1); continue; }
        double best = jq;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    for (uint32_t o = head[pos_key(d[k], jq, dx, dy, dz) & (size - 1)]; o != 0xFFFFFFFFu; o = next[o]) {
                        if (o == own) continue;
                        double dd = sqrt(dot3(sub3(d[o], d[k]), sub3(d[o], d[k])));
                        if (dd < best || (dd == best && parent[k] >= 0 && (int32_t)o < parent[k])) { best = dd; parent[k] = (int32_t)o; }
                    }
        wb->joined += parent[k] >= 0;
    }
    for (uint32_t k = 0; k < V; ++k) if (parent[k] >= 0) nkids[parent[k]]++;
    /* leaves first (Kahn's), so each point comes after all its upstream */
    size_t m = 0, q = 0;
    for (uint32_t k = 0; k < V; ++k) if (kind[k] == 1 && nkids[k] == 0) up[m++] = k;
    while (q < m) {
        uint32_t k = up[q++];
        if (parent[k] >= 0 && --nkids[parent[k]] == 0) up[m++] = (uint32_t)parent[k];
    }
    tree_fit(V, parent, h, up, m, x);
    for (size_t i = 0; i < m; ++i) {
        uint32_t k = up[i];
        double flow = (width[k] / WIDTH_K) * (width[k] / WIDTH_K);
        fit_stats(&wb->rivers, h[k], x[k]);
        wb->bed[k] = (float)(x[k] - channel_depth(flow));
    }
    for (uint32_t k = 0; k < V; ++k) if (kind[k] == 1 && nkids[k] > 0) wb->bed[k] = h[k];   /* on a loop, which the data shouldn't hold */

    /* the lakes: each outer ring (anticlockwise, seen from above with north
     * up) begins a lake, its holes (clockwise) follow it; the lowest land on
     * any of its rings is its level */
    int32_t *lake_of = (int32_t *)xmalloc(V * sizeof(int32_t));
    float *lake_level = (float *)xmalloc(V * sizeof(float));
    int32_t lakes = 0;
    for (uint32_t k = 0; k < V;) {
        uint32_t e = k;
        while (e + 1 < V && (flags[e] & 1)) ++e;
        if (kind[k] == 2) {
            /* its winding (anticlockwise seen from outside the sphere is
             * positive) and whether the water lies to the left of its longest
             * side: the water within it, an outer ring, where the two agree */
            TerrainDir c = { 0, 0, 0 };
            for (uint32_t i = k; i <= e; ++i) c = add3(c, d[i]);
            c = unit3(c);
            double area = 0.0, longest = -1.0;
            uint32_t at = k;
            for (uint32_t i = k; i < e; ++i) {
                area += dot3(c, cross3(scale3(d[i], 1.0 / dot3(d[i], c)), scale3(d[i + 1], 1.0 / dot3(d[i + 1], c))));
                double l = dot3(sub3(d[i + 1], d[i]), sub3(d[i + 1], d[i]));
                if (l > longest) { longest = l; at = i; }
            }
            TerrainDir mid = unit3(add3(d[at], d[at + 1])), left = unit3(cross3(mid, sub3(d[at + 1], d[at])));
            int wet_left = terrain_earth_shore(unit3(add3(mid, scale3(left, SIDE_M / PLANET_RADIUS)))) < 0.0;
            if ((area > 0.0) == wet_left || lakes == 0) { lake_level[lakes++] = 1e30f; wb->lakes++; }
            else wb->holes++;
            for (uint32_t i = k; i <= e; ++i) {
                lake_of[i] = lakes - 1;
                if (h[i] < lake_level[lakes - 1]) lake_level[lakes - 1] = h[i];
            }
        } else for (uint32_t i = k; i <= e; ++i) lake_of[i] = -1;
        k = e + 1;
    }
    for (uint32_t k = 0; k < V; ++k) if (kind[k] == 2) wb->bed[k] = lake_level[lake_of[k]];

    /* the buckets inside a lake that list no shore: their lake's level from
     * the buckets round them, out from those that do list one (the lake of
     * the shore nearest the bucket's centre) */
    size_t cells = 6 * (size_t)B * B;
    float *lev = (float *)xmalloc(cells * sizeof(float));
    uint8_t *state = (uint8_t *)calloc(cells, 1);   /* 1 lists a shore, 2 reached */
    uint32_t *queue = (uint32_t *)xmalloc(cells * sizeof(uint32_t));
    if (!state) { fprintf(stderr, "earth_streams: out of memory for the lakes\n"); return 0; }
    size_t qn = 0, qh = 0;
    for (size_t b = 0; b < cells; ++b) {
        int f = (int)(b / ((size_t)B * B)), j = (int)(b / B % B), i = (int)(b % B);
        TerrainDir c = terrain_face_dir(f, -1.0 + (2.0 * i + 1.0) / B, -1.0 + (2.0 * j + 1.0) / B);
        double best = 1e30;
        for (uint32_t e = start[b]; e < start[b + 1]; ++e) {
            uint32_t k = entry[e];
            if (kind[k] != 2) continue;
            TerrainDir a = { xyz[3 * k], xyz[3 * k + 1], xyz[3 * k + 2] }, z = { xyz[3 * k + 3], xyz[3 * k + 4], xyz[3 * k + 5] };
            double dd = water_arc_dist(c, a, z);
            if (dd < best) { best = dd; lev[b] = wb->bed[k]; }
        }
        if (best < 1e29) { state[b] = 1; queue[qn++] = (uint32_t)b; }
    }
    while (qh < qn) {
        size_t b = queue[qh++];
        int f = (int)(b / ((size_t)B * B)), j = (int)(b / B % B), i = (int)(b % B);
        static const int NI[4] = { 1, -1, 0, 0 }, NJ[4] = { 0, 0, 1, -1 };
        for (int s = 0; s < 4; ++s) {
            int64_t o = bucket_at(B, f, i + NI[s], j + NJ[s]);
            if (state[o] || !inside[o]) continue;
            state[o] = 2; lev[o] = lev[b];
            queue[qn++] = (uint32_t)o;
            wb->nlevels++;
        }
    }
    wb->bucket = (uint32_t *)xmalloc((wb->nlevels + 1) * sizeof(uint32_t));
    wb->level = (float *)xmalloc((wb->nlevels + 1) * sizeof(float));
    uint32_t nl = 0;
    for (size_t b = 0; b < cells; ++b)
        if (state[b] == 2) { wb->bucket[nl] = (uint32_t)b; wb->level[nl++] = lev[b]; }
    free(lev); free(state); free(queue);
    free(d); free(h); free(x); free(parent); free(nkids); free(up); free(head); free(next); free(lake_of); free(lake_level);
    return 1;
}

/* The plan's probes: the settlements' count, then each settlement's disc's
 * centre and PLAN_PROBES points round its edge, as directions in f32 and
 * plan_city_edge where the engine will read it (at the f32 direction). */
static float *plan_probes(uint32_t *count) {
    int n = plan_settlement_count();
    uint32_t np = 1 + (uint32_t)n * (1 + PLAN_PROBES);
    float *p = (float *)xmalloc((size_t)np * 4 * sizeof(float));
    TerrainChart wc = terrain_world_chart();
    p[0] = (float)n; p[1] = p[2] = p[3] = 0.0f;
    uint32_t at = 1;
    for (int k = 0; k < n; ++k) {
        PlanSettlement s = plan_settlement(k);
        for (int q = 0; q <= PLAN_PROBES; ++q) {
            double a = 2.0 * PI * q / PLAN_PROBES, r = q == 0 ? 0.0 : s.radius;
            TerrainDir d = terrain_chart_dir(&wc, s.disc.x + r * cos(a), s.disc.z + r * sin(a));
            float *o = p + 4 * at++;
            o[0] = (float)d.x; o[1] = (float)d.y; o[2] = (float)d.z;
            TerrainDir df = { o[0], o[1], o[2] };
            double x, z;
            terrain_dir_chart(&wc, unit3(df), &x, &z);
            o[3] = (float)plan_city_edge(x, z);
        }
    }
    *count = np;
    return p;
}

/* --- The seams ------------------------------------------------------------------------------
 * A link that runs into the halo ends at a cell of its own face's grid; the
 * next face's link starts at a cell of that face's, no more than a cell or
 * so away. Each such end moves to the nearest start on another face within
 * SEAM_JOIN cells, found through a hash of the starts by position. */
typedef struct StartHash { uint32_t *head, *next; size_t mask; double q; } StartHash;

static uint64_t pos_key(TerrainDir d, double q, int dx, int dy, int dz) {
    int64_t x = (int64_t)floor(d.x / q) + dx, y = (int64_t)floor(d.y / q) + dy, z = (int64_t)floor(d.z / q) + dz;
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ull ^ (uint64_t)y * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)z * 0x165667B19E3779F9ull;
    return h ^ (h >> 29);
}

static size_t join_seams(Links *L, double cell, size_t *rises) {
    StartHash h;
    size_t size = 1;
    while (size < 2 * L->nl + 2) size <<= 1;
    h.mask = size - 1;
    h.q = SEAM_JOIN * cell / PLANET_RADIUS;
    h.head = (uint32_t *)xmalloc(size * sizeof(uint32_t)); h.next = (uint32_t *)xmalloc((L->nl + 1) * sizeof(uint32_t));
    memset(h.head, 0xFF, size * sizeof(uint32_t));
    for (size_t k = 0; k < L->nl; ++k) {
        size_t b = pos_key(L->pt[L->link[k].first].d, h.q, 0, 0, 0) & h.mask;
        h.next[k] = h.head[b]; h.head[b] = (uint32_t)k;
    }
    size_t joined = 0;
    *rises = 0;
    for (size_t k = 0; k < L->nl; ++k) {
        Link *lk = &L->link[k];
        if (lk->end != END_SEAM) continue;
        Pt *e = &L->pt[lk->first + lk->count - 1];
        double best = h.q;
        long found = -1;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    for (uint32_t o = h.head[pos_key(e->d, h.q, dx, dy, dz) & h.mask]; o != 0xFFFFFFFFu; o = h.next[o]) {
                        if (L->link[o].face == lk->face) continue;
                        TerrainDir s = L->pt[L->link[o].first].d;
                        double d = sqrt(dot3(sub3(s, e->d), sub3(s, e->d)));
                        if (d < best) { best = d; found = (long)o; }
                    }
        if (found < 0) continue;
        const Pt *s = &L->pt[L->link[found].first];
        e->d = s->d;
        if (s->bed > e->bed) ++*rises;   /* the next face's start lies higher than this end: left, and counted */
        e->flow = fmax(e->flow, s->flow);
        lk->end = END_JUNCTION;
        ++joined;
    }
    free(h.head); free(h.next);
    return joined;
}

/* --- main ----------------------------------------------------------------------------------- */

typedef struct Opts { double cell, min_flow, halo, tol; int threads; } Opts;

static int derive(const char *bake, const char *out, const Runoff *runoff, const Opts *o) {
    double t0 = now_s();
    if (!terrain_earth_load(bake)) return 0;
    terrain_earth_load_streams("");   /* any old ones aside: the water here is the bake's, with no beds */
    plan_world_init(o->threads);      /* the settlements, whose ground the streams cross */
    printf("plan: %d settlements, placed in %.1f s\n", plan_settlement_count(), now_s() - t0);
    FILE *bf = fopen(bake, "rb");
    unsigned char bh[256];
    if (!bf || fread(bh, 1, 256, bf) != 256) { fprintf(stderr, "earth_streams: cannot read %s's header\n", bake); if (bf) fclose(bf); return 0; }
    fclose(bf);
    int n = (int)floor(0.5 * PI * PLANET_RADIUS / o->cell + 0.5), H = (int)ceil(o->halo / o->cell);
    int G = n + 2 * H;
    double cell = 0.5 * PI * PLANET_RADIUS / n;
    size_t cells = (size_t)G * G;
    if (cells >= 0xFFFFFFFFull) { fprintf(stderr, "earth_streams: a grid of %d a side is too large\n", G); return 0; }
    printf("grid: %d cells a face side (%.1f m), halo %d cells, %d a side with it; %.1f GB a face\n", n, cell, H, G, cells * 14.0 / 1e9);
    Grid g;
    memset(&g, 0, sizeof g);
    g.n = n; g.H = H; g.G = G; g.cell = cell; g.runoff = runoff;
    g.lv = (float *)xmalloc(cells * sizeof(float)); g.q = (float *)xmalloc(cells * sizeof(float));
    g.cls = (uint8_t *)xmalloc(cells); g.rcv = (uint8_t *)xmalloc(cells); g.inc = (uint8_t *)xmalloc(cells);
    uint32_t *order = (uint32_t *)xmalloc(cells * sizeof(uint32_t));
    Links L;
    memset(&L, 0, sizeof L);
    Scratch s;
    memset(&s, 0, sizeof s);
    FitStats fs;
    memset(&fs, 0, sizeof fs);
    double tol = o->tol / PLANET_RADIUS;
    long off[8];
    for (int k = 0; k < 8; ++k) off[k] = (long)DJ[k] * G + DI[k];
    for (int f = 0; f < 6; ++f) {
        double t1 = now_s();
        g.face = f;
        thread_parallel_for(G, o->threads, classify_task, &g);
        double t2 = now_s();
        size_t land = flood(&g, order);
        double t3 = now_s();
        for (size_t k = land; k-- > 0;) {   /* each cell after the one it drains to: summed upstream first */
            size_t c = order[k];
            if (g.rcv[c] >= OFF) continue;
            size_t nx = c + off[g.rcv[c]];
            if (g.cls[nx] == LAND) g.q[nx] += g.q[c];
        }
        mark_streams(&g, o->min_flow);
        fit_streams(&g, order, land, o->threads, &fs);
        size_t before = L.nl, pts = L.np;
        trace(&g, &L, tol, &s);
        printf("face %d: %zu land cells (%.0f%%); %zu links, %zu points; classify %.1f s, flood %.1f s, trace %.1f s\n", f, land,
               100.0 * land / cells, L.nl - before, L.np - pts, t2 - t1, t3 - t2, now_s() - t3);
        fflush(stdout);
    }
    free(g.lv); free(g.q); free(g.cls); free(g.rcv); free(g.inc); free(order);
    size_t rises;
    size_t joined = join_seams(&L, cell, &rises);

    /* the water section: each link a run of kind 3 */
    Water w;
    memset(&w, 0, sizeof w);
    w.V = (uint32_t)L.np;
    w.xyz = (float *)xmalloc(L.np * 3 * sizeof(float)); w.width = (uint16_t *)xmalloc(L.np * sizeof(uint16_t));
    w.kind = (uint8_t *)xmalloc(L.np); w.flags = (uint8_t *)xmalloc(L.np);
    float *bed = (float *)xmalloc(L.np * sizeof(float)), *flow = (float *)xmalloc(L.np * sizeof(float));
    double length = 0.0;
    for (size_t k = 0; k < L.nl; ++k) {
        const Link *lk = &L.link[k];
        for (uint32_t i = 0; i < lk->count; ++i) {
            size_t p = lk->first + i;
            const Pt *pt = &L.pt[p];
            w.xyz[3 * p] = (float)pt->d.x; w.xyz[3 * p + 1] = (float)pt->d.y; w.xyz[3 * p + 2] = (float)pt->d.z;
            double wm = floor(WIDTH_K * sqrt(pt->flow) + 0.5);
            w.width[p] = (uint16_t)(wm < 1.0 ? 1.0 : wm > 65535.0 ? 65535.0 : wm);
            w.kind[p] = 3;
            w.flags[p] = i + 1 < lk->count;
            bed[p] = (float)pt->bed; flow[p] = (float)pt->flow;
            if (i > 0) length += PLANET_RADIUS * sqrt(dot3(sub3(pt->d, L.pt[p - 1].d), sub3(pt->d, L.pt[p - 1].d)));
        }
    }
    if (!water_index(&w, BUCKETS, REACH_M, NULL, o->threads)) return 0;
    fit_print("streams' levels", &fs);

    /* the bake's water's beds, and the plan's probes */
    WaterBeds wb;
    if (!water_beds(&wb, cell, o->threads)) return 0;
    fit_print("rivers' levels", &wb.rivers);
    printf("rivers: %zu reaches, %zu running on into the next; lakes: %zu, with %zu holes; %u buckets inside a lake take its level from round them\n",
           wb.reaches, wb.joined, wb.lakes, wb.holes, wb.nlevels);
    uint32_t nplan;
    float *plan = plan_probes(&nplan);

    /* the probes: the land at the first PROBES of a spiral of directions that lie on land */
    unsigned char hd[HEADER];
    memset(hd, 0, sizeof hd);
    memcpy(hd, MAGIC, 8);
    put_u32(hd + 8, VERSION); put_u32(hd + 12, (uint32_t)n); put_f32(hd + 16, (float)cell); put_f32(hd + 20, (float)o->min_flow);
    put_f32(hd + 24, (float)WIDTH_K); put_u32(hd + 28, PROBES);
    memcpy(hd + 32, bh + 128, 64);
    put_u32(hd + 96, (uint32_t)w.B); put_f32(hd + 100, w.reach); put_u32(hd + 104, w.V); put_u32(hd + 108, w.E); put_u32(hd + 112, (uint32_t)L.nl);
    put_u32(hd + 116, nplan);
    int probes = 0;
    for (int k = 0; k < 4096 && probes < PROBES; ++k) {
        double z = 1.0 - (2.0 * k + 1.0) / 4096.0, r = sqrt(1.0 - z * z), a = k * 2.399963229728653;
        TerrainDir d = { r * cos(a), r * sin(a), z };
        float fx = (float)d.x, fy = (float)d.y, fz = (float)d.z;
        TerrainDir df = { fx, fy, fz };
        df = unit3(df);
        if (terrain_shore(df, cell) < 100.0) continue;
        double lift;
        unsigned char *p = hd + 120 + 16 * probes++;
        put_f32(p, fx); put_f32(p + 4, fy); put_f32(p + 8, fz);
        put_f32(p + 12, (float)terrain_land_parts(df, TERRAIN_WILD_EDGE, cell, &lift));
    }
    if (probes < PROBES) { fprintf(stderr, "earth_streams: only %d probes found land\n", probes); return 0; }

    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "earth_streams: cannot write %s\n", out); return 0; }
    int ok = fwrite(hd, 1, HEADER, f) == HEADER && water_write(f, &w);
    unsigned char buf[4096];
    for (int pass = 0; pass < 2 && ok; ++pass) {
        const float *v = pass ? flow : bed;
        for (size_t k = 0; k < w.V && ok; k += 1024) {
            size_t m = w.V - k < 1024 ? w.V - k : 1024;
            for (size_t i = 0; i < m; ++i) put_f32(buf + 4 * i, v[k + i]);
            ok = fwrite(buf, 4, m, f) == m;
        }
    }
    /* the plan's probes, the bake's water's beds, the lakes' buckets' levels */
    const struct { const void *v; size_t n; int u32; } tail[] = {
        { plan, (size_t)nplan * 4, 0 }, { &wb.V, 1, 1 }, { wb.bed, wb.V, 0 }, { &wb.nlevels, 1, 1 }, { wb.bucket, wb.nlevels, 1 }, { wb.level, wb.nlevels, 0 },
    };
    for (size_t t = 0; t < sizeof tail / sizeof tail[0] && ok; ++t)
        for (size_t k = 0; k < tail[t].n && ok; k += 1024) {
            size_t m = tail[t].n - k < 1024 ? tail[t].n - k : 1024;
            for (size_t i = 0; i < m; ++i) {
                if (tail[t].u32) put_u32(buf + 4 * i, ((const uint32_t *)tail[t].v)[k + i]);
                else put_f32(buf + 4 * i, ((const float *)tail[t].v)[k + i]);
            }
            ok = fwrite(buf, 4, m, f) == m;
        }
    if (fclose(f) != 0) ok = 0;
    if (!ok) { fprintf(stderr, "earth_streams: writing %s failed\n", out); return 0; }
    size_t used, most, wet;
    double mb;
    water_stats(&w, &used, &most, &wet, &mb);
    printf("streams: %zu links, %u points, %.0f km; ends:", L.nl, w.V, length / 1000.0);
    for (int e = 0; e < END_KINDS; ++e) printf(" %zu %s%s", L.ends[e], END_NAME[e], e + 1 < END_KINDS ? "," : "");
    printf("; %zu face-seam ends joined (%zu to a higher start), %zu left\n", joined, rises, L.ends[END_SEAM] - joined);
    printf("bed against the land at the streams' %zu cells: cut %.1f m on average (at most %.0f), filled %.1f m (at most %.0f); %zu cells (%.1f%%) more than 2 m under it\n",
           L.cells, L.carve_sum / fmax(1.0, (double)L.cells), L.carve_max, L.fill_sum / fmax(1.0, (double)L.cells), L.fill_max, L.ponded,
           100.0 * L.ponded / fmax(1.0, (double)L.cells));
    printf("wrote %s: %u bucket entries, %zu buckets list streams (most %zu); %.1f MB; %.1f s\n", out, w.E, used, most,
           (HEADER + mb * 1e6 + 8.0 * w.V) / 1e6, now_s() - t0);
    return 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: earth_streams <bake> <out.streams> [--stage <stage.r16> | --runoff-const L]\n"
        "                     [--cell M] [--min-flow Q] [--halo M] [--tolerance M] [--threads T]\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }
    Opts o = { 500.0, 0.5, 30000.0, 60.0, cpu_count() };
    Runoff r;
    memset(&r, 0, sizeof r);
    r.constant = -1.0;
    const char *stage = NULL;
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { usage(); return 1; }
        if (!strcmp(argv[i], "--stage")) stage = argv[i + 1];
        else if (!strcmp(argv[i], "--runoff-const")) r.constant = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--cell")) o.cell = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--min-flow")) o.min_flow = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--halo")) o.halo = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--tolerance")) o.tol = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) o.threads = atoi(argv[i + 1]);
        else { usage(); return 1; }
    }
    if ((stage == NULL) == (r.constant < 0.0)) { fprintf(stderr, "earth_streams: give the runoff by --stage or --runoff-const, one of them\n"); return 1; }
    if (stage && !runoff_read(&r, stage)) return 1;
    if (o.cell < 50.0 || o.min_flow <= 0.0 || o.halo < 0.0 || o.tol <= 0.0) { usage(); return 1; }
    if (o.threads < 1) o.threads = 1;
    return derive(argv[1], argv[2], &r, &o) ? 0 : 1;
}
