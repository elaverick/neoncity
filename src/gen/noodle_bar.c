#include "noodle_bar.h"
#include "pmodel_food.h"
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

const char *nb_form_name(int form) {
    static const char *const N[NB_FORMS] = { "straight", "corner", "hatch", "arcade", "tiered" };
    return form >= 0 && form < NB_FORMS ? N[form] : "?";
}
static int g_upper = -1;
void nb_force_upper(int kind) { g_upper = kind >= 0 && kind < NB_UPPERS ? kind : -1; }
const char *nb_upper_name(int kind) {
    static const char *const N[NB_UPPERS] = { "lantern", "plant", "both" };
    return kind >= 0 && kind < NB_UPPERS ? N[kind] : "?";
}
int nb_upper_by_name(const char *name) {
    for (int k = 0; k < NB_UPPERS; ++k) if (strcmp(name, nb_upper_name(k)) == 0) return k;
    return -1;
}

int nb_form_by_name(const char *name) {
    for (int f = 0; f < NB_FORMS; ++f) if (strcmp(name, nb_form_name(f)) == 0) return f;
    return -1;
}

/* The draws the slice's forms take, in order (hash.h: draws are sequenced). The forms SF2 added draw theirs by
 * hash, so a straight or corner bar's are NB5's whatever else changes. */
typedef struct NbDraws { float bw, bd, aisle, cd, cy, ov, spring, rise, pitch, fh, spanC, pick, spitch, rangeL, sideR; } NbDraws;

static void set_wall(NbPlan *p, int i, float ox, float oz, float ux, float uz, float len, float h, int kind) {
    p->wall[i] = (NbWall){ { ox, oz }, { ux, uz }, len, h };
    p->wallKind[i] = (uint8_t)kind;
    if (p->nwall < i + 1) p->nwall = i + 1;
}

/* SF2: a wall stall's depth, front to back: the bench, the cook's aisle, the counter, the stools in front and the
 * hood's (or deck's) overhang past them; a narrower aisle and bench where the site is shallow, then a standing
 * counter (no stools). Sets the plan's depths; returns the overhang the lip may reach to (fits: 0 if none). */
static void bar_depth(NbPlan *p, float D, float bd, float aisle, float cd, float cy, float ov, int *fits) {
    /* (a bar the slice's size never narrows: these only reach the shallow sites SF2 brought in) */
    float need = p->lining + bd + aisle + cd + 0.62f;
    if (need > D) aisle = maxf(0.70f, aisle - (need - D));
    need = p->lining + bd + aisle + cd + 0.62f;
    if (need > D) bd = maxf(0.60f, bd - (need - D));   /* the range's wok and stations want 0.6 m of bench */
    need = p->lining + bd + aisle + cd + 0.62f;
    p->stools = (uint8_t)(need + 0.06f <= D);   /* a stool's seat reaches 0.66 m out from the counter */
    p->cd = cd; p->cy = cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = p->lining + bd + aisle + cd;
    p->lip = minf(p->front + ov, D - 0.02f);
    if (p->lip < p->front + (p->stools ? 0.55f : 0.40f)) *fits = 0;
}

/* ----------------------------------------------- the slice's forms (NB4) --- */

static int plan_bar(NbPlan *p, const NbDraws *dr, float W, float D, float ceil, int form, int host, int *reOut) {
    float bw = dr->bw, bd = dr->bd, aisle = dr->aisle, cd = dr->cd, cy = dr->cy, ov = dr->ov, spring = dr->spring;
    float rise = dr->rise, pitch = dr->pitch, fh = dr->fh;
    /* SF2: down to 2.2 x 2.4 m (the slice's bars, 3.2 x 3.0 and over, keep their draws' every value) */
    int fits = W >= 2.2f && D >= 2.4f && ceil >= 3.1f;
    bw = clampf(minf(bw, W - 0.1f), 2.1f, 4.4f);
    p->x0 = -0.5f * bw; p->x1 = 0.5f * bw;
    p->lining = 0.10f;
    bar_depth(p, D, bd, aisle, cd, cy, ov, &fits);
    bd = p->benchD;
    /* under a ceiling the hood comes down; the fascia keeps 2.05 m under it */
    if (spring + rise > ceil - 0.15f) rise = maxf(0.35f, ceil - 0.15f - spring);
    if (spring + rise > ceil - 0.15f) spring = maxf(2.4f, ceil - 0.15f - rise);
    fh = minf(fh, spring - 2.05f);

    int corner = form >= 0 ? form == NB_CORNER : g_form >= 0 ? g_form == NB_CORNER : dr->pick < 0.35f;
    if (bw < 3.4f || D < 3.2f || !host || !p->stools) corner = 0;
    p->form = corner ? NB_CORNER : NB_STRAIGHT;
    p->side = dr->sideR < 0.5f ? 1 : -1;

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
        re = dr->rangeL < 0.5f ? -1 : 1;
        /* side walls from the host wall to the counter's back; the lining across */
        set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, cz0, spring, NB_WALL_SIDE);
        set_wall(p, 1, p->x1, cz0, 0.0f, -1.0f, cz0, spring, NB_WALL_SIDE);
        set_wall(p, 2, p->x0, p->lining, 1.0f, 0.0f, bw, p->hoodTop, NB_WALL_LINING);
        if (!host) {
            /* SF2: freestanding: the back's outside clad, and the slots beside the lining up to the hood closed */
            set_wall(p, 3, p->x1, 0.0f, -1.0f, 0.0f, bw, p->hoodTop, NB_WALL_CLAD);
            set_wall(p, 4, p->x0, 0.0f, 0.0f, 1.0f, p->lining, p->hoodTop, NB_WALL_CLAD);
            set_wall(p, 5, p->x1, p->lining, 0.0f, -1.0f, p->lining, p->hoodTop, NB_WALL_CLAD);
        }
        p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = cz0;
        /* the counter: one run, the cook's gate at the end away from the range */
        NbRun *c = &p->run[0];
        c->o[0] = p->x0; c->o[1] = p->front; c->u[0] = 1.0f; c->u[1] = 0.0f; c->len = bw;
        if (re < 0) { c->gate0 = bw - 0.68f; c->gate1 = bw - 0.06f; } else { c->gate0 = 0.06f; c->gate1 = 0.68f; }
        p->nrun = 1;
        if (p->stools) stool_line(p, p->x0 + 0.45f, p->front + STOOL_OUT, p->x1 - 0.45f, p->front + STOOL_OUT, dr->spitch, 6);
    } else {
        const int s = p->side;
        const float xe = s > 0 ? p->x1 : p->x0, xo = s > 0 ? p->x0 : p->x1;   /* the corner's end, the closed end */
        const float xc = xe - (float)s * CUST;                                 /* the side run's face */
        const float xi = xc - (float)s * cd;                                   /* its back */
        const float zs = p->lining + bd + 0.15f;                               /* where it starts */
        float span = clampf(dr->spanC, 1.4f, minf(p->lip - p->lining - 0.5f, fabsf(xe - xo) - 0.9f));
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
        if (s > 0) {
            set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, zsp, p->hoodTop, NB_WALL_SIDE);
            set_wall(p, 1, p->x0, zsp, 0.0f, 1.0f, cz0 - zsp, spring, NB_WALL_SIDE);
            set_wall(p, 3, xc, zs, 0.0f, -1.0f, zs, 0.0f, NB_WALL_SIDE);
        } else {
            set_wall(p, 0, p->x1, zsp, 0.0f, -1.0f, zsp, p->hoodTop, NB_WALL_SIDE);
            set_wall(p, 1, p->x1, cz0, 0.0f, -1.0f, cz0 - zsp, spring, NB_WALL_SIDE);
            set_wall(p, 3, xc, 0.0f, 0.0f, 1.0f, zs, 0.0f, NB_WALL_SIDE);
        }
        /* the lining: up to the deck behind it, to the spring under the side leg (its end plate closes above) */
        if (s > 0) { set_wall(p, 2, p->x0, p->lining, 1.0f, 0.0f, xsp - p->x0, p->hoodTop, NB_WALL_LINING);
                     set_wall(p, 4, xsp, p->lining, 1.0f, 0.0f, p->x1 - xsp, spring, NB_WALL_LINING); }
        else       { set_wall(p, 2, xsp, p->lining, 1.0f, 0.0f, p->x1 - xsp, p->hoodTop, NB_WALL_LINING);
                     set_wall(p, 4, p->x0, p->lining, 1.0f, 0.0f, xsp - p->x0, spring, NB_WALL_LINING); }
        p->wall[3].h = nb_hood_under(p, fabsf(xc - xsp)) - 0.01f;
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
        stool_line(p, xo + (float)s * 0.45f, p->front + STOOL_OUT, xc - (float)s * 0.32f, p->front + STOOL_OUT, dr->spitch, 5);
        stool_line(p, xc + (float)s * STOOL_OUT, p->front - 0.2f, xc + (float)s * STOOL_OUT, zs + 0.3f, dr->spitch, 3);
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
    *reOut = re;
    return fits;
}

/* ------------------------------------------------- the shop bay's stall (SF3) --- */

#define RECESS_PANEL 0.56f   /* the header panel kept over the hood at the building line: the shop's board on it */

/* The straight form taking over a shop bay: its back R (the plan's recess) inside the building line, across the
 * whole opening (W), the kitchen filling the recess and its counter's front on the line; the side walls are the
 * opening's jambs and go up with the lining to its head (ceil), where a soffit closes the recess; the hood springs
 * from the lining and comes out through the opening, its lip on posts over the sidewalk; a header panel in the
 * facade's plane closes the opening over the hood (recess_parts). */
static int plan_recessed(NbPlan *p, const NbDraws *dr, float W, float D, float ceil, int *reOut) {
    const float R = p->recess;
    float bd = dr->bd, cd = dr->cd, ov = dr->ov, spring = dr->spring, rise = dr->rise, fh = dr->fh;
    int fits = W >= 3.0f && R >= 1.9f && D >= R + 0.45f && ceil >= 3.1f;
    p->form = NB_STRAIGHT;
    p->side = dr->sideR < 0.5f ? 1 : -1;
    p->head = ceil;
    W -= 0.1f;   /* the opening; the site is 5 cm wider each side, for the hood's end plates and the walls' rivets */
    p->x0 = -0.5f * W; p->x1 = 0.5f * W;
    float aisle = R - 0.10f - bd - cd;
    if (aisle < 0.75f) { bd = maxf(0.60f, bd - (0.75f - aisle)); aisle = R - 0.10f - bd - cd; }
    aisle = clampf(aisle, 0.70f, 1.25f);
    p->lining = R - bd - aisle - cd;
    if (p->lining < 0.08f) fits = 0;
    p->cd = cd; p->cy = dr->cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = R;
    p->stools = (uint8_t)(D >= R + 0.68f);
    p->lip = minf(R + ov, D - 0.02f);
    if (p->lip < R + (p->stools ? 0.55f : 0.40f)) fits = 0;
    /* the hood under the opening's head, the fascia 2.05 m clear */
    if (spring + rise > ceil - 0.15f) rise = maxf(0.35f, ceil - 0.15f - spring);
    if (spring + rise > ceil - 0.15f) spring = maxf(2.4f, ceil - 0.15f - rise);
    fh = minf(fh, spring - 2.05f);
    PmHood *h = &p->hood;
    pm_hood_default(h, W, 1.0f);
    h->spring = spring; h->rise = rise; h->pitch = dr->pitch; h->fascia_h = fh;
    p->hoodTop = spring + rise;
    h->spine[0][0] = p->x0; h->spine[0][1] = p->lining;
    h->spine[1][0] = p->x1; h->spine[1][1] = p->lining;
    h->nspine = 2;
    h->span = p->lip - p->lining;
    /* SF3 (the user, on the first board): the shop's neon board goes on the header panel over the hood
     * (mesh_building.c face_signs, vendor_stall_board), so the hood leaves RECESS_PANEL of it under the head */
    for (int it = 0; it < 12 && nb_hood_under(p, R - p->lining) > ceil - RECESS_PANEL; ++it) {
        if (h->rise > 0.351f) h->rise = maxf(0.35f, h->rise - 0.06f);
        else if (h->spring > 2.401f) h->spring = maxf(2.4f, h->spring - 0.05f);
        else break;
    }
    spring = h->spring; rise = h->rise;
    fh = minf(fh, spring - 2.05f);
    h->fascia_h = fh;
    p->hoodTop = spring + rise;
    const int re = dr->rangeL < 0.5f ? -1 : 1;
    /* the jambs from the back to the building line (a centimetre short: their ends not in the facade's plane), and
     * the lining, all up to the head */
    set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, R - 0.01f, ceil, NB_WALL_SIDE);
    set_wall(p, 1, p->x1, R - 0.01f, 0.0f, -1.0f, R - 0.01f, ceil, NB_WALL_SIDE);
    set_wall(p, 2, p->x0, p->lining, 1.0f, 0.0f, W, ceil, NB_WALL_LINING);
    p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = R - cd;
    /* the counter between the jambs, the cook's gate at the end away from the range */
    NbRun *c = &p->run[0];
    const float cl = W - 0.08f;
    c->o[0] = p->x0 + 0.04f; c->o[1] = R; c->u[0] = 1.0f; c->u[1] = 0.0f; c->len = cl;
    if (re < 0) { c->gate0 = cl - 0.68f; c->gate1 = cl - 0.06f; } else { c->gate0 = 0.06f; c->gate1 = 0.68f; }
    p->nrun = 1;
    if (p->stools) stool_line(p, p->x0 + 0.45f, R + STOOL_OUT, p->x1 - 0.45f, R + STOOL_OUT, dr->spitch, 7);
    /* the posts at the lip's corners */
    p->fascia = h->span - 0.5f * h->lip_r + 0.0015f;
    const float ph = spring - fh, z = p->lining + p->fascia - 0.035f - 0.003f;
    p->post[0][0] = p->x0 + 0.035f; p->post[0][1] = z; p->post[0][2] = ph;
    p->post[1][0] = p->x1 - 0.035f; p->post[1][1] = z; p->post[1][2] = ph;
    p->npost = 2;
    p->panel = nb_hood_under(p, R - p->lining);
    *reOut = re;
    return fits;
}

/* ------------------------------------------- the inset corner unit (SF3) --- */

/* SF3 (the user, on the second board): a building's lit corner shop taken over by the corner form set into it: the
 * corner shop opened up on both faces, the building's corner left standing on a pier (INSET_PIER square), the counter
 * in an L along both faces inside the building lines, stools all along both, the customers sitting under the
 * building. The site's frame: z out of the front face F (its building line at recess), x along it; the side face S's
 * building line at x = side * (w/2 - 0.05) (side: the site's end the corner is at); the closed end's jamb at the
 * site's other end, 5 cm in (the jamb's rivets). The hood wraps the corner inside the pier's inner faces, its fascia
 * and icons seen under the header panels that close both openings over it (inset_parts); the kitchen along the
 * back; the soffit at the opening's head over all of it. */
#define INSET_PIER   NB_INSET_PIER     /* the corner's pier, square (its outer faces the building's) */
#define INSET_LINING NB_INSET_LINING   /* the lining's face: the side face's opening runs back to it */

static int plan_inset(NbPlan *p, const NbDraws *dr, float W, float ceil, int s, int *reOut) {
    const float R = p->recess, P = INSET_PIER, sf = (float)s;
    const float xB = sf * (0.5f * W - 0.05f), xj = -sf * (0.5f * W - 0.05f);   /* S's building line; the jamb */
    const float lip = R - P - 0.03f, xe = xB - sf * (P + 0.03f);               /* the hood's lips, inside the pier */
    float bd = dr->bd, cd = dr->cd, spring = dr->spring, rise = dr->rise, fh = dr->fh;
    p->form = NB_CORNER;
    p->side = s;
    p->inset = (int8_t)s;
    p->head = ceil;
    p->x0 = minf(xj, xe); p->x1 = maxf(xj, xe);
    const float bw = p->x1 - p->x0;
    int fits = bw >= 3.4f && bw <= 5.5f && R >= 3.0f && ceil >= 3.1f;
    p->lining = INSET_LINING;
    /* the kitchen's depth from the customers' strip (0.95 m at least, inside the pier) back to the lining */
    float aisle = lip - 0.95f - p->lining - bd - cd;
    if (aisle < 0.75f) { bd = maxf(0.60f, bd - (0.75f - aisle)); aisle = lip - 0.95f - p->lining - bd - cd; }
    aisle = clampf(aisle, 0.70f, 1.25f);
    p->cd = cd; p->cy = dr->cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = p->lining + bd + aisle + cd;
    if (lip - p->front < 0.90f) fits = 0;
    p->stools = 1;
    p->lip = lip;
    /* the hood under the soffit, the header panels' bottoms just over its springing (their boards on them: the
     * hood leaves RECESS_PANEL of panel under the head) */
    if (spring + rise > ceil - 0.15f) rise = maxf(0.35f, ceil - 0.15f - spring);
    if (spring + rise > ceil - 0.15f) spring = maxf(2.4f, ceil - 0.15f - rise);
    if (spring + 0.04f > ceil - RECESS_PANEL) spring = maxf(2.4f, ceil - RECESS_PANEL - 0.04f);
    fh = minf(fh, spring - 2.05f);
    p->panel = spring + 0.04f;
    PmHood *h = &p->hood;
    pm_hood_default(h, bw, 1.0f);
    h->spring = spring; h->rise = rise; h->pitch = dr->pitch; h->fascia_h = fh;
    p->hoodTop = spring + rise;
    const float cz0 = p->front - cd;                 /* the counter's back */
    const float xo = s > 0 ? p->x0 : p->x1;          /* the closed end (the jamb) */
    const float xc = xe - sf * CUST;                 /* the side run's face */
    const float xi = xc - sf * cd;                   /* its back */
    const float zs = p->lining + bd + 0.15f;         /* where it starts */
    const float span = clampf(dr->spanC, 1.4f, minf(lip - p->lining - 0.5f, bw - 0.9f));
    const float zsp = lip - span, xsp = xe - sf * span;
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
    const int re = -s;
    /* the jamb at the closed end, from the back to F's building line, and the lining from it to S's building line,
     * both up to the head; the short wall at the corner end from the lining to the side run, up under the side leg */
    /* (both a centimetre short of the building lines: their ends not in the facade's plane) */
    if (s > 0) {
        set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, R - 0.01f, ceil, NB_WALL_SIDE);
        set_wall(p, 1, p->x0, p->lining, 1.0f, 0.0f, xB - 0.01f - p->x0, ceil, NB_WALL_LINING);
        set_wall(p, 2, xc, zs, 0.0f, -1.0f, zs, 0.0f, NB_WALL_SIDE);
    } else {
        set_wall(p, 0, p->x1, R - 0.01f, 0.0f, -1.0f, R - 0.01f, ceil, NB_WALL_SIDE);
        set_wall(p, 1, xB + 0.01f, p->lining, 1.0f, 0.0f, p->x1 - xB - 0.01f, ceil, NB_WALL_LINING);
        set_wall(p, 2, xc, 0.0f, 0.0f, 1.0f, zs, 0.0f, NB_WALL_SIDE);
    }
    p->wall[2].h = nb_hood_under(p, fabsf(xc - xsp)) - 0.01f;
    p->kx0 = s > 0 ? p->x0 + 0.05f : xi; p->kx1 = s > 0 ? xi : p->x1 - 0.05f; p->kz1 = cz0;
    /* the counter: the front run from the jamb (inside its rivets) to the side run's face, the side run back to the
     * short wall; the cook's gate in the front run at the closed end */
    NbRun *c = p->run;
    const float flen = fabsf(xc - xo) - 0.04f;
    if (s > 0) { c[0] = (NbRun){ { p->x0 + 0.04f, p->front }, { 1.0f, 0.0f }, flen, 0.06f, 0.68f };
                 c[1] = (NbRun){ { xc, cz0 }, { 0.0f, -1.0f }, cz0 - zs, 0.0f, 0.0f }; }
    else       { c[0] = (NbRun){ { xc, p->front }, { 1.0f, 0.0f }, flen, flen - 0.68f, flen - 0.06f };
                 c[1] = (NbRun){ { xc, zs }, { 0.0f, 1.0f }, cz0 - zs, 0.0f, 0.0f }; }
    p->nrun = 2;
    /* the column in the counter's corner, up under the hood's hip */
    const float colx = xc - sf * 0.5f * cd, colz = p->front - 0.5f * cd;
    p->column[0] = colx; p->column[1] = colz; p->column[2] = 0.5f * cd + 0.04f;
    p->column[3] = nb_hood_under(p, maxf(colz - zsp, sf * (colx - xsp)) + p->column[2]) - 0.005f;
    /* stools all along both runs */
    stool_line(p, xo + sf * 0.45f, p->front + STOOL_OUT, xc - sf * 0.32f, p->front + STOOL_OUT, dr->spitch, 8);
    stool_line(p, xc + sf * STOOL_OUT, p->front - 0.2f, xc + sf * STOOL_OUT, zs + 0.3f, dr->spitch, 4);
    /* the posts at the lip's free ends: by the jamb, and the side leg's at the lining */
    p->fascia = h->span - 0.5f * h->lip_r + 0.0015f;
    const float ph = spring - fh;
    p->post[0][0] = xo + sf * 0.035f; p->post[0][1] = zsp + p->fascia - 0.038f; p->post[0][2] = ph;
    p->post[1][0] = xsp + sf * (p->fascia - 0.038f); p->post[1][1] = p->lining + 0.04f; p->post[1][2] = ph;
    p->npost = 2;
    *reOut = re;
    return fits;
}

/* ------------------------------------------------ the deep shop unit (SF3) --- */

#define DEEP_WALL   0.06f   /* the room's walls, inside the opening's edges */
#define DEEP_FRAME  0.10f   /* the shopfront's frame, in from the building line (its glass in its middle) */
#define DEEP_LEDGE  0.30f   /* the window ledge, inside the frame */
#define DEEP_SEAT   0.47f   /* a low stool's middle out from its table's */

static void hash_units(uint32_t h, float *u, int n);

static void add_seat(NbPlan *p, float x, float z) {
    if (p->nseat < NB_SEATS_MAX) { p->seat[p->nseat][0] = x; p->seat[p->nseat][1] = z; p->nseat++; }
}

/* SF3 (the user, on the first board): a shop bay taken over deep. The bay's own glazed shopfront stays on the
 * building line (real glass in a frame in the trim coat, mullions on the bay's thirds as the shops' windows, a
 * stall riser under them, a glazed door at one end under a fanlight); behind it a room about as deep as the shops'
 * interior mapping (5 m: shaders/city/facade.glsl), tiled to the dado, its ceiling the opening's head. The serving
 * bar at its back is the arcade counter (its deck and riveted header hung from the ceiling on rods, the kitchen
 * behind), against the room's wall away from the door, the passage from the door to the cook's gate beside it. In
 * front of it tables with low stools, a ledge along the window with stools at it, battens on the ceiling; an
 * awning out over the street where the site reaches out for one (the site's depth past the building line). */
