#include "interior_internal.h"

/* ===========================================================================
 * Step 3: anchors
 * ========================================================================= */

void in_place_columns(Gen *G) {
    ILayout *L = G->L;
    if (!G->columns) return;
    float sp = G->colSpacing, W = G->maxx - G->minx, D = G->maxz - G->minz;
    int na = (int)floorf(W / sp), nb = (int)floorf(D / sp);
    float ox = G->minx + (W - (float)na * sp) * 0.5f, oz = G->minz + (D - (float)nb * sp) * 0.5f;
    for (int b = 0; b <= nb; ++b)
        for (int a = 0; a <= na; ++a) {
            float x = ox + (float)a * sp, z = oz + (float)b * sp;
            if (!bfoot_contains(&L->foot, x, z) || bfoot_boundary_dist(&L->foot, x, z, NULL, NULL) < 2.6f) continue;
            int c = cell_at(L, x, z);
            if (c < 0 || L->occ[c] != IO_USABLE || (L->flag[c] & (IF_EDGE | IF_ENTRY)) || L->ncol >= IN_MAX_COL) continue;
            L->flag[c] |= IF_COLUMN;
            L->col[L->ncol][0] = (int16_t)(c % L->nx);
            L->col[L->ncol][1] = (int16_t)(c / L->nx);
            L->ncol++;
        }
    alog(G, "  info: column grid: %d columns at %.1f m\n", L->ncol, sp);
}

/* Core templates: rectangles in template cells (u, v); the hall opens along -v
 * (single bank) or -u (facing banks). Appendix A sizes: lift 2.5 x 2.5 m,
 * stair 5.5 x 3.5 m, risers 1 m deep, hall 3.5-4 m. */
typedef struct Part { uint8_t type; int16_t u0, v0, u1, v1; } Part;
typedef struct Tmpl { int tw, td, np, mouth; Part p[24]; int16_t m[4]; } Tmpl;

static void tpart(Tmpl *t, int type, int u0, int v0, int u1, int v1) {
    if (t->np >= 24 || u1 <= u0 || v1 <= v0) return;
    Part *p = &t->p[t->np++];
    p->type = (uint8_t)type; p->u0 = (int16_t)u0; p->v0 = (int16_t)v0; p->u1 = (int16_t)u1; p->v1 = (int16_t)v1;
}

static void tmpl_single(Tmpl *t, int L, int S, int side, int hallD, int mouthD) {
    memset(t, 0, sizeof(*t));
    int left = S == 2 || (S == 1 && side == 0), right = S == 2 || (S == 1 && side == 1);
    int off = left ? 11 : 0;
    t->tw = L * 5 + S * 11; t->td = hallD + 7; t->mouth = 0;
    tpart(t, IR_HALL, 0, 0, t->tw, hallD);
    for (int i = 0; i < L; ++i) tpart(t, IR_LIFT, off + 5 * i, hallD, off + 5 * i + 5, hallD + 5);
    tpart(t, IR_RISER, off, hallD + 5, off + L * 5, hallD + 7);
    if (left) tpart(t, IR_STAIR, 0, hallD, 11, hallD + 7);
    if (right) tpart(t, IR_STAIR, off + L * 5, hallD, off + L * 5 + 11, hallD + 7);
    t->m[0] = 0; t->m[1] = (int16_t)-mouthD; t->m[2] = (int16_t)t->tw; t->m[3] = 0;
}

static void tmpl_facing(Tmpl *t, int L, int hallW, int mouthD) {
    memset(t, 0, sizeof(*t));
    int nA = (L + 1) / 2, nB = L - nA;
    t->td = 14 + hallW; t->tw = nA * 5 + 7; t->mouth = 3;
    tpart(t, IR_HALL, 0, 7, nA * 5, 7 + hallW);
    tpart(t, IR_RISER, 0, 0, nA * 5, 2);
    for (int i = 0; i < nA; ++i) tpart(t, IR_LIFT, 5 * i, 2, 5 * i + 5, 7);
    for (int i = 0; i < nB; ++i) tpart(t, IR_LIFT, 5 * i, 7 + hallW, 5 * i + 5, 12 + hallW);
    tpart(t, IR_RISER, nB * 5, 7 + hallW, nA * 5, 12 + hallW);
    tpart(t, IR_RISER, 0, 12 + hallW, nA * 5, 14 + hallW);
    tpart(t, IR_STAIR, nA * 5, 0, t->tw, t->td / 2);
    tpart(t, IR_STAIR, nA * 5, t->td / 2, t->tw, t->td);
    t->m[0] = (int16_t)-mouthD; t->m[1] = 7; t->m[2] = 0; t->m[3] = (int16_t)(7 + hallW);
}

