#include "arcology_internal.h"

/* ===========================================================================
 * Step 6: fixtures, then the style layers (signage, retrofit, light)
 * ========================================================================= */

static AFix *add_fix(ALayout *A, int type, int facing, float x0, float z0, float x1, float z1, float y0, float y1, uint32_t seed) {
    if (A->nfix >= AR_MAX_FIX) return NULL;
    AFix *f = &A->fix[A->nfix++];
    f->type = (uint8_t)type; f->facing = (uint8_t)facing;
    f->x0 = x0; f->z0 = z0; f->x1 = x1; f->z1 = z1; f->y0 = y0; f->y1 = y1; f->seed = seed;
    return f;
}

static ASign *add_sign(ALayout *A, int kind, float x0, float z0, float x1, float z1, float y0, float y1, int nx, int nz, Rng *r) {
    if (A->nsign >= AR_MAX_SIGN) return NULL;
    ASign *s = &A->sign[A->nsign++];
    s->kind = (uint8_t)kind;
    s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1; s->y0 = y0; s->y1 = y1;
    s->nx = (int8_t)nx; s->nz = (int8_t)nz;
    s->color = rng_float(r) < 0.7f ? A->palette[rng_int(r, 0, 2)] : (uint8_t)rng_int(r, 0, 7);
    s->style = (uint8_t)(rng_float(r) < 0.5f ? 1 : 0);
    float f = rng_float(r);
    s->flick = (uint8_t)(f < 0.08f ? 1 : f < 0.12f ? 2 : 0);
    s->seed = rng_u32(r);
    return s;
}

/* Sign density at stratum height t: full in the street stratum, none at the corporate. */
static float sign_density(const ALayout *A, float t) { return A->signDensity * clampf(1.0f - t / A->stratumEdge[2], 0.0f, 1.0f); }
static float retrofit_density(const ALayout *A, float t) { return A->retrofit * clampf(1.0f - t / A->stratumEdge[1], 0.0f, 1.0f); }

/* The sides of a void that carry a gallery: (side, line, a0, a1); none against the spine. */
static int void_sides(const ALayout *A, int v, float (*out)[4]) {
    ARect r = A->voids[v];
    int n = 0;
    for (int side = 0; side < 4; ++side) {
        float line = side == 0 ? r.z0 : side == 1 ? r.x1 : side == 2 ? r.z1 : r.x0;
        float a0 = (side == 0 || side == 2) ? r.x0 : r.z0, a1 = (side == 0 || side == 2) ? r.x1 : r.z1;
        float mx = (side == 0 || side == 2) ? 0.5f * (a0 + a1) : line, mz = (side == 0 || side == 2) ? line : 0.5f * (a0 + a1);
        if (A->canyon != AC_SPINE && in_rect(expand(A->spine, 0.1f), mx, mz)) continue;
        out[n][0] = (float)side; out[n][1] = line; out[n][2] = a0; out[n][3] = a1;
        n++;
    }
    return n;
}

