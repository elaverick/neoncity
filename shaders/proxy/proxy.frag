#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

/* The distant proxies (proxy.vert): a building's section seen from far off.
 * The facade is shadeFacade's far end (shaders/city/facade.glsl) from the
 * same seeds: its walls, glass and window sizes, floor and bay rhythm and
 * share of lit rooms, so a building looks the same as it hands over to its
 * full mesh. Rooms are lit floor by floor and bay by bay with the facade's
 * odds, though not the very rooms its faces would light; finer than a pixel
 * they blur to the average lit window. Plus the LED strips under a section's
 * top edge. Lit only by the city's glow, and seen through the city's haze. */

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec2 vFace;
layout(location = 2) flat in vec3 vNrm;
layout(location = 3) flat in uint vSeed;
layout(location = 4) flat in uint vInfo;
layout(location = 5) flat in vec2 vSize;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;   // nothing for the screen-space reflections (city.frag)

// PX_* (src/city/city.h)
const uint PX_BANDS = 1u << 9, PX_FINE = 1u << 10, PX_LEDTOP = 1u << 11;

// As shaders/city.frag's helpers and city/lighting.glsl's and city/rooms.glsl's
// (which need a chunk's state, so can't be included here).
float hf(uint h, uint k) { return hfloat(hash_combine(h, k)); }
float pulseInt(float x, float a, float b) { return floor(x) * (b - a) + clamp(fract(x), a, b) - a; }
float filteredPulse(float x, float a, float b, float w) {
    w = max(w, 1e-4);
    return clamp((pulseInt(x + 0.5 * w, a, b) - pulseInt(x - 0.5 * w, a, b)) / w, 0.0, 1.0);
}
// How much of a pixel w wide round x lies in [a, b].
float boxCover(float x, float a, float b, float w) {
    w = max(w, 1e-4);
    return clamp((min(x + 0.5 * w, b) - max(x - 0.5 * w, a)) / w, 0.0, 1.0);
}
vec3 ambientLight(vec3 n, float h) {
    vec3 sky = vec3(0.006, 0.007, 0.013);
    vec3 bounce = vec3(0.013, 0.008, 0.006);
    float up = n.y * 0.5 + 0.5;
    return mix(bounce, sky, up) * (0.8 + 0.8 * exp(-h / 30.0));
}
vec3 neonBounce(vec3 w, float h) {
    float m = vnoise(w.xz / 23.0, 0xB0B0u);
    float k = vnoise(w.xz / 17.0 + 7.3, 0xB0B1u);
    vec3 c = mix(vec3(1.0, 0.1, 0.5), vec3(0.1, 0.8, 1.0), smoothstep(0.3, 0.7, m));
    return c * smoothstep(0.35, 0.8, k) * exp(-h / 14.0) * 0.035;
}
vec3 kelvin(float k) {
    const vec3 c2700 = vec3(1.00, 0.62, 0.32), c3000 = vec3(1.00, 0.69, 0.42), c3500 = vec3(1.00, 0.77, 0.55),
               c4000 = vec3(1.00, 0.84, 0.68), c5000 = vec3(1.00, 0.93, 0.85), c6500 = vec3(0.93, 0.97, 1.00);
    if (k < 3000.0) return mix(c2700, c3000, clamp((k - 2700.0) / 300.0, 0.0, 1.0));
    if (k < 3500.0) return mix(c3000, c3500, (k - 3000.0) / 500.0);
    if (k < 4000.0) return mix(c3500, c4000, (k - 3500.0) / 500.0);
    if (k < 5000.0) return mix(c4000, c5000, (k - 4000.0) / 1000.0);
    return mix(c5000, c6500, clamp((k - 5000.0) / 1500.0, 0.0, 1.0));
}

