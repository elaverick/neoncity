#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNrm;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint  vSeed;
layout(location = 5) flat in uvec4 vParams;
layout(location = 6) flat in uint  vChunk;
layout(location = 7) flat in uint  vFacePack;
layout(location = 8) flat in uint  vAux;

layout(location = 0) out vec4 outColor;

ChunkGPU gC;
float gTime;
vec3 gWorld;    // chunk-local position + wrapped chunk offset: continuous for noise

const float PI = 3.14159265;

/* ======================================================================
 * Small helpers
 * ==================================================================== */

float band(float x, float a, float b, float aa) {
    return smoothstep(a - aa, a + aa, x) * (1.0 - smoothstep(b - aa, b + aa, x));
}

// Integral of a unit-periodic pulse that is 1 on [a,b]; used for box-filtered window masks.
float pulseInt(float x, float a, float b) { return floor(x) * (b - a) + clamp(fract(x), a, b) - a; }
float filteredPulse(float x, float a, float b, float w) {
    w = max(w, 1e-4);
    return clamp((pulseInt(x + 0.5 * w, a, b) - pulseInt(x - 0.5 * w, a, b)) / w, 0.0, 1.0);
}

float hf(uint h, uint k) { return hfloat(hash_combine(h, k)); }

vec3 shopColor(uint h) { return mix(neonColor(h >> 4), vec3(1.0, 0.82, 0.62), 0.45); }

/* ======================================================================
 * Lighting: street lamps and lit shopfronts come from the chunk's light
 * table, built from the same plan as the geometry, so light pools sit
 * exactly under the lamp heads and shop spill matches the lit storefronts.
 * ==================================================================== */

vec3 ambientLight(vec3 n, float h) {
    vec3 sky = vec3(0.016, 0.019, 0.032);
    vec3 bounce = vec3(0.034, 0.020, 0.016);
    float up = n.y * 0.5 + 0.5;
    return mix(bounce, sky, up) * (0.8 + 0.8 * exp(-h / 30.0));
}

void addLamp(vec3 p, vec3 n, vec3 V, float shin, float specK, vec3 lpos, vec3 lcol,
             inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    float d2 = dot(l, l);
    float d = sqrt(d2);
    float range = smoothstep(34.0, 14.0, d);
    if (range <= 0.0) return;
    vec3 L = l / d;
    float spot = smoothstep(0.05, 0.55, L.y);
    float att = 70.0 / (d2 + 1.0) * range * spot;
    float ndl = max(dot(n, L), 0.0);
    diff += lcol * att * ndl;
    vec3 H = normalize(L + V);
    spec += lcol * att * pow(max(dot(n, H), 0.0), shin) * specK * ndl * (shin + 8.0) / 25.0;
}

// Coloured light thrown onto the sidewalk and street by a lit shopfront.
void addShop(vec3 p, vec3 n, float specK, vec3 lpos, vec3 col, inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    float d = length(l);
    if (d > 12.0) return;
    float ndl = max(dot(n, l / max(d, 1e-3)), 0.0);
    vec3 c = col * 0.45 * exp(-d * 0.38) * smoothstep(12.0, 7.0, d);
    diff += c * (0.35 + 0.65 * ndl);
    spec += c * specK * 0.6;
}

/* Facade faces are numbered within their section, round the outline. The
 * cell (window bay or 6 m shop bay) that wraps a convex corner is keyed as
 * the next face's first cell, so both windows of a corner room or shop look
 * into the same space. */
ivec2 cornerKey(int face, int next, bool endShare, int idx, int last) {
    return (endShare && idx >= last && last > 0) ? ivec2(next, 0) : ivec2(face, idx);
}

// Storefronts are 6 m bays along each ground-floor face (city_shop_hash in src/city.c).
uint shopHash(uint seed, int section, int face, int next, bool endShare, float along, float faceW) {
    ivec2 k = cornerKey(face, next, endShare, int(floor(along / 6.0)), int(floor((faceW - 0.01) / 6.0)));
    return hash3i(WORLD_SEED ^ 0x5B0Au, int(seed), section * 64 + k.x, k.y);
}

void cityLights(vec3 p, vec3 n, vec3 V, float shin, float specK, inout vec3 diff, inout vec3 spec) {
    int base = gC.info.z;
    if (base < 0 || p.y > 32.0) return;
    vec4 hdr = uintBitsToFloat(lightRec[base]);
    ivec2 t = clamp(ivec2(floor((p.xz - hdr.xy) * hdr.zw)), ivec2(0), ivec2(3));
    int ti = t.y * 4 + t.x;
    uint tile = lightRec[base + 1 + (ti >> 2)][ti & 3];
    int first = base + int(tile >> 16), count = int(tile & 0xFFFFu);
    for (int i = 0; i < count; ++i) {
        uvec4 r = lightRec[first + i];
        vec3 lp = uintBitsToFloat(r.xyz);
        uint type = r.w & 3u;
        if (type < 2u) addLamp(p, n, V, shin, specK, lp, lampColor(int(type)), diff, spec);
        else addShop(p, n, specK, lp, type == 2u ? shopColor(r.w) : vec3(0.70, 0.78, 0.95), diff, spec);
    }
}

// Soft coloured light from the sea of signs, strongest near street level.
vec3 neonBounce(vec3 w, float h) {
    float m = vnoise(w.xz / 23.0, 0xB0B0u);
    float k = vnoise(w.xz / 17.0 + 7.3, 0xB0B1u);
    vec3 c = mix(vec3(1.0, 0.1, 0.5), vec3(0.1, 0.8, 1.0), smoothstep(0.3, 0.7, m));
    return c * smoothstep(0.35, 0.8, k) * exp(-h / 14.0) * 0.06;
}

vec3 envReflection(vec3 R) {
    vec3 c = hazeColor(R) * 1.5;
    float below = smoothstep(0.1, -0.2, R.y);
    c += vec3(0.05, 0.025, 0.04) * below;   // the lit city below
    return c;
}

/* ======================================================================
 * Ground: asphalt with markings, sidewalks, curbs, lots
 * ==================================================================== */

vec2 laneLines(float t, float along, float half_, float aa) {
    float at = abs(t);
    float centre = band(at, 0.10, 0.24, aa);
    float dash = step(fract(along / 9.0), 0.42);
    float lanes = 0.0;
    for (int i = 1; i <= 3; ++i) {
        float L = 3.4 * float(i);
        if (L < half_ - 1.6) lanes = max(lanes, band(at, L - 0.07, L + 0.07, aa) * dash);
    }
    float edge = band(at, half_ - 0.55, half_ - 0.42, aa);
    return vec2(max(centre, max(lanes, edge)), centre);
}

vec2 crosswalk(float lateral, float half_, float into, float walk, float aa) {
    float zone = band(into, 0.5, walk - 0.5, aa);
    float stripes = band(fract(lateral / 1.1), 0.0, 0.5, aa * 0.9);
    stripes *= 1.0 - smoothstep(half_ - 0.9, half_ - 0.6, abs(lateral));
    float stop = band(into, walk + 0.4, walk + 0.85, aa) * step(0.3, abs(lateral)) *
                 (1.0 - smoothstep(half_ - 0.7, half_ - 0.5, abs(lateral)));
    return vec2(max(zone * stripes, stop), 0.0);
}

