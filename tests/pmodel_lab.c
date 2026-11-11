/* The procedural modelling kit's lab (src/gen/pmodel*.c; the street vendor rework, NB1).
 *
 * Builds every library module at its three levels of detail and every part
 * (the hood straight and round a corner, a sheet wall, corrugated sheet, a
 * pipe run, a cable bundle, grates and grilles), and checks: the same input
 * gives the same mesh (twice over), closed modules are watertight and the
 * right way out, every vertex normal agrees with the faces using it, each
 * level keeps to its module's triangle budget, the library as a whole keeps
 * to NB0's 300,000 triangles over its levels, occlusion and wear are baked.
 * It prints a table (triangles, vertices, bytes, build time) per module and
 * level.
 *
 * With an output directory it also renders contact sheets, lit by a key
 * light with ray-traced shadows over an ambient term scaled by the baked
 * occlusion: each module or part a row, the columns LOD0, LOD1, LOD2, then
 * LOD0's occlusion and its wear alone; a faint 10 cm checker shows the
 * metre UVs.
 *
 *   pmodel_lab [out_dir] [--only NAME] */
#include "pmodel.h"
#include "hash.h"
#include "mathlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static double now_ms(void) { return 1000.0 * (double)clock() / (double)CLOCKS_PER_SEC; }

/* --------------------------------------------------------------- parts --- */
enum { PART_HOOD = 0, PART_HOOD_CORNER, PART_SHEET_WALL, PART_CORRUGATED, PART_PIPE, PART_CABLES, PART_GRATE, PART_GRILLE_ROUND,
       PART_GRILLE_BARS, PART_BEVEL_BOXES, PART_COUNT };
static const char *PART_NAME[PART_COUNT] = { "hood", "hood_corner", "sheet_wall", "corrugated", "pipe_run", "cable_bundle", "grate", "grille_round",
                                             "grille_bars", "bevel_boxes" };

