#include "bake.h"

/* A bake on disk (docs/interior-impostors.md, section 8): one KTX2 2D array
 * per channel, layer = view * K + depth layer, rows bottom up
 * (KTXorientation "ru"), and the JSON sidecar. IM1 writes the channels
 * uncompressed and with one level; the block formats and mips of section 6
 * come with the engine's reader (IM2).
 *
 *   lint    RGBA16F   radiance from the module's own lights (IM1: direct light only)
 *   albedo  RGBA8     base colour
 *   normal  RGBA8     the surface normal, n * 0.5 + 0.5; a = 1 on layer 0 where the texel is full
 *   depth   RG16      entry and exit as fractions of D (entry 1: no layer)
 */

#define BAKE_FORMAT_VERSION 1
#define BAKER_VERSION "im1-direct-1"

enum { VK_R8G8B8A8_UNORM = 37, VK_R16G16_UNORM = 77, VK_R16G16B16A16_SFLOAT = 97 };

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* channels of bits each (8 or 16), float (half) or unsigned normalised; data: layers * h * w texels. */
static bool write_ktx2(const char *path, uint32_t vkFormat, int channels, int bits, bool isFloat,
                       uint32_t w, uint32_t h, uint32_t layers, const void *data) {
    uint32_t texel = (uint32_t)(channels * bits / 8);
    uint64_t levelBytes = (uint64_t)texel * w * h * layers;
    /* data format descriptor: one basic block, a sample per channel */
    uint8_t dfd[4 + 24 + 16 * 4];
    memset(dfd, 0, sizeof(dfd));
    uint32_t blockSize = 24 + 16 * (uint32_t)channels, dfdSize = 4 + blockSize;
    put32(dfd, dfdSize);
    uint8_t *b = dfd + 4;
    put32(b, 0);                                       /* vendor Khronos, basic descriptor */
    b[4] = 2; b[5] = 0;                                /* version 2 */
    b[6] = (uint8_t)(blockSize & 0xFF); b[7] = (uint8_t)(blockSize >> 8);
    b[8] = 1;                                          /* KHR_DF_MODEL_RGBSDA */
    b[9] = 1;                                          /* BT.709 primaries */
    b[10] = 1;                                         /* linear */
    b[11] = 0;
    b[16] = (uint8_t)texel;                            /* bytesPlane0 */
    static const uint8_t ids[4] = { 0, 1, 2, 15 };
    for (int c = 0; c < channels; ++c) {
        uint8_t *s = b + 24 + 16 * c;
        uint16_t off = (uint16_t)(c * bits);
        memcpy(s, &off, 2);
        s[2] = (uint8_t)(bits - 1);
        s[3] = (uint8_t)(ids[c] | (isFloat ? 0xC0 : 0));   /* float | signed */
        put32(s + 8, isFloat ? 0xBF800000u : 0u);
        put32(s + 12, isFloat ? 0x3F800000u : (bits == 8 ? 255u : 65535u));
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
    uint32_t fields[9] = { vkFormat, (uint32_t)(bits / 8), w, h, 0, layers, 1, 1, 0 };
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

bool bake_write(const char *dir, const Scene *s, const Capture *c, const ImpConfig *ic) {
    int nx = c->grid.nx, ny = c->grid.ny, views = view_count(&c->views), K = ic->K;
    size_t plane = (size_t)nx * ny, n = (size_t)views * K * plane;
    uint16_t *lint = malloc(n * 8), *depth = malloc(n * 4);
    uint8_t *albedo = malloc(n * 4), *normal = malloc(n * 4);
    if (!lint || !depth || !albedo || !normal) fatal("out of memory writing a bake");
    for (int v = 0; v < views; ++v)
        for (int l = 0; l < K; ++l)
            for (size_t i = 0; i < plane; ++i) {
                size_t texel = (size_t)v * plane + i, o = ((size_t)v * K + l) * plane + i;
                int slot = imp_slot(c, ic, texel, l);
                const CapLayer *cl = &c->layers[texel * CAP_SLOTS + (slot < 0 ? 0 : slot)];
                for (int k = 0; k < 4; ++k) lint[o * 4 + k] = half_from_float(slot >= 0 && k < 3 ? cl->lit[k] : 0.0f);
                float e = slot >= 0 ? cl->depth[0] : 1.0f, x = slot >= 0 ? cl->depth[1] : 1.0f;
                depth[o * 2] = (uint16_t)lroundf(fminf(fmaxf(e, 0.0f), 1.0f) * 65535.0f);
                depth[o * 2 + 1] = (uint16_t)lroundf(fminf(fmaxf(x, 0.0f), 1.0f) * 65535.0f);
                uint32_t a = slot >= 0 ? cl->albedo : 0u, nn = slot >= 0 ? cl->normal : 0x00808080u;
                memcpy(albedo + o * 4, &a, 4);
                albedo[o * 4 + 3] = 0;   /* IM4: the specular share of the exterior light */
                memcpy(normal + o * 4, &nn, 4);
                normal[o * 4 + 3] = l == 0 && c->counts[texel] > (uint32_t)K ? 255 : 0;
            }
    char path[1200];
    bool ok = true;
    snprintf(path, sizeof(path), "%s/%s.lint.ktx2", dir, s->name);
    ok &= write_ktx2(path, VK_R16G16B16A16_SFLOAT, 4, 16, true, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), lint);
    snprintf(path, sizeof(path), "%s/%s.albedo.ktx2", dir, s->name);
    ok &= write_ktx2(path, VK_R8G8B8A8_UNORM, 4, 8, false, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), albedo);
    snprintf(path, sizeof(path), "%s/%s.normal.ktx2", dir, s->name);
    ok &= write_ktx2(path, VK_R8G8B8A8_UNORM, 4, 8, false, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), normal);
    snprintf(path, sizeof(path), "%s/%s.depth.ktx2", dir, s->name);
    ok &= write_ktx2(path, VK_R16G16_UNORM, 2, 16, false, (uint32_t)nx, (uint32_t)ny, (uint32_t)(views * K), depth);
    free(lint); free(depth); free(albedo); free(normal);

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
    fprintf(f, "  \"type\": \"%s\",\n  \"family\": \"office\",\n  \"light_state\": \"im1_direct\",\n", types[s->type]);
    fprintf(f, "  \"size\": [%.3f, %.3f, %.3f],        // W, H, D in metres\n", s->W, s->H, s->D);
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
    fprintf(f, "  \"bake_scale\": 1.0,\n  \"bay_means\": {\n    \"lint\": [");
    for (int b = 0; b < bays; ++b) {
        double sum[3] = { 0, 0, 0 };
        int cnt = 0;
        for (int ty = 0; ty < ny; ++ty) {
            float y = c->grid.y0 + (ty + 0.5f) / c->grid.tpm;
            if (y < s->sill || y > s->head) continue;
            for (int tx = 0; tx < nx; ++tx) {
                float x = c->grid.x0 + (tx + 0.5f) / c->grid.tpm;
                if (x < b * bayW || x >= (b + 1) * bayW) continue;
                size_t texel = (size_t)front * plane + (size_t)ty * nx + tx;
                const CapLayer *cl = &c->layers[texel * CAP_SLOTS];
                for (int k = 0; k < 3; ++k) sum[k] += c->counts[texel] ? cl->lit[k] : s->outside[k];
                cnt++;
            }
        }
        if (!cnt) cnt = 1;
        fprintf(f, "%s[%.5f, %.5f, %.5f]", b ? ", " : "", sum[0] / cnt, sum[1] / cnt, sum[2] / cnt);
    }
    fprintf(f, "]\n  },\n  \"files\": { \"lint\": \"%s.lint.ktx2\", \"albedo\": \"%s.albedo.ktx2\", \"normal\": \"%s.normal.ktx2\", \"depth\": \"%s.depth.ktx2\" }\n}\n",
            s->name, s->name, s->name, s->name);
    ok &= !ferror(f);
    fclose(f);
    printf("wrote %s/%s.{lint,albedo,normal,depth}.ktx2 and .json (%d views x %d layers, %d x %d)\n", dir, s->name, views, K, nx, ny);
    return ok;
}
