#include "vault.h"
#include "hash.h"
#include "mathlib.h"
#include <math.h>
#include <string.h>

/* ===========================================================================
 * A covered way's vault (vault.h; street-vendor roadmap SF2).
 * ========================================================================= */

#define HALF_PI 1.5707963f

void vault_plan(VaultPlan *v, float len, float span, uint32_t seed) {
    memset(v, 0, sizeof(*v));
    float u[8];
    for (int k = 0; k < 8; ++k) u[k] = hash_float(hash_combine(seed, 0x7A417u + (uint32_t)k));
    v->seed = seed;
    v->len = len; v->span = span;
    v->spring = 4.2f + 0.4f * u[0];
    v->girder = 0.42f + 0.08f * u[1];
    v->inset = 0.15f;
    v->rise = span * (0.22f + 0.08f * u[2]);
    /* the ribs' circle through both springings and the crown */
    const float h = 0.5f * span - v->inset;
    v->radius = (h * h + v->rise * v->rise) / (2.0f * v->rise);
    v->yc = v->spring + v->rise - v->radius;
    const float want = 1.9f + 0.5f * u[3];
    v->nbay = len > want ? (int)floorf(len / want + 0.5f) : 1;
    if (v->nbay > VAULT_BAYS_MAX) v->nbay = VAULT_BAYS_MAX;
    v->pitch = len / (float)v->nbay;
    v->glazed = (uint8_t)(u[4] < 0.45f);
}

void vault_set_spring(VaultPlan *v, float spring) {
    v->spring = spring;
    const float h = 0.5f * v->span - v->inset;
    v->radius = (h * h + v->rise * v->rise) / (2.0f * v->rise);
    v->yc = v->spring + v->rise - v->radius;
}

int vault_add_post(VaultPlan *v, int side, float x) {
    if (side < 0 || side > 1 || v->npost[side] >= VAULT_POSTS_MAX) return 0;
    int i = v->npost[side]++;
    while (i > 0 && v->post[side][i - 1] > x) { v->post[side][i] = v->post[side][i - 1]; --i; }
    v->post[side][i] = x;
    return 1;
}

float vault_under(const VaultPlan *v, float z) {
    const float d = z - 0.5f * v->span, h = 0.5f * v->span - v->inset;
    if (fabsf(d) >= h) return v->spring;
    return v->yc + sqrtf(maxf(v->radius * v->radius - d * d, 0.0f)) - 0.09f;
}

float vault_top(const VaultPlan *v) { return v->yc + v->radius + 0.1f; }

void vault_bay_range(const VaultPlan *v, int bay, float *x0, float *x1) {
    *x0 = v->pitch * (float)bay;
    *x1 = bay + 1 >= v->nbay ? v->len : v->pitch * (float)(bay + 1);
}

int vault_lamp(const VaultPlan *v, int bay, float out[3]) {
    if (!(bay & 1) && v->nbay > 1) return 0;
    float x0, x1;
    vault_bay_range(v, bay, &x0, &x1);
    out[0] = 0.5f * (x0 + x1); out[1] = v->spring - 0.35f - 0.24f; out[2] = 0.5f * v->span;
    return 1;
}

/* ----------------------------------------------------------------- parts --- */

static void bb(Pm *pm, float x0, float y0, float z0, float x1, float y1, float z1, float bevel) {
    const float c[3] = { 0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (z0 + z1) };
    const float h[3] = { 0.5f * fabsf(x1 - x0), 0.5f * fabsf(y1 - y0), 0.5f * fabsf(z1 - z0) };
    if (h[0] < 1e-4f || h[1] < 1e-4f || h[2] < 1e-4f) return;
    pm_bbox(pm, c, h, bevel, 1);
}

/* A rivet's head on a face at (x, y, z) facing axis a (0 x, 1 y, 2 z) of sign sg: a low dome. */
static void rivet(Pm *pm, float x, float y, float z, int a, float sg) {
    static const PmProf head[3] = { { 0.0f, 0.0f, 0 }, { 0.009f, 0.0f, 1 }, { 0.0f, 0.007f, 0 } };
    pm_push(pm);
    pm_translate(pm, x, y, z);
    if (a == 0) pm_rotate_z(pm, sg > 0.0f ? -HALF_PI : HALF_PI);
    else if (a == 2) pm_rotate_x(pm, sg > 0.0f ? HALF_PI : -HALF_PI);
    else if (sg < 0.0f) pm_rotate_x(pm, 2.0f * HALF_PI);
    pm_lathe(pm, head, 3, 6, 0.0f, 0.0f);
    pm_pop(pm);
}
/* A row of rivets from a to b (pitch about p) on a face facing axis a, sign sg. */
static void rivet_row(Pm *pm, const float a0[3], const float b0[3], float p, int a, float sg) {
    const float d[3] = { b0[0] - a0[0], b0[1] - a0[1], b0[2] - a0[2] };
    const float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const int n = 1 + (int)(L / p);
    for (int i = 0; i < n; ++i) {
        const float t = n > 1 ? (float)i / (float)(n - 1) : 0.5f;
        rivet(pm, a0[0] + d[0] * t, a0[1] + d[1] * t, a0[2] + d[2] * t, a, sg);
    }
}

