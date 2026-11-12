#include "pmodel.h"
#include "pmodel_food.h"
#include "mathlib.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The street-food trades' modules (roadmap SF1; pmodel.h PMK_CHAR_GRILL on):
 * the stations that stand on a bar's flat range top (a charcoal grill, the
 * vertical spit, griddles, the fryer, crepe plates, the urn and espresso
 * machine, a chestnut wok and a roasting pan), what they cook (skewers,
 * patties, sausages, buns, flatbreads, tortillas, fried food, crepes,
 * dumplings, pans of fillings, a pot of curry), and the trades' neon icons.
 * Frames as pmodel_kit.c's: what stands, stands on y 0 centred; a station's
 * front faces +z (the cook). Heights the line needs are in pmodel_food.h.
 * ========================================================================= */

#define L0 (pm->lod == PM_LOD0)
#define L2 (pm->lod == PM_LOD2)
#define HALF_PI 1.5707963f

static void lathe(Pm *pm, const PmProf *p, int n, int segs) { pm_lathe(pm, p, n, pm_segs(pm, segs, 4), 0, 0); }
static void shell(Pm *pm, const PmProf *o, int n, float t, float rim, int segs) {
    PmProf s[120];
    int k = pm_prof_shell(o, n, t, rim, L0 ? 6 : 3, s, 120);
    if (k) lathe(pm, s, k, segs);
}
static void band(Pm *pm, float r0, float r1, float y0, float y1, int segs) {
    PmProf p[5] = { { r0, y0, 1 }, { r1, y0, 1 }, { r1, y1, 1 }, { r0, y1, 1 }, { r0, y0, 1 } };
    lathe(pm, p, 5, segs);
}
static void bx(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    const float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    const float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    pm_bbox(pm, c, h, bevel, 2);
}
/* a small seeded stream (modules are fixed shapes: the same scatter every time) */
static float rnd(uint32_t *h) { *h = *h * 1664525u + 1013904223u; return (float)(*h >> 8) / 16777216.0f; }
/* a knob on a front face (z), its face out +z */
static void knob_at(Pm *pm, float x, float y, float z) {
    pm_push(pm); pm_translate(pm, x, y, z); pm_rotate_x(pm, HALF_PI); pm_place(pm, PMK_KNOB); pm_pop(pm);
}
/* a pilot lamp on a front face */
static void pilot_at(Pm *pm, float x, float y, float z) {
    pm_mat(pm, PMAT_RED);
    pm_push(pm); pm_translate(pm, x, y, z); pm_rotate_x(pm, HALF_PI);
    PmProf lp[4] = { { 0, 0, 0 }, { 0.006f, 0, 1 }, { 0.006f, 0.005f, 0 }, { 0, 0.008f, 0 } };
    lathe(pm, lp, 4, 12);
    pm_pop(pm);
}
/* rubber feet under a box's corners, y 0 .. h */
static void feet(Pm *pm, float hx, float hz, float h) {
    pm_mat(pm, PMAT_RUBBER);
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? hx : -hx, z = (k & 2) ? hz : -hz;
        bx(pm, x - 0.015f, 0.0f, z - 0.015f, x + 0.015f, h, z + 0.015f, 0.003f);
    }
}
/* a closed lump: a squashed lathe of radius r, height h, its bumps from seed (food) */
static void lump(Pm *pm, float r, float h, uint32_t seed, int segs) {
    PmProf p[7];
    uint32_t s = seed;
    const float j[4] = { 0.9f + 0.2f * rnd(&s), 0.9f + 0.2f * rnd(&s), 0.85f + 0.3f * rnd(&s), 0.8f + 0.3f * rnd(&s) };
    p[0] = (PmProf){ 0, 0, 0 };
    p[1] = (PmProf){ 0.7f * r * j[0], 0.02f * h, 0 };
    p[2] = (PmProf){ r * j[1], 0.3f * h, 0 };
    p[3] = (PmProf){ 0.92f * r * j[2], 0.65f * h, 0 };
    p[4] = (PmProf){ 0.55f * r * j[3], 0.92f * h, 0 };
    p[5] = (PmProf){ 0, h, 0 };
    pm_lathe(pm, p, 6, pm_segs(pm, segs, 5), 0, 0);
}

/* ------------------------------------------------------------ the grill --- */
/* A charcoal grill (yakitori), 0.80 x 0.30: a blackened steel trough on stubby feet, a bed of ash and coals,
 * the skewers laid across its rims (FOOD_GRILL_RIM over its foot); a sliding vent and end handles. */
void m_char_grill(Pm *pm) {
    const float W = 0.80f, D = 0.30f, y0 = 0.03f, rim = FOOD_GRILL_RIM, t = 0.004f;
    pm_mat(pm, PMAT_CAST_IRON);
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1) ? 0.5f * W - 0.04f : -0.5f * W + 0.04f, z = (k & 2) ? 0.5f * D - 0.04f : -0.5f * D + 0.04f;
        bx(pm, x - 0.02f, 0.0f, z - 0.02f, x + 0.02f, y0, z + 0.02f, 0.003f);
    }
    bx(pm, -0.5f * W, y0, -0.5f * D, 0.5f * W, y0 + t, 0.5f * D, 0.0f);
    bx(pm, -0.5f * W, y0 + t, 0.5f * D - t, 0.5f * W, rim, 0.5f * D, 0.0f);
    bx(pm, -0.5f * W, y0 + t, -0.5f * D, 0.5f * W, rim, -0.5f * D + t, 0.0f);
    bx(pm, -0.5f * W, y0 + t, -0.5f * D + t, -0.5f * W + t, rim, 0.5f * D - t, 0.0f);
    bx(pm, 0.5f * W - t, y0 + t, -0.5f * D + t, 0.5f * W, rim, 0.5f * D - t, 0.0f);
    /* the rims rolled over (the skewers rest on them) */
    pm_mat(pm, PMAT_STEEL);
    bx(pm, -0.5f * W - 0.004f, rim - 0.012f, 0.5f * D - 0.002f, 0.5f * W + 0.004f, rim, 0.5f * D + 0.01f, 0.003f);
    bx(pm, -0.5f * W - 0.004f, rim - 0.012f, -0.5f * D - 0.01f, 0.5f * W + 0.004f, rim, -0.5f * D + 0.002f, 0.003f);
    /* the vent's sliding plate on the front, its knob */
    bx(pm, -0.16f, y0 + 0.02f, 0.5f * D, 0.16f, y0 + 0.06f, 0.5f * D + 0.004f, 0.001f);
    if (!L2) bx(pm, 0.1f, y0 + 0.032f, 0.5f * D + 0.004f, 0.12f, y0 + 0.048f, 0.5f * D + 0.02f, 0.003f);
    /* end handles */
    for (int e = -1; e <= 1; e += 2) {
        PmPath pa;
        pm_path_init(&pa, 4);
        pm_path_add(&pa, (float)e * 0.5f * W, rim - 0.04f, -0.07f, 1);
        pm_path_add(&pa, (float)e * (0.5f * W + 0.04f), rim - 0.04f, -0.07f, 1);
        pm_path_add(&pa, (float)e * (0.5f * W + 0.04f), rim - 0.04f, 0.07f, 1);
        pm_path_add(&pa, (float)e * 0.5f * W, rim - 0.04f, 0.07f, 1);
        PmPath pf;
        pm_path_init(&pf, 24);
        pm_path_fillet(&pa, 0.012f, 3, &pf);
        pm_tube(pm, &pf, 0.005f, 8, 3);
        pm_path_free(&pa); pm_path_free(&pf);
    }
    /* ash, then the coals: lumps of charcoal, most of them alight */
    pm_mat(pm, PMAT_GRIT);
    bx(pm, -0.5f * W + t, y0 + t, -0.5f * D + t, 0.5f * W - t, FOOD_GRILL_BED - 0.025f, 0.5f * D - t, 0.0f);
    pm_mat(pm, PMAT_EMBER);
    bx(pm, -0.5f * W + t, FOOD_GRILL_BED - 0.025f, -0.5f * D + t, 0.5f * W - t, FOOD_GRILL_BED - 0.012f, 0.5f * D - t, 0.0f);
    const int n = L0 ? 90 : L2 ? 0 : 30;
    uint32_t h = 0xC0A1u;
    for (int i = 0; i < n; ++i) {
        const float x = (rnd(&h) - 0.5f) * (W - 0.06f), z = (rnd(&h) - 0.5f) * (D - 0.06f);
        const float sx = 0.014f + 0.012f * rnd(&h), sy = 0.01f + 0.008f * rnd(&h), sz = 0.014f + 0.012f * rnd(&h);
        const float yaw = 6.2831853f * rnd(&h), tilt = 0.5f * (rnd(&h) - 0.5f);
        pm_mat(pm, rnd(&h) < 0.72f ? PMAT_EMBER : PMAT_CAST_IRON);
        pm_push(pm);
        pm_translate(pm, x, FOOD_GRILL_BED - 0.01f + 0.01f * rnd(&h), z);
        pm_rotate_y(pm, yaw); pm_rotate_x(pm, tilt);
        pm_box(pm, -sx, -sy, -sz, sx, sy, sz);
        pm_pop(pm);
    }
}

