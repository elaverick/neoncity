#include "plan_internal.h"
#include "terrain.h"
#include "assets.h"
#include "hash.h"
#include "noise.h"
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The world's settlements (see plan.h): the list of cities read and merged,
 * each footprint cut at its coast and its mountains and pushed away from
 * them, the sea to come on the roads' grid and which land is one continent,
 * and an index so the edge measure and the settlement near a point are cheap
 * to find.
 *
 * The list: PLAN_CITY_LIST under one of the data roots (data_roots), a
 * header and then rank,name,latitude,longitude,ghsl_id,ghsl_name,area_km2,...
 * a row, the name in double quotes where it holds a comma; a row without a
 * GHSL centre has the last empty (and counts as the list's smallest area).
 * Rows within HOME_KM of the world chart's origin are the megacity (Los
 * Angeles, on the Earth's bake). Cities whose footprints would come closer
 * than the spacing below are one, chained, until none would: a merged city
 * has the summed area of its rows' urban centres (a centre two rows share
 * counted once), its size from that, and stands at its rows' centre weighted
 * by their areas (the megacity at the origin).
 *
 * The cut. Every settlement has a cut grid in its own chart (the megacity's
 * about its core in the world chart): nodes CUT_CELL apart over the reach of
 * its edge measure, each the larger of three signed distances, interpolated
 * between them: to the shore (terrain_shore at mip 0; positive over the sea)
 * plus PLAN_COAST_M, to a lake's shore (terrain_earth_shore, exact within
 * LAKE_KNOWN_M of a lake and carried on beyond by a chamfer distance;
 * positive in the lake) plus PLAN_LAKE_M, and into ground steeper than
 * PLAN_STEEP_GRADE (the Earth's grade at mip 0 between the nodes either side)
 * less PLAN_STEEP_REACH_M. The footprint's edge is the larger of the warped
 * disc's and the grid's, so the footprint stops PLAN_COAST_M short of the
 * sea, PLAN_LAKE_M short of a lake and PLAN_STEEP_REACH_M into the
 * mountains; the grid's bilinear distance changes by at most sqrt 2 a metre,
 * well inside CITY_EDGE_RATE. Where the grid finds none of them it is left
 * out. Between the nodes the grid can miss a lake's narrow arm, so near a
 * lake (lake_exact) the edge is also at least PLAN_LAKE_M less the exact
 * distance to its shore, falling away faster past LAKE_EXACT_FROM so that it
 * is under the grid's term before LAKE_KNOWN_M, where it is no longer read. A footprint the cut leaves short of its area by
 * more than AREA_SLACK is pushed: its centre steps away from the part cut
 * off, by a PUSH_STEPS-th of its radius at a time (PUSH_STEP_MIN at least)
 * up to its radius (in PUSH_DIRS directions round that), to the nearest
 * place where it keeps its area, or to where it keeps the most; its centre
 * must stay CENTRE_INLAND inside the cut. One that finds no place for its
 * centre so near (on a cape, a spit or a delta) looks on to PUSH_FAR, all
 * round. Then its radius grows, up to GROW_MAX, until it has its area. A
 * city whose centre finds no such land is left out (polders below the sea;
 * the stand-in's seas, without a bake). Areas are counted in the grid's
 * nodes, every area_stride-th along each way. The megacity is never left out
 * and its core never moves: its disc alone is pushed, no further than keeps
 * the core HOME_CORE inside its edge.
 *
 * Spacing. Two footprints never meet: between centres there is at least the
 * sum of their radii, twice the larger warp and SEP_GAP, so the settlement
 * nearest a point (plan_settlement_near) changes only where both edge
 * measures are positive. Where two would come closer after the cut, the
 * lower-ranked one shrinks (the megacity never).
 *
 * The city's centre is moved to the centre of its town in the world chart,
 * so its core is a town as the megacity's is (plan_town_regional); its chart
 * is about that place.
 *
 * The sea to come: terrain_sea_margin on a grid over the sphere, its nodes
 * no more than PLAN_SEA_CELL apart (plan_cube_*), interpolated between them.
 * A road may go where the grid's margin is PLAN_ROAD_SEA_M or more; where it
 * is less, but not out in the ocean, the Earth's shore is read finely
 * (plan_road_dry), so the coastal plains, which the grid's 20 km mix with
 * the sea beside them, are land. The nodes a road may reach, joined along
 * the grid's lines (across the cube's edges too), are one continent.
 *
 * Determinism: the list is read in order and everything after it is
 * sequenced (the cuts on threads, each its own); counts of nodes are
 * compared, not areas, so the game (built with fast floating point) and the
 * tests place the same cities unless a node falls within rounding of an edge.
 *
 * Until plan_world_init has placed them, the edge measure and everything
 * else here knows only the megacity. */

#define PI 3.14159265358979323846

#define HOME_KM       30.0        /* a row of the list this near the world chart's origin is the megacity (km) */
#define CUT_CELL      1000.0      /* the cut grid's spacing (m) */
#define STEEP_SPACING 1000.0      /* the Earth's grade read at this spacing: mip 0 */
#define LAKE_KNOWN_M  1500.0      /* the lakes' shores are read exactly this near them (under the bake's reach, 2 km) */
#define LAKE_EXACT_FROM 500.0     /* the exact lake term falls away an extra... */
#define LAKE_EXACT_FALL 1.0       /* ...this per metre past here: under the grid's (bilinear, chamfered: within 0.71 CUT_CELL and 8.3%) by LAKE_KNOWN_M */
#define LAKE_NEAR_NODE  3200.0    /* a lake may be within LAKE_KNOWN_M of a point only if a corner node's chamfer distance is under this */
#define AREA_SLACK    0.03        /* a footprint may lose this share of its area to the cut */
#define AREA_NODES    80          /* areas are counted on about this many nodes across a footprint, at most */
#define PUSH_STEPS    40          /* a push steps out by this fraction of the radius... */
#define PUSH_STEP_MIN 500.0       /* ...or this, at least (m) */
#define PUSH_DIRS     5           /* inland and 25 and 50 degrees either side */
#define PUSH_FAR      40000.0     /* how far a city with nowhere nearer may be pushed (m) */
#define GROW_MAX      2.0
#define CENTRE_INLAND (CITY_BELT + 2.0 * TOWN_SIZE)   /* the centre stands this far inside the cut, and its town's (m) */
#define HOME_CORE     (CITY_BELT + CITY_FRINGE + 2.0 * TOWN_SIZE)   /* the megacity's core stays this far inside its edge (m) */
#define SEP_GAP       1000.0      /* m */
#define SHRINK_MIN    3000.0      /* a settlement shrunk smaller than this radius is left out (m) */
#define CONTINENT_NEAR 2          /* a settlement whose centre the sea grid takes for shore joins the land this many nodes off */
/* The index: settlements by INDEX_CELL squares of the world chart, each
 * listed in every square its reach (settlement_reach) touches there. It
 * covers the whole chart. */
#define INDEX_CELL    100000.0
#define INDEX_EXTENT  (PI * PLANET_RADIUS + INDEX_CELL)
#define INDEX_SIDE    ((int)(2.0 * INDEX_EXTENT / INDEX_CELL) + 1)
/* The warp moves a point by at most this (m) for a settlement of CITY_RADIUS. */
#define WARP_MAX      (1.4142135623730951 * (CITY_WARP1_M + CITY_WARP2_M))

enum { WORLD_EMPTY = 0, WORLD_PLACING, WORLD_SETTLED, WORLD_READY };

typedef struct Cut {
    int    n;      /* nodes a side, 0 for none */
    double half;   /* the nodes span [-half, half]^2 about the settlement's centre in its chart (m) */
    float *d;      /* n * n, row by row: the largest of the signed distances to the coast, the lakes and into the mountains (m) */
    float *lake;   /* n * n: each node's distance to the lakes (m; negative in one), or NULL with none near */
} Cut;

static void cut_free(Cut *c) { free(c->d); free(c->lake); c->d = c->lake = NULL; }

typedef struct World {
    int            state;
    int            n;
    PlanSettlement s[PLAN_MAX_SETTLEMENTS];
    char           name[PLAN_MAX_SETTLEMENTS][64];
    Cut            cut[PLAN_MAX_SETTLEMENTS];
    TerrainChart   wc;           /* the world chart */
    double         relief_lo, relief_hi;
    int            sea_n;        /* the sea grid: nodes a face's side (plan_cube_*), PLAN_SEA_CELL apart at most */
    float         *sea;          /* its nodes' terrain_sea_margin, by plan_cube_index */
    uint8_t       *dry;          /* each node's plan_road_dry */
    uint8_t       *join;         /* each node's joins along the grid (JOIN_I to the next node along i, JOIN_J along j) */
    int           *land;         /* each node's continent, 0 for the sea */
    int            continents;
    int           *cell_start;   /* the index: INDEX_SIDE^2 + 1 offsets into cell_ids */
    int           *cell_ids;
} World;

static World W;

static float vnoise(double x, double z, uint32_t salt) { return noise_value2(WORLD_SEED ^ salt, x, z); }

static double pdot(TerrainDir a, TerrainDir b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static TerrainDir padd(TerrainDir a, TerrainDir b) { TerrainDir r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static TerrainDir pscale(TerrainDir a, double s) { TerrainDir r = { a.x * s, a.y * s, a.z * s }; return r; }

static void ensure(void) {
    if (W.state == WORLD_EMPTY) plan_world_init(1);
}

/* --- Footprints -------------------------------------------------------------------- */

/* A footprint is a disc of the settlement's radius in a frame warped by two
 * octaves of value noise, the warp scaled with the radius (see plan.h), so
 * its edge measure changes no faster than CITY_EDGE_RATE. At CITY_RADIUS the
 * octaves are as given. The noise is read at (x, z); the disc is about c. */
static double warp_scale(double radius) { return radius / CITY_RADIUS; }

static double footprint_edge(DVec2 c, double radius, double x, double z) {
    double k = warp_scale(radius), w1 = CITY_WARP1_WAVE, w2 = CITY_WARP2_WAVE;
    if (k > 1.0) { w1 *= k; w2 *= k; }
    double wx = CITY_WARP1_M * (2.0 * vnoise(x / w1, z / w1, 0xED61u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / w2, z / w2, 0xED63u) - 1.0);
    double wz = CITY_WARP1_M * (2.0 * vnoise(x / w1, z / w1, 0xED62u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / w2, z / w2, 0xED64u) - 1.0);
    if (k != 1.0) { wx *= k; wz *= k; }
    return hypot(x - c.x + wx, z - c.z + wz) - radius;
}

static double warp_of(double radius) { return WARP_MAX * warp_scale(radius); }

/* How far from its centre a settlement's edge measure can be under PLAN_EDGE_CAP. */
static double settlement_reach(const PlanSettlement *s) {
    return s->radius + warp_of(s->radius) + PLAN_EDGE_CAP + 10.0;
}

/* The cut grid at (u, v) from its centre, or far below any edge off it. */
static double cut_at(const Cut *c, double u, double v) {
    if (!c->n) return -1e300;
    double fu = (u + c->half) / CUT_CELL, fv = (v + c->half) / CUT_CELL;
    if (!(fu >= 0.0 && fv >= 0.0 && fu < c->n - 1 && fv < c->n - 1)) return -1e300;
    int i = (int)fu, j = (int)fv;
    double tu = fu - i, tv = fv - j;
    const float *r0 = &c->d[(size_t)j * c->n + i], *r1 = r0 + c->n;
    return (1.0 - tv) * ((1.0 - tu) * r0[0] + tu * r0[1]) + tv * ((1.0 - tu) * r1[0] + tu * r1[1]);
}

/* The exact lake term at (u, v) of cut c, p there: PLAN_LAKE_M less the
 * distance to the nearest lake's shore, less LAKE_EXACT_FALL a metre of it
 * past LAKE_EXACT_FROM; far below any edge where no corner node is near a
 * lake, or the shore is LAKE_KNOWN_M or more away (then the grid's is the
 * larger anyway). */
static double lake_exact(const Cut *c, double u, double v, TerrainDir p) {
    if (!c->lake) return -1e300;
    double fu = (u + c->half) / CUT_CELL, fv = (v + c->half) / CUT_CELL;
    if (!(fu >= 0.0 && fv >= 0.0 && fu < c->n - 1 && fv < c->n - 1)) return -1e300;
    const float *r0 = &c->lake[(size_t)(int)fv * c->n + (int)fu], *r1 = r0 + c->n;
    if (fminf(fminf(r0[0], r0[1]), fminf(r1[0], r1[1])) >= (float)LAKE_NEAR_NODE) return -1e300;
    double d = terrain_earth_shore(p);
    if (d >= LAKE_KNOWN_M) return -1e300;
    return PLAN_LAKE_M - d - LAKE_EXACT_FALL * fmax(0.0, d - LAKE_EXACT_FROM);
}

/* Settlement k's edge measure at (u, v) of its own chart, p there: the disc
 * about its origin, its warp read where its centre stands in the world
 * chart, cut at the coast, the lakes and the mountains. */
static double settlement_edge(int k, double u, double v, TerrainDir p) {
    const PlanSettlement *s = &W.s[k];
    double e = footprint_edge(s->disc, s->radius, s->centre.x + u, s->centre.z + v);
    double c = fmax(cut_at(&W.cut[k], u, v), lake_exact(&W.cut[k], u, v, p));
    return e > c ? e : c;
}

/* The megacity's chart is the world's, and its cut grid lies about its core. */
double plan_home_edge(double x, double z) {
    ensure();
    const PlanSettlement *s = &W.s[0];
    double e = footprint_edge(s->disc, s->radius, x, z);
    double c = cut_at(&W.cut[0], x - s->centre.x, z - s->centre.z);
    if (W.cut[0].lake) c = fmax(c, lake_exact(&W.cut[0], x - s->centre.x, z - s->centre.z, terrain_chart_dir(&W.wc, x, z)));
    return e > c ? e : c;
}

double plan_home_reach(void) {
    ensure();
    return dlen(W.s[0].disc) + W.s[0].radius + warp_of(W.s[0].radius);
}

/* The index's square holding (x, z), or -1 beyond it. */
static int index_cell(double x, double z) {
    double fx = (x + INDEX_EXTENT) / INDEX_CELL, fz = (z + INDEX_EXTENT) / INDEX_CELL;
    if (!(fx >= 0.0 && fz >= 0.0 && fx < INDEX_SIDE && fz < INDEX_SIDE)) return -1;
    return (int)fz * INDEX_SIDE + (int)fx;
}

double plan_city_edge(double x, double z) {
    ensure();
    double e = fmin(plan_home_edge(x, z), PLAN_EDGE_CAP);
    if (W.state < WORLD_SETTLED) return e;
    int c = index_cell(x, z);
    if (c < 0 || W.cell_start[c] == W.cell_start[c + 1]) return e;
    TerrainDir p = terrain_chart_dir(&W.wc, x, z);
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        int id = W.cell_ids[k];
        const PlanSettlement *s = &W.s[id];
        double u, v;
        terrain_dir_chart(&s->chart, p, &u, &v);
        double d = hypot(u, v) - s->radius - warp_of(s->radius);
        if (d < e) e = fmin(e, settlement_edge(id, u, v, p));
    }
    return e;
}

/* --- Settlements --------------------------------------------------------------------- */

double plan_settlement_relief(double radius) {
    return fmin(PLAN_RELIEF_MAX_M, PLAN_RELIEF_MEGA_M * sqrt(CITY_RADIUS / radius));
}

double plan_settlement_radius(double km) { return CITY_RADIUS - 500.0 * (20.0 - km); }

double plan_settlement_km(double km2, double lo, double hi) {
    if (!(hi > lo) || km2 <= lo) return PLAN_CITY_MIN_KM;
    if (km2 >= hi) return PLAN_CITY_MAX_KM;
    return PLAN_CITY_MIN_KM + (PLAN_CITY_MAX_KM - PLAN_CITY_MIN_KM) * log(km2 / lo) / log(hi / lo);
}

int plan_settlement_count(void) {
    ensure();
    return W.n;
}

PlanSettlement plan_settlement(int k) {
    ensure();
    return W.s[k >= 0 && k < W.n ? k : 0];
}

const char *plan_settlement_name(int k) {
    ensure();
    return W.name[k >= 0 && k < W.n ? k : 0];
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
    if (c < 0 || W.cell_start[c] == W.cell_start[c + 1]) return 0;
    TerrainDir p = terrain_chart_dir(&W.wc, x, z);
    double bd = PLAN_EDGE_CAP;
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        const PlanSettlement *s = &W.s[W.cell_ids[k]];
        double u, v;
        terrain_dir_chart(&s->chart, p, &u, &v);
        double d = hypot(u, v) - s->radius;
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

/* --- A grid over the sphere ----------------------------------------------------------- */

size_t plan_cube_index(int n, int f, int i, int j) { return ((size_t)f * n + j) * n + i; }

int plan_cube_side(double spacing) { return (int)ceil(0.5 * PI * PLANET_RADIUS / spacing) + 1; }

TerrainDir plan_cube_dir(int n, int f, int i, int j) {
    return terrain_face_dir(f, -1.0 + 2.0 * i / (n - 1), -1.0 + 2.0 * j / (n - 1));
}

int plan_cube_at(int n, TerrainDir p, double *fi, double *fj) {
    double u, v;
    int f = terrain_dir_face(p, &u, &v);
    *fi = fmin(fmax(0.5 * (u + 1.0) * (n - 1), 0.0), n - 1.0);
    *fj = fmin(fmax(0.5 * (v + 1.0) * (n - 1), 0.0), n - 1.0);
    return f;
}

int plan_cube_step(int n, int f, int i, int j, int di, int dj, int *gi, int *gj) {
    int a = i + di, b = j + dj;
    if (a >= 0 && b >= 0 && a < n && b < n) { *gi = a; *gj = b; return f; }
    double fi, fj;
    int g = plan_cube_at(n, terrain_face_dir(f, -1.0 + 2.0 * a / (n - 1), -1.0 + 2.0 * b / (n - 1)), &fi, &fj);
    *gi = (int)floor(fi + 0.5);
    *gj = (int)floor(fj + 0.5);
    return g;
}

/* --- The sea to come, and the continents ---------------------------------------------- */

/* plan_road_dry at p, given the sea grid's margin there; the shore read
 * finely only where the grid finds the land low, not out in the ocean. Until
 * the settlements are placed (the continents' grid) there are none near. */
#define ROAD_SHORE_SPACING 2000.0   /* about mip 0 (m) */
#define ROAD_SHORE_DEEP    (-1000.0)
static int road_dry_at(TerrainDir p, double sea) {
    if (sea >= PLAN_ROAD_SEA_M) return 1;
    if (sea < ROAD_SHORE_DEEP) return 0;
    double shore = terrain_shore(p, ROAD_SHORE_SPACING);
    if (shore >= PLAN_ROAD_SHORE_M) return 1;
    if (shore < 0.0 || W.state < WORLD_SETTLED) return 0;
    double x, z;
    terrain_dir_chart(&W.wc, p, &x, &z);
    return plan_city_edge(x, z) < PLAN_EDGE_CAP;
}

/* Two nodes next to each other along a face are joined where the land
 * between them is dry all the way, read every JOIN_SAMPLES-th of the way (a
 * strait narrower than the grid's spacing parts them). */
#define JOIN_I       1
#define JOIN_J       2
#define JOIN_SAMPLES 8
static int joined(TerrainDir a, TerrainDir b) {
    for (int q = 1; q < JOIN_SAMPLES; ++q) {
        double t = (double)q / JOIN_SAMPLES;
        TerrainDir p = padd(pscale(a, 1.0 - t), pscale(b, t));
        p = pscale(p, 1.0 / sqrt(pdot(p, p)));
        if (!road_dry_at(p, terrain_sea_margin_dir(p))) return 0;
    }
    return 1;
}

static void sea_row(void *ctx, int r) {
    (void)ctx;
    const int n = W.sea_n, f = r / n, j = r % n;
    for (int i = 0; i < n; ++i) {
        TerrainDir p = plan_cube_dir(n, f, i, j);
        size_t k = plan_cube_index(n, f, i, j);
        W.sea[k] = (float)terrain_sea_margin_dir(p);
        W.dry[k] = (uint8_t)road_dry_at(p, W.sea[k]);
    }
}

/* After sea_row, every node's dry: its joins to the next along each way. */
static void join_row(void *ctx, int r) {
    (void)ctx;
    const int n = W.sea_n, f = r / n, j = r % n;
    for (int i = 0; i < n; ++i) {
        size_t k = plan_cube_index(n, f, i, j);
        uint8_t m = 0;
        if (W.dry[k]) {
            TerrainDir p = plan_cube_dir(n, f, i, j);
            if (i + 1 < n && W.dry[k + 1] && joined(p, plan_cube_dir(n, f, i + 1, j))) m |= JOIN_I;
            if (j + 1 < n && W.dry[k + n] && joined(p, plan_cube_dir(n, f, i, j + 1))) m |= JOIN_J;
        }
        W.join[k] = m;
    }
}

double plan_world_sea_dir(TerrainDir p) {
    ensure();
    const int n = W.sea_n;
    double fi, fj;
    int f = plan_cube_at(n, p, &fi, &fj);
    int i = fi < n - 1 ? (int)fi : n - 2, j = fj < n - 1 ? (int)fj : n - 2;
    double ti = fi - i, tj = fj - j;
    const float *r0 = &W.sea[plan_cube_index(n, f, i, j)], *r1 = r0 + n;
    return (1.0 - tj) * ((1.0 - ti) * r0[0] + ti * r0[1]) + tj * ((1.0 - ti) * r1[0] + ti * r1[1]);
}

double plan_world_sea(double x, double z) { ensure(); return plan_world_sea_dir(terrain_chart_dir(&W.wc, x, z)); }

int plan_road_dry_dir(TerrainDir p) { return road_dry_at(p, plan_world_sea_dir(p)); }

int plan_road_dry(double x, double z) { ensure(); return plan_road_dry_dir(terrain_chart_dir(&W.wc, x, z)); }

/* The continent of the land node nearest p within `near` nodes of the cell
 * holding it on its face, or 0. */
static int continent_near(TerrainDir p, int near) {
    const int n = W.sea_n;
    double fi, fj;
    int f = plan_cube_at(n, p, &fi, &fj), i0 = (int)floor(fi), j0 = (int)floor(fj), best = 0;
    double bd = -2.0;
    for (int j = j0 - near; j <= j0 + 1 + near; ++j)
        for (int i = i0 - near; i <= i0 + 1 + near; ++i) {
            if (i < 0 || j < 0 || i >= n || j >= n) continue;
            int m = W.land[plan_cube_index(n, f, i, j)];
            double d = pdot(p, plan_cube_dir(n, f, i, j));
            if (m && d > bd) { bd = d; best = m; }
        }
    return best;
}

int plan_world_continent_dir(TerrainDir p) {
    ensure();
    return plan_road_dry_dir(p) ? continent_near(p, 0) : 0;
}

int plan_world_continent(double x, double z) { ensure(); return plan_world_continent_dir(terrain_chart_dir(&W.wc, x, z)); }

static double sphere_m(TerrainDir a, TerrainDir b);

/* A settlement's continent: its centre's, or by the sea, where the grid
 * takes its centre for shore, the nearest land node's within
 * CONTINENT_NEAR. Without the Earth's bake, only within PLAN_STANDIN_ROADS
 * of the megacity: the stand-in's continents are noise, not the list's. */
static int settlement_continent(TerrainDir p) {
    if (!terrain_earth_loaded() && sphere_m(p, W.s[0].dir) > PLAN_STANDIN_ROADS) return 0;
    int n = plan_world_continent_dir(p);
    return n ? n : continent_near(p, CONTINENT_NEAR);
}

/* Land nodes joined along the grid's lines (join_row), and across the faces'
 * edges, numbered in the order a scan of the faces' rows first meets them. */
static void label_continents(void) {
    const int n = W.sea_n, total = 6 * n * n;
    int *stack = (int *)malloc(sizeof(int) * (size_t)total);
    if (!stack) fatal("out of memory labelling the continents");
    W.continents = 0;
    for (int start = 0; start < total; ++start) {
        if (W.land[start] || !W.dry[start]) continue;
        int label = ++W.continents, top = 0;
        W.land[start] = label;
        stack[top++] = start;
        while (top) {
            int c = stack[--top], f = c / (n * n), ci = c % n, cj = c / n % n;
            const int nb[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (int k = 0; k < 4; ++k) {
                int gi, gj, g = plan_cube_step(n, f, ci, cj, nb[k][0], nb[k][1], &gi, &gj);
                int q = (int)plan_cube_index(n, g, gi, gj);
                if (W.land[q] || !W.dry[q]) continue;
                if (g == f && !(k == 0 ? W.join[q] & JOIN_I : k == 1 ? W.join[c] & JOIN_I : k == 2 ? W.join[q] & JOIN_J : W.join[c] & JOIN_J)) continue;
                W.land[q] = label;
                stack[top++] = q;
            }
        }
    }
    free(stack);
}

/* --- The list ------------------------------------------------------------------------ */

typedef struct ListCity { int rank; char name[64]; double lat, lon; int ghsl; double area; } ListCity;

/* The next field of a CSV line from *p: quoted ("" for a quote) or plain, up
 * to a comma or the end; *p moves past its comma. */
static void csv_field(const char **p, char *out, size_t cap) {
    const char *s = *p;
    size_t n = 0;
    if (*s == '"') {
        for (++s; *s && !(*s == '"' && s[1] != '"'); ++s) {
            if (*s == '"') ++s;
            if (n + 1 < cap) out[n++] = *s;
        }
        if (*s == '"') ++s;
    }
    for (; *s && *s != ','; ++s)
        if (n + 1 < cap) out[n++] = *s;
    out[n] = 0;
    *p = *s == ',' ? s + 1 : s;
}

/* The list's rows, in its order; fatal without it. */
static int read_list(ListCity **out) {
    char roots[3][1024], path[1200];
    FILE *f = NULL;
    int nr = data_roots(roots);
    for (int r = 0; r < nr && !f; ++r) {
        snprintf(path, sizeof path, "%s%s", roots[r], PLAN_CITY_LIST);
        f = fopen(path, "rb");
    }
    if (!f) fatal("plan: no city list " PLAN_CITY_LIST " (beside the executable, here, or up one)");
    int n = 0, cap = 256;
    ListCity *c = (ListCity *)malloc(sizeof(ListCity) * (size_t)cap);
    char line[512], field[128];
    if (!c) fatal("out of memory reading the city list");
    for (int row = 0; fgets(line, sizeof line, f); ++row) {
        line[strcspn(line, "\r\n")] = 0;
        if (row == 0 || !line[0]) continue;   /* the header */
        if (n == cap && !(c = (ListCity *)realloc(c, sizeof(ListCity) * (size_t)(cap *= 2)))) fatal("out of memory reading the city list");
        const char *p = line;
        ListCity *k = &c[n];
        csv_field(&p, field, sizeof field); k->rank = atoi(field);
        csv_field(&p, k->name, sizeof k->name);
        csv_field(&p, field, sizeof field); k->lat = atof(field);
        csv_field(&p, field, sizeof field); k->lon = atof(field);
        csv_field(&p, field, sizeof field); k->ghsl = atoi(field);   /* 0 where empty */
        csv_field(&p, field, sizeof field);                          /* the centre's name */
        csv_field(&p, field, sizeof field); k->area = atof(field);
        if (k->rank < 1 || !k->name[0] || fabs(k->lat) > 90.0 || fabs(k->lon) > 180.0 || !(k->area >= 0.0)) {
            fprintf(stderr, "plan: %s row %d isn't rank,name,latitude,longitude,ghsl_id,ghsl_name,area_km2,...\n", path, row + 1);
            fatal("the city list has a bad row");
        }
        n++;
    }
    fclose(f);
    if (!n) fatal("the city list is empty");
    *out = c;
    return n;
}

static double sphere_m(TerrainDir a, TerrainDir b) {
    TerrainDir c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return PLANET_RADIUS * atan2(sqrt(pdot(c, c)), pdot(a, b));
}

static int uf_find(int *p, int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }

/* Two footprints of these radii are too close when their centres are nearer
 * than this: their discs, twice the larger warp and SEP_GAP. */
static double spacing_of(double ra, double rb) { return ra + rb + 2.0 * fmax(warp_of(ra), warp_of(rb)) + SEP_GAP; }

/* A city of the list, as merged and sized, before its cut; `row` is its
 * rows' root in the merging (n for the megacity's, when no row is). */
typedef struct Group { TerrainDir dir; double km, area; int rank; int home; int row; const char *name; } Group;

/* The merging's start: every row its own city but those at the megacity's
 * place (node n is the megacity, at the chart's origin). */
static void merge_start(const ListCity *c, int n, int *parent) {
    for (int k = 0; k <= n; ++k) parent[k] = k;
    for (int a = 0; a < n; ++a)
        if (sphere_m(terrain_latlon_dir(c[a].lat, c[a].lon), W.wc.o) < 1000.0 * HOME_KM) parent[uf_find(parent, a)] = uf_find(parent, n);
}

/* The list merged, from the rows already joined in parent (n + 1 nodes, the
 * last the megacity): the megacity's group first, then by rank. Returns how
 * many. */
static int merge_list(const ListCity *c, int n, int *parent, Group *g, int max) {
    int *root = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    TerrainDir *d = (TerrainDir *)malloc(sizeof(TerrainDir) * (size_t)n), *gd = (TerrainDir *)malloc(sizeof(TerrainDir) * (size_t)(n + 1));
    double *ga = (double *)malloc(sizeof(double) * (size_t)(n + 1)), *gr = (double *)malloc(sizeof(double) * (size_t)(n + 1));
    if (!root || !d || !gd || !ga || !gr) fatal("out of memory merging the city list");
    double lo = 1e300, hi = 0.0;
    for (int k = 0; k < n; ++k) {
        d[k] = terrain_latlon_dir(c[k].lat, c[k].lon);
        if (c[k].area > 0.0) { lo = fmin(lo, c[k].area); hi = fmax(hi, c[k].area); }
    }
    if (hi <= 0.0) lo = hi = 1.0;   /* no areas: every city the smallest */
    for (;;) {
        /* each group's area (its rows' centres, each once) and place (weighted by their areas) */
        TerrainDir zero = { 0.0, 0.0, 0.0 };
        for (int k = 0; k <= n; ++k) { ga[k] = 0.0; gd[k] = zero; }
        for (int k = 0; k < n; ++k) {
            int r = uf_find(parent, k), seen = 0;
            double a = c[k].area > 0.0 ? c[k].area : lo;
            for (int j = 0; j < k && c[k].ghsl && !seen; ++j) seen = c[j].ghsl == c[k].ghsl && uf_find(parent, j) == r;
            if (!seen) ga[r] += a;
            gd[r] = padd(gd[r], pscale(d[k], a));
        }
        int home = uf_find(parent, n);
        if (ga[home] <= 0.0) ga[home] = hi;   /* a megacity no row stands for: the largest */
        for (int k = 0; k <= n; ++k) {
            if (uf_find(parent, k) != k) continue;
            gd[k] = k == home ? W.wc.o : pscale(gd[k], 1.0 / sqrt(pdot(gd[k], gd[k])));
            gr[k] = plan_settlement_radius(plan_settlement_km(ga[k], lo, hi));
        }
        int any = 0;
        for (int a = 0; a <= n; ++a) {
            if (uf_find(parent, a) != a) continue;
            for (int b = a + 1; b <= n; ++b) {
                if (parent[b] != b || uf_find(parent, a) != a) continue;
                if (sphere_m(gd[a], gd[b]) >= spacing_of(gr[a], gr[b])) continue;
                parent[b] = a;
                any = 1;
            }
        }
        if (!any) break;
    }
    int ng = 0, home = uf_find(parent, n);
    for (int k = 0; k <= n; ++k) root[k] = -1;
    /* the megacity's group is group 0 */
    root[home] = ng;
    g[ng++] = (Group){ W.wc.o, plan_settlement_km(ga[home], lo, hi), ga[home], 0, 1, home, "the megacity" };
    for (int k = 0; k < n; ++k) {
        int r = uf_find(parent, k);
        if (root[r] < 0) {
            if (ng == max) fatal("the city list has more cities than PLAN_MAX_SETTLEMENTS");
            root[r] = ng;
            g[ng++] = (Group){ gd[r], plan_settlement_km(ga[r], lo, hi), ga[r], c[k].rank, 0, r, c[k].name };
        }
        Group *q = &g[root[r]];
        if (q->rank == 0 || c[k].rank < q->rank) { q->rank = c[k].rank; q->name = c[k].name; }
    }
    /* the rest by rank (insertion sort: equal ranks keep the list's order) */
    for (int a = 2; a < ng; ++a)
        for (int b = a; b > 1 && g[b].rank < g[b - 1].rank; --b) { Group t = g[b]; g[b] = g[b - 1]; g[b - 1] = t; }
    free(root); free(d); free(gd); free(ga); free(gr);
    return ng;
}

/* --- The cut ------------------------------------------------------------------------- */

/* The squared distance from each of n samples to the nearest where f is 0
 * (f is 0 there and huge elsewhere), in place: Felzenszwalb and
 * Huttenlocher's lower envelope of parabolas. v and z hold n and n + 1. */
static void edt_line(double *f, int n, int stride, int *v, double *z, double *out) {
    int k = 0;
    v[0] = 0; z[0] = -1e300; z[1] = 1e300;
    for (int q = 1; q < n; ++q) {
        double fq = f[(size_t)q * stride] + (double)q * q, s;
        for (;;) {
            int p = v[k];
            s = (fq - (f[(size_t)p * stride] + (double)p * p)) / (2.0 * (q - p));
            if (s > z[k]) break;
            k--;   /* z[0] is below every s, so k stays >= 0 */
        }
        k++;
        v[k] = q; z[k] = s; z[k + 1] = 1e300;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) k++;
        int p = v[k];
        out[q] = (double)(q - p) * (q - p) + f[(size_t)p * stride];
    }
    for (int q = 0; q < n; ++q) f[(size_t)q * stride] = out[q];
}

/* The squared distance in nodes from each node to the nearest marked one. */
static void edt(const uint8_t *mark, int n, double *d) {
    int *v = (int *)malloc(sizeof(int) * (size_t)n);
    double *z = (double *)malloc(sizeof(double) * (size_t)(n + 1)), *tmp = (double *)malloc(sizeof(double) * (size_t)n);
    if (!v || !z || !tmp) fatal("out of memory cutting the footprints");
    for (int k = 0; k < n * n; ++k) d[k] = mark[k] ? 0.0 : 1e30;
    for (int j = 0; j < n; ++j) edt_line(&d[(size_t)j * n], n, 1, v, z, tmp);
    for (int i = 0; i < n; ++i) edt_line(&d[i], n, n, v, z, tmp);
    free(v); free(z); free(tmp);
}

/* The signed distance in metres from each node to the edge of the marked
 * nodes, positive among them: the edge half a node from the last node
 * either side of it. in and out are scratch. */
static void signed_distance(const uint8_t *mark, int n, double *in, double *out, uint8_t *inv, float *d, float add) {
    for (size_t q = 0; q < (size_t)n * n; ++q) inv[q] = !mark[q];
    edt(mark, n, in);
    edt(inv, n, out);
    for (size_t q = 0; q < (size_t)n * n; ++q)
        d[q] = (float)((mark[q] ? sqrt(out[q]) - 0.5 : 0.5 - sqrt(in[q])) * CUT_CELL + add);
}

/* The distance from each node to the lakes (m): d holds it where it is
 * known (exactly, near a lake) and 1e30 elsewhere, and is carried on to the
 * rest by a two-pass chamfer, a node's at most its neighbour's plus the step
 * between them, so it changes by no more than a node's spacing a node. */
static void chamfer(float *d, int n) {
    const float a = (float)CUT_CELL, g = (float)(CUT_CELL * 1.4142135623730951);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            float *q = &d[(size_t)j * n + i];
            if (i > 0) *q = fminf(*q, q[-1] + a);
            if (j > 0) {
                *q = fminf(*q, q[-n] + a);
                if (i > 0) *q = fminf(*q, q[-n - 1] + g);
                if (i < n - 1) *q = fminf(*q, q[-n + 1] + g);
            }
        }
    for (int j = n - 1; j >= 0; --j)
        for (int i = n - 1; i >= 0; --i) {
            float *q = &d[(size_t)j * n + i];
            if (i < n - 1) *q = fminf(*q, q[1] + a);
            if (j < n - 1) {
                *q = fminf(*q, q[n] + a);
                if (i < n - 1) *q = fminf(*q, q[n + 1] + g);
                if (i > 0) *q = fminf(*q, q[n - 1] + g);
            }
        }
}

/* The cut grid over [-half, half]^2 about (ox, oz) of chart ch; none where it
 * finds neither sea, lakes nor mountains. */
static void cut_build(Cut *c, const TerrainChart *ch, double ox, double oz, double half) {
    memset(c, 0, sizeof(*c));
    int n = (int)ceil(2.0 * half / CUT_CELL) + 1;
    double h0 = 0.5 * (n - 1) * CUT_CELL;
    size_t nn = (size_t)n * n;
    uint8_t *sea = (uint8_t *)malloc(nn), *steep = (uint8_t *)malloc(nn), *inv = (uint8_t *)malloc(nn);
    double *h = (double *)malloc(sizeof(double) * nn), *din = (double *)malloc(sizeof(double) * nn), *dout = (double *)malloc(sizeof(double) * nn);
    float *ds = (float *)malloc(sizeof(float) * nn), *lake = (float *)malloc(sizeof(float) * nn);
    if (!sea || !steep || !inv || !h || !din || !dout || !ds || !lake) fatal("out of memory cutting the footprints");
    int nsea = 0, nsteep = 0, nlake = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            size_t q = (size_t)j * n + i;
            TerrainDir p = terrain_chart_dir(ch, ox - h0 + i * CUT_CELL, oz - h0 + j * CUT_CELL);
            /* the sea, at the node or half a node either way, so no channel slips between the nodes */
            double u = ox - h0 + i * CUT_CELL, v = oz - h0 + j * CUT_CELL;
            sea[q] = terrain_shore(p, CUT_CELL) < 0.0;
            for (int e = 0; e < 4 && !sea[q]; ++e)
                sea[q] = terrain_shore(terrain_chart_dir(ch, u + (e == 0 ? 0.5 : e == 1 ? -0.5 : 0.0) * CUT_CELL,
                                                         v + (e == 2 ? 0.5 : e == 3 ? -0.5 : 0.0) * CUT_CELL), CUT_CELL) < 0.0;
            h[q] = terrain_earth_height(p, STEEP_SPACING);
            nsea += sea[q];
            /* the lakes' shores: exact near them, inside one however far in */
            double s = terrain_earth_shore(p);
            lake[q] = s < 0.0 ? (float)s : s < LAKE_KNOWN_M ? (float)s : 1e30f;
            nlake += s < LAKE_KNOWN_M;
        }
    /* the grade between the nodes either side (one side at the grid's border), on land */
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            size_t q = (size_t)j * n + i;
            int i0 = i > 0 ? i - 1 : i, i1 = i < n - 1 ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < n - 1 ? j + 1 : j;
            double gx = (h[(size_t)j * n + i1] - h[(size_t)j * n + i0]) / ((i1 - i0) * CUT_CELL);
            double gz = (h[(size_t)j1 * n + i] - h[(size_t)j0 * n + i]) / ((j1 - j0) * CUT_CELL);
            steep[q] = !sea[q] && hypot(gx, gz) > PLAN_STEEP_GRADE;
            nsteep += steep[q];
        }
    if (nsteep) {
        /* closed: grown by PLAN_STEEP_CLOSE_M and shrunk back, so the steep
         * patches of one range (the Earth's 2.4 km soften a mountainside's
         * grade) are one, the valleys between them with them */
        double k2 = (PLAN_STEEP_CLOSE_M / CUT_CELL) * (PLAN_STEEP_CLOSE_M / CUT_CELL);
        edt(steep, n, din);
        for (size_t q = 0; q < nn; ++q) inv[q] = din[q] > k2;   /* beyond the grown mask */
        edt(inv, n, dout);
        for (size_t q = 0; q < nn; ++q) {
            int closed = !inv[q] && dout[q] > k2 && !sea[q];
            nsteep += closed && !steep[q];
            steep[q] |= (uint8_t)closed;
        }
    }
    if (nsea || nsteep || nlake) {
        c->n = n;
        c->half = h0;
        c->d = (float *)malloc(sizeof(float) * nn);
        if (!c->d) fatal("out of memory cutting the footprints");
        signed_distance(sea, n, din, dout, inv, c->d, (float)PLAN_COAST_M);
        signed_distance(steep, n, din, dout, inv, ds, (float)-PLAN_STEEP_REACH_M);
        for (size_t q = 0; q < nn; ++q) c->d[q] = fmaxf(c->d[q], ds[q]);
        if (nlake) {
            chamfer(lake, n);
            for (size_t q = 0; q < nn; ++q) c->d[q] = fmaxf(c->d[q], (float)PLAN_LAKE_M - lake[q]);
            c->lake = lake;
            lake = NULL;
        }
    }
    free(sea); free(steep); free(inv); free(h); free(din); free(dout); free(ds); free(lake);
}

