#version 460
#extension GL_GOOGLE_include_directive : require
#ifdef RT_SHADOWS
#extension GL_EXT_ray_query : require
#endif
#define SCENE_SET 1   // the scene set's chunks, light tables, shadow slots and TLAS, for the city's lamps
#include "common.glsl"
#include "ground.glsl"
#include "roads.glsl"
#include "grass/debug.glsl"
#ifdef CAVE_CUT
#define CAVE_HOLES_SET 0
#define CAVE_HOLES_BINDING 4
#include "terrain/cave_hole.glsl"
#define CAVE_CUT_DISCARD if (hole) discard;
#else
#define CAVE_CUT_DISCARD
#endif

/* The terrain clipmap (terrain.vert): the bare land (ground.glsl) and the
 * roads between the settlements laid on it (roads.glsl). Built again with
 * CAVE_CUT (terrain_cut.frag) for the tiles near a cave's mouth: cut away
 * where it lies in a cave's air there (terrain/cave_hole.glsl; the cut comes
 * last, after the derivatives, which a quad's discarded neighbour would
 * spoil). Only there, since a shader that can discard loses the early depth
 * test: facing a hillside, all the ground behind it would be shaded.
 *
 * Beside the city the land takes its lamps, shopfronts and flares (M11b:
 * LAND_LIGHTS, all but the cut, as no cave lies near a city): the light table
 * of the chunk terrain/land_lights.glsl finds, walked as city.frag walks its
 * own (city/lighting.glsl cityLights), out to LAND_LIGHTS_FAR. With
 * RT_SHADOWS (terrain_rt.frag) their shadows: the tiles near the lamps are in
 * the depth prepass (terrain_pre.frag), so the slots at a pixel are the
 * land's there; the lights they don't hold are traced here.
 *
 * Under a clear sky the moon's shadows (M11c: terrain/dir_shadow.glsl): the
 * ray through the TLAS near the camera with RT_SHADOWS, beyond it (and in
 * the other builds) the march through the clipmap's heights. */

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 3) in vec4  vCoverA;
layout(location = 4) in vec4  vCoverB;
layout(location = 5) in vec3  vFlat;
layout(location = 0) out vec4 outColor;
// Nothing for the screen-space reflections (city.frag): the land's wet
// sheen reflects its own sky (landReflection), not the city's
// (envReflection), which ssr_apply.frag takes the reflections' place of.
layout(location = 1) out vec4 outRefl;

#ifndef CAVE_CUT
#define LAND_LIGHTS 1
#endif
#ifdef LAND_LIGHTS
// city/lighting.glsl's surroundings (city.frag's)
#define LIGHTING_FOR_LAND 1
ChunkGPU gC;
float gTime;
vec3 gWorld;
float gBase;
float gStreak = 1.0;
const float SHEEN = 3.5;
const float PI = 3.14159265;
#include "city/lighting.glsl"
#include "terrain/land_lights.glsl"
#ifdef RT_SHADOWS
#define DIR_SHADOW_TRACE 1   // the moon's ray through the TLAS city/lighting.glsl binds
#endif

// The land's own roughness for the lamps' highlights where it is dry, and its
// reflectance square on; wet, its film's or its puddles' (wetSurface).
const float LAND_DRY_ROUGH = 0.8, LAND_DRY_F0 = 0.04;

// The shadow slots at this pixel, if the prepass drew this surface there with
// chunk record rec (as city/lighting.glsl shadowsBegin; fwd and tol taken in
// uniform control flow).
void landSlots(int rec, float dist, float fwd, float tol) {
#ifdef RT_SHADOWS
    gFwd = fwd; gFwdTol = tol;
    gGeoN = normalize(vNrm);
    gShadowCull = RT_MASK_LAMPS;
    if (!shadowsOn()) return;
    gShadowFade = smoothstep(SHADOW_FADE, SHADOW_FAR, dist);
    ivec2 px = ivec2(gl_FragCoord.xy);
    gSlotVis = texelFetch(shadowVisTex, px, 0);
    gSlotsOurs = (gSlotVis.w & SH_VALID) != 0u && (gSlotVis.w & 0xFFFFu) == uint(rec) && abs(uintBitsToFloat(gSlotVis.z) - fwd) < tol;
    if (gSlotsOurs) gSlotIds = texelFetch(shadowIdsTex, px, 0);
#endif
}

// The city's lights on the land here, facing n, its highlights by GGX of
// roughness rough and reflectance f0: their light falling on it (diff) and
// their highlights (spec). False where none reaches.
bool landLights(vec3 n, vec3 V, float rough, float f0, float fwd, float tol, out vec3 diff, out vec3 spec) {
    diff = spec = vec3(0.0);
    float dist = length(vRel);
    if (dist > LAND_LIGHTS_FAR) return false;
    int rec = landChunk(vFlat.xz);
    if (rec < 0) return false;
    gC = chunks[rec];
    gTime = frame.camPos.w;
    vec3 p = vec3(vFlat.x - gC.origin.x, vFlat.y, vFlat.z - gC.origin.z);   // chunk-local (the chunks' y is world Y)
    gWorld = p + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
    gBase = p.y;
    landSlots(rec, dist, fwd, tol);
    cityLights(p, n, V, 1.0, f0, rough, diff, spec);
    float fade = 1.0 - smoothstep(LAND_LIGHTS_FAR - 100.0, LAND_LIGHTS_FAR, dist);
    diff *= fade;
    spec *= fade;
    return true;
}
#endif

