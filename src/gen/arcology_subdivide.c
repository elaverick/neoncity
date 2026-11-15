#include "arcology_internal.h"

/* ===========================================================================
 * Step 5: subdivide
 * ========================================================================= */

/* Walking distance (m) over the walkable cells from every exit: portals, the
 * express hall, local core doors and escape stairs. Columns are obstacles.
 * Returns the farthest distance and its cell. */
float travel(Gen *G, int *farCell) {
    ALayout *A = G->A;
    int N = A->nx * A->nz, nh = 0;
    float *d = G->dist;
    int *heap = G->heap;
    for (int c = 0; c < N; ++c) d[c] = 1e30f;
#define PUSH(C, D) do { int c_ = (C); float d_ = (D); if (d_ < d[c_]) { d[c_] = d_; int k_ = nh++; heap[k_] = c_; \
        while (k_ > 0 && d[heap[(k_ - 1) / 2]] > d[heap[k_]]) { int t_ = heap[k_]; heap[k_] = heap[(k_ - 1) / 2]; heap[(k_ - 1) / 2] = t_; k_ = (k_ - 1) / 2; } } } while (0)
    for (int j = 0; j < A->nz; ++j)
        for (int i = 0; i < A->nx; ++i) {
            int c = IDX(A, i, j), u = A->use[c];
            if (!(WALK & BIT(u)) || G->colMask[c]) continue;
            int src = u == AU_HALL;
            for (int q = 0; q < 4 && !src; ++q) {
                int n = use_ij(A, i + DX[q], j + DZ[q]);
                if (n == AU_STAIR || (n == AU_CORE && u == AU_COREHALL)) src = 1;
            }
            for (int p = 0; p < A->nportal && !src; ++p) src = behind_portal(A, &A->portal[p], i, j, 0.75f);
            if (src) PUSH(c, 0.0f);
        }
    float worst = 0.0f;
    *farCell = -1;
    while (nh > 0) {
        int c = heap[0];
        heap[0] = heap[--nh];
        int k = 0;
        for (;;) {
            int l = 2 * k + 1, r = l + 1, m = k;
            if (l < nh && d[heap[l]] < d[heap[m]]) m = l;
            if (r < nh && d[heap[r]] < d[heap[m]]) m = r;
            if (m == k) break;
            int t = heap[k]; heap[k] = heap[m]; heap[m] = t;
            k = m;
        }
        int i = c % A->nx, j = c / A->nx;
        if (d[c] > worst) { worst = d[c]; *farCell = c; }
        for (int dj = -1; dj <= 1; ++dj)
            for (int di = -1; di <= 1; ++di) {
                if (!di && !dj) continue;
                int a = i + di, b = j + dj;
                if (!in_grid(A, a, b)) continue;
                int n = IDX(A, a, b);
                if (!(WALK & BIT(A->use[n])) || G->colMask[n]) continue;
                if (di && dj && (!(WALK & BIT(A->use[IDX(A, a, j)])) || !(WALK & BIT(A->use[IDX(A, i, b)])))) continue;
                PUSH(n, d[c] + ((di && dj) ? 1.41421f : 1.0f) * CELL);
            }
    }
#undef PUSH
    return worst;
}

/* 2. Escape stairs first: claimed at the nearest block edge wherever the walk
 *    to a stair or portal exceeds the 45 m travel limit. */
void place_stairs(Gen *G) {
    ALayout *A = G->A;
    for (int round = 0; round < AR_MAX_STAIR; ++round) {
        int far;
        float worst = travel(G, &far);
        if (worst <= 45.0f || far < 0) return;
        int fi = far % A->nx, fj = far / A->nx;
        /* the block frontage cells nearest the far cell, nearest first */
        int placed = 0;
        for (int radius = 2; radius < 60 && !placed; radius += 2) {
            for (int j = fj - radius; j <= fj + radius && !placed; ++j)
                for (int i = fi - radius; i <= fi + radius && !placed; ++i) {
                    if (abs(i - fi) != radius && abs(j - fj) != radius) continue;
                    if (use_ij(A, i, j) != AU_BLOCK) continue;
                    for (int d = 0; d < 4 && !placed; ++d) {
                        if (!arcology_public(use_ij(A, i + DX[d], j + DZ[d]))) continue;
                        /* 4 m along the frontage, 7 m deep into the block */
                        int i0, j0, i1, j1;
                        if (d == 0)      { i0 = i - 4; i1 = i + 4; j0 = j; j1 = j + 14; }
                        else if (d == 2) { i0 = i - 4; i1 = i + 4; j0 = j - 13; j1 = j + 1; }
                        else if (d == 1) { j0 = j - 4; j1 = j + 4; i0 = i - 13; i1 = i + 1; }
                        else             { j0 = j - 4; j1 = j + 4; i0 = i; i1 = i + 14; }
                        ARect r = rect(A->gx0 + (float)i0 * CELL, A->gz0 + (float)j0 * CELL, A->gx0 + (float)i1 * CELL, A->gz0 + (float)j1 * CELL);
                        if (!all_cells(A, r, BIT(AU_BLOCK))) continue;
                        AStair *s = &A->stair[A->nstair++];
                        s->r = r; s->facing = (uint8_t)d;
                        mark(A, r, BIT(AU_BLOCK), AU_STAIR);
                        placed = 1;
                    }
                }
        }
        if (!placed) { alog(G, "  simplify: no block edge for an escape stair near a far point (%.0f m)\n", worst); return; }
        if (A->nstair >= AR_MAX_STAIR) return;
    }
}

/* Blocks: what is left between concourses, the canyon floor and the facade.
 * Narrow ones become kiosk strips; one at the rear becomes the service yard. */
