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
 * each footprint cut at its coast and pushed inland, the sea to come on the
 * roads' grid and which land is one continent, and an index so the edge
 * measure and the settlement near a point are cheap to find.
 *
 * The list: PLAN_CITY_LIST under one of the data roots (data_roots), a
 * header and then rank,name,latitude,longitude a row, the name in double
 * quotes where it holds a comma. Rows within PLAN_MERGE_KM of each other over
 * the sphere are one city, chained; so are those within it of the world
 * chart's origin, which join the megacity (Los Angeles, on the Earth's bake).
 * A merged city stands at its parts' centre weighted by their areas.
 *
 * The coast. Every settlement but the megacity has a coast grid in its own
 * chart: nodes COAST_CELL apart over the reach of its edge measure, each the
 * signed distance to the shore (terrain_shore at mip 0; positive over the
 * sea) plus PLAN_COAST_M, interpolated between them. The footprint's edge is
 * the larger of the warped disc's and the grid's, so the footprint stops
 * PLAN_COAST_M short of the sea; the grid's bilinear distance changes by at
 * most sqrt 2 a metre, well inside CITY_EDGE_RATE. Where the grid finds no
 * sea it is left out. A footprint the cut leaves short of its area by more
 * than AREA_SLACK is pushed: its centre steps inland, away from the part cut
 * off, by PUSH_STEP at a time up to its radius (in PUSH_DIRS directions
 * round that), to the nearest place where it keeps its area, or to where it
 * keeps the most; its centre must stay CENTRE_INLAND inside the cut. One
 * that finds no place for its centre so near (on a cape, a spit or a delta)
 * looks on to PUSH_FAR, all round. Then its radius grows, up to GROW_MAX,
 * until it has its area. A city whose centre finds no such land is left out
 * (polders below the sea; the stand-in's seas, without a bake). Areas are
 * counted in the grid's nodes.
 *
 * Spacing. Two footprints never meet: between centres there is at least the
 * sum of their radii, twice the larger warp and SEP_GAP, so the settlement
 * nearest a point (plan_settlement_near) changes only where both edge
 * measures are positive. Where two would come closer, the lower-ranked one
 * shrinks (the megacity never).
 *
 * The city's centre is moved to the centre of its town in the world chart,
 * so its core is a town as the megacity's is (plan_town_regional); its chart
 * is about that place.
 *
 * The sea to come: terrain_sea_margin on a grid of PLAN_SEA_CELL over the
 * roads' reach, interpolated between its nodes. A road may go where the
 * grid's margin is PLAN_ROAD_SEA_M or more; where it is less, but not out in
 * the ocean, the Earth's shore is read finely (plan_road_dry), so the coastal
 * plains, which the grid's 20 km mix with the sea beside them, are land. The
 * nodes a road may reach, joined along the grid's lines, are one continent.
 *
 * Determinism: the list is read in order and everything after it is
 * sequenced (the coasts on threads, each its own); counts of nodes are
 * compared, not areas, so the game (built with fast floating point) and the
 * tests place the same cities unless a node falls within rounding of an edge.
 *
 * Until plan_world_init has placed them, the edge measure and everything
 * else here knows only the megacity. */

#define PI 3.14159265358979323846

#define SEA_EXTENT    (PLAN_ROAD_REACH + 600000.0)   /* the sea grid covers |x|, |z| up to this (m) */
#define SEA_SIDE      ((int)(2.0 * SEA_EXTENT / PLAN_SEA_CELL) + 2)
#define COAST_CELL    500.0       /* the coast grid's spacing (m) */
#define COAST_SCAN    1000.0      /* a first look for the sea at this spacing... */
#define COAST_SCAN_M  30.0        /* ...finds it wherever the shore is under this (m) */
#define AREA_SLACK    0.03        /* a footprint may lose this share of its area to the coast */
#define PUSH_STEP     500.0       /* m */
#define PUSH_DIRS     5           /* inland and 25 and 50 degrees either side */
#define PUSH_FAR      40000.0     /* how far a city with nowhere nearer may be pushed (m) */
#define GROW_MAX      1.3
#define CENTRE_INLAND (CITY_BELT + 2.0 * TOWN_SIZE)   /* the centre stands this far inside the cut, and its town's (m) */
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

typedef struct Coast {
    int    n;      /* nodes a side, 0 for none */
    double half;   /* the nodes span [-half, half]^2 of the settlement's chart (m) */
    float *d;      /* n * n, row by row: the signed distance to the shore plus PLAN_COAST_M (m) */
} Coast;

typedef struct World {
    int            state;
    int            n;
    PlanSettlement s[PLAN_MAX_SETTLEMENTS];
    char           name[PLAN_MAX_SETTLEMENTS][64];
    Coast          coast[PLAN_MAX_SETTLEMENTS];
    TerrainChart   wc;           /* the world chart */
    double         relief_lo, relief_hi;
    float         *sea;          /* SEA_SIDE^2 nodes, row by row: node (i, j) at -SEA_EXTENT + (i, j) * PLAN_SEA_CELL */
    uint8_t       *dry;          /* each node's plan_road_dry */
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
 * octaves of value noise, the warp scaled with the radius up to the
 * megacity's (so its edge measure changes no faster than the megacity's,
 * CITY_EDGE_RATE). At CITY_RADIUS the formula is the megacity's own,
 * operation for operation. The noise is read at (x, z); the disc is about c. */
static double warp_scale(double radius) { return radius < CITY_RADIUS ? radius / CITY_RADIUS : 1.0; }

static double footprint_edge(DVec2 c, double radius, double x, double z) {
    double k = warp_scale(radius);
    double wx = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED61u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED63u) - 1.0);
    double wz = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED62u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED64u) - 1.0);
    if (k != 1.0) { wx *= k; wz *= k; }
    return hypot(x - c.x + wx, z - c.z + wz) - radius;
}

