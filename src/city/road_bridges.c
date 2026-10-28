#include "city_internal.h"

/* ===========================================================================
 * The bridges of the roads between the settlements (plan.h, PlanRoadBridge),
 * where they cross the rivers and lakes out in the land: their decks as the
 * chunks they cross draw them (build_road_bridges) and the walker meets them
 * (city_layout).
 *
 * A deck runs the length of its span and half a metre on over the
 * embankment at each end, at the road's profile (world Y, a couple of
 * centimetres proud of the ground it meets there), out to the shoulders;
 * along each side a concrete parapet, its outer face the deck's fascia down
 * to the soffit, PLAN_ROAD_DECK_DEPTH under the surface. It is cut into
 * pieces at the road's points, at its profile's stations (so each piece is
 * straight and its surface flat) and at most ROAD_DECK_STEP long; each chunk
 * draws the pieces whose middle lies in it. Wall piers stand under a span
 * longer than ROAD_PIER_SPAN, evenly, down into the ground. The surface is the
 * city's asphalt, marked as the roads out in the land are (shaders/city/
 * ground.glsl, ROAD_DECK_MARK).
 * ========================================================================= */

#define DECK_LIFT   0.02f   /* m: the deck's surface over the profile, clear of the ground it meets */
#define DECK_OVER   0.5     /* m: the deck runs on this far past its span at each end */
#define PIER_HALF   0.8f    /* half a pier's thickness along the road (m) */
#define PIER_INSET  1.5f    /* a pier stands this far in from the deck's edges (m) */
#define MAX_SPANS   64
#define DECK_PHASE  60.0    /* m: the roads' dashes' period (12 m) and the freeways' lamps' (shaders/city/ground.glsl) both divide it */

/* Road r's point and its edges' direction s along it: the normal (unit, to
 * the left) of the segment there, mitred at a point of the road (*widen the
 * edges' stretch along it there, 1 elsewhere). */
static DVec2 road_point(const PlanRoad *R, double s, DVec2 *nrm, double *widen) {
    int lo = 0, hi = R->npts - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (R->s[mid] <= s) lo = mid; else hi = mid;
    }
    DVec2 d = dnorm(dsub(R->pts[hi], R->pts[lo]));
    *nrm = dv(-d.z, d.x);
    *widen = 1.0;
    double sl = R->s[hi] - R->s[lo], t = sl > 0.0 ? (s - R->s[lo]) / sl : 0.0;
    t = fmin(fmax(t, 0.0), 1.0);
    int at = t <= 1e-9 ? lo : t >= 1.0 - 1e-9 ? hi : -1;
    if (at > 0 && at < R->npts - 1) {
        DVec2 d0 = dnorm(dsub(R->pts[at], R->pts[at - 1])), d1 = dnorm(dsub(R->pts[at + 1], R->pts[at]));
        DVec2 n0 = dv(-d0.z, d0.x), n1 = dv(-d1.z, d1.x), m = dadd(n0, n1);
        if (dlen(m) > 1e-6) {
            m = dnorm(m);
            *nrm = m;
            *widen = 1.0 / fmax(ddot(m, n1), 0.5);
        }
    }
    return dadd(R->pts[lo], dscale(dsub(R->pts[hi], R->pts[lo]), t));
}

static int cmp_double(const void *pa, const void *pb) {
    double a = *(const double *)pa, b = *(const double *)pb;
    return a < b ? -1 : a > b ? 1 : 0;
}

