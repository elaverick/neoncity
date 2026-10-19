#pragma once
#include "terrain.h"

/* Terrain internals, shared by the terrain sources and tests/terrain_test.c.
 *
 *   terrain_height.c   the height field: urban ground blended into the land
 *   terrain_land.c     the land: rolling ground, regional landforms and canyons
 *   terrain_lod.c      the clipmap: where each level's window goes, the strips
 *                      a move exposes, the per-update budget, readiness, morph,
 *                      block bounds and the screen-space error's choice of levels
 *   terrain_mesh.c     filling a strip of a level: heights, parent heights, normals
 *   terrain_world.c    the planet: the chart world X/Z is, the cube-sphere and its
 *                      patches, the ground over the whole sphere, the bend and the horizon
 *   terrain_patch.c    the planet's patches: their meshes, the cache of them and the
 *                      quadtree selection of which to draw
 *
 * height = w * urban + (1 - w) * land, w = plan_urban(x, z):
 *   urban  slow relief under the city, wavelengths no shorter than
 *          TERRAIN_URBAN_WAVE / 2, so streets can follow it as gentle grades.
 *   land   gentle rolling ground at the city's edge; with distance beyond it,
 *          the region's landforms (see below).
 * Both are built of fractal sums of value noise with fixed octave counts. A
 * level of detail (terrain_height_lod) leaves out the octaves its spacing
 * can't carry but never renormalises the rest, so coarse levels don't reshape
 * the land. */

/* The city reads its ground from terrain_urban_height (city_ground_y): its
 * streets are draped onto it and its buildings stand on it. 0 flattens the
 * urban relief (the city on a datum at y = 0). */
#define TERRAIN_URBAN_GRADED   1
/* The urban relief's amplitude is the settlement's own (PlanSettlement.relief_m:
 * 10 m for the megacity, more for smaller ones). */
#define TERRAIN_URBAN_WAVE     3000.0   /* its longest wavelength (m) */
/* How far the drawn surface (TerrainSample) sits under terrain_height where
 * plan_urban is 1, in proportion to it elsewhere: the city's lowest ground,
 * the asphalt at y = -0.03 under its streets, then always wins the depth
 * test, wherever the city's chunks happen to be loaded. A coarse level whose
 * spacing can't follow the urban relief that closely (terrain_urban_lod_error,
 * for the megacity's 10 m: 0.16 m at level 7, 0.66 m at level 8, 4.2 m at
 * level 9, which leaves out the shorter urban octave; in proportion to the
 * relief elsewhere) sinks by that error plus TERRAIN_URBAN_SINK_CLEAR_M
 * instead (terrain_urban_sink): levels 8 and 9, only drawn from far off or high
 * up. The finer levels all sink alike, so the sink adds nothing to the
 * screen-space error of dropping one for its parent. */
#define TERRAIN_URBAN_SINK_M       0.5
#define TERRAIN_URBAN_SINK_CLEAR_M 0.25
#define TERRAIN_URBAN_OCTAVES  2

/* --- The land (terrain_land.c) ------------------------------------------------
 * land = base + M * macro + E * sum over types t of w_t * F_t - C * canyon
 *   base    rolling ground everywhere, TERRAIN_BASE_M high.
 *   macro   the planet's broad highlands and basins, TERRAIN_MACRO_M high, a
 *           fractal sum over the sphere itself (the point's direction, so it
 *           has no seam anywhere). Where it is high the mountains and
 *           plateaus gain affinity and the plains lose it, by
 *           TERRAIN_MACRO_BIAS a unit; low, the reverse. M is 0 out to
 *           TERRAIN_MACRO_FROM beyond the megacity's edge, rising to 1 at
 *           TERRAIN_MACRO_FULL, so its region is as it was. The other
 *           settlements' ground rises and falls with M * macro (slowly:
 *           TERRAIN_LIFT_SLOPE), so they stand on their highlands.
 *   F_t     each landform type's heights (TERRAIN_PLAINS ... ; terrain.h):
 *           plains, low swells; hills, rolling relief; mountain belts, an uplift
 *           under ridged relief whose ridges run along the belt's strike;
 *           plateaus, a level top raised by a steep step on a slow mask, which
 *           gives escarpments where the mask crosses its threshold.
 *   w_t     the region's mix of types (terrain_landform): slow affinity fields
 *           in a warped frame, the mountains' stretched along the strike so
 *           belts run as long ranges. A type more than TERRAIN_REGION_MARGIN
 *           behind the leading one has weight 0 (and isn't evaluated); the
 *           weights change continuously, so types blend into each other, and
 *           a plateau's top falls gently to its region's edge while its
 *           escarpments inside stay sharp.
 *   E       0 out to TERRAIN_REGION_FROM beyond the city's edge, rising to 1
 *           over TERRAIN_LAND_RISE: the city's surroundings stay gentle.
 *   canyon  explicit meandering paths, one or none per TERRAIN_CANYON_CELL
 *           square, cut into whatever they cross: a level floor, steep walls,
 *           tapering at both ends. C keeps them TERRAIN_CANYON_FROM clear of
 *           the city's edge.
 * Every length is world metres; every height, metres above y = 0. */