// Ground. Carriageway quads carry uv = (offset from the centre line, distance
// along), their length in the seed and half-width (quarter metres) in params;
// the plain asphalt underneath (half-width 0) fills junctions and verges.
vec3 shadeGround(uint mat, vec3 p, vec3 n, vec3 V, vec2 uv, uint seed, uvec4 prm) {
    vec3 w = gWorld;
    float aa = max(fwidth(p.x), fwidth(p.z)) * 0.7 + 0.004;
    vec3 alb;
    float shin = 16.0, specK = 0.08;

    if (mat == MAT_ROAD) {
        float nn = fbm(w.xz * 0.45, 0xA5u);
        float big = vnoise(w.xz * 0.045, 0xA6u);
        alb = vec3(0.030, 0.030, 0.033) * (0.7 + 0.6 * nn) * (0.75 + 0.5 * big);
        // patched / resurfaced rectangles
        vec2 pc = floor(w.xz / 7.0);
        uint ph = hash2i(0xA7u, int(pc.x), int(pc.y));
        if (hfloat(ph) < 0.12) alb *= 0.75;

        float half_ = float(prm.y) * 0.25;
        vec2 mk = vec2(0.0);
        float dCurb = 10.0;
        if (half_ > 0.0) {
            float t = uv.x, s = uv.y, len = uintBitsToFloat(seed);
            mk = laneLines(t, s, half_, aa);
            if (prm.z == 1u) {
                float into = min(s, len - s);
                vec2 cw = crosswalk(t, half_, into, 4.2, aa);
                if (into < 5.2) mk = cw; else mk = max(mk, cw);
            }
            dCurb = half_ - abs(t);
        }
        float wear = 0.45 + 0.55 * smoothstep(0.25, 0.6, fbm(w.xz * 1.7, 0xA8u));
        vec3 paint = mix(vec3(0.42, 0.41, 0.38), vec3(0.45, 0.30, 0.05), mk.y);
        alb = mix(alb, paint, mk.x * wear);

        // manhole covers
        vec2 mc = floor(w.xz / 13.0);
        uint mh = hash2i(0xA9u, int(mc.x), int(mc.y));
        if (hfloat(mh) < 0.25) {
            vec2 c = (mc + 0.3 + 0.4 * vec2(hf(mh, 1u), hf(mh, 2u))) * 13.0;
            float r = length(w.xz - c);
            float ring = band(r, 0.30, 0.36, aa);
            float disc = 1.0 - smoothstep(0.3, 0.3 + aa, r);
            float grid = step(0.5, fract((w.x + w.z) * 6.0));
            alb = mix(alb, vec3(0.045, 0.04, 0.035) * (0.8 + 0.3 * grid), disc);
            alb *= 1.0 - ring * 0.5;
        }
        // gutter grime along the kerbs
        alb *= mix(0.6, 1.0, smoothstep(0.0, 0.6, dCurb));
        shin = 60.0;
        specK = (0.25 + 0.5 * big) * (1.0 - 0.6 * mk.x);   // oily, slightly slick asphalt
    } else if (mat == MAT_SIDEWALK) {
        // paving runs with the block grain (uv); prm.y = 1 is the kerb-stone edge
        vec2 tp = uv / 1.2;
        vec2 ti = floor(tp), tf = fract(tp);
        float th = hfloat(hash2i(0x7111u, int(ti.x), int(ti.y)));
        float g = min(min(tf.x, 1.0 - tf.x), min(tf.y, 1.0 - tf.y)) * 1.2;
        float grout = 1.0 - smoothstep(0.012, 0.03 + aa, g);
        alb = vec3(0.080, 0.077, 0.074) * (0.78 + 0.4 * th) * (0.65 + 0.7 * fbm(w.xz * 0.7, 0x7112u));
        alb = mix(alb, vec3(0.028), grout * 0.75);
        if (prm.y == 1u) alb = vec3(0.12, 0.115, 0.11) * (0.8 + 0.3 * fbm(w.xz * 3.0, 0x7113u));
        alb *= 0.6 + 0.4 * smoothstep(0.2, 0.55, fbm(w.xz * 0.15, 0x7114u));  // stains
        shin = 24.0; specK = 0.1;
    } else if (mat == MAT_CURB) {
        alb = vec3(0.11, 0.105, 0.10) * (0.7 + 0.5 * fbm(w.xz * 2.0 + w.y, 0x7115u));
    } else if (prm.y == 1u) {
        // park: rough dark grass with worn patches
        float gr = fbm(w.xz * 0.9, 0x7117u);
        alb = mix(vec3(0.020, 0.032, 0.018), vec3(0.034, 0.040, 0.022), gr) * (0.6 + 0.8 * fbm(w.xz * 4.0, 0x7118u));
        shin = 8.0; specK = 0.03;
    } else {
        alb = vec3(0.045, 0.043, 0.04) * (0.6 + 0.8 * fbm(w.xz * 0.5, 0x7116u));
    }

    vec3 diff = ambientLight(n, p.y);
    vec3 spec = vec3(0.0);
    cityLights(p, n, V, shin, specK, diff, spec);
    diff += neonBounce(w, p.y);
    // sky / haze sheen at grazing angles
    float fres = pow(1.0 - max(dot(n, V), 0.0), 5.0);
    spec += envReflection(reflect(-V, n)) * fres * specK * 2.0;
    return alb * diff + spec;
}

/* ======================================================================
 * Interior mapping
 *
 * Each window cell is a room box behind the facade: [0,W] x [0,H] x [0,D]
 * in tangent space (x along the wall, y up, z into the building). The view
 * ray is intersected with the box from the inside and the nearest wall,
 * floor or ceiling is shaded; a furniture plane and blinds add depth layers.
 * ==================================================================== */

struct Room {
    bool  lit;
    vec3  light;
    float bright;
    vec3  wall;
    vec3  floorC;
    float blinds;    // 0..1 fraction of the window covered from the top
    float curtain;   // 0 or 1
    vec3  curtainC;
    float furnZ;     // furniture plane depth fraction
    bool  person;
    bool  shop;
    uint  h;
};

// Shop walls: shelving with rows of colourful stock.
vec3 shelves(vec2 w, vec3 base, uint h) {
    float row = floor(w.y / 0.42);
    float fy = fract(w.y / 0.42);
    if (w.y < 0.3 || w.y > 2.3) return base;
    if (fy < 0.1) return vec3(0.05);
    float item = floor(w.x / 0.22);
    uint ih = hash3i(h, int(item), int(row), 7);
    float fx = fract(w.x / 0.22);
    float top = 0.45 + 0.5 * hfloat(ih);
    if (fx < 0.1 || fx > 0.9 || fy > top) return base * 0.4;
    return mix(neonColor(ih >> 5), vec3(0.8), 0.35) * (0.4 + 0.6 * hf(ih, 3u));
}

Room makeRoom(uint h, float litChance, float tintSel, bool shop, vec3 shopCol) {
    Room r;
    r.h = h;
    r.lit = shop || hf(h, 1u) < litChance;
    float pick = hf(h, 2u);
    vec3 warm = vec3(1.0, 0.68, 0.40), cool = vec3(0.72, 0.88, 1.0), pink = vec3(1.0, 0.35, 0.62),
         teal = vec3(0.30, 0.95, 0.85), tv = vec3(0.45, 0.60, 1.0);
    vec3 base = tintSel < 0.5 ? warm : cool;
    vec3 alt = tintSel < 0.5 ? cool : warm;
    r.light = pick < 0.6 ? base : pick < 0.82 ? alt : pick < 0.9 ? pink : pick < 0.95 ? teal : tv;
    r.bright = mix(0.9, 2.6, hf(h, 3u));
    if (pick >= 0.95) {   // television flicker
        float f = vnoise(vec2(gTime * 7.0, float(h & 255u)), 0xF11Cu);
        r.bright *= 0.35 + 0.9 * f;
    }
    if (shop) { r.light = shopCol; r.bright = mix(0.9, 1.7, hf(h, 3u)); }
    r.wall = mix(vec3(0.35, 0.33, 0.30), vec3(0.55, 0.50, 0.45), hf(h, 4u)) * (0.5 + 0.5 * hf(h, 5u));
    r.floorC = vec3(0.12, 0.10, 0.09) * (0.6 + 0.8 * hf(h, 6u));
    float b = hf(h, 7u);
    r.blinds = (!shop && b < 0.45) ? mix(0.15, 0.95, hf(h, 8u)) : 0.0;
    r.curtain = (!shop && b > 0.86) ? 1.0 : 0.0;
    r.curtainC = mix(vec3(0.9, 0.25, 0.12), vec3(0.85, 0.65, 0.35), hf(h, 9u));
    r.furnZ = mix(0.3, 0.65, hf(h, 10u));
    r.person = r.lit && hf(h, 11u) < (shop ? 0.4 : 0.08);
    r.shop = shop;
    return r;
}

