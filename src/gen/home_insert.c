#include "home_char_internal.h"
#include "pmodel_retrofit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * Retrofit insertions (residential roadmap HM5c2, ~/.claude/plans/residential-buildings.md "HM5c2"; the user's concept
 * sheets in docs/reference/international-residential, panels 2-6 of each: docs/residential-target.md "Retrofit
 * insertions").
 *
 *   the draw      home_inserts: three units in five are retrofitted (the user, 2026-11-20), each owner drawing one kind or
 *                 two from the style's table (the sheets' panels, the plan's survey), recorded built or not in
 *                 HGraph.insPlan (the user's choice, 2026-11-23: the final table from the start; a kind a later step
 *                 builds leaves its unit plain until then). Every draw is a hash of the unit's own seed, so a step that
 *                 adds builders moves nothing an earlier one placed.
 *   the graph     the insertions this step builds (HM5c2a: boxes and rounded bays), each across whole bays of its unit's
 *                 front (home_front_bays) so the windows beside it keep their places; what stands in the way is taken in
 *                 or tried elsewhere; home_check_inserts: their rules
 *   the shell     home_shell_ins.c: their solids and their own openings, joined to the house by its CSG
 *   the dressing  hs_inserts: lights under a hung one, a screen box's breeze blocks, a cage's bars, plants on top
 *
 * The rules (section 13.5's spirit, as the balconies'): on its own unit's slice of the main's street front, 0.3 m off a
 * party wall or a corner; hung from a storey its unit has, its top under the eaves' gutter (at the deck of a flat roof);
 * out over its own plot (0.3 m inside the boundary) or at most 0.8 m over a street-line front's pavement; on the ground only
 * where its unit's bay stood, in the front yard's projection margin, off the door, the path, the drive and the cars;
 * nothing rising under it in front of the wall (a flat porch roof may carry it); no balcony on its storeys (they are taken in),
 * its top under one over it, no glazed one under it; the unit's door still sheltered; a unit's two insertions apart.
 * ========================================================================= */

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }

const char *home_insert_name(int k) {
    static const char *const N[HI_COUNT] = { "box", "round", "frame", "hex", "fins", "screen", "stairs", "tower", "unfinished", "roofroom" };
    return k >= 0 && k < HI_COUNT ? N[k] : "?";
}
const char *home_insert_face_name(int f) {
    static const char *const N[HIF_COUNT] = { "concrete", "glass", "screen", "container" };
    return f >= 0 && f < HIF_COUNT ? N[f] : "?";
}

/* ---------------------------------------------------------------------------
 * The table: each sheet's panels 2-6 (the plan's "HM5c2" survey)
 * ------------------------------------------------------------------------- */

typedef struct IStyle {
    float kind[HI_COUNT];       /* box, round, frame, hex, fins, screen, stairs, tower, unfinished, roofroom */
    float face[HIF_COUNT];      /* a box's make: concrete, glass, screen, container */
    float cage;                 /* a glass box barred over */
} IStyle;
#define KINDS(box, round, frame, hex, fins, screen, stairs, tower, unf, roof) { box, round, frame, hex, fins, screen, stairs, tower, unf, roof }

