#ifndef COMMON_GLSL
#define COMMON_GLSL

// VIEW_DISTANCE, TERRAIN_DISTANCE, NEAR_PLANE, FAR_PLANE: shared with C
#include "distances.h"
// PLANET_RADIUS: shared with C
#include "terrain_planet.h"
// CLOUD_CITIES, CLOUD_BORDER, CLOUD_FADE, CLOUD_RAGGED, CLOUD_REACH: shared with C
#include "clouds.h"

/* Must match src/core/common.h */
const float CHUNK_SIZE      = 110.0;
const float LAMP_HEIGHT     = 7.5;
const float SIDEWALK_HEIGHT = 0.15;
const float KERB_STONE_WIDTH = 0.32;
const float DROP_RAMP_RUN = 1.5;
const float SHELTER_SIZE    = 1024.0;   // shelter map (src/core/common.h)
const float SHELTER_LO      = -60.0;
const float SHELTER_HI      = 500.0;
const float CLOUD_LOW       = 115.0;    // the rain-cloud layer: scud...
const float CLOUD_HIGH      = 670.0;    // ...to deck top

// Sky lights (src/core/common.h)
const int   SKY_GRID        = 32;
const int   SKY_MAX_LIGHTS  = 2048;
const int   SKY_MAX_INDEX   = 32768;
const float SKY_GAIN        = 4.0;
const float SKY_RANGE_MAX   = 220.0;
const uint  SKY_KIND_SCREEN = 32u;
// Air lights (src/core/common.h): what lights the rain mist
const int   AIR_MAX_LIGHTS  = 256;
const float AIR_RANGE       = 140.0;
const uint  AIR_SIGN = 0u, AIR_LAMP_SODIUM = 1u, AIR_LAMP_LED = 2u, AIR_SHOP = 3u, AIR_LOBBY = 4u, AIR_FLAME = 5u;
float skyLightRange(float power) { return min(sqrt(power * SKY_GAIN / 0.04), SKY_RANGE_MAX); }
const uint  WORLD_SEED      = 0xC1B3A7u;

const uint MAT_ROAD = 0u, MAT_SIDEWALK = 1u, MAT_CURB = 2u, MAT_LOT = 3u,
           MAT_FACADE = 4u, MAT_ROOF = 5u, MAT_NEON = 6u, MAT_METAL = 7u,
           MAT_EMISSIVE = 8u, MAT_SCREEN = 9u, MAT_LEDSTRIP = 10u, MAT_DOOR = 11u,
           MAT_INTERIOR = 12u, MAT_LITTER = 13u, MAT_ARCOLOGY = 14u, MAT_INDUSTRY = 15u;
// Litter kinds (src/core/common.h)
const uint LK_SHADOW = 0u, LK_NEWS = 1u, LK_FLYER = 2u, LK_SCRAP = 3u, LK_CARDBOARD = 4u, LK_WAD = 5u, LK_CAN = 6u,
           LK_BOTTLE = 7u, LK_BUTT = 8u, LK_CUP = 9u, LK_BOX = 10u, LK_BAG = 11u, LK_BINBAG = 12u;
// Closer than this, a lobby's glass shows its real interior (src/core/common.h INTERIOR_DISTANCE).
const float REAL_GLASS_DIST = 150.0;

// Authored PBR materials (src/render/textures.c): maps at material * PBR_MAPS_PER_MATERIAL + map.
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
    vec4 weather;    // x rain intensity, y street wetness (src/app/main.c: weather_update), z haze datum (hazeAmount), w the chunks' edge (edgeFog)
    vec4 shelter;    // shelter map: camera-relative centre x, z; half-width
    vec4 shadow;     // ray-traced shadows: x frame number, y SHADOWS_ON | SHADOWS_DEBUG, zw where sample 0 lies in a pixel
    mat4 prevViewProj;   // the previous frame's viewProj, taking this frame's camera-relative positions
    vec4 sky;        // x the city's light in the air round the camera (0 countryside .. 1 city), y how many cities[], z cloud cover over the camera (src/app/weather.c)
    vec4 cities[CLOUD_CITIES];   // the settlements whose clouds are drawn: camera-relative centre x, z; footprint radius (clouds.h)
} frame;

