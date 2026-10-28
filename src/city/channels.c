#include "city_internal.h"

/* ===========================================================================
 * The rivers' channels through the city (plan.h, "Rivers through the
 * cities"): their walls, floors and parapets, and the bridges' decks over
 * them, as the streets draw them (build_channels) and the walker meets them
 * (city_layout).
 *
 * A wall stands on every edge of a piece of channel but an open one, save
 * where another piece lies against it or over it (the next piece across a
 * road, a river crossing another); out in the belt, only over the city's
 * ground. A bank's wall carries a parapet, stopped where a bridge's deck
 * crosses it. A deck spans the channel the width of its road and sidewalks,
 * a parapet along each side, its fascia and soffit DECK_DEPTH deep, resting
 * on the walls. Heights are over the city's ground (the mesh drapes them).
 * ========================================================================= */

#define WALL_EPS   0.005   /* m: a wall this near another piece's outline lies against it */
#define MAX_SPANS  16

typedef struct Span { double t0, t1; } Span;

static double orient(const DVec2 *v, int n) {
    double a = 0.0;
    for (int i = 1; i + 1 < n; ++i) a += dcross(dsub(v[i], v[0]), dsub(v[i + 1], v[0]));
    return a >= 0.0 ? 1.0 : -1.0;
}

int city_seg_in(DVec2 p0, DVec2 p1, const DVec2 *v, int n, double g, double *t0, double *t1) {
    double o = orient(v, n), lo = 0.0, hi = 1.0;
    DVec2 d = dsub(p1, p0);
    for (int i = 0; i < n; ++i) {
        DVec2 a = v[i], e = dsub(v[(i + 1) % n], a);
        double l = dlen(e);
        if (l < 1e-9) continue;
        double k0 = o * dcross(e, dsub(p0, a)) / l + g, k1 = o * dcross(e, d) / l;   /* inside: k0 + t k1 >= 0 */
        if (fabs(k1) < 1e-15) { if (k0 < 0.0) return 0; continue; }
        double t = -k0 / k1;
        if (k1 > 0.0) lo = fmax(lo, t); else hi = fmin(hi, t);
    }
    if (hi - lo <= 1e-9) return 0;
    *t0 = lo; *t1 = hi;
    return 1;
}

/* s[n] less [a, b]. */
static int spans_minus(Span *s, int n, double a, double b) {
    Span t[MAX_SPANS];
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (b <= s[i].t0 || a >= s[i].t1) { if (k < MAX_SPANS) t[k++] = s[i]; continue; }
        if (a > s[i].t0 && k < MAX_SPANS) { t[k].t0 = s[i].t0; t[k].t1 = a; k++; }
        if (b < s[i].t1 && k < MAX_SPANS) { t[k].t0 = b; t[k].t1 = s[i].t1; k++; }
    }
    memcpy(s, t, sizeof(Span) * (size_t)k);
    return k;
}

/* s[n] within the union of u[nu]. */
static int spans_within(Span *s, int n, const Span *u, int nu) {
    Span t[MAX_SPANS];
    int k = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < nu; ++j) {
            double a = fmax(s[i].t0, u[j].t0), b = fmin(s[i].t1, u[j].t1);
            if (b > a && k < MAX_SPANS) { t[k].t0 = a; t[k].t1 = b; k++; }
        }
    memcpy(s, t, sizeof(Span) * (size_t)k);
    return k;
}

/* A bridge's deck as a convex outline: from s0 to s1 along its road, its
 * road's and sidewalks' width across. */
static void deck_rect(const PlanBridge *b, DVec2 *v) {
    DVec2 n = dv(-b->d.z, b->d.x);
    double w = b->half + b->walk;
    v[0] = dadd(dadd(b->p, dscale(b->d, b->s0)), dscale(n, -w));
    v[1] = dadd(dadd(b->p, dscale(b->d, b->s1)), dscale(n, -w));
    v[2] = dadd(dadd(b->p, dscale(b->d, b->s1)), dscale(n, w));
    v[3] = dadd(dadd(b->p, dscale(b->d, b->s0)), dscale(n, w));
}

