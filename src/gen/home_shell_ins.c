#include "home_shell_internal.h"
#include <math.h>
#include <string.h>

#define PI_D 3.14159265358979323846

/* ===========================================================================
 * The retrofit insertions' shells (residential roadmap HM5c2a; the graph's HInsert, placed by home_insert.c).
 *
 * How an insertion joins its house: each is convex solids (SK_INS) added before the house's windows are cut, so a window
 * behind one is never cut (add_windows' box_blocked sees them) and the wall behind is removed by the CSG, as a balcony's
 * slab joins it; its back runs 0.05 m into the wall. Its own windows are openings carved into it, their reveals its finish.
 *
 *   concrete box   one solid, a window a storey in its front (and, on a deep one, a narrow one in its sides)
 *   screen box     one solid, a recess a storey across its front with glass at the back; the kit's breeze blocks stand in
 *                  the recess's mouth (hs_inserts)
 *   container      one solid of corrugated sheet (the roof's metal on its walls), a window or two
 *   glass box      floor bands and corner posts of painted steel, glass between the bands inset 0.05 m: glass on a solid's
 *                  faces, each face a pane with its uv and size (emit_all), mullions painted (HG_CURTAIN)
 *   rounded bay    one convex plan (the front's corners rounded, chords of 3-6 a quarter): full bands (plinth, spandrels,
 *                  heads) and glass bands 0.08 m in, a cornice slab 0.1 m out over the top; each facet a pane
 * Every pane of one insertion's storey shares its key (HOUSE_AUX_SHARED), so a ribbon window is lit as one room.
 * Far: one solid each, a box's walls with the house's windows painted, a glass box's glass, a rounded bay's plan at two
 * chords a quarter.
 * ========================================================================= */

#define GLASS_IN  0.08   /* a rounded bay's ribbon glass, in from its bands */
#define CORNICE   0.10   /* its cornice, out past them */
#define POST      0.10   /* a glass box's corner posts */
#define BOX_GLASS 0.05   /* its glass, in from its posts and bands */
#define INTO_WALL 0.05   /* how far an insertion's back runs into the wall */

/* A plan: its edges m.(x, z) <= d (m outward); the back's (in the wall) has no face. */
typedef struct Plan { int n; double m[20][2], d[20]; uint8_t back[20]; } Plan;

static void edge(Plan *P, double mx, double mz, double d, int back) {
    if (P->n >= 20) return;
    P->m[P->n][0] = mx; P->m[P->n][1] = mz; P->d[P->n] = d; P->back[P->n] = (uint8_t)back;
    P->n++;
}

static void plan_box(const HInsert *in, Plan *P) {
    P->n = 0;
    edge(P, -1.0, 0.0, -in->x0, 0);
    edge(P, 1.0, 0.0, in->x1, 0);
    edge(P, 0.0, -1.0, -(in->z - in->depth), 0);
    edge(P, 0.0, 1.0, in->z + INTO_WALL, 1);
}

/* the front's corners rounded by in->r: K chords a quarter, their corners on the arc */
static void plan_round(const HInsert *in, int K, Plan *P) {
    const double r = in->r, zf = in->z - in->depth, half = 0.5 * PI_D / K;
    P->n = 0;
    edge(P, -1.0, 0.0, -in->x0, 0);
    for (int q = 0; q < 2; ++q) {
        const double cx = q ? in->x1 - r : in->x0 + r, cz = zf + r, a0 = q ? 1.5 * PI_D : PI_D;
        for (int k = 0; k < K; ++k) {
            const double am = a0 + (2 * k + 1) * half, mx = cos(am), mz = sin(am);
            edge(P, mx, mz, mx * cx + mz * cz + r * cos(half), 0);
        }
        if (!q) edge(P, 0.0, -1.0, -zf, 0);
    }
    edge(P, 1.0, 0.0, in->x1, 0);
    edge(P, 0.0, 1.0, in->z + INTO_WALL, 1);
}

/* A solid of the plan, its faces pushed out by off (in, if negative; the back stays in the wall), ya..yb. */
static int plan_solid(Ctx *C, const HInsert *in, const Plan *P, double off, double ya, double yb, int side, int top, int bot) {
    Solid *s = hs_solid_new(C, in->unit, -1, 0);
    s->kind = SK_INS;
    s->wallFin = in->fin; s->wallCol = in->col; s->trimCol = in->col;
    for (int e = 0; e < P->n; ++e) {
        const double d = P->back[e] ? P->d[e] : P->d[e] + off;
        hs_plane(s, P->m[e][0], 0.0, P->m[e][1], P->m[e][0] * d, 0.0, P->m[e][1] * d, P->back[e] ? NOFACE : side);
    }
    hs_plane(s, 0, 1, 0, 0, yb, 0, top);
    hs_plane(s, 0, -1, 0, 0, ya, 0, bot);
    for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
    hs_lift(s, C->dy ? C->dy[in->unit] : 0.0);
    return (int)(s - C->s);
}

/* A box x0..x1 by z0..z1 (its back, if `back`, in the wall without a face), ya..yb. */
static int box_at(Ctx *C, const HInsert *in, double x0, double z0, double x1, double z1, double ya, double yb, int surf, int bot, int back) {
    Plan P;
    P.n = 0;
    edge(&P, -1.0, 0.0, -x0, 0);
    edge(&P, 1.0, 0.0, x1, 0);
    edge(&P, 0.0, -1.0, -z0, 0);
    edge(&P, 0.0, 1.0, z1, back);
    return plan_solid(C, in, &P, 0.0, ya, yb, surf, surf, bot);
}

