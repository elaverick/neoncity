#include "home_char_internal.h"
#include "pmodel_house.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The houses' character layer (residential roadmap HM5): see home_shell.h.
 *
 *   the sets      style x affluence x upkeep: a row's set, owners' variations
 *   placing       frames on the mesh's side of a mirrored row; runs cut where
 *                 another solid stands in their way (sampled along them)
 *   the dressing  windows, bays, doors and their surrounds, porches, canopies,
 *                 eaves (gutters, downpipes, brackets, rafter tails), verges
 *                 (bargeboards, finials, knee braces, half-timbering), ridges
 *                 and hips, stacks, the front's reliefs, dormers, rooflights
 * ========================================================================= */

#define PI_D 3.14159265358979323846

/* ---------------------------------------------------------------------------
 * The sets
 * ------------------------------------------------------------------------- */

static int ch(uint32_t h, uint32_t k, float p) { return hash_float(hash_combine(h, k)) < p; }
static int pick(uint32_t h, uint32_t k, int n) { return (int)(hash_combine(h, k) % (uint32_t)n); }

/* A row's set: its style's, built as one (docs/residential-target.md "The character layer"). */
static void row_set(const HGraph *g, HCharSet *s) {
    const uint32_t h = hash_combine(g->seed, 0xC4A2u);
    const float aff = g->affluence;
    HFinish f;
    home_finish(g, 0, &f);
    memset(s, 0, sizeof(*s));
    s->frame = s->frameBack = HK_FRAME_CASEMENT;
    s->sill = HK_SILL_CONCRETE;
    s->head = s->headBack = HK_HEAD_NONE;
    s->door = HK_DOOR_4P;
    s->gutter = HK_GUTTER_HALF;
    s->rain = HKR_GREY;
    s->ridge = 1;
    s->post = HK_POST_SQUARE;
    s->canopy = HCC_FLAT;
    switch (g->style) {
    case HS_CRAFTSMAN:
        s->frame = s->frameBack = HK_FRAME_GRID; s->sill = HK_SILL_TIMBER; s->head = s->headBack = HK_HEAD_CASING;
        s->door = HK_DOOR_CRAFT; s->gutter = HK_GUTTER_OGEE; s->rain = ch(h, 1, 0.6f) ? HKR_WHITE : HKR_BROWN;
        s->eaves = HCE_RAFTERS; s->knees = 1; s->post = HK_POST_CRAFT; s->balustrade = ch(h, 2, 0.5f); s->canopy = HCC_GABLE;
        s->dormer = ch(h, 3, 0.3f) ? 1 + HK_DORMER_FLAT : 0;
        break;
    case HS_SPANISH:
        s->frame = s->frameBack = HK_FRAME_CASEMENT; s->sill = HK_SILL_NONE; s->door = HK_DOOR_4P;
        s->gutter = ch(h, 1, 0.5f) ? HK_GUTTER_HALF : 255; s->rain = HKR_BROWN; s->grilles = ch(h, 2, 0.5f) ? 1 : 0;
        s->boxes = ch(h, 3, 0.3f);   /* iron-bracketed planters under the street's windows */
        s->post = HK_POST_SQUARE; s->canopy = HCC_FLAT;
        break;
    case HS_RANCH:
        s->frame = HK_FRAME_PICTURE; s->frameBack = HK_FRAME_UPVC; s->sill = HK_SILL_TIMBER;
        s->door = ch(h, 1, 0.5f) ? HK_DOOR_6P : HK_DOOR_HALF; s->gutter = HK_GUTTER_OGEE; s->rain = ch(h, 2, 0.6f) ? HKR_WHITE : HKR_BROWN;
        s->shutters = ch(h, 3, 0.3f) ? HK_SHUT_PANEL : 0; s->canopy = HCC_FLAT;
        break;
    case HS_TRACT:
        s->frame = s->frameBack = ch(h, 1, 0.45f) ? HK_FRAME_UPVC : HK_FRAME_GRID; s->sill = HK_SILL_TIMBER;
        s->head = s->headBack = ch(h, 2, 0.4f) ? HK_HEAD_CASING : HK_HEAD_NONE;
        s->door = HK_DOOR_6P; s->gutter = HK_GUTTER_OGEE; s->rain = HKR_WHITE;
        s->shutters = ch(h, 3, 0.5f) ? HK_SHUT_LOUVRE : 0; s->canopy = HCC_GABLE;
        break;
    case HS_ROWHOUSE:
        s->frame = s->frameBack = HK_FRAME_SASH; s->sill = HK_SILL_STONE;
        s->head = f.wall == HW_STONE ? HK_HEAD_LINTEL : HK_HEAD_ARCH; s->headBack = HK_HEAD_ARCH;
        s->door = ch(h, 1, 0.5f) ? HK_DOOR_4P : HK_DOOR_6P; s->surround = ch(h, 2, 0.5f) ? HK_CASE_FANLIGHT : HK_CASE_NONE;
        s->gutter = HK_GUTTER_OGEE; s->rain = HKR_IRON; s->hopper = 1;
        s->eaves = HCE_BRACKETS; s->cornice = 1; s->string = ch(h, 3, 0.4f); s->plinth = 1;
        s->canopy = HCC_FLAT;
        break;
    case HS_TRIPLEDECKER:
        s->frame = s->frameBack = HK_FRAME_SASH; s->sill = HK_SILL_TIMBER; s->head = s->headBack = HK_HEAD_CASING;
        s->door = HK_DOOR_HALF; s->gutter = HK_GUTTER_OGEE; s->rain = HKR_WHITE;
        s->eaves = HCE_BRACKETS; s->post = ch(h, 1, 0.5f) ? HK_POST_TURNED : HK_POST_SQUARE; s->balustrade = 1;
        break;
    case HS_VICTORIAN:
        s->frame = s->frameBack = HK_FRAME_SASH; s->sill = HK_SILL_STONE; s->head = HK_HEAD_HOOD; s->headBack = HK_HEAD_ARCH;
        s->door = HK_DOOR_4P; s->surround = HK_CASE_FANLIGHT; s->gutter = ch(h, 1, 0.6f) ? HK_GUTTER_OGEE : HK_GUTTER_HALF;
        s->rain = HKR_IRON; s->hopper = 1; s->eaves = ch(h, 2, 0.6f) ? HCE_BRACKETS : HCE_PLAIN;
        s->barge = 1 + HK_BARGE_GOTHIC; s->finial = 1; s->ridge = ch(h, 3, 0.25f + 0.3f * aff) ? 2 : 1;
        s->string = ch(h, 4, 0.7f); s->plinth = ch(h, 5, 0.6f); s->quoins = ch(h, 6, 0.2f);
        s->post = HK_POST_TURNED; s->spandrels = 1; s->canopy = HCC_GABLE;
        break;
    case HS_GEORGIAN:
        s->frame = s->frameBack = HK_FRAME_GEORGIAN; s->sill = HK_SILL_STONE; s->head = HK_HEAD_KEYSTONE; s->headBack = HK_HEAD_LINTEL;
        s->door = HK_DOOR_6P; s->surround = ch(h, 1, 0.5f) ? HK_CASE_PEDIMENT : HK_CASE_FLAT;
        s->gutter = HK_GUTTER_OGEE; s->rain = HKR_IRON; s->hopper = 1;
        s->quoins = ch(h, 2, 0.5f); s->string = 1; s->plinth = 1; s->cornice = 1;
        s->canopy = HCC_FLAT;
        break;
    case HS_INTERWAR:
        s->frame = s->frameBack = HK_FRAME_CASEMENT; s->sill = ch(h, 1, 0.6f) ? HK_SILL_TIMBER : HK_SILL_CONCRETE;
        s->head = s->headBack = f.wall == HW_BRICK ? HK_HEAD_SOLDIER : HK_HEAD_NONE;
        s->door = HK_DOOR_HALF; s->gutter = HK_GUTTER_HALF; s->rain = ch(h, 2, 0.5f) ? HKR_IRON : HKR_GREY;
        s->barge = ch(h, 3, 0.7f) ? 1 + HK_BARGE_SCALLOP : 0; s->finial = s->barge && ch(h, 4, 0.5f);
        s->tudor = ch(h, 5, 0.5f); s->hung = ch(h, 6, 0.4f);
        if (s->tudor == 0) s->tudor = ch(h, 7, 0.2f);
        s->post = HK_POST_SQUARE; s->canopy = HCC_GABLE;
        break;
    case HS_POSTWAR:
        s->frame = s->frameBack = ch(h, 1, 0.6f) ? HK_FRAME_CASEMENT : HK_FRAME_UPVC; s->sill = HK_SILL_CONCRETE;
        s->hung = ch(h, 4, 0.25f);   /* a 1960s tile-hung panel between the storeys */
        s->head = s->headBack = HK_HEAD_CONCRETE; s->door = ch(h, 2, 0.6f) ? HK_DOOR_UPVC : HK_DOOR_HALF;
        s->rain = ch(h, 3, 0.5f) ? HKR_GREY : HKR_WHITE; s->canopy = HCC_FLAT;
        break;
    case HS_MODERN:
        s->frame = HK_FRAME_UPVC; s->frameBack = HK_FRAME_UPVC; s->sill = ch(h, 1, 0.5f) ? HK_SILL_CONCRETE : HK_SILL_NONE;
        s->door = HK_DOOR_UPVC; s->rain = HKR_GREY; s->canopy = HCC_FLAT;
        break;
    case HS_SEU_VILLA:
        s->frame = s->frameBack = HK_FRAME_CASEMENT; s->sill = HK_SILL_STONE; s->head = s->headBack = HK_HEAD_LINTEL;
        s->door = ch(h, 1, 0.5f) ? HK_DOOR_4P : HK_DOOR_6P; s->surround = ch(h, 2, 0.3f) ? HK_CASE_FANLIGHT : HK_CASE_NONE;
        s->rain = HKR_BROWN; s->shutters = ch(h, 3, 0.7f) ? (ch(h, 4, 0.7f) ? HK_SHUT_LOUVRE : HK_SHUT_PANEL) : 0;
        s->boxes = ch(h, 5, 0.4f); s->grilles = ch(h, 6, 0.4f) ? 1 : 0; s->quoins = ch(h, 7, 0.4f); s->string = ch(h, 8, 0.5f);
        break;
    case HS_TOWNHOUSE:
        s->frame = s->frameBack = HK_FRAME_SASH; s->sill = HK_SILL_STONE; s->head = s->headBack = HK_HEAD_LINTEL;
        s->door = HK_DOOR_6P; s->surround = ch(h, 1, 0.5f) ? HK_CASE_FANLIGHT : HK_CASE_NONE; s->rain = HKR_BROWN;
        s->shutters = ch(h, 2, 0.6f) ? HK_SHUT_LOUVRE : 0; s->boxes = ch(h, 3, 0.4f); s->string = 1; s->plinth = ch(h, 4, 0.5f);
        s->cornice = 1;
        break;
    case HS_JAPAN:
        s->frame = s->frameBack = ch(h, 1, 0.6f) ? HK_FRAME_UPVC : HK_FRAME_GRID; s->sill = HK_SILL_NONE;
        s->door = ch(h, 2, 0.5f) ? HK_DOOR_UPVC : HK_DOOR_HALF; s->rain = ch(h, 3, 0.5f) ? HKR_GREY : HKR_BROWN;
        s->grilles = ch(h, 4, 0.4f) ? 1 : 0;
        break;
    case HS_CHINA:
        s->frame = s->frameBack = HK_FRAME_GRID; s->sill = HK_SILL_CONCRETE; s->door = HK_DOOR_4P; s->grilles = ch(h, 1, 0.6f) ? 2 : 1;
        break;
    case HS_KOREA:
        s->frame = s->frameBack = HK_FRAME_UPVC; s->door = HK_DOOR_UPVC; s->grilles = ch(h, 1, 0.4f) ? 1 : 0;
        break;
    case HS_SHOPHOUSE:
        s->frame = s->frameBack = ch(h, 1, 0.5f) ? HK_FRAME_SASH : HK_FRAME_CASEMENT; s->sill = HK_SILL_STONE;
        s->shutters = ch(h, 2, 0.5f) ? HK_SHUT_LOUVRE : 0; s->grilles = ch(h, 3, 0.5f) ? 1 : 0; s->string = 1; s->plinth = 1;
        s->post = HK_POST_SQUARE;
        break;
    case HS_LINKHOUSE:
        s->frame = s->frameBack = HK_FRAME_UPVC; s->door = HK_DOOR_UPVC; s->grilles = 2;
        break;
    case HS_RCC:
        s->frame = s->frameBack = ch(h, 1, 0.5f) ? HK_FRAME_UPVC : HK_FRAME_CASEMENT; s->head = s->headBack = HK_HEAD_NONE;   /* its chajjas (HM5e1) */
        s->grilles = 2; s->gutter = 255;
        break;
    case HS_BUNGALOW:
        s->head = s->headBack = HK_HEAD_CONCRETE; s->grilles = ch(h, 1, 0.6f) ? 2 : 1;
        break;
    case HS_MENA_VILLA:
        s->frame = s->frameBack = ch(h, 1, 0.5f) ? HK_FRAME_UPVC : HK_FRAME_CASEMENT; s->sill = HK_SILL_STONE; s->head = s->headBack = HK_HEAD_LINTEL;
        s->grilles = 2; s->shutters = ch(h, 2, 0.2f) ? HK_SHUT_PANEL : 0; s->gutter = 255;
        break;
    case HS_COURTYARD:
        s->sill = HK_SILL_NONE; s->grilles = 2; s->gutter = 255;
        break;
    case HS_BLOCK: case HS_WALLED:
        s->frame = s->frameBack = ch(h, 1, 0.5f) ? HK_FRAME_UPVC : HK_FRAME_CASEMENT; s->grilles = 2;
        s->gutter = ch(h, 2, 0.4f) ? HK_GUTTER_HALF : 255;
        break;
    case HS_SELFBUILT:
        s->frame = s->frameBack = ch(h, 1, 0.6f) ? HK_FRAME_UPVC : HK_FRAME_CASEMENT; s->grilles = 2; s->door = ch(h, 2, 0.5f) ? HK_DOOR_UPVC : HK_DOOR_4P;
        break;
    case HS_COLONIAL:
        s->frame = s->frameBack = HK_FRAME_CASEMENT; s->sill = HK_SILL_STONE; s->head = s->headBack = HK_HEAD_LINTEL;
        s->door = HK_DOOR_6P; s->surround = ch(h, 1, 0.3f) ? HK_CASE_FANLIGHT : HK_CASE_NONE;
        s->shutters = ch(h, 2, 0.5f) ? HK_SHUT_PANEL : 0; s->grilles = ch(h, 3, 0.5f) ? 1 : 0; s->boxes = ch(h, 4, 0.2f);
        s->rain = HKR_BROWN;
        break;
    default: break;
    }
    /* ridges: rolls on tiles and slates (crested for some Victorian rows), none on shingles or sheet */
    if (f.roof == HRM_SHINGLE || f.roof == HRM_METAL) s->ridge = 0;
    /* poorer rows bar their ground floors in the regions where that is the custom; elsewhere only the run-down do */
    if (s->grilles && aff > 0.75f && ch(h, 9, 0.5f)) s->grilles = 1;
    /* back slopes: loft conversions are the owners' (home_char_set) */
}