#define TERRAIN_BASE_M         12.0
#define TERRAIN_MACRO_M        1500.0
#define TERRAIN_MACRO_WAVE     3.0e6    /* longest wavelength over the sphere (m) */
#define TERRAIN_MACRO_OCTAVES  4
#define TERRAIN_MACRO_FROM     40000.0
#define TERRAIN_MACRO_FULL     400000.0
#define TERRAIN_MACRO_BIAS     0.25
/* A bound on how steep M * macro is (m/m): M rises over FULL - FROM as a
 * smoothstep (1.5 / (FULL - FROM) at its steepest) of an edge measure that
 * changes by at most CITY_EDGE_RATE a metre, times |macro| <= 1; and the
 * macro's fractal sum of value noise over the sphere changes by at most
 * 3 sqrt(3) / L per metre an octave of wavelength L, times its weight (the
 * chart's distances are never shorter than the sphere's). */
#define TERRAIN_LIFT_SLOPE (TERRAIN_MACRO_M * (1.5 * CITY_EDGE_RATE / (TERRAIN_MACRO_FULL - TERRAIN_MACRO_FROM) + \
                            3.0 * 1.7320508075688772 * TERRAIN_MACRO_OCTAVES / (1.875 * TERRAIN_MACRO_WAVE)) + 1e-4)
#define TERRAIN_BASE_WAVE      4000.0   /* longest wavelength (m) */
#define TERRAIN_BASE_OCTAVES   6
#define TERRAIN_REGION_FROM    1000.0
#define TERRAIN_LAND_RISE      6000.0
#define TERRAIN_REGION_WAVE    18000.0  /* the affinity fields' wavelength (m) */
#define TERRAIN_REGION_MARGIN  0.12     /* in affinity, which runs 0..1 */
#define TERRAIN_WARP_M         4000.0   /* the region frame's warp */
#define TERRAIN_WARP_WAVE      20000.0
#define TERRAIN_STRIKE_X       0.8196480178454796   /* the mountain belts' strike: the unit */
#define TERRAIN_STRIKE_Z       0.5728674601004813   /* direction 0.61 rad from +x towards +z */
#define TERRAIN_BELT_ALONG     2.5      /* a belt's affinity, stretched along the strike... */
#define TERRAIN_BELT_ACROSS    0.7      /* ...and squeezed across it */
#define TERRAIN_PLAINS_BIAS    0.6      /* the plains' affinity: where nothing else rises above it */
#define TERRAIN_PLAINS_M       20.0
#define TERRAIN_PLAINS_WAVE    5000.0
#define TERRAIN_PLAINS_OCTAVES 4
#define TERRAIN_HILLS_M        230.0
#define TERRAIN_HILLS_WAVE     4000.0
#define TERRAIN_HILLS_OCTAVES  6
#define TERRAIN_UPLIFT_M       300.0    /* a belt's broad rise... */
#define TERRAIN_RIDGE_M        800.0    /* ...and its ridges' relief over it */
#define TERRAIN_RIDGE_WAVE     4500.0   /* ridges' longest wavelength across the strike (m)... */
#define TERRAIN_RIDGE_ALONG    2.4      /* ...and this many times longer along it: the main ridges;
                                           the spurs off them (the finer octaves) run every way */
#define TERRAIN_RIDGE_WARP_M   700.0    /* the ridges' frame warped by this much... */
#define TERRAIN_RIDGE_WARP_WAVE 2500.0  /* ...this slowly, so they don't follow value noise's lattice */
#define TERRAIN_RIDGE_OCTAVES  8
#define TERRAIN_PLATEAU_M      320.0    /* a plateau's top over the land below its escarpments */
#define TERRAIN_MASK_WAVE      6000.0   /* the plateau mask's longest wavelength (m) */
#define TERRAIN_MASK_OCTAVES   3
#define TERRAIN_MASK_LEVEL     0.0      /* the mask's value at the foot of the step... */
#define TERRAIN_MASK_STEP      0.12     /* ...and how much more it takes to climb it */
#define TERRAIN_TOP_M          25.0     /* relief on a plateau's top */
#define TERRAIN_TOP_WAVE       3000.0
#define TERRAIN_TOP_OCTAVES    3
#define TERRAIN_CANYON_CELL    20000.0
#define TERRAIN_CANYON_CHANCE  0.7      /* of a cell holding a canyon */
#define TERRAIN_CANYON_DEEP_M  320.0    /* the deepest (each 180 m or more) */
#define TERRAIN_CANYON_WALL    1.6      /* walls' mean slope: depth over run */
#define TERRAIN_CANYON_FROM    2500.0   /* none nearer the city's edge than this... */
#define TERRAIN_CANYON_FULL    6000.0   /* ...nor at full depth nearer than this */
/* How high and how deep the land can reach (m). */
#define TERRAIN_LAND_TOP_M  (TERRAIN_BASE_M + TERRAIN_MACRO_M + TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M)
#define TERRAIN_LAND_DEEP_M (TERRAIN_BASE_M + TERRAIN_MACRO_M + TERRAIN_HILLS_M + TERRAIN_CANYON_DEEP_M)

