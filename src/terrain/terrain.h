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

/* The ground as drawn, at full detail: terrain_height, sunk a little under
 * the city (see TerrainSample). What stands beyond the city's edge stands on it. */
double terrain_surface(double x, double z);

/* The ground the city builds on (city_ground_y in src/city): the urban relief
 * alone, never blended into the land, so it stays gentle everywhere. Where
 * plan_urban is 1 it is terrain_height, bit for bit, and the city is only
 * ever built there (CITY_BUILT_EDGE). */
double terrain_urban_height(double x, double z);

/* Conservative bounds on terrain_urban_height over the rectangle [x0,x1] x
 * [z0,z1], as terrain_height_range: from its height at the centre and how
 * steep it can be, so a chunk's are a metre or two apart. */
void   terrain_urban_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi);

/* The same ground as sampled every `spacing` metres: detail too fine for that
 * spacing to carry is left out, the rest is unchanged (no renormalising, so
 * coarse samples don't drift from terrain_height by more than the dropped
 * detail). spacing 0 is terrain_height, bit for bit. */
double terrain_height_lod(double x, double z, double spacing);

/* Conservative bounds on terrain_height (and terrain_height_lod at any
 * spacing) over the rectangle [x0,x1] x [z0,z1]: every height inside lies in
 * [*lo, *hi]. Cheap; not tight. */
void   terrain_height_range(double x0, double z0, double x1, double z1, double *lo, double *hi);

/* The region's landforms (src/terrain/terrain_land.c): the mix of landform
 * types at a point, weights in [0, 1] summing to 1, and how deep a canyon cuts
 * there (m). Near the city the land stays gentle whatever the mix. A pure
 * function of position, for finding level ground for towns and for the tests. */
enum { TERRAIN_PLAINS = 0, TERRAIN_HILLS, TERRAIN_MOUNTAINS, TERRAIN_PLATEAU, TERRAIN_TYPES };
typedef struct TerrainLandform { float w[TERRAIN_TYPES]; float canyon; } TerrainLandform;
void   terrain_landform(double x, double z, TerrainLandform *out);

/* --- The planet (src/terrain/terrain_world.c) ----------------------------------
 * The world is a sphere of PLANET_RADIUS, and world X/Z is a chart of it: the
 * azimuthal-equidistant projection about the chart's origin, world (0, 0), so
 * a point's distance from the origin in X/Z is its distance over the sphere.
 * Y is height above the sphere. The city, the plan and the clipmap all work
 * in the chart; over the 20 km round the city it is the sphere's tangent plane
 * to within a millimetre in a kilometre or two (terrain_bend).
 *
 * The planet frame has its origin at the planet's centre, +Z through the
 * north pole; a TerrainDir in it is a direction (unit) or a point (metres).
 * The chart's origin is at a fixed place on the planet, and world +x points
 * east there, +y up and +z south.
 *
 * The planet's surface is addressed as a cube-sphere: the six faces of a cube,
 * each a square [-1, 1]^2 of (u, v) projected out onto the sphere. Its
 * projection is equal-angle (u = atan(cube coordinate) * 4 / pi), so patches
 * at a level are near enough the same size everywhere. Face f's normal is
 * axis f / 2 of the planet frame, positive for even f; u and v run along the
 * other two axes in increasing order, both positive, so two faces sharing an
 * edge parametrise it alike and evaluate its points bit for bit the same. */
#include "terrain_planet.h"   /* PLANET_RADIUS: shared with the shaders */

typedef struct TerrainDir { double x, y, z; } TerrainDir;

/* The direction of world (x, z), in the planet frame, and back. The chart
 * covers the sphere but for the origin's antipode, at distance pi R. */
TerrainDir terrain_chart_dir(double x, double z);
void       terrain_dir_chart(TerrainDir p, double *x, double *z);
/* World (x, y, z) as a point in the planet frame (m). */
TerrainDir terrain_chart_point(double x, double y, double z);
/* The local frame at world (x, z): up along the sphere's normal, and x and z
 * along the chart's x and z there, orthogonal to it and to each other. */
