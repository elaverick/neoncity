#include "home_shell_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI_D 3.14159265358979323846

/* ===========================================================================
 * Houses' shells (residential roadmap HM3a): see home_shell.h.
 *
 *   finishes   each style's walls, roofs, trim and glazing, and a unit's picks
 *   solids     convex polyhedra as half-spaces (n.p <= d), a face on each plane
 *   clipping   a face less the solids it meets, an opening's faces inside its host
 *   the house  the volumes' bodies and roof slabs, parapets, upstands, stacks,
 *              porches' floors and posts, steps, and the openings
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * Finishes (house_surf.h; the palettes' colours are shaders/city/house.glsl's)
 *
 *   brick    0 red, 1 dark red-brown, 2 orange-red, 3 buff stock, 4 brown, 5 blue-black, 6 grey, 7 cream
 *   render   0 white, 1 cream, 2 magnolia, 3 pale grey, 4 stone grey, 5 sand, 6 ochre, 7 terracotta, 8 peach,
 *            9 pink, a pale blue, b pale green, c yellow, d turquoise, e lilac, f pebbledash
 *   siding   0 white, 1 cream, 2 pale yellow, 3 sage, 4 slate blue, 5 grey, 6 dark grey, 7 barn red,
 *            8 light blue, 9 tan, a dark green, b brown
 *   stone    0 limestone, 1 sandstone, 2 brownstone, 3 granite
 *   block    0 bare grey, 1 dark grey, 2 whitewash, 3 pale pink, 4 pale blue, 5 sand
 *   trim     0 white, 1 cream, 2 black, 3 dark green, 4 grey, 5 brown, 6 navy, 7 red
 * ------------------------------------------------------------------------- */

#define NROOFP 7   /* the pitched coverings: HRM_CLAY .. HRM_GLAZED */
static const uint8_t ROOF_COLOURS[HRM_COUNT] = { 4, 3, 4, 6, 6, 3, 3, 2, 2 };

typedef struct FinishDef {
    float       wall[HW_COUNT];
    const char *col[HW_COUNT];    /* each finish's colours, palette entries as hex digits (a repeat weighs) */
    float       roof[NROOFP];
    const char *trim;
    uint8_t     glazing, step;
    float       winW, winH;       /* a window's usual size (m) */
} FinishDef;

static const FinishDef FIN[HS_COUNT] = {
    [HS_CRAFTSMAN]    = { { 0.15f, 0.15f, 0.70f, 0, 0 }, { "014", "125f", "34ab9017", "", "" }, { [HRM_SHINGLE] = 0.8f, [HRM_METAL] = 0.1f, [HRM_CLAY] = 0.1f }, "0153", HG_GRID, HST_TIMBER, 1.0f, 1.4f },
    [HS_SPANISH]      = { { 0, 1, 0, 0, 0 }, { "", "01285", "", "", "" }, { [HRM_BARREL] = 0.9f, [HRM_CLAY] = 0.1f }, "503", HG_CASEMENT, HST_TILE, 1.0f, 1.3f },
    [HS_RANCH]        = { { 0.40f, 0.25f, 0.35f, 0, 0 }, { "0143", "01235", "05961", "", "" }, { [HRM_SHINGLE] = 0.6f, [HRM_CONCRETE] = 0.25f, [HRM_METAL] = 0.15f }, "015", HG_PLAIN, HST_CONCRETE, 1.5f, 1.1f },
    [HS_TRACT]        = { { 0.20f, 0.30f, 0.50f, 0, 0 }, { "043", "1235", "015689", "", "" }, { [HRM_SHINGLE] = 0.8f, [HRM_CONCRETE] = 0.2f }, "01", HG_PLAIN, HST_CONCRETE, 1.2f, 1.3f },
    [HS_ROWHOUSE]     = { { 0.70f, 0, 0, 0.30f, 0 }, { "0124", "", "", "220", "" }, { [HRM_SLATE] = 0.6f, [HRM_SHINGLE] = 0.4f }, "22536", HG_SASH, HST_STONE, 0.95f, 1.7f },
    [HS_TRIPLEDECKER] = { { 0, 0, 1, 0, 0 }, { "", "", "01234589", "", "" }, { [HRM_SHINGLE] = 1.0f }, "01", HG_SASH, HST_TIMBER, 0.9f, 1.5f },
    [HS_VICTORIAN]    = { { 0.75f, 0.25f, 0, 0, 0 }, { "01342", "013", "", "", "" }, { [HRM_SLATE] = 0.65f, [HRM_CONCRETE] = 0.25f, [HRM_CLAY] = 0.1f }, "00011", HG_SASH, HST_STONE, 0.95f, 1.6f },
    [HS_GEORGIAN]     = { { 0.60f, 0.40f, 0, 0, 0 }, { "1403", "012", "", "", "" }, { [HRM_SLATE] = 0.8f, [HRM_CONCRETE] = 0.2f }, "0001", HG_GEORGIAN, HST_STONE, 0.95f, 1.6f },
    [HS_INTERWAR]     = { { 0.45f, 0.55f, 0, 0, 0 }, { "024", "012f3", "", "", "" }, { [HRM_CLAY] = 0.8f, [HRM_CONCRETE] = 0.2f }, "031", HG_CASEMENT, HST_CONCRETE, 1.5f, 1.2f },
    [HS_POSTWAR]      = { { 0.70f, 0.30f, 0, 0, 0 }, { "0432", "01f3", "", "", "" }, { [HRM_CONCRETE] = 0.8f, [HRM_CLAY] = 0.2f }, "045", HG_CASEMENT, HST_CONCRETE, 1.4f, 1.2f },
    [HS_MODERN]       = { { 0.50f, 0.30f, 0.20f, 0, 0 }, { "3074", "034", "6b59", "", "" }, { [HRM_CONCRETE] = 0.7f, [HRM_SLATE] = 0.3f }, "240", HG_PLAIN, HST_CONCRETE, 1.6f, 1.3f },
    [HS_SEU_VILLA]    = { { 0, 0.85f, 0, 0.15f, 0 }, { "", "1567802", "", "01", "" }, { [HRM_CLAY] = 1.0f }, "0351", HG_CASEMENT, HST_STONE, 1.0f, 1.3f },
    [HS_TOWNHOUSE]    = { { 0, 0.85f, 0, 0.15f, 0 }, { "", "56789abc1", "", "01", "" }, { [HRM_CLAY] = 1.0f }, "0365", HG_CASEMENT, HST_STONE, 0.9f, 1.6f },
    [HS_JAPAN]        = { { 0, 0.30f, 0.70f, 0, 0 }, { "", "0342", "01596", "", "" }, { [HRM_GLAZED] = 0.6f, [HRM_METAL] = 0.2f, [HRM_SLATE] = 0.2f }, "240", HG_GRID, HST_CONCRETE, 1.6f, 1.0f },
    [HS_CHINA]        = { { 0.55f, 0.45f, 0, 0, 0 }, { "667", "034", "", "", "" }, { [HRM_GLAZED] = 0.7f, [HRM_CLAY] = 0.3f }, "247", HG_GRID, HST_STONE, 1.2f, 1.3f },
    [HS_KOREA]        = { { 0.60f, 0.40f, 0, 0, 0 }, { "0142", "031", "", "", "" }, { [HRM_GLAZED] = 0.4f, [HRM_METAL] = 0.3f, [HRM_CLAY] = 0.3f }, "04", HG_PLAIN, HST_CONCRETE, 1.4f, 1.2f },
    [HS_SHOPHOUSE]    = { { 0, 1, 0, 0, 0 }, { "", "1289abcd6", "", "", "" }, { [HRM_CLAY] = 1.0f }, "0136", HG_CASEMENT, HST_TILE, 1.0f, 1.6f },
    [HS_LINKHOUSE]    = { { 0, 1, 0, 0, 0 }, { "", "013abc", "", "", "" }, { [HRM_CONCRETE] = 0.6f, [HRM_CLAY] = 0.4f }, "04", HG_CASEMENT, HST_CONCRETE, 1.4f, 1.3f },
    [HS_RCC]          = { { 0, 0.80f, 0, 0, 0.20f }, { "", "9abcde605", "", "", "01" }, { [HRM_METAL] = 0.5f, [HRM_CLAY] = 0.5f }, "0367", HG_PLAIN, HST_CONCRETE, 1.2f, 1.3f },
    [HS_BUNGALOW]     = { { 0.20f, 0.80f, 0, 0, 0 }, { "02", "015bca", "", "", "" }, { [HRM_CLAY] = 0.6f, [HRM_CONCRETE] = 0.4f }, "03", HG_CASEMENT, HST_TILE, 1.2f, 1.3f },
    [HS_MENA_VILLA]   = { { 0, 0.85f, 0, 0.15f, 0 }, { "", "01523", "", "01", "" }, { [HRM_CLAY] = 1.0f }, "150", HG_PLAIN, HST_STONE, 1.1f, 1.3f },
    [HS_COURTYARD]    = { { 0, 0.70f, 0, 0, 0.30f }, { "", "561f", "", "", "05" }, { [HRM_CLAY] = 1.0f }, "51", HG_PLAIN, HST_STONE, 0.6f, 0.8f },
    [HS_BLOCK]        = { { 0, 0.50f, 0, 0, 0.50f }, { "", "015bcda", "", "", "012345" }, { [HRM_METAL] = 0.85f, [HRM_CONCRETE] = 0.15f }, "0367", HG_PLAIN, HST_CONCRETE, 1.0f, 1.2f },
    [HS_WALLED]       = { { 0, 0.70f, 0, 0, 0.30f }, { "", "015ca", "", "", "02" }, { [HRM_METAL] = 0.5f, [HRM_CONCRETE] = 0.3f, [HRM_CLAY] = 0.2f }, "03", HG_PLAIN, HST_CONCRETE, 1.1f, 1.2f },
    [HS_SELFBUILT]    = { { 0.40f, 0.45f, 0, 0, 0.15f }, { "202", "9abcde60", "", "", "01" }, { [HRM_METAL] = 0.5f, [HRM_CLAY] = 0.5f }, "037", HG_PLAIN, HST_CONCRETE, 1.0f, 1.2f },
    [HS_COLONIAL]     = { { 0, 1, 0, 0, 0 }, { "", "c9ad6807", "", "", "" }, { [HRM_CLAY] = 1.0f }, "015", HG_CASEMENT, HST_STONE, 1.0f, 1.8f },
};

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; }
static int pick_hex(const char *s, uint32_t h) {
    const size_t n = s ? strlen(s) : 0;
    return n ? hexval(s[h % (uint32_t)n]) : 0;
}
static int pick_w(const float *w, int n, uint32_t h) {
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) sum += w[i];
    if (sum <= 0.0f) return 0;
    float x = hash_float(h) * sum;
    for (int i = 0; i < n; ++i) {
        if (x < w[i]) return i;
        x -= w[i];
    }
    return n - 1;
}

void home_finish(const HGraph *g, int unit, HFinish *f) {
    const FinishDef *D = &FIN[g->style < HS_COUNT ? g->style : HS_TRACT];
    const uint32_t row = hash_combine(g->seed, 0xF1A1u);
    const uint32_t own = hash_combine(unit >= 0 && unit < g->nunit ? g->unit[unit].seed : g->seed, 0xF1A2u);
    memset(f, 0, sizeof(*f));
    /* the row's walls and roof: built as one */
    f->wall = (uint8_t)pick_w(D->wall, HW_COUNT, hash_combine(row, 1u));
    if (!D->col[f->wall][0]) f->wall = HW_RENDER;
    f->wallCol = (uint8_t)pick_hex(D->col[f->wall], hash_combine(row, 2u));
    f->roof = (uint8_t)pick_w(D->roof, NROOFP, hash_combine(row, 3u));
    f->roofCol = (uint8_t)(hash_combine(row, 4u) % ROOF_COLOURS[f->roof]);
    const int wet = g->region == PLAN_REGION_NA || g->region == PLAN_REGION_UK || g->region == PLAN_REGION_SEU || g->region == PLAN_REGION_EAS;
    f->flat = wet ? HRM_FELT : HRM_SLAB;
    f->flatCol = (uint8_t)(hash_combine(row, 5u) & 1u);
    f->trimCol = (uint8_t)pick_hex(D->trim, hash_combine(row, 6u));
    f->garageCol = (uint8_t)(hash_combine(row, 7u) % 6u);
    /* owners: render and siding repainted (three in ten), a brick front painted over (one in twelve), the trim
     * their own (one in five), the door always their own */
    const int painted = f->wall == HW_BRICK && hash_float(hash_combine(own, 1u)) < 0.08f;
    if (painted) { f->wall = HW_RENDER; f->wallCol = (uint8_t)pick_hex("0123", hash_combine(own, 2u)); }
    else if ((f->wall == HW_RENDER || f->wall == HW_SIDING) && hash_float(hash_combine(own, 3u)) < 0.3f)
        f->wallCol = (uint8_t)pick_hex(D->col[f->wall], hash_combine(own, 4u));
    if (hash_float(hash_combine(own, 5u)) < 0.2f) f->trimCol = (uint8_t)pick_hex(D->trim, hash_combine(own, 6u));
    f->doorCol = (uint8_t)(hash_combine(own, 7u) % 12u);
    /* HM5e1: the sheets' doors: China's red (a dark timber one in six), the Middle East's and Latin America's timber */
    if (g->style == HS_CHINA) f->doorCol = hash_float(hash_combine(own, 9u)) < 0.84f ? 0 : 9;
    else if (g->style == HS_COURTYARD || g->style == HS_MENA_VILLA) f->doorCol = (uint8_t)pick_hex("8899", hash_combine(own, 9u));
    else if (g->style == HS_COLONIAL) f->doorCol = (uint8_t)pick_hex("889935", hash_combine(own, 9u));
    /* stacks: brick where the walls are, or in Britain and North America whatever the walls; else rendered */
    if (f->wall == HW_BRICK) { f->stack = HW_BRICK; f->stackCol = f->wallCol; }
    else if (g->region == PLAN_REGION_UK || g->region == PLAN_REGION_NA) { f->stack = HW_BRICK; f->stackCol = (uint8_t)pick_hex("014", hash_combine(row, 8u)); }
    else { f->stack = f->wall; f->stackCol = f->wallCol; }
    /* the styles built before the war lay old stock brick, sooted (house_brick); later ones a cleaner wire-cut */
    const int old = g->style == HS_VICTORIAN || g->style == HS_GEORGIAN || g->style == HS_ROWHOUSE || g->style == HS_INTERWAR ||
                    g->style == HS_CHINA || g->style == HS_SELFBUILT;
    if (old && f->wall == HW_BRICK) f->wallCol |= HOUSE_COL_OLD;
    if (old && f->stack == HW_BRICK) f->stackCol |= HOUSE_COL_OLD;
    f->glazing = D->glazing;
    f->step = D->step;
    f->winW = D->winW;
    f->winH = D->winH;
}

