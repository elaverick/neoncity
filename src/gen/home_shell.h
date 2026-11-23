#pragma once
#include "home.h"
#include "house_surf.h"

/* The houses' shells (residential roadmap HM3a, ~/.claude/plans/residential-buildings.md; the target in
 * docs/residential-target.md, "Massing and parts").
 *
 * A house graph's volumes (home.h) become convex solids: each volume's body (its walls up to its roof's
 * planes), a slab for each plane of its roof (its covering on top, its soffit under, the fascia and the
 * verges' boards round its overhang), parapets and their copings, party walls standing up through a row's
 * roof where its units step with the ground, chimney stacks and their caps, steps, porch floors and posts;
 * and openings, solids carved out of a body (windows with their glass set back, doors, garage doors, a
 * recessed porch). Every face of a solid is clipped against every other solid it meets (constructive solid
 * geometry: the faces of a union, less the openings), so a wing's or an outrigger's roof runs into the main
 * one in valleys, a lean-to meets its wall, a stack comes up through the tiles, and nothing shows inside
 * anything else. Coplanar faces are settled by their normals: a face looking into another solid is inside
 * it; of two looking the same way the earlier solid's is kept.
 *
 * Two levels: HSH_NEAR, the near layer's shell (the eaves' depth, real openings, steps and posts: within
 * HOME_NEAR_DISTANCE), and HSH_FAR, the base layer's (walls and roof planes, stacks; windows painted on by
 * the shader), to the view distance. Everything is in the row's frame (home.h): x along the street, z in from
 * it, y up from the garden at the front wall. A row's units may stand at different heights (dy: unit u's
 * floor above the row's lowest), as a terrace steps up a slope. */

enum { HSH_NEAR = 0, HSH_FAR };

/* A unit's finishes (house_surf.h): its walls' (HW_*) and their colour, its roof's (HRM_*), its stacks', its
 * trim's, front door's and garage door's colours, its windows' glazing (HG_*) and size. A row was built as
 * one: its walls and roof are the row's, but owners repaint render and siding, and doors are their own. */
typedef struct HFinish {
    uint8_t wall, wallCol, roof, roofCol, flat, flatCol, stack, stackCol, trimCol, doorCol, garageCol, glazing, step;
    float   winW, winH;
} HFinish;
void home_finish(const HGraph *g, int unit, HFinish *f);

#define HSH_MAXV 32
typedef struct HShellFace {
    int      n;                       /* corners, convex, anticlockwise seen from in front */
    float    p[HSH_MAXV][3];          /* the row's frame, the row's lowest unit's floor at y = 0 + dy */
    float    uv[HSH_MAXV][2];
    float    nrm[3];
    uint8_t  surf, finish, colour;    /* HF_*, and params z, w (house_surf.h) */
    int8_t   unit;                    /* the unit it stands on (its base) */
    uint32_t aux;                     /* house_surf.h */
} HShellFace;
typedef void (*HShellFn)(void *ctx, const HShellFace *f);

typedef struct HShellStats {
    int solids, openings, faces, tris;   /* faces and triangles emitted (a face of n corners is n - 2) */
    int windows, doors;
} HShellStats;

/* Builds graph g's shell at level lod, handing each face to fn. dy: each unit's floor over the row's lowest
 * (NULL: all level). st may be NULL. Thread-safe; allocates its working space. */
void home_shell(const HGraph *g, int lod, const float *dy, HShellFn fn, void *ctx, HShellStats *st);

/* A unit's reach: the highest point of anything of the house (its ridge, a stack's top), y in the graph's frame. */
float home_top(const HGraph *g);

/* ---------------------------------------------------------------------------
 * The character layer (residential roadmap HM5; src/gen/home_character.c): the HM4 kit (pmodel_house.h) set
 * down on a near shell by a second grammar. Style, affluence and upkeep choose a set a unit (HCharSet); a row
 * or a pair is built as one, so its units share the row's set, and owners vary it (a replaced window or door,
 * their own gutters, shutters taken down, a loft conversion's dormer). The placement rules are checks with
 * logged repairs ("repair char-<rule>: ..."): downpipes at a corner or a party wall and clear of openings, bays
 * and porches; dormers clear of stacks, hips, valleys, party walls and the ridge; shutters, doorcases and
 * fanlights where the wall has room; quoins on free corners; a porch, canopy or doorcase over every front door.
 * ------------------------------------------------------------------------- */

