// Part of shaders/city.frag, which includes the parts in order: neon signs, screens, emitters, metal, roofs, doors.

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

float flickerOn(uint seed, uint mode) { return neonFlicker(seed, mode, gTime); }

// Distance (metres) from board point m to the nearest tube's centreline: the
// glyphs, and on boards and banners a border tube. Points between glyphs are
// measured to the nearest one, so the light the tubes throw on the board runs
// on unbroken. gi: that glyph.
float tubeDist(vec2 m, uint seed, float wM, float hM, uint style, bool vertical, float cell, float pad, out float gi) {
    vec2 g;
    if (vertical) {
        float n = max(floor((hM - pad) / (cell * 1.08)), 1.0);
        float k = (hM - pad - m.y) / (cell * 1.08);
        gi = clamp(floor(k), 0.0, n - 1.0);
        g = vec2((m.x - pad) / cell, 1.0 - (k - gi) * 1.08);
    } else {
        float n = max(floor((wM - pad) / (cell * 0.95)), 1.0);
        float k = (m.x - pad) / (cell * 0.95);
        gi = clamp(floor(k), 0.0, n - 1.0);
        g = vec2((k - gi) * 0.95, (m.y - pad) / cell);
    }
    vec2 gc = clamp(g, 0.0, 1.0);
    float d = (glyphDist(hash_combine(seed, uint(gi)), gc) + length(g - gc)) * cell;
    if (style != 0u) {
        vec2 e = min(m, vec2(wM, hM) - m);
        d = min(d, abs(min(e.x, e.y) - 0.12));
    }
    return d;
}

/* A neon sign: glass tubes bent into glyphs (and a border on boards and
 * banners), standing a few centimetres off a dark painted board. The view ray
 * meets the tubes' layer before the board, so seen at an angle they slide
 * across it, apart from the light they throw on it: a tight pool round each
 * tube, falling off as a line source's does. */
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
    float px = max(fwidth(m.x), fwidth(m.y));

    // Where the ray crosses the tubes' layer, `stand` in front of the board, in
    // board metres: the board's tangent frame from the screen derivatives of
    // position and m.
    float stand = 0.03 + cell * 0.02;
    vec3 dpx = dFdx(rel), dpy = dFdy(rel);
    vec2 dmx = dFdx(m), dmy = dFdy(m);
    float det = dmx.x * dmy.y - dmy.x * dmx.y;
    vec2 mt = m;
    if (abs(det) > 1e-12) {
        vec3 T = (dpx * dmy.y - dpy * dmx.y) / det, B = (dpy * dmx.x - dpx * dmy.x) / det;   // d(position)/d(m)
        vec3 N = normalize(cross(T, B)), V = -normalize(rel);
        if (dot(N, V) < 0.0) N = -N;
        vec3 off = V * (stand / max(dot(N, V), 0.2));   // from the board back along the ray to the tubes
        mt += vec2(dot(off, T) / max(dot(T, T), 1e-8), dot(off, B) / max(dot(B, B), 1e-8));
    }
    float gi, giT;
    float dB = tubeDist(m, seed, wM, hM, style, vertical, cell, pad, gi);     // the board, to the tube over it
    float dT = tubeDist(mt, seed, wM, hM, style, vertical, cell, pad, giT);   // the tubes
    // dead segments on some signs
    float liveB = flick == 2u && hfloat(hash_combine(seed, uint(gi) + 99u)) < 0.3 ? 0.05 : 1.0;
    float liveT = flick == 2u && hfloat(hash_combine(seed, uint(giT) + 99u)) < 0.3 ? 0.05 : 1.0;

    // the tube: a white-hot core in its colour; thinner than a pixel, its light spreads over the pixel
    float r = max(0.008, cell * 0.026), rw = max(r, px * 0.6);
    float cover = 1.0 - smoothstep(rw - px * 0.5, rw + px * 0.5, dT);
    float x = clamp(dT / rw, 0.0, 1.0), prof = sqrt(max(1.0 - x * x, 0.0));   // depth of glowing gas along the ray
    vec3 tube = (mix(col, vec3(1.0), 0.6 * prof * prof) * (2.0 + 7.0 * prof) * on * liveT + col * 0.03) * (r / rw);

    // the board: dark paint, lit by the tubes standing off it, and a metal lip round boards and banners
    float s2 = stand * stand, halo = s2 / (s2 + dB * dB);
    vec3 board = vec3(0.005, 0.005, 0.006) + col * on * ((0.4 + 0.8 * halo) * halo * liveB + 0.003);
    if (style != 0u) {
        vec2 e = min(m, vec2(wM, hM) - m);
        board = mix(board, vec3(0.02) + col * on * 0.04, 1.0 - smoothstep(0.03, 0.03 + px, min(e.x, e.y)));
    }
    vec3 c = mix(board, tube, cover);
    // far away: average emission (avoids shimmering strokes)
    float lod = smoothstep(0.06, 0.2, px / max(cell, 0.1));
    vec3 avg = vec3(0.008, 0.007, 0.009) + col * 0.6 * on;
    return mix(c, avg, lod);
}

