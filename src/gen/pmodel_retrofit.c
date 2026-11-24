#include "pmodel_retrofit.h"
#include "mathlib.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The houses' shared retrofit kit (residential roadmap HM5c1): see
 * pmodel_retrofit.h for the frames, and the concept sheets in
 * docs/reference/international-residential for what each piece answers.
 *
 *   roof        solar panels and rows, canopy posts and beams, downlights,
 *               LED strips, tanks (plastic, steel), stands, aerials
 *   services    copper and steel pipes, elbows, a valve, clips, meters
 *   screens     breeze blocks (diamonds, rings), railings, gates, razor wire
 *   lights      lanterns, bulkheads
 *   plants      pots, a palm, troughs, vines, trailing plants
 *   assemblies  what places them (pmodel_retrofit.h)
 * ========================================================================= */

#define L0 (pm->lod == PM_LOD0)
#define L1 (pm->lod == PM_LOD1)
#define L2 (pm->lod == PM_LOD2)
#define HALF_PI 1.5707963f

/* ---------------------------------------------------------------- helpers --- */
/* the retrofit kit keeps no support loops either (pmodel_house.c's quiet) */
static void quiet(Pm *pm) { pm->support = 0.0f; pm->maxlen = 0.0f; }
static float rnd(uint32_t *h) { *h = *h * 1664525u + 1013904223u; return (float)(*h >> 8) / 16777216.0f; }
static int segs(const Pm *pm, int base, int lo) { return pm_segs(pm, base, lo); }
static void bx(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    const float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    const float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    pm_bbox(pm, c, h, L2 ? 0.0f : bevel, 1);
}
/* a cylinder along x (x0..x1) or z through (y, z) or (x, y) */
static void cyl_x(Pm *pm, float x0, float x1, float y, float z, float r, int s) {
    pm_push(pm); pm_translate(pm, 0.0f, y, z); pm_rotate_z(pm, -HALF_PI); pm_cyl(pm, x0, x1, r, s); pm_pop(pm);
}
static void cyl_z(Pm *pm, float z0, float z1, float x, float y, float r, int s) {
    pm_push(pm); pm_translate(pm, x, y, 0.0f); pm_rotate_x(pm, HALF_PI); pm_cyl(pm, z0, z1, r, s); pm_pop(pm);
}
static void cyl_y(Pm *pm, float y0, float y1, float x, float z, float r, int s) {
    pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, y0, y1, r, s); pm_pop(pm);
}
static void beam(Pm *pm, float ax, float ay, float az, float bxx, float by, float bz, float w, float h, const float *up) {
    const float a[3] = { ax, ay, az }, b[3] = { bxx, by, bz };
    pm_beam(pm, a, b, w, h, 0.0f, up);
}
/* a closed frustum of a square standing on y0: half side a at y0 to b at y1 */
static void frustum(Pm *pm, float y0, float y1, float a, float b, int s) {
    PmProf p[4] = { { 0, y0, 0 }, { a, y0, 1 }, { b, y1, 1 }, { 0, y1, 0 } };
    pm_push(pm); pm_rotate_y(pm, 0.7853982f);   /* the lathe's four segments as a square's sides */
    pm_lathe(pm, p, 4, s, 0, 0);
    pm_pop(pm);
}
/* A clump of leaves: a squat blob of radius r and height h (a leaf mass seen from the street), standing on y 0. */
static void clump(Pm *pm, float x, float y, float z, float r, float h, float flat) {
    PmProf p[4] = { { 0, 0, 0 }, { r, 0.25f * h, 0 }, { 0.7f * r, 0.8f * h, 0 }, { 0, h, 0 } };
    pm_push(pm); pm_translate(pm, x, y, z); pm_scale(pm, 1.0f, 1.0f, flat);
    pm_lathe(pm, p, 4, segs(pm, 7, 4), 0, 0);
    pm_pop(pm);
}

/* ------------------------------------------------------------------- roof --- */
/* A panel in its aluminium frame, its middle at (cx, 0, 0): clamps to its feet, the cells under the glass. The
 * sheets' panels are framed black glass in a grid of cells (house.glsl draws the cells over HPOLY's metres). */
static void panel(Pm *pm, float cx) {
    const float hw = 0.5f * HR_PANEL_W, hl = 0.5f * HR_PANEL_L, t = HR_PANEL_H;
    if (L2) { pm_mat(pm, PMAT_HSOLAR); bx(pm, cx - hw, t - 0.04f, -hl, cx + hw, t, hl, 0.0f); return; }
    pm_mat(pm, PMAT_HGALV);
    for (int k = 0; k < 4; ++k) {   /* clamps and feet to the rails under it */
        const float x = cx + (k & 1 ? 0.38f : -0.38f), z = k & 2 ? 0.55f : -0.55f;
        bx(pm, x - 0.025f, 0.0f, z - 0.03f, x + 0.025f, t - 0.035f, z + 0.03f, 0.0f);
    }
    const float f = 0.035f;
    bx(pm, cx - hw, t - 0.04f, -hl, cx + hw, t, -hl + f, 0.003f);
    bx(pm, cx - hw, t - 0.04f, hl - f, cx + hw, t, hl, 0.003f);
    bx(pm, cx - hw, t - 0.04f, -hl + f, cx - hw + f, t, hl - f, 0.003f);
    bx(pm, cx + hw - f, t - 0.04f, -hl + f, cx + hw, t, hl - f, 0.003f);
    pm_mat(pm, PMAT_HSOLAR);
    bx(pm, cx - hw + f, t - 0.036f, -hl + f, cx + hw - f, t - 0.006f, hl - f, 0.0f);
}
void m_r_solar_panel(Pm *pm) { quiet(pm); panel(pm, 0.0f); }
void m_r_solar_row(Pm *pm) {
    quiet(pm);
    for (int k = 0; k < HR_ROW_N; ++k) panel(pm, (k - 0.5f * (HR_ROW_N - 1)) * (HR_PANEL_W + 0.02f));
    if (!L2) {   /* the two rails under the row */
        pm_mat(pm, PMAT_HGALV);
        const float half = 0.5f * HR_ROW_N * (HR_PANEL_W + 0.02f);
        for (int k = 0; k < 2; ++k) { const float z = k ? 0.55f : -0.55f; bx(pm, -half, 0.0f, z - 0.02f, half, 0.04f, z + 0.02f, 0.0f); }
    }
}

/* A canopy's post: an 80 mm square hollow section on a bolted base plate (stretched to its height). */
void m_r_frame_post(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float h = 0.5f * HR_POST;
    bx(pm, -h, 0.0f, -h, h, 1.0f, h, 0.004f);
    if (!L2) bx(pm, -0.08f, 0.0f, -0.08f, 0.08f, 0.012f, 0.08f, 0.0f);
    if (L0) for (int k = 0; k < 4; ++k) cyl_y(pm, 0.012f, 0.03f, k & 1 ? 0.055f : -0.055f, k & 2 ? 0.055f : -0.055f, 0.008f, 6);
}
/* A canopy's beam: a 140 x 60 mm rectangular hollow section along x, its top on y 0. */
void m_r_frame_beam(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    bx(pm, 0.0f, -HR_BEAM_H, -0.03f, 1.0f, 0.0f, 0.03f, 0.004f);
}
/* A downlight under a beam or a slab: a shallow can and its warm lens. */
void m_r_downlight(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    PmProf b[5] = { { 0, -0.03f, 0 }, { 0.05f, -0.03f, 1 }, { 0.058f, -0.024f, 0 }, { 0.058f, 0.0f, 1 }, { 0, 0.0f, 0 } };
    pm_lathe(pm, b, 5, segs(pm, 14, 6), 0, 0);
    pm_mat(pm, PMAT_HLED);
    PmProf g[4] = { { 0, -0.036f, 0 }, { 0.042f, -0.036f, 1 }, { 0.042f, -0.028f, 1 }, { 0, -0.028f, 0 } };
    pm_lathe(pm, g, 4, segs(pm, 14, 6), 0, 0);
}
/* An LED strip: an aluminium channel along x, its opal diffuser underneath. */
void m_r_led_strip(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    bx(pm, 0.0f, -0.014f, -0.0125f, 1.0f, 0.0f, 0.0125f, 0.0f);
    pm_mat(pm, PMAT_HLED);
    bx(pm, 0.0f, -0.019f, -0.009f, 1.0f, -0.012f, 0.009f, 0.0f);
}