/* an opening's reveals and frame: the insertion's finish, a modern frame's paint */
static void opening_finish(Ctx *C, int o, const HInsert *in, int wallFin, int wallCol) {
    if (o < 0) return;
    C->s[o].wallFin = (uint8_t)wallFin; C->s[o].wallCol = (uint8_t)wallCol;
    /* white or grey aluminium (dark frames vanish against glass by day) */
    C->s[o].trimCol = (uint8_t)(in->face == HIF_GLASS ? in->col : (hash_combine((uint32_t)in->unit * 31u + (uint32_t)(in->x0 * 10.0f), 0xF4A3u) & 1u) ? 0 : 4);
}

/* The aux of glass shared by an insertion's storey (house_surf.h HOUSE_AUX_SHARED): its pattern and the insertion */
static uint32_t shared_aux(int i, int pattern) {
    return (uint32_t)(pattern & 15) << HOUSE_AUX_PATTERN | HOUSE_AUX_SHARED | (uint32_t)(i & 15) << HOUSE_AUX_INS;
}

/* the far wall's window rhythm (home_shell.c's window_rhythm, for its unit's storeys) */
static uint32_t far_rhythm(const HGraph *g, int storeys) {
    const uint32_t sh = (uint32_t)fmax(0.0, fmin(127.0, (g->storeyH - 2.0f) * 100.0f + 0.5f));
    const uint32_t pl = (uint32_t)fmax(0.0, fmin(255.0, g->plinth * 100.0f + 0.5f));
    return sh << HOUSE_AUX_STOREYH | pl << HOUSE_AUX_PLINTH | (uint32_t)((storeys > 0 ? storeys - 1 : 0) & 3) << HOUSE_AUX_STOREYS;
}

static void add_far(Ctx *C, const HInsert *in, int idx, double y0, double y1) {
    const HGraph *g = C->g;
    const int ground = in->s0 == 0, bot = ground ? NOFACE : HF_WALL;
    Plan P;
    if (in->kind == HI_ROUND) {
        plan_round(in, 2, &P);
        plan_solid(C, in, &P, 0.0, y0, y1, HF_WALL, HF_COPING, bot);
        return;
    }
    plan_box(in, &P);
    switch (in->face) {
    case HIF_GLASS: {
        const int s = plan_solid(C, in, &P, 0.0, y0, y1, HF_GLASS, HF_TRIM, ground ? NOFACE : HF_TRIM);
        C->s[s].aux = shared_aux(idx, HG_CURTAIN);
        break;
    }
    case HIF_CONTAINER: {
        const int s = plan_solid(C, in, &P, 0.0, y0, y1, HF_ROOF, HF_ROOF, ground ? NOFACE : HF_ROOF);
        C->s[s].roofFin = HRM_METAL; C->s[s].roofCol = in->col;
        break;
    }
    default: {
        /* the house's windows painted on its walls, at its storeys */
        const int s = plan_solid(C, in, &P, 0.0, y0, y1, HF_WALL, HF_COPING, bot);
        HVol own;
        const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
        Solid *S = &C->s[s];
        S->aux = far_rhythm(g, m->storeys);
        for (int i = 0; i < S->np; ++i) S->noWin[i] = S->surf[i] != HF_WALL || fabs(S->pl[i].n[1]) > 0.5;
        break;
    }
    }
}

/* A concrete, screen or container box near: its solid and its openings. */
static void add_box(Ctx *C, const HInsert *in, int idx, double y0, double y1) {
    const HGraph *g = C->g;
    const int u = in->unit, ground = in->s0 == 0;
    const double zf = in->z - in->depth, W = in->x1 - in->x0;
    const double fm[2] = { 0.0, -1.0 }, fe[2] = { 1.0, 0.0 };
    Plan P;
    plan_box(in, &P);
    const int cont = in->face == HIF_CONTAINER;
    const int bi = cont ? plan_solid(C, in, &P, 0.0, y0, y1, HF_ROOF, HF_ROOF, ground ? NOFACE : HF_ROOF)
                        : plan_solid(C, in, &P, 0.0, y0, y1, HF_WALL, HF_COPING, ground ? NOFACE : HF_WALL);
    /* a container's reveals: its steel's dark grey */
    const int rFin = cont ? HW_BLOCK : in->fin, rCol = cont ? 1 : in->col;
    if (cont) { C->s[bi].roofFin = HRM_METAL; C->s[bi].roofCol = in->col; C->s[bi].wallFin = HW_BLOCK; C->s[bi].wallCol = 1; }
    for (int k = in->s0; k <= in->s1; ++k) {
        if (in->face == HIF_SCREEN) {
            /* a recess the storey's height, glass at its back (hs_inserts stands the breeze blocks in its mouth) */
            const double ya = k == in->s0 ? y0 + 0.2 : g->plinth + k * g->storeyH + 0.12;
            const double yb = k == in->s1 ? y1 - 0.2 : g->plinth + (k + 1) * g->storeyH - 0.18;
            if (yb - ya < 0.6) continue;
            const double w = W - 0.34, deep = fmin(0.4, in->depth - 0.15);
            const int o = hs_opening(C, bi, u, fm, fe, in->x0 + 0.17, zf, w, ya, yb, deep, HF_GLASS, shared_aux(idx, HG_CURTAIN) | (hs_opening_aux(w, yb - ya, 0) & 0xFFFFFu));
            opening_finish(C, o, in, rFin, rCol);
            continue;
        }
        float sl, hd;
        if (!home_insert_window(g, in, k, &sl, &hd)) continue;
        if (cont && hd - sl > 1.2f) sl = hd - 1.2f;
        /* the front: one wide window (a container's: one, or two on a long one) */
        const int two = cont && W > 3.0;
        const double ww = cont ? fmin(1.4, two ? 0.5 * W - 0.7 : W - 0.8) : fmax(0.6, fmin(2.6, W - 0.7));
        for (int q = 0; q <= two; ++q) {
            const double c = two ? in->x0 + W * (q ? 0.72 : 0.28) : 0.5 * (in->x0 + in->x1);
            const int o = hs_opening(C, bi, u, fm, fe, c - 0.5 * ww, zf, ww, sl, hd, cont ? 0.08 : 0.1, HF_GLASS,
                                     hs_opening_aux(ww, hd - sl, ww > 1.3 ? HG_CURTAIN : HG_CASEMENT));
            opening_finish(C, o, in, rFin, rCol);
        }
        /* a deep concrete box's sides: a narrow window in one or both, a time in two */
        if (!cont && in->depth >= 0.9f && (hash_combine(g->unit[u].seed, 0x51D0u + (uint32_t)k + (uint32_t)idx * 13u) & 1u)) {
            const double sw = fmin(0.55, in->depth - 0.45), zm = zf + 0.5 * in->depth;
            const double ml[2] = { -1.0, 0.0 }, el[2] = { 0.0, -1.0 }, mr[2] = { 1.0, 0.0 }, er[2] = { 0.0, 1.0 };
            opening_finish(C, hs_opening(C, bi, u, ml, el, in->x0, zm + 0.5 * sw, sw, sl, hd, 0.1, HF_GLASS, hs_opening_aux(sw, hd - sl, HG_PLAIN)), in, rFin, rCol);
            opening_finish(C, hs_opening(C, bi, u, mr, er, in->x1, zm - 0.5 * sw, sw, sl, hd, 0.1, HF_GLASS, hs_opening_aux(sw, hd - sl, HG_PLAIN)), in, rFin, rCol);
        }
    }
}

