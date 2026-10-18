// Part of shaders/city.frag, which includes the parts in order: arcology concourse levels.

/* ======================================================================
 * Arcologies (emit_arcology in src/city/mesh_arcology.c). Low-key light: a dark base,
 * neon washing the street stratum, sodium pools under the concourse lamps
 * (worked out here from the lamp row each street floor carries in aux), the
 * arcology's few real lights from the chunk's light table, cold light from
 * the crown higher up, and haze over everything, thick and warm low down,
 * thin and cool above. params y surface (ARS_* in src/gen/arcology.h), z
 * variety, w stratum height * 255.
 * ==================================================================== */

const uint ARS_FLOOR = 0u, ARS_STREET = 1u, ARS_SLAT = 2u, ARS_CEILING = 3u, ARS_COLUMN = 4u, ARS_CAPITAL = 5u, ARS_BEAM = 6u,
           ARS_BLOCK = 7u, ARS_ROOF = 8u, ARS_STALL = 9u, ARS_STALLROOF = 10u, ARS_SHUTTER = 11u, ARS_COUNTER = 12u,
           ARS_SPINE = 13u, ARS_CORE = 14u, ARS_GALLERY = 15u, ARS_SOFFIT = 16u, ARS_FASCIA = 17u, ARS_UNITS = 18u,
           ARS_RAIL = 19u, ARS_METAL = 20u, ARS_CROWN = 21u, ARS_SHAFT = 22u, ARS_SEARCH = 23u, ARS_PUDDLE = 24u,
           ARS_STEAM = 25u, ARS_CRATE = 26u, ARS_BRIDGE = 27u, ARS_PORTAL = 28u, ARS_STAIR = 29u, ARS_LINING = 30u,
           ARS_KIOSK = 31u, ARS_LIFTDOOR = 32u, ARS_SKYLOBBY = 33u;
// block uses (AB_* in src/gen/arcology.h)
const uint AB_MARKET = 0u, AB_NOODLE = 1u, AB_ARCADE = 2u, AB_CLINIC = 3u, AB_CAPSULE = 4u, AB_REPAIR = 5u, AB_BATH = 6u,
           AB_EXCHANGE = 7u, AB_HOUSING = 8u;

const vec3 CROWN_COLD = vec3(0.55, 0.72, 1.0);

// Joints of a w-metre grid, 1 on the joint line.
float grid(vec2 q, float w, float jw, float aa) {
    vec2 t = fract(q / w) * w;
    vec2 e = min(t, w - t);
    return max(1.0 - smoothstep(jw, jw + aa, e.x), 1.0 - smoothstep(jw, jw + aa, e.y));
}

// Covers one MSAA sample (never sample 0, which depth and reflections read):
// a translucent veil of light over what stands behind it. The sample is fixed
// per face by the way it faces, so overlapping faces of one volume take
// different samples and add up evenly instead of in speckles.
void veil() {
    vec3 g = normalize(vNrm);
    int k = g.x > 0.5 ? 1 : g.x < -0.5 ? 2 : g.z > 0.5 ? 3 : g.z < -0.5 ? 1 : 2;
    gl_SampleMask[0] = 1 << k;
}

// Pools under a concourse's lamp rows, from the floor's (along, across) uv and
// the row in aux. (The lamp heads' reflections in the wet floor come from the
// screen: ssr_apply streaks them toward the viewer.)
vec3 streetPools(vec2 uv) {
    float step_ = max(float(vAux & 255u) * 0.1, 1.0), phase = float((vAux >> 8) & 255u) * 0.1;
    float off = float((vAux >> 16) & 255u) * 0.1, h = float(vAux >> 24) * 0.1;
    vec3 E = vec3(0.0);
    vec3 lc = lampColor(0);
    float k0 = floor((uv.x - phase) / step_ + 0.5);
    for (int row = 0; row < 2; ++row) {
        if (row == 1 && off < 0.01) break;
        float across = row == 0 ? off : -off;
        for (int k = -1; k <= 1; ++k) {
            float a = phase + (k0 + float(k)) * step_;
            vec2 dxy = vec2(a - uv.x, across - uv.y);
            float r2 = dot(dxy, dxy), d2 = r2 + h * h, d = sqrt(d2);
            float att = 20.0 * h / (d2 * d + 1.0);
            E += lc * att;
        }
    }
    return E;
}

