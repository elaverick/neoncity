#include "pmodel.h"
#include "pmodel_food.h"
#include "mathlib.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * The module library (pmodel.h): every module built in its own frame, y up,
 * at three levels of detail. Sizes after docs/noodle-bar-target.md. Frames:
 * things that stand (stools, pots, bottles) stand on y 0 centred on the axis;
 * things fixed to a surface (bolts, rivets, switches, gauges' stems,
 * brackets) have that surface at y 0 (or z 0 for wall pieces, out along +z);
 * things that hang (utensils, hooks) hang from the origin.
 * ========================================================================= */

#define L0 (pm->lod == PM_LOD0)
#define L2 (pm->lod == PM_LOD2)

static void lathe(Pm *pm, const PmProf *p, int n, int segs) { pm_lathe(pm, p, n, pm_segs(pm, segs, 4), 0, 0); }
/* A lathe flat-shaded round the turn (hex heads and nuts). */
static void lathe_faceted(Pm *pm, const PmProf *p, int n, int segs) {
    uint32_t v0 = pm->out->nv, i0 = pm->out->ni;
    pm_lathe(pm, p, n, segs, 0, 0);
    pm_flatten(pm, v0, i0);
}
/* A turned shell from its outside, wall t, rim bead r. */
static void shell(Pm *pm, const PmProf *o, int n, float t, float rim, int segs) {
    PmProf s[120];
    int k = pm_prof_shell(o, n, t, rim, L0 ? 6 : 3, s, 120);
    if (k) lathe(pm, s, k, segs);
}
/* A thin ring band round the axis (a label, a hoop): r0 inside, r1 outside, y0..y1. */
static void band(Pm *pm, float r0, float r1, float y0, float y1, int segs) {
    PmProf p[5] = { { r0, y0, 1 }, { r1, y0, 1 }, { r1, y1, 1 }, { r0, y1, 1 }, { r0, y0, 1 } };
    lathe(pm, p, 5, segs);
}
static void set3(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }

/* --------------------------------------------------------- small parts --- */
static void m_rivet(Pm *pm) {
    if (!L0) return;
    pm_mat(pm, PMAT_STEEL);
    /* a dome, closed underneath (it sits 0.4 mm into the sheet) */
    float sup = pm->support;
    pm->support = 0.0f;
    PmProf p[5] = { { 0.0f, -0.0004f, 0 }, { 0.0032f, -0.0004f, 0 }, { 0.0029f, 0.0010f, 0 }, { 0.0017f, 0.0021f, 0 }, { 0.0f, 0.0025f, 0 } };
    pm_lathe(pm, p, 5, 6, 0, 0);
    pm->support = sup;
}
static void rivets_x(Pm *pm, int n) {
    for (int k = 0; k < n; ++k) { pm_push(pm); pm_translate(pm, 0.06f * (float)k, 0, 0); m_rivet(pm); pm_pop(pm); }
}
static void m_rivet_row4(Pm *pm) { rivets_x(pm, 4); }
static void m_rivet_row10(Pm *pm) { rivets_x(pm, 10); }

static void hex(Pm *pm, float r, float y0, float y1, float ch) {
    PmProf p[6] = { { 0, y0, 0 }, { r, y0, 1 }, { r, y1 - ch, 1 }, { r - ch, y1, 1 }, { 0, y1, 0 } };
    lathe_faceted(pm, p, 5, 6);
}
static void m_bolt(Pm *pm) {
    if (L2) return;
    pm_mat(pm, PMAT_STEEL);
    if (L0) { PmProf w[4] = { { 0, 0, 0 }, { 0.0085f, 0, 0 }, { 0.0085f, 0.0016f, 0 }, { 0, 0.0016f, 0 } }; lathe(pm, w, 4, 12); }
    hex(pm, 0.0075f, 0.0016f, 0.0072f, L0 ? 0.0008f : 0.0f);
}
static void m_nut_washer(Pm *pm) {
    if (L2) return;
    pm_mat(pm, PMAT_STEEL);
    PmProf w[4] = { { 0, 0, 0 }, { 0.0095f, 0, 0 }, { 0.0095f, 0.0018f, 0 }, { 0, 0.0018f, 0 } };
    lathe(pm, w, 4, 12);
    hex(pm, 0.0078f, 0.0018f, 0.0085f, L0 ? 0.0008f : 0.0f);
    PmProf s[5] = { { 0, 0.0085f, 0 }, { 0.004f, 0.0085f, 0 }, { 0.004f, 0.0125f, 0 }, { 0.0032f, 0.0133f, 0 }, { 0, 0.0133f, 0 } };
    lathe(pm, s, 5, 10);
}

/* A range knob, 40 mm, its face up +y. */
static void m_knob(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    PmProf sk[5] = { { 0, 0, 0 }, { 0.024f, 0, 0 }, { 0.024f, 0.002f, 0 }, { 0.02f, 0.0035f, 0 }, { 0, 0.0035f, 0 } };
    lathe(pm, sk, 5, 24);
    pm_mat(pm, PMAT_PLASTIC);
    PmProf b[7] = { { 0, 0.003f, 0 }, { 0.0185f, 0.003f, 0 }, { 0.0185f, 0.012f, 0 }, { 0.0165f, 0.02f, 0 }, { 0.0145f, 0.0235f, 0 }, { 0.011f, 0.025f, 0 }, { 0, 0.0255f, 0 } };
    lathe(pm, b, 7, 24);
    float c[3] = { 0, 0.03f, 0 }, h[3] = { 0.015f, 0.006f, 0.0045f };
    pm_bbox(pm, c, h, 0.002f, 2);
    if (L0) { pm_mat(pm, PMAT_DIAL); float c2[3] = { 0.0105f, 0.0362f, 0 }, h2[3] = { 0.0035f, 0.0003f, 0.0008f }; pm_bbox(pm, c2, h2, 0.0f, 1); }
}

/* A toggle switch on its plate, the panel's face at y 0. */
static void m_toggle(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    float c[3] = { 0, 0.0015f, 0 }, h[3] = { 0.016f, 0.0015f, 0.026f };
    pm_bbox(pm, c, h, 0.001f, 2);
    if (L0) for (int k = -1; k <= 1; k += 2) { pm_push(pm); pm_translate(pm, 0, 0.003f, 0.019f * (float)k); pm_place(pm, PMK_BOLT); pm_pop(pm); }
    pm_mat(pm, PMAT_BRASS);
    PmProf b[4] = { { 0, 0.003f, 0 }, { 0.006f, 0.003f, 0 }, { 0.006f, 0.012f, 0 }, { 0, 0.012f, 0 } };
    lathe(pm, b, 4, 12);
    pm_mat(pm, PMAT_STEEL);
    hex(pm, 0.0082f, 0.003f, 0.0055f, 0.0f);
    pm_mat(pm, PMAT_STAINLESS);
    pm_push(pm);
    pm_translate(pm, 0, 0.011f, 0);
    pm_rotate_x(pm, 0.35f);
    PmProf l[6] = { { 0, 0, 0 }, { 0.0024f, 0, 0 }, { 0.0016f, 0.02f, 0 }, { 0.0028f, 0.021f, 0 }, { 0.0026f, 0.0245f, 0 }, { 0, 0.0255f, 0 } };
    lathe(pm, l, 6, 10);
    pm_pop(pm);
}

/* A pressure gauge (63 mm) on its stem: the stem's foot at the origin, up +y; the face looks +z. */
static void m_gauge(Pm *pm) {
    pm_mat(pm, PMAT_BRASS);
    hex(pm, 0.0095f, 0.0f, 0.009f, L0 ? 0.001f : 0.0f);
    PmProf s[4] = { { 0, 0.008f, 0 }, { 0.0055f, 0.008f, 0 }, { 0.0055f, 0.036f, 0 }, { 0, 0.036f, 0 } };
    lathe(pm, s, 4, 12);
    pm_push(pm);
    pm_translate(pm, 0, 0.066f, -0.012f);
    pm_rotate_x(pm, 0.5f * PI_F);   /* local +y (the face) to +z */
    pm_mat(pm, PMAT_STEEL);
    PmProf c[9] = { { 0, 0, 0 }, { 0.0300f, 0, 0 }, { 0.0320f, 0.002f, 0 }, { 0.0320f, 0.017f, 0 }, { 0.0337f, 0.018f, 0 },
                    { 0.0337f, 0.0225f, 0 }, { 0.0318f, 0.0245f, 0 }, { 0.0292f, 0.0245f, 1 }, { 0, 0.0245f, 0 } };
    /* the bezel's top drops to the face inside it */
    c[8].y = 0.0195f; c[7].y = 0.0245f;
    PmProf cc[10];
    memcpy(cc, c, sizeof(PmProf) * 8);
    cc[8].r = 0.0292f; cc[8].y = 0.0195f; cc[8].hard = 1;
    cc[9].r = 0.0f; cc[9].y = 0.0195f; cc[9].hard = 0;
    lathe(pm, cc, 10, 28);
    pm_mat(pm, PMAT_DIAL);
    PmProf f[4] = { { 0, 0.0195f, 0 }, { 0.029f, 0.0195f, 0 }, { 0.029f, 0.0205f, 0 }, { 0, 0.0205f, 0 } };
    lathe(pm, f, 4, 28);
    pm_mat(pm, PMAT_RED);
    {
        float a = 0.6f, c3[3] = { 0.009f * cosf(a), 0.0213f, -0.009f * sinf(a) }, h3[3] = { 0.012f, 0.0004f, 0.0007f };
        pm_push(pm); pm_translate(pm, c3[0], c3[1], c3[2]); pm_rotate_y(pm, a);
        float z[3] = { 0, 0, 0 };
        pm_bbox(pm, z, h3, 0.0f, 1);
        pm_pop(pm);
        PmProf hb[4] = { { 0, 0.0205f, 0 }, { 0.0025f, 0.0205f, 0 }, { 0.0025f, 0.0222f, 0 }, { 0, 0.0222f, 0 } };
        lathe(pm, hb, 4, 10);
    }
    pm_mat(pm, PMAT_GLASS);
    PmProf g[5] = { { 0, 0.0228f, 0 }, { 0.0294f, 0.0228f, 0 }, { 0.0294f, 0.0234f, 1 }, { 0.016f, 0.0244f, 0 }, { 0, 0.0247f, 0 } };
    lathe(pm, g, 5, 28);
    pm_pop(pm);
}

/* A gate valve on a pipe along x through the origin, its wheel up +y. */
static void m_valve_wheel(Pm *pm) {
    pm_mat(pm, PMAT_BRASS);
    pm_push(pm);
    pm_rotate_z(pm, -0.5f * PI_F);   /* +y onto +x: the body along the pipe */
    PmProf b[11] = { { 0, -0.036f, 0 }, { 0.0145f, -0.036f, 0 }, { 0.0145f, -0.027f, 0 }, { 0.012f, -0.025f, 0 }, { 0.019f, -0.016f, 0 },
                     { 0.022f, 0.0f, 0 }, { 0.019f, 0.016f, 0 }, { 0.012f, 0.025f, 0 }, { 0.0145f, 0.027f, 0 }, { 0.0145f, 0.036f, 0 }, { 0, 0.036f, 0 } };
    lathe(pm, b, 11, 16);
    pm_pop(pm);
    PmProf bn[7] = { { 0, 0.012f, 0 }, { 0.017f, 0.012f, 0 }, { 0.017f, 0.038f, 0 }, { 0.012f, 0.043f, 0 }, { 0.009f, 0.052f, 0 }, { 0.009f, 0.058f, 0 }, { 0, 0.058f, 0 } };
    lathe(pm, bn, 7, 16);
    pm_mat(pm, PMAT_STEEL);
    PmProf st[4] = { { 0, 0.058f, 0 }, { 0.004f, 0.058f, 0 }, { 0.004f, 0.098f, 0 }, { 0, 0.098f, 0 } };
    lathe(pm, st, 4, 8);
    pm_mat(pm, PMAT_RED);
    pm_push(pm);
    pm_translate(pm, 0, 0.088f, 0);
    pm_torus(pm, 0.038f, 0.0045f, 28, 8);
    int ns = L2 ? 3 : 5;
    for (int k = 0; k < ns; ++k) {
        float a = 2.0f * PI_F * (float)k / (float)ns, p0[3], p1[3];
        set3(p0, 0.008f * cosf(a), 0, 0.008f * sinf(a)); set3(p1, 0.036f * cosf(a), -0.004f, 0.036f * sinf(a));
        pm_beam(pm, p0, p1, 0.006f, 0.004f, 0.0f, NULL);
    }
    PmProf h[4] = { { 0, -0.004f, 0 }, { 0.009f, -0.004f, 0 }, { 0.009f, 0.005f, 0 }, { 0, 0.005f, 0 } };
    lathe(pm, h, 4, 12);
    pm_pop(pm);
    pm_mat(pm, PMAT_STEEL);
    pm_push(pm); pm_translate(pm, 0, 0.093f, 0); m_nut_washer(pm); pm_pop(pm);
}

/* A ball valve on a pipe along x through the origin, the lever along the pipe (open). */
static void m_valve_lever(Pm *pm) {
    pm_mat(pm, PMAT_BRASS);
    float c[3] = { 0, 0, 0 }, h[3] = { 0.022f, 0.016f, 0.016f };
    pm_bbox(pm, c, h, 0.004f, 2);
    for (int s = -1; s <= 1; s += 2) {
        pm_push(pm);
        pm_translate(pm, 0.022f * (float)s, 0, 0);
        pm_rotate_z(pm, -0.5f * PI_F * (float)s);
        PmProf e[4] = { { 0, 0, 0 }, { 0.0145f, 0, 1 }, { 0.0145f, 0.012f, 1 }, { 0, 0.012f, 0 } };
        lathe_faceted(pm, e, 4, 6);
        pm_pop(pm);
    }
    PmProf st[4] = { { 0, 0.016f, 0 }, { 0.005f, 0.016f, 0 }, { 0.005f, 0.026f, 0 }, { 0, 0.026f, 0 } };
    lathe(pm, st, 4, 10);
    pm_mat(pm, PMAT_RED);
    float a[3] = { -0.006f, 0.029f, 0 }, b[3] = { 0.1f, 0.029f, 0 };
    pm_beam(pm, a, b, 0.004f, 0.014f, 0.0f, NULL);
    pm_mat(pm, PMAT_PLASTIC);
    float gc[3] = { 0.08f, 0.029f, 0 }, gh[3] = { 0.026f, 0.0065f, 0.0045f };
    pm_bbox(pm, gc, gh, 0.002f, 2);
    pm_mat(pm, PMAT_STEEL);
    pm_push(pm); pm_translate(pm, 0, 0.031f, 0); pm_place(pm, PMK_BOLT); pm_pop(pm);
}

