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
 *                 (HM5c2b: frames, hexagons, fins and screen strips as members; HM5c2c1: stairs as members, laid out from their
 *                 upper doors, and glass stair towers on the ground; HM5c2c2: the roof's kinds, unfinished storeys and roof
 *                 rooms, and the stubs on RCC and self-built roofs, in home_insert_roof.c)
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
static int overlap(float a0, float a1, float b0, float b1, float pad) { return a1 > b0 - pad && a0 < b1 + pad; }

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

/* HM5c2c1: stairs (the layout's section below) and stair towers */
#define STAIR_LAND 1.05f   /* a landing at a door, along the front (INS_DOOR_W and its frame) */
#define STAIR_V    0.20f   /* a concrete flight's slab, plumb */
#define STAIR_LT   0.20f   /* a landing's slab (a steel one's plate 0.08) */
#define STAIR_UP   0.95f   /* an upstand's or a rail's top over the nosings */
#define STAIR_UT   0.12f   /* an upstand's thickness */
#define STAIR_COL  0.25f   /* a concrete dog-leg's columns (a steel stair's posts 0.08) */
#define STAIR_HMAX 0.20f   /* the steepest riser */
#define STAIR_HMAX_HUNG 0.22f   /* ...a hung one's on a street-line front */
#define STAIR_PARTY 0.20f  /* a stair's ends off a party wall or a corner */
#define TOWER_HEAD 2.60f   /* a stair tower's head over a flat roof's deck or a parapet's wall plate (a door out onto the roof) */

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

/* HM5c2c1: a stair tower rises past a flat roof or a parapet by a stair head onto the roof (Georgian 5) */
static int tower_rises(const HVol *m) { return m->roof == HR_FLAT || m->parapet == 1; }

void home_insert_span(const HGraph *g, const HInsert *in, float *y0, float *y1) {
    if (home_insert_roof(in->kind)) { home_roof_span(g, in, y0, y1); return; }   /* HM5c2c2: home_insert_roof.c */
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    const float P = g->plinth, H = g->storeyH;
    if (in->kind == HI_STAIRS) {
        /* from the ground, or a hung one's bottom landing's slab; to its top landing's upstands */
        *y0 = in->s0 == 0 ? 0.0f : P + (float)in->s0 * H - STAIR_LT - 0.05f;
        *y1 = P + (float)in->s1 * H + STAIR_UP + 0.05f;
        return;
    }
    if (in->kind == HI_TOWER) {
        *y0 = 0.0f;
        *y1 = tower_rises(m) ? m->eaves + TOWER_HEAD : ins_ceiling(m);
        return;
    }
    *y0 = in->s0 == 0 ? 0.0f : P + (float)in->s0 * H - INS_SLAB;
    *y1 = fminf(P + (float)(in->s1 + 1) * H - 0.1f, ins_ceiling(m));
    const int memb = home_insert_membered(in);
    /* HM5c2b: a one-storey house's frame, fins or strip with no yard to stand in (a street-line front's: the pavement) stands
     * on corbels over its floor */
    if (memb && in->s0 == 0 && in->raised) *y0 = P + 0.4f;
    /* ...and a hung one's foot clears a canopy, porch roof or bay just under it */
    for (int i = 0; i < g->nvol && memb && in->s0 > 0; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind)) || v->z0 > m->z0 - 0.01f) continue;
        if ((v->unit != in->unit && v->unit != HG_ROW_WIDE) || v->x1 < in->x0 - 0.1f || v->x0 > in->x1 + 0.1f) continue;
        if (v->top > *y0 - 0.15f && v->top < *y0 + 0.6f) *y0 = v->top + 0.18f;
    }
    /* a hung one resting on a flat porch roof under it (a double veranda's, flattened under the floor it took in) */
    for (int i = 0; i < g->nvol && in->s0 > 0 && !memb; ++i) {
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
 * HM5c2b: the front's windows, and the members of frames, fins, hexagons and screen strips
 * ------------------------------------------------------------------------- */

const char *home_insert_shape_name(int s) {
    static const char *const N[HIS_COUNT] = { "portal", "chamfer", "lean", "zigzag", "apex", "blade", "brise-soleil", "grid", "louvre",
                                              "honeycomb", "hex-clad", "strip", "star strip", "straight", "dog-leg" };
    return s >= 0 && s < HIS_COUNT ? N[s] : "?";
}

int home_front_windows(const HGraph *g, int mi, int u, float (*out)[4], int max) {
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return 0;
    HVol own;
    const HVol *v = home_unit_vol(g, mi, u, &own);
    HFinish fin;
    home_finish(g, u, &fin);
    const float ww0 = fminf(fin.winW, bw - 0.6f);
    if (ww0 < 0.3f) return 0;
    int n = 0;
    for (int k = 0; k < v->storeys; ++k) {
        const float fl = g->plinth + (float)k * g->storeyH;
        float sill = fl + (k == 0 ? 0.75f : 0.85f), head = fminf(fl + 2.15f, v->eaves - 0.25f);
        if (k == g->vol[0].storeys - 1 && g->style == HS_GEORGIAN && v->storeys >= 3) head = fminf(head, sill + 1.0f);
        if (g->style == HS_COURTYARD && k == 0) { sill = fl + 1.6f; head = fminf(head, sill + 0.7f); }
        if (head - sill < 0.5f) continue;
        if (head - sill > fin.winH) sill = head - fin.winH;
        for (int b = 0; b < nb && n < max; ++b) {
            const float c = s0 + ((float)b + 0.5f) * bw;
            float ww = ww0, sl = sill, hd = head;
            int fb = -1;
            for (int i = 0; i < g->nbalc && k > 0; ++i) {
                const HBalcony *bc = &g->balc[i];
                if (bc->unit == u && bc->storey == k && c > bc->x0 && c < bc->x1) fb = i;
            }
            if (fb >= 0 && home_balcony_carved(g->balc[fb].kind)) continue;
            if (fb >= 0) { ww = home_french_width(ww0, bw); sl = fl + 0.03f; hd = fminf(fl + 2.2f, v->eaves - 0.2f); }
            /* HM5c2c1: a stair's door in its place */
            if (k > 0 && home_insert_door_at(g, u, k, c, v->z0, 0.5f * ww, 0.0f, -1.0f)) continue;
            out[n][0] = c - 0.5f * ww - 0.12f; out[n][1] = c + 0.5f * ww + 0.12f;
            out[n][2] = sl - 0.12f;            out[n][3] = hd + 0.12f;
            n++;
        }
    }
    return n;
}

/* HM5c2c1: a wall of unit u's slice of main mi: its face (z, or x on a side), its run along it (a0..a1: the slice at the front
 * and the back, the main's depth on a side), how far out of it a stair may reach (to the yard's margin, the rear boundary,
 * the side boundary), and whether a stair may stand there at all (a side only where the slice ends at the main's own end: no
 * party wall; the front only of a house with a front yard: never on a pavement). */
static int stair_wall(const HGraph *g, int mi, int u, int wall, float *face, float *a0, float *a1, float *reach) {
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const HUnit *un = &g->unit[u];
    const float sx0 = fmaxf(m->x0, un->x0), sx1 = fminf(m->x1, un->x1);
    if (sx1 - sx0 < 1.0f) return 0;
    switch (wall) {
    case HIW_FRONT: {
        if (home_style_street(g->style)) return 0;
        float s0, bw;
        const int nb = home_front_bays(g, mi, u, &s0, &bw);
        if (nb <= 0) return 0;
        *face = m->z0; *a0 = s0; *a1 = s0 + bw * (float)nb;
        *reach = m->z0 - home_yard(g->region)->proj;
        return 1;
    }
    case HIW_BACK:
        *face = m->z1; *a0 = sx0; *a1 = sx1; *reach = un->depth - m->z1 - 0.3f;
        return 1;
    case HIW_WEST:
        if (sx0 > m->x0 + 0.01f) return 0;
        *face = m->x0; *a0 = m->z0; *a1 = m->z1; *reach = m->x0 - un->x0 - 0.1f;
        return 1;
    case HIW_EAST:
        if (sx1 < m->x1 - 0.01f) return 0;
        *face = m->x1; *a0 = m->z0; *a1 = m->z1; *reach = un->x1 - m->x1 - 0.1f;
        return 1;
    default: return 0;
    }
}

int home_wall_windows(const HGraph *g, int mi, int u, int wall, float (*out)[4], int max) {
    if (wall == HIW_FRONT) return home_front_windows(g, mi, u, out, max);
    float face, a0, a1, reach;
    HVol own;
    const HVol *v = home_unit_vol(g, mi, u, &own);
    /* (the front's test of a free wall, without its pavement rule: the back and a side whatever the style) */
    const HUnit *un = &g->unit[u];
    const float sx0 = fmaxf(v->x0, un->x0), sx1 = fminf(v->x1, un->x1);
    if (wall == HIW_BACK) { face = v->z1; a0 = sx0; a1 = sx1; }
    else if (!stair_wall(g, mi, u, wall, &face, &a0, &a1, &reach)) return 0;
    HFinish fin;
    home_finish(g, u, &fin);
    /* the shell's add_windows: a window a bay of about 2.7 m (4 m on a side, some of them left blank there: all counted) */
    const float L = a1 - a0, bwant = home_wall_side(wall) ? 4.0f : 2.7f;
    if (L < 1.4f) return 0;
    const int nb = (int)fmaxf(1.0f, floorf(L / bwant + 0.5f));
    const float bw = L / (float)nb, ww = fminf(fin.winW, bw - 0.6f);
    if (ww < 0.3f) return 0;
    const float mx = wall == HIW_WEST ? -1.0f : wall == HIW_EAST ? 1.0f : 0.0f, mz = wall == HIW_BACK ? 1.0f : 0.0f;
    int n = 0;
    for (int k = 0; k < v->storeys; ++k) {
        const float fl = g->plinth + (float)k * g->storeyH;
        float sill = fl + (k == 0 ? 0.75f : 0.85f);
        const float head = fminf(fl + 2.15f, v->eaves - 0.25f);
        if (head - sill < 0.8f) continue;
        if (head - sill > fin.winH) sill = head - fin.winH;
        for (int b = 0; b < nb && n < max; ++b) {
            const float c = a0 + ((float)b + 0.5f) * bw;
            const float x = home_wall_side(wall) ? face : c, z = home_wall_side(wall) ? c : face;
            if (k > 0 && home_insert_door_at(g, u, k, x, z, 0.5f * ww, mx, mz)) continue;
            /* behind a volume standing against the wall (an outrigger, an extension, a wing): the shell cuts none */
            int behind = 0;
            for (int i = 0; i < g->nvol && !behind; ++i) {
                const HVol *o = &g->vol[i];
                if (o->role <= HV_ROW || head < o->y0 || sill > o->top) continue;
                const float px = x + mx * 0.2f, pz = z + mz * 0.2f;
                behind = px > o->x0 - 0.5f * ww && px < o->x1 + 0.5f * ww && pz > o->z0 - 0.5f * ww && pz < o->z1 + 0.5f * ww;
            }
            if (behind) continue;
            out[n][0] = c - 0.5f * ww - 0.12f; out[n][1] = c + 0.5f * ww + 0.12f;
            out[n][2] = sill - 0.12f;          out[n][3] = head + 0.12f;
            n++;
        }
    }
    return n;
}

/* whether the rectangle x0..x1, y0..y1 of the front stands clear of every window (their margins) */
static int clear_of(const float (*win)[4], int nw, float x0, float x1, float y0, float y1) {
    for (int i = 0; i < nw; ++i)
        if (x1 > win[i][0] + HEPS && x0 < win[i][1] - HEPS && y1 > win[i][2] + HEPS && y0 < win[i][3] - HEPS) return 0;
    return 1;
}

#define INTO_WALL_M 0.05f
#define FIN_PARTY   0.05f   /* blades, a grid's or a crown's uprights may stand this near a party wall (thin, they come in pairs
                               where both neighbours have them; the Georgian sheet's run on between the houses) */   /* how far a member touching the wall runs into it (home_shell_ins.c's INTO_WALL) */

typedef struct MB { HMember *m; int n, max; const HGraph *g; const HInsert *in; } MB;

static void mb_poly(MB *b, int axis, int role, const float (*p)[2], int n, float a0, float a1) {
    if (b->n >= b->max || n < 3 || n > 8 || a1 - a0 < 0.01f) return;
    /* anticlockwise in its plane */
    float area = 0.0f;
    for (int i = 0; i < n; ++i) area += p[i][0] * p[(i + 1) % n][1] - p[(i + 1) % n][0] * p[i][1];
    if (fabsf(area) < 1e-4f) return;
    HMember *m = &b->m[b->n++];
    m->axis = (uint8_t)axis; m->role = (uint8_t)role; m->n = (uint8_t)n; m->a0 = a0; m->a1 = a1;
    for (int i = 0; i < n; ++i) {
        const int j = area > 0.0f ? i : n - 1 - i;
        m->p[i][0] = p[j][0]; m->p[i][1] = p[j][1];
    }
}
static void mb_rect(MB *b, int axis, int role, float x0, float y0, float x1, float y1, float a0, float a1) {
    if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) return;
    const float p[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    mb_poly(b, axis, role, p, 4, a0, a1);
}
/* a raked member in the elevation from centre xa at height ya to xb at yb, w wide along its level ends */
static void mb_rake(MB *b, int role, float xa, float ya, float xb, float yb, float w, float a0, float a1) {
    if (yb - ya < 0.05f) return;
    const float hw = 0.5f * w;   /* w across, level: its ends stay within xa - w/2 .. xb + w/2 */
    const float p[4][2] = { { xa - hw, ya }, { xa + hw, ya }, { xb + hw, yb }, { xb - hw, yb } };
    mb_poly(b, HMB_ELEV, role, p, 4, a0, a1);
}

/* the convex hull of up to 8 points (anticlockwise), as a member's outline */
static int hull(float (*p)[2], int n, float (*out)[2]) {
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && (p[j][0] < p[j - 1][0] || (p[j][0] == p[j - 1][0] && p[j][1] < p[j - 1][1])); --j) {
            const float t0 = p[j][0], t1 = p[j][1];
            p[j][0] = p[j - 1][0]; p[j][1] = p[j - 1][1]; p[j - 1][0] = t0; p[j - 1][1] = t1;
        }
    float h[16][2];
    int k = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int start = k;
        for (int ii = 0; ii < n; ++ii) {
            const int i = pass ? n - 1 - ii : ii;
            while (k >= start + 2 && (h[k - 1][0] - h[k - 2][0]) * (p[i][1] - h[k - 2][1]) - (h[k - 1][1] - h[k - 2][1]) * (p[i][0] - h[k - 2][0]) <= 1e-6f) k--;
            h[k][0] = p[i][0]; h[k][1] = p[i][1]; k++;
        }
        k--;
    }
    if (k > 8) k = 8;
    for (int i = 0; i < k; ++i) { out[i][0] = h[i][0]; out[i][1] = h[i][1]; }
    return k;
}

/* A shelf or tie back to the wall across x0..x1 at y0..y1, out to `out`, where it clears the windows; else, if `beam`, only
 * its outer part (a beam in the frame's plane, from f0). Returns 1 if it reached the wall. */
/* whether anything stands in front of the wall in the box x0..x1, y0..y1, out to `out` (a porch, a bay, a canopy) */
static int vol_in_way(const HGraph *g, const HInsert *in, float x0, float x1, float y0, float y1, float out) {
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind)) || v->z0 > m->z0 - 0.01f) continue;
        if (v->unit != in->unit && v->unit != HG_ROW_WIDE) continue;
        if (overlap(v->x0, v->x1, x0, x1, 0.05f) && out > -0.05f && m->z0 - v->z0 > -0.05f && y0 < v->top + 0.15f && y1 > v->y0 - 0.05f) return 1;
    }
    return 0;
}

static int mb_tie(MB *b, const float (*win)[4], int nw, int role, float x0, float x1, float y0, float y1, float f0, float out, int beam) {
    if (clear_of(win, nw, x0, x1, y0, y1) && !vol_in_way(b->g, b->in, x0, x1, y0, y1, out)) {
        mb_rect(b, HMB_ELEV, role, x0, y0, x1, y1, -INTO_WALL_M, out);
        return 1;
    }
    if (beam) mb_rect(b, HMB_ELEV, HMR_BEAM, x0, y0, x1, y1, f0, out);
    return 0;
}

