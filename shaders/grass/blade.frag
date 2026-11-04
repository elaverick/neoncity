#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/ground.glsl"
#include "grass_rules.h"
#include "grass/wind.glsl"
#include "grass/wake.glsl"

/* Grass blades (blade.vert). Coloured by the species' profile
 * (frame.grassSpecies: assets/grass/<name>.json): root to tip, and towards
 * the dry colour by the blade's tint, mostly its clump's (spec 10.1: colour
 * coherent over a clump, not per blade), times the ground's large-scale
 * variation at the root (blade.vert: vVary), darker into the tuft (its
 * species' root shade), and darkened by the rain as the land's grass is, so
 * it agrees with the ground's field (grass/field.glsl, field_city.glsl) where
 * the blades give way to it.
 *
 * Lit by the light the ground at the root takes, facing its normal, a little
 * more where the blade's face turns up to it, and by its own thin-surface
 * terms (below, GR7):
 * - on the land, the land's (groundLightIn) and beside a freeway its lamps'
 *   pools (blade.vert: vFwy, the root's patch's freeway), hazed as the land is;
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
layout(location = 11) flat in vec4 vFwy;
layout(location = 12) flat in vec4 vLight;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;

/* ---- a thin blade's own light (GR7, spec 10.2-10.3) ---------------------
 * Each light with a direction (the city's lamps, shopfronts and flares, by
 * city/lighting.glsl's hook; on the land the moon and the freeway's lamps)
 * gives, beyond the ground's light at the root that the field is matched to:
 * - its face: the blade lit with its own normal's part across the ground
 *   added to the ground's, (n + N_h).L, in place of n.L. A blade turned to a
 *   low lamp takes more of it, one turned away less; over every way a blade
 *   faces, the mean is the ground's again;
 * - the light through it, where the face seen is turned from the light,
 *   strongest looking towards the light (a blade against a lamp);
 * - its sheen: GGX on its own normal, a little dry, more wet.
 * All three fade out as the blade shrinks on screen (blade.vert: vLight.x),
 * so the field far off is the ground's, and nothing fine sparkles there. */
vec3 gN, gNg, gV;              // the blade's normal (the camera's side), the ground's, towards the camera
float gAlpha, gF0;             // its GGX alpha and reflectance square on
vec3 gThinFace = vec3(0.0), gThinBack = vec3(0.0), gThinSpec = vec3(0.0);

// the ground's normal (n), turned no further down than this towards a light
// for the light's square-on irradiance from what the ground took (the freeway's pools)
const float GRASS_THIN_NL_MIN = 0.2;
// how much of its own face's light a blade takes, at most (the rest the ground's at its root)
const float GRASS_THIN_FACE = 0.4;
// the dry sheen's strength, a share of the wet's
const float GRASS_SHEEN_DRY = 0.3;

// spec: whether its sheen is drawn (the moon's isn't: its highlight is
// wetHighlight's, at the disc's own brightness, far under its light's)
void grassThinLightS(vec3 L, vec3 c, bool spec) {
    float ng = max(dot(gNg, L), 0.0), nl = dot(gN, L);
    vec3 Nh = gN - gNg * dot(gN, gNg);
    gThinFace += c * (max(dot(gNg + Nh, L), 0.0) - ng);
    gThinBack += c * max(-nl, 0.0) * (0.4 + 0.6 * pow(max(dot(-gV, L), 0.0), 2.0));
    if (spec && nl > 0.0) gThinSpec += c * landGgx(gN, L, gV, gAlpha, gF0);   // (terrain/ground.glsl: n.L in, pi carried)
}
void grassThinLight(vec3 L, vec3 c) { grassThinLightS(L, c, true); }

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
#include "terrain/freeway_light.glsl"

// On the land, the lights with a direction, for the fragment at rel
// (camera-relative): the moon through the cloud (as groundLightIn's), and
// beside the root's freeway (vFwy) its three nearest lamps, each as its pool
// lights the ground (roads.glsl shadeRoad). Returns the pools' light at the
// root, as the ground there takes it.
vec3 landThinLights(LandSky sky, vec3 rel) {
    grassThinLightS(MOON_DIR, MOON_LIGHT * (1.0 - 0.9 * sky.cover), false);
    vec3 pools = vec3(0.0);
    if (vLight.y <= 0.0) return pools;
    float a = abs(vFwy.x), beside = freewayBeside(a, vLight.y);
    if (beside <= 0.0) return pools;
    float ds = freewayLampDs(vFwy.y);
    pools = ROAD_SODIUM * FREEWAY_POOL * freewayPool(ds, a, 0.0) * beside;
    vec2 across = vFwy.zw, along = vec2(across.y, -across.x);
    vec3 up = rel - vRoot;   // the fragment over its root
    for (int j = -1; j <= 1; ++j) {
        float dj = ds - float(j) * FREEWAY_LAMP_SPACING;
        float pj = exp(-dj * dj / (2.0 * 14.0 * 14.0) - a * a / (2.0 * 11.0 * 11.0)) * beside;
        if (pj < 1e-3) continue;
        vec3 L = normalize(freewayLampVec(along, across, ds, vFwy.x, j) - up);
        // the pool is what the ground takes; square on to the lamp, more
        vec3 c = ROAD_SODIUM * FREEWAY_POOL * pj / max(dot(gNg, L), GRASS_THIN_NL_MIN);
        grassThinLight(L, c);
    }
    return pools;
}

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

