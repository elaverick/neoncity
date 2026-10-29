#include "bake.h"
#include "stb_image_write.h"

/* A bake on disk (docs/interior-impostors.md, section 8): one KTX2 2D array
 * per channel, rows bottom up (KTXorientation "ru"), one level, and the JSON
 * sidecar the engine reads (src/render/interior_def.h).
 *
 *   lint    B10G11R11 float   radiance from the module's own lights, path traced (IM3); layer = view * K + depth layer
 *   albedo  RGBA8             base colour; a, the specular share of the exterior light (IM4: 0 until then)
 *   depth   BC5               entry and exit as fractions of D (entry 1: no layer)
 *   flags   R8                one layer per view: 1 where the texel is full (it met more surfaces than K)
 *
 * IM3 and IM4 turn lint into BC6H and albedo into BC7 when their contents
 * are final; the march reads texels, so there are no mips yet. */

#define BAKE_FORMAT_VERSION 2
#define BAKER_VERSION "im3-gi-1"
/* Radiance into the engine's units. Provisional until IM4 calibrates it against the lobby's panels:
 * chosen so a bay's mean through the glass comes near a lit room's (rooms.glsl). IM2's direct light
 * alone took 5; IM3's paths add the light that bounces. */
#define BAKE_SCALE 5.0f

/* A format as KTX2's data format descriptor describes it: a basic block, a sample per channel. */
typedef struct Sample { uint16_t offset; uint8_t bits, channel; uint32_t lower, upper; } Sample;
typedef struct Format {
    uint32_t vk, typeSize;
    uint8_t  model, block, bytes;   /* block: texels a side (1, or 4 for BC); bytes: a texel's or a block's */
    int      nsamples;
    Sample   s[4];
} Format;

enum { DF_RGBSDA = 1, DF_BC5 = 132, DF_FLOAT = 0x80 };
#define ONE_F 0x3F800000u
static const Format F_LINT = { 122, 4, DF_RGBSDA, 1, 4, 3,   /* VK_FORMAT_B10G11R11_UFLOAT_PACK32 */
                               { { 0, 11, 0 | DF_FLOAT, 0, ONE_F }, { 11, 11, 1 | DF_FLOAT, 0, ONE_F }, { 22, 10, 2 | DF_FLOAT, 0, ONE_F } } };
static const Format F_ALBEDO = { 37, 1, DF_RGBSDA, 1, 4, 4,  /* VK_FORMAT_R8G8B8A8_UNORM */
                                 { { 0, 8, 0, 0, 255 }, { 8, 8, 1, 0, 255 }, { 16, 8, 2, 0, 255 }, { 24, 8, 15, 0, 255 } } };
static const Format F_DEPTH = { 141, 1, DF_BC5, 4, 16, 2,    /* VK_FORMAT_BC5_UNORM_BLOCK */
                                { { 0, 64, 0, 0, 0xFFFFFFFFu }, { 64, 64, 1, 0, 0xFFFFFFFFu } } };
static const Format F_FLAGS = { 9, 1, DF_RGBSDA, 1, 1, 1,    /* VK_FORMAT_R8_UNORM */
                                { { 0, 8, 0, 0, 255 } } };

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static uint64_t level_bytes(const Format *f, uint32_t w, uint32_t h, uint32_t layers) {
    return (uint64_t)((w + f->block - 1) / f->block) * ((h + f->block - 1) / f->block) * f->bytes * layers;
}

