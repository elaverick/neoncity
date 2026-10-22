#include "plan_internal.h"
#include "terrain.h"
#include "hash.h"
#include "noise.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The world's settlements (see plan.h): where the sea will be and which land
 * is one continent, the cities placed on the land, and an index so the edge
 * measure and the settlement near a point are cheap to find.
 *
 * The sea: terrain_sea_margin on a grid of PLAN_SEA_CELL over the world's
 * reach, interpolated between its nodes; land is where it is above 0, and
 * the nodes a road may reach (PLAN_ROAD_SEA_M above it) joined along the
 * grid's lines are one continent.
 *
 * Placement. Candidate sites are the jittered centres of a CAND_CELL grid over
 * the world's reach. Each is scored for a city (site_score): level land (the
 * region's plains; less for hills and plateaus, against mountains), no
 * canyon, and the lowlands near the coast to come, where cities gather, with
 * a little jitter so the best sites don't line up along the fields. Sites are
 * taken best first: the colossal cities, centres at least SEP_COLOSSAL apart,
 * then the megacities, the best sites the largest. Any two footprints keep
 * SEP_EDGE between them, and each must clear the sea, canyons and mountains,
 * stand on ground the Earth tilts no more than FOOT_GRADE and keep out of the
 * Earth's no-settlement mask (footprint_ok). With a bake the land, the sea and
 * the landforms are the Earth's (terrain_sea_margin, terrain_landform), so the
 * cities gather on its real lowlands. Should the land run out at those spacings they shrink by
 * SEP_RELAX and the pass goes on. A centre is moved to the centre of its town,
 * so its core is a town as the megacity's is (plan_town_regional).
 *
 * Determinism: scores are quantised before they are compared, so the game
 * (built with fast floating point) and the tests place the same cities unless
 * a score falls within rounding of a quantum's edge.
 *
 * Until plan_world_init has placed them, the edge measure and everything
 * else here knows only the megacity: placement reads the land as the megacity
 * alone leaves it (terrain_landform). */

#define PI 3.14159265358979323846

#define SEA_EXTENT    (PLAN_WORLD_REACH + 600000.0)   /* the sea grid covers |x|, |z| up to this (m) */
#define SEA_SIDE      ((int)(2.0 * SEA_EXTENT / PLAN_SEA_CELL) + 2)
#define CAND_CELL     25000.0     /* a candidate site per square this size (m) */
#define SEA_SETTLE_M  100.0       /* a settlement's centre stands at least this above the sea to come (m)... */
#define SEA_FOOT_M    40.0        /* ...and the ring round its footprint this */
#define FOOT_PAD      2000.0      /* the ring's distance beyond the footprint's radius (m) */
#define FOOT_MOUNTAIN 0.35f       /* the most mountain weight a footprint may hold */
#define FOOT_GRADE    0.02        /* the most the Earth may rise or fall from a footprint's centre to its ring, a metre out */
#define NOSETTLE_MAX  0.5         /* the hints' no-settlement mask keeps settlements off above this */
#define SEP_COLOSSAL  450000.0    /* colossal cities' centres at least this far apart (m) */
#define SEP_EDGE      90000.0     /* any two footprints at least this far apart (m) */
#define SEP_RELAX     0.85
#define SCORE_QUANTUM 4096.0
/* The index: settlements by INDEX_CELL squares of the chart, each listed in
 * every square its reach (settlement_reach) touches. */
#define INDEX_CELL    100000.0
#define INDEX_EXTENT  (PLAN_WORLD_REACH + 100000.0)
#define INDEX_SIDE    ((int)(2.0 * INDEX_EXTENT / INDEX_CELL) + 1)
/* The warp moves a point by at most this (m) for a settlement of CITY_RADIUS. */
#define WARP_MAX      (1.4142135623730951 * (CITY_WARP1_M + CITY_WARP2_M))

enum { WORLD_EMPTY = 0, WORLD_PLACING, WORLD_SETTLED, WORLD_READY };

typedef struct World {
    int            state;
    PlanSettlement s[PLAN_SETTLEMENTS];
    double         relief_lo, relief_hi;
    float         *sea;          /* SEA_SIDE^2 nodes, row by row: node (i, j) at -SEA_EXTENT + (i, j) * PLAN_SEA_CELL */
    int           *land;         /* each node's continent, 0 for the sea */
    int            continents;
    int           *cell_start;   /* the index: INDEX_SIDE^2 + 1 offsets into cell_ids */
    int           *cell_ids;
} World;

