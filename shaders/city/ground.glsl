// Part of shaders/city.frag, which includes the parts in order: roads and their markings, sidewalks, kerbs, tactile paving, lots.

/* ======================================================================
 * Ground: asphalt with markings, sidewalks, curbs, lots
 * ==================================================================== */

vec2 laneLines(float t, float along, float half_, float aa) {
    float at = abs(t);
    float centre = band(at, 0.10, 0.24, aa);
    float dash = band(mod(along + 1.0, 9.0), 1.0, 4.78, aa);   // 3.78 m dashes every 9 m
    float lanes = 0.0;
    for (int i = 1; i <= 3; ++i) {
        float L = 3.4 * float(i);
        if (L < half_ - 1.6) lanes = max(lanes, band(at, L - 0.07, L + 0.07, aa) * dash);
    }
    float edge = band(at, half_ - 0.55, half_ - 0.42, aa);
    return vec2(max(centre, max(lanes, edge)), centre);
}

vec2 crosswalk(float lateral, float half_, float into, float walk, float aa) {
    float zone = band(into, 0.5, walk - 0.5, aa);
    float stripes = band(fract(lateral / 1.1), 0.0, 0.5, aa * 0.9);
    stripes *= 1.0 - smoothstep(half_ - 0.9, half_ - 0.6, abs(lateral));
    float stop = band(into, walk + 0.4, walk + 0.85, aa) * step(0.3, abs(lateral)) *
                 (1.0 - smoothstep(half_ - 0.7, half_ - 0.5, abs(lateral)));
    return vec2(max(zone * stripes, stop), 0.0);
}

// Road paint at carriageway uv (t across, s along, metres): lane lines, and the
// crosswalk and stop line at a crossing end. x = coverage, y = yellow.
vec2 roadPaint(vec2 uv, float half_, float len, bool crossing, float aa) {
    vec2 mk = laneLines(uv.x, uv.y, half_, aa);
    if (crossing) {
        float into = min(uv.y, len - uv.y);
        vec2 cw = crosswalk(uv.x, half_, into, 4.2, aa);
        mk = into < 5.2 ? cw : max(mk, cw);
    }
    return mk;
}

// Road markings are thermoplastic, laid a few millimetres thick.
const float PAINT_H = 0.003;

// Tactile paving at the foot of a drop kerb (src/city/streets.c: emit_drop): concrete
// slabs 0.4 m square, each set with 7 x 7 stainless steel studs 30 mm across
// that stand 5 mm proud. uv counts from the slab grid, in metres.
const float SLAB = 0.4, STUD_PITCH = SLAB / 7.0, STUD_R = 0.015, STUD_H = 0.005;
const float STUD_COVER = 3.14159265 * STUD_R * STUD_R / (STUD_PITCH * STUD_PITCH);

// Offset (metres) of uv from the centre of the stud whose square it is in.
vec2 studOffset(vec2 uv) { return (fract(uv / STUD_PITCH) - 0.5) * STUD_PITCH; }

