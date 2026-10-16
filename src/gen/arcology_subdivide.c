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
