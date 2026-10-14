#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* Blends screen-space reflections (ssr.comp) into the HDR scene, additively.
 *
 * Wet asphalt and stone don't mirror the city: their rain-film roughness
 * smears each reflection into a long streak toward the viewer. So the hits
 * are averaged along the screen's vertical over a length that grows with
 * roughness: puddles stay sharp, while the film turns lamp heads, neon and
 * lit shopfronts into tall bars of colour. The scene pass already added the
 * sky reflection (envReflection) with this pixel's weight; the blend swaps it
 * for the screen's reflection in proportion to the confidence:
 *     out += weight * confidence * (ssr - sky). */
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 3) uniform sampler2D ssrTex;
layout(set = 0, binding = 4) uniform sampler2D reflTex;

const int TAPS = 12;   // either side

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 g = reflSample(reflTex, px);
    outColor = vec4(0.0);   // additive: zero leaves the scene as it is
    if (g.a <= 1e-3) return;
    float rough = max(g.b, 0.0);   // glass is marked negative (city.frag: GLASS_ROUGH)
    bool indoor = g.b < -2.0;      // a lobby floor (city.frag: INDOOR_ROUGH): polished, with no sky added
    if (indoor) rough = 0.06;

    ivec2 size = textureSize(ssrTex, 0);
    float len = float(size.y) * 0.11 * smoothstep(0.04, 0.4, rough);   // streak half-length, pixels
    vec3 sum = vec3(0.0);
    float conf = 0.0, wsum = 0.0;
    for (int i = -TAPS; i <= TAPS; ++i) {
        float o = float(i) / float(TAPS);
        float w = exp(-o * o * 2.5);
        // a slight sideways scatter, so neighbouring columns don't streak independently
        float side = sin(float(i) * 2.4 + float(px.y & 7)) * (1.5 + len * 0.03);
        ivec2 q = clamp(px + ivec2(int(round(side)), int(round(o * len))), ivec2(0), size - 1);
        vec4 s = texelFetch(ssrTex, q, 0);
        sum += s.rgb * s.a * w;
        conf += s.a * w;
        wsum += w;
    }
    if (conf <= 1e-4) return;
    vec3 col = sum / conf;
    conf /= wsum;

    // the sky reflection the scene pass added here (city.frag: shadeGround)
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 dir = normalize(frame.rayBasis[2].xyz + ndc.x * frame.rayBasis[0].xyz - ndc.y * frame.rayBasis[1].xyz);
    vec3 n = vec3(g.x, sqrt(max(1.0 - g.x * g.x - g.y * g.y, 0.0)), g.y);
    vec3 sky = indoor ? vec3(0.0) : envReflection(normalize(mix(reflect(dir, n), n, rough * rough)));
    outColor = vec4(g.a * conf * (col - sky), 0.0);
}
