#version 450
/* Blends the half-resolution clouds (clouds.comp) over the full-resolution HDR
 * scene: out = scene * transmittance + in-scattered light (set by the
 * pipeline's blend state). The upsample is depth-aware: of the four nearest
 * half-res texels it weights those whose march stopped at about this pixel's
 * depth, so cloud in front of the sky never smears over a tower's edge. The
 * rain mist (clouds.comp: mistLight), in front of everything, is added on top
 * with the same weights. */
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D cloudTex;
layout(set = 0, binding = 1) uniform sampler2D cloudDistTex;
layout(set = 0, binding = 2) uniform sampler2D sceneDepth;
layout(set = 0, binding = 3) uniform sampler2D mistTex;
layout(push_constant) uniform Push { float zn, zf; } pc;

float forwardDist(float d) { return d > 0.0 ? pc.zn * pc.zf / (d * (pc.zf - pc.zn) + pc.zn) : 1e9; }

void main() {
    float z = forwardDist(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    ivec2 hs = textureSize(cloudTex, 0);
    vec2 u = gl_FragCoord.xy * 0.5 - 0.5;
    ivec2 i0 = ivec2(floor(u));
    vec2 f = fract(u);
    vec4 acc = vec4(0.0);
    vec3 mist = vec3(0.0), bestMist = vec3(0.0);
    float wsum = 0.0, bestDz = 1e30;
    vec4 best = vec4(0.0, 0.0, 0.0, 1.0);
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        ivec2 c = clamp(i0 + o, ivec2(0), hs - 1);
        vec4 cv = texelFetch(cloudTex, c, 0);
        float cz = texelFetch(cloudDistTex, c, 0).r;
        float dz = abs(cz - z) / max(min(cz, z), 1.0);   // relative depth difference
        vec2 b = mix(1.0 - f, f, vec2(o));
        float w = b.x * b.y * exp(-dz * 24.0);
        vec3 mv = texelFetch(mistTex, c, 0).rgb;
        acc += cv * w;
        mist += mv * w;
        wsum += w;
        if (dz < bestDz) { bestDz = dz; best = cv; bestMist = mv; }
    }
    outColor = wsum > 1e-4 ? acc / wsum : best;
    outColor.rgb += wsum > 1e-4 ? mist / wsum : bestMist;
}