void home_char_set(const HGraph *g, int unit, HCharSet *s) {
    row_set(g, s);
    const uint32_t o = hash_combine(unit >= 0 && unit < g->nunit ? g->unit[unit].seed : g->seed, 0xC4A3u);
    const float aff = g->affluence;
    const int uk = g->region == PLAN_REGION_UK, na = g->region == PLAN_REGION_NA;
    /* upkeep: mostly kept, some tired, a few run-down (never derelict); affluence tips it */
    const float kept = 0.45f + 0.35f * aff, r = hash_float(hash_combine(o, 1u));
    s->upkeep = r < kept ? HK_KEPT : r < kept + (1.0f - kept) * 0.72f ? HK_TIRED : HK_RUNDOWN;
    const float worn = s->upkeep == HK_KEPT ? 1.0f : s->upkeep == HK_TIRED ? 1.4f : 1.8f;
    /* replacement windows: plastic in Britain (most post-war, half the inter-war, a third of the Victorian), vinyl in
     * North America; the rest of the world fits what it fits */
    float rep = 0.0f;
    switch (g->style) {
    case HS_VICTORIAN: rep = 0.15f; break;
    case HS_GEORGIAN: rep = 0.06f; break;
    case HS_INTERWAR: rep = 0.25f; break;
    case HS_POSTWAR: rep = 0.2f; break;
    case HS_ROWHOUSE: case HS_TRIPLEDECKER: rep = 0.18f; break;
    case HS_CRAFTSMAN: rep = 0.1f; break;
    case HS_TRACT: case HS_RANCH: rep = 0.15f; break;
    case HS_SEU_VILLA: case HS_TOWNHOUSE: case HS_COLONIAL: rep = 0.12f; break;
    default: rep = 0.1f; break;
    }
    if (ch(o, 2u, fminf(0.9f, rep * worn))) {
        if (s->frame != HK_FRAME_PICTURE) s->frame = HK_FRAME_UPVC;
        s->frameBack = HK_FRAME_UPVC;
    } else if (s->frameBack != HK_FRAME_UPVC && ch(o, 3u, 0.12f * worn)) s->frameBack = HK_FRAME_UPVC;   /* the back done first */
    /* the door their own: a plastic one in Britain, a steel six-panel in North America */
    if (ch(o, 4u, 0.18f * worn)) s->door = uk ? HK_DOOR_UPVC : na ? HK_DOOR_6P : pick(o, 5u, 2) ? HK_DOOR_UPVC : HK_DOOR_HALF;
    /* their own gutters: plastic replacing iron, a colour of their own */
    if (s->rain == HKR_IRON && ch(o, 6u, 0.3f * worn)) s->rain = ch(o, 7u, 0.6f) ? HKR_GREY : HKR_WHITE;
    else if (ch(o, 8u, 0.12f)) s->rain = (uint8_t)pick(o, 9u, 4);
    if (s->rain != HKR_IRON) s->hopper = s->hopper && ch(o, 10u, 0.5f);
    /* shutters taken down, window boxes their own */
    if (s->shutters && ch(o, 11u, 0.2f * worn)) s->shutters = 0;
    s->boxes = ((s->boxes && ch(o, 12u, 0.6f)) || ((uk || na) && s->upkeep == HK_KEPT && ch(o, 21u, 0.12f))) && s->upkeep != HK_RUNDOWN;
    /* porch posts their own: a turned post for a plain one or the other way (not a craftsman's columns) */
    if (s->post != HK_POST_CRAFT && ch(o, 22u, 0.15f)) s->post = s->post == HK_POST_TURNED ? HK_POST_SQUARE : HK_POST_TURNED;
    if (s->post == HK_POST_TURNED && (uk || na) && g->style != HS_CRAFTSMAN) s->spandrels = s->spandrels || ch(o, 23u, 0.5f);
    /* grilles: the run-down bar their ground floor windows anywhere; elsewhere the region's custom stands */
    if (!s->grilles && s->upkeep == HK_RUNDOWN && ch(o, 13u, 0.4f)) s->grilles = 1;
    if (s->grilles == 1 && aff < 0.35f && ch(o, 14u, 0.3f)) s->grilles = 2;
    /* a canopy their own */
    if (ch(o, 15u, 0.12f)) s->canopy = s->canopy == HCC_GABLE ? HCC_FLAT : HCC_GABLE;
    /* the roof: a dormer on the street's slope by the row's own pattern (craftsman), loft conversions at the back,
     * rooflights and a soil vent */
    if (uk && ch(o, 16u, 0.2f + 0.15f * aff)) s->dormerBack = 1 + HK_DORMER_FLAT;
    else if ((g->style == HS_CRAFTSMAN || g->style == HS_TRACT) && ch(o, 17u, 0.1f)) s->dormerBack = 1 + HK_DORMER_GABLE;
    if (!s->dormer && (g->style == HS_VICTORIAN || g->style == HS_INTERWAR) && ch(o, 18u, 0.2f))
        s->dormer = 1 + (g->style == HS_VICTORIAN ? HK_DORMER_GABLE : HK_DORMER_HIP);
    if (!s->dormer && g->style == HS_TRACT && ch(o, 24u, 0.15f)) s->dormer = 1 + HK_DORMER_GABLE;   /* a Cape Cod's */
    s->rooflight = !s->dormerBack && ch(o, 19u, g->style == HS_MODERN ? 0.4f : uk || g->region == PLAN_REGION_SEU ? 0.2f : 0.05f);
    s->vent = (uk || na) && ch(o, 20u, 0.5f);
}

int home_char_frame(const HCharSet *cs, int front, int storey, double w) {
    int f = front ? cs->frame : cs->frameBack;
    if (f == HK_FRAME_PICTURE && !(front && storey == 0 && w >= 1.2)) f = cs->frameBack == HK_FRAME_PICTURE ? HK_FRAME_UPVC : cs->frameBack;
    return f;
}

uint16_t home_kit_flags(const HGraph *g, int unit, uint8_t *part) {
    HFinish f;
    home_finish(g, unit, &f);
    HCharSet cs;
    home_char_set(g, unit, &cs);
    unsigned brick = 0, old = 0;
    if (f.wall == HW_BRICK) { brick = f.wallCol & 7u; old = (f.wallCol & HOUSE_COL_OLD) != 0; }
    else if (f.stack == HW_BRICK) { brick = f.stackCol & 7u; old = (f.stackCol & HOUSE_COL_OLD) != 0; }
    unsigned stone;
    switch (g->style) {
    case HS_ROWHOUSE: stone = 2; break;   /* brownstone */
    case HS_MENA_VILLA: case HS_COURTYARD: stone = 1; break;
    default: stone = hash_combine(g->seed, 0x5701u) % 3u == 0 ? 1u : 0u; break;   /* limestone, some sandstone */
    }
    if (part) *part = (uint8_t)((f.roof & 15u) << HOUSE_KIT_ROOF | (f.roofCol & 7u) << HOUSE_KIT_ROOFCOL);
    return (uint16_t)((f.trimCol & 7u) << HOUSE_KIT_TRIM | (f.doorCol % 12u) << HOUSE_KIT_DOOR | brick << HOUSE_KIT_BRICK |
                      old << HOUSE_KIT_OLD | stone << HOUSE_KIT_STONE | (cs.rain & 3u) << HOUSE_KIT_RAIN);
}

const char *home_dress_rule(int r) {
    static const char *N[HDR_COUNT] = { "downpipe", "dormer", "shutters", "doorcase", "quoins", "canopy", "porch", "bargeboards", "budget",
                                            "solar", "tank", "riser", "wall", "terrace" };
    return r >= 0 && r < HDR_COUNT ? N[r] : "?";
}

/* ---------------------------------------------------------------------------
 * Placing
 * ------------------------------------------------------------------------- */

static int region_boxes_ground(const HGraph *g);

/* instances a house (docs/residential-target.md "Budgets"): HM5's 150, and HM5c1's retrofit kit on top of it (the
 * user's sheets put tanks, solar, risers, meters, lights and plants on every house) */
#define UNIT_BUDGET 230

static void repair(DC *d, int rule, const char *fmt, int unit, double a) {
    d->D->st.repairs[rule]++;
    if (!d->D->log) return;
    blog_printf(d->D->log, "  repair char-%s: unit %d: ", home_dress_rule(rule), unit);
    blog_printf(d->D->log, fmt, a);
    blog_printf(d->D->log, "\n");
}

static void begin(DC *d, int u) { d->unit = u; d->pm->part = d->part[u >= 0 && u < HG_MAX_UNITS ? u : 0]; }
/* Tags the instances placed since the last call with the unit placing them. */
static void done(DC *d) {
    PmInstances *l = d->pm->inst;
    if (!l) return;
    HDress *D = d->D;
    if (l->n > D->capUnit) {
        D->capUnit = l->n * 2 + 64;
        D->unit = (uint8_t *)realloc(D->unit, D->capUnit);
        if (!D->unit) abort();
    }
    for (uint32_t i = d->tagged; i < l->n; ++i) D->unit[i] = (uint8_t)d->unit;
    if (d->unit >= 0 && d->unit < HG_MAX_UNITS) d->count[d->unit] += (int)(l->n - d->tagged);
    d->tagged = l->n;
}

static void mesh_pt(const DC *d, const double p[3], float o[3]) { o[0] = (float)(d->sx * p[0]); o[1] = (float)p[1]; o[2] = (float)p[2]; }

/* A frame at p (the row's frame) facing out along the horizontal (mx, mz): z out, y up, x along the wall (y x z, in
 * the mesh's frame, so no instance is ever mirrored). */
