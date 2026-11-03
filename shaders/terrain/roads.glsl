#ifndef ROADS_GLSL
#define ROADS_GLSL
#include "ground.glsl"

/* The roads between the settlements (src/plan/plan_roads.c), painted onto the
 * clipmap's ground, which already follows their graded surfaces
 * (plan_road_ground). Their centre lines' segments round the camera come in
 * one buffer laid out by src/render/road_gpu.c: a header, a grid of cells
 * into a list of segments, the segments. Freeways (PLAN_ROAD_FREEWAY, half
 * 16 m): a concrete median barrier, three lanes each way between solid edge
 * lines, dashed between, hard shoulders, and sodium lamps down the median
 * every LAMP_SPACING whose pools light the carriageways and the land beside
 * them. Trunk roads (half 6 m): a lane each way about a dashed centre line,
 * solid edge lines, gravel shoulders, unlit. Both are laid in the city's own
 * asphalt (road.json, as its carriageways), and beyond the paved edge a strip
 * of gravel (roof_gravel.json) on the level verge gives way to the land's
 * cover. On a bridge's span the road is the deck the city's chunks draw over
 * the water (src/city/road_bridges.c), out to VIEW_DISTANCE; past that the
 * paint stands in for it on the ground below: paved to its edge, as the deck,
 * and nothing beyond. */

layout(std430, set = 0, binding = 2) readonly buffer Roads { uint roadData[]; };

const float FREEWAY_LAMP_SPACING = 60.0;   // m
const vec3  ROAD_SODIUM = vec3(1.0, 0.55, 0.22);

// How far beyond a road's paved edge it is shaded (m): a trunk road's verge,
// and beside a freeway its lamps' light on the land, faded out from
// FREEWAY_LIGHT_FADE. road_gpu.c files each segment in the cells within
// ROAD_CELL_REACH of it, to cover the farther.
const float FREEWAY_REACH = 14.0, TRUNK_REACH = 3.0, FREEWAY_LIGHT_FADE = 6.0;

// A trunk road's asphalt runs this far either side of its centre line (just
// past the edge line), its gravel shoulders beyond to the paved edge.
const float TRUNK_ASPHALT = 4.0;
// The gravel runs on this far beyond the paved edge, give or take
// ROAD_VERGE_VARY along the road, then gives way to the cover over ROAD_VERGE_SOFT.
const float ROAD_VERGE = 0.8, ROAD_VERGE_VARY = 0.4, ROAD_VERGE_SOFT = 0.5;

// Drawn about these colours (linear albedo): the gravel, dusty and a little
// lighter than the asphalt (whose colour is its own map's, times its
// albedo_scale, as the city's); the median's concrete (as the city's decks').
const vec3 ROAD_GRAVEL = vec3(0.070, 0.066, 0.058);
const vec3 ROAD_BARRIER = vec3(0.11);

// The road's textures fade into their flat colours as a pixel comes to span
// this many metres: by then their mips have all but reached their means.
const float ROAD_TEX_PX0 = 0.3, ROAD_TEX_PX1 = 0.6;

float roadF(uint i) { return uintBitsToFloat(roadData[i]); }

struct RoadHit {
    float off;    // signed distance from the centre line (m)
    float s;      // distance along the road (m)
    float hw;     // half the paved width (m)
    int   cls;    // 1 trunk, 2 freeway
    bool  bridge; // on a bridge's span
    vec2  across; // the world's x, z direction of increasing off
};

const int ROAD_SEG_BRIDGE = 4;   // in a segment's class (src/render/road_gpu.c)

