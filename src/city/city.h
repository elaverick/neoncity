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
#define MAX_GROUND       8
#define MAX_CURB_PV     PLAN_MAX_PV
#define MAX_DROPS       64
#define MAX_BLOCK_DROPS 24
#define MAX_DROP_PV     16
#define MAX_COLL        4096
#define MAX_COLL_VERTS  20480
#define MAX_FLOORS      128
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
    float    base;      /* where its frame stands, world Y (building_base) */
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
/* A floor walked on at a building at street level, a lobby or an arcology's
 * concourse, or a tread of the steps up to it: its outline (in collv, as
 * CollPoly), at world height y. A floor lies at the building's base, above the
 * street's ground by as much as the plot falls. */
typedef struct FloorPoly { CollPoly at; float y; } FloorPoly;

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
    int      bounded, nground;      /* the city ends near here (PlanArea.bounded): its ground is */
    CurbPoly ground[MAX_GROUND];    /* these outlines, and beyond them the land's (terrain_surface) */
    int      ndrops;
    DropKerb drop[MAX_DROPS];       /* drop kerbs cut into them, touching this chunk */
    int      ncoll, ncollv;         /* what stands at street level: walls, piers, cores, columns */
    CollPoly coll[MAX_COLL];
    int      nfloor;                /* floors inside them, of the buildings in this chunk */
    FloorPoly floor[MAX_FLOORS];
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
    Flame    *flame;     /* flares' fire in this chunk (chunk-local) */
    uint32_t  nflame, capflame;
    Plume    *plume;     /* stacks' smoke and steam in this chunk (chunk-local) */
    uint32_t  nplume, capplume;
    int       xf;        /* emission transform: local frame -> chunk-local */
    float     xc, xs, xx, xz;
    float     xy;        /* ...raised this far: a building's or a lamp's base on the ground (city_ground_y),
                          * which its vertices' heights count from (Vertex.base) */
    int       drape;     /* each vertex is raised by the ground under it: the streets' surfaces
                          * (outside a frame, a vertex's base is the ground under it anyway) */
    int       footing;   /* in a building's frame, walls standing at street level reach down to the
                          * ground under them, which may lie below the building's base (mesh_push) */
    int       grounded;  /* outside a frame: every vertex counts from `ground` (one piece of litter's,
                          * one plant's), not from the ground looked up under it */
    float     ground;
    double    ox, oz;    /* the chunk's origin in the world, for the ground under a vertex */
    float     cell;      /* the drape's cuts (city_drape_cell) and how far a footing reaches */
    float     sink;      /* into the ground (city_footing_sink), for the chunk's ground */
} Mesh;

float    city_height_cap(int cx, int cz);       /* conservative upper bound of anything in the chunk, above its ground */
/* Conservative bounds on the ground under anything of the chunk (overhangs too), world Y (m). */
void     city_ground_range(int cx, int cz, float *lo, float *hi);
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
/* Round the city's outline (where it ends, PlanArea.bounded) a grass verge
 * slopes from the asphalt down to the land over CITY_VERGE_W, to
 * CITY_VERGE_DROP under the city's ground: a little under the land there,
 * which the terrain sinks 0.5 m under it (src/terrain). city_verge_height is
 * its height over city_ground_y at `dist` metres beyond the outline (src/city
 * streets.c draws it; the player walks on it). */
#define CITY_VERGE_W    4.0f
#define CITY_VERGE_DROP 0.55f
float    city_verge_height(float dist);

/* Distant proxies (src/city/proxy.c). Beyond the chunks' VIEW_DISTANCE the
 * city is drawn as boxes: one for each section of every building (src/gen
 * building.c's graph), the box round its footprint in the building's frame,
 * from its bottom to its top. They are the same buildings the chunks build,
 * where the chunks build them, so a building handed from its proxy to its
 * full mesh keeps its place, orientation, height and identity.
 *
 * Proxies are generated in tiles of PROXY_TILE_CHUNKS x PROXY_TILE_CHUNKS
 * chunks. A building belongs to the chunk holding its centre (as in
 * city_layout), and its boxes to that chunk's run of the tile, so a renderer
 * can leave out a chunk it draws in full. Like the chunks, a tile is a pure
 * function of its coordinates. */