void       terrain_chart_frame(double x, double z, TerrainDir *ax, TerrainDir *up, TerrainDir *az);

/* A point at world (cam_x + dx, h, cam_z + dz) as seen from a camera at world
 * (cam_x, cam_y, cam_z), in the camera's local frame (terrain_chart_frame):
 * the ground curving down away from it. In float, as the shaders compute it
 * (planetBend in shaders/common.glsl); exact for a camera at the chart's
 * origin, and for any camera close to it the chart's distortion is small
 * (terrain_test measures it). */
void       terrain_bend(float dx, float dz, float h, float cam_y, float out[3]);
/* Widens a camera-relative box (world heights less the camera's, as for a
 * flat world) to hold its bent image, for frustum culling what the shaders
 * bend: the bend only lowers, by up to s^2 / 2R at s off, and moves across by
 * a part in a thousand at most, for ground lower than 6 km. */
void       terrain_bend_bounds(float mn[3], float mx[3]);

/* Cube-sphere addressing. A direction for face coordinates (u, v), which may
 * reach a little past the face's edge; the face holding a direction and its
 * (u, v) there. */
TerrainDir terrain_face_dir(int face, double u, double v);
int        terrain_dir_face(TerrainDir p, double *u, double *v);

/* A patch: the square of face `face` at quadtree level `level`, whose (u, v)
 * runs over cells (i, j) of 2^level a side. Its identity is these four
 * numbers, never where it sits in a list. */
#define TERRAIN_PATCH_MAX_LEVEL 24
typedef struct TerrainPatch { int face, level; int64_t i, j; } TerrainPatch;
/* Vertex (gi, gj) of a patch's grid of `cells` cells a side, gi and gj in
 * [0, cells]: the same direction, bit for bit, as the same point asked for
 * through any other patch, level or face that has it. */
TerrainDir   terrain_patch_dir(TerrainPatch t, int cells, int gi, int gj);
TerrainPatch terrain_patch_at(TerrainDir p, int level);
TerrainPatch terrain_patch_parent(TerrainPatch t);
TerrainPatch terrain_patch_child(TerrainPatch t, int k);          /* k = 0..3: i + 2 j of the quarter */
/* The patch of the same level across side `side` (0 -u, 1 +u, 2 -v, 3 +v),
 * on this face or the next. */
TerrainPatch terrain_patch_neighbour(TerrainPatch t, int side);

/* The ground on the planet: terrain_height_lod / terrain_surface_lod at the
 * chart's position for p over the hemisphere round the city, bit for bit.
 * Towards the antipode, where the chart stretches, the land is blended into
 * the land of a second chart centred on the antipode. */
double terrain_planet_height_lod(TerrainDir p, double spacing);
double terrain_planet_surface_lod(TerrainDir p, double spacing);
/* No ground anywhere on the planet lies lower than this (m, world Y). */
double terrain_planet_lowest(void);

/* Horizon culling: whether a ball (centre, radius; planet frame, m) is wholly
 * hidden from a camera (planet frame) by a sphere of radius `occluder` round
 * the planet's centre, the planet's lowest ground (terrain_planet_lowest)
 * making a sphere nothing can be seen through. Conservative: a ball it
 * reports hidden is hidden. */
int        terrain_horizon_hidden(TerrainDir cam, TerrainDir centre, double radius, double occluder);