/* An angle bracket on a wall (z 0, out +z): a 30 x 4 mm flat bar 0.2 m down the wall and 0.2 m out, braced. */
static void m_bracket(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    float a[3], b[3];
    set3(a, 0, 0.0f, 0.002f); set3(b, 0, -0.2f, 0.002f);
    float up[3] = { 0, 0, 1 };
    pm_beam(pm, a, b, 0.03f, 0.004f, 0.0f, up);
    set3(a, 0, 0.002f, 0.0f); set3(b, 0, 0.002f, 0.2f);
    float up2[3] = { 0, 1, 0 };
    pm_beam(pm, a, b, 0.03f, 0.004f, 0.0f, up2);
    set3(a, 0, -0.17f, 0.004f); set3(b, 0, -0.004f, 0.17f);
    pm_beam(pm, a, b, 0.022f, 0.004f, 0.0f, NULL);
    if (!L2)
        for (int k = 0; k < 2; ++k) {
            pm_push(pm);
            pm_translate(pm, 0, -0.05f - 0.1f * (float)k, 0.004f);
            pm_rotate_x(pm, 0.5f * PI_F);
            pm_place(pm, PMK_BOLT);
            pm_pop(pm);
        }
}

/* A saddle clip for a 22 mm pipe along z, on a surface at y 0. */
static void m_saddle(Pm *pm) {
    pm_mat(pm, PMAT_GALV);
    PmPath pa;
    pm_path_init(&pa, 24);
    float c[3] = { 0, 0.011f, 0 }, ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 };
    pm_path_arc(&pa, c, ex, ey, 0.0122f, 0.0f, PI_F, L0 ? 12 : 6);
    PmSect s;
    pm_sect_rect(&s, 0.0016f, 0.012f, 0.0f);
    float up[3] = { 0, 0, 1 };
    pm_sweep(pm, &pa, &s, up, 3);
    pm_path_free(&pa);
    for (int k = -1; k <= 1; k += 2) {
        float fc[3] = { 0.021f * (float)k, 0.0008f, 0 }, fh[3] = { 0.009f, 0.0008f, 0.006f };
        pm_bbox(pm, fc, fh, 0.0f, 1);
        float wc[3] = { 0.0128f * (float)k, 0.0055f, 0 }, wh[3] = { 0.0008f, 0.0055f, 0.006f };
        pm_bbox(pm, wc, wh, 0.0f, 1);
        if (L0) { pm_push(pm); pm_translate(pm, 0.023f * (float)k, 0.0016f, 0); pm_place(pm, PMK_BOLT); pm_pop(pm); }
    }
}

/* A ceramic post insulator on its steel pin, standing on y 0. */
static void m_insulator(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    hex(pm, 0.009f, 0.0f, 0.006f, 0.0f);
    PmProf pin[4] = { { 0, 0.006f, 0 }, { 0.006f, 0.006f, 0 }, { 0.006f, 0.02f, 0 }, { 0, 0.02f, 0 } };
    lathe(pm, pin, 4, 8);
    pm_mat(pm, PMAT_CERAMIC);
    PmProf p[18] = {
        { 0, 0.018f, 0 }, { 0.014f, 0.018f, 0 }, { 0.014f, 0.028f, 0 }, { 0.033f, 0.035f, 0 }, { 0.033f, 0.038f, 0 }, { 0.016f, 0.042f, 0 },
        { 0.029f, 0.049f, 0 }, { 0.029f, 0.052f, 0 }, { 0.015f, 0.056f, 0 }, { 0.015f, 0.064f, 0 }, { 0.0115f, 0.067f, 0 },
        { 0.0115f, 0.071f, 0 }, { 0.0155f, 0.074f, 0 }, { 0.0155f, 0.079f, 0 }, { 0.012f, 0.084f, 0 }, { 0.0f, 0.0855f, 0 } };
    lathe(pm, p, 16, 24);
}

/* A counter bell, 85 mm, standing on y 0. */
static void m_bell(Pm *pm) {
    pm_mat(pm, PMAT_PLASTIC);
    PmProf b[5] = { { 0, 0, 0 }, { 0.046f, 0, 0 }, { 0.046f, 0.007f, 0 }, { 0.041f, 0.012f, 0 }, { 0, 0.012f, 0 } };
    lathe(pm, b, 5, 28);
    pm_mat(pm, PMAT_BRASS);
    PmProf d[8] = { { 0, 0.012f, 0 }, { 0.0425f, 0.012f, 0 }, { 0.0425f, 0.016f, 1 }, { 0.040f, 0.028f, 0 }, { 0.033f, 0.042f, 0 },
                    { 0.022f, 0.051f, 0 }, { 0.010f, 0.055f, 0 }, { 0, 0.056f, 0 } };
    lathe(pm, d, 8, 32);
    pm_mat(pm, PMAT_STEEL);
    PmProf pl[6] = { { 0, 0.054f, 0 }, { 0.0028f, 0.054f, 0 }, { 0.0028f, 0.068f, 0 }, { 0.006f, 0.07f, 0 }, { 0.006f, 0.074f, 0 }, { 0, 0.076f, 0 } };
    lathe(pm, pl, 6, 12);
}

/* ------------------------------------------------------------- seating --- */
/* A pedestal stool: seat 0.36 m round at 0.75 m, a 60 mm pedestal, a 0.40 m base, a foot ring. */
static void m_stool(Pm *pm) {
    pm_mat(pm, PMAT_PAINTED);
    PmProf b[8] = { { 0, 0, 0 }, { 0.2f, 0, 0 }, { 0.2f, 0.007f, 0 }, { 0.193f, 0.013f, 0 }, { 0.09f, 0.03f, 0 }, { 0.045f, 0.038f, 0 }, { 0.034f, 0.045f, 0 }, { 0, 0.045f, 0 } };
    lathe(pm, b, 8, 32);
    PmProf p[8] = { { 0, 0.04f, 0 }, { 0.03f, 0.04f, 0 }, { 0.03f, 0.63f, 0 }, { 0.036f, 0.635f, 0 }, { 0.036f, 0.655f, 0 }, { 0.06f, 0.66f, 0 }, { 0.06f, 0.666f, 0 }, { 0, 0.666f, 0 } };
    lathe(pm, p, 8, 24);
    pm_mat(pm, PMAT_STAINLESS);
    pm_push(pm);
    pm_translate(pm, 0, 0.29f, 0);
    pm_torus(pm, 0.165f, 0.011f, 32, 8);
    pm_pop(pm);
    pm_mat(pm, PMAT_PAINTED);
    for (int k = 0; k < 3; ++k) {
        float a = 2.0f * PI_F * (float)k / 3.0f + 0.3f, p0[3], p1[3];
        set3(p0, 0.025f * cosf(a), 0.27f, 0.025f * sinf(a)); set3(p1, 0.158f * cosf(a), 0.29f, 0.158f * sinf(a));
        pm_beam(pm, p0, p1, 0.018f, 0.012f, 0.002f, NULL);
    }
    pm_mat(pm, PMAT_STEEL);
    PmProf pan[5] = { { 0, 0.662f, 0 }, { 0.165f, 0.662f, 0 }, { 0.178f, 0.672f, 0 }, { 0.178f, 0.676f, 0 }, { 0, 0.676f, 0 } };
    lathe(pm, pan, 5, 32);
    pm_mat(pm, PMAT_RUBBER);
    PmProf s[9] = { { 0, 0.674f, 0 }, { 0.176f, 0.674f, 0 }, { 0.183f, 0.682f, 0 }, { 0.184f, 0.712f, 0 }, { 0.181f, 0.732f, 0 },
                    { 0.172f, 0.744f, 0 }, { 0.15f, 0.749f, 0 }, { 0.08f, 0.751f, 0 }, { 0, 0.751f, 0 } };
    lathe(pm, s, 9, 32);
    /* the seat's piping */
    if (L0) { pm_push(pm); pm_translate(pm, 0, 0.7105f, 0); pm_torus(pm, 0.1845f, 0.0035f, 32, 6); pm_pop(pm); }
}

/* SF3: a cafe table (the deep shop unit's room): a 0.6 m square stainless top at 0.74 m on a steel frame, a 76 mm
 * pedestal, a cross foot of two flat bars on rubber pads. */
static void m_cafe_table(Pm *pm) {
    const float e = 0.3f, ty = 0.74f;
    pm_mat(pm, PMAT_STAINLESS);
    { float c[3] = { 0.0f, ty - 0.016f, 0.0f }, h[3] = { e, 0.016f, e }; pm_bbox(pm, c, h, L2 ? 0.0f : 0.008f, L0 ? 2 : 1); }
    pm_mat(pm, PMAT_STEEL);
    { float c[3] = { 0.0f, ty - 0.05f, 0.0f }, h[3] = { 0.2f, 0.018f, 0.2f }; pm_bbox(pm, c, h, 0.002f, 1); }
    PmProf col[6] = { { 0, 0.034f, 0 }, { 0.038f, 0.034f, 0 }, { 0.038f, ty - 0.074f, 0 }, { 0.06f, ty - 0.072f, 1 }, { 0.06f, ty - 0.066f, 0 }, { 0, ty - 0.066f, 0 } };
    lathe(pm, col, 6, 20);
    for (int k = 0; k < 2; ++k) {
        float a[3] = { k ? 0.0f : -0.27f, 0.019f, k ? -0.27f : 0.0f }, b[3] = { k ? 0.0f : 0.27f, 0.019f, k ? 0.27f : 0.0f };
        pm_beam(pm, a, b, 0.05f, 0.03f, 0.003f, NULL);
    }
    pm_mat(pm, PMAT_RUBBER);
    for (int k = 0; k < 4; ++k) {
        const float x = k == 0 ? -0.25f : k == 1 ? 0.25f : 0.0f, z = k == 2 ? -0.25f : k == 3 ? 0.25f : 0.0f;
        pm_push(pm); pm_translate(pm, x, 0.0f, z); pm_cyl(pm, 0.0f, 0.0045f, 0.018f, pm_segs(pm, 10, 4)); pm_pop(pm);
    }
}

/* SF3: a low stool (at the deep shop unit's tables): a 0.32 m round seat at 0.45 m in the trim coat, four splayed
 * steel legs, a ring between them, rubber feet. */
static void m_low_stool(Pm *pm) {
    pm_mat(pm, PMAT_PAINTED);
    PmProf s[6] = { { 0, 0.422f, 0 }, { 0.148f, 0.422f, 0 }, { 0.16f, 0.43f, 0 }, { 0.16f, 0.444f, 0 }, { 0.152f, 0.45f, 0 }, { 0, 0.45f, 0 } };
    lathe(pm, s, 6, 24);
    pm_mat(pm, PMAT_STEEL);
    for (int k = 0; k < 4; ++k) {
        const float a = 0.25f * PI_F + 0.5f * PI_F * (float)k, c = cosf(a), sn = sinf(a);
        float p0[3] = { 0.10f * c, 0.424f, 0.10f * sn }, p1[3] = { 0.165f * c, 0.012f, 0.165f * sn };
        pm_beam(pm, p0, p1, 0.022f, 0.022f, 0.004f, NULL);
        if (!L2) {
            pm_mat(pm, PMAT_RUBBER);
            pm_push(pm); pm_translate(pm, 0.166f * c, 0.0f, 0.166f * sn); pm_cyl(pm, 0.0f, 0.014f, 0.016f, pm_segs(pm, 10, 4)); pm_pop(pm);
            pm_mat(pm, PMAT_STEEL);
        }
    }
    pm_push(pm); pm_translate(pm, 0.0f, 0.16f, 0.0f); pm_torus(pm, 0.141f, 0.007f, pm_segs(pm, 20, 8), L2 ? 3 : 5); pm_pop(pm);
}

/* ------------------------------------------------------ SF4: shutters --- */
/* A roller shutter's curtain (stretched by its instance to the opening: its slats are the shutter set's, laid in
 * world metres on its plane by kit.glsl), its bottom bar with a rubber seal, the guide channels at its ends and a
 * padlock on a hasp. */
static void m_shutter(Pm *pm) {
    pm_mat(pm, PMAT_SHUTTER);
    pm_box(pm, -0.5f, 0.0f, -0.006f, 0.5f, 1.0f, 0.006f);
}
static void m_shutter_bar(Pm *pm) {
    pm_mat(pm, PMAT_GALV);
    float c[3] = { 0.0f, 0.036f, 0.0f }, h[3] = { 0.5f, 0.026f, 0.019f };
    pm_bbox(pm, c, h, L2 ? 0.0f : 0.004f, 1);
    pm_mat(pm, PMAT_RUBBER);
    pm_box(pm, -0.5f, 0.0f, -0.012f, 0.5f, 0.010f, 0.012f);
}
static void m_shutter_guide(Pm *pm) {
    pm_mat(pm, PMAT_GALV);
    pm_box(pm, -0.032f, 0.0f, 0.009f, 0.022f, 1.0f, 0.032f);
    pm_box(pm, -0.032f, 0.0f, -0.032f, 0.022f, 1.0f, -0.009f);
    pm_box(pm, -0.032f, 0.0f, -0.009f, -0.026f, 1.0f, 0.009f);
}
static void m_shutter_lock(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    pm_box(pm, -0.03f, 0.005f, 0.019f, 0.03f, 0.055f, 0.023f);   /* the hasp's plate on the bar */
    float c[3] = { 0.0f, 0.032f, 0.026f }, h[3] = { 0.008f, 0.014f, 0.004f };
    pm_bbox(pm, c, h, 0.0f, 1);                                  /* its staple */
    pm_mat(pm, PMAT_BRASS);
    float lc[3] = { 0.0f, -0.004f, 0.036f }, lh[3] = { 0.019f, 0.017f, 0.008f };
    pm_bbox(pm, lc, lh, L2 ? 0.0f : 0.003f, L0 ? 2 : 1);         /* the padlock's body, hanging below the staple */
    if (!L2) {
        pm_mat(pm, PMAT_STEEL);
        pm_push(pm); pm_translate(pm, 0.0f, 0.020f, 0.036f); pm_rotate_x(pm, 0.5f * PI_F);
        pm_torus(pm, 0.011f, 0.0028f, pm_segs(pm, 16, 6), 5);   /* its shackle through the staple */
        pm_pop(pm);
    }
}