void find_blocks(Gen *G) {
    ALayout *A = G->A;
    int N = A->nx * A->nz;
    memset(A->blk, NONE, (size_t)N);
    int *q = G->heap;
    for (int c0 = 0; c0 < N; ++c0) {
        if (A->use[c0] != AU_BLOCK || A->blk[c0] != NONE) continue;
        if (A->nblock >= AR_MAX_BLOCK) break;
        int b = A->nblock++, head = 0, tail = 0;
        ABlock *B = &A->block[b];
        B->box = rect(1e30f, 1e30f, -1e30f, -1e30f);
        B->cells = 0; B->core = -1;
        q[tail++] = c0;
        A->blk[c0] = (uint8_t)b;
        while (head < tail) {
            int c = q[head++], i = c % A->nx, j = c / A->nx;
            B->cells++;
            B->box.x0 = minf(B->box.x0, A->gx0 + (float)i * CELL); B->box.x1 = maxf(B->box.x1, A->gx0 + (float)(i + 1) * CELL);
            B->box.z0 = minf(B->box.z0, A->gz0 + (float)j * CELL); B->box.z1 = maxf(B->box.z1, A->gz0 + (float)(j + 1) * CELL);
            for (int d = 0; d < 4; ++d) {
                int a = i + DX[d], bb = j + DZ[d];
                if (!in_grid(A, a, bb)) continue;
                int n = IDX(A, a, bb);
                if (A->use[n] == AU_BLOCK && A->blk[n] == NONE) { A->blk[n] = (uint8_t)b; q[tail++] = n; }
                else if (A->use[n] == AU_CORE && B->core < 0)
                    for (int k = 0; k < A->ncore; ++k) if (in_rect(A->core[k].r, ccx(A, a), ccz(A, bb))) B->core = k;
            }
        }
    }
    /* width: twice the deepest cell's distance from anything that is not this block */
    float *dd = G->dist;
    int head = 0, tail = 0;
    for (int c = 0; c < N; ++c) {
        dd[c] = 1e30f;
        if (A->use[c] != AU_BLOCK) continue;
        int i = c % A->nx, j = c / A->nx, edge = 0;
        for (int d = 0; d < 4 && !edge; ++d) edge = use_ij(A, i + DX[d], j + DZ[d]) != AU_BLOCK;
        if (edge) { dd[c] = 1.0f; q[tail++] = c; }
    }
    while (head < tail) {
        int c = q[head++], i = c % A->nx, j = c / A->nx;
        for (int d = 0; d < 4; ++d) {
            int a = i + DX[d], b = j + DZ[d];
            if (!in_grid(A, a, b)) continue;
            int n = IDX(A, a, b);
            if (A->use[n] == AU_BLOCK && dd[n] > dd[c] + 1.0f) { dd[n] = dd[c] + 1.0f; q[tail++] = n; }
        }
    }
    float deep[AR_MAX_BLOCK] = { 0 };
    for (int c = 0; c < N; ++c) if (A->blk[c] != NONE) deep[A->blk[c]] = maxf(deep[A->blk[c]], dd[c]);
    /* the service yard: the block with most frontage on the rear facade */
    int yard = -1, yardRun = 0;
    for (int b = 0; b < A->nblock; ++b) {
        if (A->block[b].core >= 0 || deep[b] * CELL * 2.0f < 7.0f) continue;
        int run = 0;
        for (int c = 0; c < N; ++c) {
            if (A->blk[c] != b) continue;
            int i = c % A->nx, j = c / A->nx;
            if (ccz(A, j) > A->cz && use_ij(A, i, j + 1) == AU_OUT) run++;
        }
        if (run > yardRun) { yardRun = run; yard = b; }
    }
    if (yardRun < 10) yard = -1;
    int kiosks = 0;
    for (int b = 0; b < A->nblock; ++b) {
        ABlock *B = &A->block[b];
        B->height = minf(rng_range(&G->rb, 4.0f, 5.5f), (A->ceilY - A->floorY) * 0.62f);
        if (deep[b] * CELL * 2.0f < 7.0f) {
            B->use = AB_KIOSK;
            kiosks++;
        } else if (b == yard) {
            B->use = AB_YARD;
        } else if (B->core >= 0) {
            B->use = AB_HOUSING;
        } else {
            float area = (float)B->cells * CELL * CELL;
            float w[8] = { area > 250.0f ? 2.5f : 0.6f, 1.6f, 1.2f, 1.0f, area > 200.0f ? 1.4f : 0.4f, 1.3f, area > 220.0f ? 1.0f : 0.3f, 0.9f };
            float tot = 0.0f;
            for (int k = 0; k < 8; ++k) tot += w[k];
            float r = rng_float(&G->rb) * tot;
            int u = 0;
            while (u < 7 && r > w[u]) { r -= w[u]; u++; }
            B->use = (uint8_t)u;
        }
        int to = B->use == AB_KIOSK ? AU_KIOSK : B->use == AB_YARD ? AU_YARD : AU_BLOCK;
        if (to != AU_BLOCK)
            for (int c = 0; c < N; ++c) if (A->blk[c] == b) A->use[c] = (uint8_t)to;
    }
    /* the service portal: on the rear facade, into the yard */
    if (yard >= 0 && A->nportal < AR_MAX_PORTAL) {
        const ABlock *B = &A->block[yard];
        const BLoop *fl = &A->tier[0].foot.outer, *ll = &G->g->sec[G->ground].foot.outer;
        float mid = 0.5f * (B->box.x0 + B->box.x1), px, pz;
        int face;
        if (outline_crossing(fl, 1, mid, 1, &px, &pz, &face)) {
            int k1 = (face + 1) % ll->n;
            float ex = ll->x[k1] - ll->x[face], ez = ll->z[k1] - ll->z[face], len = sqrtf(ex * ex + ez * ez), lx, lz;
            arcology_to_local(A, px, pz, &lx, &lz);
            float w = minf(4.5f, minf(B->box.x1 - B->box.x0 - 2.0f, len - 2.0f));
            if (w >= 3.0f) {
                APortal *p = &A->portal[A->nportal++];
                p->face = face;
                p->u = clampf(((lx - ll->x[face]) * ex + (lz - ll->z[face]) * ez) / len, w * 0.5f + 0.6f, len - w * 0.5f - 0.6f);
                p->width = w; p->height = 4.8f;
                p->fx = px; p->fz = pz; p->nx = 0.0f; p->nz = 1.0f;
                p->main = 0; p->service = 1;
            }
        }
    }
    alog(G, "  info: %d market blocks (%d kiosk strips%s)\n", A->nblock, kiosks, yard >= 0 ? ", a service yard" : "");
}

