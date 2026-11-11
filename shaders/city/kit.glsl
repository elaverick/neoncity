// Part of shaders/city.frag, which includes the parts in order: the vendors' kit.

/* ======================================================================
 * The vendors' kit (src/gen/pmodel.h modules, drawn instanced by
 * src/render/kit_gpu.c through shaders/kit.vert), lit as the street furniture
 * is. params y is the part's material (PMAT_*), z the piece's variation (the
 * instance's part), w its flags (kit_gpu.h: the vendor's wear in bits 0-3,
 * KIT_HEAT near the range); aux the piece of the module; the seed the
 * instance's. uv runs in metres along the surface. kit.vert passes the baked
 * occlusion and wear (128 flat, up on convex edges, down in concave corners)
 * in vLitter.
 *
 * NB3: each material is an authored set (assets/materials/kit_*.json; kitSet,
 * after src/gen/pmodel_kit.c PM_MAT_SET), drawn in the photograph's own colour
 * or its grain under the material's (the file's params.w). Metalness comes
 * from the set's map (orm's alpha, where pbr_pack kept one) times the
 * material's. Glass, glazes, printed faces and food are painted here. Over
 * that, the wear, more as the vendor wears: paint chipped to bare steel on
 * the edges, grime in the corners, runs down upright faces (rust bleeding
 * from the steel, dirt and rain), rust creeping up from the wet ground,
 * grease and soot round the range, smoke-film up near the hood, dirty grout.
 * ==================================================================== */

const uint PMAT_RUST = 0u, PMAT_PLATE = 1u, PMAT_STAINLESS = 2u, PMAT_COPPER = 3u, PMAT_CAST_IRON = 4u, PMAT_PAINTED = 5u,
           PMAT_TILE = 6u, PMAT_RUBBER = 7u, PMAT_ENAMEL = 8u, PMAT_GLASS = 9u, PMAT_BRASS = 10u, PMAT_TIMBER = 11u,
           PMAT_CHALK = 12u, PMAT_CERAMIC = 13u, PMAT_BAMBOO = 14u, PMAT_PAPER = 15u, PMAT_PLASTIC = 16u, PMAT_GALV = 17u,
           PMAT_STEEL = 18u, PMAT_DIAL = 19u, PMAT_RED = 20u, PMAT_BROTH = 21u, PMAT_NOODLE = 22u, PMAT_CURRY = 23u,
           PMAT_RICE = 24u, PMAT_OIL = 25u, PMAT_GINGER = 26u, PMAT_RUST_STEEL = 27u, PMAT_CHEQUER = 28u;   // src/gen/pmodel.h
const uint KIT_MATS = 29u;
const uint KIT_HEAT = 16u;   // flags: the instance stands at the range (kit_gpu.h)

// rgb: the colour (linear) a set's grain goes under (params.w 0) or the painted colour; a: metalness, times the
// set's map where it has one (sets without one read 1 there, and are all non-metals)
const vec4 KIT_BASE[29] = vec4[29](
    vec4(0.20, 0.085, 0.045, 1.0), vec4(0.10, 0.12, 0.11, 1.0), vec4(0.56, 0.56, 0.55, 1.0), vec4(0.62, 0.34, 0.22, 1.0),
    vec4(0.035, 0.034, 0.033, 0.6), vec4(0.17, 0.05, 0.035, 1.0), vec4(0.68, 0.68, 0.64, 0.0), vec4(0.022, 0.022, 0.022, 0.0),
    vec4(0.72, 0.71, 0.67, 0.0), vec4(0.04, 0.05, 0.045, 0.0), vec4(0.74, 0.56, 0.28, 1.0), vec4(0.22, 0.13, 0.07, 0.0),
    vec4(0.03, 0.035, 0.03, 0.0), vec4(0.62, 0.60, 0.54, 0.0), vec4(0.30, 0.22, 0.11, 0.0), vec4(0.58, 0.53, 0.42, 0.0),
    vec4(0.018, 0.018, 0.018, 0.0), vec4(0.40, 0.41, 0.41, 1.0), vec4(0.16, 0.155, 0.15, 1.0), vec4(0.74, 0.72, 0.64, 0.0),
    vec4(0.30, 0.03, 0.02, 1.0), vec4(0.33, 0.18, 0.07, 0.0), vec4(0.62, 0.52, 0.28, 0.0), vec4(0.42, 0.23, 0.04, 0.0),
    vec4(0.78, 0.76, 0.70, 0.0), vec4(0.33, 0.04, 0.01, 0.0), vec4(0.72, 0.43, 0.38, 0.0), vec4(0.30, 0.25, 0.22, 1.0),
    vec4(0.30, 0.30, 0.31, 1.0));