static const IStyle IST[HS_COUNT] = {
    /* 2 a re-front with screen strips; 3 a rounded bay over two storeys; 4, 5 steel-and-glass boxes */
    [HS_VICTORIAN]  = { KINDS(0.45f, 0.25f, 0.0f,  0.0f,  0.05f, 0.25f, 0.0f,  0.0f,  0.0f,  0.0f),  { 0.25f, 0.6f, 0.15f, 0.0f } },
    /* 2 a steel-and-glass box over two storeys; 3 fins; 4 a roof pavilion; 5 a stair tower; 6 hexagons */
    [HS_GEORGIAN]   = { KINDS(0.25f, 0.0f,  0.0f,  0.15f, 0.2f,  0.1f,  0.0f,  0.15f, 0.0f,  0.15f), { 0.15f, 0.85f, 0.0f, 0.0f } },
    /* 2, 6 the bays rebuilt rounded; 3 screens over them; 4 steel-and-glass boxes for them; 5 faceted aprons */
    [HS_INTERWAR]   = { KINDS(0.25f, 0.4f,  0.15f, 0.0f,  0.0f,  0.2f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.3f, 0.7f, 0.0f, 0.0f } },
    /* 2 first-floor boxes; 5 angled frames round the doors and bays */
    [HS_POSTWAR]    = { KINDS(0.5f,  0.0f,  0.35f, 0.0f,  0.0f,  0.15f, 0.0f,  0.0f,  0.0f,  0.0f),  { 0.6f, 0.25f, 0.15f, 0.0f } },
    /* 2 a glass box three storeys; 3 zigzag frames; 4 a rounded bay the full height; 5 strip and box; 6 a service tower */
    [HS_ROWHOUSE]   = { KINDS(0.35f, 0.2f,  0.2f,  0.0f,  0.0f,  0.1f,  0.0f,  0.15f, 0.0f,  0.0f),  { 0.25f, 0.75f, 0.0f, 0.0f } },
    /* 2 concrete boxes with screens; 3 a rounded balcony; 4 a pergola room; 5 an inclined frame; 6 a round tower */
    [HS_SEU_VILLA]  = { KINDS(0.3f,  0.25f, 0.15f, 0.0f,  0.0f,  0.0f,  0.1f,  0.0f,  0.0f,  0.2f),  { 0.4f, 0.1f, 0.5f, 0.0f } },
    /* 1, 3 screen boxes; 2, 4 concrete ones; 5 faceted panels; 6 a round tower */
    [HS_TOWNHOUSE]  = { KINDS(0.6f,  0.15f, 0.0f,  0.0f,  0.15f, 0.1f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.45f, 0.1f, 0.45f, 0.0f } },
    /* 2 a concrete grid; 3 rusted steel boxes; 4 hexagon cladding and a box; 5 a louvred crown; 6 stacked boxes */
    [HS_JAPAN]      = { KINDS(0.45f, 0.0f,  0.0f,  0.2f,  0.35f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.5f, 0.1f, 0.0f, 0.4f } },
    /* 2 cages; 3, 5, 6 boxes (a screen on some); 4 a screen strip */
    [HS_CHINA]      = { KINDS(0.7f,  0.0f,  0.1f,  0.0f,  0.0f,  0.2f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.4f, 0.3f, 0.3f, 0.0f }, 0.8f },
    /* 2, 5 boxes and screens; 3, 6 rounded; 2, 6 stairs; 4 a roof room */
    [HS_KOREA]      = { KINDS(0.35f, 0.25f, 0.0f,  0.0f,  0.0f,  0.1f,  0.2f,  0.0f,  0.0f,  0.1f),  { 0.5f, 0.1f, 0.4f, 0.0f } },
    /* 2 a box behind louvres; 3 a rounded corner; 4 a cage; 6 a box and stairs; screen strips */
    [HS_SHOPHOUSE]  = { KINDS(0.45f, 0.2f,  0.0f,  0.0f,  0.0f,  0.2f,  0.15f, 0.0f,  0.0f,  0.0f),  { 0.3f, 0.3f, 0.4f, 0.0f }, 0.8f },
    /* 2, 4 screen boxes; 3 a portal frame; 5 a rounded canopy; 6 a cage */
    [HS_LINKHOUSE]  = { KINDS(0.55f, 0.2f,  0.25f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.15f, 0.25f, 0.6f, 0.0f }, 0.8f },
    /* 1 starter bars; 2 a screen box; 3 stairs and strips; 5 cages; 6 a rounded bay and a screen box */
    [HS_RCC]        = { KINDS(0.4f,  0.15f, 0.0f,  0.0f,  0.0f,  0.1f,  0.2f,  0.0f,  0.15f, 0.0f),  { 0.2f, 0.35f, 0.45f, 0.0f }, 0.8f },
    /* 2 screens; 3 an angled car porch frame; 4 a container on the roof; 6 fins */
    [HS_BUNGALOW]   = { KINDS(0.0f,  0.0f,  0.3f,  0.0f,  0.2f,  0.25f, 0.0f,  0.0f,  0.0f,  0.25f), { 1.0f, 0.0f, 0.0f, 0.0f } },
    /* 2 a mashrabiya box; 3 rounded; 4 a timber oriel; 5 an iwan frame; 6 an angled frame and a screen */
    [HS_MENA_VILLA] = { KINDS(0.45f, 0.15f, 0.25f, 0.0f,  0.0f,  0.15f, 0.0f,  0.0f,  0.0f,  0.0f),  { 0.3f, 0.0f, 0.7f, 0.0f } },
    /* 2 a mashrabiya oriel; 3 a screen box; 5 roof terraces; 6 a frame and a screen box */
    [HS_COURTYARD]  = { KINDS(0.6f,  0.0f,  0.25f, 0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.15f), { 0.2f, 0.0f, 0.8f, 0.0f } },
    /* 2 an unfinished storey on starter bars; 4 a block parapet */
    [HS_BLOCK]      = { KINDS(0.0f,  0.0f,  0.0f,  0.0f,  0.0f,  0.2f,  0.0f,  0.0f,  0.8f,  0.0f),  { 1.0f, 0.0f, 0.0f, 0.0f } },
    /* 2 an angled screen box; 3 a rounded upper storey; 5 frames; 6 screens */
    [HS_WALLED]     = { KINDS(0.35f, 0.25f, 0.2f,  0.0f,  0.1f,  0.1f,  0.0f,  0.0f,  0.0f,  0.0f),  { 0.4f, 0.0f, 0.6f, 0.0f } },
    /* 1 stairs; 2 screen box and box; 3, 6 frames; 4 rounded with fins; 5 a container and a steel stair */
    [HS_SELFBUILT]  = { KINDS(0.3f,  0.1f,  0.25f, 0.0f,  0.0f,  0.0f,  0.2f,  0.0f,  0.05f, 0.1f),  { 0.4f, 0.0f, 0.35f, 0.25f } },
    /* 2 a strip; 3 a stair strut; 4 a screen room on the roof; 6 a box and a frame */
    [HS_COLONIAL]   = { KINDS(0.25f, 0.0f,  0.25f, 0.0f,  0.0f,  0.2f,  0.15f, 0.0f,  0.0f,  0.15f), { 0.5f, 0.0f, 0.5f, 0.0f } },
};
#undef KINDS

/* ---------------------------------------------------------------------------
 * Heights
 * ------------------------------------------------------------------------- */