struct ChunkGPU {
    vec4  origin;    // chunk origin relative to camera
    ivec4 info;      // cx, cz, first light record (-1: none), unused
};
layout(std430, set = 0, binding = 1) readonly buffer Chunks { ChunkGPU chunks[]; };

// Per-chunk light tables (see build_lights in src/city/light_tiles.c): a header record,
// 16 tile words (first << 16 | count), then lights (xyz, w = type | data).
layout(std430, set = 0, binding = 2) readonly buffer Lights { uvec4 lightRec[]; };

/* ---------------- hashing: bit-identical to src/core/hash.h ---------------- */
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

/* Gusts: a slow noise field blowing across the city with the cloud deck's
 * wind, occasional and shared by things near each other, which stir together:
 * 0 still air .. 1 a full gust, at world xz w. Cables swing and litter stirs in
 * them (shaders/city.vert); the stacks' plumes bend over and roil (smoke.frag). */
const vec2 GUST_WIND = vec2(0.94, 0.34);   // clouds.comp WIND, normalised
float gustAt(vec2 w, float t) {
    return smoothstep(0.45, 0.75, vnoise(w / 70.0 - GUST_WIND * t * 0.09, 0x6057u));   // gusts drift at ~6 m/s
}

/* Facade rhythm: must match city_floor_height / city_ground_height in src/city/city.c */
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

// A lit storefront's light (its shop hash picks the colour; src/city/city.c: city_shop_hash).
vec3 shopColor(uint h) { return mix(neonColor(h >> 4), vec3(1.0, 0.82, 0.62), 0.45); }

// Light from the lobbies thrown onto the street and into the rain (about 2900 K,
// between the lobbies' 2700 K and 3000 K panels: city.frag lobbyLight).
const vec3 LOBBY_SPILL = vec3(1.0, 0.66, 0.37);

/* ---------------- ray-traced shadows ----------------
 * src/render/rt.c builds the acceleration structures; shadow_trace.comp traces one
 * ray per light at each pixel's most important lights, shadow_denoise.comp
 * filters them into soft shadows, and city.frag looks each light up
 * (shadowVis), tracing a hard shadow itself wherever the pixel's surface
 * isn't the one the prepass saw. */
const float SHADOW_FADE = 350.0, SHADOW_FAR = 450.0;    // src/core/common.h
const uint  SHADOWS_ON = 1u, SHADOWS_DEBUG = 2u;         // frame.shadow.y (renderer.c)
bool shadowsOn() { return (uint(frame.shadow.y) & SHADOWS_ON) != 0u; }
const int   SHADOW_SLOTS = 8;                            // lights a pixel keeps soft shadows for
const uint  RT_MASK_LITTER = 0x04u;                      // src/render/rt.h
const float LIGHT_NEGLIGIBLE = 0.002;                    // a light this faint shows no shadow worth tracing
// Prepass G-buffer (prepass.frag): x the normal (octEncode), y the draw chunk (PG_CHUNK; MAX_DRAW_CHUNKS
// in src/core/common.h) | these flags; for a lobby surface (PG_LOBBY), bits 12..19 hold its ceiling's
// height above the floor (half metres) and bits 20..27 its own (eighths of a metre).
const uint  PG_LIT = 1u << 30, PG_LITTER = 1u << 29, PG_LOBBY = 1u << 28, PG_CHUNK = 0xFFFu;
// Higher than this above their ground no street lamp or shopfront reaches (cityLights).
const float CITY_LIGHTS_TOP = 32.0;
/* Whether any light in the chunk's table (at record base; -1: none) can reach a
 * surface at height y, `above` its ground: street lamps and shopfronts no
 * higher than CITY_LIGHTS_TOP, a flare's fire as high as its table says
 * (record 5: build_lights in src/city/light_tiles.c). The rest of the skyline
 * skips the light walk, and the shadow passes leave it out. */
bool lightsReach(int base, float y, float above) {
    return base >= 0 && (above <= CITY_LIGHTS_TOP || y <= uintBitsToFloat(lightRec[base + 5].x));
}
/* A lobby is lit by the panels in its ceiling (city.frag: shadeInterior), not
 * by the light table, so its pixels' slots hold the ceiling instead: CEIL_SLOTS
 * rays, one into each quarter of the sky above the surface, under ids no light
 * has (shadow_trace.comp: traceCeiling). */
