// Part of shaders/city.frag, which includes the parts in order: office lobbies: finishes, lighting, emblems, plants.

/* ======================================================================
 * Interiors (emit_interior in src/city/mesh_lobby.c): real lobby geometry behind the
 * ground-floor glass of office buildings, lit by the ceiling's light panels
 * in the building's lobby colour temperature, as the glass outside shows it.
 * params y surface, z variety (a region or fixture type), w ceiling height
 * above the floor in half metres. Floors and ceilings carry the building's
 * frame angle in aux, so their uv (building metres) can be followed.
 * ==================================================================== */

// Real glass over real geometry: with MSAA the pane covers only some of the
// pixel's samples (always sample 0, so depth and reflections see the pane)
// and the rest show what stands behind it; more of the pane at grazing
// angles, where it reflects more. Returns the covered share. Without MSAA
// the pane vanishes and the view through is clear.
float paneCoverage(float F) {
    int k = F < 0.25 ? 1 : F < 0.5 ? 2 : 3;
    gl_SampleMask[0] = (1 << k) - 1;
    return float(k) * 0.25;
}

const uint IS_FLOOR = 0u, IS_ROOMFLOOR = 1u, IS_WALL = 2u, IS_CEILING = 3u, IS_CORE = 4u, IS_METAL = 5u, IS_WOOD = 6u,
           IS_FABRIC = 7u, IS_GLASS = 8u, IS_LIGHT = 9u, IS_PLANT = 10u, IS_LINING = 11u, IS_STONE = 12u, IS_LIFTDOOR = 13u,
           IS_DOOR = 14u, IS_SCREEN = 15u, IS_SKYLIGHT = 16u, IS_OFFICE = 17u, IS_SIGN = 18u, IS_EMBLEM = 19u;
// region types (IR_* in src/gen/interior.h) that change a floor's finish
const uint IR_VESTIBULE = 1u, IR_HALL = 2u, IR_SECURE = 3u, IR_TOILET = 7u, IR_WC = 8u, IR_SECURITY = 10u, IR_MAIL = 11u,
           IR_FACILITIES = 12u, IR_MEETING = 14u, IR_SHOWER = 15u;

/* Lobby finishes. Each building picks a dark, warm palette (styleHash):
 * polished stone underfoot (a black and white chequer, dark granite, veined
 * dark marble or warm limestone), walls of dark marble, smoked bronze, walnut
 * or charcoal plaster, and cores clad in dark stone. The marbles, limestone
 * and woods are authored textures (finish), with the procedural ones as the
 * fallback. Warm panels in a dark ceiling light it low, and light is built
 * into the walls: a slot at the ceiling washes down them, coves glow at their
 * feet and slots run up the cores. */

vec3 lobbyLight(uint seed) {
    return (hf(styleHash(seed), 8u) < 0.5 ? kelvin(3000.0) : kelvin(2700.0)) * 1.35;   // as the lobby glass (shadeFacade)
}

// The share of the panels' light still reaching a surface in their full shadow (under a desk):
// what the lit floor, walls and ceiling throw back.
const float PANEL_FILL = 0.2;

// The light in the walls' slots and coves: warmer than the panels.
const vec3 COVE = vec3(1.0, 0.55, 0.22);

// A lobby's accent light outside (lobby_accent in src/city/city.c): its mullion lines and canopy strip.
vec3 lobbyAccent(uint seed) {
    float a = hf(styleHash(seed), 61u);
    return neonColor(a < 0.55 ? 1u : a < 0.7 ? 6u : a < 0.85 ? 4u : 3u);
}

// The ceiling's light fittings (src/gen/interior_lights.c): 1.2 m squares on
// grids FIT_PITCH apart in the building's frame, kept where they fit in the
// room. A floor knows only the open floor's grid (its aux: fitting centres at
// off + k * FIT_PITCH), so its mirror of them is the whole grid's.
const float FIT_PITCH = 3.6;
float ceilingPanel(vec2 q, vec2 off, float aa) {
    vec2 d = abs(fract((q - off) / FIT_PITCH + 0.5) - 0.5) * FIT_PITCH;
    return (1.0 - smoothstep(0.6 - aa, 0.6 + aa, d.x)) * (1.0 - smoothstep(0.6 - aa, 0.6 + aa, d.y));
}

/* The fittings, where their authored textures are loaded: a recessed troffer
 * of three tubes (PBR_LIGHT_FIXTURE) behind a prismatic diffuser
 * (PBR_LIGHT_DIFFUSER), one image per fitting. The troffer's tray is steep
 * parallax mapped from its height map, so the tubes and their tray shift
 * behind the frame as the viewer moves. The diffuser lies flat in the frame's
 * opening and stays put, letting part of what is behind it through and
 * glowing with the tubes' light scattered in it. */
const float FIXTURE_DEPTH = 0.16;      // metres from the frame's face (the ceiling's plane) down to height 0
const float DIFFUSER_OPACITY = 0.7;
// The height map's frame face: at least 0.79 all round, and nothing inside the
// opening above 0.70. The frame lies in the ceiling's plane, so a ray that meets
// it stops there; were it below the plane, rays would slide over its inner
// edge onto the tubes, which then showed through the frame, undiffused.
const float FIXTURE_FRAME = 0.8;