/* ----------------------------------------------------------- the pots --- */
static void loop_handle(Pm *pm, float R, float y, float side, float span, float out, float r) {
    PmPath pa;
    pm_path_init(&pa, 24);
    float c[3] = { side * R, y, 0 }, ex[3] = { side, 0, 0 }, ey[3] = { 0, 0, 1 };
    /* a D loop: an ellipse half round out from the wall */
    int n = L0 ? 12 : L2 ? 4 : 6;
    for (int k = 0; k <= n; ++k) {
        float a = -0.5f * PI_F + PI_F * (float)k / (float)n;
        pm_path_add(&pa, c[0] + ex[0] * out * cosf(a), c[1], c[2] + ey[2] * span * sinf(a), 0);
    }
    pm_tube(pm, &pa, r, 10, 3);
    pm_path_free(&pa);
    /* the riveted plates it hangs from */
    for (int k = -1; k <= 1; k += 2) {
        float pc[3] = { side * (R + 0.003f), y, (float)k * span }, ph[3] = { 0.003f, 0.012f, 0.009f };
        pm_bbox(pm, pc, ph, 0.0015f, 1);
        if (L0) { pm_push(pm); pm_translate(pm, side * (R + 0.006f), y, (float)k * span); pm_rotate_z(pm, -0.5f * PI_F * side); pm_place(pm, PMK_RIVET); pm_pop(pm); }
    }
}

static void stockpot(Pm *pm, float R, float H, int copper) {
    pm_mat(pm, copper ? PMAT_COPPER : PMAT_STAINLESS);
    PmProf o[5] = { { 0, 0, 0 }, { R - 0.012f, 0, 0 }, { R - 0.003f, 0.004f, 0 }, { R, 0.012f, 0 }, { R, H, 0 } };
    shell(pm, o, 5, 0.0025f, 0.004f, 48);
    /* a capsule base */
    pm_mat(pm, PMAT_STAINLESS);
    PmProf bs[4] = { { 0, -0.004f, 0 }, { R - 0.02f, -0.004f, 0 }, { R - 0.02f, 0.001f, 0 }, { 0, 0.001f, 0 } };
    lathe(pm, bs, 4, 40);
    pm_mat(pm, copper ? PMAT_BRASS : PMAT_STAINLESS);
    loop_handle(pm, R, H - 0.055f, 1.0f, 0.035f, 0.045f, 0.0055f);
    loop_handle(pm, R, H - 0.055f, -1.0f, 0.035f, 0.045f, 0.0055f);
    /* broth at four fifths full */
    pm_mat(pm, PMAT_BROTH);
    float rb = R - 0.0035f;
    PmProf br[4] = { { 0, 0.8f * H - 0.004f, 0 }, { rb, 0.8f * H - 0.004f, 0 }, { rb, 0.8f * H, 0 }, { 0, 0.8f * H, 0 } };
    lathe(pm, br, 4, 40);
}
static void m_stockpot_s(Pm *pm) { stockpot(pm, 0.15f, 0.30f, 1); }
static void m_stockpot_l(Pm *pm) { stockpot(pm, 0.20f, 0.38f, 0); }

/* A lid for the small pot, its rim at y 0, a loop handle on top. */
static void m_lid(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    PmProf p[8] = { { 0, 0, 0 }, { 0.148f, 0, 0 }, { 0.156f, 0.002f, 0 }, { 0.156f, 0.006f, 1 }, { 0.148f, 0.01f, 0 }, { 0.11f, 0.026f, 0 }, { 0.05f, 0.036f, 0 }, { 0, 0.038f, 0 } };
    lathe(pm, p, 8, 48);
    PmPath pa;
    pm_path_init(&pa, 16);
    int n = L0 ? 10 : 4;
    for (int k = 0; k <= n; ++k) { float a = PI_F * (float)k / (float)n; pm_path_add(&pa, -0.035f * cosf(a), 0.036f + 0.025f * sinf(a), 0, 0); }
    pm_tube(pm, &pa, 0.005f, 10, 3);
    pm_path_free(&pa);
}

/* A stack of six enamel bowls, 0.19 m round. */
/* A bowl; one under another shows only its outside and rim, so it is closed by a disc just below the
 * rim of the one sitting in it. */
static void bowl(Pm *pm, int top) {
    PmProf o[8] = { { 0, 0.004f, 0 }, { 0.031f, 0.004f, 0 }, { 0.033f, 0.0f, 1 }, { 0.039f, 0.0f, 1 }, { 0.041f, 0.007f, 1 }, { 0.07f, 0.025f, 0 }, { 0.088f, 0.05f, 0 }, { 0.095f, 0.072f, 0 } };
    if (top) { shell(pm, o, 8, 0.004f, 0.0f, 40); return; }
    PmProf c[11];
    memcpy(c, o, sizeof(o));
    c[8].r = 0.0955f; c[8].y = 0.0745f; c[8].hard = 0;
    c[9].r = 0.091f;  c[9].y = 0.0745f; c[9].hard = 1;
    c[10].r = 0.0f;   c[10].y = 0.0745f; c[10].hard = 0;
    lathe(pm, c, 11, 40);
}
static void m_bowl_stack(Pm *pm) {
    pm_mat(pm, PMAT_ENAMEL);
    if (!L0) {
        /* NB5: the stack as one turned outline (the bottom bowl's outside, each bowl's wall rising out of the
         * rim below it, the top one's inside): the kit draws a stack at these levels from a metre, where the
         * old L1 (six whole bowls, 4,160 triangles) was too dense and the old L2 a plain cup */
        PmProf s[40];
        int n = 0;
        static const float O[7][2] = { { 0, 0.004f }, { 0.031f, 0.004f }, { 0.033f, 0.0f }, { 0.039f, 0.0f }, { 0.041f, 0.007f }, { 0.07f, 0.025f }, { 0.088f, 0.05f } };
        for (int k = 0; k < 7; ++k) { s[n].r = O[k][0]; s[n].y = O[k][1]; s[n].hard = (uint8_t)(k >= 2 && k <= 4); n++; }
        for (int b = 0; b < 6; ++b) {
            const float y = 0.022f * (float)b;
            if (b) { s[n].r = 0.088f; s[n].y = y + 0.05f; s[n].hard = 1; n++; }
            s[n].r = 0.095f; s[n].y = y + 0.072f; s[n].hard = 0; n++;
            s[n].r = 0.0955f; s[n].y = y + 0.0745f; s[n].hard = 1; n++;
            if (b < 5) { s[n].r = 0.0915f; s[n].y = y + 0.0745f; s[n].hard = 1; n++; }
        }
        const float t = 0.022f * 5.0f;
        static const float I[5][2] = { { 0.091f, 0.0745f }, { 0.084f, 0.05f }, { 0.066f, 0.027f }, { 0.03f, 0.009f }, { 0, 0.008f } };
        for (int k = 0; k < 5; ++k) { s[n].r = I[k][0]; s[n].y = t + I[k][1]; s[n].hard = (uint8_t)(k == 0); n++; }
        lathe(pm, s, n, L2 ? 48 : 36);
        return;
    }
    for (int k = 0; k < 6; ++k) { pm_push(pm); pm_translate(pm, 0, 0.022f * (float)k, 0); pm->part = (uint8_t)k; bowl(pm, k == 5); pm_pop(pm); }
    pm->part = 0;
}

/* A glass jar, its contents, a lid and a label. */
static void jar(Pm *pm, float R, float H, int contents) {
    const int sg = L2 ? 48 : 32;   /* NB5: its far level round enough from a metre (16 sides, not 10) */
    pm_mat(pm, PMAT_GLASS);
    PmProf o[7] = { { 0, 0, 0 }, { R - 0.006f, 0, 0 }, { R, 0.006f, 0 }, { R, H - 0.025f, 0 }, { R - 0.006f, H - 0.015f, 0 }, { R - 0.01f, H - 0.012f, 0 }, { R - 0.01f, H - 0.002f, 0 } };
    shell(pm, o, 7, 0.003f, 0.0f, sg);
    pm_mat(pm, contents);
    float fill = contents == PMAT_OIL ? 0.7f : 0.82f;
    PmProf c[4] = { { 0, 0.004f, 0 }, { R - 0.0035f, 0.004f, 0 }, { R - 0.0035f, fill * H, 0 }, { 0, fill * H, 0 } };
    lathe(pm, c, 4, 24);
    /* the contents as seen through the glass (the kit draws no glass clear): a skin just outside it up to the
     * fill, shaded glossy (NB5) */
    band(pm, R + 0.0001f, R + 0.0004f, 0.007f, fill * H, sg);
    pm_mat(pm, PMAT_STAINLESS);   /* a tin lid (NB5: red lids over red oil read as one red block) */
    PmProf l[6] = { { 0, H - 0.014f, 0 }, { R - 0.007f, H - 0.014f, 0 }, { R - 0.007f, H + 0.004f, 0 }, { R - 0.009f, H + 0.006f, 0 }, { 0, H + 0.006f, 0 } };
    lathe(pm, l, 5, sg);
    /* the label at every level (the kit draws a jar at its far level from a metre or two) */
    pm_mat(pm, PMAT_PAPER);
    band(pm, R + 0.0005f, R + 0.0009f, 0.36f * H, 0.66f * H, sg);
}
static void m_jar_oil(Pm *pm) { jar(pm, 0.05f, 0.16f, PMAT_OIL); }
static void m_jar_ginger(Pm *pm) { jar(pm, 0.065f, 0.13f, PMAT_GINGER); }

/* A sauce bottle, dark glass, 0.25 m. */
static void m_bottle(Pm *pm) {
    pm_mat(pm, PMAT_GLASS);
    PmProf p[10] = { { 0, 0.003f, 0 }, { 0.026f, 0.0f, 0 }, { 0.031f, 0.004f, 0 }, { 0.032f, 0.012f, 0 }, { 0.032f, 0.15f, 0 },
                     { 0.028f, 0.172f, 0 }, { 0.015f, 0.2f, 0 }, { 0.0125f, 0.228f, 0 }, { 0.0125f, 0.232f, 0 }, { 0, 0.232f, 0 } };
    lathe(pm, p, 10, 28);
    pm_mat(pm, PMAT_RED);
    PmProf c[5] = { { 0, 0.226f, 0 }, { 0.0138f, 0.226f, 0 }, { 0.0138f, 0.249f, 0 }, { 0.0125f, 0.251f, 0 }, { 0, 0.251f, 0 } };
    lathe(pm, c, 5, 20);
    pm_mat(pm, PMAT_PAPER);   /* the label at every level (NB5) */
    band(pm, 0.0321f, 0.0326f, 0.04f, 0.12f, 28);
}

/* --------------------------------------------------------- the range --- */
/* A ring burner, 0.12 m, on y 0. */
static void m_burner(Pm *pm) {
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf b[8] = { { 0, 0, 0 }, { 0.06f, 0, 0 }, { 0.06f, 0.014f, 0 }, { 0.056f, 0.018f, 0 }, { 0.05f, 0.018f, 0 }, { 0.046f, 0.026f, 0 }, { 0.046f, 0.03f, 0 }, { 0, 0.03f, 0 } };
    lathe(pm, b, 8, 32);
    /* the flame ports: notches round the crown, as teeth at LOD0 */
    if (L0)
        for (int k = 0; k < 24; ++k) {
            float a = 2.0f * PI_F * (float)k / 24.0f;
            pm_push(pm); pm_rotate_y(pm, a);
            float c[3] = { 0.043f, 0.0335f, 0 }, h[3] = { 0.004f, 0.0035f, 0.0025f };
            pm_bbox(pm, c, h, 0.0f, 1);
            pm_pop(pm);
        }
    pm_mat(pm, PMAT_BRASS);
    PmProf c[6] = { { 0, 0.034f, 0 }, { 0.040f, 0.034f, 0 }, { 0.042f, 0.037f, 0 }, { 0.040f, 0.041f, 0 }, { 0.02f, 0.044f, 0 }, { 0, 0.045f, 0 } };
    lathe(pm, c, 6, 32);
}
static void m_grate(Pm *pm) { pm_grate(pm, 0.45f, 0.015f); }

/* A wok burner: the jet ring in a drip pan, a raised cradle (ring 0.35 m). */
static void m_wok_burner(Pm *pm) {
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf pan[7] = { { 0, 0, 0 }, { 0.2f, 0, 0 }, { 0.2f, 0.035f, 1 }, { 0.192f, 0.035f, 1 }, { 0.192f, 0.008f, 1 }, { 0.0f, 0.008f, 0 } };
    lathe(pm, pan, 6, 40);
    pm_push(pm); pm_translate(pm, 0, 0.03f, 0); pm_torus(pm, 0.06f, 0.012f, 32, 8); pm_pop(pm);
    int nj = L0 ? 16 : L2 ? 0 : 8;
    pm_mat(pm, PMAT_BRASS);
    for (int k = 0; k < nj; ++k) {
        float a = 2.0f * PI_F * (float)k / (float)nj;
        pm_push(pm);
        pm_translate(pm, 0.06f * cosf(a), 0.04f, 0.06f * sinf(a));
        PmProf j[5] = { { 0, 0, 0 }, { 0.0055f, 0, 0 }, { 0.0045f, 0.014f, 0 }, { 0.003f, 0.016f, 0 }, { 0, 0.016f, 0 } };
        lathe(pm, j, 5, 8);
        pm_pop(pm);
    }
    PmProf hub[4] = { { 0, 0.008f, 0 }, { 0.028f, 0.008f, 0 }, { 0.024f, 0.05f, 0 }, { 0, 0.05f, 0 } };
    lathe(pm, hub, 4, 20);
    pm_mat(pm, PMAT_CAST_IRON);
    pm_push(pm); pm_translate(pm, 0, 0.18f, 0); pm_torus(pm, 0.175f, 0.012f, 48, 8); pm_pop(pm);
    for (int k = 0; k < 4; ++k) {
        float a = 0.5f * PI_F * (float)k + 0.25f * PI_F, p0[3], p1[3];
        set3(p0, 0.19f * cosf(a), 0.0f, 0.19f * sinf(a)); set3(p1, 0.176f * cosf(a), 0.18f, 0.176f * sinf(a));
        pm_beam(pm, p0, p1, 0.022f, 0.016f, 0.003f, NULL);
        float f0[3], f1[3], f2[3];
        set3(f0, 0.175f * cosf(a + 0.25f * PI_F), 0.18f, 0.175f * sinf(a + 0.25f * PI_F));
        set3(f1, 0.11f * cosf(a + 0.25f * PI_F), 0.17f, 0.11f * sinf(a + 0.25f * PI_F));
        set3(f2, 0.11f * cosf(a + 0.25f * PI_F), 0.205f, 0.11f * sinf(a + 0.25f * PI_F));
        pm_beam(pm, f0, f1, 0.014f, 0.016f, 0.002f, NULL);
        pm_beam(pm, f1, f2, 0.014f, 0.014f, 0.002f, NULL);
    }
}