/* upkeep (docs/residential-target.md "Upkeep": mostly kept, some tired, a few run-down, never derelict) */
enum { HK_KEPT = 0, HK_TIRED, HK_RUNDOWN };
enum { HCE_PLAIN = 0, HCE_BRACKETS, HCE_RAFTERS };   /* eaves */
enum { HCC_NONE = 0, HCC_FLAT, HCC_GABLE };          /* a door canopy (the porch HP_CANOPY's) */
typedef struct HCharSet {
    uint8_t upkeep;                                  /* HK_KEPT .. HK_RUNDOWN */
    uint8_t frame, frameBack, sill, head, headBack;  /* the street's and the rest's windows: HK_FRAME_*, HK_SILL_*, HK_HEAD_* */
    uint8_t door, surround;                          /* HK_DOOR_*, HK_CASE_* */
    uint8_t gutter, rain, hopper;                    /* HK_GUTTER_* (255: none), HKR_*, a hopper head (else a swan neck) */
    uint8_t eaves, knees;                            /* HCE_*; knee braces under the gables' verges */
    uint8_t barge, finial, tudor, hung;              /* the street's gables: bargeboards (0, or 1 + HK_BARGE_*), a finial,
                                                        half-timbering; a band of hung tiles between the storeys */
    uint8_t ridge;                                   /* 0 none, 1 rolls, 2 crested */
    uint8_t quoins, string, plinth, cornice;         /* the front's reliefs */
    uint8_t shutters, boxes, grilles;                /* HK_SHUT_* on the street's windows; window boxes; grilles (1 the
                                                        ground floor's street windows, 2 every ground floor window) */
    uint8_t post, spandrels, balustrade, canopy;     /* porch posts (HK_POST_*), spandrels, a balustrade; HCC_* */
    uint8_t dormer, dormerBack;                      /* 0 none, or 1 + HK_DORMER_*: the street's slope, the back's */
    uint8_t rooflight, vent;                         /* on the back slope */
    uint8_t lean;                                    /* dressed again leaner: it came out over the budget (hs_dress) */
} HCharSet;
enum { HK_POST_SQUARE = 0, HK_POST_TURNED, HK_POST_CRAFT };
/* Unit u's set (pure: the shell and the city read the same). */
void home_char_set(const HGraph *g, int unit, HCharSet *s);
/* A unit's kit instances' flags (house_surf.h HOUSE_KIT_*) and part byte (its roof). */
uint16_t home_kit_flags(const HGraph *g, int unit, uint8_t *part);

/* the rules the dressing checks, for its repairs' counts */
/* ...and HM5c1's retrofit kit: panels and canopies, tanks, risers, what goes on a wall (meters, lights, gates, vines),
 * a flat roof's terrace (railings, screens, trailing plants, razor wire) */
enum { HDR_DOWNPIPE = 0, HDR_DORMER, HDR_SHUTTERS, HDR_DOORCASE, HDR_QUOINS, HDR_CANOPY, HDR_PORCH, HDR_BARGE, HDR_BUDGET,
       HDR_SOLAR, HDR_TANK, HDR_RISER, HDR_WALLKIT, HDR_TERRACE, HDR_COUNT };
const char *home_dress_rule(int r);
typedef struct HDressStats {
    int instances;                 /* placed */
    int maxUnit;                   /* the most on one unit */
    int repairs[HDR_COUNT];
    int windows, doors, gutters, pipes, dormers, stacks;
    int retrofit;                  /* HM5c1: the retrofit kit's instances (of instances) */
    int balconies;                 /* HM5e2: balconies dressed */
    int inserts;                   /* HM5c2: insertions dressed */
} HDressStats;
/* Where a dressing goes: a modelling builder (its instance list, or its mesh with the modules built in), the
 * row's frame to the mesh's (x scaled by sx: -1 for a mirrored row; the kit itself is never mirrored), each
 * instance's unit (grown alongside pm->inst), the log for its repairs. */
typedef struct HDress {
    struct Pm *pm;
    float      sx;
    uint8_t   *unit;
    uint32_t   capUnit;
    BLog      *log;
    HDressStats st;
} HDress;

/* The near shell dressed (HM5): as home_shell(HSH_NEAR) with the shell made ready for the kit (openings sized to
 * its frames, glass painted bars only past the kit's reach, stack caps, porch posts and door canopies left to
 * it), then the kit set down through D. */
void home_shell_kit(const HGraph *g, const float *dy, HShellFn fn, void *ctx, HShellStats *st, HDress *D);