/* ---------------------------------------------------------------------------
 * Solids
 * ------------------------------------------------------------------------- */

#define EPS_S   0.002     /* a face runs this far into a solid it meets, so no crack shows where they join */
#define EPS_N   0.008     /* coplanar: how near a face must lie to another solid's plane to count as on it */
#define ON_TOL  1e-3
#define BIG     400.0
#define PARAPET_W 0.25f
#define UPSTAND   0.25f   /* a party wall's rise over the roof */

static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const double *a, const double *b, double *o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static void norm3(double *a) {
    double l = sqrt(dot3(a, a));
    if (l > 1e-12) { a[0] /= l; a[1] /= l; a[2] /= l; }
}

static Solid *solid_new(Ctx *C, int unit, int vol, int neg) {
    if (C->ns == C->cap) {
        C->cap = C->cap ? C->cap * 2 : 256;
        C->s = (Solid *)realloc(C->s, sizeof(Solid) * (size_t)C->cap);
        if (!C->s) abort();
    }
    Solid *s = &C->s[C->ns++];
    memset(s, 0, sizeof(*s));
    s->unit = (int8_t)unit; s->vol = (int8_t)vol; s->neg = (uint8_t)neg; s->host = -1;
    const HFinish *f = &C->fin[unit >= 0 && unit < HG_MAX_UNITS ? unit : 0];
    s->wallFin = f->wall; s->wallCol = f->wallCol;
    s->roofFin = f->roof; s->roofCol = f->roofCol;
    s->trimCol = f->trimCol; s->stepFin = f->step;
    return s;
}

/* The half-space n.p <= n.q (n outward, through point q), its face's surface. */
static void plane_at(Solid *s, double nx, double ny, double nz, double qx, double qy, double qz, int surf) {
    if (s->np >= MAXP) return;
    SPlane *p = &s->pl[s->np];
    p->n[0] = nx; p->n[1] = ny; p->n[2] = nz;
    norm3(p->n);
    p->d = p->n[0] * qx + p->n[1] * qy + p->n[2] * qz;
    s->surf[s->np] = (uint8_t)surf;
    s->noWin[s->np] = 0;
    s->np++;
}

/* An axis-aligned box's six planes: sides, top, bottom. */
static void box_planes(Solid *s, double x0, double y0, double z0, double x1, double y1, double z1, int side, int top, int bot) {
    plane_at(s, -1, 0, 0, x0, 0, 0, side); plane_at(s, 1, 0, 0, x1, 0, 0, side);
    plane_at(s, 0, 0, -1, 0, 0, z0, side); plane_at(s, 0, 0, 1, 0, 0, z1, side);
    plane_at(s, 0, 1, 0, 0, y1, 0, top);   plane_at(s, 0, -1, 0, 0, y0, 0, bot);
}

/* Raises a solid by dy (its unit's floor over the row's lowest). */
static void solid_lift(Solid *s, double dy) {
    for (int i = 0; i < s->np; ++i) s->pl[i].d += s->pl[i].n[1] * dy;
    s->o[1] += dy;
}

/* keeps the part of in where sgn (n.p - d) <= 0 */
static void clip_poly(const SPoly *in, const double *n, double d, double sgn, SPoly *out) {
    out->n = 0;
    for (int i = 0; i < in->n; ++i) {
        const double *a = in->p[i], *b = in->p[(i + 1) % in->n];
        const double da = sgn * (dot3(n, a) - d), db = sgn * (dot3(n, b) - d);
        if (da <= 0.0 && out->n < PV) { memcpy(out->p[out->n], a, sizeof(double) * 3); out->n++; }
        if (((da < 0.0 && db > 0.0) || (da > 0.0 && db < 0.0)) && out->n < PV) {
            const double t = da / (da - db);
            for (int k = 0; k < 3; ++k) out->p[out->n][k] = a[k] + t * (b[k] - a[k]);
            out->n++;
        }
    }
}

/* Twice the area, seen along n. */
static double poly_area2(const SPoly *p, const double *n) {
    double a = 0.0, e1[3], e2[3], c[3];
    for (int i = 1; i + 1 < p->n; ++i) {
        for (int k = 0; k < 3; ++k) { e1[k] = p->p[i][k] - p->p[0][k]; e2[k] = p->p[i + 1][k] - p->p[0][k]; }
        cross3(e1, e2, c);
        a += dot3(c, n);
    }
    return a;
}

static int poly_ok(const SPoly *p, const double *n) { return p->n >= 3 && poly_area2(p, n) > 2e-6; }

/* Drops corners closer than a tenth of a millimetre to the one before. */
static void poly_weld(SPoly *p) {
    int w = 0;
    for (int i = 0; i < p->n; ++i) {
        const double *a = p->p[i], *b = w ? p->p[w - 1] : p->p[p->n - 1];
        const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        if (w && dx * dx + dy * dy + dz * dz < 1e-8) continue;
        if (w != i) memcpy(p->p[w], a, sizeof(double) * 3);
        w++;
    }
    if (w > 1) {
        const double *a = p->p[w - 1], *b = p->p[0];
        const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        if (dx * dx + dy * dy + dz * dz < 1e-8) w--;
    }
    p->n = w;
}