/* --- Clipmap -----------------------------------------------------------------
 * Nested square grids of terrain samples round the camera, for drawing the
 * ground out to the horizon (Losasso and Hoppe's geometry clipmaps). Level l
 * samples the ground every TERRAIN_CLIP_SPACING0 * 2^l metres, at world sample
 * indices (i, j) = (x, z) / spacing. Its window is the TERRAIN_CLIP_W x
 * TERRAIN_CLIP_W samples from its origin, kept within one sample of the
 * camera's centre. Origins are even, and W - 1 is even, so a window's edges
 * lie on its parent's (level l + 1's) samples, and it sits inside the
 * parent's window with about a quarter of the parent's width to spare on each
 * side. Each level is drawn as the ring between its own window and its
 * child's.
 *
 * Samples are stored toroidally: world sample (i, j) of a level lives in slot
 * (i mod N, j mod N) of its N x N array, so when the window moves only the
 * newly exposed strips are filled, and a slot's contents depend only on the
 * world sample, never on the camera's path.
 *
 * Geomorph: towards the outer edge of its window a level blends into its
 * parent (terrain_clip_morph), reaching it exactly at the edge, so neighbouring
 * rings meet without cracks or pops. */

#include "terrain_clip.h"   /* TERRAIN_CLIP_LEVELS, _N, _W, _SPACING0, _MORPH: shared with the shaders */
/* Heights evaluated per update, at most: a fill evaluates its band, a
 * one-sample apron for the normals and the parent's samples under it
 * (terrain_clip_fill_cost), so a thin strip costs about 2.5 times the samples
 * it writes and a whole window about 1.3 times. A height takes about 90 ns
 * under the city and 250-500 ns over the region's landforms (the mountains
 * most), so a full update takes 1-9 ms, on the renderer's worker thread
 * (src/render/terrain_gpu.c). Walking spends a few hundred; the most steady motion asks
 * for is a camera dithering across a snap line of every level at once (every
 * 1024 m), each level moving a sample pair each way: about 1,280 a level, 1,020
 * for the coarsest, 12,560 in all. A jump fills in over about 17 updates,
 * coarsest level first. */
#define TERRAIN_CLIP_BUDGET   13312
#define TERRAIN_CLIP_MAX_SPANS 8                      /* pieces of one level's update (see terrain_lod.c) */
#define TERRAIN_CLIP_MAX_DIRTY (2 * TERRAIN_CLIP_MAX_SPANS)

/* One sample of the drawn surface: terrain_height_lod at the level's spacing,
 * sunk a little under the city (see terrain_internal.h) so the city's own
 * ground always covers it. Heights are world Y (m); (nx, nz) are the x and z
 * components of the unit normal (ny = sqrt(1 - nx^2 - nz^2)), from the
 * level's own heights. hc is the parent level's surface at this sample: its
 * sample where the two coincide (i and j even), the mean of the two parent
 * samples either side along an edge, and at a parent cell's centre the mean
 * of the two ends of the cell's diagonal from (i - 1, j - 1) to
 * (i + 1, j + 1), which is how the parent's grid must be triangulated. On the
 * coarsest level hc = h. urban is plan_urban there: 1 in the city, 0 beyond
 * its belt (the renderer hazes the two differently). */
typedef struct TerrainSample { float h, hc, nx, nz, urban; } TerrainSample;

/* Bounds over a block of a level's storage: the TERRAIN_CLIP_BLOCK^2 slots
 * from (bx, bz) * BLOCK. Storage blocks are world-aligned: world sample (i, j)
 * lies in world block (i, j) / BLOCK (rounding down), stored in block
 * (that mod TERRAIN_CLIP_NB), so a block holds one world block's samples,
 * except the block at the window's seam, which holds a piece of each end of
 * the window and bounds both. Only the window's samples count. lo and hi
 * bound the drawn height whatever its morph (h and hc); err is the largest
 * |h - hc|: how far the surface moves there if the level is dropped for its
 * parent. Both surfaces are linear over each of the level's triangles (each
 * lies in one of the parent's), so between samples the move is no larger. */
#define TERRAIN_CLIP_BLOCK 8
#define TERRAIN_CLIP_NB    (TERRAIN_CLIP_N / TERRAIN_CLIP_BLOCK)
typedef struct TerrainBlock { float lo, hi, err; } TerrainBlock;

/* A rectangle of storage slots, [x0, x1) x [z0, z1); never wraps. */
typedef struct TerrainRect { int x0, z0, x1, z1; } TerrainRect;