static float floor_y(const HGraph *g, int k) { return g->plinth + (float)k * g->storeyH; }

/* A frame: its plane `depth` out, the members `t` thick from depth - t, `w` wide; tied back by shelves at its floor lines
 * and a slab at its head where they clear the windows. */
static void frame_members(MB *b, const HGraph *g, const HInsert *in, float y0, float y1, const float (*win)[4], int nw) {
    const float X0 = in->x0, X1 = in->x1, w = in->w, D = in->depth, f0 = D - in->t, hw = 0.5f * w;
    const int ground = in->s0 == 0;
    float lv[8];
    int nl = 0;
    lv[nl++] = y0;
    for (int k = in->s0 + 1; k <= in->s1 && nl < 6; ++k) lv[nl++] = floor_y(g, k);
    lv[nl++] = y1;
    /* the legs and the head */
    float c = 0.0f;
    switch (in->shape) {
    case HIS_CHAMFER:
        c = fminf(0.9f, fmaxf(w + 0.25f, 0.25f * (X1 - X0)));
        /* fall through */
    case HIS_PORTAL: case HIS_ZIGZAG:
        mb_rect(b, HMB_ELEV, HMR_LEG, X0, y0, X0 + w, y1 - c, f0, D);
        mb_rect(b, HMB_ELEV, HMR_LEG, X1 - w, y0, X1, y1 - c, f0, D);
        if (c > 0.0f) {
            /* the corners cut at 45 degrees: from the legs' tops to the head's ends */
            const float l[4][2] = { { X0, y1 - c }, { X0 + w, y1 - c }, { X0 + c, y1 - w }, { X0 + c, y1 } };
            const float r[4][2] = { { X1 - w, y1 - c }, { X1, y1 - c }, { X1 - c, y1 }, { X1 - c, y1 - w } };
            mb_poly(b, HMB_ELEV, HMR_LEG, l, 4, f0, D);
            mb_poly(b, HMB_ELEV, HMR_LEG, r, 4, f0, D);
        }
        mb_rect(b, HMB_ELEV, HMR_BEAM, X0 + c, y1 - w, X1 - c, y1, f0, D);
        break;
    case HIS_LEAN: {
        /* one leg raked, its foot in by |lean| (a trapezoid wider at its head) */
        const float L = fminf(fabsf(in->lean), 0.5f * (X1 - X0) - 1.2f * w);
        if (in->lean >= 0.0f) { mb_rect(b, HMB_ELEV, HMR_LEG, X0, y0, X0 + w, y1, f0, D); mb_rake(b, HMR_LEG, X1 - hw - fmaxf(L, 0.0f), y0, X1 - hw, y1, w, f0, D); }
        else { mb_rect(b, HMB_ELEV, HMR_LEG, X1 - w, y0, X1, y1, f0, D); mb_rake(b, HMR_LEG, X0 + hw + fmaxf(L, 0.0f), y0, X0 + hw, y1, w, f0, D); }
        mb_rect(b, HMB_ELEV, HMR_BEAM, X0, y1 - w, X1, y1, f0, D);
        break;
    }
    case HIS_APEX: {
        const float xc = 0.5f * (X0 + X1);
        mb_rake(b, HMR_LEG, X0 + hw, y0, xc, y1, w, f0, D);
        mb_rake(b, HMR_LEG, X1 - hw, y0, xc, y1, w, f0, D);
        break;
    }
    default: break;
    }
    /* the zigzag's diagonals, one a storey, turn about */
    if (in->shape == HIS_ZIGZAG)
        for (int i = 0; i + 1 < nl; ++i) {
            const float ya = lv[i] + (i == 0 && !ground ? w : 0.0f), yb = lv[i + 1] - (i + 2 == nl ? w : 0.0f);
            const float xl = X0 + w + hw, xr = X1 - w - hw;
            if (i & 1) mb_rake(b, HMR_LEG, xr, ya, xl, yb, w, f0, D);
            else mb_rake(b, HMR_LEG, xl, ya, xr, yb, w, f0, D);
        }
    /* the foot of a hung one: a beam in its plane and a shelf back to the wall at its floor line */
    if (!ground) {
        mb_rect(b, HMB_ELEV, HMR_BEAM, X0, y0, X1, y0 + fmaxf(w, 0.25f), f0, D);
        mb_tie(b, win, nw, HMR_SHELF, X0, X1, y0, floor_y(g, in->s0) + 0.02f, f0, D, 0);
    }
    /* shelves at the floor lines between (an apex's only as wide as its legs there) */
    for (int i = 1; i + 1 < nl; ++i) {
        float xa = X0, xb = X1;
        if (in->shape == HIS_APEX) {
            const float f = (lv[i] - y0) / (y1 - y0), xc = 0.5f * (X0 + X1);
            xa = X0 + hw + (xc - X0 - hw) * f; xb = X1 - hw - (X1 - hw - xc) * f;
        }
        mb_tie(b, win, nw, HMR_SHELF, xa, xb, lv[i] - 0.2f, lv[i] + 0.02f, f0, D, 1);
    }
    /* the head's slab back to the wall */
    if (in->shape != HIS_APEX) mb_tie(b, win, nw, HMR_SHELF, X0 + c, X1 - c, y1 - 0.17f, y1, f0, f0 + 0.02f, 0);   /* (0.3 m under a floor is a recess's ceiling's plane) */
}

/* The piers of the front about bay line x: from the window left of it to the one right (their margins), within lo..hi. */
static void pier_at(const float (*win)[4], int nw, float x, float ya, float yb, float lo, float hi, float *pl, float *pr) {
    *pl = lo; *pr = hi;
    for (int i = 0; i < nw; ++i) {
        if (yb <= win[i][2] || ya >= win[i][3]) continue;
        const float c = 0.5f * (win[i][0] + win[i][1]);
        if (c < x) *pl = fmaxf(*pl, win[i][1]);
        else *pr = fminf(*pr, win[i][0]);
    }
}

/* Blades on the bay lines (in plan, raked by lean), a grid's shelves, a crown's slats between two cheeks. */
/* A brise-soleil (the bungalow sheet's panel 6, Japan's, the compound's): thin upright fins a 0.4 m pitch standing off the
 * wall in front of the windows, between two cheeks `w` wide, a head beam and (hung) a foot; brackets back to the wall at
 * the floor lines. */
static void brise_members(MB *b, const HGraph *g, const HInsert *in, float y0, float y1, const float (*win)[4], int nw) {
    const float X0 = in->x0, X1 = in->x1, w = in->w, D = in->depth, f0 = fmaxf(INS_OFF, D - 0.32f);
    const int ground = y0 < 0.05f;
    mb_rect(b, HMB_ELEV, HMR_POST, X0, y0, X0 + w, y1, f0, D);
    mb_rect(b, HMB_ELEV, HMR_POST, X1 - w, y0, X1, y1, f0, D);
    mb_rect(b, HMB_ELEV, HMR_BEAM, X0 + w, y1 - 0.16f, X1 - w, y1, f0, D);
    if (!ground) mb_rect(b, HMB_ELEV, HMR_BEAM, X0 + w, y0, X1 - w, y0 + 0.16f, f0, D);
    const float a = X0 + w, L = X1 - w - a;
    const int ns = (int)floorf(L / 0.4f);
    for (int k = 1; k < ns && b->n < b->max - 8; ++k) {
        const float x = a + L * (float)k / (float)ns;
        mb_rect(b, HMB_ELEV, HMR_SLAT, x - 0.035f, y0 + (ground ? 0.0f : 0.16f), x + 0.035f, y1 - 0.16f, f0 + 0.02f, D - 0.02f);
    }
    if (!ground && in->s0 > 0) mb_tie(b, win, nw, HMR_TIE, X0, X1, y0, floor_y(g, in->s0) + 0.02f, f0, f0 + 0.02f, 0);
    for (int k = in->s0 + 1; k <= in->s1 + 1; ++k) {
        const float ya = k <= in->s1 ? floor_y(g, k) - 0.18f : y1 - 0.16f, yb = k <= in->s1 ? floor_y(g, k) : y1;
        if (yb > y1 + HEPS) break;
        mb_tie(b, win, nw, HMR_TIE, X0, X0 + w, ya, yb, f0, f0 + 0.02f, 0);
        mb_tie(b, win, nw, HMR_TIE, X1 - w, X1, ya, yb, f0, f0 + 0.02f, 0);
    }
}

static void fin_members(MB *b, const HGraph *g, const HInsert *in, float y0, float y1, const float (*win)[4], int nw) {
    if (in->shape == HIS_BLADE_SQ) { brise_members(b, g, in, y0, y1, win, nw); return; }
    float s0, bw;
    const int nb = home_front_bays(g, in->vol, in->unit, &s0, &bw);
    if (nb <= 0) return;
    const float lo = s0 + FIN_PARTY, hi = s0 + bw * (float)nb - FIN_PARTY, D = in->depth;
    const int crown = in->shape == HIS_LOUVRE;
    float bx0 = 1e9f, bx1 = -1e9f;
    int first = -1, last = -1;
    for (int j = 0; j <= nb; ++j) {
        const float x = s0 + bw * (float)j;
        if (x < in->x0 - 0.01f || x > in->x1 + 0.01f) continue;
        if (first < 0) first = j;
        last = j;
    }
    for (int j = first; j >= 0 && j <= last; ++j) {
        if (crown && j != first && j != last) continue;
        const float x = s0 + bw * (float)j;
        float pl, pr;
        pier_at(win, nw, x, y0, y1, lo, hi, &pl, &pr);
        const float wb = fminf(in->w, pr - pl - 0.02f);
        if (wb < 0.1f) {
            /* a grid's or a crown's upright with no pier to stand on: off the wall, on the bay line, tied by the shelves */
            if ((in->shape == HIS_GRID || crown) && D >= INS_OFF + 0.15f) {
                const float xc = fminf(fmaxf(x, lo + 0.5f * in->w), hi - 0.5f * in->w);
                const float p[4][2] = { { xc - 0.5f * in->w, INS_OFF }, { xc + 0.5f * in->w, INS_OFF }, { xc + 0.5f * in->w, D }, { xc - 0.5f * in->w, D } };
                mb_poly(b, HMB_PLAN, HMR_POST, p, 4, y0, y1);
                bx0 = fminf(bx0, xc - 0.5f * in->w); bx1 = fmaxf(bx1, xc + 0.5f * in->w);
            }
            continue;
        }
        /* on the bay line if the pier allows, else the pier's middle */
        const float xc = fminf(fmaxf(x, pl + 0.01f + 0.5f * wb), pr - 0.01f - 0.5f * wb);
        /* a raked blade (the Georgian sheet's panel 3): square on its pier out to the shell's window test (0.36 m), then raked
         * by lean in front of the windows to its tip; its tip within the slice */
        float L = in->shape == HIS_BLADE && D > 0.5f ? in->lean : 0.0f;
        if (L > 0.0f) L = fminf(L, hi - (xc + 0.5f * wb));
        if (L < 0.0f) L = fmaxf(L, lo - (xc - 0.5f * wb));
        if (fabsf(L) < 0.15f) L = 0.0f;   /* (a slight rake would lay its sides on its base's: one straight blade) */
        const float sq = L != 0.0f ? 0.37f : D;
        const float p[4][2] = { { xc - 0.5f * wb, -INTO_WALL_M }, { xc + 0.5f * wb, -INTO_WALL_M }, { xc + 0.5f * wb, sq }, { xc - 0.5f * wb, sq } };
        mb_poly(b, HMB_PLAN, HMR_FIN, p, 4, y0, y1);
        if (L != 0.0f) {
            const float q[4][2] = { { xc - 0.5f * wb, 0.36f }, { xc + 0.5f * wb, 0.36f }, { xc + 0.5f * wb + L, D }, { xc - 0.5f * wb + L, D } };
            mb_poly(b, HMB_PLAN, HMR_FIN, q, 4, y0, y1);
        }
        bx0 = fminf(bx0, xc - 0.5f * wb); bx1 = fmaxf(bx1, xc + 0.5f * wb);
    }
    if (in->shape == HIS_BLADE && b->n < 2) {
        /* no piers for them (a narrow front's windows fill its bays): panels raked in plan standing off the wall in front of
         * the windows, about 0.8 m apart, a head beam over them and brackets back to the wall at its floor lines */
        b->n = 0;
        if (D < INS_OFF + 0.25f) return;
        const int np = (int)fmaxf(2.0f, floorf((hi - lo) / 0.8f) + 1.0f);
        const float pw = fminf(in->w, 0.2f), step = (hi - lo - pw) / (float)(np - 1);
        float L = in->lean;
        L = L > 0.0f ? fminf(L, 0.5f * step) : fmaxf(L, -0.5f * step);
        for (int k = 0; k < np; ++k) {
            float xa = lo + step * (float)k;
            if (L > 0.0f) xa = fminf(xa, hi - pw - L); else xa = fmaxf(xa, lo - L);
            const float p[4][2] = { { xa, INS_OFF }, { xa + pw, INS_OFF }, { xa + pw + L, D }, { xa + L, D } };
            mb_poly(b, HMB_PLAN, HMR_FIN, p, 4, y0, y1);
        }
        mb_rect(b, HMB_ELEV, HMR_BEAM, lo, y1 - 0.16f, hi, y1, INS_OFF, D);
        for (int k = in->s0 + 1; k <= in->s1 + 1; ++k) {
            const float ya = k <= in->s1 ? floor_y(g, k) - 0.18f : y1 - 0.16f, yb = k <= in->s1 ? floor_y(g, k) : y1;
            if (yb > y1 + HEPS) break;
            mb_tie(b, win, nw, HMR_TIE, lo, lo + pw, ya, yb, INS_OFF, INS_OFF + 0.02f, 0);
            mb_tie(b, win, nw, HMR_TIE, hi - pw, hi, ya, yb, INS_OFF, INS_OFF + 0.02f, 0);
        }
        return;
    }
    if (bx1 - bx0 < 0.5f) return;
    if (in->shape == HIS_GRID) {
        /* shelves at its floor lines and its top: an egg-crate */
        for (int kk = in->s0; kk <= in->s1; ++kk) {
            if (kk == 0) continue;
            const float f = floor_y(g, kk);
            mb_tie(b, win, nw, HMR_SHELF, bx0, bx1, kk == in->s0 ? y0 : f - 0.2f, f + 0.02f, D, D, 0);
        }
        mb_tie(b, win, nw, HMR_SHELF, bx0, bx1, y1 - 0.17f, y1, D, D, 0);
    }
    if (crown) {
        /* slats 0.05 m thick a 0.24 m pitch, 0.42 m off the wall and more; a cap over them */
        const float sa = fmaxf(INS_OFF + 0.02f, D - 0.25f);
        for (float y = y0 + 0.35f; y < y1 - 0.25f; y += 0.24f) mb_rect(b, HMB_ELEV, HMR_SLAT, bx0 + 0.02f, y, bx1 - 0.02f, y + 0.05f, sa, D - 0.03f);
        if (!mb_tie(b, win, nw, HMR_SHELF, bx0, bx1, y1 - 0.14f, y1, sa, D, 0)) mb_rect(b, HMB_ELEV, HMR_BEAM, bx0, y1 - 0.14f, bx1, y1, sa, D);
    }
}

/* A honeycomb: flat-topped hexagons of corner radius r, a column every 1.5 r and the odd ones half a hexagon up; each a ring
 * of six members round its glass (the window it frames), a side shared by two hexagons one member. */
static const int8_t HEXCELL[4][5][2] = {
    { { 0, 0 } }, { { 0, 0 }, { 0, 1 } }, { { 0, 0 }, { 0, 1 }, { 1, 0 } }, { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 2, 0 }, { 2, 1 } },
};
static int hex_layout(int cells) { return cells >= 5 ? 3 : cells >= 3 ? 2 : cells == 2 ? 1 : 0; }
static int hex_count(int cells) { static const int N[4] = { 1, 2, 3, 5 }; return N[hex_layout(cells)]; }
static float hex_width(int cells, float r) { static const float W[4] = { 2.0f, 2.0f, 3.5f, 5.0f }; return W[hex_layout(cells)] * r; }
static float hex_height(int cells, float r) { return (cells >= 2 ? 4.0f : 2.0f) * 0.8660254f * r; }