/* The face of solid s on its plane i: a big square on the plane, clipped by the rest. */
static int solid_face(const Solid *s, int i, SPoly *out, SPoly *tmp) {
    const double *n = s->pl[i].n;
    double a[3] = { 0, 1, 0 }, u[3], v[3], c[3];
    if (fabs(n[1]) > 0.9) { a[0] = 1; a[1] = 0; }
    cross3(a, n, u); norm3(u);
    cross3(n, u, v);
    for (int k = 0; k < 3; ++k) c[k] = n[k] * s->pl[i].d;
    static const double S[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    out->n = 4;
    for (int q = 0; q < 4; ++q)
        for (int k = 0; k < 3; ++k) out->p[q][k] = c[k] + BIG * (S[q][0] * u[k] + S[q][1] * v[k]);
    for (int j = 0; j < s->np && out->n >= 3; ++j) {
        if (j == i) continue;
        clip_poly(out, s->pl[j].n, s->pl[j].d, 1.0, tmp);
        *out = *tmp;
    }
    poly_weld(out);
    return poly_ok(out, n);
}

/* A solid's bounds and whether it has any faces at all. */
static void solid_bounds(Solid *s, SPoly *f, SPoly *tmp) {
    for (int k = 0; k < 3; ++k) { s->mn[k] = 1e30; s->mx[k] = -1e30; }
    s->live = 0;
    for (int i = 0; i < s->np; ++i) {
        if (!solid_face(s, i, f, tmp)) continue;
        s->live = 1;
        for (int q = 0; q < f->n; ++q)
            for (int k = 0; k < 3; ++k) { s->mn[k] = fmin(s->mn[k], f->p[q][k]); s->mx[k] = fmax(s->mx[k], f->p[q][k]); }
    }
}

static int boxes_meet(const double *amn, const double *amx, const double *bmn, const double *bmx, double pad) {
    for (int k = 0; k < 3; ++k)
        if (amx[k] < bmn[k] - pad || amn[k] > bmx[k] + pad) return 0;
    return 1;
}

int hs_in_solid(const Solid *s, const double p[3], double in) {
    for (int j = 0; j < s->np; ++j)
        if (dot3(s->pl[j].n, p) > s->pl[j].d - in) return 0;
    return 1;
}

int hs_inside(const Ctx *C, const double p[3], int skip, double in) {
    for (int i = 0; i < C->ns; ++i) {
        const Solid *s = &C->s[i];
        if (i == skip || s->neg || !s->live) continue;
        if (p[0] < s->mn[0] || p[0] > s->mx[0] || p[1] < s->mn[1] || p[1] > s->mx[1] || p[2] < s->mn[2] || p[2] > s->mx[2]) continue;
        if (hs_in_solid(s, p, in)) return i;
    }
    return -1;
}

/* The dressing's records, grown as the shell is built (kit only). */
#define GROW(arr, n, cap) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 32; (arr) = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); if (!(arr)) abort(); } } while (0)

/* The parts of piece P (normal nf) outside solid T, into out (at most max); returns how many. tie: T was made
 * first, so where P lies on one of T's faces looking the same way, P gives way. Openings (T->neg) shrink by
 * EPS_S, so the face round an opening runs on under its reveals. */
static int subtract(const SPoly *P, const double *nf, const Solid *T, int tie, int exact, SPoly *out, int max, SPoly *tmp) {
    double dd[MAXP];
    int on = -1;
    for (int j = 0; j < T->np && on < 0; ++j) {
        const double c = dot3(T->pl[j].n, nf);
        if (fabs(c) < 0.999) continue;
        int all = 1;
        for (int q = 0; q < P->n && all; ++q) all = fabs(dot3(T->pl[j].n, P->p[q]) - T->pl[j].d) < ON_TOL;
        if (!all) continue;
        if (!T->neg) {
            /* on T's face, looking out of it: P stays if its solid came first, or T has no face there to show */
            if (c > 0.0 && (!tie || T->surf[j] == NOFACE)) { out[0] = *P; return 1; }
        } else if (c > 0.0 || !tie) {
            /* an opening's face on another opening's side (a door's threshold on a recessed porch's floor): the
             * earlier keeps what they share */
            out[0] = *P;
            return 1;
        }
        on = j;
    }
    for (int j = 0; j < T->np; ++j) dd[j] = on >= 0 || exact ? T->pl[j].d : T->pl[j].d - EPS_S;
    if (on >= 0) dd[on] = T->pl[on].d + EPS_N;
    /* wholly outside one of T's planes: all of P stays */
    for (int j = 0; j < T->np; ++j) {
        int outside = 1;
        for (int q = 0; q < P->n && outside; ++q) outside = dot3(T->pl[j].n, P->p[q]) >= dd[j] - 1e-9;
        if (outside) { out[0] = *P; return 1; }
    }
    SPoly rem = *P;
    int k = 0;
    for (int j = 0; j < T->np; ++j) {
        clip_poly(&rem, T->pl[j].n, dd[j], -1.0, tmp);
        poly_weld(tmp);
        if (poly_ok(tmp, nf) && k < max) out[k++] = *tmp;
        clip_poly(&rem, T->pl[j].n, dd[j], 1.0, tmp);
        poly_weld(tmp);
        if (!poly_ok(tmp, nf)) return k;
        rem = *tmp;
    }
    return k;   /* what is left lies inside T */
}

/* ---------------------------------------------------------------------------
 * Emitting a face: its surface, finish, colour, uv and aux
 * ------------------------------------------------------------------------- */

static void emit_piece(Ctx *C, const Solid *s, int surf, const SPoly *P, const double *n, double u0, double ulen) {
    HShellFace f;
    memset(&f, 0, sizeof(f));
    const int unit = s->unit >= 0 ? s->unit : 0;
    const double dy = C->dy ? C->dy[unit] : 0.0;
    f.n = P->n < HSH_MAXV ? P->n : HSH_MAXV;
    f.surf = (uint8_t)surf;
    f.unit = (int8_t)unit;
    for (int k = 0; k < 3; ++k) f.nrm[k] = (float)n[k];
    switch (surf) {
    case HF_ROOF:    f.finish = s->roofFin; f.colour = s->roofCol; break;
    case HF_FLAT:    f.finish = C->fin[unit].flat; f.colour = C->fin[unit].flatCol; break;
    case HF_SOFFIT: case HF_FASCIA: case HF_TRIM: case HF_CEILING: f.colour = s->trimCol; break;
    case HF_DOOR:    f.colour = C->fin[unit].doorCol; break;
    case HF_GLASS:   f.colour = s->trimCol; break;
    case HF_GARAGE:  f.colour = C->fin[unit].garageCol; break;
    case HF_STEP:    f.finish = s->stepFin; break;
    default:         f.finish = s->wallFin; f.colour = s->wallCol; break;
    }
    if (surf == HF_GLASS || surf == HF_DOOR || surf == HF_GARAGE) f.aux = s->aux;
    /* the uv frame */
    double h[3] = { n[2], 0.0, -n[0] }, b[3];
    const int vertical = fabs(n[1]) < 0.05, flat = fabs(n[1]) > 0.95;
    norm3(h);
    b[0] = -n[0] * n[1]; b[1] = 1.0 - n[1] * n[1]; b[2] = -n[2] * n[1];
    norm3(b);
    const int opening = surf == HF_GLASS || surf == HF_DOOR || surf == HF_GARAGE;
    for (int q = 0; q < f.n; ++q) {
        const double *p = P->p[q];
        double y = p[1] - dy;
        f.p[q][0] = (float)p[0]; f.p[q][1] = (float)p[1]; f.p[q][2] = (float)p[2];
        if (opening) {
            const double r[3] = { p[0] - s->o[0], p[1] - s->o[1], p[2] - s->o[2] };
            f.uv[q][0] = (float)dot3(r, s->t); f.uv[q][1] = (float)r[1];
        } else if (flat) {
            f.uv[q][0] = (float)p[0]; f.uv[q][1] = (float)p[2];
        } else if (vertical) {
            f.uv[q][0] = (float)(dot3(p, h) - u0); f.uv[q][1] = (float)y;
        } else {
            const double pp[3] = { p[0], y, p[2] };
            f.uv[q][0] = (float)dot3(pp, h); f.uv[q][1] = (float)dot3(pp, b);
        }
    }
    if (C->lod == HSH_FAR) {
        f.aux |= HOUSE_AUX_FAR;
        if (surf == HF_WALL && vertical && s->aux) {
            const uint32_t len = (uint32_t)fmin(1023.0, ulen * 10.0 + 0.5);
            f.aux |= s->aux | HOUSE_AUX_WINDOWS | len << HOUSE_AUX_LENGTH;
        }
    }
    C->fn(C->ctx, &f);
    C->st.faces++;
    C->st.tris += f.n - 2;
}

/* Every face of every solid, less the others. */
static void emit_all(Ctx *C) {
    SPoly face, tmp;
    for (int si = 0; si < C->ns; ++si) {
        const Solid *s = &C->s[si];
        if (!s->live) continue;
        for (int i = 0; i < s->np; ++i) {
            if (s->surf[i] == NOFACE || !solid_face(s, i, &face, &tmp)) continue;
            double n[3] = { s->pl[i].n[0], s->pl[i].n[1], s->pl[i].n[2] };
            /* a far wall's windows are spread along the whole face, before it is cut */
            double h[3] = { n[2], 0.0, -n[0] }, u0 = 1e30, u1 = -1e30;
            norm3(h);
            for (int q = 0; q < face.n; ++q) { const double u = dot3(face.p[q], h); u0 = fmin(u0, u); u1 = fmax(u1, u); }
            Solid sw = *s;
            if (s->noWin[i]) sw.aux = 0;
            if (s->neg) {
                /* an opening's face, turned in, inside its host */
                for (int q = 0; q < face.n / 2; ++q) {
                    double t3[3];
                    memcpy(t3, face.p[q], sizeof(t3));
                    memcpy(face.p[q], face.p[face.n - 1 - q], sizeof(t3));
                    memcpy(face.p[face.n - 1 - q], t3, sizeof(t3));
                }
                for (int k = 0; k < 3; ++k) n[k] = -n[k];
                const Solid *H = &C->s[s->host];
                for (int j = 0; j < H->np && face.n >= 3; ++j) {
                    clip_poly(&face, H->pl[j].n, H->pl[j].d, 1.0, &tmp);
                    face = tmp;
                }
                poly_weld(&face);
                if (!poly_ok(&face, n)) continue;
            }
            int np = 1;
            C->piece[0] = face;
            double fmn[3] = { 1e30, 1e30, 1e30 }, fmx[3] = { -1e30, -1e30, -1e30 };
            for (int q = 0; q < face.n; ++q)
                for (int k = 0; k < 3; ++k) { fmn[k] = fmin(fmn[k], face.p[q][k]); fmx[k] = fmax(fmx[k], face.p[q][k]); }
            for (int ti = 0; ti < C->ns && np; ++ti) {
                const Solid *T = &C->s[ti];
                if (ti == si || !T->live || (s->neg && !T->neg) || !boxes_meet(fmn, fmx, T->mn, T->mx, 0.01)) continue;
                int nn = 0;
                for (int p = 0; p < np && nn < C->maxPieces; ++p)
                    nn += subtract(&C->piece[p], n, T, ti < si, s->neg && T->neg, C->next + nn, C->maxPieces - nn, &tmp);
                SPoly *sw2 = C->piece; C->piece = C->next; C->next = sw2;
                np = nn;
            }
            for (int p = 0; p < np; ++p) emit_piece(C, &sw, s->surf[i], &C->piece[p], n, u0, u1 - u0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * The house
 * ------------------------------------------------------------------------- */

static void foot_of(const float (*q)[2], int n, Foot *F) {
    double cx = 0.0, cz = 0.0;
    F->n = n;
    for (int i = 0; i < n; ++i) { F->p[i][0] = q[i][0]; F->p[i][1] = q[i][1]; cx += q[i][0] / n; cz += q[i][1] / n; }
    for (int i = 0; i < n; ++i) {
        const double *a = F->p[i], *b = F->p[(i + 1) % n];
        double mx = b[1] - a[1], mz = -(b[0] - a[0]);
        const double l = sqrt(mx * mx + mz * mz);
        if (l < 1e-9) { F->m[i][0] = F->m[i][1] = 0.0; continue; }
        mx /= l; mz /= l;
        if (mx * (0.5 * (a[0] + b[0]) - cx) + mz * (0.5 * (a[1] + b[1]) - cz) < 0.0) { mx = -mx; mz = -mz; }
        F->m[i][0] = mx; F->m[i][1] = mz;
    }
}

/* The roof plane over eaves edge e: y <= E + t * (distance in from the edge). */
static void eaves_plane(const Roof *R, int e, double off, double out[4]);
void hs_eaves_plane(const Roof *R, int e, double off, double out[4]) { eaves_plane(R, e, off, out); }
static void eaves_plane(const Roof *R, int e, double off, double out[4]) {
    const double mx = R->F.m[e][0], mz = R->F.m[e][1], a = mx * R->F.p[e][0] + mz * R->F.p[e][1];
    double n[3] = { R->t * mx, 1.0, R->t * mz };
    const double l = sqrt(dot3(n, n));
    out[0] = n[0] / l; out[1] = n[1] / l; out[2] = n[2] / l;
    out[3] = (R->E + off + R->t * a) / l;
}

/* The horizontal half-space where eaves plane e lies at or under plane f (their hip or ridge). */
static void under_plane(const Roof *R, int e, int f, double out[4]) {
    const double ae = R->F.m[e][0] * R->F.p[e][0] + R->F.m[e][1] * R->F.p[e][1];
    const double af = R->F.m[f][0] * R->F.p[f][0] + R->F.m[f][1] * R->F.p[f][1];
    double n[3] = { R->t * (R->F.m[f][0] - R->F.m[e][0]), 0.0, R->t * (R->F.m[f][1] - R->F.m[e][1]) };
    const double l = sqrt(dot3(n, n));
    if (l < 1e-9) { out[0] = out[1] = out[2] = 0.0; out[3] = 1.0; return; }
    out[0] = n[0] / l; out[1] = 0.0; out[2] = n[2] / l;
    out[3] = R->t * (af - ae) / l;
}

static void add_plane4(Solid *s, const double *p4, int surf) {
    if (s->np >= MAXP || (p4[0] == 0.0 && p4[1] == 0.0 && p4[2] == 0.0)) return;
    SPlane *p = &s->pl[s->np];
    p->n[0] = p4[0]; p->n[1] = p4[1]; p->n[2] = p4[2]; p->d = p4[3];
    s->surf[s->np] = (uint8_t)surf;
    s->noWin[s->np] = 0;
    s->np++;
}

/* A vertical plane along footprint edge e, pushed out by off. */
static void edge_plane(Solid *s, const Foot *F, int e, double off, int surf) {
    const double mx = F->m[e][0], mz = F->m[e][1];
    plane_at(s, mx, 0.0, mz, F->p[e][0] + mx * off, 0.0, F->p[e][1] + mz * off, surf);
}

static int edge_along_x(const Foot *F, int e) { return fabs(F->m[e][1]) > fabs(F->m[e][0]); }
static int side_of(const Foot *F, int e) {   /* HSIDE_* of an edge (by its normal) */
    const double mx = F->m[e][0], mz = F->m[e][1];
    return fabs(mx) > fabs(mz) ? (mx < 0.0 ? HSIDE_W : HSIDE_E) : (mz < 0.0 ? HSIDE_S : HSIDE_N);
}

static int habitable(const HVol *v) { return v->role <= HV_EXTENSION || v->role == HV_OUTRIGGER || v->role == HV_BAY; }

/* A far body's walls' window rhythm (house_surf.h), or 0 for a garage or a porch. */
static uint32_t window_rhythm(const HGraph *g, const HVol *v) {
    if (!habitable(v) || (v->role == HV_WING && v->kind == HWING_GARAGE)) return 0;
    const uint32_t sh = (uint32_t)fmax(0.0, fmin(127.0, (g->storeyH - 2.0f) * 100.0f + 0.5f));
    const uint32_t pl = (uint32_t)fmax(0.0, fmin(255.0, g->plinth * 100.0f + 0.5f));
    return sh << HOUSE_AUX_STOREYH | pl << HOUSE_AUX_PLINTH | (uint32_t)((v->storeys > 0 ? v->storeys - 1 : 0) & 3) << HOUSE_AUX_STOREYS;
}

/* The volume's body: its walls up to its roof's planes, from yb (ground, or a porch's eaves: its attic). */
static int add_body(Ctx *C, const HVol *v, int vi, int unit, const Roof *R, double yb, int ceiling) {
    Solid *s = solid_new(C, unit, vi, 0);
    s->kind = SK_BODY;
    const int far = C->lod == HSH_FAR;
    for (int e = 0; e < R->W.n; ++e) {
        edge_plane(s, &R->W, e, 0.0, v->role == HV_PORCH ? HF_TRIM : HF_WALL);
        s->noWin[s->np - 1] = (uint8_t)(R->cut[e] || R->role[e] == ER_HIGH || R->role[e] == ER_EXT);
    }
    /* a roof extended into its parent: its body runs in too, out of sight */
    for (int e = 0; e < R->F.n; ++e)
        if (R->role[e] == ER_EXT) { s->np = 0; break; }
    if (s->np == 0)
        for (int e = 0; e < R->F.n; ++e) {
            edge_plane(s, &R->F, e, 0.0, v->role == HV_PORCH ? HF_TRIM : HF_WALL);
            s->noWin[s->np - 1] = (uint8_t)(R->cut[e] || R->role[e] == ER_HIGH || R->role[e] == ER_EXT);
        }
    plane_at(s, 0, -1, 0, 0, yb, 0, ceiling ? HF_CEILING : NOFACE);
    if (R->kind == HR_FLAT) {
        const double top = far && v->parapet == 2 ? v->top : R->E;
        plane_at(s, 0, 1, 0, 0, top, 0, v->parapet == 2 || far ? HF_FLAT : NOFACE);
    } else {
        for (int e = 0; e < R->F.n; ++e) {
            if (R->role[e] != ER_EAVES) continue;
            double p4[4];
            eaves_plane(R, e, 0.0, p4);
            add_plane4(s, p4, far ? HF_ROOF : NOFACE);
        }
    }
    if (far) s->aux = window_rhythm(C->g, v);
    return (int)(s - C->s);
}

/* A slab of a row's main cut to its unit's slice. */
static void slab_cut(Solid *s, const Roof *R) {
    if (R->cx0 > -1e29) plane_at(s, -1, 0, 0, R->cx0, 0, 0, NOFACE);
    if (R->cx1 < 1e29) plane_at(s, 1, 0, 0, R->cx1, 0, 0, NOFACE);
}

/* The roof's slabs: one a pitched plane (its covering, soffit, fascia and verges), or a flat one's. */
static void add_slabs(Ctx *C, const HVol *v, int vi, int unit, const Roof *R) {
    if (C->lod == HSH_FAR) return;
    if (R->kind == HR_FLAT) {
        if (v->parapet == 2) return;   /* a deck inside its parapets (add_parapets) */
        Solid *s = solid_new(C, unit, vi, 0);
        s->kind = SK_SLAB;
        for (int e = 0; e < R->F.n; ++e)
            edge_plane(s, &R->F, e, R->role[e] == ER_HIGH || R->role[e] == ER_EXT ? R->ext[e] : R->ov,
                       R->role[e] == ER_HIGH || R->role[e] == ER_EXT ? NOFACE : HF_FASCIA);
        slab_cut(s, R);
        plane_at(s, 0, 1, 0, 0, v->top, 0, HF_FLAT);
        plane_at(s, 0, -1, 0, 0, R->E, 0, HF_SOFFIT);
        return;
    }
    for (int e = 0; e < R->F.n; ++e) {
        if (R->role[e] != ER_EAVES) continue;
        Solid *s = solid_new(C, unit, vi, 0);
        s->kind = SK_SLAB;
        double p4[4];
        eaves_plane(R, e, 0.0, p4);
        add_plane4(s, p4, HF_ROOF);
        eaves_plane(R, e, -SLAB_T, p4);
        p4[0] = -p4[0]; p4[1] = -p4[1]; p4[2] = -p4[2]; p4[3] = -p4[3];
        add_plane4(s, p4, HF_SOFFIT);
        edge_plane(s, &R->F, e, R->ext[e], HF_FASCIA);   /* R->ov, but none over a neighbour's (roof_of) */
        for (int f = 0; f < R->F.n; ++f) {
            if (f == e) continue;
            if (R->role[f] == ER_EAVES) { under_plane(R, e, f, p4); add_plane4(s, p4, NOFACE); }
            else edge_plane(s, &R->F, f, R->ext[f], R->role[f] == ER_VERGE ? HF_FASCIA : NOFACE);
        }
        slab_cut(s, R);
    }
}

/* Whether the unit across a row's party wall (the slice's cut edge e) stands lower: the deck's edge there wants a
 * parapet of its own (an unevenly built flat row, HM5e1). */
static int cut_over_lower(const Ctx *C, int vi, int unit, const Roof *R, int e) {
    const HGraph *g = C->g;
    const int side = side_of(&R->W, e), nb = side == HSIDE_W ? unit - 1 : side == HSIDE_E ? unit + 1 : -1;
    if (nb < 0 || nb >= g->nunit) return 0;
    HVol a, b;
    const HVol *va = home_unit_vol(g, vi, unit, &a), *vb = home_unit_vol(g, vi, nb, &b);
    const double da = C->dy ? C->dy[unit] : 0.0, db = C->dy ? C->dy[nb] : 0.0;
    return vb->top + db < va->eaves + da - 0.05;
}

/* The sheets' crenellations (HM5e1: the courtyard house's and the Middle East's villa's parapets): merlons along the
 * coping of parapet edge e, 0.4 m square in section, a crenel of the same between, the street's front and its ends. */
static void add_merlons(Ctx *C, int vi, int unit, const Roof *R, int e, double top) {
    const double mx = R->W.m[e][0], mz = R->W.m[e][1], ex = -mz, ez = mx;
    const double *a = R->W.p[e], *b = R->W.p[(e + 1) % R->W.n];
    const double sa = ex * a[0] + ez * a[1], sb = ex * b[0] + ez * b[1], at = mx * a[0] + mz * a[1];
    const double s0 = fmin(sa, sb), s1 = fmax(sa, sb), L = s1 - s0;
    const int n = (int)floor((L + 0.4) / 0.8);
    if (n < 2) return;
    const double pitch = (L - 0.4) / (n - 1);
    for (int k = 0; k < n; ++k) {
        const double c = s0 + 0.2 + pitch * k;
        Solid *s = solid_new(C, unit, vi, 0);
        plane_at(s, mx, 0, mz, mx * at, 0, mz * at, HF_WALL);
        s->noWin[s->np - 1] = 1;
        plane_at(s, -mx, 0, -mz, mx * (at - PARAPET_W), 0, mz * (at - PARAPET_W), HF_WALL);
        s->noWin[s->np - 1] = 1;
        plane_at(s, ex, 0, ez, ex * (c + 0.2), 0, ez * (c + 0.2), HF_WALL);
        s->noWin[s->np - 1] = 1;
        plane_at(s, -ex, 0, -ez, ex * (c - 0.2), 0, ez * (c - 0.2), HF_WALL);
        s->noWin[s->np - 1] = 1;
        plane_at(s, 0, 1, 0, 0, top + 0.4, 0, HF_COPING);
        plane_at(s, 0, -1, 0, 0, top - 0.02, 0, NOFACE);
    }
}
static int style_crenellated(int style) { return style == HS_COURTYARD || style == HS_MENA_VILLA; }

/* Parapets round a flat roof's deck (and a georgian front's), their copings on top. sliced: a row's main cut to a
 * unit (its cut edges over a lower neighbour take a parapet too). */
static void add_parapets(Ctx *C, const HVol *v, int vi, int unit, const Roof *R, int sliced) {
    if (C->lod == HSH_FAR) return;
    for (int e = 0; e < R->W.n; ++e) {
        if (v->parapet == 1 && side_of(&R->W, e) != HSIDE_S) continue;
        if (v->parapet != 1 && (v->roof != HR_FLAT || v->parapet != 2)) return;
        if ((R->cut[e] && !(sliced && v->parapet == 2 && cut_over_lower(C, vi, unit, R, e))) || R->role[e] == ER_HIGH || R->role[e] == ER_EXT) continue;
        Solid *s = solid_new(C, unit, vi, 0);
        edge_plane(s, &R->W, e, 0.0, HF_WALL);
        s->noWin[s->np - 1] = 1;
        const double mx = R->W.m[e][0], mz = R->W.m[e][1];
        plane_at(s, -mx, 0.0, -mz, R->W.p[e][0] - mx * PARAPET_W, 0.0, R->W.p[e][1] - mz * PARAPET_W, HF_WALL);
        s->noWin[s->np - 1] = 1;
        for (int f = 0; f < R->W.n; ++f) {
            if (f == e || fabs(R->W.m[f][0] * mx + R->W.m[f][1] * mz) >= 0.5) continue;
            edge_plane(s, &R->W, f, 0.0, HF_WALL);
            s->noWin[s->np - 1] = 1;
        }
        const double top = v->parapet == 1 ? R->E + 0.8 : v->top;
        plane_at(s, 0, 1, 0, 0, top, 0, HF_COPING);
        plane_at(s, 0, -1, 0, 0, v->parapet == 1 ? R->E - 0.3 : R->E, 0, NOFACE);
        /* the street's front and its returns crenellated; the back plain */
        if (v->parapet == 2 && style_crenellated(C->g->style) && (v->role <= HV_ROW) && side_of(&R->W, e) != HSIDE_N && !R->cut[e])
            add_merlons(C, vi, unit, R, e, top);
    }
}

/* The roof of volume v: its planes from its whole outline F (a row's main's, not a unit's slice of it: its
 * hips stand at the row's ends), its walls on W (the slice), cut at x0 where cutW, at x1 where cutE. */
static void roof_of(const HGraph *g, const HVol *v, const Foot *W, const Foot *F, int cutW, int cutE, Roof *R) {
    memset(R, 0, sizeof(*R));
    R->W = *W;
    R->F = *F;
    R->cx0 = -1e30; R->cx1 = 1e30;
    for (int e = 0; e < W->n; ++e) {
        const int side = side_of(W, e);
        if (side == HSIDE_W && cutW) { R->cut[e] = 1; R->cx0 = W->p[e][0]; }
        if (side == HSIDE_E && cutE) { R->cut[e] = 1; R->cx1 = W->p[e][0]; }
    }
    R->kind = v->roof;
    R->E = v->eaves;
    R->t = tan(v->pitch * 0.017453292519943295);
    R->ov = fmax(v->overhang, v->role == HV_PORCH ? 0.12 : 0.04);
    /* the verges: half the eaves' overhang, to 0.3 m; a craftsman's gables reach as far as its eaves (deep verges on
     * knee braces, HM5) */
    const double ovg = g->style == HS_CRAFTSMAN && v->roof == HR_GABLE ? fmin(0.75, fmax(0.3, v->overhang)) : fmin(0.3, fmax(0.05, 0.5 * v->overhang));
    const HVol *par = v->parent >= 0 ? &g->vol[v->parent] : NULL;
    for (int e = 0; e < F->n; ++e) {
        const int side = side_of(F, e);
        int role;
        switch (v->roof) {
        case HR_GABLE: role = (edge_along_x(F, e) == (v->ridgeX != 0)) ? ER_EAVES : ER_VERGE; break;
        case HR_HIP:   role = ER_EAVES; break;
        case HR_LEAN: {
            const double hx = v->high == HSIDE_E ? 1.0 : v->high == HSIDE_W ? -1.0 : 0.0, hz = v->high == HSIDE_N ? 1.0 : v->high == HSIDE_S ? -1.0 : 0.0;
            const double c = F->m[e][0] * hx + F->m[e][1] * hz;
            role = c > 0.9 ? ER_HIGH : c < -0.3 ? ER_EAVES : ER_VERGE;
            break;
        }
        default: role = ER_VERGE; break;
        }
        R->ext[e] = role == ER_VERGE ? ovg : role == ER_EAVES ? R->ov : 0.15;
        /* a unit's own part against a party wall (an outrigger back to back with its neighbour's): its verge or eaves
         * stops at the wall, never over the neighbour's (HM5e1: the townhouses' tall outriggers met there) */
        if ((role == ER_VERGE || role == ER_EAVES) && v->unit != HG_ROW_WIDE && v->unit < g->nunit) {
            const int u = v->unit;
            if ((side == HSIDE_W && u > 0 && fabs(v->x0 - g->unit[u].x0) < 0.02) || (side == HSIDE_E && u + 1 < g->nunit && fabs(v->x1 - g->unit[u].x1) < 0.02))
                R->ext[e] = 0.0;
        }
        /* the edge against its parent: a gabled outrigger's or porch's roof runs on into the parent's, to its
         * ridge, where that is pitched across it; anything else stops just inside the parent's wall */
        if (par && role != ER_EAVES) {
            const int touch = (side == HSIDE_S && fabs(v->z0 - par->z1) < 0.05f) || (side == HSIDE_N && fabs(v->z1 - par->z0) < 0.05f) ||
                              (side == HSIDE_W && fabs(v->x0 - par->x1) < 0.05f) || (side == HSIDE_E && fabs(v->x1 - par->x0) < 0.05f);
            if (touch) {
                double run = 0.15;
                if (v->roof == HR_GABLE && (par->roof == HR_GABLE || par->roof == HR_HIP) && (par->ridgeX != 0) == (side == HSIDE_S || side == HSIDE_N))
                    run = 0.5 * (par->ridgeX ? par->z1 - par->z0 : par->x1 - par->x0);
                role = role == ER_HIGH ? ER_HIGH : ER_EXT;
                R->ext[e] = run;
            }
        }
        R->role[e] = (uint8_t)role;
    }
    /* a roof run on into its parent: its footprint moved in */
    for (int e = 0; e < R->F.n; ++e) {
        if (R->role[e] != ER_EXT) continue;
        const int a = e, b = (e + 1) % R->F.n;
        const double mx = R->F.m[e][0], mz = R->F.m[e][1], run = R->ext[e];
        R->F.p[a][0] += mx * run; R->F.p[a][1] += mz * run;
        R->F.p[b][0] += mx * run; R->F.p[b][1] += mz * run;
        R->ext[e] = 0.0;
    }
}

static void foot_rect(double x0, double z0, double x1, double z1, Foot *F) {
    const float q[4][2] = { { (float)x0, (float)z0 }, { (float)x1, (float)z0 }, { (float)x1, (float)z1 }, { (float)x0, (float)z1 } };
    foot_of(q, 4, F);
}

/* One volume (or a slice of a row's main): body, roof, parapets. */
static void add_volume(Ctx *C, int vi, int unit, double sx0, double sx1, int cutW, int cutE) {
    const HGraph *g = C->g;
    HVol own;
    const HVol *v = home_unit_vol(g, vi, unit, &own);   /* a unevenly built row's unit at its own storeys (HM5e1) */
    Foot W, F;
    float q[6][2];
    const int n = hvol_outline(v, q);
    foot_of((const float (*)[2])q, n, &F);
    if (sx0 < sx1) foot_rect(sx0, v->z0, sx1, v->z1, &W);
    else W = F;
    Roof R;
    roof_of(g, v, &W, &F, cutW, cutE, &R);
    const int first = C->ns;
    const double dy = C->dy ? C->dy[unit] : 0.0;
    if (C->kit && C->lod == HSH_NEAR && !(v->role == HV_PORCH && (home_porch_recessed(v->kind) || v->kind == HP_CANOPY))) {
        GROW(C->roof, C->nroof, C->caproof);
        HsRoof *hr = &C->roof[C->nroof++];
        hr->vol = (int8_t)vi; hr->unit = (int8_t)unit; hr->dy = dy; hr->R = R;
    }
    if (v->role == HV_PORCH) {
        if (home_porch_recessed(v->kind)) return;   /* an opening (add_front) */
        if (v->kind == HP_CANOPY && C->kit) return;   /* the kit's canopy on its brackets (home_character.c) */
        if (C->lod == HSH_FAR && v->kind != HP_ENCLOSED) return;
        /* its attic: a pediment and a ceiling (a flat-roofed one has none: its slab's soffit is its ceiling) */
        if (v->kind == HP_OPEN && R.kind != HR_FLAT) C->body[vi][unit] = add_body(C, v, vi, unit, &R, R.E, 1);
        else if (v->kind == HP_ENCLOSED) C->body[vi][unit] = add_body(C, v, vi, unit, &R, 0.0, 0);
        add_slabs(C, v, vi, unit, &R);
    } else {
        C->body[vi][unit] = add_body(C, v, vi, unit, &R, v->y0, 0);
        add_slabs(C, v, vi, unit, &R);
        add_parapets(C, v, vi, unit, &R, sx0 < sx1);
    }
    for (int i = first; i < C->ns; ++i) solid_lift(&C->s[i], dy);
}

/* A party wall's upstand through a stepped (or a style's) row's roof, or a flat row's dividing parapet. */
static void add_upstand(Ctx *C, const HParty *p) {
    const HGraph *g = C->g;
    const HVol *v = &g->vol[p->vol];
    const double da = C->dy ? C->dy[p->a] : 0.0, db = C->dy ? C->dy[p->b] : 0.0;
    const int st = g->style == HS_VICTORIAN || g->style == HS_GEORGIAN || g->style == HS_TOWNHOUSE || g->style == HS_SHOPHOUSE ||
                   g->style == HS_CHINA || g->style == HS_ROWHOUSE;
    if (C->lod == HSH_FAR || (v->roof != HR_FLAT && fabs(da - db) < 1e-3 && !st)) return;
    const int hi = da >= db ? p->a : p->b;
    /* an unevenly built flat row: the dividing parapet stands to the lower unit's top (the taller one's deck has its
     * own parapet over it: add_parapets) */
    HVol ua, ub;
    const HVol *va = home_unit_vol(g, p->vol, p->a, &ua), *vb = home_unit_vol(g, p->vol, p->b, &ub);
    if (v->roof == HR_FLAT && va->storeys != vb->storeys) {
        const int lo = va->top + da <= vb->top + db ? p->a : p->b;
        const HVol *vl = lo == p->a ? va : vb;
        Solid *s = solid_new(C, lo, p->vol, 0);
        plane_at(s, -1, 0, 0, p->x - 0.12, 0, 0, HF_WALL); s->noWin[s->np - 1] = 1;
        plane_at(s, 1, 0, 0, p->x + 0.12, 0, 0, HF_WALL);  s->noWin[s->np - 1] = 1;
        plane_at(s, 0, 0, -1, 0, 0, v->z0, HF_WALL);       s->noWin[s->np - 1] = 1;
        plane_at(s, 0, 0, 1, 0, 0, v->z1, HF_WALL);        s->noWin[s->np - 1] = 1;
        plane_at(s, 0, -1, 0, 0, vl->eaves - 0.5, 0, NOFACE);
        plane_at(s, 0, 1, 0, 0, vl->top, 0, HF_COPING);
        solid_lift(s, lo == p->a ? da : db);
        return;
    }
    Solid *s = solid_new(C, hi, p->vol, 0);
    const double half = v->roof == HR_FLAT ? 0.12 : 0.15;
    plane_at(s, -1, 0, 0, p->x - half, 0, 0, HF_WALL); s->noWin[s->np - 1] = 1;
    plane_at(s, 1, 0, 0, p->x + half, 0, 0, HF_WALL);  s->noWin[s->np - 1] = 1;
    plane_at(s, 0, 0, -1, 0, 0, v->z0, HF_WALL);       s->noWin[s->np - 1] = 1;
    plane_at(s, 0, 0, 1, 0, 0, v->z1, HF_WALL);        s->noWin[s->np - 1] = 1;
    plane_at(s, 0, -1, 0, 0, v->eaves - 0.5, 0, NOFACE);
    if (v->roof == HR_FLAT) plane_at(s, 0, 1, 0, 0, v->top, 0, HF_COPING);
    else {
        Foot W;
        foot_rect(v->x0, v->z0, v->x1, v->z1, &W);
        Roof R;
        roof_of(g, v, &W, &W, 0, 0, &R);
        for (int e = 0; e < R.F.n; ++e) {
            if (R.role[e] != ER_EAVES) continue;
            double p4[4];
            eaves_plane(&R, e, UPSTAND, p4);
            add_plane4(s, p4, HF_COPING);
        }
    }
    solid_lift(s, da >= db ? da : db);
}

/* A chimney stack and its cap; a stack outside its gable wall stands on the ground. */
static void add_stack(Ctx *C, const HChimney *k) {
    const HGraph *g = C->g;
    const HVol *v = &g->vol[k->vol];
    int unit = k->unit >= 0 && k->unit < g->nunit ? k->unit : 0;
    if (k->on == HC_PARTY && unit + 1 < g->nunit && C->dy && C->dy[unit + 1] > C->dy[unit]) unit++;
    const double x0 = k->x - 0.5 * k->w, x1 = k->x + 0.5 * k->w, z0 = k->z - 0.5 * k->d, z1 = k->z + 0.5 * k->d;
    const int outside = x1 < v->x0 + 0.01f || x0 > v->x1 - 0.01f || z1 < v->z0 + 0.01f || z0 > v->z1 - 0.01f;
    const double yb = outside ? 0.0 : v->eaves - 0.6, top = k->top;
    const double dy = C->dy ? C->dy[unit] : 0.0;
    Solid *s = solid_new(C, unit, k->vol, 0);
    s->wallFin = C->fin[unit].stack; s->wallCol = C->fin[unit].stackCol;
    if (C->lod == HSH_FAR) { box_planes(s, x0, yb, z0, x1, top, z1, HF_CHIMNEY, HF_COPING, NOFACE); solid_lift(s, dy); return; }
    if (C->kit) {
        /* the kit's corbelled cap and its pots (hk_chimney_top) stand over the stack's top */
        box_planes(s, x0, yb, z0, x1, top, z1, HF_CHIMNEY, HF_COPING, NOFACE);
        solid_lift(s, dy);
        GROW(C->stack, C->nstack, C->capstack);
        HsStack *hs = &C->stack[C->nstack++];
        hs->unit = (int8_t)unit; hs->chim = (int8_t)(k - g->chim);
        hs->x = k->x; hs->z = k->z; hs->w = k->w; hs->d = k->d; hs->top = top + dy;
        return;
    }
    box_planes(s, x0, yb, z0, x1, top - 0.12, z1, HF_CHIMNEY, NOFACE, NOFACE);
    solid_lift(s, dy);
    Solid *c = solid_new(C, unit, k->vol, 0);
    c->wallFin = C->fin[unit].stack; c->wallCol = C->fin[unit].stackCol;
    box_planes(c, x0 - 0.06, top - 0.18, z0 - 0.06, x1 + 0.06, top, z1 + 0.06, HF_COPING, HF_COPING, HF_COPING);
    solid_lift(c, dy);
}

/* ---------------------------------------------------------------------------
 * Openings, steps, posts (near)
 * ------------------------------------------------------------------------- */

/* An opening in host's wall: along the wall (its outward normal m, the way across it e) from a, across w,
 * from y0 to y1, set back `depth` behind the wall's face (at `wall`, its offset along m). Its faces: the
 * back's `back`, the rest reveals. */
static int add_opening_arch(Ctx *C, int host, int unit, const double m[2], const double e[2], double ax, double az, double w,
                            double y0, double y1, double depth, int back, uint32_t aux, double rise);
static int add_opening(Ctx *C, int host, int unit, const double m[2], const double e[2], double ax, double az, double w,
                       double y0, double y1, double depth, int back, uint32_t aux) {
    return add_opening_arch(C, host, unit, m, e, ax, az, w, y0, y1, depth, back, aux, 0.0);
}
/* ...with an arched head (HM5e1): y1 its springing, rise its arch's over that (a half circle at most, w / 2), the head
 * a polygon of tangent planes round the arc (convex, as every solid here). */
static int add_opening_arch(Ctx *C, int host, int unit, const double m[2], const double e[2], double ax, double az, double w,
                            double y0, double y1, double depth, int back, uint32_t aux, double rise) {
    if (host < 0) return -1;
    Solid *s = solid_new(C, unit, C->s[host].vol, 1);
    s->host = host;
    s->aux = aux;
    const double out = 0.02;
    /* front (open), back, the two jambs, head and sill */
    plane_at(s, m[0], 0, m[1], ax + m[0] * out, 0, az + m[1] * out, NOFACE);
    plane_at(s, -m[0], 0, -m[1], ax - m[0] * depth, 0, az - m[1] * depth, back);
    plane_at(s, -e[0], 0, -e[1], ax, 0, az, HF_REVEAL);
    plane_at(s, e[0], 0, e[1], ax + e[0] * w, 0, az + e[1] * w, HF_REVEAL);
    plane_at(s, 0, 1, 0, 0, y1 + rise, 0, HF_REVEAL);
    plane_at(s, 0, -1, 0, 0, y0, 0, back == HF_GARAGE ? NOFACE : HF_REVEAL);
    if (rise > 0.02) {
        /* the arc through both springings and the crown: radius R, its centre under the crown */
        const double half = 0.5 * w, r = fmin(rise, half), R = (half * half + r * r) / (2.0 * r);
        const double cs = 0.5 * w, cy = y1 + r - R;                       /* centre: along the opening, up */
        const double t0 = asin(fmax(-1.0, fmin(1.0, (y1 - cy) / R)));    /* the springing's angle over the horizontal */
        const int K = r > 0.45 * w ? 8 : 5;
        for (int k = 0; k <= K; ++k) {
            const double t = t0 + (PI_D - 2.0 * t0) * k / K;
            const double ce = cos(t), su = sin(t);
            if (su > 0.9995 || fabs(ce) > 0.9995) continue;               /* the crown's and the jambs' planes are there */
            /* the tangent at angle t: n = (cos t) e + (sin t) up, through the arc's point */
            const double px = ax + e[0] * (cs + R * ce), pz = az + e[1] * (cs + R * ce), py = cy + R * su;
            plane_at(s, e[0] * ce, su, e[1] * ce, px, py, pz, HF_REVEAL);
        }
    }
    s->o[0] = ax - m[0] * depth; s->o[1] = y0; s->o[2] = az - m[1] * depth;
    s->t[0] = e[0]; s->t[1] = 0.0; s->t[2] = e[1];
    solid_lift(s, C->dy ? C->dy[unit] : 0.0);
    return (int)(s - C->s);
}

static uint32_t opening_aux(double w, double h, int pattern) {
    const uint32_t cw = (uint32_t)fmin(1023.0, w * 100.0 + 0.5), ch = (uint32_t)fmin(1023.0, h * 100.0 + 0.5);
    return cw << HOUSE_AUX_W | ch << HOUSE_AUX_H | (uint32_t)(pattern & 15) << HOUSE_AUX_PATTERN;
}

/* Whether a box (corners lo, hi) meets any solid (other than the volume's own; a window's ignores the balconies, HM5e2,
 * whose rules keep them off the windows they don't open from) */
static int box_blocked_by(const Ctx *C, int vi, int unit, const double *lo, const double *hi, int balc) {
    for (int i = 0; i < C->ns; ++i) {
        const Solid *s = &C->s[i];
        if (s->neg || !s->live || (s->vol == vi && s->unit == unit && s->kind != SK_OTHER) || (!balc && s->kind == SK_BALC)) continue;
        if (!boxes_meet(lo, hi, s->mn, s->mx, 0.0)) continue;
        int sep = 0;
        for (int j = 0; j < s->np && !sep; ++j) {
            sep = 1;
            for (int c = 0; c < 8 && sep; ++c) {
                const double p[3] = { c & 1 ? hi[0] : lo[0], c & 2 ? hi[1] : lo[1], c & 4 ? hi[2] : lo[2] };
                sep = dot3(s->pl[j].n, p) >= s->pl[j].d;
            }
        }
        if (!sep) return 1;
    }
    return 0;
}
static int box_blocked(const Ctx *C, int vi, int unit, const double *lo, const double *hi) { return box_blocked_by(C, vi, unit, lo, hi, 0); }

/* The windows of volume vi's body (a row's main's slice for unit): each wall that faces out, each storey, a
 * window in each bay of about 2.7 m (section 13.7) where nothing stands in front of it and no door is. */
/* Whether a ground-storey window centred at (x, z) on its wall stands where its unit's ground floor is carved out (a five-
 * foot way, pilotis, a reveal) or its shop front is (HM5e1). */
static int ground_taken(const HGraph *g, int unit, double x, double z, double hw, int front) {
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *p = &g->vol[i];
        if (p->role != HV_PORCH || p->unit != unit || !home_porch_recessed(p->kind)) continue;
        const double pad = p->kind == HP_PILOTIS ? PILOTIS_COL + 0.5 : 0.3;
        if (x + hw > p->x0 - pad && x - hw < p->x1 + pad && z > p->z0 - 0.6 && z < p->z1 + (p->kind == HP_PILOTIS ? 0.2 : -0.05)) return 1;
    }
    const HUnit *u = &g->unit[unit];
    if (front && u->shopX1 > u->shopX0 && z < g->vol[0].z0 + 2.5 && x + hw > u->shopX0 - 0.3 && x - hw < u->shopX1 + 0.3) return 1;
    return 0;
}

/* An RCC window's chajja (HM5e1, the South Asian sheet): a thin concrete sunshade slab over its head, 0.5 m out. */
static void add_chajja(Ctx *C, int vi, int unit, const double m[2], const double e[2], double at, double a0, double w, double head) {
    const double dy = C->dy ? C->dy[unit] : 0.0, out = 0.5, y0 = head + 0.1, y1 = head + 0.2;
    const double s0 = a0 - 0.18, s1 = a0 + w + 0.18;
    double lo[3] = { 1e30, y0 + dy, 1e30 }, hi[3] = { -1e30, y1 + dy, -1e30 };
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            const double ss = a ? s1 : s0, o = b ? out : 0.02;
            const double x = m[0] * (at + o) + e[0] * ss, z = m[1] * (at + o) + e[1] * ss;
            lo[0] = fmin(lo[0], x); hi[0] = fmax(hi[0], x); lo[2] = fmin(lo[2], z); hi[2] = fmax(hi[2], z);
        }
    if (box_blocked(C, vi, unit, lo, hi)) return;
    Solid *s = solid_new(C, unit, vi, 0);
    plane_at(s, m[0], 0, m[1], m[0] * (at + out), 0, m[1] * (at + out), HF_WALL);
    s->noWin[s->np - 1] = 1;
    plane_at(s, -m[0], 0, -m[1], m[0] * (at - 0.03), 0, m[1] * (at - 0.03), NOFACE);
    plane_at(s, e[0], 0, e[1], e[0] * s1, 0, e[1] * s1, HF_WALL);
    s->noWin[s->np - 1] = 1;
    plane_at(s, -e[0], 0, -e[1], e[0] * s0, 0, e[1] * s0, HF_WALL);
    s->noWin[s->np - 1] = 1;
    plane_at(s, 0, 1, 0, 0, y1, 0, HF_COPING);
    plane_at(s, 0, -1, 0, 0, y0, 0, HF_WALL);
    solid_lift(s, dy);
}

/* ---------------------------------------------------------------------------
 * Balconies (HM5e2; the graph's HBalcony, placed by home_balcony.c)
 * ------------------------------------------------------------------------- */

/* The balcony of a unit's storey whose span holds x (along its main's front), or -1. */
static int balc_at(const HGraph *g, int unit, int storey, double x) {
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        if (b->unit == unit && b->storey == storey && x > b->x0 && x < b->x1) return i;
    }
    return -1;
}

/* A balcony slab's top: stone in Britain and Southern Europe, tiles in the Middle East, Africa and the link house's,
 * else the concrete's own. */
static int balc_step(int style) {
    switch (style) {
    case HS_GEORGIAN: case HS_TOWNHOUSE: case HS_SEU_VILLA: return HST_STONE;
    case HS_MENA_VILLA: case HS_WALLED: case HS_LINKHOUSE: return HST_TILE;
    default: return HST_CONCRETE;
    }
}

/* A box standing out of the front wall (its face at z, facing -z): x0..x1, from zf (its front) back into the wall to
 * zb, y0..y1 over the unit's floor; its front and ends `edge`, its top and underside. */
static Solid *balc_box(Ctx *C, int unit, int vi, double x0, double x1, double zf, double zb, double y0, double y1, int edge, int top, int bot) {
    Solid *s = solid_new(C, unit, vi, 0);
    s->kind = SK_BALC;
    plane_at(s, -1, 0, 0, x0, 0, 0, edge); s->noWin[s->np - 1] = 1;
    plane_at(s, 1, 0, 0, x1, 0, 0, edge);  s->noWin[s->np - 1] = 1;
    plane_at(s, 0, 0, -1, 0, 0, zf, edge); s->noWin[s->np - 1] = 1;
    plane_at(s, 0, 0, 1, 0, 0, zb, NOFACE);
    plane_at(s, 0, 1, 0, 0, y1, 0, top);
    plane_at(s, 0, -1, 0, 0, y0, 0, bot);
    solid_lift(s, C->dy ? C->dy[unit] : 0.0);
    return s;
}

/* What of the balconies stands out of the walls, at both levels: slabs (a balconette's ledge), a glazed one's upstand and
 * the slab over it where no balcony over it serves. Carved ones are the near shell's (add_balcony_carves). */
static void add_balcony_solids(Ctx *C) {
    const HGraph *g = C->g;
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        if (home_balcony_carved(b->kind) || b->unit < 0 || b->unit >= g->nunit) continue;
        const int u = b->unit, vi = b->vol;
        const double y = home_balcony_floor(g, b), zf = b->z - b->depth, zb = b->z + 0.03;
        if (b->kind == HBK_BALCONETTE) {
            Solid *s = balc_box(C, u, vi, b->x0, b->x1, zf, zb, y - 0.12, y, HF_COPING, HF_STEP, HF_COPING);
            s->stepFin = HST_STONE;
            continue;
        }
        Solid *s = balc_box(C, u, vi, b->x0, b->x1, zf, zb, y - BALC_SLAB, y, HF_COPING, HF_STEP, HF_SOFFIT);
        s->stepFin = (uint8_t)balc_step(g->style);
        if (b->kind != HBK_GLAZED) continue;
        /* the glazed one's upstand (its front and ends, the unit's wall), the glass over it the kit's */
        const double t = 0.12, h = 0.95;
        balc_box(C, u, vi, b->x0, b->x1, zf, zf + t, y, y + h, HF_WALL, HF_COPING, NOFACE);
        balc_box(C, u, vi, b->x0, b->x0 + t, zf + t - 0.01, zb, y, y + h, HF_WALL, HF_COPING, NOFACE);
        balc_box(C, u, vi, b->x1 - t, b->x1, zf + t - 0.01, zb, y, y + h, HF_WALL, HF_COPING, NOFACE);
        /* the slab over it: the next storey's balcony where one covers it, else its own under the next floor or the eaves */
        int over = 0;
        for (int j = 0; j < g->nbalc && !over; ++j) {
            const HBalcony *o = &g->balc[j];
            over = o->unit == b->unit && o->storey == b->storey + 1 && !home_balcony_carved(o->kind) &&
                   o->x0 <= b->x0 + 0.01 && o->x1 >= b->x1 - 0.01 && o->depth >= b->depth - 0.01;
        }
        if (over) continue;
        HVol own;
        const HVol *m = home_unit_vol(g, vi, u, &own);
        const double top = fmin(y + BALC_DROP + g->storeyH - BALC_DROP, m->eaves - 0.02);
        balc_box(C, u, vi, b->x0, b->x1, zf, zb, top - BALC_SLAB, top, HF_COPING, HF_COPING, HF_SOFFIT);
    }
}

/* ---------------------------------------------------------------------------
 * Boundaries and gates (HM5e3; the graph's HBound, HGate and kiosk, set by home_bound.c)
 * ------------------------------------------------------------------------- */

static Solid *bound_box(Ctx *C, int unit, double dy, double x0, double y0, double z0, double x1, double y1, double z1, int side, int top,
                        int fin, int col) {
    Solid *s = solid_new(C, unit, -1, 0);
    s->wallFin = (uint8_t)fin; s->wallCol = (uint8_t)col;
    box_planes(s, x0, y0, z0, x1, y1, z1, side, top, NOFACE);
    for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
    solid_lift(s, dy);
    return s;
}

/* A gabled roof as one solid: its ridge along x (alongX) or z, over a0..a1 along it and c0..c1 across, its eaves at y0
 * rising `rise` to the ridge, `thick` deep at its eaves (its soffit flat under it). Its slopes the house's roof covering,
 * its gable ends `ends`. China's tile cap on a wall, its gatehouse's roof. */
static void roof_prism(Ctx *C, int unit, double dy, int alongX, double a0, double a1, double c0, double c1, double y0, double rise,
                       double thick, int ends) {
    Solid *s = solid_new(C, unit, -1, 0);
    if (C->g->style == HS_CHINA) { s->roofFin = HRM_GLAZED; s->roofCol = 0; }   /* the sheet's dark grey tile, whatever the house's roof */
    const double half = 0.5 * (c1 - c0);
    if (alongX) {
        plane_at(s, 0, half, rise, 0, y0, c1, HF_ROOF);
        plane_at(s, 0, half, -rise, 0, y0, c0, HF_ROOF);
        plane_at(s, -1, 0, 0, a0, 0, 0, ends);
        plane_at(s, 1, 0, 0, a1, 0, 0, ends);
        plane_at(s, 0, 0, 1, 0, 0, c1, HF_FASCIA);
        plane_at(s, 0, 0, -1, 0, 0, c0, HF_FASCIA);
    } else {
        plane_at(s, rise, half, 0, c1, y0, 0, HF_ROOF);
        plane_at(s, -rise, half, 0, c0, y0, 0, HF_ROOF);
        plane_at(s, 0, 0, -1, 0, 0, a0, ends);
        plane_at(s, 0, 0, 1, 0, 0, a1, ends);
        plane_at(s, 1, 0, 0, c1, 0, 0, HF_FASCIA);
        plane_at(s, -1, 0, 0, c0, 0, 0, HF_FASCIA);
    }
    plane_at(s, 0, -1, 0, 0, y0 - thick, 0, HF_SOFFIT);
    for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
    solid_lift(s, dy);
}

/* The gate kiosk (the walled compound's): a small rendered room beside the gate, its front in the wall's line with a
 * window to the street, its door to the yard, under a hipped roof of the house's covering. */
static void add_kiosk(Ctx *C, const HSite *k) {
    const HGraph *g = C->g;
    const int u = k->unit;
    const double dy = C->dy ? C->dy[u] : 0.0;
    double wallTop = 0.0;
    for (int i = 0; i < g->nbound; ++i)
        if (g->bound[i].unit == u && home_bound_drawn(g, &g->bound[i])) wallTop = fmax(wallTop, g->bound[i].h + g->bound[i].th);
    const double Ye = fmax(2.5, wallTop + 0.25), ov = 0.35, tp = tan(25.0 * PI_D / 180.0);
    const int host = (int)C->ns;
    Solid *b = solid_new(C, u, -1, 0);
    box_planes(b, k->x0, 0.0, k->z0, k->x1, Ye, k->z1, HF_WALL, NOFACE, NOFACE);
    for (int i = 0; i < b->np; ++i) b->noWin[i] = 1;
    solid_lift(b, dy);
    /* the roof: four slopes through the wall plates, the overhang's fascias, a flat soffit */
    Solid *r = solid_new(C, u, -1, 0);
    const double ct = cos(atan(tp)), st = sin(atan(tp));
    plane_at(r, 0, ct, -st, 0, Ye, k->z0, HF_ROOF);
    plane_at(r, 0, ct, st, 0, Ye, k->z1, HF_ROOF);
    plane_at(r, -st, ct, 0, k->x0, Ye, 0, HF_ROOF);
    plane_at(r, st, ct, 0, k->x1, Ye, 0, HF_ROOF);
    plane_at(r, 0, 0, -1, 0, 0, k->z0 - ov, HF_FASCIA);
    plane_at(r, 0, 0, 1, 0, 0, k->z1 + ov, HF_FASCIA);
    plane_at(r, -1, 0, 0, k->x0 - ov, 0, 0, HF_FASCIA);
    plane_at(r, 1, 0, 0, k->x1 + ov, 0, 0, HF_FASCIA);
    plane_at(r, 0, -1, 0, 0, Ye - ov * tp - 0.12, 0, HF_SOFFIT);
    for (int i = 0; i < r->np; ++i) r->noWin[i] = 1;
    solid_lift(r, dy);
    if (C->lod != HSH_NEAR) return;
    const double ms[2] = { 0.0, -1.0 }, es[2] = { 1.0, 0.0 }, mb[2] = { 0.0, 1.0 }, eb[2] = { -1.0, 0.0 };
    const double kw = k->x1 - k->x0, ww = fmin(0.9, kw - 0.8);
    if (ww > 0.4) add_opening(C, host, u, ms, es, k->x0 + 0.5 * (kw - ww), k->z0, ww, 1.15, 1.95, 0.12, HF_GLASS, opening_aux(ww, 0.8, C->fin[u].glazing));
    add_opening(C, host, u, mb, eb, k->x1 - 0.3, k->z1, 0.85, 0.0, 2.05, 0.1, HF_DOOR, opening_aux(0.85, 2.05, 1));
}

/* China's gatehouse over its gate: deep brick piers, a lintel wall over the red plank doors, a tile roof along the wall. */
static void add_gatehouse(Ctx *C, const HGate *gt, double t, int fin, int col) {
    const int u = gt->unit;
    const double dy = C->dy ? C->dy[u] : 0.0, gx0 = gt->x - 0.5 * gt->w, gx1 = gt->x + 0.5 * gt->w, pw = 0.5;
    const double hp = gt->h + 0.55, zf = -0.12, zb = t + 0.3;
    bound_box(C, u, dy, gx0 - pw, 0.0, zf, gx0, hp, zb, HF_WALL, NOFACE, fin, col);
    bound_box(C, u, dy, gx1, 0.0, zf, gx1 + pw, hp, zb, HF_WALL, NOFACE, fin, col);
    bound_box(C, u, dy, gx0 - 0.01, gt->h, zf + 0.06, gx1 + 0.01, hp, zb - 0.06, HF_WALL, NOFACE, fin, col);
    roof_prism(C, u, dy, 1, gx0 - pw - 0.4, gx1 + pw + 0.4, zf - 0.5, zb + 0.5, hp - 0.02, 0.62, 0.16, HF_WALL);
    /* the doors: a pair of plank leaves (house.glsl's door pattern 4) in the wall's middle, both faces */
    Solid *s = solid_new(C, u, -1, 0);
    const double zc = 0.5 * t;
    box_planes(s, gx0, 0.0, zc - 0.04, gx1, gt->h, zc + 0.04, NOFACE, NOFACE, NOFACE);
    for (int i = 0; i < s->np; ++i) if (fabs(s->pl[i].n[2]) > 0.5) s->surf[i] = HF_DOOR;
    s->aux = opening_aux(gt->w, gt->h, 4);
    s->o[0] = gx0; s->o[1] = 0.0; s->o[2] = zc - 0.04;
    s->t[0] = 1.0; s->t[1] = 0.0; s->t[2] = 0.0;
    solid_lift(s, dy);
}

static void add_bounds(Ctx *C) {
    const HGraph *g = C->g;
    const HBoundSet *S = &g->bset;
    if (!S->drawn) return;
    const int far = C->lod == HSH_FAR;
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        if (!home_bound_drawn(g, b)) continue;
        const int u = b->unit >= 0 && b->unit < g->nunit ? b->unit : 0;
        const double dy = C->dy ? C->dy[u] : 0.0;
        const double total = b->h + b->th;
        const int band = b->top == HBT_BREEZE || b->top == HBT_RINGS;
        /* far off, only a wall tall enough to hide a garden (a compound's, China's): its solid part, a breeze band as solid */
        if (far && b->h + (band ? b->th : 0.0f) < 1.5f) continue;
        float x0, z0, x1, z1;
        home_bound_rect(g, b, &x0, &z0, &x1, &z1);
        /* a railing's kerb is stone; the rest the row's finish */
        const int fin = b->kind == HB_RAILING ? HW_STONE : S->fin, col = b->kind == HB_RAILING && S->fin != HW_STONE ? 0 : S->col;
        const int capped = b->kind == HB_WALL && (b->top == HBT_NONE || band || b->top == HBT_IRON || b->top == HBT_SPEAR);
        if (far) { bound_box(C, u, dy, x0, 0.0, z0, x1, band ? total : b->h, z1, HF_WALL, HF_COPING, fin, col); continue; }
        bound_box(C, u, dy, x0, 0.0, z0, x1, b->h, z1, HF_WALL, HF_COPING, fin, col);
        if (!capped) continue;
        const double y = band ? total : b->h;
        const int alongX = fabsf(b->z1 - b->z0) < 0.01f;
        if (S->coping == HBC_TILE) {
            if (alongX) roof_prism(C, u, dy, 1, x0, x1, z0 - 0.12, z1 + 0.12, y, 0.16, 0.05, HF_COPING);
            else roof_prism(C, u, dy, 0, z0, z1, x0 - 0.12, x1 + 0.12, y, 0.16, 0.05, HF_COPING);
        } else {
            const double c = BOUND_COPE;
            if (alongX) bound_box(C, u, dy, x0, y, z0 - c, x1, y + 0.07, z1 + c, HF_COPING, HF_COPING, fin, col);
            else bound_box(C, u, dy, x0 - c, y, z0, x1 + c, y + 0.07, z1, HF_COPING, HF_COPING, fin, col);
        }
    }
    /* the piers: a party line's stands on the lower of its two units' grounds, reaching up to the higher's */
    HPier P[160];
    const int np = home_piers(g, P, 160);
    const double pw = S->pierW;
    for (int k = 0; k < np; ++k) {
        int u = P[k].unit >= 0 && P[k].unit < g->nunit ? P[k].unit : 0;
        double lo = C->dy ? C->dy[u] : 0.0, hi = lo;
        for (int n = 0; n < g->nunit && C->dy; ++n)
            if (P[k].x > g->unit[n].x0 - 0.5 * pw && P[k].x < g->unit[n].x1 + 0.5 * pw) {
                if (C->dy[n] < lo) { lo = C->dy[n]; u = n; }
                hi = fmax(hi, C->dy[n]);
            }
        const double top = P[k].top + (hi - lo);
        if (far) break;   /* past the near layer's 220 m a pier doesn't read: the far shell keeps the walls alone */
        bound_box(C, u, lo, P[k].x - 0.5 * pw, 0.0, P[k].z - 0.5 * pw, P[k].x + 0.5 * pw, top, P[k].z + 0.5 * pw, HF_WALL, HF_COPING, S->fin, S->col);
        if (!far) {
            const double c = 0.04;
            bound_box(C, u, lo, P[k].x - 0.5 * pw - c, top, P[k].z - 0.5 * pw - c, P[k].x + 0.5 * pw + c, top + 0.08, P[k].z + 0.5 * pw + c,
                      HF_COPING, HF_COPING, S->fin, S->col);
        }
    }
    /* China's gatehouses, the compound's kiosk */
    for (int i = 0; i < g->ngate; ++i) {
        const HGate *gt = &g->gate[i];
        if (gt->house && gt->unit >= 0 && gt->unit < g->nunit) add_gatehouse(C, gt, BOUND_T, S->fin, S->col);
    }
    for (int i = 0; i < g->nsite; ++i)
        if (g->site[i].kind == HE_KIOSK && g->site[i].unit >= 0 && g->site[i].unit < g->nunit) add_kiosk(C, &g->site[i]);
}

