#pragma once
#include "plan.h"

/* Plan internals, shared by the plan's sources.
 *
 *   plan.c          towns, neighbourhoods, roads and blocks of a settlement
 *   plan_world.c    the settlements over the planet, their footprints, the
 *                   future sea and the continents
 *   plan_roads.c    the roads between the settlements */

/* Whether town (i, j) is the centre town of a settlement other than the
 * megacity (whose centre is town (0, 0)). */
int  plan_world_core_town(int i, int j);

/* The road network (plan_roads.c): built by plan_world_init once the
 * settlements are placed, on up to `threads` threads. */
void plan_roads_build(int threads);

/* The pieces of the city (plan.c), for plan_water.c: a neighbourhood's piece
 * of a town, the cell of its site among the town's (plan_area's pieces), and
 * whether the town is built. A PlanPieces caches what it reads; use one per
 * thread. */
typedef struct PlanPiece { int ti, tj, ni, nj, built; } PlanPiece;
typedef struct PlanPieces PlanPieces;
PlanPieces *plan_pieces_new(void);
void        plan_pieces_free(PlanPieces *pp);
void        plan_piece_at(PlanPieces *pp, double x, double z, PlanPiece *out);
/* The road between two pieces that meet, a boulevard between towns or an
 * arterial within one, and its centre line: through q, square to the unit
 * normal n, which points from a to b. */
RoadAttr    plan_piece_road(const PlanPiece *a, const PlanPiece *b, DVec2 *q, DVec2 *n);
/* A built town's piece's outline, as plan_area cuts it; 0 if it has none. */
int         plan_piece_poly(PlanPieces *pp, const PlanPiece *pc, Poly *out);

/* The rivers through the cities (plan_water.c): built by plan_world_init
 * once the settlements are placed, before the roads. */
void plan_water_build(int threads);
/* The pieces of channel across piece (ti, tj, ni, nj) of a built town: how
 * many, and *out pointing at them. */
int  plan_water_piece(int ti, int tj, int ni, int nj, const PlanChannel *const **out);
/* Fills out's channels and bridges, those near its rectangle. */
void plan_water_area(PlanArea *out);
