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
 *   terrain_world.c    the planet: the chart world X/Z is, the bend, the horizon and
 *                      the lowest ground (the cube-sphere is terrain_cube.c)
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
 * land = base + macro + E * g * sum over types t of w_t * F_t - C * s * canyon
 *   base    rolling ground everywhere, TERRAIN_BASE_M high.
 *   macro   the planet's broad highlands and basins: the Earth
 *           (terrain_earth_height) less the datum E0, its height under the
 *           megacity's centre, so world Y = elevation - E0 and the sea will stand at
 *           Y = -E0. Under the settlements and their belts it is read at
 *           TERRAIN_LIFT_SPACING, so a city's grade stays tiny; out on the
 *           land the data's own detail blends in from TERRAIN_DETAIL_FROM
 *           beyond the nearest edge to TERRAIN_DETAIL_FULL. Without a bake
 *           the procedural stand-in, TERRAIN_MACRO_M times a fractal sum
 *           over the sphere, takes its place with E0 = 0. The landforms
 *           follow the Earth (terrain_earth_land, below at w_t, F_t, g and
 *           s); without a bake, where the stand-in is high the mountains and
 *           plateaus gain affinity and the plains lose it, by
 *           TERRAIN_MACRO_BIAS a unit of it; low, the reverse. Every
 *           settlement's ground, the megacity's too, rises and falls with
 *           macro, so it stands on its highlands.
 *   F_t     each landform type's heights (TERRAIN_PLAINS ... ; terrain.h):
 *           plains, low swells; hills, rolling relief; mountain belts, an uplift
 *           under ridged relief whose ridges run along the belt's strike;
 *           plateaus, a level top raised by a steep step on a slow mask, which
 *           gives escarpments where the mask crosses its threshold. The
 *           strike is the Earth's own where its ridges have one (blended by
 *           its coherence with TERRAIN_STRIKE_*): the ridges are
 *           drawn in TERRAIN_STRIKES frames turned from the default, the two
 *           either side of the strike blended. Where the data already
 *           carries the relief the belts are scaled down, to
 *           TERRAIN_RIDGE_KEEP of their height from TERRAIN_KEEP_FULL_M of
 *           relief, so the ranges don't double up.
 *   w_t     the region's mix of types (terrain_landform): slow affinity fields
 *           in a warped frame, the mountains' stretched along the strike so
 *           belts run as long ranges, which give TERRAIN_DATA_SHARE of their
 *           place to the Earth's: much relief round a point
 *           makes mountains, some hills, high and level ground plateaus, low
 *           and level ground plains, and the hints' override wins where it
 *           is given. A type more than TERRAIN_REGION_MARGIN
 *           behind the leading one has weight 0 (and isn't evaluated); the
 *           weights change continuously, so types blend into each other, and
 *           a plateau's top falls gently to its region's edge while its
 *           escarpments inside stay sharp.
 *   E       0 out to TERRAIN_REGION_FROM beyond the city's edge, rising to 1
 *           over TERRAIN_LAND_RISE: the city's surroundings stay gentle.
 *   canyon  explicit meandering paths, one or none per cell of a lattice
 *           on each cube face, TERRAIN_CANYON_CELL across at the face's
 *           middle, cut into whatever they cross: a level floor, steep walls,
 *           tapering at both ends. C keeps them TERRAIN_CANYON_FROM clear of
 *           the city's edge.
 * The land is a function of the direction on the planet: base, the region
 * and the types are value noise in three dimensions at R times the
 * direction, and the belts and ridges are drawn in fixed frames, one per
 * tile of the cube's faces, blended across the tiles' edges
 * (terrain_land.c). Only E, C and the Earth's detail read the world's chart,
 * through the edge measure. Every length is metres; every height, metres above y = 0. */
#define TERRAIN_BASE_M         12.0
#define TERRAIN_MACRO_M        1500.0
#define TERRAIN_MACRO_WAVE     3.0e6    /* longest wavelength over the sphere (m) */
#define TERRAIN_MACRO_OCTAVES  4
#define TERRAIN_MACRO_BIAS     0.25     /* the stand-in's, without a bake */
#define TERRAIN_DATA_SHARE     0.8      /* the Earth's share of the affinities */
#define TERRAIN_RIDGE_KEEP     0.35     /* the mountains' height kept where the data has the relief... */
#define TERRAIN_KEEP_FROM_M    500.0    /* ...none of it taken below this relief (m)... */
#define TERRAIN_KEEP_FULL_M    2500.0   /* ...and all of it from here */
#define TERRAIN_SEA_FLOOR_M    -200.0   /* the canyons, none below this elevation, all from sea level (m) */
#define TERRAIN_LIFT_SPACING   20000.0  /* the Earth under the settlements, read this coarsely (m) */
#define TERRAIN_DETAIL_FROM    2000.0   /* the Earth's own detail: none nearer an edge than this... */
#define TERRAIN_DETAIL_FULL    15000.0  /* ...and all of it from here (within PLAN_EDGE_CAP) */
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
#define TERRAIN_STRIKES        8        /* the ridges' frames, each pi / 8 on from the last */
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
/* How high and how deep the land can reach above and below its lift (m;
 * terrain_lift_bound is the lift's own reach). */
#define TERRAIN_LAND_TOP_M  (TERRAIN_BASE_M + TERRAIN_UPLIFT_M + TERRAIN_RIDGE_M)
#define TERRAIN_LAND_DEEP_M (TERRAIN_BASE_M + TERRAIN_HILLS_M + TERRAIN_CANYON_DEEP_M)

/* An edge measure past every rise (E, C and the Earth's detail all 1): the
 * land as it would be with no settlement near, which the streams are
 * derived from (terrain_earth_load_streams). */
#define TERRAIN_WILD_EDGE 1.0e9

/* The water carves the land down to its floor (terrain_earth_water) by at
 * most this; it must stay under TERRAIN_WATER_BANK times the water's reach,
 * so no segment beyond the reach can carve (m). Whether the water has its
 * beds (the streams' file is loaded): without them it carves nothing. */
#define TERRAIN_WATER_CARVE_M 30.0
/* The rivers' channels through the cities (plan.h, "Rivers through the
 * cities") are cut into the ground this far past their outlines across the
 * towns (m): one and a half times the spacing of the coarsest level of
 * detail the city's chunks are drawn over (32 m, to VIEW_DISTANCE), so no
 * triangle of it over a channel reaches out of the cut. Every level to the
 * last spacing cuts them so (m); no coarser one does. */
#define TERRAIN_CHANNEL_CITY_PAD    48.0
#define TERRAIN_CHANNEL_LOD_SPACING 64.0
/* Past the cut's floor its banks rise to the city's ground over this (m);
 * the floor's reach and this stay within PLAN_CHANNEL_REACH. */
#define TERRAIN_CHANNEL_BANK_M      40.0
int    terrain_earth_beds(void);
/* The ground at p as height_lod makes it but with neither the roads between
 * the settlements nor the water's beds in it: the land the streams are
 * derived from, with the settlements (tools/earth_streams.c). */
double terrain_ground_bare_dir(TerrainDir p, double spacing);
/* The bake's water as loaded, for the tools: its points (xyz V x 3, widths,
 * kinds, flags; returns V, 0 with none) and its buckets (returns B). */
uint32_t terrain_earth_water_points(const float **xyz, const uint16_t **width, const uint8_t **kind, const uint8_t **flags);
int      terrain_earth_water_buckets(const uint8_t **inside, const uint32_t **start, const uint32_t **entry);

/* The urban field before blending, unit amplitude, [-1, 1]. */
double terrain_urban_shape(double x, double z);
/* The land at direction d and `spacing`, given plan_city_edge at its place
 * (terrain_height_lod's); and macro in it in *lift: the planet's highlands,
 * which a settlement's ground rises and falls with too (terrain_height.c).
 * The procedural land (the base, the landforms, the canyons) fades out over
 * TERRAIN_VALLEY_M towards the real water's edge (terrain_earth_valley) and
 * is gone in it; _at takes that distance already read (a terrain_earth_water's
 * valley), and is otherwise the same. */
#define TERRAIN_VALLEY_M 1000.0   /* under the water's reach (2 km), so the fade is exact */
double terrain_land_parts(TerrainDir d, double edge, double spacing, double *lift);
double terrain_land_parts_at(TerrainDir d, double edge, double spacing, double valley, double *lift);
/* macro alone, at `spacing`, as it is wherever plan_urban is above 0 (the
 * Earth's detail is left out there). */
double terrain_lift(double x, double z, double spacing);
/* The procedural stand-in for the Earth over the sphere, in [-1, 1]. */
double terrain_macro(TerrainDir p, double spacing);
/* |macro| is at most this. */
double terrain_lift_bound(void);
/* The lift under a settlement's ground at (x, z): terrain_lift at full
 * detail. terrain_urban_height is it plus the relief. */
double terrain_urban_lift(double x, double z);
/* Bounds on terrain_urban_lift over the rectangle (m). */
void   terrain_urban_lift_range(double x0, double z0, double x1, double z1, double *lo, double *hi);

/* The Earth's part in the chart and the land (terrain_earth.c), fixed at
 * load: the chart's basis about the bake's origin, x east and z south turned
 * by the heading (0 without a bake, and the procedural chart stands); the
 * datum E0, the Earth at the origin at TERRAIN_LIFT_SPACING (0 without a
 * bake); the hints' largest roughness (1 without them); and the Earth's
 * part of terrain_lod_key (0 wherever it reads mip 0 alone, or has no bake). */
int      terrain_earth_chart(TerrainChart *c);
double   terrain_earth_datum(void);
double   terrain_earth_rough_max(void);
uint32_t terrain_earth_lod_key(double spacing);
/* The Earth's affinity for each landform type at p, each in [0, 1], as the
 * land takes them (terrain_land.c); 0 without a bake. */
int      terrain_earth_forms(TerrainDir p, float a[TERRAIN_TYPES]);
/* terrain_earth_range over only the mips `spacing` itself reads. */
void     terrain_earth_range_at(TerrainDir p, double radius, double spacing, double *lo, double *hi);
/* The default strike of the mountain belts at p, along the sphere: the
 * frame's that leads there (terrain_land.c). */
TerrainDir terrain_frame_strike(TerrainDir p);
/* Bounds on the land wherever plan_city_edge is at most `edge` (m). */
void   terrain_land_range(double edge, double *lo, double *hi);
/* A bound on how far the land at `spacing` strays from the land at full detail
 * at (x, z) (m): the octaves left out, through each field's weight and slope. */
double terrain_land_lod_error(double x, double z, double spacing);
/* The ground without the roads between the settlements (terrain_height_lod
 * less plan_road_ground's blend), and a bound on how far the roads alone move
 * terrain_height_lod at `spacing` from full detail (m). */
double terrain_height_bare(double x, double z, double spacing);
double terrain_road_lod_error(double x, double z, double spacing);
/* A bound on how far the rivers' channels through the cities alone move
 * terrain_height_lod at `spacing` from full detail (m). */
double terrain_channel_lod_error(double x, double z, double spacing);
/* How deep the canyons cut at (x, z) before C keeps them from the city (m). */
double terrain_canyon_depth(double x, double z);
/* Which octaves every fractal sum keeps at `spacing`, and the Earth's mips,
 * as one number: two spacings with the same key evaluate the same heights. */
uint64_t terrain_lod_key(double spacing);
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