// Furniture / figure silhouette at the furniture plane. x in metres, y in metres.
float furniture(float x, float y, float W, Room r) {
    uint h = r.h;
    float a1 = W * (0.05 + 0.35 * hf(h, 20u)), b1 = a1 + W * (0.2 + 0.3 * hf(h, 21u)), c1 = 0.7 + 0.5 * hf(h, 22u);
    float m = (x > a1 && x < b1 && y < c1) ? 1.0 : 0.0;
    float a2 = W * (0.55 + 0.3 * hf(h, 23u)), b2 = a2 + W * (0.07 + 0.12 * hf(h, 24u)), c2 = 1.4 + 0.7 * hf(h, 25u);
    if (hf(h, 26u) < 0.6 && x > a2 && x < b2 && y < c2) m = 1.0;
    if (r.person) {
        float px = W * (0.5 + 0.33 * sin(gTime * (0.12 + 0.1 * hf(h, 27u)) + 6.28 * hf(h, 28u)));
        float dx = abs(x - px);
        float body = (y < 1.48 && dx < mix(0.13, 0.23, smoothstep(0.0, 1.3, y))) ? 1.0 : 0.0;
        float head = length(vec2(dx, y - 1.63)) < 0.12 ? 1.0 : 0.0;
        m = max(m, max(body, head) * 2.0);
    }
    return m;
}

