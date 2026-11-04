#ifndef GRASS_SURFACE_GLSL
#define GRASS_SURFACE_GLSL
#include "grass/frame.glsl"

/* The ground the terrain draws, at any camera-relative x, z within its
 * windows, for the grass to stand on: the clipmap's samples (terrain.vert's
 * binding) read as terrain.vert reads them, each vertex blended into its
 * parent as there, and the vertices interpolated across the triangle the
 * point lies in (cells split along the (i, j)-(i + 1, j + 1) diagonal,
 * src/render/terrain_gpu.c). So a root sits on the surface drawn, and the
 * rules read the cover and normal the land's debug views show there. */

layout(std430, set = 0, binding = 1) readonly buffer Samples { uint samples[]; };

struct GrassGround {
    float h;       // world Y
    vec3  n;       // unit normal
    vec4  ca, cb;  // the cover (ground.glsl Cover)
    float urban;
    int   level;
};

const int GRASS_CN = TERRAIN_CLIP_N, GRASS_CW = TERRAIN_CLIP_W;

int grassSlot(int level, ivec2 s) { return (((level * GRASS_CN) + (s.y & (GRASS_CN - 1))) * GRASS_CN + (s.x & (GRASS_CN - 1))) * 7; }
float grassWord(int i) { return uintBitsToFloat(samples[i]); }
vec3 grassNormalAt(int base) {
    vec2 n = vec2(grassWord(base + 2), grassWord(base + 3));
    return vec3(n.x, sqrt(max(1.0 - dot(n, n), 0.0)), n.y);
}

struct GrassVertex { float h; vec3 n; vec4 ca, cb; float urban; };

// Vertex g of drawn level l, as terrain.vert makes it.
GrassVertex grassVertex(int l, ivec2 g) {
    GrassLevel L = gf.lv[l];
    int base = grassSlot(L.d.x, L.c.xy + g);
    GrassVertex v;
    float h = grassWord(base), hc = grassWord(base + 1);
    precise vec2 mxz = L.a.zw + (L.b.xy + vec2(g) * L.b.z);
    const float END = float((GRASS_CW - 1) / 2 - 1);
    float d = max(abs(mxz.x), abs(mxz.y)) / L.b.z;
    float morph = clamp((d - (END - float(TERRAIN_CLIP_MORPH))) / float(TERRAIN_CLIP_MORPH), 0.0, 1.0);
    morph = max(morph, L.b.w);
    vec3 n = grassNormalAt(base);
    vec4 ca = unpackUnorm4x8(samples[base + 5]), cb = unpackUnorm4x8(samples[base + 6]);
    if (morph > 0.0 && L.d.y == 0) {
        ivec2 o = g & 1, p = L.c.zw + (g >> 1);
        int pb = grassSlot(L.d.x + 1, p);
        vec3 pn = grassNormalAt(pb);
        vec4 pa = unpackUnorm4x8(samples[pb + 5]), pq = unpackUnorm4x8(samples[pb + 6]);
        if (o.x != 0 || o.y != 0) {
            int pb2 = grassSlot(L.d.x + 1, p + o);
            pn += grassNormalAt(pb2);
            pa = 0.5 * (pa + unpackUnorm4x8(samples[pb2 + 5]));
            pq = 0.5 * (pq + unpackUnorm4x8(samples[pb2 + 6]));
        }
        n = normalize(mix(n, normalize(pn), morph));
        ca = mix(ca, pa, morph);
        cb = mix(cb, pq, morph);
    }
    v.h = h + morph * (hc - h);
    v.n = n;
    v.ca = ca;
    v.cb = cb;
    v.urban = grassWord(base + 4);
    return v;
}

// The drawn ground at camera-relative rel: the finest drawn level whose
// window holds it (each finer window fills its parent's hole). False outside
// every window, or when the terrain draws nothing.
bool grassSurface(vec2 rel, out GrassGround g) {
    g.h = 0.0; g.n = vec3(0.0, 1.0, 0.0); g.ca = vec4(0.0); g.cb = vec4(0.0); g.urban = 1.0; g.level = -1;
    for (int l = gf.info.x; l < TERRAIN_CLIP_LEVELS; ++l) {
        GrassLevel L = gf.lv[l];
        vec2 q = (rel - L.a.xy - L.b.xy) / L.b.z;   // samples from the window's corner
        if (q.x < 0.0 || q.y < 0.0 || q.x > float(GRASS_CW - 1) || q.y > float(GRASS_CW - 1)) continue;
        ivec2 c = min(ivec2(floor(q)), ivec2(GRASS_CW - 2));
        vec2 f = q - vec2(c);
        GrassVertex v0 = grassVertex(l, c), v2 = grassVertex(l, c + 1), v1;
        vec3 w;
        if (f.x >= f.y) { v1 = grassVertex(l, c + ivec2(1, 0)); w = vec3(1.0 - f.x, f.y, f.x - f.y); }
        else            { v1 = grassVertex(l, c + ivec2(0, 1)); w = vec3(1.0 - f.y, f.x, f.y - f.x); }
        // w: v0, v2, v1
        g.h = w.x * v0.h + w.y * v2.h + w.z * v1.h;
        g.n = normalize(w.x * v0.n + w.y * v2.n + w.z * v1.n);
        g.ca = w.x * v0.ca + w.y * v2.ca + w.z * v1.ca;
        g.cb = w.x * v0.cb + w.y * v2.cb + w.z * v1.cb;
        g.urban = w.x * v0.urban + w.y * v2.urban + w.z * v1.urban;
        g.level = l;
        return true;
    }
    return false;
}

#endif
