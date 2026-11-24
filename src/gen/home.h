#pragma once
#include "building.h"

/* Houses (docs/procedural-building-generation.md section 13; the residential roadmap HM2,
 * ~/.claude/plans/residential-buildings.md; the target in docs/residential-target.md).
 *
 * A house graph is one row of the plots src/city/plots.c cuts along a suburb
 * street (HomePlot): a detached house on its plot, a semi-detached pair on two,
 * or a terrace of 3-12 units. It holds section 13.2's volumes (each a rectangle with
 * storeys and a roof), its site elements (gardens, boundaries with gates, paths,
 * drives, sheds, cars, trees) and its relationships:
 *   shares_party_wall_with   HParty, between two units of a pair or a row;
 *   attached_to              HVol.parent, a wing, extension, bay, outrigger or porch to its main;
 *   supported_by             every volume stands on the ground (HVol.y0 = 0);
 *   opens_on                 HDoor.path, the path from a front door to its gate.
 *
 * Everything is in the row's frame: x along the street (the plots' u, less
 * the row's first u0), z in from the boundary on the street (the plots' v),
 * y up from the garden at the front wall. The street is at z = 0, so a front
 * wall faces -z. Section 13.10's driver builds a proposal at three levels (full,
 * without optional parts, plain), repairs what it can, validates every rule
 * of section 13.5 against the volumes' own outlines (not the house's bounding box,
 * section 13.8) and logs every reject and repair with a tag. home_build always ends
 * in a valid house; a plot too small for even the plainest is left a garden
 * (HGraph.empty) and logged. */

#define HG_MAX_UNITS 12
#define HG_MAX_VOL   72
#define HG_MAX_DOOR  HG_MAX_UNITS
#define HG_MAX_PARTY (HG_MAX_UNITS - 1)
#define HG_MAX_CHIM  32
#define HG_MAX_BOUND 96
#define HG_MAX_GATE  (2 * HG_MAX_UNITS)
#define HG_MAX_SITE  128
#define HG_ROW_WIDE  0xFF   /* HVol.unit of a pair's or a row's main: every unit's */

/* The house styles: each region's palette (docs/residential-target.md, "Regions"). */
enum { HS_CRAFTSMAN = 0, HS_SPANISH, HS_RANCH, HS_TRACT, HS_ROWHOUSE, HS_TRIPLEDECKER,   /* na */
       HS_VICTORIAN, HS_GEORGIAN, HS_INTERWAR, HS_POSTWAR, HS_MODERN,                    /* uk */
       HS_SEU_VILLA, HS_TOWNHOUSE,                                                       /* seu */
       HS_JAPAN, HS_CHINA, HS_KOREA,                                                     /* eas */
       HS_SHOPHOUSE, HS_LINKHOUSE,                                                       /* sea */
       HS_RCC, HS_BUNGALOW,                                                              /* sas */
       HS_MENA_VILLA, HS_COURTYARD,                                                      /* mena */
       HS_BLOCK, HS_WALLED,                                                              /* ssa */
       HS_SELFBUILT, HS_COLONIAL,                                                        /* lat */
       HS_COUNT };

/* Volume roles (section 13.2): a detached house's main, a semi pair's or a terrace row's
 * shared main (one roof over all its units), and what is attached to them. */
enum { HV_MAIN = 0, HV_PAIR, HV_ROW, HV_WING, HV_EXTENSION, HV_BAY, HV_OUTRIGGER, HV_PORCH, HV_ROLE_COUNT };
enum { HR_GABLE = 0, HR_HIP, HR_LEAN, HR_FLAT, HR_COUNT };
/* A volume's sides: -x, +x, -z (the street's), +z (the back). */
enum { HSIDE_W = 0, HSIDE_E, HSIDE_S, HSIDE_N };
/* Porches (HVol.kind of an HV_PORCH): open and gabled on posts, a lean-to, enclosed and glazed,
 * recessed into the front wall (a tiled step), a canopy on brackets, a veranda (most of the front); and the
 * ground floors HM5e1 added, carved out of the main as a recessed porch is, the door at their back: a five-foot
 * way (HP_ARCADE: the upper storeys over a colonnade on the street, the ground wall set back behind piers; the
 * shophouse) and pilotis (HP_PILOTIS: the ground storey open on columns, parking under it, the door and the stair
 * in a core at the back; a Korean villa). home_porch_recessed: the kinds carved out of the main. */