static void push_wall(DC *d, const double p[3], double mx, double mz) {
    float o[3], ez[3] = { (float)(d->sx * mx), 0.0f, (float)mz }, ey[3] = { 0.0f, 1.0f, 0.0f };
    const float l = sqrtf(ez[0] * ez[0] + ez[2] * ez[2]);
    if (l > 1e-6f) { ez[0] /= l; ez[2] /= l; }
    const float ex[3] = { ez[2], 0.0f, -ez[0] };
    mesh_pt(d, p, o);
    pm_push(d->pm);
    pm_frame(d->pm, o, ex, ey, ez);
}
/* A frame at p with x along dir (the row's frame), y as near up as that allows. */
static void push_along(DC *d, const double p[3], const double dir[3]) {
    float o[3], ex[3] = { (float)(d->sx * dir[0]), (float)dir[1], (float)dir[2] }, ey[3], ez[3];
    float l = sqrtf(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
    if (l < 1e-6f) { ex[0] = 1.0f; ex[1] = ex[2] = 0.0f; l = 1.0f; }
    for (int k = 0; k < 3; ++k) ex[k] /= l;
    /* ez = ex x up, ey = ez x ex */
    ez[0] = -ex[2]; ez[1] = 0.0f; ez[2] = ex[0];
    l = sqrtf(ez[0] * ez[0] + ez[2] * ez[2]);
    if (l < 1e-6f) { ez[0] = 0.0f; ez[2] = 1.0f; l = 1.0f; }
    ez[0] /= l; ez[2] /= l;
    ey[0] = ez[1] * ex[2] - ez[2] * ex[1]; ey[1] = ez[2] * ex[0] - ez[0] * ex[2]; ey[2] = ez[0] * ex[1] - ez[1] * ex[0];
    mesh_pt(d, p, o);
    pm_push(d->pm);
    pm_frame(d->pm, o, ex, ey, ez);
}
static void pop(DC *d) { pm_pop(d->pm); }

/* The kit's x along a wall of outward normal m, in the row's frame: sign along e = (-m.z, m.x). */
static double kit_dir_along(const DC *d) { return -d->sx; }

/* A run on a wall (its face m.(x,z) = at, along e from sa to sb, at height y, `out` metres proud): the frame at the
 * end the kit's x runs from; returns the run's length. */
static double push_run_wall(DC *d, const double m[2], const double e[2], double at, double sa, double sb, double y, double out) {
    const double s = kit_dir_along(d) > 0.0 ? sa : sb;
    const double p[3] = { m[0] * (at + out) + e[0] * s, y, m[1] * (at + out) + e[1] * s };
    push_wall(d, p, m[0], m[1]);
    return sb - sa;
}

/* The stretches of [a, b] along p0 + dir * t where the test point (offset by off) is clear of every solid,
 * sampled every `step`; those shorter than minLen dropped. */
static int clear_runs(const Ctx *C, const double p0[3], const double dir[3], const double off[3], double a, double b,
                      double step, double minLen, Ival *out, int max) {
    if (b - a < minLen) return 0;
    const int ns = (int)fmax(1.0, ceil((b - a) / step));
    const double h = (b - a) / ns;
    int n = 0, open = 0;
    double start = a;
    for (int i = 0; i <= ns; ++i) {
        const double t = a + h * i;
        const double p[3] = { p0[0] + dir[0] * t + off[0], p0[1] + dir[1] * t + off[1], p0[2] + dir[2] * t + off[2] };
        const int clear = hs_inside(C, p, -1, 0.003) < 0;
        if (clear && !open) { open = 1; start = i ? t - 0.5 * h : a; }
        if (!clear && open) {
            open = 0;
            const double end = t - 0.5 * h;
            if (end - start >= minLen && n < max) { out[n].a = start; out[n].b = end; n++; }
        }
    }
    if (open && b - start >= minLen && n < max) { out[n].a = start; out[n].b = b; n++; }
    return n;
}

/* Cuts [c0, c1] out of a set of intervals. */
static int cut_runs(Ival *r, int n, double c0, double c1, double minLen, int max) {
    Ival o[32];
    int k = 0;
    for (int i = 0; i < n; ++i) {
        if (r[i].b <= c0 || r[i].a >= c1) { if (k < 32) o[k++] = r[i]; continue; }
        if (c0 - r[i].a >= minLen && k < 32) { o[k].a = r[i].a; o[k].b = c0; k++; }
        if (r[i].b - c1 >= minLen && k < 32) { o[k].a = c1; o[k].b = r[i].b; k++; }
    }
    if (k > max) k = max;
    memcpy(r, o, sizeof(Ival) * (size_t)k);
    return k;
}

/* ---------------------------------------------------------------------------
 * Windows and bays
 * ------------------------------------------------------------------------- */

static int wall_is_brick(const DC *d, int u) { return d->C->fin[u].wall == HW_BRICK; }

/* The free run along a window's wall beside it (to the next opening of its storey, a door, the wall's end). */
static void window_room(const DC *d, const HsWin *w, double *left, double *right) {
    double lo = w->s0, hi = w->s1;
    const Ctx *C = d->C;
    for (int i = 0; i < C->nwin; ++i) {
        const HsWin *o = &C->win[i];
        if (o == w || o->host != w->host || fabs(o->at - w->at) > 0.02 || o->m[0] * w->m[0] + o->m[1] * w->m[1] < 0.99) continue;
        if (o->y1 < w->y0 || o->y0 > w->y1) continue;
        if (o->a0 + o->w <= w->a0 + 1e-3) lo = fmax(lo, o->a0 + o->w);
        if (o->a0 >= w->a0 + w->w - 1e-3) hi = fmin(hi, o->a0);
    }
    for (int i = 0; i < C->ndoor; ++i) {
        const HsDoor *o = &C->door[i];
        if (w->m[1] > -0.7 || fabs(-o->z - w->at) > 0.05 || o->y0 > w->y1 || o->y1 < w->y0) continue;
        const double s = o->x * w->e[0] + o->z * w->e[1];
        if (s + 0.5 * o->w <= w->a0) lo = fmax(lo, s + 0.5 * o->w + 0.1);
        if (s - 0.5 * o->w >= w->a0 + w->w) hi = fmin(hi, s - 0.5 * o->w - 0.1);
    }
    *left = w->a0 - lo;
    *right = hi - (w->a0 + w->w);
}

/* Whether anything stands just in front of a wall at (s along e, y): a bay, a porch, a stack. */
static int front_blocked(const DC *d, const HsWin *w, double s, double y, double out) {
    const double p[3] = { w->m[0] * (w->at + out) + w->e[0] * s, y, w->m[1] * (w->at + out) + w->e[1] * s };
    return hs_inside(d->C, p, -1, 0.003) >= 0;
}

/* A box on a window's wall: s0..s1 along it, y0..y1, out to `out` from its face. */
static void occupy_wall(DC *d, const HsWin *w, double s0, double s1, double y0, double y1, double out) {
    double lo[3] = { 1e30, y0, 1e30 }, hi[3] = { -1e30, y1, -1e30 };
    for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) {
            const double s = a ? s1 : s0, o = b ? out : -0.02;
            const double x = w->m[0] * (w->at + o) + w->e[0] * s, z = w->m[1] * (w->at + o) + w->e[1] * s;
            lo[0] = fmin(lo[0], x); hi[0] = fmax(hi[0], x); lo[2] = fmin(lo[2], z); hi[2] = fmax(hi[2], z);
        }
    hc_occupy(d, lo, hi);
}

static void dress_window(DC *d, const HsWin *w) {
    const int u = w->unit;
    const HCharSet *cs = &d->C->cs[u];
    const int front = w->m[1] < -0.7;
    HkWindow k;
    memset(&k, 0, sizeof(k));
    k.frame = (uint8_t)home_char_frame(cs, front, w->storey, w->w);
    k.sill = cs->sill;
    if (d->C->fin[u].wall == HW_SIDING && k.sill != HK_SILL_NONE) k.sill = HK_SILL_TIMBER;
    int head = front ? cs->head : cs->lean ? HK_HEAD_NONE : cs->headBack;
    if ((head == HK_HEAD_SOLDIER || head == HK_HEAD_ARCH) && !wall_is_brick(d, u)) head = HK_HEAD_NONE;
    if (head == HK_HEAD_KEYSTONE && w->storey >= 2) head = HK_HEAD_LINTEL;   /* a Georgian top floor's plain lintels */
    k.head = (uint8_t)head;
    k.w = (float)w->w; k.h = (float)(w->y1 - w->y0); k.reveal = (float)w->depth;
    if (front && cs->shutters) {
        double l, r;
        window_room(d, w, &l, &r);
        const double need = 0.5 * w->w + 0.08;
        const double c = w->a0 + 0.5 * w->w, off = 0.75 * w->w + 0.03;
        if (l >= need && r >= need && !front_blocked(d, w, c - off, 0.5 * (w->y0 + w->y1), 0.05) && !front_blocked(d, w, c + off, 0.5 * (w->y0 + w->y1), 0.05))
            k.shutters = cs->shutters;
        else repair(d, HDR_SHUTTERS, "no room beside a window %.1f m wide: no shutters", u, w->w);
    }
    k.grille = (uint8_t)(w->storey == 0 && (cs->grilles == 2 || (cs->grilles == 1 && front)));
    k.box = (uint8_t)(front && cs->boxes && !cs->lean && !k.grille && (w->storey >= 1 || region_boxes_ground(d->g)) &&
                      ch(d->g->unit[u].seed, 0xB0u + (uint32_t)(w->a0 * 10.0) + w->storey * 97u, 0.6f));
    const double c = w->a0 + 0.5 * w->w;
    const double p[3] = { w->m[0] * w->at + w->e[0] * c, w->y0, w->m[1] * w->at + w->e[1] * c };
    begin(d, u);
    push_wall(d, p, w->m[0], w->m[1]);
    hk_window(d->pm, &k);
    pop(d);
    done(d);
    d->D->st.windows++;
    /* what stands proud of the wall round it: its shutters, its box */
    if (k.shutters) occupy_wall(d, w, w->a0 - 0.8 * w->w - 0.05, w->a0 + 1.8 * w->w + 0.05, w->y0 - 0.05, w->y1 + 0.05, 0.12);
    if (k.box) occupy_wall(d, w, w->a0 - 0.1, w->a0 + w->w + 0.1, w->y0 - 0.45, w->y0 + 0.4, 0.32);
}

/* A bay's dressing from its windows (hk_bay's parts, set by the shell's own lights): a frame, sill and apron in
 * each light, a cornice round each storey's head, posts at its corners. */