void place_fixtures(Gen *G) {
    ALayout *A = G->A;
    float fy = A->floorY;
    /* express hall: gate lanes by stratum, a checkpoint booth, an escort gate */
    {
        ARect h = A->hall;
        float z = h.z0 + 0.3f;
        for (float x = h.x0 + 2.6f; x + 0.25f < h.x1 - 1.0f; x += 1.1f)
            add_fix(A, AX_GATE, 2, x, z, x + 0.25f, z + 1.1f, 0.0f, 1.0f, (uint32_t)(x * 10.0f));
        add_fix(A, AX_BOOTH, 0, h.x0 + 0.3f, z, h.x0 + 2.3f, z + 2.0f, 0.0f, 2.4f, A->seed);
        float lane = (h.x1 - h.x0 - 3.0f) / 4.0f;
        for (int s = 0; s < 4; ++s) {
            ASign *sg = add_sign(A, AG_HALL, h.x0 + 2.6f + lane * (float)s, z - 0.05f, h.x0 + 2.6f + lane * (float)(s + 1) - 0.2f, z - 0.05f,
                                 fy + 3.0f, fy + 3.6f, 0, -1, &G->rn);
            if (sg) { sg->color = (uint8_t)(s == 0 ? 3 : s == 1 ? 2 : s == 2 ? 5 : 1); sg->style = 1; sg->flick = 0; }
        }
    }
    /* stall counters, awnings over market stalls, vents and steam on their roofs */
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        ARect r = st->r;
        int d = st->facing;
        float c0, c1;                                   /* the counter: 0.6 m deep at the front */
        if (d == 0)      { c0 = r.z0 + 0.1f; c1 = c0 + 0.6f; add_fix(A, AX_COUNTER, d, r.x0 + 0.4f, c0, r.x1 - 0.9f, c1, 0.0f, 1.05f, st->seed); }
        else if (d == 2) { c1 = r.z1 - 0.1f; c0 = c1 - 0.6f; add_fix(A, AX_COUNTER, d, r.x0 + 0.4f, c0, r.x1 - 0.9f, c1, 0.0f, 1.05f, st->seed); }
        else if (d == 1) { c1 = r.x1 - 0.1f; c0 = c1 - 0.6f; add_fix(A, AX_COUNTER, d, c0, r.z0 + 0.4f, c1, r.z1 - 0.9f, 0.0f, 1.05f, st->seed); }
        else             { c0 = r.x0 + 0.1f; c1 = c0 + 0.6f; add_fix(A, AX_COUNTER, d, c0, r.z0 + 0.4f, c1, r.z1 - 0.9f, 0.0f, 1.05f, st->seed); }
        if (st->kind == AB_MARKET || st->kind == AB_NOODLE) {
            float o = 1.1f;                              /* over the colonnade, never the concourse */
            if (d == 0)      add_fix(A, AX_AWNING, d, r.x0, r.z0 - o, r.x1, r.z0, 2.75f, 3.2f, st->seed);
            else if (d == 2) add_fix(A, AX_AWNING, d, r.x0, r.z1, r.x1, r.z1 + o, 2.75f, 3.2f, st->seed);
            else if (d == 1) add_fix(A, AX_AWNING, d, r.x1, r.z0, r.x1 + o, r.z1, 2.75f, 3.2f, st->seed);
            else             add_fix(A, AX_AWNING, d, r.x0 - o, r.z0, r.x0, r.z1, 2.75f, 3.2f, st->seed);
        }
        if ((st->kind == AB_NOODLE || st->kind == AB_BATH || rng_float(&G->rr) < 0.25f * A->retrofit) && !st->closed) {
            float vx = 0.5f * (r.x0 + r.x1) - (float)DX[d] * 1.2f, vz = 0.5f * (r.z0 + r.z1) - (float)DZ[d] * 1.2f;
            add_fix(A, AX_VENT, d, vx - 0.35f, vz - 0.35f, vx + 0.35f, vz + 0.35f, 3.3f, 4.0f, st->seed);
            add_fix(A, AX_STEAM, d, vx - 0.6f, vz - 0.6f, vx + 0.6f, vz + 0.6f, 4.0f, 4.0f + rng_range(&G->rr, 2.5f, 4.5f), st->seed ^ 0x57EAu);
        }
    }
    /* kiosks along kiosk strips */
    for (int b = 0; b < A->nblock; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use != AB_KIOSK) continue;
        int alongX = B->box.x1 - B->box.x0 >= B->box.z1 - B->box.z0;
        float a0 = alongX ? B->box.x0 : B->box.z0, a1 = alongX ? B->box.x1 : B->box.z1;
        float m0 = alongX ? B->box.z0 : B->box.x0, m1 = alongX ? B->box.z1 : B->box.x1, mid = 0.5f * (m0 + m1);
        for (float a = a0 + 0.5f; a + 2.2f <= a1 - 0.5f; a += 3.6f) {
            ARect k = alongX ? rect(a, mid - 1.1f, a + 2.2f, mid + 1.1f) : rect(mid - 1.1f, a, mid + 1.1f, a + 2.2f);
            if (!all_cells(A, k, BIT(AU_KIOSK))) continue;
            add_fix(A, AX_KIOSK, alongX ? 0 : 3, k.x0, k.z0, k.x1, k.z1, 0.0f, 2.6f, rng_u32(&G->rt));
        }
    }
    /* clutter on the block roofs, crates and bins in the yard */
    for (int b = 0; b < A->nblock; ++b) {
        const ABlock *B = &A->block[b];
        if (B->use == AB_KIOSK) continue;
        int yard = B->use == AB_YARD;
        int n = clampi((int)((float)B->cells * CELL * CELL / (yard ? 25.0f : 45.0f)), 1, yard ? 14 : 9);
        for (int k = 0, tries = 0; k < n && tries < n * 8; ++tries) {
            float sx = rng_range(&G->rb, 1.0f, 2.6f), sz = rng_range(&G->rb, 1.0f, 2.6f);
            float x = rng_range(&G->rb, B->box.x0 + 0.5f, B->box.x1 - 0.5f - sx), z = rng_range(&G->rb, B->box.z0 + 0.5f, B->box.z1 - 0.5f - sz);
            ARect r = rect(x, z, x + sx, z + sz);
            if (!all_cells(A, expand(r, 0.25f), BIT(yard ? AU_YARD : AU_BLOCK))) continue;
            int i0, j0, i1, j1, own = 1;
            cells_of(A, r, &i0, &j0, &i1, &j1);
            for (int j = j0; j < j1 && own; ++j) for (int i = i0; i < i1 && own; ++i) own = A->blk[IDX(A, i, j)] == b;
            if (!own) continue;
            float pick = rng_float(&G->rb);
            if (yard) {
                float h = rng_range(&G->rb, 0.6f, 1.8f);
                add_fix(A, pick < 0.6f ? AX_CRATE : AX_BIN, 0, x, z, x + sx * 0.7f, z + sz * 0.7f, 0.0f, h, rng_u32(&G->rb));
            }
            else {
                int type = pick < 0.4f ? AX_PLANT : pick < 0.6f ? AX_TANK : pick < 0.85f ? AX_CRATE : AX_VENT;
                float y0 = B->height, h = type == AX_TANK ? rng_range(&G->rb, 1.6f, 2.6f) : rng_range(&G->rb, 0.6f, 1.6f);
                add_fix(A, type, 0, x, z, x + sx, z + sz, y0, y0 + h, rng_u32(&G->rb));
            }
            k++;
        }
    }
    /* the canyon floor: benches facing in, puddles where the crown drips */
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        float sides[4][4];
        int ns = void_sides(A, v, sides);
        for (int s = 0; s < ns; ++s) {
            int side = (int)sides[s][0];
            float line = sides[s][1];
            for (float a = sides[s][2] + 3.0f; a + 2.0f < sides[s][3] - 3.0f; a += 6.5f) {
                ARect bch;
                float in = 1.6f;
                if (side == 0)      bch = rect(a, line + in, a + 2.0f, line + in + 0.6f);
                else if (side == 2) bch = rect(a, line - in - 0.6f, a + 2.0f, line - in);
                else if (side == 1) bch = rect(line - in - 0.6f, a, line - in, a + 2.0f);
                else                bch = rect(line + in, a, line + in + 0.6f, a + 2.0f);
                int onStreet = 0;
                for (int c = 0; c < A->nconc && !onStreet; ++c) onStreet = overlap(expand(bch, 0.8f), A->conc[c].r);
                if (onStreet || !all_cells(A, expand(bch, 0.3f), BIT(AU_PLAZA))) continue;
                add_fix(A, AX_BENCH, (side + 2) & 3, bch.x0, bch.z0, bch.x1, bch.z1, 0.0f, 0.45f, rng_u32(&G->rt));
            }
        }
        int np = rng_int(&G->rr, 5, 10);
        for (int k = 0, tries = 0; k < np && tries < 40; ++tries) {
            float rad = rng_range(&G->rr, 0.6f, 2.2f);
            float x = rng_range(&G->rr, r.x0 + 2.0f + rad, r.x1 - 2.0f - rad), z = rng_range(&G->rr, r.z0 + 2.0f + rad, r.z1 - 2.0f - rad);
            if (!all_cells(A, rect(x - rad, z - rad, x + rad, z + rad), BIT(AU_PLAZA) | BIT(AU_CONCOURSE))) continue;
            add_fix(A, AX_PUDDLE, 0, x - rad, z - rad, x + rad, z + rad, 0.0f, 0.0f, rng_u32(&G->rr));
            k++;
        }
    }
}