static void hex_members(MB *b, const HGraph *g, const HInsert *in, float y0) {
    const float R = in->r, hh = 0.8660254f * R, Ri = R - in->w / 0.8660254f, D = in->depth;
    if (Ri < 0.3f) return;
    const int L = hex_layout(in->cells), n = hex_count(in->cells);
    const float yb = fmaxf(floor_y(g, in->s0), y0 + INS_SLAB);
    float cx[5], cy[5];
    for (int i = 0; i < n; ++i) {
        const int c = HEXCELL[L][i][0], r = HEXCELL[L][i][1];
        cx[i] = in->x0 + R + 1.5f * R * (float)c;
        cy[i] = yb + hh + 2.0f * hh * (float)r + ((c & 1) ? hh : 0.0f);
    }
    for (int i = 0; i < n; ++i)
        for (int e = 0; e < 6; ++e) {
            const float a0 = 1.0471976f * (float)e, a1 = 1.0471976f * (float)(e + 1);
            const float mx = cx[i] + R * 0.5f * (cosf(a0) + cosf(a1)), my = cy[i] + R * 0.5f * (sinf(a0) + sinf(a1));
            /* the side's other hexagon: emitted by the first of the two */
            int other = -1;
            for (int j = 0; j < n && other < 0; ++j) {
                if (j == i) continue;
                for (int f = 0; f < 6; ++f) {
                    const float b0 = 1.0471976f * (float)f, b1 = 1.0471976f * (float)(f + 1);
                    if (fabsf(cx[j] + R * 0.5f * (cosf(b0) + cosf(b1)) - mx) < 0.01f && fabsf(cy[j] + R * 0.5f * (sinf(b0) + sinf(b1)) - my) < 0.01f) { other = j; break; }
                }
            }
            if (other >= 0 && other < i) continue;
            float p[8][2];
            int np = 0;
            p[np][0] = cx[i] + R * cosf(a0); p[np][1] = cy[i] + R * sinf(a0); np++;
            p[np][0] = cx[i] + R * cosf(a1); p[np][1] = cy[i] + R * sinf(a1); np++;
            p[np][0] = cx[i] + Ri * cosf(a0); p[np][1] = cy[i] + Ri * sinf(a0); np++;
            p[np][0] = cx[i] + Ri * cosf(a1); p[np][1] = cy[i] + Ri * sinf(a1); np++;
            if (other >= 0) {
                /* the other's inner corners on that side: the reflection of ours through the side */
                for (int q = 2; q < 4; ++q) { p[np][0] = 2.0f * mx - p[q][0]; p[np][1] = 2.0f * my - p[q][1]; np++; }
            }
            float hp[8][2];
            const int nh = hull(p, np, hp);
            mb_poly(b, HMB_ELEV, HMR_RING, hp, nh, -INTO_WALL_M, D);
        }
    /* the glass, inside the rings */
    for (int i = 0; i < n; ++i) {
        float p[6][2];
        for (int e = 0; e < 6; ++e) { p[e][0] = cx[i] + (Ri + 0.02f) * cosf(1.0471976f * (float)e); p[e][1] = cy[i] + (Ri + 0.02f) * sinf(1.0471976f * (float)e); }
        mb_poly(b, HMB_ELEV, HMR_GLASS, p, 6, -INTO_WALL_M, D - 0.15f);
    }
}

/* A screen strip: two posts and a head in a plane `depth` out (a foot on the ground or a beam over a corbel), brackets back
 * to the wall at the floor lines; the lattice between them is the dressing's. */
static void strip_members(MB *b, const HGraph *g, const HInsert *in, float y0, float y1, const float (*win)[4], int nw) {
    const float X0 = in->x0, X1 = in->x1, w = in->w, D = in->depth, f0 = D - in->t;
    const int ground = in->s0 == 0;
    mb_rect(b, HMB_ELEV, HMR_POST, X0, y0, X0 + w, y1, f0, D);
    mb_rect(b, HMB_ELEV, HMR_POST, X1 - w, y0, X1, y1, f0, D);
    mb_rect(b, HMB_ELEV, HMR_BEAM, X0 + w, y1 - w, X1 - w, y1, f0, D);
    mb_rect(b, HMB_ELEV, HMR_BEAM, X0 + w, y0, X1 - w, y0 + (ground ? 0.25f : w), f0, D);
    if (!ground) mb_tie(b, win, nw, HMR_TIE, X0, X1, y0, floor_y(g, in->s0) + 0.02f, f0, f0 + 0.02f, 0);
    for (int k = in->s0 + 1; k <= in->s1 + 1; ++k) {
        const float ya = k <= in->s1 ? floor_y(g, k) - 0.18f : y1 - 0.18f, yb = k <= in->s1 ? floor_y(g, k) : y1;
        if (yb > y1 + HEPS) break;
        mb_tie(b, win, nw, HMR_TIE, X0, X0 + w, ya, yb, f0, f0 + 0.02f, 0);
        mb_tie(b, win, nw, HMR_TIE, X1 - w, X1, ya, yb, f0, f0 + 0.02f, 0);
    }
}

/* ---------------------------------------------------------------------------
 * HM5c2c1: stairs. The layout along the front (the plan's "HM5c2c1"): a stair rises the way in->lean points, to a landing
 * STAIR_LAND long round its door at in->door on each floor it reaches.
 *   straight  one climb: its flight against the wall (out 0..w) from the foot up to the landing; hung, a landing at a door
 *             on the floor below beyond its foot
 *   dog-leg   a climb a storey: the first flight in the outer lane (out w + STAIR_GAP .. D) from beside the landing away
 *             from it, a half landing, the second in the lane against the wall back to the landing; a second storey's
 *             climb stacked over the first (the same risers a flight, so the half landings stand over each other)
 * Concrete: an inclined slab a flight (STAIR_V plumb) with a triangle a step on it, landings STAIR_LT thick, upstands
 * (in->screen) on the open sides and a spine between a dog-leg's lanes; columns under a grounded dog-leg's half landings and
 * its landing's free corner. Steel (in->cage): two stringers a flight, plate treads, plate landings on posts.
 * ------------------------------------------------------------------------- */

typedef struct StairPlan {
    int   nclimb, n;       /* climbs (a storey each); risers a climb's flight (straight) or each flight (dog-leg) */
    int   dog, hung;
    float s, t, w, D, Lh;  /* the way it rises; a tread; a flight's width; out; half a landing */
    float yA[3], h[3];     /* each climb's foot and its risers */
    float xf;              /* the foot riser's x (straight); a dog-leg's first flight's (beside the landing) */
    float xh0, xh1;        /* a dog-leg's half landings, from the near edge to the far */
    float xb0, xb1;        /* a hung straight stair's bottom landing */
} StairPlan;

static float floor_y(const HGraph *g, int k);

static int stair_plan(const HGraph *g, const HInsert *in, StairPlan *P) {
    memset(P, 0, sizeof(*P));
    P->dog = in->shape == HIS_DOGLEG; P->hung = in->s0 > 0;
    P->s = in->lean >= 0.0f ? 1.0f : -1.0f; P->t = in->t; P->w = in->w; P->D = in->depth; P->Lh = 0.5f * (in->r > STAIR_LAND ? in->r : STAIR_LAND);
    P->nclimb = in->s1 - in->s0;
    if (P->nclimb < 1 || P->nclimb > 2 || (!P->dog && P->nclimb != 1) || P->t < 0.15f) return 0;
    for (int c = 0; c < P->nclimb; ++c) P->yA[c] = in->s0 + c == 0 ? 0.0f : floor_y(g, in->s0 + c);
    const float rise0 = floor_y(g, in->s0 + 1) - P->yA[0];
    if (P->dog) {
        P->n = (int)ceilf(rise0 / (2.0f * STAIR_HMAX) - 1e-4f);   /* risers a flight, the same every climb */
        for (int c = 0; c < P->nclimb; ++c) P->h[c] = (floor_y(g, in->s0 + c + 1) - P->yA[c]) / (float)(2 * P->n);
        P->xf = in->door + P->s * P->Lh;
        P->xh0 = P->xf + P->s * (float)(P->n - 1) * P->t;
        P->xh1 = P->xh0 + P->s * (P->w + 0.1f);
    } else {
        /* the steepest the risers allow: a terrace unit's front is short (a hung one's, a self-built favela's, steeper still) */
        P->n = (int)ceilf(rise0 / (P->hung ? STAIR_HMAX_HUNG : STAIR_HMAX) - 1e-4f);
        P->h[0] = rise0 / (float)P->n;
        P->xf = in->door - P->s * P->Lh - P->s * (float)(P->n - 1) * P->t;
        if (P->hung) { P->xb1 = P->xf; P->xb0 = P->xf - P->s * STAIR_LAND; }
    }
    return 1;
}

/* the stair's extent along the front, from its plan */
static void stair_extent(const StairPlan *P, float door, float *x0, float *x1) {
    float a = door - P->Lh, b = door + P->Lh;
    const float e[3] = { P->xf, P->dog ? P->xh1 : P->xf, P->hung ? P->xb0 : P->xf };
    for (int i = 0; i < 3; ++i) { a = fminf(a, e[i]); b = fmaxf(b, e[i]); }
    *x0 = a; *x1 = b;
}

/* A stair's doors: their floors and middles. */
static int stair_doors(const HGraph *g, const HInsert *in, int *k, float *x, int max) {
    StairPlan P;
    if (!stair_plan(g, in, &P)) return 0;
    int n = 0;
    if (P.hung && n < max) { k[n] = in->s0; x[n] = P.dog ? in->door : 0.5f * (P.xb0 + P.xb1); n++; }
    for (int f = in->s0 + 1; f <= in->s1 && n < max; ++f) { k[n] = f; x[n] = in->door; n++; }
    return n;
}

int home_insert_door_at(const HGraph *g, int u, int k, float x, float z, float hw, float mx, float mz) {
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (in->kind != HI_STAIRS || in->unit != u || k < in->s0 || k > in->s1) continue;
        float ex, ez, nx, nz, fx0, fz0;
        home_ins_axes(in, &ex, &ez, &nx, &nz);
        home_ins_point(in, 0.0f, 0.0f, &fx0, &fz0);
        /* a window on the stair's own wall: facing its way, in its face */
        if (nx * mx + nz * mz < 0.9f || fabsf((x - fx0) * nx + (z - fz0) * nz) > 0.3f) continue;
        const float a = (x - fx0) * ex + (z - fz0) * ez;
        int fk[4];
        float fa[4];
        const int n = stair_doors(g, in, fk, fa, 4);
        for (int j = 0; j < n; ++j)
            if (fk[j] == k && fabsf(a - fa[j]) < hw + 0.5f * INS_DOOR_W + 0.1f) return 1;
    }
    return 0;
}

/* One flight: n risers of h from the foot riser at xa (height ya) the way s points, treads t, in the lane o0..o1. */
static void flight_members(MB *b, const HInsert *in, float xa, float ya, int n, float h, float s, float t, float o0, float o1, int outer) {
    const float xe = xa + s * (float)(n - 1) * t;   /* the last riser: the landing's edge */
    const float top = ya + (float)n * h;
    if (in->cage) {
        /* stringers at the lane's sides, plate treads between */
        const float V = 0.22f;
        for (int q = 0; q < 2; ++q) {
            const float a0 = q ? o1 - 0.03f : o0, a1 = q ? o1 : o0 + 0.03f;
            /* along the nosings, V deep plumb; its foot cut level at the flight's foot */
            float p[5][2];
            int np = 0;
            const float yb0 = ya + h - V;
            p[np][0] = xa; p[np][1] = ya + h; np++;
            p[np][0] = xe; p[np][1] = top; np++;
            p[np][0] = xe; p[np][1] = top - V; np++;
            if (yb0 < ya) {
                p[np][0] = xa + s * (ya - yb0) * t / h; p[np][1] = ya; np++;
                p[np][0] = xa; p[np][1] = ya; np++;
            } else { p[np][0] = xa; p[np][1] = yb0; np++; }
            mb_poly(b, HMB_ELEV, HMR_STRINGER, (const float (*)[2])p, np, a0, a1);
        }
        for (int i = 1; i < n; ++i) {
            const float xi = xa + s * (float)(i - 1) * t, y = ya + (float)i * h;
            mb_rect(b, HMB_ELEV, HMR_TREAD, fminf(xi, xi + s * t), y - 0.04f, fmaxf(xi, xi + s * t), y, o0 + 0.03f, o1 - 0.03f);
        }
        return;
    }
    /* the slab: under the line of the steps' inner corners, STAIR_V plumb; on the ground its foot cut level */
    const float x1i = xe, y1i = top - h;
    if (ya < 0.01f) {
        const float cut = xa + s * STAIR_V * t / h;
        const float p[4][2] = { { xa, 0.0f }, { cut, 0.0f }, { x1i, y1i - STAIR_V }, { x1i, y1i } };
        mb_poly(b, HMB_ELEV, HMR_FLIGHT, p, 4, o0, o1);
    } else {
        const float p[4][2] = { { xa, ya }, { xa, ya - STAIR_V }, { x1i, y1i - STAIR_V }, { x1i, y1i } };
        mb_poly(b, HMB_ELEV, HMR_FLIGHT, p, 4, o0, o1);
    }
    for (int i = 1; i < n; ++i) {
        const float xi = xa + s * (float)(i - 1) * t, y = ya + (float)i * h;
        const float p[3][2] = { { xi, y - h }, { xi, y }, { xi + s * t, y } };
        mb_poly(b, HMB_ELEV, HMR_STEP, p, 3, o0, o1);
    }
    /* its upstand on the open side (an outer lane's), from the slab's soffit to STAIR_UP over the nosings */
    if (in->screen && outer) {
        const float base = ya < 0.01f ? 0.0f : ya - STAIR_V;
        const float p[5][2] = { { xa, base }, { xa, ya + h + STAIR_UP }, { xe, top + STAIR_UP }, { xe, y1i - STAIR_V },
                                { ya < 0.01f ? xa + s * STAIR_V * t / h : xa, ya < 0.01f ? 0.0f : ya - STAIR_V } };
        float hp[5][2], c[8][2];
        memcpy(hp, p, sizeof(hp));
        const int nh = hull(hp, 5, c);
        mb_poly(b, HMB_ELEV, HMR_UPSTAND, (const float (*)[2])c, nh, o1 - STAIR_UT, o1);
    }
}

/* A landing xa..xb (either order) at floor y, out to D; its upstands on its outer edge and its free end (at xe, if set). */
static void landing_members(MB *b, const HInsert *in, float xa, float xb, float y, float D, float xe, int freeEnd) {
    const float x0 = fminf(xa, xb), x1 = fmaxf(xa, xb);
    const float T = in->cage ? 0.08f : STAIR_LT;
    mb_rect(b, HMB_ELEV, HMR_LANDING, x0, y - T, x1, y, -INTO_WALL_M, D);
    if (!in->screen || in->cage) return;
    mb_rect(b, HMB_ELEV, HMR_UPSTAND, x0, y - T, x1, y + STAIR_UP, D - STAIR_UT, D);
    if (freeEnd) {
        const float e0 = xe < 0.5f * (x0 + x1) ? x0 : x1 - STAIR_UT;
        mb_rect(b, HMB_ELEV, HMR_UPSTAND, e0, y - T, e0 + STAIR_UT, y + STAIR_UP, -INTO_WALL_M, D - STAIR_UT);
    }
}

/* a column (a concrete dog-leg's) or a post (a steel stair's) from the ground (or y0) to y1 at x, out d */
static void stair_post(MB *b, const HInsert *in, float x, float d, float y0, float y1) {
    const float c = in->cage ? 0.08f : STAIR_COL, hc = 0.5f * c;
    mb_rect(b, HMB_ELEV, HMR_LEG, x - hc, y0, x + hc, y1, d - c, d);
}