static int main_vol(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

/* The highest an insertion's top may reach on main m's street front: a flat roof's deck, a parapet's or a gable end's wall
 * plate; else under the eaves' overhang, its slab and the gutter hung from it. */
static float ins_ceiling(const HVol *m) {
    if (m->roof == HR_FLAT || m->parapet == 1 || (m->roof == HR_GABLE && !m->ridgeX)) return m->eaves;
    return m->eaves - (m->overhang * tanf(m->pitch * 0.017453293f) + 0.2f + 0.3f);
}

void home_insert_span(const HGraph *g, const HInsert *in, float *y0, float *y1) {
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    const float P = g->plinth, H = g->storeyH;
    *y0 = in->s0 == 0 ? 0.0f : P + (float)in->s0 * H - INS_SLAB;
    *y1 = fminf(P + (float)(in->s1 + 1) * H - 0.1f, ins_ceiling(m));
    /* a hung one resting on a flat porch roof under it (a double veranda's, flattened under the floor it took in) */
    for (int i = 0; i < g->nvol && in->s0 > 0; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role != HV_PORCH || v->unit != in->unit || home_porch_recessed(v->kind) || v->roof != HR_FLAT || v->z0 > m->z0 - 0.01f) continue;
        if (v->x1 > in->x0 && v->x0 < in->x1 && v->top > *y0 - 0.15f && v->top < *y0 + 0.3f) *y0 = fmaxf(*y0, v->top);
    }
    /* under a balcony over it: its slab, and its consoles (Southern Europe's) */
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *b = &g->balc[i];
        if (b->unit != in->unit || b->storey != in->s1 + 1 || home_balcony_carved(b->kind) || b->x1 < in->x0 - 0.2f || b->x0 > in->x1 + 0.2f) continue;
        *y1 = fminf(*y1, home_balcony_floor(g, b) - BALC_SLAB - (b->consoles ? 0.4f : 0.06f));
    }
}

int home_insert_window(const HGraph *g, const HInsert *in, int k, float *sill, float *head) {
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    const float f = g->plinth + (float)k * g->storeyH;
    const int round = in->kind == HI_ROUND;
    float s = f + (k == 0 ? (round ? 0.8f : 0.75f) : (round ? 0.9f : 0.85f));
    const float h = fminf(f + (round ? 2.1f : 2.15f), y1 - 0.3f);
    if (k == in->s0 && in->s0 > 0) s = fmaxf(s, y0 + 0.5f);
    if (h - s < 0.7f) return 0;
    *sill = s; *head = h;
    return 1;
}

/* ---------------------------------------------------------------------------
 * The rules
 * ------------------------------------------------------------------------- */

static int unit_slice(const HGraph *g, int mi, int u, float *a, float *b) {
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return 0;
    *a = s0; *b = s0 + bw * (float)nb;
    return nb;
}

/* the nearest the street a hung one's front comes (z) */
static float reach_min(const HGraph *g) { return home_style_street(g->style) ? -INS_STREET : 0.3f; }

static int overlap(float a0, float a1, float b0, float b1, float pad) { return a1 > b0 - pad && a0 < b1 + pad; }

/* The door's shelter: a porch over it, the balcony over it, or a hung insertion (the unit's door rule, home_validate's) */
int home_door_insert(const HGraph *g, int u) {
    if (u < 0 || u >= g->nunit || g->unit[u].door < 0) return -1;
    const HDoor *d = &g->door[g->unit[u].door];
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (in->unit != u || in->s0 != 1 || in->depth < 0.6f - HEPS || fabsf(d->z - in->z) > HEPS) continue;
        if (d->x - 0.5f * d->width >= in->x0 + 0.05f - HEPS && d->x + 0.5f * d->width <= in->x1 - 0.05f + HEPS) return i;
    }
    return -1;
}
static int door_sheltered(const HGraph *g, int u) {
    if (u < 0 || u >= g->nunit || g->unit[u].door < 0) return 1;
    const HDoor *d = &g->door[g->unit[u].door];
    const float d0 = d->x - 0.5f * d->width, d1 = d->x + 0.5f * d->width;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *p = &g->vol[i];
        if (p->role == HV_PORCH && p->unit == u && p->x0 <= d0 + HEPS && p->x1 >= d1 - HEPS && fabsf(p->z1 - d->z) < HEPS) return 1;
    }
    return home_door_balcony(g, u) >= 0 || home_door_insert(g, u) >= 0;
}