/* A French window out onto balcony bi: centred at c in the front wall (its face at zface, facing the street), its sill a
 * threshold over the storey's floor. */
static void add_french(Ctx *C, int host, int vi, int unit, int storey, int bi, double c, double w, double floorY, double head,
                       double zface, double s0, double s1) {
    const double m[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 }, sill = floorY + 0.03, depth = 0.1;
    const double a0 = c - 0.5 * w, dy = C->dy ? C->dy[unit] : 0.0;
    if (head - sill < 1.8 || w < 0.6) return;
    uint32_t aux = opening_aux(w, head - sill, HG_CASEMENT);
    if (C->kit) aux |= HOUSE_AUX_FRAMED;
    const int opened = add_opening(C, host, unit, m, e, a0, zface, w, sill, head, depth, HF_GLASS, aux);
    if (opened < 0) return;
    C->st.windows++;
    if (!C->kit) return;
    GROW(C->win, C->nwin, C->capwin);
    HsWin *hw = &C->win[C->nwin++];
    memset(hw, 0, sizeof(*hw));
    hw->unit = (int8_t)unit; hw->vol = (int8_t)vi; hw->storey = (uint8_t)storey; hw->bay = 0; hw->balc = (int8_t)bi; hw->host = host;
    hw->m[0] = m[0]; hw->m[1] = m[1]; hw->e[0] = e[0]; hw->e[1] = e[1]; hw->at = -zface;
    hw->a0 = a0; hw->w = w; hw->s0 = s0; hw->s1 = s1;
    hw->y0 = sill + dy; hw->y1 = head + dy; hw->depth = depth; hw->floorY = floorY + dy;
}

