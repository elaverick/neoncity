// Part of shaders/city.frag, which includes the parts in order: the rooms behind street vendors' openings.

/* ======================================================================
 * A vendor's painted room (roadmap SV3; docs/street-vendor-generation.md
 * section 6). Each opening of a vendor (a kiosk's hatch, a container's hatch
 * or walk-in front, a canopy bar's or lean-to's shelf recess, the gap behind
 * a market stall's table, a barrow's cabinet) is one quad; behind it a box is
 * traced, and what it holds is painted procedurally from the vendor's trade,
 * palette and seed, so no two are alike and nothing is stored or streamed:
 *
 *   - the back wall: shelves with real depth (their boards' fronts, tops and
 *     undersides met by the ray) and the painted stock on them, and the
 *     feature wall (a menu board, a tiled splashback, a mural, a wall of
 *     screens, a pegboard, stacked crates, backlit photographs);
 *   - the side walls, more stock painted flat; the floor; the ceiling and
 *     its strip light;
 *   - a far card: the prep table, with what the trade works with on it;
 *   - a near card: what hangs just inside the head (utensils, lanterns,
 *     bunches, garments, cables, fish) and the bare bulb, if the lamp is one.
 *
 * The cards are cut-outs at fixed depths, so the back wall, the cards and the
 * opening move against each other as the eye does. No figures (the user's
 * call): the far card holds only the table. The room is lit by its lamp
 * (the vendor's, its colour), and by what reaches the opening from outside
 * (the light on the opening's own face, as a window's room has gWinE). Far
 * off it fades to its mean.
 *
 * The quad's words (src/city/mesh_vendor.c room_word; src/core/common.h):
 * params z, w its width and height (2.5 cm), the normal's w its depth
 * (2.5 cm); aux its stock (3 x 5 bits), feature wall (3), kind (3), palette
 * (4), cooking (2), wear (3), a tube lamp (1), shut (1); the seed its lamp's
 * colour (RGB565, high half) and a hash (low half). uv metres along and up.
 * ==================================================================== */

const uint VO_HATCH = 0u, VO_WALKIN = 1u, VO_RECESS = 2u, VO_BACKCLOTH = 3u, VO_CABINET = 4u;
const uint VK_BOWLS = 0u, VK_BOTTLES = 1u, VK_JARS = 2u, VK_STEAMERS = 3u, VK_TANKS = 4u, VK_FISHRACK = 5u,
           VK_DRAWERS = 6u, VK_CRT = 7u, VK_PARTS = 8u, VK_SLEEVES = 9u, VK_BOLTS = 10u, VK_MASKS = 11u,
           VK_CLOCKS = 12u, VK_TOOLS = 13u, VK_PLANTS = 14u, VK_FRUIT = 15u, VK_VEG = 16u, VK_FLOWERS = 17u,
           VK_LOAVES = 18u, VK_JOINTS = 19u, VK_BOOKS = 20u, VK_CUPS = 21u, VK_BOXES = 22u, VK_CERAMICS = 23u,
           VK_TEXTILES = 24u;
const uint VW_SHELVES = 0u, VW_MENU = 1u, VW_MOSAIC = 2u, VW_MURAL = 3u, VW_SCREENS = 4u, VW_PEGBOARD = 5u,
           VW_CRATES = 6u, VW_LIGHTBOX = 7u;

// The vendors' palettes, sRGB: body, trim, two accents (src/gen/vendor_tables.c V_PALETTE; keep in step).
const uint V_PAL[48] = uint[48](
    0x2F6F6Au, 0xC9B98Fu, 0xC0392Bu, 0xE8C547u,   0x8E1E1Eu, 0x1C1C1Cu, 0xE8C547u, 0xF2EAD3u,
    0x4F6B3Au, 0xB8A27Au, 0x7A3B2Eu, 0xD9CBA3u,   0xD4A017u, 0x222222u, 0x3A6EA5u, 0xEEEEEEu,
    0x2E5C9Au, 0x8A8A80u, 0xE07B39u, 0xDADADAu,   0x9C4A21u, 0x3B3B3Bu, 0x5E8C8Au, 0xCFC6B0u,
    0x3C2A5Au, 0x111111u, 0xD94F8Cu, 0x7FD1E0u,   0xE9E1CCu, 0x8E1E1Eu, 0x2B4C3Fu, 0xC9A45Cu,
    0x2B2B2Bu, 0xA67B4Fu, 0xD9C7A7u, 0x6E7F5Eu,   0x7C8C76u, 0xB08D57u, 0xEFE9DDu, 0x2F3B36u,
    0x9AA0A3u, 0x3A3F44u, 0xD8B67Au, 0xF1EFEAu,   0x161616u, 0xC2A67Eu, 0x5B4636u, 0xE8E2D6u);

vec3 vSrgb(uint c) {
    vec3 s = vec3(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u)) / 255.0;
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), step(0.04045, s));
}
vec3 vPal(uint pal, uint slot) { return vSrgb(V_PAL[min(pal, 11u) * 4u + slot]); }

struct VRoom {
    float W, H, D, fl, ce;   // the opening's width and height, the room's depth; its floor and ceiling (from the sill)
    uint  kind, feature, cook, pal, h;
    uint  stock[3];
    float wear;
    bool  tube;
    vec3  lamp;              // the lamp's colour
    vec3  bulb;              // where a bulb hangs (room metres)
    vec3  outside;           // what reaches the opening from outside (the facade's gWinE)
    float sd;                // the shelves' depth
};

// The room's light at p (room metres: x along the opening, y up from its sill, z in), on a surface facing nr.
vec3 vrLight(VRoom R, vec3 p, vec3 nr) {
    vec3 lp = R.tube ? vec3(clamp(p.x, 0.12 * R.W, 0.88 * R.W), R.ce - 0.05, min(0.4, R.D * 0.4)) : R.bulb;
    vec3 l = lp - p;
    float dd = dot(l, l);
    float lam = 0.3 + 0.7 * max(dot(nr, l * inversesqrt(max(dd, 1e-4))), 0.0);
    vec3 c = R.lamp * lam * (0.12 + 1.9 / (1.0 + dd * 1.1));
    // and the street's, in through the opening, fading as it goes in
    c += R.outside * (0.25 + 0.5 * max(-nr.z, 0.0)) / (1.0 + p.z * p.z * 2.0);
    return c * mix(1.0, 0.75, R.wear);
}

/* ----------------------------------------------------------------- stock --- */

