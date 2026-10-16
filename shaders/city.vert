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
#ifndef PREPASS   // (prepass.frag reads neither)
layout(location = 7) flat out uint  vFacePack;   // facades: packed face widths (exact, so not interpolated)
layout(location = 8) flat out uint  vAux;        // facades: face identity, neighbour width, corners
#endif
layout(location = 9) out vec2 vLitter;           // litter: ambient occlusion, how soaked
// The depth prepass (renderer.c: record_prepass) and the scene pass must agree to the bit.
invariant gl_Position;

// Shelter map (renderer.c: record_shelter), as shaders/city.frag reads it.
layout(set = 0, binding = 4) uniform sampler2D shelterMap;

/* Overhead cables (emit_cable in src/city/cables.c) swing about their chord when a
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

/* Litter (src/city/litter.c). aux holds each vertex's offset from its item's
 * anchor on the ground, so the item can be reshaped about it:
 * - Soaked: rain soaks whatever lies in the open (shelter map, one tap at the
 *   anchor), and the gutter's run-off soaks what lies in it whatever the
 *   weather is doing. Under cover things stay nearly dry.
 * - Slumped: soaked paper, cardboard and carrier bags slump flat to the
 *   ground, a paper ball sags, and anything run over by traffic lies flat.
 * - Stirred: dry paper flaps and creeps downwind with the cables' gusts, its
 *   edges most (flex), and a dry carrier bag billows.
 * - Faded: beyond about 70% of its fade distance the item shrinks into its
 *   anchor and is gone (degenerate triangles) by the fade distance.
 * Writes the ambient occlusion and soak for the fragment shader. */
float shelterOver(vec3 q) {
    vec2 uv = (q.xz - frame.shelter.xy) / frame.shelter.z * 0.5 + 0.5;
    float e = max(abs(uv.x * 2.0 - 1.0), abs(uv.y * 2.0 - 1.0));
    if (e >= 1.0) return 0.0;
    float top = SHELTER_LO + textureLod(shelterMap, uv, 0.0).r * (SHELTER_HI - SHELTER_LO);
    return smoothstep(0.25, 0.6, top - q.y) * (1.0 - smoothstep(0.85, 1.0, e));
}

void litter(inout vec3 rel, inout vec3 nrm, out vec2 lit) {
    uint kind = inParams.y, w = inParams.w;
    vec3 off = vec3(float(int(inAux << 21) >> 21) * 0.002, float(inAux >> 22) * 0.001, float(int(inAux << 10) >> 21) * 0.002);
    vec3 anchor = rel - off;
    float wet = frame.weather.y;
    float soak = wet * mix(0.15, 1.0, 1.0 - shelterOver(anchor + vec3(0.0, 0.05, 0.0)));
    if ((w & 64u) != 0u) soak = max(soak, smoothstep(0.5, 0.7, wet));
    float slump = kind == LK_NEWS || kind == LK_FLYER || kind == LK_SCRAP ? 0.88
                : kind == LK_CARDBOARD ? 0.75 : kind == LK_BAG ? 0.7 : kind == LK_WAD ? 0.35 : 0.0;
    float flat_ = slump * max(smoothstep(0.35, 0.8, soak), (w & 128u) != 0u ? 1.0 : 0.0);
    off.y *= 1.0 - flat_;
    nrm = mix(nrm, vec3(0.0, 1.0, 0.0), flat_);
    float flex = float(inParams.z) / 255.0 * (1.0 - smoothstep(0.25, 0.5, soak)) * (1.0 - flat_);
    if (flex > 0.0) {
        float t = frame.camPos.w;
        vec2 g = (anchor.xz + frame.camPos.xz) / 70.0 - GUST_WIND * t * 0.09;
        float gust = smoothstep(0.45, 0.75, vnoise(g, 0x6057u));
        float stir = flex * (0.15 + 0.85 * gust);                   // a breath of air always stirs a dry sheet
        float wave = sin(t * 11.0 - dot(off.xz, GUST_WIND) * 18.0 + hfloat(inSeed) * 6.2831853);
        if (kind == LK_BAG) off *= 1.0 + stir * (0.22 + 0.12 * wave);
        else {
            float k = kind == LK_CARDBOARD ? 0.3 : 1.0;
            off.y += stir * k * (0.035 + 0.025 * wave);
            off.xz += GUST_WIND * stir * k * 0.02 * (1.0 + wave);
        }
    }
    float fade = float(w & 63u) * 4.0;
    rel = anchor + off * (1.0 - smoothstep(fade * 0.7, fade, length(anchor)));
    lit = vec2(inNrm.w, soak);
}