/* 3. Stall rows: every block edge facing a concourse or the canyon floor is cut
 *    into stall units at the band depth. */
void place_stalls(Gen *G) {
    ALayout *A = G->A;
    const int D = 7;                                     /* 3.5 m deep */
    for (int d = 0; d < 4; ++d) {
        int alongI = d == 0 || d == 2;                   /* runs along i for -z / +z frontages */
        int nLines = alongI ? A->nz : A->nx, nAlong = alongI ? A->nx : A->nz;
        for (int line = 0; line < nLines; ++line) {
            int start = -1, runBlock = -1;
            for (int a = 0; a <= nAlong; ++a) {
                int ok = 0, b = -1;
                if (a < nAlong) {
                    int i = alongI ? a : line, j = alongI ? line : a, c = IDX(A, i, j);
                    if (A->use[c] == AU_BLOCK && A->blk[c] != NONE && A->block[A->blk[c]].use != AB_KIOSK && A->block[A->blk[c]].use != AB_YARD) {
                        int n = use_ij(A, i + DX[d], j + DZ[d]);
                        ok = n == AU_CONCOURSE || n == AU_COLONNADE || n == AU_PLAZA;
                        b = A->blk[c];
                    }
                }
                if (ok && start >= 0 && b != runBlock) { ok = 0; a--; }     /* a new block starts: close this run */
                if (ok && start < 0) { start = a; runBlock = b; }
                if (ok) continue;
                if (start >= 0) {
                    /* cut [start, a) into units of 3 to 4.5 m */
                    int pos = start, end = a;
                    while (end - pos >= 4 && A->nstall < AR_MAX_STALL) {
                        int w = rng_int(&G->rt, 6, 9);
                        if (end - pos - w < 6) w = end - pos <= 11 ? end - pos : w;
                        int u0 = pos, u1 = pos + w;
                        pos += w;
                        for (int depth = D; depth >= 5; depth -= 2) {
                            int placed = 0;
                            for (int trim = 0; trim <= 3 && !placed; ++trim) {
                                int t0 = u0 + trim, t1 = u1 - trim;
                                if (t1 - t0 < 4) break;
                                int i0, i1, j0, j1;
                                if (alongI) { i0 = t0; i1 = t1; j0 = d == 0 ? line : line - depth + 1; j1 = j0 + depth; }
                                else        { j0 = t0; j1 = t1; i0 = d == 3 ? line : line - depth + 1; i1 = i0 + depth; }
                                ARect r = rect(A->gx0 + (float)i0 * CELL, A->gz0 + (float)j0 * CELL, A->gx0 + (float)i1 * CELL, A->gz0 + (float)j1 * CELL);
                                int okAll = all_cells(A, r, BIT(AU_BLOCK));
                                if (okAll) {
                                    int ci0, cj0, ci1, cj1;
                                    cells_of(A, r, &ci0, &cj0, &ci1, &cj1);
                                    for (int j = cj0; j < cj1 && okAll; ++j) for (int i = ci0; i < ci1 && okAll; ++i) okAll = A->blk[IDX(A, i, j)] == runBlock;
                                }
                                if (!okAll) continue;
                                AStall *s = &A->stall[A->nstall++];
                                s->r = r; s->facing = (uint8_t)d; s->block = (uint8_t)runBlock;
                                s->kind = A->block[runBlock].use;
                                if (s->kind == AB_HOUSING) {   /* shops at the foot of the housing */
                                    static const uint8_t RETAIL[5] = { AB_MARKET, AB_NOODLE, AB_REPAIR, AB_CLINIC, AB_ARCADE };
                                    s->kind = RETAIL[rng_int(&G->rt, 0, 4)];
                                }
                                s->closed = (uint8_t)(rng_float(&G->rt) < 0.14f);
                                s->seed = rng_u32(&G->rt);
                                mark(A, r, BIT(AU_BLOCK), AU_STALL);
                                placed = 1;
                            }
                            if (placed) break;
                        }
                    }
                    start = -1; runBlock = -1;
                }
            }
        }
    }
    alog(G, "  info: %d stalls\n", A->nstall);
}

/* SF5b (street vendors, ~/.claude/plans/street-vendors.md "SF5b"): the noodle halls and night markets opened into
 * halls you walk into (the user's calls: only those blocks, one mouth, seating by use, always open; arcology.h AHall).
 * A block's longest row of stall units on a concourse is given up as the hall's mouth; its other units stay, their
 * backs the hall's walls; the rest of the block is the hall's floor (AU_FOOD) under the block's roof. Then, from the
 * hall's own hash (nothing else of the layout moves): wall stalls along its walls facing in (the longest walls first,
 * HALL_BAR_D deep, the way in front of each kept clear), tables in the middle on the grid that fits most (clear of the
 * bars' ways, the columns and a strip inside the mouth), a fan over each table and rows of battens across the hall
 * parallel to its mouth, clear of the bars (their own lights) and the fans. */
