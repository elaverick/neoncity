#include "home_char_internal.h"
#include "pmodel_retrofit.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The shared retrofit kit set down (residential roadmap HM5c1; the kit in
 * pmodel_retrofit.c; the user's concept sheets in
 * docs/reference/international-residential, panel 1 of every sheet).
 *
 * What a megacity's owners bolt onto a house of any age, on every house of a
 * style with a sheet, retrofitted or not (the user, 2026-11-20): tanks and
 * solar on the roof, risers and meters on the walls, a light by the door,
 * plants, and on a flat roof a terrace (railings, a screen, trailing plants,
 * razor wire where people are poorer). Each unit's owner chooses (a row is
 * not built as one here: neighbours differ), by the region's custom, the
 * row's affluence and the unit's upkeep (the profiles below, after the sheets).
 *
 * It runs after the character layer on the same near shell, reading its
 * solids and openings and keeping out of what that layer set down (the
 * occupancy: downpipes, dormers, rooflights, shutters, window boxes) and out
 * of its own pieces as it goes. Every refusal is a logged repair
 * ("repair char-solar: ...", -tank, -riser, -wall, -terrace).
 * ========================================================================= */

#define PI_D 3.14159265358979323846

static int ch(uint32_t h, uint32_t k, float p) { return hash_float(hash_combine(h, k)) < p; }
static float fr(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }
static double clampd(double x, double a, double b) { return x < a ? a : x > b ? b : x; }

int home_retrofit_style(int s) {
    switch (s) {
    case HS_CRAFTSMAN: case HS_SPANISH: case HS_RANCH: case HS_TRACT: case HS_TRIPLEDECKER: case HS_MODERN: return 0;   /* no sheet */
    default: return s >= 0 && s < HS_COUNT;
    }
}

/* ---------------------------------------------------------------------------
 * The profiles: what each region's sheets show on their houses, as chances at middling affluence
 * ------------------------------------------------------------------------- */

typedef struct RProf {
    float solar, canopy, ridge;     /* solar at all; on a flat roof a canopy; on a pitched one a frame over the ridge (else
                                       panels on the slope) */
    float tanks, stand, steel;      /* tanks; on a tall stand; steel, not black plastic */
    float stand0, stand1;           /* a stand's height */
    float risers, copper;           /* pipes up the walls; copper, not steel */
    float meter, bank;              /* a meter box; a bank of two to four */
    float light, lantern, pair;     /* a light by the door; a lantern, not a bulkhead; one each side */
    float led;                      /* LED strips under the eaves or a slab's edge */
    float aerial, cross;            /* an aerial; the old cross one */
    float vine, bloom;              /* climbing plants; their flowering share */
    float pots, palm;               /* pots by the door; a palm or a banana among them */
    float trail, rail, breeze;      /* a flat roof's terrace: plants over its parapet, railings, a breeze-block screen */
    float gate, razor;              /* a security gate over the door; razor wire on the parapet */
} RProf;

static void profile(const HGraph *g, int u, const HCharSet *cs, RProf *P) {
    memset(P, 0, sizeof(*P));
    P->stand0 = 1.5f; P->stand1 = 3.0f; P->copper = 0.5f; P->bloom = 0.3f;
    /* the style's region (a style is its region's custom: a forced style keeps its own, home_lab's and the captures') */
    switch (home_style_region(g->style)) {
    case PLAN_REGION_UK:    /* the Victorian, Georgian, inter-war and council sheets: panels on the slopes, a black tank by the
                               stack, copper risers, a lantern, the old cross aerials, climbing plants up the front */
        P->solar = 0.6f; P->canopy = 0.6f; P->ridge = 0.3f; P->tanks = 0.55f; P->steel = 0.35f;
        P->risers = 0.7f; P->copper = 0.6f; P->meter = 0.9f; P->light = 0.85f; P->lantern = 0.6f; P->pair = 0.15f;
        P->led = 0.3f; P->aerial = 0.65f; P->cross = 0.5f; P->vine = 0.5f; P->bloom = 0.25f; P->pots = 0.7f;
        P->trail = 0.3f; P->rail = 0.4f; P->breeze = 0.1f;
        break;
    case PLAN_REGION_NA:    /* the brownstone sheet: roof terraces, steel tanks, lanterns at the stoop, pots up it */
        P->solar = 0.55f; P->canopy = 0.7f; P->ridge = 0.2f; P->tanks = 0.6f; P->steel = 0.6f;
        P->risers = 0.6f; P->meter = 0.8f; P->light = 0.85f; P->lantern = 0.7f; P->pair = 0.4f;
        P->led = 0.35f; P->aerial = 0.3f; P->cross = 0.3f; P->vine = 0.5f; P->pots = 0.8f;
        P->trail = 0.35f; P->rail = 0.6f; P->breeze = 0.15f; P->gate = 0.05f;
        break;
    case PLAN_REGION_SEU:   /* bougainvillea, terraces, tanks */
        P->solar = 0.6f; P->canopy = 0.6f; P->ridge = 0.2f; P->tanks = 0.6f; P->steel = 0.4f;
        P->risers = 0.6f; P->meter = 0.9f; P->light = 0.8f; P->lantern = 0.6f; P->pair = 0.2f;
        P->led = 0.35f; P->aerial = 0.4f; P->cross = 0.2f; P->vine = 0.6f; P->bloom = 0.45f; P->pots = 0.85f; P->palm = 0.25f;
        P->trail = 0.45f; P->rail = 0.5f; P->breeze = 0.2f; P->gate = 0.1f;
        break;
    case PLAN_REGION_EAS:   /* Japan's meters, bulkheads and potted plants on the step; China's and Korea's tanks */
        P->solar = 0.45f; P->canopy = 0.5f; P->ridge = 0.1f; P->tanks = g->style == HS_JAPAN ? 0.1f : 0.7f; P->steel = 0.6f;
        P->risers = 0.6f; P->copper = 0.3f; P->meter = 1.0f; P->bank = g->style == HS_JAPAN ? 0.3f : 0.2f;
        P->light = 0.85f; P->lantern = 0.15f; P->led = 0.3f; P->aerial = 0.45f; P->cross = 0.1f;
        P->vine = 0.35f; P->pots = 0.8f; P->trail = 0.35f; P->rail = 0.6f; P->breeze = 0.15f; P->gate = 0.15f;
        break;
    case PLAN_REGION_SEA:   /* the shophouse and link house sheets: canopies, tanks, copper risers, plants everywhere */
        P->solar = 0.6f; P->canopy = 0.75f; P->ridge = 0.25f; P->tanks = 0.8f; P->steel = 0.4f;
        P->risers = 0.8f; P->copper = 0.7f; P->meter = 1.0f; P->bank = 0.2f; P->light = 0.75f; P->lantern = 0.3f;
        P->led = 0.45f; P->aerial = 0.3f; P->cross = 0.1f; P->vine = 0.6f; P->bloom = 0.25f; P->pots = 0.9f; P->palm = 0.4f;
        P->trail = 0.6f; P->rail = 0.7f; P->breeze = 0.4f; P->gate = 0.35f;
        break;
    case PLAN_REGION_SAS:   /* the RCC and bungalow sheets: black tanks on every roof, banks of meters, steel risers */
        P->solar = 0.5f; P->canopy = 0.7f; P->ridge = 0.1f; P->tanks = 0.95f; P->stand = 0.35f; P->steel = 0.1f;
        P->stand0 = 0.8f; P->stand1 = 1.8f; P->risers = 0.9f; P->copper = 0.35f; P->meter = 1.0f; P->bank = 0.55f;
        P->light = 0.7f; P->lantern = 0.15f; P->led = 0.4f; P->aerial = 0.55f; P->cross = 0.3f;
        P->vine = 0.5f; P->bloom = 0.5f; P->pots = 0.7f; P->palm = 0.2f;
        P->trail = 0.45f; P->rail = 0.8f; P->breeze = 0.5f; P->gate = 0.55f; P->razor = 0.1f;
        break;
    case PLAN_REGION_MENA:  /* the villa and courtyard sheets: tanks on low stands, canopies, lanterns, bougainvillea */
        P->solar = 0.6f; P->canopy = 0.75f; P->tanks = 0.9f; P->stand = 0.3f; P->steel = 0.2f; P->stand0 = 0.6f; P->stand1 = 1.2f;
        P->risers = 0.6f; P->meter = 0.9f; P->light = 0.85f; P->lantern = 0.75f; P->pair = 0.5f;
        P->led = 0.45f; P->aerial = 0.45f; P->cross = 0.2f; P->vine = 0.5f; P->bloom = 0.6f; P->pots = 0.7f; P->palm = 0.6f;
        P->trail = 0.4f; P->rail = 0.4f; P->breeze = 0.6f; P->gate = 0.2f;
        break;
    case PLAN_REGION_SSA:   /* the block and compound sheets: tanks on tall steel stands, a bare bulkhead, gates, razor wire */
        P->solar = 0.4f; P->canopy = 0.5f; P->ridge = 0.1f; P->tanks = 0.9f; P->stand = 0.6f; P->steel = 0.05f;
        P->stand0 = 2.5f; P->stand1 = 4.5f; P->risers = 0.5f; P->copper = 0.2f; P->meter = 0.9f;
        P->light = 0.8f; P->lantern = 0.05f; P->led = 0.1f; P->aerial = 0.3f; P->cross = 0.2f;
        P->vine = 0.3f; P->pots = 0.6f; P->palm = 0.5f; P->trail = 0.2f; P->rail = 0.4f; P->breeze = 0.6f;
        P->gate = 0.6f; P->razor = 0.4f;
        break;
    case PLAN_REGION_LAT:   /* the self-built and colonial sheets: a black tank and a canopy on every flat roof, railings */
        P->solar = 0.6f; P->canopy = 0.8f; P->ridge = 0.15f; P->tanks = 0.95f; P->stand = 0.25f; P->steel = 0.05f;
        P->stand0 = 1.0f; P->stand1 = 2.0f; P->risers = 0.7f; P->copper = 0.4f; P->meter = 1.0f; P->bank = 0.15f;
        P->light = 0.7f; P->lantern = 0.3f; P->led = 0.4f; P->aerial = 0.4f; P->cross = 0.2f;
        P->vine = 0.5f; P->bloom = 0.35f; P->pots = 0.6f; P->palm = 0.3f;
        P->trail = 0.5f; P->rail = 0.8f; P->breeze = 0.5f; P->gate = 0.5f; P->razor = 0.2f;
        break;
    default: break;
    }
    /* style: a Georgian or a brownstone front pairs its lanterns; a council house's is a bulkhead */
    if (g->style == HS_GEORGIAN || g->style == HS_ROWHOUSE) { P->lantern = 0.85f; P->pair = 0.45f; }
    if (g->style == HS_POSTWAR) { P->lantern = 0.15f; P->vine *= 0.6f; }
    /* affluence (0 poor .. 1 rich): solar, canopies, LEDs, lanterns, steel tanks and planting rise with it; gates, razor
     * wire, cheap tanks on stands and the old aerials with poverty; upkeep thins the plants and the lights */
    const float a = (float)clampd(g->affluence, 0.0, 1.0);
    P->solar = fminf(0.95f, P->solar * (0.6f + 0.7f * a));
    P->canopy = fminf(0.95f, P->canopy * (0.7f + 0.5f * a));
    P->led = fminf(0.9f, P->led * (0.4f + 1.2f * a));
    P->lantern = fminf(0.95f, P->lantern * (0.7f + 0.6f * a));
    P->steel = fminf(0.9f, P->steel * (0.6f + 0.9f * a));
    P->gate = fminf(0.9f, P->gate * (1.5f - 1.0f * a));
    P->razor = fminf(0.8f, P->razor * (1.8f - 1.6f * a));
    P->stand = fminf(0.9f, P->stand * (1.3f - 0.6f * a));
    P->cross = fminf(0.9f, P->cross * (1.4f - 0.8f * a));
    const float green = 0.75f + 0.45f * a;
    P->vine *= green; P->pots *= green; P->trail *= green;
    if (cs->upkeep == HK_TIRED) { P->vine *= 0.85f; P->pots *= 0.8f; P->led *= 0.6f; }
    if (cs->upkeep == HK_RUNDOWN) { P->vine *= 0.6f; P->pots *= 0.5f; P->trail *= 0.5f; P->led *= 0.2f; P->lantern *= 0.4f;
                                    P->gate = fminf(0.9f, P->gate + 0.15f); P->razor = fminf(0.8f, P->razor * 1.5f); }
    (void)u;
}

