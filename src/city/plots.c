#include "city_internal.h"
#include "terrain.h"

/* ===========================================================================
 * The houses' plots (residential roadmap HM1, ~/.claude/plans/residential-buildings.md;
 * the target in docs/residential-target.md, "Plots")
 *
 * A suburb block (ZONE_SUBURB) is cut into plots along its streets, each
 * plot running in from its street: boundary, front garden, house, rear
 * garden (section 13.5's order). Each street side of the block is a strip as deep
 * as a plot, laid with one archetype (a street's houses are built together):
 * terraces in rows of 4.2-6 m units at most 48 m long with a passage between
 * rows, semi-detached pairs of 7-10 m halves, or detached houses on 10-18 m
 * frontages. The archetype comes from the city's region (plan_settlement_region).
 * The local streets are laid first, then the busier ones, the longest first;
 * a later strip keeps clear of the earlier ones, so a corner plot fronts one
 * street and the side street's first plot starts behind its rear garden. A
 * block deeper than two plots keeps its middle as backland (gardens, HM7).
 * ========================================================================= */

#define STRIP_MIN_DEPTH 20.0    /* front garden 3 + house 8 + rear garden 8, and a little over */
#define STRIP_MIN_LEN   10.0
#define PROBE           0.5     /* m: the step the free stretches of a strip are found at */
#define MAX_STRIPS      PLAN_MAX_PV

typedef struct Strip {
    DVec2 o, t, n;          /* origin on the lot's edge, along it, into the block */
    double u0, u1, depth;   /* the part its plots take */
} Strip;

/* The u-extent of the convex polygon along v = const in the frame (o, t, n). */
static int slice(const Poly *p, DVec2 o, DVec2 t, DVec2 n, double v, double *lo, double *hi) {
    *lo = 1e300; *hi = -1e300;
    for (int i = 0; i < p->n; ++i) {
        DVec2 a = dsub(p->v[i], o), b = dsub(p->v[(i + 1) % p->n], o);
        double ua = ddot(a, t), va = ddot(a, n), ub = ddot(b, t), vb = ddot(b, n);
        if ((va - v) * (vb - v) > 0.0) continue;
        if (fabs(vb - va) < 1e-9) { *lo = fmin(*lo, fmin(ua, ub)); *hi = fmax(*hi, fmax(ua, ub)); }
        else { double u = ua + (ub - ua) * (v - va) / (vb - va); *lo = fmin(*lo, u); *hi = fmax(*hi, u); }
    }
    return *lo <= *hi;
}

/* An oriented rectangle: centre, unit axes, half extents. */
typedef struct ORect { DVec2 c, a, b; double ha, hb; } ORect;

static ORect strip_rect(DVec2 o, DVec2 t, DVec2 n, double u0, double u1, double v0, double v1) {
    ORect r;
    r.c = dadd(o, dadd(dscale(t, 0.5 * (u0 + u1)), dscale(n, 0.5 * (v0 + v1))));
    r.a = t; r.b = n; r.ha = 0.5 * (u1 - u0); r.hb = 0.5 * (v1 - v0);
    return r;
}

static double orect_reach(const ORect *r, DVec2 axis) { return r->ha * fabs(ddot(r->a, axis)) + r->hb * fabs(ddot(r->b, axis)); }

/* Separating axes: whether two oriented rectangles overlap. */
static int orect_overlap(const ORect *p, const ORect *q) {
    DVec2 d = dsub(q->c, p->c), ax[4] = { p->a, p->b, q->a, q->b };
    for (int k = 0; k < 4; ++k)
        if (fabs(ddot(d, ax[k])) > orect_reach(p, ax[k]) + orect_reach(q, ax[k])) return 0;
    return 1;
}

/* The archetypes' weights by region (detached, semi, terrace): docs/residential-target.md,
 * "Regions". North America has no semis, and rowhouses only in the north-east's old cities. */
