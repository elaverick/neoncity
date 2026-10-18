#pragma once
#include "city.h"
#include "building.h"
#include "interior.h"
#include "arcology.h"
#include "plant.h"
#include "hash.h"
#include "mathlib.h"
#include "geom2d.h"

/* Shared between the files that generate a chunk (src/city/). Not part of
 * the public interface in city.h.
 *
 *   city.c           entry points (city_build_mesh, _interiors, _plants), shared rules, spawn
 *   chunk_gen.c      everything near one chunk: plan area, buildings, lamps
 *   lots.c           filling plan blocks with building plots
 *   drops.c          drop kerbs at the crosswalks
 *   lamps.c          street lamp positions
 *   collision.c      what the player collides with (city_layout)
 *   mesh_emit.c      mesh emission helpers: quads, boxes, frames, faces, sign lights
 *   mesh_building.c  a building graph's facades, roofs, signs and doors
 *   mesh_lobby.c     office lobbies (src/gen/interior.c) realised as geometry
 *   mesh_planting.c  the lobbies' planters and the plants grown in them
 *   mesh_arcology.c  arcology concourse levels (src/gen/arcology.c)
 *   streets.c        roads, sidewalks, kerbs, drop-kerb slopes, lamp posts
 *   light_tiles.c    each chunk's light table
 *   cables.c         overhead cables
 *   litter.c         the litter layer */

/* ---------------------------------------------------------------------------
 * Everything near one chunk (chunk_gen.c): the plan round it, the buildings
 * of the blocks near it and the lamps along its roads.
 * ------------------------------------------------------------------------- */

typedef struct ChunkGen {
    int       cx, cz;
    double    ox, oz;       /* chunk origin, world */
    PlanArea *pa;
    Building *bld;
    int      *bblk;         /* block index of each building */
    int       nb, capb;
    Lamp     *lamp;
    int       nl, capl;
    BGraph  **graph;        /* building graphs, generated on demand */
} ChunkGen;

/* planPad: plan area margin. Buildings are generated for blocks within
 * buildPad of the chunk and lamps within lampPad; lampPad must stay 20 m
 * inside planPad so every road and kerb a lamp is checked against is known. */
void          chunk_gen_init(ChunkGen *g, int cx, int cz, double planPad, double buildPad, double lampPad);
void          chunk_gen_free(ChunkGen *g);
/* Building i's graph, generated when first asked for. */
const BGraph *chunk_gen_graph(ChunkGen *g, int i);
/* Whether (x, z) lies in this chunk, which then owns what stands there. */
int           chunk_gen_owns(const ChunkGen *g, double x, double z);
/* Whether a world rectangle comes within pad of the chunk. */
int           chunk_gen_near(const ChunkGen *g, double x0, double z0, double x1, double z1, double pad);

/* A building's plot frame. */
static inline DVec2 to_world(const Building *b, double lx, double lz) {
    return dv(b->ox + b->c * lx - b->s * lz, b->oz + b->s * lx + b->c * lz);
}
static inline int inside_building(const Building *b, double wx, double wz, double margin) {
    double dx = wx - b->ox, dz = wz - b->oz;
    double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
    return lx > b->foot.x0 - margin && lx < b->foot.x1 + margin && lz > b->foot.z0 - margin && lz < b->foot.z1 + margin;
}

/* ---------------------------------------------------------------------------
 * Shared types
 * ------------------------------------------------------------------------- */

#define DROP_DEPTH    (KERB_STONE_WIDTH + DROP_RAMP_RUN)

/* A surface's material: the vertex seed, params (material and its parameters) and aux. */
typedef struct Mat { uint32_t seed; uint8_t p[4]; uint32_t aux; } Mat;
/* Emission frames compose: a building's frame inside the chunk, raised to its
 * base, then a face's frame inside the building (x along the face, z out of it). */
typedef struct Xf { int on; float c, s, x, z, y; } Xf;
enum { F_NX = 1, F_PX = 2, F_NZ = 4, F_PZ = 8, F_TOP = 16, F_BOT = 32, F_SIDES = 15, F_NOBOT = 31 };
enum { UV_FACADE = 0, UV_UNIT = 1 };
/* A face in its own frame: origin at its left corner on the ground, x along it, z out of it. */
typedef struct Face { vec3 o, t, n; float width; } Face;

