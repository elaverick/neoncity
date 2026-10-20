#include "building_internal.h"

/* ===========================================================================
 * Industrial plants and electrical substations (section 11 of
 * docs/procedural-building-generation.md)
 *
 * A plant is laid out in a fenced yard. The control building goes first, on
 * the fence line beside the gate with its door to the street; then, rarely, a
 * gasholder, which needs the largest free circle; then the layout's core: a
 * pipe rack with a row of units along each side, or an elevated deck carried
 * by legs and by the vessels that pass through it, with smaller vessels under
 * it and units round it; then a stack and perhaps flares; then the works
 * (halls, warehouses, an office block) in whatever ground is left; then a
 * ladder or stair to every elevated level, and pipes between the units. Each
 * unit is proposed, checked on its own (slenderness, footprint, clearance)
 * and adjusted (widened, framed, shortened) before it is dropped, and the
 * reason is logged. industry_validate then checks the whole with the same
 * rules, after the general ones (building.c).
 *
 * On the colossal tier every tall unit's rise is multiplied by g->rise, so
 * stacks, flares, columns and office blocks stand among the towers; the rules
 * are the same ones, so a taller unit is a wider one.
 *
 * A substation is the same machinery on a small fenced pad: transformers in
 * their oil bunds in a row, a busbar gantry behind them, a switch house and a
 * kiosk either side of the gate.
 * ========================================================================= */

#define FENCE_IN      1.0f     /* the fence stands this far inside the plot */
#define GAP           1.6f     /* the access gap left between units */
#define GAP_MIN       1.2f     /* ... and the least validation accepts */
#define PORT_MAX      26.0f    /* pipe ports stay this low on a colossal unit */
#define REACH         10.0f    /* every point of a deck within this of a support */
#define DECK_OVER     3.0f     /* how far a deck may overhang the hull of its supports */
#define BENT          6.0f     /* a pipe rack's bents */
#define BENT_MAX      8.0f
#define PIPE_SPAN_MAX 9.0f     /* a pipe's supports at most this far apart */
#define LEVEL         6.0f     /* a steel frame's levels */
#define TOP_CAP       235.0f
#define SLIM_COLUMN   7.5f     /* height over diameter */
#define SLIM_FRAMED   14.0f
#define SLIM_STACK    13.0f
#define SQUAT_TANK    1.3f
#define HALL_SPAN     22.0f
#define FLARE_BASE    22.0f
#define POST_GAP      9.0f     /* a gasholder's guide posts round its circumference */
#define BUND_MARGIN   1.5f
#define BUND_GAP      5.0f
#define FIREWALL_GAP  10.0f
#define GANTRY_BAY    12.0f

static const char *const UNIT_NAMES[IU_COUNT] = {
    "-", "column", "tank", "dome vessel", "exchanger", "stack", "flare", "gasholder", "frame", "pipe rack",
    "transformer", "firewall", "busbar gantry", "control building", "hall", "warehouse", "office block",
    "switch house", "kiosk" };
static const char *const STYLE_NAMES[PST_COUNT] = { "gas plant", "gothic refinery", "night refinery" };

const char *bgen_unit_name(int u) { return u >= 0 && u < IU_COUNT ? UNIT_NAMES[u] : "?"; }
const char *bgen_style_name(int s) { return s >= 0 && s < PST_COUNT ? STYLE_NAMES[s] : "?"; }

/* ===========================================================================
 * Shared geometry
 * ========================================================================= */

static float loop_gap(const BLoop *a, const BLoop *b) {
    if (loops_intersect(a, b)) return 0.0f;
    float d = 1e30f;
    for (int i = 0; i < a->n; ++i) d = loop_dist2(b, a->x[i], a->z[i], NULL, NULL, d);
    for (int i = 0; i < b->n; ++i) d = loop_dist2(a, b->x[i], b->z[i], NULL, NULL, d);
    return sqrtf(d);
}

static void rect_foot(BFoot *f, float x0, float z0, float x1, float z1) {
    FootSpec s;
    memset(&s, 0, sizeof(s));
    s.fam = FAM_RECT; s.w = x1 - x0; s.d = z1 - z0; s.cx = (x0 + x1) * 0.5f; s.cz = (z0 + z1) * 0.5f;
    make_foot(&s, f);
}

static void drum_foot(BFoot *f, float cx, float cz, float r) {
    FootSpec s;
    memset(&s, 0, sizeof(s));
    s.fam = FAM_DRUM; s.w = s.d = 2.0f * r; s.p1 = r >= 7.0f ? 20.0f : r >= 3.0f ? 16.0f : 12.0f; s.cx = cx; s.cz = cz;
    make_foot(&s, f);
}

/* A flare's lattice mast: an equilateral triangle of legs, `side` apart. */
static void tri_foot(BFoot *f, float cx, float cz, float side) {
    memset(f, 0, sizeof(*f));
    float R = side / sqrtf(3.0f);
    for (int k = 0; k < 3; ++k) {
        float a = PI_F * (0.5f + 2.0f * (float)k / 3.0f);
        loop_add(&f->outer, cx + R * cosf(a), cz + R * sinf(a));
    }
    loop_orient(&f->outer, 1);
}

static void rect_loop(BLoop *l, float x0, float z0, float x1, float z1) {
    l->n = 0;
    loop_add(l, x0, z0); loop_add(l, x0, z1); loop_add(l, x1, z1); loop_add(l, x1, z0);
}

void bloop_circle(const BLoop *l, float *cx, float *cz, float *r) {
    float x = 0.0f, z = 0.0f;
    for (int i = 0; i < l->n; ++i) { x += l->x[i]; z += l->z[i]; }
    x /= (float)l->n; z /= (float)l->n;
    *cx = x; *cz = z;
    *r = sqrtf((l->x[0] - x) * (l->x[0] - x) + (l->z[0] - z) * (l->z[0] - z));
}

static void bounds(const BLoop *l, float *x0, float *z0, float *x1, float *z1) { loop_bounds(l, x0, z0, x1, z1); }

int industry_tied(const BGraph *g, int i, int j) {
    const BSection *a = &g->sec[i], *b = &g->sec[j];
    if (a->braceBy == j + 1 || b->braceBy == i + 1) return 1;
    for (int h = 0; h < g->nhole; ++h)
        if ((g->hole[h].deck == i && g->hole[h].unit == j) || (g->hole[h].deck == j && g->hole[h].unit == i)) return 1;
    return 0;
}

/* Whether a horizontal or vertical pipe segment of radius r comes within reach of section s. */
static int seg_hits(const BSection *s, float ax, float ay, float az, float bx, float by, float bz, float r) {
    float lo = minf(ay, by) - r - 0.15f, hi = maxf(ay, by) + r + 0.15f;
    if (hi <= s->y0 || lo >= s->y1) return 0;
    const BLoop *l = &s->foot.outer;
    float ex = bx - ax, ez = bz - az, len = sqrtf(ex * ex + ez * ez);
    int steps = (int)ceilf(len / 0.5f);
    for (int k = 0; k <= steps; ++k) {
        float t = steps ? (float)k / (float)steps : 0.0f;
        float x = ax + ex * t, z = az + ez * t;
        if (bloop_inside(l, x, z) || loop_dist2(l, x, z, NULL, NULL, 1e30f) < (r + 0.15f) * (r + 0.15f)) return 1;
    }
    return 0;
}

/* A deck section's top, if (x, z) lies on it. */
static int on_deck(const BGraph *g, float x, float z, float *top) {
    for (int i = 0; i < g->nsec; ++i)
        if (g->sec[i].type == SEC_DECK && bfoot_contains(&g->sec[i].foot, x, z)) { *top = g->sec[i].y1; return i; }
    return -1;
}

int bgen_pipe_posts(const BGraph *g, const BPipe *p, float (*out)[4], int max, float *longest) {
    int n = 0;
    float run = 0.0f, worst = 0.0f;
    for (int k = 0; k + 1 < p->n; ++k) {
        float ax = p->x[k], ay = p->y[k], az = p->z[k], bx = p->x[k + 1], by = p->y[k + 1], bz = p->z[k + 1];
        float len = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
        if (len < 0.01f || fabsf(by - ay) > 0.01f) { run = 0.0f; continue; }   /* a riser holds itself up */
        int steps = (int)ceilf(len / 3.0f);
        for (int j = 1; j <= steps; ++j) {
            float t = (float)j / (float)steps;
            float x = ax + (bx - ax) * t, z = az + (bz - az) * t;
            run += len / (float)steps;
            if (j == steps && k + 2 == p->n) break;       /* the far end is its nozzle */
            if (run < 4.5f) continue;
            /* what can carry it here: a rack or frame it passes through, a deck under it, or the ground */
            int carried = 0, blocked = 0;
            float base = G0, top;
            for (int i = 0; i < g->nsec && !carried && !blocked; ++i) {
                const BSection *s = &g->sec[i];
                if (s->y0 > ay || !bfoot_contains(&s->foot, x, z)) continue;
                if (bsec_open(s) && s->y1 >= ay - 2.5f) carried = 1;
                else if (s->type != SEC_DECK) blocked = 1;
            }
            int d = on_deck(g, x, z, &top);
            if (d >= 0) {
                if (top <= ay - p->r) base = top;
                else if (g->sec[d].y0 >= ay + p->r) base = G0;
                else carried = 1;                          /* it runs through the deck's girders */
            }
            if (!blocked && !carried && base == G0)
                for (int i = 0; i < g->nsec && !blocked; ++i)
                    if (g->sec[i].y0 <= G0 + 0.01f && g->sec[i].type != SEC_DECK && !bsec_open(&g->sec[i]) &&
                        bfoot_boundary_dist(&g->sec[i].foot, x, z, NULL, NULL) < 0.35f) blocked = 1;
            if (blocked) continue;
            if (!carried && n < max) { out[n][0] = x; out[n][1] = z; out[n][2] = base; out[n][3] = ay - p->r; n++; }
            worst = maxf(worst, run);
            run = 0.0f;
        }
    }
    worst = maxf(worst, run);
    if (longest) *longest = worst;
    return n;
}

/* ===========================================================================
 * The yard
 * ========================================================================= */

typedef struct Plant {
    Ctx    *c;
    BGraph *g;
    BLog   *log;
    Rng    *r;
    float   x0, z0, x1, z1;        /* the yard inside the fence */
    float   cap;                   /* nothing above G0 + cap */
    int     simplify;
    /* the street side: its origin, the way along it and inward, its length and the yard's depth */
    int     street;
    float   ox, oz, ux, uz, ix, iz, len, depth;
    int     nkeep;
    BLoop   keep[12];              /* kept clear on the ground: the gate lane, before doors, stairs */
    int     rack, deck;            /* the layout's core, -1 if none */
} Plant;

static void at(const Plant *P, float u, float v, float *x, float *z) {
    *x = P->ox + P->ux * u + P->ix * v;
    *z = P->oz + P->uz * u + P->iz * v;
}

/* The rectangle u0..u1 along the street, v0..v1 in from it, in the plot's frame. */
static void uv_rect(const Plant *P, float u0, float v0, float u1, float v1, float *x0, float *z0, float *x1, float *z1) {
    float ax, az, bx, bz;
    at(P, u0, v0, &ax, &az);
    at(P, u1, v1, &bx, &bz);
    *x0 = minf(ax, bx); *x1 = maxf(ax, bx); *z0 = minf(az, bz); *z1 = maxf(az, bz);
}

static void keep_rect(Plant *P, float x0, float z0, float x1, float z1) {
    if (P->nkeep < 12) rect_loop(&P->keep[P->nkeep++], x0, z0, x1, z1);
}

static int pick_street(const Building *b) {
    static const int order[4] = { SIDE_S, SIDE_N, SIDE_W, SIDE_E };
    float W = b->foot.x1 - b->foot.x0, D = b->foot.z1 - b->foot.z0;
    int best = SIDE_S;
    float bl = -1.0f;
    for (int k = 0; k < 4; ++k) {
        if (!(b->frontage & order[k])) continue;
        float l = order[k] == SIDE_S || order[k] == SIDE_N ? W : D;
        if (l > bl) { bl = l; best = order[k]; }
    }
    return best;
}

static void yard_init(Plant *P, Ctx *c, int simplify, float inset) {
    memset(P, 0, sizeof(*P));
    P->c = c; P->g = c->g; P->log = c->log; P->r = &c->rf;
    P->simplify = simplify;
    const Building *b = c->b;
    P->x0 = b->foot.x0 + inset; P->x1 = b->foot.x1 - inset;
    P->z0 = b->foot.z0 + inset; P->z1 = b->foot.z1 - inset;
    P->rack = P->deck = -1;
    P->street = pick_street(b);
    switch (P->street) {
    case SIDE_S: P->ox = P->x0; P->oz = P->z0; P->ux = 1; P->uz = 0; P->ix = 0; P->iz = 1; break;
    case SIDE_N: P->ox = P->x1; P->oz = P->z1; P->ux = -1; P->uz = 0; P->ix = 0; P->iz = -1; break;
    case SIDE_W: P->ox = P->x0; P->oz = P->z1; P->ux = 0; P->uz = -1; P->ix = 1; P->iz = 0; break;
    default:     P->ox = P->x1; P->oz = P->z0; P->ux = 0; P->uz = 1; P->ix = -1; P->iz = 0; break;
    }
    int ns = P->street == SIDE_S || P->street == SIDE_N;
    P->len = ns ? P->x1 - P->x0 : P->z1 - P->z0;
    P->depth = ns ? P->z1 - P->z0 : P->x1 - P->x0;
    BGraph *g = c->g;
    g->fence[0] = P->x0; g->fence[1] = P->z0; g->fence[2] = P->x1; g->fence[3] = P->z1;
    g->gateSide = P->street;
}

