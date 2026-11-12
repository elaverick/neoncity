/* The procedural modelling kit's lab (src/gen/pmodel*.c; the street vendor rework, NB1).
 *
 * Builds every library module at its three levels of detail and every part
 * (the hood straight and round a corner, a sheet wall, corrugated sheet, a
 * pipe run, a cable bundle, grates and grilles), and checks: the same input
 * gives the same mesh (twice over), closed modules are watertight and the
 * right way out, every vertex normal agrees with the faces using it, each
 * level keeps to its module's triangle budget, the library as a whole keeps
 * to NB0's 300,000 triangles over its levels, occlusion and wear are baked;
 * every material is drawn with an authored set (PM_MAT_SET, its file in
 * assets/materials/) or painted by the shader, and no one set covers over
 * 40% of a noodle bar's area (the plausible kit and the body's main
 * surfaces, NB3).
 * It prints a table (triangles, vertices, bytes, build time) per module and
 * level, then the noodle bar's body (NB4, src/gen/noodle_bar.c) in both its
 * forms at the slice's site and wider ones: the merged structure's triangles,
 * its instances, its far shell's, the time to build and bake it, the same
 * plan twice over, the budgets (the structure, the far shell's 500).
 *
 * With an output directory it also renders contact sheets, lit by a key
 * light with ray-traced shadows over an ambient term scaled by the baked
 * occlusion: each module or part a row, the columns LOD0, LOD1, LOD2, then
 * LOD0's occlusion and its wear alone; a faint 10 cm checker shows the
 * metre UVs.
 *
 *   pmodel_lab [out_dir] [--only NAME]   (NAME body: the noodle bar's bodies and lines; line: the lines) */
#include "pmodel.h"
#include "noodle_bar.h"
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

static void build_part(int id, Pm *pm);

/* Triangle areas of m by material, added to area[] times n. */
static void add_area(const PmMesh *m, double n, double area[PMAT_COUNT]) {
    for (uint32_t i = 0; i + 2 < m->ni; i += 3) {
        const PmVertex *a = &m->v[m->idx[i]], *b = &m->v[m->idx[i + 1]], *c = &m->v[m->idx[i + 2]];
        float e[3], f[3], x[3];
        for (int k = 0; k < 3; ++k) { e[k] = b->p[k] - a->p[k]; f[k] = c->p[k] - a->p[k]; }
        x[0] = e[1] * f[2] - e[2] * f[1]; x[1] = e[2] * f[0] - e[0] * f[2]; x[2] = e[0] * f[1] - e[1] * f[0];
        if (a->mat < PMAT_COUNT) area[a->mat] += n * 0.5 * sqrt((double)(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]));
    }
}

/* The noodle bar's materials (NB3): each one's set exists, and no one set covers over 40% of the area of the
 * slice's bar: its body and its cooking line (NB5), modules and all. */
