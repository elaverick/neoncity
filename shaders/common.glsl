#ifndef COMMON_GLSL
#define COMMON_GLSL

/* Must match src/common.h */
const float CHUNK_SIZE      = 110.0;
const float LAMP_HEIGHT     = 7.5;
const float SIDEWALK_HEIGHT = 0.15;
const float KERB_STONE_WIDTH = 0.32;
const float DROP_RAMP_RUN = 1.5;
const float NEAR_PLANE      = 0.08;
const float FAR_PLANE       = 1995.0;   // VIEW_DISTANCE * 1.05 (reverse-Z projection, src/main.c)
const float SHELTER_SIZE    = 1024.0;   // shelter map (src/common.h)
const float SHELTER_LO      = -60.0;
const float SHELTER_HI      = 500.0;
const float CLOUD_LOW       = 115.0;    // the rain-cloud layer: scud...
const float CLOUD_HIGH      = 670.0;    // ...to deck top

// Sky lights (src/common.h)
const int   SKY_GRID        = 32;
const int   SKY_MAX_LIGHTS  = 2048;
const int   SKY_MAX_INDEX   = 32768;
const float SKY_GAIN        = 4.0;
const float SKY_RANGE_MAX   = 220.0;
const uint  SKY_KIND_SCREEN = 32u;
// Air lights (src/common.h): what lights the rain mist
const int   AIR_MAX_LIGHTS  = 256;
const float AIR_RANGE       = 140.0;
const uint  AIR_SIGN = 0u, AIR_LAMP_SODIUM = 1u, AIR_LAMP_LED = 2u, AIR_SHOP = 3u, AIR_LOBBY = 4u;
float skyLightRange(float power) { return min(sqrt(power * SKY_GAIN / 0.04), SKY_RANGE_MAX); }
const uint  WORLD_SEED      = 0xC1B3A7u;

const uint MAT_ROAD = 0u, MAT_SIDEWALK = 1u, MAT_CURB = 2u, MAT_LOT = 3u,
           MAT_FACADE = 4u, MAT_ROOF = 5u, MAT_NEON = 6u, MAT_METAL = 7u,
           MAT_EMISSIVE = 8u, MAT_SCREEN = 9u, MAT_LEDSTRIP = 10u, MAT_DOOR = 11u,
           MAT_INTERIOR = 12u, MAT_LITTER = 13u, MAT_ARCOLOGY = 14u;
// Litter kinds (src/common.h)
const uint LK_SHADOW = 0u, LK_NEWS = 1u, LK_FLYER = 2u, LK_SCRAP = 3u, LK_CARDBOARD = 4u, LK_WAD = 5u, LK_CAN = 6u,
           LK_BOTTLE = 7u, LK_BUTT = 8u, LK_CUP = 9u, LK_BOX = 10u, LK_BAG = 11u, LK_BINBAG = 12u;
// Closer than this, a lobby's glass shows its real interior (src/common.h INTERIOR_DISTANCE).
const float REAL_GLASS_DIST = 150.0;

// Authored PBR materials (src/textures.c): maps at material * PBR_MAPS_PER_MATERIAL + map.
const int PBR_PAVING_STONES = 0, PBR_ROAD = 1, PBR_KERB = 2, PBR_TACTILE = 3, PBR_MATERIAL_COUNT = 4;
const int PBR_ALBEDO = 0, PBR_NORMAL = 1, PBR_ORM = 2, PBR_MAPS_PER_MATERIAL = 3;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 rayBasis;   // col0 right*tanX, col1 up*tanY, col2 forward
    vec4 camPos;     // xyz world (approx), w = time
    vec4 camFwd;     // xyz forward, w = view distance
    vec4 screen;
    vec4 weather;    // x rain intensity, y street wetness (src/main.c: weather_update)
    vec4 shelter;    // shelter map: camera-relative centre x, z; half-width
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

// A lit storefront's light (its shop hash picks the colour; src/city.c: city_shop_hash).
vec3 shopColor(uint h) { return mix(neonColor(h >> 4), vec3(1.0, 0.82, 0.62), 0.45); }

// Light from the lobbies thrown onto the street and into the rain (about 3700 K,
// between the lobbies' warm and neutral panels: city.frag lobbyLight).
const vec3 LOBBY_SPILL = vec3(1.0, 0.80, 0.60);

vec3 lampColor(int type) {
    return type == 0 ? vec3(1.0, 0.42, 0.10) * 1.0   // sodium: deep amber
                     : vec3(0.55, 0.80, 1.0) * 0.9;  // cold LED
}

