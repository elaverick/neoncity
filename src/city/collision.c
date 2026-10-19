#include "city_internal.h"

/* ===========================================================================
 * Everything near one chunk
 * ========================================================================= */

static void collide_building(BlockLayout *L, const Building *b, const BGraph *bg);

void city_layout(int cx, int cz, BlockLayout *L) {
    const double P = CHUNK_SIZE;
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, 25.0, 0.0, 0.0);
    memset(L, 0, sizeof(*L));
    L->cx = cx; L->cz = cz;
    L->density = plan_density((cx + 0.5) * P, (cz + 0.5) * P);
    for (int i = 0; i < g.nb && L->nbuild < MAX_BUILDINGS; ++i) {
        const Building *b = &g.bld[i];
        if (!chunk_gen_owns(&g, b->ox, b->oz)) continue;
        L->build[L->nbuild++] = *b;
        collide_building(L, b, chunk_gen_graph(&g, i));
    }
    for (int i = 0; i < g.nl && L->nlamps < MAX_LAMPS; ++i)
        if (chunk_gen_owns(&g, g.lamp[i].x, g.lamp[i].z)) L->lamp[L->nlamps++] = g.lamp[i];
    L->bounded = g.pa->bounded;
    for (int i = 0; i < g.pa->ntowns && L->nground < MAX_GROUND; ++i) {
        const Poly *gp = &g.pa->ground[i];
        double x0, z0, x1, z1;
        poly_bounds(gp, &x0, &z0, &x1, &z1);
        if (x1 < g.ox || x0 > g.ox + P || z1 < g.oz || z0 > g.oz + P) continue;
        CurbPoly *c = &L->ground[L->nground++];
        c->n = gp->n;
        for (int k = 0; k < c->n; ++k) { c->v[k][0] = gp->v[k].x; c->v[k][1] = gp->v[k].z; }
    }
    for (int i = 0; i < g.pa->nblocks && L->ncurbs < MAX_CURBS; ++i) {
        const Block *bk = &g.pa->blocks[i];
        if (bk->x1 < g.ox || bk->x0 > g.ox + P || bk->z1 < g.oz || bk->z0 > g.oz + P) continue;
        CurbPoly *c = &L->curb[L->ncurbs++];
        c->n = bk->curb.n;
        for (int k = 0; k < c->n; ++k) { c->v[k][0] = bk->curb.v[k].x; c->v[k][1] = bk->curb.v[k].z; }
        DropKerb dk[MAX_BLOCK_DROPS];
        int nd = city_block_drops(g.pa, bk, dk, MAX_BLOCK_DROPS);
        for (int d = 0; d < nd && L->ndrops < MAX_DROPS; ++d) {
            double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
            for (int k = 0; k < dk[d].n; ++k) {
                x0 = fmin(x0, dk[d].v[k][0]); x1 = fmax(x1, dk[d].v[k][0]);
                z0 = fmin(z0, dk[d].v[k][1]); z1 = fmax(z1, dk[d].v[k][1]);
            }
            if (x1 >= g.ox && x0 <= g.ox + P && z1 >= g.oz && z0 <= g.oz + P) L->drop[L->ndrops++] = dk[d];
        }
    }
    chunk_gen_free(&g);
}

/* ===========================================================================
 * Collision: what stands at street level
 * ========================================================================= */

static int coll_begin(BlockLayout *L, int n) {
    if (L->ncoll >= MAX_COLL || L->ncollv + n > MAX_COLL_VERTS) return 0;
    CollPoly *c = &L->coll[L->ncoll++];
    c->first = L->ncollv; c->n = n; c->holeFirst = 0; c->holeN = 0;
    return 1;
}

static void coll_vertex(BlockLayout *L, const Building *b, float x, float z) {
    DVec2 w = to_world(b, x, z);
    L->collv[L->ncollv][0] = w.x; L->collv[L->ncollv][1] = w.z; L->ncollv++;
}

/* A box in the building frame, given as a frame rectangle of an interior. */
static void coll_frame_box(BlockLayout *BL, const Building *b, const ILayout *L, float x0, float z0, float x1, float z1) {
    float ax, az, bx, bz;
    interior_to_local(L, x0, z0, &ax, &az);
    interior_to_local(L, x1, z1, &bx, &bz);
    if (!coll_begin(BL, 4)) return;
    float lx0 = minf(ax, bx), lx1 = maxf(ax, bx), lz0 = minf(az, bz), lz1 = maxf(az, bz);
    coll_vertex(BL, b, lx0, lz0); coll_vertex(BL, b, lx0, lz1); coll_vertex(BL, b, lx1, lz1); coll_vertex(BL, b, lx1, lz0);
}