// The troffer's height at st in its panel, 1 at the frame and the ceiling round it.
float fixtureHeight(vec2 st, vec2 gx, vec2 gy) {
    if (any(lessThan(st, vec2(0.0))) || any(greaterThan(st, vec2(1.0)))) return 1.0;
    return min(textureGrad(pbrMaps[PBR_LIGHT_FIXTURE * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, gx, gy).b / FIXTURE_FRAME, 1.0);
}

// Steep parallax: steps the view ray into the troffer in even layers of depth
// until it passes under the height field, then puts the hit between the last
// two layers by where the ray crossed it. dir: st's shift along the ray per
// unit of depth (height 1 -> 0).
vec2 fixtureParallax(vec2 st, vec2 dir, float layers, vec2 gx, vec2 gy) {
    float inc = 1.0 / layers, depth = 0.0;
    float gap = 1.0 - fixtureHeight(st, gx, gy), prev = gap;   // how far the height field lies below the ray
    if (gap <= 0.0) return st;
    vec2 q = st;
    for (int i = 0; i < 64; ++i) {
        prev = gap;
        q += dir * inc;
        depth += inc;
        gap = 1.0 - fixtureHeight(q, gx, gy) - depth;
        if (gap <= 0.0) break;
    }
    return q - dir * inc * (-gap / (prev - gap));
}

// A fitting's radiance at st (0..1 across it, along the building's x and z):
// gx and gy st's screen derivatives, V toward the eye, Lc the lobby's light,
// diff the room's light on the ceiling.
vec3 ceilingFixture(vec2 st, vec2 gx, vec2 gy, vec3 V, mat2 toFrame, vec3 Lc, vec3 diff) {
    const int F = PBR_LIGHT_FIXTURE * PBR_MAPS_PER_MATERIAL, D = PBR_LIGHT_DIFFUSER * PBR_MAPS_PER_MATERIAL;
    // the image's right and down in the world (the ceiling faces down)
    vec2 rx = vec2(1.0, 0.0) * toFrame, dz = vec2(0.0, 1.0) * toFrame;
    vec3 T = vec3(rx.x, 0.0, rx.y), Bd = vec3(dz.x, 0.0, dz.y), N = vec3(0.0, -1.0, 0.0);

    // the tray, as the ray into it finds it: shallow looks need more layers
    vec3 r = -V;
    float up = max(r.y, 0.12);
    vec2 dir = toFrame * (r.xz / up) * (FIXTURE_DEPTH / 1.2);
    vec2 sp = clamp(fixtureParallax(st, dir, mix(64.0, 16.0, up), gx, gy), 0.0, 1.0);
    vec3 alb = textureGrad(pbrMaps[F + PBR_ALBEDO], sp, gx, gy).rgb;
    vec3 orm = textureGrad(pbrMaps[F + PBR_ORM], sp, gx, gy).rgb;
    float tube = textureGrad(pbrMaps[F + PBR_EMISSIVE], sp, gx, gy).r;
    vec3 tn = textureGrad(pbrMaps[F + PBR_NORMAL], sp, gx, gy).xyz * 2.0 - 1.0;
    vec3 nf = normalize(T * tn.x - Bd * tn.y + N * tn.z);
    float tray = 1.0 - smoothstep(0.72, 0.78, orm.b);           // below the frame: lit by the tubes
    vec3 fix = alb * (diff * (0.6 + 0.4 * max(-nf.y, 0.0)) + Lc * 0.9 * orm.r * tray) + Lc * 6.0 * tube;

    // the diffuser, across the frame's opening where the eye meets the ceiling
    float opening = 1.0 - smoothstep(0.72, 0.78, fixtureHeight(st, gx, gy) * FIXTURE_FRAME);
    vec3 dAlb = textureGrad(pbrMaps[D + PBR_ALBEDO], st, gx, gy).rgb;
    vec3 dn = textureGrad(pbrMaps[D + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    float dAo = textureGrad(pbrMaps[D + PBR_ORM], st, gx, gy).r;
    vec3 nd = normalize(T * dn.x - Bd * dn.y + N * dn.z);
    // the tubes' light spread through the sheet (the emissive map's broad mips), and its prisms
    // catching it as they turn to the eye
    float spread = textureLod(pbrMaps[F + PBR_EMISSIVE], st, 5.5).r * 0.7 + textureLod(pbrMaps[F + PBR_EMISSIVE], st, 9.0).r * 0.6;
    float prism = mix(0.7, 1.3, smoothstep(0.85, 1.0, dot(nd, V)));
    vec3 sheet = dAlb * (diff + Lc * 3.5 * spread * prism * dAo);
    return mix(fix, sheet, DIFFUSER_OPACITY * opening);
}

// Polished stone (q in metres): a base broken by soft clouding and thin wandering veins.
vec3 marble(vec2 q, vec3 base, vec3 vein, uint salt, float aa) {
    float cloud = fbm(q * 0.4, salt);
    float s = abs(sin(q.x * 0.9 + q.y * 1.6 + fbm(q * 0.5 + 7.0, salt + 3u) * 3.0));
    float v = (1.0 - smoothstep(0.0, 0.02 + aa * 2.0, s)) * smoothstep(0.35, 0.75, fbm(q * 1.3, salt + 9u));
    v *= 1.0 - smoothstep(0.02, 0.12, aa);   // veins finer than a pixel fade out
    return mix(base * (0.75 + 0.5 * cloud), vein, v * 0.4);
}

/* Authored stone and wood (src/render/textures.c, assets/textures/): m a PBR_*
 * material whose image spans `size` metres of q, shifted by off (each slab or
 * panel its own piece of the stone) and turned a quarter if `turn`. gx, gy:
 * q's screen derivatives, taken where q is continuous, so a slab's offset
 * doesn't upset the filtering at its joints. rough: the map's roughness. */
vec3 finish(int m, vec2 q, float size, vec2 off, bool turn, vec2 gx, vec2 gy, out float rough) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    if (turn) { q = q.yx; gx = gx.yx; gy = gy.yx; }
    vec2 st = q / size + off;
    gx /= size; gy /= size;
    rough = textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).g;
    return textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb;
}

// How glossy a polished finish is where its map says it is rough (worn, honed or open-grained).
float polishOf(float rough) { return 1.0 - smoothstep(0.05, 0.7, rough); }

// A piece of stone per cell: an offset into the image.
vec2 slabOffset(uint h, vec2 cell) { return vec2(hfloat(hash3i(h, int(cell.x), int(cell.y), 11)), hfloat(hash3i(h, int(cell.x), int(cell.y), 12))); }

// Book-matched panels w metres wide: each pair cut from one block, the second
// mirrored, so the veins meet at the joint between them. Returns the panel's
// q (mirrored where it should be) and its pair's index in `pair`.
vec2 bookMatch(vec2 q, float w, out float pair) {
    float k = floor(q.x / w), x = q.x - k * w;
    pair = floor(k * 0.5);
    return vec2(mod(k, 2.0) < 0.5 ? x : 2.0 * w - x, q.y);
}

// The lobby floor: 1.2 m slabs. polish: how glossy the stone is between its joints.
vec3 lobbyStone(uint seed, vec2 q, vec2 gx, vec2 gy, float aa, out float joint, out float polish) {
    uint sh = styleHash(seed), salt = sh & 0xFFFFu;
    float pick = hf(sh, 21u);
    vec2 cell = floor(q / 1.2), t = fract(q / 1.2);
    vec2 off = slabOffset(sh, cell);
    bool turn = hfloat(hash3i(sh, int(cell.x), int(cell.y), 13)) < 0.5;
    vec3 c;
    float rough;
    polish = 0.75;
    float w = 0.004 + aa;
    joint = max(1.0 - smoothstep(0.0, w, min(t.x, 1.0 - t.x) * 1.2), 1.0 - smoothstep(0.0, w, min(t.y, 1.0 - t.y) * 1.2));
    if (pick < 0.35) {                // a chequer of black and white marble
        bool dark = mod(cell.x + cell.y, 2.0) > 0.5;
        if (dark && pbrLoaded(PBR_MARBLE_DARK)) {
            c = finish(PBR_MARBLE_DARK, q, 2.0, off, turn, gx, gy, rough) * 0.8;
            polish *= polishOf(rough);
        } else if (!dark && pbrLoaded(PBR_MARBLE_WHITE)) {
            c = finish(PBR_MARBLE_WHITE, q, 2.0, off, turn, gx, gy, rough) * 0.5;
            polish *= polishOf(rough);
        } else {
            c = dark ? marble(q, vec3(0.028, 0.028, 0.03), vec3(0.20, 0.19, 0.18), salt, aa)
                     : marble(q, vec3(0.22, 0.215, 0.20), vec3(0.09, 0.085, 0.08), salt + 5u, aa);
        }
    } else if (pick < 0.6) {          // dark granite
        float speck = mix(vnoise(q * 40.0, salt), 0.5, smoothstep(0.005, 0.03, aa));
        c = vec3(0.045, 0.043, 0.042) * (0.6 + 0.8 * speck);
    } else if (pick < 0.8) {          // veined dark marble
        if (pbrLoaded(PBR_MARBLE_DARK)) {
            c = finish(PBR_MARBLE_DARK, q, 2.0, off, turn, gx, gy, rough);
            polish *= polishOf(rough);
        } else {
            c = marble(q, vec3(0.05, 0.045, 0.04), vec3(0.32, 0.27, 0.20), salt, aa);
        }
    } else {                          // warm limestone, honed: 0.6 m tiles with their own joints
        polish = 0.4;
        if (pbrLoaded(PBR_LIMESTONE)) {
            joint = 0.0;
            c = finish(PBR_LIMESTONE, q, 3.6, vec2(0.0), false, gx, gy, rough) * 0.75;
            polish *= polishOf(rough * 0.6);
            return c;
        }
        c = vec3(0.30, 0.27, 0.22) * (0.85 + 0.3 * fbm(q * 0.9, salt));
    }
    c *= 0.92 + 0.16 * hfloat(hash3i(sh, int(cell.x), int(cell.y), 5));
    return c;
}

// The lobby's walls (q = metres along, height): the building's finish.
vec3 lobbyWall(uint seed, vec2 q, vec2 gx, vec2 gy, float aa, out float gloss) {
    uint sh = styleHash(seed), salt = (sh >> 8) & 0xFFFFu;
    float pick = hf(sh, 64u), rough;
    if (pick < 0.35) {                // dark marble, book-matched in 1.2 m panels
        gloss = 0.5;
        if (pbrLoaded(PBR_MARBLE_DARK)) {
            float pair, gap = 1.0 - band(fract(q.x / 1.2), 0.003, 0.997, aa / 1.2 + 0.001);
            vec3 c = finish(PBR_MARBLE_DARK, bookMatch(q, 1.2, pair), 2.4, slabOffset(sh, vec2(pair, 7.0)), false, gx, gy, rough);
            gloss *= polishOf(rough);
            return c * (1.0 - 0.7 * gap);
        }
        return marble(q, vec3(0.05, 0.047, 0.045), vec3(0.30, 0.28, 0.26), salt, aa);
    }
    if (pick < 0.6) {                 // smoked bronze panels with shadow gaps
        gloss = 0.45;
        float gap = 1.0 - band(fract(q.x / 1.2), 0.008, 0.992, aa / 1.2 + 0.002);
        return vec3(0.075, 0.052, 0.032) * (0.85 + 0.3 * vnoise(vec2(q.x * 0.3, q.y * 6.0), salt)) * (1.0 - 0.8 * gap);
    }
    if (pick < 0.8) {                 // walnut slats, the grain running up them
        gloss = 0.2;
        float slat = mix(band(fract(q.x / 0.12), 0.12, 0.88, aa / 0.12 + 0.01), 0.76, smoothstep(0.3, 0.8, aa / 0.12));
        vec3 c = pbrLoaded(PBR_WOOD_MID)
               ? finish(PBR_WOOD_MID, q, 1.5, vec2(0.0, floor(q.x / 0.12) * 0.37), true, gx, gy, rough) * 1.2
               : vec3(0.10, 0.055, 0.03) * (0.8 + 0.3 * sin(q.y * 9.0 + vnoise(q * vec2(8.0, 0.5), salt) * 5.0));
        return c * mix(0.25, 1.0, slat);
    }
    gloss = 0.1;                      // charcoal plaster
    return vec3(0.06, 0.058, 0.055) * (0.85 + 0.3 * fbm(q * 0.8, salt));
}

// Light washing down a wall from the slot where it meets the ceiling:
// brightest just under the ceiling, and even along the wall.
float wallWash(float y, float ceilH) {
    float dy = ceilH - y;
    return smoothstep(0.0, 0.35, dy) * exp(-dy * 0.5);
}

// A vertical light slot dx metres away (y0..y1 high): its glow, and the light it grazes the wall beside it with.
void lightSlot(float dx, float y, float y0, float y1, float aa, inout vec3 emit, inout vec3 irr) {
    float w = max(0.015, aa);
    float vert = band(y, y0, y1, aa + 0.005);
    emit += COVE * 5.0 * (1.0 - smoothstep(w * 0.5, w, dx)) * (0.015 / w) * vert;
    irr += COVE * 1.4 * exp(-dx / 0.12) * smoothstep(y0 - 0.3, y0 + 0.2, y) * smoothstep(y1 + 0.3, y1 - 0.2, y);
}

// A lit cove at the foot of a wall, and the light it washes up the wall.
void footCove(float y, float aa, inout vec3 emit, inout vec3 irr) {
    emit += COVE * 3.0 * (1.0 - smoothstep(0.035, 0.035 + aa, y));
    irr += COVE * 1.1 * exp(-y / 0.35);
}

// Sharp-cornered equilateral triangle of circumradius-ish r (Quilez).
float sdTriangle(vec2 p, float r) {
    const float k = 1.7320508;
    p.x = abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) * 0.5;
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}