/* The two fields before blending. urban_shape is unit amplitude, [-1, 1];
 * land is in metres. */
double terrain_urban_shape(double x, double z);
double terrain_land(double x, double z);
/* The land at `spacing`, given plan_city_edges at the point (terrain_height_lod's):
 * the edge measure, and the megacity's own. */
double terrain_land_lod(double x, double z, double edge, double home, double spacing);
/* The same, and M * macro in it in *lift: the planet's highlands, which a
 * settlement's ground rises and falls with too (terrain_height.c). */
double terrain_land_parts(double x, double z, double edge, double home, double spacing, double *lift);
/* M * macro alone, at `spacing`, for the megacity's edge measure `home`: 0
 * within TERRAIN_MACRO_FROM of the megacity. */
double terrain_lift(double x, double z, double home, double spacing);
/* The macro field itself over the sphere, in [-1, 1]. */
double terrain_macro(TerrainDir p, double spacing);
/* |M * macro| is at most this where the megacity's edge measure is at most `home`. */
double terrain_lift_bound(double home);
/* The lift under a settlement's ground at (x, z): terrain_lift at full detail,
 * for the megacity's measure there. terrain_urban_height is it plus the relief. */
double terrain_urban_lift(double x, double z);
/* The same at a point of some chart, (x, z) in it and p its direction on the
 * planet: the macro field reads p, the rest (x, z). terrain_land_lod is this
 * in the world's chart (terrain_chart_dir); terrain_world.c's second chart
 * passes its own. */
double terrain_land_at(double x, double z, TerrainDir p, double edge, double spacing);
/* Bounds on the land wherever plan_city_edge is at most `edge` and the
 * megacity's own measure at most `home` (m). */
void   terrain_land_range(double edge, double home, double *lo, double *hi);
/* A bound on how far the land at `spacing` strays from the land at full detail
 * at (x, z) (m): the octaves left out, through each field's weight and slope. */
double terrain_land_lod_error(double x, double z, double spacing);
/* The ground without the roads between the settlements (terrain_height_lod
 * less plan_road_ground's blend), and a bound on how far the roads alone move
 * terrain_height_lod at `spacing` from full detail (m). */
double terrain_height_bare(double x, double z, double spacing);
double terrain_road_lod_error(double x, double z, double spacing);
/* How deep the canyons cut at (x, z) before C keeps them from the city (m). */
double terrain_canyon_depth(double x, double z);
/* Which octaves every fractal sum keeps at `spacing`, as one number: two
 * spacings with the same key evaluate the same heights. */
uint32_t terrain_lod_key(double spacing);
/* Of a fractal sum of `octaves` octaves whose longest wavelength is `wave`,
 * the share of its weight left out at `spacing`. */
double terrain_dropped(double wave, int octaves, double spacing);

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
/* A bound on how far urban relief of amplitude `relief` (m) drawn at
 * `spacing` strays from the city's ground (m); 0 at spacing 0. */
double terrain_urban_lod_error(double spacing, double relief);
/* How far the surface drawn at `spacing` sinks where plan_urban is 1, over
 * ground of that relief (m). */
double terrain_urban_sink(double spacing, double relief);

/* Vector arithmetic on planet-frame directions and points (double). */
static inline double dot3(TerrainDir a, TerrainDir b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline TerrainDir cross3(TerrainDir a, TerrainDir b) {
    TerrainDir r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
static inline TerrainDir scale3(TerrainDir a, double s) { TerrainDir r = { a.x * s, a.y * s, a.z * s }; return r; }
static inline TerrainDir add3(TerrainDir a, TerrainDir b) { TerrainDir r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static inline TerrainDir sub3(TerrainDir a, TerrainDir b) { TerrainDir r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static inline TerrainDir unit3(TerrainDir a) { return scale3(a, 1.0 / sqrt(dot3(a, a))); }

/* Scratch sizes for a fill: a span (at most W a side) with a one-sample apron
 * for normals, and the parent samples under it. */
#define TERRAIN_FILL_SIDE   (TERRAIN_CLIP_W + 2)
#define TERRAIN_PARENT_SIDE ((TERRAIN_CLIP_W + 1) / 2 + 1)

/* Fills the samples of `span` (which must not wrap in storage; see
 * terrain_lod.c) on level `level` of c: heights at the level's spacing, the
 * parent's surface (hc), normals and urban weights. Every value written is a pure function of
 * the level and the world sample. Returns the heights it evaluated (at most
 * terrain_clip_fill_cost). */
int  terrain_mesh_fill(TerrainClipmap *c, int level, TerrainSpan span);