static void dress_bay(DC *d, int vi) {
    const HGraph *g = d->g;
    const HVol *v = &g->vol[vi];
    const Ctx *C = d->C;
    const int u = v->unit < g->nunit ? v->unit : 0;
    const HCharSet *cs = &C->cs[u];
    float ol[6][2];
    const int n = hvol_outline(v, ol);
    if (n != 4) return;
    const double dyu = C->dy ? C->dy[u] : 0.0;
    const int masonry = g->style == HS_ROWHOUSE;
    begin(d, u);
    for (int k = 0; k < v->storeys; ++k) {
        const double floorY = g->plinth + k * g->storeyH + dyu;
        double head = -1.0;
        for (int i = 0; i < C->nwin; ++i)
            if (C->win[i].vol == vi && C->win[i].storey == k) head = fmax(head, C->win[i].y1);
        if (head < 0.0) continue;
        for (int i = 0; i < C->nwin; ++i) {
            const HsWin *w = &C->win[i];
            if (w->vol != vi || w->storey != k) continue;
            HkWindow hw;
            memset(&hw, 0, sizeof(hw));
            hw.frame = (uint8_t)home_char_frame(cs, 1, k, w->w);
            hw.sill = cs->sill == HK_SILL_NONE ? HK_SILL_TIMBER : cs->sill;
            hw.w = (float)w->w; hw.h = (float)(w->y1 - w->y0); hw.reveal = (float)w->depth;
            const double c = w->a0 + 0.5 * w->w;
            const double p[3] = { w->m[0] * w->at + w->e[0] * c, w->y0, w->m[1] * w->at + w->e[1] * c };
            push_wall(d, p, w->m[0], w->m[1]);
            hk_window(d->pm, &hw);
            const double sillH = w->y0 - floorY;
            if (sillH > 0.5 && !masonry) {
                pm_push(d->pm);
                pm_translate(d->pm, 0.0f, (float)(0.12 - sillH), 0.0f);
                pm_scale(d->pm, (float)w->w, (float)((sillH - 0.22) / 0.7), 1.0f);
                pm_place(d->pm, PMK_H_BAY_APRON);
                pm_pop(d->pm);
            }
            pop(d);
            d->D->st.windows++;
        }
        /* the cornice round the storey's head, face by face (the bay's outline but its edge on the main wall) */
        for (int f = 0; f < 4; ++f) {
            const double ax = ol[f][0], az = ol[f][1], bx = ol[(f + 1) % 4][0], bz = ol[(f + 1) % 4][1];
            if (fabs(az - v->z1) < 0.01 && fabs(bz - v->z1) < 0.01) continue;   /* on the main's wall */
            double ex = bx - ax, ez = bz - az;
            const double len = sqrt(ex * ex + ez * ez);
            if (len < 0.3) continue;
            ex /= len; ez /= len;
            /* the outward normal: the edge turned, away from the bay's middle */
            double mx = ez, mz = -ex;
            const double cx = 0.5 * (v->x0 + v->x1), cz = 0.5 * (v->z0 + v->z1);
            if (mx * (0.5 * (ax + bx) - cx) + mz * (0.5 * (az + bz) - cz) < 0.0) { mx = -mx; mz = -mz; }
            const double m[2] = { mx, mz }, e[2] = { -mz, mx };
            const double at = mx * ax + mz * az, sa = e[0] * ax + e[1] * az, sb = e[0] * bx + e[1] * bz;
            const double lo = fmin(sa, sb) - 0.06, hi = fmax(sa, sb) + 0.06;
            const double L = push_run_wall(d, m, e, at, lo, hi, head, 0.0);
            hk_run(d->pm, PMK_H_BAY_CORNICE, (float)L, 1.0f);
            pop(d);
        }
        /* posts at its corners: square to the main wall where they meet it, bisecting the rest */
        for (int c = 0; c < 4 && !masonry; ++c) {
            const double px = ol[c][0], pz = ol[c][1];
            double mx = 0.0, mz = -1.0;
            if (fabs(pz - v->z1) > 0.01) {
                const double pa[2] = { ol[(c + 3) % 4][0], ol[(c + 3) % 4][1] }, pb[2] = { ol[(c + 1) % 4][0], ol[(c + 1) % 4][1] };
                double ux = px - pa[0], uz = pz - pa[1], wx = pb[0] - px, wz = pb[1] - pz;
                double lu = sqrt(ux * ux + uz * uz), lw = sqrt(wx * wx + wz * wz);
                if (lu > 1e-6 && lw > 1e-6) {
                    ux /= lu; uz /= lu; wx /= lw; wz /= lw;
                    /* each edge's outward normal (the outline runs anticlockwise seen from above, or not: take the one
                     * away from the middle) */
                    const double cx = 0.5 * (v->x0 + v->x1), cz = 0.5 * (v->z0 + v->z1);
                    double n1x = uz, n1z = -ux, n2x = wz, n2z = -wx;
                    if (n1x * (px - cx) + n1z * (pz - cz) < 0.0) { n1x = -n1x; n1z = -n1z; }
                    if (n2x * (px - cx) + n2z * (pz - cz) < 0.0) { n2x = -n2x; n2z = -n2z; }
                    mx = n1x + n2x; mz = n1z + n2z;
                    const double l = sqrt(mx * mx + mz * mz);
                    if (l > 1e-6) { mx /= l; mz /= l; } else { mx = 0.0; mz = -1.0; }
                }
            }
            const double p[3] = { px, floorY, pz };
            push_wall(d, p, mx, mz);
            pm_scale(d->pm, 1.0f, (float)(head - floorY), 1.0f);
            pm_place(d->pm, PMK_H_BAY_POST);
            pop(d);
        }
    }
    done(d);
}

/* ---------------------------------------------------------------------------
 * Doors, canopies, porches
 * ------------------------------------------------------------------------- */

static const HVol *unit_porch(const HGraph *g, int u) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role == HV_PORCH && g->vol[i].unit == u) return &g->vol[i];
    return NULL;
}

/* Whether a surround (half its width, its top) over a door at (x, y0) on the front wall at z keeps clear of the
 * windows over and beside it and the eaves. */
static int surround_fits(const DC *d, const HsDoor *dr, double half, double top, int recessed) {
    const Ctx *C = d->C;
    for (int i = 0; i < C->nwin; ++i) {
        const HsWin *w = &C->win[i];
        if (w->m[1] > -0.7 || fabs(w->at + dr->z) > 0.05 || w->bay) continue;
        const double s0 = fmin(w->a0 * w->e[0], (w->a0 + w->w) * w->e[0]), s1 = fmax(w->a0 * w->e[0], (w->a0 + w->w) * w->e[0]);
        if (s1 < dr->x - half - 0.05 || s0 > dr->x + half + 0.05) continue;
        if (w->y0 < top + 0.08 && w->y1 > dr->y0) return 0;
    }
    const HVol *m = &d->g->vol[dr->vol];
    const double dyu = d->C->dy ? d->C->dy[dr->unit] : 0.0;
    if (top > m->eaves + dyu - 0.25) return 0;
    /* nothing standing in front of it (a bay beside the door, a porch's posts); not asked at the back of a recess,
     * whose space the house's body still holds (the recess is cut out of it as an opening) */
    for (int k = -1; k <= 1 && !recessed; ++k) {
        const double p[3] = { dr->x + k * half, dr->y0 + 0.6 * (top - dr->y0), dr->z - 0.06 };
        if (hs_inside(C, p, -1, 0.003) >= 0) return 0;
    }
    return 1;
}

/* The porch's door canopy (HP_CANOPY: the kit's, on brackets) at underside y0: the kind that fits (gabled, then flat),
 * clear of the windows over it and of what stands in front of the wall; placed unless dry. Returns HCC_* or HCC_NONE. */
static int canopy_at(DC *d, int u, const HVol *p, double y0, int dry, double *halfOut, double *riseOut) {
    const HCharSet *cs = &d->C->cs[u];
    int ck = cs->canopy == HCC_NONE ? HCC_FLAT : cs->canopy;
    const double pw = p->x1 - p->x0, pd = p->z1 - p->z0, cx = 0.5 * (p->x0 + p->x1);
    for (; ck != HCC_NONE; ck = ck == HCC_GABLE ? HCC_FLAT : HCC_NONE) {
        const double W0 = ck == HCC_GABLE ? 1.5 : 1.4, D0 = ck == HCC_GABLE ? 0.8 : 0.65;
        const double sxw = fmin(1.3, fmax(0.85, pw / W0)), szd = fmin(1.35, fmax(0.9, pd / D0));
        const double half = 0.5 * W0 * sxw, depth = D0 * szd, rise = ck == HCC_GABLE ? 0.12 + half * tan(40.0 * PI_D / 180.0) + 0.1 : 0.2;
        int ok = 1;
        for (int i = 0; i < d->C->nwin && ok; ++i) {
            const HsWin *w = &d->C->win[i];
            if (w->m[1] > -0.7 || fabs(w->at + p->z1) > 0.05) continue;
            const double s0 = fmin(w->a0 * w->e[0], (w->a0 + w->w) * w->e[0]), s1 = fmax(w->a0 * w->e[0], (w->a0 + w->w) * w->e[0]);
            if (s1 > cx - half && s0 < cx + half && w->y0 < y0 + rise + 0.05 && w->y1 > y0 - 0.45) ok = 0;
        }
        for (int a = -1; a <= 1 && ok; a += 2)
            for (int b = 0; b < 2 && ok; ++b) {
                const double q[3] = { cx + a * (half - 0.05), y0 + 0.08, p->z1 - (b ? depth - 0.05 : 0.08) };
                if (hs_inside(d->C, q, -1, 0.003) >= 0) ok = 0;
            }
        if (!ok) continue;
        if (!dry) {
            const double q[3] = { cx, y0, p->z1 };
            begin(d, u);
            push_wall(d, q, 0.0, -1.0);
            pm_scale(d->pm, (float)sxw, 1.0f, (float)szd);
            pm_place(d->pm, ck == HCC_GABLE ? PMK_H_CANOPY_GABLE : PMK_H_CANOPY_FLAT);
            pop(d);
            done(d);
        }
        if (halfOut) *halfOut = half;
        if (riseOut) *riseOut = rise;
        return ck;
    }
    return HCC_NONE;
}

static void dress_door(DC *d, const HsDoor *dr) {
    const int u = dr->unit;
    const HCharSet *cs = &d->C->cs[u];
    const HGraph *g = d->g;
    const HVol *p = unit_porch(g, u);
    const int pk = p ? p->kind : -1;
    const double dyu = d->C->dy ? d->C->dy[u] : 0.0;
    int kind = cs->door, sur = HK_CASE_NONE;
    double fanH = 0.5;      /* a fanlight's height: the module's 0.5 m, less under a low porch's head */
    double canopyY = p ? p->y0 + dyu : 0.0;
    if (dr->porch) kind = HK_DOOR_HALF;   /* an enclosed porch's own door: half glazed */
    else if (dr->arch) {
        /* an arched door's or China's gate's leaves are the shell's (HM5e1): the kit sets down nothing, the door's room
         * on the wall is its reveal's */
        d->doorX[u] = dr->x;
        d->doorHalf[u] = p ? fmax(0.5 * dr->w + 0.05, 0.5 * (p->x1 - p->x0) + 0.05) : 0.5 * dr->w + 0.05;
        d->doorTop[u] = p && home_porch_recessed(pk) ? p->eaves + dyu : dr->y1 + 0.5 * dr->w;
        d->D->st.doors++;
        return;
    } else {
        sur = cs->surround;
        /* doorcases stand on a bare front (or where a canopy would); a fanlight over any door but an enclosed porch's
         * inner one, shrunk to the head of a recess or the ceiling of a porch over it */
        if ((sur == HK_CASE_FLAT || sur == HK_CASE_PEDIMENT) && pk != HP_CANOPY && pk != -1) sur = HK_CASE_FANLIGHT;
        if (sur == HK_CASE_FANLIGHT && pk == HP_ENCLOSED) sur = HK_CASE_NONE;
        const double h = dr->y1 - dr->y0;
        while (sur != HK_CASE_NONE) {
            double half, top;
            int ok;
            if (sur == HK_CASE_FANLIGHT) {
                double head = 1e9;   /* what stands over the door: a recess's head, a porch's ceiling */
                if (home_porch_recessed(pk)) head = p->eaves + dyu;
                else if (pk == HP_OPEN || pk == HP_LEAN || pk == HP_VERANDA) head = p->eaves + dyu - SLAB_T;
                fanH = fmin(0.5, head - dr->y1 - 0.06);
                half = 0.5 * (dr->w + 0.1);
                top = dr->y1 + 0.02 + fanH;
                ok = fanH >= 0.3 && surround_fits(d, dr, half, top, home_porch_recessed(pk));
                /* under a canopy: the canopy goes up over the fanlight, if one still fits there */
                if (ok && pk == HP_CANOPY) {
                    canopyY = fmax(canopyY, top + 0.04);
                    ok = canopy_at(d, u, p, canopyY, 1, NULL, NULL) != HCC_NONE;
                }
            } else {
                half = 0.9 * (dr->w + 0.1);
                top = dr->y0 + (sur == HK_CASE_PEDIMENT ? 3.5 : 3.0) * (h + 0.05) / 2.1;
                ok = !home_porch_recessed(pk) && surround_fits(d, dr, half, top, 0);
            }
            if (ok) { d->doorHalf[u] = half; d->doorTop[u] = top; break; }
            const int was = sur;
            sur = sur == HK_CASE_PEDIMENT ? HK_CASE_FLAT : sur == HK_CASE_FLAT ? HK_CASE_FANLIGHT : HK_CASE_NONE;
            if (sur == HK_CASE_NONE && pk == HP_CANOPY) canopyY = p->y0 + dyu;
            repair(d, HDR_DOORCASE, "no room for its surround (%.0f): a plainer one", u, (double)was);
        }
    }
    const double pp[3] = { dr->x, dr->y0, dr->z };
    begin(d, u);
    push_wall(d, pp, 0.0, -1.0);
    if (sur == HK_CASE_FANLIGHT && fanH < 0.49) {
        /* a shallow fanlight (under a recess's head): the leaf, then the fanlight squashed to the room there */
        const float w = (float)dr->w, hh = (float)(dr->y1 - dr->y0);
        hk_door(d->pm, kind, HK_CASE_NONE, w, hh, (float)dr->recess);
        pm_push(d->pm);
        pm_translate(d->pm, 0.0f, hh + 0.02f, 0.0f);
        pm_scale(d->pm, (w + 0.1f) / HK_FAN_W, (float)(fanH / HK_FAN_H), 1.0f);
        pm_place(d->pm, PMK_H_FANLIGHT);
        pm_pop(d->pm);
    } else hk_door(d->pm, kind, sur, (float)dr->w, (float)(dr->y1 - dr->y0), (float)dr->recess);
    pop(d);
    done(d);
    d->D->st.doors++;
    if (dr->porch) return;
    d->doorX[u] = dr->x;
    if (sur == HK_CASE_NONE) { d->doorHalf[u] = 0.5 * dr->w + 0.05; d->doorTop[u] = dr->y1; }
    /* the canopy over it (the porch's), unless a doorcase stands there */
    if (pk != HP_CANOPY || sur == HK_CASE_FLAT || sur == HK_CASE_PEDIMENT) return;
    double half = 0.0, rise = 0.0;
    if (canopy_at(d, u, p, canopyY, 0, &half, &rise) != HCC_NONE) {
        d->doorHalf[u] = fmax(d->doorHalf[u], half + 0.05);
        d->doorTop[u] = fmax(d->doorTop[u], canopyY + rise);
        if (cs->canopy == HCC_GABLE && rise < 0.3) repair(d, HDR_CANOPY, "a gabled canopy meets a window or a bay: a flat one (%.0f)", u, 0.0);
    } else repair(d, HDR_PORCH, "its front door has no canopy, porch or doorcase (%.0f)", u, 0.0);
}

