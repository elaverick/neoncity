#include "interior_internal.h"

/* ===========================================================================
 * Step 5: back of house, cut from bands along the facades
 * ========================================================================= */

/* Bands: 0 rear (+z facade), 1 left (-x), 2 right (+x), 3 front (-z).
 * Along: x for rear/front, z for left/right. The facade position is the cell
 * line where the band's outermost usable cell ends. */
static int band_len(const ILayout *L, int side) { return (side == 0 || side == 3) ? L->nx : L->nz; }

static int band_facade(const ILayout *L, int side, int a) {
    switch (side) {
    case 0:  for (int j = L->nz - 1; j >= 0; --j) if (usable(L, a, j)) return j + 1; return -1;
    case 3:  for (int j = 0; j < L->nz; ++j) if (usable(L, a, j)) return j; return -1;
    case 1:  for (int i = 0; i < L->nx; ++i) if (usable(L, i, a)) return i; return -1;
    default: for (int i = L->nx - 1; i >= 0; --i) if (usable(L, i, a)) return i + 1; return -1;
    }
}

/* Cell at along a and depth d (0 = against the facade at `outer`). */
static void band_cell(int side, int outer, int a, int d, int *i, int *j) {
    switch (side) {
    case 0:  *i = a; *j = outer - 1 - d; break;
    case 3:  *i = a; *j = outer + d; break;
    case 1:  *j = a; *i = outer + d; break;
    default: *j = a; *i = outer - 1 - d; break;
    }
}

typedef struct BandCand { int side, a, w, depth, outer; float score; } BandCand;

static float hall_dist(const ILayout *L, float x, float z) {
    float best = 1e9f;
    for (int k = 0; k < L->ncore; ++k) {
        const IRegion *h = &L->reg[L->core[k].hall];
        float hx = L->gx0 + (h->x0 + h->x1) * 0.5f * CELL, hz = L->gz0 + (h->z0 + h->z1) * 0.5f * CELL;
        best = minf(best, sqrtf((x - hx) * (x - hx) + (z - hz) * (z - hz)));
    }
    return best;
}

/* Cells of the lobby (and other open regions) the route start can reach. */
static void compute_reach(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, head = 0, tail = 0;
    memset(G->reach, 0, (size_t)N);
    G->heap[tail++] = L->routeStart;
    G->reach[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b), o = L->own[n];
            if (G->reach[n] || (o != NONE && interior_kind(L->reg[o].type) != IK_OPEN)) continue;
            G->reach[n] = 1;
            G->heap[tail++] = n;
        }
    }
}

/* Longest run of reachable, walkable cells just inside the inner face over [a, a+w). */
static int face_run(const Gen *G, int side, int outer, int a, int w, int depth) {
    const ILayout *L = G->L;
    int run = 0, best = 0;
    for (int k = a; k < a + w; ++k) {
        int i, j;
        band_cell(side, outer, k, depth, &i, &j);
        int ok = usable(L, i, j) && G->reach[IDX(L, i, j)] && !(L->flag[IDX(L, i, j)] & IF_COLUMN);
        if (ok) { int o = L->own[IDX(L, i, j)]; ok = o == NONE || L->reg[o].type != IR_VESTIBULE; }
        run = ok ? run + 1 : 0;
        best = run > best ? run : best;
    }
    return best;
}

