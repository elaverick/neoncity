#include "city_internal.h"

/* ===========================================================================
 * Buildings: emitting a validated building graph (src/gen/building.c) in the
 * building's own frame
 * ========================================================================= */

EdgeInfo edge_of(const BLoop *l, int k) {
    int k1 = (k + 1) % l->n;
    EdgeInfo e;
    e.ax = l->x[k]; e.az = l->z[k];
    float dx = l->x[k1] - l->x[k], dz = l->z[k1] - l->z[k];
    e.len = sqrtf(dx * dx + dz * dz);
    e.ex = e.len > 1e-6f ? dx / e.len : 1.0f;
    e.ez = e.len > 1e-6f ? dz / e.len : 0.0f;
    return e;
}

/* Face frame of edge k: x along the face (the shader's u), z out of the building. */
float enter_face(Mesh *m, const BLoop *l, int k) {
    EdgeInfo e = edge_of(l, k);
    xf_enter(m, e.ax, e.az, e.ex, e.ez);
    return e.len;
}

static uint32_t w16(float w) { float v = w * 16.0f + 0.5f; return v < 0.0f ? 0u : v > 4095.0f ? 4095u : (uint32_t)v; }

FaceKey face_key(const BLoop *l, int k, int base) {
    int n = l->n, kp = (k + n - 1) % n, kn = (k + 1) % n;
    EdgeInfo e = edge_of(l, k), ep = edge_of(l, kp), en = edge_of(l, kn);
    FaceKey f;
    f.face = base + k; f.next = base + kn;
    f.len = e.len; f.prevLen = ep.len; f.nextLen = en.len;
    f.startConvex = ep.ex * e.ez - ep.ez * e.ex < -0.05f;
    f.endConvex = e.ex * en.ez - e.ez * en.ex < -0.05f;
    f.start90 = f.startConvex && fabsf(ep.ex * e.ex + ep.ez * e.ez) < 0.1f;
    f.end90 = f.endConvex && fabsf(e.ex * en.ex + e.ez * en.ez) < 0.1f;
    return f;
}

/* Does a closed room or a core stand 0.4 m in from u along edge e? */
static int lined_at(const ILayout *L, const EdgeInfo *e, float u) {
    float px = e->ax + e->ex * u + e->ez * 0.4f, pz = e->az + e->ez * u - e->ex * 0.4f, fx, fz;
    interior_to_frame(L, px, pz, &fx, &fz);
    int i = (int)floorf((fx - L->gx0) / IN_CELL), j = (int)floorf((fz - L->gz0) / IN_CELL);
    int r = (i >= 0 && j >= 0 && i < L->nx && j < L->nz && L->occ[j * L->nx + i] == IO_USABLE) ? L->own[j * L->nx + i] : 255;
    return r != 255 && ((L->reg[r].kind == IK_ROOM && !L->reg[r].glazed) || L->reg[r].kind == IK_SOLID);
}

/* Does something opaque (a closed room, a core, a market block) stand 0.4 m in from u along edge e? */
static int behind_lined(const Behind *bh, const EdgeInfo *e, float u) {
    if (bh->L) return lined_at(bh->L, e, u);
    float px = e->ax + e->ex * u + e->ez * 0.4f, pz = e->az + e->ez * u - e->ex * 0.4f, fx, fz;
    arcology_to_frame(bh->A, px, pz, &fx, &fz);
    int use = arcology_use_at(bh->A, fx, fz);
    return use == AU_BLOCK || use == AU_STALL || use == AU_CORE || use == AU_STAIR || use == AU_SPINE || use == AU_KIOSK;
}

/* Openings cut through face k of loop li: (u0, u1, top) — the lobby's entrance or the arcology's portals. */
int face_openings(const BGraph *g, const Behind *bh, int li, int k, float (*op)[3], int max) {
    int n = 0;
    if (!bh || li != 0) return 0;
    if (bh->L)
        for (int d = 0; d < g->ndoor && n < max; ++d)
            if (g->door[d].section == bh->si && g->door[d].face == k && g->door[d].type == DOOR_PRIMARY) {
                op[n][0] = g->door[d].u - g->door[d].width * 0.5f; op[n][1] = g->door[d].u + g->door[d].width * 0.5f;
                op[n][2] = G0 + g->door[d].height; n++;
            }
    if (bh->A)
        for (int p = 0; p < bh->A->nportal && n < max; ++p)
            if (bh->A->portal[p].face == k) {
                const APortal *pt = &bh->A->portal[p];
                op[n][0] = pt->u - pt->width * 0.5f; op[n][1] = pt->u + pt->width * 0.5f; op[n][2] = G0 + pt->height; n++;
            }
    return n;
}

/* Spans (u0, u1 along face k of loop li) of a planned ground floor's
 * envelope, with what stands behind each: 1 where a closed room or a core
 * backs onto it (the facade is opaque there), 0 where the lobby looks out
 * through glass. Each change is found on the partition between the two, then
 * the opaque side is widened to the next mullion of the facade's 1.5 m module
 * (shadeFacade), so it is whole back-painted panes and no clear strip is left
 * beside the partition; it never widens over an opening. */
int envelope_spans(const BGraph *g, const Behind *bh, const BLoop *l, int li, int k, float (*span)[2], uint8_t *lined, int max) {
    EdgeInfo e = edge_of(l, k);
    float bnd[65], op[8][3];
    uint8_t raw[64];
    int nop = face_openings(g, bh, li, k, op, 8);
    for (int o = 0; o < nop; ++o) { op[o][0] -= 0.15f; op[o][1] += 0.15f; }
    if (max > 64) max = 64;
    int nS = (int)ceilf(e.len / 0.25f), n = 0, prev = -1;
    float step = nS ? e.len / (float)nS : 0.0f;
    for (int q = 0; q < nS; ++q) {
        int cur = behind_lined(bh, &e, ((float)q + 0.5f) * step);
        if (cur == prev) continue;
        if (n == max) break;
        float b = 0.0f;
        if (n > 0) {
            float lo = ((float)q - 0.5f) * step, hi = ((float)q + 0.5f) * step;
            for (int it = 0; it < 14; ++it) {
                float mid = 0.5f * (lo + hi);
                if (behind_lined(bh, &e, mid) == prev) lo = mid; else hi = mid;
            }
            float c = 0.5f * (lo + hi);   /* the partition */
            b = prev ? ceilf((c + 0.01f) / 1.5f) * 1.5f : floorf((c - 0.01f) / 1.5f) * 1.5f;
            for (int o = 0; o < nop; ++o) {
                float du0 = op[o][0], du1 = op[o][1];
                if (prev && c <= du0 && b > du0) b = maxf(du0, c + 0.07f);
                if (!prev && c >= du1 && b < du1) b = minf(du1, c - 0.07f);
            }
            b = clampf(b, 0.0f, e.len);
        }
        bnd[n] = b; raw[n] = (uint8_t)cur; n++;
        prev = cur;
    }
    if (n == 0) return 0;
    bnd[n] = e.len;
    /* a clear stretch squeezed out between two widened opaque ones goes, and they join */
    int m = 0;
    for (int t = 0; t < n; ++t) {
        float a = bnd[t], b = bnd[t + 1];
        if (!raw[t] && b - a < 0.01f) continue;
        if (m > 0 && lined[m - 1] == raw[t]) { span[m - 1][1] = maxf(span[m - 1][1], b); continue; }
        if (m > 0) a = span[m - 1][1];
        span[m][0] = a; span[m][1] = maxf(a, b); lined[m] = raw[t]; m++;
    }
    return m;
}

