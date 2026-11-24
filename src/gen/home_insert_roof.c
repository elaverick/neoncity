#include "home_char_internal.h"
#include "pmodel_retrofit.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The roof's retrofit insertions (residential roadmap HM5c2c2, ~/.claude/plans/residential-buildings.md "HM5c2c2"; the
 * user's calls of 2026-11-24; docs/residential-target.md "Unfinished storeys and roof rooms"). Each kind's use and the way
 * up to it first:
 *
 *   unfinished storey  the next storey, built as money comes (block 2, RCC, self-built): block or brick walls each to its
 *                      own height, holes where its windows will go, concrete columns cast to the next floor with their
 *                      bars out, no roof. The house's own stair carries on up to it; a one-storey house has a builder's
 *                      ladder against it. Block's one-storey houses have their pitched roof taken off and a flat deck cast
 *                      to build on (the sheet's panel 2; no parapet: the new walls are its edge).
 *   roof room          on a flat roof (Korea 4, courtyard 5, self-built 5, the bungalow's flat ones): a room on the terrace
 *                      (glass, a container, a screen room, a concrete room) with a door onto it, reached by the house's
 *                      stair to the roof; on a pitched one (Georgian 3, 4, 6; the Southern European villa's 4, 6; bungalow
 *                      4; colonial 4) a loft room as a box dormer rising through the front slope from the top storey's
 *                      ceiling, behind the gutter or the parapet, its top under the ridge, reached from inside.
 *   stubs              the RCC and self-built sheets' panel 1: the columns cast for a storey not built yet, their bars
 *                      out, on most of their flat roofs: part of the house as built (no insertion).
 *
 * The rules (home_roof_fault): an unfinished storey on its unit's whole slice of a flat deck, its walls on the parapets,
 * HOME_STOREYS_MAX at most; a room within its slice 0.3 m off a party wall, on a flat roof 1 m back from the front and
 * 0.4 m off the back; a dormer behind the gutter (0.5 m) or the parapet (0.6 m), its top 0.3 m under the ridge, inside the
 * hips at its top, clear of the stacks and of any roof run up into the front slope (a bay's, a porch's); both apart from
 * the unit's other insertions (a stair tower's head and the way out of its door).
 * ========================================================================= */

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }
static int overlap(float a0, float a1, float b0, float b1, float pad) { return a1 > b0 - pad && a0 < b1 + pad; }

#define ROOF_PARTY  0.30f   /* a room kept off a party wall or the main's end */
#define ROOM_FRONT  1.00f   /* a flat roof's room behind the front wall */
#define ROOM_BACK   0.40f   /* ...and off the back */
#define DORMER_SET  0.50f   /* a dormer's front behind the wall's face: behind the gutter */
#define DORMER_PARA 0.60f   /* ...behind a front parapet (its coping and the gutter behind it) */
#define DORMER_TOP  0.30f   /* its top under the ridge */
#define DORMER_RISE 2.20f   /* the least ridge over the wall plate a loft room needs */
#define DORMER_T    1.90f   /* the lowest a dormer's top stands over the wall plate */
#define ROOM_SLAB   0.20f   /* a concrete room's roof slab */
#define ROOM_HOOD   0.60f   /* ...out over a flat roof's room's door */
#define CONT_W      2.44f   /* a container's width, height and lengths (10 and 20 foot) */
#define CONT_H      2.59f
#define CONT_L10    2.99f
#define CONT_L20    6.06f
#define CONT_RIDGE  0.30f   /* how far a container set through a slope may stand past the ridge */
#define UNF_SILL    0.93f   /* an unfinished wall's heights over the deck: to the sills, to the heads (off the 5 cm steps a */
#define UNF_HEAD    2.21f   /* row's units and their parapets stand at, so no top meets a neighbour's in one plane) */

/* The makes of the sheets' roof rooms by style (concrete, glass, screen, container); the rest's concrete. */
static const float ROOM_FACE[HS_COUNT][HIF_COUNT] = {
    [HS_GEORGIAN]  = { 0.15f, 0.85f, 0.0f,  0.0f  },   /* 3, 4, 6: glass pavilions behind the parapet */
    [HS_SEU_VILLA] = { 0.35f, 0.45f, 0.2f,  0.0f  },   /* 4 a pergola room, 6 a glass room */
    [HS_KOREA]     = { 0.5f,  0.3f,  0.0f,  0.2f  },   /* 4 a room on the roof */
    [HS_BUNGALOW]  = { 0.3f,  0.0f,  0.0f,  0.7f  },   /* 4 a container through the roof */
    [HS_COURTYARD] = { 0.5f,  0.0f,  0.5f,  0.0f  },   /* 5 rooms on the roof terraces */
    [HS_SELFBUILT] = { 0.4f,  0.0f,  0.0f,  0.6f  },   /* 5 a container */
    [HS_COLONIAL]  = { 0.3f,  0.0f,  0.7f,  0.0f  },   /* 4 a screen room on the roof */
};

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