int home_stair_parts(const HGraph *g, const HInsert *in, HFlight *fl, int *nfl, HLanding *la, int *nla, float *tread) {
    StairPlan P;
    *nfl = *nla = 0;
    if (in->kind != HI_STAIRS || !stair_plan(g, in, &P)) return 0;
    *tread = P.t;
    const float s = P.s, w = P.w, D = P.D, ins = -INTO_WALL_M;
#define FL(XA, YA, H, S, O0, O1, OUT) do { HFlight *f_ = &fl[(*nfl)++]; f_->xa = (XA); f_->ya = (YA); f_->h = (H); f_->s = (S); \
                                          f_->o0 = (O0); f_->o1 = (O1); f_->n = P.n; f_->outer = (OUT); } while (0)
#define LA(A, B, Y, XF) do { HLanding *l_ = &la[(*nla)++]; l_->x0 = fminf((A), (B)); l_->x1 = fmaxf((A), (B)); l_->y = (Y); \
                             l_->d = D; l_->xFree = (XF); } while (0)
    if (!P.dog) {
        FL(P.xf, P.yA[0], P.h[0], s, ins, w, 1);
        /* the landing at the door, its free end away from the flight; a hung one's bottom landing beyond its foot */
        LA(in->door - P.Lh, in->door + P.Lh, floor_y(g, in->s1), in->door + s * P.Lh);
        if (P.hung) LA(P.xb0, P.xb1, P.yA[0], P.xb0);
    } else
        for (int c = 0; c < P.nclimb; ++c) {
            const float ya = P.yA[c], h = P.h[c], half = ya + (float)P.n * h, top = ya + 2.0f * (float)P.n * h;
            /* out in the outer lane, the half landing over both lanes, back in the lane against the wall to the landing */
            FL(P.xf, ya, h, s, w + STAIR_GAP, D, 1);
            LA(P.xh0, P.xh1, half, P.xh1);
            FL(P.xh0, half, h, -s, ins, w, 0);
            LA(in->door - P.Lh, in->door + P.Lh, top, in->door - s * P.Lh);
        }
#undef FL
#undef LA
    return 1;
}

int home_insert_doors(const HGraph *g, const HInsert *in, int *k, float *x, int max) {
    return in->kind == HI_STAIRS ? stair_doors(g, in, k, x, max) : 0;
}

static void stair_members(MB *b, const HGraph *g, const HInsert *in) {
    StairPlan P;
    HFlight fl[6];
    HLanding la[6];
    int nf, nl;
    float t;
    if (!stair_plan(g, in, &P) || !home_stair_parts(g, in, fl, &nf, la, &nl, &t)) return;
    const float s = P.s, w = P.w, D = P.D, T = in->cage ? 0.08f : STAIR_LT;
    for (int i = 0; i < nf; ++i) flight_members(b, in, fl[i].xa, fl[i].ya, fl[i].n, fl[i].h, fl[i].s, t, fl[i].o0, fl[i].o1, fl[i].outer);
    for (int i = 0; i < nl; ++i) landing_members(b, in, la[i].x0, la[i].x1, la[i].y, la[i].d, la[i].xFree, 1);
    if (!P.dog) {
        /* a grounded steel landing on two posts at its outer corners (a concrete one cantilevers from the wall) */
        if (!P.hung && in->cage) {
            const float top = floor_y(g, in->s1);
            stair_post(b, in, in->door - P.Lh + 0.06f, D, 0.0f, top - T);
            stair_post(b, in, in->door + P.Lh - 0.06f, D, 0.0f, top - T);
        }
        return;
    }
    /* the spine between a dog-leg's lanes (solid upstands): under the first flight's soffit, up to the second's upstand */
    for (int c = 0; c < P.nclimb && !in->cage && in->screen; ++c) {
        const float ya = P.yA[c], h = P.h[c], half = ya + (float)P.n * h, top = ya + 2.0f * (float)P.n * h;
        const float p[4][2] = { { P.xf, ya < 0.01f ? 0.0f : ya - STAIR_V }, { P.xh0, half - h - STAIR_V },
                                { P.xh0, half + h + STAIR_UP }, { P.xf, top + STAIR_UP } };
        float hp[4][2], q[8][2];
        memcpy(hp, p, sizeof(hp));
        const int nh = hull(hp, 4, q);
        mb_poly(b, HMB_ELEV, HMR_UPSTAND, (const float (*)[2])q, nh, w, w + STAIR_GAP);
    }
    /* columns: under the half landings' far outer corner (to the top one), and the landing's free outer corner */
    if (!P.hung) {
        const float c = in->cage ? 0.08f : STAIR_COL;
        const float topHalf = P.yA[P.nclimb - 1] + (float)P.n * P.h[P.nclimb - 1] - T;
        stair_post(b, in, P.xh1 - s * 0.5f * c, D, 0.0f, topHalf);
        const float topLand = P.yA[P.nclimb - 1] + 2.0f * (float)P.n * P.h[P.nclimb - 1] - T;
        stair_post(b, in, in->door - s * (P.Lh - 0.5f * c), D, 0.0f, topLand);
    }
}

/* HM5c2c1: a stair tower's head over a flat roof runs back over the deck (its door out onto the roof), over a parapet's
 * coping only on a pitched roof behind one; 0 where it stops under the eaves */
float home_tower_back(const HGraph *g, const HInsert *in) {
    if (in->kind != HI_TOWER) return 0.0f;
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    return !tower_rises(m) ? 0.0f : m->roof == HR_FLAT ? 1.0f : 0.3f;
}

int home_insert_members(const HGraph *g, const HInsert *in, HMember *out, int max) {
    if (!home_insert_membered(in) || in->vol < 0 || in->unit < 0 || in->unit >= g->nunit) return 0;
    MB b = { out, 0, max, g, in };
    if (in->kind == HI_STAIRS) { stair_members(&b, g, in); return b.n; }
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    float win[64][4];
    const int nw = home_front_windows(g, in->vol, in->unit, win, 64);
    switch (in->kind) {
    case HI_FRAME:  frame_members(&b, g, in, y0, y1, win, nw); break;
    case HI_FINS:   fin_members(&b, g, in, y0, y1, win, nw); break;
    case HI_HEX:    hex_members(&b, g, in, y0); break;
    case HI_SCREEN: strip_members(&b, g, in, y0, y1, win, nw); break;
    default: break;
    }
    return b.n;
}

/* A member's bounds: x, y and out from the wall. */
static void member_bounds(const HMember *m, float *x0, float *x1, float *y0, float *y1, float *d0, float *d1) {
    float a = 1e9f, bb = -1e9f, c = 1e9f, d = -1e9f;
    for (int i = 0; i < m->n; ++i) { a = fminf(a, m->p[i][0]); bb = fmaxf(bb, m->p[i][0]); c = fminf(c, m->p[i][1]); d = fmaxf(d, m->p[i][1]); }
    *x0 = a; *x1 = bb;
    if (m->axis == HMB_ELEV) { *y0 = c; *y1 = d; *d0 = m->a0; *d1 = m->a1; }
    else { *y0 = m->a0; *y1 = m->a1; *d0 = c; *d1 = d; }
}

/* Whether a member's outline (in its own plane) meets the rectangle u0..u1, v0..v1 of that plane, by separating axes. */
static int poly_meets_rect(const HMember *m, float u0, float u1, float v0, float v1) {
    float a = 1e9f, b = -1e9f, c = 1e9f, d = -1e9f;
    for (int i = 0; i < m->n; ++i) { a = fminf(a, m->p[i][0]); b = fmaxf(b, m->p[i][0]); c = fminf(c, m->p[i][1]); d = fmaxf(d, m->p[i][1]); }
    if (b <= u0 + HEPS || a >= u1 - HEPS || d <= v0 + HEPS || c >= v1 - HEPS) return 0;
    for (int i = 0; i < m->n; ++i) {
        const int j = (i + 1) % m->n;
        const float nx = m->p[j][1] - m->p[i][1], ny = m->p[i][0] - m->p[j][0];   /* outward (anticlockwise) */
        const float lim = nx * m->p[i][0] + ny * m->p[i][1];
        const float lo = fminf(nx * u0, nx * u1) + fminf(ny * v0, ny * v1);
        if (lo >= lim - HEPS * sqrtf(nx * nx + ny * ny)) return 0;
    }
    return 1;
}

/* Whether a member meets the box x0..x1, y0..y1, out d0..d1 from the wall. */
static int member_meets(const HMember *m, float x0, float x1, float y0, float y1, float d0, float d1) {
    if (m->axis == HMB_ELEV) return m->a1 > d0 + HEPS && m->a0 < d1 - HEPS && poly_meets_rect(m, x0, x1, y0, y1);
    return m->a1 > y0 + HEPS && m->a0 < y1 - HEPS && poly_meets_rect(m, x0, x1, d0, d1);
}

/* HM5c2c1: the stretch along x of a member's part under height H (what a path, a car or a door's head meets under a flight
 * rising over it); 0 if none of it is that low. */
static int member_low(const HMember *m, float H, float *x0, float *x1) {
    float a = 1e9f, b = -1e9f;
    if (m->axis == HMB_PLAN) {
        if (m->a0 >= H) return 0;
        for (int i = 0; i < m->n; ++i) { a = fminf(a, m->p[i][0]); b = fmaxf(b, m->p[i][0]); }
    } else
        for (int i = 0; i < m->n; ++i) {
            const int j = (i + 1) % m->n;
            const float *p = m->p[i], *q = m->p[j];
            if (p[1] < H) { a = fminf(a, p[0]); b = fmaxf(b, p[0]); }
            if ((p[1] < H) != (q[1] < H)) {
                const float x = p[0] + (q[0] - p[0]) * (H - p[1]) / (q[1] - p[1]);
                a = fminf(a, x); b = fmaxf(b, x);
            }
        }
    if (b < a) return 0;
    *x0 = a; *x1 = b;
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


/* The door's shelter: a porch over it, the balcony over it, or a hung insertion (the unit's door rule, home_validate's) */
/* HM5c2c1: whether a grounded stair's landing on its first floor stands over the door (its soffit the door's shelter) */
static int stair_over_door(const HInsert *in, const HDoor *d) {
    if (in->kind != HI_STAIRS || in->wall != HIW_FRONT || in->s0 != 0 || in->depth < 0.6f - HEPS || fabsf(d->z - in->z) > HEPS) return 0;
    const float Lh = 0.5f * (in->r > STAIR_LAND ? in->r : STAIR_LAND);
    return d->x - 0.5f * d->width >= in->door - Lh + 0.05f - HEPS && d->x + 0.5f * d->width <= in->door + Lh - 0.05f + HEPS;
}

int home_door_insert(const HGraph *g, int u) {
    if (u < 0 || u >= g->nunit || g->unit[u].door < 0) return -1;
    const HDoor *d = &g->door[g->unit[u].door];
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (in->unit == u && stair_over_door(in, d)) return i;
        if (home_insert_membered(in) || home_insert_roof(in->kind) || in->unit != u || in->s0 != 1 || in->depth < 0.6f - HEPS || fabsf(d->z - in->z) > HEPS) continue;
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

/* HM5c2c1: a box along an insertion's wall (a0..a1 along it, d0..d1 out of it) in the row's frame (x, z) */
static void ins_box(const HInsert *in, float a0, float a1, float d0, float d1, float *x0, float *x1, float *z0, float *z1) {
    float px, pz, qx, qz;
    home_ins_point(in, a0, d0, &px, &pz);
    home_ins_point(in, a1, d1, &qx, &qz);
    *x0 = fminf(px, qx); *x1 = fmaxf(px, qx); *z0 = fminf(pz, qz); *z1 = fmaxf(pz, qz);
}

/* An insertion's boxes in the row's frame (x0, x1, y0, y1, z0, z1): a box's or a rounded bay's whole volume, a member kind's
 * members'. */
static int ins_boxes(const HGraph *g, const HInsert *in, float (*bx)[6], int max) {
    if (!home_insert_membered(in)) {
        float y0, y1;
        home_insert_span(g, in, &y0, &y1);
        const float q[6] = { in->x0, in->x1, y0, y1, in->z - in->depth, in->z };
        memcpy(bx[0], q, sizeof(q));
        return 1;
    }
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    int k = 0;
    for (int i = 0; i < n && k < max; ++i, ++k) {
        float a0, a1, d0, d1;
        member_bounds(&M[i], &a0, &a1, &bx[k][2], &bx[k][3], &d0, &d1);
        ins_box(in, a0, a1, d0, d1, &bx[k][0], &bx[k][1], &bx[k][4], &bx[k][5]);
    }
    return k;
}

/* A unit's insertions apart (0.3 m between them), and its door still sheltered. */
static const char *apart_fault(const HGraph *g, const HInsert *in, int n) {
    const int u = in->unit;
    for (int i = 0; i < n; ++i) {
        const HInsert *o = &g->ins[i];
        if (o == in || o->unit != u) continue;
        if (!home_insert_membered(in) && !home_insert_membered(o)) {
            if (overlap(o->x0, o->x1, in->x0, in->x1, 0.3f) && o->s0 <= in->s1 + 1 && in->s0 <= o->s1 + 1) return "apart";
            continue;
        }
        float A[HG_MAX_MEMBERS][6], B[HG_MAX_MEMBERS][6];
        const int na = ins_boxes(g, in, A, HG_MAX_MEMBERS), nb = ins_boxes(g, o, B, HG_MAX_MEMBERS);
        for (int p = 0; p < na; ++p)
            for (int q = 0; q < nb; ++q)
                if (overlap(A[p][0], A[p][1], B[q][0], B[q][1], 0.05f) && overlap(A[p][2], A[p][3], B[q][2], B[q][3], 0.05f) &&
                    overlap(A[p][4], A[p][5], B[q][4], B[q][5], 0.0f)) return "apart";
    }
    if (!door_sheltered(g, u)) return "door shelter";
    return NULL;
}

/* A member kind's rules (HM5c2b): every member within the slice and its reach, under the eaves; one on the ground in the
 * front yard's margin, off the door, the garden's paths, cars and trees' places, and the boundaries; nothing in front of the
 * wall under one (a porch, a bay, a canopy); no balcony in its way (they were taken in); none touching the wall in front of
 * a window (but the hexagons, whose glass is the new window); a hung one tied back to the wall. */
static const char *member_fault(const HGraph *g, const HInsert *in, const HVol *m, float a, float b) {
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    if (n < 2) return "members";
    float win[64][4];
    const int nw = home_front_windows(g, in->vol, in->unit, win, 64);
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    if (y1 - y0 < (in->kind == HI_FINS && in->shape == HIS_LOUVRE ? 1.2f : 1.6f)) return "eaves";
    const int street = home_style_street(g->style), u = in->unit;
    const float reach = in->z - reach_min(g), most = in->z - home_yard(g->region)->proj;
    int ties = 0, onGround = 0;
    for (int i = 0; i < n; ++i) {
        const HMember *e = &M[i];
        float x0, x1, ya, yb, d0, d1;
        member_bounds(e, &x0, &x1, &ya, &yb, &d0, &d1);
        const float party = in->kind == HI_FINS && in->shape != HIS_BLADE_SQ ? FIN_PARTY : INS_PARTY;
        if (x0 < a + party - HEPS || x1 > b - party + HEPS) return "party";
        if (yb > y1 + HEPS) return "eaves";
        const int ground = ya < 0.05f;
        if (ground) { if (street || d1 > most + HEPS) return "reach"; onGround = 1; }
        else if (d1 > reach + HEPS) return "reach";
        ties += d0 < 0.0f && e->role != HMR_GLASS;
        /* a window behind it: only a member standing off the wall may cross one */
        if (in->kind != HI_HEX && d0 < INS_OFF - HEPS)
            for (int w = 0; w < nw; ++w)
                if (member_meets(e, win[w][0], win[w][1], win[w][2], win[w][3], -1.0f, 0.36f)) return "window";
        /* nothing in front of the wall under it (or, on the ground, where it stands) */
        for (int k = 0; k < g->nvol; ++k) {
            const HVol *v = &g->vol[k];
            if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind))) continue;
            if (v->z0 > m->z0 - 0.01f || (v->unit != u && v->unit != HG_ROW_WIDE)) continue;
            const float vd = m->z0 - v->z0;
            if (member_meets(e, v->x0 - 0.05f, v->x1 + 0.05f, -100.0f, v->top + 0.15f, -0.01f, vd + 0.05f)) return ground ? "overlap" : "under";
            /* nor so near in front of it that its windows go (the shell's 0.35 m test from its face) */
            if (member_meets(e, v->x0 - 0.05f, v->x1 + 0.05f, v->y0, v->top, vd, vd + INS_OFF - HEPS)) return "window";
        }
        /* balconies of its unit in its way */
        for (int k = 0; k < g->nbalc; ++k) {
            const HBalcony *bc = &g->balc[k];
            if (bc->unit != u || home_balcony_carved(bc->kind)) continue;
            const float fl = home_balcony_floor(g, bc);
            if (member_meets(e, bc->x0 - 0.05f, bc->x1 + 0.05f, fl - BALC_SLAB - 0.05f, fl + 1.15f, -0.01f, bc->depth + 0.05f)) return "balcony";
        }
        if (!ground) continue;
        const float z0 = in->z - d1, z1 = in->z - d0;
        const HUnit *un = &g->unit[u];
        if (un->door >= 0) {
            const HDoor *dr = &g->door[un->door];
            if (overlap(dr->x - 0.5f * dr->width, dr->x + 0.5f * dr->width, x0, x1, 0.1f) && d0 < 1.6f) return "door";
        }
        for (int k = 0; k < g->nsite; ++k) {
            const HSite *s = &g->site[k];
            if (s->kind == HE_FRONT || s->kind == HE_REAR) continue;
            if (overlap(s->x0, s->x1, x0, x1, -HEPS) && overlap(s->z0, s->z1, z0, z1, -HEPS)) return "garden clear";
        }
        for (int k = 0; k < g->nbound; ++k) {
            const HBound *bd = &g->bound[k];
            if (!home_bound_drawn(g, bd)) continue;
            float bx0, bz0, bx1, bz1;
            home_bound_rect(g, bd, &bx0, &bz0, &bx1, &bz1);
            if (overlap(bx0, bx1, x0, x1, -HEPS) && overlap(bz0, bz1, z0, z1, -HEPS)) return "boundary";
        }
    }
    /* a frame or strip hung from the wall is tied back to it (on the ground it stands on its legs) */
    if ((in->kind == HI_FRAME || in->kind == HI_SCREEN || in->kind == HI_FINS) && !onGround && !ties) return "tie";
    (void)onGround;
    return NULL;
}

