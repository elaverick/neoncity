#include "bake.h"
#include "stb_image_write.h"

/* The path tracer's tests (ctest bake_test: interior_bake --test tests/data;
 * docs/interior-impostors.md, section 15). Each is small and runs in seconds.
 *
 *   furnace      A sealed room whose walls all emit Le and reflect: in a
 *                Lambert room of albedo a every point's radiance is
 *                Le / (1 - a), and for any surface, next-event estimation
 *                with MIS and BSDF sampling alone must agree. Within 1%.
 *   cornell      A Cornell box with a metal block, a clearcoated block and a
 *                pane of glass, against the reference in tests/data
 *                (cornell_ref.pfm, rendered by --write-ref at 16 times the
 *                samples): its mean within 1%, and every 8 x 8 block of
 *                pixels within the noise.
 *   determinism  The same capture and truth twice: the same bytes. Another
 *                seed: other bytes. */

#define CORNELL_RES  96
#define CORNELL_SPP  1024
#define CORNELL_REF_SPP (16 * CORNELL_SPP)
#define BLOCK 8

static int failures;

static void check(bool ok, const char *what) {
    printf("  %s  %s\n", ok ? "pass" : "FAIL", what);
    if (!ok) failures++;
}

static double image_mean(const float *rgba, int n) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) sum += (rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2]) / 3.0;
    return sum / n;
}

/* ------------------------------------------------------------------ scenes --- */

/* A sealed room, inside [-1, 1]^3, every wall of one material emitting Le. */
static void furnace(Scene *s, float albedo, float rough, float metal, float f0, float coat, float le) {
    scene_begin(s, "furnace");
    s->W = 2.0f; s->H = 2.0f; s->D = 10.0f; s->ceilY = 2.0f;
    s->sealed = 1;
    float a[3] = { albedo, albedo, albedo }, e[3] = { le, le, le };
    int m = scene_material(s, a, e, rough, metal, f0, coat, 0.1f);
    const float r = 1.0f, t = 1.2f;
    scene_box(s, m, 0, -t, -t, -t, -r, t, t);
    scene_box(s, m, 0, r, -t, -t, t, t, t);
    scene_box(s, m, 0, -t, -t, -t, t, -r, t);
    scene_box(s, m, 0, -t, r, -t, t, t, t);
    scene_box(s, m, 0, -t, -t, -t, t, t, -r);
    scene_box(s, m, 0, -t, -t, r, t, t, t);
}