static World W;

static float vnoise(double x, double z, uint32_t salt) { return noise_value2(WORLD_SEED ^ salt, x, z); }

static void ensure(void) {
    if (W.state == WORLD_EMPTY) plan_world_init(1);
}

/* --- Footprints -------------------------------------------------------------------- */

/* A footprint is a disc of the settlement's radius in a frame warped by two
 * octaves of value noise, the warp scaled with the radius (so its edge measure
 * changes no faster than the megacity's, CITY_EDGE_RATE). At CITY_RADIUS the
 * formula is the megacity's own, operation for operation. */
static double footprint_edge(DVec2 c, double radius, double x, double z) {
    double k = radius / CITY_RADIUS;
    double wx = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED61u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED63u) - 1.0);
    double wz = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED62u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED64u) - 1.0);
    if (k != 1.0) { wx *= k; wz *= k; }
    return hypot(x - c.x + wx, z - c.z + wz) - radius;
}

/* How far from its centre a settlement's edge measure can be under PLAN_EDGE_CAP. */
static double settlement_reach(const PlanSettlement *s) {
    return s->radius + WARP_MAX * (s->radius / CITY_RADIUS) + PLAN_EDGE_CAP + 10.0;
}

double plan_home_edge(double x, double z) { return footprint_edge(plan_town_centre(0, 0), CITY_RADIUS, x, z); }

/* The index's square holding (x, z), or -1 beyond it. */
static int index_cell(double x, double z) {
    double fx = (x + INDEX_EXTENT) / INDEX_CELL, fz = (z + INDEX_EXTENT) / INDEX_CELL;
    if (!(fx >= 0.0 && fz >= 0.0 && fx < INDEX_SIDE && fz < INDEX_SIDE)) return -1;
    return (int)fz * INDEX_SIDE + (int)fx;
}

double plan_city_edges(double x, double z, double *home) {
    ensure();
    double h = plan_home_edge(x, z), e = fmin(h, PLAN_EDGE_CAP);
    if (home) *home = h;
    if (W.state < WORLD_SETTLED) return e;
    int c = index_cell(x, z);
    if (c < 0) return e;
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        const PlanSettlement *s = &W.s[W.cell_ids[k]];
        double d = hypot(x - s->centre.x, z - s->centre.z) - s->radius - WARP_MAX * (s->radius / CITY_RADIUS);
        if (d < e) e = fmin(e, footprint_edge(s->centre, s->radius, x, z));
    }
    return e;
}

double plan_city_edge(double x, double z) { return plan_city_edges(x, z, NULL); }

/* --- Settlements --------------------------------------------------------------------- */

double plan_settlement_relief(double radius) {
    return fmin(PLAN_RELIEF_MAX_M, PLAN_RELIEF_MEGA_M * sqrt(CITY_RADIUS / radius));
}

double plan_settlement_radius(double km) { return CITY_RADIUS - 500.0 * (20.0 - km); }

PlanSettlement plan_settlement(int k) {
    ensure();
    return W.s[k >= 0 && k < PLAN_SETTLEMENTS ? k : 0];
}

void plan_settlement_relief_range(double *lo, double *hi) {
    ensure();
    *lo = W.relief_lo;
    *hi = W.relief_hi;
}

/* plan_settlement_near answers from the index's square, or the megacity
 * beyond the index: so only the megacity and the settlements listed in the
 * squares the rectangle touches can be its answer. */
void plan_settlement_relief_in(double x0, double z0, double x1, double z1, double *lo, double *hi) {
    ensure();
    *lo = *hi = W.s[0].relief_m;
    if (W.state < WORLD_SETTLED) return;
    double i0 = floor((x0 + INDEX_EXTENT) / INDEX_CELL), i1 = floor((x1 + INDEX_EXTENT) / INDEX_CELL);
    double j0 = floor((z0 + INDEX_EXTENT) / INDEX_CELL), j1 = floor((z1 + INDEX_EXTENT) / INDEX_CELL);
    if (i1 < 0.0 || j1 < 0.0 || i0 >= INDEX_SIDE || j0 >= INDEX_SIDE) return;
    if ((i1 - i0 + 1.0) * (j1 - j0 + 1.0) > 64.0) { *lo = W.relief_lo; *hi = W.relief_hi; return; }
    for (int j = (int)fmax(j0, 0.0); j <= (int)fmin(j1, INDEX_SIDE - 1.0); ++j)
        for (int i = (int)fmax(i0, 0.0); i <= (int)fmin(i1, INDEX_SIDE - 1.0); ++i) {
            int c = j * INDEX_SIDE + i;
            for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
                *lo = fmin(*lo, W.s[W.cell_ids[k]].relief_m);
                *hi = fmax(*hi, W.s[W.cell_ids[k]].relief_m);
            }
        }
}