/* Facade vertex data (decoded by shadeFacade in shaders/city.frag):
 *   uv.y   this face's width | the next face's width << 12, in 1/16 m (an exact integer)
 *   aux    the previous face's width | face << 12 | next face << 18 |
 *          corner flags << 24 (start/end convex, start/end square) | plant-floor code << 28
 *   params y section, z at-ground | ground mode << 1 | window << 3 | bands << 5 | fine bays << 6 |
 *          a real interior behind the ground floor << 7, w palette | shop level << 3 |
 *          a room or core behind this stretch of the ground floor << 6, an arcology's
 *          market block (lightboxes in the glass) << 7 (both set by emit_walls) */
static Mat facade_mat(const Building *b, const BGraph *g, int si, const FaceKey *fk, int real, float *packW) {
    const BSection *s = &g->sec[si];
    int shop = bgen_industrial(g) ? 0 : b->shop;   /* a plant's buildings have no shopfronts */
    Mat mt = mat(MAT_FACADE, g->seed, (uint8_t)si,
                 (uint8_t)((bsec_grounded(s) ? 1 : 0) | (g->groundMode << 1) | (s->win << 3) | (s->bands ? 32 : 0) |
                           (g->fine ? 64 : 0) | (real ? 128 : 0)),
                 (uint8_t)(s->palette | (shop << 3)));
    mt.aux = w16(fk->prevLen) | (uint32_t)fk->face << 12 | (uint32_t)fk->next << 18 |
             (uint32_t)(fk->startConvex | fk->endConvex << 1 | fk->start90 << 2 | fk->end90 << 3) << 24 |
             (uint32_t)(s->plant & 15) << 28;
    *packW = (float)(w16(fk->len) | w16(fk->nextLen) << 12);
    return mt;
}

/* With a real interior behind it (bh), each face is cut round its openings
 * (a lobby's entrance, an arcology's portals), so you can walk in, and split
 * where something opaque stands against it, so the facade there is opaque. */
static void emit_walls(Mesh *m, const Building *b, const BGraph *g, int si, const Behind *bh) {
    const BSection *s = &g->sec[si];
    const BLoop *loops[2] = { &s->foot.outer, &s->foot.hole };
    int base = 0;
    for (int li = 0; li < 2; ++li) {
        const BLoop *l = loops[li];
        for (int k = 0; k < l->n; ++k) {
            FaceKey fk = face_key(l, k, base);
            float pw, op[8][3];
            Mat mt = facade_mat(b, g, si, &fk, bh != NULL, &pw);
            Xf sv = xf_save(m);
            float len = enter_face(m, l, k);
            int nop = face_openings(g, bh, li, k, op, 8);
            for (int i = 0; i < nop; ++i)                     /* left to right */
                for (int j = i; j > 0 && op[j][0] < op[j - 1][0]; --j) { float t[3]; memcpy(t, op[j], sizeof(t)); memcpy(op[j], op[j - 1], sizeof(t)); memcpy(op[j - 1], t, sizeof(t)); }
            float span[64][2] = { { 0.0f, len } };
            uint8_t lined[64] = { 0 };
            int ns = bh ? envelope_spans(g, bh, l, li, k, span, lined, 64) : 1;
            for (int q = 0; q < ns; ++q) {
                float sa = span[q][0], sb = q == ns - 1 ? len : span[q][1];
                Mat mq = mt;
                if (lined[q]) mq.p[3] |= (uint8_t)(bh->A ? 64 | 128 : 64);   /* an arcology's: lightboxes */
                /* full height between openings, above each opening from its head */
                float cur = sa;
                for (int o = 0; o <= nop; ++o) {
                    float a = cur, c = o < nop ? minf(sb, op[o][0]) : sb;
                    float pieces[2][4] = { { a, c, s->y0, s->y1 }, { 0, 0, 0, 0 } };
                    int np = 1;
                    if (o < nop) {
                        pieces[1][0] = maxf(sa, op[o][0]); pieces[1][1] = minf(sb, op[o][1]);
                        pieces[1][2] = op[o][2]; pieces[1][3] = s->y1; np = 2;
                        cur = maxf(cur, op[o][1]);
                    }
                    for (int p = 0; p < np; ++p) {
                        float pa = pieces[p][0], pc = pieces[p][1], y0 = pieces[p][2], y1 = pieces[p][3];
                        if (pc - pa < 1e-3f || y1 - y0 < 1e-3f) continue;
                        float uv[4][2] = { { pa, pw }, { pc, pw }, { pc, pw }, { pa, pw } };
                        emit_quad(m, v3(pa, y0, 0), v3(pc, y0, 0), v3(pc, y1, 0), v3(pa, y1, 0), v3(0, 0, 1), uv, mq);
                    }
                    /* the step up to the opening: from its sill (the floor) down to the ground,
                     * or a flight of steps out over the sidewalk where that is too high a step */
                    if (o < nop && bsec_grounded(s)) {
                        float pa = maxf(sa, op[o][0]), pc = minf(sb, op[o][1]);
                        float ga = mesh_ground_local(m, pa, 0.0f) + SIDEWALK_HEIGHT, gc = mesh_ground_local(m, pc, 0.0f) + SIDEWALK_HEIGHT;
                        if (pc - pa >= 1e-3f && minf(ga, gc) < s->y0 - 0.005f) {
                            float uv[4][2] = { { pa, pw }, { pc, pw }, { pc, pw }, { pa, pw } };
                            float g = minf(ga, gc), rise = s->y0 - g;
                            int ft = m->footing, nr = entrance_risers(rise);
                            m->footing = 0;
                            if (nr == 1)
                                emit_quad(m, v3(pa, minf(ga, s->y0) - m->sink, 0), v3(pc, minf(gc, s->y0) - m->sink, 0),
                                          v3(pc, s->y0, 0), v3(pa, s->y0, 0), v3(0, 0, 1), uv, mq);
                            else {
                                float y1 = s->y0 - rise / (float)nr;
                                emit_quad(m, v3(pa, y1, 0), v3(pc, y1, 0), v3(pc, s->y0, 0), v3(pa, s->y0, 0), v3(0, 0, 1), uv, mq);
                                for (int t = 1; t < nr; ++t)   /* tread t, the riser below it at its front */
                                    emit_box(m, v3(pa, g - m->sink, (float)(t - 1) * STEP_TREAD), v3(pc, s->y0 - rise * (float)t / (float)nr, (float)t * STEP_TREAD),
                                             F_NX | F_PX | F_PZ | F_TOP, mat(MAT_CURB, 0, 0, 0, 0), UV_FACADE);
                            }
                            m->footing = ft;
                        }
                    }
                }
            }
            xf_restore(m, sv);
        }
        base += l->n;
    }
}

void emit_cap(Mesh *m, const BFoot *f, float y, int down, Mat mt) {
    float pts[2 * BG_MAX_PV][2];
    int tri[2 * BG_MAX_PV][3], np;
    int nt = bfoot_triangulate(f, pts, &np, tri, 2 * BG_MAX_PV);
    vec3 n = v3(0, down ? -1.0f : 1.0f, 0);
    for (int t = 0; t < nt; ++t) {
        float uv[3][2];
        vec3 p[3];
        for (int i = 0; i < 3; ++i) {
            p[i] = v3(pts[tri[t][i]][0], y, pts[tri[t][i]][1]);
            uv[i][0] = p[i].x; uv[i][1] = p[i].z;
        }
        emit_tri(m, p[0], p[1], p[2], n, uv, mt);
    }
}