/* A rectangle of world sample indices, [i0, i0 + w) x [j0, j0 + d). */
typedef struct TerrainSpan { int64_t i0, j0; int w, d; } TerrainSpan;

typedef struct TerrainClipLevel {
    double         spacing;    /* metres between samples */
    int64_t        origin[2];  /* world sample index (i, j) of the window's corner, when filled */
    int            ready;      /* filled, and the window is where the camera wants it: drawable */
    TerrainSample *samples;    /* N * N slots; see terrain_clip_sample */
    /* Slots written by the last update, for the renderer to upload. */
    int            n_dirty;
    TerrainRect    dirty[TERRAIN_CLIP_MAX_DIRTY];
    /* Bounds of each storage block, exact for the window once filled (they
     * then depend only on the window, never on the camera's path). */
    TerrainBlock   blocks[TERRAIN_CLIP_NB * TERRAIN_CLIP_NB];
    /* terrain_lod.c's bookkeeping. */
    int            filled;     /* every slot of the window at origin holds its sample */
    int64_t        target[2];  /* origin being moved to while spans are pending */
    int            n_pending;
    TerrainSpan    pending[TERRAIN_CLIP_MAX_SPANS];
} TerrainClipLevel;

typedef struct TerrainClipmap {
    TerrainClipLevel level[TERRAIN_CLIP_LEVELS];
    int              finest;   /* levels finest .. LEVELS - 1 are ready; LEVELS if none is */
    int              want;     /* the finest level worth drawing (terrain_clip_select) */
    double           cam[2];   /* camera (x, z) at the last update (m) */
    TerrainSample   *storage;  /* every level's samples, one allocation */
    double          *scratch_h;
    float           *scratch_p;
} TerrainClipmap;

/* Allocates the levels, all empty; free with terrain_clip_free. */
void   terrain_clip_init(TerrainClipmap *c);
void   terrain_clip_free(TerrainClipmap *c);
/* Moves the levels to follow the camera, evaluating at most `budget` heights
 * (whole rows; an update that can't afford a row fills one anyway, so work
 * always progresses). Levels are filled coarsest first: a finer level is only
 * worth drawing inside a ready coarser one. Levels more than one finer than
 * c->want (terrain_clip_select) are left as they are, filled or part filled:
 * nothing draws them, and a fast camera high up would spend most of each
 * update on them. The one just finer than c->want is kept, so the selection
 * can refine at once; finer still follow a level at a time as it asks.
 * Returns the heights evaluated. Clears and refills each level's dirty list. */
int    terrain_clip_update(TerrainClipmap *c, double cam_x, double cam_z, int budget);
/* An upper bound on the heights terrain_mesh_fill evaluates for a span of
 * w x d samples of a level: the span and its apron, and below the coarsest
 * level the parent samples under it. One row of the window costs at most
 * terrain_clip_fill_cost(0, TERRAIN_CLIP_W, 1). */
static inline int terrain_clip_fill_cost(int level, int w, int d) {
    int cost = (w + 2) * (d + 2);
    if (level < TERRAIN_CLIP_LEVELS - 1) cost += ((w + 1) / 2 + 1) * ((d + 1) / 2 + 1);
    return cost;
}
/* Blend weight of level l into its parent at world (x, z), for the camera of
 * the last update: 0 over most of the window, rising to exactly 1 at and
 * beyond its edge, and 0 wherever its child level is drawn. The drawn height
 * is h + morph * (hc - h). The renderer's shader must compute the same. */
float  terrain_clip_morph(const TerrainClipmap *c, int level, double x, double z);
/* Bounds on level l's drawn height (any morph) over world samples
 * [i0, i1] x [j0, j1] of its window, from its blocks: conservative, and
 * exact to the block. */
void   terrain_clip_bounds(const TerrainClipLevel *lv, int64_t i0, int64_t j0, int64_t i1, int64_t j1, float *lo, float *hi);

