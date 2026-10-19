#include "plan.h"
#include "hash.h"
#include "mathlib.h"
#include "noise.h"

#define VERT_EPS        0.05     /* metres: vertices closer than this are the same junction */
#define MAX_TOWN_SEEDS  48
#define TOWN_CACHE      96
#define MAX_JDIRS       16

typedef struct Site { int i, j; DVec2 p; uint32_t h; } Site;

static int64_t site_key(Site s) { return (int64_t)(((uint64_t)(uint32_t)s.i << 32) | (uint32_t)s.j); }
static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }


/* ===========================================================================
 * Lattice sites
 * ========================================================================= */

/* Jitter is kept to [0.2, 0.8] of a cell so the nearest site to any point is
 * always within the surrounding 3x3 cells and a Voronoi cell is fully
 * determined by the 5x5 cells around its site. */
static Site town_site(int i, int j) {
    Site s; s.i = i; s.j = j;
    s.h = hash2i(WORLD_SEED ^ 0x70A1u, i, j);
    s.p.x = (i + 0.2 + 0.6 * hf(s.h, 1u)) * TOWN_SIZE;
    s.p.z = (j + 0.2 + 0.6 * hf(s.h, 2u)) * TOWN_SIZE;
    return s;
}

static Site nbhd_site(int i, int j) {
    Site s; s.i = i; s.j = j;
    s.h = hash2i(WORLD_SEED ^ 0x4B6Du, i, j);
    s.p.x = (i + 0.2 + 0.6 * hf(s.h, 1u)) * NBHD_SIZE;
    s.p.z = (j + 0.2 + 0.6 * hf(s.h, 2u)) * NBHD_SIZE;
    return s;
}

int plan_town_regional(int i, int j) {
    if (i == 0 && j == 0) return 1;   /* the player starts in a regional centre */
    return hf(hash2i(WORLD_SEED ^ 0x70A1u, i, j), 3u) < 0.15f;
}

DVec2 plan_town_centre(int i, int j) { return town_site(i, j).p; }

static void town_strength(Site t, float *intensity, float *sigma) {
    float r = (t.i == 0 && t.j == 0) ? 1.0f : hf(t.h, 4u);
    if (plan_town_regional(t.i, t.j)) { *intensity = 1.0f + 0.25f * r; *sigma = 640.0f; }
    else                              { *intensity = 0.5f + 0.3f * r;  *sigma = 400.0f; }
}

/* ===========================================================================
 * Density: one peak per town centre, so the city has many centres
 * ========================================================================= */

static float vnoise(double x, double z, uint32_t salt) { return noise_value2(WORLD_SEED ^ salt, x, z); }

float plan_density(double x, double z) {
    /* Peaks are windowed to zero by 1.8 km, which is never closer than the
     * nearest site outside the 3x3 town cells, so the sum is exact. */
    const double W2 = 1800.0 * 1800.0;
    int ci = (int)floor(x / TOWN_SIZE), cj = (int)floor(z / TOWN_SIZE);
    double sum = 0.08;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            Site t = town_site(ci + di, cj + dj);
            double dx = x - t.p.x, dz = z - t.p.z, r2 = dx * dx + dz * dz;
            if (r2 >= W2) continue;
            float I, sg;
            town_strength(t, &I, &sg);
            double w = 1.0 - r2 / W2;
            sum += I * exp(-r2 / (2.0 * sg * sg)) * w * w;
        }
    sum += 0.16 * (vnoise(x / 850.0, z / 850.0, 0xDE45u) - 0.5);
    /* Through the fringe the whole field falls to a fifth: suburbs, then open land. */
    float f = plan_fringe(x, z);
    if (f > 0.0f) sum *= 1.0 - 0.8 * f;
    return clampf((float)sum, 0.0f, 1.0f);
}

/* Affluence: the regional cores are well off, and so is whatever stands
 * nearest the centres. Beyond that, a slow field makes whole districts
 * better or worse off, and a neighbourhood-scale one varies them within it. */
float plan_affluence(double x, double z) {
    double a = 0.5 + 0.45 * (plan_density(x, z) - 0.4)
                   + 0.6 * (vnoise(x / 1300.0, z / 1300.0, 0xAF1Au) - 0.5)
                   + 0.35 * (vnoise(x / 430.0, z / 430.0, 0xAF1Bu) - 0.5);
    return clampf((float)a, 0.0f, 1.0f);
}

/* ===========================================================================
 * The city's footprint
 * ========================================================================= */

double plan_city_edge(double x, double z) {
    DVec2 c = plan_town_centre(0, 0);
    double wx = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED61u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED63u) - 1.0);
    double wz = CITY_WARP1_M * (2.0 * vnoise(x / CITY_WARP1_WAVE, z / CITY_WARP1_WAVE, 0xED62u) - 1.0)
              + CITY_WARP2_M * (2.0 * vnoise(x / CITY_WARP2_WAVE, z / CITY_WARP2_WAVE, 0xED64u) - 1.0);
    return hypot(x - c.x + wx, z - c.z + wz) - CITY_RADIUS;
}

float plan_urban(double x, double z) { return plan_urban_weight(plan_city_edge(x, z)); }

float plan_fringe(double x, double z) {
    return smoothstepf((float)(CITY_BUILT_EDGE - CITY_FRINGE), (float)CITY_BUILT_EDGE, (float)plan_city_edge(x, z));
}