/* HM5c2c1: a stair's rules, in the row's frame (it may stand against the front, the back or a free side): its layout from the
 * ground; its wall one a stair may stand against (stair_wall), its doors and members within its run (0.2 m off a party wall
 * or a corner) and its reach (the yard's margin, the rear boundary, the side boundary), within its own plot; nothing standing
 * in its way (a porch, a bay, a wing, an outrigger, an extension, a stack); on the front, no balcony in its way (those it
 * meets were given up) and its doors never onto a carved one. What is under 2.2 m of it keeps off the paths, drives, cars,
 * sheds and boundaries (a flight rises over a path); nothing under the door's head and 0.15 m stands in the front door's way
 * (a door set back in a recess, a pilotis' core or a five-foot way's, keeps a way in 1.2 m wide through the recess's mouth),
 * nor under 2.6 m in front of a shop front. Not the members' window rule: a flight may stand in front of windows (the shell
 * leaves them uncut; the placing picks the run that costs fewest). */
static const char *stair_fault(const HGraph *g, const HInsert *in) {
    const int mi = in->vol, u = in->unit, front = in->wall == HIW_FRONT;
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    StairPlan P;
    if (n < 2 || in->s0 != 0 || !stair_plan(g, in, &P)) return "stair";
    float face, r0, r1, reach;
    if (!stair_wall(g, mi, u, in->wall, &face, &r0, &r1, &reach) || fabsf(in->z - face) > 0.01f) return "wall";
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const HUnit *un = &g->unit[u];
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    int fk[4];
    float fa[4];
    const int nd = stair_doors(g, in, fk, fa, 4);
    for (int k = 0; k < nd; ++k) {
        if (fa[k] - 0.5f * INS_DOOR_W < r0 + STAIR_PARTY - HEPS || fa[k] + 0.5f * INS_DOOR_W > r1 - STAIR_PARTY + HEPS) return "party";
        for (int i = 0; i < g->nbalc && front; ++i) {
            const HBalcony *bc = &g->balc[i];
            if (bc->unit == u && bc->storey == fk[k] && home_balcony_carved(bc->kind) && overlap(bc->x0, bc->x1, fa[k] - 0.5f, fa[k] + 0.5f, 0.1f)) return "balcony";
        }
    }
    const HDoor *dr = un->door >= 0 ? &g->door[un->door] : NULL;
    const int recessed = dr && dr->z >= m->z0 + 0.5f;
    int nlow = 0;
    float low[HG_MAX_MEMBERS][2];   /* its low parts in front of a recessed door's mouth (x) */
    for (int i = 0; i < n; ++i) {
        const HMember *e = &M[i];
        float a0, a1, ya, yb, d0, d1;
        member_bounds(e, &a0, &a1, &ya, &yb, &d0, &d1);
        if (a0 < r0 + STAIR_PARTY - HEPS || a1 > r1 - STAIR_PARTY + HEPS) return "party";
        if (yb > y1 + HEPS) return "eaves";
        if (d1 > reach + HEPS) return "reach";
        float x0, x1, z0, z1;
        ins_box(in, a0, a1, fmaxf(d0, 0.0f), d1, &x0, &x1, &z0, &z1);
        if (x0 < un->x0 + 0.05f - HEPS || x1 > un->x1 - 0.05f + HEPS || z0 < 0.05f - HEPS || z1 > un->depth - 0.05f + HEPS) return "plot";
        /* nothing standing in its way: a porch, a bay, a wing, an outrigger, an extension (a neighbour's too), a stack */
        for (int k = 0; k < g->nvol; ++k) {
            const HVol *v = &g->vol[k];
            if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind))) continue;
            if (yb < v->y0 - 0.05f || ya > v->top + 0.15f) continue;
            if (overlap(v->x0, v->x1, x0, x1, 0.1f) && overlap(v->z0, v->z1, z0, z1, 0.1f)) return "overlap";
        }
        for (int k = 0; k < g->nchim; ++k) {
            const HChimney *c = &g->chim[k];
            if (ya < c->top && overlap(c->x - 0.5f * c->w, c->x + 0.5f * c->w, x0, x1, 0.1f) && overlap(c->z - 0.5f * c->d, c->z + 0.5f * c->d, z0, z1, 0.1f)) return "chimney";
        }
        for (int k = 0; k < g->nbalc && front; ++k) {
            const HBalcony *bc = &g->balc[k];
            if (bc->unit != u || home_balcony_carved(bc->kind)) continue;
            const float fl = home_balcony_floor(g, bc);
            if (member_meets(e, bc->x0 - 0.05f, bc->x1 + 0.05f, fl - BALC_SLAB - 0.05f, fl + 1.15f, -0.01f, bc->depth + 0.05f)) return "balcony";
        }
        /* what is low of it */
        float la0, la1, lx0, lx1, lz0, lz1;
        if (dr && !recessed && member_low(e, g->plinth + dr->height + 0.15f, &la0, &la1)) {
            ins_box(in, la0, la1, fmaxf(d0, 0.0f), d1, &lx0, &lx1, &lz0, &lz1);
            if (overlap(dr->x - 0.5f * dr->width, dr->x + 0.5f * dr->width, lx0, lx1, 0.15f) && overlap(dr->z - 1.6f, dr->z, lz0, lz1, 0.0f)) return "door";
        }
        if (dr && recessed && front && d0 < 1.6f && nlow < HG_MAX_MEMBERS && member_low(e, 2.2f, &la0, &la1)) { low[nlow][0] = la0; low[nlow][1] = la1; nlow++; }
        if (un->shopX1 > un->shopX0 && member_low(e, g->plinth + 2.6f, &la0, &la1)) {
            ins_box(in, la0, la1, fmaxf(d0, 0.0f), d1, &lx0, &lx1, &lz0, &lz1);
            if (overlap(un->shopX0, un->shopX1, lx0, lx1, 0.1f) && overlap(m->z0 - 1.6f, m->z0, lz0, lz1, 0.0f)) return "shop";
        }
        if (!member_low(e, 2.2f, &la0, &la1)) continue;
        ins_box(in, la0, la1, fmaxf(d0, 0.0f), d1, &lx0, &lx1, &lz0, &lz1);
        for (int k = 0; k < g->nsite; ++k) {
            const HSite *st = &g->site[k];
            if (st->kind == HE_FRONT || st->kind == HE_REAR) continue;
            if (overlap(st->x0, st->x1, lx0, lx1, -HEPS) && overlap(st->z0, st->z1, lz0, lz1, -HEPS)) return "garden clear";
        }
        for (int k = 0; k < g->nbound; ++k) {
            const HBound *bd = &g->bound[k];
            if (!home_bound_drawn(g, bd)) continue;
            float bx0, bz0, bx1, bz1;
            home_bound_rect(g, bd, &bx0, &bz0, &bx1, &bz1);
            if (overlap(bx0, bx1, lx0, lx1, -HEPS) && overlap(bz0, bz1, lz0, lz1, -HEPS)) return "boundary";
        }
    }
    if (nlow) {
        /* the recess's mouth: a way in 1.2 m wide past the stair's low parts */
        float m0 = dr->x - 0.6f, m1 = dr->x + 0.6f;
        for (int i = 0; i < g->nvol; ++i) {
            const HVol *p = &g->vol[i];
            if (p->role == HV_PORCH && p->unit == u && home_porch_recessed(p->kind)) { m0 = fminf(m0, p->x0); m1 = fmaxf(m1, p->x1); }
        }
        float x = m0, gap = 0.0f;
        for (int pass = 0; pass < 2 * nlow + 1 && x < m1; ++pass) {
            float next = m1;
            int pushed = 0;
            for (int i = 0; i < nlow; ++i) {
                if (low[i][0] <= x + HEPS && low[i][1] > x) { x = low[i][1]; pushed = 1; }
                else if (low[i][0] > x) next = fminf(next, low[i][0]);
            }
            if (pushed) continue;
            gap = fmaxf(gap, fminf(next, m1) - x);
            x = next;
        }
        if (gap < 1.2f - HEPS) return "door";
    }
    return NULL;
}

/* What breaks a rule, or NULL: in checked against the graph and the insertions before it (n of them). */
static const char *ins_fault(const HGraph *g, const HInsert *in, int n) {
    const int mi = in->vol, u = in->unit;
    if (u < 0 || u >= g->nunit) return "unit";
    if (mi < 0 || mi >= g->nvol || g->vol[mi].role > HV_ROW) return "main";
    if (!home_insert_built(in->kind)) return "kind";
    if (home_insert_roof(in->kind)) return home_roof_fault(g, in, n);   /* HM5c2c2: home_insert_roof.c */
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    if (in->s1 < in->s0 || in->s1 >= m->storeys) return "storey";
    if (in->kind == HI_STAIRS) {
        const char *f = stair_fault(g, in);
        return f ? f : apart_fault(g, in, n);
    }
    if (in->wall != HIW_FRONT || fabsf(in->z - m->z0) > 0.01f) return "wall";
    float a, b;
    if (!unit_slice(g, mi, u, &a, &b)) return "slice";
    if (home_insert_membered(in)) {
        const char *f = member_fault(g, in, m, a, b);
        if (f) return f;
        return apart_fault(g, in, n);
    }
    if (in->x0 < a + INS_PARTY - HEPS || in->x1 > b - INS_PARTY + HEPS || in->x1 - in->x0 < 1.2f - HEPS) return "party";
    if (in->depth < (in->kind == HI_TOWER ? 1.4f : 0.5f) - HEPS) return "shallow";
    if (in->kind == HI_TOWER && in->s0 != 0) return "storey";
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
    return apart_fault(g, in, n);
}

const char *home_ins_fault(const HGraph *g, const HInsert *in, int n) { return ins_fault(g, in, n); }
int home_ins_boxes(const HGraph *g, const HInsert *in, float (*bx)[6], int max) { return ins_boxes(g, in, bx, max); }

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

/* HM5c2c1: whether unit u's front can hold a stair at all: a front a storey's straight flight and its landing long at its
 * steepest (a hung one's two landings), or, in a yard two lanes deep, a dog-leg's; a grounded one's yard a flight wide */
static int stair_room(const HGraph *g, int mi, int u, const HVol *m) {
    const float rise = g->plinth + g->storeyH;
    const int n = (int)ceilf(rise / STAIR_HMAX - 1e-4f), n2 = (int)ceilf(rise / (2.0f * STAIR_HMAX) - 1e-4f);
    (void)m;
    for (int wall = HIW_FRONT; wall <= HIW_EAST; ++wall) {
        float face, a0, a1, reach;
        if (!stair_wall(g, mi, u, wall, &face, &a0, &a1, &reach) || reach < 0.72f) continue;
        const float L = a1 - a0 - 2.0f * STAIR_PARTY;
        if (L >= STAIR_LAND + (float)(n - 1) * 0.22f) return 1;
        if (reach >= 2.0f * 0.72f + STAIR_GAP && L >= STAIR_LAND + (float)(n2 - 1) * 0.22f + 0.82f) return 1;
    }
    return 0;
}

/* Whether a unit's body can take a kind at all (fixed for every kind at HM5c2a, so the draw is the final table's: a later
 * step only builds what was drawn; HM5c2c1 gave stairs the room they need, so a unit too narrow or too shallow for one
 * draws its next kind). */
