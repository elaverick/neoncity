#include "arcology_internal.h"

/* ===========================================================================
 * Step 3: anchors, in order
 * ========================================================================= */

/* 2. The spine core: express lifts at the vertical axis, inside the top tier with a margin. */
int place_spine(Gen *G) {
    ALayout *A = G->A;
    const BLoop *tl = &A->tier[A->ntier - 1].foot.outer;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (int i = 0; i < tl->n; ++i) { x0 = minf(x0, tl->x[i]); x1 = maxf(x1, tl->x[i]); z0 = minf(z0, tl->z[i]); z1 = maxf(z1, tl->z[i]); }
    float size = minf(rng_range(&G->rs, 10.0f, 16.0f), minf(x1 - x0, z1 - z0) - 4.0f);
    size = floorf(size);
    if (size < 8.0f) return fail(G, "top tier too small for the spine core");
    for (;;) {
        A->spine = rect(A->cx - size * 0.5f, A->cz - size * 0.5f, A->cx + size * 0.5f, A->cz + size * 0.5f);
        int ok = 1;
        for (int t = 0; t < A->ntier && ok; ++t) ok = rect_inside(&A->tier[t].foot, A->spine, 2.0f);
        if (ok) break;
        size -= 1.0f;
        if (size < 8.0f) return fail(G, "no spine core fits every tier with a 2 m margin");
    }
    A->expressLifts = clampi(4 + (A->ntier - 1) / 2, 4, 8);
    A->hall = rect(A->spine.x0, A->spine.z0 - 4.0f, A->spine.x1, A->spine.z0);
    alog(G, "  info: spine core %.0f x %.0f m, %d express lifts to %d sky lobbies\n", size, size, A->expressLifts, A->ntier - 1);
    return 1;
}

/* The canyon's void rectangles for its mode at a scale (3: shrink and retry). */
static void void_rects(Gen *G, float scale) {
    ALayout *A = G->A;
    ARect s = A->spine;
    float w = s.x1 - s.x0;
    if (A->canyon == AC_SPINE) {
        A->voids[0] = expand(s, maxf(G->margin * scale, 5.0f));
        A->nvoid = 1;
    } else if (A->canyon == AC_TWIN) {
        float L = maxf(G->naveL * scale, 8.0f);
        int alongX = (G->maxx - G->minx) >= (G->maxz - G->minz);
        if (alongX) { A->voids[0] = rect(s.x0 - L, s.z0, s.x0, s.z1); A->voids[1] = rect(s.x1, s.z0, s.x1 + L, s.z1); }
        else        { A->voids[0] = rect(s.x0, s.z0 - L, s.x1, s.z0); A->voids[1] = rect(s.x0, s.z1, s.x1, s.z1 + L); }
        A->nvoid = 2;
    } else {
        float L = maxf(G->wellL * scale, 8.0f);
        A->voids[0] = rect(s.x0, s.z0 - L, s.x0 + w, s.z0);
        A->nvoid = 1;
    }
}

/* 3. The canyon: raised tier by tier while it fits inside the tier inset by the
 *    gallery depth + 1 m. Until the ladder gives up on the upper tiers (attempt
 *    5), a canyon that cannot reach the first upper tier rejects the attempt,
 *    so another mode and size are drawn; after that it becomes a plaza. */
int place_canyon(Gen *G) {
    ALayout *A = G->A;
    static const float SCALE[4] = { 1.0f, 0.8f, 0.65f, 0.5f };
    for (int shrink = 0; shrink < 4; ++shrink) {
        void_rects(G, SCALE[shrink]);
        int top = -1;
        for (int t = 0; t < A->ntier && t < G->maxTiers; ++t) {
            int ok = 1;
            for (int v = 0; v < A->nvoid && ok; ++v) ok = rect_inside(&A->tier[t].foot, expand(A->voids[v], A->galleryD), 1.0f);
            if (!ok) break;
            top = t;
        }
        int need = (G->maxTiers > 1 && A->ntier > 1) ? 1 : 0;
        if (top >= need) {
            A->topTier = top;
            A->crownY = A->tier[top].y1;
            alog(G, "  info: %s canyon rises through %d tier(s) to a light crown at %.0f m\n", CANYON_NAMES[A->canyon], top + 1, A->crownY - G0);
            return 1;
        }
        if (shrink < 3) alog(G, "  simplify: the %s canyon does not fit the first upper tier; shrinking it\n", CANYON_NAMES[A->canyon]);
    }
    if (G->attempt < 4) return fail(G, "no %s canyon fits the first upper tier", CANYON_NAMES[A->canyon]);
    A->canyon = AC_PLAZA;
    A->nvoid = 0;
    A->topTier = -1;
    A->crownY = A->ceilY;
    alog(G, "  simplify: no canyon fits; the canyon floor is a double-height plaza\n");
    return 1;
}