// Lays the studs over the slab's surface. They are metal: little diffuse,
// a bright grey specular. As with the road paint, a stud's top shows shifted
// toward the viewer by its height, with its near side between; a ring of
// grime and shadow sits round its foot. Beyond where a stud is a pixel or so
// the pattern gives way to its average. Studs stand clear of the water, and
// water lies in the joints between slabs.
void tactileStuds(vec3 p, vec3 V, vec2 uv, vec3 T, vec3 B, vec3 nGeom, float aa,
                  inout vec3 alb, inout vec3 n, inout float rough, inout float specK, inout float ao,
                  inout float relH, inout float sink) {
    mat2 Juv = mat2(dFdx(uv), dFdy(uv)), Jw = mat2(dFdx(p.xz), dFdy(p.xz));
    mat2 toUV = abs(determinant(Jw)) > 1e-12 ? Juv * inverse(Jw) : mat2(0.0);
    vec2 lift = V.xz * (STUD_H / max(V.y, 0.02));
    lift *= min(1.0, 0.03 / max(length(lift), 1e-6));
    vec2 uvTop = uv + toUV * lift;
    vec2 oTop = studOffset(uvTop), oBase = studOffset(uv);
    float e = 0.5 * aa + 0.0004;
    float covTop = 1.0 - smoothstep(STUD_R - e, STUD_R + e, length(oTop));
    float covBase = 1.0 - smoothstep(STUD_R - e, STUD_R + e, length(oBase));
    float side = max(covBase - covTop, 0.0);
    float vis = 1.0 - smoothstep(0.25, 0.6, aa / STUD_PITCH);

    vec3 sAlb, sN;
    float sRough, sAO = 1.0;
    if (pbrLoaded(PBR_TACTILE)) {
        int i = PBR_TACTILE * PBR_MAPS_PER_MATERIAL;
        vec2 st = oTop / (2.0 * STUD_R) + 0.5;
        vec2 gx = dFdx(uvTop) / (2.0 * STUD_R), gy = dFdy(uvTop) / (2.0 * STUD_R);
        sAlb = textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb;
        vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
        sN = normalize(T * tn.x + B * tn.y + nGeom * tn.z);
        vec3 orm = textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).rgb;
        sAO = orm.r;
        sRough = orm.g * 0.7;   // the tops are polished by feet
    } else {
        // a low dome, brushed steel
        vec2 d = oTop / STUD_R;
        sN = normalize(nGeom + (T * d.x - B * d.y) * 0.6);
        sAlb = vec3(0.5, 0.5, 0.52) * (0.8 + 0.3 * vnoise(uvTop * 300.0, 0x57D0u));
        sRough = 0.35;
    }
    float lum = dot(sAlb, vec3(0.2126, 0.7152, 0.0722));
    float m = mix(STUD_COVER, covTop, vis);
    alb = mix(alb, sAlb * 0.12, m);
    specK = mix(specK, mix(0.45, 0.7, lum), m);
    rough = mix(rough, mix(0.3, sRough, vis), m);
    n = normalize(mix(n, sN, covTop * vis));
    ao *= mix(1.0, sAO, m);
    // the near side wall faces the viewer, dimmer and rougher
    n = normalize(mix(n, normalize(vec3(V.x, 0.0, V.z) + vec3(0.0, 0.3, 0.0)), side * vis));
    alb *= 1.0 - 0.3 * side * vis;
    // grime and shadow round its foot
    float ring = (1.0 - covBase) * (1.0 - smoothstep(STUD_R, STUD_R + 0.008, length(oBase)));
    ao *= 1.0 - 0.45 * ring * vis;
    alb *= 1.0 - 0.25 * ring * vis;
    relH += 4.0 * max(covTop, covBase) * vis;
    sink -= 3.0 * m;

    // the joints between slabs
    vec2 g = abs(fract(uv / SLAB - 0.5) - 0.5) * SLAB;
    float joint = 1.0 - smoothstep(0.002, 0.004 + aa, min(g.x, g.y));
    alb *= 1.0 - 0.5 * joint;
    ao *= 1.0 - 0.3 * joint;
    sink += 0.8 * joint;
}
/* Fine litter, too small and too many for the litter layer (src/city/litter.c):
 * flecks of paper and wrappers, dead leaves, grit and cigarette ends, and
 * flattened chewing gum on the sidewalks, more of them the more squalid the
 * street (sq, from the plan via params), over broad grimy stains. One cell
 * per pixel, each fleck inside its own cell, faded out as the cells shrink
 * toward a pixel. density: the chance of a fleck in each 0.3 m cell at full
 * squalor; gum: the chance of gum in each 0.25 m cell. */
