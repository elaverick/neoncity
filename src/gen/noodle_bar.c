#include "noodle_bar.h"
#include "hash.h"
#include "mathlib.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * The noodle bar's body (noodle_bar.h; roadmap NB4 in
 * ~/.claude/plans/street-vendors.md, after docs/noodle-bar-target.md).
 * ========================================================================= */

#define HALF_PI   1.5707963f
#define THICK     0.0015f   /* sheet */
#define RIVETS    0.06f     /* a rivet row's pitch */
#define STOOL_OUT 0.48f     /* a stool's middle out from the counter's face */
#define CUST      0.78f     /* the corner form: the customers' strip beyond its side run */
/* the cooking line (NB5) */
#define RANGE_TRAY  0.86f    /* the range's burner wells' floor */
#define WOK_FLOOR   0.75f    /* the wok well's floor (the wok burner stands on it) */
#define GRATE_TOP   0.047f   /* a grate's tips over its feet (pm_grate: 15 mm bars) */
#define PIPE_R      0.011f   /* the gas line (22 mm copper), its middle 11 mm off the lining (the saddle clips) */
#define BM_TOP      1.06f    /* the bain-marie's top: the pans' flanges rest on it */
#define BM_DEPTH    0.43f    /* its box, front to back: the well (0.325) and the control panel */
#define RAIL_LEN    0.6f     /* the utensil rail (pmodel_kit.c m_utensil_rail) */

static int g_form = -1;
void nb_force_form(int form) { g_form = form; }
int  nb_forced_form(void) { return g_form; }

float nb_hood_under(const NbPlan *p, float out) {
    const PmHood *h = &p->hood;
    float c = h->curl, s = clampf(out / maxf(h->span, 1e-3f), 0.0f, 1.0f) * sinf(c);
    float a = asinf(s);
    return h->spring + h->rise * (cosf(a) - cosf(c)) / (1.0f - cosf(c));
}

/* ------------------------------------------------------------------ plan --- */

