#include "interior_internal.h"

/* ===========================================================================
 * Boundaries (section 6.4) and realised runs
 * ========================================================================= */

int interior_boundary(const ILayout *L, int a, int b) {
    if (a == b) return IB_NONE;
    if (a < 0 || b < 0) return IB_ENVELOPE;
    const IRegion *ra = &L->reg[a], *rb = &L->reg[b];
    if (ra->kind == IK_SOLID || rb->kind == IK_SOLID) return IB_SOLID;
    if (ra->type == IR_VESTIBULE || rb->type == IR_VESTIBULE) return IB_GLASS;
    if (ra->kind == IK_ROOM && rb->kind == IK_ROOM) return IB_WALL;
    if (ra->kind == IK_ROOM) return ra->glazed ? IB_GLASS : IB_WALL;
    if (rb->kind == IK_ROOM) return rb->glazed ? IB_GLASS : IB_WALL;
    if (ra->type == IR_LOBBY || rb->type == IR_LOBBY) {
        const IRegion *o = ra->type == IR_LOBBY ? rb : ra;
        if (o->type == IR_SECURE) return IB_BARRIER;
        if (o->type == IR_HALL) return L->core[o->core].gated ? IB_BARRIER : IB_OPEN;
        return IB_OPEN;
    }
    if (ra->core == rb->core) return IB_OPEN;      /* secure side and its own hall */
    return IB_WALL;
}

/* Door index + 1 on each cell edge: horizontal edges (axis 0) then vertical. */
static int16_t *door_map(const ILayout *L) {
    int nh = (L->nz + 1) * L->nx, nv = L->nz * (L->nx + 1);
    int16_t *m = (int16_t *)calloc((size_t)(nh + nv), sizeof(int16_t));
    if (!m) fatal("out of memory realising an interior");
    for (int d = 0; d < L->ndoor; ++d) {
        const IDoor *dr = &L->door[d];
        for (int e = 0; e < dr->len; ++e) {
            int p = dr->at + e;
            if (dr->axis == 0) { if (p >= 0 && p < L->nx && dr->line >= 0 && dr->line <= L->nz) m[dr->line * L->nx + p] = (int16_t)(d + 1); }
            else if (p >= 0 && p < L->nz && dr->line >= 0 && dr->line <= L->nx) m[nh + p * (L->nx + 1) + dr->line] = (int16_t)(d + 1);
        }
    }
    return m;
}

/* Moves the ends of a run onto the envelope: back if they poke out of it,
 * forward (up to 1 m) if they stop short of it. */
static void fit_envelope(const ILayout *L, float *x0, float *z0, float *x1, float *z1) {
    float dx = *x1 - *x0, dz = *z1 - *z0, len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-4f) return;
    float ux = dx / len, uz = dz / len;
    /* crossings of the line with the footprint, as distances along it from (x0, z0) */
    float ts[2 * BG_MAX_PV];
    int nt = 0;
    const BLoop *loops[2] = { &L->foot.outer, &L->foot.hole };
    for (int li = 0; li < 2; ++li) {
        const BLoop *l = loops[li];
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            float ax = l->x[k], az = l->z[k], bx = l->x[k1] - ax, bz = l->z[k1] - az;
            float den = ux * bz - uz * bx;
            if (fabsf(den) < 1e-6f) continue;
            float wx = ax - *x0, wz = az - *z0;
            float t = (wx * bz - wz * bx) / den, s = (wx * uz - wz * ux) / den;
            if (s >= -1e-4f && s <= 1.0f + 1e-4f && nt < 2 * BG_MAX_PV) ts[nt++] = t;
        }
    }
    const float ext = 1.0f;
    int in0 = bfoot_contains(&L->foot, *x0 + ux * 0.02f, *z0 + uz * 0.02f), in1 = bfoot_contains(&L->foot, *x1 - ux * 0.02f, *z1 - uz * 0.02f);
    float t0 = 0.0f, t1 = len;
    if (in0) { float best = -ext; for (int i = 0; i < nt; ++i) if (ts[i] <= 0.0f && ts[i] > best) best = ts[i]; if (best > -ext) t0 = best; }
    else     { float best = len;  for (int i = 0; i < nt; ++i) if (ts[i] > 0.0f && ts[i] < best) best = ts[i]; t0 = best; }
    if (in1) { float best = len + ext; for (int i = 0; i < nt; ++i) if (ts[i] >= len && ts[i] < best) best = ts[i]; if (best < len + ext) t1 = best; }
    else     { float best = 0.0f; for (int i = 0; i < nt; ++i) if (ts[i] < len && ts[i] > best) best = ts[i]; t1 = best; }
    if (t1 <= t0 + 0.05f) { t0 = 0.0f; t1 = len; }
    float ox = *x0, oz = *z0;
    *x0 = ox + ux * t0; *z0 = oz + uz * t0;
    *x1 = ox + ux * t1; *z1 = oz + uz * t1;
}

