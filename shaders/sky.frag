#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 dir = normalize(frame.rayBasis[2].xyz + ndc.x * frame.rayBasis[0].xyz - ndc.y * frame.rayBasis[1].xyz);
    vec3 col = hazeColor(dir);

    // Low smog banks lit from below by the city.
    if (dir.y > 0.0) {
        vec2 p = dir.xz / (dir.y + 0.08);
        float n = fbm(p * 1.6 + vec2(frame.camPos.w * 0.004, 0.0), 0x5C1Du);
        float band = smoothstep(0.35, 0.85, n) * (1.0 - smoothstep(0.0, 0.6, dir.y));
        col += vec3(0.075, 0.036, 0.030) * band;
    }
    outColor = vec4(col, 1.0);
}