/* A bamboo steamer stack, 0.30 m: three tiers and a lid. */
static void steamer_tier(Pm *pm, float y) {
    PmProf w[5] = { { 0.142f, y, 1 }, { 0.15f, y, 1 }, { 0.15f, y + 0.07f, 1 }, { 0.142f, y + 0.07f, 1 }, { 0.142f, y, 1 } };
    lathe(pm, w, 5, 40);
    if (!L2) { band(pm, 0.1495f, 0.153f, y + 0.004f, y + 0.016f, 40); band(pm, 0.1495f, 0.153f, y + 0.054f, y + 0.066f, 40); }
    /* the slatted floor is hidden by the tier and lid above; only the bottom one's underside shows */
    if (y == 0.0f) { PmProf f[4] = { { 0, y + 0.009f, 0 }, { 0.142f, y + 0.009f, 0 }, { 0.142f, y + 0.015f, 0 }, { 0, y + 0.015f, 0 } }; lathe(pm, f, 4, 24); }
}
static void m_steamer(Pm *pm) {
    pm_mat(pm, PMAT_BAMBOO);
    for (int k = 0; k < 3; ++k) { pm->part = (uint8_t)k; steamer_tier(pm, 0.066f * (float)k); }
    pm->part = 3;
    float y = 0.198f;
    PmProf l[7] = { { 0, y, 0 }, { 0.152f, y, 0 }, { 0.152f, y + 0.022f, 1 }, { 0.14f, y + 0.03f, 0 }, { 0.09f, y + 0.05f, 0 }, { 0.03f, y + 0.058f, 0 }, { 0, y + 0.059f, 0 } };
    lathe(pm, l, 7, 40);
    pm->part = 0;
}

/* A GN 1/3 hotel pan (325 x 176 over its flange, 150 deep), lidded or open on its curry; three fill a GN 1/1
 * well side by side (NB5: the flange had stood 13 mm past the size). */
static void gn_pan(Pm *pm, int lid) {
    const float rc = 0.02f, hx = 0.1625f - rc - 0.013f, hz = 0.088f - rc - 0.013f;
    int segs = pm_segs(pm, 24, 4);
    pm_mat(pm, PMAT_STAINLESS);
    PmProf o[6] = { { 0, 0, 0 }, { rc - 0.012f, 0, 0 }, { rc - 0.008f, 0.004f, 0 }, { rc, 0.146f, 0 }, { rc + 0.004f, 0.15f, 1 }, { rc + 0.013f, 0.15f, 0 } };
    PmProf s[40];
    int k = pm_prof_shell(o, 6, 0.0012f, 0.0f, 2, s, 40);
    pm_lathe(pm, s, k, segs, hx, hz);
    pm_mat(pm, PMAT_CURRY);
    PmProf c[4] = { { 0, 0.09f, 0 }, { rc - 0.0035f, 0.09f, 0 }, { rc - 0.0025f, 0.122f, 0 }, { 0, 0.122f, 0 } };
    pm_lathe(pm, c, 4, segs, hx, hz);
    if (lid) {
        pm_mat(pm, PMAT_STAINLESS);
        PmProf l[6] = { { 0, 0.1505f, 0 }, { rc + 0.011f, 0.1505f, 0 }, { rc + 0.0125f, 0.153f, 0 }, { rc + 0.006f, 0.157f, 0 }, { 0.0f, 0.158f, 0 } };
        pm_lathe(pm, l, 5, segs, hx, hz);
        float a[3] = { -0.04f, 0.168f, 0 }, b[3] = { 0.04f, 0.168f, 0 };
        pm_beam(pm, a, b, 0.012f, 0.006f, 0.002f, NULL);
        for (int e = -1; e <= 1; e += 2) { float p0[3] = { 0.034f * (float)e, 0.158f, 0 }, p1[3] = { 0.04f * (float)e, 0.168f, 0 }; pm_beam(pm, p0, p1, 0.01f, 0.004f, 0.0f, NULL); }
    }
}
static void m_gn_pan(Pm *pm) { gn_pan(pm, 1); }
static void m_gn_pan_open(Pm *pm) { gn_pan(pm, 0); }

/* A rice cooker, 0.35 m round, 0.30 high. */
static void m_rice_cooker(Pm *pm) {
    pm_mat(pm, PMAT_ENAMEL);
    PmProf b[8] = { { 0, 0.012f, 0 }, { 0.15f, 0.012f, 0 }, { 0.168f, 0.03f, 0 }, { 0.175f, 0.08f, 0 }, { 0.175f, 0.215f, 0 }, { 0.171f, 0.228f, 1 }, { 0.165f, 0.228f, 0 }, { 0, 0.228f, 0 } };
    lathe(pm, b, 8, 48);
    pm_mat(pm, PMAT_STAINLESS);
    PmProf l[7] = { { 0, 0.226f, 0 }, { 0.17f, 0.226f, 0 }, { 0.172f, 0.232f, 0 }, { 0.16f, 0.25f, 0 }, { 0.11f, 0.282f, 0 }, { 0.04f, 0.295f, 0 }, { 0, 0.296f, 0 } };
    lathe(pm, l, 7, 48);
    pm_mat(pm, PMAT_PLASTIC);
    PmProf k[5] = { { 0, 0.29f, 0 }, { 0.022f, 0.29f, 0 }, { 0.02f, 0.306f, 0 }, { 0.012f, 0.312f, 0 }, { 0, 0.313f, 0 } };
    lathe(pm, k, 5, 16);
    for (int s = -1; s <= 1; s += 2) {
        float c[3] = { 0.183f * (float)s, 0.19f, 0 }, h[3] = { 0.012f, 0.012f, 0.045f };
        pm_bbox(pm, c, h, 0.005f, 2);
    }
    for (int f = 0; f < 3; ++f) {
        float a = 2.0f * PI_F * (float)f / 3.0f;
        pm_push(pm); pm_translate(pm, 0.12f * cosf(a), 0, 0.12f * sinf(a));
        PmProf ft[4] = { { 0, 0, 0 }, { 0.015f, 0, 0 }, { 0.013f, 0.013f, 0 }, { 0, 0.013f, 0 } };
        lathe(pm, ft, 4, 10);
        pm_pop(pm);
    }
    /* the front: switch and pilot lamp */
    float sc[3] = { 0, 0.07f, 0.172f }, sh[3] = { 0.03f, 0.022f, 0.012f };
    pm_bbox(pm, sc, sh, 0.004f, 2);
    pm_mat(pm, PMAT_RED);
    pm_push(pm); pm_translate(pm, 0.0f, 0.11f, 0.172f); pm_rotate_x(pm, 0.5f * PI_F);
    PmProf lp[4] = { { 0, 0, 0 }, { 0.006f, 0, 0 }, { 0.006f, 0.007f, 0 }, { 0, 0.009f, 0 } };
    lathe(pm, lp, 4, 12);
    pm_pop(pm);
}

/* --------------------------------------------------------- utensils --- */
/* A wire hook bent over the rail at the origin, its eye below. */
static void hang_hook(Pm *pm, float drop) {
    PmPath pa;
    pm_path_init(&pa, 24);
    float c[3] = { 0, 0, 0 }, ex[3] = { 0, 0, 1 }, ey[3] = { 0, 1, 0 };
    pm_path_arc(&pa, c, ex, ey, 0.011f, -0.3f, PI_F, L0 ? 10 : 4);
    pm_path_add(&pa, 0, -drop, -0.011f, 0);
    pm_tube(pm, &pa, 0.0022f, 8, 3);
    pm_path_free(&pa);
}

/* A ladle hanging by its hooked handle, the bowl (90 mm) at the bottom, opening out (+z). */
static void m_ladle(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    PmPath pa;
    pm_path_init(&pa, 32);
    float c[3] = { 0, 0, 0 }, ex[3] = { 0, 0, 1 }, ey[3] = { 0, 1, 0 };
    pm_path_arc(&pa, c, ex, ey, 0.014f, -0.2f, PI_F, L0 ? 10 : 4);
    pm_path_add(&pa, 0, -0.06f, -0.014f, 0);
    pm_path_add(&pa, 0, -0.33f, -0.014f, 0);
    pm_path_add(&pa, 0, -0.355f, -0.004f, 0);
    PmSect s;
    pm_sect_rect(&s, 0.012f, 0.003f, 0.0008f);
    PmPath f;
    pm_path_init(&f, 64);
    pm_path_fillet(&pa, 0.02f, L0 ? 6 : 2, &f);
    float up[3] = { 1, 0, 0 };
    pm_sweep(pm, &f, &s, up, 3);
    pm_path_free(&f);
    pm_path_free(&pa);
    pm_push(pm);
    pm_translate(pm, 0, -0.4f, 0.0f);
    pm_rotate_x(pm, 0.5f * PI_F);   /* the bowl's opening (+y) out to +z */
    pm_translate(pm, 0, -0.045f, 0);
    PmProf o[6] = { { 0, 0, 0 }, { 0.02f, 0.002f, 0 }, { 0.034f, 0.01f, 0 }, { 0.042f, 0.022f, 0 }, { 0.045f, 0.035f, 0 }, { 0.045f, 0.045f, 0 } };
    shell(pm, o, 6, 0.0015f, 0.002f, 28);
    pm_pop(pm);
}

/* A spider (wire skimmer): a bamboo handle hung from its loop, the basket below facing out. */
static void m_spider(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    hang_hook(pm, 0.01f);
    pm_mat(pm, PMAT_BAMBOO);
    PmProf h[5] = { { 0, -0.29f, 0 }, { 0.011f, -0.29f, 0 }, { 0.0125f, -0.03f, 0 }, { 0.011f, -0.018f, 0 }, { 0, -0.018f, 0 } };
    lathe(pm, h, 5, 12);
    pm_mat(pm, PMAT_BRASS);
    band(pm, 0.0, 0.0135f, -0.31f, -0.285f, 12);
    pm_mat(pm, PMAT_STEEL);
    /* the twisted wire shank and the basket */
    float a[3] = { 0.003f, -0.31f, 0 }, b[3] = { 0.003f, -0.38f, 0.0f };
    float a2[3] = { -0.003f, -0.31f, 0 }, b2[3] = { -0.003f, -0.38f, 0.0f };
    pm_beam(pm, a, b, 0.003f, 0.003f, 0.0f, NULL);
    pm_beam(pm, a2, b2, 0.003f, 0.003f, 0.0f, NULL);
    pm_push(pm);
    pm_translate(pm, 0, -0.46f, 0.0f);
    pm_rotate_x(pm, 0.5f * PI_F);
    int rings = L0 ? 5 : L2 ? 1 : 3;
    for (int k = 1; k <= rings; ++k) {
        float t = (float)k / (float)rings, r = 0.08f * t, y = -0.03f * (1.0f - t * t);
        pm_push(pm); pm_translate(pm, 0, y, 0); pm_torus(pm, r, k == rings ? 0.0025f : 0.0012f, 32, 4); pm_pop(pm);
    }
    int sp = L0 ? 12 : L2 ? 0 : 6;
    for (int k = 0; k < sp; ++k) {
        float ang = 2.0f * PI_F * (float)k / (float)sp;
        PmPath pa;
        pm_path_init(&pa, 12);
        for (int j = 0; j <= 6; ++j) { float t = (float)j / 6.0f, r = 0.08f * t; pm_path_add(&pa, r * cosf(ang), -0.03f * (1.0f - t * t), r * sinf(ang), 0); }
        pm_tube(pm, &pa, 0.0011f, 4, 3);
        pm_path_free(&pa);
    }
    pm_pop(pm);
}

/* A slotted spatula hung from its hole; a riveted timber handle. */
static void m_spatula(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    hang_hook(pm, 0.015f);
    pm_mat(pm, PMAT_TIMBER);
    float hc[3] = { 0, -0.11f, 0 }, hh[3] = { 0.012f, 0.085f, 0.009f };
    pm_bbox(pm, hc, hh, 0.004f, 2);
    if (L0) for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, 0, -0.07f - 0.07f * (float)k, 0.009f); pm_rotate_x(pm, 0.5f * PI_F); pm_mat(pm, PMAT_BRASS); m_rivet(pm); pm_pop(pm); }
    pm_mat(pm, PMAT_STAINLESS);
    const float zup[3] = { 0, 0, 1 };
    float a[3] = { 0, -0.19f, 0 }, b[3] = { 0, -0.28f, 0.012f };
    pm_beam(pm, a, b, 0.016f, 0.003f, 0.0f, zup);
    /* the blade: tines between slots, with its top and its edge */
    float y0 = -0.28f, y1 = -0.37f, w = 0.1f;
    float top0[3] = { -0.5f * w, y0, 0.012f }, top1[3] = { 0.5f * w, y0, 0.012f };
    pm_beam(pm, top0, top1, 0.012f, 0.002f, 0.0f, zup);
    float e0[3] = { -0.5f * w, y1, 0.012f }, e1[3] = { 0.5f * w, y1, 0.012f };
    pm_beam(pm, e0, e1, 0.012f, 0.002f, 0.0f, zup);
    int nt = L2 ? 1 : 6;
    for (int k = 0; k < nt; ++k) {
        float x = nt == 1 ? 0.0f : -0.5f * w + 0.005f + (w - 0.01f) * (float)k / (float)(nt - 1);
        float p0[3] = { x, y0, 0.012f }, p1[3] = { x, y1, 0.012f };
        pm_beam(pm, p0, p1, nt == 1 ? w : 0.009f, 0.002f, 0.0f, zup);
    }
}

/* A cleaver hung by the hole in its blade's corner. */
static void m_cleaver(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    hang_hook(pm, 0.012f);
    pm_mat(pm, PMAT_STAINLESS);
    float poly[6][2] = { { -0.02f, -0.012f }, { -0.02f, -0.1f }, { 0.185f, -0.104f }, { 0.19f, -0.015f }, { 0.17f, -0.008f }, { 0.0f, -0.008f } };
    pm_extrude(pm, (const float (*)[2])poly, 6, -0.0015f, 0.0015f);
    pm_mat(pm, PMAT_TIMBER);
    float a[3] = { 0.185f, -0.03f, 0 }, b[3] = { 0.3f, -0.026f, 0 };
    pm_beam(pm, a, b, 0.016f, 0.024f, 0.004f, NULL);
    if (L0)
        for (int k = 0; k < 3; ++k)
            for (int s = -1; s <= 1; s += 2) {
                pm_push(pm); pm_translate(pm, 0.205f + 0.035f * (float)k, -0.029f, 0.008f * (float)s); pm_rotate_x(pm, 0.5f * PI_F * (float)s);
                pm_mat(pm, PMAT_BRASS); m_rivet(pm); pm_pop(pm);
            }
}

