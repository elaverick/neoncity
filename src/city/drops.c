#include "city_internal.h"

/* ===========================================================================
 * Drop kerbs: at each side of every crosswalk the kerb is lowered for the
 * width of the crossing and the paving behind it slopes back up to the
 * sidewalk between kerb-stone sides, with tactile paving at the foot. Each is
 * a rectangle along its kerb, cut back where it would run into another kerb.
 * Where two come together at a corner they lower the whole corner between
 * them, each taking the side nearer its own kerb, so their slopes meet in a
 * valley with no wall between. Pure function of the plan, like the lamps.
 * ========================================================================= */

#define DROP_SPAN0    0.4                        /* along the carriageway from its trimmed end: a */
#define DROP_SPAN1    3.8                        /* little wider than the crosswalk (0.5-3.7 m, roadPaint) */
#define DROP_CLEAR    (2.0 * KERB_STONE_WIDTH)   /* kept off any other kerb: its stones, then the side's own */
#define DROP_MIN_W    1.5                        /* the narrowest lowered kerb worth cutting */
#define DROP_SLAB_GAP 0.08                       /* tactile slabs stay this far off a kerb-stone side */

/* Keeps the part of convex polygon `in` where p.nr <= c. With labels, the edge
 * cut along the line takes `lab`; out must not alias in. */
int clip_hp(const DVec2 *in, const uint8_t *li, int n, DVec2 nr, double c, uint8_t lab, DVec2 *out, uint8_t *lo) {
    int k = 0;
    for (int i = 0; i < n && k < 46; ++i) {
        DVec2 a = in[i], b = in[(i + 1) % n];
        double da = ddot(a, nr) - c, db = ddot(b, nr) - c;
        uint8_t la = li ? li[i] : 0;
        if (da <= 0.0) { if (lo) lo[k] = la; out[k++] = a; }
        if ((da <= 0.0) != (db <= 0.0)) {
            double t = da / (da - db);
            if (lo) lo[k] = da <= 0.0 ? lab : la;
            out[k++] = dv(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t);
        }
    }
    return k >= 3 ? k : 0;
}

double area2(const DVec2 *p, int n) {
    double s = 0.0;
    for (int i = 0; i < n; ++i) s += p[i].x * p[(i + 1) % n].z - p[(i + 1) % n].x * p[i].z;
    return s;
}

typedef struct DropCand { int edge, alive; double u0, u1; } DropCand;
typedef struct HPlane { DVec2 nr; double c; uint8_t lab; } HPlane;   /* keep p.nr <= c */

/* The block's curb edges: inward normals, the kerb-stone frame's along
 * direction (as build_blocks) and the range of u along each. */
typedef struct CurbFrame { int n; DVec2 nin[PLAN_MAX_PV], al[PLAN_MAX_PV]; double u0[PLAN_MAX_PV], u1[PLAN_MAX_PV]; } CurbFrame;

/* A rectangle along curb edge e, from u0 to u1, as corners and as its four half-planes. */
static void drop_rect(const Poly *c, const CurbFrame *F, int e, double u0, double u1, DVec2 *q, uint8_t *lb, HPlane *hp) {
    DVec2 o = c->v[e], t = F->al[e], nn = F->nin[e];
    q[0] = dadd(o, dscale(t, u0));
    q[1] = dadd(o, dscale(t, u1));
    q[2] = dadd(q[1], dscale(nn, DROP_DEPTH));
    q[3] = dadd(q[0], dscale(nn, DROP_DEPTH));
    lb[0] = DE_KERB; lb[1] = DE_SIDE; lb[2] = DE_BACK; lb[3] = DE_SIDE;
    double on = ddot(o, nn), ot = ddot(o, t);
    hp[0].nr = dv(-nn.x, -nn.z); hp[0].c = -on;              hp[0].lab = DE_KERB;
    hp[1].nr = t;                 hp[1].c = ot + u1;          hp[1].lab = DE_SIDE;
    hp[2].nr = nn;                hp[2].c = on + DROP_DEPTH;  hp[2].lab = DE_BACK;
    hp[3].nr = dv(-t.x, -t.z);    hp[3].c = -(ot + u0);       hp[3].lab = DE_SIDE;
}