/* The canyon floor (the void's footprint and an apron round the spine), the spine and its express hall. */
void place_plaza(Gen *G) {
    ALayout *A = G->A;
    for (int v = 0; v < A->nvoid; ++v) mark(A, A->voids[v], BIT(AU_BLOCK), AU_PLAZA);
    mark(A, expand(A->spine, G->apron), BIT(AU_BLOCK), AU_PLAZA);
    mark(A, A->spine, BIT(AU_BLOCK) | BIT(AU_PLAZA), AU_SPINE);
    mark(A, A->hall, BIT(AU_PLAZA) | BIT(AU_BLOCK), AU_HALL);
}

static void add_concourse(Gen *G, int axis, float a0, float a1, int main, int boulevard, int rib) {
    ALayout *A = G->A;
    if (A->nconc >= AR_MAX_CONC || a1 - a0 < 3.0f) return;
    AConcourse *c = &A->conc[A->nconc];
    c->r = axis ? rect(a0, G->minz, a1, G->maxz) : rect(G->minx, a0, G->maxx, a1);
    int n = count_cells(A, c->r, ~BIT(AU_OUT));
    if ((float)n * CELL * CELL < 20.0f * (a1 - a0)) return;              /* less than 20 m of it inside */
    mark(A, c->r, BIT(AU_BLOCK) | BIT(AU_COLONNADE), AU_CONCOURSE);
    c->axis = (uint8_t)axis; c->main = (uint8_t)main; c->boulevard = (uint8_t)boulevard; c->rib = (uint8_t)rib;
    c->lampStep = rng_range(&G->rli, 11.0f, 13.0f);
    c->lampPhase = c->lampStep * 0.5f;
    c->lampOff = boulevard ? A->colSpacing * 0.5f : 0.0f;
    A->nconc++;
}

/* Colonnade strip on the column line at `line` (axis as the concourse it lines). */
static void colonnade(Gen *G, int axis, float line) {
    ALayout *A = G->A;
    float h = A->colSize * 0.5f + 0.5f;
    ARect r = axis ? rect(line - h, G->minz, line + h, G->maxz) : rect(G->minx, line - h, G->maxx, line + h);
    mark(A, r, BIT(AU_BLOCK), AU_COLONNADE);
}