/* A steel-and-glass box near: bands at its foot, its floors and its top, posts at its corners, glass between. */
static void add_glass(Ctx *C, const HInsert *in, int idx, double y0, double y1) {
    const HGraph *g = C->g;
    const int ground = in->s0 == 0;
    const double zf = in->z - in->depth;
    Plan P;
    plan_box(in, &P);
    const double base = ground ? g->plinth + 0.3 : g->plinth + in->s0 * g->storeyH + 0.08;
    plan_solid(C, in, &P, 0.0, y0, base, HF_TRIM, HF_TRIM, ground ? NOFACE : HF_TRIM);
    for (int k = in->s0 + 1; k <= in->s1; ++k) {
        const double f = g->plinth + k * g->storeyH;
        plan_solid(C, in, &P, 0.0, f - 0.22, f + 0.08, HF_TRIM, HF_TRIM, HF_TRIM);
    }
    plan_solid(C, in, &P, 0.0, y1 - 0.25, y1, HF_TRIM, HF_TRIM, HF_TRIM);
    for (int k = in->s0; k <= in->s1; ++k) {
        const double lo = k == in->s0 ? base : g->plinth + k * g->storeyH + 0.08;
        const double hi = k == in->s1 ? y1 - 0.25 : g->plinth + (k + 1) * g->storeyH - 0.22;
        if (hi - lo < 0.5) continue;
        const int s = plan_solid(C, in, &P, -BOX_GLASS, lo, hi, HF_GLASS, NOFACE, NOFACE);
        C->s[s].aux = shared_aux(idx, HG_CURTAIN);
    }
    const double xs[2][2] = { { in->x0, in->x0 + POST }, { in->x1 - POST, in->x1 } }, zs[2][2] = { { zf, zf + POST }, { in->z - POST, in->z + INTO_WALL } };
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) box_at(C, in, xs[a][0], zs[b][0], xs[a][1], zs[b][1], y0, y1, HF_TRIM, ground ? NOFACE : HF_TRIM, b);
}

/* A rounded bay near: bands of one plan, the glass between them in, a cornice out over its top. */
static void add_round(Ctx *C, const HInsert *in, int idx, double y0, double y1) {
    const HGraph *g = C->g;
    const int ground = in->s0 == 0;
    int K = (int)floor(in->r / 0.3 + 0.5);
    K = K < 3 ? 3 : K > 6 ? 6 : K;
    Plan P;
    plan_round(in, K, &P);
    const double top = y1 - 0.18;
    double cur = y0;
    for (int k = in->s0; k <= in->s1; ++k) {
        float sl, hd;
        if (!home_insert_window(g, in, k, &sl, &hd) || hd > top - 0.05) continue;
        if (sl - cur > 0.02) plan_solid(C, in, &P, 0.0, cur, sl, HF_WALL, HF_WALL, cur <= y0 && ground ? NOFACE : HF_WALL);
        const int s = plan_solid(C, in, &P, -GLASS_IN, sl, hd, HF_GLASS, NOFACE, NOFACE);
        C->s[s].aux = shared_aux(idx, HG_CURTAIN);
        C->s[s].trimCol = (uint8_t)(hash_combine((uint32_t)in->unit, 0x7C0Du + (uint32_t)idx) % 3u == 0u ? 0 : 4);   /* white or grey frames */
        cur = hd;
    }
    plan_solid(C, in, &P, 0.0, cur, top, HF_WALL, HF_WALL, cur <= y0 && ground ? NOFACE : HF_WALL);
    plan_solid(C, in, &P, CORNICE, top, y1, HF_COPING, HF_COPING, HF_WALL);
}

/* HM5c2b: a member kind's members (home_insert_members), each a convex solid of the insertion's concrete: its faces in the
 * wall none, its upper faces copings; a hexagon's glass a pane on its front only (the rings bury its sides). Far, only the
 * honeycomb's glass: the frames', fins' and strips' members are under a pixel there. */
