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
 * loops positive. */

#define BG_MAX_PV     40
#define BG_MAX_SECT   24
#define BG_MAX_SUPP   72
#define BG_MAX_DOOR   6

enum { ARCH_OFFICE = 0, ARCH_SKYSCRAPER, ARCH_PROJECTION, ARCH_TWIN, ARCH_GATEWAY, ARCH_ARCOLOGY,
       ARCH_SHOPHOUSE, ARCH_SLAB, ARCH_POINT, ARCH_SHED, ARCH_COUNT };
enum { SEC_GROUND = 0, SEC_PODIUM, SEC_BODY, SEC_SETBACK, SEC_TOWER, SEC_SLICE, SEC_CROWN,
       SEC_PROJECTION, SEC_PIER, SEC_SPAN, SEC_BRIDGE, SEC_CORE, SEC_COUNT };
/* How a section is held up: the ground, an ordinary stack, or an explicit arrangement. */
enum { ARR_GROUND = 0, ARR_STACK, ARR_ARCADE, ARR_PROJECTION, ARR_STEP, ARR_SPAN, ARR_BRIDGE };
enum { SUP_COLUMN = 0, SUP_STRUT, SUP_CORE, SUP_TRANSFER };
enum { GF_RETAIL = 0, GF_LOBBY, GF_DOUBLE, GF_VOID };                 /* ground floor treatment */
enum { WIN_PUNCHED = 0, WIN_CURTAIN, WIN_RIBBON, WIN_SLIT };          /* = shader window types */
enum { ROOF_FLAT = 0, ROOF_PLANT, ROOF_HELIPAD, ROOF_GARDEN, ROOF_TANKS, ROOF_SOLAR, ROOF_HIP, ROOF_PYRAMID };
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

typedef struct BGraph {
    uint32_t seed;
    int      archetype, district, groundMode, fine;
    float    floorH, groundH;     /* typical floor and (treated) ground floor heights */
    int      nsec, nsup, ndoor;
    BSection sec[BG_MAX_SECT];
    BSupport sup[BG_MAX_SUPP];
    BDoor    door[BG_MAX_DOOR];
    int      attempts, simplified;
} BGraph;

/* Optional diagnostics: accepted and rejected proposals, with reasons. */
typedef struct BLog { char *buf; int cap, len; } BLog;

void        bgen_build(const Building *b, BGraph *g, BLog *log);
int         bgen_validate(const BGraph *g, const Building *b, BLog *log);
const char *bgen_archetype_name(int a);
const char *bgen_district_name(int d);

/* Polygon helpers (local frame). */
float bloop_area(const BLoop *l);
int   bloop_inside(const BLoop *l, float x, float z);
int   bfoot_contains(const BFoot *f, float x, float z);
float bfoot_boundary_dist(const BFoot *f, float x, float z, float *qx, float *qz);
/* Triangulates a footprint (with its courtyard) into indices over pts; returns triangle count. */
int   bfoot_triangulate(const BFoot *f, float (*pts)[2], int *npts, int (*tri)[3], int maxTri);

/* Facade rhythm shared with the shader: the building's base bay width. */
float bgen_bay_width(uint32_t seed, int fine);
