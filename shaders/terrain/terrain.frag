#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "ground.glsl"
#include "roads.glsl"

/* The terrain clipmap (terrain.vert): the bare land (ground.glsl) and the
 * roads between the settlements painted on it (roads.glsl). */

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing reflective (city.frag)

void main() {
    vec3 n = normalize(vNrm);
    vec3 col = groundLit(vRel, n, vUrban);
    RoadHit road;
    if (roadNear(vRel.xz, road)) col = shadeRoad(col, vRel, n, road);
    outColor = vec4(groundHazed(col, vRel, vUrban), 1.0);
    outRefl = vec4(0.0);
}
