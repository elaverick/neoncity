#include "home_internal.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * Houses: repairs and rules (section 13.5's bounded quantities, 13.4's roofs and
 * chimneys, 13.8's doors and paths; the residential roadmap HM2)
 *
 * Every quantity is checked on the volumes' own outlines (a canted bay's
 * trapezoid), never the house's bounding box (section 13.8). Rejections are
 * logged "  rejected <tag>: ...", repairs "  repair <tag>: ...", so home_lab
 * can count them by tag.
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * Outlines
 * ------------------------------------------------------------------------- */

int hvol_outline(const HVol *v, float (*p)[2]) {
    if (v->role == HV_BAY && v->kind == HBAY_CANTED) {
        /* canted at 45 degrees from the wall (z1) out to its front (z0), keeping a 0.6 m face */
        float c = v->z1 - v->z0;
        if (v->x1 - v->x0 - 2.0f * c < 0.6f) c = 0.5f * (v->x1 - v->x0 - 0.6f);
        if (c < 0.0f) c = 0.0f;
        p[0][0] = v->x0 + c; p[0][1] = v->z0;
        p[1][0] = v->x1 - c; p[1][1] = v->z0;
        p[2][0] = v->x1;     p[2][1] = v->z1;
        p[3][0] = v->x0;     p[3][1] = v->z1;
        return 4;
    }
    p[0][0] = v->x0; p[0][1] = v->z0;
    p[1][0] = v->x1; p[1][1] = v->z0;
    p[2][0] = v->x1; p[2][1] = v->z1;
    p[3][0] = v->x0; p[3][1] = v->z1;
    return 4;
}

static void rect_outline(float x0, float z0, float x1, float z1, float (*p)[2]) {
    p[0][0] = x0; p[0][1] = z0; p[1][0] = x1; p[1][1] = z0; p[2][0] = x1; p[2][1] = z1; p[3][0] = x0; p[3][1] = z1;
}

/* The largest gap between two convex outlines along their edges' normals: negative where they
 * overlap (by that much at least), about 0 where they touch. */
static float poly_gap(const float (*a)[2], int na, const float (*b)[2], int nb) {
    float best = -1e30f;
    for (int pass = 0; pass < 2; ++pass) {
        const float (*p)[2] = pass ? b : a;
        const int np = pass ? nb : na;
        for (int i = 0; i < np; ++i) {
            const int j = (i + 1) % np;
            float nx = -(p[j][1] - p[i][1]), nz = p[j][0] - p[i][0];
            const float l = sqrtf(nx * nx + nz * nz);
            if (l < 1e-6f) continue;
            nx /= l; nz /= l;
            float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
            for (int k = 0; k < na; ++k) { const float d = a[k][0] * nx + a[k][1] * nz; a0 = fminf(a0, d); a1 = fmaxf(a1, d); }
            for (int k = 0; k < nb; ++k) { const float d = b[k][0] * nx + b[k][1] * nz; b0 = fminf(b0, d); b1 = fmaxf(b1, d); }
            const float gap = fmaxf(b0 - a1, a0 - b1);
            if (gap > best) best = gap;
        }
    }
    return best;
}

static float vol_gap(const HVol *a, const HVol *b) {
    float pa[6][2], pb[6][2];
    const int na = hvol_outline(a, pa), nb = hvol_outline(b, pb);
    return poly_gap((const float (*)[2])pa, na, (const float (*)[2])pb, nb);
}

static float rect_vol_gap(float x0, float z0, float x1, float z1, const HVol *v) {
    float pa[4][2], pb[6][2];
    rect_outline(x0, z0, x1, z1, pa);
    const int nb = hvol_outline(v, pb);
    return poly_gap((const float (*)[2])pa, 4, (const float (*)[2])pb, nb);
}

/* Whether (x, z) lies inside the outline by more than m. */
static int inside_outline(const HVol *v, float x, float z, float m) {
    float p[6][2];
    const int n = hvol_outline(v, p);
    float area = 0.0f;
    for (int i = 0; i < n; ++i) { const int j = (i + 1) % n; area += p[i][0] * p[j][1] - p[j][0] * p[i][1]; }
    const float s = area > 0.0f ? 1.0f : -1.0f;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float ex = p[j][0] - p[i][0], ez = p[j][1] - p[i][1], l = sqrtf(ex * ex + ez * ez);
        if (l < 1e-6f) continue;
        if (s * (ex * (z - p[i][1]) - ez * (x - p[i][0])) / l <= m) return 0;
    }
    return 1;
}

/* Things standing on the ground: everything but a canopy on its brackets. */
static int grounded(const HVol *v) { return !(v->role == HV_PORCH && v->kind == HP_CANOPY); }
static int habitable(const HVol *v) { return v->role <= HV_EXTENSION || v->role == HV_OUTRIGGER; }
static int main_index(const HGraph *g) {
    for (int i = 0; i < g->nvol; ++i)
        if (g->vol[i].role <= HV_ROW) return i;
    return -1;
}

/* ---------------------------------------------------------------------------
 * Repairs (section 13.10: "apply repairs while rules fail")
 * ------------------------------------------------------------------------- */

/* Removes an attached volume. Doors, chimneys, party walls and parents only refer to mains,
 * which come first, so no reference moves. */
