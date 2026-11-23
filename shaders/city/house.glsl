// Part of shaders/city.frag, which includes the parts in order: the houses.

/* ======================================================================
 * The houses' shells (residential roadmap HM3a; src/gen/home_shell.c, src/city/homes.c; what a vertex
 * carries: src/core/house_surf.h). Each surface takes its finish's colour from the palettes
 * (city/house_palette.glsl: the entries src/gen/home_shell.c's styles pick) under an authored grain, one set
 * a covering (HM3b, assets/materials/house_*.json): chosen for the city's air over the references, so
 * weathered, sooted and stained rather than clean (old stock brick on the pre-war styles, stained render,
 * roughcast, grimed siding; slate, sooted concrete tiles, lichened shingles and clay, scuffed sheet). Over
 * them the rain's runs down the walls and a unit's own grime. A far wall paints its windows in its rhythm;
 * near, the windows are real openings, their glass set back.
 * ==================================================================== */

#include "city/house_palette.glsl"

const float HOME_FAR_SHADE = 170.0;   // m: shadeHouse's far path from here
vec3 gLocalHouse;   // the pixel's chunk-local position (paneSheen)
float gHouseDist;   // and its distance

// A box-filtered stripe: 1 on [a, b] of each period of 1 in x, w the pixel's footprint (filteredPulse).
float hStripe(float x, float a, float b, float w) { return filteredPulse(x, a, b, w); }

// A pitched covering's authored set (HM3b); barrel and glazed tiles share the city's clay roof tiles.
int houseRoofSet(uint fin) {
    return fin == HRM_CLAY ? PBR_HOUSE_CLAY : fin == HRM_SLATE ? PBR_HOUSE_SLATE : fin == HRM_CONCRETE ? PBR_HOUSE_CONCRETE_TILE :
           fin == HRM_SHINGLE ? PBR_HOUSE_SHINGLE : fin == HRM_METAL ? PBR_HOUSE_SHEET : PBR_ROOF_TILES;
}
// A wall's (walls, reveals, stacks): old stock brick on the pre-war styles (HOUSE_COL_OLD), roughcast for
// pebbledash and concrete block.
int houseWallSet(uint fin, uint col) {
    if (fin == HW_BRICK)  return (col & HOUSE_COL_OLD) != 0u ? PBR_HOUSE_BRICK : PBR_FACADE_BRICK;
    if (fin == HW_RENDER) return (col & 15u) == 15u ? PBR_HOUSE_ROUGHCAST : PBR_HOUSE_RENDER;
    if (fin == HW_SIDING) return PBR_HOUSE_SIDING;
    if (fin == HW_STONE)  return PBR_FACADE_STONE;
    if (fin == HW_CONCRETE) return PBR_CONCRETE;
    return PBR_HOUSE_ROUGHCAST;
}

// Where a set has not loaded: the courses of a covering drawn here (slate, shingles, a sheet's ribs). q: metres along the eaves and up
// the slope. Returns the tone (x) and the slope of the bump across courses (y: along the slope).
vec2 houseCourses(uint fin, vec2 q, vec2 fw, uint seed) {
    if (fin == HRM_SLATE || fin == HRM_SHINGLE) {
        float course = fin == HRM_SLATE ? 0.24 : 0.14, w = fin == HRM_SLATE ? 0.30 : 0.33;
        float row = floor(q.y / course), f = fract(q.y / course);
        float x = q.x / w + 0.5 * mod(row, 2.0);
        float cell = floor(x);
        float tone = 0.8 + 0.4 * hf(hash2i(seed, int(cell), int(row)), 7u);
        float joint = 1.0 - 0.55 * (1.0 - hStripe(x, 0.03, 0.97, fw.x / w));
        float lip = smoothstep(0.0, 0.18, f);   // the next course's edge shades the foot of this one
        return vec2(tone * joint * mix(0.55, 1.0, lip), (1.0 - f) * 0.25);
    }
    if (fin == HRM_METAL) {
        float r = sin(q.x / 0.076 * 6.2831853);
        return vec2(0.85 + 0.15 * r, 0.0);
    }
    return vec2(1.0, 0.0);
}

// Glazing bars over a window of size s (m) at uv: a frame round it and its pattern's bars (HG_*). Returns the
// bars' coverage.
float houseBars(vec2 uv, vec2 s, uint pattern, float aa) {
    float fr = 0.055, bar = 0.022;
    float edge = min(min(uv.x, s.x - uv.x), min(uv.y, s.y - uv.y));
    float m = 1.0 - smoothstep(fr, fr + aa, edge);
    if (pattern == HG_SASH || pattern == HG_GEORGIAN) m = max(m, 1.0 - smoothstep(0.03, 0.03 + aa, abs(uv.y - 0.5 * s.y)));   // the meeting rail
    if (pattern == HG_GEORGIAN) {
        float cx = s.x / 3.0, cy = s.y / 6.0;
        m = max(m, 1.0 - smoothstep(bar * 0.5, bar * 0.5 + aa, abs(fract(uv.x / cx + 0.5) - 0.5) * cx));
        m = max(m, 1.0 - smoothstep(bar * 0.5, bar * 0.5 + aa, abs(fract(uv.y / cy + 0.5) - 0.5) * cy));
    } else if (pattern == HG_CASEMENT) {
        m = max(m, 1.0 - smoothstep(0.03, 0.03 + aa, abs(uv.x - 0.5 * s.x)));
        m = max(m, 1.0 - smoothstep(0.03, 0.03 + aa, abs(uv.y - 0.72 * s.y)));
    } else if (pattern == HG_CURTAIN) {
        // a glazed wall's mullions about a metre apart (a pane under 1.4 m keeps its frame alone)
        float n = max(1.0, floor(s.x / 1.0 + 0.5)), cx = s.x / n;
        if (n > 1.0) m = max(m, 1.0 - smoothstep(0.022, 0.022 + aa, abs(fract(uv.x / cx + 0.5) - 0.5) * cx));
    } else if (pattern == HG_GRID) {
        float cx = s.x / max(2.0, floor(s.x / 0.4)), y0 = 0.6 * s.y;
        if (uv.y > y0) m = max(m, 1.0 - smoothstep(bar * 0.5, bar * 0.5 + aa, abs(fract(uv.x / cx + 0.5) - 0.5) * cx));
        m = max(m, 1.0 - smoothstep(0.03, 0.03 + aa, abs(uv.y - y0)));
    }
    return clamp(m, 0.0, 1.0);
}