static void build_part(int id, Pm *pm) {
    switch (id) {
    case PART_HOOD: { PmHood h; pm_hood_default(&h, 3.85f, 4.2f); pm_hood(pm, &h); break; }
    case PART_HOOD_CORNER: {
        PmHood h;
        pm_hood_default(&h, 3.0f, 3.2f);
        h.spine[0][0] = -2.6f; h.spine[0][1] = 0.0f;
        h.spine[1][0] = 0.0f;  h.spine[1][1] = 0.0f;
        h.spine[2][0] = 0.0f;  h.spine[2][1] = -2.2f;
        h.nspine = 3;
        pm_hood(pm, &h);
        break;
    }
    case PART_SHEET_WALL: pm_mat(pm, PMAT_RUST); pm_sheet_wall(pm, 3.0f, 2.3f, 1.2f, 0.0015f, 1); break;
    case PART_CORRUGATED: pm_mat(pm, PMAT_GALV); pm_corrugated(pm, 1.2f, 1.8f, 0.076f, 0.018f, 0.0008f); break;
    case PART_PIPE: {
        PmPath pa;
        pm_path_init(&pa, 8);
        pm_path_add(&pa, -0.6f, 0.0f, 0.06f, 0); pm_path_add(&pa, 0.2f, 0.0f, 0.06f, 0); pm_path_add(&pa, 0.2f, 0.9f, 0.06f, 0);
        pm_path_add(&pa, 0.2f, 0.9f, 0.4f, 0); pm_path_add(&pa, 0.7f, 0.9f, 0.4f, 0);
        pm_mat(pm, PMAT_COPPER);
        pm_pipe(pm, &pa, 0.011f, 0.05f, 3);
        PmPath pb;
        pm_path_init(&pb, 8);
        pm_path_add(&pb, -0.6f, 0.25f, 0.08f, 0); pm_path_add(&pb, 0.0f, 0.25f, 0.08f, 0); pm_path_add(&pb, 0.0f, 0.7f, 0.25f, 0);
        pm_mat(pm, PMAT_GALV);
        pm_pipe(pm, &pb, 0.03f, 0.12f, 7);
        /* a gauge and a valve on the copper line, saddle clips along it */
        pm_push(pm); pm_translate(pm, -0.3f, 0.011f, 0.06f); pm_place(pm, PMK_GAUGE); pm_pop(pm);
        pm_push(pm); pm_translate(pm, 0.2f, 0.5f, 0.06f); pm_rotate_z(pm, 0.5f * PI_F); pm_place(pm, PMK_VALVE_WHEEL); pm_pop(pm);
        for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, -0.5f + 0.35f * (float)k, -0.012f, 0.06f); pm_rotate_z(pm, PI_F); pm_rotate_y(pm, 0.5f * PI_F); pm_place(pm, PMK_SADDLE); pm_pop(pm); }
        pm_path_free(&pa); pm_path_free(&pb);
        break;
    }
    case PART_CABLES: {
        float a[3] = { -1.2f, 0.6f, 0 }, b[3] = { 1.2f, 0.5f, 0 };
        pm_cable_bundle(pm, a, b, 0.25f, 5, 0.008f, 7u);
        float c[3] = { -1.2f, 0.7f, 0.1f }, d[3] = { 1.2f, 0.75f, 0.1f };
        pm_cable(pm, c, d, 0.35f, 0.012f);
        break;
    }
    case PART_GRATE: pm_place(pm, PMK_GRATE); pm_push(pm); pm_translate(pm, 0, 0.0f, 0); pm_place(pm, PMK_BURNER); pm_pop(pm); break;
    case PART_GRILLE_ROUND: pm_mat(pm, PMAT_STEEL); pm_grille_round(pm, 0.18f, 0.003f, 7, 8); break;
    case PART_GRILLE_BARS: pm_mat(pm, PMAT_PAINTED); pm_grille_bars(pm, 0.6f, 0.4f, 0.006f, 0.03f); break;
    case PART_BEVEL_BOXES: {
        pm_mat(pm, PMAT_STAINLESS);
        float h[3] = { 0.2f, 0.1f, 0.15f };
        for (int k = 0; k < 4; ++k) {
            float c[3] = { -0.75f + 0.5f * (float)k, 0.1f, 0 };
            pm_bbox(pm, c, h, k == 0 ? 0.0f : 0.02f, k == 0 ? 1 : k == 1 ? 1 : k == 2 ? 3 : 6);
        }
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------- render --- */
typedef struct Image { int w, h; unsigned char *p; } Image;
static Image image_new(int w, int h) {
    Image im = { w, h, (unsigned char *)malloc((size_t)w * h * 3) };
    for (int i = 0; i < w * h; ++i) { im.p[i * 3] = 22; im.p[i * 3 + 1] = 22; im.p[i * 3 + 2] = 28; }
    return im;
}
static float srgb_to_lin(float s) { return s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float l) { l = clampf(l, 0.0f, 1.0f); return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f; }
static void hex_lin(uint32_t c, float o[3]) { for (int k = 0; k < 3; ++k) o[k] = srgb_to_lin((float)((c >> (16 - 8 * k)) & 255u) / 255.0f); }

static const uint32_t MAT_RGB[PMAT_COUNT] = {
    0x7A4A2E, 0x56605E, 0x9A9C9E, 0xB06A3C, 0x2E2C2A, 0x3F5A52, 0xD8D6CC, 0x1C1C1C, 0xE0DCCF, 0x6F8A86, 0xB89A4A, 0x7A5A3A,
    0x262A28, 0xE8E4DA, 0xC2A46A, 0xE2D8B0, 0x222222, 0x8A9092, 0x4A4C4E, 0xF0EEE6, 0xA02A20, 0xB07A3A, 0xE8D49A, 0xB8761C,
    0xF2EFE6, 0x9A1A0A, 0xE8A0A0 };

/* A 5 x 7 font: digits, letters, underscore, point. */
static const uint8_t *glyph(char c) {
    static const uint8_t G[40][7] = {
        { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 },
        { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E },
        { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
        { 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
        { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E },
        { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
        { 0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F },
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
        { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E }, { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 }, { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C }, { 0, 0, 0, 0, 0, 0, 0x1F }, { 0, 0, 0, 0, 0, 0x0C, 0x0C }, { 0, 0, 0, 0, 0, 0, 0 } };
    if (c >= 'a' && c <= 'z') return G[c - 'a'];
    if (c >= 'A' && c <= 'Z') return G[c - 'A'];
    if (c >= '0' && c <= '9') return G[26 + c - '0'];
    if (c == '_') return G[36];
    if (c == '.') return G[37];
    return G[38];
}
static void text(Image *im, int x, int y, const char *s, int scale) {
    for (; *s; ++s, x += 6 * scale) {
        const uint8_t *g = glyph(*s);
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c) {
                if (!(g[r] & (0x10 >> c))) continue;
                for (int dy = 0; dy < scale; ++dy)
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x + c * scale + dx, py = y + r * scale + dy;
                        if (px < 0 || py < 0 || px >= im->w || py >= im->h) continue;
                        unsigned char *p = &im->p[(py * im->w + px) * 3];
                        p[0] = 235; p[1] = 230; p[2] = 200;
                    }
            }
    }
}

enum { SHOW_LIT = 0, SHOW_OCC, SHOW_WEAR };

/* Renders mesh m into cell (cx, cy, cw, ch) of the image. */
static void render(Image *im, const PmMesh *m, int cx, int cy, int cw, int ch, int show) {
    if (!m->ni) return;
    const int SS = 2, W = cw * SS, H = ch * SS;
    float az = 0.65f, el = 0.42f;
    float ez[3] = { sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el) };   /* to the viewer */
    float ex[3] = { cosf(az), 0.0f, -sinf(az) };
    float ey[3] = { ez[1] * ex[2] - ez[2] * ex[1], ez[2] * ex[0] - ez[0] * ex[2], ez[0] * ex[1] - ez[1] * ex[0] };
    PmStats st;
    pm_stats(m, &st);
    float mnx = 1e30f, mxx = -1e30f, mny = 1e30f, mxy = -1e30f;
    for (int c = 0; c < 8; ++c) {
        float p[3] = { (c & 1) ? st.hi[0] : st.lo[0], (c & 2) ? st.hi[1] : st.lo[1], (c & 4) ? st.hi[2] : st.lo[2] };
        float x = p[0] * ex[0] + p[1] * ex[1] + p[2] * ex[2], y = p[0] * ey[0] + p[1] * ey[1] + p[2] * ey[2];
        mnx = minf(mnx, x); mxx = maxf(mxx, x); mny = minf(mny, y); mxy = maxf(mxy, y);
    }
    float scale = 0.84f * minf((float)W / maxf(mxx - mnx, 1e-4f), (float)H / maxf(mxy - mny, 1e-4f));
    float ox = 0.5f * (float)W - 0.5f * (mnx + mxx) * scale, oy = 0.5f * (float)H + 0.5f * (mny + mxy) * scale + 0.02f * (float)H;
    float *zb = (float *)malloc(sizeof(float) * (size_t)W * H);
    int32_t *tid = (int32_t *)malloc(sizeof(int32_t) * (size_t)W * H);
    float (*bw)[3] = (float (*)[3])malloc(sizeof(float) * 3 * (size_t)W * H);
    for (int i = 0; i < W * H; ++i) { zb[i] = -1e30f; tid[i] = -1; }
    for (uint32_t t = 0; t < m->ni / 3; ++t) {
        float sx[3], sy[3], sz[3];
        for (int k = 0; k < 3; ++k) {
            const float *p = m->v[m->idx[t * 3 + k]].p;
            sx[k] = ox + (p[0] * ex[0] + p[1] * ex[1] + p[2] * ex[2]) * scale;
            sy[k] = oy - (p[0] * ey[0] + p[1] * ey[1] + p[2] * ey[2]) * scale;
            sz[k] = p[0] * ez[0] + p[1] * ez[1] + p[2] * ez[2];
        }
        float d = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (fabsf(d) < 1e-9f) continue;
        int x0 = (int)floorf(minf(sx[0], minf(sx[1], sx[2]))), x1 = (int)ceilf(maxf(sx[0], maxf(sx[1], sx[2])));
        int y0 = (int)floorf(minf(sy[0], minf(sy[1], sy[2]))), y1 = (int)ceilf(maxf(sy[0], maxf(sy[1], sy[2])));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > W - 1) x1 = W - 1;
        if (y1 > H - 1) y1 = H - 1;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                float w1 = ((px - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (py - sy[0])) / d;
                float w2 = ((sx[1] - sx[0]) * (py - sy[0]) - (px - sx[0]) * (sy[1] - sy[0])) / d;
                float w0 = 1.0f - w1 - w2;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                int i = y * W + x;
                if (z <= zb[i]) continue;
                zb[i] = z; tid[i] = (int32_t)t; bw[i][0] = w0; bw[i][1] = w1; bw[i][2] = w2;
            }
    }
    PmBvh *bvh = show == SHOW_LIT ? pm_bvh_build(m) : NULL;
    float L[3] = { 0.55f, 0.75f, 0.38f };
    float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    for (int k = 0; k < 3; ++k) L[k] /= ll;
    float diag = sqrtf((st.hi[0] - st.lo[0]) * (st.hi[0] - st.lo[0]) + (st.hi[1] - st.lo[1]) * (st.hi[1] - st.lo[1]) + (st.hi[2] - st.lo[2]) * (st.hi[2] - st.lo[2]));
    float ground = st.lo[1];
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            float acc[3] = { 0, 0, 0 };
            for (int sy2 = 0; sy2 < SS; ++sy2)
                for (int sx2 = 0; sx2 < SS; ++sx2) {
                    int i = (y * SS + sy2) * W + x * SS + sx2;
                    float col[3];
                    if (tid[i] < 0) {
                        /* the ground under it, catching its shadow */
                        float a = ((float)(x * SS + sx2) + 0.5f - ox) / scale, b = (oy - ((float)(y * SS + sy2) + 0.5f)) / scale;
                        float q[3] = { a * ex[0] + b * ey[0], a * ex[1] + b * ey[1], a * ex[2] + b * ey[2] };
                        float t = (q[1] - ground) / ez[1];
                        float P[3] = { q[0] - t * ez[0], ground, q[2] - t * ez[2] };
                        float cxz = 0.5f * (st.lo[0] + st.hi[0]), czz = 0.5f * (st.lo[2] + st.hi[2]);
                        float r = sqrtf((P[0] - cxz) * (P[0] - cxz) + (P[2] - czz) * (P[2] - czz));
                        float g = 0.055f * clampf(1.4f - r / maxf(diag, 1e-3f), 0.0f, 1.0f);
                        if (show == SHOW_LIT && g > 0.0f) {
                            float o2[3] = { P[0], P[1] + 1e-4f, P[2] };
                            if (pm_bvh_ray(bvh, o2, L, 0.0f, 1e3f) > 0.0f) g *= 0.45f;
                        }
                        col[0] = 0.0085f + g; col[1] = 0.0085f + g; col[2] = 0.011f + g;
                    } else {
                        const uint32_t *ix = &m->idx[tid[i] * 3];
                        const PmVertex *v0 = &m->v[ix[0]], *v1 = &m->v[ix[1]], *v2 = &m->v[ix[2]];
                        float n[3], P[3], uv[2], occ, wear;
                        for (int k = 0; k < 3; ++k) {
                            n[k] = bw[i][0] * v0->n[k] + bw[i][1] * v1->n[k] + bw[i][2] * v2->n[k];
                            P[k] = bw[i][0] * v0->p[k] + bw[i][1] * v1->p[k] + bw[i][2] * v2->p[k];
                        }
                        float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (int k = 0; k < 3; ++k) n[k] /= maxf(nl, 1e-9f);
                        for (int k = 0; k < 2; ++k) uv[k] = bw[i][0] * v0->uv[k] + bw[i][1] * v1->uv[k] + bw[i][2] * v2->uv[k];
                        occ = (bw[i][0] * v0->occ + bw[i][1] * v1->occ + bw[i][2] * v2->occ) / 255.0f;
                        wear = (bw[i][0] * v0->wear + bw[i][1] * v1->wear + bw[i][2] * v2->wear) / 255.0f;
                        if (show == SHOW_OCC) { float o = 0.02f + 0.75f * occ * occ; col[0] = col[1] = col[2] = o; }
                        else if (show == SHOW_WEAR) {
                            float w = (wear - 0.5f) * 2.0f;
                            col[0] = w > 0 ? 0.08f + 0.9f * w : 0.08f; col[1] = 0.08f + 0.02f * fabsf(w); col[2] = w < 0 ? 0.08f - 0.9f * w : 0.08f;
                            if (fabsf(w) < 0.04f) col[0] = col[1] = col[2] = 0.08f;
                        } else {
                            float base[3];
                            hex_lin(MAT_RGB[v0->mat < PMAT_COUNT ? v0->mat : 0], base);
                            /* per-part variation and the 10 cm checker */
                            float pv = 0.9f + 0.2f * hash_float(hash_u32(v0->part * 7919u + 13u));
                            int chk = ((int)floorf(uv[0] / 0.1f) + (int)floorf(uv[1] / 0.1f)) & 1;
                            float k = pv * (chk ? 1.06f : 0.94f);
                            float w = (wear - 0.5f) * 2.0f;
                            k *= w > 0 ? 1.0f + 0.35f * w : 1.0f + 0.35f * w;
                            float ndl = maxf(0.0f, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
                            if (ndl > 0.0f) {
                                float o2[3] = { P[0] + n[0] * 2e-4f, P[1] + n[1] * 2e-4f, P[2] + n[2] * 2e-4f };
                                if (pm_bvh_ray(bvh, o2, L, 0.0f, 1e3f) > 0.0f) ndl = 0.0f;
                            }
                            float sky = 0.28f * occ * (0.6f + 0.4f * n[1]);
                            /* a little specular for the metals, to read their shape */
                            float h[3] = { L[0] + ez[0], L[1] + ez[1], L[2] + ez[2] }, hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
                            float sp = ndl > 0.0f ? powf(maxf(0.0f, (n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) / hl), 40.0f) * 0.25f : 0.0f;
                            for (int c = 0; c < 3; ++c) col[c] = base[c] * k * (1.1f * ndl + sky) + sp;
                        }
                    }
                    for (int c = 0; c < 3; ++c) acc[c] += col[c];
                }
            unsigned char *p = &im->p[((cy + y) * im->w + cx + x) * 3];
            for (int c = 0; c < 3; ++c) p[c] = (unsigned char)(lin_to_srgb(acc[c] / (float)(SS * SS)) * 255.0f + 0.5f);
        }
    pm_bvh_free(bvh);
    free(zb); free(tid); free(bw);
}