void debris(vec2 xz, float sq, float density, float gum, float aa, inout vec3 alb, inout float rough) {
    if (sq <= 0.0) return;
    alb *= 1.0 - 0.3 * sq * smoothstep(0.45, 0.8, vnoise(xz * 0.35, 0xD1E7u));
    float px = aa / 0.3, vis = 1.0 - smoothstep(0.06, 0.18, px);
    if (vis > 0.0) {
        vec2 q = xz / 0.3, c = floor(q);
        uint h = hash2i(0xDEB1u, int(c.x), int(c.y));
        if (hfloat(h) < density * sq) {
            float a = hf(h, 3u) * 6.2831853, ca = cos(a), sa = sin(a);
            vec2 d = fract(q) - (0.3 + 0.4 * vec2(hf(h, 1u), hf(h, 2u)));
            d = vec2(ca * d.x + sa * d.y, -sa * d.x + ca * d.y);
            uint k = hash_combine(h, 4u) % 5u;
            float e = px * 0.7 + 0.01, m;
            if (k == 1u) m = 1.0 - smoothstep(1.0 - e * 12.0, 1.0, length(d / vec2(0.1, 0.05)));   // a leaf
            else {
                vec2 sz = vec2(0.04 + 0.1 * hf(h, 5u), 0.03 + 0.08 * hf(h, 6u));                // a scrap, 1-4 cm
                if (k == 4u) sz = vec2(0.05, 0.014);                                               // a cigarette end
                m = (1.0 - smoothstep(sz.x - e, sz.x + e, abs(d.x))) * (1.0 - smoothstep(sz.y - e, sz.y + e, abs(d.y)));
            }
            vec3 col = k == 0u ? vec3(0.27, 0.26, 0.245) : k == 1u ? vec3(0.13, 0.08, 0.03)
                     : k == 2u ? mix(neonColor(h >> 8) * 0.3, vec3(0.3), 0.6) : k == 3u ? vec3(0.05, 0.045, 0.04) : vec3(0.36, 0.2, 0.08);
            col *= 1.0 - 0.5 * WETNESS;   // sodden, like the litter itself
            m *= vis;
            alb = mix(alb, col, m);
            if (rough >= 0.0) rough = mix(rough, 0.7, m);
        }
    }
    float gpx = aa / 0.25, gvis = 1.0 - smoothstep(0.05, 0.14, gpx);
    if (gum > 0.0 && gvis > 0.0) {
        vec2 q = xz / 0.25, c = floor(q);
        uint h = hash2i(0x6E11u, int(c.x), int(c.y));
        if (hfloat(h) < gum * sq * sq) {
            float r = length(fract(q) - (0.25 + 0.5 * vec2(hf(h, 1u), hf(h, 2u)))) / (0.03 + 0.03 * hf(h, 3u));
            float m = (1.0 - smoothstep(1.0 - gpx * 20.0, 1.0, r)) * gvis;
            alb = mix(alb, vec3(0.035, 0.034, 0.033) + 0.03 * hf(h, 4u), m);
            if (rough >= 0.0) rough = mix(rough, 0.5, m);
        }
    }
}