enum { HP_OPEN = 0, HP_LEAN, HP_ENCLOSED, HP_RECESSED, HP_CANOPY, HP_VERANDA, HP_ARCADE, HP_PILOTIS, HP_COUNT };
static inline int home_porch_recessed(int kind) { return kind == HP_RECESSED || kind == HP_ARCADE || kind == HP_PILOTIS; }
/* A pilotis' columns (m square) and the most a span runs between two along the front before a middle one stands */
#define PILOTIS_COL  0.35f
#define PILOTIS_SPAN 6.0f
/* An arcade's piers at its unit's sides (m: each side's half of a pier shared at a party wall) */
#define ARCADE_PIER  0.22f
/* HVol.kind of an HV_BAY and an HV_WING. */
enum { HBAY_SQUARE = 0, HBAY_CANTED };
/* A canted bay's front light (m): drawn from CANT_FACE0-1, never under CANT_FACE_MIN (narrower, it is built square) */
#define CANT_FACE0    1.1f
#define CANT_FACE1    1.5f
#define CANT_FACE_MIN 1.0f
enum { HWING_ROOM = 0, HWING_GARAGE };
/* Boundaries: kinds, and where they run. A railing (HM5e3) is iron or steel bars on a kerb or a dwarf wall: Georgian and
 * brownstone area railings, the Victorian front's. */
enum { HB_NONE = 0, HB_WALL, HB_HEDGE, HB_FENCE, HB_RAILING, HB_COUNT };
enum { HBR_FRONT = 0, HBR_SIDE, HBR_PARTY, HBR_REAR };
/* What stands on a boundary's solid part (HM5e3, HBound.top): plain iron bars, spear-topped iron railings, painted steel
 * bars (the link house's), a galvanised mesh (Japan's), a band of breeze blocks under the coping, pierced blocks of rings
 * (the bungalow's balustrade wall); nothing. */
enum { HBT_NONE = 0, HBT_IRON, HBT_SPEAR, HBT_STEEL, HBT_MESH, HBT_BREEZE, HBT_RINGS, HBT_COUNT };
/* Gates (HM5e3, HGate.kind): none (an open gap), an iron leaf with spear tops, steel bars (a security gate's), painted
 * sheet steel, pierced steel (the Middle East's), a mesh leaf, painted steel bars (the link house's), plank leaves under
 * China's gatehouse. HGate.leaves: 1 a single leaf, 2 a pair, 3 one leaf sliding behind the wall. */
enum { HGK_OPEN = 0, HGK_IRON, HGK_BARS, HGK_SHEET, HGK_PIERCED, HGK_MESH, HGK_STEEL, HGK_PLANK, HGK_COUNT };
enum { HGL_SINGLE = 1, HGL_DOUBLE, HGL_SLIDING };
/* A row's boundaries are built as one (HM5e3; HGraph.bset, from the style's sheet): their finish and colour (HW_*), the
 * coping (a slab, bricks on edge, China's tile cap), piers along them every `pier` metres (0: at their ends and gates
 * only), `pierW` square; razor wire along the walls' tops; lanterns on the gate piers; iron rails up a stoop's steps. */
enum { HBC_SLAB = 0, HBC_BRICK, HBC_TILE };
typedef struct HBoundSet {
    uint8_t drawn;               /* built at all: the styles with a concept sheet (the rest's are planned, never drawn) */
    uint8_t fin, col, coping;
    uint8_t razor, lamps, stoopRail;
    float   pier, pierW;
} HBoundSet;
#define BOUND_T     0.22f   /* a wall's thickness (a railing's kerb's 0.3) */
#define BOUND_KERB  0.30f
#define BOUND_COPE  0.04f   /* a coping's overhang each side */
/* Chimney stacks: on a ridge, at a gable end (on or outside its wall), on a party wall (section 13.4). */
enum { HC_RIDGE = 0, HC_GABLE, HC_PARTY };
/* Site elements (section 13.2): gardens, paths, drives, sheds, parked cars and the places a tree may
 * stand (whether one does is HM7's call). */
