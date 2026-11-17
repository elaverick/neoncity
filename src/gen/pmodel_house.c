#include "pmodel_house.h"
#include "mathlib.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * The houses' character kit (residential roadmap HM4): see pmodel_house.h for
 * the frames. Sizes after docs/residential-target.md ("Massing and parts",
 * "The character kit") and the signed-off references.
 *
 *   helpers     sections run along an axis, faces and slabs, frames
 *   roof edges  rafter tails, knee braces, brackets, bargeboards, finials,
 *               gutters, swan necks, downpipes, clips, shoes, hoppers, ridges
 *   walls       string courses, plinths, quoins, soldier courses, timbers,
 *               hung tiles
 *   openings    sills, lintels, keystones, arches, hood moulds, casings,
 *               frames, shutters, grilles, window boxes
 *   doors       leaves, fanlights, doorcases
 *   porches     posts, spandrels, balustrades, canopies; bays' parts
 *   roofscape   dormers, pots, caps, vent pipes, rooflights
 *   assemblies  what places them (pmodel_house.h)
 * ========================================================================= */

#define L0 (pm->lod == PM_LOD0)
#define L1 (pm->lod == PM_LOD1)
#define L2 (pm->lod == PM_LOD2)
#define HALF_PI 1.5707963f
#define DEG (PI_F / 180.0f)