/* A black 1000 litre tank (the sheets' rotomoulded ones, on every roof from Lagos to Delhi to Leeds): a ribbed drum,
 * a shoulder, its screw lid, the outlet's and the overflow's fittings. */
void m_r_tank_poly(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HPOLY);
    const float R = 0.5f * HR_TANK_D;
    if (L2) {
        PmProf p[5] = { { 0, 0, 0 }, { R, 0, 1 }, { R, 1.06f, 0 }, { 0.3f, 1.2f, 0 }, { 0, 1.24f, 0 } };
        pm_lathe(pm, p, 5, 8, 0, 0);
        return;
    }
    PmProf p[32];
    int n = 0;
    p[n++] = (PmProf){ 0, 0, 0 };
    p[n++] = (PmProf){ R - 0.04f, 0, 1 };
    p[n++] = (PmProf){ R, 0.04f, 0 };
    for (int k = 0; k < 4; ++k) {   /* the ribs: a groove every ~0.24 m */
        const float y = 0.17f + 0.24f * (float)k;
        if (L0) { p[n++] = (PmProf){ R, y, 0 }; p[n++] = (PmProf){ R - 0.025f, y + 0.025f, 0 }; p[n++] = (PmProf){ R, y + 0.05f, 0 }; }
        else if (k == 1 || k == 3) { p[n++] = (PmProf){ R, y, 0 }; p[n++] = (PmProf){ R - 0.02f, y + 0.025f, 0 }; p[n++] = (PmProf){ R, y + 0.05f, 0 }; }
    }
    p[n++] = (PmProf){ R, 1.05f, 0 };
    p[n++] = (PmProf){ R - 0.07f, 1.14f, 0 };
    p[n++] = (PmProf){ 0.30f, 1.19f, 0 };
    p[n++] = (PmProf){ 0.20f, 1.205f, 1 };
    p[n++] = (PmProf){ 0.20f, 1.25f, 1 };   /* the lid */
    p[n++] = (PmProf){ 0.17f, 1.265f, 0 };
    p[n++] = (PmProf){ 0, 1.27f, 0 };
    pm_lathe(pm, p, n, segs(pm, 22, 10), 0, 0);
    pm_mat(pm, PMAT_HGALV);   /* the outlet low on its side, the overflow high on the other */
    cyl_z(pm, R - 0.05f, R + 0.09f, 0.0f, 0.12f, 0.03f, segs(pm, 10, 6));
    cyl_z(pm, -R - 0.07f, -R + 0.05f, 0.0f, 1.0f, 0.022f, segs(pm, 10, 6));
}
/* A steel tank: an upright stainless drum with two bands on four short legs, a conical top and its vent. */
void m_r_tank_steel(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float R = 0.5f * HR_STEEL_D;
    for (int k = 0; k < 4; ++k) {
        const float x = (k & 1 ? 1.0f : -1.0f) * 0.62f * R, z = (k & 2 ? 1.0f : -1.0f) * 0.62f * R;
        bx(pm, x - 0.03f, 0.0f, z - 0.03f, x + 0.03f, 0.32f, z + 0.03f, 0.0f);
    }
    PmProf p[16];
    int n = 0;
    p[n++] = (PmProf){ 0, 0.28f, 0 };
    p[n++] = (PmProf){ R, 0.28f, 1 };
    if (!L2) {
        p[n++] = (PmProf){ R, 0.62f, 0 }; p[n++] = (PmProf){ R + 0.015f, 0.64f, 1 }; p[n++] = (PmProf){ R + 0.015f, 0.68f, 1 }; p[n++] = (PmProf){ R, 0.70f, 0 };
        p[n++] = (PmProf){ R, 1.04f, 0 }; p[n++] = (PmProf){ R + 0.015f, 1.06f, 1 }; p[n++] = (PmProf){ R + 0.015f, 1.10f, 1 }; p[n++] = (PmProf){ R, 1.12f, 0 };
    }
    p[n++] = (PmProf){ R, 1.36f, 1 };
    p[n++] = (PmProf){ 0.10f, 1.48f, 1 };
    p[n++] = (PmProf){ 0.10f, 1.52f, 1 };
    p[n++] = (PmProf){ 0.06f, HR_STEEL_H, 0 };
    p[n++] = (PmProf){ 0, HR_STEEL_H, 0 };
    pm_lathe(pm, p, n, segs(pm, 22, 10), 0, 0);
    if (!L2) cyl_z(pm, R - 0.05f, R + 0.1f, 0.0f, 0.38f, 0.025f, segs(pm, 10, 6));
}
/* One bay of a tank stand: four angle legs, a ring of angles round its head, X braces of flat bar on each face. */
void m_r_stand_bay(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float h = 0.5f * HR_STAND_W, H = HR_STAND_BAY, a = 0.05f, t = 0.006f;
    for (int k = 0; k < 4; ++k) {   /* the legs, their angles' backs to the outside */
        const float sx = k & 1 ? 1.0f : -1.0f, sz = k & 2 ? 1.0f : -1.0f, x = sx * h, z = sz * h;
        if (L2) { bx(pm, x - 0.5f * a, 0.0f, z - 0.5f * a, x + 0.5f * a, H, z + 0.5f * a, 0.0f); continue; }
        bx(pm, fminf(x, x - sx * a), 0.0f, fminf(z, z - sz * t), fmaxf(x, x - sx * a), H, fmaxf(z, z - sz * t), 0.0f);
        bx(pm, fminf(x, x - sx * t), 0.0f, fminf(z, z - sz * a), fmaxf(x, x - sx * t), H, fmaxf(z, z - sz * a), 0.0f);
    }
    /* the ring round the head */
    bx(pm, -h, H - a, -h, h, H, -h + t * 2.0f, 0.0f);
    bx(pm, -h, H - a, h - t * 2.0f, h, H, h, 0.0f);
    bx(pm, -h, H - a, -h + t * 2.0f, -h + t * 2.0f, H, h - t * 2.0f, 0.0f);
    bx(pm, h - t * 2.0f, H - a, -h + t * 2.0f, h, H, h - t * 2.0f, 0.0f);
    if (L2) return;
    /* the braces: across each face, corner to corner */
    for (int f = 0; f < 4; ++f) {
        const float up[3] = { f < 2 ? 0.0f : 1.0f, 0.0f, f < 2 ? 1.0f : 0.0f };
        for (int d = 0; d < 2; ++d) {
            const float s0 = d ? h : -h, s1 = -s0;
            if (f < 2) { const float z = f ? h - 0.004f : -h + 0.004f; beam(pm, s0, 0.04f, z, s1, H - a, z, 0.035f, 0.006f, up); }
            else       { const float x = f == 3 ? h - 0.004f : -h + 0.004f; beam(pm, x, 0.04f, s0, x, H - a, s1, 0.035f, 0.006f, up); }
        }
    }
}
/* A stand's platform: angle bearers and a deck of steel plate (what the tank stands on). */
void m_r_stand_top(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float h = 0.5f * HR_STAND_W + 0.08f;
    bx(pm, -h, HR_STAND_TOP - 0.012f, -h, h, HR_STAND_TOP, h, 0.0f);
    if (L2) return;
    for (int k = 0; k < 3; ++k) { const float z = (k - 1) * (h - 0.05f); bx(pm, -h, 0.0f, z - 0.03f, h, HR_STAND_TOP - 0.012f, z + 0.03f, 0.0f); }
    bx(pm, -h, HR_STAND_TOP, -h, h, HR_STAND_TOP + 0.04f, -h + 0.008f, 0.0f);   /* an upstand on two sides */
    bx(pm, -h, HR_STAND_TOP, h - 0.008f, h, HR_STAND_TOP + 0.04f, h, 0.0f);
}

