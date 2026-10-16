#include "building.h"
#include "gen_common.h"
#include "hash.h"
#include "mathlib.h"
#include <stdarg.h>

#define G0          SIDEWALK_HEIGHT
#define MAX_ATTEMPTS 6

static const char *ARCH_NAMES[ARCH_COUNT] = {
    "office block", "skyscraper", "podium with projection", "twin towers", "gateway",
    "arcology", "shophouse row", "slab block", "point block", "shed" };
static const char *DIST_NAMES[DIST_COUNT] = { "downtown", "midrise", "centre", "estate", "industrial" };
static const char *ARR_NAMES[] = { "ground", "stack", "arcade", "projection", "step-out", "span", "sky bridge" };

const char *bgen_archetype_name(int a) { return a >= 0 && a < ARCH_COUNT ? ARCH_NAMES[a] : "?"; }
const char *bgen_district_name(int d) { return d >= 0 && d < DIST_COUNT ? DIST_NAMES[d] : "?"; }

float bgen_bay_width(uint32_t seed, int fine) {
    float h = hash_float(hash_combine(hash_u32(seed ^ 0x5f3759dfu), 4u));
    return fine ? 1.6f + 0.8f * h : 2.4f + 2.6f * h;
}

/* ===========================================================================
 * Polygons (simple, possibly non-convex)
 * ========================================================================= */

float bloop_area(const BLoop *l) {
    double s = 0.0;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        s += (double)l->x[i] * l->z[j] - (double)l->z[i] * l->x[j];
    }
    return (float)(0.5 * s);
}

static void loop_add(BLoop *l, float x, float z) {
    if (l->n < BG_MAX_PV) { l->x[l->n] = x; l->z[l->n] = z; l->n++; }
}

static void loop_reverse(BLoop *l) {
    for (int i = 0, j = l->n - 1; i < j; ++i, --j) {
        float tx = l->x[i], tz = l->z[i];
        l->x[i] = l->x[j]; l->z[i] = l->z[j];
        l->x[j] = tx; l->z[j] = tz;
    }
}

/* Outer loops negative area, courtyard loops positive (see building.h). */
static void loop_orient(BLoop *l, int outer) {
    float a = bloop_area(l);
    if ((outer && a > 0.0f) || (!outer && a < 0.0f)) loop_reverse(l);
}

int bloop_inside(const BLoop *l, float x, float z) {
    int in = 0;
    for (int i = 0, j = l->n - 1; i < l->n; j = i++) {
        if ((l->z[i] > z) != (l->z[j] > z)) {
            float xc = l->x[j] + (z - l->z[j]) * (l->x[i] - l->x[j]) / (l->z[i] - l->z[j]);
            if (x < xc) in = !in;
        }
    }
    return in;
}

int bfoot_contains(const BFoot *f, float x, float z) {
    return bloop_inside(&f->outer, x, z) && !(f->hole.n && bloop_inside(&f->hole, x, z));
}

static float seg_d2(float px, float pz, float ax, float az, float bx, float bz, float *qx, float *qz) {
    float ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
    float t = l2 > 1e-12f ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float x = ax + ex * t, z = az + ez * t;
    if (qx) { *qx = x; *qz = z; }
    return (px - x) * (px - x) + (pz - z) * (pz - z);
}

static float loop_dist2(const BLoop *l, float x, float z, float *qx, float *qz, float best) {
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        float cx, cz, d = seg_d2(x, z, l->x[i], l->z[i], l->x[j], l->z[j], &cx, &cz);
        if (d < best) { best = d; if (qx) { *qx = cx; *qz = cz; } }
    }
    return best;
}

float bfoot_boundary_dist(const BFoot *f, float x, float z, float *qx, float *qz) {
    float d = loop_dist2(&f->outer, x, z, qx, qz, 1e30f);
    if (f->hole.n) d = loop_dist2(&f->hole, x, z, qx, qz, d);
    return sqrtf(d);
}

static float orient2(float ax, float az, float bx, float bz, float cx, float cz) {
    return (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
}

static int segs_cross(float ax, float az, float bx, float bz, float cx, float cz, float dx, float dz) {
    float o1 = orient2(ax, az, bx, bz, cx, cz), o2 = orient2(ax, az, bx, bz, dx, dz);
    float o3 = orient2(cx, cz, dx, dz, ax, az), o4 = orient2(cx, cz, dx, dz, bx, bz);
    const float e = 1e-5f;
    return ((o1 > e && o2 < -e) || (o1 < -e && o2 > e)) && ((o3 > e && o4 < -e) || (o3 < -e && o4 > e));
}

static int loop_simple(const BLoop *l) {
    for (int i = 0; i < l->n; ++i)
        for (int j = i + 2; j < l->n; ++j) {
            if (i == 0 && j == l->n - 1) continue;
            int i1 = (i + 1) % l->n, j1 = (j + 1) % l->n;
            if (segs_cross(l->x[i], l->z[i], l->x[i1], l->z[i1], l->x[j], l->z[j], l->x[j1], l->z[j1])) return 0;
        }
    return 1;
}

static float loop_min_edge(const BLoop *l) {
    float m = 1e30f;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        float d = sqrtf((l->x[j] - l->x[i]) * (l->x[j] - l->x[i]) + (l->z[j] - l->z[i]) * (l->z[j] - l->z[i]));
        if (d < m) m = d;
    }
    return m;
}

static float loop_perimeter(const BLoop *l) {
    float p = 0.0f;
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n;
        p += sqrtf((l->x[j] - l->x[i]) * (l->x[j] - l->x[i]) + (l->z[j] - l->z[i]) * (l->z[j] - l->z[i]));
    }
    return p;
}

static void loop_bounds(const BLoop *l, float *x0, float *z0, float *x1, float *z1) {
    *x0 = *z0 = 1e30f; *x1 = *z1 = -1e30f;
    for (int i = 0; i < l->n; ++i) {
        *x0 = minf(*x0, l->x[i]); *x1 = maxf(*x1, l->x[i]);
        *z0 = minf(*z0, l->z[i]); *z1 = maxf(*z1, l->z[i]);
    }
}

static int loops_intersect(const BLoop *a, const BLoop *b) {
    for (int i = 0; i < a->n; ++i) {
        int i1 = (i + 1) % a->n;
        for (int j = 0; j < b->n; ++j) {
            int j1 = (j + 1) % b->n;
            if (segs_cross(a->x[i], a->z[i], a->x[i1], a->z[i1], b->x[j], b->z[j], b->x[j1], b->z[j1])) return 1;
        }
    }
    for (int i = 0; i < a->n; ++i) if (bloop_inside(b, a->x[i], a->z[i])) return 1;
    for (int j = 0; j < b->n; ++j) if (bloop_inside(a, b->x[j], b->z[j])) return 1;
    return 0;
}

static int loop_convex(const BLoop *l) {
    for (int i = 0; i < l->n; ++i) {
        int j = (i + 1) % l->n, k = (i + 2) % l->n;
        if (orient2(l->x[i], l->z[i], l->x[j], l->z[j], l->x[k], l->z[k]) > 1e-4f) return 0;  /* outer loops turn negative */
    }
    return 1;
}

/* Ear clipping; a courtyard is joined to the outer loop by a bridge first. */
int bfoot_triangulate(const BFoot *f, float (*pts)[2], int *npts, int (*tri)[3], int maxTri) {
    int no = f->outer.n, nh = f->hole.n, np = 0;
    for (int i = 0; i < no; ++i) { pts[np][0] = f->outer.x[i]; pts[np][1] = f->outer.z[i]; np++; }
    for (int i = 0; i < nh; ++i) { pts[np][0] = f->hole.x[i]; pts[np][1] = f->hole.z[i]; np++; }
    *npts = np;
    int idx[2 * BG_MAX_PV + 4], n = 0;
    if (!nh) {
        for (int i = 0; i < no; ++i) idx[n++] = i;
    } else {
        /* Bridge from the hole vertex with the largest x to the nearest outer
         * vertex that can see it without crossing any edge. */
        int hm = 0;
        for (int i = 1; i < nh; ++i) if (f->hole.x[i] > f->hole.x[hm]) hm = i;
        float hx = f->hole.x[hm], hz = f->hole.z[hm], best = 1e30f;
        int ov = 0;
        for (int i = 0; i < no; ++i) {
            float d = (f->outer.x[i] - hx) * (f->outer.x[i] - hx) + (f->outer.z[i] - hz) * (f->outer.z[i] - hz);
            if (d >= best) continue;
            int blocked = 0;
            for (int k = 0; k < no && !blocked; ++k) {
                int k1 = (k + 1) % no;
                if (k == i || k1 == i) continue;
                blocked = segs_cross(hx, hz, f->outer.x[i], f->outer.z[i], f->outer.x[k], f->outer.z[k], f->outer.x[k1], f->outer.z[k1]);
            }
            for (int k = 0; k < nh && !blocked; ++k) {
                int k1 = (k + 1) % nh;
                if (k == hm || k1 == hm) continue;
                blocked = segs_cross(hx, hz, f->outer.x[i], f->outer.z[i], f->hole.x[k], f->hole.z[k], f->hole.x[k1], f->hole.z[k1]);
            }
            if (!blocked) { best = d; ov = i; }
        }
        for (int i = 0; i <= ov; ++i) idx[n++] = i;
        for (int k = 0; k <= nh; ++k) idx[n++] = no + (hm + k) % nh;
        for (int i = ov; i < no; ++i) idx[n++] = i;
    }
    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        area += pts[idx[i]][0] * pts[idx[j]][1] - pts[idx[i]][1] * pts[idx[j]][0];
    }
    float sgn = area < 0.0f ? -1.0f : 1.0f;
    int nt = 0, guard = 0;
    while (n > 3 && nt < maxTri && guard++ < 4 * 2 * BG_MAX_PV * 2 * BG_MAX_PV) {
        int found = -1;
        for (int i = 0; i < n && found < 0; ++i) {
            int a = idx[(i + n - 1) % n], b = idx[i], c = idx[(i + 1) % n];
            float ax = pts[a][0], az = pts[a][1], bx = pts[b][0], bz = pts[b][1], cx = pts[c][0], cz = pts[c][1];
            if (orient2(ax, az, bx, bz, cx, cz) * sgn <= 1e-6f) continue;          /* reflex */
            int clear = 1;
            for (int k = 0; k < n && clear; ++k) {
                int p = idx[k];
                float px = pts[p][0], pz = pts[p][1];
                if ((px == ax && pz == az) || (px == bx && pz == bz) || (px == cx && pz == cz)) continue;
                float o1 = orient2(ax, az, bx, bz, px, pz) * sgn, o2 = orient2(bx, bz, cx, cz, px, pz) * sgn,
                      o3 = orient2(cx, cz, ax, az, px, pz) * sgn;
                if (o1 > 0.0f && o2 > 0.0f && o3 > 0.0f) clear = 0;
            }
            if (clear) found = i;
        }
        if (found < 0) found = 0;   /* degenerate remainder: clip anyway rather than loop */
        tri[nt][0] = idx[(found + n - 1) % n]; tri[nt][1] = idx[found]; tri[nt][2] = idx[(found + 1) % n];
        nt++;
        for (int k = found; k < n - 1; ++k) idx[k] = idx[k + 1];
        n--;
    }
    if (n == 3 && nt < maxTri) { tri[nt][0] = idx[0]; tri[nt][1] = idx[1]; tri[nt][2] = idx[2]; nt++; }
    return nt;
}

