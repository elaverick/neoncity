#include "pmodel_region.h"
#include "mathlib.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The other regions' kit (residential roadmap HM5b1): see pmodel_region.h for
 * the frames, and the concept sheets in docs/reference/international-residential
 * for what each piece answers (the sheet and panel named on each).
 *
 *   HM5b1a   Japan, China, Korea, the shophouse, the link house
 *   HM5b1b   RCC, the bungalow, the Middle Eastern villa and courtyard house, the block, the walled compound
 * ========================================================================= */

#define L0 (pm->lod == PM_LOD0)
#define L1 (pm->lod == PM_LOD1)
#define L2 (pm->lod == PM_LOD2)
#define HALF_PI 1.5707963f
#define DEG (PI_F / 180.0f)

/* ---------------------------------------------------------------- helpers --- */
/* the house kits keep no support loops (pmodel_house.c's quiet) */
static void quiet(Pm *pm) { pm->support = 0.0f; pm->maxlen = 0.0f; }
static int segs(const Pm *pm, int base, int lo) { return pm_segs(pm, base, lo); }
static void bx(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    const float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    const float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    pm_bbox(pm, c, h, L2 ? 0.0f : bevel, 1);
}
static void ccw(float (*p)[2], int n) {
    float a = 0.0f;
    for (int k = 0; k < n; ++k) { const int j = (k + 1) % n; a += p[k][0] * p[j][1] - p[j][0] * p[k][1]; }
    if (a >= 0.0f) return;
    for (int k = 0; k < n / 2; ++k) { float t0 = p[k][0], t1 = p[k][1]; p[k][0] = p[n - 1 - k][0]; p[k][1] = p[n - 1 - k][1]; p[n - 1 - k][0] = t0; p[n - 1 - k][1] = t1; }
}
/* A section (n points (out z, up y), either winding) run along x from x0 to x1. */
static void run_x(Pm *pm, const float (*sec)[2], int n, float x0, float x1) {
    float poly[64][2];
    if (n > 64) n = 64;
    memcpy(poly, sec, sizeof(float) * 2 * (size_t)n);
    ccw(poly, n);
    pm_push(pm);
    pm_rotate_y(pm, -HALF_PI);   /* the section's first coordinate onto +z, the extrusion's z onto -x */
    pm_extrude(pm, (const float (*)[2])poly, n, -x1, -x0);
    pm_pop(pm);
}
/* A section (n points (x, y)) run along z from z0 to z1 (a board in the wall's plane). */
static void run_z(Pm *pm, const float (*sec)[2], int n, float z0, float z1) {
    float poly[64][2];
    if (n > 64) n = 64;
    memcpy(poly, sec, sizeof(float) * 2 * (size_t)n);
    ccw(poly, n);
    pm_extrude(pm, (const float (*)[2])poly, n, z0, z1);
}
/* four members round a rectangle (x0..x1, y0..y1), sides ws wide, the bottom wb, the top wt, z0..z1 deep */
static void rect_frame(Pm *pm, float x0, float y0, float x1, float y1, float ws, float wb, float wt, float z0, float z1, float bev) {
    bx(pm, x0, y0, z0, x1, y0 + wb, z1, bev);
    bx(pm, x0, y1 - wt, z0, x1, y1, z1, bev);
    bx(pm, x0, y0 + wb, z0, x0 + ws, y1 - wt, z1, bev);
    bx(pm, x1 - ws, y0 + wb, z0, x1, y1 - wt, z1, bev);
}
/* cylinders along x through (y, z), along z through (x, y), up y through (x, z) */
static void cyl_x(Pm *pm, float x0, float x1, float y, float z, float r, int s) {
    pm_push(pm); pm_translate(pm, 0.0f, y, z); pm_rotate_z(pm, -HALF_PI); pm_cyl(pm, x0, x1, r, s); pm_pop(pm);
}
static void cyl_z(Pm *pm, float z0, float z1, float x, float y, float r, int s) {
    pm_push(pm); pm_translate(pm, x, y, 0.0f); pm_rotate_x(pm, HALF_PI); pm_cyl(pm, z0, z1, r, s); pm_pop(pm);
}
/* a bar of w x w from (ax, ay) to (bx, by) in the plane z0..z1 (a lattice's or a vent's diagonal) */
static void bar_xy(Pm *pm, float ax, float ay, float bx_, float by, float w, float z0, float z1) {
    const float dx = bx_ - ax, dy = by - ay, l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-5f) return;
    pm_push(pm);
    pm_translate(pm, 0.5f * (ax + bx_), 0.5f * (ay + by), 0.0f);
    pm_rotate_z(pm, atan2f(dy, dx));
    bx(pm, -0.5f * l, -0.5f * w, z0, 0.5f * l, 0.5f * w, z1, 0.0f);
    pm_pop(pm);
}

/* ======================================================= Japan (HM5b1a) === */
/* An aluminium sliding window (the Japan sheet's panels 1, 4, 5): a deep frame (the tracks), two sashes overlapping at
 * the middle, the left behind, the right in front; a crescent lock where they meet. The frosted one carries milky panes
 * (the vertex's piece 1: house.glsl lights them by night as the room behind). */
static void sliding(Pm *pm, int frost) {
    pm_mat(pm, PMAT_HALU);
    const float W = HK_SLIDE_W, H = HK_SLIDE_H, x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.04f;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.05f, fw, 0.0f, 0.07f, 0.0f);
    for (int s = 0; s < 2; ++s) {
        const float lx0 = s ? -0.015f : x0 + fw, lx1 = s ? x1 - fw : 0.015f, z0 = s ? 0.04f : 0.012f, z1 = z0 + 0.022f;
        pm_mat(pm, PMAT_HALU);
        if (L2) bx(pm, s ? -0.015f : -0.02f, 0.05f, z0, s ? 0.02f : 0.015f, H - fw, z1, 0.0f);   /* the meeting stiles */
        else rect_frame(pm, lx0, 0.05f, lx1, H - fw, 0.035f, 0.04f, 0.035f, z0, z1, 0.0f);
        if (frost) {
            pm_mat(pm, PMAT_HFROST);
            pm->part = 1;
            bx(pm, lx0 + 0.035f, 0.09f, z0 + 0.008f, lx1 - 0.035f, H - fw - 0.035f, z0 + 0.012f, 0.0f);
            pm->part = 0;
        }
    }
    if (L0) { pm_mat(pm, PMAT_HALU); bx(pm, -0.03f, 0.48f * H, 0.062f, 0.0f, 0.48f * H + 0.04f, 0.075f, 0.0f); }   /* the lock */
}
void m_h_win_sliding(Pm *pm) { quiet(pm); sliding(pm, 0); }
void m_h_win_sliding_frost(Pm *pm) { quiet(pm); sliding(pm, 1); }

/* The storm shutters' pocket box (panels 1, 4): pressed aluminium beside the window, its face ribbed, a cap over it. */
void m_h_amado(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    const float W = HK_AMADO_W, y0 = -0.05f, y1 = HK_AMADO_H - 0.05f;
    bx(pm, 0.0f, y0, 0.0f, W, y1, 0.13f, 0.006f);
    bx(pm, -0.01f, y1, 0.0f, W + 0.01f, y1 + 0.025f, 0.15f, 0.004f);
    if (L2) return;
    const int n = L0 ? 12 : 6;
    for (int k = 1; k < n; ++k) { const float y = y0 + (y1 - y0) * (float)k / (float)n; bx(pm, 0.02f, y - 0.006f, 0.13f, W - 0.02f, y + 0.006f, 0.136f, 0.0f); }
}
/* The rail the shutters slide in (x 0..1), over the window's head: a back strip and a front lip. */
void m_h_amado_rail(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    bx(pm, 0.0f, 0.0f, 0.0f, 1.0f, 0.05f, 0.02f, 0.0f);
    bx(pm, 0.0f, 0.0f, 0.075f, 1.0f, 0.05f, 0.09f, 0.0f);
    bx(pm, 0.0f, 0.04f, 0.02f, 1.0f, 0.05f, 0.075f, 0.0f);
}
/* A door hood (hisashi, panels 1, 5): a thin aluminium plate out from the wall falling 3 degrees, a deep fascia. */
void m_h_hisashi(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    const float D = HK_HISASHI_D, f = -D * tanf(3.0f * DEG);
    const float s[6][2] = { { 0.0f, 0.0f }, { D, f }, { D, f - 0.07f }, { D - 0.025f, f - 0.07f }, { D - 0.025f, f - 0.03f }, { 0.0f, -0.03f } };
    run_x(pm, s, 6, 0.0f, 1.0f);
}
/* A slat screen (panel 5's band under the roof; x 0..1 and y 0..1 stretched): horizontal slats on two battens. */
void m_h_slats(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    if (L2) { bx(pm, 0.0f, 0.0f, 0.03f, 1.0f, 1.0f, 0.06f, 0.0f); return; }
    for (int k = 0; k < 2; ++k) { const float x = k ? 0.88f : 0.08f; bx(pm, x, 0.0f, 0.0f, x + 0.04f, 1.0f, 0.03f, 0.0f); }
    const int n = L0 ? 12 : 6;
    for (int k = 0; k < n; ++k) { const float y = (float)k / (float)n; bx(pm, 0.0f, y + 0.01f, 0.03f, 1.0f, y + (L0 ? 0.055f : 0.12f), 0.06f, L0 ? 0.004f : 0.0f); }
}

/* ================================================ China, Korea (HM5b1a) === */
/* A ridge end (the China sheet's panel 1, the gatehouse): a block gripping the ridge's end, a horn rising from it that
 * curls back in over the ridge, in the roof's covering. Its silhouette is what reads, side on from the street. */
void m_h_ridge_end(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HROOF);
    const float k = HK_RIDGE_END_H / 0.70f;
    const float o[17][2] = { { 0.0f, 0.0f }, { 0.40f, 0.0f }, { 0.40f, 0.12f }, { 0.22f, 0.16f }, { 0.11f, 0.26f }, { 0.10f, 0.40f },
                             { 0.17f, 0.49f }, { 0.28f, 0.48f }, { 0.34f, 0.55f }, { 0.29f, 0.63f }, { 0.18f, 0.69f }, { 0.06f, 0.70f },
                             { -0.04f, 0.65f }, { -0.09f, 0.54f }, { -0.08f, 0.38f }, { -0.04f, 0.20f }, { -0.01f, 0.06f } };
    float p[17][2];
    for (int i = 0; i < 17; ++i) { p[i][0] = o[i][0] * k; p[i][1] = o[i][1] * k; }
    run_z(pm, (const float (*)[2])p, 17, -0.07f, 0.07f);
    bx(pm, -0.01f, 0.0f, -0.12f, 0.42f, 0.15f, 0.12f, 0.01f);   /* the block over the ridge's rolls */
    if (L0) {   /* a fin down the horn's back, its spines */
        const float f[5][2] = { { -0.07f * k, 0.30f * k }, { -0.12f * k, 0.36f * k }, { -0.13f * k, 0.48f * k }, { -0.09f * k, 0.52f * k }, { -0.08f * k, 0.42f * k } };
        run_z(pm, f, 5, -0.02f, 0.02f);
    }
}
/* Korea's (the Korea sheet's panel 4, the hanok): a lower block sweeping up off the ridge's end, an end tile on its face. */
void m_h_ridge_end_kr(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HROOF);
    const float p[8][2] = { { 0.0f, 0.0f }, { 0.50f, 0.0f }, { 0.50f, 0.16f }, { 0.30f, 0.19f }, { 0.14f, 0.26f }, { 0.02f, 0.36f },
                            { -0.07f, 0.40f }, { -0.04f, 0.18f } };
    run_z(pm, p, 8, -0.11f, 0.11f);
    pm_push(pm); pm_translate(pm, -0.055f, 0.27f, 0.0f); pm_rotate_z(pm, HALF_PI + 0.35f);
    pm_cyl(pm, 0.0f, 0.03f, 0.09f, 12);
    pm_pop(pm);
}
/* Eave-end tiles (China 1, Korea 4): four round caps a metre ending the cover tiles, a pointed drip tile between them
 * under the pan tiles' ends, in the covering. */
void m_h_eave_tiles(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HROOF);
    for (int k = 0; k < 4; ++k) {
        /* the caps between the drips; a piece's last drip straddles its end (the next piece has none at its start) */
        const float xc = HK_EAVE_PITCH * ((float)k + 0.5f), xd = HK_EAVE_PITCH * (float)(k + 1);
        cyl_z(pm, -0.03f, 0.03f, xc, 0.055f, 0.068f, 12);   /* pm_cyl reduces its segments by level */
        if (L0) cyl_z(pm, 0.03f, 0.042f, xc, 0.055f, 0.032f, 8);   /* the cap's boss */
        const float d[5][2] = { { xd - 0.075f, 0.03f }, { xd + 0.075f, 0.03f }, { xd + 0.075f, -0.03f }, { xd, -0.10f }, { xd - 0.075f, -0.03f } };
        if (!L2) run_z(pm, d, 5, -0.01f, 0.012f);
    }
}
/* A ring knocker (China 1, 4, 5): a bronze plate with its boss, a ring hanging from it. */
void m_h_knocker(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCOPPER);
    cyl_z(pm, 0.0f, 0.012f, 0.0f, 0.0f, 0.085f, 16);
    if (!L2) {
        PmProf b[4] = { { 0, 0.0f, 0 }, { 0.045f, 0.0f, 1 }, { 0.03f, 0.03f, 0 }, { 0, 0.038f, 0 } };
        pm_push(pm); pm_translate(pm, 0.0f, 0.01f, 0.012f); pm_rotate_x(pm, HALF_PI); pm_lathe(pm, b, 4, segs(pm, 12, 6), 0, 0); pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, 0.0f, -0.07f, 0.03f); pm_rotate_x(pm, HALF_PI);
    pm_torus(pm, 0.065f, L2 ? 0.01f : 0.008f, 16, 6);
    pm_pop(pm);
}
/* A drum stone (China 1): a stone block, a drum on edge over it (its axis along the wall), a ring of studs round each
 * face; one each side of a gate. */
void m_h_drum_stone(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    bx(pm, -0.2f, 0.0f, -0.26f, 0.2f, 0.30f, 0.26f, 0.02f);
    bx(pm, -0.14f, 0.30f, -0.12f, 0.14f, 0.36f, 0.12f, 0.01f);   /* the cushion it rests on */
    cyl_x(pm, -0.13f, 0.13f, 0.60f, 0.0f, 0.26f, 24);
    if (L2) return;
    for (int e = 0; e < 2; ++e) {
        pm_push(pm); pm_translate(pm, e ? 0.115f : -0.115f, 0.60f, 0.0f); pm_rotate_z(pm, HALF_PI);
        pm_torus(pm, 0.245f, 0.017f, 24, 6);
        pm_pop(pm);
    }
}
/* Couplets (China 1, 2, 4, 5, 6): a red paper strip pasted on the wall, its characters the shader's; the fu diamond
 * (the vertex's piece 1: one big character). */