static void remove_vol(HGraph *g, int i) {
    for (int k = i; k + 1 < g->nvol; ++k) g->vol[k] = g->vol[k + 1];
    g->nvol--;
}

static float lean_run(const HVol *v) { return v->high == HSIDE_S || v->high == HSIDE_N ? v->z1 - v->z0 : v->x1 - v->x0; }

/* The lowest a part's eaves may come: a porch or canopy over the door's head, a room's ceiling. */
static float least_eaves(const HGraph *g, const HVol *v) {
    switch (v->role) {
    case HV_PORCH: return g->plinth + (v->kind == HP_CANOPY ? 2.4f : 2.2f);
    case HV_WING:  return v->kind == HWING_GARAGE ? 2.3f : g->plinth + 2.3f;
    default:       return g->plinth + 2.3f;
    }
}

/* A part against its parent's wall stays under the parent's eaves (a lean-to or a flat roof),
 * or under its ridge (a gabled porch or outrigger: a lower ridge running in). */
static void repair_height(HGraph *g, int i, float minPitch, BLog *log) {
    HVol *v = &g->vol[i];
    if (v->parent < 0) return;
    const HVol *p = &g->vol[v->parent];
    if (v->role == HV_PORCH && v->kind == HP_RECESSED) return;
    if (v->roof == HR_LEAN || (v->roof == HR_FLAT && v->role != HV_OUTRIGGER)) {
        const float cap = p->eaves - 0.05f;
        if (v->top <= cap + 1e-4f) return;
        if (v->roof == HR_LEAN) {
            const float run = lean_run(v), want = atanf((cap - v->eaves) / fmaxf(run, 0.1f)) * 57.29578f;
            if (want >= minPitch) {
                home_repair(g, log, "lean", "%s %d: pitch %.0f -> %.0f deg to stay under the eaves", home_role_name(v->role), i, v->pitch, want);
                v->pitch = want;
                home_vol_top(v);
                return;
            }
            const float eaves = cap - tanf(minPitch * 0.0174532925f) * run;
            if (eaves >= least_eaves(g, v)) {
                home_repair(g, log, "lean", "%s %d: eaves lowered %.2f -> %.2f m", home_role_name(v->role), i, v->eaves, eaves);
                v->eaves = eaves;
                v->pitch = minPitch;
                home_vol_top(v);
                return;
            }
            home_repair(g, log, "lean", "%s %d: no lean-to fits under the eaves, flat", home_role_name(v->role), i);
            v->roof = HR_FLAT;
            v->parapet = (uint8_t)(habitable(v) ? 2 : 0);
            v->overhang = 0.0f;
            home_vol_top(v);
        }
        if (v->top > cap + 1e-4f) {
            const float e = cap - (v->top - v->eaves);
            if (e >= least_eaves(g, v)) {
                home_repair(g, log, "height", "%s %d: flat roof lowered to %.2f m", home_role_name(v->role), i, e);
                v->eaves = e;
                home_vol_top(v);
            } else if (v->parapet && !habitable(v)) {
                home_repair(g, log, "height", "%s %d: parapet dropped under the eaves", home_role_name(v->role), i);
                v->parapet = 0;
                home_vol_top(v);
            }
        }
        return;
    }
    if (v->roof == HR_GABLE || v->roof == HR_HIP) {
        const float cap = p->top - 0.3f;
        if (v->top <= cap + 1e-4f) return;
        const float half = 0.5f * (v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0);
        const float want = atanf((cap - v->eaves) / fmaxf(half, 0.1f)) * 57.29578f;
        if (want >= minPitch) {
            home_repair(g, log, "ridge", "%s %d: pitch %.0f -> %.0f deg, its ridge under the main's", home_role_name(v->role), i, v->pitch, want);
            v->pitch = want;
        } else {
            home_repair(g, log, "ridge", "%s %d: its ridge would clear the main's, flat", home_role_name(v->role), i);
            v->roof = HR_FLAT;
            v->parapet = (uint8_t)(habitable(v) ? 2 : 0);
            v->overhang = 0.0f;
            home_vol_top(v);
            repair_height(g, i, minPitch, log);   /* and now under the eaves, as a flat part */
            return;
        }
        home_vol_top(v);
    }
}

/* Moves everything on the plot (the volumes) by dz along z. */
static void shift_house(HGraph *g, float dz) {
    for (int i = 0; i < g->nvol; ++i) { g->vol[i].z0 += dz; g->vol[i].z1 += dz; }
    for (int i = 0; i < g->ndoor; ++i) g->door[i].z += dz;
    for (int i = 0; i < g->nchim; ++i) g->chim[i].z += dz;
    for (int i = 0; i < g->nparty; ++i) { g->party[i].z0 += dz; g->party[i].z1 += dz; }
}

/* Section 13.4: a stack on its roof's ridge (between a hip's hips), at a gable end (on or just
 * outside the wall, on the ridge's line), or on a party wall, never mid-slope. */