// A window's glass seen from outside: the sky's reflection, and behind it a dark room or, by the homes' hours
// (cityHours), a warm one behind its blind (HM6 gives the rooms their own look). key: the window's own.
vec3 houseGlass(vec3 n, vec3 V, uint key, vec2 uv, vec2 s) {
    float F = glassFresnel(n, V);
    float lit = hf(key, 3u) < 0.38 * cityHours(true) * (1.0 - smoothstep(0.1, 0.6, gLampDay)) ? 1.0 : 0.0;
    float blind = hf(key, 4u);
    vec3 warm = mix(vec3(1.0, 0.62, 0.30), vec3(1.0, 0.80, 0.55), hf(key, 5u)) * mix(0.6, 1.4, hf(key, 6u));
    float drop = smoothstep(0.0, 0.25, uv.y / max(s.y, 0.1) - blind * 0.6);   // a blind part down
    vec3 room = vec3(0.012, 0.012, 0.014) + lit * warm * mix(0.42, 0.14, drop);   // a lamp behind a curtain, not a light box
    vec3 col = room * (1.0 - F) + envReflection(reflect(-V, n)) * F;
    if (gHouseDist < 150.0) col += paneSheen(gLocalHouse, n, V);   // its lamps' blooms: near only (a far wall's painted panes)
    glassRefl(n, F);
    return col;
}

// A far wall's windows, painted in its rhythm (house_surf.h) over its colour c: bays of about 2.7 m along it, a
// window a storey. Each window's key from its bay and storey, the unit and the way the wall faces.
vec3 houseFarWindows(vec3 c, vec3 n, vec3 V, vec2 uv, vec2 fw, uint seed, uint aux) {
    float storeyH = 2.0 + float((aux >> HOUSE_AUX_STOREYH) & 127u) * 0.01;
    float plinth = float((aux >> HOUSE_AUX_PLINTH) & 255u) * 0.01;
    float storeys = float(((aux >> HOUSE_AUX_STOREYS) & 3u) + 1u);   // storeys less one (1-4, HM5e1)
    float L = float((aux >> HOUSE_AUX_LENGTH) & 1023u) * 0.1;
    float nb = max(1.0, floor(L / 2.7 + 0.5)), bw = L / nb, ww = min(1.2, bw - 0.6);
    float k = floor((uv.y - plinth) / storeyH);
    if (ww <= 0.3 || k < 0.0 || k >= storeys || L <= 1.4) return c;
    float fy = uv.y - plinth - k * storeyH;
    float sill = k < 0.5 ? 0.75 : 0.85, head = min(2.15, storeys * storeyH - k * storeyH - 0.25);
    float bx = fract(uv.x / bw) * bw - 0.5 * bw;
    float aa = max(fw.x, fw.y);
    float win = (1.0 - smoothstep(0.5 * ww - aa, 0.5 * ww + aa, abs(bx))) * (smoothstep(sill - aa, sill + aa, fy) - smoothstep(head - aa, head + aa, fy));
    if (win <= 0.0 || head <= sill + 0.6) return c;
    int facing = int(round(atan(n.z, n.x) * 0.63661977)) & 3;
    uint key = hash3i(seed ^ 0x6A56u, int(floor(uv.x / bw)), int(k), facing);
    vec3 g = houseGlass(n, V, key, vec2(bx + 0.5 * ww, fy - sill), vec2(ww, head - sill));
    gRefl.w *= win;
    return mix(c, g, win);
}

// A shop front under a house (HM5e1; house_surf.h: HF_GARAGE, pattern 1): its roller shutter down out of its hours, else
// rolled up into its box over the shop behind, lit by its own strip light: shelves of goods in bays over a counter. Shops
// keep their own hours (most 7-22, a few all night, a few never up); key: the shop's own, from its lower left corner.
vec3 houseShop(vec3 p, vec3 n, vec3 V, vec2 uv, vec2 s, uint seed, uint col, float aa, float h, float wet) {
    vec3 c0 = gWorld - uv.x * vec3(-n.z, 0.0, n.x) - vec3(0.0, uv.y, 0.0);
    uint key = hash3i(seed ^ 0x5409u, int(round(c0.x * 4.0)), int(round(c0.y * 4.0)), int(round(c0.z * 4.0)));
    float hr = mod(frame.clock.x, 24.0);
    float hours = smoothstep(6.5, 8.0, hr) * (1.0 - smoothstep(21.0, 23.0, hr));
    bool open = hf(key, 1u) < 0.08 + 0.74 * hours;
    const float boxH = 0.34;
    vec3 spec;
    if (open && uv.y < s.y - boxH) {
        // the shop: shelves every 0.42 m, packs on them 4-12 cm wide, each its own height and colour (mostly card, tins
        // and grey, a few printed), the back wall dark behind and over them, each shelf's shadow under the next; a
        // counter's dark front below 0.9 m at one side
        float fy = fract(uv.y / 0.42), row = floor(uv.y / 0.42);
        float pw = mix(0.05, 0.12, hf(hash3i(key, int(row), 0, 9), 1u));
        float px = uv.x / pw;
        uint pk = hash3i(key, int(floor(px)), int(row), 5);
        float tallP = mix(0.3, 0.82, hf(pk, 1u));
        vec3 card = mix(vec3(0.36, 0.28, 0.19), vec3(0.46, 0.45, 0.42), hf(pk, 2u));
        vec3 accent = vec3(hf(pk, 3u), hf(pk, 4u), hf(pk, 5u));
        accent = mix(vec3(dot(accent, vec3(0.333))), accent, 0.6) * 0.55 + 0.06;
        vec3 goods = mix(card, accent, step(0.72, hf(pk, 6u)));
        float edge = aa / pw * 2.0;
        float inPack = (1.0 - smoothstep(tallP - 0.02, tallP, fy)) * smoothstep(0.06, 0.06 + edge, fract(px)) * step(0.06, fy);
        inPack *= step(0.12, hf(pk, 7u));                                                  // a gap where a pack has gone
        vec3 inside = mix(vec3(0.07, 0.065, 0.06), goods * mix(0.7, 1.0, fy / max(tallP, 0.1)), inPack);
        inside *= 1.0 - 0.45 * smoothstep(0.7, 1.0, fy);                                     // under the shelf over it
        inside = mix(inside, vec3(0.42, 0.41, 0.39), 1.0 - smoothstep(0.0, 0.05 + aa * 3.0, fy));   // the shelf's edge
        float counter = step(uv.y, 0.9) * step(hf(key, 6u) < 0.5 ? uv.x : s.x - uv.x, 0.45 * s.x);
        inside = mix(inside, vec3(0.04, 0.035, 0.03), counter * 0.85);
        vec3 light = mix(vec3(1.0, 0.84, 0.62), vec3(0.86, 0.95, 1.0), step(0.55, hf(key, 2u))) * mix(0.55, 1.1, hf(key, 3u));
        float fall = mix(0.5, 1.0, smoothstep(0.0, s.y - boxH, uv.y));
        gRefl.w *= 0.0;
        return inside * light * fall * 0.85;
    }
    // the shutter down, or its box over the open shop: galvanised slats, sooted and streaked; a few painted
    vec3 alb = mix(vec3(0.30, 0.31, 0.32), H_GARAGE[col % 6u] * 0.8, step(0.75, hf(key, 5u)));
    float grime = 0.55 + 0.6 * fbm(vec2(uv.x * 0.9, uv.y * 0.9) + vec2(float(key & 255u)), 0x5417u);
    vec3 nn = n;
    vec2 q = vec2(uv.x, -uv.y);
    vec3 T = normalize(vec3(n.z, 0.0, -n.x)), B = vec3(0.0, 1.0, 0.0);
    if (pbrLoaded(PBR_SHUTTER)) { Grain g = pbrGrain(PBR_SHUTTER, q, dFdx(q), dFdy(q), T, B, n); alb *= g.tint * g.ao; nn = g.n; }
    else alb *= 0.65 + 0.35 * smoothstep(0.3, 0.5, abs(fract(uv.y / 0.08) - 0.5) * 2.0);
    float rail = step(uv.y, 0.06);   // the bottom rail
    alb *= grime * mix(1.0, 0.6, rail) * mix(1.0, 0.75, wet);
    vec3 diff = wallLighting(p, nn, V, mix(20.0, 60.0, wet), mix(0.12, 0.35, wet), h, spec);
    return alb * diff + spec;
}