typedef struct EdgeInfo { float ax, az, ex, ez, len; } EdgeInfo;
/* Identity of face k and of the corner it ends on: faces are numbered round
 * the outer loop, then the courtyard; a convex corner's bays are shared with
 * the next face (convex: the outline turns towards the building). */
typedef struct FaceKey { int face, next, startConvex, endConvex, start90, end90; float len, prevLen, nextLen; } FaceKey;
/* What stands behind a ground section's envelope: a planned office lobby (L)
 * or an arcology's concourse level (A), for the facade, its inside and collision. */
typedef struct Behind { const ILayout *L; const ALayout *A; int si; } Behind;
/* The planned ground floors of one building: office lobbies (the main one and
 * at most one more), or an arcology's concourse level and canyon. */
typedef struct Interiors { int n; ILayout *lay[2]; ALayout *arc; } Interiors;

/* MAT_INTERIOR surfaces (params y; shadeInterior in shaders/city.frag). */
enum { IS_FLOOR = 0, IS_ROOMFLOOR, IS_WALL, IS_CEILING, IS_CORE, IS_METAL, IS_WOOD, IS_FABRIC, IS_GLASS,
       IS_LIGHT, IS_PLANT, IS_LINING, IS_STONE, IS_LIFTDOOR, IS_DOOR, IS_SCREEN, IS_SKYLIGHT, IS_OFFICE, IS_SIGN, IS_EMBLEM,
       IS_BARK, IS_SOIL };   /* IS_PLANT: leaves; IS_BARK: stems (grow_plants) */
#define POT_HEIGHT 0.55f   /* lobby planters: the pot's rim above the floor */
#define PLANT_BUDGET 188000   /* plant vertices per chunk, inside MAX_CHUNK_PAGES * PAGE_VERTS */

typedef struct PlantSpot {
    float    x, y, z, c, s;   /* chunk-local soil centre; the plant's turn */
    float    clear;           /* open floor round the planter, metres */
    int      group, member, tier;
    uint32_t bseed;           /* the building (the lobby's light and finishes) */
    float    base;            /* ... and its base (Vertex.base) */
    uint8_t  ceil;            /* ceiling height above the floor, half metres */
    PlantParams pp;
} PlantSpot;
typedef struct Planting { PlantSpot *s; int n, cap; } Planting;

/* ---------------------------------------------------------------------------
 * Functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* city.c */
uint32_t style_hash(uint32_t seed);
uint8_t lobby_accent(uint32_t seed);
/* Where a building's frame stands, world Y (m): its sections' heights (src/gen,
 * from G0) count from there. plot_base works it out when the plot is laid out
 * (lots.c); building_base reads it. */
float plot_base(const Building *b);
static inline float building_base(const Building *b) { return b->base; }
/* The ground's gradient at world (x, z): rise (m) per metre along x and along z. */
void city_ground_slope(double x, double z, float *gx, float *gz);
/* Emits in building b's frame from here on (xf_restore or m->xf = 0 to leave). */
void enter_building(Mesh *m, const ChunkGen *g, const Building *b);

/* drops.c */
int clip_hp(const DVec2 *in, const uint8_t *li, int n, DVec2 nr, double c, uint8_t lab, DVec2 *out, uint8_t *lo);
double area2(const DVec2 *p, int n);
float drop_y(double d);

/* mesh_emit.c */
/* How far a footing (Mesh.footing) reaches into the ground: under the draped
 * sidewalk or lot, which can stand that much proud of the ground between the
 * drape's cuts, and under a long wall's straight foot. */
#define FOOTING_SINK 0.05f
/* Draped surfaces (Mesh.drape) are cut into squares this wide (chunk-local, so
 * the cuts line up across chunks) to follow the ground's curve. At the urban
 * relief's 10 m a straight 55 m span strays at most 13 mm from the ground
 * (27.5 m: 3 mm); finer cuts cost every chunk's street mesh vertices. Hillier
 * settlements will want finer ones. */
#define DRAPE_CELL (CHUNK_SIZE / 2.0f)
/* Empties m for chunk (cx, cz)'s layer: no vertices, lights or transform, bounds reset. */
void mesh_begin(Mesh *m, int cx, int cz);
/* The ground under point (x, z) of the current emission frame, as a height in that frame. */
float mesh_ground_local(const Mesh *m, float x, float z);
/* One vertex, through the emission transform (and onto the ground, if draping);
 * the mesh's bounds grow to hold it. */