/* ---------------------------------------------------------------- helpers --- */
static void set3(float o[3], float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void v3sub(float o[3], const float a[3], const float b[3]) { for (int i = 0; i < 3; ++i) o[i] = a[i] - b[i]; }
static float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(float o[3], const float a[3], const float b[3]) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static int v3norm(float a[3]) {
    const float l = sqrtf(v3dot(a, a));
    if (l < 1e-12f) return 0;
    for (int i = 0; i < 3; ++i) a[i] /= l;
    return 1;
}
/* curves' segments by level: base at LOD0, two thirds, a third (at least lo) */
static int segs(const Pm *pm, int base, int lo) { return pm_segs(pm, base, lo); }

/* a box with its edges chamfered by bevel (dropped at the coarser levels when small) */
static void bx(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    const float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    const float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    pm_bbox(pm, c, h, bevel, 1);
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
/* A section (n points (x, z)) run up y from y0 to y1. */
static void run_y(Pm *pm, const float (*sec)[2], int n, float y0, float y1) {
    float poly[64][2];
    if (n > 64) n = 64;
    for (int k = 0; k < n; ++k) { poly[k][0] = sec[k][0]; poly[k][1] = -sec[k][1]; }
    ccw(poly, n);
    pm_push(pm);
    pm_rotate_x(pm, -HALF_PI);   /* the extrusion's z onto +y, the section's y onto -z */
    pm_extrude(pm, (const float (*)[2])poly, n, y0, y1);
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

/* A flat face (a convex planar polygon) of a closed solid whose inside holds `inside`: its normal outwards, its
 * own vertices (hard edges), metre UVs along its first edge and across. */
static void face(Pm *pm, const float (*p)[3], int n, const float inside[3]) {
    float nn[3] = { 0, 0, 0 };
    for (int k = 0; k < n; ++k) {   /* Newell */
        const float *a = p[k], *b = p[(k + 1) % n];
        nn[0] += (a[1] - b[1]) * (a[2] + b[2]); nn[1] += (a[2] - b[2]) * (a[0] + b[0]); nn[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    if (!v3norm(nn)) return;
    float d[3];
    v3sub(d, p[0], inside);
    if (v3dot(d, nn) < 0.0f) { nn[0] = -nn[0]; nn[1] = -nn[1]; nn[2] = -nn[2]; }
    float u[3], v[3];
    v3sub(u, p[1], p[0]);
    if (!v3norm(u)) return;
    v3cross(v, nn, u);
    uint32_t i0 = 0;
    for (int k = 0; k < n; ++k) {
        float e[3];
        v3sub(e, p[k], p[0]);
        uint32_t i = pm_vert(pm, p[k], nn, v3dot(e, u), v3dot(e, v));
        if (k == 0) i0 = i;
    }
    for (int k = 1; k + 1 < n; ++k) pm_tri(pm, i0, i0 + (uint32_t)k, i0 + (uint32_t)k + 1);
}
/* A closed hexahedron from its corners: 0-3 one end, 4-7 the other, k + 4 opposite k (planar faces). */
static void hexa(Pm *pm, const float c[8][3]) {
    pm->shell++;
    float m[3] = { 0, 0, 0 };
    for (int k = 0; k < 8; ++k) { m[0] += c[k][0] * 0.125f; m[1] += c[k][1] * 0.125f; m[2] += c[k][2] * 0.125f; }
    const int F[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    for (int f = 0; f < 6; ++f) {
        float q[4][3];
        for (int k = 0; k < 4; ++k) memcpy(q[k], c[F[f][k]], sizeof(q[k]));
        face(pm, (const float (*)[3])q, 4, m);
    }
}
/* A closed slab: the convex planar polygon p (n corners in order) and its copy moved t along -nrm. */
static void slab(Pm *pm, const float (*p)[3], int n, const float nrm[3], float t) {
    if (n < 3 || n > 8) return;
    pm->shell++;
    float q[8][3], m[3] = { 0, 0, 0 };
    for (int k = 0; k < n; ++k) {
        for (int i = 0; i < 3; ++i) q[k][i] = p[k][i] - nrm[i] * t;
        for (int i = 0; i < 3; ++i) m[i] += (p[k][i] + q[k][i]) * 0.5f / (float)n;
    }
    face(pm, p, n, m);
    face(pm, (const float (*)[3])q, n, m);
    for (int k = 0; k < n; ++k) {
        const int j = (k + 1) % n;
        float s[4][3];
        memcpy(s[0], p[k], sizeof(s[0])); memcpy(s[1], p[j], sizeof(s[1])); memcpy(s[2], q[j], sizeof(s[2])); memcpy(s[3], q[k], sizeof(s[3]));
        face(pm, (const float (*)[3])s, 4, m);
    }
}
/* A frustum standing on y0: a rectangle of half sides (ax, az) at y0 to (bx, bz) at y1, centred. */
static void frustum(Pm *pm, float y0, float y1, float ax, float az, float bxh, float bz) {
    float c[8][3];
    const float sx[4] = { -1, 1, 1, -1 }, sz[4] = { -1, -1, 1, 1 };
    for (int k = 0; k < 4; ++k) { set3(c[k], sx[k] * ax, y0, sz[k] * az); set3(c[k + 4], sx[k] * bxh, y1, sz[k] * bz); }
    hexa(pm, (const float (*)[3])c);
}

/* four members round a rectangle (x0..x1, y0..y1), sides ws wide, the bottom wb, the top wt, z0..z1 deep */
static void rect_frame(Pm *pm, float x0, float y0, float x1, float y1, float ws, float wb, float wt, float z0, float z1, float bev) {
    bx(pm, x0, y0, z0, x1, y0 + wb, z1, bev);
    bx(pm, x0, y1 - wt, z0, x1, y1, z1, bev);
    bx(pm, x0, y0 + wb, z0, x0 + ws, y1 - wt, z1, bev);
    bx(pm, x1 - ws, y0 + wb, z0, x1, y1 - wt, z1, bev);
}
/* a raised and fielded panel in x0..x1, y0..y1 on the plane z0 (its field up to z1, its edges bevelled) */
static void field(Pm *pm, float x0, float y0, float x1, float y1, float z0, float z1) {
    if (L2) return;
    const float in = 0.03f;
    bx(pm, x0 + in, y0 + in, z0 - 0.002f, x1 - in, y1 - in, z1, L0 ? 0.008f : 0.0f);
}
/* the house kit keeps no support loops: its parts are seen from the street, and a house has a hundred of them */
static void quiet(Pm *pm) { pm->support = 0.0f; pm->maxlen = 0.0f; }
static float rnd(uint32_t *h) { *h = *h * 1664525u + 1013904223u; return (float)(*h >> 8) / 16777216.0f; }

/* ------------------------------------------------------------- roof edges --- */
/* An exposed rafter tail (craftsman): 50 x 140 mm, out from the wall (z) to HK_RAFTER_OUT, its top at y 0 (the
 * roof's underside), its end cut back in a notched curve; it starts 50 mm inside the soffit. */
void m_h_rafter_tail(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float o = HK_RAFTER_OUT;
    if (L2) {
        const float t[5][2] = { { -0.05f, -0.14f }, { o - 0.08f, -0.14f }, { o, -0.05f }, { o, 0.0f }, { -0.05f, 0.0f } };
        run_x(pm, t, 5, -0.025f, 0.025f);
        return;
    }
    const float s[8][2] = { { -0.05f, -0.14f }, { o - 0.12f, -0.14f }, { o - 0.06f, -0.115f }, { o - 0.045f, -0.085f },
                            { o - 0.045f, -0.065f }, { o, -0.045f }, { o, 0.0f }, { -0.05f, 0.0f } };
    run_x(pm, s, 8, -0.025f, 0.025f);
}

/* A craftsman knee brace, 100 mm timbers: a post down the wall (z 0..0.1, y -0.75..0), an arm out under the
 * eaves (its end cut back), a strut between; the top of the arm at y 0. */
void m_h_knee_brace(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -0.05f, -0.75f, 0.0f, 0.05f, -0.1f, 0.1f, 0.006f);
    const float arm[5][2] = { { 0.0f, -0.12f }, { 0.62f, -0.12f }, { 0.75f, -0.05f }, { 0.75f, 0.0f }, { 0.0f, 0.0f } };
    run_x(pm, arm, 5, -0.05f, 0.05f);
    float a[3] = { 0.0f, -0.66f, 0.05f }, b[3] = { 0.0f, -0.07f, 0.64f }, up[3] = { 0.0f, 0.7071f, -0.7071f };
    pm_beam(pm, a, b, 0.09f, 0.09f, L2 ? 0.0f : 0.005f, up);
    if (L0) {   /* pegs */
        pm_mat(pm, PMAT_HTIMBER);
        for (int k = 0; k < 2; ++k) {
            pm_push(pm);
            pm_translate(pm, 0.05f, k ? -0.06f : -0.55f, k ? 0.55f : 0.05f);
            pm_rotate_z(pm, -HALF_PI);
            pm_cyl(pm, -0.004f, 0.004f, 0.012f, 8);
            pm_pop(pm);
        }
    }
}

/* A Victorian eaves bracket (a console): 100 mm thick, its back on the wall from y -0.34 to its top at y 0,
 * reaching 0.28 out under the soffit, its underside an ogee with a scroll at each end. */
void m_h_eaves_bracket(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    float s[24][2];
    int n = 0;
    s[n][0] = 0.0f; s[n][1] = -0.34f; n++;
    const int k1 = segs(pm, 12, 4);
    for (int k = 0; k <= k1; ++k) {   /* an ogee from the foot out to the top's end */
        const float t = (float)k / (float)k1;
        const float z = 0.035f + 0.245f * t;
        const float y = -0.34f + 0.29f * (0.5f - 0.5f * cosf(PI_F * t)) + 0.035f * sinf(2.0f * PI_F * t);
        s[n][0] = z; s[n][1] = y; n++;
    }
    s[n][0] = 0.28f; s[n][1] = 0.0f; n++;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    run_x(pm, (const float (*)[2])s, n, -0.05f, 0.05f);
    if (!L2) {   /* the scrolls: rolls across its ends */
        for (int k = 0; k < 2; ++k) {
            pm_push(pm);
            pm_translate(pm, 0.0f, k ? -0.04f : -0.30f, k ? 0.255f : 0.045f);
            pm_rotate_z(pm, HALF_PI);
            pm_cyl(pm, -0.055f, 0.055f, k ? 0.03f : 0.04f, segs(pm, 12, 6));
            pm_pop(pm);
        }
    }
    if (L0) {   /* a drop under the foot */
        pm_push(pm); pm_translate(pm, 0.0f, -0.34f, 0.05f);
        PmProf d[5] = { { 0, -0.09f, 0 }, { 0.016f, -0.075f, 0 }, { 0.022f, -0.035f, 0 }, { 0.03f, 0.0f, 1 }, { 0, 0.0f, 0 } };
        pm_lathe(pm, d, 5, segs(pm, 10, 6), 0, 0);
        pm_pop(pm);
    }
}

/* Bargeboards: a board along x 0..1 (stretched along the rake), its top edge at y 0 against the verge, 32 mm
 * thick (z 0..0.032), its lower edge cut in a pattern: scallops (four a metre) or gothic cusps (four a metre). */
static void barge(Pm *pm, int gothic) {
    pm_mat(pm, PMAT_HTRIM);
    float s[64][2];
    int n = 0;
    const float band = gothic ? 0.11f : 0.12f;
    s[n][0] = 1.0f; s[n][1] = 0.0f; n++;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    const int per = L2 ? 2 : L1 ? 4 : 7;
    for (int k = 0; k < 4; ++k) {
        const float a = 0.25f * (float)k;
        if (gothic) {   /* concave arcs between points hanging to -0.24 */
            for (int i = 0; i < per; ++i) {
                const float t = (float)i / (float)per;
                s[n][0] = a + 0.25f * t; s[n][1] = -0.24f + (0.24f - band) * sinf(PI_F * t); n++;
            }
        } else {        /* round lobes to -0.22, a notch between */
            for (int i = 0; i <= per; ++i) {
                const float th = PI_F + PI_F * (float)i / (float)per;
                s[n][0] = a + 0.125f + 0.105f * cosf(th); s[n][1] = -band + 0.1f * sinf(th); n++;
            }
        }
    }
    if (gothic) { s[n][0] = 1.0f; s[n][1] = -0.24f; n++; }
    else { s[n][0] = 1.0f; s[n][1] = -band; n++; }
    run_z(pm, (const float (*)[2])s, n, 0.0f, 0.032f);
}
void m_h_barge_scallop(Pm *pm) { quiet(pm); barge(pm, 0); }
void m_h_barge_gothic(Pm *pm) { quiet(pm); barge(pm, 1); }

/* A turned finial at a gable's apex (the origin, on the bargeboards' middle plane z 0.016): a square block where
 * the boards meet, a drop pendant under it, a turned spire with a ball over it, 0.95 m in all. */
void m_h_finial(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    pm_push(pm);
    pm_translate(pm, 0.0f, 0.0f, 0.016f);
    bx(pm, -0.05f, -0.14f, -0.05f, 0.05f, 0.10f, 0.05f, 0.006f);
    PmProf drop[7] = { { 0, -0.42f, 0 }, { 0.014f, -0.40f, 0 }, { 0.03f, -0.33f, 0 }, { 0.022f, -0.25f, 0 }, { 0.038f, -0.18f, 1 },
                       { 0.038f, -0.13f, 1 }, { 0, -0.13f, 0 } };
    pm_lathe(pm, drop, 7, segs(pm, 12, 6), 0, 0);
    PmProf top[10] = { { 0, 0.09f, 0 }, { 0.045f, 0.09f, 1 }, { 0.045f, 0.13f, 1 }, { 0.028f, 0.16f, 0 }, { 0.04f, 0.24f, 0 },
                       { 0.046f, 0.29f, 0 }, { 0.03f, 0.34f, 0 }, { 0.016f, 0.38f, 0 }, { 0.008f, 0.48f, 0 }, { 0, 0.53f, 0 } };
    pm_lathe(pm, top, 10, segs(pm, 12, 6), 0, 0);
    pm_pop(pm);
}

/* Gutters: x 0..1 (stretched to the eaves' length), the back on the fascia's face (z 0), the top at y 0. */
void m_h_gutter_half(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    const float r = 0.0575f, t = L2 ? 0.006f : 0.004f, cz = r + 0.008f;
    const int k1 = segs(pm, 12, 4);
    float s[64][2];
    int n = 0;
    for (int k = 0; k <= k1; ++k) { const float th = PI_F + PI_F * (float)k / (float)k1; s[n][0] = cz + r * cosf(th); s[n][1] = r * sinf(th); n++; }
    if (L0) { s[n][0] = cz + r + 0.004f; s[n][1] = 0.004f; n++; }   /* the bead along the front */
    for (int k = k1; k >= 0; --k) { const float th = PI_F + PI_F * (float)k / (float)k1; s[n][0] = cz + (r - t) * cosf(th); s[n][1] = (r - t) * sinf(th); n++; }
    run_x(pm, (const float (*)[2])s, n, 0.0f, 1.0f);
}
void m_h_gutter_ogee(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    const float t = L2 ? 0.006f : 0.004f;
    const float o[8][2] = { { 0.008f, 0.0f }, { 0.008f, -0.076f }, { 0.07f, -0.076f }, { 0.098f, -0.062f }, { 0.112f, -0.04f },
                            { 0.108f, -0.022f }, { 0.12f, -0.008f }, { 0.124f, 0.0f } };
    float s[16][2];
    int n = 0;
    for (int k = 0; k < 8; ++k) { s[n][0] = o[k][0]; s[n][1] = o[k][1]; n++; }
    for (int k = 7; k >= 0; --k) {
        const float dz = k == 0 || k == 1 ? t : -t, dy = k >= 1 && k <= 2 ? t : 0.0f;
        s[n][0] = o[k][0] + dz; s[n][1] = o[k][1] + dy; n++;
    }
    run_x(pm, (const float (*)[2])s, n, 0.0f, 1.0f);
}
/* A stop end: a half disc closing a gutter, 6 mm thick (x 0..0.006). */
void m_h_gutter_stop(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    const float r = 0.064f, cz = 0.0655f;
    const int k1 = segs(pm, 10, 4);
    float s[32][2];
    int n = 0;
    for (int k = 0; k <= k1; ++k) { const float th = PI_F + PI_F * (float)k / (float)k1; s[n][0] = cz + r * cosf(th); s[n][1] = r * sinf(th) + 0.002f; n++; }
    run_x(pm, (const float (*)[2])s, n, 0.0f, 0.006f);
}

/* A swan neck: from a gutter's outlet (its top at the origin's height, dz out from the pipe's line) down and back
 * to the pipe's line (z 0), where the downpipe carries on below. */
static void swan(Pm *pm, float dz, float drop) {
    pm_mat(pm, PMAT_HRAIN);
    PmPath pa, f;
    pm_path_init(&pa, 4);
    pm_path_add(&pa, 0, 0.0f, dz, 0);
    pm_path_add(&pa, 0, -0.07f, dz, 0);
    pm_path_add(&pa, 0, -drop + 0.08f, 0.0f, 0);
    pm_path_add(&pa, 0, -drop, 0.0f, 0);
    pm_path_init(&f, 32);
    pm_path_fillet(&pa, 0.05f, L0 ? 4 : 2, &f);
    pm_tube(pm, &f, 0.034f, segs(pm, 12, 6), 3);
    pm_path_free(&pa); pm_path_free(&f);
    if (!L2) {   /* the outlet's socket */
        pm_push(pm); pm_translate(pm, 0.0f, -0.06f, dz);
        pm_cyl(pm, 0.0f, 0.055f, 0.039f, segs(pm, 12, 6));
        pm_pop(pm);
    }
}
void m_h_swan_neck(Pm *pm) { quiet(pm); swan(pm, HK_SWAN_DZ, 0.40f); }
void m_h_swan_neck_l(Pm *pm) { quiet(pm); swan(pm, 2.0f * HK_SWAN_DZ, 0.62f); }
/* A downpipe: 68 mm, y 0..1 (stretched), its axis the origin's (the assembly sets it HK_PIPE_Z off the wall). */
void m_h_downpipe(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    pm_cyl(pm, 0.0f, 1.0f, 0.034f, segs(pm, 12, 6));
}
/* A pipe clip: a band round the pipe (its axis the origin's), two lugs back to the wall (z -HK_PIPE_Z). */
void m_h_pipe_clip(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    PmProf b[5] = { { 0.034f, -0.016f, 1 }, { 0.041f, -0.016f, 1 }, { 0.041f, 0.016f, 1 }, { 0.034f, 0.016f, 1 }, { 0.034f, -0.016f, 1 } };
    pm_lathe(pm, b, 5, segs(pm, 12, 6), 0, 0);
    bx(pm, -0.065f, -0.016f, -HK_PIPE_Z, 0.065f, 0.016f, -HK_PIPE_Z + 0.02f, 0.002f);
    bx(pm, -0.012f, -0.016f, -HK_PIPE_Z + 0.015f, 0.012f, 0.016f, -0.03f, 0.0f);
    if (L0) {
        pm_mat(pm, PMAT_HIRON);
        for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, k ? 0.045f : -0.045f, 0.0f, -HK_PIPE_Z + 0.02f); pm_rotate_x(pm, HALF_PI); pm_cyl(pm, 0.0f, 0.006f, 0.008f, 6); pm_pop(pm); }
    }
}
/* A shoe at a downpipe's foot (on the ground at y 0): the pipe from 0.32 m bends out over the drain. */
void m_h_pipe_shoe(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    PmPath pa, f;
    pm_path_init(&pa, 3);
    pm_path_add(&pa, 0, 0.32f, 0.0f, 0);
    pm_path_add(&pa, 0, 0.10f, 0.0f, 0);
    pm_path_add(&pa, 0, 0.05f, 0.12f, 0);
    pm_path_init(&f, 16);
    pm_path_fillet(&pa, 0.06f, L0 ? 4 : 2, &f);
    pm_tube(pm, &f, 0.036f, segs(pm, 12, 6), 3);
    pm_path_free(&pa); pm_path_free(&f);
}
/* A hopper head (Victorian): open at the top (y 0), tapering down to an outlet over the pipe's line, its back
 * into the wall; a moulded band round its rim. */
void m_h_hopper(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    PmProf o[5] = { { 0, -0.26f, 0 }, { 0.02f, -0.26f, 0 }, { 0.03f, -0.20f, 1 }, { 0.07f, -0.05f, 0 }, { 0.075f, 0.0f, 0 } };
    PmProf s[40];
    int n = pm_prof_shell(o, 5, 0.006f, L0 ? 0.008f : 0.0f, L0 ? 4 : 2, s, 40);
    if (n) pm_lathe(pm, s, n, segs(pm, 16, 8), 0.07f, 0.0f);
    pm_cyl(pm, -0.34f, -0.24f, 0.036f, segs(pm, 12, 6));
}
/* Ridge tiles: three half-round tiles to a metre (x 0..1), each 0.11 m round, on the ridge's line (y 0) on a
 * mortar bedding that runs down into the slopes on either side. */
static void ridge_tiles(Pm *pm) {
    pm_mat(pm, PMAT_HCEMENT);
    const float bed[4][2] = { { -0.10f, 0.0f }, { -0.21f, -0.26f }, { 0.21f, -0.26f }, { 0.10f, 0.0f } };
    run_x(pm, bed, 4, 0.0f, 1.0f);
    pm_mat(pm, PMAT_HCLAY);
    const int k1 = segs(pm, 12, 4);
    float s[40][2];
    int n = 0;
    for (int k = 0; k <= k1; ++k) { const float th = PI_F * (float)k / (float)k1; s[n][0] = 0.112f * cosf(th); s[n][1] = 0.112f * sinf(th) - 0.012f; n++; }
    for (int k = k1; k >= 0; --k) { const float th = PI_F * (float)k / (float)k1; s[n][0] = 0.094f * cosf(th); s[n][1] = 0.094f * sinf(th) - 0.012f; n++; }
    if (L2) { run_x(pm, (const float (*)[2])s, n, 0.0f, 1.0f); return; }
    for (int t = 0; t < 3; ++t) { pm->part = (uint8_t)t; run_x(pm, (const float (*)[2])s, n, (float)t / 3.0f + 0.004f, (float)(t + 1) / 3.0f - 0.004f); }
    pm->part = 0;
}
void m_h_ridge_roll(Pm *pm) { quiet(pm); ridge_tiles(pm); }
/* Crested ridge tiles (Victorian): the rolls with a pierced-looking crest standing along their top. */
void m_h_ridge_crest(Pm *pm) { quiet(pm);
    ridge_tiles(pm);
    pm_mat(pm, PMAT_HCLAY);
    float s[64][2];
    int n = 0;
    s[n][0] = 1.0f; s[n][1] = 0.085f; n++;
    s[n][0] = 0.0f; s[n][1] = 0.085f; n++;
    const int per = L2 ? 2 : L1 ? 4 : 6;
    for (int k = 0; k < 6; ++k) {   /* a fleur on each half tile: a pointed lobe and a dip */
        const float a = (float)k / 6.0f, w = 1.0f / 6.0f;
        for (int i = 0; i < per; ++i) {
            const float t = (float)i / (float)per;
            const float lobe = sinf(PI_F * t);
            s[n][0] = a + w * t; s[n][1] = 0.13f + 0.09f * lobe * lobe * lobe + (fabsf(t - 0.5f) < 0.01f ? 0.02f : 0.0f); n++;
        }
    }
    s[n][0] = 1.0f; s[n][1] = 0.13f; n++;
    run_z(pm, (const float (*)[2])s, n, -0.009f, 0.009f);
}

/* ------------------------------------------------------------------ walls --- */
/* A stone string course (x 0..1): 0.15 m high from y 0, 50 mm proud, a drip under its nose, its top weathered. */
void m_h_string_course(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    if (L0) {
        const float s[7][2] = { { 0.0f, 0.0f }, { 0.04f, 0.0f }, { 0.04f, 0.008f }, { 0.05f, 0.012f }, { 0.05f, 0.115f }, { 0.044f, 0.122f }, { 0.0f, 0.15f } };
        run_x(pm, s, 7, 0.0f, 1.0f);
    } else {
        const float s[4][2] = { { 0.0f, 0.0f }, { 0.05f, 0.0f }, { 0.05f, 0.115f }, { 0.0f, 0.15f } };
        run_x(pm, s, 4, 0.0f, 1.0f);
    }
}
/* A plinth (x 0..1): 0.45 m from the ground at y 0, 40 mm proud, its top chamfered back to the wall. */
void m_h_plinth(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float s[4][2] = { { 0.0f, 0.0f }, { 0.04f, 0.0f }, { 0.04f, 0.39f }, { 0.0f, 0.45f } };
    run_x(pm, s, 4, 0.0f, 1.0f);
}
/* Quoins: two courses (y 0..0.6) of dressed stones wrapping a corner, long and short by turns, 20 mm proud. */
void m_h_quoin(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float p = 0.02f, b = 0.008f;
    for (int c = 0; c < 2; ++c) {
        const float y0 = 0.3f * (float)c + 0.006f, y1 = y0 + 0.288f;
        const float la = c ? 0.25f : 0.45f, lb = c ? 0.45f : 0.25f;
        pm->part = (uint8_t)c;
        bx(pm, -la, y0, -0.10f, p, y1, p, b);
        bx(pm, -0.10f, y0, -lb, p, y1, p, b);
    }
    pm->part = 0;
}
/* A soldier course over an opening (x 0..1): bricks on end, 13 a metre, 0.215 m high from y 0, 12 mm proud of a
 * mortar backing. */
void m_h_soldier(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    bx(pm, 0.0f, 0.0f, 0.0f, 1.0f, 0.215f, 0.004f, 0.0f);
    pm_mat(pm, PMAT_HBRICK);
    if (L2) { bx(pm, 0.0f, 0.004f, 0.0f, 1.0f, 0.211f, 0.012f, 0.0f); return; }
    const int n = 13;
    for (int k = 0; k < n; ++k) {
        pm->part = (uint8_t)k;
        const float x0 = (float)k / (float)n + 0.004f, x1 = (float)(k + 1) / (float)n - 0.004f;
        bx(pm, x0, 0.004f, 0.0f, x1, 0.211f, 0.012f, L0 ? 0.002f : 0.0f);
    }
    pm->part = 0;
}
/* A timber (mock-Tudor half-timbering): 160 mm wide (x), y 0..1 (stretched along its length), 35 mm proud. */
void m_h_timber(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTIMBER);
    bx(pm, -0.08f, 0.0f, 0.0f, 0.08f, 1.0f, 0.035f, 0.006f);
}
/* Hung tiles over a 1 m panel (x 0..1, y 0..1): courses at a 100 mm gauge, tiles 165 mm wide broken half a tile
 * course by course, each kicked out at its foot by the one under it. */
void m_h_hung_tiles(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTILE);
    const float g = 0.1f, tw = 1.0f / 6.0f;
    if (L2) { bx(pm, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.02f, 0.0f); return; }
    for (int c = 0; c < 10; ++c) {
        const float y0 = g * (float)c, y1 = minf(y0 + 0.19f, 1.0f);
        const float sec[4][2] = { { 0.012f, y0 }, { 0.026f, y0 }, { 0.026f - 0.014f * (y1 - y0) / 0.19f, y1 }, { 0.0f, y1 } };
        if (L1) { run_x(pm, sec, 4, 0.0f, 1.0f); continue; }
        const float off = (c & 1) ? 0.5f * tw : 0.0f;
        for (int k = -1; k < 7; ++k) {
            float x0 = off + tw * (float)k + 0.003f, x1 = off + tw * (float)(k + 1) - 0.003f;
            if (x1 <= 0.0f || x0 >= 1.0f) continue;
            x0 = maxf(x0, 0.0f); x1 = minf(x1, 1.0f);
            if (x1 - x0 < 0.02f) continue;
            pm->part = (uint8_t)(c * 8 + k + 1);
            run_x(pm, sec, 4, x0, x1);
        }
    }
    pm->part = 0;
}

/* --------------------------------------------------------------- openings --- */
/* Sills (x 0..1; the assembly sets them 80 mm past the opening each side): their top at the opening's foot
 * (y 0), from inside the reveal (z -0.16, behind the glass) to 60 mm past the wall, weathered to the front. */
void m_h_sill_stone(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    if (L0) {
        const float s[10][2] = { { -0.16f, 0.02f }, { -0.16f, -0.075f }, { 0.022f, -0.075f }, { 0.022f, -0.064f }, { 0.032f, -0.064f },
                                 { 0.032f, -0.075f }, { 0.06f, -0.075f }, { 0.06f, -0.012f }, { 0.0f, 0.01f }, { -0.02f, 0.02f } };
        run_x(pm, s, 10, 0.0f, 1.0f);
    } else {
        const float s[5][2] = { { -0.16f, 0.02f }, { -0.16f, -0.075f }, { 0.06f, -0.075f }, { 0.06f, -0.012f }, { 0.0f, 0.012f } };
        run_x(pm, s, 5, 0.0f, 1.0f);
    }
}
void m_h_sill_timber(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float s[6][2] = { { -0.14f, 0.018f }, { -0.14f, -0.035f }, { 0.05f, -0.035f }, { 0.05f, -0.014f }, { 0.0f, 0.008f }, { -0.02f, 0.018f } };
    run_x(pm, s, 6, 0.0f, 1.0f);
}
void m_h_sill_concrete(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    const float s[5][2] = { { -0.16f, 0.018f }, { -0.16f, -0.06f }, { 0.045f, -0.06f }, { 0.045f, -0.01f }, { 0.0f, 0.012f } };
    run_x(pm, s, 5, 0.0f, 1.0f);
}
/* Lintels (x 0..1; set past the opening each side): from the opening's head at y 0 up, proud of the wall. */
void m_h_lintel_stone(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float s[5][2] = { { 0.0f, 0.0f }, { 0.014f, 0.0f }, { 0.014f, 0.21f }, { 0.006f, 0.22f }, { 0.0f, 0.22f } };
    run_x(pm, s, 5, 0.0f, 1.0f);
}
void m_h_lintel_concrete(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HCEMENT);
    const float s[4][2] = { { 0.0f, 0.0f }, { 0.01f, 0.0f }, { 0.01f, 0.15f }, { 0.0f, 0.15f } };
    run_x(pm, s, 4, 0.0f, 1.0f);
}
/* A keystone over an opening's middle (x 0): wider at its top, dropping 30 mm under the head, 30 mm proud. */
void m_h_keystone(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float s[4][2] = { { -0.07f, -0.03f }, { 0.07f, -0.03f }, { 0.10f, 0.27f }, { -0.10f, 0.27f } };
    run_z(pm, s, 4, 0.0f, 0.03f);
    if (L0) bx(pm, -0.11f, 0.25f, 0.0f, 0.11f, 0.28f, 0.036f, 0.004f);
}
/* A segmental brick arch over an opening (x 0..1, its head at y 0): voussoirs rising 80 mm at the middle, a
 * brick and a half deep (0.225), 12 mm proud of a mortar backing. */