static void add_members(Ctx *C, const HInsert *in, int idx) {
    const HGraph *g = C->g;
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    const double dy = C->dy ? C->dy[in->unit] : 0.0;
    /* HM5c2c1: a stair's treads and landings are steps (tiled on some of the Southern European and colonial ones), a steel
     * stair painted steel all over; far, its flights' slabs, stringers, landings, upstands and columns (not its steps) */
    const int stair = in->kind == HI_STAIRS, steel = stair && in->cage;
    const uint32_t sh = hash_combine((uint32_t)in->unit, 0x7C0Eu + (uint32_t)idx);
    const int side = steel ? HF_TRIM : HF_WALL;
    float wex, wez, wnx, wnz, fx0, fz0;   /* its wall's frame: along, out, the face at 0 along it */
    home_ins_axes(in, &wex, &wez, &wnx, &wnz);
    home_ins_point(in, 0.0f, 0.0f, &fx0, &fz0);
    for (int k = 0; k < n; ++k) {
        const HMember *e = &M[k];
        const int glass = e->role == HMR_GLASS;
        const int big = e->role == HMR_FLIGHT || e->role == HMR_LANDING || e->role == HMR_UPSTAND || e->role == HMR_LEG || e->role == HMR_STRINGER;
        if (C->lod == HSH_FAR && !glass && !(stair && big)) continue;
        const int stepTop = stair && !steel && (e->role == HMR_STEP || e->role == HMR_LANDING || e->role == HMR_FLIGHT);
        const int top = steel ? HF_TRIM : stepTop ? HF_STEP : HF_COPING;
        Solid *s = hs_solid_new(C, in->unit, -1, 0);
        s->kind = SK_INS;
        s->wallFin = in->fin; s->wallCol = in->col; s->trimCol = (uint8_t)(steel ? in->col : sh % 3u == 0u ? 0 : 4);
        s->stepFin = (uint8_t)((g->style == HS_SEU_VILLA || g->style == HS_COLONIAL) && (sh >> 4 & 1u) ? HST_TILE : HST_CONCRETE);
        for (int i = 0; i < e->n; ++i) {
            const int j = (i + 1) % e->n;
            const double ex = e->p[j][0] - e->p[i][0], ey = e->p[j][1] - e->p[i][1], L = sqrt(ex * ex + ey * ey);
            if (L < 1e-6) continue;
            const double nx = ey / L, ny = -ex / L;   /* outward of an anticlockwise outline */
            if (e->axis == HMB_ELEV) {
                const int sunk = ny < -0.7 && e->p[i][1] <= 0.01f;   /* a leg's foot on the ground */
                const int surf = glass || sunk ? NOFACE : ny > 0.7 ? top : side;
                /* (along its wall: HM5c2c1's stairs stand against any wall; the rest the front, e (1, 0)) */
                hs_plane(s, nx * wex, ny, nx * wez, fx0 + e->p[i][0] * wex, e->p[i][1], fz0 + e->p[i][0] * wez, surf);
            } else {
                /* plan: (x, out) to (x, z = in->z - out): the out axis turned over */
                const int inwall = e->p[i][1] <= 0.0f && e->p[j][1] <= 0.0f;
                hs_plane(s, nx, 0.0, -ny, e->p[i][0], 0.0, in->z - e->p[i][1], inwall ? NOFACE : HF_WALL);
            }
        }
        if (e->axis == HMB_ELEV) {
            hs_plane(s, wnx, 0.0, wnz, fx0 + e->a1 * wnx, 0.0, fz0 + e->a1 * wnz, glass ? HF_GLASS : side);
            hs_plane(s, -wnx, 0.0, -wnz, fx0 + e->a0 * wnx, 0.0, fz0 + e->a0 * wnz, e->a0 <= 0.0f ? NOFACE : side);
        } else {
            hs_plane(s, 0.0, 1.0, 0.0, 0.0, e->a1, 0.0, HF_COPING);
            hs_plane(s, 0.0, -1.0, 0.0, 0.0, e->a0, 0.0, e->a0 < 0.05f ? NOFACE : HF_WALL);
        }
        for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
        if (glass) s->aux = shared_aux(idx, HG_PLAIN);
        hs_lift(s, dy);
    }
}

/* HM5c2c1: a stair's doors, cut in the main's body at its landings (near): the unit's door colour, panelled or glazed over */
static void add_stair_doors(Ctx *C, const HInsert *in, int idx) {
    const HGraph *g = C->g;
    const int host = C->body[in->vol][in->unit];
    if (host < 0) return;
    int fk[4];
    float fx[4];
    const int n = home_insert_doors(g, in, fk, fx, 4);
    /* its wall's outward normal m and the way along it an opening runs, e = (-m.z, m.x) (add_windows' convention) */
    float ex, ez, nx, nz;
    home_ins_axes(in, &ex, &ez, &nx, &nz);
    const double m[2] = { nx, nz }, e[2] = { -nz, nx };
    const int pat = (int)(hash_combine((uint32_t)in->unit, 0xD00Cu + (uint32_t)idx) & 1u);
    for (int k = 0; k < n; ++k) {
        const double f = g->plinth + fk[k] * g->storeyH;
        float cx, cz;
        home_ins_point(in, fx[k], 0.0f, &cx, &cz);
        hs_opening(C, host, in->unit, m, e, cx - e[0] * 0.5 * INS_DOOR_W, cz - e[1] * 0.5 * INS_DOOR_W, INS_DOOR_W, f, f + INS_DOOR_H, 0.12, HF_DOOR,
                   hs_opening_aux(INS_DOOR_W, INS_DOOR_H, pat));
        C->st.doors++;
    }
}