/* Template rectangle turned by o quarter turns, at grid position (x, z). */
static void rot_rect(const Tmpl *t, int o, int x, int z, int u0, int v0, int u1, int v1, int *i0, int *j0, int *i1, int *j1) {
    int TW = t->tw, TD = t->td;
    switch (o & 3) {
    case 0:  *i0 = u0;      *i1 = u1;      *j0 = v0;      *j1 = v1;      break;
    case 1:  *i0 = v0;      *i1 = v1;      *j0 = TW - u1; *j1 = TW - u0; break;
    case 2:  *i0 = TW - u1; *i1 = TW - u0; *j0 = TD - v1; *j1 = TD - v0; break;
    default: *i0 = TD - v1; *i1 = TD - v0; *j0 = u0;      *j1 = u1;      break;
    }
    *i0 += x; *i1 += x; *j0 += z; *j1 += z;
}

static int rot_dir(int o, int d) { return (d + 4 - (o & 3)) & 3; }

/* Can the entrance reach the mouth strip through a corridor at least 1.5 m
 * wide, with the candidate core in place? (step 3.3) */
static int core_reachable(Gen *G, int ci0, int cj0, int ci1, int cj1, int mi0, int mj0, int mi1, int mj1) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) {
        int i = c % L->nx, j = c / L->nx;
        int inCore = i >= ci0 && i < ci1 && j >= cj0 && j < cj1;
        G->mark[c] = (uint8_t)(L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & IF_COLUMN) || inCore);
    }
    dist_field(G);
    int ei = G->entryCell % L->nx, ej = G->entryCell / L->nx;
    for (int c = 0; c < N; ++c) G->prev[c] = 0;
    int head = 0, tail = 0;
    G->heap[tail++] = G->entryCell;
    G->prev[G->entryCell] = 1;
    while (head < tail) {
        int c = G->heap[head++];
        int i = c % L->nx, j = c / L->nx;
        if (i >= mi0 && i < mi1 && j >= mj0 && j < mj1) return 1;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->prev[n] || G->mark[n]) continue;
            int nearE = abs(a - ei) <= 6 && abs(b - ej) <= 6;
            if (G->dist[n] < (nearE ? 1.0f : 2.0f)) continue;
            G->prev[n] = 1;
            G->heap[tail++] = n;
        }
    }
    return 0;
}

static int cmp_cand(const void *a, const void *b) {
    const Cand *x = (const Cand *)a, *y = (const Cand *)b;
    if (x->score < y->score) return -1;
    if (x->score > y->score) return 1;
    if (x->o != y->o) return x->o - y->o;
    if (x->z != y->z) return x->z - y->z;
    return x->x - y->x;
}

static void push_cand(Gen *G, float score, int x, int z, int o) {
    if (G->ncand == G->capcand) {
        G->capcand = G->capcand ? G->capcand * 2 : 4096;
        G->cand = (Cand *)realloc(G->cand, sizeof(Cand) * (size_t)G->capcand);
        if (!G->cand) fatal("out of memory planning an interior");
    }
    Cand *c = &G->cand[G->ncand++];
    c->score = score; c->x = x; c->z = z; c->o = o;
}

