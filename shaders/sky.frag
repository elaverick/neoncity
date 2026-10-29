#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* The sky behind everything: the haze (hazeColor), with low smog banks lit
 * from below over and near a city; beyond the haze, the stars and the moon,
 * dimmed by the air they shine through. The cities' clouds (clouds.comp) are
 * laid over all of it. */

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing reflective (city.frag)

// How much of the light from beyond the air along dir gets through its haze.
float skyClear(vec3 dir) {
    const float FAR = 1e6;
    return 1.0 - hazeAmount(dir * FAR, FAR, airDensity());
}

/* Stars: one in about half the cells of a grid over each face of a cube round
 * the camera, at a random spot in its cell. Their brightness follows a power
 * law: most are faint, and a few are bright. Their colour runs from
 * blue-white to orange. Each is drawn as a soft point about a pixel across
 * (px: a pixel's angle, radians), so it neither shimmers nor drops out as it
 * moves over the pixels. Near the horizon they twinkle. The cube is fixed
 * against the compass, as the moon is: dir is turned from the frame's axes
 * into east, up and south (frame.moon.w), so the stars don't turn when the
 * frame changes. */
const float STAR_CELLS = 180.0;   // cells along a face's edge
vec3 stars(vec3 dir, float px) {
    vec2 e = vec2(cos(frame.moon.w), sin(frame.moon.w));   // east, in the frame's (x, z)
    dir = vec3(dot(dir.xz, e), dir.y, dot(dir.xz, vec2(-e.y, e.x)));
    vec3 a = abs(dir);
    uint face;
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = dir.x > 0.0 ? 0u : 1u; uv = dir.zy / a.x; }
    else if (a.y >= a.z)          { face = dir.y > 0.0 ? 2u : 3u; uv = dir.xz / a.y; }
    else                          { face = dir.z > 0.0 ? 4u : 5u; uv = dir.xy / a.z; }
    vec2 g = (uv * 0.5 + 0.5) * STAR_CELLS;
    // a pixel in cells: the face stretches towards its corners
    float pc = px * 0.5 * STAR_CELLS * (1.0 + 0.5 * dot(uv, uv));
    float s2 = 2.0 * 0.36 * pc * pc;   // the blur: a gaussian of 0.6 pixel
    float twinkle = 0.45 * (1.0 - smoothstep(0.02, 0.35, dir.y));
    ivec2 c0 = ivec2(floor(g));
    vec3 sum = vec3(0.0);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            ivec2 c = c0 + ivec2(i, j);
            uint h = hash3i(0x57A2u, c.x, c.y, int(face));
            if (hfloat(h) > 0.45) continue;
            vec2 p = vec2(c) + vec2(hfloat(hash_combine(h, 1u)), hfloat(hash_combine(h, 2u)));
            vec2 d = g - p;
            float dd = dot(d, d);
            if (dd > 9.0 * s2) continue;
            // flux: Fmin u^(-2/3), as for stars spread evenly through space
            float F = 0.02 * pow(max(hfloat(hash_combine(h, 3u)), 0.002), -0.6667);
            F *= 1.0 + twinkle * sin(frame.camPos.w * (9.0 + 7.0 * hfloat(hash_combine(h, 4u))) + 6.2832 * hfloat(hash_combine(h, 5u)));
            vec3 tint = mix(vec3(0.72, 0.82, 1.0), vec3(1.0, 0.82, 0.62), hfloat(hash_combine(h, 6u)));
            sum += tint * F * exp(-dd / s2) / 2.26;   // 2.26 = 2 pi 0.6^2: the flux spread over the blur
        }
    return sum;
}

/* The moon: a waning gibbous disc MOON_RADIUS across, lit from the side by
 * the sun below the horizon, with darker seas and a faint earthshine on its
 * unlit side; its edge anti-aliased over a pixel. */
const vec3 MOON_SURFACE = vec3(1.0, 0.96, 0.90) * 2.5;
vec3 moon(vec3 dir, float px) {
    float c = dot(dir, MOON_DIR);
    if (c < cos(MOON_RADIUS + 2.0 * px)) return vec3(0.0);
    vec3 T = normalize(cross(vec3(0.0, 1.0, 0.0), MOON_DIR)), B = cross(MOON_DIR, T);
    vec2 q = vec2(dot(dir, T), dot(dir, B)) / MOON_RADIUS;   // on the disc: |q| < 1, y up the sky
    float disc = clamp((1.0 - length(q)) * MOON_RADIUS / px + 0.5, 0.0, 1.0);
    vec3 nrm = vec3(q, sqrt(max(1.0 - dot(q, q), 0.0)));      // its surface; z towards the eye
    const vec3 SUN = vec3(-0.72, 0.30, 0.62);                 // from the moon, the sun lies to the left and behind it
    float lit = smoothstep(-0.04, 0.12, dot(nrm, SUN)) * (0.55 + 0.45 * max(dot(nrm, SUN), 0.0));
    float seas = fbm(q * 2.3 + 4.0, 0x300Fu) * 0.7 + 0.3 * vnoise(q * 9.0, 0x3010u);
    float albedo = mix(1.0, 0.6, smoothstep(0.42, 0.62, seas));
    return MOON_SURFACE * albedo * (lit + 0.012) * disc;
}

void main() {
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 dir = normalize(frame.rayBasis[2].xyz + ndc.x * frame.rayBasis[0].xyz - ndc.y * frame.rayBasis[1].xyz);
    vec3 col = hazeColor(dir);

    // Low smog banks lit from below by the city.
    if (dir.y > 0.0 && frame.sky.x > 0.0) {
        vec2 p = dir.xz / (dir.y + 0.08);
        float n = fbm(p * 1.6 + vec2(frame.camPos.w * 0.004, 0.0), 0x5C1Du);
        float band = smoothstep(0.35, 0.85, n) * (1.0 - smoothstep(0.0, 0.6, dir.y));
        col += vec3(0.075, 0.036, 0.030) * band * frame.sky.x;
    }

    float px = length(frame.rayBasis[1].xyz) * 2.0 * frame.screen.w;   // a pixel's angle (radians)
    col += (stars(dir, px) + moon(dir, px)) * skyClear(dir);
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