/* How far a flare of height h keeps from every other unit, for the heat it throws. */
static float flare_clearance(float h) { return maxf(7.0f, 0.15f * h); }

/* Whether footprint f, standing y0..y1, keeps `gap` from everything placed
 * whose heights it shares (but `skip`, and its legs), keeps out of the lanes
 * on the ground and stays in the yard. */
static int clear_of(const Plant *P, const BLoop *l, float y0, float y1, float gap, int skip) {
    for (int k = 0; k < l->n; ++k)
        if (l->x[k] < P->x0 - 0.01f || l->x[k] > P->x1 + 0.01f || l->z[k] < P->z0 - 0.01f || l->z[k] > P->z1 + 0.01f) return 0;
    if (y0 <= G0 + 0.01f)
        for (int k = 0; k < P->nkeep; ++k) if (loop_gap(l, &P->keep[k]) < 0.3f) return 0;
    const BGraph *g = P->g;
    for (int t = 0; t < g->nsec; ++t) {
        const BSection *s = &g->sec[t];
        if (t == skip || s->y1 <= y0 + 0.01f || s->y0 >= y1 - 0.01f) continue;
        float need = s->unit == IU_FLARE ? maxf(gap, flare_clearance(s->y1 - G0)) : gap;
        if (loop_gap(l, &s->foot.outer) < need) return 0;
    }
    for (int k = 0; k < g->nsup; ++k) {
        const BSupport *sp = &g->sup[k];
        if (sp->type != SUP_LEG || sp->section == skip || sp->y1 <= y0 || sp->y0 >= y1) continue;
        if (bloop_inside(l, sp->x0, sp->z0) || loop_dist2(l, sp->x0, sp->z0, NULL, NULL, 1e30f) < 0.64f) return 0;
    }
    return 1;
}

/* The facade palette nearest a paint, for the distant proxies' boxes. */
static uint8_t proxy_palette(int paint) {
    static const uint8_t p[16] = { 3, 5, 1, 1, 2, 5, 4, 3, 0, 1, 5, 2, 0, 4, 6, 7 };
    return p[paint & 15];
}

static int vessel_paint(const Plant *P, Rng *r) {
    switch (P->g->style) {
    case PST_GAS:    return rng_float(r) < 0.7f ? PA_PALE : PA_WHITE;
    case PST_GOTHIC: { float x = rng_float(r); return x < 0.45f ? PA_ORANGE : x < 0.8f ? PA_CREAM : PA_RED; }
    default:        return rng_float(r) < 0.75f ? PA_SOOT : PA_RUST;
    }
}

static int steel_paint(const Plant *P) { return P->g->style == PST_GAS ? PA_GALV : P->g->style == PST_GOTHIC ? PA_SOOT : PA_STEEL; }

static int add_unit(Plant *P, int unit, const BFoot *f, float h, int head, int paint) {
    int si = add_sec(P->c, SEC_UNIT, G0, G0 + h, f, -1, -1);
    if (si < 0) return -1;
    BSection *s = &P->g->sec[si];
    s->unit = (uint8_t)unit; s->head = (uint8_t)head; s->paint = (uint8_t)paint;
    s->win = WIN_PUNCHED; s->palette = proxy_palette(paint); s->bands = 0; s->fins = 0; s->ledTop = 0;
    s->bands = P->g->style == PST_GOTHIC && (unit == IU_DOME || unit == IU_COLUMN || unit == IU_TANK);   /* painted bands */
    return si;
}

/* ===========================================================================
 * Units: proposed, checked on their own and adjusted before they are dropped
 * ========================================================================= */

/* A column at (cx, cz), within `room` of it: widened, then framed, then
 * shortened until it is slender enough; then its footprint, and its frame's. */
static int propose_column(Plant *P, float cx, float cz, float room, float minTop, int unit) {
    Rng *r = P->r;
    BGraph *g = P->g;
    float rad = rng_range(r, 0.9f, 2.0f);
    float h = rng_range(r, 14.0f, 30.0f);
    if (unit == IU_DOME) { rad = rng_range(r, 2.2f, 4.2f); h = rad * rng_range(r, 2.4f, 5.0f); }
    rad = minf(rad, room);
    if (rad < 0.8f) return -1;
    h = minf(maxf(h * g->rise, minTop), P->cap);
    float rmax = minf(room, g->tier == PT_COLOSSAL ? 6.5f : 2.8f);
    if (h / (2.0f * rad) > SLIM_COLUMN && g->tier == PT_COLOSSAL) {
        float want = h / (2.0f * SLIM_COLUMN);
        if (want <= rmax) {
            blog_printf(P->log, "  %s %.0f m: %.1f slender > %.1f, widened to %.1f m across\n", UNIT_NAMES[unit], h, h / (2.0f * rad), SLIM_COLUMN, 2.0f * want);
            rad = want;
        }
    }
    int framed = 0;
    float half = 0.0f, hf = 0.0f;
    if (h / (2.0f * rad) > SLIM_COLUMN && P->simplify < 1) {
        float rr = maxf(rad, h / (2.0f * SLIM_FRAMED));
        int levels = (int)ceilf(h * 0.6f / LEVEL);
        if (levels < 2) levels = 2;
        hf = minf((float)levels * LEVEL, floorf((h - 2.0f) / LEVEL) * LEVEL);
        half = rr + 1.6f;
        if (rr <= rmax + 0.01f && hf >= 2.0f * LEVEL && half <= room + 1.6f) {
            BFoot ff;
            rect_foot(&ff, cx - half, cz - half, cx + half, cz + half);
            if (clear_of(P, &ff.outer, G0, G0 + hf, GAP, -1)) { framed = 1; rad = rr; }
        }
        if (!framed) blog_printf(P->log, "  rejected: frame for a %.0f m %s (no room)\n", h, UNIT_NAMES[unit]);
    }
    if (!framed && h / (2.0f * rad) > SLIM_COLUMN) {
        float h2 = 2.0f * rad * SLIM_COLUMN;
        blog_printf(P->log, "  %s %.0f m: %.1f slender, shortened to %.0f m\n", UNIT_NAMES[unit], h, h / (2.0f * rad), h2);
        h = h2;
        if (h < minTop) return -1;
    }
    BFoot f;
    drum_foot(&f, cx, cz, rad);
    int head = unit == IU_DOME ? IH_DOME : g->style == PST_NIGHT && rng_float(r) < 0.4f ? IH_CONE : IH_DOME;
    float hh = bgen_head_height(head, rad);
    if (h < hh + 3.0f) return -1;
    if (!framed && !clear_of(P, &f.outer, G0, G0 + h, GAP, -1)) return -1;
    int fi = -1;
    if (framed) {
        BFoot ff;
        rect_foot(&ff, cx - half, cz - half, cx + half, cz + half);
        fi = add_unit(P, IU_FRAME, &ff, hf, IH_FLAT, steel_paint(P));
        if (fi < 0) return -1;
        int per = half * 2.0f > 8.0f ? 3 : 2;
        for (int a = 0; a < per; ++a)
            for (int b = 0; b < per; ++b) {
                if (a > 0 && a < per - 1 && b > 0 && b < per - 1) continue;
                float lx = cx - half + 0.25f + (2.0f * half - 0.5f) * (float)a / (float)(per - 1);
                float lz = cz - half + 0.25f + (2.0f * half - 0.5f) * (float)b / (float)(per - 1);
                add_support(P->c, SUP_LEG, fi, lx, G0, lz, lx, G0 + hf, lz, 0.4f);
            }
        blog_printf(P->log, "  %s %.0f m framed: %.1f slender, tied at %d levels\n", UNIT_NAMES[unit], h, h / (2.0f * rad), (int)(hf / LEVEL));
    }
    int si = add_unit(P, unit, &f, h, head, vessel_paint(P, r));
    if (si >= 0 && fi >= 0) g->sec[si].braceBy = (uint8_t)(fi + 1);
    return si;
}

static int propose_tank(Plant *P, float cx, float cz, float room, float maxH) {
    Rng *r = P->r;
    float rad = minf(rng_range(r, 2.5f, 6.5f), room);
    if (rad < 1.8f) return -1;
    float h = 2.0f * rad * rng_range(r, 0.45f, 1.45f);
    if (h > SQUAT_TANK * 2.0f * rad) {
        blog_printf(P->log, "  tank %.1f m on %.1f m: too tall, cut to %.1f m\n", h, 2.0f * rad, SQUAT_TANK * 2.0f * rad);
        h = SQUAT_TANK * 2.0f * rad;
    }
    int head = P->g->style == PST_GOTHIC ? IH_DOME : rng_float(r) < 0.6f ? IH_CONE : IH_FLAT;
    h = minf(h, 16.0f);
    h = minf(h, maxH);
    if (h < 2.5f) return -1;
    BFoot f;
    drum_foot(&f, cx, cz, rad);
    if (!clear_of(P, &f.outer, G0, G0 + h, GAP, -1)) return -1;
    return add_unit(P, IU_TANK, &f, h, head, vessel_paint(P, r));
}

/* A horizontal exchanger, its long side along x (alongX) or z. */
static int propose_exchanger(Plant *P, float cx, float cz, int alongX, float roomAlong, float roomAcross, float maxH) {
    Rng *r = P->r;
    float L = minf(rng_range(r, 5.0f, 9.0f), roomAlong * 2.0f), d = minf(rng_range(r, 1.2f, 2.0f), roomAcross * 2.0f - 0.6f);
    if (L < 4.0f || d < 1.0f || 1.1f + d > maxH) return -1;
    float hw = alongX ? L * 0.5f : (d + 0.6f) * 0.5f, hd = alongX ? (d + 0.6f) * 0.5f : L * 0.5f;
    BFoot f;
    rect_foot(&f, cx - hw, cz - hd, cx + hw, cz + hd);
    if (!clear_of(P, &f.outer, G0, G0 + 1.1f + d, GAP, -1)) return -1;
    return add_unit(P, IU_EXCHANGER, &f, 1.1f + d, IH_FLAT, P->g->style == PST_GAS ? PA_WHITE : vessel_paint(P, r));
}

/* One process unit of the style's mix, centred at (cx, cz) with room round it. */
static int propose_unit(Plant *P, float cx, float cz, int alongX, float roomAlong, float roomAcross, float maxH) {
    static const float W[PST_COUNT][4] = {   /* column, tank, dome, exchanger */
        { 0.4f, 0.3f, 0.05f, 0.25f }, { 0.2f, 0.2f, 0.45f, 0.15f }, { 0.45f, 0.25f, 0.1f, 0.2f } };
    int k = pick(P->r, W[P->g->style], 4);
    float room = minf(roomAlong, roomAcross);
    if (maxH < 12.0f && k == 0) k = 1;
    if (k == 0) return propose_column(P, cx, cz, room, 0.0f, IU_COLUMN);
    if (k == 1) return propose_tank(P, cx, cz, room, maxH);
    if (k == 2) return maxH < 12.0f ? -1 : propose_column(P, cx, cz, room, 0.0f, IU_DOME);
    return propose_exchanger(P, cx, cz, alongX, roomAlong, roomAcross, maxH);
}

/* A stack anywhere free: widened up to its largest diameter, then shortened, until it is slender enough. */
static int place_stack(Plant *P) {
    Rng *r = P->r;
    BGraph *g = P->g;
    float rad = rng_range(r, 1.1f, 2.0f), h = rng_range(r, 30.0f, 60.0f);
    h = minf(h * g->rise, P->cap);
    float rmax = g->tier == PT_COLOSSAL ? 9.0f : 2.8f;
    if (h / (2.0f * rad) > SLIM_STACK) {
        float want = minf(h / (2.0f * SLIM_STACK), rmax);
        blog_printf(P->log, "  stack %.0f m: %.1f slender > %.0f, widened to %.1f m across\n", h, h / (2.0f * rad), SLIM_STACK, 2.0f * want);
        rad = want;
        if (h / (2.0f * rad) > SLIM_STACK) { h = 2.0f * rad * SLIM_STACK; blog_printf(P->log, "  stack shortened to %.0f m\n", h); }
    }
    int head = g->style == PST_GOTHIC ? IH_COWL : IH_FLAT;
    for (int tries = 0; tries < 40; ++tries) {
        float cx = rng_range(r, P->x0 + rad + 0.5f, P->x1 - rad - 0.5f), cz = rng_range(r, P->z0 + rad + 0.5f, P->z1 - rad - 0.5f);
        BFoot f;
        drum_foot(&f, cx, cz, rad);
        if (!clear_of(P, &f.outer, G0, G0 + h, GAP, -1)) continue;
        int near = 0;   /* stacks keep clear of decks */
        for (int i = 0; i < g->nsec && !near; ++i) near = g->sec[i].type == SEC_DECK && loop_gap(&f.outer, &g->sec[i].foot.outer) < 3.0f;
        if (near) continue;
        int si = add_unit(P, IU_STACK, &f, h, head, g->style == PST_GAS ? PA_MINT : g->style == PST_GOTHIC ? PA_RUST : PA_SOOT);
        if (si >= 0) blog_printf(P->log, "  stack: %.0f m, %.1f m across\n", h, 2.0f * rad);
        return si;
    }
    blog_printf(P->log, "  rejected: stack (no room)\n");
    return -1;
}