// A goods' colour: per kind, varied by the item's hash; some take the vendor's accents.
vec3 vStockColour(uint k, uint ih, uint pal) {
    float a = hf(ih, 3u), b = hf(ih, 4u);
    vec3 acc = vPal(pal, 2u + (ih & 1u));
    if (k == VK_BOWLS || k == VK_CUPS) return a < 0.7 ? vec3(0.62, 0.6, 0.55) * mix(0.8, 1.0, b) : a < 0.85 ? vec3(0.42, 0.3, 0.18) : acc * 0.8;
    if (k == VK_BOTTLES) {
        vec3 g[5] = vec3[5](vec3(0.06, 0.16, 0.06), vec3(0.16, 0.07, 0.02), vec3(0.35, 0.38, 0.36), vec3(0.3, 0.05, 0.04), vec3(0.4, 0.3, 0.05));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_JARS) {
        vec3 g[5] = vec3[5](vec3(0.45, 0.25, 0.04), vec3(0.3, 0.05, 0.03), vec3(0.18, 0.25, 0.06), vec3(0.5, 0.4, 0.25), vec3(0.08, 0.05, 0.03));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_STEAMERS) return vec3(0.42, 0.3, 0.15) * mix(0.75, 1.1, a);
    if (k == VK_FISHRACK) return mix(vec3(0.3, 0.26, 0.2), vec3(0.45, 0.42, 0.38), a);
    if (k == VK_DRAWERS) return vec3(0.2, 0.11, 0.05) * mix(0.8, 1.2, a);
    if (k == VK_CRT) return a < 0.5 ? vec3(0.4, 0.38, 0.33) : vec3(0.04);
    if (k == VK_PARTS || k == VK_SLEEVES || k == VK_BOLTS) {
        vec3 c = neonColor(ih >> 6);
        return mix(c, vec3(dot(c, vec3(0.33))), k == VK_PARTS ? 0.2 : 0.35) * mix(0.25, 0.55, b);
    }
    if (k == VK_MASKS) return a < 0.4 ? vec3(0.6, 0.57, 0.5) : a < 0.75 ? vec3(0.4, 0.04, 0.03) : vec3(0.45, 0.32, 0.05);
    if (k == VK_CLOCKS) return a < 0.5 ? vec3(0.22, 0.12, 0.05) : vec3(0.4, 0.3, 0.1);
    if (k == VK_TOOLS) return a < 0.6 ? vec3(0.22, 0.23, 0.24) : a < 0.8 ? vec3(0.45, 0.06, 0.03) : vec3(0.5, 0.35, 0.02);
    if (k == VK_PLANTS) return mix(vec3(0.04, 0.14, 0.03), vec3(0.12, 0.25, 0.05), a);
    if (k == VK_FRUIT) {
        vec3 g[5] = vec3[5](vec3(0.6, 0.18, 0.02), vec3(0.5, 0.04, 0.03), vec3(0.6, 0.45, 0.04), vec3(0.2, 0.4, 0.05), vec3(0.55, 0.3, 0.03));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_VEG) {
        vec3 g[5] = vec3[5](vec3(0.08, 0.25, 0.04), vec3(0.15, 0.32, 0.06), vec3(0.2, 0.05, 0.15), vec3(0.55, 0.5, 0.4), vec3(0.5, 0.2, 0.02));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_FLOWERS) {
        vec3 g[5] = vec3[5](vec3(0.6, 0.12, 0.25), vec3(0.65, 0.55, 0.08), vec3(0.65, 0.62, 0.6), vec3(0.5, 0.03, 0.05), vec3(0.35, 0.15, 0.5));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_LOAVES) return mix(vec3(0.35, 0.18, 0.05), vec3(0.55, 0.35, 0.12), a);
    if (k == VK_JOINTS) return mix(vec3(0.35, 0.05, 0.03), vec3(0.5, 0.22, 0.15), a);
    if (k == VK_BOOKS) {
        vec3 g[6] = vec3[6](vec3(0.25, 0.04, 0.04), vec3(0.04, 0.08, 0.2), vec3(0.4, 0.3, 0.1), vec3(0.05, 0.15, 0.08), vec3(0.5, 0.47, 0.4), vec3(0.06, 0.05, 0.05));
        return g[uint(a * 6.0) % 6u] * mix(0.7, 1.1, b);
    }
    if (k == VK_BOXES) return a < 0.6 ? vec3(0.38, 0.26, 0.14) * mix(0.8, 1.1, b) : a < 0.8 ? vec3(0.6) : acc * 0.7;
    if (k == VK_CERAMICS) {
        vec3 g[5] = vec3[5](vec3(0.55, 0.6, 0.5), vec3(0.65, 0.63, 0.58), vec3(0.45, 0.3, 0.12), vec3(0.12, 0.2, 0.35), vec3(0.3, 0.12, 0.06));
        return g[uint(a * 5.0) % 5u];
    }
    if (k == VK_TEXTILES) return mix(acc, vec3(0.55, 0.5, 0.42), 0.4 + 0.4 * b) * 0.8;
    return vec3(0.3);
}

/* One shelf's goods of kind k: x metres along the shelf, y up from the board, up to rowH. apx: the
 * metres a pixel spans there (narrow goods give way to their average as it grows). Returns the
 * albedo and coverage; e the goods' own light (screens, tanks). */