/* Is (x, z) on the open roof of section si (not under anything standing on it)? */
static int roof_free(const BGraph *g, int si, float x, float z, float clear) {
    const BSection *s = &g->sec[si];
    if (!bfoot_contains(&s->foot, x, z) || bfoot_boundary_dist(&s->foot, x, z, NULL, NULL) < clear) return 0;
    for (int t = 0; t < g->nsec; ++t) {
        const BSection *o = &g->sec[t];
        if (t == si || fabsf(o->y0 - s->y1) > 0.05f) continue;
        if (bfoot_contains(&o->foot, x, z) || bfoot_boundary_dist(&o->foot, x, z, NULL, NULL) < clear) return 0;
    }
    return 1;
}

static int has_above(const BGraph *g, int si) {
    for (int t = 0; t < g->nsec; ++t)
        if (t != si && fabsf(g->sec[t].y0 - g->sec[si].y1) < 0.05f) return 1;
    return 0;
}

typedef struct Placed { float x0, z0, x1, z1; } Placed;

/* A free spot for a sx-by-sz item on the roof of si, clear of what is already there. */
static int roof_spot(const BGraph *g, int si, Rng *r, float sx, float sz, Placed *pl, int *npl, float *ox, float *oz) {
    float x0, z0, x1, z1;
    const BLoop *l = &g->sec[si].foot.outer;
    x0 = z0 = 1e30f; x1 = z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) { x0 = minf(x0, l->x[i]); x1 = maxf(x1, l->x[i]); z0 = minf(z0, l->z[i]); z1 = maxf(z1, l->z[i]); }
    for (int attempt = 0; attempt < 12; ++attempt) {
        float x = rng_range(r, x0 + 1.0f, x1 - 1.0f - sx), z = rng_range(r, z0 + 1.0f, z1 - 1.0f - sz);
        if (!roof_free(g, si, x, z, 1.0f) || !roof_free(g, si, x + sx, z, 1.0f) || !roof_free(g, si, x, z + sz, 1.0f) ||
            !roof_free(g, si, x + sx, z + sz, 1.0f) || !roof_free(g, si, x + sx * 0.5f, z + sz * 0.5f, 1.0f)) continue;
        int hit = 0;
        for (int i = 0; i < *npl && !hit; ++i)
            hit = x < pl[i].x1 + 0.8f && x + sx > pl[i].x0 - 0.8f && z < pl[i].z1 + 0.8f && z + sz > pl[i].z0 - 0.8f;
        if (hit) continue;
        if (*npl < 32) { pl[*npl].x0 = x; pl[*npl].z0 = z; pl[*npl].x1 = x + sx; pl[*npl].z1 = z + sz; (*npl)++; }
        *ox = x; *oz = z;
        return 1;
    }
    return 0;
}

/* Bounds and vertex centroid of an outline. */
static void loop_box(const BLoop *l, float *x0, float *z0, float *x1, float *z1) {
    *x0 = 1e30f; *x1 = -1e30f; *z0 = 1e30f; *z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) { *x0 = minf(*x0, l->x[i]); *x1 = maxf(*x1, l->x[i]); *z0 = minf(*z0, l->z[i]); *z1 = maxf(*z1, l->z[i]); }
}
static void loop_centre(const BLoop *l, float *cx, float *cz) {
    float x = 0.0f, z = 0.0f;
    for (int i = 0; i < l->n; ++i) { x += l->x[i]; z += l->z[i]; }
    *cx = x / (float)l->n; *cz = z / (float)l->n;
}

