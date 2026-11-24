#include "home_char_internal.h"
#include "pmodel_house.h"
#include "pmodel_region.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The other regions' pieces (residential roadmap HM5b2; the plan's section
 * "HM5b2: the other regions' grammar, style by style"; the sheets in
 * docs/reference/international-residential): what the regional kit
 * (pmodel_region.h) sets down that is not a window, a door, a porch post or
 * a roof's edge, which home_character.c's passes dress in their own kinds.
 * Run after the posts, before the balconies, boundaries, insertions and the
 * retrofit layer, which keep clear of what is taken here (hc_occupy).
 *
 *   HM5b2a   China: the gatehouse's ridge, ends, eave tiles, knockers, drum
 *            stones, couplets and lanterns; a house door's knockers, couplets,
 *            the fu and its lanterns. Korea: the pilotis' hazard sleeves, the
 *            mailboxes by the stair; a hanok door's knockers. The shophouse:
 *            the five-foot way's columns, pilasters on its piers and upper
 *            storeys, lanterns from its ceiling. The link house: the car
 *            porch over its pad, the gable's roundel. Japan: a slat screen
 *            beside the door.
 * ========================================================================= */

static int ch(uint32_t h, uint32_t k, float p) { return hash_float(hash_combine(h, k)) < p; }
static double unit_dy(const DC *d, int u) { return d->C->dy && u >= 0 ? d->C->dy[u] : 0.0; }
static const HVol *porch_of(const HGraph *g, int u) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role == HV_PORCH && g->vol[i].unit == u) return &g->vol[i];
    return NULL;
}
static const double FRONT_M[2] = { 0.0, -1.0 }, FRONT_E[2] = { 1.0, 0.0 };

/* Whether a point stands in the open: no solid holds it, or an opening carved it out (a recess, the five-foot way, the
 * pilotis: hs_inside counts a body's solid whole) */
static int open_at(const DC *d, double x, double y, double z) {
    const double p[3] = { x, y, z };
    const Ctx *C = d->C;
    int held = 0;
    for (int i = 0; i < C->ns; ++i) {
        const Solid *s = &C->s[i];
        if (!s->live || p[0] < s->mn[0] || p[0] > s->mx[0] || p[1] < s->mn[1] || p[1] > s->mx[1] || p[2] < s->mn[2] || p[2] > s->mx[2]) continue;
        if (s->neg) { if (hs_in_solid(s, p, 0.003)) return 1; }
        else if (!held && hs_in_solid(s, p, 0.003)) held = 1;
    }
    return !held;
}
/* Whether a box is open: its corners and middle in no solid, and nothing the kit set down holds it */
static int open_box(const DC *d, const double lo[3], const double hi[3]) {
    if (!hc_free(d, lo, hi)) return 0;
    static const double F[3] = { 0.08, 0.5, 0.92 };
    for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b)
            for (int c = 0; c < 3; ++c)
                if (!open_at(d, lo[0] + (hi[0] - lo[0]) * F[a], lo[1] + (hi[1] - lo[1]) * F[b], lo[2] + (hi[2] - lo[2]) * F[c])) return 0;
    return 1;
}
/* A module on the front wall (its face at z, facing the street) at (x, y), stretched sx by sy, `out` off the face */
static void put_front(DC *d, int module, double x, double y, double z, double out, double sx, double sy) {
    const double p[3] = { x, y, z };
    hc_push_wall(d, p, FRONT_M[0], FRONT_M[1]);
    pm_translate(d->pm, 0.0f, 0.0f, (float)out);
    pm_scale(d->pm, (float)sx, (float)sy, 1.0f);
    pm_place(d->pm, module);
    hc_pop(d);
}
/* A paper lantern hung from (x, y, z), if the space under its hook is open */
static int lantern(DC *d, double x, double y, double z) {
    const double lo[3] = { x - 0.22, y - HK_LANTERN_H, z - 0.22 }, hi[3] = { x + 0.22, y - 0.03, z + 0.22 };
    if (!open_box(d, lo, hi)) return 0;
    const double p[3] = { x, y, z };
    hc_push_wall(d, p, FRONT_M[0], FRONT_M[1]);
    pm_place(d->pm, PMK_H_LANTERN_PAPER);
    hc_pop(d);
    hc_occupy(d, lo, hi);
    return 1;
}

