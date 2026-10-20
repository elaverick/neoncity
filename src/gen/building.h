#pragma once
#include "city.h"

/* Rule-based building generation (see docs/procedural-building-generation.md).
 *
 * A building is a typed graph: sections (vertically bounded extrusions of a
 * polygon footprint), the supports that carry them, and its doors. The
 * building grammar picks an archetype and its sections, the footprint grammar
 * builds each footprint from a small family of shapes, and validation checks
 * every section against what holds it up before the graph is accepted. A
 * failed proposal is retried with other choices and finally simplified to a
 * plain block, so generation always ends in a valid building.
 *
 * Everything happens in the building's local frame (the plot's site rectangle
 * Building.foot, centred on the origin) and is a pure function of the
 * Building record, which is itself a pure function of the plan, so a building
 * is identical every time its area is revisited.
 *
 * Footprint loops run so that a facade's u coordinate (metres along the face,
 * as the facade shader sees it) follows the vertex order: edge k runs from
 * v[k] to v[k+1], its outward normal is (-dz, dx) and the building lies on
 * the other side. Outer loops have negative signed area, courtyard (hole)
 * loops positive.
 *
 * Industrial plants and electrical substations (section 11 of the design;
 * src/gen/building_industry.c) use the same graph. Their vessels, stacks,
 * flares, frames, racks, transformers and gantries are sections of type
 * SEC_UNIT, standing on the ground as polygons with a height; an elevated
 * deck is a SEC_DECK. Their halls, warehouses, offices, control buildings
 * and switch houses are ordinary sections with facades, marked with the
 * unit kind they are. Pipes (and a substation's conductors), ladders and
 * stairs, and the holes vessels pass through decks by are lists of their own. */

#define BG_MAX_PV     40
#define BG_MAX_SECT   32
#define BG_MAX_SUPP   96
#define BG_MAX_DOOR   6
#define BG_MAX_PIPE   24
#define BG_MAX_ACCESS 16
#define BG_MAX_HOLE   6
#define BG_PIPE_PV    6

enum { ARCH_OFFICE = 0, ARCH_SKYSCRAPER, ARCH_PROJECTION, ARCH_TWIN, ARCH_GATEWAY, ARCH_ARCOLOGY,
       ARCH_SHOPHOUSE, ARCH_SLAB, ARCH_POINT, ARCH_SHED, ARCH_PLANT, ARCH_SUBSTATION, ARCH_COUNT };
enum { SEC_GROUND = 0, SEC_PODIUM, SEC_BODY, SEC_SETBACK, SEC_TOWER, SEC_SLICE, SEC_CROWN,
       SEC_PROJECTION, SEC_PIER, SEC_SPAN, SEC_BRIDGE, SEC_CORE, SEC_UNIT, SEC_DECK, SEC_COUNT };
/* How a section is held up: the ground, an ordinary stack, or an explicit arrangement. */
enum { ARR_GROUND = 0, ARR_STACK, ARR_ARCADE, ARR_PROJECTION, ARR_STEP, ARR_SPAN, ARR_BRIDGE, ARR_DECK };
/* SUP_LEG: a steel leg or post standing on the ground (a deck's, a rack's bents, a frame's, a gantry's). */
enum { SUP_COLUMN = 0, SUP_STRUT, SUP_CORE, SUP_TRANSFER, SUP_LEG };
enum { GF_RETAIL = 0, GF_LOBBY, GF_DOUBLE, GF_VOID };                 /* ground floor treatment */
enum { WIN_PUNCHED = 0, WIN_CURTAIN, WIN_RIBBON, WIN_SLIT };          /* = shader window types */
enum { ROOF_FLAT = 0, ROOF_PLANT, ROOF_HELIPAD, ROOF_GARDEN, ROOF_TANKS, ROOF_SOLAR, ROOF_HIP, ROOF_PYRAMID,
       ROOF_GABLE, ROOF_SAWTOOTH };                                   /* the last two: industrial sheds */
