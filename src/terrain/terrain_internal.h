#pragma once
#include "terrain.h"

/* Terrain internals, shared by the terrain sources and tests/terrain_test.c.
 *
 *   terrain_height.c   the height field: urban ground blended into the land
 *
 * height = w * urban + (1 - w) * land, w = plan_urban(x, z):
 *   urban  slow relief under the city, wavelengths no shorter than
 *          TERRAIN_URBAN_WAVE / 2, so streets can follow it as gentle grades.
 *   land   rolling fractal relief, low at the city's edge and rising with
 *          distance beyond it into hills. A placeholder for real landforms.
 * Both are fractal sums of value noise with a fixed octave count, so no
 * octave is ever added or renormalised by level of detail. */

/* The city's streets are still built flat at y = 0 (src/city), so its relief
 * stays off until the city builds on terrain_height. */
#define TERRAIN_URBAN_GRADED   0
/* Urban relief amplitude when graded (m). Megacities are kept nearly flat;
 * once the plan has more than one settlement, this becomes the settlement's
 * own, larger for small towns. */
#define TERRAIN_URBAN_RELIEF_M 10.0
#define TERRAIN_URBAN_WAVE     3000.0   /* its longest wavelength (m) */
#define TERRAIN_URBAN_OCTAVES  2

#define TERRAIN_LAND_NEAR_M    12.0     /* land relief amplitude at the city's edge (m) */
#define TERRAIN_LAND_FAR_M     280.0    /* ... and once the land has fully risen (m) */
#define TERRAIN_LAND_RISE      6000.0   /* distance beyond the edge over which it rises (m) */
#define TERRAIN_LAND_WAVE      4000.0   /* land's longest wavelength (m) */
#define TERRAIN_LAND_OCTAVES   6

/* The two fields before blending. urban_shape is unit amplitude, [-1, 1];
 * land is in metres. */
double terrain_urban_shape(double x, double z);
double terrain_land(double x, double z);
/* Land relief amplitude at a given plan_city_edge value (m); non-decreasing. */
double terrain_land_amplitude(double edge);
