#ifndef ROAD_LOOKUP_GLSL
#define ROAD_LOOKUP_GLSL

/* Finding the road near a point (roads.glsl paints them; the grass's prepare
 * pass, shaders/grass/prepare.comp, keeps off them). Their centre lines'
 * segments round the camera come in one buffer laid out by
 * src/render/road_gpu.c: a header, a grid of cells into a list of segments,
 * the segments. Needs only common.glsl. */

layout(std430, set = 0, binding = 2) readonly buffer Roads { uint roadData[]; };

// How far beyond a road's paved edge it is shaded (m): a trunk road's verge,
// and beside a freeway its lamps' light on the land, faded out from
// FREEWAY_LIGHT_FADE. road_gpu.c files each segment in the cells within
// ROAD_CELL_REACH of it, to cover the farther.
const float FREEWAY_REACH = 14.0, TRUNK_REACH = 3.0, FREEWAY_LIGHT_FADE = 6.0;

// A trunk road's asphalt runs this far either side of its centre line (just
// past the edge line), its gravel shoulders beyond to the paved edge.
const float TRUNK_ASPHALT = 4.0;

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

// How far rel is from the nearest paved edge of the roads filed in its cell
// (m; 1e9 if none): every road whose centre line is within ROAD_CELL_REACH
// (src/render/road_gpu.c, 31 m) of rel is among them.
float roadEdgeNear(vec2 rel) {
    vec2 p = rel - vec2(roadF(0u), roadF(1u));
    float cell = roadF(2u);
    int dim = int(roadData[3]);
    if (dim <= 0) return 1e9;
    ivec2 c = ivec2(floor(p / cell));
    if (c.x < 0 || c.y < 0 || c.x >= dim || c.y >= dim) return 1e9;
    uint ci = 8u + 2u * uint(c.y * dim + c.x);
    uint start = roadData[ci], count = roadData[ci + 1u], listAt = roadData[5], segAt = roadData[6];
    float best = 1e9;
    for (uint k = 0u; k < count; ++k) {
        uint b = segAt + 8u * roadData[listAt + start + k];
        vec2 a = vec2(roadF(b), roadF(b + 1u)), e = vec2(roadF(b + 2u), roadF(b + 3u)) - a;
        float l2 = max(dot(e, e), 1e-6), t = clamp(dot(p - a, e) / l2, 0.0, 1.0);
        best = min(best, length(p - (a + e * t)) - roadF(b + 6u));
    }
    return best;
}

// Whether the asphalt covers the whole pixel (px its width across the road):
// the land under it needn't be textured.
bool roadPaved(RoadHit r, float px) { return abs(r.off) + 0.5 * px < (r.bridge || r.cls == 2 ? r.hw : TRUNK_ASPHALT) - 0.06; }

#endif