void m_h_couplet(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HPAPER);
    bx(pm, -0.5f * HK_COUPLET_W, 0.0f, 0.0f, 0.5f * HK_COUPLET_W, HK_COUPLET_H, 0.003f, 0.0f);
}
void m_h_fu(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HPAPER);
    pm->part = 1;
    pm_push(pm); pm_rotate_z(pm, 0.25f * PI_F);
    bx(pm, -0.5f * HK_FU, -0.5f * HK_FU, 0.0f, 0.5f * HK_FU, 0.5f * HK_FU, 0.003f, 0.0f);
    pm_pop(pm);
    pm->part = 0;
}
/* A paper lantern (China 1, 2, 6; the shophouse's panel 1): its hook, black caps, the red body (lit from inside by
 * night: PMAT_HLANTERN glows), a tassel. */
void m_h_lantern_paper(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    pm_cyl(pm, -0.08f, 0.0f, 0.006f, 6);
    const int s = segs(pm, 16, 6);
    PmProf top[4] = { { 0, -0.115f, 0 }, { 0.105f, -0.115f, 1 }, { 0.09f, -0.08f, 1 }, { 0, -0.08f, 0 } };
    pm_lathe(pm, top, 4, s, 0, 0);
    PmProf bot[4] = { { 0, -0.49f, 0 }, { 0.09f, -0.49f, 1 }, { 0.105f, -0.455f, 1 }, { 0, -0.455f, 0 } };
    pm_lathe(pm, bot, 4, s, 0, 0);
    pm_mat(pm, PMAT_HLANTERN);
    PmProf b[8] = { { 0, -0.46f, 0 }, { 0.10f, -0.46f, 1 }, { 0.17f, -0.42f, 0 }, { 0.21f, -0.34f, 0 }, { 0.215f, -0.285f, 0 },
                    { 0.21f, -0.23f, 0 }, { 0.17f, -0.15f, 0 }, { 0.10f, -0.11f, 1 } };
    PmProf bb[9];
    memcpy(bb, b, sizeof(b));
    bb[8].r = 0.0f; bb[8].y = -0.11f; bb[8].hard = 0;
    pm_lathe(pm, bb, 9, s, 0, 0);
    pm_mat(pm, PMAT_HPAPER);
    pm->part = 2;   /* plain: no characters */
    bx(pm, -0.025f, -HK_LANTERN_H, -0.025f, 0.025f, -0.49f, 0.025f, 0.0f);
    pm->part = 0;
}
/* Lattice windows (China 1, 3: the fret; Korea 4: the fine grid): a timber frame, the lattice standing over paper (the
 * frosted glass's material, piece 1: lit by night). The fret: a grid of cells, each with a square set in it and tied
 * to the cell's sides (the step pattern); Korea's: close upright bars, the cross bars in three bands. */
static void lattice_frame(Pm *pm) {
    pm_mat(pm, PMAT_HTIMBER);
    const float W = HK_LAT_W, H = HK_LAT_H, fw = 0.06f;
    rect_frame(pm, -0.5f * W, 0.0f, 0.5f * W, H, fw, fw, fw, 0.0f, 0.06f, 0.004f);
    pm_mat(pm, PMAT_HFROST);
    pm->part = 1;
    bx(pm, -0.5f * W + fw, fw, 0.003f, 0.5f * W - fw, H - fw, 0.008f, 0.0f);
    pm->part = 0;
    pm_mat(pm, PMAT_HTIMBER);
}
void m_h_lattice(Pm *pm) { quiet(pm);
    lattice_frame(pm);
    const float fw = 0.06f, X0 = -0.5f * HK_LAT_W + fw, X1 = 0.5f * HK_LAT_W - fw, Y0 = fw, Y1 = HK_LAT_H - fw, w = 0.022f;
    const int nx = 4, ny = 5;
    const float cw = (X1 - X0) / (float)nx, ch = (Y1 - Y0) / (float)ny, z0 = 0.01f, z1 = 0.048f;
    for (int i = 1; i < nx; ++i) { const float x = X0 + cw * (float)i; bx(pm, x - 0.5f * w, Y0, z0, x + 0.5f * w, Y1, z1, 0.0f); }
    for (int j = 1; j < ny; ++j) { const float y = Y0 + ch * (float)j; bx(pm, X0, y - 0.5f * w, z0, X1, y + 0.5f * w, z1, 0.0f); }
    if (L2) return;
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j) {
            const float cx = X0 + cw * ((float)i + 0.5f), cy = Y0 + ch * ((float)j + 0.5f), hx = 0.24f * cw, hy = 0.24f * ch;
            rect_frame(pm, cx - hx, cy - hy, cx + hx, cy + hy, w, w, w, z0, z1, 0.0f);
            if (!L0) continue;
            /* the ties: from the square's sides to the cell's, alternating so the steps turn */
            const int a = (i + j) & 1;
            bx(pm, a ? cx - 0.5f * w : cx - 0.5f * cw, a ? cy + hy : cy - 0.5f * w, z0, a ? cx + 0.5f * w : cx - hx, a ? cy + 0.5f * ch : cy + 0.5f * w, z1, 0.0f);
            bx(pm, a ? cx - 0.5f * w : cx + hx, a ? cy - 0.5f * ch : cy - 0.5f * w, z0, a ? cx + 0.5f * w : cx + 0.5f * cw, a ? cy - hy : cy + 0.5f * w, z1, 0.0f);
        }
}
void m_h_lattice_kr(Pm *pm) { quiet(pm);
    lattice_frame(pm);
    const float fw = 0.06f, X0 = -0.5f * HK_LAT_W + fw, X1 = 0.5f * HK_LAT_W - fw, Y0 = fw, Y1 = HK_LAT_H - fw, w = 0.014f;
    const float z0 = 0.01f, z1 = 0.04f;
    const int nv = L2 ? 3 : L1 ? 6 : 10;
    for (int i = 1; i < nv; ++i) { const float x = X0 + (X1 - X0) * (float)i / (float)nv; bx(pm, x - 0.5f * w, Y0, z0, x + 0.5f * w, Y1, z1, 0.0f); }
    for (int b = 0; b < 3; ++b) {   /* the bands: low, middle, high */
        const float yc = Y0 + (Y1 - Y0) * (b == 0 ? 0.12f : b == 1 ? 0.5f : 0.88f);
        const int nl = L2 ? 1 : 3;
        for (int k = 0; k < nl; ++k) { const float y = yc + (nl > 1 ? 0.05f * (float)(k - 1) : 0.0f); bx(pm, X0, y - 0.5f * w, z0, X1, y + 0.5f * w, z1, 0.0f); }
    }
}
/* A window cage (China 2: the blocks' steel cages, out from the wall; x -0.5..0.5 and y 0..1 stretched): square tubes
 * round its front and back to the wall, upright bars across its face, bars along its sides, a sheet over its top
 * (rain off), a shelf of bars under it. */
void m_h_cage(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HGALV);
    const float D = HK_CAGE_D, t = 0.022f;
    rect_frame(pm, -0.5f, 0.0f, 0.5f, 1.0f, t, t, t, D - t, D, 0.0f);
    for (int k = 0; k < 4; ++k) {   /* the returns to the wall */
        const float x = k & 1 ? 0.5f - t : -0.5f, y = k & 2 ? 1.0f - t : 0.0f;
        bx(pm, x, y, 0.0f, x + t, y + t, D - t, 0.0f);
    }
    const int nb = L2 ? 3 : L1 ? 5 : 9;
    for (int k = 1; k < nb; ++k) { const float x = -0.5f + (float)k / (float)nb; bx(pm, x - 0.006f, t, D - 0.016f, x + 0.006f, 1.0f - t, D - 0.006f, 0.0f); }
    bx(pm, -0.52f, 1.0f, 0.0f, 0.52f, 1.012f, D + 0.04f, 0.0f);   /* the sheet over it */
    if (L2) return;
    for (int s = 0; s < 2; ++s) {
        const float x = s ? 0.5f - 0.016f : -0.5f + 0.004f;
        for (int k = 1; k <= 2; ++k) { const float y = (float)k / 3.0f; bx(pm, x, y - 0.006f, 0.0f, x + 0.012f, y + 0.006f, D - t, 0.0f); }
    }
    for (int k = 1; k <= 2; ++k) { const float z = D * (float)k / 3.0f; bx(pm, -0.5f + t, 0.004f, z - 0.006f, 0.5f - t, 0.016f, z + 0.006f, 0.0f); }
}
/* A hazard sleeve round a pilotis column (Korea 1, 3: yellow and black, scuffed): four plates. */
void m_h_hazard(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HHAZARD);
    const float H = HK_HAZARD_H, t = 0.02f;
    bx(pm, -0.5f - t, 0.0f, 0.5f, 0.5f + t, H, 0.5f + t, 0.0f);
    bx(pm, -0.5f - t, 0.0f, -0.5f - t, 0.5f + t, H, -0.5f, 0.0f);
    bx(pm, 0.5f, 0.0f, -0.5f, 0.5f + t, H, 0.5f, 0.0f);
    bx(pm, -0.5f - t, 0.0f, -0.5f, -0.5f, H, 0.5f, 0.0f);
}
/* A bank of mailboxes (Korea 2, 5): an aluminium case, two rows of three doors with their slots, a hood over it. */
void m_h_mailbox(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    const float W = HK_MAIL_W, H = HK_MAIL_H;
    bx(pm, -0.5f * W, 0.0f, 0.0f, 0.5f * W, H, 0.13f, 0.004f);
    bx(pm, -0.5f * W - 0.02f, H, 0.0f, 0.5f * W + 0.02f, H + 0.02f, 0.16f, 0.004f);
    if (L2) return;
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) {
            const float x0 = -0.5f * W + 0.02f + (W - 0.04f) * (float)c / 3.0f, x1 = x0 + (W - 0.04f) / 3.0f - 0.012f;
            const float y0 = 0.02f + (H - 0.04f) * (float)r / 2.0f, y1 = y0 + (H - 0.04f) / 2.0f - 0.012f;
            pm_mat(pm, PMAT_HALU);
            bx(pm, x0, y0, 0.13f, x1, y1, 0.138f, 0.0f);
            if (!L0) continue;
            pm_mat(pm, PMAT_HIRON);
            bx(pm, x0 + 0.05f, y1 - 0.06f, 0.138f, x1 - 0.05f, y1 - 0.045f, 0.141f, 0.0f);   /* the slot */
            bx(pm, x1 - 0.04f, y0 + 0.05f, 0.138f, x1 - 0.025f, y0 + 0.09f, 0.146f, 0.0f);    /* the latch */
        }
}

/* ============================================ the shophouse (HM5b1a) === */
/* A pilaster (the Shophouse sheet's panels 1, 5): a plinth and a base moulding, the shaft with a sunk panel's border, a
 * capital (necking, a bell with its volutes and leaves, the abacus), in the trim's paint. */
void m_h_pilaster(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float H = HK_COL_H, w = 0.5f * HK_PIL_W;
    bx(pm, -w - 0.06f, 0.0f, 0.0f, w + 0.06f, 0.30f, 0.11f, 0.01f);
    bx(pm, -w - 0.04f, 0.30f, 0.0f, w + 0.04f, 0.38f, 0.095f, 0.025f);
    bx(pm, -w, 0.38f, 0.0f, w, H - 0.40f, 0.065f, 0.0f);
    if (!L2) rect_frame(pm, -w + 0.04f, 0.52f, w - 0.04f, H - 0.56f, 0.03f, 0.03f, 0.03f, 0.065f, 0.08f, 0.0f);
    bx(pm, -w - 0.01f, H - 0.40f, 0.0f, w + 0.01f, H - 0.34f, 0.08f, 0.01f);
    bx(pm, -w - 0.04f, H - 0.34f, 0.0f, w + 0.04f, H - 0.18f, 0.10f, 0.03f);
    bx(pm, -w - 0.08f, H - 0.18f, 0.0f, w + 0.08f, H, 0.13f, 0.01f);
    if (!L0) return;
    for (int s = 0; s < 2; ++s) cyl_z(pm, 0.0f, 0.12f, s ? w + 0.02f : -w - 0.02f, H - 0.22f, 0.045f, 10);   /* volutes */
    for (int k = 0; k < 3; ++k) {   /* leaves on the bell */
        const float x = (float)(k - 1) * 0.12f;
        PmProf l[4] = { { 0, 0.0f, 0 }, { 0.05f, 0.03f, 0 }, { 0.03f, 0.12f, 0 }, { 0, 0.15f, 0 } };
        pm_push(pm); pm_translate(pm, x, H - 0.36f, 0.10f); pm_scale(pm, 1.0f, 1.0f, 0.5f); pm_lathe(pm, l, 4, 8, 0, 0); pm_pop(pm);
    }
}
/* A round column (the five-foot way's, panels 1-6): a square plinth, a torus base, a shaft swelling a little and
 * tapering, a necking ring, the echinus, a square abacus. */
void m_h_column(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float H = HK_COL_H, r = HK_COL_R;
    bx(pm, -0.25f, 0.0f, -0.25f, 0.25f, 0.22f, 0.25f, 0.01f);
    PmProf p[14] = { { 0, 0.22f, 0 }, { 0.225f, 0.22f, 1 }, { 0.235f, 0.25f, 0 }, { 0.225f, 0.285f, 0 }, { 0.19f, 0.305f, 1 },
                     { r + 0.005f, 0.33f, 1 }, { r + 0.002f, 0.9f, 0 }, { r - 0.008f, 1.9f, 0 }, { r - 0.022f, H - 0.42f, 1 },
                     { r - 0.005f, H - 0.40f, 1 }, { r - 0.005f, H - 0.36f, 1 }, { r + 0.03f, H - 0.30f, 0 }, { 0.225f, H - 0.24f, 1 },
                     { 0, H - 0.24f, 0 } };
    pm_lathe(pm, p, 14, segs(pm, 20, 8), 0, 0);
    bx(pm, -0.25f, H - 0.24f, -0.25f, 0.25f, H - 0.08f, 0.25f, 0.01f);
    bx(pm, -0.23f, H - 0.08f, -0.23f, 0.23f, H, 0.23f, 0.0f);
}
/* A louvred French window (the shophouse's panels 1, 5: full height, green): a frame, a transom with a barred top light,
 * two leaves of louvres over a rail. */