int interior_walls(const ILayout *L, IWall *out, int max) {
    int16_t *dm = door_map(L);
    int nh = (L->nz + 1) * L->nx, n = 0;
    for (int axis = 0; axis < 2; ++axis) {
        int nLines = axis == 0 ? L->nz + 1 : L->nx + 1, nAlong = axis == 0 ? L->nx : L->nz;
        for (int line = 0; line < nLines; ++line) {
            int runType = IB_NONE, runDoor = 0, runSide = 0, runAt = 0, runKind = 0;
            for (int k = 0; k <= nAlong; ++k) {
                int type = IB_NONE, door = 0, side = 0, dk = 0;
                if (k < nAlong) {
                    int ra, rb;
                    if (axis == 0) { ra = region_at(L, k, line - 1); rb = region_at(L, k, line); }
                    else           { ra = region_at(L, line - 1, k); rb = region_at(L, line, k); }
                    type = interior_boundary(L, ra, rb);
                    int di = axis == 0 ? dm[line * L->nx + k] : dm[nh + k * (L->nx + 1) + line];
                    if (di) { door = L->door[di - 1].kind == ID_GATE ? 2 : 1; dk = L->door[di - 1].kind + 1; }
                    if (type == IB_WALL || type == IB_GLASS) {
                        int ka = kind_of(L, ra), kb = kind_of(L, rb);
                        side = (ka == IK_ROOM && kb != IK_ROOM) ? -1 : (kb == IK_ROOM && ka != IK_ROOM) ? 1 : 0;
                        if (ra >= 0 && rb >= 0 && L->reg[ra].type == IR_VESTIBULE) side = -1;
                        if (ra >= 0 && rb >= 0 && L->reg[rb].type == IR_VESTIBULE) side = 1;
                    }
                    if (type != IB_WALL && type != IB_GLASS && type != IB_BARRIER) { type = IB_NONE; door = 0; dk = 0; }
                }
                if (type == runType && door == runDoor && side == runSide && dk == runKind && type != IB_NONE) continue;
                if (runType != IB_NONE && n < max) {
                    IWall *w = &out[n++];
                    w->type = (uint8_t)runType; w->door = (uint8_t)runDoor; w->doorKind = (uint8_t)(runKind ? runKind - 1 : 0); w->side = (int8_t)runSide;
                    float a0 = (float)runAt * CELL, a1 = (float)k * CELL, lm = (float)line * CELL;
                    if (axis == 0) { w->x0 = L->gx0 + a0; w->x1 = L->gx0 + a1; w->z0 = w->z1 = L->gz0 + lm; }
                    else           { w->z0 = L->gz0 + a0; w->z1 = L->gz0 + a1; w->x0 = w->x1 = L->gx0 + lm; }
                    if (!runDoor) fit_envelope(L, &w->x0, &w->z0, &w->x1, &w->z1);
                }
                runType = type; runDoor = door; runSide = side; runAt = k; runKind = dk;
            }
        }
    }
    free(dm);
    return n;
}

/* ===========================================================================
 * Step 7: validation
 * ========================================================================= */

static int passable(const ILayout *L, const int16_t *dm, int c, int n, int axis, int line, int along) {
    int a = L->own[c], b = L->own[n];
    if (a == b) return 1;
    int nh = (L->nz + 1) * L->nx;
    int di = axis == 0 ? dm[line * L->nx + along] : dm[nh + along * (L->nx + 1) + line];
    if (di) return 1;
    int t = interior_boundary(L, a == NONE ? -1 : a, b == NONE ? -1 : b);
    return t == IB_OPEN;
}