/* Footprints are counted on every this-many-th node of the grid each way, so
 * about AREA_NODES across the largest. */
static int area_stride(double r) { return 1 + (int)(2.0 * r / CUT_CELL / AREA_NODES); }

/* The cut grid's nodes (every stride-th each way) inside a footprint of
 * radius r about (cu, cv) from the grid's centre, its warp read at off plus
 * the node's place; with cut 0, the disc's alone. */
static int cut_area(const Cut *c, DVec2 off, double cu, double cv, double r, int cut, int stride) {
    double reach = r + warp_of(r);
    int i0 = (int)floor((cu - reach + c->half) / CUT_CELL), i1 = (int)ceil((cu + reach + c->half) / CUT_CELL);
    int j0 = (int)floor((cv - reach + c->half) / CUT_CELL), j1 = (int)ceil((cv + reach + c->half) / CUT_CELL);
    i0 = i0 < 0 ? 0 : i0 / stride * stride;
    j0 = j0 < 0 ? 0 : j0 / stride * stride;
    if (i1 > c->n - 1) i1 = c->n - 1;
    if (j1 > c->n - 1) j1 = c->n - 1;
    int count = 0;
    for (int j = j0; j <= j1; j += stride)
        for (int i = i0; i <= i1; i += stride) {
            double u = -c->half + i * CUT_CELL, v = -c->half + j * CUT_CELL;
            if (cut && c->d[(size_t)j * c->n + i] >= 0.0f) continue;
            count += footprint_edge(dv(off.x + cu, off.z + cv), r, off.x + u, off.z + v) < 0.0;
        }
    return count;
}

