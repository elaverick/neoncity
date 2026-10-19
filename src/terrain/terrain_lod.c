#include "terrain_internal.h"
#include "mathlib.h"
#include <float.h>
#include <stdlib.h>
#include <string.h>

/* A level's update: when its wanted origin differs from the one its storage
 * holds, the samples of the new window that the old one lacked are listed as
 * pending spans, and filled, a band of rows at a time, as the budget allows.
 * Until the last span is filled the level is not ready (its slots are part
 * old window, part new) and the renderer draws the coarser levels in its
 * place. A move then completes even if the camera moves on meanwhile; the
 * next update moves on from where it completed. So the slots outside the
 * spans always still hold the old window's samples, and nothing depends on
 * the camera's path: a slot only ever receives its world sample. */

/* The camera lies in samples [2c, 2c + 2) of a level (c the parent sample
 * below it); the window is centred on sample 2c + 1, so the camera is within
 * one sample of its centre. (W - 1) / 2 is odd, so the origin is even. */
static int64_t want_origin(double cam, double spacing) {
    int64_t c2 = 2 * (int64_t)floor(cam / (2.0 * spacing));
    return c2 + 1 - (TERRAIN_CLIP_W - 1) / 2;
}

/* Adds a span to the pending list, split where it wraps in storage so each
 * piece fills one storage rectangle. A span is at most W < N samples on a
 * side, so it wraps at most once each way: one move's two spans make at most
 * eight pieces. */
static void push_span(TerrainClipLevel *lv, TerrainSpan sp) {
    if (sp.w <= 0 || sp.d <= 0) return;
    int sx = terrain_clip_slot(sp.i0), sz = terrain_clip_slot(sp.j0);
    if (sx + sp.w > TERRAIN_CLIP_N) {
        TerrainSpan a = sp, b = sp;
        a.w = TERRAIN_CLIP_N - sx;
        b.i0 += a.w;
        b.w -= a.w;
        push_span(lv, a);
        push_span(lv, b);
        return;
    }
    if (sz + sp.d > TERRAIN_CLIP_N) {
        TerrainSpan a = sp, b = sp;
        a.d = TERRAIN_CLIP_N - sz;
        b.j0 += a.d;
        b.d -= a.d;
        push_span(lv, a);
        push_span(lv, b);
        return;
    }
    if (lv->n_pending >= TERRAIN_CLIP_MAX_SPANS) fatal("terrain clipmap: too many spans in one move");
    lv->pending[lv->n_pending++] = sp;
}

/* Plans a level's move to `want`. The samples the old window lacks are an L:
 * the exposed columns at full height, then the exposed rows across the other
 * columns. If nothing of the old window survives (or there is none yet), the
 * whole window. */
static void plan_move(TerrainClipLevel *lv, const int64_t want[2]) {
    const int W = TERRAIN_CLIP_W;
    int64_t dx = want[0] - lv->origin[0], dz = want[1] - lv->origin[1];
    lv->n_pending = 0;
    if (!lv->filled || llabs(dx) >= W || llabs(dz) >= W) {
        push_span(lv, (TerrainSpan){ want[0], want[1], W, W });
    } else {
        int ax = (int)llabs(dx), az = (int)llabs(dz);
        push_span(lv, (TerrainSpan){ dx > 0 ? want[0] + W - ax : want[0], want[1], ax, W });
        push_span(lv, (TerrainSpan){ dx > 0 ? want[0] : want[0] + ax, dz > 0 ? want[1] + W - az : want[1], W - ax, az });
    }
    lv->target[0] = want[0];
    lv->target[1] = want[1];
    lv->filled = 0;
}

/* World block of sample i (rounding down), and the storage block holding it. */
static int64_t block_of(int64_t i) { return (i >= 0 ? i : i - (TERRAIN_CLIP_BLOCK - 1)) / TERRAIN_CLIP_BLOCK; }
static int     block_slot(int64_t b) { return (int)(((b % TERRAIN_CLIP_NB) + TERRAIN_CLIP_NB) % TERRAIN_CLIP_NB); }

/* Recomputes storage block (bx, bz) from the slots holding the window at o:
 * every slot but the one row and column the window leaves free. */
static void refresh_block(TerrainClipLevel *lv, const int64_t o[2], int bx, int bz) {
    const int B = TERRAIN_CLIP_BLOCK;
    int fx = terrain_clip_slot(o[0] - 1), fz = terrain_clip_slot(o[1] - 1);
    TerrainBlock b = { FLT_MAX, -FLT_MAX, 0.0f };
    for (int z = bz * B; z < bz * B + B; ++z) {
        if (z == fz) continue;
        for (int x = bx * B; x < bx * B + B; ++x) {
            if (x == fx) continue;
            const TerrainSample *t = &lv->samples[z * TERRAIN_CLIP_N + x];
            b.lo = fminf(b.lo, fminf(t->h, t->hc));
            b.hi = fmaxf(b.hi, fmaxf(t->h, t->hc));
            b.err = fmaxf(b.err, fabsf(t->h - t->hc));
        }
    }
    lv->blocks[bz * TERRAIN_CLIP_NB + bx] = b;
}

