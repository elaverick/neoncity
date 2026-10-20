#ifndef ROADS_GLSL
#define ROADS_GLSL
#include "ground.glsl"

/* The roads between the settlements (src/plan/plan_roads.c), painted onto the
 * clipmap's ground, which already follows their graded surfaces
 * (plan_road_ground). Their centre lines' segments round the camera come in
 * one buffer laid out by src/render/road_gpu.c: a header, a grid of cells
 * into a list of segments, the segments. Freeways (PLAN_ROAD_FREEWAY, half
 * 16 m): a concrete median, three lanes each way between solid edge lines,
 * dashed between, hard shoulders, and sodium lamps down the median every
 * LAMP_SPACING whose pools light the carriageways. Trunk roads (half 6 m): a
 * lane each way about a dashed centre line, solid edge lines, unlit. */

layout(std430, set = 0, binding = 2) readonly buffer Roads { uint roadData[]; };

const float FREEWAY_LAMP_SPACING = 60.0;   // m
const vec3  ROAD_SODIUM = vec3(1.0, 0.55, 0.22);

float roadF(uint i) { return uintBitsToFloat(roadData[i]); }

struct RoadHit {
    float off;    // signed distance from the centre line (m)
    float s;      // distance along the road (m)
    float hw;     // half the paved width (m)
    int   cls;    // 1 trunk, 2 freeway
};

// The road nearest rel (camera-relative x, z) whose paved edge is within a
// metre of it, if any.
bool roadNear(vec2 rel, out RoadHit h) {
    h.off = 1e9; h.s = 0.0; h.hw = 0.0; h.cls = 0;
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
            h.cls = int(roadF(b + 7u) + 0.5);
        }
    }
    return best < 1.0;
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

vec3 shadeRoad(vec3 col, vec3 rel, vec3 n, RoadHit r) {
    float a = abs(r.off);
    float px = max(fwidth(r.off), 1e-3), pxs = max(fwidth(r.s), 1e-3);
    float paved = clamp(0.5 + (r.hw - a) / px, 0.0, 1.0);
    if (paved <= 0.0) return col;
    float wet = frame.weather.y;
    // asphalt, darker in the wet, a little lighter on the shoulders
    float albedo = 0.032 * (1.0 - 0.35 * wet);
    float paint = 0.0, barrier = 0.0;
    if (r.cls == 2) {
        barrier = roadLine(a, 0.0, 1.6, px);
        paint += roadLine(a, 1.6, 0.2, px) + roadLine(a, 12.55, 0.2, px);
        paint += (roadLine(a, 5.25, 0.15, px) + roadLine(a, 8.9, 0.15, px)) * roadDash(r.s, 3.0, 12.0, pxs);
        albedo *= a > 12.6 ? 1.25 : 1.0;
    } else {
        paint += roadLine(a, 0.0, 0.15, px) * roadDash(r.s, 3.0, 12.0, pxs);
        paint += roadLine(a, 3.6, 0.15, px);
        albedo *= a > 3.7 ? 1.2 : 1.0;
    }
    paint = clamp(paint, 0.0, 1.0);
    albedo = mix(albedo, 0.11, barrier);
    albedo = mix(albedo, 0.45 * (1.0 - 0.3 * wet), paint * (1.0 - barrier));
    vec3 light = groundLight(rel, n);
    if (r.cls == 2) {
        // the median's lamps: a pool round each, longer along the road
        float ds = r.s - FREEWAY_LAMP_SPACING * floor(r.s / FREEWAY_LAMP_SPACING + 0.5);
        float pool = exp(-ds * ds / (2.0 * 14.0 * 14.0) - a * a / (2.0 * 11.0 * 11.0));
        // seen from afar a pixel spans several lamps: their mean
        float mean = 2.5066 * 14.0 / FREEWAY_LAMP_SPACING * exp(-a * a / (2.0 * 11.0 * 11.0));
        pool = mix(pool, mean, clamp(pxs / (0.5 * FREEWAY_LAMP_SPACING), 0.0, 1.0));
        light += ROAD_SODIUM * 1.4 * pool;
        // the wet road's sheen under them
        col = mix(col, vec3(albedo) * light + ROAD_SODIUM * 0.25 * wet * pool, paved);
        return col;
    }
    return mix(col, vec3(albedo) * light, paved);
}

#endif
