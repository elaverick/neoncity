#pragma once
#include "common.h"
#include "geom2d.h"
#include "mathlib.h"

/* The city plan: a polycentric, cellular layout in the spirit of Singapore's
 * new towns, generated as a pure function of world position.
 *
 *   Towns          Voronoi cells of a jittered 1.5 km lattice. Each has a
 *                  centre whose intensity feeds a polycentric density field;
 *                  about one in seven is a regional centre with a dense core.
 *                  Town boundaries are boulevards.
 *   Neighbourhoods Voronoi cells of a jittered ~430 m lattice, clipped to and
 *                  computed within their town. Their edges are arterials that
 *                  meet the boulevards in T-junctions.
 *   Blocks         Each neighbourhood is split recursively along its own grain
 *                  by local streets, giving staggered T-junctions rather than a
 *                  through grid. Leaves are inset by the road and sidewalk
 *                  widths of their edges and zoned (core, centre, estate,
 *                  industrial, park, rural) from density, distance to the
 *                  centres and nearness to the city's edge.
 *
 * The city is finite (plan_city_edge). It is built of whole towns: a town is
 * built when all of its ground lies where plan_urban is 1, so the city's
 * ground and the terrain beyond agree wherever the two meet. Towards the edge
 * the city thins (plan_fringe): lower, with more industry, parks and open
 * land. Its outermost boulevards are the city's perimeter road; the ground
 * reaches across them and ends a shoulder beyond, where the land begins.
 *
 * Nothing here depends on query order or region: a road or block comes out
 * identical whichever area it is requested through. Coordinates are world
 * metres in double precision. */

#define TOWN_SIZE        1500.0
#define NBHD_SIZE         430.0
#define PLAN_MAX_PV        24
#define PLAN_MAX_ROADS    512
#define PLAN_MAX_BLOCKS   384
#define PLAN_MAX_TOWNS     24
#define PLAN_MAX_EDGES     96

/* The city is finite: a disc round town (0,0)'s centre, its edge warped by two
 * octaves of value noise so it doesn't read as a circle. Inside the edge, a
 * belt where the city gives way to the land around it. Metres. */
#define CITY_RADIUS     12500.0
#define CITY_BELT        1500.0
#define CITY_WARP1_M     1800.0   /* warp amplitude and wavelength, first octave */
#define CITY_WARP1_WAVE  6000.0
#define CITY_WARP2_M      500.0   /* second octave */
#define CITY_WARP2_WAVE  2000.0
/* Bound on how fast plan_city_edge changes, metres per metre. Each warp
 * component's gradient is at most 3*sqrt(2)*(M1/WAVE1 + M2/WAVE2) (value
 * noise: 1.5 per lattice unit per axis), so the warp's Jacobian has norm at
 * most 6*(...), and the edge (a length through it) changes by at most 1 plus
 * that. A little over, for float rounding. */
#define CITY_EDGE_RATE  (1.0 + 6.0 * (CITY_WARP1_M / CITY_WARP1_WAVE + CITY_WARP2_M / CITY_WARP2_WAVE) + 0.01)
/* A built town's ground stays at or inside this edge measure, where
 * plan_urban is exactly 1 (and terrain_height is the city's ground). */
#define CITY_BUILT_EDGE  (-CITY_BELT)
/* The ground reaches across a perimeter boulevard to this far beyond its
 * carriageway (m); every town's ground lies within CITY_GROUND_PAD of its cell. */
#define CITY_SHOULDER      1.5
#define CITY_GROUND_PAD   (13.0 + CITY_SHOULDER)   /* the widest boulevard's half-width, plus the shoulder */
/* Inside the built edge, the depth of the fringe over which the city thins out (m). */
#define CITY_FRINGE      3000.0

/* Settlements: the cities and towns on the land, each with a footprint and
 * ground of its own. For now the plan holds one, the megacity, whose
 * footprint is plan_city_edge's; more come with roads out of it. The ground
 * under a settlement rises and falls gently by its relief, larger the smaller
 * the settlement: a megacity is kept nearly flat, a small city can be
 * moderately hilly (plan_settlement_relief). */
#define PLAN_SETTLEMENTS   1
#define PLAN_RELIEF_MEGA_M 10.0    /* relief of a settlement of CITY_RADIUS (m) */
#define PLAN_RELIEF_MAX_M  40.0    /* the most relief any settlement has (m) */
typedef struct PlanSettlement {
    DVec2  centre;     /* world (x, z) of its footprint's centre (m) */
    double radius;     /* its footprint's radius before the warp (m) */
    double relief_m;   /* amplitude of its ground's relief (m) */
} PlanSettlement;
/* Settlement k, 0 <= k < PLAN_SETTLEMENTS. */
PlanSettlement plan_settlement(int k);
/* The settlement whose footprint lies nearest (x, z). */
int    plan_settlement_near(double x, double z);
/* Ground relief of a settlement of this radius: PLAN_RELIEF_MEGA_M at
 * CITY_RADIUS exactly, rising as sqrt(CITY_RADIUS / radius) for smaller ones,
 * to at most PLAN_RELIEF_MAX_M. */