vec4 vStock(uint k, float x, float y, float rowH, uint h, uint pal, float apx, out vec3 e) {
    e = vec3(0.0);
    if (y < 0.0 || y > rowH) return vec4(0.0);
    // the cell an item stands in, and how tall it is
    float cw = k == VK_BOTTLES ? 0.085 : k == VK_SLEEVES ? 0.018 : k == VK_BOOKS ? 0.04 : k == VK_CUPS ? 0.1 :
               k == VK_JARS || k == VK_PARTS || k == VK_TOOLS ? 0.13 : k == VK_TANKS ? 0.55 : k == VK_CRT ? 0.36 :
               k == VK_STEAMERS || k == VK_BOLTS || k == VK_TEXTILES || k == VK_FRUIT || k == VK_VEG ? 0.32 :
               k == VK_DRAWERS ? 0.2 : 0.21;
    // too small to draw: the shelf's goods as one
    float blur = smoothstep(0.18, 0.55, apx / cw);
    vec3 avg = (vStockColour(k, h, pal) + vStockColour(k, h ^ 0x3A1u, pal) + vStockColour(k, h ^ 0x5C7u, pal)) / 3.0;
    avg = mix(avg, vec3(dot(avg, vec3(0.2126, 0.7152, 0.0722))), 0.5) * 0.6;   // muted: goods and the gaps between them
    if (k == VK_CRT || k == VK_TANKS) e = (k == VK_TANKS ? vec3(0.06, 0.25, 0.28) : vec3(0.25, 0.3, 0.35)) * blur;
    if (blur >= 1.0) return vec4(avg, step(y, rowH * 0.75));
    float ci = floor(x / cw), fx = fract(x / cw);
    uint ih = hash_combine(h, uint(int(ci) + 8192));
    float lim = rowH - 0.03;
    float hh = min(lim, k == VK_BOTTLES ? mix(0.2, 0.3, hf(ih, 1u)) : k == VK_SLEEVES ? 0.31 :
                        k == VK_BOOKS ? mix(0.17, 0.28, hf(ih, 1u)) : k == VK_JARS ? mix(0.1, 0.18, hf(ih, 1u)) :
                        k == VK_CRT ? 0.28 : k == VK_TANKS ? lim : k == VK_LOAVES ? mix(0.07, 0.12, hf(ih, 1u)) :
                        k == VK_DRAWERS ? lim : mix(0.12, 0.3, hf(ih, 1u)));
    // a gap now and then: a gap-toothed shelf reads as a used one
    if (k != VK_DRAWERS && k != VK_TANKS && k != VK_SLEEVES && k != VK_BOOKS && hf(ih, 2u) < 0.12) return vec4(0.0);
    vec3 col = vStockColour(k, ih, pal);
    float cov = 0.0, sx = (fx - 0.5) * cw;   // metres from the cell's middle
    if (k == VK_BOTTLES) {
        float body = hh * 0.62, r = y < body ? 0.032 : y < body + 0.04 ? mix(0.032, 0.012, (y - body) / 0.04) : 0.012;
        cov = step(abs(sx), r) * step(y, hh);
        if (y > hh - 0.025) col = vec3(0.12, 0.1, 0.08);                  // the cap
        else if (y > body * 0.3 && y < body * 0.75) col = mix(col, vec3(0.6, 0.55, 0.45), 0.6);   // the label
    } else if (k == VK_JARS || k == VK_CUPS || k == VK_BOWLS || k == VK_STEAMERS || k == VK_BOLTS || k == VK_TEXTILES) {
        float w = k == VK_BOWLS ? mix(0.085, 0.07, fract(y / 0.035)) : k == VK_CUPS ? 0.035 : k == VK_STEAMERS ? 0.14 : k == VK_JARS ? 0.05 : 0.15;
        cov = step(abs(sx), w) * step(y, hh);
        float bandH = k == VK_STEAMERS ? 0.07 : k == VK_BOLTS ? 0.06 : k == VK_TEXTILES ? 0.045 : k == VK_BOWLS ? 0.035 : k == VK_CUPS ? 0.02 : 1.0;
        float bi = floor(y / bandH);
        if (k == VK_BOLTS || k == VK_TEXTILES) col = vStockColour(k, hash_combine(ih, uint(bi)), pal);
        col *= 1.0 - 0.35 * step(bandH - 0.008, mod(y, bandH));          // the line between them
        if (k == VK_JARS && y > hh - 0.025) col = vec3(0.3, 0.3, 0.28);    // the lid
    } else if (k == VK_SLEEVES || k == VK_BOOKS) {
        cov = step(0.12, fx) * step(y, hh);
        if (k == VK_SLEEVES && hf(ih, 9u) < 0.08) { cov = step(abs(sx), 0.009) * step(y, 0.31); col = vec3(0.6, 0.58, 0.5); }
    } else if (k == VK_CRT) {
        cov = step(abs(sx), 0.16) * step(y, hh);
        vec2 s = vec2(sx, y - hh * 0.55);
        if (abs(s.x) < 0.12 && abs(s.y) < 0.09) {
            vec3 sc = neonColor(ih >> 4) * 0.4 + 0.35;
            float flick = 0.7 + 0.3 * vnoise(vec2(gTime * 7.0, float(ih & 255u)), 0x7A2u);
            e = sc * flick * (hf(ih, 7u) < 0.7 ? 1.4 : 0.05);
            col = vec3(0.02);
        }
    } else if (k == VK_TANKS) {
        cov = step(abs(sx), cw * 0.46) * step(y, hh);
        // lit water; fish drifting across it
        float fy = hh * (0.3 + 0.5 * hf(ih, 6u)), fxp = fract(gTime * 0.05 * (0.5 + hf(ih, 5u)) + hf(ih, 8u)) * cw - cw * 0.5;
        float fish = step(length((vec2(sx, y) - vec2(fxp, fy)) / vec2(0.06, 0.02)), 1.0);
        e = mix(vec3(0.08, 0.35, 0.4), vec3(0.03, 0.08, 0.06), fish) * 0.9;
        col = vec3(0.02);
        if (y > hh - 0.03) { e = vec3(0.0); col = vec3(0.04); }   // its lid
    } else if (k == VK_DRAWERS) {
        float dy = fract(y / 0.12);
        cov = 1.0;
        col *= 1.0 - 0.6 * (step(fx, 0.04) + step(0.96, fx) + step(dy, 0.06));
        if (length(vec2(sx, (dy - 0.55) * 0.12)) < 0.012) col = vec3(0.35, 0.3, 0.2);   // its knob
        if (abs(sx) < 0.05 && abs(dy - 0.25) < 0.08) col = mix(col, vec3(0.55, 0.5, 0.4), 0.6);   // a label
    } else if (k == VK_FRUIT || k == VK_VEG) {
        // a crate, and its fruit heaped over the rim
        float rim = min(0.11, hh * 0.6);
        float bump = 0.035 * abs(sin(x * 70.0 + float(ih & 15u))) + 0.02;
        cov = step(abs(sx), cw * 0.46) * step(y, rim + bump);
        if (y < rim) col = vec3(0.3, 0.22, 0.12) * (fract(y / 0.035) < 0.25 ? 0.5 : 1.0);
        else col = vStockColour(k, hash_combine(ih, uint(floor(x / 0.06))), pal);
    } else if (k == VK_PLANTS || k == VK_FLOWERS) {
        float pot = 0.09, r = min(0.08, (hh - pot) * 0.5);
        if (y < pot) { cov = step(abs(sx), mix(0.045, 0.06, y / pot)); col = k == VK_FLOWERS ? vec3(0.15, 0.15, 0.15) : vec3(0.32, 0.12, 0.06); }
        else {
            vec2 q = vec2(sx, y - pot - r) / r;
            float nz = vnoise(vec2(x, y) * 40.0, ih & 0xFFFu);
            cov = step(length(q), 0.85 + 0.35 * nz);
            if (k == VK_FLOWERS && nz > 0.45) col = vStockColour(k, ih ^ 0x55u, pal);
            else col = vec3(0.05, 0.18, 0.04) * (0.6 + 0.6 * nz);
        }
    } else if (k == VK_LOAVES) {
        float q = 2.0 * fx - 1.0;
        cov = step(y, hh * sqrt(max(1.0 - q * q, 0.0)));
        col *= 0.8 + 0.3 * (y / max(hh, 0.01));
    } else if (k == VK_MASKS || k == VK_CLOCKS || k == VK_JOINTS || k == VK_FISHRACK) {
        float r = min(cw * 0.42, hh * 0.5);
        vec2 q = vec2(sx, y - r) / vec2(r * (k == VK_FISHRACK ? 1.0 : k == VK_MASKS ? 0.75 : 1.0), r * (k == VK_FISHRACK ? 0.4 : 1.0));
        cov = step(length(q), 1.0);
        if (k == VK_MASKS && (length(q - vec2(-0.35, 0.2)) < 0.18 || length(q - vec2(0.35, 0.2)) < 0.18)) col = vec3(0.01);
        if (k == VK_CLOCKS) {
            if (length(q) < 0.8) col = vec3(0.6, 0.58, 0.5);
            float ang = atan(q.y, q.x);
            if (length(q) < 0.65 && (abs(sin(ang - 1.3)) < 0.06 || abs(sin(ang - float(ih & 7u))) < 0.06) && dot(q, q) > 0.0) col = vec3(0.02);
        }
    } else if (k == VK_CERAMICS) {
        float t = y / max(hh, 0.01);
        float r = 0.035 + 0.035 * sin(t * 3.0 + hf(ih, 6u) * 2.0) * (0.6 + 0.6 * hf(ih, 5u));
        if (t > 0.85) r = 0.025;
        cov = step(abs(sx), r) * step(y, hh);
        col *= 0.8 + 0.4 * smoothstep(0.0, r, r - abs(sx));
    } else {   // boxes, parts' bins, tools, the rest: boxes with a label
        cov = step(abs(sx), cw * 0.44) * step(y, hh);
        if (k == VK_PARTS && y > hh * 0.6) col = vec3(0.05);   // the bin's open top, its parts in the dark
        else if (abs(y - hh * 0.5) < 0.025 && abs(sx) < cw * 0.3) col = mix(col, vec3(0.6), 0.5);
    }
    return vec4(mix(col, avg, blur), max(cov, step(0.5, blur) * step(y, rowH * 0.75)));
}

