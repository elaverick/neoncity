#pragma once
#include "common.h"
#include "plan.h"

/* Deterministic city generation.
 *
 * The plan (src/plan.c) lays out towns, neighbourhoods, roads and zoned
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
#define MAX_COLL        384
#define MAX_COLL_VERTS  8192
#define CITY_OVERHANG   70.0f

typedef struct Rect { float x0, z0, x1, z1; } Rect;

enum { BK_BLOCK = 0, BK_TOWER = 1, BK_MEGA = 2 };
enum { SIDE_W = 1, SIDE_E = 2, SIDE_S = 4, SIDE_N = 8 };

/* A building's plot: an axis-aligned site rectangle in its own frame, placed
 * in the world by a rotation (c, s) about its origin: world = o + (c*x - s*z,
 * s*x + c*z). src/building.c generates the building itself inside it. */
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
    int       xf;        /* emission transform: local frame -> chunk-local */
    float     xc, xs, xx, xz;
} Mesh;

float    city_height_cap(int cx, int cz);       /* conservative upper bound of anything in the chunk */
void     city_layout(int cx, int cz, BlockLayout *out);
void     city_build_mesh(int cx, int cz, Mesh *m);
void     mesh_free(Mesh *m);

/* Buildings filling one plan block (all of them, wherever their centres are). */
int      city_block_buildings(const Block *bk, Building *out, int max);
/* Lamp poles along road `ri` of a plan area, limited to a rectangle. */
int      city_road_lamps(const PlanArea *pa, int ri, double x0, double z0, double x1, double z1, Lamp *out, int max);
/* A spot on a sidewalk in the central regional centre, facing along the street. */
void     city_spawn(double *x, double *z, float *yaw);

/* Facade rhythm shared with the shader (shaders/common.glsl: facadeFloorH/facadeGroundH). */
float    city_floor_height(uint32_t seed);
float    city_ground_height(uint32_t seed);
/* Lit storefronts, shared with the shader (shopHash in shaders/city.frag). A
 * bay wrapping a convex corner (endShare) is keyed as the next face's first. */
uint32_t city_shop_hash(uint32_t seed, int section, int face, int seg, float faceWidth, int nextFace, int endShare);
int      city_shop_lit(uint32_t h, int level);
