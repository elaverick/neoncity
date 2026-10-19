#include "city_internal.h"

/* ===========================================================================
 * Streets
 * ========================================================================= */

#define BASE_Y (-0.03f)   /* plain asphalt under everything; marked carriageways sit at 0 */

/* Now and then a street lamp is failing (lampState in shaders/common.glsl):
 * about one in 250 is dead, one in 250 cycles on and off as a worn sodium
 * lamp does, and one in 250 burns dim and red. State in the low 2 bits, a
 * seed for its rhythm above; the same for every chunk that sees the lamp. */
uint32_t lamp_fault(const Lamp *L) {
    uint32_t h = hash2i(WORLD_SEED ^ 0x1A3BADu, (int32_t)floor(L->x * 4.0), (int32_t)floor(L->z * 4.0));
    float r = hash_float(h);
    uint32_t state = r < 0.004f ? 1u : r < 0.008f ? 2u : r < 0.012f ? 3u : 0u;
    return state | (hash_u32(h) & 0x3FFFFFu) << 2;
}

/* Pole at the origin, arm along +x (the emission transform turns it to the
 * road and stands it on the ground). */
void build_lamp(Mesh *m, const ChunkGen *g, const Lamp *L, uint32_t idx) {
    m->xf = 1;
    m->xc = L->dx; m->xs = L->dz;
    m->xx = (float)(L->x - g->ox); m->xz = (float)(L->z - g->oz);
    m->xy = city_ground_y(L->x, L->z);
    const float gy = SIDEWALK_HEIGHT, top = LAMP_HEIGHT;
    Mat metal = mat(MAT_METAL, idx, 4, 0, 0);
    emit_box(m, v3(-0.11f, gy, -0.11f), v3(0.11f, top, 0.11f), F_NOBOT, metal, UV_FACADE);
    emit_box(m, v3(-0.2f, gy, -0.2f), v3(0.2f, gy + 1.0f, 0.2f), F_NOBOT, metal, UV_FACADE);
    emit_box(m, v3(-0.06f, top - 0.14f, -0.06f), v3(1.96f, top, 0.06f), F_NOBOT | F_BOT, metal, UV_FACADE);
    /* head: 1.7 m out, over the edge of the carriageway */
    const float hx = 1.7f, hw = 0.42f, hd = 0.2f;
    emit_box(m, v3(hx - hw, top - 0.32f, -hd), v3(hx + hw, top - 0.05f, hd), F_NOBOT, metal, UV_FACADE);
    float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    float y = top - 0.33f;
    emit_quad(m, v3(hx - hw + 0.04f, y, -hd + 0.04f), v3(hx + hw - 0.04f, y, -hd + 0.04f),
              v3(hx + hw - 0.04f, y, hd - 0.04f), v3(hx - hw + 0.04f, y, hd - 0.04f),
              v3(0, -1, 0), uv, mat(MAT_EMISSIVE, lamp_fault(L), 0, (uint8_t)(2 + L->type), 0));
    m->xf = 0;
}