/* Scores a band candidate, or returns -1e30 if it breaks a rule (step 5.3). */
static float band_score(Gen *G, const RoomSpec *sp, int side, int a, int w, int depth, const SDoor *anchor, int bandRank, int *outerOut) {
    ILayout *L = G->L;
    int n = band_len(L, side);
    if (a < 0 || a + w > n) return -1e30f;
    int hiSide = side == 0 || side == 2;
    int outer = hiSide ? -1 : 1 << 20, inner = hiSide ? 1 << 20 : -1;
    for (int k = a; k < a + w; ++k) {
        int f = band_facade(L, side, k);
        if (f < 0) return -1e30f;
        if (hiSide) { outer = f > outer ? f : outer; inner = f < inner ? f : inner; }
        else        { outer = f < outer ? f : outer; inner = f > inner ? f : inner; }
    }
    if ((float)abs(outer - inner) > 0.6f * (float)depth) return -1e30f;   /* the facade varies too much */
    int used = 0;
    for (int k = a; k < a + w; ++k)
        for (int d = 0; d < depth; ++d) {
            int i, j;
            band_cell(side, outer, k, d, &i, &j);
            if (!in_grid(L, i, j)) return -1e30f;
            if (!usable(L, i, j)) continue;
            int c = IDX(L, i, j);
            if (L->own[c] != NONE) return -1e30f;
            if (L->flag[c] & (IF_RESERVE | IF_VOID | IF_APPROACH | IF_DOORCLEAR | IF_ENTRY)) return -1e30f;
            if (!anchor && (L->flag[c] & IF_FRONT)) return -1e30f;
            used++;
        }
    if ((float)used < 0.7f * (float)(w * depth)) return -1e30f;                 /* compactness */
    if ((float)used * CELL * CELL < sp->minArea || (float)w * CELL < sp->minDim - 0.01f) return -1e30f;
    /* a straight run on the inner face for a door */
    int need = sp->type == IR_PLANT ? 3 : sp->type == IR_LOADING ? 4 : 2;
    if (face_run(G, side, outer, a, w, depth) < need && !anchor) return -1e30f;
    if (anchor) {
        if (anchor->side != (side == 0 ? 2 : side == 1 ? 3 : side == 2 ? 1 : 0)) return -1e30f;
        float da = (side == 0 || side == 3) ? anchor->x - L->gx0 : anchor->z - L->gz0;
        int d0 = (int)floorf((da - anchor->w * 0.5f) / CELL), d1 = (int)ceilf((da + anchor->w * 0.5f) / CELL);
        if (d0 < a || d1 > a + w) return -1e30f;
        for (int k = d0; k < d1; ++k) if (abs(band_facade(L, side, k) - outer) > 1) return -1e30f;
    }
    /* score by placement preference */
    int ci0, cj0, ci1, cj1;
    band_cell(side, outer, a, 0, &ci0, &cj0);
    band_cell(side, outer, a + w - 1, depth - 1, &ci1, &cj1);
    float cx = (ccx(L, ci0) + ccx(L, ci1)) * 0.5f, cz = (ccz(L, cj0) + ccz(L, cj1)) * 0.5f;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    float de = sqrtf((cx - ex) * (cx - ex) + (cz - ez) * (cz - ez)), s = 0.0f;
    switch (sp->pref) {
    case PREF_HALL:     s = hall_dist(L, cx, cz); break;
    case PREF_ENTRANCE: s = de; break;
    case PREF_REAR:     s = G->maxz - cz; break;
    case PREF_FAR:      s = -0.5f * (hall_dist(L, cx, cz) + de); break;
    default:            s = 0.0f; break;
    }
    /* abutting rooms, cores or the facade at the ends; no narrow alcoves */
    for (int end = 0; end < 2; ++end) {
        int dirA = end ? 1 : -1, k0 = end ? a + w : a - 1, gap = 0, hit = 0;
        for (int t = 0; t < 7; ++t) {
            int i, j;
            band_cell(side, outer, k0 + dirA * t, depth / 2, &i, &j);
            int r = region_at(L, i, j);
            if (!usable(L, i, j) || (r >= 0 && interior_kind(L->reg[r].type) != IK_OPEN)) { hit = 1; break; }
            gap++;
        }
        if (hit && gap == 0) s -= 3.0f;
        else if (hit && gap < 6) s += 6.0f;
    }
    s += (float)bandRank + rng_float(&G->rr);
    *outerOut = outer;
    return s;
}

static int claim_band(Gen *G, int type, const BandCand *bc) {
    ILayout *L = G->L;
    int pieces[3][2], np = 0;
    int wcw = bc->w >= 18 ? 4 : 0, hw = (bc->w - wcw) / 2;
    int splits = type == IR_TOILET && bc->w >= 14 && face_run(G, bc->side, bc->outer, bc->a, hw, bc->depth) >= 2 &&
                 face_run(G, bc->side, bc->outer, bc->a + hw + wcw, bc->w - hw - wcw, bc->depth) >= 2;
    if (splits && wcw && face_run(G, bc->side, bc->outer, bc->a + hw, wcw, bc->depth) < 2) {
        wcw = 0; hw = bc->w / 2;
        splits = face_run(G, bc->side, bc->outer, bc->a, hw, bc->depth) >= 2 && face_run(G, bc->side, bc->outer, bc->a + hw, bc->w - hw, bc->depth) >= 2;
    }
    if (splits) {
        int wc = wcw, half = hw;
        pieces[np][0] = bc->a; pieces[np][1] = half; np++;
        if (wc) { pieces[np][0] = bc->a + half; pieces[np][1] = wc; np++; }
        pieces[np][0] = bc->a + half + wc; pieces[np][1] = bc->w - half - wc; np++;
        alog(G, "  info: toilets split into A and B%s\n", wc ? " with an accessible WC" : "");
    } else {
        pieces[0][0] = bc->a; pieces[0][1] = bc->w; np = 1;
    }
    for (int p = 0; p < np; ++p) {
        int t = (np == 3 && p == 1) ? IR_WC : type;
        int r = add_region(G, t, 0);
        if (r < 0) return 0;
        for (int k = pieces[p][0]; k < pieces[p][0] + pieces[p][1]; ++k)
            for (int d = 0; d < bc->depth; ++d) {
                int i, j;
                band_cell(bc->side, bc->outer, k, d, &i, &j);
                if (usable(L, i, j)) L->own[IDX(L, i, j)] = (uint8_t)r;
            }
    }
    /* door clearance in front of the inner face */
    for (int k = bc->a; k < bc->a + bc->w; ++k)
        for (int d = bc->depth; d < bc->depth + 3; ++d) {
            int i, j;
            band_cell(bc->side, bc->outer, k, d, &i, &j);
            if (usable(L, i, j) && L->own[IDX(L, i, j)] == NONE) L->flag[IDX(L, i, j)] |= IF_DOORCLEAR;
        }
    return 1;
}

