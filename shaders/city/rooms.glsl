// Part of shaders/city.frag, which includes the parts in order: interior mapping: the rooms seen through windows.

/* ======================================================================
 * Interior mapping
 *
 * Each window cell is a room box behind the facade: [0,W] x [0,H] x [0,D]
 * in tangent space (x along the wall, y up, z into the building). The view
 * ray is intersected with the box from the inside and the nearest wall,
 * floor or ceiling is shaded; a furniture plane and blinds add depth layers.
 * ==================================================================== */

struct Room {
    bool  lit;
    vec3  light;
    float bright;
    vec3  wall;
    vec3  floorC;
    float blinds;    // 0..1 fraction of the window covered from the top
    float curtain;   // 0 or 1
    vec3  curtainC;
    float furnZ;     // furniture plane depth fraction
    bool  person;
    bool  shop;
    bool  tv;        // dark room lit only by a television on the back wall
    uint  h;
};

// Shop walls: shelving with rows of colourful stock.
vec3 shelves(vec2 w, vec3 base, uint h) {
    float row = floor(w.y / 0.42);
    float fy = fract(w.y / 0.42);
    if (w.y < 0.3 || w.y > 2.3) return base;
    if (fy < 0.1) return vec3(0.05);
    float item = floor(w.x / 0.22);
    uint ih = hash3i(h, int(item), int(row), 7);
    float fx = fract(w.x / 0.22);
    float top = 0.45 + 0.5 * hfloat(ih);
    if (fx < 0.1 || fx > 0.9 || fy > top) return base * 0.4;
    return mix(neonColor(ih >> 5), vec3(0.8), 0.35) * (0.4 + 0.6 * hf(ih, 3u));
}

// Lamp colour at a colour temperature (K): blackbody hues from warm incandescent
// to daylight, softened as an eye adapted to mixed city light sees them.
vec3 kelvin(float k) {
    const vec3 c2700 = vec3(1.00, 0.62, 0.32), c3000 = vec3(1.00, 0.69, 0.42), c3500 = vec3(1.00, 0.77, 0.55),
               c4000 = vec3(1.00, 0.84, 0.68), c5000 = vec3(1.00, 0.93, 0.85), c6500 = vec3(0.93, 0.97, 1.00);
    if (k < 3000.0) return mix(c2700, c3000, clamp((k - 2700.0) / 300.0, 0.0, 1.0));
    if (k < 3500.0) return mix(c3000, c3500, (k - 3000.0) / 500.0);
    if (k < 4000.0) return mix(c3500, c4000, (k - 3500.0) / 500.0);
    if (k < 5000.0) return mix(c4000, c5000, (k - 4000.0) / 1000.0);
    return mix(c5000, c6500, clamp((k - 5000.0) / 1500.0, 0.0, 1.0));
}

// A television's light: a new scene every few seconds (mostly cool blue-white,
// sometimes warm or green), flickering within each scene.
vec3 tvGlow(uint h) {
    float scene = floor(gTime * (0.2 + 0.3 * hf(h, 14u)) + 64.0 * hf(h, 15u));
    uint sh = hash_combine(h, uint(scene));
    float k = hf(sh, 1u);
    vec3 c = k < 0.6 ? mix(vec3(0.50, 0.64, 1.0), vec3(0.80, 0.88, 1.0), hf(sh, 2u))
           : k < 0.85 ? vec3(1.0, 0.76, 0.52) : vec3(0.58, 0.90, 0.72);
    float flicker = 0.7 + 0.3 * vnoise(vec2(gTime * 9.0, float(h & 255u)), 0xF11Cu);
    return c * mix(0.45, 1.0, hf(sh, 3u)) * flicker;
}