/* An S hook: its top over the rail at the origin. */
static void m_s_hook(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    PmPath pa;
    pm_path_init(&pa, 32);
    float c0[3] = { 0, 0, 0 }, c1[3] = { 0, -0.05f, 0 }, ex[3] = { 0, 0, 1 }, ey[3] = { 0, 1, 0 };
    int n = L0 ? 10 : 4;
    pm_path_arc(&pa, c0, ex, ey, 0.011f, -0.2f, PI_F, n);
    pa.n--;   /* join the arcs with the straight between them */
    PmPath b;
    pm_path_init(&b, 16);
    pm_path_arc(&b, c1, ex, ey, 0.011f, 0.0f, -PI_F - 0.2f, n);
    for (int k = 0; k < b.n; ++k) pm_path_add(&pa, b.p[k][0], b.p[k][1], b.p[k][2], 0);
    pm_tube(pm, &pa, 0.0025f, 8, 3);
    pm_path_free(&b);
    pm_path_free(&pa);
}

/* A utensil rail, 0.6 m along x (NB5: it hangs on a side wall by the range), 45 mm off the wall (z 0), on two
 * stand-offs. */
static void m_utensil_rail(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    PmPath pa;
    pm_path_init(&pa, 2);
    pm_path_add(&pa, -0.3f, 0, 0.045f, 0);
    pm_path_add(&pa, 0.3f, 0, 0.045f, 0);
    pm_tube(pm, &pa, 0.008f, 14, 3);
    pm_path_free(&pa);
    for (int s = -1; s <= 1; s += 2) {
        pm_push(pm);
        pm_translate(pm, 0.26f * (float)s, 0, 0);
        pm_rotate_x(pm, 0.5f * PI_F);
        PmProf p[7] = { { 0, 0, 0 }, { 0.025f, 0, 0 }, { 0.025f, 0.004f, 0 }, { 0.01f, 0.008f, 0 }, { 0.008f, 0.012f, 0 }, { 0.008f, 0.046f, 0 }, { 0, 0.046f, 0 } };
        lathe(pm, p, 7, 16);
        if (L0) for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, 0.017f * (k ? 1.0f : -1.0f), 0.004f, 0); pm_place(pm, PMK_BOLT); pm_pop(pm); }
        pm_pop(pm);
    }
}

/* ------------------------------------------------------- gas, fan, power --- */
static void gas_bottle(Pm *pm, float R, float H) {
    pm_mat(pm, PMAT_RED);
    /* the foot ring */
    PmProf f[5] = { { R * 0.86f, 0, 1 }, { R * 0.9f, 0, 1 }, { R * 0.9f, 0.06f, 1 }, { R * 0.86f, 0.06f, 1 }, { R * 0.86f, 0, 1 } };
    lathe(pm, f, 5, 40);
    /* the body: a cylinder with ellipsoidal ends */
    PmProf b[16];
    int n = 0;
    int e = L0 ? 6 : 3;
    for (int k = 0; k <= e; ++k) { float a = -0.5f * PI_F + 0.5f * PI_F * (float)k / (float)e; b[n].r = R * cosf(a); b[n].y = 0.035f + 0.45f * R + 0.45f * R * sinf(a); b[n].hard = 0; n++; }
    for (int k = 0; k <= e; ++k) { float a = 0.5f * PI_F * (float)k / (float)e; b[n].r = R * cosf(a); b[n].y = H - 0.12f - 0.45f * R + 0.45f * R * sinf(a); b[n].hard = 0; n++; }
    lathe(pm, b, n, 40);
    if (!L2) { band(pm, R - 0.001f, R + 0.002f, 0.035f + 0.45f * R - 0.01f, 0.035f + 0.45f * R + 0.01f, 40); }
    /* the shroud round the valve */
    float ys = H - 0.12f - 0.08f;
    PmProf sh[6] = { { 0.085f, ys, 1 }, { 0.09f, ys, 1 }, { 0.09f, H - 0.005f, 1 }, { 0.083f, H, 0 }, { 0.083f, ys + 0.02f, 1 }, { 0.085f, ys, 1 } };
    lathe(pm, sh, 6, 32);
    /* the valve */
    pm_mat(pm, PMAT_BRASS);
    float yv = H - 0.12f;
    PmProf v[6] = { { 0, yv - 0.01f, 0 }, { 0.016f, yv - 0.01f, 0 }, { 0.016f, yv + 0.035f, 0 }, { 0.012f, yv + 0.04f, 0 }, { 0.008f, yv + 0.055f, 0 }, { 0, yv + 0.055f, 0 } };
    lathe(pm, v, 6, 16);
    pm_push(pm); pm_translate(pm, 0, yv + 0.025f, 0); pm_rotate_z(pm, -0.5f * PI_F);
    PmProf o[4] = { { 0, 0, 0 }, { 0.007f, 0, 0 }, { 0.007f, 0.04f, 0 }, { 0, 0.04f, 0 } };
    lathe(pm, o, 4, 10);
    pm_pop(pm);
    pm_mat(pm, PMAT_RED);
    pm_push(pm); pm_translate(pm, 0, yv + 0.058f, 0); pm_torus(pm, 0.026f, 0.004f, 20, 6);
    for (int k = 0; k < 3; ++k) { float a = 2.0f * PI_F * (float)k / 3.0f, p0[3] = { 0, 0, 0 }, p1[3] = { 0.025f * cosf(a), 0, 0.025f * sinf(a) }; pm_beam(pm, p0, p1, 0.005f, 0.004f, 0.0f, NULL); }
    pm_pop(pm);
}
static void m_gas_13(Pm *pm) { gas_bottle(pm, 0.155f, 0.6f); }
static void m_gas_47(Pm *pm) { gas_bottle(pm, 0.185f, 1.3f); }

/* An extractor fan, 0.35 m, in its housing on a wall (z 0), facing +z behind a grille. */
static void m_fan(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    float R = 0.175f;
    pm_push(pm);
    pm_rotate_x(pm, 0.5f * PI_F);   /* the lathe's axis (+y) out of the wall (+z) */
    PmProf h[8] = { { R + 0.01f, 0, 1 }, { R + 0.04f, 0, 1 }, { R + 0.04f, 0.008f, 1 }, { R + 0.01f, 0.012f, 1 }, { R + 0.01f, 0.14f, 1 }, { R + 0.002f, 0.14f, 1 }, { R + 0.002f, 0.0f, 1 }, { R + 0.01f, 0, 1 } };
    lathe(pm, h, 8, 48);
    pm_mat(pm, PMAT_STEEL);
    PmProf mo[6] = { { 0, 0.0f, 0 }, { 0.06f, 0.0f, 0 }, { 0.06f, 0.06f, 0 }, { 0.04f, 0.075f, 0 }, { 0.02f, 0.085f, 0 }, { 0, 0.086f, 0 } };
    lathe(pm, mo, 6, 24);
    /* the blades round the hub, pitched */
    int nb = 5;
    for (int k = 0; k < nb; ++k) {
        pm_push(pm);
        pm_translate(pm, 0, 0.095f, 0);
        pm_rotate_y(pm, 2.0f * PI_F * (float)k / (float)nb);
        pm_translate(pm, 0.1f, 0, 0);
        pm_rotate_x(pm, 0.45f);
        float c[3] = { 0, 0, 0 }, hh[3] = { 0.07f, 0.0015f, 0.045f };
        pm_bbox(pm, c, hh, 0.0012f, 1);
        pm_pop(pm);
    }
    PmProf hub[5] = { { 0, 0.085f, 0 }, { 0.035f, 0.085f, 0 }, { 0.035f, 0.105f, 0 }, { 0.02f, 0.115f, 0 }, { 0, 0.118f, 0 } };
    lathe(pm, hub, 5, 20);
    pm_pop(pm);
    pm_mat(pm, PMAT_STAINLESS);
    pm_push(pm);
    pm_translate(pm, 0, 0, 0.143f);
    pm_grille_round(pm, R + 0.005f, 0.003f, L0 ? 5 : L2 ? 2 : 3, L0 ? 8 : 4);
    pm_pop(pm);
}

/* A power panel: a riveted box 0.4 x 0.6 x 0.15 on a wall (z 0, its foot at y 0), a door, switches,
 * insulators on a cross arm, glands underneath, a warning card. */
static void m_power_panel(Pm *pm) {
    const float W = 0.4f, H = 0.6f, D = 0.15f;
    pm_mat(pm, PMAT_STAINLESS);   /* NB4b: in the kitchen, so stainless */
    float c[3] = { 0, 0.5f * H, 0.5f * D - 0.006f }, h[3] = { 0.5f * W, 0.5f * H, 0.5f * D - 0.006f };
    pm_bbox(pm, c, h, 0.006f, 2);
    /* the door, proud of the front, and its frame */
    float dc[3] = { 0, 0.5f * H, D - 0.006f }, dh[3] = { 0.5f * W - 0.012f, 0.5f * H - 0.012f, 0.007f };
    pm_bbox(pm, dc, dh, 0.004f, 2);
    float n[3] = { 0, 0, 1 };
    if (pm->inst || L0) {
        float y0 = 0.03f, y1 = H - 0.03f, x0 = -0.5f * W + 0.03f, x1 = 0.5f * W - 0.03f, z = D + 0.001f;
        float a[3] = { x0, y0, z }, b[3] = { x1, y0, z }, cc[3] = { x1, y1, z }, d[3] = { x0, y1, z };
        pm_rivet_row(pm, a, b, n, 0.06f); pm_rivet_row(pm, d, cc, n, 0.06f);
        pm_rivet_row(pm, a, d, n, 0.06f); pm_rivet_row(pm, b, cc, n, 0.06f);
    }
    /* hinges and a handle */
    pm_mat(pm, PMAT_STEEL);
    for (int k = 0; k < 2; ++k) {
        pm_push(pm); pm_translate(pm, -0.5f * W + 0.004f, 0.12f + 0.36f * (float)k, D + 0.004f);
        PmProf hp[4] = { { 0, -0.03f, 0 }, { 0.007f, -0.03f, 0 }, { 0.007f, 0.03f, 0 }, { 0, 0.03f, 0 } };
        lathe(pm, hp, 4, 10);
        pm_pop(pm);
    }
    {
        float a[3] = { 0.5f * W - 0.05f, 0.24f, D + 0.025f }, b[3] = { 0.5f * W - 0.05f, 0.36f, D + 0.025f };
        pm_beam(pm, a, b, 0.012f, 0.012f, 0.003f, NULL);
        for (int k = 0; k < 2; ++k) { float p0[3] = { 0.5f * W - 0.05f, 0.25f + 0.1f * (float)k, D + 0.001f }, p1[3] = { 0.5f * W - 0.05f, 0.25f + 0.1f * (float)k, D + 0.025f }; pm_beam(pm, p0, p1, 0.01f, 0.01f, 0.0f, NULL); }
    }
    /* four toggles on the door, a warning card and the hand-written circuit card */
    for (int k = 0; k < 4; ++k) {
        pm_push(pm); pm_translate(pm, -0.11f + 0.07f * (float)k, 0.42f, D + 0.001f); pm_rotate_x(pm, 0.5f * PI_F);
        pm_place(pm, PMK_TOGGLE);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_PAPER);
    {
        float tri[3][2] = { { -0.05f, 0.0f }, { 0.05f, 0.0f }, { 0.0f, 0.087f } };
        pm_push(pm); pm_translate(pm, -0.05f, 0.22f, D + 0.001f);
        pm->part = 1;
        pm_extrude(pm, (const float (*)[2])tri, 3, 0.0f, 0.0015f);
        pm_pop(pm);
        pm->part = 2;
        float cc2[3] = { 0.06f, 0.15f, D + 0.0018f }, ch[3] = { 0.06f, 0.04f, 0.0008f };
        pm_bbox(pm, cc2, ch, 0.0f, 1);
        pm->part = 0;
    }
    /* the cross arm and its insulators on top */
    pm_mat(pm, PMAT_STEEL);
    {
        float a[3] = { -0.22f, H + 0.02f, 0.06f }, b[3] = { 0.22f, H + 0.02f, 0.06f };
        pm_beam(pm, a, b, 0.04f, 0.04f, 0.0f, NULL);
        for (int s = -1; s <= 1; s += 2) { float p0[3] = { 0.15f * (float)s, H, 0.01f }, p1[3] = { 0.15f * (float)s, H + 0.02f, 0.06f }; pm_beam(pm, p0, p1, 0.03f, 0.006f, 0.0f, NULL); }
        for (int k = 0; k < 3; ++k) { pm_push(pm); pm_translate(pm, -0.15f + 0.15f * (float)k, H + 0.04f, 0.06f); pm_place(pm, PMK_INSULATOR); pm_pop(pm); }
    }
    /* glands underneath, where the cables leave */
    pm_mat(pm, PMAT_PLASTIC);
    for (int k = 0; k < 3; ++k) {
        pm_push(pm); pm_translate(pm, -0.12f + 0.12f * (float)k, 0.0f, 0.07f); pm_rotate_x(pm, PI_F);
        PmProf g[6] = { { 0, 0, 0 }, { 0.016f, 0, 1 }, { 0.016f, 0.008f, 1 }, { 0.012f, 0.012f, 0 }, { 0.011f, 0.03f, 0 }, { 0, 0.03f, 0 } };
        lathe(pm, g, 6, 16);
        pm_pop(pm);
    }
}

/* A shelf bracket on a wall (z 0): a gusset 0.28 deep, its top at y 0. */
static void m_shelf_bracket(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    float poly[5][2] = { { 0.0f, 0.0f }, { 0.0f, -0.25f }, { 0.03f, -0.25f }, { 0.28f, -0.03f }, { 0.28f, 0.0f } };
    pm_push(pm);
    pm_rotate_y(pm, -0.5f * PI_F);   /* the polygon's x (out) onto +z */
    pm_extrude(pm, (const float (*)[2])poly, 5, -0.003f, 0.003f);
    pm_pop(pm);
    /* flanges along the wall and under the shelf */
    float a[3] = { 0, -0.25f, 0.002f }, b[3] = { 0, 0.0f, 0.002f }, up[3] = { 0, 0, 1 };
    pm_beam(pm, a, b, 0.03f, 0.004f, 0.0f, up);
    float c[3] = { 0, -0.002f, 0.0f }, d[3] = { 0, -0.002f, 0.28f };
    pm_beam(pm, c, d, 0.03f, 0.004f, 0.0f, NULL);
    if (!L2) for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, 0, -0.05f - 0.15f * (float)k, 0.004f); pm_rotate_x(pm, 0.5f * PI_F); pm_place(pm, PMK_BOLT); pm_pop(pm); }
}