/* ---------------- authored adverts (assets/adverts/, src/render/adverts.c) ---------------- */
#include "adverts.glsl"
// Each advert's frames, one per layer, block compressed with their mip chain; a screen
// picks its advert's, so the index is non-uniform across a draw.
layout(set = 0, binding = 10) uniform sampler2DArray advertFrames[ADVERT_TABLE_SIZE];

// One LED: red, green and blue emitters side by side across the middle of its cell, laid
// out as rgb_led_billboard_v3.py lays them (ovals 0.27 x 0.34 of the pitch at 0.18, 0.5
// and 0.82 across), each with a soft glow (sigma 0.22 of the pitch) that carries `glow`
// of its light. f: the point in the cell (0..1); px: a pixel's size in cells. Returns
// each channel's light relative to the cell's mean, so a cell gives off its colour on
// average however its light is split.
vec3 ledEmitters(vec2 f, float glow, float px) {
    const vec2  R = vec2(0.135, 0.17);
    const float CORE = PI * 0.135 * 0.17;          // an oval's share of the cell
    const float S = 0.22;
    const float HALO = 2.0 * PI * S * S * 0.954;   // the glow's integral over the cell
    float w = px / R.x;                            // a pixel, in oval radii
    vec3 e;
    for (int k = 0; k < 3; ++k) {
        vec2 d = f - vec2(0.18 + 0.32 * float(k), 0.5);
        d -= round(d);                             // the glow of the like emitters in the cells round it
        float core = 1.0 - smoothstep(1.0 - 0.5 * w, 1.0 + 0.5 * w, length(d / R));
        e[k] = (1.0 - glow) * core / CORE + glow * exp(-dot(d, d) / (2.0 * S * S)) / HALO;
    }
    return e;
}

/* An authored advert on a screen of its shape (advertFits): one LED per pixel of its
 * frames, the picture filling the screen edge to edge. Close to, every LED shows its
 * three emitters; further off, the grid of LEDs; further still they merge into the
 * picture, whose mip chain averages the LEDs under each pixel. Each stage gives off
 * the same light on average, so nothing brightens or dims with distance.
 * t: seconds into the advert, which holds its last frame once it ends. */
vec3 shadeAdvert(int a, vec2 uv, float t) {
    Advert A = advert[a];
    vec2 res = vec2(A.info.yz);
    vec2 q = clamp(vec2(uv.x, 1.0 - uv.y), 0.0, 1.0) * res;   // in LEDs from the top left
    vec2 gx = dFdx(q), gy = dFdy(q);
    float px = max(fwidth(q.x), fwidth(q.y));
    vec2 ci = min(floor(q), res - 1.0), f = q - ci;
    float fr = clamp(floor(t * A.play.x), 0.0, float(A.info.x) - 1.0);
    vec3 cell = textureGrad(advertFrames[nonuniformEXT(a)], vec3((ci + 0.5) / res, fr), gx / res, gy / res).rgb * A.play.y;
    // The emitters while a cell spans three pixels or more; then, to about two, only the
    // grid of LEDs (the cells' fundamental, mean 1), finer detail than the pixels would alias.
    float rgb = 1.0 - smoothstep(0.12, 0.3, px), grid = 1.0 - smoothstep(0.25, 0.45, px);
    vec2 k = 1.0 + 0.85 * cos(2.0 * PI * (f - 0.5));
    vec3 e = mix(mix(vec3(1.0), vec3(k.x * k.y), grid), ledEmitters(f, A.play.z, px), rgb);
    return cell * e + vec3(0.0015);   // the black panel between
}