/* A box of the tower's x0..x1, z0..z1, ya..yb, every side a face (surf), its top `top` and bottom `bot` */
static int tower_box(Ctx *C, const HInsert *in, double x0, double z0, double x1, double z1, double ya, double yb, int surf, int top, int bot, int back) {
    Solid *s = hs_solid_new(C, in->unit, -1, 0);
    s->kind = SK_INS;
    s->wallFin = in->fin; s->wallCol = in->col; s->trimCol = in->col;
    hs_plane(s, -1, 0, 0, x0, 0, 0, surf);
    hs_plane(s, 1, 0, 0, x1, 0, 0, surf);
    hs_plane(s, 0, 0, -1, 0, 0, z0, surf);
    hs_plane(s, 0, 0, 1, 0, 0, z1, back);
    hs_plane(s, 0, 1, 0, 0, yb, 0, top);
    hs_plane(s, 0, -1, 0, 0, ya, 0, bot);
    for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
    hs_lift(s, C->dy ? C->dy[in->unit] : 0.0);
    return (int)(s - C->s);
}

/* A glass stair tower (HM5c2c1; Georgian 5, brownstone 6): painted steel posts at its corners and bands at its foot, its
 * floors and its top, glass between with a stairwell behind it (HG_STAIR; the ground storey's front its door, HG_STAIR_DOOR);
 * over a flat roof or a parapet a head runs back over the roof (home_tower_back), its back steel with a door out onto a deck;
 * a cap over it all. Far: its glass, its head and its cap. */
static void add_tower(Ctx *C, const HInsert *in, int idx, double y0, double y1) {
    const HGraph *g = C->g;
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    const double back = home_tower_back(g, in), zf = in->z - in->depth, zw = in->z + INTO_WALL;
    const double zb = back > 0.0 ? in->z + back : zw;   /* the head's back */
    const double eav = back > 0.0 ? m->eaves : y1;      /* the shaft against the wall; the head over it */
    const double x0 = in->x0, x1 = in->x1, G = BOX_GLASS, top = y1 - 0.25;
    const int far = C->lod == HSH_FAR;
    const uint32_t aux = shared_aux(idx, HG_STAIR), auxDoor = shared_aux(idx, HG_STAIR_DOOR);
    /* the cap, proud of it all round */
    tower_box(C, in, x0 - 0.08, zf - 0.08, x1 + 0.08, back > 0.0 ? zb + 0.08 : zw, y1, y1 + 0.14, HF_TRIM, HF_COPING, HF_TRIM, back > 0.0 ? HF_TRIM : NOFACE);
    if (far) {
        const int s = tower_box(C, in, x0, zf, x1, zw, 0.0, eav, HF_GLASS, HF_TRIM, NOFACE, NOFACE);
        C->s[s].aux = aux;
        if (back > 0.0) tower_box(C, in, x0, zf, x1, zb, eav, y1, HF_TRIM, HF_TRIM, HF_TRIM, HF_TRIM);
        return;
    }
    /* the bands: its foot, each floor, the head's floor, its top */
    const double foot = 0.3;
    tower_box(C, in, x0, zf, x1, zw, 0.0, foot, HF_TRIM, HF_TRIM, NOFACE, NOFACE);
    for (int k = 1; k <= in->s1; ++k) {
        const double f = g->plinth + k * g->storeyH;
        if (f + 0.08 < eav - 0.3) tower_box(C, in, x0, zf, x1, zw, f - 0.22, f + 0.08, HF_TRIM, HF_TRIM, HF_TRIM, NOFACE);
    }
    if (back > 0.0) tower_box(C, in, x0, zf, x1, zb, eav - 0.2, eav + 0.08, HF_TRIM, HF_TRIM, HF_TRIM, HF_TRIM);
    tower_box(C, in, x0, zf, x1, back > 0.0 ? zb : zw, top, y1, HF_TRIM, HF_TRIM, HF_TRIM, back > 0.0 ? HF_TRIM : NOFACE);
    /* the glass a storey at a time; the ground storey's front a pane of its own (its door) */
    const double shaftTop = back > 0.0 ? eav - 0.2 : top;
    for (int k = 0; k <= in->s1; ++k) {
        const double lo = k == 0 ? foot : g->plinth + k * g->storeyH + 0.08;
        const double hi = k == in->s1 ? shaftTop : g->plinth + (k + 1) * g->storeyH - 0.22;
        if (hi - lo < 0.5) continue;
        if (k == 0) {
            const int d = tower_box(C, in, x0 + G, zf + G, x1 - G, zf + 0.15, lo, hi, HF_GLASS, NOFACE, NOFACE, NOFACE);
            C->s[d].aux = auxDoor;
            const int s = tower_box(C, in, x0 + G, zf + 0.15, x1 - G, zw, lo, hi, HF_GLASS, NOFACE, NOFACE, NOFACE);
            C->s[s].aux = aux;
            /* (its front, at zf + 0.15, lies inside the door's pane: no face shows there) */
            continue;
        }
        const int s = tower_box(C, in, x0 + G, zf + G, x1 - G, zw, lo, hi, HF_GLASS, NOFACE, NOFACE, NOFACE);
        C->s[s].aux = aux;
    }
    if (back > 0.0) {
        /* the head: glass at its front and sides, steel at its back with the door out onto a flat roof's deck */
        const int s = tower_box(C, in, x0 + G, zf + G, x1 - G, zb - G, eav + 0.08, top, HF_GLASS, NOFACE, NOFACE, HF_TRIM);
        C->s[s].aux = aux;
        if (m->roof == HR_FLAT && x1 - x0 > 1.3) {
            const double mb[2] = { 0.0, 1.0 }, eb[2] = { -1.0, 0.0 }, dw = 0.85;
            const double c = 0.5 * (x0 + x1);
            hs_opening(C, s, in->unit, mb, eb, c + 0.5 * dw, zb - G, dw, eav + 0.08, fmin(eav + 2.1, top - 0.05), 0.06, HF_DOOR,
                       hs_opening_aux(dw, fmin(2.02, top - 0.05 - eav - 0.08), 1));
        }
    }
    /* the posts at its corners: the front ones its full height, the back ones in the wall, the head's at its back */
    const double P = POST;
    box_at(C, in, x0, zf, x0 + P, zf + P, 0.0, y1, HF_TRIM, NOFACE, 0);
    box_at(C, in, x1 - P, zf, x1, zf + P, 0.0, y1, HF_TRIM, NOFACE, 0);
    box_at(C, in, x0, in->z - P, x0 + P, zw, 0.0, eav, HF_TRIM, NOFACE, 1);
    box_at(C, in, x1 - P, in->z - P, x1, zw, 0.0, eav, HF_TRIM, NOFACE, 1);
    if (back > 0.0) {
        tower_box(C, in, x0, zb - P, x0 + P, zb, eav - 0.2, y1, HF_TRIM, HF_TRIM, NOFACE, HF_TRIM);
        tower_box(C, in, x1 - P, zb - P, x1, zb, eav - 0.2, y1, HF_TRIM, HF_TRIM, NOFACE, HF_TRIM);
    }
}