static int feasible(const HGraph *g, int kind, const HVol *m, int bay, float yard, int mi, int u) {
    const int st = m->storeys;
    switch (kind) {
    case HI_BOX: case HI_ROUND: return st >= 2 || bay >= 0;
    case HI_HEX: return st >= 2;
    /* HM5c2c1: from the ground to the first floor against a wall with room for it (never on a pavement) */
    case HI_STAIRS: return st >= 2 && stair_room(g, mi, u, m);
    case HI_FRAME: case HI_FINS: case HI_SCREEN: return 1;
    case HI_TOWER: return st >= 2 && yard >= 1.8f && !home_style_street(g->style);
    /* HM5c2c2: a flat main under four storeys or block's one-storey house (its deck cast); a room's deck or front slope */
    case HI_UNFINISHED: case HI_ROOFROOM: return home_roof_feasible(g, kind, m, mi, u);
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
        for (int k = 0; k < HI_COUNT; ++k) w[k] = feasible(g, k, m, bay, m->z0, mi, u) ? S->kind[k] : 0.0f;
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
static int clear_members(HGraph *g, const HInsert *in, BLog *log) {
    int took = 0;
    HMember M[HG_MAX_MEMBERS];
    /* HM5c2c1: a stair's landing over the door shelters it: the canopy there goes (HM5c2a's boxes' way) */
    const int du = in->unit >= 0 && in->unit < g->nunit ? g->unit[in->unit].door : -1;
    if (du >= 0 && stair_over_door(in, &g->door[du]))
        for (int i = g->nvol - 1; i >= 0; --i) {
            const HVol *v = &g->vol[i];
            if (v->role != HV_PORCH || v->kind != HP_CANOPY || v->unit != in->unit) continue;
            if (!overlap(v->x0, v->x1, g->door[du].x - 0.5f * g->door[du].width, g->door[du].x + 0.5f * g->door[du].width, 0.0f)) continue;
            home_repair(g, log, "insert", "unit %d: the stair's landing over its door is its canopy", in->unit);
            home_drop_vol(g, i);
        }
    for (int pass = 0; pass < 2; ++pass) {
        /* (the windows move when a balcony goes: French ones become a storey's own, so the members are drawn again) */
        const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
        for (int i = g->nbalc - 1; i >= 0 && in->wall == HIW_FRONT; --i) {
            const HBalcony *bc = &g->balc[i];
            if (bc->unit != in->unit || home_balcony_carved(bc->kind)) continue;
            const float fl = home_balcony_floor(g, bc);
            int hit = 0;
            for (int k = 0; k < n && !hit; ++k) hit = member_meets(&M[k], bc->x0 - 0.05f, bc->x1 + 0.05f, fl - BALC_SLAB - 0.05f, fl + 1.15f, -0.01f, bc->depth + 0.05f);
            if (!hit) continue;
            home_repair(g, log, "insert", "unit %d storey %d: its %s given up to the %s", in->unit, bc->storey, home_balcony_name(bc->kind), home_insert_name(in->kind));
            for (int k = i; k + 1 < g->nbalc; ++k) g->balc[k] = g->balc[k + 1];
            g->nbalc--;
            took++;
        }
    }
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    for (int i = g->nsite - 1; i >= 0; --i) {
        const HSite *t = &g->site[i];
        if (t->kind != HE_TREE) continue;
        int hit = 0;
        for (int k = 0; k < n && !hit; ++k) {
            float a0, a1, y0, y1, d0, d1, x0, x1, z0, z1;
            member_bounds(&M[k], &a0, &a1, &y0, &y1, &d0, &d1);
            ins_box(in, a0, a1, d0, d1, &x0, &x1, &z0, &z1);
            hit = y0 < 0.05f && overlap(t->x0, t->x1, x0, x1, 0.0f) && overlap(t->z0, t->z1, z0, z1, 0.0f);
        }
        if (!hit) continue;
        home_repair(g, log, "insert", "unit %d: a tree's place under its %s given up", in->unit, home_insert_name(in->kind));
        for (int k = i; k + 1 < g->nsite; ++k) g->site[k] = g->site[k + 1];
        g->nsite--;
        for (int k = 0; k < g->ndoor; ++k) if (g->door[k].path > i) g->door[k].path--;
    }
    return took;
}

static int clear_for(HGraph *g, const HInsert *in, int bay, BLog *log) {
    if (home_insert_membered(in)) return clear_members(g, in, log);
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
    /* HM5c2c1: a stair's door takes a window, so the unit's insertions laid out by its windows are checked again */
    for (int i = 0; i + 1 < t->nins && !f && in.kind == HI_STAIRS; ++i)
        if (t->ins[i].unit == in.unit) f = ins_fault(t, &t->ins[i], i);
    free(t);
    if (f) {
        if (!*why) *why = f;
        return 0;
    }
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
    /* Japan's box clad in hexagonal panels (HM5c2b; the sheet's panel 4): grey concrete, its joints hexagons */
    if (kind == HI_HEX) { in.shape = HIS_HEXCLAD; in.col = (uint8_t)((rng_float(&r) < 0.5f ? 4 : 2) | 0x10); }
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

/* ---------------------------------------------------------------------------
 * HM5c2b: placing frames, fins, hexagons and screen strips
 * ------------------------------------------------------------------------- */

/* Each style's forms, from its sheet's panels (the plan's "HM5c2b"). */
static int frame_shape(int style, float r) {
    static const float W[HS_COUNT][5] = {
        /*                portal chamfer lean  zigzag apex */
        [HS_POSTWAR]    = { 0.4f, 0.6f, 0.0f, 0.0f, 0.0f },   /* 5: frames round the bays, their corners cut */
        [HS_INTERWAR]   = { 0.0f, 0.5f, 0.5f, 0.0f, 0.0f },   /* 5: faceted aprons */
        [HS_ROWHOUSE]   = { 0.0f, 0.0f, 0.0f, 0.8f, 0.2f },   /* 3: the zigzag's N */
        [HS_SEU_VILLA]  = { 0.3f, 0.0f, 0.7f, 0.0f, 0.0f },   /* 5: an inclined frame */
        [HS_CHINA]      = { 0.5f, 0.0f, 0.5f, 0.0f, 0.0f },
        [HS_LINKHOUSE]  = { 0.5f, 0.0f, 0.5f, 0.0f, 0.0f },   /* 3: a portal, one leg raked */
        [HS_BUNGALOW]   = { 0.0f, 0.4f, 0.6f, 0.0f, 0.0f },   /* 3: an angled car porch frame */
        [HS_MENA_VILLA] = { 0.3f, 0.0f, 0.5f, 0.0f, 0.2f },   /* 5, 6: an iwan frame, an angled one */
        [HS_COURTYARD]  = { 0.6f, 0.0f, 0.4f, 0.0f, 0.0f },
        [HS_WALLED]     = { 0.0f, 0.0f, 0.3f, 0.3f, 0.4f },   /* 5: frames, Z and triangle */
        [HS_SELFBUILT]  = { 0.3f, 0.0f, 0.3f, 0.4f, 0.0f },   /* 3, 6 */
        [HS_COLONIAL]   = { 0.5f, 0.0f, 0.5f, 0.0f, 0.0f },   /* 3: a raked strut; 6 a frame */
    };
    float w[5];
    memcpy(w, W[style], sizeof(w));
    if (w[0] + w[1] + w[2] + w[3] + w[4] <= 0.0f) { w[0] = 0.4f; w[2] = 0.3f; w[1] = 0.15f; w[3] = 0.1f; w[4] = 0.05f; }
    const int k = pick_w(w, 5, r);
    return k < 0 ? HIS_PORTAL : HIS_PORTAL + k;
}
static int fins_shape(int style, float r) {
    switch (style) {
    case HS_JAPAN:    return r < 0.5f ? HIS_GRID : r < 0.8f ? HIS_LOUVRE : HIS_BLADE_SQ;   /* 2 the grid, 5 the crown */
    case HS_BUNGALOW: return HIS_BLADE_SQ;                                                /* 6 */
    case HS_WALLED:   return r < 0.5f ? HIS_BLADE : HIS_BLADE_SQ;
    default:          return HIS_BLADE;                                                   /* Georgian 3, townhouse 5 */
    }
}
static int strip_star(int style) {
    return style == HS_GEORGIAN || style == HS_VICTORIAN || style == HS_MENA_VILLA || style == HS_COURTYARD || style == HS_WALLED;
}
/* bare concrete in the sheets' tones: pale, greys, buff; Japan's grey */
static uint8_t member_colour(int style, float r) {
    static const uint8_t PALE[4] = { 0, 3, 0, 1 }, GREY[4] = { 2, 4, 1, 2 }, WARM[4] = { 0, 3, 7, 3 };
    const int k = (int)(r * 4.0f) & 3;
    if (style == HS_JAPAN || style == HS_CHINA || style == HS_KOREA || style == HS_POSTWAR) return GREY[k];
    if (home_style_region(style) == PLAN_REGION_MENA || home_style_region(style) == PLAN_REGION_SSA || home_style_region(style) == PLAN_REGION_SAS) return WARM[k];
    return PALE[k];
}

/* The bays to try, nearest the drawn one first. */
static int bay_order(int nb, int first, int span, int *out) {
    int n = 0;
    for (int dist = 0; dist < nb; ++dist)
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            if (dist == 0 && sgn > 0) continue;
            const int f = first + sgn * dist;
            if (f < 0 || f + span > nb) continue;
            out[n++] = f;
        }
    return n;
}

/* How far out what stands in front of unit u's wall over x0..x1 reaches (a porch, a veranda, a bay, a canopy): a frame or a
 * strip on the ground stands in front of it. 0 if nothing. */
static float front_clear(const HGraph *g, const HVol *m, int u, float x0, float x1) {
    float d = 0.0f;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        if (v->role <= HV_ROW || (v->role == HV_PORCH && home_porch_recessed(v->kind)) || v->z0 > m->z0 - 0.01f) continue;
        if ((v->unit != u && v->unit != HG_ROW_WIDE) || !overlap(v->x0, v->x1, x0, x1, 0.1f)) continue;
        d = fmaxf(d, m->z0 - v->z0);
    }
    return d;
}

/* The runs of unit u's front a..b clear of its insertions so far (0.45 m off each), longest first. */
static int free_runs(const HGraph *g, int u, float a, float b, float (*out)[2], int max) {
    float cut[HG_MAX_INS][2];
    int nc = 0;
    for (int i = 0; i < g->nins; ++i)
        if (g->ins[i].unit == u) { cut[nc][0] = g->ins[i].x0 - 0.45f; cut[nc][1] = g->ins[i].x1 + 0.45f; nc++; }
    for (int i = 1; i < nc; ++i)
        for (int j = i; j > 0 && cut[j][0] < cut[j - 1][0]; --j) { float t0 = cut[j][0], t1 = cut[j][1]; cut[j][0] = cut[j - 1][0]; cut[j][1] = cut[j - 1][1]; cut[j - 1][0] = t0; cut[j - 1][1] = t1; }
    int n = 0;
    float x = a;
    for (int i = 0; i <= nc && n < max; ++i) {
        const float e = i < nc ? cut[i][0] : b;
        if (fminf(e, b) - x >= 1.0f) { out[n][0] = x; out[n][1] = fminf(e, b); n++; }
        if (i < nc) x = fmaxf(x, cut[i][1]);
    }
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && out[j][1] - out[j][0] > out[j - 1][1] - out[j - 1][0]; --j) { float t0 = out[j][0], t1 = out[j][1]; out[j][0] = out[j - 1][0]; out[j][1] = out[j - 1][1]; out[j - 1][0] = t0; out[j - 1][1] = t1; }
    return n;
}

static void place_m(HGraph *g, int mi, int u, int kind, BLog *log) {
    Rng r = rng_make(hash_combine(g->unit[u].seed, 0x1C2C0u + (uint32_t)kind));
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return;
    const float a = s0, b = s0 + bw * (float)nb;
    HInsert in;
    memset(&in, 0, sizeof(in));
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = (uint8_t)kind; in.z = m->z0;
    in.face = HIF_CONCRETE; in.fin = HW_CONCRETE;
    in.col = member_colour(g->style, rng_float(&r));
    const int street = home_style_street(g->style), top = m->storeys - 1;
    const float yard = m->z0 - home_yard(g->region)->proj, hung = in.z - reach_min(g);
    const float groundR = rng_float(&r), tallR = rng_float(&r), wideR = rng_float(&r);
    int first = (int)(rng_float(&r) * (float)nb);
    if (first >= nb) first = nb - 1;
    const char *why = NULL;
    int order[16];
    switch (kind) {
    case HI_FRAME: {
        in.shape = (uint8_t)frame_shape(g->style, rng_float(&r));
        in.w = rng_range(&r, 0.32f, 0.45f); in.t = rng_range(&r, 0.3f, 0.38f);
        const float D = rng_range(&r, 0.85f, 1.2f);
        in.lean = in.shape == HIS_LEAN ? (rng_float(&r) < 0.5f ? 1.0f : -1.0f) * rng_range(&r, 0.5f, 1.0f) : 0.0f;
        in.screen = (uint8_t)(rng_float(&r) < 0.35f);
        const int span0 = nb >= 2 && (wideR < 0.5f || in.shape == HIS_APEX || in.shape == HIS_ZIGZAG) ? 2 : 1;
        /* on the ground first where the yard takes its legs (a one-storey house's always), else hung from the first floor */
        /* a one-storey street-line house's stands on corbels over the pavement (home_insert_span) */
        const int canGround = !street && yard >= in.t + INS_OFF + 0.05f, raised = top == 0;
        for (int pass = 0; pass < 2; ++pass) {
            const int ground = (pass == 0) == (canGround && (groundR < 0.6f || top == 0));
            if (ground && !canGround) continue;
            if (!ground && top < 1 && !raised) continue;
            in.s0 = (uint8_t)(ground || raised ? 0 : 1);
            in.raised = (uint8_t)(!ground && raised);
            const float lim = ground ? yard : hung;
            const int lo = in.s0 + (ground && top >= 1 ? 1 : 0);
            for (int s1 = top; s1 >= lo && s1 >= in.s0; --s1)
                for (int sp = span0; sp >= 1; --sp) {
                    const int no = bay_order(nb, first + sp > nb ? nb - sp : first, sp, order);
                    for (int o = 0; o < no; ++o) {
                        HInsert t = in;
                        t.s1 = (uint8_t)s1;
                        /* within its bays (its legs stand off the wall, so they may cross a window's edge; the next bay
                         * stays free for another insertion) */
                        t.x0 = fmaxf(a + INS_PARTY, s0 + bw * (float)order[o] + 0.05f);
                        t.x1 = fminf(b - INS_PARTY, s0 + bw * (float)(order[o] + sp) - 0.05f);
                        if (t.x1 - t.x0 < 1.4f) continue;
                        /* on the ground, in front of a porch or bay in its way (the bungalow's car porch frame) */
                        const float cd = ground || t.raised ? front_clear(g, m, u, t.x0, t.x1) : 0.0f;
                        t.depth = fminf(cd > 0.0f ? fmaxf(D, cd + INS_OFF + t.t) : D, lim);
                        if (t.depth - t.t < INS_OFF) t.t = fmaxf(0.25f, t.depth - INS_OFF);
                        if (t.depth - t.t < INS_OFF - HEPS || (cd > 0.0f && t.depth - t.t < cd + INS_OFF - HEPS)) continue;
                        if (try_add(g, t, -1, log, &why)) return;
                    }
                }
        }
        break;
    }
    case HI_FINS: {
        in.shape = (uint8_t)fins_shape(g->style, rng_float(&r));
        const int sh = in.shape;
        in.w = sh == HIS_BLADE ? rng_range(&r, 0.22f, 0.3f) : sh == HIS_BLADE_SQ ? rng_range(&r, 0.14f, 0.2f) : rng_range(&r, 0.18f, 0.25f);
        const float D = sh == HIS_BLADE ? rng_range(&r, 0.75f, 1.05f) : sh == HIS_LOUVRE ? rng_range(&r, 0.7f, 0.8f) :
                        sh == HIS_GRID ? rng_range(&r, 0.6f, 0.8f) : rng_range(&r, 0.68f, 0.8f);
        in.lean = sh == HIS_BLADE ? (rng_float(&r) < 0.5f ? 1.0f : -1.0f) * rng_range(&r, 0.45f, 0.85f) : 0.0f;
        /* the run: the whole front, else a half of it, else a bay; beside another insertion of the unit, the runs clear of it */
        float runs[12][2];
        int nr = 0, others = 0;
        for (int i = 0; i < g->nins; ++i) others += g->ins[i].unit == u;
        if (!others) {
            runs[nr][0] = s0; runs[nr][1] = b; nr++;
            if (nb >= 2) {
                const int h = (nb + 1) / 2;
                runs[nr][0] = s0; runs[nr][1] = s0 + bw * (float)h; nr++;
                runs[nr][0] = s0 + bw * (float)(nb - h); runs[nr][1] = b; nr++;
            }
        } else nr = free_runs(g, u, s0, b, runs, 4);
        if (sh != HIS_LOUVRE) {
            const int no = bay_order(nb, first, 1, order);
            for (int o = 0; o < no && nr < 12; ++o) { runs[nr][0] = s0 + bw * (float)order[o]; runs[nr][1] = runs[nr][0] + bw; nr++; }
        }
        const int canGround = !street && yard >= D + 0.05f, brise = sh == HIS_BLADE_SQ, raised = top == 0;
        for (int pass = 0; pass < 2; ++pass) {
            int ground;
            if (sh == HIS_LOUVRE) { if (pass || top < 1) continue; ground = 0; in.s0 = (uint8_t)top; }
            else {
                ground = (pass == 0) == (top == 0 || (sh != HIS_BLADE && canGround && groundR < 0.5f));
                if (ground && !canGround) continue;
                if (!ground && top < 1 && !raised) continue;
                in.s0 = (uint8_t)(ground || raised ? 0 : 1);
                in.raised = (uint8_t)(!ground && raised);
            }
            const float lim = ground ? yard : hung;
            in.depth = fminf(D, lim);
            if ((sh == HIS_LOUVRE || brise) && in.depth < INS_OFF + 0.25f) continue;
            for (int s1 = top; s1 >= in.s0 && s1 >= (sh == HIS_LOUVRE ? top : in.s0 + (top > in.s0 ? 1 : 0)); --s1)
                for (int k = 0; k < nr; ++k) {
                    HInsert t = in;
                    t.s1 = (uint8_t)s1;
                    t.x0 = runs[k][0]; t.x1 = runs[k][1];
                    if (brise) {
                        /* its ends off the party walls; on the ground, in front of a porch or veranda in its way */
                        t.x0 = fmaxf(t.x0, a + INS_PARTY); t.x1 = fminf(t.x1, b - INS_PARTY);
                        if (t.x1 - t.x0 < 1.0f) continue;
                        const float cd = ground || t.raised ? front_clear(g, m, u, t.x0, t.x1) : 0.0f;
                        if (cd > 0.0f) t.depth = fminf(fmaxf(D, cd + INS_OFF + 0.32f), lim);
                        if (cd > 0.0f && fmaxf(INS_OFF, t.depth - 0.32f) < cd + INS_OFF - HEPS) continue;
                    }
                    if (try_add(g, t, -1, log, &why)) return;
                }
        }
        break;
    }
    case HI_HEX: {
        in.shape = HIS_HONEYCOMB;
        in.w = rng_range(&r, 0.22f, 0.3f);
        in.depth = fminf(rng_range(&r, 0.5f, 0.7f), hung);
        if (top < 1 || in.depth < 0.35f) break;
        const float avail0 = b - a - 2.0f * INS_PARTY;
        for (int s0i = (top >= 3 && tallR < 0.3f ? 2 : 1); s0i >= 1; --s0i)
            for (int s1 = top; s1 >= s0i; --s1) {
                HInsert t = in;
                t.s0 = (uint8_t)s0i; t.s1 = (uint8_t)s1;
                float y0, y1;
                home_insert_span(g, &t, &y0, &y1);
                const float tall = y1 - fmaxf(floor_y(g, s0i), y0 + INS_SLAB);
                static const int CELLS[4] = { 5, 3, 2, 1 };
                for (int c = 0; c < 4; ++c) {
                    t.cells = (uint8_t)CELLS[c];
                    t.r = fminf(1.7f, tall / (CELLS[c] >= 2 ? 4.0f * 0.8660254f : 2.0f * 0.8660254f));
                    if (hex_width(t.cells, t.r) > avail0) t.r = avail0 / hex_width(t.cells, t.r) * t.r;
                    if (t.r < 0.85f) continue;
                    const float W = hex_width(t.cells, t.r), slack = avail0 - W;
                    t.x0 = a + INS_PARTY + slack * (0.25f + 0.5f * wideR);
                    t.x1 = t.x0 + W;
                    if (try_add(g, t, -1, log, &why)) return;
                }
            }
        break;
    }
    case HI_SCREEN: {
        in.shape = (uint8_t)(strip_star(g->style) && rng_float(&r) < 0.7f ? HIS_STRIP_STAR : HIS_STRIP);
        in.screen = (uint8_t)(rng_float(&r) < 0.5f);
        in.w = rng_range(&r, 0.16f, 0.22f); in.t = rng_range(&r, 0.18f, 0.22f);
        const float D = rng_range(&r, 0.62f, 0.75f);
        const int full = wideR < 0.5f;
        const float W = rng_range(&r, 1.0f, 1.5f);
        const int canGround = !street && yard >= D + 0.05f, raised = top == 0;
        for (int pass = 0; pass < 2; ++pass) {
            const int ground = (pass == 0) == (top == 0 || (canGround && groundR < 0.5f));
            if (ground && !canGround) continue;
            if (!ground && top < 1 && !raised) continue;
            in.s0 = (uint8_t)(ground || raised ? 0 : 1);
            in.raised = (uint8_t)(!ground && raised);
            const float lim = ground ? yard : hung;
            const int hi = tallR < 0.6f || top <= in.s0 + 1 ? top : in.s0 + 1;
            for (int s1 = hi; s1 >= in.s0 + (top > in.s0 ? 1 : 0); --s1) {
                const int no = bay_order(nb, first, 1, order);
                for (int o = 0; o < no; ++o) {
                    HInsert t = in;
                    t.s1 = (uint8_t)s1;
                    const float c = s0 + bw * ((float)order[o] + 0.5f), hw = full ? 0.5f * bw - 0.1f : 0.5f * fminf(W, bw - 0.2f);
                    t.x0 = fmaxf(a + INS_PARTY, c - hw); t.x1 = fminf(b - INS_PARTY, c + hw);
                    if (t.x1 - t.x0 < 0.8f) continue;
                    /* on the ground, in front of a porch, veranda or bay in its way */
                    const float cd = ground || t.raised ? front_clear(g, m, u, t.x0, t.x1) : 0.0f;
                    t.depth = fminf(cd > 0.0f ? fmaxf(D, cd + INS_OFF + t.t) : D, lim);
                    if (t.depth - t.t < INS_OFF) t.t = fmaxf(0.12f, t.depth - INS_OFF);
                    if (t.depth - t.t < INS_OFF - HEPS || (cd > 0.0f && t.depth - t.t < cd + INS_OFF - HEPS)) continue;
                    if (try_add(g, t, -1, log, &why)) return;
                }
            }
        }
        break;
    }
    default: return;
    }
    home_repair(g, log, "insert", "unit %d: no room on its front for a %s (%s), left off", u, home_insert_name(kind), why ? why : "?");
}