/* data: the one level, layer after layer. */
static bool write_ktx2(const char *path, const Format *fmt, uint32_t w, uint32_t h, uint32_t layers, const void *data) {
    uint64_t levelBytes = level_bytes(fmt, w, h, layers);
    uint8_t dfd[4 + 24 + 16 * 4];
    memset(dfd, 0, sizeof(dfd));
    uint32_t blockSize = 24 + 16 * (uint32_t)fmt->nsamples, dfdSize = 4 + blockSize;
    put32(dfd, dfdSize);
    uint8_t *b = dfd + 4;
    put32(b, 0);                                       /* vendor Khronos, basic descriptor */
    b[4] = 2; b[5] = 0;                                /* version 2 */
    b[6] = (uint8_t)(blockSize & 0xFF); b[7] = (uint8_t)(blockSize >> 8);
    b[8] = fmt->model;
    b[9] = 1;                                          /* BT.709 primaries */
    b[10] = 1;                                         /* linear */
    b[11] = 0;
    b[12] = b[13] = (uint8_t)(fmt->block - 1);         /* texel block dimensions, less one */
    b[16] = fmt->bytes;                                /* bytesPlane0 */
    for (int c = 0; c < fmt->nsamples; ++c) {
        const Sample *sm = &fmt->s[c];
        uint8_t *s = b + 24 + 16 * c;
        memcpy(s, &sm->offset, 2);
        s[2] = (uint8_t)(sm->bits - 1);
        s[3] = sm->channel;
        put32(s + 8, sm->lower);
        put32(s + 12, sm->upper);
    }
    /* key/values, sorted */
    const char *kv[2][2] = { { "KTXorientation", "ru" }, { "KTXwriter", "NeonCity interior_bake " BAKER_VERSION } };
    uint8_t kvd[256];
    uint32_t kvn = 0;
    for (int i = 0; i < 2; ++i) {
        uint32_t len = (uint32_t)(strlen(kv[i][0]) + 1 + strlen(kv[i][1]) + 1);
        put32(kvd + kvn, len);
        memcpy(kvd + kvn + 4, kv[i][0], strlen(kv[i][0]) + 1);
        memcpy(kvd + kvn + 4 + strlen(kv[i][0]) + 1, kv[i][1], strlen(kv[i][1]) + 1);
        kvn += 4 + len;
        while (kvn & 3) kvd[kvn++] = 0;
    }
    uint8_t head[80 + 24];
    memset(head, 0, sizeof(head));
    static const uint8_t id[12] = { 0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n' };
    memcpy(head, id, 12);
    uint32_t dfdOff = 80 + 24, kvdOff = dfdOff + dfdSize;
    uint64_t dataOff = (kvdOff + kvn + 15) & ~15ull;
    uint32_t fields[9] = { fmt->vk, fmt->typeSize, w, h, 0, layers, 1, 1, 0 };
    for (int i = 0; i < 9; ++i) put32(head + 12 + 4 * i, fields[i]);
    put32(head + 48, dfdOff); put32(head + 52, dfdSize);
    put32(head + 56, kvdOff); put32(head + 60, kvn);
    put64(head + 64, 0); put64(head + 72, 0);
    put64(head + 80, dataOff); put64(head + 88, levelBytes); put64(head + 96, levelBytes);

    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "can't write %s\n", path); return false; }
    fwrite(head, 1, sizeof(head), f);
    fwrite(dfd, 1, dfdSize, f);
    fwrite(kvd, 1, kvn, f);
    static const uint8_t zero[16] = { 0 };
    fwrite(zero, 1, (size_t)(dataOff - (kvdOff + kvn)), f);
    fwrite(data, 1, (size_t)levelBytes, f);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

/* An unsigned float of B10G11R11: a 5-bit exponent and a mant-bit mantissa, from a half, rounded. */
static uint32_t ufloat(float f, int mant) {
    if (!(f > 0.0f)) return 0;   /* negative, zero or NaN */
    int drop = 10 - mant;
    uint32_t v = ((uint32_t)half_from_float(f) + (1u << (drop - 1))) >> drop;
    uint32_t largest = (30u << mant) | ((1u << mant) - 1u);
    return v > largest ? largest : v;
}

