#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "ground.glsl"
#include "roads.glsl"

/* The terrain clipmap (terrain.vert): the bare land (ground.glsl) and the
 * roads between the settlements laid on it (roads.glsl). */

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 3) in vec4  vCoverA;
layout(location = 4) in vec4  vCoverB;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing reflective (city.frag)

void main() {
    vec3 n = normalize(vNrm), nLand = n;
    Cover cover = Cover(vCoverA, vCoverB);
    LandPoint l = landPoint(vRel);
    RoadHit road;
    bool onRoad = roadNear(vRel.xz, road);
    // the pixel's width across and along the road, in uniform control flow
    float px = max(fwidth(road.off), 1e-3), pxs = max(fwidth(road.s), 1e-3);
    // under the asphalt the land's textures aren't read
    vec3 land = groundAlbedo(vRel, l, vUrban, cover, !(onRoad && roadPaved(road, px)), nLand);
    vec3 col = onRoad ? shadeRoad(land, nLand, n, vRel, l, vUrban, road, px, pxs)
                      : groundUrban(land * groundLight(vRel, nLand), vUrban);
    outColor = vec4(groundHazed(col, vRel, vUrban), 1.0);
    outRefl = vec4(0.0);
}
