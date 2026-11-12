#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "smoke.glsl"

/* A noodle bar's steam and its wok's smoke as puffs (NB7: src/gen/noodle_bar.h
 * nb_steam; the doc's section 10.3): STEAM_PUFFS camera-facing quads a plume
 * (instance / STEAM_PUFFS; anything but a stall's plume is thrown away), each
 * placed from the time alone. A puff leaves the pot's rim at a phase of its
 * own and runs along the plume's knots (out along dir, up: up to the
 * extractor, under it, up under the hood, along under it to the lip, down
 * behind the fascia and out under it), then climbs free past the last, the
 * wind carrying it the further the higher it gets. It swells as it goes,
 * keeps under the ceiling over the knots (a sheet there, spread sideways and
 * flattened), and thins away by the end of its climb. Its colour is the bar's
 * own light where it is (summed when the chunk was built: over the pot, under
 * the hood, out past the fascia), the sky only once it is out from under the
 * hood, and the city's glow; a wok's smoke greyer, in bursts. steam.frag
 * draws the soft blob, torn by the noise, faded into what stands behind. */

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out vec4 vCol;    // its colour (linear, unexposed), its peak opacity
layout(location = 2) out vec3 vRel;         // the quad's point, camera-relative
layout(location = 3) flat out vec4 vPuff;   // radius, its seed's share, how far through its life, its kind (1 wok)

