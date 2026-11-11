#include "noodle_bar.h"
#include "hash.h"
#include "mathlib.h"
#include <math.h>
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
    return fits;
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
        bb(pm, a - (a < 0.01f ? 0.02f : 0.0f), yt, -cd + 0.03f, b + (b > L - 0.01f ? 0.02f : 0.0f), cy, 0.03f, 0.006f);
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

static void kitchen(Pm *pm, const NbPlan *p) {
    const float zl = p->lining, by = p->benchY, bd = p->benchD;
    pm->part = 120;
    /* chequer plate over the kitchen's floor */
    pm_mat(pm, PMAT_CHEQUER);
    bb(pm, p->kx0, 0.0f, zl, p->kx1, 0.012f, p->kz1 + 0.04f, 0.002f);
    /* the back bench: a worn steel top with its upstand and apron, an undershelf, square legs */
    pm_mat(pm, PMAT_STAINLESS);
    const float b0 = p->benchX0, b1 = p->benchX1;
    if (b1 - b0 > 0.3f) {
        pm->part = 121;
        bb(pm, b0, by - 0.035f, zl, b1, by, zl + bd, 0.004f);
        bb(pm, b0, by, zl, b1, by + 0.08f, zl + 0.012f, 0.002f);
        bb(pm, b0, by - 0.11f, zl + bd - 0.016f, b1, by - 0.035f, zl + bd, 0.002f);
        bb(pm, b0 + 0.03f, 0.16f, zl + 0.03f, b1 - 0.03f, 0.18f, zl + bd - 0.03f, 0.002f);
        int nl = 2 + (int)((b1 - b0) / 1.2f);
        for (int i = 0; i < nl; ++i) {
            float x = b0 + 0.03f + (b1 - b0 - 0.06f) * (float)i / (float)(nl - 1);
            for (int z = 0; z < 2; ++z) {
                float zz = z ? zl + bd - 0.035f : zl + 0.035f;
                bb(pm, x - 0.02f, 0.012f, zz - 0.02f, x + 0.02f, by - 0.035f, zz + 0.02f, 0.003f);
            }
        }
    }
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
        pm_mat(pm, PMAT_GLASS);
        cyl_x(pm, -0.5f * L, 0.5f * L, 0.0f, 0.0f, 0.013f, 12);
        pm_mat(pm, PMAT_PLASTIC);
        cyl_x(pm, -0.5f * L - 0.035f, -0.5f * L, 0.0f, 0.0f, 0.017f, 10);
        cyl_x(pm, 0.5f * L, 0.5f * L + 0.035f, 0.0f, 0.0f, 0.017f, 10);
        for (int e = -1; e <= 1; e += 2) bb(pm, (float)e * (0.5f * L + 0.02f) - 0.012f, 0.0f, -0.012f, (float)e * (0.5f * L + 0.02f) + 0.012f, 0.02f, 0.012f, 0.0f);
        pm_pop(pm);
    }
    /* the icon housings on the fascia: a steel box, its face glass (NB6 lights it) */
    for (int i = 0; i < p->nicon; ++i) {
        const float *c = p->icon[i], s = c[5];
        pm->part = (uint8_t)(140 + i);
        pm_push(pm);
        frame_on(pm, c[0], c[1], c[2], c[4], -c[3]);   /* along: the fascia (cross(up, out)); out: its facing */
        pm_mat(pm, PMAT_STEEL);
        bb(pm, -0.5f * s, -0.5f * s, -0.002f, 0.5f * s, 0.5f * s, 0.055f, 0.006f);
        pm_mat(pm, PMAT_GLASS);
        pm_box(pm, -0.5f * s + 0.022f, -0.5f * s + 0.022f, 0.055f, 0.5f * s - 0.022f, 0.5f * s - 0.022f, 0.058f);
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
    kitchen(pm, p);
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
        /* its face alone: at range the housing's sides are nothing */
        pm->shell++;
        float n[3] = { 0, 0, 1 }, q[4][3] = { { -0.5f * s, -0.5f * s, 0.055f }, { 0.5f * s, -0.5f * s, 0.055f }, { 0.5f * s, 0.5f * s, 0.055f }, { -0.5f * s, 0.5f * s, 0.055f } };
        uint32_t id[4];
        for (int k = 0; k < 4; ++k) id[k] = pm_vert(pm, q[k], n, q[k][0], q[k][1]);
        pm_quad(pm, id[0], id[1], id[2], id[3]);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_PAINTED);
    for (int i = 0; i < p->nstool; ++i) {
        const float x = p->stool[i][0], z = p->stool[i][1];
        pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, 0.64f, 0.75f, 0.18f, 6); pm_pop(pm);
        pm_box(pm, x - 0.03f, 0.0f, z - 0.03f, x + 0.03f, 0.64f, z + 0.03f);
    }
}

/* ------------------------------------------------------- the stand-in line --- */

static void put_knob(Pm *pm, float x, float y, float z) {
    pm_push(pm);
    pm_translate(pm, x, y, z);
    pm_rotate_x(pm, HALF_PI);   /* its face up +y turned out +z */
    pm_place(pm, PMK_KNOB);
    pm_pop(pm);
}

