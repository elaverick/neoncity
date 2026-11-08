#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/cave.glsl"

/* A brick of a cave (src/render/cave_gpu.c; the mesh is
 * terrain_cave_brick_mesh's, TerrainCaveVertex). A brick's vertices and
 * indices are its block of the pool, reached through the draw's vertexOffset
 * and firstIndex; its draw is caveDraw[gl_InstanceIndex] (the draw's
 * firstInstance). A vertex's place in the chart, from its system's anchor, is
 * the brick's offset plus its own: both whole numbers of a vertex's unit
 * (1/1024 of a cell), so the sum is exact in float and two bricks that share a
 * vertex put it at the same place. From there into the frame by the chart's
 * map at the anchor, then bent over the planet as the terrain is
 * (planetBend), with the height less the camera's taken from doubles.
 * With CAVE_SHELTER (cave_shelter.vert) into the shelter map instead, so no
 * rain falls under the rock (renderer.c: record_shelter). */

// TerrainCaveVertex, four words: x | y << 16 (int16), z | sky << 16, the normal (octahedral), its height over the ground (float)
layout(std430, set = 0, binding = 1) readonly buffer Pool { uint pool[]; };

layout(location = 0) out vec3 vRel;      // camera-relative position, bent over the planet
layout(location = 1) out vec3 vNrm;      // facing the air
layout(location = 2) out vec3 vChart;    // the chart's (x, h, z) from the anchor
layout(location = 3) flat out ivec3 vInfo;   // its system, discard box, level
layout(location = 4) out vec3 vNrmC;     // the normal in the chart (the rock's mapping)
layout(location = 5) out vec3 vFlat;     // camera-relative world x, z (not bent) and the height (world Y): cave.frag's cut at the ground
layout(location = 6) out float vAbove;   // its height over the ground, near a mouth (TERRAIN_CAVE_DEEP elsewhere)
layout(location = 7) out float vSky;     // the sky it sees (terrain_cave_sky)

void main() {
    CaveDraw d = caveDraw[gl_InstanceIndex];
    CaveSys s = caveSys[d.info.x];
    uint base = uint(gl_VertexIndex) * 4u, w0 = pool[base], w1 = pool[base + 1u];
    ivec3 q = ivec3(int(w0 << 16) >> 16, int(w0) >> 16, int(w1 << 16) >> 16);
    precise vec3 c = d.off.xyz + vec3(q) * d.off.w;
    precise vec2 xz = s.anchor.xy + vec2(s.J.x * c.x + s.J.y * c.z, s.J.z * c.x + s.J.w * c.z);
    // planetBend (common.glsl), the height less the camera's from the anchor's
    const float R = PLANET_RADIUS;
    float h = s.anchor.w + c.y, sd = length(xz), th = sd / R;
    float sinc = th > 1e-3 ? sin(th) / th : 1.0 - th * th / 6.0;
    float k = (1.0 + h / R) * sinc, hs = sin(0.5 * th);
    vRel = vec3(xz.x * k, (s.anchor.z + c.y) - (R + h) * 2.0 * hs * hs, xz.y * k);
    vec3 n = octDecode(pool[base + 2u]);
    vec2 nxz = vec2(s.J.x * n.x + s.J.y * n.z, s.J.z * n.x + s.J.w * n.z);
    vNrm = planetTilt(xz, normalize(vec3(nxz.x, n.y, nxz.y)));
    vChart = c;
    vNrmC = n;
    vInfo = d.info.xyz;
    vFlat = vec3(xz.x, h, xz.y);
    vAbove = uintBitsToFloat(pool[base + 3u]);
    vSky = float(w1 >> 16) * (1.0 / 65535.0);
#ifdef CAVE_SHELTER
    gl_Position = caveShelter.topDown * vec4(vRel, 1.0);
#else
    gl_Position = frame.viewProj * vec4(vRel, 1.0);
#endif
}