// Ground. Carriageway quads carry uv = (offset from the centre line, distance
// along), their length in the seed and half-width (quarter metres) in params;
// the plain asphalt underneath (half-width 0) fills junctions and verges.
vec3 shadeGround(uint mat, vec3 p, vec3 n, vec3 V, vec2 uv, uint seed, uvec4 prm) {
    vec3 w = gWorld;
    float aa = max(fwidth(p.x), fwidth(p.z)) * 0.7 + 0.004;
    vec3 alb;
    float shin = 16.0, specK = 0.08, ao = 1.0;
    float rough = -1.0;   // >= 0: GGX with F0 = specK (authored materials)
    vec3 nGeom = n;
    // wet coat inputs (authored materials): relative height of the surface's
    // relief, and how far ruts, gutters and joints sink it further
    bool wettable = false;
    float relH = 0.0, relBroad = 0.0, sink = 0.0, jointLevel = -1.0;
    float paintC = 0.0;   // road paint coverage (raised: stays out of the standing water)

    if (mat == MAT_ROAD) {
        // Base asphalt (authored or procedural), then markings and covers
        // layered over it, then grime.
        bool pbr = pbrLoaded(PBR_ROAD);
        float big = vnoise(w.xz * 0.045, 0xA6u);
        Surface s;
        if (pbr) {
            // world-planar: +x is image right, -z image up
            s = samplePBR(PBR_ROAD, w.xz / ROAD_TILE, vec3(1.0, 0.0, 0.0), vec3(0.0, 0.0, -1.0), n);
            s.albedo *= ROAD_ALBEDO;
        } else {
            float nn = fbm(w.xz * 0.45, 0xA5u);
            s = Surface(vec3(0.030, 0.030, 0.033) * (0.7 + 0.6 * nn) * (0.75 + 0.5 * big), n, 1.0, 0.5, 0.5);
        }
        // patched / resurfaced rectangles
        vec2 pc = floor(w.xz / 7.0);
        uint ph = hash2i(0xA7u, int(pc.x), int(pc.y));
        if (hfloat(ph) < 0.12) s.albedo *= 0.75;

        float half_ = float(prm.y) * 0.25;
        vec2 mk = vec2(0.0);
        float dCurb = 10.0, side = 0.0, rim = 0.0;
        vec3 tilt = vec3(0.0);
        float wear = 0.45 + 0.55 * smoothstep(0.25, 0.6, fbm(w.xz * 1.7, 0xA8u));
        if (half_ > 0.0) {
            float len = uintBitsToFloat(seed);
            bool xing = (prm.z & 1u) != 0u;
            // The paint stands PAINT_H proud. A view ray crosses the paint's top
            // nearer the eye than it meets the asphalt, so the top shows shifted
            // toward the viewer, with the near edge's side wall between. uv are
            // metres; the screen derivatives map world offsets into them.
            mat2 Juv = mat2(dFdx(uv), dFdy(uv)), Jw = mat2(dFdx(p.xz), dFdy(p.xz));
            mat2 toUV = abs(determinant(Jw)) > 1e-12 ? Juv * inverse(Jw) : mat2(0.0);
            vec2 lift = V.xz * (PAINT_H / max(V.y, 0.02));
            lift *= min(1.0, 0.15 / max(length(lift), 1e-6));
            vec2 uvTop = uv + toUV * lift;
            vec2 mTop = roadPaint(uvTop, half_, len, xing, aa);
            vec2 mBase = roadPaint(uv, half_, len, xing, aa);
            mk = vec2(max(mTop.x, mBase.x), max(mTop.y, mBase.y));
            side = max(mBase.x - mTop.x, 0.0);
            // the rounded edges of the top catch the light: its slope from the coverage gradient
            float e = aa * 0.5 + 0.002;
            vec2 g = vec2(roadPaint(uvTop + vec2(e, 0.0), half_, len, xing, aa).x - mTop.x,
                          roadPaint(uvTop + vec2(0.0, e), half_, len, xing, aa).x - mTop.x) / e;
            vec2 gw = transpose(toUV) * g;
            tilt = -vec3(gw.x, 0.0, gw.y) * PAINT_H * wear;
            // water dams up against the paint's edges, and grime collects in the angle
            rim = max(roadPaint(uv, half_, len, xing, aa + 0.025).x - mk.x, 0.0) * wear;
            dCurb = half_ - abs(uv.x);
            // run-off pools in the gutter at the foot of each drop kerb
            if (xing) sink += 0.9 * band(min(uv.y, len - uv.y), 0.3, 3.9, 0.3) * (1.0 - smoothstep(0.15, 0.9, dCurb));
            // ruts faintly worn into the wheel paths, 0.85 m either side of each lane's centre
            float u = mod(abs(uv.x), 3.4);
            sink += 0.2 * (exp(-(u - 0.85) * (u - 0.85) / 0.04) + exp(-(u - 2.55) * (u - 2.55) / 0.04));
        }
        sink += 0.6 * (1.0 - smoothstep(0.1, 0.35, dCurb));   // the gutter along the kerb
        if (pbr) sink += 0.6 * hollow(w.xz, 0.25, 0x7E7Bu);
        // speckled with the glass beads and grit pressed into it
        float bead = vnoise(w.xz * 45.0, 0xA8Bu) * (1.0 - smoothstep(0.004, 0.02, aa));
        vec3 paint = mix(vec3(0.42, 0.41, 0.38), vec3(0.45, 0.30, 0.05), mk.y) * (1.0 - 0.3 * side) * (0.88 + 0.24 * bead);
        paintC = mk.x * wear;
        vec3 nAsphalt = s.n;
        s = layer(s, n, paint, 0.45, paintC);
        // the coat follows the asphalt's coarse grain a little; the bevel on its
        // top, and the side wall, which faces the viewer
        s.n = normalize(mix(s.n + (nAsphalt - n) * 0.35 * paintC + tilt, normalize(vec3(V.x, 0.0, V.z) + vec3(0.0, 0.3, 0.0)), side * wear));
        sink += 1.0 * rim - 1.2 * paintC;
        s.albedo *= 1.0 - 0.35 * rim;

        // manhole covers
        vec2 mc = floor(w.xz / 13.0);
        uint mh = hash2i(0xA9u, int(mc.x), int(mc.y));
        if (hfloat(mh) < 0.25) {
            vec2 c = (mc + 0.3 + 0.4 * vec2(hf(mh, 1u), hf(mh, 2u))) * 13.0;
            float r = length(w.xz - c);
            float ring = band(r, 0.30, 0.36, aa);
            float disc = 1.0 - smoothstep(0.3, 0.3 + aa, r);
            float grid = step(0.5, fract((w.x + w.z) * 6.0));
            s = layer(s, n, vec3(0.045, 0.04, 0.035) * (0.8 + 0.3 * grid), 0.35, disc);
            s.albedo *= 1.0 - ring * 0.5;
        }
        // gutter grime along the kerbs, and the debris washed into the gutter
        s.albedo *= mix(0.6, 1.0, smoothstep(0.0, 0.6, dCurb));
        debris(w.xz, float(prm.z >> 1) / 127.0, mix(0.03, 0.55, 1.0 - smoothstep(0.15, 0.7, dCurb)), 0.0, aa, s.albedo, s.rough);
        alb = s.albedo; n = s.n; ao = s.ao;
        if (pbr) {
            rough = s.rough; specK = 0.04;   // dielectric F0
            wettable = true;
            relH = mix((s.height - ROAD_RELIEF.x) / ROAD_RELIEF.y, 1.0, paintC);   // the paint fills the relief, and stands above it
            relBroad = (pbrBroadHeight(PBR_ROAD, w.xz / ROAD_TILE, 5.0) - ROAD_RELIEF.x) / ROAD_RELIEF.y;
            jointLevel = -1.0;   // asphalt is too fine-grained for its pits to hold water
        } else {
            shin = 60.0;
            specK = (0.25 + 0.5 * big) * (1.0 - 0.6 * mk.x);   // oily, slightly slick asphalt
        }
    } else if (mat == MAT_SIDEWALK && (prm.y == 0u || prm.y == 2u) && pbrLoaded(PBR_PAVING_STONES)) {
        // authored paving laid with the block grain (uv); a drop kerb's slope (2)
        // is laid along its kerb instead (uv = along, in from the kerb line)
        vec3 T, B;
        grainFrame(seed, T, B);
        Surface s = samplePBR(PBR_PAVING_STONES, uv / PAVING_TILE, T, B, n);
        alb = s.albedo * PAVING_ALBEDO;
        alb *= 0.6 + 0.4 * smoothstep(0.2, 0.55, fbm(w.xz * 0.15, 0x7114u));  // stains, as below
        n = s.n; ao = s.ao;
        rough = s.rough; specK = 0.04;   // dielectric F0
        wettable = true;
        relH = (s.height - PAVING_RELIEF.x) / PAVING_RELIEF.y;
        relBroad = (pbrBroadHeight(PBR_PAVING_STONES, uv / PAVING_TILE, 4.0) - PAVING_RELIEF.x) / PAVING_RELIEF.y;
        jointLevel = -0.3;   // the joints between slabs
        sink += 0.6 * hollow(w.xz, 0.1, 0x7E7Cu);
    } else if (mat == MAT_SIDEWALK && (prm.y == 1u || prm.y == 3u) && pbrLoaded(PBR_KERB)) {
        // authored kerb stones, laid along the edge (uv = along, inward); the
        // tactile slabs at a drop kerb (3) are cast in the same concrete
        vec3 T, B;
        grainFrame(seed, T, B);
        Surface s = samplePBR(PBR_KERB, uv / KERB_TILE, T, B, n);
        alb = s.albedo * KERB_ALBEDO;
        alb *= 0.6 + 0.4 * smoothstep(0.2, 0.55, fbm(w.xz * 0.15, 0x7114u));  // stains, as the paving
        n = s.n; ao = s.ao;
        rough = s.rough; specK = 0.04;   // dielectric F0
        wettable = true;
        relH = (s.height - KERB_RELIEF.x) / KERB_RELIEF.y;
        relBroad = (pbrBroadHeight(PBR_KERB, uv / KERB_TILE, 4.0) - KERB_RELIEF.x) / KERB_RELIEF.y;
        jointLevel = -0.4;
    } else if (mat == MAT_CURB && pbrLoaded(PBR_KERB)) {
        // authored kerb face: uv = (metres along cross(up, n), height), so image up is -y
        Surface s = samplePBR(PBR_KERB, uv / KERB_TILE, normalize(vec3(n.z, 0.0, -n.x)), vec3(0.0, -1.0, 0.0), n);
        alb = s.albedo * KERB_ALBEDO;
        n = s.n; ao = s.ao;
        rough = s.rough; specK = 0.04;   // dielectric F0
        wettable = true;   // vertical: damp, never pooled
    } else if (mat == MAT_SIDEWALK) {
        // paving runs with the block grain (uv); prm.y = 1 is the kerb-stone edge
        vec2 tp = uv / 1.2;
        vec2 ti = floor(tp), tf = fract(tp);
        float th = hfloat(hash2i(0x7111u, int(ti.x), int(ti.y)));
        float g = min(min(tf.x, 1.0 - tf.x), min(tf.y, 1.0 - tf.y)) * 1.2;
        float grout = 1.0 - smoothstep(0.012, 0.03 + aa, g);
        alb = vec3(0.080, 0.077, 0.074) * (0.78 + 0.4 * th) * (0.65 + 0.7 * fbm(w.xz * 0.7, 0x7112u));
        alb = mix(alb, vec3(0.028), grout * 0.75);
        if (prm.y == 1u || prm.y == 3u) alb = vec3(0.12, 0.115, 0.11) * (0.8 + 0.3 * fbm(w.xz * 3.0, 0x7113u));
        alb *= 0.6 + 0.4 * smoothstep(0.2, 0.55, fbm(w.xz * 0.15, 0x7114u));  // stains
        shin = 24.0; specK = 0.1;
    } else if (mat == MAT_CURB) {
        alb = vec3(0.11, 0.105, 0.10) * (0.7 + 0.5 * fbm(w.xz * 2.0 + w.y, 0x7115u));
    } else if (prm.y == 1u) {
        // park: rough dark grass with worn patches
        float gr = fbm(w.xz * 0.9, 0x7117u);
        alb = mix(vec3(0.020, 0.032, 0.018), vec3(0.034, 0.040, 0.022), gr) * (0.6 + 0.8 * fbm(w.xz * 4.0, 0x7118u));
        shin = 8.0; specK = 0.03;
    } else {
        alb = vec3(0.045, 0.043, 0.04) * (0.6 + 0.8 * fbm(w.xz * 0.5, 0x7116u));
    }

    // fine debris on the paving and lots: thicker along the kerb, none on the kerb face
    if (mat == MAT_SIDEWALK) debris(w.xz, float(prm.z) / 255.0, prm.y == 1u ? 0.3 : 0.16, 1.0, aa, alb, rough);
    else if (mat == MAT_LOT) debris(w.xz, float(prm.z) / 255.0, prm.y == 1u ? 0.05 : 0.14, prm.y == 1u ? 0.0 : 0.6, aa, alb, rough);

    if (mat == MAT_SIDEWALK && prm.y == 1u) {
        // The joint where the kerb stones meet the paving (v = KERB_STONE_WIDTH): a
        // narrow gap packed with grit, dead leaves and a little moss, with the
        // stone's worn arris rounding down into it.
        float jd = KERB_STONE_WIDTH - uv.y;          // metres from the paving edge
        float fw = fwidth(uv.y) + 1e-4;
        float gap = 1.0 - smoothstep(0.015 - fw, 0.015 + fw, jd);
        float arris = 1.0 - smoothstep(0.0, 0.035, jd - 0.015);
        vec3 T, B;
        grainFrame(seed, T, B);                      // -B points into the paving
        float grit = fbm(vec2(uv.x * 30.0, jd * 60.0), 0x7119u);
        vec3 dirt = mix(vec3(0.030, 0.025, 0.018), vec3(0.075, 0.060, 0.042), smoothstep(0.3, 0.75, grit));
        dirt = mix(dirt, vec3(0.020, 0.032, 0.014), smoothstep(0.55, 0.8, fbm(vec2(uv.x * 3.0, 0.0), 0x711Au)));
        // grime washed out of the joint onto the stone beside it, patchy along the kerb
        float spill = (1.0 - smoothstep(0.0, 0.05, jd - 0.015)) * smoothstep(0.35, 0.7, fbm(vec2(uv.x * 2.0, 1.7), 0x711Bu));
        n = normalize(n - B * 0.45 * arris * (1.0 - gap));
        alb = mix(alb * (1.0 - 0.35 * spill), dirt, gap);
        ao *= mix(1.0, 0.45, gap) * (1.0 - 0.25 * arris);
        if (rough >= 0.0) rough = mix(rough, 0.92, gap);
        sink += 0.9 * gap;   // water runs into the joint
    }

    if (mat == MAT_SIDEWALK && prm.y == 3u) {
        vec3 T, B;
        grainFrame(seed, T, B);
        tactileStuds(p, V, uv, T, B, nGeom, aa, alb, n, rough, specK, ao, relH, sink);
    }

    if (mat == MAT_SIDEWALK && prm.w == 1u) {
        // A drop kerb sits low: water stands on its lowered kerb stones and
        // creeps up the foot of its slope, further the harder it rains.
        float d = prm.y == 3u ? uv.y + KERB_STONE_WIDTH : uv.y;   // in from the kerb line
        float foot = 1.0 - smoothstep(0.0, 0.2 + 0.5 * WETNESS, d - KERB_STONE_WIDTH);
        sink += 1.1 * foot * (0.55 + 0.9 * vnoise(w.xz * 1.6, 0xD20Bu));
    }

    if (wettable && WETNESS > 0.0) {
        // Under a canopy, arcade or overhang no rain falls, but water runs in,
        // so the ground stays wet and keeps its puddles; only the ripples stop.
        float wet = WETNESS;
        float open = 1.0 - shelterAt(vRel);
        gStreak = 1.0 + 3.5 * wet;
        // Wet coat: in steady rain every authored surface carries a film of water
        // (porous asphalt and stone darken, and turn glossy), but water only
        // stands where it collects: gutters, joints, faint wheel-path ruts and
        // the odd isolated hollow. The surface's own relief just breaks up the
        // puddles' edges, so the street still reads as flat.
        float level = -0.25 * wet;
        float broad = smoothstep(0.0, 0.08, level - (0.3 * relBroad + 0.05 * relH - sink));
        float fine = smoothstep(0.0, 0.1, wet * jointLevel - relH);
        float pool = max(broad, fine) * smoothstep(0.7, 0.9, nGeom.y) * smoothstep(0.3, 0.6, wet);
        alb *= mix(1.0, mix(0.55, 0.85, paintC), wet) * mix(1.0, 0.92, pool);   // paint soaks up no water
        rough = mix(rough, rough * 0.45, wet);
        n = normalize(mix(n, nGeom, 0.5 * wet));   // the film fills the fine relief
        // standing water: a smooth film over the surface, filling its relief
        rough = mix(rough, 0.03, pool);
        n = normalize(mix(n, nGeom, pool));
        // drops ringing the standing water, and faintly the film (none under cover)
        vec2 rg = ripples(w.xz, gTime, 0.25 + 0.6 * frame.weather.x);
        n = normalize(n + vec3(rg.x, 0.0, rg.y) * mix(0.15, 1.0, pool) * open);
        ao = mix(ao, 1.0, pool);
        specK = mix(specK, 0.02, pool);   // water's F0
    }

    if (rough >= 0.0) {
        // Specular anti-aliasing: widen the lobe by how much the normal turns
        // across this pixel, so detail finer than a pixel blurs the highlight
        // instead of sparkling.
        vec3 dnx = dFdx(n), dny = dFdy(n);
        float kernel = min(0.5 * (dot(dnx, dnx) + dot(dny, dny)), 0.2);
        rough = pow(rough * rough * rough * rough + kernel, 0.25);
    }

    vec3 diff = ambientLight(n, p.y) * ao;
    vec3 spec = vec3(0.0);
    cityLights(p, n, V, shin, specK, rough, diff, spec);
    diff += neonBounce(w, p.y) * ao;
    if (rough >= 0.0) {
        // sky / haze reflection: rougher surfaces reflect closer to the normal
        float ndv = max(dot(n, V), 1e-4);
        vec3 R = normalize(mix(reflect(-V, n), n, rough * rough));
        float w = envBRDF(specK, rough, ndv) * ao;
        spec += envReflection(R) * w;
        gRefl = vec4(n.x, n.z, rough, w);   // screen-space reflections refine this
    } else {
        // sky / haze sheen at grazing angles
        float fres = pow(1.0 - max(dot(n, V), 0.0), 5.0);
        spec += envReflection(reflect(-V, n)) * fres * specK * 2.0 * ao;
    }
    return alb * diff + spec;
}
