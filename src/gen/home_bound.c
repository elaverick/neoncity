#include "home_char_internal.h"
#include "pmodel_retrofit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Boundaries and gates (residential roadmap HM5e3, ~/.claude/plans/residential-buildings.md "HM5e"; the user's concept
 * sheets in docs/reference/international-residential, panel 1 of each: docs/residential-target.md "Boundaries and
 * gates (HM5e3)").
 *
 *   the graph     home_bounds: the boundaries and gates the garden laid (HM2's HBound, HGate) given their style's
 *                 heights, tops and gates, from a stream of their own; a Korean villa's pilotis opened to the street;
 *                 a walled compound's gate kiosk; home_check_bounds: their rules
 *   the shell     home_shell.c: walls, kerbs and plinths, copings (China's tile cap), piers and their caps as solids
 *                 (the tall ones at both levels); China's gatehouse; the kiosk
 *   the dressing  hs_bounds: what stands on the walls (iron, spear-topped, steel and mesh railings, breeze-block
 *                 bands), the gates' leaves, lanterns on the gate piers, razor wire, a stoop's iron rails
 *
 * Only the styles with a concept sheet build their boundaries (HBoundSet.drawn); North America's others keep theirs
 * planned and undrawn, as the user chose (2026-11-20). Hedges, and the timber fences behind the houses, are HM7's.
 * ========================================================================= */

static int ch(uint32_t h, uint32_t k, float p) { return hash_float(hash_combine(h, k)) < p; }

const char *home_bound_name(int k) {
    static const char *const N[HB_COUNT] = { "none", "wall", "hedge", "fence", "railing" };
    return k >= 0 && k < HB_COUNT ? N[k] : "?";
}
const char *home_top_name(int k) {
    static const char *const N[HBT_COUNT] = { "none", "iron", "spear", "steel", "mesh", "breeze", "rings" };
    return k >= 0 && k < HBT_COUNT ? N[k] : "?";
}
const char *home_gate_name(int k) {
    static const char *const N[HGK_COUNT] = { "open", "iron", "bars", "sheet", "pierced", "mesh", "steel", "plank" };
    return k >= 0 && k < HGK_COUNT ? N[k] : "?";
}

/* ---------------------------------------------------------------------------
 * The styles: each sheet's panel 1 (the plan's "Panel 1, style by style", its boundary column)
 * ------------------------------------------------------------------------- */

typedef struct BKind {
    float   h0, h1;          /* the solid part: a wall, a railing's kerb or dwarf wall, a fence's plinth */
    uint8_t top;             /* HBT_* on it, on a row's chance `share` */
    float   share, th0, th1;
} BKind;
#define FIN_HOUSE 255        /* the walls' finish: the house's own (siding made render) */
typedef struct BDef {
    BKind   k[HB_COUNT];     /* by kind: HB_WALL, HB_FENCE, HB_RAILING */
    uint8_t fin;             /* HW_*, or FIN_HOUSE */
    const char *col;         /* its palette (a repeat weighs; NULL: the house's colour) */
    uint8_t coping;
    float   pier, pierW;     /* piers along the front every pier m (0: ends and gates), pierW square (0: no piers) */
    uint8_t gate, drive, leaves;         /* the path's gate, a drive's, the drive's leaves (HGL_*) */
    float   gh0, gh1, dh0, dh1;          /* their heights (0: the boundary's own) */
    float   lamps, razor, kiosk, gatehouse;
    uint8_t stoop;           /* iron rails up a stoop's steps */
} BDef;

static const BDef BDEFS[HS_COUNT] = {
    /* the brownstone: spear-topped area railings on a stone kerb along the street, iron rails up the stoop */
    [HS_ROWHOUSE]   = { .k[HB_RAILING] = { 0.25f, 0.4f, HBT_SPEAR, 1.0f, 0.9f, 1.1f }, .fin = HW_STONE, .col = "2",
                        .gate = HGK_IRON, .gh0 = 1.0f, .gh1 = 1.15f, .stoop = 1 },
    /* a low brick wall with spear-topped railings on it, or railings on a stone kerb; an iron gate */
    [HS_VICTORIAN]  = { .k[HB_WALL] = { 0.45f, 0.7f, HBT_SPEAR, 0.85f, 0.55f, 0.75f }, .k[HB_RAILING] = { 0.2f, 0.35f, HBT_SPEAR, 1.0f, 0.95f, 1.15f },
                        .fin = FIN_HOUSE, .pierW = 0.4f, .gate = HGK_IRON, .gh0 = 0.95f, .gh1 = 1.1f },
    /* area railings on the street line: tall, spear-topped, on a stone kerb; rails up the steps */
    [HS_GEORGIAN]   = { .k[HB_RAILING] = { 0.25f, 0.4f, HBT_SPEAR, 1.0f, 1.0f, 1.2f }, .fin = HW_STONE, .col = "01",
                        .gate = HGK_IRON, .gh0 = 1.1f, .gh1 = 1.3f, .stoop = 1 },
    /* a low brick wall with piers, short railings on about half */
    [HS_INTERWAR]   = { .k[HB_WALL] = { 0.6f, 0.9f, HBT_IRON, 0.45f, 0.3f, 0.45f }, .fin = FIN_HOUSE, .pier = 2.4f, .pierW = 0.4f,
                        .gate = HGK_IRON, .gh0 = 0.9f, .gh1 = 1.05f },
    /* the council estate's low brick walls, piers at the gates, steel railings on about half */
    [HS_POSTWAR]    = { .k[HB_WALL] = { 0.5f, 0.8f, HBT_IRON, 0.5f, 0.35f, 0.5f }, .fin = HW_BRICK, .pierW = 0.36f,
                        .gate = HGK_IRON, .gh0 = 0.9f, .gh1 = 1.0f },
    /* a rendered wall with piers, iron railings on it, an iron gate between lamp-lit piers */
    [HS_SEU_VILLA]  = { .k[HB_WALL] = { 0.7f, 1.0f, HBT_IRON, 0.8f, 0.5f, 0.8f }, .fin = FIN_HOUSE, .pier = 2.6f, .pierW = 0.45f,
                        .gate = HGK_IRON, .drive = HGK_IRON, .leaves = HGL_DOUBLE, .gh0 = 1.5f, .gh1 = 1.8f, .dh0 = 1.5f, .dh1 = 1.8f, .lamps = 0.7f },
    /* a block plinth under a galvanised mesh fence, a mesh gate; some a block wall */
    [HS_JAPAN]      = { .k[HB_WALL] = { 0.9f, 1.3f, HBT_NONE, 0.0f, 0.0f, 0.0f }, .k[HB_FENCE] = { 0.35f, 0.55f, HBT_MESH, 1.0f, 0.6f, 0.9f },
                        .fin = HW_BLOCK, .col = "001", .gate = HGK_MESH, .gh0 = 1.0f, .gh1 = 1.2f },
    /* the courtyard wall in grey brick under a tile cap, a gatehouse's roof over the red door */
    [HS_CHINA]      = { .k[HB_WALL] = { 2.0f, 2.5f, HBT_NONE, 0.0f, 0.0f, 0.0f }, .fin = HW_BRICK, .col = "6", .coping = HBC_TILE,
                        .gate = HGK_PLANK, .drive = HGK_SHEET, .leaves = HGL_DOUBLE, .gh0 = 2.1f, .gh1 = 2.3f, .dh0 = 1.9f, .dh1 = 2.1f, .gatehouse = 1.0f },
    /* the walk-up villa's low wall at the sides; the pilotis open to the street */
    [HS_KOREA]      = { .k[HB_WALL] = { 0.9f, 1.2f, HBT_NONE, 0.0f, 0.0f, 0.0f }, .fin = FIN_HOUSE, .pierW = 0.3f,
                        .gate = HGK_BARS, .gh0 = 1.1f, .gh1 = 1.3f },
    /* a low rendered wall with piers under painted steel railings, a sliding gate across the car porch */
    [HS_LINKHOUSE]  = { .k[HB_FENCE] = { 0.45f, 0.7f, HBT_STEEL, 1.0f, 0.9f, 1.1f }, .fin = FIN_HOUSE, .pierW = 0.4f,
                        .gate = HGK_STEEL, .drive = HGK_STEEL, .leaves = HGL_SLIDING, .lamps = 0.4f },
    /* a rendered wall, some with a pierced band, a steel gate */
    [HS_RCC]        = { .k[HB_WALL] = { 1.0f, 1.4f, HBT_BREEZE, 0.25f, 0.3f, 0.4f }, .fin = FIN_HOUSE, .pierW = 0.35f,
                        .gate = HGK_BARS, .drive = HGK_SHEET, .leaves = HGL_DOUBLE, .gh0 = 1.4f, .gh1 = 1.8f, .dh0 = 1.6f, .dh1 = 1.9f, .lamps = 0.2f },
    /* a low rendered wall under pierced blocks, lamp-lit piers, an iron gate */
    [HS_BUNGALOW]   = { .k[HB_WALL] = { 0.55f, 0.75f, HBT_RINGS, 0.9f, 0.35f, 0.45f }, .fin = FIN_HOUSE, .pier = 2.5f, .pierW = 0.45f,
                        .gate = HGK_IRON, .drive = HGK_IRON, .leaves = HGL_DOUBLE, .gh0 = 1.2f, .gh1 = 1.4f, .dh0 = 1.2f, .dh1 = 1.4f, .lamps = 0.8f },
    /* a high rendered wall with capped piers and lanterns, pierced steel gates, a pair across the drive */
    [HS_MENA_VILLA] = { .k[HB_WALL] = { 1.9f, 2.6f, HBT_NONE, 0.0f, 0.0f, 0.0f }, .fin = FIN_HOUSE, .pier = 3.0f, .pierW = 0.5f,
                        .gate = HGK_PIERCED, .drive = HGK_PIERCED, .leaves = HGL_DOUBLE, .gh0 = 2.0f, .gh1 = 2.4f, .dh0 = 2.2f, .dh1 = 2.6f, .lamps = 0.9f },
    /* a breeze-block wall on a solid plinth, steel sheet gates */
    [HS_BLOCK]      = { .k[HB_WALL] = { 0.25f, 0.4f, HBT_BREEZE, 1.0f, 0.9f, 1.2f }, .fin = HW_BLOCK, .col = "0122", .pierW = 0.35f,
                        .gate = HGK_SHEET, .drive = HGK_SHEET, .leaves = HGL_DOUBLE, .gh0 = 1.8f, .gh1 = 2.0f, .dh0 = 1.8f, .dh1 = 2.1f },
    /* the compound: a 2.4 m rendered wall, a breeze band on half, razor wire, steel sheet gates, the gate kiosk */
    [HS_WALLED]     = { .k[HB_WALL] = { 2.0f, 2.3f, HBT_BREEZE, 0.5f, 0.35f, 0.45f }, .fin = FIN_HOUSE, .pier = 3.0f, .pierW = 0.45f,
                        .gate = HGK_SHEET, .drive = HGK_SHEET, .leaves = HGL_DOUBLE, .gh0 = 2.0f, .gh1 = 2.2f, .dh0 = 2.2f, .dh1 = 2.5f,
                        .lamps = 0.7f, .razor = 0.8f, .kiosk = 0.6f },
};

float home_bound_thick(const HBound *b) { return b->kind == HB_RAILING ? BOUND_KERB : b->kind == HB_FENCE ? 0.2f : BOUND_T; }

static const HUnit *unit_of(const HGraph *g, const HBound *b) { return &g->unit[b->unit >= 0 && b->unit < g->nunit ? b->unit : 0]; }

void home_bound_rect(const HGraph *g, const HBound *b, float *x0, float *z0, float *x1, float *z1) {
    const float t = home_bound_thick(b);
    float a0 = fminf(b->x0, b->x1), a1 = fmaxf(b->x0, b->x1), c0 = fminf(b->z0, b->z1), c1 = fmaxf(b->z0, b->z1);
    if (c1 - c0 < 0.01f) {   /* along x: the street's line (z 0), or the back's */
        if (c0 < 0.5f) c1 = c0 + t;
        else c0 = c1 - t;
    } else {                 /* along z: a side line, in from the plot's edge, or straddling a party line */
        const HUnit *u = unit_of(g, b);
        if (b->role == HBR_PARTY) { a0 -= 0.5f * t; a1 += 0.5f * t; }
        else if (fabsf(a0 - u->x0) < 0.01f) a1 = a0 + t;
        else a0 = a1 - t;
    }
    *x0 = a0; *z0 = c0; *x1 = a1; *z1 = c1;
}

/* ---------------------------------------------------------------------------
 * Piers
 * ------------------------------------------------------------------------- */

static int add_pier(HPier *out, int n, int max, float x, float z, float top, int unit, int gate) {
    for (int i = 0; i < n; ++i)
        if (fabsf(out[i].x - x) < 0.05f && fabsf(out[i].z - z) < 0.05f) {
            if (top > out[i].top) { out[i].top = top; out[i].unit = (int8_t)unit; }
            out[i].gate |= (uint8_t)gate;
            return n;
        }
    if (n >= max) return n;
    out[n].x = x; out[n].z = z; out[n].top = top; out[n].unit = (int8_t)unit; out[n].gate = (uint8_t)gate;
    return n + 1;
}

/* whether a gap's edge (a gate's) lies at x along unit u's front */
static int gate_edge(const HGraph *g, int u, float x) {
    for (int i = 0; i < g->ngate; ++i) {
        const HGate *gt = &g->gate[i];
        if (gt->unit != u) continue;
        if (fabsf(gt->x - 0.5f * gt->w - x) < 0.06f || fabsf(gt->x + 0.5f * gt->w - x) < 0.06f) return 1;
    }
    return 0;
}

int home_piers(const HGraph *g, HPier *out, int max) {
    const HBoundSet *S = &g->bset;
    if (!S->drawn || S->pierW <= 0.0f) return 0;
    const float pw = S->pierW, zc = 0.5f * pw - 0.02f;
    int n = 0;
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        if (b->role != HBR_FRONT || !home_bound_drawn(g, b) || b->kind == HB_RAILING) continue;
        const int k = b->unit >= 0 && b->unit < g->nunit ? b->unit : 0;
        const HUnit *u = &g->unit[k];
        const float a = fminf(b->x0, b->x1), c = fmaxf(b->x0, b->x1);
        const float top = b->h + b->th + (b->th > 0.0f ? 0.12f : 0.15f);
        float e[2];
        int gate[2];
        for (int s = 0; s < 2; ++s) {
            const float x = s ? c : a, in = s ? -1.0f : 1.0f;
            gate[s] = 0;
            if (fabsf(x - u->x0) < 0.01f && k > 0) e[s] = x;                       /* a party line: the pier straddles it */
            else if (fabsf(x - u->x1) < 0.01f && k + 1 < g->nunit) e[s] = x;
            else if (fabsf(x - u->x0) < 0.01f || fabsf(x - u->x1) < 0.01f) e[s] = x + in * 0.5f * pw;   /* the row's end */
            else { e[s] = x + in * 0.5f * pw; gate[s] = gate_edge(g, k, x); }       /* a gap's edge: the pier set back into the wall */
        }
        if (c - a < pw + 0.05f) {   /* a stub between two gaps: one pier in its middle */
            n = add_pier(out, n, max, 0.5f * (a + c), zc, top, k, gate[0] | gate[1]);
            continue;
        }
        n = add_pier(out, n, max, e[0], zc, top, k, gate[0]);
        n = add_pier(out, n, max, e[1], zc, top, k, gate[1]);
        if (S->pier > 0.0f) {
            const float L = e[1] - e[0];
            const int m = (int)ceilf(L / S->pier - 0.1f);
            for (int j = 1; j < m; ++j) n = add_pier(out, n, max, e[0] + L * (float)j / (float)m, zc, top, k, 0);
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The graph
 * ------------------------------------------------------------------------- */

static int main_vol(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

static int add_bound(HGraph *g, const HBound *proto, float x0, float x1) {
    if (g->nbound >= HG_MAX_BOUND || x1 - x0 < 0.05f) return -1;
    HBound *b = &g->bound[g->nbound++];
    *b = *proto;
    b->x0 = x0; b->x1 = x1;
    return g->nbound - 1;
}

/* Cuts unit u's front boundary away over a..b (a kiosk's front, pilotis opened to the street). */
static void cut_front(HGraph *g, int u, float a, float b) {
    const int n = g->nbound;
    for (int i = 0; i < n; ++i) {
        HBound *bd = &g->bound[i];
        if (bd->unit != u || bd->role != HBR_FRONT) continue;
        const float x0 = fminf(bd->x0, bd->x1), x1 = fmaxf(bd->x0, bd->x1);
        if (x1 <= a + 0.01f || x0 >= b - 0.01f) continue;
        const HBound keep = *bd;
        if (x0 < a - 0.05f) { bd->x0 = x0; bd->x1 = a; if (x1 > b + 0.05f) add_bound(g, &keep, b, x1); }
        else if (x1 > b + 0.05f) { bd->x0 = b; bd->x1 = x1; }
        else bd->kind = HB_NONE;   /* wholly cut: left as nothing (the rules and the shell skip it) */
    }
}

/* whether a rectangle is clear of the house (pad round it) and of the garden's paths, drives, cars, sheds and trees */
static int rect_clear(const HGraph *g, float x0, float z0, float x1, float z1, float pad) {
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->y0 > 0.01f) continue;
        if (x1 + pad > v->x0 && x0 - pad < v->x1 && z1 + pad > v->z0 && z0 - pad < v->z1) return 0;
    }
    for (int i = 0; i < g->nsite; ++i) {
        const HSite *s = &g->site[i];
        if (s->kind == HE_FRONT || s->kind == HE_REAR) continue;
        if (x1 > s->x0 - 0.1f && x0 < s->x1 + 0.1f && z1 > s->z0 - 0.1f && z0 < s->z1 + 0.1f) return 0;
    }
    return 1;
}

/* the front boundary of unit u that stands against x on side s (-1 its left, +1 its right), or -1 */
static int front_beside(const HGraph *g, int u, float x, int s) {
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        if (b->unit != u || b->role != HBR_FRONT || b->kind == HB_NONE) continue;
        const float x0 = fminf(b->x0, b->x1), x1 = fmaxf(b->x0, b->x1);
        if (s < 0 ? fabsf(x1 - x) < 0.06f : fabsf(x0 - x) < 0.06f) return i;
    }
    return -1;
}

void home_bounds(HGraph *g, int level, BLog *log) {
    memset(&g->bset, 0, sizeof(g->bset));
    if (g->empty || !home_retrofit_style(g->style)) return;
    const BDef *D = &BDEFS[g->style];
    HBoundSet *S = &g->bset;
    Rng r = rng_make(hash_combine(g->seed, 0xB0D5E3u));   /* their own stream: the rest of the house is as it was */
    S->drawn = 1;
    /* the finish: the house's walls (siding rendered over), or the sheet's */
    HFinish f;
    home_finish(g, 0, &f);
    if (D->fin == FIN_HOUSE) {
        S->fin = f.wall == HW_SIDING ? HW_RENDER : f.wall;
        S->col = f.wall == HW_SIDING ? 1 : f.wallCol;
    } else {
        S->fin = D->fin;
        const char *c = D->col ? D->col : "0";
        const size_t nc = strlen(c);
        const char hx = c[(size_t)(rng_float(&r) * (float)nc) % nc];
        S->col = (uint8_t)(hx >= 'a' ? hx - 'a' + 10 : hx - '0');
        if (D->fin == HW_BRICK && (g->style == HS_CHINA || g->style == HS_VICTORIAN)) S->col |= HOUSE_COL_OLD;
        if (D->fin == HW_BRICK && f.wall == HW_BRICK && g->style != HS_CHINA) S->col = f.wallCol;   /* the house's brick */
    }
    S->coping = D->coping;
    S->pier = D->pier;
    S->pierW = D->pierW;
    S->lamps = rng_float(&r) < D->lamps;
    S->razor = rng_float(&r) < D->razor * (1.3f - 0.6f * g->affluence);
    S->stoopRail = D->stoop;
    /* the row's heights and tops, by kind (built as one) */
    float h[HB_COUNT], th[HB_COUNT];
    uint8_t top[HB_COUNT];
    for (int k = 0; k < HB_COUNT; ++k) {
        const BKind *K = &D->k[k];
        h[k] = rng_range(&r, K->h0, K->h1);
        top[k] = rng_float(&r) < K->share ? K->top : HBT_NONE;
        th[k] = top[k] != HBT_NONE ? rng_range(&r, K->th0, K->th1) : 0.0f;
        if (K->h1 <= 0.0f) {   /* a kind the sheet doesn't show, an owner's own: a plain low wall, railings on a kerb */
            h[k] = k == HB_RAILING ? 0.3f : k == HB_FENCE ? 0.5f : rng_range(&r, 0.7f, 1.1f);
            top[k] = k == HB_RAILING ? HBT_IRON : k == HB_FENCE ? HBT_STEEL : HBT_NONE;
            th[k] = top[k] != HBT_NONE ? 0.9f : 0.0f;
        }
    }
    const int mi = main_vol(g);
    const float front = mi >= 0 ? g->vol[mi].z0 : 0.0f;
    for (int i = 0; i < g->nbound; ++i) {
        HBound *b = &g->bound[i];
        if (b->kind != HB_WALL && b->kind != HB_FENCE && b->kind != HB_RAILING) continue;
        const int behind = b->role == HBR_REAR || fminf(b->z0, b->z1) > 0.01f || fmaxf(b->z0, b->z1) > front + 0.05f;
        if (behind && b->kind != HB_WALL) { b->top = HBT_NONE; b->th = 0.0f; continue; }   /* HM7's fences */
        b->h = h[b->kind];
        b->top = top[b->kind];
        b->th = th[b->kind];
    }
    /* the gates */
    for (int i = 0; i < g->ngate; ++i) {
        HGate *gt = &g->gate[i];
        const int u = gt->unit >= 0 && gt->unit < g->nunit ? gt->unit : 0;
        /* the boundary it opens in: its kind's height */
        int bi = front_beside(g, u, gt->x - 0.5f * gt->w, -1);
        if (bi < 0) bi = front_beside(g, u, gt->x + 0.5f * gt->w, 1);
        if (bi < 0 || !home_bound_drawn(g, &g->bound[bi])) { gt->kind = HGK_OPEN; gt->leaves = 0; continue; }
        const HBound *bd = &g->bound[bi];
        const float wallTop = bd->h + bd->th;
        if (gt->drive) {
            gt->kind = D->drive;
            gt->leaves = gt->kind == HGK_OPEN ? 0 : D->leaves ? D->leaves : HGL_DOUBLE;
            gt->h = D->dh1 > 0.0f ? rng_range(&r, D->dh0, D->dh1) : fmaxf(0.9f, wallTop);
            /* a sliding leaf runs behind a wall at least its width long beside it */
            if (gt->leaves == HGL_SLIDING) {
                const int lb = front_beside(g, u, gt->x - 0.5f * gt->w, -1), rb = front_beside(g, u, gt->x + 0.5f * gt->w, 1);
                const float ll = lb >= 0 ? fabsf(g->bound[lb].x1 - g->bound[lb].x0) : 0.0f, rl = rb >= 0 ? fabsf(g->bound[rb].x1 - g->bound[rb].x0) : 0.0f;
                if (fmaxf(ll, rl) < gt->w + 0.2f) gt->leaves = HGL_DOUBLE;
            }
        } else {
            gt->kind = D->gate;
            gt->leaves = gt->kind == HGK_OPEN ? 0 : HGL_SINGLE;
            gt->h = D->gh1 > 0.0f ? rng_range(&r, D->gh0, D->gh1) : fmaxf(0.9f, wallTop);
            if (gt->kind == HGK_PLANK) {
                gt->leaves = HGL_DOUBLE;
                gt->house = rng_float(&r) < D->gatehouse && bd->kind == HB_WALL && bd->h >= 1.6f && level < 2;
                if (!gt->house) { gt->kind = HGK_SHEET; gt->h = fminf(gt->h, bd->h); }   /* no gatehouse: a plain steel door in the wall */
            }
        }
        /* where the row is poor, owners' iron gates become steel security gates (South Asia, Africa, Latin America) */
        if (gt->kind == HGK_IRON && (g->region == PLAN_REGION_SAS || g->region == PLAN_REGION_SSA || g->region == PLAN_REGION_LAT) &&
            ch(g->unit[u].seed, 0x6A7Eu + (uint32_t)i, 0.5f - 0.5f * g->affluence)) gt->kind = HGK_BARS;
        if (bd->kind == HB_RAILING || (bd->kind == HB_WALL && bd->top != HBT_NONE && wallTop < 1.5f))
            gt->h = fminf(gt->h, wallTop + 0.05f);   /* a low front's gate no taller than its railing */
    }
    /* pilotis opened to the street (the Korean sheet): the front cut away over the parking, an open gap; a gate that
     * stood there opens with it */
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *p = &g->vol[i];
        if (p->role != HV_PORCH || p->kind != HP_PILOTIS || p->unit >= g->nunit) continue;
        const int u = p->unit;
        const float a = fmaxf(g->unit[u].x0 + 0.4f, p->x0 + 0.1f), b = fminf(g->unit[u].x1 - 0.4f, p->x1 - 0.1f);
        if (b - a < 2.4f || g->ngate >= HG_MAX_GATE) continue;
        cut_front(g, u, a, b);
        for (int j = 0; j < g->ngate; ++j) {
            HGate *o = &g->gate[j];
            if (o->unit == u && o->x - 0.5f * o->w > a - 0.3f && o->x + 0.5f * o->w < b + 0.3f) { o->kind = HGK_OPEN; o->leaves = 0; o->house = 0; }
        }
        HGate *gt = &g->gate[g->ngate++];
        memset(gt, 0, sizeof(*gt));
        gt->unit = (int8_t)u; gt->drive = 1; gt->kind = HGK_OPEN; gt->x = 0.5f * (a + b); gt->w = b - a;
    }
    /* the walled compound's gate kiosk: beside a gate, its front in the wall's line, its door to the yard */
    if (D->kiosk > 0.0f && level == 0 && mi >= 0 && rng_float(&r) < D->kiosk) {
        const float kw = rng_range(&r, 1.8f, 2.2f), kd = rng_range(&r, 1.7f, 2.1f);
        int placed = 0;
        for (int pass = 0; pass < 2 && !placed; ++pass)
            for (int i = 0; i < g->ngate && !placed; ++i) {
                const HGate *gt = &g->gate[i];
                if ((pass == 0) != (gt->drive != 0) || gt->kind == HGK_OPEN) continue;
                const int u = gt->unit;
                const HUnit *un = &g->unit[u];
                if (kd > front - 0.6f) continue;
                const float pw = S->pierW;
                for (int s = 0; s < 2 && !placed; ++s) {
                    const float x0 = s ? gt->x + 0.5f * gt->w + pw : gt->x - 0.5f * gt->w - pw - kw, x1 = x0 + kw;
                    if (x0 < un->x0 + 0.4f || x1 > un->x1 - 0.4f || !rect_clear(g, x0, 0.0f, x1, kd, 0.4f)) continue;
                    /* the wall it stands in must run there */
                    if (front_beside(g, u, s ? gt->x + 0.5f * gt->w : gt->x - 0.5f * gt->w, s ? 1 : -1) < 0) continue;
                    if (g->nsite >= HG_MAX_SITE) continue;
                    HSite *k = &g->site[g->nsite++];
                    memset(k, 0, sizeof(*k));
                    k->kind = HE_KIOSK; k->unit = (int8_t)u; k->door = -1;
                    k->x0 = x0; k->z0 = 0.0f; k->x1 = x1; k->z1 = kd;
                    cut_front(g, u, x0, x1);
                    placed = 1;
                }
            }
    }
    int kiosks = 0, houses = 0;
    for (int i = 0; i < g->nsite; ++i) kiosks += g->site[i].kind == HE_KIOSK;
    for (int i = 0; i < g->ngate; ++i) houses += g->gate[i].house != 0;
    blog_printf(log, "  boundaries: %s %s, piers %.1f, gates %d%s%s, kiosks %d, gatehouses %d\n", home_bound_name(g->nbound ? g->bound[0].kind : 0),
                home_top_name(g->nbound ? g->bound[0].top : 0), (double)S->pierW, g->ngate, S->razor ? ", razor wire" : "", S->lamps ? ", lamps" : "",
                kiosks, houses);
}

/* The rules: a gate in a gap of its unit's front (no boundary across it); a sliding leaf with a wall to run behind; a
 * double gate's leaves 0.45 m at least; walls no taller than 3 m; a kiosk on its plot, its front in the wall's line, clear
 * of the house, the garden's paths, drives and cars, and the front wall cut where it stands. */
#define REJECT(tag, ...) do { if (log) { blog_printf(log, "  rejected %s: ", tag); blog_printf(log, __VA_ARGS__); blog_printf(log, "\n"); } ok = 0; } while (0)
int home_check_bounds(const HGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->ngate; ++i) {
        const HGate *gt = &g->gate[i];
        const float a = gt->x - 0.5f * gt->w, b = gt->x + 0.5f * gt->w;
        for (int j = 0; j < g->nbound; ++j) {
            const HBound *bd = &g->bound[j];
            if (bd->unit != gt->unit || bd->role != HBR_FRONT || bd->kind == HB_NONE) continue;
            if (fminf(bd->x0, bd->x1) < b - 0.05f && fmaxf(bd->x0, bd->x1) > a + 0.05f) { REJECT("gate", "gate %d stands in boundary %d", i, j); break; }
        }
        if (gt->leaves == HGL_DOUBLE && 0.5f * gt->w < 0.45f) REJECT("gate", "gate %d's leaves %.2f m wide", i, 0.5 * gt->w);
        if (gt->leaves == HGL_SLIDING) {
            const int lb = front_beside(g, gt->unit, a, -1), rb = front_beside(g, gt->unit, b, 1);
            const float ll = lb >= 0 ? fabsf(g->bound[lb].x1 - g->bound[lb].x0) : 0.0f, rl = rb >= 0 ? fabsf(g->bound[rb].x1 - g->bound[rb].x0) : 0.0f;
            if (fmaxf(ll, rl) < gt->w + 0.2f - 0.01f) REJECT("gate", "gate %d slides behind no wall", i);
        }
        if (gt->h > 3.0f) REJECT("gate", "gate %d %.1f m high", i, (double)gt->h);
    }
    for (int j = 0; j < g->nbound; ++j) {
        const HBound *bd = &g->bound[j];
        if (home_bound_drawn(g, bd) && bd->h + bd->th > 3.0f) REJECT("boundary", "boundary %d %.1f m high", j, (double)(bd->h + bd->th));
    }
    for (int i = 0; i < g->nsite; ++i) {
        const HSite *k = &g->site[i];
        if (k->kind != HE_KIOSK) continue;
        if (k->unit < 0 || k->unit >= g->nunit) { REJECT("kiosk", "kiosk %d belongs to no unit", i); continue; }
        const HUnit *u = &g->unit[k->unit];
        if (k->x0 < u->x0 - HEPS || k->x1 > u->x1 + HEPS || fabsf(k->z0) > HEPS) REJECT("kiosk", "kiosk %d off its plot's front", i);
        for (int v = 0; v < g->nvol; ++v) {
            const HVol *w = &g->vol[v];
            if (w->y0 > 0.01f) continue;
            if (k->x1 > w->x0 - 0.3f && k->x0 < w->x1 + 0.3f && k->z1 > w->z0 - 0.3f && k->z0 < w->z1 + 0.3f) { REJECT("kiosk", "kiosk %d against %s %d", i, home_role_name(w->role), v); break; }
        }
        for (int j = 0; j < g->nsite; ++j) {
            const HSite *s = &g->site[j];
            if (j == i || s->kind == HE_FRONT || s->kind == HE_REAR) continue;
            if (k->x1 > s->x0 + HEPS && k->x0 < s->x1 - HEPS && k->z1 > s->z0 + HEPS && k->z0 < s->z1 - HEPS) { REJECT("kiosk", "kiosk %d stands on element %d", i, j); break; }
        }
        for (int j = 0; j < g->nbound; ++j) {
            const HBound *bd = &g->bound[j];
            if (bd->unit != k->unit || bd->role != HBR_FRONT || bd->kind == HB_NONE) continue;
            if (fminf(bd->x0, bd->x1) < k->x1 - 0.05f && fmaxf(bd->x0, bd->x1) > k->x0 + 0.05f) { REJECT("kiosk", "kiosk %d behind boundary %d", i, j); break; }
        }
    }
    return ok;
}
#undef REJECT