/* Failing street lamps (lamp_fault in src/city.c): fault & 3 is the state,
 * fault >> 2 a seed. 0 works; 1 is dead; 2 cycles, as a worn sodium lamp does:
 * it runs, sputters, goes out, then restrikes with a red glow that warms back
 * to orange; 3 is dim and reddened. Multiplies the lamp's colour. */
vec3 lampState(uint fault, float t) {
    uint state = fault & 3u, s = fault >> 2;
    if (state == 0u) return vec3(1.0);
    if (state == 1u) return vec3(0.0);
    if (state == 3u) return vec3(0.30, 0.17, 0.11);
    float period = 9.0 + 8.0 * hfloat(hash_combine(s, 1u));
    float ph = fract(t / period + hfloat(s));
    if (ph < 0.5) return vec3(1.0);
    if (ph < 0.66) return vec3(step(0.45, hfloat(hash_combine(s, uint(t * 14.0)))) * 0.9 + 0.1);   // sputtering
    if (ph < 0.82) return vec3(0.0);                                                                // out
    float w = smoothstep(0.82, 1.0, ph);                                                            // restrike
    return mix(vec3(0.25, 0.04, 0.03), vec3(1.0), w) * mix(0.3, 1.0, w);
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
    float density = (1.0 + 1.2 * frame.weather.x) / 380.0;   // rain thickens the air
    const float H = 110.0;                       // haze scale height
    float h0 = max(frame.camPos.y, 0.0);
    float h1 = max(frame.camPos.y + rel.y, 0.0);
    float dh = h1 - h0;
    float avg = abs(dh) > 1.0 ? H * (exp(-h0 / H) - exp(-h1 / H)) / dh : exp(-h0 / H);
    float f = 1.0 - exp(-dist * density * (0.25 + 0.75 * avg));
    return max(f, smoothstep(viewDist * 0.72, viewDist * 0.98, dist));
}

// The rain-cloud deck (clouds.comp) as reflections see it: an overcast lit
// sodium-orange from below, filling the upper sky.
const vec3 CLOUD_DECK = vec3(0.10, 0.052, 0.034);

/* What a reflection sees where the screen can't show it (the fallback that
 * screen-space reflections replace by confidence): the overcast lit
 * sodium-orange from below; around the horizon a blurred band of the lit
 * facades across the way, blocks of windows and the odd sign, varying with
 * direction; and below, the dark wet street under its lamps. */
vec3 envReflection(vec3 R) {
    vec3 c = mix(hazeColor(R) * 1.5, CLOUD_DECK, smoothstep(0.03, 0.3, R.y) * 0.85);
    float az = atan(R.z, R.x);
    float band = smoothstep(-0.3, -0.03, R.y) * (1.0 - smoothstep(0.08, 0.45, R.y));
    float rows = 0.45 + 0.55 * vnoise(vec2(az * 7.0, 0.0), 0xFAC1u);          // blocks of lit windows
    uint sh = hash2i(0xFAC3u, int(floor(az * 5.0)), 0);
    float sign_ = smoothstep(0.6, 0.85, vnoise(vec2(az * 13.0, 3.1), 0xFAC2u)) * (1.0 - smoothstep(-0.04, 0.1, R.y));
    vec3 facades = vec3(0.09, 0.068, 0.05) * rows + neonColor(sh) * sign_ * 0.12;
    c = mix(c, facades, band * 0.85);
    vec3 street = vec3(0.016, 0.014, 0.014) + lampColor(0) * 0.012;
    return mix(c, street, smoothstep(-0.12, -0.4, R.y));
}

// Fresnel reflectance of window glass (n.V = cos of the view angle). Real
// panes never quite reach a perfect mirror at grazing angles: rain film,
// grime and the slight bow of large panes keep it to about 70%.
float glassFresnel(vec3 n, vec3 V) { return 0.04 + 0.66 * pow(1.0 - max(dot(n, V), 0.0), 5.0); }

// Forward (view-axis) distance from a reverse-Z depth value; 0 (the sky) is infinitely far.
float forwardDist(float z) { return z > 0.0 ? NEAR_PLANE * FAR_PLANE / (z * (FAR_PLANE - NEAR_PLANE) + NEAR_PLANE) : 1e9; }

vec3 applyFog(vec3 col, vec3 rel) {
    float d = length(rel);
    vec3 dir = rel / max(d, 1e-4);
    return mix(col, hazeColor(dir), fogAmount(rel, d));
}

#endif