static int plan_deep(NbPlan *p, const NbDraws *dr, float W, float D, float ceil, int *reOut) {
    float u[12];
    hash_units(hash_combine(p->seed, 0xDEE9u), u, 12);
    const float R = p->recess, OH = 0.5f * (W - 0.1f);
    int fits = W >= 4.6f && R >= 4.2f && ceil >= 3.0f;
    p->form = NB_ARCADE;
    p->deep = 1;
    p->head = ceil;
    p->roomX0 = -OH + DEEP_WALL; p->roomX1 = OH - DEEP_WALL;
    const int ds = u[0] < 0.5f ? 1 : -1;
    p->doorSide = (int8_t)ds;
    /* the door against the jamb at its end */
    const float dw = 0.9f + 0.1f * u[1];
    if (ds > 0) { p->shopDoor[1] = p->roomX1; p->shopDoor[0] = p->roomX1 - dw; }
    else        { p->shopDoor[0] = p->roomX0; p->shopDoor[1] = p->roomX0 + dw; }
    /* the kitchen against the far wall, the passage (0.85 m at least) on the door's side */
    const float RW = p->roomX1 - p->roomX0;
    const float bw = clampf(minf(dr->bw, RW - 0.95f), 2.4f, 4.4f);
    if (RW - bw < 0.85f) fits = 0;
    if (ds > 0) { p->x0 = p->roomX0 + 0.01f; p->x1 = p->x0 + bw; } else { p->x1 = p->roomX1 - 0.01f; p->x0 = p->x1 - bw; }
    p->lining = 0.10f;
    const float bd = dr->bd, aisle = dr->aisle, cd = dr->cd;
    p->cd = cd; p->cy = dr->cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = p->lining + bd + aisle + cd;
    p->lip = p->front + 0.35f + 0.2f * u[2];   /* the header over the queue at the counter */
    p->stools = 0;                              /* the customers sit at the tables and the ledge */
    const float deckY = clampf(ceil - 0.08f, 2.4f, 2.75f), hb = maxf(2.02f, deckY - (0.42f + 0.14f * u[3]));
    p->deckY = deckY;
    p->rodTop = ceil - 0.03f;   /* the ceiling's underside */
    PmHood *h = &p->hood;
    pm_hood_default(h, bw, 1.0f);
    h->nspine = 0;
    h->spring = deckY; h->rise = 0.0f; h->fascia_h = deckY - hb;
    h->span = p->lip - p->lining;
    p->hoodTop = deckY;
    p->fascia = h->span;
    p->deck[0] = p->x0; p->deck[1] = 0.0f; p->deck[2] = p->x1; p->deck[3] = p->lip;
    const int re = -ds;   /* the range at the far wall's end, the cook's gate at the passage's */
    const float cz0 = p->front - cd;
    /* its side wall on the passage (the far wall is the room's own), its lining */
    if (ds > 0) set_wall(p, 0, p->x1, cz0, 0.0f, -1.0f, cz0, deckY, NB_WALL_SIDE);
    else        set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, cz0, deckY, NB_WALL_SIDE);
    set_wall(p, 1, p->x0, p->lining, 1.0f, 0.0f, bw, deckY, NB_WALL_LINING);
    p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = cz0;
    NbRun *c = &p->run[0];
    c->o[0] = p->x0; c->o[1] = p->front; c->u[0] = 1.0f; c->u[1] = 0.0f; c->len = bw;
    if (re < 0) { c->gate0 = bw - 0.68f; c->gate1 = bw - 0.06f; } else { c->gate0 = 0.06f; c->gate1 = 0.68f; }
    p->nrun = 1;
    /* the window ledge along the glazed thirds (not the door's), stools at it facing the street, along the
     * kitchen's stretch of the room (the passage kept) */
    const float zl = R - DEEP_FRAME - DEEP_LEDGE;
    const float lx0 = ds > 0 ? p->roomX0 : -1.0f + 0.03f, lx1 = ds > 0 ? 1.0f - 0.03f : p->roomX1;
    p->ledge[0] = lx0; p->ledge[1] = lx1; p->ledge[2] = zl;
    const float tx0 = p->x0 + 0.05f, tx1 = p->x1 - 0.05f;
    stool_line(p, maxf(lx0, tx0) + 0.30f, zl - 0.30f, minf(lx1, tx1) - 0.30f, zl - 0.30f, 0.62f, 6);
    /* the tables between the queue and the ledge stools' backs: four seats where the room's deep enough, else two
     * (a stool each side along the row) */
    const float za = p->lip + 0.15f, zb = zl - 0.30f - 0.40f, dz = zb - za;
    if (dz >= 0.60f) {
        const int four = dz >= 1.30f;
        const float cell = 2.0f * (DEEP_SEAT + 0.17f), gap = 0.14f, zt = 0.5f * (za + zb);
        int n = (int)floorf((tx1 - tx0 + gap) / (cell + gap));
        if (n > NB_TABLES_MAX) n = NB_TABLES_MAX;
        const float used = (float)n * cell + (float)(n - 1) * gap, xt0 = 0.5f * (tx0 + tx1 - used) + 0.5f * cell;
        for (int k = 0; k < n; ++k) {
            const float x = xt0 + (float)k * (cell + gap);
            p->table[p->ntable][0] = x; p->table[p->ntable][1] = zt; p->table[p->ntable][2] = four ? 4.0f : 2.0f;
            p->ntable++;
            add_seat(p, x - DEEP_SEAT, zt); add_seat(p, x + DEEP_SEAT, zt);
            if (four) { add_seat(p, x, zt - DEEP_SEAT); add_seat(p, x, zt + DEEP_SEAT); }
        }
    }
    if (p->ntable == 0 && p->nstool == 0) fits = 0;
    /* the awning where the site reaches out past the building line for one */
    p->awning = D - R >= 0.85f ? D - R - 0.03f : 0.0f;
    *reOut = re;
    return fits;
}

/* The deep unit's battens on its ceiling, over the tables and the window: the last of the plan's tubes (lit as
 * the bar's), after plan_signs' (the header's). */
static void deep_tubes(NbPlan *p) {
    const float zf = p->recess - DEEP_FRAME, y = p->head - 0.03f - 0.06f, cx = 0.5f * (p->roomX0 + p->roomX1);
    const float zs[2] = { p->lip + 0.45f * (zf - p->lip), p->lip + 0.85f * (zf - p->lip) };
    for (int k = 0; k < 2 && p->ntube < NB_TUBES_MAX; ++k) {
        float *t = p->tube[p->ntube++];
        t[0] = cx - 0.6f; t[1] = y; t[2] = zs[k]; t[3] = cx + 0.6f; t[4] = y; t[5] = zs[k];
        p->roomTubes++;
    }
}

/* ---------------------------------------------------- the hatch kiosk (SF2) --- */

static void hash_units(uint32_t h, float *u, int n) { for (int k = 0; k < n; ++k) u[k] = hash_float(hash_combine(h, (uint32_t)k)); }

static int plan_hatch(NbPlan *p, float W, float D, float ceil, int host, int *reOut) {
    float u[16];
    hash_units(hash_combine(p->seed, 0x4A7C4u), u, 16);
    int fits = W >= 1.9f && D >= 1.65f && ceil >= 2.95f;
    p->form = NB_HATCH;
    const float bw = maxf(minf(clampf(W - 0.15f - 0.5f * u[0], 1.8f, 3.2f), W - 0.1f), 1.0f);
    p->x0 = -0.5f * bw; p->x1 = 0.5f * bw;
    p->lining = 0.10f;
    float bd = 0.60f + 0.08f * u[1], cd = 0.32f + 0.08f * u[2], aisle = 0.60f + 0.25f * u[3], ov = 0.32f + 0.22f * u[4];
    /* the box's depth: the bench, the aisle, the inner counter, the lip past its front over the sill */
    const float room = D - 0.02f - ov;
    float need = p->lining + bd + aisle + cd;
    if (need > room) aisle = maxf(0.45f, aisle - (need - room));   /* a kerb kiosk's cook stands sideways */
    need = p->lining + bd + aisle + cd;
    if (need > room) bd = maxf(0.60f, bd - (need - room));
    need = p->lining + bd + aisle + cd;
    if (need > room) cd = maxf(0.30f, cd - (need - room));
    need = p->lining + bd + aisle + cd;
    if (need > room) ov = maxf(0.0f, D - 0.02f - need);   /* the sill wants 0.2 at least (below) */
    p->cd = cd; p->cy = 1.0f + 0.05f * u[5]; p->benchD = bd; p->benchY = 0.90f;
    p->front = p->lining + bd + aisle + cd;
    p->lip = minf(p->front + ov, D - 0.02f);
    p->sill = minf(maxf(0.18f + 0.06f * u[6], 0.5f - p->cd), p->lip - p->front);   /* under the lip, flush at most */
    if (p->sill < 0.14f || p->cd + p->sill < 0.5f) fits = 0;   /* the inner counter and the sill: one counter's depth */
    float spring = 2.30f + 0.15f * u[7], rise = 0.42f + 0.20f * u[8];
    if (spring + rise > ceil - 0.15f) rise = maxf(0.3f, ceil - 0.15f - spring);
    const float fh = minf(0.24f + 0.06f * u[9], spring - 2.0f);
    PmHood *h = &p->hood;
    pm_hood_default(h, bw, 1.0f);
    h->spring = spring; h->rise = rise; h->pitch = 0.46f + 0.10f * u[10]; h->fascia_h = fh;
    h->spine[0][0] = p->x0; h->spine[0][1] = p->lining;
    h->spine[1][0] = p->x1; h->spine[1][1] = p->lining;
    h->nspine = 2;
    h->span = p->lip - p->lining;
    p->hoodTop = spring + rise;
    p->fascia = h->span - 0.5f * h->lip_r + 0.0015f;
    const int re = u[11] < 0.5f ? -1 : 1;
    const float kz1 = p->front - cd;
    /* the sides to the front, up to the spring (the hood's end plates close above); the back lining; the back's
     * outside where it stands free (always seen from a kerb) */
    set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, p->front, spring, NB_WALL_SIDE);
    set_wall(p, 1, p->x1, p->front, 0.0f, -1.0f, p->front, spring, NB_WALL_SIDE);
    set_wall(p, 2, p->x0, p->lining, 1.0f, 0.0f, bw, p->hoodTop, NB_WALL_LINING);
    if (!host) set_wall(p, 3, p->x1, 0.0f, -1.0f, 0.0f, bw, p->hoodTop, NB_WALL_CLAD);
    p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = kz1;
    /* the inner counter under the glazing: its face the box's front, no gate (the door's in the side wall) */
    p->run[0] = (NbRun){ { p->x0, p->front }, { 1.0f, 0.0f }, bw, 0.0f, 0.0f };
    p->nrun = 1;
    /* the glazing up to under the header; the hatch towards the closed end (the range's heat away from the
     * customers), 0.6-0.9 m wide */
    const float hf = nb_hood_under(p, p->front - p->lining);
    p->glazeTop = minf(2.05f, hf - 0.16f);
    if (p->glazeTop < p->cy + 0.75f) fits = 0;
    const float hw = minf(0.6f + 0.3f * u[12], bw - 0.5f);
    const float hx = -(float)re * 0.5f * maxf(bw - hw - 0.3f, 0.0f) * (0.3f + 0.7f * u[13]);
    p->hatch[0] = hx - 0.5f * hw; p->hatch[1] = hx + 0.5f * hw;
    /* the door in the side wall at the closed end, across the cook's aisle */
    p->door[0] = re < 0 ? p->x1 : p->x0;
    p->door[1] = p->lining + bd + 0.03f; p->door[2] = kz1 - 0.03f;
    if (p->door[2] - p->door[1] < 0.38f || spring < 2.02f) p->door[1] = p->door[2] = 0.0f;   /* a kiosk's door is narrow */
    /* stools at the sill where the site leaves room in front */
    p->stools = (uint8_t)(D - p->front >= 0.66f);
    if (p->stools) stool_line(p, p->hatch[0] + 0.05f, p->front + STOOL_OUT, p->hatch[1] - 0.05f, p->front + STOOL_OUT, 0.66f, 2);
    *reOut = re;
    return fits;
}

/* ------------------------------------------------- the arcade counter (SF2) --- */

static int plan_arcade(NbPlan *p, const NbDraws *dr, float W, float D, float ceiling, int host, int hang, int *reOut) {
    float u[8];
    hash_units(hash_combine(p->seed, 0xA4CAu), u, 8);
    int fits = W >= 2.2f && D >= 2.2f && (ceiling <= 0.0f || ceiling >= 2.55f);
    p->form = NB_ARCADE;
    const float bw = clampf(minf(dr->bw, W - 0.1f), 2.1f, 4.4f);
    p->x0 = -0.5f * bw; p->x1 = 0.5f * bw;
    p->lining = 0.10f;
    bar_depth(p, D, dr->bd, dr->aisle, dr->cd, dr->cy, dr->ov, &fits);
    /* the soffit under the ceiling (or, forced on an open site, at its own height on posts); the header's bottom
     * at 2.02 m at least */
    const float deckY = ceiling > 0.0f ? clampf(ceiling - 0.08f, 2.4f, 2.75f) : 2.55f + 0.15f * u[0];
    const float hb = maxf(2.02f, deckY - (0.42f + 0.14f * u[1]));
    p->deckY = deckY;
    p->rodTop = hang && ceiling > 0.0f ? ceiling : 0.0f;   /* hangers (short where the deck's close under it); under a vault the front's overhead is higher */
    PmHood *h = &p->hood;
    pm_hood_default(h, bw, 1.0f);
    h->nspine = 0;
    h->spring = deckY; h->rise = 0.0f; h->fascia_h = deckY - hb;
    h->span = p->lip - p->lining;
    p->hoodTop = deckY;
    p->fascia = h->span;
    p->deck[0] = p->x0; p->deck[1] = 0.0f; p->deck[2] = p->x1; p->deck[3] = p->lip;
    const int re = dr->rangeL < 0.5f ? -1 : 1;
    const float cz0 = p->front - p->cd;
    set_wall(p, 0, p->x0, 0.0f, 0.0f, 1.0f, cz0, deckY, NB_WALL_SIDE);
    set_wall(p, 1, p->x1, cz0, 0.0f, -1.0f, cz0, deckY, NB_WALL_SIDE);
    set_wall(p, 2, p->x0, p->lining, 1.0f, 0.0f, bw, deckY, NB_WALL_LINING);
    if (!host) set_wall(p, 3, p->x1, 0.0f, -1.0f, 0.0f, bw, deckY, NB_WALL_CLAD);
    p->kx0 = p->x0 + 0.05f; p->kx1 = p->x1 - 0.05f; p->kz1 = cz0;
    NbRun *c = &p->run[0];
    c->o[0] = p->x0; c->o[1] = p->front; c->u[0] = 1.0f; c->u[1] = 0.0f; c->len = bw;
    if (re < 0) { c->gate0 = bw - 0.68f; c->gate1 = bw - 0.06f; } else { c->gate0 = 0.06f; c->gate1 = 0.68f; }
    p->nrun = 1;
    if (p->stools) stool_line(p, p->x0 + 0.45f, p->front + STOOL_OUT, p->x1 - 0.45f, p->front + STOOL_OUT, dr->spitch, 6);
    if (!p->rodTop) {   /* nothing to hang from: posts under the header's ends */
        const float z = p->lip - 0.06f - 0.035f;
        p->post[0][0] = p->x0 + 0.035f; p->post[0][1] = z; p->post[0][2] = hb;
        p->post[1][0] = p->x1 - 0.035f; p->post[1][1] = z; p->post[1][2] = hb;
        p->npost = 2;
    }
    *reOut = re;
    return fits;
}

/* ---------------------------------------------------- the tiered kiosk (SF2) --- */

static int plan_tiered(NbPlan *p, const NbDraws *dr, float W, float D, float ceil, int *reOut) {
    float u[8];
    hash_units(hash_combine(p->seed, 0x7135Du), u, 8);
    int fits = W >= 4.0f && D >= 3.4f && ceil >= 4.8f;
    p->form = NB_TIERED;
    const float ch = 0.28f + 0.10f * u[0], ov = 0.66f + 0.12f * u[1], S = 1.05f + 0.13f * u[2];
    /* the front leg's lip within 4.4 m across (the span a fascia carries between the hips' posts) */
    const float XR = clampf(minf(0.5f * W - ov - 0.04f, 2.2f - ov), 1.2f, 2.1f), XL = -XR;
    float bd = dr->bd, aisle = dr->aisle;
    const float cd = dr->cd, cy = dr->cy;
    p->x0 = XL; p->x1 = XR;
    p->lining = 2.0f * ch + 0.06f;
    float F = p->lining + bd + aisle + cd;
    if (F + ov > D - 0.02f) aisle = maxf(0.8f, aisle - (F + ov - D + 0.02f));
    F = p->lining + bd + aisle + cd;
    if (F + ov > D - 0.02f) bd = maxf(0.55f, bd - (F + ov - D + 0.02f));
    F = p->lining + bd + aisle + cd;
    if (F + ov > D - 0.02f) fits = 0;
    p->cd = cd; p->cy = cy; p->benchD = bd; p->benchY = 0.90f;
    p->front = F; p->lip = F + ov;
    p->stools = 1;
    const float sl = XL - ov + S, sr = XR + ov - S, sf = F + ov - S;
    if (sr - sl < 2.0f * ch + 0.3f || sf < p->lining + 0.3f) fits = 0;
    float spring = dr->spring, rise = dr->rise;
    const float fh = minf(dr->fh, spring - 2.05f);
    PmHood *h = &p->hood;
    pm_hood_default(h, XR - XL, 1.0f);
    h->spring = spring; h->rise = rise; h->pitch = dr->pitch; h->fascia_h = fh;
    /* the U round the kitchen: the left leg out to -x, the front leg out to +z, the right leg out to +x */
    h->spine[0][0] = sl; h->spine[0][1] = 0.05f;   /* the legs' end plates a little in from the back */
    h->spine[1][0] = sl; h->spine[1][1] = sf;
    h->spine[2][0] = sr; h->spine[2][1] = sf;
    h->spine[3][0] = sr; h->spine[3][1] = 0.05f;
    h->nspine = 4;
    h->span = S;
    p->hoodTop = spring + rise;
    p->fascia = h->span - 0.5f * h->lip_r + 0.0015f;
    p->deck[0] = sl; p->deck[1] = 0.0f; p->deck[2] = sr; p->deck[3] = sf;
    const float zs = p->lining + bd + 0.15f, kz1 = F - cd;
    if (kz1 - zs < 0.69f) fits = 0;   /* the gate at the side run's back */
    /* the back's outside: under the side legs to the spring (their end plates close above), behind the deck to
     * it; the short side walls from the back to the side runs, up under the legs; the kitchen's lining */
    set_wall(p, 0, sl, 0.0f, -1.0f, 0.0f, sl - XL, spring, NB_WALL_CLAD);
    set_wall(p, 1, sr, 0.0f, -1.0f, 0.0f, sr - sl, p->hoodTop, NB_WALL_CLAD);
    set_wall(p, 2, XL, p->lining, 1.0f, 0.0f, XR - XL, spring, NB_WALL_LINING);
    set_wall(p, 3, XR, 0.0f, -1.0f, 0.0f, XR - sr, spring, NB_WALL_CLAD);
    set_wall(p, 4, XL, 0.0f, 0.0f, 1.0f, zs, nb_hood_under(p, sl - XL) - 0.01f, NB_WALL_SIDE);
    set_wall(p, 5, XR, zs, 0.0f, -1.0f, zs, nb_hood_under(p, XR - sr) - 0.01f, NB_WALL_SIDE);
    p->kx0 = XL + 0.05f; p->kx1 = XR - 0.05f; p->kz1 = kz1;
    /* the counter on three sides: the front across, the side runs back from it to the short walls; the gate in
     * the left run by the back */
    p->run[0] = (NbRun){ { XL, F }, { 1.0f, 0.0f }, XR - XL, 0.0f, 0.0f };
    p->run[1] = (NbRun){ { XL, zs }, { 0.0f, 1.0f }, kz1 - zs, 0.06f, 0.68f };
    p->run[2] = (NbRun){ { XR, kz1 }, { 0.0f, -1.0f }, kz1 - zs, 0.0f, 0.0f };
    p->nrun = 3;
    stool_line(p, XL + 0.35f, F + STOOL_OUT, XR - 0.35f, F + STOOL_OUT, dr->spitch, 6);
    stool_line(p, XL - STOOL_OUT, F - 0.2f, XL - STOOL_OUT, zs + 0.95f, dr->spitch, 3);
    stool_line(p, XR + STOOL_OUT, F - 0.2f, XR + STOOL_OUT, zs + 0.3f, dr->spitch, 3);
    /* posts at the side legs' free lips (the back) and under the front hips */
    const float ph = spring - fh, fo = p->fascia - 0.038f;
    p->post[0][0] = sl - fo; p->post[0][1] = 0.09f; p->post[0][2] = ph;
    p->post[1][0] = sr + fo; p->post[1][1] = 0.09f; p->post[1][2] = ph;
    p->post[2][0] = sl - fo; p->post[2][1] = sf + fo; p->post[2][2] = ph;
    p->post[3][0] = sr + fo; p->post[3][1] = sf + fo; p->post[3][2] = ph;
    p->npost = 4;
    /* the upper tier on the deck round the column, the column through it */
    p->upperKind = (uint8_t)(g_upper >= 0 ? g_upper : (int)(u[4] * (float)NB_UPPERS) % NB_UPPERS);
    static const float UH[NB_UPPERS] = { 0.70f, 1.05f, 1.25f };   /* lantern, plant box, both */
    const float y0 = p->hoodTop, y1 = minf(minf(y0 + UH[p->upperKind] + 0.1f * u[3], 4.45f), ceil - 0.60f);
    if (y1 - y0 < 0.6f) fits = 0;
    p->upper[0] = sl + 0.12f; p->upper[1] = 0.02f; p->upper[2] = sr - 0.12f; p->upper[3] = sf - 0.12f;
    p->upper[4] = y0; p->upper[5] = y1;
    p->column[0] = 0.0f; p->column[1] = ch + 0.02f; p->column[2] = ch; p->column[3] = minf(4.98f, y1 + 0.45f);
    *reOut = dr->rangeL < 0.5f ? -1 : 1;
    return fits;
}