/* ===========================================================================
 * Convex polygons with per-edge road attributes
 * ========================================================================= */

static RoadAttr no_road(void) { RoadAttr a; memset(&a, 0, sizeof(a)); return a; }

static int near_pt(DVec2 a, DVec2 b, double eps) { return fabs(a.x - b.x) < eps && fabs(a.z - b.z) < eps; }

static void push_v(Poly *o, DVec2 p, RoadAttr a) {
    if (o->n > 0 && near_pt(o->v[o->n - 1], p, 1e-7)) { o->e[o->n - 1] = a; return; }
    if (o->n >= PLAN_MAX_PV) return;
    o->v[o->n] = p; o->e[o->n] = a; o->n++;
}

/* Sutherland-Hodgman against one half-plane. The cut edge gets `cut`; parts of
 * existing edges keep their own attributes. in and out may alias. */
int poly_clip(const Poly *in, DVec2 q, DVec2 n, RoadAttr cut, Poly *out) {
    Poly t; t.n = 0;
    for (int i = 0; i < in->n; ++i) {
        DVec2 a = in->v[i], b = in->v[(i + 1) % in->n];
        double da = (a.x - q.x) * n.x + (a.z - q.z) * n.z;
        double db = (b.x - q.x) * n.x + (b.z - q.z) * n.z;
        int ina = da <= 0.0, inb = db <= 0.0;
        if (ina) push_v(&t, a, in->e[i]);
        if (ina != inb) {
            double s = da / (da - db);
            push_v(&t, dv(a.x + (b.x - a.x) * s, a.z + (b.z - a.z) * s), ina ? cut : in->e[i]);
        }
    }
    if (t.n > 1 && near_pt(t.v[t.n - 1], t.v[0], 1e-7)) t.n--;
    if (t.n < 3) t.n = 0;
    *out = t;
    return out->n;
}

static double signed_area(const Poly *p) {
    double s = 0;
    for (int i = 1; i + 1 < p->n; ++i)
        s += dcross(dsub(p->v[i], p->v[0]), dsub(p->v[i + 1], p->v[0]));
    return 0.5 * s;
}

double poly_area(const Poly *p) { return fabs(signed_area(p)); }

DVec2 poly_centroid(const Poly *p) {
    double ax = 0, az = 0, A = 0;
    for (int i = 1; i + 1 < p->n; ++i) {
        DVec2 a = dsub(p->v[i], p->v[0]), b = dsub(p->v[i + 1], p->v[0]);
        double w = dcross(a, b);
        ax += w * (a.x + b.x) / 3.0; az += w * (a.z + b.z) / 3.0; A += w;
    }
    if (fabs(A) < 1e-9) {
        DVec2 c = dv(0, 0);
        for (int i = 0; i < p->n; ++i) { c.x += p->v[i].x; c.z += p->v[i].z; }
        return p->n ? dv(c.x / p->n, c.z / p->n) : c;
    }
    return dv(p->v[0].x + ax / A, p->v[0].z + az / A);
}

void poly_bounds(const Poly *p, double *x0, double *z0, double *x1, double *z1) {
    *x0 = *z0 = 1e300; *x1 = *z1 = -1e300;
    for (int i = 0; i < p->n; ++i) {
        if (p->v[i].x < *x0) *x0 = p->v[i].x;
        if (p->v[i].z < *z0) *z0 = p->v[i].z;
        if (p->v[i].x > *x1) *x1 = p->v[i].x;
        if (p->v[i].z > *z1) *z1 = p->v[i].z;
    }
}

/* Inside, or no more than `margin` outside (negative margin: at least that far inside). */
int poly_contains(const Poly *p, double x, double z, double margin) {
    if (p->n < 3) return 0;
    double o = signed_area(p) >= 0 ? 1.0 : -1.0;
    DVec2 q = dv(x, z);
    for (int i = 0; i < p->n; ++i) {
        DVec2 a = p->v[i], b = p->v[(i + 1) % p->n];
        DVec2 e = dsub(b, a);
        double l = dlen(e);
        if (l < 1e-9) continue;
        if (o * dcross(e, dsub(q, a)) / l < -margin) return 0;
    }
    return 1;
}

/* Shrink each edge inward by its road half-width (mode 0), sidewalk width
 * (mode 1) or nothing (mode 2), plus `extra`. Corners come out mitred. */
int poly_inset(const Poly *in, int mode, double extra, Poly *out) {
    if (in->n < 3) { out->n = 0; return 0; }
    DVec2 c = poly_centroid(in);
    Poly cur = *in;
    for (int i = 0; i < in->n && cur.n; ++i) {
        DVec2 a = in->v[i], b = in->v[(i + 1) % in->n];
        DVec2 d = dsub(b, a);
        if (dlen(d) < 1e-6) continue;
        d = dnorm(d);
        DVec2 n = dv(d.z, -d.x);
        if (ddot(n, dsub(c, a)) > 0) n = dv(-n.x, -n.z);            /* outward */
        double off = extra + (mode == 0 ? in->e[i].half : mode == 1 ? in->e[i].walk : 0.0);
        if (off <= 0.0) continue;
        poly_clip(&cur, dv(a.x - n.x * off, a.z - n.z * off), n, in->e[i], &cur);
    }
    *out = cur;
    return out->n;
}