/* ----------------------------------------------------------------- main --- */
#define CW 300
#define CH 240
#define ROWS_PER_SHEET 8

static void sheet_row(Image *im, int row, const char *name, PmMesh lods[PM_LODS]) {
    int y = row * CH;
    for (int l = 0; l < PM_LODS; ++l) render(im, &lods[l], l * CW, y, CW, CH, SHOW_LIT);
    render(im, &lods[0], 3 * CW, y, CW, CH, SHOW_OCC);
    render(im, &lods[0], 4 * CW, y, CW, CH, SHOW_WEAR);
    text(im, 6, y + 6, name, 2);
    PmStats st;
    pm_stats(&lods[0], &st);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2fm", maxf(st.hi[0] - st.lo[0], maxf(st.hi[1] - st.lo[1], st.hi[2] - st.lo[2])));
    text(im, 6, y + 24, buf, 1);
    for (int l = 0; l < PM_LODS; ++l) {
        snprintf(buf, sizeof(buf), "l%d %d", l, (int)(lods[l].ni / 3));
        text(im, l * CW + 6, y + CH - 14, buf, 1);
    }
    text(im, 3 * CW + 6, y + CH - 14, "occlusion", 1);
    text(im, 4 * CW + 6, y + CH - 14, "wear", 1);
}