// Unit fronts on a gallery's back wall at the fine window pitch: shopfronts
// in the street stratum, flat doors in habitat, glass offices in commerce,
// panelled timber in the corporate tier.
vec3 unitFronts(vec2 uv, uint seed, uint st, float t, float aa) {
    float pitch = 1.9 + 0.5 * hf(seed, 70u);
    float bay = floor(uv.x / pitch), fx = fract(uv.x / pitch), y = uv.y;
    uint h = hash3i(seed, int(bay), int(t * 997.0), 71);
    float edgeM = 1.0 - smoothstep(0.0, 0.02 + aa / pitch, min(fx, 1.0 - fx));
    vec3 wall = st == 0u ? vec3(0.05, 0.045, 0.042) : st == 1u ? vec3(0.13, 0.13, 0.125) : st == 2u ? vec3(0.06, 0.05, 0.04) : vec3(0.16, 0.10, 0.05);
    if (st == 0u) {
        // shopfronts: lit glass or a shutter, a neon tube over each
        bool lit = hf(h, 1u) < 0.48;
        float win = band(fx, 0.06, 0.94, aa) * band(y, 0.15, 2.5, aa * 3.0);
        vec3 inside = lit ? shopColor(h) * (0.55 + 0.6 * hf(h, 2u)) * (0.75 + 0.25 * step(0.5, fract(y * 3.0 + fx * 2.0))) : vec3(0.008);
        if (!lit && hf(h, 3u) < 0.6) inside = vec3(0.05, 0.05, 0.055) * (0.6 + 0.4 * step(0.5, fract(y / 0.09)));   // shutter
        vec3 c = mix(wall, inside, win);
        float tube = band(y, 2.68, 2.76, aa * 3.0) * band(fx, 0.1, 0.9, aa) * step(0.35, hf(h, 4u));
        c += neonColor(h >> 5) * tube * 4.0 * neonFlicker(h, uint(hf(h, 5u) < 0.12), gTime);
        return mix(c, vec3(0.02), edgeM);
    }
    if (st == 1u) {
        // homes: a painted door, a small lit window, a number
        float door = band(fx, 0.12, 0.42, aa) * band(y, 0.0, 2.1, aa * 3.0);
        float win = band(fx, 0.55, 0.88, aa) * band(y, 1.0, 1.9, aa * 3.0);
        vec3 doorC = mix(vec3(0.10, 0.12, 0.13), vec3(0.16, 0.06, 0.05), step(0.6, hf(h, 6u)));
        bool lit = hf(h, 7u) < 0.55;
        vec3 winC = lit ? (hf(h, 8u) < 0.5 ? kelvin(3000.0) : kelvin(6500.0)) * 0.55 * (0.6 + 0.4 * hf(h, 9u)) : vec3(0.01);
        vec3 c = mix(mix(wall, doorC, door), winC, win);
        c += vec3(0.3, 0.25, 0.15) * band(fx, 0.2, 0.32, aa) * band(y, 2.25, 2.4, aa * 3.0) * 1.5;   // the number plate's lamp
        return mix(c, wall * 0.5, edgeM * 0.6);
    }
    if (st == 2u) {
        // offices and hotel frontages: tall glass on warm rooms
        float glass = band(y, 0.1, 2.9, aa * 3.0);
        bool lit = hf(h, 10u) < 0.7;
        vec3 room = lit ? kelvin(3200.0) * (0.25 + 0.35 * hf(h, 11u)) * (0.6 + 0.4 * smoothstep(1.5, 2.8, y)) : vec3(0.006);
        vec3 c = mix(wall, room, glass);
        return mix(c, vec3(0.18, 0.13, 0.07), edgeM);   // bronze mullions
    }
    // the corporate tier: timber panels, gold reveals, a low warm wash
    vec3 timber = vec3(0.14, 0.07, 0.03) * (0.8 + 0.2 * sin(uv.x * 30.0 + vnoise(uv * 4.0, 0x7111u) * 4.0));
    vec3 c = timber * (0.6 + 0.8 * exp(-y * 0.8));
    c += kelvin(2400.0) * 0.12 * band(y, 0.0, 0.25, aa * 3.0);
    return mix(c, vec3(0.5, 0.36, 0.12) * 0.6, edgeM);
}