#define HALL_AREA   60.0f    /* the least floor a hall opens with (m2) */
#define HALL_MOUTH  6.0f     /* the shortest mouth */
#define HALL_BAR_D  3.2f     /* a bar's site deep (its stools fit: noodle_bar.c bar_depth) */
#define HALL_BAR_W0 2.8f     /* its width across, from... */
#define HALL_BAR_W1 3.45f    /* ...to (under 3.4 m between its walls: the straight form, never the corner) */
#define HALL_WAY    1.5f     /* kept clear in front of a bar (src/city/vendor_sites.h VENDOR_WALK_LOT) */
#define HALL_ENTRY  2.0f     /* the strip inside the mouth kept clear of tables */
static int hall_col(const ALayout *A, ARect r, float grow) {
    for (int c = 0; c < A->ncol; ++c) {
        const float h = 0.575f * A->col[c].size + grow;   /* (the walker meets a column 15% larger than it's drawn) */
        if (overlap(r, rect(A->col[c].x - h, A->col[c].z - h, A->col[c].x + h, A->col[c].z + h))) return 1;
    }
    return 0;
}
/* Every cell under r the hall's floor (AU_FOOD of block b). */
static int hall_cells(const ALayout *A, ARect r, int b) {
    if (!all_cells(A, r, BIT(AU_FOOD))) return 0;
    int i0, j0, i1, j1;
    cells_of(A, r, &i0, &j0, &i1, &j1);
    for (int j = j0; j < j1; ++j) for (int i = i0; i < i1; ++i) if (A->blk[IDX(A, i, j)] != b) return 0;
    return 1;
}
/* A bar's site (its back on the wall at `line`, facing f, from a0 to a1 along it, d deep) and the way in front. */
static ARect hall_site(int f, float line, float a0, float a1, float d) {
    switch (f) {
    case 0:  return rect(a0, line - d, a1, line);
    case 2:  return rect(a0, line, a1, line + d);
    case 1:  return rect(line, a0, line + d, a1);
    default: return rect(line - d, a0, line, a1);
    }
}
static ARect hall_bar_rect(const AHallBar *b, float extra) {
    const int f = b->facing;
    const float line = f == 0 || f == 2 ? b->bz : b->bx, mid = f == 0 || f == 2 ? b->bx : b->bz;
    return hall_site(f, line, mid - 0.5f * b->w, mid + 0.5f * b->w, b->d + extra);
}
/* The strip inside the mouth. */
static ARect hall_entry(const AHall *H, float depth) {
    switch (H->facing) {
    case 0:  return rect(H->m0, H->line, H->m1, H->line + depth);
    case 2:  return rect(H->m0, H->line - depth, H->m1, H->line);
    case 1:  return rect(H->line - depth, H->m0, H->line, H->m1);
    default: return rect(H->line, H->m0, H->line + depth, H->m1);
    }
}
/* The mouth's pier k. */
static ARect hall_pier(const AHall *H, int k) {
    const float a = H->pier[k], hp = 0.5f * AR_HALL_PIER, p1 = H->line + (H->facing == 0 || H->facing == 3 ? 1.0f : -1.0f) * AR_HALL_PIER;
    return H->facing == 0 || H->facing == 2 ? rect(a - hp, minf(H->line, p1), a + hp, maxf(H->line, p1))
                                            : rect(minf(H->line, p1), a - hp, maxf(H->line, p1), a + hp);
}
static int hall_clear_of_piers(const AHall *H, ARect r) {
    for (int k = 0; k < H->npier; ++k) if (overlap(r, expand(hall_pier(H, k), 0.05f))) return 0;
    return 1;
}
static int hall_clear_of_bars(const AHall *H, ARect r, float gap) {
    for (int k = 0; k < H->nbar; ++k) if (overlap(expand(r, gap), hall_bar_rect(&H->bar[k], HALL_WAY))) return 0;
    return 1;
}

/* The walls a bar may stand against, facing f into the hall: runs of the hall's cells whose neighbour behind isn't
 * the hall (nor its mouth). */
typedef struct HallRun { float line, a0, a1; int f, out; } HallRun;   /* out: the facade behind it */
#define HALL_FACADE 0.6f   /* a bar's back off the facade's cells (the cells overhang the outline by up to 0.25 m, its wall stands 0.3 m in) */
static int hall_runs(const ALayout *A, const AHall *H, int b, HallRun *out, int max) {
    int n = 0;
    for (int f = 0; f < 4; ++f) {
        const int alongI = f == 0 || f == 2;
        const int nLines = alongI ? A->nz : A->nx, nAlong = alongI ? A->nx : A->nz;
        const float g0 = alongI ? A->gx0 : A->gz0, gl = alongI ? A->gz0 : A->gx0;
        for (int line = 0; line < nLines; ++line) {
            int start = -1, startOut = 0;
            for (int a = 0; a <= nAlong; ++a) {
                int ok = 0, fac = 0;
                if (a < nAlong) {
                    const int i = alongI ? a : line, j = alongI ? line : a;
                    const int c = IDX(A, i, j);
                    if (A->use[c] == AU_FOOD && A->blk[c] == b) {
                        const int bi = i - DX[f], bj = j - DZ[f];
                        const int behind = in_grid(A, bi, bj) && A->use[IDX(A, bi, bj)] == AU_FOOD && A->blk[IDX(A, bi, bj)] == b;
                        ok = !behind;
                        fac = use_ij(A, bi, bj) == AU_OUT;
                        if (ok && (f + 2) % 4 == H->facing) {   /* the mouth is no wall */
                            const float at = alongI ? ccx(A, i) : ccz(A, j);
                            ok = at < H->m0 || at > H->m1;
                        }
                    }
                }
                /* a run ends where the wall does, or where the facade starts or stops behind it */
                if (start >= 0 && (!ok || fac != startOut)) {
                    if (n < max) {
                        HallRun *r = &out[n++];
                        r->f = f;
                        r->a0 = g0 + (float)start * CELL; r->a1 = g0 + (float)a * CELL;
                        /* the back edge: the cells' side towards -f; off the facade by HALL_FACADE */
                        r->line = gl + (float)line * CELL + (DX[f] + DZ[f] < 0 ? CELL : 0.0f);
                        r->out = startOut;
                        if (startOut) r->line += (float)(DX[f] + DZ[f]) * HALL_FACADE;
                    }
                    start = -1;
                }
                if (ok && start < 0) { start = a; startOut = fac; }
            }
        }
    }
    return n;
}