// A door's leaves (house_surf.h HF_DOOR): pattern 4 a pair of plank leaves, iron-studded, ring handles (HM5e1: an arched
// door's, China's gate's), else panels.
float houseDoorPlanks(vec2 uv, vec2 s, float aa, inout vec3 alb) {
    float leaf = abs(uv.x - 0.5 * s.x);
    float board = 1.0 - 0.4 * (1.0 - hStripe(fract(uv.x / 0.15), 0.05, 0.95, aa / 0.15));
    float split = smoothstep(0.012, 0.012 + aa, leaf);
    vec2 st = vec2((fract(uv.x / 0.3) - 0.5) * 0.3, (fract(uv.y / 0.35) - 0.5) * 0.35);
    float stud = (1.0 - smoothstep(0.016, 0.016 + aa, length(st))) * step(0.15, uv.y);
    float ring = 0.0;
    for (int k = 0; k < 2; ++k) {
        vec2 rc = vec2(0.5 * s.x + (k == 0 ? -0.16 : 0.16), 1.15);
        ring = max(ring, 1.0 - smoothstep(0.008, 0.008 + aa, abs(length(uv - rc) - 0.055)));
    }
    alb = mix(alb, vec3(0.06, 0.055, 0.05), max(stud * 0.85, ring));
    return board * mix(0.3, 1.0, split);
}