void m_h_win_louvre(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float W = HK_LOUVRE_W, H = HK_LOUVRE_H, x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.06f, yt = H - 0.36f;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.04f, fw, 0.0f, 0.06f, 0.004f);
    bx(pm, x0 + fw, yt - 0.035f, 0.0f, x1 - fw, yt + 0.035f, 0.065f, 0.004f);
    if (!L2) for (int k = 1; k <= 3; ++k) { const float x = x0 + W * (float)k / 4.0f; bx(pm, x - 0.012f, yt + 0.035f, 0.02f, x + 0.012f, H - fw, 0.045f, 0.0f); }
    for (int s = 0; s < 2; ++s) {
        const float lx0 = s ? 0.0f : x0 + fw, lx1 = s ? x1 - fw : 0.0f, ly0 = 0.04f, ly1 = yt - 0.035f, ym = 0.95f;
        if (L2) { bx(pm, lx0 + 0.01f, ly0, 0.0f, lx1 - 0.01f, ly1, 0.045f, 0.0f); continue; }
        rect_frame(pm, lx0, ly0, lx1, ly1, 0.06f, 0.12f, 0.06f, 0.012f, 0.06f, 0.003f);
        bx(pm, lx0 + 0.06f, ym - 0.04f, 0.012f, lx1 - 0.06f, ym + 0.04f, 0.06f, 0.003f);
        const float pitch = L0 ? 0.05f : 0.1f;
        for (int p = 0; p < 2; ++p) {
            const float y0 = p ? ym + 0.04f : ly0 + 0.12f, y1 = p ? ly1 - 0.06f : ym - 0.04f;
            for (float y = y0 + 0.008f; y + 0.035f <= y1; y += pitch) {
                const float sct[4][2] = { { 0.016f, y }, { 0.026f, y }, { 0.05f, y + 0.035f }, { 0.04f, y + 0.035f } };
                run_x(pm, sct, 4, lx0 + 0.06f, lx1 - 0.06f);
            }
        }
    }
    if (L0) {   /* the handles */
        pm_mat(pm, PMAT_HIRON);
        for (int s = 0; s < 2; ++s) { const float x = s ? 0.07f : -0.07f; bx(pm, x - 0.01f, 1.0f, 0.06f, x + 0.01f, 1.14f, 0.08f, 0.0f); }
    }
}
/* A vent panel over a window (the shophouse's panel 1: pierced plaster): a border, a dark ground behind, a row of
 * diamonds with a rosette between each pair. */
void m_h_vent_panel(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float W = HK_VENT_W, H = HK_VENT_H, x0 = -0.5f * W;
    bx(pm, x0 + 0.04f, 0.04f, 0.0f, -x0 - 0.04f, H - 0.04f, 0.004f, 0.0f);   /* the dark ground seen through it */
    pm_mat(pm, PMAT_HTRIM);
    rect_frame(pm, x0, 0.0f, -x0, H, 0.05f, 0.05f, 0.05f, 0.0f, 0.04f, 0.004f);
    if (L2) return;
    const float yc = 0.5f * H, r = 0.5f * H - 0.07f, w = 0.022f;
    for (int k = 0; k < 4; ++k) {
        const float xc = x0 + W * ((float)k + 0.5f) / 4.0f;
        bar_xy(pm, xc - r, yc, xc, yc + r, w, 0.004f, 0.03f);
        bar_xy(pm, xc, yc + r, xc + r, yc, w, 0.004f, 0.03f);
        bar_xy(pm, xc + r, yc, xc, yc - r, w, 0.004f, 0.03f);
        bar_xy(pm, xc, yc - r, xc - r, yc, w, 0.004f, 0.03f);
        if (L0) { cyl_z(pm, 0.004f, 0.03f, xc, yc, 0.035f, 10); if (k) cyl_z(pm, 0.004f, 0.028f, xc - 0.5f * W / 4.0f, yc, 0.025f, 8); }
    }
    bx(pm, x0 + 0.05f, yc - 0.5f * w, 0.004f, -x0 - 0.05f, yc + 0.5f * w, 0.02f, 0.0f);
}
/* A parapet's crest (the shophouse's panels 1, 5): a gable of scrolls rising to a curved head, a panel on its face, a
 * medallion, the scrolls' eyes and a small urn on top. */
void m_h_crest(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float W = HK_CREST_W / 2.4f, Hk = HK_CREST_H / 0.95f;
    static const float R[10][2] = { { 1.2f, 0.0f }, { 1.2f, 0.12f }, { 1.1f, 0.2f }, { 0.97f, 0.27f }, { 0.82f, 0.33f }, { 0.62f, 0.40f },
                                    { 0.47f, 0.52f }, { 0.36f, 0.70f }, { 0.20f, 0.85f }, { 0.0f, 0.95f } };
    float p[20][2];
    int n = 0;
    for (int i = 0; i < 10; ++i) { p[n][0] = R[i][0] * W; p[n][1] = R[i][1] * Hk; n++; }
    for (int i = 8; i >= 0; --i) { p[n][0] = -R[i][0] * W; p[n][1] = R[i][1] * Hk; n++; }
    run_z(pm, (const float (*)[2])p, n, -0.12f, 0.0f);
    if (L2) return;
    float q[20][2];
    for (int i = 0; i < n; ++i) { q[i][0] = p[i][0] * 0.84f; q[i][1] = 0.04f + (p[i][1] - 0.0f) * 0.82f; }
    run_z(pm, (const float (*)[2])q, n, 0.0f, 0.025f);
    cyl_z(pm, 0.025f, 0.06f, 0.0f, 0.46f * Hk, 0.17f, 18);
    if (!L0) return;
    cyl_z(pm, 0.06f, 0.08f, 0.0f, 0.46f * Hk, 0.11f, 14);
    for (int s = 0; s < 2; ++s) cyl_z(pm, 0.0f, 0.05f, (s ? 1.06f : -1.06f) * W, 0.17f * Hk, 0.085f, 12);
    PmProf u[6] = { { 0, 0.0f, 0 }, { 0.06f, 0.0f, 1 }, { 0.05f, 0.04f, 0 }, { 0.08f, 0.12f, 0 }, { 0.03f, 0.2f, 0 }, { 0, 0.24f, 0 } };
    pm_push(pm); pm_translate(pm, 0.0f, 0.95f * Hk - 0.01f, -0.06f); pm_lathe(pm, u, 6, 10, 0, 0); pm_pop(pm);
}
/* Swing half doors (pintu pagar, the shophouse's panel 1): two leaves before the main door, a solid panel below, upright
 * balusters above, a carved top rising to the outer stiles. */
void m_h_pintu_pagar(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float y0 = HK_PAGAR_Y, H = HK_PAGAR_H, y1 = y0 + H, ym = y0 + 0.55f;
    for (int s = 0; s < 2; ++s) {
        const float a = s ? 0.006f : -0.5f * HK_PAGAR_W, b = s ? 0.5f * HK_PAGAR_W : -0.006f;
        const float xo = s ? b : a;   /* the outer stile's edge */
        rect_frame(pm, a, y0, b, y1, 0.055f, 0.09f, 0.06f, 0.0f, 0.045f, 0.003f);
        bx(pm, a + 0.055f, ym - 0.04f, 0.0f, b - 0.055f, ym + 0.04f, 0.045f, 0.003f);
        bx(pm, a + 0.055f, y0 + 0.09f, 0.008f, b - 0.055f, ym - 0.04f, 0.03f, 0.0f);   /* the panel */
        if (L0) bx(pm, a + 0.1f, y0 + 0.14f, 0.03f, b - 0.1f, ym - 0.09f, 0.04f, 0.006f);
        if (!L2) {
            const int nb = L0 ? 5 : 3;
            for (int k = 1; k <= nb; ++k) {
                const float x = a + 0.055f + (b - a - 0.11f) * (float)k / (float)(nb + 1);
                bx(pm, x - 0.014f, ym + 0.04f, 0.01f, x + 0.014f, y1 - 0.06f, 0.035f, 0.0f);
            }
        }
        /* the carved top: a curve from the meeting stile up to the outer one */
        float c[12][2];
        int n = 0;
        const float xi = s ? a : b;
        c[n][0] = xi; c[n][1] = y1; n++;
        const int k1 = L2 ? 2 : 8;
        for (int k = 0; k <= k1; ++k) {
            const float t = (float)k / (float)k1, x = xi + (xo - xi) * t;
            c[n][0] = x; c[n][1] = y1 + 0.03f + 0.13f * t * t + 0.02f * sinf(t * 3.0f * PI_F) * (1.0f - t); n++;
        }
        c[n][0] = xo; c[n][1] = y1; n++;
        run_z(pm, (const float (*)[2])c, n, 0.0f, 0.04f);
    }
}
/* A canvas awning (the shophouse's and the self-built's shops; x 0..1, whole metres): canvas from the wall out and
 * down, a scalloped valance, the front bar, a folding arm. */
void m_h_awning(Pm *pm) { quiet(pm);
    const float D = HK_AWNING_D, F = HK_AWNING_DROP, len = sqrtf(D * D + F * F);
    pm_mat(pm, PMAT_HCANVAS);
    const float s[4][2] = { { 0.0f, 0.0f }, { D, -F }, { D, -F - 0.012f }, { 0.0f, -0.012f } };
    (void)len;
    run_x(pm, s, 4, 0.0f, 1.0f);
    float v[16][2];
    int n = 0;
    v[n][0] = 0.0f; v[n][1] = -F; n++;
    v[n][0] = 1.0f; v[n][1] = -F; n++;
    if (L0) {
        for (int k = 4; k >= 0; --k) {   /* four scallops, right to left */
            v[n][0] = 0.25f * (float)k; v[n][1] = -F - 0.2f; n++;
            if (k) { v[n][0] = 0.25f * (float)k - 0.125f; v[n][1] = -F - 0.25f; n++; }
        }
    } else { v[n][0] = 1.0f; v[n][1] = -F - 0.22f; n++; v[n][0] = 0.0f; v[n][1] = -F - 0.22f; n++; }
    run_z(pm, (const float (*)[2])v, n, D - 0.006f, D + 0.006f);
    pm_mat(pm, PMAT_HGALV);
    cyl_x(pm, 0.0f, 1.0f, -F - 0.015f, D - 0.02f, 0.02f, 8);
    if (L2) return;
    const float a[3] = { 0.5f, -0.95f, 0.02f }, b[3] = { 0.5f, -F - 0.03f, D - 0.03f };
    pm_beam(pm, a, b, 0.03f, 0.02f, 0.0f, NULL);
    bx(pm, 0.47f, -1.0f, 0.0f, 0.53f, -0.88f, 0.04f, 0.0f);
}

/* ============================================ the link house (HM5b1a) === */
/* The car porch's canopy (the Link House sheet's panel 1; x 0..1, bays of about 1.5 m): a curved steel rib at x 0
 * springing from the wall's plate, frosted sheet over it, the front's edge beam. Built at HK_CARPORT_D deep. */
static float carport_y(float t) { return HK_CARPORT_RISE * sinf(PI_F * 0.8f * t); }
void m_h_carport(Pm *pm) { quiet(pm);
    const float D = HK_CARPORT_D;
    const int n = L0 ? 24 : L1 ? 12 : 6;
    float band[64][2];
    int m = 0;
    for (int k = 0; k <= n; ++k) { const float t = (float)k / (float)n; band[m][0] = D * t; band[m][1] = carport_y(t) + 0.012f; m++; }
    for (int k = n; k >= 0; --k) { const float t = (float)k / (float)n; band[m][0] = D * t; band[m][1] = carport_y(t); m++; }
    pm_mat(pm, PMAT_HFROST);
    run_x(pm, (const float (*)[2])band, m, 0.0f, 1.0f);
    m = 0;
    for (int k = 0; k <= n; ++k) { const float t = (float)k / (float)n; band[m][0] = D * t; band[m][1] = carport_y(t) - 0.002f; m++; }
    for (int k = n; k >= 0; --k) { const float t = (float)k / (float)n; band[m][0] = D * t; band[m][1] = carport_y(t) - 0.11f; m++; }
    pm_mat(pm, PMAT_HGALV);
    run_x(pm, (const float (*)[2])band, m, -0.025f, 0.025f);
    const float yf = carport_y(1.0f);
    bx(pm, 0.0f, yf - 0.14f, D - 0.08f, 1.0f, yf, D, 0.0f);
    bx(pm, 0.0f, -0.1f, 0.0f, 1.0f, 0.0f, 0.08f, 0.0f);   /* the wall plate */
}
/* A gable's vent (the link house's panels 1, 5): a moulded ring, louvres across it, dark behind. */
void m_h_gable_vent(Pm *pm) { quiet(pm);
    const float R = HK_VENT_R;
    pm_mat(pm, PMAT_HTIMBER);
    cyl_z(pm, 0.0f, 0.006f, 0.0f, 0.0f, R - 0.02f, 24);
    pm_mat(pm, PMAT_HTRIM);
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.02f); pm_rotate_x(pm, HALF_PI);
    pm_torus(pm, R, 0.035f, 24, 6);
    pm_pop(pm);
    if (L2) return;
    const float pitch = L0 ? 0.06f : 0.12f;
    for (float y = -R + 0.06f; y <= R - 0.06f; y += pitch) {
        const float hw = sqrtf(R * R - y * y) - 0.02f;
        const float s[4][2] = { { 0.006f, y - 0.02f }, { 0.016f, y - 0.02f }, { 0.04f, y + 0.02f }, { 0.03f, y + 0.02f } };
        run_x(pm, s, 4, -hw, hw);
    }
}
/* A window hood (the link house's panel 1: small aluminium hoods over the upper windows; x 0..1): a ribbed plate out
 * from the wall, falling; its cheeks; a lip along its front. */
void m_h_win_hood(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    const float D = HK_HOOD_D, F = 0.16f;
    const float s[4][2] = { { 0.0f, 0.0f }, { D, -F }, { D, -F - 0.012f }, { 0.0f, -0.012f } };
    run_x(pm, s, 4, 0.0f, 1.0f);
    const float c[4][2] = { { 0.0f, 0.0f }, { D, -F }, { D, -F - 0.04f }, { 0.0f, -0.36f } };
    run_x(pm, c, 4, 0.0f, 0.012f);
    run_x(pm, c, 4, 0.988f, 1.0f);
    bx(pm, 0.0f, -F - 0.05f, D - 0.012f, 1.0f, -F, D + 0.006f, 0.0f);
    if (L2) return;
    const int nr = L0 ? 6 : 3;
    for (int k = 1; k < nr; ++k) {
        const float x = (float)k / (float)nr;
        const float ye = -F * (D - 0.02f) / D;   /* the rib along the plate's top, down the fall */
        const float r[4][2] = { { 0.0f, 0.0f }, { D - 0.02f, ye }, { D - 0.02f, ye + 0.018f }, { 0.0f, 0.018f } };
        run_x(pm, r, 4, x - 0.012f, x + 0.012f);
    }
}

/* ===================================== HM5b1b: South Asia, the Middle East and Africa ===================================== */
/* a ring in the wall's plane (its axis along z) round (x, y) at z */
static void ring_xy(Pm *pm, float x, float y, float z, float R, float r, int s, int ts) {
    pm_push(pm); pm_translate(pm, x, y, z); pm_rotate_x(pm, HALF_PI); pm_torus(pm, R, r, s, ts); pm_pop(pm);
}
/* an eight-pointed star of bars (two squares, one turned 45 degrees) round (cx, cy), its points R out */
static void star8(Pm *pm, float cx, float cy, float R, float w, float z0, float z1) {
    for (int sq = 0; sq < 2; ++sq)
        for (int k = 0; k < 4; ++k) {
            const float a0 = (sq ? 0.0f : 0.25f * PI_F) + HALF_PI * (float)k, a1 = a0 + HALF_PI;
            bar_xy(pm, cx + R * cosf(a0), cy + R * sinf(a0), cx + R * cosf(a1), cy + R * sinf(a1), w, z0, z1);
        }
}