void place_signage(Gen *G) {
    ALayout *A = G->A;
    float fy = A->floorY, sd0 = sign_density(A, 0.0f);
    /* stall fascias, facing the street */
    for (int s = 0; s < A->nstall; ++s) {
        const AStall *st = &A->stall[s];
        if (rng_float(&G->rn) > sd0 + 0.1f) continue;
        ARect r = st->r;
        int d = st->facing;
        float y0 = fy + 3.35f, y1 = fy + 3.35f + rng_range(&G->rn, 0.7f, 1.1f);
        if (d == 0)      add_sign(A, AG_FASCIA, r.x0 + 0.15f, r.z0 - 0.08f, r.x1 - 0.15f, r.z0 - 0.08f, y0, y1, 0, -1, &G->rn);
        else if (d == 2) add_sign(A, AG_FASCIA, r.x0 + 0.15f, r.z1 + 0.08f, r.x1 - 0.15f, r.z1 + 0.08f, y0, y1, 0, 1, &G->rn);
        else if (d == 1) add_sign(A, AG_FASCIA, r.x1 + 0.08f, r.z0 + 0.15f, r.x1 + 0.08f, r.z1 - 0.15f, y0, y1, 1, 0, &G->rn);
        else             add_sign(A, AG_FASCIA, r.x0 - 0.08f, r.z0 + 0.15f, r.x0 - 0.08f, r.z1 - 0.15f, y0, y1, -1, 0, &G->rn);
    }
    /* blade signs off column faces over the concourses, above the clear height */
    float top = A->ceilY - 0.7f;
    for (int c = 0; c < A->ncol; ++c) {
        const AColumn *cl = &A->col[c];
        for (int d = 0; d < 4; ++d) {
            float h = cl->size * 0.5f;
            if (arcology_use_at(A, cl->x + (float)DX[d] * (h + 1.2f), cl->z + (float)DZ[d] * (h + 1.2f)) != AU_CONCOURSE) continue;
            if (rng_float(&G->rn) > sd0) continue;
            float y0 = fy + 5.6f + rng_range(&G->rn, 0.0f, 0.8f), y1 = minf(y0 + rng_range(&G->rn, 2.4f, 4.0f), top);
            if (y1 - y0 < 1.4f) continue;
            float depth = rng_range(&G->rn, 1.2f, 2.0f), a0 = h + 0.25f, a1 = a0 + depth;
            add_sign(A, AG_BLADE, cl->x + (float)DX[d] * a0, cl->z + (float)DZ[d] * a0, cl->x + (float)DX[d] * a1, cl->z + (float)DZ[d] * a1, y0, y1, 0, 0, &G->rn);
        }
    }
    /* banners on the gallery fascias, facing the canyon */
    for (int k = 0; k < A->nlevel; ++k) {
        float t = arcology_t(A, A->levelY[k]), dens = sign_density(A, t);
        if (dens <= 0.0f) continue;
        for (int v = 0; v < A->nvoid; ++v) {
            float sides[4][4];
            int ns = void_sides(A, v, sides);
            for (int s = 0; s < ns; ++s) {
                int side = (int)sides[s][0];
                float line = sides[s][1], a0 = sides[s][2], a1 = sides[s][3];
                int n = (int)(dens * (a1 - a0) / 8.0f + rng_float(&G->rn));
                for (int q = 0; q < n; ++q) {
                    float w = rng_range(&G->rn, 1.1f, 2.2f), hgt = rng_range(&G->rn, 1.8f, minf(3.2f, A->floorH * 0.85f));
                    float a = rng_range(&G->rn, a0 + 0.5f, a1 - 0.5f - w);
                    float y1 = A->levelY[k] - 0.45f, y0 = y1 - hgt;
                    float o = 0.25f;
                    ASign *sg;
                    if (side == 0)      sg = add_sign(A, AG_BANNER, a, line + o, a + w, line + o, y0, y1, 0, 1, &G->rn);
                    else if (side == 2) sg = add_sign(A, AG_BANNER, a, line - o, a + w, line - o, y0, y1, 0, -1, &G->rn);
                    else if (side == 1) sg = add_sign(A, AG_BANNER, line - o, a, line - o, a + w, y0, y1, -1, 0, &G->rn);
                    else                sg = add_sign(A, AG_BANNER, line + o, a, line + o, a + w, y0, y1, 1, 0, &G->rn);
                    if (sg && t > A->stratumEdge[1]) sg->style = 1;
                }
            }
        }
    }
    /* giant screens on the spine core's faces, facing the canyon, in the lower strata */
    if (A->canyon != AC_PLAZA) {
        ARect s = A->spine;
        float limit = A->floorY;
        for (int k = 0; k < A->nlevel; ++k) if (arcology_t(A, A->levelY[k]) < A->stratumEdge[1]) limit = A->levelY[k];
        limit = minf(limit, A->crownY - 8.0f);
        for (int d = 0; d < 4; ++d) {
            float fx = d == 1 ? s.x1 : d == 3 ? s.x0 : 0.5f * (s.x0 + s.x1), fz = d == 0 ? s.z0 : d == 2 ? s.z1 : 0.5f * (s.z0 + s.z1);
            if (!in_void(A, fx + (float)DX[d] * 1.0f, fz + (float)DZ[d] * 1.0f, 0.0f)) continue;
            float fw = (d == 0 || d == 2) ? s.x1 - s.x0 : s.z1 - s.z0;
            float y = A->ceilY + rng_range(&G->rn, 2.0f, 8.0f);
            for (int n = 0; n < 4; ++n) {
                float h = rng_range(&G->rn, 10.0f, 22.0f), w = fw * rng_range(&G->rn, 0.6f, 0.85f);
                if (y + h > limit) break;
                float a = rng_range(&G->rn, 0.0f, fw - w), o = 0.25f;
                ASign *sg;
                if (d == 0)      sg = add_sign(A, AG_SCREEN, s.x0 + a, s.z0 - o, s.x0 + a + w, s.z0 - o, y, y + h, 0, -1, &G->rn);
                else if (d == 2) sg = add_sign(A, AG_SCREEN, s.x0 + a, s.z1 + o, s.x0 + a + w, s.z1 + o, y, y + h, 0, 1, &G->rn);
                else if (d == 1) sg = add_sign(A, AG_SCREEN, s.x1 + o, s.z0 + a, s.x1 + o, s.z0 + a + w, y, y + h, 1, 0, &G->rn);
                else             sg = add_sign(A, AG_SCREEN, s.x0 - o, s.z0 + a, s.x0 - o, s.z0 + a + w, y, y + h, -1, 0, &G->rn);
                (void)sg;
                y += h + rng_range(&G->rn, 6.0f, 16.0f);
            }
        }
    }
    alog(G, "  info: %d signs and screens\n", A->nsign);
}

