// Part of shaders/city.frag, which includes the parts in order: street vendors' shells.

/* ======================================================================
 * Street vendors (src/city/mesh_vendor.c, src/gen/vendor.h; roadmap SV2):
 * the structure of each stall, kiosk, lean-to, container, market stall and
 * barrow, in the chunk's base layer. params y the surface (VS_*), z its flags
 * (VPF_*), w its wear; aux its colour (sRGB 0xRRGGBB, the vendor's palette
 * resolved) | its part's role << 24; the normal's w its part's top (city.vert
 * passes it in vLitter.x). uv are metres along the part's faces (along, then
 * up on an upright face). Each surface is an authored material (pbrGrain):
 * flake board and weathered planks in their own colour, painted corrugated
 * iron rusting through and rusted sheet in relief, painted steel, canvas and
 * tarp in the vendor's. Wear brings grime: blotches, dirt run down from each
 * part's top edge (an authored decal), dirt splashed up from the ground. The
 * rooms behind the openings are painted by vendor_room.glsl (SV3), or shut;
 * lamps glow, but light nothing yet (SV4). A painted sign's face (VS_NEON, unlit) is hand-lettered:
 * the neon's own glyphs (tubeDist, signs.glsl) as brush strokes. Lit neon is
 * MAT_NEON (shadeNeon).
 * ==================================================================== */

const uint VS_STEEL = 0u, VS_CORRUGATED = 1u, VS_RUST = 2u, VS_TIMBER = 3u, VS_DECK = 4u, VS_TILE = 5u, VS_BRICK = 6u,
           VS_TARP = 7u, VS_STRIPE = 8u, VS_CLOTH = 9u, VS_STAINLESS = 10u, VS_LAMINATE = 11u, VS_CHROME = 12u,
           VS_RUBBER = 13u, VS_GLASS = 14u, VS_NEONFACE = 17u, VS_TUBE = 18u, VS_BULB = 19u, VS_ROOM = 20u,
           VS_SIGN = 21u, VS_SHUTTER = 22u, VS_CHIPBOARD = 25u;
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

float vLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