int plan_settlement_near(double x, double z) {
    ensure();
    if (W.state < WORLD_SETTLED) return 0;
    int c = index_cell(x, z), best = 0;
    if (c < 0) return 0;
    double bd = PLAN_EDGE_CAP;
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        const PlanSettlement *s = &W.s[W.cell_ids[k]];
        double d = hypot(x - s->centre.x, z - s->centre.z) - s->radius;
        if (d < bd) { bd = d; best = W.cell_ids[k]; }
    }
    return best;
}

int plan_world_core_town(int i, int j) {
    if (W.state < WORLD_SETTLED) return 0;
    int c = index_cell((i + 0.5) * TOWN_SIZE, (j + 0.5) * TOWN_SIZE);
    if (c < 0) return 0;
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        const PlanSettlement *s = &W.s[W.cell_ids[k]];
        if (s->town[0] == i && s->town[1] == j) return 1;
    }
    return 0;
}

/* --- The sea to come, and the continents ---------------------------------------------- */

static double sea_node_x(int i) { return -SEA_EXTENT + i * PLAN_SEA_CELL; }

static void sea_row(void *ctx, int j) {
    (void)ctx;
    for (int i = 0; i < SEA_SIDE; ++i) {
        double x = sea_node_x(i), z = sea_node_x(j);
        W.sea[(size_t)j * SEA_SIDE + i] = hypot(x, z) > SEA_EXTENT ? -1e9f : (float)terrain_sea_margin(x, z);
    }
}

double plan_world_sea(double x, double z) {
    ensure();
    double fx = (x + SEA_EXTENT) / PLAN_SEA_CELL, fz = (z + SEA_EXTENT) / PLAN_SEA_CELL;
    if (!(fx >= 0.0 && fz >= 0.0 && fx < SEA_SIDE - 1 && fz < SEA_SIDE - 1)) return -1e9;
    int i = (int)fx, j = (int)fz;
    double tx = fx - i, tz = fz - j;
    const float *r0 = &W.sea[(size_t)j * SEA_SIDE + i], *r1 = r0 + SEA_SIDE;
    return (1.0 - tz) * ((1.0 - tx) * r0[0] + tx * r0[1]) + tz * ((1.0 - tx) * r1[0] + tx * r1[1]);
}

int plan_world_continent(double x, double z) {
    ensure();
    if (plan_world_sea(x, z) < PLAN_ROAD_SEA_M) return 0;
    int i = (int)floor((x + SEA_EXTENT) / PLAN_SEA_CELL), j = (int)floor((z + SEA_EXTENT) / PLAN_SEA_CELL);
    /* the nearest of the four nodes round it that is land */
    int best = 0;
    double bd = 1e300;
    for (int dj = 0; dj <= 1; ++dj)
        for (int di = 0; di <= 1; ++di) {
            int n = W.land[(size_t)(j + dj) * SEA_SIDE + (i + di)];
            double d = hypot(x - sea_node_x(i + di), z - sea_node_x(j + dj));
            if (n && d < bd) { bd = d; best = n; }
        }
    return best;
}

/* Land nodes joined along the grid's lines, numbered in the order a scan of
 * the rows first meets them. */
static void label_continents(void) {
    const int n = SEA_SIDE;
    int *stack = (int *)malloc(sizeof(int) * (size_t)n * n);
    if (!stack) fatal("out of memory labelling the continents");
    W.continents = 0;
    for (int start = 0; start < n * n; ++start) {
        if (W.land[start] || W.sea[start] < PLAN_ROAD_SEA_M) continue;
        int label = ++W.continents, top = 0;
        W.land[start] = label;
        stack[top++] = start;
        while (top) {
            int c = stack[--top], ci = c % n, cj = c / n;
            const int nb[4][2] = { { ci - 1, cj }, { ci + 1, cj }, { ci, cj - 1 }, { ci, cj + 1 } };
            for (int k = 0; k < 4; ++k) {
                if (nb[k][0] < 0 || nb[k][1] < 0 || nb[k][0] >= n || nb[k][1] >= n) continue;
                int q = nb[k][1] * n + nb[k][0];
                if (W.land[q] || W.sea[q] < PLAN_ROAD_SEA_M) continue;
                W.land[q] = label;
                stack[top++] = q;
            }
        }
    }
    free(stack);
}