void nb_standin(Pm *pm, const NbPlan *p) {
    const float zl = p->lining, yb = p->benchY, zb = zl + 0.55f * p->benchD, zf = zl + p->benchD;
    int part = 200;
    /* the range: two burners on a 0.45 m pitch with their grates, a large pot and a small lidded one; knobs */
    for (int i = 0; i < 2; ++i) {
        float x = p->rangeX0 + 0.25f + 0.45f * (float)i;
        pm->part = (uint8_t)part++;
        put(pm, x, yb, zb, 0.0f, PMK_BURNER);
        put(pm, x, yb, zb, 0.0f, PMK_GRATE);
        put(pm, x, yb + 0.047f, zb, 0.0f, i == 0 ? PMK_STOCKPOT_L : PMK_STOCKPOT_S);
        if (i == 1) put(pm, x, yb + 0.047f + 0.300f, zb, 0.0f, PMK_LID);
        for (int k = 0; k < 2; ++k) put_knob(pm, x - 0.06f + 0.12f * (float)k, yb - 0.07f, zf);
    }
    /* the rest of the bench: the bain-marie's pans (one open), the steamer and the rice cooker */
    const int left = p->rangeX0 - p->benchX0 < p->benchX1 - p->rangeX1;   /* the range at the bench's -x end */
    float r0 = left ? p->rangeX1 + 0.12f : p->benchX0 + 0.08f, r1 = left ? p->benchX1 - 0.08f : p->rangeX0 - 0.12f;
    float x = r0;
    for (int i = 0; i < 4 && x + 0.19f <= r1; ++i, x += 0.19f) {
        pm->part = (uint8_t)part++;
        put(pm, x + 0.095f, yb, zb, HALF_PI, i == 2 ? PMK_GN_PAN_OPEN : PMK_GN_PAN);
    }
    if (x + 0.36f <= r1) { pm->part = (uint8_t)part++; put(pm, x + 0.20f, yb, zb, 0.0f, PMK_STEAMER); x += 0.40f; }
    if (x + 0.40f <= r1) { pm->part = (uint8_t)part++; put(pm, x + 0.21f, yb, zb, 0.0f, PMK_RICE_COOKER); }
    /* the shelves' stock: bowls and jars low, bottles high */
    if (p->shelfX1 > p->shelfX0) {
        const float s0 = p->shelfX0, s1 = p->shelfX1;
        static const int LOW[] = { PMK_BOWL_STACK, PMK_JAR_OIL, PMK_JAR_GINGER, PMK_JAR_OIL, PMK_BOWL_STACK, PMK_JAR_GINGER, PMK_JAR_OIL };
        int nl = (int)((s1 - s0 - 0.2f) / 0.22f) + 1;
        nl = nl < 1 ? 1 : nl > 7 ? 7 : nl;
        for (int i = 0; i < nl; ++i) {
            pm->part = (uint8_t)part++;
            put(pm, s0 + 0.15f + (s1 - s0 - 0.3f) * (nl > 1 ? (float)i / (float)(nl - 1) : 0.5f), 1.45f, zl + 0.15f, 0.0f, LOW[i]);
        }
        int nb = nl > 4 ? 4 : nl;
        for (int i = 0; i < nb; ++i) {
            pm->part = (uint8_t)part++;
            put(pm, s0 + 0.2f + (s1 - s0 - 0.4f) * (nb > 1 ? (float)i / (float)(nb - 1) : 0.5f), 1.80f, zl + 0.14f, 0.0f, PMK_BOTTLE);
        }
    }
    /* the utensil rail over the range at 1.55 m, and what hangs from it */
    {
        float xr = 0.5f * (p->rangeX0 + p->rangeX1), yr = 1.55f, z0 = zl + 0.01f, zr = z0 + 0.045f;
        pm->part = (uint8_t)part++;
        put(pm, xr, yr, z0, 0.0f, PMK_UTENSIL_RAIL);
        put(pm, xr - 0.38f, yr + 0.02f, zr, 0.0f, PMK_LADLE);
        put(pm, xr - 0.20f, yr + 0.02f, zr, 0.0f, PMK_SPIDER);
        put(pm, xr - 0.02f, yr + 0.02f, zr, 0.0f, PMK_SPATULA);
        for (int i = 0; i < 4; ++i) put(pm, xr + 0.12f + 0.09f * (float)i, yr + 0.02f, zr, 0.0f, PMK_S_HOOK);
        put(pm, xr + 0.12f, yr + 0.02f - 0.063f, zr, 0.0f, PMK_CLEAVER);
    }
    /* the gas: bottles at the bench's far end, the power panel over them, the fan high up; the line along the
     * lining at 1.0 m from the bottles to the range: saddles, a valve and its gauge */
    {
        const float xg = p->gasX, sg = p->gasX > p->rangeX1 ? 1.0f : -1.0f;
        pm->part = (uint8_t)part++;
        put(pm, xg, 0.0f, zl + 0.24f, 0.0f, PMK_GAS_47);
        put(pm, xg + 0.04f * sg, 0.0f, zl + 0.66f, 0.0f, PMK_GAS_13);
        put(pm, xg, 1.6f, zl, 0.0f, PMK_POWER_PANEL);
        put(pm, xg, 2.25f, zl, 0.0f, PMK_FAN);
        float a = sg > 0.0f ? p->rangeX1 - 0.1f : xg + 0.25f, b = sg > 0.0f ? xg - 0.25f : p->rangeX0 + 0.1f;
        for (int i = 0; i < 6; ++i) {
            pm_push(pm);
            pm_translate(pm, a + (b - a) * (float)i / 5.0f, 1.0f, zl);
            pm_rotate_x(pm, HALF_PI);   /* its surface (y 0) on the wall, out +z */
            pm_rotate_y(pm, HALF_PI);   /* its pipe along x */
            pm_place(pm, PMK_SADDLE);
            pm_pop(pm);
        }
        put(pm, xg - 0.35f * sg, 1.0f, zl + 0.03f, 0.0f, PMK_VALVE_LEVER);
        put(pm, xg - 0.20f * sg, 1.0f, zl + 0.03f, 0.0f, PMK_GAUGE);
    }
}

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
