#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "flame.glsl"
#include "smoke.glsl"

/* A stack's plume, ray-marched through its box and blended over the HDR scene
 * by its transmittance (after the clouds, before the fire and bloom:
 * src/render/renderer_passes.c record_clouds).
 *
 * The density is a field of noise carried through a velocity field. The gas
 * rises out of the mouth, slowing as it climbs (plumeAge), while the wind
 * carries it downwind: the wind's history at the mouth over a parcel's life
 * (KNOTS samples of gustAt, the gusts that swing the cables) says how far
 * each height has drifted, so a gust bends the plume over and the kink climbs
 * away with the gas that felt it. About that bent centreline:
 * - billows: the cloud noise volume, scaled to the plume's width where they
 *   are, so they swell as it spreads, and carried up at the gas's speed;
 * - eddies: finer noise carried by the local velocity (rise, wind and a swirl
 *   from the billows' warp); a gust passing speeds and roughens them;
 * both flow maps (two phases of advection cross-faded, so the noise rides the
 * flow without smearing however long it runs);
 * - and the whole thinned as it spreads, eaten from the edges as it climbs,
 *   gone by the top of its rise.
 * It is lit by the sodium-lit overcast and the moon from above (one tap
 * towards them for its own shadow), the city's glow from below, any flare
 * near it (flickering with flameGout), and, for a refinery's smoke, the flue's
 * fire glowing in its throat. */

layout(set = 0, binding = 4) uniform sampler2D sceneDepth;
layout(set = 0, binding = 5) uniform sampler3D noiseTex;   // the clouds' noise volume (cloud_noise.comp)
layout(location = 0) in vec3 vRel;
layout(location = 1) flat in uint vIdx;
layout(location = 0) out vec4 outColor;

const int   KNOTS  = 8;     // the wind's history over a parcel's life, sampled this many times
const float FLOW_T = 2.0;   // seconds each phase of the advected noise runs before it fades out and restarts (a vent's: 4)

Plume P;
float gTime, gTop;          // gTop: the age of the gas at the top of the rise
vec2  gMouth;               // the mouth, world xz
vec3  gSeed;                // this plume's place in the noise
float gDrift[KNOTS];        // how far the wind has carried gas of each knot's age...
float gWind[KNOTS];         // ...and the wind as it left the mouth (m/s)

void windAt(float a, out float drift, out float wind) {
    float x = clamp(a / gTop, 0.0, 1.0) * float(KNOTS - 1);
    int i = min(int(x), KNOTS - 2);
    float f = x - float(i);
    drift = mix(gDrift[i], gDrift[i + 1], f);
    wind = mix(gWind[i], gWind[i + 1], f);
}

