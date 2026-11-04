#ifndef ROADS_GLSL
#define ROADS_GLSL
#include "ground.glsl"
// the Roads buffer (binding 2), RoadHit and roadNear
#include "road_lookup.glsl"

/* The roads between the settlements (src/plan/plan_roads.c), painted onto the
 * clipmap's ground, which already follows their graded surfaces
 * (plan_road_ground). Their centre lines' segments round the camera come in
 * one buffer laid out by src/render/road_gpu.c: a header, a grid of cells
 * into a list of segments, the segments. Freeways (PLAN_ROAD_FREEWAY, half
 * 16 m): a concrete median barrier, three lanes each way between solid edge
 * lines, dashed between, hard shoulders, and sodium lamps down the median
 * every LAMP_SPACING whose pools light the carriageways and the land beside
 * them. Trunk roads (half 6 m): a lane each way about a dashed centre line,
 * solid edge lines, gravel shoulders, unlit. Both are laid in the city's own
 * asphalt (road.json, as its carriageways), and beyond the paved edge a strip
 * of gravel (roof_gravel.json) on the level verge gives way to the land's
 * cover. On a bridge's span the road is the deck the city's chunks draw over
 * the water (src/city/road_bridges.c), out to VIEW_DISTANCE; past that the
 * paint stands in for it on the ground below: paved to its edge, as the deck,
 * and nothing beyond. */

// The freeway's lamps (FREEWAY_LAMP_*, ROAD_SODIUM, their pools) are
// terrain/freeway_light.glsl's (road_lookup.glsl includes it), shared with the
// grass's blades.

// The gravel runs on this far beyond the paved edge, give or take
// ROAD_VERGE_VARY along the road, then gives way to the cover over ROAD_VERGE_SOFT.
const float ROAD_VERGE = 0.8, ROAD_VERGE_VARY = 0.4, ROAD_VERGE_SOFT = 0.5;

// Drawn about these colours (linear albedo): the gravel, dusty and a little
// lighter than the asphalt (whose colour is its own map's, times its
// albedo_scale, as the city's); the median's concrete (as the city's decks').
const vec3 ROAD_GRAVEL = vec3(0.070, 0.066, 0.058);
const vec3 ROAD_BARRIER = vec3(0.11);

// The road's textures fade into their flat colours as a pixel comes to span
// this many metres: by then their mips have all but reached their means.
const float ROAD_TEX_PX0 = 0.3, ROAD_TEX_PX1 = 0.6;

// The wet asphalt's film's roughness (GGX's).
const float ROAD_FILM_ROUGH = 0.35;

// Material m in the rain (its file's wet: pbrWet), or where it didn't load, a third darker, no puddles, a sheen.
vec3 roadWet(int m) { return pbrLoaded(m) ? pbrWet(m) : vec3(0.35, 0.0, 0.5); }

// A painted line `w` wide centred `at` across, anti-aliased over a pixel's
// width px, and faded to its share of the pixel once it is narrower.
float roadLine(float a, float at, float w, float px) {
    float cover = clamp(0.5 + (0.5 * max(w, px) - abs(a - at)) / px, 0.0, 1.0);
    return cover * min(w / px, 1.0);
}

// Dashes `on` long every `period` along the road, anti-aliased over pxs;
// their average once a pixel spans a dash.
float roadDash(float s, float on, float period, float pxs) {
    float f = mod(s, period);
    float c = clamp((on - f) / pxs + 0.5, 0.0, 1.0) * clamp(f / pxs + 0.5, 0.0, 1.0);
    return mix(c, on / period, clamp(2.0 * pxs / period, 0.0, 1.0));
}

// Where the asphalt ends, from the centre line: the paved edge, but for a
// trunk road's gravel shoulders (not on a bridge's deck); ragged a little
// along the road (wp: the world's x, z) where gravel meets it.
float roadAsphaltEdge(RoadHit r, vec2 wp) {
    if (r.bridge) return r.hw;
    float e = r.cls == 2 ? r.hw : TRUNK_ASPHALT;
    return e + 0.06 * (vnoise(wp / 0.6, 0x5A31u) - 0.5) + 0.05 * (vnoise(wp / 2.3, 0x5A32u) - 0.5);
}