/* Porch posts, spandrels, balustrades. */
static void dress_posts(DC *d) {
    const Ctx *C = d->C;
    const HGraph *g = d->g;
    static const int POST[3] = { PMK_H_POST_SQUARE, PMK_H_POST_TURNED, PMK_H_POST_CRAFT };
    for (int i = 0; i < C->npost; ++i) {
        const HsPost *hp = &C->post[i];
        const int u = hp->unit;
        const HCharSet *cs = &C->cs[u];
        const HVol *p = &g->vol[hp->vol];
        begin(d, u);
        const double q[3] = { hp->x, hp->y0, hp->z };
        push_wall(d, q, 0.0, -1.0);
        pm_scale(d->pm, 1.0f, (float)((hp->y1 - hp->y0) / HK_POST_H), 1.0f);
        pm_place(d->pm, POST[cs->post < 3 ? cs->post : 0]);
        pop(d);
        /* the beam's underside over the post: an open porch's pediment's foot, a lean-to's soffit */
        const double tp = tan(p->pitch * PI_D / 180.0);
        const double beam = p->kind == HP_OPEN ? hp->y1 : hp->y1 - SLAB_T + tp * 0.1;
        /* its neighbours along the porch's front */
        double prev = -1e9, next = 1e9;
        for (int j = 0; j < C->npost; ++j) {
            const HsPost *o = &C->post[j];
            if (j == i || o->vol != hp->vol) continue;
            if (o->x < hp->x && o->x > prev) prev = o->x;
            if (o->x > hp->x && o->x < next) next = o->x;
        }
        if (cs->spandrels && cs->post == HK_POST_TURNED) {
            for (int s = 0; s < 2; ++s) {
                const double nb = s ? next : prev;
                if (fabs(nb) > 1e8 || fabs(nb - hp->x) < 1.1) continue;
                const double dir[3] = { s ? 1.0 : -1.0, 0.0, 0.0 };
                const double a[3] = { hp->x + (s ? 0.06 : -0.06), beam, hp->z };
                push_along(d, a, dir);
                pm_place(d->pm, PMK_H_SPANDREL);
                pop(d);
            }
        }
        if (cs->balustrade && next < 1e8 && hp->y0 - (C->dy ? C->dy[u] : 0.0) > 0.25) {
            /* along to the next post, leaving the steps in front of the door open */
            Ival r[4] = { { hp->x + 0.07, next - 0.07 } };
            int nr = 1;
            const HUnit *un = &g->unit[u];
            if (un->door >= 0) {
                const HDoor *dr = &g->door[un->door];
                const double sw = fmin(1.6, dr->width + 0.5);
                nr = cut_runs(r, nr, dr->x - 0.5 * sw - 0.05, dr->x + 0.5 * sw + 0.05, 0.4, 4);
            }
            for (int k = 0; k < nr; ++k) {
                const double a[3] = { r[k].a, hp->y0, hp->z }, dd[3] = { 1.0, 0.0, 0.0 };
                push_along(d, a, dd);
                hk_balustrade(d->pm, (float)(r[k].b - r[k].a));
                pop(d);
            }
        }
        done(d);
    }
}

/* ---------------------------------------------------------------------------
 * Roofs: eaves, verges, ridges, dormers
 * ------------------------------------------------------------------------- */

static int is_house_body(int role) { return role == HV_MAIN || role == HV_PAIR || role == HV_ROW; }
static int drains(int role) { return role <= HV_EXTENSION || role == HV_OUTRIGGER; }

/* The ends of eaves edge e of R's footprint, run on past its neighbours (a verge's reach, a hip's overhang): the
 * fascia's line from t0 to t1 along d from F.p[e] (moved out ov). */
static void eaves_line(const Roof *R, int e, double d[2], double *len, double *t0, double *t1) {
    const int n = R->F.n, a = (e + n - 1) % n, b = (e + 1) % n;
    const double *p = R->F.p[e], *q = R->F.p[(e + 1) % n];
    d[0] = q[0] - p[0]; d[1] = q[1] - p[1];
    *len = sqrt(d[0] * d[0] + d[1] * d[1]);
    if (*len > 1e-9) { d[0] /= *len; d[1] /= *len; }
    const double ea = R->role[a] == ER_EAVES ? R->ov : R->role[a] == ER_VERGE ? R->ext[a] : 0.0;
    const double eb = R->role[b] == ER_EAVES ? R->ov : R->role[b] == ER_VERGE ? R->ext[b] : 0.0;
    *t0 = -ea; *t1 = *len + eb;
    /* a slice of a row's main: cut where its slabs are */
    if (fabs(d[0]) > 0.5) {
        const double ta = (R->cx0 - p[0]) / d[0], tb = (R->cx1 - p[0]) / d[0];
        if (R->cx0 > -1e29) { if (d[0] > 0) *t0 = fmax(*t0, ta); else *t1 = fmin(*t1, ta); }
        if (R->cx1 < 1e29) { if (d[0] > 0) *t1 = fmin(*t1, tb); else *t0 = fmax(*t0, tb); }
    } else if (p[0] < R->cx0 - 0.01 || p[0] > R->cx1 + 0.01) *t1 = *t0;
}

static int window_near(const DC *d, const double m[2], double at, double s, double y0, double y1, double margin) {
    const Ctx *C = d->C;
    for (int i = 0; i < C->nwin; ++i) {
        const HsWin *w = &C->win[i];
        if (fabs(w->at - at) > 0.05 || w->m[0] * m[0] + w->m[1] * m[1] < 0.99) continue;
        if (s < w->a0 - margin || s > w->a0 + w->w + margin || w->y1 < y0 || w->y0 > y1) continue;
        return 1;
    }
    if (m[1] < -0.7)
        for (int i = 0; i < C->ndoor; ++i) {
            const HsDoor *o = &C->door[i];
            if (fabs(-o->z - at) > 0.05) continue;
            const double sx = o->x;   /* on a front wall (m = (0, -1)) e = (1, 0): s is x */
            const double half = o->porch ? 0.5 * o->w + 0.15 : fmax(0.5 * o->w + 0.15, d->doorHalf[o->unit] + 0.1);
            if (fabs(s - sx) < half) return 1;
        }
    return 0;
}

static void dress_eaves(DC *d, const HsRoof *hr) {
    const HGraph *g = d->g;
    const Roof *R = &hr->R;
    const HVol *v = &g->vol[hr->vol];
    const int u = hr->unit;
    const HCharSet *cs = &d->C->cs[u];
    const Ctx *C = d->C;
    if (R->F.n != 4 || v->role == HV_BAY) return;
    const int flat = R->kind == HR_FLAT;
    if (flat && v->parapet == 2) return;
    for (int e = 0; e < R->F.n; ++e) {
        if (!flat && R->role[e] != ER_EAVES) continue;
        if (flat && (R->role[e] == ER_HIGH || R->role[e] == ER_EXT)) continue;
        double dd[2], len, t0, t1;
        eaves_line(R, e, dd, &len, &t0, &t1);
        if (t1 - t0 < 0.4) continue;
        const double *m = R->F.m[e];
        const double front = m[1] < -0.7;
        if (v->parapet == 1 && front) continue;   /* behind a Georgian front's parapet */
        const double yTop = (flat ? v->top : R->E - R->t * R->ov) + hr->dy;
        const double p0[3] = { R->F.p[e][0] + m[0] * R->ov, 0.0, R->F.p[e][1] + m[1] * R->ov };
        const double dir[3] = { dd[0], 0.0, dd[1] };
        Ival run[16];
        const double off[3] = { m[0] * 0.04, (flat ? R->E + hr->dy + 0.05 : yTop - 0.06), m[1] * 0.04 };
        const int nr = clear_runs(C, p0, dir, off, t0, t1, 0.1, 0.4, run, 16);
        const double at = m[0] * R->F.p[e][0] + m[1] * R->F.p[e][1];   /* the wall's face */
        const double e2[2] = { -m[1], m[0] };
        const double sOf0 = e2[0] * R->F.p[e][0] + e2[1] * R->F.p[e][1];
        const double sDir = e2[0] * dd[0] + e2[1] * dd[1];   /* +1 or -1: t to s */
        for (int k = 0; k < nr; ++k) {
            const double sa = sOf0 + sDir * (sDir > 0 ? run[k].a : run[k].b), sb = sOf0 + sDir * (sDir > 0 ? run[k].b : run[k].a);
            begin(d, u);
            /* the gutter: on the fascia's face, stop ends where it ends but at a party wall */
            if (!flat && cs->gutter != 255 && v->role != HV_PORCH) {
                const double L = push_run_wall(d, m, e2, at, sa, sb, yTop - 0.01, R->ov);
                const double xa = R->F.p[e][0] + dd[0] * run[k].a, xb = R->F.p[e][0] + dd[0] * run[k].b;
                const int cutA = fabs(dd[0]) > 0.5 && (fabs(xa - R->cx0) < 0.05 || fabs(xa - R->cx1) < 0.05);
                const int cutB = fabs(dd[0]) > 0.5 && (fabs(xb - R->cx0) < 0.05 || fabs(xb - R->cx1) < 0.05);
                /* which end the kit's x starts from: sa when it runs along +e */
                const int startIsA = (kit_dir_along(d) > 0.0) == (sDir > 0.0);
                const int stops = ((startIsA ? !cutA : !cutB) ? 1 : 0) | ((startIsA ? !cutB : !cutA) ? 2 : 0);
                hk_gutter(d->pm, cs->gutter, (float)L, stops);
                pop(d);
                d->D->st.gutters++;
            }
            /* brackets under the eaves (a flat slab's or a shallow-ish soffit), rafter tails */
            if (cs->eaves == HCE_BRACKETS && front && is_house_body(v->role) && R->ov > 0.15) {
                const double sc = fmin(1.0, (R->ov - 0.02) / 0.28);
                const double y = (flat ? R->E : R->E - SLAB_T) + hr->dy;
                const double lo = fmax(sa, fmin(e2[0] * R->W.p[e][0] + e2[1] * R->W.p[e][1], e2[0] * R->W.p[(e + 1) % 4][0] + e2[1] * R->W.p[(e + 1) % 4][1])) + 0.3;
                const double hi = fmin(sb, fmax(e2[0] * R->W.p[e][0] + e2[1] * R->W.p[e][1], e2[0] * R->W.p[(e + 1) % 4][0] + e2[1] * R->W.p[(e + 1) % 4][1])) - 0.3;
                if (hi > lo) {
                    const int nb = (int)floor((hi - lo) / 0.85) + 1;
                    for (int b = 0; b < nb; ++b) {
                        const double s = nb > 1 ? lo + (hi - lo) * b / (nb - 1) : 0.5 * (lo + hi);
                        if (window_near(d, m, at, s, y - 0.4, y, 0.02)) continue;
                        const double q[3] = { m[0] * at + e2[0] * s, y, m[1] * at + e2[1] * s };
                        push_wall(d, q, m[0], m[1]);
                        pm_scale(d->pm, (float)sc, (float)sc, (float)sc);
                        pm_place(d->pm, PMK_H_EAVES_BRACKET);
                        pop(d);
                    }
                }
            }
            if (cs->eaves == HCE_RAFTERS && !flat) {
                /* along the wall, not round a hip's corner (the next slab's soffit falls the other way: they would
                 * come up through its tiles), and where nothing runs in under the eaves (a porch's roof) */
                const int n4 = R->F.n, pe = (e + n4 - 1) % n4, ne = (e + 1) % n4;
                const double wa = (R->W.p[e][0] - R->F.p[e][0]) * dd[0] + (R->W.p[e][1] - R->F.p[e][1]) * dd[1];
                const double wb = (R->W.p[ne][0] - R->F.p[e][0]) * dd[0] + (R->W.p[ne][1] - R->F.p[e][1]) * dd[1];
                double ta = run[k].a, tb = run[k].b;
                if (R->role[pe] == ER_EAVES) ta = fmax(ta, fmin(wa, wb) + 0.05);
                if (R->role[ne] == ER_EAVES) tb = fmin(tb, fmax(wa, wb) - 0.05);
                const double q0[3] = { R->F.p[e][0] + m[0] * 0.5 * R->ov, 0.0, R->F.p[e][1] + m[1] * 0.5 * R->ov };
                const double qo[3] = { 0.0, R->E - SLAB_T - R->t * 0.5 * R->ov - 0.07 + hr->dy, 0.0 };
                Ival tr[16];
                const int nt = tb - ta > 0.3 ? clear_runs(C, q0, dir, qo, ta, tb, 0.1, 0.3, tr, 16) : 0;
                for (int j = 0; j < nt; ++j) {
                    const double ra = sOf0 + sDir * (sDir > 0 ? tr[j].a : tr[j].b), rb = sOf0 + sDir * (sDir > 0 ? tr[j].b : tr[j].a);
                    const double L = push_run_wall(d, m, e2, at, ra, rb, R->E - SLAB_T - 0.005 + hr->dy, 0.0);
                    hk_rafter_tails(d->pm, (float)L, cs->lean && !front ? 1.2f : 0.6f, (float)(atan(R->t) * 180.0 / PI_D), (float)R->ov);
                    pop(d);
                }
            }
            done(d);
        }
        /* a downpipe from the longest run, at a corner or a party wall, clear of openings, bays and porches */
        if (flat || cs->gutter == 255 || !drains(v->role) || nr == 0) continue;
        int best = 0;
        for (int k = 1; k < nr; ++k) if (run[k].b - run[k].a > run[best].b - run[best].a) best = k;
        if (run[best].b - run[best].a < 1.0) continue;
        /* the wall's run along t */
        const double wa = (R->W.p[e][0] - R->F.p[e][0]) * dd[0] + (R->W.p[e][1] - R->F.p[e][1]) * dd[1];
        const double wb = (R->W.p[(e + 1) % 4][0] - R->F.p[e][0]) * dd[0] + (R->W.p[(e + 1) % 4][1] - R->F.p[e][1]) * dd[1];
        const double w0 = fmin(wa, wb) + 0.15, w1 = fmax(wa, wb) - 0.15;
        double cand[2] = { fmax(run[best].a + 0.1, w0), fmin(run[best].b - 0.1, w1) };
        /* prefer the end at a party wall, then the end away from the front door */
        const double xa = R->F.p[e][0] + dd[0] * cand[0], xb = R->F.p[e][0] + dd[0] * cand[1];
        int first = 0;
        if (fabs(dd[0]) > 0.5 && (fabs(xb - R->cx0) < 0.3 || fabs(xb - R->cx1) < 0.3)) first = 1;
        else if (front && g->unit[u].door >= 0) {
            const double dx = g->door[g->unit[u].door].x;
            first = fabs(xb - dx) > fabs(xa - dx);
        }
        int placed = 0;
        for (int c = 0; c < 2 && !placed; ++c) {
            const double t = cand[c ? !first : first];
            if (t < w0 - 1e-6 || t > w1 + 1e-6) continue;
            const double px = R->F.p[e][0] + dd[0] * t, pz = R->F.p[e][1] + dd[1] * t;
            const double s = e2[0] * px + e2[1] * pz;
            int ok = !window_near(d, m, at, s, hr->dy, yTop, 0.14);
            for (double y = hr->dy + 0.15; y < yTop - 0.15 && ok; y += 0.3) {
                const double q[3] = { px + m[0] * HK_PIPE_Z, y, pz + m[1] * HK_PIPE_Z };
                if (hs_inside(C, q, -1, 0.0) >= 0) ok = 0;
            }
            if (!ok) continue;
            const double q[3] = { px, hr->dy, pz };
            begin(d, u);
            push_wall(d, q, m[0], m[1]);
            hk_downpipe(d->pm, (float)(yTop - 0.1 - hr->dy), (float)(R->ov + HK_GUTTER_Z), cs->hopper);
            pop(d);
            done(d);
            {   /* the pipe's run up the wall, kept clear by what comes after (the retrofit's risers and meters) */
                const double cx = px + m[0] * 0.07, cz = pz + m[1] * 0.07;
                const double lo[3] = { cx - 0.13, hr->dy, cz - 0.13 }, hi[3] = { cx + 0.13, yTop + 0.1, cz + 0.13 };
                hc_occupy(d, lo, hi);
            }
            d->D->st.pipes++;
            placed = 1;
        }
        if (!placed) repair(d, HDR_DOWNPIPE, "every place for a downpipe on a %.1f m eaves is blocked: none", u, run[best].b - run[best].a);
    }
}