void m_h_arch_brick(Pm *pm) { quiet(pm);
    const float rise = 0.08f, R = (0.25f + rise * rise) / (2.0f * rise), cy = rise - R, D = 0.225f;
    const float a = asinf(0.5f / R);
    const int n = L2 ? 5 : 13;
    pm_mat(pm, PMAT_HCEMENT);
    {   /* the backing: the whole band */
        float s[32][2];
        int m = 0;
        const int k1 = L2 ? 4 : 10;
        for (int k = 0; k <= k1; ++k) { const float t = -a + 2.0f * a * (float)k / (float)k1; s[m][0] = 0.5f + R * sinf(t); s[m][1] = cy + R * cosf(t); m++; }
        for (int k = k1; k >= 0; --k) { const float t = -a + 2.0f * a * (float)k / (float)k1; s[m][0] = 0.5f + (R + D) * sinf(t); s[m][1] = cy + (R + D) * cosf(t); m++; }
        run_z(pm, (const float (*)[2])s, m, 0.0f, 0.004f);
    }
    pm_mat(pm, PMAT_HBRICK);
    const float gap = 0.004f / R;
    for (int k = 0; k < n; ++k) {
        const float t0 = -a + 2.0f * a * (float)k / (float)n + gap, t1 = -a + 2.0f * a * (float)(k + 1) / (float)n - gap;
        const float s[4][2] = { { 0.5f + R * sinf(t0), cy + R * cosf(t0) }, { 0.5f + R * sinf(t1), cy + R * cosf(t1) },
                                { 0.5f + (R + D) * sinf(t1), cy + (R + D) * cosf(t1) }, { 0.5f + (R + D) * sinf(t0), cy + (R + D) * cosf(t0) } };
        pm->part = (uint8_t)k;
        run_z(pm, s, 4, 0.0f, 0.012f);
    }
    pm->part = 0;
}
/* A hood mould (Victorian label) over an opening (x 0..1, its head at y 0): a moulded drip 50 mm over the head
 * running 80 mm past each side, its returns dropping 0.2 m to carved stops. */
void m_h_hood_mould(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HSTONE);
    const float s[6][2] = { { 0.0f, 0.05f }, { 0.03f, 0.05f }, { 0.046f, 0.066f }, { 0.05f, 0.09f }, { 0.03f, 0.12f }, { 0.0f, 0.13f } };
    run_x(pm, s, 6, -0.14f, 1.14f);
    for (int k = 0; k < 2; ++k) {
        const float x = k ? 1.08f : -0.08f;
        bx(pm, x - 0.06f, -0.13f, 0.0f, x + 0.06f, 0.06f, 0.045f, 0.006f);
        if (!L2) bx(pm, x - 0.05f, -0.21f, 0.0f, x + 0.05f, -0.12f, 0.055f, L0 ? 0.015f : 0.0f);   /* the stop */
    }
}
/* A craftsman's casing round an opening HK_GRID_W x HK_GRID_H (its foot at y 0, centred), on the wall's face
 * (z 0): side boards, a frieze board over the head and a cap with a drip. */
void m_h_casing(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float w = 0.5f * HK_GRID_W, h = HK_GRID_H;
    bx(pm, -w - 0.11f, -0.04f, 0.0f, -w, h, 0.022f, 0.003f);
    bx(pm, w, -0.04f, 0.0f, w + 0.11f, h, 0.022f, 0.003f);
    bx(pm, -w - 0.13f, h, 0.0f, w + 0.13f, h + 0.17f, 0.025f, 0.003f);
    const float cap[6][2] = { { 0.0f, h + 0.17f }, { 0.045f, h + 0.17f }, { 0.045f, h + 0.18f }, { 0.055f, h + 0.19f }, { 0.055f, h + 0.215f }, { 0.0f, h + 0.215f } };
    run_x(pm, cap, 6, -w - 0.17f, w + 0.17f);
}

/* Frames: centred on x 0, the opening's foot at y 0, the glass's plane at z 0, out to +z (under the wall's face:
 * the shallowest reveal is 0.10 m). Painted timber but the plastic replacement. */
/* A sash window HK_SASH_W x HK_SASH_H: a box frame, two sashes (the upper proud of the lower) meeting at the
 * middle; panes split by bars: `vbars` upright bars a sash and `hbars` across (Georgian six over six: 2, 1),
 * horns under the upper sash's meeting rail (Victorian). */
static void sash(Pm *pm, float W, float H, int vbars, int hbars, int horns, int vtop_only) {
    pm_mat(pm, PMAT_HTRIM);
    const float x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.06f;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.07f, fw, 0.0f, 0.06f, 0.004f);
    const float ym = 0.07f + 0.5f * (H - 0.13f);
    const float sx0 = x0 + fw, sx1 = x1 - fw;
    /* lower sash, inner */
    rect_frame(pm, sx0, 0.07f, sx1, ym + 0.02f, 0.045f, 0.07f, 0.035f, 0.008f, 0.045f, 0.003f);
    /* upper sash, outer */
    rect_frame(pm, sx0, ym - 0.02f, sx1, H - fw, 0.045f, 0.035f, 0.05f, 0.045f, 0.08f, 0.003f);
    if (L2) return;
    const float gx0 = sx0 + 0.045f, gx1 = sx1 - 0.045f, bw = vbars > 1 ? 0.009f : 0.011f;
    for (int s = 0; s < 2; ++s) {
        const float y0 = s ? ym + 0.015f : 0.14f, y1 = s ? H - fw - 0.05f : ym - 0.015f;
        const float z0 = s ? 0.05f : 0.013f, z1 = s ? 0.075f : 0.04f;
        if (vtop_only && !s) continue;
        for (int k = 1; k <= vbars; ++k) { const float x = gx0 + (gx1 - gx0) * (float)k / (float)(vbars + 1); bx(pm, x - bw, y0, z0, x + bw, y1, z1, 0.0f); }
        for (int k = 1; k <= hbars; ++k) { const float y = y0 + (y1 - y0) * (float)k / (float)(hbars + 1); bx(pm, gx0, y - bw, z0, gx1, y + bw, z1, 0.0f); }
    }
    if (horns && L0)
        for (int k = 0; k < 2; ++k) {
            const float xa = k ? sx1 - 0.045f : sx0, xb = k ? sx1 : sx0 + 0.045f;
            bx(pm, xa, ym - 0.09f, 0.045f, xb, ym - 0.02f, 0.08f, 0.004f);
        }
    if (L0) {   /* sash lifts */
        pm_mat(pm, PMAT_HIRON);
        for (int k = 0; k < 2; ++k) { const float x = (k ? 0.25f : -0.25f) * W; bx(pm, x - 0.025f, 0.10f, 0.045f, x + 0.025f, 0.13f, 0.06f, 0.0f); }
    }
}
void m_h_win_sash(Pm *pm) { quiet(pm); sash(pm, HK_SASH_W, HK_SASH_H, 1, 0, 1, 0); }
void m_h_win_georgian(Pm *pm) { quiet(pm); sash(pm, HK_SASH_W, HK_SASH_H, 2, 1, 0, 0); }
void m_h_win_grid(Pm *pm) { quiet(pm); sash(pm, HK_GRID_W, HK_GRID_H, 3, 0, 0, 1); }
/* Casements W x H: a frame, a mullion, a transom at 0.72 of the height, opening leaves proud of it below and top
 * lights above; bars (inter-war): upright in the top lights, across the leaves. */