/* the ends of the ribs' arc: from the side-0 springing (angle al) over the crown to the side-1 one (ar) */
static void arc_ends(const VaultPlan *v, float *al, float *ar) {
    const float h = 0.5f * v->span - v->inset;
    *ar = atan2f(v->spring - v->yc, h);
    *al = 3.14159265f - *ar;
}

/* A rib across at x: a box section 0.18 deep swept round the arc, bolted to both girders' tops. */
static void rib(Pm *pm, const VaultPlan *v, float x) {
    float al, ar;
    arc_ends(v, &al, &ar);
    const int n = pm->lod == PM_LOD0 ? 24 : pm->lod == PM_LOD1 ? 16 : 10;
    PmPath pa;
    pm_path_init(&pa, n + 2);
    const float c[3] = { x, v->yc, 0.5f * v->span }, ex[3] = { 0.0f, 0.0f, 1.0f }, ey[3] = { 0.0f, 1.0f, 0.0f };
    pm_path_arc(&pa, c, ex, ey, v->radius, al, ar, n);
    PmSect s;
    pm_sect_rect(&s, 0.18f, 0.10f, 0.012f);
    const float up[3] = { 1.0f, 0.0f, 0.0f };
    pm_mat(pm, PMAT_PAINTED);
    pm_sweep(pm, &pa, &s, up, 3);
    pm_path_free(&pa);
    /* its feet: plates on the girders */
    for (int e = 0; e < 2; ++e) {
        const float z = e ? v->span - v->inset : v->inset;
        bb(pm, x - 0.1f, v->spring, z - 0.12f, x + 0.1f, v->spring + 0.02f, z + 0.12f, 0.003f);
    }
}

/* The bay's sheet over the ribs from x0 to x1 (a thin slab round the arc), the crown glazed in some. */
static void sheet(Pm *pm, const VaultPlan *v, float x0, float x1) {
    float al, ar;
    arc_ends(v, &al, &ar);
    const int n = pm->lod == PM_LOD0 ? 24 : pm->lod == PM_LOD1 ? 16 : 10;
    const float r0 = v->radius + 0.092f, r1 = r0 + 0.004f;
    const float glass = v->glazed ? 0.3f * (al - HALF_PI) : -1.0f;   /* the glazed strip's half angle round the crown */
    for (int i = 0; i < n; ++i) {
        const float a0 = al + (ar - al) * (float)i / (float)n, a1 = al + (ar - al) * (float)(i + 1) / (float)n;
        const float am = 0.5f * (a0 + a1);
        const int glazed = fabsf(am - HALF_PI) < glass;
        pm->shell++;
        pm_mat(pm, glazed ? PMAT_GLAZING : PMAT_RUST);
        for (int side = 0; side < (glazed ? 1 : 2); ++side) {
            const float r = side ? r1 : r0, sg = side ? 1.0f : -1.0f;
            uint32_t id[4];
            const float aa[2] = { a0, a1 }, xx[2] = { x0, x1 };
            for (int k = 0; k < 4; ++k) {
                const float a = aa[k >> 1], x = xx[(k ^ (k >> 1)) & 1];
                const float p[3] = { x, v->yc + r * sinf(a), 0.5f * v->span + r * cosf(a) };
                const float nn[3] = { 0.0f, sg * sinf(a), sg * cosf(a) };
                id[k] = pm_vert(pm, p, nn, x, v->radius * (al - a));
            }
            pm_quad(pm, id[0], id[1], id[2], id[3]);
        }
        if (glazed) {
            /* a glazing bar along the bay at the strip's edges */
            pm_mat(pm, PMAT_STEEL);
            if (fabsf(a0 - HALF_PI) >= glass - 1e-4f || fabsf(a1 - HALF_PI) >= glass - 1e-4f) {
                const float a = fabsf(a0 - HALF_PI) > fabsf(a1 - HALF_PI) ? a0 : a1;
                const float y = v->yc + r0 * sinf(a), z = 0.5f * v->span + r0 * cosf(a);
                bb(pm, x0, y - 0.02f, z - 0.025f, x1, y + 0.012f, z + 0.025f, 0.002f);
            }
        }
    }
}