static DVec2 frame_extent(const Poly *p, DVec2 cen, DVec2 axis) {
    double lo = 1e300, hi = -1e300;
    for (int i = 0; i < p->n; ++i) {
        double w = ddot(dsub(p->v[i], cen), axis);
        if (w < lo) lo = w;
        if (w > hi) hi = w;
    }
    return dv(lo, hi);
}

/* ===========================================================================
 * Road attributes for Voronoi edges (identical from both sides)
 * ========================================================================= */

static uint32_t pair_hash(uint32_t salt, Site a, Site b) {
    int64_t ka = site_key(a), kb = site_key(b);
    Site lo = ka < kb ? a : b, hi = ka < kb ? b : a;
    return hash_combine(hash2i(WORLD_SEED ^ salt, lo.i, lo.j), hash2i(salt, hi.i, hi.j));
}

static RoadAttr boulevard_attr(Site a, Site b) {
    uint32_t h = pair_hash(0xB0E1u, a, b);
    RoadAttr r = no_road();
    r.id = h | 1u;
    r.cls = ROAD_BOULEVARD;
    r.half = ((h >> 4) & 1u) ? 13.0f : 11.5f;
    r.walk = 5.5f;
    r.lamp = 0;   /* every street lamp is sodium */
    r.own = site_key(a) < site_key(b);
    return r;
}

static RoadAttr arterial_attr(Site a, Site b) {
    uint32_t h = pair_hash(0xA27Eu, a, b);
    RoadAttr r = no_road();
    r.id = h | 1u;
    r.cls = ROAD_ARTERIAL;
    r.half = ((h >> 4) & 1u) ? 9.5f : 8.0f;
    r.walk = ((h >> 6) & 1u) ? 5.0f : 4.5f;
    r.lamp = 0;   /* every street lamp is sodium */
    r.own = site_key(a) < site_key(b);
    return r;
}

/* ===========================================================================
 * Towns and their neighbourhood pieces
 * ========================================================================= */

/* A town's neighbours are the 5 x 5 lattice cells round it, numbered
 * (dj + 2) * 5 + (di + 2); the town itself is NB_SELF, and neighbour k's
 * neighbour back is 24 - k. */
#define NB_SELF 12
static int nb_di(int k) { return k % 5 - 2; }
static int nb_dj(int k) { return k / 5 - 2; }

typedef struct TownEntry {
    int64_t key;
    Site    s;
    Poly    poly;                 /* the town's Voronoi cell; every edge is a boulevard */
    int8_t  nb[PLAN_MAX_PV];      /* the neighbour across each edge */
    double  x0, z0, x1, z1;
    int8_t  built;                /* plan_town_built; -1 until asked */
    int8_t  hasGround;
    Poly    ground;               /* built towns: the cell widened across its perimeter boulevards */
    int8_t  gnb[PLAN_MAX_PV];     /* the neighbour whose bisector each ground edge lies on */
} TownEntry;
typedef struct Piece { Site nb; Poly poly; double x0, z0, x1, z1; } Piece;
typedef struct TownPlan { Site t; Poly poly; int n; Piece pc[MAX_TOWN_SEEDS]; } TownPlan;

typedef struct PlanCtx {
    TownEntry towns[TOWN_CACHE];
    int       ntowns;
    TownEntry scratch;
    TownPlan  plan;
    PlanArea *out;
    double    A[4];      /* query rectangle */
} PlanCtx;

/* The town's cell: a square cut by the bisector with each neighbour, moved
 * out by push[k] (metres) where push is given. nb gets each edge's neighbour. */
static void town_cut(Site t, const double *push, Poly *p, int8_t *nb) {
    const double R = 2.0 * TOWN_SIZE;
    uint32_t ids[25];
    p->n = 4;
    p->v[0] = dv(t.p.x - R, t.p.z - R); p->v[1] = dv(t.p.x + R, t.p.z - R);
    p->v[2] = dv(t.p.x + R, t.p.z + R); p->v[3] = dv(t.p.x - R, t.p.z + R);
    for (int k = 0; k < 4; ++k) p->e[k] = no_road();
    for (int k = 0; k < 25; ++k) {
        ids[k] = 0;
        if (k == NB_SELF) continue;
        Site o = town_site(t.i + nb_di(k), t.j + nb_dj(k));
        RoadAttr a = boulevard_attr(t, o);
        DVec2 n = dsub(o.p, t.p), q = dv((o.p.x + t.p.x) * 0.5, (o.p.z + t.p.z) * 0.5);
        if (push && push[k] > 0.0) q = dadd(q, dscale(dnorm(n), push[k]));
        poly_clip(p, q, n, a, p);
        ids[k] = a.id;
    }
    for (int e = 0; e < p->n; ++e) {
        nb[e] = -1;
        for (int k = 0; k < 25 && nb[e] < 0; ++k)
            if (ids[k] && p->e[e].id == ids[k]) nb[e] = (int8_t)k;
    }
}

/* Whether every point within CITY_GROUND_PAD of convex p that lies in the
 * square of half-width h round (cx, cz) is at or inside CITY_BUILT_EDGE.
 * Proven square by square: no point of a square has an edge measure more
 * than CITY_EDGE_RATE times its half-diagonal above the measure at its
 * centre. A square still unsettled at the last depth counts as failing, so a
 * town is never built on a guess. */