/* What breaks a rule, or NULL: in checked against the graph and the insertions before it (n of them). */
static const char *ins_fault(const HGraph *g, const HInsert *in, int n) {
    const int mi = in->vol, u = in->unit;
    if (u < 0 || u >= g->nunit) return "unit";
    if (mi < 0 || mi >= g->nvol || g->vol[mi].role > HV_ROW) return "main";
    if (!home_insert_built(in->kind)) return "kind";
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    if (in->s1 < in->s0 || in->s1 >= m->storeys) return "storey";
    if (fabsf(in->z - m->z0) > 0.01f) return "wall";
    float a, b;
    if (!unit_slice(g, mi, u, &a, &b)) return "slice";
    if (in->x0 < a + INS_PARTY - HEPS || in->x1 > b - INS_PARTY + HEPS || in->x1 - in->x0 < 1.2f - HEPS) return "party";
    if (in->depth < 0.5f - HEPS) return "shallow";
    const float zf = in->z - in->depth;
    const int ground = in->s0 == 0;
    if (ground ? home_style_street(g->style) || zf < home_yard(g->region)->proj - HEPS : zf < reach_min(g) - HEPS) return "reach";
    if (in->kind == HI_ROUND && (in->r > in->depth + HEPS || in->r > 0.5f * (in->x1 - in->x0) + HEPS || in->r < 0.2f)) return "radius";
    float y0, y1, sl, hd;
    home_insert_span(g, in, &y0, &y1);
    if (y1 - y0 < 1.6f || !home_insert_window(g, in, in->s1, &sl, &hd)) return "eaves";
    /* nothing in front of the wall rising under it (a grounded one: nothing where it stands) */
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind))) continue;
        if (v->z0 > m->z0 - 0.01f || (v->unit != u && v->unit != HG_ROW_WIDE) || !overlap(v->x0, v->x1, in->x0, in->x1, ground ? 0.05f : 0.2f)) continue;
        if (ground) { if (v->y0 < 0.01f) return "overlap"; continue; }   /* a canopy beside it: the kit's keeps off its solids */
        if (v->top > y0 - 0.15f && !(v->roof == HR_FLAT && v->top <= y0 + 0.01f)) return "under";   /* (or it rests on its flat roof) */
    }
    /* balconies: none on its storeys (they are taken in), no glazed one's cap under it (one over it caps its top: the span) */
    for (int i = 0; i < g->nbalc; ++i) {
        const HBalcony *bc = &g->balc[i];
        if (bc->unit != u || !overlap(bc->x0, bc->x1, in->x0, in->x1, 0.2f)) continue;
        if ((bc->storey >= in->s0 && bc->storey <= in->s1) || (bc->kind == HBK_GLAZED && bc->storey + 1 == in->s0)) return "balcony";
    }
    if (ground) {
        const HUnit *un = &g->unit[u];
        if (un->door >= 0) {
            const HDoor *d = &g->door[un->door];
            if (overlap(d->x - 0.5f * d->width, d->x + 0.5f * d->width, in->x0, in->x1, 0.05f)) return "door";
        }
        for (int i = 0; i < g->nsite; ++i) {
            const HSite *s = &g->site[i];
            if (s->kind == HE_FRONT || s->kind == HE_REAR) continue;
            if (overlap(s->x0, s->x1, in->x0, in->x1, -HEPS) && overlap(s->z0, s->z1, zf, in->z, -HEPS)) return "garden clear";
        }
        for (int i = 0; i < g->nbound; ++i) {
            const HBound *bd = &g->bound[i];
            if (!home_bound_drawn(g, bd)) continue;
            float bx0, bz0, bx1, bz1;
            home_bound_rect(g, bd, &bx0, &bz0, &bx1, &bz1);
            if (overlap(bx0, bx1, in->x0, in->x1, -HEPS) && overlap(bz0, bz1, zf, in->z, -HEPS)) return "boundary";
        }
    }
    for (int i = 0; i < n; ++i) {
        const HInsert *o = &g->ins[i];
        if (o == in || o->unit != u) continue;
        if (overlap(o->x0, o->x1, in->x0, in->x1, 0.3f) && o->s0 <= in->s1 + 1 && in->s0 <= o->s1 + 1) return "apart";
    }
    if (!door_sheltered(g, u)) return "door shelter";
    return NULL;
}

int home_check_inserts(const HGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nins; ++i) {
        const char *f = ins_fault(g, &g->ins[i], i);
        if (!f) continue;
        blog_printf(log, "  rejected insert: %s %d (unit %d, storeys %d-%d) breaks %s\n", home_insert_name(g->ins[i].kind), i,
                    g->ins[i].unit, g->ins[i].s0, g->ins[i].s1, f);
        ok = 0;
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * The draw and the placing
 * ------------------------------------------------------------------------- */

void home_drop_vol(HGraph *g, int i) {
    if (i < 0 || i >= g->nvol) return;
    for (int k = i; k + 1 < g->nvol; ++k) g->vol[k] = g->vol[k + 1];
    g->nvol--;
#define FIX(x) do { if ((x) > i) (x)--; else if ((x) == i) (x) = -1; } while (0)
    for (int k = 0; k < g->nvol; ++k) FIX(g->vol[k].parent);
    for (int k = 0; k < g->ndoor; ++k) FIX(g->door[k].vol);
    for (int k = 0; k < g->nchim; ++k) FIX(g->chim[k].vol);
    for (int k = 0; k < g->nparty; ++k) FIX(g->party[k].vol);
    for (int k = 0; k < g->nbalc; ++k) FIX(g->balc[k].vol);
    for (int k = 0; k < g->nins; ++k) FIX(g->ins[k].vol);
#undef FIX
}

/* the unit's bay on the main's street front, or -1 */
static int unit_bay(const HGraph *g, int u, const HVol *m) {
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role == HV_BAY && v->unit == u && fabsf(v->z1 - m->z0) < 0.02f) return i;
    }
    return -1;
}

/* Whether a unit's body can take a kind at all (fixed for every kind at HM5c2a, so the draw is the final table's: a later
 * step only builds what was drawn). */
