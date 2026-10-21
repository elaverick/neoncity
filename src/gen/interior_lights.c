#include "interior_internal.h"

/* ===========================================================================
 * Ceiling lights: IL_TILE square fittings (shaders/city/lobby.glsl:
 * ceilingFixture) set out on grids of IL_PITCH in the building frame, the
 * frame the ceilings' uv follow. The open floor (lobby, vestibule, halls and
 * secure sides) shares one grid, centred on the lobby; the floors carry its
 * offset so polished stone mirrors the fittings where they are
 * (ceilingPanel). Each enclosed room has its own grid, centred in it.
 *
 * A fitting is kept only where its whole tile, and a border of IL_TILE all
 * round it, lies over usable floor that is not cut off from the tile by a
 * wall, glass, a solid part or the envelope (open edges and barriers between
 * the lobby and its halls don't count), nor, beneath a gallery or a podium's
 * open ceiling, by the atrium's void. No column stands under a tile.
 * Neighbours on a grid are IL_PITCH - IL_TILE apart; fittings on different
 * grids have a wall and both borders between them. A room too small for its
 * grid to keep any gets one fitting as near its middle as fits, with a border of
 * IL_SMALL_BORDER (give or take a quarter metre, the planning grid's
 * resolution), if that fits. A layout whose open floor keeps none is
 * rejected (interior_lobby_lit, in validate).
 * ========================================================================= */

/* The offset along one axis of a grid centred on [a, b): tile centres at
 * o + k * IL_PITCH, as many as fit inside their borders, with o in steps of
 * IL_PITCH / 256 (what a vertex carries) in [0, IL_PITCH). */
static uint8_t centred(float a, float b) {
    float span = b - a - 2.0f * IL_TILE;
    int n = span >= IL_TILE ? 1 + (int)floorf((span - IL_TILE) / IL_PITCH) : 1;
    float first = 0.5f * (a + b) - 0.5f * (float)(n - 1) * IL_PITCH;
    float o = fmodf(first, IL_PITCH);
    if (o < 0.0f) o += IL_PITCH;
    return (uint8_t)((int)floorf(o / IL_PITCH * 256.0f + 0.5f) & 255);
}

/* Region r's bounding box in the building frame. */
static void region_box(const ILayout *L, int r, float *x0, float *z0, float *x1, float *z1) {
    const IRegion *g = &L->reg[r];
    float ax, az, bx, bz;
    interior_to_local(L, L->gx0 + g->x0 * CELL, L->gz0 + g->z0 * CELL, &ax, &az);
    interior_to_local(L, L->gx0 + g->x1 * CELL, L->gz0 + g->z1 * CELL, &bx, &bz);
    *x0 = minf(ax, bx); *x1 = maxf(ax, bx); *z0 = minf(az, bz); *z1 = maxf(az, bz);
}

void interior_light_grid(const ILayout *L, uint8_t q[2]) {
    float x0, z0, x1, z1;
    if (L->nreg > 0 && L->reg[0].cells) region_box(L, 0, &x0, &z0, &x1, &z1);
    else {
        float ax, az, bx, bz;
        interior_to_local(L, L->gx0, L->gz0, &ax, &az);
        interior_to_local(L, L->gx0 + L->nx * CELL, L->gz0 + L->nz * CELL, &bx, &bz);
        x0 = minf(ax, bx); x1 = maxf(ax, bx); z0 = minf(az, bz); z1 = maxf(az, bz);
    }
    q[0] = centred(x0, x1);
    q[1] = centred(z0, z1);
}

/* The cells under building-frame rectangle [x0, x1) x [z0, z1): those it
 * overlaps at all, or (centres) only those whose centres it holds. */