/* A planned ground floor's envelope: solid 0.3 m in from the facade, open at its openings. */
static void collide_envelope(BlockLayout *BL, const Building *b, const BGraph *g, const Behind *bh) {
    const BSection *s = &g->sec[bh->si];
    const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
    for (int li = 0; li < 2; ++li)
        for (int k = 0; k < loops[li]->n; ++k) {
            EdgeInfo e = edge_of(loops[li], k);
            float op[8][3];
            int nop = face_openings(g, bh, li, k, op, 8);
            for (int i = 0; i < nop; ++i)
                for (int j = i; j > 0 && op[j][0] < op[j - 1][0]; --j) { float t[3]; memcpy(t, op[j], sizeof(t)); memcpy(op[j], op[j - 1], sizeof(t)); memcpy(op[j - 1], t, sizeof(t)); }
            float cur = 0.0f;
            for (int o = 0; o <= nop; ++o) {
                float u0 = cur, u1 = o < nop ? op[o][0] : e.len;
                if (o < nop) cur = maxf(cur, op[o][1]);
                if (u1 - u0 < 0.05f || !coll_begin(BL, 4)) continue;
                float ix = e.ez * 0.3f, iz = -e.ex * 0.3f;          /* 0.3 m in from the facade */
                coll_vertex(BL, b, e.ax + e.ex * u0, e.az + e.ez * u0);
                coll_vertex(BL, b, e.ax + e.ex * u1, e.az + e.ez * u1);
                coll_vertex(BL, b, e.ax + e.ex * u1 + ix, e.az + e.ez * u1 + iz);
                coll_vertex(BL, b, e.ax + e.ex * u0 + ix, e.az + e.ez * u0 + iz);
            }
        }
}

/* A box given as a frame rectangle of an arcology. */
static void coll_arc_box(BlockLayout *BL, const Building *b, const ALayout *A, ARect r) {
    float ax, az, bx, bz;
    arcology_to_local(A, r.x0, r.z0, &ax, &az);
    arcology_to_local(A, r.x1, r.z1, &bx, &bz);
    if (!coll_begin(BL, 4)) return;
    float lx0 = minf(ax, bx), lx1 = maxf(ax, bx), lz0 = minf(az, bz), lz1 = maxf(az, bz);
    coll_vertex(BL, b, lx0, lz0); coll_vertex(BL, b, lx0, lz1); coll_vertex(BL, b, lx1, lz1); coll_vertex(BL, b, lx1, lz0);
}

/* An arcology's concourse level is walked into through its portals: columns,
 * cores, stairs, block masses, stalls and street furniture are solid. */
static void collide_arcology(BlockLayout *BL, const Building *b, const BGraph *g, const ALayout *A) {
    Behind bh = { NULL, A, A->tier[0].section };
    collide_envelope(BL, b, g, &bh);
    for (int c = 0; c < A->ncol; ++c) {
        float h = A->col[c].size * 0.5f * 1.15f;
        coll_arc_box(BL, b, A, (ARect){ A->col[c].x - h, A->col[c].z - h, A->col[c].x + h, A->col[c].z + h });
    }
    coll_arc_box(BL, b, A, A->spine);
    for (int k = 0; k < A->ncore; ++k) coll_arc_box(BL, b, A, A->core[k].r);
    for (int k = 0; k < A->nstair; ++k) coll_arc_box(BL, b, A, A->stair[k].r);
    ARect *rc = (ARect *)malloc(sizeof(ARect) * 512);
    if (!rc) fatal("out of memory building collision");
    int n = arc_rects(A, 1u << AU_BLOCK, -1, rc, 512);
    for (int k = 0; k < n; ++k) coll_arc_box(BL, b, A, rc[k]);
    free(rc);
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        ARect r = st->r;
        if (st->closed) { coll_arc_box(BL, b, A, r); continue; }
        if (st->facing == 0 || st->facing == 2) {
            coll_arc_box(BL, b, A, (ARect){ r.x0, r.z0, r.x0 + 0.1f, r.z1 });
            coll_arc_box(BL, b, A, (ARect){ r.x1 - 0.1f, r.z0, r.x1, r.z1 });
        } else {
            coll_arc_box(BL, b, A, (ARect){ r.x0, r.z0, r.x1, r.z0 + 0.1f });
            coll_arc_box(BL, b, A, (ARect){ r.x0, r.z1 - 0.1f, r.x1, r.z1 });
        }
    }
    for (int f = 0; f < A->nfix; ++f) {
        const AFix *x = &A->fix[f];
        if (x->y0 > 0.5f || x->y1 < 0.3f || x->type == AX_PUDDLE || x->type == AX_AWNING || x->type == AX_STEAM) continue;
        coll_arc_box(BL, b, A, (ARect){ x->x0, x->z0, x->x1, x->z1 });
    }
}