static int ground_inside(const Poly *p, double cx, double cz, double h, int depth) {
    double r = h * 1.4142135623730951;
    if (!poly_contains(p, cx, cz, r + CITY_GROUND_PAD)) return 1;   /* the square misses the padded cell */
    double e = plan_city_edge(cx, cz);
    if (e + CITY_EDGE_RATE * r <= CITY_BUILT_EDGE) return 1;
    if (depth == 0 || (e > CITY_BUILT_EDGE && poly_contains(p, cx, cz, CITY_GROUND_PAD))) return 0;
    h *= 0.5;
    return ground_inside(p, cx - h, cz - h, h, depth - 1) && ground_inside(p, cx + h, cz - h, h, depth - 1) &&
           ground_inside(p, cx - h, cz + h, h, depth - 1) && ground_inside(p, cx + h, cz + h, h, depth - 1);
}

static int cell_built(Site s, const Poly *p, double x0, double z0, double x1, double z1) {
    if (plan_city_edge(s.p.x, s.p.z) > CITY_BUILT_EDGE) return 0;   /* the site is in its own cell */
    double h = 0.5 * fmax(x1 - x0, z1 - z0) + CITY_GROUND_PAD;
    return ground_inside(p, 0.5 * (x0 + x1), 0.5 * (z0 + z1), h, 7);
}

int plan_town_built(int i, int j) {
    Site s = town_site(i, j);
    Poly p;
    int8_t nb[PLAN_MAX_PV];
    double x0, z0, x1, z1;
    town_cut(s, NULL, &p, nb);
    poly_bounds(&p, &x0, &z0, &x1, &z1);
    return cell_built(s, &p, x0, z0, x1, z1);
}

/* The returned pointer is valid until the next town_get call (or anything
 * that makes one: town_built, town_ground). */
static TownEntry *town_get(PlanCtx *c, int i, int j) {
    Site s = town_site(i, j);
    int64_t key = site_key(s);
    for (int k = 0; k < c->ntowns; ++k)
        if (c->towns[k].key == key) return &c->towns[k];
    TownEntry *e = c->ntowns < TOWN_CACHE ? &c->towns[c->ntowns++] : &c->scratch;
    e->key = key;
    e->s = s;
    town_cut(s, NULL, &e->poly, e->nb);
    poly_bounds(&e->poly, &e->x0, &e->z0, &e->x1, &e->z1);
    e->built = -1;
    e->hasGround = 0;
    return e;
}

static int town_built(PlanCtx *c, int i, int j) {
    TownEntry *e = town_get(c, i, j);
    if (e->built < 0) e->built = (int8_t)cell_built(e->s, &e->poly, e->x0, e->z0, e->x1, e->z1);
    return e->built;
}

/* A built town's ground: its cell, with each edge to a town that isn't built
 * moved out across that boulevard to the shoulder beyond its carriageway.
 * Edges to built towns stay where they are, so neighbouring grounds meet
 * without overlapping. Its edges carry no roads. */
static const TownEntry *town_ground(PlanCtx *c, int i, int j) {
    TownEntry *e = town_get(c, i, j);
    if (e->hasGround) return e;
    int8_t nb[PLAN_MAX_PV];
    int n = e->poly.n;
    memcpy(nb, e->nb, sizeof(nb));
    Site s = e->s;
    double push[25] = { 0 };
    for (int k = 0; k < n; ++k)
        if (nb[k] >= 0 && !town_built(c, i + nb_di(nb[k]), j + nb_dj(nb[k])))
            push[nb[k]] = boulevard_attr(s, town_site(i + nb_di(nb[k]), j + nb_dj(nb[k]))).half + CITY_SHOULDER;
    e = town_get(c, i, j);   /* the neighbours' lookups may have reused its slot */
    town_cut(s, push, &e->ground, e->gnb);
    for (int k = 0; k < e->ground.n; ++k) e->ground.e[k] = no_road();
    e->hasGround = 1;
    return e;
}

/* A town's neighbourhoods: Voronoi cells of the neighbourhood sites that fall
 * inside the town, computed among those sites only and clipped to the town.
 * A boulevard to a town that isn't built is the city's perimeter road, and
 * this side of it, the only side there is, owns it. */
static void town_plan(PlanCtx *c, Site t, TownPlan *tp) {
    const TownEntry *te = town_get(c, t.i, t.j);
    int8_t nb[PLAN_MAX_PV];
    memcpy(nb, te->nb, sizeof(nb));
    tp->t = t;
    tp->poly = te->poly;
    tp->n = 0;
    for (int k = 0; k < tp->poly.n; ++k)
        if (nb[k] >= 0 && !town_built(c, t.i + nb_di(nb[k]), t.j + nb_dj(nb[k]))) tp->poly.e[k].own = 1;
    te = town_get(c, t.i, t.j);
    double x0 = te->x0, z0 = te->z0, x1 = te->x1, z1 = te->z1;
    Site seeds[MAX_TOWN_SEEDS];
    int ns = 0;
    for (int j = (int)floor(z0 / NBHD_SIZE); j <= (int)floor(z1 / NBHD_SIZE); ++j)
        for (int i = (int)floor(x0 / NBHD_SIZE); i <= (int)floor(x1 / NBHD_SIZE); ++i) {
            Site s = nbhd_site(i, j);
            if (ns < MAX_TOWN_SEEDS && poly_contains(&tp->poly, s.p.x, s.p.z, -1.0)) seeds[ns++] = s;
        }
    if (ns == 0) {
        seeds[0] = nbhd_site((int)floor(t.p.x / NBHD_SIZE), (int)floor(t.p.z / NBHD_SIZE));
        seeds[0].p = t.p;
        ns = 1;
    }
    for (int k = 0; k < ns; ++k) {
        Piece *pc = &tp->pc[tp->n];
        pc->nb = seeds[k];
        pc->poly = tp->poly;
        for (int m = 0; m < ns && pc->poly.n; ++m) {
            if (m == k) continue;
            DVec2 n = dsub(seeds[m].p, seeds[k].p);
            DVec2 q = dv((seeds[m].p.x + seeds[k].p.x) * 0.5, (seeds[m].p.z + seeds[k].p.z) * 0.5);
            poly_clip(&pc->poly, q, n, arterial_attr(seeds[k], seeds[m]), &pc->poly);
        }
        if (pc->poly.n < 3) continue;
        poly_bounds(&pc->poly, &pc->x0, &pc->z0, &pc->x1, &pc->z1);
        tp->n++;
    }
}