/* ===========================================================================
 * HM5c2c2: the roof's kinds (home_insert_roof.c places them)
 *
 *   unfinished storey  its columns first (their faces show where the walls run flush past them), cast concrete, to the
 *                      next floor's slab; its walls of bare block or brick on the deck's edge (on the parapets), each to its
 *                      own height, holes through them where its windows will go (a notch where a wall stops under their
 *                      heads). Far: its walls.
 *   roof room          concrete and screen rooms: a body and a roof slab proud of it (out over the door on a flat deck), a
 *                      door and windows (a screen room's front a recess the blocks stand in); a container: corrugated steel
 *                      all round, its door and windows cut; glass: a foot band, a roof band, glass between and posts at
 *                      its square corners (a drum's front rounded). A loft room's box runs down into the roof, which
 *                      hides its foot. Far: one solid of its make.
 *   stubs              0.3 m columns of cast concrete standing on the deck (near).
 * ========================================================================= */

#define ROOM_SLAB_S 0.20   /* home_insert_roof.c's ROOM_SLAB and ROOM_HOOD */
#define ROOM_HOOD_S 0.60
#define UNF_CONC    2      /* the unfinished storeys' columns and the stubs: the cast concrete's cool grey (H_CONCRETE) */

/* A box x0..x1, y0..y1, z0..z1 of the given finish: its sides `side`, its top `top`, its foot `bot`. */
static int ubox(Ctx *C, int unit, double x0, double y0, double z0, double x1, double y1, double z1, int side, int top, int bot, int fin, int col) {
    Solid *s = hs_solid_new(C, unit, -1, 0);
    s->kind = SK_INS;
    s->wallFin = (uint8_t)fin; s->wallCol = (uint8_t)col; s->trimCol = (uint8_t)col;
    hs_plane(s, -1, 0, 0, x0, 0, 0, side);
    hs_plane(s, 1, 0, 0, x1, 0, 0, side);
    hs_plane(s, 0, 0, -1, 0, 0, z0, side);
    hs_plane(s, 0, 0, 1, 0, 0, z1, side);
    hs_plane(s, 0, 1, 0, 0, y1, 0, top);
    hs_plane(s, 0, -1, 0, 0, y0, 0, bot);
    for (int i = 0; i < s->np; ++i) s->noWin[i] = 1;
    hs_lift(s, C->dy ? C->dy[unit] : 0.0);
    return (int)(s - C->s);
}

static void add_unfinished(Ctx *C, const HInsert *in) {
    const HGraph *g = C->g;
    const int u = in->unit, far = C->lod == HSH_FAR;
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    const double base = y0 - 0.05, c = 0.5 * UNF_COL, T = UNF_WALL;
    if (!far) {
        float col[48][2];
        const int nc = home_unf_columns(g, in, col, 48);
        for (int k = 0; k < nc; ++k)
            ubox(C, u, col[k][0] - c, base, col[k][1] - c, col[k][0] + c, y1, col[k][1] + c, HF_WALL, HF_COPING, NOFACE, HW_CONCRETE, UNF_CONC);
    }
    HUnfWall W[4];
    const int nw = home_unf_walls(g, in, W);
    for (int i = 0; i < nw; ++i) {
        const HUnfWall *w = &W[i];
        double x0, x1, z0, z1;
        switch (w->wall) {
        case HIW_FRONT: x0 = w->a0; x1 = w->a1; z0 = w->face; z1 = w->face + T; break;
        case HIW_BACK:  x0 = w->a0; x1 = w->a1; z0 = w->face - T; z1 = w->face; break;
        case HIW_WEST:  x0 = w->face; x1 = w->face + T; z0 = w->a0; z1 = w->a1; break;
        default:        x0 = w->face - T; x1 = w->face; z0 = w->a0; z1 = w->a1; break;
        }
        const double wt = y0 + w->h;
        const int s = ubox(C, u, x0, base, z0, x1, wt, z1, HF_WALL, HF_COPING, NOFACE, in->fin, in->col);
        if (far) continue;
        /* the holes its windows will fill, through it; a notch to its top where it stops near their heads */
        float mid[16], wid[16];
        const int nh = home_unf_holes(g, in, w, mid, wid, 16);
        const double m[2] = { w->wall == HIW_WEST ? -1.0 : w->wall == HIW_EAST ? 1.0 : 0.0, w->wall == HIW_FRONT ? -1.0 : w->wall == HIW_BACK ? 1.0 : 0.0 };
        const double e[2] = { -m[1], m[0] };
        const double sill = y0 + 0.87, head = wt < y0 + 2.4 ? wt + 0.1 : y0 + 2.16;   /* (off the 5 cm steps: home_insert_roof.c) */
        for (int k = 0; k < nh; ++k) {
            const double px = home_wall_side(w->wall) ? w->face : mid[k], pz = home_wall_side(w->wall) ? mid[k] : w->face;
            const int o = hs_opening(C, s, u, m, e, px - e[0] * 0.5 * wid[k], pz - e[1] * 0.5 * wid[k], wid[k], sill, head, T + 0.04, NOFACE, 0);
            if (o >= 0) { C->s[o].wallFin = in->fin; C->s[o].wallCol = in->col; }
        }
    }
}

