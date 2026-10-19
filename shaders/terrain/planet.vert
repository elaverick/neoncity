#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* One of the planet's patches (src/render/planet_gpu.c; the mesh is
 * terrain_patch_fill's, TerrainPatchVertex): the ground beyond the clipmap's
 * window, out to the horizon. A patch's vertices are its cache slot's run of
 * the storage buffer, reached through the draw's vertexOffset. Positions are
 * relative to the patch's centre in the planet frame; the CPU gives the
 * centre relative to the camera in the camera's local frame, from doubles,
 * so they stay exact however far the patch lies. Towards the distance its
 * parent splits at, each vertex blends onto its parent's surface
 * (terrain.h, "Planet patches"). Inside the clipmap's window, where the
 * clipmap draws the ground, it sinks a little (and planet.frag discards it),
 * so the clipmap wins the depth test where the two overlap. */

struct PatchVertex { float p[12]; };   // pos[3], morph[3], n[3], chart[2], urban
layout(std430, set = 0, binding = 1) readonly buffer Verts { PatchVertex verts[]; };

// PlanetPush (src/render/renderer_internal.h)
layout(push_constant) uniform Patch {
    vec4 ax, up, az;     // the camera's local axes in the planet frame (xyz)
    vec4 centre;         // xyz: the patch's centre, camera-relative, local frame; w: how far it sinks in the window (m)
    vec4 morph;          // x, y: the morph's inner and outer distances (m; y 0 for none); z, w: the patch's chart centre less the camera's
    vec4 window;         // the clipmap's window, camera-relative chart x0, z0, x1, z1 (all 0: none)
} pc;

layout(location = 0) out vec3  vRel;     // camera-relative position
layout(location = 1) out vec3  vNrm;
layout(location = 2) out float vUrban;
layout(location = 3) out vec2  vChart;   // camera-relative chart position

vec3 toLocal(vec3 v) { return vec3(dot(v, pc.ax.xyz), dot(v, pc.up.xyz), dot(v, pc.az.xyz)); }

void main() {
    PatchVertex v = verts[gl_VertexIndex];
    vec3 pos = vec3(v.p[0], v.p[1], v.p[2]), mv = vec3(v.p[3], v.p[4], v.p[5]);
    vec3 rel = pc.centre.xyz + toLocal(pos);
    if (pc.morph.y > 0.0) {
        float m = clamp((length(rel) - pc.morph.x) / (pc.morph.y - pc.morph.x), 0.0, 1.0);
        rel += toLocal(mv) * m;
    }
    vChart = vec2(v.p[9], v.p[10]) + pc.morph.zw;
    if (pc.window.z > pc.window.x) {
        // how far inside the window, sinking fully a hole's margin in
        float inside = min(min(vChart.x - pc.window.x, pc.window.z - vChart.x), min(vChart.y - pc.window.y, pc.window.w - vChart.y));
        rel -= normalize(rel + frame.camPos.y * vec3(0.0, 1.0, 0.0) + vec3(0.0, PLANET_RADIUS, 0.0))
               * pc.centre.w * clamp(inside / TERRAIN_PLANET_HOLE_M, 0.0, 1.0);
    }
    vRel = rel;
    vNrm = toLocal(vec3(v.p[6], v.p[7], v.p[8]));
    vUrban = v.p[11];
    gl_Position = frame.viewProj * vec4(rel, 1.0);
}