/* --- Placement ---------------------------------------------------------------------- */

enum { CAND_FREE = 0, CAND_PLACED, CAND_FAILED };   /* FAILED: its footprint isn't clear, at this pass's radius */
typedef struct Cand { double x, z; int64_t key; int state; } Cand;

typedef struct CandRows {
    int   side;
    Cand *rows;     /* side candidates a row, at most */
    int  *count;    /* how many each row holds */
} CandRows;

/* Whether the Earth's hints keep settlements off (x, z) (never without a bake). */
static int nosettle(double x, double z) {
    TerrainEarthLand el;
    TerrainChart wc = terrain_world_chart();
    return terrain_earth_land(terrain_chart_dir(&wc, x, z), &el) && el.nosettle > NOSETTLE_MAX;
}

static double smooth01(double t) { t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t; return t * t * (3.0 - 2.0 * t); }

/* A site's worth for a city: level land, lowlands by the coast to come. */
static double site_score(const TerrainLandform *lf, double sea, uint32_t h) {
    double flat = lf->w[TERRAIN_PLAINS] + 0.55 * lf->w[TERRAIN_HILLS] + 0.3 * lf->w[TERRAIN_PLATEAU] - 1.2 * lf->w[TERRAIN_MOUNTAINS];
    double coast = 1.0 - smooth01((sea - SEA_SETTLE_M) / 1500.0);
    return flat + 0.25 * coast + 0.2 * hash_float(h);
}

static void cand_row(void *ctx, int j) {
    CandRows *cr = (CandRows *)ctx;
    Cand *row = &cr->rows[(size_t)j * cr->side];
    int n = 0;
    for (int i = 0; i < cr->side; ++i) {
        uint32_t h = hash2i(WORLD_SEED ^ 0x5E7Au, i, j);
        double x = -PLAN_WORLD_REACH + (i + 0.2 + 0.6 * hash_float(hash_combine(h, 1u))) * CAND_CELL;
        double z = -PLAN_WORLD_REACH + (j + 0.2 + 0.6 * hash_float(hash_combine(h, 2u))) * CAND_CELL;
        if (hypot(x, z) > PLAN_WORLD_REACH - CITY_RADIUS) continue;
        double sea = plan_world_sea(x, z);
        if (sea < SEA_SETTLE_M || nosettle(x, z)) continue;
        TerrainLandform lf;
        terrain_landform(x, z, &lf);
        if (lf.canyon > 0.0f || lf.w[TERRAIN_MOUNTAINS] > FOOT_MOUNTAIN) continue;
        Cand *c = &row[n++];
        c->x = x; c->z = z; c->state = CAND_FREE;
        c->key = (int64_t)floor(site_score(&lf, sea, hash_combine(h, 3u)) * SCORE_QUANTUM + 0.5);
    }
    cr->count[j] = n;
}

static int cand_cmp(const void *pa, const void *pb) {
    const Cand *a = (const Cand *)pa, *b = (const Cand *)pb;
    if (a->key != b->key) return a->key > b->key ? -1 : 1;
    return 0;   /* qsort isn't stable: the candidates' order is broken by position below */
}

/* Best first; equal scores in the order of the rows, then along them. */
static int cand_order(const void *pa, const void *pb) {
    int c = cand_cmp(pa, pb);
    if (c) return c;
    const Cand *a = (const Cand *)pa, *b = (const Cand *)pb;
    if (a->z != b->z) return a->z < b->z ? -1 : 1;
    return a->x < b->x ? -1 : a->x > b->x ? 1 : 0;
}

/* Whether a footprint of radius r round (x, z) is clear: the ring FOOT_PAD
 * beyond it well above the sea and no steeper from the centre than
 * FOOT_GRADE (the sea's grid is the Earth as coarsely as the land under a
 * settlement reads it, TERRAIN_LIFT_SPACING), and neither it, a ring halfway
 * in nor its centre on a canyon, in the mountains or in the hints' mask. */
