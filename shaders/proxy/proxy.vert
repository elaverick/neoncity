#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* The distant proxies (src/render/proxy_gpu.c): a box round each section of
 * every building the chunks don't draw in full. Box b is vertices 30 b to
 * 30 b + 29 of the draw: its four sides, then its top, two triangles each,
 * wound to face out as the city's quads are (emit_quad). firstInstance
 * picks the tile's origin.
 *
 * Built with HOUSE_PRISM (proxy_house.vert, residential roadmap HM3b): a house
 * row's prism (src/city/city.h, PX_HOUSE) is vertices 42 p to 42 p + 41, its
 * fourteen triangles: the long walls to the eaves, the ends with their gables
 * (a hip's sloping triangles), and the two slopes up to the ridge. */

// ProxyBox (src/city/city.h)
struct ProxyBox {
    vec2 xz;     // centre, relative to the tile's origin
    vec2 y;      // bottom and top, world Y
    uint half_;  // half extents along the box's own x and z (packHalf2x16)
    uint axis;   // its own +x axis in the world
    uint seed;   // the building's
    uint info;   // PX_* fields
};
layout(std430, set = 0, binding = 1) readonly buffer Boxes { ProxyBox boxes[]; };
// Each tile's origin, camera-relative (x, z), and the turn (cos, sin) from
// its settlement's frame into the active one, by its layer and table slot
layout(std430, set = 0, binding = 2) readonly buffer Tiles { vec4 tileOrigin[]; };

layout(location = 0) out vec3 vRel;          // camera-relative position, bent over the planet (planetBend)
layout(location = 1) out vec2 vFace;         // metres along the face from its left corner; height above the building's base
layout(location = 2) flat out vec3 vNrm;
layout(location = 3) flat out uint vSeed;
layout(location = 4) flat out uint vInfo;
layout(location = 5) flat out vec2 vSize;    // the face's width; the box's top above the building's base

// The corners of a face's two triangles: (along, up) from its bottom left.
const vec2 QUAD[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 0), vec2(1, 1), vec2(0, 1));
// The top's corners, in half extents.
const vec2 TOP[6] = vec2[6](vec2(-1, -1), vec2(1, 1), vec2(1, -1), vec2(-1, -1), vec2(-1, 1), vec2(1, 1));

#ifdef HOUSE_PRISM
// The prism's corners in its own frame (x along the ridge, z across, y up): the ground's (G), the eaves' (E)
// and the ridge's ends (R), by the signs of x and z. Each triangle by three of them: 0-3 G(-,-) G(+,-) G(+,+)
// G(-,+); 4-7 E the same; 8 R(-), 9 R(+).
const int PRISM_TRI[42] = int[42](0, 1, 5,  0, 5, 4,     // the wall along -z
                                  2, 3, 7,  2, 7, 6,     // along +z
                                  3, 0, 4,  3, 4, 7,  7, 4, 8,   // the -x end: its wall, its gable or hip
                                  1, 2, 6,  1, 6, 5,  5, 6, 9,   // the +x end
                                  4, 5, 9,  4, 9, 8,     // the slope on the -z side
                                  6, 7, 8,  6, 8, 9);    // on the +z side
vec3 prismCorner(int c, vec2 h, float y0, float y1, float rise, float inset) {
    if (c >= 8) return vec3((c == 8 ? -1.0 : 1.0) * max(h.x - inset, 0.0), y1 + rise, 0.0);
    int k = c & 3;
    vec2 s = vec2(k == 1 || k == 2 ? 1.0 : -1.0, k >= 2 ? 1.0 : -1.0);
    return vec3(s.x * h.x, c >= 4 ? y1 : y0, s.y * h.y);
}