// Material m mapped in the world's x and z (q, wrapped as the city's, so it
// runs on unbroken from a bridge's deck; +x image right, -z image up; gx, gy
// q's screen derivatives), drawn about colour (its map's mean drawn as that),
// the normal N bent by its normal map into n; as tex, from 1 to 0, falls, the
// flat colour and N. Its height too, in its map's spreads (pbrRelief).
vec3 roadSurface(int m, vec2 q, vec2 gx, vec2 gy, vec3 colour, float tex, vec3 N, out vec3 n, out float relief) {
    n = N; relief = 0.0;
    if (tex <= 0.0 || !pbrLoaded(m)) return colour;
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec2 t = pbrTile(m), st = q / t;
    gx /= t; gy /= t;
    vec3 a = textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb / max(pbrAlbedoMean(m), vec3(1e-3));
    vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    relief = tex * pbrRelief(m, textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).b);
    n = normalize(N + tex * (vec3(tn.x, 0.0, -tn.y)) / max(tn.z, 0.2));
    return colour * mix(vec3(1.0), a, tex);
}

// What a lane's wear does to the asphalt at x across it from its centre (m):
// darker down the middle where oil drips, lighter in the polished wheel
// paths; their mean across the lane once a pixel spans px of it.
float laneWear(float x, float px) {
    float oil = exp(-x * x / (2.0 * 0.3 * 0.3));
    float wx = abs(x) - 0.9, paths = exp(-wx * wx / (2.0 * 0.25 * 0.25));
    float f = 1.0 - 0.10 * oil + 0.06 * paths;
    return mix(f, 1.0 - 0.10 * 0.206 + 0.06 * 0.343, clamp(px / 0.8, 0.0, 1.0));
}

// The wheel paths' ruts at x across a lane from its centre (m): 1 deepest.
float laneRut(float x) { float wx = abs(x) - 0.9; return exp(-wx * wx / (2.0 * 0.3 * 0.3)); }

// The freeway's lamps' highlights on a wet surface (normal n, roughness
// rough) seen along V, beside road r: the five nearest down the median, each
// lighting it as its pool does beneath it (1.4 ROAD_SODIUM) over the square
// of its distance, the lobe widened by its size (Karis's sphere lights).
vec3 freewayGlints(RoadHit r, vec3 n, vec3 V, float rough) {
    vec2 along = vec2(r.across.y, -r.across.x);   // the way s grows
    float ds = freewayLampDs(r.s);
    float alpha = max(rough * rough, FREEWAY_GLINT_ALPHA), g = 0.0;
    for (int j = -2; j <= 2; ++j) {
        vec3 d = freewayLampVec(along, r.across, ds, r.off, j);
        float d2 = dot(d, d), dist = sqrt(d2);
        g += FREEWAY_LAMP_H * FREEWAY_LAMP_H / d2 * landGgx(n, d / dist, V, min(alpha + FREEWAY_LAMP_R / (2.0 * dist), 1.0), WATER_F0);
    }
    return ROAD_SODIUM * FREEWAY_POOL * g;
}

// The wet ground's sheen at rel (landSheen), beside a road r (onRoad) with
// its freeway's lamps' glints in it, by lamps (0 .. 1: none off a freeway,
// fading as its pool does, and as a pixel comes to span several lamps). The
// clipmap's one call (terrain.frag), so each part of it is compiled once.
vec3 wetSheen(vec3 rel, LandPoint l, vec3 n, vec3 nGeom, float rough, LandWater w, bool onRoad, RoadHit r, float lamps) {
    if (w.film + w.pool <= 0.0) return vec3(0.0);
    vec3 nW, V = -normalize(rel);
    float rW, k = wetSurface(l, n, nGeom, rough, w, nW, rW);
    vec3 c = wetHighlight(nW, V, rW, l.sky);
    if (onRoad && lamps > 0.0) c += lamps * freewayGlints(r, nW, V, rW);
    return k * c;
}