/* ===========================================================================
 * Footprint grammar: a small family of base shapes, built from parameters so
 * setbacks, slices and variants are always regenerated rather than edited
 * ========================================================================= */

enum { FAM_RECT = 0, FAM_CHAMFER, FAM_L, FAM_T, FAM_U, FAM_CROSS, FAM_DRUM, FAM_COURT, FAM_COUNT };
static const char *FAM_NAMES[FAM_COUNT] = { "rectangle", "chamfered", "L", "T", "U", "cross", "drum", "courtyard" };

/* p1/p2 are proportions of w/d, so scaling a spec keeps its shape. quarter
 * turns the feature (notch, bar, opening) towards another side; w and d are
 * the extents after that turn. */
typedef struct FootSpec { int fam, quarter; float w, d, p1, p2, ang, cx, cz; } FootSpec;

static void make_foot(const FootSpec *s, BFoot *f) {
    memset(f, 0, sizeof(*f));       /* whole graphs compare byte for byte */
    BLoop *l = &f->outer;
    float w = (s->quarter & 1) ? s->d : s->w, d = (s->quarter & 1) ? s->w : s->d;
    float hw = w * 0.5f, hd = d * 0.5f;
    switch (s->fam) {
    case FAM_CHAMFER: {
        float c = s->p1 * minf(w, d);
        loop_add(l, -hw + c, -hd); loop_add(l, hw - c, -hd); loop_add(l, hw, -hd + c); loop_add(l, hw, hd - c);
        loop_add(l, hw - c, hd); loop_add(l, -hw + c, hd); loop_add(l, -hw, hd - c); loop_add(l, -hw, -hd + c);
        break;
    }
    case FAM_L: {
        float aw = s->p1 * w, ad = s->p2 * d;      /* notch at the +x,+z corner */
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, -hd + ad);
        loop_add(l, -hw + aw, -hd + ad); loop_add(l, -hw + aw, hd); loop_add(l, -hw, hd);
        break;
    }
    case FAM_T: {
        float st = s->p1 * w, bar = s->p2 * d;     /* bar along +z, stem towards -z */
        loop_add(l, -st * 0.5f, -hd); loop_add(l, st * 0.5f, -hd); loop_add(l, st * 0.5f, hd - bar);
        loop_add(l, hw, hd - bar); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        loop_add(l, -hw, hd - bar); loop_add(l, -st * 0.5f, hd - bar);
        break;
    }
    case FAM_U: {
        float arm = s->p1 * w, base = s->p2 * d;   /* opening towards +z */
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, hw - arm, hd);
        loop_add(l, hw - arm, -hd + base); loop_add(l, -hw + arm, -hd + base); loop_add(l, -hw + arm, hd); loop_add(l, -hw, hd);
        break;
    }
    case FAM_CROSS: {
        float ax = s->p1 * w * 0.5f, az = s->p2 * d * 0.5f;
        loop_add(l, -ax, -hd); loop_add(l, ax, -hd); loop_add(l, ax, -az); loop_add(l, hw, -az);
        loop_add(l, hw, az); loop_add(l, ax, az); loop_add(l, ax, hd); loop_add(l, -ax, hd);
        loop_add(l, -ax, az); loop_add(l, -hw, az); loop_add(l, -hw, -az); loop_add(l, -ax, -az);
        break;
    }
    case FAM_DRUM: {
        int n = (int)s->p1;
        n = n < 8 ? 8 : n > 24 ? 24 : n;
        for (int i = 0; i < n; ++i) {
            float a = 2.0f * PI_F * ((float)i + 0.5f) / (float)n;
            loop_add(l, hw * cosf(a), hd * sinf(a));
        }
        break;
    }
    case FAM_COURT: {
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        float iw = s->p1 * w * 0.5f, id = s->p2 * d * 0.5f;
        BLoop *h = &f->hole;
        loop_add(h, -iw, -id); loop_add(h, iw, -id); loop_add(h, iw, id); loop_add(h, -iw, id);
        break;
    }
    default:
        loop_add(l, -hw, -hd); loop_add(l, hw, -hd); loop_add(l, hw, hd); loop_add(l, -hw, hd);
        break;
    }
    float a = s->ang + (float)s->quarter * PI_F * 0.5f;
    float c = cosf(a), sn = sinf(a);
    BLoop *loops[2] = { &f->outer, &f->hole };
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < loops[k]->n; ++i) {
            float x = loops[k]->x[i], z = loops[k]->z[i];
            loops[k]->x[i] = s->cx + c * x - sn * z;
            loops[k]->z[i] = s->cz + sn * x + c * z;
        }
    loop_orient(&f->outer, 1);
    if (f->hole.n) loop_orient(&f->hole, 0);
}

static FootSpec spec_inset(FootSpec s, float m) { s.w -= 2.0f * m; s.d -= 2.0f * m; return s; }
static FootSpec spec_scale(FootSpec s, float k) { s.w *= k; s.d *= k; return s; }

/* Feature sizes have to make sense at building scale (arms and courts >= 8 m). */
static int spec_ok(const FootSpec *s) {
    float mn = minf(s->w, s->d);
    if (s->w < 8.0f || s->d < 8.0f) return 0;
    switch (s->fam) {
    case FAM_CHAMFER: return s->p1 * mn >= 1.5f;
    case FAM_L: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p1) * s->w >= 6.0f && (1.0f - s->p2) * s->d >= 6.0f;
    case FAM_T: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p2) * s->d >= 6.0f;
    case FAM_U: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - 2.0f * s->p1) * s->w >= 8.0f;
    case FAM_CROSS: return s->p1 * s->w >= 8.0f && s->p2 * s->d >= 8.0f && (1.0f - s->p1) * s->w >= 8.0f;
    case FAM_DRUM: return mn >= 14.0f;
    case FAM_COURT: return s->p1 * s->w >= 10.0f && s->p2 * s->d >= 10.0f &&
                           (1.0f - s->p1) * s->w * 0.5f >= 7.0f && (1.0f - s->p2) * s->d * 0.5f >= 7.0f;
    default: return 1;
    }
}

/* ===========================================================================
 * Building grammar
 * ========================================================================= */

typedef struct Ctx {
    const Building *b;
    BGraph *g;
    BLog   *log;
    float   W, D;          /* site extents */
    float   H;             /* target height above the sidewalk */
    int     N;             /* floors above the ground floor */
    int     frontage;
    Rng     ra, rf, rh, rs, rr, rd;   /* archetype, footprint, heights, surfaces, roofs, doors */
    uint8_t defWin, defPal, defBands, defFins, defLed;
} Ctx;

static float lvl(const Ctx *c, int k) { return G0 + c->g->groundH + (float)k * c->g->floorH; }

static int pick(Rng *r, const float *w, int n) {
    float t = 0.0f;
    for (int i = 0; i < n; ++i) t += w[i];
    if (t <= 0.0f) return 0;
    float x = rng_float(r) * t;
    for (int i = 0; i < n; ++i) { if (x < w[i]) return i; x -= w[i]; }
    return n - 1;
}

static int add_sec(Ctx *c, int type, float y0, float y1, const BFoot *f, int b0, int b1) {
    BGraph *g = c->g;
    if (g->nsec >= BG_MAX_SECT) return -1;
    BSection *s = &g->sec[g->nsec];
    memset(s, 0, sizeof(*s));
    s->type = type; s->y0 = y0; s->y1 = y1; s->foot = *f;
    if (b0 >= 0) s->below[s->nbelow++] = b0;
    if (b1 >= 0) s->below[s->nbelow++] = b1;
    s->arr = s->nbelow ? ARR_STACK : ARR_GROUND;
    s->win = c->defWin; s->palette = c->defPal; s->bands = c->defBands; s->fins = c->defFins; s->ledTop = c->defLed;
    return g->nsec++;
}

static void add_support(Ctx *c, int type, int sec, float x0, float y0, float z0, float x1, float y1, float z1, float size) {
    BGraph *g = c->g;
    if (g->nsup >= BG_MAX_SUPP) return;
    BSupport *s = &g->sup[g->nsup++];
    s->type = type; s->section = sec;
    s->x0 = x0; s->y0 = y0; s->z0 = z0; s->x1 = x1; s->y1 = y1; s->z1 = z1; s->size = size;
}

/* Columns under the outer edges of section `sec`, inset from its edge, on
 * every edge (mask < 0) or on the edges whose outward normals are in `mask`
 * (SIDE_* bits), skipping any that would stand inside something already
 * built in the storeys they pass through. */