/* ===========================================================================
 * Junctions: how far each carriageway stops short of the junction centre
 * ========================================================================= */

typedef struct JDir { DVec2 d; float half; } JDir;

static void add_dir(JDir *d, int *n, DVec2 v, RoadAttr a) {
    if (!a.cls || *n >= MAX_JDIRS) return;
    v = dnorm(v);
    for (int k = 0; k < *n; ++k)
        if (ddot(d[k].d, v) > 0.9995) return;
    d[*n].d = v; d[*n].half = a.half; (*n)++;
}

static void gather_dirs(const Poly *p, DVec2 P, JDir *d, int *n) {
    for (int k = 0; k < p->n; ++k) {
        if (!near_pt(p->v[k], P, VERT_EPS)) continue;
        int nx = (k + 1) % p->n, pv = (k + p->n - 1) % p->n;
        add_dir(d, n, dsub(p->v[nx], p->v[k]), p->e[k]);
        add_dir(d, n, dsub(p->v[pv], p->v[k]), p->e[pv]);
    }
}

/* Distance along `dir` at which a carriageway of half-width `half` clears
 * every other road leaving the same point. */
static double clearance(DVec2 dir, float half, const JDir *d, int n) {
    double r = 0.0;
    for (int k = 0; k < n; ++k) {
        double c = ddot(dir, d[k].d);
        if (c > 0.9995 || c < -0.96) continue;   /* itself, or straight on */
        double s = fabs(dcross(dir, d[k].d));
        double need = (d[k].half + half * fabs(c)) / (s > 0.15 ? s : 0.15);
        if (need > r) r = need;
    }
    return r > 0.0 ? minf((float)(r + 0.5), 80.0f) : 0.0;
}

/* Side road meeting a through road at a T. */
static double t_trim(DVec2 dir, float half, DVec2 edge, float edgeHalf) {
    edge = dnorm(edge);
    double c = fabs(ddot(dir, edge)), s = fabs(dcross(dir, edge));
    return minf((float)((edgeHalf + half * c) / (s > 0.15 ? s : 0.15) + 0.3), 80.0f);
}

/* Boulevards end at town Voronoi vertices, where three towns meet. Only the
 * built towns' boulevards are there: between two towns beyond the city's
 * edge there is none. */
static double town_vertex_trim(PlanCtx *c, DVec2 P, DVec2 dir, float half) {
    int ci = (int)floor(P.x / TOWN_SIZE), cj = (int)floor(P.z / TOWN_SIZE);
    double dist[9], best = 1e300;
    for (int k = 0; k < 9; ++k) {
        Site s = town_site(ci + k % 3 - 1, cj + k / 3 - 1);
        dist[k] = dlen(dsub(P, s.p));
        if (dist[k] < best) best = dist[k];
    }
    JDir d[MAX_JDIRS];
    int n = 0;
    for (int k = 0; k < 9; ++k)
        if (dist[k] <= best + VERT_EPS && town_built(c, ci + k % 3 - 1, cj + k / 3 - 1))
            gather_dirs(&town_get(c, ci + k % 3 - 1, cj + k / 3 - 1)->poly, P, d, &n);
    return clearance(dir, half, d, n);
}

/* An arterial ends either at a T on the town's boulevard, or where three
 * neighbourhoods of the same town meet. `other` is the piece edge sharing the vertex. */
static double arterial_trim(const TownPlan *tp, const Poly *p, int vtx, int other, DVec2 dir, float half) {
    RoadAttr oe = p->e[other];
    if (oe.cls == ROAD_BOULEVARD)
        return t_trim(dir, half, dsub(p->v[(other + 1) % p->n], p->v[other]), oe.half);
    JDir d[MAX_JDIRS];
    int n = 0;
    for (int k = 0; k < tp->n; ++k) gather_dirs(&tp->pc[k].poly, p->v[vtx], d, &n);
    return clearance(dir, half, d, n);
}

/* ===========================================================================
 * Output
 * ========================================================================= */

static int box_overlaps(const double A[4], double x0, double z0, double x1, double z1, double pad) {
    return x1 + pad >= A[0] && x0 - pad <= A[2] && z1 + pad >= A[1] && z0 - pad <= A[3];
}

static void add_road(PlanCtx *c, DVec2 a, DVec2 b, double ta, double tb, RoadAttr at) {
    PlanArea *o = c->out;
    double pad = at.half + at.walk + 1.0;
    if (!box_overlaps(c->A, fmin(a.x, b.x), fmin(a.z, b.z), fmax(a.x, b.x), fmax(a.z, b.z), pad)) return;
    if (o->nroads >= PLAN_MAX_ROADS) return;
    Road *r = &o->roads[o->nroads++];
    r->a = a; r->b = b; r->trimA = ta; r->trimB = tb; r->at = at;
}

