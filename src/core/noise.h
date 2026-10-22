#pragma once
#include "hash.h"
#include "mathlib.h"

/* Value noise on the integer lattice, for slow procedural fields (the plan's
 * density and affluence, terrain). Coordinates are in lattice units: divide a
 * world position by the wavelength wanted. A pure function of (seed, x, z).
 *
 * The output feeds generated geometry, so changing the arithmetic here (the
 * fade curve, float vs double, the lattice hash) changes the city: check
 * city_test --fingerprint. */

static inline float noise_lattice(uint32_t seed, int64_t x, int64_t z) { return hash_float(hash2i(seed, (int32_t)x, (int32_t)z)); }

/* [0, 1). Smoothstep fade, so the field is C1 and its gradient is at most
 * 1.5 per lattice unit along each axis. */
static inline float noise_value2(uint32_t seed, double x, double z) {
    double fx0 = floor(x), fz0 = floor(z);
    int64_t ix = (int64_t)fx0, iz = (int64_t)fz0;
    float fx = (float)(x - fx0), fz = (float)(z - fz0);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float a = noise_lattice(seed, ix, iz), b = noise_lattice(seed, ix + 1, iz);
    float c = noise_lattice(seed, ix, iz + 1), d = noise_lattice(seed, ix + 1, iz + 1);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fz);
}

/* Signed fractal sum in [-1, 1]: octave k has wavelength 2^-k (lattice units)
 * and weight 2^-k, normalised by the total weight of `octaves` octaves, so the
 * octave count of a call site fixes its scale and must not vary with level of
 * detail. Each octave has its own seed and a fixed offset, so their lattices
 * don't line up.
 *
 * Only the first `used` octaves (used <= octaves) are summed: a level of
 * detail filters out the finer ones while the coarse ones keep their
 * amplitude, so the large-scale field is never reshaped. noise_fbm2 sums them
 * all, with the same arithmetic as before the band existed (check
 * terrain_test --fingerprint). */
static inline float noise_fbm2_band(uint32_t seed, double x, double z, int octaves, int used) {
    double sum = 0.0, wsum = 0.0, w = 1.0, f = 1.0;
    for (int k = 0; k < octaves; ++k) {
        if (k < used) {
            float v = noise_value2(hash_combine(seed, (uint32_t)k), x * f + 17.31 * k, z * f - 41.77 * k);
            sum += w * (2.0 * v - 1.0);
        }
        wsum += w;
        w *= 0.5;
        f *= 2.0;
    }
    return (float)(sum / wsum);
}

static inline float noise_fbm2(uint32_t seed, double x, double z, int octaves) {
    return noise_fbm2_band(seed, x, z, octaves, octaves);
}

/* The same in three dimensions, for fields over the planet's sphere (the
 * terrain's landforms, the stand-in's continents), which no chart can carry
 * without a seam. */
static inline float noise_value3(uint32_t seed, double x, double y, double z) {
    double fx0 = floor(x), fy0 = floor(y), fz0 = floor(z);
    int32_t ix = (int32_t)fx0, iy = (int32_t)fy0, iz = (int32_t)fz0;
    float fx = (float)(x - fx0), fy = (float)(y - fy0), fz = (float)(z - fz0);
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    fz = fz * fz * (3.0f - 2.0f * fz);
    float c[2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            c[dz][dy] = lerpf(hash_float(hash3i(seed, ix, iy + dy, iz + dz)), hash_float(hash3i(seed, ix + 1, iy + dy, iz + dz)), fx);
    return lerpf(lerpf(c[0][0], c[0][1], fy), lerpf(c[1][0], c[1][1], fy), fz);
}

static inline float noise_fbm3_band(uint32_t seed, double x, double y, double z, int octaves, int used) {
    double sum = 0.0, wsum = 0.0, w = 1.0, f = 1.0;
    for (int k = 0; k < octaves; ++k) {
        if (k < used) {
            float v = noise_value3(hash_combine(seed, (uint32_t)k), x * f + 17.31 * k, y * f + 29.53 * k, z * f - 41.77 * k);
            sum += w * (2.0 * v - 1.0);
        }
        wsum += w;
        w *= 0.5;
        f *= 2.0;
    }
    return (float)(sum / wsum);
}