static int feasible(const HGraph *g, int kind, const HVol *m, int bay, float yard) {
    const int st = m->storeys;
    switch (kind) {
    case HI_BOX: case HI_ROUND: return st >= 2 || bay >= 0;
    case HI_HEX: case HI_STAIRS: return st >= 2;
    case HI_FRAME: case HI_FINS: case HI_SCREEN: return 1;
    case HI_TOWER: return st >= 2 && yard >= 1.8f && !home_style_street(g->style);
    case HI_UNFINISHED: return m->roof == HR_FLAT && st < HOME_STOREYS_MAX;
    case HI_ROOFROOM: return m->roof == HR_FLAT;
    default: return 0;
    }
}

static int pick_w(const float *w, int n, float r) {
    float t = 0.0f;
    for (int k = 0; k < n; ++k) t += w[k];
    if (t <= 0.0f) return -1;
    r *= t;
    for (int k = 0; k < n; ++k) {
        if (w[k] <= 0.0f) continue;
        if (r < w[k]) return k;
        r -= w[k];
    }
    for (int k = n - 1; k >= 0; --k) if (w[k] > 0.0f) return k;
    return -1;
}

/* every unit's owner's draw: retrofitted three times in five, one kind or (three in ten) two */
static void draw_plans(HGraph *g, int mi) {
    memset(g->insPlan, 0, sizeof(g->insPlan));
    if (!home_retrofit_style(g->style)) return;
    const IStyle *S = &IST[g->style];
    for (int u = 0; u < g->nunit; ++u) {
        const uint32_t h = hash_combine(g->unit[u].seed, 0x1C2A0u);
        if (hf(h, 1u) >= 0.6f) continue;
        HVol own;
        const HVol *m = home_unit_vol(g, mi, u, &own);
        const int bay = unit_bay(g, u, m);
        float w[HI_COUNT];
        for (int k = 0; k < HI_COUNT; ++k) w[k] = feasible(g, k, m, bay, m->z0) ? S->kind[k] : 0.0f;
        const int k1 = pick_w(w, HI_COUNT, hf(h, 2u));
        if (k1 < 0) continue;
        uint16_t plan = (uint16_t)(1u << k1);
        if (hf(h, 3u) < 0.3f) {
            w[k1] = 0.0f;
            const int k2 = pick_w(w, HI_COUNT, hf(h, 4u));
            if (k2 >= 0) plan |= (uint16_t)(1u << k2);
        }
        g->insPlan[u] = plan;
    }
}

/* What stands in the way of insertion in, cleared: the bay it takes the place of; a door canopy under a hung one at the
 * first floor that covers the door (its soffit shelters it); the unit's balconies on its storeys (taken in). */
static int clear_for(HGraph *g, const HInsert *in, int bay, BLog *log) {
    int took = 0;
    if (bay >= 0) {
        home_repair(g, log, "insert", "unit %d: its bay rebuilt as a %s %s", in->unit, home_insert_face_name(in->face), home_insert_name(in->kind));
        home_drop_vol(g, bay);
    }
    /* a tree's place (HM7 decides which hold one) where a grounded one now stands */
    for (int i = g->nsite - 1; i >= 0 && in->s0 == 0; --i) {
        const HSite *t = &g->site[i];
        if (t->kind != HE_TREE || !overlap(t->x0, t->x1, in->x0, in->x1, 0.0f) || !overlap(t->z0, t->z1, in->z - in->depth, in->z, 0.0f)) continue;
        home_repair(g, log, "insert", "unit %d: a tree's place under its %s given up", in->unit, home_insert_name(in->kind));
        for (int k = i; k + 1 < g->nsite; ++k) g->site[k] = g->site[k + 1];
        g->nsite--;
        for (int k = 0; k < g->ndoor; ++k) if (g->door[k].path > i) g->door[k].path--;
    }
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    const HDoor *d = g->unit[in->unit].door >= 0 ? &g->door[g->unit[in->unit].door] : NULL;
    const int covers = d && in->s0 == 1 && in->depth >= 0.6f && fabsf(d->z - in->z) < HEPS &&
                       d->x - 0.5f * d->width >= in->x0 + 0.05f && d->x + 0.5f * d->width <= in->x1 - 0.05f;
    for (int i = g->nvol - 1; i >= 0 && in->s0 > 0; --i) {
        const HVol *v = &g->vol[i];
        if (v->role != HV_PORCH || v->kind != HP_CANOPY || v->unit != in->unit || !covers) continue;
        if (!overlap(v->x0, v->x1, in->x0, in->x1, 0.2f) || v->top <= y0 - 0.15f) continue;
        home_repair(g, log, "insert", "unit %d: the %s over its door is its canopy", in->unit, home_insert_name(in->kind));
        home_drop_vol(g, i);
    }
    for (int i = g->nbalc - 1; i >= 0; --i) {
        const HBalcony *bc = &g->balc[i];
        if (bc->unit != in->unit || bc->storey < in->s0 || bc->storey > in->s1 || !overlap(bc->x0, bc->x1, in->x0, in->x1, 0.2f)) continue;
        home_repair(g, log, "insert", "unit %d storey %d: its %s taken into the %s", in->unit, bc->storey, home_balcony_name(bc->kind), home_insert_name(in->kind));
        for (int k = i; k + 1 < g->nbalc; ++k) g->balc[k] = g->balc[k + 1];
        g->nbalc--;
        took++;
    }
    return took;
}