static int chimney_placed(const HGraph *g, const HChimney *c) {
    const HVol *v = &g->vol[c->vol];
    if (v->roof != HR_GABLE && v->roof != HR_HIP) return 0;
    const float along = v->ridgeX ? c->x : c->z, across = v->ridgeX ? c->z : c->x;
    const float L0 = v->ridgeX ? v->x0 : v->z0, L1 = v->ridgeX ? v->x1 : v->z1;
    const float ridge = v->ridgeX ? 0.5f * (v->z0 + v->z1) : 0.5f * (v->x0 + v->x1), half = 0.5f * (v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0);
    const int onRidgeLine = fabsf(across - ridge) <= 0.3f;
    if (c->on == HC_RIDGE) {
        const float h0 = v->roof == HR_HIP ? L0 + half : L0, h1 = v->roof == HR_HIP ? L1 - half : L1;
        return onRidgeLine && along >= fminf(h0, h1) - 0.3f && along <= fmaxf(h0, h1) + 0.3f;
    }
    if (c->on == HC_GABLE) return v->roof == HR_GABLE && onRidgeLine && (fabsf(along - L0) <= 0.8f || fabsf(along - L1) <= 0.8f);
    for (int p = 0; p < g->nparty; ++p)
        if (g->party[p].vol == c->vol && fabsf(c->x - g->party[p].x) <= 0.05f && c->z >= v->z0 && c->z <= v->z1) return 1;
    return 0;
}

/* The stacks of a roof that turned or went flat. */
static void drop_chimneys(HGraph *g, int vi, BLog *log, const char *why) {
    int n = 0;
    for (int i = 0; i < g->nchim; ++i)
        if (g->chim[i].vol != vi) g->chim[n++] = g->chim[i];
    if (n < g->nchim) home_repair(g, log, "chimney", "%s %d: %d stacks removed (%s)", home_role_name(g->vol[vi].role), vi, g->nchim - n, why);
    g->nchim = n;
}

static void repair_rear(HGraph *g, int mi, BLog *log) {
    for (int k = 0; k < g->nunit; ++k) {
        float lack = REAR_MIN - (g->unit[k].depth - home_unit_rear(g, k));
        if (lack <= 1e-4f) continue;
        /* the unit's own parts behind first: shorter, then gone */
        for (int i = g->nvol - 1; i >= 0 && lack > 1e-4f; --i) {
            HVol *v = &g->vol[i];
            if (v->unit != k || (v->role != HV_EXTENSION && v->role != HV_OUTRIGGER && v->role != HV_WING)) continue;
            const float least = v->role == HV_WING ? 5.0f : 2.0f;
            const float over = v->z1 - (g->unit[k].depth - REAR_MIN);
            if (over <= 0.0f) continue;
            if (v->z1 - over - v->z0 >= least) {
                home_repair(g, log, "rear garden", "%s %d shortened %.1f m", home_role_name(v->role), i, over);
                v->z1 -= over;
                v->ridgeX = (v->x1 - v->x0) >= (v->z1 - v->z0);
                home_vol_top(v);
            } else {
                home_repair(g, log, "rear garden", "unit %d: %s %d removed", k, home_role_name(v->role), i);
                remove_vol(g, i);
            }
            lack = REAR_MIN - (g->unit[k].depth - home_unit_rear(g, k));
        }
        if (lack <= 1e-4f) continue;
        /* then the main shallower, with what is behind it */
        HVol *m = &g->vol[mi];
        const float cut = fminf(lack, (m->z1 - m->z0) - MIN_DEPTH);
        if (cut > 1e-4f) {
            home_repair(g, log, "rear garden", "main %.1f m shallower", cut);
            const float back = m->z1;
            for (int i = 0; i < g->nvol; ++i)
                if (i != mi && fabsf(g->vol[i].z0 - back) < 0.05f) { g->vol[i].z0 -= cut; g->vol[i].z1 -= cut; }
            m->z1 -= cut;
            for (int i = 0; i < g->nchim; ++i)
                if (g->chim[i].vol == mi) g->chim[i].z = fminf(g->chim[i].z, m->z1 - 0.3f);
            for (int i = 0; i < g->nparty; ++i) g->party[i].z1 = m->z1;
            const int turned = m->ridgeX != ((m->x1 - m->x0) >= (m->z1 - m->z0));
            m->ridgeX = (m->x1 - m->x0) >= (m->z1 - m->z0);
            home_vol_top(m);
            if (turned) drop_chimneys(g, mi, log, "its ridge turned");
            /* chimneys on its ridge follow it */
            for (int i = 0; i < g->nchim; ++i) {
                HChimney *c = &g->chim[i];
                if (c->vol != mi) continue;
                if (m->ridgeX) c->z = 0.5f * (m->z0 + m->z1);
                c->top = fmaxf(c->top, m->top + 0.9f);
            }
            lack -= cut;
        }
        if (lack <= 1e-4f) continue;
        /* last, the house nearer the street, if the front garden and the projections allow */
        float room = m->z0 - FRONT_MIN;
        for (int i = 0; i < g->nvol; ++i)
            if (g->vol[i].z0 < m->z0) room = fminf(room, g->vol[i].z0 - PROJ_MIN);
        if (room >= lack) {
            home_repair(g, log, "rear garden", "the house %.1f m nearer the street", lack);
            shift_house(g, -lack);
            for (int u = 0; u < g->nunit; ++u) g->unit[u].front -= lack;
        }
    }
}