const int STEAM_PUFFS = 64;
const int POT_PUFFS = 20;   // of them, a pot's own (not the duct's)
const float STALL_LIGHT = 0.25;   // in-scattered share of the bar's light (set by eye, NB7 captures)
const vec2 Q[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    int pi = gl_InstanceIndex / STEAM_PUFFS, j = gl_InstanceIndex % STEAM_PUFFS;
    Plume P = plumes[pi];
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // thrown away unless it is drawn
    vUV = vec2(0.0); vCol = vec4(0.0); vRel = vec3(0.0); vPuff = vec4(0.0);
    if (!plumeStall(P) || P.nknot == 0u) return;
    float dist = length(P.pos);
    // fewer puffs far off, each the denser
    int use = int(mix(float(STEAM_PUFFS), 16.0, smoothstep(20.0, 150.0, dist)));
    if (j >= use) return;
    float t = frame.camPos.w;
    uint h = hash_u32(P.seed * 7919u + uint(j) * 0x9E3779B9u);
    float h0 = hfloat(h), h1 = hfloat(hash_u32(h + 1u)), h2 = hfloat(hash_u32(h + 2u)), h3 = hfloat(hash_u32(h + 3u)), h4 = hfloat(hash_u32(h + 4u));

    // the path's length: the knots, then the free climb
    int n = int(P.nknot);
    float S = 0.0, s1 = P.rise, sE = 0.0, sL = 0.0;
    vec2 prev = vec2(0.0);
    for (int i = 1; i < n; ++i) {
        S += length(P.knot[i].xy - prev); prev = P.knot[i].xy;
        if (i == 1) s1 = S;
        if (i == 2) sE = S;
        if (i == 5) sL = S;
    }
    vec2 last = prev;
    float Stot = S + P.rise;
    // its place along: quicker off the pot, then at the flow's pace
    float life = s1 / STALL_EXIT + (Stot - s1) / STALL_FLOW;
    float ph = fract(t / life + h0);
    float tt = ph * life, s = tt * STALL_EXIT;
    if (s > s1) s = s1 + (tt - s1 / STALL_EXIT) * STALL_FLOW;
    // the pot's own puffs (a pot's plume): short-lived, rising slowly off the rim and gathering under the extractor
    bool pot = n > 1 && j < POT_PUFFS;
    float potPh = 0.0;
    if (pot) {
        potPh = fract(t / 1.8 + h0);
        s = potPh * (s1 + 0.3);
    }
    float k = s / Stot;

    // the point of the path at s, its way there, and whether it runs under a ceiling (a sheet)
    vec2 at = last + vec2(0.0, s - S), bu = vec2(0.0, 1.0);
    bool sheet = false;
    float acc = 0.0, roof = 1e9;
    prev = vec2(0.0);
    for (int i = 1; i < n; ++i) {
        vec2 kn = P.knot[i].xy;
        float len = length(kn - prev);
        if (s <= acc + len) {
            float f = (s - acc) / max(len, 1e-4);
            at = mix(prev, kn, f);
            bu = (kn - prev) / max(len, 1e-4);
            sheet = abs(bu.y) < 0.4;
            break;
        }
        acc += len; prev = kn;
    }
    float r = stallRadius(P, s);
    float rv = sheet ? min(r, 0.14 + 0.04 * s) : r, rb = r + 0.08 * max(s - s1, 0.0);
    // scattered about the path: across it in its plane, sideways, and wandering
    vec2 perp = vec2(-bu.y, bu.x);
    float wob = t * (0.6 + 0.5 * h4) + 6.2831853 * h3;
    at += perp * ((h1 - 0.5) * 1.2 * rv + 0.25 * rv * sin(wob));
    float b = (h2 - 0.5) * 1.6 * rb + 0.25 * rb * cos(wob * 0.8);
    // the climb's height past the path's end, and the wind on it
    float climb = max(s - S, 0.0);
    vec2 D = P.dir, T = vec2(-D.y, D.x);
    vec3 c = P.pos + vec3(D.x * at.x + T.x * b, at.y, D.y * at.x + T.y * b);
    vec2 mouth = P.pos.xz + frame.camPos.xz;
    float gust = PLUME_BREEZE + PLUME_GUST * gustAt(mouth, t - climb / STALL_EXIT);
    c.xz += GUST_WIND * gust * STALL_WIND * climb / STALL_EXIT;

    // its size; under a ceiling it keeps beneath it (the hood's sheet, between the knots linearly)
    float rad = min(0.07 + 0.45 * (sheet ? rv : r), 0.4) * (0.75 + 0.5 * h3);   // a puff, not the plume: they crowd it
    if (pot) rad = (0.05 + 0.35 * P.radius) * mix(0.6, 1.5, potPh) * (0.75 + 0.5 * h3);
    float aRaw = at.x, pa = 0.0, pr = 0.0;
    bool any = false;
    for (int i = 1; i < n; ++i) {
        vec4 kn = P.knot[i];
        if (kn.z <= 0.0) continue;
        if (aRaw <= kn.x) { roof = any ? mix(pr, kn.z, clamp((aRaw - pa) / max(kn.x - pa, 1e-3), 0.0, 1.0)) : kn.z; break; }
        any = true; pa = kn.x; pr = kn.z;
    }
    if (roof < 1e8) c.y = min(c.y, P.pos.y + roof - 0.45 * rad);

    // how thick: in as it leaves the pot, thinning as it spreads and climbs; a wok's in bursts as the cook tosses
    float leave = t - s / STALL_FLOW;
    float pulse = P.kind == PLUME_WOK ? 0.15 + 0.85 * smoothstep(0.5, 0.8, vnoise(vec2(leave * 0.8, float(P.seed & 255u)), 0x57A1u))
                                      : 0.75 + 0.25 * vnoise(vec2(leave * 1.3, float(P.seed & 255u)), 0x57A2u);
    float alpha = P.strength * pulse * smoothstep(0.0, 0.05, k) * (1.0 - 0.35 * min(s, S) / max(S, 1e-3)) * pow(1.0 - min(climb / max(P.rise, 0.1), 1.0), 1.3) * 0.5 * float(STEAM_PUFFS) / float(use);
    if (pot) alpha = P.strength * pulse * smoothstep(0.0, 0.15, potPh) * pow(1.0 - potPh, 0.8) * 0.8;
    alpha *= smoothstep(0.25, 0.7, length(c) - rad * 0.5);   // never a wall of it over the eye

    // its light: the bar's where it is, thinning as it climbs away; the sky once out from under the hood
    vec3 bar = n <= 1 ? P.light[0].rgb
             : mix(mix(P.light[0].rgb, P.light[1].rgb, smoothstep(sE - 0.1, sE + 0.4, s)), P.light[2].rgb, smoothstep(sL - 0.1, sL + 0.3, s));
    bar *= exp(-0.9 * climb);
    float open = n <= 1 ? 1.0 : smoothstep(sL - 0.1, sL + 0.4, s);
    float city = frame.sky.x;
    vec3 above = CLOUD_DECK * (1.0 + 1.5 * city) + MOON_LIGHT * 0.35 * (1.0 - frame.sky.z) + dayLevel() * (0.6 * daylightThrough());
    vec3 below = vec3(1.0, 0.55, 0.3) * (0.1 + 0.5 * city);
    vec3 albedo = P.kind == PLUME_WOK ? vec3(0.50, 0.47, 0.44) : vec3(0.85, 0.86, 0.88);
    vec3 col = albedo * (bar * STALL_LIGHT + above * mix(0.15, 1.0, open) + below * 0.5);
    float dc = length(c), fog = fogAmount(c, dc);
    col = mix(col, hazeColor(c / dc), fog);

    // facing the camera
    vec3 dir = c / max(dc, 1e-3);
    vec3 right = normalize(cross(dir, vec3(0.0, 1.0, 0.0)) + vec3(1e-5, 0.0, 0.0));
    vec3 up = cross(right, dir);
    vec2 q = Q[gl_VertexIndex];
    vec3 p = c + (right * q.x + up * q.y) * rad;
    vUV = q;
    vCol = vec4(col, alpha);
    vRel = p;
    vPuff = vec4(rad, h4, k, P.kind == PLUME_WOK ? 1.0 : 0.0);
    gl_Position = frame.viewProj * vec4(p, 1.0);
}