/* A girder's stretch from x0 to x1 along side's line: an I of riveted plate, its web stiffened at each rib. */
static void girder(Pm *pm, const VaultPlan *v, float x0, float x1, int side) {
    const float zc = side ? v->span - v->inset : v->inset, y1 = v->spring, y0 = v->spring - v->girder;
    pm_mat(pm, PMAT_PAINTED);
    bb(pm, x0, y1 - 0.022f, zc - 0.10f, x1, y1, zc + 0.10f, 0.003f);
    bb(pm, x0, y0, zc - 0.10f, x1, y0 + 0.022f, zc + 0.10f, 0.003f);
    bb(pm, x0, y0 + 0.022f, zc - 0.007f, x1, y1 - 0.022f, zc + 0.007f, 0.0f);
    /* the stiffeners either side of the web at the bay's rib, and a splice plate over the web there, riveted */
    bb(pm, x0, y0 + 0.022f, zc - 0.09f, x0 + 0.012f, y1 - 0.022f, zc + 0.09f, 0.0f);
    const float out = side ? -1.0f : 1.0f, zf = zc + out * 0.007f;   /* the web's face towards the lane */
    bb(pm, x0 - 0.12f, y0 + 0.05f, out > 0.0f ? zf : zf - 0.008f, x0 + 0.12f, y1 - 0.05f, out > 0.0f ? zf + 0.008f : zf, 0.001f);
    for (int c = 0; c < 2; ++c) {
        const float x = x0 + (c ? 0.07f : -0.07f), z = zf + out * 0.008f;
        const float a[3] = { x, y0 + 0.08f, z }, b[3] = { x, y1 - 0.08f, z };
        rivet_row(pm, a, b, 0.09f, 2, out);
    }
}

/* Where the girder meets a building's wall (bay x, side): a riveted wall plate and a knee bracket under it. */
static void wall_bracket(Pm *pm, const VaultPlan *v, float x, int side) {
    const float wz = side ? v->span : 0.0f, out = side ? -1.0f : 1.0f, zc = side ? v->span - v->inset : v->inset;
    const float y1 = v->spring - v->girder;
    pm_mat(pm, PMAT_PAINTED);
    const float za = wz, zb = wz + out * 0.014f;
    bb(pm, x - 0.11f, y1 - 0.85f, fminf(za, zb), x + 0.11f, v->spring, fmaxf(za, zb), 0.002f);
    for (int c = 0; c < 2; ++c) {
        const float a[3] = { x + (c ? 0.075f : -0.075f), y1 - 0.80f, zb }, b[3] = { x + (c ? 0.075f : -0.075f), v->spring - 0.06f, zb };
        rivet_row(pm, a, b, 0.11f, 2, out);
    }
    /* the knee: a tee from the plate's foot up to the girder's bottom flange */
    const float a[3] = { x, y1 - 0.72f, zb }, b[3] = { x, y1 - 0.01f, zc - out * 0.02f };
    const float up[3] = { 1.0f, 0.0f, 0.0f };
    pm_beam(pm, a, b, 0.016f, 0.10f, 0.002f, up);
}

/* A post under side's girder at x, against the wall: a riveted I-beam (its flanges facing along the lane, its web
 * square to the wall), its base and cap plates, knee braces to the girder. */
