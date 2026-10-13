#pragma once
#include "building.h"

/* Arcology interiors (procedural-office-atrium-generation.md, section 11).
 *
 * An arcology's ground floor is a district, not a lobby: concourses carry
 * the street grid through the building from facade to facade between the
 * columns of a megastructure grid, market blocks of stalls fill the space
 * between them, and a canyon (a void several tiers tall, ringed by a gallery
 * on every floor) rises from a plaza round the express core to a glazed light
 * crown. Each upper tier's outline comes down through the building as a ring
 * of piers, so the stepped exterior repeats inside as nested colonnades.
 * One value, the stratum height t (0 at the ground, 1 at the top tier), sets
 * density, signage, retrofit clutter and palette.
 *
 * The plan is a pure function of the building and a variant. Like the office
 * planner it works in a frame: the building frame turned in quarter turns so
 * the main portal faces -z, on a 0.5 m grid over the ground footprint. The
 * realisation (emit_arcology in src/city.c) reads it; nothing here makes
 * geometry. */

#define AR_CELL        0.5f
#define AR_MAX_DIM     200          /* cells per side: 100 m, more than any mega plot */
#define AR_MAX_TIER    10
#define AR_MAX_COL     512
#define AR_MAX_BEAM    96
#define AR_MAX_CONC    12
#define AR_MAX_PORTAL  16
#define AR_MAX_BLOCK   96
#define AR_MAX_STALL   384
#define AR_MAX_CORE    10
#define AR_MAX_STAIR   16
#define AR_MAX_LEVEL   200
#define AR_MAX_BRIDGE  240
#define AR_MAX_SIGN    1600
#define AR_MAX_LAMP    128
#define AR_MAX_DUCT    160
#define AR_MAX_FIX     1200
#define AR_MAX_LIGHT   8            /* the real-light budget (Appendix B) */
#define AR_MAX_SHAFT   8
#define AR_MAX_SEARCH  2
#define AR_ATTEMPTS    6

enum { AC_SPINE = 0, AC_TWIN, AC_WELL, AC_PLAZA };                 /* canyon modes; plaza: no void fitted */
enum { AP_CROSS = 0, AP_GRID, AP_BOULEVARD };                      /* concourse patterns */
enum { AS_STREET = 0, AS_HABITAT, AS_COMMERCE, AS_CORPORATE };     /* strata */

/* What each concourse-level cell is. Public walkable: concourse, colonnade, plaza, hall. */
enum { AU_OUT = 0, AU_CONCOURSE, AU_COLONNADE, AU_PLAZA, AU_HALL, AU_SPINE, AU_CORE, AU_COREHALL,
       AU_STAIR, AU_BLOCK, AU_STALL, AU_KIOSK, AU_YARD, AU_COUNT };

/* One use per market block (section 11.6, step 5). */
enum { AB_MARKET = 0, AB_NOODLE, AB_ARCADE, AB_CLINIC, AB_CAPSULE, AB_REPAIR, AB_BATH, AB_EXCHANGE,
       AB_HOUSING, AB_YARD, AB_KIOSK, AB_COUNT };

enum { AG_FASCIA = 0, AG_BLADE, AG_BANNER, AG_SCREEN, AG_HALL };   /* signs */
enum { AX_COUNTER = 0, AX_KIOSK, AX_BENCH, AX_GATE, AX_BOOTH, AX_VENT, AX_CRATE, AX_PLANT, AX_PUDDLE,
       AX_TANK, AX_AWNING, AX_BIN, AX_STEAM, AX_COUNT };          /* fixtures and dressing */
enum { AD_DUCT = 0, AD_TRAY, AD_PIPE };                            /* retrofit runs */
enum { AL_SODIUM = 0, AL_COLD = 1 };                               /* real lights: = shader lamp types */

/* MAT_ARCOLOGY surfaces (params y; shadeArcology in shaders/city.frag, which
 * mirrors these). params z is a variety, params w the stratum height * 255. */
enum { ARS_FLOOR = 0, ARS_STREET, ARS_SLAT, ARS_CEILING, ARS_COLUMN, ARS_CAPITAL, ARS_BEAM, ARS_BLOCK, ARS_ROOF,
       ARS_STALL, ARS_STALLROOF, ARS_SHUTTER, ARS_COUNTER, ARS_SPINE, ARS_CORE, ARS_GALLERY, ARS_SOFFIT, ARS_FASCIA,
       ARS_UNITS, ARS_RAIL, ARS_METAL, ARS_CROWN, ARS_SHAFT, ARS_SEARCH, ARS_PUDDLE, ARS_STEAM, ARS_CRATE, ARS_BRIDGE,
       ARS_PORTAL, ARS_STAIR, ARS_LINING, ARS_KIOSK, ARS_LIFTDOOR, ARS_SKYLOBBY, ARS_COUNT };

typedef struct ARect { float x0, z0, x1, z1; } ARect;