/* ---------------------------------------------------------------------------
 * China: the gatehouse and the doors
 * ------------------------------------------------------------------------- */

/* The gatehouse over a courtyard gate (home_shell.c add_gatehouse's geometry: piers 0.5 m, the lintel wall to h + 0.55,
 * the tiled roof's eaves 0.5 m out at both faces, its ridge 0.62 m over them): the ridge's rolls and ends, eave tiles
 * along both eaves, knockers on the leaves, couplets on the piers and over the gate, drum stones, lanterns. */
static void gatehouse(DC *d, const HGate *gt) {
    const int u = gt->unit;
    const HCharSet *cs = &d->C->cs[u];
    const int korea = d->g->style == HS_KOREA, endM = korea ? PMK_H_RIDGE_END_KR : PMK_H_RIDGE_END;   /* a hanok's gate: Korea's ends, no
                                                                                                         couplets, stones or lanterns */
    const double dy = unit_dy(d, u), pw = 0.5, gx0 = gt->x - 0.5 * gt->w, gx1 = gt->x + 0.5 * gt->w;
    const double hp = gt->h + 0.55, zf = -0.12, zb = BOUND_T + 0.3;
    const double a0 = gx0 - pw - 0.4, a1 = gx1 + pw + 0.4, c0 = zf - 0.5, c1 = zb + 0.5, y0 = hp - 0.02 + dy, yr = y0 + 0.62;
    const double zc = 0.5 * (c0 + c1), zLeaf = 0.5 * BOUND_T - 0.04;
    hc_begin(d, u);
    {   /* the ridge */
        const double ra[3] = { a0, yr, zc }, rb[3] = { a1, yr, zc }, px[3] = { 1.0, 0.0, 0.0 }, nx[3] = { -1.0, 0.0, 0.0 };
        hc_push_along(d, ra, px); hk_ridge(d->pm, 0, (float)(a1 - a0)); hc_pop(d);
        hc_push_along(d, ra, px); pm_place(d->pm, endM); hc_pop(d);
        hc_push_along(d, rb, nx); pm_place(d->pm, endM); hc_pop(d);
    }
    {   /* the eave tiles, the street's eaves and the yard's */
        const double mb[2] = { 0.0, 1.0 }, eb[2] = { -1.0, 0.0 };
        double L = hc_push_run_wall(d, FRONT_M, FRONT_E, -c0, a0, a1, y0, 0.0);
        hk_run(d->pm, PMK_H_EAVE_TILES, (float)L, 1.0f);
        hc_pop(d);
        L = hc_push_run_wall(d, mb, eb, c1, -a1, -a0, y0, 0.0);
        hk_run(d->pm, PMK_H_EAVE_TILES, (float)L, 1.0f);
        hc_pop(d);
    }
    /* knockers on the leaves' street face, either side of their meeting */
    if (gt->w >= 0.9)
        for (int s = -1; s <= 1; s += 2) put_front(d, PMK_H_KNOCKER, gt->x + s * 0.14, dy + 1.15, zLeaf, 0.002, 1.0, 1.0);
    if (korea) { hc_done(d); return; }
    /* drum stones before the piers, the couplets on the piers over them, one over the gate on the lintel wall */
    for (int s = 0; s < 2; ++s) {
        const double xp = s ? gx1 + 0.5 * pw : gx0 - 0.5 * pw, xd = s ? gx1 + 0.2 : gx0 - 0.2;
        if (cs->drums) {
            const double lo[3] = { xd - 0.21, dy + 0.02, zf - 0.55 }, hi[3] = { xd + 0.21, dy + 0.9, zf - 0.02 };
            if (open_box(d, lo, hi)) { put_front(d, PMK_H_DRUM_STONE, xd, dy, zf - 0.27, 0.0, 1.0, 1.0); hc_occupy(d, lo, hi); }
        }
        if (cs->couplets) put_front(d, PMK_H_COUPLET, xp, dy + (cs->drums ? 0.95 : 0.75), zf, 0.002, 1.0, 1.0);
        if (cs->lanterns) lantern(d, s ? gx1 + 0.3 : gx0 - 0.3, y0 - 0.17, zf - 0.32);
    }
    if (cs->couplets) {
        /* the strip laid along the lintel wall's face, centred over the gate (its length, the module's y, along the wall) */
        const double p[3] = { gt->x, dy + gt->h + 0.27, zf + 0.06 };
        hc_push_wall(d, p, FRONT_M[0], FRONT_M[1]);
        pm_translate(d->pm, 0.0f, 0.0f, 0.002f);
        pm_rotate_z(d->pm, -1.5707963f);
        pm_translate(d->pm, 0.0f, (float)(-0.5 * HK_COUPLET_H), 0.0f);
        pm_place(d->pm, PMK_H_COUPLET);
        hc_pop(d);
        put_front(d, PMK_H_FU, gt->x - 0.18 * gt->w, dy + 1.6, zLeaf, 0.003, 1.0, 1.0);
    }
    hc_done(d);
}