/* Free lobby cells the route start cannot reach when `claimed` cells are taken. */
static int stranded(Gen *G, const uint8_t *claimed) {
    ILayout *L = G->L;
    int N = L->nx * L->nz, head = 0, tail = 0, reached = 0, total = 0;
    for (int c = 0; c < N; ++c) G->prev[c] = 0;
#define OPENCELL(n) (L->occ[n] == IO_USABLE && !claimed[n] && (L->own[n] == NONE || interior_kind(L->reg[L->own[n]].type) == IK_OPEN))
    G->heap[tail++] = L->routeStart;
    G->prev[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        if (L->own[c] == NONE) reached++;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->prev[n] || !OPENCELL(n)) continue;
            G->prev[n] = 1;
            G->heap[tail++] = n;
        }
    }
    for (int c = 0; c < N; ++c) total += L->occ[c] == IO_USABLE && !claimed[c] && L->own[c] == NONE;
#undef OPENCELL
    return total - reached;
}

static int cmp_band(const void *a, const void *b) {
    const BandCand *x = (const BandCand *)a, *y = (const BandCand *)b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    if (x->side != y->side) return x->side - y->side;
    if (x->w != y->w) return x->w - y->w;
    return x->a - y->a;
}

int place_room(Gen *G, int type, const SDoor *anchor) {
    ILayout *L = G->L;
    const RoomSpec *sp = room_spec(type);
    compute_reach(G);
    float pw = rng_range(&G->rr, sp->minW, sp->maxW);
    int widths[3] = { cells_of(pw), cells_of(sp->minW), cells_of(sp->maxW) };
    int depth = type == IR_ESCAPE ? 6 : type == IR_LOADING ? 14 : cells_of(G->bandDepth);
    int cap = 256, nc = 0;
    BandCand *cand = (BandCand *)malloc(sizeof(BandCand) * (size_t)cap);
    if (!cand) fatal("out of memory planning an interior");
    for (int bi = 0; bi < 4; ++bi) {
        int side = bi < 3 ? G->bandOrder[bi] : 3;
        if (side == 3 && !anchor) continue;
        for (int wi = 0; wi < 3; ++wi) {
            int w = widths[wi];
            if (wi && w == widths[0]) continue;
            for (int a = 0; a + w <= band_len(L, side); ++a) {
                int outer;
                float s = band_score(G, sp, side, a, w, depth, anchor, bi, &outer);
                if (s <= -1e29f) continue;
                if (nc == cap) { cap *= 2; cand = (BandCand *)realloc(cand, sizeof(BandCand) * (size_t)cap); if (!cand) fatal("out of memory planning an interior"); }
                BandCand *bc = &cand[nc++];
                bc->side = side; bc->a = a; bc->w = w; bc->depth = depth; bc->outer = outer; bc->score = s;
            }
        }
    }
    qsort(cand, (size_t)nc, sizeof(BandCand), cmp_band);
    /* the best position that strands no part of the lobby */
    int N = L->nx * L->nz, base = -1, pick = -1;
    for (int k = 0; k < nc && k < 16 && pick < 0; ++k) {
        if (base < 0) { memset(G->mark, 0, (size_t)N); base = stranded(G, G->mark); }
        memset(G->mark, 0, (size_t)N);
        const BandCand *bc = &cand[k];
        for (int q = bc->a; q < bc->a + bc->w; ++q)
            for (int d = 0; d < bc->depth; ++d) {
                int i, j;
                band_cell(bc->side, bc->outer, q, d, &i, &j);
                if (usable(L, i, j)) G->mark[IDX(L, i, j)] = 1;
            }
        if (stranded(G, G->mark) <= base) pick = k;
    }
    if (pick < 0) {
        free(cand);
        alog(G, "  simplify: %s dropped: no position in a band%s\n", REG_NAMES[type], nc ? " that keeps the lobby whole" : "");
        return 0;
    }
    BandCand best = cand[pick];
    free(cand);
    static const char *BANDS[] = { "rear", "left", "right", "front" };
    alog(G, "  info: %s %.1f x %.1f m in the %s band%s\n", REG_NAMES[type], best.w * CELL, best.depth * CELL, BANDS[best.side],
         anchor ? ", round its service door" : "");
    return claim_band(G, type, &best);
}