/* The half-width of the cut grid a footprint of radius r needs: over its
 * edge measure's reach, so that off the grid the cut can't matter. */
static double cut_half(double r) { return r + warp_of(r) + PLAN_EDGE_CAP + 2.0 * CUT_CELL; }

/* Where a footprint of radius r keeps its area against cut grid cs: its
 * centre (cu, cv) from the grid's centre, its warp read at off plus the
 * place. ok(ctx, cu, cv) says whether a centre may stand there. The disc
 * steps away from what the cut takes off it, up to `near` (in PUSH_DIRS
 * directions round that), then all round up to `far`. Returns the area kept
 * (in counted nodes), or -1 where no centre may stand anywhere so near. */
typedef int (*CentreOk)(const void *ctx, const Cut *cs, DVec2 off, double r, double cu, double cv);
static int push_footprint(const Cut *cs, DVec2 off, double r, double near, double far, CentreOk ok, const void *ctx, DVec2 *at) {
    int stride = area_stride(r), target = cut_area(cs, off, 0.0, 0.0, r, 0, stride), want = (int)ceil((1.0 - AREA_SLACK) * target);
    int best = -1;
    double step = fmax(PUSH_STEP_MIN, r / PUSH_STEPS);
    *at = dv(0.0, 0.0);
    if (ok(ctx, cs, off, r, 0.0, 0.0)) best = cut_area(cs, off, 0.0, 0.0, r, 1, stride);
    if (best >= want) return best;
    /* away from the centroid of what the cut takes */
    double gx = 0.0, gz = 0.0;
    int ncut = 0;
    for (int j = 0; j < cs->n; j += stride)
        for (int i = 0; i < cs->n; i += stride) {
            double u = -cs->half + i * CUT_CELL, v = -cs->half + j * CUT_CELL;
            if (cs->d[(size_t)j * cs->n + i] < 0.0f || hypot(u, v) > r + warp_of(r)) continue;
            if (footprint_edge(off, r, off.x + u, off.z + v) < 0.0) { gx += u; gz += v; ncut++; }
        }
    double a0 = ncut ? atan2(-gz, -gx) : 0.0;
    int found = 0;
    for (int k = 1; !found && k * step <= (best < 0 ? far : near); ++k) {
        int inland = ncut && k * step <= near;
        for (int q = 0; q < (inland ? PUSH_DIRS : 16) && !found; ++q) {
            /* 0, +25, -25, +50, -50 degrees; all round where nothing points away, and beyond `near` */
            double a = inland ? a0 + (q == 0 ? 0.0 : ((q + 1) / 2) * (q % 2 ? 1.0 : -1.0) * 25.0 * PI / 180.0) : 2.0 * PI * q / 16.0;
            double cu = k * step * cos(a), cv = k * step * sin(a);
            if (!ok(ctx, cs, off, r, cu, cv)) continue;
            int area = cut_area(cs, off, cu, cv, r, 1, stride);
            if (area > best) { best = area; *at = dv(cu, cv); }
            found = area >= want;
        }
    }
    return best;
}