int city_channel_walls(const PlanArea *pa, ChannelWall *out, int max) {
    int nw = 0;
    for (int k = 0; k < pa->nchannels; ++k) {
        const PlanChannel *c = &pa->channels[k];
        double o = orient(c->v, c->n);
        for (int i = 0; i < c->n; ++i) {
            if (c->edge[i] == CHANNEL_EDGE_OPEN) continue;
            DVec2 a = c->v[i], b = c->v[(i + 1) % c->n], e = dsub(b, a);
            double len = dlen(e), t0, t1;
            if (len < 0.05) continue;
            Span s[MAX_SPANS] = { { 0.0, 1.0 } };
            int ns = 1;
            for (int q = 0; q < pa->nchannels && ns; ++q) {
                if (q == k) continue;
                const PlanChannel *h = &pa->channels[q];
                if (h->x1 < fmin(a.x, b.x) - 1.0 || h->x0 > fmax(a.x, b.x) + 1.0 || h->z1 < fmin(a.z, b.z) - 1.0 || h->z0 > fmax(a.z, b.z) + 1.0) continue;
                if (city_seg_in(a, b, h->v, h->n, WALL_EPS, &t0, &t1)) ns = spans_minus(s, ns, t0, t1);
            }
            if (!c->built && pa->bounded) {   /* out in the belt, over the city's ground only */
                Span u[MAX_SPANS];
                int nu = 0;
                for (int g = 0; g < pa->ntowns && nu < MAX_SPANS; ++g)
                    if (city_seg_in(a, b, pa->ground[g].v, pa->ground[g].n, 0.0, &t0, &t1)) { u[nu].t0 = t0; u[nu].t1 = t1; nu++; }
                ns = spans_within(s, ns, u, nu);
            }
            DVec2 in = dscale(dv(-e.z * o, e.x * o), 1.0 / len);
            for (int q = 0; q < ns && nw < max; ++q) {
                if ((s[q].t1 - s[q].t0) * len < 0.02) continue;
                ChannelWall *w = &out[nw++];
                w->a = dadd(a, dscale(e, s[q].t0));
                w->b = dadd(a, dscale(e, s[q].t1));
                w->in = in;
                w->bank = c->edge[i] == CHANNEL_EDGE_BANK;
            }
        }
    }
    return nw;
}

int city_channel_parapets(const PlanArea *pa, const ChannelWall *w, int nw, Parapet *out, int max) {
    int np = 0;
    /* along the banks, stopped at the decks */
    for (int i = 0; i < nw; ++i) {
        if (!w[i].bank) continue;
        Span s[MAX_SPANS] = { { 0.0, 1.0 } };
        int ns = 1;
        double t0, t1;
        for (int b = 0; b < pa->nbridges && ns; ++b) {
            DVec2 r[4];
            deck_rect(&pa->bridges[b], r);
            if (city_seg_in(w[i].a, w[i].b, r, 4, 0.0, &t0, &t1)) ns = spans_minus(s, ns, t0, t1);
        }
        DVec2 e = dsub(w[i].b, w[i].a);
        for (int q = 0; q < ns && np < max; ++q) {
            if ((s[q].t1 - s[q].t0) * dlen(e) < 0.05) continue;
            Parapet *p = &out[np++];
            p->a = dadd(w[i].a, dscale(e, s[q].t0));
            p->b = dadd(w[i].a, dscale(e, s[q].t1));
            p->out = dv(-w[i].in.x, -w[i].in.z);
            p->base = 0.0f;
        }
    }
    /* along each side of each deck, over the channel */
    for (int b = 0; b < pa->nbridges; ++b) {
        const PlanBridge *br = &pa->bridges[b];
        DVec2 n = dv(-br->d.z, br->d.x);
        double wd = br->half + br->walk;
        for (int side = -1; side <= 1; side += 2) {
            DVec2 a = dadd(dadd(br->p, dscale(br->d, br->s0)), dscale(n, side * wd));
            DVec2 e = dscale(br->d, br->s1 - br->s0);
            Span u[MAX_SPANS];
            int nu = 0;
            double t0, t1;
            for (int k = 0; k < pa->nchannels && nu < MAX_SPANS; ++k)
                if (city_seg_in(a, dadd(a, e), pa->channels[k].v, pa->channels[k].n, 0.0, &t0, &t1)) { u[nu].t0 = t0; u[nu].t1 = t1; nu++; }
            /* the union, in order */
            for (int i = 1; i < nu; ++i)
                for (int j = i; j > 0 && u[j - 1].t0 > u[j].t0; --j) { Span t = u[j]; u[j] = u[j - 1]; u[j - 1] = t; }
            for (int i = 0; i < nu && np < max; ) {
                double lo = u[i].t0, hi = u[i].t1;
                int j = i + 1;
                while (j < nu && u[j].t0 <= hi + 1e-6) { hi = fmax(hi, u[j].t1); ++j; }
                i = j;
                Parapet *p = &out[np++];
                p->a = dadd(a, dscale(e, lo));
                p->b = dadd(a, dscale(e, hi));
                p->out = dv(-n.x * side, -n.z * side);   /* onto the deck */
                p->base = SIDEWALK_HEIGHT;
            }
        }
    }
    return np;
}