static void region_weights(int region, double lat, double lon, float w[HOME_ARCH_COUNT]) {
    static const float W[PLAN_REGION_COUNT][HOME_ARCH_COUNT] = {
        { 0.90f, 0.00f, 0.10f },   /* na */
        { 0.31f, 0.31f, 0.38f },   /* uk: section 13.9's suburb weights over its houses */
        { 0.40f, 0.00f, 0.60f },   /* seu: terraced townhouses, detached villas */
        { 0.60f, 0.00f, 0.40f },   /* eas */
        { 0.25f, 0.05f, 0.70f },   /* sea: terraces most */
        { 0.35f, 0.00f, 0.65f },   /* sas */
        { 0.60f, 0.00f, 0.40f },   /* mena: walled detached, courtyard rows */
        { 0.90f, 0.00f, 0.10f },   /* ssa */
        { 0.15f, 0.00f, 0.85f },   /* lat: attached rows */
    };
    for (int k = 0; k < HOME_ARCH_COUNT; ++k) w[k] = W[region][k];
    if (region == PLAN_REGION_NA && lon > -82.0 && lat > 37.0) { w[0] = 0.45f; w[2] = 0.55f; }   /* the north-east's rowhouses */
}

/* Frontage of a plot (m), per unit of a terrace, a half of a pair or a detached house. */
static void frontage(int arch, int region, double *lo, double *hi) {
    switch (arch) {
    case HOME_TERRACE: *lo = 4.2; *hi = 6.0; break;
    case HOME_SEMI:    *lo = 7.0; *hi = 10.0; break;
    default:           *lo = region == PLAN_REGION_NA ? 13.0 : 10.0; *hi = 18.0; break;
    }
}

/* A plot's depth by archetype: terraces shallower, North America's lots deeper. */
static double plot_depth(int arch, int region, Rng *r) {
    double d = arch == HOME_TERRACE ? rng_range(r, 24.0f, 32.0f) : arch == HOME_SEMI ? rng_range(r, 28.0f, 38.0f) : rng_range(r, 30.0f, 42.0f);
    return region == PLAN_REGION_NA ? fmin(d + 4.0, 45.0) : d;
}

static int storeys(int arch, int region, uint32_t h) {
    float x = hash_float(h);
    if (arch == HOME_TERRACE) {
        switch (region) {
        case PLAN_REGION_NA: case PLAN_REGION_SEU: case PLAN_REGION_SEA: case PLAN_REGION_SAS: return x < 0.6f ? 3 : 2;
        case PLAN_REGION_LAT: return x < 0.4f ? 1 : 2;
        default: return x < 0.25f ? 3 : 2;
        }
    }
    if (arch == HOME_SEMI) return 2;
    switch (region) {
    case PLAN_REGION_NA:  return x < 0.45f ? 1 : 2;   /* ranches */
    case PLAN_REGION_SSA: case PLAN_REGION_LAT: return x < 0.5f ? 1 : 2;
    case PLAN_REGION_SAS: case PLAN_REGION_SEU: return x < 0.3f ? 3 : 2;
    default: return x < 0.1f ? 1 : 2;   /* the odd bungalow */
    }
}

/* The front garden: 3 m at least (section 13.5); open lawns in North America, tight in the old rows. */
static float front_garden(int arch, int region, uint32_t h) {
    float x = hash_float(h);
    if (region == PLAN_REGION_NA) return arch == HOME_TERRACE ? 3.0f + 1.5f * x : 6.0f + 3.0f * x;
    return arch == HOME_TERRACE ? 3.0f + 1.5f * x : 4.0f + 3.0f * x;
}

typedef struct PlotCtx {
    const Block *bk;
    HomePlot    *out;
    int          n, max, rows, region;
} PlotCtx;