/* An aerial on its mast: a Yagi on a boom at the top (eight directors and a reflector), the mast in a bracket. */
void m_r_aerial_yagi(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float H = HR_AERIAL_H;
    bx(pm, -0.06f, 0.0f, -0.06f, 0.06f, 0.012f, 0.06f, 0.0f);
    cyl_y(pm, 0.0f, H, 0.0f, 0.0f, 0.021f, segs(pm, 8, 5));
    const float y = H - 0.15f;
    cyl_x(pm, -0.45f, 0.75f, y, 0.0f, 0.011f, segs(pm, 6, 4));
    if (L2) return;
    bx(pm, -0.05f, y - 0.04f, -0.035f, 0.05f, y + 0.04f, 0.035f, 0.0f);   /* the clamp */
    const int n = L0 ? 8 : 4;
    for (int k = 0; k < n; ++k) {
        const float x = -0.35f + 1.05f * (float)k / (float)(n - 1), half = 0.2f - 0.07f * (float)k / (float)(n - 1);
        cyl_z(pm, -half, half, x, y, 0.004f, L0 ? 5 : 4);
    }
    /* the reflector: a short rack at the back */
    for (int k = 0; k < 2; ++k) cyl_y(pm, y - 0.18f, y + 0.18f, -0.43f, k ? 0.12f : -0.12f, 0.004f, 4);
    cyl_z(pm, -0.14f, 0.14f, -0.43f, y, 0.006f, 4);
}
/* The old cross aerial the sheets put on chimneys: a mast with two cross bars and spikes up from their ends. */
void m_r_aerial_cross(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float H = HR_AERIAL_H;
    bx(pm, -0.06f, 0.0f, -0.06f, 0.06f, 0.012f, 0.06f, 0.0f);
    cyl_y(pm, 0.0f, H, 0.0f, 0.0f, 0.021f, segs(pm, 8, 5));
    const float yA = H - 0.55f, yB = H - 0.15f;
    cyl_z(pm, -0.5f, 0.5f, 0.0f, yA, 0.012f, segs(pm, 6, 4));
    cyl_z(pm, -0.34f, 0.34f, 0.0f, yB, 0.012f, segs(pm, 6, 4));
    if (L2) return;
    for (int k = 0; k < 2; ++k) {
        const float z = k ? 0.5f : -0.5f, zb = k ? 0.34f : -0.34f;
        cyl_y(pm, yA - 0.05f, yA + 0.38f, 0.0f, z, 0.006f, 5);
        cyl_y(pm, yB - 0.05f, yB + 0.3f, 0.0f, zb, 0.006f, 5);
    }
    cyl_y(pm, H, H + 0.35f, 0.0f, 0.0f, 0.007f, 5);   /* the spike over the mast */
}

/* --------------------------------------------------------------- services --- */
void m_r_pipe_cu(Pm *pm) { quiet(pm); pm_mat(pm, PMAT_HCOPPER); pm_cyl(pm, 0.0f, 1.0f, HR_PIPE_CU, segs(pm, 10, 5)); }
void m_r_pipe_fe(Pm *pm) { quiet(pm); pm_mat(pm, PMAT_HGALV); pm_cyl(pm, 0.0f, 1.0f, HR_PIPE_FE, segs(pm, 12, 6)); }
/* A quarter bend from +y round to +z, its collars (a copper fitting's sockets, a steel one's threaded ends). */
static void elbow(Pm *pm, float r, int mat) {
    pm_mat(pm, mat);
    PmPath pa;
    pm_path_init(&pa, 12);
    const int n = L0 ? 8 : L1 ? 5 : 3;
    for (int k = 0; k <= n; ++k) {
        const float a = HALF_PI * (float)k / (float)n;
        pm_path_add(&pa, 0.0f, HR_BEND * sinf(a), HR_BEND * (1.0f - cosf(a)), 0);
    }
    pm_tube(pm, &pa, r, segs(pm, 10, 5), 3);
    pm_path_free(&pa);
    if (L2) return;
    cyl_y(pm, -0.035f, 0.0f, 0.0f, 0.0f, r * 1.18f, segs(pm, 10, 5));
    cyl_z(pm, HR_BEND, HR_BEND + 0.035f, 0.0f, HR_BEND, r * 1.18f, segs(pm, 10, 5));
}
void m_r_elbow_cu(Pm *pm) { quiet(pm); elbow(pm, HR_PIPE_CU, PMAT_HCOPPER); }
void m_r_elbow_fe(Pm *pm) { quiet(pm); elbow(pm, HR_PIPE_FE, PMAT_HGALV); }
/* A gate valve on a steel riser: its body round the pipe, the bonnet and a handwheel out towards +z. */
void m_r_valve(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HCOPPER);   /* bronze */
    bx(pm, -0.055f, -0.07f, -0.055f, 0.055f, 0.07f, 0.055f, 0.008f);
    cyl_y(pm, -0.09f, 0.09f, 0.0f, 0.0f, 0.05f, segs(pm, 10, 6));
    cyl_z(pm, 0.05f, 0.13f, 0.0f, 0.0f, 0.022f, segs(pm, 8, 5));
    pm_mat(pm, PMAT_HIRON);
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.14f); pm_rotate_x(pm, HALF_PI);
    pm_torus(pm, 0.06f, 0.008f, segs(pm, 14, 6), L0 ? 6 : 4);
    pm_pop(pm);
    if (!L2) { beam(pm, -0.06f, 0.0f, 0.14f, 0.06f, 0.0f, 0.14f, 0.012f, 0.008f, NULL); beam(pm, 0.0f, -0.06f, 0.14f, 0.0f, 0.06f, 0.14f, 0.012f, 0.008f, NULL); }
}
/* A saddle clip: a band round the riser (its axis the origin's), a leg back to a plate on the wall. */
void m_r_pipe_clip(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float r = HR_PIPE_FE + 0.003f;
    PmProf b[4] = { { r, -0.015f, 1 }, { r + 0.006f, -0.015f, 1 }, { r + 0.006f, 0.015f, 1 }, { r, 0.015f, 1 } };
    PmProf c[5] = { b[0], b[1], b[2], b[3], b[0] };
    pm_lathe(pm, c, 5, segs(pm, 12, 6), 0, 0);
    bx(pm, -0.012f, -0.015f, -HR_PIPE_Z, 0.012f, 0.015f, -r, 0.0f);
    bx(pm, -0.04f, -0.02f, -HR_PIPE_Z, 0.04f, 0.02f, -HR_PIPE_Z + 0.008f, 0.0f);
}
/* An electricity meter's cabinet: a grey steel box, a window over the dial, a latch; a conduit up from its top. */
void m_r_meter(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float hw = 0.5f * HR_METER_W, H = HR_METER_H, D = 0.16f;
    bx(pm, -hw, 0.0f, 0.0f, hw, H, D, 0.012f);
    if (!L2) {
        bx(pm, -hw - 0.006f, H - 0.01f, -0.002f, hw + 0.006f, H + 0.02f, D + 0.02f, 0.004f);   /* its drip hood */
        cyl_y(pm, H, H + 0.25f, 0.0f, 0.05f, 0.014f, segs(pm, 8, 4));
        cyl_y(pm, -0.2f, 0.0f, -0.08f, 0.05f, 0.018f, segs(pm, 8, 4));
        pm_mat(pm, PMAT_HGLASS);
        bx(pm, -0.09f, H - 0.21f, D - 0.01f, 0.09f, H - 0.09f, D + 0.006f, 0.0f);
    }
    if (L0) { pm_mat(pm, PMAT_HIRON); bx(pm, hw - 0.05f, 0.18f, D, hw - 0.025f, 0.26f, D + 0.012f, 0.0f); }
}