/* The French windows at the back of a carved balcony: one in each bay of the front's rhythm whose middle falls in an
 * opening a0..a1 (else one in its middle). */
static void french_at_back(Ctx *C, int host, const HBalcony *b, int bi, double a0, double a1, double ceiling) {
    const HGraph *g = C->g;
    float s0, bw;
    const int nb = home_front_bays(g, b->vol, b->unit, &s0, &bw);
    if (nb <= 0) return;
    const double floorY = home_balcony_floor(g, b) + BALC_DROP, head = fmin(floorY + 2.2, ceiling - 0.15);
    const double w = fmin(home_french_width(C->fin[b->unit].winW, bw), a1 - a0 - 0.6);
    int n = 0;
    for (int k = 0; k < nb; ++k) {
        const double c = s0 + (k + 0.5) * bw;
        if (c - 0.5 * w < a0 + 0.3 || c + 0.5 * w > a1 - 0.3) continue;
        add_french(C, host, b->vol, b->unit, b->storey, bi, c, w, floorY, head, b->z + b->depth, a0, a1);
        n++;
    }
    if (!n) add_french(C, host, b->vol, b->unit, b->storey, bi, 0.5 * (a0 + a1), w, floorY, head, b->z + b->depth, a0, a1);
}

static int add_carve(Ctx *C, int host, int unit, double x0, double z0, double x1, double z1, double y0, double y1, int fin);