/* ------------------------------------------------- what every form shares --- */

/* The kitchen along the lining: the bench, the range at one end, the gas at the other, the shelves between; the
 * duct up through the roof. */
static void plan_kitchen(NbPlan *p, int re, float ceil) {
    /* SF1: in about a third of the straight bars the range stands along the bench, a short bench between it and
     * the side wall (its extractor, splashback and duct go with it) */
    const float benchLen = p->kx1 - p->kx0 - 0.6f;
    const float midB = clampf(0.3f * (benchLen - 0.95f), 0.45f, 0.85f);
    p->rangeMid = (uint8_t)(p->form == NB_STRAIGHT && benchLen - 0.95f - midB >= 1.0f && hash_float(hash_combine(p->seed, 0x4D1Du)) < 0.35f);
    float inset = p->rangeMid ? midB : 0.15f;
    if (p->form == NB_TIERED) {
        /* SF2: the range in the middle of the bench, under the upper tier, so its duct rises into it */
        p->rangeMid = 1;
        inset = maxf(0.45f, re < 0 ? -0.475f - p->kx0 : p->kx1 - 0.475f);
    }
    if (re < 0) {
        p->benchX0 = p->kx0; p->benchX1 = p->kx1 - 0.6f; p->gasX = p->kx1 - 0.3f;
        p->rangeX0 = p->benchX0 + inset; p->rangeX1 = p->rangeX0 + 0.95f;
        p->shelfX0 = p->rangeX1 + 0.25f; p->shelfX1 = p->benchX1 - 0.05f;
    } else {
        p->benchX1 = p->kx1; p->benchX0 = p->kx0 + 0.6f; p->gasX = p->kx0 + 0.3f;
        p->rangeX1 = p->benchX1 - inset; p->rangeX0 = p->rangeX1 - 0.95f;
        p->shelfX0 = p->benchX0 + 0.05f; p->shelfX1 = p->rangeX0 - 0.25f;
    }
    if (p->shelfX1 - p->shelfX0 < 0.5f) p->shelfX0 = p->shelfX1 = 0.0f;
    p->ductR = 0.13f;
    p->ductX = 0.5f * (p->rangeX0 + p->rangeX1);
    p->ductZ = p->lining + 0.035f + p->ductR;
    if (p->form == NB_TIERED) {
        /* up through the deck and the plant box, out over its roof; into the lantern, which vents it */
        p->ductTop = p->upperKind == NB_UPPER_LANTERN ? p->upper[4] + 0.30f : minf(p->upper[5] + 0.3f, minf(4.98f, ceil - 0.02f) - 0.27f);
        p->top = maxf(p->ductTop + 0.27f, p->column[3]);
        return;
    }
    p->ductTop = minf(p->hoodTop + 0.75f, ceil - 0.05f);
    p->top = p->ductTop + 0.27f;
    if (p->top > ceil) p->top = ceil;
    /* SF2: no room for its cap under a ceiling: up into it, through a flashing plate (nb_duct_capped) */
    if (p->ceiling > 0.0f && p->ductTop + 0.27f > p->ceiling) { p->ductTop = p->ceiling - 0.005f; p->top = p->ceiling; }
}

int nb_duct_capped(const NbPlan *p) {
    if (p->form == NB_TIERED && p->upperKind == NB_UPPER_LANTERN) return 0;   /* it ends inside the lantern */
    return p->ceiling <= 0.0f || p->ductTop + 0.27f <= p->ceiling + 1e-3f;
}

/* The strip lights' tubes and the icon housings: behind and on the hood's fascia, or the arcade's header. */
static void plan_signs(NbPlan *p) {
    const PmHood *h = &p->hood;
    const float spring = h->spring, fh = h->fascia_h;
    const float yt = spring - fh + 0.05f;
    if (p->form == NB_ARCADE) {
        /* the tubes under the deck behind the header (on brackets to its back), the icons on its face */
        const float L = p->x1 - p->x0, zt = p->lip - 0.06f - 0.11f;
        int nt = (int)(L / 1.5f);
        nt = nt < 1 ? 1 : nt > 3 ? 3 : nt;
        const float tl = minf(1.2f, L / (float)nt - 0.25f);
        for (int k = 0; k < nt && p->ntube < NB_TUBES_MAX && tl > 0.4f; ++k) {
            const float cx = p->x0 + L * ((float)k + 0.5f) / (float)nt;
            float *t = p->tube[p->ntube++];
            t[0] = cx - 0.5f * tl; t[1] = yt; t[2] = zt; t[3] = cx + 0.5f * tl; t[4] = yt; t[5] = zt;
        }
        int ni = (int)((L - 0.3f) / 0.9f);
        ni = ni < 1 ? 1 : ni > 4 ? 4 : ni;
        const float size = minf(fh - 0.08f, 0.40f);
        for (int k = 0; k < ni && p->nicon < NB_ICONS_MAX && size > 0.2f; ++k) {
            float *c = p->icon[p->nicon++];
            c[0] = p->x0 + L * ((float)k + 0.5f) / (float)ni; c[1] = spring - 0.5f * fh; c[2] = p->lip;
            c[3] = 0.0f; c[4] = 1.0f; c[5] = size;
        }
        return;
    }
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
}

/* The A-frame's turned footprint (half sizes, its legs splayed) at yaw, a few centimetres to spare. */
static void aframe_half(float yaw, float *hx, float *hz) {
    const float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
    *hx = 0.32f * c + 0.22f * sn + 0.04f; *hz = 0.32f * sn + 0.22f * c + 0.04f;
}
/* Whether the A-frame at (x, z, yaw) meets anything the bar stands on the ground (nb_solids: the body, the counters,
 * posts, the column, stools, the kiosk's sill) or leaves the site. */
static int aframe_hits(const NbPlan *p, float W, float D, float x, float z, float yaw) {
    float hx, hz;
    aframe_half(yaw, &hx, &hz);
    /* inside the site (its own footprint, as the clamp above), clear of the solids by the margin */
    if (x - hx + 0.04f < -0.5f * W - 1e-3f || x + hx - 0.04f > 0.5f * W + 1e-3f || z - hz + 0.04f < -1e-3f || z + hz - 0.04f > D + 1e-3f) return 1;
    NbPlan q = *p;
    q.aframeOn = 0;
    float b[NB_SOLIDS_MAX][4];
    const int n = nb_solids(&q, b, NB_SOLIDS_MAX);
    for (int i = 0; i < n; ++i)
        if (x + hx > b[i][0] && x - hx < b[i][2] && z + hz > b[i][1] && z - hz < b[i][3]) return 1;
    return 0;
}
/* SF2 (the user: A-frames clipped into stools): the A-frame where its draws put it if that's clear, else the
 * nearest clear place along the front (or along the bar's side, beside it), else none. */
static void aframe_clear(NbPlan *p, float W, float D, int beside) {
    const float x = p->aframe[0], z = p->aframe[1], yaw = p->aframe[2];
    if (!aframe_hits(p, W, D, x, z, yaw)) return;
    float hx, hz;
    aframe_half(yaw, &hx, &hz);
    /* along the front where it stands, then out at the site's front edge, beyond the stools */
    const float zs[2] = { z, D - (hz - 0.04f) - 0.01f };
    for (int r = 0; r < (beside ? 1 : 2); ++r) {
        if (r && zs[1] <= z + 0.01f) break;
        for (int k = r ? 0 : 1; k <= 8; ++k)
            for (int sg = -1; sg <= 1; sg += 2) {
                const float d = (float)sg * 0.25f * (float)k;
                const float cx = beside ? x : x + d, cz = beside ? z + d : zs[r];
                if (!aframe_hits(p, W, D, cx, cz, yaw)) { p->aframe[0] = cx; p->aframe[1] = cz; return; }
            }
    }
    p->aframeOn = 0;
}

/* The A-frame out front at the closed end; the bell on the counter near the gate (the kiosk's on its sill). */
static void plan_front(NbPlan *p, float W, float D, int re) {
    const float cy = p->cy, cd = p->cd;
    float xo = p->form == NB_CORNER ? (p->side > 0 ? p->x0 : p->x1) : (re < 0 ? p->x0 : p->x1);
    float sg = xo < 0.0f ? 1.0f : -1.0f;
    /* SF1: about two bars in three put an A-frame out: by the closed end, anywhere along the front, or beside
     * the bar where the site leaves room, never square to it (by hash: the draws stay NB5's) */
    const uint32_t ha = hash_combine(p->seed, 0xAF4Au);
    const float u0 = hash_float(ha), u1 = hash_float(hash_combine(ha, 1u)), u2 = hash_float(hash_combine(ha, 2u));
    const float u3 = hash_float(hash_combine(ha, 3u)), u4 = hash_float(hash_combine(ha, 4u));
    p->aframeOn = (uint8_t)(u0 < 0.65f);
    const float zlo = maxf(p->front + 0.9f, p->lip + 0.3f), zhi = maxf(zlo, D - 0.26f);
    const float side = 0.5f * W - (sg > 0.0f ? -xo : xo);   /* room between the closed end's wall and the site's edge */
    float yaw = 0.5f * (u3 - 0.5f) + (u4 < 0.2f ? (u4 < 0.1f ? 0.45f : -0.45f) : 0.0f);
    float lim = 0.36f;
    int beside = 0;
    if (u1 < 0.25f && side >= 0.55f && (p->form == NB_STRAIGHT || p->form == NB_HATCH || p->form == NB_ARCADE)) {
        lim = 0.24f;
        beside = 1;
        /* beside the bar, facing along the frontage */
        p->aframe[0] = xo - sg * 0.5f * side;
        p->aframe[1] = p->lining + 0.6f + (p->front - p->lining - 0.6f) * u2;
        yaw += HALF_PI;
    } else if (u1 < 0.65f) {
        p->aframe[0] = xo + sg * (0.36f + 0.4f * u2);
        p->aframe[1] = zlo + (zhi - zlo) * u3;
    } else {
        p->aframe[0] = p->x0 + 0.4f + (p->x1 - p->x0 - 0.8f) * u2;
        p->aframe[1] = zlo + (zhi - zlo) * hash_float(hash_combine(ha, 5u));
    }
    p->aframe[0] = clampf(p->aframe[0], -0.5f * W + lim, 0.5f * W - lim);
    p->aframe[1] = clampf(p->aframe[1], 0.3f, D - 0.26f);
    p->aframe[2] = yaw;
    {   /* SF2: its turned footprint inside the site (nb_solids' box) */
        const float c = fabsf(cosf(yaw)), sn = fabsf(sinf(yaw));
        const float hx = 0.32f * c + 0.22f * sn, hz = 0.32f * sn + 0.22f * c;
        p->aframe[0] = clampf(p->aframe[0], -0.5f * W + hx + 0.01f, 0.5f * W - hx - 0.01f);
        p->aframe[1] = minf(p->aframe[1], D - hz - 0.01f);
    }
    /* SF2: the small forms' sites often leave no room in front: then none (it would stand in the counter) */
    if (!beside && p->aframe[1] < p->front + 0.35f) p->aframeOn = 0;
    if (p->form == NB_HATCH) {
        /* on the sill, at the hatch's end towards the middle */
        const float bx = p->hatch[0] + p->hatch[1] < 0.0f ? p->hatch[1] - 0.12f : p->hatch[0] + 0.12f;
        p->bell[0] = bx; p->bell[1] = cy; p->bell[2] = p->front + 0.5f * p->sill;
    } else {
        const NbRun *c = &p->run[0];
        float g = c->gate1 > c->gate0 ? 0.5f * (c->gate0 + c->gate1) : 0.0f;
        float at = clampf(g < 0.5f * c->len ? g + 0.75f : g - 0.75f, 0.2f, c->len - 0.2f);
        p->bell[0] = c->o[0] + c->u[0] * at; p->bell[1] = cy; p->bell[2] = p->front - 0.45f * cd;
    }
    if (p->aframeOn) aframe_clear(p, W, D, beside);
}