/* Whether a's rectangle comes within DROP_CLEAR of b's. */
static int drop_near(const Poly *c, const CurbFrame *F, const DropCand *a, const DropCand *b) {
    DVec2 qa[4], qb[4], p[48], r[48];
    uint8_t lb[4];
    HPlane hb[4], ha[4];
    drop_rect(c, F, a->edge, a->u0, a->u1, qa, lb, ha);
    drop_rect(c, F, b->edge, b->u0, b->u1, qb, lb, hb);
    int n = 4;
    memcpy(p, qa, sizeof(qa));
    for (int k = 0; k < 4 && n; ++k) {
        n = clip_hp(p, NULL, n, hb[k].nr, hb[k].c + DROP_CLEAR, 0, r, NULL);
        memcpy(p, r, sizeof(DVec2) * n);
    }
    return n && fabs(area2(p, n)) > 2e-3;
}

/* Every half-plane bounding candidate ai, its rectangle's four first. For each
 * other kerb: where a drop on it comes near, the two share the corner, each
 * taking the side nearer its own kerb, and when the corner is the one between
 * their kerbs, each runs right round to it so the whole corner is lowered;
 * otherwise it keeps DROP_CLEAR off that kerb. */
static int drop_planes(const Poly *c, const CurbFrame *F, const DropCand *cd, int nc, int ai, HPlane *hp, double *u0, double *u1) {
    const DropCand *a = &cd[ai];
    int n = c->n, k = 4, ea = a->edge;
    DVec2 na = F->nin[ea];
    *u0 = a->u0; *u1 = a->u1;
    for (int e = 0; e < n; ++e) {
        if (e == ea || F->u1[e] - F->u0[e] < 1e-6) continue;
        DVec2 ne = F->nin[e];
        if (ddot(na, ne) > 0.996) continue;   /* the same kerb line carrying on */
        int share = 0;
        for (int b = 0; b < nc && !share; ++b)
            share = b != ai && cd[b].alive && cd[b].edge == e && (drop_near(c, F, a, &cd[b]) || drop_near(c, F, &cd[b], a));
        if (share) {
            hp[k].nr = dsub(na, ne);
            hp[k].c = ddot(c->v[ea], na) - ddot(c->v[e], ne);
            hp[k++].lab = DE_OPEN;
            int next = e == (ea + 1) % n, prev = e == (ea + n - 1) % n;
            if (next || prev) {
                double cu = next ? ddot(dsub(c->v[(ea + 1) % n], c->v[ea]), F->al[ea]) : 0.0;
                if (cu <= 0.5 * (a->u0 + a->u1)) *u0 = fmin(*u0, cu - DROP_DEPTH - 1.0);
                else *u1 = fmax(*u1, cu + DROP_DEPTH + 1.0);
            }
        } else {
            hp[k].nr = dv(-ne.x, -ne.z);
            hp[k].c = -ddot(c->v[e], ne) - DROP_CLEAR;
            hp[k++].lab = DE_SIDE;
        }
    }
    DVec2 q[4];
    uint8_t lb[4];
    drop_rect(c, F, ea, *u0, *u1, q, lb, hp);
    return k;
}

/* Candidate ai's outline (labelled), and its half-planes. */
static int drop_outline(const Poly *c, const CurbFrame *F, const DropCand *cd, int nc, int ai, HPlane *hp, int *nhp, DVec2 *p, uint8_t *lp) {
    double u0, u1;
    int k = drop_planes(c, F, cd, nc, ai, hp, &u0, &u1);
    HPlane rh[4];
    DVec2 q[48];
    uint8_t lq[48];
    drop_rect(c, F, cd[ai].edge, u0, u1, p, lp, rh);
    int m = 4;
    for (int j = 4; j < k && m; ++j) {
        m = clip_hp(p, lp, m, hp[j].nr, hp[j].c, hp[j].lab, q, lq);
        memcpy(p, q, sizeof(DVec2) * m);
        memcpy(lp, lq, (size_t)m);
    }
    *nhp = k;
    return m;
}

