// Part of shaders/city.frag, which includes the parts in order: litter.

/* ======================================================================
 * Litter (src/city/litter.c): rubbish lying about the streets, as ordinary
 * geometry in the near-only litter layer, lit by its chunk's lamps and
 * shopfronts. params y is the kind (LK_*); the seed picks colours, print and
 * brand; uv runs over the item's own surface. city.vert passes the ground's
 * ambient occlusion and how soaked the item is (vLitter). Paper, card and
 * cigarette butts soak up the rain in patches: darker, duller, and glossy
 * with the film of water. Plastic, glass and metal just turn glossier and
 * bead with drops.
 * ==================================================================== */

// Lit as the authored ground is (GGX with F0 = f0); tint colours the specular (bare metal).
vec3 litterLight(vec3 p, vec3 n, vec3 V, vec3 alb, float rough, float f0, vec3 tint, float ao) {
    vec3 dnx = dFdx(n), dny = dFdy(n);   // specular anti-aliasing, as shadeGround
    rough = pow(pow(clamp(rough, 0.03, 1.0), 4.0) + min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.2), 0.25);
    vec3 diff = ambientLight(n, p.y - gBase) * ao;
    vec3 spec = vec3(0.0);
    cityLights(p, n, V, 0.0, f0, rough, diff, spec);
    diff += neonBounce(gWorld, p.y - gBase) * ao;
    float ndv = max(dot(n, V), 1e-4);
    vec3 R = normalize(mix(reflect(-V, n), n, rough * rough));
    spec += envReflection(R) * envBRDF(f0, rough, ndv) * ao;
    return alb * diff + spec * tint;
}

// Contact shadow under an item: a dark disc covering a share of each pixel's
// samples, more toward its middle, with an ordered dither between the levels.
// Like paneCoverage it assumes 4x MSAA; it never covers sample 0, so depth
// and reflections still see the ground, and without MSAA it vanishes.
vec3 litterShadow(vec2 uv) {
    const float BAYER[4] = float[4](0.0, 2.0, 3.0, 1.0);
    ivec2 f = ivec2(gl_FragCoord.xy) & 1;
    float cov = (1.0 - smoothstep(0.3, 1.0, length(uv))) * 0.85;
    int k = min(int(cov * 3.0 + (BAYER[f.y * 2 + f.x] + 0.5) * 0.25), 3);
    gl_SampleMask[0] = ((1 << k) - 1) << (4 - k);
    return vec3(0.0015, 0.0013, 0.0012);
}

// Drops beading on plastic, glass and metal: one in some of the ~1 cm cells
// of whichever plane the surface faces most, gone once cells shrink below a pixel.
vec3 beads(vec3 p, vec3 n, float soak, inout float rough) {
    vec3 a = abs(n);
    int ax = a.y > max(a.x, a.z) ? 1 : a.x > a.z ? 0 : 2;
    vec2 q = (ax == 1 ? p.xz : ax == 0 ? p.zy : p.xy) * 90.0;
    float px = max(fwidth(q.x), fwidth(q.y));
    if (px > 0.6 || soak <= 0.0) return n;
    vec2 c = floor(q);
    uint h = hash2i(0xBEADu, int(c.x), int(c.y));
    if (hfloat(h) > soak * 0.5) return n;
    vec2 d = fract(q) - 0.5 - (vec2(hf(h, 1u), hf(h, 2u)) - 0.5) * 0.4;
    float bead = (1.0 - smoothstep(0.7, 1.0, length(d) / (0.15 + 0.15 * hf(h, 3u)))) * (1.0 - smoothstep(0.3, 0.6, px));
    vec3 t = ax == 1 ? vec3(d.x, 0.0, d.y) : ax == 0 ? vec3(0.0, d.y, d.x) : vec3(d.x, d.y, 0.0);
    rough = mix(rough, 0.04, bead);
    return normalize(n + t * 4.0 * bead);
}