/* ---------------------------------------------------------------------------
 * HM5c2c1: placing stairs and stair towers
 * ------------------------------------------------------------------------- */

int home_insert_windows_lost(const HGraph *g, const HInsert *in) {
    HMember M[HG_MAX_MEMBERS];
    const int nm = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    float win[64][4];
    const int nw = home_wall_windows(g, in->vol, in->unit, in->wall, win, 64);
    int lost = 0;
    for (int w = 0; w < nw; ++w) {
        int hit = 0;
        for (int k = 0; k < nm && !hit; ++k) hit = member_meets(&M[k], win[w][0], win[w][1], win[w][2], win[w][3], -1.0f, 0.36f);
        lost += hit;
    }
    return lost;
}


/* Whether stair in keeps every rule with what stands in its way cleared (its unit's other insertions too: its door takes a
 * window), and the front windows it costs: those its doors take and those its flights stand in front of (the shell leaves
 * them uncut). */
static const char *stair_try(const HGraph *g, const HInsert *in, int *lost) {
    if (g->nins >= HG_MAX_INS) return "full";
    HGraph *t = (HGraph *)malloc(sizeof(HGraph));
    if (!t) abort();
    *t = *g;
    clear_for(t, in, -1, NULL);
    float win[64][4];
    const int before = home_wall_windows(t, main_vol(t), in->unit, in->wall, win, 64);
    HInsert c = *in;
    c.vol = (int8_t)main_vol(t);
    t->ins[t->nins++] = c;
    const char *f = ins_fault(t, &t->ins[t->nins - 1], t->nins - 1);
    for (int i = 0; i + 1 < t->nins && !f; ++i)
        if (t->ins[i].unit == c.unit) f = ins_fault(t, &t->ins[i], i);
    if (!f) *lost = before - home_wall_windows(t, c.vol, c.unit, c.wall, win, 64) + home_insert_windows_lost(t, &c);
    free(t);
    return f;
}

/* A unit's stair (the sheets: Korea 2 and 6, RCC 3, self-built 1, 3 and 5, shophouse 6, the Southern European villa's 3),
 * from the ground to the first floor (the second a time in three on a dog-leg). Against the front of a house with a front
 * yard or a free side (the side gap of a detached house, a semi's open side, a terrace's end unit: the user, 2026-11-23, "on
 * the side of the property rather than the front"), never on a pavement; where neither takes it, against the back (into the
 * rear yard, towards the lane: a mid-terrace unit's). The drawn shape and make first; of the walls, doors (the wall's window
 * places on the first floor, then every quarter metre) and ways that keep the rules, the one that costs fewest windows; a
 * side's flight rises away from the street (its foot by the pavement). */
static void place_stairs(HGraph *g, int mi, int u, BLog *log) {
    Rng r = rng_make(hash_combine(g->unit[u].seed, 0x1C2C0u + (uint32_t)HI_STAIRS));
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    HFinish fin;
    home_finish(g, u, &fin);
    const int top = m->storeys - 1, st = g->style;
    if (top < 1) return;
    HInsert in;
    memset(&in, 0, sizeof(in));
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = HI_STAIRS; in.face = HIF_CONCRETE;
    /* the make: steel in the self-built sheets (Self Built 5) and a few South and South-East Asian owners'; solid upstands
     * in Korea's (2) and the colonial and villa sheets' */
    const float steelP = st == HS_SELFBUILT ? 0.35f : st == HS_RCC || st == HS_SHOPHOUSE ? 0.15f : 0.05f;
    const float solidP = st == HS_KOREA ? 0.6f : st == HS_COLONIAL ? 0.5f : st == HS_SEU_VILLA ? 0.4f : 0.25f;
    in.cage = (uint8_t)(rng_float(&r) < steelP);
    const float solidR = rng_float(&r), colR = rng_float(&r), colR2 = rng_float(&r);
    in.screen = (uint8_t)(!in.cage && solidR < solidP);
    /* a steel stair black, in red oxide primer or dark green (H_TRIM): grey steel is lost against a grey wall */
    static const uint8_t STEEL[4] = { 2, 7, 2, 3 };
    const int warm = g->region == PLAN_REGION_SAS || g->region == PLAN_REGION_SEA || g->region == PLAN_REGION_LAT || g->region == PLAN_REGION_SEU;
    if (in.cage) { in.fin = HW_CONCRETE; in.col = STEEL[(int)(colR2 * 4.0f) & 3]; }
    else if (fin.wall == HW_RENDER && warm && colR < 0.5f) { in.fin = HW_RENDER; in.col = fin.wallCol; }   /* painted with the house */
    else { in.fin = HW_CONCRETE; in.col = member_colour(st, colR2); }
    in.w = rng_range(&r, 0.85f, 1.1f);
    in.t = in.cage || st == HS_SELFBUILT ? rng_range(&r, 0.21f, 0.23f) : rng_range(&r, 0.22f, 0.25f);
    const float sdir = rng_float(&r) < 0.5f ? 1.0f : -1.0f;
    const float dogP = st == HS_KOREA || st == HS_RCC || st == HS_SELFBUILT || in.cage ? 0.55f : 0.3f;
    const int dogFirst = rng_float(&r) < dogP, twoUp = rng_float(&r) < 0.4f;
    const int sidePref = rng_float(&r) < 0.5f ? HIW_WEST : HIW_EAST;   /* the side its owner would rather (a street's not all one way) */
    /* the front door: a front stair's landing over it is its shelter (Korea 6), a landing its width and a margin */
    const HDoor *fd = g->unit[u].door >= 0 && fabsf(g->door[g->unit[u].door].z - m->z0) < HEPS ? &g->door[g->unit[u].door] : NULL;
    static const int GROUP[2][3] = { { HIW_FRONT, HIW_WEST, HIW_EAST }, { HIW_BACK, -1, -1 } };
    const char *why = NULL;
    for (int grp = 0; grp < 2; ++grp)
        for (int pass = 0; pass < 2; ++pass) {
            const int dog = pass == 0 ? dogFirst : !dogFirst;
            for (int climbs = dog && twoUp && top >= 2 ? 2 : 1; climbs >= 1; --climbs) {
                HInsert best = in;
                int bestCost = 1 << 20;
                for (int wi = 0; wi < 3; ++wi) {
                    const int wall = GROUP[grp][wi];
                    float face, r0, r1, reach;
                    if (wall < 0 || !stair_wall(g, mi, u, wall, &face, &r0, &r1, &reach)) continue;
                    HInsert t = in;
                    t.wall = (uint8_t)wall; t.z = face;
                    t.shape = (uint8_t)(dog ? HIS_DOGLEG : HIS_STAIR);
                    t.s0 = 0; t.s1 = (uint8_t)climbs;
                    if (t.w > reach) t.w = fmaxf(0.72f, reach);   /* a narrower flight in a shallow yard or side gap */
                    if (dog) t.w = fmaxf(0.72f, fminf(fminf(t.w, 0.95f), 0.5f * (reach - STAIR_GAP)));
                    t.depth = dog ? 2.0f * t.w + STAIR_GAP : t.w;
                    if (t.depth > reach + HEPS) { if (!why) why = "reach"; continue; }
                    const int side = home_wall_side(wall);
                    /* the doors to try: over the front door (two margins), the wall's windows on the first floor, then every
                     * quarter metre along it */
                    float doors[96];
                    int nd = 0, over = 0;
                    if (wall == HIW_FRONT && fd) { doors[nd++] = fd->x; doors[nd++] = fd->x; over = 2; }
                    float win[64][4];
                    const int nw = home_wall_windows(g, mi, u, wall, win, 64);
                    const float f1 = floor_y(g, 1);
                    for (int w = 0; w < nw && nd < 96; ++w)
                        if (win[w][2] > f1 && win[w][2] < f1 + 1.5f) doors[nd++] = 0.5f * (win[w][0] + win[w][1]);
                    const int pref = nd;
                    for (float x = r0 + STAIR_PARTY + 0.5f * STAIR_LAND; x <= r1 - STAIR_PARTY - 0.5f * STAIR_LAND + HEPS && nd < 96; x += 0.25f) {
                        int near = 0;
                        for (int k = 0; k < pref; ++k) near |= fabsf(x - doors[k]) < 0.12f;
                        if (!near) doors[nd++] = x;
                    }
                    for (int d = 0; d < 2; ++d)
                        for (int o = 0; o < nd; ++o) {
                            HInsert c = t;
                            /* a side's flight rises from by the street to the back first */
                            c.lean = side ? (d ? -1.0f : 1.0f) : d ? -sdir : sdir;
                            c.door = doors[o];
                            c.r = o < over ? fmaxf(STAIR_LAND, fd->width + (o ? 0.3f : 0.6f)) : 0.0f;   /* the flight's low end off it */
                            StairPlan P;
                            if (!stair_plan(g, &c, &P)) continue;
                            stair_extent(&P, c.door, &c.x0, &c.x1);
                            if (c.x0 < r0 + STAIR_PARTY - HEPS || c.x1 > r1 - STAIR_PARTY + HEPS) { if (!why) why = "party"; continue; }
                            int lost = 0;
                            const char *f = stair_try(g, &c, &lost);
                            if (f) { if (!why) why = f; continue; }
                            /* the fewest windows; on a tie a window's place, the drawn way, the front over a side */
                            const int cost = lost * 6 + (o >= pref) * 3 + d * 2 + side * (1 + (wall != sidePref));
                            if (cost < bestCost) { bestCost = cost; best = c; }
                        }
                }
                if (bestCost < (1 << 20) && try_add(g, best, -1, log, &why)) return;
            }
        }
    home_repair(g, log, "insert", "unit %d: no room on a wall for a stair (%s), left off", u, why ? why : "?");
}

/* A unit's glass stair tower (Georgian 5, brownstone 6): a bay wide on the ground in the yard, the full height of the house
 * (past a flat roof or a parapet by its head); the bays nearest the drawn one first. */
static void place_tower(HGraph *g, int mi, int u, BLog *log) {
    Rng r = rng_make(hash_combine(g->unit[u].seed, 0x1C2C0u + (uint32_t)HI_TOWER));
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    if (nb <= 0) return;
    const float a = s0, b = s0 + bw * (float)nb;
    static const uint8_t STEEL[4] = { 2, 4, 4, 5 };
    HInsert in;
    memset(&in, 0, sizeof(in));
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = HI_TOWER; in.z = m->z0; in.face = HIF_GLASS;
    in.fin = HW_CONCRETE; in.col = STEEL[(int)(rng_float(&r) * 4.0f) & 3];
    in.s0 = 0; in.s1 = (uint8_t)(m->storeys - 1);
    in.screen = (uint8_t)(rng_float(&r) < 0.5f);   /* a riser pipe up its side (the brownstone's panel 6) */
    const float yard = m->z0 - home_yard(g->region)->proj;
    in.depth = fminf(rng_range(&r, 1.7f, 2.4f), yard);
    const float W = rng_range(&r, 1.9f, 2.5f);
    int first = (int)(rng_float(&r) * (float)nb);
    if (first >= nb) first = nb - 1;
    const char *why = in.depth < 1.4f ? "reach" : NULL;
    int order[16];
    const int no = in.depth >= 1.4f ? bay_order(nb, first, 1, order) : 0;
    /* a bay in its way is taken down for it (the brownstone's panel 6: the tower where the bay was) */
    const int bay = unit_bay(g, u, m);
    for (int o = 0; o < no; ++o) {
        HInsert t = in;
        const float c = s0 + bw * ((float)order[o] + 0.5f);
        t.x0 = fmaxf(a + INS_PARTY, c - 0.5f * W); t.x1 = fminf(b - INS_PARTY, c + 0.5f * W);
        if (t.x1 - t.x0 < 1.6f) continue;
        const int takes = bay >= 0 && overlap(g->vol[bay].x0, g->vol[bay].x1, t.x0, t.x1, 0.05f) ? bay : -1;
        if (takes >= 0) {
            /* over the bay's whole width (its stump never left beside it) */
            t.x0 = fmaxf(a + INS_PARTY, fminf(t.x0, g->vol[bay].x0 - 0.05f)); t.x1 = fminf(b - INS_PARTY, fmaxf(t.x1, g->vol[bay].x1 + 0.05f));
            if (t.x0 > g->vol[bay].x0 + HEPS || t.x1 < g->vol[bay].x1 - HEPS) continue;
        }
        if (try_add(g, t, takes, log, &why)) return;
    }
    home_repair(g, log, "insert", "unit %d: no room in its yard for a stair tower (%s), left off", u, why ? why : "?");
}