static void add_plot(PlotCtx *P, const Strip *s, uint32_t road, double u0, double u1, int arch, int unit, int units, uint32_t rowSeed) {
    if (P->n >= P->max) return;
    HomePlot *p = &P->out[P->n];
    memset(p, 0, sizeof(*p));
    p->ox = s->o.x; p->oz = s->o.z;
    p->tx = (float)s->t.x; p->tz = (float)s->t.z;
    p->nx = (float)s->n.x; p->nz = (float)s->n.z;
    p->u0 = (float)u0; p->u1 = (float)u1;
    p->depth = (float)s->depth;
    p->seed = hash_combine(rowSeed, (uint32_t)unit + 1u);
    p->road = road;
    p->row = (uint16_t)P->rows;
    p->arch = (uint8_t)arch;
    p->unit = (uint8_t)unit; p->units = (uint8_t)units;
    p->region = (uint8_t)P->region;
    /* a row is built as one: its storeys and front garden are the row's; a street's houses
     * were mostly built together, so three rows in four take the street's storeys */
    uint32_t built = hash_float(hash_combine(rowSeed, 0x5702u)) < 0.75f ? hash_combine(road, (uint32_t)arch) : rowSeed;
    p->storeys = (uint8_t)storeys(arch, P->region, hash_combine(built, 0x5701u));
    /* and room behind it for the house (9 m) and the rear garden (8 m) */
    p->front = fmaxf(3.0f, fminf(front_garden(arch, P->region, hash_combine(rowSeed, 0xF407u)), p->depth - 17.0f));
    P->n++;
}

/* Lays one archetype's rows along [a, b] of the strip; returns where the last plot ends. */
static double lay_rows(PlotCtx *P, const Strip *s, uint32_t road, int arch, double a, double b, Rng *r) {
    double lo, hi, x = a, end = a;
    frontage(arch, P->region, &lo, &hi);
    while (P->n < P->max) {
        uint32_t rowSeed = hash_combine(P->bk->seed ^ 0x40FE0u, (uint32_t)P->rows * 2654435761u);
        if (arch == HOME_TERRACE) {
            double w = rng_range(r, (float)lo, (float)hi);
            int units = (int)floor(fmin(48.0, b - x) / w);
            if (units > 12) units = 12;
            if (units < 3) break;
            for (int k = 0; k < units; ++k) add_plot(P, s, road, x + k * w, x + (k + 1) * w, arch, k, units, rowSeed);
            end = x + units * w;
            x = end + rng_range(r, 1.2f, 3.0f);   /* a passage to the rear gardens */
        } else if (arch == HOME_SEMI) {
            double w = rng_range(r, (float)lo, (float)hi);
            if (b - x < 2.0 * w) break;
            add_plot(P, s, road, x, x + w, arch, 0, 2, rowSeed);
            add_plot(P, s, road, x + w, x + 2.0 * w, arch, 1, 2, rowSeed);
            end = x = x + 2.0 * w;
        } else {
            double w = rng_range(r, (float)lo, (float)hi);
            if (b - x < w) { if (b - x >= lo) w = b - x; else break; }
            add_plot(P, s, road, x, x + w, arch, 0, 1, rowSeed);
            end = x = x + w;
        }
        P->rows++;
    }
    return end;
}