/* ---------------------------------------------------------------------------
 * Walls
 * ------------------------------------------------------------------------- */

typedef struct Wall {
    double m[2], e[2];   /* outward normal, along it (e = (-m.z, m.x)) */
    double at;           /* m.(x, z) of its face */
    double s0, s1;       /* its run along e */
    double y0, y1;       /* the ground, its top (under the soffit, a slab, or a parapet's coping) */
    int    front;
} Wall;

static void wall_pt(const Wall *w, double s, double y, double out, double p[3]) {
    p[0] = w->m[0] * (w->at + out) + w->e[0] * s;
    p[1] = y;
    p[2] = w->m[1] * (w->at + out) + w->e[1] * s;
}

/* Whether p lies in an opening (a window's, a door's, a recessed porch's): carved out of the wall it is in. */
static int in_opening(const Ctx *C, const double p[3]) {
    for (int i = 0; i < C->ns; ++i) {
        const Solid *s = &C->s[i];
        if (s->neg && hs_in_solid(s, p, 0.0)) return 1;
    }
    return 0;
}

/* Whether a box on a wall (s0..s1 along it, y0..y1, out to `out`) is clear: on the wall's own run, no window or door's
 * surround within margin, no solid standing in it (a bay, a porch, a wing, a step), the wall behind it all the way, and
 * nothing the kit has set down there. */
static int wall_clear(const DC *d, const Wall *w, double s0, double s1, double y0, double y1, double out, double margin) {
    if (s0 < w->s0 + 0.05 || s1 > w->s1 - 0.05 || y0 < w->y0 - 0.01 || y1 > w->y1 + 0.01) return 0;
    const double ds = fmax(0.05, fmin(0.2, 0.5 * (s1 - s0)));
    for (double s = s0; s <= s1 + 1e-6; s += ds)
        if (hc_window_near(d, w->m, w->at, s, y0, y1, margin)) return 0;
    const int ns = (int)fmax(2.0, ceil((s1 - s0) / 0.25) + 1.0), ny = (int)fmax(2.0, ceil((y1 - y0) / 0.3) + 1.0);
    double lo[3] = { 1e30, y0, 1e30 }, hi[3] = { -1e30, y1, -1e30 };
    for (int i = 0; i < ns; ++i) {
        const double s = s0 + (s1 - s0) * i / (ns - 1);
        for (int j = 0; j < ny; ++j) {
            const double y = y0 + (y1 - y0) * j / (ny - 1);
            double p[3];
            wall_pt(w, s, y, 0.5 * out, p);
            if (hs_inside(d->C, p, -1, 0.003) >= 0) return 0;
            wall_pt(w, s, y, out, p);
            if (hs_inside(d->C, p, -1, 0.003) >= 0) return 0;
            wall_pt(w, s, y, -0.06, p);
            if (hs_inside(d->C, p, -1, 0.0) < 0 || in_opening(d->C, p)) return 0;
        }
        for (int b = 0; b < 2; ++b) {
            double p[3];
            wall_pt(w, s, y0, b ? out : -0.02, p);
            lo[0] = fmin(lo[0], p[0]); hi[0] = fmax(hi[0], p[0]); lo[2] = fmin(lo[2], p[2]); hi[2] = fmax(hi[2], p[2]);
        }
    }
    return hc_free(d, lo, hi);
}

static void wall_occupy(DC *d, const Wall *w, double s0, double s1, double y0, double y1, double out) {
    double lo[3] = { 1e30, y0, 1e30 }, hi[3] = { -1e30, y1, -1e30 };
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            double p[3];
            wall_pt(w, a ? s1 : s0, y0, b ? out : -0.02, p);
            lo[0] = fmin(lo[0], p[0]); hi[0] = fmax(hi[0], p[0]); lo[2] = fmin(lo[2], p[2]); hi[2] = fmax(hi[2], p[2]);
        }
    hc_occupy(d, lo, hi);
}

/* a frame on the wall at (s, y), facing out */
static void push_on(DC *d, const Wall *w, double s, double y, double out) {
    double p[3];
    wall_pt(w, s, y, out, p);
    hc_push_wall(d, p, w->m[0], w->m[1]);
}

