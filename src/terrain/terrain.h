#pragma once
#include "common.h"

/* Terrain: the ground surface of the world, everywhere, independent of the
 * city chunks and of the camera. Heights are world Y in metres; X and Z are
 * the world ground plane in metres (double precision, as in src/plan). Every
 * function here is a pure function of position and WORLD_SEED: a point
 * evaluates to the same height whenever, and through whichever query, it is
 * asked for.
 *
 * Under the city the ground is kept gentle (see plan_urban); beyond the
 * city's edge the land rises into hills. */

/* Ground height at a point. */
double terrain_height(double x, double z);

/* Conservative bounds on terrain_height over the rectangle [x0,x1] x [z0,z1]:
 * every height inside lies in [*lo, *hi]. Cheap; not tight. */
void   terrain_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi);