/* A menu A-frame, 0.6 x 0.9: two chalk boards in timber frames, hinged at the top, a chain between. */
static void m_a_frame(Pm *pm) {
    const float W = 0.6f, H = 0.9f, splay = 0.2f;
    for (int s = -1; s <= 1; s += 2) {
        pm_push(pm);
        pm_translate(pm, 0, H * cosf(splay), 0);
        pm_rotate_x(pm, (float)s * splay);
        pm_translate(pm, 0, -H, 0);
        pm_mat(pm, PMAT_PAINTED);   /* NB4b: its frame in the bar's trim coat, or bare timber (kit.glsl KIT_BARE_WOOD) */
        float fw = 0.035f, ft = 0.02f;
        float c[4][3] = { { -0.5f * W, 0, 0 }, { 0.5f * W, 0, 0 }, { 0.5f * W, H, 0 }, { -0.5f * W, H, 0 } };
        for (int k = 0; k < 4; ++k) {
            float a[3], b[3], d[3];
            for (int i = 0; i < 3; ++i) { a[i] = c[k][i]; b[i] = c[(k + 1) % 4][i]; d[i] = b[i] - a[i]; }
            float l = sqrtf(d[0] * d[0] + d[1] * d[1]);
            for (int i = 0; i < 2; ++i) { a[i] += (k & 1 ? 0 : 0) ; d[i] /= l; }
            /* inset the bars' centrelines by half their width */
            float in[4][2] = { { 0, 0.5f * fw }, { -0.5f * fw, 0 }, { 0, -0.5f * fw }, { 0.5f * fw, 0 } };
            a[0] += in[k][0] - d[0] * 0.5f * fw; a[1] += in[k][1] - d[1] * 0.5f * fw;
            b[0] += in[k][0] + d[0] * 0.5f * fw; b[1] += in[k][1] + d[1] * 0.5f * fw;
            float up[3] = { 0, 0, 1 };
            pm_beam(pm, a, b, fw, ft, 0.003f, up);
        }
        pm_mat(pm, PMAT_CHALK);
        float bc[3] = { 0, 0.5f * H, 0 }, bh[3] = { 0.5f * W - fw + 0.004f, 0.5f * H - fw + 0.004f, 0.005f };
        pm_bbox(pm, bc, bh, 0.0f, 1);
        pm_pop(pm);
    }
    pm_mat(pm, PMAT_STEEL);
    for (int s = -1; s <= 1; s += 2) {
        pm_push(pm); pm_translate(pm, 0.25f * (float)s, H * cosf(splay), 0); pm_rotate_z(pm, 0.5f * PI_F);
        PmProf hp[4] = { { 0, -0.03f, 0 }, { 0.006f, -0.03f, 0 }, { 0.006f, 0.03f, 0 }, { 0, 0.03f, 0 } };
        lathe(pm, hp, 4, 10);
        pm_pop(pm);
    }
    if (!L2) {
        /* the chain: links along the span between the boards at a third of the height */
        float y = H * cosf(splay) * 0.35f, z = (H - H * 0.35f) * sinf(splay);
        int nl = L0 ? 9 : 3;
        for (int k = 0; k < nl; ++k) {
            float t = ((float)k + 0.5f) / (float)nl, zz = lerpf(-z, z, t), sag = 0.03f * 4.0f * t * (1.0f - t);
            pm_push(pm); pm_translate(pm, 0.28f, y - sag, zz);
            if (k & 1) pm_rotate_z(pm, 0.5f * PI_F);
            pm_rotate_x(pm, 0.5f * PI_F);
            pm_scale(pm, 1.0f, 1.0f, 1.6f);
            pm_torus(pm, 0.008f, 0.0018f, 12, 4);
            pm_pop(pm);
        }
    }
}

/* ------------------------------------------------ the cooking line (NB5) --- */
/* A carbon-steel wok, 0.38 m: a spherical bowl (its bottom at the origin), a long hollow handle out +z with a
 * timber grip, a helper ear at -z. Seasoned black (cast iron's finish). */
static void m_wok(Pm *pm) {
    const float S = 0.26f, R = 0.19f, a1 = asinf(R / S), hr = S - sqrtf(S * S - R * R);
    pm_mat(pm, PMAT_CAST_IRON);
    PmProf o[12];
    int e = L0 ? 9 : L2 ? 3 : 5, n = 0;
    for (int k = 0; k <= e; ++k) { float a = a1 * (float)k / (float)e; o[n].r = S * sinf(a); o[n].y = S - S * cosf(a); o[n].hard = 0; n++; }
    shell(pm, o, n, 0.0022f, 0.003f, 48);
    /* the handle: a socket riveted under the rim, the tube rising out, the grip */
    pm_mat(pm, PMAT_STEEL);
    float s0[3] = { 0, hr - 0.035f, R - 0.012f }, s1[3] = { 0, hr - 0.012f, R + 0.06f };
    pm_beam(pm, s0, s1, 0.03f, 0.018f, 0.004f, NULL);
    PmPath pa;
    pm_path_init(&pa, 4);
    pm_path_add(&pa, 0, hr - 0.014f, R + 0.05f, 0);
    pm_path_add(&pa, 0, hr + 0.035f, R + 0.17f, 0);
    pm_tube(pm, &pa, 0.0105f, 12, 3);
    pm_mat(pm, PMAT_TIMBER);
    pa.n = 0;
    pm_path_add(&pa, 0, hr + 0.03f, R + 0.155f, 0);
    pm_path_add(&pa, 0, hr + 0.085f, R + 0.31f, 0);
    pm_tube(pm, &pa, 0.0155f, 12, 3);
    pm_path_free(&pa);
    if (L0) for (int k = -1; k <= 1; k += 2) { pm_push(pm); pm_translate(pm, 0.012f * (float)k, hr - 0.024f, R + 0.02f); pm_rotate_z(pm, -0.5f * PI_F * (float)k); m_rivet(pm); pm_pop(pm); }
    /* the ear: a loop of flat bar riveted under the rim at -z */
    pm_mat(pm, PMAT_STEEL);
    PmPath ear;
    pm_path_init(&ear, 16);
    int ne = L0 ? 8 : 3;
    for (int k = 0; k <= ne; ++k) { float t = PI_F * (float)k / (float)ne; pm_path_add(&ear, 0.035f * cosf(t), hr - 0.012f, -(R - 0.004f) - 0.04f * sinf(t), 0); }
    PmSect sc;
    pm_sect_rect(&sc, 0.012f, 0.004f, 0.0f);
    pm_sweep(pm, &ear, &sc, NULL, 3);
    pm_path_free(&ear);
}

/* A noodle basket: a wire cup 0.13 m round and 0.16 deep, its rim at the origin, the handle out +z to a hook
 * that sits on the pot's rim; noodles inside, their top above the broth. */
static void m_noodle_basket(Pm *pm) {
    const float r = 0.064f, d = 0.16f;
    pm_mat(pm, PMAT_STAINLESS);
    if (L2) {
        PmProf c[6] = { { 0, -d, 0 }, { r, -d, 1 }, { r, 0.0f, 1 }, { r - 0.004f, 0.0f, 1 }, { r - 0.004f, -d + 0.004f, 1 }, { 0, -d + 0.004f, 0 } };
        lathe(pm, c, 6, 36);   /* 12 sides at L2: 4 let the noodles through */
    } else {
        pm_torus(pm, r, 0.0028f, L0 ? 32 : 16, 6);
        int nr = L0 ? 4 : 2;
        for (int k = 1; k <= nr; ++k) { pm_push(pm); pm_translate(pm, 0, -d * (float)k / (float)nr, 0); pm_torus(pm, r, 0.0012f, L0 ? 32 : 16, 4); pm_pop(pm); }
        int nw = L0 ? 16 : 8;
        for (int k = 0; k < nw; ++k) {
            float a = 2.0f * PI_F * (float)k / (float)nw, p0[3] = { r * cosf(a), 0.0f, r * sinf(a) }, p1[3] = { r * cosf(a), -d, r * sinf(a) };
            pm_beam(pm, p0, p1, 0.0018f, 0.0018f, 0.0f, NULL);
        }
        pm_push(pm); pm_translate(pm, 0, -d, 0); pm_rotate_x(pm, -0.5f * PI_F); pm_grille_round(pm, r, 0.0011f, L0 ? 3 : 1, L0 ? 8 : 4); pm_pop(pm);
    }
    /* the handle: two wires twisted out and up from the rim, a timber grip, the hook down onto the rim */
    pm_mat(pm, PMAT_STEEL);
    PmPath pa;
    pm_path_init(&pa, 8);
    pm_path_add(&pa, 0, -0.01f, r, 0);
    pm_path_add(&pa, 0, 0.02f, r + 0.07f, 0);
    pm_path_add(&pa, 0, 0.06f, r + 0.15f, 0);
    pm_tube(pm, &pa, 0.0032f, 8, 3);
    pm_path_free(&pa);
    pm_mat(pm, PMAT_TIMBER);
    PmPath g;
    pm_path_init(&g, 2);
    pm_path_add(&g, 0, 0.055f, r + 0.14f, 0);
    pm_path_add(&g, 0, 0.09f, r + 0.23f, 0);
    pm_tube(pm, &g, 0.011f, 10, 3);
    pm_path_free(&g);
    pm_mat(pm, PMAT_STEEL);
    PmPath h;
    pm_path_init(&h, 8);
    pm_path_add(&h, 0, 0.012f, r + 0.045f, 0);
    pm_path_add(&h, 0, 0.022f, r + 0.07f, 0);
    pm_path_add(&h, 0, 0.0f, r + 0.095f, 0);
    pm_tube(pm, &h, 0.0025f, 6, 3);
    pm_path_free(&h);
    /* the noodles, a loose mound */
    pm_mat(pm, PMAT_NOODLE);
    PmProf nd[6] = { { 0, -d + 0.01f, 0 }, { r - 0.006f, -d + 0.01f, 0 }, { r - 0.005f, -0.07f, 0 }, { r - 0.016f, -0.04f, 0 }, { 0.025f, -0.028f, 0 }, { 0, -0.024f, 0 } };
    lathe(pm, nd, 6, L2 ? 36 : 20);
}

/* A copper saucepan, 0.20 m round and 0.10 high, a long riveted handle out +z; broth inside. */
static void m_saucepan(Pm *pm) {
    const float R = 0.10f, H = 0.10f;
    pm_mat(pm, PMAT_COPPER);
    PmProf o[5] = { { 0, 0, 0 }, { R - 0.01f, 0, 0 }, { R - 0.002f, 0.004f, 0 }, { R, 0.012f, 0 }, { R, H, 0 } };
    shell(pm, o, 5, 0.0022f, 0.003f, 40);
    pm_mat(pm, PMAT_BRASS);
    float a[3] = { 0, H - 0.03f, R - 0.004f }, b[3] = { 0, H + 0.01f, R + 0.06f };
    pm_beam(pm, a, b, 0.026f, 0.012f, 0.003f, NULL);
    PmPath pa;
    pm_path_init(&pa, 4);
    pm_path_add(&pa, 0, H + 0.005f, R + 0.05f, 0);
    pm_path_add(&pa, 0, H + 0.04f, R + 0.2f, 0);
    pm_tube(pm, &pa, 0.008f, 10, 3);
    pm_path_free(&pa);
    if (L0) for (int k = -1; k <= 1; k += 2) { pm_push(pm); pm_translate(pm, 0.008f * (float)k, H - 0.02f, R + 0.002f); pm_rotate_x(pm, 0.5f * PI_F); m_rivet(pm); pm_pop(pm); }
    pm_mat(pm, PMAT_BROTH);
    PmProf br[4] = { { 0, 0.62f * H, 0 }, { R - 0.003f, 0.62f * H, 0 }, { R - 0.003f, 0.65f * H, 0 }, { 0, 0.65f * H, 0 } };
    lathe(pm, br, 4, 32);
}

/* A round chopping block, a slice of trunk 0.34 m across and 70 mm thick, its edge rough, a steel band. */
static void m_chop_block(Pm *pm) {
    pm_mat(pm, PMAT_TIMBER);
    PmProf p[7] = { { 0, 0, 0 }, { 0.163f, 0, 0 }, { 0.17f, 0.006f, 1 }, { 0.172f, 0.035f, 0 }, { 0.168f, 0.065f, 1 }, { 0.16f, 0.07f, 0 }, { 0, 0.071f, 0 } };
    lathe(pm, p, 7, 40);
    if (!L2) { pm_mat(pm, PMAT_STEEL); band(pm, 0.1718f, 0.1738f, 0.02f, 0.04f, 40); }
}

/* A steel tub of bamboo chopsticks, 85 mm round, 0.11 high. */
static void m_chopsticks(Pm *pm) {
    pm_mat(pm, PMAT_STAINLESS);
    PmProf o[4] = { { 0, 0, 0 }, { 0.04f, 0, 0 }, { 0.0425f, 0.004f, 0 }, { 0.0425f, 0.11f, 0 } };
    shell(pm, o, 4, 0.0012f, 0.0015f, 24);
    pm_mat(pm, PMAT_BAMBOO);
    if (L2) { PmProf t[4] = { { 0, 0.09f, 0 }, { 0.038f, 0.09f, 0 }, { 0.05f, 0.21f, 0 }, { 0, 0.21f, 0 } }; lathe(pm, t, 4, 8); return; }
    int n = L0 ? 18 : 8;
    uint32_t h = 0x9E3779B9u;
    for (int k = 0; k < n; ++k) {
        h = h * 1664525u + 1013904223u;
        float a = 2.0f * PI_F * (float)k / (float)n + 0.3f * (float)(h >> 24) / 255.0f;
        float rr = 0.012f + 0.022f * (float)((h >> 16) & 255u) / 255.0f, lean = 0.06f + 0.05f * (float)((h >> 8) & 255u) / 255.0f;
        float p0[3] = { rr * cosf(a), 0.003f, rr * sinf(a) };
        float top = 0.2f + 0.035f * (float)(h & 255u) / 255.0f;
        float p1[3] = { p0[0] + lean * cosf(a) * top, top, p0[2] + lean * sinf(a) * top };
        pm_beam(pm, p0, p1, 0.006f, 0.006f, 0.0f, NULL);
    }
}