/* ===========================================================================
 * Blocks: recursive split of a neighbourhood along its grain
 * ========================================================================= */

typedef struct SplitCtx {
    PlanCtx        *c;
    const TownPlan *tp;
    const Piece    *pc;
    uint32_t        seed;
    DVec2           gu, gv;      /* grain frame */
    double          blockSize;
    uint8_t         lamp;
} SplitCtx;

/* Towards the edge the city gives way in bands of the fringe (plan_fringe,
 * wobbled by a slow field so they don't ring the city): industry and
 * logistics, then parks and allotments, then open land with the odd
 * farmstead. A town's or neighbourhood's centre keeps its high street
 * wherever it stands; the core has thinned out with the density. */
#define FRINGE_INDUSTRY 0.35f
#define FRINGE_PARKS    0.6f
#define FRINGE_RURAL    0.8f
static int zone_for(const SplitCtx *sc, const Block *b, DVec2 cen, float d) {
    if (d > 0.62f) return ZONE_CORE;
    if (dlen(dsub(cen, sc->pc->nb.p)) < 110.0 || dlen(dsub(cen, sc->tp->t.p)) < 170.0) return ZONE_CENTRE;
    float f = plan_fringe(cen.x, cen.z);
    if (f > 0.0f) f = clampf(f + 0.3f * (vnoise(cen.x / 1100.0, cen.z / 1100.0, 0xF41Bu) - 0.5f), 0.0f, 1.0f);
    float r = hf(b->seed, 3u);
    if (f >= FRINGE_RURAL) return r < 0.8f ? ZONE_RURAL : r < 0.9f ? ZONE_PARK : ZONE_RESIDENTIAL;
    if (f >= FRINGE_PARKS) return r < 0.45f ? ZONE_PARK : r < 0.6f ? ZONE_RURAL : ZONE_RESIDENTIAL;
    if (f >= FRINGE_INDUSTRY) return r < 0.55f ? ZONE_INDUSTRIAL : r < 0.65f ? ZONE_PARK : ZONE_RESIDENTIAL;
    int boulevard = 0;
    for (int i = 0; i < b->curb.n; ++i) if (b->curb.e[i].cls == ROAD_BOULEVARD) boulevard = 1;
    if (hf(b->seed, 1u) < (boulevard ? 0.22f : 0.07f) + 0.2f * f) return ZONE_PARK;   /* green buffers along the boulevards */
    if (d < 0.32f && hf(b->seed, 2u) < 0.25f + 0.3f * f) return ZONE_INDUSTRIAL;
    return ZONE_RESIDENTIAL;
}

static void emit_block(SplitCtx *sc, const Poly *leaf, uint32_t path) {
    PlanArea *out = sc->c->out;
    double x0, z0, x1, z1;
    poly_bounds(leaf, &x0, &z0, &x1, &z1);
    if (!box_overlaps(sc->c->A, x0, z0, x1, z1, 0.0) || out->nblocks >= PLAN_MAX_BLOCKS) return;
    Block *b = &out->blocks[out->nblocks];
    if (!poly_inset(leaf, 0, 0.0, &b->curb) || poly_area(&b->curb) < 40.0) return;
    poly_bounds(&b->curb, &b->x0, &b->z0, &b->x1, &b->z1);
    if (!box_overlaps(sc->c->A, b->x0, b->z0, b->x1, b->z1, 0.0)) return;
    if (!poly_inset(&b->curb, 1, 0.0, &b->lot) || poly_area(&b->lot) < 60.0) b->lot.n = 0;
    b->seed = hash_combine(sc->seed, path ^ 0xB10C0000u);
    DVec2 cen = poly_centroid(&b->curb);
    b->density = plan_density(cen.x, cen.z);
    b->gc = (float)sc->gu.x; b->gs = (float)sc->gu.z;
    b->zone = zone_for(sc, b, cen, b->density);
    out->nblocks++;
}