static double warp_of(double radius) { return WARP_MAX * warp_scale(radius); }

/* How far from its centre a settlement's edge measure can be under PLAN_EDGE_CAP. */
static double settlement_reach(const PlanSettlement *s) {
    return s->radius + warp_of(s->radius) + PLAN_EDGE_CAP + 10.0;
}

/* The coast grid at (u, v) of its chart, or far below any edge off it. */
static double coast_at(const Coast *c, double u, double v) {
    if (!c->n) return -1e300;
    double fu = (u + c->half) / COAST_CELL, fv = (v + c->half) / COAST_CELL;
    if (!(fu >= 0.0 && fv >= 0.0 && fu < c->n - 1 && fv < c->n - 1)) return -1e300;
    int i = (int)fu, j = (int)fv;
    double tu = fu - i, tv = fv - j;
    const float *r0 = &c->d[(size_t)j * c->n + i], *r1 = r0 + c->n;
    return (1.0 - tv) * ((1.0 - tu) * r0[0] + tu * r0[1]) + tv * ((1.0 - tu) * r1[0] + tu * r1[1]);
}

/* Settlement k's edge measure at (u, v) of its own chart: the disc about its
 * origin, its warp read where its centre stands in the world chart, cut at
 * the coast. */
static double settlement_edge(int k, double u, double v) {
    const PlanSettlement *s = &W.s[k];
    double e = footprint_edge(s->centre, s->radius, s->centre.x + u, s->centre.z + v);
    double c = coast_at(&W.coast[k], u, v);
    return e > c ? e : c;
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
    if (c < 0 || W.cell_start[c] == W.cell_start[c + 1]) return e;
    TerrainDir p = terrain_chart_dir(&W.wc, x, z);
    for (int k = W.cell_start[c]; k < W.cell_start[c + 1]; ++k) {
        int id = W.cell_ids[k];
        const PlanSettlement *s = &W.s[id];
        double u, v;
        terrain_dir_chart(&s->chart, p, &u, &v);
        double d = hypot(u, v) - s->radius - warp_of(s->radius);
        if (d < e) e = fmin(e, settlement_edge(id, u, v));
    }
    return e;
}