/* ---------------------------------------------------------------------------
 * The dressing (near: home_character.c's dress_pass, after the balconies, before the roofs and the retrofit layer)
 * ------------------------------------------------------------------------- */

static void push_at(DC *d, double x, double y, double z, double dx, double dz) {
    const double p[3] = { x, y, z }, dir[3] = { dx, 0.0, dz };
    hc_push_along(d, p, dir);
}

/* The stretches of a drawn boundary's line between its piers: its centreline from s0 to s1 along its direction. */
typedef struct BRun { double s0, s1; } BRun;
static int bound_runs(const HGraph *g, const HBound *b, const HPier *P, int np, double *ox, double *oz, double *dx, double *dz, BRun *out, int max) {
    float x0, z0, x1, z1;
    home_bound_rect(g, b, &x0, &z0, &x1, &z1);
    const int alongX = z1 - z0 < x1 - x0 + 1e-3f && fabsf(b->z1 - b->z0) < 0.01f;
    *ox = alongX ? x0 : 0.5 * (x0 + x1);
    *oz = alongX ? 0.5 * (z0 + z1) : z0;
    *dx = alongX ? 1.0 : 0.0;
    *dz = alongX ? 0.0 : 1.0;
    const double L = alongX ? x1 - x0 : z1 - z0, pw = g->bset.pierW;
    double cut[64][2];
    int nc = 0;
    for (int i = 0; i < np && nc < 64; ++i) {
        const double px = P[i].x - *ox, pz = P[i].z - *oz;
        const double s = px * *dx + pz * *dz, off = px * *dz - pz * *dx;
        if (fabs(off) > 0.5 * pw + 0.05 || s < -0.5 * pw || s > L + 0.5 * pw) continue;
        cut[nc][0] = s - 0.5 * pw; cut[nc][1] = s + 0.5 * pw; nc++;
    }
    for (int a = 1; a < nc; ++a)
        for (int k = a; k > 0 && cut[k][0] < cut[k - 1][0]; --k) { double t0 = cut[k][0], t1 = cut[k][1]; cut[k][0] = cut[k - 1][0]; cut[k][1] = cut[k - 1][1]; cut[k - 1][0] = t0; cut[k - 1][1] = t1; }
    int n = 0;
    double s = 0.0;
    for (int i = 0; i <= nc && n < max; ++i) {
        const double e = i < nc ? cut[i][0] : L;
        if (e - s > 0.15) { out[n].s0 = s; out[n].s1 = e; n++; }
        if (i < nc && cut[i][1] > s) s = cut[i][1];
    }
    return n;
}

