#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "flame.glsl"

/* A flare's fire, ray-marched through its box and added to the HDR scene
 * (after the clouds, before bloom: src/render/renderer_passes.c record_clouds).
 *
 * The fire is a tapered column above the nozzle: narrow where it leaves, it
 * widens as it climbs. flameGout (common.glsl) says which stretch of it is
 * burning now: a gout shooting up, cut off and burning away behind itself, a
 * steady flame, and always the pilot at the nozzle. Through that envelope two
 * fetches of the tiling cloud noise volume (its billows, warped by its finer
 * octaves), scrolling up at the speed the gas rises, tear the edges into
 * licking tongues. Each sample's temperature (hottest in the dense core and low
 * down, cooling as it climbs and as the gout spends itself) picks its colour
 * from a blackbody-like ramp (fireColor) and its brightness; the fire hides a
 * little of itself, and the march stops at whatever stands in front. */

layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D noiseTex;   // the clouds' noise volume (cloud_noise.comp)
layout(location = 0) in vec3 vRel;
layout(location = 1) flat in uint vIdx;
layout(location = 0) out vec4 outColor;

const float EMIT = 3.5;    // radiance of fire at its hottest, per metre of dense flame

void main() {
    Flame f = flames[vIdx];
    vec3 lo, hi;
    flameBox(f, lo, hi);
    float d = length(vRel);
    vec3 dir = vRel / d;
    vec3 inv = 1.0 / (dir + vec3(dir.x >= 0.0 ? 1e-7 : -1e-7, dir.y >= 0.0 ? 1e-7 : -1e-7, dir.z >= 0.0 ? 1e-7 : -1e-7));
    vec3 ta = lo * inv, tb = hi * inv;
    vec3 tn = min(ta, tb), tx = max(ta, tb);
    float t0 = max(max(tn.x, tn.y), max(tn.z, 0.0)), t1 = min(min(tx.x, tx.y), tx.z);
    // nothing to march, or this is a near face: the far one marches the whole box
    if (t1 <= t0 || d < t1 - max(0.05, t1 * 2e-3)) discard;

    float time = frame.camPos.w;
    Gout g = flameGout(f.id, time);
    float L = f.reach, R = f.radius;
    // the heights that burn now: the pilot at the nozzle up to the head of the column
    float y0 = f.pos.y - 0.3, y1 = f.pos.y + (g.head + 0.3) * L;
    if (abs(dir.y) > 1e-5) {
        float a = (y0) / dir.y, b = (y1) / dir.y;
        t0 = max(t0, min(a, b)); t1 = min(t1, max(a, b));
    }
    // what stands in front
    float zScene = forwardDist(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    t1 = min(t1, zScene / max(dot(dir, frame.camFwd.xyz), 1e-3));
    if (t1 <= t0) discard;

    // fewer steps far off, where the fire is a few pixels across
    int N = int(mix(28.0, 10.0, smoothstep(120.0, 900.0, t0)));
    float dt = (t1 - t0) / float(N), jit = ign(gl_FragCoord.xy + vec2(fract(time * 13.7) * 64.0, 0.0));
    float rise = g.steady ? 4.0 + 0.15 * L : 1.1 * L;   // how fast the gas climbs (m/s): a gout is a jet
    vec3 acc = vec3(0.0);
    float T = 1.0;
    float burn = g.head > 0.2 ? clamp(g.tail / g.head, 0.0, 1.0) : 0.0;   // how far the cut-off column has burnt away
    for (int i = 0; i < N; ++i) {
        vec3 p = dir * (t0 + (float(i) + jit) * dt) - f.pos;
        float h = p.y / L;
        if (h > g.head + 0.3 || (h < g.tail - 0.3 && h > 0.14)) continue;   // nowhere near the fire
        // turbulence: billows warped by finer eddies, all carried up with the gas
        vec3 q = vec3(p.x, p.y - time * rise, p.z) / (R * 2.4) + vec3(float(f.id) * 0.173, 0.0, float(f.id) * 0.311);
        vec4 nz = texture(noiseTex, q * 0.22);
        vec3 warp = (nz.gba - 0.5) * 2.0;
        float fine = texture(noiseTex, q * 0.57 + warp * 0.25 + vec3(0.0, -time * 0.35, 0.0)).r;
        float turb = 0.55 * nz.r + 0.45 * fine;
        // the burning stretch of the column, its ends torn by the turbulence into tongues,
        // and the pilot always alight at the nozzle
        float hn = h + (turb - 0.5) * 0.45;
        float col = smoothstep(g.tail - 0.05, g.tail + 0.15, hn) * (1.0 - smoothstep(g.head - 0.3, g.head + 0.05, hn));
        float pilot = (1.0 - smoothstep(0.05, 0.16, h + (turb - 0.5) * 0.08)) * smoothstep(-0.03, 0.0, h) * 0.85;
        float win = max(col, pilot);
        if (win <= 0.002) continue;
        // the envelope: narrow at the nozzle, widening as it climbs, rounding off at the head;
        // the cut-off column swells as it burns away, and breaks up into patches
        float r = R * (0.2 + 0.8 * smoothstep(-0.05, 0.55, h)) * (1.0 + 0.4 * max(h - 1.0, 0.0) + 1.3 * burn)
                    * (0.55 + 0.45 * smoothstep(g.head + 0.05, g.head - 0.35, h));
        float rad = length(p.xz + warp.xz * R * 0.35) / r;
        float dens = smoothstep(0.0, 1.0, (1.0 - rad) * 1.2 + (turb - 0.5) * 1.6) * win;
        dens *= smoothstep(burn * 0.35, burn * 0.35 + 0.25, turb);
        if (dens <= 0.002) continue;
        // hottest in the dense core and low down; cooling as it climbs and as the gout is spent
        float up = clamp((h - g.tail) / max(g.head - g.tail, 0.1), 0.0, 1.0);   // up what is still burning
        // ...and the last of a burnt-away column dies out rather than lingering as a ball
        float temp = 0.95 * dens * (1.1 - 0.45 * up) * (0.7 + 0.3 * g.power) * (h < 0.16 ? 1.0 : 1.0 - smoothstep(0.55, 0.95, burn));   // (not the pilot)
        acc += fireColor(temp) * (temp * temp * EMIT * dens * dt * T);
        T *= exp(-dens * dt * 0.22);
        if (T < 0.02) break;
    }
    float fog = fogAmount(dir * t0, t0);
    outColor = vec4(acc * (1.0 - fog), 0.0);
}