/* Procedural emblems: a small grammar for corporate marks; h makes every choice.
 *   mark  := motif | motif in a frame
 *   motif := bars      2-4 sheared bars, level or rising like a chart
 *          | chevrons  1-3 nested, pointing up or down
 *          | ring      cut by a gap, with a bar or a dot inside
 *          | diamonds  2-4 turned squares in a row or a cluster, solid or hollow
 *          | monogram  a pseudo-kanji glyph, as on the signs
 *          | delta     a triangle cut by a slit or hollowed by an inner one
 *   frame := none | circle | square
 * q in [-1, 1]^2, y up. Returns the signed distance (negative inside) in q units. */
float emblemDist(uint h, vec2 q) {
    uint frame = (h >> 24) % 3u, kind = h % 6u;
    float sc = frame == 0u ? 1.0 : 0.62;
    vec2 p = q / sc;
    float d = 1e9;
    if (kind == 0u) {
        int n = 2 + int(hf(h, 1u) * 3.0);
        float shear = 0.2 + 0.5 * hf(h, 2u), pitch = 1.7 / float(n);
        bool rise = hf(h, 3u) < 0.6;
        p *= 1.0 + 0.6 * shear;                     // room for the lean
        for (int i = 0; i < 4; ++i) {
            if (i >= n) break;
            float ht = rise ? 0.35 + 0.65 * float(i + 1) / float(n) : 1.0;
            vec2 b = p - vec2(-0.85 + pitch * (float(i) + 0.5), -0.85 + 0.85 * ht);
            d = min(d, sdBox(vec2(b.x - shear * b.y, b.y), vec2(pitch * 0.3, 0.85 * ht)));
        }
        d /= 1.0 + 0.6 * shear;
    } else if (kind == 1u) {
        int n = 1 + int(hf(h, 1u) * 3.0);
        float t = 0.07 + 0.06 * hf(h, 2u);
        vec2 pp = vec2(abs(p.x), p.y * (hf(h, 3u) < 0.5 ? 1.0 : -1.0));
        for (int i = 0; i < 3; ++i) {
            if (i >= n) break;
            float y0 = 0.3 + 0.42 * (float(i) - 0.5 * float(n - 1));
            d = min(d, segDist(pp, vec2(0.0, y0), vec2(0.8, y0 - 0.6)) - t);
        }
    } else if (kind == 2u) {
        float t = 0.08 + 0.05 * hf(h, 2u), a = hf(h, 3u) * 6.2831853;
        vec2 r = mat2(cos(a), sin(a), -sin(a), cos(a)) * p;
        d = max(abs(length(p) - 0.75) - t, -sdBox(r - vec2(0.75, 0.0), vec2(0.3, 0.12 + 0.2 * hf(h, 4u))));
        d = min(d, hf(h, 5u) < 0.5 ? length(p) - 0.22 : sdBox(r, vec2(0.5, t)));
    } else if (kind == 3u) {
        int n = 2 + int(hf(h, 1u) * 3.0);
        bool hollow = hf(h, 2u) < 0.4, row = hf(h, 3u) < 0.5;
        float s = row ? 0.75 / float(n) + 0.12 : 0.3;
        for (int i = 0; i < 4; ++i) {
            if (i >= n) break;
            vec2 c = row ? vec2(-0.8 + 1.6 * (float(i) + 0.5) / float(n), 0.0)
                         : vec2(i == 1 ? -0.42 : i == 2 ? 0.42 : 0.0, i == 0 ? 0.42 : i == 3 ? -0.42 : 0.0);
            vec2 b = (p - c) * 0.7071;
            float dd = sdBox(vec2(b.x + b.y, b.y - b.x), vec2(s * 0.7071));
            d = min(d, hollow ? abs(dd) - 0.05 : dd);
        }
    } else if (kind == 4u) {
        d = glyphDist(h, clamp(p * 0.5 + 0.5, 0.0, 1.0)) * 2.0 - 0.09;
    } else {
        d = sdTriangle(p - vec2(0.0, -0.15), 0.95);
        d = hf(h, 1u) < 0.5 ? max(d, -sdBox(p - vec2(0.0, -0.2), vec2(0.06, 0.65)))
                            : max(d, -sdTriangle(p - vec2(0.0, -0.32), 0.4));
    }
    d *= sc;
    if (frame == 1u) d = min(d, abs(length(q) - 0.92) - 0.06);
    else if (frame == 2u) d = min(d, abs(sdBox(q, vec2(0.86))) - 0.06);
    return d;
}