/* ---------------------------------------------------------------- screens --- */
/* Breeze-block panels: three blocks by three, their webs a grid of 35 mm, a pattern in each block. */
void m_r_breeze_diamond(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HBREEZE);
    const float B = HR_BREEZE, b = B / 3.0f, t = 0.5f * HR_BREEZE_T, w = 0.0175f;
    for (int k = 0; k <= 3; ++k) {
        const float y0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, y1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        bx(pm, 0.0f, y0, -t, B, y1, t, 0.0f);
    }
    for (int k = 0; k <= 3; ++k) {
        const float x0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, x1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        for (int j = 0; j < 3; ++j) bx(pm, x0, b * (float)j + w, -t, x1, b * (float)(j + 1) - w, t, 0.0f);
    }
    if (L2) return;
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float cx = b * ((float)i + 0.5f), cy = b * ((float)j + 0.5f), r = 0.5f * b - w * 0.5f;
            /* a diamond from the middles of the block's sides */
            beam(pm, cx - r, cy, 0.0f, cx, cy + r, 0.0f, 0.03f, 2.0f * t * 0.9f, up);
            beam(pm, cx, cy + r, 0.0f, cx + r, cy, 0.0f, 0.03f, 2.0f * t * 0.9f, up);
            beam(pm, cx + r, cy, 0.0f, cx, cy - r, 0.0f, 0.03f, 2.0f * t * 0.9f, up);
            beam(pm, cx, cy - r, 0.0f, cx - r, cy, 0.0f, 0.03f, 2.0f * t * 0.9f, up);
            if (L0) bx(pm, cx - 0.025f, cy - 0.025f, -t * 0.9f, cx + 0.025f, cy + 0.025f, t * 0.9f, 0.0f);   /* the boss */
        }
}
/* ------------------------------------------------------------- starter bars --- */
void m_r_rebar(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HCOPPER);
    const float h = 0.1f;   /* the cage's half width */
    for (int k = 0; k < 4; ++k) {
        const float sx = k & 1 ? 1.0f : -1.0f, sz = k & 2 ? 1.0f : -1.0f;
        const float lean = 0.03f + 0.02f * (float)(k & 1) + 0.015f * (float)(k >> 1);
        PmPath pa;
        pm_path_init(&pa, 4);
        pm_path_add(&pa, sx * h, 0.0f, sz * h, 0);
        pm_path_add(&pa, sx * h, 0.86f + 0.04f * (float)k, sz * h, 1);
        pm_path_add(&pa, sx * (h + lean), 1.0f, sz * (h + lean * 0.7f), 0);
        pm_tube(pm, &pa, 0.009f, L0 ? 6 : 4, 3);
        pm_path_free(&pa);
    }
}

/* A builder's ladder (HM5c2c2): two timber stiles and rungs, 1 m long (stretched to its length: the rungs' thickness is
 * authored for one of about 3.5 m) */
void m_r_ladder(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float W = 0.22f, S = 0.035f, D = 0.065f;
    bx(pm, -W - S, 0.0f, 0.0f, -W, 1.0f, D, 0.0f);
    bx(pm, W, 0.0f, 0.0f, W + S, 1.0f, D, 0.0f);
    const int n = L2 ? 6 : 12;
    for (int k = 0; k < n; ++k) {
        const float y = ((float)k + 0.6f) / (float)n, t = 0.016f;
        bx(pm, -W, y - 0.5f * t, 0.015f, W, y + 0.5f * t, 0.05f, 0.0f);
    }
}

/* The star lattice (HM5c2b; the Georgian, Victorian and Middle Eastern sheets' mashrabiya): in each block an eight-pointed
 * star of two squares, one turned 45 degrees, its corners tied to the block's, so the stars interlock across the panel. */
void m_r_breeze_star(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HBREEZE);
    const float B = HR_BREEZE, b = B / 3.0f, t = 0.5f * HR_BREEZE_T, w = 0.0175f;
    for (int k = 0; k <= 3; ++k) {
        const float y0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, y1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        bx(pm, 0.0f, y0, -t, B, y1, t, 0.0f);
    }
    for (int k = 0; k <= 3; ++k) {
        const float x0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, x1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        for (int j = 0; j < 3; ++j) bx(pm, x0, b * (float)j + w, -t, x1, b * (float)(j + 1) - w, t, 0.0f);
    }
    if (L2) return;
    const float up[3] = { 0.0f, 0.0f, 1.0f }, d = 2.0f * t * 0.9f;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float cx = b * ((float)i + 0.5f), cy = b * ((float)j + 0.5f), r = 0.5f * b - w, s = 0.62f * r;
            const float D[4][2] = { { cx - r, cy }, { cx, cy + r }, { cx + r, cy }, { cx, cy - r } };
            const float S[4][2] = { { cx - s, cy - s }, { cx - s, cy + s }, { cx + s, cy + s }, { cx + s, cy - s } };
            for (int k = 0; k < 4; ++k) {
                beam(pm, D[k][0], D[k][1], 0.0f, D[(k + 1) & 3][0], D[(k + 1) & 3][1], 0.0f, 0.022f, d, up);
                if (!L0) continue;   /* past the near level the turned square alone (the diamond block's budget) */
                beam(pm, S[k][0], S[k][1], 0.0f, S[(k + 1) & 3][0], S[(k + 1) & 3][1], 0.0f, 0.022f, d, up);
                {
                    /* the square's corners out to the block's */
                    const float qx = S[k][0] + (S[k][0] > cx ? 1.0f : -1.0f) * (r - s), qy = S[k][1] + (S[k][1] > cy ? 1.0f : -1.0f) * (r - s);
                    beam(pm, S[k][0], S[k][1], 0.0f, qx, qy, 0.0f, 0.02f, d, up);
                }
            }
        }
}
void m_r_breeze_ring(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HBREEZE);
    const float B = HR_BREEZE, b = B / 3.0f, t = 0.5f * HR_BREEZE_T, w = 0.0175f;
    for (int k = 0; k <= 3; ++k) {
        const float y0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, y1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        bx(pm, 0.0f, y0, -t, B, y1, t, 0.0f);
    }
    for (int k = 0; k <= 3; ++k) {
        const float x0 = k == 0 ? 0.0f : k == 3 ? B - 2.0f * w : b * (float)k - w, x1 = k == 0 ? 2.0f * w : k == 3 ? B : b * (float)k + w;
        for (int j = 0; j < 3; ++j) bx(pm, x0, b * (float)j + w, -t, x1, b * (float)(j + 1) - w, t, 0.0f);
    }
    if (L2) return;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float cx = b * ((float)i + 0.5f), cy = b * ((float)j + 0.5f);
            /* a ring touching the block's sides, and four petals' worth of corner fillets */
            pm_push(pm); pm_translate(pm, cx, cy, 0.0f); pm_scale(pm, 1.0f, 1.0f, 2.2f); pm_rotate_x(pm, HALF_PI);
            pm_torus(pm, 0.5f * b - w - 0.02f, 0.02f, L0 ? 12 : 8, 4);
            pm_pop(pm);
            if (L0) {
                const float up[3] = { 0.0f, 0.0f, 1.0f }, q = 0.5f * b - w;
                for (int c = 0; c < 4; ++c) {
                    const float sx = c & 1 ? 1.0f : -1.0f, sy = c & 2 ? 1.0f : -1.0f;
                    beam(pm, cx + sx * q * 0.45f, cy + sy * q, 0.0f, cx + sx * q, cy + sy * q * 0.45f, 0.0f, 0.025f, 2.0f * t * 0.9f, up);
                }
            }
        }
}
/* A roof railing: a square post at x 0, a round top rail, a bottom rail, balusters at ~0.12 m (painted steel). */
void m_r_rail(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float H = HR_RAIL_H;
    bx(pm, 0.0f, 0.0f, -0.02f, 0.04f, H, 0.02f, 0.003f);
    cyl_x(pm, 0.0f, 1.0f, H - 0.02f, 0.0f, 0.021f, segs(pm, 8, 4));
    bx(pm, 0.0f, 0.08f, -0.012f, 1.0f, 0.12f, 0.012f, 0.0f);
    if (L2) return;
    bx(pm, 0.0f, 0.5f * H - 0.015f, -0.008f, 1.0f, 0.5f * H + 0.015f, 0.008f, 0.0f);
    const int n = L0 ? 7 : 3;
    for (int k = 1; k <= n; ++k) {
        const float x = (float)k / (float)(n + 1);
        bx(pm, x - 0.007f, 0.12f, -0.007f, x + 0.007f, H - 0.03f, 0.007f, 0.0f);
    }
}
/* A security gate in front of a door: a flat frame, round bars every 0.1 m, two rails and a lattice between them,
 * a padlocked hasp (the sheets' grilles over the South Asian, Latin American and African doors). */
