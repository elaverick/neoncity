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

/* A grid over the sphere (plan_world.c): on each cube face
 * (terrain_face_dir) n nodes a side, node (f, i, j) at u = -1 + 2 i / (n - 1)
 * and v likewise, so the nodes on a face's edges are its neighbour's too.
 * plan_cube_side is the n whose nodes are no more than `spacing` apart (the
 * faces' middles are where they are farthest apart); plan_cube_at gives the
 * face p is on and its place there in nodes, in [0, n - 1] each way;
 * plan_cube_step the node (di, dj) from node (f, i, j), stepping over a
 * face's edge onto the nearest node of the next face (returns its face). */
int        plan_cube_side(double spacing);
size_t     plan_cube_index(int n, int f, int i, int j);
TerrainDir plan_cube_dir(int n, int f, int i, int j);
int        plan_cube_at(int n, TerrainDir p, double *fi, double *fj);
int        plan_cube_step(int n, int f, int i, int j, int di, int dj, int *gi, int *gj);

/* The road network (plan_roads.c): built by plan_world_init once the
 * settlements are placed, on up to `threads` threads. It publishes each
 * settlement's frame's roads through plan_world_frame_roads. */
void plan_roads_build(int threads);
void plan_world_frame_roads(int k, const void *roads);

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