static void add_stool(NbPlan *p, float x, float z) {
    for (int i = 0; i < p->nstool; ++i) {
        float dx = p->stool[i][0] - x, dz = p->stool[i][1] - z;
        if (dx * dx + dz * dz < 0.62f * 0.62f) return;
    }
    if (p->nstool < NB_STOOLS_MAX) { p->stool[p->nstool][0] = x; p->stool[p->nstool][1] = z; p->nstool++; }
}
/* stools along a line from a to b (x, z) at a pitch, centred */
static void stool_line(NbPlan *p, float ax, float az, float bx, float bz, float pitch, int most) {
    float L = sqrtf((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    if (L < 0.0f) return;
    int n = (int)floorf(L / pitch) + 1;
    if (n > most) n = most;
    if (n < 1) return;
    float used = (float)(n - 1) * pitch, s0 = 0.5f * (L - used);
    for (int k = 0; k < n; ++k) {
        float t = L > 1e-4f ? (s0 + pitch * (float)k) / L : 0.5f;
        add_stool(p, ax + (bx - ax) * t, az + (bz - az) * t);
    }
}

/* The legs of the hood's lip: for leg j, its spine from a to b, the way it reaches out (o), its length. */
static int hood_leg(const NbPlan *p, int j, float a[2], float b[2], float o[2], float *len) {
    const PmHood *h = &p->hood;
    if (j + 1 >= h->nspine) return 0;
    a[0] = h->spine[j][0]; a[1] = h->spine[j][1]; b[0] = h->spine[j + 1][0]; b[1] = h->spine[j + 1][1];
    float dx = b[0] - a[0], dz = b[1] - a[1], L = sqrtf(dx * dx + dz * dz);
    if (L < 1e-4f) return 0;
    o[0] = -dz / L; o[1] = dx / L;
    *len = L;
    return 1;
}

int nb_plan(NbPlan *p, float W, float D, float ceiling, uint32_t seed, int form) {
    memset(p, 0, sizeof(*p));
    Rng r = rng_make(hash_combine(seed, 0x4E42u));
    p->seed = seed; p->w = W; p->d = D;
    const float ceil = ceiling > 0.0f ? ceiling : 99.0f;
    /* every draw first, in order (hash.h: draws are sequenced) */
    float bw     = rng_range(&r, 3.4f, 4.4f);
    float bd     = rng_range(&r, 0.60f, 0.68f);
    float aisle  = rng_range(&r, 0.85f, 1.0f);
    float cd     = rng_range(&r, 0.50f, 0.55f);
    float cy     = rng_range(&r, 1.00f, 1.05f);
    float ov     = rng_range(&r, 0.65f, 0.9f);
    float spring = rng_range(&r, 2.45f, 2.6f);
    float rise   = rng_range(&r, 0.6f, 0.85f);
    float pitch  = rng_range(&r, 0.48f, 0.58f);
    float fh     = rng_range(&r, 0.30f, 0.36f);
    float spanC  = rng_range(&r, 1.65f, 2.0f);
    float pick   = rng_float(&r);
    float spitch = rng_range(&r, 0.64f, 0.72f);
    float rangeL = rng_float(&r);
    float sideR  = rng_float(&r);

    int fits = W >= 3.2f && D >= 3.0f && ceil >= 3.1f;
    bw = clampf(minf(bw, W - 0.1f), 2.4f, 4.4f);
    p->x0 = -0.5f * bw; p->x1 = 0.5f * bw;
    p->lining = 0.10f;
    /* depth: bench, the cook's aisle, the counter, the stools in front, the hood's overhang past them */
    float need = p->lining + bd + aisle + cd + 0.62f;
    if (need > D) aisle = maxf(0.8f, aisle - (need - D));
    need = p->lining + bd + aisle + cd + 0.62f;
    if (need > D) bd = maxf(0.55f, bd - (need - D));
    p->cd = cd; p->cy = cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = p->lining + bd + aisle + cd;
    p->lip = minf(p->front + ov, D - 0.02f);
    if (p->lip < p->front + 0.55f) fits = 0;
    /* under a ceiling the hood comes down; the fascia keeps 2.05 m under it */
    if (spring + rise > ceil - 0.15f) rise = maxf(0.35f, ceil - 0.15f - spring);
    if (spring + rise > ceil - 0.15f) spring = maxf(2.4f, ceil - 0.15f - rise);
    fh = minf(fh, spring - 2.05f);

    int corner = form >= 0 ? form == NB_CORNER : g_form >= 0 ? g_form == NB_CORNER : pick < 0.35f;
    if (bw < 3.4f || D < 3.2f) corner = 0;
    p->form = corner ? NB_CORNER : NB_STRAIGHT;
    p->side = sideR < 0.5f ? 1 : -1;

    PmHood *h = &p->hood;
    pm_hood_default(h, bw, 1.0f);
    h->spring = spring; h->rise = rise; h->pitch = pitch; h->fascia_h = fh;
    p->hoodTop = spring + rise;
    const float cz0 = p->front - cd;   /* the counter's back */
    int re;                            /* the range's end of the kitchen: -1 its -x end, +1 its +x */

    if (!corner) {
        h->spine[0][0] = p->x0; h->spine[0][1] = p->lining;
        h->spine[1][0] = p->x1; h->spine[1][1] = p->lining;
        h->nspine = 2;
        h->span = p->lip - p->lining;
        re = rangeL < 0.5f ? -1 : 1;
        /* side walls from the host wall to the counter's back; the lining across */
        NbWall *w = p->wall;
        w[0] = (NbWall){ { p->x0, 0.0f }, { 0.0f, 1.0f }, cz0, spring };
        w[1] = (NbWall){ { p->x1, cz0 }, { 0.0f, -1.0f }, cz0, spring };
        w[2] = (NbWall){ { p->x0, p->lining }, { 1.0f, 0.0f }, bw, p->hoodTop };
        p->nwall = 3;
        p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = cz0;
        /* the counter: one run, the cook's gate at the end away from the range */
        NbRun *c = &p->run[0];
        c->o[0] = p->x0; c->o[1] = p->front; c->u[0] = 1.0f; c->u[1] = 0.0f; c->len = bw;
        if (re < 0) { c->gate0 = bw - 0.68f; c->gate1 = bw - 0.06f; } else { c->gate0 = 0.06f; c->gate1 = 0.68f; }
        p->nrun = 1;
        stool_line(p, p->x0 + 0.45f, p->front + STOOL_OUT, p->x1 - 0.45f, p->front + STOOL_OUT, spitch, 6);
    } else {
        const int s = p->side;
        const float xe = s > 0 ? p->x1 : p->x0, xo = s > 0 ? p->x0 : p->x1;   /* the corner's end, the closed end */
        const float xc = xe - (float)s * CUST;                                 /* the side run's face */
        const float xi = xc - (float)s * cd;                                   /* its back */
        const float zs = p->lining + bd + 0.15f;                               /* where it starts */
        float span = clampf(spanC, 1.4f, minf(p->lip - p->lining - 0.5f, fabsf(xe - xo) - 0.9f));
        float zsp = p->lip - span, xsp = xe - (float)s * span;
        h->span = span;
        if (s > 0) {
            h->spine[0][0] = p->x0; h->spine[0][1] = zsp;
            h->spine[1][0] = xsp;   h->spine[1][1] = zsp;
            h->spine[2][0] = xsp;   h->spine[2][1] = p->lining;
        } else {
            h->spine[0][0] = xsp;   h->spine[0][1] = p->lining;
            h->spine[1][0] = xsp;   h->spine[1][1] = zsp;
            h->spine[2][0] = p->x1; h->spine[2][1] = zsp;
        }
        h->nspine = 3;
        p->deck[0] = minf(xo, xsp); p->deck[2] = maxf(xo, xsp); p->deck[1] = p->lining; p->deck[3] = zsp;
        re = -s;
        /* the closed end's wall: up to the deck behind the spine, to the spring in front; the lining; the short
         * wall at the corner end from the lining to the side run, up under the side leg */
        NbWall *w = p->wall;
        if (s > 0) {
            w[0] = (NbWall){ { p->x0, 0.0f }, { 0.0f, 1.0f }, zsp, p->hoodTop };
            w[1] = (NbWall){ { p->x0, zsp }, { 0.0f, 1.0f }, cz0 - zsp, spring };
            w[3] = (NbWall){ { xc, zs }, { 0.0f, -1.0f }, zs, 0.0f };
        } else {
            w[0] = (NbWall){ { p->x1, zsp }, { 0.0f, -1.0f }, zsp, p->hoodTop };
            w[1] = (NbWall){ { p->x1, cz0 }, { 0.0f, -1.0f }, cz0 - zsp, spring };
            w[3] = (NbWall){ { xc, 0.0f }, { 0.0f, 1.0f }, zs, 0.0f };
        }
        /* the lining: up to the deck behind it, to the spring under the side leg (its end plate closes above) */
        if (s > 0) { w[2] = (NbWall){ { p->x0, p->lining }, { 1.0f, 0.0f }, xsp - p->x0, p->hoodTop };
                     w[4] = (NbWall){ { xsp, p->lining }, { 1.0f, 0.0f }, p->x1 - xsp, spring }; }
        else       { w[2] = (NbWall){ { xsp, p->lining }, { 1.0f, 0.0f }, p->x1 - xsp, p->hoodTop };
                     w[4] = (NbWall){ { p->x0, p->lining }, { 1.0f, 0.0f }, xsp - p->x0, spring }; }
        w[3].h = nb_hood_under(p, fabsf(xc - xsp)) - 0.01f;
        p->nwall = 5;
        p->kx0 = s > 0 ? p->x0 + 0.05f : xi; p->kx1 = s > 0 ? xi : p->x1 - 0.05f; p->kz1 = cz0;
        /* the counter: the front run to the side run's face, the side run back to the short wall; the gate in
         * the front run at the closed end */
        NbRun *c = p->run;
        float flen = fabsf(xc - xo);
        if (s > 0) { c[0] = (NbRun){ { p->x0, p->front }, { 1.0f, 0.0f }, flen, 0.06f, 0.68f };
                     c[1] = (NbRun){ { xc, cz0 }, { 0.0f, -1.0f }, cz0 - zs, 0.0f, 0.0f }; }
        else       { c[0] = (NbRun){ { xc, p->front }, { 1.0f, 0.0f }, flen, flen - 0.68f, flen - 0.06f };
                     c[1] = (NbRun){ { xc, zs }, { 0.0f, 1.0f }, cz0 - zs, 0.0f, 0.0f }; }
        p->nrun = 2;
        /* the column in the counter's corner, up under the hood's hip */
        float colx = xc - (float)s * 0.5f * cd, colz = p->front - 0.5f * cd;
        p->column[0] = colx; p->column[1] = colz; p->column[2] = 0.5f * cd + 0.04f;
        p->column[3] = nb_hood_under(p, maxf(colz - zsp, (float)s * (colx - xsp)) + p->column[2]) - 0.005f;
        stool_line(p, xo + (float)s * 0.45f, p->front + STOOL_OUT, xc - (float)s * 0.32f, p->front + STOOL_OUT, spitch, 5);
        stool_line(p, xc + (float)s * STOOL_OUT, p->front - 0.2f, xc + (float)s * STOOL_OUT, zs + 0.3f, spitch, 3);
    }
    /* the posts at the lip's free corners */
    p->fascia = h->span - 0.5f * h->lip_r + 0.0015f;
    const float ph = spring - fh;
    if (!corner) {
        float z = p->lining + p->fascia - 0.035f - 0.003f;
        p->post[0][0] = p->x0 + 0.035f; p->post[0][1] = z; p->post[0][2] = ph;
        p->post[1][0] = p->x1 - 0.035f; p->post[1][1] = z; p->post[1][2] = ph;
        p->npost = 2;
    } else {
        const int s = p->side;
        const float xo = s > 0 ? p->x0 : p->x1, zsp = p->lip - h->span, xsp = (s > 0 ? p->x1 : p->x0) - (float)s * h->span;
        p->post[0][0] = xo + (float)s * 0.035f; p->post[0][1] = zsp + p->fascia - 0.038f; p->post[0][2] = ph;
        p->post[1][0] = xsp + (float)s * (p->fascia - 0.038f); p->post[1][1] = p->lining + 0.04f; p->post[1][2] = ph;
        p->npost = 2;
    }
    /* the kitchen along the lining: the bench, the range at one end, the gas at the other, the shelves between */
    if (re < 0) {
        p->benchX0 = p->kx0; p->benchX1 = p->kx1 - 0.6f; p->gasX = p->kx1 - 0.3f;
        p->rangeX0 = p->benchX0 + 0.15f; p->rangeX1 = p->rangeX0 + 0.95f;
        p->shelfX0 = p->rangeX1 + 0.25f; p->shelfX1 = p->benchX1 - 0.05f;
    } else {
        p->benchX1 = p->kx1; p->benchX0 = p->kx0 + 0.6f; p->gasX = p->kx0 + 0.3f;
        p->rangeX1 = p->benchX1 - 0.15f; p->rangeX0 = p->rangeX1 - 0.95f;
        p->shelfX0 = p->benchX0 + 0.05f; p->shelfX1 = p->rangeX0 - 0.25f;
    }
    if (p->shelfX1 - p->shelfX0 < 0.5f) p->shelfX0 = p->shelfX1 = 0.0f;
    p->ductR = 0.13f;
    p->ductX = 0.5f * (p->rangeX0 + p->rangeX1);
    p->ductZ = p->lining + 0.035f + p->ductR;
    p->ductTop = minf(p->hoodTop + 0.75f, ceil - 0.05f);
    p->top = p->ductTop + 0.27f;
    if (p->top > ceil) p->top = ceil;

    /* the strip lights' tubes behind the fascia, and the icon housings on it */
    const float yt = spring - fh + 0.05f;
    for (int j = 0; j + 1 < h->nspine; ++j) {
        float a[2], b[2], o[2], L;
        if (!hood_leg(p, j, a, b, o, &L)) continue;
        int nt = (int)(L / 1.5f);
        nt = nt < 1 ? 1 : nt > 3 ? 3 : nt;
        float tl = minf(1.2f, L / (float)nt - 0.25f);
        for (int k = 0; k < nt && p->ntube < NB_TUBES_MAX && tl > 0.4f; ++k) {
            float f = ((float)k + 0.5f) / (float)nt, off = p->fascia - 0.11f;
            float cx = a[0] + (b[0] - a[0]) * f + o[0] * off, cz = a[1] + (b[1] - a[1]) * f + o[1] * off;
            float ux = (b[0] - a[0]) / L, uz = (b[1] - a[1]) / L;
            float *t = p->tube[p->ntube++];
            t[0] = cx - ux * 0.5f * tl; t[1] = yt; t[2] = cz - uz * 0.5f * tl;
            t[3] = cx + ux * 0.5f * tl; t[4] = yt; t[5] = cz + uz * 0.5f * tl;
        }
        /* icons face the street (the front leg), one on a long enough side leg */
        int front = fabsf(o[1]) > 0.5f;
        int ni = front ? (int)((L - 0.3f) / 0.9f) : (L > 1.2f ? 1 : 0);
        ni = ni < 0 ? 0 : ni > 4 ? 4 : ni;
        float size = fh - 0.05f;
        for (int k = 0; k < ni && p->nicon < NB_ICONS_MAX; ++k) {
            float f = ((float)k + 0.5f) / (float)ni, off = p->fascia;
            float *c = p->icon[p->nicon++];
            c[0] = a[0] + (b[0] - a[0]) * f + o[0] * off; c[1] = spring - 0.5f * fh; c[2] = a[1] + (b[1] - a[1]) * f + o[1] * off;
            c[3] = o[0]; c[4] = o[1]; c[5] = size;
        }
    }
    /* the A-frame out front at the closed end; the bell on the counter near the gate */
    {
        float xo = p->form == NB_CORNER ? (p->side > 0 ? p->x0 : p->x1) : (re < 0 ? p->x0 : p->x1);
        float sg = xo < 0.0f ? 1.0f : -1.0f;
        p->aframe[0] = xo + sg * 0.38f;
        p->aframe[1] = clampf(p->lip + 0.3f, p->front + 0.9f, D - 0.22f);
        p->aframe[2] = 0.0f;
        const NbRun *c = &p->run[0];
        float g = 0.5f * (c->gate0 + c->gate1), at = clampf(g < 0.5f * c->len ? g + 0.75f : g - 0.75f, 0.2f, c->len - 0.2f);
        p->bell[0] = c->o[0] + c->u[0] * at; p->bell[1] = cy; p->bell[2] = p->front - 0.45f * cd;
    }
    /* NB6: light and signs, by hash (the draws above stay NB5's) */
    {
        const uint32_t hl = hash_combine(seed, 0x4C4954u);
        p->tubeWhite = hash_float(hash_combine(hl, 1u)) < 0.6f ? NB_WHITE_6500 : NB_WHITE_5000;
        /* the icons' neon: a pair of colours a vendor, red and cyan most (the references) */
        static const uint8_t PAIR[6][2] = { { 2, 1 }, { 2, 1 }, { 0, 1 }, { 1, 2 }, { 2, 3 }, { 0, 5 } };
        const int pair = (int)(hash_float(hash_combine(hl, 2u)) * 6.0f) % 6;
        const int first = (int)(hash_float(hash_combine(hl, 3u)) * 4.0f) & 3;
        const int bad = hash_float(hash_combine(hl, 4u)) < 0.35f ? (int)(hash_float(hash_combine(hl, 5u)) * (float)NB_ICONS_MAX) : -1;
        const int badMode = hash_float(hash_combine(hl, 6u)) < 0.35f ? 2 : 1;
        for (int i = 0; i < p->nicon; ++i) {
            p->iconKind[i] = (uint8_t)(PMK_ICON_BOWL + ((first + i) & 3));
            p->iconGlow[i] = (uint8_t)(PAIR[pair][i & 1] | (i == bad ? badMode : 0) << 3);
        }
        /* the lamp hangs under the extractor's baffles, over the range's middle */
        p->lamp[0] = 0.5f * (p->rangeX0 + p->rangeX1); p->lamp[1] = 1.955f; p->lamp[2] = p->lining + 0.40f;
        /* the batten under the lower shelf, clear of its brackets */
        p->batten[0] = p->batten[1] = 0.0f;
        if (p->shelfX1 - p->shelfX0 > 0.65f) {
            p->batten[0] = p->shelfX0 + 0.12f; p->batten[1] = p->shelfX1 - 0.12f;
            p->batten[2] = 1.43f - 0.022f; p->batten[3] = p->lining + 0.20f;
        }
    }
    return fits;
}

uint8_t nb_icon_seed(const NbPlan *p, int icon) { return (uint8_t)hash_combine(p->seed, 0x1C0u + (uint32_t)icon); }

int nb_lights(const NbPlan *p, NbLight *out, int max) {
    int n = 0;
#define NB_ADD(K, C) (n < max ? (memset(&out[n], 0, sizeof(NbLight)), out[n].kind = (K), out[n].colour = (C), &out[n++]) : NULL)
    for (int i = 0; i < p->ntube; ++i) {
        NbLight *l = NB_ADD(NB_LIGHT_TUBE, p->tubeWhite);
        if (!l) break;
        memcpy(l->a, &p->tube[i][0], sizeof(l->a)); memcpy(l->b, &p->tube[i][3], sizeof(l->b));
    }
    if (p->lamp[1] > 0.0f) {
        NbLight *l = NB_ADD(NB_LIGHT_LAMP, NB_WHITE_2700);
        if (l) { l->a[0] = p->lamp[0]; l->a[1] = p->lamp[1] - 0.035f; l->a[2] = p->lamp[2]; }
    }
    if (p->batten[1] > p->batten[0]) {
        NbLight *l = NB_ADD(NB_LIGHT_TUBE, NB_WHITE_3000);
        if (l) {
            l->a[0] = p->batten[0]; l->a[1] = p->batten[2] - 0.004f; l->a[2] = p->batten[3];
            l->b[0] = p->batten[1]; l->b[1] = l->a[1]; l->b[2] = l->a[2];
        }
    }
    NbLine L;
    nb_line(p, &L);
    for (int b = 0; b < 2; ++b) {
        const int wok = b == 1 && L.kind == NB_POT_WOK;
        NbLight *l = NB_ADD(NB_LIGHT_FLAME, NB_FLAME_BLUE);
        if (!l) break;
        l->a[0] = L.burner[b][0]; l->a[1] = wok ? WOK_FLOOR + 0.08f : RANGE_TRAY + 0.04f; l->a[2] = L.burner[b][1];
        l->seed = (uint8_t)hash_combine(p->seed, 0xF1u + (uint32_t)b);
    }
    for (int i = 0; i < p->nicon; ++i) {
        const float *c = p->icon[i];
        NbLight *l = NB_ADD(NB_LIGHT_NEON, (uint8_t)(NB_NEON + (p->iconGlow[i] & 7u)));
        if (!l) break;
        /* in front of its tubes, so its own housing never shadows it */
        l->a[0] = c[0] + c[3] * 0.12f; l->a[1] = c[1]; l->a[2] = c[2] + c[4] * 0.12f;
        l->b[0] = c[3]; l->b[1] = c[4];
        l->size = c[5];
        l->flicker = (uint8_t)(p->iconGlow[i] >> 3);
        l->seed = nb_icon_seed(p, i);
    }
#undef NB_ADD
    return n;
}

/* ----------------------------------------------------------- the helpers --- */

/* A frame on the site: origin (x, y, z), local x along (ux, uz), y up, z out (cross(u, up)). */
static void frame_on(Pm *pm, float x, float y, float z, float ux, float uz) {
    float o[3] = { x, y, z }, ex[3] = { ux, 0.0f, uz }, ey[3] = { 0.0f, 1.0f, 0.0f }, ez[3] = { -uz, 0.0f, ux };
    pm_frame(pm, o, ex, ey, ez);
}
static void bb(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    if (h[0] < 1e-4f || h[1] < 1e-4f || h[2] < 1e-4f) return;
    pm_bbox(pm, c, h, bevel, 1);
}
/* A sheet facing up over a top (x0..x1, z0..z1 at y), its vertices every `step` or so: where the line's things
 * stand on it, the body's baked occlusion (pm_bake_with: the line as occluders) has vertices to darken round
 * them (NB6). It lies a millimetre over the slab whose top it is. */
#define TOP_STEP 0.08f
static void top_sheet(Pm *pm, float x0, float z0, float x1, float z1, float y) {
    const int nx = (int)ceilf((x1 - x0) / TOP_STEP), nz = (int)ceilf((z1 - z0) / TOP_STEP);
    if (nx < 1 || nz < 1) return;
    pm->shell++;
    const float n[3] = { 0.0f, 1.0f, 0.0f };
    uint32_t first = 0;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            float q[3] = { x0 + (x1 - x0) * (float)i / (float)nx, y, z0 + (z1 - z0) * (float)j / (float)nz };
            uint32_t id = pm_vert(pm, q, n, q[0], q[2]);
            if (!i && !j) first = id;
        }
    for (int j = 0; j < nz; ++j)
        for (int i = 0; i < nx; ++i) {
            uint32_t a = first + (uint32_t)(j * (nx + 1) + i);
            pm_quad(pm, a, a + 1, a + (uint32_t)(nx + 1) + 1, a + (uint32_t)(nx + 1));
        }
}
static void put(Pm *pm, float x, float y, float z, float a, int id) {
    pm_push(pm);
    pm_translate(pm, x, y, z);
    if (a != 0.0f) pm_rotate_y(pm, a);
    pm_place(pm, id);
    pm_pop(pm);
}
static void rivets(Pm *pm, float ax, float ay, float az, float bx, float by, float bz, float nx, float ny, float nz) {
    float a[3] = { ax, ay, az }, b[3] = { bx, by, bz }, n[3] = { nx, ny, nz };
    pm_rivet_row(pm, a, b, n, RIVETS);
}
/* A cylinder along local x from x0 to x1, radius r, its axis at (y, z). */
static void cyl_x(Pm *pm, float x0, float x1, float y, float z, float r, int segs) {
    pm_push(pm);
    pm_translate(pm, x0, y, z);
    pm_rotate_z(pm, -HALF_PI);   /* y onto +x */
    pm_cyl(pm, 0.0f, x1 - x0, r, segs);
    pm_pop(pm);
}

/* ---------------------------------------------------------------- walls --- */

/* A wall in its frame: x along it, z out (its outside: the street for a side wall, the kitchen for the back
 * lining). A side wall (framing) is painted sheet outside, a flat brushed stainless lining inside, stainless
 * angle framing over that; the back lining is brushed stainless sheet (NB4b: the kitchen's metal is
 * stainless; the lining a sheet of its own, so no one set covers most of the bar). */
static void wall(Pm *pm, const NbPlan *p, const NbWall *w, int k, int framing) {
    if (w->len < 0.05f || w->h < 0.05f) return;
    pm_push(pm);
    frame_on(pm, w->o[0], 0.0f, w->o[1], w->u[0], w->u[1]);
    pm_push(pm);
    pm_translate(pm, 0.0f, 0.0f, -2.0f * THICK);
    pm_mat(pm, framing ? PMAT_PLATE : PMAT_LINING);
    pm->part = (uint8_t)(16 + k * 8);
    float sheet = 1.0f + 0.25f * hash_float(hash_combine(p->seed, 0x5711u + (uint32_t)k));
    pm_sheet_wall(pm, w->len, w->h, sheet, THICK, 1);
    pm_pop(pm);
    if (framing) {
        /* the lining, then angle framing over it: posts at the ends, a rail at the top and one at counter height */
        pm_mat(pm, PMAT_LINING);
        const float zi = -2.0f * THICK, L = w->len, H = w->h;
        bb(pm, 0.0f, 0.012f, zi - 0.0035f, L, H, zi - 0.0015f, 0.0f);
        pm_mat(pm, PMAT_STAINLESS);
        for (int e = 0; e < 2; ++e) {
            float a = e ? L - 0.04f : 0.0f, lg = e ? L - 0.004f : 0.0f;
            bb(pm, a, 0.0f, zi - 0.004f, a + 0.04f, H, zi, 0.0015f);
            bb(pm, lg, 0.0f, zi - 0.04f, lg + 0.004f, H, zi - 0.004f, 0.0015f);
        }
        float ys[2] = { H - 0.045f, 1.05f };
        for (int r = 0; r < 2; ++r) {
            if (ys[r] > H - 0.05f && r) continue;
            bb(pm, 0.04f, ys[r], zi - 0.004f, L - 0.04f, ys[r] + 0.04f, zi, 0.0015f);
            bb(pm, 0.04f, ys[r], zi - 0.04f, L - 0.04f, ys[r] + 0.004f, zi - 0.004f, 0.0015f);
        }
    }
    pm_pop(pm);
}

static void post(Pm *pm, float x, float z, float h) {
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, x - 0.03f, 0.012f, z - 0.03f, x + 0.03f, h - 0.008f, z + 0.03f, 0.004f);
    pm_mat(pm, PMAT_STEEL);
    bb(pm, x - 0.075f, 0.0f, z - 0.075f, x + 0.075f, 0.012f, z + 0.075f, 0.002f);
    bb(pm, x - 0.05f, h - 0.008f, z - 0.05f, x + 0.05f, h, z + 0.05f, 0.002f);
    for (int k = 0; k < 4; ++k) put(pm, x + ((k & 1) ? 0.052f : -0.052f), 0.012f, z + ((k & 2) ? 0.052f : -0.052f), 0.0f, PMK_BOLT);
}