static int clip_axis(const DVec2 *in, int n, int axis, double sign, double lim, DVec2 *out) {
    int k = 0;
    for (int i = 0; i < n; ++i) {
        DVec2 a = in[i], b = in[(i + 1) % n];
        double da = sign * ((axis ? a.z : a.x) - lim), db = sign * ((axis ? b.z : b.x) - lim);
        if (da <= 0.0) out[k++] = a;
        if ((da <= 0.0) != (db <= 0.0)) {
            double t = da / (da - db);
            out[k++] = dv(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
        }
    }
    return k;
}

/* Clip a convex polygon (chunk-local) to the chunk square. */
static int clip_chunk(const DVec2 *in, int n, DVec2 *out) {
    const double P = CHUNK_SIZE;
    DVec2 a[48], b[48];
    if (n > 40) n = 40;
    n = clip_axis(in, n, 0, -1.0, 0.0, a);
    n = clip_axis(a, n, 0, 1.0, P, b);
    n = clip_axis(b, n, 1, -1.0, 0.0, a);
    n = clip_axis(a, n, 1, 1.0, P, out);
    return n >= 3 ? n : 0;
}

static int clip_segment(DVec2 *a, DVec2 *b) {
    const double P = CHUNK_SIZE;
    double t0 = 0.0, t1 = 1.0, dx = b->x - a->x, dz = b->z - a->z;
    double p[4] = { -dx, dx, -dz, dz }, q[4] = { a->x, P - a->x, a->z, P - a->z };
    for (int k = 0; k < 4; ++k) {
        if (fabs(p[k]) < 1e-12) { if (q[k] < 0.0) return 0; continue; }
        double r = q[k] / p[k];
        if (p[k] < 0.0) t0 = fmax(t0, r); else t1 = fmin(t1, r);
    }
    if (t1 - t0 < 1e-6) return 0;
    DVec2 a0 = *a;
    *a = dv(a0.x + dx * t0, a0.z + dz * t0);
    *b = dv(a0.x + dx * t1, a0.z + dz * t1);
    return 1;
}

/* Where the city ends (PlanArea.bounded): the plain asphalt only over the
 * city's ground, and along its outline a face down past the land beyond,
 * which lies TERRAIN_URBAN_SINK_M below the city's ground there. */
#define EDGE_DROP 1.0f

static void build_city_edge(Mesh *m, const ChunkGen *g) {
    const PlanArea *pa = g->pa;
    float buv[48][2] = { { 0 } };
    for (int t = 0; t < pa->ntowns; ++t) {
        const Poly *gp = &pa->ground[t];
        DVec2 l[PLAN_MAX_PV], cl[48];
        for (int i = 0; i < gp->n; ++i) l[i] = dv(gp->v[i].x - g->ox, gp->v[i].z - g->oz);
        int k = clip_chunk(l, gp->n, cl);
        if (k) emit_flat(m, cl, k, BASE_Y, (const float (*)[2])buv, mat(MAT_ROAD, 0, 0, 0, 0));
    }
    for (int i = 0; i < pa->nedges; ++i) {
        const PlanEdge *e = &pa->edges[i];
        DVec2 a = dv(e->a.x - g->ox, e->a.z - g->oz), b = dv(e->b.x - g->ox, e->b.z - g->oz), a0 = a;
        if (!clip_segment(&a, &b)) continue;
        DVec2 eh = dnorm(dsub(e->b, e->a));
        vec3 nn = v3((float)e->n.x, 0, (float)e->n.z);
        float u0 = (float)dlen(dsub(a, a0)), u1 = (float)dlen(dsub(b, a0)), y0 = BASE_Y - EDGE_DROP;
        if (eh.x * nn.z - eh.z * nn.x < 0) { u0 = -u0; u1 = -u1; }   /* as kerb_face */
        float quv[4][2] = { { u0, y0 }, { u1, y0 }, { u1, BASE_Y }, { u0, BASE_Y } };
        emit_quad(m, v3((float)a.x, y0, (float)a.z), v3((float)b.x, y0, (float)b.z),
                  v3((float)b.x, BASE_Y, (float)b.z), v3((float)a.x, BASE_Y, (float)a.z), nn, quv, mat(MAT_CURB, 0, 0, 0, 0));
    }
}

/* Carriageways, cut back at junctions so markings never overlap. uv = (offset
 * from the centre line, distance along); seed carries the length for the
 * crossings at each end, params the half-width (quarter metres), crossings |
 * squalor (0..127) << 1, and class. */
void build_roads(Mesh *m, const ChunkGen *g) {
    const double P = CHUNK_SIZE;
    if (!g->pa->bounded) {
        DVec2 base[4] = { {0, 0}, {P, 0}, {P, P}, {0, P} };
        float buv[4][2] = { {0, 0}, {0, 0}, {0, 0}, {0, 0} };
        emit_flat(m, base, 4, BASE_Y, buv, mat(MAT_ROAD, 0, 0, 0, 0));
    } else {
        build_city_edge(m, g);
    }

    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        const Road *r = &g->pa->roads[ri];
        DVec2 a = dv(r->a.x - g->ox, r->a.z - g->oz), b = dv(r->b.x - g->ox, r->b.z - g->oz);
        double len = dlen(dsub(b, a));
        double L = len - r->trimA - r->trimB;
        if (len < 1.0 || L < 1.0) continue;
        DVec2 dir = dscale(dsub(b, a), 1.0 / len), nrm = dv(-dir.z, dir.x);
        DVec2 s0 = dadd(a, dscale(dir, r->trimA)), s1 = dadd(a, dscale(dir, len - r->trimB));
        double h = r->at.half;
        DVec2 q[4] = { dadd(s0, dscale(nrm, h)), dadd(s1, dscale(nrm, h)), dsub(s1, dscale(nrm, h)), dsub(s0, dscale(nrm, h)) };
        DVec2 c[48];
        int n = clip_chunk(q, 4, c);
        if (!n) continue;
        float uv[48][2];
        for (int i = 0; i < n; ++i) {
            DVec2 d = dsub(c[i], s0);
            uv[i][0] = (float)ddot(d, nrm);
            uv[i][1] = (float)ddot(d, dir);
        }
        float Lf = (float)L;
        uint32_t lbits;
        memcpy(&lbits, &Lf, 4);
        uint8_t hq = (uint8_t)clampf(r->at.half * 4.0f + 0.5f, 1.0f, 255.0f);
        uint8_t sq = (uint8_t)(city_squalor(-1, (r->a.x + r->b.x) * 0.5, (r->a.z + r->b.z) * 0.5) * 127.0f + 0.5f);
        emit_flat(m, c, n, 0.0f, (const float (*)[2])uv, mat(MAT_ROAD, lbits, hq, (uint8_t)((L > 16.0 ? 1 : 0) | sq << 1), r->at.cls));
    }
}