/* --- Screen-space error --------------------------------------------------------
 * Which levels are worth drawing. Level m's error is the most, in pixels, its
 * surface would move on screen were it dropped for its parent's:
 *   rho(m) = max over its blocks of err * K / distance(camera, block),
 * K the viewport's pixels per unit of tangent, height / (2 tan(fovy / 2)),
 * and the distance at least TERRAIN_SSE_NEAR. Drawing level k + 1 in place of
 * levels 0 .. k moves the surface by at most rho(0) + ... + rho(k) pixels:
 * that sum decides the finest level drawn, so flat ground (the city's) and
 * high cameras drop the fine levels and rough ground nearby keeps them.
 *
 * Hysteresis: the finest level refines once the sum for the level below it
 * exceeds TERRAIN_SSE_REFINE pixels, and coarsens only once its own sum is
 * under TERRAIN_SSE_COARSEN, so a camera hovering at a threshold doesn't flip
 * levels back and forth. Either switch moves the surface by at most its
 * threshold, a pixel or less, which is the transition between whole levels;
 * the geomorph still joins the levels drawn.
 *
 * The update keeps the levels down to one finer than the selection filled
 * (terrain_clip_update), so refining a level is ready at once; finer levels
 * keep the window they last had, and their errors are from it until the
 * selection reaches them and they catch up. */
#define TERRAIN_SSE_REFINE  1.0
#define TERRAIN_SSE_COARSEN 0.5
#define TERRAIN_SSE_NEAR    1.0      /* nearer than this (m) counts as this */
/* rho(level) for a camera at (c->cam, cam_y). */
double terrain_clip_error(const TerrainClipmap *c, int level, double cam_y, double K);
/* Moves c->want by the rules above and returns the finest level to draw:
 * c->want, or the finest ready level if that is coarser. */
int    terrain_clip_select(TerrainClipmap *c, double cam_y, double K);

/* --- Level transitions ---------------------------------------------------------
 * When the selection's level changes, the drawn surface doesn't jump to it:
 * levels are added or dropped one at a time, each blended over
 * TERRAIN_FADE_TIME seconds (shortened by how many levels behind the
 * selection the drawing is, so a fast descent still catches up). A level
 * fading in is drawn with its morph at least 1 - progress, so it starts as its
 * parent's surface exactly; one fading out with its morph at least progress,
 * ending as its parent's, and is then dropped. Only the finest level drawn
 * ever fades, so the rings outside it meet it as before. If the levels drawn
 * stop being ready (a jump), the drawing goes straight to the selection. */
#define TERRAIN_FADE_TIME 0.3
typedef struct TerrainFade {
    int    shown;      /* the finest level drawn; TERRAIN_CLIP_LEVELS before the first step */
    int    level;      /* the level fading in or out (always shown), or -1 */
    int    in;         /* fading in (1) or out (0) */
    double progress;   /* 0 .. 1 */
} TerrainFade;
void  terrain_fade_init(TerrainFade *f);
/* Advances dt seconds towards drawing `target` (terrain_clip_select's level),
 * never drawing a level finer than c->finest. */
void  terrain_fade_step(TerrainFade *f, const TerrainClipmap *c, int target, double dt);
/* The least morph level l is drawn with: 0 unless it is fading. */
float terrain_fade_morph(const TerrainFade *f, int level);

static inline double terrain_clip_spacing(int level) { return ldexp(TERRAIN_CLIP_SPACING0, level); }
static inline int    terrain_clip_slot(int64_t i) { return (int)(((i % TERRAIN_CLIP_N) + TERRAIN_CLIP_N) % TERRAIN_CLIP_N); }
/* The slot holding world sample (i, j) of a level (valid inside a filled window). */
static inline TerrainSample *terrain_clip_sample(const TerrainClipLevel *lv, int64_t i, int64_t j) {
    return &lv->samples[terrain_clip_slot(j) * TERRAIN_CLIP_N + terrain_clip_slot(i)];
}