/* Lobby plants (src/gen/plant.c, grow_plants in src/city/mesh_planting.c). Leaves (IS_PLANT)
 * carry their own surface coordinates (see PlantVertex in src/gen/plant.h) and
 * are textured with assets/textures/leaf, tinted to each species' green;
 * stems (IS_BARK) theirs, (around 0..1, along in metres). params z is the
 * leaf shape or stem material (LS_* / WM_* in src/gen/plant.h); aux the organ's
 * tint | its hash << 8 | species << 24; vLitter.x how open the crown is there.
 * A leaf is a single surface (drawn unculled); its normal is the upper side,
 * so `under` says the viewer sees its paler underside. They are lit as the lobby is: the warm panels overhead
 * and a little bounce, the panels' sheen on glossy leaves, and light through
 * the thin blades seen from below. The pots' top dressing is IS_SOIL. */
const uint IS_BARK = 20u, IS_SOIL = 21u;
const uint LS_KENTIA = 0u, LS_LINEAR = 1u, LS_FIDDLE = 2u, LS_ELLIPTIC = 3u, LS_UMBRELLA = 4u, LS_ZZ = 5u;
const uint WM_PALM = 0u, WM_CANE = 1u, WM_FICUS = 2u, WM_UMBRELLA = 3u, WM_GREEN = 4u, WM_ZZ = 5u, WM_ROOT = 6u;

// The leaf texture (assets/textures/leaf): where its midrib lies across the image, and its mean albedo.
const float LEAF_MIDRIB = 0.513;
const vec3 LEAF_MEAN = vec3(0.100, 0.166, 0.047);

// The dracaena blade's width along it (the linear profile in src/gen/plant.c), as a fraction of its widest.
float linearWidth(float t) {
    return t < 0.06 ? mix(0.55, 0.9, t / 0.06) : t < 0.25 ? mix(0.9, 1.0, (t - 0.06) / 0.19)
         : t < 0.6 ? mix(1.0, 0.85, (t - 0.25) / 0.35) : t < 0.85 ? mix(0.85, 0.45, (t - 0.6) / 0.25) : mix(0.45, 0.0, (t - 0.85) / 0.15);
}