int place_core(Gen *G, int ci) {
    ILayout *L = G->L;
    float ex = ccx(L, G->entryCell % L->nx), ez = ccz(L, G->entryCell / L->nx);
    /* where the core wants to be: under its tower, else the default rule */
    float tx, tz;
    int hasTower = ci < G->ntower;
    if (hasTower) {
        const BLoop *tl = &G->tower[ci].outer;
        tx = tz = 0.0f;
        for (int i = 0; i < tl->n; ++i) { tx += tl->x[i]; tz += tl->z[i]; }
        tx /= (float)tl->n; tz /= (float)tl->n;
        for (int c = 0; c < L->nx * L->nz; ++c) G->mark[c] = (uint8_t)bfoot_contains(&G->tower[ci], ccx(L, c % L->nx), ccz(L, c / L->nx));
    } else {
        float cxm = (G->minx + G->maxx) * 0.5f, czm = (G->minz + G->maxz) * 0.5f;
        if (G->coreRule == 0)      { tx = cxm; tz = G->maxz - 6.0f; }
        else if (G->coreRule == 1) { tx = G->coreSide ? G->maxx - 8.0f : G->minx + 8.0f; tz = G->maxz - 6.0f; }
        else                       { tx = cxm; tz = czm; }
        if (ci == 1) tx = 2.0f * cxm - tx;   /* a second core balances the first */
    }
    static const char *RULE[] = { "rear-centre", "rear-side", "centre" };
    for (int step = 0; step < 2 * G->lifts + 1; ++step) {
        /* fewer lifts each time, and a compact hall (3 m deep, 2 m mouth) before giving up a lift;
         * last, a tiny core whose lift and stair open almost straight onto the lobby */
        int tiny = step == 2 * G->lifts;
        int lifts = tiny ? 1 : G->lifts - step / 2, compact = tiny ? 2 : step & 1;
        Tmpl t;
        int facing = G->facing && lifts >= 4;
        int hallD = compact == 2 ? 3 : compact ? 6 : G->hallD, mouthD = compact == 2 ? 2 : compact ? 4 : 6;
        if (facing) tmpl_facing(&t, lifts, compact ? 7 : G->hallD, mouthD);
        else tmpl_single(&t, lifts, lifts >= 3 && G->stairs >= 2 && !compact ? 2 : 1, G->stairSide, hallD, mouthD);
        G->ncand = 0;
        /* prefix sums: free for a solid core, free for its mouth, outside its tower, in the frontage */
        sat_build(G, Q_SOLID, G->sat[0]);
        sat_build(G, Q_MOUTH, G->sat[1]);
        if (hasTower) sat_build(G, Q_MARKED, G->sat[2]);
        sat_build(G, Q_FRONT, G->sat[3]);
        for (int o = 0; o < 4; ++o) {
            int W = (o & 1) ? t.td : t.tw, D = (o & 1) ? t.tw : t.td;
            int md = rot_dir(o, t.mouth);
            float area = (float)(W * D);
            for (int z = 0; z + D <= L->nz; ++z)
                for (int x = 0; x + W <= L->nx; ++x) {
                    if (sat_count(L, G->sat[0], x, z, x + W, z + D)) continue;
                    int mi0, mj0, mi1, mj1;
                    rot_rect(&t, o, x, z, t.m[0], t.m[1], t.m[2], t.m[3], &mi0, &mj0, &mi1, &mj1);
                    if (sat_count(L, G->sat[1], mi0, mj0, mi1, mj1)) continue;
                    float cx = L->gx0 + ((float)x + (float)W * 0.5f) * CELL, cz = L->gz0 + ((float)z + (float)D * 0.5f) * CELL;
                    float mx = L->gx0 + (float)(mi0 + mi1) * 0.5f * CELL, mz = L->gz0 + (float)(mj0 + mj1) * 0.5f * CELL;
                    /* distance from the tower centre or the default rule's position */
                    float s = sqrtf((cx - tx) * (cx - tx) + (cz - tz) * (cz - tz));
                    /* share of the core outside the tower footprint */
                    if (hasTower) s += 40.0f * (float)sat_count(L, G->sat[2], x, z, x + W, z + D) / area;
                    /* the hall mouth facing the entrance */
                    float dx = ex - mx, dz = ez - mz, dl = sqrtf(dx * dx + dz * dz) + 1e-3f;
                    s -= 6.0f * ((float)DX[md] * dx + (float)DZ[md] * dz) / dl;
                    /* free space in front of the mouth */
                    int fi0 = mi0 + DX[md] * 6, fj0 = mj0 + DZ[md] * 6, fi1 = mi1 + DX[md] * 6, fj1 = mj1 + DZ[md] * 6;
                    int fb = sat_count(L, G->sat[1], fi0, fj0, fi1, fj1);
                    s -= 4.0f * (fb >= (1 << 20) ? 0.0f : 1.0f - (float)fb / (float)((fi1 - fi0) * (fj1 - fj0)));
                    /* intrusion into the frontage */
                    s += 20.0f * (float)sat_count(L, G->sat[3], x, z, x + W, z + D) / area;
                    /* the hall's distance from the entrance: 9 m or more preferred */
                    if (dl < 9.0f) s += (9.0f - dl) * 2.5f;
                    s += dl * 0.05f + rng_float(&G->rc) * 0.5f;
                    push_cand(G, s, x, z, o);
                }
        }
        qsort(G->cand, (size_t)G->ncand, sizeof(Cand), cmp_cand);
        for (int k = 0; k < G->ncand && k < 40; ++k) {
            const Cand *c = &G->cand[k];
            int W = (c->o & 1) ? t.td : t.tw, D = (c->o & 1) ? t.tw : t.td;
            int mi0, mj0, mi1, mj1;
            rot_rect(&t, c->o, c->x, c->z, t.m[0], t.m[1], t.m[2], t.m[3], &mi0, &mj0, &mi1, &mj1);
            if (!core_reachable(G, c->x, c->z, c->x + W, c->z + D, mi0, mj0, mi1, mj1)) continue;
            /* accept: claim the parts */
            ICore *K = &L->core[L->ncore++];
            memset(K, 0, sizeof(*K));
            K->lifts = (uint8_t)lifts; K->facing = (uint8_t)facing; K->mouth = (uint8_t)rot_dir(c->o, t.mouth);
            K->x0 = (int16_t)c->x; K->z0 = (int16_t)c->z; K->x1 = (int16_t)(c->x + W); K->z1 = (int16_t)(c->z + D);
            K->hall = K->secure = -1;
            char by[24];
            snprintf(by, sizeof(by), "core %d", ci);
            absorb_columns(G, c->x, c->z, c->x + W, c->z + D, by);
            for (int p = 0; p < t.np; ++p) {
                int i0, j0, i1, j1;
                rot_rect(&t, c->o, c->x, c->z, t.p[p].u0, t.p[p].v0, t.p[p].u1, t.p[p].v1, &i0, &j0, &i1, &j1);
                int r = add_region(G, t.p[p].type, ci);
                if (r < 0) return fail(G, "too many regions");
                claim_rect(G, r, i0, j0, i1, j1);
                if (t.p[p].type == IR_HALL) {
                    K->hall = (int16_t)r;
                    for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) L->flag[IDX(L, i, j)] |= IF_APPROACH;
                }
                if (t.p[p].type == IR_STAIR) K->stairs++;
            }
            snprintf(by, sizeof(by), "lift hall %d", ci);
            absorb_columns(G, mi0, mj0, mi1, mj1, by);
            for (int j = mj0; j < mj1; ++j)
                for (int i = mi0; i < mi1; ++i) if (in_grid(L, i, j)) L->flag[IDX(L, i, j)] |= IF_APPROACH;
            /* lift and stair doors onto the hall */
            for (int r = 0; r < L->nreg; ++r) {
                if (L->reg[r].core != ci || (L->reg[r].type != IR_LIFT && L->reg[r].type != IR_STAIR)) continue;
                Run run = best_run(L, r, K->hall);
                if (run.len < 2) continue;
                int w = 2;
                add_door(G, L->reg[r].type == IR_LIFT ? ID_LIFT : ID_STAIR, run.axis, run.line, run.at + (run.len - w) / 2, w, K->hall, r);
            }
            static const char *DIRS[] = { "front", "right", "rear", "left" };
            alog(G, "  info: core %d: %d lift%s%s, %d stair%s at (%.1f, %.1f), hall opening to the %s, %s\n", ci, lifts,
                 lifts == 1 ? "" : "s", facing ? " in facing banks" : " in one bank", K->stairs, K->stairs == 1 ? "" : "s",
                 L->gx0 + (c->x + W * 0.5f) * CELL, L->gz0 + (c->z + D * 0.5f) * CELL, DIRS[K->mouth],
                 hasTower ? "under its tower" : RULE[G->coreRule]);
            return 1;
        }
        if (tiny) break;
        if (!compact) alog(G, "  simplify: core %d does not fit with %d lifts; trying a compact hall\n", ci, lifts);
        else if (lifts > 1) alog(G, "  simplify: core %d does not fit with %d lifts; trying %d\n", ci, lifts, lifts - 1);
        else alog(G, "  simplify: core %d does not fit; trying a tiny core\n", ci);
    }
    return fail(G, "no reachable position for core %d", ci);
}