/* A socket strip on a wall (z 0, out +z): a black box 0.30 x 0.05, three sockets, a lit switch, screws. */
static void m_power_strip(Pm *pm) {
    pm_mat(pm, PMAT_PLASTIC);
    float c[3] = { 0, 0.025f, 0.019f }, h[3] = { 0.15f, 0.025f, 0.019f };
    pm_bbox(pm, c, h, 0.006f, 2);
    for (int k = 0; k < 3; ++k) {
        float x = -0.09f + 0.07f * (float)k;
        pm_mat(pm, PMAT_ENAMEL);   /* the sockets' faces, off-white */
        float sc[3] = { x, 0.025f, 0.0385f }, sh[3] = { 0.021f, 0.019f, 0.0012f };
        pm_bbox(pm, sc, sh, 0.003f, 1);
        if (L0) {
            pm_mat(pm, PMAT_PLASTIC);
            for (int j = 0; j < 3; ++j) { float pc[3] = { x + (j == 2 ? 0.0f : (j ? 0.007f : -0.007f)), 0.022f + (j == 2 ? 0.01f : 0.0f), 0.0396f }, ph[3] = { 0.0022f, 0.0035f, 0.0004f }; pm_bbox(pm, pc, ph, 0.0f, 1); }
        }
    }
    pm_mat(pm, PMAT_RED);
    float wc[3] = { 0.125f, 0.025f, 0.039f }, wh[3] = { 0.009f, 0.014f, 0.003f };
    pm_bbox(pm, wc, wh, 0.002f, 1);
    if (L0) for (int k = -1; k <= 1; k += 2) { pm_push(pm); pm_translate(pm, 0.142f * (float)k, 0.025f, 0.038f); pm_rotate_x(pm, 0.5f * PI_F); pm_place(pm, PMK_BOLT); pm_pop(pm); }
}

/* A gas regulator on a bottle's outlet: the outlet's tip at the origin, the nut along +x, the diaphragm's
 * round body over it, the hose barb pointing down. */
static void m_regulator(Pm *pm) {
    pm_mat(pm, PMAT_BRASS);
    pm_push(pm);
    pm_rotate_z(pm, -0.5f * PI_F);   /* +y onto +x */
    hex(pm, 0.012f, 0.0f, 0.016f, L0 ? 0.001f : 0.0f);
    PmProf s[4] = { { 0, 0.016f, 0 }, { 0.008f, 0.016f, 0 }, { 0.008f, 0.032f, 0 }, { 0, 0.032f, 0 } };
    lathe(pm, s, 4, 12);
    pm_pop(pm);
    pm_mat(pm, PMAT_GALV);
    pm_push(pm);
    pm_translate(pm, 0.065f, -0.016f, 0);
    PmProf b[8] = { { 0, 0, 0 }, { 0.038f, 0, 0 }, { 0.04f, 0.004f, 0 }, { 0.04f, 0.018f, 1 }, { 0.038f, 0.02f, 0 }, { 0.03f, 0.03f, 0 }, { 0.012f, 0.036f, 0 }, { 0, 0.037f, 0 } };
    lathe(pm, b, 8, 28);
    if (!L2) { pm_mat(pm, PMAT_RED); PmProf k[4] = { { 0, 0.036f, 0 }, { 0.008f, 0.036f, 0 }, { 0.008f, 0.046f, 0 }, { 0, 0.046f, 0 } }; lathe(pm, k, 4, 10); }
    pm_mat(pm, PMAT_BRASS);
    PmProf o[6] = { { 0, -0.032f, 0 }, { 0.0045f, -0.032f, 0 }, { 0.0058f, -0.024f, 0 }, { 0.0045f, -0.02f, 0 }, { 0.007f, 0.001f, 0 }, { 0, 0.001f, 0 } };
    lathe(pm, o, 6, 10);
    pm_pop(pm);
}

/* ------------------------------------------------------ NB6: light, signs --- */
/* A neon stroke along points in icon units (the PM_ICON_SIZE square's half is 1) at z 0: a tube of 5.5 mm
 * glass, its ends capped and turning back into the housing as electrode legs (blacked-out glass, to z -0.03). */
static void neon_stroke(Pm *pm, const float (*pt)[2], int n) {
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
        pm_rotate_x(pm, 0.5f * PI_F);   /* local +y to +z: the leg from the back up to the tube */
        pm_cyl(pm, -0.03f, -0.004f, r, pm_segs(pm, 8, 4));
        pm_pop(pm);
    }
}
/* points along an ellipse's arc (centre, radii, angles), n of them */
static int arc_pts(float (*o)[2], int n, float cx, float cy, float rx, float ry, float a0, float a1) {
    for (int i = 0; i < n; ++i) {
        float a = a0 + (a1 - a0) * (float)i / (float)(n - 1);
        o[i][0] = cx + rx * cosf(a); o[i][1] = cy + ry * sinf(a);
    }
    return n;
}
/* a wisp of steam rising from (x, y0) to y1: a sine of `waves` waves (bends no tighter than the tube allows) */
static int wisp_pts(float (*o)[2], int n, float x, float y0, float y1, float amp, float waves) {
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)(n - 1);
        o[i][0] = x + amp * sinf(t * 2.0f * waves * PI_F); o[i][1] = y0 + (y1 - y0) * t;
    }
    return n;
}
static int icon_n(const Pm *pm, int n) { return L0 ? n : L2 ? (n + 3) / 4 + 1 : (n + 1) / 2 + 1; }
/* the bowl every bowl icon shares: rim, body, foot; y0 lowers it */
static void icon_bowl(Pm *pm, float y0) {
    float q[40][2];
    float rim[2][2] = { { -0.86f, 0.05f + y0 }, { 0.86f, 0.05f + y0 } };
    neon_stroke(pm, (const float (*)[2])rim, 2);
    int n = arc_pts(q, icon_n(pm, 24), 0.0f, 0.05f + y0, 0.78f, 0.62f, -0.08f * PI_F, -0.92f * PI_F);
    neon_stroke(pm, (const float (*)[2])q, n);
    float foot[2][2] = { { -0.3f, -0.72f + y0 }, { 0.3f, -0.72f + y0 } };
    neon_stroke(pm, (const float (*)[2])foot, 2);
}
static void m_icon_bowl(Pm *pm) {
    icon_bowl(pm, -0.1f);
    /* noodles heaped over the rim: three humps */
    float q[40][2];
    int n = icon_n(pm, 28);
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)(n - 1);
        q[i][0] = -0.66f + 1.32f * t; q[i][1] = 0.05f + 0.16f * fabsf(sinf(t * 3.0f * PI_F));
    }
    neon_stroke(pm, (const float (*)[2])q, n);
}
static void m_icon_chopsticks(Pm *pm) {
    icon_bowl(pm, -0.18f);
    float a[2][2] = { { -0.2f, 0.02f }, { 0.78f, 0.9f } }, b[2][2] = { { -0.02f, -0.04f }, { 0.92f, 0.72f } };
    neon_stroke(pm, (const float (*)[2])a, 2);
    neon_stroke(pm, (const float (*)[2])b, 2);
}
static void m_icon_steam(Pm *pm) {
    icon_bowl(pm, -0.22f);
    float q[24][2];
    for (int k = -1; k <= 1; ++k) {
        int n = wisp_pts(q, icon_n(pm, 16), 0.38f * (float)k, 0.0f, k ? 0.72f : 0.9f, 0.11f, 1.5f);
        neon_stroke(pm, (const float (*)[2])q, n);
    }
}
static void m_icon_cup(Pm *pm) {
    float q[40][2];
    /* the cup: its sides tapering to a flat bottom, its corners rounded */
    {
        const float h = 0.5f * PM_ICON_SIZE;
        PmPath c, f;
        pm_path_init(&c, 4);
        pm_path_init(&f, 32);
        pm_path_add(&c, -0.52f * h, 0.5f * h, 0.0f, 0); pm_path_add(&c, -0.4f * h, -0.56f * h, 0.0f, 0);
        pm_path_add(&c, 0.4f * h, -0.56f * h, 0.0f, 0); pm_path_add(&c, 0.52f * h, 0.5f * h, 0.0f, 0);
        pm_path_fillet(&c, 0.2f * h, L0 ? 4 : 2, &f);
        int n = f.n < 40 ? f.n : 40;
        for (int i = 0; i < n; ++i) { q[i][0] = f.p[i][0] / h; q[i][1] = f.p[i][1] / h; }
        pm_path_free(&c); pm_path_free(&f);
        neon_stroke(pm, (const float (*)[2])q, n);
    }
    int n;
    float rim[2][2] = { { -0.6f, 0.5f }, { 0.6f, 0.5f } };
    neon_stroke(pm, (const float (*)[2])rim, 2);
    n = arc_pts(q, icon_n(pm, 14), 0.5f, 0.06f, 0.3f, 0.26f, -0.42f * PI_F, 0.42f * PI_F);
    neon_stroke(pm, (const float (*)[2])q, n);
    float saucer[2][2] = { { -0.82f, -0.72f }, { 0.82f, -0.72f } };
    neon_stroke(pm, (const float (*)[2])saucer, 2);
    for (int k = -1; k <= 1; k += 2) {
        n = wisp_pts(q, icon_n(pm, 12), 0.22f * (float)k, 0.62f, 0.98f, 0.06f, 1.0f);
        neon_stroke(pm, (const float (*)[2])q, n);
    }
}
/* a tongue of flame: a cone from its base on the origin up local +y */
static void tongue(Pm *pm, float r, float h) {
    PmProf c[4] = { { 0, 0, 0 }, { r, 0, 0 }, { 0.75f * r, 0.45f * h, 0 }, { 0, h, 0 } };
    pm_lathe(pm, c, 4, pm_segs(pm, 6, 4), 0, 0);
}
/* A ring burner's flames: tongues out and up from its crown's ports (m_burner: 24 at 43 mm, 33.5 mm up). */
static void m_flame_ring(Pm *pm) {
    pm_mat(pm, PMAT_FLAME);
    const int n = L0 ? 24 : L2 ? 8 : 12;
    for (int k = 0; k < n; ++k) {
        float a = 2.0f * PI_F * ((float)k + 0.5f) / (float)n;
        pm_push(pm);
        pm_rotate_y(pm, a);
        pm_translate(pm, 0.046f, 0.0335f, 0.0f);
        pm_rotate_z(pm, -0.75f);   /* leaning out (+x) about 43 degrees off upright */
        tongue(pm, L0 ? 0.0032f : 0.005f, 0.017f);
        pm_pop(pm);
    }
}
/* A wok burner's jets (m_wok_burner: 16 at 60 mm, their tips 56 mm up), roaring up into the wok. */
static void m_flame_jet(Pm *pm) {
    pm_mat(pm, PMAT_FLAME);
    const int n = L0 ? 16 : 8;
    for (int k = 0; k < n; ++k) {
        float a = 2.0f * PI_F * (float)k / (float)n;
        pm_push(pm);
        pm_rotate_y(pm, a);
        pm_translate(pm, 0.06f, 0.055f, 0.0f);
        pm_rotate_z(pm, -0.12f);
        tongue(pm, L0 ? 0.0055f : 0.008f, 0.075f);
        pm_pop(pm);
    }
}
/* The kitchen's lamp: a bulkhead hanging from the origin (a ceiling or a canopy's underside), its warm glass. */
static void m_lamp(Pm *pm) {
    pm_mat(pm, PMAT_STEEL);
    PmProf b[5] = { { 0, -0.016f, 0 }, { 0.058f, -0.016f, 1 }, { 0.062f, -0.008f, 0 }, { 0.06f, 0.0f, 1 }, { 0, 0.0f, 0 } };
    lathe(pm, b, 5, 28);
    pm_mat(pm, PMAT_BULB);
    PmProf g[6] = { { 0, -0.052f, 0 }, { 0.022f, -0.05f, 0 }, { 0.038f, -0.042f, 0 }, { 0.048f, -0.028f, 0 }, { 0.05f, -0.015f, 0 }, { 0, -0.015f, 0 } };
    lathe(pm, g, 6, 24);
}

/* ----------------------------------------------------------- materials --- */
const char *const PM_MAT_NAME[PMAT_COUNT] = {
    [PMAT_RUST] = "rust", [PMAT_PLATE] = "plate", [PMAT_STAINLESS] = "stainless", [PMAT_COPPER] = "copper",
    [PMAT_CAST_IRON] = "cast_iron", [PMAT_PAINTED] = "painted", [PMAT_TILE] = "tile", [PMAT_RUBBER] = "rubber",
    [PMAT_ENAMEL] = "enamel", [PMAT_GLASS] = "glass", [PMAT_BRASS] = "brass", [PMAT_TIMBER] = "timber",
    [PMAT_CHALK] = "chalk", [PMAT_CERAMIC] = "ceramic", [PMAT_BAMBOO] = "bamboo", [PMAT_PAPER] = "paper",
    [PMAT_PLASTIC] = "plastic", [PMAT_GALV] = "galv", [PMAT_STEEL] = "steel", [PMAT_DIAL] = "dial", [PMAT_RED] = "red",
    [PMAT_BROTH] = "broth", [PMAT_NOODLE] = "noodle", [PMAT_CURRY] = "curry", [PMAT_RICE] = "rice", [PMAT_OIL] = "oil",
    [PMAT_GINGER] = "ginger", [PMAT_RUST_STEEL] = "rust_steel", [PMAT_CHEQUER] = "chequer", [PMAT_LINING] = "lining",
    [PMAT_TUBE] = "tube", [PMAT_NEON] = "neon", [PMAT_FLAME] = "flame", [PMAT_BULB] = "bulb", [PMAT_SIGN_FAR] = "sign_far",
    [PMAT_MEAT] = "meat", [PMAT_DOUGH] = "dough", [PMAT_BREAD] = "bread", [PMAT_FRIED] = "fried", [PMAT_FRYOIL] = "fryoil",
    [PMAT_CHESTNUT] = "chestnut", [PMAT_GREENS] = "greens", [PMAT_SAUCE] = "sauce", [PMAT_GRIT] = "grit", [PMAT_EMBER] = "ember", [PMAT_RADIANT] = "radiant",
    [PMAT_GLAZING] = "glazing", [PMAT_WINDOW_FAR] = "window_far", [PMAT_SHUTTER] = "shutter",
};
/* Mirrored by kitSet in shaders/city/kit.glsl. Sets may serve two materials in different colours (the
 * rubber's grain under black plastic, the white paint's under red): the share check counts them together. */
const char *const PM_MAT_SET[PMAT_COUNT] = {
    [PMAT_RUST] = "kit_rust_sheet", [PMAT_PLATE] = "kit_paint", [PMAT_STAINLESS] = "kit_stainless",
    [PMAT_COPPER] = "kit_copper", [PMAT_CAST_IRON] = "kit_cast_iron", [PMAT_PAINTED] = "kit_paint",
    [PMAT_TILE] = "kit_tile", [PMAT_RUBBER] = "kit_rubber", [PMAT_BRASS] = "kit_brass", [PMAT_TIMBER] = "vendor_planks",
    [PMAT_BAMBOO] = "kit_bamboo", [PMAT_PAPER] = "paper", [PMAT_PLASTIC] = "kit_rubber", [PMAT_GALV] = "kit_galv",
    [PMAT_STEEL] = "kit_steel", [PMAT_RED] = "kit_painted", [PMAT_RUST_STEEL] = "kit_rust_steel",
    [PMAT_CHEQUER] = "kit_chequer", [PMAT_LINING] = "kit_lining", [PMAT_SHUTTER] = "shutter",
};