/* Segments of a concourse's centre line (offset `off` across) that run over street, not the canyon floor. */
static int street_runs(const ALayout *A, const AConcourse *c, float off, float (*run)[2], int max) {
    float mid = (c->axis ? 0.5f * (c->r.x0 + c->r.x1) : 0.5f * (c->r.z0 + c->r.z1)) + off;
    float a0 = c->axis ? c->r.z0 : c->r.x0, a1 = c->axis ? c->r.z1 : c->r.x1;
    int n = 0, inRun = 0;
    float start = 0.0f;
    for (float a = a0; a <= a1 + 1e-3f; a += CELL) {
        int u = (a < a1) ? (c->axis ? arcology_use_at(A, mid, a + 0.25f) : arcology_use_at(A, a + 0.25f, mid)) : AU_OUT;
        int ok = u == AU_CONCOURSE || u == AU_COLONNADE;
        if (ok && !inRun) { inRun = 1; start = a; }
        if (!ok && inRun) {
            inRun = 0;
            if (a - start >= 3.0f && n < max) { run[n][0] = start; run[n][1] = a; n++; }
        }
    }
    return n;
}

void place_retrofit(Gen *G) {
    ALayout *A = G->A;
    float rd0 = retrofit_density(A, 0.0f);
    /* ducts along the concourse centre lines at ceiling height, lamps hung under them */
    for (int c = 0; c < A->nconc; ++c) {
        const AConcourse *cc = &A->conc[c];
        int lines = cc->boulevard ? 2 : 1;
        for (int l = 0; l < lines; ++l) {
            float off = cc->boulevard ? (l ? cc->lampOff : -cc->lampOff) : 0.0f;
            float run[16][2];
            int nr = street_runs(A, cc, off, run, 16);
            float mid = (cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1)) + off;
            float size = cc->rib ? 0.7f : rng_range(&G->rr, 1.0f, 1.5f);
            float y = A->ceilY - 0.5f - size;
            int duct = cc->main || rng_float(&G->rr) < rd0;
            for (int r = 0; r < nr; ++r) {
                if (duct && A->nduct < AR_MAX_DUCT) {
                    ADuct *dt = &A->duct[A->nduct++];
                    dt->kind = AD_DUCT; dt->size = size; dt->y = y;
                    if (cc->axis) { dt->x0 = dt->x1 = mid; dt->z0 = run[r][0]; dt->z1 = run[r][1]; }
                    else          { dt->z0 = dt->z1 = mid; dt->x0 = run[r][0]; dt->x1 = run[r][1]; }
                }
                float a0 = cc->axis ? cc->r.z0 : cc->r.x0;
                for (float a = a0 + cc->lampPhase; a < run[r][1]; a += cc->lampStep) {
                    if (a < run[r][0] + 0.5f || A->nlamp >= AR_MAX_LAMP) continue;
                    ALamp *lp = &A->lamp[A->nlamp++];
                    lp->x = cc->axis ? mid : a; lp->z = cc->axis ? a : mid; lp->y = A->floorY + 5.0f;
                }
            }
        }
        /* cable trays along the colonnades either side */
        if (cc->rib || rng_float(&G->rr) > rd0) continue;
        float h = A->colSize * 0.5f + 0.5f;
        float mid = cc->axis ? 0.5f * (cc->r.x0 + cc->r.x1) : 0.5f * (cc->r.z0 + cc->r.z1);
        for (int side = 0; side < 2; ++side) {
            float line = cc->axis ? (side ? cc->r.x1 + h : cc->r.x0 - h) : (side ? cc->r.z1 + h : cc->r.z0 - h);
            float run[16][2];
            int nr = street_runs(A, cc, line - mid, run, 16);
            for (int r = 0; r < nr && A->nduct < AR_MAX_DUCT; ++r) {
                ADuct *dt = &A->duct[A->nduct++];
                dt->kind = AD_TRAY; dt->size = 0.6f; dt->y = A->ceilY - 2.0f;
                if (cc->axis) { dt->x0 = dt->x1 = line; dt->z0 = run[r][0]; dt->z1 = run[r][1]; }
                else          { dt->z0 = dt->z1 = line; dt->x0 = run[r][0]; dt->x1 = run[r][1]; }
            }
        }
    }
    /* pipes under the gallery soffits in the lower strata */
    for (int k = 0; k + 1 < A->nlevel; ++k) {
        float t = arcology_t(A, A->levelY[k]), rd = retrofit_density(A, t);
        if (rd <= 0.0f) continue;
        for (int v = 0; v < A->nvoid; ++v) {
            float sides[4][4];
            int ns = void_sides(A, v, sides);
            for (int s = 0; s < ns && A->nduct < AR_MAX_DUCT; ++s) {
                if (rng_float(&G->rr) > rd * 0.5f) continue;
                int side = (int)sides[s][0];
                float back = sides[s][1] + ((side == 0 || side == 3) ? -1.0f : 1.0f) * (A->galleryD - 0.9f);
                ADuct *dt = &A->duct[A->nduct++];
                dt->kind = AD_PIPE; dt->size = rng_range(&G->rr, 0.2f, 0.4f);
                dt->y = A->levelY[k + 1] - 0.45f - dt->size;
                if (side == 0 || side == 2) { dt->z0 = dt->z1 = back; dt->x0 = sides[s][2] - A->galleryD + 0.5f; dt->x1 = sides[s][3] + A->galleryD - 0.5f; }
                else                        { dt->x0 = dt->x1 = back; dt->z0 = sides[s][2] - A->galleryD + 0.5f; dt->z1 = sides[s][3] + A->galleryD - 0.5f; }
            }
        }
    }
}

