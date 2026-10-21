/* pbr_pack: converts an ambientCG-style PBR download into NeonCity's texture layout.
 *
 *   pbr_pack <source prefix> <output dir> [size]
 *
 * The prefix is the download's common file stem, e.g.
 *   Downloads/PavingStones/PavingStones111_4K-PNG
 * from which <prefix>_Color.png, _NormalGL.png and _Roughness.png are read, and
 * _AmbientOcclusion.png, _Displacement.png and _Opacity.png if present (8- or
 * 16-bit PNG).
 * Maps are box-filtered down to `size` (default 2048; the source must be an
 * integer multiple of it) and written to the output directory as:
 *   albedo.png  colour, sRGB; alpha the opacity (RGB without an opacity map)
 *   normal.png  tangent-space normal, OpenGL convention (+Y green = image up)
 *   orm.png     R = ambient occlusion, G = roughness, B = height
 * Nothing else in the download (.blend, .mtlx, .tres, .usdc, NormalDX,
 * Metalness, the preview sphere) is needed. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

typedef struct Map { int w, h, c; float *px; } Map;

/* Loads <prefix>_<suffix>.png as `channels` floats in [0,1]; returns false if absent. */
static int load_map(const char *prefix, const char *suffix, int channels, Map *m) {
    char path[1024];
    snprintf(path, sizeof(path), "%s_%s.png", prefix, suffix);
    int w, h, c;
    size_t n;
    if (stbi_is_16_bit(path)) {
        stbi_us *src = stbi_load_16(path, &w, &h, &c, channels);
        if (!src) return 0;
        n = (size_t)w * h * channels;
        m->px = (float *)malloc(n * sizeof(float));
        for (size_t i = 0; i < n; ++i) m->px[i] = src[i] / 65535.0f;
        stbi_image_free(src);
    } else {
        stbi_uc *src = stbi_load(path, &w, &h, &c, channels);
        if (!src) return 0;
        n = (size_t)w * h * channels;
        m->px = (float *)malloc(n * sizeof(float));
        for (size_t i = 0; i < n; ++i) m->px[i] = src[i] / 255.0f;
        stbi_image_free(src);
    }
    m->w = w; m->h = h; m->c = channels;
    printf("  %-18s %dx%d\n", suffix, w, h);
    return 1;
}

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

/* Box-filters m down by an integer factor so its width becomes `size`. */
static void downsample(Map *m, int size) {
    int f = m->w / size;
    if (f <= 1) return;
    int w = m->w / f, h = m->h / f, c = m->c;
    float *out = (float *)calloc((size_t)w * h * c, sizeof(float));
    float inv = 1.0f / (float)(f * f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < c; ++k) {
                float s = 0.0f;
                for (int dy = 0; dy < f; ++dy)
                    for (int dx = 0; dx < f; ++dx)
                        s += m->px[((size_t)(y * f + dy) * m->w + (x * f + dx)) * c + k];
                out[((size_t)y * w + x) * c + k] = s * inv;
            }
    free(m->px);
    m->px = out; m->w = w; m->h = h;
}

static unsigned char to8(float v) {
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static int write_png(const char *dir, const char *name, int w, int h, int c, const unsigned char *px) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (!stbi_write_png(path, w, h, c, px, w * c)) { fprintf(stderr, "cannot write %s\n", path); return 0; }
    printf("  wrote %s\n", path);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: pbr_pack <source prefix> <output dir> [size]\n"
                        "  e.g. pbr_pack Downloads/PavingStones/PavingStones111_4K-PNG assets/textures/paving_stones_111 2048\n");
        return 1;
    }
    const char *prefix = argv[1], *outDir = argv[2];
    int size = argc > 3 ? atoi(argv[3]) : 2048;

    Map color = { 0 }, normal = { 0 }, rough = { 0 }, ao = { 0 }, height = { 0 }, opacity = { 0 };
    printf("Reading %s_*\n", prefix);
    if (!load_map(prefix, "Color", 3, &color) || !load_map(prefix, "NormalGL", 3, &normal) ||
        !load_map(prefix, "Roughness", 1, &rough)) {
        fprintf(stderr, "missing Color, NormalGL or Roughness map\n");
        return 1;
    }
    int haveAO = load_map(prefix, "AmbientOcclusion", 1, &ao);
    int haveHeight = load_map(prefix, "Displacement", 1, &height);
    int haveOpacity = load_map(prefix, "Opacity", 1, &opacity);

    Map *all[6] = { &color, &normal, &rough, haveAO ? &ao : NULL, haveHeight ? &height : NULL, haveOpacity ? &opacity : NULL };
    for (int i = 0; i < 6; ++i) {
        if (!all[i]) continue;
        if (all[i]->w != color.w || all[i]->h != color.h) { fprintf(stderr, "maps differ in size\n"); return 1; }
    }
    if (size <= 0 || size > color.w || color.w % size || color.h % (color.w / size)) {
        fprintf(stderr, "size %d must divide the source width %d (and its height %d) evenly\n", size, color.w, color.h);
        return 1;
    }

    /* Filter colour in linear light and normals as vectors. */
    size_t n = (size_t)color.w * color.h;
    for (size_t i = 0; i < n * 3; ++i) color.px[i] = srgb_to_linear(color.px[i]);
    for (size_t i = 0; i < n * 3; ++i) normal.px[i] = normal.px[i] * 2.0f - 1.0f;
    for (int i = 0; i < 6; ++i) if (all[i]) downsample(all[i], size);

    int w = color.w, h = color.h;
    n = (size_t)w * h;
    unsigned char *px = (unsigned char *)malloc(n * 4);
    double meanAlb = 0.0, meanRough = 0.0, cover = 0.0;

    /* With an opacity map the mean albedo is over what is there, weighted by its opacity. */
    make_dir(outDir);
    int ca = haveOpacity ? 4 : 3;
    for (size_t i = 0; i < n; ++i) {
        const float *c = &color.px[i * 3];
        float a = haveOpacity ? opacity.px[i] : 1.0f;
        meanAlb += a * (0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]);
        cover += a;
        for (int k = 0; k < 3; ++k) px[i * ca + k] = to8(linear_to_srgb(c[k]));
        if (haveOpacity) px[i * ca + 3] = to8(a);
    }
    if (!write_png(outDir, "albedo.png", w, h, ca, px)) return 1;

    for (size_t i = 0; i < n; ++i) {
        float *v = &normal.px[i * 3];
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-6f) { v[0] = v[1] = 0.0f; v[2] = l = 1.0f; }
        for (int k = 0; k < 3; ++k) px[i * 3 + k] = to8(v[k] / l * 0.5f + 0.5f);
    }
    if (!write_png(outDir, "normal.png", w, h, 3, px)) return 1;

    for (size_t i = 0; i < n; ++i) {
        meanRough += rough.px[i];
        px[i * 3 + 0] = haveAO ? to8(ao.px[i]) : 255;
        px[i * 3 + 1] = to8(rough.px[i]);
        px[i * 3 + 2] = haveHeight ? to8(height.px[i]) : 128;
    }
    if (!write_png(outDir, "orm.png", w, h, 3, px)) return 1;

    printf("%dx%d, mean linear albedo %.3f, mean roughness %.3f%s%s", w, h, meanAlb / (cover > 0.0 ? cover : 1.0), meanRough / n,
           haveAO ? "" : ", no AO (white)", haveHeight ? "" : ", no height (flat)");
    if (haveOpacity) printf(", opacity %.3f", cover / n);
    printf("\n");
    return 0;
}