vec3 shadePlant(vec3 p, vec3 V, vec3 n, vec2 uv, uint seed, uint surf, uint kind, float y, bool under, float panels) {
    vec3 Lc = lobbyLight(seed) * panels;
    float open = clamp(vLitter.x, 0.0, 1.0), occ = mix(0.3, 1.0, open);
    float tint = (float(vAux & 255u) - 128.0) / 127.0;
    uint org = (vAux >> 8) & 0x7FFFu;
    vec2 fw = fwidth(uv);
    vec3 alb;
    float gloss, trans = 0.0, shin = 30.0;
    if (surf == IS_PLANT) {
        // uv: across as a fraction of the blade's widest half-width; along, t + 2 * its length / width in quarters
        float A = floor(uv.y * 0.5), t = uv.y - 2.0 * A, aspect = max(A * 0.25, 0.25), s = uv.x;
        float u = kind == LS_LINEAR ? s / max(linearWidth(t), 0.05) : s, au = abs(u), aa = max(fw.x, 1e-3);
        vec3 base;
        if (kind == LS_KENTIA)        { base = vec3(0.045, 0.10, 0.032);  gloss = 0.25; trans = 0.35; }
        else if (kind == LS_LINEAR)   { base = vec3(0.03, 0.07, 0.03);    gloss = 0.4;  trans = 0.3; }
        else if (kind == LS_FIDDLE)   { base = vec3(0.03, 0.075, 0.022);  gloss = 0.55; trans = 0.15; }
        else if (kind == LS_ELLIPTIC) { base = vec3(0.035, 0.09, 0.028);  gloss = 0.6;  trans = 0.25; shin = 50.0; }
        else if (kind == LS_UMBRELLA) { base = vec3(0.05, 0.12, 0.03);    gloss = 0.55; trans = 0.25; shin = 45.0; }
        else                          { base = vec3(0.02, 0.06, 0.022);   gloss = 0.85; trans = 0.08; shin = 70.0; }   // zz
        if (pbrLoaded(PBR_LEAF)) {
            /* The leaf texture tiles along its midrib but not across it, so the
             * midrib runs down the blade's middle, the image's width spans at
             * most the blade's width, and it repeats only along the blade, at
             * the same scale both ways. Narrow blades take a strip either side
             * of the midrib; each blade starts at its own place along it. */
            float k = clamp(2.0 / aspect, 0.22, 0.95);
            vec2 st = vec2(LEAF_MIDRIB + 0.5 * k * s, hfloat(hash_combine(org, 5u)) * 4.0 - t * aspect * k);
            vec3 dp1 = dFdx(p), dp2 = dFdy(p), q1 = cross(n, dp1), q2 = cross(dp2, n);
            vec2 d1 = dFdx(st), d2 = dFdy(st);
            vec3 T = q2 * d1.x + q1 * d2.x, B = q2 * d1.y + q1 * d2.y;   // the image's right and down
            float im = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
            Surface sf = samplePBR(PBR_LEAF, st, T * im, -B * im, n);
            alb = sf.albedo / LEAF_MEAN * base;                         // the species' own green
            n = sf.n;
            gloss *= clamp((1.0 - sf.rough) / 0.75, 0.3, 1.3);
            occ *= mix(1.0, sf.ao, 0.7);
        } else {
            alb = mix(base, base * 1.8, (1.0 - smoothstep(0.0, 0.05 + aa, au)) * 0.5);   // a paler midrib
        }
        if (kind == LS_LINEAR) alb = mix(alb, vec3(0.13, 0.02, 0.035), smoothstep(0.8, 0.92, au));   // dracaena: red margins
        // the odd dry, browned tip on the long narrow leaves
        if ((kind == LS_KENTIA || kind == LS_LINEAR) && hfloat(hash_combine(uint(org), 3u)) < 0.25)
            alb = mix(alb, vec3(0.12, 0.08, 0.04), smoothstep(0.9, 1.0, t));
        alb *= 1.6 + 0.24 * tint;
        alb.r *= 1.0 + 0.25 * tint;
        if (under) { alb = alb * 1.3 + vec3(0.01, 0.015, 0.005); gloss *= 0.3; }
    } else if (surf == IS_BARK) {
        float aa = max(fw.y, 1e-3);
        gloss = 0.12;
        if (kind == WM_PALM) {                   // ringed with old frond scars
            float r = fract(uv.y / 0.045);
            alb = vec3(0.17, 0.17, 0.12) * (1.0 - 0.45 * (1.0 - smoothstep(0.0, 0.08 + aa / 0.045, min(r, 1.0 - r))));
        } else if (kind == WM_CANE) {            // leaf scars close up the cane
            float r = fract(uv.y / 0.014 + hfloat(uint(org)));
            alb = mix(vec3(0.13, 0.10, 0.075), vec3(0.22, 0.19, 0.15), (1.0 - smoothstep(0.0, 0.15 + aa / 0.014, min(r, 1.0 - r))) * (1.0 - smoothstep(0.3, 1.0, aa / 0.014)));
            gloss = 0.15;
        } else if (kind == WM_FICUS) {           // smooth grey bark flecked with lenticels
            alb = vec3(0.20, 0.18, 0.15) * (0.85 + 0.3 * vnoise(vec2(uv.x * 6.0, uv.y * 20.0), 0x7A11u));
        } else if (kind == WM_UMBRELLA) {
            alb = vec3(0.22, 0.18, 0.13) * (0.9 + 0.2 * vnoise(vec2(uv.x * 4.0, uv.y * 12.0), 0x7A12u));
        } else if (kind == WM_GREEN) {           // petioles and rachises
            alb = vec3(0.06, 0.12, 0.035); gloss = 0.3;
        } else if (kind == WM_ZZ) {              // fleshy, glossy, mottled dark
            float mo = smoothstep(0.55, 0.75, vnoise(vec2(uv.x * 8.0, uv.y * 30.0), 0x7A13u));
            alb = mix(vec3(0.045, 0.10, 0.03), vec3(0.015, 0.03, 0.012), mo * 0.7); gloss = 0.6; shin = 50.0;
        } else {
            alb = vec3(0.14, 0.10, 0.07);
        }
    } else {                                     // the pot's top dressing: polished black pebbles, or bark
        occ = 1.0;
        uint sh = styleHash(seed);
        if (hf(sh, 81u) < 0.6) {
            vec2 q = uv / 0.03, c = floor(q), f = fract(q) - 0.5;
            float k = hfloat(hash2i(sh, int(c.x), int(c.y)));
            float d = length(f + (vec2(k, fract(k * 7.3)) - 0.5) * 0.3);
            float stone = 1.0 - smoothstep(0.32, 0.46, d);
            alb = mix(vec3(0.004), vec3(0.03, 0.03, 0.033) * (0.6 + 0.8 * k), stone);
            gloss = 0.6 * stone; shin = 60.0;
            n = normalize(n + vec3(f.x, 0.0, f.y) * stone * 1.2);
        } else {
            alb = vec3(0.035, 0.024, 0.016) * (0.4 + fbm(uv * 40.0, 0x5017u));
            gloss = 0.03;
        }
    }
    float up = n.y * 0.5 + 0.5;
    float E = mix(0.12, 0.62, up * up) * mix(0.65, 1.0, smoothstep(0.0, 0.7, y)) * mix(1.0, 0.75, smoothstep(5.0, 14.0, y));
    vec3 col = alb * (Lc * E + ambientLight(n, p.y - gBase) * 0.5) * occ;
    col += alb * vec3(1.1, 1.3, 0.6) * Lc * 0.35 * trans * occ * smoothstep(0.0, 0.8, -n.y);   // through the blade
    vec3 Hh = normalize(vec3(0.0, 1.0, 0.0) + V);
    float spec = pow(max(dot(n, Hh), 0.0), shin) * (shin + 2.0) / 32.0;
    float F = 0.04 + 0.96 * pow(1.0 - max(dot(n, V), 0.0), 5.0);
    col += Lc * (spec * 0.1 + 0.05 * F) * gloss * occ;
    return col;
}