// Newsprint: columns of text, a headline, a photograph, on paper yellowed by age.
vec3 newsprint(vec2 uv, uint h, vec2 fw) {
    vec3 paper = mix(vec3(0.44, 0.43, 0.39), vec3(0.42, 0.38, 0.29), hf(h, 1u));
    float cols = 4.0 + floor(hf(h, 2u) * 3.0);
    float cu = (uv.x - 0.05) / 0.9 * cols, fu = fwidth(cu) + 1e-5;
    float page = band(uv.x, 0.05, 0.95, fw.x) * band(uv.y, 0.05, 0.95, fw.y);
    float head = band(uv.y, 0.07, 0.15, fw.y) * band(uv.x, 0.06, 0.4 + 0.5 * hf(h, 3u), fw.x);
    float pc = floor(hf(h, 4u) * (cols - 1.0)), py = 0.2 + 0.25 * hf(h, 5u);
    float photo = band(cu, pc + 0.08, pc + 1.92, fu) * band(uv.y, py, py + 0.22, fw.y);
    float tone = mix(0.25 + 0.5 * vnoise(uv * vec2(30.0, 24.0), h & 0xFFFFu), 0.5, smoothstep(0.6, 1.5, fw.x * 30.0));
    float ly = uv.y * 72.0;
    float text = filteredPulse(cu, 0.0, 0.9, fu) * filteredPulse(ly, 0.1, 0.55, fwidth(ly) + 1e-5) * step(0.17, uv.y);
    float ink = page * max(max(head * 0.9, photo * tone), text * 0.5 * (1.0 - photo));
    return mix(paper, vec3(0.03), ink);
}

// Flyers for club nights and takeaways: saturated print on gloss.
vec3 flyerPrint(vec2 uv, uint h, vec2 fw) {
    bool dark = hf(h, 10u) < 0.35;
    vec3 bg = dark ? vec3(0.02) : neonColor(h >> 3) * 0.4 + 0.04;
    vec3 ink = dark ? neonColor(h >> 7) * 0.55 : vec3(0.45);
    float tx = uv.x * 9.0;
    float title = band(uv.y, 0.1, 0.26, fw.y) * band(uv.x, 0.1, 0.9, fw.x) * filteredPulse(tx, 0.1, 0.8, fwidth(tx) + 1e-5);
    float ty = uv.y * 50.0;
    float small = band(uv.y, 0.8, 0.92, fw.y) * band(uv.x, 0.12, 0.88, fw.x) * filteredPulse(ty, 0.0, 0.5, fwidth(ty) + 1e-5);
    float disc = 1.0 - smoothstep(0.22 - fw.x, 0.22 + fw.x, length((uv - vec2(0.5, 0.55)) * vec2(1.0, 1.4)));
    vec3 c = mix(bg, ink, max(title, small * 0.7));
    return mix(c, dark ? neonColor(h >> 11) * 0.5 : vec3(0.03), disc * 0.85);
}

// Kraft card: flutes showing through the liner, a printed mark, a strip of tape, stains.
vec3 cardboard(vec2 uv, uint h, vec2 fw) {
    vec3 c = vec3(0.34, 0.235, 0.13) * (0.85 + 0.3 * hf(h, 60u));
    float fl = uv.x * 70.0;
    c *= 0.94 + 0.06 * filteredPulse(fl, 0.0, 0.5, fwidth(fl) + 1e-5);
    float mark = band(uv.x, 0.3, 0.7, fw.x) * band(uv.y, 0.35, 0.6, fw.y) * step(0.4, hf(h, 61u));
    float letters = mix(step(0.5, vnoise(uv * vec2(30.0, 10.0), h & 0xFFFu)), 0.5, smoothstep(0.6, 1.5, fw.x * 30.0));
    c = mix(c, vec3(0.04, 0.035, 0.03), mark * (0.4 + 0.5 * letters));
    c = mix(c, vec3(0.45, 0.38, 0.25), band(uv.y, 0.47, 0.53, fw.y) * step(0.5, hf(h, 62u)));
    return c * (0.7 + 0.3 * smoothstep(0.2, 0.6, fbm(uv * 3.0 + hf(h, 63u) * 7.0, 0xCA4Du)));
}

// A drinks can's livery: a brand colour, a wave and a roundel; bare aluminium
// at the rims, lid and base, and where the paint is scuffed.
vec3 canPaint(vec2 uv, uint h, vec2 fw, out float metal) {
    uint b = hash_combine(h, 31u) & 7u;
    vec3 base = b == 0u ? vec3(0.42, 0.03, 0.03) : b == 1u ? vec3(0.36, 0.37, 0.38) : b == 2u ? vec3(0.03, 0.07, 0.32)
              : b == 3u ? vec3(0.04, 0.24, 0.07) : b == 4u ? vec3(0.02) : b == 5u ? neonColor(h >> 9) * 0.45
              : b == 6u ? vec3(0.45, 0.44, 0.41) : vec3(0.4, 0.28, 0.05);
    vec3 accent = b == 4u ? neonColor(h >> 12) * 0.55 : b == 6u ? vec3(0.4, 0.03, 0.03) : vec3(0.45);
    float wave = band(uv.y + 0.06 * sin(uv.x * 12.566 + hf(h, 32u) * 6.28), 0.42, 0.56, fw.y * 1.5);
    vec2 lq = vec2(fract(uv.x + hf(h, 33u)) - 0.5, uv.y - 0.7) * vec2(3.2, 1.0);
    float logo = 1.0 - smoothstep(0.08 - fw.y, 0.08 + fw.y, length(lq));
    float scuff = smoothstep(0.62, 0.75, vnoise(uv * vec2(24.0, 10.0), h & 0xFFFFu)) * hf(h, 34u);
    metal = max(1.0 - band(uv.y, 0.07, 0.9, fw.y), scuff * 0.8);
    return mix(base, accent, max(wave, logo));
}