double plan_city_edge(double x, double z) { return plan_city_edges(x, z, NULL); }

/* --- Settlements --------------------------------------------------------------------- */

double plan_settlement_relief(double radius) {
    return fmin(PLAN_RELIEF_MAX_M, PLAN_RELIEF_MEGA_M * sqrt(CITY_RADIUS / radius));
}

double plan_settlement_radius(double km) { return CITY_RADIUS - 500.0 * (20.0 - km); }

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

/* --- The sea to come, and the continents ---------------------------------------------- */

static double sea_node_x(int i) { return -SEA_EXTENT + i * PLAN_SEA_CELL; }

/* plan_road_dry, given the sea grid's margin there; the shore read finely
 * only where the grid finds the land low, not out in the ocean. Until the
 * settlements are placed (the continents' grid) there are none near. */
#define ROAD_SHORE_SPACING 2000.0   /* about mip 0 (m) */
#define ROAD_SHORE_DEEP    (-1000.0)
static int road_dry_at(double x, double z, double sea) {
    if (sea >= PLAN_ROAD_SEA_M) return 1;
    if (sea < ROAD_SHORE_DEEP) return 0;
    double shore = terrain_shore(terrain_chart_dir(&W.wc, x, z), ROAD_SHORE_SPACING);
    if (shore >= PLAN_ROAD_SHORE_M) return 1;
    return shore >= 0.0 && W.state >= WORLD_SETTLED && plan_city_edge(x, z) < PLAN_EDGE_CAP;
}