vec3 shadeHouse(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, uint aux) {
    vec3 V = -normalize(rel);
    gLocalHouse = p;
    uint surf = prm.y, fin = prm.z, col = prm.w;
    float h = p.y - gBase;
    gHouseDist = length(rel);
    float wet = WETNESS > 0.0 ? WETNESS * (1.0 - shelterAt(rel)) : 0.0;
    vec2 uvx = dFdx(uv), uvy = dFdy(uv);
    vec2 fw = vec2(length(vec2(uvx.x, uvy.x)), length(vec2(uvx.y, uvy.y))) + 1e-4;
    float vary = mix(0.88, 1.12, hf(seed, 0x41u));
    vec3 alb, nn = n, spec;
    float shin = 12.0, specK = 0.04;

    // far off (past HOME_FAR_SHADE: the far shells and the near layer's edge): the palette's colour under the
    // lights, without the grain, the courses or the noise, which a pixel there spans many of (the grain's
    // average shade stands in for them)
    if (gHouseDist > HOME_FAR_SHADE && surf != HF_GLASS) {
        if (surf == HF_ROOF || surf == HF_FLAT) alb = houseRoofColour(fin, col) * 0.85 * mix(0.7, 1.05, vnoise(gWorld.xz * 0.15, 0x4A12u));
        else if (surf == HF_SOFFIT || surf == HF_FASCIA || surf == HF_TRIM || surf == HF_CEILING) alb = H_TRIM[col & 7u] * 0.9;
        else if (surf == HF_DOOR) alb = H_DOOR[col % 12u];
        else if (surf == HF_GARAGE) alb = H_GARAGE[col % 6u];
        else if (surf == HF_STEP) alb = vec3(0.30, 0.29, 0.27);
        else alb = houseWallColour(fin, col) * (fin == HW_SIDING ? 0.85 : 0.82);
        alb *= vary * mix(1.0, 0.75, wet);
        vec3 diff = wallLighting(p, n, V, mix(12.0, 60.0, wet), surf == HF_ROOF ? mix(0.05, 0.35, wet) : 0.04, h, spec);
        vec3 c = alb * diff + spec;
        if ((aux & HOUSE_AUX_WINDOWS) != 0u && surf == HF_WALL) c = houseFarWindows(c, n, V, uv, fw, seed, aux);
        return c;
    }

    if (surf == HF_GLASS) {
        vec2 s = vec2(float((aux >> HOUSE_AUX_W) & 1023u), float((aux >> HOUSE_AUX_H) & 1023u)) * 0.01;
        float aa = max(fw.x, fw.y);
        float bars = houseBars(uv, s, (aux >> HOUSE_AUX_PATTERN) & 15u, aa);
        // HM5: where the kit's frame stands in it, its bars are painted only as the frame shrinks away (kit_gpu.c:
        // a frame fades out over the last quarter of KIT_DISTANCE)
        if ((aux & HOUSE_AUX_FRAMED) != 0u) bars *= smoothstep(92.0, 116.0, gHouseDist);
        // the window's own key, from its lower left corner (an opening's u runs along (-n.z, n.x): src/city/homes.c)
        vec3 c = gWorld - uv.x * vec3(-n.z, 0.0, n.x) - vec3(0.0, uv.y, 0.0);
        uint key = hash3i(seed ^ 0x6A55u, int(round(c.x * 4.0)), int(round(c.y * 4.0)), int(round(c.z * 4.0)));
        // HM5c2: an insertion's panes share their storey's key (a ribbon window lit as one room)
        if ((aux & HOUSE_AUX_SHARED) != 0u) key = hash3i(seed ^ 0x6A57u, int(round(c.y * 4.0)), int((aux >> HOUSE_AUX_INS) & 15u), 0);
        vec3 glass = houseGlass(n, V, key, uv, s);
        vec3 diff = wallLighting(p, n, V, 30.0, 0.15, h, spec);
        vec3 frame = H_TRIM[col & 7u] * 0.9 * diff + spec;
        gRefl.w *= 1.0 - bars;
        return mix(glass, frame, bars);
    }

    if (surf == HF_ROOF) {
        alb = houseRoofColour(fin, col) * vary;
        vec3 T = abs(n.y) > 0.98 ? vec3(1.0, 0.0, 0.0) : normalize(vec3(n.z, 0.0, -n.x)), B = normalize(cross(n, T));   // (level: a kit dormer's cheek's top)
        if (dot(B, vec3(0.0, 1.0, 0.0)) < 0.0) B = -B;
        vec2 q = vec2(uv.x, -uv.y);
        int m = houseRoofSet(fin);
        if (pbrLoaded(m)) {
            Grain g = pbrGrain(m, q * (fin == HRM_GLAZED ? 0.85 : 1.0), dFdx(q), dFdy(q), T, B, n);
            alb *= g.tint * g.ao;
            nn = g.n;
            specK = (fin == HRM_METAL ? 0.12 : 0.05) * grainSpec(g);
            if (fin == HRM_METAL) shin = 40.0;
        } else {
            vec2 cr = houseCourses(fin, uv, fw, seed);
            alb *= cr.x * (0.85 + 0.3 * vnoise(gWorld.xz * 0.7, 0x4A11u));
            nn = normalize(n - B * cr.y * 0.6);
            if (fin == HRM_METAL) { float r = cos(uv.x / 0.076 * 6.2831853); nn = normalize(n + T * r * 0.35); shin = 40.0; specK = 0.2; }
        }
        alb *= mix(0.7, 1.05, vnoise(gWorld.xz * 0.15, 0x4A12u));   // weathering in blotches
        alb *= mix(1.0, 0.7, wet);
        shin = mix(shin, 60.0, wet); specK = mix(specK, 0.35, wet);
        vec3 diff = wallLighting(p, nn, V, shin, specK, h, spec);
        return alb * diff + spec;
    }

    if (surf == HF_FLAT) {
        alb = houseRoofColour(fin, col) * vary;
        vec3 T = vec3(1.0, 0.0, 0.0), B = vec3(0.0, 0.0, -1.0);
        vec2 q = vec2(gWorld.x, gWorld.z);
        int m = fin == HRM_FELT ? PBR_ROOF_GRAVEL : PBR_CONCRETE;
        if (pbrLoaded(m)) { Grain g = pbrGrain(m, q, dFdx(q), dFdy(q), T, B, n); alb *= g.tint * g.ao; nn = g.n; }
        alb *= mix(0.65, 1.1, vnoise(gWorld.xz * 0.3, 0x4A13u)) * mix(1.0, 0.65, wet);
        vec3 diff = wallLighting(p, nn, V, mix(10.0, 80.0, wet), mix(0.03, 0.5, wet), h, spec);
        return alb * diff + spec;
    }

    if (surf == HF_SOFFIT || surf == HF_FASCIA || surf == HF_TRIM || surf == HF_CEILING) {
        alb = H_TRIM[col & 7u] * (surf == HF_SOFFIT || surf == HF_CEILING ? 0.75 : 1.0);
        vec2 q = vec2(uv.x, -uv.y);
        vec3 T = abs(n.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : normalize(vec3(n.z, 0.0, -n.x)), B = normalize(cross(n, T));
        if (pbrLoaded(PBR_KIT_PAINT)) { Grain g = pbrGrain(PBR_KIT_PAINT, q, dFdx(q), dFdy(q), T, B, n); alb *= mix(vec3(1.0), g.tint, 0.4); }
        alb *= mix(0.8, 1.0, vnoise(gWorld.xz * 0.9 + gWorld.y, 0x4A14u));
        vec3 diff = wallLighting(p, nn, V, 20.0, 0.06, h, spec);
        return alb * diff + spec;
    }

    if (surf == HF_DOOR || surf == HF_GARAGE) {
        vec2 s = vec2(float((aux >> HOUSE_AUX_W) & 1023u), float((aux >> HOUSE_AUX_H) & 1023u)) * 0.01;
        float aa = max(fw.x, fw.y);
        alb = surf == HF_DOOR ? H_DOOR[col % 12u] : H_GARAGE[col % 6u];
        float shade = 1.0;
        uint dpat = (aux >> HOUSE_AUX_PATTERN) & 15u;
        if (surf == HF_GARAGE && dpat == 1u) return houseShop(p, n, V, uv, s, seed, col, aa, h, wet);
        if (surf == HF_DOOR && dpat == 4u) shade = houseDoorPlanks(uv, s, aa, alb);
        else if (surf == HF_GARAGE) {
            shade = 1.0 - 0.35 * (1.0 - hStripe(uv.y / 0.53, 0.04, 1.0, aa / 0.53));   // the sections' joints
        } else {
            uint pat = (aux >> HOUSE_AUX_PATTERN) & 15u;
            // panels: four, or two over a glazed top
            vec2 c = uv / s;
            float px = abs(fract(c.x * 2.0) - 0.5), py = abs(fract(c.y * (pat == 1u ? 2.0 : 2.0)) - 0.5);
            float panel = (1.0 - smoothstep(0.32, 0.32 + aa * 2.0 / s.x, px)) * (1.0 - smoothstep(0.36, 0.36 + aa * 2.0 / s.y, py));
            shade = mix(0.82, 1.0, panel);
            if (pat == 1u && c.y > 0.62 && c.y < 0.92 && c.x > 0.2 && c.x < 0.8) {
                vec3 dg = houseGlass(n, V, hash_combine(seed, 0xD0u), uv, s);
                return dg;
            }
            float knob = 1.0 - smoothstep(0.025, 0.025 + aa, length(uv - vec2(s.x - 0.12, 1.0)));
            alb = mix(alb, vec3(0.5, 0.42, 0.2), knob);
        }
        if (pbrLoaded(PBR_KIT_PAINT)) {
            vec2 q = vec2(uv.x, -uv.y);
            Grain g = pbrGrain(PBR_KIT_PAINT, q, dFdx(q), dFdy(q), normalize(vec3(n.z, 0.0, -n.x)), vec3(0.0, 1.0, 0.0), n);
            alb *= mix(vec3(1.0), g.tint, 0.5);
        }
        vec3 diff = wallLighting(p, n, V, 30.0, 0.1, h, spec);
        return alb * shade * diff + spec;
    }

    if (surf == HF_STEP) {
        alb = fin == HST_STONE ? vec3(0.40, 0.37, 0.32) : fin == HST_TILE ? vec3(0.42, 0.20, 0.12) : fin == HST_TIMBER ? vec3(0.22, 0.15, 0.10) : vec3(0.34, 0.33, 0.31);
        vec3 T, B; vec2 q;
        planarFrame(n, gWorld, T, B, q);
        int m = fin == HST_TIMBER ? PBR_WOOD_LIGHT : fin == HST_TILE ? PBR_FLOOR_TILE : PBR_CONCRETE;
        if (pbrLoaded(m)) { Grain g = pbrGrain(m, q, dFdx(q), dFdy(q), T, B, n); alb *= g.tint * g.ao; nn = g.n; }
        alb *= mix(1.0, 0.7, wet);
        vec3 diff = wallLighting(p, nn, V, mix(10.0, 60.0, wet), mix(0.04, 0.3, wet), h, spec);
        return alb * diff + spec;
    }

    // walls, reveals, stacks, copings: the wall's finish
    alb = houseWallColour(fin, col) * vary;
    if (surf == HF_COPING) alb = fin == HW_BRICK ? vec3(0.38, 0.36, 0.33) : alb * 1.08;
    vec3 T = abs(n.y) > 0.9 ? vec3(1.0, 0.0, 0.0) : normalize(vec3(n.z, 0.0, -n.x));
    vec3 B = abs(n.y) > 0.9 ? vec3(0.0, 0.0, -1.0) : vec3(0.0, 1.0, 0.0);
    vec2 q = abs(n.y) > 0.9 ? vec2(gWorld.x, gWorld.z) : vec2(uv.x, -uv.y);
    int m = houseWallSet(fin, col);
    if (surf == HF_COPING && fin == HW_BRICK) m = PBR_CONCRETE;
    if (fin == HW_SIDING && surf != HF_COPING && !pbrLoaded(m)) {
        // boards lapped up the wall, the lower edge of each in its own shadow
        float f = fract(uv.y / 0.18);
        alb *= mix(0.62, 1.0, smoothstep(0.0, 0.12, f)) * (0.95 + 0.1 * hf(hash_combine(seed, uint(floor(uv.y / 0.18)) + 0x5D00u), 1u));
        nn = normalize(n - vec3(0.0, 1.0, 0.0) * 0.25 * (1.0 - f));
    } else if (fin == HW_CONCRETE && surf != HF_COPING) {
        // cast concrete's panels (HM5c2): a joint every 1.2 m along and 0.6 m up (on a level face, in the world's x and z)
        vec2 pq = abs(n.y) > 0.9 ? gWorld.xz : uv;
        vec2 pf = abs(n.y) > 0.9 ? vec2(fw.x) : fw;
        if ((col & 0x10u) != 0u) {
            // HM5c2b: Japan's box clad in hexagonal panels (its sheet's panel 4), 0.55 m across their flats
            const float S = 0.55;
            vec2 hp = pq / S, R = vec2(1.0, 1.7320508), Hh = 0.5 * R;
            vec2 a = mod(hp, R) - Hh, b = mod(hp - Hh, R) - Hh;
            vec2 gv = dot(a, a) < dot(b, b) ? a : b;
            vec2 q = abs(gv);
            float e = 0.5 - max(dot(q, vec2(0.5, 0.8660254)), q.x);
            float joint = smoothstep(0.012, 0.03 + max(pf.x, pf.y) / S, e);
            vec2 id = floor((hp - gv) * 2.0 + 0.5);
            alb *= mix(0.62, 1.0, joint) * mix(0.86, 1.08, hf(hash2i(seed ^ 0xC0C1u, int(id.x), int(id.y)), 1u));
        } else {
            float joint = min(hStripe(pq.x / 1.2, 0.012, 0.988, pf.x / 1.2), hStripe(pq.y / 0.6, 0.02, 0.98, pf.y / 0.6));
            alb *= mix(0.72, 1.0, joint) * mix(0.9, 1.06, hf(hash2i(seed ^ 0xC0C0u, int(floor(pq.x / 1.2)), int(floor(pq.y / 0.6))), 1u));
        }
    } else if (fin == HW_BLOCK) {
        float bx = (uv.x + 0.2 * mod(floor(uv.y / 0.2), 2.0)) / 0.4, by = uv.y / 0.2;
        float joint = min(hStripe(bx, 0.03, 0.97, fw.x / 0.4), hStripe(by, 0.06, 0.94, fw.y / 0.2));
        alb *= mix(0.7, 1.0, joint);
    }
    if (pbrLoaded(m)) {
        Grain g = pbrGrain(m, q, dFdx(q), dFdy(q), T, B, nn);
        alb *= g.tint * g.ao;
        nn = g.n;
        specK = 0.04 * grainSpec(g);
    }
    if (surf == HF_REVEAL) alb *= 0.85;
    // grime: splashback towards the ground, blotches, and the rain's runs down the wall, each unit's own amount
    // (upkeep, HM6, will set it; until then a spread by the unit's seed)
    float along = dot(gWorld.xz, T.xz);
    float grime = mix(0.35, 1.0, hf(seed, 0x4A16u));
    float runs = smoothstep(0.55, 0.85, vnoise(vec2(along * 3.1, gWorld.y * 0.22), 0x4A17u)) * smoothstep(0.3, 0.7, vnoise(vec2(along * 0.7, gWorld.y * 0.05), 0x4A18u));
    alb *= mix(0.8, 1.0, smoothstep(0.0, 1.2, h)) * mix(0.85, 1.05, vnoise(vec2(along * 0.8, gWorld.y * 0.15), 0x4A15u));
    alb *= 1.0 - 0.28 * grime * runs * (abs(n.y) > 0.9 ? 0.0 : 1.0);
    alb *= mix(1.0, 0.8, wet);
    vec3 diff = wallLighting(p, nn, V, shin, specK, h, spec);
    vec3 c = alb * diff + spec;

    if ((aux & HOUSE_AUX_WINDOWS) != 0u && surf == HF_WALL) c = houseFarWindows(c, n, V, uv, fw, seed, aux);
    return c;
}

/* ======================================================================
 * The character kit (residential roadmap HM4: src/gen/pmodel_house.c, its materials PMAT_HTRIM on in
 * src/gen/pmodel.h), drawn instanced as MAT_KIT but shaded here, with the shells' own palettes and sets, so a
 * sill, a soldier course or a dormer's cheek matches the walls and roof it stands on. Its colours come from the
 * instance (house_surf.h HOUSE_KIT_*: the unit's trim, door, brick, stone, rainwater goods; the roof in its part
 * byte). Runs of stone, brick and cement are stretched along their length by their instances, so their grain is
 * mapped in the world, not along the module's own metres. occ: the module's baked occlusion; aux: its piece.
 * ==================================================================== */
const uint PMAT_HTRIM = 49u, PMAT_HDOOR = 50u, PMAT_HSTONE = 51u, PMAT_HBRICK = 52u, PMAT_HCLAY = 53u, PMAT_HTILE = 54u,
           PMAT_HRAIN = 55u, PMAT_HLEAD = 56u, PMAT_HIRON = 57u, PMAT_HPLANT = 58u, PMAT_HGLASS = 59u, PMAT_HTIMBER = 60u,
           PMAT_HUPVC = 61u, PMAT_HROOF = 62u, PMAT_HCEMENT = 63u,   // src/gen/pmodel.h
           // HM5c1: the shared retrofit kit (src/gen/pmodel_retrofit.c)
           PMAT_HGALV = 64u, PMAT_HCOPPER = 65u, PMAT_HPOLY = 66u, PMAT_HSOLAR = 67u, PMAT_HBREEZE = 68u, PMAT_HBLOOM = 69u,
           PMAT_HLED = 70u, PMAT_HLAMP = 71u,
           PMAT_HVINE = 72u;   // HMv: the vines' cards (shadeVine)
bool kitIsHouse(uint m) { return m >= PMAT_HTRIM && m <= PMAT_HVINE; }

/* ======================================================================
 * HMv: the vines. A climbing plant is one card (src/gen/pmodel_retrofit.c m_r_vine) standing the leaves' depth
 * (vine.json params x: pmodel_retrofit.h HR_VINE_DEPTH) off its wall, W x H metres (kit.vert hands them over from
 * the instance), its uv the card's unit square. Over it the plant grows as columns of its atlas cell (vine.json:
 * the instance's part the plant, HR_VINE_*), each params z metres across and repeating every params y of its
 * widths up the wall, overlapping where the card is wider than one: each its own way round, from its own height in
 * the tile, the outer ones stopping short of the top, every top thinning to its highest leaves. The leaves are a
 * height field from the card (1) down to the wall (0): the eye's ray steps down through it (steep parallax, the
 * highest column's height at each step) and where it reaches the wall with nothing met, the wall shows: the pixel
 * is cut, and its coverage of a pixel's samples follows the atlas's alpha (so far off, where the mips have blurred
 * the leaves, they thin by their coverage). What the ray met shadows itself from each light (pbr.glsl
 * reliefShadow). The cards are drawn after the rest of the kit (pm_module_cut) and kept out of the depth prepass
 * (prepass.frag): the wall's shadows are theirs.
 * ==================================================================== */
const float VINE_CELLS = 6.0;   // plants in the atlas
struct VineCard { float W, H, cw, tileH, x0, pitch, species; int n; uint seed; };

// Column i of a card: its left edge (card metres), mirrored or not, where in its tile it starts, how high it climbs.
void vineColumn(VineCard c, int i, out float left, out bool mir, out float voff, out float top) {
    uint h = hash_combine(c.seed, uint(i) * 0x9E37u + 0x5100u);
    left = c.x0 + c.pitch * float(i);
    mir = (h & 1u) != 0u;
    voff = hfloat(hash_combine(h, 1u));
    float r = hfloat(hash_combine(h, 2u));
    top = c.H - (c.n > 1 && (i == 0 || i == c.n - 1) ? 0.3 + 1.3 * r : 0.35 * r);
}
// The leaves' height at card point xy (0 where nothing grows): the highest of the columns over it; the atlas point
// it came from, and whether its column is mirrored.
float vineHeight(VineCard c, vec2 xy, vec2 gx, vec2 gy, out vec2 st, out bool mirOut) {
    int oi = PBR_VINE * PBR_MAPS_PER_MATERIAL + PBR_ORM;
    float best = 0.0;
    st = vec2(0.0);
    mirOut = false;
    int i0 = 0, i1 = 0;
    if (c.n > 1) {
        i0 = max(0, int(ceil((xy.x - c.cw - c.x0) / c.pitch)));
        i1 = min(min(c.n - 1, int(floor((xy.x - c.x0) / c.pitch))), i0 + 3);   // at most four over a point
    }
    for (int i = i0; i <= i1; ++i) {
        float left, voff, top;
        bool mir;
        vineColumn(c, i, left, mir, voff, top);
        float u = (xy.x - left) / c.cw;
        if (u <= 0.0 || u >= 1.0 || xy.y < 0.0 || xy.y > top + 0.05) continue;
        vec2 s = vec2((c.species + (mir ? 1.0 - u : u)) / VINE_CELLS, voff - xy.y / c.tileH);
        float h = textureGrad(pbrMaps[oi], s, gx, gy).b;
        if (h <= smoothstep(top - 0.7, top + 0.05, xy.y)) continue;   // the top: only its highest leaves
        if (h > best) { best = h; st = s; mirOut = mir; }
    }
    return best;
}

vec3 shadeVine(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, vec3 lit) {
    if (!pbrLoaded(PBR_VINE)) discard;
    vec4 k = pbrParams(PBR_VINE);
    VineCard c;
    c.W = max(lit.y, 0.05); c.H = max(lit.z, 0.05);
    c.seed = seed;
    c.species = float(min(prm.z & 255u, 5u));
    c.tileH = k.z * k.y;
    if (c.W < 1.3 * k.z) { c.n = 1; c.cw = c.W; c.x0 = 0.0; c.pitch = 1.0; }   // one column, stretched to the card
    else {
        c.cw = k.z;
        c.n = int(ceil((c.W - c.cw) / (0.5 * c.cw))) + 1;   // close enough that the columns read as one plant, not stripes
        c.x0 = 0.0;
        c.pitch = (c.W - c.cw) / float(c.n - 1);
    }
    // the card's frame: up the wall, across it (the way uv.x runs, mirrored rows or not)
    vec3 Bn = vec3(0.0, 1.0, 0.0);
    vec3 Tn = normalize(cross(Bn, n) + vec3(1e-6, 0.0, 0.0));
    vec3 dp1 = dFdx(rel), dp2 = dFdy(rel);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    float det = du1.x * du2.y - du1.y * du2.x;
    vec3 dpdu = (dp1 * du2.y - dp2 * du1.y) / (abs(det) > 1e-14 ? det : 1e-14);
    if (dot(dpdu, Tn) < 0.0) Tn = -Tn;
    vec2 xy = uv * vec2(c.W, c.H);
    vec2 xyx = dFdx(xy), xyy = dFdy(xy);
    vec2 g = vec2(1.0 / (c.cw * VINE_CELLS), -1.0 / c.tileH);
    vec2 gx = xyx * g, gy = xyy * g;
    float px = max(min(length(xyx), length(xyy)), 1e-5);   // metres a pixel spans, the finer way

    // the eye's ray down through the leaves to the wall
    vec3 V = -normalize(rel);
    float vn = max(dot(V, n), 0.0);
    vec2 dir = -vec2(dot(V, Tn), dot(V, Bn)) / max(vn, 0.17) * k.x;   // card metres per unit of depth
    float shiftPx = length(dir) / px;
    vec2 st;
    bool mir;
    float h = vineHeight(c, xy, gx, gy, st, mir);
    if (shiftPx > 1.0) {
        int layers = int(clamp(shiftPx * 0.5, 4.0, 16.0));
        float inc = 1.0 / float(layers), d = 0.0, gap = h - 1.0, prev = gap;
        vec2 q = xy;
        bool hit = gap >= 0.0;
        for (int s = 0; s < 16 && !hit && s < layers; ++s) {
            prev = gap;
            q += dir * inc; d += inc;
            h = vineHeight(c, q, gx, gy, st, mir);
            gap = h - (1.0 - d);
            hit = gap >= 0.0 && h > 0.02;
        }
        if (!hit) discard;   // down to the wall, nothing met
        if (d > 0.0) {       // back to where it crossed, if the leaves are there too
            vec2 st2;
            bool mir2;
            vec2 q2 = q - dir * inc * clamp(gap / max(gap - prev, 1e-5), 0.0, 1.0);
            float h2 = vineHeight(c, q2, gx, gy, st2, mir2);
            if (h2 > 0.02) { h = h2; st = st2; mir = mir2; }
        }
    } else if (h <= 0.02) discard;

    int base = PBR_VINE * PBR_MAPS_PER_MATERIAL;
    vec4 alb4 = textureGrad(pbrMaps[base + PBR_ALBEDO], st, gx, gy);
    // its share of the pixel's samples, dithered (as a plant's fence: industry.glsl shadeFence)
    const float BAYER[4] = float[4](0.0, 2.0, 3.0, 1.0);
    ivec2 f = ivec2(gl_FragCoord.xy) & 1;
    int kc = min(int(alb4.a * 4.0 + (BAYER[f.y * 2 + f.x] + 0.5) * 0.25), 4);
    if (kc == 0) discard;
    gl_SampleMask[0] = (1 << kc) - 1;

    vec3 T = mir ? -Tn : Tn;   // the column's image right
    vec3 tn = textureGrad(pbrMaps[base + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    vec3 nn = normalize(n + (T * tn.x + Bn * tn.y) / max(tn.z, 0.2));
    vec3 orm = textureGrad(pbrMaps[base + PBR_ORM], st, gx, gy).rgb;
    // its own shadows: from where the ray met the leaves towards each light
    gRelFade = 1.0 - smoothstep(k.x, 2.0 * k.x, px);
    gReliefReady = gRelFade > 0.0 && h < 0.98;
    gRelM = PBR_VINE; gRelSt = st; gRelSx = gx; gRelSy = gy;
    gRelScale = k.x * vec2(1.0 / (c.cw * VINE_CELLS), 1.0 / c.tileH);
    gRelT = T; gRelB = Bn; gRelN = n; gRelH = h;

    // the photograph's colours under the city's soot, grimier near the ground; darker and glossier wet
    float y = p.y - gBase;
    vec3 alb = alb4.rgb * k.w;
    alb = mix(vec3(dot(alb, vec3(0.2126, 0.7152, 0.0722))), alb, 0.85) * mix(0.75, 1.0, smoothstep(0.0, 1.5, y));
    float wet = WETNESS > 0.0 ? WETNESS * (1.0 - shelterAt(rel)) : 0.0;
    alb *= mix(1.0, 0.8, wet);
    float shin = mix(mix(24.0, 8.0, orm.g), 60.0, wet), specK = mix(mix(0.07, 0.02, orm.g), 0.25, wet);
    vec3 spec;
    reliefLightsOn();
    vec3 diff = wallLighting(p, nn, V, shin, specK, y, spec);
    reliefLightsOff();
    float ao = orm.r * mix(0.4, 1.0, lit.x);
    return alb * diff * ao + spec * ao;
}

/* HM5c1: whether a house's outside lights are on (its LEDs, downlights, lantern: one seed a unit, src/city/homes.c).
 * By the lamps' photocells (DN7: on under the cities' deck all day, out in daylight past its edge) and by the hour: most
 * houses keep a light on through the evening, fewer in the small hours. */
float houseLightOn(uint seed) {
    float day = 1.0 - smoothstep(0.25, 0.6, gLampDay);
    float share = clamp(0.35 + 0.5 * cityHours(true), 0.0, 0.9);
    return hfloat(hash_combine(seed, 0x11D8u)) < share ? day : 0.0;
}
vec3 shadeHouseKit(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, float occ, uint aux) {
    uint m = prm.y & 255u, fl = prm.w, part = prm.z;
    uint trim = (fl >> HOUSE_KIT_TRIM) & 7u, door = (fl >> HOUSE_KIT_DOOR) & 15u, brick = (fl >> HOUSE_KIT_BRICK) & 7u;
    uint stone = (fl >> HOUSE_KIT_STONE) & 3u, rain = (fl >> HOUSE_KIT_RAIN) & 3u;
    uint roofFin = (part >> HOUSE_KIT_ROOF) & 15u, roofCol = (part >> HOUSE_KIT_ROOFCOL) & 7u;
    bool old = ((fl >> HOUSE_KIT_OLD) & 1u) != 0u;
    float ao = mix(0.4, 1.0, occ);
    // the shells' own surfaces: trim, the roof's covering (a dormer's or a canopy's), hung clay tiles
    if (m == PMAT_HTRIM)  return shadeHouse(p, rel, n, uv, seed, uvec4(MAT_HOUSE, HF_TRIM, 0u, trim), 0u) * ao;
    if (m == PMAT_HROOF)  return shadeHouse(p, rel, n, uv, seed, uvec4(MAT_HOUSE, HF_ROOF, roofFin, roofCol), 0u) * ao;
    if (m == PMAT_HTILE)  return shadeHouse(p, rel, n, uv, seed, uvec4(MAT_HOUSE, HF_ROOF, HRM_CLAY, roofFin == HRM_CLAY ? roofCol : 1u), 0u) * ao;

    vec3 V = -normalize(rel);
    gLocalHouse = p;
    gHouseDist = length(rel);
    float h = p.y - gBase;
    float wet = WETNESS > 0.0 ? WETNESS * (1.0 - shelterAt(rel)) : 0.0;
    uint key = hash_combine(seed, aux * 0x9E37u + m);
    float vary = mix(0.9, 1.1, hf(key, 0x4B01u));
    if (m == PMAT_HGLASS) {   // the kit's own glass (dormers, fanlights, a door's lights): the shells' glass
        vec3 g = houseGlass(dot(n, V) < 0.0 ? -n : n, V, key, uv, vec2(1.0));
        return g;
    }
    if (m == PMAT_HLED || m == PMAT_HLAMP) {   // HM5c1: a strip's or a downlight's opal diffuser, a lantern's glass
        float on = houseLightOn(seed);
        float facing = abs(dot(n, V));
        vec3 warm = m == PMAT_HLED ? vec3(1.0, 0.80, 0.56) : vec3(1.0, 0.66, 0.36);
        vec3 glow = warm * (m == PMAT_HLED ? 5.0 + 2.0 * facing : 3.0 + 2.5 * facing);
        vec3 nv = dot(n, V) < 0.0 ? -n : n, spec;
        vec3 dark = vec3(0.30, 0.30, 0.28) * wallLighting(p, nv, V, 40.0, 0.06, h, spec) + spec;
        return mix(dark, glow, on);
    }
    vec3 T, B, nn = n, spec;
    vec2 q;
    planarFrame(n, gWorld, T, B, q);
    vec3 alb;
    int set = -1;
    float shin = 14.0, specK = 0.04, grainK = 1.0;
    if (m == PMAT_HSTONE)       { alb = H_STONE[stone] * 1.04; set = PBR_FACADE_STONE; }
    else if (m == PMAT_HBRICK)  { alb = houseWallColour(HW_BRICK, brick) * (old ? 0.85 : 1.0) * mix(0.85, 1.15, hf(key, 0x4B02u)); set = PBR_HOUSE_ROUGHCAST; grainK = 0.6; }
    else if (m == PMAT_HCEMENT) { alb = vec3(0.34, 0.33, 0.31); set = PBR_CONCRETE; }
    else if (m == PMAT_HCLAY) {   // terracotta pots and ridge tiles: red, buff or near brown, sooted
        uint t = uint(hf(key, 0x4B03u) * 3.0);
        alb = t == 0u ? vec3(0.40, 0.15, 0.07) : t == 1u ? vec3(0.46, 0.27, 0.13) : vec3(0.24, 0.11, 0.06);
        alb *= mix(0.45, 1.0, vnoise(gWorld.xz * 3.0 + gWorld.y * 2.0, 0x4B04u));
    }
    else if (m == PMAT_HRAIN) {   // gutters and pipes: black cast iron, white, grey or brown plastic, grimed
        alb = rain == HKR_IRON ? vec3(0.025) : rain == HKR_WHITE ? vec3(0.56, 0.56, 0.54) : rain == HKR_GREY ? vec3(0.20, 0.20, 0.21) : vec3(0.11, 0.065, 0.04);
        alb *= mix(0.7, 1.0, vnoise(vec2(dot(gWorld.xz, vec2(0.7, 0.7)) * 2.0, gWorld.y * 0.6), 0x4B05u));
        shin = 40.0; specK = 0.07;
    }
    else if (m == PMAT_HLEAD)   { alb = mix(vec3(0.17, 0.18, 0.19), vec3(0.36, 0.37, 0.36), smoothstep(0.55, 0.85, vnoise(gWorld.xz * 4.0 + gWorld.y, 0x4B06u))); shin = 20.0; specK = 0.06; }
    else if (m == PMAT_HIRON)   { alb = vec3(0.022, 0.022, 0.024); shin = 30.0; specK = 0.06; set = PBR_KIT_PAINT; grainK = 0.3; }
    else if (m == PMAT_HPLANT)  { alb = mix(vec3(0.03, 0.08, 0.02), vec3(0.09, 0.14, 0.04), hf(key, 0x4B07u)) * mix(0.6, 1.1, vnoise(gWorld.xz * 9.0 + gWorld.y * 7.0, 0x4B08u)); specK = 0.03; }
    else if (m == PMAT_HTIMBER) { alb = vec3(0.04, 0.028, 0.02); set = PBR_VENDOR_PLANKS; grainK = 0.5; }
    else if (m == PMAT_HUPVC)   { alb = vec3(0.60, 0.60, 0.57) * mix(0.9, 1.0, hf(seed, 0x4B09u)); shin = 40.0; specK = 0.06; }
    // HM5c1, the retrofit kit: galvanised or grey-painted steel (rust weeping from its joints), copper going brown with a
    // little verdigris, tank plastic (black, a few blue, green or cream), solar cells under glass, breeze blocks (bare
    // or limewashed), flowers
    else if (m == PMAT_HGALV) {
        float k = hf(key, 0x4C01u);
        alb = k < 0.55 ? vec3(0.30, 0.31, 0.31) : k < 0.85 ? vec3(0.07, 0.075, 0.08) : vec3(0.16, 0.17, 0.16);
        float rust = smoothstep(0.62, 0.9, vnoise(vec2(dot(gWorld.xz, vec2(0.8, 0.6)) * 3.0, gWorld.y * 1.2), 0x4C02u));
        alb = mix(alb, vec3(0.16, 0.07, 0.03), rust * 0.6);
        set = PBR_KIT_GALV; grainK = 0.5; shin = 35.0; specK = mix(0.12, 0.05, rust);
    }
    else if (m == PMAT_HCOPPER) {
        alb = mix(vec3(0.36, 0.17, 0.08), vec3(0.20, 0.09, 0.05), vnoise(gWorld.xz * 2.0 + gWorld.y * 1.5, 0x4C03u));
        alb = mix(alb, vec3(0.12, 0.26, 0.21), smoothstep(0.7, 0.92, vnoise(vec2(dot(gWorld.xz, vec2(0.6, 0.8)) * 4.0, gWorld.y * 3.0), 0x4C04u)) * 0.7);
        set = PBR_KIT_COPPER; grainK = 0.4; shin = 45.0; specK = 0.18;
    }
    else if (m == PMAT_HPOLY) {
        float k = hf(seed, 0x4C05u);
        alb = k < 0.84 ? vec3(0.018, 0.018, 0.02) : k < 0.92 ? vec3(0.02, 0.05, 0.13) : k < 0.97 ? vec3(0.03, 0.08, 0.04) : vec3(0.38, 0.36, 0.30);
        alb = mix(alb, vec3(0.16, 0.15, 0.13), smoothstep(0.4, 0.9, n.y) * 0.35 * vnoise(gWorld.xz * 5.0, 0x4C06u));   // dust on its shoulder
        shin = 30.0; specK = 0.08;
    }
    else if (m == PMAT_HSOLAR) {
        // cells about 0.16 m square in the panel's metres, silver lines between them; the glass reflects like glass
        vec2 c = abs(fract(uv / 0.163) - 0.5);
        float line = 1.0 - smoothstep(0.44, 0.47, max(c.x, c.y));
        alb = mix(vec3(0.012, 0.018, 0.04), vec3(0.012, 0.018, 0.04) * 0.8, vnoise(uv * 3.0, 0x4C07u));
        alb = mix(vec3(0.2, 0.2, 0.21), alb, line);
        alb = mix(alb, vec3(0.09, 0.085, 0.08), 0.35 * vnoise(gWorld.xz * 1.3, 0x4C08u));   // the city's grime on the glass
        shin = 220.0; specK = 0.45;
    }
    else if (m == PMAT_HBREEZE) {
        float k = hf(key, 0x4C09u);
        alb = k < 0.5 ? vec3(0.40, 0.38, 0.34) : k < 0.8 ? vec3(0.55, 0.52, 0.45) : vec3(0.48, 0.40, 0.30);
        set = PBR_CONCRETE; grainK = 0.8;
    }
    else if (m == PMAT_HBLOOM) {
        float k = hf(key, 0x4C0Au);
        alb = k < 0.55 ? vec3(0.42, 0.02, 0.18) : k < 0.72 ? vec3(0.55, 0.20, 0.02) : k < 0.86 ? vec3(0.45, 0.03, 0.04) : vec3(0.55, 0.52, 0.48);
        alb *= mix(0.55, 1.1, vnoise(gWorld.xz * 11.0 + gWorld.y * 9.0, 0x4C0Bu));
        specK = 0.03;
    }
    else                        { alb = H_DOOR[door % 12u]; set = PBR_KIT_PAINT; grainK = 0.5; shin = 30.0; specK = 0.1; }   // PMAT_HDOOR
    alb *= vary;
    if (set >= 0 && pbrLoaded(set)) {
        Grain g = pbrGrain(set, q, dFdx(q), dFdy(q), T, B, n);
        alb *= mix(vec3(1.0), g.tint * g.ao, grainK);
        nn = normalize(mix(n, g.n, grainK));
    }
    // the city's grime: in the module's corners and down what faces up, more on the stone and brick
    alb *= mix(0.75, 1.0, occ) * mix(0.85, 1.0, smoothstep(0.0, 1.2, h));
    alb *= mix(1.0, 0.75, wet);
    shin = mix(shin, 60.0, wet); specK = mix(specK, 0.3, wet);
    vec3 diff = wallLighting(p, nn, V, shin, specK, h, spec);
    return alb * diff * ao + spec;
}