static void cells_under(const ILayout *L, float x0, float z0, float x1, float z1, int centres, int *i0, int *j0, int *i1, int *j1) {
    float ax, az, bx, bz, e = centres ? 0.5f : 1e-3f;
    interior_to_frame(L, x0, z0, &ax, &az);
    interior_to_frame(L, x1, z1, &bx, &bz);
    if (centres) {
        *i0 = (int)ceilf((minf(ax, bx) - L->gx0) / CELL - e); *i1 = (int)floorf((maxf(ax, bx) - L->gx0) / CELL - e) + 1;
        *j0 = (int)ceilf((minf(az, bz) - L->gz0) / CELL - e); *j1 = (int)floorf((maxf(az, bz) - L->gz0) / CELL - e) + 1;
    } else {
        *i0 = (int)floorf((minf(ax, bx) - L->gx0) / CELL + e); *i1 = (int)ceilf((maxf(ax, bx) - L->gx0) / CELL - e);
        *j0 = (int)floorf((minf(az, bz) - L->gz0) / CELL + e); *j1 = (int)ceilf((maxf(az, bz) - L->gz0) / CELL - e);
    }
}

/* The region of the tile whose corner is (x0, z0), if a fitting may hang
 * there with `border` clear round it (-1 if not). The tile may not overlap
 * an edge at all; the border counts whole cells (every cell it touches), or
 * if `loose` the cells whose centres it holds, so it may fall short by up to
 * half a cell. clearVoid: the void counts as an edge. */
static int fits(const ILayout *L, float x0, float z0, float border, int loose, int clearVoid) {
    int ti0, tj0, ti1, tj1, bi0, bj0, bi1, bj1;
    cells_under(L, x0, z0, x0 + IL_TILE, z0 + IL_TILE, 0, &ti0, &tj0, &ti1, &tj1);
    cells_under(L, x0 - border, z0 - border, x0 + IL_TILE + border, z0 + IL_TILE + border, loose, &bi0, &bj0, &bi1, &bj1);
    if (bi0 > ti0) bi0 = ti0;
    if (bj0 > tj0) bj0 = tj0;
    if (bi1 < ti1) bi1 = ti1;
    if (bj1 < tj1) bj1 = tj1;
    int r0 = region_at(L, (ti0 + ti1) / 2, (tj0 + tj1) / 2);
    if (r0 < 0 || L->reg[r0].kind == IK_SOLID) return -1;
    for (int j = bj0; j < bj1; ++j)
        for (int i = bi0; i < bi1; ++i) {
            int r = region_at(L, i, j);
            if (r < 0) return -1;
            int b = interior_boundary(L, r0, r);
            if (b != IB_NONE && b != IB_OPEN && b != IB_BARRIER) return -1;
            uint16_t f = L->flag[IDX(L, i, j)];
            if (clearVoid && (f & IF_VOID)) return -1;
            if ((f & IF_COLUMN) && i >= ti0 && i < ti1 && j >= tj0 && j < tj1) return -1;
        }
    return r0;
}

/* Lays one grid (offset q) over [x0, x1) x [z0, z1): room r's, or the open floor's if r < 0. */
static int lay_grid(const ILayout *L, int r, const uint8_t q[2], float x0, float z0, float x1, float z1,
                    int level, int clearVoid, ILight *out, int n, int max) {
    float ox = q[0] * (IL_PITCH / 256.0f), oz = q[1] * (IL_PITCH / 256.0f), h = 0.5f * IL_TILE;
    for (float cz = oz + IL_PITCH * floorf((z0 - oz) / IL_PITCH); cz - h < z1; cz += IL_PITCH)
        for (float cx = ox + IL_PITCH * floorf((x0 - ox) / IL_PITCH); cx - h < x1 && n < max; cx += IL_PITCH) {
            if (cx - h < x0 || cz - h < z0 || cx + h > x1 || cz + h > z1) continue;
            int t = fits(L, cx - h, cz - h, IL_TILE, 0, clearVoid);
            if (t < 0 || (r >= 0 ? t != r : L->reg[t].kind != IK_OPEN)) continue;
            out[n].level = (uint8_t)level;
            out[n].region = (int16_t)r;
            out[n].x0 = cx - h; out[n].z0 = cz - h; out[n].x1 = cx + h; out[n].z1 = cz + h;
            n++;
        }
    return n;
}

