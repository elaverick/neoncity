#include "bake.h"

/* Block encoders for the bake's colour channels (docs/interior-impostors.md,
 * section 6): BC6H for the radiance, BC7 for the albedo and the exterior
 * bases. Each uses one mode, a single line through the block's colours: BC6H's
 * mode 11 (10-bit endpoints, 16 weights) and BC7's mode 6 (8-bit endpoints in
 * RGBA, 16 weights). The line is the texels' principal axis, then refined by
 * least squares (BC6H also tries one between its bright and dark texels' means,
 * and keeps what looks nearer). Slow encoders with many modes would gain little here: a bake
 * is noise-free and mostly smooth, and these run in seconds. The decoders
 * measure what the encoders lose (ktx2.c, test.c). */

static const int WEIGHTS[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

typedef struct Bits { uint8_t *b; int at; } Bits;

static void put(Bits *o, uint32_t v, int n) {
    for (int i = 0; i < n; ++i, ++o->at)
        if ((v >> i) & 1u) o->b[o->at >> 3] |= (uint8_t)(1u << (o->at & 7));
}

static uint32_t get(const uint8_t *b, int *at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++*at) v |= (uint32_t)((b[*at >> 3] >> (*at & 7)) & 1u) << i;
    return v;
}

/* The line through the valid points (dim channels, each of them in p[i]): its two ends, along the
 * principal axis, refined by least squares against the 16 weights. */
static void fit_line(const float (*p)[4], const uint8_t *valid, int dim, float e0[4], float e1[4]) {
    float mean[4] = { 0, 0, 0, 0 };
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        if (valid && !valid[i]) continue;
        for (int k = 0; k < dim; ++k) mean[k] += p[i][k];
        n++;
    }
    if (!n) { for (int k = 0; k < 4; ++k) e0[k] = e1[k] = 0.0f; return; }
    for (int k = 0; k < dim; ++k) mean[k] /= (float)n;
    float cov[4][4] = { { 0 } };
    for (int i = 0; i < 16; ++i) {
        if (valid && !valid[i]) continue;
        for (int a = 0; a < dim; ++a)
            for (int b = 0; b < dim; ++b) cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
    }
    float axis[4] = { 1, 1, 1, 1 };
    for (int it = 0; it < 8; ++it) {
        float next[4] = { 0, 0, 0, 0 }, len = 0.0f;
        for (int a = 0; a < dim; ++a) {
            for (int b = 0; b < dim; ++b) next[a] += cov[a][b] * axis[b];
            len += next[a] * next[a];
        }
        if (len <= 1e-12f) break;
        len = sqrtf(len);
        for (int a = 0; a < dim; ++a) axis[a] = next[a] / len;
    }
    float t0 = 1e30f, t1 = -1e30f;
    for (int i = 0; i < 16; ++i) {
        if (valid && !valid[i]) continue;
        float t = 0.0f;
        for (int k = 0; k < dim; ++k) t += (p[i][k] - mean[k]) * axis[k];
        t0 = fminf(t0, t);
        t1 = fmaxf(t1, t);
    }
    for (int k = 0; k < dim; ++k) { e0[k] = mean[k] + axis[k] * t0; e1[k] = mean[k] + axis[k] * t1; }
    for (int pass = 0; pass < 2; ++pass) {
        /* each point's weight on the line, then the ends that best fit those weights */
        float aa = 0.0f, ab = 0.0f, bb = 0.0f, ax[4] = { 0, 0, 0, 0 }, bx[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 16; ++i) {
            if (valid && !valid[i]) continue;
            int best = 0;
            float bestE = 1e30f;
            for (int w = 0; w < 16; ++w) {
                float t = (float)WEIGHTS[w] / 64.0f, err = 0.0f;
                for (int k = 0; k < dim; ++k) { float d = e0[k] + (e1[k] - e0[k]) * t - p[i][k]; err += d * d; }
                if (err < bestE) { bestE = err; best = w; }
            }
            float t = (float)WEIGHTS[best] / 64.0f, s = 1.0f - t;
            aa += s * s; ab += s * t; bb += t * t;
            for (int k = 0; k < dim; ++k) { ax[k] += s * p[i][k]; bx[k] += t * p[i][k]; }
        }
        float det = aa * bb - ab * ab;
        if (fabsf(det) < 1e-6f) break;
        for (int k = 0; k < dim; ++k) {
            e0[k] = (ax[k] * bb - bx[k] * ab) / det;
            e1[k] = (bx[k] * aa - ax[k] * ab) / det;
        }
    }
}