/* A roof room's plan: square, or its front corners rounded by in->r (K chords a quarter); every side a face. */
static void plan_room(const HInsert *in, int K, Plan *P) {
    const double zf = in->z - in->depth;
    P->n = 0;
    edge(P, -1.0, 0.0, -in->x0, 0);
    if (K > 0 && in->r > 0.05f) {
        const double r = in->r, half = 0.5 * PI_D / K;
        for (int q = 0; q < 2; ++q) {
            const double cx = q ? in->x1 - r : in->x0 + r, cz = zf + r, a0 = q ? 1.5 * PI_D : PI_D;
            for (int k = 0; k < K; ++k) {
                const double am = a0 + (2 * k + 1) * half, mx = cos(am), mz = sin(am);
                edge(P, mx, mz, mx * cx + mz * cz + r * cos(half), 0);
            }
            if (!q) edge(P, 0.0, -1.0, -zf, 0);
        }
    } else edge(P, 0.0, -1.0, -zf, 0);
    edge(P, 1.0, 0.0, in->x1, 0);
    edge(P, 0.0, 1.0, in->z, 0);
}

/* A glass room: its foot (a band on the deck; a loft room's up to just over the roof's covering at its front), a roof band,
 * glass between, painted steel posts at its square corners; a room on a deck's door a glazed pair in its front. */
static void add_room_glass(Ctx *C, const HInsert *in, int idx, const Plan *P, double y0, double y1, double cov) {
    const HGraph *g = C->g;
    const int u = in->unit, flat = !in->raised;
    const double zf = in->z - in->depth, foot = flat ? y0 + 0.15 : cov + 0.12, band = y1 - 0.22;
    plan_solid(C, in, P, 0.0, y0, foot, HF_TRIM, HF_TRIM, NOFACE);
    plan_solid(C, in, P, 0.0, band, y1, HF_TRIM, HF_TRIM, HF_TRIM);
    const int s = plan_solid(C, in, P, -BOX_GLASS, foot, band, HF_GLASS, NOFACE, NOFACE);
    C->s[s].aux = shared_aux(idx, HG_CURTAIN);
    const double q = POST;
    for (int k = 0; k < 4; ++k) {
        const int right = k & 1, back = k >> 1;
        if (!back && in->r > 0.05f) continue;   /* a drum's front is its glass */
        const double x0 = right ? in->x1 - q : in->x0, z0 = back ? in->z - q : zf;
        ubox(C, u, x0, y0, z0, x0 + q, band, z0 + q, HF_TRIM, NOFACE, NOFACE, HW_CONCRETE, in->col);
    }
    float dx;
    if (home_room_door(g, in, &dx)) {
        const double m[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 }, h = fmin(2.05, band - foot - 0.05);
        const int o = hs_opening(C, s, u, m, e, dx - 0.5 * INS_DOOR_W, zf + BOX_GLASS, INS_DOOR_W, foot, foot + h, 0.06, HF_GLASS,
                                 hs_opening_aux(INS_DOOR_W, h, HG_CASEMENT));
        opening_finish(C, o, in, HW_CONCRETE, in->col);
        C->st.doors++;
    }
}