/* Pieces of convex polygons cut round other convex polygons (the sidewalk
 * round its drop kerbs, a slope round its tactile slabs). */
#define MAX_SHARDS 96
typedef struct Shard { int n; DVec2 v[48]; } Shard;

/* Replaces the *ns shards in s with their parts outside convex polygon b;
 * tmp is scratch for MAX_SHARDS more. */
static void shards_minus(Shard *s, int *ns, Shard *tmp, const DVec2 *b, int nb) {
    double bx0 = 1e300, bz0 = 1e300, bx1 = -1e300, bz1 = -1e300;
    for (int i = 0; i < nb; ++i) {
        bx0 = fmin(bx0, b[i].x); bx1 = fmax(bx1, b[i].x);
        bz0 = fmin(bz0, b[i].z); bz1 = fmax(bz1, b[i].z);
    }
    double sg = area2(b, nb) >= 0.0 ? 1.0 : -1.0;
    int nt = 0;
    for (int k = 0; k < *ns; ++k) {
        const Shard *a = &s[k];
        double ax0 = 1e300, az0 = 1e300, ax1 = -1e300, az1 = -1e300;
        for (int i = 0; i < a->n; ++i) {
            ax0 = fmin(ax0, a->v[i].x); ax1 = fmax(ax1, a->v[i].x);
            az0 = fmin(az0, a->v[i].z); az1 = fmax(az1, a->v[i].z);
        }
        if (ax1 <= bx0 || ax0 >= bx1 || az1 <= bz0 || az0 >= bz1) {
            if (nt < MAX_SHARDS) tmp[nt++] = *a;
            continue;
        }
        /* outside each of b's edges in turn, inside the ones before */
        DVec2 cur[48], nxt[48];
        int nc = a->n;
        memcpy(cur, a->v, sizeof(DVec2) * nc);
        for (int j = 0; j < nb && nc; ++j) {
            DVec2 e = dsub(b[(j + 1) % nb], b[j]), out = dv(e.z * sg, -e.x * sg);
            double c = ddot(b[j], out);
            if (nt < MAX_SHARDS) {
                int m = clip_hp(cur, NULL, nc, dv(-out.x, -out.z), -c, 0, tmp[nt].v, NULL);
                if (m && fabs(area2(tmp[nt].v, m)) > 1e-6) tmp[nt++].n = m;
            }
            nc = clip_hp(cur, NULL, nc, out, c, 0, nxt, NULL);
            memcpy(cur, nxt, sizeof(DVec2) * nc);
        }
    }
    memcpy(s, tmp, sizeof(Shard) * nt);
    *ns = nt;
}

