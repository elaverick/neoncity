#pragma once
#include "common.h"
#include "geom2d.h"
#include "mathlib.h"
#include "terrain.h"   /* PLANET_RADIUS, TerrainDir, TerrainChart */

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
#define PLAN_MAX_GATES      8

/* A city is finite: a disc, its edge warped by two octaves of value noise so
 * it doesn't read as a circle (plan_world.c). Inside the edge, a belt where
 * the city gives way to the land around it. The warp's octaves are given for
 * a city of CITY_RADIUS, the footprint of one built 20 km across; a bigger
 * city's warp is as much bigger, amplitudes and wavelengths alike, so its
 * edge changes no faster; a smaller one's amplitudes only shrink. Metres. */
#define CITY_RADIUS     12500.0
#define CITY_BELT        1500.0
#define CITY_WARP1_M     1800.0   /* warp amplitude and wavelength, first octave, at CITY_RADIUS */
#define CITY_WARP1_WAVE  6000.0
#define CITY_WARP2_M      500.0   /* second octave */
#define CITY_WARP2_WAVE  2000.0
/* Bound on how fast plan_city_edge changes, metres per metre. Each warp
 * component's gradient is at most 3*sqrt(2)*(M1/WAVE1 + M2/WAVE2) (value
 * noise: 1.5 per lattice unit per axis), so the warp's Jacobian has norm at
 * most 6*(...), and the edge (a length through it) changes by at most 1 plus
 * that; the coast and the slopes cut it by a bilinear distance, which
 * changes by at most sqrt 2, and a lake near by its exact distance, falling
 * away at most twice as fast. A little over, for float rounding. */
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

/* Settlements: the cities on the land, each with a footprint and ground of
 * its own (src/plan/plan_world.c). They are the world's largest metropolitan
 * areas, read at plan_world_init from PLAN_CITY_LIST (rank, name, latitude,
 * longitude, and the real urban centre's area from the GHSL: earth.py
 * cities). Settlement 0 is Los Angeles, the megacity the player starts in,
 * its core town (0, 0) at the world chart's origin; the rest follow by rank.
 * Every one is a megacity of the future, far bigger than the city it was: its
 * size across falls with the log of its real area, from PLAN_CITY_MAX_KM for
 * the largest in the list to PLAN_CITY_MIN_KM for the smallest (or a city
 * the GHSL lacks). Cities whose discs would overlap are one conurbation,
 * chained, with the best rank's name and the summed area of its parts' urban
 * centres (each counted once), at most PLAN_CITY_MAX_KM.
 *
 * Each settlement has its own chart, about its centre with north up, and its
 * footprint is measured in it: a warped disc, cut back to PLAN_COAST_M from
 * the shore (terrain_shore), to PLAN_LAKE_M from a lake's (terrain_earth_shore)
 * and to PLAN_STEEP_REACH_M into ground steeper
 * than PLAN_STEEP_GRADE (the Earth read at mip 0). A footprint the cut leaves
 * short is pushed away from it, and grown if it must be, until it keeps its
 * size; two footprints never meet. The megacity is cut and grown too, but its
 * core stays where the player starts: only its footprint's disc moves
 * (`disc`). The ground under a settlement rises and falls gently by its
 * relief, larger the smaller the settlement (plan_settlement_relief), on the
 * Earth's highlands.
 *
 * The city itself is still laid out in the world chart, X/Z (terrain.h, "The
 * planet"), where settlement 0's chart is the world's: the plan's towns of a
 * far settlement are its footprint's image there. */
