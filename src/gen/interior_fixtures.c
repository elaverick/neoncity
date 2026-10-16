#include "interior_internal.h"

/* ===========================================================================
 * Step 6: doors, then fixtures
 * ========================================================================= */

int place_room_doors(Gen *G) {
    ILayout *L = G->L;
    for (int r = 0; r < L->nreg; ++r) {
        if (L->reg[r].kind != IK_ROOM) continue;
        int t = L->reg[r].type;
        Run run = best_run(L, r, -2);
        int w = t == IR_PLANT ? 3 : t == IR_LOADING ? 4 : 2;
        int anchored = 0;
        for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && L->door[d].kind == ID_SERVICE) anchored = 1;
        if (run.len < 2) {
            if (anchored) { alog(G, "  simplify: %s reached only through its service door\n", REG_NAMES[t]); continue; }
            return fail(G, "the %s has no possible door", REG_NAMES[t]);
        }
        if (run.len < w) w = run.len;
        int slack = run.len - w, at = run.at + slack / 2;
        if (slack >= 4) at += rng_int(&G->rd, -(slack / 2 - 1), slack / 2 - 1);
        int d = add_door(G, ID_ROOM, run.axis, run.line, at, w, r, run.other);
        if (d >= 0) mark_door_clear(G, &L->door[d], 3);
        if (t == IR_ESCAPE) {
            Run fire = best_run(L, r, -1);
            if (fire.len >= 2) add_door(G, ID_FIRE, fire.axis, fire.line, fire.at + (fire.len - 2) / 2, 2, r, -1);
            else alog(G, "  simplify: escape stair has no facade for its fire exit\n");
        }
    }
    return 1;
}

int fix_cells(const ILayout *L, float x0, float z0, float x1, float z1, int *i0, int *j0, int *i1, int *j1) {
    *i0 = (int)floorf((x0 - L->gx0) / CELL + 1e-3f);
    *j0 = (int)floorf((z0 - L->gz0) / CELL + 1e-3f);
    *i1 = (int)ceilf((x1 - L->gx0) / CELL - 1e-3f);
    *j1 = (int)ceilf((z1 - L->gz0) / CELL - 1e-3f);
    return *i0 >= 0 && *j0 >= 0 && *i1 <= L->nx && *j1 <= L->nz && *i1 > *i0 && *j1 > *j0;
}

enum { FIT_SOLID = 0, FIT_CLEAR, FIT_VOID };

/* Does a footprint fit in region r? FIT_SOLID: furniture; FIT_CLEAR: a clear
 * zone (may lie on the route reserve); FIT_VOID: a feature allowed in the void. */
static int fits(const ILayout *L, int r, float x0, float z0, float x1, float z1, int mode) {
    int i0, j0, i1, j1;
    if (!fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1)) return 0;
    uint16_t bad = IF_COLUMN | IF_FIXTURE | IF_EDGE;
    if (mode != FIT_CLEAR) bad |= IF_RESERVE | IF_DOORCLEAR | IF_APPROACH | IF_ENTRY | IF_ACCESS;
    if (mode == FIT_SOLID && r == 0) bad |= IF_VOID;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            int c = IDX(L, i, j);
            if (L->occ[c] != IO_USABLE || L->own[c] != r || (L->flag[c] & bad)) return 0;
        }
    return 1;
}

static IFixture *add_fix(Gen *G, int type, int facing, int region, float x0, float z0, float x1, float z1, float y0, float y1, int mark) {
    ILayout *L = G->L;
    if (L->nfix >= IN_MAX_FIX) return NULL;
    IFixture *f = &L->fix[L->nfix++];
    f->type = (uint8_t)type; f->facing = (uint8_t)facing; f->group = IP_NONE; f->member = 0; f->region = (int16_t)region;
    f->x0 = x0; f->z0 = z0; f->x1 = x1; f->z1 = z1; f->y0 = y0; f->y1 = y1;
    if (mark) {
        int i0, j0, i1, j1;
        if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
            for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_FIXTURE;
    }
    return f;
}

static void mark_rect(ILayout *L, float x0, float z0, float x1, float z1) {
    int i0, j0, i1, j1;
    if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_FIXTURE;
}

/* ---------------------------------------------------------------------------
 * Access: a fixture group may not cut anything off. Each group declares use
 * zones (the visitor side of the reception desk, the staff side behind it,
 * the edge of a bay, the foot of a stair); a placement stands only if every
 * zone placed so far, its own and each lobby door is still reached from the
 * main route through lobby floor at least 1 m wide (a free 2 x 2 block of
 * cells). One flood of the grid per test, and a placement tests its best few
 * candidates in score order, so it stays cheap.
 * ------------------------------------------------------------------------- */

