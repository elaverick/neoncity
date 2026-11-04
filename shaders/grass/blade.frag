#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/ground.glsl"
#include "grass_rules.h"
#include "grass/wind.glsl"

/* Grass blades (blade.vert). Coloured by the species' profile
 * (frame.grassSpecies: assets/grass/<name>.json): root to tip, and towards
 * the dry colour by the blade's tint, mostly its clump's (spec 10.1: colour
 * coherent over a clump, not per blade), times the ground's large-scale
 * variation at the root (blade.vert: vVary) and, on the land, darkened by the
 * rain as the land's grass is, so it agrees with the ground's field
 * (grass/field.glsl) where the blades give way to it.
 *
 * Lit, until GR7, by the light the ground at the root takes, facing its
 * normal, a little more where the blade's face turns up to it:
 * - on the land, the land's (groundLightIn), hazed as the land is;
 * - in the city (lawn and rough, GR6), the city's lights from its chunk's
 *   table (city/lighting.glsl cityLights, as city.frag's ground), with the
 *   soft shadows the ground at the root has: the blades aren't in the depth
 *   prepass, so the slots at the root's pixel (blade.vert: vRootPx) are the
 *   ground's, its very spot, if nothing stands in front of it there (its
 *   chunk and forward distance match); else, and for lights the slots don't
 *   hold, unshadowed, never a traced ray (GRASS_SLOTS: the ray-traced
 *   variant, blade_rt.frag). The ambient, the signs' bounce and the city's
 *   haze as the ground's.
 * --grass-debug lod colours them by bucket (full red, mid yellow, clump
 * blue), cull by patch, wind by the wind's strength at the root; F10's
 * shadow view, a city blade's light alone. Two-sided. Nothing for the
 * reflections. */

layout(location = 0) in vec3 vRel;
layout(location = 1) in float vT;
layout(location = 2) flat in vec3 vRoot;
layout(location = 3) flat in ivec2 vKind;
layout(location = 4) flat in float vTint;
layout(location = 5) in vec3 vN;
layout(location = 6) flat in vec3 vGround;
layout(location = 7) flat in vec3 vVary;
layout(location = 8) flat in float vWind;
layout(location = 9) flat in uint vChunk;
layout(location = 10) flat in vec3 vRootPx;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;

// city/lighting.glsl's surroundings (city.frag's)
#ifdef GRASS_SLOTS
#define SLOT_SHADOWS 1
#endif
#define LIGHTING_FOR_GRASS 1
ChunkGPU gC;
float gTime;
vec3 gWorld;
float gBase;
float gStreak = 1.0;
const float SHEEN = 3.5;
const float PI = 3.14159265;
#include "city/lighting.glsl"

const vec3 GRASS_LOD_COLOUR[GRASS_BUCKETS] = vec3[](vec3(0.85, 0.10, 0.06), vec3(0.85, 0.70, 0.05), vec3(0.10, 0.35, 0.95));

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// The ground's shadow slots at the root (gSlotsOurs, gSlotIds, gSlotVis, gShadowFade).
void rootShadows() {
#ifdef GRASS_SLOTS
    if (!shadowsOn()) return;
    gShadowFade = smoothstep(SHADOW_FADE, SHADOW_FAR, length(vRoot));
    ivec2 px = ivec2(vRootPx.xy);
    if (any(lessThan(px, ivec2(0))) || any(greaterThanEqual(px, ivec2(frame.screen.xy)))) return;
    gSlotVis = texelFetch(shadowVisTex, px, 0);
    float tol = 0.05 + vRootPx.z * 0.01;
    gSlotsOurs = (gSlotVis.w & SH_VALID) != 0u && (gSlotVis.w & 0xFFFFu) == vChunk && abs(uintBitsToFloat(gSlotVis.z) - vRootPx.z) < tol;
    if (gSlotsOurs) gSlotIds = texelFetch(shadowIdsTex, px, 0);
#endif
}

void main() {
    int view = int(frame.grass.z + 0.5);
    int k = clamp(vKind.x, 0, GRASS_SPECIES - 1);
    bool city = (k == GRASS_LAWN || k == GRASS_ROUGH) && vChunk != GRASS_CHUNK_NONE;
    vec3 col;
    if (view == GRASS_DEBUG_LOD) {
        col = GRASS_LOD_COLOUR[clamp(vKind.y, 0, GRASS_BUCKETS - 1)] * (0.4 + 0.6 * vT);
    } else if (view == GRASS_DEBUG_CULL) {
        vec2 q = frame.grass.xy + vRoot.xz;
        ivec2 gp = frame.grassPatch.xy + ivec2(floor(q / float(GRASS_PATCH_M)));
        uint h = hash2i(0x9A7C4u, gp.x, gp.y);
        col = (0.15 + 0.6 * vec3(hfloat(h), hfloat(hash_u32(h)), hfloat(hash_u32(h + 7u)))) * (0.4 + 0.6 * vT);
    } else if (view == GRASS_DEBUG_WIND) {
        col = grassWindColour(vWind) * (0.5 + 0.5 * vT);
    } else {
        GrassSpecies sp = frame.grassSpecies[k];
        float up = pow(clamp(vT, 0.0, 1.0), 0.7);
        vec3 lush = mix(sp.root.rgb, sp.tip.rgb, up);
        // the dry colour is the tip's; down the blade it darkens as the lush one does
        float rootShare = lum(sp.root.rgb) / max(lum(sp.tip.rgb), 1e-4);
        vec3 dry = sp.dry.rgb * mix(rootShare, 1.0, up);
        float dryness = clamp(vTint + 0.5, 0.0, 1.0) * sp.dry.w;
        vec3 albedo = mix(lush, dry, dryness) * (1.0 + 0.3 * vTint) * vVary;
        vec3 N = normalize(vN);
        float face = 0.8 + 0.2 * max(dot(N, vGround), 0.0);
        if (city) {
            // the city's light on the ground at the root (city.frag, shadeGround's)
            gC = chunks[vChunk];
            gTime = frame.camPos.w;
            vec3 p = vRel - gC.origin.xyz;   // chunk-local
            gWorld = p + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
            gBase = vRoot.y - gC.origin.y;
            rootShadows();
            vec3 V = -normalize(vRel);
            float h = p.y - gBase;
            vec3 diff = ambientLight(vGround, h), spec = vec3(0.0);
            cityLights(p, vGround, V, 8.0, 0.0, -1.0, diff, spec);
            if ((uint(frame.shadow.y) & SHADOWS_DEBUG) != 0u) {
                vec3 d = vec3(0.0), sp2 = vec3(0.0);
                cityLights(p, vGround, V, 1.0, 0.0, -1.0, d, sp2);
                outColor = vec4(d * 0.3 + vec3(0.004), 1.0);
                outRefl = vec4(0.0);
                return;
            }
            diff += neonBounce(gWorld, h);
            col = albedo * face * diff;
            float dist = length(vRel);
            col = mix(col, hazeColor(vRel / max(dist, 1e-4)), fogAmount(vRel, dist));
        } else {
            LandSky sky = landSky(vRel);
            col = groundHazed(albedo * landDarken(2, sky.wet) * face * groundLightIn(sky, vGround), vRel, 0.0);
        }
    }
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