/* The cut grid cropped to [-half, half]^2, about the same centre. */
static void cut_crop(Cut *c, double half) {
    int m = (int)ceil(2.0 * half / CUT_CELL) + 1;
    if (!c->n || m >= c->n) return;
    if ((c->n - m) & 1) m++;   /* the same nodes, about the same centre */
    int o = (c->n - m) / 2;
    for (int pass = 0; pass < 2; ++pass) {
        float **src = pass ? &c->lake : &c->d;
        if (!*src) continue;
        float *d = (float *)malloc(sizeof(float) * (size_t)m * m);
        if (!d) fatal("out of memory cutting the footprints");
        for (int j = 0; j < m; ++j) memcpy(&d[(size_t)j * m], &(*src)[(size_t)(j + o) * c->n + o], sizeof(float) * (size_t)m);
        free(*src);
        *src = d;
    }
    c->n = m;
    c->half = 0.5 * (m - 1) * CUT_CELL;
}

/* The radius, from r up to GROW_MAX times it, at which a footprint about
 * (cu, cv) of cut grid c keeps its area. */
static double grow_footprint(const Cut *c, DVec2 off, double cu, double cv, double r) {
    int stride = area_stride(GROW_MAX * r), want = (int)ceil((1.0 - AREA_SLACK) * cut_area(c, off, cu, cv, r, 0, stride));
    double grown = r;
    for (double grow = 1.0; grow <= GROW_MAX + 1e-9; grow += 0.02) {
        grown = r * grow;
        if (cut_area(c, off, cu, cv, grown, 1, stride) >= want) break;
    }
    return fmin(grown, r * GROW_MAX);
}