static void repair_projection(HGraph *g, int mi, BLog *log) {
    for (int i = g->nvol - 1; i >= 0; --i) {
        HVol *v = &g->vol[i];
        if ((v->role != HV_PORCH && v->role != HV_BAY) || v->z0 >= PROJ_MIN - 1e-4f || (v->role == HV_PORCH && v->kind == HP_RECESSED)) continue;
        const float deep = v->z1 - PROJ_MIN, least = v->role == HV_BAY ? 0.45f : v->kind == HP_CANOPY ? 0.6f : v->kind == HP_VERANDA ? 1.5f : 1.2f;
        if (deep >= least) {
            home_repair(g, log, "projection", "%s %d: %.1f -> %.1f m deep", home_role_name(v->role), i, v->z1 - v->z0, deep);
            v->z0 = v->z1 - deep;
            home_vol_top(v);
        } else if (v->role == HV_PORCH) {
            home_repair(g, log, "projection", "porch %d: a canopy in a %.1f m garden", i, g->vol[mi].z0);
            v->kind = HP_CANOPY;
            v->z0 = v->z1 - 0.8f;
            v->roof = HR_LEAN; v->high = HSIDE_N;
            v->y0 = g->plinth + 2.25f;
            v->eaves = g->plinth + 2.45f;
            home_vol_top(v);
        } else {
            home_repair(g, log, "projection", "bay %d removed: a %.1f m garden", i, g->vol[mi].z0);
            remove_vol(g, i);
        }
    }
}

/* A stack outside a gable wall that would break the side clearance moves inside it. */
static void repair_chimneys(HGraph *g, int mi, BLog *log) {
    const HVol *m = &g->vol[mi];
    int n = 0;
    for (int i = 0; i < g->nchim; ++i) {   /* a stack the repairs left off its ridge (a hip's ridge shortened) */
        if (chimney_placed(g, &g->chim[i])) g->chim[n++] = g->chim[i];
        else home_repair(g, log, "chimney", "stack %d off its ridge after the repairs: removed", i);
    }
    g->nchim = n;
    for (int i = 0; i < g->nchim; ++i) {
        HChimney *c = &g->chim[i];
        if (c->top < m->top + 0.9f) c->top = m->top + 0.9f;
        if (c->on == HC_PARTY) continue;   /* it straddles its party wall */
        const HUnit *u = &g->unit[c->unit >= 0 && c->unit < g->nunit ? c->unit : 0];
        const float side = g->arch == HOME_DETACHED ? SIDE_DETACHED : g->arch == HOME_SEMI ? SIDE_SEMI : 0.0f;
        const float lo = (g->arch == HOME_SEMI && c->unit == 1) || g->arch == HOME_TERRACE ? u->x0 : u->x0 + side;
        const float hi = (g->arch == HOME_SEMI && c->unit == 0) || g->arch == HOME_TERRACE ? u->x1 : u->x1 - side;
        if (c->x - 0.5f * c->w < lo - 1e-4f) {
            home_repair(g, log, "chimney", "stack %d inside its gable (side clearance)", i);
            c->x = m->x0 + 0.5f * c->w + 0.05f;
        } else if (c->x + 0.5f * c->w > hi + 1e-4f) {
            home_repair(g, log, "chimney", "stack %d inside its gable (side clearance)", i);
            c->x = m->x1 - 0.5f * c->w - 0.05f;
        }
    }
}

void home_repair_all(HGraph *g, int level, BLog *log) {
    (void)level;
    const int mi = main_index(g);
    if (mi < 0) return;
    const float minPitch = home_style_min_pitch(g->style);
    /* section 13.10: a span over 12 m -> flat, "simplify" */
    for (int i = 0; i < g->nvol; ++i) {
        HVol *v = &g->vol[i];
        if (v->roof != HR_GABLE && v->roof != HR_HIP) continue;
        const float span = v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0;
        if (span > SPAN_MAX && v->role != HV_PORCH) {
            home_repair(g, log, "simplify", "%s %d spans %.1f m: flat", home_role_name(v->role), i, span);
            v->roof = HR_FLAT;
            v->parapet = (uint8_t)(habitable(v) ? 2 : 0);
            v->overhang = 0.0f;
            home_vol_top(v);
            drop_chimneys(g, i, log, "a flat roof");
        }
    }
    repair_rear(g, mi, log);
    repair_projection(g, mi, log);
    for (int i = 0; i < g->nvol; ++i) repair_height(g, i, minPitch, log);
    repair_chimneys(g, mi, log);
    for (int i = 0; i < g->nparty; ++i) g->party[i].top = g->vol[mi].top;
}

/* ---------------------------------------------------------------------------
 * Rules
 * ------------------------------------------------------------------------- */

#define REJECT(tag, ...) do { blog_printf(log, "  rejected %s: ", tag); blog_printf(log, __VA_ARGS__); blog_printf(log, "\n"); ok = 0; } while (0)