// Extinction (per metre) at p (relative to the mouth); r the plume's radius there,
// near how close to the mouth (1 at it).
float plumeSigma(vec3 p, out float r, out float near) {
    float h = max(p.y, 0.0);
    float a = plumeAge(P, h), d, u;
    windAt(a, d, u);
    r = plumeRadius(P, h, d);
    near = exp(-h / (P.radius * 1.5));
    float path = length(vec2(h, d)), k = path / P.rise;   // how far along its path; k 1 where it has gone
    // across the bent plume: slanted by the wind, its level section stretches downwind
    vec2 off = p.xz - GUST_WIND * d;
    float slope = u / plumeRiseSpeed(P, h);
    vec2 q = vec2(dot(off, GUST_WIND) / sqrt(1.0 + slope * slope), dot(off, vec2(-GUST_WIND.y, GUST_WIND.x))) / r;
    if (k > 1.1 || dot(q, q) > 2.6 || p.y < -0.6) return 0.0;   // beyond the turbulence's reach

    bool vent = P.kind == PLUME_VENT;
    float gust = gustAt(gMouth + p.xz, gTime);   // the gust here now
    float turb = 0.35 + (vent ? 0.25 : 0.65) * gust;   // down in the street a gust only stirs a wisp
    // Both layers are flow maps: each of two phases carries its noise along the
    // flow for flowT seconds, fading in and out so the other covers its restart
    // and nothing is stretched further than one phase's worth of the flow. Each
    // plume keeps its own clock, and along a wisp it runs later the further the
    // steam has come, so its fades never come all at once (a pulse).
    float flowT = vent ? 4.0 : FLOW_T;
    float ph = fract(gTime / flowT + gSeed.x + (vent ? 0.12 * path : 0.0)), w0 = 1.0 - abs(2.0 * ph - 1.0);
    float ph1 = fract(ph + 0.5), keep = 1.0 / sqrt(w0 * w0 + (1.0 - w0) * (1.0 - w0));   // keeps its contrast through the fade
    // billows, the size of the plume where they are: across it, and along its path as the gas goes
    float w = plumeRiseSpeed(P, h), speed = length(vec2(w, u));
    vec4 n0 = texture(noiseTex, vec3(q.x, (path - speed * ph * flowT) / r, q.y) * 0.35 + gSeed);
    vec4 n1 = texture(noiseTex, vec3(q.x, (path - speed * ph1 * flowT) / r, q.y) * 0.35 + gSeed + 0.37);
    vec4 nz = n0 * w0 + n1 * (1.0 - w0);
    float billow = (nz.r - 0.5) * keep + 0.5;
    vec3 warp = (nz.gba - 0.5) * 2.0;
    q += warp.xz * 0.3 * turb;
    // eddies a couple of metres across, carried by the local velocity: the rise, the wind, and a swirl
    vec3 vel = vec3(GUST_WIND.x, 0.0, GUST_WIND.y) * (PLUME_BREEZE + PLUME_GUST * gust) * plumeWind(P) + vec3(0.0, w, 0.0)
             + warp * (1.0 + 2.5 * turb) * (vent ? 0.25 : 1.0);   // a wisp only drifts and curls
    float eddy = vent ? 0.16 : 0.07;   // a wisp's eddies are under a metre, not a couple of metres
    float f0 = texture(noiseTex, (p - vel * ph * flowT) * eddy + gSeed.zxy).g;
    float f1 = texture(noiseTex, (p - vel * ph1 * flowT) * eddy + gSeed.zxy + 0.5).g;
    float fine = (f0 * w0 + f1 * (1.0 - w0) - 0.5) * keep + 0.5;
    float shape = 0.5 + (0.6 * billow + 0.4 * fine - 0.5) * ((vent ? 2.0 : 1.4) + 0.6 * turb);

    // the billows decide where it is (as the clouds' coverage does): solid in the
    // core, lumps and holes towards its edges; a manhole's steam only in torn wisps
    float cover = 1.0 - smoothstep(0.0, 1.25, length(q)) - (vent ? 0.08 : 0.0);
    float dens = clamp((shape - 1.0 + cover) * 2.5, 0.0, 1.0);
    dens *= smoothstep(k * 0.6 - 0.1, k * 0.6 + 0.2, shape);          // eaten from the edges as it climbs
    dens *= 1.0 - smoothstep(0.6, 1.0, k + (shape - 0.5) * 0.3);      // gone by the top
    dens *= smoothstep(-0.6, 0.3, p.y);
    float sigma = P.kind == PLUME_SMOKE ? 0.9 : vent ? 0.9 : 0.6;
    return sigma * dens * sqrt(P.radius * 1.3 / r) * (1.0 - 0.4 * k);   // thinned as it spreads
}