/* The walls of a unit's main (its slice of a row's): not a party wall's cut, not where a roof runs on into its parent. */
static int unit_walls(const DC *d, const HsRoof *hr, Wall *out, int max) {
    const Roof *R = &hr->R;
    HVol own;
    const HVol *v = home_unit_vol(d->g, hr->vol, hr->unit, &own);   /* a unit at its own storeys (HM5e1) */
    int n = 0;
    double top;
    if (R->kind == HR_FLAT) top = (v->parapet == 2 ? v->top : R->E) + hr->dy;
    else top = R->E - SLAB_T + hr->dy;
    for (int e = 0; e < R->W.n && n < max; ++e) {
        if (R->cut[e] || R->role[e] == ER_HIGH || R->role[e] == ER_EXT) continue;
        Wall *w = &out[n];
        w->m[0] = R->W.m[e][0]; w->m[1] = R->W.m[e][1];
        w->e[0] = -w->m[1]; w->e[1] = w->m[0];
        const double *a = R->W.p[e], *b = R->W.p[(e + 1) % R->W.n];
        w->at = w->m[0] * a[0] + w->m[1] * a[1];
        const double sa = w->e[0] * a[0] + w->e[1] * a[1], sb = w->e[0] * b[0] + w->e[1] * b[1];
        w->s0 = fmin(sa, sb); w->s1 = fmax(sa, sb);
        w->y0 = hr->dy;
        /* a pitched roof's verge walls run up into their gables: the eaves' height still bounds what goes on them */
        w->y1 = top;
        w->front = w->m[1] < -0.7;
        if (w->s1 - w->s0 > 0.8) n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The context of one unit
 * ------------------------------------------------------------------------- */

typedef struct RU {
    DC          *d;
    int          u;
    uint32_t     o;          /* the owner's seed */
    RProf        P;
    const HCharSet *cs;
    const HsRoof *hr;        /* its main's roof (its slice) */
    const HVol   *v;         /* its main as its slice stands (its own storeys: home_unit_vol) */
    HVol          vOwn;
    Wall         w[6];
    int          nw;
    const HsDoor *dr;        /* its front door */
    int          lean;
    int          lightSide;  /* the side of the door its light took (-1, 1; 0 none or both) */
} RU;

static const Wall *front_wall(const RU *r) {
    for (int i = 0; i < r->nw; ++i) if (r->w[i].front) return &r->w[i];
    return NULL;
}
/* the wall the front door is in (its face at -dr->z): the main's front, or an enclosed porch's */
static int door_wall(const RU *r, Wall *w) {
    if (!r->dr) return 0;
    /* at the back of a five-foot way or under pilotis (HM5e1): the wall the door stands in, the width of the space */
    for (int i = 0; i < r->d->g->nvol; ++i) {
        const HVol *p = &r->d->g->vol[i];
        if (p->role != HV_PORCH || p->unit != r->u || (p->kind != HP_ARCADE && p->kind != HP_PILOTIS)) continue;
        memset(w, 0, sizeof(*w));
        w->m[0] = 0.0; w->m[1] = -1.0; w->e[0] = 1.0; w->e[1] = 0.0;
        w->at = -r->dr->z;
        w->s0 = p->x0 + (p->kind == HP_PILOTIS ? PILOTIS_COL : 0.0); w->s1 = p->x1 - (p->kind == HP_PILOTIS ? PILOTIS_COL : 0.0);
        w->y0 = r->hr->dy; w->y1 = p->eaves + r->hr->dy;
        w->front = 1;
        return 1;
    }
    const Wall *f = front_wall(r);
    /* the door in the main's front, or set back in a recess in it (its lights and meter then beside the recess) */
    if (f && (fabs(f->at + r->dr->z) < 0.05 || (-r->dr->z < f->at && f->at + r->dr->z < 2.0 &&
                                                 r->dr->x > f->s0 && r->dr->x < f->s1))) { *w = *f; return 1; }
    /* the door is in another face (a porch's front, a wing's): a wall 3 m either side of it */
    memset(w, 0, sizeof(*w));
    w->m[0] = 0.0; w->m[1] = -1.0; w->e[0] = 1.0; w->e[1] = 0.0;
    w->at = -r->dr->z;
    w->s0 = r->dr->x - 3.0; w->s1 = r->dr->x + 3.0;
    w->y0 = r->hr->dy; w->y1 = r->dr->y1 + 1.0;
    w->front = 1;
    return 1;
}

/* How far either side of the door's middle its surround, canopy or recess reaches along the front. */
static double door_half(const RU *r) {
    const DC *d = r->d;
    double half = fmax(d->doorHalf[r->u], 0.5 * r->dr->w + 0.05);
    for (int i = 0; i < d->g->nvol; ++i) {
        const HVol *p = &d->g->vol[i];
        if (p->role == HV_PORCH && p->unit == r->u && p->x0 - 0.05 < r->dr->x && p->x1 + 0.05 > r->dr->x && p->kind != HP_ARCADE && p->kind != HP_PILOTIS)
            half = fmax(half, fmax(r->dr->x - p->x0, p->x1 - r->dr->x) + 0.05);
    }
    return half;
}

static int placed_since(const DC *d, uint32_t n0) { return d->pm->inst ? (int)(d->pm->inst->n - n0) : 0; }
static uint32_t inst_n(const DC *d) { return d->pm->inst ? d->pm->inst->n : 0; }

/* ---------------------------------------------------------------------------
 * The door: a light, a meter, a gate, pots
 * ------------------------------------------------------------------------- */

static void door_light(RU *r) {
    DC *d = r->d;
    Wall w;
    if (!ch(r->o, 0x11u, r->P.light) || !door_wall(r, &w)) return;
    const HsDoor *dr = r->dr;
    const int lantern = ch(r->o, 0x12u, r->P.lantern);
    const int pair = lantern && ch(r->o, 0x13u, r->P.pair);
    const double half = door_half(r);
    int done = 0;
    /* a bulkhead over the door where nothing stands there (a fanlight, a canopy) */
    if (!lantern && d->doorTop[r->u] < dr->y1 + 0.05 && ch(r->o, 0x14u, 0.5f)) {
        const double y = dr->y1 + 0.16;
        if (wall_clear(d, &w, dr->x - 0.13, dr->x + 0.13, y - 0.08, y + 0.08, 0.12, 0.02)) {
            hc_begin(d, r->u);
            push_on(d, &w, dr->x, y, 0.0);
            pm_place(d->pm, PMK_R_BULKHEAD);
            hc_pop(d);
            hc_done(d);
            wall_occupy(d, &w, dr->x - 0.13, dr->x + 0.13, y - 0.08, y + 0.08, 0.12);
            return;
        }
    }
    const int first = ch(r->o, 0x15u, 0.5f) ? 1 : -1;
    for (int k = 0; k < 2 && (!done || pair); ++k) {
        const int side = k ? -first : first;
        const double off = half + 0.3;   /* hc_window_near keeps 0.1 m past a door's surround, and the light's own half */
        const double s = dr->x * w.e[0] + side * off;   /* front walls: s is x */
        const double y = lantern ? fmin(dr->y1 - 0.05, dr->y0 + 1.9) : dr->y1 - 0.2;
        const double y0 = y - (lantern ? 0.26 : 0.1), y1 = y + (lantern ? 0.22 : 0.1);
        const double out = lantern ? 0.34 : 0.12;
        if (!wall_clear(d, &w, s - 0.13, s + 0.13, y0, y1, out, 0.04)) continue;
        hc_begin(d, r->u);
        push_on(d, &w, s, y, 0.0);
        pm_place(d->pm, lantern ? PMK_R_LANTERN : PMK_R_BULKHEAD);
        hc_pop(d);
        hc_done(d);
        wall_occupy(d, &w, s - 0.13, s + 0.13, y0, y1, out);
        if (!done) r->lightSide = side;
        else r->lightSide = 0;
        done++;
    }
    if (!done) hc_repair(d, HDR_WALLKIT, "no room for a light by its door (%.0f)", r->u, (double)lantern);
}

static int meter_on(RU *r, const Wall *w, double s, double y, int n, int conduit) {
    DC *d = r->d;
    const double pitch = HR_METER_W + 0.06, half = 0.5 * pitch * n;
    if (!wall_clear(d, w, s - half, s + half, y - 0.22, y + HR_METER_H + 0.3, 0.2, 0.06)) return 0;
    hc_begin(d, r->u);
    for (int k = 0; k < n; ++k) {
        push_on(d, w, s - half + pitch * (k + 0.5), y, 0.0);
        pm_place(d->pm, PMK_R_METER);
        hc_pop(d);
    }
    /* the conduit up to the eaves, clear of the windows over it */
    const double cy = y + HR_METER_H + 0.25, top = w->y1 - 0.12;
    if (conduit && top - cy > 0.4 && wall_clear(d, w, s - 0.05, s + 0.05, cy, top, 0.08, 0.03)) {
        double p[3];
        wall_pt(w, s, cy, 0.05, p);
        hc_push_wall(d, p, w->m[0], w->m[1]);
        pm_scale(d->pm, 0.35f, (float)(top - cy), 0.35f);
        pm_place(d->pm, PMK_R_PIPE_FE);
        hc_pop(d);
        wall_occupy(d, w, s - 0.05, s + 0.05, cy, top, 0.08);
    }
    hc_done(d);
    wall_occupy(d, w, s - half, s + half, y - 0.22, y + HR_METER_H + 0.3, 0.2);
    return 1;
}

static void door_meter(RU *r) {
    DC *d = r->d;
    if (!ch(r->o, 0x21u, r->P.meter)) return;
    const int n = ch(r->o, 0x22u, r->P.bank) ? 2 + (int)(fr(r->o, 0x23u) * 3.0f) : 1;
    const int conduit = ch(r->o, 0x24u, 0.6f);
    Wall w;
    /* beside the front door, the side away from its light; or along the front; or on a side wall near the front */
    if (door_wall(r, &w)) {
        const HsDoor *dr = r->dr;
        const double half = door_half(r);
        const double ys[2] = { dr->y0 + 1.15, r->hr->dy + 0.5 };
        for (int t = 0; t < 2; ++t)
            for (int k = 0; k < 2; ++k) {
                const int side = k ? r->lightSide : -r->lightSide;
                const double off = half + 0.3 + 0.5 * (HR_METER_W + 0.06) * n + (side == r->lightSide && r->lightSide ? 0.4 : 0.0);
                const double s = dr->x + (side ? side : (k ? 1 : -1)) * off;
                if (meter_on(r, &w, s, ys[t], n, conduit)) return;
            }
    }
    for (int i = 0; i < r->nw; ++i) {
        const Wall *x = &r->w[i];
        for (int k = 0; k < 6; ++k) {
            const double s = x->s0 + 0.5 + (x->s1 - x->s0 - 1.0) * k / 5.0;
            if (meter_on(r, x, s, x->y0 + 1.1, n, conduit)) return;
        }
    }
    hc_repair(d, HDR_WALLKIT, "no room for its meter (%.0f)", r->u, (double)n);
}

static void door_gate(RU *r) {
    DC *d = r->d;
    const HsDoor *dr = r->dr;
    if (!dr || !ch(r->o, 0x31u, r->P.gate)) return;
    /* not over a doorcase's pilasters (it would stand on them) */
    if (d->doorHalf[r->u] > 0.5 * dr->w + 0.2 && d->doorTop[r->u] > dr->y1 + 0.6) { hc_repair(d, HDR_WALLKIT, "a doorcase: no gate over its door (%.0f)", r->u, 0.0); return; }
    const double w = dr->w + 0.14, h = dr->y1 - dr->y0 + 0.06;
    hc_begin(d, r->u);
    const double p[3] = { dr->x, dr->y0, dr->z - 0.004 };
    hc_push_wall(d, p, 0.0, -1.0);
    pm_scale(d->pm, (float)(w / HR_GATE_W), (float)(h / HR_GATE_H), 1.0f);
    pm_place(d->pm, PMK_R_GATE_BARS);
    hc_pop(d);
    hc_done(d);
    const double lo[3] = { dr->x - 0.5 * w, dr->y0, dr->z - 0.06 }, hi[3] = { dr->x + 0.5 * w, dr->y0 + h, dr->z + 0.02 };
    hc_occupy(d, lo, hi);
}

static void door_pots(RU *r) {
    DC *d = r->d;
    Wall w;
    if (!ch(r->o, 0x41u, r->P.pots) || !door_wall(r, &w)) return;
    const HsDoor *dr = r->dr;
    const int want = 1 + (int)(fr(r->o, 0x42u) * (r->lean ? 1.5f : 3.99f));
    const double half = door_half(r);
    int n = 0;
    for (int k = 0; k < 8 && n < want; ++k) {
        const int side = k & 1 ? 1 : -1;
        const double s = dr->x + side * (half + 0.45 + 0.5 * (k >> 1));
        const int palm = ch(r->o, 0x43u + (uint32_t)k, r->P.palm);
        const double rad = palm ? 0.3 : 0.2, out = 0.12 + rad;
        const double y = r->hr->dy;
        /* on the ground in front of the wall: clear of the steps and the path up to the door */
        if (fabs(s - dr->x) < 0.75) continue;
        double c[3];
        wall_pt(&w, s, y, out, c);
        int ok = 1;
        for (int j = 0; j < 5 && ok; ++j) {
            const double q[3] = { c[0] + (j == 1 ? rad : j == 2 ? -rad : 0), y + (j == 0 ? 0.15 : 0.3), c[2] + (j == 3 ? rad : j == 4 ? -rad : 0) };
            if (hs_inside(d->C, q, -1, 0.0) >= 0) ok = 0;
        }
        /* the ground under it: nothing below its foot but the garden (a pot never floats off a stoop) */
        const double under[3] = { c[0], y - 0.05, c[2] };
        if (hs_inside(d->C, under, -1, 0.0) >= 0) ok = 0;
        const double lo[3] = { c[0] - rad, y, c[2] - rad }, hi[3] = { c[0] + rad, y + 1.0, c[2] + rad };
        if (!ok || !hc_free(d, lo, hi)) continue;
        /* on the plot: in front of the wall by no more than the yard */
        hc_begin(d, r->u);
        const double p[3] = { c[0], y, c[2] };
        hc_push_wall(d, p, w.m[0], w.m[1]);
        pm_rotate_y(d->pm, (float)(fr(r->o, 0x48u + (uint32_t)k) * 6.2831853));
        if (!palm) { const float sc = 0.85f + 0.4f * fr(r->o, 0x50u + (uint32_t)k); pm_scale(d->pm, sc, sc, sc); }
        pm_place(d->pm, palm ? PMK_R_POT_PALM : PMK_R_POT);
        hc_pop(d);
        hc_done(d);
        hc_occupy(d, lo, hi);
        n++;
    }
}

/* ---------------------------------------------------------------------------
 * Walls: risers, vines, LED strips
 * ------------------------------------------------------------------------- */

static void risers(RU *r) {
    DC *d = r->d;
    if (!ch(r->o, 0x51u, r->P.risers)) return;
    const int fe = !ch(r->o, 0x52u, r->P.copper);
    const int n = 1 + (int)(fr(r->o, 0x53u) * (fe ? 2.2f : 3.0f));
    const double gap = fe ? 0.12 : 0.085, half = 0.5 * gap * (n - 1) + 0.08;
    const int flat = r->hr->R.kind == HR_FLAT, parapet = flat && r->v->parapet == 2;
    /* over a parapet the pipes come down onto the deck (its height under the coping) */
    const double drop = parapet ? r->v->top - r->hr->R.E + 0.18 - 0.08 : 0.0;
    /* the walls in turn from one of the owner's choosing: the sheets' pipes run up fronts as often as sides */
    int start = (int)(fr(r->o, 0x54u) * r->nw);
    if (ch(r->o, 0x56u, 0.6f)) for (int i = 0; i < r->nw; ++i) if (r->w[i].front) start = i;   /* the street's wall, mostly */
    for (int t = 0; t < r->nw; ++t) {
        const Wall *w = &r->w[(start + t) % r->nw];
        const double top = parapet ? w->y1 + 0.18 : w->y1 - 0.12;
        /* places to try: near each corner, then a few along */
        double cand[8];
        int nc = 0;
        cand[nc++] = w->s0 + 0.25 + half; cand[nc++] = w->s1 - 0.25 - half;
        for (int k = 1; k <= 4; ++k) cand[nc++] = w->s0 + (w->s1 - w->s0) * k / 5.0;
        for (int c = 0; c < nc; ++c) {
            const double s = cand[c];
            if (!wall_clear(d, w, s - half, s + half, w->y0 + 0.05, fmin(top, w->y1) - 0.02, HR_PIPE_Z + 0.08, 0.1)) continue;
            /* over the parapet: its coping and the deck behind it clear (no railing, tank or canopy there yet) */
            if (parapet) {
                double a[3], b[3], lo[3], hi[3];
                wall_pt(w, s - half, w->y1 - drop + 0.1, -0.7, a);
                wall_pt(w, s + half, w->y1 + 0.35, 0.2, b);
                for (int c2 = 0; c2 < 3; ++c2) { lo[c2] = fmin(a[c2], b[c2]); hi[c2] = fmax(a[c2], b[c2]); }
                if (!hc_free(d, lo, hi)) continue;
                double q[3];
                wall_pt(w, s, w->y1 - drop + 0.3, -0.5, q);
                if (hs_inside(d->C, q, -1, 0.0) >= 0) continue;
            }
            /* the pipes: up the wall from the ground; over a parapet and down onto the deck, or into the wall under the eaves */
            hc_begin(d, r->u);
            push_on(d, w, s, w->y0, 0.0);
            for (int k = 0; k < n; ++k) {
                const float x = (float)(gap * (k - 0.5 * (n - 1)));
                const float z = (float)HR_PIPE_Z + (float)(fe ? 0.0 : 0.0);
                const float H = (float)(top - w->y0) + 0.06f * (float)k * (parapet ? 1.0f : 0.0f);
                float P[4][3];
                int np = 0;
                P[np][0] = x; P[np][1] = 0.0f; P[np][2] = z; np++;
                P[np][0] = x; P[np][1] = H; P[np][2] = z; np++;
                if (parapet) {
                    P[np][0] = x; P[np][1] = H; P[np][2] = z - 0.55f; np++;
                    P[np][0] = x; P[np][1] = H - (float)drop; P[np][2] = z - 0.55f; np++;
                } else {
                    P[np][0] = x; P[np][1] = H; P[np][2] = -0.08f; np++;   /* into the wall under the soffit */
                }
                hr_pipe_path(d->pm, fe, (const float (*)[3])P, np);
                const float a[3] = { x, 0.0f, z }, b[3] = { x, H - 0.2f, z };
                if (k == 0) hr_pipe_clips(d->pm, fe, a, b);
                if (fe && k == 0 && ch(r->o, 0x55u, 0.5f)) {
                    pm_push(d->pm); pm_translate(d->pm, x, 1.1f, z); pm_place(d->pm, PMK_R_VALVE); pm_pop(d->pm);
                }
            }
            hc_pop(d);
            hc_done(d);
            wall_occupy(d, w, s - half, s + half, w->y0, top + 0.1, HR_PIPE_Z + 0.08);
            if (parapet) {
                double a[3], b[3], lo[3], hi[3];
                wall_pt(w, s - half, w->y1 - drop, -0.7, a);
                wall_pt(w, s + half, w->y1 + 0.35, 0.2, b);
                for (int c2 = 0; c2 < 3; ++c2) { lo[c2] = fmin(a[c2], b[c2]); hi[c2] = fmax(a[c2], b[c2]); }
                hc_occupy(d, lo, hi);
            }
            return;
        }
    }
    hc_repair(d, HDR_RISER, "no clear run up any wall for %.0f risers", r->u, (double)n);
}

static void vines(RU *r) {
    DC *d = r->d;
    if (!ch(r->o, 0x61u, r->P.vine)) return;
    const int want = 1 + (ch(r->o, 0x62u, 0.35f) && !r->lean);
    int got = 0;
    const int start = (int)(fr(r->o, 0x63u) * r->nw);
    for (int t = 0; t < r->nw * 2 && got < want; ++t) {
        const Wall *w = &r->w[(start + t) % r->nw];
        if (!w->front && t < r->nw && got == 0 && ch(r->o, 0x64u + (uint32_t)t, 0.6f)) continue;   /* the street's side first */
        const double width = 0.9 + 0.8 * fr(r->o, 0x70u + (uint32_t)t), top = w->y1 - 0.25;
        for (int k = 0; k < 7; ++k) {
            const double s = k == 0 ? w->s0 + 0.1 + 0.5 * width : k == 1 ? w->s1 - 0.1 - 0.5 * width : w->s0 + (w->s1 - w->s0) * (k - 1) / 6.0;
            /* as tall as it can climb: to the eaves, or to under the first window in its way */
            double h = top - w->y0;
            int ok = 0;
            while (h >= 1.4) {
                if (wall_clear(d, w, s - 0.5 * width, s + 0.5 * width, w->y0, w->y0 + h, 0.22, 0.05)) { ok = 1; break; }
                h -= 0.5;
            }
            if (!ok) continue;
            const int foot = h > 1.6 && ch(r->o, 0x80u + (uint32_t)k, 0.5f);
            hc_begin(d, r->u);
            push_on(d, w, s, w->y0, 0.0);
            hr_vine(d->pm, (float)width, (float)h, r->P.bloom, hash_combine(r->o, 0x90u + (uint32_t)k));
            hc_pop(d);
            /* a trough at its foot, if the ground in front is clear */
            if (foot) {
                double c[3];
                wall_pt(w, s, w->y0 + 0.2, 0.25, c);
                const double c2[3] = { c[0], w->y0 - 0.05, c[2] };
                if (hs_inside(d->C, c, -1, 0.0) < 0 && hs_inside(d->C, c2, -1, 0.0) < 0) {
                    const double L = fmin(width, 1.4);
                    push_on(d, w, s + 0.5 * L * (-d->sx), w->y0, 0.2);
                    pm_scale(d->pm, (float)L, 1.0f, 1.0f);
                    pm_place(d->pm, PMK_R_TROUGH);
                    hc_pop(d);
                }
            }
            hc_done(d);
            wall_occupy(d, w, s - 0.5 * width, s + 0.5 * width, w->y0, w->y0 + h, 0.4);
            got++;
            break;
        }
    }
    if (!got) hc_repair(d, HDR_WALLKIT, "no wall clear for a climbing plant (%.0f)", r->u, 0.0);
}

/* LED strips under the street's eaves (a pitched roof's soffit, a flat slab's edge). */
static void eaves_led(RU *r) {
    DC *d = r->d;
    if (r->lean || !ch(r->o, 0xA1u, r->P.led)) return;
    const Roof *R = &r->hr->R;
    if (R->kind == HR_FLAT && r->v->parapet == 2) return;
    if (R->ov < 0.18) return;
    for (int e = 0; e < R->F.n; ++e) {
        if (R->F.m[e][1] > -0.7) continue;
        if (R->kind != HR_FLAT && R->role[e] != ER_EAVES) continue;
        double dd[2], len, t0, t1;
        hc_eaves_line(R, e, dd, &len, &t0, &t1);
        /* inside the walls only, clear of what runs in under the eaves (a bay's roof, a porch's) */
        const double a = fmax(t0, 0.0) + 0.15, b = fmin(t1, len) - 0.15;
        if (b - a < 0.8) return;
        const double *m = R->F.m[e];
        const double out = 0.5 * R->ov;
        const double y = (R->kind == HR_FLAT ? R->E : R->E - SLAB_T - R->t * out) + r->hr->dy;
        const double p0[3] = { R->F.p[e][0] + m[0] * out, 0.0, R->F.p[e][1] + m[1] * out };
        const double dir[3] = { dd[0], 0.0, dd[1] }, off[3] = { 0.0, y - 0.06, 0.0 };
        Ival run[16];
        const int nr = hc_clear_runs(d->C, p0, dir, off, a, b, 0.1, 0.6, run, 16);
        hc_begin(d, r->u);
        for (int k = 0; k < nr; ++k) {
            /* the kit's x runs along -sx e: from whichever end that starts at */
            const double e2[2] = { -m[1], m[0] };
            const double sDir = e2[0] * dd[0] + e2[1] * dd[1];
            const double ta = (-d->sx > 0.0) == (sDir > 0.0) ? run[k].a : run[k].b;
            const double q[3] = { p0[0] + dd[0] * ta, y - 0.004, p0[2] + dd[1] * ta };
            hc_push_wall(d, q, m[0], m[1]);
            hr_led(d->pm, (float)(run[k].b - run[k].a));
            hc_pop(d);
        }
        hc_done(d);
        return;
    }
}

/* A low breeze-block wall along a veranda's or an open porch's front, a gap at its steps (the block sheets). */
static void porch_breeze(RU *r) {
    DC *d = r->d;
    if (r->lean || !ch(r->o, 0xB1u, r->P.breeze)) return;
    const HGraph *g = d->g;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *p = &g->vol[i];
        if (p->role != HV_PORCH || p->unit != r->u || (p->kind != HP_VERANDA && p->kind != HP_OPEN)) continue;
        const double dy = d->C->dy ? d->C->dy[r->u] : 0.0;
        /* the porch's floor: the first solid down from its middle */
        const double mx = 0.5 * (p->x0 + p->x1), z = p->z0 + 0.1;
        double floorY = dy;
        for (double y = dy + 1.6; y > dy; y -= 0.05) {
            const double q[3] = { mx, y, p->z0 + 0.3 };
            if (hs_inside(d->C, q, -1, 0.0) >= 0) { floorY = y + 0.05; break; }
        }
        const double gapX = r->dr ? r->dr->x : mx, gap = 0.65;
        const double segs[2][2] = { { p->x0 + 0.1, gapX - gap }, { gapX + gap, p->x1 - 0.1 } };
        hc_begin(d, r->u);
        for (int k = 0; k < 2; ++k) {
            const double x0 = segs[k][0], x1 = segs[k][1];
            if (x1 - x0 < 0.6) continue;
            int ok = 1;
            for (double x = x0 + 0.1; x < x1 && ok; x += 0.3) {
                const double q[3] = { x, floorY + 0.5, z }, q2[3] = { x, floorY - 0.05, z };
                if (hs_inside(d->C, q, -1, 0.0) >= 0 || hs_inside(d->C, q2, -1, 0.0) < 0) ok = 0;
            }
            const double lo[3] = { x0, floorY, z - 0.06 }, hi[3] = { x1, floorY + 0.95, z + 0.06 };
            if (!ok || !hc_free(d, lo, hi)) continue;
            /* the panels' x along the mesh's: from x0 in the row's frame, mirrored rows from x1 */
            const double sx0 = d->sx > 0.0 ? x0 : x1;
            const double q[3] = { sx0, floorY, z };
            hc_push_wall(d, q, 0.0, 1.0);   /* facing in: its x then runs along +x in the mesh */
            hr_breeze(d->pm, (int)(r->o & 1u), (float)(x1 - x0), 0.95f);
            hc_pop(d);
            hc_occupy(d, lo, hi);
        }
        hc_done(d);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * Roofs
 * ------------------------------------------------------------------------- */

/* Whether a point lies just under a covering of the roof of volume vol (on one of its own slabs). */
static int on_slab(const DC *d, int vol, const double p[3]) {
    for (int si = 0; si < d->C->ns; ++si) {
        const Solid *so = &d->C->s[si];
        if (so->live && !so->neg && so->kind == SK_SLAB && so->vol == vol && hs_in_solid(so, p, 0.0)) return 1;
    }
    return 0;
}
static int clear_at(const DC *d, double x, double y, double z) {
    const double p[3] = { x, y, z };
    return hs_inside(d->C, p, -1, 0.0) < 0;
}

/* A flat roof's deck: its rectangle (inside its parapets), height, and the parapets' coping. */
typedef struct Deck { double x0, z0, x1, z1, y, top; int parapet; } Deck;
static int deck_of(const HsRoof *hr, const HVol *v, Deck *k) {
    const Roof *R = &hr->R;
    if (R->kind != HR_FLAT || R->W.n != 4) return 0;
    double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
    for (int e = 0; e < 4; ++e) {
        x0 = fmin(x0, R->W.p[e][0]); x1 = fmax(x1, R->W.p[e][0]);
        z0 = fmin(z0, R->W.p[e][1]); z1 = fmax(z1, R->W.p[e][1]);
    }
    k->parapet = v->parapet == 2;
    k->y = (k->parapet ? R->E : v->top) + hr->dy;
    k->top = v->top + hr->dy;
    const double in = k->parapet ? 0.35 : 0.2;
    k->x0 = x0 + in; k->x1 = x1 - in; k->z0 = z0 + in; k->z1 = z1 - in;
    return k->x1 - k->x0 > 1.6 && k->z1 - k->z0 > 1.6;
}
/* Whether a box over a deck is clear: inside the deck, nothing standing in it (a stack, an upstand), nothing set there. */
static int deck_clear(const DC *d, const Deck *k, double x0, double z0, double x1, double z1, double h) {
    if (x0 < k->x0 - 1e-6 || x1 > k->x1 + 1e-6 || z0 < k->z0 - 1e-6 || z1 > k->z1 + 1e-6) return 0;
    for (int i = 0; i <= 3; ++i)
        for (int j = 0; j <= 3; ++j)
            for (int l = 0; l < 3; ++l)
                if (!clear_at(d, x0 + (x1 - x0) * i / 3.0, k->y + 0.08 + (h - 0.1) * l / 2.0, z0 + (z1 - z0) * j / 3.0)) return 0;
    const double lo[3] = { x0, k->y, z0 }, hi[3] = { x1, k->y + h, z1 };
    return hc_free(d, lo, hi);
}

static void place_tank(DC *d, int u, const double p[3], int kind, float size, float stand) {
    hc_begin(d, u);
    hc_push_wall(d, p, 0.0, -1.0);
    hr_tank(d->pm, kind, size, stand);
    hc_pop(d);
    hc_done(d);
}

/* Tanks on a deck: one to three along its back, on the deck or on a stand. */
static int deck_tanks(RU *r, const Deck *k, int want) {
    DC *d = r->d;
    int got = 0;
    for (int t = 0; t < want; ++t) {
        const int steel = ch(r->o, 0xC1u + (uint32_t)t, r->P.steel);
        const int stand = ch(r->o, 0xC4u, r->P.stand);
        const float size = steel ? 0.85f + 0.25f * fr(r->o, 0xC5u + (uint32_t)t) : 0.75f + 0.35f * fr(r->o, 0xC8u + (uint32_t)t);
        const float sh = stand ? r->P.stand0 + (r->P.stand1 - r->P.stand0) * fr(r->o, 0xCBu) : 0.0f;
        const double rad = stand ? 0.5 * HR_STAND_W + 0.12 : 0.5 * (steel ? HR_STEEL_D : HR_TANK_D) * size + 0.08;
        const double h = (stand ? sh + HR_STAND_TOP : 0.0) + (steel ? HR_STEEL_H : HR_TANK_H) * size + 0.1;
        int ok = 0;
        /* along the back from one side, then the sides' back corners */
        for (int c = 0; c < 8 && !ok; ++c) {
            const int sideFirst = (int)(r->o >> 7 & 1u);
            const double fx = (c >> 1) / 3.0;
            const double x = (c & 1) ^ sideFirst ? k->x1 - rad - fx * (k->x1 - k->x0 - 2.0 * rad) : k->x0 + rad + fx * (k->x1 - k->x0 - 2.0 * rad);
            const double z = k->z1 - rad;
            if (deck_clear(d, k, x - rad, z - rad, x + rad, z + rad, h)) {
                const double p[3] = { x, k->y, z };
                place_tank(d, r->u, p, steel, size, sh);
                const double lo[3] = { x - rad, k->y, z - rad }, hi[3] = { x + rad, k->y + h, z + rad };
                hc_occupy(d, lo, hi);
                ok = 1;
            }
        }
        if (!ok) break;
        got++;
    }
    return got;
}

/* A solar canopy over a deck's front, behind its tanks' row. */
static int deck_canopy(RU *r, const Deck *k) {
    DC *d = r->d;
    const double W = k->x1 - k->x0, D = k->z1 - k->z0;
    const double h = 2.3 + 0.3 * fr(r->o, 0xD1u), rise = 0.15 + 0.2 * fr(r->o, 0xD2u);
    for (int t = 0; t < 6; ++t) {
        const double w = clampd(t < 3 ? W - 0.1 : 0.6 * W, 2.4, 7.5), dep = clampd((t % 3 == 0 ? 0.6 : t % 3 == 1 ? 0.45 : 0.35) * D, 2.0, 4.5);
        if (w > W + 1e-6 || dep > D + 1e-6) continue;
        const double x0 = t < 3 ? k->x0 + 0.5 * (W - w) : (t & 1 ? k->x0 : k->x1 - w);
        const double z0 = k->z0;
        if (!deck_clear(d, k, x0, z0, x0 + w, z0 + dep, h + rise + 0.2)) continue;
        const double p[3] = { x0 + 0.5 * w, k->y, z0 + 0.5 * dep };
        hc_begin(d, r->u);
        hc_push_wall(d, p, 0.0, 1.0);   /* facing back: its front beam (the lights) on the street's side */
        hr_solar_canopy(d->pm, (float)w, (float)dep, (float)h, (float)rise, ch(r->o, 0xD3u, 0.3f + 0.6f * (float)d->g->affluence), NULL);
        hc_pop(d);
        hc_done(d);
        const double lo[3] = { x0, k->y, z0 }, hi[3] = { x0 + w, k->y + h + rise + 0.3, z0 + dep };
        hc_occupy(d, lo, hi);
        return 1;
    }
    return 0;
}

/* A run along a deck's parapet (or its slab's edge): rails, a breeze screen, razor wire on its top, plants over it. */
typedef struct Edge { double a[3], b[3], m[2]; int front; } Edge;
static int deck_edges(const RU *r, const Deck *k, Edge *E) {
    const Roof *R = &r->hr->R;
    int n = 0;
    for (int e = 0; e < 4; ++e) {
        if (R->cut[e] || R->role[e] == ER_HIGH || R->role[e] == ER_EXT) continue;
        const double *m = R->W.m[e], *a = R->W.p[e], *b = R->W.p[(e + 1) % 4];
        /* the coping's middle line, or a slab edge's line just inside it */
        const double in = k->parapet ? -0.125 : R->ov - 0.1;
        Edge *x = &E[n++];
        x->a[0] = a[0] + m[0] * in; x->a[1] = k->top; x->a[2] = a[1] + m[1] * in;
        x->b[0] = b[0] + m[0] * in; x->b[1] = k->top; x->b[2] = b[1] + m[1] * in;
        x->m[0] = m[0]; x->m[1] = m[1];
        x->front = m[1] < -0.7;
    }
    return n;
}
/* The clear stretches of an edge at height dy over the coping (nothing standing on it), as world points' t. */
static int edge_runs(const DC *d, const Edge *x, double dy, Ival *run) {
    double dir[3] = { x->b[0] - x->a[0], 0.0, x->b[2] - x->a[2] };
    const double len = sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    if (len < 0.6) return 0;
    dir[0] /= len; dir[2] /= len;
    const double p0[3] = { x->a[0], 0.0, x->a[2] }, off[3] = { 0.0, x->a[1] + dy, 0.0 };
    int n = hc_clear_runs(d->C, p0, dir, off, 0.1, len - 0.1, 0.1, 0.6, run, 16);
    /* and clear of what the kit set on the deck's edge */
    for (int k = 0; k < n; ++k) {
        const double lo[3] = { x->a[0] + dir[0] * run[k].a, x->a[1], x->a[2] + dir[2] * run[k].a };
        const double hi[3] = { x->a[0] + dir[0] * run[k].b, x->a[1] + 1.2, x->a[2] + dir[2] * run[k].b };
        double l2[3], h2[3];
        for (int i = 0; i < 3; ++i) { l2[i] = fmin(lo[i], hi[i]) - (i != 1 ? 0.15 : 0.0); h2[i] = fmax(lo[i], hi[i]) + (i != 1 ? 0.15 : 0.0); }
        if (!hc_free(d, l2, h2)) { run[k] = run[n - 1]; n--; k--; }
    }
    return n;
}
/* a frame at t along an edge, facing out, with the kit's x running the edge's way from there */
static void push_edge(DC *d, const Edge *x, const Ival *run, double y) {
    double dir[2] = { x->b[0] - x->a[0], x->b[2] - x->a[2] };
    const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    dir[0] /= len; dir[1] /= len;
    const double e2[2] = { -x->m[1], x->m[0] };
    const double sDir = e2[0] * dir[0] + e2[1] * dir[1];
    const double t = (-d->sx > 0.0) == (sDir > 0.0) ? run->a : run->b;
    const double q[3] = { x->a[0] + dir[0] * t, y, x->a[2] + dir[1] * t };
    hc_push_wall(d, q, x->m[0], x->m[1]);
}
static void edge_occupy(DC *d, const Edge *x, const Ival *run, double h) {
    double dir[2] = { x->b[0] - x->a[0], x->b[2] - x->a[2] };
    const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    dir[0] /= len; dir[1] /= len;
    const double lo[3] = { x->a[0] + dir[0] * run->a, x->a[1], x->a[2] + dir[1] * run->a };
    const double hi[3] = { x->a[0] + dir[0] * run->b, x->a[1] + h, x->a[2] + dir[1] * run->b };
    double l2[3], h2[3];
    for (int i = 0; i < 3; ++i) { l2[i] = fmin(lo[i], hi[i]) - (i != 1 ? 0.12 : 0.0); h2[i] = fmax(lo[i], hi[i]) + (i != 1 ? 0.12 : 0.0); }
    hc_occupy(d, l2, h2);
}

/* The highest window head on a wall under an edge's stretch (where plants hanging over the parapet must stop). */
static double head_under(const DC *d, const Edge *x, const Ival *run) {
    double best = -1e30;
    for (int i = 0; i < d->C->nwin; ++i) {
        const HsWin *w = &d->C->win[i];
        if (w->m[0] * x->m[0] + w->m[1] * x->m[1] < 0.99) continue;
        /* the window's run in t along the edge */
        double dir[2] = { x->b[0] - x->a[0], x->b[2] - x->a[2] };
        const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
        dir[0] /= len; dir[1] /= len;
        const double s0 = w->a0, s1 = w->a0 + w->w;
        const double pa[2] = { w->m[0] * w->at + w->e[0] * s0, w->m[1] * w->at + w->e[1] * s0 };
        const double pb[2] = { w->m[0] * w->at + w->e[0] * s1, w->m[1] * w->at + w->e[1] * s1 };
        const double ta = (pa[0] - x->a[0]) * dir[0] + (pa[1] - x->a[2]) * dir[1], tb = (pb[0] - x->a[0]) * dir[0] + (pb[1] - x->a[2]) * dir[1];
        const double off = fabs((pa[0] - x->a[0]) * x->m[0] + (pa[1] - x->a[2]) * x->m[1]);
        if (off > 0.5) continue;
        if (fmax(ta, tb) < run->a - 0.1 || fmin(ta, tb) > run->b + 0.1) continue;
        best = fmax(best, w->y1);
    }
    return best;
}

static void deck_terrace(RU *r, const Deck *k) {
    DC *d = r->d;
    Edge E[4];
    const int ne = deck_edges(r, k, E);
    const int rail = ch(r->o, 0xE1u, r->P.rail), breeze = !r->lean && ch(r->o, 0xE2u, r->P.breeze) && k->parapet;
    const int trail = !r->lean && ch(r->o, 0xE3u, r->P.trail) && k->parapet, razor = ch(r->o, 0xE4u, r->P.razor) && k->parapet;
    int any = 0;
    hc_begin(d, r->u);
    for (int i = 0; i < ne; ++i) {
        const Edge *x = &E[i];
        Ival run[16];
        /* plants trailing over the street's parapet first (they hang over its face, not its top) */
        if (trail && x->front) {
            const int n = edge_runs(d, x, 0.4, run);
            for (int j = 0; j < n; ++j) {
                const double head = head_under(d, x, &run[j]);
                const double drop = fmin(1.1, k->top - head - 0.1);
                if (drop < 0.35) continue;
                /* on the parapet's outer face: the edge's line is the coping's middle */
                Edge f = *x;
                f.a[0] += x->m[0] * 0.125; f.a[2] += x->m[1] * 0.125; f.b[0] += x->m[0] * 0.125; f.b[2] += x->m[1] * 0.125;
                push_edge(d, &f, &run[j], k->top);
                hr_trail(d->pm, (float)(run[j].b - run[j].a), (float)drop, hash_combine(r->o, 0xE5u + (uint32_t)j));
                hc_pop(d);
                any = 1;
            }
        }
        const int n = edge_runs(d, x, 0.4, run);
        for (int j = 0; j < n; ++j) {
            const double L = run[j].b - run[j].a;
            if (x->front && breeze && L > 1.0) {   /* a screened terrace: breeze blocks on the street's parapet */
                push_edge(d, x, &run[j], k->top);
                hr_breeze(d->pm, (int)(r->o >> 3 & 1u), (float)L, HR_BREEZE);
                hc_pop(d);
                edge_occupy(d, x, &run[j], HR_BREEZE);
                any = 1;
            } else if (razor && !x->front) {
                push_edge(d, x, &run[j], k->top);
                hr_razor(d->pm, (float)L);
                hc_pop(d);
                edge_occupy(d, x, &run[j], HR_RAZOR_D);
                any = 1;
            } else if (rail) {
                push_edge(d, x, &run[j], k->top);
                hr_rail(d->pm, (float)L);
                hc_pop(d);
                edge_occupy(d, x, &run[j], HR_RAIL_H);
                any = 1;
            }
        }
    }
    hc_done(d);
    /* pots along the inside of the street's parapet */
    if (ch(r->o, 0xE6u, r->P.pots) && !r->lean) {
        const int want = 2 + (int)(fr(r->o, 0xE7u) * 3.0f);
        int got = 0;
        for (int t = 0; t < 10 && got < want; ++t) {
            const double x = k->x0 + 0.3 + (k->x1 - k->x0 - 0.6) * fr(r->o, 0xF0u + (uint32_t)t), z = k->z0 + 0.3;
            if (!deck_clear(d, k, x - 0.22, z - 0.22, x + 0.22, z + 0.22, 0.9)) continue;
            const int palm = ch(r->o, 0xFAu + (uint32_t)t, r->P.palm * 0.5f);
            const double p[3] = { x, k->y, z };
            hc_begin(d, r->u);
            hc_push_wall(d, p, 0.0, -1.0);
            pm_rotate_y(d->pm, (float)(fr(r->o, 0x100u + (uint32_t)t) * 6.2831853));
            pm_place(d->pm, palm ? PMK_R_POT_PALM : PMK_R_POT);
            hc_pop(d);
            hc_done(d);
            const double lo[3] = { x - 0.25, k->y, z - 0.25 }, hi[3] = { x + 0.25, k->y + 1.0, z + 0.25 };
            hc_occupy(d, lo, hi);
            got++;
        }
    }
    if (!any && (rail || breeze || trail || razor)) hc_repair(d, HDR_TERRACE, "no clear parapet for its terrace (%.0f)", r->u, 0.0);
}

/* An aerial on a deck's corner. */
static void deck_aerial(RU *r, const Deck *k) {
    DC *d = r->d;
    const int cross = ch(r->o, 0x111u, r->P.cross);
    for (int c = 0; c < 4; ++c) {
        const double x = c & 1 ? k->x1 - 0.15 : k->x0 + 0.15, z = c & 2 ? k->z0 + 0.15 : k->z1 - 0.15;
        if (!deck_clear(d, k, x - 0.1, z - 0.1, x + 0.1, z + 0.1, 2.0)) continue;
        const double p[3] = { x, k->y, z };
        hc_begin(d, r->u);
        hc_push_wall(d, p, 0.0, -1.0);
        pm_rotate_y(d->pm, (float)(fr(r->o, 0x112u) * 6.2831853));
        pm_place(d->pm, cross ? PMK_R_AERIAL_CROSS : PMK_R_AERIAL_YAGI);
        hc_pop(d);
        hc_done(d);
        const double lo[3] = { x - 0.6, k->y, z - 0.6 }, hi[3] = { x + 0.6, k->y + 2.8, z + 0.6 };
        hc_occupy(d, lo, hi);
        return;
    }
}

/* A flat roof in two passes: its tanks and canopy (before the risers, which come up to them), then its terrace's
 * edges and aerial (after the risers, whose pipes cross the parapet: the railings break round them). */
static void flat_roof(RU *r, int pass) {
    Deck k;
    if (!deck_of(r->hr, r->v, &k)) return;
    DC *d = r->d;
    if (pass) {
        deck_terrace(r, &k);
        if (ch(r->o, 0x125u, r->P.aerial)) deck_aerial(r, &k);
        return;
    }
    /* tanks along the back first (the sheets' every flat roof), then a canopy in front of them, then the terrace's edges */
    if (ch(r->o, 0x121u, r->P.tanks)) {
        const int want = 1 + (int)(fr(r->o, 0x122u) * (home_style_region(d->g->style) == PLAN_REGION_SAS || home_style_region(d->g->style) == PLAN_REGION_LAT ? 2.9f : 1.8f));
        if (!deck_tanks(r, &k, want)) hc_repair(d, HDR_TANK, "no room on its deck for a tank (%.0f)", r->u, (double)want);
    }
    if (ch(r->o, 0x123u, r->P.solar)) {
        if (!ch(r->o, 0x124u, r->P.canopy) || !deck_canopy(r, &k)) hc_repair(d, HDR_SOLAR, "no canopy over its deck (%.0f)", r->u, 0.0);
    }
}

/* The pitched roof's slopes: panels laid on one; a level frame over the ridge with panels on it; a tank on a cradle. */
typedef struct Slope { int e; double dd[2], m[2], a, b, half, t, cosA, sinA; } Slope;
static int slope_of(const RU *r, int front, Slope *S) {
    const Roof *R = &r->hr->R;
    if (R->F.n != 4 || (R->kind != HR_GABLE && R->kind != HR_HIP)) return 0;
    for (int e = 0; e < 4; ++e) {
        if (R->role[e] != ER_EAVES) continue;
        const double *m = R->F.m[e];
        if (front ? m[1] > -0.7 : m[1] < 0.7) continue;
        double len, t0, t1;
        hc_eaves_line(R, e, S->dd, &len, &t0, &t1);
        S->a = fmax(t0, 0.0); S->b = fmin(t1, len);
        double span = 1e9;
        for (int f = 0; f < 4; ++f)
            if (f != e && R->F.m[f][0] * m[0] + R->F.m[f][1] * m[1] < -0.99)
                span = fmin(span, fabs((R->F.p[f][0] - R->F.p[e][0]) * m[0] + (R->F.p[f][1] - R->F.p[e][1]) * m[1]));
        if (span > 1e8) return 0;
        S->e = e; S->m[0] = m[0]; S->m[1] = m[1];
        S->half = 0.5 * span; S->t = R->t;
        const double ang = atan(R->t);
        S->cosA = cos(ang); S->sinA = sin(ang);
        return S->b - S->a > 1.5;
    }
    return 0;
}
/* a point on the slope: tc along the eaves, q in (plan) from the wall line, raised `up` over the covering */
static void slope_pt(const RU *r, const Slope *S, double tc, double q, double up, double p[3]) {
    const Roof *R = &r->hr->R;
    p[0] = R->F.p[S->e][0] + S->dd[0] * tc - S->m[0] * q;
    p[2] = R->F.p[S->e][1] + S->dd[1] * tc - S->m[1] * q;
    p[1] = R->E + R->t * q + r->hr->dy + up;
}
/* whether panels over tc0..tc1 along the eaves and q0..q1 in from it fit: on this roof's own slabs, nothing over them */
static int slope_area_fits(const RU *r, const Slope *S, double tc0, double tc1, double q0, double q1, double tall) {
    const DC *d = r->d;
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
    for (int i = 0; i <= 3; ++i)
        for (int j = 0; j <= 3; ++j) {
            const double tc = tc0 + (tc1 - tc0) * i / 3.0, q = q0 + (q1 - q0) * j / 3.0;
            double p[3];
            slope_pt(r, S, tc, q, -0.04, p);
            if (!on_slab(d, r->hr->vol, p)) return 0;
            for (int k = 1; k <= 2; ++k) {
                slope_pt(r, S, tc, q, tall * k / 2.0, p);
                if (!clear_at(d, p[0], p[1], p[2])) return 0;
            }
            slope_pt(r, S, tc, q, 0.0, p);
            for (int c = 0; c < 3; ++c) { lo[c] = fmin(lo[c], p[c]); hi[c] = fmax(hi[c], p[c]); }
        }
    hi[1] += tall;
    return hc_free(d, lo, hi);
}

static int slope_panels(RU *r, const Slope *S) {
    DC *d = r->d;
    const double pw = HR_PANEL_W + 0.02, pl = (HR_PANEL_L + 0.02) * S->cosA;   /* a panel's plan depth up the slope */
    int maxCols = (int)floor((S->b - S->a - 0.5) / pw), maxRows = (int)floor((S->half - 0.7) / pl);
    if (maxCols > 8) maxCols = 8;
    if (maxRows > 3) maxRows = 3;
    if (maxCols < 2 || maxRows < 1) return 0;
    for (int rows = maxRows; rows >= 1; --rows)
        for (int cols = maxCols; cols >= 2; --cols) {
            static const double SHIFT[3] = { 0.0, -0.6, 0.6 };
            for (int sh = 0; sh < 3; ++sh) {
                const double tc = 0.5 * (S->a + S->b) + SHIFT[sh], q0 = 0.45;
                const double w = cols * pw, q1 = q0 + rows * pl;
                if (tc - 0.5 * w < S->a + 0.2 || tc + 0.5 * w > S->b - 0.2) continue;
                if (!slope_area_fits(r, S, tc - 0.5 * w, tc + 0.5 * w, q0, q1, 0.2)) continue;
                double p[3];
                slope_pt(r, S, tc, q0, 0.004, p);
                const double ey[3] = { S->m[0] * S->sinA, S->cosA, S->m[1] * S->sinA }, ez[3] = { -S->m[0] * S->cosA, S->sinA, -S->m[1] * S->cosA };
                hc_begin(d, r->u);
                hc_push_yz(d, p, ey, ez);
                hr_solar_flush(d->pm, cols, rows);
                hc_pop(d);
                hc_done(d);
                double lo[3], hi[3], a[3], b[3];
                slope_pt(r, S, tc - 0.5 * w, q0, 0.0, a);
                slope_pt(r, S, tc + 0.5 * w, q1, 0.25, b);
                for (int c = 0; c < 3; ++c) { lo[c] = fmin(a[c], b[c]); hi[c] = fmax(a[c], b[c]); }
                hc_occupy(d, lo, hi);
                return 1;
            }
        }
    return 0;
}

/* A level frame over the ridge (the inter-war sheet's canopies standing over the roof), its posts down to both slopes. */
static int ridge_canopy(RU *r) {
    DC *d = r->d;
    const Roof *R = &r->hr->R;
    Slope F, B;
    if (!slope_of(r, 1, &F) || !slope_of(r, 0, &B)) return 0;
    if (fabs(F.half - B.half) > 0.05) return 0;
    const double ridgeY = R->E + R->t * F.half + r->hr->dy;
    /* its middle over the unit's slice of the ridge */
    const double w = fmin(F.b - F.a - 0.8, 5.5), dep = fmin(2.0 * F.half - 1.2, 2.4 + 0.8 * fr(r->o, 0x131u));
    if (w < 2.2 || dep < 1.8) return 0;
    const double tcF = 0.5 * (F.a + F.b);
    double mid[3];
    slope_pt(r, &F, tcF, F.half, 0.0, mid);   /* on the ridge */
    const double h = 0.55, top = ridgeY + h;
    /* posts: two columns (three when long) each side; their feet on the slopes */
    const int nx = w > 3.6 ? 3 : 2;
    float feet[6];
    for (int i = 0; i < nx; ++i)
        for (int k = 0; k < 2; ++k) {
            const double along = -0.5 * w + 0.04 + (w - 0.08) * i / (nx - 1);
            const double qIn = F.half - 0.5 * dep + 0.04;   /* in from that slope's eaves */
            const Slope *S = k ? &B : &F;
            /* along the front slope's eaves dd; the back's runs the other way */
            const double tc = S == &F ? tcF + along : (S->dd[0] * (mid[0] + F.dd[0] * along) + S->dd[1] * (mid[2] + F.dd[1] * along)) -
                                                      (S->dd[0] * R->F.p[S->e][0] + S->dd[1] * R->F.p[S->e][1]);
            double p[3];
            slope_pt(r, S, tc, qIn, -0.04, p);
            if (!on_slab(d, r->hr->vol, p)) return 0;
            slope_pt(r, S, tc, qIn, 0.0, p);
            feet[i * 2 + k] = (float)(p[1] - ridgeY);
            const double lo[3] = { p[0] - 0.1, p[1], p[2] - 0.1 }, hi[3] = { p[0] + 0.1, top, p[2] + 0.1 };
            if (!hc_free(d, lo, hi)) return 0;
        }
    /* nothing coming up through it (a stack) */
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; j <= 3; ++j) {
            const double al = -0.5 * w + w * i / 4.0, ac = -0.5 * dep + dep * j / 3.0;
            const double x = mid[0] + F.dd[0] * al - F.m[0] * ac, z = mid[2] + F.dd[1] * al - F.m[1] * ac;
            if (!clear_at(d, x, top + 0.1, z) || !clear_at(d, x, top - 0.25, z)) return 0;
        }
    const double p[3] = { mid[0], ridgeY, mid[2] };
    hc_begin(d, r->u);
    hc_push_wall(d, p, -F.m[0], -F.m[1]);   /* facing back: its front on the street's side */
    hr_solar_canopy(d->pm, (float)w, (float)dep, (float)h, 0.0f, 0, feet);
    hc_pop(d);
    hc_done(d);
    const double lo[3] = { mid[0] - 0.5 * w - 0.5 * dep, ridgeY - 1.0, mid[2] - 0.5 * w - 0.5 * dep }, hi[3] = { mid[0] + 0.5 * w + 0.5 * dep, top + 0.3, mid[2] + 0.5 * w + 0.5 * dep };
    hc_occupy(d, lo, hi);
    return 1;
}

/* A tank on a pitched roof: on a flat extension's roof behind, or on a cradle on the back slope by the ridge. */
static int pitched_tank(RU *r) {
    DC *d = r->d;
    const HGraph *g = d->g;
    const int steel = ch(r->o, 0x141u, r->P.steel);
    const float size = steel ? 0.85f : 0.75f + 0.3f * fr(r->o, 0x142u);
    const double rad = 0.5 * (steel ? HR_STEEL_D : HR_TANK_D) * size + 0.08;
    const double h = (steel ? HR_STEEL_H : HR_TANK_H) * size + 0.1;
    /* a flat roof of the unit's own behind the main: an extension's or an outrigger's */
    for (int i = 0; i < d->C->nroof; ++i) {
        const HsRoof *hr = &d->C->roof[i];
        const HVol *v = &g->vol[hr->vol];
        if (hr->unit != r->u || hc_is_house_body(v->role) || v->role == HV_PORCH || v->role == HV_BAY) continue;
        Deck k;
        if (!deck_of(hr, v, &k)) continue;
        for (int c = 0; c < 4; ++c) {
            const double x = c & 1 ? k.x1 - rad : k.x0 + rad, z = c & 2 ? k.z0 + rad : k.z1 - rad;
            if (!deck_clear(d, &k, x - rad, z - rad, x + rad, z + rad, h)) continue;
            const double p[3] = { x, k.y, z };
            place_tank(d, r->u, p, steel, size, 0.0f);
            const double lo[3] = { x - rad, k.y, z - rad }, hi[3] = { x + rad, k.y + h, z + rad };
            hc_occupy(d, lo, hi);
            return 1;
        }
    }
    /* a cradle on the back slope: a platform level with the ridge less a little, its four legs down to the tiles */
    Slope S;
    if (!slope_of(r, 0, &S)) return 0;
    const Roof *R = &r->hr->R;
    const double plat = HR_STAND_W + 0.16, hp = 0.5 * plat;
    const double ridgeY = R->E + R->t * S.half + r->hr->dy;
    for (int t = 0; t < 10; ++t) {
        /* along the ridge from its middle (a hipped roof's ridge is short), then towards its ends; by the ridge, then
         * lower down the slope */
        static const double TRY[5] = { 0.0, 1.0, -1.0, 2.0, -2.0 };
        const double tc = 0.5 * (S.a + S.b) + TRY[t % 5];
        if (tc - hp < S.a + 0.2 || tc + hp > S.b - 0.2) continue;
        /* the platform's back edge by the ridge, its front as far down the slope as its width */
        const double qBack = S.half - (t < 5 ? 0.15 : 0.9), qFront = qBack - plat;
        if (qFront < 0.8) continue;
        double p0[3], p1[3];
        slope_pt(r, &S, tc, qBack, 0.0, p1);
        slope_pt(r, &S, tc, qFront, 0.0, p0);
        const double top = fmax(p1[1] + 0.12, ridgeY - 0.35);
        if (top - p0[1] < 0.25) continue;
        int ok = 1;
        float feet[4];
        for (int k = 0; k < 4 && ok; ++k) {
            const double tk = tc + (k & 1 ? hp - 0.06 : -hp + 0.06), qk = k & 2 ? qBack - 0.06 : qFront + 0.06;
            double f[3];
            slope_pt(r, &S, tk, qk, -0.04, f);
            if (!on_slab(d, r->hr->vol, f)) ok = 0;
            slope_pt(r, &S, tk, qk, 0.0, f);
            feet[k] = (float)(f[1] - top);
        }
        if (!ok) continue;
        double c[3];
        slope_pt(r, &S, tc, 0.5 * (qBack + qFront), 0.0, c);
        const double lo[3] = { c[0] - hp - 0.1, c[1] - 0.2, c[2] - hp - 0.1 }, hi[3] = { c[0] + hp + 0.1, top + HR_STAND_TOP + h, c[2] + hp + 0.1 };
        if (!hc_free(d, lo, hi)) continue;
        for (int i = 0; i <= 2 && ok; ++i)
            for (int j = 0; j <= 2 && ok; ++j)
                for (int l = 0; l < 3 && ok; ++l)
                    ok = clear_at(d, c[0] + (i - 1) * hp, top + 0.1 + h * l / 2.0, c[2] + (j - 1) * hp);
        if (!ok) continue;
        const double p[3] = { c[0], top, c[2] };
        hc_begin(d, r->u);
        hc_push_wall(d, p, S.m[0], S.m[1]);
        /* the legs: square posts from the tiles to the platform's underside */
        for (int k = 0; k < 4; ++k) {
            pm_push(d->pm);
            pm_translate(d->pm, k & 1 ? (float)(hp - 0.06) : (float)(-hp + 0.06), feet[k], k & 2 ? -(float)(hp - 0.06) : (float)(hp - 0.06));
            pm_scale(d->pm, 0.75f, -feet[k], 0.75f);
            pm_place(d->pm, PMK_R_FRAME_POST);
            pm_pop(d->pm);
        }
        pm_place(d->pm, PMK_R_STAND_TOP);
        pm_push(d->pm); pm_translate(d->pm, 0.0f, HR_STAND_TOP, 0.0f);
        hr_tank(d->pm, steel, size, 0.0f);
        pm_pop(d->pm);
        hc_pop(d);
        hc_done(d);
        hc_occupy(d, lo, hi);
        return 1;
    }
    return 0;
}

/* An aerial: lashed to the top of one of the unit's stacks, or on its ridge. */
static void pitched_aerial(RU *r) {
    DC *d = r->d;
    if (!ch(r->o, 0x151u, r->P.aerial)) return;
    const int cross = ch(r->o, 0x152u, r->P.cross);
    for (int i = 0; i < d->C->nstack; ++i) {
        const HsStack *s = &d->C->stack[i];
        if (s->unit != r->u) continue;
        const int end = ch(r->o, 0x153u, 0.5f) ? 1 : -1;
        const double p[3] = { s->x + end * (0.5 * s->w - 0.1), s->top + 0.03, s->z + 0.5 * s->d - 0.07 };
        const double lo[3] = { p[0] - 0.5, p[1], p[2] - 0.5 }, hi[3] = { p[0] + 0.5, p[1] + 2.8, p[2] + 0.5 };
        if (!hc_free(d, lo, hi)) continue;
        hc_begin(d, r->u);
        hc_push_wall(d, p, 0.0, -1.0);
        pm_rotate_y(d->pm, (float)(fr(r->o, 0x154u) * 6.2831853));
        pm_place(d->pm, cross ? PMK_R_AERIAL_CROSS : PMK_R_AERIAL_YAGI);
        hc_pop(d);
        hc_done(d);
        hc_occupy(d, lo, hi);
        return;
    }
    /* no stack: on the ridge near an end of its slice (a gable's) */
    Slope F;
    if (!slope_of(r, 1, &F) || r->hr->R.kind != HR_GABLE) return;
    const double tc = ch(r->o, 0x155u, 0.5f) ? F.a + 0.5 : F.b - 0.5;
    double p[3], under[3];
    slope_pt(r, &F, tc, F.half - 0.02, -0.06, under);
    if (!on_slab(d, r->hr->vol, under)) return;
    slope_pt(r, &F, tc, F.half, 0.02, p);
    const double lo[3] = { p[0] - 0.5, p[1], p[2] - 0.5 }, hi[3] = { p[0] + 0.5, p[1] + 2.8, p[2] + 0.5 };
    if (!hc_free(d, lo, hi) || !clear_at(d, p[0], p[1] + 0.5, p[2])) return;
    hc_begin(d, r->u);
    hc_push_wall(d, p, 0.0, -1.0);
    pm_place(d->pm, cross ? PMK_R_AERIAL_CROSS : PMK_R_AERIAL_YAGI);
    hc_pop(d);
    hc_done(d);
    hc_occupy(d, lo, hi);
}

/* A tank on a tall stand in the yard, behind the house or beside it (the African sheets' panels 1, 3-5): on the unit's own
 * plot, clear of its wings and extensions. */
static int yard_tank(RU *r) {
    DC *d = r->d;
    const HUnit *U = &d->g->unit[r->u];
    const Roof *R = &r->hr->R;
    double x0 = 1e30, x1 = -1e30, z1 = -1e30;
    for (int e = 0; e < R->W.n; ++e) { x0 = fmin(x0, R->W.p[e][0]); x1 = fmax(x1, R->W.p[e][0]); z1 = fmax(z1, R->W.p[e][1]); }
    const float sh = r->P.stand0 + (r->P.stand1 - r->P.stand0) * fr(r->o, 0x171u);
    const double rad = 0.5 * HR_STAND_W + 0.12, h = sh + HR_STAND_TOP + HR_TANK_H + 0.1, y = r->hr->dy;
    double cand[6][2];
    int n = 0;
    const double mid = 0.5 * (x0 + x1);
    cand[n][0] = mid + 1.2; cand[n++][1] = z1 + rad + 0.5;   /* the back yard */
    cand[n][0] = mid - 1.2; cand[n++][1] = z1 + rad + 0.5;
    cand[n][0] = mid; cand[n++][1] = z1 + rad + 1.5;
    cand[n][0] = x1 + rad + 0.3; cand[n++][1] = z1 - rad - 0.5;   /* the side yards, at their backs */
    cand[n][0] = x0 - rad - 0.3; cand[n++][1] = z1 - rad - 0.5;
    for (int c = 0; c < n; ++c) {
        const double x = cand[c][0], z = cand[c][1];
        if (x - rad < U->x0 + 0.2 || x + rad > U->x1 - 0.2 || z + rad > U->depth - 0.3 || z - rad < 0.5) continue;
        int ok = 1;
        for (int i = 0; i <= 2 && ok; ++i)
            for (int j = 0; j <= 2 && ok; ++j)
                for (int l = 0; l < 4 && ok; ++l) ok = clear_at(d, x + (i - 1) * rad, y + 0.1 + (h - 0.1) * l / 3.0, z + (j - 1) * rad);
        const double lo[3] = { x - rad, y, z - rad }, hi[3] = { x + rad, y + h, z + rad };
        if (!ok || !hc_free(d, lo, hi)) continue;
        const double p[3] = { x, y, z };
        place_tank(d, r->u, p, 0, 0.9f + 0.2f * fr(r->o, 0x172u), sh);
        hc_occupy(d, lo, hi);
        return 1;
    }
    return 0;
}

static void pitched_roof(RU *r) {
    DC *d = r->d;
    /* tanks first (on a stand in the yard where that is the custom, else a cradle by the ridge), then solar, then the
     * aerial on the stack */
    if (ch(r->o, 0x161u, r->P.tanks)) {
        int ok = ch(r->o, 0x165u, fminf(0.9f, 1.5f * r->P.stand)) && yard_tank(r);
        if (!ok) ok = pitched_tank(r);
        if (!ok) hc_repair(d, HDR_TANK, "no flat roof or clear back slope for its tank (%.0f)", r->u, 0.0);
    }
    if (ch(r->o, 0x162u, r->P.solar)) {
        int ok = 0;
        if (ch(r->o, 0x163u, r->P.ridge)) ok = ridge_canopy(r);
        /* panels on a slope: the back's mostly, the street's for some (the sheets show them both ways) */
        const int frontFirst = ch(r->o, 0x164u, 0.35f);
        for (int k = 0; k < 2 && !ok; ++k) {
            Slope S;
            if (slope_of(r, k ? !frontFirst : frontFirst, &S)) ok = slope_panels(r, &S);
        }
        if (!ok) hc_repair(d, HDR_SOLAR, "no clear slope for its panels (%.0f)", r->u, 0.0);
    }
    pitched_aerial(r);
}

/* ---------------------------------------------------------------------------
 * The driver
 * ------------------------------------------------------------------------- */

void hs_retrofit(DC *d) {
    const HGraph *g = d->g;
    if (!home_retrofit_style(g->style)) return;
    /* porch posts: kept clear (they are the kit's, not solids) */
    for (int i = 0; i < d->C->npost; ++i) {
        const HsPost *p = &d->C->post[i];
        const double lo[3] = { p->x - 0.15, p->y0, p->z - 0.15 }, hi[3] = { p->x + 0.15, p->y1, p->z + 0.15 };
        hc_occupy(d, lo, hi);
    }
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) {
        RU r;
        memset(&r, 0, sizeof(r));
        r.d = d; r.u = u;
        r.cs = &d->C->cs[u];
        r.lean = r.cs->lean;
        r.o = hash_combine(g->unit[u].seed, 0x2E7Fu);
        profile(g, u, r.cs, &r.P);
        for (int i = 0; i < d->C->nroof && !r.hr; ++i) {
            const HsRoof *hr = &d->C->roof[i];
            if (hr->unit == u && hc_is_house_body(g->vol[hr->vol].role) && hr->R.W.n == 4) r.hr = hr;
        }
        if (!r.hr) continue;
        r.v = home_unit_vol(g, r.hr->vol, u, &r.vOwn);
        r.nw = unit_walls(d, r.hr, r.w, 6);
        for (int i = 0; i < d->C->ndoor; ++i) if (d->C->door[i].unit == u && !r.dr) r.dr = &d->C->door[i];
        for (int i = 0; i < d->C->ndoor; ++i) if (d->C->door[i].unit == u && !d->C->door[i].porch) r.dr = &d->C->door[i];
        const uint32_t n0 = inst_n(d);
        /* the roof first (its pieces are the biggest), then the door's, then the walls' */
        const int flat = r.hr->R.kind == HR_FLAT;
        if (flat) flat_roof(&r, 0);
        else pitched_roof(&r);
        door_light(&r);
        door_gate(&r);
        door_meter(&r);
        risers(&r);
        if (flat) flat_roof(&r, 1);
        vines(&r);
        eaves_led(&r);
        porch_breeze(&r);
        door_pots(&r);
        d->D->st.retrofit += placed_since(d, n0);
    }
}
