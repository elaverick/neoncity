// Part of shaders/city.frag, which includes the parts in order: street vendors' shells.

/* ======================================================================
 * Street vendors (src/city/mesh_vendor.c, src/gen/vendor.h; roadmap SV2):
 * the structure of each stall, kiosk, lean-to, container, market stall and
 * barrow, in the chunk's base layer. params y the surface (VS_*), z its flags
 * (VPF_*), w its wear; aux its colour (sRGB 0xRRGGBB, the vendor's palette
 * resolved) | its part's role << 24. uv are metres along the part's faces
 * (along, then up on an upright face). Each surface is the procedural colour
 * over an authored material's grain (pbrGrain, as the arcology's and the
 * plants'): corrugated and rusted sheet in relief, painted steel, timber,
 * tile, brick, canvas and tarp, brushed steel. Wear brings grime: blotches,
 * streaks down from the top, dirt splashed up from the ground. The rooms
 * behind the openings stay dark until SV3 paints them; lamps glow, but light
 * nothing yet (SV4). A painted sign's face (VS_NEON, unlit) is hand-lettered:
 * the neon's own glyphs (tubeDist, signs.glsl) as brush strokes. Lit neon is
 * MAT_NEON (shadeNeon).
 * ==================================================================== */

const uint VS_STEEL = 0u, VS_CORRUGATED = 1u, VS_RUST = 2u, VS_TIMBER = 3u, VS_DECK = 4u, VS_TILE = 5u, VS_BRICK = 6u,
           VS_TARP = 7u, VS_STRIPE = 8u, VS_CLOTH = 9u, VS_STAINLESS = 10u, VS_LAMINATE = 11u, VS_CHROME = 12u,
           VS_RUBBER = 13u, VS_GLASS = 14u, VS_NEONFACE = 17u, VS_TUBE = 18u, VS_BULB = 19u, VS_ROOM = 20u,
           VS_SIGN = 21u, VS_SHUTTER = 22u;
const uint VPF_EMISSIVE = 4u;

vec3 vendorColour(uint aux) {
    vec3 s = vec3(float((aux >> 16) & 255u), float((aux >> 8) & 255u), float(aux & 255u)) / 255.0;
    return mix(s / 12.92, pow((s + 0.055) / 1.055, vec3(2.4)), step(0.04045, s));
}

// A painted sign: brush-lettered glyphs on a painted board, lit as a wall.
vec3 vendorPaintedSign(vec3 p, vec3 V, vec3 n, vec2 uv, uint seed, uvec4 prm, float y, float wear) {
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec2 m = uv * vec2(wM, hM);
    bool vertical = hM >= wM;
    float cell = vertical ? wM * 0.78 : hM * 0.72;
    float pad = vertical ? (wM - cell) * 0.5 : (hM - cell) * 0.5;
    float gi, px = max(fwidth(m.x), fwidth(m.y));
    float d = tubeDist(m, seed, wM, hM, 1u, vertical, cell, pad, gi);
    float stroke = cell * 0.045;
    float ink = 1.0 - smoothstep(stroke - px, stroke + px, d);
    ink = mix(ink, 0.25, smoothstep(0.06, 0.2, px / max(cell, 0.1)));   // far off: the strokes' average
    const vec3 GROUND[4] = vec3[4](vec3(0.62, 0.56, 0.42), vec3(0.55, 0.06, 0.04), vec3(0.6, 0.45, 0.06), vec3(0.04, 0.04, 0.045));
    const vec3 INK[4] = vec3[4](vec3(0.35, 0.03, 0.02), vec3(0.62, 0.56, 0.42), vec3(0.03, 0.03, 0.03), vec3(0.6, 0.5, 0.2));
    uint k = hash_combine(seed, 77u) & 3u;
    vec3 alb = mix(GROUND[k], INK[k], ink) * 0.7;
    alb *= mix(1.0, mix(0.55, 1.0, fbm(m * 3.0, seed & 0xFFFFu)), wear);   // faded and flaking
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 20.0, 0.1, y, spec);
    return alb * diff + spec;
}

