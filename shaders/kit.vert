#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* The vendors' kit, instanced (src/render/kit_gpu.c): a module of the library
 * (binding 0: the city's vertices, in the module's frame; params x MAT_KIT, y
 * its material PMAT_*, z its baked occlusion, w its wear; aux the piece of the
 * module it belongs to) set down by its instance (binding 1: kit_gpu.h
 * KitInstGPU: the module's frame to camera-relative, rows; then its seed, its
 * chunk record, its part, its base). It hands city.frag (and prepass.frag) what
 * city.vert does; the kit's own terms ride in vLitter (occlusion, wear),
 * interpolated. Both passes run this same shader, so they agree to the bit. */

layout(location = 0) in vec3  inPos;
layout(location = 1) in vec4  inNrm;
layout(location = 2) in vec2  inUV;
layout(location = 3) in uint  inSeed;
layout(location = 4) in uvec4 inParams;
layout(location = 5) in uint  inAux;
layout(location = 6) in float inBase;
layout(location = 7) in vec4  inRow0;
layout(location = 8) in vec4  inRow1;
layout(location = 9) in vec4  inRow2;
layout(location = 10) in uvec4 inInst;   // seed, chunk record, part | flags << 8, base (the float's bits)

layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vLocal;
layout(location = 2) out vec3 vNrm;
layout(location = 3) out vec2 vUV;
layout(location = 4) flat out uint  vSeed;
layout(location = 5) flat out uvec4 vParams;
layout(location = 6) flat out uint  vChunk;
layout(location = 7) flat out uint  vFacePack;
layout(location = 8) flat out uint  vAux;
layout(location = 9) out vec3 vLitter;   // the kit: baked occlusion, wear, the cooking's grime (NB7)
layout(location = 10) out float vBase;
invariant gl_Position;

void main() {
    vec4 p = vec4(inPos, 1.0);
    vec3 rel = vec3(dot(inRow0, p), dot(inRow1, p), dot(inRow2, p));
    vec3 n = inNrm.xyz;
    vec3 nrm = vec3(dot(inRow0.xyz, n), dot(inRow1.xyz, n), dot(inRow2.xyz, n));
    uint rec = inInst.y & 0x7FFFFFFFu;   // the chunk record; the top bit: its vendor shut (kit_gpu.h KIT_INST_SHUT, SF4)
    ChunkGPU c = chunks[rec];
    vRel = rel;
    vLocal = rel - c.origin.xyz;
    vNrm = normalize(nrm + vec3(0.0, 1e-6, 0.0));   // a module shrunk to nothing as it fades has no normal left
    vUV = inUV;
    vSeed = inInst.x;
    vParams = uvec4(MAT_KIT, inParams.y | ((inInst.y & 0x80000000u) != 0u ? KIT_SHUT : 0u), inInst.z & 255u, (inInst.z >> 8) & 0xFFFFu);
    vChunk = rec;
    vFacePack = 0u;
    vAux = inAux;
    vLitter = vec3(vec2(float(inParams.z), float(inParams.w)) / 255.0, float(int(inInst.z) >> 24) / 127.0);   // grime: the instance's (kit_gpu.c)
    vBase = uintBitsToFloat(inInst.w);
    gl_Position = frame.viewProj * vec4(rel, 1.0);
}