/* A house's door with plank leaves (China's, a hanok's): knockers; China's couplets beside it (beside the recess's mouth
 * where it stands at the back of one), the fu on a leaf, a lintel couplet over it; a pair of lanterns from the recess's
 * ceiling or under its canopy. */
static void plank_door(DC *d, const HsDoor *dr) {
    const HGraph *g = d->g;
    const int u = dr->unit;
    const HCharSet *cs = &d->C->cs[u];
    const HVol *p = porch_of(g, u);
    const double dy = unit_dy(d, u);
    const int china = g->style == HS_CHINA;
    hc_begin(d, u);
    if (dr->w >= 0.8)
        for (int s = -1; s <= 1; s += 2) put_front(d, PMK_H_KNOCKER, dr->x + s * 0.14, dr->y0 + 1.1, dr->z, -dr->recess + 0.002, 1.0, 1.0);
    if (china && cs->couplets) {
        const int rec = p && home_porch_recessed(p->kind);
        const double e0 = rec ? p->x0 : dr->x - 0.5 * dr->w, e1 = rec ? p->x1 : dr->x + 0.5 * dr->w;
        const double zf = rec ? p->z0 : dr->z, top = rec ? p->eaves + dy : dr->y1;
        for (int s = 0; s < 2; ++s) {
            const double x = s ? e1 + 0.12 + 0.5 * HK_COUPLET_W : e0 - 0.12 - 0.5 * HK_COUPLET_W;
            const double y0 = fmax(dr->y0 + 0.2, top - HK_COUPLET_H - 0.05), y1 = y0 + HK_COUPLET_H;
            const double lo[3] = { x - 0.5 * HK_COUPLET_W, y0, zf - 0.04 }, hi[3] = { x + 0.5 * HK_COUPLET_W, y1, zf - 0.005 };
            if (!open_box(d, lo, hi) || open_at(d, x, 0.5 * (y0 + y1), zf + 0.05) || hc_window_near(d, FRONT_M, -zf, x, y0, y1, 0.08)) continue;
            put_front(d, PMK_H_COUPLET, x, y0, zf, 0.002, 1.0, 1.0);
            hc_occupy(d, lo, hi);
        }
        if (dr->arch && dr->w >= 0.8) put_front(d, PMK_H_FU, dr->x - 0.25 * dr->w, dr->y0 + 1.55, dr->z, -dr->recess + 0.003, 1.0, 1.0);
    }
    if (china && cs->lanterns && p) {
        if (home_porch_recessed(p->kind) && p->x1 - p->x0 >= 1.3) {
            for (int s = 0; s < 2; ++s) lantern(d, s ? p->x1 - 0.3 : p->x0 + 0.3, p->eaves + dy - 0.01, p->z0 + 0.3);
        } else if (p->kind == HP_CANOPY && d->doorTop[u] > p->y0 + dy + 0.05) {
            const double cx = 0.5 * (p->x0 + p->x1), hw = fmax(0.35, 0.5 * (p->x1 - p->x0) - 0.15);
            for (int s = -1; s <= 1; s += 2) lantern(d, cx + s * hw, p->y0 + dy - 0.02, p->z1 - 0.38);
        }
    }
    hc_done(d);
}

/* ---------------------------------------------------------------------------
 * Korea: the pilotis
 * ------------------------------------------------------------------------- */

/* Hazard sleeves on the pilotis' columns (home_shell.c add_pilotis's: the free corners front and back, the front's middle
 * ones), the bank of mailboxes on the core's wall beside the door. */