/* a run of top pieces of module m about `piece` long (x 0..1 stretched, y 0..1 stretched to h) */
static void top_run(Pm *pm, int module, float len, float h, float piece) {
    int n = (int)floorf(len / piece + 0.5f);
    if (n < 1) n = 1;
    const float seg = len / (float)n;
    for (int k = 0; k < n; ++k) { pm_push(pm); pm_translate(pm, seg * (float)k, 0.0f, 0.0f); pm_scale(pm, seg, h, 1.0f); pm_place(pm, module); pm_pop(pm); }
}

static void dress_top(DC *d, const HBound *b, const HPier *P, int np) {
    const HGraph *g = d->g;
    const double dy = d->C->dy ? d->C->dy[b->unit] : 0.0;
    double ox, oz, dx, dz;
    BRun runs[24];
    const int nr = bound_runs(g, b, P, np, &ox, &oz, &dx, &dz, runs, 24);
    const double y = b->h + dy;
    for (int i = 0; i < nr; ++i) {
        const double s0 = runs[i].s0 + 0.01, len = runs[i].s1 - runs[i].s0 - 0.02;
        if (len < 0.15) continue;
        push_at(d, ox + dx * s0, y, oz + dz * s0, dx, dz);
        Pm *pm = d->pm;
        switch (b->top) {
        case HBT_IRON:   pm_scale(pm, 1.0f, b->th / HK_RAIL_H, 1.0f); hk_rail(pm, 0, (float)len, 1); break;
        case HBT_SPEAR:  top_run(pm, PMK_H_RAIL_SPEAR, (float)len, b->th, 1.0f);
                         if (g->bset.pierW <= 0.0f) { pm_push(pm); pm_translate(pm, (float)len - 0.02f, 0.0f, 0.0f); pm_scale(pm, 1.0f, b->th / HK_RAIL_H, 1.0f); pm_place(pm, PMK_H_RAIL_POST); pm_pop(pm); }
                         break;
        case HBT_STEEL:  top_run(pm, PMK_R_FENCE_STEEL, (float)len, b->th, 1.2f); break;
        case HBT_MESH:   top_run(pm, PMK_R_FENCE_MESH, (float)len, b->th, 1.8f);
                         pm_push(pm); pm_translate(pm, (float)len - 0.02f, 0.0f, 0.0f); pm_scale(pm, 0.6f, b->th, 0.6f); pm_place(pm, PMK_R_FRAME_POST); pm_pop(pm);
                         break;
        case HBT_BREEZE: hr_breeze(pm, 0, (float)len, b->th); break;
        case HBT_RINGS:  hr_breeze(pm, 1, (float)len, b->th); break;
        default: break;
        }
        hc_pop(d);
    }
}