/* How far a flare of height h keeps from every other unit, for the heat it throws. */
/* A flare: a three-legged mast at least a tenth as wide as it is tall, kept
 * well away from every other unit. */
static int place_flare(Plant *P) {
    Rng *r = P->r;
    BGraph *g = P->g;
    float h0 = minf(rng_range(r, 25.0f, 50.0f) * g->rise, P->cap);
    for (int step = 0; step < 4; ++step) {
        float h = h0 * (1.0f - 0.2f * (float)step);
        float base = maxf(h / 10.0f, 4.5f);
        if (base > FLARE_BASE) { base = FLARE_BASE; h = minf(h, FLARE_BASE * 10.0f); }
        float sep = flare_clearance(h);
        for (int tries = 0; tries < 24; ++tries) {
            float m = base * 0.6f + 0.3f;
            float cx = rng_range(r, P->x0 + m, P->x1 - m), cz = rng_range(r, P->z0 + m, P->z1 - m);
            BFoot f;
            tri_foot(&f, cx, cz, base);
            if (!clear_of(P, &f.outer, G0, G0 + h, sep, -1)) continue;
            int si = add_unit(P, IU_FLARE, &f, h, IH_FLAT, steel_paint(P));
            if (si < 0) return -1;
            for (int k = 0; k < 3; ++k)
                add_support(P->c, SUP_LEG, si, f.outer.x[k], G0, f.outer.z[k], f.outer.x[k], G0 + h * 0.3f, f.outer.z[k], 0.35f);
            blog_printf(P->log, "  flare: %.0f m mast on a %.1f m base, %.0f m clear\n", h, base, sep);
            return si;
        }
    }
    blog_printf(P->log, "  rejected: flare (no room clear of the other units)\n");
    return -1;
}

/* The gasholder: a guide frame of posts and rings round nested lifts. It
 * needs the largest free circle, and takes one as far into a corner of the
 * free ground (fx0..fx1, fz0..fz1) as leaves it the size it wants, or is omitted. */
static int place_gasholder(Plant *P, float fx0, float fz0, float fx1, float fz1) {
    Rng *r = P->r;
    float want = rng_range(r, 9.0f, 14.0f), best = 0.0f, bestFar = -1.0f, bx = 0.0f, bz = 0.0f, largest = 0.0f;
    float mx = (fx0 + fx1) * 0.5f, mz = (fz0 + fz1) * 0.5f;
    for (float z = fz0 + 8.0f; z <= fz1 - 8.0f; z += 1.0f)
        for (float x = fx0 + 8.0f; x <= fx1 - 8.0f; x += 1.0f) {
            float d = minf(minf(x - P->x0, P->x1 - x), minf(z - P->z0, P->z1 - z));
            for (int t = 0; t < P->g->nsec && d > 8.6f; ++t)
                d = minf(d, sqrtf(loop_dist2(&P->g->sec[t].foot.outer, x, z, NULL, NULL, 1e30f)) - GAP - (bloop_inside(&P->g->sec[t].foot.outer, x, z) ? 1e3f : 0.0f));
            for (int k = 0; k < P->nkeep && d > 8.6f; ++k)
                d = minf(d, sqrtf(loop_dist2(&P->keep[k], x, z, NULL, NULL, 1e30f)) - 0.3f - (bloop_inside(&P->keep[k], x, z) ? 1e3f : 0.0f));
            largest = maxf(largest, d);
            if (d < minf(want, 8.0f) + 0.6f) continue;
            float far = (x - mx) * (x - mx) + (z - mz) * (z - mz) + 50.0f * minf(d, want + 0.6f);   /* into a corner, but at full size */
            if (far > bestFar) { bestFar = far; best = d; bx = x; bz = z; }
        }
    float rad = minf(want, best - 0.6f);
    if (bestFar < 0.0f || rad < 8.0f) { blog_printf(P->log, "  gasholder omitted: largest free circle %.1f m across\n", 2.0f * largest); return -1; }
    float h = 2.0f * rad * rng_range(r, 0.6f, 1.0f);
    BFoot f;
    drum_foot(&f, bx, bz, rad);
    if (!clear_of(P, &f.outer, G0, G0 + h, GAP, -1)) return -1;
    int si = add_unit(P, IU_GASHOLDER, &f, h, IH_FLAT, P->g->style == PST_GAS ? PA_MINT : P->g->style == PST_GOTHIC ? PA_RED : PA_RUST);
    if (si < 0) return -1;
    BSection *s = &P->g->sec[si];
    s->posts = (uint8_t)ceilf(2.0f * PI_F * rad / (POST_GAP - 0.5f));
    s->fill = rng_range(r, 0.15f, 1.0f);
    blog_printf(P->log, "  gasholder: %.0f m across, %.0f m frame, %d guide posts, %.0f%% full\n", 2.0f * rad, h, s->posts, s->fill * 100.0f);
    return si;
}

/* ===========================================================================
 * Buildings
 * ========================================================================= */

static int add_building(Plant *P, int unit, float x0, float z0, float x1, float z1, float h, int roof) {
    BFoot f;
    rect_foot(&f, x0, z0, x1, z1);
    int si = add_sec(P->c, SEC_GROUND, G0, G0 + h, &f, -1, -1);
    if (si < 0) return -1;
    BSection *s = &P->g->sec[si];
    s->unit = (uint8_t)unit;
    s->roof = (uint8_t)roof;
    s->ledTop = 0; s->accent = 0; s->bands = 0; s->plant = 0;
    /* pale cladding and ribbon glazing; ochre walls, punched windows and fins; dark cladding */
    static const uint8_t pal[PST_COUNT] = { 3, 5, 2 }, win[PST_COUNT] = { WIN_RIBBON, WIN_PUNCHED, WIN_RIBBON };
    s->palette = pal[P->g->style];
    s->win = win[P->g->style];
    s->fins = P->g->style == PST_GOTHIC && unit != IU_KIOSK;
    return si;
}

static void add_plain_door(Plant *P, int type, int sec, int face, float u, float w, float h) {
    BGraph *g = P->g;
    if (g->ndoor >= BG_MAX_DOOR) return;
    BDoor *d = &g->door[g->ndoor++];
    memset(d, 0, sizeof(*d));
    d->type = type; d->section = sec; d->face = face; d->u = u; d->width = w; d->height = h;
}

/* The face of a rectangle section whose outward normal is (nx, nz). */
static int face_facing(const BSection *s, float nx, float nz) {
    const BLoop *l = &s->foot.outer;
    for (int k = 0; k < l->n; ++k) {
        int k1 = (k + 1) % l->n;
        float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
        if (len > 1e-3f && (-ez / len) * nx + (ex / len) * nz > 0.9f) return k;
    }
    return 0;
}

/* The ground before face k of a section, `deep` metres out and a metre wider than w either side. */
static void apron(const BSection *s, int k, float u, float w, float deep, BLoop *out) {
    const BLoop *l = &s->foot.outer;
    int k1 = (k + 1) % l->n;
    float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], len = sqrtf(ex * ex + ez * ez);
    ex /= len; ez /= len;
    float nx = -ez, nz = ex;
    float ax = l->x[k] + ex * (u - w * 0.5f - 1.0f), az = l->z[k] + ez * (u - w * 0.5f - 1.0f);
    float bx = l->x[k] + ex * (u + w * 0.5f + 1.0f), bz = l->z[k] + ez * (u + w * 0.5f + 1.0f);
    float x0 = minf(minf(ax, bx), minf(ax + nx * deep, bx + nx * deep)) + 0.05f * fabsf(nx);
    float x1 = maxf(maxf(ax, bx), maxf(ax + nx * deep, bx + nx * deep));
    float z0 = minf(minf(az, bz), minf(az + nz * deep, bz + nz * deep));
    float z1 = maxf(maxf(az, bz), maxf(az + nz * deep, bz + nz * deep));
    /* start 5 cm off the wall, so the apron doesn't touch the building it serves */
    if (nx > 0.5f) x0 += 0.05f; else if (nx < -0.5f) x1 -= 0.05f;
    if (nz > 0.5f) z0 += 0.05f; else if (nz < -0.5f) z1 -= 0.05f;
    rect_loop(out, x0, z0, x1, z1);
}

/* A service door on whichever face has free ground before it; the ground stays clear. */
static int service_door(Plant *P, int si, int loading) {
    Rng *r = P->r;
    BGraph *g = P->g;
    const BSection *s = &g->sec[si];
    int k0 = rng_int(r, 0, s->foot.outer.n - 1);
    for (int t = 0; t < s->foot.outer.n; ++t) {
        int k = (k0 + t) % s->foot.outer.n;
        const BLoop *l = &s->foot.outer;
        int k1 = (k + 1) % l->n;
        float len = sqrtf((l->x[k1] - l->x[k]) * (l->x[k1] - l->x[k]) + (l->z[k1] - l->z[k]) * (l->z[k1] - l->z[k]));
        float w = loading ? minf(4.5f, len - 2.4f) : 1.3f;
        if (w < (loading ? 3.0f : 1.0f)) continue;
        float u = len * 0.5f;
        BLoop ap;
        apron(s, k, u, w, 4.0f, &ap);
        if (!clear_of(P, &ap, G0, G0 + 3.0f, 0.2f, si)) continue;
        add_plain_door(P, loading ? DOOR_LOADING : DOOR_SERVICE, si, k, u, w, loading ? 4.4f : 2.3f);
        if (P->nkeep < 12) P->keep[P->nkeep++] = ap;
        return 1;
    }
    return 0;
}

/* The control building on the fence line towards one end of the street side,
 * its door to the street, and the gate beside it with its lane kept clear
 * `lane` metres into the yard. */
static int place_control(Plant *P, float bw, float bd, float bh, int unit, float gw, float lane) {
    Rng *r = P->r;
    BGraph *g = P->g;
    float end = rng_range(r, 0.5f, maxf(0.5f, minf(6.0f, P->len - bw - gw - 4.0f)));
    int right = rng_float(r) < 0.5f;
    float u0 = right ? P->len - end - bw : end;
    float ug = right ? u0 - 1.5f - gw * 0.5f : u0 + bw + 1.5f + gw * 0.5f;
    if (u0 < 0.5f || u0 + bw > P->len - 0.5f || ug - gw * 0.5f < 0.5f || ug + gw * 0.5f > P->len - 0.5f) return -1;
    float x0, z0, x1, z1;
    uv_rect(P, u0, 0.0f, u0 + bw, bd, &x0, &z0, &x1, &z1);
    BFoot f;
    rect_foot(&f, x0, z0, x1, z1);
    if (!clear_of(P, &f.outer, G0, G0 + bh, GAP, -1)) return -1;
    int si = add_building(P, unit, x0, z0, x1, z1, bh, ROOF_FLAT);
    if (si < 0) return -1;
    int face = face_facing(&g->sec[si], -P->ix, -P->iz);
    add_plain_door(P, DOOR_PRIMARY, si, face, bw * 0.5f, unit == IU_SWITCH ? 1.6f : 2.0f, 2.5f);
    g->gateW = gw; g->gateU = ug;
    float lx0, lz0, lx1, lz1;
    uv_rect(P, ug - gw * 0.5f - 0.5f, -1.0f, ug + gw * 0.5f + 0.5f, lane, &lx0, &lz0, &lx1, &lz1);
    keep_rect(P, lx0, lz0, lx1, lz1);
    return si;
}

