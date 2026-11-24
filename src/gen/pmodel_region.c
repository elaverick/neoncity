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
