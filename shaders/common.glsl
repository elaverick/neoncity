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
const int PBR_PAVING_STONES = 0, PBR_ROAD = 1, PBR_KERB = 2, PBR_TACTILE = 3,
          PBR_MARBLE_DARK = 4, PBR_MARBLE_WHITE = 5, PBR_LIMESTONE = 6, PBR_WOOD_DARK = 7, PBR_WOOD_MID = 8, PBR_WOOD_LIGHT = 9,
          PBR_LEAF = 10, PBR_MATERIAL_COUNT = 11;
const int PBR_ALBEDO = 0, PBR_NORMAL = 1, PBR_ORM = 2, PBR_MAPS_PER_MATERIAL = 3;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 rayBasis;   // col0 right*tanX, col1 up*tanY, col2 forward
    vec4 camPos;     // xyz world (approx), w = time
    vec4 camFwd;     // xyz forward, w = view distance
    vec4 screen;
    vec4 weather;    // x rain intensity, y street wetness (src/main.c: weather_update)
    vec4 shelter;    // shelter map: camera-relative centre x, z; half-width
    vec4 shadow;     // ray-traced shadows: x frame number, y SHADOWS_ON | SHADOWS_DEBUG, zw where sample 0 lies in a pixel
    mat4 prevViewProj;   // the previous frame's viewProj, taking this frame's camera-relative positions
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

// Light from the lobbies thrown onto the street and into the rain (about 2900 K,
// between the lobbies' 2700 K and 3000 K panels: city.frag lobbyLight).
const vec3 LOBBY_SPILL = vec3(1.0, 0.66, 0.37);

/* ---------------- ray-traced shadows ----------------
 * src/rt.c builds the acceleration structures; shadow_trace.comp traces one
 * ray per light at each pixel's most important lights, shadow_denoise.comp
 * filters them into soft shadows, and city.frag looks each light up
 * (shadowVis), tracing a hard shadow itself wherever the pixel's surface
 * isn't the one the prepass saw. */
const float SHADOW_FADE = 350.0, SHADOW_FAR = 450.0;    // src/common.h
const uint  SHADOWS_ON = 1u, SHADOWS_DEBUG = 2u;         // frame.shadow.y (renderer.c)
bool shadowsOn() { return (uint(frame.shadow.y) & SHADOWS_ON) != 0u; }
const int   SHADOW_SLOTS = 8;                            // lights a pixel keeps soft shadows for
const uint  RT_MASK_LITTER = 0x04u;                      // src/rt.h
const float LIGHT_NEGLIGIBLE = 0.002;                    // a light this faint shows no shadow worth tracing
// Prepass G-buffer (prepass.frag): x the normal (octEncode), y the draw chunk | these flags.
const uint  PG_LIT = 1u << 30, PG_LITTER = 1u << 29;
// Shadow slots (shadow_trace.comp, shadow_denoise.comp): flags in raw.x and vis.w.
const uint  SH_VALID = 1u << 30, SH_OVERFLOW = 1u << 31;

// Street lamp falloff (city.frag: addLamp), l = light - p: the beam below the
// head, and a spill all round it that carries up the columns and facades.
float lampFalloff(vec3 l) {
    float d2 = dot(l, l), d = sqrt(d2);
    float spot = mix(0.12, 1.0, smoothstep(0.05, 0.55, l.y / max(d, 1e-4)));
    return 90.0 / (d2 + 1.0) * smoothstep(34.0, 14.0, d) * spot;
}
// A lit shopfront's or lobby's spill, d metres from its light (city.frag: addShop).
float shopFalloff(float d) { return d > 12.0 ? 0.0 : 0.45 * exp(-d * 0.38) * smoothstep(12.0, 7.0, d); }

/* Of a lamp's light reaching p, the share that comes straight from the head:
 * lampFalloff's beam, rather than its spill all round, which stands for the
 * light the wet street throws back up from the pool under the lamp. */
float lampBeamShare(vec3 lp, vec3 p) {
    vec3 l = lp - p;
    float t = smoothstep(0.05, 0.55, l.y / max(length(l), 1e-4));
    return 0.88 * t / (0.12 + 0.88 * t);
}
const float POOL_RADIUS = 4.0, POOL_HEIGHT = 0.5;   // the lit pool a lamp's spill comes from, just above the street

/* A point on light record w's emitter (lp chunk-local) as seen from p, from
 * xi in [0,1)^3, and the emitter's size (for penumbra estimates). A street
 * lamp's beam glows through the diffuser under the head, a disc just below it,
 * and its spill rises from the pool of light beneath it: xi.z picks one, by
 * the share each has at p. Arcology lights are broader (LE_* in src/city.c).
 * A shop or lobby light stands 1 m out from its pane, which glows over the
 * bay's width from knee height to over the door. */
