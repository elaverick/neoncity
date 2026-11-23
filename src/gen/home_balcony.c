#include "home_char_internal.h"
#include "pmodel_retrofit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Balconies (residential roadmap HM5e2, ~/.claude/plans/residential-buildings.md "HM5e"; the user's concept
 * sheets in docs/reference/international-residential, panel 1 of each: docs/residential-target.md "Balconies").
 *
 *   the graph     home_balconies: a row's balconies by its style, on its units' fronts, after the repairs (the
 *                 volumes are final), each across whole bays of the front's window rhythm (home_front_bays) so
 *                 the windows behind it become French windows; home_check_balconies: their rules
 *   the shell     home_shell.c: slabs, ledges, upstands and caps as solids (both levels); loggias and verandas
 *                 carved (near); French windows
 *   the dressing  hs_balconies: rails, glazing, consoles, and what owners put out on them (pots, trailing
 *                 plants, an LED strip under the slab), kept out of the retrofit layer's way
 *
 * The rules (section 13.5's spirit, the projections' and clearances'): a balcony stands on an upper storey of
 * its own unit (a storey its unit has: an unevenly built row's short units have fewer), whole within its unit's
 * slice of the front and BALC_PARTY off a party wall or a corner; projecting, it stays over its plot (a
 * street-line front reaches at most BALC_STREET over the pavement, as balconies there do); it stands over no
 * porch, bay or canopy that rises within reach of its slab; the French window behind it fits under the eaves;
 * two on one storey of a unit keep apart; a carved one leaves the body three metres behind it.
 * ========================================================================= */

static int ch(uint32_t h, uint32_t k, float p) { return hash_float(hash_combine(h, k)) < p; }

const char *home_balcony_name(int k) {
    static const char *const N[HBK_COUNT] = { "balconette", "slab", "glazed", "loggia", "veranda" };
    return k >= 0 && k < HBK_COUNT ? N[k] : "?";
}

/* ---------------------------------------------------------------------------
 * The styles: each sheet's panel 1 (the plan's "Panel 1, style by style")
 * ------------------------------------------------------------------------- */

typedef struct BStyle {
    float   chance;           /* a unit's chance of balconies (a row's, if row) */
    uint8_t row;              /* the row decides as one (an owner leaves them off one time in ten) */
    uint8_t kind, kind2;      /* HBK_*, and a second kind a unit may take */
    float   share2;           /* ...its share */
    uint8_t rail;             /* HBL_* */
    float   noRail;           /* a unit's chance its slabs were never railed (self-built) */
    uint8_t lo, hi;           /* the storeys (hi 0: every upper one) */
    uint8_t each;             /* every window its own (else bays across) */
    float   perBay;           /* each: a window's chance */
    uint8_t span0, span1;     /* else bays across (span1 0: the whole front) */
    uint8_t centred;          /* ...set in the middle of the front */
    float   stack;            /* each storey over the first: the chance it has one too, over the same bays */
    float   depth0, depth1;
    float   consoles;         /* stone consoles under a slab */
    float   arch;             /* a loggia's arched head */
} BStyle;

static const BStyle BSTYLES[HS_COUNT] = {
    /* iron balconettes across the first floor, on their stone ledges: every window of the row's piano nobile */
    [HS_GEORGIAN]   = { .chance = 0.85f, .row = 1, .kind = HBK_BALCONETTE, .rail = HBL_ORNATE, .lo = 1, .hi = 1, .each = 1, .perBay = 0.95f,
                        .stack = 0.0f, .depth0 = 0.26f, .depth1 = 0.32f },
    /* an iron balcony at every upper window, a small stone slab on consoles */
    [HS_TOWNHOUSE]  = { .chance = 0.85f, .kind = HBK_SLAB, .rail = HBL_IRON, .lo = 1, .each = 1, .perBay = 0.8f, .stack = 1.0f,
                        .depth0 = 0.45f, .depth1 = 0.7f, .consoles = 0.8f },
    /* an iron balcony on a slab over the first floor's middle */
    [HS_SEU_VILLA]  = { .chance = 0.75f, .kind = HBK_SLAB, .rail = HBL_IRON, .lo = 1, .hi = 1, .span0 = 1, .span1 = 2, .centred = 1,
                        .depth0 = 0.8f, .depth1 = 1.2f, .consoles = 0.6f },
    /* the blocks' slab balconies, some glazed in */
    [HS_CHINA]      = { .chance = 0.65f, .kind = HBK_SLAB, .kind2 = HBK_GLAZED, .share2 = 0.35f, .rail = HBL_STEEL, .lo = 1, .span0 = 1, .span1 = 2,
                        .stack = 0.85f, .depth0 = 0.8f, .depth1 = 1.2f },
    /* the walk-up villa's stacked balconies, glazed or railed, broad */
    [HS_KOREA]      = { .chance = 0.9f, .kind = HBK_GLAZED, .kind2 = HBK_SLAB, .share2 = 0.45f, .rail = HBL_STEEL, .lo = 1, .span0 = 2, .span1 = 0,
                        .stack = 0.9f, .depth0 = 1.0f, .depth1 = 1.4f },
    /* a small first-floor balcony */
    [HS_LINKHOUSE]  = { .chance = 0.55f, .kind = HBK_SLAB, .rail = HBL_STEEL, .lo = 1, .hi = 1, .span0 = 1, .span1 = 1,
                        .depth0 = 0.9f, .depth1 = 1.2f },
    /* cantilevered slabs with railings, on most storeys; chajjas shade the rest (HM5e1) */
    [HS_RCC]        = { .chance = 0.8f, .kind = HBK_SLAB, .rail = HBL_STEEL, .lo = 1, .span0 = 1, .span1 = 2, .stack = 0.7f,
                        .depth0 = 0.9f, .depth1 = 1.3f },
    /* slabs out over the street, some never railed */
    [HS_SELFBUILT]  = { .chance = 0.7f, .kind = HBK_SLAB, .rail = HBL_STEEL, .noRail = 0.15f, .lo = 1, .span0 = 1, .span1 = 0, .stack = 0.6f,
                        .depth0 = 0.6f, .depth1 = 0.8f },
    /* the recessed loggia over the door, railed, under an arch more often than not */
    [HS_MENA_VILLA] = { .chance = 0.8f, .kind = HBK_LOGGIA, .rail = HBL_IRON, .lo = 1, .hi = 1, .span0 = 1, .span1 = 2, .centred = 1,
                        .depth0 = 1.2f, .depth1 = 1.6f, .arch = 0.7f },
    /* the upper veranda between piers, its balustrade */
    [HS_WALLED]     = { .chance = 0.8f, .kind = HBK_VERANDA, .rail = HBL_BALUSTRADE, .lo = 1, .hi = 1, .span0 = 1, .span1 = 0,
                        .depth0 = 1.8f, .depth1 = 2.4f },
};

/* ---------------------------------------------------------------------------
 * The rules
 * ------------------------------------------------------------------------- */

static int main_vol(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

/* a unit's slice of the main's front: a..b; and whether each end is a party wall */
static int unit_slice(const HGraph *g, int mi, int u, float *a, float *b) {
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return 0;
    *a = s0; *b = s0 + bw * (float)nb;
    return nb;
}

/* the least a balcony of a kind reaches out (a carved one's, how deep it is cut) */
static float least_depth(int kind) { return kind == HBK_BALCONETTE ? 0.2f : kind == HBK_GLAZED ? 0.8f : kind == HBK_SLAB ? 0.45f : 1.0f; }

/* the nearest the street a projecting balcony's front comes (z) */
static float reach_min(const HGraph *g) { return home_style_street(g->style) ? -BALC_STREET : 0.3f; }

/* What breaks a rule, or NULL: b checked against the graph and the balconies before it (n of them). */
static const char *balcony_fault(const HGraph *g, const HBalcony *b, int n) {
    const int mi = b->vol;
    if (b->unit < 0 || b->unit >= g->nunit) return "unit";
    if (mi < 0 || mi >= g->nvol || g->vol[mi].role > HV_ROW) return "main";
    HVol own;
    const HVol *m = home_unit_vol(g, mi, b->unit, &own);
    if (b->storey < 1 || b->storey >= m->storeys) return "storey";
    if (fabsf(b->z - m->z0) > 0.01f) return "wall";
    float a, e;
    if (!unit_slice(g, mi, b->unit, &a, &e)) return "slice";
    if (b->x0 < a + BALC_PARTY - HEPS || b->x1 > e - BALC_PARTY + HEPS || b->x1 - b->x0 < 0.9f) return "party";
    if (b->depth < least_depth(b->kind) - HEPS) return "shallow";
    if (home_balcony_carved(b->kind)) {
        if (b->depth > (m->z1 - m->z0) - 3.0f + HEPS) return "deep";
    } else if (b->z - b->depth < reach_min(g) - HEPS) return "reach";
    const float floorY = home_balcony_floor(g, b) + BALC_DROP;
    if (floorY + 2.0f > m->eaves - 0.2f + HEPS) return "eaves";
    /* nothing rising in front of the wall under it: porches (but those carved in), bays, canopies */
    /* (a carved one's floor is the body's: only what reaches into its opening) */
    const float under = home_balcony_floor(g, b) - (home_balcony_carved(b->kind) ? 0.0f : BALC_SLAB + 0.15f);
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->unit != b->unit || (v->role != HV_PORCH && v->role != HV_BAY) || (v->role == HV_PORCH && home_porch_recessed(v->kind))) continue;
        if (v->x1 < b->x0 - 0.2f || v->x0 > b->x1 + 0.2f || v->z0 > m->z0 - 0.01f) continue;
        if (v->top > under) return "under";
    }
    for (int i = 0; i < n; ++i) {
        const HBalcony *o = &g->balc[i];
        if (o == b || o->unit != b->unit || o->storey != b->storey) continue;
        if (b->x0 < o->x1 + 0.2f && b->x1 > o->x0 - 0.2f) return "apart";
    }
    return NULL;
}

int home_check_balconies(const HGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nbalc; ++i) {
        const char *f = balcony_fault(g, &g->balc[i], i);
        if (!f) continue;
        blog_printf(log, "  rejected balcony: %s %d (unit %d, storey %d) breaks %s\n", home_balcony_name(g->balc[i].kind), i,
                    g->balc[i].unit, g->balc[i].storey, f);
        ok = 0;
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * The grammar
 * ------------------------------------------------------------------------- */

/* The balcony a unit's front door stands under (a projecting one on the first floor, deep enough to keep the rain off
 * it, the door's whole width under it), or -1: it serves as the door's canopy. */
int home_door_balcony(const HGraph *g, int u) {
    if (u < 0 || u >= g->nunit || g->unit[u].door < 0) return -1;
    const HDoor *d = &g->door[g->unit[u].door];
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        if (b->unit != u || b->storey != 1 || home_balcony_carved(b->kind) || b->kind == HBK_BALCONETTE || b->depth < 0.6f - HEPS) continue;
        if (fabsf(d->z - b->z) < HEPS && d->x - 0.5f * d->width >= b->x0 + 0.05f - HEPS && d->x + 0.5f * d->width <= b->x1 - 0.05f + HEPS) return i;
    }
    return -1;
}

static void drop_vol(HGraph *g, int i) {   /* an attached volume (nothing refers to them but by role) */
    for (int k = i; k + 1 < g->nvol; ++k) g->vol[k] = g->vol[k + 1];
    g->nvol--;
}

/* What stands in front of the wall under a balcony: a projecting one takes the place of a door canopy whose door it
 * covers (its slab is the canopy); a carved one has the ground floor's porch roofs flattened under its floor. */
static void clear_under(HGraph *g, const HBalcony *b, BLog *log) {
    const HVol *m = &g->vol[b->vol];
    const float floorY = home_balcony_floor(g, b);
    const int carved = home_balcony_carved(b->kind);
    const float under = floorY - (carved ? 0.0f : BALC_SLAB + 0.15f);
    for (int i = g->nvol - 1; i >= 0; --i) {
        HVol *v = &g->vol[i];
        if (v->unit != b->unit || v->role != HV_PORCH || home_porch_recessed(v->kind)) continue;
        if (v->x1 < b->x0 - 0.2f || v->x0 > b->x1 + 0.2f || v->z0 > m->z0 - 0.01f || v->top <= under) continue;
        if (!carved && v->kind == HP_CANOPY && b->kind != HBK_BALCONETTE && b->depth >= 0.6f) {
            const HDoor *d = g->unit[b->unit].door >= 0 ? &g->door[g->unit[b->unit].door] : NULL;
            if (d && d->x - 0.5f * d->width >= b->x0 + 0.05f && d->x + 0.5f * d->width <= b->x1 - 0.05f) {
                home_repair(g, log, "balcony", "unit %d: its balcony's slab is the door's canopy", b->unit);
                drop_vol(g, i);
                continue;
            }
        }
        if (carved) {
            /* a double veranda: the ground floor's porch roofed flat under the upper one's floor */
            const float e = fminf(v->eaves, floorY - 0.35f);
            if (e >= g->plinth + 2.2f) {
                home_repair(g, log, "balcony", "unit %d: its %s roofed flat under the %s", b->unit, home_porch_name(v->kind), home_balcony_name(b->kind));
                v->roof = HR_FLAT; v->parapet = 0; v->overhang = 0.0f; v->eaves = e;
                if (v->kind == HP_CANOPY) v->y0 = fminf(v->y0, e - 0.2f);
                home_vol_top(v);
            }
        }
    }
}

/* Adds a balcony if it keeps the rules (a projecting one drawn in to its reach first; what stands under it cleared if
 * `clear`), else logs the repair (a dry try, clear 0, logs nothing). */
static int try_add(HGraph *g, HBalcony b, int clear, BLog *log) {
    if (g->nbalc >= HG_MAX_BALC) return 0;
    if (!home_balcony_carved(b.kind)) {
        const float most = b.z - reach_min(g);
        if (b.depth > most) b.depth = most;
    }
    const char *f = balcony_fault(g, &b, g->nbalc);
    if (f && clear && strcmp(f, "under") == 0) {
        /* cleared on a copy first: kept only if the balcony then fits (a door must not lose its canopy for nothing) */
        HGraph *t = (HGraph *)malloc(sizeof(HGraph));
        if (!t) abort();
        *t = *g;
        clear_under(t, &b, NULL);
        if (!balcony_fault(t, &b, t->nbalc)) { clear_under(g, &b, log); f = balcony_fault(g, &b, g->nbalc); }
        free(t);
    }
    if (f) {
        if (clear) home_repair(g, log, "balcony", "unit %d storey %d: a %s %.1f m wide would break %s, left off", b.unit, b.storey,
                               home_balcony_name(b.kind), b.x1 - b.x0, f);
        return 0;
    }
    g->balc[g->nbalc++] = b;
    return 1;
}

/* A span of bays (first .. first + span - 1) as a balcony's x0..x1 */
static void span_x(HBalcony *b, float s0, float bw, int nb, int first, int span, float wf) {
    const float sa = s0, sb = s0 + bw * (float)nb;
    const float c0 = s0 + ((float)first + 0.5f) * bw, c1 = s0 + ((float)(first + span) - 0.5f) * bw;
    const float pad = b->kind == HBK_LOGGIA ? 0.35f : 0.3f;
    if (span == nb && span > 1) { b->x0 = sa + BALC_PARTY; b->x1 = sb - BALC_PARTY; }   /* wall to wall */
    else { b->x0 = fmaxf(sa + BALC_PARTY, c0 - 0.5f * wf - pad); b->x1 = fminf(sb - BALC_PARTY, c1 + 0.5f * wf + pad); }
}

void home_balconies(HGraph *g, int level, BLog *log) {
    g->nbalc = 0;
    if (g->empty || level >= 2 || g->style < 0 || g->style >= HS_COUNT) return;
    const BStyle *B = &BSTYLES[g->style];
    const int mi = main_vol(g);
    if (B->chance <= 0.0f || mi < 0) return;
    /* the Chinese courtyard rows (one storey, pitched) have none: the blocks behind them do */
    if (g->style == HS_CHINA && g->vol[mi].roof != HR_FLAT) return;
    Rng r = rng_make(hash_combine(hash_combine(g->seed, 0x45E2u), (uint32_t)level));
    const int rowHas = B->row && rng_float(&r) < B->chance;
    for (int u = 0; u < g->nunit; ++u) {
        if (B->row ? !rowHas || rng_float(&r) < 0.1f : rng_float(&r) >= B->chance) continue;
        float s0, bw;
        const int nb = home_front_bays(g, mi, u, &s0, &bw);
        if (nb <= 0) continue;
        HVol own;
        const HVol *m = home_unit_vol(g, mi, u, &own);
        HFinish fin;
        home_finish(g, u, &fin);
        const float ww = fminf(fin.winW, bw - 0.6f), wf = home_french_width(ww, bw);
        const float sa = s0, sb = s0 + bw * (float)nb;
        HBalcony b;
        memset(&b, 0, sizeof(b));
        b.unit = (int8_t)u; b.vol = (int8_t)mi; b.z = m->z0;
        b.kind = (uint8_t)(B->share2 > 0.0f && rng_float(&r) < B->share2 ? B->kind2 : B->kind);
        b.rail = (uint8_t)(rng_float(&r) < B->noRail ? HBL_NONE : B->rail);
        b.depth = rng_range(&r, B->depth0, B->depth1);
        b.consoles = (uint8_t)(b.kind == HBK_SLAB && rng_float(&r) < B->consoles);
        const int arched = rng_float(&r) < B->arch;
        const int top = B->hi ? (B->hi < m->storeys - 1 ? B->hi : m->storeys - 1) : m->storeys - 1;
        /* the bays across (the same on every storey it stacks to) */
        int span = B->span1 ? B->span0 + (int)(rng_float(&r) * (float)(B->span1 - B->span0 + 1)) : nb;
        if (span > nb) span = nb;
        if (span < 1) span = 1;
        int first = B->centred ? (nb - span + (rng_float(&r) < 0.5f ? 1 : 0)) / 2 : (int)(rng_float(&r) * (float)(nb - span + 1));
        int prev = 1;
        for (int k = B->lo; k <= top; ++k) {
            if (k > B->lo && ((!prev && !B->each) || rng_float(&r) >= B->stack)) break;
            b.storey = (uint8_t)k;
            int placed = 0;
            if (B->each) {
                const float pad = b.kind == HBK_BALCONETTE ? 0.12f : fminf(0.3f, 0.5f * (bw - wf) - 0.1f);
                for (int i = 0; i < nb; ++i) {
                    if (rng_float(&r) >= B->perBay) continue;
                    const float c = s0 + ((float)i + 0.5f) * bw;
                    b.x0 = c - 0.5f * wf - pad; b.x1 = c + 0.5f * wf + pad;
                    placed += try_add(g, b, 1, log);
                }
            } else if (b.kind == HBK_VERANDA) {
                b.x0 = sa + 0.4f; b.x1 = sb - 0.4f;
                placed += try_add(g, b, 1, log);
            } else {
                /* the chosen bays, clearing what stands under them; else the nearest bays that need nothing cleared (the
                 * first storey's choice holds for the storeys over it) */
                span_x(&b, s0, bw, nb, first, span, wf);
                b.arch = b.kind == HBK_LOGGIA && arched ? (uint8_t)(fminf(0.5f * (b.x1 - b.x0), 0.55f) * 100.0f + 0.5f) : 0;
                placed = try_add(g, b, 1, log);
                for (int dist = 1; !placed && k == B->lo && dist < nb; ++dist)
                    for (int sgn = -1; sgn <= 1 && !placed; sgn += 2) {
                        const int f2 = first + sgn * dist;
                        if (f2 < 0 || f2 + span > nb) continue;
                        HBalcony t = b;
                        span_x(&t, s0, bw, nb, f2, span, wf);
                        t.arch = b.arch ? (uint8_t)(fminf(0.5f * (t.x1 - t.x0), 0.55f) * 100.0f + 0.5f) : 0;
                        if (try_add(g, t, 0, NULL)) { placed = 1; first = f2; b = t; }
                    }
            }
            prev = placed > 0;
        }
    }
    if (g->nbalc) blog_printf(log, "  balconies: %d (%s)\n", g->nbalc, home_balcony_name(g->balc[0].kind));
}

/* ---------------------------------------------------------------------------
 * The dressing (near: home_character.c's dress_pass, before the retrofit layer)
 * ------------------------------------------------------------------------- */

static void push_at(DC *d, double x, double y, double z, double dx, double dz) {
    const double p[3] = { x, y, z }, dir[3] = { dx, 0.0, dz };
    hc_push_along(d, p, dir);
}

/* A railing of kind rail from (x, z) along (dx, dz), len long, standing on y. */
static void rail_run(DC *d, int rail, double x, double y, double z, double dx, double dz, double len, int endPost) {
    if (len < 0.15) return;
    push_at(d, x, y, z, dx, dz);
    switch (rail) {
    case HBL_IRON: case HBL_ORNATE: hk_rail(d->pm, rail == HBL_ORNATE, (float)len, endPost); break;
    case HBL_STEEL: hr_rail(d->pm, (float)len); break;
    case HBL_BALUSTRADE: hk_balustrade(d->pm, (float)len); break;
    default: break;
    }
    hc_pop(d);
}

/* What the region's owners put out on their balconies: pots (the chance), plants over the rail, an LED strip under it. */
static void region_plants(int region, float *pots, float *trail, float *led) {
    switch (region) {
    case PLAN_REGION_SEU: case PLAN_REGION_LAT: *pots = 0.6f; *trail = 0.45f; *led = 0.15f; break;
    case PLAN_REGION_MENA: case PLAN_REGION_SAS: case PLAN_REGION_SEA: *pots = 0.55f; *trail = 0.35f; *led = 0.3f; break;
    case PLAN_REGION_EAS: *pots = 0.45f; *trail = 0.15f; *led = 0.4f; break;
    default: *pots = 0.3f; *trail = 0.15f; *led = 0.05f; break;
    }
}

void hs_balconies(DC *d) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    float pots, trail, led;
    region_plants(g->region, &pots, &trail, &led);
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        const int u = b->unit;
        const double dy = C->dy ? C->dy[u] : 0.0, y = home_balcony_floor(g, b) + dy, zf = b->z - b->depth;
        const uint32_t h = hash_combine(g->unit[u].seed, 0xBA1C0u + (uint32_t)i * 7919u);
        const HCharSet *cs = &C->cs[u];
        double ytop = y + HK_RAIL_H + 0.1;
        hc_begin(d, u);
        switch (b->kind) {
        case HBK_BALCONETTE: case HBK_SLAB: {
            const double fz = zf + (b->kind == HBK_BALCONETTE ? 0.035 : 0.05), inset = 0.03;
            rail_run(d, b->rail, b->x0 + inset, y, fz, 1.0, 0.0, b->x1 - b->x0 - 2.0 * inset, 1);
            /* its ends back to the wall, stopping short of the front's posts */
            const double side = (b->z - fz) - (b->rail == HBL_STEEL ? 0.1 : 0.04);
            rail_run(d, b->rail, b->x0 + inset, y, b->z, 0.0, -1.0, side, 0);
            rail_run(d, b->rail, b->x1 - inset, y, b->z, 0.0, -1.0, side, 0);
            if (b->consoles) {
                const double reach = fmin(b->depth - 0.05, 0.45) / 0.45;
                for (int k = 0; k < 2; ++k) {
                    const double p[3] = { k ? b->x1 - 0.18 : b->x0 + 0.18, y - BALC_SLAB, b->z };
                    hc_push_wall(d, p, 0.0, -1.0);
                    pm_scale(d->pm, 1.0f, 1.0f, (float)reach);
                    pm_place(d->pm, PMK_H_CONSOLE);
                    hc_pop(d);
                }
            }
            break;
        }
        case HBK_GLAZED: {
            HVol own;
            const HVol *m = home_unit_vol(g, b->vol, u, &own);
            ytop = fmin(y + g->storeyH, m->eaves + dy - 0.02) - BALC_SLAB;
            const double y0 = y + 0.95, hh = ytop - y0;
            if (hh < 0.3) break;
            const double len = b->x1 - b->x0 - 0.12;
            push_at(d, b->x0 + 0.06, y0, zf + 0.06, 1.0, 0.0);
            hk_glazing(d->pm, (float)len, (float)hh);
            pm_push(d->pm); pm_translate(d->pm, (float)len, 0.0f, 0.0f); pm_scale(d->pm, 0.5f, (float)hh, 0.5f); pm_place(d->pm, PMK_R_FRAME_POST); pm_pop(d->pm);
            hc_pop(d);
            for (int k = 0; k < 2; ++k) {
                push_at(d, k ? b->x1 - 0.06 : b->x0 + 0.06, y0, b->z, 0.0, -1.0);
                hk_glazing(d->pm, (float)(b->depth - 0.12), (float)hh);
                hc_pop(d);
            }
            break;
        }
        case HBK_LOGGIA:
            rail_run(d, b->rail, b->x0 + 0.02, y, b->z + 0.1, 1.0, 0.0, b->x1 - b->x0 - 0.04, 1);
            break;
        case HBK_VERANDA: {
            float w;
            const int n = home_veranda_bays(b, &w);
            for (int k = 0; k < n; ++k)
                rail_run(d, b->rail, b->x0 + BALC_PIER + k * (w + BALC_PIER) + 0.01, y, b->z + 0.15, 1.0, 0.0, w - 0.02, 0);
            break;
        }
        default: break;
        }
        /* the owners' (not on a balconette's ledge; fewer where the house is run down) */
        if (b->kind != HBK_BALCONETTE && !cs->lean) {
            const float keep = cs->upkeep == HK_RUNDOWN ? 0.5f : 1.0f;
            const double inner = b->kind == HBK_LOGGIA || b->kind == HBK_VERANDA ? b->z + 0.35 : zf + 0.25;
            if (b->depth >= 0.6 && ch(h, 1u, pots * keep)) {
                const int two = ch(h, 2u, 0.4f) && b->x1 - b->x0 > 2.0;
                for (int k = 0; k <= two; ++k) {
                    const double p[3] = { k ? b->x1 - 0.3 : b->x0 + 0.3, y, inner };
                    hc_push_wall(d, p, 0.0, -1.0);
                    pm_scale(d->pm, 0.8f, 0.8f, 0.8f);
                    pm_place(d->pm, ch(h, 3u + (uint32_t)k, 0.3f) ? PMK_R_POT_PALM : PMK_R_POT);
                    hc_pop(d);
                }
            }
            if (!home_balcony_carved(b->kind) && b->kind != HBK_GLAZED && b->rail != HBL_NONE && ch(h, 5u, trail * keep)) {
                /* plants over the front rail: on the wall's frame (its x runs the kit's way along the front) */
                const double len = fmin(b->x1 - b->x0 - 0.3, 0.6 + 2.0 * hash_float(hash_combine(h, 6u)));
                const double x = d->sx > 0.0 ? b->x1 - 0.15 : b->x0 + 0.15;
                const double p[3] = { x, y + HK_RAIL_H, zf + 0.02 };
                hc_push_wall(d, p, 0.0, -1.0);
                hr_trail(d->pm, (float)len, 0.5f + 0.5f * hash_float(hash_combine(h, 7u)), home_trail_species(g->style, h));
                hc_pop(d);
            }
            if (!home_balcony_carved(b->kind) && ch(h, 8u, led * (0.5f + g->affluence))) {
                push_at(d, b->x0 + 0.1, y - BALC_SLAB, zf + 0.08, 1.0, 0.0);
                hr_led(d->pm, (float)(b->x1 - b->x0 - 0.2));
                hc_pop(d);
            }
        }
        hc_done(d);
        d->D->st.balconies++;
        /* the retrofit layer keeps out of it */
        const double lo[3] = { b->x0 - 0.1, y - BALC_SLAB - 0.35, (home_balcony_carved(b->kind) ? b->z : zf) - 0.35 };
        const double hi[3] = { b->x1 + 0.1, home_balcony_carved(b->kind) ? y + 2.6 : ytop + 0.25, b->z + (home_balcony_carved(b->kind) ? b->depth : 0.05) };
        hc_occupy(d, lo, hi);
    }
}