/* Verges: bargeboards and a finial on the street's gables, half-timbering in them, knee braces under them. */
static void dress_verges(DC *d, const HsRoof *hr) {
    const HGraph *g = d->g;
    const Roof *R = &hr->R;
    const HVol *v = &g->vol[hr->vol];
    const int u = hr->unit;
    const HCharSet *cs = &d->C->cs[u];
    if (R->kind != HR_GABLE || R->F.n != 4) return;
    const double pitch = atan(R->t) * 180.0 / PI_D;
    for (int f = 0; f < 4; ++f) {
        if (R->role[f] != ER_VERGE || R->cut[f]) continue;
        const double *m = R->F.m[f];
        const double *a = R->F.p[f], *b = R->F.p[(f + 1) % 4];
        const double L = sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1])), half = 0.5 * L;
        const double mx = 0.5 * (a[0] + b[0]), mz = 0.5 * (a[1] + b[1]);
        const double yApex = R->E + R->t * half + hr->dy;
        const int street = m[1] < -0.7;
        /* the gables seen from the street: its own, and the sides' (a detached house's, a semi's open side, a
         * terrace's ends: the verges here are never cut at a party wall), not the back's */
        const int seen = street || (fabs(m[0]) > 0.7 && !cs->lean);   /* a lean dressing keeps the street's alone */
        begin(d, u);
        if (seen && cs->barge) {
            const double apex[3] = { mx + m[0] * (R->ext[f] + 0.012), yApex, mz + m[1] * (R->ext[f] + 0.012) };
            const double t1[3] = { apex[0] + m[0] * 0.05, yApex - 0.12, apex[2] + m[1] * 0.05 };
            const double dl[3] = { (a[0] - mx) * 0.5, -R->t * half * 0.5, (a[1] - mz) * 0.5 };
            const double t2[3] = { t1[0] + dl[0], t1[1] + dl[1], t1[2] + dl[2] };
            if (hs_inside(d->C, t1, -1, 0.003) < 0 && hs_inside(d->C, t2, -1, 0.003) < 0) {
                push_wall(d, apex, m[0], m[1]);
                hk_bargeboards(d->pm, cs->barge - 1, (float)(half + R->ov), (float)pitch, cs->finial);
                pop(d);
            } else repair(d, HDR_BARGE, "a gable's verge meets another roof: no bargeboards (%.1f m)", u, L);
        }
        if (seen && cs->tudor && half > 1.2) {
            const double q[3] = { mx, R->E + hr->dy, mz };
            const double t[3] = { mx + m[0] * 0.06, R->E + hr->dy + 0.4 * R->t * half, mz + m[1] * 0.06 };
            if (hs_inside(d->C, t, -1, 0.003) < 0) {
                push_wall(d, q, m[0], m[1]);
                hk_tudor_gable(d->pm, (float)half, (float)pitch, 0.45f);
                pop(d);
            }
        }
        if (cs->knees && R->ext[f] > 0.25) {
            const double fr[3] = { 0.12, 0.5, 0.88 };
            for (int k = 0; k < 3; ++k) {
                const double s = fr[k] * L, din = fmin(s, L - s);
                const double y = R->E - SLAB_T + R->t * din + hr->dy;
                const double q[3] = { a[0] + (b[0] - a[0]) * fr[k], y, a[1] + (b[1] - a[1]) * fr[k] };
                const double t[3] = { q[0] + m[0] * 0.15, y - 0.4, q[2] + m[1] * 0.15 };
                if (y - hr->dy < R->E - SLAB_T + 0.2 || hs_inside(d->C, t, -1, 0.003) >= 0) continue;
                push_wall(d, q, m[0], m[1]);
                pm_scale(d->pm, 1.0f, 1.0f, (float)((R->ext[f] - 0.03) / 0.75));
                pm_place(d->pm, PMK_H_KNEE_BRACE);
                pop(d);
            }
        }
        done(d);
    }
}

/* Ridge tiles along a ridge or a hip (the row's frame, a to b), where it is clear of stacks, upstands and the
 * roofs running into it. */
static void ridge_run(DC *d, const HsRoof *hr, const double a[3], const double b[3], int crested) {
    const Roof *R = &hr->R;
    double dir[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 0.3) return;
    for (int k = 0; k < 3; ++k) dir[k] /= len;
    double t0 = 0.0, t1 = len;
    if (fabs(dir[0]) > 1e-6) {
        if (R->cx0 > -1e29) { const double tc = (R->cx0 - a[0]) / dir[0]; if (dir[0] > 0.0) t0 = fmax(t0, tc); else t1 = fmin(t1, tc); }
        if (R->cx1 < 1e29) { const double tc = (R->cx1 - a[0]) / dir[0]; if (dir[0] > 0.0) t1 = fmin(t1, tc); else t0 = fmax(t0, tc); }
    } else if (a[0] < R->cx0 - 0.01 || a[0] > R->cx1 + 0.01) return;
    Ival run[8];
    const double off[3] = { 0.0, 0.05, 0.0 };
    const int nr = clear_runs(d->C, a, dir, off, t0, t1, 0.1, 0.3, run, 8);
    for (int k = 0; k < nr; ++k) {
        /* laid from the end the kit's x runs from (push_along keeps x along dir) */
        const double s[3] = { a[0] + dir[0] * run[k].a, a[1] + dir[1] * run[k].a, a[2] + dir[2] * run[k].a };
        push_along(d, s, dir);
        hk_ridge(d->pm, crested, (float)(run[k].b - run[k].a));
        pop(d);
    }
}