static void columns_under(Ctx *c, int sec, float inset, float spacing, float y0, float size, int mask) {
    const BLoop *l = &c->g->sec[sec].foot.outer;
    float y1 = c->g->sec[sec].y0;
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
        if (len < 1.0f) continue;
        ex /= len; ez /= len;
        float nx = -ez, nz = ex;              /* outward */
        if (mask >= 0) {
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            if (!(mask & side)) continue;
        }
        int n = (int)ceilf(len / spacing);
        for (int j = 0; j < n; ++j) {
            float u = clampf(len * (float)j / (float)n, inset, len - inset);
            float x = l->x[k] + ex * u - nx * inset, z = l->z[k] + ez * u - nz * inset;
            if (!bfoot_contains(&c->g->sec[sec].foot, x, z)) continue;
            int blocked = 0;
            for (int t = 0; t < c->g->nsec && !blocked; ++t) {
                const BSection *o = &c->g->sec[t];
                if (t == sec || o->y0 >= y1 - 0.1f || o->y1 <= y0 + 0.1f) continue;
                blocked = bfoot_contains(&o->foot, x, z) || bfoot_boundary_dist(&o->foot, x, z, NULL, NULL) < size * 0.5f + 0.4f;
            }
            for (int t = 0; t < c->g->nsup && !blocked; ++t)
                blocked = c->g->sup[t].type == SUP_COLUMN && fabsf(c->g->sup[t].x0 - x) < size && fabsf(c->g->sup[t].z0 - z) < size;
            if (!blocked) add_support(c, SUP_COLUMN, sec, x, y0, z, x, y1, z, size);
        }
    }
}

static FootSpec site_spec(Ctx *c, int fam, float margin) {
    FootSpec s;
    memset(&s, 0, sizeof(s));
    s.fam = fam;
    s.w = c->W - 2.0f * margin;
    s.d = c->D - 2.0f * margin;
    Rng *r = &c->rf;
    switch (fam) {
    case FAM_CHAMFER: s.p1 = rng_range(r, 0.08f, 0.2f); break;
    case FAM_L:       s.p1 = rng_range(r, 0.4f, 0.6f); s.p2 = rng_range(r, 0.4f, 0.6f); break;
    case FAM_T:       s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    case FAM_U:       s.p1 = rng_range(r, 0.25f, 0.33f); s.p2 = rng_range(r, 0.3f, 0.45f); break;
    case FAM_CROSS:   s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    case FAM_DRUM:    s.p1 = minf(s.w, s.d) >= 30.0f ? 16.0f : 12.0f; break;
    case FAM_COURT:   s.p1 = rng_range(r, 0.35f, 0.5f); s.p2 = rng_range(r, 0.35f, 0.5f); break;
    default: break;
    }
    /* Turn notches and openings away from the street where possible. */
    if (fam == FAM_L || fam == FAM_T || fam == FAM_U) {
        static const int faceOfQuarter[4] = { SIDE_N, SIDE_W, SIDE_S, SIDE_E };
        int ok[4], nok = 0;
        for (int q = 0; q < 4; ++q) if (!(c->frontage & faceOfQuarter[q])) ok[nok++] = q;
        s.quarter = nok ? ok[rng_int(r, 0, nok - 1)] : rng_int(r, 0, 3);
    }
    return s;
}

/* Ground floor plus body up to floor nTop, optionally with an arcade: the
 * ground floor set back behind perimeter columns (on every side, or on the
 * street sides only). Returns the body (or the ground floor if nTop is 0). */
static int build_base(Ctx *c, const FootSpec *s, int nTop, int arcade, int streetOnly, int bodyType, int *groundOut) {
    BFoot body;
    make_foot(s, &body);
    float yg = lvl(c, 0);
    if (arcade && nTop > 0) {
        float sb = rng_range(&c->rf, 2.2f, 3.4f);
        FootSpec gs = *s;
        int mask = -1;
        if (streetOnly && s->fam == FAM_RECT && s->ang == 0.0f && !(s->quarter & 1)) {
            float x0 = s->cx - s->w * 0.5f, x1 = s->cx + s->w * 0.5f, z0 = s->cz - s->d * 0.5f, z1 = s->cz + s->d * 0.5f;
            if (c->frontage & SIDE_W) x0 += sb;
            if (c->frontage & SIDE_E) x1 -= sb;
            if (c->frontage & SIDE_S) z0 += sb;
            if (c->frontage & SIDE_N) z1 -= sb;
            gs.w = x1 - x0; gs.d = z1 - z0; gs.cx = (x0 + x1) * 0.5f; gs.cz = (z0 + z1) * 0.5f;
            mask = c->frontage;
        } else {
            gs = spec_inset(gs, sb);
        }
        if (spec_ok(&gs) && mask != 0) {
            BFoot gf;
            make_foot(&gs, &gf);
            int gi = add_sec(c, SEC_GROUND, G0, yg, &gf, -1, -1);
            int bi = add_sec(c, bodyType, yg, lvl(c, nTop), &body, gi, -1);
            if (bi < 0) return -1;
            c->g->sec[bi].arr = ARR_ARCADE;
            columns_under(c, bi, 0.7f, rng_range(&c->rf, 4.5f, 6.5f), G0, 0.6f, mask);
            blog_printf(c->log, "  arcade: ground floor set back %.1f m behind columns\n", sb);
            *groundOut = gi;
            return bi;
        }
    }
    int gi = add_sec(c, SEC_GROUND, G0, yg, &body, -1, -1);
    *groundOut = gi;
    if (nTop <= 0) return gi;
    return add_sec(c, bodyType, yg, lvl(c, nTop), &body, gi, -1);
}

/* A tower above section `on`: straight with setbacks, or a tapered, waisted,
 * flared or twisted stack of thin slices derived from the first. Returns the
 * top section. */
static int build_tower(Ctx *c, FootSpec ts, int on, int k0, int kTop, int allowProfile) {
    static const char *PROF[] = { "straight", "tapered", "waisted", "flared", "twisted" };
    float pw[5] = { 0.5f, 0.15f, 0.1f, 0.1f, 0.15f };
    int prof = allowProfile && kTop - k0 >= 10 ? pick(&c->rf, pw, 5) : 0;
    blog_printf(c->log, "  tower: %s %s %.0fx%.0f m, floors %d-%d\n", PROF[prof], FAM_NAMES[ts.fam], ts.w, ts.d, k0, kTop);
    int below = on;
    if (prof == 0) {
        int steps = rng_float(&c->rf) < 0.3f ? rng_int(&c->rf, 1, 2) : 0;
        int k = k0;
        FootSpec cur = ts;
        for (int i = 0; i <= steps; ++i) {
            int kEnd = i == steps ? kTop : k + (int)((float)(kTop - k) * rng_range(&c->rf, 0.55f, 0.8f));
            if (kEnd <= k) continue;
            BFoot f;
            make_foot(&cur, &f);
            int si = add_sec(c, i ? SEC_SETBACK : SEC_TOWER, lvl(c, k), lvl(c, kEnd), &f, below, -1);
            if (si < 0) break;
            below = si;
            k = kEnd;
            FootSpec nx = spec_inset(cur, rng_range(&c->rf, 2.0f, 4.0f));
            if (!spec_ok(&nx)) { c->g->sec[si].y1 = lvl(c, kTop); break; }
            cur = nx;
        }
        return below;
    }
    int S = (int)clampf((float)((kTop - k0) / 2), 4.0f, 10.0f);
    float twist = 0.0f;
    if (prof == 4) {
        twist = rng_range(&c->rf, 0.35f, 1.1f);
        if (rng_float(&c->rf) < 0.5f) twist = -twist;
    }
    for (int i = 0; i < S; ++i) {
        float t = S > 1 ? (float)i / (float)(S - 1) : 0.0f;
        float sc = prof == 1 ? 1.0f - 0.35f * t : prof == 2 ? 1.0f - 0.28f * sinf(PI_F * t) : prof == 3 ? 0.72f + 0.28f * t : 1.0f;
        FootSpec sp = spec_scale(ts, sc);
        sp.ang = ts.ang + twist * t;
        int ka = k0 + (kTop - k0) * i / S, kb = k0 + (kTop - k0) * (i + 1) / S;
        if (kb <= ka) continue;
        BFoot f;
        make_foot(&sp, &f);
        int si = add_sec(c, SEC_SLICE, lvl(c, ka), lvl(c, kb), &f, below, -1);
        if (si < 0) break;
        if (i > 0) c->g->sec[si].arr = ARR_STEP;
        below = si;
    }
    return below;
}

static int add_crown(Ctx *c, int top, FootSpec base, int kFrom, int kTo, int spire) {
    if (kTo <= kFrom) return top;
    FootSpec cs = spec_inset(base, rng_range(&c->rf, 1.5f, 3.0f));
    if (!spec_ok(&cs)) return top;
    BFoot f;
    make_foot(&cs, &f);
    int si = add_sec(c, SEC_CROWN, lvl(c, kFrom), lvl(c, kTo), &f, top, -1);
    if (si < 0) return top;
    if (spire) c->g->sec[si].spire = (uint8_t)rng_int(&c->rr, 10, 28);
    c->g->sec[si].ledTop = 1;
    return si;
}

static int arch_office(Ctx *c) {
    float fw[FAM_COUNT] = { 0.35f, 0.2f, 0.15f, 0.05f, 0.1f, 0.0f, 0.0f, 0.15f };
    int fam = pick(&c->rf, fw, FAM_COUNT);
    FootSpec s = site_spec(c, fam, rng_range(&c->rf, 0.0f, 1.2f));
    if (!spec_ok(&s)) s = site_spec(c, FAM_RECT, 0.5f);
    blog_printf(c->log, "  footprint: %s %.0fx%.0f m\n", FAM_NAMES[s.fam], s.w, s.d);
    int arcade = (s.fam == FAM_RECT || s.fam == FAM_CHAMFER) && rng_float(&c->rf) < 0.12f;
    int setback = c->N >= 6 && rng_float(&c->rf) < 0.35f;
    int kLow = setback ? (int)maxf(2.0f, (float)c->N * rng_range(&c->rf, 0.55f, 0.8f)) : c->N;
    int gi, body = build_base(c, &s, kLow, arcade, 0, SEC_BODY, &gi);
    if (body < 0) return 0;
    if (setback && kLow < c->N) {
        FootSpec u = spec_inset(s, rng_range(&c->rf, 2.5f, 5.0f));
        if (spec_ok(&u)) {
            BFoot f;
            make_foot(&u, &f);
            add_sec(c, SEC_SETBACK, lvl(c, kLow), lvl(c, c->N), &f, body, -1);
        } else c->g->sec[body].y1 = lvl(c, c->N);
    }
    return 1;
}