void home_inserts(HGraph *g, int level, BLog *log) {
    g->nins = 0;
    memset(g->insPlan, 0, sizeof(g->insPlan));
    const int mi = main_vol(g);
    if (g->empty || level >= 2 || mi < 0 || g->style < 0 || g->style >= HS_COUNT) return;
    draw_plans(g, mi);
    for (int u = 0; u < g->nunit; ++u)
        for (int k = 0; k < HI_COUNT; ++k) {
            if (!(g->insPlan[u] >> k & 1u) || !home_insert_built(k)) continue;
            /* Japan's hexagons are a clad box six times in ten (its panel 4), placed as a box */
            const int clad = k == HI_HEX && g->style == HS_JAPAN && hf(hash_combine(g->unit[u].seed, 0x1C2E0u), 1u) < 0.6f;
            if (k == HI_BOX || k == HI_ROUND || clad) place(g, main_vol(g), u, k, log);
            else if (k == HI_STAIRS) place_stairs(g, main_vol(g), u, log);
            else if (k == HI_TOWER) place_tower(g, main_vol(g), u, log);
            else if (home_insert_roof(k)) home_roof_place(g, main_vol(g), u, k, log);   /* HM5c2c2 */
            else place_m(g, main_vol(g), u, k, log);
        }
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
void hc_region_dress(int region, float *light, float *trail) { region_dress(region, light, trail); }

/* HM5c2c1: a steel rail up a flight's open edge `out` from the wall: posts every metre or so on the treads, a top rail and a
 * middle one along the slope */
/* a point a along a stair's wall, `out` out of it, in the row's frame */
static void wall_pt(const HInsert *in, double a, double out, double *x, double *z) {
    float fx, fz;
    home_ins_point(in, (float)a, (float)out, &fx, &fz);
    *x = fx; *z = fz;
}

static void flight_rail(DC *d, const HInsert *in, const HFlight *f, float t, double out, double dy) {
    float ex, ez, nx, nz;
    home_ins_axes(in, &ex, &ez, &nx, &nz);
    const int step = (int)fmax(1.0, floor(1.15 / t + 0.5));
    int at[32], np = 0;
    for (int i = 1; i <= f->n && np < 31; i += step) at[np++] = i;
    if (at[np - 1] != f->n) at[np++] = f->n;
    for (int k = 0; k < np; ++k) {
        const double a = f->xa + f->s * ((at[k] - 1) * t + 0.06), y = f->ya + at[k] * f->h;
        double x, z;
        wall_pt(in, a, out, &x, &z);
        push_at(d, x, y + dy, z, ex, ez);
        pm_scale(d->pm, 0.5f, STAIR_UP, 0.5f);
        pm_place(d->pm, PMK_R_FRAME_POST);
        hc_pop(d);
    }
    const double xa = f->xa + f->s * 0.06, ya = f->ya + f->h, xb = f->xa + f->s * ((f->n - 1) * t + 0.06), yb = f->ya + f->n * f->h;
    const double L = hypot(xb - xa, yb - ya);
    if (L < 0.3) return;
    double px, pz;
    wall_pt(in, xa, out, &px, &pz);
    for (int q = 0; q < 2; ++q) {
        const double up = q ? 0.48 : STAIR_UP;
        const double p[3] = { px, ya + up + dy, pz }, ey[3] = { (xb - xa) / L * ex, (yb - ya) / L, (xb - xa) / L * ez }, ez3[3] = { nx, 0.0, nz };
        hc_push_yz(d, p, ey, ez3);
        pm_scale(d->pm, 0.6f, (float)L, 0.6f);
        pm_place(d->pm, PMK_R_PIPE_FE);
        hc_pop(d);
    }
}

/* HM5c2c1: a stair's dressing: rails where it has no upstands (a steel stair's, a concrete one's on a kerb) up its flights'
 * open edges and round its landings; a bulkhead over each door; a pot or two on a concrete one's landings */
static void dress_stair(DC *d, const HInsert *in, int idx) {
    const HGraph *g = d->g;
    const int u = in->unit;
    const double dy = d->C->dy ? d->C->dy[u] : 0.0;
    HFlight fl[6];
    HLanding la[6];
    int nf, nl;
    float t;
    if (!home_stair_parts(g, in, fl, &nf, la, &nl, &t)) return;
    const uint32_t h = hash_combine(g->unit[u].seed, 0x1C3A0u + (uint32_t)idx * 7919u);
    const HCharSet *cs = &d->C->cs[u];
    const int rails = in->cage || !in->screen;
    float ex, ez, nx, nz;
    home_ins_axes(in, &ex, &ez, &nx, &nz);
    double x, z;
    for (int i = 0; i < nf && rails; ++i) {
        const HFlight *f = &fl[i];
        /* the open edge: a straight flight's or an outer lane's outside, the lane against the wall's spine side */
        flight_rail(d, in, f, t, f->o1 - 0.04, dy);
    }
    for (int i = 0; i < nl && rails; ++i) {
        const HLanding *l = &la[i];
        wall_pt(in, l->x0, l->d - 0.04, &x, &z);
        push_at(d, x, l->y + dy, z, ex, ez);
        hr_rail(d->pm, l->x1 - l->x0);
        hc_pop(d);
        const double xe = l->xFree < 0.5 * (l->x0 + l->x1) ? l->x0 + 0.04 : l->x1 - 0.04;
        wall_pt(in, xe, 0.0, &x, &z);
        push_at(d, x, l->y + dy, z, nx, nz);
        hr_rail(d->pm, l->d - 0.08f);
        hc_pop(d);
    }
    int fk[4];
    float fx[4];
    const int nd = home_insert_doors(g, in, fk, fx, 4);
    for (int k = 0; k < nd; ++k) {
        wall_pt(in, fx[k], 0.0, &x, &z);
        const double p[3] = { x, g->plinth + fk[k] * g->storeyH + INS_DOOR_H + 0.22 + dy, z };
        hc_push_wall(d, p, nx, nz);
        pm_place(d->pm, PMK_R_BULKHEAD);
        hc_pop(d);
    }
    for (int i = 0; i < nl && !in->cage && !cs->lean; ++i) {
        const HLanding *l = &la[i];
        if (hf(h, 30u + (uint32_t)i) >= 0.4f) continue;
        const double xe = l->xFree < 0.5 * (l->x0 + l->x1) ? l->x0 + 0.3 : l->x1 - 0.3;
        wall_pt(in, xe, 0.35, &x, &z);
        push_at(d, x, l->y + dy, z, ex, ez);
        pm_place(d->pm, hf(h, 40u + (uint32_t)i) < 0.3f ? PMK_R_POT_PALM : PMK_R_POT);
        hc_pop(d);
    }
}

/* The members' dressing (HM5c2b): a strip's lattice; a frame's planters trailing over its shelves, a screen in one of its
 * cells, lights under its head; plants over a hexagon's sill. Every member's box is kept clear by the retrofit layer. */
static void dress_members(DC *d, const HInsert *in, int idx) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    const int u = in->unit;
    const double dy = C->dy ? C->dy[u] : 0.0;
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    float y0, y1;
    home_insert_span(g, in, &y0, &y1);
    const uint32_t h = hash_combine(g->unit[u].seed, 0x1C2F0u + (uint32_t)idx * 7919u);
    const HCharSet *cs = &C->cs[u];
    const float keep = cs->upkeep == HK_RUNDOWN ? 0.5f : cs->upkeep == HK_TIRED ? 0.8f : 1.0f;
    float light, trail;
    region_dress(g->region, &light, &trail);
    const double D = in->depth, dc = in->depth - 0.5 * in->t;
    hc_begin(d, u);
    if (in->kind == HI_STAIRS) dress_stair(d, in, idx);
    if (in->kind == HI_SCREEN) {
        /* the lattice between the posts, in their plane */
        const double xa = in->x0 + in->w, xb = in->x1 - in->w;
        const double ya = y0 + (in->s0 == 0 ? 0.25 : in->w), yb = y1 - in->w;
        if (xb - xa > 0.3 && yb - ya > 0.5) {
            push_at(d, xa, ya + dy, in->z - dc, 1.0, 0.0);
            hr_breeze(d->pm, in->shape == HIS_STRIP_STAR ? 2 : in->screen, (float)(xb - xa), (float)(yb - ya));
            hc_pop(d);
        }
    }
    if (in->kind == HI_FRAME) {
        /* planters along the shelves' fronts */
        for (int k = 0; k < n && !cs->lean; ++k) {
            const HMember *e = &M[k];
            if (e->role != HMR_SHELF || e->axis != HMB_ELEV || e->a1 < D - 0.05f) continue;
            float x0, x1, ya, yb, d0, d1;
            member_bounds(e, &x0, &x1, &ya, &yb, &d0, &d1);
            if (yb > y1 - 0.25f || hf(h, 10u + (uint32_t)k) >= fminf(0.9f, trail * keep * 1.8f)) continue;
            const double len = fmin(x1 - x0 - 0.5, 0.8 + 1.8 * hf(h, 40u + (uint32_t)k));
            const double x = d->sx > 0.0 ? x1 - 0.25 : x0 + 0.25;
            const double p[3] = { x, yb + dy, in->z - d1 + 0.02 };
            hc_push_wall(d, p, 0.0, -1.0);
            hr_trail(d->pm, (float)len, 0.4f + 0.7f * hf(h, 70u + (uint32_t)k), home_trail_species(g->style, h + (uint32_t)k));
            hc_pop(d);
        }
        /* a screen in one storey's cell of a portal or a lean-to frame, at its plane */
        if (in->screen && (in->shape == HIS_PORTAL || in->shape == HIS_CHAMFER || in->shape == HIS_LEAN)) {
            float lv[8];
            int nl = 0;
            lv[nl++] = in->s0 == 0 ? 0.0f : y0 + fmaxf(in->w, 0.25f);
            for (int k = in->s0 + 1; k <= in->s1 && nl < 6; ++k) lv[nl++] = floor_y(g, k) - 0.2f;
            lv[nl++] = y1 - in->w;
            const int band = (int)(hf(h, 2u) * (float)(nl - 1)) % (nl - 1);
            double ya = lv[band] + (band ? 0.22 : 0.0), yb = lv[band + 1];
            double xa = in->x0 + in->w + 0.02, xb = in->x1 - in->w - 0.02;
            if (in->shape == HIS_LEAN) {
                /* clear of the raked leg over the whole band */
                const float L = fminf(fabsf(in->lean), 0.5f * (in->x1 - in->x0) - 1.2f * in->w);
                if (in->lean >= 0.0f) xb -= L + 0.1; else xa += L + 0.1;
            }
            if (hf(h, 3u) < 0.5f) { if (hf(h, 4u) < 0.5f) xb = 0.5 * (xa + xb); else xa = 0.5 * (xa + xb); }
            if (xb - xa > 0.6 && yb - ya > 0.8) {
                push_at(d, xa, ya + dy, in->z - dc, 1.0, 0.0);
                hr_breeze(d->pm, strip_star(g->style) ? 2 : (int)(h & 1u), (float)(xb - xa), (float)(yb - ya));
                hc_pop(d);
            }
        }
        /* downlights under its head */
        if (!cs->lean && in->shape != HIS_APEX && hf(h, 5u) < light * keep * (0.6f + 0.6f * g->affluence)) {
            const int nl = in->x1 - in->x0 > 2.6f ? 3 : 2;
            for (int k = 0; k < nl; ++k) {
                push_at(d, in->x0 + (in->x1 - in->x0) * (k + 1) / (nl + 1), y1 - in->w + dy, in->z - dc, 1.0, 0.0);
                pm_place(d->pm, PMK_R_DOWNLIGHT);
                hc_pop(d);
            }
        }
    }
    if (in->kind == HI_HEX && !cs->lean) {
        /* plants over some hexagons' lower sides (the Georgian sheet's panel 6) */
        for (int k = 0; k < n; ++k) {
            const HMember *e = &M[k];
            if (e->role != HMR_GLASS || hf(h, 20u + (uint32_t)k) >= fminf(0.8f, trail * keep * 1.6f)) continue;
            float x0, x1, ya, yb, d0, d1;
            member_bounds(e, &x0, &x1, &ya, &yb, &d0, &d1);
            const double len = 0.5 * (x1 - x0);
            const double x = d->sx > 0.0 ? 0.5 * (x0 + x1) + 0.5 * len : 0.5 * (x0 + x1) - 0.5 * len;
            const double p[3] = { x, ya + dy, in->z - D + 0.03 };
            hc_push_wall(d, p, 0.0, -1.0);
            hr_trail(d->pm, (float)len, 0.3f + 0.5f * hf(h, 50u + (uint32_t)k), home_trail_species(g->style, h ^ (uint32_t)k));
            hc_pop(d);
        }
    }
    hc_done(d);
    d->D->st.inserts++;
    /* the retrofit layer keeps clear of every member */
    for (int k = 0; k < n; ++k) {
        float a0, a1, ya, yb, d0, d1, x0, x1, z0, z1;
        member_bounds(&M[k], &a0, &a1, &ya, &yb, &d0, &d1);
        ins_box(in, a0, a1, d0, d1, &x0, &x1, &z0, &z1);
        const double lo[3] = { x0 - 0.08, ya + dy - 0.1, z0 - 0.1 }, hi[3] = { x1 + 0.08, yb + dy + 0.1, z1 + 0.05 };   /* (the front's pads as HM5c2b had them) */
        hc_occupy(d, lo, hi);
    }
}

void hs_inserts(DC *d) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    float light, trail;
    region_dress(g->region, &light, &trail);
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (home_insert_membered(in)) { dress_members(d, in, i); continue; }
        if (home_insert_roof(in->kind)) { hs_roof_insert(d, in, i); continue; }   /* HM5c2c2: home_insert_roof.c */
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
            hr_trail(d->pm, (float)len, 0.5f + 0.6f * hf(h, 5u), home_trail_species(g->style, h));
            hc_pop(d);
        }
        /* HM5c2c1: a stair tower's riser up its side (the brownstone's panel 6) and plants trailing over its cap (Georgian 5) */
        if (in->kind == HI_TOWER) {
            if (in->screen) {
                const double x = d->sx > 0.0 ? in->x1 + 0.12 : in->x0 - 0.12;
                push_at(d, x, 0.15 + dy, zf + 0.3, 1.0, 0.0);
                pm_scale(d->pm, 1.8f, (float)(y1 - dy - 0.45), 1.8f);
                pm_place(d->pm, PMK_R_PIPE_FE);
                hc_pop(d);
            }
            if (!cs->lean && hf(h, 6u) < trail * keep * 1.2f) {
                const double len = fmin(W - 0.2, 0.7 + 1.2 * hf(h, 7u));
                const double x = d->sx > 0.0 ? in->x1 - 0.05 : in->x0 + 0.05;   /* (the wall's frame runs to -x: box tops' as) */
                const double p[3] = { x, y1 + 0.14, zf - 0.08 };
                hc_push_wall(d, p, 0.0, -1.0);
                hr_trail(d->pm, (float)len, 0.8f + 1.4f * hf(h, 8u), home_trail_species(g->style, h + 7u));
                hc_pop(d);
            }
        }
        hc_done(d);
        d->D->st.inserts++;
        /* the retrofit layer keeps clear of it (a tower's head over the roof too) */
        const double zb = in->kind == HI_TOWER ? in->z + home_tower_back(g, in) + 0.1 : in->z + 0.05;
        const double lo[3] = { in->x0 - 0.1, y0 - 0.3, zf - 0.35 }, hi[3] = { in->x1 + 0.1, y1 + 0.3, zb };
        hc_occupy(d, lo, hi);
    }
    hs_stubs(d);   /* HM5c2c2: the starter bars on RCC and self-built roofs */
}