vec3 shadeInterior(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    bool flipped = dot(n, V) < 0.0;
    if (flipped) n = -n;                               // thin panes and panels are seen from both sides
    uint surf = prm.y, sub = prm.z;
    float ceilH = max(float(prm.w) * 0.5, 2.6), y = p.y - gBase - SIDEWALK_HEIGHT;
    // the panels' light, where furniture, plants and walls let it through (ray-traced shadows); the room's bounce fills the shade
    float panels = mix(PANEL_FILL, 1.0, ceilingVis());
    if (surf == IS_PLANT || surf == IS_BARK || surf == IS_SOIL) return shadePlant(p, V, n, uv, seed, surf, sub, y, flipped, panels);
    vec3 Lc = lobbyLight(seed);
    uint sh = styleHash(seed);
    float aa = max(length(fwidth(uv)), 1e-4);
    // screen derivatives of the floor's uv and of a wall's (along, height), for the authored finishes
    vec2 uvx = dFdx(uv), uvy = dFdy(uv), wx = vec2(uvx.x, dFdx(p.y)), wy = vec2(uvy.x, dFdy(p.y));
    // the building's frame: world -> building directions, for floors and ceilings
    float ang = (float(vAux & 0xFFFFu) / 65535.0) * 6.2831853 - 3.14159265;
    mat2 toFrame = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));

    // irradiance from the panels overhead: strong on what faces up, softer on walls, a little bounce above
    float E = n.y > 0.5 ? 0.55 : n.y < -0.5 ? 0.14 : 0.26 + 0.14 * smoothstep(0.0, ceilH, y);
    E *= mix(0.65, 1.0, smoothstep(0.0, 0.7, y)) * mix(1.0, 0.75, smoothstep(5.0, 14.0, y));
    vec3 diff = Lc * E * panels + ambientLight(n, p.y - gBase) * 0.5;
    vec3 irr = vec3(0.0);                              // light built into the walls, falling on this surface
    float specB = pow(max(dot(n, normalize(vec3(0.0, 1.0, 0.0) + V)), 0.0), 40.0);
    bool cove = hf(sh, 70u) < 0.7;                     // lit coves at the walls' feet

    vec3 alb = vec3(0.5), emit = vec3(0.0);
    float gloss = 0.05;
    uint h = hash_combine(seed, sub);
    if (surf == IS_FLOOR || (surf == IS_ROOMFLOOR && (sub == IR_HALL || sub == IR_SECURE)) || (surf == IS_STONE && sub == 2u)) {
        float joint, polish;
        alb = lobbyStone(seed, uv, uvx, uvy, aa, joint, polish);
        if (surf == IS_ROOMFLOOR) alb *= 0.6;
        alb *= 1.0 - 0.5 * joint;
        gloss = polish * (1.0 - joint);
    } else if (surf == IS_ROOMFLOOR) {
        if (sub == IR_VESTIBULE) {
            alb = vec3(0.035, 0.035, 0.04) * (0.8 + 0.2 * step(0.5, fract(uv.x * 12.0)));
        } else if (sub == IR_TOILET || sub == IR_WC || sub == IR_SHOWER) {
            vec2 t = fract(uv / 0.3);
            float j = max(1.0 - smoothstep(0.0, 0.03 + aa * 3.0, min(t.x, 1.0 - t.x)), 1.0 - smoothstep(0.0, 0.03 + aa * 3.0, min(t.y, 1.0 - t.y)));
            alb = mix(vec3(0.52, 0.52, 0.50), vec3(0.22), j);
            gloss = 0.3;
        } else if (sub == IR_MEETING || sub == IR_SECURITY || sub == IR_FACILITIES || sub == IR_MAIL) {
            vec3 pal = hf(seed, 23u) < 0.5 ? vec3(0.06, 0.07, 0.09) : vec3(0.05, 0.08, 0.08);
            vec2 cell = floor(uv / 0.5);
            alb = pal * (0.85 + 0.3 * hfloat(hash3i(h, int(cell.x), int(cell.y), 2))) * (0.8 + 0.4 * vnoise(uv * 9.0, 0xCA9Eu));
            gloss = 0.0;
        } else {
            alb = vec3(0.22, 0.21, 0.20) * (0.7 + 0.5 * fbm(uv * 0.7, 0xC0C0u));
            gloss = 0.1;
        }
    } else if (surf == IS_WALL) {
        if (sub == 1u) {
            alb = vec3(0.04, 0.038, 0.036);            // slab edges round a void
            gloss = 0.3;
        } else {
            alb = lobbyWall(seed, vec2(uv.x, y), wx, wy, aa, gloss);
            irr += Lc * 0.3 * wallWash(y, ceilH);
            if (cove) footCove(y, aa, emit, irr);
            else if (y < 0.1) alb *= 0.3;              // skirting
        }
    } else if (surf == IS_CEILING) {
        // sub 2: a light fitting, its uv 0..1 across it
        alb = vec3(0.045, 0.044, 0.042);
        if (sub == 2u) {
            if (pbrLoaded(PBR_LIGHT_FIXTURE) && pbrLoaded(PBR_LIGHT_DIFFUSER)) return ceilingFixture(uv, uvx, uvy, V, toFrame, Lc, diff);
            emit = Lc * 2.6;
        }
    } else if (surf == IS_CORE || surf == IS_EMBLEM) {
        // dark stone cladding (now and then pale marble or bronze) in 1.2 m by 3 m panels
        float pick = hf(sh, 22u);
        uint salt = (sh >> 4) & 0xFFFFu;
        vec2 q = vec2(uv.x, y);
        if (surf == IS_EMBLEM) pick = hf(sh, 68u) < 0.6 ? 0.8 : 0.0;   // the feature panel against the cladding
        gloss = 0.55;
        float pair, rough;
        vec2 bq = bookMatch(q, 1.2, pair), boff = slabOffset(sh, vec2(pair, floor(y / 3.0) + 3.0));
        if (pick < 0.7 && pbrLoaded(PBR_MARBLE_DARK)) {
            alb = finish(PBR_MARBLE_DARK, bq, 2.4, boff, false, wx, wy, rough) * 0.8;
            gloss *= polishOf(rough);
        } else if (pick >= 0.7 && pick < 0.88 && pbrLoaded(PBR_MARBLE_WHITE)) {
            alb = finish(PBR_MARBLE_WHITE, bq, 2.4, boff, false, wx, wy, rough) * 0.6;
            gloss *= polishOf(rough);
        } else {
            alb = pick < 0.7 ? marble(q, vec3(0.04, 0.038, 0.036), vec3(0.24, 0.22, 0.20), salt, aa)
                : pick < 0.88 ? marble(q, vec3(0.30, 0.295, 0.28), vec3(0.12, 0.115, 0.11), salt, aa)
                : vec3(0.08, 0.055, 0.035) * (0.85 + 0.3 * vnoise(vec2(q.x * 0.3, q.y * 6.0), salt));
        }
        irr += Lc * 0.3 * wallWash(y, ceilH);
        if (cove) footCove(y, aa, emit, irr);
        if (surf == IS_CORE) {
            float jx = fract(q.x / 1.2), jy = fract(y / 3.0);
            alb *= 1.0 - 0.6 * max(1.0 - smoothstep(0.0, 0.004 + aa, min(jx, 1.0 - jx) * 1.2), 1.0 - smoothstep(0.0, 0.004 + aa, min(jy, 1.0 - jy) * 3.0));
            // light slots in every second or third vertical joint (none on a face's first)
            float every = 2.0 + floor(hf(sh, 67u) * 2.0), k = floor(q.x / 1.2 + 0.5);
            if (hf(sh, 66u) < 0.8 && k > 0.5 && mod(k, every) < 0.5)
                lightSlot(abs(q.x - k * 1.2), y, 0.12, ceilH - 0.12, aa, emit, irr);
        } else {
            // the building's mark (emit_emblem in src/city/mesh_lobby.c; sub: the panel's width in half metres)
            float W = float(sub) * 0.5;
            float S = clamp(min(W * 0.26, (ceilH - 1.2) * 0.25), 0.3, 0.85);   // the mark's half-size
            vec2 c = vec2(W * 0.5, min(2.4, ceilH * 0.55));
            uint mh = hash_combine(seed, 0xE3B1u);
            float d = emblemDist(mh, (q - c) / S) * S;
            // a wordmark under it: a row of glyphs
            float gs = S * 0.3, pitch = gs * 1.2;
            int nG = 3 + int(hf(mh, 40u) * 4.0);
            vec2 wq = q - vec2(c.x - 0.5 * float(nG) * pitch, c.y - S * 1.3 - gs);
            float gi = clamp(floor(wq.x / pitch), 0.0, float(nG) - 1.0);
            vec2 gq = vec2(wq.x - gi * pitch, wq.y) / gs, gc = clamp(gq, 0.0, 1.0);
            d = min(d, (glyphDist(hash_combine(mh, uint(gi)), gc) + length(gq - gc)) * gs - 0.012);
            float inside = 1.0 - smoothstep(-aa, aa, d);
            if (hf(sh, 69u) < 0.45) {
                emit += kelvin(4000.0) * 3.0 * inside;                            // backlit acrylic
            } else {
                // brass letters standing off the stone, lit from behind: a halo round them on the panel
                emit += COVE * 1.2 * exp(-max(d, 0.0) / 0.04) * (1.0 - inside);
                alb = mix(alb, vec3(0.55, 0.40, 0.18), inside);
                gloss = mix(gloss, 0.9, inside);
            }
            lightSlot(abs(uv.x - 0.04), y, 0.12, ceilH - 0.12, aa, emit, irr);
            lightSlot(abs(uv.x - (W - 0.04)), y, 0.12, ceilH - 0.12, aa, emit, irr);
        }
    } else if (surf == IS_METAL) {
        alb = sub == 1u ? vec3(0.30, 0.20, 0.10) : sub == 2u ? vec3(0.18) : vec3(0.06, 0.065, 0.07);   // dark anodised frames
        if (sub == 10u + 10u) alb = vec3(0.25) * (0.6 + 0.4 * step(0.1, fract(y / 0.4)));                    // shelving
        if (sub == 10u + 12u) alb = hf(seed, 25u) < 0.5 ? vec3(0.05, 0.15, 0.06) : vec3(0.2, 0.2, 0.21);   // bins
        if (sub == 10u + 15u) alb = vec3(0.30, 0.32, 0.35) * (0.7 + 0.3 * step(0.03, fract(uv.x / 0.5)));  // lockers
        gloss = 0.5;
    } else if (surf == IS_WOOD) {
        // sub 1 counters in pale oak; other pieces in dark or light veneer
        bool pale = sub == 1u || hfloat(h) >= 0.5;
        float rough;
        gloss = 0.25;
        if (pale && pbrLoaded(PBR_WOOD_LIGHT)) {
            alb = finish(PBR_WOOD_LIGHT, uv, 2.0, vec2(hfloat(h), 0.0), false, uvx, uvy, rough) * 0.9;
            gloss *= 0.6 + 0.6 * polishOf(rough);
        } else if (!pale && pbrLoaded(PBR_WOOD_DARK)) {
            alb = finish(PBR_WOOD_DARK, uv, 1.5, vec2(hfloat(h), 0.0), false, uvx, uvy, rough) * 1.4;
            gloss *= 0.6 + 0.6 * polishOf(rough);
        } else {
            alb = sub == 1u ? vec3(0.40, 0.30, 0.18) : pale ? vec3(0.45, 0.32, 0.18) : vec3(0.22, 0.12, 0.06);
            alb *= 0.85 + 0.15 * sin(uv.x * 40.0 + vnoise(uv * 3.0, 0x900Du) * 6.0);
        }
    } else if (surf == IS_FABRIC) {
        float k = hfloat(h);
        alb = sub == 200u ? vec3(0.40, 0.28, 0.15) : k < 0.25 ? vec3(0.06) : k < 0.45 ? vec3(0.03, 0.12, 0.12) : k < 0.65 ? vec3(0.35, 0.25, 0.05) :
              k < 0.8 ? vec3(0.18, 0.03, 0.04) : vec3(0.2);
    } else if (surf == IS_GLASS) {
        float F = glassFresnel(n, V);
        float cov = paneCoverage(F);
        return (Lc * (0.05 + 0.6 * specB) * (sub == 1u ? 0.6 : 1.0) * mix(1.0, 4.0, F) + vec3(0.004, 0.006, 0.006)) * 0.25 / cov;
    } else if (surf == IS_LIGHT) {
        return sub == 1u ? vec3(1.0, 0.45, 0.1) * 4.0 : sub == 3u ? neonColor(seed >> 5) * 5.0 : sub == 4u ? COVE * 3.5 : Lc * (sub == 2u ? 3.0 : 4.0);
    } else if (surf == IS_SIGN) {
        return vec3(0.1, 1.0, 0.35) * 3.0;
    } else if (surf == IS_SCREEN) {
        float scan = 0.85 + 0.15 * step(0.5, fract(y * 120.0));
        return vec3(0.25, 0.45, 0.9) * 2.5 * scan * (0.7 + 0.3 * vnoise(vec2(uv.x * 6.0, gTime * 2.0), h & 0xFFFFu));
    } else if (surf == IS_LINING) {
        if (sub == 1u) alb = hf(seed, 26u) < 0.5 ? vec3(0.35, 0.36, 0.38) : vec3(0.30, 0.38, 0.40);
        else { alb = vec3(0.015); gloss = 0.8; }
    } else if (surf == IS_STONE) {
        // 1 planters, 3 vanities, 4 the reception desk: dark polished stone
        alb = sub == 1u ? vec3(0.10, 0.10, 0.11) : sub == 3u ? vec3(0.70) : sub == 4u ? marble(vec2(uv.x, y) * 2.0, vec3(0.035), vec3(0.22, 0.2, 0.18), 0x0DE5u, aa)
            : vec3(0.65, 0.64, 0.62);
        gloss = sub == 4u ? 0.7 : 0.5;
        if (sub == 4u && pbrLoaded(PBR_MARBLE_DARK)) {
            float rough;
            bool top = abs(n.y) > 0.5;   // the top's uv runs over it; the sides' along and up
            alb = finish(PBR_MARBLE_DARK, top ? uv : vec2(uv.x, y), 1.6, vec2(0.3, 0.6), false, top ? uvx : wx, top ? uvy : wy, rough) * 0.8;
            gloss *= polishOf(rough);
        }
    } else if (surf == IS_LIFTDOOR) {
        float seam = 1.0 - smoothstep(0.0, 0.01 + aa, abs(fract(uv.x) - 0.5) - 0.0);
        alb = (hf(sh, 71u) < 0.5 ? vec3(0.40) : vec3(0.42, 0.30, 0.16)) * (0.9 + 0.1 * vnoise(vec2(uv.x * 60.0, y * 2.0), 0x11F7u)) * (1.0 - 0.7 * seam);
        gloss = 0.7;
    } else if (surf == IS_DOOR) {
        alb = sub == 1u ? vec3(0.25, 0.27, 0.30) : vec3(0.30, 0.20, 0.12);
        gloss = 0.2;
    } else if (surf == IS_SKYLIGHT) {
        vec2 t = fract(uv / 1.5);
        float bar = max(1.0 - smoothstep(0.0, 0.02 + aa, min(t.x, 1.0 - t.x)), 1.0 - smoothstep(0.0, 0.02 + aa, min(t.y, 1.0 - t.y)));
        return mix(CLOUD_DECK * 2.2 + Lc * 0.03, vec3(0.01), bar);
    } else if (surf == IS_OFFICE) {
        // the podium floors round an atrium: dark glazing onto lit offices
        float fh = facadeFloorH(seed), fy = fract((y - ceilH) / fh) * fh;
        float bay = floor(uv.x / 1.5), mull = 1.0 - smoothstep(0.0, 0.03 + aa, min(fract(uv.x / 1.5), 1.0 - fract(uv.x / 1.5)) * 1.5);
        float lit = step(0.35, hfloat(hash3i(seed, int(bay), int(floor((y - ceilH) / fh)), 9)));
        vec3 room = Lc * (0.08 + 0.5 * smoothstep(fh - 0.7, fh - 0.4, fy)) * lit + vec3(0.004);
        vec3 c = fy < 0.9 ? vec3(0.03) * diff : room;
        return mix(c, vec3(0.02), mull);
    }

    vec3 col = alb * (diff + irr) + Lc * specB * gloss * 0.08 * panels + emit;
    float ndv = max(dot(n, V), 0.0), F = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
    if (abs(n.y) < 0.5) col += Lc * 0.05 * F * gloss;   // polished walls catch the lit room
    if (gloss > 0.4 && n.y > 0.5) {
        // polished floors mirror the ceiling's panels, and (screen-space) the lit walls round them
        vec3 R = reflect(-V, n);
        if (R.y > 0.05) {
            float t = (ceilH - y) / R.y;
            vec2 q = uv + toFrame * (R.xz * t), off = vec2(float((vAux >> 16) & 255u), float(vAux >> 24)) * (FIT_PITCH / 256.0);
            col += (Lc * 0.8 * ceilingPanel(q, off, aa + 0.12 * t + 0.1) * F * gloss + Lc * 0.02 * F) * panels;
        }
        gRefl = vec4(n.x, n.z, INDOOR_ROUGH, min((F * 1.5 + 0.08) * gloss, 0.9));
    }
    return col;
}