/* A gate's leaves across x0..x1 of the front (its line at z, the unit's ground lifted by dy). */
static void dress_gate(DC *d, const HGate *gt, double x0, double x1, double z, double dy) {
    const double w = x1 - x0;
    if (w < 0.3 || gt->kind == HGK_OPEN || gt->kind == HGK_PLANK) return;
    const int n = gt->leaves == HGL_DOUBLE ? 2 : 1;
    const double lw = w / n - 0.02;
    for (int k = 0; k < n; ++k) {
        const double a = x0 + 0.01 + k * (lw + 0.02);
        Pm *pm = d->pm;
        push_at(d, a, dy + 0.04, z, 1.0, 0.0);
        switch (gt->kind) {
        case HGK_IRON:    pm_scale(pm, (float)lw, gt->h - 0.04f, 1.0f); pm_place(pm, PMK_H_GATE_IRON); break;
        case HGK_PIERCED: pm_scale(pm, (float)lw, gt->h - 0.04f, 1.0f); pm_place(pm, PMK_R_GATE_PIERCED); break;
        case HGK_MESH:    pm_scale(pm, (float)lw, gt->h - 0.04f, 1.0f); pm_place(pm, PMK_R_FENCE_MESH); break;
        case HGK_STEEL:   pm_scale(pm, (float)lw, gt->h - 0.04f, 1.0f); pm_place(pm, PMK_R_FENCE_STEEL); break;
        case HGK_BARS: case HGK_SHEET: {   /* the retrofit kit's leaves: centred on x, z 0..0.04 */
            const float H = gt->kind == HGK_BARS ? HR_GATE_H : HR_SHEET_H;
            pm_translate(pm, (float)(0.5 * lw), 0.0f, -0.02f);
            pm_scale(pm, (float)(lw / HR_GATE_W), (gt->h - 0.04f) / H, 1.0f);
            pm_place(pm, gt->kind == HGK_BARS ? PMK_R_GATE_BARS : PMK_R_GATE_SHEET);
            break;
        }
        default: break;
        }
        hc_pop(d);
    }
    /* a sliding leaf's track along the ground */
    if (gt->leaves == HGL_SLIDING) {
        push_at(d, x0 - 0.1, dy + 0.02, z, 1.0, 0.0);
        pm_push(d->pm); pm_scale(d->pm, (float)(2.0 * w + 0.2), 0.4f, 0.4f); pm_place(d->pm, PMK_H_BAR_IRON); pm_pop(d->pm);
        hc_pop(d);
    }
}