static int arch_skyscraper(Ctx *c) {
    int N = c->N;
    if (N < 8) return 0;
    int P = (int)clampf((float)N * rng_range(&c->rf, 0.08f, 0.18f), 1.0f, 5.0f);
    int fam = rng_float(&c->rf) < 0.65f ? FAM_RECT : FAM_CHAMFER;
    FootSpec ps = site_spec(c, fam, rng_range(&c->rf, 0.0f, 1.0f));
    if (!spec_ok(&ps)) ps = site_spec(c, FAM_RECT, 0.5f);
    int gi, pod = build_base(c, &ps, P, rng_float(&c->rf) < 0.15f, 0, SEC_PODIUM, &gi);
    if (pod < 0) return 0;
    if (rng_float(&c->rs) < 0.4f) {      /* a masonry podium under a glass tower */
        uint8_t stone[4] = { 5, 6, 0, 3 };
        uint8_t p = stone[rng_int(&c->rs, 0, 3)];
        for (int i = 0; i < c->g->nsec; ++i) { c->g->sec[i].palette = p; if (c->g->sec[i].win == WIN_CURTAIN) c->g->sec[i].win = WIN_PUNCHED; }
    }
    float tfw[FAM_COUNT] = { 0.3f, 0.25f, 0.0f, 0.0f, 0.0f, 0.15f, 0.15f, 0.0f };
    FootSpec ts = site_spec(c, pick(&c->rf, tfw, FAM_COUNT), 0.0f);
    float k = rng_range(&c->rf, 0.5f, 0.78f);
    ts.w = ps.w * k; ts.d = ps.d * rng_range(&c->rf, 0.5f, 0.78f);
    if (ts.fam == FAM_CROSS || ts.fam == FAM_DRUM) ts.w = ts.d = minf(ts.w, ts.d) * 1.1f;
    ts.cx = ps.cx; ts.cz = ps.cz;
    if (rng_float(&c->rf) < 0.3f) {       /* off-centre over the podium */
        ts.cx += (ps.w - ts.w) * 0.5f * rng_range(&c->rf, -0.8f, 0.8f);
        ts.cz += (ps.d - ts.d) * 0.5f * rng_range(&c->rf, -0.8f, 0.8f);
    }
    if (rng_float(&c->rf) < 0.2f) {       /* turned against its podium */
        ts.ang = rng_range(&c->rf, 0.17f, 0.7f);
        ts = spec_scale(ts, 0.8f);
    }
    if (!spec_ok(&ts)) return 0;
    int crown = rng_float(&c->rf) < 0.55f && N - P >= 12 ? rng_int(&c->rf, 2, 4) : 0;
    int top = build_tower(c, ts, pod, P, N - crown, 1);
    if (crown) {
        /* crown follows the shape the tower ended with */
        FootSpec last = ts;
        const BSection *t = &c->g->sec[top];
        float x0, z0, x1, z1;
        loop_bounds(&t->foot.outer, &x0, &z0, &x1, &z1);
        if (t->type == SEC_SLICE) { last.fam = FAM_RECT; last.w = (x1 - x0) * 0.8f; last.d = (z1 - z0) * 0.8f; last.ang = 0; last.cx = (x0 + x1) * 0.5f; last.cz = (z0 + z1) * 0.5f; last.quarter = 0; }
        top = add_crown(c, top, last, N - crown, N, rng_float(&c->rr) < 0.45f);
    }
    for (int i = 0; i < c->g->nsec; ++i) {
        BSection *s = &c->g->sec[i];
        if (s->type == SEC_TOWER || s->type == SEC_SLICE || s->type == SEC_SETBACK) {
            if (N >= 24 && rng_float(&c->rs) < 0.5f) s->plant = (uint8_t)rng_int(&c->rs, 1, 4);
        }
    }
    for (int i = 0; i < c->g->nsec; ++i)
        if (c->g->sec[i].type == SEC_TOWER || c->g->sec[i].type == SEC_SLICE) {
            c->g->sec[i].accent = rng_float(&c->rs) < 0.5f;
            break;
        }
    return 1;
}

static int arch_projection(Ctx *c) {
    int N = c->N;
    if (N < 4) return 0;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float p = rng_range(&c->rf, 2.0f, 7.0f);
    float sw[3] = { 0.45f, 0.3f, 0.25f };
    int sup = pick(&c->rf, sw, 3);       /* columns, struts or the core alone */
    int sideMask;
    static const int sides[4] = { SIDE_W, SIDE_E, SIDE_S, SIDE_N };
    int first = sides[rng_int(&c->rf, 0, 3)];
    for (int i = 0; i < 4; ++i) if (c->frontage & sides[i]) { first = sides[i]; break; }
    sideMask = first;
    if (rng_float(&c->rf) < 0.3f) sideMask |= (first & (SIDE_W | SIDE_E)) ? (rng_float(&c->rf) < 0.5f ? SIDE_S : SIDE_N)
                                                                           : (rng_float(&c->rf) < 0.5f ? SIDE_W : SIDE_E);
    float x0 = -c->W * 0.5f + m, x1 = c->W * 0.5f - m, z0 = -c->D * 0.5f + m, z1 = c->D * 0.5f - m;
    FootSpec blk;
    memset(&blk, 0, sizeof(blk));
    blk.fam = FAM_RECT; blk.w = x1 - x0; blk.d = z1 - z0;
    if (sideMask & SIDE_W) x0 += p;
    if (sideMask & SIDE_E) x1 -= p;
    if (sideMask & SIDE_S) z0 += p;
    if (sideMask & SIDE_N) z1 -= p;
    FootSpec bs = blk;
    bs.w = x1 - x0; bs.d = z1 - z0; bs.cx = (x0 + x1) * 0.5f; bs.cz = (z0 + z1) * 0.5f;
    if (!spec_ok(&bs)) return 0;
    int k0 = rng_int(&c->rf, 1, N - 3 < 4 ? N - 3 : 4);
    int k1 = k0 + rng_int(&c->rf, 2, 6);
    if (k1 > N) k1 = N;
    static const char *SUP[] = { "columns", "raked struts", "core" };
    blog_printf(c->log, "  projection: %.1f m on %s%s, %s, floors %d-%d\n", p,
         (sideMask & (SIDE_W | SIDE_E)) && (sideMask & (SIDE_S | SIDE_N)) ? "a corner" : "one side", "", SUP[sup], k0, k1);
    int gi, base = build_base(c, &bs, k0, 0, 0, SEC_BODY, &gi);
    if (base < 0) return 0;
    BFoot bf, kf;
    make_foot(&bs, &bf);
    make_foot(&blk, &kf);
    int pb = add_sec(c, SEC_PROJECTION, lvl(c, k0), lvl(c, k1), &kf, base, -1);
    if (pb < 0) return 0;
    c->g->sec[pb].arr = ARR_PROJECTION;
    c->g->sec[pb].support = sup == 0 ? SUP_COLUMN : sup == 1 ? SUP_STRUT : SUP_CORE;
    if (sup == 0) {
        columns_under(c, pb, 0.6f, 6.0f, G0, 0.7f, sideMask);
    } else if (sup == 1) {
        /* raked struts from the face of the base up to the projection's outer edge */
        const BLoop *l = &kf.outer;
        for (int k = 0; k < l->n; ++k) {
            int k1i = (k + 1) % l->n;
            float ex = l->x[k1i] - l->x[k], ez = l->z[k1i] - l->z[k], len = sqrtf(ex * ex + ez * ez);
            ex /= len; ez /= len;
            float nx = -ez, nz = ex;
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            if (!(sideMask & side)) continue;
            int n = (int)ceilf(len / 6.0f);
            for (int j = 0; j <= n; ++j) {
                float u = clampf(len * (float)j / (float)n, 1.0f, len - 1.0f);
                float tx = l->x[k] + ex * u - nx * 0.4f, tz = l->z[k] + ez * u - nz * 0.4f;
                float bx = tx - nx * (p - 0.4f), bz = tz - nz * (p - 0.4f);
                float by = maxf(lvl(c, k0) - p * 1.3f, lvl(c, 0));
                if (!bfoot_contains(&bf, bx - nx * 0.3f, bz - nz * 0.3f)) continue;
                add_support(c, SUP_STRUT, pb, bx, by, bz, tx, lvl(c, k0), tz, 0.45f);
            }
        }
    }
    if (k1 < N) {
        if (c->b->kind == BK_TOWER && N - k1 >= 8) {
            FootSpec ts = bs;
            ts = spec_scale(ts, rng_range(&c->rf, 0.6f, 0.85f));
            ts.fam = rng_float(&c->rf) < 0.5f ? FAM_RECT : FAM_CHAMFER;
            ts.p1 = rng_range(&c->rf, 0.08f, 0.2f);
            if (spec_ok(&ts)) build_tower(c, ts, pb, k1, N, 0);
        } else {
            add_sec(c, SEC_BODY, lvl(c, k1), lvl(c, N), &bf, pb, -1);
        }
    }
    return 1;
}

