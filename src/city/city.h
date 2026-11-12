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
#define MAX_FLOORS      256
#define MAX_CHANNELS    24
#define MAX_DECKS       24
#define MAX_BARRIERS    256
#define MAX_ROAD_DECKS  32
#define MAX_LOBBY_DOORS 24
#define CITY_OVERHANG   70.0f

typedef struct Rect { float x0, z0, x1, z1; } Rect;

/* A wall the walker can't pass: a line a b (world x, z) `half` thick. A low
 * one (BARRIER_LOW, a channel's wall) stops feet down in the channel, more
 * than a metre under the city's ground; a deck's (BARRIER_DECK, a road
 * bridge's parapet) feet up on the deck, its surface at y; any other (a
 * parapet) feet up at the street. */
enum { BARRIER_STREET = 0, BARRIER_LOW = 1, BARRIER_DECK = 2 };
typedef struct Barrier { double a[2], b[2]; float half, y; uint8_t low; } Barrier;
/* A piece of a road bridge's deck (src/city/road_bridges.c): its centre line
 * a b (world x, z), its surface from ya to yb (world Y), `half` across to its
 * parapets. */
typedef struct RoadDeckSpan { double a[2], b[2]; float half, ya, yb; } RoadDeckSpan;

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

/* An automatic door at a lobby's entrance or its vestibule's inner screen
 * (IAutoDoor, src/gen/interior_doors.c) in world terms, for src/app/doors.c to
 * open, turn and collide with: c the middle of its opening (a revolving
 * door's drum's centre), a along the opening and n across it into the
 * building (world x, z), y its floor (world Y). What stands still of it is
 * in coll; what moves, src/app/doors.c pushes the walker out of. */
typedef struct LobbyDoor {
    uint32_t key;
    uint8_t  type, leaves, outer;   /* IE_SLIDE or IE_REVOLVE; leaves or wings; in the facade (1) or a vestibule's screen */
    double   c[2];
    float    a[2], n[2];
    float    width, height, y, radius;
} LobbyDoor;

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
    int      vcoll0;                /* coll[vcoll0..ncoll): the street vendors' (the last; city_test leaves them out) */
    CollPoly coll[MAX_COLL];
    int      nfloor;                /* floors inside them, of the buildings in this chunk */
    FloorPoly floor[MAX_FLOORS];
    int      nchan, ndeck, nbarrier;
    CurbPoly chan[MAX_CHANNELS];    /* the rivers' channels touching this chunk (PlanChannel): their floors, */
    uint8_t  chanbuilt[MAX_CHANNELS]; /* (each across a built town, or out in the belt: there, only over the city's ground) */
    CurbPoly deck[MAX_DECKS];       /* the bridges' decks over them, */
    Barrier  barrier[MAX_BARRIERS]; /* and their walls and parapets; and the roads' bridges' parapets */
    int      nrdeck;
    RoadDeckSpan rdeck[MAX_ROAD_DECKS]; /* the roads' bridges' decks touching this chunk */
    int      ndoor;
    LobbyDoor door[MAX_LOBBY_DOORS]; /* the automatic doors of the lobbies of the buildings in this chunk */
    double   collv[MAX_COLL_VERTS][2];
} BlockLayout;

/* A piece of the city's grass the blades grow on (src/city/grass_city.c; GR6
 * of the grass, src/grass/grass_rules.h): a triangle of a park's, a field's
 * or the edge verge's ground, as the chunk's mesh draws it, less what stands
 * on it, within one grass patch (world x, z over GRASS_PATCH_M, rounded
 * down). Its corners: x and z metres from the patch's corner, y world Y. */
typedef struct CityGrassTri {
    int32_t px, pz;
    float   v[3][3];
    float   maintained;   /* 1 mown (lawn) .. 0 neglected (rough) */
} CityGrassTri;
/* A green lot's vertices (MAT_LOT, params y 1) carry in params w whether
 * blades grow there and how well it is kept: 0 none (a roof garden), else
 * 1 + 254 x maintained. shaders/city/ground.glsl reads it too, for the field
 * the blades leave at range (grass/field.glsl). */
#define CITY_GRASS_NONE 0
static inline uint8_t city_grass_param(float maintained) {
    float m = maintained < 0.0f ? 0.0f : maintained > 1.0f ? 1.0f : maintained;
    return (uint8_t)(1.0f + 254.0f * m + 0.5f);
}
static inline float city_grass_maintained(uint8_t w) { return w ? (float)(w - 1) / 254.0f : 0.0f; }

