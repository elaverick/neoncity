#pragma once
#include "common.h"
#include "plan.h"

/* Deterministic city generation.
 *
 * The plan (src/plan/plan.c) lays out towns, neighbourhoods, roads and zoned
 * blocks over the whole world. This file fills blocks with buildings that
 * conform to them (oriented to their streets or the neighbourhood grain) and
 * turns everything that touches a chunk into a mesh.
 *
 * Every function here is a pure function of the chunk coordinates (and the
 * compile-time WORLD_SEED). Nothing depends on visit order, timing, thread or
 * any mutable state, so a chunk is rebuilt bit-identically whenever it is
 * streamed back in. Chunks are only streaming tiles: a building belongs to
 * the chunk containing its centre and may overhang its neighbours by up to
 * CITY_OVERHANG metres. */

#define MAX_BUILDINGS   128
#define MAX_LAMPS       96
#define MAX_CURBS       48
#define MAX_CURB_PV     PLAN_MAX_PV
#define MAX_DROPS       64
#define MAX_BLOCK_DROPS 24
#define MAX_DROP_PV     16
#define MAX_COLL        4096
#define MAX_COLL_VERTS  20480
#define CITY_OVERHANG   70.0f

typedef struct Rect { float x0, z0, x1, z1; } Rect;

enum { BK_BLOCK = 0, BK_TOWER = 1, BK_MEGA = 2 };
enum { SIDE_W = 1, SIDE_E = 2, SIDE_S = 4, SIDE_N = 8 };

/* A building's plot: an axis-aligned site rectangle in its own frame, placed
 * in the world by a rotation (c, s) about its origin: world = o + (c*x - s*z,
 * s*x + c*z). src/gen/building.c generates the building itself inside it. */
typedef struct Building {
    Rect     foot;      /* site rectangle in the local frame */
    double   ox, oz;    /* world position of the local origin (the footprint centre) */
    float    c, s;      /* local +x axis in world space */
    float    height;
    uint32_t seed;
    int      kind;
    int      frontage;  /* SIDE_* bits for faces that look onto a street */
    int      shop;      /* 0..7: share of lit ground-floor shops, in eighths */
    int      zone;
    float    density;   /* plan density of its block */
} Building;

typedef struct Lamp {
    double x, z;        /* pole, world */
    float  dx, dz;      /* arm direction (towards the carriageway) */
    int    type;        /* 0 sodium, 1 cold LED */
} Lamp;

typedef struct CurbPoly { int n; double v[MAX_CURB_PV][2]; } CurbPoly;

/* A drop kerb: where a crosswalk meets a sidewalk, the kerb is cut down to
 * DROP_KERB_HEIGHT for the width of the crossing, and the paving behind it
 * slopes back up to the sidewalk over DROP_RAMP_RUN, between kerb-stone
 * sides. Its frame: o on the kerb line, t along it, n inward; d = (p - o).n
 * is the distance in from the kerb. The outline is convex (world), its edges
 * labelled DE_*. Tactile paving covers the foot of the slope in whole slabs
 * (DROP_TILE), in rows of slabs from u = tu[r][0] to tu[r][1] along t,
 * measured from tc, the row r being d = KERB_STONE_WIDTH + r * DROP_TILE
 * to the next. */
enum { DE_KERB = 0, DE_BACK, DE_SIDE, DE_OPEN };   /* lowered kerb, top of the slope, kerb-stone side, open to the next drop */
#define DROP_TILE       0.4
#define DROP_ROWS       2
typedef struct DropKerb {
    int     edge;                 /* the block's curb edge it is cut into */
    double  ox, oz, tx, tz, nx, nz;
    int     n;
    double  v[MAX_DROP_PV][2];
    uint8_t lab[MAX_DROP_PV];     /* edge v[i] -> v[i + 1] */
    double  tc, tu[DROP_ROWS][2];
    int     nopen;                /* the corners it shares: keeps p.x * open[i][0] + p.z * open[i][1] <= open[i][2] */
    double  open[4][3];
} DropKerb;

/* Solid ground-level outline (world coordinates in BlockLayout.collv), with an optional courtyard. */
typedef struct CollPoly { int first, n, holeFirst, holeN; } CollPoly;