/* A city's centre stands CENTRE_INLAND inside the cut. */
static int centre_inland(const void *ctx, const Cut *cs, DVec2 off, double r, double cu, double cv) {
    (void)ctx; (void)off; (void)r;
    return cut_at(cs, cu, cv) <= -CENTRE_INLAND;
}

/* The megacity's core, at the grid's centre, stays HOME_CORE inside the edge
 * of a disc about (cu, cv). */
static int home_core(const void *ctx, const Cut *cs, DVec2 off, double r, double cu, double cv) {
    (void)ctx;
    return cut_at(cs, 0.0, 0.0) > -HOME_CORE || footprint_edge(dv(off.x + cu, off.z + cv), r, off.x, off.z) <= -HOME_CORE;
}

typedef struct PlaceCtx { const Group *g; int ng; PlanSettlement *s; int *ok; } PlaceCtx;

/* Group k placed: pushed off its cut, moved to its town, its cut grid made
 * in its own chart and its radius grown to keep its area. Sets ok[k] to 0
 * where it finds no land. */
static void place_job(void *ctx, int job) {
    PlaceCtx *pc = (PlaceCtx *)ctx;
    int k = job + 1;   /* group 0 is the megacity */
    const Group *g = &pc->g[k];
    PlanSettlement *s = &pc->s[k];
    Cut *final = &W.cut[k];
    memset(s, 0, sizeof(*s));
    memset(final, 0, sizeof(*final));
    pc->ok[k] = 1;
    double r = plan_settlement_radius(g->km);
    TerrainChart sc = terrain_chart_about(g->dir, 0.0);
    DVec2 off;
    terrain_dir_chart(&W.wc, g->dir, &off.x, &off.z);
    Cut cs;
    cut_build(&cs, &sc, 0.0, 0.0, r + fmax(r, PUSH_FAR) + warp_of(r) + 2.0 * CUT_CELL);   /* the footprint, pushed as far as it may be */
    DVec2 at = dv(0.0, 0.0);
    if (cs.n) {
        int best = push_footprint(&cs, off, r, r, fmax(r, PUSH_FAR), centre_inland, NULL, &at);
        cut_free(&cs);
        if (best < 0) { pc->ok[k] = 0; return; }   /* nowhere inland enough for its centre */
    }
    /* its town, its own chart about it, and its cut there */
    double wx, wz;
    terrain_dir_chart(&W.wc, terrain_chart_dir(&sc, at.x, at.z), &wx, &wz);
    plan_town_at(wx, wz, &s->town[0], &s->town[1]);
    s->centre = s->disc = plan_town_centre(s->town[0], s->town[1]);
    s->dir = terrain_chart_dir(&W.wc, s->centre.x, s->centre.z);
    s->chart = terrain_chart_about(s->dir, 0.0);
    s->size_km = g->km;
    s->area_km2 = g->area;
    s->rank = g->rank;
    s->colossal = g->rank <= PLAN_COLOSSAL_RANK;
    s->radius = r;
    cut_build(final, &s->chart, 0.0, 0.0, cut_half(GROW_MAX * r));
    if (!final->n) return;
    if (cut_at(final, 0.0, 0.0) > -CITY_BELT) { pc->ok[k] = 0; return; }   /* its town took it to the cut */
    s->radius = grow_footprint(final, s->centre, 0.0, 0.0, r);
    cut_crop(final, cut_half(s->radius));
}