/* The ceilings' levels: level 0 the ceiling (open over a podium's void), 1 a
 * gallery's underside, round the void. Returns how many; clearVoid[level]:
 * whether the void is an edge there. */
static int levels(const ILayout *L, int clearVoid[2]) {
    const IAtrium *A = &L->atrium;
    int hasVoid = A->x1 > A->x0 && (A->mode == IA_GALLERY || A->mode == IA_PODIUM);
    clearVoid[0] = hasVoid && A->mode == IA_PODIUM;
    clearVoid[1] = hasVoid;
    return hasVoid && A->mode == IA_GALLERY ? 2 : 1;
}

/* The open floor's grid, over the whole planning grid. */
static int lay_open(const ILayout *L, int level, int clearVoid, ILight *out, int n, int max) {
    float ax, az, bx, bz;
    uint8_t open[2];
    interior_to_local(L, L->gx0, L->gz0, &ax, &az);
    interior_to_local(L, L->gx0 + L->nx * CELL, L->gz0 + L->nz * CELL, &bx, &bz);
    interior_light_grid(L, open);
    return lay_grid(L, -1, open, minf(ax, bx), minf(az, bz), maxf(ax, bx), maxf(az, bz), level, clearVoid, out, n, max);
}

int interior_lights(const ILayout *L, ILight *out, int max) {
    int clearVoid[2], nlev = levels(L, clearVoid), n = 0;
    for (int level = 0; level < nlev; ++level) {
        n = lay_open(L, level, clearVoid[level], out, n, max);
        for (int r = 0; r < L->nreg; ++r) {
            if (L->reg[r].kind != IK_ROOM || !L->reg[r].cells) continue;
            float x0, z0, x1, z1;
            uint8_t q[2];
            region_box(L, r, &x0, &z0, &x1, &z1);
            q[0] = centred(x0, x1);
            q[1] = centred(z0, z1);
            int n0 = n;
            n = lay_grid(L, r, q, x0, z0, x1, z1, level, clearVoid[level], out, n, max);
            /* too small for its grid: one fitting with a narrower border, as near the middle as fits
             * (an L-shaped room's middle may be outside it), on quarter-metre steps from the middle */
            if (n == n0 && n < max) {
                float mx = 0.5f * (x0 + x1 - IL_TILE), mz = 0.5f * (z0 + z1 - IL_TILE), best = 1e9f, bx = 0.0f, bz = 0.0f;
                int kx = (int)((x1 - x0 - IL_TILE) / 0.5f), kz = (int)((z1 - z0 - IL_TILE) / 0.5f);
                for (int j = -kz; j <= kz; ++j)
                    for (int i = -kx; i <= kx; ++i) {
                        float cx = mx + 0.25f * (float)i, cz = mz + 0.25f * (float)j;
                        float d = (cx - mx) * (cx - mx) + (cz - mz) * (cz - mz);
                        if (d < best && fits(L, cx, cz, IL_SMALL_BORDER, 1, clearVoid[level]) == r) { best = d; bx = cx; bz = cz; }
                    }
                if (best < 1e9f) {
                    out[n].level = (uint8_t)level;
                    out[n].region = (int16_t)r;
                    out[n].x0 = bx; out[n].z0 = bz; out[n].x1 = bx + IL_TILE; out[n].z1 = bz + IL_TILE;
                    n++;
                }
            }
        }
    }
    return n;
}

int interior_lobby_lit(const ILayout *L) {
    ILight one;
    int clearVoid[2], nlev = levels(L, clearVoid);
    for (int level = 0; level < nlev; ++level)
        if (lay_open(L, level, clearVoid[level], &one, 0, 1)) return 1;
    return 0;
}
