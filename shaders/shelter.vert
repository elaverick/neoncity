#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* Shelter map (renderer.c: record_shelter): the city seen from straight
 * above, depth-only. topDown maps camera-relative x, z onto the map square
 * and height onto depth, highest nearest, so each texel keeps the highest
 * surface over that spot. */
layout(location = 0) in vec3 inPos;
layout(push_constant) uniform Push { mat4 topDown; } pc;

void main() {
    gl_Position = pc.topDown * vec4(inPos + chunks[gl_InstanceIndex].origin.xyz, 1.0);
}