/* -------------------------------------------------------------- counter --- */

/* One run, in its frame: x 0..len along it, its face at z 0 (customers +z), its back at -cd. */
static void counter_run(Pm *pm, const NbPlan *p, const NbRun *r, int k) {
    const float L = r->len, cd = p->cd, cy = p->cy, zb = -cd + 0.05f, yt = cy - 0.04f;
    if (L < 0.2f) return;
    pm_push(pm);
    frame_on(pm, r->o[0], 0.0f, r->o[1], r->u[0], r->u[1]);
    const int gate = r->gate1 > r->gate0 + 0.2f;
    float seg[2][2];
    int ns = 0;
    if (gate) {
        if (r->gate0 > 0.05f) { seg[ns][0] = 0.0f; seg[ns][1] = r->gate0; ns++; }
        if (r->gate1 < L - 0.05f) { seg[ns][0] = r->gate1; seg[ns][1] = L; ns++; }
    } else { seg[0][0] = 0.0f; seg[0][1] = L; ns = 1; }
    for (int s = 0; s < ns; ++s) {
        const float a = seg[s][0], b = seg[s][1];
        pm->part = (uint8_t)(64 + k * 8 + s * 3);
        /* the carcass, its kick plate, the top */
        pm_mat(pm, PMAT_PLATE);
        bb(pm, a, 0.10f, zb, b, yt, 0.0f, 0.003f);
        pm_mat(pm, PMAT_STEEL);
        bb(pm, a, 0.0f, zb + 0.01f, b, 0.10f, 0.004f, 0.002f);
        pm_mat(pm, PMAT_STAINLESS);
        {
            const float ta = a - (a < 0.01f ? 0.02f : 0.0f), tb = b + (b > L - 0.01f ? 0.02f : 0.0f);
            bb(pm, ta, yt, -cd + 0.03f, tb, cy, 0.03f, 0.006f);
            top_sheet(pm, ta + 0.006f, -cd + 0.036f, tb - 0.006f, 0.024f, cy + 0.001f);   /* the condiments' contact */
        }
        /* the front's panels: frames of flat bar 6 mm proud, riveted along their middles */
        pm_mat(pm, PMAT_PLATE);
        int np = (int)floorf((b - a) / 0.6f + 0.5f);
        if (np < 1) np = 1;
        const float fw = 0.05f, y0 = 0.10f, y1 = yt;
        bb(pm, a, y1 - 0.06f, 0.0f, b, y1, 0.006f, 0.002f);
        bb(pm, a, y0, 0.0f, b, y0 + 0.06f, 0.006f, 0.002f);
        rivets(pm, a + 0.03f, y1 - 0.03f, 0.006f, b - 0.03f, y1 - 0.03f, 0.006f, 0, 0, 1);
        rivets(pm, a + 0.03f, y0 + 0.03f, 0.006f, b - 0.03f, y0 + 0.03f, 0.006f, 0, 0, 1);
        for (int i = 0; i <= np; ++i) {
            float x = a + (b - a) * (float)i / (float)np;
            float xa = clampf(x - 0.5f * fw, a, b - fw);
            bb(pm, xa, y0 + 0.06f, 0.0f, xa + fw, y1 - 0.06f, 0.006f, 0.002f);
            rivets(pm, xa + 0.5f * fw, y0 + 0.09f, 0.006f, xa + 0.5f * fw, y1 - 0.09f, 0.006f, 0, 0, 1);
        }
        /* the foot rail on its brackets, its ends bent back into the front */
        if (b - a > 0.8f) {
            pm_mat(pm, PMAT_STEEL);
            PmPath pa, pf;
            pm_path_init(&pa, 4);
            const float ra = a + 0.15f, rb = b - 0.15f, yr = 0.20f, zr = 0.25f;
            pm_path_add(&pa, ra, yr, 0.004f, 1); pm_path_add(&pa, ra, yr, zr, 1);
            pm_path_add(&pa, rb, yr, zr, 1); pm_path_add(&pa, rb, yr, 0.004f, 1);
            pm_path_init(&pf, 32);
            pm_path_fillet(&pa, 0.07f, 4, &pf);
            pm_tube(pm, &pf, 0.022f, 14, 0);
            pm_path_free(&pa); pm_path_free(&pf);
            int nb = (int)((rb - ra) / 1.1f);
            for (int i = 1; i <= nb; ++i) {
                float x = ra + (rb - ra) * (float)i / (float)(nb + 1);
                float c0[3] = { x, yr + 0.16f, 0.006f }, c1[3] = { x, yr + 0.01f, zr - 0.01f };
                pm_beam(pm, c0, c1, 0.03f, 0.006f, 0.0015f, NULL);
                bb(pm, x - 0.03f, yr + 0.12f, 0.006f, x + 0.03f, yr + 0.2f, 0.012f, 0.0015f);
            }
        }
    }
    if (gate) {
        /* the half door, its hinges and handle; the flap over it, hinged along its side */
        const float g0 = r->gate0, g1 = r->gate1;
        pm->part = (uint8_t)(64 + k * 8 + 7);
        pm_mat(pm, PMAT_PLATE);
        bb(pm, g0 + 0.008f, 0.12f, -0.03f, g1 - 0.008f, yt - 0.02f, -0.012f, 0.003f);
        bb(pm, g0 + 0.06f, 0.20f, -0.012f, g1 - 0.06f, 0.24f, -0.006f, 0.0015f);
        bb(pm, g0 + 0.06f, yt - 0.14f, -0.012f, g1 - 0.06f, yt - 0.10f, -0.006f, 0.0015f);
        pm_mat(pm, PMAT_STEEL);
        for (int h = 0; h < 2; ++h) { pm_push(pm); pm_translate(pm, g0 + 0.012f, h ? yt - 0.25f : 0.25f, -0.008f); pm_cyl(pm, 0.0f, 0.09f, 0.009f, 10); pm_pop(pm); }
        PmPath pa, pf;
        pm_path_init(&pa, 4);
        float hx = g1 - 0.07f;
        pm_path_add(&pa, hx, 0.62f, -0.012f, 1); pm_path_add(&pa, hx, 0.62f, 0.03f, 1);
        pm_path_add(&pa, hx, 0.80f, 0.03f, 1); pm_path_add(&pa, hx, 0.80f, -0.012f, 1);
        pm_path_init(&pf, 16);
        pm_path_fillet(&pa, 0.012f, 3, &pf);
        pm_tube(pm, &pf, 0.007f, 8, 0);
        pm_path_free(&pa); pm_path_free(&pf);
        pm_mat(pm, PMAT_STAINLESS);
        bb(pm, g0 + 0.003f, yt + 0.004f, -cd + 0.03f, g1 - 0.003f, cy, 0.03f, 0.004f);
        pm_mat(pm, PMAT_STEEL);
        for (int h = 0; h < 2; ++h) {
            pm_push(pm);
            pm_translate(pm, g0 + 0.003f, cy + 0.002f, -cd * (h ? 0.75f : 0.25f) - 0.025f);
            pm_rotate_x(pm, HALF_PI);
            pm_cyl(pm, 0.0f, 0.05f, 0.006f, 8);
            pm_pop(pm);
        }
    }
    pm_pop(pm);
}

/* -------------------------------------------------------------- kitchen --- */

/* A duct's flanged joint: a ring 30 mm proud and 10 mm thick round it, eight bolts. */
static void duct_flange(Pm *pm, float x, float y, float z, float r) {
    PmProf f[4] = { { r - 0.002f, -0.005f, 1 }, { r + 0.03f, -0.005f, 1 }, { r + 0.03f, 0.005f, 1 }, { r - 0.002f, 0.005f, 1 } };
    pm_push(pm);
    pm_translate(pm, x, y, z);
    pm_lathe(pm, f, 4, 32, 0.0f, 0.0f);
    for (int k = 0; k < 8; ++k) {
        float a = 0.7853982f * ((float)k + 0.5f);
        put(pm, (r + 0.018f) * cosf(a), 0.005f, (r + 0.018f) * sinf(a), 0.0f, PMK_BOLT);
    }
    pm_pop(pm);
}

static int bench_legs(float b0, float b1, float *x, int max);
static void bench_span(const NbPlan *p, const NbLine *L, float *b0, float *b1);

/* The range's carcass (NB5): stainless on a steel plinth, two doors, the knob rail proud along its front, a
 * stainless top over its burner wells (a cast-iron tray under each ring burner, a deep well for a wok burner),
 * a back guard the gas line drops into. */
static void range_carcass(Pm *pm, const NbPlan *p, const NbLine *L) {
    const float zl = p->lining, zf = zl + p->benchD, x0 = L->carc0, x1 = L->carc1, bz = L->burner[0][1], hw = 0.21f;
    pm->part = 126;
    pm_mat(pm, PMAT_STEEL);
    bb(pm, x0 + 0.03f, 0.0f, zl + 0.03f, x1 - 0.03f, 0.10f, zf - 0.04f, 0.002f);
    pm_mat(pm, PMAT_STAINLESS);
    bb(pm, x0, 0.10f, zl, x1, 0.74f, zf, 0.003f);
    bb(pm, x0, 0.74f, zl, x0 + 0.012f, RANGE_TRAY, zf, 0.0015f);
    bb(pm, x1 - 0.012f, 0.74f, zl, x1, RANGE_TRAY, zf, 0.0015f);
    bb(pm, x0, 0.74f, zl, x1, RANGE_TRAY, zl + 0.012f, 0.0015f);
    bb(pm, x0, 0.74f, zf - 0.02f, x1, RANGE_TRAY, zf + 0.03f, 0.004f);   /* the knob rail */
    /* the doors and their bar handles */
    const int nd = x1 - x0 > 0.8f ? 2 : 1;
    for (int d = 0; d < nd; ++d) {
        const float a = x0 + (x1 - x0) * (float)d / (float)nd + 0.006f, b = x0 + (x1 - x0) * (float)(d + 1) / (float)nd - 0.006f;
        pm->part = (uint8_t)(127 + d);
        bb(pm, a, 0.14f, zf, b, 0.70f, zf + 0.008f, 0.002f);
        pm_mat(pm, PMAT_STEEL);
        const float hx0 = a + 0.08f, hx1 = b - 0.08f;
        if (hx1 > hx0) {
            PmPath pa, pf;
            pm_path_init(&pa, 4);
            pm_path_add(&pa, hx0, 0.64f, zf + 0.008f, 1); pm_path_add(&pa, hx0, 0.64f, zf + 0.04f, 1);
            pm_path_add(&pa, hx1, 0.64f, zf + 0.04f, 1); pm_path_add(&pa, hx1, 0.64f, zf + 0.008f, 1);
            pm_path_init(&pf, 24);
            pm_path_fillet(&pa, 0.012f, 3, &pf);
            pm_tube(pm, &pf, 0.007f, 8, 0);
            pm_path_free(&pa); pm_path_free(&pf);
        }
        pm_mat(pm, PMAT_STAINLESS);
    }
    /* the top: stainless strips round the wells, at bench height (cast iron there read as blotched granite in
     * the sun; the grates and the wells' trays stay cast iron) */
    pm->part = 129;
    pm_mat(pm, PMAT_STAINLESS);
    float wx[2] = { L->burner[0][0], L->burner[1][0] };
    if (wx[0] > wx[1]) { float t = wx[0]; wx[0] = wx[1]; wx[1] = t; }
    const float yt = p->benchY;
    bb(pm, x0, RANGE_TRAY, zl, x1, yt, bz - hw, 0.002f);
    bb(pm, x0, RANGE_TRAY, bz + hw, x1, yt, zf + 0.03f, 0.003f);
    bb(pm, x0, RANGE_TRAY, bz - hw, wx[0] - hw, yt, bz + hw, 0.002f);
    bb(pm, wx[0] + hw, RANGE_TRAY, bz - hw, wx[1] - hw, yt, bz + hw, 0.002f);
    bb(pm, wx[1] + hw, RANGE_TRAY, bz - hw, x1, yt, bz + hw, 0.002f);
    /* the wells: a tray under a ring burner; a deep well, stainless walls, under a wok burner */
    for (int b = 0; b < 2; ++b) {
        const float x = L->burner[b][0];
        const int wok = b == 1 && L->kind == NB_POT_WOK;
        pm_mat(pm, PMAT_CAST_IRON);
        if (!wok) { bb(pm, x - hw, RANGE_TRAY - 0.006f, bz - hw, x + hw, RANGE_TRAY, bz + hw, 0.0f); continue; }
        bb(pm, x - hw, WOK_FLOOR - 0.006f, bz - hw, x + hw, WOK_FLOOR, bz + hw, 0.0f);
        pm_mat(pm, PMAT_STAINLESS);
        bb(pm, x - hw, WOK_FLOOR, bz - hw, x - hw + 0.004f, RANGE_TRAY, bz + hw, 0.0f);
        bb(pm, x + hw - 0.004f, WOK_FLOOR, bz - hw, x + hw, RANGE_TRAY, bz + hw, 0.0f);
        bb(pm, x - hw, WOK_FLOOR, bz - hw, x + hw, RANGE_TRAY, bz - hw + 0.004f, 0.0f);
        bb(pm, x - hw, WOK_FLOOR, bz + hw - 0.004f, x + hw, RANGE_TRAY, bz + hw, 0.0f);
    }
    /* the back guard */
    pm_mat(pm, PMAT_STAINLESS);
    bb(pm, x0, yt, zl, x1, yt + 0.065f, zl + 0.06f, 0.004f);
}

/* The bain-marie (NB5): a stainless box on the bench, its top a frame round a well the GN pans hang in, a
 * control panel along its front (the dial is a kit knob), a label, pilot lights, rubber feet. */