static int arch_twin(Ctx *c) {
    int alongX = c->W >= c->D;
    float L = alongX ? c->W : c->D, S = alongX ? c->D : c->W;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float gap = rng_range(&c->rf, 9.0f, 14.0f);
    float tl = (L - 2.0f * m - gap) * 0.5f * rng_range(&c->rf, 0.85f, 1.0f);
    float tsd = (S - 2.0f * m) * rng_range(&c->rf, 0.6f, 0.9f);
    if (tl < 10.0f || tsd < 10.0f || c->N < 12) return 0;
    int P = rng_int(&c->rf, 1, 3);
    FootSpec ps = site_spec(c, FAM_RECT, m);
    int gi, pod = build_base(c, &ps, P, rng_float(&c->rf) < 0.15f, 0, SEC_PODIUM, &gi);
    if (pod < 0) return 0;
    float fw[FAM_COUNT] = { 0.4f, 0.35f, 0, 0, 0, 0, 0.25f, 0 };
    FootSpec ts = site_spec(c, pick(&c->rf, fw, FAM_COUNT), 0.0f);
    ts.w = alongX ? tl : tsd; ts.d = alongX ? tsd : tl;
    if (ts.fam == FAM_DRUM && minf(ts.w, ts.d) < 14.0f) ts.fam = FAM_RECT;
    float off = gap * 0.5f + tl * 0.5f;
    int crown = rng_float(&c->rf) < 0.5f ? rng_int(&c->rf, 2, 3) : 0;
    int NB = c->N - (rng_float(&c->rf) < 0.5f ? rng_int(&c->rf, 0, c->N / 8) : 0);
    int top[2];
    for (int t = 0; t < 2; ++t) {
        FootSpec tt = ts;
        if (alongX) tt.cx = t ? off : -off; else tt.cz = t ? off : -off;
        int nt = (t ? NB : c->N) - crown;
        top[t] = build_tower(c, tt, pod, P, nt, 0);
        if (crown) top[t] = add_crown(c, top[t], tt, nt, nt + crown, rng_float(&c->rr) < 0.4f);
    }
    /* sky bridge between the two shafts, bearing on both */
    int lowTop = NB < c->N ? NB : c->N;
    int kb = P + (int)((float)(lowTop - crown - P) * rng_range(&c->rf, 0.45f, 0.7f));
    float bw = minf(tsd * 0.5f, rng_range(&c->rf, 6.0f, 9.0f));
    FootSpec br;
    memset(&br, 0, sizeof(br));
    br.fam = FAM_RECT;
    float blen = gap + tl * 0.4f;
    br.w = alongX ? blen : bw; br.d = alongX ? bw : blen;
    BFoot bf;
    make_foot(&br, &bf);
    /* the tower sections the bridge rests on */
    int ta = -1, tb = -1;
    for (int i = 0; i < c->g->nsec; ++i) {
        const BSection *s = &c->g->sec[i];
        if (s->y0 > lvl(c, kb) + 0.01f || s->y1 < lvl(c, kb + 2) - 0.01f || i == pod || i == gi) continue;
        float x0, z0, x1, z1;
        loop_bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
        float cpos = alongX ? (x0 + x1) * 0.5f : (z0 + z1) * 0.5f;
        if (cpos < 0.0f && ta < 0) ta = i;
        if (cpos > 0.0f && tb < 0) tb = i;
    }
    if (ta >= 0 && tb >= 0) {
        int bi = add_sec(c, SEC_BRIDGE, lvl(c, kb), lvl(c, kb + 2), &bf, ta, tb);
        if (bi >= 0) {
            c->g->sec[bi].arr = ARR_BRIDGE;
            c->g->sec[bi].win = WIN_CURTAIN;
            c->g->sec[bi].ledTop = 1;
            c->g->sec[bi].fins = 0;
            blog_printf(c->log, "  sky bridge: floors %d-%d, clear span %.1f m\n", kb, kb + 2, gap);
        }
    }
    return 1;
}

static int arch_gateway(Ctx *c) {
    int alongX = c->W >= c->D;
    float L = alongX ? c->W : c->D, S = alongX ? c->D : c->W;
    float m = rng_range(&c->rf, 0.0f, 0.8f);
    float pw = L * rng_range(&c->rf, 0.2f, 0.3f);
    int ks = rng_int(&c->rf, 2, 4);
    int ke = ks + rng_int(&c->rf, 3, 7);
    if (ke > c->N) ke = c->N;
    if (ke <= ks || pw < 8.0f || S - 2.0f * m < 10.0f) return 0;
    blog_printf(c->log, "  gateway: piers %.0f m wide, clear span %.1f m, span floors %d-%d\n", pw, L - 2.0f * m - 2.0f * pw, ks, ke);
    int pier[2];
    for (int t = 0; t < 2; ++t) {
        FootSpec s;
        memset(&s, 0, sizeof(s));
        s.fam = FAM_RECT;
        float cpos = (t ? 1.0f : -1.0f) * (L * 0.5f - m - pw * 0.5f);
        s.w = alongX ? pw : S - 2.0f * m; s.d = alongX ? S - 2.0f * m : pw;
        if (alongX) s.cx = cpos; else s.cz = cpos;
        BFoot f;
        make_foot(&s, &f);
        pier[t] = add_sec(c, SEC_PIER, G0, lvl(c, ks), &f, -1, -1);
        if (pier[t] < 0) return 0;
    }
    FootSpec sp = site_spec(c, FAM_RECT, m + 0.3f);
    BFoot f;
    make_foot(&sp, &f);
    int span = add_sec(c, SEC_SPAN, lvl(c, ks), lvl(c, ke), &f, pier[0], pier[1]);
    if (span < 0) return 0;
    c->g->sec[span].arr = ARR_SPAN;
    /* transfer structure: a deep girder across the passage */
    float in = L * 0.5f - m - pw, y1 = lvl(c, ks), y0 = y1 - 1.6f, hs = (S - 2.0f * m) * 0.5f - 0.3f;
    if (alongX) add_support(c, SUP_TRANSFER, span, -in - 0.5f, y0, -hs, in + 0.5f, y1, hs, 0.0f);
    else        add_support(c, SUP_TRANSFER, span, -hs, y0, -in - 0.5f, hs, y1, in + 0.5f, 0.0f);
    if (c->b->kind == BK_TOWER && c->N - ke >= 8) {
        FootSpec ts = spec_scale(sp, rng_range(&c->rf, 0.5f, 0.75f));
        ts.fam = FAM_CHAMFER;
        ts.p1 = rng_range(&c->rf, 0.08f, 0.2f);
        if (spec_ok(&ts)) build_tower(c, ts, span, ke, c->N, 1);
    }
    return 1;
}

static int arch_arcology(Ctx *c) {
    FootSpec s = site_spec(c, rng_float(&c->rf) < 0.6f ? FAM_RECT : FAM_CHAMFER, 1.0f);
    if (!spec_ok(&s)) return 0;
    int steps = rng_int(&c->rf, 6, 9), below = -1, k = 0;
    float remaining = (float)c->N;
    for (int i = 0; i < steps; ++i) {
        int kEnd = i == steps - 1 ? c->N : k + (int)maxf(1.0f, remaining * rng_range(&c->rf, 0.16f, 0.26f));
        if (kEnd > c->N) kEnd = c->N;
        BFoot f;
        make_foot(&s, &f);
        int si = add_sec(c, i ? SEC_SETBACK : SEC_GROUND, i ? lvl(c, k) : G0, lvl(c, kEnd), &f, below, -1);
        if (si < 0) break;
        below = si;
        remaining -= (float)(kEnd - k);
        k = kEnd;
        if (k >= c->N) break;
        FootSpec nx = spec_inset(s, rng_range(&c->rf, 4.0f, 8.0f));
        if (nx.w < 14.0f || nx.d < 14.0f) { c->g->sec[si].y1 = lvl(c, c->N); break; }
        s = nx;
    }
    if (below >= 0) c->g->sec[below].spire = (uint8_t)rng_int(&c->rr, 30, 60);
    return below >= 0;
}

static int arch_shophouse(Ctx *c) {
    FootSpec s = site_spec(c, FAM_RECT, 0.0f);
    int gi, body = build_base(c, &s, c->N, rng_float(&c->rf) < 0.55f, 1, SEC_BODY, &gi);
    return body >= 0;
}

/* Housing slab or point block, optionally lifted on a void deck: an open
 * ground floor of lift cores and columns under the block. */
static int arch_housing(Ctx *c, int point) {
    FootSpec s;
    if (point) {
        float fw[FAM_COUNT] = { 0.15f, 0.3f, 0, 0, 0, 0.45f, 0.1f, 0 };
        float m = rng_range(&c->rf, 0.0f, 1.0f);
        s = site_spec(c, pick(&c->rf, fw, FAM_COUNT), m);
        s.w = s.d = minf(s.w, s.d);
    } else {
        s = site_spec(c, rng_float(&c->rf) < 0.7f ? FAM_RECT : FAM_CHAMFER, 0.0f);
        if (s.fam == FAM_CHAMFER) s.p1 = rng_range(&c->rf, 0.05f, 0.1f);
    }
    if (!spec_ok(&s)) s = site_spec(c, FAM_RECT, 0.0f);
    int crown = point && rng_float(&c->rf) < 0.5f && c->N >= 12 ? 2 : 0;
    if (c->N - crown < 1) return 0;
    int voidDeck = rng_float(&c->rf) < (point ? 0.3f : 0.4f);
    int top;
    if (voidDeck) {
        c->g->groundMode = GF_VOID;
        BFoot body;
        make_foot(&s, &body);
        float yg = lvl(c, 0);
        int cores[2] = { -1, -1 }, nc = 0;
        int alongX = s.w >= s.d;
        float L = alongX ? s.w : s.d, S = alongX ? s.d : s.w;
        int want = !point && L >= 40.0f ? 2 : 1;
        for (int i = 0; i < want; ++i) {
            FootSpec cs;
            memset(&cs, 0, sizeof(cs));
            cs.fam = FAM_RECT;
            float cl = minf(8.0f, L * 0.3f), cw = minf(S - 4.0f, 8.0f);
            cs.w = alongX ? cl : cw; cs.d = alongX ? cw : cl;
            float cpos = want == 1 ? 0.0f : (i ? 1.0f : -1.0f) * L * 0.25f;
            cs.cx = s.cx + (alongX ? cpos : 0.0f); cs.cz = s.cz + (alongX ? 0.0f : cpos);
            BFoot cf;
            make_foot(&cs, &cf);
            cores[nc++] = add_sec(c, SEC_CORE, G0, yg, &cf, -1, -1);
        }
        top = add_sec(c, SEC_BODY, yg, lvl(c, c->N - crown), &body, cores[0], cores[1]);
        if (top < 0) return 0;
        c->g->sec[top].arr = ARR_ARCADE;
        columns_under(c, top, 0.8f, 6.0f, G0, 0.7f, -1);
        blog_printf(c->log, "  void deck: %d lift core(s) and columns\n", nc);
    } else {
        int gi;
        top = build_base(c, &s, c->N - crown, 0, 0, SEC_BODY, &gi);
        if (top < 0) return 0;
    }
    if (crown) {
        top = add_crown(c, top, s, c->N - crown, c->N, 0);
        c->g->sec[top].roof = ROOF_TANKS;
    }
    return 1;
}

static int arch_shed(Ctx *c) {
    FootSpec s = site_spec(c, FAM_RECT, rng_range(&c->rf, 0.0f, 1.0f));
    if (!spec_ok(&s)) return 0;
    int gi, body = build_base(c, &s, c->N, 0, 0, SEC_BODY, &gi);
    return body >= 0;
}

/* ===========================================================================
 * Districts: biases over the choices, never relaxations of a rule
 * ========================================================================= */

static int district_of(const Building *b) {
    switch (b->zone) {
    case ZONE_CORE:       return b->density >= 0.75f ? DIST_DOWNTOWN : DIST_MIDRISE;
    case ZONE_CENTRE:     return DIST_CENTRE;
    case ZONE_INDUSTRIAL: return DIST_INDUSTRIAL;
    default:              return DIST_ESTATE;
    }
}