#ifdef PREPASS
/* The prepass leaves out what city.frag draws over only some of a pixel's
 * samples, or discards: glass over real interiors (paneCoverage), veils of
 * light (veil), puddles, and litter's contact shadows. (The glazed ground
 * floors of facades with a real lobby behind are cut out by prepass.frag.) */
bool partialCoverage() {
    uint m = inParams.x, y = inParams.y;
    if (m == MAT_INTERIOR) return y == 8u;                     // IS_GLASS
    if (m == MAT_LITTER) return y == LK_SHADOW;
    if (m == MAT_ARCOLOGY)                                     // ARS_SHAFT, ARS_SEARCH, ARS_PUDDLE, ARS_STEAM; ARS_RAIL's glass
        return (y >= 22u && y <= 25u) || (y == 19u && (inParams.z == 0u || inParams.z == 3u));
    return false;
}
#endif

void main() {
    ChunkGPU c = chunks[gl_InstanceIndex];
    vec3 rel = inPos + c.origin.xyz;
    vec3 local = inPos;
    vec3 nrm = inNrm.xyz;
    vec2 lit = vec2(1.0, 0.0);
    if (inParams.x == MAT_METAL && inParams.w == 1u && inUV.x > 0.0) {
        vec3 d = cableSway(rel + frame.camPos.xyz, inUV.x, inUV.y, inAux, inSeed, frame.camPos.w);
        rel += d;
        local += d;
    } else if (inParams.x == MAT_LITTER) {
        vec3 r0 = rel;
        litter(rel, nrm, lit);
        local += rel - r0;
    } else if (inParams.x == MAT_INTERIOR && (inParams.y == 10u || inParams.y == 20u)) {
        lit = vec2(inNrm.w, 0.0);   // lobby plants (grow_plants in src/city/mesh_planting.c): how open the crown is there
    } else if (inParams.x == MAT_ARCOLOGY && inParams.y == 23u) {
        /* An arcology's searchlight beam (emit_arc_searchlight in src/city/mesh_arcology.c)
         * hangs from its pivot and swings to and fro along the canyon: uv is
         * the pivot less this vertex (chunk xz), aux how far along | the drop
         * below the pivot << 16 (1/64 m), params z the swing direction, w the
         * phase, seed the amplitude | speed << 8. */
        float az = float(inParams.z) / 255.0 * 6.2831853;
        vec2 S = vec2(cos(az), sin(az));
        float amp = float(inSeed & 255u) / 255.0 * 0.6, spd = 0.1 + float((inSeed >> 8) & 255u) / 255.0 * 0.3;
        float th = amp * sin(frame.camPos.w * spd + float(inParams.w) / 255.0 * 6.2831853);
        float c = cos(th), sn = sin(th);
        vec3 d = vec3(-inUV.x, -float(inAux >> 16) / 64.0, -inUV.y);
        float s = dot(d.xz, S);
        vec2 q = d.xz - s * S;
        float s2 = s * c - d.y * sn, y2 = s * sn + d.y * c;
        vec3 off = vec3(q.x + s2 * S.x, y2, q.y + s2 * S.y) - d;
        rel += off;
        local += off;
        float ns = dot(nrm.xz, S);
        vec2 nq = nrm.xz - ns * S;
        float ns2 = ns * c - nrm.y * sn;
        nrm = vec3(nq.x + ns2 * S.x, ns * sn + nrm.y * c, nq.y + ns2 * S.y);
        lit = vec2(float(inAux & 0xFFFFu) / 65535.0, 0.0);
    }
    vRel = rel;
    vLocal = local;
    vNrm = normalize(nrm);
    vLitter = lit;
    vUV = inUV;
    vSeed = inSeed;
    vParams = inParams;
    vChunk = uint(gl_InstanceIndex);
#ifndef PREPASS
    vFacePack = uint(max(inUV.y, 0.0) + 0.5);
    vAux = inAux;
#endif
    gl_Position = frame.viewProj * vec4(rel, 1.0);
#ifdef PREPASS
    if (partialCoverage()) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // all three corners outside: the triangle is clipped away
#endif
}
