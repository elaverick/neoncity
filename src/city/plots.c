#include "city_internal.h"
#include "home.h"
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
 * rows, semi-detached pairs, or detached houses. The archetype comes from the
 * city's region (plan_settlement_region), and so since HM2b do the plots'
 * frontages, depths and yards (home_yard): a megacity's are tight, small and
 * mostly paved, some rows parking in their front yards.
 * The local streets are laid first, then the busier ones, the longest first;
 * a later strip keeps clear of the earlier ones, so a corner plot fronts one
 * street and the side street's first plot starts behind its rear garden. The
 * plan cuts a suburb into blocks about two plots deep (plan.c, split_suburb),
 * so they meet back to back; what a block leaves between them is backland
 * (yards, HM7).
 * ========================================================================= */

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

/* Frontage of a plot (m), per unit of a terrace, a half of a pair or a detached house: the region's (HM2b). */
static void frontage(int arch, int region, double *lo, double *hi) {
    const HomeYard *Y = home_yard(region);
    *lo = Y->width[arch][0]; *hi = Y->width[arch][1];
}

/* A plot's depth by archetype, the region's (HM2b: tight, between Tokyo's and a British terraced street's). */
static double plot_depth(int arch, int region, Rng *r) {
    const HomeYard *Y = home_yard(region);
    return rng_range(r, Y->depth[arch][0], Y->depth[arch][1]);
}

/* The front yard (HM2b): the region's small one, or a car's depth where the row parks in it (and the plot
 * leaves a house and the rear yard behind). Returns its depth; *pad says which. */
static float front_yard(int arch, int region, float depth, uint32_t h, uint8_t *pad) {
    const HomeYard *Y = home_yard(region);
    const float room = depth - HOME_HOUSE_MIN - Y->rearMin;
    *pad = hash_float(hash_combine(h, 0x9AD0u)) < Y->pad[arch] && room >= HOME_PAD0;
    float f = *pad ? HOME_PAD0 + (HOME_PAD1 - HOME_PAD0) * hash_float(h) : Y->front0 + (Y->front1 - Y->front0) * hash_float(h);
    if (*pad && f > room) f = room;
    return fmaxf(Y->frontMin, fminf(f, room));
}

typedef struct PlotCtx {
    const Block *bk;
    HomePlot    *out;
    int          n, max, rows, region;
    double       lat, lon;
} PlotCtx;

/* The harness's forcing (city_home_force): an archetype for every street, a style for every row. */
static int g_forceArch = -1, g_forceStyle = -1, g_forceStoreys = 0;
void city_home_force(int arch, int style) {
    g_forceArch = arch >= 0 && arch < HOME_ARCH_COUNT ? arch : -1;
    g_forceStyle = style >= 0 && style < HS_COUNT ? style : -1;
}
/* HM5b2: every row's storeys forced too (a style's rarer body for the captures: Korea's one-storey hanok); 0 lets them be */
void city_home_force_storeys(int n) { g_forceStoreys = n >= 1 && n <= HOME_STOREYS_MAX ? n : 0; }

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
     * were mostly built together, so three rows in four take the street's style and storeys */
    uint32_t built = hash_float(hash_combine(rowSeed, 0x5702u)) < 0.75f ? hash_combine(road, (uint32_t)arch) : rowSeed;
    p->style = (uint8_t)(g_forceStyle >= 0 ? g_forceStyle : home_style_pick(P->region, arch, P->lat, P->lon, hash_combine(built, 0x57E1u)));
    p->storeys = (uint8_t)(g_forceStoreys ? g_forceStoreys : home_style_storeys(p->style, arch, hash_combine(built, 0x5701u)));
    /* the row's front yard, with room behind it for the house and the rear yard */
    p->front = front_yard(arch, P->region, p->depth, hash_combine(rowSeed, 0xF407u), &p->pad);
    /* a style built on the street line (HM5e1: townhouses, shophouses, courtyard houses, colonial and self-built rows)
     * has no front yard, and parks in the street */
    if (home_style_street(p->style)) { p->front = 0.0f; p->pad = 0; }
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
    /* a style the harness forces into another region's city lays its plots as its own region does (its frontages, depths,
     * yards and pads), and its rows are its region's (owners, flat roofs, yards' rules): as in a city of its own (HM5b2a) */
    if (g_forceStyle >= 0) P.region = home_style_region(g_forceStyle);
    P.lat = lat; P.lon = lon;
    float w[HOME_ARCH_COUNT];
    region_weights(P.region, lat, lon, w);
    /* the shallowest strip: the region's least front yard, a house and its least rear yard (HM2b) */
    const HomeYard *Y = home_yard(P.region);
    const double minDepth = Y->frontMin + HOME_HOUSE_MIN + Y->rearMin;
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
        if (g_forceArch >= 0) arch = g_forceArch;
        /* the block's depth square to this edge: back to back with the far side's plots, or alone */
        double vmax = 0.0;
        for (int v = 0; v < lot->n; ++v) vmax = fmax(vmax, ddot(dsub(lot->v[v], s.o), s.n));
        double want = plot_depth(arch, P.region, &r);
        s.depth = vmax < 2.0 * want ? 0.5 * vmax - 0.05 : want;
        if (s.depth < minDepth) s.depth = fmin(want, vmax - 0.5);
        if (s.depth < minDepth) continue;
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
                    if (arch != HOME_DETACHED && b - end >= Y->width[HOME_DETACHED][0]) end = lay_rows(&P, &s, road, HOME_DETACHED, end, b, &r);
                    if (P.n > before) { first = fmin(first, P.out[before].u0); last = fmax(last, end); }
                }
            }
            if (done) break;
        }
        if (last > first && nused < MAX_STRIPS) used[nused++] = strip_rect(s.o, s.t, s.n, first, last, 0.0, s.depth);
    }
    return P.n;
}

/* A rough figure: storeys of 2.85 m over a 0.3 m plinth, and a roof over a 9 m deep house at 35 degrees. */
float city_home_height(const HomePlot *p) { return 0.3f + 2.85f * p->storeys + 3.2f; }

void city_home_row(const HomePlot *row, int n, HGraph *g) {
    double x = row[0].ox, z = row[0].oz;
    if (n > 0) {
        const double u = 0.5 * (row[0].u0 + row[n - 1].u1), v = 0.5 * row[0].depth;
        x += u * row[0].tx + v * row[0].nx;
        z += u * row[0].tz + v * row[0].nz;
    }
    home_build(row, n, plan_affluence(x, z), g, NULL);
}