void mesh_push(Mesh *m, vec3 p, vec3 n, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux);
/* As mesh_push, with w in the normal's fourth component (litter: ambient occlusion). */
void mesh_push_w(Mesh *m, vec3 p, vec3 n, int8_t w, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux);
Mat mat(uint8_t id, uint32_t seed, uint8_t a, uint8_t b, uint8_t c);
void emit_quad(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 d, vec3 n, const float uv[4][2], Mat mt);
void emit_tri(Mesh *m, vec3 a, vec3 b, vec3 c, vec3 n, const float uv[3][2], Mat mt);
void emit_flat(Mesh *m, const DVec2 *p, int n, float y, const float (*uv)[2], Mat mt);
Xf xf_save(const Mesh *m);
void xf_restore(Mesh *m, Xf t);
void xf_enter(Mesh *m, float ox, float oz, float c, float s);
void emit_box(Mesh *m, vec3 mn, vec3 mx, int faces, Mat mt, int uvmode);
Face face_local(float width);
void push_light(SkyLight **arr, uint32_t *n, uint32_t *cap, const SkyLight *L);
void sign_lights(Mesh *m, vec3 c, vec3 n, float area, Mat mt);
void emit_face_panel(Mesh *m, const Face *f, float u0, float u1, float y0, float y1, float out, Mat mt);
uint8_t q_half(float metres);
void emit_emissive_box(Mesh *m, vec3 c, float half, uint8_t color, uint8_t mode, uint8_t phase);
void emit_beam(Mesh *m, vec3 a, vec3 b, float size, Mat mt);

/* mesh_building.c */
EdgeInfo edge_of(const BLoop *l, int k);
float enter_face(Mesh *m, const BLoop *l, int k);
FaceKey face_key(const BLoop *l, int k, int base);
int face_openings(const BGraph *g, const Behind *bh, int li, int k, float (*op)[3], int max);
int envelope_spans(const BGraph *g, const Behind *bh, const BLoop *l, int li, int k, float (*span)[2], uint8_t *lined, int max);
void emit_cap(Mesh *m, const BFoot *f, float y, int down, Mat mt);
void emit_building(Mesh *m, const Building *b, const BGraph *g, const Interiors *in);

/* mesh_lobby.c */
void interiors_plan(const Building *b, const BGraph *g, Interiors *in);
void interiors_free(Interiors *in);
const ILayout *interior_of(const Interiors *in, int si);
int behind_of(const Interiors *in, int si, Behind *bh);
Mat iframe(const Mesh *m, Mat mt);
int region_rects(const ILayout *L, int r, int wholeOnly, int16_t (*out)[4], int max);
int clip_half(const float (*in)[2], int n, float a, float b, float c, float (*out)[2]);
void emit_cap_minus(Mesh *m, const ILayout *L, const BLoop *outer, float vx0, float vz0, float vx1, float vz1, float y, int down, Mat mt);
void run_strip(const IWall *r, float t, float *x0, float *z0, float *x1, float *z1);
void emit_interior(Mesh *m, const BGraph *g, const ILayout *L);
void emit_interior_doors(Mesh *m, const BGraph *g, const ILayout *L);

/* mesh_planting.c */
void grow_plants(Mesh *m, Planting *pl);
void collect_plants(Mesh *m, const BGraph *g, const ILayout *L, Planting *pl);

/* mesh_arcology.c */
int arc_rects(const ALayout *A, uint32_t useMask, int blk, ARect *out, int max);
void emit_cap_holes(Mesh *m, const ALayout *A, const BLoop *outer, const ARect *holes, int nh, float y, int down, Mat mt);
void emit_arcology(Mesh *m, const BGraph *g, const ALayout *A);
void emit_arc_portals(Mesh *m, const BGraph *g, const ALayout *A);
void emit_arc_frontage(Mesh *m, const BGraph *g, const ALayout *A);
void emit_arc_terraces(Mesh *m, const BGraph *g, const ALayout *A);

/* streets.c */
uint32_t lamp_fault(const Lamp *L);
void build_lamp(Mesh *m, const ChunkGen *g, const Lamp *L, uint32_t idx);
void build_roads(Mesh *m, const ChunkGen *g);
void build_blocks(Mesh *m, const ChunkGen *g);

/* light_tiles.c */
void build_lights(Mesh *m, ChunkGen *g);

/* cables.c */
void build_cables(Mesh *m, ChunkGen *g);