int city_bridge_decks(const PlanArea *pa, DeckPiece *out, int max) {
    int nd = 0;
    for (int b = 0; b < pa->nbridges; ++b) {
        DVec2 r[4];
        deck_rect(&pa->bridges[b], r);
        for (int k = 0; k < pa->nchannels && nd < max; ++k) {
            const PlanChannel *c = &pa->channels[k];
            DVec2 cur[48], nxt[48];
            int n = 4;
            memcpy(cur, r, sizeof r);
            double o = orient(c->v, c->n);
            for (int i = 0; i < c->n && n; ++i) {
                DVec2 a = c->v[i], e = dsub(c->v[(i + 1) % c->n], a);
                DVec2 outn = dv(o * e.z, -o * e.x);   /* out of c */
                n = clip_hp(cur, NULL, n, outn, ddot(a, outn), 0, nxt, NULL);
                memcpy(cur, nxt, sizeof(DVec2) * (size_t)n);
            }
            if (n < 3 || n > DECK_PV || fabs(area2(cur, n)) < 1e-3) continue;
            DeckPiece *d = &out[nd++];
            d->bridge = b;
            d->n = n;
            memcpy(d->v, cur, sizeof(DVec2) * (size_t)n);
        }
    }
    return nd;
}

/* --- The mesh ------------------------------------------------------------------------ */

#define MESH_WALLS    512
#define MESH_PARAPETS 512
#define MESH_DECKS    64

static DVec2 local(const ChunkGen *g, DVec2 p) { return dv(p.x - g->ox, p.z - g->oz); }

/* Clips a convex polygon (chunk-local) to the chunk square; the streets' clip. */
static int clip_square(const DVec2 *in, int n, DVec2 *out) {
    const double P = CHUNK_SIZE;
    DVec2 a[48], b[48];
    if (n > 40) n = 40;
    n = clip_hp(in, NULL, n, dv(-1, 0), 0.0, 0, a, NULL);
    if (n) n = clip_hp(a, NULL, n, dv(1, 0), P, 0, b, NULL);
    if (n) n = clip_hp(b, NULL, n, dv(0, -1), 0.0, 0, a, NULL);
    if (n) n = clip_hp(a, NULL, n, dv(0, 1), P, 0, out, NULL);
    return n;
}