int validate(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    if (!L->ncore) return fail(G, "no core");
    int lobbyCells = 0;
    for (int c = 0; c < N; ++c) lobbyCells += L->own[c] == 0;
    if (!lobbyCells) return fail(G, "no lobby");
    for (int k = 0; k < L->ncore; ++k) if (!L->core[k].stairs) return fail(G, "core %d has no stair", k);

    /* reachability from the entrance, crossing boundaries only where they pass */
    int16_t *dm = door_map(L);
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    int head = 0, tail = 0;
    int start = L->routeStart;
    for (int d = 0; d < L->ndoor; ++d)
        if (L->door[d].kind == ID_ENTRANCE) {
            int i0, j0, i1, j1;
            door_edge_cells(&L->door[d], L->door[d].len / 2, &i0, &j0, &i1, &j1);
            if (usable(L, i1, j1)) start = IDX(L, i1, j1);
        }
    G->heap[tail++] = start;
    G->mark[start] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->mark[n] || L->own[n] == NONE) continue;
            if (interior_kind(L->reg[L->own[n]].type) == IK_SOLID) continue;
            int axis = DZ[d] != 0 ? 0 : 1;
            int line = axis == 0 ? (DZ[d] > 0 ? j + 1 : j) : (DX[d] > 0 ? i + 1 : i);
            int along = axis == 0 ? i : j;
            if (!passable(L, dm, c, n, axis, line, along)) continue;
            G->mark[n] = 1;
            G->heap[tail++] = n;
        }
    }
    free(dm);
    int ok = 1;
    for (int d = 0; d < L->ndoor && ok; ++d) {
        const IDoor *dr = &L->door[d];
        if (dr->kind == ID_ENTRANCE || dr->kind == ID_SERVICE || dr->kind == ID_FIRE) continue;
        int i0, j0, i1, j1;
        door_edge_cells(dr, dr->len / 2, &i0, &j0, &i1, &j1);
        int r0 = region_at(L, i0, j0), r1 = region_at(L, i1, j1);
        int c0 = usable(L, i0, j0) ? IDX(L, i0, j0) : -1, c1 = usable(L, i1, j1) ? IDX(L, i1, j1) : -1;
        int reached = (c0 >= 0 && kind_of(L, r0) != IK_SOLID && G->mark[c0]) || (c1 >= 0 && kind_of(L, r1) != IK_SOLID && G->mark[c1]);
        if (!reached) ok = fail(G, "%s door of the %s is unreachable", dr->kind == ID_LIFT ? "lift" : dr->kind == ID_STAIR ? "stair" : "a",
                                REG_NAMES[L->reg[dr->a >= 0 ? dr->a : 0].type]);
    }
    for (int r = 0; r < L->nreg && ok; ++r) {
        const IRegion *R = &L->reg[r];
        if (R->kind == IK_SOLID || !R->cells) continue;
        int hasDoor = R->kind != IK_ROOM;
        for (int d = 0; d < L->ndoor; ++d) if (L->door[d].a == r && L->door[d].kind == ID_ROOM) hasDoor = 1;
        if (!hasDoor) continue;
        int reached = 0;
        for (int c = 0; c < N && !reached; ++c) reached = L->own[c] == r && G->mark[c];
        if (!reached) ok = fail(G, "the %s is unreachable", REG_NAMES[R->type]);
    }
    if (!ok) return 0;

    /* no improper overlap, clear approaches */
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    for (int f = 0; f < L->nfix; ++f) {
        const IFixture *x = &L->fix[f];
        if (x->region < 0 || x->type == IX_CHAIR || x->type == IX_SCREEN) continue;
        int i0, j0, i1, j1;
        if (!fix_cells(L, x->x0 + 0.01f, x->z0 + 0.01f, x->x1 - 0.01f, x->z1 - 0.01f, &i0, &j0, &i1, &j1)) return fail(G, "fixture off the grid");
        for (int j = j0; j < j1; ++j)
            for (int i = i0; i < i1; ++i) {
                int c = IDX(L, i, j);
                if (L->own[c] != x->region) return fail(G, "fixture outside its region");
                if (L->flag[c] & (IF_COLUMN | IF_APPROACH)) return fail(G, "fixture on a column or lift approach");
                if (G->mark[c] && x->region == 0 && x->type != IX_SOFA && x->type != IX_TABLE && x->type != IX_PLANTER && x->type != IX_COUNTER)
                    return fail(G, "fixtures overlap");
                G->mark[c] = 1;
            }
    }
    for (int k = 0; k < L->ncol; ++k) {
        int c = IDX(L, L->col[k][0], L->col[k][1]);
        if (L->flag[c] & IF_APPROACH) return fail(G, "column in a lift approach");
    }

    /* route width, with fixtures as obstacles */
    for (int c = 0; c < N; ++c) {
        int o = L->own[c];
        G->mark[c] = (uint8_t)(L->occ[c] != IO_USABLE || o == NONE || interior_kind(L->reg[o].type) != IK_OPEN ||
                               (L->flag[c] & (IF_COLUMN | IF_FIXTURE)));
    }
    for (int f = 0; f < L->nfix; ++f) {
        const IFixture *x = &L->fix[f];
        if (x->region != 0) continue;
        int i0, j0, i1, j1;
        if (fix_cells(L, x->x0 + 0.01f, x->z0 + 0.01f, x->x1 - 0.01f, x->z1 - 0.01f, &i0, &j0, &i1, &j1))
            for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) G->mark[IDX(L, i, j)] = 1;
    }
    dist_field(G);
    for (int k = 0; k < L->ncore; ++k) {
        int n = G->npath[k];
        for (int p = 6; p < n - 4; ++p) {
            if (L->core[k].gated && abs(p - G->gateAt[k]) <= 4) continue;
            int c = G->path[k][p];
            if (L->own[c] == L->core[k].hall) continue;
            if (clear_width(G->dist[c]) < 1.5f - 1e-3f) return fail(G, "route to core %d narrows to %.1f m", k, clear_width(G->dist[c]));
        }
    }

    /* the void is preserved */
    for (int c = 0; c < N; ++c)
        if ((L->flag[c] & IF_VOID) && L->own[c] != NONE && interior_kind(L->reg[L->own[c]].type) != IK_OPEN) return fail(G, "a room or core stands in the void");

    /* every use zone can be walked to */
    if (G->nneed && !access_ok(G, NULL, NULL, 0)) return fail(G, "a fixture's use zone is cut off");
    return 1;
}
