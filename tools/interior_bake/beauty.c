#include "bake.h"
#include "stb_image_write.h"

/* The beauty render (docs/interior-impostors.md, section 16): a perspective
 * still from inside a module, framed like the reference, for judging the look.
 * Each light basis is traced alone and composited as the facade shader
 * composites a baked texel, with a fixed set of colours for what lies outside:
 * so a still shows what the module's channels can do, and the passes (raw .pfm
 * beside the PNG, rows bottom up) can be recoloured without tracing again. */

/* What lies outside, in the engine's units: sodium street and lit towers, red-magenta signs, the plum overcast, cool screens. */
static const float LOOK[EXT_BASES][3] = {
    { 0.90f, 0.50f, 0.20f }, { 2.40f, 0.26f, 0.22f }, { 0.050f, 0.030f, 0.045f }, { 0.20f, 0.32f, 0.50f },
};
#define LOOK_EXPOSURE 1.3f   /* composite.frag's default exposure, near enough, with the grade left out */

static float aces(float x) { return fminf(fmaxf(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f), 1.0f); }

static bool write_pfm(const char *path, const float *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "PF\n%d %d\n-1.0\n", w, h);
    for (int i = 0; i < w * h; ++i) fwrite(rgba + i * 4, sizeof(float), 3, f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool beauty_render(Gpu *g, const Scene *s, const TraceConfig *tc, const char *camera, int w, int h, const char *png) {
    float c[6] = { 0, 0, 0, 0, 0, 55.0f };
    int got = sscanf(camera, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]);
    if (got < 5) { fprintf(stderr, "--beauty wants x,y,z,yaw,pitch[,fov]\n"); return false; }
    const float rad = 3.14159265f / 180.0f;
    float yaw = c[3] * rad, pitch = c[4] * rad, th = tanf(c[5] * rad * 0.5f), aspect = (float)w / (float)h;
    float fwd[3] = { sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch) };
    float right[3] = { cosf(yaw) * th * aspect, 0.0f, -sinf(yaw) * th * aspect };
    float up[3] = { -sinf(yaw) * sinf(pitch) * th, cosf(pitch) * th, -cosf(yaw) * sinf(pitch) * th };
    Camera cam = { { c[0], c[1], c[2] }, "beauty", 0 };
    EvalGrid e = { 0.0f, 0.0f, 1.0f, 1.0f, w, h, 1 };
    size_t px = (size_t)w * h;
    float *one = malloc(px * 16), *sum = calloc(px * 3, sizeof(float));
    uint8_t *img = malloc(px * 3);
    if (!one || !sum || !img) fatal("out of memory rendering a still");
    TraceConfig t = *tc;
    t.extPeriod = 96.0f;   /* a skyline that doesn't repeat in the frame; the light it sheds barely changes */
    gpu_set_trace(g, &t);
    gpu_set_perspective(g, fwd, right, up);
    const char *dot = strrchr(png, '.');
    int stem = dot ? (int)(dot - png) : (int)strlen(png);
    static const char *const names[BASIS_COUNT] = { "interior", "street", "neon", "sky", "screens" };
    bool ok = true;
    for (int b = 0; b < BASIS_COUNT; ++b) {
        gpu_set_basis(g, s, b);
        gpu_truth(g, s, &cam, &e, one);
        char path[1200];
        snprintf(path, sizeof(path), "%.*s.%s.pfm", stem, png, names[b]);
        ok &= write_pfm(path, one, w, h);
        for (size_t i = 0; i < px; ++i)
            for (int k = 0; k < 3; ++k)
                sum[i * 3 + k] += one[i * 4 + k] * (b == 0 ? BAKE_SCALE : LOOK[b - 1][k]);
        printf("  %s done\n", names[b]);
    }
    gpu_set_basis(g, s, BASIS_ALL);
    gpu_set_perspective(g, NULL, NULL, NULL);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) {
                float v = aces(sum[((size_t)(h - 1 - y) * w + x) * 3 + k] * LOOK_EXPOSURE);
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                img[((size_t)y * w + x) * 3 + k] = (uint8_t)(v * 255.0f + 0.5f);
            }
    ok &= stbi_write_png(png, w, h, 3, img, w * 3) != 0;
    free(one); free(sum); free(img);
    if (ok) printf("wrote %s\n", png);
    return ok;
}
