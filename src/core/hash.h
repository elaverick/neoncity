#pragma once
#include <stdint.h>

/* Integer hashing used for all procedural decisions. shaders/common.glsl has a
 * bit-identical copy so CPU-built geometry and GPU-shaded detail agree. */

static inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static inline uint32_t hash_combine(uint32_t h, uint32_t v) {
    return hash_u32(h ^ (v + 0x9e3779b9U + (h << 6) + (h >> 2)));
}

static inline uint32_t hash2i(uint32_t seed, int32_t a, int32_t b) {
    return hash_combine(hash_combine(seed, (uint32_t)a), (uint32_t)b);
}

static inline uint32_t hash3i(uint32_t seed, int32_t a, int32_t b, int32_t c) {
    return hash_combine(hash2i(seed, a, b), (uint32_t)c);
}

/* [0,1) from the top 24 bits. */
static inline float hash_float(uint32_t h) { return (float)(h >> 8) * (1.0f / 16777216.0f); }

/* Small deterministic stream generator seeded from a hash: same seed, same sequence. */
typedef struct Rng { uint32_t state; } Rng;
static inline Rng rng_make(uint32_t seed) { Rng r = { hash_u32(seed ^ 0xA511E9B3U) }; return r; }
static inline uint32_t rng_u32(Rng *r) { r->state = hash_u32(r->state + 0x6D2B79F5U); return r->state; }
static inline float rng_float(Rng *r) { return hash_float(rng_u32(r)); }
static inline float rng_range(Rng *r, float lo, float hi) { return lo + (hi - lo) * rng_float(r); }
static inline int rng_int(Rng *r, int lo, int hi_inclusive) {
    return lo + (int)(rng_u32(r) % (uint32_t)(hi_inclusive - lo + 1));
}