/* --------------------------------------------------------------- table --- */
static const PmModuleDef DEFS[PMK_COUNT] = {
    [PMK_RIVET]        = { "rivet",          m_rivet,          { 50, 0, 0 },          1 },
    [PMK_RIVET_ROW4]   = { "rivet_row4",     m_rivet_row4,     { 200, 0, 0 },         1 },
    [PMK_RIVET_ROW10]  = { "rivet_row10",    m_rivet_row10,    { 400, 0, 0 },         1 },
    [PMK_BOLT]         = { "bolt",           m_bolt,           { 100, 50, 0 },         1 },
    [PMK_NUT_WASHER]   = { "nut_washer",     m_nut_washer,     { 200, 150, 0 },        1 },
    [PMK_KNOB]         = { "knob",           m_knob,           { 800, 350, 100 },      1 },
    [PMK_TOGGLE]       = { "toggle",         m_toggle,         { 700, 150, 100 },      1 },
    [PMK_GAUGE]        = { "gauge",          m_gauge,          { 1500, 850, 200 },     1 },
    [PMK_VALVE_WHEEL]  = { "valve_wheel",    m_valve_wheel,    { 1550, 800, 300 },    1 },
    [PMK_VALVE_LEVER]  = { "valve_lever",    m_valve_lever,    { 850, 400, 150 },     1 },
    [PMK_BRACKET]      = { "bracket",        m_bracket,        { 250, 100, 50 },      1 },
    [PMK_SADDLE]       = { "saddle",         m_saddle,         { 500, 150, 150 },      1 },
    [PMK_INSULATOR]    = { "insulator",      m_insulator,      { 1400, 550, 300 },     1 },
    [PMK_BELL]         = { "bell",           m_bell,           { 1050, 600, 200 },     1 },
    [PMK_STOOL]        = { "stool",          m_stool,          { 4400, 1600, 550 },   1 },
    [PMK_STOCKPOT_S]   = { "stockpot_s",     m_stockpot_s,     { 4700, 1400, 550 },   1 },
    [PMK_STOCKPOT_L]   = { "stockpot_l",     m_stockpot_l,     { 4950, 1550, 550 },   1 },
    [PMK_LID]          = { "lid",            m_lid,            { 1000, 500, 250 },    1 },
    [PMK_BOWL_STACK]   = { "bowl_stack",     m_bowl_stack,     { 7350, 2100, 900 },   1 },
    [PMK_JAR_OIL]      = { "jar_oil",        m_jar_oil,        { 3200, 1800, 700 },    1 },
    [PMK_JAR_GINGER]   = { "jar_ginger",     m_jar_ginger,     { 3100, 1800, 700 },    1 },
    [PMK_BOTTLE]       = { "bottle",         m_bottle,         { 1400, 750, 250 },     1 },
    [PMK_BURNER]       = { "burner",         m_burner,         { 1450, 650, 200 },    1 },
    [PMK_GRATE]        = { "grate",          m_grate,          { 3100, 250, 250 },     1 },
    [PMK_WOK_BURNER]   = { "wok_burner",     m_wok_burner,     { 5900, 1750, 550 },   1 },
    [PMK_STEAMER]      = { "steamer",        m_steamer,        { 5950, 3100, 500 },   1 },
    [PMK_GN_PAN]       = { "gn_pan",         m_gn_pan,         { 3050, 1700, 500 },    1 },
    [PMK_GN_PAN_OPEN]  = { "gn_pan_open",    m_gn_pan_open,    { 2400, 1350, 350 },    1 },
    [PMK_RICE_COOKER]  = { "rice_cooker",    m_rice_cooker,    { 3650, 1600, 500 },   1 },
    [PMK_LADLE]        = { "ladle",          m_ladle,          { 1450, 750, 450 },    1 },
    [PMK_SPIDER]       = { "spider",         m_spider,         { 2950, 1150, 250 },    1 },
    [PMK_SPATULA]      = { "spatula",        m_spatula,        { 700, 300, 150 },     1 },
    [PMK_CLEAVER]      = { "cleaver",        m_cleaver,        { 700, 250, 100 },     1 },
    [PMK_S_HOOK]       = { "s_hook",         m_s_hook,         { 400, 100, 100 },      1 },
    [PMK_UTENSIL_RAIL] = { "utensil_rail",   m_utensil_rail,   { 1150, 400, 150 },     1 },
    [PMK_GAS_13]       = { "gas_13",         m_gas_13,         { 4000, 1850, 600 },    1 },
    [PMK_GAS_47]       = { "gas_47",         m_gas_47,         { 4500, 1950, 600 },    1 },
    [PMK_FAN]          = { "fan",            m_fan,            { 5900, 2150, 650 },   1 },
    [PMK_POWER_PANEL]  = { "power_panel",    m_power_panel,    { 10350, 3200, 1400 },   1 },
    [PMK_SHELF_BRACKET]= { "shelf_bracket",  m_shelf_bracket,  { 300, 150, 50 },      1 },
    [PMK_A_FRAME]      = { "a_frame",        m_a_frame,        { 4850, 450, 200 },    1 },
    [PMK_WOK]          = { "wok",            m_wok,            { 4000, 1500, 400 },   1 },
    [PMK_NOODLE_BASKET]= { "noodle_basket",  m_noodle_basket,  { 3600, 1100, 250 },   1 },
    [PMK_SAUCEPAN]     = { "saucepan",       m_saucepan,       { 3000, 1200, 400 },   1 },
    [PMK_CHOP_BLOCK]   = { "chop_block",     m_chop_block,     { 1450, 700, 200 },    1 },
    [PMK_CHOPSTICKS]   = { "chopsticks",     m_chopsticks,     { 1400, 500, 150 },    1 },
    [PMK_POWER_STRIP]  = { "power_strip",    m_power_strip,    { 1300, 300, 100 },    1 },
    [PMK_REGULATOR]    = { "regulator",      m_regulator,      { 1000, 500, 200 },    1 },
    [PMK_ICON_BOWL]    = { "icon_bowl",      m_icon_bowl,      { 4000, 1800, 800 },   1 },
    [PMK_ICON_CHOPSTICKS] = { "icon_chopsticks", m_icon_chopsticks, { 4000, 1800, 800 }, 1 },
    [PMK_ICON_STEAM]   = { "icon_steam",     m_icon_steam,     { 4000, 1800, 800 },   1 },
    [PMK_ICON_CUP]     = { "icon_cup",       m_icon_cup,       { 4000, 1800, 800 },   1 },
    [PMK_FLAME_RING]   = { "flame_ring",     m_flame_ring,     { 1200, 400, 200 },    1 },
    [PMK_FLAME_JET]    = { "flame_jet",      m_flame_jet,      { 1000, 400, 400 },    1 },
    [PMK_LAMP]         = { "lamp",           m_lamp,           { 700, 350, 150 },     1 },
    /* SF1: the street-food trades (pmodel_food.c) */
    [PMK_CHAR_GRILL   ] = { "char_grill",     m_char_grill,      { 9000, 3800, 900 }, 1 },
    [PMK_SKEWERS      ] = { "skewers",        m_skewers,         { 4000, 1600, 600 }, 1 },
    [PMK_SKEWER_TRAY  ] = { "skewer_tray",    m_skewer_tray,     { 3600, 2000, 500 }, 1 },
    [PMK_SPIT         ] = { "spit",           m_spit,            { 6000, 2400, 800 }, 1 },
    [PMK_GRIDDLE      ] = { "griddle",        m_griddle,         { 5600, 1600, 500 }, 1 },
    [PMK_GRIDDLE_S    ] = { "griddle_s",      m_griddle_s,       { 5000, 1600, 500 }, 1 },
    [PMK_PATTY        ] = { "patty",          m_patty,           { 300, 200, 100 }, 1 },
    [PMK_SAUSAGE      ] = { "sausage",        m_sausage,         { 300, 200, 100 }, 1 },
    [PMK_BUN          ] = { "bun",            m_bun,             { 300, 200, 100 }, 1 },
    [PMK_BREAD_STACK  ] = { "bread_stack",    m_bread_stack,     { 1700, 600, 200 }, 1 },
    [PMK_TORTILLA     ] = { "tortilla",       m_tortilla,        { 250, 150, 80 }, 1 },
    [PMK_FRYER        ] = { "fryer",          m_fryer,           { 6000, 2000, 600 }, 1 },
    [PMK_FRY_BASKET   ] = { "fry_basket",     m_fry_basket,      { 5200, 1800, 300 }, 1 },
    [PMK_FRIED_TRAY   ] = { "fried_tray",     m_fried_tray,      { 4200, 1800, 500 }, 1 },
    [PMK_CREPE_PLATE  ] = { "crepe_plate",    m_crepe_plate,     { 3400, 1300, 500 }, 1 },
    [PMK_CREPE        ] = { "crepe",          m_crepe,           { 400, 250, 100 }, 1 },
    [PMK_CREPE_RAKE   ] = { "crepe_rake",     m_crepe_rake,      { 300, 150, 60 }, 1 },
    [PMK_URN          ] = { "urn",            m_urn,             { 4200, 1900, 650 }, 1 },
    [PMK_ESPRESSO     ] = { "espresso",       m_espresso,        { 12500, 5200, 1200 }, 1 },
    [PMK_CUP_STACK    ] = { "cup_stack",      m_cup_stack,       { 2900, 1100, 250 }, 1 },
    [PMK_CHESTNUT_WOK ] = { "chestnut_wok",   m_chestnut_wok,    { 9000, 3200, 600 }, 1 },
    [PMK_ROAST_PAN    ] = { "roast_pan",      m_roast_pan,       { 3200, 1600, 500 }, 1 },
    [PMK_DUMPLING_TRAY] = { "dumpling_tray",  m_dumpling_tray,   { 5600, 3000, 300 }, 1 },
    [PMK_GN_GREENS    ] = { "gn_greens",      m_gn_greens,       { 2600, 1400, 400 }, 1 },
    [PMK_GN_MEAT      ] = { "gn_meat",        m_gn_meat,         { 2600, 1400, 400 }, 1 },
    [PMK_GN_SAUCE     ] = { "gn_sauce",       m_gn_sauce,        { 2600, 1400, 400 }, 1 },
    [PMK_CURRY_POT    ] = { "curry_pot",      m_curry_pot,       { 4200, 1400, 500 }, 1 },
    [PMK_PAPER_CONES  ] = { "paper_cones",    m_paper_cones,     { 1600, 600, 250 }, 1 },
    [PMK_ICON_SKEWER  ] = { "icon_skewer",    m_icon_skewer,     { 4000, 1800, 800 }, 1 },
    [PMK_ICON_DUMPLING] = { "icon_dumpling",  m_icon_dumpling,   { 4000, 1800, 800 }, 1 },
    [PMK_ICON_KEBAB   ] = { "icon_kebab",     m_icon_kebab,      { 4000, 1800, 800 }, 1 },
    [PMK_ICON_BURGER  ] = { "icon_burger",    m_icon_burger,     { 4000, 1800, 800 }, 1 },
    [PMK_ICON_HOTDOG  ] = { "icon_hotdog",    m_icon_hotdog,     { 4000, 1800, 800 }, 1 },
    [PMK_ICON_DRUMSTICK] = { "icon_drumstick", m_icon_drumstick,  { 4000, 1800, 800 }, 1 },
    [PMK_ICON_FRIES   ] = { "icon_fries",     m_icon_fries,      { 4000, 1800, 800 }, 1 },
    [PMK_ICON_TACO    ] = { "icon_taco",      m_icon_taco,       { 4000, 1800, 800 }, 1 },
    [PMK_ICON_CREPE   ] = { "icon_crepe",     m_icon_crepe,      { 4000, 1800, 800 }, 1 },
    [PMK_ICON_CHESTNUT] = { "icon_chestnut",  m_icon_chestnut,   { 4000, 1800, 800 }, 1 },
    [PMK_ICON_TEAPOT  ] = { "icon_teapot",    m_icon_teapot,     { 4000, 1800, 800 }, 1 },
    [PMK_ICON_FLAME   ] = { "icon_flame",     m_icon_flame,      { 4000, 1800, 800 }, 1 },
    [PMK_CAFE_TABLE   ] = { "cafe_table",     m_cafe_table,      { 2600, 800, 300 }, 1 },
    [PMK_LOW_STOOL    ] = { "low_stool",      m_low_stool,       { 2400, 900, 300 }, 1 },
    [PMK_SHUTTER      ] = { "shutter",        m_shutter,         { 200, 60, 12 },   1 },
    [PMK_SHUTTER_BAR  ] = { "shutter_bar",    m_shutter_bar,     { 600, 400, 24 },  1 },
    [PMK_SHUTTER_GUIDE] = { "shutter_guide",  m_shutter_guide,   { 300, 100, 36 },  1 },
    [PMK_SHUTTER_LOCK ] = { "shutter_lock",   m_shutter_lock,    { 900, 300, 40 },  1 },
};

int pm_module_is_icon(int id) { return (id >= PMK_ICON_BOWL && id <= PMK_ICON_CUP) || (id >= PMK_ICON_SKEWER && id <= PMK_ICON_FLAME); }
/* everything but the lights' own glass, flames and neon (SF1's stations cast, their coals included) */
int pm_module_casts(int id) {
    return id >= 0 && id < PMK_COUNT && !pm_module_is_icon(id) && id != PMK_FLAME_RING && id != PMK_FLAME_JET && id != PMK_LAMP;
}

int pm_module_count(void) { return PMK_COUNT; }
const PmModuleDef *pm_module(int id) { return id >= 0 && id < PMK_COUNT ? &DEFS[id] : NULL; }

void pm_place(Pm *pm, int id) {
    const PmModuleDef *d = pm_module(id);
    if (!d) return;
    if (pm->inst) {
        PmInstances *l = pm->inst;
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = (PmInstance *)realloc(l->v, sizeof(PmInstance) * l->cap); }
        PmInstance *o = &l->v[l->n++];
        memcpy(o->xf, pm->xf, sizeof(o->xf));
        o->module = (uint16_t)id; o->part = pm->part; o->show = pm->show;
        return;
    }
    uint8_t mat = pm->mat, part = pm->part;
    int depth = pm->depth;
    pm_push(pm);
    d->build(pm);
    pm_pop(pm);
    pm->depth = depth;
    pm->mat = mat; pm->part = part;
}

void pm_module_mesh(int id, int lod, PmMesh *out) {
    out->nv = out->ni = 0;
    const PmModuleDef *d = pm_module(id);
    if (!d) return;
    Pm pm;
    pm_begin(&pm, out, NULL, lod);
    d->build(&pm);
    if (out->ni) pm_bake(out, 24, pm_reach(out));
}