/* The kerb's face to the road, from t0 to t1 along the edge from c (unit eh), up to y1. */
static void kerb_face(Mesh *m, DVec2 c, DVec2 eh, double t0, double t1, vec3 nn, float y1) {
    DVec2 a = dadd(c, dscale(eh, t0)), b = dadd(c, dscale(eh, t1));
    if (!clip_segment(&a, &b)) return;
    float u0 = (float)dlen(dsub(a, c)), u1 = (float)dlen(dsub(b, c));
    /* u runs along cross(up, n), so the shader can rebuild the face's tangent from its normal. */
    if (eh.x * nn.z - eh.z * nn.x < 0) { u0 = -u0; u1 = -u1; }
    float quv[4][2] = { { u0, BASE_Y }, { u1, BASE_Y }, { u1, y1 }, { u0, y1 } };
    emit_quad(m, v3((float)a.x, BASE_Y, (float)a.z), v3((float)b.x, BASE_Y, (float)b.z),
              v3((float)b.x, y1, (float)b.z), v3((float)a.x, y1, (float)a.z), nn, quv, mat(MAT_CURB, 0, 0, 0, 0));
}

/* A convex piece of a drop kerb's slope (chunk-local; o, t, nn its frame):
 * its height follows d, the distance in from the kerb line. uv = (u - du, d - dd). */
static void emit_slope(Mesh *m, const DVec2 *p, int n, DVec2 o, DVec2 t, DVec2 nn, double du, double dd, Mat mt) {
    const float k = (SIDEWALK_HEIGHT - DROP_KERB_HEIGHT) / DROP_RAMP_RUN;
    vec3 N = v3_norm(v3((float)(-k * nn.x), 1.0f, (float)(-k * nn.z)));
    for (int i = 1; i + 1 < n; ++i) {
        int id[3] = { 0, i, i + 1 };
        vec3 q[3];
        float uv[3][2];
        for (int j = 0; j < 3; ++j) {
            DVec2 r = dsub(p[id[j]], o);
            double d = ddot(r, nn);
            q[j] = v3((float)p[id[j]].x, drop_y(d), (float)p[id[j]].z);
            uv[j][0] = (float)(ddot(r, t) - du);
            uv[j][1] = (float)(d - dd);
        }
        emit_tri(m, q[0], q[1], q[2], N, uv, mt);
    }
}

/* A drop kerb (outline v, chunk-local): the lowered kerb stones, the slope
 * behind them round its tactile slabs, and the kerb-stone sides. The stones,
 * slope and slabs are marked lowered (params.w = 1) for the water they hold. */