/* ---------------------------------------------------------------- BC6H --- */

/* BC6H's unsigned path: interpolation runs on 16-bit values that finish as (v * 31) >> 6, the bits
 * of an unsigned half. So it is linear in the half's bits: nearly logarithmic in the radiance. */
static int bc6_unquant(int c) { return c == 0 ? 0 : c == 1023 ? 0xFFFF : ((c << 16) + 0x8000) >> 10; }
static int bc6_interp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }
static int bc6_finish(int v) { return (v * 31) >> 6; }

static float float_from_half(int h) {
    int e = (h >> 10) & 31, m = h & 1023;
    return e == 0 ? (float)m / 16777216.0f : ldexpf(1.0f + (float)m / 1024.0f, e - 15);
}

/* How far apart two radiances look, for choosing a block's line: nearly linear in the dark, where a
 * small slip doesn't show, and logarithmic in the light, where a slip of a stop in one channel shows
 * as a change of hue. */
static float bc6_look(float v) { return logf(1.0f + 8.0f * BAKE_SCALE * v); }

void bc6h_encode(const float rgb[16][3], const uint8_t *valid, uint8_t out[16]) {
    float p[16][4], look[16][3];
    float lumMin = 1e30f, lumMax = -1e30f, lum[16];
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) {
            float v = rgb[i][k];
            int h = v > 0.0f ? half_from_float(v) : 0;
            if (h > 0x7BFF) h = 0x7BFF;
            p[i][k] = (float)h * (64.0f / 31.0f);
            look[i][k] = bc6_look(v > 0.0f ? v : 0.0f);
        }
        p[i][3] = 0.0f;
        lum[i] = p[i][0] + p[i][1] + p[i][2];
        if (!valid || valid[i]) { lumMin = fminf(lumMin, lum[i]); lumMax = fmaxf(lumMax, lum[i]); }
    }
    /* Two lines to try. The least-squares one treats every texel's bits alike, and a block holding a bright
     * strip, the dark floor and a third colour can leave the strip's texels a stop out in one channel: a red
     * fringe. The other runs from the bright texels' mean to the dark texels' mean, so both ends are right. */
    float cand[2][2][4];
    fit_line((const float (*)[4])p, valid, 3, cand[0][0], cand[0][1]);
    float hi[3] = { 0, 0, 0 }, lo[3] = { 0, 0, 0 }, mid = 0.5f * (lumMin + lumMax);
    int nh = 0, nl = 0;
    for (int i = 0; i < 16; ++i) {
        if (valid && !valid[i]) continue;
        bool up = lum[i] > mid;
        for (int k = 0; k < 3; ++k) { if (up) hi[k] += p[i][k]; else lo[k] += p[i][k]; }
        if (up) nh++; else nl++;
    }
    for (int k = 0; k < 3; ++k) {
        cand[1][0][k] = nl ? lo[k] / (float)nl : (nh ? hi[k] / (float)nh : 0.0f);
        cand[1][1][k] = nh ? hi[k] / (float)nh : cand[1][0][k];
    }
    int e[2][3], idx[16];
    float bestAll = 1e30f;
    for (int c = 0; c < 2; ++c) {
        int ec[2][3], ic[16];
        float dec[16][3], total = 0.0f;
        for (int k = 0; k < 3; ++k) {
            ec[0][k] = (int)lroundf(fminf(fmaxf(cand[c][0][k], 0.0f), 65535.0f) * (1023.0f / 65535.0f));
            ec[1][k] = (int)lroundf(fminf(fmaxf(cand[c][1][k], 0.0f), 65535.0f) * (1023.0f / 65535.0f));
            int u0 = bc6_unquant(ec[0][k]), u1 = bc6_unquant(ec[1][k]);
            for (int w = 0; w < 16; ++w) dec[w][k] = bc6_look(float_from_half(bc6_finish(bc6_interp(u0, u1, WEIGHTS[w]))));
        }
        for (int i = 0; i < 16; ++i) {
            float best = 1e30f;
            ic[i] = 0;
            for (int w = 0; w < 16; ++w) {
                float err = 0.0f;
                for (int k = 0; k < 3; ++k) { float d = dec[w][k] - look[i][k]; err += d * d; }
                if (err < best) { best = err; ic[i] = w; }
            }
            if (!valid || valid[i]) total += best;
        }
        if (total < bestAll) { bestAll = total; memcpy(e, ec, sizeof(e)); memcpy(idx, ic, sizeof(idx)); }
    }
    if (idx[0] >= 8) {   /* the anchor's top bit is implied 0: swap the ends */
        for (int k = 0; k < 3; ++k) { int t = e[0][k]; e[0][k] = e[1][k]; e[1][k] = t; }
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }
    memset(out, 0, 16);
    Bits o = { out, 0 };
    put(&o, 3u, 5);   /* mode 11: 00011 */
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 3; ++k) put(&o, (uint32_t)e[s][k], 10);
    for (int i = 0; i < 16; ++i) put(&o, (uint32_t)idx[i], i == 0 ? 3 : 4);
}