enum { HE_FRONT = 0, HE_REAR, HE_PATH, HE_DRIVE, HE_SHED, HE_CAR, HE_TREE, HE_KIOSK, HE_COUNT };   /* HE_KIOSK: a walled
                                                    compound's gate kiosk (HM5e3), its front in the front wall's line */

typedef struct HVol {
    uint8_t role, roof, storeys, unit;   /* HV_*, HR_*, storeys, its unit (HG_ROW_WIDE for a pair's or row's main) */
    int8_t  parent;                      /* attached_to: its main, or -1 */
    uint8_t ridgeX;                      /* gable, hip: the ridge runs along x (the span is the z extent) */
    uint8_t high;                        /* lean: the side that rises (HSIDE_*), against its parent's wall */
    uint8_t kind;                        /* HP_* of a porch, HBAY_* of a bay, HWING_* of a wing */
    uint8_t parapet;                     /* 1 a parapet along the front hides the roof (georgian), 2 all round (flat) */
    uint8_t arch;                        /* a recessed porch's: its head an arch (and its door's) */
    float   x0, z0, x1, z1;
    float   y0, eaves, top;              /* the ground, the wall plate, the ridge (a flat roof's parapet) */
    float   pitch, overhang;             /* degrees; the eaves' overhang (m) */
} HVol;

typedef struct HUnit {
    float    x0, x1, depth;      /* its plot: x0..x1 by 0..depth */
    float    front;              /* the plot's planned front garden */
    uint32_t seed;
    uint8_t  mirrored;           /* handed: its door towards +x */
    int8_t   door;               /* its front door */
    /* a shop front (HM5e1: the RCC, self-built and shophouse sheets' shop under the house): a roller-shuttered
     * opening shopX0..shopX1 in the ground storey of the wall its door is in (shopX1 > shopX0) */
    float    shopX0, shopX1;
    uint8_t  storeys;            /* its own storeys where a flat row's owners built up unevenly (the self-built and RCC
                                    sheets); 0: the main's. Never more than the main's (home_unit_vol) */
} HUnit;

/* A front door: centred at x, in the wall at z; height to its head, or an arched one's springing (its head a half
 * circle of half its width over that) */
typedef struct HDoor { int8_t unit, vol, path; uint8_t side, arch; float x, z, width, height; } HDoor;
typedef struct HParty { int8_t a, b, vol; float x, z0, z1, top; } HParty;                           /* units a and b share the wall at x */
typedef struct HChimney { int8_t vol, unit; uint8_t on, pots; float x, z, w, d, top; } HChimney;
/* A boundary along a line of its unit's plot: its solid part h high (a wall, a railing's kerb or dwarf wall, a fence's
 * plinth), and `top` (HBT_*) th high over that (HM5e3) */
typedef struct HBound { uint8_t kind, role, top; int8_t unit; float x0, z0, x1, z1, h, th; } HBound;
/* a gap in the front boundary (z = 0), x its middle, w wide; its gate (HM5e3): HGK_*, HGL_* leaves, h high, under a
 * gatehouse's roof (China's) */
typedef struct HGate { int8_t unit; uint8_t drive, kind, leaves, house; float x, w, h; } HGate;
typedef struct HSite { uint8_t kind, surface; int8_t unit, door; float x0, z0, x1, z1; } HSite;     /* a rectangle; a tree's is its canopy's square; a yard's surface HY_* */

/* Balconies (HM5e2, the sheets' panel 1; docs/residential-target.md "Balconies"): on a unit's slice of the main's front
 * wall (its face at z, facing the street), at a storey over the ground, across x0..x1 (whole bays of the front's window
 * rhythm, home_front_bays), the windows behind made French windows. A balconette is a rail on a stone ledge `depth`
 * deep (the Georgian first floor's); a slab a cantilevered one with a rail; a glazed one a slab with a solid upstand and
 * glass to a slab over it (the Korean villa's); a loggia is carved `depth` into the body under a flat or arched head
 * (the MENA villa's); a veranda the front of an upper storey carved between piers (the walled compound's). */