/* Adds insertion in (taking bay's place if bay >= 0) if, with what stands in its way cleared, it keeps the rules; a dry try
 * (log NULL) logs nothing. */
/* why: the first candidate's fault is kept there, for the repair that leaves one off */
static int try_add(HGraph *g, HInsert in, int bay, BLog *log, const char **why) {
    if (g->nins >= HG_MAX_INS) return 0;
    HGraph *t = (HGraph *)malloc(sizeof(HGraph));
    if (!t) abort();
    *t = *g;
    clear_for(t, &in, bay, NULL);
    in.vol = (int8_t)main_vol(t);
    t->ins[t->nins++] = in;
    const char *f = ins_fault(t, &t->ins[t->nins - 1], t->nins - 1);
    free(t);
    if (f) { if (!*why) *why = f; return 0; }
    in.took = (uint8_t)clear_for(g, &in, bay, log);
    g->ins[g->nins++] = in;
    return 1;
}

static void span_x(HInsert *in, float s0, float bw, int first, int span, float a, float b) {
    in->x0 = fmaxf(a + INS_PARTY, s0 + (float)first * bw + 0.2f);
    in->x1 = fminf(b - INS_PARTY, s0 + (float)(first + span) * bw - 0.2f);
}

/* A unit's box or rounded bay: in place of its bay (on the ground) where it has one, else hung across a bay or two of its
 * front from the first floor (the second, a time in four, on a taller house); the bays nearest the drawn ones and fewer
 * storeys tried before it is left off. */
static void place(HGraph *g, int mi, int u, int kind, BLog *log) {
    const IStyle *S = &IST[g->style];
    Rng r = rng_make(hash_combine(g->unit[u].seed, 0x1C2B0u + (uint32_t)kind));
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return;
    const float a = s0, b = s0 + bw * (float)nb;
    HFinish fin;
    home_finish(g, u, &fin);
    HInsert in;
    memset(&in, 0, sizeof(in));
    const char *why = NULL;
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = (uint8_t)kind; in.z = m->z0;
    /* the make */
    const int face = kind == HI_BOX ? pick_w(S->face, HIF_COUNT, rng_float(&r)) : HIF_CONCRETE;
    in.face = (uint8_t)(face < 0 ? HIF_CONCRETE : face);
    in.cage = (uint8_t)(in.face == HIF_GLASS && rng_float(&r) < S->cage);
    in.screen = (uint8_t)(rng_float(&r) < 0.35f);
    static const uint8_t STEEL[4] = { 2, 4, 4, 5 };   /* black, grey, bronze (house_palette.glsl H_TRIM) */
    switch (in.face) {
    case HIF_GLASS: in.fin = HW_CONCRETE; in.col = STEEL[(int)(rng_float(&r) * 4.0f) & 3]; break;
    case HIF_CONTAINER: in.fin = HW_BLOCK; in.col = (uint8_t)((int)(rng_float(&r) * 6.0f) % 6); break;
    default:
        /* concrete, or (a rounded bay half the time) the house's own render repainted with it */
        if (kind == HI_ROUND && fin.wall == HW_RENDER && rng_float(&r) < 0.5f) { in.fin = HW_RENDER; in.col = fin.wallCol; }
        else { in.fin = HW_CONCRETE; in.col = (uint8_t)((int)(rng_float(&r) * 8.0f) & 7); }
        break;
    }
    const float depthR = rng_float(&r), roundR = rng_range(&r, 0.75f, 1.0f), upR = rng_float(&r), tallR = rng_float(&r);
    int first = (int)(rng_float(&r) * (float)nb);
    const int wide = rng_float(&r) < 0.35f;
    const int bay = unit_bay(g, u, m);
    /* on the ground in the bay's place: a rounded bay most times, a glass or concrete box's (the inter-war and brownstone
     * sheets' boxes for bays) */
    if (bay >= 0 && (kind == HI_ROUND ? rng_float(&r) < 0.85f : (in.face == HIF_GLASS || in.face == HIF_CONCRETE) && rng_float(&r) < 0.6f)) {
        const HVol *v = &g->vol[bay];
        const float most = m->z0 - home_yard(g->region)->proj;
        HInsert t = in;
        t.s0 = 0;
        const int s1top = (v->storeys < m->storeys ? v->storeys : m->storeys) - 1 + (v->storeys < m->storeys && tallR < 0.3f);   /* a storey over the bay's */
        /* wider and deeper than the bay it replaces (the inter-war sheet's rounded fronts), never over the door; else the bay's own */
        const HDoor *dr = g->unit[u].door >= 0 ? &g->door[g->unit[u].door] : NULL;
        for (int grow = 1; grow >= 0; --grow) {
            const float wd = grow ? 0.3f + 0.4f * depthR : 0.0f;
            t.x0 = fmaxf(v->x0 - wd, a + INS_PARTY); t.x1 = fminf(v->x1 + wd, b - INS_PARTY);
            if (dr && grow) {
                const float d0 = dr->x - 0.5f * dr->width - 0.35f, d1 = dr->x + 0.5f * dr->width + 0.35f;
                if (v->x1 <= d0 + 0.35f) t.x1 = fminf(t.x1, fmaxf(v->x1, d0));
                if (v->x0 >= d1 - 0.35f) t.x0 = fmaxf(t.x0, fminf(v->x0, d1));
            }
            t.depth = fminf(fmaxf(v->z1 - v->z0, 0.8f) + (grow ? 0.2f + 0.3f * depthR : 0.0f), most);
            t.r = fminf(t.depth, 0.5f * (t.x1 - t.x0)) * roundR;
            for (int s1 = s1top; s1 >= 0; --s1) {
                t.s1 = (uint8_t)s1;
                if (try_add(g, t, bay, log, &why)) return;
            }
        }
    }
    if (m->storeys < 2) { home_repair(g, log, "insert", "unit %d: no storey to hang its %s from, left off", u, home_insert_name(kind)); return; }
    /* hung */
    int span = wide && nb >= 3 ? 2 : 1;
    if (in.face == HIF_CONTAINER && bw < 2.4f && nb >= 2) span = 2;
    if (span > nb) span = nb;
    if (first + span > nb) first = nb - span;
    in.s0 = (uint8_t)(m->storeys >= 3 && upR < 0.25f ? 2 : 1);
    int ns = in.face == HIF_CONTAINER ? 1 : 1 + (tallR < (in.face == HIF_GLASS ? 0.6f : 0.5f)) + (in.face == HIF_GLASS && tallR < 0.25f);
    if (kind == HI_ROUND) ns = 1 + (tallR < 0.5f);
    in.s1 = (uint8_t)(in.s0 + ns - 1 < m->storeys - 1 ? in.s0 + ns - 1 : m->storeys - 1);
    static const float D0[HIF_COUNT] = { 0.9f, 0.8f, 0.9f, 1.1f }, D1[HIF_COUNT] = { 1.5f, 1.3f, 1.4f, 1.6f };
    in.depth = kind == HI_ROUND ? 0.9f + 0.6f * depthR : D0[in.face] + (D1[in.face] - D0[in.face]) * depthR;
    in.depth = fminf(in.depth, in.z - reach_min(g));
    /* bays without a balcony first; a balcony is taken in only where no free bay will do */
    for (int pass = 0; pass < 2; ++pass)
        for (int s1 = in.s1; s1 >= in.s0; --s1)
            for (int dist = 0; dist < nb; ++dist)
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    if (dist == 0 && sgn > 0) continue;
                    const int f = first + sgn * dist;
                    if (f < 0 || f + span > nb) continue;
                    HInsert t = in;
                    t.s1 = (uint8_t)s1;
                    span_x(&t, s0, bw, f, span, a, b);
                    t.r = fminf(t.depth, 0.5f * (t.x1 - t.x0)) * roundR;
                    int taken = 0;
                    for (int i = 0; i < g->nbalc && !pass; ++i) {
                        const HBalcony *bc = &g->balc[i];
                        taken |= bc->unit == u && bc->storey >= t.s0 && bc->storey <= t.s1 && overlap(bc->x0, bc->x1, t.x0, t.x1, 0.2f);
                    }
                    if (!taken && try_add(g, t, -1, log, &why)) return;
                }
    home_repair(g, log, "insert", "unit %d: no room on its front for a %s %s (%s), left off", u, home_insert_face_name(in.face), home_insert_name(kind), why ? why : "?");
}