static void emit_drop(Mesh *m, const ChunkGen *g, const DropKerb *D, const DVec2 *v, uint8_t sq, Shard *sh, Shard *tmp) {
    const float gy = SIDEWALK_HEIGHT;
    const double KS = KERB_STONE_WIDTH;
    DVec2 o = dv(D->ox - g->ox, D->oz - g->oz), t = dv(D->tx, D->tz), nn = dv(D->nx, D->nz);
    double on = ddot(o, nn);
    float ea = (float)atan2(t.z, t.x);
    uint32_t ebits;
    memcpy(&ebits, &ea, 4);
    DVec2 p[48], q[48], cl[48];
    float uv[48][2];
    int k;

    /* the lowered kerb stones, laid as the kerb's own (uv along, and in from the kerb line) */
    k = clip_hp(v, NULL, D->n, nn, on + KS, 0, p, NULL);
    k = k ? clip_chunk(p, k, cl) : 0;
    for (int i = 0; i < k; ++i) { DVec2 r = dsub(cl[i], o); uv[i][0] = (float)ddot(r, t); uv[i][1] = (float)ddot(r, nn); }
    if (k) emit_flat(m, cl, k, DROP_KERB_HEIGHT + 0.003f, (const float (*)[2])uv, mat(MAT_SIDEWALK, ebits, 1, sq, 1));

    /* the slope, round its tactile slabs; the slabs' uv count from the slab grid */
    int ns = 0;
    k = clip_hp(v, NULL, D->n, dv(-nn.x, -nn.z), -(on + KS), 0, sh[0].v, NULL);
    if (k) { sh[0].n = k; ns = 1; }
    DVec2 slab[DROP_ROWS][4];
    int rows[DROP_ROWS];
    for (int r = 0; r < DROP_ROWS; ++r) {
        double d0 = KS + r * DROP_TILE, d1 = d0 + DROP_TILE, u0 = D->tu[r][0], u1 = D->tu[r][1];
        rows[r] = u1 - u0 > 1e-6;
        if (!rows[r]) continue;
        slab[r][0] = dadd(o, dadd(dscale(t, u0), dscale(nn, d0)));
        slab[r][1] = dadd(o, dadd(dscale(t, u1), dscale(nn, d0)));
        slab[r][2] = dadd(o, dadd(dscale(t, u1), dscale(nn, d1)));
        slab[r][3] = dadd(o, dadd(dscale(t, u0), dscale(nn, d1)));
        shards_minus(sh, &ns, tmp, slab[r], 4);
    }
    for (int s = 0; s < ns; ++s) {
        k = clip_chunk(sh[s].v, sh[s].n, cl);
        if (k) emit_slope(m, cl, k, o, t, nn, 0.0, 0.0, mat(MAT_SIDEWALK, ebits, 2, sq, 1));
    }
    for (int r = 0; r < DROP_ROWS; ++r) {
        k = rows[r] ? clip_chunk(slab[r], 4, cl) : 0;
        if (k) emit_slope(m, cl, k, o, t, nn, D->tc, KS, mat(MAT_SIDEWALK, ebits, 3, sq, 1));
    }

    /* the sides: a kerb-stone wall from the slope up to the sidewalk, with
     * kerb stones along its top on the sidewalk side */
    DVec2 vc = dv(0, 0);
    for (int i = 0; i < D->n; ++i) vc = dadd(vc, v[i]);
    vc = dscale(vc, 1.0 / D->n);
    for (int i = 0; i < D->n; ++i) {
        if (D->lab[i] != DE_SIDE) continue;
        DVec2 a = v[i], b = v[(i + 1) % D->n], e = dsub(b, a);
        if (dlen(e) < 1e-6) continue;
        DVec2 w = dnorm(dv(-e.z, e.x));
        if (ddot(w, dsub(vc, a)) < 0) w = dv(-w.x, -w.z);   /* into the cut */
        vec3 wn = v3((float)w.x, 0.0f, (float)w.z);
        DVec2 cr = dv(w.z, -w.x);                             /* cross(up, n), as the kerb faces */
        double da = ddot(a, nn) - on, db = ddot(b, nn) - on;
        DVec2 pts[3] = { a, b, b };
        int np = 2;
        if ((da - KS) * (db - KS) < 0.0) { pts[1] = dadd(a, dscale(e, (KS - da) / (db - da))); np = 3; }
        Mat face = mat(MAT_CURB, 0, 0, 0, 0);
        for (int s = 0; s + 1 < np; ++s) {
            DVec2 A = pts[s], B = pts[s + 1];
            if (!clip_segment(&A, &B)) continue;
            float ya = drop_y(ddot(A, nn) - on), yb = drop_y(ddot(B, nn) - on);
            if (ya > gy - 1e-4f && yb > gy - 1e-4f) continue;
            float ua = (float)ddot(dsub(A, o), cr), ub = (float)ddot(dsub(B, o), cr);
            vec3 A0 = v3((float)A.x, ya, (float)A.z), A1 = v3((float)A.x, gy, (float)A.z);
            vec3 B0 = v3((float)B.x, yb, (float)B.z), B1 = v3((float)B.x, gy, (float)B.z);
            if (ya > gy - 1e-4f) {
                float tuv[3][2] = { { ua, gy }, { ub, yb }, { ub, gy } };
                emit_tri(m, A1, B0, B1, wn, tuv, face);
            } else if (yb > gy - 1e-4f) {
                float tuv[3][2] = { { ua, ya }, { ub, gy }, { ua, gy } };
                emit_tri(m, A0, B1, A1, wn, tuv, face);
            } else {
                float quv[4][2] = { { ua, ya }, { ub, yb }, { ub, gy }, { ua, gy } };
                emit_quad(m, A0, B0, B1, A1, wn, quv, face);
            }
        }
        /* its kerb stones: from the lowered kerb's back to the top of the slope,
         * and no further than a corner shared with the next drop */
        DVec2 out = dv(-w.x, -w.z), al = dv(out.z, -out.x);
        DVec2 st[4] = { a, b, dadd(b, dscale(out, KS)), dadd(a, dscale(out, KS)) };
        k = clip_hp(st, NULL, 4, dv(-nn.x, -nn.z), -(on + KS), 0, p, NULL);
        k = k ? clip_hp(p, NULL, k, nn, on + DROP_DEPTH, 0, q, NULL) : 0;
        for (int j = 0; j < D->nopen && k; ++j) {
            DVec2 nr = dv(D->open[j][0], D->open[j][1]);
            k = clip_hp(q, NULL, k, nr, D->open[j][2] - (nr.x * g->ox + nr.z * g->oz), 0, p, NULL);
            memcpy(q, p, sizeof(DVec2) * k);
        }
        k = k ? clip_chunk(q, k, cl) : 0;
        if (!k) continue;
        float sa = (float)atan2(al.z, al.x);
        uint32_t sbits;
        memcpy(&sbits, &sa, 4);
        for (int j = 0; j < k; ++j) { DVec2 r = dsub(cl[j], a); uv[j][0] = (float)ddot(r, al); uv[j][1] = (float)ddot(r, out); }
        emit_flat(m, cl, k, gy + 0.003f, (const float (*)[2])uv, mat(MAT_SIDEWALK, sbits, 1, sq, 0));
    }
}