static int choose_archetype(Ctx *c, Rng *r) {
    const Building *b = c->b;
    if (b->kind == BK_MEGA) return ARCH_ARCOLOGY;
    float w[ARCH_COUNT] = { 0 };
    float L = maxf(c->W, c->D), S = minf(c->W, c->D);
    int twinOk = L >= 42.0f && S >= 18.0f, gateOk = L >= 40.0f && S >= 16.0f;
    int tower = b->kind == BK_TOWER;
    switch (c->g->district) {
    case DIST_DOWNTOWN:
        if (tower) { w[ARCH_SKYSCRAPER] = 0.5f; w[ARCH_TWIN] = twinOk ? 0.18f : 0; w[ARCH_PROJECTION] = 0.14f; w[ARCH_GATEWAY] = gateOk ? 0.08f : 0; }
        else       { w[ARCH_OFFICE] = 0.6f; w[ARCH_PROJECTION] = 0.25f; w[ARCH_SHOPHOUSE] = 0.15f; }
        break;
    case DIST_MIDRISE:
        if (tower) { w[ARCH_SKYSCRAPER] = 0.55f; w[ARCH_PROJECTION] = 0.25f; w[ARCH_TWIN] = twinOk ? 0.06f : 0; w[ARCH_GATEWAY] = gateOk ? 0.04f : 0; }
        else       { w[ARCH_OFFICE] = 0.6f; w[ARCH_PROJECTION] = 0.15f; w[ARCH_SHOPHOUSE] = 0.25f; }
        break;
    case DIST_CENTRE:
        if (tower) w[ARCH_SKYSCRAPER] = 1.0f;
        else       { w[ARCH_SHOPHOUSE] = 0.8f; w[ARCH_OFFICE] = 0.2f; }
        break;
    case DIST_INDUSTRIAL:
        w[ARCH_SHED] = 0.9f; w[ARCH_OFFICE] = 0.1f;
        break;
    default:
        w[tower ? ARCH_POINT : ARCH_SLAB] = 1.0f;
        break;
    }
    return pick(r, w, ARCH_COUNT);
}

/* Palettes (shader wallAlbedo): 0 concrete, 1 brick, 2 teal metal, 3 pale tile,
 * 4 black glass, 5 warm stone, 6 bronze, 7 blue glass. */
static void choose_surfaces(Ctx *c) {
    static const float PAL[DIST_COUNT][8] = {
        { 0.10f, 0.00f, 0.20f, 0.00f, 0.30f, 0.00f, 0.10f, 0.30f },   /* downtown: glass towers */
        { 0.20f, 0.10f, 0.05f, 0.15f, 0.05f, 0.25f, 0.20f, 0.00f },   /* midrise: stone and bronze */
        { 0.15f, 0.25f, 0.10f, 0.30f, 0.00f, 0.20f, 0.00f, 0.00f },   /* centre */
        { 0.30f, 0.00f, 0.00f, 0.50f, 0.00f, 0.20f, 0.00f, 0.00f },   /* estate */
        { 0.50f, 0.20f, 0.30f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f },   /* industrial */
    };
    int d = c->g->district;
    c->defPal = (uint8_t)pick(&c->rs, PAL[d], 8);
    int glass = c->defPal == 4 || c->defPal == 7;
    float ww[4];
    if (glass)                       { ww[0] = 0.0f;  ww[1] = 0.6f;  ww[2] = 0.4f;  ww[3] = 0.0f; }
    else if (d == DIST_ESTATE)       { ww[0] = 0.7f;  ww[1] = 0.0f;  ww[2] = 0.0f;  ww[3] = 0.3f; }
    else if (d == DIST_INDUSTRIAL)   { ww[0] = 0.2f;  ww[1] = 0.0f;  ww[2] = 0.8f;  ww[3] = 0.0f; }
    else                             { ww[0] = 0.5f;  ww[1] = 0.1f;  ww[2] = 0.25f; ww[3] = 0.15f; }
    c->defWin = (uint8_t)pick(&c->rs, ww, 4);
    c->defBands = c->defWin != WIN_CURTAIN && rng_float(&c->rs) < 0.3f;
    c->defFins = (d == DIST_DOWNTOWN || d == DIST_MIDRISE) && c->b->kind == BK_TOWER && rng_float(&c->rs) < 0.25f;
    c->defLed = glass ? rng_float(&c->rs) < 0.5f : rng_float(&c->rs) < 0.1f;
}

static int choose_ground(Ctx *c) {
    float w[4] = { 1, 0, 0, 0 };
    switch (c->g->archetype) {
    case ARCH_ARCOLOGY: return GF_DOUBLE;
    case ARCH_SHOPHOUSE: case ARCH_SHED: case ARCH_SLAB: case ARCH_POINT: return GF_RETAIL;
    default:
        if (c->g->district == DIST_DOWNTOWN) { w[0] = 0.3f; w[1] = 0.35f; w[2] = 0.35f; }
        else if (c->g->district == DIST_MIDRISE) { w[0] = 0.5f; w[1] = 0.3f; w[2] = 0.2f; }
        else { w[0] = 0.8f; w[1] = 0.2f; }
    }
    int m = pick(&c->rs, w, 4);
    if (m == GF_DOUBLE && c->H < 60.0f) m = GF_LOBBY;
    return m;
}

/* ===========================================================================
 * Roofs, doors
 * ========================================================================= */

static int section_has_above(const BGraph *g, int s) {
    for (int i = 0; i < g->nsec; ++i)
        for (int k = 0; k < g->sec[i].nbelow; ++k)
            if (g->sec[i].below[k] == s) return 1;
    return 0;
}

static void choose_roofs(Ctx *c) {
    BGraph *g = c->g;
    for (int i = 0; i < g->nsec; ++i) {
        BSection *s = &g->sec[i];
        if (s->roof) continue;
        int convex = !s->foot.hole.n && loop_convex(&s->foot.outer);
        int rect = convex && s->foot.outer.n == 4;
        float x0, z0, x1, z1;
        loop_bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
        float mn = minf(x1 - x0, z1 - z0);
        if (section_has_above(g, i)) {
            float w[8] = { 0.2f, 0.5f, 0, 0.3f, 0, 0, 0, 0 };
            s->roof = (uint8_t)pick(&c->rr, w, 8);
            continue;
        }
        float w[8] = { 0.1f, 0.4f, 0, 0.1f, 0.15f, 0.1f, 0, 0 };
        if (s->y1 - G0 > 60.0f && mn >= 22.0f && convex) w[ROOF_HELIPAD] = 0.25f;
        if (rect && s->y1 - G0 < 30.0f) w[ROOF_HIP] = g->archetype == ARCH_SHOPHOUSE ? 0.35f : 0.1f;
        if (convex && mn >= 14.0f && g->archetype != ARCH_SHED) w[ROOF_PYRAMID] = 0.06f;
        if (g->district == DIST_ESTATE) { w[ROOF_TANKS] = 0.6f; w[ROOF_GARDEN] = 0.0f; w[ROOF_SOLAR] = 0.05f; }
        if (g->district == DIST_INDUSTRIAL) { w[ROOF_SOLAR] = 0.4f; w[ROOF_GARDEN] = 0.0f; }
        s->roof = (uint8_t)pick(&c->rr, w, 8);
    }
}

static int face_side(float nx, float nz) {
    return fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
}

/* Whether the face of a ground-level section looks onto the street: facing a
 * frontage side and standing near that edge of the plot (not in a notch). */
static int face_on_street(const Ctx *c, const BLoop *l, int k) {
    int k1 = (k + 1) % l->n;
    float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
    if (len < 1e-3f) return 0;
    float nx = -ez / len, nz = ex / len;
    int side = face_side(nx, nz);
    if (!(c->frontage & side)) return 0;
    float mx = (l->x[k] + l->x[k1]) * 0.5f, mz = (l->z[k] + l->z[k1]) * 0.5f;
    float gap = side == SIDE_W ? mx + c->W * 0.5f : side == SIDE_E ? c->W * 0.5f - mx :
                side == SIDE_S ? mz + c->D * 0.5f : c->D * 0.5f - mz;
    return gap < 4.5f && fabsf(side == SIDE_W || side == SIDE_E ? nx : nz) > 0.6f;
}

/* Gap between columns standing in front of this face that can take a door. */
static float door_u(const BGraph *g, const BLoop *l, int k, float width, float want) {
    int k1 = (k + 1) % l->n;
    float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
    ex /= len; ez /= len;
    float nx = -ez, nz = ex;
    float us[BG_MAX_SUPP + 2];
    int n = 0;
    us[n++] = 0.0f;
    for (int i = 0; i < g->nsup; ++i) {
        const BSupport *s = &g->sup[i];
        if (s->type != SUP_COLUMN || s->y0 > G0 + 0.1f) continue;
        float dx = s->x0 - l->x[k], dz = s->z0 - l->z[k];
        float out = dx * nx + dz * nz, u = dx * ex + dz * ez;
        if (out < -0.5f || out > 6.0f || u < -1.0f || u > len + 1.0f) continue;
        us[n++] = u;
    }
    us[n++] = len;
    for (int i = 0; i < n; ++i)                /* insertion sort, n is small */
        for (int j = i; j > 0 && us[j] < us[j - 1]; --j) { float t = us[j]; us[j] = us[j - 1]; us[j - 1] = t; }
    if (n == 2) return want;
    float best = -1.0f, bd = 1e30f;
    for (int i = 0; i + 1 < n; ++i) {
        float a = us[i] + (i ? 0.6f : 0.6f), b = us[i + 1] - 0.6f;
        if (b - a < width + 0.4f) continue;
        float cu = (a + b) * 0.5f;
        if (fabsf(cu - want) < bd) { bd = fabsf(cu - want); best = cu; }
    }
    return best;
}

static void add_door(Ctx *c, int type, int sec, int face, float u, float w, float h) {
    BGraph *g = c->g;
    if (g->ndoor >= BG_MAX_DOOR) return;
    BDoor *d = &g->door[g->ndoor++];
    d->type = type; d->section = sec; d->face = face; d->u = u; d->width = w; d->height = h;
    /* a canopy over a primary entrance unless something already covers it */
    const BLoop *l = &g->sec[sec].foot.outer;
    int k1 = (face + 1) % l->n;
    float ex = l->x[k1] - l->x[face], ez = l->z[k1] - l->z[face], len = sqrtf(ex * ex + ez * ez);
    float px = l->x[face] + ex / len * u - ez / len * 1.2f, pz = l->z[face] + ez / len * u + ex / len * 1.2f;
    int covered = 0;
    for (int i = 0; i < g->nsec && !covered; ++i)
        if (i != sec && g->sec[i].y0 < G0 + g->groundH + 8.0f && g->sec[i].y0 > G0 + 0.5f && bfoot_contains(&g->sec[i].foot, px, pz)) covered = 1;
    d->canopy = type == DOOR_PRIMARY && !covered;
}