/* ------------------------------------------------- the pointed arch (shared) --- */
/* Two-centred: each side an arc round a centre HK_ARCH_E across the middle at the springing, through its jamb. */
float hk_arch_y(float x, float grow) {
    const float r = 0.5f * HK_ARCH_W + HK_ARCH_E + grow, ax = fabsf(x) + HK_ARCH_E, q = r * r - ax * ax;
    return HK_ARCH_SPRING + (q > 0.0f ? sqrtf(q) : 0.0f);
}
float hk_arch_apex(float grow) { return hk_arch_y(0.0f, grow); }
/* the left side's arc grown by `grow`, from the springing (t 0) to the apex (t 1) */
static void arch_pt(float grow, float t, float *x, float *y) {
    const float r = 0.5f * HK_ARCH_W + HK_ARCH_E + grow, ta = acosf(HK_ARCH_E / r) * t;
    *x = fminf(HK_ARCH_E - r * cosf(ta), 0.0f);
    *y = HK_ARCH_SPRING + r * sinf(ta);
}

/* ================================================================== RCC (HM5b1b) === */
/* An ornamental window grille (the RCC sheet's panels 1, 3, 5: patterned bars, not straight ones; x -0.5..0.5, y 0..1
 * stretched over the opening): a flat frame, uprights, two flats across; lozenges in the middle band, rings in the
 * bottom one, a sunburst over the top. */
void m_h_grille_orn(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    rect_frame(pm, -0.5f, 0.0f, 0.5f, 1.0f, 0.025f, 0.025f, 0.025f, 0.0f, 0.014f, 0.0f);
    for (int k = 0; k < 2; ++k) { const float y = k ? 0.7f : 0.3f; bx(pm, -0.48f, y - 0.012f, 0.004f, 0.48f, y + 0.012f, 0.022f, 0.0f); }
    const int nu = L2 ? 3 : 6;
    for (int k = 1; k < nu; ++k) { const float x = -0.5f + (float)k / (float)nu; bx(pm, x - 0.007f, 0.025f, 0.006f, x + 0.007f, 0.975f, 0.02f, 0.0f); }
    if (L2) return;
    const float cw = 1.0f / 6.0f, w = 0.012f;
    for (int k = 0; k < 6; ++k) {   /* lozenges, one a bay */
        const float cx = -0.5f + cw * ((float)k + 0.5f), hx = 0.5f * cw - 0.018f, cy = 0.5f, hy = 0.17f;
        bar_xy(pm, cx - hx, cy, cx, cy + hy, w, 0.006f, 0.02f);
        bar_xy(pm, cx, cy + hy, cx + hx, cy, w, 0.006f, 0.02f);
        bar_xy(pm, cx + hx, cy, cx, cy - hy, w, 0.006f, 0.02f);
        bar_xy(pm, cx, cy - hy, cx - hx, cy, w, 0.006f, 0.02f);
        if (L0) ring_xy(pm, cx, 0.15f, 0.013f, 0.05f, 0.007f, 10, 4);
    }
    /* the sunburst: rays from a hub on the upper flat, an arc round them */
    const float hy0 = 0.7f, R = 0.27f, r0 = 0.06f;
    for (int k = 1; k < 8; ++k) { const float a = PI_F * (float)k / 8.0f; bar_xy(pm, r0 * cosf(a), hy0 + r0 * sinf(a), R * cosf(a), hy0 + R * sinf(a), w, 0.006f, 0.02f); }
    const int na = L0 ? 8 : 4;
    for (int k = 0; k < na; ++k) {
        const float a0 = PI_F * (float)k / (float)na, a1 = PI_F * (float)(k + 1) / (float)na;
        bar_xy(pm, R * cosf(a0), hy0 + R * sinf(a0), R * cosf(a1), hy0 + R * sinf(a1), w, 0.006f, 0.02f);
        if (L0 && (k & 1) == 0) {
            const float b0 = PI_F * (float)k / (float)na, b1 = PI_F * (float)(k + 2) / (float)na;
            bar_xy(pm, r0 * cosf(b0), hy0 + r0 * sinf(b0), r0 * cosf(b1), hy0 + r0 * sinf(b1), w, 0.006f, 0.02f);
        }
    }
}
/* A precast pierced balustrade (RCC 4; the bungalow's 2, 5; x 0..1, whole metres): a plinth, two cast panels a metre
 * between piers, each a ring tied to its corners and its rails, a boss in the ring; a coping over them. */
void m_h_balustrade_pc(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HBREEZE);
    const float Y1 = HK_PCB_H - 0.1f, y0 = 0.12f;
    bx(pm, 0.0f, 0.0f, -0.08f, 1.0f, y0, 0.08f, 0.008f);
    bx(pm, 0.0f, Y1, -0.1f, 1.0f, HK_PCB_H, 0.1f, 0.01f);
    for (int p = 0; p < 2; ++p) { const float x = p ? 0.5f : 0.0f; bx(pm, x, y0, -0.07f, x + 0.06f, Y1, 0.07f, 0.006f); }
    for (int b = 0; b < 2; ++b) {
        const float x0 = b ? 0.56f : 0.06f, x1 = b ? 1.0f : 0.5f, cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + Y1), t = 0.04f, w = 0.04f;
        rect_frame(pm, x0, y0, x1, Y1, t, t, t, -0.04f, 0.04f, 0.0f);
        const float ix0 = x0 + t, ix1 = x1 - t, iy0 = y0 + t, iy1 = Y1 - t, R = 0.15f, d = 0.7071f * R;
        if (L2) { bar_xy(pm, ix0, iy0, ix1, iy1, w, -0.035f, 0.035f); bar_xy(pm, ix0, iy1, ix1, iy0, w, -0.035f, 0.035f); continue; }
        ring_xy(pm, cx, cy, 0.0f, R, 0.03f, 12, 4);
        bar_xy(pm, cx - d, cy - d, ix0, iy0, w, -0.035f, 0.035f);
        bar_xy(pm, cx + d, cy - d, ix1, iy0, w, -0.035f, 0.035f);
        bar_xy(pm, cx - d, cy + d, ix0, iy1, w, -0.035f, 0.035f);
        bar_xy(pm, cx + d, cy + d, ix1, iy1, w, -0.035f, 0.035f);
        bx(pm, cx - 0.02f, cy + R, -0.035f, cx + 0.02f, iy1, 0.035f, 0.0f);
        bx(pm, cx - 0.02f, iy0, -0.035f, cx + 0.02f, cy - R, 0.035f, 0.0f);
        if (L0) cyl_z(pm, -0.04f, 0.04f, cx, cy, 0.05f, 10);
    }
}
/* A corrugated tin awning (RCC 1's neighbours, the block's veranda lean-tos; x 0..1, whole metres): the sheet from a
 * timber batten on the wall out and down, its corrugations' crests down the fall, a turned-down front edge; a steel
 * rafter under it and a strut back to the wall. */
void m_h_awning_tin(Pm *pm) { quiet(pm);
    const float D = HK_TIN_D, F = HK_TIN_DROP;
    pm_mat(pm, PMAT_HTIN);
    const float s[4][2] = { { 0.0f, 0.0f }, { D, -F }, { D, -F - 0.004f }, { 0.0f, -0.004f } };
    run_x(pm, s, 4, 0.0f, 1.0f);
    bx(pm, 0.0f, -F - 0.05f, D - 0.004f, 1.0f, -F, D + 0.002f, 0.0f);
    if (!L2) {
        const int nr = L0 ? 10 : 5;
        for (int k = 0; k < nr; ++k) {
            const float x = ((float)k + 0.5f) / (float)nr;
            const float a[3] = { x, 0.004f, 0.0f }, b[3] = { x, -F + 0.004f, D };
            pm_beam(pm, a, b, 0.035f, 0.01f, 0.0f, NULL);
        }
    }
    pm_mat(pm, PMAT_HTIMBER);
    bx(pm, 0.0f, -0.08f, 0.0f, 1.0f, -0.004f, 0.06f, 0.0f);
    pm_mat(pm, PMAT_HGALV);
    {
        const float a[3] = { 0.5f, -0.03f, 0.0f }, b[3] = { 0.5f, -F - 0.03f, D - 0.02f };
        pm_beam(pm, a, b, 0.04f, 0.05f, 0.0f, NULL);
    }
    if (L2) return;
    const float a[3] = { 0.5f, -0.80f, 0.02f }, b[3] = { 0.5f, -0.6f * F - 0.05f, 0.6f * D };
    pm_beam(pm, a, b, 0.03f, 0.03f, 0.0f, NULL);
    bx(pm, 0.46f, -0.86f, 0.0f, 0.54f, -0.74f, 0.012f, 0.0f);
}

/* ============================================================= the bungalow (HM5b1b) === */
/* A fretwork frieze under a veranda's eaves (the Bungalow sheet's panel 1; x 0..1, four arches a metre): a rail under
 * the beam, scalloped arches hanging from it, a turned drop where they meet, a ring in each arch. */
void m_h_frieze(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float H = HK_FRIEZE_H, yr = -0.09f, dip = 0.21f, w = 0.025f;
    bx(pm, 0.0f, yr, -0.015f, 1.0f, 0.0f, 0.015f, 0.0f);
    const int ns = L0 ? 8 : L1 ? 4 : 2;
    for (int c = 0; c < 4; ++c) {
        const float x0 = 0.25f * (float)c;
        for (int k = 0; k < ns; ++k) {
            const float t0 = (float)k / (float)ns, t1 = (float)(k + 1) / (float)ns;
            bar_xy(pm, x0 + 0.25f * t0, yr - dip * sinf(PI_F * t0), x0 + 0.25f * t1, yr - dip * sinf(PI_F * t1), w, -0.012f, 0.012f);
        }
        if (L2) continue;
        const float cx = x0 + 0.125f;
        ring_xy(pm, cx, yr - 0.08f, 0.0f, 0.035f, 0.008f, 10, 4);
        if (L0) bx(pm, cx - 0.008f, yr - 0.045f, -0.008f, cx + 0.008f, yr, 0.008f, 0.0f);
        /* the drop where two arches meet */
        PmProf d[7] = { { 0, -H, 0 }, { 0.014f, -H + 0.015f, 0 }, { 0.026f, -H + 0.05f, 0 }, { 0.012f, -H + 0.09f, 0 },
                        { 0.012f, yr - 0.03f, 0 }, { 0.02f, yr - 0.01f, 0 }, { 0, yr, 0 } };
        pm_push(pm); pm_translate(pm, x0, 0.0f, 0.0f); pm_lathe(pm, d, 7, segs(pm, 8, 4), 0, 0); pm_pop(pm);
    }
}
/* The Art Deco parapet's pediment and roundel (the Bungalow sheet's panel 5): stepped shoulders, a segmental head, a
 * raised border; the roundel's ring, spokes and boss; speed lines on the shoulders. */
void m_h_deco_roundel(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float W = 0.5f * HK_DECO_W, H = HK_DECO_H;
    /* the half outline, right side, from the shoulder's foot up to the head's crown */
    float half[16][2];
    int n = 0;
    half[n][0] = W; half[n][1] = 0.0f; n++;
    half[n][0] = W; half[n][1] = 0.26f * H; n++;
    half[n][0] = 0.81f * W; half[n][1] = 0.26f * H; n++;
    half[n][0] = 0.81f * W; half[n][1] = 0.44f * H; n++;
    half[n][0] = 0.58f * W; half[n][1] = 0.44f * H; n++;
    const int na = L2 ? 3 : 8;
    for (int k = 1; k <= na; ++k) { const float t = (float)k / (float)na, a = HALF_PI * t; half[n][0] = 0.58f * W * cosf(a); half[n][1] = 0.44f * H + 0.56f * H * sinf(a); n++; }
    float p[40][2];
    int m = 0;
    for (int i = 0; i < n; ++i) { p[m][0] = half[i][0]; p[m][1] = half[i][1]; m++; }
    for (int i = n - 2; i >= 0; --i) { p[m][0] = -half[i][0]; p[m][1] = half[i][1]; m++; }
    run_z(pm, (const float (*)[2])p, m, -0.15f, 0.0f);
    const float cy = 0.52f * H;
    if (!L2) {   /* the raised border round the head */
        float q[40][2];
        for (int i = 0; i < m; ++i) { q[i][0] = p[i][0] * 0.9f; q[i][1] = 0.05f + p[i][1] * 0.88f; }
        run_z(pm, (const float (*)[2])q, m, 0.0f, 0.02f);
    }
    cyl_z(pm, 0.0f, 0.04f, 0.0f, cy, 0.27f, 24);
    ring_xy(pm, 0.0f, cy, 0.045f, 0.27f, 0.028f, 24, 6);
    if (L2) return;
    ring_xy(pm, 0.0f, cy, 0.045f, 0.12f, 0.018f, 16, 4);
    cyl_z(pm, 0.04f, 0.08f, 0.0f, cy, 0.05f, 12);
    if (!L0) return;
    for (int k = 0; k < 8; ++k) { const float a = PI_F * 0.25f * (float)k; bar_xy(pm, 0.12f * cosf(a), cy + 0.12f * sinf(a), 0.25f * cosf(a), cy + 0.25f * sinf(a), 0.02f, 0.04f, 0.06f); }
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 3; ++k) {
            const float y = 0.06f * H + 0.05f * (float)k, x0 = 0.84f * W, x1 = 0.97f * W;
            bx(pm, s ? x0 : -x1, y, 0.0f, s ? x1 : -x0, y + 0.022f, 0.02f, 0.0f);
            const float y2 = 0.29f * H + 0.045f * (float)k, a0 = 0.62f * W, a1 = 0.78f * W;
            bx(pm, s ? a0 : -a1, y2, 0.0f, s ? a1 : -a0, y2 + 0.02f, 0.02f, 0.0f);
        }
}

/* ============================================================ the Middle East (HM5b1b) === */
/* A pointed arch's carved surround (the Middle Eastern Villa sheet's panels 1, 5; the Courtyard's 1, 2, 4, 5): jambs and
 * the arch's moulding in one band each side; carved spandrels inside a rectangular alfiz, a star on each; behind the
 * moulding, the corners between the arch and a square opening filled back into the wall. In the unit's stone. */