static void dress_ridges(DC *d, const HsRoof *hr) {
    const Roof *R = &hr->R;
    const HVol *v = &d->g->vol[hr->vol];
    const int u = hr->unit;
    const HCharSet *cs = &d->C->cs[u];
    if (!cs->ridge || R->F.n != 4 || (R->kind != HR_GABLE && R->kind != HR_HIP) || v->role == HV_BAY) return;
    begin(d, u);
    const int crest = cs->ridge == 2 && !cs->lean && is_house_body(v->role);
    if (R->kind == HR_GABLE) {
        int v1 = -1, v2 = -1;
        for (int f = 0; f < 4; ++f) if (R->role[f] != ER_EAVES) { if (v1 < 0) v1 = f; else v2 = f; }
        if (v1 < 0 || v2 < 0) { done(d); return; }
        const double *a0 = R->F.p[v1], *a1 = R->F.p[(v1 + 1) % 4], *b0 = R->F.p[v2], *b1 = R->F.p[(v2 + 1) % 4];
        const double half = 0.5 * sqrt((a1[0] - a0[0]) * (a1[0] - a0[0]) + (a1[1] - a0[1]) * (a1[1] - a0[1]));
        const double y = R->E + R->t * half + hr->dy;
        const double ea = R->role[v1] == ER_VERGE ? R->ext[v1] : 0.0, eb = R->role[v2] == ER_VERGE ? R->ext[v2] : 0.0;
        const double a[3] = { 0.5 * (a0[0] + a1[0]) + R->F.m[v1][0] * ea, y, 0.5 * (a0[1] + a1[1]) + R->F.m[v1][1] * ea };
        const double b[3] = { 0.5 * (b0[0] + b1[0]) + R->F.m[v2][0] * eb, y, 0.5 * (b0[1] + b1[1]) + R->F.m[v2][1] * eb };
        ridge_run(d, hr, a, b, crest);
    } else {
        const double *p = R->F.p[0];
        const double l0 = sqrt((R->F.p[1][0] - p[0]) * (R->F.p[1][0] - p[0]) + (R->F.p[1][1] - p[1]) * (R->F.p[1][1] - p[1]));
        const double l1 = sqrt((R->F.p[2][0] - R->F.p[1][0]) * (R->F.p[2][0] - R->F.p[1][0]) + (R->F.p[2][1] - R->F.p[1][1]) * (R->F.p[2][1] - R->F.p[1][1]));
        const double cx = 0.25 * (R->F.p[0][0] + R->F.p[1][0] + R->F.p[2][0] + R->F.p[3][0]);
        const double cz = 0.25 * (R->F.p[0][1] + R->F.p[1][1] + R->F.p[2][1] + R->F.p[3][1]);
        const int along0 = l0 >= l1;
        const double half = 0.5 * (along0 ? l1 : l0), lng = 0.5 * (along0 ? l0 : l1);
        double ax = along0 ? R->F.p[1][0] - p[0] : R->F.p[2][0] - R->F.p[1][0], az = along0 ? R->F.p[1][1] - p[1] : R->F.p[2][1] - R->F.p[1][1];
        const double al = sqrt(ax * ax + az * az);
        ax /= al; az /= al;
        const double y = R->E + R->t * half + hr->dy, r = lng - half;
        const double ra[3] = { cx - ax * r, y, cz - az * r }, rb[3] = { cx + ax * r, y, cz + az * r };
        if (r > 0.15) ridge_run(d, hr, ra, rb, crest);
        /* the hips: from each fascia's corner (ov out on both sides, E - t ov) up to the ridge's nearer end */
        for (int c = 0; c < 4; ++c) {
            const int pe = (c + 3) % 4;
            const double ox = R->F.p[c][0] + (R->F.m[c][0] + R->F.m[pe][0]) * R->ov, oz = R->F.p[c][1] + (R->F.m[c][1] + R->F.m[pe][1]) * R->ov;
            const double lo[3] = { ox, R->E - R->t * R->ov + hr->dy, oz };
            const double *end = (ox - cx) * ax + (oz - cz) * az < 0.0 ? ra : rb;
            if (!cs->lean) ridge_run(d, hr, lo, end, 0);   /* a lean dressing leaves the hips bare */
        }
    }
    done(d);
}

/* Whether a dormer (or a rooflight) of width w, its front foot `in` metres in from eaves edge e at s along it, fits
 * the slope: its footprint on this roof's own slabs, nothing standing over it, under the ridge. */
/* What a dormer, a rooflight or a vent takes of a slope (its front foot q on the roof plane over eaves of outward
 * normal m, w across along dd, deep up the slope in plan, tall over it): a box kept clear by the retrofit's panels. */
static void occupy_slope(DC *d, const double q[3], const double *m, const double dd[2], double w, double deep, double tall) {
    double lo[3] = { 1e30, q[1] - 0.05, 1e30 }, hi[3] = { -1e30, q[1] + tall + 0.15, -1e30 };
    for (int a = -1; a <= 1; a += 2)
        for (int b = 0; b < 2; ++b) {
            const double x = q[0] + dd[0] * a * (0.5 * w + 0.15) - m[0] * (b ? deep + 0.15 : -0.15);
            const double z = q[2] + dd[1] * a * (0.5 * w + 0.15) - m[1] * (b ? deep + 0.15 : -0.15);
            lo[0] = fmin(lo[0], x); hi[0] = fmax(hi[0], x); lo[2] = fmin(lo[2], z); hi[2] = fmax(hi[2], z);
        }
    hc_occupy(d, lo, hi);
}

static int slope_fits(const DC *d, const HsRoof *hr, int e, double tc, double w, double in, double deep, double tall) {
    const Roof *R = &hr->R;
    const double *m = R->F.m[e];
    double dd[2] = { R->F.p[(e + 1) % 4][0] - R->F.p[e][0], R->F.p[(e + 1) % 4][1] - R->F.p[e][1] };
    const double l = sqrt(dd[0] * dd[0] + dd[1] * dd[1]);
    dd[0] /= l; dd[1] /= l;
    for (int i = 0; i <= 2; ++i)
        for (int j = 0; j <= 2; ++j) {
            const double across = (i - 1) * (0.5 * w + 0.12), up = in + deep * j / 2.0;
            const double x = R->F.p[e][0] + dd[0] * (tc + across) - m[0] * up, z = R->F.p[e][1] + dd[1] * (tc + across) - m[1] * up;
            const double y = R->E + R->t * up + hr->dy;
            /* just under the covering: in one of this roof's own slabs (the body under it holds the point too) */
            const double below[3] = { x, y - 0.04, z };
            int on = 0;
            for (int si = 0; si < d->C->ns && !on; ++si) {
                const Solid *so = &d->C->s[si];
                on = so->live && !so->neg && so->kind == SK_SLAB && so->vol == hr->vol && hs_in_solid(so, below, 0.0);
            }
            if (!on) return 0;
            for (int k = 1; k <= 2; ++k) {
                const double above[3] = { x, y + tall * k / 2.0, z };
                if (hs_inside(d->C, above, -1, 0.0) >= 0) return 0;
            }
        }
    return 1;
}

static void dress_slopes(DC *d, const HsRoof *hr) {
    const HGraph *g = d->g;
    const Roof *R = &hr->R;
    const HVol *v = &g->vol[hr->vol];
    const int u = hr->unit;
    const HCharSet *cs = &d->C->cs[u];
    if (!is_house_body(v->role) || R->F.n != 4 || (R->kind != HR_GABLE && R->kind != HR_HIP)) return;
    const double pitch = atan(R->t) * 180.0 / PI_D;
    for (int e = 0; e < 4; ++e) {
        if (R->role[e] != ER_EAVES) continue;
        const double *m = R->F.m[e];
        const int front = m[1] < -0.7, back = m[1] > 0.7;
        if (!front && !back) continue;
        double dd[2], len, t0, t1;
        eaves_line(R, e, dd, &len, &t0, &t1);
        /* the unit's part of the slope (inside its walls) */
        const double a = fmax(t0, 0.0), b = fmin(t1, len);
        if (b - a < 1.6) continue;
        /* half the span: to the ridge */
        double span = 1e9;
        for (int f = 0; f < 4; ++f)
            if (f != e && R->F.m[f][0] * m[0] + R->F.m[f][1] * m[1] < -0.99) {
                const double gap = fabs((R->F.p[f][0] - R->F.p[e][0]) * m[0] + (R->F.p[f][1] - R->F.p[e][1]) * m[1]);
                span = fmin(span, gap);
            }
        const double half = 0.5 * span, ridgeY = R->E + R->t * half;
        const int kind = front ? cs->dormer : cs->dormerBack;
        begin(d, u);
        if (kind && pitch >= HK_DORMER_P - 0.5 && pitch <= 52.0) {
            const int dk = kind - 1;
            const double w = dk == HK_DORMER_FLAT ? (back ? 2.2 : 1.8) : 1.5;
            const double in = 0.8, tall = dk == HK_DORMER_FLAT ? 1.45 : 1.75, deep = tall / R->t;
            int placed = 0;
            if (R->E + R->t * in + tall + 0.2 < ridgeY) {
                static const double TRY[5] = { 0.0, 0.7, -0.7, 1.4, -1.4 };
                for (int k = 0; k < 5 && !placed; ++k) {
                    const double tc = 0.5 * (a + b) + TRY[k];
                    if (tc - 0.5 * w < a + 0.3 || tc + 0.5 * w > b - 0.3) continue;
                    if (!slope_fits(d, hr, e, tc, w, in, deep, tall)) continue;
                    const double q[3] = { R->F.p[e][0] + dd[0] * tc - m[0] * in, R->E + R->t * in + hr->dy, R->F.p[e][1] + dd[1] * tc - m[1] * in };
                    push_wall(d, q, m[0], m[1]);
                    hk_dormer(d->pm, dk, (float)w);
                    pop(d);
                    d->D->st.dormers++;
                    placed = 1;
                    occupy_slope(d, q, m, dd, w, deep, tall);
                }
            }
            if (!placed) repair(d, HDR_DORMER, "no clear place on a %.1f m slope for its dormer: none", u, b - a);
        }
        if (back && cs->rooflight && !cs->lean && pitch >= 20.0) {
            const double in = 1.6, tc = 0.5 * (a + b) + 0.7;
            if (R->E + R->t * (in + 0.6) < ridgeY - 0.3 && slope_fits(d, hr, e, tc, 0.8, in - 0.5, 1.0, 0.2)) {
                const double q[3] = { R->F.p[e][0] + dd[0] * tc - m[0] * in, R->E + R->t * in + hr->dy, R->F.p[e][1] + dd[1] * tc - m[1] * in };
                push_wall(d, q, m[0], m[1]);
                pm_rotate_x(d->pm, (float)(pitch * PI_D / 180.0));
                pm_place(d->pm, PMK_H_ROOFLIGHT);
                pop(d);
                occupy_slope(d, q, m, dd, 0.8, 1.0, 0.25);
            }
        }
        if (back && cs->vent && !cs->lean) {
            const double in = 1.0, tc = a + 0.7;
            if (tc < b - 0.5 && slope_fits(d, hr, e, tc, 0.2, in - 0.1, 0.2, 0.6)) {
                const double q[3] = { R->F.p[e][0] + dd[0] * tc - m[0] * in, R->E + R->t * in + hr->dy, R->F.p[e][1] + dd[1] * tc - m[1] * in };
                push_wall(d, q, m[0], m[1]);
                pm_place(d->pm, PMK_H_VENT_PIPE);
                pop(d);
                occupy_slope(d, q, m, dd, 0.3, 0.3, 0.7);
            }
        }
        done(d);
    }
}

/* ---------------------------------------------------------------------------
 * Stacks, the front's reliefs
 * ------------------------------------------------------------------------- */

static void dress_stacks(DC *d) {
    const Ctx *C = d->C;
    for (int i = 0; i < C->nstack; ++i) {
        const HsStack *s = &C->stack[i];
        const HChimney *k = &d->g->chim[s->chim];
        begin(d, s->unit);
        float o[3];
        const double p[3] = { s->x, s->top, s->z };
        mesh_pt(d, p, o);
        pm_push(d->pm);
        pm_translate(d->pm, o[0], o[1], o[2]);
        hk_chimney_top(d->pm, (float)s->w, (float)s->d, k->pots > 0 ? k->pots : 1, d->g->unit[s->unit].seed + (uint32_t)i * 7919u);
        pm_pop(d->pm);
        done(d);
        d->D->st.stacks++;
    }
}

/* A run of a relief along the front wall, clear of what stands in front of it and of the door's surround. */
static int front_runs(const DC *d, const Roof *R, int e, double y, double out, double dyCheck, int door, Ival *r, int max) {
    const double *m = R->W.m[e];
    const double *a = R->W.p[e], *b = R->W.p[(e + 1) % 4];
    double dir[3] = { b[0] - a[0], 0.0, b[1] - a[1] };
    const double len = sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    if (len < 0.5) return 0;
    dir[0] /= len; dir[2] /= len;
    const double p0[3] = { a[0], y, a[1] };
    const double off[3] = { m[0] * out, dyCheck, m[1] * out };
    int n = clear_runs(d->C, p0, dir, off, 0.0, len, 0.1, 0.3, r, max);
    if (door >= 0 && fabs(dir[0]) > 0.5) {
        const double c0 = (d->doorX[door] - d->doorHalf[door] - 0.05 - a[0]) / dir[0], c1 = (d->doorX[door] + d->doorHalf[door] + 0.05 - a[0]) / dir[0];
        n = cut_runs(r, n, fmin(c0, c1), fmax(c0, c1), 0.3, max);
    }
    if (fabs(dir[0]) > 0.5) {
        const HGraph *g = d->g;
        for (int i = 0; i < g->nvol; ++i) {
            const HVol *p = &g->vol[i];
            if (p->role != HV_PORCH || !home_porch_recessed(p->kind)) continue;
            const double dyu = d->C->dy ? d->C->dy[p->unit] : 0.0;
            if (y > p->eaves + dyu + 0.05) continue;
            const double pad = p->kind == HP_PILOTIS ? 0.05 : 0.08;
            const double c0 = (p->x0 - pad - a[0]) / dir[0], c1 = (p->x1 + pad - a[0]) / dir[0];
            n = cut_runs(r, n, fmin(c0, c1), fmax(c0, c1), 0.3, max);
        }
        for (int u = 0; u < g->nunit; ++u) {
            const HUnit *un = &g->unit[u];
            const double dyu = d->C->dy ? d->C->dy[u] : 0.0;
            if (un->shopX1 <= un->shopX0 || y > g->plinth + dyu + 2.8) continue;
            const double c0 = (un->shopX0 - 0.1 - a[0]) / dir[0], c1 = (un->shopX1 + 0.1 - a[0]) / dir[0];
            n = cut_runs(r, n, fmin(c0, c1), fmax(c0, c1), 0.3, max);
        }
    }
    /* back to s along e for push_run_wall */
    const double e2[2] = { -m[1], m[0] };
    const double s0 = e2[0] * a[0] + e2[1] * a[1], sd = e2[0] * dir[0] + e2[1] * dir[2];
    for (int k = 0; k < n; ++k) {
        const double sa = s0 + sd * r[k].a, sb = s0 + sd * r[k].b;
        r[k].a = fmin(sa, sb); r[k].b = fmax(sa, sb);
    }
    return n;
}