static void casements(Pm *pm, float W, float H, int bars, int mat) {
    pm_mat(pm, mat);
    const float x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.055f, yt = 0.72f * H;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.07f, fw, 0.0f, 0.06f, 0.004f);
    bx(pm, x0 + fw, yt - 0.03f, 0.0f, x1 - fw, yt + 0.03f, 0.065f, 0.004f);
    bx(pm, -0.03f, 0.07f, 0.0f, 0.03f, H - fw, 0.06f, 0.004f);
    for (int s = 0; s < 2; ++s)
        for (int r = 0; r < 2; ++r) {
            const float lx0 = s ? 0.03f : x0 + fw, lx1 = s ? x1 - fw : -0.03f;
            const float ly0 = r ? yt + 0.03f : 0.07f, ly1 = r ? H - fw : yt - 0.03f;
            if (!L2) rect_frame(pm, lx0, ly0, lx1, ly1, 0.04f, 0.045f, 0.04f, 0.04f, 0.075f, 0.003f);
            if (bars && !L2) {
                if (r) { const float x = 0.5f * (lx0 + lx1); bx(pm, x - 0.01f, ly0 + 0.04f, 0.045f, x + 0.01f, ly1 - 0.04f, 0.068f, 0.0f); }
                else for (int k = 1; k <= 2; ++k) { const float y = ly0 + (ly1 - ly0) * (float)k / 3.0f; bx(pm, lx0 + 0.04f, y - 0.01f, 0.045f, lx1 - 0.04f, y + 0.01f, 0.068f, 0.0f); }
            }
        }
}
void m_h_win_casement(Pm *pm) { quiet(pm); casements(pm, HK_CASE_W, HK_CASE_H, 1, PMAT_HTRIM); }
/* A ranch's picture window HK_PICT_W x HK_PICT_H: a heavy frame, a fixed light between two narrow opening lights. */
void m_h_win_picture(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float W = HK_PICT_W, H = HK_PICT_H, x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.07f, m = 0.5f * W - 0.38f;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.08f, fw, 0.0f, 0.06f, 0.004f);
    for (int k = 0; k < 2; ++k) { const float x = k ? m : -m; bx(pm, x - 0.03f, 0.08f, 0.0f, x + 0.03f, H - fw, 0.065f, 0.004f); }
    if (!L2)
        for (int k = 0; k < 2; ++k) {
            const float lx0 = k ? m + 0.03f : x0 + fw, lx1 = k ? x1 - fw : -m - 0.03f;
            rect_frame(pm, lx0, 0.08f, lx1, H - fw, 0.04f, 0.045f, 0.04f, 0.04f, 0.072f, 0.003f);
        }
}
/* A plastic replacement window HK_UPVC_W x HK_UPVC_H: a chunky white frame, a top-hung opener over a fixed
 * light and a side-hung one (the mismatched replacement along a row). */
void m_h_win_upvc(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HUPVC);
    const float W = HK_UPVC_W, H = HK_UPVC_H, x0 = -0.5f * W, x1 = 0.5f * W, fw = 0.065f, yt = 0.76f * H;
    rect_frame(pm, x0, 0.0f, x1, H, fw, 0.075f, fw, 0.0f, 0.07f, L2 ? 0.0f : 0.01f);
    bx(pm, x0 + fw, yt - 0.03f, 0.0f, x1 - fw, yt + 0.03f, 0.07f, L2 ? 0.0f : 0.008f);
    bx(pm, -0.03f, 0.075f, 0.0f, 0.03f, yt - 0.03f, 0.07f, L2 ? 0.0f : 0.008f);
    if (L2) return;
    rect_frame(pm, x0 + fw, yt + 0.03f, x1 - fw, H - fw, 0.055f, 0.055f, 0.055f, 0.05f, 0.085f, 0.01f);
    rect_frame(pm, x0 + fw, 0.075f, -0.03f, yt - 0.03f, 0.055f, 0.055f, 0.055f, 0.05f, 0.085f, 0.01f);
    if (L0) { bx(pm, -0.07f, yt + 0.035f, 0.085f, -0.03f, yt + 0.05f, 0.11f, 0.004f); bx(pm, -0.07f, 0.5f * yt - 0.06f, 0.085f, -0.055f, 0.5f * yt + 0.06f, 0.105f, 0.004f); }
}

/* Shutters: a leaf centred on x 0 (0.5 x 1.6, stretched to half the opening's width and its height), flat on the
 * wall (z 0..0.035) beside the opening; louvred (slats in two panels) or panelled; hinges both sides (a leaf
 * serves either hand). */
static void shutter_frame(Pm *pm) {
    pm_mat(pm, PMAT_HDOOR);
    rect_frame(pm, -0.25f, 0.0f, 0.25f, 1.6f, 0.06f, 0.09f, 0.06f, 0.0f, 0.035f, 0.003f);
    bx(pm, -0.19f, 0.76f, 0.0f, 0.19f, 0.84f, 0.035f, 0.003f);
    if (L0) {
        pm_mat(pm, PMAT_HIRON);
        for (int k = 0; k < 4; ++k) { const float x = (k & 1) ? 0.25f : -0.25f, y = (k & 2) ? 1.35f : 0.25f; bx(pm, x - 0.012f, y - 0.03f, 0.0f, x + 0.012f, y + 0.03f, 0.045f, 0.0f); }
    }
}
void m_h_shutter_louvre(Pm *pm) { quiet(pm);
    shutter_frame(pm);
    pm_mat(pm, PMAT_HDOOR);
    if (L2) { bx(pm, -0.19f, 0.09f, 0.002f, 0.19f, 1.54f, 0.02f, 0.0f); return; }
    const float pitch = L0 ? 0.045f : 0.09f;
    for (int p = 0; p < 2; ++p) {
        const float y0 = p ? 0.84f : 0.09f, y1 = p ? 1.54f : 0.76f;
        for (float y = y0 + 0.01f; y + 0.03f <= y1; y += pitch) {
            const float s[4][2] = { { 0.004f, y }, { 0.014f, y }, { 0.03f, y + 0.035f }, { 0.02f, y + 0.035f } };
            run_x(pm, s, 4, -0.19f, 0.19f);
        }
    }
}
void m_h_shutter_panel(Pm *pm) { quiet(pm);
    shutter_frame(pm);
    pm_mat(pm, PMAT_HDOOR);
    bx(pm, -0.19f, 0.09f, 0.0f, 0.19f, 1.54f, 0.02f, 0.0f);
    field(pm, -0.19f, 0.09f, 0.19f, 0.76f, 0.02f, 0.03f);
    field(pm, -0.19f, 0.84f, 0.19f, 1.54f, 0.02f, 0.03f);
}
/* A security grille over a window (x -0.5..0.5, y 0..1, stretched to the opening and a margin), on the wall's
 * face: a flat frame, round bars every 0.12 m, two flats across, fixing lugs. */
void m_h_grille(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HIRON);
    rect_frame(pm, -0.5f, 0.0f, 0.5f, 1.0f, 0.03f, 0.03f, 0.03f, 0.0f, 0.012f, 0.0f);
    for (int k = 1; k <= 2; ++k) { const float y = (float)k / 3.0f; bx(pm, -0.47f, y - 0.015f, 0.006f, 0.47f, y + 0.015f, 0.032f, 0.0f); }
    const int nb = L2 ? 4 : 8;
    for (int k = 1; k < nb; ++k) {
        const float x = -0.5f + (float)k / (float)nb;
        if (L2) { bx(pm, x - 0.008f, 0.03f, 0.008f, x + 0.008f, 0.97f, 0.024f, 0.0f); continue; }
        pm_push(pm); pm_translate(pm, x, 0.0f, 0.018f);
        pm_cyl(pm, 0.03f, 0.97f, 0.008f, segs(pm, 8, 4));
        pm_pop(pm);
    }
}
/* A window box (x 0..1, its top at y 0, against the wall's face): a painted timber trough on two iron brackets,
 * soil and plants spilling over its edge. */
void m_h_window_box(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, 0.0f, -0.18f, 0.0f, 1.0f, 0.0f, 0.2f, 0.004f);
    pm_mat(pm, PMAT_HIRON);
    for (int k = 0; k < 2; ++k) {
        const float x = k ? 0.85f : 0.15f;
        const float s[3][2] = { { 0.0f, -0.18f }, { 0.0f, -0.36f }, { 0.17f, -0.18f } };
        run_x(pm, s, 3, x - 0.012f, x + 0.012f);
    }
    pm_mat(pm, PMAT_HPLANT);
    uint32_t h = 0x7B0Bu;
    const int n = L2 ? 3 : L1 ? 6 : 11;
    for (int k = 0; k < n; ++k) {
        const float x = 0.06f + 0.88f * ((float)k + 0.3f + 0.4f * rnd(&h)) / (float)n, z = 0.06f + 0.1f * rnd(&h);
        const float r = 0.07f + 0.05f * rnd(&h), hh = 0.12f + 0.14f * rnd(&h);
        pm->part = (uint8_t)k;
        pm_push(pm); pm_translate(pm, x, -0.03f, z);
        PmProf p[5] = { { 0, 0, 0 }, { r, 0.02f, 0 }, { 0.95f * r, 0.45f * hh, 0 }, { 0.5f * r, 0.85f * hh, 0 }, { 0, hh, 0 } };
        pm_lathe(pm, p, 5, segs(pm, 9, 5), 0, 0);
        pm_pop(pm);
    }
    pm->part = 0;
}

/* ------------------------------------------------------------------ doors --- */
/* Door leaves HK_DOOR_W x HK_DOOR_H, centred, their foot at y 0, their back on the door's plane (z 0): stiles and
 * rails 45 mm proud, panels between at 25 mm with raised fields; furniture at L0. */
static void leaf_frame(Pm *pm, float stile, float bottom, float top) {
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H;
    bx(pm, x0, 0.0f, 0.0f, x1, H, 0.025f, 0.0f);
    rect_frame(pm, x0, 0.0f, x1, H, stile, bottom, top, 0.0f, 0.045f, 0.003f);
}
static void furniture(Pm *pm, int letterbox, int knocker, float lockY) {
    if (!L0) return;
    pm_mat(pm, PMAT_HIRON);
    const float x1 = 0.5f * HK_DOOR_W;
    pm_push(pm); pm_translate(pm, x1 - 0.07f, lockY, 0.045f); pm_rotate_x(pm, HALF_PI);
    PmProf k[5] = { { 0, 0, 0 }, { 0.025f, 0, 1 }, { 0.012f, 0.03f, 0 }, { 0.028f, 0.06f, 0 }, { 0, 0.07f, 0 } };
    pm_lathe(pm, k, 5, 10, 0, 0);
    pm_pop(pm);
    if (letterbox) bx(pm, -0.14f, lockY - 0.03f, 0.04f, 0.14f, lockY + 0.03f, 0.052f, 0.002f);
    if (knocker) {
        bx(pm, -0.03f, 1.48f, 0.04f, 0.03f, 1.56f, 0.055f, 0.003f);
        pm_push(pm); pm_translate(pm, 0.0f, 1.44f, 0.06f); pm_rotate_x(pm, HALF_PI);
        pm_torus(pm, 0.04f, 0.007f, 10, 5);
        pm_pop(pm);
    }
}
void m_h_door_4p(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H, st = 0.11f;
    leaf_frame(pm, st, 0.22f, 0.11f);
    bx(pm, x0 + st, 0.9f, 0.0f, x1 - st, 1.08f, 0.045f, 0.003f);
    bx(pm, -0.05f, 0.22f, 0.0f, 0.05f, 0.9f, 0.045f, 0.003f);
    bx(pm, -0.05f, 1.08f, 0.0f, 0.05f, H - 0.11f, 0.045f, 0.003f);
    for (int k = 0; k < 4; ++k) {
        const float px0 = (k & 1) ? 0.05f : x0 + st, px1 = (k & 1) ? x1 - st : -0.05f;
        const float py0 = (k & 2) ? 1.08f : 0.22f, py1 = (k & 2) ? H - 0.11f : 0.9f;
        field(pm, px0, py0, px1, py1, 0.025f, 0.036f);
    }
    furniture(pm, 1, 1, 0.99f);
}
void m_h_door_6p(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H, st = 0.11f;
    leaf_frame(pm, st, 0.22f, 0.11f);
    const float rails[2][2] = { { 0.84f, 1.0f }, { 1.52f, 1.62f } };
    for (int r = 0; r < 2; ++r) bx(pm, x0 + st, rails[r][0], 0.0f, x1 - st, rails[r][1], 0.045f, 0.003f);
    bx(pm, -0.05f, 0.22f, 0.0f, 0.05f, H - 0.11f, 0.045f, 0.003f);
    const float ys[3][2] = { { 0.22f, 0.84f }, { 1.0f, 1.52f }, { 1.62f, H - 0.11f } };
    for (int r = 0; r < 3; ++r)
        for (int s = 0; s < 2; ++s) field(pm, s ? 0.05f : x0 + st, ys[r][0], s ? x1 - st : -0.05f, ys[r][1], 0.025f, 0.036f);
    furniture(pm, 1, 1, 0.92f);
}
void m_h_door_craft(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H, st = 0.11f;
    leaf_frame(pm, st, 0.22f, 0.11f);
    bx(pm, x0 + st, 1.36f, 0.0f, x1 - st, 1.48f, 0.045f, 0.003f);
    for (int k = 1; k <= 2; ++k) { const float x = x0 + st + (x1 - x0 - 2.0f * st) * (float)k / 3.0f; bx(pm, x - 0.02f, 1.48f, 0.0f, x + 0.02f, H - 0.11f, 0.045f, 0.0f); }
    bx(pm, -0.04f, 0.22f, 0.0f, 0.04f, 1.36f, 0.045f, 0.003f);
    field(pm, x0 + st, 0.22f, -0.04f, 1.36f, 0.025f, 0.036f);
    field(pm, 0.04f, 0.22f, x1 - st, 1.36f, 0.025f, 0.036f);
    /* the dentil shelf under the lights */
    const float sh[4][2] = { { 0.045f, 1.44f }, { 0.085f, 1.44f }, { 0.085f, 1.475f }, { 0.045f, 1.475f } };
    run_x(pm, sh, 4, x0 + 0.04f, x1 - 0.04f);
    if (L0) for (int k = 0; k < 9; ++k) { const float x = x0 + 0.08f + (x1 - x0 - 0.16f) * (float)k / 8.0f; bx(pm, x - 0.012f, 1.405f, 0.045f, x + 0.012f, 1.44f, 0.07f, 0.0f); }
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, x0 + st, 1.48f, 0.024f, x1 - st, H - 0.11f, 0.03f, 0.0f);
    furniture(pm, 0, 0, 0.98f);
}
void m_h_door_half(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HDOOR);
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H, st = 0.10f;
    leaf_frame(pm, st, 0.2f, 0.1f);
    bx(pm, x0 + st, 1.0f, 0.0f, x1 - st, 1.1f, 0.045f, 0.003f);
    bx(pm, -0.04f, 0.2f, 0.0f, 0.04f, 1.0f, 0.045f, 0.003f);
    field(pm, x0 + st, 0.2f, -0.04f, 1.0f, 0.025f, 0.036f);
    field(pm, 0.04f, 0.2f, x1 - st, 1.0f, 0.025f, 0.036f);
    bx(pm, x0 + st, 1.52f, 0.0f, x1 - st, 1.56f, 0.04f, 0.0f);
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, x0 + st, 1.1f, 0.024f, x1 - st, H - 0.1f, 0.03f, 0.0f);
    furniture(pm, 1, 0, 0.95f);
}
void m_h_door_upvc(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HUPVC);
    const float x0 = -0.5f * HK_DOOR_W, x1 = 0.5f * HK_DOOR_W, H = HK_DOOR_H;
    bx(pm, x0, 0.0f, 0.0f, x1, H, 0.03f, 0.0f);
    rect_frame(pm, x0, 0.0f, x1, H, 0.09f, 0.12f, 0.09f, 0.0f, 0.06f, L2 ? 0.0f : 0.012f);
    rect_frame(pm, x0 + 0.17f, 1.15f, x1 - 0.17f, H - 0.17f, 0.03f, 0.03f, 0.03f, 0.028f, 0.045f, 0.0f);
    field(pm, x0 + 0.15f, 0.25f, x1 - 0.15f, 1.0f, 0.03f, 0.042f);
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, x0 + 0.2f, 1.18f, 0.026f, x1 - 0.2f, H - 0.2f, 0.032f, 0.0f);
    if (L0) {
        pm_mat(pm, PMAT_HIRON);
        bx(pm, x1 - 0.1f, 0.85f, 0.03f, x1 - 0.075f, 1.15f, 0.045f, 0.003f);
        bx(pm, x1 - 0.22f, 0.98f, 0.06f, x1 - 0.08f, 1.0f, 0.08f, 0.003f);
    }
}

