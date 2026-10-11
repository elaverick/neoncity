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

/* Overhead cables (emit_cable in src/city.c) swing about their chord when a
 * gust passes. uv.x is this vertex's drop below the chord, uv.y the cable's
 * sag, aux the horizontal direction across it. The swing rotates the drop
 * about the chord, so a taut cable, with little drop, barely stirs, and like
 * a pendulum hanging its sag it swings slower the slacker it hangs. Gusts are
 * a slow noise field blowing across the city with the cloud deck's wind:
 * occasional, and shared by cables near each other, which stir together.
 * Cables across the wind take it most; along it, hardly at all. */
const vec2 GUST_WIND = vec2(0.94, 0.34);   // clouds.comp WIND, normalised

vec3 cableSway(vec3 world, float drop, float sag, uint aux, uint seed, float t) {
    float a = (float(aux & 0xFFFFu) / 65535.0 - 0.5) * 6.2831853;
    vec2 across = vec2(cos(a), sin(a));
    vec2 g = world.xz / 70.0 - GUST_WIND * t * 0.09;   // gusts drift at ~6 m/s
    float gust = smoothstep(0.45, 0.75, vnoise(g, 0x6057u));
    float facing = 0.25 + 0.75 * abs(dot(across, GUST_WIND));
    float w = clamp(sqrt(9.81 / max(sag, 0.1)), 2.0, 9.0);
    // pushed over by the gust, rocking about that while it lasts
    float theta = 0.5 * gust * facing * (0.55 + 0.45 * sin(w * t + hfloat(seed) * 1.5)) * sign(dot(across, GUST_WIND) + 1e-3);
    return vec3(across.x * sin(theta), 1.0 - cos(theta), across.y * sin(theta)) * drop;
}

void main() {
    ChunkGPU c = chunks[gl_InstanceIndex];
    vec3 rel = inPos + c.origin.xyz;
    vec3 local = inPos;
    if (inParams.x == MAT_METAL && inParams.w == 1u && inUV.x > 0.0) {
        vec3 d = cableSway(rel + frame.camPos.xyz, inUV.x, inUV.y, inAux, inSeed, frame.camPos.w);
        rel += d;
        local += d;
    }
    vRel = rel;
    vLocal = local;
    vNrm = normalize(inNrm.xyz);
    vUV = inUV;
    vSeed = inSeed;
    vParams = inParams;
    vChunk = uint(gl_InstanceIndex);
    vFacePack = uint(max(inUV.y, 0.0) + 0.5);
    vAux = inAux;
    gl_Position = frame.viewProj * vec4(rel, 1.0);
}