vec3 interiorColor(vec2 cell, vec3 d, float W, float H, float D, Room r) {
    vec3 o = vec3(cell.x * W, cell.y * H, 0.0);
    d.z = max(d.z, 1e-3);
    float tx = d.x > 0.0 ? (W - o.x) / d.x : (d.x < 0.0 ? -o.x / d.x : 1e9);
    float ty = d.y > 0.0 ? (H - o.y) / d.y : (d.y < 0.0 ? -o.y / d.y : 1e9);
    float tz = (D - o.z) / d.z;
    float t = min(tx, min(ty, tz));
    vec3 p = o + d * t;

    vec3 alb;
    bool ceiling = false;
    if (t == tz) {
        alb = r.wall;
        // a door or poster on the back wall
        float dx = abs(p.x - W * (0.25 + 0.5 * hf(r.h, 30u)));
        if (r.shop) alb = shelves(p.xy, r.wall, r.h);
        else if (dx < 0.45 && p.y < 2.1) alb *= 0.45;
        else if (hf(r.h, 31u) < 0.3 && dx > 0.8 && dx < 1.4 && p.y > 1.1 && p.y < 1.9)
            alb = neonColor(r.h >> 3) * 0.5;
    } else if (t == tx) {
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (d.y > 0.0) {
        alb = vec3(0.55);
        ceiling = true;
    } else {
        alb = r.floorC;
    }

    vec3 col;
    if (r.lit) {
        vec3 lp = vec3(W * 0.5, H - 0.05, D * 0.5);
        vec3 dl = p - lp;
        float illum = r.bright * (0.18 + 2.2 / (1.0 + dot(dl, dl) * 0.35));
        col = alb * r.light * illum;
        if (ceiling && abs(p.x - W * 0.5) < W * 0.2 && abs(p.z - D * 0.5) < min(D * 0.15, 0.6))
            col = r.light * r.bright * 5.0;
    } else {
        col = alb * vec3(0.010, 0.012, 0.020);   // only the city glow from outside
    }

    // furniture / figure layer
    float tf = (D * r.furnZ) / d.z;
    if (tf < t) {
        vec3 pf = o + d * tf;
        if (pf.x > 0.0 && pf.x < W && pf.y > 0.0) {
            float m = furniture(pf.x, pf.y, W, r);
            if (m > 0.5) {
                vec3 sil = r.lit ? r.light * r.bright * (m > 1.5 ? 0.03 : 0.10) : vec3(0.002);
                col = sil;
            }
        }
    }
    return col;
}

/* What a corner room's far window looks out on. The real scene behind the
 * building is not available, so this models a typical one from the window's
 * height: a street below, lit by lamps and shop spill near the far kerb, and
 * the facade across it (about 26 m away) with lit shopfronts and signs at
 * street level, windows above and the haze beyond its roofline. */
vec3 throughView(vec3 dir, float eyeY) {
    const float L = 26.0;
    float hl = max(length(dir.xz), 1e-3);
    float tWall = L / hl;
    float yWall = eyeY + dir.y * tWall;
    float sAlong = atan(dir.z, dir.x) * L;      // metres along the far side of the street
    vec3 c;
    float dist;
    if (yWall < 0.0) {
        float tg = eyeY / max(-dir.y, 1e-3);
        float across = hl * tg;
        dist = tg;
        float lampAt = (fract(sAlong / 27.0) - 0.5) * 27.0;
        float pool = exp(-lampAt * lampAt / 60.0) * exp(-(across - L + 6.0) * (across - L + 6.0) / 50.0);
        c = vec3(0.030, 0.030, 0.034) + lampColor(0) * pool * 0.25
          + vec3(0.55, 0.35, 0.45) * 0.08 * exp(-(L - across) / 3.0);
    } else {
        dist = tWall;
        float roof = 18.0 + 90.0 * hfloat(hash2i(0x0B1Du, int(floor(sAlong / 22.0)), 0));
        if (yWall > roof) return hazeColor(dir);
        if (yWall < 4.6) {
            uint h = hash2i(0x5B0Bu, int(floor(sAlong / 6.0)), 0);
            float fs = fract(sAlong / 6.0);
            float win = band(fs, 0.06, 0.94, 0.02) * band(yWall, 0.5, 3.4, 0.05);
            bool open = hfloat(h) < 0.7;
            c = mix(vec3(0.03), open ? shopColor(h) * 0.6 : vec3(0.02), win);
            if (open && hf(h, 3u) < 0.8)
                c += neonColor(h >> 6) * band(yWall, 3.7, 4.4, 0.05) * band(fs, 0.1, 0.9, 0.02) * 1.5;
        } else {
            float fx = sAlong / 2.8, fy = (yWall - 4.6) / 3.5;
            uint h = hash2i(0x0DD5u, int(floor(fx)), int(floor(fy)));
            float win = band(fract(fx), 0.2, 0.8, 0.05) * band(fract(fy), 0.25, 0.75, 0.05) * step(hfloat(h), 0.35);
            vec3 tint = hf(h, 1u) < 0.6 ? vec3(1.0, 0.68, 0.40) : vec3(0.72, 0.88, 1.0);
            c = mix(vec3(0.025, 0.025, 0.030), tint * (0.3 + 0.6 * hf(h, 2u)), win);
        }
    }
    return mix(c, hazeColor(dir), 1.0 - exp(-dist / 380.0));
}

/* A corner room or corner shop: glazed at the front and on its corner side
 * wall(s), glassL at x = 0 and/or glassR at x = W, through the neighbouring
 * face's corner window. Seen from either face it is the same box: this face's
 * corner bay wide and the neighbouring face's corner bay deep. The side
 * window openings are given in metres in the room (zL/zR: depth range on the
 * left/right wall, yA: height range), so they line up with the windows seen
 * from outside. A ray reaching side glass leaves the building. The furniture
 * is a traced square desk or counter in the inner corner, so it too is the
 * same object from both sides. */
vec3 cornerInterior(vec2 cell, vec3 d, float W, float H, float D, Room r, bool glassL, bool glassR,
                    vec3 worldDir, float eyeY, vec2 zL, vec2 zR, vec2 yA) {
    vec3 o = vec3(cell.x * W, cell.y * H, 0.0);
    d.z = max(d.z, 1e-3);
    float tx = d.x > 0.0 ? (W - o.x) / d.x : (d.x < 0.0 ? -o.x / d.x : 1e9);
    float ty = d.y > 0.0 ? (H - o.y) / d.y : (d.y < 0.0 ? -o.y / d.y : 1e9);
    float tz = (D - o.z) / d.z;
    float t = min(tx, min(ty, tz));

    // desk / counter island against the inner walls
    float s = min(W, D) * 0.45;
    float bx = (glassR && !glassL) ? 0.0 : (glassL && !glassR) ? W - s : 0.5 * (W - s);
    vec3 bmin = vec3(bx, 0.0, D - s), bmax = vec3(bx + s, r.shop ? 1.0 : 0.75, D);
    vec3 dd = vec3(abs(d.x) < 1e-5 ? 1e-5 : d.x, abs(d.y) < 1e-5 ? 1e-5 : d.y, d.z);
    vec3 t0 = (bmin - o) / dd, t1 = (bmax - o) / dd;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float tin = max(max(tn.x, tn.y), tn.z), tout = min(min(tf.x, tf.y), tf.z);
    bool desk = tin < tout && tin > 0.0 && tin < t;
    if (desk) t = tin;
    vec3 p = o + d * t;

    vec3 alb;
    bool ceiling = false;
    if (desk) {
        alb = vec3(0.10, 0.085, 0.07) * (tin == tn.y ? 1.5 : 0.6);
    } else if (t == tx && ((d.x > 0.0 && glassR) || (d.x < 0.0 && glassL))) {
        vec2 za = d.x > 0.0 ? zR : zL;
        float ax = (p.z - za.x) / max(za.y - za.x, 1e-3), ay = (p.y - yA.x) / max(yA.y - yA.x, 1e-3);
        if (ax > 0.0 && ax < 1.0 && ay > 0.0 && ay < 1.0) {
            // the room's curtain and blinds cover this window too
            float open = 0.18 + 0.2 * hf(r.h, 41u);
            if (r.curtain > 0.5 && (ax < open || ax > 1.0 - open)) {
                float fold = 0.75 + 0.25 * sin(ax * 60.0);
                return r.lit ? r.curtainC * r.light * r.bright * 0.35 * fold : r.curtainC * 0.01;
            }
            if (r.blinds > 0.0 && ay > 1.0 - r.blinds) {
                float sl = fract(p.y / 0.055);
                float slat = smoothstep(0.08, 0.2, sl) * smoothstep(0.92, 0.8, sl);
                return r.lit ? r.light * r.bright * 0.28 * (0.55 + 0.45 * slat) : vec3(0.02) * slat;
            }
            float edge = min(min(ax, 1.0 - ax) * (za.y - za.x), min(ay, 1.0 - ay) * (yA.y - yA.x));
            vec3 outside = throughView(worldDir, eyeY) + (r.lit ? r.light * r.bright * 0.025 : vec3(0.0));
            return mix(vec3(0.012, 0.012, 0.014), outside, smoothstep(0.03, 0.06, edge));
        }
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (t == tz) {
        alb = r.shop ? shelves(p.xy, r.wall, r.h) : r.wall;
    } else if (t == tx) {
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (d.y > 0.0) {
        alb = vec3(0.55);
        ceiling = true;
    } else {
        alb = r.floorC;
    }

    if (!r.lit) return alb * vec3(0.010, 0.012, 0.020);
    vec3 lp = vec3(W * 0.5, H - 0.05, D * 0.5);
    vec3 dl = p - lp;
    float illum = r.bright * (0.18 + 2.2 / (1.0 + dot(dl, dl) * 0.35));
    float panel = min(min(W, D) * 0.2, 0.6);   // square, so it matches from both faces
    if (ceiling && abs(p.x - W * 0.5) < panel && abs(p.z - D * 0.5) < panel)
        return r.light * r.bright * 5.0;
    return alb * r.light * illum;
}

/* ======================================================================
 * Facades
 * ==================================================================== */

vec3 wallLighting(vec3 p, vec3 n, vec3 V, float shin, float specK, float height, out vec3 spec) {
    vec3 diff = ambientLight(n, height);
    spec = vec3(0.0);
    cityLights(p, n, V, shin, specK, diff, spec);
    diff += neonBounce(gWorld, height) * 1.5;
    return diff;
}

vec3 shadeFacade(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    // Facade data (facade_mat in src/city.c):
    //   prm.y section; prm.z at-ground | ground mode << 1 | window << 3 | bands << 5 | fine bays << 6;
    //   prm.w palette | shop level << 3; vFacePack face width | next face width << 12 (1/16 m);
    //   vAux previous face width | face << 12 | next face << 18 | corners << 24 | plant floors << 28.
    int section = int(prm.y);
    bool atGround = (prm.z & 1u) == 1u;
    uint groundMode = (prm.z >> 1) & 3u;          // 0 shops, 1 lobby, 2 double-height lobby, 3 void deck
    int type = int((prm.z >> 3) & 3u);            // 0 punched, 1 curtain wall, 2 ribbon, 3 slit
    bool bands = (prm.z & 32u) != 0u, fine = (prm.z & 64u) != 0u;
    uint palette = prm.w & 7u;
    float shopP = float((prm.w >> 3) & 7u) * 0.125;
    int faceDir = int((vAux >> 12) & 63u), nextFace = int((vAux >> 18) & 63u);
    uint corners = (vAux >> 24) & 15u;
    bool endShare = (corners & 2u) != 0u, start90 = (corners & 4u) != 0u, end90 = (corners & 8u) != 0u;
    uint plantCode = vAux >> 28;
    vec3 T = vec3(n.z, 0.0, -n.x);

    uint sh = styleHash(seed);
    float floorH = facadeFloorH(seed);
    float groundH = facadeGroundH(seed) * (groundMode == 2u ? 2.0 : 1.0);
    float bayW = fine ? 1.6 + 0.8 * hf(sh, 4u) : 2.4 + 2.6 * hf(sh, 4u);
    float winW, winH;
    if (type == 0)      { winW = mix(0.45, 0.7, hf(sh, 5u)); winH = mix(0.45, 0.65, hf(sh, 9u)); }
    else if (type == 1) { winW = 0.94; winH = 0.86; }
    else if (type == 2) { winW = 1.0;  winH = mix(0.4, 0.55, hf(sh, 9u)); }
    else                { winW = mix(0.2, 0.32, hf(sh, 5u)); winH = mix(0.7, 0.85, hf(sh, 9u)); }
    float litFrac = 0.16 + 0.42 * hf(sh, 6u);
    float tintSel = hf(sh, 8u);

    // Wall material: the section's palette (choose_surfaces in src/building.c)
    vec3 wallAlb = palette == 0u ? vec3(0.085, 0.080, 0.075) :   // concrete
                   palette == 1u ? vec3(0.080, 0.060, 0.045) :   // grimy brick-brown
                   palette == 2u ? vec3(0.035, 0.055, 0.060) :   // teal metal panels
                   palette == 3u ? vec3(0.110, 0.105, 0.095) :   // pale tile
                   palette == 4u ? vec3(0.020, 0.022, 0.028) :   // black glass
                   palette == 5u ? vec3(0.105, 0.090, 0.068) :   // warm stone
                   palette == 6u ? vec3(0.070, 0.050, 0.030) :   // bronze
                                   vec3(0.014, 0.024, 0.040);    // blue glass
    wallAlb *= (0.7 + 0.6 * hf(sh, 10u)) * 1.4;
    bool glassWall = palette == 4u || palette == 7u || type == 1;

    // uv.x = metres along the face; chunk-local y is absolute height.
    float u = uv.x, vAbs = p.y;
    float faceW = float(vFacePack & 4095u) / 16.0, nextW = float(vFacePack >> 12) / 16.0, prevW = float(vAux & 4095u) / 16.0;
    float relV = vAbs - SIDEWALK_HEIGHT - groundH;
    // Derivatives must be taken in uniform control flow, before any per-pixel branching.
    float dU = fwidth(u), dV = fwidth(vAbs);
    // Whole bays only: stretch the bay so every face ends on a full window at its corners.
    float baseBayW = bayW;
    float nBays = max(1.0, floor(faceW / bayW + 0.5));
    bayW = faceW / nBays;
    float prevBayW = prevW / max(1.0, floor(prevW / baseBayW + 0.5));   // the neighbours' corner bays
    float nextBayW = nextW / max(1.0, floor(nextW / baseBayW + 0.5));

    // ---------------- lobbies and void decks ----------------
    if (atGround && relV < 0.0 && groundMode != 0u) {
        float yy = vAbs - SIDEWALK_HEIGHT;
        float glassTop = groundH - (groundMode == 2u ? 1.2 : 0.7);
        float fs = fract(u / 1.5);
        float aa = dU / 1.5 * 1.5 + dV * 0.2;
        float inWin = band(yy, 0.08, glassTop, dV + 0.01) * (1.0 - band(fs, 0.0, 0.04, aa) - band(fs, 0.96, 1.0, aa));
        vec3 spec;
        vec3 diffL = wallLighting(p, n, V, 30.0, 0.15, vAbs, spec);
        vec3 wallC = wallAlb * 0.9 * diffL + spec;
        uint lh = hash3i(seed ^ 0x10BBu, section, faceDir, int(floor(u / 8.0)));
        Room r = makeRoom(lh, 1.0, tintSel, false, vec3(0.0));
        r.lit = true; r.blinds = 0.0; r.curtain = 0.0;
        r.light = mix(vec3(0.80, 0.86, 1.0), vec3(1.0, 0.86, 0.66), step(0.5, tintSel));
        r.bright = groundMode == 3u ? 0.7 : 1.35;
        r.wall = vec3(0.55, 0.53, 0.50);
        r.person = hf(lh, 11u) < 0.3;
        vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
        vec3 inner = interiorColor(vec2(fract(u / 8.0), clamp(yy / groundH, 0.0, 1.0)), d, 8.0, groundH, 9.0, r);
        float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
        vec3 glass = inner * (1.0 - F) + envReflection(reflect(-V, n)) * F;
        glass = mix(glass, r.light * r.bright * 0.5, smoothstep(0.08, 0.4, dU / 8.0));
        return mix(wallC, glass, clamp(inWin, 0.0, 1.0));
    }

    // ---------------- storefronts ----------------
    if (atGround && relV < 0.0) {
        float along = u;
        float fs = fract(along / 6.0);
        uint h = shopHash(seed, section, faceDir, nextFace, endShare, along, faceW);
        bool lit = hfloat(h) < shopP;
        float yy = vAbs - SIDEWALK_HEIGHT;
        float winTop = groundH - 1.5;
        float dFs = dU / 6.0;
        float aa = dFs * 1.5 + dV * 0.2;
        float inWin = band(fs, 0.05, 0.95, aa) * band(yy, 0.45, winTop, dV + 0.01);
        vec3 spec;
        vec3 diffL = wallLighting(p, n, V, 20.0, 0.1, vAbs, spec);
        vec3 wallC = wallAlb * 0.8 * diffL + spec;
        if (lit) {
            Room r = makeRoom(h, 1.0, 0.0, true, shopColor(h));
            vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
            // Shop bays are 6 m from each face's start, so each face ends on a part bay.
            // A corner shop is this face's corner bay wide and the neighbouring face's
            // corner bay deep; its side window spans 0.3 m in from each end of that bay.
            float seg = floor(along / 6.0), lastSeg = floor((faceW - 0.01) / 6.0);
            float tailW = faceW - 6.0 * lastSeg, tailP = prevW - 6.0 * floor((prevW - 0.01) / 6.0);
            bool shopL = start90 && seg <= 0.0, shopR = end90 && seg >= lastSeg;
            vec3 inner;
            if (shopL || shopR) {
                float Ws = seg >= lastSeg ? tailW : min(6.0, faceW);
                float Ds = shopL && shopR ? min(tailP, min(6.0, nextW)) : shopR ? min(6.0, nextW) : tailP;
                vec2 zR = vec2(0.3, min(5.7, Ds)), zL = vec2(max(0.0, Ds - 5.7), Ds - 0.3);
                inner = cornerInterior(vec2((along - 6.0 * seg) / Ws, clamp(yy / groundH, 0.0, 1.0)), d, Ws, groundH, Ds,
                                       r, shopL, shopR, -V, vAbs, zL, zR, vec2(0.45, winTop));
            } else {
                inner = interiorColor(vec2(fs, clamp(yy / groundH, 0.0, 1.0)), d, 6.0, groundH, 5.0, r);
            }
            float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
            vec3 glass = inner * (1.0 - F) + envReflection(reflect(-V, n)) * F;
            // mullions
            float mull = band(fract(fs * 3.0), 0.0, 0.02, aa * 3.0) + band(fract(fs * 3.0), 0.98, 1.0, aa * 3.0);
            glass = mix(glass, vec3(0.01), clamp(mull, 0.0, 1.0));
            // farther away, just the average glow
            float lod = smoothstep(0.08, 0.4, dFs);
            glass = mix(glass, r.light * r.bright * 0.5, lod);
            return mix(wallC, glass, inWin);
        } else {
            // rolled-down shutter
            float rib = 0.6 + 0.4 * smoothstep(0.3, 0.5, abs(fract(yy / 0.09) - 0.5) * 2.0);
            float grime = 0.5 + 0.7 * fbm(vec2(along * 0.8, yy * 0.8), 0x5417u);
            vec3 shut = vec3(0.07, 0.072, 0.075) * rib * grime;
            float graf = smoothstep(0.62, 0.66, fbm(vec2(along, yy) * 0.9, h));
            shut = mix(shut, neonColor(h >> 6) * 0.08, graf * 0.8);
            vec3 sc = shut * diffL + spec;
            return mix(wallC, sc, inWin);
        }
    }

    // ---------------- upper floors ----------------
    float fx = u / bayW, fy = relV / floorH;
    float bayIdx = floor(fx), floorIdx = floor(fy);
    float fu = fract(fx), fv = fract(fy);
    float fwU = dU / bayW, fwV = dV / floorH;
    float x0 = 0.5 - winW * 0.5, x1 = 0.5 + winW * 0.5;
    float sill = (1.0 - winH) * 0.42;
    float y0 = sill, y1 = sill + winH;
    float cov = filteredPulse(fx, x0, x1, fwU * 1.2) * filteredPulse(fy, y0, y1, fwV * 1.2);
    float lodT = smoothstep(0.05, 0.22, max(fwU, fwV));
    float farT = smoothstep(0.45, 1.4, max(fwU, fwV));

    // Corner rooms belong to both faces; key them (and their floor) by the face they start.
    // Square convex corners get the corner trace, glazed on the neighbouring face too.
    ivec2 rk = cornerKey(faceDir, nextFace, endShare, int(bayIdx), int(nBays) - 1);
    bool glassL = start90 && int(bayIdx) <= 0, glassR = end90 && int(bayIdx) >= int(nBays) - 1;
    bool cornerRoom = glassL || glassR;
    // Louvred plant floors at intervals up tall towers.
    if (plantCode > 0u) {
        int every = 6 + 3 * int(plantCode);
        if (floorIdx > 0.0 && int(floorIdx) % every == every - 1) cov = 0.0;
    }
    bool plantFloor = plantCode > 0u && floorIdx > 0.0 && int(floorIdx) % (6 + 3 * int(plantCode)) == 5 + 3 * int(plantCode);
    // per-floor correlation: some floors are whole lit offices, some are dark
    uint floorH_ = hash3i(seed ^ 0xF100u, int(floorIdx), rk.x, section);
    float litChance = litFrac * (0.25 + 1.5 * hfloat(floorH_));
    uint roomHash = hash3i(seed ^ (uint(rk.x) * 0x9E3779B1u), rk.y, int(floorIdx), section);
    Room room = makeRoom(roomHash, litChance, tintSel, false, vec3(0.0));

    // wall surface
    vec3 wspec;
    float wallShin = glassWall ? 80.0 : 12.0;
    float wallSpecK = glassWall ? 0.4 : 0.05;
    vec3 wdiff = wallLighting(p, n, V, wallShin, wallSpecK, vAbs, wspec);
    float streak = vnoise(vec2(u * 1.3, vAbs * 0.04), seed & 0xFFFFu);
    float grime = mix(0.65, 1.15, streak) * mix(0.8, 1.0, smoothstep(0.0, 25.0, vAbs));
    vec3 wallC = wallAlb * grime;
    if (type == 0 || type == 3) {
        // slab edges and pilasters
        float slab = band(fv, 0.0, 0.05, fwV + 0.005) + band(fv, 0.95, 1.0, fwV + 0.005);
        wallC *= 1.0 - 0.35 * clamp(slab, 0.0, 1.0) * (1.0 - lodT);
        // rust drips under windows
        float drip = band(fu, x0, x1, fwU + 0.01) * band(fv, y0 - 0.22, y0, fwV + 0.01);
        wallC *= 1.0 - 0.3 * drip * (1.0 - lodT);
    }
    if (bands) {
        // spandrel bands: a contrasting panel between the windows of each floor
        vec3 accent = glassWall ? vec3(0.05, 0.055, 0.06) : wallAlb * vec3(0.55, 0.6, 0.7);
        float sp = 1.0 - band(fv, y0 - 0.03, y1 + 0.03, fwV + 0.005);
        wallC = mix(wallC, accent, clamp(sp, 0.0, 1.0) * (1.0 - farT * 0.5));
    }
    if (plantFloor) {
        // louvres: tight horizontal metal slats
        float sl = fract(vAbs / 0.22);
        wallC = vec3(0.06, 0.062, 0.065) * mix(0.55, 1.0, smoothstep(0.3, 0.6, sl) * (1.0 - lodT) + lodT * 0.75);
    }
    vec3 wallLit = wallC * wdiff + wspec;
    if (glassWall) {
        float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
        wallLit += envReflection(reflect(-V, n)) * (0.08 + F * 0.9);
    }
    // shop signs and light below throw colour up the lower facade
    if (atGround && relV < 4.0) {
        uint h = shopHash(seed, section, faceDir, nextFace, endShare, u, faceW);
        if (groundMode == 0u && hfloat(h) < shopP) wallLit += wallC * shopColor(h) * exp(-relV * 0.6) * 1.2;
        else if (groundMode != 0u) wallLit += wallC * vec3(0.7, 0.78, 0.95) * exp(-relV * 0.6) * 0.8;
    }

    // window: near = full interior mapping, mid = per-room flat glow, far = average
    vec3 R = reflect(-V, n);
    float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
    vec3 refl = envReflection(R);
    vec3 roomAvg = room.lit ? room.light * room.bright * 0.42 : vec3(0.003, 0.0035, 0.005);
    if (room.blinds > 0.0 && room.lit) roomAvg *= 1.0 - 0.4 * room.blinds;
    vec3 winMid = roomAvg * (1.0 - F) + refl * F;

    vec3 winNear = winMid;
    if (lodT < 1.0 && cov > 0.0) {
        // A corner room is the neighbouring face's corner bay deep: the same box from either side.
        float W = bayW, H = floorH;
        float D = glassL && glassR ? min(prevBayW, nextBayW) : glassR ? nextBayW : glassL ? prevBayW : bayW * mix(1.3, 2.4, hf(roomHash, 40u));
        vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
        vec3 inner = cornerRoom ? cornerInterior(vec2(fu, fv), d, W, H, D, room, glassL, glassR, -V, vAbs,
                                                 vec2(x0, x1) * D, vec2(x0, x1) * D, vec2(y0, y1) * H)
                                : interiorColor(vec2(fu, fv), d, W, H, D, room);
        float wy = (fv - y0) / winH, wx = (fu - x0) / winW;
        if (room.curtain > 0.5) {
            float open = 0.18 + 0.2 * hf(roomHash, 41u);
            float cMask = 1.0 - band(wx, open, 1.0 - open, 0.01);
            float fold = 0.75 + 0.25 * sin(wx * 60.0);
            vec3 cc = room.lit ? room.curtainC * room.light * room.bright * 0.35 * fold : room.curtainC * 0.01;
            inner = mix(inner, cc, cMask);
        }
        if (room.blinds > 0.0 && wy > 1.0 - room.blinds) {
            float s = fract((fv * H) / 0.055);
            float slat = smoothstep(0.08, 0.2, s) * smoothstep(0.92, 0.8, s);
            vec3 bc = room.lit ? room.light * room.bright * 0.28 * (0.55 + 0.45 * slat) : vec3(0.02) * slat;
            inner = mix(inner, bc, 0.9);
        }
        winNear = inner * (1.0 - F) * 0.9 + refl * F;
        // window frame & mullions
        float fw = 0.05;
        float frameU = fw / (W * winW), frameV = fw / (H * winH);
        float frame = 1.0 - band(wx, frameU, 1.0 - frameU, fwU / winW + 0.002) * band(wy, frameV, 1.0 - frameV, fwV / winH + 0.002);
        if (type == 2) frame = max(frame, band(fract(u / 1.25), 0.0, 0.03, dU / 1.25 + 0.002));
        winNear = mix(winNear, vec3(0.012, 0.012, 0.014) * wdiff * 2.0, clamp(frame, 0.0, 1.0));
    }

    vec3 near = mix(wallLit, winNear, cov);
    vec3 mid = mix(wallLit, winMid, cov);
    vec3 col = mix(near, mid, lodT);
    if (farT > 0.0) {
        // average of many cells: expected lit fraction and window area
        vec3 avgLight = mix(vec3(1.0, 0.68, 0.40), vec3(0.72, 0.88, 1.0), step(0.5, tintSel)) * 1.75 * 0.42;
        vec3 avgWin = avgLight * litFrac * 0.9 * (1.0 - F) + refl * F;
        vec3 far_ = mix(wallLit, avgWin, winW * winH);
        col = mix(col, far_, farT);
    }
    return col;
}

/* ======================================================================
 * Neon signs, screens and other emitters
 * ==================================================================== */

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float t = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * t);
}

// Pseudo-kanji built like real characters: a few horizontal strokes, one or
// two verticals, an occasional sweeping diagonal and sometimes a box radical.
// q in [0,1]^2 (y up); returns distance to the nearest stroke in glyph units.
float glyphDist(uint g, vec2 q) {
    uint h = hash_u32(g);
    float d = 1e9;
    int rows = 2 + int(h % 3u);
    for (int j = 0; j < 4; ++j) {
        if (j >= rows) break;
        uint hj = hash_combine(h, uint(j));
        float y = 0.1 + 0.8 * (float(j) + 0.5) / float(rows) + (hf(hj, 9u) - 0.5) * 0.08;
        float x0 = 0.1 + 0.3 * hfloat(hj) * step(0.5, hf(hj, 1u));
        float x1 = 0.9 - 0.3 * hf(hj, 2u) * step(0.5, hf(hj, 3u));
        d = min(d, segDist(q, vec2(x0, y), vec2(x1, y)));
    }
    int cols = 1 + int((h >> 4) & 1u);
    for (int i = 0; i < 2; ++i) {
        if (i >= cols) break;
        uint hi = hash_combine(h, 10u + uint(i));
        float x = (cols == 1 ? 0.5 : (i == 0 ? 0.3 : 0.7)) + (hfloat(hi) - 0.5) * 0.15;
        float y0 = 0.05 + 0.4 * hf(hi, 1u), y1 = 0.95 - 0.3 * hf(hi, 2u);
        d = min(d, segDist(q, vec2(x, y0), vec2(x, y1)));
    }
    if (((h >> 6) & 1u) == 1u) {
        uint hd = hash_combine(h, 20u);
        vec2 a = vec2(0.15 + 0.3 * hfloat(hd), 0.1 + 0.2 * hf(hd, 1u));
        vec2 b = vec2(0.55 + 0.3 * hf(hd, 2u), 0.5 + 0.4 * hf(hd, 3u));
        if (hf(hd, 4u) < 0.5) { a.x = 1.0 - a.x; b.x = 1.0 - b.x; }
        d = min(d, segDist(q, a, b));
    }
    if (((h >> 7) & 3u) == 0u) {
        vec2 bq = abs(q - vec2(0.5, 0.3)) - vec2(0.25, 0.18);
        d = min(d, abs(max(bq.x, bq.y)) + max(min(max(bq.x, bq.y), 0.0), 0.0));
    }
    return d;
}

float flickerOn(uint seed, uint mode) {
    if (mode == 0u) return 1.0;
    if (mode == 1u) {   // occasional dropout
        float t = floor(gTime * 14.0);
        float burst = step(0.93, hfloat(hash_combine(seed, uint(floor(gTime * 0.5)))));
        return 1.0 - burst * step(0.5, hfloat(hash_combine(seed, uint(t))));
    }
    // failing tube: buzzes and dims
    float ph = fract(gTime * 0.23 + hfloat(seed));
    float buzz = step(0.85, ph) * step(0.4, hfloat(hash_combine(seed, uint(gTime * 20.0))));
    return mix(1.0, 0.15, buzz) * (0.85 + 0.15 * sin(gTime * 120.0));
}

vec3 shadeNeon(vec2 uv, uint seed, uvec4 prm, vec3 rel) {
    uint colI = prm.y & 7u, style = (prm.y >> 3) & 7u, flick = (prm.y >> 6) & 3u;
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec3 col = neonColor(colI);
    vec2 m = uv * vec2(wM, hM);
    float on = flickerOn(seed, flick);

    // glyph layout
    bool vertical = hM >= wM;
    float cell = vertical ? wM * 0.78 : hM * 0.72;
    float pad = vertical ? (wM - cell) * 0.5 : (hM - cell) * 0.5;
    vec2 g;
    float gi;
    if (vertical) {
        float n = max(floor((hM - pad) / (cell * 1.08)), 1.0);
        float yTop = hM - pad;
        float k = (yTop - m.y) / (cell * 1.08);
        gi = floor(k);
        g = vec2((m.x - pad) / cell, 1.0 - fract(k) * 1.08);
        if (gi < 0.0 || gi >= n || g.y < 0.0) g = vec2(-1.0);
    } else {
        float n = max(floor((wM - pad) / (cell * 0.95)), 1.0);
        float k = (m.x - pad) / (cell * 0.95);
        gi = floor(k);
        g = vec2(fract(k) * 0.95, (m.y - pad) / cell);
        if (gi < 0.0 || gi >= n) g = vec2(-1.0);
    }
    float d = 1e3;
    if (all(greaterThanEqual(g, vec2(0.0))) && all(lessThanEqual(g, vec2(1.0))))
        d = glyphDist(hash_combine(seed, uint(gi)), g) * cell;     // metres
    // border tube on boards and banners
    if (style != 0u) {
        vec2 e = min(m, vec2(wM, hM) - m);
        float bd = abs(min(e.x, e.y) - 0.12);
        d = min(d, bd);
    }
    float px = max(fwidth(m.x), fwidth(m.y));
    float tube = 0.025 + cell * 0.018 + px * 0.5;
    float core = 1.0 - smoothstep(tube * 0.5, tube, d);
    float glow = exp(-d * 7.0 / max(cell, 0.6));
    // dead segments on some signs
    if (flick == 2u && hfloat(hash_combine(seed, uint(gi) + 99u)) < 0.3) { core *= 0.05; glow *= 0.05; }

    vec3 bg = vec3(0.012, 0.010, 0.014) + col * 0.05 * on;
    vec3 c = bg + (mix(col, vec3(1.0), 0.45) * core * 7.0 + col * glow * 1.6) * on;
    // far away: average emission (avoids shimmering strokes)
    float lod = smoothstep(0.03, 0.12, px / max(cell, 0.1));
    vec3 avg = bg + col * 0.9 * on;
    return mix(c, avg, lod);
}

vec3 shadeScreen(vec2 uv, uint seed, uvec4 prm) {
    float wM = float(prm.z) * 0.5, hM = float(prm.w) * 0.5;
    float slot = floor((gTime + hfloat(seed) * 30.0) / 9.0);
    uint h = hash_combine(seed, uint(slot));
    uint scene = h & 3u;
    vec3 c1 = neonColor(h >> 4), c2 = neonColor(h >> 8);
    vec3 col;
    float t = gTime;
    vec2 q = uv;
    if (scene == 0u) {
        // portrait advert: gradient, glowing face-like blob and a glyph column
        col = mix(c1 * 0.25, c2 * 0.6, q.y);
        float blob = exp(-dot((q - vec2(0.62, 0.55)) * vec2(wM / hM, 1.0), (q - vec2(0.62, 0.55)) * vec2(wM / hM, 1.0)) * 18.0);
        col += vec3(1.0, 0.75, 0.6) * blob * (0.8 + 0.2 * sin(t * 2.0));
        vec2 g = vec2(q.x / 0.15, fract(q.y * 5.0));
        if (q.x < 0.15) col += c1 * 2.0 * (1.0 - smoothstep(0.03, 0.08, glyphDist(hash_combine(h, uint(q.y * 5.0)), vec2(g.x, g.y))));
    } else if (scene == 1u) {
        // scrolling ticker rows
        float rows = max(floor(hM / 2.5), 2.0);
        float r = floor(q.y * rows);
        float x = q.x * wM / (hM / rows) + t * (1.5 + hfloat(hash_combine(h, uint(r)))) * (mod(r, 2.0) * 2.0 - 1.0);
        vec2 g = vec2(fract(x), fract(q.y * rows));
        float d = glyphDist(hash_combine(h, uint(floor(x)) + uint(r) * 977u), g);
        col = vec3(0.01, 0.02, 0.06) + c1 * 2.2 * (1.0 - smoothstep(0.03, 0.09, d));
    } else if (scene == 2u) {
        // pulsing rings
        vec2 cc = (q - 0.5) * vec2(wM / hM, 1.0);
        float r = length(cc);
        col = mix(c1, c2, 0.5 + 0.5 * sin(r * 22.0 - t * 3.0)) * (0.5 + 0.5 * sin(r * 22.0 - t * 3.0));
        col *= 1.2 * smoothstep(0.75, 0.0, r);
    } else {
        // brand mark: one huge glyph
        vec2 cc = (q - 0.5) * vec2(wM / hM, 1.0) + 0.5;
        float d = glyphDist(h, clamp(cc, 0.0, 1.0));
        col = mix(vec3(0.02, 0.0, 0.03), c2 * 0.15, q.y) + c1 * 2.5 * (1.0 - smoothstep(0.04, 0.1, d)) * (0.8 + 0.2 * sin(t * 4.0));
    }
    // transition static
    float ph = fract((gTime + hfloat(seed) * 30.0) / 9.0);
    if (ph < 0.025) col = vec3(hfloat(hash2i(h, int(q.x * 200.0), int(q.y * 120.0 + t * 60.0)))) * 0.8;
    // LED pixel grid and scanline when close
    vec2 pm = uv * vec2(wM, hM) / 0.12;
    float px = max(fwidth(pm.x), fwidth(pm.y));
    float grid = mix(0.55 + 0.45 * smoothstep(0.45, 0.2, length(fract(pm) - 0.5)), 1.0, smoothstep(0.3, 0.8, px));
    col *= grid * (0.92 + 0.08 * sin(uv.y * 300.0 - gTime * 10.0));
    return col * 1.8;
}

vec3 shadeEmissive(uvec4 prm) {
    uint mode = prm.z;
    if (mode == 1u) {   // aircraft warning light
        float ph = float(prm.w) / 255.0;
        float b = smoothstep(0.0, 0.05, fract(gTime / 1.6 + ph)) * smoothstep(0.35, 0.2, fract(gTime / 1.6 + ph));
        return vec3(1.0, 0.05, 0.02) * (0.3 + 30.0 * b);
    }
    if (mode >= 2u) return lampColor(int(mode) - 2) * 22.0;
    return neonColor(prm.y) * 8.0;
}

vec3 shadeMetal(vec3 p, vec3 rel, vec3 n, vec2 uv, uvec4 prm) {
    vec3 V = -normalize(rel);
    uint v = prm.y;
    vec3 alb = v == 1u ? vec3(0.09, 0.09, 0.095) : v == 2u ? vec3(0.05) : v == 4u ? vec3(0.04, 0.045, 0.042) : vec3(0.03);
    if (v == 1u) alb *= 0.75 + 0.25 * step(0.5, fract(uv.y / 0.12));  // vents
    if (v == 0u) alb = vec3(0.07, 0.068, 0.065) * (0.7 + 0.5 * fbm(gWorld.xz * 0.5 + uv.y, 0x3E7u));
    if (v == 5u) alb = vec3(0.075, 0.072, 0.068) * (0.75 + 0.45 * fbm(gWorld.xz * 2.0 + p.y * 0.7, 0x3E8u));   // concrete
    if (v == 6u) {                                                                                       // solar panel
        vec2 cell = fract(uv * vec2(6.0, 4.0));
        float grid = step(0.06, cell.x) * step(0.06, cell.y);
        vec3 spec;
        vec3 diff = wallLighting(p, n, V, 120.0, 0.9, p.y, spec);
        return vec3(0.008, 0.012, 0.03) * grid * diff + spec + envReflection(reflect(-V, n)) * 0.25;
    }
    if (v == 7u) {                                                                                       // glass pyramid
        float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
        float mull = max(step(0.94, fract(uv.x * 8.0)), step(0.94, fract(uv.y * 8.0)));
        vec3 glow = mix(vec3(0.9, 0.7, 0.45), vec3(0.6, 0.8, 1.0), hfloat(vSeed)) * 0.35;
        return mix(glow * (1.0 - F) + envReflection(reflect(-V, n)) * F, vec3(0.02), mull * 0.8);
    }
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 40.0, 0.25, p.y, spec);
    return alb * diff + spec;
}