enum { HBK_BALCONETTE = 0, HBK_SLAB, HBK_GLAZED, HBK_LOGGIA, HBK_VERANDA, HBK_COUNT };
/* what guards it: plain iron bars, Georgian ironwork, a painted steel tube rail, a turned balustrade, nothing */
enum { HBL_IRON = 0, HBL_ORNATE, HBL_STEEL, HBL_BALUSTRADE, HBL_NONE };
typedef struct HBalcony {
    int8_t  unit, vol;              /* its unit, the main it stands on */
    uint8_t storey, kind, rail;     /* 1..: the storey whose floor it is at; HBK_*; HBL_* */
    uint8_t arch;                   /* a loggia's arched head: its rise, cm (0 flat) */
    uint8_t consoles;               /* stone consoles under a slab (Southern Europe's) */
    float   x0, x1, z, depth;
} HBalcony;
#define HG_MAX_BALC   48
#define BALC_SLAB     0.18f   /* a slab's depth */
#define BALC_DROP     0.04f   /* its top under its storey's floor (a step down out of the French window) */
#define BALC_PARTY    0.30f   /* kept off a party wall, or a corner */
#define BALC_STREET   0.80f   /* how far an upper floor's balcony may reach over a street-line front's pavement */
#define BALC_PIER     0.30f   /* a veranda's piers */
#define BALC_VBAY     3.20f   /* ...and the most a veranda's opening spans between two */
static inline int home_balcony_carved(int kind) { return kind == HBK_LOGGIA || kind == HBK_VERANDA; }
const char *home_balcony_name(int kind);

/* Retrofit insertions (HM5c2; the sheets' panels 2-6, docs/residential-target.md "Retrofit insertions"): what a unit's
 * owner has added to the house since it was built, joined to it by the shell's CSG. Three units in five are retrofitted
 * (the user, 2026-11-20), one kind or two, each owner drawing from the style's final table (HGraph.insPlan); the steps
 * build their kinds in turn (HM5c2a boxes and rounded bays; HM5c2b frames, hexagons, fins and screen strips; HM5c2c
 * stairs, stair towers, unfinished storeys and roof rooms), and a unit whose kind is not built yet stays plain (the user's
 * choice, 2026-11-23). An insertion stands on its unit's slice of the main's street front, across whole bays of its
 * window rhythm: hung from the first floor up (s0 >= 1), or on the ground in the place of the unit's bay (s0 0). */
enum { HI_BOX = 0, HI_ROUND, HI_FRAME, HI_HEX, HI_FINS, HI_SCREEN, HI_STAIRS, HI_TOWER, HI_UNFINISHED, HI_ROOFROOM, HI_COUNT };
/* a box's make: cast concrete with windows, steel and glass (barred over as a cage in South and South-East Asia and
 * China), a breeze-block screen in front of glass, a shipping container's corrugated steel; a rounded bay is concrete or
 * render with ribbon glass between its bands */
enum { HIF_CONCRETE = 0, HIF_GLASS, HIF_SCREEN, HIF_CONTAINER, HIF_COUNT };
typedef struct HInsert {
    int8_t  unit, vol;          /* its unit; the main whose front it stands on */
    uint8_t kind, face;         /* HI_*, HIF_* */
    uint8_t s0, s1;             /* the storeys it spans (s0 0: on the ground, where the unit's bay stood) */
    uint8_t fin, col;           /* its walls' finish (house_surf.h HW_*) and colour; a glass box's steel (a trim colour),
                                   a container's (the metal palette) */
    uint8_t cage, screen;       /* bars over a glass box; a screen box's blocks (0 diamonds, 1 rings); HM5c2c1: a stair of
                                   steel (cage), a concrete one's solid upstands (screen); a stair tower's riser (screen) */
    uint8_t took;               /* the balconies it took in (the house as built had them: home_lab's panel-1 survey) */
    uint8_t shape, cells;       /* HM5c2b: a frame's, fins', hexagons' or strip's form (HIS_*); a honeycomb's hexagons */
    uint8_t raised;             /* ...and a one-storey house's (s0 0) standing on corbels 0.4 m over its floor, not the ground */
    float   x0, x1, z, depth;   /* along the front; the wall's face; how far out (a member kind's: its front's) */
    float   r;                  /* a rounded bay's corners' radius; a honeycomb's hexagons' (corner to centre); a stair's
                                   landings' length where its landing stands over the front door (0: STAIR_LAND) */
    float   w, t, lean;         /* HM5c2b: its members' width and thickness (front to back), a raked one's lean */
                                /* HM5c2c1 stairs: a flight's width, a tread, the way it rises along the front (+1 to +x) */
    float   door;               /* HM5c2c1 stairs: the upper doors' middle (along its wall), one at each landing on a floor */
    uint8_t wall;               /* HM5c2c1: the wall it stands against (HIW_*); every kind but a stair's the street front */
} HInsert;
/* HM5c2c1: an insertion's wall. A stair may stand against the street front (a front yard's), the back (into the rear yard,
 * towards the lane) or a free side (the side gap of a detached house, a semi's open side, a terrace's end unit). in->z is
 * that wall's face: its z (front, back) or its x (a side); along it, a member's "x" (a) runs along x (front, back) or z (a
 * side), and "out" leaves the wall. */