/* Bridge b of road r cut into its pieces, each passed to fn. */
static void deck_pieces(int r, int b, void (*fn)(void *ctx, const RoadDeck *d), void *ctx) {
    const PlanRoad *R = plan_road(r);
    const PlanRoadBridge *B = &R->bridges[b];
    double len = R->s[R->npts - 1], S0 = fmax(B->s0 - DECK_OVER, 0.0), S1 = fmin(B->s1 + DECK_OVER, len);
    int cap = 64, n = 0;
    double *cut = (double *)malloc(sizeof(double) * (size_t)cap);
    if (!cut) fatal("out of memory building a bridge");
#define CUT(v) do { if (n == cap) { cap *= 2; cut = (double *)realloc(cut, sizeof(double) * (size_t)cap); if (!cut) fatal("out of memory building a bridge"); } cut[n++] = (v); } while (0)
    CUT(S0);
    CUT(S1);
    for (int k = 0; k < R->npts; ++k)
        if (R->s[k] > S0 && R->s[k] < S1) CUT(R->s[k]);
    for (double s = ceil(S0 / R->ds) * R->ds; s < S1; s += R->ds)
        if (s > S0) CUT(s);
    qsort(cut, (size_t)n, sizeof(double), cmp_double);
    RoadDeck d;
    memset(&d, 0, sizeof d);
    d.road = r;
    d.bridge = b;
    d.cls = R->cls;
    d.half = R->half;
    d.edge = R->half + (float)PLAN_ROAD_SHOULDER;
    for (int k = 0; k + 1 < n; ++k) {
        if (cut[k + 1] - cut[k] < 1e-3) continue;
        int pieces = (int)ceil((cut[k + 1] - cut[k]) / ROAD_DECK_STEP);
        for (int q = 0; q < pieces; ++q) {
            d.sa = cut[k] + (cut[k + 1] - cut[k]) * q / pieces;
            d.sb = cut[k] + (cut[k + 1] - cut[k]) * (q + 1) / pieces;
            /* the ends' normals: the mitre at a point of the road, else the piece's own */
            double wa, wb;
            d.a = road_point(R, d.sa, &d.na, &wa);
            d.b = road_point(R, d.sb, &d.nb, &wb);
            DVec2 dir = dnorm(dsub(d.b, d.a)), own = dv(-dir.z, dir.x);
            if (q > 0) { d.na = own; wa = 1.0; }
            if (q + 1 < pieces) { d.nb = own; wb = 1.0; }
            d.wa = (float)wa; d.wb = (float)wb;
            d.ya = (float)plan_road_height(r, d.sa) + DECK_LIFT;
            d.yb = (float)plan_road_height(r, d.sb) + DECK_LIFT;
            d.first = d.sa <= S0 + 1e-9;
            d.last = d.sb >= S1 - 1e-9;
            fn(ctx, &d);
        }
    }
#undef CUT
    free(cut);
}

typedef struct DeckGather { double x0, z0, x1, z1; RoadDeck *out; int n, max; } DeckGather;

static void gather_deck(void *ctx, const RoadDeck *d) {
    DeckGather *g = (DeckGather *)ctx;
    double m = d->edge * fmax(d->wa, d->wb) + ROAD_PARAPET_W;
    if (fmax(d->a.x, d->b.x) + m < g->x0 || fmin(d->a.x, d->b.x) - m > g->x1 ||
        fmax(d->a.z, d->b.z) + m < g->z0 || fmin(d->a.z, d->b.z) - m > g->z1) return;
    if (g->n < g->max) g->out[g->n++] = *d;
}

int city_road_decks(double x0, double z0, double x1, double z1, RoadDeck *out, int max) {
    PlanRoadSpan sp[MAX_SPANS];
    int ns = plan_road_bridges_in(x0, z0, x1, z1, ROAD_DECK_STEP, sp, MAX_SPANS);
    DeckGather g = { x0, z0, x1, z1, out, 0, max };
    for (int k = 0; k < ns; ++k) deck_pieces(sp[k].road, sp[k].bridge, gather_deck, &g);
    return g.n;
}

int city_road_deck_range(double x0, double z0, double x1, double z1, float *lo, float *hi) {
    RoadDeck d[64];
    int n = city_road_decks(x0, z0, x1, z1, d, 64);
    for (int k = 0; k < n; ++k) {
        /* the piers reach down to the ground under the deck, the water's (its least at the piece's ends and middle) */
        float a = fminf(d[k].ya, d[k].yb) - (float)PLAN_ROAD_DECK_DEPTH, b = fmaxf(d[k].ya, d[k].yb) + ROAD_PARAPET_H;
        for (int q = 0; q <= 2; ++q) {
            DVec2 p = dadd(d[k].a, dscale(dsub(d[k].b, d[k].a), 0.5 * q));
            a = fminf(a, (float)terrain_height(p.x, p.z) - 5.0f);
        }
        if (k == 0) { *lo = a; *hi = b; } else { *lo = fminf(*lo, a); *hi = fmaxf(*hi, b); }
    }
    return n > 0;
}

/* --- The mesh ------------------------------------------------------------------------ */

static vec3 at3(DVec2 p, float y, const ChunkGen *g) { return v3((float)(p.x - g->ox), y, (float)(p.z - g->oz)); }

/* A vertical face from p0 to p1 (world), its foot y0a / y0b and top y1a / y1b
 * at each end, facing n (horizontal, unit); uv metres along and up, as the
 * kerbs' faces. */