/* Flood G->reach over free lobby floor, with frame rect blk (if any) taken as filled. */
static int access_flood(Gen *G, const float *blk) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, nx = L->nx, bi0 = 0, bj0 = 0, bi1 = 0, bj1 = 0;
    if (blk && !fix_cells(L, blk[0], blk[1], blk[2], blk[3], &bi0, &bj0, &bi1, &bj1)) bi0 = bi1 = 0;
    uint8_t *m = G->mark, *rc = G->reach;
    for (int c = 0; c < N; ++c) {
        int i = c % nx, j = c / nx;
        m[c] = L->occ[c] == IO_USABLE && L->own[c] == 0 && !(L->flag[c] & (IF_FIXTURE | IF_COLUMN)) &&
               !(i >= bi0 && i < bi1 && j >= bj0 && j < bj1);
        rc[c] = 0;
    }
    /* bit 1: the 2 x 2 block with this cell at its low corner is free */
    for (int j = 0; j + 1 < L->nz; ++j)
        for (int i = 0; i + 1 < nx; ++i) {
            int c = j * nx + i;
            if ((m[c] & 1) && (m[c + 1] & 1) && (m[c + nx] & 1) && (m[c + nx + 1] & 1)) m[c] |= 2;
        }
    int head = 0, tail = 0;
    for (int c = 0; c < N; ++c)
        if ((m[c] & 1) && (L->flag[c] & IF_ROUTE)) { rc[c] = 1; G->heap[tail++] = c; }
    while (head < tail) {
        int c = G->heap[head++], i = c % nx, j = c / nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (rc[n] || !(m[n] & 1)) continue;
            int wide = (m[n] & 2) || (a > 0 && (m[n - 1] & 2)) || (b > 0 && (m[n - nx] & 2)) || (a > 0 && b > 0 && (m[n - nx - 1] & 2));
            if (!wide) continue;
            rc[n] = 1;
            G->heap[tail++] = n;
        }
    }
    return tail > 0;
}

/* Does the last flood reach any cell of frame rect r? */
static int reached(const Gen *G, const float *r) {
    const ILayout *L = G->L;
    int i0 = (int)floorf((r[0] - L->gx0) / CELL + 1e-3f), j0 = (int)floorf((r[1] - L->gz0) / CELL + 1e-3f);
    int i1 = (int)ceilf((r[2] - L->gx0) / CELL - 1e-3f), j1 = (int)ceilf((r[3] - L->gz0) / CELL - 1e-3f);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > L->nx) i1 = L->nx;
    if (j1 > L->nz) j1 = L->nz;
    for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) if (G->reach[IDX(L, i, j)]) return 1;
    return 0;
}

/* Would filling blk leave every use zone, and the zones in extra, reachable? */
int access_ok(Gen *G, const float *blk, const float (*extra)[4], int nextra) {
    if (!access_flood(G, blk)) return 1;            /* no route through the lobby: nothing to keep */
    for (int k = 0; k < G->nneed; ++k) if (!reached(G, G->need[k])) return 0;
    for (int k = 0; k < nextra; ++k) if (!reached(G, extra[k])) return 0;
    return 1;
}

static void need_add(Gen *G, float x0, float z0, float x1, float z1) {
    if (G->nneed >= 48) return;
    float *n = G->need[G->nneed++];
    n[0] = x0; n[1] = z0; n[2] = x1; n[3] = z1;
}

/* The lobby side of each room door is a use zone, if it is reached before any furniture. */
void need_doors(Gen *G) {
    ILayout *L = G->L;
    if (!access_flood(G, NULL)) return;
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind == ID_ENTRANCE || dr->kind == ID_SERVICE || dr->kind == ID_FIRE) continue;
        for (int e = 0; e < 2; ++e) {
            int i0, j0, i1, j1;
            door_edge_cells(dr, dr->len / 2, &i0, &j0, &i1, &j1);
            int i = e ? i1 : i0, j = e ? j1 : j0;
            if (region_at(L, i, j) != 0) continue;
            float r[4] = { L->gx0 + (float)i * CELL, L->gz0 + (float)j * CELL, L->gx0 + (float)(i + 1) * CELL, L->gz0 + (float)(j + 1) * CELL };
            if (reached(G, r)) need_add(G, r[0], r[1], r[2], r[3]);
        }
    }
}

/* The best few candidates of a placement, in score order (ties keep the first found). */
#define TOP_N 12
typedef struct Top { int n; Cand c[TOP_N]; } Top;

static void top_push(Top *t, float s, int x, int z, int o) {
    if (t->n == TOP_N && s >= t->c[TOP_N - 1].score) return;
    int k = t->n < TOP_N ? t->n++ : TOP_N - 1;
    while (k > 0 && t->c[k - 1].score > s) { t->c[k] = t->c[k - 1]; --k; }
    t->c[k].score = s; t->c[k].x = x; t->c[k].z = z; t->c[k].o = o;
}

static void mark_access(ILayout *L, float x0, float z0, float x1, float z1) {
    int i0, j0, i1, j1;
    if (fix_cells(L, x0, z0, x1, z1, &i0, &j0, &i1, &j1))
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_ACCESS;
}