static void split_rec(SplitCtx *sc, const Poly *p, uint32_t path, int depth) {
    Rng r = rng_make(hash_combine(sc->seed, path));
    DVec2 cen = poly_centroid(p);
    DVec2 eu = frame_extent(p, cen, sc->gu), ev = frame_extent(p, cen, sc->gv);
    double su = eu.z - eu.x, sv = ev.z - ev.x;
    double B = sc->blockSize * rng_range(&r, 0.85f, 1.15f);
    if ((su <= B && sv <= B) || depth >= 8) { emit_block(sc, p, path); return; }

    int alongU = su >= sv;
    if (fabs(su - sv) < 0.12 * B) alongU = (rng_u32(&r) & 1u) != 0;
    DVec2 axis = alongU ? sc->gu : sc->gv;
    DVec2 ext = alongU ? eu : ev;
    double cut = ext.x + (ext.z - ext.x) * rng_range(&r, 0.38f, 0.62f);
    DVec2 q = dv(cen.x + axis.x * cut, cen.z + axis.z * cut);

    /* The chord where the new street crosses the polygon, and the edges it meets. */
    DVec2 hit[2];
    int edge[2], nh = 0;
    for (int i = 0; i < p->n && nh < 2; ++i) {
        DVec2 a = p->v[i], b = p->v[(i + 1) % p->n];
        double da = ddot(dsub(a, q), axis), db = ddot(dsub(b, q), axis);
        if ((da <= 0.0) != (db <= 0.0)) {
            double s = da / (da - db);
            hit[nh] = dv(a.x + (b.x - a.x) * s, a.z + (b.z - a.z) * s);
            edge[nh++] = i;
        }
    }
    double chord = nh == 2 ? dlen(dsub(hit[1], hit[0])) : 0.0;
    if (nh != 2 || chord < 25.0) { emit_block(sc, p, path); return; }

    uint32_t h = hash_combine(sc->seed, path * 7919u + 13u);
    RoadAttr at = no_road();
    at.id = h | 1u;
    at.cls = ROAD_LOCAL;
    if (depth == 0 && chord > 260.0) { at.half = 7.0f; at.walk = 4.5f; }   /* collector */
    else { at.half = ((h >> 3) & 1u) ? 6.0f : 5.0f; at.walk = ((h >> 4) & 1u) ? 4.0f : 3.5f; }
    at.lamp = sc->lamp;
    at.own = 1;

    Poly lo, hi;
    poly_clip(p, q, axis, at, &lo);
    poly_clip(p, q, dv(-axis.x, -axis.z), at, &hi);
    double A = poly_area(p);
    if (!lo.n || !hi.n || poly_area(&lo) < 0.12 * A || poly_area(&hi) < 0.12 * A) { emit_block(sc, p, path); return; }

    DVec2 dir = dnorm(dsub(hit[1], hit[0]));
    RoadAttr e0 = p->e[edge[0]], e1 = p->e[edge[1]];
    double ta = t_trim(dir, at.half, dsub(p->v[(edge[0] + 1) % p->n], p->v[edge[0]]), e0.half);
    double tb = t_trim(dir, at.half, dsub(p->v[(edge[1] + 1) % p->n], p->v[edge[1]]), e1.half);
    add_road(sc->c, hit[0], hit[1], ta, tb, at);

    split_rec(sc, &lo, path * 2u, depth + 1);
    split_rec(sc, &hi, path * 2u + 1u, depth + 1);
}

/* ===========================================================================
 * Driver
 * ========================================================================= */

static void process_piece(PlanCtx *c, const TownPlan *tp, const Piece *pc) {
    const Poly *p = &pc->poly;
    /* Arterials: each shared edge is emitted once, by the side that owns it. */
    for (int k = 0; k < p->n; ++k) {
        RoadAttr e = p->e[k];
        if (e.cls != ROAD_ARTERIAL || !e.own) continue;
        DVec2 a = p->v[k], b = p->v[(k + 1) % p->n];
        DVec2 d = dnorm(dsub(b, a));
        double pad = e.half + e.walk + 1.0;
        if (!box_overlaps(c->A, fmin(a.x, b.x), fmin(a.z, b.z), fmax(a.x, b.x), fmax(a.z, b.z), pad)) continue;
        double ta = arterial_trim(tp, p, k, (k + p->n - 1) % p->n, d, e.half);
        double tb = arterial_trim(tp, p, (k + 1) % p->n, (k + 1) % p->n, dv(-d.x, -d.z), e.half);
        add_road(c, a, b, ta, tb, e);
    }

    SplitCtx sc;
    sc.c = c; sc.tp = tp; sc.pc = pc;
    sc.seed = hash2i(WORLD_SEED ^ 0x9EC3u, pc->nb.i, pc->nb.j);
    /* Grain: usually square to the neighbourhood's longest boundary road,
     * otherwise its own angle, so each cell has a distinct internal pattern. */
    double ang = hf(sc.seed, 1u) * PI_F * 0.5;
    if (hf(sc.seed, 2u) < 0.65f) {
        double best = -1.0;
        for (int k = 0; k < p->n; ++k) {
            DVec2 e = dsub(p->v[(k + 1) % p->n], p->v[k]);
            if (dlen(e) > best) { best = dlen(e); ang = atan2(e.z, e.x); }
        }
    }
    sc.gu = dv(cos(ang), sin(ang));
    sc.gv = dv(-sc.gu.z, sc.gu.x);
    DVec2 cen = poly_centroid(p);
    float d = plan_density(cen.x, cen.z);
    sc.blockSize = 230.0 - 120.0 * d;
    sc.lamp = 0;  /* every street lamp is sodium */
    split_rec(&sc, p, 1u, 0);
}

static void process_town(PlanCtx *c, const TownPlan *tp) {
    const Poly *p = &tp->poly;
    for (int k = 0; k < p->n; ++k) {
        RoadAttr e = p->e[k];
        if (e.cls != ROAD_BOULEVARD || !e.own) continue;
        DVec2 a = p->v[k], b = p->v[(k + 1) % p->n];
        DVec2 d = dnorm(dsub(b, a));
        double pad = e.half + e.walk + 1.0;
        if (!box_overlaps(c->A, fmin(a.x, b.x), fmin(a.z, b.z), fmax(a.x, b.x), fmax(a.z, b.z), pad)) continue;
        double ta = town_vertex_trim(c, a, d, e.half);
        double tb = town_vertex_trim(c, b, dv(-d.x, -d.z), e.half);
        add_road(c, a, b, ta, tb, e);
    }
    for (int k = 0; k < tp->n; ++k) {
        const Piece *pc = &tp->pc[k];
        if (box_overlaps(c->A, pc->x0, pc->z0, pc->x1, pc->z1, 25.0)) process_piece(c, tp, pc);
    }
}