/* Eight skewers side by side along x (0.04 apart), each 0.30 long along z, four chunks of meat on each; the
 * sticks' undersides on y 0 (the grill's rims). */
void m_skewers(Pm *pm) {
    uint32_t h = 0x5E3Eu;
    for (int s = 0; s < 8; ++s) {
        const float x = -0.14f + 0.04f * (float)s;
        pm->part = (uint8_t)s;
        pm_mat(pm, PMAT_BAMBOO);
        float a[3] = { x, 0.0022f, -0.15f }, b[3] = { x, 0.0022f, 0.15f };
        pm_beam(pm, a, b, 0.0045f, 0.0045f, 0.0f, NULL);
        pm_mat(pm, PMAT_MEAT);
        for (int k = 0; k < 4; ++k) {
            const float z = -0.07f + 0.038f * (float)k + 0.006f * (rnd(&h) - 0.5f);
            const float r = 0.013f + 0.003f * rnd(&h), l = 0.028f + 0.006f * rnd(&h);
            pm_push(pm);
            pm_translate(pm, x, 0.0022f, z - 0.5f * l);
            pm_rotate_z(pm, 6.2831853f * rnd(&h));
            pm_rotate_x(pm, HALF_PI);   /* the lump's axis along the stick */
            if (L2) pm_box(pm, -r, 0.0f, -r, r, l, r); else lump(pm, r, l, h, L0 ? 7 : 5);
            pm_pop(pm);
        }
    }
    pm->part = 0;
}

/* A shallow tray, w x d (over its flange), deep, its rounded corners: a stainless sheet shell. */
static void tray(Pm *pm, float w, float d, float deep) {
    const float rc = 0.02f, hx = 0.5f * w - rc - 0.008f, hz = 0.5f * d - rc - 0.008f;
    pm_mat(pm, PMAT_STAINLESS);
    if (L2) { bx(pm, -0.5f * w, 0.0f, -0.5f * d, 0.5f * w, deep, 0.5f * d, 0.0f); return; }
    PmProf o[6] = { { 0, 0, 0 }, { rc - 0.008f, 0, 0 }, { rc - 0.004f, 0.003f, 0 }, { rc, deep - 0.004f, 0 }, { rc + 0.003f, deep, 1 }, { rc + 0.008f, deep, 0 } };
    PmProf s[40];
    int k = pm_prof_shell(o, 6, 0.0012f, 0.0f, 2, s, 40);
    pm_lathe(pm, s, k, pm_segs(pm, 24, 4), hx, hz);
}

/* A tray of raw skewers waiting (0.34 x 0.22), six along x. */
void m_skewer_tray(Pm *pm) {
    tray(pm, 0.34f, 0.22f, 0.03f);
    uint32_t h = 0x7EA1u;
    for (int s = 0; s < 6; ++s) {
        const float z = -0.075f + 0.03f * (float)s;
        pm->part = (uint8_t)s;
        pm_mat(pm, PMAT_BAMBOO);
        float a[3] = { -0.15f, 0.014f, z }, b[3] = { 0.15f, 0.014f, z };
        pm_beam(pm, a, b, 0.004f, 0.004f, 0.0f, NULL);
        pm_mat(pm, PMAT_MEAT);
        for (int k = 0; k < 4; ++k) {
            pm_push(pm);
            pm_translate(pm, -0.075f + 0.036f * (float)k, 0.014f, z);
            pm_rotate_x(pm, 6.2831853f * rnd(&h));
            pm_rotate_z(pm, -HALF_PI);   /* along the stick (x) */
            if (L2) pm_box(pm, -0.012f, 0.0f, -0.012f, 0.012f, 0.03f, 0.012f); else lump(pm, 0.012f, 0.03f, h, L0 ? 7 : 5);
            pm_pop(pm);
        }
    }
    pm->part = 0;
}

/* ------------------------------------------------------------- the spit --- */
/* The vertical spit (doner, shawarma): a drip tray, the column at the back with two radiant panels glowing
 * behind their guards, the arm over the top and its motor, the rod and the cone of meat on its plate. */
void m_spit(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.20f, 0.0f, -0.17f, 0.20f, 0.045f, 0.17f, 0.004f);         /* the drip tray */
    bx(pm, -0.20f, 0.045f, -0.17f, 0.20f, 0.62f, -0.11f, 0.004f);       /* the column */
    bx(pm, -0.03f, 0.60f, -0.11f, 0.03f, 0.635f, 0.06f, 0.003f);        /* the arm */
    bx(pm, -0.04f, 0.635f, -0.03f, 0.04f, 0.69f, 0.08f, 0.006f);        /* the motor */
    /* the radiants: two columns of infrared ceramic tiles (2 x 5 a side) in a dark steel frame, each tile its own
     * box so its glow is shaded in its own metres (kit.glsl kitGlow) */
    for (int s = -1; s <= 1; s += 2) {
        const float cx = (float)s * 0.105f;
        pm_mat(pm, PMAT_STEEL);
        bx(pm, cx - 0.072f, 0.095f, -0.114f, cx + 0.072f, 0.565f, -0.108f, 0.0f);
        /* every level keeps its tiles: the kit draws this module at its far level from a couple of metres */
        pm_mat(pm, PMAT_RADIANT);
        for (int c = 0; c < 2; ++c)
            for (int r = 0; r < 5; ++r) {
                pm->part = (uint8_t)(s > 0 ? 10 + c * 5 + r : c * 5 + r);
                pm_push(pm);
                pm_translate(pm, cx - 0.033f + 0.066f * (float)c, 0.14f + 0.0915f * (float)r, -0.104f);
                pm_box(pm, -0.031f, -0.0435f, -0.004f, 0.031f, 0.0435f, 0.004f);
                pm_pop(pm);
            }
        /* the tiles' clamp bars across */
        pm_mat(pm, PMAT_STAINLESS);
        if (L0) for (int r = 0; r <= 5; ++r) bx(pm, cx - 0.07f, 0.0935f + 0.0915f * (float)r, -0.103f, cx + 0.07f, 0.0965f + 0.0915f * (float)r, -0.097f, 0.0f);
    }
    pm->part = 0;
    pm_mat(pm, PMAT_STEEL);
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.03f);
    pm_cyl(pm, 0.045f, 0.60f, 0.006f, pm_segs(pm, 12, 6));
    PmProf pl[5] = { { 0, 0.068f, 0 }, { 0.085f, 0.068f, 1 }, { 0.088f, 0.072f, 0 }, { 0.085f, 0.076f, 1 }, { 0, 0.076f, 0 } };
    pm_mat(pm, PMAT_STAINLESS);
    lathe(pm, pl, 5, 32);
    /* the cone, wider at the top, its outside carved in uneven bands */
    pm_mat(pm, PMAT_MEAT);
    PmProf c[14];
    uint32_t h = 0xD0E2u;
    int n = 0;
    c[n++] = (PmProf){ 0, 0.076f, 0 };
    const int bands = L0 ? 10 : L2 ? 3 : 5;
    for (int k = 0; k <= bands; ++k) {
        const float t = (float)k / (float)bands, y = 0.08f + 0.46f * t;
        c[n++] = (PmProf){ (0.068f + 0.042f * t) * (0.95f + 0.1f * rnd(&h)), y, 0 };
    }
    c[n++] = (PmProf){ 0.06f, 0.565f, 0 };
    c[n++] = (PmProf){ 0, 0.57f, 0 };
    lathe(pm, c, n, 28);
    pm_pop(pm);
}