/* 4. Concourses run between column lines, facade to facade, merging with the canyon floor. */
void place_concourses(Gen *G) {
    ALayout *A = G->A;
    float s = A->colSpacing, h = A->colSize * 0.5f + 0.5f;
    G->ox = A->cx + (A->pattern == AP_BOULEVARD ? 0.0f : 0.5f * s);
    G->oz = A->cz + 0.5f * s;
    A->gridOx = G->ox; A->gridOz = G->oz;
    /* along z */
    if (A->pattern == AP_BOULEVARD) {
        add_concourse(G, 1, A->cx - s + h, A->cx + s - h, 1, 1, 0);
        colonnade(G, 1, A->cx - s); colonnade(G, 1, A->cx + s);
        /* the central colonnade stands in the boulevard */
        float hc = A->colSize * 0.5f + 0.5f;
        mark(A, rect(A->cx - hc, G->minz, A->cx + hc, G->maxz), BIT(AU_CONCOURSE), AU_COLONNADE);
    } else {
        int bays[3] = { -1, -3, 1 }, nb = A->pattern == AP_GRID ? 3 : 1;
        for (int k = 0; k < nb; ++k) {
            float l0 = G->ox + (float)bays[k] * s, l1 = l0 + s;
            if (k && (l0 < G->minx + 13.0f || l1 > G->maxx - 13.0f)) continue;   /* room for a real block beyond it */
            int before = A->nconc;
            add_concourse(G, 1, l0 + h, l1 - h, k == 0, 0, 0);
            if (A->nconc > before) { colonnade(G, 1, l0); colonnade(G, 1, l1); }
        }
    }
    /* along x: the cross, grid rows, or ribs */
    int rows[3] = { -1, -3, 1 }, nr = A->pattern == AP_CROSS ? 1 : 3;
    for (int k = 0; k < nr; ++k) {
        float l0 = G->oz + (float)rows[k] * s, l1 = l0 + s;
        if (k && (l0 < G->minz + 13.0f || l1 > G->maxz - 13.0f)) continue;
        if (A->pattern == AP_BOULEVARD) {
            float rw = clampf(0.45f * (s - A->colSize - 1.0f), 4.0f, 6.0f), mid = 0.5f * (l0 + l1);
            add_concourse(G, 0, mid - rw * 0.5f, mid + rw * 0.5f, 0, 0, 1);
        } else {
            int before = A->nconc;
            add_concourse(G, 0, l0 + h, l1 - h, 0, 0, 0);
            if (A->nconc > before) { colonnade(G, 0, l0); colonnade(G, 0, l1); }
        }
    }
    alog(G, "  info: %d concourse(s) (%s)\n", A->nconc, PATTERN_NAMES[A->pattern]);
}

/* A portal opens wherever a concourse meets the facade; the main portal is on the primary side. */
void place_portals(Gen *G) {
    ALayout *A = G->A;
    const BLoop *fl = &A->tier[0].foot.outer, *ll = &G->g->sec[G->ground].foot.outer;
    for (int c = 0; c < A->nconc; ++c) {
        const AConcourse *cc = &A->conc[c];
        float mid = cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1);
        float width = cc->axis ? cc->r.x1 - cc->r.x0 : cc->r.z1 - cc->r.z0;
        for (int end = 0; end < 2 && A->nportal < AR_MAX_PORTAL; ++end) {
            float px, pz;
            int face;
            if (!outline_crossing(fl, cc->axis, mid, end, &px, &pz, &face)) continue;
            int k1 = (face + 1) % ll->n;
            float ex = ll->x[k1] - ll->x[face], ez = ll->z[k1] - ll->z[face], len = sqrtf(ex * ex + ez * ez);
            float lx, lz;
            arcology_to_local(A, px, pz, &lx, &lz);
            float u = ((lx - ll->x[face]) * ex + (lz - ll->z[face]) * ez) / len;
            float w = minf(width, len - 1.2f);
            if (w < 3.0f) continue;
            u = clampf(u, w * 0.5f + 0.6f, len - w * 0.5f - 0.6f);
            APortal *p = &A->portal[A->nportal++];
            p->face = face; p->u = u; p->width = w;
            p->height = minf(A->ceilY - A->floorY - 2.5f, 7.5f);
            p->fx = px; p->fz = pz;
            p->nx = cc->axis ? 0.0f : (end ? 1.0f : -1.0f);
            p->nz = cc->axis ? (end ? 1.0f : -1.0f) : 0.0f;
            p->main = (uint8_t)(cc->main && end == 0);
            p->service = 0;
        }
    }
    alog(G, "  info: %d portal(s)\n", A->nportal);
}

static int column_ok(const ALayout *A, float x, float z, float size, int allowConcourse) {
    float h = size * 0.5f;
    uint32_t ok = BIT(AU_BLOCK) | BIT(AU_COLONNADE) | (allowConcourse ? BIT(AU_CONCOURSE) : 0u);
    return all_cells(A, rect(x - h, z - h, x + h, z + h), ok) && !in_void(A, x, z, h + 0.5f);
}

/* The column mask: cells a column stands on (1: grid column, 2: setback pier). */
static void col_mask(Gen *G) {
    ALayout *A = G->A;
    memset(G->colMask, 0, (size_t)A->nx * (size_t)A->nz);
    for (int c = 0; c < A->ncol; ++c) {
        int i0, j0, i1, j1;
        float h = A->col[c].size * 0.5f;
        cells_of(A, rect(A->col[c].x - h, A->col[c].z - h, A->col[c].x + h, A->col[c].z + h), &i0, &j0, &i1, &j1);
        for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) G->colMask[IDX(A, i, j)] = (uint8_t)(A->col[c].ring ? 2 : 1);
    }
}