// The clipmap's samples (terrain.vert's), for the moon's shadows' march
layout(std430, set = 0, binding = 1) readonly buffer Samples { uint samples[]; };
#include "terrain/dir_shadow.glsl"

void shadePixel() {
    vec3 n = normalize(vNrm), nLand = n;
#ifdef CAVE_CUT
    bool hole = caveHoleCut(vFlat.xz, vFlat.y, 0.0);
#endif
    Cover cover = Cover(vCoverA, vCoverB);
    // Where the city's own ground lies (the cover's last byte: src/plan/plan.h
    // plan_city_ground): there its stand-in (groundUrban) from afar, no textures
    // and no grass. Not the urban weight, which stays 1 for up to a town's
    // width past the built edge; that only hazes the land as the city.
    float city = vCoverB.w;
    LandPoint l = landPoint(vRel);
    // the moon's shadow, where the sky is clear enough for it to cast one (ground.glsl landMoon)
    if (l.sky.cover < 0.99) gMoonVis = dirShadow(vFlat, nLand, frame.dirLight.xyz, length(vRel));
    RoadHit road;
    bool onRoad = roadNear(vRel.xz, road);
    // the pixel's width across and along the road, in uniform control flow
    float px = max(fwidth(road.off), 1e-3), pxs = max(fwidth(road.s), 1e-3);
#ifdef LAND_LIGHTS
    // the forward distance the shadow slots are matched by (city/lighting.glsl shadowsBegin)
    float fwd = dot(vRel, frame.camFwd.xyz), tol = fwidth(fwd) * 1.5 + fwd * 0.002 + 0.01;
    vec3 V = -normalize(vRel);
    if ((uint(frame.shadow.y) & SHADOWS_DEBUG) != 0u) {   // F10: only the light the city's lamps and the moon throw on it, shadowed
        vec3 d, sp;
        landLights(n, V, 1.0, 0.0, fwd, tol, d, sp);
        // (the moonlight its shadow takes, in red)
        float nl = max(dot(n, MOON_DIR), 0.0) * (1.0 - 0.9 * l.sky.cover), vis = landMoon(l.sky);
        vec3 moon = MOON_LIGHT * nl * vis + vec3(0.25, 0.0, 0.0) * nl * (1.0 - vis);
        outColor = vec4((d + moon) * 0.3 + vec3(0.004), 1.0);
        outRefl = vec4(0.0);
        return;
    }
#endif
    int grassView = int(frame.grass.z + 0.5);
    if (grassView != GRASS_DEBUG_OFF && grassView != GRASS_DEBUG_ALBEDO) {   // --grass-debug: the grass's rules, unlit
        outColor = vec4(grassDebug(grassView, vRel, n, city, cover, onRoad, road, l.px), 1.0);
        outRefl = vec4(0.0);
        CAVE_CUT_DISCARD
        return;
    }
    // under the asphalt the land's textures aren't read
    LandWater w;
    bool paved = onRoad && roadPaved(road, px);
    float beyond = !onRoad ? float(GRASS_ROAD_FAR) : paved ? -1.0 : abs(road.off) - road.hw;   // for the grass's field
    vec3 land = groundAlbedo(vRel, l, city, cover, !paved, beyond, nLand, w);
    if (grassView == GRASS_DEBUG_ALBEDO) {   // --grass-debug albedo: the ground's (the grass's field in it), unlit; the asphalt dark grey
        outColor = vec4((paved ? vec3(0.03) : land) * float(GRASS_DEBUG_ALBEDO_GAIN), 1.0);
        outRefl = vec4(0.0);
        CAVE_CUT_DISCARD
        return;
    }
    // lit, then the wet ground's sheen over it (the road's, and the land's beside it)
    vec3 nS = nLand, col, alb = land;
    float roughS = LAND_FILM_ROUGH, lamps = 0.0;
    if (onRoad) col = shadeRoad(land, nLand, n, vRel, l, city, road, px, pxs, w, nS, roughS, lamps, alb);
    else        col = groundUrban(land * groundLightIn(l.sky, nLand), city);
#ifdef LAND_LIGHTS
    {
        // the city's lights: on the albedo (where the city's own ground lies, groundUrban's asphalt),
        // their highlights on the ground's film or puddles where it is wet, else on the dry ground
        vec3 nW;
        float rW, shine = clamp(wetSurface(l, nS, n, roughS, w, nW, rW), 0.0, 1.0);
        gStreak = 1.0 + 3.5 * l.sky.wet * shine;   // the wet ground streaks them towards the eye, as the streets do
        vec3 d, sp;
        if (landLights(normalize(mix(nS, nW, shine)), V, mix(LAND_DRY_ROUGH, rW, shine), mix(LAND_DRY_F0, WATER_F0, shine), fwd, tol, d, sp))
            col += mix(alb, vec3(0.02), city) * d + sp;
    }
#endif
    col += wetSheen(vRel, l, nS, n, roughS, w, onRoad, road, lamps);
    outColor = vec4(groundHazed(col, vRel, vUrban), 1.0);
    outRefl = vec4(0.0);
    CAVE_CUT_DISCARD
}

EXPOSE_OUT   // main: shadePixel, its colour pre-exposed (common.glsl)