/* A lobby is walked into: its envelope (open at the entrance), partitions,
 * cores and furniture are solid, the rest of the floor is not. */
static void collide_interior(BlockLayout *BL, const Building *b, const BGraph *g, const ILayout *L) {
    Behind bh = { L, NULL, L->section };
    collide_envelope(BL, b, g, &bh);
    IWall *w = (IWall *)malloc(sizeof(IWall) * 4096);
    int16_t (*rc)[4] = (int16_t (*)[4])malloc(sizeof(int16_t) * 4 * 512);
    if (!w || !rc) fatal("out of memory building collision");
    int n = interior_walls(L, w, 4096);
    for (int k = 0; k < n; ++k) {
        if (w[k].door) continue;
        float x0, z0, x1, z1;
        run_strip(&w[k], 0.1f, &x0, &z0, &x1, &z1);
        coll_frame_box(BL, b, L, x0, z0, x1, z1);
    }
    for (int r = 0; r < L->nreg; ++r) {
        if (L->reg[r].kind != IK_SOLID || !L->reg[r].cells) continue;
        int nr = region_rects(L, r, 0, rc, 512);
        for (int q = 0; q < nr; ++q)
            coll_frame_box(BL, b, L, L->gx0 + rc[q][0] * IN_CELL, L->gz0 + rc[q][1] * IN_CELL, L->gx0 + rc[q][2] * IN_CELL, L->gz0 + rc[q][3] * IN_CELL);
    }
    for (int f = 0; f < L->nfix; ++f)
        if (L->fix[f].y1 > 0.3f) coll_frame_box(BL, b, L, L->fix[f].x0, L->fix[f].z0, L->fix[f].x1, L->fix[f].z1);
    free(w);
    free(rc);
}

/* Ground floors, piers, cores and columns are solid; a planned lobby is walked into. */
static void collide_building(BlockLayout *L, const Building *b, const BGraph *bg) {
    Interiors in;
    interiors_plan(b, bg, &in);
    for (int si = 0; si < bg->nsec; ++si) {
        const BSection *s = &bg->sec[si];
        if (s->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
        const ILayout *il = interior_of(&in, si);
        if (il) { collide_interior(L, b, bg, il); continue; }
        if (in.arc && in.arc->tier[0].section == si) { collide_arcology(L, b, bg, in.arc); continue; }
        const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
        if (!coll_begin(L, loops[0]->n + loops[1]->n)) break;
        CollPoly *c = &L->coll[L->ncoll - 1];
        c->n = loops[0]->n;
        for (int k = 0; k < loops[0]->n; ++k) coll_vertex(L, b, loops[0]->x[k], loops[0]->z[k]);
        c->holeFirst = L->ncollv; c->holeN = loops[1]->n;
        for (int k = 0; k < loops[1]->n; ++k) coll_vertex(L, b, loops[1]->x[k], loops[1]->z[k]);
    }
    interiors_free(&in);
    for (int k = 0; k < bg->nsup; ++k) {
        const BSupport *sp = &bg->sup[k];
        if (sp->type != SUP_COLUMN || sp->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
        if (!coll_begin(L, 4)) break;
        float h = sp->size * 0.5f;
        float cx[4] = { -h, -h, h, h }, cz[4] = { -h, h, h, -h };   /* negative turn, like every outer loop */
        for (int q = 0; q < 4; ++q) coll_vertex(L, b, sp->x0 + cx[q], sp->z0 + cz[q]);
    }
}