/* Halls, warehouses and an office block in what ground is left, each with a door onto free ground. */
static void place_works(Plant *P, int want) {
    Rng *r = P->r;
    BGraph *g = P->g;
    Ctx *c = P->c;
    float bay = clampf(bgen_bay_width(g->seed, 0), 2.5f, 3.6f);
    int placed = 0, office = 0;
    for (int n = 0; n < want + 2 && placed < want; ++n) {
        float kind = rng_float(r);
        int unit = kind < 0.45f ? IU_HALL : kind < 0.8f || office ? IU_WAREHOUSE : IU_OFFICE;
        float span, len, h;
        int roof;
        if (unit == IU_OFFICE) {
            span = rng_range(r, 3.0f * bay + 0.5f, 5.0f * bay);
            len = span * rng_range(r, 1.0f, 2.0f);
            int floors = rng_int(r, 3, 7);
            h = (g->groundH + (float)floors * g->floorH) * (g->tier == PT_COLOSSAL ? g->rise : 1.0f);
            float lim = span * (g->tier == PT_COLOSSAL ? 10.0f : 4.5f);
            if (h > lim) { blog_printf(P->log, "  office block %.0f m on a %.0f m side: lowered to %.0f m\n", h, span, lim); h = lim; }
            if (h > 40.0f && span < 4.0f * bay) span = 4.0f * bay + 0.1f;   /* four bays across once it is tall */
            h = minf(h, P->cap);
            /* whole floors */
            int fl = (int)floorf((h - g->groundH) / g->floorH);
            if (fl < 1) continue;
            h = g->groundH + (float)fl * g->floorH;
            roof = ROOF_PLANT;
        } else {
            span = rng_range(r, 10.0f, unit == IU_HALL ? 24.0f : 18.0f);
            len = rng_range(r, 14.0f, 32.0f);
            if (span > HALL_SPAN) { blog_printf(P->log, "  %s roof span %.0f m > %.0f: narrowed\n", UNIT_NAMES[unit], span, HALL_SPAN); span = HALL_SPAN; }
            h = unit == IU_HALL ? rng_range(r, 7.0f, 11.0f) : rng_range(r, 6.0f, 9.0f);
            roof = unit == IU_HALL && (g->style == PST_NIGHT || rng_float(r) < 0.3f) ? ROOF_SAWTOOTH : ROOF_GABLE;
        }
        int turn = rng_float(r) < 0.5f;
        float sx = turn ? span : len, sz = turn ? len : span;
        int ok = 0;
        for (int shrink = 0; shrink < 3 && !ok; ++shrink) {
            for (int tries = 0; tries < 30 && !ok; ++tries) {
                if (P->x1 - P->x0 < sx + 1.0f || P->z1 - P->z0 < sz + 1.0f) break;
                float x0 = rng_range(r, P->x0 + 0.5f, P->x1 - 0.5f - sx), z0 = rng_range(r, P->z0 + 0.5f, P->z1 - 0.5f - sz);
                BFoot f;
                rect_foot(&f, x0, z0, x0 + sx, z0 + sz);
                if (!clear_of(P, &f.outer, G0, G0 + h, GAP, -1)) continue;
                int si = add_building(P, unit, x0, z0, x0 + sx, z0 + sz, h, roof);
                if (si < 0) return;
                if (unit == IU_OFFICE) {
                    /* the floors over a ground floor, as any block */
                    float yg = G0 + g->groundH;
                    g->sec[si].y1 = yg;
                    int bi = add_sec(c, SEC_BODY, yg, G0 + h, &f, si, -1);
                    if (bi >= 0) {
                        BSection *bs = &g->sec[bi];
                        bs->unit = IU_OFFICE; bs->roof = ROOF_PLANT; bs->fins = g->sec[si].fins;
                        bs->palette = g->sec[si].palette; bs->win = g->sec[si].win; bs->ledTop = 0; bs->accent = 0; bs->bands = 0;
                    }
                    g->sec[si].roof = ROOF_FLAT;
                }
                if (!service_door(P, si, unit == IU_WAREHOUSE)) {
                    /* no ground before any door: take it away again */
                    g->nsec = si;
                    continue;
                }
                blog_printf(P->log, "  %s: %.0fx%.0f m, %.0f m high\n", UNIT_NAMES[unit], sx, sz, h);
                ok = 1;
                placed++;
                office |= unit == IU_OFFICE;
            }
            /* too little ground: shrink it */
            sx *= 0.8f; sz *= 0.8f;
            if (unit != IU_OFFICE && minf(sx, sz) < 8.0f) break;
            if (unit == IU_OFFICE && minf(sx, sz) < 3.0f * bay) break;
        }
    }
}

/* ===========================================================================
 * Layouts
 * ========================================================================= */

/* A pipe rack down the middle of the free ground, with a row of units along each side. */
static int layout_rack(Plant *P, float rx0, float rz0, float rx1, float rz1) {
    Rng *r = P->r;
    BGraph *g = P->g;
    int alongX = rx1 - rx0 >= rz1 - rz0;
    float a0 = alongX ? rx0 : rz0, a1 = alongX ? rx1 : rz1, b0 = alongX ? rz0 : rx0, b1 = alongX ? rz1 : rx1;
    float wr = rng_range(r, 4.0f, 6.0f), hr = rng_range(r, 6.0f, 8.5f);
    float mid = (b0 + b1) * 0.5f + (b1 - b0) * rng_range(r, -0.12f, 0.12f);
    if (a1 - a0 < 16.0f || b1 - b0 < wr + 6.0f) return 0;
    float s0 = a0 + 1.0f, s1 = a1 - 1.0f;
    BFoot f;
    if (alongX) rect_foot(&f, s0, mid - wr * 0.5f, s1, mid + wr * 0.5f);
    else        rect_foot(&f, mid - wr * 0.5f, s0, mid + wr * 0.5f, s1);
    if (!clear_of(P, &f.outer, G0, G0 + hr, GAP, -1)) {
        blog_printf(P->log, "  rejected: pipe rack (the gasholder is in the way)\n");
        return 0;
    }
    int ri = add_unit(P, IU_RACK, &f, hr, IH_FLAT, steel_paint(P));
    if (ri < 0) return 0;
    P->rack = ri;
    int bents = (int)ceilf((s1 - s0) / BENT);
    for (int k = 0; k <= bents; ++k) {
        float a = s0 + 0.2f + (s1 - s0 - 0.4f) * (float)k / (float)bents;
        for (int sd = -1; sd <= 1; sd += 2) {
            float b = mid + (float)sd * (wr * 0.5f - 0.2f);
            float x = alongX ? a : b, z = alongX ? b : a;
            add_support(P->c, SUP_LEG, ri, x, G0, z, x, G0 + hr, z, 0.35f);
        }
    }
    blog_printf(P->log, "  pipe rack: %.0f m long, %.1f m wide, %.1f m high, %d bents\n", s1 - s0, wr, hr, bents + 1);
    int maxRow = P->simplify ? 2 : 5;
    for (int sd = -1; sd <= 1; sd += 2) {
        float edge = mid + (float)sd * (wr * 0.5f + GAP);
        float room = sd < 0 ? edge - b0 : b1 - edge;
        if (room < 3.0f) continue;
        int n = 0;
        for (float a = s0 + 0.5f; a < s1 - 2.0f && n < maxRow;) {
            float across = minf(room * 0.5f, 7.0f), along = minf(rng_range(r, 3.8f, 7.0f), (s1 - a) * 0.5f);
            float ca = a + along, cb = edge + (float)sd * across;
            float cx = alongX ? ca : cb, cz = alongX ? cb : ca;
            int ns = g->nsec;
            int si = propose_unit(P, cx, cz, alongX, along, across, 99.0f);
            if (si < 0) { a += 2.0f; continue; }
            float hiA = a;   /* past the unit, and its frame */
            for (int t = ns; t < g->nsec; ++t) {
                float x0, z0, x1, z1;
                bounds(&g->sec[t].foot.outer, &x0, &z0, &x1, &z1);
                hiA = maxf(hiA, alongX ? x1 : z1);
            }
            a = hiA + GAP;
            n++;
        }
    }
    return 1;
}

/* An elevated deck, carried by legs and by the vessels that pass through it,
 * with smaller vessels under it. */
static int layout_deck(Plant *P, float rx0, float rz0, float rx1, float rz1) {
    Rng *r = P->r;
    BGraph *g = P->g;
    Ctx *c = P->c;
    float W = rx1 - rx0, D = rz1 - rz0;
    if (W < 16.0f || D < 14.0f) return 0;
    float dw = rng_range(r, 12.0f, minf(24.0f, W - 4.0f)), dd = rng_range(r, 10.0f, minf(20.0f, D - 4.0f));
    float cx = (rx0 + rx1) * 0.5f + (W - dw - 4.0f) * 0.5f * rng_range(r, -0.6f, 0.6f);
    float cz = (rz0 + rz1) * 0.5f + (D - dd - 4.0f) * 0.5f * rng_range(r, -0.6f, 0.6f);
    float yd = g->tier == PT_COLOSSAL ? rng_range(r, 13.0f, 22.0f) : rng_range(r, 7.0f, 11.0f);
    FootSpec s;
    memset(&s, 0, sizeof(s));
    s.fam = FAM_CHAMFER; s.w = dw; s.d = dd; s.p1 = 1.6f / minf(dw, dd); s.cx = cx; s.cz = cz;
    BFoot f;
    make_foot(&s, &f);
    if (!clear_of(P, &f.outer, G0 + yd - 1.0f, G0 + yd, GAP, -1)) return 0;
    int di = add_sec(c, SEC_DECK, G0 + yd - 1.0f, G0 + yd, &f, -1, -1);
    if (di < 0) return 0;
    BSection *ds = &g->sec[di];
    ds->arr = ARR_DECK; ds->unit = IU_NONE; ds->paint = (uint8_t)(g->style == PST_GOTHIC ? PA_CREAM : steel_paint(P));
    ds->palette = proxy_palette(ds->paint); ds->ledTop = 0; ds->fins = 0; ds->bands = 0;
    P->deck = di;
    blog_printf(P->log, "  deck: %.0fx%.0f m at %.1f m\n", dw, dd, yd);
    /* carrier vessels through it */
    int nc = rng_int(r, 1, P->simplify ? 1 : 3);
    for (int k = 0; k < nc && g->nhole < BG_MAX_HOLE; ++k) {
        for (int tries = 0; tries < 16; ++tries) {
            float rad = rng_range(r, 1.6f, 3.2f), hole = rad + 0.6f, m = hole + 1.0f;
            if (dw < 2.0f * m || dd < 2.0f * m) break;
            float vx = rng_range(r, cx - dw * 0.5f + m, cx + dw * 0.5f - m), vz = rng_range(r, cz - dd * 0.5f + m, cz + dd * 0.5f - m);
            float h = (yd + rng_range(r, 5.0f, 14.0f)) * (g->tier == PT_COLOSSAL ? 1.0f + (g->rise - 1.0f) * 0.5f : 1.0f);
            h = minf(maxf(h, yd + 4.0f), P->cap);
            if (h / (2.0f * rad) > SLIM_COLUMN) rad = h / (2.0f * SLIM_COLUMN);
            hole = rad + 0.6f;
            if (vx - hole < cx - dw * 0.5f + 1.0f || vx + hole > cx + dw * 0.5f - 1.0f || vz - hole < cz - dd * 0.5f + 1.0f || vz + hole > cz + dd * 0.5f - 1.0f) continue;
            BFoot vf;
            drum_foot(&vf, vx, vz, rad);
            if (!clear_of(P, &vf.outer, G0, G0 + h, GAP + 1.2f, di)) continue;
            int unit = g->style == PST_GOTHIC || rng_float(r) < 0.4f ? IU_DOME : IU_COLUMN;
            int vi = add_unit(P, unit, &vf, h, IH_DOME, vessel_paint(P, r));
            if (vi < 0) break;
            BHole *hl = &g->hole[g->nhole++];
            hl->deck = di; hl->unit = vi; hl->x = vx; hl->z = vz; hl->r = hole;
            blog_printf(P->log, "  carrier %s: %.1f m across, %.0f m, through the deck\n", UNIT_NAMES[unit], 2.0f * rad, h);
            break;
        }
    }
    /* legs round the edge, then inside wherever a point is out of reach */
    const BLoop *dl = &f.outer;
    for (int k = 0; k < dl->n; ++k) {
        int k1 = (k + 1) % dl->n;
        float ex = dl->x[k1] - dl->x[k], ez = dl->z[k1] - dl->z[k], len = sqrtf(ex * ex + ez * ez);
        int n = (int)ceilf(len / 8.0f);
        for (int j = 0; j < n; ++j) {
            float t = (float)j / (float)n;
            float lx = dl->x[k] + ex * t, lz = dl->z[k] + ez * t;
            lx += (cx - lx) * 0.6f / maxf(1.0f, sqrtf((cx - lx) * (cx - lx) + (cz - lz) * (cz - lz)));
            lz += (cz - lz) * 0.6f / maxf(1.0f, sqrtf((cx - lx) * (cx - lx) + (cz - lz) * (cz - lz)));
            int inV = 0;
            for (int h = 0; h < g->nhole; ++h) inV |= (lx - g->hole[h].x) * (lx - g->hole[h].x) + (lz - g->hole[h].z) * (lz - g->hole[h].z) < (g->hole[h].r + 0.4f) * (g->hole[h].r + 0.4f);
            for (int t = 0; t < g->nsec && !inV; ++t)
                inV = t != di && g->sec[t].y0 <= G0 + 0.01f && bfoot_boundary_dist(&g->sec[t].foot, lx, lz, NULL, NULL) < 0.8f + (bfoot_contains(&g->sec[t].foot, lx, lz) ? 1e3f : 0.0f);
            if (!inV) add_support(c, SUP_LEG, di, lx, G0, lz, lx, G0 + yd - 1.0f, lz, 0.45f);
        }
    }
    for (float z = cz - dd * 0.5f + 3.0f; z < cz + dd * 0.5f - 2.0f; z += 6.0f)
        for (float x = cx - dw * 0.5f + 3.0f; x < cx + dw * 0.5f - 2.0f; x += 6.0f) {
            float near = 1e30f;
            for (int k = 0; k < g->nsup; ++k)
                if (g->sup[k].section == di) near = minf(near, sqrtf((g->sup[k].x0 - x) * (g->sup[k].x0 - x) + (g->sup[k].z0 - z) * (g->sup[k].z0 - z)));
            for (int h = 0; h < g->nhole; ++h)
                near = minf(near, sqrtf((g->hole[h].x - x) * (g->hole[h].x - x) + (g->hole[h].z - z) * (g->hole[h].z - z)) - g->hole[h].r);
            if (near < REACH * 0.7f) continue;
            int inV = 0;
            for (int h = 0; h < g->nhole; ++h) inV |= (x - g->hole[h].x) * (x - g->hole[h].x) + (z - g->hole[h].z) * (z - g->hole[h].z) < (g->hole[h].r + 1.0f) * (g->hole[h].r + 1.0f);
            for (int t = 0; t < g->nsec && !inV; ++t)
                inV = t != di && g->sec[t].y0 <= G0 + 0.01f && bfoot_boundary_dist(&g->sec[t].foot, x, z, NULL, NULL) < 0.8f + (bfoot_contains(&g->sec[t].foot, x, z) ? 1e3f : 0.0f);
            if (!inV) add_support(c, SUP_LEG, di, x, G0, z, x, G0 + yd - 1.0f, z, 0.45f);
        }
    /* under it: small vessels below the girders */
    int nu = P->simplify ? 0 : rng_int(r, 0, 2);
    for (int k = 0; k < nu; ++k)
        for (int tries = 0; tries < 10; ++tries) {
            float ux = rng_range(r, cx - dw * 0.5f + 2.0f, cx + dw * 0.5f - 2.0f), uz = rng_range(r, cz - dd * 0.5f + 2.0f, cz + dd * 0.5f - 2.0f);
            int si = rng_float(r) < 0.5f ? propose_tank(P, ux, uz, 3.0f, yd - 1.0f - 1.5f)
                                         : propose_exchanger(P, ux, uz, rng_float(r) < 0.5f, 4.0f, 1.5f, yd - 1.0f - 1.5f);
            if (si >= 0) break;
        }
    /* a stair to it, or a caged ladder up to a high one (placed with the other access) */
    return 1;
}