vec3 shadeVendor(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, uint aux) {
    vec3 V = -normalize(rel);
    uint surf = prm.y;
    float y = p.y - gBase, wear = float(prm.w) / 255.0;
    if (surf == VS_NEONFACE) return vendorPaintedSign(p, V, n, uv, seed, prm, y, wear);
    vec3 alb = vendorColour(aux);
    // lamps: a warm bulb, a cold tube (lit on their own; the light they throw is SV4's)
    if (surf == VS_TUBE || surf == VS_BULB || (prm.z & VPF_EMISSIVE) != 0u) return alb * (surf == VS_TUBE ? 3.0 : 5.0);
    // the room behind an opening: dark, but for a little of its lamp (SV3 paints it)
    if (surf == VS_ROOM) return alb * 0.004 + vec3(0.002);

    float shin = 25.0, specK = 0.12;
    int gm = -1;
    bool relief = false;
    if (surf == VS_STEEL || surf == VS_SIGN) gm = PBR_PAINTED_STEEL;
    else if (surf == VS_CORRUGATED || surf == VS_RUST) { gm = PBR_FACADE_METAL; relief = true; }
    else if (surf == VS_SHUTTER) { gm = PBR_SHUTTER; relief = true; }
    else if (surf == VS_TIMBER || surf == VS_DECK) gm = hf(seed, 5u) < 0.5 ? PBR_WOOD_MID : PBR_WOOD_DARK;
    else if (surf == VS_TILE) gm = PBR_FACADE_TILE;
    else if (surf == VS_BRICK) gm = PBR_FACADE_BRICK;
    else if (surf == VS_TARP || surf == VS_CLOTH || surf == VS_STRIPE) gm = PBR_FABRIC;
    else if (surf == VS_STAINLESS || surf == VS_CHROME) gm = PBR_BRUSHED_METAL;
    else if (surf == VS_RUBBER) gm = PBR_RUBBER_MAT;

    alb *= 0.7;
    if (surf == VS_RUST) alb = mix(alb, vec3(0.19, 0.045, 0.012), 0.4 + 0.4 * wear);
    else if (surf == VS_TIMBER || surf == VS_DECK) alb = mix(alb, vec3(0.25, 0.12, 0.04), 0.65);
    else if (surf == VS_BRICK) alb = mix(alb, vec3(0.25, 0.045, 0.025), 0.6);
    else if (surf == VS_STRIPE) {   // awning stripes across the cloth
        float f = fract(uv.x / 0.56), aa = fwidth(uv.x / 0.56) + 0.002;
        alb = mix(alb, vec3(0.55, 0.53, 0.5), band(f, 0.0, 0.5, aa));
    }
    else if (surf == VS_STAINLESS) { alb = vec3(0.32); specK = 0.45; shin = 60.0; }
    else if (surf == VS_CHROME) { alb = vec3(0.4); specK = 0.7; shin = 120.0; }
    else if (surf == VS_RUBBER) { alb = vec3(0.015); specK = 0.05; }
    else if (surf == VS_GLASS) { alb *= 0.15; specK = 0.6; shin = 200.0; }
    else if (surf == VS_SIGN) alb = vec3(0.012, 0.012, 0.014);
    else if (surf == VS_LAMINATE) specK = 0.2;

    // weathering: blotchy grime, streaks down from the top, dirt splashed up from the ground; cloth fades
    vec2 wq = vec2(uv.x * 0.7, y * 0.3);
    float g = fbm(wq, seed & 0xFFFFu);
    float streak = vnoise(vec2(uv.x * 5.0, y * 0.12), (seed >> 16) | 1u);
    alb *= mix(1.0, mix(0.6, 1.1, g) * (1.0 - 0.35 * smoothstep(0.55, 0.9, streak)), 0.35 + 0.65 * wear);
    alb = mix(alb, vec3(0.03, 0.026, 0.022), (0.15 + 0.45 * wear) * (1.0 - smoothstep(0.0, 0.6, y)));
    if (surf == VS_TARP || surf == VS_CLOTH || surf == VS_STRIPE) alb = mix(alb, vec3(dot(alb, vec3(0.33))), 0.3 * wear);

    Grain gr = Grain(vec3(1.0), n, 1.0, 0.5);
    if (gm >= 0 && pbrLoaded(gm)) {
        vec3 gT, gB;
        modelFrame(p, uv, n, gT, gB);
        vec2 q = vec2(uv.x, -uv.y);   // upright: down the image is down the surface
        gr = relief ? pbrGrainRelief(gm, q, dFdx(q), dFdy(q), gT, -gB, n, V) : pbrGrain(gm, q, dFdx(q), dFdy(q), gT, -gB, n);
        alb *= gr.tint * mix(1.0, gr.ao, 0.7);
        specK *= grainSpec(gr);
    }
    vec3 spec;
    if (relief) reliefLightsOn();   // the corrugations shadow their own lights (pbrGrainRelief)
    vec3 diff = wallLighting(p, gr.n, V, shin, specK, y, spec);
    reliefLightsOff();
    return alb * diff + spec;
}