/* Distance (m) from each cell to the main route, into G->dist. */
static void route_distance(Gen *G) {
    ILayout *L = G->L;
    for (int c = 0; c < L->nx * L->nz; ++c) G->mark[c] = (uint8_t)((L->flag[c] & IF_ROUTE) != 0);
    dist_field(G);
}

/* A box of size (w along the facing's across axis, d along it), with its front
 * at distance `front` from (x, z) in direction f... expressed as a rect. */
static void oriented(int f, float ox, float oz, float w, float d0, float d1, float *x0, float *z0, float *x1, float *z1) {
    /* (ox, oz): front-left reference such that the rect spans `w` across and
     * [d0, d1) behind the front plane, going against the facing direction. */
    switch (f) {
    case 0:  *x0 = ox; *x1 = ox + w; *z0 = oz + d0; *z1 = oz + d1; break;    /* faces -z: depth grows +z */
    case 2:  *x0 = ox; *x1 = ox + w; *z0 = oz - d1; *z1 = oz - d0; break;
    case 1:  *z0 = oz; *z1 = oz + w; *x0 = ox - d1; *x1 = ox - d0; break;    /* faces +x: depth grows -x */
    default: *z0 = oz; *z1 = oz + w; *x0 = ox + d0; *x1 = ox + d1; break;
    }
}

int place_reception(Gen *G, float *rx, float *rz) {
    ILayout *L = G->L;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    float len = rng_range(&G->rrec, 4.5f, 6.5f);
    route_distance(G);
    for (int pass = 0; pass < 2; ++pass) {
        float dMin = pass ? 3.0f : 5.0f, dMax = pass ? 22.0f : 16.0f, rMin = pass ? 1.5f : 2.5f, rMax = pass ? 12.0f : 7.0f;
        float bx = 0, bz = 0;
        int bf = -1;
        Top top = { 0 };
        for (int f = 0; f < 4; ++f)
            for (int j = 0; j < L->nz; ++j)
                for (int i = 0; i < L->nx; ++i) {
                    float ox = L->gx0 + (float)i * CELL, oz = L->gz0 + (float)j * CELL;
                    float cx0, cz0, cx1, cz1, dx0, dz0, dx1, dz1, sx0, sz0, sx1, sz1;
                    oriented(f, ox, oz, len, 0.0f, 1.5f, &cx0, &cz0, &cx1, &cz1);     /* clear area in front */
                    oriented(f, ox, oz, len, 1.5f, 2.5f, &dx0, &dz0, &dx1, &dz1);     /* desk */
                    oriented(f, ox, oz, len, 2.5f, 4.0f, &sx0, &sz0, &sx1, &sz1);     /* staff zone */
                    float mx = (dx0 + dx1) * 0.5f, mz = (dz0 + dz1) * 0.5f;
                    float de = sqrtf((mx - ex) * (mx - ex) + (mz - ez) * (mz - ez));
                    if (de < dMin || de > dMax) continue;
                    float fdot = ((float)DX[f] * (ex - mx) + (float)DZ[f] * (ez - mz)) / (de + 1e-3f);
                    if (fdot < 0.4f) continue;
                    int mc = cell_at(L, mx, mz);
                    if (mc < 0) continue;
                    float dr = G->dist[mc] * CELL;
                    if (dr < rMin || dr > rMax) continue;
                    if (!fits(L, 0, dx0, dz0, dx1, dz1, FIT_SOLID) || !fits(L, 0, sx0, sz0, sx1, sz1, FIT_SOLID) ||
                        !fits(L, 0, cx0, cz0, cx1, cz1, FIT_CLEAR)) continue;
                    /* backed onto a wall? */
                    float bx0, bz0, bx1, bz1;
                    oriented(f, ox, oz, len, 4.0f, 4.5f, &bx0, &bz0, &bx1, &bz1);
                    int back = 0, nb = 0, i0, j0, i1, j1;
                    if (fix_cells(L, bx0, bz0, bx1, bz1, &i0, &j0, &i1, &j1)) {
                        for (int b = j0; b < j1; ++b) for (int a = i0; a < i1; ++a) { nb++; back += !usable(L, a, b) || L->own[IDX(L, a, b)] != 0; }
                    } else { back = nb = 1; }
                    float s = -3.0f * fdot - 5.0f * (float)back / (float)(nb ? nb : 1) + 0.15f * fabsf(de - 9.0f);
                    float off = fabsf(mx - ex);
                    s += G->recSide ? -0.25f * off : 0.35f * off;
                    s += 0.3f * rng_float(&G->rrec);
                    top_push(&top, s, i, j, f);
                }
        /* the best that leaves its visitor and staff sides, and all else, reachable */
        for (int t = 0; t < top.n && bf < 0; ++t) {
            float ox = L->gx0 + (float)top.c[t].x * CELL, oz = L->gz0 + (float)top.c[t].z * CELL, desk[4], use[2][4];
            oriented(top.c[t].o, ox, oz, len, 1.5f, 2.5f, &desk[0], &desk[1], &desk[2], &desk[3]);
            oriented(top.c[t].o, ox, oz, len, 0.0f, 1.5f, &use[0][0], &use[0][1], &use[0][2], &use[0][3]);
            oriented(top.c[t].o, ox, oz, len, 2.5f, 4.0f, &use[1][0], &use[1][1], &use[1][2], &use[1][3]);
            if (access_ok(G, desk, (const float (*)[4])use, 2)) { bx = ox; bz = oz; bf = top.c[t].o; }
            else if (t == 0) alog(G, "  info: best reception spot would be cut off; trying the next\n");
        }
        if (bf < 0) continue;
        float x0, z0, x1, z1;
        oriented(bf, bx, bz, len, 1.5f, 2.5f, &x0, &z0, &x1, &z1);
        add_fix(G, IX_DESK, bf, 0, x0, z0, x1, z1, 0.0f, 1.1f, 1);
        *rx = (x0 + x1) * 0.5f; *rz = (z0 + z1) * 0.5f;
        oriented(bf, bx, bz, len, 0.0f, 1.5f, &x0, &z0, &x1, &z1);     /* visitors' side */
        mark_access(L, x0, z0, x1, z1);
        need_add(G, x0, z0, x1, z1);
        oriented(bf, bx, bz, len, 2.5f, 4.0f, &x0, &z0, &x1, &z1);     /* staff side */
        mark_access(L, x0, z0, x1, z1);
        need_add(G, x0, z0, x1, z1);
        for (int k = 0; k < 2; ++k) {
            float t = len * (0.3f + 0.4f * (float)k);
            float cx0, cz0, cx1, cz1;
            oriented(bf, bx, bz, len, 2.8f, 3.4f, &cx0, &cz0, &cx1, &cz1);
            if (bf == 0 || bf == 2) { cx0 = bx + t - 0.3f; cx1 = cx0 + 0.6f; } else { cz0 = bz + t - 0.3f; cz1 = cz0 + 0.6f; }
            add_fix(G, IX_CHAIR, bf, 0, cx0, cz0, cx1, cz1, 0.0f, 1.0f, 0);
        }
        oriented(bf, bx, bz, len, 0.0f, 4.0f, &x0, &z0, &x1, &z1);
        IZone *z = &L->zone[L->nzone++];
        z->type = IZ_RECEPTION; z->x0 = x0; z->z0 = z0; z->x1 = x1; z->z1 = z1;
        if (pass) alog(G, "  simplify: reception placed with relaxed distances\n");
        alog(G, "  info: reception desk %.1f m long, %.1f m from the entrance\n", len,
             sqrtf((*rx - ex) * (*rx - ex) + (*rz - ez) * (*rz - ez)));
        return 1;
    }
    alog(G, "  simplify: reception dropped: no position faces the entrance at a fair distance\n");
    return 0;
}