enum { HIW_FRONT = 0, HIW_BACK, HIW_WEST, HIW_EAST };
static inline int home_wall_side(int wall) { return wall == HIW_WEST || wall == HIW_EAST; }
/* the way along the wall (ex, ez) and out of it (nx, nz), in the row's frame */
static inline void home_ins_axes(const HInsert *in, float *ex, float *ez, float *nx, float *nz) {
    const int side = home_wall_side(in->wall);
    *ex = side ? 0.0f : 1.0f; *ez = side ? 1.0f : 0.0f;
    *nx = in->wall == HIW_WEST ? -1.0f : in->wall == HIW_EAST ? 1.0f : 0.0f;
    *nz = in->wall == HIW_FRONT ? -1.0f : in->wall == HIW_BACK ? 1.0f : 0.0f;
}
/* the point a along the wall, `out` out of it, in the row's frame */
static inline void home_ins_point(const HInsert *in, float a, float out, float *x, float *z) {
    float ex, ez, nx, nz;
    home_ins_axes(in, &ex, &ez, &nx, &nz);
    const int side = home_wall_side(in->wall);
    *x = (side ? in->z : 0.0f) + a * ex + out * nx;
    *z = (side ? 0.0f : in->z) + a * ez + out * nz;
}
/* HM5c2b's forms. Frames: two legs and a head (corners chamfered or not), one leg raked, a diagonal a storey, two raked legs
 * meeting over the middle. Fins: blades on the bay lines (raked in plan), a brise-soleil of thin upright fins standing off
 * the wall in front of the windows (HIS_BLADE_SQ), an egg-crate grid, a louvred crown over the top storey. Hexagons: a honeycomb of hexagonal frames, a box clad in hexagonal panels. Screen strips: a lattice of
 * breeze blocks or a star lattice (mashrabiya). */
enum {
    HIS_PORTAL = 0, HIS_CHAMFER, HIS_LEAN, HIS_ZIGZAG, HIS_APEX,
    HIS_BLADE, HIS_BLADE_SQ, HIS_GRID, HIS_LOUVRE,
    HIS_HONEYCOMB, HIS_HEXCLAD,
    HIS_STRIP, HIS_STRIP_STAR,
    HIS_STAIR, HIS_DOGLEG,      /* HM5c2c1: a straight flight a storey along the front; two a storey, out and back */
    HIS_COUNT
};
/* HM5c2c1: stairs. A stair (HI_STAIRS) rises along its unit's front to a landing at an upper door on each floor it reaches:
 * from the ground (s0 0) to the floors s0 + 1..s1, or hung from a landing at a door on floor s0 to s1 (a street-line front's).
 * Concrete (solid upstands if `screen`, else a kerb and the kit's rails) or steel (`cage`). The doors: INS_DOOR_W wide, at
 * in->door on each landing's floor (home_insert_door_at). */
#define INS_DOOR_W   0.90f
#define INS_DOOR_H   2.05f
#define STAIR_GAP    0.12f   /* a dog-leg's spine between its lanes */
static inline int home_stair_door_floor(const HInsert *in, int k) { return k >= (in->s0 ? in->s0 : 1) && k <= in->s1; }
const char *home_insert_shape_name(int shape);
/* An insertion's members (HM5c2b): convex prisms of concrete. HMB_ELEV: a polygon p in the front's elevation (x along it,
 * y up) pushed out from the wall a0..a1 (out: towards the street, from the wall's face; a negative a0 runs into the wall);
 * HMB_PLAN: a polygon in plan (x, out) pushed up a0..a1. A hexagon's glass (HMR_GLASS) is the window it frames. The rules,
 * the shell, the dressing and the walker all read home_insert_members. */
