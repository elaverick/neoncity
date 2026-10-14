/* stud_pack: converts a texture set of one round object shot on a plain
 * background (a tactile stud) into NeonCity's texture layout, cropped to it.
 *
 *   stud_pack <colour.png> <normal.png> <orm.png> <output dir> [size]
 *
 * The ORM map (R = ambient occlusion, G = roughness, B = height) has the
 * background as (255, 255, 0); everything else is the object. Its bounding
 * square is cropped out of all three maps and box-filtered down to `size`
 * (default 512), so the object's diameter spans the image, and written as:
 *   albedo.png  colour, sRGB; alpha = how much of the texel is object. The
 *               background takes the object's mean colour, so mips don't
 *               bleed white into it.
 *   normal.png  tangent-space normal, OpenGL convention; flat background
 *   orm.png     R = ambient occlusion, G = roughness, B = height; the
 *               background is fully lit, rough and at height 0 */
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

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f; }

static unsigned char to8(float v) {
    v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    return (unsigned char)(v * 255.0f + 0.5f);
}

static unsigned char *load(const char *path, int *w, int *h) {
    int c;
    unsigned char *px = stbi_load(path, w, h, &c, 3);
    if (!px) fprintf(stderr, "cannot read %s\n", path);
    else printf("  %-40s %dx%d\n", path, *w, *h);
    return px;
}

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: stud_pack <colour.png> <normal.png> <orm.png> <output dir> [size]\n");
        return 1;
    }
    const char *outDir = argv[4];
    int size = argc > 5 ? atoi(argv[5]) : 512;
    int w, h, w1, h1, w2, h2;
    unsigned char *col = load(argv[1], &w, &h), *nrm = load(argv[2], &w1, &h1), *orm = load(argv[3], &w2, &h2);
    if (!col || !nrm || !orm) return 1;
    if (w1 != w || h1 != h || w2 != w || h2 != h) { fprintf(stderr, "maps differ in size\n"); return 1; }

    /* the object: wherever the ORM map isn't the background */
    size_t n = (size_t)w * h;
    unsigned char *mask = (unsigned char *)malloc(n);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const unsigned char *o = &orm[((size_t)y * w + x) * 3];
            int on = (255 - o[0]) + (255 - o[1]) + o[2] > 24;
            mask[(size_t)y * w + x] = (unsigned char)on;
            if (on) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    if (x1 < 0) { fprintf(stderr, "no object found in the ORM map\n"); return 1; }
    double cx = (x0 + x1 + 1) * 0.5, cy = (y0 + y1 + 1) * 0.5;
    double side = (x1 - x0 + 1) > (y1 - y0 + 1) ? (x1 - x0 + 1) : (y1 - y0 + 1);
    printf("object %d..%d x %d..%d, %.0f px across\n", x0, x1, y0, y1, side);
    if (size <= 0 || size > side) { fprintf(stderr, "size %d must be 1..%.0f\n", size, side); return 1; }

    /* the object's mean colour (linear), for the background */
    double mean[3] = { 0, 0, 0 }, cnt = 0.0;
    for (size_t i = 0; i < n; ++i)
        if (mask[i]) { for (int k = 0; k < 3; ++k) mean[k] += srgb_to_linear(col[i * 3 + k] / 255.0f); cnt += 1.0; }
    for (int k = 0; k < 3; ++k) mean[k] /= cnt;

    /* box filter: each output texel averages the source texels whose centres fall in its square */
    unsigned char *A = (unsigned char *)malloc((size_t)size * size * 4);
    unsigned char *N = (unsigned char *)malloc((size_t)size * size * 3);
    unsigned char *O = (unsigned char *)malloc((size_t)size * size * 3);
    double scale = side / size, sx0 = cx - side * 0.5, sy0 = cy - side * 0.5;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int ax = (int)ceil(sx0 + x * scale - 0.5), bx = (int)ceil(sx0 + (x + 1) * scale - 0.5);
            int ay = (int)ceil(sy0 + y * scale - 0.5), by = (int)ceil(sy0 + (y + 1) * scale - 0.5);
            double c[3] = { 0, 0, 0 }, nv[3] = { 0, 0, 0 }, o[3] = { 0, 0, 0 }, cov = 0.0, tot = 0.0;
            for (int sy = ay; sy < by; ++sy)
                for (int sx = ax; sx < bx; ++sx) {
                    tot += 1.0;
                    if (sx < 0 || sy < 0 || sx >= w || sy >= h || !mask[(size_t)sy * w + sx]) {
                        nv[2] += 1.0; o[0] += 1.0; o[1] += 1.0;   /* background: flat, lit, rough, height 0 */
                        continue;
                    }
                    size_t i = ((size_t)sy * w + sx) * 3;
                    cov += 1.0;
                    for (int k = 0; k < 3; ++k) {
                        c[k] += srgb_to_linear(col[i + k] / 255.0f);
                        nv[k] += nrm[i + k] / 255.0 * 2.0 - 1.0;
                        o[k] += orm[i + k] / 255.0;
                    }
                }
            size_t q = (size_t)y * size + x;
            for (int k = 0; k < 3; ++k) {
                double lin = cov > 0.0 ? c[k] / cov : mean[k];
                A[q * 4 + k] = to8(linear_to_srgb((float)lin));
                O[q * 3 + k] = to8((float)(o[k] / tot));
            }
            A[q * 4 + 3] = to8((float)(cov / tot));
            double l = sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            if (l < 1e-6) { nv[0] = nv[1] = 0.0; nv[2] = l = 1.0; }
            for (int k = 0; k < 3; ++k) N[q * 3 + k] = to8((float)(nv[k] / l * 0.5 + 0.5));
        }

    make_dir(outDir);
    char path[1024];
    snprintf(path, sizeof(path), "%s/albedo.png", outDir);
    if (!stbi_write_png(path, size, size, 4, A, size * 4)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    printf("  wrote %s\n", path);
    snprintf(path, sizeof(path), "%s/normal.png", outDir);
    if (!stbi_write_png(path, size, size, 3, N, size * 3)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    printf("  wrote %s\n", path);
    snprintf(path, sizeof(path), "%s/orm.png", outDir);
    if (!stbi_write_png(path, size, size, 3, O, size * 3)) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    printf("  wrote %s\n", path);
    printf("%dx%d, mean linear albedo %.3f\n", size, size, 0.2126 * mean[0] + 0.7152 * mean[1] + 0.0722 * mean[2]);
    return 0;
}