/* A bay (waiting or cafe) of w x d m in the lobby, scored by `kind`. */
static int find_bay(Gen *G, float w, float d, int kind, float rx, float rz, int hasRec, float *ox, float *oz, int *rot) {
    ILayout *L = G->L;
    Rng *r = kind == IZ_WAITING ? &G->rwait : &G->rcafe;
    Top top = { 0 };
    for (int o = 0; o < 2; ++o) {
        float bw = o ? d : w, bd = o ? w : d;
        for (int j = 0; j < L->nz; ++j)
            for (int i = 0; i < L->nx; ++i) {
                float x0 = L->gx0 + (float)i * CELL, z0 = L->gz0 + (float)j * CELL;
                float cx = x0 + bw * 0.5f, cz = z0 + bd * 0.5f;
                float dr = hasRec ? sqrtf((cx - rx) * (cx - rx) + (cz - rz) * (cz - rz)) : 10.0f;
                if (kind == IZ_WAITING && (dr < 4.0f || dr > 14.0f)) continue;
                if (kind == IZ_CAFE && dr < 8.0f) continue;
                if (!fits(L, 0, x0, z0, x0 + bw, z0 + bd, FIT_SOLID)) continue;
                float s = (cz - G->minz) * (kind == IZ_WAITING ? 0.3f : 0.15f);
                if (kind == IZ_WAITING) s += 0.4f * fabsf(dr - 7.0f);
                if (kind == IZ_CAFE && L->atrium.x1 > L->atrium.x0) {
                    float vx = L->gx0 + (L->atrium.x0 + L->atrium.x1) * 0.5f * CELL, vz = L->gz0 + (L->atrium.z0 + L->atrium.z1) * 0.5f * CELL;
                    s += 0.1f * sqrtf((cx - vx) * (cx - vx) + (cz - vz) * (cz - vz));
                }
                s += 0.5f * rng_float(r);
                top_push(&top, s, i, j, o);
            }
    }
    /* the best that cuts nothing off and can itself be walked up to */
    for (int t = 0; t < top.n; ++t) {
        float bw = top.c[t].o ? d : w, bd = top.c[t].o ? w : d;
        float x0 = L->gx0 + (float)top.c[t].x * CELL, z0 = L->gz0 + (float)top.c[t].z * CELL;
        float bay[4] = { x0, z0, x0 + bw, z0 + bd }, ring[1][4] = { { x0 - CELL, z0 - CELL, x0 + bw + CELL, z0 + bd + CELL } };
        if (!access_ok(G, bay, (const float (*)[4])ring, 1)) continue;
        if (t > 0) alog(G, "  info: %s moved to its #%d spot to keep the floor connected\n", kind == IZ_WAITING ? "waiting area" : "cafe", t + 1);
        *ox = x0; *oz = z0; *rot = top.c[t].o;
        need_add(G, ring[0][0], ring[0][1], ring[0][2], ring[0][3]);
        return 1;
    }
    return 0;
}