/* Plant units on a flat roof, and a plant room on a big one. */
static void roof_plant(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float y = s->y1;
    float area = -bloop_area(l) - (s->foot.hole.n ? bloop_area(&s->foot.hole) : 0.0f);
    Placed pl[32];
    int npl = 0;
    float x, z;
    int units = (int)clampf(area / 180.0f, 1.0f, 7.0f);
    for (int i = 0; i < units; ++i) {
        float sx = rng_range(r, 2.0f, 5.0f), sz = rng_range(r, 2.0f, 5.0f), sy = rng_range(r, 1.2f, 3.2f);
        if (roof_spot(g, si, r, sx, sz, pl, &npl, &x, &z))
            emit_box(m, v3(x, y, z), v3(x + sx, y + sy, z + sz), F_NOBOT, mat(MAT_METAL, g->seed + (uint32_t)i, 1, 0, 0), UV_FACADE);
    }
    if (area > 900.0f && roof_spot(g, si, r, 7.0f, 5.0f, pl, &npl, &x, &z))   /* plant room */
        emit_box(m, v3(x, y, z), v3(x + 7.0f, y + 3.4f, z + 5.0f), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
}

/* Water tanks on legs. */
static void roof_tanks(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    float y = s->y1;
    Placed pl[32];
    int npl = 0;
    float x, z;
    int n = rng_int(r, 1, 4);
    Mat metal = mat(MAT_METAL, g->seed, 1, 0, 0), legs = mat(MAT_METAL, g->seed, 2, 0, 0);
    for (int i = 0; i < n; ++i) {
        float sz = rng_range(r, 3.0f, 4.5f);
        if (!roof_spot(g, si, r, sz, sz, pl, &npl, &x, &z)) continue;
        for (int k = 0; k < 4; ++k) {
            float lx = x + ((k & 1) ? sz - 0.3f : 0.1f), lz = z + ((k & 2) ? sz - 0.3f : 0.1f);
            emit_box(m, v3(lx, y, lz), v3(lx + 0.2f, y + 1.4f, lz + 0.2f), F_SIDES, legs, UV_FACADE);
        }
        emit_box(m, v3(x, y + 1.4f, z), v3(x + sz, y + 3.9f, z + sz), F_NOBOT | F_BOT, metal, UV_FACADE);
    }
}

/* Rows of solar panels wherever the roof is open. */
static void roof_solar(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float y = s->y1;
    (void)r;
    Mat frame = mat(MAT_METAL, g->seed, 2, 0, 0), panel = mat(MAT_METAL, g->seed, 6, 0, 0);
    float x0, z0, x1, z1;
    loop_box(l, &x0, &z0, &x1, &z1);
    int panels = 0;
    for (float pz = z0 + 1.5f; pz + 1.2f < z1 - 1.0f && panels < 160; pz += 2.6f)
        for (float px = x0 + 1.5f; px + 1.7f < x1 - 1.0f && panels < 160; px += 1.9f) {
            if (!roof_free(g, si, px, pz, 0.8f) || !roof_free(g, si, px + 1.7f, pz + 1.1f, 0.8f)) continue;
            float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            vec3 a = v3(px, y + 0.45f, pz), b = v3(px + 1.7f, y + 0.45f, pz), c = v3(px + 1.7f, y + 0.85f, pz + 1.1f), d = v3(px, y + 0.85f, pz + 1.1f);
            emit_quad(m, a, b, c, d, v3_norm(v3(0, 1.1f, -0.4f)), uv, panel);
            emit_box(m, v3(px + 0.75f, y, pz + 0.4f), v3(px + 0.95f, y + 0.6f, pz + 0.6f), F_SIDES, frame, UV_FACADE);
            panels++;
        }
}

/* A planted roof with raised beds. */
static void roof_garden(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    float y = s->y1;
    Placed pl[32];
    int npl = 0;
    float x, z;
    emit_cap(m, &s->foot, y + 0.06f, 0, mat(MAT_LOT, 0, 1, 0, 0));
    int n = rng_int(r, 2, 6);
    for (int i = 0; i < n; ++i) {
        float sx = rng_range(r, 1.5f, 4.0f), sz = rng_range(r, 1.0f, 2.0f);
        if (roof_spot(g, si, r, sx, sz, pl, &npl, &x, &z))
            emit_box(m, v3(x, y, z), v3(x + sx, y + 0.7f, z + sz), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
    }
}

/* A helipad with its H and edge lights, if the roof has room. */
static void roof_helipad(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float y = s->y1;
    (void)r;
    float cx, cz;
    loop_centre(l, &cx, &cz);
    int ok = 1;
    for (int k = 0; k < 8 && ok; ++k)
        ok = roof_free(g, si, cx + 9.0f * cosf(k * PI_F * 0.25f), cz + 9.0f * sinf(k * PI_F * 0.25f), 0.5f);
    if (!ok) return;
    BFoot pad;
    memset(&pad, 0, sizeof(pad));
    for (int k = 0; k < 8; ++k) {
        float a = -(k + 0.5f) * PI_F * 0.25f;   /* negative turn: an outer loop */
        pad.outer.x[k] = cx + 8.0f * cosf(a); pad.outer.z[k] = cz + 8.0f * sinf(a);
    }
    pad.outer.n = 8;
    emit_cap(m, &pad, y + 0.3f, 0, mat(MAT_METAL, g->seed, 2, 0, 0));
    /* the H, and edge lights */
    emit_box(m, v3(cx - 2.2f, y + 0.3f, cz - 3.0f), v3(cx - 1.6f, y + 0.36f, cz + 3.0f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
    emit_box(m, v3(cx + 1.6f, y + 0.3f, cz - 3.0f), v3(cx + 2.2f, y + 0.36f, cz + 3.0f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
    emit_box(m, v3(cx - 1.6f, y + 0.3f, cz - 0.3f), v3(cx + 1.6f, y + 0.36f, cz + 0.3f), F_TOP, mat(MAT_EMISSIVE, 0, 6, 0, 0), UV_UNIT);
    for (int k = 0; k < 8; ++k)
        emit_emissive_box(m, v3(cx + 8.3f * cosf(k * PI_F * 0.25f), y + 0.4f, cz + 8.3f * sinf(k * PI_F * 0.25f)), 0.12f, 5, 0, 0);
}

/* A hipped roof (on a rectangle) or a glass pyramid. */
static void roof_pitched(Mesh *m, const BGraph *g, int si, Rng *r) {
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float y = s->y1;
    (void)r;
    if (s->foot.hole.n) return;
    float cx, cz;
    loop_centre(l, &cx, &cz);
    float x0, z0, x1, z1;
    loop_box(l, &x0, &z0, &x1, &z1);
    float mn = minf(x1 - x0, z1 - z0);
    int rect = l->n == 4 && fabsf((x1 - x0) * (z1 - z0) + bloop_area(l)) < 0.5f;
    if (s->roof == ROOF_HIP && rect) {
        /* ridge along the long axis */
        float h = minf(mn * 0.3f, 7.0f), half = mn * 0.5f;
        int alongX = x1 - x0 >= z1 - z0;
        vec3 r0 = alongX ? v3(x0 + half, y + h, cz) : v3(cx, y + h, z0 + half);
        vec3 r1 = alongX ? v3(x1 - half, y + h, cz) : v3(cx, y + h, z1 - half);
        vec3 c00 = v3(x0, y, z0), c10 = v3(x1, y, z0), c11 = v3(x1, y, z1), c01 = v3(x0, y, z1);
        Mat tiles = mat(MAT_ROOF, g->seed, 0, 0, 1);
        float uv4[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} }, uv3[3][2] = { {0,0}, {1,0}, {0.5f,1} };
        vec3 q[4][4], t[2][3];
        if (alongX) {
            q[0][0] = c00; q[0][1] = c10; q[0][2] = r1; q[0][3] = r0;
            q[1][0] = c11; q[1][1] = c01; q[1][2] = r0; q[1][3] = r1;
            t[0][0] = c01; t[0][1] = c00; t[0][2] = r0;
            t[1][0] = c10; t[1][1] = c11; t[1][2] = r1;
        } else {
            q[0][0] = c10; q[0][1] = c11; q[0][2] = r1; q[0][3] = r0;
            q[1][0] = c01; q[1][1] = c00; q[1][2] = r0; q[1][3] = r1;
            t[0][0] = c00; t[0][1] = c10; t[0][2] = r0;
            t[1][0] = c11; t[1][1] = c01; t[1][2] = r1;
        }
        for (int i = 0; i < 2; ++i) {
            vec3 n = v3_norm(v3_cross(v3_sub(q[i][1], q[i][0]), v3_sub(q[i][3], q[i][0])));
            if (n.y < 0) n = v3_scale(n, -1.0f);
            emit_quad(m, q[i][0], q[i][1], q[i][2], q[i][3], n, uv4, tiles);
            vec3 tn = v3_norm(v3_cross(v3_sub(t[i][1], t[i][0]), v3_sub(t[i][2], t[i][0])));
            if (tn.y < 0) tn = v3_scale(tn, -1.0f);
            emit_tri(m, t[i][0], t[i][1], t[i][2], tn, uv3, tiles);
        }
    } else if (s->roof == ROOF_PYRAMID) {
        float h = minf(mn * 0.35f, 12.0f);
        vec3 apex = v3(cx, y + h, cz);
        Mat glass = mat(MAT_METAL, g->seed, 7, 0, 0);
        float uv3[3][2] = { {0,0}, {1,0}, {0.5f,1} };
        for (int k = 0; k < l->n; ++k) {
            int k1 = (k + 1) % l->n;
            vec3 a = v3(l->x[k], y, l->z[k]), b = v3(l->x[k1], y, l->z[k1]);
            vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(apex, a)));
            if (n.y < 0) n = v3_scale(n, -1.0f);
            emit_tri(m, a, b, apex, n, uv3, glass);
        }
        emit_emissive_box(m, v3(cx, y + h + 0.3f, cz), 0.3f, 2, 1, 0);
    }
}

/* An industrial shed's roof: a shallow gable along its length, or saw teeth
 * across it, their steep faces glazed and lit from within. */
static void roof_shed(Mesh *m, const BGraph *g, int si) {
    const BSection *s = &g->sec[si];
    float x0, z0, x1, z1;
    loop_box(&s->foot.outer, &x0, &z0, &x1, &z1);
    int alongX = x1 - x0 >= z1 - z0;
    float L = alongX ? x1 - x0 : z1 - z0, W = alongX ? z1 - z0 : x1 - x0, y = s->y1;
    int clad = g->style == PST_GAS ? PA_CLAD : g->style == PST_GOTHIC ? PA_RUST : PA_SOOT;
    Mat roof = mat(MAT_INDUSTRY, g->seed, IND_PAINT, (uint8_t)(clad | 32), 0);
    /* (a, b): along and across the shed, to the frame */
#define P(a, h, b) (alongX ? v3(x0 + (a), y + (h), z0 + (b)) : v3(x0 + (b), y + (h), z0 + (a)))
    float uv4[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } }, uv3[3][2] = { { 0, 0 }, { 1, 0 }, { 0.5f, 1 } };
    if (s->roof == ROOF_GABLE) {
        float h = minf(W * 0.18f, 3.0f), o = 0.35f;
        for (int sd = 0; sd < 2; ++sd) {
            float b0 = sd ? W + o : -o, rise = h + o * h / (W * 0.5f);
            vec3 a = P(-o, h - rise, b0), b = P(L + o, h - rise, b0), c = P(L + o, h, W * 0.5f), d = P(-o, h, W * 0.5f);
            vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(d, a)));
            if (n.y < 0.0f) n = v3_scale(n, -1.0f);
            emit_quad(m, a, b, c, d, n, uv4, roof);
            emit_quad(m, a, b, c, d, v3_scale(n, -1.0f), uv4, roof);   /* the eaves' underside */
        }
        for (int e = 0; e < 2; ++e) {   /* the gable ends */
            float a = e ? L : 0.0f;
            vec3 n = alongX ? v3(e ? 1.0f : -1.0f, 0, 0) : v3(0, 0, e ? 1.0f : -1.0f);
            emit_tri(m, P(a, 0.0f, 0.0f), P(a, 0.0f, W), P(a, h, W * 0.5f), n, uv3, roof);
        }
    } else {
        int teeth = (int)maxf(2.0f, floorf(L / 5.0f));
        float t = L / (float)teeth, h = minf(t * 0.45f, 2.6f);
        Mat glass = mat(MAT_INDUSTRY, g->seed, IND_GLOW, (uint8_t)(g->style == PST_GOTHIC ? PA_GREEN : PA_ORANGE), 0);
        for (int k = 0; k < teeth; ++k) {
            float a0 = (float)k * t, a1 = a0 + t;
            vec3 p0 = P(a0, 0.0f, 0.0f), p1 = P(a1, h, 0.0f), p2 = P(a1, h, W), p3 = P(a0, 0.0f, W);
            vec3 n = v3_norm(v3_cross(v3_sub(p1, p0), v3_sub(p3, p0)));
            if (n.y < 0.0f) n = v3_scale(n, -1.0f);
            emit_quad(m, p0, p1, p2, p3, n, uv4, roof);
            vec3 gn = alongX ? v3(1, 0, 0) : v3(0, 0, 1);
            float guv[4][2] = { { 0, 0 }, { W, 0 }, { W, h }, { 0, h } };
            emit_quad(m, P(a1, 0.0f, 0.0f), P(a1, 0.0f, W), P(a1, h, W), P(a1, h, 0.0f), gn, guv, glass);
            for (int sd = 0; sd < 2; ++sd) {
                vec3 sn = alongX ? v3(0, 0, sd ? 1.0f : -1.0f) : v3(sd ? 1.0f : -1.0f, 0, 0);
                float b = sd ? W : 0.0f;
                emit_tri(m, P(a0, 0.0f, b), P(a1, 0.0f, b), P(a1, h, b), sn, uv3, roof);
            }
        }
    }
#undef P
}