static int check_volumes(const HGraph *g, int mi, BLog *log) {
    int ok = 1;
    const float minPitch = home_style_min_pitch(g->style);
    const float X0 = g->unit[0].x0, X1 = g->unit[g->nunit - 1].x1, D = g->unit[0].depth;
    for (int i = 0; i < g->nvol; ++i) {
        const HVol *v = &g->vol[i];
        const char *nm = home_role_name(v->role);
        if (v->storeys < 1 || v->storeys > 3) REJECT("storeys", "%s %d has %d storeys", nm, i, v->storeys);
        if (v->x1 - v->x0 < 0.3f || v->z1 - v->z0 < 0.3f) REJECT("shape", "%s %d is %.2f x %.2f m", nm, i, v->x1 - v->x0, v->z1 - v->z0);
        /* supported_by: the ground, but for a canopy on its brackets */
        if (grounded(v) ? fabsf(v->y0) > 1e-4f : (v->parent < 0 || v->y0 <= 0.0f)) REJECT("support", "%s %d stands at %.2f m", nm, i, v->y0);
        /* site fit */
        const float ux0 = v->unit == HG_ROW_WIDE ? X0 : g->unit[v->unit].x0, ux1 = v->unit == HG_ROW_WIDE ? X1 : g->unit[v->unit].x1;
        if (v->unit != HG_ROW_WIDE && v->unit >= g->nunit) REJECT("site", "%s %d belongs to unit %d of %d", nm, i, v->unit, g->nunit);
        else if (v->x0 < ux0 - HEPS || v->x1 > ux1 + HEPS || v->z0 < -HEPS || v->z1 > D + HEPS)
            REJECT("site", "%s %d (%.1f..%.1f, %.1f..%.1f) leaves its plot (%.1f..%.1f, 0..%.1f)", nm, i, v->x0, v->x1, v->z0, v->z1, ux0, ux1, D);
        /* attached_to: what isn't a main hangs on one and touches it */
        if (v->role > HV_ROW) {
            if (v->parent < 0 || v->parent >= g->nvol || g->vol[v->parent].role > HV_ROW) REJECT("attached", "%s %d is attached to nothing", nm, i);
            else if (vol_gap(v, &g->vol[v->parent]) > 0.05f) REJECT("attached", "%s %d stands %.2f m off its main", nm, i, vol_gap(v, &g->vol[v->parent]));
        } else if (i != mi) REJECT("shared roof", "a second main (%s %d)", nm, i);
        /* roofs: a pitched span at most 12 m across the shorter side, at the style's least pitch */
        if (v->roof == HR_GABLE || v->roof == HR_HIP) {
            const float span = v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0, along = v->ridgeX ? v->x1 - v->x0 : v->z1 - v->z0;
            if (v->role != HV_PORCH) {   /* a gabled porch faces the street whatever its shape (a departure, docs/residential-target.md) */
                if (span > SPAN_MAX + 1e-3f) REJECT("span", "%s %d spans %.1f m", nm, i, span);
                if (span > along + HEPS) REJECT("span side", "%s %d spans its longer side (%.1f of %.1f m)", nm, i, span, along);
            }
            if (v->pitch < minPitch - 0.01f || v->pitch > 60.0f) REJECT("pitch", "%s %d pitched at %.1f deg", nm, i, v->pitch);
        } else if (v->roof == HR_LEAN) {
            if (v->parent < 0) REJECT("lean", "%s %d leans against nothing", nm, i);
            else {
                const HVol *p = &g->vol[v->parent];
                const int faces = v->high == HSIDE_N ? fabsf(v->z1 - p->z0) < 0.05f : v->high == HSIDE_S ? fabsf(v->z0 - p->z1) < 0.05f
                                : v->high == HSIDE_E ? fabsf(v->x1 - p->x0) < 0.05f : fabsf(v->x0 - p->x1) < 0.05f;
                if (!faces) REJECT("lean", "%s %d rises away from its parent's wall", nm, i);
            }
            if (v->pitch < minPitch - 0.01f || v->pitch > 60.0f) REJECT("pitch", "%s %d leans at %.1f deg", nm, i, v->pitch);
        } else if (habitable(v) && v->parapet != 2) REJECT("parapet", "%s %d has a flat roof without a parapet", nm, i);
        if (v->top < v->eaves - 1e-4f || v->eaves <= v->y0) REJECT("height", "%s %d: eaves %.2f, top %.2f", nm, i, v->eaves, v->top);
        /* heights against the parent: lower; a lean-to or flat part under its eaves; a ridge running in under its ridge */
        if (v->parent >= 0 && v->parent < g->nvol && !(v->role == HV_PORCH && v->kind == HP_RECESSED)) {
            const HVol *p = &g->vol[v->parent];
            if (v->top > p->top - 0.05f) REJECT("height", "%s %d (%.2f m) reaches its main's top (%.2f)", nm, i, v->top, p->top);
            if ((v->roof == HR_LEAN || (v->roof == HR_FLAT && v->role != HV_OUTRIGGER)) && v->top > p->eaves - 0.05f + 1e-3f)
                REJECT("height", "%s %d (%.2f m) rises over its main's eaves (%.2f)", nm, i, v->top, p->eaves);
            if (v->role == HV_EXTENSION && (v->eaves > p->eaves || v->storeys > 1)) REJECT("extension", "rear extension %d is not lower than its main", i);
            if (v->storeys > p->storeys) REJECT("storeys", "%s %d has more storeys (%d) than its main (%d)", nm, i, v->storeys, p->storeys);
        }
        /* a garage wing: set back from the front, a lean-to or flat roof, a metre off the boundary */
        if (v->role == HV_WING && v->kind == HWING_GARAGE && v->parent >= 0) {
            const HVol *p = &g->vol[v->parent];
            if (v->roof != HR_LEAN && v->roof != HR_FLAT) REJECT("garage", "garage %d has a %s roof", i, home_roof_name(v->roof));
            if (v->z0 < p->z0 + 0.3f) REJECT("garage", "garage %d is not set back from the front wall", i);
            if (v->x0 < ux0 + GARAGE_CLEAR - HEPS || v->x1 > ux1 - GARAGE_CLEAR + HEPS) REJECT("garage clearance", "garage %d is under %.0f m from the plot's side", i, GARAGE_CLEAR);
        }
        /* bays and porches on the front, short of the street */
        if ((v->role == HV_BAY || v->role == HV_PORCH) && !(v->role == HV_PORCH && v->kind == HP_RECESSED)) {
            if (v->z0 < PROJ_MIN - HEPS) REJECT("projection", "%s %d comes within %.2f m of the street", nm, i, v->z0);
            if (v->parent >= 0 && fabsf(v->z1 - g->vol[v->parent].z0) > HEPS) REJECT("projection", "%s %d is not on the front wall", nm, i);
        }
    }
    /* no volume overlaps another unless attached to it */
    for (int i = 0; i < g->nvol; ++i)
        for (int j = i + 1; j < g->nvol; ++j) {
            const HVol *a = &g->vol[i], *b = &g->vol[j];
            if (a->parent == j || b->parent == i) continue;
            const float gap = vol_gap(a, b);
            if (gap < -HEPS) REJECT("overlap", "%s %d and %s %d overlap by %.2f m", home_role_name(a->role), i, home_role_name(b->role), j, -gap);
        }
    return ok;
}

