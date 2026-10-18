#pragma once
#include "terrain.h"

/* Terrain internals, shared by the terrain sources and tests/terrain_test.c.
 *
 *   terrain_height.c   the height field: urban ground blended into the land
 *   terrain_lod.c      the clipmap: where each level's window goes, the strips
 *                      a move exposes, the per-update budget, readiness, morph
 *   terrain_mesh.c     filling a strip of a level: heights, parent heights, normals
 *
 * height = w * urban + (1 - w) * land, w = plan_urban(x, z):
 *   urban  slow relief under the city, wavelengths no shorter than
 *          TERRAIN_URBAN_WAVE / 2, so streets can follow it as gentle grades.
 *   land   rolling fractal relief, low at the city's edge and rising with
 *          distance beyond it into hills. A placeholder for real landforms.
 * Both are fractal sums of value noise with a fixed octave count. A level of
 * detail (terrain_height_lod) leaves out the octaves its spacing can't carry
 * but never renormalises the rest, so coarse levels don't reshape the land. */

/* The city reads its ground from terrain_urban_height (city_ground_y): its
 * streets are draped onto it and its buildings stand on it. 0 flattens the
 * urban relief (the city on a datum at y = 0). */
#define TERRAIN_URBAN_GRADED   1
/* Urban relief amplitude when graded (m). Megacities are kept nearly flat;
 * once the plan has more than one settlement, this becomes the settlement's
 * own, larger for small towns. */
#define TERRAIN_URBAN_RELIEF_M 10.0
#define TERRAIN_URBAN_WAVE     3000.0   /* its longest wavelength (m) */
/* How far the drawn surface (TerrainSample) sits under terrain_height where
 * plan_urban is 1, in proportion to it elsewhere: the city's lowest ground,
 * the asphalt at y = -0.03 under its streets, then always wins the depth
 * test, wherever the city's chunks happen to be loaded. */
#define TERRAIN_URBAN_SINK_M   0.5
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

/* An octave is kept at a sample spacing while its wavelength spans at least
 * this many samples. Below about 4, linear interpolation between samples
 * flattens its crests by a third or more, and at 2 it aliases. */
#define TERRAIN_LOD_SAMPLES_PER_WAVE 4.0
/* Of a fractal sum whose longest wavelength is `wave` (m), the number of its
 * `octaves` kept at `spacing` (m); all of them at spacing 0. */
int terrain_octaves(double wave, int octaves, double spacing);

/* The drawn surface: terrain_height_lod less the urban sink. *urban, if not
 * NULL, gets plan_urban at the point. */
double terrain_surface_lod(double x, double z, double spacing, float *urban);

/* Scratch sizes for a fill: a span (at most W a side) with a one-sample apron
 * for normals, and the parent samples under it. */
#define TERRAIN_FILL_SIDE   (TERRAIN_CLIP_W + 2)
#define TERRAIN_PARENT_SIDE ((TERRAIN_CLIP_W + 1) / 2 + 1)

/* Fills the samples of `span` (which must not wrap in storage; see
 * terrain_lod.c) on level `level` of c: heights at the level's spacing, the
 * parent's surface (hc), normals and urban weights. Every value written is a pure function of
 * the level and the world sample. */
void terrain_mesh_fill(TerrainClipmap *c, int level, TerrainSpan span);