/* Units round the core in what ground is left. */
static void flanking(Plant *P, float rx0, float rz0, float rx1, float rz1, int want) {
    Rng *r = P->r;
    int placed = 0;
    for (int tries = 0; tries < 40 && placed < want; ++tries) {
        float cx = rng_range(r, rx0 + 2.0f, rx1 - 2.0f), cz = rng_range(r, rz0 + 2.0f, rz1 - 2.0f);
        float room = 6.0f;
        if (propose_unit(P, cx, cz, rng_float(r) < 0.5f, room, room, 99.0f) >= 0) placed++;
    }
}

/* ===========================================================================
 * Access and pipes
 * ========================================================================= */

static int foot_free(const Plant *P, float x, float z, float half, int skip) {
    BLoop l;
    rect_loop(&l, x - half, z - half, x + half, z + half);
    return clear_of(P, &l, G0, G0 + 2.5f, 0.3f, skip);
}

static int add_access(Plant *P, int type, int sec, float x, float z, float y1, float nx, float nz) {
    BGraph *g = P->g;
    if (g->nacc >= BG_MAX_ACCESS) return 0;
    BAccess *a = &g->acc[g->nacc++];
    a->type = type; a->section = sec; a->x = x; a->z = z; a->y1 = y1; a->nx = nx; a->nz = nz;
    return 1;
}

/* A ladder up the side of section si to y1, on whichever side has free ground at its foot. */
static int ladder_to(Plant *P, int si, float y1) {
    BGraph *g = P->g;
    const BSection *s = &g->sec[si];
    float cx, cz, rad;
    int round = s->unit != IU_FRAME && s->type != SEC_DECK;
    float x0, z0, x1, z1;
    bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
    bloop_circle(&s->foot.outer, &cx, &cz, &rad);
    if (!round) { cx = (x0 + x1) * 0.5f; cz = (z0 + z1) * 0.5f; }
    int k0 = rng_int(P->r, 0, 7);
    for (int t = 0; t < 8; ++t) {
        float a = (float)((k0 + t) & 7) * PI_F * 0.25f, nx = cosf(a), nz = sinf(a), px, pz;
        if (round) { px = cx + nx * (rad + 0.45f); pz = cz + nz * (rad + 0.45f); }
        else {
            if (((k0 + t) & 1)) continue;   /* a frame's or deck's straight sides */
            float along = rng_range(P->r, -0.3f, 0.3f);
            px = nx > 0.5f ? x1 + 0.45f : nx < -0.5f ? x0 - 0.45f : cx + along * (x1 - x0);
            pz = nz > 0.5f ? z1 + 0.45f : nz < -0.5f ? z0 - 0.45f : cz + along * (z1 - z0);
        }
        if (!foot_free(P, px, pz, 0.4f, si)) continue;
        return add_access(P, ACC_LADDER, si, px, pz, y1, nx, nz);
    }
    return 0;
}

/* A straight stair up beside one of the deck's edges, its foot on free ground. */
static int stair_to(Plant *P, int di) {
    BGraph *g = P->g;
    const BSection *s = &g->sec[di];
    float x0, z0, x1, z1;
    bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
    float y = s->y1 - G0, run = y * 1.15f;
    int k0 = rng_int(P->r, 0, 7);
    for (int t = 0; t < 8; ++t) {
        int k = (k0 + t) & 7, side = k & 3, dir = k >> 2 ? 1 : -1;
        /* side: the edge (S, N, W, E) the stair runs along, 0.4..1.6 m out from it; dir: which way it climbs */
        float ex = side < 2 ? 1.0f : 0.0f, ez = side < 2 ? 0.0f : 1.0f;
        float topU = side < 2 ? (dir > 0 ? x1 - 1.0f : x0 + 1.0f) : (dir > 0 ? z1 - 1.0f : z0 + 1.0f);
        float off = side == 0 ? z0 - 1.0f : side == 1 ? z1 + 1.0f : side == 2 ? x0 - 1.0f : x1 + 1.0f;
        float tx = side < 2 ? topU : off, tz = side < 2 ? off : topU;
        float fx = tx - ex * (float)dir * run, fz = tz - ez * (float)dir * run;
        float sx0 = minf(fx, tx) - (side < 2 ? 0.0f : 0.6f), sx1 = maxf(fx, tx) + (side < 2 ? 0.0f : 0.6f);
        float sz0 = minf(fz, tz) - (side < 2 ? 0.6f : 0.0f), sz1 = maxf(fz, tz) + (side < 2 ? 0.6f : 0.0f);
        BLoop l;
        rect_loop(&l, sx0, sz0, sx1, sz1);
        if (!clear_of(P, &l, G0, G0 + y - 1.2f, 0.3f, di)) continue;
        BLoop fl;   /* and the foot, with room to step off */
        rect_loop(&fl, fx - 1.0f - (side < 2 ? 0.0f : 0.0f), fz - 1.0f, fx + 1.0f, fz + 1.0f);
        if (!clear_of(P, &fl, G0, G0 + 2.0f, 0.2f, di)) continue;
        if (!add_access(P, ACC_STAIR, di, fx, fz, s->y1, ex * (float)dir, ez * (float)dir)) return 0;
        if (P->nkeep < 12) P->keep[P->nkeep++] = l;
        return 1;
    }
    return 0;
}

/* Is this a unit pipes run from? */
static int process_unit(const BSection *s) {
    return s->type == SEC_UNIT && (s->unit == IU_COLUMN || s->unit == IU_TANK || s->unit == IU_DOME || s->unit == IU_EXCHANGER);
}

/* Where a pipe leaves section s towards (tx, tz), at height y: on its shell, and a stub out from it. */
static void port_of(const BSection *s, float tx, float tz, float *px, float *pz, float *sx, float *sz) {
    float cx, cz, rad;
    if (s->unit == IU_EXCHANGER || s->unit == IU_RACK || s->type == SEC_DECK || bsec_facade(s)) {
        /* a box: the nearest point of its outline, a stub straight out */
        float qx, qz;
        loop_dist2(&s->foot.outer, tx, tz, &qx, &qz, 1e30f);
        float dx = tx - qx, dz = tz - qz;
        if (fabsf(dx) > fabsf(dz)) { dx = dx > 0 ? 1.0f : -1.0f; dz = 0.0f; } else { dz = dz > 0 ? 1.0f : -1.0f; dx = 0.0f; }
        *px = qx; *pz = qz; *sx = qx + dx * 0.8f; *sz = qz + dz * 0.8f;
        return;
    }
    bloop_circle(&s->foot.outer, &cx, &cz, &rad);
    float dx = tx - cx, dz = tz - cz;
    /* leave square to the plot's axes, so the run stays orthogonal */
    if (fabsf(dx) > fabsf(dz)) { dx = dx > 0 ? 1.0f : -1.0f; dz = 0.0f; } else { dz = dz > 0 ? 1.0f : -1.0f; dx = 0.0f; }
    *px = cx + dx * rad; *pz = cz + dz * rad;
    *sx = cx + dx * (rad + 0.8f); *sz = cz + dz * (rad + 0.8f);
}

/* Whether a pipe's run is clear of every unit but where it starts and ends,
 * keeps off the lanes at the foot, and is held up often enough. */
static int pipe_ok(const BGraph *g, const BPipe *p, float *why) {
    for (int k = 0; k + 1 < p->n; ++k)
        for (int i = 0; i < g->nsec; ++i) {
            const BSection *s = &g->sec[i];
            if ((i == p->a && k <= 1) || (i == p->b && k >= p->n - 3)) continue;
            if (bsec_open(s) && s->unit != IU_FLARE) continue;   /* through a rack or frame */
            if (seg_hits(s, p->x[k], p->y[k], p->z[k], p->x[k + 1], p->y[k + 1], p->z[k + 1], p->r)) { if (why) *why = (float)i; return 0; }
        }
    float longest;
    float posts[32][4];
    bgen_pipe_posts(g, p, posts, 32, &longest);
    if (longest > PIPE_SPAN_MAX) { if (why) *why = -longest; return 0; }
    return 1;
}

static int route(Plant *P, int a, int b, float H, int order, float r, BPipe *p) {
    BGraph *g = P->g;
    const BSection *A = &g->sec[a], *B = &g->sec[b];
    float acx, acz, bcx, bcz, t;
    bloop_circle(&A->foot.outer, &acx, &acz, &t);
    bloop_circle(&B->foot.outer, &bcx, &bcz, &t);
    float pax, paz, sax, saz, pbx, pbz, sbx, sbz;
    port_of(A, bcx, bcz, &pax, &paz, &sax, &saz);
    port_of(B, acx, acz, &pbx, &pbz, &sbx, &sbz);
    float ya = minf(H, A->y1 - (A->unit == IU_EXCHANGER ? 0.6f : 1.2f)), yb = minf(H, B->y1 - (B->unit == IU_EXCHANGER ? 0.6f : 1.2f));
    if (B->unit == IU_RACK) { yb = H; sbx = pbx; sbz = pbz; }
    ya = maxf(ya, G0 + 0.8f); yb = maxf(yb, G0 + 0.8f);
    float pts[8][3] = { { pax, ya, paz }, { sax, ya, saz }, { sax, H, saz }, { order ? sbx : sax, H, order ? saz : sbz },
                        { sbx, H, sbz }, { sbx, yb, sbz }, { pbx, yb, pbz } };
    memset(p, 0, sizeof(*p));
    p->kind = PIPE_PROCESS; p->a = a; p->b = b; p->r = r;
    for (int k = 0; k < 7; ++k) {
        if (p->n && fabsf(pts[k][0] - p->x[p->n - 1]) < 0.01f && fabsf(pts[k][1] - p->y[p->n - 1]) < 0.01f && fabsf(pts[k][2] - p->z[p->n - 1]) < 0.01f) continue;
        if (p->n >= 2) {   /* straight on: drop the middle point */
            float d1x = p->x[p->n - 1] - p->x[p->n - 2], d1y = p->y[p->n - 1] - p->y[p->n - 2], d1z = p->z[p->n - 1] - p->z[p->n - 2];
            float d2x = pts[k][0] - p->x[p->n - 1], d2y = pts[k][1] - p->y[p->n - 1], d2z = pts[k][2] - p->z[p->n - 1];
            float cx = d1y * d2z - d1z * d2y, cy = d1z * d2x - d1x * d2z, cz = d1x * d2y - d1y * d2x;
            if (cx * cx + cy * cy + cz * cz < 1e-6f && d1x * d2x + d1y * d2y + d1z * d2z > 0.0f) p->n--;
        }
        if (p->n >= BG_PIPE_PV) return 0;
        p->x[p->n] = pts[k][0]; p->y[p->n] = pts[k][1]; p->z[p->n] = pts[k][2]; p->n++;
    }
    return p->n >= 2 && pipe_ok(g, p, NULL);
}

