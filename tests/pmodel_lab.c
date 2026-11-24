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
#include "pmodel_house.h"
#include "pmodel_retrofit.h"
#include "pmodel_region.h"
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
        nb_plan(&p, 3.85f, 3.48f, 0.0f, 0x5EED01u, NB_STRAIGHT, VT_NOODLE, 0.5f);
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
    0xF4F8FF, 0xFF3060, 0x4080FF, 0xFFD090, 0xC04080,     /* NB6: tube, neon, flame, bulb, the far icon face */
    0x6A2E14, 0xE6DCC4, 0xC89050, 0xD49A30, 0x8A5A10, 0x4A2410, 0x3A7A20, 0xA02010, 0x1E1C1A, 0xFF7020, 0xFF6418,   /* SF1's food, grit, embers */
    0x7FA0A8, 0xFFC888,     /* SF2: glazing, the far window */
    0x8A9092,               /* SF4: the shutter */
    /* HM4, the houses' kit: trim, door, stone, brick, clay, hung tile, rainwater, lead, iron, plant, glass, timber,
     * plastic, the roof (slate here), cement */
    0xD6D0C2, 0x1F4A33, 0xB5A88C, 0x8C4A32, 0xA4542F, 0x9A4A30, 0x1E1F20, 0x6B7275, 0x1A1A1C, 0x3E6B2C, 0x2B3A40,
    0x2E241D, 0xEEEEEA, 0x464C55, 0x9A978F,
    /* HM5c1, the retrofit kit: galvanised steel, copper, tank plastic, solar cells, breeze blocks, flowers, LED, lamp glass */
    0x8E9496, 0xA8582C, 0x1C1C1E, 0x1A2440, 0xB8B2A2, 0xC0287A, 0xFFE2B0, 0xFFC27A,
    0x3E6B2C,               /* HMv: a vine's card */
    /* HM5b1a: aluminium, red paper, a lit lantern, hazard yellow, canvas (green here), frosted glass */
    0xA8ACB0, 0xB0201A, 0xFF4A28, 0xE0B020, 0x2E7A4A, 0xD8DCD8,
    0x8A6A50 };             /* HM5b1b: tin (rusting) */
/* The house vignettes' stand-ins for the shells' surfaces (a part number from LAB_STANDIN: drawn flat in these) */
#define LAB_STANDIN 250
static const uint32_t STANDIN_RGB[6] = { 0xC9BFA6, 0x7D8A80, 0x40454C, 0x8E3F2A, 0x7A4030, 0x5A5550 };   /* render, siding, slate, clay roof, brick, dark */
static float g_az = 0.65f, g_el = 0.42f;   /* the view (the vignettes turn it to the street) */

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
    float az = g_az, el = g_el;
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
                            if (v0->part >= LAB_STANDIN && v0->part < LAB_STANDIN + 6) hex_lin(STANDIN_RGB[v0->part - LAB_STANDIN], base);
                            /* per-part variation and the 10 cm checker */
                            float pv = v0->part >= LAB_STANDIN ? 1.0f : 0.9f + 0.2f * hash_float(hash_u32(v0->part * 7919u + 13u));
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
    static const struct { const char *name; float w, d, ceil; int form, side, host; uint32_t seed; float recess; int site; } B[] = {
        { "straight (slice)", 3.85f, 3.48f, 0.0f, NB_STRAIGHT, 0, 1, 0x5EED01u },
        { "straight wide", 5.2f, 3.3f, 0.0f, NB_STRAIGHT, 0, 1, 0x5EED04u },
        { "corner right", 4.4f, 3.6f, 0.0f, NB_CORNER, 1, 1, 0x5EED02u },
        { "corner left", 4.0f, 3.4f, 0.0f, NB_CORNER, -1, 1, 0x5EED03u },
        /* SF2's forms */
        { "stall compact", 2.4f, 2.5f, 0.0f, NB_STRAIGHT, 0, 1, 0x5EED05u },
        { "stall free", 3.4f, 3.1f, 0.0f, NB_STRAIGHT, 0, 0, 0x5EED06u },
        { "hatch kerb", 2.3f, 1.8f, 0.0f, NB_HATCH, 0, 0, 0x5EED07u },
        { "hatch wide", 3.2f, 2.6f, 0.0f, NB_HATCH, 0, 1, 0x5EED08u },
        { "arcade low", 3.2f, 3.0f, 2.9f, NB_ARCADE, 0, 1, 0x5EED09u },
        { "arcade vault", 3.6f, 3.3f, 3.8f, NB_ARCADE, 0, 1, 0x5EED0Au },
        { "tiered lantern", 4.6f, 3.9f, 0.0f, NB_TIERED, 0, 0, 0x5EED0Bu },
        { "tiered plant", 4.6f, 3.9f, 0.0f, NB_TIERED, 0, 0, 0x5EED0Bu },
        { "tiered both", 4.6f, 3.9f, 0.0f, NB_TIERED, 0, 0, 0x5EED0Bu },
        /* SF3's sites: a shop bay's stall (stools; a standing counter), the deep shop unit (the arcade counter at
         * the back of its room; with an awning), the inset corner unit in a building's corner shop (a whole bay; a
         * short one under a high head) */
        { "shop bay", 5.5f, 3.9f, 3.6f, NB_STRAIGHT, 0, 1, 0x5EED0Cu, 2.45f, 0 },
        { "shop standing", 5.5f, 2.95f, 4.4f, NB_STRAIGHT, 0, 1, 0x5EED0Du, 2.45f, 0 },
        { "shop deep", 5.5f, 5.8f, 3.4f, NB_ARCADE, 0, 1, 0x5EED0Eu, 5.2f, NB_SITE_DEEP },
        { "shop deep awning", 5.5f, 6.5f, 4.3f, NB_ARCADE, 0, 1, 0x5EED0Fu, 5.5f, NB_SITE_DEEP },
        { "corner inset", 5.8f, 3.6f, 3.6f, NB_CORNER, 1, 1, 0x5EED10u, 3.55f, NB_SITE_CORNER_POS },
        { "corner inset short", 4.2f, 3.4f, 4.6f, NB_CORNER, -1, 1, 0x5EED11u, 3.35f, NB_SITE_CORNER_NEG } };
    const int nb = (int)(sizeof(B) / sizeof(B[0]));
    const int BW = 620, BH = 460;
    Image im = { 0, 0, NULL };
    if (out) im = image_new(3 * BW, nb * BH);
    printf("\n%-18s %7s %6s %8s %5s %8s %6s %6s %6s  %s\n", "body", "struct", "inst", "drawn", "far", "KB", "build", "bake", "far ms", "checks");
    for (int b = 0; b < nb; ++b) {
        NbPlan p;
        uint32_t seed = B[b].seed;
        int fits = 0;
        /* SF2: the tiered kiosk's upper tier in each of its kinds */
        nb_force_upper(strstr(B[b].name, "lantern") ? NB_UPPER_LANTERN : strstr(B[b].name, "plant") ? NB_UPPER_PLANT :
                       strstr(B[b].name, "both") ? NB_UPPER_BOTH : -1);
        for (int t = 0; t < 64; ++t, seed += 7919u) {   /* a seed whose plan turns the corner the way asked */
            fits = nb_plan_site(&p, B[b].w, B[b].d, B[b].ceil, seed, B[b].form, VT_NOODLE, 0.5f, B[b].host | B[b].site, B[b].recess);
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
        nb_plan_site(&q, B[b].w, B[b].d, B[b].ceil, seed, B[b].form, VT_NOODLE, 0.5f, B[b].host | B[b].site, B[b].recess);
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
        {   /* SF2: the body at the coarser levels (what a lane of bodies would cost the pool) */
            PmMesh m1 = { 0 };
            int lt[2];
            for (int l = 0; l < 2; ++l) {
                m1.nv = m1.ni = 0;
                pm_begin(&pm, &m1, &li2, l ? PM_LOD2 : PM_LOD1);
                li2.n = 0;
                nb_body(&pm, &p);
                lt[l] = (int)(m1.ni / 3);
            }
            pm_mesh_free(&m1);
            li2.n = 0;
            printf("  %s: body at LOD1 %d, LOD2 %d triangles\n", B[b].name, lt[0], lt[1]);
        }
        CHECK(s.bad_normals == 0, "body %s: %d vertices whose normals disagree with their faces", B[b].name, s.bad_normals);
        /* SF2: the tiered kiosk (three counters, an upper tier) has a budget of its own */
        const int sbud = B[b].form == NB_TIERED ? 24000 : B[b].recess > 0.0f ? 22000 : NB_STRUCT_BUDGET,   /* SF3: the widest, its jambs to the head */ fbud = B[b].form == NB_TIERED ? 700 : B[b].site & (NB_SITE_CORNER_POS | NB_SITE_CORNER_NEG) ? 600 : NB_FAR_BUDGET;   /* (SF3: the inset unit's two panels, eight stools) */
        CHECK(s.tris <= sbud, "body %s: %d structure triangles, over %d", B[b].name, s.tris, sbud);
        t0 = now_ms();
        pm_begin(&pm, &f, NULL, PM_LOD2);
        nb_shell(&pm, &p);
        pm_bake(&f, NB_BAKE_RAYS, pm_reach(&f));
        double tf = now_ms() - t0;
        PmStats sf;
        pm_stats(&f, &sf);
        CHECK(sf.tris <= fbud, "body %s: far shell %d triangles, over %d", B[b].name, sf.tris, fbud);
        CHECK(sf.bad_normals == 0, "body %s: far shell: %d vertices whose normals disagree with their faces", B[b].name, sf.bad_normals);
        /* inside the site, under the duct's cap */
        CHECK(s.lo[0] > -0.5f * B[b].w - 0.01f && s.hi[0] < 0.5f * B[b].w + 0.01f && s.lo[2] > -0.01f && s.hi[2] < B[b].d + 0.25f,
              "body %s: reaches x %.2f..%.2f z %.2f..%.2f on a %.2f x %.2f m site", B[b].name, s.lo[0], s.hi[0], s.lo[2], s.hi[2], B[b].w, B[b].d);
        CHECK(B[b].ceil <= 0.0f || s.hi[1] <= B[b].ceil + 0.002f, "body %s: reaches %.2f m, over its ceiling at %.2f", B[b].name, s.hi[1], B[b].ceil);
        {   /* SF2: the line's rules hold in every form */
            NbLine Lc;
            nb_line(&p, &Lc);
            char err[512];
            const int bad = nb_line_check(&p, &Lc, err, sizeof(err));
            CHECK(bad == 0, "body %s: the line breaks %d rules: %s", B[b].name, bad, err);
        }
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
               nb_form_name(p.form), p.side > 0 ? "+x" : "-x", B[b].w, B[b].d, p.nstool, p.ntube, p.nicon, s.open_edges);
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

/* The kitchen of a bar's mesh m, cut away (the hood, the counter and the street taken off), into im's tile. */
static void cutaway(Image *im, const PmMesh *mm, const NbPlan *pp, int x, int y, int w, int h) {
    const PmMesh m = *mm;
    const NbPlan p = *pp;
    PmMesh cut = { 0 };
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
    render(im, &cut, x, y, w, h, SHOW_LIT);
    free(cut.idx); free(cut.v); free(cut.shell);
}

static const char *const TRADE_NAME[VT_COUNT] = { "noodles", "soup", "dumplings", "skewers", "kebab", "burgers", "fried",
                                                  "curry", "tacos", "crepes", "roast", "drinks" };

/* --------------------------------------------------- the cooking line --- */
/* NB5: the line's rules over a sweep of sites and seeds in both forms (nb_line_check), its determinism, the
 * second burner's mix; cutaway renders of four kitchens (the hood, the counter and the street taken off). */
static void line_section(const char *out) {
    static const float WS[] = { 3.2f, 3.5f, 3.85f, 4.2f, 4.6f, 5.2f }, DS[] = { 3.0f, 3.3f, 3.48f, 3.8f };
    int lines = 0, failing = 0, shown = 0, kinds[NB_POT_KINDS] = { 0 }, items = 0, fans = 0, bms = 0, steams = 0, trades[VT_COUNT] = { 0 }, mids = 0, aframes = 0;
    double t0 = now_ms();
    for (size_t wi = 0; wi < sizeof(WS) / sizeof(WS[0]); ++wi)
        for (size_t di = 0; di < sizeof(DS) / sizeof(DS[0]); ++di)
            for (int form = 0; form < NB_FORMS; ++form)
                for (uint32_t k = 0; k < 40; ++k) {
                    NbPlan p;
                    const uint32_t seed = hash_combine(0x11E5u, (uint32_t)(wi * 1000 + di * 100 + (size_t)form * 50) + k);
                    /* SF1: every trade, every clutter */
                    const int trade = (int)(k % VT_COUNT);
                    if (!nb_plan(&p, WS[wi], DS[di], 0.0f, seed, form, trade, (float)((k * 37u) % 100u) / 100.0f)) continue;
                    trades[trade]++; mids += p.rangeMid; aframes += p.aframeOn;
                    if (p.aframeOn) {   /* SF2: the A-frame clear of the stools, posts and counters (nb_solids) */
                        float sb[NB_SOLIDS_MAX][4];
                        NbPlan q = p;
                        q.aframeOn = 0;
                        const int ns = nb_solids(&q, sb, NB_SOLIDS_MAX);
                        const float ca = fabsf(cosf(p.aframe[2])), sa = fabsf(sinf(p.aframe[2]));
                        const float hx = 0.32f * ca + 0.22f * sa, hz = 0.32f * sa + 0.22f * ca;
                        for (int i = 0; i < ns; ++i)
                            CHECK(!(p.aframe[0] + hx > sb[i][0] && p.aframe[0] - hx < sb[i][2] && p.aframe[1] + hz > sb[i][1] && p.aframe[1] - hz < sb[i][3]),
                                  "line %.2f x %.2f seed %08x: the A-frame meets solid %d", WS[wi], DS[di], seed, i);
                    }
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
                        if (shown < 24) { printf("  line %.2f x %.2f %s seed %08x (%s, %s): %d broken\n%s", WS[wi], DS[di], p.form ? "corner" : "straight", seed, TRADE_NAME[trade], L.kind == NB_POT_WOK ? "wok" : L.kind == NB_POT_STEAMER ? "steamer" : L.kind == NB_POT_SAUCEPAN ? "saucepan" : "noodles", bad, err); shown++; }
                    }
                }
    printf("\nthe cooking line: %d lines over the sweep (%.1f ms), %d break a rule; %.1f items a line; second burner: noodles %d, wok %d, steamer %d, saucepan %d; a bain-marie in %d, a fan in %d; %d plumes\n",
           lines, now_ms() - t0, failing, lines ? (double)items / lines : 0.0, kinds[0], kinds[1], kinds[2], kinds[3], bms, fans, steams);
    printf("  the range along the bench in %d, an A-frame out at %d\n", mids, aframes);
    printf("  by trade:");
    for (int t = 0; t < VT_COUNT; ++t) printf(" %s %d", TRADE_NAME[t], trades[t]);
    printf("\n");
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
            nb_plan(&p, B[b].w, B[b].d, 0.0f, seed, B[b].form, VT_NOODLE, 0.5f);
            if (!B[b].side || p.side == B[b].side) break;
        }
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
        cutaway(&im, &m, &p, (b & 1) * CWL, (b >> 1) * CHL, CWL, CHL);
        static const char *const KIND[NB_POT_KINDS] = { "noodles", "wok", "steamer", "saucepan" };
        char label[96];
        snprintf(label, sizeof(label), "%s %.2fx%.2f %s %d pans", p.form ? "corner" : "straight", B[b].w, B[b].d, KIND[L.kind], L.bmPans);
        text(&im, (b & 1) * CWL + 8, (b >> 1) * CHL + 8, label, 2);
        pm_mesh_free(&m); pm_mesh_free(&none); pm_inst_free(&li);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/pmodel_line.png", out);
    stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
    free(im.p);
    /* SF1: the slice's bar in every trade */
    Image tr = image_new(4 * CWL, 3 * CHL);
    for (int t = 0; t < VT_COUNT; ++t) {
        NbPlan p;
        nb_plan(&p, 3.85f, 3.48f, 0.0f, 0x5EED01u, NB_STRAIGHT, t, 0.3f);
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
        cutaway(&tr, &m, &p, (t & 3) * CWL, (t >> 2) * CHL, CWL, CHL);
        char label[96];
        snprintf(label, sizeof(label), "%s %d items", TRADE_NAME[t], L.n);
        text(&tr, (t & 3) * CWL + 8, (t >> 2) * CHL + 8, label, 2);
        pm_mesh_free(&m); pm_mesh_free(&none); pm_inst_free(&li);
    }
    snprintf(path, sizeof(path), "%s/pmodel_trades.png", out);
    stbi_write_png(path, tr.w, tr.h, 3, tr.p, tr.w * 3);
    free(tr.p);
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