vec3 shadeLitter(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    uint kind = prm.y;
    if (kind == LK_SHADOW) return litterShadow(uv);
    vec3 V = -normalize(rel);
    float ao = clamp(vLitter.x, 0.0, 1.0), soak = vLitter.y;
    uint h = seed, var = (seed >> 4) & 3u;   // var: the variant src/city/litter.c built
    vec2 fw = fwidth(uv) + 1e-5;
    vec3 alb = vec3(0.4), tint = vec3(1.0);
    float rough = 0.8, f0 = 0.04;
    bool porous = true;

    if (kind == LK_NEWS) alb = newsprint(uv, h, fw);
    else if (kind == LK_FLYER) { alb = flyerPrint(uv, h, fw); rough = 0.45; f0 = 0.05; }
    else if (kind == LK_SCRAP) {
        if ((var & 1u) == 0u) {   // till receipt: thermal paper, faint grey print
            float ry = uv.y * 30.0;
            alb = vec3(0.46, 0.46, 0.44) * (1.0 - 0.25 * filteredPulse(ry, 0.1, 0.4, fwidth(ry) + 1e-5) * band(uv.x, 0.15, 0.85, fw.x));
            rough = 0.5;
        } else {                  // wrapper, sometimes foil
            alb = mix(neonColor(h >> 6) * 0.35 + 0.02, vec3(0.42), band(uv.y, 0.4, 0.6, fw.y));
            rough = 0.5;
            if (hf(h, 12u) < 0.35) { porous = false; tint = alb * 2.0 + 0.1; alb *= 0.3; f0 = 0.6; rough = 0.3; }
        }
    }
    else if (kind == LK_CARDBOARD) alb = cardboard(uv, h, fw);
    else if (kind == LK_WAD) { alb = hf(h, 57u) < 0.5 ? newsprint(fract(uv * 2.0), h, fw * 2.0) : vec3(0.44, 0.43, 0.41); rough = 0.85; }
    else if (kind == LK_BUTT) {   // filter (cork or white) at v < 0.6, paper, then the burnt end
        vec3 filt = hf(h, 50u) < 0.65 ? vec3(0.36, 0.2, 0.07) : vec3(0.44, 0.43, 0.4);
        alb = mix(filt, vec3(0.45, 0.44, 0.42), smoothstep(0.6 - fw.y, 0.6 + fw.y, uv.y));
        alb = mix(alb, vec3(0.06, 0.055, 0.05), smoothstep(0.92 - fw.y, 0.92 + fw.y, uv.y));
        rough = 0.85;
    } else if (kind == LK_CUP) {
        if (uv.y > 0.9) { alb = hf(h, 51u) < 0.6 ? vec3(0.42) : vec3(0.025); rough = 0.35; porous = false; }   // the lid
        else {
            uint cv = hash_combine(h, 52u) % 3u;
            vec3 paperC = cv == 0u ? vec3(0.45, 0.44, 0.42) : cv == 1u ? vec3(0.3, 0.2, 0.12) : neonColor(h >> 5) * 0.35 + 0.03;
            float sleeve = band(uv.y, 0.35, 0.7, fw.y) * step(0.5, hf(h, 53u));
            float logo = 1.0 - smoothstep(0.1 - fw.y, 0.1 + fw.y, length(vec2(fract(uv.x + hf(h, 54u)) - 0.5, uv.y - 0.5) * vec2(2.5, 1.0)));
            alb = mix(mix(paperC, vec3(0.28, 0.19, 0.11), sleeve), vec3(0.05, 0.2, 0.1), logo * (1.0 - sleeve) * step(0.4, hf(h, 55u)));
            rough = 0.6;
        }
    } else if (kind == LK_BOX) {
        if (var == 0u) { alb = mix(vec3(0.44, 0.43, 0.41), vec3(0.4, 0.03, 0.02), band(uv.y, 0.55, 0.75, fw.y) * 0.9); rough = 0.6; }   // noodle box
        else if (var == 1u) {   // pizza box, greasy
            alb = vec3(0.32, 0.22, 0.12) * (1.0 - 0.45 * smoothstep(0.55, 0.7, vnoise(uv * 6.0, h & 0xFFu)));
            alb = mix(alb, vec3(0.05, 0.03, 0.02), band(length(uv - 0.5), 0.12, 0.17, fw.x) * 0.8);
            rough = 0.85;
        }
        else if (var == 2u) { alb = vec3(0.47, 0.46, 0.44); rough = 0.9; porous = false; }   // polystyrene
        else { alb = mix(vec3(0.45, 0.44, 0.42), hf(h, 56u) < 0.5 ? vec3(0.4, 0.03, 0.03) : vec3(0.35, 0.26, 0.06), step(0.5, uv.y)); rough = 0.4; }   // cigarettes
    } else {
        porous = false;
        if (kind == LK_CAN) {
            float metal;
            vec3 paint = canPaint(uv, h, fw, metal);
            alb = mix(paint, vec3(0.02), metal);
            f0 = mix(0.06, 0.75, metal);
            tint = mix(vec3(1.0), vec3(0.85, 0.86, 0.88), metal);
            rough = mix(0.3, 0.22, metal);
        } else if (kind == LK_BOTTLE) {
            vec3 glass = var == 0u ? vec3(0.03, 0.11, 0.04) : var == 1u ? vec3(0.1, 0.045, 0.01) : var == 2u ? vec3(0.2, 0.22, 0.21) : vec3(0.02, 0.05, 0.09);
            float label = band(uv.y, 0.15, 0.48, fw.y) * step(0.3, hf(h, 40u));
            alb = mix(glass * 0.4, vec3(0.42, 0.38, 0.3) * (0.6 + 0.4 * hf(h, 41u)), label);
            rough = mix(0.05, 0.7, label);
            f0 = mix(0.05, 0.04, label);
        } else if (kind == LK_BAG) {
            float sx = uv.x * 6.0;
            alb = var == 0u ? vec3(0.4, 0.4, 0.38) : var == 1u ? vec3(0.02) : var == 2u ? vec3(0.05, 0.1, 0.3)
                : mix(vec3(0.42), vec3(0.4, 0.04, 0.03), filteredPulse(sx, 0.0, 0.5, fwidth(sx) + 1e-5));
            rough = 0.45;
        } else {   // bin bag: black, green or blue-grey film, stretched and creased
            alb = var == 1u ? vec3(0.012, 0.028, 0.016) : var == 2u ? vec3(0.02, 0.024, 0.034) : vec3(0.012);
            vec3 q = p * 9.0;
            n = normalize(n + 0.4 * (vec3(vnoise(q.xz + q.y, 0xB1Bu), vnoise(q.zy + 3.1, 0xB1Cu), vnoise(q.xy + 7.7, 0xB1Du)) - 0.5));
            rough = 0.3;
            f0 = 0.05;
        }
    }

    alb *= mix(1.0, 0.6, hf(h, 21u) * hf(h, 21u));   // the longer it has lain there, the dirtier
    if (porous) {
        // Soaks in patches as the rain goes on, a sheet from its edges in:
        // darker, duller, glossy with water. Sodden sheets wrinkle where they
        // cling to the ground, and the creases catch the light.
        bool sheet = kind == LK_NEWS || kind == LK_FLYER || kind == LK_SCRAP || kind == LK_CARDBOARD;
        float patchy = fbm(uv * 2.5 + vec2(hf(h, 22u), hf(h, 23u)) * 9.0, 0x50A4u);
        float edge = sheet ? 1.0 - smoothstep(0.0, 0.1, min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y))) : 0.0;
        float wetM = smoothstep(0.0, 0.15, soak * 1.5 - 0.3 - 0.7 * patchy + 0.4 * edge);
        alb = mix(alb, mix(alb, vec3(luma(alb)), 0.35) * 0.55, wetM);
        rough = mix(rough, 0.25, wetM);
        f0 = mix(f0, 0.03, wetM);
        if (sheet) {
            vec2 g = vec2(vnoise(uv * 9.0 + 1.7, 0x3C1Au), vnoise(uv * 9.0 + 5.3, 0x3C1Bu)) - 0.5;
            n = normalize(n + vec3(g.x, 0.0, g.y) * (0.25 + 0.5 * wetM) * (1.0 - smoothstep(0.3, 0.8, max(fw.x, fw.y) * 9.0)));
        }
    } else {
        rough = mix(rough, rough * 0.35, soak);
        n = beads(p, n, soak, rough);
    }
    return litterLight(p, n, V, alb, rough, f0, tint, ao);
}