static void material_share(void) {
    double area[PMAT_COUNT] = { 0 };
    {
        /* the slice's bar (NB4, NB5): its body, its modules and its cooking line's, built in */
        NbPlan p;
        nb_plan(&p, 3.85f, 3.48f, 0.0f, 0x5EED01u, NB_STRAIGHT);
        NbLine L;
        nb_line(&p, &L);
        PmMesh m = { 0 }, none = { 0 };
        PmInstances li = { 0 };
        Pm pm;
        pm_begin(&pm, &m, &li, PM_LOD0);
        nb_body(&pm, &p);
        pm_begin(&pm, &none, &li, PM_LOD0);
        nb_line_place(&pm, &p, &L);
        pm_instances_bake(&m, &li, PM_LOD0);
        add_area(&m, 1.0, area);
        pm_mesh_free(&m); pm_mesh_free(&none); pm_inst_free(&li);
    }
    double total = 0.0, set[PMAT_COUNT] = { 0 }, painted = 0.0;
    for (int i = 0; i < PMAT_COUNT; ++i) total += area[i];
    /* sets serving two materials count once: fold each onto the first material naming it */
    for (int i = 0; i < PMAT_COUNT; ++i) {
        if (!PM_MAT_SET[i]) { painted += area[i]; continue; }
        int first = i;
        for (int j = 0; j < i; ++j) if (PM_MAT_SET[j] && !strcmp(PM_MAT_SET[j], PM_MAT_SET[i])) { first = j; break; }
        set[first] += area[i];
    }
    const char *dirs[] = { "assets/materials", "../assets/materials", "build/assets/materials" };
    int dir = -1;
    for (int d = 0; d < 3 && dir < 0; ++d) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.json", dirs[d], PM_MAT_SET[PMAT_RUST]);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); dir = d; }
    }
    printf("\nmaterials (the slice's bar: its body and its cooking line, %.1f m2)\n", total);
    for (int i = 0; i < PMAT_COUNT; ++i) {
        if (!PM_MAT_SET[i]) continue;
        if (dir >= 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s.json", dirs[dir], PM_MAT_SET[i]);
            FILE *f = fopen(path, "rb");
            CHECK(f != NULL, "material %s names the set %s, but %s is missing", PM_MAT_NAME[i], PM_MAT_SET[i], path);
            if (f) fclose(f);
        }
        if (set[i] <= 0.0) continue;
        printf("  %-20s %6.2f m2 %5.1f%%\n", PM_MAT_SET[i], set[i], 100.0 * set[i] / total);
        CHECK(set[i] <= 0.4 * total, "the set %s covers %.0f%% of a noodle bar (at most 40%%)", PM_MAT_SET[i], 100.0 * set[i] / total);
    }
    printf("  %-20s %6.2f m2 %5.1f%%  (glass, glazes, printed faces, food)\n", "painted by the shader", painted, 100.0 * painted / total);
    if (dir < 0) printf("  (assets/materials not found from here: the sets' files not checked)\n");
}

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
    0xF2EFE6, 0x9A1A0A, 0xE8A0A0, 0x5E4A40, 0x6A6E72, 0xA8AAAC,
    0xF4F8FF, 0xFF3060, 0x4080FF, 0xFFD090, 0xC04080 };   /* NB6: tube, neon, flame, bulb, the far icon face */

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

/* ------------------------------------------------------- the bar's body --- */
#define NB_STRUCT_BUDGET 17500   /* the merged structure at its one (near) level; NB0 guessed 6,000; NB6 +1,200
                                  * for the bench's and counter's fine top sheets (contact darkening) */
#define NB_FAR_BUDGET    500