/* Plant units (BSection.unit). Those from IU_CONTROL on are buildings with facades. */
enum { IU_NONE = 0, IU_COLUMN, IU_TANK, IU_DOME, IU_EXCHANGER, IU_STACK, IU_FLARE, IU_GASHOLDER, IU_FRAME,
       IU_RACK, IU_TRANSFORMER, IU_FIREWALL, IU_GANTRY,
       IU_CONTROL, IU_HALL, IU_WAREHOUSE, IU_OFFICE, IU_SWITCH, IU_KIOSK, IU_COUNT };
/* A vessel's or stack's head (BSection.head). */
enum { IH_FLAT = 0, IH_DOME, IH_CONE, IH_COWL };
/* Plant styles (BGraph.style) and scale tiers (BGraph.tier). */
enum { PST_GAS = 0, PST_GOTHIC, PST_NIGHT, PST_COUNT };
enum { PT_GROUND = 0, PT_COLOSSAL };
/* Paints (BSection.paint; shaders/city/industry.glsl: industryPaint). */
enum { PA_PALE = 0, PA_CREAM, PA_ORANGE, PA_RED, PA_MINT, PA_YELLOW, PA_SOOT, PA_WHITE, PA_GALV, PA_RUST,
       PA_OCHRE, PA_GREEN, PA_CLAD, PA_STEEL, PA_BRASS, PA_BLUE };
enum { PIPE_PROCESS = 0, PIPE_CONDUCTOR };
enum { ACC_LADDER = 0, ACC_STAIR };
enum { DOOR_PRIMARY = 0, DOOR_SERVICE, DOOR_LOADING };
enum { DIST_DOWNTOWN = 0, DIST_MIDRISE, DIST_CENTRE, DIST_ESTATE, DIST_INDUSTRIAL, DIST_COUNT };

typedef struct BLoop { int n; float x[BG_MAX_PV], z[BG_MAX_PV]; } BLoop;
typedef struct BFoot { BLoop outer, hole; } BFoot;                   /* hole.n == 0: no courtyard */

typedef struct BSection {
    int     type;
    float   y0, y1;
    BFoot   foot;
    int     nbelow, below[2];     /* supporting sections; none = the ground */
    int     arr;                  /* accepted support arrangement (ARR_*) */
    int     support;              /* SUP_* used by a projection */
    float   bound;                /* its validated quantity: overhang, step or span (m) */
    /* surface treatments */
    uint8_t win, palette, bands, fins, plant, roof, spire, accent, ledTop;
    /* plant units: kind (IU_*), head (IH_*), paint (shaders/city/industry.glsl), the frame
     * that braces it (braced_by: 1 + its section, 0 none), a gasholder's guide posts */
    uint8_t unit, head, paint, braceBy, posts;
    float   fill;                 /* a gasholder's lifts, raised 0..1 */
} BSection;

/* The ground floor in a building's own frame, whose y = 0 is the street: the
 * sidewalk's height. The generators (buildings, lobbies, arcologies) build up
 * from it, and src/city raises the frame by the ground under the plot. */
#define G0 SIDEWALK_HEIGHT
/* A section standing on the ground floor (its y0 is G0, give or take rounding). */
static inline int bsec_grounded(const BSection *s) { return s->y0 <= G0 + 0.01f; }

/* Column or strut: centre line from (x0,y0,z0) to (x1,y1,z1), square of side `size`.
 * Transfer structure / core: box x0..x1, y0..y1, z0..z1. */
typedef struct BSupport { int type, section; float x0, y0, z0, x1, y1, z1, size; } BSupport;

/* A semantic opening on face `face` (outer edge) of `section`, centred at u. */
typedef struct BDoor { int type, section, face; float u, width, height; int canopy; } BDoor;

/* connects: a pipe (or a substation's conductor) from a port on section a to
 * one on section b, an orthogonal run through n points of radius r. */
typedef struct BPipe { int kind, a, b, n; float r; float x[BG_PIPE_PV], y[BG_PIPE_PV], z[BG_PIPE_PV]; } BPipe;
/* reached_by: a ladder or stair from free ground at (x, z) up to y1 on
 * section `section`; (nx, nz) points away from what it climbs. */
