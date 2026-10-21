#pragma once
#include "building.h"

/* Procedural office-lobby interiors (see docs/procedural-office-atrium-generation.md).
 *
 * The ground floor of an office building is planned as a semantic layout, not
 * a mesh: a spatial grammar says which spaces the building needs, a planning
 * grid of half-metre cells holds them while they are fitted to the footprint,
 * and validation checks the result before src/city/mesh_lobby.c realises it as walls,
 * floors, cores and furniture.
 *
 * The grid lies in a planning frame: the building's own frame turned in
 * quarter turns so the primary entrance faces "front", the low-z edge. Rules
 * therefore speak of front, rear, left and right whatever the building's
 * orientation, and because the turn is a multiple of 90 degrees, rectangles
 * in the frame are rectangles in the building frame too.
 *
 * Layout order is part of the design: structure, cores, vestibule, atrium
 * void, main route and gates are anchored first; back-of-house rooms are then
 * cut from bands along the facades; the lobby is whatever remains. A failed
 * layout is retried down a fixed ladder of simplifications, and everything is
 * a pure function of the building, so a lobby is identical whenever its area
 * is revisited. */

#define IN_CELL       0.5f
#define IN_MAX_DIM    192            /* cells per side: footprints up to 96 m */
#define IN_MAX_REG    72
#define IN_MAX_DOOR   112
#define IN_MAX_FIX    384
#define IN_MAX_ZONE   4
#define IN_MAX_CORE   2
#define IN_MAX_COL    96
#define IN_ATTEMPTS   6

/* Cell occupancy and flags. */
enum { IO_OUTSIDE = 0, IO_USABLE, IO_COURT };
enum { IF_ROUTE = 1, IF_RESERVE = 2, IF_VOID = 4, IF_DOORCLEAR = 8, IF_FIXTURE = 16, IF_COLUMN = 32,
       IF_APPROACH = 64, IF_EDGE = 128 /* not wholly inside the footprint */, IF_FRONT = 256 /* frontage keep-clear */,
       IF_ENTRY = 512 /* just inside the entrance */, IF_ACCESS = 1024 /* a fixture's use zone: kept clear */ };

/* Region types; their kind (open, enclosed room, solid part) follows from the type. */
enum { IR_LOBBY = 0, IR_VESTIBULE, IR_HALL, IR_SECURE, IR_LIFT, IR_STAIR, IR_RISER,
       IR_TOILET, IR_WC, IR_CLEANER, IR_SECURITY, IR_MAIL, IR_FACILITIES, IR_PLANT, IR_MEETING,
       IR_SHOWER, IR_CYCLE, IR_BINS, IR_LOADING, IR_ESCAPE, IR_COUNT };
enum { IK_OPEN = 0, IK_ROOM, IK_SOLID };
enum { IG_OFFICE = 0, IG_SERVICE };                     /* grammars */
enum { IA_NONE = 0, IA_DOUBLE, IA_GALLERY, IA_PODIUM }; /* atrium modes */

/* Doors and gates. */
enum { ID_ROOM = 0, ID_LIFT, ID_STAIR, ID_INNER, ID_ENTRANCE, ID_SERVICE, ID_FIRE, ID_GATE };
/* Boundary types (section 6.4 of the design). */
enum { IB_NONE = 0, IB_OPEN, IB_BARRIER, IB_GLASS, IB_WALL, IB_SOLID, IB_ENVELOPE };
/* Fixtures: rough shapes for now. */
enum { IX_DESK = 0, IX_CHAIR, IX_SOFA, IX_TABLE, IX_COUNTER, IX_PLANTER, IX_FEATURE, IX_FLIGHT,
       IX_CUBICLE, IX_VANITY, IX_SHELF, IX_UNIT, IX_BIN, IX_PALLET, IX_RACK, IX_LOCKER, IX_SCREEN,
       IX_TURNSTILE, IX_COUNT };
enum { IZ_RECEPTION = 0, IZ_WAITING, IZ_CAFE };

typedef struct IRegion {
    uint8_t type, kind, glazed, core;     /* core: the core a hall, secure side, lift or stair belongs to */
    int16_t x0, z0, x1, z1;               /* bounding cells, half-open */
    int     cells;
} IRegion;

/* A run of `len` cell edges on a grid line. axis 0: the line z = line (edges
 * between rows line-1 and line), running along x from cell `at`; axis 1: the
 * line x = line, running along z. a, b: the regions either side (-1: outside). */
typedef struct IDoor { uint8_t kind, axis; int16_t line, at, len, a, b; } IDoor;

/* Planting groups: planters set out together hold plants of one kind. */
enum { IP_NONE = 0, IP_ROW /* along the frontage */, IP_PAIR /* either side of a lift hall */, IP_SINGLE /* by the waiting area */ };

/* Box in frame metres; facing 0..3 = -z, +x, +z, -x (the way its front looks).
 * Planters: group IP_*, member its place in the group (a pair: the core * 2 + side). */