enum { HMB_ELEV = 0, HMB_PLAN };
enum { HMR_LEG = 0, HMR_BEAM, HMR_SHELF, HMR_FIN, HMR_SLAT, HMR_RING, HMR_GLASS, HMR_POST, HMR_TIE,
       HMR_FLIGHT, HMR_STEP, HMR_LANDING, HMR_UPSTAND, HMR_STRINGER, HMR_TREAD };   /* HM5c2c1: a stair's */
typedef struct HMember {
    uint8_t axis, role, n;
    float   p[8][2];
    float   a0, a1;
} HMember;
#define HG_MAX_MEMBERS 80
#define INS_OFF        0.40f   /* a member crossing windows stands this far off the wall at least (the shell's 0.35 m
                                  "something in front" test: the window behind is still cut) */
/* the kinds built of members */
static inline int home_insert_membered(const HInsert *in) {
    return in->kind == HI_FRAME || in->kind == HI_FINS || in->kind == HI_SCREEN || in->kind == HI_STAIRS || (in->kind == HI_HEX && in->shape != HIS_HEXCLAD);
}
#define HG_MAX_INS  24
#define INS_PARTY   0.30f   /* kept off a party wall or a corner (the balconies' figure) */
#define INS_STREET  0.80f   /* how far a hung one may reach over a street-line front's pavement */
#define INS_SLAB    0.22f   /* a hung one's floor slab under its lowest storey's floor (over every recess's ceiling, 0.25-0.35 m under it) */
const char *home_insert_name(int kind);
const char *home_insert_face_name(int face);
/* the kinds the steps so far build */
static inline int home_insert_built(int kind) { return kind <= HI_TOWER; }   /* HM5c2a boxes, rounded bays; HM5c2b frames, hexagons, fins,
                                                                                strips; HM5c2c1 stairs and stair towers */

/* The yards by region (residential roadmap HM2b; the user, 2026-11-16): a megacity's houses have small,
 * mostly paved yards, somewhere between Tokyo-tight and British terraced, Europe on the larger side, and
 * no lawns anywhere in a city. Section 13.5's garden and clearance figures are loosened to these, a stated
 * megacity departure (docs/residential-target.md, "Plots"); the rules check each row against its region's.
 * Frontages and depths are by archetype (HOME_DETACHED a plot, HOME_SEMI a half, HOME_TERRACE a unit). */
enum { HY_PAVED = 0, HY_GRAVEL, HY_PLANTED, HY_SOIL, HY_COUNT };   /* a yard: slabs or concrete, gravel, paving with beds and pots, swept earth */
typedef struct HomeYard {
    float frontMin, rearMin;               /* the rules: the front yard (bays and porches stand in it), the rear yard */
    float front0, front1;                  /* a front yard's planned depth, without a car */
    float side, semiOpen, gap;             /* a detached house's clearance each side, a semi's on its open side, between two detached houses */
    float proj;                            /* how near the street a bay or porch may come (the boundary, its gate, the path's start);
                                              frontMin - proj leaves a door canopy's 0.6 m at least */
    float width[HOME_ARCH_COUNT][2];       /* frontage */
    float depth[HOME_ARCH_COUNT][2];       /* plot depth, the street's boundary to the back fence */
    float pad[HOME_ARCH_COUNT];            /* the chance a row parks its cars in the front yard, a paved pad HOME_PAD0-1 deep */
    float surface[HY_COUNT];               /* the yards' surfaces' weights at middling affluence */
} HomeYard;
#define HOME_PAD0      5.2f   /* a car's 4.6 m and a margin */
#define HOME_PAD1      6.0f
#define HOME_HOUSE_MIN 7.0f   /* the plots leave a main this deep at least */
const HomeYard *home_yard(int region);
const char     *home_surface_name(int s);