const int   CEIL_SLOTS = 4;
const uint  CEIL_ID = 0xFFF0u;
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
/* Flares' fire (src/city/mesh_industry.c; shaders/flame.frag marches it). Most
 * flares burn in gouts, as in the opening of Blade Runner: a jet shoots up out
 * of the nozzle, the valve shuts, and the column it fed climbs on and burns
 * away from the bottom up, leaving only a pilot flame until the next. About one
 * in four burns steadily instead. id (0..63) picks the timing. head and tail
 * are the burning column's top and bottom as shares of the gout's reach; power
 * how much light it throws, 0..1. The march, the light tables (cityLights) and
 * the rain mist and clouds (clouds.comp) all call it, so they flare together. */
struct Gout { float head, tail, power; bool steady; };
Gout flameGout(uint id, float t) {
    uint h = hash_combine(id, 0xF1A3E5u);
    Gout g;
    g.steady = hfloat(h) < 0.25;
    float flick = vnoise(vec2(t * 6.0, float(id) * 3.7), 0xF1A4u);
    if (g.steady) { g.head = 0.55 + 0.08 * flick; g.tail = 0.0; g.power = 0.55 + 0.25 * flick; return g; }
    float period = 7.0 + 9.0 * hfloat(hash_combine(h, 1u));
    float s = fract(t / period + hfloat(hash_combine(h, 2u))) * period;   // seconds into this cycle
    const float SHOOT = 0.6, CUT = 1.5, BURN = 2.4;                         // it shoots up, the valve shuts, it burns away
    if (s < SHOOT) {
        float k = s / SHOOT;
        g.head = 0.1 + 0.9 * (1.0 - (1.0 - k) * (1.0 - k)); g.tail = 0.0; g.power = 0.3 + 0.7 * k;
    } else if (s < CUT) {
        g.head = 1.0 + 0.1 * (s - SHOOT) / (CUT - SHOOT); g.tail = 0.0; g.power = 1.0;
    } else if (s < CUT + BURN) {
        float k = (s - CUT) / BURN;
        g.head = 1.1 + 0.35 * k;            // still climbing
        g.tail = g.head * pow(k, 1.5);      // burning away from the bottom up
        g.power = (1.0 - k) * (1.0 - k);    // dark as the last of it burns out (the pilot's glimmer is the pilot phase's)
    } else {
        g.head = 0.1; g.tail = 0.0; g.power = 0.06;   // the pilot
    }
    g.power *= 0.85 + 0.3 * flick;
    return g;
}

// Fire's colour by its temperature: 0 a dull red (about 900 K) .. 1 yellow-white (about 1900 K).
vec3 fireColor(float T) {
    T = clamp(T, 0.0, 1.0);
    vec3 c = mix(vec3(0.45, 0.03, 0.0), vec3(1.0, 0.28, 0.02), smoothstep(0.0, 0.35, T));
    c = mix(c, vec3(1.0, 0.62, 0.14), smoothstep(0.3, 0.7, T));
    return mix(c, vec3(1.0, 0.92, 0.72), smoothstep(0.7, 1.0, T));
}

// A flare's light (LE_FLAME in src/city/light_tiles.c: lamp class, bits 2..7 its
// timing id, 8..23 its gout's reach in 1/16 m): how far it reaches, and its falloff.
const float FLAME_REACH = 80.0;
bool isFlame(uint w) { return (w & 3u) < 2u && ((w >> 26) & 3u) == 3u; }
float flameFalloff(vec3 l) {
    float d2 = dot(l, l);
    return 260.0 / (d2 + 30.0) * smoothstep(FLAME_REACH, FLAME_REACH * 0.45, sqrt(d2));
}
// Its light now: from the middle of the burning column, as bright as the gout.
vec3 flameLightPos(vec3 lp, uint w, float t) {
    Gout g = flameGout((w >> 2) & 63u, t);
    return lp + vec3(0.0, 0.4 * (g.head + g.tail) * float((w >> 8) & 0xFFFFu) / 16.0, 0.0);
}
vec3 flameLightColor(uint w, float t) {
    Gout g = flameGout((w >> 2) & 63u, t);
    return fireColor(0.5 + 0.25 * g.power) * g.power;
}