static void pilotis(DC *d, int u, const HVol *m, const HVol *p) {
    const HGraph *g = d->g;
    const HCharSet *cs = &d->C->cs[u];
    const double dy = unit_dy(d, u), c = PILOTIS_COL;
    const int freeW = p->x0 <= m->x0 + 0.05, freeE = p->x1 >= m->x1 - 0.05;
    const double fx0 = freeW ? p->x0 + c : p->x0 + 0.12, fx1 = freeE ? p->x1 - c : p->x1 - 0.12;
    double col[8][2];
    int n = 0;
    if (freeW) { col[n][0] = p->x0 + 0.5 * c; col[n++][1] = p->z0 + 0.5 * c; col[n][0] = p->x0 + 0.5 * c; col[n++][1] = p->z1 - 0.5 * c; }
    if (freeE) { col[n][0] = p->x1 - 0.5 * c; col[n++][1] = p->z0 + 0.5 * c; col[n][0] = p->x1 - 0.5 * c; col[n++][1] = p->z1 - 0.5 * c; }
    const int nbay = (int)fmax(1.0, ceil((fx1 - fx0) / PILOTIS_SPAN));
    const double bw = (fx1 - fx0 - (nbay - 1) * c) / nbay;
    for (int k = 0; k + 1 < nbay && n < 8; ++k) { col[n][0] = fx0 + (k + 1) * bw + k * c + 0.5 * c; col[n++][1] = p->z0 + 0.5 * c; }
    hc_begin(d, u);
    if (cs->hazard) {
        for (int k = 0; k < n; ++k) {
            const double q[3] = { col[k][0], dy, col[k][1] };
            hc_push_wall(d, q, FRONT_M[0], FRONT_M[1]);
            pm_scale(d->pm, (float)c, 1.0f, (float)c);
            pm_place(d->pm, PMK_H_HAZARD);
            hc_pop(d);
        }
        /* the party wall's fin between two units' pilotis: a band round its street end (from the unit west of it) */
        if (!freeE && u + 1 < g->nunit) {
            const double t = 0.24, dz = 0.4, q[3] = { g->unit[u].x1, dy, p->z0 + 0.5 * dz };
            const double lo[3] = { q[0] - 0.5 * t - 0.03, dy, p->z0 - 0.03 }, hi[3] = { q[0] + 0.5 * t + 0.03, dy + HK_HAZARD_H, p->z0 + dz };
            /* (the yards' dividing wall often runs up to it: the band meets the wall's end) */
            if (!open_at(d, q[0] - 0.06, dy + 0.6, q[2]) && open_at(d, q[0] - 0.06, dy + HK_HAZARD_H + 0.1, p->z0 - 0.05)) {
                hc_push_wall(d, q, FRONT_M[0], FRONT_M[1]);
                pm_scale(d->pm, (float)t, 1.0f, (float)dz);
                pm_place(d->pm, PMK_H_HAZARD);
                hc_pop(d);
                hc_occupy(d, lo, hi);
            }
        }
    }
    const HUnit *un = &g->unit[u];
    if (cs->mailbox && un->door >= 0) {
        const HDoor *dr = &g->door[un->door];
        int placed = 0;
        for (int s = 0; s < 2 && !placed; ++s) {
            const double x = s ? dr->x - 0.5 * dr->width - 0.3 - 0.5 * HK_MAIL_W : dr->x + 0.5 * dr->width + 0.3 + 0.5 * HK_MAIL_W;
            if (x - 0.5 * HK_MAIL_W < fx0 + 0.15 || x + 0.5 * HK_MAIL_W > fx1 - 0.15) continue;
            const double y = dy + 0.95;
            const double lo[3] = { x - 0.5 * HK_MAIL_W, y, p->z1 - 0.2 }, hi[3] = { x + 0.5 * HK_MAIL_W, y + HK_MAIL_H + 0.03, p->z1 - 0.01 };
            if (!open_box(d, lo, hi) || open_at(d, x, y + 0.25, p->z1 + 0.05)) continue;
            put_front(d, PMK_H_MAILBOX, x, y, p->z1, 0.0, 1.0, 1.0);
            hc_occupy(d, lo, hi);
            placed = 1;
        }
        if (!placed) hc_repair(d, HDR_REGION, "no wall beside its stair door for the mailboxes (%.1f m)", u, fx1 - fx0);
    }
    hc_done(d);
}