/* Iron rails up both sides of a stoop's steps (the brownstone's, the Georgian's): from a newel at the foot along the
 * steps' nosings to the stoop's top, then level to the wall; balusters on each tread. As home_shell.c's add_front lays
 * the stoop and add_steps its steps. */
static void dress_stoop(DC *d, int u) {
    const HGraph *g = d->g;
    const HUnit *un = &g->unit[u];
    if (un->door < 0) return;
    const HDoor *dr = &g->door[un->door];
    const HVol *m = &g->vol[dr->vol];
    const double P = g->plinth;
    if (P < 0.5 || home_style_street(g->style)) return;
    for (int i = 0; i < g->nvol; ++i) {   /* a porch's own steps, a five-foot way's or pilotis' none: only a bare stoop */
        const HVol *p = &g->vol[i];
        if (p->role == HV_PORCH && p->unit == u && p->kind != HP_CANOPY && p->kind != HP_RECESSED) return;
    }
    const double dy = d->C->dy ? d->C->dy[u] : 0.0;
    const double sw = fmin(1.6, dr->width + 0.5), depth = fmin(1.0, fmax(0.3, m->z0 - 0.4)), zf = m->z0 - depth;
    const int nr = (int)ceil(P / 0.18);
    const double rh = P / nr;
    double tread = 0.28;
    if (nr > 1 && zf - 0.1 < tread * (nr - 1)) tread = fmax(0.16, (zf - 0.1) / (nr - 1));
    int last = 0;   /* the steps built (add_steps stops at the street) */
    for (int i = 1; i < nr; ++i) { if (zf - (i - 1) * tread < 0.05) break; last = i; }
    if (last < 2) return;
    const double zb = fmax(0.05, zf - last * tread), H = 0.9;   /* the foot's nosing; the rail over the nosings */
    const double yb = P - last * rh;                            /* the lowest step's... the ground under the foot */
    hc_begin(d, u);
    for (int s = 0; s < 2; ++s) {
        const double x = dr->x + (s ? 1.0 : -1.0) * (0.5 * sw - 0.05);
        /* the sloped handrail: from over the foot to over the stoop's edge */
        const double ya = (yb > 0.0 ? yb : 0.0) + H + 0.1 + dy, yt = P + H + dy;
        const double sx = 0.0, sz = zf - zb, sy = yt - ya, L = sqrt(sz * sz + sy * sy);
        {
            const double p[3] = { x, ya, zb }, dir[3] = { sx, sy / L, sz / L };
            hc_push_along(d, p, dir);
            pm_scale(d->pm, (float)L, 1.0f, 1.0f);
            pm_place(d->pm, PMK_H_BAR_IRON);
            hc_pop(d);
        }
        /* level along the stoop to the wall */
        push_at(d, x, yt, zf, 0.0, 1.0);
        pm_scale(d->pm, (float)(m->z0 - zf - 0.02), 1.0f, 1.0f);
        pm_place(d->pm, PMK_H_BAR_IRON);
        hc_pop(d);
        /* the newel at the foot, balusters on the treads and the stoop */
        push_at(d, x, dy, zb + 0.04, 1.0, 0.0);
        pm_scale(d->pm, 1.4f, (float)((ya - dy + 0.06) / HK_RAIL_H), 1.4f);
        pm_place(d->pm, PMK_H_RAIL_POST);
        hc_pop(d);
        for (int i = 1; i <= last; ++i) {
            const double z = zf - (i - 0.5) * tread, yTread = P - i * rh, yRail = ya + (z - zb) / sz * sy - dy;
            if (z < zb + 0.1) continue;
            push_at(d, x, yTread + dy, z, 1.0, 0.0);
            pm_scale(d->pm, 0.4f, (float)((yRail - yTread) / HK_RAIL_H), 0.4f);
            pm_place(d->pm, PMK_H_RAIL_POST);
            hc_pop(d);
        }
        for (double z = zf + 0.3; z < m->z0 - 0.15; z += 0.3) {
            push_at(d, x, P + dy, z, 1.0, 0.0);
            pm_scale(d->pm, 0.4f, (float)(H / HK_RAIL_H), 0.4f);
            pm_place(d->pm, PMK_H_RAIL_POST);
            hc_pop(d);
        }
    }
    hc_done(d);
}