// the painted materials' roughness (the sets carry their own)
const float KIT_ROUGH[29] = float[29](0.82, 0.6, 0.32, 0.4, 0.75, 0.55, 0.2, 0.8, 0.25, 0.05, 0.35, 0.7, 0.9, 0.3, 0.6, 0.9,
                                      0.4, 0.5, 0.5, 0.4, 0.45, 0.08, 0.5, 0.35, 0.6, 0.08, 0.4, 0.6, 0.5);

// The authored set material m is drawn with, or -1 (painted here). Mirrors PM_MAT_SET (src/gen/pmodel_kit.c);
// about a third of the copper pots are the bright one with verdigris.
int kitSet(uint m, uint seed) {
    switch (m) {
    case PMAT_RUST:       return PBR_KIT_RUST_SHEET;
    case PMAT_RUST_STEEL: return PBR_KIT_RUST_STEEL;
    case PMAT_PLATE:      return PBR_KIT_PLATE;
    case PMAT_PAINTED:
    case PMAT_RED:        return PBR_KIT_PAINTED;
    case PMAT_STAINLESS:  return PBR_KIT_STAINLESS;
    case PMAT_COPPER:     return hf(seed, 41u) < 0.33 ? PBR_KIT_COPPER_PATINA : PBR_KIT_COPPER;
    case PMAT_CAST_IRON:  return PBR_KIT_CAST_IRON;
    case PMAT_STEEL:      return PBR_KIT_STEEL;
    case PMAT_GALV:       return PBR_KIT_GALV;
    case PMAT_BRASS:      return PBR_KIT_BRASS;
    case PMAT_TILE:       return PBR_KIT_TILE;
    case PMAT_RUBBER:
    case PMAT_PLASTIC:    return PBR_KIT_RUBBER;
    case PMAT_BAMBOO:     return PBR_KIT_BAMBOO;
    case PMAT_CHEQUER:    return PBR_KIT_CHEQUER;
    case PMAT_TIMBER:     return PBR_VENDOR_PLANKS;
    case PMAT_PAPER:      return PBR_PAPER;
    }
    return -1;
}

float kitLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 shadeKit(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, float occ, float wear) {
    vec3 V = -normalize(rel);
    uint m = min(prm.y, KIT_MATS - 1u);
    vec4 b = KIT_BASE[m];
    float vw = float(prm.w & 15u) / 15.0;   // the vendor's wear: 0 new .. 1 rusted and grimy
    bool heat = (prm.w & KIT_HEAT) != 0u;
    float hgt = p.y - gBase;                 // above the vendor's floor

    // the surface's frame and metres in it: image right along u, image up along v; each instance and piece
    // shifted in the image, so a row of stools or a wall's panels never repeat each other
    vec3 gT, gB;
    modelFrame(p, uv, n, gT, gB);
    vec2 q = vec2(uv.x, -uv.y) + vec2(hf(seed, 3u + prm.z), hf(seed, 5u + prm.z)) * 7.3;
    vec2 qx = dFdx(q), qy = dFdy(q);
    float hx = dFdx(hgt), hy = dFdy(hgt);   // taken here, in uniform control flow

    vec3 base = b.rgb * mix(0.88, 1.1, hf(seed, prm.z + 7u));
    float metal = b.a, rough = KIT_ROUGH[m], ao = 1.0, height = 0.5;
    vec3 nn = n;
    int s = kitSet(m, seed);
    if (s >= 0 && pbrLoaded(s)) {
        int i = s * PBR_MAPS_PER_MATERIAL;
        vec2 tile = pbrTile(s);
        vec2 st = q / tile, sx = qx / tile, sy = qy / tile;
        vec4 k = pbrParams(s);
        vec3 a = textureGrad(pbrMaps[i + PBR_ALBEDO], st, sx, sy).rgb;
        vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, sx, sy).xyz * 2.0 - 1.0;
        vec4 orm = textureGrad(pbrMaps[i + PBR_ORM], st, sx, sy);
        vec3 mean = pbrAlbedoMean(s);
        vec3 tint = mix(vec3(1.0), a / max(mean, vec3(1e-3)), k.x);
        // the photograph's own colour, a little of the instance's variation; or its grain under the material's
        base = k.w > 0.5 ? mean * tint * mix(0.92, 1.06, hf(seed, prm.z + 7u)) : base * tint;
        nn = normalize(n + (gT * tn.x - gB * tn.y) * k.y / max(tn.z, 0.2));   // image up is -gB
        ao = orm.r; rough = orm.g; height = orm.b;
        metal *= orm.a;
    }
    // the painted sets' rust (where the photograph is clearly red over green) browner and duller: their own reads as
    // fresh blood at a stool's scale
    if (m == PMAT_PAINTED || m == PMAT_PLATE) {
        float rusty = smoothstep(0.25, 0.55, (base.r - base.g) / max(base.r, 1e-3));
        base = mix(base, vec3(kitLum(base)) * vec3(1.45, 0.85, 0.55) * 0.8, 0.6 * rusty);
    }
    if (m == PMAT_RED) base =vec3(0.30, 0.03, 0.02) * clamp(kitLum(base) / 0.7, 0.2, 1.1)   // red paint, its chips rust
                              + vec3(0.12, 0.04, 0.02) * (1.0 - smoothstep(0.2, 0.5, kitLum(base) / 0.7));
    bool painted = m == PMAT_PAINTED || m == PMAT_PLATE || m == PMAT_RED;
    bool steel = painted || m == PMAT_RUST || m == PMAT_RUST_STEEL || m == PMAT_STEEL || m == PMAT_GALV
              || m == PMAT_CAST_IRON || m == PMAT_CHEQUER;
    bool bright = m == PMAT_STAINLESS || m == PMAT_COPPER || m == PMAT_BRASS;
    bool food = m >= PMAT_BROTH && m <= PMAT_GINGER;
    const vec3 RUST = vec3(0.13, 0.045, 0.02), GRIME = vec3(0.035, 0.03, 0.024);

    // edges: paint and plate chipped through to steel (rusting as the vendor wears), in ragged chips; bright metal
    // rubbed brighter and smoother
    float edge = smoothstep(0.58, 0.9, wear), corner = smoothstep(0.45, 0.15, wear);
    float ragged = vnoise(q * 31.0, 0x6B1u);
    if (painted) {
        float chip = smoothstep(0.5, 0.62, edge * mix(0.75, 1.25, vw) + 0.45 * (ragged - 0.5));
        vec3 bare = mix(vec3(0.32, 0.31, 0.30), RUST * 1.4, smoothstep(0.3, 0.8, vw + 0.3 * (ragged - 0.5)));
        float bareMetal = 1.0 - smoothstep(0.3, 0.8, vw);
        base = mix(base, bare, chip); metal = mix(metal, bareMetal, chip); rough = mix(rough, mix(0.4, 0.85, 1.0 - bareMetal), chip);
    } else if (bright) {
        base *= 1.0 + 0.3 * edge; rough *= 1.0 - 0.4 * edge;
    } else if (!food && m != PMAT_GLASS) {
        base *= 1.0 + 0.15 * edge;
    }

    // corners and crevices: grime packed in (dull, not metal)
    float g = corner * mix(0.6, 1.0, vw);
    base = mix(base, GRIME, 0.55 * g); metal *= 1.0 - 0.7 * g; rough = mix(rough, 0.9, 0.6 * g);

    // runs down upright faces: dirt and, from steel, rust bled down its fall; more as the vendor wears
    float up = 1.0 - smoothstep(0.5, 0.8, abs(n.y));
    if (up > 0.0 && !food && m != PMAT_GLASS) {
        float run = fbm(vec2(uv.x * 9.0, p.y * 0.9) + float(seed & 255u), 0x52Cu);
        run = smoothstep(0.52, 0.78, run) * up * mix(0.25, 0.85, vw);
        vec3 col = steel || m == PMAT_TILE || m == PMAT_STAINLESS ? mix(GRIME, RUST, steel ? 0.7 : 0.4) : GRIME;
        base = mix(base, col, 0.55 * run); metal *= 1.0 - 0.6 * run; rough = mix(rough, 0.85, 0.5 * run);
    }

    // rust creeping up from the wet ground over steel, knee-high at most (an authored decal: its bottom row
    // the ground)
    if (steel && hgt < 0.45 && up > 0.0 && pbrLoaded(PBR_KIT_RUST_CREEP)) {
        vec2 tile = pbrTile(PBR_KIT_RUST_CREEP);
        float reach = mix(0.15, 0.42, vw) * mix(0.8, 1.2, hf(seed, 13u));
        vec2 st = vec2(q.x / tile.x, 1.0 - hgt / reach);
        vec4 d = textureGrad(pbrMaps[PBR_KIT_RUST_CREEP * PBR_MAPS_PER_MATERIAL + PBR_ALBEDO], st,
                             vec2(qx.x / tile.x, -hx / reach), vec2(qy.x / tile.x, -hy / reach));
        float a = d.a * up * step(0.0, st.y) * step(st.y, 1.0);
        base = mix(base, d.rgb * 0.9, a); metal *= 1.0 - a; rough = mix(rough, 0.85, a);
    }

    // at the range: grease spattered on what faces up (darker, glossier), soot rising up from the flames on
    // what stands over them (the pots' feet, the grates)
    if (heat && !food) {
        float spat = smoothstep(0.55, 0.8, vnoise(q * 22.0, 0x6EAu) * 0.6 + vnoise(q * 5.0, 0x6EBu) * 0.6) * smoothstep(0.2, 0.7, n.y);
        float greasy = (0.35 + 0.5 * vw) * spat;
        base = mix(base, vec3(0.05, 0.03, 0.012), 0.6 * greasy); rough = mix(rough, 0.25, greasy); metal *= 1.0 - 0.5 * greasy;
        float soot = (1.0 - smoothstep(0.95, 1.2, hgt)) * smoothstep(0.88, 0.95, hgt) * mix(0.5, 0.9, vw);
        base = mix(base, vec3(0.012, 0.011, 0.01), soot * up); rough = mix(rough, 0.95, soot * up); metal *= 1.0 - soot * up;
    }
    // the smoke's film up under the hood and on what is up there, thicker the higher
    float film = smoothstep(1.7, 2.4, hgt) * mix(0.25, 0.6, vw);
    base = mix(base, vec3(0.03, 0.022, 0.014), film); metal *= 1.0 - 0.5 * film;

    // dirty grout between the tiles (their height map's joints)
    if (m == PMAT_TILE) {
        float grout = 1.0 - smoothstep(0.25, 0.45, height);
        base = mix(base, mix(vec3(0.16, 0.13, 0.09), vec3(0.05, 0.035, 0.02), vw), grout * mix(0.5, 0.95, vw));
        rough = mix(rough, 0.9, grout);
    }

    ao *= mix(0.2, 1.0, occ);
    float wet = WETNESS * (1.0 - shelterAt(rel));
    rough = mix(rough, rough * 0.45, wet);
    base *= mix(1.0, 0.8, wet * (1.0 - metal));
    metal = clamp(metal, 0.0, 1.0);
    vec3 alb = base * (1.0 - metal);
    float f0 = mix(m == PMAT_GLASS ? 0.06 : 0.04, 0.85, metal);
    vec3 tint = mix(vec3(1.0), clamp(base / max(max(base.r, max(base.g, base.b)), 1e-3), 0.0, 1.0), metal);
    return litterLight(p, nn, V, alb, rough, f0, tint, ao);
}