/* NB6: light and signs, by hash (the draws stay NB5's) */
static void plan_lights(NbPlan *p) {
    const uint32_t hl = hash_combine(p->seed, 0x4C4954u);
    p->tubeWhite = hash_float(hash_combine(hl, 1u)) < 0.6f ? NB_WHITE_6500 : NB_WHITE_5000;
    /* the icons' neon: a pair of colours a vendor, red and cyan most (the references) */
    static const uint8_t PAIR[6][2] = { { 2, 1 }, { 2, 1 }, { 0, 1 }, { 1, 2 }, { 2, 3 }, { 0, 5 } };
    const int pair = (int)(hash_float(hash_combine(hl, 2u)) * 6.0f) % 6;
    const int first = (int)(hash_float(hash_combine(hl, 3u)) * 4.0f) & 3;
    const int bad = hash_float(hash_combine(hl, 4u)) < 0.35f ? (int)(hash_float(hash_combine(hl, 5u)) * (float)NB_ICONS_MAX) : -1;
    const int badMode = hash_float(hash_combine(hl, 6u)) < 0.35f ? 2 : 1;
    int kinds[4];
    const int nk = nb_trade_icons(p->trade, kinds);
    for (int i = 0; i < p->nicon; ++i) {
        p->iconKind[i] = (uint8_t)kinds[(first + i) % nk];
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

int nb_plan(NbPlan *p, float W, float D, float ceiling, uint32_t seed, int form, int trade, float affluence) {
    return nb_plan_site(p, W, D, ceiling, seed, form, trade, affluence, 1, 0.0f);
}

/* SF3: the deep unit's A-frame stands out on the sidewalk by the window away from the door, where the site reaches
 * out past the building line. */
static void deep_front(NbPlan *p, float W, float D) {
    const float R = p->recess;
    p->aframeOn = (uint8_t)(D - R >= 0.6f && hash_float(hash_combine(p->seed, 0xDEEAu)) < 0.65f);
    p->aframe[0] = -(float)p->doorSide * (0.5f * W - 0.75f);
    p->aframe[1] = minf(R + 0.34f, D - 0.30f);
    p->aframe[2] = 0.4f * (hash_float(hash_combine(p->seed, 0xDEEBu)) - 0.5f);
}

int nb_plan_site(NbPlan *p, float W, float D, float ceiling, uint32_t seed, int form, int trade, float affluence, int site,
                 float recess) {
    const int host = site & NB_SITE_HOST, hang = !(site & NB_SITE_VAULT);
    memset(p, 0, sizeof(*p));
    p->trade = trade >= 0 && trade < VT_COUNT ? trade : VT_NOODLE;
    p->affluence = clampf(affluence, 0.0f, 1.0f);
    Rng r = rng_make(hash_combine(seed, 0x4E42u));
    p->seed = seed; p->w = W; p->d = D;
    p->ceiling = ceiling > 0.0f ? ceiling : 0.0f;
    p->freestanding = (uint8_t)!host;
    p->recess = recess > 0.0f ? recess : 0.0f;
    const int forceSide = (site & NB_SITE_CORNER_POS) ? 1 : (site & NB_SITE_CORNER_NEG) ? -1 : 0;
    const float ceil = ceiling > 0.0f ? ceiling : 99.0f;
    /* every draw first, in order (hash.h: draws are sequenced) */
    NbDraws dr;
    dr.bw     = rng_range(&r, 3.4f, 4.4f);
    dr.bd     = rng_range(&r, 0.60f, 0.68f);
    dr.aisle  = rng_range(&r, 0.85f, 1.0f);
    dr.cd     = rng_range(&r, 0.50f, 0.55f);
    dr.cy     = rng_range(&r, 1.00f, 1.05f);
    dr.ov     = rng_range(&r, 0.65f, 0.9f);
    dr.spring = rng_range(&r, 2.45f, 2.6f);
    dr.rise   = rng_range(&r, 0.6f, 0.85f);
    dr.pitch  = rng_range(&r, 0.48f, 0.58f);
    dr.fh     = rng_range(&r, 0.30f, 0.36f);
    dr.spanC  = rng_range(&r, 1.65f, 2.0f);
    dr.pick   = rng_float(&r);
    dr.spitch = rng_range(&r, 0.64f, 0.72f);
    dr.rangeL = rng_float(&r);
    dr.sideR  = rng_float(&r);

    int re, fits;
    if (p->recess > 0.0f && forceSide) fits = plan_inset(p, &dr, W, ceil, forceSide, &re);   /* SF3 */
    else if (p->recess > 0.0f && (site & NB_SITE_DEEP)) fits = plan_deep(p, &dr, W, D, ceil, &re);
    else switch (form) {
    case NB_HATCH:  fits = plan_hatch(p, W, D, ceil, host, &re); break;
    case NB_ARCADE: fits = plan_arcade(p, &dr, W, D, ceiling, host, hang, &re); break;
    case NB_TIERED: fits = plan_tiered(p, &dr, W, D, ceil, &re); break;
    default:
        fits = p->recess > 0.0f ? plan_recessed(p, &dr, W, D, ceil, &re) : plan_bar(p, &dr, W, D, ceil, form, host, &re);
        break;
    }
    plan_kitchen(p, re, ceil);
    plan_signs(p);
    if (p->deep) deep_tubes(p);
    plan_front(p, W, D, re);
    if (p->deep) deep_front(p, W, D);
    if (p->inset) p->aframeOn = 0;   /* (nothing of it out on the sidewalks) */
    plan_lights(p);
    return fits;
}

int nb_trade_icons(int trade, int *out) {
    static const int I[VT_COUNT][5] = {
        [VT_NOODLE]   = { 4, PMK_ICON_BOWL, PMK_ICON_CHOPSTICKS, PMK_ICON_STEAM, PMK_ICON_CUP },   /* NB6's, in its order */
        [VT_SOUP]     = { 3, PMK_ICON_STEAM, PMK_ICON_BOWL, PMK_ICON_CHOPSTICKS },
        [VT_DUMPLING] = { 3, PMK_ICON_DUMPLING, PMK_ICON_CHOPSTICKS, PMK_ICON_STEAM },
        [VT_SKEWER]   = { 2, PMK_ICON_SKEWER, PMK_ICON_FLAME },
        [VT_KEBAB]    = { 2, PMK_ICON_KEBAB, PMK_ICON_FLAME },
        [VT_BURGER]   = { 2, PMK_ICON_BURGER, PMK_ICON_HOTDOG },
        [VT_FRIED]    = { 2, PMK_ICON_DRUMSTICK, PMK_ICON_FRIES },
        [VT_CURRY]    = { 2, PMK_ICON_STEAM, PMK_ICON_FLAME },
        [VT_TACO]     = { 2, PMK_ICON_TACO, PMK_ICON_FLAME },
        [VT_CREPE]    = { 2, PMK_ICON_CREPE, PMK_ICON_CUP },
        [VT_ROAST]    = { 2, PMK_ICON_CHESTNUT, PMK_ICON_FLAME },
        [VT_DRINKS]   = { 2, PMK_ICON_CUP, PMK_ICON_TEAPOT },
    };
    const int *r = I[trade >= 0 && trade < VT_COUNT ? trade : 0];
    for (int k = 0; k < r[0]; ++k) out[k] = r[1 + k];
    return r[0];
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
    if (0 && p->upper[2] > p->upper[0]) {   /* SF2: (the glazed tier's lamp: retired with its glass) */
        NbLight *l = NB_ADD(NB_LIGHT_LAMP, NB_WHITE_3000);
        if (l) { l->a[0] = 0.5f * (p->upper[0] + p->upper[2]); l->a[1] = p->upper[5] - 0.2f; l->a[2] = p->upper[3] - 0.25f; }
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
        /* the burners' blue flames; SF1: coals and a spit's radiants glow orange, the rest give no light */
        const int h = L.heat[b], gas = h == NB_HEAT_RING || h == NB_HEAT_WOK, ember = h == NB_HEAT_COALS || h == NB_HEAT_SPIT;
        if (!gas && !ember) continue;
        NbLight *l = NB_ADD(NB_LIGHT_FLAME, gas ? NB_FLAME_BLUE : NB_EMBER);
        if (!l) break;
        l->a[0] = L.burner[b][0]; l->a[2] = L.burner[b][1];
        l->a[1] = h == NB_HEAT_WOK ? WOK_FLOOR + 0.08f : h == NB_HEAT_RING ? RANGE_TRAY + 0.04f : L.heatY[b] + 0.03f;
        if (h == NB_HEAT_SPIT) l->a[2] -= 0.08f;   /* in front of its radiants, behind the cone */
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
    const float step = pm->lod == PM_LOD0 ? TOP_STEP : 2.0f * TOP_STEP;   /* SF2: coarser in a city body (LOD1) */
    const int nx = (int)ceilf((x1 - x0) / step), nz = (int)ceilf((z1 - z0) / step);
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
static void wall(Pm *pm, const NbPlan *p, const NbWall *w, int k, int kind) {
    if (w->len < 0.05f || w->h < 0.05f) return;
    const int framing = kind == NB_WALL_SIDE;
    pm_push(pm);
    frame_on(pm, w->o[0], 0.0f, w->o[1], w->u[0], w->u[1]);
    pm_push(pm);
    pm_translate(pm, 0.0f, 0.0f, -2.0f * THICK);
    pm_mat(pm, kind == NB_WALL_LINING ? PMAT_LINING : PMAT_PLATE);
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
        /* the foot rail on its brackets, its ends bent back into the front (SF2: not under a kiosk's glazing) */
        if (b - a > 0.8f && p->form != NB_HATCH) {
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
    if (L->top == NB_TOP_FLAT) {
        /* SF1: a flat stainless top for the trade's station to stand on, its back guard */
        bb(pm, x0, RANGE_TRAY, zl, x1, yt, zf + 0.03f, 0.003f);
        top_sheet(pm, x0 + 0.004f, zl + 0.004f, x1 - 0.004f, zf - 0.004f, yt + 0.001f);
        bb(pm, x0, yt, zl, x1, yt + 0.065f, zl + 0.06f, 0.004f);
        return;
    }
    bb(pm, x0, RANGE_TRAY, zl, x1, yt, bz - hw, 0.002f);
    bb(pm, x0, RANGE_TRAY, bz + hw, x1, yt, zf + 0.03f, 0.003f);
    bb(pm, x0, RANGE_TRAY, bz - hw, wx[0] - hw, yt, bz + hw, 0.002f);
    bb(pm, wx[0] + hw, RANGE_TRAY, bz - hw, wx[1] - hw, yt, bz + hw, 0.002f);
    bb(pm, wx[1] + hw, RANGE_TRAY, bz - hw, x1, yt, bz + hw, 0.002f);
    /* the wells: a tray under a ring burner; a deep well, stainless walls, under a wok burner */
    for (int b = 0; b < 2; ++b) {
        const float x = L->burner[b][0];
        const int wok = L->heat[b] == NB_HEAT_WOK;
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
    if (L->pipeY <= 0.0f) return;   /* an electric trade: no gas (SF1) */
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
    float spans[2][2];
    int nspan = 1;
    bench_span(p, L, &spans[0][0], &spans[0][1]);
    if (p->rangeMid) {   /* SF1: the short bench between the range and the side wall */
        if (L->rangeEnd < 0) { spans[1][0] = p->benchX0; spans[1][1] = L->carc0; } else { spans[1][0] = L->carc1; spans[1][1] = p->benchX1; }
        nspan = 2;
    }
    for (int sp = 0; sp < nspan; ++sp) {
    const float b0 = spans[sp][0], b1 = spans[sp][1];
    if (b1 - b0 > 0.3f) {
        pm->part = (uint8_t)(121 + 9 * sp);
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
        if (p->form == NB_ARCADE || p->form == NB_TIERED) yh = nb_ceil(p, dx, dz) + 0.01f;   /* SF2: the deck, a leg */
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
        if (!nb_duct_capped(p)) {
            /* SF2: up into a ceiling close over the deck: a flashing plate where it goes through (none in a lantern:
             * its open mouth vents there) */
            if (p->form != NB_TIERED) bb(pm, dx - 0.2f, t - 0.012f, dz - 0.2f, dx + 0.2f, t, dz + 0.2f, 0.002f);
            return;
        }
        if (p->upper[2] > p->upper[0] && dx > p->upper[0] && dx < p->upper[2] && dz > p->upper[1] && dz < p->upper[3]) {
            /* SF2: out through the upper tier's roof */
            pm_push(pm);
            pm_translate(pm, dx, p->upper[5] + 0.05f, dz);
            PmProf fu[4] = { { r + 0.004f, 0.0f, 1 }, { r + 0.07f, 0.0f, 1 }, { r + 0.012f, 0.1f, 1 }, { r + 0.004f, 0.1f, 1 } };
            pm_lathe(pm, fu, 4, 24, 0.0f, 0.0f);
            pm_pop(pm);
        }
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
    /* the tube channels behind the fascia, on brackets to it (SF3: not the deep unit's ceiling battens) */
    for (int i = 0; i < p->ntube - p->roomTubes; ++i) {
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

/* ------------------------------------------------------ the forms (SF2) --- */

/* A pane of glass in the local xy plane at z, facing +z (one face: seen from outside; the city shades it as
 * real glass over what stands behind, kit.glsl PMAT_GLAZING). */
static void pane(Pm *pm, float x0, float y0, float x1, float y1, float z) {
    if (x1 - x0 < 0.02f || y1 - y0 < 0.02f) return;
    pm->shell++;
    const uint8_t mat = pm->mat;
    pm_mat(pm, PMAT_GLAZING);
    const float n[3] = { 0.0f, 0.0f, 1.0f };
    const float q[4][3] = { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } };
    uint32_t id[4];
    for (int k = 0; k < 4; ++k) id[k] = pm_vert(pm, q[k], n, q[k][0], q[k][1]);
    pm_quad(pm, id[0], id[1], id[2], id[3]);
    pm->mat = mat;
}

/* The kiosk's front over its inner counter: the header up to the hood, the glazing in its frame with the hatch
 * open (its pane slid behind its neighbour in a track), the sill outside on its brackets. */
static void hatch_front(Pm *pm, const NbPlan *p) {
    const float z = p->front, cy = p->cy, gt = p->glazeTop, x0 = p->x0, x1 = p->x1, fw = 0.04f;
    const float hf = nb_hood_under(p, z - p->lining) - 0.004f;
    const float h0 = p->hatch[0], h1 = p->hatch[1], hw = h1 - h0;
    pm->part = 200;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, x0, gt, z - 0.03f, x1, hf, z, 0.003f);
    rivets(pm, x0 + 0.05f, gt + 0.03f, z, x1 - 0.05f, gt + 0.03f, z, 0, 0, 1);
    if (hf - gt > 0.16f) rivets(pm, x0 + 0.05f, hf - 0.035f, z, x1 - 0.05f, hf - 0.035f, z, 0, 0, 1);
    /* the frame in the trim coat: sill and head rails, jambs, mullions between the panes and either side of the
     * hatch */
    pm->part = 201;
    pm_mat(pm, PMAT_PAINTED);
    const float yb = cy + 0.035f, yt = gt - fw;
    bb(pm, x0, cy, z - fw, x1, yb, z, 0.002f);
    bb(pm, x0, yt, z - fw, x1, gt, z, 0.002f);
    float xs[16];
    int nx = 0;
    xs[nx++] = x0 + 0.5f * fw;
    const float reg[2][2] = { { x0 + 0.5f * fw, h0 }, { h1, x1 - 0.5f * fw } };
    for (int r = 0; r < 2; ++r) {
        const float len = reg[r][1] - reg[r][0];
        const int np = len > 0.1f ? (int)maxf(1.0f, floorf(len / 0.85f + 0.5f)) : 0;
        for (int k = 1; k < np && nx < 13; ++k) xs[nx++] = reg[r][0] + len * (float)k / (float)np;
        if (r == 0) { xs[nx++] = h0; xs[nx++] = h1; }
    }
    xs[nx++] = x1 - 0.5f * fw;
    for (int i = 0; i < nx; ++i) bb(pm, xs[i] - 0.5f * fw, yb, z - fw, xs[i] + 0.5f * fw, yt, z, 0.002f);
    for (int i = 0; i + 1 < nx; ++i) {
        if (xs[i] >= h0 - 1e-3f && xs[i + 1] <= h1 + 1e-3f) continue;   /* the hatch: open */
        pane(pm, xs[i] + 0.5f * fw, yb, xs[i + 1] - 0.5f * fw, yt, z - 0.02f);
    }
    /* the hatch's pane, slid behind its neighbour on the side with room, in its own thin frame; the track */
    const int right = h1 + hw <= x1 - fw;
    const float s0 = right ? h1 - 0.01f : h0 - hw + 0.01f, s1 = s0 + hw, zs = z - 0.065f;
    bb(pm, s0, yb + 0.012f, zs - 0.012f, s1, yb + 0.037f, zs + 0.012f, 0.002f);
    bb(pm, s0, yt - 0.037f, zs - 0.012f, s1, yt - 0.012f, zs + 0.012f, 0.002f);
    bb(pm, s0, yb + 0.037f, zs - 0.012f, s0 + 0.025f, yt - 0.037f, zs + 0.012f, 0.002f);
    bb(pm, s1 - 0.025f, yb + 0.037f, zs - 0.012f, s1, yt - 0.037f, zs + 0.012f, 0.002f);
    pane(pm, s0 + 0.025f, yb + 0.037f, s1 - 0.025f, yt - 0.037f, zs);
    pm_mat(pm, PMAT_STEEL);
    {
        const float a = minf(s0, h0) - 0.02f, b = maxf(s1, h1) + 0.02f;
        bb(pm, a, yb, zs - 0.02f, b, yb + 0.012f, z - fw, 0.001f);
        bb(pm, a, yt - 0.012f, zs - 0.02f, b, yt, z - fw, 0.001f);
        /* a pull on the slid pane's edge by the opening */
        const float px = right ? s0 + 0.04f : s1 - 0.06f;
        bb(pm, px, 0.5f * (yb + yt) - 0.08f, zs + 0.012f, px + 0.02f, 0.5f * (yb + yt) + 0.08f, zs + 0.03f, 0.002f);
    }
    /* the sill outside, beyond the counter top's nose, on two brackets */
    pm->part = 202;
    const float sx0 = maxf(x0 + 0.05f, minf(h0, p->bell[0]) - 0.25f), sx1 = minf(x1 - 0.05f, maxf(h1, p->bell[0]) + 0.25f);
    const float zs1 = z + p->sill;
    pm_mat(pm, PMAT_STAINLESS);
    bb(pm, sx0, cy - 0.035f, z + 0.03f, sx1, cy, zs1, 0.004f);
    top_sheet(pm, sx0 + 0.006f, z + 0.036f, sx1 - 0.006f, zs1 - 0.006f, cy + 0.001f);
    pm_mat(pm, PMAT_STEEL);
    for (int e = 0; e < 2; ++e) {
        const float bx = e ? sx1 - 0.1f : sx0 + 0.1f;
        bb(pm, bx - 0.015f, cy - 0.041f, z, bx + 0.015f, cy - 0.035f, zs1 - 0.03f, 0.001f);
        const float a[3] = { bx, cy - 0.30f, z + 0.004f }, b[3] = { bx, cy - 0.045f, zs1 - 0.05f };
        pm_beam(pm, a, b, 0.03f, 0.006f, 0.0015f, NULL);
        bb(pm, bx - 0.02f, cy - 0.34f, z, bx + 0.02f, cy - 0.26f, z + 0.006f, 0.001f);
    }
}

/* The kiosk's door in its side wall, across the cook's aisle: a framed plate leaf, two panels, a kick plate,
 * hinges and a D handle. */
static void side_door(Pm *pm, const NbPlan *p) {
    if (p->door[2] <= p->door[1]) return;
    const float x = p->door[0], L = p->door[2] - p->door[1], H = 1.95f;
    pm->part = 205;
    pm_push(pm);
    if (x > 0.0f) frame_on(pm, x, 0.0f, p->door[2], 0.0f, -1.0f);
    else          frame_on(pm, x, 0.0f, p->door[1], 0.0f, 1.0f);
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, -0.035f, 0.0f, 0.0f, 0.0f, H + 0.035f, 0.022f, 0.002f);
    bb(pm, L, 0.0f, 0.0f, L + 0.035f, H + 0.035f, 0.022f, 0.002f);
    bb(pm, 0.0f, H, 0.0f, L, H + 0.035f, 0.022f, 0.002f);
    pm_mat(pm, PMAT_PLATE);
    bb(pm, 0.006f, 0.012f, 0.004f, L - 0.006f, H - 0.006f, 0.016f, 0.003f);
    bb(pm, 0.08f, 0.24f, 0.016f, L - 0.08f, 0.88f, 0.021f, 0.002f);
    bb(pm, 0.08f, 1.04f, 0.016f, L - 0.08f, H - 0.12f, 0.021f, 0.002f);
    pm_mat(pm, PMAT_STEEL);
    bb(pm, 0.012f, 0.014f, 0.016f, L - 0.012f, 0.19f, 0.019f, 0.001f);
    for (int h = 0; h < 2; ++h) { pm_push(pm); pm_translate(pm, 0.004f, h ? H - 0.3f : 0.25f, 0.02f); pm_cyl(pm, 0.0f, 0.1f, 0.009f, 10); pm_pop(pm); }
    PmPath pa, pf;
    pm_path_init(&pa, 4);
    const float hx = L - 0.07f;
    pm_path_add(&pa, hx, 0.92f, 0.016f, 1); pm_path_add(&pa, hx, 0.92f, 0.055f, 1);
    pm_path_add(&pa, hx, 1.10f, 0.055f, 1); pm_path_add(&pa, hx, 1.10f, 0.016f, 1);
    pm_path_init(&pf, 16);
    pm_path_fillet(&pa, 0.012f, 3, &pf);
    pm_tube(pm, &pf, 0.008f, 8, 0);
    pm_path_free(&pa); pm_path_free(&pf);
    pm_pop(pm);
}

/* The arcade counter's header (a riveted box girder across the front, its face at the lip), its returns down
 * the sides over the customers, the joists under the deck, the drop rods up to the ceiling. */
static void arcade_parts(Pm *pm, const NbPlan *p) {
    const float hb = p->hood.spring - p->hood.fascia_h, dy = p->deckY, zl = p->lip, x0 = p->x0, x1 = p->x1;
    const float cz0 = p->front - p->cd;
    pm->part = 210;
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, x0, hb, zl - 0.06f, x1, dy + 0.07f, zl, 0.004f);
    rivets(pm, x0 + 0.04f, hb + 0.025f, zl, x1 - 0.04f, hb + 0.025f, zl, 0, 0, 1);
    rivets(pm, x0 + 0.04f, dy + 0.045f, zl, x1 - 0.04f, dy + 0.045f, zl, 0, 0, 1);
    pm->part = 211;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, x0, hb, cz0, x0 + 0.03f, dy, zl - 0.06f, 0.003f);
    bb(pm, x1 - 0.03f, hb, cz0, x1, dy, zl - 0.06f, 0.003f);
    /* the joists, clear of the extractor and the duct by the lining */
    pm->part = 212;
    pm_mat(pm, PMAT_STEEL);
    const float j0 = p->lining + 0.72f, j1 = zl - 0.2f;
    const int nj = j1 > j0 ? 1 + (int)((j1 - j0) / 0.8f) : 0;
    for (int k = 0; k < nj; ++k) {
        const float z = nj > 1 ? j0 + (j1 - j0) * (float)k / (float)(nj - 1) : 0.5f * (j0 + j1);
        bb(pm, x0 + 0.03f, dy - 0.05f, z - 0.02f, x1 - 0.03f, dy, z + 0.02f, 0.002f);
    }
    if (p->rodTop > dy) {
        const int nr = x1 - x0 > 3.0f ? 3 : 2;
        for (int k = 0; k < nr; ++k) {
            const float x = k == 0 ? x0 + 0.15f : k == 1 ? x1 - 0.15f : 0.5f * (x0 + x1), z = zl - 0.03f;
            if (p->rodTop - 0.012f > dy + 0.1f) { pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, dy + 0.085f, p->rodTop - 0.012f, 0.008f, 8); pm_pop(pm); }
            bb(pm, x - 0.06f, p->rodTop - 0.012f, z - 0.06f, x + 0.06f, p->rodTop, z + 0.06f, 0.002f);
            bb(pm, x - 0.03f, dy + 0.07f, z - 0.03f, x + 0.03f, dy + 0.085f, z + 0.03f, 0.001f);
        }
    }
}

/* A band of louvre blades in a face's frame (x along 0..L, y up, z out, the face at z 0): its frame of angles, the
 * blades every 75 mm tilted 40 degrees down and out so the rain runs off them. */
static void louvres(Pm *pm, float L, float y0, float y1) {
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, 0.0f, y0, -0.05f, 0.035f, y1, 0.0f, 0.002f);
    bb(pm, L - 0.035f, y0, -0.05f, L, y1, 0.0f, 0.002f);
    bb(pm, 0.0f, y0, -0.05f, L, y0 + 0.03f, 0.0f, 0.002f);
    bb(pm, 0.0f, y1 - 0.03f, -0.05f, L, y1, 0.0f, 0.002f);
    const float up[3] = { 0.0f, 0.766f, 0.643f };
    for (float y = y0 + 0.07f; y < y1 - 0.04f; y += 0.075f) {
        const float a[3] = { 0.035f, y, -0.025f }, b[3] = { L - 0.035f, y, -0.025f };
        pm_beam(pm, a, b, 0.07f, 0.005f, 0.0f, up);
    }
}

/* A face of the plant box (x along 0..L, z out): riveted plate from y0 to y1, its edges' rivet rows. */
static void plate_face(Pm *pm, float L, float y0, float y1) {
    pm_mat(pm, PMAT_PLATE);
    bb(pm, 0.0f, y0, -0.03f, L, y1, 0.0f, 0.003f);
    rivets(pm, 0.04f, y0 + 0.04f, 0.0f, L - 0.04f, y0 + 0.04f, 0.0f, 0, 0, 1);
    rivets(pm, 0.04f, y1 - 0.04f, 0.0f, L - 0.04f, y1 - 0.04f, 0.0f, 0, 0, 1);
    rivets(pm, 0.04f, y0 + 0.08f, 0.0f, 0.04f, y1 - 0.08f, 0.0f, 0, 0, 1);
    rivets(pm, L - 0.04f, y0 + 0.08f, 0.0f, L - 0.04f, y1 - 0.08f, 0.0f, 0, 0, 1);
}

/* An access door in a face's frame at x a..b, y0..y1: a framed plate leaf, two hinges, a turn handle, a warning
 * plate. */
static void access_door(Pm *pm, float a, float b, float y0, float y1) {
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, a - 0.025f, y0 - 0.025f, 0.0f, b + 0.025f, y0, 0.018f, 0.002f);
    bb(pm, a - 0.025f, y1, 0.0f, b + 0.025f, y1 + 0.025f, 0.018f, 0.002f);
    bb(pm, a - 0.025f, y0, 0.0f, a, y1, 0.018f, 0.002f);
    bb(pm, b, y0, 0.0f, b + 0.025f, y1, 0.018f, 0.002f);
    pm_mat(pm, PMAT_PLATE);
    bb(pm, a + 0.004f, y0 + 0.004f, 0.002f, b - 0.004f, y1 - 0.004f, 0.014f, 0.002f);
    pm_mat(pm, PMAT_STEEL);
    for (int h = 0; h < 2; ++h) { pm_push(pm); pm_translate(pm, a + 0.004f, h ? y1 - 0.18f : y0 + 0.08f, 0.018f); pm_cyl(pm, 0.0f, 0.1f, 0.008f, 8); pm_pop(pm); }
    const float hy = 0.5f * (y0 + y1);
    bb(pm, b - 0.07f, hy - 0.06f, 0.014f, b - 0.045f, hy + 0.06f, 0.03f, 0.002f);
    pm_mat(pm, PMAT_PAPER);
    bb(pm, 0.5f * (a + b) - 0.07f, y1 - 0.2f, 0.014f, 0.5f * (a + b) + 0.07f, y1 - 0.08f, 0.016f, 0.0f);
}

/* The tiered kiosk's upper tier on the deck round the column (SF2; the user asked for variations, NB_UPPER_*):
 * built face by face (front +z, the sides, the back behind the column), its roof over them. */
static void tiered_upper(Pm *pm, const NbPlan *p) {
    const float *u = p->upper;
    if (u[2] <= u[0]) return;
    const float X0 = u[0], Z0 = u[1], X1 = u[2], Z1 = u[3], Y0 = u[4], Y1 = u[5];
    const float W = X1 - X0, Dp = Z1 - Z0;
    const int kind = p->upperKind;
    /* the faces' frames: origin, along (ux, uz), length; each faces cross(along, up): out of the tier */
    const float F[4][5] = { { X0, Z1, 1.0f, 0.0f, W }, { X1, Z1, 0.0f, -1.0f, Dp }, { X1, Z0, -1.0f, 0.0f, W }, { X0, Z0, 0.0f, 1.0f, Dp } };
    const float split = kind == NB_UPPER_BOTH ? Y0 + 0.55f : Y0;   /* the solid band's top (both) */
    pm->part = 220;
    for (int f = 0; f < 4; ++f) {
        pm_push(pm);
        frame_on(pm, F[f][0], 0.0f, F[f][1], F[f][2], F[f][3]);
        const float L = F[f][4];
        if (kind == NB_UPPER_LANTERN) louvres(pm, L, Y0 + 0.02f, Y1 - 0.02f);
        else if (kind == NB_UPPER_PLANT) {
            plate_face(pm, L, Y0, Y1);
            if (f == 0) access_door(pm, 0.5f * L - 0.32f, 0.5f * L + 0.32f, Y0 + 0.08f, Y1 - 0.1f);
            if (f == 1 || f == 3) {   /* a small vent grille, proud of the plate, its slats */
                pm_push(pm); pm_translate(pm, 0.5f * L - 0.22f, 0.0f, 0.045f); louvres(pm, 0.44f, Y1 - 0.48f, Y1 - 0.14f); pm_pop(pm);
            }
        } else {
            plate_face(pm, L, Y0, split);
            louvres(pm, L, split, Y1 - 0.02f);
            if (f == 0) access_door(pm, 0.5f * L - 0.3f, 0.5f * L + 0.3f, Y0 + 0.06f, split - 0.06f);
        }
        pm_pop(pm);
    }
    /* the corner angles over the faces' ends */
    pm_mat(pm, PMAT_PAINTED);
    const float cx[2] = { X0, X1 }, cz[2] = { Z0, Z1 };
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            bb(pm, cx[i] - 0.03f, Y0, cz[j] - 0.03f, cx[i] + 0.03f, Y1, cz[j] + 0.03f, 0.003f);
    /* the roof: an overhanging cap (the lantern's wider, to keep the rain off its slats), a lip round it */
    pm->part = 221;
    const float ov = kind == NB_UPPER_LANTERN ? 0.18f : 0.08f;
    pm_mat(pm, kind == NB_UPPER_PLANT ? PMAT_PLATE : PMAT_RUST);
    bb(pm, X0 - ov, Y1, Z0 - 0.02f, X1 + ov, Y1 + 0.04f, Z1 + ov, 0.006f);
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, X0 - ov - 0.01f, Y1 - 0.05f, Z1 + ov - 0.01f, X1 + ov + 0.01f, Y1 + 0.04f, Z1 + ov + 0.01f, 0.003f);
    if (kind == NB_UPPER_LANTERN) {
        /* a low ridge over the cap: the lantern's own small roof */
        pm_mat(pm, PMAT_RUST);
        bb(pm, X0 + 0.15f, Y1 + 0.04f, Z0 + 0.1f, X1 - 0.15f, Y1 + 0.1f, Z1 - 0.1f, 0.01f);
    } else {
        /* the plant box's conduit: down its side by the back from under the roof to the deck, in saddles */
        pm_mat(pm, PMAT_GALV);
        const float x = X1 + 0.025f, z = Z0 + 0.15f;
        pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, Y0 - 0.02f, Y1 - 0.06f, 0.016f, 10); pm_pop(pm);
        for (float y = Y0 + 0.2f; y < Y1 - 0.1f; y += 0.35f) bb(pm, x - 0.025f, y, z - 0.03f, x + 0.004f, y + 0.02f, z + 0.03f, 0.0f);
    }
}

/* What one form has that the others haven't. */
/* SF3: a shop bay's stall closes its opening: the soffit over the recess at the head, the header panel in the
 * facade's plane from the hood up to it (riveted along its edges), and a painted angle frame round the opening,
 * standing just proud of the facade. */