static void bain_marie(Pm *pm, const NbPlan *p, const NbLine *L) {
    (void)p;
    if (!L->bmPans) return;
    const float x0 = L->bm0, x1 = L->bm1, z0 = L->bmZ0, z1 = L->bmZ1, y0 = p->benchY + 0.012f, y1 = BM_TOP;
    const float wx0 = x0 + 0.025f, wx1 = x1 - 0.025f, wz0 = z0 + 0.02f, wz1 = wz0 + 0.325f;
    pm->part = 136;
    pm_mat(pm, PMAT_RUBBER);
    for (int k = 0; k < 4; ++k) {
        const float fx = (k & 1) ? x1 - 0.04f : x0 + 0.04f, fz = (k & 2) ? z1 - 0.04f : z0 + 0.04f;
        bb(pm, fx - 0.015f, p->benchY, fz - 0.015f, fx + 0.015f, y0, fz + 0.015f, 0.003f);
    }
    pm_mat(pm, PMAT_STAINLESS);
    bb(pm, x0, y0, wz1, x1, y1, z1, 0.004f);                 /* the control panel's housing */
    bb(pm, x0, y0, z0, x1, y1, wz0, 0.003f);                 /* the back */
    bb(pm, x0, y0, wz0, wx0, y1, wz1, 0.002f);               /* the ends */
    bb(pm, wx1, y0, wz0, x1, y1, wz1, 0.002f);
    bb(pm, wx0, y0, wz0, wx1, y0 + 0.008f, wz1, 0.0f);       /* the well's floor */
    /* the water in the well, glimpsed between the pans */
    pm_mat(pm, PMAT_GLASS);
    bb(pm, wx0, y1 - 0.09f, wz0, wx1, y1 - 0.088f, wz1, 0.0f);
    /* the label, two pilot lights (NB6 lights them) */
    pm_mat(pm, PMAT_PAPER);
    pm->part = 137;
    bb(pm, x0 + 0.13f, 1.0f, z1, x0 + 0.26f, 1.03f, z1 + 0.0012f, 0.0f);
    for (int k = 0; k < 2; ++k) {
        pm_mat(pm, k ? PMAT_RED : PMAT_GLASS);
        pm_push(pm);
        pm_translate(pm, x1 - 0.05f - 0.035f * (float)k, 0.985f, z1);
        pm_rotate_x(pm, HALF_PI);
        PmProf lp[5] = { { 0, 0, 0 }, { 0.0075f, 0, 1 }, { 0.0075f, 0.004f, 1 }, { 0.0055f, 0.0075f, 0 }, { 0, 0.009f, 0 } };
        pm_lathe(pm, lp, 5, 12, 0.0f, 0.0f);
        pm_pop(pm);
    }
}

/* The gas line (NB5): 22 mm copper from its hose tail by the bottles, along the lining in its clips, down
 * beside the splashback, behind the range into its back guard; the regulator's rubber hose to its tail. */
static void gas_line(Pm *pm, const NbPlan *p, const NbLine *L) {
    const float zl = p->lining, zp = zl + PIPE_R, y = L->pipeY, low = p->benchY + 0.095f;
    const float xm = 0.5f * (L->burner[0][0] + L->burner[1][0]);
    pm->part = 138;
    pm_mat(pm, PMAT_COPPER);
    PmPath pa;
    pm_path_init(&pa, 8);
    pm_path_add(&pa, L->pipeX0, y - 0.07f, zp, 1);
    pm_path_add(&pa, L->pipeX0, y, zp, 1);
    pm_path_add(&pa, L->pipeX1, y, zp, 1);
    pm_path_add(&pa, L->pipeX1, low, zp, 1);
    pm_path_add(&pa, xm, low, zp, 1);
    pm_path_add(&pa, xm, p->benchY + 0.04f, zp, 1);
    pm_pipe(pm, &pa, PIPE_R, 0.035f, 0);
    pm_path_free(&pa);
    /* the hose tail under the start, brass */
    pm_mat(pm, PMAT_BRASS);
    pm_push(pm);
    pm_translate(pm, L->pipeX0, y - 0.07f, zp);
    PmProf t[6] = { { 0, -0.035f, 0 }, { 0.0045f, -0.035f, 0 }, { 0.006f, -0.027f, 0 }, { 0.0045f, -0.022f, 0 }, { 0.0125f, -0.004f, 1 }, { 0, 0.002f, 0 } };
    pm_lathe(pm, t, 6, 12, 0.0f, 0.0f);
    pm_pop(pm);
    /* the hose from the regulator's barb (pmodel_kit.c m_regulator: 0.065 along, 0.048 down from the outlet) */
    const float toward = L->pipeX1 > L->pipeX0 ? 1.0f : -1.0f;
    const float a[3] = { p->gasX + toward * (0.04f + 0.065f), 1.3f - 0.12f + 0.025f - 0.048f, zl + 0.24f };
    const float b[3] = { L->pipeX0, y - 0.105f, zp };
    pm_cable(pm, a, b, 0.07f, 0.0065f);
}

/* The power's cables (NB5): a bundle from the panel's insulators up the lining to the hood; to the fan along
 * the wall; down from a gland to the socket strip; the strip's cords to the rice cooker and the bain-marie. */
static void power_cables(Pm *pm, const NbPlan *p, const NbLine *L) {
    const float zl = p->lining, px = L->panelX;
    float py = 1.6f;
    for (int i = 0; i < L->n; ++i) if (L->item[i].module == PMK_POWER_PANEL) py = L->item[i].y;
    pm->part = 139;
    const uint32_t s = hash_combine(p->seed, 0xCAB1u);
    /* up from the insulators (0.04 over the panel's top: pmodel_kit.c m_power_panel) */
    {
        const float a[3] = { px, py + 0.72f, zl + 0.06f };
        const float top = nb_ceil(p, px, zl + 0.06f) + 0.005f;
        const float b[3] = { px, top, zl + 0.06f };
        if (top > a[1] + 0.05f) pm_cable_bundle(pm, a, b, 0.0f, 3, 0.007f, s);
    }
    if (L->fan[1] > 0.0f) {
        /* to the fan: along the lining to the corner, then the side wall to the housing */
        const float xw = L->fan[0], sx = xw > px ? 1.0f : -1.0f, y = py + 0.68f;
        const float a[3] = { px + sx * 0.15f, y, zl + 0.06f }, c[3] = { xw - sx * 0.03f, y + 0.02f, zl + 0.06f };
        const float d[3] = { xw - sx * 0.03f, L->fan[1] + 0.05f, L->fan[2] - 0.2f };
        pm_cable_bundle(pm, a, c, 0.05f, 2, 0.006f, s + 1u);
        pm_cable_bundle(pm, c, d, 0.07f, 2, 0.006f, s + 2u);
    }
    /* down from a gland to the socket strip's end */
    {
        const float sx = L->stripX > px ? 1.0f : -1.0f;
        const float a[3] = { px + sx * 0.12f, py - 0.03f, zl + 0.07f }, b[3] = { L->stripX - sx * 0.152f, 1.03f, zl + 0.02f };
        pm_cable(pm, a, b, 0.03f, 0.006f);
    }
    /* cords from the strip's sockets to the rice cooker's and the bain-marie's backs */
    if (L->cookerX != 0.0f) {
        const float a[3] = { L->stripX - 0.02f, 1.02f, zl + 0.04f }, b[3] = { L->cookerX, 1.0f, L->cookerZ - 0.175f };
        pm_cable(pm, a, b, 0.05f, 0.004f);
    }
    if (L->bmPans) {
        const float a[3] = { L->stripX + 0.05f, 1.02f, zl + 0.04f }, b[3] = { L->rangeEnd < 0 ? L->bm1 - 0.08f : L->bm0 + 0.08f, 1.0f, L->bmZ0 };
        pm_cable(pm, a, b, 0.06f, 0.004f);
    }
}

static void kitchen(Pm *pm, const NbPlan *p, const NbLine *L) {
    const float zl = p->lining, by = p->benchY, bd = p->benchD;
    pm->part = 120;
    /* chequer plate over the kitchen's floor */
    pm_mat(pm, PMAT_CHEQUER);
    bb(pm, p->kx0, 0.0f, zl, p->kx1, 0.012f, p->kz1 + 0.04f, 0.002f);
    /* the back bench beside the range: a worn steel top with its upstand and apron, an undershelf, square legs */
    pm_mat(pm, PMAT_STAINLESS);
    float b0, b1;
    bench_span(p, L, &b0, &b1);
    if (b1 - b0 > 0.3f) {
        pm->part = 121;
        bb(pm, b0, by - 0.035f, zl, b1, by, zl + bd, 0.004f);
        top_sheet(pm, b0 + 0.004f, zl + 0.004f, b1 - 0.004f, zl + bd - 0.004f, by + 0.001f);   /* the line's contact */
        bb(pm, b0, by, zl, b1, by + 0.08f, zl + 0.012f, 0.002f);
        bb(pm, b0, by - 0.11f, zl + bd - 0.016f, b1, by - 0.035f, zl + bd, 0.002f);
        bb(pm, b0 + 0.03f, 0.16f, zl + 0.03f, b1 - 0.03f, 0.18f, zl + bd - 0.03f, 0.002f);
        float lx[8];
        const int nl = bench_legs(b0, b1, lx, 8);
        for (int i = 0; i < nl; ++i)
            for (int z = 0; z < 2; ++z) {
                float zz = z ? zl + bd - 0.035f : zl + 0.035f;
                bb(pm, lx[i] - 0.02f, 0.012f, zz - 0.02f, lx[i] + 0.02f, by - 0.035f, zz + 0.02f, 0.003f);
            }
    }
    range_carcass(pm, p, L);
    bain_marie(pm, p, L);
    gas_line(pm, p, L);
    power_cables(pm, p, L);
    pm_mat(pm, PMAT_STAINLESS);
    /* the splashback behind the range: small tiles on a backer, steel trims */
    {
        float x0 = maxf(p->kx0, p->rangeX0 - 0.15f), x1 = minf(p->kx1, p->rangeX1 + 0.15f), y0 = 1.04f, y1 = 1.85f;
        pm->part = 122;
        pm_mat(pm, PMAT_TILE);
        pm_box(pm, x0, y0, zl, x1, y1, zl + 0.01f);
        pm_mat(pm, PMAT_STAINLESS);
        bb(pm, x0 - 0.012f, y1, zl, x1 + 0.012f, y1 + 0.015f, zl + 0.02f, 0.002f);
        bb(pm, x0 - 0.012f, y0, zl, x0, y1, zl + 0.02f, 0.002f);
        bb(pm, x1, y0, zl, x1 + 0.012f, y1, zl + 0.02f, 0.002f);
    }
    /* shelves at 1.45 and 1.80 m on their brackets */
    if (p->shelfX1 > p->shelfX0) {
        pm->part = 123;
        for (int k = 0; k < 2; ++k) {
            float y = k ? 1.80f : 1.45f;
            pm_mat(pm, PMAT_STAINLESS);
            bb(pm, p->shelfX0, y - 0.02f, zl, p->shelfX1, y, zl + 0.28f, 0.003f);
            bb(pm, p->shelfX0, y, zl + 0.268f, p->shelfX1, y + 0.025f, zl + 0.28f, 0.0015f);
            put(pm, p->shelfX0 + 0.05f, y - 0.02f, zl, 0.0f, PMK_SHELF_BRACKET);
            put(pm, p->shelfX1 - 0.05f, y - 0.02f, zl, 0.0f, PMK_SHELF_BRACKET);
        }
    }
    /* the extractor canopy over the range, baffle filters under it */
    {
        const float e0 = p->rangeX0 - 0.1f, e1 = p->rangeX1 + 0.1f;
        pm->part = 124;
        pm_mat(pm, PMAT_STAINLESS);
        float poly[5][2] = { { 0.0f, 1.97f }, { 0.60f, 1.97f }, { 0.60f, 2.08f }, { 0.22f, 2.40f }, { 0.0f, 2.40f } };
        float o[3] = { e1, 0.0f, zl + 0.004f }, ex[3] = { 0, 0, 1 }, ey[3] = { 0, 1, 0 }, ez[3] = { -1, 0, 0 };
        pm_push(pm);
        pm_frame(pm, o, ex, ey, ez);
        pm_extrude(pm, (const float (*)[2])poly, 5, 0.0f, e1 - e0);
        pm_pop(pm);
        float g[3] = { e0 + 0.03f, 1.963f, zl + 0.05f }, gx[3] = { 1, 0, 0 }, gy[3] = { 0, 0, 1 }, gz[3] = { 0, -1, 0 };
        pm_push(pm);
        pm_frame(pm, g, gx, gy, gz);
        pm_translate(pm, 0.5f * (e1 - e0 - 0.06f), 0.25f, 0.0f);
        pm_grille_bars(pm, e1 - e0 - 0.06f, 0.5f, 0.006f, 0.035f);
        pm_pop(pm);
        /* its lamp under the baffles (NB6) */
        if (p->lamp[1] > 0.0f) put(pm, p->lamp[0], p->lamp[1], p->lamp[2], 0.0f, PMK_LAMP);
    }
    /* the warm batten under the lower shelf (NB6): an aluminium channel, its opal diffuser */
    if (p->batten[1] > p->batten[0]) {
        const float x0 = p->batten[0], x1 = p->batten[1], y = p->batten[2], z = p->batten[3];
        pm->part = 126;
        pm_mat(pm, PMAT_STEEL);
        pm_box(pm, x0, y + 0.007f, z - 0.017f, x1, 1.43f, z + 0.017f);
        pm_mat(pm, PMAT_BULB);
        pm_box(pm, x0 + 0.01f, y, z - 0.012f, x1 - 0.01f, y + 0.007f, z + 0.012f);
    }
    /* the duct up the lining and through the roof: flanged joints, a band to the wall, the flashing, the cap */
    {
        const float dx = p->ductX, dz = p->ductZ, r = p->ductR;
        int under_deck = p->deck[2] > p->deck[0] && dx > p->deck[0] && dx < p->deck[2] && dz < p->deck[3];
        float yh = under_deck ? p->hoodTop : nb_hood_under(p, dz - p->hood.spine[0][1]);
        if (p->form == NB_CORNER && !under_deck) yh = nb_hood_under(p, fabsf(dx - p->hood.spine[1][0]));
        pm->part = 125;
        pm_mat(pm, PMAT_GALV);
        PmPath pa;
        pm_path_init(&pa, 2);
        pm_path_add(&pa, dx, 2.40f, dz, 0); pm_path_add(&pa, dx, yh - 0.25f, dz, 0);
        pm_pipe(pm, &pa, r, 0.2f, 0);
        duct_flange(pm, dx, 2.405f, dz, r);
        duct_flange(pm, dx, yh - 0.25f, dz, r);
        pa.n = 0;
        pm_path_add(&pa, dx, yh - 0.25f, dz, 0); pm_path_add(&pa, dx, p->ductTop, dz, 0);
        pm_pipe(pm, &pa, r, 0.2f, 0);
        pm_path_free(&pa);
        float ym = 0.5f * (2.40f + yh - 0.25f);
        pm_push(pm); pm_translate(pm, dx, ym, dz); pm_torus(pm, r + 0.005f, 0.006f, 24, 6); pm_pop(pm);
        float c0[3] = { dx, ym, dz - r - 0.005f }, c1[3] = { dx, ym, zl + 0.004f };
        pm_beam(pm, c0, c1, 0.04f, 0.005f, 0.0015f, NULL);
        pm_push(pm);
        pm_translate(pm, dx, yh, dz);
        PmProf fl[5] = { { r + 0.004f, -0.004f, 1 }, { r + 0.07f, -0.004f, 1 }, { r + 0.07f, 0.006f, 1 }, { r + 0.012f, 0.11f, 1 }, { r + 0.004f, 0.11f, 1 } };
        pm_lathe(pm, fl, 5, 24, 0.0f, 0.0f);
        pm_pop(pm);
        float t = p->ductTop;
        PmProf cap[4] = { { 0.0f, 0.17f, 0 }, { 0.21f, 0.095f, 1 }, { 0.215f, 0.105f, 1 }, { 0.0f, 0.265f, 0 } };
        pm_push(pm); pm_translate(pm, dx, t, dz); pm_lathe(pm, cap, 4, 24, 0.0f, 0.0f); pm_pop(pm);
        pm_mat(pm, PMAT_STEEL);
        for (int k = 0; k < 3; ++k) {
            float a = 2.0943951f * (float)k + 0.5f;
            float l0[3] = { dx + cosf(a) * (r + 0.004f), t - 0.04f, dz + sinf(a) * (r + 0.004f) };
            float l1[3] = { dx + cosf(a) * (r + 0.004f), t + 0.14f, dz + sinf(a) * (r + 0.004f) };
            pm_beam(pm, l0, l1, 0.025f, 0.004f, 0.0f, NULL);
        }
    }
}