/* The carved balconies (near): a loggia cut into the front under a flat or arched head, a veranda's openings between its
 * piers (open behind the piers), and the French windows at their backs. */
static void add_balcony_carves(Ctx *C) {
    const HGraph *g = C->g;
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        if (!home_balcony_carved(b->kind) || b->unit < 0 || b->unit >= g->nunit) continue;
        const int u = b->unit, host = C->body[b->vol][u];
        if (host < 0) continue;
        HVol own;
        const HVol *m = home_unit_vol(g, b->vol, u, &own);
        const double y = home_balcony_floor(g, b), floorY = y + BALC_DROP;
        const int fin = balc_step(g->style);
        if (b->kind == HBK_LOGGIA) {
            const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
            const double head = fmin(floorY + 2.6, m->eaves - 0.3), rise = 0.01 * b->arch;
            const int r = add_opening_arch(C, host, u, mm, e, b->x0, b->z, b->x1 - b->x0, y, head - rise, b->depth, HF_WALL, 0, rise);
            if (r < 0) continue;
            Solid *s = &C->s[r];
            for (int k = 0; k < s->np; ++k) {
                if (s->pl[k].n[1] > 0.5 && rise <= 0.0) s->surf[k] = HF_CEILING;
                else if (s->pl[k].n[1] < -0.5) s->surf[k] = HF_STEP;
                else if (s->surf[k] == HF_REVEAL) s->surf[k] = HF_WALL;
            }
            s->stepFin = (uint8_t)fin;
            french_at_back(C, host, b, i, b->x0, b->x1, head - rise);
        } else {
            float w;
            const int n = home_veranda_bays(b, &w);
            const double top = fmin(floorY + g->storeyH - 0.3, m->eaves - 0.3);
            for (int k = 0; k < n; ++k) {
                const double a = b->x0 + BALC_PIER + k * (w + BALC_PIER);
                add_carve(C, host, u, a, b->z - 0.3, a + w, b->z + b->depth, y, top, fin);
                if (k + 1 < n) add_carve(C, host, u, a + w - 0.01, b->z + BALC_PIER, a + w + BALC_PIER + 0.01, b->z + b->depth, y, top, fin);
                french_at_back(C, host, b, i, a, a + w, top);   /* each opening its French window */
            }
        }
    }
}