static void recess_parts(Pm *pm, const NbPlan *p) {
    const float R = p->recess, H = p->head, x0 = p->x0, x1 = p->x1;
    const float hy = nb_hood_under(p, R - p->lining);
    pm->part = 240;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, x0 + 0.04f, H - 0.03f, 0.0f, x1 - 0.04f, H, R - 0.03f, 0.002f);
    pm->part = 241;
    pm_mat(pm, PMAT_PAINTED);
    if (H - hy > 0.06f) {
        bb(pm, x0 + 0.04f, hy, R - 0.03f, x1 - 0.04f, H, R, 0.003f);
        rivets(pm, x0 + 0.1f, H - 0.06f, R, x1 - 0.1f, H - 0.06f, R, 0, 0, 1);
        if (H - hy > 0.3f) rivets(pm, x0 + 0.1f, hy + 0.12f, R, x1 - 0.1f, hy + 0.12f, R, 0, 0, 1);
    }
    pm->part = 242;
    bb(pm, x0, 0.0f, R, x0 + 0.06f, H, R + 0.025f, 0.003f);
    bb(pm, x1 - 0.06f, 0.0f, R, x1, H, R + 0.025f, 0.003f);
    bb(pm, x0 + 0.06f, H - 0.08f, R, x1 - 0.06f, H, R + 0.025f, 0.003f);
}

/* SF3: the inset corner unit closes its two openings: the soffit over the whole room at the head; the header panels
 * in both faces' planes from just over the hood's springing up to it (riveted; the shop's boards on them); the corner's
 * pier, its inner faces in the trim coat (its outer faces are the building's), a centimetre behind the facade; a
 * painted angle frame round each opening, standing just proud of the facade; the customers' floor (tiles) in an L
 * round the counter, out to the building lines. */
static void inset_parts(Pm *pm, const NbPlan *p) {
    const int s = p->inset;
    const float sf = (float)s, R = p->recess, H = p->head, P = INSET_PIER, hy = p->panel, lin = p->lining;
    const float xB = sf * (0.5f * p->w - 0.05f), xo = s > 0 ? p->x0 : p->x1, xp = xB - sf * P;
    const float xa = minf(xo, xB), xb = maxf(xo, xB);          /* the room across */
    const float fa = minf(xo, xp), fb = maxf(xo, xp);          /* F's opening */
    const float sa = minf(xB - sf * 0.005f, xB - sf * 0.035f), sb = maxf(xB - sf * 0.005f, xB - sf * 0.035f);   /* S's panel, through x
                                                                                                          * (5 mm behind the facade) */
    pm->part = 240;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, xa + 0.03f, H - 0.03f, 0.0f, xb - 0.03f, H, R - 0.03f, 0.002f);
    pm->part = 241;
    pm_mat(pm, PMAT_PAINTED);
    if (H - hy > 0.06f) {
        bb(pm, fa + (s > 0 ? 0.04f : 0.0f), hy, R - 0.035f, fb - (s > 0 ? 0.0f : 0.04f), H, R - 0.005f, 0.003f);
        rivets(pm, fa + 0.1f, H - 0.06f, R, fb - 0.1f, H - 0.06f, R, 0, 0, 1);
        bb(pm, sa, hy, lin, sb, H, R - P, 0.003f);
        rivets(pm, xB, H - 0.06f, lin + 0.1f, xB, H - 0.06f, R - P - 0.1f, sf, 0, 0);
        if (H - hy > 0.3f) {
            rivets(pm, fa + 0.1f, hy + 0.12f, R, fb - 0.1f, hy + 0.12f, R, 0, 0, 1);
            rivets(pm, xB, hy + 0.12f, lin + 0.1f, xB, hy + 0.12f, R - P - 0.1f, sf, 0, 0);
        }
    }
    /* the pier */
    bb(pm, minf(xp, xB - sf * 0.01f), 0.0f, R - P, maxf(xp, xB - sf * 0.01f), H, R - 0.01f, 0.003f);
    /* the frames: F's jambs (the closed end's, the pier's) and head; S's jambs (the pier's, the lining's) and head */
    pm->part = 242;
    const float j0 = s > 0 ? xo : xo - 0.06f, j1 = s > 0 ? xp - 0.06f : xp;
    bb(pm, j0, 0.0f, R, j0 + 0.06f, H, R + 0.025f, 0.003f);
    bb(pm, j1, 0.0f, R, j1 + 0.06f, H, R + 0.025f, 0.003f);
    bb(pm, fa + 0.06f, H - 0.08f, R, fb - 0.06f, H, R + 0.025f, 0.003f);
    const float ox0 = minf(xB, xB + sf * 0.025f), ox1 = maxf(xB, xB + sf * 0.025f);
    bb(pm, ox0, 0.0f, R - P - 0.06f, ox1, H, R - P, 0.003f);
    bb(pm, ox0, 0.0f, lin, ox1, H, lin + 0.06f, 0.003f);
    bb(pm, ox0, H - 0.08f, lin + 0.06f, ox1, H, R - P - 0.06f, 0.003f);
    /* the floor round the counter: along F's line, then down the side to the lining */
    pm->part = 243;
    pm_mat(pm, PMAT_TILE);
    const float xc = p->run[1].o[0];
    bb(pm, xa, -0.02f, p->front, xb, 0.0f, R, 0.0f);
    bb(pm, minf(xc, xB), -0.02f, lin, maxf(xc, xB), 0.0f, p->front, 0.0f);
}

static void far_window(Pm *pm, float x0, float y0, float x1, float y1, float z);
/* SF3: the deep unit's awning: its drop at the front and its sheet's frame (o the back edge's -x end at the head,
 * ex down the slope, ey across, ez up out of it). */
static float deep_awning(const NbPlan *p, float o[3], float ex[3], float ey[3], float ez[3], float *L) {
    const float R = p->recess, H = p->head, ad = p->awning - 0.02f;
    const float drop = minf(0.30f + 0.15f * hash_float(hash_combine(p->seed, 0xA7A1u)), H - 2.6f);
    *L = sqrtf(ad * ad + drop * drop);
    o[0] = p->roomX0 - DEEP_WALL + 0.04f; o[1] = H - 0.03f; o[2] = R + 0.02f;
    ex[0] = 0.0f; ex[1] = -drop / *L; ex[2] = ad / *L;
    ey[0] = 1.0f; ey[1] = 0.0f; ey[2] = 0.0f;
    ez[0] = 0.0f; ez[1] = ad / *L; ez[2] = drop / *L;
    return drop;
}

/* SF3: the deep unit's room (its walls tiled to the dado and painted over it in the body's coat, the floor's tiles
 * round the kitchen's chequer plate, the ceiling in the trim coat with its battens), its shopfront on the building
 * line (the frame in the trim coat: jambs over the room walls' ends, the head, mullions on the bay's thirds, the
 * door's own mullion and transom; a stall riser and sill under the windows; the glass; the door: a glazed leaf, its
 * pull handles, a kick plate, the threshold), the window ledge on its brackets, and the awning. The tables and seats
 * are set down with the stools (nb_body). */
static void deep_parts(Pm *pm, const NbPlan *p) {
    const float R = p->recess, H = p->head, rx0 = p->roomX0, rx1 = p->roomX1, OX0 = rx0 - DEEP_WALL, OX1 = rx1 + DEEP_WALL;
    const float zf = R - DEEP_FRAME, dado = 1.2f, sk = 0.004f;   /* the tiles stand 4 mm proud of the paint */
    const int ds = p->doorSide;
    const float d0 = p->shopDoor[0], d1 = p->shopDoor[1];
    /* the walls: the coat full height, the tiles over it to the dado */
    pm->part = 250;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, OX0, 0.0f, 0.0f, rx0 - sk, H, zf, 0.0f);
    bb(pm, rx1 + sk, 0.0f, 0.0f, OX1, H, zf, 0.0f);
    bb(pm, rx0 - sk, 0.0f, 0.0f, rx1 + sk, H, 0.04f - sk, 0.0f);
    pm_mat(pm, PMAT_TILE);
    bb(pm, rx0 - sk, 0.0f, 0.04f, rx0, dado, zf, 0.0f);
    bb(pm, rx1, 0.0f, 0.04f, rx1 + sk, dado, zf, 0.0f);
    bb(pm, rx0, 0.0f, 0.04f - sk, rx1, dado, 0.04f, 0.0f);
    /* the floor round the kitchen's plate: the room in front of it, the passage beside it, the strips round it */
    {
        const float kz = p->kz1 + 0.04f, px0 = ds > 0 ? p->x1 : rx0, px1 = ds > 0 ? rx1 : p->x0;
        const float r[5][4] = { { rx0, kz, rx1, zf }, { px0, 0.04f, px1, kz }, { p->x0, 0.04f, p->kx0, kz },
                                { p->kx1, 0.04f, p->x1, kz }, { p->kx0, 0.04f, p->kx1, p->lining } };
        for (int k = 0; k < 5; ++k)
            if (r[k][2] - r[k][0] > 0.005f && r[k][3] - r[k][1] > 0.005f) bb(pm, r[k][0], -0.02f, r[k][1], r[k][2], 0.0f, r[k][3], 0.0f);
    }
    pm->part = 251;
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, OX0, H - 0.03f, 0.0f, OX1, H, zf, 0.0f);
    /* the battens on the ceiling (the plan's last tubes) */
    for (int i = p->ntube - p->roomTubes; i < p->ntube; ++i) {
        const float *t = p->tube[i], z = t[2];
        pm_mat(pm, PMAT_STEEL);
        bb(pm, t[0] - 0.06f, H - 0.03f - 0.045f, z - 0.065f, t[3] + 0.06f, H - 0.03f, z + 0.065f, 0.003f);
        pm_mat(pm, PMAT_TUBE);
        bb(pm, t[0], t[1] - 0.012f, z - 0.045f, t[3], H - 0.075f, z + 0.045f, 0.004f);
    }
    /* the shopfront's frame: the jambs, the head, the mullions on the thirds, the door's mullion and transom */
    pm->part = 252;
    pm_mat(pm, PMAT_PAINTED);
    const float fx0 = rx0 + 0.02f, fx1 = rx1 - 0.02f, yh = H - 0.10f, ys = 0.50f, yt = 2.15f;
    bb(pm, OX0, 0.0f, zf, fx0, H, R, 0.003f);
    bb(pm, fx1, 0.0f, zf, OX1, H, R, 0.003f);
    bb(pm, fx0, yh, zf, fx1, H, R, 0.003f);
    const float dm = ds > 0 ? d0 - 0.05f : d1;   /* the door's mullion: its -x edge */
    const float mx[3] = { -1.025f, 0.975f, dm };
    for (int k = 0; k < 3; ++k) bb(pm, mx[k], k == 2 ? 0.0f : ys, zf + 0.02f, mx[k] + 0.05f, yh, R - 0.02f, 0.002f);
    const float tx0 = ds > 0 ? dm + 0.05f : fx0, tx1 = ds > 0 ? fx1 : dm;   /* the door's bay */
    bb(pm, tx0, yt, zf + 0.02f, tx1, yt + 0.06f, R - 0.02f, 0.002f);
    /* the riser and sill under the windows (not the door's bay) */
    const float wx0 = ds > 0 ? fx0 : dm + 0.05f, wx1 = ds > 0 ? dm : fx1;
    pm_mat(pm, PMAT_PLATE);
    bb(pm, wx0, 0.0f, zf + 0.01f, wx1, ys - 0.05f, R - 0.005f, 0.003f);
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, wx0, ys - 0.05f, zf, wx1, ys, R + 0.03f, 0.004f);
    /* the glass: the windows between the mullions, the door's fanlight */
    const float zg = R - 0.05f;
    {
        float e[6];
        int ne = 0;
        e[ne++] = wx0;
        for (int k = 0; k < 2; ++k) if (mx[k] > wx0 && mx[k] < wx1) { e[ne++] = mx[k]; e[ne++] = mx[k] + 0.05f; }
        e[ne++] = wx1;
        for (int k = 0; k + 1 < ne; k += 2) pane(pm, e[k], ys, e[k + 1], yh, zg);
    }
    pane(pm, tx0, yt + 0.06f, tx1, yh, zg);
    /* the door: a glazed leaf in its frame, handles both sides at its lock stile, a kick plate; the threshold */
    pm->part = 253;
    {
        const float a = tx0, b = tx1, z0 = zf + 0.025f, z1 = R - 0.025f, st = 0.075f;
        pm_mat(pm, PMAT_PAINTED);
        bb(pm, a, 0.0f, z0, a + st, yt - 0.005f, z1, 0.003f);
        bb(pm, b - st, 0.0f, z0, b, yt - 0.005f, z1, 0.003f);
        bb(pm, a + st, yt - 0.085f, z0, b - st, yt - 0.005f, z1, 0.003f);
        bb(pm, a + st, 0.0f, z0, b - st, 0.24f, z1, 0.003f);
        pane(pm, a + st, 0.24f, b - st, yt - 0.085f, zg);
        pm_mat(pm, PMAT_STAINLESS);
        bb(pm, a + st, 0.02f, z1, b - st, 0.22f, z1 + 0.003f, 0.001f);
        const float hx = ds > 0 ? a + 0.11f : b - 0.11f;   /* the lock stile: away from the hinges at the jamb */
        for (int sd = 0; sd < 2; ++sd) {
            const float zz = sd ? z1 + 0.05f : z0 - 0.05f, zb = sd ? z1 : z0;
            pm_push(pm); pm_translate(pm, hx, 0.0f, zz); pm_cyl(pm, 0.86f, 1.36f, 0.014f, 12); pm_pop(pm);
            for (int k = 0; k < 2; ++k) {
                const float y = k ? 1.30f : 0.92f;
                bb(pm, hx - 0.008f, y - 0.008f, minf(zz, zb), hx + 0.008f, y + 0.008f, maxf(zz, zb), 0.0f);
            }
        }
        bb(pm, a, 0.0f, zf, b, 0.008f, R + 0.04f, 0.002f);
    }
    /* the window ledge (timber) on steel brackets */
    pm->part = 254;
    {
        const float l0 = p->ledge[0] + 0.01f, l1 = p->ledge[1] - 0.01f, zl = p->ledge[2];
        pm_mat(pm, PMAT_TIMBER);
        bb(pm, l0, 1.02f, zl, l1, 1.05f, zf, 0.004f);
        pm_mat(pm, PMAT_STEEL);
        const int nbk = 2 + (int)((l1 - l0) / 1.4f);
        for (int k = 0; k < nbk; ++k) {
            const float x = l0 + 0.12f + (l1 - l0 - 0.24f) * (float)k / (float)(nbk - 1);
            bb(pm, x - 0.006f, 0.80f, zf - 0.02f, x + 0.006f, 1.02f, zf, 0.0f);
            const float a[3] = { x, 0.84f, zf - 0.01f }, b[3] = { x, 1.015f, zl + 0.04f };
            pm_beam(pm, a, b, 0.012f, 0.02f, 0.001f, NULL);
        }
    }
    /* the awning: the hood's metal sloping out from the head, a valance in the trim coat, cheeks, two arms */
    if (p->awning > 0.0f) {
        pm->part = 255;
        float o[3], ex[3], ey[3], ez[3], L;
        const float drop = deep_awning(p, o, ex, ey, ez, &L), W = OX1 - OX0 - 0.08f;
        const float yf = o[1] - drop, zfr = R + p->awning;
        pm_mat(pm, PMAT_RUST);
        pm_push(pm);
        pm_frame(pm, o, ex, ey, ez);
        pm_sheet_wall(pm, L, W, 1.1f, THICK, 1);
        pm_pop(pm);
        pm_mat(pm, PMAT_PAINTED);
        bb(pm, o[0], yf - 0.20f, zfr - 0.03f, o[0] + W, yf + 0.03f, zfr, 0.003f);
        bb(pm, o[0], o[1] - 0.06f, R, o[0] + W, o[1], R + 0.05f, 0.003f);
        for (int e = 0; e < 2; ++e) {
            const float poly[3][2] = { { R + 0.02f, yf }, { zfr - 0.03f, yf }, { R + 0.02f, o[1] } };
            const float oo[3] = { e ? o[0] + W : o[0] + 0.015f, 0.0f, 0.0f }, ax[3] = { 0, 0, 1 }, ay[3] = { 0, 1, 0 }, az[3] = { -1, 0, 0 };
            pm_push(pm);
            pm_frame(pm, oo, ax, ay, az);
            pm_extrude(pm, poly, 3, 0.0f, 0.015f);
            pm_pop(pm);
        }
        pm_mat(pm, PMAT_STEEL);
        for (int e = 0; e < 2; ++e) {
            const float x = e ? o[0] + W - 0.08f : o[0] + 0.08f;
            const float a[3] = { x, yf - 0.55f, R + 0.03f }, b[3] = { x, yf - 0.02f, zfr - 0.08f };
            pm_beam(pm, a, b, 0.03f, 0.03f, 0.003f, NULL);
            bb(pm, x - 0.04f, yf - 0.62f, R, x + 0.04f, yf - 0.48f, R + 0.012f, 0.002f);
        }
    }
}

/* SF3: the deep unit's far shell: its shopfront, the windows one lit far window over the room, the awning. */
static void shell_deep(Pm *pm, const NbPlan *p) {
    const float R = p->recess, H = p->head, rx0 = p->roomX0, rx1 = p->roomX1, OX0 = rx0 - DEEP_WALL, OX1 = rx1 + DEEP_WALL;
    const float zf = R - DEEP_FRAME, yh = H - 0.10f;
    pm->part = 0;
    pm_mat(pm, PMAT_PAINTED);
    pm_box(pm, OX0, 0.0f, zf, rx0 + 0.02f, H, R);
    pm_box(pm, rx1 - 0.02f, 0.0f, zf, OX1, H, R);
    pm_box(pm, rx0 + 0.02f, yh, zf, rx1 - 0.02f, H, R);
    const float dm = p->doorSide > 0 ? p->shopDoor[0] - 0.05f : p->shopDoor[1];
    const float wx0 = p->doorSide > 0 ? rx0 + 0.02f : dm + 0.05f, wx1 = p->doorSide > 0 ? dm : rx1 - 0.02f;
    pm_box(pm, -1.025f, 0.5f, R - 0.06f, -0.975f, yh, R - 0.02f);
    pm_box(pm, 0.975f, 0.5f, R - 0.06f, 1.025f, yh, R - 0.02f);
    pm_box(pm, dm, 0.0f, R - 0.06f, dm + 0.05f, yh, R - 0.02f);
    pm_mat(pm, PMAT_PLATE);
    pm_box(pm, wx0, 0.0f, zf, wx1, 0.5f, R);
    far_window(pm, rx0 + 0.02f, 0.5f, rx1 - 0.02f, yh, R - 0.05f);
    far_window(pm, p->doorSide > 0 ? dm + 0.05f : rx0 + 0.02f, 0.0f, p->doorSide > 0 ? rx1 - 0.02f : dm, 0.5f, R - 0.05f);
    if (p->awning > 0.0f) {
        float o[3], ex[3], ey[3], ez[3], L;
        const float drop = deep_awning(p, o, ex, ey, ez, &L), W = OX1 - OX0 - 0.08f;
        const float a[3] = { o[0] + 0.5f * W, o[1] - 0.005f, o[2] }, b[3] = { o[0] + 0.5f * W, o[1] - drop - 0.005f, R + p->awning };
        pm_mat(pm, PMAT_RUST);
        pm_beam(pm, a, b, W, 0.01f, 0.0f, ez);
        pm_mat(pm, PMAT_PAINTED);
        pm_box(pm, o[0], o[1] - drop - 0.20f, R + p->awning - 0.03f, o[0] + W, o[1] - drop + 0.03f, R + p->awning);
    }
}

static void form_parts(Pm *pm, const NbPlan *p) {
    if (p->deep) deep_parts(pm, p);
    else if (p->inset) inset_parts(pm, p);
    else if (p->recess > 0.0f) recess_parts(pm, p);
    if (p->form == NB_HATCH) { hatch_front(pm, p); side_door(pm, p); }
    if (p->form == NB_ARCADE) arcade_parts(pm, p);
    if (p->form == NB_TIERED) tiered_upper(pm, p);
    if (p->freestanding && (p->form == NB_STRAIGHT || p->form == NB_HATCH)) {
        /* the cap over the slot between the clad back and the lining, under the hood's spine */
        pm->part = 230;
        pm_mat(pm, PMAT_PAINTED);
        bb(pm, p->x0 - 0.005f, p->hoodTop - 0.002f, -0.005f, p->x1 + 0.005f, p->hoodTop + 0.012f, p->lining + 0.02f, 0.003f);
    }
}

void nb_body(Pm *pm, const NbPlan *p) {
    /* long straight runs split at 0.6 m (not the modules' 0.12 m): enough for the baked occlusion along a
     * counter or a bench, and a sixth of the triangles */
    const float maxlen = pm->maxlen, support = pm->support;
    if (pm->maxlen > 0.0f) pm->maxlen = 0.6f;
    pm->support = 0.0f;   /* the chamfers keep the wear at the edges (the modules keep their loops) */
    pm->part = 0;
    if (p->hood.nspine >= 2) pm_hood(pm, &p->hood);
    hood_fittings(pm, p);
    deck(pm, p);
    for (int i = 0; i < p->nwall; ++i) wall(pm, p, &p->wall[i], i, p->wallKind[i]);
    form_parts(pm, p);
    for (int i = 0; i < p->npost; ++i) { pm->part = (uint8_t)(100 + i); post(pm, p->post[i][0], p->post[i][1], p->post[i][2]); }
    column(pm, p);
    for (int i = 0; i < p->nrun; ++i) counter_run(pm, p, &p->run[i], i);
    NbLine line;
    nb_line(p, &line);
    kitchen(pm, p, &line);
    /* the stools, the bell and the A-frame (SF4: shown while open; the stools go up on the counter when shut,
     * nb_shut_place, but a deep unit's at its ledge and a hatch kiosk's stay out) */
    const uint8_t show = pm->show;
    if (!p->deep && p->form != NB_HATCH) pm->show = NB_SHOW_OPEN;
    for (int i = 0; i < p->nstool; ++i) { pm->part = (uint8_t)(170 + i); put(pm, p->stool[i][0], 0.0f, p->stool[i][1], 0.0f, PMK_STOOL); }
    pm->show = show;
    /* SF3: the deep unit's tables, each with chopsticks and a jar of chilli oil, and their low stools */
    for (int i = 0; i < p->ntable; ++i) {
        const float x = p->table[i][0], z = p->table[i][1], yaw = 0.3f * (hash_float(hash_combine(p->seed, 0x7AB0u + (uint32_t)i)) - 0.5f);
        pm->part = (uint8_t)(220 + i);
        put(pm, x, 0.0f, z, 0.0f, PMK_CAFE_TABLE);
        put(pm, x - 0.13f, 0.74f, z + 0.09f, 0.0f, PMK_CHOPSTICKS);
        put(pm, x + 0.12f, 0.74f, z - 0.08f, yaw, PMK_JAR_OIL);
    }
    for (int i = 0; i < p->nseat; ++i) {
        pm->part = (uint8_t)(226 + (i & 3));
        put(pm, p->seat[i][0], 0.0f, p->seat[i][1], 0.5f * hash_float(hash_combine(p->seed, 0x5EA7u + (uint32_t)i)), PMK_LOW_STOOL);
    }
    pm->show = NB_SHOW_OPEN;
    pm->part = 190;
    put(pm, p->bell[0], p->bell[1], p->bell[2], 0.0f, PMK_BELL);
    pm->part = 191;
    if (p->aframeOn) put(pm, p->aframe[0], 0.0f, p->aframe[1], p->aframe[2], PMK_A_FRAME);
    pm->show = show;
    pm->maxlen = maxlen; pm->support = support;
}