void bc6h_decode(const uint8_t in[16], float rgb[16][3]) {
    int at = 5, u[2][3];
    for (int s = 0; s < 2; ++s)
        for (int k = 0; k < 3; ++k) u[s][k] = bc6_unquant((int)get(in, &at, 10));
    for (int i = 0; i < 16; ++i) {
        int w = WEIGHTS[get(in, &at, i == 0 ? 3 : 4)];
        for (int k = 0; k < 3; ++k) rgb[i][k] = float_from_half(bc6_finish(bc6_interp(u[0][k], u[1][k], w)));
    }
}

/* ----------------------------------------------------------------- BC7 --- */

/* An end's 7-bit channels and shared low bit, nearest to the 0..255 values t. */
static void bc7_quant(const float t[4], int c[4], int *pbit) {
    float best = 1e30f;
    for (int p = 0; p < 2; ++p) {
        int q[4];
        float err = 0.0f;
        for (int k = 0; k < 4; ++k) {
            q[k] = (int)lroundf((fminf(fmaxf(t[k], 0.0f), 255.0f) - (float)p) * 0.5f);
            q[k] = q[k] < 0 ? 0 : q[k] > 127 ? 127 : q[k];
            float d = (float)(q[k] * 2 + p) - t[k];
            err += d * d;
        }
        if (err < best) { best = err; *pbit = p; memcpy(c, q, sizeof(q)); }
    }
}

void bc7_encode(const uint8_t rgba[16][4], const uint8_t *valid, uint8_t out[16]) {
    float p[16][4];
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 4; ++k) p[i][k] = (float)rgba[i][k];
    float a[4], b[4];
    fit_line((const float (*)[4])p, valid, 4, a, b);
    int e[2][4], pb[2];
    bc7_quant(a, e[0], &pb[0]);
    bc7_quant(b, e[1], &pb[1]);
    int idx[16];
    for (int i = 0; i < 16; ++i) {
        int best = 1 << 30;
        idx[i] = 0;
        for (int w = 0; w < 16; ++w) {
            int err = 0;
            for (int k = 0; k < 4; ++k) {
                int v0 = e[0][k] * 2 + pb[0], v1 = e[1][k] * 2 + pb[1];
                int d = ((64 - WEIGHTS[w]) * v0 + WEIGHTS[w] * v1 + 32) / 64 - rgba[i][k];
                err += d * d;
            }
            if (err < best) { best = err; idx[i] = w; }
        }
    }
    if (idx[0] >= 8) {
        for (int k = 0; k < 4; ++k) { int t = e[0][k]; e[0][k] = e[1][k]; e[1][k] = t; }
        int t = pb[0]; pb[0] = pb[1]; pb[1] = t;
        for (int i = 0; i < 16; ++i) idx[i] = 15 - idx[i];
    }
    memset(out, 0, 16);
    Bits o = { out, 0 };
    put(&o, 1u << 6, 7);   /* mode 6 */
    for (int k = 0; k < 4; ++k) { put(&o, (uint32_t)e[0][k], 7); put(&o, (uint32_t)e[1][k], 7); }
    put(&o, (uint32_t)pb[0], 1);
    put(&o, (uint32_t)pb[1], 1);
    for (int i = 0; i < 16; ++i) put(&o, (uint32_t)idx[i], i == 0 ? 3 : 4);
}

void bc7_decode(const uint8_t in[16], uint8_t rgba[16][4]) {
    int at = 7, e[2][4];
    for (int k = 0; k < 4; ++k) { e[0][k] = (int)get(in, &at, 7); e[1][k] = (int)get(in, &at, 7); }
    int p0 = (int)get(in, &at, 1), p1 = (int)get(in, &at, 1);
    for (int i = 0; i < 16; ++i) {
        int w = WEIGHTS[get(in, &at, i == 0 ? 3 : 4)];
        for (int k = 0; k < 4; ++k)
            rgba[i][k] = (uint8_t)(((64 - w) * (e[0][k] * 2 + p0) + w * (e[1][k] * 2 + p1) + 32) >> 6);
    }
}