/* ---------------------------------------------------------- the griddle --- */
/* A flat griddle w x d: a stainless body on rubber feet, two knobs and a pilot on its front, a steel plate on
 * top (FOOD_GRIDDLE_TOP), a grease trough along the front, splash guards round the back and sides. */
static void griddle(Pm *pm, float W, float D) {
    const float top = FOOD_GRIDDLE_TOP;
    feet(pm, 0.5f * W - 0.04f, 0.5f * D - 0.04f, 0.02f);
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.5f * W, 0.02f, -0.5f * D, 0.5f * W, top - 0.015f, 0.5f * D, 0.004f);
    pm_mat(pm, PMAT_CAST_IRON);
    bx(pm, -0.5f * W, top - 0.015f, -0.5f * D, 0.5f * W, top, 0.5f * D - 0.06f, 0.002f);
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.5f * W, top - 0.015f, 0.5f * D - 0.06f, 0.5f * W, top - 0.008f, 0.5f * D, 0.0f);       /* the trough's floor */
    bx(pm, -0.5f * W, top - 0.008f, 0.5f * D - 0.006f, 0.5f * W, top + 0.01f, 0.5f * D, 0.002f);     /* its lip */
    bx(pm, -0.5f * W, top, -0.5f * D, 0.5f * W, top + 0.09f, -0.5f * D + 0.004f, 0.0015f);          /* the back guard */
    for (int s = -1; s <= 1; s += 2) {
        const float x = (float)s * 0.5f * W;
        bx(pm, s < 0 ? x : x - 0.004f, top, -0.5f * D, s < 0 ? x + 0.004f : x, top + 0.09f, 0.5f * D - 0.12f, 0.0015f);
    }
    for (int k = -1; k <= 1; k += 2) knob_at(pm, (float)k * 0.25f * W, 0.5f * (0.02f + top - 0.015f), 0.5f * D);
    pilot_at(pm, 0.0f, 0.5f * (0.02f + top - 0.015f), 0.5f * D);
}
void m_griddle(Pm *pm) { griddle(pm, 0.75f, 0.50f); }
void m_griddle_s(Pm *pm) { griddle(pm, 0.40f, 0.45f); }

void m_patty(Pm *pm) {
    pm_mat(pm, PMAT_MEAT);
    PmProf p[6] = { { 0, 0, 0 }, { 0.046f, 0, 0 }, { 0.052f, 0.005f, 0 }, { 0.05f, 0.011f, 0 }, { 0.04f, 0.014f, 0 }, { 0, 0.0145f, 0 } };
    lathe(pm, p, 6, 24);
}
/* a sausage along x, 0.14 long, lying on y 0 */
void m_sausage(Pm *pm) {
    pm_mat(pm, PMAT_MEAT);
    const float r = 0.012f, l = 0.07f;
    PmProf p[9];
    int n = 0;
    p[n++] = (PmProf){ 0, -l - r, 0 };
    for (int k = 1; k < 4; ++k) { float a = -HALF_PI + HALF_PI * (float)k / 4.0f; p[n++] = (PmProf){ r * cosf(a), -l + r * sinf(a), 0 }; }
    p[n++] = (PmProf){ r, -l, 0 };
    p[n++] = (PmProf){ r, l, 0 };
    for (int k = 1; k < 3; ++k) { float a = HALF_PI * (float)k / 3.0f; p[n++] = (PmProf){ r * cosf(a), l + r * sinf(a), 0 }; }
    p[n++] = (PmProf){ 0, l + r, 0 };
    pm_push(pm); pm_translate(pm, 0, r, 0); pm_rotate_z(pm, HALF_PI);
    lathe(pm, p, n, 12);
    pm_pop(pm);
}
/* a bun's crown, cut side down */
void m_bun(Pm *pm) {
    pm_mat(pm, PMAT_BREAD);
    PmProf p[6] = { { 0, 0, 0 }, { 0.054f, 0, 0 }, { 0.057f, 0.009f, 0 }, { 0.049f, 0.027f, 0 }, { 0.028f, 0.038f, 0 }, { 0, 0.041f, 0 } };
    lathe(pm, p, 6, 24);
}
/* a stack of flatbreads, 0.22 round, their edges uneven */
void m_bread_stack(Pm *pm) {
    pm_mat(pm, PMAT_BREAD);
    PmProf p[32];
    int n = 0;
    p[n++] = (PmProf){ 0, 0, 0 };
    const int layers = L0 ? 12 : L2 ? 3 : 6;
    for (int k = 0; k < layers; ++k) {
        const float y = 0.05f * (float)k / (float)layers;
        p[n++] = (PmProf){ 0.106f + 0.004f * (float)(k & 1), y + 0.001f, 0 };
        p[n++] = (PmProf){ 0.111f - 0.003f * (float)(k % 3 == 0), y + 0.5f * 0.05f / (float)layers, 0 };
    }
    p[n++] = (PmProf){ 0.104f, 0.05f, 0 };
    p[n++] = (PmProf){ 0, 0.052f, 0 };
    lathe(pm, p, n, 32);
}
void m_tortilla(Pm *pm) {
    pm_mat(pm, PMAT_BREAD);
    PmProf p[5] = { { 0, 0, 0 }, { 0.074f, 0, 0 }, { 0.076f, 0.0015f, 0 }, { 0.073f, 0.003f, 0 }, { 0, 0.0032f, 0 } };
    lathe(pm, p, 5, 24);
}

/* ------------------------------------------------------------ the fryer --- */
/* A twin-well fryer, 0.56 x 0.46: a stainless body on rubber feet, two wells of hot oil (FOOD_FRYER_OIL), the
 * frame round them (FOOD_FRYER_TOP), knobs and pilots on the front, the basket rail at the back on its posts. */
void m_fryer(Pm *pm) {
    const float W = 0.56f, D = 0.46f, top = FOOD_FRYER_TOP, mid = 0.20f;
    feet(pm, 0.5f * W - 0.04f, 0.5f * D - 0.04f, 0.02f);
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.5f * W, 0.02f, -0.5f * D, 0.5f * W, mid, 0.5f * D, 0.004f);
    bx(pm, -0.5f * W, mid, 0.13f, 0.5f * W, top, 0.5f * D, 0.003f);
    bx(pm, -0.5f * W, mid, -0.5f * D, 0.5f * W, top, -0.19f, 0.003f);
    bx(pm, -0.5f * W, mid, -0.19f, -0.25f, top, 0.13f, 0.002f);
    bx(pm, 0.25f, mid, -0.19f, 0.5f * W, top, 0.13f, 0.002f);
    bx(pm, -0.012f, mid, -0.19f, 0.012f, top, 0.13f, 0.002f);
    pm_mat(pm, PMAT_FRYOIL);
    bx(pm, -0.25f, mid, -0.19f, -0.012f, FOOD_FRYER_OIL, 0.13f, 0.0f);
    bx(pm, 0.012f, mid, -0.19f, 0.25f, FOOD_FRYER_OIL, 0.13f, 0.0f);
    for (int k = -1; k <= 1; k += 2) {
        knob_at(pm, (float)k * 0.13f, 0.11f, 0.5f * D);
        pilot_at(pm, (float)k * 0.13f + 0.06f, 0.11f, 0.5f * D);
    }
    /* the rail the baskets hang from */
    pm_mat(pm, PMAT_STEEL);
    for (int s = -1; s <= 1; s += 2) {
        float a[3] = { (float)s * 0.26f, top, -0.21f }, b[3] = { (float)s * 0.26f, FOOD_FRYER_RAIL + 0.012f, -0.21f };
        pm_beam(pm, a, b, 0.016f, 0.016f, 0.002f, NULL);
    }
    PmPath pa;
    pm_path_init(&pa, 2);
    pm_path_add(&pa, -0.27f, FOOD_FRYER_RAIL, -0.21f, 0);
    pm_path_add(&pa, 0.27f, FOOD_FRYER_RAIL, -0.21f, 0);
    pm_tube(pm, &pa, 0.006f, 10, 3);
    pm_path_free(&pa);
}