static int connect_units(Plant *P, int a, int b, const float *heights, int nh) {
    BGraph *g = P->g;
    if (g->npipe >= BG_MAX_PIPE) return 0;
    float r = g->style == PST_GOTHIC ? rng_range(P->r, 0.15f, 0.35f) : rng_range(P->r, 0.1f, 0.25f);
    for (int k = 0; k < nh; ++k)
        for (int order = 0; order < 2; ++order) {
            BPipe p;
            if (route(P, a, b, heights[k], order, r, &p)) { g->pipe[g->npipe++] = p; return 1; }
        }
    blog_printf(P->log, "  pipe from the %s to the %s dropped: no clear route\n", UNIT_NAMES[g->sec[a].unit], g->sec[b].type == SEC_DECK ? "deck" : UNIT_NAMES[g->sec[b].unit]);
    return 0;
}

static void place_pipes(Plant *P) {
    BGraph *g = P->g;
    float portTop = G0 + minf(PORT_MAX, 12.0f * g->rise);
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        if (!process_unit(s)) continue;
        int linked = 0;
        for (int k = 0; k < g->npipe; ++k) linked |= g->pipe[k].a == si || g->pipe[k].b == si;
        if (linked) continue;
        if (P->rack >= 0) {
            float hr = g->sec[P->rack].y1;
            float hs[3] = { hr - 0.5f, hr - 2.2f, hr + 1.0f };
            if (connect_units(P, si, P->rack, hs, 3)) continue;
        }
        /* the nearest other process unit, over or under the deck */
        float cx, cz, t, best = 1e30f;
        int to = -1;
        bloop_circle(&s->foot.outer, &cx, &cz, &t);
        for (int j = 0; j < g->nsec; ++j) {
            if (j == si || !process_unit(&g->sec[j])) continue;
            float jx, jz;
            bloop_circle(&g->sec[j].foot.outer, &jx, &jz, &t);
            float d = (jx - cx) * (jx - cx) + (jz - cz) * (jz - cz);
            if (d < best) { best = d; to = j; }
        }
        if (to < 0) continue;
        float hs[4], dy = P->deck >= 0 ? g->sec[P->deck].y1 : 0.0f;
        int nh = 0;
        if (P->deck >= 0) { hs[nh++] = dy + 2.5f; hs[nh++] = g->sec[P->deck].y0 - 0.9f; }
        hs[nh++] = minf(G0 + 4.5f, portTop);
        hs[nh++] = minf(G0 + 7.5f, portTop);
        connect_units(P, si, to, hs, nh);
    }
}

/* ===========================================================================
 * The plant
 * ========================================================================= */

int arch_plant(Ctx *c, int simplify) {
    BGraph *g = c->g;
    Rng *r = &c->rf;
    Plant P;
    yard_init(&P, c, simplify, FENCE_IN);
    if (P.len < 20.0f || P.depth < 18.0f) return 0;
    static const float SW[DIST_COUNT][PST_COUNT] = {   /* the district's bias over the styles */
        { 0.4f, 0.3f, 0.3f }, { 0.4f, 0.3f, 0.3f }, { 0.4f, 0.3f, 0.3f }, { 0.4f, 0.3f, 0.3f }, { 0.4f, 0.25f, 0.35f } };
    g->style = pick(&c->rs, SW[g->district], PST_COUNT);
    g->tier = PT_GROUND;
    g->rise = 1.0f;
    float colossal = c->H >= 140.0f ? 0.7f : c->H >= 100.0f ? 0.35f : 0.0f;   /* the plot's limit says where it may */
    if (simplify < 2 && rng_float(r) < colossal) { g->tier = PT_COLOSSAL; g->rise = rng_range(r, 2.6f, 3.9f); }
    P.cap = minf(TOP_CAP, c->H);
    g->groundMode = GF_RETAIL;
    blog_printf(c->log, "  plant: %s, %s tier (rise %.1f), yard %.0fx%.0f m\n", STYLE_NAMES[g->style],
                g->tier == PT_COLOSSAL ? "colossal" : "ground", g->rise, P.len, P.depth);
    /* the control building and the gate beside it */
    float bw = rng_range(r, 8.0f, 12.0f), bd = rng_range(r, 6.0f, 8.0f);
    if (place_control(&P, bw, bd, g->groundH, IU_CONTROL, 6.0f, minf(10.0f, P.depth * 0.35f)) < 0) {
        blog_printf(c->log, "  rejected: no room for the control building by the gate\n");
        return 0;
    }
    /* the free ground behind the street band */
    float fx0, fz0, fx1, fz1;
    uv_rect(&P, 0.0f, bd + 2.0f, P.len, P.depth, &fx0, &fz0, &fx1, &fz1);
    /* rarely, a gasholder; the core then takes the largest stretch of ground beside it */
    float cx0 = fx0, cz0 = fz0, cx1 = fx1, cz1 = fz1;
    float gw = g->style == PST_GAS ? 0.09f : 0.16f;
    if (simplify < 2 && rng_float(r) < gw) {
        int gi = place_gasholder(&P, fx0, fz0, fx1, fz1);
        if (gi >= 0) {
            float gx0, gz0, gx1, gz1;
            bounds(&g->sec[gi].foot.outer, &gx0, &gz0, &gx1, &gz1);
            float side[4][4] = { { fx0, fz0, gx0 - GAP, fz1 }, { gx1 + GAP, fz0, fx1, fz1 }, { fx0, fz0, fx1, gz0 - GAP }, { fx0, gz1 + GAP, fx1, fz1 } };
            float best = 0.0f;
            for (int k = 0; k < 4; ++k) {
                float a = maxf(side[k][2] - side[k][0], 0.0f) * maxf(side[k][3] - side[k][1], 0.0f);
                if (a > best) { best = a; cx0 = side[k][0]; cz0 = side[k][1]; cx1 = side[k][2]; cz1 = side[k][3]; }
            }
        }
    }
    /* the core */
    float pd = g->style == PST_GOTHIC ? 0.7f : g->style == PST_GAS ? 0.25f : 0.5f;
    int deckFirst = rng_float(r) < pd;
    int core = deckFirst ? layout_deck(&P, cx0, cz0, cx1, cz1) : layout_rack(&P, cx0, cz0, cx1, cz1);
    if (!core) core = deckFirst ? layout_rack(&P, cx0, cz0, cx1, cz1) : layout_deck(&P, cx0, cz0, cx1, cz1);
    if (!core) { blog_printf(c->log, "  rejected: neither a rack nor a deck fits\n"); return 0; }
    if (P.deck >= 0) {   /* the deck's way up first, so nothing stands at its foot */
        const BSection *ds = &g->sec[P.deck];
        if (!(ds->y1 - G0 <= 12.0f && stair_to(&P, P.deck)) && !ladder_to(&P, P.deck, ds->y1))
            blog_printf(c->log, "  rejected: no way up to the deck\n");
    }
    float area = P.len * P.depth;   /* a heavy site has two to four works buildings, a light one none or one */
    int works = area >= 1500.0f && !simplify ? rng_int(r, 2, 4) : rng_int(r, 0, 1);
    place_works(&P, works);
    place_stack(&P);
    if (g->style == PST_NIGHT && !simplify && rng_float(r) < 0.5f) place_stack(&P);
    int flares = g->style == PST_NIGHT ? rng_int(r, 1, simplify ? 1 : 3) : rng_float(r) < (g->style == PST_GOTHIC ? 0.5f : 0.4f) ? 1 : 0;
    for (int k = 0; k < flares; ++k) place_flare(&P);
    flanking(&P, fx0, fz0, fx1, fz1, P.deck >= 0 ? rng_int(r, 1, simplify ? 2 : 4) : rng_int(r, 0, simplify ? 0 : 2));
    /* every elevated level reached (the deck's already), then pipes */
    for (int si = 0; si < g->nsec; ++si) {
        const BSection *s = &g->sec[si];
        if (s->unit == IU_FRAME || s->unit == IU_GASHOLDER || (s->unit == IU_TANK && s->y1 - G0 >= 3.0f))
            if (!ladder_to(&P, si, s->y1)) blog_printf(c->log, "  rejected: no free ground at the foot of a ladder up the %s\n", UNIT_NAMES[s->unit]);
    }
    place_pipes(&P);
    int units = 0;
    for (int i = 0; i < g->nsec; ++i) units += g->sec[i].type == SEC_UNIT;
    blog_printf(c->log, "  %d units, %d pipes, %d ways up\n", units, g->npipe, g->nacc);
    return 1;
}

/* ===========================================================================
 * The substation
 * ========================================================================= */

int arch_substation(Ctx *c, int simplify) {
    BGraph *g = c->g;
    Rng *r = &c->rf;
    Plant P;
    yard_init(&P, c, simplify, 0.6f);
    float pw = minf(rng_range(r, 35.0f, 55.0f), P.len - 1.0f), pd = minf(rng_range(r, 27.0f, 34.0f), P.depth - 1.0f);
    if (pw < 32.0f || pd < 25.0f) { blog_printf(c->log, "  rejected: no room for a %.0fx%.0f m substation pad\n", pw, pd); return 0; }
    /* the pad on the street side, its fence round it */
    float u0 = (P.len - pw) * 0.5f;
    float x0, z0, x1, z1;
    uv_rect(&P, u0, 0.0f, u0 + pw, pd, &x0, &z0, &x1, &z1);
    P.x0 = x0; P.z0 = z0; P.x1 = x1; P.z1 = z1;
    g->fence[0] = x0; g->fence[1] = z0; g->fence[2] = x1; g->fence[3] = z1;
    /* re-frame on the pad */
    float px, pz;
    at(&P, u0, 0.0f, &px, &pz);
    P.ox = px; P.oz = pz;
    P.len = pw; P.depth = pd;
    static const float SW[PST_COUNT] = { 0.5f, 0.2f, 0.3f };
    g->style = pick(&c->rs, SW, PST_COUNT);
    g->tier = PT_GROUND; g->rise = 1.0f;
    g->groundMode = GF_RETAIL;
    blog_printf(c->log, "  substation: %s, pad %.0fx%.0f m\n", STYLE_NAMES[g->style], pw, pd);
    /* the switch house one side of the gate, the kiosk the other */
    float sw = rng_range(r, 8.0f, 12.0f), sd = rng_range(r, 6.0f, 8.0f);
    int sh = place_control(&P, sw, sd, g->groundH, IU_SWITCH, 5.0f, pd * 0.42f);
    if (sh < 0) { blog_printf(c->log, "  rejected: no room for the switch house\n"); return 0; }
    {
        float kx0, kz0, kx1, kz1;
        float shu0, shv0, shu1, shv1;   /* which side of the gate the switch house took */
        bounds(&g->sec[sh].foot.outer, &shu0, &shv0, &shu1, &shv1);
        float scx = (shu0 + shu1) * 0.5f, scz = (shv0 + shv1) * 0.5f, gx, gz;
        at(&P, g->gateU, 0.0f, &gx, &gz);
        int right = (scx - gx) * P.ux + (scz - gz) * P.uz > 0.0f;
        float ku0 = right ? g->gateU - 3.0f - 1.5f - 3.5f : g->gateU + 3.0f + 1.5f;
        uv_rect(&P, ku0, 0.0f, ku0 + 3.5f, 2.6f, &kx0, &kz0, &kx1, &kz1);
        BFoot kf;
        rect_foot(&kf, kx0, kz0, kx1, kz1);
        if (ku0 > 0.3f && ku0 + 3.5f < pw - 0.3f && clear_of(&P, &kf.outer, G0, G0 + 2.8f, GAP, -1))
            add_building(&P, IU_KIOSK, kx0, kz0, kx1, kz1, 2.8f, ROOF_FLAT);
    }
    /* the transformers in a row across the middle, each in its bund */
    int n = simplify >= 2 ? 1 : rng_int(r, 1, 3);
    float tw = rng_range(r, 3.5f, 5.0f), td = rng_range(r, 2.5f, 3.5f), th = rng_range(r, 3.2f, 4.2f);
    float m = BUND_MARGIN + rng_range(r, 0.0f, 0.5f), bw = tw + 2.0f * m, bdp = td + 2.0f * m;
    float gap = rng_float(r) < 0.6f ? rng_range(r, BUND_GAP + 0.5f, FIREWALL_GAP - 1.0f) : rng_range(r, FIREWALL_GAP, FIREWALL_GAP + 3.0f);
    while (n > 1 && (float)n * bw + (float)(n - 1) * gap > pw - 6.0f) { blog_printf(c->log, "  transformer dropped: the row runs off the pad\n"); n--; }
    if (bw > pw - 6.0f) return 0;
    float row = (float)n * bw + (float)(n - 1) * gap, ru0 = (pw - row) * 0.5f, vmid = pd * 0.56f;
    float ycond = G0 + th + 1.6f;
    int tr[3], nt = 0;
    for (int k = 0; k < n; ++k) {
        float ua = ru0 + (float)k * (bw + gap);
        float bx0, bz0, bx1, bz1;
        uv_rect(&P, ua, vmid - bdp * 0.5f, ua + bw, vmid + bdp * 0.5f, &bx0, &bz0, &bx1, &bz1);
        BFoot f;
        rect_foot(&f, bx0, bz0, bx1, bz1);
        if (!clear_of(&P, &f.outer, G0, ycond, GAP, -1)) { blog_printf(c->log, "  transformer moved off: it would block the gate or the switch house\n"); continue; }
        int si = add_unit(&P, IU_TRANSFORMER, &f, ycond - G0, IH_FLAT, g->style == PST_GAS ? PA_BLUE : g->style == PST_GOTHIC ? PA_GREEN : PA_STEEL);
        if (si < 0) break;
        g->sec[si].fill = m;   /* the bund's margin round the tank */
        tr[nt++] = si;
        if (k > 0 && gap < FIREWALL_GAP) {
            float fu = ua - gap * 0.5f;
            float wx0, wz0, wx1, wz1;
            uv_rect(&P, fu - 0.2f, vmid - bdp * 0.5f - 0.5f, fu + 0.2f, vmid + bdp * 0.5f + 0.5f, &wx0, &wz0, &wx1, &wz1);
            BFoot wf;
            rect_foot(&wf, wx0, wz0, wx1, wz1);
            int wi = add_unit(&P, IU_FIREWALL, &wf, th + 1.0f, IH_FLAT, PA_PALE);
            (void)wi;
        }
    }
    if (!nt) return 0;
    /* the gantry along the back, its bays over every transformer */
    float hg = rng_range(r, 9.0f, 12.0f);
    float ga0 = maxf(0.6f, ru0 - 2.0f), ga1 = minf(pw - 0.6f, ru0 + row + 2.0f);
    float gv0 = pd - 4.0f, gv1 = pd - 1.0f;
    float gx0, gz0, gx1, gz1;
    uv_rect(&P, ga0, gv0, ga1, gv1, &gx0, &gz0, &gx1, &gz1);
    BFoot gf;
    rect_foot(&gf, gx0, gz0, gx1, gz1);
    if (!clear_of(&P, &gf.outer, G0, G0 + hg, GAP, -1)) return 0;
    int gi = add_unit(&P, IU_GANTRY, &gf, hg, IH_FLAT, PA_GALV);
    if (gi < 0) return 0;
    int bays = (int)ceilf((ga1 - ga0) / GANTRY_BAY);
    g->sec[gi].posts = (uint8_t)bays;
    for (int k = 0; k <= bays; ++k) {
        float u = ga0 + 0.3f + (ga1 - ga0 - 0.6f) * (float)k / (float)bays;
        for (int sd2 = 0; sd2 < 2; ++sd2) {
            float x, z;
            at(&P, u, sd2 ? gv1 - 0.3f : gv0 + 0.3f, &x, &z);
            add_support(c, SUP_LEG, gi, x, G0, z, x, G0 + hg, z, 0.4f);
        }
    }
    /* three phases from each transformer's bushings up to the gantry */
    for (int k = 0; k < nt && g->npipe + 3 <= BG_MAX_PIPE; ++k) {
        float bx0, bz0, bx1, bz1;
        bounds(&g->sec[tr[k]].foot.outer, &bx0, &bz0, &bx1, &bz1);
        float tcx = (bx0 + bx1) * 0.5f, tcz = (bz0 + bz1) * 0.5f;
        for (int ph = -1; ph <= 1; ++ph) {
            BPipe *p = &g->pipe[g->npipe++];
            memset(p, 0, sizeof(*p));
            p->kind = PIPE_CONDUCTOR; p->a = tr[k]; p->b = gi; p->r = 0.04f; p->n = 2;
            float off = (float)ph * minf(1.0f, tw * 0.3f);
            p->x[0] = tcx + P.ux * off + P.ix * td * 0.3f; p->z[0] = tcz + P.uz * off + P.iz * td * 0.3f; p->y[0] = ycond - 0.1f;
            float ex, ez;
            at(&P, 0.0f, gv0 + 0.3f, &ex, &ez);
            /* straight back to the gantry's front beam */
            p->x[1] = P.ux != 0.0f ? p->x[0] : ex; p->z[1] = P.uz != 0.0f ? p->z[0] : ez; p->y[1] = G0 + hg - 0.8f;
        }
    }
    blog_printf(c->log, "  %d transformer(s), gantry of %d bays over them, %s\n", nt, bays, gap < FIREWALL_GAP && nt > 1 ? "firewalls between" : "no firewalls needed");
    return 1;
}