void home_inserts(HGraph *g, int level, BLog *log) {
    g->nins = 0;
    memset(g->insPlan, 0, sizeof(g->insPlan));
    const int mi = main_vol(g);
    if (g->empty || level >= 2 || mi < 0 || g->style < 0 || g->style >= HS_COUNT) return;
    draw_plans(g, mi);
    for (int u = 0; u < g->nunit; ++u)
        for (int k = 0; k < HI_COUNT; ++k)
            if ((g->insPlan[u] >> k & 1u) && home_insert_built(k)) place(g, main_vol(g), u, k, log);
    if (g->nins) blog_printf(log, "  inserts: %d (%s %s)\n", g->nins, home_insert_face_name(g->ins[0].face), home_insert_name(g->ins[0].kind));
}

/* ---------------------------------------------------------------------------
 * The dressing (near: home_character.c's dress_pass, after the balconies, before the retrofit layer)
 * ------------------------------------------------------------------------- */

static void push_at(DC *d, double x, double y, double z, double dx, double dz) {
    const double p[3] = { x, y, z }, dir[3] = { dx, 0.0, dz };
    hc_push_along(d, p, dir);
}

/* A face's grilles (a cage's): pieces about a metre wide over len along the wall from s0 (its frame: the wall at p0 facing
 * (mx, mz), the way along it e), h tall from y */
static void cage_face(DC *d, double px, double pz, double ex, double ez, double mx, double mz, double len, double y, double h) {
    const int n = (int)fmax(1.0, floor(len / 1.0 + 0.5));
    const double w = len / n;
    for (int k = 0; k < n; ++k) {
        const double c = (k + 0.5) * w, p[3] = { px + ex * c, y, pz + ez * c };
        hc_push_wall(d, p, mx, mz);
        pm_scale(d->pm, (float)w, (float)h, 1.0f);
        pm_place(d->pm, PMK_H_GRILLE);
        hc_pop(d);
    }
}