void main() {
    ProxyBox b = boxes[gl_VertexIndex / 42];
    int k = gl_VertexIndex % 42, t = k / 3, j = k % 3;
    vec4 to = tileOrigin[gl_InstanceIndex];
    mat2 turn = mat2(to.z, to.w, -to.w, to.z);
    vec2 h = unpackHalf2x16(b.half_), ax = turn * unpackHalf2x16(b.axis), roof = unpackHalf2x16(b.seed);
    vec2 az = vec2(-ax.y, ax.x);
    vec2 cxz = to.xy + turn * b.xz;
    // the triangle's corners, turned to face out (away from the middle of its base): a gable's triangle is
    // the wall's, a hip's slopes
    vec3 A = prismCorner(PRISM_TRI[t * 3], h, b.y.x, b.y.y, roof.x, roof.y);
    vec3 B = prismCorner(PRISM_TRI[t * 3 + 1], h, b.y.x, b.y.y, roof.x, roof.y);
    vec3 C = prismCorner(PRISM_TRI[t * 3 + 2], h, b.y.x, b.y.y, roof.x, roof.y);
    vec3 nl = cross(B - A, C - A);
    vec3 out_ = (A + B + C) / 3.0 - vec3(0.0, b.y.x, 0.0);
    if (dot(nl, out_) < 0.0) { vec3 s = B; B = C; C = s; nl = -nl; }
    vec3 q = j == 0 ? A : j == 1 ? B : C;
    float len = length(nl);
    nl = len > 1e-6 ? nl / len : vec3(0.0, 1.0, 0.0);   // a flat roof's gable has none: it draws nothing
    vec3 p = vec3(cxz.x + ax.x * q.x + az.x * q.z, q.y, cxz.y + ax.y * q.x + az.y * q.z);
    vec3 n = vec3(ax.x * nl.x + az.x * nl.z, nl.y, ax.y * nl.x + az.y * nl.z);
    float base = b.y.x + 0.5;
    vRel = planetBend(p.xz, p.y);
    vFace = vec2(0.0, p.y - base);
    vNrm = planetTilt(p.xz, n);
    vSeed = hash_combine(floatBitsToUint(b.xz.x), floatBitsToUint(b.xz.y));   // the row's own (it has no seed of its own here)
    vInfo = b.info;
    vSize = vec2(0.0, b.y.y + roof.x - base);
    gl_Position = frame.viewProj * vec4(vRel, 1.0);
}
#else
void main() {
    ProxyBox b = boxes[gl_VertexIndex / 30];
    int k = gl_VertexIndex % 30, face = k / 6;
    vec2 q = QUAD[k % 6];
    vec4 to = tileOrigin[gl_InstanceIndex];
    mat2 turn = mat2(to.z, to.w, -to.w, to.z);   // (x, z) -> (c x - s z, s x + c z)
    vec2 h = unpackHalf2x16(b.half_), ax = turn * unpackHalf2x16(b.axis);
    vec2 az = vec2(-ax.y, ax.x);   // its own +z axis in the world
    vec2 cxz = to.xy + turn * b.xz;
    vec3 c = vec3(cxz.x, 0.0, cxz.y);
    // where the building's heights count from (src/city/proxy.c: a section on the ground reaches 1 m into it)
    float floor0 = float(b.info >> 18 & 2047u) * 0.5;
    float base = floor0 <= SIDEWALK_HEIGHT + 0.01 ? b.y.x + 1.0 : b.y.x - floor0;

    vec3 p, n;
    float u, w;
    if (face < 4) {
        // sides: -x, +x, -z, +z. Along a face is T = (n.z, -n.x), as on the facades.
        vec2 nl = face == 0 ? vec2(-1, 0) : face == 1 ? vec2(1, 0) : face == 2 ? vec2(0, -1) : vec2(0, 1);
        vec2 tl = vec2(nl.y, -nl.x);
        float hn = abs(nl.x) * h.x + abs(nl.y) * h.y, ht = abs(tl.x) * h.x + abs(tl.y) * h.y;
        vec2 o = nl * hn + tl * ht * (2.0 * q.x - 1.0);
        p = c + vec3(ax * o.x + az * o.y, 0.0).xzy;
        p.y = mix(b.y.x, b.y.y, q.y);
        n = vec3(ax * nl.x + az * nl.y, 0.0).xzy;
        u = q.x * 2.0 * ht;
        w = 2.0 * ht;
    } else {
        // the top, as emit_flat winds a floor facing up
        vec2 o = h * TOP[k % 6];
        p = c + vec3(ax * o.x + az * o.y, 0.0).xzy;
        p.y = b.y.y;
        n = vec3(0, 1, 0);
        u = o.x;
        w = 2.0 * h.x;
    }
    vRel = planetBend(p.xz, p.y);
    vFace = vec2(u, p.y - base);
    vNrm = planetTilt(p.xz, n);
    vSeed = b.seed;
    vInfo = b.info;
    vSize = vec2(w, b.y.y - base);
    gl_Position = frame.viewProj * vec4(vRel, 1.0);
}
#endif