/* 1. The megastructure grid, and each upper tier's outline brought down as a ring of piers. */
void ar_place_columns(Gen *G) {
    ALayout *A = G->A;
    float s = A->colSpacing, cs = A->colSize;
    const BFoot *gf = &A->tier[0].foot;
    /* rings first: they win over grid columns near them */
    int rings = 0;
    for (int t = 1; t < A->ntier; ++t) {
        const BLoop *l = &A->tier[t].foot.outer;
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            float ax = l->x[k], az = l->z[k], bx = l->x[k1], bz = l->z[k1];
            float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
            int n = (int)maxf(1.0f, floorf(len / s + 0.5f));
            for (int j = 0; j < n; ++j) {
                float f = (float)j / (float)n, x = ax + (bx - ax) * f, z = az + (bz - az) * f;
                if (A->ncol >= AR_MAX_COL) break;
                if (bfoot_boundary_dist(gf, x, z, NULL, NULL) < 1.5f + cs * 0.5f) continue;
                if (!column_ok(A, x, z, cs, 0)) continue;     /* on a concourse: a transfer beam instead; in the void: the crown */
                int close = 0;
                for (int c = 0; c < A->ncol && !close; ++c) close = fabsf(A->col[c].x - x) < 0.4f * s && fabsf(A->col[c].z - z) < 0.4f * s;
                if (close) continue;
                AColumn *c = &A->col[A->ncol++];
                c->x = x; c->z = z; c->size = cs; c->ring = 1; c->tier = (uint8_t)t;
                rings++;
            }
            /* transfer beams where this ring edge crosses a concourse */
            for (int c = 0; c < A->nconc && A->nbeam < AR_MAX_BEAM; ++c) {
                ARect r = A->conc[c].r;
                float t0 = 0.0f, t1 = 1.0f, dx = bx - ax, dz = bz - az;
                float p[4] = { -dx, dx, -dz, dz }, qv[4] = { ax - r.x0, r.x1 - ax, az - r.z0, r.z1 - az };
                int out = 0;
                for (int e = 0; e < 4 && !out; ++e) {
                    if (fabsf(p[e]) < 1e-6f) { if (qv[e] < 0.0f) out = 1; continue; }
                    float tt = qv[e] / p[e];
                    if (p[e] < 0.0f) t0 = maxf(t0, tt); else t1 = minf(t1, tt);
                }
                if (out || t1 - t0 < 1e-3f || (t1 - t0) * len < 1.0f) continue;
                float mx = ax + dx * 0.5f * (t0 + t1), mz = az + dz * 0.5f * (t0 + t1);
                if (in_void(A, mx, mz, 0.0f) || !bfoot_contains(gf, mx, mz)) continue;
                ABeam *bm = &A->beam[A->nbeam++];
                bm->x0 = ax + dx * t0; bm->z0 = az + dz * t0; bm->x1 = ax + dx * t1; bm->z1 = az + dz * t1;
            }
        }
    }
    /* the grid: columns on the line crossings, clear of the rings, the street and the void */
    int grid = 0;
    for (float x = G->ox - ceilf((G->ox - G->minx) / s) * s; x <= G->maxx; x += s)
        for (float z = G->oz - ceilf((G->oz - G->minz) / s) * s; z <= G->maxz; z += s) {
            if (A->ncol >= AR_MAX_COL) break;
            if (!bfoot_contains(gf, x, z) || bfoot_boundary_dist(gf, x, z, NULL, NULL) < 2.6f + cs * 0.5f) continue;
            int boulevardLine = A->pattern == AP_BOULEVARD && fabsf(x - A->cx) < 0.01f;
            if (!column_ok(A, x, z, cs, boulevardLine)) continue;
            int close = 0;
            for (int c = 0; c < A->ncol && !close; ++c) close = A->col[c].ring && fabsf(A->col[c].x - x) < 0.4f * s && fabsf(A->col[c].z - z) < 0.4f * s;
            if (close) continue;
            AColumn *c = &A->col[A->ncol++];
            c->x = x; c->z = z; c->size = cs; c->ring = 0; c->tier = 0;
            grid++;
        }
    col_mask(G);
    alog(G, "  info: %d grid columns, %d setback piers in %d ring(s), %d transfer beams\n", grid, rings, A->ntier - 1, A->nbeam);
}