void m_h_arch_surround(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float hw = 0.5f * HK_ARCH_W, B = HK_ARCH_BAND, A = HK_ARCH_ALFIZ, ab = 0.14f;
    const float apI = hk_arch_apex(0.0f), apO = hk_arch_apex(B), Yt = apO + 0.30f;
    const int na = L0 ? 16 : L1 ? 9 : 5;
    for (int s = 0; s < 2; ++s) {
        const float sg = s ? -1.0f : 1.0f;
        float p[72][2];
        int n = 0;
        /* the band: up the jamb's outside, round the outer arc to the apex, back down the inner arc and the jamb */
        p[n][0] = -(hw + B); p[n][1] = 0.0f; n++;
        for (int k = 0; k <= na; ++k) { float x, y; arch_pt(B, (float)k / (float)na, &x, &y); p[n][0] = x; p[n][1] = y; n++; }
        for (int k = na; k >= 0; --k) { float x, y; arch_pt(0.0f, (float)k / (float)na, &x, &y); p[n][0] = x; p[n][1] = y; n++; }
        p[n][0] = -hw; p[n][1] = 0.0f; n++;
        for (int i = 0; i < n; ++i) p[i][0] *= sg;
        run_z(pm, (const float (*)[2])p, n, 0.0f, 0.06f);
        /* the square opening's corner filled to the arch */
        n = 0;
        for (int k = 0; k <= na; ++k) { float x, y; arch_pt(0.0f, (float)k / (float)na, &x, &y); p[n][0] = sg * x; p[n][1] = y; n++; }
        p[n][0] = sg * -hw; p[n][1] = apI; n++;
        run_z(pm, (const float (*)[2])p, n, -0.35f, 0.0f);
        /* the spandrel's carved field, from the band to the alfiz */
        n = 0;
        p[n][0] = -(A - ab); p[n][1] = HK_ARCH_SPRING; n++;
        for (int k = 0; k <= na; ++k) { float x, y; arch_pt(B, (float)k / (float)na, &x, &y); p[n][0] = x; p[n][1] = y; n++; }
        p[n][0] = 0.0f; p[n][1] = Yt - ab; n++;
        p[n][0] = -(A - ab); p[n][1] = Yt - ab; n++;
        for (int i = 0; i < n; ++i) p[i][0] *= sg;
        run_z(pm, (const float (*)[2])p, n, 0.0f, 0.025f);
        if (!L2) star8(pm, sg * -(A - ab - 0.26f), Yt - ab - 0.26f, 0.16f, 0.025f, 0.025f, 0.045f);
        if (L0) cyl_z(pm, 0.025f, 0.05f, sg * -(A - ab - 0.26f), Yt - ab - 0.26f, 0.045f, 10);
    }
    /* the alfiz: its sides down to the ground, its head */
    bx(pm, -A, 0.0f, 0.0f, -A + ab, Yt, 0.075f, L2 ? 0.0f : 0.01f);
    bx(pm, A - ab, 0.0f, 0.0f, A, Yt, 0.075f, L2 ? 0.0f : 0.01f);
    bx(pm, -A, Yt - ab, 0.0f, A, Yt, 0.075f, L2 ? 0.0f : 0.01f);
    if (!L2) bx(pm, -A - 0.04f, Yt, -0.02f, A + 0.04f, Yt + 0.06f, 0.1f, 0.01f);   /* a drip over it */
}
/* A girih screen (the Middle Eastern Villa's panels 1, 2, 3, 6; the Courtyard's 3, 6; x 0..HK_GIRIH, y 0..HK_GIRIH):
 * a frame, three by three eight-pointed stars, tied to their cells' edges and (L0) corners, pierced between. */
void m_h_girih(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float G = HK_GIRIH, f = 0.06f, cs = (G - 2.0f * f) / 3.0f, w = 0.024f, R = 0.15f;
    rect_frame(pm, 0.0f, 0.0f, G, G, f, f, f, -0.04f, 0.04f, L2 ? 0.0f : 0.006f);
    if (L2) {
        for (int k = 1; k < 3; ++k) {
            const float t = f + cs * (float)k;
            bx(pm, t - 0.5f * w, f, -0.03f, t + 0.5f * w, G - f, 0.03f, 0.0f);
            bx(pm, f, t - 0.5f * w, -0.03f, G - f, t + 0.5f * w, 0.03f, 0.0f);
        }
        return;
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const float cx = f + cs * ((float)i + 0.5f), cy = f + cs * ((float)j + 0.5f), h = 0.5f * cs;
            if (L0) star8(pm, cx, cy, R, w, -0.03f, 0.03f);
            else for (int k = 0; k < 4; ++k) {   /* L1: the star's upright square alone (a diamond), still tied */
                const float a0 = HALF_PI * (float)k, a1 = a0 + HALF_PI;
                bar_xy(pm, cx + R * cosf(a0), cy + R * sinf(a0), cx + R * cosf(a1), cy + R * sinf(a1), w, -0.03f, 0.03f);
            }
            bar_xy(pm, cx + R, cy, cx + h, cy, w, -0.03f, 0.03f);
            bar_xy(pm, cx - R, cy, cx - h, cy, w, -0.03f, 0.03f);
            bar_xy(pm, cx, cy + R, cx, cy + h, w, -0.03f, 0.03f);
            bar_xy(pm, cx, cy - R, cx, cy - h, w, -0.03f, 0.03f);
            if (!L0) continue;
            const float d = 0.7071f * R;
            for (int q = 0; q < 4; ++q) {
                const float sx = q & 1 ? 1.0f : -1.0f, sy = q & 2 ? 1.0f : -1.0f;
                bar_xy(pm, cx + sx * d, cy + sy * d, cx + sx * h, cy + sy * h, w, -0.03f, 0.03f);
            }
        }
}
/* A timber mashrabiya oriel (the Middle Eastern Villa's panel 4, the Courtyard's 2): its floor on carved corbels, a
 * cornice and a cap over it; solid aprons with fielded panels; lattice on its three faces in bays with arched heads,
 * the room's light through paper and glass behind (lit by night). */
void m_h_mashrabiya(Pm *pm) { quiet(pm);
    const float W = 0.5f * HK_MASH_W, H = HK_MASH_H, D = HK_MASH_D, ya = 0.5f, yh = H - 0.18f;
    pm_mat(pm, PMAT_HFROST);   /* the light behind the lattice */
    pm->part = 1;
    bx(pm, -W + 0.04f, ya, D - 0.09f, W - 0.04f, yh, D - 0.08f, 0.0f);
    for (int s = 0; s < 2; ++s) { const float x = s ? W - 0.09f : -W + 0.08f; bx(pm, x, ya, 0.04f, x + 0.01f, yh, D - 0.09f, 0.0f); }
    pm->part = 0;
    pm_mat(pm, PMAT_HTIMBER);
    bx(pm, -W - 0.05f, -0.12f, 0.0f, W + 0.05f, 0.0f, D + 0.05f, 0.01f);   /* the floor */
    bx(pm, -W - 0.1f, H, 0.0f, W + 0.1f, H + 0.1f, D + 0.1f, 0.01f);      /* the cornice */
    bx(pm, -W - 0.03f, H + 0.1f, 0.0f, W + 0.03f, H + 0.2f, D + 0.03f, 0.01f);
    for (int s = 0; s < 2; ++s) {   /* the front's corner posts; the aprons; rails over the aprons and under the head */
        const float x0 = s ? W - 0.07f : -W, x1 = x0 + 0.07f;
        bx(pm, x0, 0.0f, D - 0.07f, x1, H, D, 0.004f);
        const float sx0 = s ? W - 0.05f : -W, sx1 = sx0 + 0.05f;
        bx(pm, sx0, 0.0f, 0.0f, sx1, ya, D - 0.07f, 0.0f);
        bx(pm, sx0 - 0.005f, ya, 0.0f, sx1 + 0.005f, ya + 0.06f, D - 0.07f, 0.0f);
        bx(pm, sx0 - 0.005f, yh, 0.0f, sx1 + 0.005f, H, D - 0.07f, 0.0f);
    }
    bx(pm, -W + 0.07f, 0.0f, D - 0.05f, W - 0.07f, ya, D, 0.0f);
    bx(pm, -W + 0.07f, ya, D - 0.06f, W - 0.07f, ya + 0.06f, D + 0.005f, 0.003f);
    bx(pm, -W + 0.07f, yh, D - 0.06f, W - 0.07f, H, D + 0.005f, 0.003f);
    const float bw = (2.0f * W - 0.14f) / 3.0f;
    for (int b = 1; b < 3; ++b) { const float x = -W + 0.07f + bw * (float)b; bx(pm, x - 0.03f, ya, D - 0.06f, x + 0.03f, yh, D, 0.0f); }
    if (L0) for (int b = 0; b < 3; ++b) { const float x = -W + 0.07f + bw * (float)b; bx(pm, x + 0.08f, 0.08f, D, x + bw - 0.08f, ya - 0.08f, D + 0.02f, 0.006f); }
    /* the corbels under the floor */
    const float c[9][2] = { { 0.0f, -0.12f }, { D - 0.05f, -0.12f }, { D - 0.06f, -0.18f }, { D - 0.2f, -0.24f }, { 0.36f, -0.32f },
                            { 0.22f, -0.44f }, { 0.12f, -0.58f }, { 0.06f, -0.68f }, { 0.0f, -0.70f } };
    for (int k = 0; k < 3; ++k) { const float x = (float)(k - 1) * (W - 0.2f); run_x(pm, c, 9, x - 0.05f, x + 0.05f); }
    if (L2) return;
    /* the lattice: close bars both ways over each face, arched heads in the front's bays */
    const float sp = L0 ? 0.07f : 0.14f, bw2 = 0.009f, y0 = ya + 0.06f, y1 = yh;
    for (float x = -W + 0.07f + 0.5f * sp; x < W - 0.07f; x += sp) bx(pm, x - bw2, y0, D - 0.04f, x + bw2, y1, D - 0.02f, 0.0f);
    for (float y = y0 + 0.5f * sp; y < y1; y += sp) bx(pm, -W + 0.07f, y - bw2, D - 0.04f, W - 0.07f, y + bw2, D - 0.02f, 0.0f);
    for (int s = 0; s < 2; ++s) {
        const float xa = s ? W - 0.035f : -W + 0.015f, xb = xa + 0.02f;
        for (float z = 0.5f * sp; z < D - 0.07f; z += sp) bx(pm, xa, y0, z - bw2, xb, y1, z + bw2, 0.0f);
        for (float y = y0 + 0.5f * sp; y < y1; y += sp) bx(pm, xa, y - bw2, 0.0f, xb, y + bw2, D - 0.07f, 0.0f);
    }
    if (!L0) return;
    for (int b = 0; b < 3; ++b) {
        const float cx = -W + 0.07f + bw * ((float)b + 0.5f), r = 0.5f * bw - 0.05f, cy = y1 - 0.06f - r;
        for (int k = 0; k < 6; ++k) {
            const float a0 = PI_F * (float)k / 6.0f, a1 = PI_F * (float)(k + 1) / 6.0f;
            bar_xy(pm, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), 0.035f, D - 0.05f, D - 0.005f);
        }
    }
}
/* A wind tower (the Middle Eastern Villa's panel 5): a solid shaft off the roof, then tall slots between corner piers
 * and fins on all four faces, crossed walls inside to catch the wind from any side; a lintel band, a cornice slab,
 * stubs at its corners. In the unit's stone. */
void m_h_wind_tower(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float h = 0.5f * HK_WIND_B, H = HK_WIND_H, yb = 1.3f, yl = H - 0.32f, p = 0.24f;
    bx(pm, -h, 0.0f, -h, h, yb, h, 0.01f);
    bx(pm, -h - 0.04f, yb - 0.05f, -h - 0.04f, h + 0.04f, yb + 0.02f, h + 0.04f, 0.0f);
    for (int k = 0; k < 4; ++k) {
        const float x = k & 1 ? h - p : -h, z = k & 2 ? h - p : -h;
        bx(pm, x, yb, z, x + p, yl, z + p, 0.0f);
    }
    for (int f = -1; f <= 1; f += 2) {
        const float x = 0.19f * (float)f;
        bx(pm, x - 0.04f, yb, h - 0.2f, x + 0.04f, yl, h, 0.0f);
        bx(pm, x - 0.04f, yb, -h, x + 0.04f, yl, -h + 0.2f, 0.0f);
        bx(pm, h - 0.2f, yb, x - 0.04f, h, yl, x + 0.04f, 0.0f);
        bx(pm, -h, yb, x - 0.04f, -h + 0.2f, yl, x + 0.04f, 0.0f);
    }
    for (int d = 0; d < 2; ++d) {   /* the crossed walls inside */
        pm_push(pm); pm_rotate_y(pm, d ? 0.25f * PI_F : -0.25f * PI_F);
        bx(pm, -1.3f * h, yb, -0.03f, 1.3f * h, yl, 0.03f, 0.0f);
        pm_pop(pm);
    }
    bx(pm, -h, yl, -h, h, H - 0.14f, h, 0.0f);
    bx(pm, -h - 0.08f, H - 0.14f, -h - 0.08f, h + 0.08f, H, h + 0.08f, L2 ? 0.0f : 0.01f);
    if (!L0) return;
    for (int k = 0; k < 4; ++k) {
        const float x = k & 1 ? h - 0.16f : -h, z = k & 2 ? h - 0.16f : -h;
        bx(pm, x, H, z, x + 0.16f, H + 0.2f, z + 0.16f, 0.005f);
    }
}
/* The great studded door (the Courtyard sheet's panels 1-6, the villas'): two timber leaves with cover battens at the
 * boards' joints, iron straps across them studded along their length, a ring pull on a plate on each leaf. Square to
 * HK_STUD_H, or to the pointed arch (the leaves' heads cut to it, the straps stopping at it). */
