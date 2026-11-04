#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "ground.glsl"
#include "roads.glsl"
#include "grass/debug.glsl"

/* The terrain clipmap (terrain.vert): the bare land (ground.glsl) and the
 * roads between the settlements laid on it (roads.glsl). */

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 3) in vec4  vCoverA;
layout(location = 4) in vec4  vCoverB;
layout(location = 0) out vec4 outColor;
// Nothing for the screen-space reflections (city.frag): the land's wet
// sheen reflects its own sky (landReflection), not the city's
// (envReflection), which ssr_apply.frag takes the reflections' place of.
layout(location = 1) out vec4 outRefl;

void main() {
    vec3 n = normalize(vNrm), nLand = n;
    Cover cover = Cover(vCoverA, vCoverB);
    LandPoint l = landPoint(vRel);
    RoadHit road;
    bool onRoad = roadNear(vRel.xz, road);
    // the pixel's width across and along the road, in uniform control flow
    float px = max(fwidth(road.off), 1e-3), pxs = max(fwidth(road.s), 1e-3);
    int grassView = int(frame.grass.z + 0.5);
    if (grassView != GRASS_DEBUG_OFF) {   // --grass-debug: the grass's rules, unlit
        outColor = vec4(grassDebug(grassView, vRel, n, vUrban, cover, onRoad, road, l.px), 1.0);
        outRefl = vec4(0.0);
        return;
    }
    // under the asphalt the land's textures aren't read
    LandWater w;
    vec3 land = groundAlbedo(vRel, l, vUrban, cover, !(onRoad && roadPaved(road, px)), nLand, w);
    // lit, then the wet ground's sheen over it (the road's, and the land's beside it)
    vec3 nS = nLand, col;
    float roughS = LAND_FILM_ROUGH, lamps = 0.0;
    if (onRoad) col = shadeRoad(land, nLand, n, vRel, l, vUrban, road, px, pxs, w, nS, roughS, lamps);
    else        col = groundUrban(land * groundLightIn(l.sky, nLand), vUrban);
    col += wetSheen(vRel, l, nS, n, roughS, w, onRoad, road, lamps);
    outColor = vec4(groundHazed(col, vRel, vUrban), 1.0);
    outRefl = vec4(0.0);
}