static void post(Pm *pm, const VaultPlan *v, float x, int side) {
    const float zc = side ? v->span - v->inset : v->inset, top = v->spring - v->girder;
    const float hf = 0.10f, hd = 0.10f, tf = 0.013f, tw = 0.009f;   /* half the flange's width, half the depth */
    pm_mat(pm, PMAT_PAINTED);
    for (int f = -1; f <= 1; f += 2) {
        const float xf = x + (float)f * hd;
        bb(pm, fminf(xf, xf - (float)f * tf), -0.6f, zc - hf, fmaxf(xf, xf - (float)f * tf), top - 0.02f, zc + hf, 0.002f);
        /* two rows of rivets down each flange's outer face (where its lattice plates would be fixed) */
        for (int r = -1; r <= 1; r += 2) {
            const float a[3] = { xf, 0.25f, zc + (float)r * 0.06f }, b[3] = { xf, top - 0.12f, zc + (float)r * 0.06f };
            rivet_row(pm, a, b, 0.16f, 0, (float)f);
        }
    }
    bb(pm, x - hd + tf, -0.6f, zc - tw, x + hd - tf, top - 0.02f, zc + tw, 0.0f);
    pm_mat(pm, PMAT_STEEL);
    bb(pm, x - 0.18f, 0.0f, zc - 0.18f, x + 0.18f, 0.025f, zc + 0.18f, 0.003f);
    bb(pm, x - 0.14f, top - 0.02f, zc - 0.12f, x + 0.14f, top, zc + 0.12f, 0.003f);
    pm_mat(pm, PMAT_PAINTED);
    for (int e = -1; e <= 1; e += 2) {
        const float xb = x + (float)e * 0.7f;
        if (xb < 0.05f || xb > v->len - 0.05f) continue;
        const float a[3] = { x + (float)e * 0.1f, top - 0.75f, zc }, b[3] = { xb, top - 0.01f, zc };
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        pm_beam(pm, a, b, 0.08f, 0.014f, 0.002f, up);
    }
}

/* A pendant lamp at the bay's middle: its cable from the crown, an enamel shade, the lamp's glass. */
static void lamp(Pm *pm, const VaultPlan *v, const float at[3]) {
    const float top = v->yc + v->radius - 0.09f, mouth = at[1] + 0.24f;
    pm_mat(pm, PMAT_STEEL);
    pm_push(pm); pm_translate(pm, at[0], 0.0f, at[2]); pm_cyl(pm, mouth + 0.22f, top, 0.006f, 6); pm_pop(pm);
    pm_mat(pm, PMAT_ENAMEL);
    PmProf shade[6] = { { 0.0f, 0.24f, 0 }, { 0.035f, 0.24f, 1 }, { 0.05f, 0.17f, 0 }, { 0.17f, 0.05f, 0 }, { 0.21f, 0.0f, 1 }, { 0.205f, 0.0f, 1 } };
    pm_push(pm); pm_translate(pm, at[0], at[1], at[2]); pm_lathe(pm, shade, 6, pm_segs(pm, 20, 10), 0.0f, 0.0f); pm_pop(pm);
    pm_mat(pm, PMAT_BULB);
    PmProf bulb[3] = { { 0.0f, 0.03f, 0 }, { 0.06f, 0.08f, 0 }, { 0.0f, 0.14f, 0 } };
    pm_push(pm); pm_translate(pm, at[0], at[1], at[2]); pm_lathe(pm, bulb, 3, pm_segs(pm, 12, 8), 0.0f, 0.0f); pm_pop(pm);
}

void vault_bay(Pm *pm, const VaultPlan *v, int bay) {
    float x0, x1;
    vault_bay_range(v, bay, &x0, &x1);
    const uint8_t part0 = pm->part;
    pm->part = (uint8_t)(part0 + (bay & 7));
    rib(pm, v, x0 + (bay == 0 ? 0.05f : 0.0f));
    if (bay + 1 >= v->nbay) rib(pm, v, x1 - 0.05f);
    sheet(pm, v, x0, x1);
    for (int s = 0; s < 2; ++s) {
        girder(pm, v, x0, x1, s);
        for (int i = 0; i < v->npost[s]; ++i) {
            const float x = v->post[s][i];
            if (x >= x0 && (x < x1 || (bay + 1 >= v->nbay && x <= x1))) post(pm, v, x, s);
        }
        /* bracketed to the building's wall at its rib where one stands, clear of a post */
        if (bay < VAULT_BAYS_MAX && v->wall[s][bay]) {
            const float bx = x0 + (bay == 0 ? 0.3f : 0.0f);
            int near = 0;
            for (int i = 0; i < v->npost[s]; ++i) near |= fabsf(v->post[s][i] - bx) < 0.5f;
            if (!near) wall_bracket(pm, v, bx, s);
        }
    }
    if (!(bay & 1)) {   /* a tie rod across at every other rib, under the girders' tops */
        pm_mat(pm, PMAT_STEEL);
        pm_push(pm);
        pm_translate(pm, x0 + (bay == 0 ? 0.05f : 0.0f), v->spring - 0.06f, v->inset);
        pm_rotate_x(pm, HALF_PI);
        pm_cyl(pm, 0.0f, v->span - 2.0f * v->inset, 0.014f, 8);
        pm_pop(pm);
    }
    float at[3];
    if (vault_lamp(v, bay, at)) lamp(pm, v, at);
    pm->part = part0;
}