static void place_doors(Ctx *c) {
    BGraph *g = c->g;
    int ns = 0, cs[64], cf[64];
    float cw[64];
    for (int i = 0; i < g->nsec; ++i) {
        const BSection *s = &g->sec[i];
        if (s->y0 > G0 + 0.01f) continue;
        for (int k = 0; k < s->foot.outer.n && ns < 64; ++k) {
            const BLoop *l = &s->foot.outer;
            int k1 = (k + 1) % l->n;
            float len = sqrtf((l->x[k1] - l->x[k]) * (l->x[k1] - l->x[k]) + (l->z[k1] - l->z[k]) * (l->z[k1] - l->z[k]));
            if (len < 3.4f) continue;
            cs[ns] = i; cf[ns] = k;
            cw[ns] = len * (face_on_street(c, l, k) ? 4.0f : 0.5f);
            ns++;
        }
    }
    if (!ns) return;
    int pi = pick(&c->rd, cw, ns);
    {
        const BLoop *l = &g->sec[cs[pi]].foot.outer;
        int k = cf[pi], k1 = (k + 1) % l->n;
        float len = sqrtf((l->x[k1] - l->x[k]) * (l->x[k1] - l->x[k]) + (l->z[k1] - l->z[k]) * (l->z[k1] - l->z[k]));
        float w = g->groundMode == GF_DOUBLE ? rng_range(&c->rd, 3.6f, 5.0f) : rng_range(&c->rd, 2.4f, 3.6f);
        w = clampf(w, 1.8f, len - 1.6f);
        float want = len * 0.5f + rng_range(&c->rd, -0.25f, 0.25f) * maxf(0.0f, len - w - 1.6f);
        float u = door_u(g, l, k, w, want);
        if (u < 0.0f) { w = 1.8f; u = door_u(g, l, k, w, want); }
        if (u >= 0.0f) add_door(c, DOOR_PRIMARY, cs[pi], k, u, w, g->groundMode == GF_DOUBLE ? 3.8f : 2.7f);
    }
    /* service doors and loading bays on the back */
    int nService = g->archetype == ARCH_SHED ? rng_int(&c->rd, 1, 3) :
                   (g->archetype == ARCH_SHOPHOUSE || g->archetype == ARCH_SLAB || g->archetype == ARCH_POINT) ? 0 : 1;
    for (int n = 0; n < nService; ++n) {
        float bw[64];
        int any = 0;
        for (int i = 0; i < ns; ++i) {
            const BLoop *l = &g->sec[cs[i]].foot.outer;
            bw[i] = face_on_street(c, l, cf[i]) ? 0.2f : 1.0f;
            for (int d = 0; d < g->ndoor; ++d) if (g->door[d].section == cs[i] && g->door[d].face == cf[i]) bw[i] = 0.0f;
            if (bw[i] > 0.0f) any = 1;
        }
        if (!any) break;
        int si = pick(&c->rd, bw, ns);
        const BLoop *l = &g->sec[cs[si]].foot.outer;
        int k = cf[si], k1 = (k + 1) % l->n;
        float len = sqrtf((l->x[k1] - l->x[k]) * (l->x[k1] - l->x[k]) + (l->z[k1] - l->z[k]) * (l->z[k1] - l->z[k]));
        int loading = g->archetype == ARCH_SHED;
        float w = loading ? minf(rng_range(&c->rd, 4.0f, 5.0f), len - 2.4f) : 1.3f;
        if (w < (loading ? 3.0f : 1.0f) || len < w + 2.4f) continue;
        float u = door_u(g, l, k, w, rng_range(&c->rd, 0.6f + w * 0.5f, len - 0.6f - w * 0.5f));
        if (u >= 0.0f) add_door(c, loading ? DOOR_LOADING : DOOR_SERVICE, cs[si], k, u, w, loading ? 4.4f : 2.3f);
    }
}

/* ===========================================================================
 * Validation
 * ========================================================================= */

typedef struct Overhang { float max, outsideLen, perimeter; int corner, nOut; } Overhang;

/* How far a section's outline reaches beyond everything that holds it up. */
static Overhang overhang(const BGraph *g, const BSection *s, const BSupport *col, int ncol, float *colReach) {
    Overhang o;
    memset(&o, 0, sizeof(o));
    const BLoop *l = &s->foot.outer;
    o.perimeter = loop_perimeter(l);
    float reach = 0.0f;
    int prevOut = 0, firstOut = 0;
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
        int steps = (int)ceilf(len / 1.0f);
        if (steps < 1) steps = 1;
        int edgeOut = 0;
        for (int j = 0; j < steps; ++j) {
            float t = (float)j / (float)steps;
            float x = l->x[k] + ex * t, z = l->z[k] + ez * t;
            int in = 0;
            float d = 1e30f;
            for (int b = 0; b < s->nbelow && !in; ++b) {
                const BFoot *f = &g->sec[s->below[b]].foot;
                float bd = bfoot_boundary_dist(f, x, z, NULL, NULL);
                if (bfoot_contains(f, x, z) || bd < 0.05f) in = 1;
                else if (bd < d) d = bd;
            }
            if (in) continue;
            o.nOut++;
            o.outsideLen += len / (float)steps;
            if (d > o.max) o.max = d;
            if (j == 0 && d > 0.5f && prevOut) o.corner = 1;
            if (j == 0 && k == 0) firstOut = 1;
            edgeOut = 1;
            if (ncol) {
                float cd = 1e30f;
                for (int i = 0; i < ncol; ++i) cd = minf(cd, sqrtf((col[i].x1 - x) * (col[i].x1 - x) + (col[i].z1 - z) * (col[i].z1 - z)));
                reach = maxf(reach, minf(cd, d));
            }
        }
        prevOut = edgeOut;
        (void)firstOut;
    }
    if (colReach) *colReach = reach;
    return o;
}

static float supporter_depth(const BGraph *g, const BSection *s) {
    float m = 1e30f;
    for (int b = 0; b < s->nbelow; ++b) {
        float x0, z0, x1, z1;
        loop_bounds(&g->sec[s->below[b]].foot.outer, &x0, &z0, &x1, &z1);
        m = minf(m, minf(x1 - x0, z1 - z0));
    }
    return m;
}