/* Prefix sums of cells whose use is in `ok` (rect counts in O(1)); grid columns
 * (not setback piers, which a core can carry) count as obstacles. */
static void sat_build(Gen *G, uint32_t ok) {
    ALayout *A = G->A;
    int W = A->nx + 1;
    for (int i = 0; i <= A->nx; ++i) G->sat[i] = 0;
    for (int j = 0; j < A->nz; ++j) {
        int row = 0;
        G->sat[(j + 1) * W] = 0;
        for (int i = 0; i < A->nx; ++i) {
            row += (ok & BIT(A->use[IDX(A, i, j)])) && G->colMask[IDX(A, i, j)] != 1;
            G->sat[(j + 1) * W + i + 1] = G->sat[j * W + i + 1] + row;
        }
    }
}

static int sat_full(const Gen *G, int i0, int j0, int i1, int j1) {
    const ALayout *A = G->A;
    if (i0 < 0 || j0 < 0 || i1 > A->nx || j1 > A->nz) return 0;
    int W = A->nx + 1;
    int n = G->sat[j1 * W + i1] - G->sat[j0 * W + i1] - G->sat[j1 * W + i0] + G->sat[j0 * W + i0];
    return n == (i1 - i0) * (j1 - j0);
}

/* 5. Local cores: spread by farthest-point sampling inside the blocks; each rises
 *    until the next tier no longer contains it (3 m margin). */