/* ------------------------------------------------------------ far shell --- */

/* A far window: the kitchen's light behind the glass, in the local xy plane at z facing +z. */
static void far_window(Pm *pm, float x0, float y0, float x1, float y1, float z) {
    if (x1 - x0 < 0.05f || y1 - y0 < 0.05f) return;
    pm->shell++;
    pm_mat(pm, PMAT_WINDOW_FAR);
    const float n[3] = { 0.0f, 0.0f, 1.0f };
    const float q[4][3] = { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } };
    uint32_t id[4];
    for (int k = 0; k < 4; ++k) id[k] = pm_vert(pm, q[k], n, q[k][0] - x0, q[k][1] - y0);
    pm_quad(pm, id[0], id[1], id[2], id[3]);
}

/* The far shell's form parts (SF2): the kiosk's header, lit window and sill; the arcade's header and returns;
 * the tiered kiosk's upper tier with its lit band. */
static void shell_form(Pm *pm, const NbPlan *p) {
    if (p->inset) {   /* SF3: the soffit, the header panels, the pier */
        const int s = p->inset;
        const float sf = (float)s, R = p->recess, H = p->head, P = INSET_PIER, hy = p->panel;
        const float xB = sf * (0.5f * p->w - 0.05f), xo = s > 0 ? p->x0 : p->x1, xp = xB - sf * P;
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, minf(xo, xB - sf * 0.01f), H - 0.03f, 0.0f, maxf(xo, xB - sf * 0.01f), H, R - 0.03f);
        pm_mat(pm, PMAT_PAINTED);
        if (H - hy > 0.06f) {
            pm_box(pm, minf(xo, xp), hy, R - 0.035f, maxf(xo, xp), H, R - 0.005f);
            pm_box(pm, minf(xB - sf * 0.005f, xB - sf * 0.035f), hy, p->lining, maxf(xB - sf * 0.005f, xB - sf * 0.035f), H, R - P);
        }
        pm_box(pm, minf(xp, xB - sf * 0.01f), 0.0f, R - P, maxf(xp, xB - sf * 0.01f), H, R - 0.01f);
    } else if (p->recess > 0.0f) {   /* SF3: the soffit, the header and the frame (the opening is in the base layer) */
        const float R = p->recess, H = p->head, hy = nb_hood_under(p, R - p->lining);
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, p->x0, H - 0.03f, 0.0f, p->x1, H, R - 0.03f);
        pm_mat(pm, PMAT_PAINTED);
        if (H - hy > 0.06f) pm_box(pm, p->x0, hy, R - 0.03f, p->x1, H, R);
        pm_box(pm, p->x0, 0.0f, R, p->x0 + 0.06f, H, R + 0.025f);
        pm_box(pm, p->x1 - 0.06f, 0.0f, R, p->x1, H, R + 0.025f);
        pm_box(pm, p->x0 + 0.06f, H - 0.08f, R, p->x1 - 0.06f, H, R + 0.025f);
    }
    if (p->form == NB_HATCH) {
        const float z = p->front, hf = nb_hood_under(p, z - p->lining);
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, p->x0, p->glazeTop, z - 0.03f, p->x1, hf, z);
        far_window(pm, p->x0, p->cy, p->x1, p->glazeTop, z - 0.01f);
        pm_mat(pm, PMAT_STAINLESS);
        pm_box(pm, maxf(p->x0 + 0.05f, p->hatch[0] - 0.25f), p->cy - 0.035f, z + 0.03f, minf(p->x1 - 0.05f, p->hatch[1] + 0.25f), p->cy, z + p->sill);
    }
    if (p->form == NB_ARCADE) {
        const float hb = p->hood.spring - p->hood.fascia_h;
        pm_mat(pm, PMAT_PAINTED);
        pm_box(pm, p->x0, hb, p->lip - 0.06f, p->x1, p->deckY + 0.07f, p->lip);
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, p->x0, hb, p->front - p->cd, p->x0 + 0.03f, p->deckY, p->lip - 0.06f);
        pm_box(pm, p->x1 - 0.03f, hb, p->front - p->cd, p->x1, p->deckY, p->lip - 0.06f);
    }
    if (p->form == NB_TIERED && p->upper[2] > p->upper[0]) {
        const float *u = p->upper, g0 = u[4] + 0.45f, g1 = u[5] - 0.28f;
        pm_mat(pm, PMAT_PLATE);
        pm_box(pm, u[0], u[4], u[1], u[2], u[5], u[3]);
        pm_mat(pm, PMAT_PAINTED);
        pm_box(pm, u[0] - 0.12f, u[5], u[1] - 0.02f, u[2] + 0.12f, u[5] + 0.06f, u[3] + 0.12f);
        (void)g0; (void)g1;
    }
}