/* A fry basket, 0.20 x 0.26 x 0.11 deep, its rim at the origin, fries in it; the handle out +z, a hook at its
 * back. */
void m_fry_basket(Pm *pm) {
    const float hx = 0.10f, hz = 0.13f, d = 0.11f;
    pm_mat(pm, PMAT_STAINLESS);
    if (L2) {
        bx(pm, -hx, -d, -hz, hx, -0.02f, hz, 0.0f);
    } else {
        /* the sides as wire grilles, the rim and floor solid */
        for (int s = 0; s < 4; ++s) {
            pm_push(pm);
            const float w = (s & 1) ? 2.0f * hz : 2.0f * hx, o = (s & 1) ? hx : hz;
            pm_rotate_y(pm, HALF_PI * (float)s);
            pm_translate(pm, 0.0f, -0.5f * d, o);
            pm_grille_bars(pm, w, d, 0.0016f, L0 ? 0.012f : 0.025f);
            pm_pop(pm);
        }
        bx(pm, -hx, -d, -hz, hx, -d + 0.003f, hz, 0.0f);
        for (int s = -1; s <= 1; s += 2) {
            bx(pm, -hx - 0.003f, -0.006f, (float)s * hz - 0.003f, hx + 0.003f, 0.0f, (float)s * hz + 0.003f, 0.0f);
            bx(pm, (float)s * hx - 0.003f, -0.006f, -hz, (float)s * hx + 0.003f, 0.0f, hz, 0.0f);
        }
    }
    pm_mat(pm, PMAT_FRIED);
    bx(pm, -hx + 0.004f, -d + 0.003f, -hz + 0.004f, hx - 0.004f, -0.045f, hz - 0.004f, 0.0f);
    if (L0) {
        uint32_t h = 0xF21Eu;
        for (int i = 0; i < 26; ++i) {
            const float x = (rnd(&h) - 0.5f) * (2.0f * hx - 0.03f), z = (rnd(&h) - 0.5f) * (2.0f * hz - 0.03f);
            pm_push(pm);
            pm_translate(pm, x, -0.05f, z);
            pm_rotate_y(pm, 6.2831853f * rnd(&h)); pm_rotate_x(pm, 0.5f + 0.9f * rnd(&h));
            const float c[3] = { 0, 0.03f, 0 }, hh[3] = { 0.0045f, 0.035f, 0.0045f };
            pm_bbox(pm, c, hh, 0.0f, 1);
            pm_pop(pm);
        }
    }
    pm_mat(pm, PMAT_STEEL);
    PmPath pa;
    pm_path_init(&pa, 4);
    pm_path_add(&pa, 0, -0.01f, hz, 0); pm_path_add(&pa, 0, 0.02f, hz + 0.08f, 0); pm_path_add(&pa, 0, 0.06f, hz + 0.17f, 0);
    pm_tube(pm, &pa, 0.004f, 8, 3);
    pa.n = 0;
    pm_path_add(&pa, 0, -0.01f, -hz, 1); pm_path_add(&pa, 0, 0.05f, -hz, 1); pm_path_add(&pa, 0, 0.05f, -hz - 0.05f, 1); pm_path_add(&pa, 0, 0.03f, -hz - 0.05f, 1);
    pm_tube(pm, &pa, 0.003f, 6, 3);
    pm_path_free(&pa);
    pm_mat(pm, PMAT_RUBBER);
    PmPath g;
    pm_path_init(&g, 2);
    pm_path_add(&g, 0, 0.045f, hz + 0.14f, 0); pm_path_add(&g, 0, 0.075f, hz + 0.24f, 0);
    pm_tube(pm, &g, 0.012f, 10, 3);
    pm_path_free(&g);
}

/* A draining tray (0.34 x 0.25) on a wire rack, fried pieces heaped on it. */
void m_fried_tray(Pm *pm) {
    tray(pm, 0.34f, 0.25f, 0.035f);
    if (!L2) {
        pm_mat(pm, PMAT_STEEL);
        pm_push(pm); pm_translate(pm, 0, 0.02f, 0); pm_rotate_x(pm, -HALF_PI);
        pm_grille_bars(pm, 0.30f, 0.21f, 0.0018f, L0 ? 0.02f : 0.04f);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_FRIED);
    const int n = L0 ? 14 : L2 ? 5 : 8;
    uint32_t h = 0xF12Du;
    for (int i = 0; i < n; ++i) {
        const float x = (rnd(&h) - 0.5f) * 0.24f, z = (rnd(&h) - 0.5f) * 0.15f;
        pm->part = (uint8_t)i;
        pm_push(pm);
        pm_translate(pm, x, 0.02f + 0.012f * rnd(&h), z);
        pm_rotate_y(pm, 6.2831853f * rnd(&h));
        pm_rotate_z(pm, HALF_PI + 0.3f * (rnd(&h) - 0.5f));
        pm_scale(pm, 0.7f, 1.0f, 1.0f);
        pm_translate(pm, 0, -0.035f, 0);
        lump(pm, 0.026f + 0.008f * rnd(&h), 0.07f, h, 10);
        pm_pop(pm);
    }
    pm->part = 0;
}

/* ----------------------------------------------------------- the crepes --- */
/* A round crepe plate: a cast-iron disc 0.40 across on a square stainless base, a knob and a pilot. */
void m_crepe_plate(Pm *pm) {
    feet(pm, 0.17f, 0.17f, 0.02f);
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.21f, 0.02f, -0.21f, 0.21f, FOOD_CREPE_TOP - 0.018f, 0.21f, 0.006f);
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf p[6] = { { 0, FOOD_CREPE_TOP - 0.018f, 0 }, { 0.198f, FOOD_CREPE_TOP - 0.018f, 0 }, { 0.2f, FOOD_CREPE_TOP - 0.014f, 0 },
                    { 0.2f, FOOD_CREPE_TOP - 0.004f, 0 }, { 0.196f, FOOD_CREPE_TOP, 0 }, { 0, FOOD_CREPE_TOP, 0 } };
    lathe(pm, p, 6, 48);
    knob_at(pm, 0.12f, 0.055f, 0.21f);
    pilot_at(pm, 0.17f, 0.055f, 0.21f);
}
void m_crepe(Pm *pm) {
    pm_mat(pm, PMAT_BREAD);
    PmProf p[5] = { { 0, 0, 0 }, { 0.168f, 0, 0 }, { 0.172f, 0.001f, 0 }, { 0.166f, 0.0018f, 0 }, { 0, 0.002f, 0 } };
    lathe(pm, p, 5, 40);
}
/* the T-rake the batter is spread with, lying flat (its handle along x) */
void m_crepe_rake(Pm *pm) {
    pm_mat(pm, PMAT_TIMBER);
    float a[3] = { -0.08f, 0.006f, 0 }, b[3] = { 0.08f, 0.006f, 0 };
    pm_beam(pm, a, b, 0.012f, 0.012f, 0.003f, NULL);
    float c[3] = { 0.08f, 0.004f, -0.065f }, d[3] = { 0.08f, 0.004f, 0.065f };
    pm_beam(pm, c, d, 0.008f, 0.008f, 0.002f, NULL);
}

/* ----------------------------------------------------------- the drinks --- */
/* A hot water urn, 0.30 across and 0.50 high: on three feet, a sight glass, a tap at the front over its drip
 * tray, a domed lid and knob, handles. */