static void add_windows(Ctx *C, int vi, int unit) {
    const HGraph *g = C->g;
    HVol own;
    const HVol *v = home_unit_vol(g, vi, unit, &own);
    const int host = C->body[vi][unit];
    if (host < 0 || !habitable(v) || (v->role == HV_WING && v->kind == HWING_GARAGE)) return;
    const Solid Hc = C->s[host], *H = &Hc;   /* a copy: adding openings may move the array */
    const HFinish *fin = &C->fin[unit];
    const double dy = C->dy ? C->dy[unit] : 0.0;
    for (int i = 0; i < H->np; ++i) {
        if (H->surf[i] != HF_WALL || H->noWin[i] || fabs(H->pl[i].n[1]) > 0.01) continue;
        /* the wall face at ground level: its run along the wall */
        SPoly f, tmp;
        if (!solid_face(H, i, &f, &tmp)) continue;
        const double m[2] = { H->pl[i].n[0], H->pl[i].n[2] }, e[2] = { -m[1], m[0] };
        double s0 = 1e30, s1 = -1e30, v0 = 1e30, v1 = -1e30;
        for (int q = 0; q < f.n; ++q) {
            const double s = f.p[q][0] * e[0] + f.p[q][2] * e[1];
            s0 = fmin(s0, s); s1 = fmax(s1, s);
        }
        /* within the volume's own outline (a body run on into its parent is longer than its walls) */
        float ol[6][2];
        const int no = hvol_outline(v, ol);
        for (int q = 0; q < no; ++q) {
            const double s = ol[q][0] * e[0] + ol[q][1] * e[1];
            v0 = fmin(v0, s); v1 = fmax(v1, s);
        }
        s0 = fmax(s0, v0); s1 = fmin(s1, v1);
        const double L = s1 - s0, at = H->pl[i].d;   /* the wall: m.(x, z) = at */
        const int bay = v->role == HV_BAY;
        if (L < (bay ? 0.5 : 1.4)) continue;
        /* a side wall (square to the street) has fewer: wider bays, and some left blank */
        const int side = !bay && fabs(m[0]) > 0.7;
        const int nb = bay ? 1 : (int)fmax(1.0, floor(L / (side ? 4.0 : 2.7) + 0.5));
        const double bw = L / nb;
        /* the courtyard house's street walls (HM5e1): blank but for a few small windows, the ground floor's set high */
        const int blank = g->style == HS_COURTYARD && !bay;
        for (int k = 0; k < v->storeys; ++k) {
            const double floorY = g->plinth + k * g->storeyH;
            double sill = floorY + (k == 0 ? 0.75 : 0.85), head = fmin(floorY + 2.15, v->eaves - 0.25);
            /* a bay's lights reach its corner posts when the kit dresses it (hk_bay's: the face less 0.13 m) */
            double ww0 = bay ? fmax(0.3, L - (C->kit ? 0.13 : 0.3)) : fmin(fin->winW, bw - 0.6);
            /* a Georgian front's attic storey: short windows (its top storey of three or four) */
            if (!bay && k == g->vol[0].storeys - 1 && g->style == HS_GEORGIAN && v->storeys >= 3) head = fmin(head, sill + 1.0);
            if (blank && k == 0) { sill = floorY + 1.6; head = fmin(head, sill + 0.7); }
            if (head - sill < (blank ? 0.5 : 0.8) || ww0 < 0.3) continue;
            if (head - sill > fin->winH && !bay) sill = head - fin->winH;
            const double sill0 = sill, head0 = head;
            for (int b = 0; b < nb; ++b) {
                const double c = s0 + (b + 0.5) * bw;
                double ww = ww0, sill = sill0, head = head0;
                /* a balcony in front of it (HM5e2): a carved one's windows are at its back; else a French window */
                const int fb = k > 0 && v->role <= HV_ROW && m[1] < -0.7 ? balc_at(g, unit, k, c) : -1;
                if (fb >= 0 && home_balcony_carved(g->balc[fb].kind)) continue;
                if (fb >= 0) {
                    ww = home_french_width((float)ww0, (float)bw);
                    sill = floorY + 0.03;
                    head = fmin(floorY + 2.2, v->eaves - 0.2);
                }
                const double a0 = c - 0.5 * ww;
                if (blank && hash_float(hash3i(g->unit[unit].seed ^ 0xB1A4u, vi * 8 + i, k, b)) < (k == 0 ? 0.6f : 0.35f)) continue;
                /* the ground floor carved out under it, or a shop front there (HM5e1) */
                if (k == 0 && ground_taken(g, unit, m[0] * at + e[0] * c, m[1] * at + e[1] * c, 0.5 * ww, m[1] < -0.7)) continue;
                /* no window over a door */
                int clash = 0;
                for (int d = 0; d < g->ndoor && !clash; ++d) {
                    const HDoor *dr = &g->door[d];
                    if (k || dr->unit != unit) continue;
                    const double ds = dr->x * e[0] + dr->z * e[1], dm = dr->x * m[0] + dr->z * m[1];
                    clash = fabs(dm - at) < 1.6 && fabs(ds - c) < 0.5 * (ww + dr->width) + 0.3;
                }
                if (clash) continue;
                if (side && hash_float(hash3i(g->unit[unit].seed ^ 0x51DEu, vi * 8 + i, k, b)) < 0.4f) continue;
                /* nothing standing in front of it: a box from the wall out 0.35 m round the window */
                double lo[3], hi[3];
                const double ax = m[0] * at + e[0] * a0, az = m[1] * at + e[1] * a0;
                for (int q = 0; q < 2; ++q) {
                    const double mg = bay && C->kit ? -0.02 : 0.12;
                    const double s = q ? ww + mg : -mg;
                    const double px = ax + e[0] * s, pz = az + e[1] * s;
                    for (int r = 0; r < 2; ++r) {
                        const double o = r ? 0.35 : 0.01, x = px + m[0] * o, z = pz + m[1] * o;
                        if (!q && !r) { lo[0] = hi[0] = x; lo[2] = hi[2] = z; }
                        lo[0] = fmin(lo[0], x); hi[0] = fmax(hi[0], x); lo[2] = fmin(lo[2], z); hi[2] = fmax(hi[2], z);
                    }
                }
                lo[1] = sill - 0.12 + dy; hi[1] = head + 0.12 + dy;
                if (box_blocked(C, vi, unit, lo, hi)) continue;
                double depth = g->style == HS_GEORGIAN || g->style == HS_VICTORIAN || g->style == HS_ROWHOUSE ? 0.14 : 0.1;
                uint32_t aux = opening_aux(ww, head - sill, fin->glazing);
                if (C->kit) {
                    /* the kit's frame stands in it: the shader paints the glazing bars only past the kit's reach,
                     * and they are the frame's (a replacement's plain glass) */
                    if (bay) depth = 0.08;   /* a bay's thin walls (hk_bay's frames) */
                    const int fr = fb >= 0 ? HK_FRAME_CASEMENT : home_char_frame(&C->cs[unit], m[1] < -0.7, k, ww);
                    static const uint8_t PAT[] = { HG_PLAIN, HG_SASH, HG_GEORGIAN, HG_CASEMENT, HG_GRID, HG_PLAIN, HG_PLAIN };
                    aux = opening_aux(ww, head - sill, PAT[fr <= HK_FRAME_UPVC ? fr : 0]) | HOUSE_AUX_FRAMED;
                } else if (fb >= 0) aux = opening_aux(ww, head - sill, HG_CASEMENT);
                const int opened = add_opening(C, host, unit, m, e, ax, az, ww, sill, head, depth, HF_GLASS, aux);
                /* an RCC window's chajja, but where a balcony over it shades it (HM5e2) */
                if (opened >= 0 && g->style == HS_RCC && !bay && (m[1] > -0.7 || balc_at(g, unit, k + 1, c) < 0))
                    add_chajja(C, vi, unit, m, e, at, a0, ww, head);
                if (opened >= 0 && C->kit) {
                    GROW(C->win, C->nwin, C->capwin);
                    HsWin *w = &C->win[C->nwin++];
                    w->unit = (int8_t)unit; w->vol = (int8_t)vi; w->storey = (uint8_t)k; w->bay = (uint8_t)bay; w->host = host;
                    w->balc = (int8_t)fb;
                    w->m[0] = m[0]; w->m[1] = m[1]; w->e[0] = e[0]; w->e[1] = e[1]; w->at = at;
                    w->a0 = a0; w->w = ww; w->s0 = s0; w->s1 = s1;
                    w->y0 = sill + dy; w->y1 = head + dy; w->depth = depth; w->floorY = floorY + dy;
                }
                C->st.windows++;
            }
        }
    }
}

/* A box of steps' or a deck's: its top and sides HF_STEP, standing on the garden. */
static void add_step_box(Ctx *C, int unit, int vi, double x0, double z0, double x1, double z1, double y1, int fin) {
    if (x1 - x0 < 0.05 || z1 - z0 < 0.05 || y1 < 0.02) return;
    Solid *s = solid_new(C, unit, vi, 0);
    s->stepFin = (uint8_t)fin;
    box_planes(s, x0, 0.0, z0, x1, y1, z1, HF_STEP, HF_STEP, NOFACE);
    solid_lift(s, C->dy ? C->dy[unit] : 0.0);
}

static void add_post(Ctx *C, int unit, int vi, double x, double z, double y0, double y1) {
    Solid *s = solid_new(C, unit, vi, 0);
    box_planes(s, x - 0.07, y0, z - 0.07, x + 0.07, y1, z + 0.07, HF_TRIM, HF_TRIM, NOFACE);
    solid_lift(s, C->dy ? C->dy[unit] : 0.0);
}

/* Steps down from zf (the front of a stoop or a porch's floor) towards the street, x0..x1 wide, from the floor P. */
static void add_steps(Ctx *C, int unit, int vi, double x0, double x1, double zf, double P, int fin) {
    if (P <= 0.22) return;
    const int nr = (int)ceil(P / 0.18);
    const double h = P / nr;
    double tread = 0.28;
    if (nr > 1 && zf - 0.1 < tread * (nr - 1)) tread = fmax(0.16, (zf - 0.1) / (nr - 1));
    for (int i = 1; i < nr; ++i) {
        const double za = zf - i * tread, zb = zf - (i - 1) * tread;
        if (zb < 0.05) break;
        add_step_box(C, unit, vi, x0, fmax(0.02, za), x1, zb + 0.05, P - i * h, fin);
    }
}

/* A box carved out of a body (HM5e1: pilotis' and a five-foot way's spaces): its top a ceiling, its floor a step's
 * finish, its sides walls (the columns' and piers' faces, the core's front). */