vec3 shadeRoof(vec3 p, vec3 rel, vec3 n, vec2 uv, uvec4 prm) {
    vec3 V = -normalize(rel);
    float g = fbm(gWorld.xz * 0.6, 0x400Fu);
    vec3 alb = vec3(0.04, 0.04, 0.042) * (0.6 + 0.8 * g);
    if (prm.w == 1u) {   // clay tiles on a hip roof
        float row = fract(p.y / 0.32);
        alb = vec3(0.075, 0.035, 0.025) * (0.6 + 0.4 * smoothstep(0.0, 0.25, row)) * (0.75 + 0.5 * g);
    }
    if (prm.z == 1u) alb *= 0.8;   // soffit under an overhang
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 10.0, 0.05, p.y, spec);
    return alb * diff + spec;
}

/* Entrances (BDoor in src/building.h): glass doors into a lit lobby, steel
 * service doors and roller-shuttered loading bays. */
vec3 shadeDoor(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec2 m = uv * vec2(wM, hM);
    float aa = max(fwidth(m.x), fwidth(m.y)) + 0.002;
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 60.0, 0.3, p.y, spec);
    float edge = min(min(m.x, wM - m.x), min(m.y, hM - m.y));
    if (prm.y == 0u) {
        vec3 inside = mix(vec3(1.0, 0.86, 0.66), vec3(0.75, 0.86, 1.0), hfloat(seed)) * 0.45;
        float frame = 1.0 - smoothstep(0.07, 0.07 + aa, edge);
        if (wM > 1.6) frame = max(frame, 1.0 - smoothstep(0.03, 0.03 + aa, abs(m.x - wM * 0.5)));   // double doors
        float bar = band(m.y, 1.0, 1.08, aa) * step(0.15, edge);
        float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
        vec3 glass = inside * (1.0 - F) + envReflection(reflect(-V, n)) * F;
        return mix(glass, vec3(0.03) * diff + spec, clamp(frame + bar, 0.0, 1.0));
    }
    if (prm.y == 1u) {
        vec3 steel = vec3(0.06, 0.065, 0.07) * (0.8 + 0.4 * fbm(m * 3.0, seed & 0xFFFFu));
        float win = band(m.x, wM * 0.3, wM * 0.7, aa) * band(m.y, hM * 0.6, hM * 0.85, aa);
        return mix(steel * diff + spec, vec3(0.5, 0.45, 0.35) * 0.4, win);
    }
    float rib = 0.6 + 0.4 * smoothstep(0.3, 0.5, abs(fract(m.y / 0.12) - 0.5) * 2.0);
    vec3 shutter = vec3(0.075, 0.07, 0.06) * rib * (0.6 + 0.6 * fbm(m * 0.8, seed & 0xFFFFu));
    float stripe = step(hM - 0.35, m.y) * step(0.5, fract(m.x / 0.6 + m.y / 0.6));   // hazard stripes at the top
    shutter = mix(shutter, vec3(0.12, 0.09, 0.0), stripe);
    return shutter * diff + spec;
}

/* ====================================================================== */

void main() {
    gC = chunks[vChunk];
    gTime = frame.camPos.w;
    gWorld = vLocal + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
    vec3 n = normalize(vNrm);
    uint mat = vParams.x;
    vec3 V = -normalize(vRel);

    vec3 col;
    if (mat <= MAT_LOT)            col = shadeGround(mat, vLocal, n, V, vUV, vSeed, vParams);
    else if (mat == MAT_FACADE)    col = shadeFacade(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_ROOF)      col = shadeRoof(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_DOOR)      col = shadeDoor(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_NEON)      col = shadeNeon(vUV, vSeed, vParams, vRel);
    else if (mat == MAT_METAL)     col = shadeMetal(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_EMISSIVE)  col = shadeEmissive(vParams);
    else if (mat == MAT_SCREEN)    col = shadeScreen(vUV, vSeed, vParams);
    else {
        // LED strip: steady or chasing
        float chase = vParams.z == 1u ? 0.25 + 0.75 * step(0.5, fract(vUV.x / 6.0 - gTime * 0.7)) : 1.0;
        col = neonColor(vParams.y) * 6.0 * chase;
    }

    col = applyFog(col, vRel);
    outColor = vec4(col, 1.0);
}