void m_urn(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    for (int k = 0; k < 3; ++k) {
        const float a = 2.0943951f * (float)k + HALF_PI;
        pm_push(pm); pm_translate(pm, 0.11f * cosf(a), 0, 0.11f * sinf(a));
        PmProf f[4] = { { 0, 0, 0 }, { 0.016f, 0, 0 }, { 0.012f, 0.035f, 0 }, { 0, 0.035f, 0 } };
        lathe(pm, f, 4, 12);
        pm_pop(pm);
    }
    PmProf b[7] = { { 0, 0.03f, 0 }, { 0.135f, 0.03f, 0 }, { 0.15f, 0.045f, 0 }, { 0.15f, 0.44f, 0 }, { 0.146f, 0.452f, 1 }, { 0.12f, 0.452f, 0 }, { 0, 0.452f, 0 } };
    lathe(pm, b, 7, 40);
    PmProf l[6] = { { 0, 0.45f, 0 }, { 0.152f, 0.45f, 0 }, { 0.152f, 0.458f, 1 }, { 0.11f, 0.485f, 0 }, { 0.04f, 0.497f, 0 }, { 0, 0.498f, 0 } };
    lathe(pm, l, 6, 40);
    pm_mat(pm, PMAT_PLASTIC);
    PmProf k[5] = { { 0, 0.495f, 0 }, { 0.022f, 0.495f, 0 }, { 0.02f, 0.512f, 0 }, { 0.01f, 0.518f, 0 }, { 0, 0.519f, 0 } };
    lathe(pm, k, 5, 16);
    for (int s = -1; s <= 1; s += 2) {
        PmPath pa;
        pm_path_init(&pa, 12);
        const int n = L0 ? 8 : 3;
        for (int i = 0; i <= n; ++i) { float t = PI_F * (float)i / (float)n; pm_path_add(&pa, (float)s * (0.148f + 0.035f * sinf(t)), 0.40f, 0.05f * cosf(t), 0); }
        pm_tube(pm, &pa, 0.005f, 8, 3);
        pm_path_free(&pa);
    }
    /* the tap: a brass body out of the front, its lever, its spout down */
    pm_mat(pm, PMAT_BRASS);
    float t0[3] = { 0, 0.10f, 0.14f }, t1[3] = { 0, 0.10f, 0.20f };
    pm_beam(pm, t0, t1, 0.022f, 0.022f, 0.004f, NULL);
    float s0[3] = { 0, 0.10f, 0.19f }, s1[3] = { 0, 0.075f, 0.19f };
    pm_beam(pm, s0, s1, 0.012f, 0.012f, 0.002f, NULL);
    pm_mat(pm, PMAT_PLASTIC);
    float h0[3] = { 0, 0.11f, 0.19f }, h1[3] = { 0, 0.15f, 0.215f };
    pm_beam(pm, h0, h1, 0.012f, 0.01f, 0.003f, NULL);
    /* the drip tray under it */
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.05f, 0.0f, 0.15f, 0.05f, 0.02f, 0.23f, 0.003f);
    /* the sight glass */
    pm_mat(pm, PMAT_GLASS);
    pm_push(pm); pm_translate(pm, -0.07f, 0, 0.145f);
    pm_cyl(pm, 0.08f, 0.40f, 0.007f, pm_segs(pm, 10, 6));
    pm_mat(pm, PMAT_BRASS);
    pm_cyl(pm, 0.07f, 0.09f, 0.01f, pm_segs(pm, 10, 6));
    pm_cyl(pm, 0.39f, 0.41f, 0.01f, pm_segs(pm, 10, 6));
    pm_pop(pm);
}

/* A cup: a ceramic shell 70 mm round, 60 high (upside down when inverted: its rim on y 0). */
static void cup(Pm *pm, int inverted) {
    pm_mat(pm, PMAT_CERAMIC);
    PmProf o[5] = { { 0, 0, 0 }, { 0.022f, 0, 0 }, { 0.025f, 0.006f, 0 }, { 0.034f, 0.05f, 0 }, { 0.035f, 0.058f, 0 } };
    pm_push(pm);
    if (inverted) { pm_translate(pm, 0, 0.058f, 0); pm_scale(pm, 1.0f, -1.0f, 1.0f); }
    shell(pm, o, 5, 0.003f, 0.0015f, 24);
    pm_pop(pm);
}

/* A two-group espresso machine, 0.56 x 0.46 x 0.42: a stainless body, its side panels in the bar's trim coat,
 * the group heads and portafilters, steam wands, a gauge, the drip tray, cups on the warmer over the top. */
void m_espresso(Pm *pm) {
    const float W = 0.56f;
    feet(pm, 0.24f, 0.18f, 0.02f);
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.5f * W, 0.02f, -0.23f, 0.5f * W, 0.07f, 0.23f, 0.004f);           /* the base */
    bx(pm, -0.5f * W, 0.07f, -0.23f, 0.5f * W, 0.40f, 0.04f, 0.006f);           /* the housing */
    bx(pm, -0.5f * W + 0.01f, 0.40f, -0.22f, 0.5f * W - 0.01f, 0.41f, 0.03f, 0.002f);   /* the warmer */
    pm_mat(pm, PMAT_PAINTED);
    for (int s = -1; s <= 1; s += 2) {
        const float x = (float)s * 0.5f * W;
        bx(pm, s < 0 ? x - 0.008f : x, 0.08f, -0.22f, s < 0 ? x : x + 0.008f, 0.39f, 0.03f, 0.003f);
    }
    /* the drip tray and its grille */
    pm_mat(pm, PMAT_STAINLESS);
    bx(pm, -0.5f * W + 0.02f, 0.07f, 0.04f, 0.5f * W - 0.02f, 0.09f, 0.22f, 0.003f);
    if (!L2) {
        pm_mat(pm, PMAT_STEEL);
        pm_push(pm); pm_translate(pm, 0, 0.091f, 0.13f); pm_rotate_x(pm, -HALF_PI);
        pm_grille_bars(pm, W - 0.08f, 0.16f, 0.002f, L0 ? 0.012f : 0.03f);
        pm_pop(pm);
    }
    /* the groups and their portafilters */
    for (int s = -1; s <= 1; s += 2) {
        const float x = (float)s * 0.13f;
        pm_mat(pm, PMAT_STAINLESS);
        float a[3] = { x, 0.27f, 0.04f }, b[3] = { x, 0.27f, 0.10f };
        pm_beam(pm, a, b, 0.07f, 0.06f, 0.012f, NULL);
        pm_push(pm); pm_translate(pm, x, 0.0f, 0.10f);
        PmProf g[6] = { { 0, 0.20f, 0 }, { 0.038f, 0.20f, 0 }, { 0.04f, 0.205f, 1 }, { 0.042f, 0.24f, 1 }, { 0.03f, 0.25f, 0 }, { 0, 0.25f, 0 } };
        lathe(pm, g, 6, 24);
        pm_pop(pm);
        pm_mat(pm, PMAT_PLASTIC);
        float h0[3] = { x, 0.215f, 0.14f }, h1[3] = { x + (float)s * 0.03f, 0.205f, 0.26f };
        pm_beam(pm, h0, h1, 0.02f, 0.018f, 0.006f, NULL);
        /* a steam wand at each side */
        pm_mat(pm, PMAT_STAINLESS);
        PmPath pa;
        pm_path_init(&pa, 4);
        pm_path_add(&pa, (float)s * 0.24f, 0.33f, 0.04f, 0);
        pm_path_add(&pa, (float)s * 0.24f, 0.33f, 0.08f, 0);
        pm_path_add(&pa, (float)s * 0.25f, 0.13f, 0.11f, 0);
        PmPath pf;
        pm_path_init(&pf, 16);
        pm_path_fillet(&pa, 0.02f, 3, &pf);
        pm_tube(pm, &pf, 0.0045f, 8, 3);
        pm_path_free(&pa); pm_path_free(&pf);
    }
    /* the gauge on the front, between the groups */
    pm_push(pm); pm_translate(pm, 0, 0.34f, 0.04f); pm_rotate_x(pm, HALF_PI);
    pm_mat(pm, PMAT_STAINLESS);
    PmProf gb[5] = { { 0, 0, 0 }, { 0.032f, 0, 0 }, { 0.034f, 0.008f, 1 }, { 0.03f, 0.012f, 0 }, { 0, 0.012f, 0 } };
    lathe(pm, gb, 5, 24);
    pm_mat(pm, PMAT_DIAL);
    PmProf gd[3] = { { 0, 0.0125f, 0 }, { 0.027f, 0.0125f, 0 }, { 0, 0.013f, 0 } };
    lathe(pm, gd, 3, 24);
    pm_pop(pm);
    /* cups upside down on the warmer, one under a group */
    const int nc = L2 ? 2 : 6;
    for (int k = 0; k < nc; ++k) {
        pm_push(pm);
        pm_translate(pm, -0.19f + 0.38f * (float)(k % 3) / 2.0f, 0.41f, -0.16f + 0.1f * (float)(k / 3));
        cup(pm, 1);
        pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, -0.13f, 0.091f, 0.11f); cup(pm, 0); pm_pop(pm);
}