/* A fanlight HK_FAN_W x HK_FAN_H over a door, on the wall's face (z 0): a frame round glass, a sunburst of bars
 * from the middle of its foot and a half ring through them. */
void m_h_fanlight(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HGLASS);
    const float W = HK_FAN_W, H = HK_FAN_H, x0 = -0.5f * W, x1 = 0.5f * W;
    bx(pm, x0 + 0.04f, 0.04f, 0.0f, x1 - 0.04f, H - 0.04f, 0.012f, 0.0f);
    pm_mat(pm, PMAT_HTRIM);
    rect_frame(pm, x0, 0.0f, x1, H, 0.05f, 0.05f, 0.05f, 0.0f, 0.05f, 0.003f);
    if (L2) return;
    const int spokes = L0 ? 9 : 5;
    const float c[3] = { 0.0f, 0.05f, 0.03f };
    for (int k = 1; k < spokes; ++k) {
        const float a = PI_F * (float)k / (float)spokes;
        float dx = cosf(a), dy = sinf(a);
        const float tx = fabsf(dx) > 1e-4f ? (0.5f * W - 0.05f) / fabsf(dx) : 1e9f, ty = (H - 0.1f) / dy, t = minf(tx, ty);
        float b[3] = { dx * t, 0.05f + dy * t, 0.03f };
        float up[3] = { 0, 0, 1 };
        pm_beam(pm, c, b, 0.014f, 0.022f, 0.0f, up);
    }
    PmPath pa;
    pm_path_init(&pa, 24);
    const float ex[3] = { 1, 0, 0 }, ey[3] = { 0, 1, 0 }, cc[3] = { 0.0f, 0.05f, 0.03f };
    pm_path_arc(&pa, cc, ex, ey, 0.22f, 0.0f, PI_F, L0 ? 14 : 8);
    pm_tube(pm, &pa, 0.009f, 6, 3);
    pm_path_free(&pa);
}
/* Doorcases round a door opening 1.0 x 2.1 (centred, its foot at y 0) on the wall's face: pilasters with bases
 * and capitals, a frieze and a cornice (or a pediment), the fanlight over the door between the pilasters. */
static void doorcase(Pm *pm, int pediment) {
    pm_mat(pm, PMAT_HTRIM);
    for (int s = 0; s < 2; ++s) {
        const float xa = s ? 0.52f : -0.72f, xb = xa + 0.2f;
        bx(pm, xa - 0.01f, 0.0f, 0.0f, xb + 0.01f, 0.24f, 0.075f, 0.004f);
        bx(pm, xa, 0.24f, 0.0f, xb, 2.5f, 0.055f, 0.003f);
        if (L0) for (int f = 0; f < 2; ++f) { const float x = xa + 0.065f + 0.07f * (float)f; bx(pm, x - 0.012f, 0.34f, 0.055f, x + 0.012f, 2.4f, 0.058f, 0.0f); }
        bx(pm, xa - 0.02f, 2.5f, 0.0f, xb + 0.02f, 2.64f, 0.08f, 0.004f);
    }
    bx(pm, -0.76f, 2.64f, 0.0f, 0.76f, 2.84f, 0.06f, 0.003f);
    const float cor[7][2] = { { 0.0f, 2.84f }, { 0.08f, 2.84f }, { 0.1f, 2.87f }, { 0.16f, 2.9f }, { 0.17f, 2.95f }, { 0.17f, 2.99f }, { 0.0f, 2.99f } };
    run_x(pm, cor, 7, -0.9f, 0.9f);
    if (pediment) {
        const float a = 25.0f * DEG, half = 0.92f, rise = half * tanf(a);
        const float tym[3][2] = { { -0.86f, 2.99f }, { 0.86f, 2.99f }, { 0.0f, 2.99f + 0.86f * tanf(a) } };
        run_z(pm, tym, 3, 0.0f, 0.06f);
        for (int s = 0; s < 2; ++s) {
            const float sx = s ? 1.0f : -1.0f;
            float p[4][3] = { { sx * half, 2.99f, 0.17f }, { 0.0f, 2.99f + rise, 0.17f }, { 0.0f, 2.99f + rise + 0.1f, 0.17f }, { sx * (half + 0.03f), 3.08f, 0.17f } };
            const float n[3] = { 0, 0, 1 };
            slab(pm, (const float (*)[3])p, 4, n, 0.17f);
        }
    }
    pm_push(pm); pm_translate(pm, 0.0f, 2.12f, 0.0f); pm_place(pm, PMK_H_FANLIGHT); pm_pop(pm);
}
void m_h_doorcase(Pm *pm) { quiet(pm); doorcase(pm, 0); }
void m_h_doorcase_ped(Pm *pm) { quiet(pm); doorcase(pm, 1); }

/* ---------------------------------------------------------------- porches --- */
/* Porch posts HK_POST_H high, standing on y 0 centred (stretched to the porch's eaves). */
void m_h_post_turned(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -0.055f, 0.0f, -0.055f, 0.055f, 0.42f, 0.055f, 0.005f);
    bx(pm, -0.055f, 2.0f, -0.055f, 0.055f, 2.34f, 0.055f, 0.005f);
    bx(pm, -0.07f, 2.34f, -0.07f, 0.07f, HK_POST_H, 0.07f, 0.005f);
    PmProf p[16] = { { 0, 0.42f, 0 }, { 0.05f, 0.42f, 1 }, { 0.05f, 0.47f, 1 }, { 0.036f, 0.52f, 0 }, { 0.05f, 0.68f, 0 }, { 0.04f, 0.9f, 0 },
                     { 0.031f, 1.15f, 0 }, { 0.045f, 1.24f, 1 }, { 0.045f, 1.29f, 1 }, { 0.032f, 1.34f, 0 }, { 0.034f, 1.7f, 0 },
                     { 0.044f, 1.86f, 0 }, { 0.05f, 1.93f, 1 }, { 0.05f, 2.0f, 1 }, { 0, 2.0f, 0 } };
    pm_lathe(pm, p, 15, segs(pm, 14, 6), 0, 0);
}
/* A craftsman column: a brick pier with a stone cap to 1.03 m, a tapered square column on it, a capital. */
void m_h_post_craft(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HBRICK);
    bx(pm, -0.23f, 0.0f, -0.23f, 0.23f, 0.95f, 0.23f, 0.004f);
    pm_mat(pm, PMAT_HSTONE);
    bx(pm, -0.26f, 0.95f, -0.26f, 0.26f, 1.03f, 0.26f, 0.006f);
    pm_mat(pm, PMAT_HTRIM);
    frustum(pm, 1.03f, 2.24f, 0.14f, 0.14f, 0.095f, 0.095f);
    bx(pm, -0.13f, 2.24f, -0.13f, 0.13f, HK_POST_H, 0.13f, 0.005f);
}
/* A plain post with stopped chamfers, a base block and a capital. */
void m_h_post_square(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -0.06f, 0.0f, -0.06f, 0.06f, 0.2f, 0.06f, 0.004f);
    bx(pm, -0.05f, 0.2f, -0.05f, 0.05f, 2.28f, 0.05f, L0 ? 0.012f : 0.0f);
    bx(pm, -0.065f, 2.28f, -0.065f, 0.065f, HK_POST_H, 0.065f, 0.005f);
}
/* A spandrel (Victorian fretwork) in the corner between a post and the beam: in the porch front's plane (z
 * -0.015..0.015), from the post's face (x 0) out along the beam to 0.45, under it (y 0) down to -0.45, its inner
 * edge a concave quarter circle; a drop at its middle. */
void m_h_spandrel(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    float s[32][2];
    int n = 0;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    s[n][0] = 0.0f; s[n][1] = -0.45f; n++;
    s[n][0] = 0.04f; s[n][1] = -0.45f; n++;
    const int k1 = segs(pm, 12, 4);
    for (int k = 0; k <= k1; ++k) {   /* from the post round to the beam, about (0.45, -0.45) */
        const float a = PI_F + HALF_PI * (float)k / (float)k1;   /* 180..270 degrees: up the post's side to the beam */
        s[n][0] = 0.45f + 0.41f * cosf(a); s[n][1] = -0.45f - 0.41f * sinf(a);
        n++;
    }
    s[n][0] = 0.45f; s[n][1] = -0.04f; n++;
    s[n][0] = 0.45f; s[n][1] = 0.0f; n++;
    run_z(pm, (const float (*)[2])s, n, -0.015f, 0.015f);
    if (L0) {
        pm_push(pm); pm_translate(pm, 0.16f, -0.16f, 0.0f);
        PmProf d[5] = { { 0, -0.07f, 0 }, { 0.012f, -0.06f, 0 }, { 0.02f, -0.025f, 0 }, { 0.012f, 0.0f, 0 }, { 0, 0.0f, 0 } };
        pm_lathe(pm, d, 5, 8, 0, 0);
        pm_pop(pm);
    }
}
/* A porch balustrade (x 0..1, stretched little: hk_balustrade lays whole metres), centred on z 0: a handrail at
 * 0.9 m, a bottom rail, square balusters every 0.11 m. */
void m_h_balustrade(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float top[6][2] = { { -0.035f, 0.85f }, { 0.035f, 0.85f }, { 0.035f, 0.88f }, { 0.02f, 0.9f }, { -0.02f, 0.9f }, { -0.035f, 0.88f } };
    run_x(pm, top, 6, 0.0f, 1.0f);
    bx(pm, 0.0f, 0.05f, -0.03f, 1.0f, 0.11f, 0.03f, 0.003f);
    const int n = 9;
    for (int k = 0; k < n; ++k) {
        const float x = (float)k / (float)n + 0.5f / (float)n;
        bx(pm, x - 0.02f, 0.11f, -0.02f, x + 0.02f, 0.85f, 0.02f, L0 ? 0.004f : 0.0f);
    }
}
/* Door canopies, against the wall (z 0) over a door: the origin at the middle of the canopy's underside at the
 * wall. Brackets: an S-curved console, 70 mm thick, on the wall from 0.42 m under the canopy, reaching out. */