/* ------------------------------------------------ the houses' kit (HM4) --- */
/* Vignettes: the kit's assemblies (pmodel_house.h) dressing stand-ins for the house shells (walls with their
 * openings, roofs, stacks, drawn flat in LAB_STANDIN colours), each the street side of a style, so the board
 * shows what the kit adds to a house. Each is built twice: its modules as instances (counted against the
 * target's 150 instances a house within 60 m, and their triangles at each level) and built in (drawn). */
enum { SI_RENDER = LAB_STANDIN, SI_SIDING, SI_SLATE, SI_CLAY, SI_BRICK, SI_DARK };
static void si(Pm *pm, int part) { pm_mat(pm, PMAT_HCEMENT); pm->part = (uint8_t)part; }
static void si_end(Pm *pm) { pm->part = 0; }
/* a wall in the xy plane from (x0, y0) w by h, its face at z 0 (t behind), less openings o (x0, y0, x1, y1 in
 * the wall's own frame), each with dark glass `reveal` behind the face */
static void si_wall(Pm *pm, int part, float x0, float y0, float w, float h, float t, const float (*o)[4], int n, float reveal) {
    float xs[64], ys[64];
    int nx = 0, ny = 0;
    xs[nx++] = x0; xs[nx++] = x0 + w; ys[ny++] = y0; ys[ny++] = y0 + h;
    for (int k = 0; k < n; ++k) { xs[nx++] = o[k][0]; xs[nx++] = o[k][2]; ys[ny++] = o[k][1]; ys[ny++] = o[k][3]; }
    for (int i = 0; i < nx; ++i) for (int j = i + 1; j < nx; ++j) if (xs[j] < xs[i]) { float q = xs[i]; xs[i] = xs[j]; xs[j] = q; }
    for (int i = 0; i < ny; ++i) for (int j = i + 1; j < ny; ++j) if (ys[j] < ys[i]) { float q = ys[i]; ys[i] = ys[j]; ys[j] = q; }
    si(pm, part);
    for (int i = 0; i + 1 < nx; ++i)
        for (int j = 0; j + 1 < ny; ++j) {
            const float a = xs[i], b = xs[i + 1], c = ys[j], d = ys[j + 1];
            if (b - a < 1e-4f || d - c < 1e-4f || a < x0 - 1e-4f || b > x0 + w + 1e-4f || c < y0 - 1e-4f || d > y0 + h + 1e-4f) continue;
            const float mx = 0.5f * (a + b), my = 0.5f * (c + d);
            int hole = 0;
            for (int k = 0; k < n; ++k) if (mx > o[k][0] && mx < o[k][2] && my > o[k][1] && my < o[k][3]) hole = 1;
            if (!hole) pm_box(pm, a, c, -t, b, d, 0.0f);
        }
    si_end(pm);
    pm_mat(pm, PMAT_HGLASS);
    for (int k = 0; k < n; ++k) pm_box(pm, o[k][0], o[k][1], -reveal - 0.03f, o[k][2], o[k][3], -reveal);
}
/* a box stand-in */
static void si_box(Pm *pm, int part, float x0, float y0, float z0, float x1, float y1, float z1) { si(pm, part); pm_box(pm, x0, y0, z0, x1, y1, z1); si_end(pm); }
/* a gabled roof over x0..x1 (its ridge along x): the front eaves at (y, zf), depth back to the rear eaves, pitch
 * degrees, slabs t thick; returns the ridge's top (y, z) */
static void si_roof(Pm *pm, int part, float x0, float x1, float y, float zf, float depth, float pitch, float t, float *ry, float *rz) {
    const float a = pitch * PI_F / 180.0f, run = 0.5f * depth / cosf(a);
    si(pm, part);
    pm_push(pm); pm_translate(pm, x0, y, zf); pm_rotate_x(pm, a); pm_box(pm, 0.0f, 0.0f, -run, x1 - x0, t, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, x0, y, zf - depth); pm_rotate_x(pm, -a); pm_box(pm, 0.0f, 0.0f, 0.0f, x1 - x0, t, run); pm_pop(pm);
    si_end(pm);
    *ry = y + t * cosf(a) + run * sinf(a);
    *rz = zf - 0.5f * depth + t * sinf(a) * 0.0f;
}
/* a point on a roof plane rising from the eaves (y, zf) at pitch: s metres up the slope, on its top (t thick) */
static void slope_pt(float y, float zf, float pitch, float t, float s, float *py, float *pz) {
    const float a = pitch * PI_F / 180.0f;
    *py = y + t * cosf(a) + s * sinf(a);
    *pz = zf + t * sinf(a) - s * cosf(a);
}

/* 1. Victorian terraced: brick, a two-storey canted bay, sashes, stone dressings, a hood mould, a hopper. */
static void vig_victorian(Pm *pm) {
    const float W = 5.0f, pl = 0.3f, sh = 2.9f, eaves = 6.0f;
    const float o[3][4] = { { 3.45f, pl, 4.35f, pl + 2.15f }, { 3.45f, pl + sh + 0.8f, 4.35f, pl + sh + 2.4f }, { 0.4f, 0.0f, 0.401f, 0.001f } };
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, eaves, 0.3f, o, 2, 0.14f);
    si_box(pm, SI_DARK, 3.45f, 0.0f, -0.6f, 4.35f, pl, 0.0f);   /* the step into the recess */
    /* the bay's walls */
    HkBay b = { HK_BAY_CANTED, 2, HK_FRAME_SASH, HK_SILL_STONE, 1.3f, 0.75f, sh, 0.8f, 2.4f };
    const float bx0 = 1.6f, F = b.front, P = b.proj;
    const float c[4][2] = { { -0.5f * F - P, 0 }, { -0.5f * F, P }, { 0.5f * F, P }, { 0.5f * F + P, 0 } };
    for (int f = 0; f < 3; ++f) {
        const float dx = c[f + 1][0] - c[f][0], dz = c[f + 1][1] - c[f][1], len = sqrtf(dx * dx + dz * dz), light = len - 0.13f;
        const float ex[3] = { dx / len, 0, dz / len }, ey[3] = { 0, 1, 0 }, ez[3] = { -dz / len, 0, dx / len };
        const float org[3] = { bx0 + 0.5f * (c[f][0] + c[f + 1][0]), 0.0f, 0.5f * (c[f][1] + c[f + 1][1]) };
        pm_push(pm); pm_frame(pm, org, ex, ey, ez);
        float oo[2][4];
        for (int s = 0; s < 2; ++s) { oo[s][0] = -0.5f * light; oo[s][2] = 0.5f * light; oo[s][1] = pl + sh * (float)s + b.sillH; oo[s][3] = pl + sh * (float)s + b.headH; }
        si_wall(pm, SI_BRICK, -0.5f * len, 0.0f, len, eaves - 0.25f, 0.25f, (const float (*)[4])oo, 2, 0.08f);
        pm_pop(pm);
    }
    si_box(pm, SI_DARK, bx0 - 0.5f * F - P - 0.05f, eaves - 0.25f, 0.0f, bx0 + 0.5f * F + P + 0.05f, eaves - 0.17f, P + 0.08f);
    pm_push(pm); pm_translate(pm, bx0, pl, 0.0f); hk_bay(pm, &b); pm_pop(pm);
    /* the door with a fanlight, the window over it with its hood mould */
    pm_push(pm); pm_translate(pm, 3.9f, pl, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.9f, 2.15f, 0.14f); pm_pop(pm);
    HkWindow w = { HK_FRAME_SASH, HK_SILL_STONE, HK_HEAD_HOOD, 0, 0, 0, 0.9f, 1.6f, 0.14f };
    pm_push(pm); pm_translate(pm, 3.9f, pl + sh + 0.8f, 0.0f); hk_window(pm, &w); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, pl + sh - 0.1f, 0.0f); hk_run(pm, PMK_H_STRING_COURSE, W, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, 0.0f); hk_run(pm, PMK_H_PLINTH, W, 0.0f); pm_pop(pm);
    /* eaves: brackets under a shallow soffit, a fascia, the ogee gutter */
    si_box(pm, SI_DARK, 0.0f, eaves - 0.02f, 0.0f, W, eaves, 0.3f);
    for (int k = 0; k < 4; ++k) { pm_push(pm); pm_translate(pm, 3.0f + 0.65f * (float)k, eaves - 0.02f, 0.0f); pm_place(pm, PMK_H_EAVES_BRACKET); pm_pop(pm); }
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, 0.0f, eaves - 0.18f, 0.28f, W, eaves + 0.05f, 0.31f);
    pm_push(pm); pm_translate(pm, 0.0f, eaves - 0.02f, 0.31f); hk_gutter(pm, HK_GUTTER_OGEE, W, 1); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W - 0.12f, 0.0f, 0.0f); hk_downpipe(pm, eaves - 0.12f, 0.31f + HK_GUTTER_Z, 1); pm_pop(pm);
    float ry, rz;
    si_roof(pm, SI_SLATE, 0.0f, W, eaves + 0.05f, 0.33f, 6.6f, 40.0f, 0.12f, &ry, &rz);
    pm_push(pm); pm_translate(pm, 0.0f, ry, rz); hk_ridge(pm, 1, W); pm_pop(pm);
    si_box(pm, SI_BRICK, W - 0.55f, ry - 1.5f, rz - 0.3f, W + 0.45f, ry + 1.2f, rz + 0.3f);
    pm_push(pm); pm_translate(pm, W - 0.05f, ry + 1.2f, rz); hk_chimney_top(pm, 1.0f, 0.6f, 4, 7u); pm_pop(pm);
}