// The road nearest rel (camera-relative x, z) whose paved edge is within its
// reach (FREEWAY_REACH, TRUNK_REACH) of it, if any.
bool roadNear(vec2 rel, out RoadHit h) {
    h.off = 1e9; h.s = 0.0; h.hw = 0.0; h.cls = 0; h.bridge = false; h.across = vec2(1.0, 0.0);
    vec2 p = rel - vec2(roadF(0u), roadF(1u));
    float cell = roadF(2u);
    int dim = int(roadData[3]);
    if (dim <= 0) return false;
    ivec2 c = ivec2(floor(p / cell));
    if (c.x < 0 || c.y < 0 || c.x >= dim || c.y >= dim) return false;
    uint ci = 8u + 2u * uint(c.y * dim + c.x);
    uint start = roadData[ci], count = roadData[ci + 1u], listAt = roadData[5], segAt = roadData[6];
    float best = 1e9;
    for (uint k = 0u; k < count; ++k) {
        uint b = segAt + 8u * roadData[listAt + start + k];
        vec2 a = vec2(roadF(b), roadF(b + 1u)), e = vec2(roadF(b + 2u), roadF(b + 3u)) - a;
        float l2 = max(dot(e, e), 1e-6), t = clamp(dot(p - a, e) / l2, 0.0, 1.0);
        vec2 d = p - (a + e * t);
        float hw = roadF(b + 6u), edge = length(d) - hw;
        if (edge < best) {
            best = edge;
            h.off = (e.x * d.y - e.y * d.x) * inversesqrt(l2);
            h.s = roadF(b + 4u) + t * roadF(b + 5u);
            h.hw = hw;
            h.across = vec2(-e.y, e.x) * inversesqrt(l2);
            int c = int(roadF(b + 7u) + 0.5);
            h.cls = c & 3;
            h.bridge = (c & ROAD_SEG_BRIDGE) != 0;
        }
    }
    // under a deck the chunks draw, the ground is the water's
    if (h.bridge && dot(rel, rel) < VIEW_DISTANCE * VIEW_DISTANCE) return false;
    return best < (h.cls == 2 ? FREEWAY_REACH : TRUNK_REACH);
}

// A painted line `w` wide centred `at` across, anti-aliased over a pixel's
// width px, and faded to its share of the pixel once it is narrower.
float roadLine(float a, float at, float w, float px) {
    float cover = clamp(0.5 + (0.5 * max(w, px) - abs(a - at)) / px, 0.0, 1.0);
    return cover * min(w / px, 1.0);
}

// Dashes `on` long every `period` along the road, anti-aliased over pxs;
// their average once a pixel spans a dash.
float roadDash(float s, float on, float period, float pxs) {
    float f = mod(s, period);
    float c = clamp((on - f) / pxs + 0.5, 0.0, 1.0) * clamp(f / pxs + 0.5, 0.0, 1.0);
    return mix(c, on / period, clamp(2.0 * pxs / period, 0.0, 1.0));
}

// Where the asphalt ends, from the centre line: the paved edge, but for a
// trunk road's gravel shoulders (not on a bridge's deck); ragged a little
// along the road (wp: the world's x, z) where gravel meets it.
float roadAsphaltEdge(RoadHit r, vec2 wp) {
    if (r.bridge) return r.hw;
    float e = r.cls == 2 ? r.hw : TRUNK_ASPHALT;
    return e + 0.06 * (vnoise(wp / 0.6, 0x5A31u) - 0.5) + 0.05 * (vnoise(wp / 2.3, 0x5A32u) - 0.5);
}

// Whether the asphalt covers the whole pixel (px its width across the road):
// the land under it needn't be textured.
bool roadPaved(RoadHit r, float px) { return abs(r.off) + 0.5 * px < (r.bridge || r.cls == 2 ? r.hw : TRUNK_ASPHALT) - 0.06; }

// Material m mapped in the world's x and z (q, wrapped as the city's, so it
// runs on unbroken from a bridge's deck; +x image right, -z image up; gx, gy
// q's screen derivatives), drawn about colour (its map's mean drawn as that),
// the normal N bent by its normal map into n; as tex, from 1 to 0, falls, the
// flat colour and N. Its height too, in its map's spreads (pbrRelief).
vec3 roadSurface(int m, vec2 q, vec2 gx, vec2 gy, vec3 colour, float tex, vec3 N, out vec3 n, out float relief) {
    n = N; relief = 0.0;
    if (tex <= 0.0 || !pbrLoaded(m)) return colour;
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec2 t = pbrTile(m), st = q / t;
    gx /= t; gy /= t;
    vec3 a = textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb / max(pbrAlbedoMean(m), vec3(1e-3));
    vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    relief = tex * pbrRelief(m, textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).b);
    n = normalize(N + tex * (vec3(tn.x, 0.0, -tn.y)) / max(tn.z, 0.2));
    return colour * mix(vec3(1.0), a, tex);
}