static void canopy_bracket(Pm *pm, float x, float reach) {
    float s[24][2];
    int n = 0;
    s[n][0] = 0.0f; s[n][1] = -0.42f; n++;
    const int k1 = segs(pm, 10, 4);
    for (int k = 0; k <= k1; ++k) {
        const float t = (float)k / (float)k1;
        s[n][0] = 0.03f + (reach - 0.05f) * t;
        s[n][1] = -0.42f + 0.38f * (0.5f - 0.5f * cosf(PI_F * t)) + 0.03f * sinf(2.0f * PI_F * t);
        n++;
    }
    s[n][0] = reach; s[n][1] = 0.0f; n++;
    s[n][0] = 0.0f; s[n][1] = 0.0f; n++;
    run_x(pm, (const float (*)[2])s, n, x - 0.035f, x + 0.035f);
}
/* A flat canopy 1.4 x 0.65: a boxed timber hood with a lead top on two brackets. */
void m_h_canopy_flat(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -0.7f, 0.0f, 0.0f, 0.7f, 0.15f, 0.65f, 0.004f);
    if (!L2) { const float c[4][2] = { { 0.65f, 0.11f }, { 0.68f, 0.11f }, { 0.68f, 0.17f }, { 0.65f, 0.17f } }; run_x(pm, c, 4, -0.73f, 0.73f); }
    pm_mat(pm, PMAT_HLEAD);
    { float p[4][3] = { { -0.71f, 0.17f, 0.0f }, { 0.71f, 0.17f, 0.0f }, { 0.71f, 0.15f, 0.67f }, { -0.71f, 0.15f, 0.67f } }; const float n[3] = { 0, 1, 0 }; slab(pm, (const float (*)[3])p, 4, n, 0.02f); }
    pm_mat(pm, PMAT_HTRIM);
    for (int k = 0; k < 2; ++k) canopy_bracket(pm, k ? 0.6f : -0.6f, 0.55f);
}
/* A gabled canopy 1.5 x 0.8 (40 degrees, its gable to the street): the roof's covering on two slopes, a boarded
 * tympanum with a bargeboard's edge, on two brackets. */
void m_h_canopy_gable(Pm *pm) { quiet(pm);
    const float half = 0.75f, D = 0.8f, a = 40.0f * DEG, rise = (half + 0.05f) * tanf(a), t = 0.06f;
    pm_mat(pm, PMAT_HROOF);
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? 1.0f : -1.0f;
        float p[4][3] = { { sx * (half + 0.05f), 0.12f, D + 0.06f }, { 0.0f, 0.12f + rise, D + 0.06f }, { 0.0f, 0.12f + rise, 0.0f }, { sx * (half + 0.05f), 0.12f, 0.0f } };
        float n[3] = { sx * sinf(a), cosf(a), 0.0f };
        for (int k = 0; k < 4; ++k) { p[k][0] += n[0] * t; p[k][1] += n[1] * t; }
        slab(pm, (const float (*)[3])p, 4, n, t);
    }
    pm_mat(pm, PMAT_HTRIM);
    const float tym[3][2] = { { -half, 0.12f }, { half, 0.12f }, { 0.0f, 0.12f + half * tanf(a) } };
    run_z(pm, tym, 3, D - 0.04f, D);
    bx(pm, -half, 0.0f, 0.0f, half, 0.12f, D, 0.004f);
    if (!L2)
        for (int s = 0; s < 2; ++s) {
            const float sx = s ? 1.0f : -1.0f;
            float p[4][3] = { { sx * (half + 0.06f), 0.06f, D + 0.07f }, { 0.0f, 0.06f + (half + 0.06f) * tanf(a), D + 0.07f },
                              { 0.0f, 0.14f + (half + 0.06f) * tanf(a), D + 0.07f }, { sx * (half + 0.06f), 0.14f, D + 0.07f } };
            const float n[3] = { 0, 0, 1 };
            slab(pm, (const float (*)[3])p, 4, n, 0.03f);
        }
    for (int k = 0; k < 2; ++k) canopy_bracket(pm, k ? 0.62f : -0.62f, 0.7f);
}

/* ------------------------------------------------------------------- bays --- */
/* A bay's corner post: y 0..1 (stretched to the storey's opening), 110 x 100 mm centred on its corner, the
 * front edges chamfered. */
void m_h_bay_post(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float s[6][2] = { { -0.055f, -0.05f }, { 0.055f, -0.05f }, { 0.055f, 0.03f }, { 0.035f, 0.05f }, { -0.035f, 0.05f }, { -0.055f, 0.03f } };
    run_y(pm, s, 6, 0.0f, 1.0f);
}
/* A bay's cornice (x 0..1, laid as whole metres): 0.2 m from y 0, 0.13 m proud, a dentil course under its corona. */
void m_h_bay_cornice(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    const float s[10][2] = { { 0.0f, 0.0f }, { 0.03f, 0.0f }, { 0.03f, 0.04f }, { 0.07f, 0.08f }, { 0.1f, 0.09f }, { 0.12f, 0.12f },
                             { 0.12f, 0.16f }, { 0.13f, 0.17f }, { 0.13f, 0.2f }, { 0.0f, 0.2f } };
    if (L2) { const float q[4][2] = { { 0.0f, 0.0f }, { 0.08f, 0.0f }, { 0.13f, 0.2f }, { 0.0f, 0.2f } }; run_x(pm, q, 4, 0.0f, 1.0f); return; }
    run_x(pm, s, 10, 0.0f, 1.0f);
    if (L0) for (int k = 0; k < 16; ++k) { const float x = ((float)k + 0.5f) / 16.0f; bx(pm, x - 0.016f, 0.045f, 0.03f, x + 0.016f, 0.085f, 0.075f, 0.0f); }
}
/* An apron panel under a bay's light (x -0.5..0.5, y 0..0.7, stretched): a frame with a raised field. */
void m_h_bay_apron(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -0.5f, 0.0f, 0.0f, 0.5f, 0.7f, 0.015f, 0.0f);
    rect_frame(pm, -0.5f, 0.0f, 0.5f, 0.7f, 0.07f, 0.07f, 0.07f, 0.0f, 0.03f, 0.003f);
    field(pm, -0.43f, 0.07f, 0.43f, 0.63f, 0.015f, 0.03f);
}

/* ---------------------------------------------------------------- dormers --- */
/* Dormers HK_DORMER_W wide: the front foot on the roof plane at the origin, the front facing +z, the roof rising
 * towards -z. Built for a roof of HK_DORMER_P: cheeks and roofs run back until they meet that slope, and under a
 * steeper one their backs lie inside the roof. The cheeks and roofs in the roof's own covering, the front boarded
 * round a casement set back 60 mm, its glass behind. */
#define DORM_EAVES 1.25f
static void dormer_front(Pm *pm, float top) {
    const float W = 0.5f * HK_DORMER_W, ww = 0.45f, y0 = 0.2f, y1 = 1.12f, r = 0.08f;
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -W, -0.3f, -0.12f, W, y0, 0.0f, 0.0f);
    bx(pm, -W, y1, -0.12f, W, top, 0.0f, 0.0f);
    bx(pm, -W, y0, -0.12f, -ww, y1, 0.0f, 0.0f);
    bx(pm, ww, y0, -0.12f, W, y1, 0.0f, 0.0f);
    bx(pm, -ww - 0.04f, y0 - 0.05f, -0.12f, ww + 0.04f, y0, 0.05f, 0.003f);   /* the sill */
    pm_push(pm); pm_translate(pm, 0.0f, y0, -r);
    casements(pm, 2.0f * ww, y1 - y0, 0, PMAT_HTRIM);
    pm_pop(pm);
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, -ww, y0, -0.12f, ww, y1, -r, 0.0f);
}
/* a cheek on side s: from the front back to where the eaves line meets the shallowest roof, its foot under it */
static void dormer_cheeks(Pm *pm, float eaves) {
    const float W = 0.5f * HK_DORMER_W, tp = tanf(HK_DORMER_P * DEG), D = eaves / tp + 0.25f;
    pm_mat(pm, PMAT_HROOF);
    const float s[4][2] = { { 0.0f, -0.3f }, { -D, D * tp - 0.3f }, { -D, eaves }, { 0.0f, eaves } };
    for (int k = 0; k < 2; ++k) run_x(pm, s, 4, k ? W - 0.1f : -W, k ? W : -W + 0.1f);
}
void m_h_dormer_gable(Pm *pm) { quiet(pm);
    const float W = 0.5f * HK_DORMER_W, a = 45.0f * DEG, ov = 0.08f, t = 0.07f;
    const float rise = (W + ov) * tanf(a), ridge = DORM_EAVES + rise, D = (ridge + 0.2f) / tanf(HK_DORMER_P * DEG);
    dormer_front(pm, DORM_EAVES);
    dormer_cheeks(pm, DORM_EAVES);
    pm_mat(pm, PMAT_HTRIM);
    const float tym[3][2] = { { -W, DORM_EAVES - 0.01f }, { W, DORM_EAVES - 0.01f }, { 0.0f, DORM_EAVES - 0.01f + W * tanf(a) } };
    run_z(pm, tym, 3, -0.12f, 0.0f);
    pm_mat(pm, PMAT_HROOF);
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? 1.0f : -1.0f;
        float n[3] = { sx * sinf(a), cosf(a), 0.0f };
        float p[4][3] = { { sx * (W + ov), DORM_EAVES - ov * tanf(a), 0.12f }, { 0.0f, ridge - ov * tanf(a), 0.12f },
                          { 0.0f, ridge - ov * tanf(a), -D }, { sx * (W + ov), DORM_EAVES - ov * tanf(a), -D } };
        for (int k = 0; k < 4; ++k) { p[k][0] += n[0] * t; p[k][1] += n[1] * t; }
        slab(pm, (const float (*)[3])p, 4, n, t);
    }
    if (!L2) {   /* the verge's boards */
        pm_mat(pm, PMAT_HTRIM);
        for (int s = 0; s < 2; ++s) {
            const float sx = s ? 1.0f : -1.0f;
            float p[4][3] = { { sx * (W + ov), DORM_EAVES - ov * tanf(a) - 0.1f, 0.13f }, { 0.0f, ridge - ov * tanf(a) - 0.1f, 0.13f },
                              { 0.0f, ridge - ov * tanf(a) + t, 0.13f }, { sx * (W + ov), DORM_EAVES - ov * tanf(a) + t, 0.13f } };
            const float n[3] = { 0, 0, 1 };
            slab(pm, (const float (*)[3])p, 4, n, 0.03f);
        }
    }
}
void m_h_dormer_hip(Pm *pm) { quiet(pm);
    const float W = 0.5f * HK_DORMER_W, a = 45.0f * DEG, ov = 0.08f, t = 0.07f;
    const float rise = (W + ov) * tanf(a), ridge = DORM_EAVES - ov * tanf(a) + rise, D = (ridge + 0.3f) / tanf(HK_DORMER_P * DEG);
    const float zf = 0.12f, ye = DORM_EAVES - ov * tanf(a), zr = zf - (W + ov);   /* the hip's front: the ridge starts W + ov back */
    dormer_front(pm, DORM_EAVES);
    dormer_cheeks(pm, DORM_EAVES);
    pm_mat(pm, PMAT_HROOF);
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? 1.0f : -1.0f;
        float n[3] = { sx * sinf(a), cosf(a), 0.0f };
        float p[4][3] = { { sx * (W + ov), ye, zf }, { 0.0f, ridge, zr }, { 0.0f, ridge, -D }, { sx * (W + ov), ye, -D } };
        for (int k = 0; k < 4; ++k) { p[k][0] += n[0] * t; p[k][1] += n[1] * t; }
        slab(pm, (const float (*)[3])p, 4, n, t);
    }
    {
        float n[3] = { 0.0f, cosf(a), sinf(a) };
        float p[3][3] = { { -(W + ov), ye, zf }, { W + ov, ye, zf }, { 0.0f, ridge, zr } };
        for (int k = 0; k < 3; ++k) { p[k][1] += n[1] * t; p[k][2] += n[2] * t; }
        slab(pm, (const float (*)[3])p, 3, n, t);
    }
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -W - ov, ye - 0.12f, -0.05f, W + ov, ye + 0.01f, zf + 0.01f, 0.003f);
}
void m_h_dormer_flat(Pm *pm) { quiet(pm);
    const float W = 0.5f * HK_DORMER_W, top = DORM_EAVES + 0.1f, D = (top + 0.2f) / tanf(HK_DORMER_P * DEG);
    dormer_front(pm, top);
    dormer_cheeks(pm, top);
    pm_mat(pm, PMAT_HTRIM);
    bx(pm, -W - 0.1f, top - 0.04f, 0.0f, W + 0.1f, top + 0.12f, 0.12f, 0.004f);
    for (int s = 0; s < 2; ++s) { const float x = s ? W + 0.06f : -W - 0.1f; bx(pm, x, top - 0.04f, -D, x + 0.04f, top + 0.12f, 0.0f, 0.0f); }
    pm_mat(pm, PMAT_HLEAD);
    float p[4][3] = { { -W - 0.1f, top + 0.13f, 0.12f }, { W + 0.1f, top + 0.13f, 0.12f }, { W + 0.1f, top + 0.15f, -D }, { -W - 0.1f, top + 0.15f, -D } };
    const float n[3] = { 0, 1, 0 };
    slab(pm, (const float (*)[3])p, 4, n, 0.13f);
}