/* ---------------------------------------------------------------------------
 * The shophouse
 * ------------------------------------------------------------------------- */

/* The five-foot way's round column at the unit's x1 party line (its core the shell's: home_arcade_core), a pilaster on a
 * row end's pier; pilasters up the upper storeys at the party lines and between the front's bays; lanterns from the walk's
 * ceiling either side of the door. */
static void shophouse(DC *d, int u, int mi, const HVol *p) {
    const HGraph *g = d->g;
    const HCharSet *cs = &d->C->cs[u];
    const HUnit *un = &g->unit[u];
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const double dy = unit_dy(d, u), P = g->plinth, sh = g->storeyH, zf = m->z0;
    hc_begin(d, u);
    if (p && p->kind == HP_ARCADE && cs->columns) {
        const double H = p->eaves - P;
        for (int side = 0; side < 2; ++side) {
            double cx, cz0, cz1, half;
            if (home_arcade_core(g, u, side, &cx, &cz0, &cz1, &half)) {
                if (side == 1) {   /* a line's column belongs to the unit on its west */
                    const double q[3] = { cx, P + dy, 0.5 * (cz0 + cz1) };
                    hc_push_wall(d, q, FRONT_M[0], FRONT_M[1]);
                    pm_scale(d->pm, 1.0f, (float)(H / HK_COL_H), 1.0f);
                    pm_place(d->pm, PMK_H_COLUMN);
                    hc_pop(d);
                }
            } else {   /* the row's end: its pier stands, a pilaster on its front */
                const double xa = side ? p->x1 : un->x0, xb = side ? un->x1 : p->x0;
                if (xb - xa > 0.25) put_front(d, PMK_H_PILASTER, 0.5 * (xa + xb), P + dy, zf, 0.0, fmin(1.0, (xb - xa) / (HK_PIL_W + 0.12)), H / HK_COL_H);
            }
        }
        if (cs->lanterns && un->door >= 0) {
            const HDoor *dr = &g->door[un->door];
            const double off = fmax(0.9, 0.5 * dr->width + 0.5), z = zf + 0.5 * (p->z1 - zf);
            for (int s = -1; s <= 1; s += 2) {
                const double x = dr->x + s * off;
                if (x > p->x0 + 0.3 && x < p->x1 - 0.3) lantern(d, x, p->eaves + dy - 0.01, z);
            }
        }
    }
    /* the upper storeys' pilasters */
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    const int first = u == 0 || un->x0 > g->vol[mi].x0 + 0.05, last = u == g->nunit - 1 || un->x1 < g->vol[mi].x1 - 0.05;
    double xs[12];
    int nx = 0;
    xs[nx++] = first ? un->x0 + 0.24 : un->x0;   /* a shared line's pilaster centred on it, the row's end's inset */
    for (int b = 1; b < nb && nx < 11; ++b) xs[nx++] = s0 + b * bw;
    if (last) xs[nx++] = un->x1 - 0.24;
    for (int k = 1; k < m->storeys; ++k) {
        const double y0 = P + k * sh + dy;
        const double y1 = k + 1 < m->storeys ? P + (k + 1) * sh - 0.12 + dy : m->eaves + dy - (m->parapet == 1 ? 0.45 : 0.25);
        if (y1 - y0 < 1.8) continue;
        for (int i = 0; i < nx; ++i) {
            if (hc_window_near(d, FRONT_M, -zf, xs[i], y0 + 0.3, y1 - 0.3, -0.02)) continue;
            if (!open_at(d, xs[i], 0.5 * (y0 + y1), zf - 0.05)) continue;
            put_front(d, PMK_H_PILASTER, xs[i], y0, zf, 0.0, 1.0, (y1 - y0) / HK_COL_H);
        }
    }
    hc_done(d);
}

/* ---------------------------------------------------------------------------
 * The link house
 * ------------------------------------------------------------------------- */

static float carport_y(double t) { return (float)(HK_CARPORT_RISE * sin(3.14159265358979 * 0.8 * t)); }

