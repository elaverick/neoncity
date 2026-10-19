#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "ground.glsl"

/* The planet's patches (planet.vert): the bare land (ground.glsl), cut away
 * inside the clipmap's window less TERRAIN_PLANET_HOLE_M, where the clipmap
 * draws the ground. */

layout(push_constant) uniform Patch { vec4 ax, up, az, centre, morph, window; } pc;

layout(location = 0) in vec3  vRel;
layout(location = 1) in vec3  vNrm;
layout(location = 2) in float vUrban;
layout(location = 3) in vec2  vChart;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing reflective (city.frag)

void main() {
    if (pc.window.z > pc.window.x) {
        const float M = TERRAIN_PLANET_HOLE_M;
        if (vChart.x > pc.window.x + M && vChart.x < pc.window.z - M && vChart.y > pc.window.y + M && vChart.y < pc.window.w - M) discard;
    }
    outColor = vec4(shadeGround(vRel, normalize(vNrm), vUrban), 1.0);
    outRefl = vec4(0.0);
}