static void studded(Pm *pm, int arch) {
    const float hw = 0.5f * HK_ARCH_W, gap = 0.005f, z1 = 0.08f;
    const float top = arch ? hk_arch_apex(0.0f) : HK_STUD_H;
    const int na = L0 ? 14 : L1 ? 8 : 4;
    pm_mat(pm, PMAT_HTIMBER);
    for (int s = 0; s < 2; ++s) {
        const float sg = s ? 1.0f : -1.0f;
        float p[40][2];
        int n = 0;
        p[n][0] = -hw; p[n][1] = 0.0f; n++;
        p[n][0] = -gap; p[n][1] = 0.0f; n++;
        if (arch) {
            for (int k = na; k >= 0; --k) { float x, y; arch_pt(0.0f, (float)k / (float)na, &x, &y); p[n][0] = fminf(x, -gap); p[n][1] = y; n++; }
        } else { p[n][0] = -gap; p[n][1] = top; n++; p[n][0] = -hw; p[n][1] = top; n++; }
        for (int i = 0; i < n; ++i) p[i][0] *= sg;
        run_z(pm, (const float (*)[2])p, n, 0.0f, z1);
    }
    /* the leaf's height at x (under the arch, or square) */
#define LEAF_Y(x) (arch ? hk_arch_y((x), 0.0f) : top)
    if (L0) {
        pm_mat(pm, PMAT_HTIMBER);
        for (int k = 1; k < 8; ++k) {
            if (k == 4) continue;
            const float x = -hw + HK_ARCH_W * (float)k / 8.0f;
            bx(pm, x - 0.012f, 0.0f, z1, x + 0.012f, LEAF_Y(x) - 0.04f, z1 + 0.01f, 0.0f);
        }
    }
    pm_mat(pm, PMAT_HIRON);
    const int ns = 4;
    for (int k = 0; k < ns; ++k) {
        const float y = 0.35f + (top - 0.75f) * (float)k / (float)(ns - 1);
        float ext = hw - 0.03f;   /* the strap's reach either side of the middle at its height */
        if (arch && y + 0.05f > HK_ARCH_SPRING) {
            const float r = hw + HK_ARCH_E, dy = y + 0.05f - HK_ARCH_SPRING, q = r * r - dy * dy;
            ext = (q > 0.0f ? sqrtf(q) : 0.0f) - HK_ARCH_E - 0.05f;
        }
        if (ext < 0.1f) continue;
        for (int s = 0; s < 2; ++s) {
            const float a = s ? gap : -ext, b = s ? ext : -gap;
            bx(pm, a, y - 0.035f, z1, b, y + 0.035f, z1 + 0.008f, 0.0f);
            if (L2) continue;
            const float step = L0 ? 0.13f : 0.26f;
            for (float x = a + 0.06f; x < b - 0.03f; x += step) bx(pm, x - 0.016f, y - 0.016f, z1 + 0.008f, x + 0.016f, y + 0.016f, z1 + 0.03f, 0.0f);
        }
    }
#undef LEAF_Y
    for (int s = 0; s < 2; ++s) {
        const float x = s ? 0.16f : -0.16f;
        cyl_z(pm, z1, z1 + 0.012f, x, 1.5f, 0.06f, 12);
        if (L2) continue;
        ring_xy(pm, x, 1.43f, z1 + 0.03f, 0.07f, 0.009f, 14, 6);
    }
}
void m_h_door_studded(Pm *pm) { quiet(pm); studded(pm, 0); }
void m_h_door_studded_arch(Pm *pm) { quiet(pm); studded(pm, 1); }
/* A small timber lattice grille over a window (the Courtyard's panels 1, 2, 4; x -0.5..0.5, y 0..1 stretched): a
 * frame, diagonal bars both ways. */
void m_h_grille_lattice(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    rect_frame(pm, -0.5f, 0.0f, 0.5f, 1.0f, 0.07f, 0.07f, 0.07f, 0.0f, 0.05f, L2 ? 0.0f : 0.004f);
    const float X0 = -0.43f, X1 = 0.43f, Y0 = 0.07f, Y1 = 0.93f, d = L0 ? 0.12f : L1 ? 0.24f : 0.43f, w = 0.022f;
    for (int fam = 0; fam < 2; ++fam)
        for (float c = (fam ? X0 - Y1 : X0 + Y0) + 0.5f * d; c < (fam ? X1 - Y0 : X1 + Y1); c += d) {
            float xa, xb;
            if (fam == 0) { xa = fmaxf(X0, c - Y1); xb = fminf(X1, c - Y0); }   /* x + y = c */
            else          { xa = fmaxf(X0, c + Y0); xb = fminf(X1, c + Y1); }   /* x - y = c */
            if (xb - xa < 0.02f) continue;
            const float ya = fam ? xa - c : c - xa, yb = fam ? xb - c : c - xb;
            bar_xy(pm, xa, ya, xb, yb, w, 0.012f, 0.04f);
        }
}
/* A waterspout through a parapet (the Courtyard's panel 4): an open channel out from the wall, falling 12 degrees, a
 * wedge of corbel under its root. In the unit's stone. */
void m_h_waterspout(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float u[8][2] = { { -0.1f, -0.04f }, { 0.1f, -0.04f }, { 0.1f, 0.11f }, { 0.065f, 0.11f }, { 0.065f, 0.0f }, { -0.065f, 0.0f },
                            { -0.065f, 0.11f }, { -0.1f, 0.11f } };
    pm_push(pm); pm_rotate_x(pm, 12.0f * DEG);
    run_z(pm, u, 8, -0.15f, HK_SPOUT_L);
    pm_pop(pm);
    if (L2) return;
    const float w[3][2] = { { 0.0f, -0.04f }, { 0.32f, -0.04f - 0.32f * tanf(12.0f * DEG) }, { 0.0f, -0.3f } };
    run_x(pm, w, 3, -0.08f, 0.08f);
}

/* ================================================================ Africa (HM5b1b) === */
/* A glass louvre window (the Block sheet's panel 4): an aluminium frame and mullion, two banks of glass blades tilted
 * open, carried in side rails; a handle to work them. */
void m_h_win_jalousie(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HALU);
    const float hw = 0.5f * HK_JAL_W, H = HK_JAL_H, f = 0.04f;
    rect_frame(pm, -hw, 0.0f, hw, H, f, f, f, 0.0f, 0.06f, 0.0f);
    bx(pm, -0.02f, f, 0.0f, 0.02f, H - f, 0.06f, 0.0f);
    for (int b = 0; b < 2; ++b) {
        const float x0 = b ? 0.02f : -hw + f, x1 = b ? hw - f : -0.02f;
        if (!L2) { bx(pm, x0, f, 0.0f, x0 + 0.012f, H - f, 0.1f, 0.0f); bx(pm, x1 - 0.012f, f, 0.0f, x1, H - f, 0.1f, 0.0f); }
        pm_mat(pm, PMAT_HGLASS);
        if (L2) { bx(pm, x0, f, 0.02f, x1, H - f, 0.026f, 0.0f); pm_mat(pm, PMAT_HALU); continue; }
        for (float y = f + 0.01f; y + 0.045f < H - f; y += 0.1f) {
            const float s[4][2] = { { 0.0f, y }, { 0.1f, y + 0.035f }, { 0.1f, y + 0.041f }, { 0.0f, y + 0.006f } };
            run_x(pm, s, 4, x0 + 0.012f, x1 - 0.012f);
        }
        pm_mat(pm, PMAT_HALU);
    }
    if (L0) { bx(pm, -0.11f, 0.5f * H - 0.1f, 0.1f, -0.095f, 0.5f * H + 0.06f, 0.12f, 0.0f); bx(pm, 0.095f, 0.5f * H - 0.1f, 0.1f, 0.11f, 0.5f * H + 0.06f, 0.12f, 0.0f); }
}
/* A barred steel security door over a front door (the Block's panels 1-6, the compounds'): a tube frame, a sheet over
 * its lower part pressed with diamonds, rails, upright bars above with a band of lozenges, hinges and a lock box. */
void m_h_door_security(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    const float hw = 0.5f * HK_SEC_W, H = HK_SEC_H, t = 0.04f, yp = 0.9f, yr = 1.5f;
    rect_frame(pm, -hw, 0.0f, hw, H, t, t, t, 0.0f, 0.04f, 0.0f);
    bx(pm, -hw + t, 0.04f, 0.012f, hw - t, yp, 0.022f, 0.0f);
    bx(pm, -hw + t, yp - 0.02f, 0.0f, hw - t, yp + 0.02f, 0.04f, 0.0f);
    bx(pm, -hw + t, yr - 0.02f, 0.0f, hw - t, yr + 0.02f, 0.04f, 0.0f);
    const int nb = L2 ? 4 : 8;
    for (int k = 1; k < nb; ++k) { const float x = -hw + t + (2.0f * (hw - t)) * (float)k / (float)nb; bx(pm, x - 0.007f, yp + 0.02f, 0.013f, x + 0.007f, H - t, 0.027f, 0.0f); }
    bx(pm, hw - 0.17f, 0.96f, 0.04f, hw - 0.06f, 1.14f, 0.09f, 0.0f);   /* the lock box */
    if (L2) return;
    const float cw = 2.0f * (hw - t) / 4.0f;
    for (int k = 0; k < 4; ++k) {
        const float cx = -hw + t + cw * ((float)k + 0.5f), cy = 0.5f * (yp + yr), ax = 0.5f * cw - 0.01f, ay = 0.5f * (yr - yp) - 0.03f;
        bar_xy(pm, cx - ax, cy, cx, cy + ay, 0.012f, 0.013f, 0.03f);
        bar_xy(pm, cx, cy + ay, cx + ax, cy, 0.012f, 0.013f, 0.03f);
        bar_xy(pm, cx + ax, cy, cx, cy - ay, 0.012f, 0.013f, 0.03f);
        bar_xy(pm, cx, cy - ay, cx - ax, cy, 0.012f, 0.013f, 0.03f);
    }
    for (int k = 0; k < 3; ++k) { pm_push(pm); pm_translate(pm, -hw - 0.012f, 0.25f + 0.8f * (float)k, 0.02f); pm_cyl(pm, 0.0f, 0.12f, 0.014f, 8); pm_pop(pm); }
    if (!L0) return;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {   /* the sheet's pressed diamonds */
            const float cx = (i ? 0.5f : -0.5f) * (hw - t), cy = 0.25f + 0.4f * (float)j;
            pm_push(pm); pm_translate(pm, cx, cy, 0.022f); pm_rotate_z(pm, 0.25f * PI_F);
            bx(pm, -0.11f, -0.11f, 0.0f, 0.11f, 0.11f, 0.008f, 0.004f);
            pm_pop(pm);
        }
}
/* A CCTV camera on a wall or a gate's pier (the Walled Compound's panel 3): a round plate, an arm out from it, a
 * bullet housing tilted down with its sun shield, a dark lens behind glass. White plastic. */
void m_h_cctv(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HUPVC);
    const float L = HK_CCTV_L;
    cyl_z(pm, 0.0f, 0.015f, 0.0f, 0.0f, 0.06f, 14);
    bx(pm, -0.018f, -0.018f, 0.015f, 0.018f, 0.018f, L, 0.0f);
    pm_push(pm); pm_translate(pm, 0.0f, -0.03f, L); pm_rotate_x(pm, 25.0f * DEG);
    cyl_z(pm, -0.06f, 0.16f, 0.0f, 0.0f, 0.045f, 14);
    bx(pm, -0.056f, 0.045f, -0.07f, 0.056f, 0.055f, 0.2f, 0.0f);   /* the sun shield */
    if (!L2) { bx(pm, -0.056f, 0.0f, -0.07f, -0.05f, 0.05f, 0.2f, 0.0f); bx(pm, 0.05f, 0.0f, -0.07f, 0.056f, 0.05f, 0.2f, 0.0f); }
    pm_mat(pm, PMAT_HIRON);
    cyl_z(pm, 0.16f, 0.165f, 0.0f, 0.0f, 0.04f, 14);
    if (!L2) { pm_mat(pm, PMAT_HGLASS); cyl_z(pm, 0.165f, 0.168f, 0.0f, 0.0f, 0.03f, 12); }
    pm_pop(pm);
    pm_mat(pm, PMAT_HUPVC);
    bx(pm, -0.025f, -0.05f, L - 0.025f, 0.025f, 0.0f, L + 0.025f, 0.0f);   /* the knuckle */
}

/* ============================== HM5b1c: Southern Europe and Latin America ============================== */
/* a spiral (a C-scroll) in the plane z0..z1 round (cx, cy) from radius r0 at angle a0, turning `turns` times (signed:
 * + counter-clockwise) as it closes to r1, of bars w wide in n pieces */
static void scroll_xy(Pm *pm, float cx, float cy, float r0, float r1, float a0, float turns, int n, float w, float z0, float z1) {
    float px = cx + r0 * cosf(a0), py = cy + r0 * sinf(a0);
    for (int k = 1; k <= n; ++k) {
        const float t = (float)k / (float)n, a = a0 + 2.0f * PI_F * turns * t, r = r0 + (r1 - r0) * t;
        const float qx = cx + r * cosf(a), qy = cy + r * sinf(a);
        bar_xy(pm, px, py, qx, qy, w, z0, z1);
        px = qx; py = qy;
    }
}

/* ======================================================= Southern Europe (HM5b1c) === */
/* A stone architrave round a window (the Villa sheet's panel 1, the Townhouse's 1-6; x -HK_ARCHI_W/2..+, y 0..HK_ARCHI_H
 * the opening, stretched to it): a flat band up each jamb and over the head, eared at the head's corners (crossettes),
 * a bead round its inner edge, blocks under the jambs' feet. The cornice head (below) sits on its top. */
void m_h_architrave(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float hw = 0.5f * HK_ARCHI_W, H = HK_ARCHI_H, B = HK_ARCHI_B, ear = 0.07f, P = 0.05f, bv = L2 ? 0.0f : 0.006f;
    for (int s = 0; s < 2; ++s) {
        const float sg = s ? 1.0f : -1.0f, a = sg * hw, b = sg * (hw + B), e = sg * (hw + B + ear);
        bx(pm, fminf(a, b), -0.05f, 0.0f, fmaxf(a, b), H, P, bv);                    /* the jamb */
        bx(pm, fminf(b, e), H - 0.13f, 0.0f, fmaxf(b, e), H + B, P, bv);             /* the ear */
        if (!L2) bx(pm, fminf(a, b) - 0.01f, -0.09f, 0.0f, fmaxf(a, b) + 0.01f, -0.03f, P + 0.01f, bv);   /* the foot's block */
    }
    bx(pm, -hw - B, H, 0.0f, hw + B, H + B, P, bv);                                   /* the head */
    if (L2) return;
    /* the bead round the opening's edge */
    bx(pm, -hw - 0.035f, -0.03f, P, -hw, H + 0.035f, P + 0.015f, 0.0f);
    bx(pm, hw, -0.03f, P, hw + 0.035f, H + 0.035f, P + 0.015f, 0.0f);
    bx(pm, -hw, H, P, hw, H + 0.035f, P + 0.015f, 0.0f);
    if (!L0) return;
    /* a step along the head's top edge, a small keyblock in the head */
    bx(pm, -hw - B - ear, H + B - 0.03f, P, hw + B + ear, H + B, P + 0.012f, 0.0f);
    bx(pm, -0.07f, H - 0.02f, P, 0.07f, H + B, P + 0.025f, 0.004f);
}
/* A cornice head over a window or door (the Villa's panel 1, the Townhouse's, the Colonial's 2-6; x 0..1 stretched
 * along the head only): a frieze, a bed moulding, the corona and a cyma, its foot on the architrave's top at y 0. */
void m_h_cornice_head(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    if (L2) { const float s[4][2] = { { 0.0f, 0.0f }, { 0.14f, 0.2f }, { 0.14f, 0.3f }, { 0.0f, 0.3f } }; run_x(pm, s, 4, 0.0f, 1.0f); return; }
    const float s[11][2] = { { 0.0f, 0.0f }, { 0.04f, 0.0f }, { 0.04f, 0.16f }, { 0.06f, 0.17f }, { 0.08f, 0.2f }, { 0.15f, 0.215f },
                             { 0.16f, 0.225f }, { 0.16f, 0.265f }, { 0.12f, 0.28f }, { 0.05f, 0.31f }, { 0.0f, 0.31f } };
    run_x(pm, s, 11, 0.0f, 1.0f);
}
/* A round-arched stone surround for a door (the Villa's panels 1, 4; the Colonial's 1, 3; the Townhouse's 1; a half
 * circle over an opening HK_RARCH_W wide springing at HK_RARCH_SPRING, stretched to the shell's arch): plinth blocks,
 * jambs, impost blocks at the springing, voussoirs and a proud keystone. */