static int main_of(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Where a room may stand
 * ------------------------------------------------------------------------- */

typedef struct RoofSite {
    int   flat, hip, parapet;
    float sx0, sx1;      /* the unit's slice */
    float a0, a1;        /* where a room may stand along the front */
    float zf, zb;        /* a flat deck's room: from 1 m behind the front to 0.4 m off the back; a dormer: its front, the ridge */
    float y;             /* the deck, or the wall plate */
    float rise, tp, T;   /* a pitched main's ridge over its wall plate, tan(pitch), a loft room's top over the plate */
} RoofSite;

/* A flat deck a room fits on (2.2 m by 2.4 m at least), or a front slope a loft room rises through: a gable with its ridge
 * along the street or a hip, the ridge DORMER_RISE over the wall plate, the room's top inside the hips. */
static int roof_site(const HGraph *g, int mi, int u, RoofSite *s) {
    memset(s, 0, sizeof(*s));
    if (mi < 0 || mi >= g->nvol || u < 0 || u >= g->nunit) return 0;
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const HUnit *un = &g->unit[u];
    s->sx0 = fmaxf(m->x0, un->x0); s->sx1 = fminf(m->x1, un->x1);
    if (s->sx1 - s->sx0 < 1.0f) return 0;
    s->a0 = s->sx0 + ROOF_PARTY; s->a1 = s->sx1 - ROOF_PARTY;
    if (m->roof == HR_FLAT) {
        s->flat = 1;
        s->y = home_deck_y(m);
        s->zf = m->z0 + ROOM_FRONT; s->zb = m->z1 - ROOM_BACK;
        return s->a1 - s->a0 >= 2.2f - HEPS && s->zb - s->zf >= 2.4f - HEPS;
    }
    if ((m->roof != HR_GABLE && m->roof != HR_HIP) || !m->ridgeX) return 0;
    s->hip = m->roof == HR_HIP; s->parapet = m->parapet == 1;
    s->y = m->eaves; s->rise = m->top - m->eaves;
    s->tp = tanf(m->pitch * 0.017453293f);
    if (s->rise < DORMER_RISE - HEPS || s->tp < 0.2f) return 0;
    s->T = fminf(2.5f, s->rise - DORMER_TOP);
    /* on a hip, lower where that keeps a 2 m room inside the hips at its top */
    if (s->hip) s->T = fminf(s->T, (0.5f * (m->x1 - m->x0 - 2.0f) - 0.2f) * s->tp - 0.1f);
    if (s->T < DORMER_T - HEPS) return 0;
    s->zf = m->z0 + (s->parapet ? DORMER_PARA : DORMER_SET);
    s->zb = 0.5f * (m->z0 + m->z1);
    const float in = s->hip ? (s->T + 0.1f) / s->tp + 0.2f : ROOF_PARTY;
    s->a0 = fmaxf(s->a0, m->x0 + in); s->a1 = fminf(s->a1, m->x1 - in);
    return s->a1 - s->a0 >= 2.0f - HEPS && m->z0 + (s->T + 0.05f) / s->tp - s->zf >= 1.2f;
}

/* Block's one-storey detached house: its pitched roof may be taken off for a deck (the sheet's panel 2). */
static int block_deck(const HGraph *g, const HVol *m) {
    return g->style == HS_BLOCK && g->arch == HOME_DETACHED && m->role == HV_MAIN && m->storeys == 1 && (m->roof == HR_GABLE || m->roof == HR_HIP);
}

int home_roof_feasible(const HGraph *g, int kind, const HVol *m, int mi, int u) {
    if (kind == HI_UNFINISHED) return m->roof == HR_FLAT ? m->storeys < HOME_STOREYS_MAX : block_deck(g, m);
    RoofSite s;
    return kind == HI_ROOFROOM && roof_site(g, mi, u, &s);
}

/* ---------------------------------------------------------------------------
 * Heights and the layouts the shell, the dressing and home_lab share
 * ------------------------------------------------------------------------- */

void home_roof_span(const HGraph *g, const HInsert *in, float *y0, float *y1) {
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    if (in->kind == HI_UNFINISHED) { *y0 = home_deck_y(m); *y1 = *y0 + g->storeyH - 0.1f; return; }   /* the columns' top */
    if (in->raised) { *y0 = m->eaves - 0.3f; *y1 = m->eaves + in->t; return; }
    *y0 = home_deck_y(m); *y1 = *y0 + in->t;
}

float home_room_sill(const HGraph *g, const HInsert *in) {
    float y0, y1;
    home_roof_span(g, in, &y0, &y1);
    if (in->kind != HI_ROOFROOM || !in->raised) return y0;
    HVol own;
    const HVol *m = home_unit_vol(g, in->vol, in->unit, &own);
    return m->eaves + tanf(m->pitch * 0.017453293f) * (in->z - in->depth - m->z0);
}

int home_room_door(const HGraph *g, const HInsert *in, float *x) {
    if (in->kind != HI_ROOFROOM || in->raised || in->x1 - in->x0 < INS_DOOR_W + 0.7f) return 0;
    const int right = (int)(hash_combine(g->unit[in->unit].seed, 0xD00Du) & 1u);
    *x = right ? in->x1 - 0.3f - 0.5f * INS_DOOR_W : in->x0 + 0.3f + 0.5f * INS_DOOR_W;
    return 1;
}

int home_room_windows(const HGraph *g, const HInsert *in, float (*out)[4], int max) {
    if (in->kind != HI_ROOFROOM || in->face == HIF_GLASS || max < 2) return 0;
    float y0, y1, dx;
    home_roof_span(g, in, &y0, &y1);
    const float cov = home_room_sill(g, in);
    const int screen = in->face == HIF_SCREEN, cont = in->face == HIF_CONTAINER;
    float a = in->x0 + (cont ? 0.3f : 0.35f), b = in->x1 - (cont ? 0.3f : 0.35f);
    if (home_room_door(g, in, &dx)) {
        if (dx < 0.5f * (in->x0 + in->x1)) a = dx + 0.5f * INS_DOOR_W + 0.3f;
        else b = dx - 0.5f * INS_DOOR_W - 0.3f;
    }
    const float top = y1 - (cont ? 0.0f : ROOM_SLAB);
    const float sill = screen ? (in->raised ? cov + 0.2f : y0 + 0.15f) : in->raised ? cov + 0.25f : y0 + 0.9f;
    const float head = top - (screen ? 0.15f : 0.3f);
    if (head - sill < 0.6f || b - a < 0.6f) return 0;
    if (screen) { out[0][0] = a; out[0][1] = b; out[0][2] = sill; out[0][3] = head; return 1; }   /* the recess */
    const float L = b - a;
    if (L > 3.2f) {
        const float w = fminf(1.4f, 0.5f * L - 0.3f);
        for (int k = 0; k < 2; ++k) {
            const float c = a + L * (k ? 0.75f : 0.25f);
            out[k][0] = c - 0.5f * w; out[k][1] = c + 0.5f * w; out[k][2] = sill; out[k][3] = head;
        }
        return 2;
    }
    const float w = fminf(cont ? 1.4f : 2.4f, L), c = 0.5f * (a + b);
    out[0][0] = c - 0.5f * w; out[0][1] = c + 0.5f * w; out[0][2] = sill; out[0][3] = head;
    return 1;
}

int home_vol_cast(const HGraph *g, int vi) {
    for (int i = 0; i < g->nins; ++i)
        if (g->ins[i].kind == HI_UNFINISHED && g->ins[i].vol == vi && g->ins[i].cage) return 1;
    return 0;
}

float home_unf_full(const HGraph *g) { return g->storeyH - 0.45f; }

/* whether the unit across side `wall` (HIW_WEST, HIW_EAST) of an unfinished storey's slice already has that storey: its body
 * is the wall there */
static int side_taken(const HGraph *g, const HInsert *in, int wall) {
    const HVol *v = &g->vol[in->vol];
    const int west = wall == HIW_WEST;
    if (west ? in->x0 <= v->x0 + 0.01f : in->x1 >= v->x1 - 0.01f) return 0;   /* the main's own end: a free side */
    const int nb = in->unit + (west ? -1 : 1);
    if (nb < 0 || nb >= g->nunit) return 0;
    HVol own;
    return home_unit_vol(g, in->vol, nb, &own)->storeys > in->s0;
}

int home_unf_walls(const HGraph *g, const HInsert *in, HUnfWall *out) {
    if (in->kind != HI_UNFINISHED) return 0;
    const float zf = in->z - in->depth, zb = in->z;
    int n = 0;
    for (int w = HIW_FRONT; w <= HIW_EAST; ++w) {
        const int lev = in->cells >> (2 * w) & 3;
        if (!lev || (home_wall_side(w) && side_taken(g, in, w))) continue;
        HUnfWall *o = &out[n++];
        o->wall = (uint8_t)w; o->level = (uint8_t)lev;
        o->h = lev == 1 ? UNF_SILL : lev == 2 ? UNF_HEAD : home_unf_full(g);
        if (home_wall_side(w)) { o->face = w == HIW_WEST ? in->x0 : in->x1; o->a0 = zf; o->a1 = zb; }
        else { o->face = w == HIW_FRONT ? zf : zb; o->a0 = in->x0; o->a1 = in->x1; }
    }
    return n;
}

/* the lines across the front and the back a frame's columns stand on: the window rhythm's bay lines (home_front_bays) */
static int bay_lines(const HGraph *g, int mi, int u, float x0, float x1, float *at, int max) {
    float s0, bw;
    const int nb = home_front_bays(g, mi, u, &s0, &bw);
    int n = 0;
    if (nb <= 0) { at[n++] = x0; at[n++] = x1; return n; }
    for (int k = 0; k <= nb && n < max; ++k) at[n++] = s0 + bw * (float)k;
    return n;
}

/* an unfinished storey's sides: spans of 3.6 m at most */
static int side_spans(const HInsert *in) { return (int)ceilf(in->depth / 3.6f - 1e-3f); }

int home_unf_columns(const HGraph *g, const HInsert *in, float (*out)[2], int max) {
    if (in->kind != HI_UNFINISHED) return 0;
    const float c = 0.5f * UNF_COL, zf = in->z - in->depth, zb = in->z;
    const int tw = side_taken(g, in, HIW_WEST), te = side_taken(g, in, HIW_EAST);
    float at[24];
    const int na = bay_lines(g, in->vol, in->unit, in->x0, in->x1, at, 24);
    int n = 0;
    for (int k = 0; k < na; ++k) {
        if ((k == 0 && tw) || (k == na - 1 && te)) continue;
        const float x = k == 0 ? at[k] + c : k == na - 1 ? at[k] - c : at[k];
        if (n < max) { out[n][0] = x; out[n][1] = zf + c; n++; }
        if (n < max) { out[n][0] = x; out[n][1] = zb - c; n++; }
    }
    const int ns = side_spans(in);
    for (int s = 0; s < 2; ++s) {
        if (s ? te : tw) continue;
        const float x = s ? in->x1 - c : in->x0 + c;
        for (int k = 1; k < ns && n < max; ++k) { out[n][0] = x; out[n][1] = zf + in->depth * (float)k / (float)ns; n++; }
    }
    return n;
}

int home_unf_holes(const HGraph *g, const HInsert *in, const HUnfWall *w, float *mid, float *width, int max) {
    if (w->h < UNF_SILL + 0.4f) return 0;   /* a wall only to the sills: none yet */
    HFinish fin;
    home_finish(g, in->unit, &fin);
    int n = 0;
    if (!home_wall_side(w->wall)) {
        float s0, bw;
        const int nb = home_front_bays(g, in->vol, in->unit, &s0, &bw);
        const float ww = fminf(fin.winW, bw - 0.6f);
        if (nb <= 0 || ww < 0.5f) return 0;
        for (int b = 0; b < nb && n < max; ++b) { mid[n] = s0 + bw * ((float)b + 0.5f); width[n] = ww; n++; }
        return n;
    }
    /* a side's: a window in some of its spans (the shell's sides leave some blank); none on a party line (the neighbour's
     * own next storey may go up against it) */
    const HVol *v = &g->vol[in->vol];
    if (w->wall == HIW_WEST ? in->x0 > v->x0 + 0.01f : in->x1 < v->x1 - 0.01f) return 0;
    const int ns = side_spans(in);
    const float span = in->depth / (float)ns, ww = fminf(fin.winW, span - 0.8f);
    if (ww < 0.5f) return 0;
    for (int k = 0; k < ns && n < max; ++k) {
        if (hash_combine(g->unit[in->unit].seed, 0x51D1u + (uint32_t)(w->wall * 16 + k)) & 1u) continue;
        mid[n] = in->z - in->depth + span * ((float)k + 0.5f); width[n] = ww; n++;
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The stubs (RCC and self-built panel 1)
 * ------------------------------------------------------------------------- */

/* whether unit u's flat roof carries stubs: seven RCC and self-built roofs in ten, none under an unfinished storey */
static int stubbed(const HGraph *g, int mi, int u) {
    if ((g->style != HS_RCC && g->style != HS_SELFBUILT) || g->empty || mi < 0 || u < 0 || u >= g->nunit) return 0;
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    if (m->roof != HR_FLAT || m->parapet != 2) return 0;
    for (int i = 0; i < g->nins; ++i)
        if (g->ins[i].unit == u && g->ins[i].kind == HI_UNFINISHED) return 0;
    return hf(hash_combine(g->unit[u].seed, 0x57B5u), 1u) < 0.7f;
}

/* whether a stub at (x, z) stands where one of unit u's insertions is on the roof (a room, a tower's head and its way out) */
static int stub_blocked(const HGraph *g, int u, float x, float z) {
    for (int i = 0; i < g->nins; ++i) {
        const HInsert *in = &g->ins[i];
        if (in->unit != u) continue;
        if (in->kind == HI_ROOFROOM && x > in->x0 - 0.5f && x < in->x1 + 0.5f && z > in->z - in->depth - 0.5f && z < in->z + 0.5f) return 1;
        if (in->kind == HI_TOWER) {
            const float back = home_tower_back(g, in);
            if (back > 0.0f && x > in->x0 - 0.4f && x < in->x1 + 0.4f && z < in->z + back + 1.2f) return 1;
        }
    }
    return 0;
}

int home_stubs(const HGraph *g, int u, float (*out)[3], int max, float *deck, float *base) {
    const int mi = main_of(g);
    if (!stubbed(g, mi, u)) return 0;
    HVol own, own2;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const HUnit *un = &g->unit[u];
    *deck = home_deck_y(m);
    *base = m->top;   /* on the parapets' copings (and a party line's dividing upstand), flush with them, cutting none */
    const float sx0 = fmaxf(m->x0, un->x0), sx1 = fminf(m->x1, un->x1), c = 0.5f * STUB_W;
    const uint32_t h = hash_combine(un->seed, 0x57B6u);
    const float H = 1.0f + 0.6f * hf(h, 1u);
    /* a party corner: none where the neighbour stands taller (its wall is there); one on the line where it has as many storeys
     * (the west unit's, or the east one's where the west one has no stubs); else in from the line on our own parapet over the
     * lower neighbour */
    int side[2] = { 0, 0 };   /* west, east: 0 a free end (in), 1 on the line, 2 none */
    for (int s = 0; s < 2; ++s) {
        const int nb = u + (s ? 1 : -1);
        if (!(s ? sx1 < m->x1 - 0.01f : sx0 > m->x0 + 0.01f) || nb < 0 || nb >= g->nunit) continue;
        const int ns = home_unit_vol(g, mi, nb, &own2)->storeys;
        side[s] = ns > m->storeys ? 2 : ns < m->storeys ? 0 : s == 1 || !stubbed(g, mi, nb) ? 1 : 2;
    }
    float at[24];
    const int na = bay_lines(g, mi, u, sx0, sx1, at, 24);
    int n = 0;
    for (int k = 0; k < na; ++k) {
        const int end = k == 0 ? 0 : k == na - 1 ? 1 : -1;
        if (end >= 0 && side[end] == 2) continue;
        const float x = end < 0 || side[end] == 1 ? at[k] : end == 0 ? at[k] + c : at[k] - c;
        for (int b = 0; b < 2 && n < max; ++b) {
            const float z = b ? m->z1 - c : m->z0 + c;
            if (stub_blocked(g, u, x, z)) continue;
            out[n][0] = x; out[n][1] = z; out[n][2] = H + 0.3f * (hf(h, 10u + (uint32_t)(k * 2 + b)) - 0.5f);
            n++;
        }
    }
    return n;
}

/* ---------------------------------------------------------------------------
 * The rules
 * ------------------------------------------------------------------------- */

const char *home_roof_fault(const HGraph *g, const HInsert *in, int n) {
    const int mi = in->vol, u = in->unit;
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    if (in->s0 != m->storeys || in->s1 != in->s0) return "storey";
    if (in->wall != HIW_FRONT) return "wall";
    const HUnit *un = &g->unit[u];
    const float sx0 = fmaxf(m->x0, un->x0), sx1 = fminf(m->x1, un->x1), zf = in->z - in->depth;
    float y0, y1;
    home_roof_span(g, in, &y0, &y1);
    if (in->kind == HI_UNFINISHED) {
        if (m->roof != HR_FLAT || (m->parapet != 2 && !in->cage)) return "flat";
        if (in->s0 >= HOME_STOREYS_MAX) return "storeys";
        if (fabsf(in->x0 - sx0) > 0.01f || fabsf(in->x1 - sx1) > 0.01f || fabsf(in->z - m->z1) > 0.01f || fabsf(zf - m->z0) > 0.01f) return "slice";
    } else {
        RoofSite s;
        if (!roof_site(g, mi, u, &s) || s.flat == (in->raised != 0)) return "roof";
        /* a container set through a slope (bungalow 4) may stand over the hips and its top a little past the ridge */
        const int box = in->raised && in->face == HIF_CONTAINER;
        const float a0 = box ? fmaxf(s.sx0, m->x0) + ROOF_PARTY : s.a0, a1 = box ? fminf(s.sx1, m->x1) - ROOF_PARTY : s.a1;
        if (in->x0 < a0 - HEPS || in->x1 > a1 + HEPS || in->x1 - in->x0 < 2.0f - HEPS) return "party";
        if (in->r > 0.0f && (in->r > in->depth + HEPS || in->r > 0.5f * (in->x1 - in->x0) + HEPS)) return "radius";
        if (s.flat) {
            if (zf < s.zf - HEPS || in->z > s.zb + HEPS || in->depth < 2.0f - HEPS) return "deck";
        } else {
            if (zf < s.zf - HEPS) return "gutter";
            if (y1 > m->top + (box ? CONT_RIDGE : -DORMER_TOP) + HEPS || in->z > s.zb - (box ? 0.1f : 0.2f) + HEPS) return "ridge";
            if (in->depth < 1.0f - HEPS) return "shallow";
            if (s.hip && !box) {
                const float hin = (in->t + 0.1f) / s.tp;
                if (in->x0 < m->x0 + hin - HEPS || in->x1 > m->x1 - hin + HEPS) return "hip";
            }
            /* nothing run up into the front slope there: a bay's, a porch's, an outrigger's roof */
            for (int i = 0; i < g->nvol; ++i) {
                const HVol *v = &g->vol[i];
                if (v->role <= HV_ROW || v->top < m->eaves - 0.05f || v->z1 > m->z0 + 0.05f) continue;
                if (overlap(v->x0, v->x1, in->x0, in->x1, 0.3f)) return "under";
            }
        }
    }
    /* clear of the stacks */
    for (int i = 0; i < g->nchim; ++i) {
        const HChimney *c = &g->chim[i];
        if (c->top > y0 + 0.1f && overlap(c->x - 0.5f * c->w, c->x + 0.5f * c->w, in->x0, in->x1, 0.3f) &&
            overlap(c->z - 0.5f * c->d, c->z + 0.5f * c->d, zf, in->z, 0.3f)) return "chimney";
    }
    /* apart from the unit's other insertions: no box of either meets the other's (a stair tower's head with a metre behind it,
     * its door's way out onto the roof) */
    float A[1][6];
    home_ins_boxes(g, in, A, 1);
    for (int i = 0; i < n; ++i) {
        const HInsert *o = &g->ins[i];
        if (o == in || o->unit != u) continue;
        float B[HG_MAX_MEMBERS + 1][6];
        int nb = home_ins_boxes(g, o, B, HG_MAX_MEMBERS);
        const float back = home_tower_back(g, o);
        if (back > 0.0f) {
            float y0o, y1o;
            home_insert_span(g, o, &y0o, &y1o);
            const float q[6] = { o->x0 - 0.3f, o->x1 + 0.3f, y0o, y1o + 0.2f, o->z - o->depth, o->z + back + 1.0f };
            memcpy(B[nb++], q, sizeof(q));
        }
        for (int q = 0; q < nb; ++q)
            if (overlap(A[0][0], A[0][1], B[q][0], B[q][1], -0.02f) && overlap(A[0][2], A[0][3], B[q][2], B[q][3], -0.02f) &&
                overlap(A[0][4], A[0][5], B[q][4], B[q][5], -0.02f)) return "apart";
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * The placing
 * ------------------------------------------------------------------------- */

/* Block's deck (the sheet's panel 2): the pitched roof taken off a one-storey house and a flat deck cast in its place, no
 * parapet (the new storey's walls are its edge), its slab's edge a little proud. */
static void cast_deck(HGraph *g, int mi, int logged, BLog *log) {
    HVol *m = &g->vol[mi];
    if (logged) home_repair(g, log, "insert", "unit %d: its %s roof taken off, a deck cast to build up on", m->unit, home_roof_name(m->roof));
    m->roof = HR_FLAT; m->parapet = 0; m->overhang = 0.12f; m->pitch = 0.0f;
    home_vol_top(m);
}

/* Adds insertion in (after casting a deck for it if `cast`) if, in a copy of the graph, it keeps its rules (and, a deck
 * cast, every rule of the house); why keeps the first fault. */
static int roof_try(HGraph *g, const HInsert *in, int cast, BLog *log, const char **why) {
    if (g->nins >= HG_MAX_INS) { if (!*why) *why = "full"; return 0; }
    HGraph *t = (HGraph *)malloc(sizeof(HGraph));
    if (!t) abort();
    *t = *g;
    if (cast) cast_deck(t, in->vol, 0, NULL);
    t->ins[t->nins++] = *in;
    const char *f = home_ins_fault(t, &t->ins[t->nins - 1], t->nins - 1);
    if (!f && cast && !home_validate(t, NULL)) f = "cast";
    free(t);
    if (f) {
        if (!*why) *why = f;
        return 0;
    }
    if (cast) cast_deck(g, in->vol, 1, log);
    g->ins[g->nins++] = *in;
    return 1;
}

/* A unit's next storey, begun on its flat roof's deck (or on a deck cast for it): its walls each to its own height, block's
 * all round most times (its panel 2), the rest a wall at a time, the front first. */
static void place_unfinished(HGraph *g, int mi, int u, BLog *log) {
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    const HUnit *un = &g->unit[u];
    const int cast = m->roof != HR_FLAT;
    if (cast && !block_deck(g, m)) { home_repair(g, log, "insert", "unit %d: no flat roof to build its next storey on, left off", u); return; }
    Rng r = rng_make(hash_combine(un->seed, 0x1C2C0u + (uint32_t)HI_UNFINISHED));
    HInsert in;
    memset(&in, 0, sizeof(in));
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = HI_UNFINISHED; in.wall = HIW_FRONT; in.face = HIF_CONCRETE;
    in.s0 = in.s1 = m->storeys;
    in.x0 = fmaxf(m->x0, un->x0); in.x1 = fminf(m->x1, un->x1);
    in.z = m->z1; in.depth = m->z1 - m->z0;
    in.cage = (uint8_t)cast;
    in.screen = (uint8_t)(m->storeys == 1);   /* no stair up to its roof: a builder's ladder */
    static const float FRONT[4] = { 0.0f, 0.2f, 0.35f, 0.45f }, REST[4] = { 0.15f, 0.2f, 0.3f, 0.35f }, BLOCK[4] = { 0.0f, 0.1f, 0.15f, 0.75f };
    unsigned cells = 0;
    for (int w = HIW_FRONT; w <= HIW_EAST; ++w) {
        const float *W = g->style == HS_BLOCK ? BLOCK : w == HIW_FRONT ? FRONT : REST;
        int lev = pick_w(W, 4, rng_float(&r));
        if (lev < 0) lev = 3;
        cells |= (unsigned)lev << (2 * w);
    }
    in.cells = (uint8_t)cells;
    /* block's bare grey block (dark grey a time in five), self-built's hollow clay brick (its panel 1), RCC's either */
    const float makeR = rng_float(&r), greyR = rng_float(&r);
    if (g->style == HS_SELFBUILT || (g->style == HS_RCC && makeR < 0.5f)) { in.fin = HW_BRICK; in.col = 2; }
    else { in.fin = HW_BLOCK; in.col = (uint8_t)(greyR < 0.2f ? 1 : 0); }
    const char *why = NULL;
    if (roof_try(g, &in, cast, log, &why)) return;
    home_repair(g, log, "insert", "unit %d: its next storey can't be begun (%s), left off", u, why ? why : "?");
}

/* A unit's roof room: on its flat deck at the back (a container's length along the street), or a loft room as a box dormer
 * through its front slope; at an end of the slice or its middle, narrower before it is left off. */
static void place_room(HGraph *g, int mi, int u, BLog *log) {
    RoofSite s;
    if (!roof_site(g, mi, u, &s)) { home_repair(g, log, "insert", "unit %d: no deck or slope for a roof room, left off", u); return; }
    Rng r = rng_make(hash_combine(g->unit[u].seed, 0x1C2C0u + (uint32_t)HI_ROOFROOM));
    HVol own;
    const HVol *m = home_unit_vol(g, mi, u, &own);
    HInsert in;
    memset(&in, 0, sizeof(in));
    in.unit = (int8_t)u; in.vol = (int8_t)mi; in.kind = HI_ROOFROOM; in.wall = HIW_FRONT;
    in.s0 = in.s1 = m->storeys;
    in.raised = (uint8_t)!s.flat;
    int face = pick_w(ROOM_FACE[g->style], HIF_COUNT, rng_float(&r));
    if (face < 0) face = HIF_CONCRETE;
    static const uint8_t STEEL[4] = { 2, 4, 4, 5 };   /* black, grey, bronze (house_palette.glsl H_TRIM) */
    const float colR = rng_float(&r), cageR = rng_float(&r);
    in.screen = (uint8_t)(rng_float(&r) < 0.4f);
    const float wR = rng_float(&r), dR = rng_float(&r), tR = rng_float(&r), sideR = rng_float(&r), roundR = rng_float(&r);
    /* a container set through a slope (bungalow 4) needs its top within CONT_RIDGE of the ridge and its depth before it; else
     * the room is built of concrete. It may stand over the hips (the sheet's does). */
    if (face == HIF_CONTAINER && !s.flat && (s.y + CONT_H > m->top + CONT_RIDGE || s.zf + CONT_W > s.zb - 0.1f)) face = HIF_CONCRETE;
    in.face = (uint8_t)face;
    switch (face) {
    case HIF_GLASS: in.fin = HW_CONCRETE; in.col = STEEL[(int)(colR * 4.0f) & 3]; break;
    case HIF_CONTAINER: in.fin = HW_BLOCK; in.col = (uint8_t)((int)(colR * 6.0f) % 6); in.cage = (uint8_t)(cageR < 0.5f); break;
    default: in.fin = HW_CONCRETE; in.col = (uint8_t)((int)(colR * 8.0f) & 7); break;
    }
    const int box = face == HIF_CONTAINER && !s.flat;
    const float ra0 = box ? fmaxf(s.sx0, m->x0) + ROOF_PARTY : s.a0, ra1 = box ? fminf(s.sx1, m->x1) - ROOF_PARTY : s.a1;
    float W, D, T;
    if (face == HIF_CONTAINER) { W = ra1 - ra0 >= CONT_L20 && wR < 0.5f ? CONT_L20 : CONT_L10; D = CONT_W; T = CONT_H; }
    else {
        W = 2.4f + 2.0f * wR;
        T = s.flat ? 2.5f + 0.3f * tR : s.T;
        D = s.flat ? 2.4f + 1.2f * dR : m->z0 + (T + 0.05f) / s.tp - s.zf;   /* a loft room's back where its top meets the slope */
    }
    if (s.flat) D = fminf(D, s.zb - s.zf);
    /* Georgian's drum (panel 4): a glass loft room's front rounded a time in three */
    const int round = face == HIF_GLASS && !s.flat && roundR < 0.34f;
    const char *why = NULL;
    for (int shrink = 0; shrink < 4; ++shrink) {
        float w = face == HIF_CONTAINER ? (shrink ? CONT_L10 : W) : fminf(W - 0.5f * (float)shrink, ra1 - ra0);
        if (face == HIF_CONTAINER && (shrink > 1 || w > ra1 - ra0 + HEPS)) { if (shrink) break; continue; }
        if (w < 2.0f - HEPS) break;
        /* an end of the slice (the drawn one first), then its middle */
        const float xs[3] = { sideR < 0.5f ? ra0 : ra1 - w, sideR < 0.5f ? ra1 - w : ra0, 0.5f * (ra0 + ra1 - w) };
        for (int p = 0; p < 3; ++p) {
            HInsert t = in;
            t.x0 = xs[p]; t.x1 = xs[p] + w;
            t.t = T;
            if (s.flat) { t.z = s.zb; t.depth = D; }
            else { t.z = s.zf + D; t.depth = D; }
            t.r = round ? fminf(fminf(t.depth, 0.5f * w) * 0.8f, 1.2f) : 0.0f;
            if (roof_try(g, &t, 0, log, &why)) return;
        }
    }
    home_repair(g, log, "insert", "unit %d: no room on its %s for a %s roof room (%s), left off", u, s.flat ? "deck" : "slope",
                home_insert_face_name(face), why ? why : "?");
}

void home_roof_place(HGraph *g, int mi, int u, int kind, BLog *log) {
    if (kind == HI_UNFINISHED) place_unfinished(g, mi, u, log);
    else if (kind == HI_ROOFROOM) place_room(g, mi, u, log);
}

/* ---------------------------------------------------------------------------
 * The dressing (home_insert.c's hs_inserts calls it)
 * ------------------------------------------------------------------------- */

static void push_up(DC *d, double x, double y, double z) {
    const double p[3] = { x, y, z };
    hc_push_wall(d, p, 0.0, -1.0);
}

/* starter bars on a column's or a stub's top at (x, y, z), `len` tall */
static void rebar(DC *d, double x, double y, double z, double len) {
    push_up(d, x, y, z);
    pm_scale(d->pm, 1.0f, (float)len, 1.0f);
    pm_place(d->pm, PMK_R_REBAR);
    hc_pop(d);
}

/* A builder's ladder up to the deck of a one-storey house: against a free side (the side gaps), else the front, its foot a
 * quarter of its rise out, clear of the windows, the door and whatever stands round the house. */
static void ladder(DC *d, const HInsert *in, float deck, double dy, uint32_t h) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    const HVol *m = &g->vol[in->vol];
    const double top = deck + 0.9, lean = 0.26, L = top * sqrt(1.0 + lean * lean), s = lean / sqrt(1.0 + lean * lean), c = 1.0 / sqrt(1.0 + lean * lean);
    const int first = (int)(h & 1u);
    for (int t = 0; t < 3; ++t) {
        const int wall = t == 2 ? HIW_FRONT : (t ^ first) ? HIW_EAST : HIW_WEST;
        const double mx = wall == HIW_WEST ? -1.0 : wall == HIW_EAST ? 1.0 : 0.0, mz = wall == HIW_FRONT ? -1.0 : 0.0;
        const double face = wall == HIW_WEST ? in->x0 : wall == HIW_EAST ? in->x1 : m->z0;
        const double a0 = home_wall_side(wall) ? m->z0 + 0.8 : in->x0 + 0.6, a1 = home_wall_side(wall) ? m->z1 - 0.8 : in->x1 - 0.6;
        const double out = lean * top;
        for (double a = a0; a <= a1 + 1e-6; a += 0.4) {
            const double fx = home_wall_side(wall) ? face + mx * out : a, fz = home_wall_side(wall) ? a : face + mz * out;
            /* its box, and nothing solid along it a hand off the wall */
            const double ex = home_wall_side(wall) ? 0.0 : 1.0, ez = home_wall_side(wall) ? 1.0 : 0.0;
            const double lo[3] = { fmin(fx, fx - mx * out) - 0.3 * ex - 0.05, dy, fmin(fz, fz - mz * out) - 0.3 * ez - 0.05 };
            const double hi[3] = { fmax(fx, fx - mx * out) + 0.3 * ex + 0.05, top + dy, fmax(fz, fz - mz * out) + 0.3 * ez + 0.05 };
            if (!hc_free(d, lo, hi)) continue;
            int clear = 1;
            for (int k = 0; k <= 6 && clear; ++k) {
                const double f = k / 6.0, q[3] = { fx - mx * out * f + mx * 0.12, dy + 0.1 + (top - 0.2) * f, fz - mz * out * f + mz * 0.12 };
                clear = hs_inside(C, q, -1, 0.0) < 0;
            }
            const double mm[2] = { mx, mz };
            if (!clear || hc_window_near(d, mm, home_wall_side(wall) ? mx * face : mz * face, home_wall_side(wall) ? (mx < 0 ? -a : a) : (mz < 0 ? a : -a), dy, top + dy, 0.35)) continue;
            const double p[3] = { fx, dy, fz }, ey[3] = { -mx * s, c, -mz * s }, ezv[3] = { mx * c, s, mz * c };
            hc_push_yz(d, p, ey, ezv);
            pm_scale(d->pm, 1.0f, (float)L, 1.0f);
            pm_place(d->pm, PMK_R_LADDER);
            hc_pop(d);
            hc_occupy(d, lo, hi);
            return;
        }
    }
}

void hs_roof_insert(DC *d, const HInsert *in, int idx) {
    const HGraph *g = d->g;
    const Ctx *C = d->C;
    const int u = in->unit;
    const double dy = C->dy ? C->dy[u] : 0.0;
    float y0, y1;
    home_roof_span(g, in, &y0, &y1);
    const uint32_t h = hash_combine(g->unit[u].seed, 0x1C3B0u + (uint32_t)idx * 7919u);
    const HCharSet *cs = &C->cs[u];
    const float keep = cs->upkeep == HK_RUNDOWN ? 0.5f : cs->upkeep == HK_TIRED ? 0.8f : 1.0f;
    float light, trail;
    hc_region_dress(g->region, &light, &trail);
    const double zf = in->z - in->depth;
    hc_begin(d, u);
    if (in->kind == HI_UNFINISHED) {
        /* starter bars out of every column; a ladder up to a one-storey house's deck */
        float col[48][2];
        const int nc = home_unf_columns(g, in, col, 48);
        for (int k = 0; k < nc; ++k) rebar(d, col[k][0], y1 + dy, col[k][1], 0.6 + 0.5 * hf(h, 1u + (uint32_t)k));
        hc_done(d);
        /* (the ladder before the storey's own box is taken: it leans on the storey's edge) */
        if (in->screen) { hc_begin(d, u); ladder(d, in, y0, dy, h); hc_done(d); }
        const double lo[3] = { in->x0 - 0.1, y0 + dy - 0.1, zf - 0.1 }, hi[3] = { in->x1 + 0.1, y1 + dy + 1.3, in->z + 0.1 };
        hc_occupy(d, lo, hi);
        d->D->st.inserts++;
        return;
    }
    const int flat = !in->raised, glass = in->face == HIF_GLASS, cont = in->face == HIF_CONTAINER;
    const double W = in->x1 - in->x0;
    float dx;
    const int door = home_room_door(g, in, &dx);
    if (door) {
        /* a downlight under the hood over its door (a concrete or screen room's), a bulkhead over a container's or a glass
         * room's; a pot or two by it on the deck */
        if (!glass && !cont) {
            push_up(d, dx, y1 - ROOM_SLAB + dy, zf - 0.5 * ROOM_HOOD);
            pm_place(d->pm, PMK_R_DOWNLIGHT);
            hc_pop(d);
        } else {
            const double p[3] = { dx, y0 + INS_DOOR_H + 0.22 + dy, zf };
            hc_push_wall(d, p, 0.0, -1.0);
            pm_place(d->pm, PMK_R_BULKHEAD);
            hc_pop(d);
        }
        for (int k = 0; k < 2 && !cs->lean; ++k) {
            if (hf(h, 20u + (uint32_t)k) >= 0.55f * keep) continue;
            const double x = dx + (k ? 0.85 : -0.85);
            if (x < in->x0 + 0.2 || x > in->x1 - 0.2) continue;
            push_up(d, x, y0 + dy, zf - 0.35);
            pm_place(d->pm, hf(h, 30u + (uint32_t)k) < 0.3f ? PMK_R_POT_PALM : PMK_R_POT);
            hc_pop(d);
        }
    }
    float win[4][4];
    const int nw = home_room_windows(g, in, win, 4);
    /* a screen room's breeze blocks in its recess's mouth (home_shell_ins.c cuts it: the same rectangle) */
    if (in->face == HIF_SCREEN && nw == 1 && win[0][1] - win[0][0] > 0.5f) {
        const double p[3] = { win[0][0], win[0][2] + dy, zf + 0.07 }, dir[3] = { 1.0, 0.0, 0.0 };
        hc_push_along(d, p, dir);
        hr_breeze(d->pm, in->screen, win[0][1] - win[0][0], win[0][3] - win[0][2]);
        hc_pop(d);
    }
    /* a container's windows barred */
    for (int k = 0; k < nw && cont && in->cage; ++k) {
        const double p[3] = { 0.5 * (win[k][0] + win[k][1]), win[k][2] + dy, zf };
        hc_push_wall(d, p, 0.0, -1.0);
        pm_scale(d->pm, win[k][1] - win[k][0], win[k][3] - win[k][2], 1.0f);
        pm_place(d->pm, PMK_H_GRILLE);
        hc_pop(d);
    }
    /* plants trailing over a flat room's roof's front edge (not a container's) */
    if (flat && !cont && !cs->lean && hf(h, 3u) < trail * keep) {
        const double len = fmin(W - 0.3, 0.8 + 1.6 * hf(h, 4u));
        const double x = d->sx > 0.0 ? in->x1 - 0.15 : in->x0 + 0.15;
        const double p[3] = { x, y1 + dy, zf - (glass ? 0.0 : ROOM_HOOD) + 0.02 };
        hc_push_wall(d, p, 0.0, -1.0);
        hr_trail(d->pm, (float)len, 0.5f + 0.6f * hf(h, 5u), home_trail_species(g->style, h));
        hc_pop(d);
    }
    hc_done(d);
    d->D->st.inserts++;
    /* the retrofit layer keeps clear of it, and of the way out of its door */
    const double lo[3] = { in->x0 - 0.15, y0 + dy - 0.1, zf - (flat && !glass && !cont ? ROOM_HOOD : 0.0) - 0.15 }, hi[3] = { in->x1 + 0.15, y1 + dy + 0.3, in->z + 0.15 };
    hc_occupy(d, lo, hi);
    if (door) {
        const double lo2[3] = { dx - 0.7, y0 + dy, zf - 1.3 }, hi2[3] = { dx + 0.7, y0 + dy + 2.3, zf };
        hc_occupy(d, lo2, hi2);
    }
}

void hs_stubs(DC *d) {
    const HGraph *g = d->g;
    for (int u = 0; u < g->nunit; ++u) {
        float st[24][3], deck, base;
        const int n = home_stubs(g, u, st, 24, &deck, &base);
        if (!n) continue;
        const double dy = d->C->dy ? d->C->dy[u] : 0.0;
        const uint32_t h = hash_combine(g->unit[u].seed, 0x57B7u);
        hc_begin(d, u);
        for (int k = 0; k < n; ++k) {
            const double len = 0.7 + 0.5 * hf(h, (uint32_t)k);
            rebar(d, st[k][0], deck + st[k][2] + dy, st[k][1], len);
            const double lo[3] = { st[k][0] - 0.3, deck + dy, st[k][1] - 0.3 }, hi[3] = { st[k][0] + 0.3, deck + st[k][2] + len + dy + 0.1, st[k][1] + 0.3 };
            hc_occupy(d, lo, hi);
        }
        hc_done(d);
    }
}