/* 2. Georgian: three storeys of brick over a stone plinth, six over six sashes under lintels and keystones, quoins,
 * a string course, a pedimented doorcase, a cornice under the parapet. */
static void vig_georgian(Pm *pm) {
    const float W = 6.5f, pl = 0.45f, sh = 3.0f, top = 9.6f;
    const float xs[3] = { 1.2f, 3.25f, 5.3f };
    const float sill[3] = { 0.75f, 0.6f, 0.6f }, hgt[3] = { 1.8f, 2.0f, 1.4f };
    float o[9][4];
    int n = 0;
    for (int f = 0; f < 3; ++f)
        for (int k = 0; k < 3; ++k) {
            if (f == 0 && k == 2) { o[n][0] = 4.8f; o[n][1] = pl; o[n][2] = 5.8f; o[n][3] = pl + 2.1f; n++; continue; }
            o[n][0] = xs[k] - 0.475f; o[n][2] = xs[k] + 0.475f; o[n][1] = pl + sh * (float)f + sill[f]; o[n][3] = o[n][1] + hgt[f]; n++;
        }
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, top, 0.3f, (const float (*)[4])o, n, 0.14f);
    for (int f = 0; f < 3; ++f)
        for (int k = 0; k < 3; ++k) {
            if (f == 0 && k == 2) continue;
            HkWindow w = { HK_FRAME_GEORGIAN, HK_SILL_STONE, f == 2 ? HK_HEAD_LINTEL : HK_HEAD_KEYSTONE, 0, 0, 0, 0.95f, hgt[f], 0.14f };
            pm_push(pm); pm_translate(pm, xs[k], pl + sh * (float)f + sill[f], 0.0f); hk_window(pm, &w); pm_pop(pm);
        }
    si_box(pm, SI_DARK, 4.7f, 0.0f, 0.0f, 5.9f, 0.15f, 0.35f);
    si_box(pm, SI_DARK, 4.75f, 0.15f, 0.0f, 5.85f, 0.3f, 0.2f);
    pm_push(pm); pm_translate(pm, 5.3f, pl, 0.0f); hk_door(pm, HK_DOOR_6P, HK_CASE_PEDIMENT, 1.0f, 2.1f, 0.14f); pm_pop(pm);
    hk_run(pm, PMK_H_PLINTH, W, 0.0f);
    pm_push(pm); pm_translate(pm, 0.0f, pl + sh - 0.12f, 0.0f); hk_run(pm, PMK_H_STRING_COURSE, W, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, top - 0.75f, 0.0f); hk_run(pm, PMK_H_BAY_CORNICE, W, 1.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W, 0.45f, 0.0f); hk_quoins(pm, top - 1.3f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, 0.45f, 0.0f); pm_rotate_y(pm, -0.5f * PI_F); hk_quoins(pm, top - 1.3f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.15f, 0.0f, 0.0f); hk_downpipe(pm, top - 0.4f, 0.1f, 1); pm_pop(pm);
}

/* 3. Craftsman bungalow: siding, a front gable on knee braces, exposed rafter tails along the side, a porch on
 * tapered columns over brick piers, grid windows in casings, the craftsman door. */
static void vig_craftsman(Pm *pm) {
    const float W = 8.0f, deck = 0.6f, wall = 3.2f, pitch = 30.0f, tp = tanf(pitch * PI_F / 180.0f), ov = 0.6f;
    const float o[3][4] = { { 1.85f, deck, 2.75f, deck + 2.05f }, { 5.0f, deck + 0.9f, 6.0f, deck + 2.3f }, { 6.6f, deck + 0.9f, 7.6f, deck + 2.3f } };
    si_wall(pm, SI_SIDING, 0.0f, 0.0f, W, wall, 0.25f, o, 3, 0.10f);
    {   /* the gable's triangle */
        si(pm, SI_SIDING);
        const float tri[3][2] = { { 0.0f, wall }, { W, wall }, { 0.5f * W, wall + 0.5f * W * tp } };
        pm_extrude(pm, tri, 3, -0.25f, 0.0f);
        si_end(pm);
    }
    /* the roof: two slopes, the ridge along z, the verges 0.6 m out over the gable */
    si(pm, SI_DARK);
    for (int s = 0; s < 2; ++s) {
        const float a = pitch * PI_F / 180.0f, run = (0.5f * W + ov) / cosf(a);
        pm_push(pm);
        pm_translate(pm, 0.5f * W, wall + 0.5f * W * tp + 0.05f, 0.0f);
        pm_rotate_z(pm, s ? -a : a);
        if (s) pm_box(pm, 0.0f, 0.0f, -9.0f, run, 0.15f, ov); else pm_box(pm, -run, 0.0f, -9.0f, 0.0f, 0.15f, ov);
        pm_pop(pm);
    }
    si_end(pm);
    /* knee braces under the verges, a plain bargeboard (the shell's verge) */
    const float bxs[3] = { 0.5f, 0.5f * W, W - 0.5f };
    for (int k = 0; k < 3; ++k) {
        const float y = wall + (0.5f * W - fabsf(bxs[k] - 0.5f * W)) * tp + 0.05f;
        pm_push(pm); pm_translate(pm, bxs[k], y - 0.05f, 0.0f); pm_scale(pm, 1.0f, 1.0f, ov / 0.75f); pm_place(pm, PMK_H_KNEE_BRACE); pm_pop(pm);
    }
    /* rafter tails along the right side's eaves */
    pm_push(pm); pm_translate(pm, W, wall, 0.0f); pm_rotate_y(pm, 0.5f * PI_F); hk_rafter_tails(pm, 8.0f, 0.6f, pitch, ov); pm_pop(pm);
    si_box(pm, SI_SIDING, W - 0.25f, 0.0f, -8.0f, W, wall, 0.0f);
    /* the porch: a deck, piers and columns, a beam, a shed roof back to the wall */
    si_box(pm, SI_DARK, 0.2f, 0.0f, 0.0f, 4.6f, deck, 2.6f);
    for (int k = 0; k < 2; ++k) { pm_push(pm); pm_translate(pm, k ? 4.25f : 0.55f, 0.0f, 2.25f); pm_scale(pm, 1.0f, 2.7f / HK_POST_H, 1.0f); pm_place(pm, PMK_H_POST_CRAFT); pm_pop(pm); }
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, 0.25f, 2.7f, 2.1f, 4.55f, 3.0f, 2.4f);
    {
        si(pm, SI_DARK);
        const float a = atan2f(0.45f, 2.6f);
        pm_push(pm); pm_translate(pm, 0.1f, 3.0f, 2.75f); pm_rotate_x(pm, a); pm_box(pm, 0.0f, 0.0f, -2.9f, 4.6f, 0.12f, 0.0f); pm_pop(pm);
        si_end(pm);
    }
    pm_push(pm); pm_translate(pm, 0.55f, deck, 2.25f); hk_balustrade(pm, 1.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 2.3f, deck, 0.0f); hk_door(pm, HK_DOOR_CRAFT, HK_CASE_NONE, 0.9f, 2.05f, 0.1f); pm_pop(pm);
    for (int k = 0; k < 2; ++k) {
        HkWindow w = { HK_FRAME_GRID, HK_SILL_TIMBER, HK_HEAD_CASING, 0, 0, 0, 1.0f, 1.4f, 0.10f };
        pm_push(pm); pm_translate(pm, k ? 7.1f : 5.5f, deck + 0.9f, 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
}

/* 4. Inter-war semi: render over brick, a square bay under a half-timbered gable with scalloped bargeboards,
 * casements, a tile-hung band, a gabled door canopy, a half-glazed door, the half-round gutter. */
static void vig_interwar(Pm *pm) {
    const float W = 5.5f, pl = 0.25f, sh = 2.75f, eaves = 5.7f, pitch = 45.0f;
    const float o[3][4] = { { 3.9f, pl, 4.8f, pl + 2.05f }, { 3.75f, pl + sh + 0.8f, 4.95f, pl + sh + 1.8f }, { 0, 0, 0, 0 } };
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, pl + sh, 0.25f, o, 1, 0.1f);
    si_wall(pm, SI_RENDER, 0.0f, pl + sh, W, eaves - pl - sh, 0.25f, o + 1, 1, 0.1f);
    /* the square bay */
    HkBay b = { HK_BAY_SQUARE, 2, HK_FRAME_CASEMENT, HK_SILL_TIMBER, 2.4f, 0.6f, sh, 0.85f, 2.2f };
    const float bc = 1.6f;
    for (int s = 0; s < 2; ++s) {
        float oo[1][4] = { { bc - 1.2f + 0.065f, pl + sh * (float)s + b.sillH, bc + 1.2f - 0.065f, pl + sh * (float)s + b.headH } };
        pm_push(pm); pm_translate(pm, 0.0f, 0.0f, b.proj);
        si_wall(pm, s ? SI_RENDER : SI_BRICK, bc - 1.2f, pl + sh * (float)s - (s ? 0.0f : pl), 2.4f, s ? eaves - pl - sh : pl + sh, 0.6f, (const float (*)[4])oo, 1, 0.08f);
        pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, bc, pl, 0.0f); hk_bay(pm, &b); pm_pop(pm);
    /* the gable over the bay: render, half-timbered, bargeboards, a finial */
    const float half = 1.45f, tp = tanf(pitch * PI_F / 180.0f);
    {
        si(pm, SI_RENDER);
        const float tri[3][2] = { { bc - half, eaves }, { bc + half, eaves }, { bc, eaves + half * tp } };
        pm_push(pm); pm_translate(pm, 0.0f, 0.0f, b.proj); pm_extrude(pm, tri, 3, -0.6f, 0.0f); pm_pop(pm);
        si_end(pm);
        si(pm, SI_CLAY);
        for (int s = 0; s < 2; ++s) {
            const float a = pitch * PI_F / 180.0f, run = (half + 0.25f) / cosf(a);
            pm_push(pm); pm_translate(pm, bc, eaves + half * tp + 0.03f, 0.0f); pm_rotate_z(pm, s ? -a : a);
            if (s) pm_box(pm, 0.0f, 0.0f, -3.0f, run, 0.1f, b.proj + 0.25f); else pm_box(pm, -run, 0.0f, -3.0f, 0.0f, 0.1f, b.proj + 0.25f);
            pm_pop(pm);
        }
        si_end(pm);
    }
    pm_push(pm); pm_translate(pm, bc, eaves, b.proj); hk_tudor_gable(pm, half, pitch, 0.45f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, bc, eaves + half * tp + 0.02f, b.proj + 0.27f); hk_bargeboards(pm, HK_BARGE_SCALLOP, half + 0.25f, pitch, 1); pm_pop(pm);
    /* the tile-hung band between the storeys on the right, the casement over it */
    pm_push(pm); pm_translate(pm, 3.3f, pl + 2.35f, 0.0f); hk_hung_tiles(pm, 2.2f, 1.1f); pm_pop(pm);
    HkWindow w = { HK_FRAME_CASEMENT, HK_SILL_TIMBER, HK_HEAD_NONE, 0, 0, 0, 1.2f, 1.0f, 0.1f };
    pm_push(pm); pm_translate(pm, 4.35f, pl + sh + 0.8f, 0.0f); hk_window(pm, &w); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 4.35f, pl, 0.0f); hk_door(pm, HK_DOOR_HALF, HK_CASE_NONE, 0.9f, 2.05f, 0.1f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 4.35f, pl + 2.25f, 0.0f); pm_place(pm, PMK_H_CANOPY_GABLE); pm_pop(pm);
    /* the main roof, its eaves and gutter */
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, 2.9f, eaves - 0.2f, 0.25f, W, eaves + 0.02f, 0.28f);
    pm_push(pm); pm_translate(pm, 2.9f, eaves, 0.28f); hk_gutter(pm, HK_GUTTER_HALF, W - 2.9f, 1); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W - 0.12f, 0.0f, 0.0f); hk_downpipe(pm, eaves - 0.1f, 0.28f + HK_GUTTER_Z, 0); pm_pop(pm);
    float ry, rz;
    si_roof(pm, SI_CLAY, 0.0f, W, eaves + 0.02f, 0.3f, 7.0f, 40.0f, 0.12f, &ry, &rz);
    pm_push(pm); pm_translate(pm, 0.0f, ry, rz); hk_ridge(pm, 0, W); pm_pop(pm);
}

/* 5. The roofscape: a slate roof with a gable verge (gothic bargeboards, a finial), the three dormers, a rooflight,
 * a vent pipe, ridge tiles, a stack with its cap and each kind of pot, the ogee gutter and a swan neck. */
