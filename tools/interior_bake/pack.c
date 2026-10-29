#include "bake.h"

/* An impostor from a capture: which of the captured intervals each layer
 * keeps, and how precisely its depths survive the texture format. */

uint16_t half_from_float(float f) {
    union { float f; uint32_t u; } v = { f };
    uint32_t sign = (v.u >> 16) & 0x8000u, mant = v.u & 0x7FFFFFu;
    int32_t exp = (int32_t)((v.u >> 23) & 0xFF) - 127 + 15;
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);   /* clamp to the largest half */
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        return (uint16_t)(sign | ((mant >> (14 - exp)) + ((mant >> (13 - exp)) & 1u)));
    }
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    return (uint16_t)(h + ((mant >> 12) & 1u));   /* round half up */
}

int imp_slot(const Capture *c, const ImpConfig *ic, size_t texel, int l) {
    int n = (int)c->counts[texel];
    if (ic->lastSlot && l == ic->K - 1 && n > ic->K) return CAP_SLOTS - 1;
    return l < n && l < CAP_SLOTS - 1 ? l : -1;
}

/* BC4 (a BC5 channel) as a simple encoder makes it: per 4 x 4 block,
 * endpoints at the block's extremes quantised to 8 bits, each value snapped
 * to the nearest of the 8 levels between them (red0 > red1: the 8-level
 * mode). img (values 0..1) is replaced by what the blocks decode to; with
 * blocks, each block's 8 bytes go to blocks + stride * its index (blocks in
 * rows, as the image's). Texels past the edge of a part block repeat the
 * edge's. */
void bc4_channel(float *img, int w, int h, uint8_t *blocks, size_t stride) {
    int bw = (w + 3) / 4;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4) {
            float lo = 1.0f, hi = 0.0f;
            for (int y = by; y < by + 4 && y < h; ++y)
                for (int x = bx; x < bx + 4 && x < w; ++x) {
                    lo = fminf(lo, img[y * w + x]);
                    hi = fmaxf(hi, img[y * w + x]);
                }
            int q0 = (int)floorf(fmaxf(lo, 0.0f) * 255.0f), q1 = (int)ceilf(fminf(hi, 1.0f) * 255.0f);
            if (q1 < q0) q1 = q0;
            lo = (float)q0 / 255.0f;
            hi = (float)q1 / 255.0f;
            uint64_t bits = 0;
            for (int j = 0; j < 16; ++j) {
                int x = bx + (j & 3), y = by + (j >> 2);
                float *p = &img[(y < h ? y : h - 1) * w + (x < w ? x : w - 1)];
                int k = hi > lo ? (int)lroundf((*p - lo) / (hi - lo) * 7.0f) : 7;
                if (k < 0) k = 0;
                if (k > 7) k = 7;
                if (x < w && y < h) *p = lo + (hi - lo) * (float)k / 7.0f;
                /* red0 = hi, red1 = lo: index 0 is hi, 1 is lo, 2..7 step down from hi */
                uint64_t idx = k == 7 ? 0u : k == 0 ? 1u : (uint64_t)(8 - k);
                bits |= idx << (3 * j);
            }
            if (!blocks) continue;
            uint8_t *b = blocks + stride * ((size_t)(by / 4) * bw + bx / 4);
            b[0] = (uint8_t)q1;
            b[1] = (uint8_t)q0;
            for (int i = 0; i < 6; ++i) b[2 + i] = (uint8_t)(bits >> (8 * i));
        }
}

uint32_t *imp_pack(const Capture *c, const ImpConfig *ic, size_t *bytes) {
    int nx = c->grid.nx, ny = c->grid.ny, views = view_count(&c->views), K = ic->K;
    size_t plane = (size_t)nx * ny;
    *bytes = (size_t)views * K * plane * 16u;
    uint32_t *out = malloc(*bytes);
    float *e = malloc(sizeof(float) * plane), *x = malloc(sizeof(float) * plane);
    if (!out || !e || !x) fatal("out of memory packing an impostor");
    for (int v = 0; v < views; ++v)
        for (int l = 0; l < K; ++l) {
            uint32_t *dst = out + ((size_t)v * K + l) * plane * 4u;
            for (int ty = 0; ty < ny; ++ty)
                for (int tx = 0; tx < nx; ++tx) {
                    int sx = tx, sy = ty;
                    if (ic->backHalf && l > 0) { sx &= ~1; sy &= ~1; }   /* half resolution: point-sampled */
                    size_t texel = (size_t)v * plane + (size_t)sy * nx + sx, i = (size_t)ty * nx + tx;
                    int slot = imp_slot(c, ic, texel, l);
                    const CapLayer *cl = &c->layers[texel * CAP_SLOTS + (slot < 0 ? 0 : slot)];
                    e[i] = slot < 0 ? 1.0f : cl->depth[0];
                    x[i] = slot < 0 ? 1.0f : cl->depth[1];
                    float lit[3] = { 0, 0, 0 };
                    if (slot >= 0) memcpy(lit, cl->lit, sizeof(lit));
                    dst[i * 4 + 1] = (uint32_t)half_from_float(lit[0]) | ((uint32_t)half_from_float(lit[1]) << 16);
                    dst[i * 4 + 2] = (uint32_t)half_from_float(lit[2]);
                    size_t own = (size_t)v * plane + i;   /* the flag is the texel's own */
                    dst[i * 4 + 3] = l == 0 && c->counts[own] > (uint32_t)K ? 1u : 0u;
                }
            if (ic->depth == DEPTH_BC5) {
                bc4_channel(e, nx, ny, NULL, 0);
                bc4_channel(x, nx, ny, NULL, 0);
            }
            for (size_t i = 0; i < plane; ++i) {
                uint32_t a = (uint32_t)lroundf(fminf(fmaxf(e[i], 0.0f), 1.0f) * 65535.0f);
                uint32_t b = (uint32_t)lroundf(fminf(fmaxf(x[i], 0.0f), 1.0f) * 65535.0f);
                dst[i * 4] = a | (b << 16);
            }
        }
    free(e);
    free(x);
    return out;
}