void place_cores(Gen *G) {
    ALayout *A = G->A;
    int want = clampi((int)(A->area / 3000.0f + 0.5f), 4, 10);
    sat_build(G, BIT(AU_BLOCK));
    const int S = 16;                                   /* 8 m in cells */
    const int M = 2;                                    /* 1 m of block kept round it */
    int ncand = 0;
    int *cand = G->heap;                                /* scratch: candidate lower-left cells */
    for (int j = M; j + S + M <= A->nz; j += 2)
        for (int i = M; i + S + M <= A->nx; i += 2) {
            if (!sat_full(G, i - M, j - M, i + S + M, j + S + M)) continue;
            ARect r = rect(A->gx0 + (float)i * CELL, A->gz0 + (float)j * CELL, A->gx0 + (float)(i + S) * CELL, A->gz0 + (float)(j + S) * CELL);
            if (!rect_inside(&A->tier[0].foot, r, 3.0f)) continue;
            cand[ncand++] = IDX(A, i, j);
        }
    float scx = A->cx, scz = A->cz;
    int ncand0 = ncand;
    while (A->ncore < want && ncand > 0) {
        float best = -1.0f;
        int bi = -1;
        for (int k = 0; k < ncand; ++k) {
            int c = cand[k];
            if (c < 0) continue;
            int i = c % A->nx, j = c / A->nx;
            float x = ccx(A, i) + 3.75f, z = ccz(A, j) + 3.75f;
            float d = sqrtf((x - scx) * (x - scx) + (z - scz) * (z - scz));
            for (int q = 0; q < A->ncore; ++q) {
                float qx = 0.5f * (A->core[q].r.x0 + A->core[q].r.x1), qz = 0.5f * (A->core[q].r.z0 + A->core[q].r.z1);
                d = minf(d, sqrtf((x - qx) * (x - qx) + (z - qz) * (z - qz)));
            }
            if (d > best + 1e-3f) { best = d; bi = k; }
        }
        if (bi < 0) break;
        int c = cand[bi], i = c % A->nx, j = c / A->nx;
        cand[bi] = -1;
        ARect r = rect(A->gx0 + (float)i * CELL, A->gz0 + (float)j * CELL, A->gx0 + (float)(i + S) * CELL, A->gz0 + (float)(j + S) * CELL);
        if (!all_cells(A, expand(r, 0.9f), BIT(AU_BLOCK))) continue;   /* a core or hall placed since */
        /* its hall: the shortest straight way out to a public street */
        int bestD = -1, bestLen = 1 << 30;
        float fcx = 0.5f * (r.x0 + r.x1), fcz = 0.5f * (r.z0 + r.z1);
        for (int d = 0; d < 4; ++d) {
            float sx = fcx + (float)DX[d] * 4.0f, sz = fcz + (float)DZ[d] * 4.0f;
            int len = 0, hit = 0;
            for (int step = 1; step <= 40 && !hit; ++step) {
                float x = sx + (float)DX[d] * (float)step * CELL, z = sz + (float)DZ[d] * (float)step * CELL;
                int u = arcology_use_at(A, x, z);
                if (arcology_public(u)) hit = 1;
                else if (u != AU_BLOCK) break;
                len = step;
            }
            if (hit && len < bestLen) { bestLen = len; bestD = d; }
        }
        if (bestD < 0) continue;
        ACore *k = &A->core[A->ncore++];
        k->r = r;
        k->door = (uint8_t)bestD;
        k->lifts = rng_int(&G->rl, 2, 4);
        k->topTier = 0;
        while (k->topTier + 1 < A->ntier && rect_inside(&A->tier[k->topTier + 1].foot, r, 3.0f)) k->topTier++;
        mark(A, r, BIT(AU_BLOCK), AU_CORE);
        /* the core carries any setback pier it stands on */
        int kept = 0;
        for (int q = 0; q < A->ncol; ++q)
            if (!(A->col[q].ring && overlap(expand(r, A->col[q].size * 0.5f), rect(A->col[q].x, A->col[q].z, A->col[q].x, A->col[q].z)))) A->col[kept++] = A->col[q];
        if (kept != A->ncol) { A->ncol = kept; col_mask(G); }
        float L = (float)bestLen * CELL + 0.5f;
        ARect h;
        if (bestD == 0)      h = rect(fcx - 1.5f, r.z0 - L, fcx + 1.5f, r.z0);
        else if (bestD == 2) h = rect(fcx - 1.5f, r.z1, fcx + 1.5f, r.z1 + L);
        else if (bestD == 1) h = rect(r.x1, fcz - 1.5f, r.x1 + L, fcz + 1.5f);
        else                 h = rect(r.x0 - L, fcz - 1.5f, r.x0, fcz + 1.5f);
        mark(A, h, BIT(AU_BLOCK), AU_COREHALL);
    }
    if (A->ncore < want) alog(G, "  simplify: %d of %d local cores placed (%d sites in the blocks)\n", A->ncore, want, ncand0);
    int tops[AR_MAX_TIER] = { 0 };
    for (int k = 0; k < A->ncore; ++k) tops[A->core[k].topTier]++;
    alog(G, "  info: %d local cores, stopping at tiers:", A->ncore);
    for (int t = 0; t < A->ntier; ++t) if (tops[t]) alog(G, " %d x%d", t, tops[t]);
    alog(G, "\n");
}

/* Tier decks: a sky lobby at the base of every upper tier, where the express
 * core stops, gated by the tier's stratum, and the terrace on the setback roof
 * of the tier below. The local cores that stop under it transfer there. */
static const char *const GATE_NAMES[] = { "public", "residents", "checkpoint", "escort only" };

void place_sky_lobbies(Gen *G) {
    ALayout *A = G->A;
    for (int t = 1; t < A->ntier && A->nsky < AR_MAX_TIER; ++t) {
        ASkyLobby *k = &A->sky[A->nsky++];
        k->tier = t;
        k->y = A->tier[t].y0;
        int st = A->tier[t].stratum;
        k->gate = (uint8_t)(st == AS_STREET ? AK_PUBLIC : st == AS_HABITAT ? AK_RESIDENTS : st == AS_COMMERCE ? AK_CHECKPOINT : AK_ESCORT);
        k->cores = 0;
        for (int c = 0; c < A->ncore; ++c) k->cores += A->core[c].topTier == t - 1;
        k->terrace = fabsf(bloop_area(&A->tier[t - 1].foot.outer)) - fabsf(bloop_area(&A->tier[t].foot.outer));
        alog(G, "  info: sky lobby %d at %.0f m (%s, %s), %d local core(s) transfer, %.0f m2 terrace\n", t, k->y - G0,
             STRATUM_NAMES[st], GATE_NAMES[k->gate], k->cores, k->terrace);
    }
}