static void wall(Mesh *m, const ChunkGen *g, DVec2 p0, DVec2 p1, float y0a, float y0b, float y1a, float y1b, DVec2 n, Mat mt) {
    DVec2 e = dsub(p1, p0);
    float l = (float)dlen(e);
    if (l < 1e-4f) return;
    float sgn = (float)(e.x * n.z - e.z * n.x) < 0.0f ? -1.0f : 1.0f;
    float u0 = (float)(ddot(p0, dscale(e, 1.0 / l)) - floor(ddot(p0, dscale(e, 1.0 / l)) / 64.0) * 64.0), u1 = u0 + l;
    float uv[4][2] = { { sgn * u0, y0a }, { sgn * u1, y0b }, { sgn * u1, y1b }, { sgn * u0, y1a } };
    emit_quad(m, at3(p0, y0a, g), at3(p1, y0b, g), at3(p1, y1b, g), at3(p0, y1a, g), v3((float)n.x, 0.0f, (float)n.z), uv, mt);
}

/* Paving along `grain`: the kerb stones' lighter concrete (the channels' parapets' tops). */
static Mat stone(DVec2 grain) {
    float ang = (float)atan2(grain.z, grain.x);
    uint32_t bits;
    memcpy(&bits, &ang, 4);
    return mat(MAT_SIDEWALK, bits, 1, 0, 0);
}

static void emit_piece(Mesh *m, const ChunkGen *g, const RoadDeck *d) {
    const Mat concrete = mat(MAT_CURB, 0, 0, 0, 0), soffit = mat(MAT_LOT, 0, 0, 0, 0);
    const Mat asphalt = mat(MAT_ROAD, 0, (uint8_t)(d->half * 4.0f + 0.5f), 0, (uint8_t)(ROAD_DECK_MARK + d->cls));
    DVec2 dir = dnorm(dsub(d->b, d->a));
    float len = (float)dlen(dsub(d->b, d->a)), slope = len > 0.0f ? (d->yb - d->ya) / len : 0.0f;
    const float D = (float)PLAN_ROAD_DECK_DEPTH, H = ROAD_PARAPET_H, W = ROAD_PARAPET_W;
    double ea = d->edge * d->wa, eb = d->edge * d->wb, oa = (d->edge + W) * d->wa, ob = (d->edge + W) * d->wb;
    DVec2 La = dadd(d->a, dscale(d->na, ea)), Ra = dsub(d->a, dscale(d->na, ea));
    DVec2 Lb = dadd(d->b, dscale(d->nb, eb)), Rb = dsub(d->b, dscale(d->nb, eb));
    DVec2 OLa = dadd(d->a, dscale(d->na, oa)), ORa = dsub(d->a, dscale(d->na, oa));
    DVec2 OLb = dadd(d->b, dscale(d->nb, ob)), ORb = dsub(d->b, dscale(d->nb, ob));
    /* the surface: uv the offset from the centre line (left +) and the distance along the road */
    vec3 up = v3_norm(v3((float)(-slope * dir.x), 1.0f, (float)(-slope * dir.z)));
    double base = floor(d->sa / DECK_PHASE) * DECK_PHASE;   /* small in a float, the dashes' and lamps' phase kept */
    float su = (float)(d->sa - base), sv = (float)(d->sb - base);
    float uv[4][2] = { { d->edge, su }, { d->edge, sv }, { -d->edge, sv }, { -d->edge, su } };
    emit_quad(m, at3(La, d->ya, g), at3(Lb, d->yb, g), at3(Rb, d->yb, g), at3(Ra, d->ya, g), up, uv, asphalt);
    /* the parapets: inner face, top, outer face down to the soffit (the fascia) */
    for (int side = -1; side <= 1; side += 2) {
        DVec2 ia = side > 0 ? La : Ra, ib = side > 0 ? Lb : Rb, xa = side > 0 ? OLa : ORa, xb = side > 0 ? OLb : ORb;
        DVec2 n = dscale(dv(-dir.z, dir.x), side);
        wall(m, g, ia, ib, d->ya, d->yb, d->ya + H, d->yb + H, dscale(n, -1.0), concrete);
        wall(m, g, xa, xb, d->ya - D, d->yb - D, d->ya + H, d->yb + H, n, concrete);
        float tuv[4][2] = { { su, 0.0f }, { sv, 0.0f }, { sv, W }, { su, W } };
        emit_quad(m, at3(ia, d->ya + H, g), at3(ib, d->yb + H, g), at3(xb, d->yb + H, g), at3(xa, d->ya + H, g), up, tuv, stone(dir));
        /* the parapet's ends at the deck's */
        if (d->first) wall(m, g, ia, xa, d->ya, d->ya, d->ya + H, d->ya + H, dscale(dir, -1.0), concrete);
        if (d->last) wall(m, g, ib, xb, d->yb, d->yb, d->yb + H, d->yb + H, dir, concrete);
    }
    /* the soffit, and the deck's ends */
    float zuv[4][2] = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } };
    vec3 down = v3(-up.x, -up.y, -up.z);
    emit_quad(m, at3(OLa, d->ya - D, g), at3(OLb, d->yb - D, g), at3(ORb, d->yb - D, g), at3(ORa, d->ya - D, g), down, zuv, soffit);
    if (d->first) wall(m, g, ORa, OLa, d->ya - D, d->ya - D, d->ya, d->ya, dscale(dir, -1.0), concrete);
    if (d->last) wall(m, g, ORb, OLb, d->yb - D, d->yb - D, d->yb, d->yb, dir, concrete);
}