int bgen_validate(const BGraph *g, const Building *b, BLog *log) {
    const float tol = 0.02f;
    int ok = 1;
#define REJECT(...) do { blog_printf(log, "  rejected: " __VA_ARGS__); blog_printf(log, "\n"); ok = 0; } while (0)
    if (g->nsec == 0) { REJECT("no sections"); return 0; }
    float top = 0.0f;
    for (int i = 0; i < g->nsec && ok; ++i) {
        const BSection *s = &g->sec[i];
        const BLoop *l = &s->foot.outer;
        top = maxf(top, s->y1);
        if (l->n < 3 || !loop_simple(l) || loop_min_edge(l) < 0.9f || bloop_area(l) > -20.0f) { REJECT("section %d: invalid footprint", i); break; }
        if (s->foot.hole.n) {
            const BLoop *h = &s->foot.hole;
            if (!loop_simple(h) || bloop_area(h) <= 0.0f) { REJECT("section %d: invalid courtyard", i); break; }
            for (int k = 0; k < h->n && ok; ++k)
                if (!bloop_inside(l, h->x[k], h->z[k]) || sqrtf(loop_dist2(l, h->x[k], h->z[k], NULL, NULL, 1e30f)) < 6.0f)
                    REJECT("section %d: courtyard leaves too thin a floor plate", i);
        }
        for (int k = 0; k < l->n && ok; ++k)
            if (l->x[k] < b->foot.x0 - tol || l->x[k] > b->foot.x1 + tol || l->z[k] < b->foot.z0 - tol || l->z[k] > b->foot.z1 + tol)
                REJECT("section %d: outside its plot", i);
        if (!ok) break;
        if (s->y1 < s->y0 + 1.0f) { REJECT("section %d: too thin", i); break; }

        /* support */
        if (s->nbelow == 0) {
            if (s->y0 > G0 + 0.01f) REJECT("section %d: floating %.1f m above the ground", i, s->y0 - G0);
            continue;
        }
        for (int k = 0; k < s->nbelow; ++k)
            if (g->sec[s->below[k]].y0 >= s->y0 - 0.01f) { REJECT("section %d: supporter %d is not below it", i, s->below[k]); break; }
        if (!ok) break;
        BSupport cols[BG_MAX_SUPP];
        int ncol = 0, nstrut = 0, transfer = 0;
        for (int k = 0; k < g->nsup; ++k) {
            if (g->sup[k].section != i) continue;
            if (g->sup[k].type == SUP_COLUMN) cols[ncol++] = g->sup[k];
            else if (g->sup[k].type == SUP_STRUT) nstrut++;
            else if (g->sup[k].type == SUP_TRANSFER) transfer++;
        }
        float reach = 0.0f;
        Overhang o = overhang(g, s, cols, ncol, &reach);
        float depth = supporter_depth(g, s);
        switch (s->arr) {
        case ARR_STACK:
            if (o.max > 0.05f) REJECT("section %d: unsupported overhang of %.1f m on a plain stack", i, o.max);
            break;
        case ARR_ARCADE:
            if (!ncol) REJECT("section %d: arcade without columns", i);
            else if (g->sec[s->below[0]].type != SEC_CORE && o.max > 5.0f) REJECT("section %d: arcade setback %.1f m > 5", i, o.max);
            else if (reach > 4.6f) REJECT("section %d: %.1f m from the nearest column", i, reach);
            break;
        case ARR_PROJECTION: {
            float lim = s->support == SUP_COLUMN ? 6.5f : s->support == SUP_STRUT ? 4.5f : (o.corner ? 1.8f : 2.6f);
            float frac = s->support == SUP_COLUMN ? 0.4f : s->support == SUP_STRUT ? 0.35f : 0.25f;
            if (o.max > lim) REJECT("section %d: projection %.1f m > %.1f for its support", i, o.max, lim);
            else if (o.max > frac * depth) REJECT("section %d: projection %.1f m > %.0f%% of the %.1f m depth below", i, o.max, frac * 100.0f, depth);
            else if (s->support == SUP_COLUMN && (!ncol || reach > 5.0f)) REJECT("section %d: projecting edge %.1f m from a column", i, reach);
            else if (s->support == SUP_STRUT && !nstrut) REJECT("section %d: strut-supported projection without struts", i);
            else if (s->support == SUP_CORE && o.outsideLen > 0.45f * o.perimeter) REJECT("section %d: core cantilever along %.0f%% of its edge", i, 100.0f * o.outsideLen / o.perimeter);
            break;
        }
        case ARR_STEP:
            if (o.max > 1.2f) REJECT("section %d: steps out %.2f m > 1.2 from the slice below", i, o.max);
            break;
        case ARR_SPAN: case ARR_BRIDGE: {
            if (s->nbelow != 2) { REJECT("section %d: span needs two supporters", i); break; }
            const BSection *a = &g->sec[s->below[0]], *c2 = &g->sec[s->below[1]];
            if (!loops_intersect(&s->foot.outer, &a->foot.outer) || !loops_intersect(&s->foot.outer, &c2->foot.outer)) { REJECT("section %d: span does not bear on both supports", i); break; }
            float clear = 1e30f;
            for (int k = 0; k < a->foot.outer.n; ++k)
                clear = minf(clear, sqrtf(loop_dist2(&c2->foot.outer, a->foot.outer.x[k], a->foot.outer.z[k], NULL, NULL, 1e30f)));
            float lim = s->arr == ARR_SPAN ? 32.0f : 40.0f;
            if (clear > lim) REJECT("section %d: clear span %.1f m > %.0f", i, clear, lim);
            else if (s->arr == ARR_SPAN && !transfer) REJECT("section %d: span without a transfer structure", i);
            else if (a->y1 < s->y0 - 0.01f || c2->y1 < s->y0 - 0.01f) REJECT("section %d: supports stop short of the span", i);
            break;
        }
        default:
            REJECT("section %d: unknown arrangement", i);
        }
        if (ok && s->arr != ARR_STACK) blog_printf(log, "  accepted %s for section %d (%.1f m)\n", ARR_NAMES[s->arr], i, o.max);
    }
    /* sections side by side must not collide */
    for (int i = 0; i < g->nsec && ok; ++i)
        for (int j = i + 1; j < g->nsec && ok; ++j) {
            const BSection *a = &g->sec[i], *c2 = &g->sec[j];
            if (a->y1 <= c2->y0 + 0.01f || c2->y1 <= a->y0 + 0.01f) continue;
            int related = 0;
            for (int k = 0; k < a->nbelow; ++k) if (a->below[k] == j) related = 1;
            for (int k = 0; k < c2->nbelow; ++k) if (c2->below[k] == i) related = 1;
            if (!related && loops_intersect(&a->foot.outer, &c2->foot.outer)) REJECT("sections %d and %d intersect", i, j);
        }
    /* supports: inside the plot, standing on the ground or on a section */
    for (int k = 0; k < g->nsup && ok; ++k) {
        const BSupport *s = &g->sup[k];
        if (s->x0 < b->foot.x0 || s->x0 > b->foot.x1 || s->z0 < b->foot.z0 || s->z0 > b->foot.z1) REJECT("support %d outside its plot", k);
        else if (s->type == SUP_COLUMN && s->y0 > G0 + 0.01f) REJECT("column %d does not reach the ground", k);
        else if (s->type == SUP_STRUT) {
            int on = 0;
            for (int i = 0; i < g->nsec && !on; ++i)
                if (g->sec[i].y0 <= s->y0 && g->sec[i].y1 >= s->y0 && bfoot_boundary_dist(&g->sec[i].foot, s->x0, s->z0, NULL, NULL) < 0.8f) on = 1;
            if (!on) REJECT("strut %d has no wall to bear on", k);
        }
    }
    /* entrances */
    int primary = 0;
    for (int d = 0; d < g->ndoor && ok; ++d) {
        const BDoor *dr = &g->door[d];
        const BSection *s = &g->sec[dr->section];
        const BLoop *l = &s->foot.outer;
        if (s->y0 > G0 + 0.01f || dr->face >= l->n) { REJECT("door %d is not on a ground-level face", d); break; }
        int k1 = (dr->face + 1) % l->n;
        float len = sqrtf((l->x[k1] - l->x[dr->face]) * (l->x[k1] - l->x[dr->face]) + (l->z[k1] - l->z[dr->face]) * (l->z[k1] - l->z[dr->face]));
        if (dr->u - dr->width * 0.5f < 0.55f || dr->u + dr->width * 0.5f > len - 0.55f) REJECT("door %d does not fit its face", d);
        for (int e = 0; e < d && ok; ++e)
            if (g->door[e].section == dr->section && g->door[e].face == dr->face &&
                fabsf(g->door[e].u - dr->u) < (g->door[e].width + dr->width) * 0.5f + 0.5f) REJECT("doors %d and %d overlap", e, d);
        if (dr->type == DOOR_PRIMARY) primary++;
    }
    if (ok && !primary) REJECT("no primary entrance");
    if (ok && top > G0 + b->height + 0.05f) REJECT("top %.1f m above the %.1f m height limit", top - G0, b->height);
#undef REJECT
    return ok;
}

/* ===========================================================================
 * Driver
 * ========================================================================= */

static void init_ctx(Ctx *c, const Building *b, BGraph *g, BLog *log, int attempt) {
    memset(c, 0, sizeof(*c));
    c->b = b; c->g = g; c->log = log;
    c->W = b->foot.x1 - b->foot.x0;
    c->D = b->foot.z1 - b->foot.z0;
    c->H = b->height;
    c->frontage = b->frontage;
    uint32_t s = b->seed;
    c->ra = rng_make(hash_combine(hash_combine(s, 0xA1u), (uint32_t)(attempt < 3 ? 0 : attempt)));
    c->rf = rng_make(hash_combine(hash_combine(s, 0xF0u), (uint32_t)attempt));
    c->rh = rng_make(hash_combine(hash_combine(s, 0x4Eu), (uint32_t)attempt));
    c->rs = rng_make(hash_combine(hash_combine(s, 0x5Fu), (uint32_t)attempt));
    c->rr = rng_make(hash_combine(hash_combine(s, 0x2Fu), (uint32_t)attempt));
    c->rd = rng_make(hash_combine(hash_combine(s, 0xD0u), (uint32_t)attempt));
    memset(g, 0, sizeof(*g));
    g->seed = s;
    g->district = district_of(b);
    g->floorH = city_floor_height(s);
}

static void set_floors(Ctx *c) {
    BGraph *g = c->g;
    g->groundH = city_ground_height(g->seed) * (g->groundMode == GF_DOUBLE ? 2.0f : 1.0f);
    c->N = (int)floorf((c->H - g->groundH) / g->floorH);
    if (c->N < 0) c->N = 0;
    while (c->N > 0 && lvl(c, c->N) > G0 + c->H) c->N--;
}

/* Always valid: the plot as a plain block with a front door. */
static void build_simple(Ctx *c) {
    BGraph *g = c->g;
    g->archetype = c->b->kind == BK_MEGA ? ARCH_ARCOLOGY : ARCH_OFFICE;
    g->groundMode = GF_RETAIL;
    set_floors(c);
    c->defFins = 0;
    FootSpec s = site_spec(c, FAM_RECT, 0.3f);
    BFoot f;
    make_foot(&s, &f);
    int gi = add_sec(c, SEC_GROUND, G0, lvl(c, 0), &f, -1, -1);
    if (c->N > 0 && lvl(c, c->N) > lvl(c, 0) + 1.0f) add_sec(c, SEC_BODY, lvl(c, 0), lvl(c, c->N), &f, gi, -1);
    g->sec[g->nsec - 1].roof = ROOF_PLANT;
    place_doors(c);
}

void bgen_build(const Building *b, BGraph *g, BLog *log) {
    Ctx c;
    blog_printf(log, "building %08x: plot %.0fx%.0f m, height %.0f m, %s\n", b->seed,
         b->foot.x1 - b->foot.x0, b->foot.z1 - b->foot.z0, b->height, DIST_NAMES[district_of(b)]);
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        init_ctx(&c, b, g, log, attempt);
        g->archetype = choose_archetype(&c, &c.ra);
        choose_surfaces(&c);
        g->groundMode = choose_ground(&c);
        g->fine = g->archetype == ARCH_ARCOLOGY;
        set_floors(&c);
        blog_printf(log, " attempt %d: %s, %d floors\n", attempt + 1, ARCH_NAMES[g->archetype], c.N);
        int built = 0;
        switch (g->archetype) {
        case ARCH_OFFICE:     built = arch_office(&c); break;
        case ARCH_SKYSCRAPER: built = arch_skyscraper(&c); break;
        case ARCH_PROJECTION: built = arch_projection(&c); break;
        case ARCH_TWIN:       built = arch_twin(&c); break;
        case ARCH_GATEWAY:    built = arch_gateway(&c); break;
        case ARCH_ARCOLOGY:   built = arch_arcology(&c); break;
        case ARCH_SHOPHOUSE:  built = arch_shophouse(&c); break;
        case ARCH_SLAB:       built = arch_housing(&c, 0); break;
        case ARCH_POINT:      built = arch_housing(&c, 1); break;
        default:              built = arch_shed(&c); break;
        }
        if (!built) { blog_printf(log, "  rejected: proposal does not fit the plot\n"); continue; }
        if (g->groundMode == GF_VOID) g->groundH = city_ground_height(g->seed);
        choose_roofs(&c);
        place_doors(&c);
        if (bgen_validate(g, b, log)) {
            g->attempts = attempt + 1;
            blog_printf(log, " accepted: %d sections, %d supports, %d doors\n", g->nsec, g->nsup, g->ndoor);
            return;
        }
    }
    init_ctx(&c, b, g, log, MAX_ATTEMPTS);
    choose_surfaces(&c);
    build_simple(&c);
    choose_roofs(&c);
    g->attempts = MAX_ATTEMPTS;
    g->simplified = 1;
    blog_printf(log, " simplified to a plain block\n");
}