void m_h_arch_stone(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float R = 0.5f * HK_RARCH_W, S = HK_RARCH_SPRING, B = 0.2f, P = 0.06f, bv = L2 ? 0.0f : 0.008f;
    for (int s = 0; s < 2; ++s) {
        const float sg = s ? 1.0f : -1.0f, a = sg * R, b = sg * (R + B);
        bx(pm, fminf(a, b), 0.0f, 0.0f, fmaxf(a, b), S - 0.14f, P, bv);
        bx(pm, fminf(a, b) - 0.03f, S - 0.14f, 0.0f, fmaxf(a, b) + 0.03f, S, P + 0.03f, bv);   /* the impost */
        if (!L2) bx(pm, fminf(a, b) - 0.03f, 0.0f, 0.0f, fmaxf(a, b) + 0.03f, 0.32f, P + 0.02f, bv);   /* the plinth block */
        if (L0) for (int k = 1; k < 4; ++k) { const float y = (S - 0.14f) * (float)k / 4.0f; bx(pm, fminf(a, b), y - 0.008f, P, fmaxf(a, b), y + 0.008f, P + 0.004f, 0.0f); }   /* the jambs' courses */
    }
    const int nv = L0 ? 13 : L1 ? 9 : 5;   /* voussoirs (odd: the keystone the middle one) */
    const float gap = L2 ? 0.0f : 0.006f;
    for (int k = 0; k < nv; ++k) {
        if (k == nv / 2) continue;
        const float t0 = PI_F * (1.0f - (float)k / (float)nv) - gap, t1 = PI_F * (1.0f - (float)(k + 1) / (float)nv) + gap;
        const float q[4][2] = { { R * cosf(t0), S + R * sinf(t0) }, { R * cosf(t1), S + R * sinf(t1) },
                                { (R + B) * cosf(t1), S + (R + B) * sinf(t1) }, { (R + B) * cosf(t0), S + (R + B) * sinf(t0) } };
        run_z(pm, q, 4, 0.0f, P);
    }
    /* the keystone: wider at its top, standing proud and past the arch's back */
    const float kh = 0.5f * PI_F / (float)nv;
    const float kq[4][2] = { { -R * sinf(kh) - 0.01f, S + R - 0.04f }, { R * sinf(kh) + 0.01f, S + R - 0.04f },
                             { 0.14f, S + R + B + 0.1f }, { -0.14f, S + R + B + 0.1f } };
    run_z(pm, kq, 4, 0.0f, P + 0.05f);
    if (L0) bx(pm, -0.15f, S + R + B + 0.08f, 0.0f, 0.15f, S + R + B + 0.12f, P + 0.07f, 0.004f);
}
/* A timber pergola over a terrace (the Villa's panel 4, the Townhouse's 4; x 0..1, whole metres): a wall plate, three
 * rafters a metre out to HK_PERGOLA_D with shaped ends, the beam under them near the front (its posts, PMK_H_PERGOLA_POST,
 * stand under it HK_PERGOLA_D - 0.39 out), slats across the rafters' tops. Its rafters' top at y 0. */
void m_h_pergola(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float D = HK_PERGOLA_D, zb = D - 0.39f;
    bx(pm, 0.0f, -0.2f, 0.0f, 1.0f, -0.04f, 0.09f, 0.0f);   /* the wall plate */
    for (int k = 0; k < 3; ++k) {
        const float x = ((float)k + 0.5f) / 3.0f;
        if (L2) { bx(pm, x - 0.03f, -0.18f, 0.0f, x + 0.03f, 0.0f, D, 0.0f); continue; }
        const float s[6][2] = { { 0.0f, -0.18f }, { D - 0.22f, -0.18f }, { D - 0.1f, -0.14f }, { D, -0.07f }, { D, 0.0f }, { 0.0f, 0.0f } };
        run_x(pm, s, 6, x - 0.03f, x + 0.03f);
    }
    bx(pm, 0.0f, -0.4f, zb - 0.06f, 1.0f, -0.18f, zb + 0.06f, L2 ? 0.0f : 0.006f);   /* the beam */
    if (L2) return;
    const int ns = L0 ? 6 : 3;
    for (int k = 0; k < ns; ++k) { const float z = 0.35f + (D - 0.5f) * (float)k / (float)(ns - 1); bx(pm, 0.0f, 0.0f, z - 0.025f, 1.0f, 0.03f, z + 0.025f, 0.0f); }
}
/* A pergola's post (y 0..1 stretched to the beam's underside): a squared timber post on a steel shoe. */
void m_h_pergola_post(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    bx(pm, -0.065f, 0.012f, -0.065f, 0.065f, 1.0f, 0.065f, L2 ? 0.0f : 0.006f);
    pm_mat(pm, PMAT_HGALV);
    bx(pm, -0.08f, 0.0f, -0.08f, 0.08f, 0.012f, 0.08f, 0.0f);
    if (!L2) for (int s = 0; s < 2; ++s) { const float x = s ? 0.065f : -0.071f; bx(pm, x, 0.012f, -0.04f, x + 0.006f, 0.12f, 0.04f, 0.0f); }
}
/* A terracotta urn (the Villa's panels 2, 5; the Townhouse's by the doors): a foot, a swelling belly, a narrow neck and a
 * rolled lip, standing on y 0 centred, HK_URN_H tall; earth and a low plant in its mouth. */
void m_h_urn(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCLAY);
    const float H = HK_URN_H;
    const PmProf p[13] = { { 0.0f, 0.0f, 1 }, { 0.11f, 0.0f, 1 }, { 0.12f, 0.04f, 1 }, { 0.1f, 0.06f, 1 }, { 0.2f, 0.2f * H, 0 },
                           { 0.26f, 0.5f * H, 0 }, { 0.22f, 0.75f * H, 0 }, { 0.14f, 0.88f * H, 0 }, { 0.15f, 0.94f * H, 1 },
                           { 0.17f, H, 1 }, { 0.13f, H, 1 }, { 0.12f, H - 0.04f, 1 }, { 0.0f, H - 0.04f, 1 } };
    const int sg = segs(pm, 16, 6);
    if (L2) { const PmProf q[6] = { p[0], p[1], p[5], p[7], p[9], { 0.0f, H, 1 } }; pm_lathe(pm, q, 6, sg, 0.0f, 0.0f); return; }
    pm_lathe(pm, p, 13, sg, 0.0f, 0.0f);
    pm_mat(pm, PMAT_HPLANT);
    const PmProf e[5] = { { 0.0f, H - 0.03f, 1 }, { 0.115f, H - 0.03f, 1 }, { 0.11f, H + 0.05f, 0 }, { 0.06f, H + 0.11f, 0 }, { 0.0f, H + 0.12f, 1 } };
    pm_lathe(pm, e, 5, sg, 0.0f, 0.0f);
}
/* A tall timber double door under a transom light (the Townhouse's panels 1-6, the Colonial's 2, 4, 6; in an opening
 * HK_DDOOR_W x HK_DDOOR_H): a frame and transom bar; two leaves of fielded panels (a short one low, a tall one, a short
 * one high); the transom's glass behind an iron fan; knobs and a knocker. */
void m_h_door_transom(Pm *pm) { quiet(pm);
    const float hw = 0.5f * HK_DDOOR_W, H = HK_DDOOR_H, T = HK_TRANSOM_H, yl = H - T, f = 0.07f;
    pm_mat(pm, PMAT_HTRIM);
    rect_frame(pm, -hw, 0.0f, hw, H, f, 0.02f, f, 0.0f, 0.08f, L2 ? 0.0f : 0.004f);
    bx(pm, -hw + f, yl, 0.0f, hw - f, yl + 0.09f, 0.09f, L2 ? 0.0f : 0.004f);   /* the transom bar */
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, -hw + f, yl + 0.09f, 0.01f, hw - f, H - f, 0.016f, 0.0f);
    if (!L2) {   /* the fan: rays from the bar's middle, an arc across them */
        pm_mat(pm, PMAT_HIRON);
        const float fy = yl + 0.09f, top = T - 0.09f - f, fr = 0.6f * fminf(hw - f, top);
        const int nr = L0 ? 7 : 5;
        for (int k = 0; k < nr; ++k) {
            const float a = PI_F * ((float)k + 0.5f) / (float)nr, c = cosf(a), s = sinf(a);
            const float t = fminf((hw - f) / fmaxf(fabsf(c), 1e-3f), top / fmaxf(s, 1e-3f));   /* to the frame */
            bar_xy(pm, 0.08f * c, fy + 0.08f * s, t * c, fy + t * s, 0.014f, 0.02f, 0.034f);
        }
        const int na = L0 ? 10 : 6;
        for (int k = 0; k < na; ++k) {
            const float a0 = PI_F * (float)k / (float)na, a1 = PI_F * (float)(k + 1) / (float)na;
            bar_xy(pm, fr * cosf(a0), fy + fr * sinf(a0), fr * cosf(a1), fy + fr * sinf(a1), 0.014f, 0.02f, 0.034f);
        }
    }
    pm_mat(pm, PMAT_HDOOR);
    for (int s = 0; s < 2; ++s) {
        const float x0 = s ? 0.004f : -hw + f, x1 = s ? hw - f : -0.004f;
        bx(pm, x0, 0.02f, 0.0f, x1, yl, 0.05f, 0.0f);
        if (L2) continue;
        /* the stiles and rails proud of the panels, the panels fielded (L0) */
        const float st = 0.11f, ys[4] = { 0.02f, 0.62f, yl - 0.55f, yl };
        bx(pm, x0, 0.02f, 0.05f, x0 + st, yl, 0.07f, 0.0f);
        bx(pm, x1 - st, 0.02f, 0.05f, x1, yl, 0.07f, 0.0f);
        for (int r = 0; r < 4; ++r) {
            const float y0 = r == 0 ? ys[0] : ys[r] - 0.07f, y1 = r == 0 ? ys[0] + 0.18f : r == 3 ? ys[3] : ys[r] + 0.07f;
            bx(pm, x0 + st, y0, 0.05f, x1 - st, y1, 0.07f, 0.0f);
        }
        if (!L0) continue;
        for (int r = 0; r < 3; ++r) {
            const float y0 = (r == 0 ? ys[0] + 0.18f : ys[r] + 0.07f) + 0.04f, y1 = ys[r + 1] - 0.07f - 0.04f;
            bx(pm, x0 + st + 0.04f, y0, 0.05f, x1 - st - 0.04f, y1, 0.062f, 0.01f);
        }
    }
    pm_mat(pm, PMAT_HIRON);
    for (int s = 0; s < 2; ++s) cyl_z(pm, 0.07f, 0.1f, s ? 0.08f : -0.08f, 1.05f, 0.025f, 10);
    if (L0) { cyl_z(pm, 0.07f, 0.08f, 0.25f, 1.55f, 0.04f, 10); ring_xy(pm, 0.25f, 1.49f, 0.09f, 0.055f, 0.009f, 12, 4); }
}
/* A roller blind's box over a window (the Townhouse's panel 3; x 0..1 stretched to the opening and its guides): a
 * painted timber box on the wall over the head (y 0..0.22, out 0.2), a lip under its front. */
void m_h_blind_box(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, 0.0f, 0.0f, 0.0f, 1.0f, 0.22f, 0.2f, L2 ? 0.0f : 0.008f);
    if (!L2) bx(pm, 0.0f, -0.015f, 0.17f, 1.0f, 0.0f, 0.2f, 0.0f);
    if (L0) bx(pm, -0.01f, 0.2f, 0.0f, 1.01f, 0.235f, 0.215f, 0.0f);
}
/* The blind let down from its box (x 0..1, y -1..0, both stretched: the opening's width, how far it is down): timber
 * slats in one folded run, a heavier bottom bar, a guide channel each side; out from the wall (z 0.1..0.15, under the
 * box's middle). */
void m_h_blind(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const int n = L0 ? 20 : L1 ? 10 : 0;
    if (n) {
        float s[64][2];
        int m = 0;
        for (int k = 0; k < n; ++k) {
            const float y = -(float)k / (float)n;
            s[m][0] = 0.13f; s[m][1] = y; m++;
            s[m][0] = 0.15f; s[m][1] = y - 0.6f / (float)n; m++;
        }
        s[m][0] = 0.13f; s[m][1] = -1.0f; m++;
        s[m][0] = 0.1f; s[m][1] = -1.0f; m++;
        s[m][0] = 0.1f; s[m][1] = 0.0f; m++;
        run_x(pm, (const float (*)[2])s, m, 0.02f, 0.98f);
    } else bx(pm, 0.02f, -1.0f, 0.1f, 0.98f, 0.0f, 0.15f, 0.0f);
    bx(pm, 0.02f, -1.0f, 0.095f, 0.98f, -0.97f, 0.165f, 0.0f);   /* the bottom bar */
    pm_mat(pm, PMAT_HTRIM);
    for (int s = 0; s < 2; ++s) { const float x = s ? 0.98f : 0.0f; bx(pm, x, -1.0f, 0.06f, x + 0.02f, 0.0f, 0.18f, 0.0f); }
}

/* ========================================================= Latin America (HM5b1c) === */
/* An exposed concrete frame member (the Self-built sheet's panels 1-6: columns and beams proud of the hollow brick
 * infill; x 0..1 stretched along the member, turned upright for a column): HK_FRAME_W across (y 0..), HK_FRAME_P proud;
 * two shuttering boards' faces a few millimetres out of line, chipped arrises, grout that leaked from the forms' joints. */
void m_h_frame_strip(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    const float W = HK_FRAME_W, P = HK_FRAME_P;
    if (L2) { bx(pm, 0.0f, 0.0f, 0.0f, 1.0f, W, P, 0.0f); return; }
    const float s[8][2] = { { 0.0f, 0.0f }, { P - 0.012f, 0.0f }, { P, 0.014f }, { P, 0.5f * W }, { P + 0.004f, 0.5f * W },
                            { P + 0.004f, W - 0.01f }, { P - 0.006f, W }, { 0.0f, W } };
    run_x(pm, s, 8, 0.0f, 1.0f);
    if (!L0) return;
    /* grout fins along the boards' joint and an arris, broken (fixed positions: the module is shared) */
    const float fx[6][2] = { { 0.04f, 0.17f }, { 0.31f, 0.38f }, { 0.52f, 0.71f }, { 0.83f, 0.9f }, { 0.12f, 0.2f }, { 0.6f, 0.66f } };
    for (int k = 0; k < 4; ++k) bx(pm, fx[k][0], 0.5f * W - 0.004f, P, fx[k][1], 0.5f * W + 0.004f, P + 0.01f, 0.0f);
    for (int k = 4; k < 6; ++k) bx(pm, fx[k][0], W - 0.004f, P - 0.012f, fx[k][1], W + 0.008f, P - 0.002f, 0.0f);
}
/* A carved timber corbel under a deep tile eave (the Colonial sheet's panel 1, the eave's row; on a wall, its top at
 * y 0 under the eave, reaching HK_CORBEL_L out): a beam end with a rolled scroll at its tip, an ogee underside back to
 * the wall HK_CORBEL_H down, a smaller roll there; carved bands on its sides. */