/* ===========================================================================
 * The city's ground and its outline
 * ========================================================================= */

static void add_edge(PlanCtx *c, DVec2 a, DVec2 b, DVec2 n) {
    PlanArea *o = c->out;
    if (dlen(dsub(b, a)) < 1e-3 || o->nedges >= PLAN_MAX_EDGES) return;
    if (!box_overlaps(c->A, fmin(a.x, b.x), fmin(a.z, b.z), fmax(a.x, b.x), fmax(a.z, b.z), 2.0)) return;
    PlanEdge *e = &o->edges[o->nedges++];
    e->a = a; e->b = b; e->n = n;
}

/* Built town (i, j)'s ground, and its part of the outline: every edge facing
 * a town that isn't built, and where an edge is shared with a built town's
 * ground, whatever of it that ground doesn't also reach (the jog where two
 * perimeter roads meet at an inward corner). */
static void town_outline(PlanCtx *c, int i, int j) {
    PlanArea *out = c->out;
    const TownEntry *te = town_ground(c, i, j);
    Poly g = te->ground;
    int8_t gnb[PLAN_MAX_PV];
    memcpy(gnb, te->gnb, sizeof(gnb));
    double gx0, gz0, gx1, gz1;
    poly_bounds(&g, &gx0, &gz0, &gx1, &gz1);
    if (!box_overlaps(c->A, gx0, gz0, gx1, gz1, 2.0)) return;
    if (out->ntowns < PLAN_MAX_TOWNS) out->ground[out->ntowns++] = g;
    for (int k = 0; k < g.n; ++k) {
        DVec2 a = g.v[k], b = g.v[(k + 1) % g.n];
        double len = dlen(dsub(b, a));
        if (len < 1e-3) continue;
        DVec2 u = dscale(dsub(b, a), 1.0 / len), n = dv(u.z, -u.x);   /* the cut square winds so this points out */
        if (!box_overlaps(c->A, fmin(a.x, b.x), fmin(a.z, b.z), fmax(a.x, b.x), fmax(a.z, b.z), 2.0)) continue;
        int on = gnb[k];
        int oi = on >= 0 ? i + nb_di(on) : 0, oj = on >= 0 ? j + nb_dj(on) : 0;
        if (on < 0 || !town_built(c, oi, oj)) { add_edge(c, a, b, n); continue; }
        /* Shared: the other ground's edge on the same bisector, as an interval along this one. */
        const TownEntry *oe = town_ground(c, oi, oj);
        double s0 = 0.0, s1 = 0.0;
        int found = 0;
        for (int q = 0; q < oe->ground.n && !found; ++q) {
            if (oe->gnb[q] != 24 - on) continue;
            DVec2 p0 = oe->ground.v[q], p1 = oe->ground.v[(q + 1) % oe->ground.n];
            double t0 = ddot(dsub(p0, a), u), t1 = ddot(dsub(p1, a), u);
            s0 = fmin(t0, t1); s1 = fmax(t0, t1);
            found = 1;
        }
        if (!found || s1 <= 0.0 || s0 >= len) { add_edge(c, a, b, n); continue; }
        if (s0 > 1e-6) add_edge(c, a, dadd(a, dscale(u, s0)), n);
        if (s1 < len - 1e-6) add_edge(c, dadd(a, dscale(u, s1)), b, n);
    }
}

void plan_area(double x0, double z0, double x1, double z1, PlanArea *out) {
    out->x0 = x0; out->z0 = z0; out->x1 = x1; out->z1 = z1;
    out->nroads = out->nblocks = 0;
    out->bounded = out->ntowns = out->nedges = 0;
    PlanCtx *c = (PlanCtx *)calloc(1, sizeof(PlanCtx));
    if (!c) fatal("out of memory in plan_area");
    c->out = out;
    c->A[0] = x0; c->A[1] = z0; c->A[2] = x1; c->A[3] = z1;
    const double m = 1.2 * TOWN_SIZE;
    int ti0 = (int)floor((x0 - m) / TOWN_SIZE), ti1 = (int)floor((x1 + m) / TOWN_SIZE);
    int tj0 = (int)floor((z0 - m) / TOWN_SIZE), tj1 = (int)floor((z1 + m) / TOWN_SIZE);
    for (int tj = tj0; tj <= tj1; ++tj)
        for (int ti = ti0; ti <= ti1; ++ti) {
            const TownEntry *te = town_get(c, ti, tj);
            if (!box_overlaps(c->A, te->x0, te->z0, te->x1, te->z1, 30.0)) continue;
            Site t = te->s;
            if (!town_built(c, ti, tj)) { out->bounded = 1; continue; }
            town_plan(c, t, &c->plan);
            process_town(c, &c->plan);
        }
    /* When every cell near the rectangle is built they tile it, and the ground
     * covers it all. Otherwise: the built towns' ground, and its outline. */
    if (out->bounded)
        for (int tj = tj0; tj <= tj1; ++tj)
            for (int ti = ti0; ti <= ti1; ++ti) {
                const TownEntry *te = town_get(c, ti, tj);
                if (box_overlaps(c->A, te->x0, te->z0, te->x1, te->z1, 30.0) && town_built(c, ti, tj)) town_outline(c, ti, tj);
            }
    free(c);
}