/* Real lights within the budget: cold light from the crown at the canyon floor's
 * corners and sodium inside the portals. Emissive surfaces carry the rest. */
void place_light(Gen *G) {
    ALayout *A = G->A;
    if (A->nvoid) {
        ARect v = A->voids[0];
        if (A->nvoid == 2) v = rect(minf(A->voids[0].x0, A->voids[1].x0), minf(A->voids[0].z0, A->voids[1].z0),
                                    maxf(A->voids[0].x1, A->voids[1].x1), maxf(A->voids[0].z1, A->voids[1].z1));
        for (int k = 0; k < 4 && A->nlight < AR_MAX_LIGHT; ++k) {
            ALight *l = &A->light[A->nlight++];
            l->type = AL_COLD;                         /* high in the canyon: a cold wash over the floor */
            l->x = (k & 1) ? v.x1 - 3.0f : v.x0 + 3.0f;
            l->z = (k & 2) ? v.z1 - 3.0f : v.z0 + 3.0f;
            l->y = A->ceilY + 11.0f;
        }
    } else {
        ARect a = expand(A->spine, G->apron * 0.5f);
        for (int k = 0; k < 2 && A->nlight < AR_MAX_LIGHT; ++k) {
            ALight *l = &A->light[A->nlight++];
            l->type = AL_COLD; l->x = k ? a.x1 : a.x0; l->z = a.z0; l->y = A->ceilY - 1.5f;
        }
    }
    for (int pass = 0; pass < 2; ++pass)
        for (int p = 0; p < A->nportal && A->nlight < AR_MAX_LIGHT; ++p) {
            const APortal *pt = &A->portal[p];
            if ((pass == 0) != (pt->main != 0) || pt->service) continue;
            ALight *l = &A->light[A->nlight++];
            l->type = AL_SODIUM; l->x = pt->fx - pt->nx * 4.0f; l->z = pt->fz - pt->nz * 4.0f; l->y = A->ceilY - 1.5f;
        }
    /* cold shafts down from the crown, searchlights sweeping the canyon */
    for (int v = 0; v < A->nvoid; ++v) {
        ARect r = A->voids[v];
        int n = rng_int(&G->rli, 2, 3);
        for (int k = 0, tries = 0; k < n && tries < 20 && A->nshaft < AR_MAX_SHAFT; ++tries) {
            float w = rng_range(&G->rli, 2.0f, 4.5f), d = rng_range(&G->rli, 2.0f, 4.5f);
            float x = rng_range(&G->rli, r.x0 + 0.8f, r.x1 - 0.8f - w), z = rng_range(&G->rli, r.z0 + 0.8f, r.z1 - 0.8f - d);
            ARect sr = rect(x, z, x + w, z + d);
            if (overlap(expand(sr, 0.5f), A->spine)) continue;
            AShaft *s = &A->shaft[A->nshaft++];
            s->r = sr; s->y0 = A->floorY; s->y1 = A->crownY - 0.5f;
            k++;
        }
    }
    if (A->nvoid) {
        /* searchlights swing along the canyon's long open run, never into its walls */
        int n = rng_int(&G->rli, 1, 2);
        for (int k = 0; k < n && A->nsearch < AR_MAX_SEARCH; ++k) {
            ARect v = A->voids[A->nvoid == 2 ? k : 0], s = A->spine;
            ASearch *sl = &A->search[A->nsearch++];
            float span;
            if (A->canyon == AC_SPINE) {           /* in the ring either side of the spine, swinging along it */
                sl->x = 0.5f * (v.x0 + v.x1);
                sl->z = k ? 0.5f * (s.z1 + v.z1) : 0.5f * (v.z0 + s.z0);
                sl->az = 0.0f; span = v.x1 - v.x0;
            } else {
                int alongX = v.x1 - v.x0 >= v.z1 - v.z0;
                sl->x = 0.5f * (v.x0 + v.x1) + (alongX ? 0.0f : (k ? 1.0f : -1.0f) * (v.x1 - v.x0) * 0.2f * (A->nvoid == 1));
                sl->z = 0.5f * (v.z0 + v.z1) + (alongX ? (k ? 1.0f : -1.0f) * (v.z1 - v.z0) * 0.2f * (A->nvoid == 1) : 0.0f);
                sl->az = alongX ? 0.0f : 1.5707963f; span = alongX ? v.x1 - v.x0 : v.z1 - v.z0;
            }
            sl->y = A->crownY - 3.0f;
            sl->len = minf(A->crownY - A->floorY - 8.0f, rng_range(&G->rli, 60.0f, 110.0f));
            sl->amp = minf(atanf(span * 0.42f / sl->len), 0.6f);
            sl->speed = rng_range(&G->rli, 0.12f, 0.3f);
            sl->phase = rng_range(&G->rli, 0.0f, 6.2831853f);
        }
    }
}