/* ---------------------------------------------------------- the room ------ */

// A bay of shelving's stock kind: mostly the first, some of the others.
uint vrBayStock(VRoom R, uint bh) {
    float a = hf(bh, 21u);
    return a < 0.55 ? R.stock[0] : a < 0.85 ? R.stock[1] : R.stock[2];
}

// The wall behind the stock: painted board, the vendor's body colour dulled; or bare board.
vec3 vrWallAlb(VRoom R, vec2 q, float px) {
    vec3 c = hf(R.h, 30u) < 0.5 ? mix(vPal(R.pal, 0u), vec3(0.3), 0.4) * 0.6 : vec3(0.0);
    int m = hf(R.h, 31u) < 0.5 ? PBR_VENDOR_OSB : PBR_VENDOR_PLANKS;
    if (c == vec3(0.0)) c = pbrLoaded(m) ? pbrAlbedoMean(m) * pbrTintLod(m, q, px) : vec3(0.3, 0.22, 0.14);
    return c * mix(1.0, 0.6, R.wear);
}

// The feature wall's panel, high in the middle of the back wall, for a menu, a lightbox or screens:
// its centre and half size (x, y; zw), zw 0 if it has none.
vec4 vrPanel(VRoom R) {
    if (R.feature != VW_MENU && R.feature != VW_LIGHTBOX && R.feature != VW_SCREENS) return vec4(0.0);
    float top = R.ce - 0.1, pw = min(R.W * 0.6, 1.4), ph = min(0.55, (top - max(R.fl + 1.25, 0.2)) * 0.9);
    if (ph < 0.15 || pw < 0.3) return vec4(0.0);
    return vec4(R.W * (0.35 + 0.3 * hf(R.h, 40u)), top - 0.05 - ph * 0.5, pw * 0.5, ph * 0.5);
}
bool vrInPanel(vec4 pr, vec2 q) { return pr.z > 0.0 && abs(q.x - pr.x) < pr.z && abs(q.y - pr.y) < pr.w; }