/* Whether a car porch over xa..xb, D out from the front (its face at z1), springing at ys, is clear of what stands there */
static int carport_clear(const DC *d, double xa, double xb, double z1, double D, double ys, double dy) {
    static const double TX[3] = { 0.05, 0.5, 0.97 };
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) {
            const double x = xa + 0.15 + (xb - xa - 0.3) * 0.5 * i, t = TX[k];
            if (!open_at(d, x, ys + carport_y(t) - 0.06, z1 - D * t)) return 0;
            if (!open_at(d, x, ys + carport_y(t) + 0.06, z1 - D * t - 0.02)) return 0;
        }
    for (int s = 0; s < 2; ++s)
        for (double y = dy + 0.3; y < ys; y += 0.6)
            if (!open_at(d, s ? xb - 0.12 : xa + 0.12, y, z1 - D + 0.1)) return 0;
    const double lo[3] = { xa, ys - 0.12, z1 - D }, hi[3] = { xb, ys + HK_CARPORT_RISE + 0.05, z1 - 0.05 };
    return hc_free(d, lo, hi);
}

/* The car porch's curved canopy from the front wall over the pad (the whole front where it is clear, else the pad), two
 * posts at its front; a roundel in a gable the street or a side sees. */
static void linkhouse(DC *d, int u, int mi) {
    const HGraph *g = d->g;
    const HCharSet *cs = &d->C->cs[u];
    const HUnit *un = &g->unit[u];
    const double dy = unit_dy(d, u);
    hc_begin(d, u);
    float px0, px1, pz1;
    if (cs->carport && home_unit_pad(g, u, &px0, &px1, &pz1)) {
        const double z1 = g->vol[mi].z0, D = fmin(6.0, z1 - 0.4), ys = g->plinth + 2.45 + dy;
        double xa = un->x0 + 0.12, xb = un->x1 - 0.12;
        int ok = D >= 3.7 && carport_clear(d, xa, xb, z1, D, ys, dy);
        if (!ok && D >= 3.7) { xa = px0 + 0.05; xb = px1 - 0.05; ok = xb - xa > 2.2 && carport_clear(d, xa, xb, z1, D, ys, dy); }
        if (ok) {
            const double L = hc_push_run_wall(d, FRONT_M, FRONT_E, -z1, xa, xb, ys, 0.0);
            pm_scale(d->pm, 1.0f, 1.0f, (float)(D / HK_CARPORT_D));
            hk_run(d->pm, PMK_H_CARPORT, (float)L, 1.6f);
            hc_pop(d);
            const double top = ys + carport_y(1.0) - 0.14;
            for (int s = 0; s < 2; ++s) {
                const double q[3] = { s ? xb - 0.12 : xa + 0.12, dy, z1 - D + 0.1 };
                hc_push_wall(d, q, FRONT_M[0], FRONT_M[1]);
                pm_scale(d->pm, 1.0f, (float)((top - dy) / HK_POST_H), 1.0f);
                pm_place(d->pm, PMK_H_POST_SQUARE);
                hc_pop(d);
                const double lo[3] = { q[0] - 0.15, dy, q[2] - 0.15 }, hi[3] = { q[0] + 0.15, top, q[2] + 0.15 };
                hc_occupy(d, lo, hi);
            }
            const double lo[3] = { xa, ys - 0.15, z1 - D - 0.05 }, hi[3] = { xb, ys + HK_CARPORT_RISE + 0.1, z1 };
            hc_occupy(d, lo, hi);
        } else hc_repair(d, HDR_REGION, "no room for the car porch over a %.1f m front yard", u, z1);
    }
    if (cs->gableVent) {
        int done = 0;
        for (int i = 0; i < d->C->nroof && !done; ++i) {
            const HsRoof *hr = &d->C->roof[i];
            const Roof *R = &hr->R;
            if (hr->unit != u || !hc_is_house_body(g->vol[hr->vol].role) || R->kind != HR_GABLE || R->F.n != 4) continue;
            for (int f = 0; f < 4 && !done; ++f) {
                if (R->role[f] != ER_VERGE || R->cut[f] || R->W.m[f][1] > 0.7) continue;   /* not the back's */
                const double *a = R->W.p[f], *b = R->W.p[(f + 1) % 4], *mm = R->W.m[f];
                const double half = 0.5 * sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
                if (R->t * half < 1.1) continue;
                const double y = R->E + hr->dy + 0.42 * R->t * half;
                const double q[3] = { 0.5 * (a[0] + b[0]), y, 0.5 * (a[1] + b[1]) };
                if (!open_at(d, q[0] + mm[0] * 0.08, y, q[2] + mm[1] * 0.08) || open_at(d, q[0] - mm[0] * 0.06, y, q[2] - mm[1] * 0.06)) continue;
                if (hc_window_near(d, mm, mm[0] * q[0] + mm[1] * q[2], -mm[1] * q[0] + mm[0] * q[2], y - HK_VENT_R, y + HK_VENT_R, 0.1)) continue;
                hc_push_wall(d, q, mm[0], mm[1]);
                pm_place(d->pm, PMK_H_GABLE_VENT);
                hc_pop(d);
                done = 1;
            }
        }
    }
    hc_done(d);
}