/* A raised block in chunk-local coordinates: its kerb outline c, the inward
 * edge normals nin, the mitred inner corners of the kerb-stone strip, and
 * the drop kerbs cut into it (outlines dl). */
typedef struct BlockFrame {
    const Block *bk;
    int      n;
    DVec2    c[PLAN_MAX_PV], in[PLAN_MAX_PV], nin[PLAN_MAX_PV];
    double   wx, wz;        /* wrapped offset, as gWorld in the shader */
    float    gc, gs;        /* the block's grain */
    uint32_t gbits;         /* the grain angle (float bits): the tangent frame for normal-mapped paving */
    uint8_t  sq;            /* squalor, 0..255 */
    int      nd;
    DropKerb dk[MAX_BLOCK_DROPS];
    DVec2    dl[MAX_BLOCK_DROPS][MAX_DROP_PV];
} BlockFrame;

static void block_frame(const ChunkGen *g, const Block *bk, BlockFrame *F) {
    const double P = CHUNK_SIZE;
    int n = bk->curb.n;
    DVec2 *c = F->c, *in = F->in, *nin = F->nin, cen = dv(0, 0);
    F->bk = bk;
    F->n = n;
    F->wx = (g->cx & 63) * P; F->wz = (g->cz & 63) * P;
    for (int i = 0; i < n; ++i) {
        c[i] = dv(bk->curb.v[i].x - g->ox, bk->curb.v[i].z - g->oz);
        cen = dadd(cen, c[i]);
    }
    cen = dscale(cen, 1.0 / n);
    /* Inward edge normals and mitred inner corners for the kerb-stone strip. */
    for (int i = 0; i < n; ++i) {
        DVec2 d = dsub(c[(i + 1) % n], c[i]);
        nin[i] = dlen(d) > 1e-6 ? dnorm(dv(-d.z, d.x)) : (i ? nin[i - 1] : dv(0, 0));
        if (ddot(nin[i], dsub(cen, c[i])) < 0) nin[i] = dv(-nin[i].x, -nin[i].z);
    }
    for (int i = 0; i < n; ++i) {
        DVec2 n0 = nin[(i + n - 1) % n], n1 = nin[i];
        DVec2 off = dscale(dadd(n0, n1), KERB_STONE_WIDTH / fmax(1.0 + ddot(n0, n1), 0.05));
        if (dlen(off) > 1.2) off = dscale(dnorm(off), 1.2);
        in[i] = dadd(c[i], off);
    }
    F->gc = bk->gc; F->gs = bk->gs;
    DVec2 bc = poly_centroid(&bk->curb);
    F->sq = (uint8_t)(city_squalor(bk->zone, bc.x, bc.z) * 255.0f + 0.5f);
    float grain = (float)atan2(F->gs, F->gc);
    memcpy(&F->gbits, &grain, 4);
    F->nd = city_block_drops(g->pa, bk, F->dk, MAX_BLOCK_DROPS);
    for (int d = 0; d < F->nd; ++d)
        for (int i = 0; i < F->dk[d].n; ++i) F->dl[d][i] = dv(F->dk[d].v[i][0] - g->ox, F->dk[d].v[i][1] - g->oz);
}