/* A stack of paper cups, 85 mm across, 0.22 high. */
void m_cup_stack(Pm *pm) {
    pm_mat(pm, PMAT_PAPER);
    PmProf p[48];
    int n = 0;
    p[n++] = (PmProf){ 0, 0, 0 };
    p[n++] = (PmProf){ 0.029f, 0, 1 };
    p[n++] = (PmProf){ 0.038f, 0.075f, 0 };
    const int rims = L0 ? 12 : L2 ? 3 : 6;
    for (int k = 0; k < rims; ++k) {
        const float y = 0.08f + 0.13f * (float)k / (float)(rims - 1);
        p[n++] = (PmProf){ 0.0405f, y, 1 };
        p[n++] = (PmProf){ 0.0425f, y + 0.003f, 1 };
        p[n++] = (PmProf){ 0.0405f, y + 0.006f, 1 };
    }
    p[n++] = (PmProf){ 0.039f, 0.216f, 1 };
    p[n++] = (PmProf){ 0.03f, 0.15f, 0 };
    p[n++] = (PmProf){ 0, 0.15f, 0 };
    lathe(pm, p, n, 28);
}

/* --------------------------------------------------------- the roasting --- */
/* A chestnut wok, 0.52 across (a sphere of 0.36 m), its bottom at the origin: the bowl, two small loop ears, a bed of
 * black grit and the chestnuts roasting on it. */
void m_chestnut_wok(Pm *pm) {
    const float S = FOOD_CHESTNUT_S, R = 0.26f, a1 = asinf(R / S), hr = S - sqrtf(S * S - R * R);
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf o[12];
    int e = L0 ? 9 : L2 ? 3 : 5, n = 0;
    for (int k = 0; k <= e; ++k) { float a = a1 * (float)k / (float)e; o[n].r = S * sinf(a); o[n].y = S - S * cosf(a); o[n].hard = 0; n++; }
    shell(pm, o, n, 0.003f, 0.004f, 56);
    pm_mat(pm, PMAT_STEEL);
    for (int s = -1; s <= 1; s += 2) {
        PmPath pa;
        pm_path_init(&pa, 12);
        const int ne = L0 ? 8 : 3;
        for (int k = 0; k <= ne; ++k) { float t = PI_F * (float)k / (float)ne; pm_path_add(&pa, (float)s * (R - 0.004f + 0.03f * sinf(t)), hr - 0.012f, 0.04f * cosf(t), 0); }
        pm_tube(pm, &pa, 0.006f, 8, 3);
        pm_path_free(&pa);
    }
    /* the grit, filled to FOOD_CHESTNUT_BED */
    const float yf = FOOD_CHESTNUT_BED, rf = sqrtf(S * S - (S - yf) * (S - yf)) - 0.004f;
    pm_mat(pm, PMAT_GRIT);
    PmProf g[12];
    n = 0;
    g[n++] = (PmProf){ 0, 0.004f, 0 };
    for (int k = 1; k <= 5; ++k) { float y = 0.004f + (yf - 0.004f) * (float)k / 5.0f; g[n++] = (PmProf){ sqrtf(maxf(S * S - (S - y) * (S - y), 0.0f)) - 0.004f, y, 0 }; }
    g[n - 1].r = rf; g[n - 1].hard = 1;
    g[n++] = (PmProf){ 0.5f * rf, yf + 0.006f, 0 };
    g[n++] = (PmProf){ 0, yf + 0.008f, 0 };
    lathe(pm, g, n, 48);
    pm_mat(pm, PMAT_CHESTNUT);
    const int nn = L0 ? 45 : L2 ? 0 : 16;
    uint32_t h = 0xC4E5u;
    for (int i = 0; i < nn; ++i) {
        const float a = 6.2831853f * rnd(&h), r = (rf - 0.04f) * sqrtf(rnd(&h));
        pm->part = (uint8_t)i;
        pm_push(pm);
        pm_translate(pm, r * cosf(a), yf + 0.002f + 0.006f * (1.0f - r / rf), r * sinf(a));
        pm_rotate_y(pm, 6.2831853f * rnd(&h));
        pm_rotate_x(pm, 1.2f + 0.4f * (rnd(&h) - 0.5f));
        pm_translate(pm, 0, -0.012f, 0);
        PmProf c[6] = { { 0, 0, 0 }, { 0.012f, 0.002f, 0 }, { 0.015f, 0.01f, 0 }, { 0.011f, 0.019f, 0 }, { 0.003f, 0.026f, 0 }, { 0, 0.027f, 0 } };
        pm_lathe(pm, c, 6, L0 ? 8 : 6, 0, 0);
        pm_pop(pm);
    }
    pm->part = 0;
}

/* A cast-iron roasting pan, 0.40 x 0.30 x 0.06, two side handles, sweet potatoes in it. */
void m_roast_pan(Pm *pm) {
    const float rc = 0.03f, hx = 0.18f - rc, hz = 0.13f - rc;
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf o[5] = { { 0, 0, 0 }, { rc - 0.01f, 0, 0 }, { rc - 0.004f, 0.004f, 0 }, { rc, 0.012f, 0 }, { rc + 0.004f, 0.06f, 0 } };
    PmProf s[40];
    int k = pm_prof_shell(o, 5, 0.004f, 0.003f, 2, s, 40);
    pm_lathe(pm, s, k, pm_segs(pm, 24, 4), hx, hz);
    for (int e = -1; e <= 1; e += 2) bx(pm, (float)e * 0.185f, 0.04f, -0.05f, (float)e * 0.22f, 0.055f, 0.05f, 0.004f);
    pm_mat(pm, PMAT_CHESTNUT);
    uint32_t h = 0x5A7Eu;
    for (int i = 0; i < 5; ++i) {
        pm->part = (uint8_t)(i + 1);
        pm_push(pm);
        pm_translate(pm, 0.02f * (rnd(&h) - 0.5f), 0.034f, -0.088f + 0.044f * (float)i);
        pm_rotate_y(pm, 0.3f * (rnd(&h) - 0.5f));
        pm_rotate_z(pm, HALF_PI);
        pm_translate(pm, 0, -0.07f - 0.02f * rnd(&h), 0);
        pm_scale(pm, 1.0f, 1.0f, 0.85f);
        lump(pm, 0.024f + 0.004f * rnd(&h), 0.14f, h, 12);
        pm_pop(pm);
    }
    pm->part = 0;
}

/* ----------------------------------------------------------- dumplings --- */
/* A sheet tray of dumplings (0.36 x 0.26): five rows of four, pleated on top. */
void m_dumpling_tray(Pm *pm) {
    tray(pm, 0.36f, 0.26f, 0.025f);
    pm_mat(pm, PMAT_DOUGH);
    if (L2) { bx(pm, -0.15f, 0.003f, -0.1f, 0.15f, 0.03f, 0.1f, 0.01f); return; }
    uint32_t h = 0xD0B9u;
    for (int i = 0; i < 20; ++i) {
        pm->part = (uint8_t)i;
        pm_push(pm);
        pm_translate(pm, -0.13f + 0.065f * (float)(i % 5) + 0.006f * (rnd(&h) - 0.5f), 0.003f, -0.075f + 0.05f * (float)(i / 5));
        pm_rotate_y(pm, 0.4f * (rnd(&h) - 0.5f));
        pm_scale(pm, 1.25f, 1.0f, 0.9f);
        PmProf d[7] = { { 0, 0, 0 }, { 0.021f, 0.001f, 0 }, { 0.023f, 0.008f, 0 }, { 0.018f, 0.018f, 0 }, { 0.009f, 0.025f, 0 }, { 0.003f, 0.031f, 0 }, { 0, 0.032f, 0 } };
        pm_lathe(pm, d, 7, L0 ? 10 : 7, 0, 0);
        pm_pop(pm);
    }
    pm->part = 0;
}