void m_r_gate_bars(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    const float hw = 0.5f * HR_GATE_W, H = HR_GATE_H, f = 0.04f;
    bx(pm, -hw, 0.0f, 0.0f, hw, f, 0.04f, 0.0f);
    bx(pm, -hw, H - f, 0.0f, hw, H, 0.04f, 0.0f);
    bx(pm, -hw, f, 0.0f, -hw + f, H - f, 0.04f, 0.0f);
    bx(pm, hw - f, f, 0.0f, hw, H - f, 0.04f, 0.0f);
    const float r0 = 0.95f, r1 = 1.15f;
    bx(pm, -hw + f, r0 - 0.02f, 0.006f, hw - f, r0 + 0.02f, 0.034f, 0.0f);
    bx(pm, -hw + f, r1 - 0.02f, 0.006f, hw - f, r1 + 0.02f, 0.034f, 0.0f);
    const int nb = L2 ? 4 : 9;
    for (int k = 1; k <= nb; ++k) {
        const float x = -hw + HR_GATE_W * (float)k / (float)(nb + 1);
        if (L2) { bx(pm, x - 0.008f, f, 0.01f, x + 0.008f, H - f, 0.03f, 0.0f); continue; }
        cyl_y(pm, f, r0 - 0.02f, x, 0.02f, 0.009f, segs(pm, 6, 4));
        cyl_y(pm, r1 + 0.02f, H - f, x, 0.02f, 0.009f, segs(pm, 6, 4));
    }
    if (!L2) {   /* the lattice between the rails */
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        const int nd = 5;
        for (int k = 0; k < nd; ++k) {
            const float x0 = -hw + f + (HR_GATE_W - 2.0f * f) * (float)k / (float)nd, x1 = -hw + f + (HR_GATE_W - 2.0f * f) * (float)(k + 1) / (float)nd;
            beam(pm, x0, r0 + 0.02f, 0.02f, x1, r1 - 0.02f, 0.02f, 0.015f, 0.02f, up);
            beam(pm, x1, r0 + 0.02f, 0.02f, x0, r1 - 0.02f, 0.02f, 0.015f, 0.02f, up);
        }
    }
    if (L0) {
        bx(pm, hw - 0.12f, 1.0f, 0.04f, hw - 0.02f, 1.1f, 0.05f, 0.0f);
        pm_mat(pm, PMAT_HCOPPER);
        bx(pm, hw - 0.1f, 0.93f, 0.05f, hw - 0.05f, 0.99f, 0.07f, 0.004f);
    }
}
/* A sheet gate for a yard (HM7 sets it in a boundary): painted sheet on a frame, a crown of bars over it. */
void m_r_gate_sheet(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float hw = 0.5f * HR_GATE_W, H = HR_SHEET_H, f = 0.05f, s = 1.35f;
    bx(pm, -hw, 0.0f, 0.0f, hw, f, 0.05f, 0.0f);
    bx(pm, -hw, H - f, 0.0f, hw, H, 0.05f, 0.0f);
    bx(pm, -hw, f, 0.0f, -hw + f, H - f, 0.05f, 0.0f);
    bx(pm, hw - f, f, 0.0f, hw, H - f, 0.05f, 0.0f);
    bx(pm, -hw + f, s - 0.02f, 0.0f, hw - f, s + 0.02f, 0.05f, 0.0f);
    bx(pm, -hw + f, f, 0.012f, hw - f, s - 0.02f, 0.022f, 0.0f);
    if (!L2) for (int k = 0; k < 2; ++k) { const float y = f + (s - f) * (float)(k + 1) / 3.0f; bx(pm, -hw + f, y - 0.03f, 0.022f, hw - f, y + 0.03f, 0.04f, 0.0f); }
    const int nb = L2 ? 3 : 8;
    for (int k = 1; k <= nb; ++k) {
        const float x = -hw + HR_GATE_W * (float)k / (float)(nb + 1);
        bx(pm, x - 0.01f, s + 0.02f, 0.015f, x + 0.01f, H - f, 0.035f, 0.0f);
    }
}
/* HM5e3: a painted steel fence (the link house's), x 0..1 and y 0..1 stretched, centred on z 0: a post at x 0, flat bars
 * between a bottom, a middle and a top rail. Its paint is the unit's trim. */
void m_r_fence_steel(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, 0.0f, 0.0f, -0.025f, 0.05f, 1.0f, 0.025f, 0.003f);
    bx(pm, 0.0f, 0.04f, -0.02f, 1.0f, 0.09f, 0.02f, 0.0f);
    bx(pm, 0.0f, 0.93f, -0.02f, 1.0f, 0.98f, 0.02f, 0.0f);
    if (!L2) bx(pm, 0.05f, 0.70f, -0.015f, 1.0f, 0.73f, 0.015f, 0.0f);
    const int n = L2 ? 4 : 9;
    for (int k = 0; k < n; ++k) {
        const float x = 0.05f + 0.95f * ((float)k + 0.5f) / (float)n;
        bx(pm, x - 0.012f, 0.09f, -0.006f, x + 0.012f, 0.93f, 0.006f, 0.0f);
    }
}
/* A galvanised welded-mesh fence (Japan's), x 0..1 and y 0..1 stretched, centred on z 0: a post at x 0, a frame, wires
 * every 0.1 m up and 0.2 m across. */
void m_r_fence_mesh(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    bx(pm, 0.0f, 0.0f, -0.02f, 0.04f, 1.0f, 0.02f, 0.0f);
    bx(pm, 0.04f, 0.0f, -0.015f, 1.0f, 0.035f, 0.015f, 0.0f);
    bx(pm, 0.04f, 0.965f, -0.015f, 1.0f, 1.0f, 0.015f, 0.0f);
    const int nv = L0 ? 10 : L1 ? 6 : 3, nh = L2 ? 1 : 4;
    for (int k = 1; k < nv; ++k) { const float x = 0.04f + 0.96f * (float)k / (float)nv; bx(pm, x - 0.003f, 0.035f, -0.003f, x + 0.003f, 0.965f, 0.003f, 0.0f); }
    for (int k = 1; k <= nh; ++k) { const float y = 0.035f + 0.93f * (float)k / (float)(nh + 1); bx(pm, 0.04f, y - 0.003f, 0.003f, 1.0f, y + 0.003f, 0.009f, 0.0f); }
}
/* A pierced steel gate's leaf (the Middle East's villa), x 0..1 and y 0..1 stretched, centred on z 0: a frame, a solid
 * panel low down, a lattice of diamonds over it with a boss at each crossing, a band of bars under the top rail. */
void m_r_gate_pierced(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    const float f = 0.045f, y0 = 0.32f, y1 = 0.86f;
    bx(pm, 0.0f, 0.0f, -0.02f, f, 1.0f, 0.02f, 0.0f);
    bx(pm, 1.0f - f, 0.0f, -0.02f, 1.0f, 1.0f, 0.02f, 0.0f);
    bx(pm, f, 0.0f, -0.02f, 1.0f - f, f, 0.02f, 0.0f);
    bx(pm, f, 1.0f - f, -0.02f, 1.0f - f, 1.0f, 0.02f, 0.0f);
    bx(pm, f, y0 - 0.03f, -0.02f, 1.0f - f, y0, 0.02f, 0.0f);
    bx(pm, f, y1, -0.02f, 1.0f - f, y1 + 0.03f, 0.02f, 0.0f);
    bx(pm, f, f, -0.006f, 1.0f - f, y0 - 0.03f, 0.006f, 0.0f);   /* the low panel */
    if (L2) {   /* far off: bars over the panel */
        for (int k = 1; k < 6; ++k) { const float x = f + (1.0f - 2.0f * f) * (float)k / 6.0f; bx(pm, x - 0.01f, y0, -0.008f, x + 0.01f, 1.0f - f, 0.008f, 0.0f); }
        return;
    }
    const float up[3] = { 0.0f, 0.0f, 1.0f };
    const int nc = 4, nr = 2;
    const float cw = (1.0f - 2.0f * f) / (float)nc, rh = (y1 - y0) / (float)nr;
    for (int i = 0; i < nc; ++i)
        for (int j = 0; j < nr; ++j) {
            const float xa = f + cw * (float)i, xb = xa + cw, ya = y0 + rh * (float)j, yb = ya + rh, xm = 0.5f * (xa + xb), ym = 0.5f * (ya + yb);
            /* a diamond in each cell, its points on the cell's sides */
            beam(pm, xa, ym, 0.0f, xm, yb, 0.0f, 0.018f, 0.025f, up);
            beam(pm, xm, yb, 0.0f, xb, ym, 0.0f, 0.018f, 0.025f, up);
            beam(pm, xb, ym, 0.0f, xm, ya, 0.0f, 0.018f, 0.025f, up);
            beam(pm, xm, ya, 0.0f, xa, ym, 0.0f, 0.018f, 0.025f, up);
            if (L0) { pm_push(pm); pm_translate(pm, xm, ym, 0.0f); pm_rotate_x(pm, HALF_PI); pm_cyl(pm, -0.016f, 0.016f, 0.035f, 8); pm_pop(pm); }
        }
    if (!L2) {   /* the band of bars under the top rail */
        for (int k = 1; k < 8; ++k) { const float x = f + (1.0f - 2.0f * f) * (float)k / 8.0f; bx(pm, x - 0.008f, y1 + 0.03f, -0.008f, x + 0.008f, 1.0f - f, 0.008f, 0.0f); }
    }
}