/* A module of the vendors' modelling kit (src/gen/pmodel.h, PMK_*) set down in a chunk, drawn instanced
 * (src/render/kit_gpu.c): a row-major 3x4 transform from the module's frame to chunk-local (x' = M x + t,
 * the translation in [3], [7], [11]; rotation and uniform scale only), its module, its piece's variation
 * (the vendor's per-part tint), its flags (the vendor's wear 0..15 in bits 0-3, KIT_FLAG_HEAT: it stands at
 * the range, so it gathers grease and soot; the vendor's paint, a coat of NB_COATS, in bits 5-7; its hood's
 * metal, NB_HOOD_*, in bits 8-9: src/gen/noodle_bar.h nb_finish; KIT_FLAG_BARE_WOOD: an A-frame left bare
 * timber, set on the A-frame alone; its glow (NB6) in bits 11-15: what glows shows it (kit.glsl): a lit tube's
 * white (NB_WHITE_*: the vendor's) in 11-13, or a neon icon's colour (neonColor) in 11-13 and its flicker in
 * 14-15; shaders/city/kit.glsl), the ground its heights count from (world Y) and a seed (a neon icon's: its
 * light's, nb_icon_seed, so the tubes and the light they throw flicker together). */
#define KIT_FLAG_WEAR 15u
#define KIT_FLAG_HEAT 16u
#define KIT_FLAG_COAT_SHIFT 5
#define KIT_FLAG_HOOD_SHIFT 8
#define KIT_FLAG_BARE_WOOD 1024u   /* NB_FINISH_BARE_WOOD */
#define KIT_FLAG_GLOW_SHIFT 11      /* NB6: 3 bits of colour or white, then 2 of flicker */
/* SF2: a vendor of the base layer's with a near body of its own: its middle (chunk-local, its floor's height)
 * and the radius it reaches (the streamer wants its body within VENDOR_BODY_DISTANCE of the camera). */
typedef struct VendorBodySpot { float x, y, z, r; } VendorBodySpot;
typedef struct KitInst { float xf[12]; uint16_t module, flags; uint8_t part; int8_t grime; uint8_t pad[2]; float base; uint32_t seed; } KitInst;   /* pad zeroed; grime: nb_grime's * 127 (NB7) */

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
    CityGrassTri *grass; /* the grass the blades grow on, by patch (pz, then px; city_build_mesh) */
    uint32_t  ngrass;
    KitInst  *kit;       /* the vendors' kit modules set down in the chunk (city_build_vendor_kit) */
    uint32_t  nkit, capkit;
    VendorBodySpot *vbody; /* SF2: the base layer's vendors with near bodies, in order (their far shells' index) */
    uint32_t  nvbody, capvbody;
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
/* How much graffiti the walls at (x, z) carry, 0 (the wealthiest districts: almost none) .. 1 (the
 * poorest, and the city's squalid border): plan_affluence, as city_squalor reads it, but without the
 * zone (src/city/litter.c). The renderer packs it at each chunk's corners (city_graffiti_corners). */
float    city_graffiti(double x, double z);
/* city_graffiti at chunk (cx, cz)'s four corners, a byte each: x0z0 | x1z0 << 8 | x0z1 << 16 | x1z1 << 24
 * (ChunkGPU.info[3]: graffitiLevel in shaders/city/surface.glsl interpolates them across the chunk). */
uint32_t city_graffiti_corners(int cx, int cz);
/* The street furniture layer (src/city/props.c): bins, bollards, post boxes and
 * crossing signals along the sidewalks, dumpsters in the back alleys, condensers
 * on the walls. Like the litter it is lit by the chunk's light table. */
void     city_build_props(int cx, int cz, Mesh *m);
/* The near vendor layer (src/city/vendor_kit.c; the noodle bar slice's NB2, NB4), within KIT_DISTANCE:
 * the canopy noodle bars' bodies (merged MAT_KIT triangles) and the kit's modules they and the cooking
 * line set down (Mesh.kit). Its bounds are the triangles' and the modules' places, grown by KIT_PAD.
 * Which vendors get them: city_set_kit (KIT_SLICE, the default: every bar, its body and its cooking line (NB5);
 * KIT_ALL: every vendor a line; KIT_OFF: no layer). */
void     city_build_vendor_kit(int cx, int cz, Mesh *m);
/* SF2: one vendor's near body and its kit (the base layer's vendor `body` of chunk (cx, cz): Mesh.vbody's order),
 * which the streamer wants within VENDOR_BODY_DISTANCE of the camera. */
void     city_build_vendor_body(int cx, int cz, int body, Mesh *m);
enum { KIT_OFF = 0, KIT_SLICE, KIT_ALL };   /* none; the bars (named for the slice); every vendor (the cost spike) */
void     city_set_kit(int mode);
int      city_kit_mode(void);
/* SF2: the detail the city builds the vendors' bodies at (PM_LOD0 or PM_LOD1, the default: with every site a body
 * a dense apron's chunk held 740k triangles at LOD0); the slice's own bar is always LOD0 (NeonCity --kit-lod). */