static void vig_roofscape(Pm *pm) {
    const float W = 7.0f, eaves = 3.0f, pitch = 45.0f, t = 0.12f, depth = 7.0f;
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, eaves, 0.3f, NULL, 0, 0.1f);
    float ry, rz;
    si_roof(pm, SI_SLATE, -0.3f, W, eaves, 0.3f, depth, pitch, t, &ry, &rz);
    {   /* the gable end's wall */
        si(pm, SI_BRICK);
        const float h = 0.5f * depth * tanf(pitch * PI_F / 180.0f);
        const float tri[3][2] = { { 0.3f, eaves }, { 0.3f - depth, eaves }, { 0.3f - 0.5f * depth, eaves + h - 0.1f } };
        pm_push(pm); pm_rotate_y(pm, -0.5f * PI_F);   /* its plane x 0, facing -x: the triangle's x along +z */
        pm_extrude(pm, tri, 3, -0.3f, 0.0f);
        pm_pop(pm);
        si_end(pm);
        si_box(pm, SI_BRICK, -0.0f, 0.0f, 0.3f - depth, 0.3f, eaves, 0.0f);
    }
    pm_push(pm); pm_translate(pm, -0.33f, ry - 0.06f, rz); pm_rotate_y(pm, -0.5f * PI_F); hk_bargeboards(pm, HK_BARGE_GOTHIC, 0.5f * depth + 0.3f, pitch, 1); pm_pop(pm);
    pm_push(pm); pm_translate(pm, -0.3f, ry, rz); hk_ridge(pm, 0, W + 0.3f); pm_pop(pm);
    const int kinds[3] = { HK_DORMER_GABLE, HK_DORMER_HIP, HK_DORMER_FLAT };
    for (int k = 0; k < 3; ++k) {
        float py, pz;
        slope_pt(eaves, 0.3f, pitch, t, 1.1f, &py, &pz);
        pm_push(pm); pm_translate(pm, 1.3f + 1.95f * (float)k, py, pz); hk_dormer(pm, kinds[k], k == 2 ? 2.0f : 1.6f); pm_pop(pm);
    }
    {
        float py, pz;
        slope_pt(eaves, 0.3f, pitch, t, 3.4f, &py, &pz);
        pm_push(pm); pm_translate(pm, 2.3f, py, pz); pm_rotate_x(pm, pitch * PI_F / 180.0f); pm_place(pm, PMK_H_ROOFLIGHT); pm_pop(pm);
        slope_pt(eaves, 0.3f, pitch, t, 3.0f, &py, &pz);
        pm_push(pm); pm_translate(pm, 5.6f, py, pz); pm_place(pm, PMK_H_VENT_PIPE); pm_pop(pm);
    }
    /* the stack on the ridge, its cap and four pots, one of each */
    si_box(pm, SI_BRICK, 3.9f, ry - 1.2f, rz - 0.3f, 5.1f, ry + 1.1f, rz + 0.3f);
    pm_push(pm); pm_translate(pm, 4.5f, ry + 1.1f, rz); pm_scale(pm, 1.2f, 1.0f, 0.6f / HK_STACK_D); pm_place(pm, PMK_H_STACK_CAP); pm_pop(pm);
    const int pots[4] = { PMK_H_POT_PLAIN, PMK_H_POT_CANNON, PMK_H_POT_CROWN, PMK_H_POT_COWL };
    for (int k = 0; k < 4; ++k) { pm_push(pm); pm_translate(pm, 4.5f - 0.45f + 0.3f * (float)k, ry + 1.14f, rz); pm_place(pm, pots[k]); pm_pop(pm); }
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, -0.3f, eaves - 0.18f, 0.28f, W, eaves + 0.02f, 0.31f);
    pm_push(pm); pm_translate(pm, -0.3f, eaves - 0.02f, 0.31f); hk_gutter(pm, HK_GUTTER_OGEE, W + 0.3f, 1); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 6.5f, 0.0f, 0.0f); hk_downpipe(pm, eaves - 0.1f, 0.31f + HK_GUTTER_Z, 0); pm_pop(pm);
}

/* 6. Post-war, run-down: render, plastic replacement windows and door, concrete sills and lintels, a flat canopy, a
 * grille over a ground-floor window, grey plastic rainwater goods. */
static void vig_postwar(Pm *pm) {
    const float W = 6.0f, pl = 0.15f, sh = 2.6f, eaves = 5.4f;
    const float o[5][4] = { { 0.7f, pl + 0.85f, 1.9f, pl + 2.05f }, { 2.55f, pl, 3.45f, pl + 2.05f }, { 4.0f, pl + 0.85f, 5.2f, pl + 2.05f },
                            { 0.7f, pl + sh + 0.85f, 1.9f, pl + sh + 2.05f }, { 4.0f, pl + sh + 0.85f, 5.2f, pl + sh + 2.05f } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, eaves, 0.25f, o, 5, 0.08f);
    for (int k = 0; k < 5; ++k) {
        if (k == 1) continue;
        HkWindow w = { HK_FRAME_UPVC, HK_SILL_CONCRETE, HK_HEAD_CONCRETE, 0, k == 0, 0, 1.2f, 1.2f, 0.08f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, 3.0f, pl, 0.0f); hk_door(pm, HK_DOOR_UPVC, HK_CASE_NONE, 0.9f, 2.05f, 0.08f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 3.0f, pl + 2.3f, 0.0f); pm_place(pm, PMK_H_CANOPY_FLAT); pm_pop(pm);
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, 0.0f, eaves - 0.2f, 0.2f, W, eaves + 0.02f, 0.23f);
    pm_push(pm); pm_translate(pm, 0.0f, eaves, 0.23f); hk_gutter(pm, HK_GUTTER_HALF, W, 3); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.15f, 0.0f, 0.0f); hk_downpipe(pm, eaves - 0.1f, 0.23f + HK_GUTTER_Z, 0); pm_pop(pm);
    float ry, rz;
    si_roof(pm, SI_DARK, -0.2f, W + 0.2f, eaves + 0.02f, 0.25f, 7.5f, 35.0f, 0.12f, &ry, &rz);
    pm_push(pm); pm_translate(pm, -0.2f, ry, rz); hk_ridge(pm, 0, W + 0.4f); pm_pop(pm);
}

/* 7. Shutters and the southern dressings: render, panelled shutters, louvred ones, a window box, a grille, flat
 * sills, a pedimented door (fanlight alone over it). */
static void vig_shutters(Pm *pm) {
    const float W = 5.6f, pl = 0.2f, sh = 3.1f, top = 6.6f;
    const float o[5][4] = { { 0.9f, pl + 0.8f, 1.9f, pl + 2.1f }, { 3.75f, pl, 4.75f, pl + 2.2f }, { 0.95f, pl + sh + 0.6f, 1.85f, pl + sh + 2.2f },
                            { 3.8f, pl + sh + 0.6f, 4.7f, pl + sh + 2.2f }, { 0, 0, 0, 0 } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, top, 0.3f, o, 4, 0.12f);
    HkWindow a = { HK_FRAME_CASEMENT, HK_SILL_STONE, HK_HEAD_NONE, HK_SHUT_PANEL, 0, 1, 1.0f, 1.3f, 0.12f };
    pm_push(pm); pm_translate(pm, 1.4f, pl + 0.8f, 0.0f); hk_window(pm, &a); pm_pop(pm);
    HkWindow b = { HK_FRAME_SASH, HK_SILL_STONE, HK_HEAD_LINTEL, HK_SHUT_LOUVRE, 0, 0, 0.9f, 1.6f, 0.12f };
    pm_push(pm); pm_translate(pm, 1.4f, pl + sh + 0.6f, 0.0f); hk_window(pm, &b); pm_pop(pm);
    HkWindow c = { HK_FRAME_SASH, HK_SILL_STONE, HK_HEAD_LINTEL, 0, 1, 0, 0.9f, 1.6f, 0.12f };
    pm_push(pm); pm_translate(pm, 4.25f, pl + sh + 0.6f, 0.0f); hk_window(pm, &c); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 4.25f, pl, 0.0f); hk_door(pm, HK_DOOR_6P, HK_CASE_FANLIGHT, 1.0f, 2.2f, 0.12f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, top - 0.2f, 0.0f); hk_run(pm, PMK_H_BAY_CORNICE, W, 1.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, pl + sh - 0.15f, 0.0f); hk_run(pm, PMK_H_STRING_COURSE, W, 0.0f); pm_pop(pm);
}

/* HM5c1, the shared retrofit kit (pmodel_retrofit.h) on stand-ins after the concept sheets
 * (docs/reference/international-residential): 8. a flat-roofed house (the RCC, self-built and shophouse sheets' panel
 * 1): a black tank on a stand and one on the deck, a solar canopy with its downlights over the front of the roof,
 * railings on the parapet, plants trailing over it, steel risers up the front and over onto the deck, a bank of
 * meters, a security gate over the door, a bulkhead, a vine, pots and an aerial. */
static void vig_retrofit_flat(Pm *pm) {
    const float W = 7.0f, D = 8.0f, pl = 0.15f, sh = 3.0f, deck = 6.0f, top = 6.9f;
    const float o[5][4] = { { 0.5f, pl + 0.8f, 1.9f, pl + 2.0f }, { 3.7f, pl, 4.7f, pl + 2.2f },
                            { 0.5f, pl + sh + 0.8f, 1.9f, pl + sh + 2.0f }, { 3.7f, pl + sh + 0.8f, 5.1f, pl + sh + 2.0f }, { 0, 0, 0, 0 } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, top, 0.3f, o, 4, 0.1f);
    si_box(pm, SI_DARK, 0.25f, deck - 0.1f, -D + 0.25f, W - 0.25f, deck, -0.3f);
    si_box(pm, SI_RENDER, 0.0f, 0.0f, -D, 0.25f, top, -0.3f);
    si_box(pm, SI_RENDER, W - 0.25f, 0.0f, -D, W, top, -0.3f);
    si_box(pm, SI_RENDER, 0.25f, deck, -D, W - 0.25f, top, -D + 0.25f);
    for (int k = 0; k < 4; ++k) {
        if (k == 1) continue;
        HkWindow w = { HK_FRAME_CASEMENT, HK_SILL_NONE, HK_HEAD_CONCRETE, 0, k == 0, 0, o[k][2] - o[k][0], 1.2f, 0.1f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, 4.2f, pl, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 1.0f, 2.2f, 0.1f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 4.2f, pl, 0.004f); pm_scale(pm, 1.14f / HR_GATE_W, 2.26f / HR_GATE_H, 1.0f); pm_place(pm, PMK_R_GATE_BARS); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 4.2f, pl + 2.4f, 0.0f); pm_place(pm, PMK_R_BULKHEAD); pm_pop(pm);
    for (int k = 0; k < 3; ++k) { pm_push(pm); pm_translate(pm, 2.3f + 0.42f * (float)k, 1.3f, 0.0f); pm_place(pm, PMK_R_METER); pm_pop(pm); }
    pm_push(pm); pm_translate(pm, 2.72f, 2.05f, 0.05f); pm_scale(pm, 0.35f, top - 0.15f - 2.05f, 0.35f); pm_place(pm, PMK_R_PIPE_FE); pm_pop(pm);
    /* risers up the front's corner, over the parapet, down onto the deck */
    for (int k = 0; k < 2; ++k) {
        const float x = 6.55f + 0.12f * (float)k, H = top + 0.18f + 0.06f * (float)k;
        const float P[4][3] = { { x, 0.0f, HR_PIPE_Z }, { x, H, HR_PIPE_Z }, { x, H, HR_PIPE_Z - 0.55f }, { x, deck + 0.08f, HR_PIPE_Z - 0.55f } };
        hr_pipe_path(pm, 1, P, 4);
        if (!k) { const float a[3] = { x, 0.0f, HR_PIPE_Z }, b[3] = { x, H - 0.2f, HR_PIPE_Z }; hr_pipe_clips(pm, 1, a, b); }
    }
    pm_push(pm); pm_translate(pm, 6.55f, 1.1f, HR_PIPE_Z); pm_place(pm, PMK_R_VALVE); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 5.65f, 0.0f, 0.0f); hr_vine(pm, 0.9f, 2.7f, HR_VINE_IVY); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 3.2f, 0.0f, 0.4f); pm_place(pm, PMK_R_POT_PALM); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 5.0f, 0.0f, 0.35f); pm_place(pm, PMK_R_POT); pm_pop(pm);
    /* the roof: tanks along the back, the canopy over the front, railings round the parapet, plants over its front */
    pm_push(pm); pm_translate(pm, W - 1.1f, deck, -D + 1.1f); hr_tank(pm, 0, 1.0f, 1.2f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W - 2.7f, deck, -D + 0.9f); hr_tank(pm, 0, 0.8f, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 3.2f, deck, -2.4f); pm_rotate_y(pm, PI_F); hr_solar_canopy(pm, 5.2f, 3.4f, 2.4f, 0.25f, 1, NULL); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.2f, top, -0.125f); hr_rail(pm, W - 0.4f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W - 0.125f, top, -0.3f); pm_rotate_y(pm, 0.5f * PI_F); hr_rail(pm, D - 0.6f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.6f, top, 0.0f); hr_trail(pm, 2.6f, 0.95f, HR_VINE_BOUGAINVILLEA); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.5f, deck, -D + 0.5f); pm_place(pm, PMK_R_AERIAL_YAGI); pm_pop(pm);
}
/* 9. A pitched roof (the Victorian and inter-war sheets' panel 1): panels on the street's slope, a black tank on a
 * cradle by the ridge, a cross aerial on the stack, copper risers up the corner into the eaves, an LED strip under the
 * soffit, a lantern and a low meter by the door, a vine, a trough, pots. */