static void hall_bars(const ALayout *A, AHall *H, int b) {
    HallRun run[256];
    int nr = hall_runs(A, H, b, run, 256);
    /* the longest first */
    for (int i = 0; i < nr; ++i)
        for (int j = i + 1; j < nr; ++j)
            if (run[j].a1 - run[j].a0 > run[i].a1 - run[i].a0 + 1e-4f) { HallRun t = run[i]; run[i] = run[j]; run[j] = t; }
    uint32_t h = hash_combine(H->seed, 0xBA25u);
    for (int r = 0; r < nr && H->nbar < AR_HALL_BARS; ++r) {
        const HallRun *R = &run[r];
        /* (6 cm off the run's ends: the walls there have their skin inside, collision.c) */
        const float a1 = R->a1 - 0.06f;
        float pos = R->a0 + 0.06f;
        while (a1 - pos >= HALL_BAR_W0 - 1e-3f && H->nbar < AR_HALL_BARS) {
            h = hash_combine(h, 1u);
            float w = HALL_BAR_W0 + (HALL_BAR_W1 - HALL_BAR_W0) * hash_float(h);
            if (a1 - pos - w < HALL_BAR_W0) w = minf(a1 - pos, HALL_BAR_W1);   /* no sliver left at the end */
            const ARect site = hall_site(R->f, R->line, pos, pos + w, HALL_BAR_D), way = hall_site(R->f, R->line, pos, pos + w, HALL_BAR_D + HALL_WAY);
            /* (a column may stand in the way past the stools, as under the arcades: not at the counter) */
            int ok = hall_cells(A, way, b) && !hall_col(A, hall_site(R->f, R->line, pos, pos + w, HALL_BAR_D + 0.9f), 0.1f) &&
                     rect_inside(&A->tier[0].foot, site, 0.3f) && rect_inside(&A->tier[0].foot, way, 0.32f) && hall_clear_of_piers(H, way);
            for (int k = 0; k < H->nbar && ok; ++k)
                ok = !overlap(expand(site, -0.005f), hall_bar_rect(&H->bar[k], HALL_WAY)) && !overlap(way, hall_bar_rect(&H->bar[k], 0.0f));
            if (!ok) { pos += CELL; continue; }
            AHallBar *o = &H->bar[H->nbar++];
            o->facing = (uint8_t)R->f; o->w = w - 0.02f; o->d = HALL_BAR_D;
            const float mid = pos + 0.5f * w;
            if (R->f == 0 || R->f == 2) { o->bx = mid; o->bz = R->line; } else { o->bx = R->line; o->bz = mid; }
            pos += w;
        }
    }
}

/* Tables on a grid square to the mouth, at the phase that fits most; a fan over each. */
static void hall_seats(const ALayout *A, AHall *H, int b) {
    const int lng = H->seating == AH_LONG;
    const int mx = H->facing == 0 || H->facing == 2;      /* the mouth runs along x */
    /* half sizes along the mouth (u) and into the hall (v); pitches */
    const float hu = lng ? 0.80f : 0.80f, hv = lng ? 1.22f : 0.80f;
    const float pu = lng ? 2.6f : 2.5f, pv = lng ? 3.3f : 2.5f;
    const ARect bx = H->box, entry = hall_entry(H, HALL_ENTRY);
    const float u0 = mx ? bx.x0 : bx.z0, u1 = mx ? bx.x1 : bx.z1, v0 = mx ? bx.z0 : bx.x0, v1 = mx ? bx.z1 : bx.x1;
    float best[AR_HALL_SEATS][2];
    int nbest = -1;
    for (int pa = 0; pa < 5; ++pa)
        for (int pb = 0; pb < 5; ++pb) {
            float got[AR_HALL_SEATS][2];
            int n = 0;
            for (float v = v0 + hv + (float)pb * pv / 5.0f; v + hv <= v1 && n < AR_HALL_SEATS; v += pv)
                for (float u = u0 + hu + (float)pa * pu / 5.0f; u + hu <= u1 && n < AR_HALL_SEATS; u += pu) {
                    const ARect fp = mx ? rect(u - hu, v - hv, u + hu, v + hv) : rect(v - hv, u - hu, v + hv, u + hu);
                    if (!hall_cells(A, expand(fp, 0.35f), b) || !rect_inside(&A->tier[0].foot, fp, 0.5f) || overlap(fp, entry) || hall_col(A, fp, 0.45f) ||
                        !hall_clear_of_bars(H, fp, 0.25f) || !hall_clear_of_piers(H, expand(fp, 0.3f))) continue;
                    got[n][0] = mx ? u : v; got[n][1] = mx ? v : u; n++;
                }
            if (n > nbest) { nbest = n; memcpy(best, got, sizeof(float) * 2 * (size_t)n); }
        }
    for (int k = 0; k < nbest; ++k) {
        float *s = H->seat[H->nseat++];
        s[0] = best[k][0]; s[1] = best[k][1];
        /* a long table's length into the hall; a hawker table at 45 degrees or square, by hash */
        s[2] = lng ? (mx ? 0.5f * PI_F : 0.0f) : (hash_float(hash_combine(H->seed, 0x5EA7u + (uint32_t)k)) < 0.5f ? 0.25f * PI_F : 0.0f);
        if (H->nfan < AR_HALL_FANS && !hall_col(A, rect(s[0] - 0.75f, s[1] - 0.75f, s[0] + 0.75f, s[1] + 0.75f), 0.1f)) {
            H->fan[H->nfan][0] = s[0]; H->fan[H->nfan][1] = s[1]; H->nfan++;
        }
    }
}

