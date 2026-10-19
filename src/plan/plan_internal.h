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