static void vig_retrofit_pitched(Pm *pm) {
    const float W = 6.0f, pl = 0.15f, sh = 2.8f, eaves = 5.6f, pitch = 40.0f, depth = 8.0f, t = 0.12f;
    const float o[5][4] = { { 0.9f, pl + 0.8f, 2.4f, pl + 2.0f }, { 3.3f, pl, 4.2f, pl + 2.1f },
                            { 0.9f, pl + sh + 0.8f, 2.4f, pl + sh + 2.0f }, { 3.6f, pl + sh + 0.8f, 4.8f, pl + sh + 2.0f }, { 0, 0, 0, 0 } };
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, eaves, 0.3f, o, 4, 0.1f);
    for (int k = 0; k < 4; ++k) {
        if (k == 1) continue;
        HkWindow w = { HK_FRAME_CASEMENT, HK_SILL_CONCRETE, HK_HEAD_SOLDIER, 0, 0, 0, o[k][2] - o[k][0], 1.2f, 0.1f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    pm_push(pm); pm_translate(pm, 3.75f, pl, 0.0f); hk_door(pm, HK_DOOR_HALF, HK_CASE_NONE, 0.9f, 2.1f, 0.1f); pm_pop(pm);
    si_box(pm, SI_DARK, 0.0f, eaves - 0.02f, 0.0f, W, eaves, 0.35f);
    pm_mat(pm, PMAT_HTRIM); pm_box(pm, 0.0f, eaves - 0.18f, 0.33f, W, eaves + 0.05f, 0.36f);
    pm_push(pm); pm_translate(pm, 0.0f, eaves - 0.02f, 0.36f); hk_gutter(pm, HK_GUTTER_HALF, W, 3); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.3f, eaves - 0.022f, 0.17f); hr_led(pm, W - 0.6f); pm_pop(pm);
    float ry, rz;
    si_roof(pm, SI_CLAY, -0.2f, W + 0.2f, eaves + 0.05f, 0.38f, depth, pitch, t, &ry, &rz);
    pm_push(pm); pm_translate(pm, -0.2f, ry, rz); hk_ridge(pm, 0, W + 0.4f); pm_pop(pm);
    /* panels on the street's slope */
    {
        const float a = pitch * PI_F / 180.0f;
        float py, pz;
        slope_pt(eaves + 0.05f, 0.38f, pitch, t, 0.9f, &py, &pz);
        const float org[3] = { 2.4f, py + 0.004f, pz }, ey[3] = { 0.0f, cosf(a), sinf(a) }, ez[3] = { 0.0f, sinf(a), -cosf(a) }, ex[3] = { -1.0f, 0.0f, 0.0f };
        pm_push(pm); pm_frame(pm, org, ex, ey, ez); hr_solar_flush(pm, 3, 2); pm_pop(pm);
    }
    /* the stack and its cross aerial */
    si_box(pm, SI_BRICK, W - 0.55f, ry - 1.5f, rz - 0.3f, W + 0.45f, ry + 1.2f, rz + 0.3f);
    pm_push(pm); pm_translate(pm, W - 0.05f, ry + 1.2f, rz); hk_chimney_top(pm, 1.0f, 0.6f, 2, 9u); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W + 0.35f, ry + 1.23f, rz + 0.23f); pm_rotate_y(pm, 0.4f); pm_place(pm, PMK_R_AERIAL_CROSS); pm_pop(pm);
    /* a tank on a cradle behind the ridge: legs down to the back slope */
    {
        const float a = pitch * PI_F / 180.0f, top = ry - 0.25f, zc = rz - 0.95f, hp = 0.5f * (HR_STAND_W + 0.16f);
        pm_push(pm); pm_translate(pm, 1.6f, top, zc);
        for (int k = 0; k < 4; ++k) {
            const float lx = k & 1 ? hp - 0.06f : -hp + 0.06f, lz = k & 2 ? hp - 0.06f : -hp + 0.06f;
            const float foot = ry - (rz - (zc + lz)) * tanf(a) - t;
            pm_push(pm); pm_translate(pm, lx, foot - top, lz); pm_scale(pm, 0.75f, top - foot, 0.75f); pm_place(pm, PMK_R_FRAME_POST); pm_pop(pm);
        }
        pm_place(pm, PMK_R_STAND_TOP);
        pm_push(pm); pm_translate(pm, 0.0f, HR_STAND_TOP, 0.0f); hr_tank(pm, 0, 0.9f, 0.0f); pm_pop(pm);
        pm_pop(pm);
    }
    /* copper risers up the left corner into the wall under the soffit */
    for (int k = 0; k < 2; ++k) {
        const float x = 0.25f + 0.085f * (float)k, H = eaves - 0.35f;
        const float P[3][3] = { { x, 0.0f, HR_PIPE_Z }, { x, H, HR_PIPE_Z }, { x, H, -0.08f } };
        hr_pipe_path(pm, 0, P, 3);
        if (!k) { const float a[3] = { x, 0.0f, HR_PIPE_Z }, b[3] = { x, H - 0.2f, HR_PIPE_Z }; hr_pipe_clips(pm, 0, a, b); }
    }
    pm_push(pm); pm_translate(pm, 4.45f, pl + 1.95f, 0.0f); pm_place(pm, PMK_R_LANTERN); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 2.85f, 0.45f, 0.0f); pm_place(pm, PMK_R_METER); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 5.35f, 0.0f, 0.0f); hr_vine(pm, 1.0f, 4.6f, HR_VINE_BOUGAINVILLEA); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.9f, 0.0f, 0.25f); pm_scale(pm, 1.5f, 1.0f, 1.0f); pm_place(pm, PMK_R_TROUGH); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 2.95f, 0.0f, 0.4f); pm_place(pm, PMK_R_POT); pm_pop(pm);
}
/* 10. The yard's pieces (the block, compound and inter-war sheets' boundaries, which HM7 sets down): breeze-block
 * screens of both patterns on a low wall, razor wire along a wall's top, a sheet gate, a black tank on a tall stand
 * (the African sheets), a steel tank, a palm in its pot. */
static void vig_retrofit_yard(Pm *pm) {
    si_box(pm, SI_RENDER, 0.0f, 0.0f, -0.2f, 5.6f, 0.6f, 0.0f);
    pm_push(pm); pm_translate(pm, 0.2f, 0.6f, -0.1f); hr_breeze(pm, 0, 2.4f, HR_BREEZE); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 3.0f, 0.6f, -0.1f); hr_breeze(pm, 1, 2.4f, HR_BREEZE); pm_pop(pm);
    si_box(pm, SI_RENDER, 5.6f, 0.0f, -0.2f, 8.2f, 1.9f, 0.0f);
    pm_push(pm); pm_translate(pm, 5.6f, 1.9f, -0.1f); hr_razor(pm, 2.6f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 9.2f, 0.0f, -0.1f); pm_scale(pm, 1.6f, 1.0f, 1.0f); pm_place(pm, PMK_R_GATE_SHEET); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 11.6f, 0.0f, -1.2f); hr_tank(pm, 0, 1.0f, 3.5f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 13.6f, 0.0f, -0.9f); hr_tank(pm, 1, 1.0f, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 8.0f, 0.0f, 1.0f); pm_place(pm, PMK_R_POT_PALM); pm_pop(pm);
}

/* HM5b1a, the other regions' kit (pmodel_region.h), each on its sheet's panel 1 (docs/reference/international-residential). */
/* a module at (x, y, z) stretched sx by sy (an opening's piece: built at its usual size) */
static void put(Pm *pm, int id, float x, float y, float z, float sx, float sy) {
    pm_push(pm); pm_translate(pm, x, y, z); pm_scale(pm, sx, sy, 1.0f); pm_place(pm, id); pm_pop(pm);
}
/* a triangular gable wall stand-in over (x0..x1) at y, rising h, its face at z 0 */
static void si_gable(Pm *pm, int part, float x0, float x1, float y, float h, float t) {
    const float tri[3][2] = { { x0, y }, { x1, y }, { 0.5f * (x0 + x1), y + h } };
    si(pm, part); pm_extrude(pm, tri, 3, -t, 0.0f); si_end(pm);
}
/* 11. Japan (the Japan sheet's panel 1): a narrow two-storey house, a low metal roof; aluminium sliding windows, the
 * ground one frosted, storm-shutter boxes and their rails; a hood over the door; a slat band under the eaves (panel 5). */
static void vig_japan(Pm *pm) {
    const float W = 6.0f, pl = 0.1f, sh = 2.8f, eaves = 5.9f;
    const float o[4][4] = { { 0.6f, pl, 1.5f, pl + 2.1f }, { 2.6f, pl + 0.9f, 4.2f, pl + 2.0f },
                            { 0.6f, pl + sh + 0.8f, 2.2f, pl + sh + 1.9f }, { 3.4f, pl + sh + 0.8f, 5.0f, pl + sh + 1.9f } };
    si_wall(pm, SI_SIDING, 0.0f, 0.0f, W, eaves, 0.25f, o, 4, 0.1f);
    pm_push(pm); pm_translate(pm, 1.05f, pl, 0.0f); hk_door(pm, HK_DOOR_UPVC, HK_CASE_NONE, 0.9f, 2.1f, 0.1f); pm_pop(pm);
    put(pm, PMK_H_HISASHI, 0.35f, pl + 2.35f, 0.0f, 1.4f, 1.0f);
    for (int k = 1; k < 4; ++k) {
        const float cx = 0.5f * (o[k][0] + o[k][2]);
        put(pm, k == 1 ? PMK_H_WIN_SLIDING_FROST : PMK_H_WIN_SLIDING, cx, o[k][1], -0.1f, 1.0f, 1.0f);
        put(pm, PMK_H_AMADO, o[k][2], o[k][1], 0.0f, 1.0f, 1.0f);
        put(pm, PMK_H_AMADO_RAIL, o[k][0], o[k][3], 0.0f, o[k][2] - o[k][0] + HK_AMADO_W, 1.0f);
        put(pm, PMK_H_SILL_CONCRETE, cx, o[k][1], 0.0f, 1.0f, 1.0f);
    }
    put(pm, PMK_H_SLATS, 0.0f, eaves - 0.75f, 0.0f, W, 0.7f);
    si_box(pm, SI_DARK, -0.3f, eaves - 0.02f, 0.0f, W + 0.3f, eaves, 0.5f);
    float ry, rz;
    si_roof(pm, SI_SLATE, -0.3f, W + 0.3f, eaves, 0.5f, 8.0f, 12.0f, 0.08f, &ry, &rz);
    pm_push(pm); pm_translate(pm, -0.3f, eaves - 0.02f, 0.52f); hk_gutter(pm, HK_GUTTER_HALF, W + 0.6f, 3); pm_pop(pm);
    pm_push(pm); pm_translate(pm, W - 0.2f, 0.0f, 0.0f); hk_downpipe(pm, eaves - 0.1f, 0.52f + HK_GUTTER_Z, 0); pm_pop(pm);
}
/* 12. China (the China sheet's panel 1): the courtyard's gatehouse in a grey brick wall: its tiled roof with ridge ends
 * and eave tiles, a red double door with ring knockers between drum stones, couplets and the fu, lanterns from the eaves,
 * a lattice window; behind, a white-tiled block with steel cages out from its windows (panel 2). */
