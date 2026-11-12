#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* A puff of a noodle bar's steam (steam.vert): a soft blob, torn into wisps by
 * the clouds' noise volume (rolling as it goes, more torn as it ages), faded
 * into whatever stands close behind it and hidden by what stands in front
 * (the pass has no depth attachment: it reads the scene's depth), blended
 * over the scene by its transmittance as the plumes are. */

layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D noiseTex;   // the clouds' noise volume (cloud_noise.comp)
layout(location = 0) in vec2 vUV;
layout(location = 1) flat in vec4 vCol;
layout(location = 2) in vec3 vRel;
layout(location = 3) flat in vec4 vPuff;
layout(location = 0) out vec4 outColor;

void shadePixel() {
    float r2 = dot(vUV, vUV);
    if (r2 >= 1.0 || vCol.a < 1e-3) discard;
    float zScene = forwardDist(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    float zHere = dot(vRel, frame.camFwd.xyz);
    // soft against what stands behind it, gone behind what stands in front
    float soft = clamp((zScene - zHere) / max(0.6 * vPuff.x, 0.03), 0.0, 1.0);
    if (soft <= 0.0) discard;
    float t = frame.camPos.w;
    vec3 nc = vec3(vUV * 0.18 + vec2(vPuff.y * 13.0, vPuff.y * 7.0), t * 0.035 + vPuff.y * 5.0);
    vec4 nz = texture(noiseTex, nc);
    float fine = texture(noiseTex, nc * 2.7 + 0.31).g;
    // its outline torn by the noise (warped), never a disc
    vec2 w = vUV + (nz.gb - 0.5) * 0.9;
    float blob = 1.0 - min(dot(w, w), 1.0);
    blob *= blob;
    float tear = 0.15 + 0.4 * vPuff.z;   // older puffs more torn
    float d = clamp(blob * blob * (1.0 + 2.0 * (nz.r - 0.5) + 1.2 * (fine - 0.5) - tear * 0.6), 0.0, 1.0) * (1.0 - r2);   // soft, wispy: no hard rim
    float a = clamp(vCol.a * d * soft, 0.0, 0.95);
    if (a < 0.002) discard;
    outColor = vec4(vCol.rgb * a, 1.0 - a);
}

EXPOSE_OUT   // main: shadePixel, its colour pre-exposed (common.glsl)