void main() {
    P = plumes[vIdx];
    vec3 lo, hi;
    plumeBox(P, lo, hi);
    float d = length(vRel);
    vec3 dir = vRel / d;
    vec3 inv = 1.0 / (dir + vec3(dir.x >= 0.0 ? 1e-7 : -1e-7, dir.y >= 0.0 ? 1e-7 : -1e-7, dir.z >= 0.0 ? 1e-7 : -1e-7));
    vec3 ta = lo * inv, tb = hi * inv;
    vec3 tn = min(ta, tb), tx = max(ta, tb);
    float t0 = max(max(tn.x, tn.y), max(tn.z, 0.0)), t1 = min(min(tx.x, tx.y), tx.z);
    // nothing to march, or this is a near face: the far one marches the whole box
    if (t1 <= t0 || d < t1 - max(0.05, t1 * 2e-3)) discard;
    // what stands in front
    float zScene = forwardDist(texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r);
    t1 = min(t1, zScene / max(dot(dir, frame.camFwd.xyz), 1e-3));
    if (t1 <= t0) discard;

    gTime = frame.camPos.w;
    gMouth = P.pos.xz + frame.camPos.xz;
    gSeed = vec3(hfloat(P.seed), hfloat(hash_u32(P.seed)), hfloat(hash_u32(P.seed + 1u)));
    // the wind's history at the mouth: how far it has carried the gas of each age
    gTop = plumeAge(P, P.rise);
    float da = gTop / float(KNOTS - 1);
    gWind[0] = (PLUME_BREEZE + PLUME_GUST * gustAt(gMouth, gTime)) * plumeWind(P);
    gDrift[0] = 0.0;
    for (int k = 1; k < KNOTS; ++k) {
        gWind[k] = (PLUME_BREEZE + PLUME_GUST * gustAt(gMouth, gTime - float(k) * da)) * plumeWind(P);
        gDrift[k] = gDrift[k - 1] + 0.5 * (gWind[k - 1] + gWind[k]) * da;
    }

    // the light on it: the overcast and the moon above, the city's glow below,
    // the flares near it (the two nearest the camera within reach)
    float city = frame.sky.x;
    vec3 above = CLOUD_DECK * (1.0 + 1.5 * city) + MOON_LIGHT * 0.35 * (1.0 - frame.sky.z);
    vec3 below = vec3(1.0, 0.55, 0.3) * (0.1 + 0.5 * city);
    vec3 albedo = P.kind == PLUME_SMOKE ? vec3(0.45, 0.42, 0.40) : vec3(0.80, 0.82, 0.85);   // smoke; steam
    vec3 flp[2], flc[2];
    int nf = 0;
    for (uint i = 0u; i < flameCount && nf < 2; ++i) {
        Flame f = flames[i];
        vec3 o = f.pos - P.pos;
        if (dot(o, o) > 170.0 * 170.0) continue;
        Gout g = flameGout(f.id, gTime);
        flp[nf] = f.pos + vec3(0.0, 0.4 * (g.head + g.tail) * f.reach, 0.0);
        flc[nf] = fireColor(0.5 + 0.25 * g.power) * g.power;
        nf++;
    }

    // fewer steps far off, where the plume is a few pixels across; no shadow tap there,
    // nor in a manhole's thin wisp
    int N = P.kind == PLUME_VENT ? int(mix(48.0, 10.0, smoothstep(30.0, 200.0, t0)))
                                 : int(mix(48.0, 12.0, smoothstep(150.0, 1000.0, t0)));
    bool tap = t0 < 350.0 && P.kind != PLUME_VENT;
    float dt = (t1 - t0) / float(N), jit = ign(gl_FragCoord.xy + vec2(fract(gTime * 13.7) * 64.0, 0.0));
    vec3 acc = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < N; ++i) {
        vec3 rel = dir * (t0 + (float(i) + jit) * dt);
        vec3 p = rel - P.pos;
        float r, near;
        float s = plumeSigma(p, r, near);
        if (s <= 1e-4) continue;
        // its own shadow: the plume above it hides the sky; its own thickness, the light from below
        float rr, nn;
        float up = tap ? plumeSigma(p + vec3(0.0, r * 0.6, 0.0), rr, nn) : s * 0.7;
        vec3 L = above * exp(-up * r * 0.9);
        float yAbove = frame.camPos.y + rel.y - frame.weather.z;
        float own = exp(-s * r * 0.5);
        L += below * exp(-max(yAbove, 0.0) / 120.0) * own;
        for (int j = 0; j < nf; ++j) {
            vec3 l = rel - flp[j];
            float dl = length(l);
            float hg = (1.0 - 0.35 * 0.35) / pow(1.0 + 0.35 * 0.35 - 0.7 * dot(l / max(dl, 1e-3), -dir), 1.5);   // forward scattering
            L += flc[j] * (400.0 / (dl * dl + 60.0)) * smoothstep(170.0, 60.0, dl) * hg * own;
        }
        vec3 S = albedo * L;
        if (P.kind == PLUME_SMOKE) S += vec3(1.0, 0.38, 0.1) * 1.4 * near;   // the flue's fire in its throat
        float a = 1.0 - exp(-s * dt);
        acc += S * a * T;
        T *= 1.0 - a;
        if (T < 0.01) break;
    }
    if (T > 0.999) discard;
    // the haze between the eye and the plume veils it as it does what stands behind
    float tm = 0.5 * (t0 + t1), fog = fogAmount(dir * tm, tm);
    outColor = vec4(acc * (1.0 - fog) + hazeColor(dir) * fog * (1.0 - T), T);
}