/* The megacity: its core stays at town (0, 0), its disc pushed off the cut
 * (no further than keeps the core HOME_CORE inside its edge) and grown. */
static void place_home(const Group *g) {
    PlanSettlement *s = &W.s[0];
    double r = plan_settlement_radius(g->km), near = fmax(0.0, r - HOME_CORE);
    DVec2 c0 = s->centre, at = dv(0.0, 0.0);
    Cut cs;
    cut_build(&cs, &W.wc, c0.x, c0.z, near + r + warp_of(r) + 2.0 * CUT_CELL);
    if (cs.n) {
        push_footprint(&cs, c0, r, near, near, home_core, NULL, &at);
        cut_free(&cs);
    }
    s->disc = dadd(c0, at);
    s->size_km = g->km;
    s->area_km2 = g->area;
    s->rank = g->rank;
    s->colossal = 1;
    s->radius = r;
    cut_build(&W.cut[0], &W.wc, c0.x, c0.z, dlen(at) + cut_half(GROW_MAX * r));
    if (W.cut[0].n) s->radius = grow_footprint(&W.cut[0], c0, at.x, at.z, r);
    cut_crop(&W.cut[0], dlen(at) + cut_half(s->radius));
    s->relief_m = plan_settlement_relief(s->radius);
}

/* Two footprints never meet: the lower-ranked of a pair too close shrinks. */
static int space_out(PlanSettlement *s, int n) {
    int shrunk = 0;
    TerrainDir *disc = (TerrainDir *)malloc(sizeof(TerrainDir) * (size_t)n);
    if (!disc) fatal("out of memory spacing the settlements");
    for (int k = 0; k < n; ++k) disc[k] = k ? s[k].dir : terrain_chart_dir(&W.wc, s[k].disc.x, s[k].disc.z);
    for (int round = 0; round < 8; ++round) {
        int any = 0;
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b) {
                if (s[a].radius == 0.0 || s[b].radius == 0.0) continue;
                double d = sphere_m(disc[a], disc[b]);
                if (d >= spacing_of(s[a].radius, s[b].radius)) continue;
                /* b follows a in rank (the megacity first) */
                double rb = d - s[a].radius - 2.0 * warp_of(s[a].radius) - SEP_GAP;
                if (rb > s[a].radius) rb = (d - s[a].radius - SEP_GAP) / (1.0 + 2.0 * WARP_MAX / CITY_RADIUS);
                s[b].radius = fmin(s[b].radius, rb - 1.0);
                if (s[b].radius < SHRINK_MIN) s[b].radius = 0.0;   /* left out: see place_settlements */
                shrunk++;
                any = 1;
            }
        if (!any) break;
    }
    free(disc);
    return shrunk;
}