#define PLAN_MAX_SETTLEMENTS 256
#define PLAN_CITY_LIST     "data/earth/largest_metropolitan_areas_227.csv"
#define PLAN_CITY_MIN_KM   60.0    /* the smallest city's built size across (km)... */
#define PLAN_CITY_MAX_KM   150.0   /* ...and the largest's */
#define PLAN_COLOSSAL_RANK 50      /* the hubs of the roads: this rank and better */
#define PLAN_COAST_M       1000.0  /* footprints stop this far from the shore (m)... */
#define PLAN_LAKE_M        200.0   /* ...and this far from a lake's (m) */
#define PLAN_STEEP_GRADE   0.08    /* ground steeper than this (the Earth at mip 0, 2.4 km) cuts a footprint... */
#define PLAN_STEEP_CLOSE_M 3000.0  /* ...its patches closed over gaps of twice this, so a range is one (m)... */
#define PLAN_STEEP_REACH_M 2000.0  /* ...this far into it (m) */
#define PLAN_RELIEF_MEGA_M 10.0    /* relief of a settlement of CITY_RADIUS (m) */
#define PLAN_RELIEF_MAX_M  40.0    /* the most relief any settlement has (m) */
typedef struct PlanSettlement {
    DVec2  centre;     /* world (x, z) of its core: a town's centre (m) */
    DVec2  disc;       /* world (x, z) of its footprint's disc's centre: the core's but for the megacity's (m) */
    double radius;     /* its footprint's radius before the warp (m) */
    double relief_m;   /* amplitude of its ground's relief (m) */
    int    town[2];    /* the town (i, j) at its core, a regional centre */
    int    colossal;   /* ranked PLAN_COLOSSAL_RANK or better */
    int    continent;  /* which land its roads join it to (plan_world_continent), 0 for none */
    double size_km;    /* its built size across, from its area (km) */
    double area_km2;   /* the real urban area it stands for (GHSL, its parts' summed), 0 where unknown */
    int    rank;       /* its rank in the list (a conurbation's best) */
    TerrainDir   dir;     /* its centre, in the planet frame (unit) */
    TerrainChart chart;   /* its own chart, about dir, north up; settlement 0's is the world's */
} PlanSettlement;
/* Places the settlements and builds the roads between them (plan_roads.c),
 * on up to `threads` threads; a pure function of WORLD_SEED and the list.
 * Call it once, before anything else asks the plan or the terrain for
 * anything from more than one thread: until it has run, the first such call
 * runs it on one. */
void   plan_world_init(int threads);
/* How many settlements there are, and settlement k, 0 <= k < that, and its name. */
int    plan_settlement_count(void);
PlanSettlement plan_settlement(int k);
const char *plan_settlement_name(int k);
/* The settlement whose footprint lies nearest (x, z), among those within
 * PLAN_EDGE_CAP of it; the megacity, 0, where none is. */
int    plan_settlement_near(double x, double z);
/* Ground relief of a settlement of this radius: PLAN_RELIEF_MEGA_M at
 * CITY_RADIUS exactly, rising as sqrt(CITY_RADIUS / radius) for smaller ones,
 * to at most PLAN_RELIEF_MAX_M. */
double plan_settlement_relief(double radius);
/* The least and the most relief of any settlement (m). */
void   plan_settlement_relief_range(double *lo, double *hi);
/* The least and the most relief of the settlements plan_settlement_near can
 * give anywhere in the rectangle [x0, x1] x [z0, z1] (m). */
void   plan_settlement_relief_in(double x0, double z0, double x1, double z1, double *lo, double *hi);
/* The footprint's radius for a settlement built `km` across: half that and
 * 2.5 km (the belt and the fringe's outer towns), CITY_RADIUS at 20 km. */
double plan_settlement_radius(double km);
/* The size across a settlement of real urban area `km2` is built (km), in a
 * list whose areas run from lo to hi (km^2): PLAN_CITY_MIN_KM at lo and
 * below, PLAN_CITY_MAX_KM at hi and above, with the log of the area between. */
double plan_settlement_km(double km2, double lo, double hi);
/* The farthest from the world chart's origin the megacity's edge measure is
 * below 0 (m): beyond, plan_urban is 0 but near another settlement. */
double plan_home_reach(void);
/* The roads between the settlements are laid out in the world chart (until
 * they are on the sphere), over PLAN_ROAD_REACH of its origin, where the
 * chart's distances are within a tenth of the sphere's: a settlement farther
 * out has no roads yet. On the Earth that is North America, from Mexico City
 * to Boston; the next city out, Bogotá, is 5600 km from Los Angeles. */