/* -------------------------------------------------------------- roofscape --- */
/* Chimney pots, standing on y 0 (bedded in the flaunching), terracotta, hollow to their flue. */
static void pot(Pm *pm, const PmProf *o, int n) {
    pm_mat(pm, PMAT_HCLAY);
    if (L2) {   /* far: the outside alone, its top closed (its flue is a dark disc nobody sees from 30 m) */
        PmProf c[16];
        int m = 0;
        for (int i = 0; i < n && m < 15; ++i) c[m++] = o[i];
        c[m].r = 0.0f; c[m].y = o[n - 1].y; c[m].hard = 0; m++;
        pm_lathe(pm, c, m, 6, 0, 0);
        return;
    }
    PmProf s[48];
    int k = pm_prof_shell(o, n, 0.016f, L0 ? 0.008f : 0.0f, L0 ? 3 : 2, s, 48);
    if (k) pm_lathe(pm, s, k, segs(pm, 16, 8), 0, 0);
}
void m_h_pot_plain(Pm *pm) { quiet(pm);
    PmProf o[6] = { { 0, 0, 0 }, { 0.13f, 0, 1 }, { 0.125f, 0.06f, 0 }, { 0.105f, 0.5f, 0 }, { 0.114f, 0.53f, 1 }, { 0.114f, 0.57f, 0 } };
    pot(pm, o, 6);
}
void m_h_pot_cannon(Pm *pm) { quiet(pm);
    PmProf o[9] = { { 0, 0, 0 }, { 0.12f, 0, 1 }, { 0.115f, 0.42f, 0 }, { 0.135f, 0.45f, 0 }, { 0.135f, 0.49f, 0 }, { 0.112f, 0.52f, 0 },
                    { 0.11f, 0.6f, 0 }, { 0.12f, 0.62f, 1 }, { 0.12f, 0.65f, 0 } };
    pot(pm, o, 9);
}
void m_h_pot_crown(Pm *pm) { quiet(pm);
    PmProf o[6] = { { 0, 0, 0 }, { 0.14f, 0, 1 }, { 0.12f, 0.08f, 0 }, { 0.1f, 0.42f, 0 }, { 0.125f, 0.47f, 0 }, { 0.125f, 0.5f, 0 } };
    pot(pm, o, 6);
    if (L2) return;
    pm_mat(pm, PMAT_HCLAY);
    for (int k = 0; k < 6; ++k) {   /* the crown's points */
        const float a = 2.0f * PI_F * (float)k / 6.0f;
        pm_push(pm); pm_translate(pm, 0.113f * cosf(a), 0.5f, 0.113f * sinf(a)); pm_rotate_y(pm, -a);
        bx(pm, -0.014f, 0.0f, -0.035f, 0.014f, 0.08f, 0.035f, 0.0f);
        pm_pop(pm);
    }
}
void m_h_pot_cowl(Pm *pm) { quiet(pm);
    PmProf o[5] = { { 0, 0, 0 }, { 0.13f, 0, 1 }, { 0.12f, 0.06f, 0 }, { 0.105f, 0.4f, 0 }, { 0.105f, 0.42f, 0 } };
    pot(pm, o, 5);
    pm_mat(pm, PMAT_HLEAD);
    PmProf c[6] = { { 0, 0.5f, 0 }, { 0.17f, 0.5f, 1 }, { 0.17f, 0.52f, 1 }, { 0.06f, 0.6f, 0 }, { 0.02f, 0.62f, 0 }, { 0, 0.625f, 0 } };
    pm_lathe(pm, c, 6, segs(pm, 14, 6), 0, 0);
    for (int k = 0; k < 3; ++k) {
        const float a = 2.0f * PI_F * (float)k / 3.0f;
        float p0[3] = { 0.1f * cosf(a), 0.38f, 0.1f * sinf(a) }, p1[3] = { 0.12f * cosf(a), 0.505f, 0.12f * sinf(a) };
        pm_beam(pm, p0, p1, 0.012f, 0.006f, 0.0f, NULL);
    }
}
/* A corbelled cap on a stack HK_STACK_W x HK_STACK_D (x by z; stretched to the stack's), the origin at the stack's
 * top in its middle: two courses oversailing 30 mm each over the last 0.15 m, a course set back to 30 mm, and the
 * flaunching's cement slope over the top for the pots to bed in. */
void m_h_stack_cap(Pm *pm) { quiet(pm);
    const float w = 0.5f * HK_STACK_W, d = 0.5f * HK_STACK_D;
    pm_mat(pm, PMAT_HBRICK);
    pm->part = 1; bx(pm, -w - 0.03f, -0.225f, -d - 0.03f, w + 0.03f, -0.15f, d + 0.03f, 0.002f);
    pm->part = 2; bx(pm, -w - 0.06f, -0.15f, -d - 0.06f, w + 0.06f, -0.075f, d + 0.06f, 0.002f);
    pm->part = 3; bx(pm, -w - 0.03f, -0.075f, -d - 0.03f, w + 0.03f, 0.0f, d + 0.03f, 0.002f);
    pm->part = 0;
    pm_mat(pm, PMAT_HCEMENT);
    frustum(pm, 0.0f, 0.1f, w + 0.01f, d + 0.01f, w - 0.12f, d - 0.12f);
}
/* A soil vent pipe through a roof: 110 mm from 0.4 m under the roof's surface (y 0) to 0.55 over it, a boot
 * round it where it comes through, a domed cage on its top. */
void m_h_vent_pipe(Pm *pm) { quiet(pm);
    pm_mat(pm, PMAT_HRAIN);
    pm_cyl(pm, -0.4f, 0.55f, 0.055f, segs(pm, 12, 6));
    pm_mat(pm, PMAT_HLEAD);
    PmProf b[5] = { { 0, -0.12f, 0 }, { 0.2f, -0.12f, 1 }, { 0.2f, -0.06f, 0 }, { 0.065f, 0.1f, 0 }, { 0, 0.1f, 0 } };
    pm_lathe(pm, b, 5, segs(pm, 12, 6), 0, 0);
    pm_mat(pm, PMAT_HIRON);
    PmProf c[5] = { { 0, 0.54f, 0 }, { 0.065f, 0.54f, 1 }, { 0.065f, 0.6f, 0 }, { 0.04f, 0.65f, 0 }, { 0, 0.67f, 0 } };
    pm_lathe(pm, c, 5, segs(pm, 12, 6), 0, 0);
}
/* A rooflight in a roof plane: the origin at its middle on the roof's surface, y the roof's normal, z down the
 * slope; a 0.78 x 0.98 dark frame standing 90 mm over the covering, its glass, a flashing collar. */
void m_h_rooflight(Pm *pm) { quiet(pm);
    const float hx = 0.39f, hz = 0.49f, f = 0.06f;
    pm_mat(pm, PMAT_HLEAD);
    bx(pm, -hx - 0.08f, -0.03f, -hz - 0.1f, hx + 0.08f, 0.015f, hz + 0.06f, 0.0f);
    pm_mat(pm, PMAT_HIRON);
    bx(pm, -hx, -0.05f, -hz, -hx + f, 0.09f, hz, 0.006f);
    bx(pm, hx - f, -0.05f, -hz, hx, 0.09f, hz, 0.006f);
    bx(pm, -hx + f, -0.05f, -hz, hx - f, 0.09f, -hz + f, 0.006f);
    bx(pm, -hx + f, -0.05f, hz - f, hx - f, 0.075f, hz, 0.006f);
    pm_mat(pm, PMAT_HGLASS);
    bx(pm, -hx + f, 0.05f, -hz + f, hx - f, 0.07f, hz - f, 0.0f);
}

/* ------------------------------------------------------------- assemblies --- */
void hk_run(Pm *pm, int module, float len, float piece) {
    if (len <= 0.01f) return;
    if (piece <= 0.0f) { pm_push(pm); pm_scale(pm, len, 1.0f, 1.0f); pm_place(pm, module); pm_pop(pm); return; }
    int n = (int)floorf(len / piece + 0.5f);
    if (n < 1) n = 1;
    const float seg = len / (float)n;
    for (int k = 0; k < n; ++k) { pm_push(pm); pm_translate(pm, seg * (float)k, 0.0f, 0.0f); pm_scale(pm, seg, 1.0f, 1.0f); pm_place(pm, module); pm_pop(pm); }
}

void hk_window(Pm *pm, const HkWindow *w) {
    static const int FRAME[] = { -1, PMK_H_WIN_SASH, PMK_H_WIN_GEORGIAN, PMK_H_WIN_CASEMENT, PMK_H_WIN_GRID, PMK_H_WIN_PICTURE, PMK_H_WIN_UPVC };
    static const float FW[] = { 1, HK_SASH_W, HK_SASH_W, HK_CASE_W, HK_GRID_W, HK_PICT_W, HK_UPVC_W };
    static const float FH[] = { 1, HK_SASH_H, HK_SASH_H, HK_CASE_H, HK_GRID_H, HK_PICT_H, HK_UPVC_H };
    const float hw = 0.5f * w->w;
    if (w->frame > 0 && w->frame <= HK_FRAME_UPVC) {
        /* an opening much wider than the frame's usual size takes several lights side by side (HM5: a square bay's
         * front, a wide casement), so bars stay bars; a picture window is one wide light by design */
        int n = 1;
        if (w->frame != HK_FRAME_PICTURE && w->w > 1.35f * FW[w->frame]) n = (int)floorf(w->w / FW[w->frame] + 0.5f);
        if (n < 1) n = 1;
        const float lw = w->w / (float)n;
        for (int k = 0; k < n; ++k) {
            pm_push(pm); pm_translate(pm, -hw + lw * ((float)k + 0.5f), 0.0f, -w->reveal); pm_scale(pm, lw / FW[w->frame], w->h / FH[w->frame], 1.0f);
            pm_place(pm, FRAME[w->frame]); pm_pop(pm);
        }
    }
    const int sill = w->sill == HK_SILL_STONE ? PMK_H_SILL_STONE : w->sill == HK_SILL_TIMBER ? PMK_H_SILL_TIMBER : w->sill == HK_SILL_CONCRETE ? PMK_H_SILL_CONCRETE : -1;
    if (sill >= 0) { pm_push(pm); pm_translate(pm, -hw - 0.08f, 0.0f, 0.0f); hk_run(pm, sill, w->w + 0.16f, 0.0f); pm_pop(pm); }
    pm_push(pm);
    pm_translate(pm, 0.0f, w->h, 0.0f);
    switch (w->head) {
    case HK_HEAD_LINTEL: case HK_HEAD_KEYSTONE:
        pm_push(pm); pm_translate(pm, -hw - 0.1f, 0.0f, 0.0f); hk_run(pm, PMK_H_LINTEL_STONE, w->w + 0.2f, 0.0f); pm_pop(pm);
        if (w->head == HK_HEAD_KEYSTONE) { pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.0f); pm_place(pm, PMK_H_KEYSTONE); pm_pop(pm); }
        break;
    case HK_HEAD_ARCH: pm_push(pm); pm_translate(pm, -hw, 0.0f, 0.0f); pm_scale(pm, w->w, 1.0f, 1.0f); pm_place(pm, PMK_H_ARCH_BRICK); pm_pop(pm); break;
    case HK_HEAD_SOLDIER: pm_push(pm); pm_translate(pm, -hw - 0.05f, 0.0f, 0.0f); hk_run(pm, PMK_H_SOLDIER, w->w + 0.1f, 0.0f); pm_pop(pm); break;
    case HK_HEAD_HOOD: pm_push(pm); pm_translate(pm, -hw, 0.0f, 0.0f); pm_scale(pm, w->w, 1.0f, 1.0f); pm_place(pm, PMK_H_HOOD_MOULD); pm_pop(pm); break;
    case HK_HEAD_CONCRETE: pm_push(pm); pm_translate(pm, -hw - 0.15f, 0.0f, 0.0f); hk_run(pm, PMK_H_LINTEL_CONCRETE, w->w + 0.3f, 0.0f); pm_pop(pm); break;
    default: break;
    }
    pm_pop(pm);
    if (w->head == HK_HEAD_CASING) { pm_push(pm); pm_scale(pm, w->w / HK_GRID_W, w->h / HK_GRID_H, 1.0f); pm_place(pm, PMK_H_CASING); pm_pop(pm); }
    if (w->shutters) {
        const int m = w->shutters == HK_SHUT_PANEL ? PMK_H_SHUTTER_PANEL : PMK_H_SHUTTER_LOUVRE;
        for (int s = 0; s < 2; ++s) {
            pm_push(pm); pm_translate(pm, (s ? 1.0f : -1.0f) * (hw + 0.25f * w->w + 0.03f), 0.0f, 0.0f); pm_scale(pm, w->w, w->h / 1.6f, 1.0f);
            pm_place(pm, m); pm_pop(pm);
        }
    }
    if (w->grille) { pm_push(pm); pm_translate(pm, 0.0f, -0.04f, 0.0f); pm_scale(pm, w->w + 0.08f, w->h + 0.08f, 1.0f); pm_place(pm, PMK_H_GRILLE); pm_pop(pm); }
    if (w->box) { pm_push(pm); pm_translate(pm, -hw - 0.05f, -0.1f, 0.0f); pm_scale(pm, w->w + 0.1f, 1.0f, 1.0f); pm_place(pm, PMK_H_WINDOW_BOX); pm_pop(pm); }
}

