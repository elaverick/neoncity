#pragma once
#include "building.h"
#include "gen_common.h"
#include "hash.h"
#include "mathlib.h"
#include <stdarg.h>

/* Shared between the building generator's files (src/gen/building*.c). Not
 * part of the public interface in building.h.
 *
 *   building.c             names, districts, roofs and doors, validation, the driver (bgen_build)
 *   building_poly.c        simple polygons: area, containment, distance, triangulation
 *   building_archetypes.c  the footprint grammar and the building grammar's archetypes */

/* ---------------------------------------------------------------------------
 * Shared definitions
 * ------------------------------------------------------------------------- */

#define G0          SIDEWALK_HEIGHT
enum { FAM_RECT = 0, FAM_CHAMFER, FAM_L, FAM_T, FAM_U, FAM_CROSS, FAM_DRUM, FAM_COURT, FAM_COUNT };
/* p1/p2 are proportions of w/d, so scaling a spec keeps its shape. quarter
 * turns the feature (notch, bar, opening) towards another side; w and d are
 * the extents after that turn. */
typedef struct FootSpec { int fam, quarter; float w, d, p1, p2, ang, cx, cz; } FootSpec;
typedef struct Ctx {
    const Building *b;
    BGraph *g;
    BLog   *log;
    float   W, D;          /* site extents */
    float   H;             /* target height above the sidewalk */
    int     N;             /* floors above the ground floor */
    int     frontage;
    Rng     ra, rf, rh, rs, rr, rd;   /* archetype, footprint, heights, surfaces, roofs, doors */
    uint8_t defWin, defPal, defBands, defFins, defLed;
} Ctx;

/* ---------------------------------------------------------------------------
 * Functions, by the file that defines them
 * ------------------------------------------------------------------------- */

/* building_poly.c */
void loop_add(BLoop *l, float x, float z);
void loop_orient(BLoop *l, int outer);
float loop_dist2(const BLoop *l, float x, float z, float *qx, float *qz, float best);
int loop_simple(const BLoop *l);
float loop_min_edge(const BLoop *l);
float loop_perimeter(const BLoop *l);
void loop_bounds(const BLoop *l, float *x0, float *z0, float *x1, float *z1);
int loops_intersect(const BLoop *a, const BLoop *b);
int loop_convex(const BLoop *l);

/* building_archetypes.c */
void make_foot(const FootSpec *s, BFoot *f);
float lvl(const Ctx *c, int k);
int pick(Rng *r, const float *w, int n);
int add_sec(Ctx *c, int type, float y0, float y1, const BFoot *f, int b0, int b1);
FootSpec site_spec(Ctx *c, int fam, float margin);
int arch_office(Ctx *c);
int arch_skyscraper(Ctx *c);
int arch_projection(Ctx *c);
int arch_twin(Ctx *c);
int arch_gateway(Ctx *c);
int arch_arcology(Ctx *c);
int arch_shophouse(Ctx *c);
int arch_housing(Ctx *c, int point);
int arch_shed(Ctx *c);