static int add_carve(Ctx *C, int host, int unit, double x0, double z0, double x1, double z1, double y0, double y1, int fin) {
    if (host < 0 || x1 - x0 < 0.05 || z1 - z0 < 0.05) return -1;
    Solid *s = solid_new(C, unit, C->s[host].vol, 1);
    s->host = host;
    box_planes(s, x0, y0, z0, x1, y1, z1, HF_WALL, HF_CEILING, HF_STEP);
    s->stepFin = (uint8_t)fin;
    s->o[0] = x0; s->o[1] = y0; s->o[2] = z0;
    s->t[0] = 1.0; s->t[1] = 0.0; s->t[2] = 0.0;
    solid_lift(s, C->dy ? C->dy[unit] : 0.0);
    return (int)(s - C->s);
}

/* Pilotis (HM5e1, the Korean villa): the ground storey of the unit's slice carved open from the front back to the core,
 * columns at its free corners and along a long front; where the slice ends at a party wall that wall comes down as a fin. */
static void add_pilotis(Ctx *C, int u, const HVol *m, const HVol *p, int host, int fin) {
    const double c = PILOTIS_COL, out = 0.3, y1 = p->eaves;
    const int freeW = p->x0 <= m->x0 + 0.05, freeE = p->x1 >= m->x1 - 0.05;
    const double fx0 = freeW ? p->x0 + c : p->x0 + 0.12, fx1 = freeE ? p->x1 - c : p->x1 - 0.12;
    /* the front between its columns: a middle column where the span runs past PILOTIS_SPAN */
    const int nbay = (int)fmax(1.0, ceil((fx1 - fx0) / PILOTIS_SPAN));
    const double bw = (fx1 - fx0 - (nbay - 1) * c) / nbay;
    for (int k = 0; k < nbay; ++k) {
        const double a = fx0 + k * (bw + c);
        add_carve(C, host, u, a, p->z0 - out, a + bw, p->z1, 0.0, y1, fin);
        /* under the middle columns' line, behind them, open too */
        if (k + 1 < nbay) add_carve(C, host, u, a + bw - 0.01, p->z0 + c, a + bw + c + 0.01, p->z1, 0.0, y1, fin);
    }
    /* the free sides open between a column at the front corner and one at the core */
    if (freeW) add_carve(C, host, u, p->x0 - out, p->z0 + c, fx0 + 0.01, p->z1 - c, 0.0, y1, fin);
    if (freeE) add_carve(C, host, u, fx1 - 0.01, p->z0 + c, p->x1 + out, p->z1 - c, 0.0, y1, fin);
}

static void add_front(Ctx *C, int u) {
    const HGraph *g = C->g;
    const HUnit *un = &g->unit[u];
    if (un->door < 0) return;
    const HDoor *d = &g->door[un->door];
    const int mi = d->vol;
    const HVol *m = &g->vol[mi];
    const HFinish *fin = &C->fin[u];
    const double P = g->plinth;
    const int host = C->body[mi][u];
    const int street = home_style_street(g->style);
    /* the porch */
    int pi = -1;
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role == HV_PORCH && g->vol[i].unit == u) pi = i;
    const HVol *p = pi >= 0 ? &g->vol[pi] : NULL;
    const int kind = p ? p->kind : HP_CANOPY;
    double zf = m->z0;   /* where the steps start down */
    const double sw = fmin(1.6, d->width + 0.5);
    /* a front on the street line, or a five-foot way or pilotis: no stoop or steps out over the pavement or the yard;
     * a step up at the door inside its reveal at most (HM5e1) */
    const int stoop = !street && kind != HP_ARCADE && kind != HP_PILOTIS;
    if (p && (kind == HP_OPEN || kind == HP_LEAN || kind == HP_VERANDA)) {
        /* the porch's floor, its posts */
        const int deckFin = fin->step == HST_TIMBER || kind == HP_VERANDA ? HST_TIMBER : fin->step;
        add_step_box(C, u, pi, p->x0, p->z0, p->x1, m->z0, P, deckFin);   /* to the wall: its back face and the wall's meet and go */
        zf = p->z0;
        const double y0 = P > 0.02 ? P : 0.0, y1 = p->eaves;
        const double inset = 0.1;
        const int nposts = kind == HP_VERANDA ? (int)fmax(2.0, ceil((p->x1 - p->x0) / 2.4) + 1.0) : 2;
        for (int k = 0; k < nposts; ++k) {
            const double x = p->x0 + inset + (p->x1 - p->x0 - 2.0 * inset) * k / (nposts - 1);
            if (C->kit) {   /* the kit's posts (turned, a craftsman's column, square) */
                const double dyu = C->dy ? C->dy[u] : 0.0;
                GROW(C->post, C->npost, C->cappost);
                HsPost *hp = &C->post[C->npost++];
                hp->unit = (int8_t)u; hp->vol = (int8_t)pi; hp->x = x; hp->z = p->z0 + inset; hp->y0 = y0 + dyu; hp->y1 = y1 + dyu;
            } else add_post(C, u, pi, x, p->z0 + inset, y0, y1);
        }
    } else if (kind == HP_ENCLOSED && p) {
        zf = p->z0;
        /* its own door, in its front */
        const int ph = C->body[pi][u];
        if (ph >= 0) {
            const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
            const double dx = fmax(p->x0 + 0.15, fmin(p->x1 - 0.15 - d->width, d->x - 0.5 * d->width));
            add_opening(C, ph, u, mm, e, dx, p->z0, d->width, P, P + d->height, 0.06, HF_DOOR,
                        opening_aux(d->width, d->height, 1));
            if (C->kit) {
                const double dyu = C->dy ? C->dy[u] : 0.0;
                GROW(C->door, C->ndoor, C->capdoor);
                HsDoor *hd = &C->door[C->ndoor++];
                memset(hd, 0, sizeof(*hd));
                hd->unit = (int8_t)u; hd->vol = (int8_t)pi; hd->porch = 1;
                hd->x = dx + 0.5 * d->width; hd->z = p->z0; hd->w = d->width; hd->y0 = P + dyu; hd->y1 = P + d->height + dyu; hd->recess = 0.06;
            }
        }
    } else if (P > 0.05 && stoop) {
        /* a stoop at the door */
        const double depth = fmin(1.0, fmax(0.3, m->z0 - 0.4));
        add_step_box(C, u, mi, d->x - 0.5 * sw, m->z0 - depth, d->x + 0.5 * sw, m->z0, P, fin->step);
        zf = m->z0 - depth;
    }
    if (stoop) add_steps(C, u, mi, d->x - 0.5 * sw, d->x + 0.5 * sw, zf, P, fin->step);
    /* a recessed porch: carved out of the front, the door at its back; a street-line front's reveal from the pavement,
     * a five-foot way at the ground floor's level behind its piers (HM5e1) */
    if (p && (kind == HP_RECESSED || kind == HP_ARCADE) && host >= 0) {
        const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
        const double rise = 0.01 * p->arch;
        const double y0 = street && kind == HP_RECESSED ? 0.0 : P;
        const double y1 = rise > 0.0 ? p->eaves - rise : p->eaves;
        const int r = add_opening_arch(C, host, u, mm, e, p->x0, m->z0, p->x1 - p->x0, y0, y1, p->z1 - m->z0, HF_WALL, 0, rise);
        if (r >= 0) {
            Solid *s = &C->s[r];
            /* its ceiling and floor (not reveals); an arch's soffit is its wall's */
            for (int i = 0; i < s->np; ++i) {
                if (s->pl[i].n[1] > 0.5 && rise <= 0.0) s->surf[i] = HF_CEILING;
                else if (s->pl[i].n[1] < -0.5) s->surf[i] = HF_STEP;
                else if (s->surf[i] == HF_REVEAL) s->surf[i] = HF_WALL;
            }
            s->stepFin = street || kind == HP_ARCADE || rise > 0.0 ? fin->step : HST_TILE;
        }
    }
    if (p && kind == HP_PILOTIS && host >= 0) add_pilotis(C, u, m, p, host, fin->step);
    /* the door itself: its leaves a pattern of their own under an arch or in China's red gate (HM5e1) */
    if (host >= 0) {
        const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
        const double rise = 0.01 * d->arch;
        const int plank = d->arch || g->style == HS_CHINA;
        add_opening_arch(C, host, u, mm, e, d->x - 0.5 * d->width, d->z, d->width, P, P + d->height, 0.12, HF_DOOR,
                         opening_aux(d->width, d->height + rise, plank ? 4 : (int)(hash_combine(un->seed, 0xD00Bu) & 3u)), rise);
        C->st.doors++;
        if (C->kit) {
            const double dyu = C->dy ? C->dy[u] : 0.0;
            GROW(C->door, C->ndoor, C->capdoor);
            HsDoor *hd = &C->door[C->ndoor++];
            memset(hd, 0, sizeof(*hd));
            hd->unit = (int8_t)u; hd->vol = (int8_t)mi; hd->porch = 0; hd->arch = (uint8_t)(plank != 0);
            hd->x = d->x; hd->z = d->z; hd->w = d->width; hd->y0 = P + dyu; hd->y1 = P + d->height + dyu; hd->recess = 0.12;
        }
    }
    /* a shop front (HM5e1): a roller-shuttered opening in the ground storey, at the back of a five-foot way or in the front */
    if (un->shopX1 > un->shopX0 && host >= 0) {
        const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
        const double z = p && kind == HP_ARCADE ? p->z1 : m->z0;
        const double top = p && kind == HP_ARCADE ? p->eaves - 0.25 : P + fmin(2.7, g->storeyH - 0.45);
        const double w = un->shopX1 - un->shopX0, h = top - P;
        if (h > 1.9) add_opening(C, host, u, mm, e, un->shopX0, z, w, P, top, 0.22, HF_GARAGE, opening_aux(w, h, 1));
    }
}

static void add_garage_door(Ctx *C, int vi) {
    const HGraph *g = C->g;
    const HVol *w = &g->vol[vi];
    const int u = w->unit < g->nunit ? w->unit : 0;
    const int host = C->body[vi][u];
    if (host < 0) return;
    const double W = w->x1 - w->x0, gw = fmin(W - 0.5, W > 4.8 ? 4.8 : 2.5);
    if (gw < 1.8) return;
    const double mm[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 }, x0 = 0.5 * (w->x0 + w->x1) - 0.5 * gw;
    const double h = fmin(2.1, w->eaves - 0.3);
    add_opening(C, host, u, mm, e, x0, w->z0, gw, 0.0, h, 0.15, HF_GARAGE, opening_aux(gw, h, 0));
}

/* ---------------------------------------------------------------------------
 * The driver
 * ------------------------------------------------------------------------- */

float home_top(const HGraph *g) {
    float top = 0.0f;
    for (int i = 0; i < g->nvol; ++i) top = fmaxf(top, g->vol[i].top);
    for (int i = 0; i < g->nchim; ++i) top = fmaxf(top, g->chim[i].top);
    return top;
}

static void shell_build(const HGraph *g, int lod, const float *dy, HShellFn fn, void *ctx, HShellStats *st, HDress *D) {
    Ctx C;
    memset(&C, 0, sizeof(C));
    C.g = g; C.lod = lod; C.dy = dy; C.fn = fn; C.ctx = ctx;
    C.kit = D != NULL && lod == HSH_NEAR;
    if (g->empty || !g->nvol) { if (st) memset(st, 0, sizeof(*st)); return; }
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) home_finish(g, u, &C.fin[u]);
    if (C.kit)
        for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) home_char_set(g, u, &C.cs[u]);
    for (int i = 0; i < HG_MAX_VOL; ++i)
        for (int u = 0; u < HG_MAX_UNITS; ++u) C.body[i][u] = -1;
    C.maxPieces = 256;
    C.pool = (SPoly *)malloc(sizeof(SPoly) * (size_t)C.maxPieces * 2);
    if (!C.pool) abort();
    C.piece = C.pool;
    C.next = C.pool + C.maxPieces;
    /* the volumes: a pair's or row's main in a slice a unit, the rest whole */
    for (int vi = 0; vi < g->nvol; ++vi) {
        const HVol *v = &g->vol[vi];
        if (v->unit == HG_ROW_WIDE) {
            for (int u = 0; u < g->nunit; ++u) {
                const double a = fmax(v->x0, g->unit[u].x0), b = fmin(v->x1, g->unit[u].x1);
                if (b - a < 0.05) continue;
                add_volume(&C, vi, u, a, b, a > v->x0 + 0.01, b < v->x1 - 0.01);
            }
        } else add_volume(&C, vi, v->unit < g->nunit ? v->unit : 0, 0.0, -1.0, 0, 0);
    }
    for (int i = 0; i < g->nparty; ++i) add_upstand(&C, &g->party[i]);
    for (int i = 0; i < g->nchim; ++i) add_stack(&C, &g->chim[i]);
    add_balcony_solids(&C);
    add_bounds(&C);   /* HM5e3 */
    SPoly f, tmp;
    if (lod == HSH_NEAR) {
        for (int u = 0; u < g->nunit; ++u) add_front(&C, u);
        add_balcony_carves(&C);
        for (int i = 0; i < C.ns; ++i) solid_bounds(&C.s[i], &f, &tmp);
        const int positives = C.ns;
        for (int vi = 0; vi < g->nvol; ++vi) {
            const HVol *v = &g->vol[vi];
            if (v->role == HV_WING && v->kind == HWING_GARAGE) add_garage_door(&C, vi);
            for (int u = 0; u < g->nunit; ++u)
                if (C.body[vi][u] >= 0) add_windows(&C, vi, u);
        }
        (void)positives;
    }
    for (int i = 0; i < C.ns; ++i) solid_bounds(&C.s[i], &f, &tmp);
    for (int i = 0; i < C.ns; ++i) {
        if (C.s[i].neg) C.st.openings++;
        else C.st.solids++;
    }
    if (C.kit) hs_dress(&C, D);
    emit_all(&C);
    if (st) *st = C.st;
    free(C.pool);
    free(C.s);
    free(C.win); free(C.door); free(C.post); free(C.stack); free(C.roof);
}

void home_shell(const HGraph *g, int lod, const float *dy, HShellFn fn, void *ctx, HShellStats *st) { shell_build(g, lod, dy, fn, ctx, st, NULL); }
void home_shell_kit(const HGraph *g, const float *dy, HShellFn fn, void *ctx, HShellStats *st, HDress *D) {
    shell_build(g, HSH_NEAR, dy, fn, ctx, st, D);
}