/* Pockets cut off from the lobby join the room they share most boundary with. */
void absorb_pockets(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) G->mark[c] = 0;
    int head = 0, tail = 0;
    G->heap[tail++] = L->routeStart;
    G->mark[L->routeStart] = 1;
    while (head < tail) {
        int c = G->heap[head++], i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!usable(L, a, b)) continue;
            int n = IDX(L, a, b), o = L->own[n];
            if (G->mark[n] || (o != NONE && interior_kind(L->reg[o].type) != IK_OPEN)) continue;
            G->mark[n] = 1;
            G->heap[tail++] = n;
        }
    }
    int counts[IN_MAX_REG];
    for (int c = 0; c < N; ++c) {
        if (G->mark[c] || L->occ[c] != IO_USABLE || L->own[c] != NONE) continue;
        /* flood this pocket */
        head = tail = 0;
        G->heap[tail++] = c;
        G->mark[c] = 2;
        memset(counts, 0, sizeof(counts));
        while (head < tail) {
            int q = G->heap[head++], i = q % L->nx, j = q / L->nx;
            for (int d = 0; d < 4; ++d) {
                int a = i + DX[d], b = j + DZ[d];
                if (!usable(L, a, b)) continue;
                int n = IDX(L, a, b), o = L->own[n];
                if (o != NONE) { if (interior_kind(L->reg[o].type) == IK_ROOM) counts[o] += 4; else counts[o] += 1; continue; }
                if (G->mark[n]) continue;
                G->mark[n] = 2;
                G->heap[tail++] = n;
            }
        }
        int best = -1;
        for (int r = 0; r < L->nreg; ++r) if (counts[r] && interior_kind(L->reg[r].type) != IK_OPEN && (best < 0 || counts[r] > counts[best])) best = r;
        if (best < 0) continue;
        for (int k = 0; k < tail; ++k) if (!(L->flag[G->heap[k]] & IF_VOID)) L->own[G->heap[k]] = (uint8_t)best;
        alog(G, "  info: pocket of %.1f m2 joins the %s\n", tail * CELL * CELL, REG_NAMES[L->reg[best].type]);
    }
}

/* Slivers narrower than 1.5 m that touch a room join it (morphological opening). */
void absorb_slivers(Gen *G) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    const uint16_t keep = IF_RESERVE | IF_APPROACH | IF_DOORCLEAR | IF_ENTRY | IF_VOID;
#define FREE(i, j) (usable(L, (i), (j)) && L->own[IDX(L, (i), (j))] == NONE)
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx, er = FREE(i, j);
        for (int b = -1; b <= 1 && er; ++b) for (int a = -1; a <= 1 && er; ++a) er = FREE(i + a, j + b);
        G->mark[c] = (uint8_t)er;
    }
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx, op = 0;
        for (int b = -1; b <= 1 && !op; ++b) for (int a = -1; a <= 1 && !op; ++a) op = in_grid(L, i + a, j + b) && (G->mark[IDX(L, i + a, j + b)] & 1);
        if (op) G->mark[c] |= 2;
    }
    int joined = 0, changed = 1;
    while (changed) {
        changed = 0;
        for (int c = 0; c < N; ++c) {
            int i = c % L->nx, j = c / L->nx;
            if (!FREE(i, j) || (G->mark[c] & 2) || (L->flag[c] & keep)) continue;
            for (int d = 0; d < 4; ++d) {
                int r = region_at(L, i + DX[d], j + DZ[d]);
                if (r >= 0 && interior_kind(L->reg[r].type) == IK_ROOM) { L->own[c] = (uint8_t)r; joined++; changed = 1; break; }
            }
        }
    }
#undef FREE
    if (joined) alog(G, "  info: %.1f m2 of slivers absorbed into rooms\n", joined * CELL * CELL);
}