int place_vestibule(Gen *G) {
    ILayout *L = G->L;
    const SDoor *e = &G->entry;
    int start = G->entryCell;
    if (G->vestibule && e->side == 0) {
        float W = e->w + 2.0f;
        int wv = cells_of(W), dv = cells_of(rng_range(&G->rp, 2.5f, 3.2f));
        int i0 = (int)floorf((e->x - W * 0.5f - L->gx0) / CELL + 0.5f), j0 = (int)floorf((e->z - L->gz0) / CELL + 0.5f);
        int ok = i0 >= 0 && j0 >= 0 && i0 + wv <= L->nx && j0 + dv < L->nz;
        for (int j = j0; ok && j < j0 + dv; ++j)
            for (int i = i0; ok && i < i0 + wv; ++i) {
                int c = IDX(L, i, j);
                if (L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & IF_VOID)) ok = 0;
                if (j > j0 && (L->flag[c] & IF_EDGE)) ok = 0;
            }
        int din = cells_of(e->w);
        if (din > wv - 2) din = wv - 2;
        int di = i0 + (wv - din) / 2;
        int sc = ok ? IDX(L, di + din / 2, j0 + dv) : -1;
        if (ok && (L->occ[sc] != IO_USABLE || L->own[sc] != NONE || (L->flag[sc] & IF_EDGE))) ok = 0;
        if (ok) {
            absorb_columns(G, i0, j0, i0 + wv, j0 + dv, "the vestibule");
            int r = add_region(G, IR_VESTIBULE, 0);
            claim_rect(G, r, i0, j0, i0 + wv, j0 + dv);
            int ed = cells_of(e->w);
            add_door(G, ID_ENTRANCE, 0, j0, (int)floorf((e->x - e->w * 0.5f - L->gx0) / CELL + 0.5f), ed, r, -1);
            int d = add_door(G, ID_INNER, 0, j0 + dv, di, din, r, 0);
            if (d >= 0) mark_door_clear(G, &L->door[d], 4);
            L->routeStart = sc;
            alog(G, "  info: vestibule %.1f x %.1f m behind the entrance\n", wv * CELL, dv * CELL);
            return 1;
        }
        alog(G, "  simplify: vestibule omitted: it does not fit cleanly behind the entrance\n");
    }
    int ei = (int)floorf((e->x - e->w * 0.5f - L->gx0) / CELL + 0.5f);
    int j0 = (int)floorf((e->z - L->gz0) / CELL + 0.5f);
    if (e->side == 0) add_door(G, ID_ENTRANCE, 0, j0, ei, cells_of(e->w), 0, -1);
    L->routeStart = start;
    return 1;
}