static int check_plot(const HGraph *g, int mi, BLog *log) {
    int ok = 1;
    const HVol *m = &g->vol[mi];
    if (m->z0 < FRONT_MIN - HEPS) REJECT("front garden", "%.2f m deep", m->z0);
    for (int k = 0; k < g->nunit; ++k) {
        const float rear = g->unit[k].depth - home_unit_rear(g, k);
        if (rear < REAR_MIN - HEPS) REJECT("rear garden", "unit %d's is %.2f m deep", k, rear);
    }
    /* the row: one shared roof over its units, party walls between them */
    const int wantRole = g->arch == HOME_TERRACE ? HV_ROW : g->arch == HOME_SEMI ? HV_PAIR : HV_MAIN;
    if (m->role != wantRole) REJECT("shared roof", "a %s's main is a %s", g->arch == HOME_TERRACE ? "terrace" : g->arch == HOME_SEMI ? "semi" : "house", home_role_name(m->role));
    if (g->arch == HOME_TERRACE) {
        if (m->x1 - m->x0 > ROW_MAX + 1e-3f) REJECT("row", "%.1f m long", m->x1 - m->x0);
        if (g->nunit < 2) REJECT("row", "%d unit", g->nunit);
        for (int k = 0; k < g->nunit; ++k)
            if (g->unit[k].x1 - g->unit[k].x0 < UNIT_MIN - 1e-3f) REJECT("unit width", "unit %d is %.2f m wide", k, g->unit[k].x1 - g->unit[k].x0);
    }
    if (g->arch == HOME_SEMI && g->nunit != 2) REJECT("row", "a pair of %d halves", g->nunit);
    if (g->arch != HOME_DETACHED) {
        if (g->nparty != g->nunit - 1) REJECT("party wall", "%d party walls for %d units", g->nparty, g->nunit);
        for (int i = 0; i < g->nparty; ++i) {
            const HParty *p = &g->party[i];
            if (p->b != p->a + 1 || p->a < 0 || p->b >= g->nunit) { REJECT("party wall", "wall %d joins units %d and %d", i, p->a, p->b); continue; }
            if (fabsf(p->x - g->unit[p->a].x1) > 1e-3f || fabsf(p->x - g->unit[p->b].x0) > 1e-3f || p->x <= m->x0 + 1.0f || p->x >= m->x1 - 1.0f)
                REJECT("party wall", "wall %d at %.2f is not between its units", i, p->x);
            if (p->vol != mi || fabsf(p->z0 - m->z0) > HEPS || fabsf(p->z1 - m->z1) > HEPS || p->top < m->top - HEPS)
                REJECT("party wall", "wall %d does not run through the main to its roof", i);
        }
    }
    /* clearances to the plot's sides: a detached house's 2 m each side, a semi's 1.2 on its open side
     * (a garage wing keeps its own metre); a stack counts with the walls */
    if (g->arch != HOME_TERRACE) {
        for (int i = 0; i < g->nvol + g->nchim; ++i) {
            float x0, x1;
            int unit;
            if (i < g->nvol) {
                const HVol *v = &g->vol[i];
                if (v->role == HV_WING && v->kind == HWING_GARAGE) continue;
                x0 = v->x0; x1 = v->x1; unit = v->unit;
            } else {
                const HChimney *c = &g->chim[i - g->nvol];
                x0 = c->x - 0.5f * c->w; x1 = c->x + 0.5f * c->w; unit = -1;
            }
            if (g->arch == HOME_DETACHED) {
                const float l = x0 - g->unit[0].x0, r = g->unit[0].x1 - x1;
                if (fminf(l, r) < SIDE_DETACHED - HEPS) REJECT("side clearance", "%s %d is %.2f m from the plot's side", i < g->nvol ? home_role_name(g->vol[i].role) : "chimney", i, fminf(l, r));
            } else {
                const float l = x0 - g->unit[0].x0, r = g->unit[1].x1 - x1;
                (void)unit;
                if (fminf(l, r) < SIDE_SEMI - HEPS) REJECT("open side", "%s %d is %.2f m from the pair's open side", i < g->nvol ? home_role_name(g->vol[i].role) : "chimney", i, fminf(l, r));
            }
        }
    }
    return ok;
}

