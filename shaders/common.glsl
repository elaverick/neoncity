#ifndef COMMON_GLSL
#define COMMON_GLSL

/* Must match src/common.h */
const float CHUNK_SIZE      = 110.0;
const float LAMP_HEIGHT     = 7.5;
const float SIDEWALK_HEIGHT = 0.15;
const float KERB_STONE_WIDTH = 0.32;
const float NEAR_PLANE      = 0.08;
const float FAR_PLANE       = 1995.0;   // VIEW_DISTANCE * 1.05 (reverse-Z projection, src/main.c)
const float CLOUD_LOW       = 165.0;    // the rain-cloud layer: scud...
const float CLOUD_HIGH      = 720.0;    // ...to deck top

// Sky lights (src/common.h)
const int   SKY_GRID        = 32;
const int   SKY_MAX_LIGHTS  = 2048;
const int   SKY_MAX_INDEX   = 32768;
const float SKY_GAIN        = 4.0;
const float SKY_RANGE_MAX   = 220.0;
const uint  SKY_KIND_SCREEN = 32u;
float skyLightRange(float power) { return min(sqrt(power * SKY_GAIN / 0.04), SKY_RANGE_MAX); }
const uint  WORLD_SEED      = 0xC1B3A7u;

const uint MAT_ROAD = 0u, MAT_SIDEWALK = 1u, MAT_CURB = 2u, MAT_LOT = 3u,
           MAT_FACADE = 4u, MAT_ROOF = 5u, MAT_NEON = 6u, MAT_METAL = 7u,
           MAT_EMISSIVE = 8u, MAT_SCREEN = 9u, MAT_LEDSTRIP = 10u, MAT_DOOR = 11u;

// Authored PBR materials (src/textures.c): maps at material * PBR_MAPS_PER_MATERIAL + map.
const int PBR_PAVING_STONES = 0, PBR_ROAD = 1, PBR_KERB = 2, PBR_MATERIAL_COUNT = 3;
const int PBR_ALBEDO = 0, PBR_NORMAL = 1, PBR_ORM = 2, PBR_MAPS_PER_MATERIAL = 3;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 rayBasis;   // col0 right*tanX, col1 up*tanY, col2 forward
    vec4 camPos;     // xyz world (approx), w = time
    vec4 camFwd;     // xyz forward, w = view distance
    vec4 screen;
} frame;

struct ChunkGPU {
    vec4  origin;    // chunk origin relative to camera
    ivec4 info;      // cx, cz, first light record (-1: none), unused
};
layout(std430, set = 0, binding = 1) readonly buffer Chunks { ChunkGPU chunks[]; };

// Per-chunk light tables (see build_lights in src/city.c): a header record,
// 16 tile words (first << 16 | count), then lights (xyz, w = type | data).
layout(std430, set = 0, binding = 2) readonly buffer Lights { uvec4 lightRec[]; };