/* -------------------------------------------------------------- pans --- */
/* A GN 1/3 pan open on its filling (as pmodel_kit.c gn_pan): the filling heaped a little. */
static void gn_fill(Pm *pm, int fill, float heap) {
    const float rc = 0.02f, hx = 0.1625f - rc - 0.013f, hz = 0.088f - rc - 0.013f;
    int segs = pm_segs(pm, 24, 4);
    pm_mat(pm, PMAT_STAINLESS);
    PmProf o[6] = { { 0, 0, 0 }, { rc - 0.012f, 0, 0 }, { rc - 0.008f, 0.004f, 0 }, { rc, 0.146f, 0 }, { rc + 0.004f, 0.15f, 1 }, { rc + 0.013f, 0.15f, 0 } };
    PmProf s[40];
    int k = pm_prof_shell(o, 6, 0.0012f, 0.0f, 2, s, 40);
    pm_lathe(pm, s, k, segs, hx, hz);
    pm_mat(pm, fill);
    PmProf c[5] = { { 0, 0.09f, 0 }, { rc - 0.0035f, 0.09f, 0 }, { rc - 0.0025f, 0.128f, 0 }, { 0.5f * rc, 0.128f + 0.6f * heap, 0 }, { 0, 0.128f + heap, 0 } };
    pm_lathe(pm, c, 5, segs, hx, hz);
}
void m_gn_greens(Pm *pm) { gn_fill(pm, PMAT_GREENS, 0.02f); }
void m_gn_meat(Pm *pm) { gn_fill(pm, PMAT_MEAT, 0.012f); }
void m_gn_sauce(Pm *pm) { gn_fill(pm, PMAT_SAUCE, 0.0f); }

/* A stock pot of curry (0.34 across, 0.32 high), two loop handles (as pmodel_kit.c's stock pots). */
void m_curry_pot(Pm *pm) {
    const float R = 0.17f, H = 0.32f;
    pm_mat(pm, PMAT_STAINLESS);
    PmProf o[5] = { { 0, 0, 0 }, { R - 0.012f, 0, 0 }, { R - 0.003f, 0.004f, 0 }, { R, 0.012f, 0 }, { R, H, 0 } };
    shell(pm, o, 5, 0.0025f, 0.004f, 48);
    for (int s = -1; s <= 1; s += 2) {
        PmPath pa;
        pm_path_init(&pa, 16);
        const int n = L0 ? 10 : 4;
        for (int k = 0; k <= n; ++k) { float a = -HALF_PI + PI_F * (float)k / (float)n; pm_path_add(&pa, (float)s * (R + 0.045f * cosf(a)), H - 0.055f, 0.035f * sinf(a), 0); }
        pm_tube(pm, &pa, 0.0055f, 10, 3);
        pm_path_free(&pa);
    }
    pm_mat(pm, PMAT_CURRY);
    const float rb = R - 0.0035f;
    PmProf br[4] = { { 0, 0.75f * H - 0.004f, 0 }, { rb, 0.75f * H - 0.004f, 0 }, { rb, 0.75f * H, 0 }, { 0, 0.75f * H, 0 } };
    lathe(pm, br, 4, 40);
}

/* A wire stand of paper cones (for fried food, chestnuts, crepes), 0.12 across, 0.25 high. */
void m_paper_cones(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    pm_push(pm); pm_translate(pm, 0, 0.004f, 0); pm_torus(pm, 0.055f, 0.004f, 24, 6); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0, 0.15f, 0); pm_torus(pm, 0.042f, 0.003f, 24, 6); pm_pop(pm);
    for (int k = 0; k < 3; ++k) {
        const float a = 2.0943951f * (float)k;
        float p0[3] = { 0.055f * cosf(a), 0.004f, 0.055f * sinf(a) }, p1[3] = { 0.042f * cosf(a), 0.15f, 0.042f * sinf(a) };
        pm_beam(pm, p0, p1, 0.005f, 0.005f, 0.0f, NULL);
    }
    pm_mat(pm, PMAT_PAPER);
    PmProf p[24];
    int n = 0;
    p[n++] = (PmProf){ 0, 0.03f, 0 };
    p[n++] = (PmProf){ 0.036f, 0.15f, 0 };
    const int rims = L0 ? 6 : 2;
    for (int k = 0; k < rims; ++k) {
        const float y = 0.17f + 0.07f * (float)k / (float)(rims > 1 ? rims - 1 : 1);
        p[n++] = (PmProf){ 0.042f, y, 1 };
        p[n++] = (PmProf){ 0.045f, y + 0.004f, 1 };
    }
    p[n++] = (PmProf){ 0.041f, 0.248f, 1 };
    p[n++] = (PmProf){ 0, 0.12f, 0 };
    lathe(pm, p, n, 24);
}

/* --------------------------------------------------------- the icons --- */
/* The trades' neon icons, as pmodel_kit.c's (NB6): bent 11 mm tube in a 0.36 m square (half 1 in icon units),
 * electrode legs into the housing at each stroke's ends. */
static void stroke(Pm *pm, const float (*pt)[2], int n) {
    const float h = 0.5f * PM_ICON_SIZE, r = 0.0055f;
    PmPath pa;
    pm_path_init(&pa, n);
    for (int i = 0; i < n; ++i) pm_path_add(&pa, pt[i][0] * h, pt[i][1] * h, 0.0f, 0);
    pm_mat(pm, PMAT_NEON);
    pm_tube(pm, &pa, r, 8, 3);
    pm_path_free(&pa);
    pm_mat(pm, PMAT_PLASTIC);
    for (int e = 0; e < 2; ++e) {
        const float *q = pt[e ? n - 1 : 0];
        pm_push(pm);
        pm_translate(pm, q[0] * h, q[1] * h, 0.0f);
        pm_rotate_x(pm, HALF_PI);
        pm_cyl(pm, -0.03f, -0.004f, r, pm_segs(pm, 8, 4));
        pm_pop(pm);
    }
}
static int ncount(const Pm *pm, int n) { return L0 ? n : L2 ? (n + 3) / 4 + 1 : (n + 1) / 2 + 1; }
static int arc(float (*o)[2], int n, float cx, float cy, float rx, float ry, float a0, float a1) {
    for (int i = 0; i < n; ++i) {
        float a = a0 + (a1 - a0) * (float)i / (float)(n - 1);
        o[i][0] = cx + rx * cosf(a); o[i][1] = cy + ry * sinf(a);
    }
    return n;
}
static void line2(Pm *pm, float ax, float ay, float bx_, float by) {
    float q[2][2] = { { ax, ay }, { bx_, by } };
    stroke(pm, (const float (*)[2])q, 2);
}
/* a polyline rounded at its corners (fillet r in icon units) */
static void rounded(Pm *pm, const float (*pt)[2], int n, float r) {
    const float h = 0.5f * PM_ICON_SIZE;
    PmPath a, f;
    pm_path_init(&a, n);
    pm_path_init(&f, 96);
    for (int i = 0; i < n; ++i) pm_path_add(&a, pt[i][0] * h, pt[i][1] * h, 0.0f, 0);
    pm_path_fillet(&a, r * h, L0 ? 4 : 2, &f);
    float q[96][2];
    int m = f.n < 96 ? f.n : 96;
    for (int i = 0; i < m; ++i) { q[i][0] = f.p[i][0] / h; q[i][1] = f.p[i][1] / h; }
    pm_path_free(&a); pm_path_free(&f);
    stroke(pm, (const float (*)[2])q, m);
}
static void wisp(Pm *pm, float x, float y0, float y1, float amp) {
    float q[24][2];
    const int n = ncount(pm, 14);
    for (int i = 0; i < n; ++i) { float t = (float)i / (float)(n - 1); q[i][0] = x + amp * sinf(t * 3.0f * PI_F); q[i][1] = y0 + (y1 - y0) * t; }
    stroke(pm, (const float (*)[2])q, n);
}