/* Paving uv: world metres along and across the block's grain. */
static void pave_uv(const BlockFrame *F, const DVec2 *cl, int k, float (*uv)[2]) {
    for (int i = 0; i < k; ++i) {
        double X = cl[i].x + F->wx, Z = cl[i].z + F->wz;
        uv[i][0] = (float)(F->gc * X + F->gs * Z); uv[i][1] = (float)(-F->gs * X + F->gc * Z);
    }
}

/* The sidewalk, round the drop kerbs. */
static void emit_sidewalk(Mesh *m, const BlockFrame *F, Shard *sh, Shard *tmp) {
    DVec2 cl[48];
    float uv[48][2];
    int ns = 1;
    sh[0].n = F->n;
    memcpy(sh[0].v, F->c, sizeof(DVec2) * F->n);
    for (int d = 0; d < F->nd; ++d) shards_minus(sh, &ns, tmp, F->dl[d], F->dk[d].n);
    for (int s = 0; s < ns; ++s) {
        int k = clip_chunk(sh[s].v, sh[s].n, cl);
        pave_uv(F, cl, k, uv);
        if (k) emit_flat(m, cl, k, SIDEWALK_HEIGHT, (const float (*)[2])uv, mat(MAT_SIDEWALK, F->gbits, 0, F->sq, 0));
    }
}

/* The kerb stones along edge i, round the drop kerbs. They are laid along
 * the edge: u runs along it and v inward from the outer edge (metres), so the
 * shader knows where the joint with the paving falls (v = KERB_STONE_WIDTH).
 * The inward normal is u turned 90 degrees, (-e.z, e.x), so the edge angle in
 * the seed rebuilds the whole frame. */
static void emit_kerb_stones(Mesh *m, const BlockFrame *F, int i, Shard *sh, Shard *tmp) {
    const DVec2 *c = F->c, *in = F->in, *nin = F->nin;
    int j = (i + 1) % F->n;
    DVec2 cl[48];
    float uv[48][2];
    DVec2 along = dv(nin[i].z, -nin[i].x);
    float ea = (float)atan2(along.z, along.x);
    uint32_t ebits;
    memcpy(&ebits, &ea, 4);
    int ns = 1;
    sh[0].n = 4;
    sh[0].v[0] = c[i]; sh[0].v[1] = c[j]; sh[0].v[2] = in[j]; sh[0].v[3] = in[i];
    for (int d = 0; d < F->nd; ++d) shards_minus(sh, &ns, tmp, F->dl[d], F->dk[d].n);
    for (int s = 0; s < ns; ++s) {
        int k = clip_chunk(sh[s].v, sh[s].n, cl);
        for (int t = 0; t < k; ++t) {
            DVec2 d = dsub(cl[t], c[i]);
            uv[t][0] = (float)ddot(d, along);
            uv[t][1] = (float)ddot(d, nin[i]);
        }
        if (k) emit_flat(m, cl, k, SIDEWALK_HEIGHT + 0.003f, (const float (*)[2])uv, mat(MAT_SIDEWALK, ebits, 1, F->sq, 0));
    }
}