// The sheen's roughness, and its strength (a share of a wet blade's), for a
// blade of species sp at t up it, wet as wet (0 .. 1), its normal turning by
// kernel across the pixel (normalKernel: the highlight widened rather than
// sparkling); sets gAlpha and gF0 for grassThinLight.
float bladeSheen(GrassSpecies sp, float wet, float kernel, out float rough) {
    rough = mix(sp.light.z, sp.light.z * 0.45, wet);   // a wet film smooths it, as the city's ground
    rough = pow(pow(rough, 4.0) + kernel, 0.25);
    gAlpha = max(rough * rough, 0.002);
    gF0 = 0.04;   // a leaf's waxy skin, about water's
    return mix(GRASS_SHEEN_DRY, 1.0, wet);
}

void main() {
    int view = int(frame.grass.z + 0.5);
    int k = clamp(vKind.x, 0, GRASS_SPECIES - 1);
    bool city = (k == GRASS_LAWN || k == GRASS_ROUGH) && vChunk != GRASS_CHUNK_NONE;
    float kernel = normalKernel(normalize(vN));   // in uniform control flow
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
    } else if (view == GRASS_DEBUG_WAKE) {
        col = grassWakeColour(vWind) * (0.5 + 0.5 * vT);
    } else {
        GrassSpecies sp = frame.grassSpecies[k];
        float up = pow(clamp(vT, 0.0, 1.0), 0.7);
        vec3 lush = mix(sp.root.rgb, sp.tip.rgb, up);
        // the dry colour is the tip's; down the blade it darkens as the lush one does
        float rootShare = lum(sp.root.rgb) / max(lum(sp.tip.rgb), 1e-4);
        vec3 dry = sp.dry.rgb * mix(rootShare, 1.0, up);
        float dryness = clamp(vTint + 0.5, 0.0, 1.0) * sp.dry.w;
        vec3 albedo = mix(lush, dry, dryness) * (1.0 + 0.3 * vTint) * vVary;
        float t = clamp(vT, 0.0, 1.0);
        albedo *= 1.0 - sp.light.x * (1.0 - t) * (1.0 - t);   // darker into the tuft (grass.c grass_field's too)
        vec3 N = normalize(vN);
        float face = 0.8 + 0.2 * max(dot(N, vGround), 0.0);
        gN = N; gNg = vGround; gV = -normalize(vRel);
        float own = vLight.x;                                   // its own light's share, by its size on screen
        float through = sp.light.y * (0.3 + 0.7 * t) * own;    // thinner towards the tip
        float rough;
        if (city) {
            // the city's light on the ground at the root (city.frag, shadeGround's)
            gC = chunks[vChunk];
            gTime = frame.camPos.w;
            vec3 p = vRel - gC.origin.xyz;   // chunk-local
            gWorld = p + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
            gBase = vRoot.y - gC.origin.y;
            rootShadows();
            // wet as the streets, but none where something stands over it from the rain
            // (the ground there stays wet, as the water runs in; a blade doesn't)
            float wet = frame.weather.y * (1.0 - vLight.z), wetB = wet * (0.7 + 0.3 * t);
            float sheen = bladeSheen(sp, wetB, kernel, rough) * own;
            albedo *= landDarken(2, wet);   // as the land's grass (and the park's ground: grass/field_city.glsl grassWetDarken)
            float h = p.y - gBase;
            vec3 diff = ambientLight(vGround, h), spec = vec3(0.0);
            cityLights(p, vGround, gV, 8.0, 0.0, -1.0, diff, spec);
            if ((uint(frame.shadow.y) & SHADOWS_DEBUG) != 0u) {
                vec3 d = vec3(0.0), sp2 = vec3(0.0);
                cityLights(p, vGround, gV, 1.0, 0.0, -1.0, d, sp2);
                outColor = vec4(d * 0.3 + vec3(0.004), 1.0);
                outRefl = vec4(0.0);
                return;
            }
            diff += neonBounce(gWorld, h);
            col = albedo * (face * (diff + GRASS_THIN_FACE * own * gThinFace) + through * gThinBack) + sheen * gThinSpec;
            // the wet blade's sheen of the sky and the lit streets round (envReflection, city.frag's)
            if (wetB > 0.0 && own > 0.0) {
                vec3 R = normalize(mix(reflect(-gV, N), N, rough * rough));
                col += envReflection(R) * envBRDF(0.02, rough, max(dot(N, gV), 1e-4)) * wetB * own;
            }
            float dist = length(vRel);
            col = mix(col, hazeColor(vRel / max(dist, 1e-4)), fogAmount(vRel, dist));
        } else {
            LandSky sky = landSky(vRel);
            float wetB = sky.wet * (0.7 + 0.3 * t);
            float sheen = bladeSheen(sp, wetB, kernel, rough) * own;
            vec3 pools = landThinLights(sky, vRel);
            vec3 E = groundLightIn(sky, vGround) + pools;
            col = albedo * landDarken(2, sky.wet) * (face * (E + GRASS_THIN_FACE * own * gThinFace) + through * gThinBack) + sheen * gThinSpec;
            // the wet blade's sheen of the sky and the moon (the land's: wetHighlight)
            if (wetB > 0.0 && own > 0.0) col += wetHighlight(N, gV, rough, sky) * wetB * own;
            col = groundHazed(col, vRel, 0.0);
        }
    }
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