static void dress_roof(Mesh *m, const BGraph *g, int si, Rng *r) {
    switch (g->sec[si].roof) {
    case ROOF_GABLE: case ROOF_SAWTOOTH: roof_shed(m, g, si); break;
    case ROOF_PLANT:   roof_plant(m, g, si, r); break;
    case ROOF_TANKS:   roof_tanks(m, g, si, r); break;
    case ROOF_SOLAR:   roof_solar(m, g, si, r); break;
    case ROOF_GARDEN:  roof_garden(m, g, si, r); break;
    case ROOF_HELIPAD: roof_helipad(m, g, si, r); break;
    case ROOF_HIP: case ROOF_PYRAMID: roof_pitched(m, g, si, r); break;
    default: break;
    }
}

/* Height of the facade above a ground-floor face: follow the sections stacked over it. */
static float facade_top(const BGraph *g, int si, float px, float pz) {
    float top = g->sec[si].y1;
    for (int guard = 0; guard < g->nsec; ++guard) {
        int found = 0;
        for (int t = 0; t < g->nsec && !found; ++t)
            if (fabsf(g->sec[t].y0 - top) < 0.05f && bfoot_contains(&g->sec[t].foot, px, pz)) { top = g->sec[t].y1; found = 1; }
        if (!found) break;
    }
    return top;
}

static int face_toward(const Building *b, const EdgeInfo *e) {
    float nx = -e->ez, nz = e->ex;
    int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
    return (b->frontage & side) != 0;
}

/* Street-level signs on one ground-floor face (in the face's frame). */
static void face_signs(Mesh *m, const Building *b, const BGraph *g, int si, const FaceKey *fk, float top, Rng *r) {
    const float gy = G0;
    float gh = g->groundH, fh = g->floorH;
    Face f = face_local(fk->len);
    int retail = g->groundMode == GF_RETAIL;

    /* Shop boards above most lit storefronts (6 m bays along the face). */
    if (retail && b->shop) {
        for (int seg = 0; seg * 6.0f < f.width; ++seg) {
            float u0 = seg * 6.0f + 0.4f, u1 = minf(seg * 6.0f + 6.0f, f.width) - 0.4f;
            if (u1 - u0 < 2.6f) continue;
            uint32_t h = city_shop_hash(b->seed, si, fk->face, seg, fk->len, fk->next, fk->endConvex);
            if (!city_shop_lit(h, b->shop) || hash_float(hash_combine(h, 0xB0A2u)) > 0.8f) continue;
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.15f ? 1 : 0);
            emit_face_panel(m, &f, u0, u1, gy + gh - 1.3f, gy + gh - 0.35f, 0.14f,
                            mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (1 << 3) | (flick << 6)), q_half(u1 - u0), q_half(0.95f)));
        }
    }

    /* Vertical blade signs sticking out over the sidewalk. */
    if (g->archetype != ARCH_ARCOLOGY && g->groundMode != GF_VOID) {
        int count = (int)(f.width / 8.0f);
        float maxH = minf(14.0f, top - (gy + gh) - 2.0f);
        for (int i = 0; i < count && maxH > 3.0f; ++i) {
            if (rng_float(r) > (retail ? 0.7f : 0.3f)) continue;
            float u = rng_range(r, 1.5f, f.width - 1.5f);
            float h = rng_range(r, 3.0f, maxH);
            float y0 = gy + gh + 0.8f + floorf(rng_range(r, 0.0f, 2.0f)) * fh;
            if (y0 + h > top - 0.5f) y0 = top - 0.5f - h;
            float depth = rng_range(r, 1.1f, 1.8f);
            float th = 0.12f;
            emit_box(m, v3(u - th, y0, 0.25f), v3(u + th, y0 + h, 0.25f + depth), F_NOBOT | F_BOT, mat(MAT_METAL, b->seed, 3, 0, 0), UV_FACADE);
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.2f ? (rng_float(r) < 0.5f ? 1 : 2) : 0);
            Mat nm = mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (0 << 3) | (flick << 6)), q_half(depth), q_half(h));
            float uvA[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            float uvB[4][2] = { {1,0}, {0,0}, {0,1}, {1,1} };
            for (int sd = -1; sd <= 1; sd += 2) {
                float uu = u + sd * (th + 0.01f);
                vec3 p0 = v3(uu, y0 + 0.1f, 0.3f), p1 = v3(uu, y0 + 0.1f, 0.2f + depth);
                vec3 p2 = v3(uu, y0 + h - 0.1f, 0.2f + depth), p3 = v3(uu, y0 + h - 0.1f, 0.3f);
                emit_quad(m, p0, p1, p2, p3, v3((float)sd, 0, 0), sd < 0 ? uvB : uvA, nm);
            }
        }
    }

    /* Small neon signs in the windows of the lower floors (bars, clinics, noodle joints). */
    int floors = (int)minf(5.0f, (top - (gy + gh)) / fh - 1.0f);
    for (int fl = 0; fl < floors; ++fl) {
        for (float u = 1.0f; u < f.width - 3.0f; u += 7.0f) {
            if (rng_float(r) > 0.16f) continue;
            float w = rng_range(r, 1.4f, 3.0f), h = rng_range(r, 0.55f, 1.0f);
            float y0 = gy + gh + fl * fh + 1.1f;
            uint8_t col = (uint8_t)(rng_u32(r) & 7);
            uint8_t flick = (uint8_t)(rng_float(r) < 0.2f ? 1 : 0);
            emit_face_panel(m, &f, u, u + w, y0, y0 + h, 0.1f,
                            mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (1 << 3) | (flick << 6)), q_half(w), q_half(h)));
        }
    }
}