/* A 1 m Cornell box open towards -z, with a metal block, a clearcoated block and a pane of glass. */
static void cornell(Scene *s) {
    scene_begin(s, "cornell");
    s->W = 1.0f; s->H = 1.0f; s->D = 1.2f; s->ceilY = 1.0f;
    const float black[3] = { 0, 0, 0 }, white[3] = { 0.73f, 0.73f, 0.73f }, red[3] = { 0.63f, 0.065f, 0.05f };
    const float green[3] = { 0.14f, 0.45f, 0.091f }, steel[3] = { 0.9f, 0.85f, 0.8f }, light[3] = { 17.0f, 12.0f, 4.0f };
    int w = scene_material(s, white, black, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    int r = scene_material(s, red, black, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    int g = scene_material(s, green, black, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    int metal = scene_material(s, steel, black, 0.3f, 1.0f, 0.04f, 0.0f, 0.0f);
    int coated = scene_material(s, white, black, 0.6f, 0.0f, 0.04f, 1.0f, 0.05f);
    int lamp = scene_material(s, white, light, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
    scene_box(s, w, 0, -0.05f, -0.05f, 0.0f, 1.05f, 0.0f, 1.05f);    /* floor */
    scene_box(s, w, 0, -0.05f, 1.0f, 0.0f, 1.05f, 1.05f, 1.05f);     /* ceiling */
    scene_box(s, w, 0, -0.05f, -0.05f, 1.0f, 1.05f, 1.05f, 1.05f);   /* back */
    scene_box(s, r, 0, -0.05f, -0.05f, 0.0f, 0.0f, 1.05f, 1.05f);    /* left */
    scene_box(s, g, 0, 1.0f, -0.05f, 0.0f, 1.05f, 1.05f, 1.05f);     /* right */
    scene_box(s, lamp, BF_EMITTER, 0.4f, 0.99f, 0.4f, 0.6f, 1.01f, 0.6f);
    scene_box(s, metal, 0, 0.55f, 0.0f, 0.5f, 0.85f, 0.6f, 0.8f);
    scene_box(s, coated, 0, 0.15f, 0.0f, 0.2f, 0.45f, 0.3f, 0.5f);
    scene_box(s, w, BF_GLASS, 0.1f, 0.3f, 0.1f, 0.5f, 0.7f, 0.112f);
}

/* --------------------------------------------------------------- the tests --- */

static TraceConfig test_trace(int spp, int bounces, int nee) {
    TraceConfig t = trace_defaults(1);
    t.spp = spp;
    t.batch = 64;
    t.bounces = bounces;
    t.clamp = 0.0f;
    t.nee = nee;
    return t;
}

static double furnace_mean(Gpu *g, const Scene *s, int nee) {
    TraceConfig t = test_trace(512, 64, nee);
    gpu_set_trace(g, &t);
    Camera c = { { 0.0f, 0.0f, -0.5f }, "inside", 0 };
    EvalGrid e = { -0.25f, -0.25f, 64.0f, 64.0f, 32, 32, 1 };
    float *rgba = malloc(sizeof(float) * 4 * e.w * e.h);
    if (!rgba) fatal("out of memory");
    gpu_truth(g, s, &c, &e, rgba);
    double m = image_mean(rgba, e.w * e.h);
    free(rgba);
    return m;
}

static void test_furnace(Gpu *g) {
    printf("furnace\n");
    char what[160];
    Scene s;
    furnace(&s, 0.8f, 1.0f, 0.0f, 0.0f, 0.0f, 0.2f);   /* Lambert: Le / (1 - a) = 1 */
    gpu_load_scene(g, &s);
    double mis = furnace_mean(g, &s, 1), bsdf = furnace_mean(g, &s, 0);
    snprintf(what, sizeof(what), "Lambert 0.8, emitting 0.2: %.4f with MIS, %.4f by BSDF sampling (1 expected)", mis, bsdf);
    check(fabs(mis - 1.0) < 0.01 && fabs(bsdf - 1.0) < 0.01, what);
    scene_free(&s);

    static const struct { const char *name; float albedo, rough, metal, f0, coat; } M[] = {
        { "glossy dielectric under a clearcoat", 0.5f, 0.4f, 0.0f, 0.04f, 1.0f },
        { "rough metal", 0.9f, 0.5f, 1.0f, 0.04f, 0.0f },
        { "near-mirror metal", 0.7f, 0.05f, 1.0f, 0.04f, 0.0f },
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); ++i) {
        furnace(&s, M[i].albedo, M[i].rough, M[i].metal, M[i].f0, M[i].coat, 0.2f);
        gpu_load_scene(g, &s);
        mis = furnace_mean(g, &s, 1);
        bsdf = furnace_mean(g, &s, 0);
        snprintf(what, sizeof(what), "%s: %.4f with MIS, %.4f by BSDF sampling (equal)", M[i].name, mis, bsdf);
        check(fabs(mis - bsdf) < 0.01 * bsdf, what);
        scene_free(&s);
    }
}

static float *render_cornell(Gpu *g, const Scene *s, int spp, uint32_t seed) {
    TraceConfig t = test_trace(spp, 16, 1);
    t.seed = seed;
    gpu_set_trace(g, &t);
    Camera c = { { 0.5f, 0.5f, -1.4f }, "front", 0 };
    EvalGrid e = { 0.0f, 0.0f, (float)CORNELL_RES, (float)CORNELL_RES, CORNELL_RES, CORNELL_RES, 1 };
    float *rgba = malloc(sizeof(float) * 4 * CORNELL_RES * CORNELL_RES);
    if (!rgba) fatal("out of memory");
    gpu_truth(g, s, &c, &e, rgba);
    return rgba;
}

/* PFM: RGB floats, rows bottom up (as the truth's). */
static bool write_pfm(const char *path, const float *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
    for (int i = 0; i < w * h; ++i) fwrite(rgba + i * 4, sizeof(float), 3, f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static float *read_pfm(const char *path, int w, int h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int fw = 0, fh = 0;
    float scale = 0.0f;
    float *rgba = NULL;
    if (fscanf(f, "PF %d %d %f", &fw, &fh, &scale) == 3 && fw == w && fh == h && scale < 0.0f && fgetc(f) == '\n') {
        rgba = calloc((size_t)w * h * 4, sizeof(float));
        for (int i = 0; rgba && i < w * h; ++i)
            if (fread(rgba + i * 4, sizeof(float), 3, f) != 3) { free(rgba); rgba = NULL; }
    }
    fclose(f);
    return rgba;
}

static void write_png(const char *path, const float *rgba, int w, int h) {
    uint8_t *img = malloc((size_t)w * h * 3);
    if (!img) return;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) {
                float v = rgba[((size_t)(h - 1 - y) * w + x) * 4 + k];
                v = v / (1.0f + v);
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                img[((size_t)y * w + x) * 3 + k] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
            }
    stbi_write_png(path, w, h, 3, img, w * 3);
    free(img);
}

static void test_cornell(Gpu *g, const char *dataDir, bool writeRef) {
    printf("cornell\n");
    char path[1200], what[200];
    Scene s;
    cornell(&s);
    gpu_load_scene(g, &s);
    snprintf(path, sizeof(path), "%s/cornell_ref.pfm", dataDir);
    if (writeRef) {
        float *ref = render_cornell(g, &s, CORNELL_REF_SPP, 1u);
        check(write_pfm(path, ref, CORNELL_RES, CORNELL_RES), "wrote the reference");
        snprintf(path, sizeof(path), "%s/cornell_ref.png", dataDir);
        write_png(path, ref, CORNELL_RES, CORNELL_RES);
        free(ref);
        scene_free(&s);
        return;
    }
    float *ref = read_pfm(path, CORNELL_RES, CORNELL_RES);
    if (!ref) { check(false, "read the reference cornell_ref.pfm"); scene_free(&s); return; }
    float *img = render_cornell(g, &s, CORNELL_SPP, 7u);
    int n = CORNELL_RES * CORNELL_RES;
    double mi = image_mean(img, n), mr = image_mean(ref, n);
    snprintf(what, sizeof(what), "mean %.4f against the reference's %.4f", mi, mr);
    check(fabs(mi - mr) < 0.01 * mr, what);
    /* each block's mean within 5% of the reference's, or of a floor for the darkest */
    double worst = 0.0;
    int wx = 0, wy = 0, nb = CORNELL_RES / BLOCK;
    for (int by = 0; by < nb; ++by)
        for (int bx = 0; bx < nb; ++bx) {
            double a = 0.0, b = 0.0;
            for (int y = by * BLOCK; y < (by + 1) * BLOCK; ++y)
                for (int x = bx * BLOCK; x < (bx + 1) * BLOCK; ++x)
                    for (int k = 0; k < 3; ++k) {
                        a += img[((size_t)y * CORNELL_RES + x) * 4 + k];
                        b += ref[((size_t)y * CORNELL_RES + x) * 4 + k];
                    }
            a /= BLOCK * BLOCK * 3; b /= BLOCK * BLOCK * 3;
            double err = fabs(a - b) / (b + 0.05 * mr);
            if (err > worst) { worst = err; wx = bx; wy = by; }
        }
    snprintf(what, sizeof(what), "every 8 x 8 block within 5%% of the reference (the worst, %d %d: %.1f%%)", wx, wy, worst * 100.0);
    check(worst < 0.05, what);
    free(img);
    free(ref);
    scene_free(&s);
}

static void test_determinism(Gpu *g) {
    printf("determinism\n");
    Scene s;
    cornell(&s);
    gpu_load_scene(g, &s);
    float *a = render_cornell(g, &s, 64, 3u), *b = render_cornell(g, &s, 64, 3u), *c = render_cornell(g, &s, 64, 4u);
    size_t bytes = sizeof(float) * 4 * CORNELL_RES * CORNELL_RES;
    check(!memcmp(a, b, bytes), "the truth: same seed, same bytes");
    check(memcmp(a, c, bytes) != 0, "the truth: another seed, other bytes");
    free(a); free(b); free(c);

    ViewGrid v = { 2, 2, GRID_AZEL, 0.5f, -0.4f, 0.4f, 0.0f, 0.0f };
    TexGrid t = { 0.0f, 0.0f, 32.0f, 32, 32, 0 };
    TraceConfig tc = test_trace(32, 8, 1);
    tc.batch = 8;
    tc.slots = 2;
    gpu_set_trace(g, &tc);
    Capture c1, c2;
    gpu_capture(g, &s, &v, &t, &c1);
    gpu_capture(g, &s, &v, &t, &c2);
    size_t texels = (size_t)view_count(&v) * t.nx * t.ny;
    bool lit = false;
    for (size_t i = 0; i < texels * CAP_SLOTS && !lit; ++i) lit = c1.layers[i].lit[0] > 0.0f;
    check(lit && !memcmp(c1.layers, c2.layers, texels * CAP_SLOTS * sizeof(CapLayer)), "a capture: same seed, same bytes");
    free(c1.layers); free(c1.counts); free(c2.layers); free(c2.counts);
    scene_free(&s);
}

int test_run(Gpu *g, const char *dataDir, bool writeRef) {
    printf("the path tracer's tests on %s\n", gpu_name(g));
    if (writeRef) {
        test_cornell(g, dataDir, true);
    } else {
        test_furnace(g);
        test_cornell(g, dataDir, false);
        test_determinism(g);
    }
    gpu_set_trace(g, NULL);
    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
