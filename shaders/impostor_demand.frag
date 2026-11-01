#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* Which interior modules are on show near the eye (record_scene in
 * src/render/renderer_passes.c, every so often, over the city's depth): one
 * pixel in 16 of each module's floors within reach counts towards it, and
 * src/render/interiors.c streams in the near tiers of the most wanted. It is
 * a pass of its own because a shader that writes a buffer loses early depth
 * testing, which the scene pass can't do without. */
layout(early_fragment_tests) in;

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNrm;       // (what city.vert writes, unused here)
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint vSeed;
layout(location = 5) flat in uvec4 vParams;
layout(location = 6) flat in uint vChunk;
layout(location = 7) flat in uint vFacePack;
layout(location = 8) flat in uint vAux;
layout(location = 9) in vec2 vLitter;
layout(location = 10) in float vBase;

float hf(uint h, uint k) { return hfloat(hash_combine(h, k)); }   // as city.frag's

#include "city/impostor_pick.glsl"

layout(set = 0, binding = 13) buffer NearDemand { uint want[INTERIOR_MAX_MODULES]; } nearDemand;

void main() {
    uint plate = vFacePack >> 24;
    ivec2 px = ivec2(gl_FragCoord.xy);
    if (vParams.x != MAT_FACADE || plate == 0u || ((px.x | px.y) & 3) != 0 || length(vRel) > interiors.info.w * 2.5) return;
    float groundH = facadeGroundH(vSeed) * (((vParams.z >> 1) & 3u) == 2u ? 2.0 : 1.0);
    float floorIdx = floor((vLocal.y - vBase - SIDEWALK_HEIGHT - groundH) / facadeFloorH(vSeed));
    if (floorIdx < 0.0) return;
    int m = impostorModule(vSeed, int(vParams.y), int(floorIdx), plate);
    if (m >= 0) atomicAdd(nearDemand.want[m], 1u);
}