static void sea_row(void *ctx, int j) {
    (void)ctx;
    for (int i = 0; i < SEA_SIDE; ++i) {
        double x = sea_node_x(i), z = sea_node_x(j);
        W.sea[(size_t)j * SEA_SIDE + i] = hypot(x, z) > SEA_EXTENT ? -1e9f : (float)terrain_sea_margin(x, z);
        W.dry[(size_t)j * SEA_SIDE + i] = (uint8_t)road_dry_at(x, z, W.sea[(size_t)j * SEA_SIDE + i]);
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

int plan_road_dry(double x, double z) { ensure(); return road_dry_at(x, z, plan_world_sea(x, z)); }

int plan_world_continent(double x, double z) {
    ensure();
    if (!plan_road_dry(x, z)) return 0;
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

/* A settlement's continent: its centre's, or by the sea, where the grid
 * takes its centre for shore, the nearest land node's within
 * CONTINENT_NEAR; 0 beyond the roads' reach. */
static int settlement_continent(DVec2 c) {
    if (dlen(c) > PLAN_ROAD_REACH - CITY_RADIUS) return 0;
    int n = plan_world_continent(c.x, c.z);
    if (n) return n;
    int i0 = (int)floor((c.x + SEA_EXTENT) / PLAN_SEA_CELL), j0 = (int)floor((c.z + SEA_EXTENT) / PLAN_SEA_CELL);
    double bd = 1e300;
    for (int j = j0 - CONTINENT_NEAR; j <= j0 + 1 + CONTINENT_NEAR; ++j)
        for (int i = i0 - CONTINENT_NEAR; i <= i0 + 1 + CONTINENT_NEAR; ++i) {
            if (i < 0 || j < 0 || i >= SEA_SIDE || j >= SEA_SIDE) continue;
            int m = W.land[(size_t)j * SEA_SIDE + i];
            double d = hypot(c.x - sea_node_x(i), c.z - sea_node_x(j));
            if (m && d < bd) { bd = d; n = m; }
        }
    return n;
}

/* Land nodes joined along the grid's lines, numbered in the order a scan of
 * the rows first meets them. */
static void label_continents(void) {
    const int n = SEA_SIDE;
    int *stack = (int *)malloc(sizeof(int) * (size_t)n * n);
    if (!stack) fatal("out of memory labelling the continents");
    W.continents = 0;
    for (int start = 0; start < n * n; ++start) {
        if (W.land[start] || !W.dry[start]) continue;
        int label = ++W.continents, top = 0;
        W.land[start] = label;
        stack[top++] = start;
        while (top) {
            int c = stack[--top], ci = c % n, cj = c / n;
            const int nb[4][2] = { { ci - 1, cj }, { ci + 1, cj }, { ci, cj - 1 }, { ci, cj + 1 } };
            for (int k = 0; k < 4; ++k) {
                if (nb[k][0] < 0 || nb[k][1] < 0 || nb[k][0] >= n || nb[k][1] >= n) continue;
                int q = nb[k][1] * n + nb[k][0];
                if (W.land[q] || !W.dry[q]) continue;
                W.land[q] = label;
                stack[top++] = q;
            }
        }
    }
    free(stack);
}

/* --- The list ------------------------------------------------------------------------ */

typedef struct ListCity { int rank; char name[64]; double lat, lon; } ListCity;

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
        if (k->rank < 1 || !k->name[0] || fabs(k->lat) > 90.0 || fabs(k->lon) > 180.0) {
            fprintf(stderr, "plan: %s row %d isn't rank,name,latitude,longitude\n", path, row + 1);
            fatal("the city list has a bad row");
        }
        n++;
    }
    fclose(f);
    if (!n) fatal("the city list is empty");
    *out = c;
    return n;
}

/* The size across a rank builds (km): the colossal cities' 20, then from
 * PLAN_MEGA_MAX_KM down to PLAN_MEGA_MIN_KM at the last rank, as a power of
 * the rank. */
static double rank_km(int rank, int last) {
    if (rank <= PLAN_COLOSSAL_RANK) return 20.0;
    if (last <= PLAN_COLOSSAL_RANK + 1) return PLAN_MEGA_MAX_KM;
    double a = log(PLAN_MEGA_MAX_KM / PLAN_MEGA_MIN_KM) / log((double)last / (PLAN_COLOSSAL_RANK + 1));
    return PLAN_MEGA_MAX_KM * pow((double)rank / (PLAN_COLOSSAL_RANK + 1), -a);
}

static double sphere_m(TerrainDir a, TerrainDir b) {
    TerrainDir c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return PLANET_RADIUS * atan2(sqrt(pdot(c, c)), pdot(a, b));
}

static int uf_find(int *p, int x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }

/* A city of the list, as merged and sized, before its coast. */
typedef struct Group { TerrainDir dir; double km; int rank; int home; const char *name; } Group;

/* The list merged: the megacity's group first, then by rank. Returns how many. */
static int merge_list(const ListCity *c, int n, Group *g, int max) {
    int *parent = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    TerrainDir *d = (TerrainDir *)malloc(sizeof(TerrainDir) * (size_t)n);
    if (!parent || !d) fatal("out of memory merging the city list");
    int last = 0;
    for (int k = 0; k < n; ++k) { d[k] = terrain_latlon_dir(c[k].lat, c[k].lon); last = c[k].rank > last ? c[k].rank : last; }
    for (int k = 0; k <= n; ++k) parent[k] = k;
    /* node n is the megacity at the chart's origin */
    for (int a = 0; a < n; ++a) {
        if (sphere_m(d[a], W.wc.o) < 1000.0 * PLAN_MERGE_KM) parent[uf_find(parent, a)] = uf_find(parent, n);
        for (int b = a + 1; b < n; ++b)
            if (sphere_m(d[a], d[b]) < 1000.0 * PLAN_MERGE_KM) parent[uf_find(parent, b)] = uf_find(parent, a);
    }
    int ng = 0;
    int *root = (int *)malloc(sizeof(int) * (size_t)(n + 1));
    if (!root) fatal("out of memory merging the city list");
    for (int k = 0; k <= n; ++k) root[k] = -1;
    /* the megacity's group is group 0 */
    root[uf_find(parent, n)] = ng;
    g[ng++] = (Group){ W.wc.o, 20.0, 0, 1, "the megacity" };
    double ksum[PLAN_MAX_SETTLEMENTS] = { 0 };
    TerrainDir acc[PLAN_MAX_SETTLEMENTS];
    acc[0] = (TerrainDir){ 0.0, 0.0, 0.0 };
    for (int k = 0; k < n; ++k) {
        int r = uf_find(parent, k);
        if (root[r] < 0) {
            if (ng == max) fatal("the city list has more cities than PLAN_MAX_SETTLEMENTS");
            root[r] = ng;
            g[ng] = (Group){ d[k], 0.0, c[k].rank, 0, c[k].name };
            acc[ng] = (TerrainDir){ 0.0, 0.0, 0.0 };
            ng++;
        }
        Group *q = &g[root[r]];
        if (q->rank == 0 || c[k].rank < q->rank) { q->rank = c[k].rank; q->name = c[k].name; }
        double km = rank_km(c[k].rank, last);
        acc[root[r]] = padd(acc[root[r]], pscale(d[k], km * km));
        ksum[root[r]] += km * km;
    }
    for (int k = 1; k < ng; ++k) {
        double l = sqrt(pdot(acc[k], acc[k]));
        g[k].dir = pscale(acc[k], 1.0 / l);
        g[k].km = fmin(20.0, sqrt(ksum[k]));
    }
    /* the rest by rank (insertion sort: equal ranks keep the list's order) */
    for (int a = 2; a < ng; ++a)
        for (int b = a; b > 1 && g[b].rank < g[b - 1].rank; --b) { Group t = g[b]; g[b] = g[b - 1]; g[b - 1] = t; }
    free(parent); free(d); free(root);
    return ng;
}

/* --- The coast ------------------------------------------------------------------------ */

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
    if (!v || !z || !tmp) fatal("out of memory cutting the coasts");
    for (int k = 0; k < n * n; ++k) d[k] = mark[k] ? 0.0 : 1e30;
    for (int j = 0; j < n; ++j) edt_line(&d[(size_t)j * n], n, 1, v, z, tmp);
    for (int i = 0; i < n; ++i) edt_line(&d[i], n, n, v, z, tmp);
    free(v); free(z); free(tmp);
}