Room makeRoom(uint h, float litChance, float tintSel, bool shop, vec3 shopCol) {
    Room r;
    r.h = h;
    r.lit = shop || hf(h, 1u) < litChance;
    // Some otherwise dark rooms have the television on.
    r.tv = !shop && !r.lit && hf(h, 12u) < 0.14;
    // Most rooms glow warm: homes (tintSel < 0.5) almost all incandescent-warm,
    // offices warm to neutral, with the odd cold fluorescent room among them.
    float pick = hf(h, 2u), kt = hf(h, 13u);
    float k = tintSel < 0.5 ? (pick < 0.88 ? mix(2700.0, 3300.0, kt) : mix(3600.0, 4500.0, kt))
                            : (pick < 0.65 ? mix(2900.0, 3600.0, kt) : pick < 0.9 ? mix(3800.0, 4500.0, kt) : mix(5000.0, 6500.0, kt));
    r.light = kelvin(k);
    r.bright = mix(0.6, 1.7, hf(h, 3u));   // kept below where the tone curve bleaches a warm room white
    if (r.tv) { r.lit = true; r.light = tvGlow(h); r.bright = 0.45; }
    if (shop) { r.light = shopCol; r.bright = mix(0.9, 1.7, hf(h, 3u)); }
    r.wall = mix(vec3(0.35, 0.33, 0.30), vec3(0.55, 0.50, 0.45), hf(h, 4u)) * (0.5 + 0.5 * hf(h, 5u));
    r.floorC = vec3(0.12, 0.10, 0.09) * (0.6 + 0.8 * hf(h, 6u));
    float b = hf(h, 7u);
    r.blinds = (!shop && b < 0.45) ? mix(0.15, 0.95, hf(h, 8u)) : 0.0;
    r.curtain = (!shop && b > 0.86) ? 1.0 : 0.0;
    r.curtainC = mix(vec3(0.9, 0.25, 0.12), vec3(0.85, 0.65, 0.35), hf(h, 9u));
    r.furnZ = mix(0.3, 0.65, hf(h, 10u));
    r.person = r.lit && !r.tv && hf(h, 11u) < (shop ? 0.4 : 0.08);
    r.shop = shop;
    return r;
}

// Furniture / figure silhouette at the furniture plane. x in metres, y in metres.
float furniture(float x, float y, float W, Room r) {
    uint h = r.h;
    float a1 = W * (0.05 + 0.35 * hf(h, 20u)), b1 = a1 + W * (0.2 + 0.3 * hf(h, 21u)), c1 = 0.7 + 0.5 * hf(h, 22u);
    float m = (x > a1 && x < b1 && y < c1) ? 1.0 : 0.0;
    float a2 = W * (0.55 + 0.3 * hf(h, 23u)), b2 = a2 + W * (0.07 + 0.12 * hf(h, 24u)), c2 = 1.4 + 0.7 * hf(h, 25u);
    if (hf(h, 26u) < 0.6 && x > a2 && x < b2 && y < c2) m = 1.0;
    if (r.person) {
        float px = W * (0.5 + 0.33 * sin(gTime * (0.12 + 0.1 * hf(h, 27u)) + 6.28 * hf(h, 28u)));
        float dx = abs(x - px);
        float body = (y < 1.48 && dx < mix(0.13, 0.23, smoothstep(0.0, 1.3, y))) ? 1.0 : 0.0;
        float head = length(vec2(dx, y - 1.63)) < 0.12 ? 1.0 : 0.0;
        m = max(m, max(body, head) * 2.0);
    }
    return m;
}

// TV rooms: the set stands against the back wall; screen centre and half size.
vec3 tvPos(Room r, float W, float D) { return vec3(W * (0.3 + 0.4 * hf(r.h, 16u)), 0.95, D); }
vec2 tvHalf(float W) { return vec2(min(0.55, W * 0.2), 0.32); }

/* Light leaving interior point p (room box: x along the wall, y up, z into the
 * room) with albedo alb. Lit rooms have a ceiling panel (half extents `panel`);
 * TV rooms are dark but for the screen and the glow it throws into the room. */