/* One large element on a tall section face: a giant video screen or a tall neon banner. */
static void face_feature(Mesh *m, const BGraph *g, const BSection *s, float width, Rng *r) {
    const float gy = G0;
    float th = s->y1 - s->y0;
    if (th < 30.0f || width < 10.0f) return;
    Face tf = face_local(width);
    float roll = rng_float(r);
    float tall = s->y1 - gy;
    if (tall > 70.0f && roll < 0.22f && width >= 14.0f) {
        float w = minf(width - 4.0f, rng_range(r, 14.0f, 42.0f));
        float h = rng_float(r) < 0.55f ? w * 0.5625f : w * 1.5f;
        h = minf(h, th - 6.0f);
        if (h < 8.0f) return;
        float u0 = rng_range(r, 2.0f, width - 2.0f - w);
        float y0 = s->y0 + rng_range(r, 0.3f, 0.9f) * (th - h - 2.0f) + 1.0f;
        emit_box(m, v3(u0 - 0.6f, y0 - 0.6f, 0.0f), v3(u0 + w + 0.6f, y0 + h + 0.6f, 0.45f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
        emit_face_panel(m, &tf, u0, u0 + w, y0, y0 + h, 0.5f, mat(MAT_SCREEN, rng_u32(r), 0, q_half(w), q_half(h)));
    } else if (roll < 0.42f && g->archetype != ARCH_ARCOLOGY) {
        float w = rng_range(r, 3.0f, 6.0f);
        float h = minf(rng_range(r, 12.0f, 45.0f), th * 0.7f);
        float u0 = rng_range(r, 1.0f, width - 1.0f - w);
        float y0 = s->y0 + rng_range(r, 0.1f, 0.9f) * (th - h - 1.0f) + 0.5f;
        if (s->y0 <= gy + 0.01f) y0 = maxf(y0, gy + g->groundH + 1.0f);
        if (y0 + h > s->y1 - 0.5f) return;
        uint8_t col = (uint8_t)(rng_u32(r) & 7);
        emit_face_panel(m, &tf, u0, u0 + w, y0, y0 + h, 0.3f, mat(MAT_NEON, rng_u32(r), (uint8_t)(col | (2 << 3)), q_half(w), q_half(h)));
    }
}

static void emit_doors(Mesh *m, const BGraph *g, const Interiors *in) {
    const float gy = G0;
    Mat metal = mat(MAT_METAL, g->seed, 3, 0, 0);
    for (int d = 0; d < g->ndoor; ++d) {
        const BDoor *dr = &g->door[d];
        Xf sv = xf_save(m);
        float len = enter_face(m, &g->sec[dr->section].foot.outer, dr->face);
        Face f = face_local(len);
        float u0 = dr->u - dr->width * 0.5f, u1 = dr->u + dr->width * 0.5f, y1 = gy + dr->height;
        if (dr->type == DOOR_PRIMARY && interior_of(in, dr->section)) {
            /* an open doorway into the lobby: jambs and a head through the wall */
            emit_box(m, v3(u0 - 0.15f, gy, -0.12f), v3(u0, y1 + 0.15f, 0.08f), F_NOBOT, metal, UV_FACADE);
            emit_box(m, v3(u1, gy, -0.12f), v3(u1 + 0.15f, y1 + 0.15f, 0.08f), F_NOBOT, metal, UV_FACADE);
            emit_box(m, v3(u0, y1, -0.12f), v3(u1, y1 + 0.15f, 0.08f), F_NOBOT | F_BOT, metal, UV_FACADE);
        } else {
            emit_box(m, v3(u0 - 0.15f, gy, 0.0f), v3(u1 + 0.15f, y1 + 0.15f, 0.08f), F_NOBOT, metal, UV_FACADE);
            /* a plant's front door is a steel one, like its others */
            uint8_t look = (uint8_t)(bgen_industrial(g) && dr->type == DOOR_PRIMARY ? DOOR_SERVICE : dr->type);
            emit_face_panel(m, &f, u0, u1, gy, y1, 0.1f, mat(MAT_DOOR, hash_combine(g->seed, (uint32_t)d), look, q_half(dr->width), q_half(dr->height)));
        }
        if (dr->canopy) {
            emit_box(m, v3(u0 - 0.8f, y1 + 0.3f, 0.0f), v3(u1 + 0.8f, y1 + 0.55f, 1.8f), F_NOBOT | F_BOT, metal, UV_FACADE);
            float uv[4][2] = { { 0, 0 }, { u1 - u0 + 1.4f, 0 }, { u1 - u0 + 1.4f, 1 }, { 0, 1 } };
            int lobby = g->groundMode == GF_LOBBY || g->groundMode == GF_DOUBLE;
            emit_quad(m, v3(u0 - 0.7f, y1 + 0.29f, 1.55f), v3(u1 + 0.7f, y1 + 0.29f, 1.55f), v3(u1 + 0.7f, y1 + 0.29f, 1.75f),
                      v3(u0 - 0.7f, y1 + 0.29f, 1.75f), v3(0, -1, 0), uv,
                      mat(MAT_LEDSTRIP, g->seed, lobby ? lobby_accent(g->seed) : (uint8_t)(g->seed & 7), 0, 0));
        } else if (dr->type != DOOR_PRIMARY) {
            emit_emissive_box(m, v3(dr->u, y1 + 0.45f, 0.25f), 0.15f, 0, 2, 0);   /* a sodium bulb over the door */
        }
        xf_restore(m, sv);
    }
}

/* Light set into the sidewalk along a lobby's face k (in the face's frame):
 * small amber uplights every two mullions, clear of its doors, and now and
 * then a red status lamp on the fascia above. Their own random stream, so the
 * signs drawn after them are unchanged. */
static void lobby_lights(Mesh *m, const BGraph *g, int si, int k, float len) {
    const float gy = G0;
    Rng r = rng_make(hash2i(g->seed ^ 0x0B1Eu, si, k));
    Mat up = mat(MAT_EMISSIVE, 0, 3, 0, 0);
    for (float u = 0.75f; u < len - 0.5f; u += 3.0f) {
        int clear = 1;
        for (int d = 0; d < g->ndoor && clear; ++d)
            if (g->door[d].section == si && g->door[d].face == k && fabsf(u - g->door[d].u) < g->door[d].width * 0.5f + 0.6f) clear = 0;
        if (clear) emit_box(m, v3(u - 0.07f, gy, 0.38f), v3(u + 0.07f, gy + 0.008f, 0.52f), F_TOP, up, UV_UNIT);
    }
    if (len > 2.0f && rng_float(&r) < 0.5f) {
        float bx = rng_range(&r, 0.5f, len - 0.5f), by = gy + g->groundH + rng_range(&r, 0.4f, 1.2f);
        emit_emissive_box(m, v3(bx, by, 0.06f), 0.05f, 2, 0, 0);
    }
}

/* Section si's roof slab and its underside (round an arcology's canyon or a podium atrium's void). */
static void emit_section_caps(Mesh *m, const BGraph *g, int si, const Interiors *in) {
    const float gy = G0;
    const BSection *s = &g->sec[si];
    /* an arcology's canyon rises through the slabs between the tiers it passes */
    int tier = -1;
    for (int t = 0; in->arc && t < in->arc->ntier; ++t) if (in->arc->tier[t].section == si) tier = t;
    if (tier >= 0 && tier < in->arc->topTier) emit_cap_holes(m, in->arc, &s->foot.outer, in->arc->voids, in->arc->nvoid, s->y1, 0, mat(MAT_ROOF, g->seed, (uint8_t)si, 0, 0));
    else emit_cap(m, &s->foot, s->y1, 0, mat(MAT_ROOF, g->seed, (uint8_t)si, 0, 0));
    if (tier >= 1 && tier <= in->arc->topTier) {
        emit_cap_holes(m, in->arc, &s->foot.outer, in->arc->voids, in->arc->nvoid, s->y0, 1, mat(MAT_ROOF, g->seed, (uint8_t)si, 1, 0));
    } else if (s->y0 > gy + 0.01f) {
        /* a podium atrium's void rises through the slab under this section */
        const ILayout *pl = NULL;
        for (int k = 0; k < in->n; ++k) {
            const ILayout *L = in->lay[k];
            if (L->atrium.mode != IA_PODIUM || L->atrium.x1 <= L->atrium.x0 || fabsf(s->y0 - L->ceilY) > 0.05f) continue;
            for (int q = 0; q < s->nbelow; ++q) if (s->below[q] == L->section) pl = L;
        }
        if (pl) emit_cap_minus(m, pl, &s->foot.outer, pl->gx0 + pl->atrium.x0 * IN_CELL, pl->gz0 + pl->atrium.z0 * IN_CELL,
                               pl->gx0 + pl->atrium.x1 * IN_CELL, pl->gz0 + pl->atrium.z1 * IN_CELL, s->y0, 1, mat(MAT_ROOF, g->seed, (uint8_t)si, 1, 0));
        else emit_cap(m, &s->foot, s->y0, 1, mat(MAT_ROOF, g->seed, (uint8_t)si, 1, 0));
    }
}

/* Trim on section si's faces: LED strips, accent bands, fins on the bay lines, parapets. */
static void emit_face_trims(Mesh *m, const BGraph *g, int si, float bay, int *fins) {
    const float gy = G0;
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    uint8_t ledCol = (uint8_t)(hash_combine(g->seed, (uint32_t)si) & 7);
    for (int k = 0; k < l->n; ++k) {
        Xf sv = xf_save(m);
        float len = enter_face(m, l, k);
        /* LED strip under the top edge, and an accent band where a tower meets its base */
        int pitched = s->roof == ROOF_HIP || s->roof == ROOF_PYRAMID || s->roof == ROOF_GABLE || s->roof == ROOF_SAWTOOTH;
        if (s->ledTop && !pitched) {
            float uv[4][2] = { { 0, 0 }, { len, 0 }, { len, 1 }, { 0, 1 } };
            emit_quad(m, v3(0, s->y1 - 0.75f, 0.06f), v3(len, s->y1 - 0.75f, 0.06f), v3(len, s->y1 - 0.45f, 0.06f),
                      v3(0, s->y1 - 0.45f, 0.06f), v3(0, 0, 1), uv, mat(MAT_LEDSTRIP, g->seed, ledCol, (uint8_t)(ledCol & 1), 0));
        }
        if (s->accent) {
            emit_box(m, v3(0, s->y0, 0), v3(len, s->y0 + 0.9f, 0.3f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
            float uv[4][2] = { { 0, 0 }, { len, 0 }, { len, 1 }, { 0, 1 } };
            emit_quad(m, v3(0, s->y0 + 0.35f, 0.31f), v3(len, s->y0 + 0.35f, 0.31f), v3(len, s->y0 + 0.55f, 0.31f),
                      v3(0, s->y0 + 0.55f, 0.31f), v3(0, 0, 1), uv, mat(MAT_LEDSTRIP, g->seed, (uint8_t)(ledCol + 3), 1, 0));
        }
        /* vertical fins on the bay lines (the shader stretches bays to fit the face) */
        if (s->fins && *fins < 320) {
            int nB = (int)maxf(1.0f, floorf(len / bay + 0.5f));
            float ys = s->y0 <= gy + 0.01f ? gy + g->groundH : s->y0;
            for (int j = 1; j < nB && *fins < 320; ++j, ++*fins) {
                float u = (float)j * len / (float)nB;
                emit_box(m, v3(u - 0.09f, ys, 0.0f), v3(u + 0.09f, s->y1, 0.45f), F_NOBOT | F_BOT, mat(MAT_METAL, g->seed, 3, 0, 0), UV_FACADE);
            }
        }
        /* parapet where the roof edge is open */
        if (!pitched) {
            EdgeInfo e = edge_of(l, k);
            float mx = e.ax + e.ex * len * 0.5f + e.ez * 0.6f, mz = e.az + e.ez * len * 0.5f - e.ex * 0.6f;
            if (roof_free(g, si, mx, mz, 0.0f))
                emit_box(m, v3(0, s->y1, -0.35f), v3(len, s->y1 + 1.1f, 0), F_NOBOT, mat(MAT_METAL, g->seed, 0, 0, 0), UV_FACADE);
        }
        xf_restore(m, sv);
    }
}

/* Masts, spires and aircraft warning lights on top of section si (nothing stands on it). */
static void emit_tops(Mesh *m, const BGraph *g, int si, Rng *rp) {
    const float gy = G0;
    const BSection *s = &g->sec[si];
    const BLoop *l = &s->foot.outer;
    float cx, cz;
    loop_centre(l, &cx, &cz);
    float tall = s->y1 - gy;
    float mh = s->spire ? (float)s->spire : (tall > 60.0f && rng_float(rp) < 0.5f ? rng_range(rp, 8.0f, 28.0f) : 0.0f);
    if (mh > 0.0f && bfoot_contains(&s->foot, cx, cz) && s->roof != ROOF_HELIPAD && s->roof != ROOF_PYRAMID) {
        float w = g->archetype == ARCH_ARCOLOGY ? 1.2f : s->spire ? 0.5f : 0.35f;
        emit_box(m, v3(cx - w, s->y1, cz - w), v3(cx + w, s->y1 + mh, cz + w), F_NOBOT, mat(MAT_METAL, g->seed, 2, 0, 0), UV_FACADE);
        emit_emissive_box(m, v3(cx, s->y1 + mh + 0.4f, cz), 0.4f, 2, 1, (uint8_t)(rng_u32(rp) & 255));
    }
    if (tall > 90.0f) {
        uint8_t ph = (uint8_t)(rng_u32(rp) & 255);
        for (int k = 0; k < l->n; ++k) {
            FaceKey fk = face_key(l, k, 0);
            if (!fk.startConvex) continue;
            float vx = l->x[k] + (cx - l->x[k]) * 0.4f / maxf(1.0f, sqrtf((cx - l->x[k]) * (cx - l->x[k]) + (cz - l->z[k]) * (cz - l->z[k])));
            float vz = l->z[k] + (cz - l->z[k]) * 0.4f / maxf(1.0f, sqrtf((cx - l->x[k]) * (cx - l->x[k]) + (cz - l->z[k]) * (cz - l->z[k])));
            emit_emissive_box(m, v3(vx, s->y1 + 1.5f, vz), 0.3f, 2, 1, ph);
        }
    }
}

/* Columns, struts and walls carrying the sections. */
static void emit_supports(Mesh *m, const BGraph *g) {
    Mat concrete = mat(MAT_METAL, g->seed, 5, 0, 0);
    for (int i = 0; i < g->nsup; ++i) {
        const BSupport *s = &g->sup[i];
        if (s->type == SUP_COLUMN) {
            float h = s->size * 0.5f;
            emit_box(m, v3(s->x0 - h, s->y0, s->z0 - h), v3(s->x0 + h, s->y1, s->z0 + h), F_SIDES, concrete, UV_FACADE);
        } else if (s->type == SUP_STRUT) {
            emit_beam(m, v3(s->x0, s->y0, s->z0), v3(s->x1, s->y1, s->z1), s->size, concrete);
        } else {
            emit_box(m, v3(s->x0, s->y0, s->z0), v3(s->x1, s->y1, s->z1), F_SIDES | F_BOT, concrete, UV_FACADE);
        }
    }
}

/* Street-level signs on faces onto the street, and big elements on tall faces. */
static void emit_street_signs(Mesh *m, const Building *b, const BGraph *g, Rng *rp) {
    const float gy = G0;
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        const BLoop *l = &s->foot.outer;
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            if (!face_toward(b, &e)) continue;
            FaceKey fk = face_key(l, k, 0);
            Xf sv = xf_save(m);
            enter_face(m, l, k);
            if (s->y0 <= gy + 0.01f) {
                float px = e.ax + e.ex * e.len * 0.5f + e.ez * 0.3f, pz = e.az + e.ez * e.len * 0.5f - e.ex * 0.3f;
                face_signs(m, b, g, si, &fk, facade_top(g, si, px, pz), rp);
                if (g->groundMode == GF_LOBBY || g->groundMode == GF_DOUBLE) lobby_lights(m, g, si, k, e.len);
            }
            face_feature(m, g, s, e.len, rp);
            xf_restore(m, sv);
        }
    }
}

/* A rooftop billboard on a low building, facing a street. */
static void emit_billboards(Mesh *m, const Building *b, const BGraph *g, Rng *rp) {
    const float gy = G0;
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        if (has_above(g, si) || s->y1 - gy > 70.0f || s->roof == ROOF_HIP || s->roof == ROOF_PYRAMID || s->roof == ROOF_HELIPAD) continue;
        if (g->archetype != ARCH_OFFICE && g->archetype != ARCH_SHOPHOUSE && g->archetype != ARCH_SHED && g->archetype != ARCH_SLAB) continue;
        if (rng_float(rp) >= 0.3f) continue;
        const BLoop *l = &s->foot.outer;
        int best = -1;
        float bl = 0.0f;
        for (int k = 0; k < l->n; ++k) {
            EdgeInfo e = edge_of(l, k);
            if (face_toward(b, &e) && e.len > bl) { bl = e.len; best = k; }
        }
        if (best < 0 || bl < 8.0f) continue;
        Xf sv = xf_save(m);
        enter_face(m, l, best);
        Face f = face_local(bl);
        float w = minf(f.width - 2.0f, rng_range(rp, 8.0f, 18.0f));
        float h = minf(w * 0.42f, 8.0f);
        float u0 = (f.width - w) * 0.5f;
        float yb = s->y1 + rng_range(rp, 2.0f, 4.0f);
        Mat metal = mat(MAT_METAL, b->seed, 3, 0, 0);
        for (int leg = 0; leg < 3; ++leg) {
            float uu = u0 + w * (0.15f + 0.35f * leg);
            emit_box(m, v3(uu - 0.15f, s->y1, -1.6f), v3(uu + 0.15f, yb, -1.3f), F_NOBOT, metal, UV_FACADE);
        }
        emit_box(m, v3(u0 - 0.3f, yb, -1.7f), v3(u0 + w + 0.3f, yb + h + 0.3f, -1.25f), F_NOBOT | F_BOT, metal, UV_FACADE);
        if (rng_float(rp) < 0.4f)
            emit_face_panel(m, &f, u0, u0 + w, yb + 0.15f, yb + h + 0.15f, -1.2f, mat(MAT_SCREEN, rng_u32(rp), 0, q_half(w), q_half(h)));
        else {
            uint32_t seed = rng_u32(rp), col = rng_u32(rp) & 7;
            emit_face_panel(m, &f, u0, u0 + w, yb + 0.15f, yb + h + 0.15f, -1.2f,
                            mat(MAT_NEON, seed, (uint8_t)(col | (1 << 3)), q_half(w), q_half(h)));
        }
        xf_restore(m, sv);
        break;
    }
}

/* Everything that makes up one building, in its own frame. The building's
 * stream r is drawn from in this order: roofs and tops section by section,
 * then signs, then billboards. */
void emit_building(Mesh *m, const Building *b, const BGraph *g, const Interiors *in) {
    Rng r = rng_make(b->seed ^ 0x77u);
    int fins = 0, plant = bgen_industrial(g);
    float bay = bgen_bay_width(g->seed, g->fine);
    for (int si = 0; si < g->nsec; ++si) {
        if (!bsec_facade(&g->sec[si])) continue;   /* a plant's units and decks: emit_industry */
        Behind bh;
        emit_walls(m, b, g, si, behind_of(in, si, &bh) ? &bh : NULL);
        emit_section_caps(m, g, si, in);
        emit_face_trims(m, g, si, bay, &fins);
        dress_roof(m, g, si, &r);
        if (!has_above(g, si) && !plant) emit_tops(m, g, si, &r);
    }
    if (plant) {   /* no shop signs or billboards: its doors, then the plant itself */
        emit_doors(m, g, in);
        emit_industry(m, b, g);
        return;
    }
    emit_supports(m, g);
    if (in->arc) {   /* the portals replace the graph's doors */
        emit_arc_portals(m, g, in->arc);
        emit_arc_frontage(m, g, in->arc);
        emit_arc_terraces(m, g, in->arc);
    }
    else emit_doors(m, g, in);
    for (int k = 0; k < in->n; ++k) emit_interior_doors(m, g, in->lay[k]);   /* the lobbies themselves: city_build_interiors */
    emit_street_signs(m, b, g, &r);
    emit_billboards(m, b, g, &r);
}