/* Razor wire: a helical coil along x, sitting on y 0. */
void m_r_razor(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float R = 0.5f * HR_RAZOR_D;
    const int turns = L2 ? 4 : 9, per = L0 ? 10 : L1 ? 6 : 4;
    PmPath pa;
    pm_path_init(&pa, turns * per + 2);
    for (int k = 0; k <= turns * per; ++k) {
        const float a = 6.2831853f * (float)k / (float)per, x = (float)k / (float)(turns * per);
        pm_path_add(&pa, x, R + R * cosf(a) * 0.96f, R * sinf(a) * 0.96f, 0);
    }
    pm_tube(pm, &pa, L2 ? 0.012f : 0.006f, 4, 3);
    pm_path_free(&pa);
}

/* ----------------------------------------------------------------- lights --- */
/* A coach lantern hanging from its bracket: a wall plate, the arm, a tapered glass body with corner bars, a roof and
 * a finial (the sheets' lanterns beside Georgian, Victorian and Middle Eastern doors and on gate piers). */
void m_r_lantern(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    const float z = HR_LANTERN_Z;
    bx(pm, -0.045f, -0.08f, 0.0f, 0.045f, 0.2f, 0.014f, 0.004f);
    beam(pm, 0.0f, 0.17f, 0.01f, 0.0f, 0.17f, z + 0.02f, 0.016f, 0.016f, NULL);
    if (!L2) beam(pm, 0.0f, 0.0f, 0.012f, 0.0f, 0.17f, z * 0.6f, 0.012f, 0.012f, NULL);   /* the strut */
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, z);
    frustum(pm, -0.2f, -0.17f, 0.05f, 0.075f, 4);                          /* the base */
    pm_mat(pm, PMAT_HLAMP);
    frustum(pm, -0.17f, 0.04f, 0.065f, 0.085f, 4);                         /* the glass */
    pm_mat(pm, PMAT_HIRON);
    frustum(pm, 0.04f, 0.12f, 0.1f, 0.02f, 4);                             /* the roof */
    if (!L2) {
        for (int k = 0; k < 4; ++k) {
            const float sx = k & 1 ? 1.0f : -1.0f, sz = k & 2 ? 1.0f : -1.0f;
            beam(pm, sx * 0.066f, -0.17f, sz * 0.066f, sx * 0.086f, 0.04f, sz * 0.086f, 0.012f, 0.012f, NULL);
        }
        cyl_y(pm, 0.12f, 0.17f, 0.0f, 0.0f, 0.008f, 5);                     /* the hanger */
    }
    if (L0) { PmProf f[4] = { { 0, -0.24f, 0 }, { 0.016f, -0.225f, 0 }, { 0.008f, -0.2f, 0 }, { 0, -0.198f, 0 } }; pm_lathe(pm, f, 4, 8, 0, 0); }
    pm_pop(pm);
}
/* A bulkhead: an oval cast body on the wall, its opal lens, a cage of three bars over it. */
void m_r_bulkhead(Pm *pm) {
    quiet(pm);
    pm_push(pm); pm_rotate_x(pm, HALF_PI);   /* the lathe's y out of the wall (+z), its long axis along x */
    pm_mat(pm, PMAT_HGALV);
    PmProf b[5] = { { 0, 0.0f, 0 }, { 0.075f, 0.0f, 1 }, { 0.075f, 0.03f, 0 }, { 0.068f, 0.04f, 1 }, { 0, 0.04f, 0 } };
    pm_lathe(pm, b, 5, segs(pm, 16, 8), 0.05f, 0.0f);
    pm_mat(pm, PMAT_HLAMP);
    PmProf g[4] = { { 0, 0.036f, 0 }, { 0.062f, 0.036f, 1 }, { 0.05f, 0.07f, 0 }, { 0, 0.078f, 0 } };
    pm_lathe(pm, g, 4, segs(pm, 16, 8), 0.05f, 0.0f);
    pm_pop(pm);
    if (L0) {
        pm_mat(pm, PMAT_HGALV);
        for (int k = -1; k <= 1; ++k) beam(pm, -0.11f, 0.03f * (float)k, 0.06f, 0.11f, 0.03f * (float)k, 0.06f, 0.008f, 0.008f, NULL);
    }
}

/* ----------------------------------------------------------------- plants --- */
/* Plants stand for the sheets' pot plants, vines and trailing greenery: clumps of leaves (squat blobs), stems, and
 * flowers as small bright clumps; never a card, so they cast and catch the city's light as the rest of the kit does. */
void m_r_pot(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HCLAY);
    PmProf p[8] = { { 0, 0, 0 }, { 0.12f, 0.0f, 1 }, { 0.17f, 0.29f, 0 }, { 0.185f, 0.30f, 1 }, { 0.185f, 0.34f, 1 },
                    { 0.165f, 0.34f, 1 }, { 0.165f, 0.32f, 1 }, { 0, 0.32f, 0 } };
    pm_lathe(pm, p, 8, segs(pm, 14, 6), 0, 0);
    pm_mat(pm, PMAT_HPLANT);
    uint32_t h = 0x9071u;
    const int n = L2 ? 2 : L1 ? 5 : 9;
    for (int k = 0; k < n; ++k) {
        const float a = 6.2831853f * rnd(&h), rr = 0.12f * rnd(&h);
        pm->part = (uint8_t)k;
        clump(pm, rr * cosf(a), 0.28f + 0.22f * rnd(&h), rr * sinf(a), 0.11f + 0.08f * rnd(&h), 0.18f + 0.14f * rnd(&h), 1.0f);
    }
    if (!L2) {
        pm_mat(pm, PMAT_HBLOOM);
        for (int k = 0; k < (L0 ? 4 : 2); ++k) {
            const float a = 6.2831853f * rnd(&h);
            clump(pm, 0.13f * cosf(a), 0.5f + 0.12f * rnd(&h), 0.13f * sinf(a), 0.04f, 0.05f, 1.0f);
        }
    }
    pm->part = 0;
}
/* A banana or a palm in a big concrete pot: a stem and arching leaves (two pieces each), the sheets' tropical yards. */
void m_r_pot_palm(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    PmProf p[7] = { { 0, 0, 0 }, { 0.2f, 0.0f, 1 }, { 0.25f, 0.42f, 1 }, { 0.27f, 0.42f, 1 }, { 0.27f, 0.46f, 1 }, { 0.23f, 0.46f, 1 }, { 0, 0.44f, 0 } };
    pm_lathe(pm, p, 7, segs(pm, 14, 6), 0, 0);
    pm_mat(pm, PMAT_HTIMBER);
    pm_cyl(pm, 0.4f, 1.0f, 0.04f, segs(pm, 7, 4));
    pm_mat(pm, PMAT_HPLANT);
    uint32_t h = 0xBA7Au;
    const int n = L2 ? 5 : L1 ? 8 : 11;
    for (int k = 0; k < n; ++k) {
        /* each leaf arches up and out from the crown and droops at its tip: three pieces, broad in the middle */
        const float a = 6.2831853f * ((float)k + 0.35f * rnd(&h)) / (float)n, c = cosf(a), s = sinf(a);
        const float y0 = 0.92f + 0.2f * rnd(&h), reach = 0.5f + 0.3f * rnd(&h), lift = 0.3f + 0.3f * rnd(&h);
        const float up[3] = { -s, 0.0f, c };   /* the leaf's flat across its sweep */
        float P[4][3];
        const float rr[4] = { 0.0f, 0.35f, 0.7f, 1.0f }, yy[4] = { 0.0f, 0.75f, 0.85f, 0.35f };
        for (int j = 0; j < 4; ++j) { P[j][0] = c * reach * rr[j]; P[j][1] = y0 + lift * yy[j]; P[j][2] = s * reach * rr[j]; }
        pm->part = (uint8_t)k;
        if (L2) { beam(pm, P[0][0], P[0][1], P[0][2], P[3][0], P[3][1], P[3][2], 0.03f, 0.16f, up); continue; }
        static const float WID[3] = { 0.09f, 0.17f, 0.1f };
        for (int j = 0; j < 3; ++j) beam(pm, P[j][0], P[j][1], P[j][2], P[j + 1][0], P[j + 1][1], P[j + 1][2], 0.015f, WID[j], up);
    }
    pm->part = 0;
}
/* A concrete trough along x with shrubs and a few flowers. */
void m_r_trough(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    bx(pm, 0.0f, 0.0f, -0.17f, 1.0f, 0.35f, 0.17f, 0.01f);
    pm_mat(pm, PMAT_HPLANT);
    uint32_t h = 0x7206u;
    const int n = L2 ? 3 : L1 ? 5 : 8;
    for (int k = 0; k < n; ++k) {
        pm->part = (uint8_t)k;
        clump(pm, (0.5f + (float)k) / (float)n, 0.3f, -0.06f + 0.12f * rnd(&h), 0.12f + 0.05f * rnd(&h), 0.22f + 0.18f * rnd(&h), 1.0f);
    }
    if (!L2) {
        pm_mat(pm, PMAT_HBLOOM);
        for (int k = 0; k < (L0 ? 5 : 2); ++k) clump(pm, 0.1f + 0.8f * rnd(&h), 0.45f + 0.12f * rnd(&h), -0.08f + 0.16f * rnd(&h), 0.04f, 0.05f, 1.0f);
    }
    pm->part = 0;
}
/* HMv: a climbing plant is one card standing HR_VINE_DEPTH off its wall, a unit across (x -0.5..0.5) and up (y 0..1),
 * stretched over the plant by its instance; its uv the card's own unit square. Its leaves are the card's steep
 * parallax down to the wall (shaders/city/house.glsl shadeVine), so it is the same two triangles at every level. */
