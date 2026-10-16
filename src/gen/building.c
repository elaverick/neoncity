#include "building_internal.h"

#define MAX_ATTEMPTS 6

static const char *const ARCH_NAMES[ARCH_COUNT] = {
    "office block", "skyscraper", "podium with projection", "twin towers", "gateway",
    "arcology", "shophouse row", "slab block", "point block", "shed" };
static const char *const DIST_NAMES[DIST_COUNT] = { "downtown", "midrise", "centre", "estate", "industrial" };
static const char *const ARR_NAMES[] = { "ground", "stack", "arcade", "projection", "step-out", "span", "sky bridge" };

const char *bgen_archetype_name(int a) { return a >= 0 && a < ARCH_COUNT ? ARCH_NAMES[a] : "?"; }
const char *bgen_district_name(int d) { return d >= 0 && d < DIST_COUNT ? DIST_NAMES[d] : "?"; }

float bgen_bay_width(uint32_t seed, int fine) {
    float h = hash_float(hash_combine(hash_u32(seed ^ 0x5f3759dfu), 4u));
    return fine ? 1.6f + 0.8f * h : 2.4f + 2.6f * h;
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

#define REJECT(...) do { blog_printf(log, "  rejected: " __VA_ARGS__); blog_printf(log, "\n"); ok = 0; } while (0)

/* Section i's footprint and courtyard are sound, inside the plot and thick enough. */
static int check_section_shape(const BGraph *g, const Building *b, int i, BLog *log) {
    const float tol = 0.02f;
    int ok = 1;
    const BSection *s = &g->sec[i];
    const BLoop *l = &s->foot.outer;
    if (l->n < 3 || !loop_simple(l) || loop_min_edge(l) < 0.9f || bloop_area(l) > -20.0f) { REJECT("section %d: invalid footprint", i); return 0; }
    if (s->foot.hole.n) {
        const BLoop *h = &s->foot.hole;
        if (!loop_simple(h) || bloop_area(h) <= 0.0f) { REJECT("section %d: invalid courtyard", i); return 0; }
        for (int k = 0; k < h->n && ok; ++k)
            if (!bloop_inside(l, h->x[k], h->z[k]) || sqrtf(loop_dist2(l, h->x[k], h->z[k], NULL, NULL, 1e30f)) < 6.0f)
                REJECT("section %d: courtyard leaves too thin a floor plate", i);
    }
    for (int k = 0; k < l->n && ok; ++k)
        if (l->x[k] < b->foot.x0 - tol || l->x[k] > b->foot.x1 + tol || l->z[k] < b->foot.z0 - tol || l->z[k] > b->foot.z1 + tol)
            REJECT("section %d: outside its plot", i);
    if (!ok) return 0;
    if (s->y1 < s->y0 + 1.0f) { REJECT("section %d: too thin", i); return 0; }
    return ok;
}

/* Section i stands on the ground or on what is below it, within the rules of its arrangement. */
static int check_section_support(const BGraph *g, int i, BLog *log) {
    int ok = 1;
    const BSection *s = &g->sec[i];
    if (s->nbelow == 0) {
        if (s->y0 > G0 + 0.01f) REJECT("section %d: floating %.1f m above the ground", i, s->y0 - G0);
        return ok;
    }
    for (int k = 0; k < s->nbelow; ++k)
        if (g->sec[s->below[k]].y0 >= s->y0 - 0.01f) { REJECT("section %d: supporter %d is not below it", i, s->below[k]); break; }
    if (!ok) return 0;
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
    return ok;
}

/* Sections side by side must not collide. */
static int check_collisions(const BGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nsec && ok; ++i)
        for (int j = i + 1; j < g->nsec && ok; ++j) {
            const BSection *a = &g->sec[i], *c2 = &g->sec[j];
            if (a->y1 <= c2->y0 + 0.01f || c2->y1 <= a->y0 + 0.01f) continue;
            int related = 0;
            for (int k = 0; k < a->nbelow; ++k) if (a->below[k] == j) related = 1;
            for (int k = 0; k < c2->nbelow; ++k) if (c2->below[k] == i) related = 1;
            if (!related && loops_intersect(&a->foot.outer, &c2->foot.outer)) REJECT("sections %d and %d intersect", i, j);
        }
    return ok;
}

/* Supports: inside the plot, standing on the ground or on a section. */
static int check_supports(const BGraph *g, const Building *b, BLog *log) {
    int ok = 1;
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
    return ok;
}

/* Entrances: on ground-level faces, fitting them, not overlapping; counts the primary ones. */
static int check_doors(const BGraph *g, BLog *log, int *primary) {
    int ok = 1;
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
        if (dr->type == DOOR_PRIMARY) (*primary)++;
    }
    return ok;
}

int bgen_validate(const BGraph *g, const Building *b, BLog *log) {
    int ok = 1;
    if (g->nsec == 0) { REJECT("no sections"); return 0; }
    float top = 0.0f;
    for (int i = 0; i < g->nsec && ok; ++i) {
        top = maxf(top, g->sec[i].y1);
        ok = check_section_shape(g, b, i, log) && check_section_support(g, i, log);
    }
    if (ok) ok = check_collisions(g, log);
    if (ok) ok = check_supports(g, b, log);
    int primary = 0;
    if (ok) ok = check_doors(g, log, &primary);
    if (ok && !primary) REJECT("no primary entrance");
    if (ok && top > G0 + b->height + 0.05f) REJECT("top %.1f m above the %.1f m height limit", top - G0, b->height);
    return ok;
}
#undef REJECT

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