typedef struct IFixture { uint8_t type, facing, group, member; int16_t region; float x0, z0, x1, z1, y0, y1; } IFixture;
typedef struct IZone { uint8_t type; float x0, z0, x1, z1; } IZone;

typedef struct ICore {
    uint8_t lifts, stairs, facing, gated, lanes, mouth;   /* mouth: the way the hall opens (0..3 as facing) */
    int16_t x0, z0, x1, z1;                               /* cells of the whole core, hall included */
    int16_t hall, secure;                                 /* regions (-1: none) */
} ICore;

typedef struct IAtrium {
    uint8_t mode, levels;                 /* levels: podium floors the void rises through */
    int16_t x0, z0, x1, z1;               /* void, cells (gallery and podium modes) */
    float   galleryY, topY;               /* gallery slab top; top of the void (glazed roof) */
} IAtrium;

typedef struct ILayout {
    int      planned;         /* 0: no interior (not an office lobby, or does not fit) */
    int      ok;              /* 1: a validated layout; 0: an open floor plate after six failures */
    int      grammar, attempts, section, quarter;
    uint32_t seed;
    float    gx0, gz0;        /* frame coordinates of the grid's corner */
    int      nx, nz;
    float    floorY, ceilY;   /* building-frame heights of the floor and the ceiling */
    float    glassTop;        /* top of the lobby glazing (the facade shader's) */
    float    area;            /* usable floor area, m2 */
    float    floors;          /* floors served by the cores */
    BFoot    foot;            /* the section's footprint in the frame */
    int      routeStart;      /* cell where the main route starts (inside the vestibule's inner door, or the entrance) */
    /* An entrance the building graph did not have (service grammar): face k of the section, centred at u. */
    int      synthFace;       /* -1: none */
    float    synthU, synthW;
    int      nreg, ndoor, nfix, nzone, ncore, ncol;
    IRegion  reg[IN_MAX_REG];
    IDoor    door[IN_MAX_DOOR];
    IFixture fix[IN_MAX_FIX];
    IZone    zone[IN_MAX_ZONE];
    ICore    core[IN_MAX_CORE];
    int16_t  col[IN_MAX_COL][2];
    IAtrium  atrium;
    uint8_t  occ[IN_MAX_DIM * IN_MAX_DIM];
    uint8_t  own[IN_MAX_DIM * IN_MAX_DIM];     /* region index, 255 = none */
    uint16_t flag[IN_MAX_DIM * IN_MAX_DIM];
} ILayout;

/* A straight run of one boundary type (frame metres, along the grid line from
 * (x0,z0) to (x1,z1)). door: 0 partition, 1 doorway, 2 gate. side: the way
 * (+1 / -1 across the line) to the room or solid part, 0 if none. */
typedef struct IWall { uint8_t type, door, doorKind; int8_t side; float x0, z0, x1, z1; } IWall;

/* Ground sections that get an interior: the one holding the primary entrance
 * (office grammar) and any other at ground level (service grammar). */
int         interior_sections(const BGraph *g, int *out, int max);
/* Plans the ground floor of section si. Returns L->planned. */
int         interior_plan(const Building *b, const BGraph *g, int si, int variant, ILayout *L, BLog *log);
/* Region and boundary meaning, shared by realisation and validation. */
int         interior_kind(int type);
const char *interior_region_name(int type);
int         interior_boundary(const ILayout *L, int a, int b);   /* region indices, -1 outside */
/* Merged partition runs, doorways and gates. Returns the count. */
int         interior_walls(const ILayout *L, IWall *out, int max);
/* Ceiling lights (src/gen/interior_lights.c): square fittings on grids in the
 * building frame, kept where a whole tile and a tile's border round it fit in
 * the room. Rectangles in building-frame metres; level 0 the ceiling, 1 a
 * gallery's underside; region the room whose grid it is on (-1: the open floor's). */
#define IL_TILE       1.2f           /* a fitting's side (m) */
#define IL_PITCH      3.6f           /* fittings' spacing on a grid, centre to centre (m) */
#define IL_SMALL_BORDER 0.6f         /* the border round a small room's single fitting (m) */
#define IL_MAX        2048
typedef struct ILight { uint8_t level; int16_t region; float x0, z0, x1, z1; } ILight;
int         interior_lights(const ILayout *L, ILight *out, int max);
/* Whether the open floor (lobby, vestibule, halls) keeps at least one fitting. */
int         interior_lobby_lit(const ILayout *L);
/* The open floor's grid: fitting centres at q * IL_PITCH / 256 + k * IL_PITCH along x and z. */
void        interior_light_grid(const ILayout *L, uint8_t q[2]);
/* Frame <-> building frame. */
void        interior_to_local(const ILayout *L, float fx, float fz, float *x, float *z);
void        interior_to_frame(const ILayout *L, float x, float z, float *fx, float *fz);