/* A tier of the stack: its footprint in the frame, height range, stratum height. */
typedef struct ATier { BFoot foot; float y0, y1, t; int section, floors, stratum; } ATier;
typedef struct AColumn { float x, z, size; uint8_t ring, tier; } AColumn;      /* ring: a setback pier of `tier` */
typedef struct ABeam { float x0, z0, x1, z1; } ABeam;                           /* transfer beam over a concourse */
/* axis 0 runs along x, 1 along z; lamps every lampStep from lampPhase along it, at lampOff across */
typedef struct AConcourse { ARect r; uint8_t axis, main, boulevard, rib; float lampStep, lampPhase, lampOff; } AConcourse;
/* An opening on face `face` of the ground section's outline, centred at u (metres along it). */
typedef struct APortal { int face; float u, width, height; float fx, fz, nx, nz; uint8_t main, service; } APortal;
typedef struct ABlock { ARect box; int cells, core; uint8_t use; float height; } ABlock;
typedef struct AStall { ARect r; uint8_t facing, block, kind, closed; uint32_t seed; } AStall;  /* facing 0..3 = -z, +x, +z, -x */
typedef struct ACore { ARect r; int topTier, lifts; uint8_t door; } ACore;     /* door: the side its hall leaves by */
typedef struct AStair { ARect r; uint8_t facing; } AStair;
typedef struct ABridge { int level; ARect r; uint8_t axis; } ABridge;           /* axis: the span runs along x (0) or z (1) */
/* A sign plane from (x0,z0) to (x1,z1), y0..y1, facing (nx,nz); (0,0): seen from both sides. */
typedef struct ASign { uint8_t kind, color, style, flick; float x0, z0, x1, z1, y0, y1; int8_t nx, nz; uint32_t seed; } ASign;
typedef struct ALamp { float x, z, y; } ALamp;
typedef struct ADuct { uint8_t kind; float x0, z0, x1, z1, y, size; } ADuct;   /* y: underside */
typedef struct AFix { uint8_t type, facing; float x0, z0, x1, z1, y0, y1; uint32_t seed; } AFix;
typedef struct ALight { uint8_t type; float x, y, z; } ALight;
/* A searchlight hung under the crown at (x, y, z), pointing down, swinging
 * amp radians either way along frame direction (cos az, sin az), sized so it
 * stays inside the canyon. */
typedef struct ASearch { float x, y, z, len, az, amp, speed, phase; } ASearch;
typedef struct AShaft { ARect r; float y0, y1; } AShaft;
/* The express stop at the base of an upper tier, gated by its stratum, with
 * the local cores that stop just below it transferring there, and its terrace
 * on the setback roof of the tier below. */
enum { AK_PUBLIC = 0, AK_RESIDENTS, AK_CHECKPOINT, AK_ESCORT };
typedef struct ASkyLobby { int tier, cores; float y, terrace; uint8_t gate; } ASkyLobby;

typedef struct ALayout {
    int      planned, ok, attempts, quarter;
    uint32_t seed;
    int      canyon, pattern;
    /* planning grid over the ground footprint (frame metres) */
    float    gx0, gz0;
    int      nx, nz;
    float    area;
    /* heights */
    float    floorY, ceilY, floorH, topY;
    /* parameters */
    float    colSpacing, colSize, galleryD, stratumEdge[3], signDensity, retrofit;
    float    cx, cz;                       /* the vertical axis: the top tier's centre */
    float    gridOx, gridOz;               /* column lines at gridOx + k * colSpacing (x), gridOz + k * colSpacing (z) */
    uint8_t  palette[3];                   /* neon colours (shaders: neonColor) */
    /* tier stack */
    int      ntier;
    ATier    tier[AR_MAX_TIER];
    /* anchors */
    ARect    spine;
    int      expressLifts;
    ARect    hall;                         /* express hall on the canyon floor */
    int      nvoid;
    ARect    voids[2];
    int      topTier;                      /* last tier the canyon rises through */
    float    crownY;                       /* the light crown: the canyon's glazed cap */
    int      nlevel;
    float    levelY[AR_MAX_LEVEL];         /* gallery floors */
    int      ncol, nbeam, nconc, nportal, nblock, nstall, ncore, nstair, nbridge;
    AColumn  col[AR_MAX_COL];
    ABeam    beam[AR_MAX_BEAM];
    AConcourse conc[AR_MAX_CONC];
    APortal  portal[AR_MAX_PORTAL];
    ABlock   block[AR_MAX_BLOCK];
    AStall   stall[AR_MAX_STALL];
    ACore    core[AR_MAX_CORE];
    AStair   stair[AR_MAX_STAIR];
    ABridge  bridge[AR_MAX_BRIDGE];
    /* dressing */
    int      nsign, nlamp, nduct, nfix, nlight, nshaft, nsearch, nsky;
    ASign    sign[AR_MAX_SIGN];
    ALamp    lamp[AR_MAX_LAMP];
    ADuct    duct[AR_MAX_DUCT];
    AFix     fix[AR_MAX_FIX];
    ALight   light[AR_MAX_LIGHT];
    AShaft   shaft[AR_MAX_SHAFT];
    ASearch  search[AR_MAX_SEARCH];
    ASkyLobby sky[AR_MAX_TIER];
    /* cells: use (AU_*) and block (255: none) */
    uint8_t  use[AR_MAX_DIM * AR_MAX_DIM];
    uint8_t  blk[AR_MAX_DIM * AR_MAX_DIM];
} ALayout;

/* Plans the arcology's interior. Returns 0 for a building that is not an
 * arcology. Otherwise A->planned is set, and A->ok says whether a valid
 * layout was found within AR_ATTEMPTS (if not, the ground floor stays the
 * painted lobby). Deterministic; log (may be NULL) receives the decision log. */
int         arcology_plan(const Building *b, const BGraph *g, int variant, ALayout *A, BLog *log);

void        arcology_to_local(const ALayout *A, float fx, float fz, float *x, float *z);
void        arcology_to_frame(const ALayout *A, float x, float z, float *fx, float *fz);
int         arcology_use_at(const ALayout *A, float fx, float fz);      /* AU_*, AU_OUT off the grid */
float       arcology_t(const ALayout *A, float y);                       /* stratum height at absolute height y */
int         arcology_stratum(const ALayout *A, float t);
int         arcology_public(int use);                                    /* walkable by anyone */
const char *arcology_canyon_name(int m);
const char *arcology_pattern_name(int p);
const char *arcology_use_name(int u);
const char *arcology_stratum_name(int s);