// Light falling into a dark room through its window (gWinE): strongest by the
// glass and fading with depth, so whatever hangs at the glass, the furniture
// and the back wall each read apart. Surfaces facing the window catch the most.
vec3 windowLight(vec3 p, bool ceiling, float D) {
    float facing = ceiling ? 0.45 : p.z > D - 0.01 ? 1.0 : p.y < 0.01 ? 0.7 : 0.5;
    return gWinE * facing / (1.0 + p.z * p.z);
}

// Curtains and blinds hang just inside the glass: in a dark room they catch
// the light coming in more than anything beyond them.
vec3 curtainLight(Room r, float fold) {
    return r.lit ? r.curtainC * r.light * r.bright * 0.35 * fold : mix(r.curtainC, vec3(0.5), 0.4) * gWinE * 0.6 * fold;
}
vec3 blindLight(Room r, float slat) {
    return r.lit ? r.light * r.bright * 0.28 * (0.55 + 0.45 * slat) : vec3(0.45) * gWinE * (0.35 + 0.65 * slat);
}

vec3 roomLight(Room r, vec3 p, vec3 alb, bool ceiling, float W, float H, float D, vec2 panel) {
    if (!r.lit) return alb * windowLight(p, ceiling, D);
    if (r.tv) {
        vec3 tp = tvPos(r, W, D);
        vec2 q = abs(p.xy - tp.xy), hs = tvHalf(W);
        if (p.z > D - 0.01 && q.x < hs.x && q.y < hs.y) return r.light * 3.0;
        vec3 dl = p - tp;
        float fwd = clamp(-dl.z / max(length(dl), 1e-3), 0.0, 1.0);
        return alb * (r.light * 1.6 * (0.2 + 0.8 * fwd) / (1.0 + dot(dl, dl) * 0.6) + windowLight(p, ceiling, D));
    }
    if (ceiling && abs(p.x - W * 0.5) < panel.x && abs(p.z - D * 0.5) < panel.y)
        return r.light * r.bright * 5.0;
    vec3 dl = p - vec3(W * 0.5, H - 0.05, D * 0.5);
    return alb * r.light * r.bright * (0.18 + 2.2 / (1.0 + dot(dl, dl) * 0.35));
}