/* ---------------------------------------------------------------------------
 * Japan
 * ------------------------------------------------------------------------- */

/* A slat screen (panel 5) on the front beside the door or its porch, where the wall is bare. */
static void japan_slats(DC *d, int u, int mi) {
    const HGraph *g = d->g;
    const HUnit *un = &g->unit[u];
    if (un->door < 0) return;
    const HDoor *dr = &g->door[un->door];
    const HVol *p = porch_of(g, u);
    const double dy = unit_dy(d, u), zf = g->vol[mi].z0, W = 1.0, H = 2.3, y0 = dy + 0.1;
    double e0 = dr->x - 0.5 * dr->width, e1 = dr->x + 0.5 * dr->width;
    if (p) { e0 = fmin(e0, p->x0); e1 = fmax(e1, p->x1); }
    hc_begin(d, u);
    int placed = 0;
    for (int s = 0; s < 2 && !placed; ++s) {
        const double xa = s ? e0 - 0.25 - W : e1 + 0.25, xb = xa + W;
        if (xa < un->x0 + 0.15 || xb > un->x1 - 0.15) continue;
        const double lo[3] = { xa, y0, zf - 0.08 }, hi[3] = { xb, y0 + H, zf - 0.005 };
        if (!open_box(d, lo, hi) || open_at(d, 0.5 * (xa + xb), y0 + 1.0, zf + 0.05)) continue;
        if (hc_window_near(d, FRONT_M, -zf, 0.5 * (xa + xb), y0, y0 + H, 0.5 * W + 0.1)) continue;
        const double L = hc_push_run_wall(d, FRONT_M, FRONT_E, -zf, xa, xb, y0, 0.0);
        pm_scale(d->pm, (float)L, (float)H, 1.0f);
        pm_place(d->pm, PMK_H_SLATS);
        hc_pop(d);
        hc_occupy(d, lo, hi);
        placed = 1;
    }
    if (!placed) hc_repair(d, HDR_REGION, "no bare wall beside the door for a slat screen (%.1f m front)", u, un->x1 - un->x0);
    hc_done(d);
}

/* ---------------------------------------------------------------------------
 * The driver
 * ------------------------------------------------------------------------- */

void hs_region(DC *d) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    const int mi = home_main(g);
    if (mi < 0) return;
    const int hanok = home_hanok(g);
    /* the doors with plank leaves: China's, a hanok's */
    if (g->style == HS_CHINA || hanok)
        for (int i = 0; i < C->ndoor; ++i)
            if (!C->door[i].porch && C->door[i].arch) plank_door(d, &C->door[i]);
    if (g->style == HS_CHINA || hanok)
        for (int i = 0; i < g->ngate; ++i)
            if (g->gate[i].house && g->gate[i].unit >= 0 && g->gate[i].unit < g->nunit) gatehouse(d, &g->gate[i]);
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) {
        const HCharSet *cs = &C->cs[u];
        const HVol *p = porch_of(g, u);
        switch (g->style) {
        case HS_KOREA:
            if (p && p->kind == HP_PILOTIS && (cs->hazard || cs->mailbox)) pilotis(d, u, &g->vol[mi], p);
            break;
        case HS_SHOPHOUSE: shophouse(d, u, mi, p); break;
        case HS_LINKHOUSE: linkhouse(d, u, mi); break;
        case HS_JAPAN: if (cs->slats) japan_slats(d, u, mi); break;
        default: break;
        }
    }
    (void)ch;
}