void m_icon_skewer(Pm *pm) {
    line2(pm, -0.9f, -0.9f, 0.88f, 0.88f);
    for (int k = 0; k < 3; ++k) {
        const float c = -0.38f + 0.4f * (float)k, s = 0.2f;
        float q[5][2] = { { c - s, c - s }, { c + s, c - s }, { c + s, c + s }, { c - s, c + s }, { c - s, c - s + 0.001f } };
        rounded(pm, (const float (*)[2])q, 5, 0.06f);
    }
}
void m_icon_dumpling(Pm *pm) {
    float q[40][2];
    int n = arc(q, ncount(pm, 24), 0.0f, -0.45f, 0.82f, 0.85f, PI_F, 0.0f);
    q[n][0] = -0.82f; q[n][1] = -0.45f; n++;
    stroke(pm, (const float (*)[2])q, n);
    /* the pleats over the top */
    for (int k = -1; k <= 1; ++k) line2(pm, 0.28f * (float)k, 0.2f, 0.18f * (float)k, 0.42f);
    wisp(pm, -0.35f, 0.55f, 0.95f, 0.07f);
    wisp(pm, 0.35f, 0.55f, 0.95f, 0.07f);
}
void m_icon_kebab(Pm *pm) {
    line2(pm, 0.0f, -0.95f, 0.0f, 0.95f);
    float c[5][2] = { { -0.55f, 0.62f }, { 0.55f, 0.62f }, { 0.3f, -0.62f }, { -0.3f, -0.62f }, { -0.55f, 0.6199f } };
    rounded(pm, (const float (*)[2])c, 5, 0.1f);
    line2(pm, -0.4f, 0.2f, 0.4f, 0.28f);
    line2(pm, -0.33f, -0.2f, 0.33f, -0.12f);
}
void m_icon_burger(Pm *pm) {
    float q[40][2];
    int n = arc(q, ncount(pm, 20), 0.0f, 0.15f, 0.85f, 0.7f, 0.0f, PI_F);
    q[n][0] = 0.85f; q[n][1] = 0.15f; n++;
    stroke(pm, (const float (*)[2])q, n);
    n = ncount(pm, 20);
    for (int i = 0; i < n; ++i) { float t = (float)i / (float)(n - 1); q[i][0] = -0.9f + 1.8f * t; q[i][1] = -0.12f + 0.07f * sinf(t * 8.0f * PI_F); }
    stroke(pm, (const float (*)[2])q, n);
    float b[5][2] = { { -0.85f, -0.35f }, { -0.85f, -0.68f }, { 0.85f, -0.68f }, { 0.85f, -0.35f }, { -0.85f, -0.3501f } };
    rounded(pm, (const float (*)[2])b, 5, 0.14f);
}
void m_icon_hotdog(Pm *pm) {
    float b[5][2] = { { -0.95f, 0.1f }, { -0.95f, -0.45f }, { 0.95f, -0.45f }, { 0.95f, 0.1f }, { -0.95f, 0.0999f } };
    rounded(pm, (const float (*)[2])b, 5, 0.25f);
    float s[5][2] = { { -0.85f, 0.42f }, { -0.85f, 0.0f }, { 0.85f, 0.0f }, { 0.85f, 0.42f }, { -0.85f, 0.4199f } };
    rounded(pm, (const float (*)[2])s, 5, 0.2f);
    float z[24][2];
    const int n = ncount(pm, 9);
    for (int i = 0; i < n; ++i) { z[i][0] = -0.6f + 1.2f * (float)i / (float)(n - 1); z[i][1] = 0.21f + ((i & 1) ? 0.08f : -0.08f); }
    stroke(pm, (const float (*)[2])z, n);
}
void m_icon_drumstick(Pm *pm) {
    float q[40][2];
    int n = arc(q, ncount(pm, 26), -0.22f, 0.25f, 0.55f, 0.5f, -0.1f * PI_F, 1.6f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
    line2(pm, 0.12f, -0.12f, 0.55f, -0.55f);
    n = arc(q, ncount(pm, 12), 0.68f, -0.48f, 0.14f, 0.14f, -0.6f * PI_F, 0.9f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
    n = arc(q, ncount(pm, 12), 0.48f, -0.68f, 0.14f, 0.14f, -1.4f * PI_F, 0.1f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
}
void m_icon_fries(Pm *pm) {
    float c[4][2] = { { -0.62f, 0.15f }, { -0.45f, -0.88f }, { 0.45f, -0.88f }, { 0.62f, 0.15f } };
    rounded(pm, (const float (*)[2])c, 4, 0.1f);
    for (int k = 0; k < 5; ++k) {
        const float x = -0.4f + 0.2f * (float)k;
        line2(pm, x, 0.0f, x + 0.06f * (float)(k - 2), 0.62f + 0.25f * (float)((k * 7) % 3) / 2.0f);
    }
}
void m_icon_taco(Pm *pm) {
    float q[40][2];
    int n = arc(q, ncount(pm, 22), 0.0f, 0.15f, 0.85f, 0.8f, PI_F, 2.0f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
    n = ncount(pm, 20);
    for (int i = 0; i < n; ++i) { float t = (float)i / (float)(n - 1); q[i][0] = -0.8f + 1.6f * t; q[i][1] = 0.22f + 0.14f * fabsf(sinf(t * 3.0f * PI_F)); }
    stroke(pm, (const float (*)[2])q, n);
}
void m_icon_crepe(Pm *pm) {
    float c[3][2] = { { -0.62f, 0.45f }, { 0.0f, -0.92f }, { 0.62f, 0.45f } };
    rounded(pm, (const float (*)[2])c, 3, 0.12f);
    float q[40][2];
    const int n = ncount(pm, 20);
    for (int i = 0; i < n; ++i) { float t = (float)i / (float)(n - 1); q[i][0] = -0.62f + 1.24f * t; q[i][1] = 0.45f + 0.22f * fabsf(sinf(t * 3.0f * PI_F)); }
    stroke(pm, (const float (*)[2])q, n);
    line2(pm, -0.5f, 0.32f, 0.22f, 0.0f);
}
void m_icon_chestnut(Pm *pm) {
    float c[9][2] = { { 0.0f, 0.88f }, { 0.45f, 0.3f }, { 0.66f, -0.25f }, { 0.4f, -0.75f }, { -0.4f, -0.75f }, { -0.66f, -0.25f }, { -0.45f, 0.3f }, { 0.0f, 0.88f }, { 0.001f, 0.88f } };
    rounded(pm, (const float (*)[2])c, 8, 0.22f);
    float q[24][2];
    const int n = arc(q, ncount(pm, 12), 0.0f, -0.05f, 0.5f, 0.3f, -0.85f * PI_F, -0.15f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
}
void m_icon_teapot(Pm *pm) {
    float q[40][2];
    int n = arc(q, ncount(pm, 26), 0.0f, -0.25f, 0.55f, 0.45f, 0.0f, 2.0f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
    float sp[3][2] = { { 0.5f, -0.35f }, { 0.78f, -0.05f }, { 0.95f, 0.3f } };
    rounded(pm, (const float (*)[2])sp, 3, 0.1f);
    n = arc(q, ncount(pm, 14), -0.55f, -0.2f, 0.3f, 0.28f, 0.5f * PI_F, 1.5f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
    n = arc(q, ncount(pm, 12), 0.0f, 0.2f, 0.3f, 0.16f, 0.0f, PI_F);
    stroke(pm, (const float (*)[2])q, n);
    n = arc(q, ncount(pm, 10), 0.0f, 0.5f, 0.09f, 0.09f, -0.5f * PI_F, 1.5f * PI_F);
    stroke(pm, (const float (*)[2])q, n);
}
void m_icon_flame(Pm *pm) {
    float o[8][2] = { { 0.0f, 0.95f }, { 0.55f, 0.1f }, { 0.55f, -0.5f }, { 0.0f, -0.9f }, { -0.55f, -0.5f }, { -0.55f, 0.1f }, { 0.0f, 0.95f }, { 0.001f, 0.95f } };
    rounded(pm, (const float (*)[2])o, 7, 0.25f);
    float i[6][2] = { { 0.0f, 0.3f }, { 0.25f, -0.2f }, { 0.0f, -0.6f }, { -0.25f, -0.2f }, { 0.0f, 0.3f }, { 0.001f, 0.3f } };
    rounded(pm, (const float (*)[2])i, 5, 0.12f);
}
