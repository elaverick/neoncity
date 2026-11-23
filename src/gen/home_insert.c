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
                                              "honeycomb", "hex-clad", "strip", "star strip" };
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
            out[n][0] = c - 0.5f * ww - 0.12f; out[n][1] = c + 0.5f * ww + 0.12f;
            out[n][2] = sl - 0.12f;            out[n][3] = hd + 0.12f;
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

int home_insert_members(const HGraph *g, const HInsert *in, HMember *out, int max) {
    if (!home_insert_membered(in) || in->vol < 0 || in->unit < 0 || in->unit >= g->nunit) return 0;
    MB b = { out, 0, max, g, in };
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
int home_door_insert(const HGraph *g, int u) {
    if (u < 0 || u >= g->nunit || g->unit[u].door < 0) return -1;
    const HDoor *d = &g->door[g->unit[u].door];
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (home_insert_membered(in) || in->unit != u || in->s0 != 1 || in->depth < 0.6f - HEPS || fabsf(d->z - in->z) > HEPS) continue;
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

/* An insertion's boxes (x0, x1, y0, y1, out d0, d1): a box's or a rounded bay's whole volume, a member kind's members'. */
static int ins_boxes(const HGraph *g, const HInsert *in, float (*bx)[6], int max) {
    if (!home_insert_membered(in)) {
        float y0, y1;
        home_insert_span(g, in, &y0, &y1);
        const float q[6] = { in->x0, in->x1, y0, y1, 0.0f, in->depth };
        memcpy(bx[0], q, sizeof(q));
        return 1;
    }
    HMember M[HG_MAX_MEMBERS];
    const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
    int k = 0;
    for (int i = 0; i < n && k < max; ++i, ++k) member_bounds(&M[i], &bx[k][0], &bx[k][1], &bx[k][2], &bx[k][3], &bx[k][4], &bx[k][5]);
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
    return NULL;
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
    if (home_insert_membered(in)) {
        const char *f = member_fault(g, in, m, a, b);
        if (f) return f;
        return apart_fault(g, in, n);
    }
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
    return apart_fault(g, in, n);
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
static int clear_members(HGraph *g, const HInsert *in, BLog *log) {
    int took = 0;
    HMember M[HG_MAX_MEMBERS];
    for (int pass = 0; pass < 2; ++pass) {
        /* (the windows move when a balcony goes: French ones become a storey's own, so the members are drawn again) */
        const int n = home_insert_members(g, in, M, HG_MAX_MEMBERS);
        for (int i = g->nbalc - 1; i >= 0; --i) {
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
            float x0, x1, y0, y1, d0, d1;
            member_bounds(&M[k], &x0, &x1, &y0, &y1, &d0, &d1);
            hit = y0 < 0.05f && overlap(t->x0, t->x1, x0, x1, 0.0f) && overlap(t->z0, t->z1, in->z - d1, in->z - d0, 0.0f);
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
            hr_trail(d->pm, (float)len, 0.4f + 0.7f * hf(h, 70u + (uint32_t)k), h + (uint32_t)k);
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
            hr_trail(d->pm, (float)len, 0.3f + 0.5f * hf(h, 50u + (uint32_t)k), h ^ (uint32_t)k);
            hc_pop(d);
        }
    }
    hc_done(d);
    d->D->st.inserts++;
    /* the retrofit layer keeps clear of every member */
    for (int k = 0; k < n; ++k) {
        float x0, x1, ya, yb, d0, d1;
        member_bounds(&M[k], &x0, &x1, &ya, &yb, &d0, &d1);
        const double lo[3] = { x0 - 0.08, ya + dy - 0.1, in->z - d1 - 0.1 }, hi[3] = { x1 + 0.08, yb + dy + 0.1, in->z - d0 + 0.05 };
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