// What a lane's wear does to the asphalt at x across it from its centre (m):
// darker down the middle where oil drips, lighter in the polished wheel
// paths; their mean across the lane once a pixel spans px of it.
float laneWear(float x, float px) {
    float oil = exp(-x * x / (2.0 * 0.3 * 0.3));
    float wx = abs(x) - 0.9, paths = exp(-wx * wx / (2.0 * 0.25 * 0.25));
    float f = 1.0 - 0.10 * oil + 0.06 * paths;
    return mix(f, 1.0 - 0.10 * 0.206 + 0.06 * 0.343, clamp(px / 0.8, 0.0, 1.0));
}

// The road r at rel, over the land there: its albedo land (groundAlbedo, the
// normal nLand bent by its textures) and the ground's own normal n. l: where
// the textures are mapped (landPoint); urban plan_urban there; px and pxs the
// pixel's width across and along the road (taken in uniform control flow).
// The land lit, before the haze.
vec3 shadeRoad(vec3 land, vec3 nLand, vec3 n, vec3 rel, LandPoint l, float urban, RoadHit r, float px, float pxs) {
    vec2 wp = frame.camPos.xz + rel.xz, q = l.p.xz, gx = l.dpx.xz, gy = l.dpy.xz;
    float a = abs(r.off), side = r.off < 0.0 ? -1.0 : 1.0;
    float wet = frame.weather.y;
    float tex = 1.0 - smoothstep(ROAD_TEX_PX0, ROAD_TEX_PX1, l.px);
    float edge = roadAsphaltEdge(r, wp);
    float asph = clamp(0.5 + (edge - a) / px, 0.0, 1.0);

    // the gravel, beyond the asphalt: out to about ROAD_VERGE past the paved
    // edge, then giving way to the cover where the gravel's height map stands
    // lower than the ground's middle (as the classes meet: landScore)
    float grav = 0.0;
    vec3 gAlb = ROAD_GRAVEL, nGrav = n;
    if (!r.bridge && asph < 1.0) {
        float reach = ROAD_VERGE + ROAD_VERGE_VARY * (2.0 * vnoise(wp / 9.0, 0x5A33u) - 1.0);
        float w = 1.0 - smoothstep(reach - ROAD_VERGE_SOFT, reach + ROAD_VERGE_SOFT, a - r.hw + 0.3 * (vnoise(wp / 0.9, 0x5A34u) - 0.5));
        if (w > 0.0) {
            float h;
            gAlb = roadSurface(PBR_ROOF_GRAVEL, q, gx, gy, ROAD_GRAVEL, tex, n, nGrav, h);
            gAlb *= 1.0 - 0.3 * wet;
            w = clamp(w, 1e-4, 1.0 - 1e-4);
            float byHeight = 1.0 / (1.0 + exp((landScore(1.0 - w, 0.0) - landScore(w, h)) / LAND_BLEND));
            grav = w >= 1.0 - 1e-4 ? 1.0 : mix(w, byHeight, tex);
        }
    }

    // the asphalt, its paint, the median's barrier
    vec3 alb = vec3(0.0), nA = n;
    float paint = 0.0, barrier = 0.0;
    if (asph > 0.0) {
        float h;
        vec3 colour = pbrLoaded(PBR_ROAD) ? pbrAlbedoMean(PBR_ROAD) * pbrAlbedoScale(PBR_ROAD) : vec3(0.032);
        alb = roadSurface(PBR_ROAD, q, gx, gy, colour, tex, n, nA, h);
        if (r.cls == 2) {
            barrier = roadLine(a, 0.0, 1.6, px);
            paint += roadLine(a, 1.6, 0.2, px) + roadLine(a, 12.55, 0.2, px);
            paint += (roadLine(a, 5.25, 0.15, px) + roadLine(a, 8.9, 0.15, px)) * roadDash(r.s, 3.0, 12.0, pxs);
            if (a > 1.6 && a < 12.55) alb *= laneWear(mod(a - 1.6, 3.65) - 1.825, px);
            alb *= a > 12.6 ? 1.25 : 1.0;   // the hard shoulders' lighter strip
            // grime at the barrier's foot
            alb *= 1.0 - 0.3 * exp(-(a - 0.85) * (a - 0.85) / (2.0 * 0.12 * 0.12)) * (1.0 - clamp(px / 0.5, 0.0, 1.0)) * step(0.8, a);
        } else {
            paint += roadLine(a, 0.0, 0.15, px) * roadDash(r.s, 3.0, 12.0, pxs);
            paint += roadLine(a, 3.6, 0.15, px);
            if (a < 3.6) alb *= laneWear(a - 1.8, px);
            alb *= r.bridge && a > 3.7 ? 1.2 : 1.0;   // a deck's shoulders, asphalt
        }
        // resurfaced lengths, each carriageway its own (not on a deck, which the city draws without)
        if (!r.bridge) alb *= 0.93 + 0.14 * hfloat(hash2i(0x5A35u, int(floor(r.s / 180.0)), int(side)));
        alb *= 1.0 - 0.35 * wet;
        paint = clamp(paint, 0.0, 1.0);
        // the barrier: concrete, its top level and its faces sloping down to the carriageways
        if (barrier > 0.0) {
            vec3 nB, bAlb = roadSurface(PBR_CONCRETE, q, gx, gy, ROAD_BARRIER, tex, n, nB, h);
            float face = smoothstep(0.2, 0.3, a) * (1.0 - clamp(px / 0.6, 0.0, 1.0));
            nB = normalize(nB + vec3(r.across.x, 0.0, r.across.y) * side * 0.9 * face);
            alb = mix(alb, bAlb, barrier);
            nA = normalize(mix(nA, nB, barrier));
        }
        // the paint over the asphalt, taking a little of its grain, flattening its relief
        vec3 pAlb = vec3(0.45 * (1.0 - 0.3 * wet)) * mix(vec3(1.0), alb / max(colour, vec3(1e-3)), 0.25);
        alb = mix(alb, pAlb, paint * (1.0 - barrier));
        nA = normalize(mix(nA, n, 0.7 * paint * (1.0 - barrier)));
    }

    // the light: the ground's, and beside a freeway its median's lamps, a pool
    // round each, longer along the road, that falls on the land beside it too
    // (not off a bridge's deck)
    float cov = 1.0 - asph, gv = cov * grav;
    vec3 nl = normalize(asph * nA + gv * nGrav + (cov - gv) * nLand);
    vec3 light = groundLight(rel, nl);
    float pool = 0.0;
    if (r.cls == 2) {
        float ds = r.s - FREEWAY_LAMP_SPACING * floor(r.s / FREEWAY_LAMP_SPACING + 0.5);
        pool = exp(-ds * ds / (2.0 * 14.0 * 14.0) - a * a / (2.0 * 11.0 * 11.0));
        // seen from afar a pixel spans several lamps: their mean
        float mean = 2.5066 * 14.0 / FREEWAY_LAMP_SPACING * exp(-a * a / (2.0 * 11.0 * 11.0));
        pool = mix(pool, mean, clamp(pxs / (0.5 * FREEWAY_LAMP_SPACING), 0.0, 1.0));
        pool *= r.bridge ? asph : 1.0 - smoothstep(r.hw + FREEWAY_LIGHT_FADE, r.hw + FREEWAY_REACH, a);
    }
    light += ROAD_SODIUM * 1.4 * pool;

    vec3 ground = mix(land, gAlb, grav);
    vec3 col = groundUrban(ground * light, urban);
    // the wet asphalt's sheen under the lamps
    vec3 road = alb * light + ROAD_SODIUM * 0.25 * wet * pool;
    return mix(col, road, asph);
}

#endif