/* The coast grid over [-half, half]^2 of chart ch; none where the scan finds no sea. */
static void coast_build(Coast *c, const TerrainChart *ch, double half) {
    memset(c, 0, sizeof(*c));
    int ns = (int)ceil(2.0 * half / COAST_SCAN) + 1, any = 0;
    for (int j = 0; j < ns && !any; ++j)
        for (int i = 0; i < ns && !any; ++i)
            any = terrain_shore(terrain_chart_dir(ch, -half + i * COAST_SCAN, -half + j * COAST_SCAN), COAST_CELL) < COAST_SCAN_M;
    if (!any) return;
    int n = (int)ceil(2.0 * half / COAST_CELL) + 1;
    c->n = n;
    c->half = 0.5 * (n - 1) * COAST_CELL;
    uint8_t *sea = (uint8_t *)malloc((size_t)n * n), *land = (uint8_t *)malloc((size_t)n * n);
    double *ds = (double *)malloc(sizeof(double) * (size_t)n * n), *dl = (double *)malloc(sizeof(double) * (size_t)n * n);
    c->d = (float *)malloc(sizeof(float) * (size_t)n * n);
    if (!sea || !land || !ds || !dl || !c->d) fatal("out of memory cutting the coasts");
    int nsea = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            size_t q = (size_t)j * n + i;
            sea[q] = terrain_shore(terrain_chart_dir(ch, -c->half + i * COAST_CELL, -c->half + j * COAST_CELL), COAST_CELL) < 0.0;
            land[q] = !sea[q];
            nsea += sea[q];
        }
    if (!nsea) { free(c->d); memset(c, 0, sizeof(*c)); }
    else {
        edt(sea, n, ds);
        edt(land, n, dl);
        /* the shore half a node from the last node either side of it */
        for (size_t q = 0; q < (size_t)n * n; ++q) {
            double sd = sea[q] ? sqrt(dl[q]) - 0.5 : 0.5 - sqrt(ds[q]);
            c->d[q] = (float)(sd * COAST_CELL + PLAN_COAST_M);
        }
    }
    free(sea); free(land); free(ds); free(dl);
}