static void vig_china(Pm *pm) {
    const float W = 8.0f, H = 3.2f, x0 = 2.6f, x1 = 5.4f, dx0 = 3.2f, dx1 = 4.8f, dh = 2.6f;
    const float o[3][4] = { { dx0, 0.0f, dx1, dh }, { 0.7f, 1.1f, 0.7f + HK_LAT_W, 1.1f + HK_LAT_H }, { 6.3f, 1.1f, 7.3f, 2.3f } };
    si_wall(pm, SI_BRICK, 0.0f, 0.0f, W, H, 0.4f, o, 3, 0.2f);
    for (int s = 0; s < 2; ++s) {
        const float cx = s ? 4.4f : 3.6f;
        pm_push(pm); pm_translate(pm, cx, 0.12f, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.8f, dh - 0.12f, 0.25f); pm_pop(pm);
        put(pm, PMK_H_KNOCKER, s ? 4.1f : 3.9f, 1.25f, -0.2f, 1.0f, 1.0f);
        put(pm, PMK_H_DRUM_STONE, s ? dx1 + 0.3f : dx0 - 0.3f, 0.0f, 0.3f, 1.0f, 1.0f);
        put(pm, PMK_H_COUPLET, s ? dx1 + 0.3f : dx0 - 0.3f, 0.95f, 0.0f, 1.0f, 1.0f);
        put(pm, PMK_H_LANTERN_PAPER, s ? x1 - 0.25f : x0 + 0.25f, H + 0.05f, 0.45f, 1.0f, 1.0f);
    }
    si_box(pm, SI_DARK, dx0, 0.0f, -0.2f, dx1, 0.12f, 0.25f);   /* the threshold */
    pm_push(pm); pm_translate(pm, 0.5f * (dx0 + dx1) - 0.75f, dh + 0.25f, 0.0f); pm_rotate_z(pm, -0.5f * PI_F); pm_place(pm, PMK_H_COUPLET); pm_pop(pm);
    put(pm, PMK_H_FU, 6.8f, 2.75f, 0.0f, 1.0f, 1.0f);
    put(pm, PMK_H_LATTICE, 0.7f + 0.5f * HK_LAT_W, 1.1f, -0.2f, 1.0f, 1.0f);
    put(pm, PMK_H_GRILLE, 6.8f, 1.05f, 0.0f, 1.1f, 1.3f);
    /* the gatehouse roof: tiles to the front's eaves, ridge ends, eave tiles */
    si_box(pm, SI_BRICK, x0, H, -0.4f, x1, H + 0.3f, 0.0f);
    float ry, rz;
    si_roof(pm, SI_SLATE, x0 - 0.3f, x1 + 0.3f, H + 0.25f, 0.7f, 3.0f, 30.0f, 0.1f, &ry, &rz);
    pm_push(pm); pm_translate(pm, x0 - 0.3f, ry, rz); hk_ridge(pm, 0, x1 - x0 + 0.6f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, x0 - 0.3f, ry, rz); pm_place(pm, PMK_H_RIDGE_END); pm_pop(pm);
    pm_push(pm); pm_translate(pm, x1 + 0.3f, ry, rz); pm_rotate_y(pm, PI_F); pm_place(pm, PMK_H_RIDGE_END); pm_pop(pm);
    pm_push(pm); pm_translate(pm, x0 - 0.3f, H + 0.25f, 0.7f); hk_run(pm, PMK_H_EAVE_TILES, x1 - x0 + 0.6f, 1.0f); pm_pop(pm);
    /* the wall's coping: a tiled ridge along it */
    pm_push(pm); pm_translate(pm, 0.0f, H, -0.2f); hk_ridge(pm, 0, x0); pm_pop(pm);
    pm_push(pm); pm_translate(pm, x1, H, -0.2f); hk_ridge(pm, 0, W - x1); pm_pop(pm);
    /* the block behind: white tile, windows with cages */
    const float bz = -6.0f, bo[4][4] = { { 1.0f, 4.0f, 2.6f, 5.4f }, { 4.4f, 4.0f, 6.0f, 5.4f }, { 1.0f, 7.0f, 2.6f, 8.4f }, { 4.4f, 7.0f, 6.0f, 8.4f } };
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, bz);
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, 7.0f, 9.6f, 0.3f, bo, 4, 0.1f);
    for (int k = 0; k < 4; ++k) {
        HkWindow w = { HK_FRAME_GRID, HK_SILL_CONCRETE, HK_HEAD_NONE, 0, 0, 0, bo[k][2] - bo[k][0], bo[k][3] - bo[k][1], 0.1f };
        pm_push(pm); pm_translate(pm, 0.5f * (bo[k][0] + bo[k][2]), bo[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
        put(pm, PMK_H_CAGE, 0.5f * (bo[k][0] + bo[k][2]), bo[k][1] - 0.15f, 0.0f, bo[k][2] - bo[k][0] + 0.3f, bo[k][3] - bo[k][1] + 0.3f);
    }
    pm_pop(pm);
}
/* 13. Korea (the Korea sheet's panel 1): a brick walk-up villa over pilotis, the columns in hazard sleeves, a bank of
 * mailboxes by the entry; beside it a hanok's wall (panel 4): a fine-grid lattice window under its little tiled roof,
 * Korea's ridge ends and eave tiles. */
static void vig_korea(Pm *pm) {
    const float W = 7.0f, gh = 2.7f, sh = 2.8f, top = gh + 3.0f * sh;
    const float o[6][4] = { { 0.6f, gh + 0.9f, 2.2f, gh + 2.2f }, { 4.4f, gh + 0.9f, 6.0f, gh + 2.2f }, { 0.6f, gh + sh + 0.9f, 2.2f, gh + sh + 2.2f },
                            { 4.4f, gh + sh + 0.9f, 6.0f, gh + sh + 2.2f }, { 0.6f, gh + 2 * sh + 0.9f, 2.2f, gh + 2 * sh + 2.2f }, { 4.4f, gh + 2 * sh + 0.9f, 6.0f, gh + 2 * sh + 2.2f } };
    si_wall(pm, SI_BRICK, 0.0f, gh, W, top - gh, 0.3f, o, 6, 0.1f);
    si_box(pm, SI_DARK, 0.0f, gh - 0.25f, -9.0f, W, gh, 0.0f);   /* the deck over the parking */
    si_box(pm, SI_RENDER, 0.0f, 0.0f, -9.0f, W, gh, -8.7f);
    for (int k = 0; k < 6; ++k) {
        HkWindow w = { HK_FRAME_UPVC, HK_SILL_CONCRETE, HK_HEAD_NONE, 0, 0, 0, o[k][2] - o[k][0], o[k][3] - o[k][1], 0.1f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    for (int k = 0; k < 3; ++k) {
        const float x = k == 0 ? 0.3f : k == 1 ? 3.5f : W - 0.3f;
        si_box(pm, SI_RENDER, x - 0.2f, 0.0f, -0.5f, x + 0.2f, gh - 0.25f, -0.1f);
        pm_push(pm); pm_translate(pm, x, 0.0f, -0.3f); pm_scale(pm, 0.4f, 1.0f, 0.4f); pm_place(pm, PMK_H_HAZARD); pm_pop(pm);
    }
    si_box(pm, SI_RENDER, 4.6f, 0.0f, -3.2f, 6.4f, gh - 0.25f, -3.0f);   /* the entry's wall, the mailboxes on it */
    put(pm, PMK_H_MAILBOX, 5.5f, 1.0f, -3.0f, 1.0f, 1.0f);
    si_box(pm, SI_DARK, -0.1f, top, -0.3f, W + 0.1f, top + 0.9f, 0.0f);   /* the parapet */
    /* the hanok's wall to the left */
    const float hx0 = -6.0f, hx1 = -0.5f, hw = 2.4f;
    const float ho[2][4] = { { hx0 + 1.0f, 0.9f, hx0 + 1.0f + HK_LAT_W, 0.9f + HK_LAT_H }, { hx0 + 3.2f, 0.9f, hx0 + 3.2f + HK_LAT_W, 0.9f + HK_LAT_H } };
    si_wall(pm, SI_SIDING, hx0, 0.0f, hx1 - hx0, hw, 0.25f, ho, 2, 0.15f);
    for (int k = 0; k < 2; ++k) put(pm, PMK_H_LATTICE_KR, 0.5f * (ho[k][0] + ho[k][2]), ho[k][1], -0.15f, 1.0f, 1.0f);
    float ry, rz;
    si_roof(pm, SI_SLATE, hx0 - 0.4f, hx1 + 0.4f, hw, 0.9f, 4.0f, 28.0f, 0.1f, &ry, &rz);
    pm_push(pm); pm_translate(pm, hx0 - 0.4f, ry, rz); hk_ridge(pm, 0, hx1 - hx0 + 0.8f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, hx0 - 0.4f, ry, rz); pm_place(pm, PMK_H_RIDGE_END_KR); pm_pop(pm);
    pm_push(pm); pm_translate(pm, hx1 + 0.4f, ry, rz); pm_rotate_y(pm, PI_F); pm_place(pm, PMK_H_RIDGE_END_KR); pm_pop(pm);
    pm_push(pm); pm_translate(pm, hx0 - 0.4f, hw, 0.9f); hk_run(pm, PMK_H_EAVE_TILES, hx1 - hx0 + 0.8f, 1.0f); pm_pop(pm);
}
/* 14. The shophouse (its sheet's panel 1): the upper storey on round columns over the five-foot way, pilasters between
 * louvred French windows with vent panels over them, the parapet's crest; under it the shop's front with swing half
 * doors before its doors, lanterns; the neighbour's canvas awning. */
static void vig_shophouse(Pm *pm) {
    const float W = 5.0f, fd = 1.9f, gh = 3.0f, uh = 3.3f, pt = gh + uh;
    const float o[3][4] = { { 0.55f, gh + 0.35f, 1.55f, gh + 2.45f }, { 2.0f, gh + 0.35f, 3.0f, gh + 2.45f }, { 3.45f, gh + 0.35f, 4.45f, gh + 2.45f } };
    si_wall(pm, SI_RENDER, 0.0f, gh, W, uh + 0.7f, 0.3f, o, 3, 0.12f);
    si_box(pm, SI_DARK, 0.0f, gh - 0.2f, -fd, W, gh, 0.0f);   /* the five-foot way's ceiling */
    for (int k = 0; k < 3; ++k) {
        const float cx = 0.5f * (o[k][0] + o[k][2]);
        put(pm, PMK_H_WIN_LOUVRE, cx, o[k][1], -0.12f, 1.0f / HK_LOUVRE_W, (o[k][3] - o[k][1]) / HK_LOUVRE_H);
        put(pm, PMK_H_VENT_PANEL, cx, o[k][3] + 0.12f, 0.0f, 1.05f, 1.0f);
        put(pm, PMK_H_SILL_STONE, cx, o[k][1], 0.0f, 1.0f, 1.0f);
    }
    for (int k = 0; k < 4; ++k) put(pm, PMK_H_PILASTER, k == 0 ? 0.2f : k == 3 ? W - 0.2f : 1.775f + 1.45f * (float)(k - 1), gh, 0.0f, 0.8f, uh / HK_COL_H);
    for (int k = 0; k < 2; ++k) put(pm, PMK_H_COLUMN, k ? W - 0.3f : 0.3f, 0.0f, -0.22f, 1.0f, (gh - 0.2f) / HK_COL_H);
    pm_push(pm); pm_translate(pm, 0.0f, gh - 0.05f, 0.0f); hk_run(pm, PMK_H_STRING_COURSE, W, 0.0f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 0.0f, pt + 0.5f, 0.0f); hk_run(pm, PMK_H_BAY_CORNICE, W, 1.0f); pm_pop(pm);
    put(pm, PMK_H_CREST, 0.5f * W, pt + 0.7f, -0.15f, 1.0f, 1.0f);
    /* the shop's front, set back over the five-foot way */
    const float so[3][4] = { { 1.8f, 0.15f, 3.2f, 2.75f }, { 0.4f, 0.9f, 1.4f, 2.2f }, { 3.6f, 0.9f, 4.6f, 2.2f } };
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, -fd);
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, gh - 0.2f, 0.3f, so, 3, 0.15f);
    for (int s = 0; s < 2; ++s) { pm_push(pm); pm_translate(pm, s ? 2.85f : 2.15f, 0.15f, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.7f, 2.6f, 0.15f); pm_pop(pm); }
    put(pm, PMK_H_PINTU_PAGAR, 2.5f, 0.15f, 0.0f, 1.0f, 1.0f);
    for (int k = 1; k < 3; ++k) {
        HkWindow w = { HK_FRAME_CASEMENT, HK_SILL_STONE, HK_HEAD_NONE, 0, 1, 0, 1.0f, 1.3f, 0.15f };
        pm_push(pm); pm_translate(pm, 0.5f * (so[k][0] + so[k][2]), so[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    pm_pop(pm);
    for (int k = 0; k < 2; ++k) put(pm, PMK_H_LANTERN_PAPER, k ? 3.6f : 1.4f, gh - 0.2f, -0.9f, 1.0f, 1.0f);
    /* the neighbour: a plain shop front under a canvas awning */
    si_box(pm, SI_SIDING, W, 0.0f, -0.3f, W + 4.0f, gh + uh, 0.0f);
    pm_push(pm); pm_translate(pm, W + 0.5f, 2.6f, 0.0f); hk_run(pm, PMK_H_AWNING, 3.0f, 1.0f); pm_pop(pm);
}
/* 15. The link house (its sheet's panel 1): a terrace house with a front gable and its vent, gridded windows under hoods,
 * the curved car-porch canopy on two posts over the yard. */
static void vig_linkhouse(Pm *pm) {
    const float W = 6.5f, pl = 0.15f, sh = 3.0f, eaves = 6.2f;
    const float o[4][4] = { { 0.6f, pl, 1.5f, pl + 2.2f }, { 2.4f, pl + 0.9f, 5.6f, pl + 2.3f },
                            { 0.7f, pl + sh + 0.9f, 2.9f, pl + sh + 2.3f }, { 3.6f, pl + sh + 0.9f, 5.8f, pl + sh + 2.3f } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, eaves, 0.25f, o, 4, 0.1f);
    pm_push(pm); pm_translate(pm, 1.05f, pl, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.9f, 2.2f, 0.1f); pm_pop(pm);
    for (int k = 1; k < 4; ++k) {
        const float cx = 0.5f * (o[k][0] + o[k][2]), w = o[k][2] - o[k][0];
        HkWindow hw = { HK_FRAME_GRID, HK_SILL_CONCRETE, HK_HEAD_NONE, 0, k == 1 ? 1 : 0, 0, w, o[k][3] - o[k][1], 0.1f };
        pm_push(pm); pm_translate(pm, cx, o[k][1], 0.0f); hk_window(pm, &hw); pm_pop(pm);
        if (k > 1) put(pm, PMK_H_WIN_HOOD, o[k][0] - 0.1f, o[k][3] + 0.25f, 0.0f, w + 0.2f, 1.0f);
    }
    si_gable(pm, SI_RENDER, 1.2f, 5.3f, eaves, 1.9f, 0.25f);
    put(pm, PMK_H_GABLE_VENT, 3.25f, eaves + 0.75f, 0.0f, 1.0f, 1.0f);
    float ry, rz;
    si_box(pm, SI_DARK, -0.3f, eaves - 0.02f, 0.0f, W + 0.3f, eaves, 0.5f);
    si_roof(pm, SI_CLAY, -0.3f, W + 0.3f, eaves, 0.5f, 9.0f, 30.0f, 0.1f, &ry, &rz);
    pm_push(pm); pm_translate(pm, -0.3f, ry, rz); hk_ridge(pm, 0, W + 0.6f); pm_pop(pm);
    /* the car porch: its canopy from the wall over the yard, two posts at its front */
    pm_push(pm); pm_translate(pm, 0.0f, 2.75f, 0.0f); hk_run(pm, PMK_H_CARPORT, W, 1.6f); pm_pop(pm);
    for (int k = 0; k < 2; ++k) put(pm, PMK_H_POST_SQUARE, k ? W - 0.15f : 0.15f, 0.0f, HK_CARPORT_D - 0.1f, 1.0f, (2.75f + HK_CARPORT_RISE * sinf(0.8f * PI_F) - 0.14f) / HK_POST_H);
}

/* HM5b1b, South Asia, the Middle East and Africa. */
/* a precast balustrade along z from the origin (out from a wall) for len */
static void run_out(Pm *pm, int module, float x, float y, float z, float len) {
    pm_push(pm); pm_translate(pm, x, y, z); pm_rotate_y(pm, -0.5f * PI_F); hk_run(pm, module, len, 1.0f); pm_pop(pm);
}
/* 16. RCC (the RCC sheet's panel 1): a narrow frame house of three storeys over a shop: chajjas over the windows,
 * ornamental grilles over them, a balcony on precast pierced balustrades (panel 4), the shop under a tin awning; the
 * roof's parapet, starter bars out of its columns, a tank. */
static void vig_rcc(Pm *pm) {
    const float W = 6.0f, gh = 3.2f, sh = 3.0f, top = gh + 3.0f * sh;
    float o[8][4];
    int n = 0;
    o[n][0] = 0.6f; o[n][1] = 0.0f; o[n][2] = 4.4f; o[n][3] = 2.6f; n++;   /* the shop */
    o[n][0] = 4.9f; o[n][1] = 0.0f; o[n][2] = 5.8f; o[n][3] = 2.2f; n++;   /* the stair's door */
    for (int s = 0; s < 3; ++s)
        for (int k = 0; k < 2; ++k) { const float x0 = k ? 3.6f : 0.8f, y0 = gh + sh * (float)s + 0.9f; o[n][0] = x0; o[n][1] = y0; o[n][2] = x0 + 1.5f; o[n][3] = y0 + 1.3f; n++; }
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, top, 0.25f, (const float (*)[4])o, n, 0.12f);
    for (int k = 2; k < n; ++k) {
        const float w = o[k][2] - o[k][0], h = o[k][3] - o[k][1], cx = 0.5f * (o[k][0] + o[k][2]);
        HkWindow hw = { HK_FRAME_CASEMENT, HK_SILL_CONCRETE, HK_HEAD_NONE, 0, 0, 0, w, h, 0.12f };
        pm_push(pm); pm_translate(pm, cx, o[k][1], 0.0f); hk_window(pm, &hw); pm_pop(pm);
        put(pm, PMK_H_GRILLE_ORN, cx, o[k][1] - 0.05f, 0.03f, w + 0.1f, h + 0.1f);
        si_box(pm, SI_DARK, o[k][0] - 0.25f, o[k][3] + 0.12f, 0.0f, o[k][2] + 0.25f, o[k][3] + 0.2f, 0.6f);   /* the chajja */
    }
    pm_push(pm); pm_translate(pm, 5.35f, 0.0f, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.9f, 2.2f, 0.12f); pm_pop(pm);
    /* the first floor's balcony */
    si_box(pm, SI_RENDER, 0.3f, gh - 0.15f, 0.0f, 5.7f, gh, 1.1f);
    pm_push(pm); pm_translate(pm, 0.3f, gh, 1.0f); hk_run(pm, PMK_H_BALUSTRADE_PC, 5.4f, 1.0f); pm_pop(pm);
    run_out(pm, PMK_H_BALUSTRADE_PC, 0.4f, gh, 0.05f, 0.9f);
    run_out(pm, PMK_H_BALUSTRADE_PC, 5.6f, gh, 0.05f, 0.9f);
    /* the shop's awning */
    pm_push(pm); pm_translate(pm, 0.4f, 2.85f, 0.0f); hk_run(pm, PMK_H_AWNING_TIN, 4.2f, 1.0f); pm_pop(pm);
    /* the roof: a parapet, the columns' stubs with their bars, a tank */
    si_box(pm, SI_RENDER, 0.0f, top, -0.2f, W, top + 1.0f, 0.0f);
    for (int k = 0; k < 2; ++k) {
        const float x = k ? W - 0.3f : 0.0f;
        si_box(pm, SI_RENDER, x, top, -0.3f, x + 0.3f, top + 1.4f, 0.0f);
        put(pm, PMK_R_REBAR, x + 0.15f, top + 1.4f, -0.15f, 1.0f, 0.9f);
    }
    pm_push(pm); pm_translate(pm, 4.0f, top, -1.8f); hr_tank(pm, 0, 1.0f, 0.0f); pm_pop(pm);
}
/* 17. The bungalow (the Bungalow sheet's panels 1 and 5): a single storey under a hipped clay roof; a deep veranda on
 * round columns, fretwork friezes between them under its flat roof, pierced balustrades but for the steps' bay; the
 * veranda's parapet carries the deco pediment and its roundel (panel 5). */
static void vig_bungalow(Pm *pm) {
    const float W = 10.0f, eh = 4.2f, vd = 2.6f, deck = 0.45f, vr = 3.2f;
    const float o[3][4] = { { 1.0f, 0.9f, 2.4f, 2.7f }, { 4.3f, deck, 5.7f, deck + 2.5f }, { 7.6f, 0.9f, 9.0f, 2.7f } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, eh, 0.3f, o, 3, 0.12f);
    for (int k = 0; k < 3; k += 2) {
        HkWindow hw = { HK_FRAME_CASEMENT, HK_SILL_STONE, HK_HEAD_NONE, 0, 1, 0, o[k][2] - o[k][0], o[k][3] - o[k][1], 0.12f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &hw); pm_pop(pm);
    }
    for (int s = 0; s < 2; ++s) { pm_push(pm); pm_translate(pm, s ? 5.35f : 4.65f, deck, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, 0.7f, 2.5f, 0.12f); pm_pop(pm); }
    si_box(pm, SI_RENDER, -0.2f, 0.0f, 0.0f, W + 0.2f, deck, vd + 0.3f);   /* the veranda's floor */
    si_box(pm, SI_DARK, 3.8f, 0.0f, vd + 0.3f, 6.2f, 0.22f, vd + 0.65f);    /* its step */
    si_box(pm, SI_RENDER, -0.2f, vr, 0.0f, W + 0.2f, vr + 0.25f, vd + 0.3f);   /* its roof */
    si_box(pm, SI_RENDER, -0.2f, vr + 0.25f, vd + 0.1f, W + 0.2f, vr + 0.8f, vd + 0.3f);   /* its parapet */
    put(pm, PMK_H_DECO_ROUNDEL, 0.5f * W, vr + 0.8f, vd + 0.3f, 1.0f, 1.0f);
    const float cx[4] = { 0.3f, 3.5f, 6.5f, 9.7f };
    for (int k = 0; k < 4; ++k) put(pm, PMK_H_COLUMN, cx[k], deck, vd, 1.0f, (vr - deck) / HK_COL_H);
    for (int b = 0; b < 3; ++b) {
        const float x0 = cx[b] + 0.25f, x1 = cx[b + 1] - 0.25f;
        pm_push(pm); pm_translate(pm, x0, vr, vd); hk_run(pm, PMK_H_FRIEZE, x1 - x0, 1.0f); pm_pop(pm);
        if (b != 1) { pm_push(pm); pm_translate(pm, x0, deck, vd); hk_run(pm, PMK_H_BALUSTRADE_PC, x1 - x0, 1.0f); pm_pop(pm); }
    }
    float ry, rz;
    si_roof(pm, SI_CLAY, -0.4f, W + 0.4f, eh, 0.4f, 9.0f, 25.0f, 0.1f, &ry, &rz);
    pm_push(pm); pm_translate(pm, -0.4f, ry, rz); hk_ridge(pm, 0, W + 0.8f); pm_pop(pm);
}
/* 18. The Middle Eastern villa (its sheet's panel 1, with panels 4 and 5's pieces): a two-storey stone block, crenellated;
 * the door under a pointed arch in its carved surround; a mashrabiya oriel (panel 4) and a girih screen over the upper
 * windows; a wind tower on the roof (panel 5). */
static void vig_mena(Pm *pm) {
    const float W = 10.0f, H = 7.2f, cx = 5.0f, apex = hk_arch_apex(0.0f), hw = 0.5f * HK_ARCH_W;
    const float o[5][4] = { { cx - hw, 0.0f, cx + hw, apex }, { 1.4f, 4.3f, 3.0f, 5.9f }, { 7.0f, 3.9f, 9.4f, 6.3f },
                            { 1.2f, 1.0f, 2.8f, 2.4f }, { 7.4f, 1.0f, 9.0f, 2.4f } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, H, 0.3f, o, 5, 0.15f);
    put(pm, PMK_H_ARCH_SURROUND, cx, 0.0f, 0.0f, 1.0f, 1.0f);
    put(pm, PMK_H_DOOR_STUDDED_ARCH, cx, 0.0f, -0.12f, 1.0f, 1.0f);
    for (int k = 3; k < 5; ++k) {
        HkWindow w = { HK_FRAME_GRID, HK_SILL_STONE, HK_HEAD_NONE, 0, 1, 0, o[k][2] - o[k][0], o[k][3] - o[k][1], 0.15f };
        pm_push(pm); pm_translate(pm, 0.5f * (o[k][0] + o[k][2]), o[k][1], 0.0f); hk_window(pm, &w); pm_pop(pm);
    }
    put(pm, PMK_H_MASHRABIYA, 2.2f, 4.0f, 0.0f, 1.0f, 1.0f);
    for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) put(pm, PMK_H_GIRIH, 7.0f + HK_GIRIH * (float)i, 3.9f + HK_GIRIH * (float)j, 0.05f, 1.0f, 1.0f);
    for (int k = 0; k < 11; ++k) { const float x = 0.1f + 0.92f * (float)k; si_box(pm, SI_RENDER, x, H, -0.3f, x + 0.5f, H + 0.6f, 0.0f); }
    si_box(pm, SI_RENDER, 0.0f, H - 0.2f, -8.0f, W, H, -0.3f);   /* the roof */
    put(pm, PMK_H_WIND_TOWER, 7.6f, H, -3.0f, 1.0f, 1.0f);
}
/* 19. The courtyard house (its sheet's panel 1, with panels 2 and 4): a blank crenellated wall, the great studded door
 * under its pointed arch, a square studded door to the side; small windows behind timber lattice; waterspouts through
 * the parapet (panel 4); a mashrabiya (panel 2). */
static void vig_courtyard(Pm *pm) {
    const float W = 13.0f, H = 5.6f, cx = 5.2f, apex = hk_arch_apex(0.0f), hw = 0.5f * HK_ARCH_W;
    const float o[6][4] = { { cx - hw, 0.0f, cx + hw, apex }, { 10.4f, 0.0f, 12.0f, HK_STUD_H }, { 7.6f, 2.0f, 8.4f, 3.0f },
                            { 2.8f, 2.0f, 3.6f, 3.0f }, { 0.9f, 3.9f, 2.9f, 5.1f }, { 8.8f, 3.8f, 9.4f, 4.6f } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, H, 0.4f, o, 6, 0.2f);
    put(pm, PMK_H_ARCH_SURROUND, cx, 0.0f, 0.0f, 1.0f, 1.0f);
    put(pm, PMK_H_DOOR_STUDDED_ARCH, cx, 0.0f, -0.15f, 1.0f, 1.0f);
    put(pm, PMK_H_DOOR_STUDDED, 11.2f, 0.0f, -0.15f, 1.0f, 1.0f);
    pm_push(pm); pm_translate(pm, 10.2f, HK_STUD_H, 0.0f); hk_run(pm, PMK_H_LINTEL_STONE, 2.0f, 0.0f); pm_pop(pm);
    for (int k = 2; k < 6; ++k) {
        if (k == 4) continue;
        const float w = o[k][2] - o[k][0], h = o[k][3] - o[k][1];
        put(pm, PMK_H_GRILLE_LATTICE, 0.5f * (o[k][0] + o[k][2]), o[k][1] - 0.06f, 0.0f, w + 0.12f, h + 0.12f);
    }
    put(pm, PMK_H_MASHRABIYA, 1.9f, 3.6f, 0.0f, 1.0f, 1.0f);
    for (int k = 0; k < 4; ++k) put(pm, PMK_H_WATERSPOUT, 4.0f + 2.6f * (float)k, H - 0.45f, 0.0f, 1.0f, 1.0f);
    for (int k = 0; k < 14; ++k) { const float x = 0.1f + 0.93f * (float)k; si_box(pm, SI_RENDER, x, H, -0.4f, x + 0.5f, H + 0.55f, 0.0f); }
}
/* 20. The block house (the Block sheet's panels 1 and 4): a single storey of rendered block under a hipped sheet roof;
 * glass louvre windows behind bars, a tin awning over them (panel 4), a barred security door over the front door, a
 * breeze-block screen at the side. */
static void vig_block(Pm *pm) {
    const float W = 9.0f, eh = 3.0f, pl = 0.3f, dx = 5.05f;
    const float o[3][4] = { { 0.9f, 1.1f, 0.9f + HK_JAL_W, 1.1f + HK_JAL_H }, { 2.8f, 1.1f, 2.8f + HK_JAL_W, 1.1f + HK_JAL_H },
                            { dx - 0.5f * HK_SEC_W, pl, dx + 0.5f * HK_SEC_W, pl + HK_SEC_H } };
    si_wall(pm, SI_RENDER, 0.0f, 0.0f, W, eh, 0.2f, o, 3, 0.1f);
    si_box(pm, SI_DARK, -0.1f, 0.0f, 0.0f, W + 0.1f, pl, 0.35f);   /* the plinth */
    for (int k = 0; k < 2; ++k) {
        const float cx = 0.5f * (o[k][0] + o[k][2]);
        put(pm, PMK_H_WIN_JALOUSIE, cx, o[k][1], -0.1f, 1.0f, 1.0f);
        put(pm, PMK_H_GRILLE, cx, o[k][1] - 0.05f, 0.0f, HK_JAL_W + 0.1f, HK_JAL_H + 0.1f);
        put(pm, PMK_H_SILL_CONCRETE, o[k][0] - 0.08f, o[k][1], 0.0f, HK_JAL_W + 0.16f, 1.0f);
    }
    pm_push(pm); pm_translate(pm, dx, pl, 0.0f); hk_door(pm, HK_DOOR_4P, HK_CASE_NONE, HK_SEC_W, HK_SEC_H, 0.12f); pm_pop(pm);
    put(pm, PMK_H_DOOR_SECURITY, dx, pl, 0.0f, 1.0f, 1.0f);
    pm_push(pm); pm_translate(pm, 0.5f, 2.6f, 0.0f); hk_run(pm, PMK_H_AWNING_TIN, 3.6f, 1.0f); pm_pop(pm);
    /* the yard's screen wall at the side */
    si_box(pm, SI_RENDER, W, 0.0f, 0.6f, W + 0.2f, 1.7f, 3.0f);
    pm_push(pm); pm_translate(pm, W + 0.2f, 0.4f, 1.8f); pm_rotate_y(pm, -0.5f * PI_F); pm_translate(pm, -1.2f, 0.0f, 0.0f); hr_breeze(pm, 0, 2.4f, HR_BREEZE); pm_pop(pm);
    float ry, rz;
    si_roof(pm, SI_SLATE, -0.4f, W + 0.4f, eh, 0.5f, 8.0f, 18.0f, 0.06f, &ry, &rz);
}
/* 21. The walled compound (its sheet's panel 3): a rendered wall with razor wire over it, piers either side of a pierced
 * gate, CCTV cameras and lanterns on the piers; behind, a two-storey house with an upper veranda on round columns and
 * a pierced balustrade (the columns and balustrade shared). */
static void vig_walled(Pm *pm) {
    const float wh = 2.6f;
    si_box(pm, SI_RENDER, 0.0f, 0.0f, -0.2f, 4.2f, wh, 0.0f);
    si_box(pm, SI_RENDER, 7.8f, 0.0f, -0.2f, 12.0f, wh, 0.0f);
    for (int k = 0; k < 2; ++k) {
        const float x = k ? 7.2f : 4.2f;
        si_box(pm, SI_RENDER, x, 0.0f, -0.35f, x + 0.6f, wh + 0.4f, 0.1f);
        put(pm, PMK_H_CCTV, x + 0.3f, wh + 0.1f, 0.1f, 1.0f, 1.0f);
        put(pm, PMK_R_LANTERN, x + 0.3f, 1.9f, 0.1f, 1.0f, 1.0f);
        pm_push(pm); pm_translate(pm, k ? 6.0f : 4.8f, 0.05f, -0.1f); pm_scale(pm, 1.2f, 2.3f, 1.0f); pm_place(pm, PMK_R_GATE_PIERCED); pm_pop(pm);
    }
    put(pm, PMK_H_CCTV, 11.7f, wh - 0.3f, 0.0f, 1.0f, 1.0f);
    pm_push(pm); pm_translate(pm, 0.0f, wh, -0.1f); hr_razor(pm, 4.2f); pm_pop(pm);
    pm_push(pm); pm_translate(pm, 7.8f, wh, -0.1f); hr_razor(pm, 4.2f); pm_pop(pm);
    /* the house behind */
    const float hz = -6.0f, sh = 3.3f, top = 2.0f * sh;
    const float o[4][4] = { { 1.5f, 0.9f, 3.1f, 2.4f }, { 8.9f, 0.9f, 10.5f, 2.4f }, { 3.5f, sh + 0.2f, 5.3f, sh + 2.6f }, { 6.7f, sh + 0.2f, 8.5f, sh + 2.6f } };
    pm_push(pm); pm_translate(pm, 0.0f, 0.0f, hz);
    si_wall(pm, SI_RENDER, 0.5f, 0.0f, 11.0f, top + 0.4f, 0.3f, o, 4, 0.12f);
    si_box(pm, SI_RENDER, 0.5f, sh - 0.2f, 0.0f, 11.5f, sh, 2.2f);   /* the upper veranda's floor */
    si_box(pm, SI_RENDER, 0.5f, top, 0.0f, 11.5f, top + 0.2f, 2.4f);  /* its roof */
    for (int k = 0; k < 4; ++k) put(pm, PMK_H_COLUMN, 0.8f + 3.4f * (float)k, sh, 2.0f, 0.8f, (top - sh) / HK_COL_H);
    for (int b = 0; b < 3; ++b) { pm_push(pm); pm_translate(pm, 1.05f + 3.4f * (float)b, sh, 2.0f); hk_run(pm, PMK_H_BALUSTRADE_PC, 2.9f, 1.0f); pm_pop(pm); }
    float ry, rz;
    si_roof(pm, SI_CLAY, 0.2f, 11.8f, top + 0.2f, 2.6f, 11.0f, 24.0f, 0.1f, &ry, &rz);
    pm_pop(pm);
    pm_push(pm); pm_translate(pm, 12.6f, 0.0f, -9.0f); hr_tank(pm, 0, 1.0f, 3.5f); pm_pop(pm);
}

typedef void (*VigFn)(Pm *pm);
static const struct { const char *name; VigFn fn; float az, el; } VIG[] = {
    { "victorian", vig_victorian, 0.42f, 0.16f }, { "georgian", vig_georgian, 0.38f, 0.10f }, { "craftsman", vig_craftsman, 0.75f, 0.02f },
    { "interwar", vig_interwar, 0.45f, 0.15f }, { "roofscape", vig_roofscape, 0.55f, 0.42f }, { "postwar", vig_postwar, 0.4f, 0.12f },
    { "shutters", vig_shutters, 0.35f, 0.1f },
    { "retrofit_flat", vig_retrofit_flat, 0.5f, 0.32f }, { "retrofit_pitched", vig_retrofit_pitched, 0.45f, 0.3f },
    { "retrofit_yard", vig_retrofit_yard, 0.3f, 0.22f },
    { "japan", vig_japan, 0.45f, 0.12f }, { "china", vig_china, 0.35f, 0.18f }, { "korea", vig_korea, 0.5f, 0.12f },
    { "shophouse", vig_shophouse, 0.42f, 0.12f }, { "linkhouse", vig_linkhouse, 0.55f, 0.2f },
    { "rcc", vig_rcc, 0.42f, 0.12f }, { "bungalow", vig_bungalow, 0.45f, 0.14f }, { "villa_mena", vig_mena, 0.4f, 0.12f },
    { "courtyard", vig_courtyard, 0.35f, 0.1f }, { "block", vig_block, 0.45f, 0.14f }, { "walled", vig_walled, 0.42f, 0.16f },
};
#define NVIG ((int)(sizeof(VIG) / sizeof(VIG[0])))

/* The house kit: its modules' rows (as the rest of the library) are drawn by main; here the vignettes. */
static void house_section(const char *out, const char *one) {
    static int tris[PMK_COUNT][PM_LODS];
    for (int id = 0; id < PMK_COUNT; ++id)
        for (int l = 0; l < PM_LODS; ++l) { PmMesh m = { 0 }; pm_module_mesh(id, l, &m); tris[id][l] = (int)(m.ni / 3); pm_mesh_free(&m); }
    printf("\nthe house kit's vignettes: instances (budget 150 a house within 60 m), their triangles by level, the stand-ins'\n");
    printf("%-11s %6s %8s %8s %8s %8s\n", "vignette", "inst", "L0", "L1", "L2", "shell");
    for (int v = 0; v < NVIG; ++v) {
        if (one && strcmp(one, VIG[v].name)) continue;
        PmMesh m = { 0 };
        PmInstances li = { 0 };
        Pm pm;
        pm_begin(&pm, &m, &li, PM_LOD0);
        VIG[v].fn(&pm);
        long k[PM_LODS] = { 0, 0, 0 };
        for (uint32_t i = 0; i < li.n; ++i) for (int l = 0; l < PM_LODS; ++l) k[l] += tris[li.v[i].module][l];
        int house = 0;
        for (uint32_t i = 0; i < li.n; ++i) if (pm_module_is_house(li.v[i].module)) house++;
        CHECK(house == (int)li.n, "vignette %s places %d modules that are not the houses'", VIG[v].name, (int)li.n - house);
        CHECK(li.n <= 150, "vignette %s places %u instances (a house's budget is 150)", VIG[v].name, li.n);
        printf("%-11s %6u %8ld %8ld %8ld %8u\n", VIG[v].name, li.n, k[0], k[1], k[2], m.ni / 3);
        pm_mesh_free(&m); pm_inst_free(&li);
        if (!out) continue;
        PmMesh d = { 0 };
        pm_begin(&pm, &d, NULL, PM_LOD0);
        VIG[v].fn(&pm);
        pm_bake(&d, 16, 0.5f);
        const int W = 1100, H = 820;
        Image im = image_new(W, H);
        g_az = VIG[v].az; g_el = VIG[v].el;
        render(&im, &d, 0, 0, W, H, SHOW_LIT);
        g_az = 0.65f; g_el = 0.42f;
        text(&im, 10, 10, VIG[v].name, 3);
        char path[512];
        snprintf(path, sizeof(path), "%s/house_vignette_%s.png", out, VIG[v].name);
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p);
        pm_mesh_free(&d);
    }
}

int main(int argc, char **argv) {
    const char *out = NULL, *only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else out = argv[i];
    }
    const int house_only = only && !strcmp(only, "house");   /* HM4: the house kit's modules and vignettes alone */
    printf("pmodel_lab: %d modules, %d parts\n", pm_module_count(), PART_COUNT);
    printf("%-15s %4s %7s %7s %8s %8s %6s  %s\n", "module", "lod", "tris", "budget", "verts", "KB", "ms", "checks");
    long total = 0;
    static int mod_tris[PMK_COUNT][PM_LODS];
    Image im = { 0, 0, NULL };
    int row = 0, sheet = 0;
    const int nmod = pm_module_count();
    for (int id = 0; id < nmod; ++id) {
        const PmModuleDef *d = pm_module(id);
        if (house_only ? !pm_module_is_house(id) : only && strcmp(only, d->name)) continue;
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
                snprintf(path, sizeof(path), "%s/%s_%d.png", out, house_only ? "house_modules" : "pmodel_modules", sheet++);
                stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
                free(im.p); im.p = NULL; row = 0;
            }
        }
        for (int l = 0; l < PM_LODS; ++l) pm_mesh_free(&lods[l]);
    }
    if (out && im.p) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s_%d.png", out, house_only ? "house_modules" : "pmodel_modules", sheet++);
        im.h = row * CH;
        stbi_write_png(path, im.w, im.h, 3, im.p, im.w * 3);
        free(im.p); im.p = NULL;
    }
    printf("library: %ld triangles over its levels (budget 480000: NB0's 300000 raised for SF1's trades, HM4's house kit, HM5c1's retrofit kit, HM5b1's regional kits)\n", total);
    if (!only) CHECK(total <= 480000, "the library holds %ld triangles over its levels, over 480000", total);
    if (house_only) {
        house_section(out, NULL);
        printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
        return g_fail ? 1 : 0;
    }
    if (!only) {
        /* the slice's cooking line (NB5) at each level: its modules as set down */
        NbPlan p;
        nb_plan(&p, 3.85f, 3.48f, 0.0f, 0x5EED01u, NB_STRAIGHT, VT_NOODLE, 0.5f);
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
    if (!only) house_section(out, NULL);
    if (!only || !strcmp(only, "body")) body_section(out);
    if (!only || !strcmp(only, "body") || !strcmp(only, "line")) line_section(out);
    printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "ok", g_fail);
    return g_fail ? 1 : 0;
}
