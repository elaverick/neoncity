#include "interior_internal.h"

/* ===========================================================================
 * Frame and grid
 * ========================================================================= */

void interior_to_local(const ILayout *L, float fx, float fz, float *x, float *z) { quarter_turn(L->quarter, fx, fz, x, z); }
void interior_to_frame(const ILayout *L, float x, float z, float *fx, float *fz) { quarter_turn(4 - L->quarter, x, z, fx, fz); }

int cell_at(const ILayout *L, float fx, float fz) {
    int i = (int)floorf((fx - L->gx0) / CELL), j = (int)floorf((fz - L->gz0) / CELL);
    if (i < 0 || j < 0 || i >= L->nx || j >= L->nz) return -1;
    return IDX(L, i, j);
}

int usable(const ILayout *L, int i, int j) { return in_grid(L, i, j) && L->occ[IDX(L, i, j)] == IO_USABLE; }
int region_at(const ILayout *L, int i, int j) {
    if (!usable(L, i, j)) return -1;
    int o = L->own[IDX(L, i, j)];
    return o == NONE ? -1 : o;
}
int kind_of(const ILayout *L, int r) { return r < 0 ? -1 : interior_kind(L->reg[r].type); }

/* ===========================================================================
 * Rectangle queries (prefix sums), distance fields and paths (section 5.2)
 * ========================================================================= */

static int blocked(const Gen *G, int q, int c) {
    const ILayout *L = G->L;
    uint16_t f = L->flag[c];
    switch (q) {
    case Q_SOLID:  return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_VOID | IF_ENTRY | IF_DOORCLEAR | IF_RESERVE));
    case Q_MOUTH:  return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_VOID));
    case Q_FREE:   return L->occ[c] != IO_USABLE || L->own[c] != NONE || (f & (IF_EDGE | IF_ENTRY | IF_DOORCLEAR));
    case Q_MARKED: return !G->mark[c];
    default:       return (f & IF_FRONT) != 0;
    }
}

void sat_build(const Gen *G, int q, int *sat) {
    const ILayout *L = G->L;
    int W = L->nx + 1;
    for (int i = 0; i < W; ++i) sat[i] = 0;
    for (int j = 0; j < L->nz; ++j) {
        int row = 0;
        sat[(j + 1) * W] = 0;
        for (int i = 0; i < L->nx; ++i) {
            row += blocked(G, q, IDX(L, i, j));
            sat[(j + 1) * W + i + 1] = sat[j * W + i + 1] + row;
        }
    }
}

/* Blocked cells in [i0,i1) x [j0,j1); anything off the grid counts as blocked. */
int sat_count(const ILayout *L, const int *sat, int i0, int j0, int i1, int j1) {
    if (i0 < 0 || j0 < 0 || i1 > L->nx || j1 > L->nz || i1 <= i0 || j1 <= j0) return 1 << 20;
    int W = L->nx + 1;
    return sat[j1 * W + i1] - sat[j0 * W + i1] - sat[j1 * W + i0] + sat[j0 * W + i0];
}

/* Chamfer distance (in cells) from each cell centre to the nearest obstacle
 * centre; off-grid counts as obstacle. obstacle[] is G->mark. */
void dist_field(Gen *G) {
    const ILayout *L = G->L;
    int nx = L->nx, nz = L->nz;
    const float D1 = 1.0f, D2 = 1.41421356f;
    for (int c = 0; c < nx * nz; ++c) G->dist[c] = G->mark[c] ? 0.0f : 1e9f;
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            int c = IDX(L, i, j);
            float d = G->dist[c];
            if (d == 0.0f) continue;
            d = minf(d, i > 0 ? G->dist[c - 1] + D1 : D1);
            d = minf(d, j > 0 ? G->dist[c - nx] + D1 : D1);
            d = minf(d, i > 0 && j > 0 ? G->dist[c - nx - 1] + D2 : D2);
            d = minf(d, i < nx - 1 && j > 0 ? G->dist[c - nx + 1] + D2 : D2);
            G->dist[c] = d;
        }
    for (int j = nz - 1; j >= 0; --j)
        for (int i = nx - 1; i >= 0; --i) {
            int c = IDX(L, i, j);
            float d = G->dist[c];
            if (d == 0.0f) continue;
            d = minf(d, i < nx - 1 ? G->dist[c + 1] + D1 : D1);
            d = minf(d, j < nz - 1 ? G->dist[c + nx] + D1 : D1);
            d = minf(d, i < nx - 1 && j < nz - 1 ? G->dist[c + nx + 1] + D2 : D2);
            d = minf(d, i > 0 && j < nz - 1 ? G->dist[c + nx - 1] + D2 : D2);
            G->dist[c] = d;
        }
}

/* Clear width (m) of a corridor whose centre cell is d cells from an obstacle. */
float clear_width(float d) { return 2.0f * (d - 0.5f) * CELL; }

void heap_push(Gen *G, float key, int c) {
    int i = G->nheap++;
    while (i > 0) {
        int p = (i - 1) >> 1;
        if (G->hkey[p] <= key) break;
        G->hkey[i] = G->hkey[p]; G->heap[i] = G->heap[p];
        i = p;
    }
    G->hkey[i] = key; G->heap[i] = c;
}

int heap_pop(Gen *G, float *key) {
    int top = G->heap[0];
    *key = G->hkey[0];
    float k = G->hkey[--G->nheap];
    int c = G->heap[G->nheap], i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        float mk = k;
        if (l < G->nheap && G->hkey[l] < mk) { m = l; mk = G->hkey[l]; }
        if (r < G->nheap && G->hkey[r] < mk) { m = r; }
        if (m == i) break;
        G->hkey[i] = G->hkey[m]; G->heap[i] = G->heap[m];
        i = m;
    }
    G->hkey[i] = k; G->heap[i] = c;
    return top;
}