// The feature wall's panel (where it is not shelving): albedo, emission; a < 0: not here.
vec4 vrFeature(VRoom R, vec2 q, float px, out vec3 e) {
    e = vec3(0.0);
    uint f = R.feature;
    float W = R.W, top = R.ce - 0.1, y = q.y;
    vec3 acc1 = vPal(R.pal, 2u), acc2 = vPal(R.pal, 3u);
    if (f == VW_MENU || f == VW_LIGHTBOX || f == VW_SCREENS) {
        vec4 pr = vrPanel(R);
        vec2 l = q - pr.xy;
        if (pr.z <= 0.0 || abs(l.x) > pr.z || abs(l.y) > pr.w) return vec4(-1.0);
        vec2 u = l / (2.0 * pr.zw) + 0.5;
        if (f == VW_SCREENS) {
            vec2 g = vec2(4.0, 2.0), cell = floor(u * g), fc = fract(u * g);
            uint sh = hash2i(R.h, int(cell.x), int(cell.y));
            if (fc.x < 0.08 || fc.y < 0.1) return vec4(0.02, 0.02, 0.02, 1.0);
            float scene = floor(gTime * (0.15 + 0.2 * hf(sh, 2u)) + 9.0 * hf(sh, 3u));
            uint ss = hash_combine(sh, uint(scene));
            e = (mix(neonColor(ss >> 3), vec3(0.6, 0.7, 0.9), 0.5) * (0.3 + 0.7 * vnoise(fc * 6.0 + scene, ss & 0xFFu))) * 1.3;
            return vec4(0.01, 0.01, 0.01, 1.0);
        }
        if (f == VW_LIGHTBOX) {
            // backlit photographs of the food: three panes, warm blobs on white
            float pane = floor(u.x * 3.0), fx = fract(u.x * 3.0);
            if (fx < 0.04 || u.y < 0.06 || u.y > 0.94) return vec4(0.03, 0.03, 0.03, 1.0);
            uint ph2 = hash_combine(R.h, uint(pane) + 50u);
            vec2 b = vec2(fx - 0.5, u.y - 0.55) * vec2(1.3, 1.0);
            float blob = smoothstep(0.32, 0.28, length(b) + 0.06 * vnoise(b * 9.0, ph2 & 0xFFu));
            vec3 food = mix(vec3(0.9, 0.55, 0.25), vec3(0.9, 0.25, 0.12), hf(ph2, 1u));
            vec3 c = mix(vec3(1.0, 0.95, 0.88), food, blob);
            if (u.y < 0.2) c = mix(c, acc1, 0.7) * (abs(fx - 0.5) < 0.3 && fract(u.x * 40.0) < 0.6 ? 0.4 : 1.0);   // its name
            e = c * 1.1;
            return vec4(0.02, 0.02, 0.02, 1.0);
        }
        // a menu board: rows of hand-written dishes and prices, light on dark or dark on light
        bool dark = hf(R.h, 41u) < 0.6;
        vec3 ground = dark ? vec3(0.02, 0.022, 0.02) : vec3(0.5, 0.46, 0.38), ink = dark ? vec3(0.55, 0.53, 0.48) : vec3(0.03);
        if (u.x < 0.03 || u.x > 0.97 || u.y < 0.04 || u.y > 0.96) return vec4(vPal(R.pal, 1u) * 0.5, 1.0);   // the frame
        float row = floor(u.y * 7.0), ry = fract(u.y * 7.0);
        uint rh = hash_combine(R.h, uint(row) + 60u);
        float len = 0.35 + 0.3 * hf(rh, 1u);
        float strokes = step(0.3, ry) * step(ry, 0.7) * step(0.08, u.x) *
                        (step(u.x, 0.08 + len) * step(0.35, fract(u.x * 26.0 + hf(rh, 2u))) + step(0.8, u.x) * step(u.x, 0.92));
        if (row == 6.0) strokes = step(0.25, ry) * step(ry, 0.85) * step(0.2, u.x) * step(u.x, 0.8) * step(0.3, fract(u.x * 14.0));
        strokes *= 1.0 - smoothstep(0.004, 0.012, px);   // far off: the board's own colour
        vec3 c = mix(ground, row == 6.0 ? acc1 : ink, strokes);
        if (!dark && hf(R.h, 42u) < 0.4) e = c * 0.25;   // a backlit one
        return vec4(c, 1.0);
    }
    if (f == VW_MOSAIC) {
        // a tiled splashback to 1.6 m, small tiles, the accents scattered through the white
        if (y > R.fl + 1.6) return vec4(-1.0);
        vec2 t = q / 0.05, ti = floor(t), tf = fract(t);
        uint th = hash2i(R.h, int(ti.x), int(ti.y));
        float k = hf(th, 1u);
        vec3 c = k < 0.12 ? acc1 : k < 0.2 ? acc2 : vec3(0.55, 0.55, 0.52) * mix(0.85, 1.0, hf(th, 2u));
        float grout = 1.0 - smoothstep(0.002, 0.01, px);
        if (tf.x < 0.08 || tf.y < 0.08) c = mix(c, vec3(0.12, 0.11, 0.1), grout);
        return vec4(c * mix(1.0, 0.55, R.wear), 1.0);
    }
    if (f == VW_MURAL) {
        // a painted wall: a sun, hills or waves in the accents, flaking
        float s = length(q - vec2(W * 0.6, top - 0.45)) - 0.3;
        float wave = y - (R.fl + 1.1 + 0.15 * sin(q.x * 4.0 + hf(R.h, 43u) * 6.0));
        vec3 c = mix(vPal(R.pal, 0u) * 0.7, acc2 * 0.8, smoothstep(0.0, 0.6, y - R.fl - 0.8));
        if (wave < 0.0) c = acc1 * 0.75;
        if (s < 0.0) c = mix(acc2, vec3(0.7, 0.55, 0.2), 0.5);
        c *= mix(1.0, 0.6 + 0.5 * fbm(q * 3.0, R.h & 0xFFFFu), 0.6 * R.wear + 0.2);
        return vec4(c, 1.0);
    }
    if (f == VW_PEGBOARD) {
        // pegboard, the holes on a 25 mm grid, its goods hung on it
        vec3 c = vec3(0.36, 0.27, 0.16);
        vec2 g = fract(q / 0.025) - 0.5;
        if (length(g) < 0.18) c *= mix(1.0, 0.25, 1.0 - smoothstep(0.002, 0.008, px));
        vec2 cell = floor(q / vec2(0.22, 0.3)), fc = fract(q / vec2(0.22, 0.3));
        uint ch = hash2i(R.h ^ 0x9E9u, int(cell.x), int(cell.y));
        if (y > R.fl + 1.0 && hf(ch, 1u) < 0.6) {
            vec3 ec;
            vec4 it = vStock(R.stock[0] == VK_TOOLS || R.stock[0] == VK_PARTS ? VK_TOOLS : R.stock[0], fc.x * 0.22, fc.y * 0.3 - 0.04, 0.26, ch, R.pal, px, ec);
            if (it.a > 0.5) { c = it.rgb; e = ec; }
        }
        return vec4(c, 1.0);
    }
    if (f == VW_CRATES) {
        // crates stacked to the ceiling, their ends out, the trade's goods in some
        vec2 cs = vec2(0.5, 0.32), cell = floor((q - vec2(0.0, R.fl)) / cs), fc = fract((q - vec2(0.0, R.fl)) / cs);
        uint ch = hash2i(R.h ^ 0xC4Au, int(cell.x), int(cell.y));
        fc.x = fract(fc.x + 0.5 * mod(cell.y, 2.0));
        vec3 c = mix(vec3(0.3, 0.2, 0.1), vec3(0.4, 0.32, 0.2), hf(ch, 1u));
        if (fc.x < 0.05 || fc.y < 0.06 || fc.x > 0.95) c *= 0.3;
        else if (fract(fc.y * 3.0) < 0.15) c *= 0.55;   // between its slats
        return vec4(c, 1.0);
    }
    return vec4(-1.0);
}