/* top: the part's top over the vendor's floor (m; 0 for none), where the grime runs down from. */
vec3 shadeVendor(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, uint aux, float top) {
    vec3 V = -normalize(rel);
    uint surf = prm.y;
    float y = p.y - gBase, wear = float(prm.w) / 255.0;
    // the grime's image coordinates and their derivatives, while control flow is uniform
    vec2 dq = vec2(uv.x / 2.0 + hf(seed, 14u), top - y);
    vec2 dqx = dFdx(dq), dqy = dFdy(dq);
    float rpx = max(length(dFdx(uv)), length(dFdy(uv)));   // a room's metres a pixel
    if (surf == VS_NEONFACE) return vendorPaintedSign(p, V, n, uv, seed, prm, y, wear);
    vec3 pal = vendorColour(aux);
    if (surf == VS_ROOM) {
        // the painted room behind an opening (vendor_room.glsl), lit by its lamp and what reaches it from outside
        if ((aux >> 31) == 0u) {
            vec3 sp;
            vec3 outside = wallLighting(p, n, V, 10.0, 0.0, y, sp) * 0.5;
            return vendorRoom(uv, -V, n, seed, prm, aux, top * 0.5, y - uv.y, rpx, length(rel), outside);
        }
        // shut: a roller shutter down, a tarp tied over a market's gap, a board over a recess or cabinet
        uint kind = (aux >> 18) & 7u;
        surf = kind == VO_BACKCLOTH ? VS_TARP : kind == VO_RECESS || kind == VO_CABINET ? VS_CHIPBOARD : VS_SHUTTER;
        pal = vPal((aux >> 21) & 15u, 0u);
        wear = float((aux >> 27) & 7u) / 7.0;
        prm.z = 0u;
    }
    // lamps: a warm bulb, a cold tube (lit on their own; the light they throw is SV4's)
    if (surf == VS_TUBE || surf == VS_BULB || (prm.z & VPF_EMISSIVE) != 0u) return pal * (surf == VS_TUBE ? 3.0 : 5.0);

    /* What it is made of. Cheap stuff, even where the district is wealthy: flake board, weathered
     * planks (fresh pine on the cleaner stalls), corrugated iron painted in the vendor's colour and
     * rusting through as it wears, or rusted almost bare. Board and timber keep their own colour
     * (the photograph's: tint x mean); paint and cloth take the vendor's. */
    float shin = 25.0, specK = 0.12;
    int gm = -1;
    bool relief = false, own = false;
    uint look = 0u;   // 1 painted sheet rusting through, 2 rust with flecks of paint, 3 board
    if (surf == VS_STEEL || surf == VS_SIGN) gm = PBR_PAINTED_STEEL;
    else if (surf == VS_CORRUGATED) {
        relief = true;
        if (wear > 0.35 && pbrLoaded(PBR_VENDOR_SHEET_WORN)) { gm = PBR_VENDOR_SHEET_WORN; look = 1u; }
        else gm = PBR_FACADE_METAL;
    }
    else if (surf == VS_RUST) { gm = PBR_VENDOR_SHEET_RUST; relief = true; look = 2u; }
    else if (surf == VS_SHUTTER) { gm = PBR_SHUTTER; relief = true; }
    else if (surf == VS_TIMBER || surf == VS_DECK) {
        gm = wear < 0.3 && hf(seed, 5u) < 0.6 ? PBR_VENDOR_PINE : hf(seed, 6u) < 0.5 ? PBR_VENDOR_PLANKS : PBR_VENDOR_PLANKS_GREY;
        own = true;
    }
    else if (surf == VS_CHIPBOARD) { gm = hf(seed, 7u) < 0.5 ? PBR_VENDOR_OSB : PBR_VENDOR_FLAKEBOARD; own = true; look = 3u; }
    else if (surf == VS_TILE) gm = PBR_FACADE_TILE;
    else if (surf == VS_BRICK) gm = PBR_FACADE_BRICK;
    else if (surf == VS_TARP || surf == VS_CLOTH || surf == VS_STRIPE) gm = PBR_FABRIC;
    else if (surf == VS_STAINLESS || surf == VS_CHROME) gm = PBR_BRUSHED_METAL;
    else if (surf == VS_RUBBER) gm = PBR_RUBBER_MAT;

    vec3 alb = pal * 0.7;
    if (surf == VS_STRIPE) {   // awning stripes across the cloth
        float f = fract(uv.x / 0.56), aa = fwidth(uv.x / 0.56) + 0.002;
        alb = mix(alb, vec3(0.55, 0.53, 0.5), band(f, 0.0, 0.5, aa));
    }
    else if (surf == VS_STAINLESS) { alb = vec3(0.32); specK = 0.45; shin = 60.0; }
    else if (surf == VS_CHROME) { alb = vec3(0.4); specK = 0.7; shin = 120.0; }
    else if (surf == VS_RUBBER) { alb = vec3(0.015); specK = 0.05; }
    else if (surf == VS_GLASS) { alb *= 0.15; specK = 0.6; shin = 200.0; }
    else if (surf == VS_SIGN) alb = vec3(0.012, 0.012, 0.014);
    else if (surf == VS_LAMINATE) specK = 0.2;

    Grain gr = Grain(vec3(1.0), n, 1.0, 0.5);
    if (gm >= 0 && pbrLoaded(gm)) {
        vec3 gT, gB;
        modelFrame(p, uv, n, gT, gB);
        vec2 q = vec2(uv.x, -uv.y);   // upright: down the image is down the surface
        gr = relief ? pbrGrainRelief(gm, q, dFdx(q), dFdy(q), gT, -gB, n, V) : pbrGrain(gm, q, dFdx(q), dFdy(q), gT, -gB, n);
        vec3 mean = pbrAlbedoMean(gm), raw = gr.tint * mean;   // the photograph's own albedo
        // where the sheet has gone furthest: patches a few metres across, so the image's own rust (it
        // repeats every 0.8 m) shows only in some of them, more as the vendor wears
        float patchy = smoothstep(0.35, 0.7, fbm(vec2(uv.x, y) * 0.45, (seed >> 8) | 3u) + 0.5 * (wear - 0.5));
        if (look == 1u) {
            // the vendor's paint where the photograph's paint is, rust where that has clearly gone (the
            // photograph's own paint, half-rusted, never shows through: it is yellow); its shading faintly
            float rust = smoothstep(0.3, 0.5, (raw.r - raw.g) / max(raw.r, 1e-3)) * patchy;
            vec3 rustCol = vec3(0.17, 0.065, 0.028) * clamp(vLum(raw) / 0.1, 0.5, 1.4);
            alb = mix(alb * mix(1.0, clamp(vLum(raw) / max(vLum(mean), 1e-3), 0.7, 1.15), 0.25), rustCol, rust);
        } else if (look == 2u) {
            // rust, darker and lighter in patches, and a few flecks of the old paint (the photograph's pale
            // blue) in the vendor's colour, faded
            float fleck = smoothstep(0.04, 0.2, raw.b - raw.r) * (1.0 - patchy);
            alb = mix(raw * mix(0.75, 1.15, patchy), mix(alb, vec3(vLum(alb)), 0.4) * 0.6, fleck * 0.8);
        } else if (own) {
            alb = raw * mix(vec3(1.0), clamp(pal / max(vLum(pal), 0.05), 0.0, 2.0), 0.15);   // a little stain
            if (look == 3u && hf(seed, 11u) < 0.3 * (1.0 - wear))   // some vendors paint their board: the flakes faintly through it
                alb = pal * 0.7 * mix(1.0, clamp(vLum(gr.tint), 0.7, 1.2), 0.3);
        } else alb *= gr.tint;
        alb *= mix(1.0, gr.ao, 0.7);
        specK *= grainSpec(gr);
    }
    // weathering: board greys and swells, everything gathers blotchy grime, cloth fades
    if (look == 3u) alb = mix(alb, vec3(vLum(alb)) * 0.8, 0.5 * wear);
    float g = fbm(vec2(uv.x * 0.7, y * 0.3), seed & 0xFFFFu);
    alb *= mix(1.0, mix(0.65, 1.1, g), 0.3 + 0.6 * wear);
    if (surf == VS_TARP || surf == VS_CLOTH || surf == VS_STRIPE) alb = mix(alb, vec3(vLum(alb)), 0.3 * wear);
    // dirt run down the walls from each part's top edge (an authored decal, its alpha the dirt), more as it wears
    int dm = hf(seed, 13u) < 0.5 ? PBR_GRIME_STREAKS_A : PBR_GRIME_STREAKS_B;
    if (top > 0.05 && abs(n.y) < 0.6 && dq.y > 0.0 && dq.y < 0.98 && pbrLoaded(dm)) {
        vec2 tile = pbrTile(dm);
        vec4 d = textureGrad(pbrMaps[dm * PBR_MAPS_PER_MATERIAL + PBR_ALBEDO], dq / tile, dqx / tile, dqy / tile);
        float k = d.a * (0.2 + 0.6 * wear) * (1.0 - smoothstep(0.7, 0.98, dq.y));
        alb *= mix(vec3(1.0), 0.35 + 4.0 * d.rgb, k);   // darkened, a little of the dirt's own colour
    }
    // and splashed up from the ground
    alb = mix(alb, vec3(0.03, 0.026, 0.022), (0.15 + 0.45 * wear) * (1.0 - smoothstep(0.0, 0.6, y)));

    vec3 spec;
    if (relief) reliefLightsOn();   // the corrugations shadow their own lights (pbrGrainRelief)
    vec3 diff = wallLighting(p, gr.n, V, shin, specK, y, spec);
    reliefLightsOff();
    return alb * diff + spec;
}