static void body_section(const char *out) {
    static const struct { const char *name; float w, d; int form, side; uint32_t seed; } B[] = {
        { "straight (slice)", 3.85f, 3.48f, NB_STRAIGHT, 0, 0x5EED01u },
        { "straight wide", 5.2f, 3.3f, NB_STRAIGHT, 0, 0x5EED04u },
        { "corner right", 4.4f, 3.6f, NB_CORNER, 1, 0x5EED02u },
        { "corner left", 4.0f, 3.4f, NB_CORNER, -1, 0x5EED03u } };
    const int nb = (int)(sizeof(B) / sizeof(B[0]));
    const int BW = 620, BH = 460;
    Image im = { 0, 0, NULL };
    if (out) im = image_new(3 * BW, nb * BH);
    printf("\n%-18s %7s %6s %8s %5s %8s %6s %6s %6s  %s\n", "body", "struct", "inst", "drawn", "far", "KB", "build", "bake", "far ms", "checks");
    for (int b = 0; b < nb; ++b) {
        NbPlan p;
        uint32_t seed = B[b].seed;
        int fits = 0;
        for (int t = 0; t < 64; ++t, seed += 7919u) {   /* a seed whose plan turns the corner the way asked */
            fits = nb_plan(&p, B[b].w, B[b].d, 0.0f, seed, B[b].form);
            if (!B[b].side || p.side == B[b].side) break;
        }
        CHECK(fits, "body %s: the plan doesn't fit its %.2f x %.2f m site", B[b].name, B[b].w, B[b].d);
        CHECK(p.form == B[b].form, "body %s: the plan's form is %d, asked %d", B[b].name, p.form, B[b].form);
        PmMesh m = { 0 }, m2 = { 0 }, f = { 0 }, line = { 0 };
        PmInstances li = { 0 }, li2 = { 0 }, ll = { 0 };
        Pm pm;
        double t0 = now_ms();
        pm_begin(&pm, &m, &li, PM_LOD0);
        nb_body(&pm, &p);
        double tb = now_ms() - t0;
        t0 = now_ms();
        PmMesh oc = { 0 };
        nb_line_occluders(&p, &oc);   /* the line darkens the bench and counter round it (NB6) */
        pm_bake_with(&m, &oc, NB_BAKE_RAYS, NB_BAKE_REACH);
        double tk = now_ms() - t0;
        PmStats s, s2;
        pm_stats(&m, &s);
        NbPlan q;
        nb_plan(&q, B[b].w, B[b].d, 0.0f, seed, B[b].form);
        pm_begin(&pm, &m2, &li2, PM_LOD0);
        nb_body(&pm, &q);
        nb_line_occluders(&q, &oc);
        pm_bake_with(&m2, &oc, NB_BAKE_RAYS, NB_BAKE_REACH);
        pm_mesh_free(&oc);
        pm_stats(&m2, &s2);
        if (b == 0) {
            int bym[PMAT_COUNT] = { 0 };
            for (uint32_t i = 0; i + 2 < m.ni; i += 3) bym[m.v[m.idx[i]].mat]++;
            for (uint32_t i = 0; i < m.nv; ++i)
                if (m.v[i].p[2] < -0.005f) { printf("  behind the wall: %s part %d at (%.3f, %.3f, %.3f)\n", PM_MAT_NAME[m.v[i].mat], m.v[i].part, m.v[i].p[0], m.v[i].p[1], m.v[i].p[2]); break; }
            printf("  the slice's structure by material:");
            for (int k = 0; k < PMAT_COUNT; ++k) if (bym[k]) printf(" %s %d", PM_MAT_NAME[k], bym[k]);
            printf("\n");
        }
        CHECK(s.hash == s2.hash && li.n == li2.n, "body %s: not deterministic", B[b].name);
        CHECK(s.bad_normals == 0, "body %s: %d vertices whose normals disagree with their faces", B[b].name, s.bad_normals);
        CHECK(s.tris <= NB_STRUCT_BUDGET, "body %s: %d structure triangles, over %d", B[b].name, s.tris, NB_STRUCT_BUDGET);
        t0 = now_ms();
        pm_begin(&pm, &f, NULL, PM_LOD2);
        nb_shell(&pm, &p);
        pm_bake(&f, NB_BAKE_RAYS, pm_reach(&f));
        double tf = now_ms() - t0;
        PmStats sf;
        pm_stats(&f, &sf);
        CHECK(sf.tris <= NB_FAR_BUDGET, "body %s: far shell %d triangles, over %d", B[b].name, sf.tris, NB_FAR_BUDGET);
        CHECK(sf.bad_normals == 0, "body %s: far shell: %d vertices whose normals disagree with their faces", B[b].name, sf.bad_normals);
        /* inside the site, under the duct's cap */
        CHECK(s.lo[0] > -0.5f * B[b].w - 0.01f && s.hi[0] < 0.5f * B[b].w + 0.01f && s.lo[2] > -0.01f && s.hi[2] < B[b].d + 0.25f,
              "body %s: reaches x %.2f..%.2f z %.2f..%.2f on a %.2f x %.2f m site", B[b].name, s.lo[0], s.hi[0], s.lo[2], s.hi[2], B[b].w, B[b].d);
        /* the drawn bar: the structure, its modules and the cooking line's built in */
        NbLine L;
        nb_line(&p, &L);
        pm_begin(&pm, &line, &ll, PM_LOD0);
        nb_line_place(&pm, &p, &L);
        uint32_t ninst = li.n;
        pm_instances_bake(&m, &li, PM_LOD0);
        pm_instances_bake(&m, &ll, PM_LOD0);
        PmStats sd;
        pm_stats(&m, &sd);
        printf("%-18s %7d %6u %8d %5d %8.0f %6.1f %6.1f %6.1f  %s %s, %.2f x %.2f, %d stools, %d tubes, %d icons, open %d\n",
               B[b].name, s.tris, ninst, sd.tris, sf.tris, (double)(s.tris * 3 * 40) / 1024.0, tb, tk, tf,
               p.form == NB_CORNER ? "corner" : "straight", p.side > 0 ? "+x" : "-x", B[b].w, B[b].d, p.nstool, p.ntube, p.nicon, s.open_edges);
        if (out) {
            render(&im, &m, 0, b * BH, BW, BH, SHOW_LIT);
            render(&im, &f, BW, b * BH, BW, BH, SHOW_LIT);
            render(&im, &m, 2 * BW, b * BH, BW, BH, SHOW_OCC);
            text(&im, 8, b * BH + 8, B[b].name, 2);
            text(&im, BW + 8, b * BH + 8, "far shell", 2);
            text(&im, 2 * BW + 8, b * BH + 8, "occlusion", 2);
        }
        pm_mesh_free(&m); pm_mesh_free(&m2); pm_mesh_free(&f); pm_mesh_free(&line);
        pm_inst_free(&li); pm_inst_free(&li2); pm_inst_free(&ll);
    }
    printf("(struct: merged triangles, near; KB: in the pool at 40 B a vertex, unindexed)\n");
    if (out) {
        char path[512];
        snprintf(path, sizeof(path), "%s/pmodel_body.png", out);
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p);
    }
}