typedef struct HGraph {
    uint32_t seed;
    uint8_t  arch, style, region;      /* HOME_*, HS_*, PLAN_REGION_* */
    uint8_t  level;                    /* the proposal accepted: 0 full, 1 without optional parts, 2 plain, 3 the simplest */
    uint8_t  empty;                    /* no house fits: the plot is left a garden */
    float    affluence;                /* plan_affluence of the row (the character layer's sets, HM5) */
    int      attempts, repairs;
    float    storeyH, plinth;          /* floor to floor; the ground floor over the garden */
    float    u0;                       /* the row's first plot's u0: x = u - u0 */
    double   ox, oz;                   /* the row's frame (its plots') */
    float    tx, tz, nx, nz;
    int      nunit, nvol, ndoor, nparty, nchim, nbound, ngate, nsite, nbalc, nins;
    HUnit    unit[HG_MAX_UNITS];
    HVol     vol[HG_MAX_VOL];
    HDoor    door[HG_MAX_DOOR];
    HParty   party[HG_MAX_PARTY];
    HChimney chim[HG_MAX_CHIM];
    HBound   bound[HG_MAX_BOUND];
    HGate    gate[HG_MAX_GATE];
    HSite    site[HG_MAX_SITE];
    HBalcony balc[HG_MAX_BALC];
    HBoundSet bset;                    /* HM5e3: the row's boundaries' set */
    HInsert  ins[HG_MAX_INS];          /* HM5c2: the insertions built */
    uint16_t insPlan[HG_MAX_UNITS];    /* ...and every unit's owner's draw, built or not (bits of HI_*) */
} HGraph;

/* HM5c2: insertion in's underside and top (y in the graph's frame, the unit's floor at the plinth); a storey's sill and head
 * in it at storey k (0: no room for a window there) */
void        home_insert_span(const HGraph *g, const HInsert *in, float *y0, float *y1);
int         home_insert_window(const HGraph *g, const HInsert *in, int k, float *sill, float *head);
/* the hung insertion over unit u's front door, sheltering it as a canopy would (HM5c2c1: or a stair's landing), or -1 */
int         home_door_insert(const HGraph *g, int u);
/* HM5c2b: insertion in's members (in the row's frame: x along the front, y over the unit's floor's datum, out from in->z) */
int         home_insert_members(const HGraph *g, const HInsert *in, HMember *out, int max);
/* HM5c2c1: whether a stair's door on floor k of unit u stands where a window centred at (x, z) on a wall facing (mx, mz), hw
 * either side of its middle, would be (the window there is the door) */
int         home_insert_door_at(const HGraph *g, int u, int k, float x, float z, float hw, float mx, float mz);
/* ...and stair in's doors: their floors and middles (along its wall); returns the count */
int         home_insert_doors(const HGraph *g, const HInsert *in, int *k, float *x, int max);
/* ...and a stair's flights and landings (the dressing's rails, home_lab): each flight's foot riser x, its foot's height, its
 * risers, a riser's height, the way it rises, its lane (out o0..o1) and whether that lane is the outer one; each landing's
 * x0..x1, floor, out to; a stair tower's head: how far back over the roof it runs (0: none) */
typedef struct HFlight { float xa, ya, h, s, o0, o1; int n, outer; } HFlight;
typedef struct HLanding { float x0, x1, y, d, xFree; } HLanding;
int         home_stair_parts(const HGraph *g, const HInsert *in, HFlight *fl, int *nfl, HLanding *la, int *nla, float *tread);
/* the windows of the wall a member insertion stands against (home_front_windows on the front) that it stands in front of,
 * within the shell's 0.35 m */
int         home_insert_windows_lost(const HGraph *g, const HInsert *in);
/* the windows of unit u's slice of main mi's wall `wall` (HIW_*) as the shell lays them out, in the wall's frame (along it,
 * up: x0, x1, y0, y1, the shell's 0.12 m margin round each); the front's are home_front_windows. Returns the count. */
int         home_wall_windows(const HGraph *g, int mi, int u, int wall, float (*out)[4], int max);
float       home_tower_back(const HGraph *g, const HInsert *in);
/* The front windows of unit u's slice of main mi as the shell cuts them (add_windows: a window a bay, sills and heads, a
 * French window where a balcony is), each widened by the shell's 0.12 m margin: x0, x1, y0, y1. Returns the count. */
int         home_front_windows(const HGraph *g, int mi, int u, float (*out)[4], int max);

/* HM5e3: names (home_lab), and whether a boundary is built (its row's set drawn; walls, railings, and fences with a top:
 * the rest, hedges and the timber fences behind the houses, are HM7's) */
