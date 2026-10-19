#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain_clip.h"

/* One ring of the terrain clipmap (src/render/terrain_gpu.c). The grid is
 * TERRAIN_CLIP_W x TERRAIN_CLIP_W vertices, vertex (i, j) = gl_VertexIndex
 * (i + j * W); the index list picks the window or the ring round the finer
 * level's. Each vertex reads its sample from the level's toroidal storage
 * and blends into the parent's surface towards the window's edge, exactly as
 * terrain_clip_morph (src/terrain/terrain_lod.c), so rings meet without cracks
 * or pops. */

// Every level's TERRAIN_CLIP_N^2 slots, five floats each: TerrainSample (src/terrain/terrain.h).
layout(std430, set = 0, binding = 1) readonly buffer Samples { float samples[]; };

// TerrainPush (src/render/renderer_internal.h)
layout(push_constant) uniform Level {
    vec2  anchor;    // camera-relative x, z of the coarsest window's corner: the same for every level
    vec2  morph;     // the same corner relative to the camera of the windows' update (it can trail the frame's)
    vec2  offset;    // this window's corner less the anchor (m; exact in float)
    float spacing;   // metres between samples
    int   level;
    ivec2 slot;      // storage slot of the window's corner sample
    ivec2 pslot;     // the parent's storage slot of the parent sample at the corner
    int   top;       // the coarsest level: no parent
    float fade;      // the least morph: the finest level blending in or out (terrain_fade_morph)
} lv;

layout(location = 0) out vec3  vRel;     // camera-relative position
layout(location = 1) out vec3  vNrm;
layout(location = 2) out float vUrban;   // plan_urban: 1 in the city, 0 beyond its belt

const int N = TERRAIN_CLIP_N, W = TERRAIN_CLIP_W;

int slotBase(int level, ivec2 s) { return (((level * N) + (s.y & (N - 1))) * N + (s.x & (N - 1))) * 5; }

vec3 normalAt(int base) {
    vec2 n = vec2(samples[base + 2], samples[base + 3]);
    return vec3(n.x, sqrt(max(1.0 - dot(n, n), 0.0)), n.y);
}

void main() {
    ivec2 g = ivec2(gl_VertexIndex % W, gl_VertexIndex / W);
    int base = slotBase(lv.level, lv.slot + g);
    float h = samples[base], hc = samples[base + 1];
    // The anchor is shared and the offset exact, so a point two levels share
    // gets the same position from both.
    precise vec2 xz = lv.anchor + (lv.offset + vec2(g) * lv.spacing);

    // terrain_clip_morph: Chebyshev distance from the camera the windows
    // were centred on, in samples (exact at sample positions, as on the CPU)
    precise vec2 mxz = lv.morph + (lv.offset + vec2(g) * lv.spacing);
    const float END = float((W - 1) / 2 - 1);
    float d = max(abs(mxz.x), abs(mxz.y)) / lv.spacing;
    float morph = clamp((d - (END - float(TERRAIN_CLIP_MORPH))) / float(TERRAIN_CLIP_MORPH), 0.0, 1.0);
    morph = max(morph, lv.fade);

    vec3 n = normalAt(base);
    if (morph > 0.0 && lv.top == 0) {
        // the parent's normal here, interpolated as hc is (window corners are even samples)
        ivec2 o = g & 1, p = lv.pslot + (g >> 1);
        vec3 pn = normalAt(slotBase(lv.level + 1, p));
        if (o.x != 0 || o.y != 0) pn += normalAt(slotBase(lv.level + 1, p + o));
        n = normalize(mix(n, normalize(pn), morph));
    }

    vRel = vec3(xz.x, h + morph * (hc - h) - frame.camPos.y, xz.y);
    vNrm = n;
    vUrban = samples[base + 4];
    gl_Position = frame.viewProj * vec4(vRel, 1.0);
}