// The road r at rel, over the land there: its albedo land (groundAlbedo, the
// normal nLand bent by its textures, the water on it w) and the ground's own
// normal n. l: where the textures are mapped (landPoint); urban plan_urban
// there; px and pxs the pixel's width across and along the road (taken in
// uniform control flow). The land lit by its diffuse light, before the haze;
// for its sheen (wetSheen), the water on the road and the land beside it into
// w, their normal nS and the film's roughness roughS, and the lamps' share.
vec3 shadeRoad(vec3 land, vec3 nLand, vec3 n, vec3 rel, LandPoint l, float urban, RoadHit r, float px, float pxs,
               inout LandWater w, out vec3 nS, out float roughS, out float lampShare) {
    vec2 wp = frame.camPos.xz + rel.xz, q = l.p.xz, gx = l.dpx.xz, gy = l.dpy.xz;
    float a = abs(r.off), side = r.off < 0.0 ? -1.0 : 1.0;
    float wet = l.sky.wet;
    float tex = 1.0 - smoothstep(ROAD_TEX_PX0, ROAD_TEX_PX1, l.px);
    float edge = roadAsphaltEdge(r, wp);
    float asph = clamp(0.5 + (edge - a) / px, 0.0, 1.0);

    // the gravel, beyond the asphalt: out to about ROAD_VERGE past the paved
    // edge, then giving way to the cover where the gravel's height map stands
    // lower than the ground's middle (as the classes meet: landScore)
    float grav = 0.0;
    vec3 gAlb = ROAD_GRAVEL, nGrav = n;
    if (!r.bridge && asph < 1.0) {
        float reach = ROAD_VERGE + ROAD_VERGE_VARY * (2.0 * vnoise(wp / 9.0, 0x5A33u) - 1.0);
        float wg = 1.0 - smoothstep(reach - ROAD_VERGE_SOFT, reach + ROAD_VERGE_SOFT, a - r.hw + 0.3 * (vnoise(wp / 0.9, 0x5A34u) - 0.5));
        if (wg > 0.0) {
            float h;
            gAlb = roadSurface(PBR_ROOF_GRAVEL, q, gx, gy, ROAD_GRAVEL, tex, n, nGrav, h);
            gAlb *= 1.0 - wet * roadWet(PBR_ROOF_GRAVEL).x;
            wg = clamp(wg, 1e-4, 1.0 - 1e-4);
            float byHeight = 1.0 / (1.0 + exp((landScore(1.0 - wg, 0.0) - landScore(wg, h)) / LAND_BLEND));
            grav = wg >= 1.0 - 1e-4 ? 1.0 : mix(wg, byHeight, tex);
        }
    }

    // the asphalt, its paint, the median's barrier, and the water standing in
    // the asphalt's hollows and the wheel paths' ruts
    vec3 alb = vec3(0.0), nA = n;
    float paint = 0.0, barrier = 0.0;
    LandWater wr = LandWater(0.0, 0.0, w.aa);
    if (asph > 0.0) {
        float h, rut = 0.0;
        vec3 colour = pbrLoaded(PBR_ROAD) ? pbrAlbedoMean(PBR_ROAD) * pbrAlbedoScale(PBR_ROAD) : vec3(0.032);
        alb = roadSurface(PBR_ROAD, q, gx, gy, colour, tex, n, nA, h);
        if (r.cls == 2) {
            barrier = roadLine(a, 0.0, 1.6, px);
            paint += roadLine(a, 1.6, 0.2, px) + roadLine(a, 12.55, 0.2, px);
            paint += (roadLine(a, 5.25, 0.15, px) + roadLine(a, 8.9, 0.15, px)) * roadDash(r.s, 3.0, 12.0, pxs);
            if (a > 1.6 && a < 12.55) {
                alb *= laneWear(mod(a - 1.6, 3.65) - 1.825, px);
                rut = laneRut(mod(a - 1.6, 3.65) - 1.825);
            }
            alb *= a > 12.6 ? 1.25 : 1.0;   // the hard shoulders' lighter strip
            // grime at the barrier's foot
            alb *= 1.0 - 0.3 * exp(-(a - 0.85) * (a - 0.85) / (2.0 * 0.12 * 0.12)) * (1.0 - clamp(px / 0.5, 0.0, 1.0)) * step(0.8, a);
        } else {
            paint += roadLine(a, 0.0, 0.15, px) * roadDash(r.s, 3.0, 12.0, pxs);
            paint += roadLine(a, 3.6, 0.15, px);
            if (a < 3.6) { alb *= laneWear(a - 1.8, px); rut = laneRut(a - 1.8); }
            alb *= r.bridge && a > 3.7 ? 1.2 : 1.0;   // a deck's shoulders, asphalt
        }
        // resurfaced lengths, each carriageway its own (not on a deck, which the city draws without)
        if (!r.bridge) alb *= 0.93 + 0.14 * hfloat(hash2i(0x5A35u, int(floor(r.s / 180.0)), int(side)));
        alb *= 1.0 - wet * roadWet(PBR_ROAD).x;
        paint = clamp(paint, 0.0, 1.0);
        // the barrier: concrete, its top level and its faces sloping down to the carriageways
        if (barrier > 0.0) {
            float hb;
            vec3 nB, bAlb = roadSurface(PBR_CONCRETE, q, gx, gy, ROAD_BARRIER, tex, n, nB, hb);
            bAlb *= 1.0 - wet * roadWet(PBR_CONCRETE).x;
            float face = smoothstep(0.2, 0.3, a) * (1.0 - clamp(px / 0.6, 0.0, 1.0));
            nB = normalize(nB + vec3(r.across.x, 0.0, r.across.y) * side * 0.9 * face);
            alb = mix(alb, bAlb, barrier);
            nA = normalize(mix(nA, nB, barrier));
        }
        // the paint over the asphalt, taking a little of its grain, flattening its relief
        vec3 pAlb = vec3(0.45 * (1.0 - 0.3 * wet)) * mix(vec3(1.0), alb / max(colour, vec3(1e-3)), 0.25);
        alb = mix(alb, pAlb, paint * (1.0 - barrier));
        nA = normalize(mix(nA, n, 0.7 * paint * (1.0 - barrier)));
        // the water: a film (the barrier's its own), puddles (none on the barrier)
        wr.film = wet * mix(roadWet(PBR_ROAD).z, roadWet(PBR_CONCRETE).z, barrier);
        if (wet > 0.0) wr.pool = landPool(l, h - 0.6 * rut, roadWet(PBR_ROAD).y, n.y, wet) * (1.0 - barrier);
        alb *= 1.0 - 0.1 * wr.pool;
        nA = normalize(mix(nA, n, wr.pool));
    }

    // the light: the ground's, and beside a freeway its median's lamps, a pool
    // round each, longer along the road, that falls on the land beside it too
    // (not off a bridge's deck)
    float cov = 1.0 - asph, gv = cov * grav;
    vec3 nl = normalize(asph * nA + gv * nGrav + (cov - gv) * nLand);
    vec3 light = groundLightIn(l.sky, nl);
    float pool = 0.0, lamps = 0.0, lampsLand = 0.0;
    if (r.cls == 2) {
        // (terrain/freeway_light.glsl; the grass's blades beside it take the same)
        pool = freewayPool(freewayLampDs(r.s), a, pxs);
        float beside = r.bridge ? 0.0 : freewayBeside(a, r.hw);
        pool *= r.bridge ? asph : beside;
        // their highlights on the wet ground, until a pixel spans several
        lamps = 1.0 - clamp(pxs / (0.5 * FREEWAY_LAMP_SPACING), 0.0, 1.0);
        lampsLand = lamps * beside;
    }
    light += ROAD_SODIUM * FREEWAY_POOL * pool;

    // the water on the land beside the road, and on the gravel; then the road's over them
    float landFilm = mix(w.film, wet * roadWet(PBR_ROOF_GRAVEL).z * (1.0 - urban), grav), poolLand = w.pool * (1.0 - grav);
    w.film = mix(landFilm, wr.film, asph);
    w.pool = mix(poolLand, wr.pool, asph);
    nS = normalize(mix(normalize(mix(nLand, nGrav, grav)), nA, asph));
    roughS = mix(LAND_FILM_ROUGH, ROAD_FILM_ROUGH, asph);
    lampShare = mix(lampsLand, lamps, asph);
    vec3 ground = mix(land, gAlb, grav);
    return mix(groundUrban(ground * light, urban), alb * light, asph);
}

#endif