/* --------------------------------------------------------- hood fittings --- */

static void hood_fittings(Pm *pm, const NbPlan *p) {
    /* the tube channels behind the fascia, on brackets to it */
    for (int i = 0; i < p->ntube; ++i) {
        const float *t = p->tube[i];
        float dx = t[3] - t[0], dz = t[5] - t[2], L = sqrtf(dx * dx + dz * dz);
        if (L < 0.1f) continue;
        pm->part = (uint8_t)(130 + i);
        pm_push(pm);
        frame_on(pm, 0.5f * (t[0] + t[3]), t[1], 0.5f * (t[2] + t[5]), dx / L, dz / L);
        pm_mat(pm, PMAT_STEEL);
        bb(pm, -0.5f * L - 0.04f, 0.018f, -0.035f, 0.5f * L + 0.04f, 0.045f, 0.035f, 0.002f);
        for (int e = -1; e <= 1; e += 2) {
            float x = (float)e * 0.4f * L;
            bb(pm, x - 0.012f, 0.03f, 0.035f, x + 0.012f, 0.036f, 0.11f - 0.002f, 0.0f);
        }
        pm_mat(pm, PMAT_TUBE);   /* lit (NB6) */
        cyl_x(pm, -0.5f * L, 0.5f * L, 0.0f, 0.0f, 0.013f, 12);
        pm_mat(pm, PMAT_PLASTIC);
        cyl_x(pm, -0.5f * L - 0.035f, -0.5f * L, 0.0f, 0.0f, 0.017f, 10);
        cyl_x(pm, 0.5f * L, 0.5f * L + 0.035f, 0.0f, 0.0f, 0.017f, 10);
        for (int e = -1; e <= 1; e += 2) bb(pm, (float)e * (0.5f * L + 0.02f) - 0.012f, 0.0f, -0.012f, (float)e * (0.5f * L + 0.02f) + 0.012f, 0.02f, 0.012f, 0.0f);
        pm_pop(pm);
    }
    /* the icon housings on the fascia: an open steel tray, blacked inside, its neon icon (NB6) on legs in it */
    for (int i = 0; i < p->nicon; ++i) {
        const float *c = p->icon[i], s = c[5], e = 0.5f * s, r = 0.022f;
        pm->part = (uint8_t)(140 + i);
        pm_push(pm);
        frame_on(pm, c[0], c[1], c[2], c[4], -c[3]);   /* along: the fascia (cross(up, out)); out: its facing */
        pm_mat(pm, PMAT_STEEL);
        bb(pm, -e, -e, -0.002f, e, e, 0.008f, 0.004f);
        pm_box(pm, -e, -e, 0.008f, e, -e + r, 0.055f);   /* the rim: plain (the back's bevel reads at its edge) */
        pm_box(pm, -e, e - r, 0.008f, e, e, 0.055f);
        pm_box(pm, -e, -e + r, 0.008f, -e + r, e - r, 0.055f);
        pm_box(pm, e - r, -e + r, 0.008f, e, e - r, 0.055f);
        pm_mat(pm, PMAT_PLASTIC);
        pm_box(pm, -e + r, -e + r, 0.008f, e - r, e - r, 0.010f);
        {
            const float k = (s - 0.07f) / PM_ICON_SIZE;
            pm_push(pm);
            pm_translate(pm, 0.0f, 0.0f, 0.040f);
            pm_scale(pm, k, k, k);
            pm_place(pm, p->iconKind[i] ? p->iconKind[i] : PMK_ICON_BOWL);
            pm_pop(pm);
        }
        for (int k = 0; k < 4; ++k) {
            pm_push(pm);
            pm_translate(pm, (k & 1) ? 0.5f * s - 0.011f : -0.5f * s + 0.011f, (k & 2) ? 0.5f * s - 0.011f : -0.5f * s + 0.011f, 0.055f);
            pm_rotate_x(pm, HALF_PI);
            pm_place(pm, PMK_BOLT);
            pm_pop(pm);
        }
        pm_pop(pm);
    }
}

/* the corner form's column: riveted plate on a steel plinth, a cap under the hood's hip */
static void column(Pm *pm, const NbPlan *p) {
    const float x = p->column[0], z = p->column[1], h = p->column[2], top = p->column[3];
    if (h <= 0.0f) return;
    pm->part = 150;
    pm_mat(pm, PMAT_STEEL);
    bb(pm, x - h - 0.02f, 0.0f, z - h - 0.02f, x + h + 0.02f, 0.16f, z + h + 0.02f, 0.006f);
    bb(pm, x - h - 0.03f, top - 0.04f, z - h - 0.03f, x + h + 0.03f, top, z + h + 0.03f, 0.004f);
    pm_mat(pm, PMAT_PLATE);
    bb(pm, x - h, 0.16f, z - h, x + h, top - 0.04f, z + h, 0.008f);
    for (int f = 0; f < 4; ++f) {
        float nx = f == 0 ? 1.0f : f == 1 ? -1.0f : 0.0f, nz = f == 2 ? 1.0f : f == 3 ? -1.0f : 0.0f;
        for (int e = -1; e <= 1; e += 2) {
            float ax = x + nx * h + (nz != 0.0f ? (float)e * (h - 0.035f) : 0.0f);
            float az = z + nz * h + (nx != 0.0f ? (float)e * (h - 0.035f) : 0.0f);
            rivets(pm, ax, 0.22f, az, ax, top - 0.1f, az, nx, 0.0f, nz);
        }
    }
}

/* the corner form's deck: lapped sheet over the kitchen behind the spine */
static void deck(Pm *pm, const NbPlan *p) {
    if (p->deck[2] <= p->deck[0]) return;
    pm->part = 160;
    pm_mat(pm, PMAT_RUST);   /* the hood's metal, whichever it is */
    float o[3] = { p->deck[0], p->hoodTop, p->deck[1] }, ex[3] = { 0, 0, 1 }, ey[3] = { 1, 0, 0 }, ez[3] = { 0, 1, 0 };
    pm_push(pm);
    pm_frame(pm, o, ex, ey, ez);
    pm_sheet_wall(pm, p->deck[3] - p->deck[1], p->deck[2] - p->deck[0], 1.1f, THICK, 1);
    pm_pop(pm);
}

void nb_body(Pm *pm, const NbPlan *p) {
    /* long straight runs split at 0.6 m (not the modules' 0.12 m): enough for the baked occlusion along a
     * counter or a bench, and a sixth of the triangles */
    const float maxlen = pm->maxlen, support = pm->support;
    if (pm->maxlen > 0.0f) pm->maxlen = 0.6f;
    pm->support = 0.0f;   /* the chamfers keep the wear at the edges (the modules keep their loops) */
    pm->part = 0;
    pm_hood(pm, &p->hood);
    hood_fittings(pm, p);
    deck(pm, p);
    for (int i = 0; i < p->nwall; ++i) wall(pm, p, &p->wall[i], i, i != 2 && i != 4);
    for (int i = 0; i < p->npost; ++i) { pm->part = (uint8_t)(100 + i); post(pm, p->post[i][0], p->post[i][1], p->post[i][2]); }
    column(pm, p);
    for (int i = 0; i < p->nrun; ++i) counter_run(pm, p, &p->run[i], i);
    NbLine line;
    nb_line(p, &line);
    kitchen(pm, p, &line);
    /* the stools, the bell and the A-frame */
    for (int i = 0; i < p->nstool; ++i) { pm->part = (uint8_t)(170 + i); put(pm, p->stool[i][0], 0.0f, p->stool[i][1], 0.0f, PMK_STOOL); }
    pm->part = 190;
    put(pm, p->bell[0], p->bell[1], p->bell[2], 0.0f, PMK_BELL);
    pm->part = 191;
    put(pm, p->aframe[0], 0.0f, p->aframe[1], p->aframe[2], PMK_A_FRAME);
    pm->maxlen = maxlen; pm->support = support;
}

/* ------------------------------------------------------------ far shell --- */

