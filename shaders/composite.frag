#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(push_constant) uniform Push {
    float time, bloom, exposure, grain;
    float width, height, aberration, vignette;
} pc;

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float hash12(vec2 p) {
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
    uint n = (q.x ^ q.y) * 1597334673u;
    return float(n) * (1.0 / 4294967295.0);
}

void main() {
    vec2 uv = vUV;
    vec2 c = uv - 0.5;
    // Slight lens chromatic aberration toward the edges.
    vec2 off = c * dot(c, c) * pc.aberration;
    vec3 col;
    col.r = texture(hdrTex, uv - off).r;
    col.g = texture(hdrTex, uv).g;
    col.b = texture(hdrTex, uv + off).b;
    vec3 bl = texture(bloomTex, uv).rgb;
    col += bl * pc.bloom;

    col *= pc.exposure;
    // Blade Runner grade: cyan-teal shadows, sodium-orange highlights.
    float l = dot(col, vec3(0.2126, 0.7152, 0.0722));
    vec3 shadowTint = vec3(0.80, 1.02, 1.12);
    vec3 highTint = vec3(1.10, 0.97, 0.84);
    col *= mix(shadowTint, highTint, smoothstep(0.02, 0.8, l));

    col = aces(col);

    // Vignette
    float v = 1.0 - pc.vignette * smoothstep(0.25, 0.95, length(c * vec2(pc.width / pc.height, 1.0)) * 1.15);
    col *= v;

    // Film grain (animated), strongest in the mid-tones.
    float g = hash12(gl_FragCoord.xy + vec2(fract(pc.time * 7.13) * 1913.0, fract(pc.time * 3.71) * 2777.0)) - 0.5;
    col += g * pc.grain * (0.25 + 0.75 * sqrt(max(col, vec3(0.0)))) * 0.5;

    outColor = vec4(max(col, vec3(0.0)), 1.0);
}