static void add_room(Ctx *C, const HInsert *in, int idx) {
    const HGraph *g = C->g;
    const int u = in->unit, far = C->lod == HSH_FAR, flat = !in->raised;
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    const double zf = in->z - in->depth, cov = home_room_sill(g, in);
    int K = 0;
    if (in->r > 0.05f) { K = (int)floor(in->r / 0.3 + 0.5); K = K < 3 ? 3 : K > 6 ? 6 : K; }
    Plan P;
    plan_room(in, far && K ? 2 : K, &P);
    const int glass = in->face == HIF_GLASS, cont = in->face == HIF_CONTAINER, screen = in->face == HIF_SCREEN;
    if (far) {
        if (glass) { const int s = plan_solid(C, in, &P, 0.0, y0, y1, HF_GLASS, HF_TRIM, NOFACE); C->s[s].aux = shared_aux(idx, HG_CURTAIN); }
        else if (cont) { const int s = plan_solid(C, in, &P, 0.0, y0, y1, HF_ROOF, HF_ROOF, NOFACE); C->s[s].roofFin = HRM_METAL; C->s[s].roofCol = in->col; }
        else plan_solid(C, in, &P, 0.0, y0, y1, HF_WALL, HF_FLAT, NOFACE);
        return;
    }
    if (glass) { add_room_glass(C, in, idx, &P, y0, y1, cov); return; }
    const double top = cont ? y1 : y1 - ROOM_SLAB_S;
    const int b = cont ? plan_solid(C, in, &P, 0.0, y0, y1, HF_ROOF, HF_ROOF, NOFACE) : plan_solid(C, in, &P, 0.0, y0, top, HF_WALL, NOFACE, NOFACE);
    const int rFin = cont ? HW_BLOCK : in->fin, rCol = cont ? 1 : in->col;
    const uint32_t h = hash_combine(g->unit[u].seed, 0x7C10u + (uint32_t)idx);
    if (cont) { C->s[b].roofFin = HRM_METAL; C->s[b].roofCol = in->col; C->s[b].wallFin = HW_BLOCK; C->s[b].wallCol = 1; }
    else {
        /* its roof slab, proud of it all round, out over the door of a room on a deck (a loft room's runs back into the roof) */
        const int s = ubox(C, u, in->x0 - 0.1, top, zf - (flat ? ROOM_HOOD_S : 0.12), in->x1 + 0.1, y1, in->z + 0.1, HF_COPING, HF_FLAT, HF_SOFFIT, in->fin, in->col);
        C->s[s].trimCol = (uint8_t)(h & 1u ? 0 : 4);   /* its soffit white or grey */
    }
    const double m[2] = { 0.0, -1.0 }, e[2] = { 1.0, 0.0 };
    float dx;
    if (home_room_door(g, in, &dx)) {
        const int o = hs_opening(C, b, u, m, e, dx - 0.5 * INS_DOOR_W, zf, INS_DOOR_W, y0 + 0.02, y0 + INS_DOOR_H, 0.12, HF_DOOR,
                                 hs_opening_aux(INS_DOOR_W, INS_DOOR_H - 0.02, (int)(h >> 1 & 1u)));
        opening_finish(C, o, in, rFin, rCol);
        C->st.doors++;
    }
    float win[4][4];
    const int nw = home_room_windows(g, in, win, 4);
    for (int k = 0; k < nw; ++k) {
        const double w = win[k][1] - win[k][0], ht = win[k][3] - win[k][2];
        /* a screen room's recess, glass at its back (hs_roof_insert stands the blocks in its mouth) */
        const uint32_t aux = screen ? shared_aux(idx, HG_CURTAIN) | (hs_opening_aux(w, ht, 0) & 0xFFFFFu) : hs_opening_aux(w, ht, w > 1.3 ? HG_CURTAIN : HG_CASEMENT);
        const int o = hs_opening(C, b, u, m, e, win[k][0], zf, w, win[k][2], win[k][3], screen ? fmin(0.4, in->depth - 0.3) : cont ? 0.08 : 0.1, HF_GLASS, aux);
        opening_finish(C, o, in, rFin, rCol);
    }
    /* a concrete room on a deck: a narrow window in a side a time in two */
    if (flat && in->face == HIF_CONCRETE && in->depth >= 2.2f && (h >> 2 & 1u)) {
        const double sw = 0.6, zm = zf + 0.5 * in->depth, sl = y0 + 0.9, hd = top - 0.3;
        const int right = (int)(h >> 3 & 1u);
        const double ms[2] = { right ? 1.0 : -1.0, 0.0 }, es[2] = { 0.0, right ? 1.0 : -1.0 };
        const int o = hs_opening(C, b, u, ms, es, right ? in->x1 : in->x0, right ? zm - 0.5 * sw : zm + 0.5 * sw, sw, sl, hd, 0.1, HF_GLASS, hs_opening_aux(sw, hd - sl, HG_PLAIN));
        opening_finish(C, o, in, rFin, rCol);
    }
}

/* The starter-bar stubs on RCC and self-built roofs (near; hs_stubs sets their bars on them): on the parapets' copings, a
 * millimetre over them so neither cuts the other's faces (the stubs cost their own ten triangles) */
static void add_stubs(Ctx *C) {
    if (C->lod == HSH_FAR) return;
    const HGraph *g = C->g;
    const double c = 0.5 * STUB_W;
    for (int u = 0; u < g->nunit; ++u) {
        float st[24][3], deck, base;
        const int n = home_stubs(g, u, st, 24, &deck, &base);
        for (int k = 0; k < n; ++k)
            ubox(C, u, st[k][0] - c, base + 0.001, st[k][1] - c, st[k][0] + c, deck + st[k][2], st[k][1] + c, HF_WALL, HF_COPING, NOFACE, HW_CONCRETE, UNF_CONC);
    }
}

void hs_add_inserts(Ctx *C) {
    const HGraph *g = C->g;
    add_stubs(C);   /* HM5c2c2 */
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (in->unit < 0 || in->unit >= g->nunit || !home_insert_built(in->kind)) continue;
        if (in->kind == HI_UNFINISHED) { add_unfinished(C, in); continue; }   /* HM5c2c2 */
        if (in->kind == HI_ROOFROOM) { add_room(C, in, i); continue; }
        if (home_insert_membered(in)) {
            add_members(C, in, i);
            if (in->kind == HI_STAIRS && C->lod == HSH_NEAR) add_stair_doors(C, in, i);
            continue;
        }
        float y0, y1;
        home_insert_span(g, in, &y0, &y1);
        if (in->kind == HI_TOWER) { add_tower(C, in, i, y0, y1); continue; }
        if (C->lod == HSH_FAR) { add_far(C, in, i, y0, y1); continue; }
        if (in->kind == HI_ROUND) add_round(C, in, i, y0, y1);
        else if (in->face == HIF_GLASS) add_glass(C, in, i, y0, y1);
        else add_box(C, in, i, y0, y1);
    }
}