vec3 emitterPoint(vec3 lp, uint w, vec3 xi, vec3 p, out float size) {
    if ((w & 3u) < 2u) {
        uint cls = (w >> 26) & 3u;
        float R = cls == 0u ? 0.25 : cls == 1u ? 0.4 : 1.5;
        vec3 c = lp;
        if (xi.z >= lampBeamShare(lp, p)) { R = POOL_RADIUS; c.y = POOL_HEIGHT; }
        float r = R * sqrt(xi.x), a = 6.2831853 * xi.y;
        size = 2.0 * R;
        return c + vec3(r * cos(a), 0.0, r * sin(a));
    }
    float ang = float((w >> 16) & 255u) * (6.2831853 / 256.0);
    vec3 N = vec3(cos(ang), 0.0, sin(ang)), T = vec3(-N.z, 0.0, N.x);
    float hw = max(float((w >> 24) & 63u) * 0.125 - 0.3, 0.3), hh = 1.25;
    size = 2.0 * max(hw, hh);
    return lp - N * 0.8 + T * ((xi.x * 2.0 - 1.0) * hw) + vec3(0.0, 0.35 + (xi.y * 2.0 - 1.0) * hh, 0.0);   // 20 cm proud of the glass
}

// Whether light record w casts shadows. An arcology's crown lights (LE_ARC_CROWN
// in src/city.c) stand for the cold light falling down the canyon from its
// glazed crown, a fill from far above rather than a light at the point they
// mark, so they light the galleries unshadowed.
bool castsShadow(uint w) { return (w & 3u) >= 2u || ((w >> 26) & 3u) != 2u; }

// The middle of the emitter, as seen from p: where city.frag aims a hard shadow ray.
vec3 emitterCentre(vec3 lp, uint w, vec3 p) {
    if ((w & 3u) < 2u) return lampBeamShare(lp, p) >= 0.5 ? lp : vec3(lp.x, POOL_HEIGHT, lp.z);
    float ang = float((w >> 16) & 255u) * (6.2831853 / 256.0);
    return lp - vec3(cos(ang), 0.0, sin(ang)) * 0.8 + vec3(0.0, 0.35, 0.0);
}

// A light's identity in a pixel's shadow slots: the quarter-metre cell it
// stands in, counted in whole cells from its chunk, so the same lamp listed in
// two chunks' tables (and in successive frames) has the same id. 0 = empty.
uint lightId(ivec2 chunk, vec3 lp, uint w) {
    uvec3 q = uvec3(ivec3(floor(lp * 4.0)) + ivec3(chunk.x * 440, 0, chunk.y * 440));   // 440 = CHUNK_SIZE * 4
    uint h = q.x * 73856093u ^ q.y * 19349663u ^ q.z * 83492791u ^ (w & 3u) * 2654435761u;
    return max((h ^ (h >> 16)) & 0xFFFFu, 1u);
}

uint octEncode(vec3 n) {
    vec2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return packSnorm2x16(p);
}
vec3 octDecode(uint u) {
    vec2 p = unpackSnorm2x16(u);
    vec3 n = vec3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// Interleaved gradient noise (Jimenez): neighbouring pixels well spread over [0,1).
float ign(vec2 px) { return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715)))); }

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

/* How much light record w (at lp) throws on a surface at p, facing n and seen
 * along V, before shadowing: its falloff, its lit side (shop light wraps round,
 * as addShop's does) and a rough mirror lobe, so a lamp glinting in a wet
 * street counts even when it barely lights it. Picks which lights get soft
 * shadows, and which are too faint to shadow at all. */
float lightImportance(vec3 p, vec3 n, vec3 V, vec3 lp, uint w) {
    vec3 l = lp - p;
    float d = length(l);
    vec3 L = l / max(d, 1e-4);
    float ndl = dot(n, L);
    float glint = ndl > 0.0 ? 4.0 * pow(max(dot(reflect(-V, n), L), 0.0), 24.0) : 0.0;
    uint type = w & 3u;
    if (type < 2u) return lampFalloff(l) * luma(lampColor(int(type))) * (max(ndl, 0.0) + 0.05 + glint);
    return shopFalloff(d) * 0.6 * (0.35 + 0.65 * max(ndl, 0.0) + glint);
}

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

// A texel of the reflection G-buffer (city.frag: outRefl), with the normal and
// roughness divided back out of the weight; a = 0 where nothing reflects.
vec4 reflSample(sampler2D t, ivec2 px) {
    vec4 g = texelFetch(t, px, 0);
    return g.a > 1e-3 ? vec4(g.xyz / g.a, g.a) : vec4(0.0);
}

vec3 applyFog(vec3 col, vec3 rel) {
    float d = length(rel);
    vec3 dir = rel / max(d, 1e-4);
    return mix(col, hazeColor(dir), fogAmount(rel, d));
}

#endif
