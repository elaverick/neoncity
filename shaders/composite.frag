#version 450
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;
layout(set = 0, binding = 3) uniform sampler2D mistTex;   // rain mist (clouds.comp): how lit the air is along each ray
layout(push_constant) uniform Push {
    float time, bloom, exposure, grain;
    float width, height, aberration, vignette;
    vec4 right;   // camera ray basis (FrameUBO.rayBasis); w = near plane
    vec4 up;      // w = far plane
    vec4 fwd;     // w = rain intensity
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

uint hash3u(uint a, uint b, uint c) {
    uint h = a * 0x9E3779B1u ^ b * 0x85EBCA77u ^ c * 0xC2B2AE3Du;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
float hf(uint h, uint k) { return float(hash3u(h, k, 0x51EDu) >> 8) * (1.0 / 16777216.0); }

/* Falling rain: drops on five cylinders around the camera, 2-40 m out.
 * Each cylinder is unrolled (arc length x height, in metres) and cut into
 * columns; a column holds drops that repeat every couple of metres down it,
 * fall at the speed of rain and slant with the wind. Each drop is drawn as
 * what a camera sees of it (Garg & Nayar): a motion-blurred streak, tapered
 * at both ends, with glints along it from the drop's oscillation, and its
 * own brightness. Near drops are out of focus, wide, soft and faint; far
 * ones are anti-aliased by their pixel footprint into a faint veil. The scene
 * depth hides drops behind walls. Returns coverage x brightness. */
float rainCover(vec3 dir, float sceneFwd, float t, float rain) {
    float hz = length(dir.xz);
    if (hz < 0.08) return 0.0;   // straight up or down
    float az = atan(dir.x, dir.z);
    float pixAngle = 2.0 * length(pc.up.xyz) / pc.height;   // radians per pixel
    float cover = 0.0;
    for (int k = 0; k < 5; ++k) {
        float D = 2.0 * pow(2.1, float(k));
        float vis = smoothstep(0.0, 1.0, sceneFwd - D * dot(dir, pc.fwd.xyz) / hz);
        if (vis <= 0.0) continue;
        vec2 q = vec2(az * D, dir.y / hz * D + t * 9.0 + float(k) * 17.3);
        q.x += q.y * 0.12;   // wind
        const float CW = 0.12, L = 2.2;
        float cx = floor(q.x / CW);
        uint hc = hash3u(uint(int(cx)), uint(k), 0x2A1Eu);
        float vv = q.y / L + hf(hc, 1u);
        uint hd = hash3u(hc, uint(int(floor(vv))), 0x2A1Fu);
        if (hf(hd, 0u) > 0.25 + 0.55 * rain) continue;
        float dx = abs(q.x - (cx + 0.15 + 0.7 * hf(hd, 1u)) * CW);
        float pxw = D * pixAngle;                          // metres per pixel at this distance
        float blur = 0.012 / (D * D);                      // defocus: wide near the lens
        float wdt = max(max(pxw, 0.003), blur);
        float len = 0.25 + 0.5 * hf(hd, 2u);
        float fy = fract(vv) * L / len;                    // 0..1 along the streak
        if (fy >= 1.0) continue;
        float taper = sin(fy * 3.14159);                   // thin, faint ends
        float glint = 0.55 + 0.45 * pow(abs(sin((fy * 3.0 + hf(hd, 3u)) * 3.14159)), 4.0);
        float across = exp(-dx * dx / (wdt * wdt * taper * taper * 0.3 + 1e-9));
        float bright = 0.35 + 0.65 * hf(hd, 4u);
        cover += across * taper * glint * bright * (0.003 / wdt) * vis * (1.0 - 0.12 * float(k));
    }
    return cover;
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

    // Rain, lit by what lights the air around it: the rain mist along this
    // ray (lamp cones, shop spill and neon), the glow of nearby lights (the
    // bloom) and a refracted glimpse of the scene behind each drop. Away from
    // light it all but disappears, as rain does at night.
    float rain = pc.fwd.w;
    if (rain > 0.0) {
        vec2 ndc = uv * 2.0 - 1.0;
        vec3 dir = normalize(pc.fwd.xyz + ndc.x * pc.right.xyz - ndc.y * pc.up.xyz);
        float z = texelFetch(depthTex, ivec2(gl_FragCoord.xy), 0).r;
        float zn = pc.right.w, zf = pc.up.w;
        float sceneFwd = z > 0.0 ? zn * zf / (z * (zf - zn) + zn) : 1e9;
        float cover = rainCover(dir, sceneFwd, pc.time, rain);
        if (cover > 0.0) {
            vec3 behind = texture(hdrTex, uv + vec2(0.0, 0.012)).rgb;
            vec3 mist = texture(mistTex, uv).rgb / max(rain, 0.05);
            vec3 light = mist * 4.0 + bl * 1.2 + behind * 0.25 + vec3(0.006, 0.007, 0.010);
            col += light * min(cover, 1.0) * 0.8;
        }
    }

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