/* what a region's owners hang under and put on top of their boxes: lights under, plants over the top's edge */
static void region_dress(int region, float *light, float *trail) {
    switch (region) {
    case PLAN_REGION_EAS: case PLAN_REGION_SEA: case PLAN_REGION_SAS: case PLAN_REGION_MENA: case PLAN_REGION_LAT: *light = 0.75f; *trail = 0.35f; break;
    case PLAN_REGION_SEU: *light = 0.6f; *trail = 0.45f; break;
    case PLAN_REGION_SSA: *light = 0.35f; *trail = 0.2f; break;
    default: *light = 0.5f; *trail = 0.25f; break;
    }
}

void hs_inserts(DC *d) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    float light, trail;
    region_dress(g->region, &light, &trail);
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        const int u = in->unit;
        const double dy = C->dy ? C->dy[u] : 0.0, zf = in->z - in->depth, W = in->x1 - in->x0;
        float fy0, fy1;
        home_insert_span(g, in, &fy0, &fy1);
        const double y0 = fy0 + dy, y1 = fy1 + dy;
        const uint32_t h = hash_combine(g->unit[u].seed, 0x1C2D0u + (uint32_t)i * 7919u);
        const HCharSet *cs = &C->cs[u];
        const float keep = cs->upkeep == HK_RUNDOWN ? 0.5f : cs->upkeep == HK_TIRED ? 0.8f : 1.0f;
        hc_begin(d, u);
        /* a screen box's breeze blocks in each storey's recess (home_shell_ins.c cuts them: the same heights) */
        if (in->kind == HI_BOX && in->face == HIF_SCREEN) {
            for (int k = in->s0; k <= in->s1; ++k) {
                const double ya = (k == in->s0 ? fy0 + 0.2 : g->plinth + k * g->storeyH + 0.12) + dy;
                const double yb = (k == in->s1 ? fy1 - 0.2 : g->plinth + (k + 1) * g->storeyH - 0.18) + dy;
                if (yb - ya < 0.6) continue;
                push_at(d, in->x0 + 0.17, ya, zf + 0.07, 1.0, 0.0);
                hr_breeze(d->pm, in->screen, (float)(W - 0.34), (float)(yb - ya));
                hc_pop(d);
            }
        }
        /* a cage: grilles over the glass, a storey at a time (the glass between its floor bands) */
        if (in->kind == HI_BOX && in->face == HIF_GLASS && in->cage) {
            for (int k = in->s0; k <= in->s1; ++k) {
                const double fl = g->plinth + k * g->storeyH;
                const double ya = (k == in->s0 ? (in->s0 ? fl + 0.08 : g->plinth + 0.3) : fl + 0.08) + dy;
                const double yb = (k == in->s1 ? fy1 - 0.25 : fl + g->storeyH - 0.22) + dy;
                if (yb - ya < 0.5) continue;
                cage_face(d, in->x0 + 0.1, zf + 0.05, 1.0, 0.0, 0.0, -1.0, W - 0.2, ya, yb - ya);
                cage_face(d, in->x0 + 0.05, zf + 0.1, 0.0, 1.0, -1.0, 0.0, in->depth - 0.2, ya, yb - ya);
                cage_face(d, in->x1 - 0.05, zf + 0.1, 0.0, 1.0, 1.0, 0.0, in->depth - 0.2, ya, yb - ya);
            }
        }
        /* lights under a hung one: an LED strip along its front, or downlights */
        if (in->s0 > 0 && !cs->lean && hf(h, 1u) < light * keep * (0.6f + 0.6f * g->affluence)) {
            if (in->kind == HI_BOX && hf(h, 2u) < 0.5f) {
                push_at(d, in->x0 + 0.12, y0, zf + 0.1, 1.0, 0.0);
                hr_led(d->pm, (float)(W - 0.24));
                hc_pop(d);
            } else {
                const int n = W > 2.6 ? 3 : 2;
                for (int k = 0; k < n; ++k) {
                    push_at(d, in->x0 + W * (k + 1) / (n + 1), y0, zf + 0.5 * fmin(in->depth, 1.0), 1.0, 0.0);
                    pm_place(d->pm, PMK_R_DOWNLIGHT);
                    hc_pop(d);
                }
            }
        }
        /* plants trailing over a box's top (not a container's) */
        if (in->kind == HI_BOX && in->face != HIF_CONTAINER && !cs->lean && hf(h, 3u) < trail * keep) {
            const double len = fmin(W - 0.3, 0.8 + 1.6 * hf(h, 4u));
            const double x = d->sx > 0.0 ? in->x1 - 0.15 : in->x0 + 0.15;
            const double p[3] = { x, y1, zf + 0.02 };
            hc_push_wall(d, p, 0.0, -1.0);
            hr_trail(d->pm, (float)len, 0.5f + 0.6f * hf(h, 5u), h);
            hc_pop(d);
        }
        hc_done(d);
        d->D->st.inserts++;
        /* the retrofit layer keeps clear of it */
        const double lo[3] = { in->x0 - 0.1, y0 - 0.3, zf - 0.35 }, hi[3] = { in->x1 + 0.1, y1 + 0.3, in->z + 0.05 };
        hc_occupy(d, lo, hi);
    }
}