static void dress_front(DC *d, const HsRoof *hr) {
    const HGraph *g = d->g;
    const Roof *R = &hr->R;
    const HVol *v = &g->vol[hr->vol];
    const int u = hr->unit;
    const HCharSet *cs = &d->C->cs[u];
    if (!is_house_body(v->role) || R->W.n != 4) return;
    int e = -1;
    for (int k = 0; k < 4; ++k) if (R->W.m[k][1] < -0.7) e = k;
    if (e < 0) return;
    const double *m = R->W.m[e];
    const double e2[2] = { -m[1], m[0] };
    const double at = m[0] * R->W.p[e][0] + m[1] * R->W.p[e][1];
    const double P = g->plinth, sh = g->storeyH, dy = hr->dy;
    const int door = g->unit[u].door >= 0 && g->door[g->unit[u].door].vol == hr->vol ? u : -1;
    Ival r[16];
    begin(d, u);
    if (cs->plinth && P >= 0.2) {
        const double hgt = fmin(0.45, P);
        const int n = front_runs(d, R, e, dy, 0.06, 0.5 * hgt, door, r, 16);
        for (int k = 0; k < n; ++k) {
            const double L = push_run_wall(d, m, e2, at, r[k].a, r[k].b, dy, 0.0);
            pm_scale(d->pm, 1.0f, (float)(hgt / 0.45), 1.0f);
            hk_run(d->pm, PMK_H_PLINTH, (float)L, 0.0f);
            pop(d);
        }
    }
    if (cs->string && !cs->lean && v->storeys >= 2) {
        const double y = dy + P + sh - 0.1;
        const int n = front_runs(d, R, e, y, 0.07, 0.08, door >= 0 && d->doorTop[u] > y - 0.05 ? door : -1, r, 16);
        for (int k = 0; k < n; ++k) {
            const double L = push_run_wall(d, m, e2, at, r[k].a, r[k].b, y, 0.0);
            hk_run(d->pm, PMK_H_STRING_COURSE, (float)L, 0.0f);
            pop(d);
        }
    }
    if (cs->hung && v->storeys >= 2) {
        const double y0 = dy + P + 2.3, y1 = dy + P + sh + 0.78;
        if (y1 - y0 > 0.3) {
            const int n = front_runs(d, R, e, y0, 0.06, 0.5 * (y1 - y0), door, r, 16);
            for (int k = 0; k < n; ++k) {
                const double L = push_run_wall(d, m, e2, at, r[k].a, r[k].b, y0, 0.0);
                hk_hung_tiles(d->pm, (float)L, (float)(y1 - y0));
                pop(d);
            }
        }
    }
    if (cs->cornice && (v->parapet == 1 || (R->kind == HR_FLAT && v->parapet == 2))) {
        const double y = dy + (v->parapet == 1 ? R->E - 0.4 : R->E - 0.25);
        const int n = front_runs(d, R, e, y, 0.14, 0.1, -1, r, 16);
        for (int k = 0; k < n; ++k) {
            const double L = push_run_wall(d, m, e2, at, r[k].a - 0.06, r[k].b + 0.06, y, 0.0);
            hk_run(d->pm, PMK_H_BAY_CORNICE, (float)L, 1.0f);
            pop(d);
            /* a bracketed cornice (a rowhouse's Italianate front): small consoles under it */
            if (cs->eaves == HCE_BRACKETS && !cs->lean) {
                const int nb = (int)floor((r[k].b - r[k].a - 0.3) / 0.75) + 1;
                for (int b = 0; b < nb && r[k].b - r[k].a > 0.6; ++b) {
                    const double s = nb > 1 ? r[k].a + 0.15 + (r[k].b - r[k].a - 0.3) * b / (nb - 1) : 0.5 * (r[k].a + r[k].b);
                    const double q[3] = { m[0] * at + e2[0] * s, y, m[1] * at + e2[1] * s };
                    push_wall(d, q, m[0], m[1]);
                    pm_scale(d->pm, 0.5f, 0.5f, 0.5f);
                    pm_place(d->pm, PMK_H_EAVES_BRACKET);
                    pop(d);
                }
            }
        }
    }
    if (cs->quoins && !cs->lean && d->C->fin[u].wall != HW_STONE && d->C->fin[u].wall != HW_SIDING) {
        /* the front's free corners: not at a party wall, not where a wing or a neighbour stands against them */
        for (int c = 0; c < 2; ++c) {
            const int side = c ? (e + 1) % 4 : (e + 3) % 4;
            if (R->cut[side] || R->role[side] == ER_EXT || R->role[side] == ER_HIGH) continue;
            const double *q = c ? R->W.p[(e + 1) % 4] : R->W.p[e];
            const double *n2 = R->W.m[side];
            const double y0 = dy + (cs->plinth && P >= 0.2 ? fmin(0.45, P) : 0.0);
            const double y1 = dy + (v->parapet == 1 ? R->E - (cs->cornice ? 0.4 : -0.6) : R->E - 0.05);
            int ok = y1 - y0 > 1.5;
            for (double y = y0 + 0.3; y < y1 && ok; y += 0.9) {
                const double t[3] = { q[0] + (m[0] + n2[0]) * 0.04, y, q[1] + (m[1] + n2[1]) * 0.04 };
                if (hs_inside(d->C, t, -1, 0.003) >= 0) ok = 0;
            }
            if (!ok) { repair(d, HDR_QUOINS, "a corner is covered (%.0f): no quoins there", u, (double)c); continue; }
            /* the module's faces: A (its z) and B (its x) with A x B = up in the mesh's frame */
            double nA[2] = { m[0], m[1] }, nB[2] = { n2[0], n2[1] };
            const double cyA = nA[1] * nB[0] * d->sx - nA[0] * nB[1] * d->sx;
            if (cyA < 0.0) { const double t0 = nA[0], t1 = nA[1]; nA[0] = nB[0]; nA[1] = nB[1]; nB[0] = t0; nB[1] = t1; }
            float o[3], ez[3] = { (float)(d->sx * nA[0]), 0.0f, (float)nA[1] }, ex[3] = { (float)(d->sx * nB[0]), 0.0f, (float)nB[1] }, ey[3] = { 0, 1, 0 };
            const double p[3] = { q[0], y0, q[1] };
            mesh_pt(d, p, o);
            pm_push(d->pm);
            pm_frame(d->pm, o, ex, ey, ez);
            hk_quoins(d->pm, (float)(y1 - y0));
            pm_pop(d->pm);
        }
    }
    done(d);
}

/* ---------------------------------------------------------------------------
 * Shared with the retrofit layer (home_char_internal.h)
 * ------------------------------------------------------------------------- */

void hc_repair(DC *d, int rule, const char *fmt, int unit, double a) { repair(d, rule, fmt, unit, a); }
void hc_begin(DC *d, int u) { begin(d, u); }
void hc_done(DC *d) { done(d); }
void hc_push_wall(DC *d, const double p[3], double mx, double mz) { push_wall(d, p, mx, mz); }
void hc_pop(DC *d) { pop(d); }
void hc_push_yz(DC *d, const double p[3], const double ey[3], const double ez[3]) {
    float o[3], y[3] = { (float)(d->sx * ey[0]), (float)ey[1], (float)ey[2] }, z[3] = { (float)(d->sx * ez[0]), (float)ez[1], (float)ez[2] };
    const float x[3] = { y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0] };
    mesh_pt(d, p, o);
    pm_push(d->pm);
    pm_frame(d->pm, o, x, y, z);
}
int hc_clear_runs(const Ctx *C, const double p0[3], const double dir[3], const double off[3], double a, double b,
                  double step, double minLen, Ival *out, int max) {
    return clear_runs(C, p0, dir, off, a, b, step, minLen, out, max);
}
int hc_window_near(const DC *d, const double m[2], double at, double s, double y0, double y1, double margin) {
    return window_near(d, m, at, s, y0, y1, margin);
}
void hc_eaves_line(const Roof *R, int e, double d[2], double *len, double *t0, double *t1) { eaves_line(R, e, d, len, t0, t1); }
int hc_is_house_body(int role) { return is_house_body(role); }
void hc_occupy(DC *d, const double lo[3], const double hi[3]) {
    if (d->nocc >= HC_MAX_OCC) return;
    HcBox *b = &d->occ[d->nocc++];
    for (int k = 0; k < 3; ++k) { b->lo[k] = fmin(lo[k], hi[k]); b->hi[k] = fmax(lo[k], hi[k]); }
}
int hc_free(const DC *d, const double lo[3], const double hi[3]) {
    for (int i = 0; i < d->nocc; ++i) {
        const HcBox *b = &d->occ[i];
        if (lo[0] < b->hi[0] && hi[0] > b->lo[0] && lo[1] < b->hi[1] && hi[1] > b->lo[1] && lo[2] < b->hi[2] && hi[2] > b->lo[2]) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * The driver
 * ------------------------------------------------------------------------- */

static int region_boxes_ground(const HGraph *g) { return g->region == PLAN_REGION_SEU || g->region == PLAN_REGION_LAT; }

static void dress_pass(Ctx *C, HDress *D, int *count);

/* The budget (150 instances a house): a unit over it is dressed again leaner (HCharSet.lean: no heads on its back and
 * sides, no quoins, string course or crest, bare hips, sparser rafter tails, no window boxes, rooflight or vent), and only
 * if that is still over is the repair logged. */
void hs_dress(Ctx *C, HDress *D) {
    const HGraph *g = C->g;
    PmInstances *l = D->pm->inst;
    const uint32_t n0 = l ? l->n : 0;
    const HDressStats st0 = D->st;
    int count[HG_MAX_UNITS];
    dress_pass(C, D, count);
    int over = 0;
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u)
        if (count[u] > UNIT_BUDGET) { C->cs[u].lean = 1; over = 1; }
    if (over && l) {
        l->n = n0;
        D->st = st0;
        dress_pass(C, D, count);
    }
}

static void dress_pass(Ctx *C, HDress *D, int *count) {
    DC d;
    memset(&d, 0, sizeof(d));
    d.C = C; d.D = D; d.g = C->g; d.pm = D->pm; d.sx = D->sx < 0.0f ? -1.0 : 1.0;
    d.tagged = d.pm->inst ? d.pm->inst->n : 0;
    const HGraph *g = C->g;
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) {
        home_kit_flags(g, u, &d.part[u]);
        d.doorX[u] = 1e9;
    }
    /* doors first: the reliefs, downpipes and canopies keep clear of what stands round them */
    for (int i = 0; i < C->ndoor; ++i) dress_door(&d, &C->door[i]);
    for (int i = 0; i < C->nwin; ++i) if (!C->win[i].bay) dress_window(&d, &C->win[i]);
    for (int vi = 0; vi < g->nvol; ++vi) if (g->vol[vi].role == HV_BAY) dress_bay(&d, vi);
    dress_posts(&d);
    for (int i = 0; i < C->nroof; ++i) {
        dress_eaves(&d, &C->roof[i]);
        dress_verges(&d, &C->roof[i]);
        dress_ridges(&d, &C->roof[i]);
        dress_slopes(&d, &C->roof[i]);
        dress_front(&d, &C->roof[i]);
    }
    dress_stacks(&d);
    /* HM5c1: the shared retrofit kit over it all, keeping clear of what the character layer set down */
    hs_retrofit(&d);
    int total = 0;
    for (int u = 0; u < g->nunit && u < HG_MAX_UNITS; ++u) {
        total += d.count[u];
        if (d.count[u] > D->st.maxUnit) D->st.maxUnit = d.count[u];
        if (d.count[u] > UNIT_BUDGET && C->cs[u].lean) repair(&d, HDR_BUDGET, "%.0f kit instances, over the house's 150", u, (double)d.count[u]);
        count[u] = d.count[u];
    }
    D->st.instances += total;
}