void m_r_vine(Pm *pm) {
    quiet(pm);
    pm_mat(pm, PMAT_HVINE);
    const float n[3] = { 0.0f, 0.0f, 1.0f }, z = HR_VINE_DEPTH;
    const float p0[3] = { -0.5f, 0.0f, z }, p1[3] = { 0.5f, 0.0f, z }, p2[3] = { 0.5f, 1.0f, z }, p3[3] = { -0.5f, 1.0f, z };
    const uint32_t a = pm_vert(pm, p0, n, 0.0f, 0.0f), b = pm_vert(pm, p1, n, 1.0f, 0.0f);
    const uint32_t c = pm_vert(pm, p2, n, 1.0f, 1.0f), d = pm_vert(pm, p3, n, 0.0f, 1.0f);
    pm_quad(pm, a, b, c, d);
}

/* ------------------------------------------------------------- assemblies --- */
static void place_at(Pm *pm, int module, float x, float y, float z, float sx, float sy, float sz) {
    pm_push(pm); pm_translate(pm, x, y, z); pm_scale(pm, sx, sy, sz); pm_place(pm, module); pm_pop(pm);
}

void hr_solar_canopy(Pm *pm, float w, float d, float h, float rise, int lights, const float *feet) {
    const int nx = w > 3.6f ? 1 + (int)ceilf(w / 3.6f) : 2;
    const float hw = 0.5f * w, hd = 0.5f * d, in = 0.5f * HR_POST;
    const float L = sqrtf(d * d + rise * rise), tilt = atan2f(rise, d);
    for (int i = 0; i < nx; ++i) {
        const float x = -hw + in + (w - 2.0f * in) * (float)i / (float)(nx - 1);
        for (int k = 0; k < 2; ++k) {
            const float top = (k ? h + rise : h) - HR_BEAM_H, foot = feet ? feet[2 * i + k] : 0.0f;
            if (top - foot > 0.02f) place_at(pm, PMK_R_FRAME_POST, x, foot, k ? hd - in : -hd + in, 1.0f, top - foot, 1.0f);
        }
        /* a rafter along the slope over the post pair */
        pm_push(pm); pm_translate(pm, x, h, -hd); pm_rotate_x(pm, -tilt); pm_rotate_y(pm, -HALF_PI); pm_scale(pm, L, 1.0f, 1.0f);
        pm_place(pm, PMK_R_FRAME_BEAM);
        pm_pop(pm);
        /* downlights under the rafters, between the posts */
        if (lights && i > 0 && i < nx - 1)
            for (int k = 1; k <= 2; ++k) {
                const float s = (float)k / 3.0f;
                place_at(pm, PMK_R_DOWNLIGHT, x, h + rise * s - HR_BEAM_H, -hd + d * s, 1.0f, 1.0f, 1.0f);
            }
    }
    /* the front and back beams */
    place_at(pm, PMK_R_FRAME_BEAM, -hw, h - 0.002f, -hd + in, w, 1.0f, 1.0f);
    place_at(pm, PMK_R_FRAME_BEAM, -hw, h + rise - 0.002f, hd - in, w, 1.0f, 1.0f);
    if (lights) {   /* along the front beam's underside: the sheets' warm points of light under the canopy's edge */
        const int nl = nx > 2 ? nx - 1 : 2;
        for (int k = 0; k < nl; ++k) place_at(pm, PMK_R_DOWNLIGHT, -hw + w * ((float)k + 0.5f) / (float)nl, h - HR_BEAM_H, -hd + in + 0.07f, 1.0f, 1.0f, 1.0f);
    }
    /* the panels: rows of three across, whole rows up the slope, each stretched a little to fill the deck */
    const float rowW = (float)HR_ROW_N * (HR_PANEL_W + 0.02f);
    int nr = (int)floorf(w / rowW + 0.5f), nd = (int)floorf(L / (HR_PANEL_L + 0.02f) + 0.5f);
    if (nd < 1) nd = 1;
    const int single = nr < 1 || w / ((float)nr * rowW) > 1.3f || w / ((float)nr * rowW) < 0.75f;
    const float cell = single ? HR_PANEL_W + 0.02f : rowW;
    int nc = single ? (int)floorf(w / cell + 0.5f) : nr;
    if (nc < 1) nc = 1;
    const float sx = w / ((float)nc * cell), sz = L / ((float)nd * (HR_PANEL_L + 0.02f));
    for (int j = 0; j < nd; ++j)
        for (int i = 0; i < nc; ++i) {
            const float s = L * ((float)j + 0.5f) / (float)nd;
            pm_push(pm);
            pm_translate(pm, -hw + w * ((float)i + 0.5f) / (float)nc, h, -hd);
            pm_rotate_x(pm, -tilt);
            pm_translate(pm, 0.0f, 0.0f, s);
            pm_scale(pm, sx, 1.0f, sz);
            pm_place(pm, single ? PMK_R_SOLAR_PANEL : PMK_R_SOLAR_ROW);
            pm_pop(pm);
        }
}

void hr_solar_flush(Pm *pm, int cols, int rows) {
    const float pw = HR_PANEL_W + 0.02f, pl = HR_PANEL_L + 0.02f;
    for (int j = 0; j < rows; ++j) {
        const float z = pl * ((float)j + 0.5f);
        int i = 0;
        while (i < cols) {
            if (cols - i >= HR_ROW_N) {
                const float x = pw * ((float)i + 0.5f * HR_ROW_N - 0.5f * (float)cols);
                place_at(pm, PMK_R_SOLAR_ROW, x, 0.0f, z, 1.0f, 1.0f, 1.0f);
                i += HR_ROW_N;
            } else {
                const float x = pw * ((float)i + 0.5f - 0.5f * (float)cols);
                place_at(pm, PMK_R_SOLAR_PANEL, x, 0.0f, z, 1.0f, 1.0f, 1.0f);
                i++;
            }
        }
    }
}