static int footprint_ok(double x, double z, double r) {
    double sea = plan_world_sea(x, z);
    if (sea < SEA_SETTLE_M) return 0;
    for (int k = 0; k <= 24; ++k) {   /* 16 round the ring, 8 halfway in, the centre */
        int ring = k < 16;
        double a = ring ? 2.0 * PI * k / 16.0 : 2.0 * PI * (k - 16 + 0.5) / 8.0;
        double rr = ring ? r + FOOT_PAD : k < 24 ? 0.5 * r : 0.0;
        double px = x + rr * cos(a), pz = z + rr * sin(a);
        if (ring) {
            double s = plan_world_sea(px, pz);
            if (s < SEA_FOOT_M || fabs(s - sea) > FOOT_GRADE * rr) return 0;
        }
        if (nosettle(px, pz)) return 0;
        TerrainLandform lf;
        terrain_landform(px, pz, &lf);
        if (lf.canyon > 0.0f || lf.w[TERRAIN_MOUNTAINS] > FOOT_MOUNTAIN) return 0;
    }
    return 1;
}

/* Whether a footprint of radius r at c keeps its distance from those placed
 * (and, for a colossal city, sep from the colossal ones). */
static int spaced(const PlanSettlement *s, int n, DVec2 c, double r, int colossal, double sepEdge, double sepColossal) {
    for (int k = 0; k < n; ++k) {
        double d = dlen(dsub(s[k].centre, c));
        if (d - s[k].radius - r < sepEdge) return 0;
        if (colossal && s[k].colossal && d < sepColossal) return 0;
    }
    return 1;
}

static PlanSettlement make_settlement(double x, double z, double km, int colossal) {
    PlanSettlement s;
    memset(&s, 0, sizeof(s));
    plan_town_at(x, z, &s.town[0], &s.town[1]);
    s.centre = plan_town_centre(s.town[0], s.town[1]);
    s.size_km = km;
    s.radius = plan_settlement_radius(km);
    s.relief_m = plan_settlement_relief(s.radius);
    s.colossal = colossal;
    return s;
}

static void place_settlements(int threads) {
    CandRows cr;
    cr.side = (int)ceil(2.0 * PLAN_WORLD_REACH / CAND_CELL);
    cr.rows = (Cand *)malloc(sizeof(Cand) * (size_t)cr.side * cr.side);
    cr.count = (int *)calloc((size_t)cr.side, sizeof(int));
    if (!cr.rows || !cr.count) fatal("out of memory placing the settlements");
    thread_parallel_for(cr.side, threads, cand_row, &cr);
    int nc = 0;
    for (int j = 0; j < cr.side; ++j) {
        memmove(&cr.rows[nc], &cr.rows[(size_t)j * cr.side], sizeof(Cand) * (size_t)cr.count[j]);
        nc += cr.count[j];
    }
    Cand *cand = cr.rows;
    qsort(cand, (size_t)nc, sizeof(Cand), cand_order);

    PlanSettlement *s = W.s;
    int n = 1;
    double sepEdge = SEP_EDGE, sepColossal = SEP_COLOSSAL;
    /* The colossal cities, then the megacities, best sites first. */
    for (int pass = 0; pass < 2; ++pass) {
        int want = pass == 0 ? PLAN_COLOSSAL : PLAN_SETTLEMENTS;
        double r = pass == 0 ? CITY_RADIUS : plan_settlement_radius(PLAN_MEGA_MAX_KM);
        for (int k = 0; k < nc; ++k)
            if (cand[k].state == CAND_FAILED) cand[k].state = CAND_FREE;   /* a smaller footprint may clear */
        for (int tries = 0; n < want && tries < 12; ++tries) {
            for (int k = 0; k < nc && n < want; ++k) {
                Cand *c = &cand[k];
                if (c->state != CAND_FREE) continue;
                /* at its town's centre, where it will stand */
                PlanSettlement t = make_settlement(c->x, c->z, pass == 0 ? 20.0 : PLAN_MEGA_MAX_KM, pass == 0);
                if (!spaced(s, n, t.centre, r, pass == 0, sepEdge, sepColossal)) continue;
                if (!footprint_ok(t.centre.x, t.centre.z, r)) { c->state = CAND_FAILED; continue; }
                c->state = CAND_PLACED;
                s[n++] = t;
            }
            if (n < want) { sepEdge *= SEP_RELAX; sepColossal *= SEP_RELAX; }
        }
        if (n < want) {
            fprintf(stderr, "plan: only %d of %d settlements found room on the land\n", n, want);
            fatal("the land is too small for the settlements");
        }
    }
    /* The megacities' sizes, the best sites the largest. */
    const int nm = PLAN_SETTLEMENTS - PLAN_COLOSSAL;
    for (int k = 0; k < nm; ++k) {
        PlanSettlement *m = &s[PLAN_COLOSSAL + k];
        double t = nm > 1 ? (double)k / (nm - 1) : 0.0;
        double jitter = 0.6 * (hash_float(hash2i(WORLD_SEED ^ 0x5E7Bu, m->town[0], m->town[1])) - 0.5);
        double km = PLAN_MEGA_MAX_KM - (PLAN_MEGA_MAX_KM - PLAN_MEGA_MIN_KM) * pow(t, 0.8) + jitter;
        km = km < PLAN_MEGA_MIN_KM ? PLAN_MEGA_MIN_KM : km > PLAN_MEGA_MAX_KM ? PLAN_MEGA_MAX_KM : km;
        *m = make_settlement(m->centre.x, m->centre.z, km, 0);
    }
    free(cr.rows);
    free(cr.count);
}