/* The coast grid's nodes inside a footprint of radius r about (cu, cv) of
 * its chart, its warp read at off plus the node's place; with cut 0, the
 * disc's alone. */
static int coast_area(const Coast *c, DVec2 off, double cu, double cv, double r, int cut) {
    double reach = r + warp_of(r);
    int i0 = (int)floor((cu - reach + c->half) / COAST_CELL), i1 = (int)ceil((cu + reach + c->half) / COAST_CELL);
    int j0 = (int)floor((cv - reach + c->half) / COAST_CELL), j1 = (int)ceil((cv + reach + c->half) / COAST_CELL);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > c->n - 1) i1 = c->n - 1;
    if (j1 > c->n - 1) j1 = c->n - 1;
    int count = 0;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            double u = -c->half + i * COAST_CELL, v = -c->half + j * COAST_CELL;
            if (cut && c->d[(size_t)j * c->n + i] >= 0.0f) continue;
            count += footprint_edge(dv(off.x + cu, off.z + cv), r, off.x + u, off.z + v) < 0.0;
        }
    return count;
}

/* The half-width of the coast grid a footprint of radius r needs: over its
 * edge measure's reach, so that off the grid the cut can't matter. */
static double coast_half(double r) { return r + warp_of(r) + PLAN_EDGE_CAP + 2.0 * COAST_CELL; }

typedef struct PlaceCtx { const Group *g; int ng; PlanSettlement *s; int *ok; } PlaceCtx;

/* Group k placed: pushed inland off the coast, moved to its town, its
 * coast grid made in its own chart and its radius grown to keep its area.
 * Sets ok[k] to 0 where it finds no land. */