void m_h_corbel_carved(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float L = HK_CORBEL_L, Hc = HK_CORBEL_H, hw = 0.065f;
    float s[24][2];
    int n = 0;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    s[n][0] = L; s[n][1] = 0.0f; n++;
    s[n][0] = L; s[n][1] = -0.13f; n++;
    const int k1 = L0 ? 10 : L1 ? 6 : 3;
    for (int k = 0; k <= k1; ++k) {   /* an ogee from under the tip back to the wall's foot */
        const float t = (float)k / (float)k1;
        s[n][0] = (L - 0.06f) * (1.0f - t) + 0.04f * t;
        s[n][1] = -0.13f - (Hc - 0.13f) * (0.5f - 0.5f * cosf(PI_F * t)) + 0.04f * sinf(2.0f * PI_F * t);
        n++;
    }
    s[n][0] = 0.0f; s[n][1] = -Hc; n++;
    run_x(pm, (const float (*)[2])s, n, -hw, hw);
    if (L2) return;
    cyl_x(pm, -hw - 0.01f, hw + 0.01f, -0.075f, L - 0.06f, 0.07f, segs(pm, 12, 6));   /* the scroll at the tip */
    cyl_x(pm, -hw - 0.006f, hw + 0.006f, -Hc + 0.06f, 0.07f, 0.045f, segs(pm, 10, 6));   /* the roll at the foot */
    if (!L0) return;
    for (int sd = 0; sd < 2; ++sd) {   /* the carved bands on its sides */
        const float x0 = sd ? hw : -hw - 0.008f, x1 = x0 + 0.008f;
        bx(pm, x0, -0.06f, 0.04f, x1, -0.03f, L - 0.16f, 0.0f);
        bx(pm, x0, -0.12f, 0.04f, x1, -0.1f, 0.5f * L, 0.0f);
    }
}
/* An iron reja (the Colonial sheet's panels 1, 4, 6): a cage projecting HK_REJA_D over a window HK_REJA_W x HK_REJA_H (its
 * foot at y 0, centred; stretched to the opening within about a quarter): ledges at the foot and head, uprights on its
 * front and sides, flats across with collars; a crest of C-scrolls over its head, a finial at the middle. */
void m_h_reja(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    const float hw = 0.5f * HK_REJA_W, H = HK_REJA_H, D = HK_REJA_D, w = 0.016f;
    for (int e = 0; e < 2; ++e) {   /* the ledges: a flat frame at the foot and the head, back to the wall */
        const float y = e ? H : 0.0f;
        bx(pm, -hw, y - 0.012f, D - 0.03f, hw, y + 0.012f, D, 0.0f);
        bx(pm, -hw, y - 0.012f, 0.0f, -hw + 0.03f, y + 0.012f, D - 0.03f, 0.0f);
        bx(pm, hw - 0.03f, y - 0.012f, 0.0f, hw, y + 0.012f, D - 0.03f, 0.0f);
    }
    const int nu = L0 ? 9 : L1 ? 7 : 5;
    for (int k = 0; k < nu; ++k) { const float x = -hw + 0.02f + (2.0f * hw - 0.04f) * (float)k / (float)(nu - 1); bx(pm, x - 0.5f * w, 0.012f, D - 0.024f, x + 0.5f * w, H - 0.012f, D - 0.008f, 0.0f); }
    const int ns = L2 ? 1 : 2;
    for (int sd = 0; sd < 2; ++sd)
        for (int k = 1; k <= ns; ++k) { const float x = sd ? hw - 0.015f : -hw + 0.015f, z = (D - 0.03f) * (float)k / (float)(ns + 1); bx(pm, x - 0.5f * w, 0.012f, z - 0.5f * w, x + 0.5f * w, H - 0.012f, z + 0.5f * w, 0.0f); }
    if (!L2) {
        for (int r = 1; r < 3; ++r) {   /* flats across, round the sides too */
            const float y = H * (float)r / 3.0f;
            bx(pm, -hw, y - 0.018f, D - 0.03f, hw, y + 0.018f, D - 0.022f, 0.0f);
            for (int sd = 0; sd < 2; ++sd) { const float x = sd ? hw - 0.008f : -hw; bx(pm, x, y - 0.018f, 0.0f, x + 0.008f, y + 0.018f, D - 0.03f, 0.0f); }
            if (L0) for (int k = 0; k < nu; ++k) { const float x = -hw + 0.02f + (2.0f * hw - 0.04f) * (float)k / (float)(nu - 1); bx(pm, x - 0.016f, y - 0.03f, D - 0.03f, x + 0.016f, y + 0.03f, D - 0.002f, 0.0f); }
        }
    }
    /* the crest: C-scrolls either side of a finial on a stem */
    const float cz0 = D - 0.024f, cz1 = D - 0.008f, cy = H + 0.012f, CH = HK_REJA_CREST;
    if (L2) { bar_xy(pm, -0.6f * hw, cy, 0.0f, cy + CH, 0.03f, cz0, cz1); bar_xy(pm, 0.0f, cy + CH, 0.6f * hw, cy, 0.03f, cz0, cz1); return; }
    const int np = L0 ? 10 : 6;
    for (int sd = 0; sd < 2; ++sd) {
        const float sg = sd ? 1.0f : -1.0f;
        /* a big scroll off the head curling in towards the middle, a smaller one at the outside, a bar to the stem */
        scroll_xy(pm, sg * 0.42f * hw, cy + 0.17f, 0.16f, 0.05f, sd ? 1.25f * PI_F : -0.25f * PI_F, sd ? -0.9f : 0.9f, np, w, cz0, cz1);
        scroll_xy(pm, sg * 0.82f * hw, cy + 0.08f, 0.08f, 0.025f, sd ? PI_F : 0.0f, sd ? -0.85f : 0.85f, L0 ? 7 : 4, w, cz0, cz1);
        bar_xy(pm, sg * 0.42f * hw, cy + 0.33f, sg * 0.04f, cy + CH - 0.08f, w, cz0, cz1);
    }
    bx(pm, -0.5f * w, cy, cz0, 0.5f * w, cy + CH - 0.04f, cz1, 0.0f);
    pm_push(pm); pm_translate(pm, 0.0f, cy + CH - 0.05f, 0.5f * (cz0 + cz1));
    const PmProf fin[5] = { { 0.0f, 0.0f, 1 }, { 0.02f, 0.01f, 0 }, { 0.035f, 0.05f, 0 }, { 0.012f, 0.09f, 0 }, { 0.0f, 0.12f, 1 } };
    pm_lathe(pm, fin, 5, segs(pm, 8, 4), 0.0f, 0.0f);
    pm_pop(pm);
}
/* A stucco crest over a window or door (the Colonial sheet's panels 2, 4, 6; on a wall from y 0 over its head, centred,
 * HK_WCREST_W x HK_WCREST_H): a plain band, a field rising in ogees to an oval cartouche at the middle; volutes at the
 * ogees' feet; at L0 a raised edge along the ogees, the volutes' spirals, a boss in the cartouche. In the unit's stone. */
void m_h_win_crest(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float W = 0.5f * HK_WCREST_W, H = HK_WCREST_H, yb = 0.08f;
    bx(pm, -W, 0.0f, 0.0f, W, yb, 0.05f, L2 ? 0.0f : 0.006f);
    const int no = L0 ? 8 : L1 ? 5 : 2;
    float half[16][2];
    int n = 0;
    for (int k = 0; k <= no; ++k) {   /* the ogee from the band's end up to the cartouche's side */
        const float t = (float)k / (float)no;
        half[n][0] = 0.82f * W - (0.82f * W - 0.15f) * t;
        half[n][1] = yb + (0.7f * H - yb) * (0.5f - 0.5f * cosf(PI_F * t));
        n++;
    }
    float p[40][2];
    int m = 0;
    for (int i = n - 1; i >= 0; --i) { p[m][0] = -half[i][0]; p[m][1] = half[i][1]; m++; }
    for (int i = 0; i < n; ++i) { p[m][0] = half[i][0]; p[m][1] = half[i][1]; m++; }
    run_z(pm, (const float (*)[2])p, m, 0.0f, 0.035f);
    /* the cartouche: an oval plaque over the middle */
    const int nc = L0 ? 14 : L1 ? 10 : 6;
    float c[16][2];
    for (int k = 0; k < nc; ++k) { const float a = 2.0f * PI_F * (float)k / (float)nc; c[k][0] = 0.17f * cosf(a); c[k][1] = 0.62f * H + 0.36f * H * sinf(a); }
    run_z(pm, (const float (*)[2])c, nc, 0.0f, 0.06f);
    if (L2) return;
    for (int sd = 0; sd < 2; ++sd) {
        const float sg = sd ? 1.0f : -1.0f;
        cyl_z(pm, 0.0f, 0.06f, sg * 0.82f * W, yb + 0.05f, 0.07f, segs(pm, 12, 6));   /* the volute's eye */
        if (!L0) continue;
        for (int i = 0; i + 1 < n; ++i) bar_xy(pm, sg * half[i][0], half[i][1] + 0.02f, sg * half[i + 1][0], half[i + 1][1] + 0.02f, 0.04f, 0.035f, 0.055f);
        scroll_xy(pm, sg * 0.82f * W, yb + 0.05f, 0.09f, 0.03f, 0.5f * PI_F, sd ? -0.8f : 0.8f, 6, 0.025f, 0.06f, 0.075f);
    }
    if (L0) cyl_z(pm, 0.06f, 0.09f, 0.0f, 0.62f * H, 0.06f, 12);
}
/* A timber balcony's rail (the Colonial sheet's panels 2, 5; x 0..1, whole metres, standing on y 0 centred on z 0): a
 * post at the metre's start, a bottom rail, turned balusters, a moulded handrail to HK_TBAL_H. */
void m_h_baluster_timber(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float H = HK_TBAL_H;
    bx(pm, 0.0f, 0.0f, -0.05f, 0.1f, H + 0.06f, 0.05f, L2 ? 0.0f : 0.006f);   /* the post */
    if (!L2) bx(pm, -0.01f, H + 0.06f, -0.06f, 0.11f, H + 0.09f, 0.06f, 0.004f);
    bx(pm, 0.1f, 0.03f, -0.04f, 1.0f, 0.11f, 0.04f, 0.0f);                    /* the bottom rail */
    const float hr[6][2] = { { -0.05f, H - 0.08f }, { 0.05f, H - 0.08f }, { 0.05f, H - 0.03f }, { 0.035f, H }, { -0.035f, H }, { -0.05f, H - 0.03f } };
    run_x(pm, hr, 6, 0.1f, 1.0f);
    const int nb = 7;
    for (int k = 0; k < nb; ++k) {
        const float x = 0.1f + 0.9f * ((float)k + 0.5f) / (float)nb;
        if (L2) { bx(pm, x - 0.022f, 0.11f, -0.022f, x + 0.022f, H - 0.08f, 0.022f, 0.0f); continue; }
        bx(pm, x - 0.026f, 0.11f, -0.026f, x + 0.026f, 0.2f, 0.026f, 0.0f);
        bx(pm, x - 0.026f, H - 0.16f, -0.026f, x + 0.026f, H - 0.08f, 0.026f, 0.0f);
        pm_push(pm); pm_translate(pm, x, 0.0f, 0.0f);
        if (L0) {
            const PmProf v[8] = { { 0.0f, 0.2f, 1 }, { 0.024f, 0.2f, 1 }, { 0.018f, 0.25f, 0 }, { 0.033f, 0.38f, 0 }, { 0.016f, 0.6f, 0 },
                                  { 0.014f, H - 0.2f, 0 }, { 0.024f, H - 0.16f, 1 }, { 0.0f, H - 0.16f, 1 } };
            pm_lathe(pm, v, 8, 8, 0.0f, 0.0f);
        } else {
            const PmProf v[5] = { { 0.0f, 0.2f, 1 }, { 0.024f, 0.2f, 1 }, { 0.032f, 0.38f, 0 }, { 0.016f, H - 0.16f, 1 }, { 0.0f, H - 0.16f, 1 } };
            pm_lathe(pm, v, 5, 5, 0.0f, 0.0f);
        }
        pm_pop(pm);
    }
}
/* A carved timber bracket under a balcony (the Colonial sheet's panels 2, 5; on a wall, its top at y 0 under the
 * balcony's floor, reaching HK_TBRACKET_L out and HK_TBRACKET_D down the wall): a scrolled S-curve from the wall's foot
 * to the tip, a turned pendant under the tip, a roll at the foot, carved bands on its sides. */
void m_h_bracket_timber(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    const float L = HK_TBRACKET_L, D = HK_TBRACKET_D, hw = 0.06f;
    float s[32][2];
    int n = 0;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    s[n][0] = L; s[n][1] = 0.0f; n++;
    s[n][0] = L; s[n][1] = -0.1f; n++;
    const int k1 = L0 ? 12 : L1 ? 7 : 3;
    for (int k = 0; k <= k1; ++k) {   /* the S: under the arm in, then down the wall, swelling once */
        const float t = (float)k / (float)k1;
        s[n][0] = (L - 0.08f) * (1.0f - t) + 0.12f * t + 0.08f * sinf(PI_F * t);
        s[n][1] = -0.1f - (D - 0.18f) * t * t;
        n++;
    }
    s[n][0] = 0.12f; s[n][1] = -D; n++;
    s[n][0] = 0.0f; s[n][1] = -D; n++;
    run_x(pm, (const float (*)[2])s, n, -hw, hw);
    if (L2) return;
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, L - 0.06f);
    const PmProf p[6] = { { 0.0f, -0.32f, 1 }, { 0.02f, -0.3f, 0 }, { 0.04f, -0.24f, 0 }, { 0.025f, -0.17f, 0 }, { 0.035f, -0.1f, 1 }, { 0.0f, -0.1f, 1 } };
    pm_lathe(pm, p, 6, segs(pm, 8, 5), 0.0f, 0.0f);
    pm_pop(pm);
    cyl_x(pm, -hw - 0.008f, hw + 0.008f, -D + 0.07f, 0.1f, 0.06f, segs(pm, 10, 6));
    if (L0) for (int sd = 0; sd < 2; ++sd) { const float x0 = sd ? hw : -hw - 0.008f; bx(pm, x0, -0.07f, 0.05f, x0 + 0.008f, -0.04f, L - 0.14f, 0.0f); }
}