/* Everything the player collides with in one chunk (world coordinates). */
typedef struct BlockLayout {
    int      cx, cz;
    float    density;               /* plan density at the chunk centre */
    int      nbuild;
    Building build[MAX_BUILDINGS];  /* buildings whose centre is in this chunk */
    int      nlamps;
    Lamp     lamp[MAX_LAMPS];       /* lamp poles standing in this chunk */
    int      ncurbs;
    CurbPoly curb[MAX_CURBS];       /* raised kerb outlines touching this chunk */
    int      ndrops;
    DropKerb drop[MAX_DROPS];       /* drop kerbs cut into them, touching this chunk */
    int      ncoll, ncollv;         /* what stands at street level: walls, piers, cores, columns */
    CollPoly coll[MAX_COLL];
    double   collv[MAX_COLL_VERTS][2];
} BlockLayout;

typedef struct Mesh {
    Vertex  *v;
    uint32_t count, cap;
    float    mn[3], mx[3];
    uint32_t *lights;    /* light tile records (4 words each), see city_build_mesh */
    uint32_t  nlights;
    SkyLight *sky;       /* signs high enough to light the clouds (chunk-local) */
    uint32_t  nsky, capsky;
    SkyLight *air;       /* lamps, shopfronts and signs in this chunk, for the rain mist (chunk-local) */
    uint32_t  nair, capair;
    int       xf;        /* emission transform: local frame -> chunk-local */
    float     xc, xs, xx, xz;
    float     xy;        /* ...raised this far: a building's or a lamp's base on the ground (city_ground_y) */
    int       drape;     /* each vertex is raised by the ground under it: the streets' surfaces */
    double    ox, oz;    /* the chunk's origin in the world, for the ground under a vertex */
} Mesh;

float    city_height_cap(int cx, int cz);       /* conservative upper bound of anything in the chunk */
void     city_layout(int cx, int cz, BlockLayout *out);
void     city_build_mesh(int cx, int cz, Mesh *m);
/* The near-only layer: real office lobbies of the buildings centred in the chunk. */
void     city_build_interiors(int cx, int cz, Mesh *m);
/* The plants in those lobbies' planters (src/gen/plant.c), a layer of their own:
 * a leaf is a single surface, drawn without back-face culling. */
void     city_build_plants(int cx, int cz, Mesh *m);
/* The litter layer (src/city/litter.c): rubbish lying in the streets, alleys and
 * doorways of the chunk. It has no light table of its own; the shader uses the chunk's. */
void     city_build_litter(int cx, int cz, Mesh *m);
/* How squalid the streets are at (x, z) in a block of `zone` (-1: on a road), 0..1:
 * how much rubbish lies about (src/city/litter.c) and how much fine debris the ground shader draws. */
float    city_squalor(int zone, double x, double z);
void     mesh_free(Mesh *m);
/* Whether a triangle with this vertex casts ray-traced shadows (src/render/rt.c).
 * Glass, veils of light, puddles and contact shadows don't; nor does flat or
 * wind-stirred litter, whose shape the vertex shader changes. */
int      city_casts_shadow(const Vertex *v);

/* Buildings filling one plan block (all of them, wherever their centres are). */
int      city_block_buildings(const Block *bk, Building *out, int max);
/* Drop kerbs cut into a block's sidewalks (all of them), one at each side of
 * every crosswalk. Uses the roads of the plan area along the block's edges. */
int      city_block_drops(const PlanArea *pa, const Block *bk, DropKerb *out, int max);
/* Whether (x, z) lies in a drop kerb's outline (grown by margin), and the
 * height of its ground there, above city_ground_y. */
int      city_drop_contains(const DropKerb *d, double x, double z, double margin);
float    city_drop_height(const DropKerb *d, double x, double z);
/* Lamp poles along road `ri` of a plan area, limited to a rectangle. */
int      city_road_lamps(const PlanArea *pa, int ri, double x0, double z0, double x1, double z1, Lamp *out, int max);
/* A spot on a sidewalk in the central regional centre, facing along the street. */
void     city_spawn(double *x, double *z, float *yaw);
/* The ground the city stands on at world (x, z), world Y (m): the height the
 * city's own heights count from. Marked carriageways lie on it, sidewalks
 * SIDEWALK_HEIGHT above it, and a building's frame (src/gen, from G0) is
 * raised by it at the building's base. It is the terrain's urban ground
 * (terrain_urban_height in src/terrain). */
float    city_ground_y(double x, double z);

/* Facade rhythm shared with the shader (shaders/common.glsl: facadeFloorH/facadeGroundH). */
float    city_floor_height(uint32_t seed);
float    city_ground_height(uint32_t seed);
/* Lit storefronts, shared with the shader (shopHash in shaders/city.frag). A
 * bay wrapping a convex corner (endShare) is keyed as the next face's first. */
uint32_t city_shop_hash(uint32_t seed, int section, int face, int seg, float faceWidth, int nextFace, int endShare);
int      city_shop_lit(uint32_t h, int level);