#define PLAN_ROAD_REACH   5000000.0   /* m */
/* Which land (x, z) stands on: a continent's number, from 1, or 0 where no
 * road may go (plan_road_dry) and beyond the roads' reach. Continents are
 * told apart on a grid of PLAN_SEA_CELL, so land joined only across what no
 * road may cross (a strait) is two. */
#define PLAN_SEA_CELL 20000.0
#define PLAN_ROAD_SEA_M 20.0   /* roads stay this far above the sea to come (terrain_sea_margin, m) */
int    plan_world_continent(double x, double z);
/* terrain_sea_margin at (x, z), read from that grid: interpolated, and -1e9
 * beyond the roads' reach (m). */
double plan_world_sea(double x, double z);
/* Whether a road may stand at (x, z) for the sea to come: PLAN_ROAD_SEA_M
 * above it on that grid, or, where the grid's 20 km take a coast's lowlands
 * for the sea, PLAN_ROAD_SHORE_M above the Earth's finer shore
 * (terrain_shore); and within PLAN_EDGE_CAP of a settlement anywhere on land
 * (the shore above 0), as its coast was cut, so the roads reach the cities
 * on the coastal plains. */
#define PLAN_ROAD_SHORE_M 5.0
int    plan_road_dry(double x, double z);

/* Roads between the settlements (src/plan/plan_roads.c): on each continent
 * a network joins every settlement, designed once at plan_world_init. Its
 * links run between nodes, the settlements (node k is settlement k) and the
 * junctions where roads branch or cross, each a freeway where the gravity
 * model sends the most traffic, a trunk road elsewhere. A road's centre line
 * runs round mountains, canyons, other cities and the sea to come. */
enum { PLAN_ROAD_TRUNK = 1, PLAN_ROAD_FREEWAY };
#define PLAN_FREEWAY_HALF 16.0f   /* half the paved width (m): two carriageways of three lanes, a median, shoulders */
#define PLAN_TRUNK_HALF    6.0f   /* two lanes and shoulders */
typedef struct PlanNode {
    DVec2 p;            /* world (x, z): a settlement's centre, or the junction */
    int   settlement;   /* its settlement, or -1 for a junction */
} PlanNode;
/* Where a road meets its settlement: on the perimeter boulevard's centre line,
 * the road's last stretch square to it. */
typedef struct PlanGate {
    DVec2 p;            /* on the boulevard's centre line */
    DVec2 n;            /* the way out of the settlement, square to the boulevard (unit) */
    float half;         /* the road's half-width */
    int   road;         /* which road */
} PlanGate;
/* A road's surface height is its profile, every `ds` metres along it
 * (PLAN_ROAD_STATION, a little less to fit), interpolated linearly: the
 * ground along it smoothed, its grade held to PLAN_FREEWAY_GRADE or
 * PLAN_TRUNK_GRADE, meeting its city's ground at a gate and every road's at
 * a junction. */
#define PLAN_ROAD_STATION  50.0
#define PLAN_FREEWAY_GRADE 0.06
#define PLAN_TRUNK_GRADE   0.09
typedef struct PlanRoad {
    int           from, to;   /* nodes */
    int           cls;        /* PLAN_ROAD_* */
    float         half;       /* half the paved width (m) */
    double        flow;       /* the traffic the gravity model sends along it (relative) */
    int           npts;
    const DVec2  *pts;        /* its centre line, world (x, z): from `from` (its gate, at a settlement) to `to` */
    const double *s;          /* distance along it at each point (m) */
    int           gate[2];    /* whether it ends at a gate at `from`, `to` (then pts begins or ends there) */
    int           nprof;
    double        ds;
    const float  *prof;       /* nprof heights, the first at s = 0, the last at its length (world Y, m) */
} PlanRoad;
int             plan_node_count(void);
PlanNode        plan_node(int k);
int             plan_road_count(void);
const PlanRoad *plan_road(int k);
/* Road k's gate at its from (end 0) or to (end 1) end; only if it has one. */
PlanGate        plan_road_gate(int k, int end);
/* Road k's surface height s metres along it. */
double          plan_road_height(int k, double s);
/* The roads as the ground takes them (terrain_height): where the nearest
 * road's surface reaches out to over (x, z), the weight in (0, 1] the
 * ground there takes it with (1 on the road and its shoulders, falling away
 * over a batter as wide as twice the cut or fill), its height into *road_h.
 * `ground` is the ground there without the road, `spacing` how coarsely it
 * is sampled (the batter widens to cover it; past PLAN_ROAD_LOD_SPACING
 * the roads are left out, 0). */