// The prep table and what is on it, a cut-out at depth z (the far card): albedo and coverage; e its glow.
vec4 vrPrep(VRoom R, vec2 q, float apx, out vec3 e) {
    e = vec3(0.0);
    float th = R.fl + (R.kind == VO_BACKCLOTH ? 0.55 : 0.88);   // the table's top (a market's crates: lower)
    float x0 = R.W * 0.06, x1 = R.W * (R.kind == VO_WALKIN ? 0.7 : 0.94);
    if (R.kind == VO_WALKIN) { x0 = R.W * (0.3 + 0.15 * hf(R.h, 50u)); x1 = x0 + min(1.4, R.W * 0.4); }
    if (q.x < x0 || q.x > x1 || q.y < R.fl) return vec4(0.0);
    if (q.y < th) {
        // the table: steel for a kitchen, board for the rest; a cupboard's doors, a shelf under it
        vec3 c = R.cook == 2u ? vec3(0.25, 0.25, 0.26) : R.kind == VO_BACKCLOTH ? vec3(0.3, 0.21, 0.11) : mix(vPal(R.pal, 1u), vec3(0.3), 0.5) * 0.7;
        if (q.y > th - 0.04) c *= 1.4;   // its top's edge
        else if (abs(fract((q.x - x0) / 0.5) - 0.5) > 0.48) c *= 0.35;   // between its doors
        if (R.kind == VO_BACKCLOTH && fract((q.y - R.fl) / 0.18) < 0.12) c *= 0.4;   // crates' slats
        return vec4(c, 1.0);
    }
    // on it: pots and a wok for a kitchen, a machine for drinks, the stock and a till for the rest
    float y = q.y - th;
    if (R.cook == 2u && R.kind != VO_BACKCLOTH) {
        float n = max(1.0, floor((x1 - x0) / 0.55));
        float cell = floor((q.x - x0) / ((x1 - x0) / n)), fx = fract((q.x - x0) / ((x1 - x0) / n));
        uint ph = hash_combine(R.h, uint(cell) + 70u);
        float k = hf(ph, 1u), sx = (fx - 0.5) * (x1 - x0) / n;
        if (R.stock[0] == VK_STEAMERS || k < 0.25) {   // a stack of bamboo steamers
            float hh = mix(0.2, 0.42, hf(ph, 2u));
            if (abs(sx) < 0.17 && y < hh) return vec4(vec3(0.42, 0.3, 0.15) * (fract(y / 0.07) > 0.85 ? 0.5 : 1.0), 1.0);
        } else if (k < 0.7) {   // a stock pot, its lid and handles
            float hh = mix(0.22, 0.34, hf(ph, 2u)), r = mix(0.14, 0.19, hf(ph, 3u));
            if (abs(sx) < r && y < hh) return vec4(vec3(0.3, 0.3, 0.31) * (y > hh - 0.03 ? 1.3 : 1.0) * (0.8 + 0.4 * (1.0 - abs(sx) / r)), 1.0);
            if (abs(sx) < 0.02 && y < hh + 0.04) return vec4(0.05, 0.05, 0.05, 1.0);
            if (abs(abs(sx) - r - 0.02) < 0.025 && abs(y - hh * 0.8) < 0.015) return vec4(0.1, 0.1, 0.1, 1.0);
        } else {   // a wok on its burner, the flame under it
            float r = 0.2;
            if (abs(sx) < r && y > 0.05 && y < 0.05 + 0.1 * (1.0 - (sx * sx) / (r * r))) return vec4(0.03, 0.03, 0.03, 1.0);
            if (abs(sx) < 0.12 && y < 0.05) {
                float fl = 0.6 + 0.4 * vnoise(vec2(q.x * 30.0, gTime * 12.0), 0xF1Au);
                e = vec3(0.2, 0.35, 1.0) * fl * 0.8;
                return vec4(0.02, 0.02, 0.03, 1.0);
            }
        }
        return vec4(0.0);
    }
    if (R.cook == 1u && R.kind != VO_BACKCLOTH) {
        // the machine: chrome and black, a red light; cups stacked beside it
        float mx = x0 + (x1 - x0) * (0.25 + 0.3 * hf(R.h, 51u));
        float sx = q.x - mx;
        if (abs(sx) < 0.25 && y < 0.42) {
            vec3 c = y > 0.36 || y < 0.04 ? vec3(0.45) : abs(sx) < 0.18 && y > 0.12 && y < 0.3 ? vec3(0.02) : vec3(0.32);
            if (length(vec2(sx - 0.18, y - 0.33)) < 0.012) e = vec3(1.0, 0.1, 0.05) * 2.0;
            return vec4(c, 1.0);
        }
        if (abs(sx - 0.42) < 0.04 && y < 0.3) return vec4(vec3(0.6, 0.58, 0.55) * (fract(y / 0.02) > 0.8 ? 0.6 : 1.0), 1.0);
    }
    // a till at one end
    float tx = hf(R.h, 52u) < 0.5 ? x0 + 0.2 : x1 - 0.2;
    if (abs(q.x - tx) < 0.15 && y < 0.14 + 0.1 * step(abs(q.x - tx - 0.05), 0.08)) {
        if (y > 0.15 && abs(q.x - tx - 0.05) < 0.06 && y < 0.23) e = vec3(0.2, 0.6, 0.35) * 0.6;
        return vec4(0.05, 0.05, 0.05, 1.0);
    }
    // and the stock laid out
    vec3 ec;
    vec4 s = vStock(R.stock[R.kind == VO_BACKCLOTH ? 0u : 1u], q.x - x0, y, 0.3, R.h ^ 0x7C3u, R.pal, apx, ec);
    e = ec;
    return s;
}

// What hangs just inside the head (the near card): albedo and coverage; e a bulb's glow.
vec4 vrHang(VRoom R, vec2 q, out vec3 e) {
    e = vec3(0.0);
    float rail = R.H - 0.05;
    if (q.y > rail + 0.02 || q.x < 0.02 || q.x > R.W - 0.02) return vec4(0.0);
    if (q.y > rail - 0.015 && R.kind != VO_BACKCLOTH) return vec4(0.08, 0.08, 0.08, 1.0);   // the rail
    // the bulb, on its flex
    if (!R.tube) {
        vec2 b = q - R.bulb.xy;
        if (abs(b.x) < 0.006 && q.y > R.bulb.y) return vec4(0.02, 0.02, 0.02, 1.0);
        if (length(b / vec2(1.0, 1.25)) < 0.032) { e = R.lamp * 6.0; return vec4(0.0, 0.0, 0.0, 1.0); }
    }
    uint k0 = R.stock[0];
    uint kind = k0 == VK_FISHRACK || k0 == VK_JOINTS ? 1u : k0 == VK_BOLTS || k0 == VK_TEXTILES ? 2u :
                k0 == VK_CRT || k0 == VK_PARTS || k0 == VK_TOOLS ? 3u :
                k0 == VK_PLANTS || k0 == VK_FLOWERS || k0 == VK_VEG || k0 == VK_FRUIT ? 4u : R.cook > 0u ? 5u : 6u;
    float cw = kind == 2u ? 0.38 : kind == 1u ? 0.2 : 0.26;
    float ci = floor(q.x / cw), fx = fract(q.x / cw), sx = (fx - 0.5) * cw;
    uint ih = hash_combine(R.h, uint(int(ci) + 900));
    if (hf(ih, 1u) > 0.45 + 0.25 * R.wear) return vec4(0.0);
    float len = mix(0.18, kind == 2u ? 0.7 : 0.45, hf(ih, 2u));
    float y = rail - q.y;   // down from the rail
    if (y > len) return vec4(0.0);
    if (kind == 1u) {   // fish or a joint, on a hook
        if (y < 0.06) return vec4(vec3(0.1) * step(abs(sx), 0.004), step(abs(sx), 0.004));
        float t = (y - 0.06) / (len - 0.06), w = (k0 == VK_JOINTS ? 0.07 : 0.04) * sin(3.14159 * clamp(t, 0.0, 1.0)) + 0.01;
        return vec4(vStockColour(k0, ih, R.pal) * 0.8, step(abs(sx), w));
    }
    if (kind == 2u) {   // a garment on a hanger
        if (y < 0.05) return vec4(0.12, 0.12, 0.12, step(abs(sx), 0.004 + y * 1.5));
        float w = y < 0.12 ? mix(0.06, 0.16, (y - 0.05) / 0.07) : 0.14;
        return vec4(vStockColour(VK_TEXTILES, ih, R.pal), step(abs(sx), w));
    }
    if (kind == 3u) {   // a coil of cable, or a tool
        vec2 c = vec2(sx, y - len * 0.6);
        float r = len * 0.3;
        return vec4(vec3(0.02), step(abs(length(c) - r), 0.012) + step(abs(sx), 0.004) * step(y, len * 0.3));
    }
    if (kind == 4u) {   // a bunch of herbs or flowers, heads down
        float t = y / len, w = mix(0.008, 0.06, t);
        vec3 c = k0 == VK_FLOWERS ? vStockColour(VK_FLOWERS, ih, R.pal) * 0.7 : vec3(0.08, 0.16, 0.05) * (0.6 + 0.6 * vnoise(q * 50.0, ih & 0xFFu));
        return vec4(c, step(abs(sx), w) * step(0.3, vnoise(q * 60.0, ih & 0xFFFu) + 0.6 - t * 0.4));
    }
    if (kind == 5u) {   // a ladle, a strainer, a cleaver on a hook
        float k = hf(ih, 3u);
        if (k < 0.5) return vec4(vec3(0.3), step(abs(sx), 0.006) * step(y, len - 0.06) + step(length(vec2(sx, y - len + 0.04) / vec2(1.0, 0.6)), 0.045));
        if (k < 0.8) return vec4(vec3(0.25), step(abs(sx), 0.006) * step(y, len * 0.5) + step(abs(length(vec2(sx, y - len * 0.7)) - 0.06), 0.008));
        return vec4(vec3(0.35), step(abs(sx), 0.008) * step(y, 0.12) + step(abs(sx + 0.02), 0.05) * step(0.12, y) * step(y, 0.28));
    }
    // a paper lantern, faintly lit, or a bag of something
    if (hf(ih, 4u) < 0.5) {
        vec2 c = vec2(sx, y - len + 0.11);
        if (y < len - 0.22) return vec4(vec3(0.05), step(abs(sx), 0.004));
        float l = length(c / vec2(0.1, 0.11));
        if (l < 1.0) { e = vec3(0.7, 0.1, 0.05) * 0.4 * (1.0 - 0.5 * l); return vec4(0.3, 0.04, 0.03, 1.0); }
        return vec4(0.0);
    }
    return vec4(vStockColour(VK_BOXES, ih, R.pal), step(abs(sx), 0.07) * step(len - 0.25, y));
}