void place_waiting(Gen *G, float rx, float rz, int hasRec) {
    ILayout *L = G->L;
    float x0, z0;
    int o;
    if (!find_bay(G, 4.5f, 3.5f, IZ_WAITING, rx, rz, hasRec, &x0, &z0, &o)) { alog(G, "  simplify: waiting area dropped: no bay\n"); return; }
    float w = o ? 3.5f : 4.5f, d = o ? 4.5f : 3.5f;
    mark_rect(L, x0, z0, x0 + w, z0 + d);
    if (!o) {
        float sx = x0 + (w - 2.2f) * 0.5f;
        add_fix(G, IX_SOFA, 2, 0, sx, z0 + 0.3f, sx + 2.2f, z0 + 1.2f, 0.0f, 0.8f, 0);
        add_fix(G, IX_TABLE, 0, 0, x0 + w * 0.5f - 0.6f, z0 + d * 0.5f - 0.3f, x0 + w * 0.5f + 0.6f, z0 + d * 0.5f + 0.3f, 0.0f, 0.42f, 0);
        add_fix(G, IX_SOFA, 0, 0, sx, z0 + d - 1.2f, sx + 2.2f, z0 + d - 0.3f, 0.0f, 0.8f, 0);
    } else {
        float sz = z0 + (d - 2.2f) * 0.5f;
        add_fix(G, IX_SOFA, 1, 0, x0 + 0.3f, sz, x0 + 1.2f, sz + 2.2f, 0.0f, 0.8f, 0);
        add_fix(G, IX_TABLE, 0, 0, x0 + w * 0.5f - 0.3f, z0 + d * 0.5f - 0.6f, x0 + w * 0.5f + 0.3f, z0 + d * 0.5f + 0.6f, 0.0f, 0.42f, 0);
        add_fix(G, IX_SOFA, 3, 0, x0 + w - 1.2f, sz, x0 + w - 0.3f, sz + 2.2f, 0.0f, 0.8f, 0);
    }
    IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, x0 + w - 0.8f, z0 + 0.1f, x0 + w - 0.1f, z0 + 0.8f, 0.0f, 1.5f, 0);
    if (pf) pf->group = IP_SINGLE;
    IZone *z = &L->zone[L->nzone++];
    z->type = IZ_WAITING; z->x0 = x0; z->z0 = z0; z->x1 = x0 + w; z->z1 = z0 + d;
    alog(G, "  info: waiting area by the frontage\n");
}

void place_cafe(Gen *G, float rx, float rz, int hasRec) {
    ILayout *L = G->L;
    float x0, z0;
    int o;
    if (!find_bay(G, 7.0f, 5.5f, IZ_CAFE, rx, rz, hasRec, &x0, &z0, &o)) { alog(G, "  simplify: cafe dropped: no bay apart from reception\n"); return; }
    float w = o ? 5.5f : 7.0f, d = o ? 7.0f : 5.5f;
    mark_rect(L, x0, z0, x0 + w, z0 + d);
    if (!o) {
        add_fix(G, IX_COUNTER, 0, 0, x0 + 0.5f, z0 + d - 1.2f, x0 + 4.1f, z0 + d - 0.4f, 0.0f, 1.05f, 0);
        for (int k = 0; k < 3; ++k) {
            float tx = x0 + 1.0f + (float)k * 2.1f, tz = z0 + 1.4f + (float)(k & 1) * 1.0f;
            add_fix(G, IX_TABLE, 0, 0, tx, tz, tx + 0.8f, tz + 0.8f, 0.0f, 0.75f, 0);
            add_fix(G, IX_CHAIR, 1, 0, tx - 0.55f, tz + 0.15f, tx - 0.05f, tz + 0.65f, 0.0f, 0.9f, 0);
            add_fix(G, IX_CHAIR, 3, 0, tx + 0.85f, tz + 0.15f, tx + 1.35f, tz + 0.65f, 0.0f, 0.9f, 0);
        }
    } else {
        add_fix(G, IX_COUNTER, 1, 0, x0 + w - 1.2f, z0 + 0.5f, x0 + w - 0.4f, z0 + 4.1f, 0.0f, 1.05f, 0);
        for (int k = 0; k < 3; ++k) {
            float tz = z0 + 1.0f + (float)k * 2.1f, tx = x0 + 1.4f + (float)(k & 1) * 1.0f;
            add_fix(G, IX_TABLE, 0, 0, tx, tz, tx + 0.8f, tz + 0.8f, 0.0f, 0.75f, 0);
            add_fix(G, IX_CHAIR, 2, 0, tx + 0.15f, tz - 0.55f, tx + 0.65f, tz - 0.05f, 0.0f, 0.9f, 0);
            add_fix(G, IX_CHAIR, 0, 0, tx + 0.15f, tz + 0.85f, tx + 0.65f, tz + 1.35f, 0.0f, 0.9f, 0);
        }
    }
    IZone *z = &L->zone[L->nzone++];
    z->type = IZ_CAFE; z->x0 = x0; z->z0 = z0; z->x1 = x0 + w; z->z1 = z0 + d;
    alog(G, "  info: lobby cafe\n");
}