#define PROXY_TILE_CHUNKS 4
#define PROXY_TILE        (PROXY_TILE_CHUNKS * (double)CHUNK_SIZE)
#define PROXY_TILE_CELLS  (PROXY_TILE_CHUNKS * PROXY_TILE_CHUNKS)

/* One box, std430 (shaders/proxy/: must match). half and axis hold two halves
 * each (packHalf2x16). */
typedef struct ProxyBox {
    float    x, z;     /* centre, relative to the tile's origin */
    float    y0, y1;   /* bottom and top, world Y */
    uint32_t half;     /* half extents along the box's own x and z */
    uint32_t axis;     /* its own +x axis in the world (the building's) */
    uint32_t seed;     /* the building's */
    uint32_t info;     /* PX_* fields */
} ProxyBox;

/* ProxyBox.info: which chunk of the tile owns it (x + PROXY_TILE_CHUNKS * z);
 * its section's window type, wall palette, bands and fine bays (as the facade's
 * vertex params: shaders/city/facade.glsl); whether its top is lit (ledTop) and
 * open (nothing stands on it); its section's index; the section's bottom above
 * the building's base, in half metres; whether it is an arcology's; whether it
 * is a plant's unit, without windows. */
#define PX_CHUNK_SHIFT    0    /* 4 bits */
#define PX_WIN_SHIFT      4    /* 2 */
#define PX_PALETTE_SHIFT  6    /* 3 */
#define PX_BANDS          (1u << 9)
#define PX_FINE           (1u << 10)
#define PX_LEDTOP         (1u << 11)
#define PX_TOP            (1u << 12)
#define PX_SECTION_SHIFT  13   /* 5 */
#define PX_FLOOR_SHIFT    18   /* 11 */
#define PX_ARCOLOGY       (1u << 29)
#define PX_PLAIN          (1u << 30)   /* a plant's unit or deck: painted steel, no windows */

/* Level of detail: within each chunk's run the boxes come largest first (by
 * their largest side), and lod[k][b] counts chunk k's boxes at least
 * PROXY_LOD_SIZE(b) metres, so a renderer drawing a far tile can draw each
 * chunk's first few, leaving out those too small to see. */
#define PROXY_LOD_BINS    8
#define PROXY_LOD_SIZE(b) ((float)(1 << (b)))   /* 1, 2, 4 ... 128 m */

typedef struct ProxyTile {
    int       tx, tz;                      /* the tile; its origin is (tx, tz) * PROXY_TILE */
    int       n, cap;
    ProxyBox *box;                         /* chunk k's boxes are box[first[k]] .. box[first[k + 1] - 1] */
    int       first[PROXY_TILE_CELLS + 1];
    uint16_t  lod[PROXY_TILE_CELLS][PROXY_LOD_BINS];
    int       nbuild;
    float     mn[3], mx[3];                /* bounds (n > 0): x and z relative to the origin, world Y */
} ProxyTile;
/* A box's size for its level of detail: its largest side (m). */
float    proxy_box_size(const ProxyBox *b);

/* Fills t with tile (tx, tz)'s proxies, reusing its storage. */
void     city_proxy_tile(int tx, int tz, ProxyTile *t);
/* Whether tile (tx, tz) provably holds no proxies (it lies beyond the city):
 * cheap, conservative. */
int      city_proxy_tile_empty(int tx, int tz);
void     proxy_tile_free(ProxyTile *t);

/* Facade rhythm shared with the shader (shaders/common.glsl: facadeFloorH/facadeGroundH). */
float    city_floor_height(uint32_t seed);
float    city_ground_height(uint32_t seed);
/* Lit storefronts, shared with the shader (shopHash in shaders/city.frag). A
 * bay wrapping a convex corner (endShare) is keyed as the next face's first. */
uint32_t city_shop_hash(uint32_t seed, int section, int face, int seg, float faceWidth, int nextFace, int endShare);
int      city_shop_lit(uint32_t h, int level);
