#version 450
#extension GL_GOOGLE_include_directive : require
#define SCENE_SET 1
#include "common.glsl"
#include "terrain/land_lights.glsl"

/* The terrain's depth prepass (src/render/terrain_gpu.c: terrain_gpu_draw_prepass):
 * the clipmap's tiles near the city's lamps, within SHADOW_FAR, after the
 * city's surfaces (prepass.frag), with the land's normal and the chunk whose
 * light table lights it (terrain/land_lights.glsl), so the shadow passes
 * trace the lamps for it. terrain.vert's position is invariant, so the scene
 * pass draws the land at this very depth. Ground no lamp reaches is left
 * unlit (no PG_LIT). */
layout(location = 1) in vec3 vNrm;
layout(location = 5) in vec3 vFlat;
layout(location = 0) out uvec2 outG;

void main() {
    int rec = landChunk(vFlat.xz);
    outG = uvec2(octEncode(normalize(vNrm)), rec >= 0 ? (uint(rec) & PG_CHUNK) | PG_LIT : 0u);
}
