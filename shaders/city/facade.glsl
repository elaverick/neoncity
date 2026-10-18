// Part of shaders/city.frag, which includes the parts in order: facades.

/* ======================================================================
 * Facades
 * ==================================================================== */

vec3 wallLighting(vec3 p, vec3 n, vec3 V, float shin, float specK, float height, out vec3 spec) {
    vec3 diff = ambientLight(n, height);
    spec = vec3(0.0);
    cityLights(p, n, V, shin, specK, -1.0, diff, spec);
    diff += neonBounce(gWorld, height) * 1.5;
    return diff;
}

vec3 shadeFacade(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    // Facade data (facade_mat in src/city/mesh_building.c):
    //   prm.y section; prm.z at-ground | ground mode << 1 | window << 3 | bands << 5 | fine bays << 6;
    //   prm.w palette | shop level << 3 | room or core behind the lobby glass << 6; vFacePack face width | next face width << 12 (1/16 m);
    //   vAux previous face width | face << 12 | next face << 18 | corners << 24 | plant floors << 28.
    int section = int(prm.y);
    bool atGround = (prm.z & 1u) == 1u;
    uint groundMode = (prm.z >> 1) & 3u;          // 0 shops, 1 lobby, 2 double-height lobby, 3 void deck
    int type = int((prm.z >> 3) & 3u);            // 0 punched, 1 curtain wall, 2 ribbon, 3 slit
    bool bands = (prm.z & 32u) != 0u, fine = (prm.z & 64u) != 0u;
    uint palette = prm.w & 7u;
    float shopP = float((prm.w >> 3) & 7u) * 0.125;
    int faceDir = int((vAux >> 12) & 63u), nextFace = int((vAux >> 18) & 63u);
    uint corners = (vAux >> 24) & 15u;
    bool endShare = (corners & 2u) != 0u, start90 = (corners & 4u) != 0u, end90 = (corners & 8u) != 0u;
    uint plantCode = vAux >> 28;
    vec3 T = vec3(n.z, 0.0, -n.x);

    uint sh = styleHash(seed);
    float floorH = facadeFloorH(seed);
    float groundH = facadeGroundH(seed) * (groundMode == 2u ? 2.0 : 1.0);
    float bayW = fine ? 1.6 + 0.8 * hf(sh, 4u) : 2.4 + 2.6 * hf(sh, 4u);
    float winW, winH;
    if (type == 0)      { winW = mix(0.45, 0.7, hf(sh, 5u)); winH = mix(0.45, 0.65, hf(sh, 9u)); }
    else if (type == 1) { winW = 0.94; winH = 0.86; }
    else if (type == 2) { winW = 1.0;  winH = mix(0.4, 0.55, hf(sh, 9u)); }
    else                { winW = mix(0.2, 0.32, hf(sh, 5u)); winH = mix(0.7, 0.85, hf(sh, 9u)); }
    float litFrac = 0.16 + 0.42 * hf(sh, 6u);
    float tintSel = hf(sh, 8u);

    // Wall material: the section's palette (choose_surfaces in src/gen/building.c)
    vec3 wallAlb = palette == 0u ? vec3(0.085, 0.080, 0.075) :   // concrete
                   palette == 1u ? vec3(0.080, 0.060, 0.045) :   // grimy brick-brown
                   palette == 2u ? vec3(0.035, 0.055, 0.060) :   // teal metal panels
                   palette == 3u ? vec3(0.110, 0.105, 0.095) :   // pale tile
                   palette == 4u ? vec3(0.020, 0.022, 0.028) :   // black glass
                   palette == 5u ? vec3(0.105, 0.090, 0.068) :   // warm stone
                   palette == 6u ? vec3(0.070, 0.050, 0.030) :   // bronze
                                   vec3(0.014, 0.024, 0.040);    // blue glass
    wallAlb *= (0.7 + 0.6 * hf(sh, 10u)) * 1.4;
    bool glassWall = palette == 4u || palette == 7u || type == 1;
    // Window glass transmission: clear float glass is faintly green; tinted
    // curtain walls and bronze-clad towers carry their tint into the rooms.
    vec3 glassT = palette == 7u ? vec3(0.72, 0.79, 0.86) :   // blue glass
                  palette == 4u ? vec3(0.58, 0.59, 0.60) :   // grey (black) glass
                  palette == 6u ? vec3(0.86, 0.74, 0.58) :   // bronze
                                  vec3(0.87, 0.89, 0.86);    // clear

    // uv.x = metres along the face; vAbs the height above the building's base.
    float u = uv.x, vAbs = p.y - gBase;
    float faceW = float(vFacePack & 4095u) / 16.0, nextW = float(vFacePack >> 12) / 16.0, prevW = float(vAux & 4095u) / 16.0;
    float relV = vAbs - SIDEWALK_HEIGHT - groundH;
    // Derivatives must be taken in uniform control flow, before any per-pixel branching.
    float dU = fwidth(u), dV = fwidth(vAbs);
    // Whole bays only: stretch the bay so every face ends on a full window at its corners.
    float baseBayW = bayW;
    float nBays = max(1.0, floor(faceW / bayW + 0.5));
    bayW = faceW / nBays;
    float prevBayW = prevW / max(1.0, floor(prevW / baseBayW + 0.5));   // the neighbours' corner bays
    float nextBayW = nextW / max(1.0, floor(nextW / baseBayW + 0.5));

    // ---------------- plinth ----------------
    // Below the ground floor, where the street falls away from the building's
    // base (Mesh.footing, and the steps up to its entrances: src/city/mesh_building.c):
    // a dark stone base course.
    if (atGround && vAbs < SIDEWALK_HEIGHT) {
        vec3 spec = vec3(0.0);
        vec3 diffL = wallLighting(p, n, V, 20.0, 0.12, vAbs, spec);
        float grain = 0.8 + 0.4 * vnoise(vec2(u * 2.0, vAbs * 6.0), seed & 0xFFFFu);
        return vec3(0.055, 0.052, 0.048) * grain * diffL + spec;
    }

    // ---------------- lobbies and void decks ----------------
    if (atGround && relV < 0.0 && groundMode != 0u) {
        float yy = vAbs - SIDEWALK_HEIGHT;
        float glassTop = groundH - (groundMode == 2u ? 1.2 : 0.7);
        float fs = fract(u / 1.5);
        float aa = dU / 1.5 * 1.5 + dV * 0.2;
        float inWin = band(yy, 0.08, glassTop, dV + 0.01) * (1.0 - band(fs, 0.0, 0.04, aa) - band(fs, 0.96, 1.0, aa));
        float lodR = smoothstep(0.08, 0.4, dU / 8.0);
        // a pane over a real lobby (below) shows no light on its own surface: skip the lamps' shadow rays there
        bool realPane = (prm.z & 128u) != 0u && (prm.w & 64u) == 0u && inWin > 0.5 && length(rel) < REAL_GLASS_DIST && lodR < 0.75;
        vec3 spec = vec3(0.0);
        vec3 diffL = realPane ? vec3(0.0) : wallLighting(p, n, V, 30.0, 0.15, vAbs, spec);
        vec3 wallC = wallAlb * 0.9 * diffL + spec;
        // light lines set into the mullions (every first, second or third) in the lobby's accent colour
        if (groundMode != 3u && hf(sh, 63u) < 0.8) {
            float every = 1.0 + floor(hf(sh, 62u) * 3.0), k = floor(u / 1.5 + 0.5);
            float dm = abs(u - k * 1.5), w = max(0.012, 0.5 * dU);
            if (mod(k, every) < 0.5)
                wallC += lobbyAccent(seed) * 6.0 * (1.0 - smoothstep(w * 0.6, w, dm)) * (0.012 / w) * band(yy, 0.08, glassTop, dV + 0.01);
        }
        if ((prm.w & 64u) != 0u) {
            // A closed room or a core of the real lobby stands behind this
            // stretch (emit_walls in src/city/mesh_building.c): opaque back-painted glass.
            float F = glassFresnel(n, V);
            vec3 pane = vec3(0.012, 0.013, 0.015) * diffL + envReflection(reflect(-V, n)) * F + paneSheen(p, n, V);
            if ((prm.w & 128u) != 0u) {
                // an arcology's market blocks stand behind: backlit poster lightboxes in some panes
                float cell = floor(u / 1.5);
                uint lh = hash3i(seed ^ 0x119Bu, section, faceDir, int(cell));
                float yy = vAbs - SIDEWALK_HEIGHT;
                if (hfloat(lh) < 0.5) {
                    // a poster at eye level: a gradient ground, a lit figure, a column of glyphs
                    vec2 q = vec2(fract(u / 1.5), (yy - 0.8) / 1.9);
                    float box = band(q.x, 0.08, 0.92, dU / 1.5 + 0.01) * band(q.y, 0.0, 1.0, dV / 1.9 + 0.01);
                    vec3 c1 = neonColor(lh >> 4), c2 = neonColor(lh >> 8);
                    vec3 art = mix(c1 * 0.15, c2 * 0.5, q.y);
                    vec2 fc = (q - vec2(0.58, 0.55)) * vec2(1.0, 1.6);
                    art += vec3(1.0, 0.8, 0.65) * exp(-dot(fc, fc) * 14.0) * 0.9;
                    vec2 gq = vec2((q.x - 0.12) / 0.18, fract(q.y * 6.0));
                    if (q.x > 0.12 && q.x < 0.3) art += c1 * 1.2 * (1.0 - smoothstep(0.04, 0.1, glyphDist(hash_combine(lh, uint(q.y * 6.0)), gq)));
                    pane += art * 0.6 * box * (0.88 + 0.12 * sin(gTime * 1.3 + hfloat(lh) * 6.0));
                }
            }
            glassRefl(n, F * clamp(inWin, 0.0, 1.0));
            return mix(wallC, pane, clamp(inWin, 0.0, 1.0));
        }
        if ((prm.z & 128u) != 0u && inWin > 0.5 && length(rel) < REAL_GLASS_DIST) {
            // A real lobby stands behind this glass (src/gen/interior.c): the pane
            // covers part of the pixel and the geometry shows through the rest.
            // Far off, where a pane is a few pixels, it settles to the lobby's glow.
            float F = glassFresnel(n, V);
            vec3 refl = envReflection(reflect(-V, n)) * F + paneSheen(p, n, V);
            if (lodR < 0.75) {
                float cov = paneCoverage(F);
                glassRefl(n, min(F / cov, 1.0));
                return refl / cov + glassT * 0.004;
            }
            glassRefl(n, F);
            return lobbyLight(seed) * 0.3 * (1.0 - F) + refl;
        }
        uint lh = hash3i(seed ^ 0x10BBu, section, faceDir, int(floor(u / 8.0)));
        Room r = makeRoom(lh, 1.0, tintSel, false, vec3(0.0));
        r.lit = true; r.blinds = 0.0; r.curtain = 0.0;
        r.light = tintSel < 0.5 ? kelvin(4000.0) : kelvin(3000.0);
        r.bright = groundMode == 3u ? 0.7 : 1.35;
        r.wall = vec3(0.55, 0.53, 0.50);
        r.person = hf(lh, 11u) < 0.3;
        vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
        vec3 inner = interiorColor(vec2(fract(u / 8.0), clamp(yy / groundH, 0.0, 1.0)), d, 8.0, groundH, 9.0, r);
        float F = glassFresnel(n, V);
        vec3 glass = inner * (1.0 - F) + envReflection(reflect(-V, n)) * F;
        glass += paneSheen(p, n, V);
        float lodL = smoothstep(0.08, 0.4, dU / 8.0);
        glass = mix(glass, r.light * r.bright * 0.5, lodL);
        glassRefl(n, F * (1.0 - lodL) * clamp(inWin, 0.0, 1.0));
        return mix(wallC, glass, clamp(inWin, 0.0, 1.0));
    }

    // ---------------- storefronts ----------------
    if (atGround && relV < 0.0) {
        float along = u;
        float fs = fract(along / 6.0);
        uint h = shopHash(seed, section, faceDir, nextFace, endShare, along, faceW);
        bool lit = hfloat(h) < shopP;
        float yy = vAbs - SIDEWALK_HEIGHT;
        float winTop = groundH - 1.5;
        float dFs = dU / 6.0;
        float aa = dFs * 1.5 + dV * 0.2;
        float inWin = band(fs, 0.05, 0.95, aa) * band(yy, 0.45, winTop, dV + 0.01);
        vec3 spec;
        vec3 diffL = wallLighting(p, n, V, 20.0, 0.1, vAbs, spec);
        vec3 wallC = wallAlb * 0.8 * diffL + spec;
        if (lit) {
            Room r = makeRoom(h, 1.0, 0.0, true, shopColor(h));
            vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
            // Shop bays are 6 m from each face's start, so each face ends on a part bay.
            // A corner shop is this face's corner bay wide and the neighbouring face's
            // corner bay deep; its side window spans 0.3 m in from each end of that bay.
            float seg = floor(along / 6.0), lastSeg = floor((faceW - 0.01) / 6.0);
            float tailW = faceW - 6.0 * lastSeg, tailP = prevW - 6.0 * floor((prevW - 0.01) / 6.0);
            bool shopL = start90 && seg <= 0.0, shopR = end90 && seg >= lastSeg;
            vec3 inner;
            if (shopL || shopR) {
                float Ws = seg >= lastSeg ? tailW : min(6.0, faceW);
                float Ds = shopL && shopR ? min(tailP, min(6.0, nextW)) : shopR ? min(6.0, nextW) : tailP;
                vec2 zR = vec2(0.3, min(5.7, Ds)), zL = vec2(max(0.0, Ds - 5.7), Ds - 0.3);
                inner = cornerInterior(vec2((along - 6.0 * seg) / Ws, clamp(yy / groundH, 0.0, 1.0)), d, Ws, groundH, Ds,
                                       r, shopL, shopR, -V, vAbs, zL, zR, vec2(0.45, winTop));
            } else {
                inner = interiorColor(vec2(fs, clamp(yy / groundH, 0.0, 1.0)), d, 6.0, groundH, 5.0, r);
            }
            float F = glassFresnel(n, V);
            vec3 glass = inner * (1.0 - F) + envReflection(reflect(-V, n)) * F + paneSheen(p, n, V);
            // mullions
            float mull = band(fract(fs * 3.0), 0.0, 0.02, aa * 3.0) + band(fract(fs * 3.0), 0.98, 1.0, aa * 3.0);
            glass = mix(glass, vec3(0.01), clamp(mull, 0.0, 1.0));
            // farther away, just the average glow
            float lod = smoothstep(0.08, 0.4, dFs);
            glass = mix(glass, r.light * r.bright * 0.5, lod);
            glassRefl(n, F * (1.0 - clamp(mull, 0.0, 1.0)) * (1.0 - lod) * inWin);
            return mix(wallC, glass, inWin);
        } else {
            // rolled-down shutter
            float rib = 0.6 + 0.4 * smoothstep(0.3, 0.5, abs(fract(yy / 0.09) - 0.5) * 2.0);
            float grime = 0.5 + 0.7 * fbm(vec2(along * 0.8, yy * 0.8), 0x5417u);
            vec3 shut = vec3(0.07, 0.072, 0.075) * rib * grime;
            float graf = smoothstep(0.62, 0.66, fbm(vec2(along, yy) * 0.9, h));
            shut = mix(shut, neonColor(h >> 6) * 0.08, graf * 0.8);
            vec3 sc = shut * diffL + spec;
            return mix(wallC, sc, inWin);
        }
    }

    // ---------------- upper floors ----------------
    float fx = u / bayW, fy = relV / floorH;
    float bayIdx = floor(fx), floorIdx = floor(fy);
    float fu = fract(fx), fv = fract(fy);
    float fwU = dU / bayW, fwV = dV / floorH;
    float x0 = 0.5 - winW * 0.5, x1 = 0.5 + winW * 0.5;
    float sill = (1.0 - winH) * 0.42;
    float y0 = sill, y1 = sill + winH;
    float cov = filteredPulse(fx, x0, x1, fwU * 1.2) * filteredPulse(fy, y0, y1, fwV * 1.2);
    float lodT = smoothstep(0.05, 0.22, max(fwU, fwV));
    float farT = smoothstep(0.45, 1.4, max(fwU, fwV));

    // Corner rooms belong to both faces; key them (and their floor) by the face they start.
    // Square convex corners get the corner trace, glazed on the neighbouring face too.
    ivec2 rk = cornerKey(faceDir, nextFace, endShare, int(bayIdx), int(nBays) - 1);
    bool glassL = start90 && int(bayIdx) <= 0, glassR = end90 && int(bayIdx) >= int(nBays) - 1;
    bool cornerRoom = glassL || glassR;
    // Louvred plant floors at intervals up tall towers.
    if (plantCode > 0u) {
        int every = 6 + 3 * int(plantCode);
        if (floorIdx > 0.0 && int(floorIdx) % every == every - 1) cov = 0.0;
    }
    bool plantFloor = plantCode > 0u && floorIdx > 0.0 && int(floorIdx) % (6 + 3 * int(plantCode)) == 5 + 3 * int(plantCode);
    // per-floor correlation: some floors are whole lit offices, some are dark
    uint floorH_ = hash3i(seed ^ 0xF100u, int(floorIdx), rk.x, section);
    float litChance = litFrac * (0.25 + 1.5 * hfloat(floorH_));
    uint roomHash = hash3i(seed ^ (uint(rk.x) * 0x9E3779B1u), rk.y, int(floorIdx), section);
    Room room = makeRoom(roomHash, litChance, tintSel, false, vec3(0.0));

    // wall surface
    vec3 wspec;
    float wallShin = glassWall ? 80.0 : 12.0;
    float wallSpecK = glassWall ? 0.4 : 0.05;
    vec3 wdiff = wallLighting(p, n, V, wallShin, wallSpecK, vAbs, wspec);
    // a dark room has only what reaches its window: the lamps, neon and city glow on the facade, through the glass
    gWinE = (wdiff + mix(CLOUD_DECK, vec3(0.03, 0.035, 0.045), 0.5) * 0.45) * glassT;   // the window's view: overcast and facades across
    float streak = vnoise(vec2(u * 1.3, vAbs * 0.04), seed & 0xFFFFu);
    float grime = mix(0.65, 1.15, streak) * mix(0.8, 1.0, smoothstep(0.0, 25.0, vAbs));
    vec3 wallC = wallAlb * grime;
    if (type == 0 || type == 3) {
        // slab edges and pilasters
        float slab = band(fv, 0.0, 0.05, fwV + 0.005) + band(fv, 0.95, 1.0, fwV + 0.005);
        wallC *= 1.0 - 0.35 * clamp(slab, 0.0, 1.0) * (1.0 - lodT);
        // rust drips under windows
        float drip = band(fu, x0, x1, fwU + 0.01) * band(fv, y0 - 0.22, y0, fwV + 0.01);
        wallC *= 1.0 - 0.3 * drip * (1.0 - lodT);
    }
    if (bands) {
        // spandrel bands: a contrasting panel between the windows of each floor
        vec3 accent = glassWall ? vec3(0.05, 0.055, 0.06) : wallAlb * vec3(0.55, 0.6, 0.7);
        float sp = 1.0 - band(fv, y0 - 0.03, y1 + 0.03, fwV + 0.005);
        wallC = mix(wallC, accent, clamp(sp, 0.0, 1.0) * (1.0 - farT * 0.5));
    }
    if (plantFloor) {
        // louvres: tight horizontal metal slats
        float sl = fract(vAbs / 0.22);
        wallC = vec3(0.06, 0.062, 0.065) * mix(0.55, 1.0, smoothstep(0.3, 0.6, sl) * (1.0 - lodT) + lodT * 0.75);
    }
    vec3 wallLit = wallC * wdiff + wspec;
    float wallW = 0.0;   // weight of the sky reflection in wallLit (glass curtain walls)
    if (glassWall) {
        float F = glassFresnel(n, V);
        wallW = 0.08 + F * 0.9;
        wallLit += envReflection(reflect(-V, n)) * wallW + paneSheen(p, n, V);
    }
    // shop signs and light below throw colour up the lower facade
    if (atGround && relV < 4.0) {
        uint h = shopHash(seed, section, faceDir, nextFace, endShare, u, faceW);
        if (groundMode == 0u && hfloat(h) < shopP) wallLit += wallC * shopColor(h) * exp(-relV * 0.6) * 1.2;
        else if (groundMode != 0u) wallLit += wallC * LOBBY_SPILL * exp(-relV * 0.6) * 0.8;
    }

    // window: near = full interior mapping, mid = per-room flat glow, far = average
    vec3 R = reflect(-V, n);
    float F = glassFresnel(n, V);
    vec3 refl = envReflection(R);
    // Privacy film across the bottom of some panes: dark, on the glass itself.
    float film = hf(roomHash, 52u) < 0.3 ? mix(0.2, 0.38, hf(roomHash, 53u)) : 0.0;
    vec3 roomAvg = room.lit ? room.light * room.bright * 0.42 : gWinE * 0.08;
    if (room.blinds > 0.0 && room.lit) roomAvg *= 1.0 - 0.4 * room.blinds;
    if (!room.lit) roomAvg += gWinE * 0.2 * max(room.blinds, room.curtain * 0.5);
    roomAvg *= 1.0 - 0.9 * film;
    // no two panes sit quite flat in their frames: each catches a slightly different reflection
    vec3 paneN = normalize(n + T * (hf(roomHash, 50u) - 0.5) * 0.08 + vec3(0.0, (hf(roomHash, 51u) - 0.5) * 0.08, 0.0));
    vec3 sheen = paneSheen(p, paneN, V);
    vec3 winMid = roomAvg * glassT * (1.0 - F) + refl * F + sheen;

    vec3 winNear = winMid;
    float nearW = F;     // ...and in the near window, after its frame
    if (lodT < 1.0 && cov > 0.0) {
        // A corner room is the neighbouring face's corner bay deep: the same box from either side.
        float W = bayW, H = floorH;
        float D = glassL && glassR ? min(prevBayW, nextBayW) : glassR ? nextBayW : glassL ? prevBayW : bayW * mix(1.3, 2.4, hf(roomHash, 40u));
        vec3 d = vec3(dot(-V, T), -V.y, -dot(-V, n));
        vec3 inner = cornerRoom ? cornerInterior(vec2(fu, fv), d, W, H, D, room, glassL, glassR, -V, vAbs,
                                                 vec2(x0, x1) * D, vec2(x0, x1) * D, vec2(y0, y1) * H)
                                : interiorColor(vec2(fu, fv), d, W, H, D, room);
        float wy = (fv - y0) / winH, wx = (fu - x0) / winW;
        // Curtains and blinds hang 8 cm inside the glass: seen at an angle they
        // slide against the frame, apart from the pane and the room.
        vec2 hang = d.xy / max(d.z, 0.05) * 0.08;
        float hx = wx + hang.x / (W * winW), hy = wy + hang.y / (H * winH);
        if (room.curtain > 0.5) {
            float open = 0.18 + 0.2 * hf(roomHash, 41u);
            float cMask = 1.0 - band(hx, open, 1.0 - open, 0.01);
            inner = mix(inner, curtainLight(room, 0.75 + 0.25 * sin(hx * 60.0)), cMask);
        }
        if (room.blinds > 0.0) {
            float s = fract((fv * H + hang.y) / 0.055);
            float slat = smoothstep(0.08, 0.2, s) * smoothstep(0.92, 0.8, s);
            slat = mix(slat, 0.75, smoothstep(0.25, 0.7, fwV * H / 0.055));   // slats finer than a pixel blur to their average
            float bMask = smoothstep(1.0 - room.blinds - 0.004, 1.0 - room.blinds + 0.004, hy);
            inner = mix(inner, blindLight(room, slat), bMask * (0.7 + 0.25 * slat));   // the room shows between the slats
        }
        float filmM = 1.0 - smoothstep(film - fwV / winH - 0.003, film + fwV / winH + 0.003, wy);
        inner *= 1.0 - 0.92 * filmM * step(0.001, film);
        winNear = inner * glassT * (1.0 - F) + refl * F + sheen;
        // window frame & mullions
        float fw = 0.05;
        float frameU = fw / (W * winW), frameV = fw / (H * winH);
        float frame = 1.0 - band(wx, frameU, 1.0 - frameU, fwU / winW + 0.002) * band(wy, frameV, 1.0 - frameV, fwV / winH + 0.002);
        if (type == 2) frame = max(frame, band(fract(u / 1.25), 0.0, 0.03, dU / 1.25 + 0.002));
        winNear = mix(winNear, vec3(0.012, 0.012, 0.014) * wdiff * 2.0, clamp(frame, 0.0, 1.0));
        nearW = F * (1.0 - clamp(frame, 0.0, 1.0));
    }

    vec3 near = mix(wallLit, winNear, cov);
    vec3 mid = mix(wallLit, winMid, cov);
    vec3 col = mix(near, mid, lodT);
    float reflW = mix(mix(wallW, nearW, cov), mix(wallW, F, cov), lodT);
    if (farT > 0.0) {
        // average of many cells: expected lit fraction and window area
        vec3 avgLight = (tintSel < 0.5 ? kelvin(3050.0) : kelvin(3600.0)) * 1.15 * 0.42;
        vec3 avgWin = (avgLight * litFrac * glassT + gWinE * 0.1 * (1.0 - litFrac)) * (1.0 - F) + refl * F;
        vec3 far_ = mix(wallLit, avgWin, winW * winH);
        col = mix(col, far_, farT);
        reflW = mix(reflW, mix(wallW, F, winW * winH), farT);
    }
    glassRefl(n, reflW);
    return col;
}
