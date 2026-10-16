#pragma once
#include "common.h"
#include "geom2d.h"

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
 *                  industrial, park) from density and distance to the centres.
 *
 * Nothing here depends on query order or region: a road or block comes out
 * identical whichever area it is requested through. Coordinates are world
 * metres in double precision. */

#define TOWN_SIZE        1500.0
#define NBHD_SIZE         430.0
#define PLAN_MAX_PV        24
#define PLAN_MAX_ROADS    512
#define PLAN_MAX_BLOCKS   384

enum { ROAD_NONE = 0, ROAD_LOCAL, ROAD_ARTERIAL, ROAD_BOULEVARD };
enum { ZONE_CORE = 0, ZONE_CENTRE, ZONE_RESIDENTIAL, ZONE_INDUSTRIAL, ZONE_PARK };

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

typedef struct PlanArea {
    double x0, z0, x1, z1;
    int    nroads, nblocks;
    Road   roads[PLAN_MAX_ROADS];
    Block  blocks[PLAN_MAX_BLOCKS];
} PlanArea;

/* Development density 0..1 (polycentric: peaks at town and regional centres). */
float plan_density(double x, double z);
/* Affluence 0 (poor) .. 1 (wealthy): highest in the regional cores, with whole
 * districts and neighbourhoods better or worse off than their density says. */
float plan_affluence(double x, double z);

/* Every road and block whose footprint touches the rectangle. PlanArea is
 * large; allocate it on the heap. */
void  plan_area(double x0, double z0, double x1, double z1, PlanArea *out);

/* Centre of town (i, j) on the town lattice; town (0,0) is a regional centre. */
DVec2 plan_town_centre(int i, int j);
int   plan_town_regional(int i, int j);

/* Convex polygon helpers (also used for lots and building placement). */
int    poly_clip(const Poly *in, DVec2 q, DVec2 n, RoadAttr cut, Poly *out);   /* keeps (p - q).n <= 0 */
int    poly_inset(const Poly *in, int mode, double extra, Poly *out);          /* mode: 0 road half, 1 sidewalk, 2 none */
int    poly_contains(const Poly *p, double x, double z, double margin);
double poly_area(const Poly *p);
DVec2  poly_centroid(const Poly *p);
void   poly_bounds(const Poly *p, double *x0, double *z0, double *x1, double *z1);