/* Battens in rows parallel to the mouth, 2.5 m apart from it inwards, 2.4 m apart along each row. */
/* Battens in rows parallel to the mouth: along the aisles between the rows of tables (the fans hang over the tables),
 * and, where no table row is near, every 2.5 m from 1.4 m in; 2.4 m apart along each row, off the bars' sites (their
 * own lights) and clear of the fans' blades. */
static void hall_battens(const ALayout *A, AHall *H, int b) {
    const int mx = H->facing == 0 || H->facing == 2;
    const ARect bx = H->box;
    const float u0 = mx ? bx.x0 : bx.z0, u1 = mx ? bx.x1 : bx.z1;
    const float in = H->facing == 0 || H->facing == 3 ? 1.0f : -1.0f;   /* into the hall from the mouth's line */
    const float depth = mx ? bx.z1 - bx.z0 : bx.x1 - bx.x0;
    const float pv = H->seating == AH_LONG ? 3.3f : 2.5f;              /* the tables' rows' pitch (hall_seats) */
    /* the rows' depths in from the mouth: the tables' rows' (where the fans are), then the battens' */
    float trow[AR_HALL_SEATS], rows[64];
    int nt = 0, nr = 0;
    for (int k = 0; k < H->nseat; ++k) {
        const float v = ((mx ? H->seat[k][1] : H->seat[k][0]) - H->line) * in;
        int dup = 0;
        for (int q = 0; q < nt && !dup; ++q) dup = fabsf(trow[q] - v) < 0.3f;
        if (!dup) trow[nt++] = v;
    }
    for (int q = 0; q < nt; ++q)
        for (int s = -1; s <= 1; s += 2) if (nr < 64) rows[nr++] = trow[q] + 0.5f * pv * (float)s;
    for (float v = 1.4f; v < depth - 0.6f && nr < 64; v += 2.5f) {
        int near = 0;
        for (int q = 0; q < nt && !near; ++q) near = fabsf(trow[q] - v) < 0.5f * pv + 0.6f;
        if (!near) rows[nr++] = v;
    }
    /* in order from the mouth, and no two within 0.8 m */
    for (int i2 = 0; i2 < nr; ++i2)
        for (int j2 = i2 + 1; j2 < nr; ++j2) if (rows[j2] < rows[i2]) { const float t = rows[i2]; rows[i2] = rows[j2]; rows[j2] = t; }
    float last = -1e30f;
    for (int r = 0; r < nr && H->nbatten < AR_HALL_BATTENS; ++r) {
        const float v = rows[r];
        if (v < 0.8f || v > depth - 0.4f || v - last < 0.8f) continue;
        last = v;
        const float line = H->line + in * v;
        const float span = u1 - u0;
        const int n = (int)floorf((span - 0.6f) / 2.4f);
        if (n < 1) continue;
        const float start = u0 + 0.5f * (span - 2.4f * (float)(n - 1));
        for (int k = 0; k < n && H->nbatten < AR_HALL_BATTENS; ++k) {
            const float u = start + 2.4f * (float)k, x = mx ? u : line, z = mx ? line : u;
            const ARect fp = mx ? rect(u - 0.8f, line - 0.1f, u + 0.8f, line + 0.1f) : rect(line - 0.1f, u - 0.8f, line + 0.1f, u + 0.8f);
            if (!hall_cells(A, fp, b) || hall_col(A, fp, 0.3f)) continue;
            int ok = 1;
            for (int q = 0; q < H->nbar && ok; ++q) ok = !overlap(fp, hall_bar_rect(&H->bar[q], 0.3f));
            for (int q = 0; q < H->nfan && ok; ++q) {   /* (a fan's blades reach 0.67 m; the batten 0.75 along itself) */
                const float du = fabsf((mx ? H->fan[q][0] : H->fan[q][1]) - u), dv = fabsf((mx ? H->fan[q][1] : H->fan[q][0]) - line);
                ok = dv > 0.8f || du > 1.5f;
            }
            if (!ok) continue;
            float *o = H->batten[H->nbatten++];
            o[0] = x; o[1] = z; o[2] = mx ? 0.0f : 1.0f;   /* along the mouth */
        }
    }
}