/* The room seen at uv on its quad (o, along the opening and up from its sill), looking along dir
 * (world). n: the opening's normal (out). sill: its height over the vendor's floor. px: the metres a
 * pixel spans there, eye the eye's distance. outside: the light falling on the opening from outside. */
// The metres a pixel spans at a point t into the room on a face of normal nr, from px at the opening
// and the eye's distance to it.
float vrPx(float px, float t, float eye, vec3 d, vec3 nr) {
    return px * (1.0 + t / max(eye, 0.3)) * max(d.z, 0.1) / max(abs(dot(d, nr)), 0.1);
}

vec3 vendorRoom(vec2 uv, vec3 dir, vec3 n, uint seed, uvec4 prm, uint aux, float depth, float sill, float px, float eye, vec3 outside) {
    VRoom R;
    R.W = max(float(prm.z) * 0.025, 0.1); R.H = max(float(prm.w) * 0.025, 0.1); R.D = max(depth, 0.05);
    R.stock[0] = aux & 31u; R.stock[1] = (aux >> 5) & 31u; R.stock[2] = (aux >> 10) & 31u;
    R.feature = (aux >> 15) & 7u; R.kind = (aux >> 18) & 7u; R.pal = (aux >> 21) & 15u; R.cook = (aux >> 25) & 3u;
    R.wear = float((aux >> 27) & 7u) / 7.0; R.tube = ((aux >> 30) & 1u) != 0u;
    uint c565 = seed >> 16;
    R.lamp = vSrgb(((c565 >> 11) & 31u) * 0x080000u | ((c565 >> 5) & 63u) * 0x000400u | (c565 & 31u) * 0x000008u);
    R.h = hash_combine(seed & 0xFFFFu, 0x5E11u);
    R.outside = outside;
    // its floor and ceiling: a hatch looks over a counter onto the floor behind it; a market's gap
    // goes down to the ground; a recess, a cabinet and a walk-in front sit on theirs
    R.fl = R.kind == VO_HATCH ? -min(sill, 1.05) : R.kind == VO_BACKCLOTH ? -sill : 0.0;
    R.ce = R.H + (R.kind == VO_HATCH ? 0.25 : R.kind == VO_WALKIN ? 0.2 : R.kind == VO_BACKCLOTH ? 0.35 : 0.0);
    R.sd = min(R.D * (R.kind == VO_RECESS || R.kind == VO_CABINET ? 0.85 : 0.4), 0.35);
    R.bulb = vec3(R.W * (0.3 + 0.4 * hf(R.h, 2u)), R.H - 0.3 - 0.1 * hf(R.h, 3u), min(0.18, R.D * 0.25));

    // the ray in the room's frame: x along, y up, z in
    vec3 u = normalize(vec3(n.z, 0.0, -n.x));
    vec3 d = vec3(dot(dir, u), dir.y, -dot(dir, n));
    d.z = max(d.z, 1e-3);
    vec3 o = vec3(uv, 0.0);

    // far off: the room's mean, its lamp on what it holds
    vec3 mean = R.lamp * 0.12 * (R.kind == VO_RECESS || R.kind == VO_CABINET ? 0.6 : 1.0) + R.outside * 0.04;
    float lod = smoothstep(0.03, 0.09, px);
    if (lod >= 1.0) return mean;

    float tx = d.x > 0.0 ? (R.W - o.x) / d.x : d.x < 0.0 ? -o.x / d.x : 1e9;
    float ty = d.y > 0.0 ? (R.ce - o.y) / d.y : d.y < 0.0 ? (R.fl - o.y) / d.y : 1e9;
    float tz = (R.D - o.z) / d.z;
    float t = min(tx, min(ty, tz));
    vec3 col = vec3(0.0), e = vec3(0.0), alb = vec3(0.0), nr = vec3(0.0, 0.0, -1.0);
    vec3 p = o + d * t;
    bool done = false;
    bool deep = R.kind == VO_HATCH || R.kind == VO_WALKIN || R.kind == VO_BACKCLOTH;

    // the near card: what hangs inside the head
    if (deep && R.D > 0.45) {
        float zn = min(0.18, R.D * 0.25), tn = (zn - o.z) / d.z;
        if (tn < t) {
            vec3 pn = o + d * tn, en;
            vec4 h = vrHang(R, pn.xy, en);
            if (h.a > 0.5) { p = pn; alb = h.rgb; e = en; nr = vec3(0.0, 0.0, -1.0); done = true; }
        }
    }
    // the far card: the prep table
    if (!done && deep && R.D > 0.7) {
        float zf = R.kind == VO_BACKCLOTH ? R.D * 0.6 : min(max(R.D * 0.5, 0.35), R.D - R.sd - 0.08), tf = (zf - o.z) / d.z;
        if (tf < t) {
            vec3 pf = o + d * tf, ef;
            vec4 h = vrPrep(R, pf.xy, vrPx(px, tf, eye, d, vec3(0.0, 0.0, -1.0)), ef);
            if (h.a > 0.5) { p = pf; alb = h.rgb; e = ef; nr = vec3(0.0, 0.0, -1.0); done = true; }
        }
    }
    if (!done) {
        // the shelving on the back wall: its boards' fronts, the stock on them, their tops and undersides
        float y0 = R.kind == VO_RECESS || R.kind == VO_CABINET ? 0.0 : R.kind == VO_BACKCLOTH ? R.fl + 0.7 : R.fl + 0.95;
        float y1 = R.ce - 0.12;
        float rows = max(1.0, floor((y1 - y0) / 0.38 + 0.5)), pt = (y1 - y0) / rows, bt = 0.022;
        bool shelving = R.feature == VW_SHELVES || R.feature == VW_MENU || R.feature == VW_SCREENS || R.feature == VW_LIGHTBOX ||
                        R.kind == VO_RECESS || R.kind == VO_CABINET || (R.feature == VW_MOSAIC && R.kind != VO_BACKCLOTH);
        if (R.kind == VO_BACKCLOTH && R.feature != VW_SHELVES && R.feature != VW_CRATES) shelving = false;
        float zfr = R.D - R.sd, zs = R.D - R.sd * 0.55;
        vec4 panel = vrPanel(R);
        bool back = t == tz;
        if (shelving && y1 - y0 > 0.25) {
            float tF = (zfr - o.z) / d.z;
            if (tF < t) {
                vec3 pF = o + d * tF;
                float ly = pF.y - y0, row = floor(ly / pt);
                bool inShelf = pF.x > 0.0 && pF.x < R.W && ly >= 0.0 && ly <= rows * pt + bt && !vrInPanel(panel, pF.xy);
                if (inShelf && mod(ly, pt) < bt) { p = pF; alb = vrWallAlb(R, pF.xy * 2.0, px) * 1.2; nr = vec3(0.0, 0.0, -1.0); done = true; }
                if (!done && inShelf) {
                    float tS = (zs - o.z) / d.z;
                    vec3 pS = o + d * tS;
                    float lyS = pS.y - y0, rS = floor(lyS / pt);
                    if (pS.x > 0.0 && pS.x < R.W && lyS >= 0.0 && rS < rows && !vrInPanel(panel, pS.xy)) {
                        vec3 ef;
                        uint bh = hash2i(R.h, int(floor(pS.x / 0.9)), int(rS));
                        vec4 s = vStock(vrBayStock(R, bh), pS.x + 0.37 * rS, lyS - rS * pt - bt, pt - bt, bh, R.pal, vrPx(px, tS, eye, d, vec3(0.0, 0.0, -1.0)), ef);
                        if (s.a > 0.5) { p = pS; alb = s.rgb; e = ef; nr = vec3(0.0, 0.0, -1.0); done = true; }
                    }
                    if (!done) {
                        // a board's top (looking down) or underside (looking up) between the front and the wall
                        float yF = pF.y, yb = d.y < 0.0 ? y0 + floor((yF - y0 - bt) / pt) * pt + bt : y0 + ceil((yF - y0) / pt) * pt;
                        float tb = d.y != 0.0 ? (yb - o.y) / d.y : 1e9;
                        if (tb > tF && tb < t && yb >= y0 && yb <= y0 + rows * pt + bt) {
                            p = o + d * tb;
                            alb = vrWallAlb(R, p.xz * 2.0, px) * (d.y < 0.0 ? 1.0 : 0.4);
                            nr = vec3(0.0, d.y < 0.0 ? 1.0 : -1.0, 0.0); done = true;
                        }
                    }
                }
            }
        }
        if (!done) {
            if (back) {
                vec3 ef;
                vec4 f = vrFeature(R, p.xy, px, ef);
                if (f.a >= 0.0) { alb = f.rgb; e = ef; }
                else alb = vrWallAlb(R, p.xy * 2.0, px);
                // the shadow under each board
                if (shelving && p.y > y0 && p.y < y1 && !vrInPanel(panel, p.xy)) alb *= 0.45 + 0.55 * smoothstep(0.0, 0.12, pt - mod(p.y - y0, pt));
                if (R.kind == VO_BACKCLOTH && !shelving && R.feature != VW_CRATES) {
                    // a tarp hung behind the stall, its folds
                    alb = vPal(R.pal, hf(R.h, 33u) < 0.5 ? 0u : 3u) * 0.55 * (0.8 + 0.2 * sin(p.x * 9.0 + 2.0 * sin(p.y * 2.0)));
                }
            } else if (t == tx) {
                // a side wall: more stock, painted flat, above a cupboard; or bare board
                nr = vec3(d.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
                alb = vrWallAlb(R, p.zy * 2.0, px) * 0.8;
                float ly = p.y - (R.fl + 0.95);
                if (deep && ly > 0.0 && p.y < R.ce - 0.12 && R.kind != VO_BACKCLOTH) {
                    float row = floor(ly / 0.4), f = mod(ly, 0.4);
                    if (f < 0.02) alb *= 1.2;
                    else {
                        vec3 ef;
                        uint bh = hash2i(R.h ^ (d.x > 0.0 ? 0x51u : 0x52u), 0, int(row));
                        vec4 s = vStock(R.stock[1u + (bh & 1u)], p.z + row * 0.3, f - 0.02, 0.38, bh, R.pal, vrPx(px, t, eye, d, nr), ef);
                        if (s.a > 0.5) { alb = s.rgb; e = ef; }
                        else alb *= 0.5;
                    }
                }
            } else if (d.y > 0.0) {
                // the ceiling: the underside of the roof, the strip light across it
                nr = vec3(0.0, -1.0, 0.0);
                alb = R.kind == VO_BACKCLOTH ? vPal(R.pal, 0u) * 0.5 : vec3(0.08, 0.075, 0.07);
                if (R.tube && abs(p.z - min(0.4, R.D * 0.4)) < 0.03 && p.x > 0.12 * R.W && p.x < 0.88 * R.W) e = R.lamp * 5.0;
            } else {
                // the floor: worn lino or concrete, dirtier where it's trodden
                nr = vec3(0.0, 1.0, 0.0);
                alb = R.kind == VO_BACKCLOTH ? vec3(0.03, 0.03, 0.032) : mix(vec3(0.1, 0.09, 0.08), vPal(R.pal, 1u) * 0.25, 0.3);
                alb *= mix(1.0, 0.5 + 0.6 * fbm(p.xz * 4.0, R.h & 0xFFFu), 0.5);
            }
        }
    }
    // the corners and edges of the box darker, the walls grimy, more as the vendor wears
    vec3 a3 = vec3(min(p.x, R.W - p.x), min(p.y - R.fl, R.ce - p.y), R.D - p.z);
    float edge = max(min(a3.x, a3.y), min(max(a3.x, a3.y), a3.z));   // the second nearest face
    alb *= 0.45 + 0.55 * smoothstep(0.0, 0.3, edge);
    alb *= mix(1.0, 0.55 + 0.6 * fbm(p.xy * 2.5 + p.z, R.h & 0xFFFu), 0.25 + 0.45 * R.wear);
    col = alb * vrLight(R, p, nr) + e;
    return mix(col, mean, lod);
}