static void place_job(void *ctx, int job) {
    PlaceCtx *pc = (PlaceCtx *)ctx;
    int k = job + 1;   /* group 0 is the megacity */
    const Group *g = &pc->g[k];
    PlanSettlement *s = &pc->s[k];
    Coast *final = &W.coast[k];
    memset(s, 0, sizeof(*s));
    memset(final, 0, sizeof(*final));
    pc->ok[k] = 1;
    double r = plan_settlement_radius(g->km);
    TerrainChart sc = terrain_chart_about(g->dir, 0.0);
    DVec2 off;
    terrain_dir_chart(&W.wc, g->dir, &off.x, &off.z);
    Coast cs;
    coast_build(&cs, &sc, r + fmax(r, PUSH_FAR) + warp_of(r) + 2.0 * COAST_CELL);   /* the footprint, pushed as far as it may be */
    DVec2 at = dv(0.0, 0.0);
    if (cs.n) {
        int target = coast_area(&cs, off, 0.0, 0.0, r, 0), want = (int)ceil((1.0 - AREA_SLACK) * target);
        int best = -1, area0 = coast_area(&cs, off, 0.0, 0.0, r, 1);
        if (coast_at(&cs, 0.0, 0.0) <= -CENTRE_INLAND) best = area0;
        if (best < want) {
            /* inland: away from the centroid of what the coast cut off */
            double gx = 0.0, gz = 0.0;
            int ncut = 0;
            for (int j = 0; j < cs.n; ++j)
                for (int i = 0; i < cs.n; ++i) {
                    double u = -cs.half + i * COAST_CELL, v = -cs.half + j * COAST_CELL;
                    if (cs.d[(size_t)j * cs.n + i] < 0.0f || hypot(u, v) > r + warp_of(r)) continue;
                    if (footprint_edge(off, r, off.x + u, off.z + v) < 0.0) { gx += u; gz += v; ncut++; }
                }
            double a0 = ncut ? atan2(-gz, -gx) : 0.0;
            int found = 0;
            for (int step = 1; !found && step * PUSH_STEP <= (best < 0 ? fmax(r, PUSH_FAR) : r); ++step) {
                int inland = ncut && step * PUSH_STEP <= r;
                for (int q = 0; q < (inland ? PUSH_DIRS : 16) && !found; ++q) {
                    /* 0, +25, -25, +50, -50 degrees; all round where nothing points inland, and beyond r */
                    double a = inland ? a0 + (q == 0 ? 0.0 : ((q + 1) / 2) * (q % 2 ? 1.0 : -1.0) * 25.0 * PI / 180.0) : 2.0 * PI * q / 16.0;
                    double cu = step * PUSH_STEP * cos(a), cv = step * PUSH_STEP * sin(a);
                    if (coast_at(&cs, cu, cv) > -CENTRE_INLAND) continue;
                    int area = coast_area(&cs, off, cu, cv, r, 1);
                    if (area > best) { best = area; at = dv(cu, cv); }
                    found = area >= want;
                }
            }
        }
        if (best < 0) {   /* nowhere inland enough for its centre */
            pc->ok[k] = 0;
            free(cs.d);
            return;
        }
        free(cs.d);
    }
    /* its town, its own chart about it, and its coast there */
    double wx, wz;
    terrain_dir_chart(&W.wc, terrain_chart_dir(&sc, at.x, at.z), &wx, &wz);
    plan_town_at(wx, wz, &s->town[0], &s->town[1]);
    s->centre = plan_town_centre(s->town[0], s->town[1]);
    s->dir = terrain_chart_dir(&W.wc, s->centre.x, s->centre.z);
    s->chart = terrain_chart_about(s->dir, 0.0);
    s->size_km = g->km;
    s->rank = g->rank;
    s->colossal = g->rank <= PLAN_COLOSSAL_RANK;
    s->radius = r;
    coast_build(final, &s->chart, coast_half(GROW_MAX * r));
    if (!final->n) return;
    if (coast_at(final, 0.0, 0.0) > -CITY_BELT) { pc->ok[k] = 0; return; }   /* its town took it to the shore */
    int target = coast_area(final, s->centre, 0.0, 0.0, r, 0), want = (int)ceil((1.0 - AREA_SLACK) * target);
    for (double grow = 1.0; grow <= GROW_MAX + 1e-9; grow += 0.02) {
        s->radius = r * grow;
        if (coast_area(final, s->centre, 0.0, 0.0, s->radius, 1) >= want) break;
    }
    if (s->radius > r * GROW_MAX) s->radius = r * GROW_MAX;
}