void hk_door(Pm *pm, int kind, int surround, float w, float h, float recess) {
    static const int LEAF[] = { PMK_H_DOOR_4P, PMK_H_DOOR_6P, PMK_H_DOOR_CRAFT, PMK_H_DOOR_HALF, PMK_H_DOOR_UPVC };
    if (kind >= 0 && kind <= HK_DOOR_UPVC) {
        pm_push(pm); pm_translate(pm, 0.0f, 0.0f, -recess); pm_scale(pm, w / HK_DOOR_W, h / HK_DOOR_H, 1.0f);
        pm_place(pm, LEAF[kind]); pm_pop(pm);
    }
    if (surround == HK_CASE_FLAT || surround == HK_CASE_PEDIMENT) {
        pm_push(pm); pm_scale(pm, (w + 0.1f) / 1.0f, (h + 0.05f) / 2.1f, 1.0f);
        pm_place(pm, surround == HK_CASE_PEDIMENT ? PMK_H_DOORCASE_PED : PMK_H_DOORCASE); pm_pop(pm);
    } else if (surround == HK_CASE_FANLIGHT) {
        pm_push(pm); pm_translate(pm, 0.0f, h + 0.02f, 0.0f); pm_scale(pm, (w + 0.1f) / HK_FAN_W, 1.0f, 1.0f); pm_place(pm, PMK_H_FANLIGHT); pm_pop(pm);
    }
}

void hk_gutter(Pm *pm, int kind, float len, int stops) {
    hk_run(pm, kind == HK_GUTTER_OGEE ? PMK_H_GUTTER_OGEE : PMK_H_GUTTER_HALF, len, 0.0f);
    if (stops & 1) pm_place(pm, PMK_H_GUTTER_STOP);
    if (stops & 2) { pm_push(pm); pm_translate(pm, len - 0.006f, 0.0f, 0.0f); pm_place(pm, PMK_H_GUTTER_STOP); pm_pop(pm); }
}

void hk_downpipe(Pm *pm, float top, float reach, int hopper) {
    float low;   /* where the straight pipe stops under what feeds it */
    pm_push(pm);
    pm_translate(pm, 0.0f, 0.0f, HK_PIPE_Z);
    if (hopper) {
        pm_push(pm); pm_translate(pm, 0.0f, top - 0.12f, 0.0f); pm_place(pm, PMK_H_HOPPER); pm_pop(pm);
        low = top - 0.12f - 0.30f;
    } else {
        const float dz = maxf(reach - HK_PIPE_Z, 0.05f);
        const int lng = dz > 0.45f;
        const float nominal = lng ? 2.0f * HK_SWAN_DZ : HK_SWAN_DZ, drop = lng ? 0.62f : 0.40f;
        pm_push(pm); pm_translate(pm, 0.0f, top, 0.0f); pm_scale(pm, 1.0f, 1.0f, dz / nominal); pm_place(pm, lng ? PMK_H_SWAN_NECK_L : PMK_H_SWAN_NECK); pm_pop(pm);
        low = top - drop;
    }
    if (low > 0.4f) {
        pm_push(pm); pm_translate(pm, 0.0f, 0.3f, 0.0f); pm_scale(pm, 1.0f, low - 0.3f + 0.01f, 1.0f); pm_place(pm, PMK_H_DOWNPIPE); pm_pop(pm);
        for (float y = 1.0f; y < low - 0.15f; y += 1.8f) { pm_push(pm); pm_translate(pm, 0.0f, y, 0.0f); pm_place(pm, PMK_H_PIPE_CLIP); pm_pop(pm); }
        pm_push(pm); pm_translate(pm, 0.0f, low - 0.1f, 0.0f); pm_place(pm, PMK_H_PIPE_CLIP); pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.0f); pm_place(pm, PMK_H_PIPE_SHOE); pm_pop(pm);
    pm_pop(pm);
}

void hk_bargeboards(Pm *pm, int kind, float half, float pitch, int finial) {
    const int m = kind == HK_BARGE_GOTHIC ? PMK_H_BARGE_GOTHIC : PMK_H_BARGE_SCALLOP;
    const float p = pitch * DEG, L = half / cosf(p);
    int n = (int)floorf(L + 0.5f);
    if (n < 1) n = 1;
    const float seg = L / (float)n;
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < n; ++k) {
            /* the left side climbs from its eaves to the apex, the right falls from the apex: both keep their faces
             * out and their patterned edges down without a mirror */
            pm_push(pm);
            if (!s) { pm_translate(pm, -half + seg * (float)k * cosf(p), -half * tanf(p) + seg * (float)k * sinf(p), 0.0f); pm_rotate_z(pm, p); }
            else { pm_translate(pm, seg * (float)k * cosf(p), -seg * (float)k * sinf(p), 0.0f); pm_rotate_z(pm, -p); }
            pm_scale(pm, seg, 1.0f, 1.0f);
            pm_place(pm, m);
            pm_pop(pm);
        }
    if (finial) pm_place(pm, PMK_H_FINIAL);
}

void hk_ridge(Pm *pm, int crested, float len) { hk_run(pm, crested ? PMK_H_RIDGE_CREST : PMK_H_RIDGE_ROLL, len, 1.0f); }

void hk_rafter_tails(Pm *pm, float len, float spacing, float pitch, float out) {
    int n = (int)floorf((len - 0.1f) / spacing) + 1;
    if (n < 1) n = 1;
    const float step = n > 1 ? (len - 0.1f) / (float)(n - 1) : 0.0f;
    for (int k = 0; k < n; ++k) {
        pm_push(pm);
        pm_translate(pm, n > 1 ? 0.05f + step * (float)k : 0.5f * len, 0.0f, 0.0f);
        pm_rotate_x(pm, pitch * DEG);
        pm_scale(pm, 1.0f, 1.0f, out / HK_RAFTER_OUT);
        pm_place(pm, PMK_H_RAFTER_TAIL);
        pm_pop(pm);
    }
}

void hk_quoins(Pm *pm, float h) {
    int n = (int)floorf(h / 0.6f + 0.5f);
    if (n < 1) return;
    const float s = h / (0.6f * (float)n);
    for (int k = 0; k < n; ++k) { pm_push(pm); pm_translate(pm, 0.0f, 0.6f * s * (float)k, 0.0f); pm_scale(pm, 1.0f, s, 1.0f); pm_place(pm, PMK_H_QUOIN); pm_pop(pm); }
}

/* a timber from a to b (in the wall's plane) */
static void timber(Pm *pm, float ax, float ay, float bx_, float by) {
    const float dx = bx_ - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.2f) return;
    pm_push(pm);
    pm_translate(pm, ax, ay, 0.0f);
    pm_rotate_z(pm, atan2f(-dx, dy));
    pm_scale(pm, 1.0f, len, 1.0f);
    pm_place(pm, PMK_H_TIMBER);
    pm_pop(pm);
}
void hk_tudor_gable(Pm *pm, float half, float pitch, float spacing) {
    const float tp = tanf(pitch * DEG), foot = 0.16f, under = 0.14f;
    timber(pm, -half + 0.1f, 0.08f, half - 0.1f, 0.08f);
    int k0 = (int)floorf((half - 0.35f) / spacing);
    for (int k = -k0; k <= k0; ++k) {
        const float x = spacing * (float)k, top = (half - fabsf(x)) * tp - under;
        if (top - foot < 0.35f) continue;
        timber(pm, x, foot, x, top);
    }
    /* braces in the outermost bays with room, rising towards the middle */
    for (int s = 0; s < 2; ++s) {
        const float sx = s ? 1.0f : -1.0f;
        for (int k = k0; k >= 1; --k) {
            const float xo = spacing * (float)k, xi = spacing * (float)(k - 1);
            const float top = (half - xo) * tp - under;
            if (top - foot < 0.8f) continue;
            timber(pm, sx * (xo - 0.06f), foot + 0.05f, sx * (xi + 0.06f), minf(top, (half - xi) * tp - under) - 0.05f);
            break;
        }
    }
}

void hk_hung_tiles(Pm *pm, float w, float h) {
    int nx = (int)floorf(w + 0.5f), ny = (int)floorf(h + 0.5f);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            pm_push(pm); pm_translate(pm, w * (float)i / (float)nx, h * (float)j / (float)ny, 0.0f); pm_scale(pm, w / (float)nx, h / (float)ny, 1.0f);
            pm_place(pm, PMK_H_HUNG_TILES); pm_pop(pm);
        }
}

void hk_bay(Pm *pm, const HkBay *b) {
    const float F = b->front, P = b->proj, hf = 0.5f * F;
    float c[4][2];   /* the corners along the bay, left to right, in (x, z) */
    if (b->kind == HK_BAY_CANTED) { c[0][0] = -hf - P; c[0][1] = 0.0f; c[1][0] = -hf; c[1][1] = P; c[2][0] = hf; c[2][1] = P; c[3][0] = hf + P; c[3][1] = 0.0f; }
    else { c[0][0] = -hf; c[0][1] = 0.0f; c[1][0] = -hf; c[1][1] = P; c[2][0] = hf; c[2][1] = P; c[3][0] = hf; c[3][1] = 0.0f; }
    for (int s = 0; s < b->storeys; ++s) {
        const float y0 = b->storeyH * (float)s;
        for (int f = 0; f < 3; ++f) {
            const float dx = c[f + 1][0] - c[f][0], dz = c[f + 1][1] - c[f][1], len = sqrtf(dx * dx + dz * dz);
            if (len < 0.3f) continue;
            const float ex[3] = { dx / len, 0.0f, dz / len }, ey[3] = { 0.0f, 1.0f, 0.0f }, ez[3] = { -dz / len, 0.0f, dx / len };
            const float o[3] = { 0.5f * (c[f][0] + c[f + 1][0]), y0, 0.5f * (c[f][1] + c[f + 1][1]) };
            pm_push(pm);
            pm_frame(pm, o, ex, ey, ez);
            const float light = len - 0.13f;
            if (light >= 0.4f) {
                HkWindow w = { b->frame, b->sill, HK_HEAD_NONE, 0, 0, 0, light, b->headH - b->sillH, 0.08f };
                pm_push(pm); pm_translate(pm, 0.0f, b->sillH, 0.0f); hk_window(pm, &w); pm_pop(pm);
                if (b->sillH > 0.5f) {
                    pm_push(pm); pm_translate(pm, 0.0f, 0.12f, 0.0f); pm_scale(pm, light, (b->sillH - 0.22f) / 0.7f, 1.0f); pm_place(pm, PMK_H_BAY_APRON); pm_pop(pm);
                }
            }
            pm_push(pm); pm_translate(pm, -0.5f * len - 0.06f, b->headH, 0.0f); hk_run(pm, PMK_H_BAY_CORNICE, len + 0.12f, 1.0f); pm_pop(pm);
            pm_pop(pm);
        }
        for (int k = 0; k < 4; ++k) {
            /* each post bisects its corner (the wall's corners square to the wall) */
            float a;
            if (k == 0 || k == 3) a = 0.0f;
            else {
                const float ux = c[k][0] - c[k - 1][0], uz = c[k][1] - c[k - 1][1], vx = c[k + 1][0] - c[k][0], vz = c[k + 1][1] - c[k][1];
                const float nx = -(uz + vz), nz = ux + vx;   /* outward: the sum of the faces' normals */
                a = atan2f(nx, nz);
            }
            pm_push(pm); pm_translate(pm, c[k][0], y0, c[k][1]); pm_rotate_y(pm, a); pm_scale(pm, 1.0f, b->headH, 1.0f); pm_place(pm, PMK_H_BAY_POST); pm_pop(pm);
        }
    }
}

void hk_dormer(Pm *pm, int kind, float w) {
    const int m = kind == HK_DORMER_HIP ? PMK_H_DORMER_HIP : kind == HK_DORMER_FLAT ? PMK_H_DORMER_FLAT : PMK_H_DORMER_GABLE;
    pm_push(pm); pm_scale(pm, w / HK_DORMER_W, 1.0f, 1.0f); pm_place(pm, m); pm_pop(pm);
}

void hk_chimney_top(Pm *pm, float w, float d, int pots, uint32_t seed) {
    pm_push(pm); pm_scale(pm, w / HK_STACK_W, 1.0f, d / HK_STACK_D); pm_place(pm, PMK_H_STACK_CAP); pm_pop(pm);
    static const int POT[4] = { PMK_H_POT_PLAIN, PMK_H_POT_CANNON, PMK_H_POT_CROWN, PMK_H_POT_COWL };
    uint32_t h = seed * 2654435761u + 0x5107u;
    const int main = (int)(rnd(&h) * 3.0f);
    for (int k = 0; k < pots; ++k) {
        int kind = main;
        if (rnd(&h) < 0.25f) kind = (int)(rnd(&h) * 4.0f) & 3;   /* a replaced pot, a cowl on one flue */
        const float x = -0.5f * w + w * ((float)k + 0.5f) / (float)pots;
        pm_push(pm); pm_translate(pm, x, 0.04f, 0.0f); pm_rotate_y(pm, rnd(&h) * 6.2831853f); pm_place(pm, POT[kind]); pm_pop(pm);
    }
}

void hk_balustrade(Pm *pm, float len) { hk_run(pm, PMK_H_BALUSTRADE, len, 1.0f); }