/* After a band is filled: the blocks it wrote into, for the window being moved to. */
static void refresh_band(TerrainClipLevel *lv, TerrainSpan sp) {
    for (int64_t bj = block_of(sp.j0); bj <= block_of(sp.j0 + sp.d - 1); ++bj)
        for (int64_t bi = block_of(sp.i0); bi <= block_of(sp.i0 + sp.w - 1); ++bi)
            refresh_block(lv, lv->target, block_slot(bi), block_slot(bj));
}

/* After a move completes: the blocks holding the free row and column, whose
 * slots may hold samples that have just left the window. Every other slot
 * that joined or left the window was refilled, and its block refreshed after
 * its last write, so each block is now exactly the window's, whatever path
 * the camera took. */
static void refresh_free(TerrainClipLevel *lv) {
    int fx = terrain_clip_slot(lv->origin[0] - 1) / TERRAIN_CLIP_BLOCK, fz = terrain_clip_slot(lv->origin[1] - 1) / TERRAIN_CLIP_BLOCK;
    for (int k = 0; k < TERRAIN_CLIP_NB; ++k) {
        refresh_block(lv, lv->origin, fx, k);
        refresh_block(lv, lv->origin, k, fz);
    }
}

static void add_dirty(TerrainClipLevel *lv, TerrainSpan sp) {
    if (lv->n_dirty >= TERRAIN_CLIP_MAX_DIRTY) fatal("terrain clipmap: too many dirty rectangles in one update");
    int x0 = terrain_clip_slot(sp.i0), z0 = terrain_clip_slot(sp.j0);
    lv->dirty[lv->n_dirty++] = (TerrainRect){ x0, z0, x0 + sp.w, z0 + sp.d };
}

void terrain_clip_init(TerrainClipmap *c) {
    memset(c, 0, sizeof(*c));
    c->storage = (TerrainSample *)calloc((size_t)TERRAIN_CLIP_LEVELS * TERRAIN_CLIP_N * TERRAIN_CLIP_N, sizeof(TerrainSample));
    c->scratch_h = (double *)malloc(sizeof(double) * TERRAIN_FILL_SIDE * TERRAIN_FILL_SIDE);
    c->scratch_p = (float *)malloc(sizeof(float) * TERRAIN_PARENT_SIDE * TERRAIN_PARENT_SIDE);
    if (!c->storage || !c->scratch_h || !c->scratch_p) fatal("out of memory allocating the terrain clipmap");
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        c->level[l].spacing = terrain_clip_spacing(l);
        c->level[l].samples = c->storage + (size_t)l * TERRAIN_CLIP_N * TERRAIN_CLIP_N;
    }
    c->finest = TERRAIN_CLIP_LEVELS;
}

void terrain_clip_free(TerrainClipmap *c) {
    free(c->storage);
    free(c->scratch_h);
    free(c->scratch_p);
    memset(c, 0, sizeof(*c));
}

/* Coarsest level first. Once a level can't finish within the budget, finer
 * levels get no work this update: they couldn't be drawn inside it anyway.
 * Levels finer than c->want - 1 get none either (see terrain.h). */
int terrain_clip_update(TerrainClipmap *c, double cam_x, double cam_z, int budget) {
    int spent = 0, stalled = 0, lazy = c->want - 1;
    c->cam[0] = cam_x;
    c->cam[1] = cam_z;
    c->finest = TERRAIN_CLIP_LEVELS;
    for (int l = TERRAIN_CLIP_LEVELS - 1; l >= 0; --l) {
        TerrainClipLevel *lv = &c->level[l];
        int64_t want[2] = { want_origin(cam_x, lv->spacing), want_origin(cam_z, lv->spacing) };
        lv->n_dirty = 0;
        while (!stalled && l >= lazy) {
            if (lv->n_pending == 0) {
                if (lv->filled && lv->origin[0] == want[0] && lv->origin[1] == want[1]) break;
                plan_move(lv, want);
            }
            TerrainSpan *sp = &lv->pending[lv->n_pending - 1];
            int rows = sp->d;
            while (rows > 0 && spent + terrain_clip_fill_cost(l, sp->w, rows) > budget) rows--;
            if (rows == 0 && spent == 0) rows = 1;   /* always progress */
            if (rows == 0) { stalled = 1; break; }
            TerrainSpan band = *sp;
            band.d = rows;
            spent += terrain_mesh_fill(c, l, band);
            add_dirty(lv, band);
            refresh_band(lv, band);
            sp->j0 += rows;
            sp->d -= rows;
            if (sp->d == 0 && --lv->n_pending == 0) {
                lv->origin[0] = lv->target[0];
                lv->origin[1] = lv->target[1];
                lv->filled = 1;
                refresh_free(lv);
            }
        }
        lv->ready = lv->filled && lv->origin[0] == want[0] && lv->origin[1] == want[1];
        if (lv->ready && c->finest == l + 1) c->finest = l;
    }
    return spent;
}