static int check_doors(const HGraph *g, int mi, BLog *log) {
    int ok = 1;
    const HVol *m = &g->vol[mi];
    for (int k = 0; k < g->nunit; ++k) {
        const HUnit *u = &g->unit[k];
        if (u->door < 0 || u->door >= g->ndoor || g->door[u->door].unit != k) { REJECT("door", "unit %d has no front door", k); continue; }
        const HDoor *d = &g->door[u->door];
        if (d->width < DOOR_MIN - 1e-3f) REJECT("door width", "unit %d's door is %.2f m wide", k, d->width);
        if (d->side != HSIDE_S) REJECT("door side", "unit %d's door does not face the street", k);
        const float d0 = d->x - 0.5f * d->width, d1 = d->x + 0.5f * d->width;
        if (d0 < u->x0 - HEPS || d1 > u->x1 + HEPS || d0 < m->x0 - HEPS || d1 > m->x1 + HEPS) REJECT("door wall", "unit %d's door is off its front", k);
        /* in the front wall, or at the back of its recessed porch; and under a porch or canopy */
        int porch = -1;
        for (int i = 0; i < g->nvol; ++i) {
            const HVol *p = &g->vol[i];
            if (p->role == HV_PORCH && p->unit == k && p->x0 <= d0 + HEPS && p->x1 >= d1 - HEPS && fabsf(p->z1 - d->z) < HEPS) porch = i;
        }
        if (porch < 0) REJECT("porch", "unit %d's door has no porch or canopy over it", k);
        const int recessed = porch >= 0 && g->vol[porch].kind == HP_RECESSED;
        if (!recessed && fabsf(d->z - m->z0) > HEPS) REJECT("door wall", "unit %d's door is not in the front wall", k);
        /* opens_on: a path to the street that crosses no building (the volumes' own outlines) */
        if (d->path < 0 || d->path >= g->nsite || g->site[d->path].kind != HE_PATH) { REJECT("path", "unit %d's door has no path", k); continue; }
        const HSite *p = &g->site[d->path];
        if (p->z0 > HEPS) REJECT("path", "unit %d's path stops %.2f m short of the street", k, p->z0);
        if (p->x0 > d->x - 0.3f || p->x1 < d->x + 0.3f) REJECT("path", "unit %d's path misses its door", k);
        if (p->x0 < u->x0 - HEPS || p->x1 > u->x1 + HEPS) REJECT("path", "unit %d's path leaves its plot", k);
        const float reach = porch >= 0 && !recessed && g->vol[porch].kind != HP_CANOPY ? g->vol[porch].z0 : m->z0;
        if (p->z1 < reach - HEPS) REJECT("path", "unit %d's path stops %.2f m short of its door", k, reach - p->z1);
        for (int i = 0; i < g->nvol; ++i) {
            const HVol *v = &g->vol[i];
            if (!grounded(v)) continue;
            const float gap = rect_vol_gap(p->x0, p->z0, p->x1, p->z1, v);
            if (gap < -HEPS) REJECT("path crosses", "unit %d's path crosses %s %d", k, home_role_name(v->role), i);
        }
        /* a gate where the path meets a front boundary */
        int bounded = 0, gated = 0;
        for (int i = 0; i < g->nbound; ++i) {
            const HBound *b = &g->bound[i];
            if (b->role != HBR_FRONT || b->unit != k) continue;
            bounded = 1;
            if (fminf(b->x0, b->x1) < p->x1 - HEPS && fmaxf(b->x0, b->x1) > p->x0 + HEPS) REJECT("gate", "unit %d's front boundary closes its path", k);
        }
        for (int i = 0; i < g->ngate; ++i)
            if (g->gate[i].unit == k && !g->gate[i].drive && g->gate[i].x - 0.5f * g->gate[i].w <= p->x0 + 0.15f && g->gate[i].x + 0.5f * g->gate[i].w >= p->x1 - 0.15f) gated = 1;
        if (bounded && !gated) REJECT("gate", "unit %d's path has no gate", k);
    }
    if (g->ndoor != g->nunit) REJECT("door", "%d doors for %d units", g->ndoor, g->nunit);
    return ok;
}