vec3 shadeArcology(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    uint surf = prm.y, var = prm.z;
    float t = float(prm.w) / 255.0;
    if (dot(n, V) < 0.0) n = -n;                     // thin planes are seen from both sides
    float y = p.y - gBase - SIDEWALK_HEIGHT;
    float aa = max(length(fwidth(uv)), 1e-4);
    float dist = length(rel);
    vec3 w = gWorld;

    /* ---- volumes: shafts, searchlight beams, steam ---- */
    if (surf == ARS_SHAFT) {
        // rays: soft-edged, streaked along their length, with motes drifting through
        veil();
        float edge = smoothstep(0.0, 0.35, uv.x) * smoothstep(0.0, 0.35, 1.0 - uv.x);
        float streak = 0.35 + 0.65 * vnoise(vec2(uv.x * 16.0 + hf(seed, 2u) * 20.0, uv.y * 0.4), seed & 0xFFFFu);   // long rays
        float motes = 0.7 + 0.3 * vnoise(vec2((w.x + w.z) * 2.0, w.y * 0.6 - gTime * 0.5), (seed >> 8) & 0xFFFFu);
        float I = (0.25 + 0.75 * pow(clamp(uv.y, 0.0, 1.0), 1.3)) * streak * motes * edge * smoothstep(3.0, 12.0, dist);
        return CROWN_COLD * I * 5.5;
    }
    if (surf == ARS_SEARCH) {
        veil();
        float along = vLitter.x;
        float I = (1.0 - along) * (1.0 - along) * 9.0 + 0.6;
        return vec3(0.8, 0.9, 1.0) * I * smoothstep(0.05, 0.6, abs(dot(n, V)));
    }
    if (surf == ARS_STEAM) {
        veil();
        float rise = gTime * 0.5;
        float puff = fbm(vec2(uv.x * 2.5 + hf(seed, 1u) * 9.0, uv.y * 2.0 - rise), seed & 0xFFFFu);
        float I = smoothstep(0.35, 0.75, puff) * band(uv.x, 0.1, 0.9, 0.15) * (1.0 - uv.y) * smoothstep(0.0, 0.15, uv.y);
        if (I < 0.02) discard;
        return (vec3(0.05, 0.035, 0.03) + neonBounce(w, y) * 6.0) * I * 3.0;
    }

    /* ---- light ---- */
    int st = t < 0.2 ? 0 : t < 0.5 ? 1 : t < 0.85 ? 2 : 3;
    float street = 1.0 - smoothstep(0.08, 0.35, t);
    vec3 diff = mix(vec3(0.010, 0.008, 0.007), vec3(0.020, 0.016, 0.012), smoothstep(0.5, 0.9, t));
    {   // neon washing the street stratum, drifting
        float m = vnoise(w.xz / 9.0 + vec2(0.0, y / 6.0), 0xA7C0u), k = vnoise(w.xz / 13.0 + vec2(3.7, y / 9.0), 0xA7C1u);
        vec3 neon = mix(vec3(1.0, 0.1, 0.5), vec3(0.1, 0.8, 1.0), smoothstep(0.3, 0.7, m)) * smoothstep(0.3, 0.8, k);
        diff += neon * 0.07 * street * (0.6 + 0.4 * max(n.y, 0.0));
    }
    // the crown's cold light falls down the canyon onto what faces up; warm controlled light in the upper strata
    diff += CROWN_COLD * 0.035 * (0.3 + 0.7 * max(n.y, 0.0)) * smoothstep(0.05, 0.4, t);
    diff += kelvin(2700.0) * 0.06 * smoothstep(0.45, 0.85, t) * (0.4 + 0.6 * max(-n.y, 0.0) + 0.3 * max(n.y, 0.0));
    vec3 spec = vec3(0.0);
    float rough = 0.6, specK = 0.04, gloss = 0.0;
    bool wet = false;
    vec3 alb = vec3(0.1), emit = vec3(0.0);
    uint h = hash_combine(seed, var);

    if (surf == ARS_STREET || surf == ARS_FLOOR || surf == ARS_SLAT) {
        // wet concrete pavers and tile: dark, slick, pooled
        vec2 q = surf == ARS_FLOOR ? uv : vec2(uv.x, uv.y);
        float pv = surf == ARS_FLOOR && var == 2u ? 1.2 : 1.0;
        vec2 cell = floor(q / pv);
        float joint = grid(q, pv, 0.012, aa);
        alb = vec3(0.05, 0.047, 0.046) * (0.7 + 0.6 * hfloat(hash3i(0xA5F1u, int(cell.x), int(cell.y), 1)));
        alb *= 0.55 + 0.8 * fbm(w.xz * 0.35, 0xA5F2u);                 // grime trodden in
        alb *= 1.0 - 0.3 * joint;
        rough = 0.32;
        wet = true;
        if (surf == ARS_FLOOR) {
            if (var == 2u) { alb = vec3(0.03, 0.028, 0.03) * (1.0 - 0.6 * joint); alb += vec3(0.25, 0.17, 0.06) * grid(q, 3.6, 0.02, aa) * 0.6; rough = 0.12; }
            else if (var == 1u) { alb = vec3(0.06, 0.058, 0.055) * (0.6 + 0.8 * fbm(w.xz * 0.8, 0xA5F3u)); rough = 0.45; }
            else if (var == 3u || var == 7u) { vec2 tl = fract(q / 0.3); float j = grid(q, 0.3, 0.008, aa); alb = mix(vec3(0.20, 0.19, 0.17), vec3(0.04), j) * (var == 7u ? 0.5 : 1.0); rough = 0.4; }
            else if (var == 4u) { alb = vec3(0.045, 0.043, 0.042) * (1.0 - 0.5 * grid(q, 0.6, 0.01, aa)); rough = 0.38; }
            else if (var == 6u) { alb = vec3(0.12, 0.115, 0.11) * (0.8 + 0.4 * vnoise(q * 6.0, 0xA5F4u)); rough = 0.25; }
            else if (var == 5u) { alb = vec3(0.05, 0.048, 0.046) * (0.7 + 0.6 * fbm(w.xz * 0.5, 0xA5F5u)); rough = 0.6; wet = false; }
        }
        // standing water in the hollows
        float pool = smoothstep(0.62, 0.72, fbm(w.xz * 0.22, 0xA5F6u)) * street;
        rough = mix(rough, 0.04, pool);
        alb *= 1.0 - 0.5 * pool;
        if (surf == ARS_STREET) diff += streetPools(uv);
        if (surf == ARS_SLAT) {
            // streetlight slatted through the mullions, falling away from the glass
            float fs = fract(uv.x / 1.5);
            float slat = band(fs, 0.06, 0.94, aa / 1.5 + 0.02);
            float fall = exp(-uv.y * 0.55);
            vec3 outside = mix(lampColor(0), neonColor(hash2i(seed, int(floor(uv.x / 6.0)), 3)), 0.35) * 0.18;
            diff += outside * slat * fall;
        }
    } else if (surf == ARS_CEILING) {
        alb = vec3(0.045, 0.043, 0.042) * (0.8 + 0.4 * fbm(uv * 0.4, 0xA5C1u));
        float coffer = grid(uv, 3.0, 0.18, aa);
        alb *= 1.0 - 0.5 * coffer;
        // bare tubes here and there, some failing
        vec2 cell = floor(uv / 3.0);
        uint hc = hash3i(seed, int(cell.x), int(cell.y), 7);
        if (hfloat(hc) < 0.12) {
            vec2 f = fract(uv / 3.0);
            float tube = band(f.x, 0.48, 0.52, aa / 3.0) * band(f.y, 0.2, 0.8, aa / 3.0);
            emit = vec3(0.7, 0.85, 0.8) * tube * 2.5 * neonFlicker(hc, hfloat(hc >> 4) < 0.3 ? 2u : 0u, gTime);
        }
    } else if (surf == ARS_COLUMN || surf == ARS_CAPITAL || surf == ARS_BEAM || surf == ARS_PORTAL) {
        // megastructure concrete: board marks, grime rising from the floor, wet streaks
        alb = vec3(0.085, 0.082, 0.078) * (0.8 + 0.4 * fbm(vec2(uv.x * 0.7, uv.y * 0.15), 0xA5D1u));
        alb *= 0.85 + 0.15 * step(0.5, fract(uv.y / 0.3));
        alb *= mix(0.55, 1.0, smoothstep(0.0, 1.5, y));
        float streaks = smoothstep(0.55, 0.8, vnoise(vec2(uv.x * 3.0, 0.0), 0xA5D2u)) * smoothstep(4.0, 0.5, y);
        rough = mix(0.7, 0.2, streaks);
        if (var == 1u && surf == ARS_COLUMN) {
            // setback piers: dark fluted stone
            alb = vec3(0.04, 0.038, 0.04) * (0.85 + 0.3 * smoothstep(0.3, 0.5, abs(fract(uv.x / 0.35) - 0.5)));
            rough = 0.3;
        }
        if (surf == ARS_CAPITAL) { alb = mix(alb, vec3(0.12, 0.08, 0.04), var == 1u ? 0.8 : 0.35); rough = 0.35; }
        if (surf == ARS_PORTAL) {   // polished black stone, a bronze middle step
            alb = var == 1u ? vec3(0.09, 0.06, 0.03) : vec3(0.025, 0.024, 0.026) * (0.8 + 0.3 * fbm(uv * 2.0, 0xA5D3u));
            rough = 0.2; specK = 0.06;
        }
        // posters and flyers on columns at street level
        if (surf == ARS_COLUMN && y < 2.6 && y > 0.7 && street > 0.5) {
            vec2 pc = floor(vec2(uv.x / 0.6, y / 0.8));
            uint hp = hash3i(seed ^ 0x9055u, int(pc.x), int(pc.y), int(floor(w.x)) ^ int(floor(w.z)));
            if (hfloat(hp) < 0.35) {
                vec2 f = fract(vec2(uv.x / 0.6, y / 0.8));
                float paper = band(f.x, 0.08, 0.92, 0.02) * band(f.y, 0.05, 0.95, 0.02);
                alb = mix(alb, mix(neonColor(hp >> 4) * 0.25 + 0.03, vec3(0.35, 0.33, 0.3), 0.4) * (0.6 + 0.4 * step(0.5, fract(f.y * 7.0))), paper * 0.9);
            }
        }
    } else if (surf == ARS_BLOCK) {
        // block masses: grimy painted concrete and sheet metal, small lit windows above the stalls, vents, graffiti
        uint u = var;
        vec3 base = u == AB_CLINIC ? vec3(0.20, 0.21, 0.21) : u == AB_BATH ? vec3(0.10, 0.13, 0.13) : u == AB_ARCADE ? vec3(0.03, 0.02, 0.04)
                  : u == AB_CAPSULE ? vec3(0.13, 0.12, 0.11) : u == AB_EXCHANGE ? vec3(0.03, 0.035, 0.04) : u == AB_HOUSING ? vec3(0.09, 0.085, 0.08)
                  : vec3(0.07, 0.06, 0.05);
        alb = base * (0.7 + 0.6 * fbm(vec2(uv.x * 0.5, uv.y * 0.8), seed & 0xFFFFu));
        alb *= mix(0.6, 1.0, smoothstep(0.0, 1.2, uv.y));
        if (u == AB_MARKET || u == AB_REPAIR) alb *= 0.8 + 0.2 * step(0.5, fract(uv.x / 0.12));   // corrugated sheet
        float ups = step(3.5, uv.y);
        vec2 cell = floor(vec2(uv.x / 1.6, (uv.y - 3.5) / 1.1));
        uint hw = hash3i(seed, int(cell.x), int(cell.y), 13);
        vec2 f = fract(vec2(uv.x / 1.6, (uv.y - 3.5) / 1.1));
        float win = ups * band(f.x, 0.25, 0.75, aa) * band(f.y, 0.25, 0.75, aa) * step(hfloat(hw), 0.35);
        vec3 winC = hfloat(hw >> 3) < 0.5 ? kelvin(2600.0) * 0.6 : neonColor(hw >> 6) * 0.5;
        if (u == AB_CAPSULE) {   // rows of capsule portholes
            vec2 c2 = fract(vec2(uv.x / 1.1, uv.y / 1.2)) - 0.5;
            float port = (1.0 - smoothstep(0.26, 0.26 + aa, length(c2))) * ups;
            win = port * step(hfloat(hash2i(seed, int(floor(uv.x / 1.1)), int(floor(uv.y / 1.2)))), 0.55);
            winC = kelvin(4500.0) * 0.7;
        }
        if (u == AB_ARCADE) { float dots = step(0.82, vnoise(uv * vec2(6.0, 6.0) + vec2(gTime * 0.7, 0.0), seed & 0xFFFFu)); emit += neonColor(uint(uv.x) + (seed >> 4)) * dots * 0.6 * ups; }
        if (u == AB_CLINIC) emit += vec3(0.1, 0.9, 1.0) * band(uv.y, 3.6, 3.75, aa * 3.0) * 1.5;
        if (u == AB_EXCHANGE) emit += neonColor(5u) * band(uv.y, 3.55, 3.85, aa * 3.0) * step(0.4, fract(uv.x * 1.3 - gTime * 0.8)) * 1.2;
        emit += winC * win;
        float graf = smoothstep(0.64, 0.68, fbm(vec2(uv.x, uv.y) * 0.7, seed & 0xFFFFu)) * (1.0 - ups);
        alb = mix(alb, neonColor(seed >> 9) * 0.12, graf * 0.7);
        rough = 0.6;
    } else if (surf == ARS_ROOF) {
        alb = vec3(0.03, 0.03, 0.032) * (0.7 + 0.6 * fbm(uv * 0.9, 0xA5E1u));
        float pool = smoothstep(0.55, 0.65, fbm(uv * 0.5, 0xA5E2u));
        rough = mix(0.7, 0.05, pool); wet = pool > 0.0;
    } else if (surf == ARS_STALL) {
        // inside a stall: shelves of goods or a menu, lit warm by its own lamps
        uint u = var;
        vec3 Ls = u == AB_CLINIC ? vec3(0.75, 0.95, 1.0) : u == AB_ARCADE ? neonColor(seed >> 3) * 0.7 + 0.2 : kelvin(2900.0 + 1500.0 * hf(seed, 3u));
        float yy = uv.y;
        vec3 goods = shelves(vec2(uv.x * 0.8, yy * 0.8), vec3(0.18, 0.15, 0.12), seed);
        alb = mix(vec3(0.16, 0.14, 0.12), goods, band(yy, 0.9, 2.3, aa * 3.0));
        if (u == AB_NOODLE || u == AB_EXCHANGE || u == AB_REPAIR) {
            // a lit menu board / screen up high
            float board = band(yy, 2.2, 2.95, aa * 3.0) * band(fract(uv.x / 2.0), 0.05, 0.95, aa);
            emit += (u == AB_EXCHANGE ? neonColor(5u) : vec3(1.0, 0.85, 0.55)) * board * 0.9 * (0.7 + 0.3 * step(0.5, fract(uv.x * 4.0 + floor(yy * 8.0) * 0.37)));
        }
        diff += Ls * (0.45 + 0.35 * smoothstep(3.2, 1.5, yy));
        rough = 0.5;
    } else if (surf == ARS_STALLROOF) {
        alb = vec3(0.04);
        if (n.y < -0.5) {
            // under the canopy: tubes across it, warm or cold by the stall
            float tube = band(fract(uv.x / 1.2), 0.46, 0.54, aa / 1.2 + 0.01);
            emit = kelvin(3000.0 + 3000.0 * hf(seed, 5u)) * (0.35 + 2.2 * tube);
        }
    } else if (surf == ARS_SHUTTER) {
        float rib = 0.6 + 0.4 * smoothstep(0.3, 0.5, abs(fract(uv.y / 0.09) - 0.5) * 2.0);
        alb = vec3(0.07, 0.072, 0.075) * rib * (0.5 + 0.7 * fbm(uv * 0.8, seed & 0xFFFFu));
        float graf = smoothstep(0.6, 0.65, fbm(uv * 0.9, seed));
        alb = mix(alb, neonColor(seed >> 6) * 0.1, graf * 0.8);
        rough = 0.5;
    } else if (surf == ARS_COUNTER) {
        alb = hf(seed, 9u) < 0.5 ? vec3(0.12, 0.12, 0.13) : vec3(0.16, 0.08, 0.04);
        diff += kelvin(3000.0) * 0.3 * max(n.y, 0.0);
        rough = 0.3;
    } else if (surf == ARS_SPINE) {
        // the express core: dark glass and steel, vertical light grooves; bronze higher up, polished stone at the top
        float groove = band(fract(uv.x / 2.4), 0.48, 0.52, aa / 2.4 + 0.01);
        alb = var == 3u ? vec3(0.02, 0.018, 0.02) : var == 2u ? vec3(0.025, 0.02, 0.015) : vec3(0.02, 0.022, 0.026);
        alb *= 0.85 + 0.3 * grid(vec2(uv.x, y), 1.2, 0.01, aa);
        vec3 gc = var == 3u ? vec3(0.9, 0.65, 0.25) : var == 2u ? vec3(0.8, 0.55, 0.3) : neonColor(seed >> 4) * 0.9;
        emit += gc * groove * (0.6 + 0.4 * sin(y * 0.15 - gTime * 0.8)) * (var >= 2u ? 0.6 : 1.0);
        rough = 0.15; specK = 0.06;
    } else if (surf == ARS_CORE) {
        alb = vec3(0.10, 0.098, 0.094) * (0.8 + 0.4 * fbm(vec2(uv.x * 0.6, y * 0.4), 0xA5B1u));
        alb = mix(alb, vec3(0.5, 0.4, 0.05), band(y, 0.0, 0.25, aa * 3.0) * step(0.5, fract((uv.x + y) / 0.5)) * 0.6);   // hazard skirt
        rough = 0.7;
    } else if (surf == ARS_GALLERY || surf == ARS_BRIDGE) {
        uint s = var;
        alb = s == 0u ? vec3(0.06, 0.058, 0.055) : s == 1u ? vec3(0.13, 0.13, 0.125) : s == 2u ? vec3(0.22, 0.20, 0.18) : vec3(0.05, 0.03, 0.02);
        alb *= 0.8 + 0.4 * fbm(uv * 0.6, 0xA5A1u);
        if (s >= 2u) alb *= 1.0 - 0.4 * grid(uv, s == 3u ? 0.25 : 0.9, 0.01, aa);
        rough = s == 0u ? 0.35 : s == 3u ? 0.2 : 0.4;
        wet = s == 0u;
        if (n.y < -0.5) {   // a bridge's underside
            alb = vec3(0.04);
            emit = (s <= 1u ? neonColor(seed >> 2) * 0.15 : kelvin(2700.0) * 0.2) * band(fract(uv.x / 4.0), 0.45, 0.55, aa / 4.0);
        }
    } else if (surf == ARS_SOFFIT) {
        uint s = var;
        alb = s == 3u ? vec3(0.12, 0.06, 0.025) : vec3(0.06, 0.058, 0.056);
        vec2 cell = floor(uv / 2.4), f = fract(uv / 2.4);
        uint hc = hash3i(seed, int(cell.x), int(cell.y), 17);
        float lamp = s == 0u ? band(f.x, 0.47, 0.53, aa / 2.4) * band(f.y, 0.2, 0.8, aa / 2.4) * step(hfloat(hc), 0.25)
                             : (1.0 - smoothstep(0.08, 0.08 + aa / 2.4, length(f - 0.5))) * step(hfloat(hc), 0.6);
        vec3 lc = s == 0u ? vec3(0.65, 0.9, 0.75) : s == 1u ? kelvin(5500.0) : kelvin(2700.0);
        emit = lc * lamp * (s == 0u ? 2.2 * neonFlicker(hc, hfloat(hc >> 5) < 0.25 ? 2u : 0u, gTime) : 2.0);
        if (s == 3u) alb *= 0.75 + 0.25 * step(0.5, fract(uv.x / 0.15));   // timber slats
        diff += lc * 0.05;
    } else if (surf == ARS_FASCIA) {
        uint s = var;
        alb = s == 0u ? vec3(0.07, 0.065, 0.06) * (0.6 + 0.8 * fbm(uv * vec2(0.5, 3.0), 0xA591u))
            : s == 1u ? vec3(0.14, 0.15, 0.16) : s == 2u ? vec3(0.20, 0.13, 0.06) : vec3(0.55, 0.40, 0.14);
        rough = s >= 2u ? 0.25 : 0.6; specK = s == 3u ? 0.5 : 0.04;
        if (var == 9u) alb = vec3(0.30, 0.28, 0.25);
    } else if (surf == ARS_UNITS) {
        vec3 c = unitFronts(uv, seed, var, t, aa);
        // windows read as emission; the wall around them takes light
        return mix(c + c * diff * 4.0, mix(vec3(0.016, 0.010, 0.008), vec3(0.008, 0.010, 0.016), smoothstep(0.15, 0.6, t)), 1.0 - exp(-dist * mix(0.010, 0.004, t)));
    } else if (surf == ARS_RAIL) {
        if (var == 0u || var == 3u) {
            float F = glassFresnel(n, V);
            float cov = paneCoverage(F);
            vec3 c = (vec3(0.012, 0.016, 0.018) + CROWN_COLD * 0.03 * smoothstep(0.1, 0.5, t) + neonBounce(w, y) * 2.0) * mix(1.0, 3.0, F);
            return c * 0.4 / cov;
        }
        alb = var == 2u ? vec3(0.30, 0.27, 0.22) : vec3(0.05, 0.05, 0.055);
        rough = 0.25; specK = var == 1u ? 0.5 : 0.04;
    } else if (surf == ARS_METAL) {
        uint v = var;
        alb = v == 0u ? vec3(0.14, 0.145, 0.15) * (0.85 + 0.15 * step(0.08, fract(uv.x / 1.2))) : v == 1u ? vec3(0.03)
            : v == 2u ? mix(vec3(0.12, 0.05, 0.03), vec3(0.05, 0.09, 0.07), hf(seed, 31u)) : v == 4u ? vec3(0.32, 0.22, 0.10)
            : v == 5u ? vec3(0.08) * (0.7 + 0.3 * step(0.5, fract(uv.y / 0.08))) : v == 8u ? vec3(0.06, 0.06, 0.065) : vec3(0.035, 0.035, 0.04);
        alb *= 0.75 + 0.5 * fbm(uv * 1.3, 0xA581u + v);
        rough = v == 4u ? 0.2 : 0.45; specK = v == 4u || v == 0u ? 0.4 : 0.06;
    } else if (surf == ARS_CROWN) {
        // the glazed cap: a cold, rain-washed sky through a grid of glazing bars
        vec2 f = fract(uv / 2.0);
        float bar = max(1.0 - smoothstep(0.0, 0.03 + aa / 2.0, min(f.x, 1.0 - f.x)), 1.0 - smoothstep(0.0, 0.03 + aa / 2.0, min(f.y, 1.0 - f.y)));
        float cloud = 0.7 + 0.3 * fbm(uv * 0.05 + vec2(gTime * 0.01, 0.0), 0xA571u);
        float rainS = 0.92 + 0.08 * step(0.7, vnoise(vec2(uv.x * 9.0, uv.y * 0.5 - gTime * 3.0), 0xA572u));
        return mix(CROWN_COLD * 0.55 * cloud * rainS + CLOUD_DECK * 0.6, vec3(0.006), bar);
    } else if (surf == ARS_PUDDLE) {
        // black water under the crown, ringed by drips
        float r = length(uv) / max(float(var) * 0.5, 0.3);
        if (r > 1.05) discard;
        vec2 g = ripples(uv * 1.8 + vec2(hf(seed, 1u) * 9.0), gTime, 0.18) * 1.6;
        n = normalize(vec3(-g.x, 1.0, -g.y));
        alb = vec3(0.004);
        rough = 0.02; specK = 0.02; wet = true;
    } else if (surf == ARS_SKYLOBBY) {
        // a sky lobby's glazing round the foot of its tier, seen from outside: a lit
        // band, cool in habitat, warm in commerce, gold in the corporate tier
        float fx = fract(uv.x / 1.5);
        float mull = 1.0 - smoothstep(0.0, 0.03 + aa / 1.5, min(fx, 1.0 - fx));
        vec3 Lc = var <= 1u ? kelvin(5600.0) : var == 2u ? kelvin(3300.0) : kelvin(2500.0);
        float people = smoothstep(0.55, 0.7, vnoise(vec2(uv.x * 1.3, 0.0), seed & 0xFFFFu)) * band(uv.y, 0.0, 1.8, 0.05);
        vec3 c = Lc * (0.3 + 0.3 * smoothstep(0.5, 2.8, uv.y)) * (1.0 - 0.7 * people);
        float F = glassFresnel(n, V);
        c = mix(c, envReflection(reflect(-V, n)), F) + paneSheen(p, n, V);
        glassRefl(n, F * (1.0 - mull));
        return mix(c, vec3(0.01), mull);
    } else if (surf == ARS_CRATE) {
        uint v = var;
        if (v == 12u || v == 13u) {   // terrace planters and their planting
            alb = v == 12u ? vec3(0.10, 0.10, 0.095) : mix(vec3(0.02, 0.05, 0.015), vec3(0.06, 0.12, 0.04), fbm(uv * 3.0, 0xA5C7u));
        } else if (v == 10u) {   // awning cloth: bold stripes
            float stripe = step(0.5, fract(uv.x / 0.45));
            alb = mix(neonColor(seed >> 3) * 0.35, vec3(0.3, 0.29, 0.26), stripe) * 0.7;
            diff += kelvin(2800.0) * 0.25 * max(-n.y, 0.0);
        } else {
            alb = v == 6u ? vec3(0.17, 0.12, 0.07) : v == 11u ? vec3(0.04, 0.12, 0.06) : v == 7u ? vec3(0.12, 0.125, 0.13) : v == 9u ? vec3(0.13, 0.06, 0.03)
                : mix(vec3(0.05, 0.10, 0.16), vec3(0.15, 0.04, 0.03), hf(seed, 41u));
            alb *= 0.7 + 0.5 * fbm(uv * 2.0, seed & 0xFFFFu);
            if (v == 7u) { vec2 c2 = fract(uv * 0.7) - 0.5; alb *= 0.5 + 0.5 * smoothstep(0.2, 0.3, length(c2)); }   // AC fans
        }
        rough = 0.6;
    } else if (surf == ARS_STAIR) {
        if (var == 1u) return vec3(0.1, 1.0, 0.35) * 3.0;
        alb = vec3(0.10, 0.11, 0.10) * (0.8 + 0.4 * fbm(vec2(uv.x, y) * 0.5, 0xA561u));
        alb = mix(alb, vec3(0.45, 0.35, 0.03), band(y, 0.0, 0.3, aa * 3.0) * step(0.5, fract((uv.x + y) / 0.4)));
        rough = 0.6;
    } else if (surf == ARS_LINING) {
        alb = vec3(0.025, 0.024, 0.026);
        rough = 0.5;
    } else if (surf == ARS_KIOSK) {
        alb = var == 1u ? vec3(0.04, 0.045, 0.05) : neonColor(seed >> 5) * 0.12 + 0.03;
        if (var == 0u) emit += kelvin(3200.0) * 0.8 * band(y, 0.95, 1.8, aa * 3.0) * band(fract(uv.x / 2.2), 0.08, 0.92, aa);
        rough = 0.4;
    } else if (surf == ARS_LIFTDOOR) {
        if (var == 9u) return vec3(1.0, 0.5, 0.1) * 3.0 * (0.8 + 0.2 * sin(gTime * 2.0 + hf(seed, 3u) * 6.0));
        float seam = 1.0 - smoothstep(0.0, 0.01 + aa, abs(uv.x - 0.5));
        alb = var == 0u ? vec3(0.42, 0.30, 0.14) : var == 1u ? vec3(0.30, 0.31, 0.32) : vec3(0.12, 0.14, 0.15);
        alb *= (0.9 + 0.1 * vnoise(vec2(uv.x * 40.0, y * 2.0), 0xA551u)) * (1.0 - 0.7 * seam);
        rough = var == 0u ? 0.15 : 0.3; specK = 0.5;
    }

    cityLights(p, n, V, 30.0, specK, rough, diff, spec);
    vec3 col = alb * diff + spec + emit;
    if (wet) {
        // The wet film. What the screen can't reflect is, indoors, the dark
        // canyon: neon haze, the crown's cold light straight above. (ssr_apply
        // swaps the street's sky reflection for the screen's; the small error
        // that leaves where it does find a reflection is lost in the haze.)
        float ndv = max(dot(n, V), 1e-4);
        vec3 R = normalize(mix(reflect(-V, n), n, rough * rough));
        float wgt = envBRDF(max(specK, 0.02), rough, ndv) * (surf == ARS_PUDDLE ? 1.0 : 0.8);
        vec3 indoor = vec3(0.008, 0.007, 0.010) + neonBounce(w, y + 6.0) * 3.0 * street + CROWN_COLD * 0.12 * smoothstep(0.9, 0.995, R.y);
        col += indoor * wgt;
        gRefl = vec4(n.x, n.z, rough, wgt);
    }
    // haze: dark, faintly sodium-warm and neon-tinted in the street stratum, thin and cool up the canyon
    float dens = mix(0.014, 0.005, smoothstep(0.1, 0.6, t));
    vec3 hz = mix(vec3(0.016, 0.010, 0.008), vec3(0.008, 0.010, 0.016), smoothstep(0.15, 0.6, t)) + neonBounce(w, y) * 0.35 * street;
    return mix(col, hz, 1.0 - exp(-dist * dens));
}
