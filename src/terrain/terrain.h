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
 * [z0,z1], as terrain_height_range. */
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
/* Samples filled per update when the camera has jumped: the whole clipmap is
 * about 9 levels' worth, so a jump takes about 9 updates to fill in. */
#define TERRAIN_CLIP_BUDGET   (TERRAIN_CLIP_W * TERRAIN_CLIP_W)
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
/* Moves the levels to follow the camera, filling at most `budget` samples
 * (whole rows; a budget under one row is raised to a row so work always
 * progresses). Levels are filled coarsest first: a finer level is only worth
 * drawing inside a ready coarser one. Returns the samples filled. Clears and
 * refills each level's dirty list. */
int    terrain_clip_update(TerrainClipmap *c, double cam_x, double cam_z, int budget);
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
 * The update still fills every level, drawn or not (cheap while the camera
 * moves smoothly), so a level asked for again is ready at once and the
 * selection's errors are its own. */
#define TERRAIN_SSE_REFINE  1.0
#define TERRAIN_SSE_COARSEN 0.5
#define TERRAIN_SSE_NEAR    1.0      /* nearer than this (m) counts as this */
/* rho(level) for a camera at (c->cam, cam_y). */
double terrain_clip_error(const TerrainClipmap *c, int level, double cam_y, double K);
/* Moves c->want by the rules above and returns the finest level to draw:
 * c->want, or the finest ready level if that is coarser. */
int    terrain_clip_select(TerrainClipmap *c, double cam_y, double K);

static inline double terrain_clip_spacing(int level) { return ldexp(TERRAIN_CLIP_SPACING0, level); }
static inline int    terrain_clip_slot(int64_t i) { return (int)(((i % TERRAIN_CLIP_N) + TERRAIN_CLIP_N) % TERRAIN_CLIP_N); }
/* The slot holding world sample (i, j) of a level (valid inside a filled window). */
static inline TerrainSample *terrain_clip_sample(const TerrainClipLevel *lv, int64_t i, int64_t j) {
    return &lv->samples[terrain_clip_slot(j) * TERRAIN_CLIP_N + terrain_clip_slot(i)];
}
