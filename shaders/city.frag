#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef RT_SHADOWS
#extension GL_EXT_ray_query : require
#endif
#include "common.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNrm;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint  vSeed;
layout(location = 5) flat in uvec4 vParams;
layout(location = 6) flat in uint  vChunk;
layout(location = 7) flat in uint  vFacePack;
layout(location = 8) flat in uint  vAux;
layout(location = 9) in vec2 vLitter;     // litter: ambient occlusion, how soaked (city.vert: litter)

layout(location = 0) out vec4 outColor;
// Reflection G-buffer for screen-space reflections (ssr.comp): world normal
// x and z, roughness, and the weight of the sky reflection this pixel added
// (zero where nothing reflective was drawn). The first three are multiplied by
// the weight, so the MSAA resolve's average weighs each sample by how much it
// reflects: a pane covering one sample over a matt room keeps its own normal
// (readers divide by the weight: reflSample in common.glsl).
layout(location = 1) out vec4 outRefl;

ChunkGPU gC;
float gTime;
vec3 gWorld;
vec4 gRefl = vec4(0.0);   // -> outRefl
vec3 gWinE = vec3(0.0);   // light reaching a window from outside, through its glass: all a dark room has (shadeFacade)

const float SHEEN = 3.5;   // pane sheen strength over glass's physical reflectance (paneSheen)

// Glass marks itself for screen-space reflections: w is the weight its sky
// reflection (envReflection) carries in this pixel's final colour, which the
// reflections pass swaps for the screen's reflection (ssr_apply.frag). Glass is
// a mirror; the negative roughness tells the passes it's glass, not wet ground.
const float GLASS_ROUGH = -1.0;
void glassRefl(vec3 n, float w) { gRefl = vec4(n.x, n.z, GLASS_ROUGH, w); }
// Indoors (a lobby's polished floor) the scene pass adds no sky reflection for
// the screen's to replace (ssr_apply.frag), and the reflection streaks only a little.
const float INDOOR_ROUGH = -3.0;
float gStreak = 1.0;      // wet surfaces: how far GGX highlights stretch toward the viewer    // chunk-local position + wrapped chunk offset: continuous for noise

const float PI = 3.14159265;

/* ======================================================================
 * Small helpers
 * ==================================================================== */

float band(float x, float a, float b, float aa) {
    return smoothstep(a - aa, a + aa, x) * (1.0 - smoothstep(b - aa, b + aa, x));
}

// Integral of a unit-periodic pulse that is 1 on [a,b]; used for box-filtered window masks.
float pulseInt(float x, float a, float b) { return floor(x) * (b - a) + clamp(fract(x), a, b) - a; }
float filteredPulse(float x, float a, float b, float w) {
    w = max(w, 1e-4);
    return clamp((pulseInt(x + 0.5 * w, a, b) - pulseInt(x - 0.5 * w, a, b)) / w, 0.0, 1.0);
}

float hf(uint h, uint k) { return hfloat(hash_combine(h, k)); }
float glyphDist(uint g, vec2 q);   // signs (below)
float segDist(vec2 p, vec2 a, vec2 b);

// Signed distance to a box of half-size b centred on the origin.
float sdBox(vec2 p, vec2 b) { vec2 d = abs(p) - b; return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0); }


#include "city/lighting.glsl"
#include "city/surface.glsl"
#include "city/ground.glsl"
#include "city/rooms.glsl"
#include "city/lobby.glsl"
#include "city/arcology.glsl"
#include "city/facade.glsl"
#include "city/signs.glsl"
#include "city/litter.glsl"

/* ====================================================================== */

void main() {
    gl_SampleMask[0] = -1;     // glass over real interiors covers fewer samples (paneCoverage)
    gC = chunks[vChunk];
    gTime = frame.camPos.w;
    gWorld = vLocal + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
    shadowsBegin();
    vec3 n = normalize(vNrm);
    uint mat = vParams.x;
    vec3 V = -normalize(vRel);

    if ((uint(frame.shadow.y) & SHADOWS_DEBUG) != 0u) {
        // shadow debug view (F10): only the light the lamps, shopfronts and lobbies throw on each surface
        vec3 d = vec3(0.0), sp = vec3(0.0);
        cityLights(vLocal, n, V, 1.0, 0.0, -1.0, d, sp);
        if (mat == MAT_INTERIOR && vParams.y != IS_GLASS) {
            // and in a lobby, the ceiling panels' light on what faces up or sideways
            vec3 nv = dot(n, V) < 0.0 ? -n : n;
            if (nv.y > -0.5) d += LOBBY_SPILL * (nv.y > 0.5 ? 0.55 : 0.3) * 3.0 * ceilingVis();
        }
        outColor = vec4(d * 0.3 + vec3(0.004), 1.0);
        outRefl = vec4(0.0);
        return;
    }

    vec3 col;
    if (mat <= MAT_LOT)            col = shadeGround(mat, vLocal, n, V, vUV, vSeed, vParams);
    else if (mat == MAT_FACADE)    col = shadeFacade(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_ROOF)      col = shadeRoof(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_DOOR)      col = shadeDoor(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_NEON)      col = shadeNeon(vUV, vSeed, vParams, vRel);
    else if (mat == MAT_METAL)     col = shadeMetal(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_EMISSIVE)  col = shadeEmissive(vParams, vSeed);
    else if (mat == MAT_SCREEN)    col = shadeScreen(vUV, vSeed, vParams);
    else if (mat == MAT_INTERIOR)  col = shadeInterior(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_LITTER)    col = shadeLitter(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_ARCOLOGY)  col = shadeArcology(vLocal, vRel, n, vUV, vSeed, vParams);
    else {
        // LED strip: steady or chasing
        float chase = vParams.z == 1u ? 0.25 + 0.75 * step(0.5, fract(vUV.x / 6.0 - gTime * 0.7)) : 1.0;
        col = neonColor(vParams.y) * 6.0 * chase;
    }

    float dist = length(vRel);
    float fog = fogAmount(vRel, dist);
    outColor = vec4(mix(col, hazeColor(vRel / max(dist, 1e-4)), fog), 1.0);
    float reflW = gRefl.w * (1.0 - fog);
    outRefl = vec4(gRefl.xyz * reflW, reflW);
}