/* ===========================================================================
 * Validation (section 11.4, 11.5 and 11.8)
 * ========================================================================= */

#define REJECT(...) do { blog_printf(log, "  rejected: " __VA_ARGS__); blog_printf(log, "\n"); ok = 0; } while (0)

static float sec_diam(const BSection *s) { float cx, cz, r; bloop_circle(&s->foot.outer, &cx, &cz, &r); return 2.0f * r; }

static int check_units(const BGraph *g, const Building *b, BLog *log) {
    int ok = 1;
    float bay = clampf(bgen_bay_width(g->seed, 0), 2.5f, 3.6f);
    for (int i = 0; i < g->nsec && ok; ++i) {
        const BSection *s = &g->sec[i];
        float h = s->y1 - G0, x0, z0, x1, z1;
        bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
        float narrow = minf(x1 - x0, z1 - z0);
        switch (s->unit) {
        case IU_COLUMN: case IU_DOME: {
            float slim = h / sec_diam(s);
            if (!s->braceBy && slim > SLIM_COLUMN + 0.01f) REJECT("section %d: free-standing %s %.1f slender > %.1f", i, UNIT_NAMES[s->unit], slim, SLIM_COLUMN);
            else if (s->braceBy) {
                const BSection *fr = &g->sec[s->braceBy - 1];
                float cx, cz, r;
                bloop_circle(&s->foot.outer, &cx, &cz, &r);
                if (fr->unit != IU_FRAME || !bloop_inside(&fr->foot.outer, cx, cz)) REJECT("section %d: braced by something that is not its frame", i);
                else if (slim > SLIM_FRAMED + 0.01f) REJECT("section %d: framed %s %.1f slender > %.0f", i, UNIT_NAMES[s->unit], slim, SLIM_FRAMED);
                else if (fr->y1 - G0 < 2.0f * LEVEL - 0.01f) REJECT("section %d: tied at fewer than two levels", i);
            }
            break;
        }
        case IU_STACK:
            if (h / sec_diam(s) > SLIM_STACK + 0.01f) REJECT("section %d: stack %.1f slender > %.0f", i, h / sec_diam(s), SLIM_STACK);
            break;
        case IU_TANK:
            if (h > SQUAT_TANK * sec_diam(s) + 0.01f) REJECT("section %d: tank %.1f m on %.1f m: taller than %.1f times across", i, h, sec_diam(s), SQUAT_TANK);
            break;
        case IU_FLARE: {
            float base = sqrtf((s->foot.outer.x[1] - s->foot.outer.x[0]) * (s->foot.outer.x[1] - s->foot.outer.x[0]) +
                               (s->foot.outer.z[1] - s->foot.outer.z[0]) * (s->foot.outer.z[1] - s->foot.outer.z[0]));
            if (base < h / 10.0f - 0.01f) REJECT("section %d: flare base %.1f m under a tenth of its %.0f m", i, base, h);
            else if (base > FLARE_BASE + 0.01f) REJECT("section %d: flare base %.1f m > %.0f", i, base, FLARE_BASE);
            for (int j = 0; j < g->nsec && ok; ++j) {
                if (j == i) continue;
                float sep = flare_clearance(h) - 0.01f, d = loop_gap(&s->foot.outer, &g->sec[j].foot.outer);
                if (d < sep) REJECT("section %d: flare %.1f m from section %d, inside its %.0f m clearance", i, d, j, sep + 0.01f);
            }
            break;
        }
        case IU_GASHOLDER: {
            float d = sec_diam(s);
            if (h > d + 0.01f) REJECT("section %d: gasholder frame %.0f m on %.0f m across", i, h, d);
            else if (!s->posts || PI_F * d / (float)s->posts > POST_GAP + 0.01f) REJECT("section %d: gasholder guide posts more than %.0f m apart", i, POST_GAP);
            break;
        }
        case IU_HALL: case IU_WAREHOUSE:
            if (narrow > HALL_SPAN + 0.01f) REJECT("section %d: %s roof span %.1f m > %.0f", i, UNIT_NAMES[s->unit], narrow, HALL_SPAN);
            break;
        case IU_OFFICE: {
            if (s->type != SEC_GROUND) break;
            float top = s->y1;
            for (int j = 0; j < g->nsec; ++j) if (g->sec[j].nbelow && g->sec[j].below[0] == i) top = maxf(top, g->sec[j].y1);
            float hh = top - G0, lim = narrow * (g->tier == PT_COLOSSAL ? 10.0f : 4.5f), need = (hh > 40.0f ? 4.0f : 3.0f) * bay;
            if (hh > lim + 0.01f) REJECT("section %d: office block %.0f m on a %.1f m side, over %.1f times", i, hh, narrow, lim / narrow);
            else if (narrow < need - 0.01f) REJECT("section %d: office block %.1f m across, under %d bays", i, narrow, hh > 40.0f ? 4 : 3);
            break;
        }
        case IU_RACK: {
            int legs = 0;
            for (int k = 0; k < g->nsup; ++k) legs += g->sup[k].section == i;
            float len = maxf(x1 - x0, z1 - z0);
            if (legs < 4 || len / (float)(legs / 2 - 1) > BENT_MAX + 0.01f) REJECT("section %d: pipe rack bents more than %.0f m apart", i, BENT_MAX);
            break;
        }
        case IU_FRAME:
            if (h < 2.0f * LEVEL - 0.01f) REJECT("section %d: frame with fewer than two levels", i);
            break;
        default:
            break;
        }
        (void)b;
    }
    return ok;
}

/* A deck: every point within reach of a leg or a vessel collar, not overhanging
 * the hull of its supports too far, its weight inside that hull, its holes
 * clear of the vessels and inside its edge. */
static int check_deck(const BGraph *g, int i, BLog *log) {
    int ok = 1;
    const BSection *s = &g->sec[i];
    float px[BG_MAX_SUPP + BG_MAX_HOLE], pz[BG_MAX_SUPP + BG_MAX_HOLE], pr[BG_MAX_SUPP + BG_MAX_HOLE];
    int np = 0;
    for (int k = 0; k < g->nsup; ++k)
        if (g->sup[k].section == i && g->sup[k].type == SUP_LEG) {
            if (g->sup[k].y1 < s->y0 - 0.05f || g->sup[k].y0 > G0 + 0.01f) { REJECT("section %d: deck leg %d does not reach from the ground to it", i, k); return 0; }
            px[np] = g->sup[k].x0; pz[np] = g->sup[k].z0; pr[np] = 0.0f; np++;
        }
    for (int h = 0; h < g->nhole; ++h) {
        const BHole *hl = &g->hole[h];
        if (hl->deck != i) continue;
        const BSection *v = &g->sec[hl->unit];
        float cx, cz, r;
        bloop_circle(&v->foot.outer, &cx, &cz, &r);
        if (hl->r < r + 0.3f) REJECT("section %d: hole %.1f m round a %.1f m vessel leaves no clear ring", i, hl->r, r);
        else if (r < 1.4f || v->y1 < s->y1 + 2.0f) REJECT("section %d: vessel %d too small to carry the deck", i, hl->unit);
        for (int k = 0; k < 8 && ok; ++k) {
            float a = (float)k * PI_F * 0.25f, x = hl->x + cosf(a) * hl->r, z = hl->z + sinf(a) * hl->r;
            if (!bloop_inside(&s->foot.outer, x, z) || loop_dist2(&s->foot.outer, x, z, NULL, NULL, 1e30f) < 0.64f) REJECT("section %d: hole %d runs off the deck", i, h);
        }
        px[np] = cx; pz[np] = cz; pr[np] = r; np++;
    }
    if (!ok) return 0;
    if (np < 3) { REJECT("section %d: deck on %d supports", i, np); return 0; }
    /* the hull of the supports */
    BLoop hull;
    memset(&hull, 0, sizeof(hull));
    {
        int idx[BG_MAX_SUPP + BG_MAX_HOLE], m = 0, start = 0;
        for (int k = 1; k < np; ++k) if (px[k] < px[start] || (px[k] == px[start] && pz[k] < pz[start])) start = k;
        int cur = start;
        do {   /* gift wrapping: few points */
            idx[m++] = cur;
            int next = (cur + 1) % np;
            for (int k = 0; k < np; ++k) {
                float o = (px[next] - px[cur]) * (pz[k] - pz[cur]) - (pz[next] - pz[cur]) * (px[k] - px[cur]);
                if (o < 0.0f) next = k;
            }
            cur = next;
        } while (cur != start && m < np && m < BG_MAX_PV);
        for (int k = 0; k < m; ++k) loop_add(&hull, px[idx[k]], pz[idx[k]]);
    }
    float x0, z0, x1, z1, sx = 0.0f, sz = 0.0f;
    int cnt = 0;
    bounds(&s->foot.outer, &x0, &z0, &x1, &z1);
    for (float z = z0 + 0.5f; z < z1 && ok; z += 1.0f)
        for (float x = x0 + 0.5f; x < x1 && ok; x += 1.0f) {
            if (!bfoot_contains(&s->foot, x, z)) continue;
            int hole = 0;
            for (int h = 0; h < g->nhole; ++h) hole |= g->hole[h].deck == i && (x - g->hole[h].x) * (x - g->hole[h].x) + (z - g->hole[h].z) * (z - g->hole[h].z) < g->hole[h].r * g->hole[h].r;
            if (hole) continue;
            sx += x; sz += z; cnt++;
            float near = 1e30f;
            for (int k = 0; k < np; ++k) near = minf(near, sqrtf((px[k] - x) * (px[k] - x) + (pz[k] - z) * (pz[k] - z)) - pr[k]);
            if (near > REACH) REJECT("section %d: deck point %.1f m from its nearest support > %.0f", i, near, REACH);
            else if (hull.n >= 3 && !bloop_inside(&hull, x, z) && sqrtf(loop_dist2(&hull, x, z, NULL, NULL, 1e30f)) > DECK_OVER)
                REJECT("section %d: deck overhangs its supports by %.1f m > %.0f", i, sqrtf(loop_dist2(&hull, x, z, NULL, NULL, 1e30f)), DECK_OVER);
        }
    if (ok && cnt && hull.n >= 3 && !bloop_inside(&hull, sx / (float)cnt, sz / (float)cnt)) REJECT("section %d: the deck's weight lies outside its supports", i);
    return ok;
}

