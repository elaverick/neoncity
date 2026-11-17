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
    float storeys = float((aux >> HOUSE_AUX_STOREYS) & 3u);
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
        // the window's own key, from its lower left corner (an opening's u runs along (-n.z, n.x): src/city/homes.c)
        vec3 c = gWorld - uv.x * vec3(-n.z, 0.0, n.x) - vec3(0.0, uv.y, 0.0);
        uint key = hash3i(seed ^ 0x6A55u, int(round(c.x * 4.0)), int(round(c.y * 4.0)), int(round(c.z * 4.0)));
        vec3 glass = houseGlass(n, V, key, uv, s);
        vec3 diff = wallLighting(p, n, V, 30.0, 0.15, h, spec);
        vec3 frame = H_TRIM[col & 7u] * 0.9 * diff + spec;
        gRefl.w *= 1.0 - bars;
        return mix(glass, frame, bars);
    }

    if (surf == HF_ROOF) {
        alb = houseRoofColour(fin, col) * vary;
        vec3 T = normalize(vec3(n.z, 0.0, -n.x)), B = normalize(cross(n, T));
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
        if (surf == HF_GARAGE) {
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