static void build_index(void) {
    const int cells = INDEX_SIDE * INDEX_SIDE;
    W.cell_start = (int *)calloc((size_t)cells + 1, sizeof(int));
    if (!W.cell_start) fatal("out of memory indexing the settlements");
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = 1; k < PLAN_SETTLEMENTS; ++k) {
            const PlanSettlement *s = &W.s[k];
            double r = settlement_reach(s);
            int i0 = (int)floor((s->centre.x - r + INDEX_EXTENT) / INDEX_CELL), i1 = (int)floor((s->centre.x + r + INDEX_EXTENT) / INDEX_CELL);
            int j0 = (int)floor((s->centre.z - r + INDEX_EXTENT) / INDEX_CELL), j1 = (int)floor((s->centre.z + r + INDEX_EXTENT) / INDEX_CELL);
            for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < INDEX_SIDE; ++j)
                for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < INDEX_SIDE; ++i) {
                    int c = j * INDEX_SIDE + i;
                    if (pass == 0) W.cell_start[c + 1]++;
                    else W.cell_ids[W.cell_start[c]++] = k;
                }
        }
        if (pass == 0) {
            for (int c = 0; c < cells; ++c) W.cell_start[c + 1] += W.cell_start[c];
            W.cell_ids = (int *)malloc(sizeof(int) * (size_t)(W.cell_start[cells] + 1));
            if (!W.cell_ids) fatal("out of memory indexing the settlements");
        }
    }
    /* the second pass moved each start to its cell's end: put them back */
    for (int c = cells; c > 0; --c) W.cell_start[c] = W.cell_start[c - 1];
    W.cell_start[0] = 0;
}

void plan_world_init(int threads) {
    if (W.state != WORLD_EMPTY) return;
    W.state = WORLD_PLACING;
    if (threads < 1) threads = 1;
    /* the megacity, first, whatever else follows */
    W.s[0] = make_settlement(plan_town_centre(0, 0).x, plan_town_centre(0, 0).z, 20.0, 1);
    W.relief_lo = W.relief_hi = W.s[0].relief_m;

    W.sea = (float *)malloc(sizeof(float) * (size_t)SEA_SIDE * SEA_SIDE);
    W.land = (int *)calloc((size_t)SEA_SIDE * SEA_SIDE, sizeof(int));
    if (!W.sea || !W.land) fatal("out of memory mapping the sea");
    thread_parallel_for(SEA_SIDE, threads, sea_row, NULL);
    label_continents();

    place_settlements(threads);
    for (int k = 0; k < PLAN_SETTLEMENTS; ++k) {
        W.s[k].continent = plan_world_continent(W.s[k].centre.x, W.s[k].centre.z);
        W.relief_lo = fmin(W.relief_lo, W.s[k].relief_m);
        W.relief_hi = fmax(W.relief_hi, W.s[k].relief_m);
    }
    build_index();
    W.state = WORLD_SETTLED;
    plan_roads_build(threads);
    W.state = WORLD_READY;
}