vec3 interiorColor(vec2 cell, vec3 d, float W, float H, float D, Room r) {
    vec3 o = vec3(cell.x * W, cell.y * H, 0.0);
    d.z = max(d.z, 1e-3);
    float tx = d.x > 0.0 ? (W - o.x) / d.x : (d.x < 0.0 ? -o.x / d.x : 1e9);
    float ty = d.y > 0.0 ? (H - o.y) / d.y : (d.y < 0.0 ? -o.y / d.y : 1e9);
    float tz = (D - o.z) / d.z;
    float t = min(tx, min(ty, tz));
    vec3 p = o + d * t;

    vec3 alb;
    bool ceiling = false;
    if (t == tz) {
        alb = r.wall;
        // a door or poster on the back wall
        float dx = abs(p.x - W * (0.25 + 0.5 * hf(r.h, 30u)));
        if (r.shop) alb = shelves(p.xy, r.wall, r.h);
        else if (dx < 0.45 && p.y < 2.1) alb *= 0.45;
        else if (hf(r.h, 31u) < 0.3 && dx > 0.8 && dx < 1.4 && p.y > 1.1 && p.y < 1.9)
            alb = mix(neonColor(r.h >> 3), vec3(0.45), 0.6) * 0.4;   // a print on the wall
    } else if (t == tx) {
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (d.y > 0.0) {
        alb = vec3(0.55);
        ceiling = true;
    } else {
        alb = r.floorC;
    }

    vec3 col = roomLight(r, p, alb, ceiling, W, H, D, vec2(W * 0.2, min(D * 0.15, 0.6)));

    // furniture / figure layer
    float tf = (D * r.furnZ) / d.z;
    if (tf < t) {
        vec3 pf = o + d * tf;
        if (pf.x > 0.0 && pf.x < W && pf.y > 0.0) {
            float m = furniture(pf.x, pf.y, W, r);
            if (m > 0.5) {
                vec3 sil = r.tv ? r.light * 0.006 : r.lit ? r.light * r.bright * (m > 1.5 ? 0.03 : 0.10)
                                 : 0.06 * gWinE / (1.0 + pf.z * pf.z);   // dark shapes against the dim room
                col = sil;
            }
        }
    }
    return col;
}

/* What a corner room's far window looks out on. The real scene behind the
 * building is not available, so this models a typical one from the window's
 * height: a street below, lit by lamps and shop spill near the far kerb, and
 * the facade across it (about 26 m away) with lit shopfronts and signs at
 * street level, windows above and the haze beyond its roofline. */
vec3 throughView(vec3 dir, float eyeY) {
    const float L = 26.0;
    float hl = max(length(dir.xz), 1e-3);
    float tWall = L / hl;
    float yWall = eyeY + dir.y * tWall;
    float sAlong = atan(dir.z, dir.x) * L;      // metres along the far side of the street
    vec3 c;
    float dist;
    if (yWall < 0.0) {
        float tg = eyeY / max(-dir.y, 1e-3);
        float across = hl * tg;
        dist = tg;
        float lampAt = (fract(sAlong / 27.0) - 0.5) * 27.0;
        float pool = exp(-lampAt * lampAt / 60.0) * exp(-(across - L + 6.0) * (across - L + 6.0) / 50.0);
        c = vec3(0.030, 0.030, 0.034) + lampColor(0) * pool * 0.25
          + vec3(0.55, 0.35, 0.45) * 0.08 * exp(-(L - across) / 3.0);
    } else {
        dist = tWall;
        float roof = 18.0 + 90.0 * hfloat(hash2i(0x0B1Du, int(floor(sAlong / 22.0)), 0));
        if (yWall > roof) return hazeColor(dir);
        if (yWall < 4.6) {
            uint h = hash2i(0x5B0Bu, int(floor(sAlong / 6.0)), 0);
            float fs = fract(sAlong / 6.0);
            float win = band(fs, 0.06, 0.94, 0.02) * band(yWall, 0.5, 3.4, 0.05);
            bool open = hfloat(h) < 0.7;
            c = mix(vec3(0.03), open ? shopColor(h) * 0.6 : vec3(0.02), win);
            if (open && hf(h, 3u) < 0.8)
                c += neonColor(h >> 6) * band(yWall, 3.7, 4.4, 0.05) * band(fs, 0.1, 0.9, 0.02) * 1.5;
        } else {
            float fx = sAlong / 2.8, fy = (yWall - 4.6) / 3.5;
            uint h = hash2i(0x0DD5u, int(floor(fx)), int(floor(fy)));
            float win = band(fract(fx), 0.2, 0.8, 0.05) * band(fract(fy), 0.25, 0.75, 0.05) * step(hfloat(h), 0.35);
            vec3 tint = hf(h, 1u) < 0.6 ? vec3(1.0, 0.68, 0.40) : vec3(0.72, 0.88, 1.0);
            c = mix(vec3(0.025, 0.025, 0.030), tint * (0.3 + 0.6 * hf(h, 2u)), win);
        }
    }
    return mix(c, hazeColor(dir), 1.0 - exp(-dist / 380.0));
}

/* A corner room or corner shop: glazed at the front and on its corner side
 * wall(s), glassL at x = 0 and/or glassR at x = W, through the neighbouring
 * face's corner window. Seen from either face it is the same box: this face's
 * corner bay wide and the neighbouring face's corner bay deep. The side
 * window openings are given in metres in the room (zL/zR: depth range on the
 * left/right wall, yA: height range), so they line up with the windows seen
 * from outside. A ray reaching side glass leaves the building. The furniture
 * is a traced square desk or counter in the inner corner, so it too is the
 * same object from both sides. */
vec3 cornerInterior(vec2 cell, vec3 d, float W, float H, float D, Room r, bool glassL, bool glassR,
                    vec3 worldDir, float eyeY, vec2 zL, vec2 zR, vec2 yA) {
    vec3 o = vec3(cell.x * W, cell.y * H, 0.0);
    d.z = max(d.z, 1e-3);
    float tx = d.x > 0.0 ? (W - o.x) / d.x : (d.x < 0.0 ? -o.x / d.x : 1e9);
    float ty = d.y > 0.0 ? (H - o.y) / d.y : (d.y < 0.0 ? -o.y / d.y : 1e9);
    float tz = (D - o.z) / d.z;
    float t = min(tx, min(ty, tz));

    // desk / counter island against the inner walls
    float s = min(W, D) * 0.45;
    float bx = (glassR && !glassL) ? 0.0 : (glassL && !glassR) ? W - s : 0.5 * (W - s);
    vec3 bmin = vec3(bx, 0.0, D - s), bmax = vec3(bx + s, r.shop ? 1.0 : 0.75, D);
    vec3 dd = vec3(abs(d.x) < 1e-5 ? 1e-5 : d.x, abs(d.y) < 1e-5 ? 1e-5 : d.y, d.z);
    vec3 t0 = (bmin - o) / dd, t1 = (bmax - o) / dd;
    vec3 tn = min(t0, t1), tf = max(t0, t1);
    float tin = max(max(tn.x, tn.y), tn.z), tout = min(min(tf.x, tf.y), tf.z);
    bool desk = tin < tout && tin > 0.0 && tin < t;
    if (desk) t = tin;
    vec3 p = o + d * t;

    vec3 alb;
    bool ceiling = false;
    if (desk) {
        alb = vec3(0.10, 0.085, 0.07) * (tin == tn.y ? 1.5 : 0.6);
    } else if (t == tx && ((d.x > 0.0 && glassR) || (d.x < 0.0 && glassL))) {
        vec2 za = d.x > 0.0 ? zR : zL;
        float ax = (p.z - za.x) / max(za.y - za.x, 1e-3), ay = (p.y - yA.x) / max(yA.y - yA.x, 1e-3);
        if (ax > 0.0 && ax < 1.0 && ay > 0.0 && ay < 1.0) {
            // the room's curtain and blinds cover this window too
            float open = 0.18 + 0.2 * hf(r.h, 41u);
            if (r.curtain > 0.5 && (ax < open || ax > 1.0 - open))
                return curtainLight(r, 0.75 + 0.25 * sin(ax * 60.0));
            if (r.blinds > 0.0 && ay > 1.0 - r.blinds) {
                float sl = fract(p.y / 0.055);
                return blindLight(r, smoothstep(0.08, 0.2, sl) * smoothstep(0.92, 0.8, sl));
            }
            float edge = min(min(ax, 1.0 - ax) * (za.y - za.x), min(ay, 1.0 - ay) * (yA.y - yA.x));
            vec3 outside = throughView(worldDir, eyeY) + (r.lit ? r.light * r.bright * 0.025 : vec3(0.0));
            return mix(vec3(0.012, 0.012, 0.014), outside, smoothstep(0.03, 0.06, edge));
        }
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (t == tz) {
        alb = r.shop ? shelves(p.xy, r.wall, r.h) : r.wall;
    } else if (t == tx) {
        alb = r.shop ? shelves(p.zy, r.wall, r.h ^ 0x55u) : r.wall * 0.85;
    } else if (d.y > 0.0) {
        alb = vec3(0.55);
        ceiling = true;
    } else {
        alb = r.floorC;
    }

    float panel = min(min(W, D) * 0.2, 0.6);   // square, so it matches from both faces
    return roomLight(r, p, alb, ceiling, W, H, D, vec2(panel));
}