vec3 shadeProxy(vec3 n, vec3 V) {
    uint seed = vSeed, info = vInfo, sh = styleHash(seed);
    int type = int((info >> 4) & 3u), section = int((info >> 13) & 31u);
    uint palette = (info >> 6) & 7u;
    float vAbs = vFace.y, u = vFace.x;
    // derivatives first, in uniform control flow
    float dU = fwidth(u), dV = fwidth(vAbs);
    vec3 world = frame.camPos.xyz + vRel;
    vec3 wdiff = ambientLight(n, max(vAbs, 0.0)) + neonBounce(world, max(vAbs, 0.0)) * 1.5;

    vec3 wallAlb = palette == 0u ? vec3(0.085, 0.080, 0.075) :
                   palette == 1u ? vec3(0.080, 0.060, 0.045) :
                   palette == 2u ? vec3(0.035, 0.055, 0.060) :
                   palette == 3u ? vec3(0.110, 0.105, 0.095) :
                   palette == 4u ? vec3(0.020, 0.022, 0.028) :
                   palette == 5u ? vec3(0.105, 0.090, 0.068) :
                   palette == 6u ? vec3(0.070, 0.050, 0.030) :
                                   vec3(0.014, 0.024, 0.040);
    wallAlb *= (0.7 + 0.6 * hf(sh, 10u)) * 1.4;
    if (n.y > 0.5) return wallAlb * 0.6 * wdiff;   // the roof

    bool glassWall = palette == 4u || palette == 7u || type == 1;
    vec3 glassT = palette == 7u ? vec3(0.72, 0.79, 0.86) : palette == 4u ? vec3(0.58, 0.59, 0.60) :
                  palette == 6u ? vec3(0.86, 0.74, 0.58) : vec3(0.87, 0.89, 0.86);
    vec3 R = reflect(-V, n);
    float F = glassFresnel(n, V);
    vec3 refl = envReflection(R);
    vec3 wallLit = wallAlb * wdiff + (glassWall ? refl * (0.08 + F * 0.9) : vec3(0.0));
    if (vAbs < SIDEWALK_HEIGHT) return vec3(0.055, 0.052, 0.048) * wdiff;   // the plinth

    float winW, winH;
    if (type == 0)      { winW = mix(0.45, 0.7, hf(sh, 5u)); winH = mix(0.45, 0.65, hf(sh, 9u)); }
    else if (type == 1) { winW = 0.94; winH = 0.86; }
    else if (type == 2) { winW = 1.0;  winH = mix(0.4, 0.55, hf(sh, 9u)); }
    else                { winW = mix(0.2, 0.32, hf(sh, 5u)); winH = mix(0.7, 0.85, hf(sh, 9u)); }
    float litFrac = 0.16 + 0.42 * hf(sh, 6u), tintSel = hf(sh, 8u);
    vec3 winE = (wdiff + mix(CLOUD_DECK, vec3(0.03, 0.035, 0.045), 0.5) * 0.45) * glassT;
    vec3 avgLight = (tintSel < 0.5 ? kelvin(3050.0) : kelvin(3600.0)) * 1.15 * 0.42;
    vec3 avgWin = (avgLight * litFrac * glassT + winE * 0.1 * (1.0 - litFrac)) * (1.0 - F) + refl * F;

    vec3 col;
    float floorH = facadeFloorH(seed), groundH = facadeGroundH(seed);
    float relV = vAbs - SIDEWALK_HEIGHT - groundH;
    if (relV < 0.0) {
        // the ground floor: shopfronts and lobbies, their average glow
        float glow = 0.35 + 0.4 * hf(sh, 11u);
        col = mix(wallLit, vec3(1.0, 0.72, 0.45) * 0.42 * glow, 0.5);
    } else {
        float bayW = (info & PX_FINE) != 0u ? 1.6 + 0.8 * hf(sh, 4u) : 2.4 + 2.6 * hf(sh, 4u);
        bayW = vSize.x / max(1.0, floor(vSize.x / bayW + 0.5));   // whole bays across the face
        float fx = u / bayW, fy = relV / floorH;
        float fwU = dU / bayW, fwV = dV / floorH;
        float sill = (1.0 - winH) * 0.42;
        float cov = filteredPulse(fx, 0.5 - winW * 0.5, 0.5 + winW * 0.5, fwU * 1.2) * filteredPulse(fy, sill, sill + winH, fwV * 1.2);
        float farT = smoothstep(0.45, 1.4, max(fwU, fwV));
        // a room per bay and floor, floors lit or dark together as the facade's are
        int face = int(floor(atan(n.z, n.x) * 0.63662 + 4.5)) & 3;
        int floorIdx = int(floor(fy)), bayIdx = int(floor(fx));
        float litChance = litFrac * (0.25 + 1.5 * hfloat(hash3i(seed ^ 0xF100u, floorIdx, face * 64 + bayIdx / 4, section)));
        uint rh = hash3i(seed ^ (uint(face) * 0x9E3779B1u), bayIdx, floorIdx, section);
        vec3 room = winE * 0.08;
        if (hf(rh, 1u) < litChance) {
            float pick = hf(rh, 2u), kt = hf(rh, 13u);
            float k = tintSel < 0.5 ? (pick < 0.88 ? mix(2700.0, 3300.0, kt) : mix(3600.0, 4500.0, kt))
                                    : (pick < 0.65 ? mix(2900.0, 3600.0, kt) : pick < 0.9 ? mix(3800.0, 4500.0, kt) : mix(5000.0, 6500.0, kt));
            room = kelvin(k) * mix(0.6, 1.7, hf(rh, 3u)) * 0.42;
        }
        vec3 winMid = room * glassT * (1.0 - F) + refl * F;
        col = mix(mix(wallLit, winMid, cov), mix(wallLit, avgWin, winW * winH), farT);
    }

    // the LED strip under the top edge (emit_face_trims), steady or chasing
    if ((info & PX_LEDTOP) != 0u) {
        uint lc = hash_combine(seed, uint(section)) & 7u;
        col += neonColor(lc) * 6.0 * ((lc & 1u) != 0u ? 0.625 : 1.0) * boxCover(vAbs, vSize.y - 0.75, vSize.y - 0.45, dV);
    }
    return col;
}

void main() {
    vec3 n = normalize(vNrm);
    float dist = length(vRel);
    vec3 V = -vRel / max(dist, 1e-4);
    vec3 col = shadeProxy(n, V);
    outColor = vec4(mix(col, hazeColor(-V), fogAmount(vRel, dist)), 1.0);
    outRefl = vec4(0.0);
}