vec3 shadeScreen(vec2 uv, uint seed, uvec4 prm) {
    float wM = float(prm.z) * 0.5, hM = float(prm.w) * 0.5;
    uint h = screenAdvert(seed, gTime);
    float ph = advertPhase(seed, gTime);
    int ad = advertPick(h, wM / max(hM, 0.5));
    if (ad >= 0 && ph >= ADVERT_STATIC) return shadeAdvert(ad, uv, (ph - ADVERT_STATIC) * ADVERT_SLOT_SECONDS) * 1.8;
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
    if (ph < ADVERT_STATIC) col =vec3(hfloat(hash2i(h, int(q.x * 200.0), int(q.y * 120.0 + t * 60.0)))) * 0.8;
    // LED pixel grid and scanline when close
    vec2 pm = uv * vec2(wM, hM) / 0.12;
    float px = max(fwidth(pm.x), fwidth(pm.y));
    float grid = mix(0.55 + 0.45 * smoothstep(0.45, 0.2, length(fract(pm) - 0.5)), 1.0, smoothstep(0.3, 0.8, px));
    col *= grid * (0.92 + 0.08 * sin(uv.y * 300.0 - gTime * 10.0));
    return col * 1.8;
}

// seed: a street lamp head's fault (lampState)
vec3 shadeEmissive(uvec4 prm, uint seed) {
    uint mode = prm.z;
    if (mode == 1u) {   // aircraft warning light
        float ph = float(prm.w) / 255.0;
        float b = smoothstep(0.0, 0.05, fract(gTime / 1.6 + ph)) * smoothstep(0.35, 0.2, fract(gTime / 1.6 + ph));
        return vec3(1.0, 0.05, 0.02) * (0.3 + 30.0 * b);
    }
    if (mode >= 2u) return vec3(0.02, 0.02, 0.021) + lampColor(int(mode) - 2) * lampState(seed, gTime) * 22.0;   // dead: cold glass
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
        vec3 diff = wallLighting(p, n, V, 120.0, 0.9, p.y - gBase, spec);
        return vec3(0.008, 0.012, 0.03) * grid * diff + spec + envReflection(reflect(-V, n)) * 0.25;
    }
    if (v == 7u) {                                                                                       // glass pyramid
        float F = glassFresnel(n, V);
        float mull = max(step(0.94, fract(uv.x * 8.0)), step(0.94, fract(uv.y * 8.0)));
        vec3 glow = mix(vec3(0.9, 0.7, 0.45), vec3(0.6, 0.8, 1.0), hfloat(vSeed)) * 0.35;
        glassRefl(n, F * (1.0 - mull * 0.8));
        return mix(glow * (1.0 - F) + envReflection(reflect(-V, n)) * F, vec3(0.02), mull * 0.8);
    }
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 40.0, 0.25, p.y - gBase, spec);
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
    vec3 diff = wallLighting(p, n, V, 10.0, 0.05, p.y - gBase, spec);
    return alb * diff + spec;
}

/* Entrances (BDoor in src/gen/building.h): glass doors into a lit lobby, steel
 * service doors and roller-shuttered loading bays. */
vec3 shadeDoor(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec2 m = uv * vec2(wM, hM);
    float aa = max(fwidth(m.x), fwidth(m.y)) + 0.002;
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 60.0, 0.3, p.y - gBase, spec);
    float edge = min(min(m.x, wM - m.x), min(m.y, hM - m.y));
    if (prm.y == 0u) {
        vec3 inside = mix(vec3(1.0, 0.86, 0.66), vec3(0.75, 0.86, 1.0), hfloat(seed)) * 0.45;
        float frame = 1.0 - smoothstep(0.07, 0.07 + aa, edge);
        if (wM > 1.6) frame = max(frame, 1.0 - smoothstep(0.03, 0.03 + aa, abs(m.x - wM * 0.5)));   // double doors
        float bar = band(m.y, 1.0, 1.08, aa) * step(0.15, edge);
        float F = glassFresnel(n, V);
        vec3 glass = inside * (1.0 - F) + envReflection(reflect(-V, n)) * F + paneSheen(p, n, V);
        glassRefl(n, F * (1.0 - clamp(frame + bar, 0.0, 1.0)));
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