/* The kerb face along edge i, lowered across each drop kerb cut into it. */
static void emit_kerb_faces(Mesh *m, const BlockFrame *F, int i) {
    const DVec2 *c = F->c, *nin = F->nin;
    int j = (i + 1) % F->n;
    DVec2 e = dsub(c[j], c[i]);
    double el = dlen(e);
    if (el < 1e-9) return;
    DVec2 eh = dscale(e, 1.0 / el);
    vec3 nn = v3((float)-nin[i].x, 0, (float)-nin[i].z);
    double cut[MAX_BLOCK_DROPS][2];
    int ncut = 0;
    for (int d = 0; d < F->nd; ++d) {
        const DropKerb *dk = &F->dk[d];
        if (dk->edge != i) continue;
        for (int q = 0; q < dk->n; ++q) {
            if (dk->lab[q] != DE_KERB) continue;
            double t0 = ddot(dsub(F->dl[d][q], c[i]), eh), t1 = ddot(dsub(F->dl[d][(q + 1) % dk->n], c[i]), eh);
            int at = ncut++;
            while (at > 0 && cut[at - 1][0] > fmin(t0, t1)) { cut[at][0] = cut[at - 1][0]; cut[at][1] = cut[at - 1][1]; --at; }
            cut[at][0] = fmin(t0, t1); cut[at][1] = fmax(t0, t1);
        }
    }
    double t = 0.0;
    for (int q = 0; q <= ncut; ++q) {
        double t0 = q < ncut ? fmax(cut[q][0], t) : el;
        if (t0 > t + 1e-6) kerb_face(m, c[i], eh, t, t0, nn, SIDEWALK_HEIGHT);
        if (q == ncut) break;
        if (cut[q][1] > t0 + 1e-6) kerb_face(m, c[i], eh, t0, cut[q][1], nn, DROP_KERB_HEIGHT);
        t = fmax(t, cut[q][1]);
    }
}

/* The lot inside the block (a park or a building plot). */
static void emit_lot(Mesh *m, const ChunkGen *g, const BlockFrame *F) {
    const Block *bk = F->bk;
    if (!bk->lot.n) return;
    DVec2 l[PLAN_MAX_PV], cl[48];
    float uv[48][2];
    for (int i = 0; i < bk->lot.n; ++i) l[i] = dv(bk->lot.v[i].x - g->ox, bk->lot.v[i].z - g->oz);
    int k = clip_chunk(l, bk->lot.n, cl);
    pave_uv(F, cl, k, uv);
    uint8_t green = bk->zone == ZONE_PARK || bk->zone == ZONE_RURAL;   /* open land is grass, as a park is */
    if (k) emit_flat(m, cl, k, SIDEWALK_HEIGHT + 0.006f, (const float (*)[2])uv, mat(MAT_LOT, 0, green, F->sq, 0));
}

/* Raised blocks: sidewalk, kerb stones, kerb faces, drop kerbs and the lot
 * inside. The paving and lot carry the block's squalor (0..255) in params.z. */
void build_blocks(Mesh *m, const ChunkGen *g) {
    const double P = CHUNK_SIZE;
    Shard *sh = (Shard *)malloc(sizeof(Shard) * MAX_SHARDS * 2);
    BlockFrame *F = (BlockFrame *)malloc(sizeof(BlockFrame));
    if (!sh || !F) fatal("out of memory building sidewalks");
    Shard *tmp = sh + MAX_SHARDS;
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < g->ox || bk->x0 > g->ox + P || bk->z1 < g->oz || bk->z0 > g->oz + P) continue;
        block_frame(g, bk, F);
        emit_sidewalk(m, F, sh, tmp);
        for (int i = 0; i < F->n; ++i) {
            emit_kerb_stones(m, F, i, sh, tmp);
            emit_kerb_faces(m, F, i);
        }
        for (int d = 0; d < F->nd; ++d) emit_drop(m, g, &F->dk[d], F->dl[d], F->sq, sh, tmp);
        emit_lot(m, g, F);
    }
    free(F);
    free(sh);
}
