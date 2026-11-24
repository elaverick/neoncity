#pragma once
#include "home.h"
#include "gen_common.h"
#include "hash.h"
#include "mathlib.h"
#include <stdarg.h>

/* Shared between the house generator's files (not part of home.h):
 *
 *   building_residential.c        the styles, the grammar of the three house archetypes, the garden, the driver
 *   building_residential_rules.c  the repairs and the rules (section 13.5, 13.4, 13.8) */

/* Section 13.5's bounded quantities, each with its reason there. Its garden and clearance figures (front
 * 3 m, rear 8 m, 2 m each side of a detached house, 1.2 m on a semi's open side, 4 m between two detached
 * houses, a garage's metre) are the region's yard's since HM2b (home_yard, HomeYard): a megacity's yards
 * are tighter, the departure written in docs/residential-target.md. A garage keeps the side clearance,
 * at most a metre. */
#define SPAN_MAX        12.0f   /* rafters between two walls */
#define ROW_MAX         48.0f   /* fire spread and drainage */
#define UNIT_MIN        4.2f
#define DOOR_MIN        1.0f
/* The grammar's own: the narrowest main a detached house or a semi's half keeps, and the shallowest. */
#define MIN_MAIN_W      5.0f
#define MIN_DEPTH       6.5f
#define HEPS            0.02f
static inline float home_garage_clear(const HomeYard *Y) { return Y->side < 1.0f ? Y->side : 1.0f; }

typedef struct HStyleDef {
    const char *name;
    uint8_t     region;
    float       arch[HOME_ARCH_COUNT];   /* the style's weight among its region's, by archetype */
    float       storeys[HOME_STOREYS_MAX];   /* 1, 2, 3, 4 */
    float       roof[HR_COUNT];          /* the main's: gable, hip, (lean: never), flat */
    float       pitch0, pitch1, minPitch;
    float       eave0, eave1;            /* the eaves' overhang (m) */
    float       depth0, depth1;          /* the main's depth (m); terraces up to a metre more */
    float       bay, bay2, canted;       /* a row's front bays; two storeys; canted, not square */
    float       outrigger, garage, extension;
    float       porch[HP_COUNT];
    float       chimney;                 /* stacks a detached house; a terrace's party walls' share is from it */
    float       bound[HB_COUNT];         /* the front boundary */
    float       plinth0, plinth1;        /* the ground floor over the garden (a stoop's to 1.5 m) */
    float       wallH0, wallH1;          /* a front wall's height where the style walls its plots (0: 0.6-1.2 m) */
    uint8_t     parapet, handed;         /* a front parapet (georgian); a terrace's doors in pairs */
    /* HM5e1: each style's body to its sheet's panel 1 */
    uint8_t     street;                  /* the front on the street line: no front yard, the door in a reveal */
    float       arched;                  /* a row's doors under an arch, in an arched reveal (courtyard, MENA villa, colonial, townhouse) */
    uint8_t     bayFull;                 /* bays run the main's full height under its cornice (the brownstone) */
    float       arcade, pilotis;         /* a row's ground floor: a five-foot way; pilotis */
    float       shop;                    /* a unit's chance of a shop front in its ground storey */
    float       doorW0, doorW1;          /* the front door's width (0: the usual 1.0-1.1 m) */
    float       storeyH0, storeyH1;      /* floor to floor (0: the usual 2.7-3.0 m; old houses' tall rooms more) */
    float       uneven;                  /* a flat row's owners built up unevenly: a unit's chance of a storey fewer */
} HStyleDef;

const HStyleDef *home_style_def(int s);
/* building_residential.c */
void  home_repair(HGraph *g, BLog *log, const char *tag, const char *fmt, ...);
void  home_vol_top(HVol *v);
float home_unit_rear(const HGraph *g, int ui);
/* building_residential_rules.c: the repairs after a proposal, before its garden is laid */
void  home_repair_all(HGraph *g, int level, BLog *log);
/* home_balcony.c (HM5e2): a row's balconies by its style, after the repairs (the volumes are final); and their rules */
void  home_balconies(HGraph *g, int level, BLog *log);
int   home_check_balconies(const HGraph *g, BLog *log);
int   home_door_balcony(const HGraph *g, int unit);   /* the balcony over a unit's front door serving as its canopy, or -1 */
/* home_bound.c (HM5e3): a row's boundaries and gates to its style's sheet, after the garden is laid; and their rules */
void  home_bounds(HGraph *g, int level, BLog *log);
int   home_check_bounds(const HGraph *g, BLog *log);
/* home_insert.c (HM5c2): every unit's owner's retrofit draw and the insertions the steps so far build, after the
 * boundaries (everything else final); their rules. Removing volume i keeps every reference to the volumes after it. */
void  home_inserts(HGraph *g, int level, BLog *log);
int   home_check_inserts(const HGraph *g, BLog *log);
void  home_drop_vol(HGraph *g, int i);
/* ...between home_insert.c and home_insert_roof.c (HM5c2c2): an insertion's fault against the graph and the n insertions
 * before it (NULL: none), and its boxes (x0, x1, y0, y1, z0, z1); the roof kinds' draw (whether unit u's body can take
 * one), span, placing and rules */
const char *home_ins_fault(const HGraph *g, const HInsert *in, int n);
int   home_ins_boxes(const HGraph *g, const HInsert *in, float (*bx)[6], int max);
int   home_roof_feasible(const HGraph *g, int kind, const HVol *m, int mi, int u);
void  home_roof_span(const HGraph *g, const HInsert *in, float *y0, float *y1);
void  home_roof_place(HGraph *g, int mi, int u, int kind, BLog *log);
const char *home_roof_fault(const HGraph *g, const HInsert *in, int n);
