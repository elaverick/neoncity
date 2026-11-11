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
 * material's. Glass, glazes, printed faces and food are painted here.
 *
 * NB4b (the user's rules, after the references): the outside is one solid
 * coat of paint per vendor (KIT_COAT: a body colour, PMAT_PLATE, and a trim
 * colour, PMAT_PAINTED; the flags' bits 5-7), chipped to steel only inside a
 * few patches; the kitchen is stainless and clean; the hood (PMAT_RUST) is
 * bare metal, rusted, galvanised or plain steel by the vendor's affluence
 * (bits 8-9: src/gen/noodle_bar.c nb_finish). The wear over that, more as the
 * vendor wears: grime in the corners, streaks down the hood from its seams,
 * faint dirt low on the paint, rust creeping a little up bare steel from the
 * wet ground, grease and soot at the range, a film of smoke under the hood
 * and over the extractor, dirty grout. Never rust on the paint or in the
 * kitchen. The A-frame's frame is the trim coat or, for some vendors, bare
 * timber (KIT_FLAG_BARE_WOOD, bit 10, on the A-frame alone).
 * ==================================================================== */

const uint PMAT_RUST = 0u, PMAT_PLATE = 1u, PMAT_STAINLESS = 2u, PMAT_COPPER = 3u, PMAT_CAST_IRON = 4u, PMAT_PAINTED = 5u,
           PMAT_TILE = 6u, PMAT_RUBBER = 7u, PMAT_ENAMEL = 8u, PMAT_GLASS = 9u, PMAT_BRASS = 10u, PMAT_TIMBER = 11u,
           PMAT_CHALK = 12u, PMAT_CERAMIC = 13u, PMAT_BAMBOO = 14u, PMAT_PAPER = 15u, PMAT_PLASTIC = 16u, PMAT_GALV = 17u,
           PMAT_STEEL = 18u, PMAT_DIAL = 19u, PMAT_RED = 20u, PMAT_BROTH = 21u, PMAT_NOODLE = 22u, PMAT_CURRY = 23u,
           PMAT_RICE = 24u, PMAT_OIL = 25u, PMAT_GINGER = 26u, PMAT_RUST_STEEL = 27u, PMAT_CHEQUER = 28u, PMAT_LINING = 29u;   // src/gen/pmodel.h
const uint KIT_MATS = 30u;
const uint KIT_HEAT = 16u;   // flags: the instance stands at the range (kit_gpu.h)
const uint KIT_HOOD_RUST = 0u, KIT_HOOD_GALV = 1u, KIT_HOOD_STEEL = 2u;
const uint KIT_BARE_WOOD = 1024u;   // flags: the A-frame's frame bare timber, not the trim coat (src/city/city.h)   // flags >> 8 (src/gen/noodle_bar.h NB_HOOD_*)

// the vendors' coats of paint (linear): body (counter fronts, walls, the column), trim (posts, fascia, stools,
// the A-frame); one pair per vendor (flags >> 5: src/gen/noodle_bar.h NB_COATS)
const vec3 KIT_GREEN = vec3(0.030, 0.075, 0.050), KIT_CREAM = vec3(0.50, 0.46, 0.36), KIT_BLACK = vec3(0.018, 0.018, 0.020);
const vec3 KIT_COAT[16] = vec3[16](
    KIT_GREEN, KIT_CREAM,                                 // bottle green, cream
    vec3(0.16, 0.030, 0.020), KIT_BLACK,                  // oxide red, near-black
    KIT_CREAM, KIT_GREEN,                                 // cream, bottle green
    vec3(0.022, 0.035, 0.085), KIT_CREAM,                 // navy, cream
    KIT_BLACK, vec3(0.20, 0.025, 0.018),                  // near-black, signal red
    vec3(0.025, 0.090, 0.085), KIT_BLACK,                 // teal, near-black
    vec3(0.060, 0.140, 0.070), KIT_BLACK,                 // mid green, near-black
    vec3(0.085, 0.018, 0.022), KIT_CREAM);                // maroon, cream
const vec3 KIT_STEEL_SHEET = vec3(0.12, 0.12, 0.125);   // a plain steel hood's colour under its sheet's grain

// rgb: the colour (linear) a set's grain goes under (params.w 0) or the painted colour; a: metalness, times the
// set's map where it has one (sets without one read 1 there, and are all non-metals)
const vec4 KIT_BASE[30] = vec4[30](
    vec4(0.20, 0.085, 0.045, 1.0), vec4(0.10, 0.12, 0.11, 0.0), vec4(0.56, 0.56, 0.55, 1.0), vec4(0.62, 0.34, 0.22, 1.0),
    vec4(0.035, 0.034, 0.033, 0.6), vec4(0.17, 0.05, 0.035, 0.0), vec4(0.68, 0.68, 0.64, 0.0), vec4(0.022, 0.022, 0.022, 0.0),
    vec4(0.72, 0.71, 0.67, 0.0), vec4(0.04, 0.05, 0.045, 0.0), vec4(0.74, 0.56, 0.28, 1.0), vec4(0.22, 0.13, 0.07, 0.0),
    vec4(0.03, 0.035, 0.03, 0.0), vec4(0.62, 0.60, 0.54, 0.0), vec4(0.30, 0.22, 0.11, 0.0), vec4(0.58, 0.53, 0.42, 0.0),
    vec4(0.018, 0.018, 0.018, 0.0), vec4(0.40, 0.41, 0.41, 1.0), vec4(0.16, 0.155, 0.15, 1.0), vec4(0.74, 0.72, 0.64, 0.0),
    vec4(0.30, 0.03, 0.02, 1.0), vec4(0.33, 0.18, 0.07, 0.0), vec4(0.62, 0.52, 0.28, 0.0), vec4(0.42, 0.23, 0.04, 0.0),
    vec4(0.78, 0.76, 0.70, 0.0), vec4(0.33, 0.04, 0.01, 0.0), vec4(0.72, 0.43, 0.38, 0.0), vec4(0.30, 0.25, 0.22, 1.0),
    vec4(0.30, 0.30, 0.31, 1.0), vec4(0.56, 0.56, 0.55, 1.0));
// the painted materials' roughness (the sets carry their own)
const float KIT_ROUGH[30] = float[30](0.82, 0.6, 0.32, 0.4, 0.75, 0.55, 0.2, 0.8, 0.25, 0.05, 0.35, 0.7, 0.9, 0.3, 0.6, 0.9,
                                      0.4, 0.5, 0.5, 0.4, 0.45, 0.08, 0.5, 0.35, 0.6, 0.08, 0.4, 0.6, 0.5, 0.35);

// The authored set material m is drawn with, or -1 (painted here). Mirrors PM_MAT_SET (src/gen/pmodel_kit.c);
// about a third of the copper pots are the bright one with verdigris.
int kitSet(uint m, uint seed) {
    switch (m) {
    case PMAT_RUST:       return PBR_KIT_RUST_SHEET;
    case PMAT_RUST_STEEL: return PBR_KIT_RUST_STEEL;
    case PMAT_PLATE:
    case PMAT_PAINTED:    return PBR_KIT_PAINT;
    case PMAT_RED:        return PBR_KIT_PAINTED;
    case PMAT_STAINLESS:  return PBR_KIT_STAINLESS;
    case PMAT_LINING:     return PBR_KIT_LINING;
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

// Round beads scattered one to a cell (cells per metre; size their radius in cells, 0..0.5): 1 inside one,
// soft at its rim; about 60% of the cells hold one.
float kitBeads(vec2 f, float cells, float size, uint salt) {
    vec2 c = f * cells, i = floor(c), g = fract(c);
    uint h = hash2i(salt, int(i.x), int(i.y));
    vec2 o = vec2(hfloat(h), hfloat(hash_combine(h, 1u))) * (1.0 - 2.0 * size) + size;
    float r = size * (0.45 + 0.55 * hfloat(hash_combine(h, 2u)));
    return step(hfloat(hash_combine(h, 3u)), 0.6) * (1.0 - smoothstep(0.7 * r, r, length(g - o)));
}

// The food (NB5), painted simply: broth with beads of fat and flecks of spring onion; noodles in strands
// lying two ways, dark between them; curry with its potato and meat and its oil pooling; rice grains; chilli
// oil with its flakes, ginger in slices (both seen through their jars' glass). f: metres over the surface;
// foot: a pixel's size there (the fine patterns fade to their average past it).
void kitFood(uint m, vec2 f, float foot, inout vec3 base, inout float rough) {
    if (m == PMAT_BROTH) {
        base = mix(vec3(0.15, 0.065, 0.02), vec3(0.30, 0.15, 0.045), fbm(f * 7.0, 0xB07u));
        float fine = 1.0 - smoothstep(0.004, 0.012, foot);
        float fat = kitBeads(f, 45.0, 0.3, 0xFA7u) * fine;
        float onion = kitBeads(f + 3.1, 16.0, 0.16, 0x0A1u) * step(0.75, vnoise(f * 5.0, 0x0A2u)) * fine;
        base = mix(base, vec3(0.52, 0.34, 0.09), 0.6 * fat);
        base = mix(base, vec3(0.10, 0.22, 0.04), onion);
        rough = mix(0.07, 0.03, fat);
    } else if (m == PMAT_NOODLE) {
        vec2 w = f + 0.012 * vec2(fbm(f * 25.0, 0x9D1u), fbm(f * 25.0 + 5.0, 0x9D2u));
        const float FR = 170.0;
        float aa = 1.0 - smoothstep(0.15, 0.5, foot * FR);
        float s1 = 0.5 + 0.5 * sin(6.2832 * FR * dot(w, vec2(0.8, 0.6)));
        float s2 = 0.5 + 0.5 * sin(6.2832 * FR * dot(w, vec2(-0.5, 0.87)) + 1.3);
        float strand = mix(s1, s2, step(0.5, vnoise(f * 18.0, 0x9D3u)));
        float lit = mix(0.7, smoothstep(0.1, 0.6, strand), aa);
        base = vec3(0.60, 0.46, 0.19) * mix(0.5, 1.05, lit) * mix(0.88, 1.06, vnoise(f * 40.0, 0x9D4u));
        rough = mix(0.55, 0.3, lit);
    } else if (m == PMAT_CURRY) {
        base = mix(vec3(0.22, 0.08, 0.018), vec3(0.40, 0.19, 0.035), fbm(f * 14.0, 0xC01u));
        float potato = kitBeads(f, 28.0, 0.4, 0xC02u), meat = kitBeads(f + 1.7, 24.0, 0.35, 0xC03u) * (1.0 - potato);
        base = mix(base, vec3(0.52, 0.36, 0.11), 0.75 * potato);
        base = mix(base, vec3(0.12, 0.045, 0.018), 0.7 * meat);
        float oil = smoothstep(0.58, 0.78, vnoise(f * 9.0, 0xC04u)) * (1.0 - potato);
        base = mix(base, vec3(0.42, 0.10, 0.015), 0.5 * oil);
        rough = mix(0.4, 0.07, oil);
    } else if (m == PMAT_RICE) {
        float aa = 1.0 - smoothstep(0.002, 0.006, foot);
        float grain = mix(0.6, kitBeads(f, 160.0, 0.45, 0x51Cu), aa);
        base = vec3(0.70, 0.68, 0.62) * mix(0.72, 1.0, grain);
        rough = 0.5;
    } else if (m == PMAT_OIL) {
        // deep red-brown oil, its flakes settled through it, glossy behind the glass
        float aa = 1.0 - smoothstep(0.002, 0.006, foot);
        float flake = kitBeads(f, 110.0, 0.25, 0x0E1u) * aa;
        base = mix(vec3(0.11, 0.022, 0.007), vec3(0.24, 0.055, 0.012), fbm(f * 30.0, 0x0E2u));
        base = mix(base, vec3(0.05, 0.012, 0.004), 0.7 * flake);
        rough = 0.06;
    } else if (m == PMAT_GINGER) {
        // pale pink slices packed in their brine, a little darker at their edges
        float aa = 1.0 - smoothstep(0.003, 0.008, foot);
        float a = kitBeads(f, 55.0, 0.45, 0x61Eu), b = kitBeads(f + 0.009, 55.0, 0.45, 0x61Fu);
        float slice = mix(0.5, max(a, 0.8 * b), aa);
        base = mix(vec3(0.46, 0.27, 0.22), vec3(0.74, 0.52, 0.45), slice);
        rough = 0.1;
    }
}

vec3 shadeKit(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm, float occ, float wear) {
    vec3 V = -normalize(rel);
    uint m = min(prm.y, KIT_MATS - 1u);
    float vw = float(prm.w & 15u) / 15.0;   // the vendor's wear: 0 new .. 1 rusted and grimy
    bool heat = (prm.w & KIT_HEAT) != 0u;
    uint coat = (prm.w >> 5) & 7u, hood = (prm.w >> 8) & 3u;
    float hgt = p.y - gBase;                 // above the vendor's floor
    // the hood's metal: rusted sheet, galvanised (as the duct) or plain steel sheet, sound
    bool isHood = m == PMAT_RUST, steelHood = isHood && hood == KIT_HOOD_STEEL;
    if (isHood && hood == KIT_HOOD_GALV) m = PMAT_GALV;
    if (steelHood) m = PMAT_STEEL;
    if (m == PMAT_PAINTED && (prm.w & KIT_BARE_WOOD) != 0u) m = PMAT_TIMBER;
    vec4 b = KIT_BASE[m];
    if (steelHood) b.rgb = KIT_STEEL_SHEET;
    if (m == PMAT_PLATE) b.rgb = KIT_COAT[coat * 2u];
    if (m == PMAT_PAINTED) b.rgb = KIT_COAT[coat * 2u + 1u];

    // the surface's frame and metres in it: image right along u, image up along v; each instance and piece
    // shifted in the image, so a row of stools or a wall's panels never repeat each other
    vec3 gT, gB;
    modelFrame(p, uv, n, gT, gB);
    vec2 q = vec2(uv.x, -uv.y) + vec2(hf(seed, 3u + prm.z), hf(seed, 5u + prm.z)) * 7.3;
    vec2 qx = dFdx(q), qy = dFdy(q);
    float hx = dFdx(hgt), hy = dFdy(hgt);   // taken here, in uniform control flow
    vec2 pw = mod(p.xz, 64.0);                // the food's metres on what faces up (world xz, kept small)
    float pfoot = max(length(dFdx(p.xz)), length(dFdy(p.xz)));

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
    // the food (NB5): on what faces up in world xz (a lathe's metres swirl round its middle), else its own
    if (m >= PMAT_BROTH && m <= PMAT_GINGER) {
        bool top = abs(n.y) > 0.6;
        kitFood(m, top ? pw + vec2(float(seed & 255u) * 0.37) : q, top ? pfoot : max(length(qx), length(qy)), base, rough);
    }
    // a plain steel hood weathered dull, mill scale and grime over it: not the galvanised one's sky-bright sheen
    if (steelHood) { rough = max(rough, 0.72); metal *= 0.45; }
    // the red paint's set's rust (where the photograph is clearly red over green) browner and duller: its own reads
    // as fresh blood at a valve wheel's scale
    if (m == PMAT_RED) {
        float rusty = smoothstep(0.25, 0.55, (base.r - base.g) / max(base.r, 1e-3));
        base = mix(base, vec3(kitLum(base)) * vec3(1.45, 0.85, 0.55) * 0.8, 0.6 * rusty);
    }
    if (m == PMAT_RED) base =vec3(0.30, 0.03, 0.02) * clamp(kitLum(base) / 0.7, 0.2, 1.1)   // red paint, its chips rust
                              + vec3(0.12, 0.04, 0.02) * (1.0 - smoothstep(0.2, 0.5, kitLum(base) / 0.7));
    bool coated = m == PMAT_PAINTED || m == PMAT_PLATE;
    bool painted = coated || m == PMAT_RED;
    bool bright = m == PMAT_STAINLESS || m == PMAT_LINING || m == PMAT_COPPER || m == PMAT_BRASS;
    bool food = m >= PMAT_BROTH && m <= PMAT_GINGER;
    // the kitchen's and the table's things: clean but for a trace of grime, grease at the range, faint streaks
    bool clean = bright || food || m == PMAT_TILE || m == PMAT_CHEQUER || m == PMAT_CAST_IRON || m == PMAT_ENAMEL
              || m == PMAT_GLASS || m == PMAT_CERAMIC || m == PMAT_BAMBOO || m == PMAT_DIAL || m == PMAT_PAPER;
    bool bare = !isHood && (m == PMAT_STEEL || m == PMAT_GALV || m == PMAT_RUST_STEEL);   // bare steel outside
    const vec3 RUST = vec3(0.13, 0.045, 0.02), GRIME = vec3(0.035, 0.03, 0.024);

    // edges: the paint chipped to steel only inside a few low-frequency patches (more of them as the vendor wears,
    // never most of a surface), a fleck or two off the faces there; bright metal rubbed brighter and smoother
    float edge = smoothstep(0.58, 0.9, wear), corner = smoothstep(0.45, 0.15, wear);
    float ragged = vnoise(q * 31.0, 0x6B1u);
    if (painted) {
        float patchN = vnoise(q * 1.7 + vec2(float(seed & 63u), float(prm.z)), 0x6B3u);
        float inPatch = m == PMAT_RED ? 1.0 : smoothstep(mix(0.82, 0.70, vw), mix(0.88, 0.76, vw), patchN);
        // only the sharpest edges: the merged body has no support loops, so its baked wear spreads across a face
        float chip = smoothstep(0.62, 0.72, smoothstep(0.78, 0.95, wear) * mix(0.85, 1.15, vw) + 0.4 * (ragged - 0.5));
        // a sparse fleck or two off the faces, small (NB4b preview: denser read as pale blotches)
        chip = inPatch * max(chip, smoothstep(0.9, 0.95, vnoise(q * 70.0, 0x6B4u)) * mix(0.3, 0.8, vw));
        // bare steel under it, dark, dulled towards brown only on the poorest (never rusted through)
        float dull = 0.5 * smoothstep(0.6, 1.0, vw);
        base = mix(base, mix(vec3(0.16, 0.155, 0.15), vec3(0.11, 0.07, 0.045), dull), chip);
        metal = mix(metal, 0.45 - 0.3 * dull, chip); rough = mix(rough, mix(0.7, 0.85, dull), chip);   // dull: bright steel mirrored the sky
    } else if (bright) {
        base *= 1.0 + 0.3 * edge; rough *= 1.0 - 0.4 * edge;
    } else if (!food && m != PMAT_GLASS) {
        base *= 1.0 + 0.15 * edge;
    }

    // corners and crevices: grime packed in (dull, not metal); a trace in the kitchen
    float g = corner * (clean ? 0.2 : mix(0.35, 0.75, vw));
    base = mix(base, GRIME, 0.5 * g); metal *= 1.0 - 0.6 * g; rough = mix(rough, 0.9, 0.5 * g);

    // streaks down upright faces: down the hood from its seams (rust from a rusted one), faint dirt low on the
    // paint and bare steel, the faintest on stainless and tile; none on the rest
    float up = 1.0 - smoothstep(0.5, 0.8, abs(n.y));
    float streak = isHood ? mix(0.25, 0.6, vw)
                 : painted ? mix(0.08, 0.3, vw) * (1.0 - smoothstep(0.4, 1.3, hgt))
                 : bare ? mix(0.1, 0.35, vw)
                 : m == PMAT_STAINLESS || m == PMAT_LINING || m == PMAT_TILE ? mix(0.05, 0.18, vw) : 0.0;
    if (up > 0.0 && streak > 0.0) {
        float run = fbm(vec2(uv.x * 9.0, p.y * 0.9) + float(seed & 255u), 0x52Cu);
        run = smoothstep(0.52, 0.78, run) * up * streak;
        vec3 col = isHood && hood == KIT_HOOD_RUST ? mix(GRIME, RUST, 0.7) : bare ? mix(GRIME, RUST, 0.35 * vw) : GRIME;
        base = mix(base, col, 0.55 * run); metal *= 1.0 - 0.6 * run; rough = mix(rough, 0.85, 0.5 * run);
    }

    // rust creeping a little up bare steel from the wet ground (an authored decal: its bottom row the ground);
    // never up the paint or in the kitchen
    if (bare && hgt < 0.35 && up > 0.0 && pbrLoaded(PBR_KIT_RUST_CREEP)) {
        vec2 tile = pbrTile(PBR_KIT_RUST_CREEP);
        float reach = mix(0.06, 0.3, vw) * mix(0.8, 1.2, hf(seed, 13u));
        vec2 st = vec2(q.x / tile.x, 1.0 - hgt / reach);
        vec4 d = textureGrad(pbrMaps[PBR_KIT_RUST_CREEP * PBR_MAPS_PER_MATERIAL + PBR_ALBEDO], st,
                             vec2(qx.x / tile.x, -hx / reach), vec2(qy.x / tile.x, -hy / reach));
        float a = d.a * up * step(0.0, st.y) * step(st.y, 1.0) * mix(0.4, 0.8, vw);
        base = mix(base, d.rgb * 0.9, a); metal *= 1.0 - a; rough = mix(rough, 0.85, a);
    }

    // at the range: grease spattered on what faces up (darker, glossier), soot rising up from the flames on
    // what stands over them (the pots' feet, the grates)
    if (heat && !food) {
        float spat = smoothstep(0.55, 0.8, vnoise(q * 22.0, 0x6EAu) * 0.6 + vnoise(q * 5.0, 0x6EBu) * 0.6) * smoothstep(0.2, 0.7, n.y);
        float greasy = (0.25 + 0.35 * vw) * spat;
        base = mix(base, vec3(0.05, 0.03, 0.012), 0.6 * greasy); rough = mix(rough, 0.25, greasy); metal *= 1.0 - 0.5 * greasy;
        float soot = (1.0 - smoothstep(0.95, 1.2, hgt)) * smoothstep(0.88, 0.95, hgt) * mix(0.5, 0.9, vw);
        base = mix(base, vec3(0.012, 0.011, 0.01), soot * up); rough = mix(rough, 0.95, soot * up); metal *= 1.0 - soot * up;
    }
    // the smoke's film: over the extractor and what is up by it, and under the hood (what faces down up there);
    // nowhere else in the kitchen
    float film = heat ? smoothstep(1.9, 2.35, hgt) * mix(0.1, 0.3, vw)
               : clean ? 0.0
               : smoothstep(1.95, 2.4, hgt) * (1.0 - smoothstep(-0.5, -0.1, n.y)) * mix(0.15, 0.45, vw);
    base = mix(base, vec3(0.03, 0.022, 0.014), film); metal *= 1.0 - 0.5 * film;

    // dirty grout between the tiles (their height map's joints)
    if (m == PMAT_TILE) {
        float grout = 1.0 - smoothstep(0.25, 0.45, height);
        base = mix(base, mix(vec3(0.22, 0.19, 0.14), vec3(0.08, 0.06, 0.04), vw), grout * mix(0.35, 0.75, vw));
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