float hr_tank(Pm *pm, int kind, float size, float stand) {
    float y = 0.0f;
    if (stand > 0.3f) {
        int n = (int)floorf(stand / HR_STAND_BAY + 0.5f);
        if (n < 1) n = 1;
        const float bh = stand / (float)n;
        for (int k = 0; k < n; ++k) place_at(pm, PMK_R_STAND_BAY, 0.0f, bh * (float)k, 0.0f, 1.0f, bh / HR_STAND_BAY, 1.0f);
        place_at(pm, PMK_R_STAND_TOP, 0.0f, stand, 0.0f, 1.0f, 1.0f, 1.0f);
        y = stand + HR_STAND_TOP;
    }
    if (kind == 0) { place_at(pm, PMK_R_TANK_POLY, 0.0f, y, 0.0f, size, size, size); return y + HR_TANK_H * size; }
    place_at(pm, PMK_R_TANK_STEEL, 0.0f, y, 0.0f, size, size, size);
    return y + HR_STEEL_H * size;
}

/* a frame with its y along unit d from o (x and z square to it, right-handed) */
static void frame_y(Pm *pm, const float o[3], const float d[3]) {
    float ez[3] = { 0.0f, 0.0f, 1.0f };
    if (fabsf(d[2]) > 0.9f) { ez[0] = 1.0f; ez[2] = 0.0f; }
    float ex[3] = { d[1] * ez[2] - d[2] * ez[1], d[2] * ez[0] - d[0] * ez[2], d[0] * ez[1] - d[1] * ez[0] };
    const float l = sqrtf(ex[0] * ex[0] + ex[1] * ex[1] + ex[2] * ex[2]);
    for (int k = 0; k < 3; ++k) ex[k] /= l;
    const float z2[3] = { ex[1] * d[2] - ex[2] * d[1], ex[2] * d[0] - ex[0] * d[2], ex[0] * d[1] - ex[1] * d[0] };
    pm_push(pm);
    pm_frame(pm, o, ex, d, z2);
}

void hr_pipe_path(Pm *pm, int fe, const float (*p)[3], int n) {
    const int pipe = fe ? PMK_R_PIPE_FE : PMK_R_PIPE_CU, bend = fe ? PMK_R_ELBOW_FE : PMK_R_ELBOW_CU;
    for (int i = 0; i + 1 < n; ++i) {
        float d[3] = { p[i + 1][0] - p[i][0], p[i + 1][1] - p[i][1], p[i + 1][2] - p[i][2] };
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-4f) continue;
        for (int k = 0; k < 3; ++k) d[k] /= len;
        const float a = i > 0 ? HR_BEND : 0.0f, b = i + 2 < n ? HR_BEND : 0.0f;
        if (len - a - b > 0.005f) {
            const float o[3] = { p[i][0] + d[0] * a, p[i][1] + d[1] * a, p[i][2] + d[2] * a };
            frame_y(pm, o, d);
            pm_scale(pm, 1.0f, len - a - b, 1.0f);
            pm_place(pm, pipe);
            pm_pop(pm);
        }
        if (i + 2 < n) {   /* the bend at p[i + 1]: in along d, out along the next leg */
            float e[3] = { p[i + 2][0] - p[i + 1][0], p[i + 2][1] - p[i + 1][1], p[i + 2][2] - p[i + 1][2] };
            const float le = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            if (le < 1e-4f) continue;
            for (int k = 0; k < 3; ++k) e[k] /= le;
            const float o[3] = { p[i + 1][0] - d[0] * HR_BEND, p[i + 1][1] - d[1] * HR_BEND, p[i + 1][2] - d[2] * HR_BEND };
            const float ex[3] = { d[1] * e[2] - d[2] * e[1], d[2] * e[0] - d[0] * e[2], d[0] * e[1] - d[1] * e[0] };
            pm_push(pm);
            pm_frame(pm, o, ex, d, e);
            pm_place(pm, bend);
            pm_pop(pm);
        }
    }
}

void hr_pipe_clips(Pm *pm, int fe, const float a[3], const float b[3]) {
    (void)fe;
    const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 0.5f) return;
    const int n = (int)floorf((len - 0.3f) / 1.2f) + 1;
    for (int k = 0; k < n; ++k) {
        const float t = n > 1 ? (0.15f + (len - 0.3f) * (float)k / (float)(n - 1)) / len : 0.5f;
        place_at(pm, PMK_R_PIPE_CLIP, a[0] + d[0] * t, a[1] + d[1] * t, a[2] + d[2] * t, 1.0f, 1.0f, 1.0f);
    }
}

void hr_breeze(Pm *pm, int kind, float w, float h) {
    int nx = (int)floorf(w / HR_BREEZE + 0.5f), ny = (int)floorf(h / HR_BREEZE + 0.5f);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    const float sx = w / ((float)nx * HR_BREEZE), sy = h / ((float)ny * HR_BREEZE);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            place_at(pm, kind == 2 ? PMK_R_BREEZE_STAR : kind ? PMK_R_BREEZE_RING : PMK_R_BREEZE_DIAMOND, HR_BREEZE * sx * (float)i, HR_BREEZE * sy * (float)j, 0.0f, sx, sy, 1.0f);
}

void hr_rail(Pm *pm, float len) {
    if (len < 0.2f) return;
    int n = (int)floorf(len + 0.5f);
    if (n < 1) n = 1;
    const float seg = len / (float)n;
    for (int k = 0; k < n; ++k) place_at(pm, PMK_R_RAIL, seg * (float)k, 0.0f, 0.0f, seg, 1.0f, 1.0f);
    place_at(pm, PMK_R_FRAME_POST, len - 0.02f, 0.0f, 0.0f, 0.5f, HR_RAIL_H, 0.5f);
}

void hr_razor(Pm *pm, float len) {
    if (len < 0.3f) return;
    int n = (int)floorf(len + 0.5f);
    if (n < 1) n = 1;
    const float seg = len / (float)n;
    for (int k = 0; k < n; ++k) place_at(pm, PMK_R_RAZOR, seg * (float)k, 0.0f, 0.0f, seg, 1.0f, 1.0f);
}

void hr_vine(Pm *pm, float w, float h, int species) {
    if (w < 0.2f || h < 0.5f) return;
    const uint8_t part = pm->part;
    pm->part = (uint8_t)species;   /* the instance's part: house.glsl shadeVine's atlas column */
    pm_push(pm);
    pm_scale(pm, w, h, 1.0f);
    pm_place(pm, PMK_R_VINE);
    pm_pop(pm);
    pm->part = part;
}

/* Plants trailing over an edge, from the vines' art (the leaf-blob strands read as rows of bells close to): a vine's
 * card hung upside down in front of the edge's face, its thinning top become the strands' tips, and a low card of the
 * same plant standing on the edge behind it, its top thinning to tufts; both cards' cut edges at the edge's line. */
void hr_trail(Pm *pm, float len, float drop, int species) {
    if (len < 0.4f || drop < 0.15f) return;
    const uint8_t part = pm->part;
    pm->part = (uint8_t)species;   /* house.glsl shadeVine's atlas column */
    pm_push(pm);                   /* hanging: the card's foot at the edge (a little over it), its top the tips */
    pm_translate(pm, 0.5f * len, 0.04f, 0.0f);
    pm_rotate_z(pm, PI_F);
    pm_scale(pm, len, drop + 0.04f, 1.0f);
    pm_place(pm, PMK_R_VINE);
    pm_pop(pm);
    pm_push(pm);                   /* on the edge: standing just in front of the face's plane, set back by its depth */
    pm_translate(pm, 0.5f * len, 0.0f, -HR_VINE_DEPTH);
    pm_scale(pm, 0.92f * len, 0.32f, 1.0f);
    pm_place(pm, PMK_R_VINE);
    pm_pop(pm);
    pm->part = part;
}

void hr_led(Pm *pm, float len) { if (len > 0.2f) place_at(pm, PMK_R_LED_STRIP, 0.0f, 0.0f, 0.0f, len, 1.0f, 1.0f); }