/* A wall pier under the deck s along road r: from the ground (the lowest
 * across it, a little into it) up to the soffit. */
static void emit_pier(Mesh *m, const ChunkGen *g, int r, double s) {
    const PlanRoad *R = plan_road(r);
    DVec2 nrm;
    double widen;
    DVec2 c = road_point(R, s, &nrm, &widen), dir = dv(nrm.z, -nrm.x);
    float half = R->half + (float)PLAN_ROAD_SHOULDER - PIER_INSET;
    float top = (float)plan_road_height(r, s) + DECK_LIFT - (float)PLAN_ROAD_DECK_DEPTH, foot = 1e30f;
    for (int k = -1; k <= 1; ++k) {
        DVec2 q = dadd(c, dscale(nrm, k * half));
        foot = fminf(foot, (float)terrain_height(q.x, q.z));
    }
    foot -= 0.5f;
    if (top - foot < 0.5f) return;
    DVec2 p[4] = { dadd(dadd(c, dscale(nrm, half)), dscale(dir, -PIER_HALF)), dadd(dadd(c, dscale(nrm, half)), dscale(dir, PIER_HALF)),
                   dadd(dsub(c, dscale(nrm, half)), dscale(dir, PIER_HALF)), dadd(dsub(c, dscale(nrm, half)), dscale(dir, -PIER_HALF)) };
    const Mat concrete = mat(MAT_CURB, 0, 0, 0, 0);
    for (int k = 0; k < 4; ++k) {
        DVec2 a = p[k], b = p[(k + 1) & 3], e = dnorm(dsub(b, a));
        wall(m, g, a, b, foot, foot, top, top, dv(e.z, -e.x), concrete);
    }
}

typedef struct DeckEmit { Mesh *m; const ChunkGen *g; } DeckEmit;

static void emit_owned(void *ctx, const RoadDeck *d) {
    DeckEmit *e = (DeckEmit *)ctx;
    DVec2 mid = dscale(dadd(d->a, d->b), 0.5);
    if (chunk_gen_owns(e->g, mid.x, mid.z)) emit_piece(e->m, e->g, d);
}

void build_road_bridges(Mesh *m, const ChunkGen *g) {
    const double P = CHUNK_SIZE;
    PlanRoadSpan sp[MAX_SPANS];
    int ns = plan_road_bridges_in(g->ox, g->oz, g->ox + P, g->oz + P, ROAD_DECK_STEP, sp, MAX_SPANS);
    if (!ns) return;
    int grounded = m->grounded;
    float ground = m->ground;
    m->grounded = 1;   /* every vertex counts from the deck, not the city's ground under it */
    DeckEmit e = { m, g };
    for (int k = 0; k < ns; ++k) {
        const PlanRoad *R = plan_road(sp[k].road);
        const PlanRoadBridge *B = &R->bridges[sp[k].bridge];
        m->ground = B->deck;
        deck_pieces(sp[k].road, sp[k].bridge, emit_owned, &e);
        /* the piers, evenly under a long span */
        int spans = (int)ceil((B->s1 - B->s0) / ROAD_PIER_SPAN);
        for (int q = 1; q < spans; ++q) {
            double s = B->s0 + (B->s1 - B->s0) * q / spans;
            DVec2 nrm;
            double widen;
            DVec2 c = road_point(R, s, &nrm, &widen);
            if (chunk_gen_owns(g, c.x, c.z)) emit_pier(m, g, sp[k].road, s);
        }
    }
    m->grounded = grounded;
    m->ground = ground;
}