void nb_shell(Pm *pm, const NbPlan *p) {
    const PmHood *h = &p->hood;
    pm->part = 0;
    /* the hood: each leg one bay, four steps round the arc, the sheet's top and underside */
    pm_mat(pm, PMAT_RUST);
    float s0 = 0.0f;
    for (int j = 0; j + 1 < h->nspine; ++j) {
        float a[2], b[2], o[2], L;
        if (!hood_leg(p, j, a, b, o, &L)) continue;
        float P[2][5][3];
        for (int e = 0; e < 2; ++e)
            for (int i = 0; i < 5; ++i) pm_hood_point(h, s0 + (e ? L : 0.0f) - (e ? 1e-4f : -1e-4f), (float)i / 4.0f, P[e][i]);
        for (int side = 0; side < 2; ++side) {
            pm->shell++;
            uint32_t base = pm->out->nv;
            for (int e = 0; e < 2; ++e)
                for (int i = 0; i < 5; ++i) {
                    int i0 = i > 0 ? i - 1 : 0, i1 = i < 4 ? i + 1 : 4;
                    float dt[3], ds[3], n[3];
                    for (int k = 0; k < 3; ++k) { dt[k] = P[e][i1][k] - P[e][i0][k]; ds[k] = P[1][i][k] - P[0][i][k]; }
                    n[0] = dt[1] * ds[2] - dt[2] * ds[1]; n[1] = dt[2] * ds[0] - dt[0] * ds[2]; n[2] = dt[0] * ds[1] - dt[1] * ds[0];
                    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (nl < 1e-9f) { n[0] = 0; n[1] = 1; n[2] = 0; } else { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
                    float out[3] = { P[e][i][0] - (e ? b[0] : a[0]), P[e][i][1], P[e][i][2] - (e ? b[1] : a[1]) };
                    if (n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0.0f) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                    /* the top on the arc, the underside 6 mm under it, facing down */
                    float q[3] = { P[e][i][0], P[e][i][1], P[e][i][2] };
                    if (side) { q[0] -= n[0] * 0.006f; q[1] -= n[1] * 0.006f; q[2] -= n[2] * 0.006f; n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
                    pm_vert(pm, q, n, (e ? L : 0.0f), (float)i * 0.25f * h->span);
                }
            for (int i = 0; i < 4; ++i) pm_quad(pm, base + (uint32_t)i, base + (uint32_t)i + 1, base + 6 + (uint32_t)i, base + 5 + (uint32_t)i);
        }
        s0 += L;
    }
    /* the fascia along the lip, mitred: the trim's coat */
    pm_mat(pm, PMAT_PAINTED);
    {
        PmPath pa;
        pm_path_init(&pa, 4);
        float sacc = 0.0f;
        for (int j = 0; j < h->nspine; ++j) {
            float lp[3];
            pm_hood_point(h, sacc, 1.0f, lp);
            float sx = h->spine[j][0], sz = h->spine[j][1], f = p->fascia / h->span;
            pm_path_add(&pa, sx + (lp[0] - sx) * f, h->spring - 0.5f * h->fascia_h, sz + (lp[2] - sz) * f, 1);
            if (j + 1 < h->nspine) {
                float dx = h->spine[j + 1][0] - sx, dz = h->spine[j + 1][1] - sz;
                sacc += sqrtf(dx * dx + dz * dz);
            }
        }
        PmSect fs;
        pm_sect_rect(&fs, 0.03f, h->fascia_h, 0.0f);
        float up[3] = { 0, 1, 0 };
        pm_sweep(pm, &pa, &fs, up, 3);
        pm_path_free(&pa);
    }
    /* the ends under the arc (as pm_hood's) */
    pm_mat(pm, PMAT_RUST);
    {
        float poly[8][2];
        int np = 0;
        for (int i = 0; i <= 4; ++i) {
            float t = (float)i / 4.0f, c = h->curl, an = t * c;
            poly[np][0] = h->span * sinf(an) / sinf(c);
            poly[np][1] = h->spring + h->rise * (cosf(an) - cosf(c)) / (1.0f - cosf(c));
            np++;
        }
        poly[np][0] = 0.0f; poly[np][1] = h->spring; np++;
        for (int e = 0; e < 2; ++e) {
            int i = e ? h->nspine - 1 : 0, j = e ? h->nspine - 2 : 1;
            float d[3] = { h->spine[j][0] - h->spine[i][0], 0.0f, h->spine[j][1] - h->spine[i][1] };
            float dl = sqrtf(d[0] * d[0] + d[2] * d[2]);
            if (dl < 1e-4f) continue;
            d[0] /= dl; d[2] /= dl;
            float ox[3] = { -d[2], 0.0f, d[0] };
            if (e) { ox[0] = d[2]; ox[2] = -d[0]; }
            float oy[3] = { 0, 1, 0 }, oz[3] = { ox[1] * oy[2] - ox[2] * oy[1], ox[2] * oy[0] - ox[0] * oy[2], ox[0] * oy[1] - ox[1] * oy[0] };
            float o[3] = { h->spine[i][0], 0.0f, h->spine[i][1] };
            pm_push(pm);
            pm_frame(pm, o, ox, oy, oz);
            float sg = oz[0] * d[0] + oz[2] * d[2] > 0.0f ? 1.0f : -1.0f;
            pm_extrude(pm, (const float (*)[2])poly, np, sg > 0.0f ? 0.0f : -0.01f, sg > 0.0f ? 0.01f : 0.0f);
            pm_pop(pm);
        }
    }
    /* walls, posts, the column, the deck, the counter, the bench, the extractor and duct, the icons, stools */
    pm_mat(pm, PMAT_PLATE);
    for (int i = 0; i < p->nwall; ++i) {
        const NbWall *w = &p->wall[i];
        if (w->len < 0.05f) continue;
        pm_push(pm);
        frame_on(pm, w->o[0], 0.0f, w->o[1], w->u[0], w->u[1]);
        pm_box(pm, 0.0f, 0.0f, -0.02f, w->len, w->h, 0.0f);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_RUST);
    if (p->deck[2] > p->deck[0]) pm_box(pm, p->deck[0], p->hoodTop, p->deck[1], p->deck[2], p->hoodTop + 0.01f, p->deck[3]);
    pm_mat(pm, PMAT_PAINTED);
    for (int i = 0; i < p->npost; ++i) pm_box(pm, p->post[i][0] - 0.03f, 0.0f, p->post[i][1] - 0.03f, p->post[i][0] + 0.03f, p->post[i][2], p->post[i][1] + 0.03f);
    if (p->column[2] > 0.0f) {
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, p->column[0] - p->column[2], 0.0f, p->column[1] - p->column[2], p->column[0] + p->column[2], p->column[3], p->column[1] + p->column[2]);
    }
    for (int i = 0; i < p->nrun; ++i) {
        const NbRun *r = &p->run[i];
        pm_push(pm);
        frame_on(pm, r->o[0], 0.0f, r->o[1], r->u[0], r->u[1]);
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, 0.0f, 0.0f, -p->cd + 0.05f, r->len, p->cy - 0.04f, 0.0f);
        pm_mat(pm, PMAT_STAINLESS);
        pm_box(pm, 0.0f, p->cy - 0.04f, -p->cd + 0.03f, r->len, p->cy, 0.03f);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_STAINLESS);
    if (p->benchX1 - p->benchX0 > 0.3f) pm_box(pm, p->benchX0, 0.0f, p->lining, p->benchX1, p->benchY, p->lining + p->benchD);
    {
        const float e0 = p->rangeX0 - 0.1f, e1 = p->rangeX1 + 0.1f;
        pm_box(pm, e0, 1.97f, p->lining, e1, 2.40f, p->lining + 0.5f);
        pm_mat(pm, PMAT_GALV);
        pm_push(pm); pm_translate(pm, p->ductX, 0.0f, p->ductZ); pm_cyl(pm, 2.40f, p->ductTop, p->ductR, 6); pm_pop(pm);
        PmProf cap[3] = { { 0.0f, 0.12f, 0 }, { 0.25f, 0.1f, 1 }, { 0.0f, 0.26f, 0 } };
        pm_push(pm); pm_translate(pm, p->ductX, p->ductTop, p->ductZ); pm_lathe(pm, cap, 3, 6, 0.0f, 0.0f); pm_pop(pm);
    }
    pm_mat(pm, PMAT_STEEL);
    for (int i = 0; i < p->nicon; ++i) {
        const float *c = p->icon[i], s = c[5];
        pm_push(pm);
        frame_on(pm, c[0], c[1], c[2], c[4], -c[3]);
        /* its face alone, glowing thinly in its neon's colour (vendor_emit_body: by its part): at range the
         * housing's sides are nothing */
        pm->shell++;
        pm->part = (uint8_t)(140 + i);
        pm_mat(pm, PMAT_SIGN_FAR);
        float n[3] = { 0, 0, 1 }, q[4][3] = { { -0.5f * s, -0.5f * s, 0.055f }, { 0.5f * s, -0.5f * s, 0.055f }, { 0.5f * s, 0.5f * s, 0.055f }, { -0.5f * s, 0.5f * s, 0.055f } };
        uint32_t id[4];
        for (int k = 0; k < 4; ++k) id[k] = pm_vert(pm, q[k], n, q[k][0], q[k][1]);
        pm_quad(pm, id[0], id[1], id[2], id[3]);
        pm_pop(pm);
    }
    pm->part = 0;
    pm_mat(pm, PMAT_PAINTED);
    for (int i = 0; i < p->nstool; ++i) {
        const float x = p->stool[i][0], z = p->stool[i][1];
        pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, 0.64f, 0.75f, 0.18f, 6); pm_pop(pm);
        pm_box(pm, x - 0.03f, 0.0f, z - 0.03f, x + 0.03f, 0.64f, z + 0.03f);
    }
}

/* ------------------------------------------------------ the cooking line --- */


float nb_ceil(const NbPlan *p, float x, float z) {
    const PmHood *h = &p->hood;
    float out;
    if (p->form == NB_CORNER) {
        const int s = p->side;
        const float zsp = p->lip - h->span, xsp = (s > 0 ? p->x1 : p->x0) - (float)s * h->span;
        out = maxf(0.0f, maxf((float)s * (x - xsp), z - zsp));
    } else {
        out = maxf(0.0f, z - h->spine[0][1]);
    }
    return nb_hood_under(p, out) - 0.01f;
}

/* the bench's legs (kitchen(): two rows, at its ends and every ~1.2 m) */
static int bench_legs(float b0, float b1, float *x, int max) {
    int nl = 2 + (int)((b1 - b0) / 1.2f);
    if (nl > max) nl = max;
    for (int i = 0; i < nl; ++i) x[i] = b0 + 0.03f + (b1 - b0 - 0.06f) * (float)i / (float)(nl - 1);
    return nl;
}
/* the bench's stretch beside the range's carcass */
static void bench_span(const NbPlan *p, const NbLine *L, float *b0, float *b1) {
    if (L->rangeEnd < 0) { *b0 = L->carc1; *b1 = p->benchX1; } else { *b0 = p->benchX0; *b1 = L->carc0; }
}
/* the side wall's inner face at the range's end (the utensil rail's), and which way it faces */
static float side_inner(const NbPlan *p, int end, float *facing) {
    *facing = end < 0 ? 1.0f : -1.0f;
    return end < 0 ? p->x0 + 0.0065f : p->x1 - 0.0065f;
}

static void add_item(NbLine *L, int module, int on, int used, float x, float y, float z, float yaw) {
    if (L->n >= NB_ITEMS_MAX) return;
    NbItem *it = &L->item[L->n++];
    it->module = (uint16_t)module; it->on = (uint8_t)on; it->used = (uint8_t)used;
    it->x = x; it->y = y; it->z = z; it->yaw = yaw;
}

/* a round thing's radius on the bench (its widest, handles and all) */
static float bench_r(int m) {
    switch (m) {
    case PMK_BOWL_STACK:  return 0.097f;
    case PMK_CHOP_BLOCK:  return 0.175f;
    case PMK_RICE_COOKER: return 0.197f;
    case PMK_STEAMER:     return 0.155f;
    case PMK_JAR_OIL:     return 0.05f;
    case PMK_JAR_GINGER:  return 0.065f;
    case PMK_BOTTLE:      return 0.033f;
    case PMK_GN_PAN:      return 0.09f;
    }
    return 0.1f;
}