/* The first LOST_NAMED cities left out, named in a list for the log. */
#define LOST_NAMED 8
static void note_lost(char *list, size_t cap, int count, const char *name) {
    if (count >= LOST_NAMED) return;
    size_t n = strlen(list);
    snprintf(list + n, cap - n, "%s%s", count ? ", " : "", name);
}

/* Cities their cuts pushed too close to each other are one, and the list is
 * merged and placed again, up to MERGE_PASSES times; then space_out shrinks
 * what is still too close. */
#define MERGE_PASSES 4

static void place_settlements(int threads) {
    ListCity *list;
    int nl = read_list(&list);
    Group *g = (Group *)malloc(sizeof(Group) * PLAN_MAX_SETTLEMENTS);
    PlanSettlement *s = (PlanSettlement *)malloc(sizeof(PlanSettlement) * PLAN_MAX_SETTLEMENTS);
    int *ok = (int *)malloc(sizeof(int) * PLAN_MAX_SETTLEMENTS), *parent = (int *)malloc(sizeof(int) * (size_t)(nl + 1));
    TerrainDir *at = (TerrainDir *)malloc(sizeof(TerrainDir) * PLAN_MAX_SETTLEMENTS);
    if (!g || !s || !ok || !parent || !at) fatal("out of memory placing the settlements");
    merge_start(list, nl, parent);
    int ng = 0, rejoined = 0;
    for (int pass = 0;; ++pass) {
        ng = merge_list(list, nl, parent, g, PLAN_MAX_SETTLEMENTS);
        place_home(&g[0]);
        PlaceCtx pc = { g, ng, s, ok };
        thread_parallel_for(ng - 1, threads, place_job, &pc);
        if (pass == MERGE_PASSES) break;
        ok[0] = 1;
        at[0] = terrain_chart_dir(&W.wc, W.s[0].disc.x, W.s[0].disc.z);
        for (int k = 1; k < ng; ++k) at[k] = s[k].dir;
        int any = 0;
        for (int a = 0; a < ng; ++a)
            for (int b = a + 1; b < ng; ++b) {
                double ra = a ? s[a].radius : W.s[0].radius;
                if (!ok[a] || !ok[b] || sphere_m(at[a], at[b]) >= spacing_of(ra, s[b].radius)) continue;
                int pa = uf_find(parent, g[a].row), pb = uf_find(parent, g[b].row);
                if (pa != pb) { parent[pb] = pa; any = 1; rejoined++; }
            }
        if (!any) break;
        for (int k = 0; k < ng; ++k) { cut_free(&W.cut[k]); memset(&W.cut[k], 0, sizeof(Cut)); }
    }
    /* the megacity, with the name of the city it is */
    snprintf(W.name[0], sizeof W.name[0], "%s", g[0].name);
    int n = 1, lost = 0;
    char gone[512] = "";
    for (int k = 1; k < ng; ++k) {
        if (!ok[k]) { note_lost(gone, sizeof gone, lost++, g[k].name); cut_free(&W.cut[k]); continue; }
        W.s[n] = s[k];
        W.cut[n] = W.cut[k];
        snprintf(W.name[n], sizeof W.name[n], "%s", g[k].name);
        n++;
    }
    for (int k = n; k < ng; ++k) memset(&W.cut[k], 0, sizeof(Cut));
    int shrunk = space_out(W.s, n), m = 1;
    for (int k = 1; k < n; ++k) {
        if (W.s[k].radius == 0.0) { note_lost(gone, sizeof gone, lost++, W.name[k]); cut_free(&W.cut[k]); continue; }
        W.s[m] = W.s[k];
        W.cut[m] = W.cut[k];
        memmove(W.name[m], W.name[k], sizeof W.name[m]);
        W.s[m].relief_m = plan_settlement_relief(W.s[m].radius);
        m++;
    }
    for (int k = m; k < n; ++k) memset(&W.cut[k], 0, sizeof(Cut));
    W.n = n = m;
    if (lost) fprintf(stderr, "plan: %d of the %d cities found no room on the land at their place: %s%s\n", lost, ng, gone,
                      lost > LOST_NAMED ? ", ..." : "");
    if (shrunk) fprintf(stderr, "plan: %d settlements shrunk to keep their neighbours' distance\n", shrunk);
    if (rejoined) fprintf(stderr, "plan: %d pairs of cities pushed too close to each other merged\n", rejoined);
    free(list); free(g); free(s); free(ok); free(parent); free(at);
}