/* The camera is within one sample of the window's centre, so every edge
 * sample is at least (W - 1) / 2 - 1 samples from it (Chebyshev distance):
 * the blend ends there. A child window reaches at most (W - 1) / 2 + 1 of its
 * own samples, about a quarter of the parent's width, from the camera, well
 * inside where the parent's blend starts. Spacings are powers of two, so at
 * sample positions the distance is exact and the edge gets exactly 1. */
float terrain_clip_morph(const TerrainClipmap *c, int level, double x, double z) {
    double s = c->level[level].spacing;
    double d = fmax(fabs(x - c->cam[0]), fabs(z - c->cam[1])) / s;
    double end = (TERRAIN_CLIP_W - 1) / 2 - 1;
    return clampf((float)((d - (end - TERRAIN_CLIP_MORPH)) / TERRAIN_CLIP_MORPH), 0.0f, 1.0f);
}

void terrain_clip_bounds(const TerrainClipLevel *lv, int64_t i0, int64_t j0, int64_t i1, int64_t j1, float *lo, float *hi) {
    *lo = FLT_MAX;
    *hi = -FLT_MAX;
    for (int64_t bj = block_of(j0); bj <= block_of(j1); ++bj)
        for (int64_t bi = block_of(i0); bi <= block_of(i1); ++bi) {
            const TerrainBlock *b = &lv->blocks[block_slot(bj) * TERRAIN_CLIP_NB + block_slot(bi)];
            *lo = fminf(*lo, b->lo);
            *hi = fmaxf(*hi, b->hi);
        }
}

/* Each world block of the window, as a box: its samples clipped to the
 * window, and the next block's first (the triangles between the two), with
 * its storage block's heights. */
double terrain_clip_error(const TerrainClipmap *c, int level, double cam_y, double K) {
    const TerrainClipLevel *lv = &c->level[level];
    const int B = TERRAIN_CLIP_BLOCK;
    double s = lv->spacing, worst = 0.0;
    int64_t e0 = lv->origin[0] + TERRAIN_CLIP_W - 1, e1 = lv->origin[1] + TERRAIN_CLIP_W - 1;
    for (int64_t bj = block_of(lv->origin[1]); bj <= block_of(e1); ++bj) {
        double z0 = (double)(bj * B > lv->origin[1] ? bj * B : lv->origin[1]) * s, z1 = (double)(bj * B + B < e1 ? bj * B + B : e1) * s;
        double dz = fmax(0.0, fmax(z0 - c->cam[1], c->cam[1] - z1));
        for (int64_t bi = block_of(lv->origin[0]); bi <= block_of(e0); ++bi) {
            const TerrainBlock *b = &lv->blocks[block_slot(bj) * TERRAIN_CLIP_NB + block_slot(bi)];
            if (b->err <= 0.0f) continue;
            double x0 = (double)(bi * B > lv->origin[0] ? bi * B : lv->origin[0]) * s, x1 = (double)(bi * B + B < e0 ? bi * B + B : e0) * s;
            double dx = fmax(0.0, fmax(x0 - c->cam[0], c->cam[0] - x1));
            double dy = fmax(0.0, fmax(b->lo - cam_y, cam_y - b->hi));
            double rho = b->err * K / fmax(sqrt(dx * dx + dy * dy + dz * dz), TERRAIN_SSE_NEAR);
            if (rho > worst) worst = rho;
        }
    }
    return worst;
}

/* The sums only grow with the level, so after refining the coarsening test
 * fails (its sum is over the refining threshold). */
int terrain_clip_select(TerrainClipmap *c, double cam_y, double K) {
    double sum[TERRAIN_CLIP_LEVELS], acc = 0.0;
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) sum[l] = acc += terrain_clip_error(c, l, cam_y, K);
    while (c->want > 0 && sum[c->want - 1] > TERRAIN_SSE_REFINE) c->want--;
    while (c->want < TERRAIN_CLIP_LEVELS - 1 && sum[c->want] < TERRAIN_SSE_COARSEN) c->want++;
    return c->want > c->finest ? c->want : c->finest;
}