void nb_line(const NbPlan *p, NbLine *L) {
    memset(L, 0, sizeof(*L));
    Rng r = rng_make(hash_combine(p->seed, 0x4C494Eu));
    /* every draw first, in order (hash.h) */
    const float kindR = rng_float(&r), swapR = rng_float(&r), pansR = rng_float(&r), openR = rng_float(&r);
    const float blockR = rng_float(&r), steamR = rng_float(&r), wokR = rng_float(&r), condR = rng_float(&r);
    const float zl = p->lining, bd = p->benchD, yb = p->benchY, zf = zl + bd;
    const int re = p->rangeX0 - p->benchX0 < p->benchX1 - p->rangeX1 ? -1 : 1;
    const float dir = (float)-re;   /* along the bench away from the range */
    L->rangeEnd = re;
    L->kind = kindR < 0.4f ? NB_POT_NOODLES : kindR < 0.65f ? NB_POT_WOK : kindR < 0.85f ? NB_POT_STEAMER : NB_POT_SAUCEPAN;
    if (re < 0) { L->carc0 = p->benchX0; L->carc1 = p->rangeX1; } else { L->carc0 = p->rangeX0; L->carc1 = p->benchX1; }

    /* the range: two wells on a 0.45 m pitch, the stock pot's at the wall end unless swapped */
    const float bz = zf - 0.30f;
    float bx[2] = { p->rangeX0 + 0.25f, p->rangeX0 + 0.70f };
    int wall0 = re < 0 ? 0 : 1;              /* the burner nearer the side wall */
    if (swapR < 0.25f) wall0 = 1 - wall0;
    L->burner[0][0] = bx[wall0];     L->burner[0][1] = bz;
    L->burner[1][0] = bx[1 - wall0]; L->burner[1][1] = bz;
    {
        const float x = L->burner[0][0], top = yb + GRATE_TOP;
        add_item(L, PMK_BURNER, NB_ON_RANGE, 1, x, RANGE_TRAY, bz, 0.0f);
        add_item(L, PMK_GRATE, NB_ON_RANGE, 1, x, yb, bz, 0.0f);
        add_item(L, PMK_STOCKPOT_L, NB_ON_RANGE, 1, x, top, bz, 0.0f);
    }
    {
        const float x = L->burner[1][0], top = yb + GRATE_TOP;
        if (L->kind == NB_POT_WOK) {
            add_item(L, PMK_WOK_BURNER, NB_ON_RANGE, 1, x, WOK_FLOOR, bz, 0.0f);
            /* the wok rests on the cradle's fingers (0.205 up, at 0.11 m out), its handle to the aisle */
            const float S = 0.26f, at = S - sqrtf(S * S - 0.11f * 0.11f);
            const float away = x > L->burner[0][0] ? 1.0f : -1.0f;   /* its handle swung away from the stock pot */
            add_item(L, PMK_WOK, NB_ON_RANGE, 1, x, WOK_FLOOR + 0.212f - at, bz, away * (0.1f + 0.35f * wokR));
        } else {
            add_item(L, PMK_BURNER, NB_ON_RANGE, 1, x, RANGE_TRAY, bz, 0.0f);
            add_item(L, PMK_GRATE, NB_ON_RANGE, 1, x, yb, bz, 0.0f);
            if (L->kind == NB_POT_SAUCEPAN) {
                add_item(L, PMK_SAUCEPAN, NB_ON_RANGE, 1, x, top, bz - 0.02f, 0.35f * dir);
            } else {
                add_item(L, PMK_STOCKPOT_S, NB_ON_RANGE, 1, x, top, bz, 0.0f);
                if (L->kind == NB_POT_STEAMER) add_item(L, PMK_STEAMER, NB_ON_RANGE, 1, x, top + 0.30f, bz, 0.0f);
                else for (int k = -1; k <= 1; k += 2) add_item(L, PMK_NOODLE_BASKET, NB_ON_RANGE, 1, x + 0.072f * (float)k, top + 0.312f, bz - 0.01f, 0.0f);
            }
        }
    }

    /* along the bench from the range: the bain-marie, bowls, the block, the rice cooker, a cold steamer */
    float b0, b1;
    bench_span(p, L, &b0, &b1);
    float cur = re < 0 ? b0 : b1;                 /* the free bench starts at the range */
    const float stop = re < 0 ? b1 - 0.04f : b0 + 0.04f;
    #define ROOM(w) (dir * (stop - (cur + dir * (w))) >= 0.0f)
    {
        int n = pansR < 0.5f ? 3 : 4;
        while (n >= 2 && !ROOM(0.06f + (float)n * 0.176f + 0.05f)) n--;
        if (n >= 2) {
            const float w = (float)n * 0.176f + 0.05f, a = cur + dir * 0.06f, b = a + dir * w;
            L->bm0 = minf(a, b); L->bm1 = maxf(a, b);
            L->bmZ0 = zl + 0.06f; L->bmZ1 = L->bmZ0 + BM_DEPTH;
            L->bmPans = n;
            const int open = (int)(openR * (float)n) % n;
            for (int k = 0; k < n; ++k)
                add_item(L, k == open ? PMK_GN_PAN_OPEN : PMK_GN_PAN, NB_ON_BENCH, 1,
                         L->bm0 + 0.025f + 0.088f + 0.176f * (float)k, BM_TOP - 0.15f, L->bmZ0 + 0.02f + 0.1625f, HALF_PI);
            cur = b;
        }
    }
    const float zc = zl + 0.5f * bd + 0.02f;
    static const int ORDER[4] = { PMK_BOWL_STACK, PMK_CHOP_BLOCK, PMK_RICE_COOKER, PMK_STEAMER };
    for (int i = 0; i < 4; ++i) {
        const int m = ORDER[i];
        if (m == PMK_CHOP_BLOCK && blockR < 0.25f) continue;
        if (m == PMK_STEAMER && (steamR < 0.4f || L->kind == NB_POT_STEAMER)) continue;
        const float rr = bench_r(m), gap = 0.05f;
        if (!ROOM(gap + 2.0f * rr)) continue;
        const float x = cur + dir * (gap + rr);
        float z = m == PMK_RICE_COOKER ? zl + 0.06f + rr : zc;
        add_item(L, m, NB_ON_BENCH, 1, x, yb, z, 0.0f);
        if (m == PMK_RICE_COOKER) { L->cookerX = x; L->cookerZ = z; }
        cur = x + dir * rr;
    }
    #undef ROOM

    /* the undershelf: spare bowls and lidded pans, clear of the legs */
    {
        float legs[8];
        const int nl = bench_legs(b0, b1, legs, 8);
        static const int SPARE[4] = { PMK_BOWL_STACK, PMK_GN_PAN, PMK_BOWL_STACK, PMK_GN_PAN };
        float x = re < 0 ? b0 + 0.06f : b1 - 0.06f;
        for (int i = 0; i < 4; ++i) {
            const int m = SPARE[i];
            const float rr = m == PMK_GN_PAN ? 0.09f : bench_r(m);
            float c = x + dir * rr;
            for (int k = 0; k < nl; ++k) if (fabsf(c - legs[k]) < rr + 0.03f) c = legs[k] + dir * (rr + 0.035f);
            if (dir * ((re < 0 ? b1 : b0) - dir * 0.05f - (c + dir * rr)) < 0.0f) break;
            add_item(L, m, NB_ON_UNDER, 0, c, 0.18f, zl + 0.5f * bd, m == PMK_GN_PAN ? HALF_PI : 0.0f);
            x = c + dir * (rr + 0.04f);
        }
    }

    /* the shelves' stock: bowls and jars low, bottles and jars high (a hash per place: no draws to keep in order) */
    if (p->shelfX1 > p->shelfX0) {
        for (int s = 0; s < 2; ++s) {
            const float y = s ? 1.80f : 1.45f;
            float x = p->shelfX0 + 0.09f;   /* clear of the brackets under the shelf above */
            for (int k = 0; k < 12; ++k) {
                const uint32_t h = hash_combine(p->seed, 0x5E1Fu + (uint32_t)(s * 16 + k));
                const float u = hash_float(h);
                /* the lower shelf starts with bowls (the assembly's spare), then bowls and jars; bottles high */
                const int m = s ? (u < 0.6f ? PMK_BOTTLE : u < 0.8f ? PMK_JAR_OIL : PMK_JAR_GINGER)
                                : (k == 0 || u < 0.4f ? PMK_BOWL_STACK : u < 0.7f ? PMK_JAR_OIL : PMK_JAR_GINGER);
                const float rr = bench_r(m), gap = 0.015f + 0.05f * hash_float(hash_combine(h, 1u));
                const float c = x + gap + rr;
                if (c + rr > p->shelfX1 - 0.09f) break;
                add_item(L, m, NB_ON_SHELF, 1, c, y, zl + 0.14f, 6.2831853f * hash_float(hash_combine(h, 2u)));
                x = c + rr;
            }
        }
    }

    /* the utensil rail on the side wall by the range, clear of the wall's framing */
    {
        float facing;
        const float xw = side_inner(p, re, &facing);
        float zc2 = zl + 0.42f;
        if (p->form == NB_CORNER) {
            const float zsp = p->lip - p->hood.span;
            if (zc2 + 0.5f * RAIL_LEN > zsp - 0.08f && zc2 - 0.5f * RAIL_LEN < zsp + 0.08f) zc2 = maxf(zl + 0.32f, zsp - 0.08f - 0.5f * RAIL_LEN);
        }
        L->rail[0] = xw; L->rail[1] = 1.60f; L->rail[2] = zc2; L->railFacing = facing;
    }

    /* the gas: the bottles at the bench's other end, the regulator on the big one's outlet towards the range */
    {
        const float xg = p->gasX, sg = p->gasX > p->rangeX1 ? 1.0f : -1.0f, toward = -sg;
        add_item(L, PMK_GAS_47, NB_ON_FLOOR, 0, xg, 0.0f, zl + 0.24f, toward > 0 ? 0.0f : PI_F);
        add_item(L, PMK_GAS_13, NB_ON_FLOOR, 0, xg + 0.04f * sg, 0.0f, zl + 0.66f, 0.0f);
        add_item(L, PMK_REGULATOR, NB_ON_FLOOR, 0, xg + toward * 0.04f, 1.3f - 0.12f + 0.025f, zl + 0.24f, toward > 0 ? 0.0f : PI_F);
        L->pipeY = 1.15f;
        L->pipeX0 = xg + toward * 0.22f;
        L->pipeX1 = re < 0 ? p->rangeX1 + 0.20f : p->rangeX0 - 0.20f;
        /* the power: the panel over the bottles, under the hood with room for its insulators */
        L->panelX = xg;
        add_item(L, PMK_POWER_PANEL, NB_ON_WALL, 1, xg, minf(1.6f, nb_ceil(p, xg, zl + 0.06f) - 0.75f), zl, 0.0f);
        /* the socket strip over the bench's end by the bottles */
        L->stripX = (re < 0 ? b1 : b0) - dir * 0.24f;
        add_item(L, PMK_POWER_STRIP, NB_ON_WALL, 1, L->stripX, 1.0f, zl, 0.0f);
        /* the fan, high on the side wall at the bottles' end, over the aisle (straight form: the corner form
         * has its side counter there) */
        if (p->form == NB_STRAIGHT) {
            float facing;
            const float xw = side_inner(p, -re, &facing), fz = zf + 0.35f;
            const float fy = minf(2.25f, p->hood.spring - 0.05f - 0.225f);
            if (fy >= 2.17f && fz + 0.24f < p->kz1 - 0.05f) {
                L->fan[0] = xw; L->fan[1] = fy; L->fan[2] = fz; L->fan[3] = facing;
                add_item(L, PMK_FAN, NB_ON_WALL, 0, xw, fy, fz, facing > 0 ? HALF_PI : -HALF_PI);
            }
        }
    }

    /* on the counter: a set (chopsticks, chilli oil, a sauce bottle) before every other stool, clear of the
     * gate and the bell */
    for (int i = 0, set = condR < 0.5f ? 0 : 1; i < p->nstool; ++i) {
        const float sx = p->stool[i][0], sz = p->stool[i][1];
        for (int k = 0; k < p->nrun; ++k) {
            const NbRun *c = &p->run[k];
            const float ox = -c->u[1], oz = c->u[0];   /* out to the customers */
            const float t = (sx - c->o[0]) * c->u[0] + (sz - c->o[1]) * c->u[1];
            const float off = (sx - c->o[0]) * ox + (sz - c->o[1]) * oz;
            if (off < 0.0f || off > 0.7f || t < 0.25f || t > c->len - 0.25f) continue;
            if (set++ & 1) break;
            const float g0 = c->gate0 - 0.2f, g1 = c->gate1 + 0.2f;
            if (c->gate1 > c->gate0 && t > g0 && t < g1) break;
            const float bt = (p->bell[0] - c->o[0]) * c->u[0] + (p->bell[2] - c->o[1]) * c->u[1];
            if (fabsf(bt - t) < 0.3f) break;
            static const int SET[3] = { PMK_CHOPSTICKS, PMK_JAR_OIL, PMK_BOTTLE };
            static const float AT[3] = { -0.11f, 0.02f, 0.12f };
            for (int j = 0; j < 3; ++j) {
                const float tt = t + AT[j], d = -0.14f;
                add_item(L, SET[j], NB_ON_COUNTER, 0, c->o[0] + c->u[0] * tt + ox * d, p->cy, c->o[1] + c->u[1] * tt + oz * d,
                         6.2831853f * hash_float(hash_combine(p->seed, 0xC0Du + (uint32_t)(i * 4 + j))));
            }
            break;
        }
    }
}

static void put_knob(Pm *pm, float x, float y, float z) {
    pm_push(pm);
    pm_translate(pm, x, y, z);
    pm_rotate_x(pm, HALF_PI);   /* its face up +y turned out +z */
    pm_place(pm, PMK_KNOB);
    pm_pop(pm);
}

void nb_line_place(Pm *pm, const NbPlan *p, const NbLine *L) {
    const float zl = p->lining, zf = zl + p->benchD;
    int part = 200;
    for (int i = 0; i < L->n; ++i) {
        const NbItem *it = &L->item[i];
        pm->part = (uint8_t)part++;
        put(pm, it->x, it->y, it->z, it->yaw, it->module);
    }
    /* the range's knobs on its rail, one by each burner and one for the pilot */
    pm->part = (uint8_t)part++;
    for (int b = 0; b < 2; ++b) put_knob(pm, L->burner[b][0], 0.80f, zf + 0.03f);
    put_knob(pm, L->rangeEnd < 0 ? L->carc0 + 0.075f : L->carc1 - 0.075f, 0.80f, zf + 0.03f);
    /* both burners alight (NB6): a ring burner's flames on its crown, the wok burner's jets roaring */
    pm->part = (uint8_t)part++;
    for (int b = 0; b < 2; ++b) {
        const int wok = b == 1 && L->kind == NB_POT_WOK;
        put(pm, L->burner[b][0], wok ? WOK_FLOOR : RANGE_TRAY, L->burner[b][1], 0.0f, wok ? PMK_FLAME_JET : PMK_FLAME_RING);
    }
    /* the bain-marie's dial */
    if (L->bmPans) put_knob(pm, L->bm0 + 0.07f, 0.985f, L->bmZ1);
    /* the cleaver: on the block if there is one, else on the rail */
    int block = -1;
    for (int i = 0; i < L->n; ++i) if (L->item[i].module == PMK_CHOP_BLOCK) block = i;
    if (block >= 0) {
        const NbItem *b = &L->item[block];
        pm_push(pm);
        pm_translate(pm, b->x - 0.1f, b->y + 0.0725f, b->z + 0.03f);
        pm_rotate_y(pm, 0.4f);
        pm_rotate_x(pm, -HALF_PI);   /* the blade (its xy plane) laid flat */
        pm_translate(pm, 0.0f, 0.06f, 0.0f);
        pm_place(pm, PMK_CLEAVER);
        pm_pop(pm);
    }
    /* the rail and what hangs from it */
    {
        const float xw = L->rail[0], y = L->rail[1], zc = L->rail[2], f = L->railFacing;
        pm->part = (uint8_t)part++;
        pm_push(pm);
        pm_translate(pm, xw, y, zc);
        pm_rotate_y(pm, f > 0.0f ? HALF_PI : -HALF_PI);   /* its wall (z 0) on the side wall, out into the kitchen */
        pm_place(pm, PMK_UTENSIL_RAIL);
        static const int HANG[6] = { PMK_LADLE, PMK_SPIDER, PMK_SPATULA, PMK_S_HOOK, PMK_CLEAVER, PMK_S_HOOK };
        static const float AT[6] = { -0.22f, -0.11f, 0.0f, 0.08f, 0.12f, 0.2f };
        for (int k = 0; k < 6; ++k) {
            if (HANG[k] == PMK_CLEAVER && block >= 0) continue;
            pm_push(pm);
            pm_translate(pm, AT[k], 0.02f, 0.045f);
            pm_place(pm, HANG[k]);
            pm_pop(pm);
        }
        pm_pop(pm);
    }
    /* the gas line's fittings: its valve and gauge by the bottles, saddle clips along it */
    {
        const float zp = zl + PIPE_R, y = L->pipeY, toward = L->pipeX1 > L->pipeX0 ? 1.0f : -1.0f;
        pm->part = (uint8_t)part++;
        put(pm, L->pipeX0 + toward * 0.09f, y, zp, 0.0f, PMK_VALVE_LEVER);
        put(pm, L->pipeX0 + toward * 0.20f, y + PIPE_R, zp, 0.0f, PMK_GAUGE);
        const float a = L->pipeX0 + toward * 0.32f, b = L->pipeX1 - toward * 0.06f;
        int ns = (int)ceilf(fabsf(b - a) / 0.5f);
        if (ns < 1) ns = 1;
        for (int i = 0; i <= ns; ++i) {
            pm_push(pm);
            pm_translate(pm, a + (b - a) * (float)i / (float)ns, y, zl);
            pm_rotate_x(pm, HALF_PI);   /* its surface (y 0) on the wall, out +z */
            pm_rotate_y(pm, HALF_PI);   /* its pipe along x */
            pm_place(pm, PMK_SADDLE);
            pm_pop(pm);
        }
        /* one on the drop */
        pm_push(pm);
        pm_translate(pm, L->pipeX1, 0.5f * (y + 1.0f) + 0.02f, zl);
        pm_rotate_x(pm, HALF_PI);
        pm_place(pm, PMK_SADDLE);
        pm_pop(pm);
    }
}

/* The line as occluders for the body's bake (NB6: contact darkening on the bench and the counter): its modules
 * built in at their coarse level, unbaked (only their shapes matter). */
void nb_line_occluders(const NbPlan *p, PmMesh *out) {
    out->nv = out->ni = 0;
    NbLine L;
    nb_line(p, &L);
    Pm pm;
    pm_begin(&pm, out, NULL, PM_LOD2);
    nb_line_place(&pm, p, &L);
}

/* ------------------------------------------------------------ the checks --- */

/* a module's bounds at its near level, and its reach round its axis in xz (computed once) */
static void module_bounds(int module, float lo[3], float hi[3], float *rad) {
    static float B[PMK_COUNT][7];
    static uint8_t done[PMK_COUNT];
    if (module < 0 || module >= PMK_COUNT) { for (int k = 0; k < 3; ++k) lo[k] = hi[k] = 0.0f; if (rad) *rad = 0.0f; return; }
    if (!done[module]) {
        PmMesh m = { 0 };
        Pm pm;
        pm_begin(&pm, &m, NULL, PM_LOD0);
        pm_module(module)->build(&pm);
        PmStats st;
        pm_stats(&m, &st);
        float r2 = 0.0f;
        for (uint32_t i = 0; i < m.nv; ++i) r2 = maxf(r2, m.v[i].p[0] * m.v[i].p[0] + m.v[i].p[2] * m.v[i].p[2]);
        for (int k = 0; k < 3; ++k) { B[module][k] = st.lo[k]; B[module][3 + k] = st.hi[k]; }
        B[module][6] = sqrtf(r2);
        pm_mesh_free(&m);
        done[module] = 1;
    }
    for (int k = 0; k < 3; ++k) { lo[k] = B[module][k]; hi[k] = B[module][3 + k]; }
    if (rad) *rad = B[module][6];
}
void nb_module_bounds(int module, float lo[3], float hi[3]) { module_bounds(module, lo, hi, NULL); }

/* an item's box in the site's frame: its module's bounds turned by its yaw, no wider than its reach round
 * its axis */