/* ===========================================================================
 * Regions and doors
 * ========================================================================= */

int add_region(Gen *G, int type, int core) {
    ILayout *L = G->L;
    if (L->nreg >= IN_MAX_REG) return -1;
    IRegion *r = &L->reg[L->nreg];
    memset(r, 0, sizeof(*r));
    r->type = (uint8_t)type;
    r->kind = (uint8_t)interior_kind(type);
    r->glazed = (uint8_t)room_spec(type)->glazed * (r->kind == IK_ROOM);
    r->core = (uint8_t)core;
    return L->nreg++;
}

void claim_rect(Gen *G, int r, int i0, int j0, int i1, int j1) {
    ILayout *L = G->L;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i)
            if (usable(L, i, j)) L->own[IDX(L, i, j)] = (uint8_t)r;
}

void absorb_columns(Gen *G, int i0, int j0, int i1, int j1, const char *by) {
    ILayout *L = G->L;
    for (int k = 0; k < L->ncol; ++k) {
        int i = L->col[k][0], j = L->col[k][1];
        if (i < i0 || i >= i1 || j < j0 || j >= j1) continue;
        L->flag[IDX(L, i, j)] &= (uint16_t)~IF_COLUMN;
        alog(G, "  info: column at (%.1f, %.1f) absorbed by %s\n", ccx(L, i), ccz(L, j), by);
        L->col[k][0] = L->col[L->ncol - 1][0];
        L->col[k][1] = L->col[L->ncol - 1][1];
        L->ncol--;
        k--;
    }
}

int add_door(Gen *G, int kind, int axis, int line, int at, int len, int a, int b) {
    ILayout *L = G->L;
    if (L->ndoor >= IN_MAX_DOOR || len <= 0) return -1;
    IDoor *d = &L->door[L->ndoor];
    d->kind = (uint8_t)kind; d->axis = (uint8_t)axis;
    d->line = (int16_t)line; d->at = (int16_t)at; d->len = (int16_t)len;
    d->a = (int16_t)a; d->b = (int16_t)b;
    return L->ndoor++;
}

/* The cells either side of edge e of a door run: c0 on the low side of the line, c1 on the high side. */
void door_edge_cells(const IDoor *d, int e, int *i0, int *j0, int *i1, int *j1) {
    if (d->axis == 0) { *i0 = *i1 = d->at + e; *j0 = d->line - 1; *j1 = d->line; }
    else              { *j0 = *j1 = d->at + e; *i0 = d->line - 1; *i1 = d->line; }
}

static int other_ok(const ILayout *L, int o, int b) {
    if (b >= 0) return o == b;
    if (b == -1) return o == -1;
    return o >= 0 && interior_kind(L->reg[o].type) == IK_OPEN && L->reg[o].type != IR_VESTIBULE;
}

static int edge_cell_ok(const ILayout *L, int i, int j, int b) {
    if (b == -1) return 1;
    return in_grid(L, i, j) && !(L->flag[IDX(L, i, j)] & (IF_COLUMN | IF_FIXTURE));
}

Run best_run(const ILayout *L, int a, int b) {
    Run best = { 0, 0, 0, 0, -1 };
    float bestScore = -1.0f;
    for (int axis = 0; axis < 2; ++axis) {
        int nLines = axis == 0 ? L->nz + 1 : L->nx + 1, nAlong = axis == 0 ? L->nx : L->nz;
        for (int line = 0; line < nLines; ++line) {
            int runAt = 0, runLen = 0, runOther = -3;
            for (int k = 0; k <= nAlong; ++k) {
                int other = -3;
                if (k < nAlong) {
                    int ia, ja, ib, jb;
                    if (axis == 0) { ia = ib = k; ja = line - 1; jb = line; } else { ja = jb = k; ia = line - 1; ib = line; }
                    int ra = region_at(L, ia, ja), rb = region_at(L, ib, jb);
                    if (ra == a && rb != a && other_ok(L, rb, b) && edge_cell_ok(L, ib, jb, b)) other = rb;
                    else if (rb == a && ra != a && other_ok(L, ra, b) && edge_cell_ok(L, ia, ja, b)) other = ra;
                }
                if (other != -3 && other == runOther) { runLen++; continue; }
                if (runLen > 0) {
                    float s = (float)runLen + (b == -2 && runOther == 0 ? 1000.0f : 0.0f);
                    if (s > bestScore) { bestScore = s; best.axis = axis; best.line = line; best.at = runAt; best.len = runLen; best.other = runOther; }
                }
                runOther = other; runAt = k; runLen = other != -3 ? 1 : 0;
            }
        }
    }
    return best;
}

void mark_door_clear(Gen *G, const IDoor *d, int depth) {
    ILayout *L = G->L;
    for (int e = 0; e < d->len; ++e) {
        int i0, j0, i1, j1;
        door_edge_cells(d, e, &i0, &j0, &i1, &j1);
        for (int k = 0; k < depth; ++k) {
            int ia = d->axis == 0 ? i0 : i0 - k, ja = d->axis == 0 ? j0 - k : j0;
            int ib = d->axis == 0 ? i1 : i1 + k, jb = d->axis == 0 ? j1 + k : j1;
            if (usable(L, ia, ja)) L->flag[IDX(L, ia, ja)] |= IF_DOORCLEAR;
            if (usable(L, ib, jb)) L->flag[IDX(L, ib, jb)] |= IF_DOORCLEAR;
        }
    }
}