void place_halls(Gen *G) {
    ALayout *A = G->A;
    for (int b = 0; b < A->nblock && A->nfhall < AR_MAX_HALL; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use != AB_NOODLE && B->use != AB_MARKET) continue;
        int stair = 0;
        for (int s = 0; s < A->nstair && !stair; ++s) stair = overlap(A->stair[s].r, B->box);
        if (stair) { alog(G, "  info: the %s block %d keeps shut: an escape stair stands in it\n", arcology_use_name(B->use), b); continue; }
        /* the mouth: the block's longest row of units on one line */
        int bf = -1;
        float bline = 0.0f, bsum = 0.0f, ba0 = 0.0f, ba1 = 0.0f;
        for (int s = 0; s < A->nstall; ++s) {
            const AStall *st = &A->stall[s];
            if (st->block != b) continue;
            const int f = st->facing;
            const float line = f == 0 ? st->r.z0 : f == 2 ? st->r.z1 : f == 1 ? st->r.x1 : st->r.x0;
            float sum = 0.0f, a0 = 1e30f, a1 = -1e30f;
            for (int t = 0; t < A->nstall; ++t) {
                const AStall *o = &A->stall[t];
                if (o->block != b || o->facing != f) continue;
                const float ol = f == 0 ? o->r.z0 : f == 2 ? o->r.z1 : f == 1 ? o->r.x1 : o->r.x0;
                if (fabsf(ol - line) > 1e-3f) continue;
                const float oa0 = f == 0 || f == 2 ? o->r.x0 : o->r.z0, oa1 = f == 0 || f == 2 ? o->r.x1 : o->r.z1;
                sum += oa1 - oa0; a0 = minf(a0, oa0); a1 = maxf(a1, oa1);
            }
            if (sum > bsum + 1e-3f) { bsum = sum; bf = f; bline = line; ba0 = a0; ba1 = a1; }
        }
        if (bf < 0 || ba1 - ba0 < HALL_MOUTH) { alog(G, "  info: the %s block %d keeps shut: no mouth of %.0f m\n", arcology_use_name(B->use), b, HALL_MOUTH); continue; }
        /* its floor: the block's mass and the mouth's units */
        int cells = 0;
        const int N = A->nx * A->nz;
        for (int c = 0; c < N; ++c) cells += A->blk[c] == b && A->use[c] == AU_BLOCK;
        int keep = 0;
        for (int s = 0; s < A->nstall; ++s) {
            AStall *st = &A->stall[s];
            const float ol = st->facing == 0 ? st->r.z0 : st->facing == 2 ? st->r.z1 : st->facing == 1 ? st->r.x1 : st->r.x0;
            if (st->block == b && st->facing == bf && fabsf(ol - bline) < 1e-3f)
                cells += (int)lrintf((st->r.x1 - st->r.x0) * (st->r.z1 - st->r.z0) / (CELL * CELL));
        }
        if ((float)cells * CELL * CELL < HALL_AREA) { alog(G, "  info: the %s block %d keeps shut: %.0f m2 of floor\n", arcology_use_name(B->use), b, (float)cells * CELL * CELL); continue; }
        /* open it */
        for (int s = 0; s < A->nstall; ++s) {
            AStall *st = &A->stall[s];
            const float ol = st->facing == 0 ? st->r.z0 : st->facing == 2 ? st->r.z1 : st->facing == 1 ? st->r.x1 : st->r.x0;
            if (st->block == b && st->facing == bf && fabsf(ol - bline) < 1e-3f) { mark(A, st->r, BIT(AU_STALL), AU_FOOD); continue; }
            A->stall[keep++] = *st;
        }
        A->nstall = keep;
        ARect box = rect(1e30f, 1e30f, -1e30f, -1e30f);
        for (int c = 0; c < N; ++c) {
            if (A->blk[c] != b) continue;
            if (A->use[c] == AU_BLOCK) A->use[c] = AU_FOOD;
            if (A->use[c] != AU_FOOD) continue;
            const int i = c % A->nx, j = c / A->nx;
            box.x0 = minf(box.x0, A->gx0 + (float)i * CELL); box.x1 = maxf(box.x1, A->gx0 + (float)(i + 1) * CELL);
            box.z0 = minf(box.z0, A->gz0 + (float)j * CELL); box.z1 = maxf(box.z1, A->gz0 + (float)(j + 1) * CELL);
        }
        AHall *H = &A->fhall[A->nfhall++];
        memset(H, 0, sizeof(*H));
        H->block = (uint8_t)b; H->use = B->use; H->facing = (uint8_t)bf;
        H->seating = B->use == AB_NOODLE ? AH_ROUND : AH_LONG;
        H->line = bline; H->m0 = ba0; H->m1 = ba1;
        H->roof = B->height;
        H->head = clampf(B->height - 0.9f, 3.2f, minf(3.8f, B->height - 0.4f));
        H->box = box;
        H->seed = hash_combine(A->seed, 0xF00D0000u + (uint32_t)b);
        /* the piers: at the mouth's ends and no more than 4.5 m apart between */
        const float len = ba1 - ba0;
        const int spans = clampi((int)ceilf((len - AR_HALL_PIER) / 4.5f), 1, AR_HALL_PIERS - 1);
        for (int k = 0; k <= spans; ++k) H->pier[H->npier++] = ba0 + 0.5f * AR_HALL_PIER + (len - AR_HALL_PIER) * (float)k / (float)spans;
        hall_bars(A, H, b);
        hall_seats(A, H, b);
        hall_battens(A, H, b);
        alog(G, "  info: the %s block %d opens as a hall: %.0f m2, a %.1f m mouth, %d bars, %d %s tables, %d fans, %d battens\n",
             arcology_use_name(B->use), b, (float)cells * CELL * CELL, len, H->nbar, H->nseat, H->seating == AH_LONG ? "long" : "hawker", H->nfan, H->nbatten);
    }
}

/* SF5 (street vendors, ~/.claude/plans/street-vendors.md "SF5: the design"): the food uses' units that a kit bar
 * takes (SF3's recessed straight form: its back on the block, its counter on the unit's front, its hood out over the
 * colonnade). A unit at least 3 m across (the bar's 2.88 between its jambs) and 2.5 deep, its head (to AR_BAR_HEAD)
 * 0.3 m under the block's top and high enough for the arcology's sign on the bar's header panel (AR_BAR_BOARD_MIN
 * tall over the hood: place_signage), and the way in front of it public and clear of columns: 1.1 m (stools under the hood) or,
 * failing that, 0.55 m (a standing counter). Its own hashes, so nothing else of the layout moves. */