bool bake_write(const char *dir, const Scene *s, const Capture *c, const ImpConfig *ic, const TraceConfig *tc) {
    int nx = c->grid.nx, ny = c->grid.ny, views = view_count(&c->views), K = ic->K;
    size_t plane = (size_t)nx * ny, n = (size_t)views * K * plane;
    size_t depthLayer = (size_t)level_bytes(&F_DEPTH, (uint32_t)nx, (uint32_t)ny, 1);
    uint32_t *lint = malloc(n * 4);
    uint8_t *albedo = malloc(n * 4), *depth = malloc(depthLayer * views * K), *flags = malloc((size_t)views * plane);
    float *e = malloc(sizeof(float) * plane), *x = malloc(sizeof(float) * plane);
    if (!lint || !depth || !albedo || !flags || !e || !x) fatal("out of memory writing a bake");
    for (int v = 0; v < views; ++v) {
        for (size_t i = 0; i < plane; ++i) flags[(size_t)v * plane + i] = c->counts[(size_t)v * plane + i] > (uint32_t)K ? 255 : 0;
        for (int l = 0; l < K; ++l) {
            for (size_t i = 0; i < plane; ++i) {
                size_t texel = (size_t)v * plane + i, o = ((size_t)v * K + l) * plane + i;
                int slot = imp_slot(c, ic, texel, l);
                const CapLayer *cl = &c->layers[texel * CAP_SLOTS + (slot < 0 ? 0 : slot)];
                lint[o] = slot < 0 ? 0u : ufloat(cl->lit[0], 6) | ufloat(cl->lit[1], 6) << 11 | ufloat(cl->lit[2], 5) << 22;
                e[i] = slot >= 0 ? cl->depth[0] : 1.0f;
                x[i] = slot >= 0 ? cl->depth[1] : 1.0f;
                uint32_t a = slot >= 0 ? cl->albedo : 0u;
                memcpy(albedo + o * 4, &a, 4);
                albedo[o * 4 + 3] = 0;   /* IM4: the specular share of the exterior light */
            }
            uint8_t *blocks = depth + depthLayer * ((size_t)v * K + l);
            bc4_channel(e, nx, ny, blocks, 16);
            bc4_channel(x, nx, ny, blocks + 8, 16);
        }
    }
    char path[1200];
    bool ok = true;
    snprintf(path, sizeof(path), "%s/%s.lint.ktx2", dir, s->name);
    ok &= write_ktx2(path, &F_LINT, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), lint);
    snprintf(path, sizeof(path), "%s/%s.albedo.ktx2", dir, s->name);
    ok &= write_ktx2(path, &F_ALBEDO, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), albedo);
    snprintf(path, sizeof(path), "%s/%s.depth.ktx2", dir, s->name);
    ok &= write_ktx2(path, &F_DEPTH, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), depth);
    snprintf(path, sizeof(path), "%s/%s.flags.ktx2", dir, s->name);
    ok &= write_ktx2(path, &F_FLAGS, (uint32_t)nx, (uint32_t)ny, (uint32_t)views, flags);
    free(lint); free(depth); free(albedo); free(flags); free(e); free(x);

    /* Per bay, the mean radiance through the opening in the view nearest straight on (the mid LOD). */
    int front = 0;
    float best = -2.0f;
    for (int v = 0; v < views; ++v) {
        float d[3];
        view_dir(&c->views, v, d);
        if (d[2] > best) { best = d[2]; front = v; }
    }
    const float bayW = 3.0f;
    int bays = (int)lroundf(s->W / bayW);
    snprintf(path, sizeof(path), "%s/%s.json", dir, s->name);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "can't write %s\n", path); return false; }
    static const char *types[] = { "through", "core", "corner" };
    const ViewGrid *vg = &c->views;
    fprintf(f, "{\n  // An interior impostor module (docs/interior-impostors.md), written by tools/interior_bake: don't edit.\n");
    fprintf(f, "  \"version\": %d,\n  \"baker\": \"%s\",\n  \"module\": \"%s\",\n", BAKE_FORMAT_VERSION, BAKER_VERSION, s->name);
    fprintf(f, "  \"type\": \"%s\",\n  \"family\": \"office\",\n  \"light_state\": \"%s\",\n", types[s->type], tc ? "im3_gi" : "im1_direct");
    if (tc)
        fprintf(f, "  \"trace\": { \"samples\": %d, \"bounces\": %d, \"clamp\": %.1f, \"seed\": %u },\n",
                tc->spp, tc->bounces, tc->clamp, tc->seed);
    fprintf(f, "  \"size\": [%.3f, %.3f, %.3f],        // W, H, D in metres\n", s->W, s->H, s->D);
    fprintf(f, "  \"ceiling\": %.3f,                     // the slab's underside above the floor (y = 0)\n", s->ceilY);
    fprintf(f, "  \"bays\": %d,\n  \"bay_width\": %.3f,\n  \"opening\": [%.3f, %.3f],     // sill, head\n", bays, bayW, s->sill, s->head);
    static const char *kinds[] = { "azel", "hemioct", "slope" };
    float range[4];
    view_range(vg, range);
    fprintf(f, "  // the view grid as shaders/city/impostor.glsl reads it (ImpViews.range), and each row's elevation in degrees\n");
    fprintf(f, "  \"views\": { \"kind\": \"%s\", \"n\": [%d, %d], \"range\": [%.6f, %.6f, %.6f, %.6f], \"rows\": [",
            kinds[vg->kind], vg->nu, vg->nv, range[0], range[1], range[2], range[3]);
    for (int j = 0; j < vg->nv; ++j) {
        float d[3];
        view_dir(vg, j * vg->nu, d);
        fprintf(f, "%s%.2f", j ? ", " : "", asinf(d[1]) * 57.29578f);
    }
    fprintf(f, "] },\n");
    fprintf(f, "  \"layers\": %d,\n  \"last_slot\": %s,\n", K, ic->lastSlot ? "true" : "false");
    fprintf(f, "  // how the facade marches it (impMarch, IM1's choice)\n");
    fprintf(f, "  \"march\": { \"max_steps\": %d, \"texels_a_step\": %.1f, \"bisections\": %d, \"guess_weight\": %.2f, \"off_grid_weight\": %.2f },\n",
            ic->maxSteps, ic->maxTexStep, ic->refine, ic->unknownW, ic->offGridW);
    fprintf(f, "  \"grid\": { \"origin\": [%.4f, %.4f], \"texels_per_metre\": %.3f, \"size\": [%d, %d], \"wrap_x\": %s },\n",
            c->grid.x0, c->grid.y0, c->grid.tpm, nx, ny, c->grid.wrap ? "true" : "false");
    fprintf(f, "  \"bake_scale\": %.3f,\n  \"bay_means\": {\n    \"lint\": [", BAKE_SCALE);
    for (int b = 0; b < bays; ++b) {
        double sum[3] = { 0, 0, 0 };
        int cnt = 0;
        for (int ty = 0; ty < ny; ++ty) {
            float y = c->grid.y0 + (ty + 0.5f) / c->grid.tpm;
            if (y < s->sill || y > s->head) continue;
            for (int tx = 0; tx < nx; ++tx) {
                float xx = c->grid.x0 + (tx + 0.5f) / c->grid.tpm;
                if (xx < b * bayW || xx >= (b + 1) * bayW) continue;
                size_t texel = (size_t)front * plane + (size_t)ty * nx + tx;
                const CapLayer *cl = &c->layers[texel * CAP_SLOTS];
                for (int k = 0; k < 3; ++k) sum[k] += c->counts[texel] ? cl->lit[k] : s->outside[k];
                cnt++;
            }
        }
        if (!cnt) cnt = 1;
        fprintf(f, "%s[%.5f, %.5f, %.5f]", b ? ", " : "", sum[0] / cnt, sum[1] / cnt, sum[2] / cnt);
    }
    fprintf(f, "]\n  },\n  \"files\": { \"lint\": \"%s.lint.ktx2\", \"albedo\": \"%s.albedo.ktx2\", \"depth\": \"%s.depth.ktx2\", \"flags\": \"%s.flags.ktx2\" }\n}\n",
            s->name, s->name, s->name, s->name);
    ok &= !ferror(f);
    fclose(f);
    printf("wrote %s/%s.{lint,albedo,depth,flags}.ktx2 and .json (%d views x %d layers, %d x %d)\n", dir, s->name, views, K, nx, ny);
    return ok;
}

bool bake_preview(const char *path, const Capture *c, float elevation, float exposure) {
    int front = 0, nx = c->grid.nx, ny = c->grid.ny;
    float best = -2.0f, want[3] = { 0.0f, sinf(elevation), cosf(elevation) };
    for (int v = 0; v < view_count(&c->views); ++v) {
        float d[3];
        view_dir(&c->views, v, d);
        float dot = d[0] * want[0] + d[1] * want[1] + d[2] * want[2];
        if (dot > best) { best = dot; front = v; }
    }
    uint8_t *img = malloc((size_t)nx * ny * 3);
    if (!img) return false;
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            size_t texel = ((size_t)front * ny + (ny - 1 - y)) * nx + x;
            const CapLayer *cl = &c->layers[texel * CAP_SLOTS];
            for (int k = 0; k < 3; ++k) {
                float v = c->counts[texel] ? cl->lit[k] * exposure : 0.0f;
                v = v / (1.0f + v);
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
                img[((size_t)y * nx + x) * 3 + k] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
            }
        }
    bool ok = stbi_write_png(path, nx, ny, 3, img, nx * 3) != 0;
    free(img);
    if (ok) printf("wrote %s\n", path);
    return ok;
}