/* ---------------- hashing: bit-identical to src/hash.h ---------------- */
uint hash_u32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint hash_combine(uint h, uint v) { return hash_u32(h ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2))); }
uint hash2i(uint seed, int a, int b) { return hash_combine(hash_combine(seed, uint(a)), uint(b)); }
uint hash3i(uint seed, int a, int b, int c) { return hash_combine(hash2i(seed, a, b), uint(c)); }
float hfloat(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

/* Value noise on an integer lattice (stable far from the origin when fed wrapped coordinates). */
float vnoise(vec2 p, uint salt) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    ivec2 ii = ivec2(i);
    float a = hfloat(hash2i(salt, ii.x, ii.y)),     b = hfloat(hash2i(salt, ii.x + 1, ii.y));
    float c = hfloat(hash2i(salt, ii.x, ii.y + 1)), d = hfloat(hash2i(salt, ii.x + 1, ii.y + 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p, uint salt) {
    return 0.5 * vnoise(p, salt) + 0.3 * vnoise(p * 2.03, salt + 17u) + 0.2 * vnoise(p * 4.11, salt + 31u);
}

/* Facade rhythm: must match city_floor_height / city_ground_height in src/city.c */
uint  styleHash(uint seed)      { return hash_u32(seed ^ 0x5f3759dfu); }
float facadeFloorH(uint seed)  { return 3.3 + 0.9 * hfloat(hash_combine(styleHash(seed), 1u)); }
float facadeGroundH(uint seed) { return 4.6 + 1.4 * hfloat(hash_combine(styleHash(seed), 2u)); }

vec3 neonColor(uint i) {
    i &= 7u;
    if (i == 0u) return vec3(1.00, 0.08, 0.50);  // hot pink
    if (i == 1u) return vec3(0.05, 0.85, 1.00);  // cyan
    if (i == 2u) return vec3(1.00, 0.10, 0.04);  // red
    if (i == 3u) return vec3(1.00, 0.50, 0.06);  // amber
    if (i == 4u) return vec3(0.55, 0.12, 1.00);  // violet
    if (i == 5u) return vec3(0.15, 1.00, 0.40);  // acid green
    if (i == 6u) return vec3(0.65, 0.85, 1.00);  // ice white
    return vec3(1.00, 0.85, 0.15);               // yellow
}

vec3 lampColor(int type) {
    return type == 0 ? vec3(1.0, 0.52, 0.20) * 1.0   // sodium
                     : vec3(0.55, 0.80, 1.0) * 0.9;  // cold LED
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

/* Neon flicker modes (the sign material's flicker bits): 0 steady,
 * 1 occasional dropout, 2 failing tube that buzzes and dims. */
float neonFlicker(uint seed, uint mode, float t) {
    if (mode == 0u) return 1.0;
    if (mode == 1u) {
        float burst = step(0.93, hfloat(hash_combine(seed, uint(floor(t * 0.5)))));
        return 1.0 - burst * step(0.5, hfloat(hash_combine(seed, uint(floor(t * 14.0)))));
    }
    float ph = fract(t * 0.23 + hfloat(seed));
    float buzz = step(0.85, ph) * step(0.4, hfloat(hash_combine(seed, uint(t * 20.0))));
    return mix(1.0, 0.15, buzz) * (0.85 + 0.15 * sin(t * 120.0));
}

// Video screens change advert every 9 s; the advert's hash picks its scene and colours.
uint screenAdvert(uint seed, float t) { return hash_combine(seed, uint(floor((t + hfloat(seed) * 30.0) / 9.0))); }

/* ---------------- atmosphere ----------------
 * A smoggy night: warm city-lit haze near the horizon, cold dark zenith.
 * Fog uses the same colour as the sky at the horizon so the finite view
 * distance dissolves seamlessly into the haze. (Weather comes later.) */
vec3 hazeColor(vec3 dir) {
    float y = dir.y;
    vec3 low  = vec3(0.095, 0.050, 0.036);   // sodium-lit smog
    vec3 mid  = vec3(0.034, 0.026, 0.040);
    vec3 high = vec3(0.005, 0.007, 0.014);
    vec3 c = mix(low, mid, smoothstep(-0.02, 0.22, y));
    c = mix(c, high, smoothstep(0.18, 0.75, y));
    return c;
}

float fogAmount(vec3 rel, float dist) {
    float viewDist = frame.camFwd.w;
    const float density = 1.0 / 380.0;
    const float H = 110.0;                       // haze scale height
    float h0 = max(frame.camPos.y, 0.0);
    float h1 = max(frame.camPos.y + rel.y, 0.0);
    float dh = h1 - h0;
    float avg = abs(dh) > 1.0 ? H * (exp(-h0 / H) - exp(-h1 / H)) / dh : exp(-h0 / H);
    float f = 1.0 - exp(-dist * density * (0.25 + 0.75 * avg));
    return max(f, smoothstep(viewDist * 0.72, viewDist * 0.98, dist));
}

vec3 applyFog(vec3 col, vec3 rel) {
    float d = length(rel);
    vec3 dir = rel / max(d, 1e-4);
    return mix(col, hazeColor(dir), fogAmount(rel, d));
}

#endif