/* Pairs of units side by side keep an access gap; stacks keep clear of decks. */
static int check_clearance(const BGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nsec && ok; ++i)
        for (int j = i + 1; j < g->nsec && ok; ++j) {
            const BSection *a = &g->sec[i], *b = &g->sec[j];
            if (a->y1 <= b->y0 + 0.01f || b->y1 <= a->y0 + 0.01f) continue;
            if (industry_tied(g, i, j)) continue;
            if (a->nbelow && a->below[0] == j) continue;
            if (b->nbelow && b->below[0] == i) continue;
            if (a->unit == IU_FIREWALL || b->unit == IU_FIREWALL) continue;   /* it stands in the gap on purpose */
            float d = loop_gap(&a->foot.outer, &b->foot.outer), need = GAP_MIN;
            if ((a->unit == IU_STACK && b->type == SEC_DECK) || (b->unit == IU_STACK && a->type == SEC_DECK)) need = 3.0f;
            if (d < need - 0.01f) REJECT("sections %d and %d %.1f m apart < %.1f", i, j, d, need);
        }
    return ok;
}

/* The gate's lane: from the gate 6 m into the yard. */
static void gate_lane(const BGraph *g, BLoop *l) {
    float x0 = g->fence[0], z0 = g->fence[1], x1 = g->fence[2], z1 = g->fence[3], h = g->gateW * 0.5f, u = g->gateU;
    switch (g->gateSide) {
    case SIDE_S: rect_loop(l, x0 + u - h, z0 - 1.0f, x0 + u + h, z0 + 6.0f); break;
    case SIDE_N: rect_loop(l, x1 - u - h, z1 - 6.0f, x1 - u + h, z1 + 1.0f); break;
    case SIDE_W: rect_loop(l, x0 - 1.0f, z1 - u - h, x0 + 6.0f, z1 - u + h); break;
    default:     rect_loop(l, x1 - 6.0f, z0 + u - h, x1 + 1.0f, z0 + u + h); break;
    }
}

/* The fenced yard: every unit inside it; the gate lane clear. */
static int check_yard(const BGraph *g, BLog *log) {
    int ok = 1;
    BLoop lane;
    gate_lane(g, &lane);
    for (int i = 0; i < g->nsec && ok; ++i)
        if (g->sec[i].y0 <= G0 + 4.5f && loop_gap(&lane, &g->sec[i].foot.outer) < 0.05f) REJECT("section %d: in the gate's lane", i);
    for (int k = 0; k < g->nsup && ok; ++k)
        if (g->sup[k].y0 <= G0 + 0.01f && bloop_inside(&lane, g->sup[k].x0, g->sup[k].z0)) REJECT("support %d: in the gate's lane", k);
    for (int i = 0; i < g->nsec && ok; ++i) {
        const BLoop *l = &g->sec[i].foot.outer;
        for (int k = 0; k < l->n && ok; ++k)
            if (l->x[k] < g->fence[0] - 0.02f || l->x[k] > g->fence[2] + 0.02f || l->z[k] < g->fence[1] - 0.02f || l->z[k] > g->fence[3] + 0.02f)
                REJECT("section %d: outside the fence", i);
    }
    return ok;
}

static int check_access(const BGraph *g, BLog *log) {
    int ok = 1;
    for (int k = 0; k < g->nacc && ok; ++k) {
        const BAccess *a = &g->acc[k];
        if (a->x < g->fence[0] - 0.02f || a->x > g->fence[2] + 0.02f || a->z < g->fence[1] - 0.02f || a->z > g->fence[3] + 0.02f) REJECT("access %d: its foot is outside the fence", k);
        for (int i = 0; i < g->nsec && ok; ++i) {
            const BSection *s = &g->sec[i];
            if (s->y0 > G0 + 0.01f || bsec_open(s)) continue;
            if (bfoot_contains(&s->foot, a->x, a->z)) REJECT("access %d: its foot stands in section %d", k, i);
        }
    }
    for (int i = 0; i < g->nsec && ok; ++i) {
        const BSection *s = &g->sec[i];
        int needs = s->type == SEC_DECK || s->unit == IU_FRAME || s->unit == IU_GASHOLDER || (s->unit == IU_TANK && s->y1 - G0 >= 3.0f);
        if (!needs) continue;
        int has = 0;
        for (int k = 0; k < g->nacc; ++k) has |= g->acc[k].section == i && g->acc[k].y1 >= s->y1 - 0.01f;
        if (!has) REJECT("section %d: no ladder or stair up the %s", i, s->type == SEC_DECK ? "deck" : UNIT_NAMES[s->unit]);
    }
    return ok;
}

static int check_pipes(const BGraph *g, BLog *log) {
    int ok = 1;
    for (int k = 0; k < g->npipe && ok; ++k) {
        const BPipe *p = &g->pipe[k];
        if (p->a < 0 || p->b < 0 || p->a >= g->nsec || p->b >= g->nsec || p->n < 2) { REJECT("pipe %d: not between two units", k); break; }
        const BSection *A = &g->sec[p->a], *B = &g->sec[p->b];
        if (p->kind == PIPE_CONDUCTOR) {   /* from the bushings over the tank to the gantry's beam */
            if (!bfoot_contains(&A->foot, p->x[0], p->z[0]) || !bfoot_contains(&B->foot, p->x[1], p->z[1]) || p->y[1] > B->y1)
                REJECT("conductor %d: not from a transformer to its gantry", k);
            continue;
        }
        if (bfoot_boundary_dist(&A->foot, p->x[0], p->z[0], NULL, NULL) > 0.35f || p->y[0] < A->y0 || p->y[0] > A->y1)
            REJECT("pipe %d: does not start at a port on section %d", k, p->a);
        else if (p->kind == PIPE_PROCESS && (bfoot_boundary_dist(&B->foot, p->x[p->n - 1], p->z[p->n - 1], NULL, NULL) > 0.35f || p->y[p->n - 1] > B->y1 + 0.01f))
            REJECT("pipe %d: does not end at a port on section %d", k, p->b);
        else if (p->kind == PIPE_PROCESS) {
            float why = 0.0f;
            if (!pipe_ok(g, p, &why)) {
                if (why < 0.0f) REJECT("pipe %d: %.1f m between supports > %.0f", k, -why, PIPE_SPAN_MAX);
                else REJECT("pipe %d: runs through section %d", k, (int)why);
            }
        }
    }
    return ok;
}

/* A plant's primary door is on a control building, facing the street; every
 * other door opens onto free ground. */
static int check_entrances(const BGraph *g, const Building *b, BLog *log) {
    int ok = 1;
    for (int d = 0; d < g->ndoor && ok; ++d) {
        const BDoor *dr = &g->door[d];
        const BSection *s = &g->sec[dr->section];
        const BLoop *l = &s->foot.outer;
        int k1 = (dr->face + 1) % l->n;
        float ex = l->x[k1] - l->x[dr->face], ez = l->z[k1] - l->z[dr->face], len = sqrtf(ex * ex + ez * ez);
        float nx = -ez / len, nz = ex / len;
        if (dr->type == DOOR_PRIMARY) {
            if (s->unit != IU_CONTROL && s->unit != IU_SWITCH) { REJECT("door %d: the primary entrance is not on a control building", d); break; }
            int side = fabsf(nx) > fabsf(nz) ? (nx < 0 ? SIDE_W : SIDE_E) : (nz < 0 ? SIDE_S : SIDE_N);
            if (side != g->gateSide) REJECT("door %d: the control building's door does not face the street", d);
            float mx = l->x[dr->face] + ex * 0.5f, mz = l->z[dr->face] + ez * 0.5f;
            float gap = side == SIDE_W ? mx - b->foot.x0 : side == SIDE_E ? b->foot.x1 - mx : side == SIDE_S ? mz - b->foot.z0 : b->foot.z1 - mz;
            if (gap > 4.5f) REJECT("door %d: %.1f m back from the street", d, gap);
            continue;
        }
        BLoop ap;
        apron(s, dr->face, dr->u, dr->width, 3.0f, &ap);
        for (int i = 0; i < g->nsec && ok; ++i) {
            if (i == dr->section || g->sec[i].y0 > G0 + 3.0f) continue;
            if (loop_gap(&ap, &g->sec[i].foot.outer) < 0.05f) REJECT("door %d opens onto section %d", d, i);
        }
        for (int k = 0; k < g->nsup && ok; ++k)
            if (g->sup[k].type == SUP_LEG && g->sup[k].y0 <= G0 + 0.01f && bloop_inside(&ap, g->sup[k].x0, g->sup[k].z0)) REJECT("door %d opens onto a leg", d);
    }
    return ok;
}

/* Section 11.8: bunds, spacing and firewalls, the gantry over the transformers, the gate lane. */
static int check_substation(const BGraph *g, BLog *log) {
    int ok = 1;
    int gantry = -1, nt = 0, tr[8];
    for (int i = 0; i < g->nsec; ++i) {
        if (g->sec[i].unit == IU_GANTRY) gantry = i;
        if (g->sec[i].unit == IU_TRANSFORMER && nt < 8) tr[nt++] = i;
    }
    if (gantry < 0 || !nt) { REJECT("a substation needs transformers and a gantry"); return 0; }
    const BSection *G = &g->sec[gantry];
    float gx0, gz0, gx1, gz1;
    bounds(&G->foot.outer, &gx0, &gz0, &gx1, &gz1);
    int alongX = gx1 - gx0 >= gz1 - gz0;
    float glen = alongX ? gx1 - gx0 : gz1 - gz0;
    if (!G->posts || glen / (float)G->posts > GANTRY_BAY + 0.01f) REJECT("gantry bays %.1f m > %.0f", glen / (float)maxf(1.0f, (float)G->posts), GANTRY_BAY);
    for (int k = 0; k < nt && ok; ++k) {
        const BSection *t = &g->sec[tr[k]];
        float x0, z0, x1, z1;
        bounds(&t->foot.outer, &x0, &z0, &x1, &z1);
        if (t->fill < BUND_MARGIN - 0.01f) REJECT("section %d: bund %.1f m larger than its tank < %.1f", tr[k], t->fill, BUND_MARGIN);
        if (alongX ? (x0 < gx0 - 0.01f || x1 > gx1 + 0.01f) : (z0 < gz0 - 0.01f || z1 > gz1 + 0.01f)) REJECT("section %d: the gantry does not reach over it", tr[k]);
        int phases = 0;
        for (int p = 0; p < g->npipe; ++p) phases += g->pipe[p].kind == PIPE_CONDUCTOR && g->pipe[p].a == tr[k] && g->pipe[p].b == gantry;
        if (phases != 3) REJECT("section %d: %d phases to the gantry, not 3", tr[k], phases);
        for (int j = k + 1; j < nt && ok; ++j) {
            float d = loop_gap(&t->foot.outer, &g->sec[tr[j]].foot.outer);
            if (d < BUND_GAP - 0.01f) REJECT("sections %d and %d: bunds %.1f m apart < %.0f", tr[k], tr[j], d, BUND_GAP);
            else if (d < FIREWALL_GAP - 0.01f) {
                int wall = 0;
                for (int w = 0; w < g->nsec && !wall; ++w) {
                    if (g->sec[w].unit != IU_FIREWALL) continue;
                    float wx, wz, wr;
                    bloop_circle(&g->sec[w].foot.outer, &wx, &wz, &wr);
                    wall = loop_gap(&g->sec[w].foot.outer, &t->foot.outer) < d && loop_gap(&g->sec[w].foot.outer, &g->sec[tr[j]].foot.outer) < d &&
                           g->sec[w].y1 >= t->y1 - 2.0f;
                }
                if (!wall) REJECT("sections %d and %d: bunds %.1f m apart without a firewall", tr[k], tr[j], d);
            }
        }
    }
    return ok;
}

int industry_validate(const BGraph *g, const Building *b, BLog *log) {
    int ok = check_yard(g, log) && check_units(g, b, log) && check_clearance(g, log) && check_access(g, log) &&
             check_pipes(g, log) && check_entrances(g, b, log);
    for (int i = 0; i < g->nsec && ok; ++i) if (g->sec[i].type == SEC_DECK) ok = check_deck(g, i, log);
    if (ok && g->archetype == ARCH_SUBSTATION) ok = check_substation(g, log);
    return ok;
}
#undef REJECT