// The ground under light record w's lamp at lp: a street lamp's light hangs LAMP_HEIGHT - 0.4
// above it (build_lights in src/city/light_tiles.c), an arcology's light holds its own height.
float lampGround(vec3 lp, uint w) {
    return ((w >> 26) & 3u) == 0u ? lp.y - (LAMP_HEIGHT - 0.4) : lp.y - float((w >> 8) & 0xFFFFu) / 16.0;
}

/* A point on light record w's emitter (lp chunk-local) as seen from p, from
 * xi in [0,1)^3, and the emitter's size (for penumbra estimates). A street
 * lamp's beam glows through the diffuser under the head, a disc just below it,
 * and its spill rises from the pool of light beneath it: xi.z picks one, by
 * the share each has at p. Arcology lights are broader (LE_* in src/city/light_tiles.c).
 * A shop or lobby light stands 1 m out from its pane, which glows over the
 * bay's width from knee height to over the door. */
vec3 emitterPoint(vec3 lp, uint w, vec3 xi, vec3 p, out float size) {
    if (isFlame(w)) {   // a ball of fire a third of the way up a gout
        size = 3.0;
        return lp + vec3(0.0, 0.35 * float((w >> 8) & 0xFFFFu) / 16.0, 0.0) + (xi - 0.5) * 3.0;
    }
    if ((w & 3u) < 2u) {
        uint cls = (w >> 26) & 3u;
        float R = cls == 0u ? 0.25 : cls == 1u ? 0.4 : 1.5;
        vec3 c = lp;
        if (xi.z >= lampBeamShare(lp, p)) { R = POOL_RADIUS; c.y = lampGround(lp, w) + POOL_HEIGHT; }
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
// in src/city/light_tiles.c) stand for the cold light falling down the canyon from its
// glazed crown, a fill from far above rather than a light at the point they
// mark, so they light the galleries unshadowed.
bool castsShadow(uint w) { return (w & 3u) >= 2u || ((w >> 26) & 3u) != 2u; }

// The middle of the emitter, as seen from p: where city.frag aims a hard shadow ray.
vec3 emitterCentre(vec3 lp, uint w, vec3 p) {
    if (isFlame(w)) return lp + vec3(0.0, 0.35 * float((w >> 8) & 0xFFFFu) / 16.0, 0.0);
    if ((w & 3u) < 2u) return lampBeamShare(lp, p) >= 0.5 ? lp : vec3(lp.x, lampGround(lp, w) + POOL_HEIGHT, lp.z);
    float ang = float((w >> 16) & 255u) * (6.2831853 / 256.0);
    return lp - vec3(cos(ang), 0.0, sin(ang)) * 0.8 + vec3(0.0, 0.35, 0.0);
}

// A light's identity in a pixel's shadow slots: the quarter-metre cell it
// stands in, counted in whole cells from its chunk, so the same lamp listed in
// two chunks' tables (and in successive frames) has the same id. 0 = empty.
uint lightId(ivec2 chunk, vec3 lp, uint w) {
    uvec3 q = uvec3(ivec3(floor(lp * 4.0)) + ivec3(chunk.x * 440, 0, chunk.y * 440));   // 440 = CHUNK_SIZE * 4
    uint h = q.x * 73856093u ^ q.y * 19349663u ^ q.z * 83492791u ^ (w & 3u) * 2654435761u;
    return clamp((h ^ (h >> 16)) & 0xFFFFu, 1u, CEIL_ID - 1u);
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

/* Failing street lamps (lamp_fault in src/city/streets.c): fault & 3 is the state,
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
    if (isFlame(w)) return flameFalloff(l) * luma(fireColor(0.6)) * (max(ndl, 0.0) + 0.05 + glint);
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

/* ---------------- the planet ----------------
 * The world is a sphere of PLANET_RADIUS and world X/Z a chart of it (see
 * src/terrain/terrain.h, "The planet"). The terrain, the distant proxies and
 * the planet's patches are drawn curving away from the camera; the city's
 * chunks are not (within VIEW_DISTANCE the ground falls 0.28 m at most, a
 * tenth of a pixel there), so their shadows and reflections stay exact. */

// A point at camera-relative chart offset d (m) and world height h, in the
// camera's local frame. terrain_bend (src/terrain/terrain_world.c) is its C
// mirror and explains the arithmetic.
vec3 planetBend(vec2 d, float h) {
    const float R = PLANET_RADIUS;
    float s = length(d), th = s / R;
    float sinc = th > 1e-3 ? sin(th) / th : 1.0 - th * th / 6.0;
    float k = (1.0 + h / R) * sinc, hs = sin(0.5 * th);
    return vec3(d.x * k, (h - frame.camPos.y) - (R + h) * 2.0 * hs * hs, d.y * k);
}

// A direction at d turned as planetBend turns the ground there: up tips away
// from the camera by the angle d subtends at the planet's centre.
vec3 planetTilt(vec2 d, vec3 n) {
    float s = length(d);
    if (s < 1.0) return n;
    vec2 r = d / s;
    float th = s / PLANET_RADIUS, c = cos(th), sn = sin(th);
    float nr = dot(n.xz, r);
    vec2 nt = n.xz - nr * r;
    float nr2 = nr * c + n.y * sn;
    return vec3(nt.x + nr2 * r.x, n.y * c - nr * sn, nt.y + nr2 * r.y);
}

// How far the ground at camera-relative horizontal offset d has fallen below
// the camera's level (m): what to add back to a camera-frame height for the
// height above the ground beneath.
float planetDrop(vec2 d) { return dot(d, d) * (0.5 / PLANET_RADIUS); }

/* ---------------- the weather ----------------
 * The rain clouds gather over the cities (src/core/clouds.h), and under them
 * the city lights the night: the deck's underside and the smog glow
 * sodium-orange. Out in the countryside the sky is clear. The moon lights
 * the land there, faintly, and the stars are out (sky.frag). */

// How far camera-relative ground point rel lies beyond the footprint of the
// nearest city drawn (m; < 0 inside it).
float cityEdge(vec2 rel) {
    float e = 1e9;
    for (int i = 0; i < int(frame.sky.y); ++i) e = min(e, length(rel - frame.cities[i].xy) - frame.cities[i].z);
    return e;
}

// The cities' cloud over rel, edge = cityEdge(rel) there: 1 over a city and
// its border, falling to 0 over the countryside along a ragged line. The C
// mirror, at the camera, is in src/app/weather.c.
float cloudCover(vec2 rel, float edge) {
    if (edge < CLOUD_BORDER - CLOUD_RAGGED) return 1.0;
    if (edge > CLOUD_BORDER + CLOUD_FADE + CLOUD_RAGGED) return 0.0;
    vec2 w = frame.camPos.xz + rel;
    float e = edge + CLOUD_RAGGED * (2.0 * fbm(w / 5000.0, 0xC1A0u) - 1.0);
    return 1.0 - smoothstep(CLOUD_BORDER, CLOUD_BORDER + CLOUD_FADE, e);
}

// How much of a city's light reaches the air and the cloud over ground
// `edge` beyond the city (cityEdge): all of it over the city, none past the
// border's outer line.
float cityLight(float edge) { return 1.0 - smoothstep(0.0, CLOUD_BORDER, edge); }

// The moon, low in the sky and so far off that its light falls the same way
// everywhere. It stays put in the camera's frame for now, wherever on the
// planet the camera is.
const vec3  MOON_DIR = vec3(0.8547, 0.4223, -0.3017);   // unit: towards the moon
const float MOON_RADIUS = 0.014;                         // its disc's angular radius (rad), about three times the real moon's
const vec3  MOON_LIGHT = vec3(0.50, 0.58, 0.80) * 0.60;  // the light it throws on ground square to it
const vec3  STARLIGHT = vec3(0.0016, 0.0020, 0.0032);    // the clear night sky's own glow, from above

/* ---------------- atmosphere ----------------
 * A smoggy night: warm city-lit haze near the horizon, cold dark zenith.
 * Out in the countryside (frame.sky.x towards 0) the haze is thin moonlit
 * mist instead, with the moon's aureole round it under a clear sky. Fog uses
 * the same colour as the sky at the horizon so the finite view distance
 * dissolves seamlessly into the haze. */
vec3 hazeColor(vec3 dir) {
    float y = dir.y, g = frame.sky.x;
    vec3 low  = mix(vec3(0.015, 0.018, 0.027), vec3(0.095, 0.050, 0.036), g);     // moonlit mist .. sodium-lit smog
    vec3 mid  = mix(vec3(0.0075, 0.009, 0.016), vec3(0.034, 0.026, 0.040), g);
    vec3 high = mix(vec3(0.0025, 0.0032, 0.007), vec3(0.005, 0.007, 0.014), g);
    vec3 c = mix(low, mid, smoothstep(-0.02, 0.22, y));
    c = mix(c, high, smoothstep(0.18, 0.75, y));
    float m = max(dot(dir, MOON_DIR), 0.0);
    return c + MOON_LIGHT * (0.03 * pow(m, 400.0) + 0.005 * pow(m, 12.0)) * (1.0 - frame.sky.z);
}

// The mean of exp(-h / H) along a straight sight line from height h0 to h1.
float hazeMean(float h0, float h1, float H) {
    float dh = h1 - h0;
    return abs(dh) > 1.0 ? H * (exp(-h0 / H) - exp(-h1 / H)) / dh : exp(-h0 / H);
}

// How much of the light from rel (camera-relative, dist away) the haze takes,
// for air of the given density (per metre) at street level, thinning with
// height: three quarters of it within HAZE_LOW_H or so of the ground, the
// rest within HAZE_HIGH_H, clear air above (distances.h). Heights count from
// the haze datum (frame.weather.z): the ground of the settlement, not y = 0,
// so a town on raised ground lies in its own street-level haze. They are
// heights above the planet's sphere, so ground bent down out of sight stays
// in the street's haze. The C mirror for the planet's patches is
// planet_haze (src/render/planet_gpu.c).
float hazeAmount(vec3 rel, float dist, float density) {
    float h0 = max(frame.camPos.y - frame.weather.z, 0.0);
    float h1 = max(frame.camPos.y + rel.y + planetDrop(rel.xz) - frame.weather.z, 0.0);
    float low = hazeMean(h0, h1, HAZE_LOW_H), high = hazeMean(h0, h1, HAZE_HIGH_H);
    return 1.0 - exp(-dist * density * (0.25 * high + 0.75 * low));
}

// The chunks' edge (frame.weather.w: VIEW_DISTANCE, or nearer while the
// chunk pool is full; src/app/main.c), where a chunk replaces its distant
// proxies with a pop: a band of haze that takes EDGE_FOG of what is left
// there, rising over EDGE_FOG_WIDTH before it and held beyond, so the pop
// happens behind it. By distance alone, not thinned with height as
// hazeAmount is, so it hides the handover from the air too.
const float EDGE_FOG = 0.7, EDGE_FOG_WIDTH = 500.0;
float edgeFog(float haze, float dist) {
    float band = EDGE_FOG * smoothstep(frame.weather.w - EDGE_FOG_WIDTH, frame.weather.w, dist);
    return 1.0 - (1.0 - haze) * (1.0 - band);
}

// The city's haze: a street-level mean free path of CITY_HAZE metres, which
// rain shortens, and the band at the chunks' edge. Past it the city goes on
// as its distant proxies (src/render/proxy_gpu.c).
const float CITY_HAZE = 1100.0;
float cityHaze(vec3 rel, float dist) {
    float density = (1.0 + 1.2 * frame.weather.x) / CITY_HAZE;
    return hazeAmount(rel, dist, density);
}
float fogAmount(vec3 rel, float dist) { return edgeFog(cityHaze(rel, dist), dist); }

// The haze's density at street level (per metre) in the air round the camera:
// the city's (CITY_HAZE) near a city, the land's clearer air (TERRAIN_HAZE)
// out in the countryside.
float airDensity() { return (1.0 + 1.2 * frame.weather.x) / mix(TERRAIN_HAZE, CITY_HAZE, frame.sky.x); }

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