/* --------------------------------------------------- the cooking line --- */
/* NB5: the line's rules over a sweep of sites and seeds in both forms (nb_line_check), its determinism, the
 * second burner's mix; cutaway renders of four kitchens (the hood, the counter and the street taken off). */
static void line_section(const char *out) {
    static const float WS[] = { 3.2f, 3.5f, 3.85f, 4.2f, 4.6f, 5.2f }, DS[] = { 3.0f, 3.3f, 3.48f, 3.8f };
    int lines = 0, failing = 0, shown = 0, kinds[NB_POT_KINDS] = { 0 }, items = 0, fans = 0, bms = 0, steams = 0;
    double t0 = now_ms();
    for (size_t wi = 0; wi < sizeof(WS) / sizeof(WS[0]); ++wi)
        for (size_t di = 0; di < sizeof(DS) / sizeof(DS[0]); ++di)
            for (int form = 0; form < NB_FORMS; ++form)
                for (uint32_t k = 0; k < 40; ++k) {
                    NbPlan p;
                    const uint32_t seed = hash_combine(0x11E5u, (uint32_t)(wi * 1000 + di * 100 + (size_t)form * 50) + k);
                    if (!nb_plan(&p, WS[wi], DS[di], 0.0f, seed, form)) continue;
                    NbLine L, L2;
                    nb_line(&p, &L);
                    nb_line(&p, &L2);
                    CHECK(!memcmp(&L, &L2, sizeof(L)), "line %.2f x %.2f seed %08x: not deterministic", WS[wi], DS[di], seed);
                    char err[2048];
                    const int bad = nb_line_check(&p, &L, err, sizeof(err));
                    lines++; items += L.n; kinds[L.kind]++; fans += L.fan[1] > 0.0f; bms += L.bmPans > 0;
                    /* NB7: its steam's paths stay under the extractor and the hood, out past the lip, lit */
                    NbSteam st[NB_STEAMS_MAX];
                    const float ceil = (k & 3u) == 3u ? 3.4f : 0.0f;
                    const int nst = nb_steam(&p, &L, ceil, st, NB_STEAMS_MAX);
                    steams += nst;
                    CHECK(nst >= 2, "line %.2f x %.2f seed %08x: %d plumes", WS[wi], DS[di], seed, nst);
                    for (int i = 0; i < nst; ++i) {
                        const NbSteam *e = &st[i];
                        for (int c = 0; c < 9; ++c)
                            CHECK(e->light[c / 3][c % 3] >= 0.0f && e->light[c / 3][c % 3] < 200.0f, "line seed %08x: steam %d's light %g", seed, i, e->light[c / 3][c % 3]);
                        CHECK(e->rise > 0.0f || ceil > 0.0f, "line seed %08x: steam %d rises %g", seed, i, e->rise);
                        for (int q = 1; q < e->nknot; ++q) {
                            const float y = e->pos[1] + e->knot[q][1], z = e->pos[2] + e->knot[q][0];
                            CHECK(z > p.lip || y < nb_ceil(&p, e->pos[0], z) - 0.02f,
                                  "line %.2f x %.2f seed %08x: steam %d's knot %d (%.2f m up at z %.2f) through the hood", WS[wi], DS[di], seed, i, q, y, z);
                            CHECK(q != 1 || y < 1.97f, "line seed %08x: steam %d over the extractor's baffles", seed, i);
                            CHECK(e->knot[q][0] >= e->knot[q - 1][0] - 1e-4f, "line seed %08x: steam %d runs back in", seed, i);
                            if (e->knot[q][2] > 0.0f) CHECK(e->knot[q][1] < e->knot[q][2], "line seed %08x: steam %d's knot %d over its ceiling", seed, i, q);
                        }
                        const float g0 = nb_grime(&p, &L, st, nst, e->pos, NULL), g1 = nb_grime(&p, &L, st, nst, e->pos, NULL);
                        CHECK(g0 == g1 && g0 >= -1.0f && g0 <= 1.0f, "line seed %08x: grime %g %g", seed, g0, g1);
                    }
                    if (bad) {
                        failing++;
                        if (shown < 6) { printf("  line %.2f x %.2f %s seed %08x (%s): %d broken\n%s", WS[wi], DS[di], p.form ? "corner" : "straight", seed, L.kind == NB_POT_WOK ? "wok" : L.kind == NB_POT_STEAMER ? "steamer" : L.kind == NB_POT_SAUCEPAN ? "saucepan" : "noodles", bad, err); shown++; }
                    }
                }
    printf("\nthe cooking line: %d lines over the sweep (%.1f ms), %d break a rule; %.1f items a line; second burner: noodles %d, wok %d, steamer %d, saucepan %d; a bain-marie in %d, a fan in %d; %d plumes\n",
           lines, now_ms() - t0, failing, lines ? (double)items / lines : 0.0, kinds[0], kinds[1], kinds[2], kinds[3], bms, fans, steams);
    CHECK(failing == 0, "%d of %d cooking lines break a rule", failing, lines);
    if (!out) return;
    /* cutaways of the body_section's four bars */
    static const struct { float w, d; int form, side; uint32_t seed; } B[] = {
        { 3.85f, 3.48f, NB_STRAIGHT, 0, 0x5EED01u }, { 5.2f, 3.3f, NB_STRAIGHT, 0, 0x5EED04u },
        { 4.4f, 3.6f, NB_CORNER, 1, 0x5EED02u }, { 4.0f, 3.4f, NB_CORNER, -1, 0x5EED03u } };
    const int CWL = 900, CHL = 640;
    Image im = image_new(2 * CWL, 2 * CHL);
    for (int b = 0; b < 4; ++b) {
        NbPlan p;
        uint32_t seed = B[b].seed;
        for (int t = 0; t < 64; ++t, seed += 7919u) {
            nb_plan(&p, B[b].w, B[b].d, 0.0f, seed, B[b].form);
            if (!B[b].side || p.side == B[b].side) break;
        }
        NbLine L;
        nb_line(&p, &L);
        PmMesh m = { 0 }, none = { 0 }, cut = { 0 };
        PmInstances li = { 0 };
        Pm pm;
        pm_begin(&pm, &m, &li, PM_LOD0);
        nb_body(&pm, &p);
        pm_begin(&pm, &none, &li, PM_LOD0);
        nb_line_place(&pm, &p, &L);
        pm_instances_bake(&m, &li, PM_LOD0);
        /* keep the kitchen: under 2.5 m, behind the counter's back, inside the side walls (the near one, +x,
         * taken off to see the bottles' end); its vertices compacted so the view fits it */
        cut.v = (PmVertex *)malloc(sizeof(PmVertex) * m.nv);
        cut.shell = (uint16_t *)malloc(sizeof(uint16_t) * m.nv);
        cut.idx = (uint32_t *)malloc(sizeof(uint32_t) * m.ni);
        uint32_t *remap = (uint32_t *)malloc(sizeof(uint32_t) * m.nv);
        for (uint32_t i = 0; i < m.nv; ++i) remap[i] = UINT32_MAX;
        for (uint32_t i = 0; i + 2 < m.ni; i += 3) {
            float c[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) c[a] += m.v[m.idx[i + k]].p[a] / 3.0f;
            if (c[1] > 2.5f || c[2] > p.kz1 - 0.02f || c[0] < p.x0 + 0.003f || c[0] > p.kx1 + 0.02f) continue;
            for (int k = 0; k < 3; ++k) {
                uint32_t v = m.idx[i + k];
                if (remap[v] == UINT32_MAX) { remap[v] = cut.nv; cut.shell[cut.nv] = m.shell[v]; cut.v[cut.nv++] = m.v[v]; }
                cut.idx[cut.ni++] = remap[v];
            }
        }
        free(remap);
        render(&im, &cut, (b & 1) * CWL, (b >> 1) * CHL, CWL, CHL, SHOW_LIT);
        static const char *const KIND[NB_POT_KINDS] = { "noodles", "wok", "steamer", "saucepan" };
        char label[96];
        snprintf(label, sizeof(label), "%s %.2fx%.2f %s %d pans", p.form ? "corner" : "straight", B[b].w, B[b].d, KIND[L.kind], L.bmPans);
        text(&im, (b & 1) * CWL + 8, (b >> 1) * CHL + 8, label, 2);
        free(cut.idx); free(cut.v); free(cut.shell);
        pm_mesh_free(&m); pm_mesh_free(&none); pm_inst_free(&li);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/pmodel_line.png", out);
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
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
            if (l == 0 && s.tris)
                printf("%-15s      bounds x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f\n", "", s.lo[0], s.hi[0], s.lo[1], s.hi[1], s.lo[2], s.hi[2]);
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
        /* the slice's cooking line (NB5) at each level: its modules as set down */
        NbPlan p;
        nb_plan(&p, 3.85f, 3.48f, 0.0f, 0x5EED01u, NB_STRAIGHT);
        NbLine L;
        nb_line(&p, &L);
        PmMesh none = { 0 };
        PmInstances li = { 0 };
        Pm pm;
        pm_begin(&pm, &none, &li, PM_LOD0);
        nb_line_place(&pm, &p, &L);
        long kit[PM_LODS] = { 0, 0, 0 };
        for (uint32_t k = 0; k < li.n; ++k)
            for (int l = 0; l < PM_LODS; ++l) kit[l] += (long)mod_tris[li.v[k].module][l];
        printf("the slice's cooking line (%u instances, %d items): L0 %ld, L1 %ld, L2 %ld triangles\n", li.n, L.n, kit[0], kit[1], kit[2]);
        pm_mesh_free(&none); pm_inst_free(&li);
        material_share();
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
    if (!only || !strcmp(only, "body")) body_section(out);
    if (!only || !strcmp(only, "body") || !strcmp(only, "line")) line_section(out);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