#define FOOD_W    3.0f
#define FOOD_OUT  1.1f
#define FOOD_SLIM 0.55f
#define FOOD_WAY  0.5f    /* the way kept clear past a bar's stools (src/city/vendor_sites.c: its site's walk) */
static ARect food_front(const AStall *st, float out) {
    const ARect r = st->r;
    switch (st->facing) {
    case 0:  return rect(r.x0, r.z0 - out, r.x1, r.z0);
    case 2:  return rect(r.x0, r.z1, r.x1, r.z1 + out);
    case 1:  return rect(r.x1, r.z0, r.x1 + out, r.z1);
    default: return rect(r.x0 - out, r.z0, r.x0, r.z1);
    }
}
static int food_front_clear(const ALayout *A, const AStall *st, float out) {
    const ARect f = food_front(st, out);
    /* clear of the bars taken before it (two units at an inside corner of a block face into one corner) */
    for (int s = 0; s < (int)(st - A->stall); ++s)
        if (A->stall[s].vendor && overlap(expand(f, 0.3f), food_front(&A->stall[s], A->stall[s].out))) return 0;
    if (!all_cells(A, f, PUBLIC)) return 0;
    /* nor by the facade (its wall stands 0.3 m into the building) */
    if (!rect_inside(&A->tier[0].foot, f, 0.45f)) return 0;
    /* the way past its stools (FOOD_WAY) public and clear of columns too */
    const ARect way = food_front(st, out + FOOD_WAY);
    if (!all_cells(A, way, PUBLIC)) return 0;
    for (int c = 0; c < A->ncol; ++c) {
        const float h = 0.575f * A->col[c].size + 0.1f;   /* (the walker meets a column 15% larger than it's drawn) */
        if (overlap(way, rect(A->col[c].x - h, A->col[c].z - h, A->col[c].x + h, A->col[c].z + h))) return 0;
    }
    return 1;
}
void place_food(Gen *G) {
    ALayout *A = G->A;
    int n = 0, food = 0, narrow = 0, low = 0, shut = 0, slim = 0;
    for (int s = 0; s < A->nstall; ++s) {
        AStall *st = &A->stall[s];
        st->vendor = 0; st->head = 3.2f; st->out = 0.0f;
        if (!arcology_food(st->kind)) continue;
        food++;
        float bx, bz, fx, fz, w, d;
        arcology_stall_frame(st, &bx, &bz, &fx, &fz, &w, &d);
        const float top = A->block[st->block].height;
        const float head = clampf(top - 0.3f, 3.2f, AR_BAR_HEAD);
        if (w < FOOD_W - 1e-3f || d < 2.5f - 1e-3f) { narrow++; continue; }
        if (head < AR_BAR_BOARD + 0.12f + AR_BAR_BOARD_MIN || head > top - 0.3f) { low++; continue; }
        const float out = food_front_clear(A, st, FOOD_OUT) ? FOOD_OUT : food_front_clear(A, st, FOOD_SLIM) ? FOOD_SLIM : 0.0f;
        if (out <= 0.0f) { shut++; continue; }
        slim += out < FOOD_OUT;
        st->vendor = 1; st->head = head; st->out = out; st->closed = 0;   /* (shut by the bar's hours) */
        n++;
    }
    alog(G, "  info: %d of %d stalls kit bars (%d food: %d too narrow, %d too low, %d with no way in front; %d standing counters)\n",
         n, A->nstall, food, narrow, low, shut, slim);
}

/* 5. Galleries: one per floor the canyon passes. Bridges: at least one per level, staggered. */
void place_galleries(Gen *G) {
    ALayout *A = G->A;
    A->nlevel = 0;
    if (A->canyon == AC_PLAZA) return;
    for (int k = 0; A->nlevel < AR_MAX_LEVEL; ++k) {
        float y = A->ceilY + (float)k * A->floorH;
        if (y > A->crownY - A->floorH * 0.6f) break;
        A->levelY[A->nlevel++] = y;
    }
    ARect s = A->spine;
    for (int k = 0; k < A->nlevel && A->nbridge < AR_MAX_BRIDGE; ++k) {
        float f = 0.25f + 0.5f * hash_float(hash_combine(A->seed ^ 0xB81Du, (uint32_t)k));
        ABridge *b = &A->bridge[A->nbridge];
        b->level = k;
        if (A->canyon == AC_SPINE) {
            ARect v = A->voids[0];
            int side = k & 3;
            if (side == 0 || side == 2) {
                float x = s.x0 + 1.5f + (s.x1 - s.x0 - 3.0f) * f;
                b->r = side == 0 ? rect(x - 1.5f, v.z0, x + 1.5f, s.z0) : rect(x - 1.5f, s.z1, x + 1.5f, v.z1);
                b->axis = 1;
            } else {
                float z = s.z0 + 1.5f + (s.z1 - s.z0 - 3.0f) * f;
                b->r = side == 1 ? rect(s.x1, z - 1.5f, v.x1, z + 1.5f) : rect(v.x0, z - 1.5f, s.x0, z + 1.5f);
                b->axis = 0;
            }
        } else {
            ARect v = A->voids[A->nvoid == 2 ? (k & 1) : 0];
            float w = v.x1 - v.x0, d = v.z1 - v.z0;
            if (w <= d) {        /* span across x, at a staggered z */
                float z = v.z0 + 1.5f + (d - 3.0f) * f;
                b->r = rect(v.x0, z - 1.5f, v.x1, z + 1.5f);
                b->axis = 0;
            } else {
                float x = v.x0 + 1.5f + (w - 3.0f) * f;
                b->r = rect(x - 1.5f, v.z0, x + 1.5f, v.z1);
                b->axis = 1;
            }
        }
        float span = b->axis ? b->r.z1 - b->r.z0 : b->r.x1 - b->r.x0;
        if (span > 24.0f || span < 1.0f) continue;
        A->nbridge++;
    }
    alog(G, "  info: %d gallery levels, %d bridges\n", A->nlevel, A->nbridge);
}