void place_decoration(Gen *G) {
    ILayout *L = G->L;
    int planters = 0;
    /* planters along the frontage */
    for (float x = G->minx + 1.5f; x < G->maxx - 2.0f; x += rng_range(&G->rdec, 3.5f, 5.0f)) {
        for (float t = 0.0f; t < 3.0f; t += CELL) {
            float z = G->minz + 0.6f + t;
            int c = cell_at(L, x, z);
            if (c < 0 || L->occ[c] != IO_USABLE) continue;
            float pr[4] = { x, z, x + 1.0f, z + 1.0f };
            if (fits(L, 0, x, z, x + 1.0f, z + 1.0f, FIT_SOLID) && access_ok(G, pr, NULL, 0)) {
                IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, x, z, x + 1.0f, z + 1.0f, 0.0f, 1.6f, 1);
                if (pf) { pf->group = IP_ROW; pf->member = (uint8_t)(planters < 255 ? planters : 255); }
                planters++;
                break;
            }
        }
    }
    /* at the ends of the lift halls */
    for (int k = 0; k < L->ncore; ++k) {
        const ICore *K = &L->core[k];
        const IRegion *h = &L->reg[K->secure >= 0 ? K->secure : K->hall];
        float hx0 = L->gx0 + h->x0 * CELL, hx1 = L->gx0 + h->x1 * CELL, hz0 = L->gz0 + h->z0 * CELL, hz1 = L->gz0 + h->z1 * CELL;
        float px[2], pz[2];
        if (K->mouth == 0 || K->mouth == 2) {
            float z = K->mouth == 0 ? hz0 - 1.3f : hz1 + 0.3f;
            px[0] = hx0 - 1.3f; px[1] = hx1 + 0.3f; pz[0] = pz[1] = z;
        } else {
            float x = K->mouth == 3 ? hx0 - 1.3f : hx1 + 0.3f;
            pz[0] = hz0 - 1.3f; pz[1] = hz1 + 0.3f; px[0] = px[1] = x;
        }
        for (int s = 0; s < 2; ++s) {
            float pr[4] = { px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f };
            if (fits(L, 0, px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f, FIT_SOLID) && access_ok(G, pr, NULL, 0)) {
                IFixture *pf = add_fix(G, IX_PLANTER, 0, 0, px[s], pz[s], px[s] + 1.0f, pz[s] + 1.0f, 0.0f, 1.6f, 1);
                if (pf) { pf->group = IP_PAIR; pf->member = (uint8_t)(k * 2 + s); }
                planters++;
            }
        }
    }
    IAtrium *A = &L->atrium;
    if (A->x1 > A->x0) {
        /* a feature stair up to the gallery, along one side of the void */
        if (A->mode == IA_GALLERY) {
            float rise = A->galleryY - L->floorY, run = rise * 1.6f;
            float vz1 = L->gz0 + A->z1 * CELL, vx0 = L->gx0 + A->x0 * CELL, vx1 = L->gx0 + A->x1 * CELL;
            int first = rng_int(&G->rstair, 0, 1), placed = 0;
            for (int s = 0; s < 2 && !placed; ++s) {
                int side = (first + s) & 1;
                float x0 = side ? vx1 - 1.6f : vx0 + 0.1f;
                if (vz1 - run < L->gz0 + A->z0 * CELL + 1.0f) break;
                if (!fits(L, 0, x0, vz1 - run, x0 + 1.5f, vz1, FIT_VOID)) continue;
                float fl[4] = { x0, vz1 - run, x0 + 1.5f, vz1 }, foot[1][4] = { { x0, vz1 - run - 1.0f, x0 + 1.5f, vz1 - run } };
                if (!access_ok(G, fl, (const float (*)[4])foot, 1)) continue;   /* its foot must be walked up to */
                add_fix(G, IX_FLIGHT, 2, 0, x0, vz1 - run, x0 + 1.5f, vz1, 0.0f, rise, 1);
                mark_access(L, foot[0][0], foot[0][1], foot[0][2], foot[0][3]);
                need_add(G, foot[0][0], foot[0][1], foot[0][2], foot[0][3]);
                placed = 1;
                alog(G, "  info: feature stair to the gallery\n");
            }
            if (!placed) alog(G, "  simplify: no feature stair: the void has no clear side\n");
        }
        /* a feature piece in the void */
        float vcx = L->gx0 + (A->x0 + A->x1) * 0.5f * CELL, vcz = L->gz0 + (A->z0 + A->z1) * 0.5f * CELL;
        float h = minf(A->topY - L->floorY - 1.0f, rng_range(&G->rdec, 3.0f, 6.0f));
        for (int t = 0; t < 25; ++t) {
            float x = vcx - 0.75f + (float)((t % 5) - 2) * 1.5f, z = vcz - 0.75f + (float)((t / 5) - 2) * 1.5f;
            float fr[4] = { x, z, x + 1.5f, z + 1.5f };
            if (fits(L, 0, x, z, x + 1.5f, z + 1.5f, FIT_VOID) && access_ok(G, fr, NULL, 0)) {
                add_fix(G, IX_FEATURE, 0, 0, x, z, x + 1.5f, z + 1.5f, 0.0f, h, 1);
                break;
            }
        }
    }
    if (planters) alog(G, "  info: %d planters\n", planters);
}