void     city_set_kit_lod(int lod);
int      city_kit_lod(void);
/* NB8's gate (NeonCity --bars all, city_test --slice ... all): every site a canopy noodle bar fits becomes one,
 * so the worst views (a row of six, a lane of bars) exist to be measured. Off by default; before any chunk. */
void     city_force_bars(int on);
int      city_forced_bars(void);
#define KIT_PAD 1.5f   /* m: the largest module's reach from its origin, and some */
/* A piece of street furniture. (x, z) is where it stands: its footprint's
 * centre, a crossing's signal pole, a condenser's back on its wall; y the
 * world Y of its foot (a condenser's underside), ground what its heights
 * count from (Vertex.base); (fx, fz) the way its front faces. */
enum { PROP_BOLLARD = 0, PROP_BIN, PROP_MAILBOX, PROP_DUMPSTER, PROP_CROSSING, PROP_AIRCON, PROP_KINDS };
/* a condenser hung on brackets; a crossing's button facing back along the kerb, its signal on a lamp post */
enum { PF_BRACKETS = 1, PF_BUTTON_BACK = 2, PF_ON_LAMP = 4 };
typedef struct Prop {
    uint8_t  kind, model, livery, flags;   /* PROP_*, its model (PM_* in models.h) and which of its liveries, PF_* */
    double   x, z;
    float    y, ground, fx, fz;
    uint32_t seed;
} Prop;
#define MAX_PROPS 512   /* in one chunk */
/* Everything standing in chunk (cx, cz), as its mesh and collision place it. */
int      city_chunk_props(int cx, int cz, Prop *out, int max);
/* The outline a piece's collision keeps the walker out of (world), or 0 for one out of reach (a condenser). */
int      prop_outline(const Prop *p, double (*v)[2]);
void     mesh_free(Mesh *m);
/* Whether a triangle with this vertex casts ray-traced shadows (src/render/rt.c).
 * Glass, veils of light, puddles and contact shadows don't; nor does flat or
 * wind-stirred litter, whose shape the vertex shader changes. */
int      city_casts_shadow(const Vertex *v);

/* Buildings filling one plan block (all of them, wherever their centres are). */
int      city_block_buildings(const Block *bk, Building *out, int max);
/* The open spaces the lot filler (src/city/lots.c) leaves in a block for the
 * street vendors (src/city/vendor_sites.c, docs/street-vendor-generation.md):
 *   SPACE_APRON  a shophouse row set back 4-7 m behind a paved hawker frontage;
 *   SPACE_LANE   an alley across the block widened to 7-10 m, a paved market lane;
 *   SPACE_POCKET a plot left unbuilt for a paved market;
 *   SPACE_PLAZA  an office tower's paved forecourt, 8-16 m deep (lunch stalls);
 *   SPACE_COURT  a yard behind the buildings reached from the street (found by
 *                vendor_sites.c among what the buildings leave, not made here).
 * A space is a rectangle u0..u1 x v0..v1 in its frame: origin o, unit axes u
 * and v (world x, z). An apron's, a plaza's and a pocket's v runs in from the
 * street (v0 on the lot's street edge); a lane's runs across it. The same
 * filler places the buildings, so the spaces and the buildings agree. */
enum { SPACE_APRON = 0, SPACE_LANE, SPACE_POCKET, SPACE_PLAZA, SPACE_COURT, SPACE_KINDS };
typedef struct CitySpace {
    uint8_t  kind;
    double   ox, oz;
    float    ux, uz, vx, vz;
    float    u0, v0, u1, v1;
    uint32_t seed;
} CitySpace;
#define MAX_BLOCK_SPACES 16
/* The spaces of a block (all of them), as city_block_buildings leaves them; returns how many. */
int      city_block_spaces(const Block *bk, CitySpace *out, int max);
/* Whether a space's ground is paved (all but a court, which is the yard's asphalt). */
static inline int city_space_paved(int kind) { return kind != SPACE_COURT; }
/* Whether world (x, z) lies in space s, grown by margin (m). */
static inline int city_space_contains(const CitySpace *s, double x, double z, double margin) {
    double dx = x - s->ox, dz = z - s->oz, u = dx * s->ux + dz * s->uz, v = dx * s->vx + dz * s->vz;
    return u > s->u0 - margin && u < s->u1 + margin && v > s->v0 - margin && v < s->v1 + margin;
}
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
/* Settlement k's ground at its core: city_ground_y at its centre, read in its
 * own frame (world Y: its height over the planet's sphere, the same in every
 * frame). Its cloud's deck hangs at the deck's heights over it (CLOUD_LOW
 * and the rest count from it: src/core/clouds.h). All of them are read at
 * the first call, which main makes before any worker starts. */
float    city_settlement_ground(int k);
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