const char *home_bound_name(int kind);
const char *home_top_name(int top);
const char *home_gate_name(int kind);
static inline int home_bound_drawn(const HGraph *g, const HBound *b) {
    return g->bset.drawn && (b->kind == HB_WALL || b->kind == HB_RAILING || (b->kind == HB_FENCE && b->top != HBT_NONE));
}
/* A drawn boundary's thickness, and the piers' centres along it: where it starts and ends, at its gates, every bset.pier
 * metres (home_bound.c; the shell, the dressing and the walker's outlines read the same) */
float home_bound_thick(const HBound *b);
typedef struct HPier { float x, z, top; int8_t unit; uint8_t gate; } HPier;
int   home_piers(const HGraph *g, HPier *out, int max);
/* a drawn boundary's footprint: its line offset into its plot by its thickness (a party line's straddles it) */
void  home_bound_rect(const HGraph *g, const HBound *b, float *x0, float *z0, float *x1, float *z1);

/* Builds the houses of one row: `row` holds its n plots in order (a row's units, as city_block_plots
 * gives them), affluence its plan_affluence. log may be NULL. */
void        home_build(const HomePlot *row, int n, float affluence, HGraph *g, BLog *log);
/* The same from a given level (1 without optional parts, 2 plain, 3 the simplest): home_lab's check that
 * every fallback is itself valid. */
void        home_build_at(const HomePlot *row, int n, float affluence, int first, HGraph *g, BLog *log);
/* Every rule of section 13.5 (and 13.4, 13.8) on a built graph; logs "rejected <tag>: ..." lines. */
int         home_validate(const HGraph *g, BLog *log);

/* Volume vi as unit u's slice of it stands: a row's or a pair's main at the unit's own storeys (HUnit.storeys, its
 * eaves and top with them), else the volume itself. tmp: working space. */
const HVol *home_unit_vol(const HGraph *g, int vi, int u, HVol *tmp);
/* The window rhythm of unit u's slice of main mi's front wall (section 13.7, a window a bay of about 2.7 m, as the
 * shell's add_windows lays them): its run starts at *s0, *bw a bay; returns the bays (0: no slice). */
int         home_front_bays(const HGraph *g, int mi, int u, float *s0, float *bw);
/* Balcony b's floor (its slab's top), y in the graph's frame. */
float       home_balcony_floor(const HGraph *g, const HBalcony *b);
/* A French window's width in a bay bw wide, from the unit's usual window width ww. */
static inline float home_french_width(float ww, float bw) { const float w = ww < 0.9f ? 0.9f : ww > 1.5f ? 1.5f : ww; return w > bw - 0.5f ? bw - 0.5f : w; }
/* A veranda's (HBK_VERANDA) openings between its piers: n openings, each w wide, the first from x0 + BALC_PIER. */
static inline int home_veranda_bays(const HBalcony *b, float *w) {
    const float L = b->x1 - b->x0 - BALC_PIER;
    int n = (int)(L / (BALC_VBAY + BALC_PIER)) + 1;
    if (n < 1) n = 1;
    *w = L / (float)n - BALC_PIER;
    return n;
}
/* A volume's outline in the row's frame (a canted bay's is its trapezoid); returns its corner count. */
int         hvol_outline(const HVol *v, float (*p)[2]);
/* The row's frame to the world. */
void        home_to_world(const HGraph *g, float x, float z, double *wx, double *wz);

/* The styles: names, regions and the pick (a row's, from its region and archetype; lat/lon split the
 * north-east's rowhouses and the East Asian countries). */
const char *home_style_name(int s);
int         home_style_region(int s);
int         home_style_find(int region, const char *name);   /* region -1: any; -1 if none */
int         home_style_pick(int region, int arch, double lat, double lon, uint32_t h);
int         home_style_storeys(int style, int arch, uint32_t h);   /* 1-4 (HOME_STOREYS_MAX) */
#define HOME_STOREYS_MAX 4
/* The styles whose fronts stand on the street line (no front yard: the townhouse, shophouse, courtyard house,
 * colonial and self-built sheets), the plots' front 0 for them. */
int         home_style_street(int s);
/* The least pitch a style's roofs take (degrees): section 13.5's 25, a ranch's 18 (docs/residential-target.md). */
float       home_style_min_pitch(int s);
const char *home_role_name(int role);
const char *home_roof_name(int roof);
const char *home_porch_name(int kind);
