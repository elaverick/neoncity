#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec4  inNrm;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uint  inSeed;
layout(location = 4) in uvec4 inParams;
layout(location = 5) in uint  inAux;

layout(location = 0) out vec3 vRel;     // camera-relative position
layout(location = 1) out vec3 vLocal;   // chunk-local position
layout(location = 2) out vec3 vNrm;
layout(location = 3) out vec2 vUV;
layout(location = 4) flat out uint  vSeed;
layout(location = 5) flat out uvec4 vParams;
layout(location = 6) flat out uint  vChunk;
layout(location = 7) flat out uint  vFacePack;   // facades: packed face widths (exact, so not interpolated)
layout(location = 8) flat out uint  vAux;        // facades: face identity, neighbour width, corners

void main() {
    ChunkGPU c = chunks[gl_InstanceIndex];
    vec3 rel = inPos + c.origin.xyz;
    vRel = rel;
    vLocal = inPos;
    vNrm = normalize(inNrm.xyz);
    vUV = inUV;
    vSeed = inSeed;
    vParams = inParams;
    vChunk = uint(gl_InstanceIndex);
    vFacePack = uint(max(inUV.y, 0.0) + 0.5);
    vAux = inAux;
    gl_Position = frame.viewProj * vec4(rel, 1.0);
}