#define PLAN_ROAD_LOD_SPACING 64.0
#define PLAN_ROAD_SHOULDER    2.0    /* m of level ground beyond the paved width */
#define PLAN_ROAD_BATTER_MIN  6.0    /* the batter's least and greatest width (m) */
#define PLAN_ROAD_BATTER_MAX  200.0  /* a cutting or embankment 100 m deep or more still slopes no steeper than 1.5 / 2 a metre */
double          plan_road_ground(double x, double z, double ground, double spacing, double *road_h);
/* Bounds on the road heights plan_road_ground can give in the rectangle
 * [x0, x1] x [z0, z1]; 0 if it gives none there. */
int             plan_road_range(double x0, double z0, double x1, double z1, double *lo, double *hi);

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
    uint8_t  bank;     /* 1: not a road but a river's bank, the top of its channel's wall (PlanChannel) */
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

/* Rivers through the cities (src/plan/plan_water.c). Wherever one of the
 * bake's rivers (terrain_earth_rivers_near) runs through a built town it runs
 * in a concrete channel, its floor PLAN_CHANNEL_DEPTH under the city's ground
 * and its walls sheer, as wide as the river (PLAN_CHANNEL_MIN_W at least).
 * Across each neighbourhood it runs straight, from the point where the river
 * crosses the road it comes in by to the point where it crosses the road it
 * leaves by, so it splits the neighbourhood in two along its banks, and
 * every arterial and boulevard it passes under is a bridge; the local
 * streets end at its banks, and a promenade PLAN_BANK_WALK wide runs along
 * them. A river that crosses back into a neighbourhood it has left runs
 * straight on through it instead. Its channel turns under the bridges; where
 * the river begins or ends within a neighbourhood (a source, or where it
 * meets another), the channel ends at a wall under the last bridge. Out of
 * the built towns, across the belt round the city, it follows the river and
 * the ground carves its bed (terrain_height) as for the other water.
 *
 * Each piece of the channel is a PlanChannel: a convex outline (its floor),
 * whose edges are its banks (CHANNEL_EDGE_BANK, a wall with a parapet over
 * it), its ends at a road (CHANNEL_EDGE_ROAD, under the road: a wall only
 * where no other piece of channel meets it) or open (CHANNEL_EDGE_OPEN, out
 * in the belt, where the next piece goes on). A bridge (PlanBridge) carries
 * a road and its sidewalks over the channel where the road crosses it. */
#define PLAN_CHANNEL_DEPTH  5.0    /* m under the city's ground */
#define PLAN_CHANNEL_MIN_W  12.0   /* the narrowest channel (m) */
#define PLAN_BANK_WALK      4.0    /* the promenade along a bank (m) */
#define PLAN_CHANNEL_PV     12
#define PLAN_MAX_CHANNELS   96
#define PLAN_MAX_BRIDGES    32
enum { CHANNEL_EDGE_OPEN = 0, CHANNEL_EDGE_BANK, CHANNEL_EDGE_ROAD };
typedef struct PlanChannel {
    int      n;
    DVec2    v[PLAN_CHANNEL_PV];      /* its floor's outline (convex), world (x, z) */
    uint8_t  edge[PLAN_CHANNEL_PV];   /* edge v[i] -> v[i + 1]: CHANNEL_EDGE_* */
    DVec2    a, b;                    /* its centre line, downstream */
    float    hw;                      /* half its width (m) */
    uint8_t  built;                   /* in a built town: walls and a floor of concrete; else the ground's bed */
    uint32_t id;                      /* stable */
    double   x0, z0, x1, z1;          /* bounds */
} PlanChannel;
typedef struct PlanBridge {
    DVec2    p;          /* where the road's centre line crosses the river */
    DVec2    d;          /* along the road (unit) */
    double   s0, s1;     /* the channel under its deck lies between p + s0 d and p + s1 d (m) */
    float    half, walk; /* the road's half-width and its sidewalks' width */
    uint8_t  cls;        /* ROAD_ARTERIAL or ROAD_BOULEVARD */
    uint32_t road;       /* the road's id */
} PlanBridge;
/* How far beyond a channel's outline the ground is cut for it, at most (m):
 * plan_channel_dist reads no further. */