void nb_shell(Pm *pm, const NbPlan *p) {
    const PmHood *h = &p->hood;
    pm->part = 0;
    if (p->deep) { shell_deep(pm, p); return; }   /* SF3: nothing of the bar shows through its far window */
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
    if (h->nspine >= 2) {
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
    if (h->nspine >= 2) {
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
        if (nb_duct_capped(p)) { pm_push(pm); pm_translate(pm, p->ductX, p->ductTop, p->ductZ); pm_lathe(pm, cap, 3, 6, 0.0f, 0.0f); pm_pop(pm); }
    }
    shell_form(pm, p);
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
    if (p->form == NB_ARCADE) return p->deckY - 0.01f;
    if (p->form == NB_TIERED) {
        /* the deck inside the U's spine, the legs' sheet out from it */
        out = maxf(0.0f, maxf(h->spine[0][0] - x, maxf(x - h->spine[2][0], z - h->spine[1][1])));
    } else if (p->form == NB_CORNER) {
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
    /* SF1 */
    case PMK_SKEWER_TRAY: case PMK_FRIED_TRAY: return 0.172f;
    case PMK_DUMPLING_TRAY: return 0.182f;
    case PMK_BREAD_STACK: return 0.113f;
    case PMK_PAPER_CONES: return 0.062f;
    case PMK_CUP_STACK:   return 0.044f;
    case PMK_CREPE_RAKE:  return 0.09f;
    case PMK_BUN:         return 0.058f;
    }
    return 0.1f;
}

/* SF1: what a trade cooks on, and with. Heat spots' steam heights are over the bench's top. */
static int trade_flat(int t) {
    return t == VT_SKEWER || t == VT_KEBAB || t == VT_BURGER || t == VT_FRIED || t == VT_TACO || t == VT_CREPE || t == VT_DRINKS;
}
static void spot(NbLine *L, int b, float x, float z, int heat, float heatY, float steamH, float steamR, float steamS, int smoke, float grease) {
    L->burner[b][0] = x; L->burner[b][1] = z;
    L->heat[b] = (uint8_t)heat; L->heatY[b] = heatY;
    L->steamH[b] = steamH; L->steamR[b] = steamR; L->steamS[b] = steamS; L->smoke[b] = (uint8_t)smoke;
    L->greaseR[b] = grease;
}
/* a pot on a ring burner: its steam (NB7's values: the mouth's height over the pot's foot, radius, strength) */
static void ring_pot(NbLine *L, int b, float h, float r, float s) {
    spot(L, b, L->burner[b][0], L->burner[b][1], NB_HEAT_RING, RANGE_TRAY, GRATE_TOP + h, r, s, 0, 0.7f);
}

/* The range for a wells top (the noodle bar's): the first burner's pot by the trade, the second's by the
 * noodle bar's kind or the trade's. */
static void range_wells(const NbPlan *p, NbLine *L, float wokR) {
    const float yb = p->benchY, bz = L->burner[0][1], top = yb + GRATE_TOP;
    const float x0 = L->burner[0][0], x1 = L->burner[1][0];
    const float dir = (float)-L->rangeEnd;
    const int t = p->trade;
    add_item(L, PMK_BURNER, NB_ON_RANGE, 1, x0, RANGE_TRAY, bz, 0.0f);
    add_item(L, PMK_GRATE, NB_ON_RANGE, 1, x0, yb, bz, 0.0f);
    if (t == VT_ROAST) {
        add_item(L, PMK_ROAST_PAN, NB_ON_RANGE, 1, x0, top, bz, HALF_PI);   /* its handles front and back */
        spot(L, 0, x0, bz, NB_HEAT_RING, RANGE_TRAY, GRATE_TOP + 0.06f, 0.15f, 0.3f, 1, 0.7f);
    } else {
        const int pot = t == VT_CURRY ? PMK_CURRY_POT : t == VT_DUMPLING ? PMK_STOCKPOT_S : PMK_STOCKPOT_L;
        add_item(L, pot, NB_ON_RANGE, 1, x0, top, bz, 0.0f);
        if (t == VT_DUMPLING) { add_item(L, PMK_STEAMER, NB_ON_RANGE, 1, x0, top + 0.30f, bz, 0.0f); ring_pot(L, 0, 0.53f, 0.13f, 1.0f); }
        else if (t == VT_CURRY) ring_pot(L, 0, 0.32f, 0.16f, 0.5f);
        else ring_pot(L, 0, 0.38f, 0.17f, 0.75f);   /* the stock pot, open, simmering */
    }
    int kind = L->kind;
    if (t == VT_SOUP) kind = NB_POT_NOODLES;
    if (t == VT_DUMPLING) kind = NB_POT_STEAMER;
    if (t == VT_CURRY) kind = NB_POT_SAUCEPAN;
    if (t == VT_ROAST) kind = NB_POT_WOK;
    L->kind = kind;
    if (kind == NB_POT_WOK) {
        add_item(L, PMK_WOK_BURNER, NB_ON_RANGE, 1, x1, WOK_FLOOR, bz, 0.0f);
        if (t == VT_ROAST) {
            /* the chestnut wok on the cradle's fingers (0.205 up, 0.11 out), its grit hot */
            const float S = FOOD_CHESTNUT_S, at = S - sqrtf(S * S - 0.11f * 0.11f), y = WOK_FLOOR + 0.212f - at;
            add_item(L, PMK_CHESTNUT_WOK, NB_ON_RANGE, 1, x1, y, bz, 0.0f);
            spot(L, 1, x1, bz, NB_HEAT_WOK, WOK_FLOOR, y + 0.12f - yb, 0.25f, 0.45f, 1, 0.8f);
        } else {
            /* the wok rests on the cradle's fingers (0.205 up, at 0.11 m out), its handle to the aisle */
            const float S = 0.26f, at = S - sqrtf(S * S - 0.11f * 0.11f);
            const float away = x1 > x0 ? 1.0f : -1.0f;   /* its handle swung away from the stock pot */
            add_item(L, PMK_WOK, NB_ON_RANGE, 1, x1, WOK_FLOOR + 0.212f - at, bz, away * (0.1f + 0.35f * wokR));
            spot(L, 1, x1, bz, NB_HEAT_WOK, WOK_FLOOR, WOK_FLOOR + 0.212f - 0.024f + 0.083f - yb, 0.16f, 1.0f, 1, 0.95f);
        }
        return;
    }
    add_item(L, PMK_BURNER, NB_ON_RANGE, 1, x1, RANGE_TRAY, bz, 0.0f);
    add_item(L, PMK_GRATE, NB_ON_RANGE, 1, x1, yb, bz, 0.0f);
    if (kind == NB_POT_SAUCEPAN) {
        add_item(L, PMK_SAUCEPAN, NB_ON_RANGE, 1, x1, top, bz - 0.02f, 0.35f * dir);
        ring_pot(L, 1, 0.10f, 0.08f, 0.45f);
    } else {
        add_item(L, PMK_STOCKPOT_S, NB_ON_RANGE, 1, x1, top, bz, 0.0f);
        if (kind == NB_POT_STEAMER) { add_item(L, PMK_STEAMER, NB_ON_RANGE, 1, x1, top + 0.30f, bz, 0.0f); ring_pot(L, 1, 0.53f, 0.13f, 1.0f); }
        else {
            for (int k = -1; k <= 1; k += 2) add_item(L, PMK_NOODLE_BASKET, NB_ON_RANGE, 1, x1 + 0.072f * (float)k, top + 0.312f, bz - 0.01f, 0.0f);
            ring_pot(L, 1, 0.30f, 0.14f, 1.0f);   /* the noodles' pot, at the boil */
        }
    }
}

/* The range for a flat top: the trade's station standing on it, from the range's end by the side wall (a)
 * along u. */
static void range_flat(const NbPlan *p, NbLine *L) {
    const float yb = p->benchY, bz = L->burner[0][1];
    const float a = L->rangeEnd < 0 ? p->rangeX0 : p->rangeX1, u = (float)-L->rangeEnd;
    const float mid = 0.5f * (p->rangeX0 + p->rangeX1);
    switch (p->trade) {
    case VT_SKEWER: {
        add_item(L, PMK_CHAR_GRILL, NB_ON_RANGE, 1, mid, yb, bz, 0.0f);
        for (int k = -1; k <= 1; k += 2) add_item(L, PMK_SKEWERS, NB_ON_RANGE, 1, mid + 0.18f * (float)k, yb + FOOD_GRILL_RIM + 0.0005f, bz, 0.0f);
        for (int k = 0; k < 2; ++k)
            spot(L, k, mid + (k ? 0.2f : -0.2f), bz, NB_HEAT_COALS, yb + FOOD_GRILL_BED, FOOD_GRILL_RIM + 0.03f, 0.18f, 0.9f, 1, 0.95f);
        L->ductS = 0.7f;
        break;
    }
    case VT_KEBAB: {
        const float sx = a + u * 0.25f, gx = a + u * 0.72f;
        add_item(L, PMK_SPIT, NB_ON_RANGE, 1, sx, yb, bz, 0.0f);
        add_item(L, PMK_GRIDDLE_S, NB_ON_RANGE, 1, gx, yb, bz, 0.0f);
        for (int k = -1; k <= 1; k += 2) add_item(L, PMK_TORTILLA, NB_ON_RANGE, 1, gx + 0.085f * (float)k, yb + FOOD_GRIDDLE_TOP, bz - 0.04f, 0.0f);
        spot(L, 0, sx, bz, NB_HEAT_SPIT, yb + 0.33f, 0.40f, 0.1f, 0.25f, 1, 0.7f);
        spot(L, 1, gx, bz, NB_HEAT_PLATE, yb + FOOD_GRIDDLE_TOP, FOOD_GRIDDLE_TOP + 0.02f, 0.15f, 0.3f, 1, 0.8f);
        break;
    }
    case VT_BURGER: case VT_TACO: {
        add_item(L, PMK_GRIDDLE, NB_ON_RANGE, 1, mid, yb, bz, 0.0f);
        const float y = yb + FOOD_GRIDDLE_TOP;
        if (p->trade == VT_BURGER) {
            static const float PAT[4][2] = { { -0.26f, -0.10f }, { -0.13f, -0.12f }, { -0.27f, 0.03f }, { -0.13f, 0.02f } };
            for (int k = 0; k < 4; ++k) add_item(L, PMK_PATTY, NB_ON_RANGE, 1, mid + u * PAT[k][0], y, bz + PAT[k][1], 0.0f);
            for (int k = 0; k < 3; ++k) add_item(L, PMK_SAUSAGE, NB_ON_RANGE, 1, mid + u * 0.12f, y, bz - 0.13f + 0.04f * (float)k, 0.0f);
            for (int k = 0; k < 2; ++k) add_item(L, PMK_BUN, NB_ON_RANGE, 1, mid + u * (0.06f + 0.14f * (float)k), y, bz + 0.08f, 0.0f);
        } else {
            static const float TOR[3][2] = { { -0.27f, 0.08f }, { -0.11f, 0.08f }, { -0.19f, -0.10f } };
            for (int k = 0; k < 3; ++k) add_item(L, PMK_TORTILLA, NB_ON_RANGE, 1, mid + u * TOR[k][0], y, bz + TOR[k][1], 0.0f);
            add_item(L, PMK_GN_MEAT, NB_ON_RANGE, 1, mid + u * 0.20f, y, bz - 0.06f, HALF_PI);
        }
        for (int k = 0; k < 2; ++k)
            spot(L, k, mid + (k ? 0.18f : -0.18f), bz, NB_HEAT_PLATE, y, FOOD_GRIDDLE_TOP + 0.02f, 0.2f, p->trade == VT_BURGER ? 0.7f : 0.5f, 1, 0.95f);
        L->ductS = 0.6f;
        break;
    }
    case VT_FRIED: {
        const float fx = a + u * 0.30f, tx = a + u * 0.77f;
        add_item(L, PMK_FRYER, NB_ON_RANGE, 1, fx, yb, bz, 0.0f);
        /* one basket down in its oil, the other hung on the rail to drain */
        add_item(L, PMK_FRY_BASKET, NB_ON_RANGE, 1, fx - u * 0.131f, yb + FOOD_FRYER_OIL + 0.06f, bz - 0.03f, 0.0f);
        add_item(L, PMK_FRY_BASKET, NB_ON_RANGE, 1, fx + u * 0.131f, yb + FOOD_FRYER_RAIL - 0.05f, bz - 0.03f, 0.0f);
        add_item(L, PMK_FRIED_TRAY, NB_ON_RANGE, 1, tx, yb, bz + 0.05f, HALF_PI);
        for (int k = 0; k < 2; ++k)
            spot(L, k, fx + u * (k ? 0.131f : -0.131f), bz - 0.03f, NB_HEAT_OIL, yb + FOOD_FRYER_OIL, FOOD_FRYER_OIL + 0.05f, 0.1f, 0.55f, 0, 0.95f);
        break;
    }
    case VT_CREPE: {
        for (int k = 0; k < 2; ++k) {
            const float x = a + u * (0.24f + 0.47f * (float)k);
            add_item(L, PMK_CREPE_PLATE, NB_ON_RANGE, 1, x, yb, bz, 0.0f);
            if (k == 0) add_item(L, PMK_CREPE, NB_ON_RANGE, 1, x, yb + FOOD_CREPE_TOP, bz, 0.0f);
            spot(L, k, x, bz, NB_HEAT_PLATE, yb + FOOD_CREPE_TOP, FOOD_CREPE_TOP + 0.02f, 0.15f, 0.25f, 0, 0.5f);
        }
        L->ductS = 0.25f;
        break;
    }
    default: {   /* drinks: the urn by the wall, the espresso machine */
        const float ux = a + u * 0.17f, ex = a + u * 0.66f;
        add_item(L, PMK_URN, NB_ON_RANGE, 1, ux, yb, bz, 0.0f);
        add_item(L, PMK_ESPRESSO, NB_ON_RANGE, 1, ex, yb, bz - 0.04f, 0.0f);
        spot(L, 0, ux, bz, NB_HEAT_ELECTRIC, yb, 0.52f, 0.05f, 0.25f, 0, 0.0f);
        spot(L, 1, ex, bz + 0.07f, NB_HEAT_ELECTRIC, yb, 0.2f, 0.06f, 0.35f, 0, 0.0f);
        L->ductS = 0.2f;
        break;
    }
    }
}

/* How many pans the trade's bain-marie holds (0: none), and which pan is k of them (`open`: the noodle bar's
 * open one). */
static int bm_pans(int t, float pansR) {
    switch (t) {
    case VT_NOODLE: case VT_SOUP: case VT_KEBAB: return pansR < 0.5f ? 3 : 4;
    case VT_CURRY: case VT_TACO: return 4;
    case VT_BURGER: return 3;
    case VT_FRIED: return 2;
    default: return 0;
    }
}
static int bm_pan(int t, int k, int open) {
    static const int SOUP[4] = { PMK_GN_GREENS, PMK_GN_MEAT, PMK_GN_GREENS, PMK_GN_PAN };
    static const int CURRY[4] = { PMK_GN_PAN_OPEN, PMK_GN_MEAT, PMK_GN_PAN_OPEN, PMK_GN_GREENS };
    static const int KEBAB[4] = { PMK_GN_GREENS, PMK_GN_SAUCE, PMK_GN_MEAT, PMK_GN_GREENS };
    static const int BURGER[4] = { PMK_GN_GREENS, PMK_GN_SAUCE, PMK_GN_PAN, PMK_GN_PAN };
    static const int TACO[4] = { PMK_GN_MEAT, PMK_GN_GREENS, PMK_GN_SAUCE, PMK_GN_GREENS };
    switch (t) {
    case VT_SOUP:   return SOUP[k & 3];
    case VT_CURRY:  return CURRY[k & 3];
    case VT_KEBAB:  return KEBAB[k & 3];
    case VT_BURGER: return BURGER[k & 3];
    case VT_TACO:   return TACO[k & 3];
    case VT_FRIED:  return PMK_GN_SAUCE;
    default:        return k == open ? PMK_GN_PAN_OPEN : PMK_GN_PAN;
    }
}
/* What goes along the bench from the range (after the bain-marie), in order; -1 ends. */
#define BENCH_BUNS 1000   /* not a module: four buns in a square */
static const int *bench_order(int t) {
    static const int O[VT_COUNT][7] = {
        [VT_NOODLE]   = { PMK_BOWL_STACK, PMK_CHOP_BLOCK, PMK_RICE_COOKER, PMK_STEAMER, -1 },
        [VT_SOUP]     = { PMK_BOWL_STACK, PMK_CHOP_BLOCK, PMK_BOWL_STACK, -1 },
        [VT_DUMPLING] = { PMK_DUMPLING_TRAY, PMK_DUMPLING_TRAY, PMK_BOWL_STACK, PMK_CHOP_BLOCK, PMK_STEAMER, -1 },
        [VT_SKEWER]   = { PMK_SKEWER_TRAY, PMK_SKEWER_TRAY, PMK_JAR_OIL, PMK_BOWL_STACK, -1 },
        [VT_KEBAB]    = { PMK_BREAD_STACK, PMK_CHOP_BLOCK, PMK_PAPER_CONES, -1 },
        [VT_BURGER]   = { BENCH_BUNS, PMK_PAPER_CONES, PMK_BOTTLE, PMK_BOTTLE, -1 },
        [VT_FRIED]    = { PMK_PAPER_CONES, PMK_BOWL_STACK, PMK_CHOP_BLOCK, -1 },
        [VT_CURRY]    = { PMK_RICE_COOKER, PMK_RICE_COOKER, PMK_BOWL_STACK, -1 },
        [VT_TACO]     = { PMK_BREAD_STACK, PMK_CHOP_BLOCK, PMK_BOWL_STACK, -1 },
        [VT_CREPE]    = { PMK_BOWL_STACK, PMK_JAR_OIL, PMK_JAR_GINGER, PMK_CREPE_RAKE, PMK_PAPER_CONES, PMK_CHOP_BLOCK, -1 },
        [VT_ROAST]    = { PMK_PAPER_CONES, PMK_CHOP_BLOCK, PMK_BOWL_STACK, -1 },
        [VT_DRINKS]   = { PMK_CUP_STACK, PMK_CUP_STACK, PMK_JAR_GINGER, PMK_BOTTLE, PMK_CUP_STACK, -1 },
    };
    return O[t >= 0 && t < VT_COUNT ? t : 0];
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
    const int t = p->trade >= 0 && p->trade < VT_COUNT ? p->trade : VT_NOODLE;
    L->trade = t;
    L->rangeEnd = re;
    L->kind = kindR < 0.4f ? NB_POT_NOODLES : kindR < 0.65f ? NB_POT_WOK : kindR < 0.85f ? NB_POT_STEAMER : NB_POT_SAUCEPAN;
    L->top = trade_flat(t) ? NB_TOP_FLAT : NB_TOP_WELLS;
    L->ductS = 0.45f;
    if (re < 0) { L->carc0 = p->benchX0; L->carc1 = p->rangeX1; } else { L->carc0 = p->rangeX0; L->carc1 = p->benchX1; }
    if (p->rangeMid) { L->carc0 = p->rangeX0; L->carc1 = p->rangeX1; }

    /* the range: two wells on a 0.45 m pitch, the stock pot's at the wall end unless swapped; or the trade's
     * station on a flat top (SF1) */
    const float bz = zf - 0.30f;
    float bx[2] = { p->rangeX0 + 0.25f, p->rangeX0 + 0.70f };
    int wall0 = re < 0 ? 0 : 1;              /* the burner nearer the side wall */
    if (swapR < 0.25f) wall0 = 1 - wall0;
    L->burner[0][0] = bx[wall0];     L->burner[0][1] = bz;
    L->burner[1][0] = bx[1 - wall0]; L->burner[1][1] = bz;
    if (L->top == NB_TOP_WELLS) range_wells(p, L, wokR);
    else range_flat(p, L);
    if (L->top == NB_TOP_WELLS && L->kind == NB_POT_WOK && t != VT_ROAST) L->ductS = 0.6f;

    /* along the bench from the range: the bain-marie, then the trade's things */
    float b0, b1;
    bench_span(p, L, &b0, &b1);
    float cur = re < 0 ? b0 : b1;                 /* the free bench starts at the range */
    const float stop = re < 0 ? b1 - 0.04f : b0 + 0.04f;
    #define ROOM(w) (dir * (stop - (cur + dir * (w))) >= 0.0f)
    {
        int n = bm_pans(t, pansR);
        while (n >= 2 && !ROOM(0.06f + (float)n * 0.176f + 0.05f)) n--;
        if (n >= 2) {
            const float w = (float)n * 0.176f + 0.05f, a = cur + dir * 0.06f, b = a + dir * w;
            L->bm0 = minf(a, b); L->bm1 = maxf(a, b);
            L->bmZ0 = zl + 0.06f; L->bmZ1 = L->bmZ0 + BM_DEPTH;
            L->bmPans = n;
            const int open = (int)(openR * (float)n) % n;
            for (int k = 0; k < n; ++k)
                add_item(L, bm_pan(t, k, open), NB_ON_BENCH, 1,
                         L->bm0 + 0.025f + 0.088f + 0.176f * (float)k, BM_TOP - 0.15f, L->bmZ0 + 0.02f + 0.1625f, HALF_PI);
            cur = b;
        }
    }
    const float zc = zl + 0.5f * bd + 0.02f;
    const int *order = bench_order(t);
    /* SF1: where the range stands along the bench, the short bench by the side wall, from the range out */
    float curB = re < 0 ? L->carc0 : L->carc1;
    const float dirB = (float)re, stopB = re < 0 ? p->benchX0 + 0.04f : p->benchX1 - 0.04f;
    for (int i = 0; order[i] >= 0; ++i) {
        const int m = order[i];
        if (m == PMK_CHOP_BLOCK && blockR < 0.25f) continue;
        if (t == VT_NOODLE && m == PMK_STEAMER && (steamR < 0.4f || L->kind == NB_POT_STEAMER)) continue;
        const float rr = m == BENCH_BUNS ? 0.12f : bench_r(m), gap = 0.05f;
        float x;
        if (ROOM(gap + 2.0f * rr)) x = cur + dir * (gap + rr);
        else if (p->rangeMid && dirB * (stopB - (curB + dirB * (gap + 2.0f * rr))) >= 0.0f) {
            x = curB + dirB * (gap + rr);
            curB = x + dirB * rr;
            const float z = m == PMK_RICE_COOKER ? zl + 0.06f + rr : zc;
            if (m == BENCH_BUNS) {
                for (int k = 0; k < 4; ++k)
                    add_item(L, PMK_BUN, NB_ON_BENCH, 1, x + ((k & 1) ? 0.06f : -0.06f), yb, z + ((k & 2) ? 0.06f : -0.06f), 0.0f);
            } else add_item(L, m, NB_ON_BENCH, 1, x, yb, z, m == PMK_CREPE_RAKE ? 0.3f : 0.0f);
            continue;
        } else continue;
        float z = m == PMK_RICE_COOKER ? zl + 0.06f + rr : zc;
        if (m == BENCH_BUNS) {
            for (int k = 0; k < 4; ++k)
                add_item(L, PMK_BUN, NB_ON_BENCH, 1, x + ((k & 1) ? 0.06f : -0.06f), yb, z + ((k & 2) ? 0.06f : -0.06f), 0.0f);
        } else {
            add_item(L, m, NB_ON_BENCH, 1, x, yb, z, m == PMK_CREPE_RAKE ? 0.3f : 0.0f);
        }
        if (m == PMK_RICE_COOKER && L->cookerX == 0.0f) { L->cookerX = x; L->cookerZ = z; }
        cur = x + dir * rr;
    }
    #undef ROOM

    /* the undershelf: spare bowls and lidded pans, clear of the legs; fewer as the district is richer (SF1:
     * affluence moves the clutter) */
    {
        float legs[8];
        const int nl = bench_legs(b0, b1, legs, 8);
        const int spares = p->affluence < 0.34f ? 4 : p->affluence < 0.67f ? 3 : 2;
        static const int SPARE[4] = { PMK_BOWL_STACK, PMK_GN_PAN, PMK_BOWL_STACK, PMK_GN_PAN };
        float x = re < 0 ? b0 + 0.06f : b1 - 0.06f;
        for (int i = 0; i < spares; ++i) {
            const int m = t == VT_DRINKS && SPARE[i] == PMK_BOWL_STACK ? PMK_CUP_STACK : SPARE[i];
            const float rr = m == PMK_GN_PAN ? 0.09f : bench_r(m);
            float c = x + dir * rr;
            for (int k = 0; k < nl; ++k) if (fabsf(c - legs[k]) < rr + 0.03f) c = legs[k] + dir * (rr + 0.035f);
            if (dir * ((re < 0 ? b1 : b0) - dir * 0.05f - (c + dir * rr)) < 0.0f) break;
            add_item(L, m, NB_ON_UNDER, 0, c, 0.18f, zl + 0.5f * bd, m == PMK_GN_PAN ? HALF_PI : 0.0f);
            x = c + dir * (rr + 0.04f);
        }
    }

    /* the shelves' stock: bowls and jars low, bottles and jars high (a hash per place: no draws to keep in
     * order); packed closer in a poor district, spaced out in a rich one (SF1) */
    if (p->shelfX1 > p->shelfX0) {
        const float spread = 0.6f + 0.9f * clampf(p->affluence, 0.0f, 1.0f);
        const int low = t == VT_DRINKS ? PMK_CUP_STACK : PMK_BOWL_STACK;
        for (int s = 0; s < 2; ++s) {
            const float y = s ? 1.80f : 1.45f;
            float x = p->shelfX0 + 0.09f;   /* clear of the brackets under the shelf above */
            for (int k = 0; k < 12; ++k) {
                const uint32_t h = hash_combine(p->seed, 0x5E1Fu + (uint32_t)(s * 16 + k));
                const float u = hash_float(h);
                /* the lower shelf starts with bowls (the assembly's spare), then bowls and jars; bottles high */
                const int m = s ? (u < 0.6f ? PMK_BOTTLE : u < 0.8f ? PMK_JAR_OIL : PMK_JAR_GINGER)
                                : (k == 0 || u < 0.4f ? low : u < 0.7f ? PMK_JAR_OIL : PMK_JAR_GINGER);
                const float rr = bench_r(m), gap = (0.015f + 0.05f * hash_float(hash_combine(h, 1u))) * spread;
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

    /* the gas: the bottles at the bench's other end, the regulator on the big one's outlet towards the range; none
     * for the electric trades (SF1, the user: crepes and drinks) */
    {
        const float xg = p->gasX, sg = p->gasX > p->rangeX1 ? 1.0f : -1.0f, toward = -sg;
        const int gas = t != VT_CREPE && t != VT_DRINKS;
        if (gas) {
            add_item(L, PMK_GAS_47, NB_ON_FLOOR, 0, xg, 0.0f, zl + 0.24f, toward > 0 ? 0.0f : PI_F);
            /* SF2: one bottle in the kiosk (its door's in the aisle there) and the tiered kiosk (its side run) */
            if (p->form != NB_HATCH && p->form != NB_TIERED)
                add_item(L, PMK_GAS_13, NB_ON_FLOOR, 0, xg + 0.04f * sg, 0.0f, zl + 0.66f, 0.0f);
            add_item(L, PMK_REGULATOR, NB_ON_FLOOR, 0, xg + toward * 0.04f, 1.3f - 0.12f + 0.025f, zl + 0.24f, toward > 0 ? 0.0f : PI_F);
        }
        L->pipeY = gas ? 1.15f : 0.0f;   /* 0: no gas line */
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
        if (p->form == NB_STRAIGHT || p->form == NB_HATCH || p->form == NB_ARCADE) {
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
            /* SF1: chopsticks where the trade eats with them; elsewhere sauces (and a cup stack for the drinks) */
            static const int ASIA[3] = { PMK_CHOPSTICKS, PMK_JAR_OIL, PMK_BOTTLE }, SAUCE[3] = { PMK_BOTTLE, PMK_JAR_OIL, PMK_BOTTLE };
            static const int SWEET[3] = { PMK_CUP_STACK, PMK_JAR_GINGER, PMK_BOTTLE };
            const int *SET = t == VT_NOODLE || t == VT_SOUP || t == VT_DUMPLING || t == VT_CURRY ? ASIA
                           : t == VT_DRINKS || t == VT_CREPE ? SWEET : SAUCE;
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
    const uint8_t show = pm->show;
    for (int i = 0; i < L->n; ++i) {
        const NbItem *it = &L->item[i];
        pm->part = (uint8_t)part++;
        pm->show = it->on == NB_ON_COUNTER ? NB_SHOW_OPEN : show;   /* SF4: the condiments taken in when shut */
        put(pm, it->x, it->y, it->z, it->yaw, it->module);
    }
    pm->show = show;
    /* the range's knobs on its rail, one by each burner and one for the pilot */
    pm->part = (uint8_t)part++;
    for (int b = 0; b < 2; ++b) put_knob(pm, L->burner[b][0], 0.80f, zf + 0.03f);
    put_knob(pm, L->rangeEnd < 0 ? L->carc0 + 0.075f : L->carc1 - 0.075f, 0.80f, zf + 0.03f);
    /* both burners alight (NB6): a ring burner's flames on its crown, the wok burner's jets roaring; out when shut (SF4) */
    pm->part = (uint8_t)part++;
    pm->show = NB_SHOW_OPEN;
    for (int b = 0; b < 2; ++b) {
        if (L->heat[b] != NB_HEAT_RING && L->heat[b] != NB_HEAT_WOK) continue;   /* SF1: only gas burners show flames */
        const int wok = L->heat[b] == NB_HEAT_WOK;
        put(pm, L->burner[b][0], wok ? WOK_FLOOR : RANGE_TRAY, L->burner[b][1], 0.0f, wok ? PMK_FLAME_JET : PMK_FLAME_RING);
    }
    pm->show = show;
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
    if (L->pipeY > 0.0f) {
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
        { PMK_GAS_47, PMK_REGULATOR }, { PMK_NOODLE_BASKET, PMK_NOODLE_BASKET }, { PMK_GRATE, PMK_GRATE },
        /* SF1: the stations and what they cook */
        { PMK_GRATE, PMK_CURRY_POT }, { PMK_GRATE, PMK_ROAST_PAN }, { PMK_WOK_BURNER, PMK_CHESTNUT_WOK },
        { PMK_GRATE, PMK_CHESTNUT_WOK },   /* the wok's bowl curves up clear of the next grate */
        { PMK_CHAR_GRILL, PMK_SKEWERS }, { PMK_GRIDDLE, PMK_PATTY }, { PMK_GRIDDLE, PMK_SAUSAGE }, { PMK_GRIDDLE, PMK_BUN },
        { PMK_GRIDDLE, PMK_TORTILLA }, { PMK_GRIDDLE, PMK_GN_MEAT }, { PMK_GRIDDLE_S, PMK_TORTILLA },
        { PMK_FRYER, PMK_FRY_BASKET }, { PMK_CREPE_PLATE, PMK_CREPE } };
    for (size_t i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); ++i) {
        int x = PAIRS[i][0], y = PAIRS[i][1];
        if (x > y) { int t = x; x = y; y = t; }
        if (a == x && b == y) return 1;
    }
    return 0;
}

#define ERR(...) do { if (err && used < cap) used += snprintf(err + used, (size_t)(cap - used), __VA_ARGS__); if (err && used < cap) err[used++] = '\n'; if (err && used >= cap) used = cap - 1; if (err) err[used] = 0; ++bad; } while (0)

int nb_line_check(const NbPlan *p, const NbLine *L, char *err, int cap) {
    int bad = 0, used = 0;
    if (err && cap > 0) err[0] = 0;
    const float zl = p->lining, zf = zl + p->benchD;
    /* burners on the pitch, and each pot on its burner */
    if (L->top == NB_TOP_WELLS) {
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
                if (L->heat[b] == NB_HEAT_NONE || L->heat[b] == NB_HEAT_ELECTRIC) continue;
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

int nb_solids(const NbPlan *p, float (*b)[4], int max) { return nb_solids_shown(p, b, NULL, max); }

int nb_solids_shown(const NbPlan *p, float (*b)[4], uint8_t *show, int max) {
    int n = 0;
    uint8_t sh = NB_SHOW_ALWAYS;
#define ADD(a, c, d, e) do { if (n < max) { b[n][0] = (a); b[n][1] = (c); b[n][2] = (d); b[n][3] = (e); if (show) show[n] = sh; n++; } } while (0)
    if (p->form == NB_CORNER) {
        const int s = p->side;
        float xo = s > 0 ? p->x0 : p->x1, xc = p->run[1].o[0] + (float)s * 0.03f;
        ADD(minf(xo, xc), 0.0f, maxf(xo, xc), p->front + 0.03f);
        if (p->column[2] > 0.0f) ADD(p->column[0] - p->column[2], p->column[1] - p->column[2], p->column[0] + p->column[2], p->column[1] + p->column[2]);
    } else if (p->form == NB_TIERED) {
        /* the counters' ring and the back wall's wings behind the side legs */
        ADD(p->x0 - 0.03f, 0.0f, p->x1 + 0.03f, p->front + 0.03f);
    } else if (p->deep) {
        ADD(p->roomX0 - DEEP_WALL, 0.0f, p->roomX1 + DEEP_WALL, p->recess);   /* SF3: the room, behind its glass */
    } else {
        ADD(p->x0, 0.0f, p->x1, p->front + 0.03f);
        if (p->form == NB_HATCH) ADD(minf(p->hatch[0], p->bell[0]) - 0.25f, p->front, maxf(p->hatch[1], p->bell[0]) + 0.25f, p->front + p->sill);
    }
    for (int i = 0; i < p->npost; ++i) ADD(p->post[i][0] - 0.06f, p->post[i][1] - 0.06f, p->post[i][0] + 0.06f, p->post[i][1] + 0.06f);
    /* SF4: the stools up on the counter and the A-frame taken in while shut (a hatch kiosk's stools stay out) */
    sh = p->form == NB_HATCH ? NB_SHOW_ALWAYS : NB_SHOW_OPEN;
    for (int i = 0; i < p->nstool && !p->deep; ++i) ADD(p->stool[i][0] - 0.2f, p->stool[i][1] - 0.2f, p->stool[i][0] + 0.2f, p->stool[i][1] + 0.2f);
    sh = NB_SHOW_OPEN;
    if (p->aframeOn) {   /* its turned footprint's box */
        const float c = fabsf(cosf(p->aframe[2])), s = fabsf(sinf(p->aframe[2]));
        const float hx = 0.32f * c + 0.22f * s, hz = 0.32f * s + 0.22f * c;
        ADD(p->aframe[0] - hx, p->aframe[1] - hz, p->aframe[0] + hx, p->aframe[1] + hz);
    }
#undef ADD
    return n;
}

/* ------------------------------------------------------------ steam (NB7) --- */

/* The bar's lights as the city shades them (shaders/common.glsl vendorWhite, neonColor, vendorPower,
 * vendorFalloff; lighting.glsl addVendor's neon facing), summed at q without a surface's turn: what a puff
 * of steam there is lit by. */
static const float NB_WHITE_RGB[6][3] = { { 0.90f, 0.96f, 1.00f }, { 1.00f, 0.95f, 0.87f }, { 1.00f, 0.73f, 0.47f },
                                          { 1.00f, 0.66f, 0.37f }, { 0.16f, 0.36f, 1.00f }, { 1.00f, 0.42f, 0.12f } };
static const float NB_NEON_RGB[8][3] = { { 1.00f, 0.08f, 0.50f }, { 0.05f, 0.85f, 1.00f }, { 1.00f, 0.10f, 0.04f },
                                         { 1.00f, 0.50f, 0.06f }, { 0.55f, 0.12f, 1.00f }, { 0.15f, 1.00f, 0.40f },
                                         { 0.65f, 0.85f, 1.00f }, { 1.00f, 0.85f, 0.15f } };
static void light_at(const NbLight *lt, int nl, const float q[3], float out[3]) {
    out[0] = out[1] = out[2] = 0.0f;
    for (int i = 0; i < nl; ++i) {
        const NbLight *l = &lt[i];
        float c[3], at[3] = { l->a[0], l->a[1], l->a[2] }, power, soft, reach;
        if (l->colour >= NB_NEON) memcpy(c, NB_NEON_RGB[(l->colour - NB_NEON) & 7], sizeof(c));
        else memcpy(c, NB_WHITE_RGB[l->colour < 6 ? l->colour : 0], sizeof(c));
        if (l->kind == NB_LIGHT_TUBE) {
            const float u[3] = { l->b[0] - l->a[0], l->b[1] - l->a[1], l->b[2] - l->a[2] };
            const float len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
            float t = len > 1e-4f ? ((q[0] - l->a[0]) * u[0] + (q[1] - l->a[1]) * u[1] + (q[2] - l->a[2]) * u[2]) / (len * len) : 0.0f;
            t = clampf(t, 0.0f, 1.0f);
            for (int k = 0; k < 3; ++k) at[k] = l->a[k] + u[k] * t;
            power = 2.2f * (l->colour >= 2 ? 0.3f : 1.0f) * maxf(len, 0.3f); soft = 0.25f; reach = 9.0f;
        } else if (l->kind == NB_LIGHT_LAMP) { power = 0.8f; soft = 0.05f; reach = 5.0f; }
        else if (l->kind == NB_LIGHT_FLAME) { power = l->colour == NB_EMBER ? 0.4f : 0.12f; soft = 0.01f; reach = 1.6f; }
        else { power = 1.2f; soft = 0.1f; reach = 6.0f; }
        const float d[3] = { q[0] - at[0], q[1] - at[1], q[2] - at[2] };
        const float dd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float att = power / (dd * dd + soft);
        const float f = clampf((reach - dd) / (0.55f * reach), 0.0f, 1.0f);
        att *= f * f * (3.0f - 2.0f * f);
        if (l->kind == NB_LIGHT_NEON && dd > 1e-4f)   /* in front of the sign */
            att *= 0.25f + 0.75f * maxf((d[0] * l->b[0] + d[2] * l->b[1]) / dd, 0.0f);
        for (int k = 0; k < 3; ++k) out[k] += c[k] * att;
    }
}

#define STEAM_GAP 0.07f   /* how far under a ceiling the sheet of steam runs (its middle) */

int nb_steam(const NbPlan *p, const NbLine *L, float ceiling, NbSteam *out, int max) {
    NbLight lt[NB_LIGHTS_MAX];
    const int nl = nb_lights(p, lt, NB_LIGHTS_MAX);
    const float ez = p->lining + 0.004f + 0.60f; /* the extractor canopy's front edge (its underside at 1.97) */
    /* SF2: the kiosk's steam runs under its roof to the glazing and out over it, under the hood's overhang */
    const int hatch = p->form == NB_HATCH;
    const float lip = hatch ? p->front : p->lip, lipOut = p->lip;
    const float under = hatch ? nb_ceil(p, 0.0f, p->front - 0.08f) + 0.01f : p->hood.spring;
    const float fascia = hatch ? p->glazeTop : p->hood.spring - p->hood.fascia_h;
    int n = 0;
    for (int b = 0; b < 2 && n < max; ++b) {
        /* each heat spot's steam or smoke, as its trade set it (nb_line: the mouth's height over the bench) */
        if (L->steamS[b] <= 0.0f) continue;
        NbSteam *s = &out[n];
        memset(s, 0, sizeof(*s));
        s->kind = (uint8_t)(L->smoke[b] ? NB_STEAM_WOK : NB_STEAM_POT);
        s->pos[0] = L->burner[b][0]; s->pos[1] = p->benchY + L->steamH[b]; s->pos[2] = L->burner[b][1];
        s->radius = L->steamR[b]; s->strength = L->steamS[b];
        const float x = s->pos[0], y0 = s->pos[1], z0 = s->pos[2];
        /* up to the extractor's baffles, out under it to its front edge, up under the hood, along under it to
         * the lip, down behind the fascia and out under it into the street; the third value the ceiling over
         * the knot (0: none), which the steam never rises through */
        const float ya = 1.97f - STEAM_GAP - y0, ze = ez + 0.06f, zm = 0.5f * (ze + lip - 0.08f);
        const float ce = nb_ceil(p, x, ze), cm = nb_ceil(p, x, zm);
        const float K[NB_KNOTS_MAX][3] = {
            { 0.0f, 0.0f, 0.0f },
            { 0.0f, ya, 0.0f },
            { ze - z0, ya, 0.0f },
            { ze - z0, ce - STEAM_GAP - y0, ce - y0 },
            { zm - z0, cm - STEAM_GAP - y0, cm - y0 },
            { lip - 0.08f - z0, under - STEAM_GAP - y0, under - y0 },
            { lip - 0.08f - z0, fascia - STEAM_GAP - y0, 0.0f },
            { lipOut + 0.22f - z0, fascia + 0.05f - y0, 0.0f },
        };
        s->nknot = NB_KNOTS_MAX;
        memcpy(s->knot, K, sizeof(K));
        s->rise = 1.5f;
        if (ceiling > 0.0f) s->rise = clampf(ceiling - 0.2f - (y0 + K[7][1]), 0.0f, 1.5f);
        /* the light on it: over the pot, under the hood, out past the fascia */
        const float q0[3] = { x, y0 + 0.35f, z0 };
        const float q1[3] = { x, y0 + K[4][1], z0 + K[4][0] };
        const float q2[3] = { x, y0 + K[7][1] + 0.3f, z0 + K[7][0] };
        light_at(lt, nl, q0, s->light[0]);
        light_at(lt, nl, q1, s->light[1]);
        light_at(lt, nl, q2, s->light[2]);
        n++;
    }
    /* what the extractor draws off, out of the duct's cap over the hood */
    if (n < max && (ceiling <= 0.0f || ceiling - p->top > 0.8f)) {
        NbSteam *s = &out[n++];
        memset(s, 0, sizeof(*s));
        s->kind = NB_STEAM_DUCT;
        s->pos[0] = p->ductX; s->pos[1] = p->ductTop + 0.12f; s->pos[2] = p->ductZ;
        s->radius = p->ductR; s->strength = L->ductS;
        s->nknot = 1;
        s->rise = ceiling > 0.0f ? minf(1.5f, ceiling - p->top - 0.3f) : 1.5f;
        const float q[3] = { s->pos[0], s->pos[1] + 0.4f, s->pos[2] };
        light_at(lt, nl, q, s->light[0]);
        memcpy(s->light[1], s->light[0], sizeof(s->light[0]));
        memcpy(s->light[2], s->light[0], sizeof(s->light[0]));
    }
    return n;
}

/* the nearest point of segment a-b (in the plane: out, up) to q; returns the squared distance, t its share */
static float seg_d2(const float a[3], const float b[3], const float q[2], float *t) {
    const float u[2] = { b[0] - a[0], b[1] - a[1] }, w[2] = { q[0] - a[0], q[1] - a[1] };
    const float uu = u[0] * u[0] + u[1] * u[1];
    *t = uu > 1e-8f ? clampf((w[0] * u[0] + w[1] * u[1]) / uu, 0.0f, 1.0f) : 0.0f;
    const float d[2] = { w[0] - u[0] * *t, w[1] - u[1] * *t };
    return d[0] * d[0] + d[1] * d[1];
}

float nb_grime(const NbPlan *p, const NbLine *L, const NbSteam *st, int nst, const float pos[3], const float *n) {
    /* grease: spat and fumes round each burner's flame, reaching higher than wide (the wok's further) */
    float grease = 0.0f;
    for (int b = 0; b < 2; ++b) {
        if (L->greaseR[b] <= 0.0f) continue;   /* SF1: as far as the trade's heat spatters (none by an urn) */
        const int wok = L->greaseR[b] >= 0.9f;
        const float c[3] = { L->burner[b][0], p->benchY + GRATE_TOP + 0.1f, L->burner[b][1] };
        const float d[3] = { pos[0] - c[0], pos[1] - c[1], pos[2] - c[2] };
        const float dy = d[1] > 0.0f ? d[1] * 0.5f : d[1] * 2.0f;
        const float r = sqrtf(d[0] * d[0] + dy * dy + d[2] * d[2]);
        const float reach = maxf(L->greaseR[b], 0.3f);
        float g = 1.0f - clampf((r - 0.15f) / (reach - 0.15f), 0.0f, 1.0f);
        g = g * (2.0f - g) * (wok ? 1.0f : 0.8f);
        if (n && r > 1e-3f) g *= clampf(0.3f - (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) / r, 0.0f, 1.0f);   /* facing the flame */
        grease = maxf(grease, g);
    }
    /* condensation: where the steam runs, from the extractor out to the fascia, on what faces it */
    float wet = 0.0f;
    for (int i = 0; i < nst; ++i) {
        const NbSteam *s = &st[i];
        if (s->kind == NB_STEAM_DUCT || s->nknot < 3) continue;
        const float q[2] = { pos[2] - s->pos[2], pos[1] - s->pos[1] };
        float best = 1e9f, along = 0.0f, acc = 0.0f, ny = 0.0f;
        for (int k = 1; k + 1 < s->nknot; ++k) {
            float t;
            const float d2 = seg_d2(s->knot[k], s->knot[k + 1], q, &t);
            const float du = s->knot[k + 1][0] - s->knot[k][0], dv = s->knot[k + 1][1] - s->knot[k][1];
            const float len = sqrtf(du * du + dv * dv);
            if (d2 < best) { best = d2; along = acc + t * len; ny = s->knot[k][1] + t * dv; }
            acc += len;
        }
        /* across the frontage the sheet spreads as it goes */
        const float side = maxf(fabsf(pos[0] - s->pos[0]) - (0.15f + 0.08f * along), 0.0f);
        const float d = sqrtf(best + side * side);
        float w = 1.0f - clampf((d - 0.08f) / 0.5f, 0.0f, 1.0f);
        w = w * w * s->strength * (s->kind == NB_STEAM_WOK ? 0.7f : 1.0f);
        /* on what faces the steam: an underside over it facing down, a face beside or under it facing up or out */
        if (n) {
            const float above = q[1] - ny;
            w *= above > 0.0f ? clampf(0.2f - n[1] * 1.2f, 0.0f, 1.0f) : clampf(0.4f + n[1], 0.0f, 1.0f);
        }
        wet = maxf(wet, w);
    }
    return wet >= grease ? wet : -grease;
}

/* ------------------------------------------------------------ hours (SF4) --- */

const char *nb_hours_name(int kind) {
    static const char *N[NB_HOURS_KINDS] = { "always", "lunch", "day", "two", "evening", "late", "morning", "allday" };
    return kind >= 0 && kind < NB_HOURS_KINDS ? N[kind] : "?";
}

/* Each trade's share of the kinds (rows sum to 1; NB_HOURS_LUNCH is the site's, not the trade's): the night food
 * keeps the evening and the small hours, the soups and dumplings the morning, the drinks and crepes the day. */
static const float HOURS_BY_TRADE[VT_COUNT][NB_HOURS_KINDS] = {
    /*               always lunch day    two    even   late   morn   allday */
    [VT_NOODLE]   = { 0.15f, 0, 0.00f, 0.30f, 0.20f, 0.20f, 0.00f, 0.15f },
    [VT_SOUP]     = { 0.00f, 0, 0.00f, 0.20f, 0.00f, 0.15f, 0.45f, 0.20f },
    [VT_DUMPLING] = { 0.00f, 0, 0.00f, 0.20f, 0.15f, 0.00f, 0.35f, 0.30f },
    [VT_SKEWER]   = { 0.00f, 0, 0.00f, 0.10f, 0.45f, 0.45f, 0.00f, 0.00f },
    [VT_KEBAB]    = { 0.10f, 0, 0.00f, 0.00f, 0.20f, 0.45f, 0.00f, 0.25f },
    [VT_BURGER]   = { 0.00f, 0, 0.00f, 0.10f, 0.30f, 0.25f, 0.00f, 0.35f },
    [VT_FRIED]    = { 0.00f, 0, 0.00f, 0.00f, 0.40f, 0.35f, 0.00f, 0.25f },
    [VT_CURRY]    = { 0.00f, 0, 0.00f, 0.40f, 0.25f, 0.00f, 0.00f, 0.35f },
    [VT_TACO]     = { 0.00f, 0, 0.00f, 0.00f, 0.40f, 0.30f, 0.00f, 0.30f },
    [VT_CREPE]    = { 0.00f, 0, 0.50f, 0.00f, 0.20f, 0.00f, 0.00f, 0.30f },
    [VT_ROAST]    = { 0.00f, 0, 0.30f, 0.00f, 0.40f, 0.00f, 0.00f, 0.30f },
    [VT_DRINKS]   = { 0.15f, 0, 0.45f, 0.00f, 0.00f, 0.00f, 0.15f, 0.25f },
};
#define LUNCH_SHARE 0.7f   /* of an office forecourt's stalls, those keeping the lunch hours */

uint8_t nb_hours(const NbPlan *p, int lunch) {
    const uint32_t h = hash_combine(p->seed, 0x4F9Eu);
    const float u = hash_float(h);
    const uint8_t jitter = (uint8_t)(hash_combine(h, 1u) & 15u);
    int kind = NB_HOURS_ALLDAY;
    if (lunch && u < LUNCH_SHARE) kind = NB_HOURS_LUNCH;
    else {
        const float *w = HOURS_BY_TRADE[p->trade >= 0 && p->trade < VT_COUNT ? p->trade : VT_NOODLE];
        const float r = lunch ? (u - LUNCH_SHARE) / (1.0f - LUNCH_SHARE) : u;
        float acc = 0.0f;
        for (int k = 0; k < NB_HOURS_KINDS; ++k) {
            acc += w[k];
            if (w[k] > 0.0f && r < acc) { kind = k; break; }
        }
    }
    return (uint8_t)(kind | jitter << 3);
}

/* Whether a kind's window (or windows) on that day holds its hour t (t past 24: the small hours after it). */
static int hours_window(int kind, int jitter, float t, int day) {
    const int weekend = day >= 5, late = day == 4 || day == 5;   /* Saturday, Sunday; the nights after Friday, Saturday */
    float a, b, a2 = -1.0f, b2 = -1.0f;
    switch (kind) {
    case NB_HOURS_LUNCH:   if (weekend) return 0; a = 11.0f; b = 14.5f; break;
    case NB_HOURS_DAY:     a = weekend ? 9.0f : 7.0f; b = 19.0f; break;
    case NB_HOURS_TWO:     a = 11.0f; b = 14.5f; a2 = 17.0f; b2 = 23.0f; break;
    case NB_HOURS_EVENING: a = 17.0f; b = 24.0f; break;
    case NB_HOURS_LATE:    a = 19.0f; b = late ? 28.0f : 27.0f; break;
    case NB_HOURS_MORNING: a = weekend ? 7.0f : 6.0f; b = 15.0f; break;
    default:               a = 10.0f; b = 23.0f; break;
    }
    const float da = ((float)(jitter & 3) - 1.5f) * 0.25f, db = ((float)(jitter >> 2) - 1.5f) * (1.0f / 3.0f);
    if (t >= a + da && t < b + db) return 1;
    return a2 >= 0.0f && t >= a2 + da && t < b2 + db;
}

int nb_hours_open(uint8_t code, float hours, int weekday) {
    const int kind = code & 7, jitter = (code >> 3) & 15;
    if (kind == NB_HOURS_ALWAYS) return 1;
    const float t = fmodf(fmodf(hours, 24.0f) + 24.0f, 24.0f);
    const int d = ((weekday % 7) + 7) % 7;
    /* today's window, or yesterday's running on past midnight */
    return hours_window(kind, jitter, t, d) || hours_window(kind, jitter, t + 24.0f, (d + 6) % 7);
}

static float g_hoursNow = 18.95f;   /* the captures' hour (SKY_CAPTURE_UTC: a weekday, 18:57) until the clock's set */
static int   g_dayNow = 2, g_hoursForce = -1;
/* What is shown: one bit a code (open), changed to the clock's only while no vendor of that code is in view (the
 * user, SF4's board: nobody sees a stall open or shut); every code takes the clock's at the first update. */
static uint32_t g_shown[4];
static int      g_shownInit;
void nb_hours_now(float hours, int weekday) { g_hoursNow = hours; g_dayNow = weekday; }
void nb_hours_force(int mode) { g_hoursForce = mode < 0 ? -1 : mode > 0; }
int  nb_hours_forced(void) { return g_hoursForce; }
void nb_hours_update(const uint32_t seen[4]) {
    for (int c = 0; c < 128; ++c) {
        const uint32_t bit = 1u << (c & 31);
        if (g_shownInit && seen && (seen[c >> 5] & bit)) continue;
        if (nb_hours_open((uint8_t)c, g_hoursNow, g_dayNow)) g_shown[c >> 5] |= bit;
        else g_shown[c >> 5] &= ~bit;
    }
    g_shownInit = 1;
}
void nb_hours_shown(uint32_t out[4]) {
    if (!g_shownInit) nb_hours_update(NULL);
    memcpy(out, g_shown, sizeof(g_shown));
}
int  nb_open_now(uint8_t code) {
    if (g_hoursForce >= 0) return g_hoursForce;
    if (!g_shownInit) nb_hours_update(NULL);
    return (int)(g_shown[(code >> 5) & 3] >> (code & 31)) & 1;
}

/* ----------------------------------------------------------- shutters (SF4) --- */

#define SHUT_BACK  0.04f   /* the curtain stands this far in from the counter top's back edge */
#define SHUT_GUIDE 0.03f   /* the guides' channels: the curtain's span is inset this much at each end */

/* Whether another run starts or ends at this run's point q (a corner of the L or the U). */
static int run_joined(const NbPlan *p, int self, float qx, float qz) {
    for (int k = 0; k < p->nrun; ++k) {
        if (k == self) continue;
        const NbRun *c = &p->run[k];
        const float e[2][2] = { { c->o[0], c->o[1] }, { c->o[0] + c->u[0] * c->len, c->o[1] + c->u[1] * c->len } };
        for (int j = 0; j < 2; ++j) {
            const float dx = e[j][0] - qx, dz = e[j][1] - qz;
            if (dx * dx + dz * dz < 0.15f * 0.15f) return 1;
        }
    }
    return 0;
}

int nb_shutters(const NbPlan *p, NbShutter *out, int max) {
    int n = 0;
    if (p->deep) {
        /* the shopfront, just out from the building line, from the ground to the head, across the opening */
        if (n < max) {
            const float x0 = p->roomX0 - DEEP_WALL, x1 = p->roomX1 + DEEP_WALL;
            out[n++] = (NbShutter){ { x0, p->recess + 0.035f }, { 1.0f, 0.0f }, x1 - x0, 0.0f, p->head - 0.01f };
        }
        return n;
    }
    if (p->form == NB_HATCH) {
        /* over the glazing, outside it: from the sill to the header */
        if (n < max) out[n++] = (NbShutter){ { p->x0 + 0.02f, p->front + 0.035f }, { 1.0f, 0.0f }, p->x1 - p->x0 - 0.04f, p->cy, p->glazeTop };
        return n;
    }
    for (int i = 0; i < p->nrun && n < max; ++i) {
        const NbRun *c = &p->run[i];
        const float ox = -c->u[1], oz = c->u[0];   /* out to the customers */
        const float din = maxf(p->cd - SHUT_BACK, 0.12f);
        const float t0 = run_joined(p, i, c->o[0], c->o[1]) ? din : 0.0f;
        const float t1 = c->len - (run_joined(p, i, c->o[0] + c->u[0] * c->len, c->o[1] + c->u[1] * c->len) ? din : 0.0f);
        if (t1 - t0 < 0.4f) continue;
        const float x = c->o[0] + c->u[0] * t0 - ox * din, z = c->o[1] + c->u[1] * t0 - oz * din;
        /* up into what is overhead: the lowest of it along the curtain (a hood's sheet, the deck; a shop bay's
         * soffit) */
        float top = 1e9f;
        for (int k = 0; k <= 4; ++k) {
            const float t = (t1 - t0) * (0.05f + 0.225f * (float)k);
            top = minf(top, nb_ceil(p, x + c->u[0] * t, z + c->u[1] * t));
        }
        if (p->recess > 0.0f && p->head > 0.0f) top = minf(top, p->head - 0.01f);
        if (top - p->cy < 0.3f) continue;
        out[n++] = (NbShutter){ { x, z }, { c->u[0], c->u[1] }, t1 - t0, p->cy, top, 0 };
        /* side panels (the user, SF4's board): where a front run's free end has no wall closing the kitchen beside
         * it (the arcade counter's passage, a freestanding end), a panel from the curtain back to the lining, from
         * the ground up into what is overhead, just outside the counter's end */
        if (fabsf(c->u[1]) > 0.01f || c->u[0] < 0.0f) continue;
        const float zc = z, zl = p->lining;
        if (zc - zl < 0.3f) continue;
        for (int k = 0; k < 2 && n < max; ++k) {
            const float xe = k ? c->o[0] + c->len : c->o[0];
            if (run_joined(p, i, xe, c->o[1])) continue;
            float cover = 0.0f;
            for (int w = 0; w < p->nwall; ++w) {
                const NbWall *wl = &p->wall[w];
                if (fabsf(wl->u[0]) > 0.01f || fabsf(wl->o[0] - xe) > 0.15f || wl->h < 1.5f) continue;
                const float a = minf(wl->o[1], wl->o[1] + wl->u[1] * wl->len), b = maxf(wl->o[1], wl->o[1] + wl->u[1] * wl->len);
                cover += maxf(0.0f, minf(b, zc) - maxf(a, zl));
            }
            if (cover > 0.7f * (zc - zl)) continue;
            const float xs = xe + (k ? 0.03f : -0.03f);
            float top = nb_ceil(p, xs, 0.5f * (zc + zl));
            if (p->recess > 0.0f && p->head > 0.0f) top = minf(top, p->head - 0.01f);
            if (top < 1.5f) continue;
            out[n++] = k ? (NbShutter){ { xs, zc }, { 0.0f, -1.0f }, zc - zl, 0.0f, top, 1 }
                         : (NbShutter){ { xs, zl }, { 0.0f, 1.0f }, zc - zl, 0.0f, top, 1 };
        }
    }
    return n;
}

/* A module stretched over a frame: its x along u (scaled sx), y up (scaled sy), z out, its origin at o. */
static void put_frame(Pm *pm, const float o[3], const float u[2], float sx, float sy, int id) {
    const float ex[3] = { u[0] * sx, 0.0f, u[1] * sx }, ey[3] = { 0.0f, sy, 0.0f }, ez[3] = { -u[1], 0.0f, u[0] };
    pm_push(pm);
    pm_frame(pm, o, ex, ey, ez);
    pm_place(pm, id);
    pm_pop(pm);
}

void nb_shut_place(Pm *pm, const NbPlan *p) {
    const uint8_t show = pm->show;
    pm->show = NB_SHOW_SHUT;
    NbShutter sh[NB_SHUTTERS_MAX];
    const int ns = nb_shutters(p, sh, NB_SHUTTERS_MAX);
    for (int i = 0; i < ns; ++i) {
        const NbShutter *s = &sh[i];
        /* its part: 244 + i over a counter, 236 + i standing on the ground (kit.glsl: where its graffiti goes) */
        pm->part = (uint8_t)(s->y0 > 0.1f ? 244 + i : 236 + i);
        const float w = s->len - 2.0f * SHUT_GUIDE, mx = s->o[0] + s->u[0] * 0.5f * s->len, mz = s->o[1] + s->u[1] * 0.5f * s->len;
        const float bar = 0.062f;
        const float c0[3] = { mx, s->y0 + bar, mz }, b0[3] = { mx, s->y0, mz };
        put_frame(pm, c0, s->u, w, s->y1 - s->y0 - bar, PMK_SHUTTER);
        put_frame(pm, b0, s->u, w, 1.0f, PMK_SHUTTER_BAR);
        /* the guides: the first's web out at -u, the other turned round (its web out at +u) */
        const float g0[3] = { s->o[0] + s->u[0] * SHUT_GUIDE, s->y0, s->o[1] + s->u[1] * SHUT_GUIDE };
        const float g1[3] = { s->o[0] + s->u[0] * (s->len - SHUT_GUIDE), s->y0, s->o[1] + s->u[1] * (s->len - SHUT_GUIDE) };
        const float back[2] = { -s->u[0], -s->u[1] };
        put_frame(pm, g0, s->u, 1.0f, s->y1 - s->y0, PMK_SHUTTER_GUIDE);
        put_frame(pm, g1, back, 1.0f, s->y1 - s->y0, PMK_SHUTTER_GUIDE);
        /* the padlock on the bar, off its middle by the seed (a side panel is bolted from inside) */
        if (s->side) continue;
        const float off = (hash_float(hash_combine(p->seed, 0x10C4u + (uint32_t)i)) - 0.5f) * 0.4f * w;
        const float l0[3] = { mx + s->u[0] * off, s->y0, mz + s->u[1] * off };
        put_frame(pm, l0, s->u, 1.0f, 1.0f, PMK_SHUTTER_LOCK);
    }
    /* the stools upside down on the counter's top, in front of the curtain (a deep unit's stay at its ledge) */
    if (!p->deep && p->form != NB_HATCH) {
        for (int i = 0; i < p->nstool; ++i) {
            const float sx = p->stool[i][0], sz = p->stool[i][1];
            for (int k = 0; k < p->nrun; ++k) {
                const NbRun *c = &p->run[k];
                const float ox = -c->u[1], oz = c->u[0];
                const float t = (sx - c->o[0]) * c->u[0] + (sz - c->o[1]) * c->u[1];
                const float off = (sx - c->o[0]) * ox + (sz - c->o[1]) * oz;
                if (off < 0.0f || off > 0.8f || t < 0.0f || t > c->len) continue;
                const float din = maxf(p->cd - SHUT_BACK, 0.12f), tt = clampf(t, 0.22f, c->len - 0.22f);
                const float x = c->o[0] + c->u[0] * tt - ox * 0.5f * din, z = c->o[1] + c->u[1] * tt - oz * 0.5f * din;
                pm->part = (uint8_t)(170 + i);
                pm_push(pm);
                pm_translate(pm, x, p->cy + 0.751f, z);   /* the seat's top on the counter */
                pm_rotate_y(pm, 6.2831853f * hash_float(hash_combine(p->seed, 0x5700u + (uint32_t)i)));
                pm_rotate_x(pm, 3.14159265f);
                pm_place(pm, PMK_STOOL);
                pm_pop(pm);
                break;
            }
        }
    }
    pm->show = show;
}