typedef struct BAccess { int type, section; float x, z, y1, nx, nz; } BAccess;
/* A vessel (unit) passing through a deck, which bears on a collar on it. */
typedef struct BHole { int deck, unit; float x, z, r; } BHole;

typedef struct BGraph {
    uint32_t seed;
    int      archetype, district, groundMode, fine;
    float    floorH, groundH;     /* typical floor and (treated) ground floor heights */
    int      nsec, nsup, ndoor;
    BSection sec[BG_MAX_SECT];
    BSupport sup[BG_MAX_SUPP];
    BDoor    door[BG_MAX_DOOR];
    int      attempts, simplified;
    /* plants and substations */
    int      style, tier;
    float    rise;                /* the colossal tier's vertical scale (1 on the ground tier) */
    float    fence[4];            /* x0, z0, x1, z1 of the fenced yard or pad */
    int      gateSide;            /* SIDE_* of the gate, centred at gateU along that side */
    float    gateU, gateW;
    int      npipe, nacc, nhole;
    BPipe    pipe[BG_MAX_PIPE];
    BAccess  acc[BG_MAX_ACCESS];
    BHole    hole[BG_MAX_HOLE];
} BGraph;

/* How far a vessel's head (IH_*) rises above its shell, radius r. */
static inline float bgen_head_height(int head, float r) {
    return head == IH_DOME ? 0.55f * r : head == IH_CONE ? 0.3f * r : head == IH_COWL ? (0.8f * r < 2.5f ? 0.8f * r : 2.5f) : 0.0f;
}
/* Where a pipe is held up: posts (x, z, foot, top) standing on the ground or
 * a deck, beside what a rack or frame carries; *longest is its longest
 * unsupported stretch (src/gen/building_industry.c). */
int  bgen_pipe_posts(const BGraph *g, const BPipe *p, float (*out)[4], int max, float *longest);
/* The centre and radius of a faceted drum (a vessel's footprint). */
void bloop_circle(const BLoop *l, float *cx, float *cz, float *r);

/* Plants and substations (section 11). */
static inline int bgen_industrial(const BGraph *g) { return g->archetype == ARCH_PLANT || g->archetype == ARCH_SUBSTATION; }
/* A section with walls and windows: everything but a plant's units and decks. */
static inline int bsec_facade(const BSection *s) { return s->type != SEC_UNIT && s->type != SEC_DECK; }
/* A plant unit you can walk through or under: a lattice or a frame on legs. */
static inline int bsec_open(const BSection *s) {
    return s->type == SEC_UNIT && (s->unit == IU_FRAME || s->unit == IU_RACK || s->unit == IU_GANTRY || s->unit == IU_FLARE);
}

/* Optional diagnostics: accepted and rejected proposals, with reasons. */
typedef struct BLog { char *buf; int cap, len; } BLog;

void        bgen_build(const Building *b, BGraph *g, BLog *log);
int         bgen_validate(const BGraph *g, const Building *b, BLog *log);
const char *bgen_archetype_name(int a);
const char *bgen_district_name(int d);
const char *bgen_unit_name(int u);
const char *bgen_style_name(int s);
/* The minimum width of a section above the ground floor (section 4.2): four
 * bays, but no more than a narrow plot leaves room for. */
float       bgen_min_width(const BGraph *g, const Building *b);

/* Polygon helpers (local frame). */
float bloop_area(const BLoop *l);
int   bloop_inside(const BLoop *l, float x, float z);
int   bfoot_contains(const BFoot *f, float x, float z);
float bfoot_boundary_dist(const BFoot *f, float x, float z, float *qx, float *qz);
/* The narrowest calliper width of an outline: its least extent across any direction (m). */
float bloop_width(const BLoop *l);
/* Triangulates a footprint (with its courtyard) into indices over pts; returns triangle count. */
int   bfoot_triangulate(const BFoot *f, float (*pts)[2], int *npts, int (*tri)[3], int maxTri);

/* Facade rhythm shared with the shader: the building's base bay width. */
float bgen_bay_width(uint32_t seed, int fine);