#define PLAN_CHANNEL_REACH 120.0
/* The signed distance (m) from (x, z) to the nearest channel's outline,
 * negative in it, the outlines across the towns grown by `grow` (m, under
 * PLAN_CHANNEL_REACH); more than PLAN_CHANNEL_REACH less that where none is
 * that near. */
double plan_channel_dist(double x, double z, double grow);
/* Whether a channel's outline comes within PLAN_CHANNEL_REACH of the rectangle. */
int    plan_channel_in(double x0, double z0, double x1, double z1);
/* How many pieces of channel and bridges there are in all; channel k and bridge k. */
int    plan_channel_count(void);
int    plan_bridge_count(void);
const PlanChannel *plan_channel(int k);
const PlanBridge  *plan_bridge(int k);

typedef struct PlanArea {
    double x0, z0, x1, z1;
    int    nroads, nblocks;
    Road   roads[PLAN_MAX_ROADS];
    Block  blocks[PLAN_MAX_BLOCKS];
    /* bounded 0: the city's ground covers the whole rectangle (ntowns and
     * nedges are 0). bounded 1: it covers exactly the union of ground[],
     * the ground of every built town near the rectangle (convex, edges
     * without roads; they meet without overlapping), and edges[] is its
     * outline wherever that comes near the rectangle, and gates[] where a
     * road between the settlements leaves it near the rectangle. */
    int      bounded, ntowns, nedges, ngates;
    Poly     ground[PLAN_MAX_TOWNS];
    PlanEdge edges[PLAN_MAX_EDGES];
    PlanGate gates[PLAN_MAX_GATES];
    /* the pieces of channel and the bridges near the rectangle (within
     * PLAN_CHANNEL_AREA_PAD), in order of id */
    int         nchannels, nbridges;
    PlanChannel channels[PLAN_MAX_CHANNELS];
    PlanBridge  bridges[PLAN_MAX_BRIDGES];
} PlanArea;
#define PLAN_CHANNEL_AREA_PAD 60.0

/* Development density 0..1 (polycentric: peaks at town and regional centres,
 * falling away through the fringe). */
float plan_density(double x, double z);
/* How far into the city's fringe a point is: exactly 0 more than CITY_FRINGE
 * inside the built edge, rising smoothly to 1 at it (and beyond). */
float plan_fringe(double x, double z);
/* Affluence 0 (poor) .. 1 (wealthy): highest in the regional cores, with whole
 * districts and neighbourhoods better or worse off than their density says. */
float plan_affluence(double x, double z);
/* Signed distance-like measure to the nearest settlement's edge, metres:
 * negative inside, positive outside, continuous, changing by at most
 * CITY_EDGE_RATE per metre (it is a distance in the warped frame, not the
 * true distance), and never more than PLAN_EDGE_CAP: nothing reads it further
 * out. */
#define PLAN_EDGE_CAP 20000.0
double plan_city_edge(double x, double z);
/* The megacity's measure alone, uncapped. */
double plan_home_edge(double x, double z);
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

/* Centre of town (i, j) on the town lattice; town (0,0) is a regional centre,
 * and so is every settlement's centre town. */
DVec2 plan_town_centre(int i, int j);
int   plan_town_regional(int i, int j);
/* The town (i, j) whose cell holds (x, z). */
void  plan_town_at(double x, double z, int *i, int *j);
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