/* Two footprints never meet: the lower-ranked of a pair too close shrinks. */
static int space_out(PlanSettlement *s, int n) {
    int shrunk = 0;
    for (int round = 0; round < 8; ++round) {
        int any = 0;
        for (int a = 0; a < n; ++a)
            for (int b = a + 1; b < n; ++b) {
                if (s[a].radius == 0.0 || s[b].radius == 0.0) continue;
                double d = sphere_m(s[a].dir, s[b].dir);
                double need = s[a].radius + s[b].radius + 2.0 * fmax(warp_of(s[a].radius), warp_of(s[b].radius)) + SEP_GAP;
                if (d >= need) continue;
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
    return shrunk;
}

/* The first LOST_NAMED cities left out, named in a list for the log. */
#define LOST_NAMED 8
static void note_lost(char *list, size_t cap, int count, const char *name) {
    if (count >= LOST_NAMED) return;
    size_t n = strlen(list);
    snprintf(list + n, cap - n, "%s%s", count ? ", " : "", name);
}

static void place_settlements(int threads) {
    ListCity *list;
    int nl = read_list(&list);
    Group *g = (Group *)malloc(sizeof(Group) * PLAN_MAX_SETTLEMENTS);
    PlanSettlement *s = (PlanSettlement *)malloc(sizeof(PlanSettlement) * PLAN_MAX_SETTLEMENTS);
    int *ok = (int *)malloc(sizeof(int) * PLAN_MAX_SETTLEMENTS);
    if (!g || !s || !ok) fatal("out of memory placing the settlements");
    int ng = merge_list(list, nl, g, PLAN_MAX_SETTLEMENTS);
    PlaceCtx pc = { g, ng, s, ok };
    thread_parallel_for(ng - 1, threads, place_job, &pc);
    /* the megacity, as it always was, with the name of the city it is */
    W.s[0].rank = g[0].rank;
    snprintf(W.name[0], sizeof W.name[0], "%s", g[0].name);
    int n = 1, lost = 0;
    char gone[512] = "";
    for (int k = 1; k < ng; ++k) {
        if (!ok[k]) { note_lost(gone, sizeof gone, lost++, g[k].name); free(W.coast[k].d); continue; }
        W.s[n] = s[k];
        W.coast[n] = W.coast[k];
        snprintf(W.name[n], sizeof W.name[n], "%s", g[k].name);
        n++;
    }
    for (int k = n; k < ng; ++k) memset(&W.coast[k], 0, sizeof(Coast));
    int shrunk = space_out(W.s, n), m = 1;
    for (int k = 1; k < n; ++k) {
        if (W.s[k].radius == 0.0) { note_lost(gone, sizeof gone, lost++, W.name[k]); free(W.coast[k].d); continue; }
        W.s[m] = W.s[k];
        W.coast[m] = W.coast[k];
        memmove(W.name[m], W.name[k], sizeof W.name[m]);
        W.s[m].relief_m = plan_settlement_relief(W.s[m].radius);
        m++;
    }
    for (int k = m; k < n; ++k) memset(&W.coast[k], 0, sizeof(Coast));
    W.n = n = m;
    if (lost) fprintf(stderr, "plan: %d of the %d cities found no room on the land at their place: %s%s\n", lost, ng, gone,
                      lost > LOST_NAMED ? ", ..." : "");
    if (shrunk) fprintf(stderr, "plan: %d settlements shrunk to keep their neighbours' distance\n", shrunk);
    free(list); free(g); free(s); free(ok);
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
    /* the megacity, first, whatever else follows */
    W.n = 1;
    memset(&W.s[0], 0, sizeof(W.s[0]));
    plan_town_at(plan_town_centre(0, 0).x, plan_town_centre(0, 0).z, &W.s[0].town[0], &W.s[0].town[1]);
    W.s[0].centre = plan_town_centre(W.s[0].town[0], W.s[0].town[1]);
    W.s[0].size_km = 20.0;
    W.s[0].radius = plan_settlement_radius(20.0);
    W.s[0].relief_m = plan_settlement_relief(W.s[0].radius);
    W.s[0].colossal = 1;
    W.s[0].dir = terrain_chart_dir(&W.wc, W.s[0].centre.x, W.s[0].centre.z);
    W.s[0].chart = W.wc;
    snprintf(W.name[0], sizeof W.name[0], "the megacity");
    W.relief_lo = W.relief_hi = W.s[0].relief_m;

    W.sea = (float *)malloc(sizeof(float) * (size_t)SEA_SIDE * SEA_SIDE);
    W.land = (int *)calloc((size_t)SEA_SIDE * SEA_SIDE, sizeof(int));
    W.dry = (uint8_t *)malloc((size_t)SEA_SIDE * SEA_SIDE);
    if (!W.sea || !W.land || !W.dry) fatal("out of memory mapping the sea");
    thread_parallel_for(SEA_SIDE, threads, sea_row, NULL);
    label_continents();

    place_settlements(threads);
    for (int k = 0; k < W.n; ++k) {
        W.s[k].continent = settlement_continent(W.s[k].centre);
        W.relief_lo = fmin(W.relief_lo, W.s[k].relief_m);
        W.relief_hi = fmax(W.relief_hi, W.s[k].relief_m);
    }
    build_index();
    W.state = WORLD_SETTLED;
    plan_roads_build(threads);
    W.state = WORLD_READY;
}