double plan_settlement_relief(double radius);

enum { ROAD_NONE = 0, ROAD_LOCAL, ROAD_ARTERIAL, ROAD_BOULEVARD };
enum { ZONE_CORE = 0, ZONE_CENTRE, ZONE_RESIDENTIAL, ZONE_INDUSTRIAL, ZONE_PARK, ZONE_RURAL, ZONE_COUNT };

/* Attributes of a polygon edge: the road (if any) running along it. */
typedef struct RoadAttr {
    uint32_t id;       /* stable road id, 0 if the edge is not a road */
    float    half;     /* half-width of the carriageway */
    float    walk;     /* sidewalk width either side */
    uint8_t  cls;      /* ROAD_* */
    uint8_t  lamp;     /* 0 sodium, 1 cold LED */
    uint8_t  own;      /* this side emits the shared road (boundary edges) */
    uint8_t  pad;
} RoadAttr;

/* Convex polygon; e[i] describes edge v[i] -> v[i+1]. */
typedef struct Poly {
    int      n;
    DVec2    v[PLAN_MAX_PV];
    RoadAttr e[PLAN_MAX_PV];
} Poly;

typedef struct Road {
    DVec2    a, b;           /* centre line between junction points */
    double   trimA, trimB;   /* carriageway marking stops this far from each end */
    RoadAttr at;
} Road;

typedef struct Block {
    uint32_t seed;
    int      zone;
    float    density;
    float    gc, gs;         /* grain direction (unit) */
    Poly     curb;           /* raised kerb outline */
    Poly     lot;            /* buildable land inside the sidewalks (n == 0 if none) */
    double   x0, z0, x1, z1; /* curb bounds */
} Block;

/* A piece of the outline of the city's ground: the ground lies on the inner
 * side of a -> b, the land beyond on the side of the unit normal n. */
typedef struct PlanEdge { DVec2 a, b, n; } PlanEdge;

typedef struct PlanArea {
    double x0, z0, x1, z1;
    int    nroads, nblocks;
    Road   roads[PLAN_MAX_ROADS];
    Block  blocks[PLAN_MAX_BLOCKS];
    /* bounded 0: the city's ground covers the whole rectangle (ntowns and
     * nedges are 0). bounded 1: it covers exactly the union of ground[],
     * the ground of every built town near the rectangle (convex, edges
     * without roads; they meet without overlapping), and edges[] is its
     * outline wherever that comes near the rectangle. */
    int      bounded, ntowns, nedges;
    Poly     ground[PLAN_MAX_TOWNS];
    PlanEdge edges[PLAN_MAX_EDGES];
} PlanArea;

/* Development density 0..1 (polycentric: peaks at town and regional centres,
 * falling away through the fringe). */
float plan_density(double x, double z);
/* How far into the city's fringe a point is: exactly 0 more than CITY_FRINGE
 * inside the built edge, rising smoothly to 1 at it (and beyond). */
float plan_fringe(double x, double z);
/* Affluence 0 (poor) .. 1 (wealthy): highest in the regional cores, with whole
 * districts and neighbourhoods better or worse off than their density says. */
float plan_affluence(double x, double z);
/* Signed distance-like measure to the city's edge, metres: negative inside,
 * positive outside, continuous, changing by at most CITY_EDGE_RATE per metre
 * (it is a distance in the warped frame, not the true distance). */
double plan_city_edge(double x, double z);
/* Urban weight: 1 inside the city, falling smoothly to 0 across the belt and
 * 0 beyond the edge. Terrain reads it to keep the ground under the city gentle. */
float  plan_urban(double x, double z);
/* The same weight from a plan_city_edge value already in hand: exactly 1 for
 * edge <= -CITY_BELT, exactly 0 for edge >= 0. */
static inline float plan_urban_weight(double edge) {
    float t = clampf((float)((edge + CITY_BELT) / CITY_BELT), 0.0f, 1.0f);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

/* Every road and block whose footprint touches the rectangle. PlanArea is
 * large; allocate it on the heap. */
void  plan_area(double x0, double z0, double x1, double z1, PlanArea *out);

/* Centre of town (i, j) on the town lattice; town (0,0) is a regional centre. */
DVec2 plan_town_centre(int i, int j);
int   plan_town_regional(int i, int j);
/* Whether town (i, j) is part of the city: its cell, and CITY_GROUND_PAD
 * round it, provably lie at or inside CITY_BUILT_EDGE. */
int   plan_town_built(int i, int j);

/* Convex polygon helpers (also used for lots and building placement). */
int    poly_clip(const Poly *in, DVec2 q, DVec2 n, RoadAttr cut, Poly *out);   /* keeps (p - q).n <= 0 */
int    poly_inset(const Poly *in, int mode, double extra, Poly *out);          /* mode: 0 road half, 1 sidewalk, 2 none */
int    poly_contains(const Poly *p, double x, double z, double margin);
double poly_area(const Poly *p);
DVec2  poly_centroid(const Poly *p);
void   poly_bounds(const Poly *p, double *x0, double *z0, double *x1, double *z1);