int city_block_plots(const Block *bk, HomePlot *out, int max) {
    if (bk->zone != ZONE_SUBURB || bk->lot.n < 3) return 0;
    const Poly *lot = &bk->lot;
    const TerrainFrame *tf = terrain_frame();
    int k = tf && tf->settlement > 0 ? tf->settlement : 0;
    double lat, lon;
    plan_settlement_latlon(k, &lat, &lon);
    PlotCtx P;
    P.bk = bk; P.out = out; P.n = 0; P.max = max; P.rows = 0;
    P.region = plan_region_of(lat, lon);
    float w[HOME_ARCH_COUNT];
    region_weights(P.region, lat, lon, w);
    Rng r = rng_make(bk->seed ^ 0x9107u);

    /* the street edges: the local streets first, then by length */
    int order[MAX_STRIPS], ne = 0;
    double key[MAX_STRIPS];
    for (int i = 0; i < lot->n; ++i) {
        const RoadAttr *e = &lot->e[i];
        double len = dlen(dsub(lot->v[(i + 1) % lot->n], lot->v[i]));
        if (e->cls == ROAD_NONE || e->bank || len < STRIP_MIN_LEN) continue;
        key[ne] = len + (e->cls == ROAD_LOCAL ? 1e4 : 0.0);
        order[ne++] = i;
    }
    for (int i = 1; i < ne; ++i)
        for (int j = i; j > 0 && key[j] > key[j - 1]; --j) {
            double tk = key[j]; key[j] = key[j - 1]; key[j - 1] = tk;
            int to = order[j]; order[j] = order[j - 1]; order[j - 1] = to;
        }

    DVec2 cen = poly_centroid(lot);
    ORect used[MAX_STRIPS];
    int nused = 0;
    for (int q = 0; q < ne && P.n < max; ++q) {
        int i = order[q];
        Strip s;
        s.o = lot->v[i];
        s.t = dnorm(dsub(lot->v[(i + 1) % lot->n], s.o));
        s.n = dv(-s.t.z, s.t.x);
        if (ddot(dsub(cen, s.o), s.n) < 0.0) s.n = dv(-s.n.x, -s.n.z);
        /* the archetype of this side of the street: a hash of the block and the side */
        float pick = hash_float(hash_combine(bk->seed ^ 0xA2C4u, (uint32_t)i));
        int arch = 0;
        for (float acc = 0.0f; arch < HOME_ARCH_COUNT - 1; ++arch) { acc += w[arch]; if (pick < acc) break; }
        /* the block's depth square to this edge: back to back with the far side's plots, or alone */
        double vmax = 0.0;
        for (int v = 0; v < lot->n; ++v) vmax = fmax(vmax, ddot(dsub(lot->v[v], s.o), s.n));
        double want = plot_depth(arch, P.region, &r);
        s.depth = vmax < 2.0 * want ? 0.5 * vmax - 0.05 : want;
        if (s.depth < STRIP_MIN_DEPTH) s.depth = fmin(want, vmax - 0.5);
        if (s.depth < STRIP_MIN_DEPTH) continue;
        double l0, h0, l1, h1;
        if (!slice(lot, s.o, s.t, s.n, 0.02, &l0, &h0) || !slice(lot, s.o, s.t, s.n, s.depth, &l1, &h1)) continue;
        double ul = fmax(l0, l1) + 0.3, ur = fmin(h0, h1) - 0.3;
        if (ur - ul < STRIP_MIN_LEN) continue;
        /* the free stretches: clear of the earlier strips' plots */
        double first = 1e300, last = -1e300, a = 0.0;
        int run = 0;
        uint32_t road = lot->e[i].id;
        for (double u = ul; ; u += PROBE) {
            int done = u + PROBE > ur, free_ = 0;
            if (!done) {
                ORect pr = strip_rect(s.o, s.t, s.n, u, u + PROBE, 0.01, s.depth - 0.01);
                free_ = 1;
                for (int j = 0; j < nused && free_; ++j) free_ = !orect_overlap(&pr, &used[j]);
            }
            if (free_ && !run) { a = u; run = 1; }
            if ((!free_ || done) && run) {
                double b = u;   /* the stretch ends where the first blocked (or untested) probe starts */
                run = 0;
                if (b - a >= STRIP_MIN_LEN) {
                    int before = P.n;
                    double end = lay_rows(&P, &s, road, arch, a, b, &r);
                    /* what a terrace or pair leaves at the end takes a detached house, if it fits */
                    if (arch != HOME_DETACHED && b - end >= 10.0) end = lay_rows(&P, &s, road, HOME_DETACHED, end, b, &r);
                    if (P.n > before) { first = fmin(first, P.out[before].u0); last = fmax(last, end); }
                }
            }
            if (done) break;
        }
        if (last > first && nused < MAX_STRIPS) used[nused++] = strip_rect(s.o, s.t, s.n, first, last, 0.0, s.depth);
    }
    return P.n;
}

/* Until HM3 builds them: two storeys of 2.85 m over a 0.3 m plinth, and a
 * roof over a 9 m deep house at 35 degrees. */
float city_home_height(const HomePlot *p) { return 0.3f + 2.85f * p->storeys + 3.2f; }