static int clip_seg(DVec2 *a, DVec2 *b) {
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

/* A vertical face along a b (chunk-local) from y0 up to y1, facing n; uv
 * (metres along cross(up, n), height) as the kerbs' faces. */
static void face(Mesh *m, DVec2 a, DVec2 b, float y0, float y1, DVec2 n, Mat mt) {
    if (!clip_seg(&a, &b)) return;
    DVec2 eh = dnorm(dsub(b, a));
    float u0 = (float)ddot(a, eh), u1 = (float)ddot(b, eh);
    if (eh.x * n.z - eh.z * n.x < 0) { u0 = -u0; u1 = -u1; }
    float uv[4][2] = { { u0, y0 }, { u1, y0 }, { u1, y1 }, { u0, y1 } };
    emit_quad(m, v3((float)a.x, y0, (float)a.z), v3((float)b.x, y0, (float)b.z), v3((float)b.x, y1, (float)b.z),
              v3((float)a.x, y1, (float)a.z), v3((float)n.x, 0.0f, (float)n.z), uv, mt);
}

/* Paving laid along `grain` (unit): the sidewalks' material, its grain's
 * angle in the seed (the tangent frame for its normal map); a = 1 the
 * kerb stones' lighter concrete. */
static Mat paving(DVec2 grain, uint8_t a) {
    float ang = (float)atan2(grain.z, grain.x);
    uint32_t bits;
    memcpy(&bits, &ang, 4);
    return mat(MAT_SIDEWALK, bits, a, 0, 0);
}

/* A convex polygon (chunk-local) at height y, facing up (or down), clipped to
 * the chunk; uv the world's metres along and across `grain` (as the blocks'
 * paving), or 0 without one. */
static void flat(Mesh *m, const DVec2 *p, int n, float y, int down, Mat mt, const DVec2 *grain, const ChunkGen *g) {
    DVec2 cl[48];
    int k = clip_square(p, n, cl);
    if (!k) return;
    float uv[48][2];
    for (int i = 0; i < k; ++i) {
        double X = cl[i].x + (g->cx & 63) * (double)CHUNK_SIZE, Z = cl[i].z + (g->cz & 63) * (double)CHUNK_SIZE;
        uv[i][0] = grain ? (float)(grain->x * X + grain->z * Z) : 0.0f;
        uv[i][1] = grain ? (float)(-grain->z * X + grain->x * Z) : 0.0f;
    }
    if (!down) { emit_flat(m, cl, k, y, (const float (*)[2])uv, mt); return; }
    for (int i = 1; i + 1 < k; ++i) {
        float tuv[3][2] = { { uv[0][0], uv[0][1] }, { uv[i][0], uv[i][1] }, { uv[i + 1][0], uv[i + 1][1] } };
        emit_tri(m, v3((float)cl[0].x, y, (float)cl[0].z), v3((float)cl[i].x, y, (float)cl[i].z),
                 v3((float)cl[i + 1].x, y, (float)cl[i + 1].z), v3(0.0f, -1.0f, 0.0f), tuv, mt);
    }
}

/* A parapet: a wall PARAPET_W thick standing out from line a b (chunk-local)
 * by `out`, from y0 to its top. */
static void parapet(Mesh *m, const Parapet *p, const ChunkGen *g, Mat mt) {
    DVec2 a = local(g, p->a), b = local(g, p->b);
    DVec2 o = dscale(p->out, CHANNEL_PARAPET_W), f = dscale(p->out, 0.01);   /* its face a centimetre behind the wall's */
    DVec2 a0 = dadd(a, f), b0 = dadd(b, f), a1 = dadd(a, o), b1 = dadd(b, o);
    float y0 = p->base + BASE_Y, y1 = SIDEWALK_HEIGHT + CHANNEL_PARAPET_H;
    face(m, a0, b0, y0, y1, dv(-p->out.x, -p->out.z), mt);
    face(m, a1, b1, y0, y1, p->out, mt);
    DVec2 eh = dnorm(dsub(b, a));
    face(m, a0, a1, y0, y1, dv(-eh.x, -eh.z), mt);
    face(m, b0, b1, y0, y1, eh, mt);
    DVec2 top[4] = { a0, b0, b1, a1 };
    flat(m, top, 4, y1, 0, paving(eh, 1), &eh, g);
}

void build_channels(Mesh *m, const ChunkGen *g) {
    const PlanArea *pa = g->pa;
    if (!pa->nchannels) return;
    ChannelWall *w = (ChannelWall *)malloc(sizeof(ChannelWall) * MESH_WALLS);
    Parapet *pp = (Parapet *)malloc(sizeof(Parapet) * MESH_PARAPETS);
    DeckPiece *dk = (DeckPiece *)malloc(sizeof(DeckPiece) * MESH_DECKS);
    if (!w || !pp || !dk) fatal("out of memory building the rivers' channels");
    const Mat concrete = mat(MAT_CURB, 0, 0, 0, 0), soffit = mat(MAT_LOT, 0, 0, 0, 0);
    const float floor_y = (float)-PLAN_CHANNEL_DEPTH;
    /* the floors: across the towns whole, out in the belt over the city's ground */
    for (int k = 0; k < pa->nchannels; ++k) {
        const PlanChannel *c = &pa->channels[k];
        DVec2 v[PLAN_CHANNEL_PV], along = dnorm(dsub(c->b, c->a));
        Mat bed = paving(along, 1);   /* concrete slabs laid along the channel */
        for (int i = 0; i < c->n; ++i) v[i] = local(g, c->v[i]);
        if (c->built || !pa->bounded) { flat(m, v, c->n, floor_y, 0, bed, &along, g); continue; }
        for (int t = 0; t < pa->ntowns; ++t) {
            DVec2 cur[48], nxt[48];
            int n = c->n;
            memcpy(cur, v, sizeof(DVec2) * (size_t)n);
            const Poly *gp = &pa->ground[t];
            double o = orient(gp->v, gp->n);
            for (int i = 0; i < gp->n && n; ++i) {
                DVec2 a = local(g, gp->v[i]), e = dsub(local(g, gp->v[(i + 1) % gp->n]), a);
                DVec2 outn = dv(o * e.z, -o * e.x);
                n = clip_hp(cur, NULL, n, outn, ddot(a, outn), 0, nxt, NULL);
                memcpy(cur, nxt, sizeof(DVec2) * (size_t)n);
            }
            if (n >= 3) flat(m, cur, n, floor_y, 0, bed, &along, g);
        }
    }
    int nw = city_channel_walls(pa, w, MESH_WALLS);
    for (int i = 0; i < nw; ++i) face(m, local(g, w[i].a), local(g, w[i].b), floor_y, BASE_Y, w[i].in, concrete);
    int np = city_channel_parapets(pa, w, nw, pp, MESH_PARAPETS);
    for (int i = 0; i < np; ++i) parapet(m, &pp[i], g, concrete);
    /* the decks: the carriageway's asphalt, the sidewalks, their kerbs, the fascias and the soffit */
    int nd = city_bridge_decks(pa, dk, MESH_DECKS);
    for (int i = 0; i < nd; ++i) {
        const PlanBridge *b = &pa->bridges[dk[i].bridge];
        DVec2 v[DECK_PV], n = dv(-b->d.z, b->d.x), p = local(g, b->p);
        for (int q = 0; q < dk[i].n; ++q) v[q] = local(g, dk[i].v[q]);
        double c0 = ddot(p, n);
        DVec2 part[48], tmp[48];
        /* the carriageway: |t| <= half */
        int k = clip_hp(v, NULL, dk[i].n, n, c0 + b->half, 0, tmp, NULL);
        if (k) k = clip_hp(tmp, NULL, k, dv(-n.x, -n.z), -(c0 - b->half), 0, part, NULL);
        if (k) flat(m, part, k, BASE_Y, 0, mat(MAT_ROAD, 0, 0, 0, 0), NULL, g);
        for (int side = -1; side <= 1; side += 2) {
            /* a sidewalk: t beyond half on this side */
            DVec2 sn = dscale(n, -side);
            k = clip_hp(v, NULL, dk[i].n, sn, ddot(dadd(p, dscale(n, side * b->half)), sn), 0, part, NULL);
            if (k) flat(m, part, k, SIDEWALK_HEIGHT, 0, paving(b->d, 0), &b->d, g);
            /* its kerb, along t = side half, where the deck reaches */
            DVec2 la = dadd(dadd(p, dscale(b->d, b->s0)), dscale(n, side * b->half)), lb = dadd(la, dscale(b->d, b->s1 - b->s0));
            double t0, t1;
            if (city_seg_in(la, lb, v, dk[i].n, 0.0, &t0, &t1)) {
                DVec2 e = dsub(lb, la);
                face(m, dadd(la, dscale(e, t0)), dadd(la, dscale(e, t1)), BASE_Y, SIDEWALK_HEIGHT, dscale(n, -side), concrete);
            }
            /* its fascia, along the deck's edge */
            DVec2 fa = dadd(dadd(p, dscale(b->d, b->s0)), dscale(n, side * (b->half + b->walk))), fb = dadd(fa, dscale(b->d, b->s1 - b->s0));
            if (city_seg_in(fa, fb, v, dk[i].n, 1e-6, &t0, &t1)) {
                DVec2 e = dsub(fb, fa);
                face(m, dadd(fa, dscale(e, t0)), dadd(fa, dscale(e, t1)), SIDEWALK_HEIGHT - CHANNEL_DECK_DEPTH, SIDEWALK_HEIGHT, dscale(n, side), concrete);
            }
        }
        flat(m, v, dk[i].n, SIDEWALK_HEIGHT - CHANNEL_DECK_DEPTH, 1, soffit, NULL, g);
    }
    free(w); free(pp); free(dk);
}