/* Largest free rectangle (histogram method). */
static int largest_rect(Gen *G, int *bx0, int *bz0, int *bx1, int *bz1) {
    ILayout *L = G->L;
    int *h = G->prev, *st = G->heap, best = 0;
    for (int i = 0; i < L->nx; ++i) h[i] = 0;
    for (int j = 0; j < L->nz; ++j) {
        for (int i = 0; i < L->nx; ++i) h[i] = G->mark[IDX(L, i, j)] ? h[i] + 1 : 0;
        int sp = 0;
        for (int i = 0; i <= L->nx; ++i) {
            int hi = i < L->nx ? h[i] : 0;
            while (sp > 0 && h[st[sp - 1]] >= hi) {
                int top = st[--sp];
                int left = sp > 0 ? st[sp - 1] + 1 : 0;
                int area = h[top] * (i - left);
                if (area > best) { best = area; *bx0 = left; *bx1 = i; *bz1 = j + 1; *bz0 = j + 1 - h[top]; }
            }
            st[sp++] = i;
        }
    }
    return best;
}

void place_void(Gen *G) {
    ILayout *L = G->L;
    IAtrium *A = &L->atrium;
    A->mode = (uint8_t)G->atrium;
    A->galleryY = L->floorY + (L->ceilY - L->floorY) * 0.5f;
    A->topY = L->ceilY;
    if (G->atrium != IA_GALLERY && G->atrium != IA_PODIUM) return;
    for (int c = 0; c < L->nx * L->nz; ++c) {
        int ok = L->occ[c] == IO_USABLE && L->own[c] == NONE && !(L->flag[c] & (IF_EDGE | IF_DOORCLEAR));
        if (ok && G->atrium == IA_PODIUM) {
            float px = ccx(L, c % L->nx), pz = ccz(L, c / L->nx), h = CELL * 0.5f;
            for (int k = 0; k < 4 && ok; ++k)
                ok = bfoot_contains(&G->above, px + ((k & 1) ? h : -h), pz + ((k & 2) ? h : -h)) &&
                     bfoot_boundary_dist(&G->above, px, pz, NULL, NULL) > 1.0f;
        }
        G->mark[c] = (uint8_t)ok;
    }
    int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    largest_rect(G, &x0, &z0, &x1, &z1);
    int W = x1 - x0, D = z1 - z0;
    if (W < 12 || D < 12) {
        alog(G, "  simplify: no room for a 6 x 6 m atrium void; %s\n", G->g->groundMode == GF_DOUBLE ? "plain double height" : "single storey");
        A->mode = G->g->groundMode == GF_DOUBLE ? IA_DOUBLE : IA_NONE;
        return;
    }
    float target = G->voidShare * L->area / (CELL * CELL);
    float k = sqrtf(target / (float)(W * D));
    if (k < 1.0f) {
        int w = (int)clampf(floorf((float)W * k + 0.5f), 12.0f, (float)W), d = (int)clampf(floorf((float)D * k + 0.5f), 12.0f, (float)D);
        int ec = (int)floorf((G->entry.x - L->gx0) / CELL);
        int nx0 = (int)clampf((float)(ec - w / 2), (float)x0, (float)(x1 - w));
        x0 = nx0; x1 = nx0 + w; z1 = z0 + d;                    /* keep the front edge */
    }
    A->x0 = (int16_t)x0; A->z0 = (int16_t)z0; A->x1 = (int16_t)x1; A->z1 = (int16_t)z1;
    for (int j = z0; j < z1; ++j) for (int i = x0; i < x1; ++i) L->flag[IDX(L, i, j)] |= IF_VOID;
    absorb_columns(G, x0, z0, x1, z1, "the atrium void");
    if (G->atrium == IA_PODIUM) {
        A->levels = (uint8_t)G->levels;
        A->topY = L->ceilY + (float)G->levels * G->g->floorH;
    }
    alog(G, "  info: atrium: %s, void %.1f x %.1f m (%.0f%% of the floor)%s\n", ATRIUM_NAMES[A->mode], (x1 - x0) * CELL, (z1 - z0) * CELL,
         100.0f * (float)((x1 - x0) * (z1 - z0)) * CELL * CELL / L->area, G->atrium == IA_PODIUM ? ", through the podium" : "");
}