/* --- The index ------------------------------------------------------------------------ */

/* The squares settlement k's reach covers in the world chart: the box round
 * its reach's rim there (the chart maps the cap onto the region its rim
 * bounds), padded for the chords between samples; all of them where the cap
 * holds the chart's antipode. */
static void reach_cells(int k, int *i0, int *i1, int *j0, int *j1) {
    const PlanSettlement *s = &W.s[k];
    double r = settlement_reach(s);
    if (sphere_m(s->dir, W.wc.o) + r > PI * PLANET_RADIUS - 2.0 * INDEX_CELL) { *i0 = *j0 = 0; *i1 = *j1 = INDEX_SIDE - 1; return; }
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (int q = 0; q < 64; ++q) {
        double a = 2.0 * PI * q / 64.0, x, z;
        terrain_dir_chart(&W.wc, terrain_chart_dir(&s->chart, r * cos(a), r * sin(a)), &x, &z);
        x0 = fmin(x0, x); x1 = fmax(x1, x); z0 = fmin(z0, z); z1 = fmax(z1, z);
    }
    double pad = 0.02 * fmax(x1 - x0, z1 - z0) + 1000.0;
    *i0 = (int)floor((x0 - pad + INDEX_EXTENT) / INDEX_CELL); *i1 = (int)floor((x1 + pad + INDEX_EXTENT) / INDEX_CELL);
    *j0 = (int)floor((z0 - pad + INDEX_EXTENT) / INDEX_CELL); *j1 = (int)floor((z1 + pad + INDEX_EXTENT) / INDEX_CELL);
    if (*i0 < 0) *i0 = 0;
    if (*j0 < 0) *j0 = 0;
    if (*i1 > INDEX_SIDE - 1) *i1 = INDEX_SIDE - 1;
    if (*j1 > INDEX_SIDE - 1) *j1 = INDEX_SIDE - 1;
}

static void build_index(void) {
    const int cells = INDEX_SIDE * INDEX_SIDE;
    W.cell_start = (int *)calloc((size_t)cells + 1, sizeof(int));
    if (!W.cell_start) fatal("out of memory indexing the settlements");
    int (*box)[4] = malloc(sizeof(int[4]) * (size_t)W.n);
    if (!box) fatal("out of memory indexing the settlements");
    for (int k = 1; k < W.n; ++k) reach_cells(k, &box[k][0], &box[k][1], &box[k][2], &box[k][3]);
    for (int pass = 0; pass < 2; ++pass) {
        for (int k = 1; k < W.n; ++k)
            for (int j = box[k][2]; j <= box[k][3]; ++j)
                for (int i = box[k][0]; i <= box[k][1]; ++i) {
                    int c = j * INDEX_SIDE + i;
                    if (pass == 0) W.cell_start[c + 1]++;
                    else W.cell_ids[W.cell_start[c]++] = k;
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
    free(box);
}

void plan_world_init(int threads) {
    if (W.state != WORLD_EMPTY) return;
    W.state = WORLD_PLACING;
    if (threads < 1) threads = 1;
    W.wc = terrain_world_chart();
    /* the megacity, first, whatever else follows: its core at town (0, 0),
     * a disc of CITY_RADIUS round it until place_settlements sizes it */
    W.n = 1;
    memset(&W.s[0], 0, sizeof(W.s[0]));
    memset(&W.cut[0], 0, sizeof(W.cut[0]));
    plan_town_at(plan_town_centre(0, 0).x, plan_town_centre(0, 0).z, &W.s[0].town[0], &W.s[0].town[1]);
    W.s[0].centre = W.s[0].disc = plan_town_centre(W.s[0].town[0], W.s[0].town[1]);
    W.s[0].size_km = 20.0;
    W.s[0].radius = plan_settlement_radius(20.0);
    W.s[0].relief_m = plan_settlement_relief(W.s[0].radius);
    W.s[0].colossal = 1;
    W.s[0].dir = terrain_chart_dir(&W.wc, W.s[0].centre.x, W.s[0].centre.z);
    W.s[0].chart = W.wc;
    snprintf(W.name[0], sizeof W.name[0], "the megacity");

    W.sea_n = plan_cube_side(PLAN_SEA_CELL);
    size_t nodes = 6 * (size_t)W.sea_n * W.sea_n;
    W.sea = (float *)malloc(sizeof(float) * nodes);
    W.land = (int *)calloc(nodes, sizeof(int));
    W.dry = (uint8_t *)malloc(nodes);
    W.join = (uint8_t *)malloc(nodes);
    if (!W.sea || !W.land || !W.dry || !W.join) fatal("out of memory mapping the sea");
    thread_parallel_for(6 * W.sea_n, threads, sea_row, NULL);
    thread_parallel_for(6 * W.sea_n, threads, join_row, NULL);
    label_continents();

    place_settlements(threads);
    W.relief_lo = W.relief_hi = W.s[0].relief_m;
    for (int k = 0; k < W.n; ++k) {
        W.s[k].continent = settlement_continent(W.s[k].dir);
        W.relief_lo = fmin(W.relief_lo, W.s[k].relief_m);
        W.relief_hi = fmax(W.relief_hi, W.s[k].relief_m);
    }
    build_index();
    W.state = WORLD_SETTLED;
    terrain_earth_check_streams();   /* the streams were derived with these settlements, or are dropped */
    plan_water_build(threads);
    plan_roads_build(threads);
    W.state = WORLD_READY;
}