int main(int argc, char **argv) {
    const char *out = NULL, *only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else out = argv[i];
    }
    printf("pmodel_lab: %d modules, %d parts\n", pm_module_count(), PART_COUNT);
    printf("%-15s %4s %7s %7s %8s %8s %6s  %s\n", "module", "lod", "tris", "budget", "verts", "KB", "ms", "checks");
    long total = 0;
    static int mod_tris[PMK_COUNT][PM_LODS];
    Image im = { 0, 0, NULL };
    int row = 0, sheet = 0;
    const int nmod = pm_module_count();
    for (int id = 0; id < nmod; ++id) {
        const PmModuleDef *d = pm_module(id);
        if (only && strcmp(only, d->name)) continue;
        PmMesh lods[PM_LODS];
        memset(lods, 0, sizeof(lods));
        for (int l = 0; l < PM_LODS; ++l) {
            double t0 = now_ms();
            pm_module_mesh(id, l, &lods[l]);
            double ms = now_ms() - t0;
            PmMesh again = { 0 };
            pm_module_mesh(id, l, &again);
            PmStats s, s2;
            pm_stats(&lods[l], &s);
            pm_stats(&again, &s2);
            pm_mesh_free(&again);
            total += s.tris;
            mod_tris[id][l] = s.tris;
            char checks[160] = "";
            CHECK(s.hash == s2.hash, "%s L%d: not deterministic", d->name, l);
            CHECK(s.tris <= d->budget[l], "%s L%d: %d triangles over its budget %d", d->name, l, s.tris, d->budget[l]);
            CHECK(s.bad_normals == 0, "%s L%d: %d triangles whose vertex normals disagree with them", d->name, l, s.bad_normals);
            if (d->closed && s.tris) {
                CHECK(s.open_edges == 0, "%s L%d: %d open edges (should be closed)", d->name, l, s.open_edges);
                CHECK(s.inverted == 0, "%s L%d: %d shells inside out", d->name, l, s.inverted);
            }
            if (s.tris) {
                int occ_lo = 255, wear_hi = 0;
                for (uint32_t i = 0; i < lods[l].nv; ++i) { if (lods[l].v[i].occ < occ_lo) occ_lo = lods[l].v[i].occ; if (lods[l].v[i].wear > wear_hi) wear_hi = lods[l].v[i].wear; }
                snprintf(checks, sizeof(checks), "open %d bad %d inv %d occ>=%d wear<=%d", s.open_edges, s.bad_normals, s.inverted, occ_lo, wear_hi);
            }
            printf("%-15s %4d %7d %7d %8d %8.1f %6.1f  %s\n", d->name, l, s.tris, d->budget[l], s.verts,
                   (double)(lods[l].nv * sizeof(PmVertex) + lods[l].ni * 4) / 1024.0, ms, checks);
        }
        if (out) {
            if (!im.p) im = image_new(5 * CW, ROWS_PER_SHEET * CH);
            sheet_row(&im, row, d->name, lods);
            if (++row == ROWS_PER_SHEET) {
                char path[512];
                snprintf(path, sizeof(path), "%s/pmodel_modules_%d.png", out, sheet++);
                stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
                free(im.p); im.p = NULL; row = 0;
            }
        }
        for (int l = 0; l < PM_LODS; ++l) pm_mesh_free(&lods[l]);
    }
    if (out && im.p) {
        char path[512];
        snprintf(path, sizeof(path), "%s/pmodel_modules_%d.png", out, sheet++);
        im.h = row * CH;
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p); im.p = NULL;
    }
    printf("library: %ld triangles over its levels (NB0's budget 300000)\n", total);
    if (!only) CHECK(total <= 300000, "the library holds %ld triangles over its levels, over 300000", total);
    if (!only) {
        /* a plausible kit for one noodle bar after the target sheet (NB4-NB5 settle the real one), at each level */
        static const int MIX[][2] = {
            { PMK_STOOL, 5 }, { PMK_STOCKPOT_S, 2 }, { PMK_STOCKPOT_L, 1 }, { PMK_LID, 2 }, { PMK_BOWL_STACK, 2 }, { PMK_JAR_OIL, 3 },
            { PMK_JAR_GINGER, 2 }, { PMK_BOTTLE, 4 }, { PMK_BURNER, 2 }, { PMK_GRATE, 2 }, { PMK_KNOB, 4 }, { PMK_STEAMER, 1 },
            { PMK_GN_PAN, 3 }, { PMK_GN_PAN_OPEN, 1 }, { PMK_RICE_COOKER, 1 }, { PMK_LADLE, 1 }, { PMK_SPIDER, 1 }, { PMK_SPATULA, 1 },
            { PMK_CLEAVER, 1 }, { PMK_S_HOOK, 4 }, { PMK_UTENSIL_RAIL, 1 }, { PMK_GAS_13, 1 }, { PMK_GAS_47, 1 }, { PMK_FAN, 1 },
            { PMK_POWER_PANEL, 1 }, { PMK_SHELF_BRACKET, 4 }, { PMK_A_FRAME, 1 }, { PMK_BELL, 1 }, { PMK_GAUGE, 1 },
            { PMK_VALVE_LEVER, 1 }, { PMK_SADDLE, 6 }, { PMK_INSULATOR, 0 } };
        long kit[PM_LODS] = { 0, 0, 0 };
        int inst = 0;
        for (size_t k = 0; k < sizeof(MIX) / sizeof(MIX[0]); ++k) {
            for (int l = 0; l < PM_LODS; ++l) kit[l] += (long)mod_tris[MIX[k][0]][l] * MIX[k][1];
            inst += MIX[k][1];
        }
        printf("a slice's kit (%d instances, without the hood and its rivets): L0 %ld, L1 %ld, L2 %ld triangles\n", inst, kit[0], kit[1], kit[2]);
    }

    /* the parts, with their modules as instances (counted) and built in (drawn) */
    printf("\n%-15s %4s %7s %7s %9s %8s %6s  %s\n", "part", "lod", "struct", "inst", "drawn", "KB", "ms", "checks");
    row = 0; sheet = 0;
    for (int id = 0; id < PART_COUNT; ++id) {
        if (only && strcmp(only, PART_NAME[id])) continue;
        PmMesh drawn[PM_LODS];
        memset(drawn, 0, sizeof(drawn));
        for (int l = 0; l < PM_LODS; ++l) {
            PmMesh m = { 0 };
            PmInstances li = { 0 };
            Pm pm;
            double t0 = now_ms();
            pm_begin(&pm, &m, &li, l);
            build_part(id, &pm);
            double ms = now_ms() - t0;
            PmStats s;
            pm_stats(&m, &s);
            /* determinism */
            PmMesh m2 = { 0 };
            PmInstances li2 = { 0 };
            Pm pm2;
            pm_begin(&pm2, &m2, &li2, l);
            build_part(id, &pm2);
            PmStats s2;
            pm_stats(&m2, &s2);
            CHECK(s.hash == s2.hash && li.n == li2.n, "%s L%d: not deterministic", PART_NAME[id], l);
            CHECK(s.bad_normals == 0, "%s L%d: %d triangles whose vertex normals disagree with them", PART_NAME[id], l, s.bad_normals);
            pm_mesh_free(&m2); pm_inst_free(&li2);
            /* the drawn mesh: the structure baked, the instances built in */
            pm_bake(&m, 24, pm_reach(&m));
            pm_instances_bake(&m, &li, l);
            PmStats sd;
            pm_stats(&m, &sd);
            printf("%-15s %4d %7d %7u %9d %8.1f %6.1f  open %d bad %d\n", PART_NAME[id], l, s.tris, li.n, sd.tris,
                   (double)(m.nv * sizeof(PmVertex) + m.ni * 4) / 1024.0, ms, s.open_edges, s.bad_normals);
            drawn[l] = m;
            pm_inst_free(&li);
        }
        if (out) {
            if (!im.p) im = image_new(5 * CW, ROWS_PER_SHEET * CH);
            sheet_row(&im, row, PART_NAME[id], drawn);
            if (++row == ROWS_PER_SHEET) {
                char path[512];
                snprintf(path, sizeof(path), "%s/pmodel_parts_%d.png", out, sheet++);
                stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
                free(im.p); im.p = NULL; row = 0;
            }
        }
        for (int l = 0; l < PM_LODS; ++l) pm_mesh_free(&drawn[l]);
    }
    if (out && im.p) {
        char path[512];
        snprintf(path, sizeof(path), "%s/pmodel_parts_%d.png", out, sheet++);
        im.h = row * CH;
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p);
    }
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