void hs_bounds(DC *d) {
    const HGraph *g = d->g;
    const HBoundSet *S = &g->bset;
    if (!S->drawn) return;
    const Ctx *C = d->C;
    HPier P[160];
    const int np = home_piers(g, P, 160);
    /* the tops, the razor wire along the walls' copings */
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        if (!home_bound_drawn(g, b)) continue;
        hc_begin(d, b->unit);
        if (b->top != HBT_NONE) dress_top(d, b, P, np);
        if (S->razor && b->kind == HB_WALL && b->role != HBR_REAR) {
            double ox, oz, dx, dz;
            BRun runs[24];
            const int nr = bound_runs(g, b, P, np, &ox, &oz, &dx, &dz, runs, 24);
            const double dy = C->dy ? C->dy[b->unit] : 0.0, y = b->h + b->th + (S->coping == HBC_TILE ? 0.2 : 0.07) + dy;
            for (int k = 0; k < nr; ++k) {
                if (runs[k].s1 - runs[k].s0 < 0.4) continue;
                push_at(d, ox + dx * (runs[k].s0 + 0.05), y - 0.04, oz + dz * (runs[k].s0 + 0.05), dx, dz);
                pm_scale(d->pm, 1.0f, 0.8f, 0.8f);
                hr_razor(d->pm, (float)(runs[k].s1 - runs[k].s0 - 0.1));
                hc_pop(d);
            }
        }
        hc_done(d);
    }
    /* the gates: their leaves between the piers (or the kerb's posts), lanterns on the piers' street faces */
    for (int i = 0; i < g->ngate; ++i) {
        const HGate *gt = &g->gate[i];
        if (gt->kind == HGK_OPEN || gt->unit < 0 || gt->unit >= g->nunit) continue;
        const double dy = C->dy ? C->dy[gt->unit] : 0.0;
        double a = gt->x - 0.5 * gt->w, b = gt->x + 0.5 * gt->w;
        for (int k = 0; k < np; ++k) {   /* a neighbour's pier standing into the gap */
            if (P[k].x + 0.5 * S->pierW > a && P[k].x < gt->x) a = P[k].x + 0.5 * S->pierW;
            if (P[k].x - 0.5 * S->pierW < b && P[k].x > gt->x) b = P[k].x - 0.5 * S->pierW;
        }
        int bi = front_beside(g, gt->unit, gt->x - 0.5f * gt->w, -1);
        if (bi < 0) bi = front_beside(g, gt->unit, gt->x + 0.5f * gt->w, 1);
        const double t = bi >= 0 ? home_bound_thick(&g->bound[bi]) : BOUND_T;
        hc_begin(d, gt->unit);
        dress_gate(d, gt, a, b, 0.5 * t, dy);
        /* a railing's gate hangs on iron posts */
        if (bi >= 0 && g->bound[bi].kind == HB_RAILING && gt->kind != HGK_OPEN)
            for (int s = 0; s < 2; ++s) {
                push_at(d, s ? b + 0.03 : a - 0.03, dy, 0.5 * t, 1.0, 0.0);
                pm_scale(d->pm, 1.8f, (gt->h + 0.12f) / HK_RAIL_H, 1.8f);
                pm_place(d->pm, PMK_H_RAIL_POST);
                hc_pop(d);
            }
        hc_done(d);
        /* the retrofit layer keeps off the gate's swing */
        const double lo[3] = { a, dy, -0.3 }, hi[3] = { b, dy + gt->h + 0.3, 1.5 };
        hc_occupy(d, lo, hi);
    }
    if (S->lamps)
        for (int k = 0; k < np; ++k) {
            if (!P[k].gate) continue;
            const int u = P[k].unit >= 0 && P[k].unit < g->nunit ? P[k].unit : 0;
            const double dy = C->dy ? C->dy[u] : 0.0;
            const double p[3] = { P[k].x, P[k].top - 0.32 + dy, P[k].z - 0.5 * S->pierW };
            hc_begin(d, u);
            hc_push_wall(d, p, 0.0, -1.0);
            pm_place(d->pm, PMK_R_LANTERN);
            hc_pop(d);
            hc_done(d);
        }
    if (S->stoopRail)
        for (int u = 0; u < g->nunit; ++u) dress_stoop(d, u);
    /* the retrofit layer keeps off the walls' tops (its pots and troughs stand in the yards) */
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        if (!home_bound_drawn(g, b) || b->role != HBR_FRONT) continue;
        float x0, z0, x1, z1;
        home_bound_rect(g, b, &x0, &z0, &x1, &z1);
        const double dy = C->dy ? C->dy[b->unit] : 0.0;
        const double lo[3] = { x0, dy, z0 - 0.3 }, hi[3] = { x1, dy + b->h + b->th + 0.4, z1 + 0.3 };
        hc_occupy(d, lo, hi);
    }
}