/* Items along the wall opposite a room's door (step 6, fixtures 5). */
void furnish_room(Gen *G, int r) {
    ILayout *L = G->L;
    const IRegion *R = &L->reg[r];
    const IDoor *dr = NULL;
    for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && (L->door[d].kind == ID_ROOM || L->door[d].kind == ID_SERVICE)) { dr = &L->door[d]; if (dr->kind == ID_ROOM) break; }
    float x0 = L->gx0 + R->x0 * CELL, x1 = L->gx0 + R->x1 * CELL, z0 = L->gz0 + R->z0 * CELL, z1 = L->gz0 + R->z1 * CELL;
    /* far side: away from the door line */
    int far = 2;
    if (dr) {
        float lineM = (dr->axis == 0 ? L->gz0 : L->gx0) + dr->line * CELL;
        if (dr->axis == 0) far = fabsf(lineM - z0) < fabsf(lineM - z1) ? 2 : 0;
        else               far = fabsf(lineM - x0) < fabsf(lineM - x1) ? 1 : 3;
    }
    float iw, id, ih, gap;
    int type;
    switch (R->type) {
    case IR_TOILET:     type = IX_CUBICLE; iw = 0.9f; id = 1.5f; ih = 2.0f; gap = 0.0f; break;
    case IR_WC:         type = IX_UNIT;    iw = 0.7f; id = 0.6f; ih = 0.8f; gap = 0.4f; break;
    case IR_CLEANER:    type = IX_SHELF;   iw = 1.2f; id = 0.5f; ih = 1.8f; gap = 0.1f; break;
    case IR_SECURITY:   type = IX_DESK;    iw = 2.0f; id = 0.8f; ih = 0.75f; gap = 0.4f; break;
    case IR_MAIL:       type = IX_SHELF;   iw = 1.2f; id = 0.4f; ih = 2.0f; gap = 0.0f; break;
    case IR_FACILITIES: type = IX_DESK;    iw = 1.6f; id = 0.8f; ih = 0.75f; gap = 0.6f; break;
    case IR_PLANT:      type = IX_UNIT;    iw = 1.8f; id = 1.2f; ih = 2.0f; gap = 0.6f; break;
    case IR_SHOWER:     type = IX_LOCKER;  iw = 0.5f; id = 0.5f; ih = 1.9f; gap = 0.0f; break;
    case IR_CYCLE:      type = IX_RACK;    iw = 0.6f; id = 1.8f; ih = 1.1f; gap = 0.0f; break;
    case IR_BINS:       type = IX_BIN;     iw = 1.2f; id = 1.0f; ih = 1.3f; gap = 0.3f; break;
    case IR_LOADING:    type = IX_PALLET;  iw = 1.2f; id = 1.0f; ih = 0.15f; gap = 0.4f; break;
    case IR_MEETING: {
        float m = 1.1f;
        if (x1 - x0 > 2.0f * m + 0.8f && z1 - z0 > 2.0f * m + 0.8f && fits(L, r, x0 + m, z0 + m, x1 - m, z1 - m, FIT_SOLID)) {
            add_fix(G, IX_TABLE, 0, r, x0 + m, z0 + m, x1 - m, z1 - m, 0.0f, 0.75f, 1);
            int alongX = (x1 - x0) >= (z1 - z0);
            float a0 = alongX ? x0 + m : z0 + m, a1 = alongX ? x1 - m : z1 - m;
            for (float a = a0 + 0.2f; a + 0.5f <= a1; a += 0.8f) {
                if (alongX) {
                    add_fix(G, IX_CHAIR, 2, r, a, z0 + m - 0.6f, a + 0.5f, z0 + m - 0.1f, 0.0f, 0.9f, 0);
                    add_fix(G, IX_CHAIR, 0, r, a, z1 - m + 0.1f, a + 0.5f, z1 - m + 0.6f, 0.0f, 0.9f, 0);
                } else {
                    add_fix(G, IX_CHAIR, 1, r, x0 + m - 0.6f, a, x0 + m - 0.1f, a + 0.5f, 0.0f, 0.9f, 0);
                    add_fix(G, IX_CHAIR, 3, r, x1 - m + 0.1f, a, x1 - m + 0.6f, a + 0.5f, 0.0f, 0.9f, 0);
                }
            }
        }
        return;
    }
    case IR_ESCAPE: {
        int alongX = (x1 - x0) >= (z1 - z0);
        float w = alongX ? (z1 - z0 - 0.4f) * 0.5f : (x1 - x0 - 0.4f) * 0.5f, rise = (L->ceilY - L->floorY) * 0.5f;
        for (int k = 0; k < 2; ++k) {
            float a = (float)k * (w + 0.1f) + 0.15f, fx0, fz0, fx1, fz1;
            if (alongX) { fx0 = x0 + 1.2f; fx1 = x1 - 1.2f; fz0 = z0 + a; fz1 = fz0 + w; }
            else        { fz0 = z0 + 1.2f; fz1 = z1 - 1.2f; fx0 = x0 + a; fx1 = fx0 + w; }
            if (fits(L, r, fx0, fz0, fx1, fz1, FIT_CLEAR))
                add_fix(G, IX_FLIGHT, alongX ? (k ? 3 : 1) : (k ? 0 : 2), r, fx0, fz0, fx1, fz1, 0.0f, rise, 0);
        }
        return;
    }
    default: return;
    }
    int alongX = far == 0 || far == 2;
    float a0 = alongX ? x0 : z0, a1 = alongX ? x1 : z1;
    for (float a = a0 + 0.15f; a + iw <= a1 - 0.1f; a += iw + gap) {
        float bx0, bz0, bx1, bz1;
        if (far == 2)      { bx0 = a; bx1 = a + iw; bz1 = z1 - 0.05f; bz0 = bz1 - id; }
        else if (far == 0) { bx0 = a; bx1 = a + iw; bz0 = z0 + 0.05f; bz1 = bz0 + id; }
        else if (far == 1) { bz0 = a; bz1 = a + iw; bx1 = x1 - 0.05f; bx0 = bx1 - id; }
        else               { bz0 = a; bz1 = a + iw; bx0 = x0 + 0.05f; bx1 = bx0 + id; }
        /* the wall may lie inside the bounding cell row: slide in until it fits */
        int ok = 0;
        for (int t = 0; t < 4 && !ok; ++t) {
            float sh = (float)t * CELL;
            float sx = far == 1 ? -sh : far == 3 ? sh : 0.0f, sz = far == 2 ? -sh : far == 0 ? sh : 0.0f;
            if (fits(L, r, bx0 + sx, bz0 + sz, bx1 + sx, bz1 + sz, FIT_SOLID)) { bx0 += sx; bx1 += sx; bz0 += sz; bz1 += sz; ok = 1; }
        }
        if (!ok) continue;
        float h = type == IX_PALLET ? 0.15f + 0.9f * rng_float(&G->rdec) : ih;
        add_fix(G, type, (far + 2) & 3, r, bx0, bz0, bx1, bz1, 0.0f, h, 1);
        if (type == IX_DESK) {
            float cx0 = bx0, cz0 = bz0, cx1 = bx1, cz1 = bz1;
            if (far == 2) { cz1 = bz0 - 0.1f; cz0 = cz1 - 0.5f; cx0 = (bx0 + bx1) * 0.5f - 0.25f; cx1 = cx0 + 0.5f; }
            else if (far == 0) { cz0 = bz1 + 0.1f; cz1 = cz0 + 0.5f; cx0 = (bx0 + bx1) * 0.5f - 0.25f; cx1 = cx0 + 0.5f; }
            else if (far == 1) { cx1 = bx0 - 0.1f; cx0 = cx1 - 0.5f; cz0 = (bz0 + bz1) * 0.5f - 0.25f; cz1 = cz0 + 0.5f; }
            else { cx0 = bx1 + 0.1f; cx1 = cx0 + 0.5f; cz0 = (bz0 + bz1) * 0.5f - 0.25f; cz1 = cz0 + 0.5f; }
            add_fix(G, IX_CHAIR, far, r, cx0, cz0, cx1, cz1, 0.0f, 0.95f, 0);
            if (R->type == IR_SECURITY) {
                float sx0 = bx0, sz0 = bz0, sx1 = bx1, sz1 = bz1;
                if (far == 2) sz0 = sz1 - 0.2f; else if (far == 0) sz1 = sz0 + 0.2f; else if (far == 1) sx0 = sx1 - 0.2f; else sx1 = sx0 + 0.2f;
                add_fix(G, IX_SCREEN, (far + 2) & 3, r, sx0, sz0, sx1, sz1, 0.75f, 1.25f, 0);
            }
        }
    }
}