int city_block_drops(const PlanArea *pa, const Block *bk, DropKerb *out, int max) {
    const Poly *c = &bk->curb;
    int n = c->n;
    if (n < 3) return 0;
    CurbFrame F;
    F.n = n;
    DVec2 cen = poly_centroid(c);
    for (int i = 0; i < n; ++i) {
        DVec2 d = dsub(c->v[(i + 1) % n], c->v[i]);
        F.nin[i] = dlen(d) > 1e-6 ? dnorm(dv(-d.z, d.x)) : dv(0, 0);
        if (ddot(F.nin[i], dsub(cen, c->v[i])) < 0) F.nin[i] = dv(-F.nin[i].x, -F.nin[i].z);
        F.al[i] = dv(F.nin[i].z, -F.nin[i].x);
        double ue = ddot(d, F.al[i]);
        F.u0[i] = fmin(0.0, ue); F.u1[i] = fmax(0.0, ue);
    }

    /* A crosswalk crosses each end of every carriageway long enough to have
     * them (build_roads), meeting the kerbs that run along its sides. */
    DropCand cd[MAX_BLOCK_DROPS];
    int nc = 0;
    for (int i = 0; i < n; ++i) {
        if (!c->e[i].id || F.u1[i] - F.u0[i] < DROP_MIN_W) continue;
        for (int ri = 0; ri < pa->nroads; ++ri) {
            const Road *r = &pa->roads[ri];
            if (r->at.id != c->e[i].id) continue;
            DVec2 d = dsub(r->b, r->a);
            double rl = dlen(d);
            if (rl < 1.0 || rl - r->trimA - r->trimB <= 16.0) continue;
            DVec2 dir = dscale(d, 1.0 / rl), w = dsub(c->v[i], r->a);
            if (fabs(ddot(dir, F.al[i])) < 0.999 || fabs(fabs(dir.x * w.z - dir.z * w.x) - r->at.half) > 0.05) continue;
            for (int end = 0; end < 2 && nc < MAX_BLOCK_DROPS; ++end) {
                double s0 = end ? rl - r->trimB - DROP_SPAN1 : r->trimA + DROP_SPAN0;
                double u0 = ddot(dsub(dadd(r->a, dscale(dir, s0)), c->v[i]), F.al[i]);
                double u1 = ddot(dsub(dadd(r->a, dscale(dir, s0 + DROP_SPAN1 - DROP_SPAN0)), c->v[i]), F.al[i]);
                if (u0 > u1) { double t = u0; u0 = u1; u1 = t; }
                if (u1 <= F.u0[i] || u0 >= F.u1[i]) continue;
                cd[nc].edge = i; cd[nc].alive = 1; cd[nc].u0 = u0; cd[nc].u1 = u1;
                ++nc;
            }
        }
    }

    /* Cut each back; drop any left too narrow, and again for the drops that
     * were sharing a corner with it. */
    HPlane hp[4 + PLAN_MAX_PV];
    for (int iter = 0; iter < 4; ++iter) {
        int changed = 0;
        for (int a = 0; a < nc; ++a) {
            if (!cd[a].alive) continue;
            DVec2 p[48];
            uint8_t lp[48];
            int k, m = drop_outline(c, &F, cd, nc, a, hp, &k, p, lp);
            double w = 0.0;
            for (int j = 0; j < m; ++j)
                if (lp[j] == DE_KERB) w += dlen(dsub(p[(j + 1) % m], p[j]));
            if (w < DROP_MIN_W) { cd[a].alive = 0; changed = 1; }
        }
        if (!changed) break;
    }

    int no = 0;
    for (int a = 0; a < nc && no < max; ++a) {
        if (!cd[a].alive) continue;
        DropKerb *D = &out[no];
        DVec2 p[48];
        uint8_t lp[48];
        int k, m = drop_outline(c, &F, cd, nc, a, hp, &k, p, lp);
        if (m < 3 || m > MAX_DROP_PV) continue;
        D->nopen = 0;
        for (int j = 4; j < k; ++j)
            if (hp[j].lab == DE_OPEN && D->nopen < 4) {
                double *s = D->open[D->nopen++];
                s[0] = hp[j].nr.x; s[1] = hp[j].nr.z; s[2] = hp[j].c;
            }
        int e = cd[a].edge;
        DVec2 o = c->v[e], t = F.al[e], nn = F.nin[e];
        D->edge = e;
        D->ox = o.x; D->oz = o.z; D->tx = t.x; D->tz = t.z; D->nx = nn.x; D->nz = nn.z;
        D->n = m;
        for (int j = 0; j < m; ++j) { D->v[j][0] = p[j].x; D->v[j][1] = p[j].z; D->lab[j] = lp[j]; }
        /* Tactile slabs: whole ones only, on a grid centred on the crossing,
         * clear of the kerb-stone sides. */
        D->tc = 0.5 * (cd[a].u0 + cd[a].u1);
        for (int r = 0; r < DROP_ROWS; ++r) {
            double d0 = KERB_STONE_WIDTH + r * DROP_TILE, d1 = d0 + DROP_TILE;
            int lo = 1000, hi = -1000;
            for (int s = -12; s < 12; ++s) {
                double ua = D->tc + s * DROP_TILE, ub = ua + DROP_TILE;
                int fits = 1;
                for (int corner = 0; corner < 4 && fits; ++corner) {
                    DVec2 pt = dadd(o, dadd(dscale(t, corner & 1 ? ub : ua), dscale(nn, corner & 2 ? d1 : d0)));
                    for (int j = 0; j < k && fits; ++j)
                        fits = ddot(pt, hp[j].nr) <= hp[j].c - (hp[j].lab == DE_SIDE ? DROP_SLAB_GAP : 0.0) + 1e-9;
                }
                if (fits) { if (s < lo) lo = s; if (s > hi) hi = s; }
            }
            D->tu[r][0] = hi >= lo ? D->tc + lo * DROP_TILE : 0.0;
            D->tu[r][1] = hi >= lo ? D->tc + (hi + 1) * DROP_TILE : 0.0;
        }
        ++no;
    }
    return no;
}