/* section 13.4: on the ridge, at a gable end, or on a party wall, never mid-slope */
static int check_chimneys(const HGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nchim; ++i) {
        const HChimney *c = &g->chim[i];
        if (c->vol < 0 || c->vol >= g->nvol) { REJECT("chimney", "stack %d stands on nothing", i); continue; }
        const HVol *v = &g->vol[c->vol];
        if (v->roof != HR_GABLE && v->roof != HR_HIP) { REJECT("chimney", "stack %d on a %s roof", i, home_roof_name(v->roof)); continue; }
        const int okPlace = chimney_placed(g, c);
        if (!okPlace) REJECT("chimney mid-slope", "stack %d (%.1f, %.1f) is not on a ridge, gable end or party wall", i, c->x, c->z);
        if (c->top < v->top + 0.6f) REJECT("chimney height", "stack %d tops out %.2f m over the ridge", i, c->top - v->top);
        if (c->x - 0.5f * c->w < g->unit[0].x0 - HEPS || c->x + 0.5f * c->w > g->unit[g->nunit - 1].x1 + HEPS)
            REJECT("site", "stack %d leaves the plot", i);
        for (int j = i + 1; j < g->nchim; ++j)
            if (fabsf(g->chim[j].x - c->x) < 0.5f * (g->chim[j].w + c->w) && fabsf(g->chim[j].z - c->z) < 0.5f * (g->chim[j].d + c->d))
                REJECT("chimney", "stacks %d and %d overlap", i, j);
    }
    return ok;
}

/* The garden: boundaries cross no building; paths, drives, sheds, cars and trees on their plot,
 * and the scattered ones (sheds, trees) clear of the buildings, the paths and the drives. */
static int check_garden(const HGraph *g, BLog *log) {
    int ok = 1;
    for (int i = 0; i < g->nbound; ++i) {
        const HBound *b = &g->bound[i];
        const float l = hypotf(b->x1 - b->x0, b->z1 - b->z0);
        const int n = (int)(l / 0.25f) + 1;
        for (int s = 0; s <= n; ++s) {
            const float t = (float)s / (float)n, x = b->x0 + t * (b->x1 - b->x0), z = b->z0 + t * (b->z1 - b->z0);
            int hit = -1;
            for (int v = 0; v < g->nvol && hit < 0; ++v)
                if (grounded(&g->vol[v]) && inside_outline(&g->vol[v], x, z, HEPS)) hit = v;
            if (hit >= 0) { REJECT("boundary", "boundary %d crosses %s %d", i, home_role_name(g->vol[hit].role), hit); break; }
        }
    }
    for (int i = 0; i < g->nsite; ++i) {
        const HSite *s = &g->site[i];
        if (s->unit < 0 || s->unit >= g->nunit) { REJECT("site", "element %d belongs to no unit", i); continue; }
        const HUnit *u = &g->unit[s->unit];
        if (s->x0 < u->x0 - HEPS || s->x1 > u->x1 + HEPS || s->z0 < -HEPS || s->z1 > u->depth + HEPS) REJECT("site", "element %d leaves its plot", i);
        if (s->kind == HE_FRONT || s->kind == HE_REAR || s->kind == HE_PATH) continue;
        for (int v = 0; v < g->nvol; ++v) {
            const HVol *w = &g->vol[v];
            if (!grounded(w)) continue;
            const float gap = rect_vol_gap(s->x0, s->z0, s->x1, s->z1, w);
            if (gap < -HEPS) REJECT(s->kind == HE_DRIVE ? "drive" : "garden clear", "element %d (kind %d) crosses %s %d", i, s->kind, home_role_name(w->role), v);
        }
        if (s->kind == HE_SHED || s->kind == HE_TREE)
            for (int j = 0; j < g->nsite; ++j) {
                const HSite *t = &g->site[j];
                if (j == i || t->kind == HE_FRONT || t->kind == HE_REAR) continue;
                if (s->x1 > t->x0 + HEPS && s->x0 < t->x1 - HEPS && s->z1 > t->z0 + HEPS && s->z0 < t->z1 - HEPS)
                    REJECT("garden clear", "element %d (kind %d) stands on element %d (kind %d)", i, s->kind, j, t->kind);
            }
        if (s->kind == HE_CAR) {
            int on = 0;
            for (int j = 0; j < g->nsite && !on; ++j) {
                const HSite *t = &g->site[j];
                on = t->kind == HE_DRIVE && s->x0 >= t->x0 - HEPS && s->x1 <= t->x1 + HEPS && s->z0 >= t->z0 - HEPS && s->z1 <= t->z1 + HEPS;
            }
            if (!on) REJECT("car", "car %d is not on a drive", i);
        }
    }
    return ok;
}

int home_validate(const HGraph *g, BLog *log) {
    if (g->empty) return 1;
    int ok = 1;
    const int mi = main_index(g);
    if (g->nunit < 1) { REJECT("fit", "no units"); return 0; }
    if (mi < 0) { REJECT("fit", "no main volume"); return 0; }
    ok &= check_volumes(g, mi, log);
    ok &= check_plot(g, mi, log);
    ok &= check_doors(g, mi, log);
    ok &= check_chimneys(g, log);
    ok &= check_garden(g, log);
    return ok;
}
#undef REJECT