/* Route walkability for core ci: the lobby, the void floor and its own hall. */
static int route_walkable(const Gen *G, int c, int ci) {
    const ILayout *L = G->L;
    if (L->occ[c] != IO_USABLE || (L->flag[c] & IF_COLUMN)) return 0;
    int o = L->own[c];
    if (o == NONE) return 1;
    return o == L->core[ci].hall || (L->reg[o].type == IR_SECURE && L->reg[o].core == ci);
}

int plan_route(Gen *G, int ci) {
    ILayout *L = G->L;
    int N = L->nx * L->nz;
    for (int c = 0; c < N; ++c) G->mark[c] = (uint8_t)!route_walkable(G, c, ci);
    dist_field(G);
    int s = L->routeStart, si = s % L->nx, sj = s / L->nx;
    for (int c = 0; c < N; ++c) { G->cost[c] = 1e30f; G->prev[c] = -1; }
    G->nheap = 0;
    G->cost[s] = 0.0f;
    heap_push(G, 0.0f, s);
    int goal = -1, hall = L->core[ci].hall;
    while (G->nheap > 0) {
        float k;
        int c = heap_pop(G, &k);
        if (k > G->cost[c]) continue;
        if (L->own[c] == hall) { goal = c; break; }
        int i = c % L->nx, j = c / L->nx;
        for (int d = 0; d < 8; ++d) {
            static const int ox[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, oz[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
            int a = i + ox[d], b = j + oz[d];
            if (!in_grid(L, a, b)) continue;
            int n = IDX(L, a, b);
            if (G->mark[n]) continue;
            if (d >= 4 && (G->mark[IDX(L, a, j)] || G->mark[IDX(L, i, b)])) continue;
            int near = abs(a - si) <= 6 && abs(b - sj) <= 6;
            float w = clear_width(G->dist[n]), pen = 0.0f;
            if (L->own[n] != hall) {
                if (w < 0.75f && !near) continue;
                if (w < 1.6f) pen = 12.0f;
                else if (w < 2.4f) pen = 2.0f;
            }
            float nc = G->cost[c] + (d >= 4 ? 1.41421356f : 1.0f) * (1.0f + pen);
            if (nc < G->cost[n]) { G->cost[n] = nc; G->prev[n] = c; heap_push(G, nc, n); }
        }
    }
    if (goal < 0) return fail(G, "no route from the entrance to core %d", ci);
    int n = 0;
    for (int c = goal; c >= 0; c = G->prev[c]) n++;
    G->npath[ci] = n;
    int k = n;
    for (int c = goal; c >= 0; c = G->prev[c]) G->path[ci][--k] = c;
    /* reserve a corridor about 2.5 m wide round it */
    for (int p = 0; p < n; ++p) {
        int c = G->path[ci][p], i = c % L->nx, j = c / L->nx;
        L->flag[c] |= IF_ROUTE;
        for (int b = -3; b <= 3; ++b)
            for (int a = -3; a <= 3; ++a) {
                if (a * a + b * b > 6.25f || !usable(L, i + a, j + b)) continue;
                int q = IDX(L, i + a, j + b);
                if (L->own[q] == NONE) L->flag[q] |= IF_RESERVE;
            }
    }
    float len = 0.0f;
    for (int p = 1; p < n; ++p) {
        int a = G->path[ci][p - 1], b = G->path[ci][p];
        len += (a % L->nx != b % L->nx && a / L->nx != b / L->nx) ? 1.414f * CELL : CELL;
    }
    alog(G, "  info: main route to core %d: %.1f m\n", ci, len);
    return 1;
}

/* Secure side and gate line in front of a core (step 3.7). */
void place_gates(Gen *G, int ci) {
    ILayout *L = G->L;
    ICore *K = &L->core[ci];
    const IRegion *H = &L->reg[K->hall];
    int md = K->mouth, ok = 0;
    int i0 = 0, j0 = 0, i1 = 0, j1 = 0;
    for (int wing = 2; wing >= 0 && !ok; wing -= 2) {
        if (md == 0)      { i0 = H->x0 - wing; i1 = H->x1 + wing; j0 = H->z0 - 6; j1 = H->z0; }
        else if (md == 2) { i0 = H->x0 - wing; i1 = H->x1 + wing; j0 = H->z1; j1 = H->z1 + 6; }
        else if (md == 1) { j0 = H->z0 - wing; j1 = H->z1 + wing; i0 = H->x1; i1 = H->x1 + 6; }
        else              { j0 = H->z0 - wing; j1 = H->z1 + wing; i0 = H->x0 - 6; i1 = H->x0; }
        ok = i0 >= 0 && j0 >= 0 && i1 <= L->nx && j1 <= L->nz;
        for (int j = j0; ok && j < j1; ++j)
            for (int i = i0; ok && i < i1; ++i) {
                int c = IDX(L, i, j);
                if (L->occ[c] != IO_USABLE || L->own[c] != NONE || (L->flag[c] & (IF_VOID | IF_EDGE | IF_COLUMN))) ok = 0;
            }
    }
    if (!ok) { alog(G, "  simplify: core %d: no room for a secure side; lift hall left open\n", ci); return; }
    int r = add_region(G, IR_SECURE, ci);
    if (r < 0) return;
    claim_rect(G, r, i0, j0, i1, j1);
    /* where the route first crosses from the lobby into the secure side or hall */
    const int *path = G->path[ci];
    int k = 1;
    while (k < G->npath[ci] && L->own[path[k]] == NONE) k++;
    if (k >= G->npath[ci]) { k = -1; }
    int axis = 0, line = 0, along = 0;
    if (k > 0) {
        int a = path[k - 1], b = path[k];
        int ai = a % L->nx, aj = a / L->nx, bi = b % L->nx, bj = b / L->nx;
        if (ai != bi && aj != bj) {     /* diagonal: cross orthogonally */
            if (L->own[IDX(L, bi, aj)] == NONE) { ai = bi; } else { aj = bj; }
        }
        if (ai == bi) { axis = 0; line = aj > bj ? aj : bj; along = ai; }
        else          { axis = 1; line = ai > bi ? ai : bi; along = aj; }
    }
    /* the straight barrier run holding the crossing */
    int lo = along, hi = along + 1, okRun = k > 0;
    if (okRun) {
        for (;;) {
            int p = lo - 1, ra, rb;
            if (axis == 0) { ra = region_at(L, p, line - 1); rb = region_at(L, p, line); }
            else           { ra = region_at(L, line - 1, p); rb = region_at(L, line, p); }
            int lob = (ra < 0 && usable(L, axis == 0 ? p : line - 1, axis == 0 ? line - 1 : p)) || (rb < 0 && usable(L, axis == 0 ? p : line, axis == 0 ? line : p));
            int sec = (ra == r || ra == K->hall) || (rb == r || rb == K->hall);
            if (p < 0 || !lob || !sec) break;
            lo = p;
        }
        for (;;) {
            int p = hi, ra, rb;
            if (axis == 0) { ra = region_at(L, p, line - 1); rb = region_at(L, p, line); }
            else           { ra = region_at(L, line - 1, p); rb = region_at(L, line, p); }
            int lob = (ra < 0 && usable(L, axis == 0 ? p : line - 1, axis == 0 ? line - 1 : p)) || (rb < 0 && usable(L, axis == 0 ? p : line, axis == 0 ? line : p));
            int sec = (ra == r || ra == K->hall) || (rb == r || rb == K->hall);
            if (p >= (axis == 0 ? L->nx : L->nz) || !lob || !sec) break;
            hi = p + 1;
        }
    }
    int lanes = G->lanes, gl = 0;
    for (; lanes >= 2; --lanes) { gl = cells_of((float)lanes * 0.9f + 1.2f); if (hi - lo >= gl) break; }
    if (!okRun || lanes < 2) {
        for (int c = 0; c < L->nx * L->nz; ++c) if (L->own[c] == r) L->own[c] = NONE;
        L->nreg--;
        alog(G, "  simplify: core %d: barrier line too short for two gate lanes; access control dropped\n", ci);
        return;
    }
    int at = (int)clampf((float)(along - gl / 2), (float)lo, (float)(hi - gl));
    add_door(G, ID_GATE, axis, line, at, gl, 0, r);
    K->gated = 1; K->lanes = (uint8_t)lanes; K->secure = (int16_t)r;
    G->gateAt[ci] = k;
    /* speed-gate cabinets: one each side of every lane and of the wide gate */
    float s0 = (float)at * CELL, lineM = (float)line * CELL;
    float pos[8];
    int np = 0;
    for (int l = 0; l <= lanes; ++l) pos[np++] = s0 + 0.1f + (float)l * 0.9f;
    pos[np++] = s0 + 0.1f + (float)lanes * 0.9f + 1.2f;
    for (int p = 0; p < np && L->nfix < IN_MAX_FIX; ++p) {
        IFixture *f = &L->fix[L->nfix++];
        memset(f, 0, sizeof(*f));
        f->type = IX_TURNSTILE; f->region = -1; f->y0 = 0.0f; f->y1 = 1.0f;
        if (axis == 0) { f->x0 = L->gx0 + pos[p] - 0.1f; f->x1 = f->x0 + 0.2f; f->z0 = L->gz0 + lineM - 0.7f; f->z1 = f->z0 + 1.4f; }
        else           { f->z0 = L->gz0 + pos[p] - 0.1f; f->z1 = f->z0 + 0.2f; f->x0 = L->gx0 + lineM - 0.7f; f->x1 = f->x0 + 1.4f; }
    }
    alog(G, "  info: core %d: secure side and %d gate lanes plus a wide gate\n", ci, lanes);
}