int city_drop_contains(const DropKerb *d, double x, double z, double margin) {
    double s = 0.0;
    for (int i = 0; i < d->n; ++i) {
        const double *a = d->v[i], *b = d->v[(i + 1) % d->n];
        s += a[0] * b[1] - b[0] * a[1];
    }
    s = s >= 0.0 ? 1.0 : -1.0;
    for (int i = 0; i < d->n; ++i) {
        const double *a = d->v[i], *b = d->v[(i + 1) % d->n];
        double ex = b[0] - a[0], ez = b[1] - a[1], l = sqrt(ex * ex + ez * ez);
        if (l < 1e-9) continue;
        if (s * (ex * (z - a[1]) - ez * (x - a[0])) / l < -margin) return 0;
    }
    return 1;
}

/* The lowered kerb stones at DROP_KERB_HEIGHT, then the slope up to the sidewalk. */
float drop_y(double d) {
    double f = (d - KERB_STONE_WIDTH) / DROP_RAMP_RUN;
    f = f < 0.0 ? 0.0 : f > 1.0 ? 1.0 : f;
    return (float)(DROP_KERB_HEIGHT + (SIDEWALK_HEIGHT - DROP_KERB_HEIGHT) * f);
}

float city_drop_height(const DropKerb *d, double x, double z) {
    return drop_y((x - d->ox) * d->nx + (z - d->oz) * d->nz);
}