static void item_box(const NbItem *it, float lo[3], float hi[3]) {
    float a[3], b[3], rad;
    module_bounds(it->module, a, b, &rad);
    const float c = cosf(it->yaw), s = sinf(it->yaw);
    lo[0] = lo[2] = 1e9f; hi[0] = hi[2] = -1e9f;
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? b[0] : a[0], z = (k & 2) ? b[2] : a[2];
        const float wx = x * c + z * s, wz = -x * s + z * c;   /* pm_rotate_y */
        lo[0] = minf(lo[0], wx); hi[0] = maxf(hi[0], wx); lo[2] = minf(lo[2], wz); hi[2] = maxf(hi[2], wz);
    }
    lo[0] = maxf(lo[0], -rad); hi[0] = minf(hi[0], rad); lo[2] = maxf(lo[2], -rad); hi[2] = minf(hi[2], rad);
    lo[0] += it->x; hi[0] += it->x; lo[2] += it->z; hi[2] += it->z;
    lo[1] = it->y + a[1]; hi[1] = it->y + b[1];
}
/* An item's footprint for the checks: its turned box, or for a round body with a long handle (the wok, the
 * saucepan) a circle and the handle's box (their turned boxes' corners swing wide of the real thing). */
typedef struct NbShape { int round; float cx, cz, r, ax[2][2], h[2]; } NbShape;
static int item_shapes(const NbItem *it, NbShape *sh) {
    float lo[3], hi[3];
    module_bounds(it->module, lo, hi, NULL);
    const float c = cosf(it->yaw), s = sinf(it->yaw);
    float body = 0.0f, hx = 0.0f, hz0 = 0.0f, hz1 = 0.0f;
    if (it->module == PMK_WOK)      { body = 0.195f; hx = 0.02f; hz0 = 0.18f; hz1 = hi[2]; }
    if (it->module == PMK_SAUCEPAN) { body = 0.102f; hx = 0.014f; hz0 = 0.09f; hz1 = hi[2]; }
    int n = 0;
    if (body > 0.0f) {
        sh[n].round = 1; sh[n].cx = it->x; sh[n].cz = it->z; sh[n].r = body; n++;
        lo[0] = -hx; hi[0] = hx; lo[2] = hz0; hi[2] = hz1;
    }
    NbShape *b = &sh[n++];
    const float mx = 0.5f * (lo[0] + hi[0]), mz = 0.5f * (lo[2] + hi[2]);
    b->round = 0;
    b->cx = it->x + mx * c + mz * s; b->cz = it->z - mx * s + mz * c;
    b->ax[0][0] = c; b->ax[0][1] = -s;   /* local x (pm_rotate_y) */
    b->ax[1][0] = s; b->ax[1][1] = c;    /* local z */
    b->h[0] = 0.5f * (hi[0] - lo[0]); b->h[1] = 0.5f * (hi[2] - lo[2]);
    return n;
}
static int shapes_meet(const NbShape *a, const NbShape *b, float slack) {
    if (a->round && b->round) {
        const float dx = b->cx - a->cx, dz = b->cz - a->cz, rr = a->r + b->r - slack;
        return dx * dx + dz * dz < rr * rr;
    }
    if (a->round || b->round) {
        const NbShape *c = a->round ? a : b, *o = a->round ? b : a;
        /* the circle's middle in the box's frame, the nearest point of the box */
        const float dx = c->cx - o->cx, dz = c->cz - o->cz;
        float d2 = 0.0f;
        for (int k = 0; k < 2; ++k) {
            const float t = dx * o->ax[k][0] + dz * o->ax[k][1], e = fabsf(t) - o->h[k];
            if (e > 0.0f) d2 += e * e;
        }
        return d2 < (c->r - slack) * (c->r - slack) && c->r > slack;
    }
    for (int i = 0; i < 2; ++i)
        for (int k = 0; k < 2; ++k) {
            const NbShape *q = i ? b : a;
            const float nx = q->ax[k][0], nz = q->ax[k][1];
            const float d = fabsf((b->cx - a->cx) * nx + (b->cz - a->cz) * nz);
            float r = 0.0f;
            for (int e = 0; e < 2; ++e) r += a->h[e] * fabsf(a->ax[e][0] * nx + a->ax[e][1] * nz) + b->h[e] * fabsf(b->ax[e][0] * nx + b->ax[e][1] * nz);
            if (d >= r - slack) return 0;
        }
    return 1;
}
/* whether two items meet, by more than `slack`: their heights, their reaches round their axes, their shapes */
static int items_meet(const NbItem *A, const NbItem *Bi, float slack) {
    float alo[3], ahi[3], blo[3], bhi[3], ra, rb;
    module_bounds(A->module, alo, ahi, &ra);
    module_bounds(Bi->module, blo, bhi, &rb);
    if (A->y + ahi[1] - slack <= Bi->y + blo[1] || Bi->y + bhi[1] - slack <= A->y + alo[1]) return 0;
    const float dx = Bi->x - A->x, dz = Bi->z - A->z;
    if (dx * dx + dz * dz >= (ra + rb - slack) * (ra + rb - slack)) return 0;
    NbShape sa[2], sb[2];
    const int na = item_shapes(A, sa), nb = item_shapes(Bi, sb);
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j)
            if (shapes_meet(&sa[i], &sb[j], slack)) return 1;
    return 0;
}
static int boxes_meet(const float alo[3], const float ahi[3], const float blo[3], const float bhi[3], float gap) {
    for (int k = 0; k < 3; ++k) if (ahi[k] + gap <= blo[k] || bhi[k] + gap <= alo[k]) return 0;
    return 1;
}
/* things meant to stand in or on each other */
static int nested(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    static const int PAIRS[][2] = {
        { PMK_STOCKPOT_S, PMK_STEAMER }, { PMK_STOCKPOT_S, PMK_NOODLE_BASKET }, { PMK_STOCKPOT_L, PMK_GRATE },
        { PMK_STOCKPOT_S, PMK_GRATE }, { PMK_BURNER, PMK_GRATE }, { PMK_GRATE, PMK_SAUCEPAN }, { PMK_WOK_BURNER, PMK_WOK },
        { PMK_GAS_47, PMK_REGULATOR }, { PMK_NOODLE_BASKET, PMK_NOODLE_BASKET }, { PMK_GRATE, PMK_GRATE } };
    for (size_t i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); ++i) {
        int x = PAIRS[i][0], y = PAIRS[i][1];
        if (x > y) { int t = x; x = y; y = t; }
        if (a == x && b == y) return 1;
    }
    return 0;
}

#define ERR(...) do { if (err && used < cap) used += snprintf(err + used, (size_t)(cap - used), __VA_ARGS__); if (err && used < cap) err[used++] = '\n'; if (err && used >= cap) used = cap - 1; ++bad; } while (0)

int nb_line_check(const NbPlan *p, const NbLine *L, char *err, int cap) {
    int bad = 0, used = 0;
    if (err && cap > 0) err[0] = 0;
    const float zl = p->lining, zf = zl + p->benchD;
    /* burners on the pitch, and each pot on its burner */
    {
        const float pitch = fabsf(L->burner[0][0] - L->burner[1][0]);
        if (pitch < 0.449f) ERR("burners %.2f m apart (0.45 at least)", pitch);
    }
    /* the aisle: where the cook stands */
    const float ax0 = p->kx0, ax1 = p->kx1, az0 = zf, az1 = p->kz1;
    int gas = 0;
    for (int i = 0; i < L->n; ++i) {
        const NbItem *it = &L->item[i];
        const char *nm = pm_module(it->module)->name;
        float lo[3], hi[3];
        item_box(it, lo, hi);
        if (it->module == PMK_GAS_13 || it->module == PMK_GAS_47) {
            gas++;
            for (int b = 0; b < 2; ++b) {
                const float dx = it->x - L->burner[b][0], dz = it->z - L->burner[b][1];
                if (dx * dx + dz * dz < 0.5f * 0.5f) ERR("%s %.2f m from a burner (0.5 at least)", nm, sqrtf(dx * dx + dz * dz));
            }
        }
        /* inside the kitchen (the counter's things on the counter) */
        if (it->on == NB_ON_COUNTER) {
            if (fabsf(it->y - p->cy) > 0.001f) ERR("%s not on the counter's top", nm);
        } else if (lo[0] < p->kx0 - 0.06f || hi[0] > p->kx1 + 0.06f || lo[2] < zl - 0.005f || hi[2] > p->kz1) {
            ERR("%s outside the kitchen: x %.2f..%.2f z %.2f..%.2f (x %.2f..%.2f z %.2f..%.2f)", nm, lo[0], hi[0], lo[2], hi[2], p->kx0, p->kx1, zl, p->kz1);
        }
        /* the reach: what the cook uses within 0.7 m of the aisle, its foot no higher than 1.85 m */
        if (it->used && it->on != NB_ON_COUNTER) {
            const float cx = clampf(it->x, ax0, ax1), cz = clampf(it->z, az0, az1);
            const float d = sqrtf((it->x - cx) * (it->x - cx) + (it->z - cz) * (it->z - cz));
            if (d > 0.7f) ERR("%s %.2f m from the aisle (0.7 at most)", nm, d);
            if (it->y > 1.85f) ERR("%s at %.2f m (1.85 at most)", nm, it->y);
        }
        /* under the hood (the ceiling over each corner of its box) */
        {
            float c = 1e9f;
            for (int k = 0; k < 4; ++k) c = minf(c, nb_ceil(p, (k & 1) ? hi[0] : lo[0], (k & 2) ? hi[2] : lo[2]));
            if (hi[1] > c - 0.03f) ERR("%s's top %.2f m through the hood (%.2f over it)", nm, hi[1], c);
        }
        /* under the extractor (its lip at 1.97) and the shelves (their undersides at 1.43, 1.78), with room */
        {
            const float elo[3] = { p->rangeX0 - 0.1f, 1.97f, zl }, ehi[3] = { p->rangeX1 + 0.1f, 2.40f, zl + 0.6f };
            if (boxes_meet(lo, hi, elo, ehi, 0.0f)) ERR("%s in the extractor", nm);
            if (it->on == NB_ON_RANGE && hi[1] > 1.97f - 0.35f) ERR("%s on the range %.2f m under the extractor (0.35 at least)", nm, 1.97f - hi[1]);
            if (p->shelfX1 > p->shelfX0)
                for (int s = 0; s < 2; ++s) {
                    const float y = s ? 1.80f : 1.45f;
                    if (it->on == NB_ON_SHELF && fabsf(it->y - y) < 0.01f) continue;
                    /* the board (with 3 cm over what stands under it) and its two brackets */
                    const float slo[3] = { p->shelfX0, y - 0.05f, zl }, shi[3] = { p->shelfX1, y, zl + 0.28f };
                    if (boxes_meet(lo, hi, slo, shi, 0.0f)) ERR("%s under the shelf at %.2f m", nm, y);
                    /* each bracket's gusset (pmodel_kit.c m_shelf_bracket) in three steps: its flange down the wall,
                     * then deeper and shallower */
                    static const float G[3][2] = { { 0.035f, 0.25f }, { 0.15f, 0.15f }, { 0.28f, 0.05f } };
                    for (int e = 0; e < 2; ++e)
                        for (int g = 0; g < 3; ++g) {
                            const float bx = e ? p->shelfX1 - 0.05f : p->shelfX0 + 0.05f;
                            const float blo[3] = { bx - 0.02f, y - G[g][1], zl }, bhi[3] = { bx + 0.02f, y, zl + G[g][0] };
                            if (boxes_meet(lo, hi, blo, bhi, 0.0f)) { ERR("%s in a bracket of the shelf at %.2f m", nm, y); e = 2; break; }
                        }
                }
        }
        /* nothing in another's way */
        for (int j = i + 1; j < L->n; ++j) {
            const NbItem *o = &L->item[j];
            if (nested(it->module, o->module)) continue;
            if (items_meet(it, o, 0.002f)) ERR("%s and %s meet", nm, pm_module(o->module)->name);
        }
    }
    if (gas > 2) ERR("%d gas bottles (2 at most)", gas);
    if (L->fan[1] > 0.0f && L->fan[1] - 0.22f < 1.95f) ERR("the fan's foot at %.2f m over the aisle (1.95 at least)", L->fan[1] - 0.22f);
    return bad;
}
#undef ERR

int nb_at_range(const NbPlan *p, const float pos[3]) {
    return pos[0] > p->rangeX0 - 0.15f && pos[0] < p->rangeX1 + 0.15f && pos[2] < p->lining + p->benchD + 0.12f &&
           pos[1] > 0.5f && pos[1] < 2.6f;
}

uint32_t nb_finish(float affluence, uint32_t seed) {
    const uint32_t h = hash_combine(seed, 0xC0A7u);
    const uint32_t coat = h % NB_COATS;
    /* the poorer third rusty; above it plain steel about half the time, galvanised the rest (no copper: the
     * user's call, it tarnishes too readily for a hood) */
    const float a = affluence + 0.3f * (hash_float(hash_combine(h, 1u)) - 0.5f);
    const uint32_t hood = a < 0.38f ? NB_HOOD_RUST : hash_float(hash_combine(h, 2u)) < 0.5f ? NB_HOOD_STEEL : NB_HOOD_GALV;
    /* the A-frame: painted in the trim coat or left bare timber, about evenly */
    const uint32_t wood = hash_float(hash_combine(h, 3u)) < 0.5f ? NB_FINISH_BARE_WOOD : 0u;
    return coat << 5 | hood << 8 | wood;
}

int nb_solids(const NbPlan *p, float (*b)[4], int max) {
    int n = 0;
#define ADD(a, c, d, e) do { if (n < max) { b[n][0] = (a); b[n][1] = (c); b[n][2] = (d); b[n][3] = (e); n++; } } while (0)
    if (p->form == NB_CORNER) {
        const int s = p->side;
        float xo = s > 0 ? p->x0 : p->x1, xc = p->run[1].o[0] + (float)s * 0.03f;
        ADD(minf(xo, xc), 0.0f, maxf(xo, xc), p->front + 0.03f);
        if (p->column[2] > 0.0f) ADD(p->column[0] - p->column[2], p->column[1] - p->column[2], p->column[0] + p->column[2], p->column[1] + p->column[2]);
    } else {
        ADD(p->x0, 0.0f, p->x1, p->front + 0.03f);
    }
    for (int i = 0; i < p->npost; ++i) ADD(p->post[i][0] - 0.06f, p->post[i][1] - 0.06f, p->post[i][0] + 0.06f, p->post[i][1] + 0.06f);
    for (int i = 0; i < p->nstool; ++i) ADD(p->stool[i][0] - 0.2f, p->stool[i][1] - 0.2f, p->stool[i][0] + 0.2f, p->stool[i][1] + 0.2f);
    ADD(p->aframe[0] - 0.32f, p->aframe[1] - 0.22f, p->aframe[0] + 0.32f, p->aframe[1] + 0.22f);
#undef ADD
    return n;
}
