// Part of shaders/city.frag, which includes the parts in order: authored PBR materials, wetness, puddles and ripples.

// The authored materials' bindings and sampling helpers (pbrMaps, materials[], samplePBR, pbrGrain,
// pbrTint, pbrTintLod, planarFrame, pbrBroadHeight) are shared with the terrain: shaders/pbr.glsl.
#include "pbr.glsl"

// A baked model's tangent frame (tools/model_pack.c; src/city/props.c): T along
// +u, B along -v, both square to the interpolated normal n, from dp/du and dp/dv,
// which the screen derivatives give (a triangle's own: they never cross an edge).
void modelFrame(vec3 p, vec2 uv, vec3 n, out vec3 T, out vec3 B) {
    vec3 dpx = dFdx(p), dpy = dFdy(p);
    vec2 dux = dFdx(uv), duy = dFdy(uv);
    float det = dux.x * duy.y - dux.y * duy.x;
    float s = abs(det) < 1e-12 ? 0.0 : 1.0 / det;
    vec3 dpdu = (dpx * duy.y - dpy * dux.y) * s, dpdv = (dpy * dux.x - dpx * duy.x) * s;
    vec3 t = dpdu - n * dot(n, dpdu);
    T = dot(t, t) > 1e-20 ? normalize(t) : normalize(cross(n, abs(n.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    B = cross(n, T) * (dot(cross(n, T), -dpdv) < 0.0 ? -1.0 : 1.0);
}

// Relief sunk into a surface, one image across it (a light fitting's tray, a
// gully grate): material m's height map as a depth below the surface's plane,
// where heights from `face` up lie, to height 0 deepest. Returns 1 in the
// plane, and round the image (st outside 0..1). gx, gy: st's screen derivatives.
float sunkHeight(int m, float face, vec2 st, vec2 gx, vec2 gy) {
    if (any(lessThan(st, vec2(0.0))) || any(greaterThan(st, vec2(1.0)))) return 1.0;
    return min(textureGrad(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, gx, gy).b / face, 1.0);
}

// Steep parallax into sunk relief: steps the view ray down in even layers of
// depth until it passes under the height field, then puts the hit between the
// last two layers by where the ray crossed it. dir: st's shift along the ray
// per unit of depth (height 1 -> 0). Returns where the ray meets the relief
// (xy) and how deep (z, 0 in the plane .. 1).
vec3 steepParallax(int m, float face, vec2 st, vec2 dir, float layers, vec2 gx, vec2 gy) {
    float inc = 1.0 / layers, depth = 0.0;
    float gap = 1.0 - sunkHeight(m, face, st, gx, gy), prev = gap;   // how far the height field lies below the ray
    if (gap <= 0.0) return vec3(st, 0.0);
    vec2 q = st;
    for (int i = 0; i < 64; ++i) {
        prev = gap;
        q += dir * inc;
        depth += inc;
        gap = 1.0 - sunkHeight(m, face, q, gx, gy) - depth;
        if (gap <= 0.0) break;
    }
    float back = inc * (-gap / (prev - gap));
    return vec3(q - dir * back, depth - back);
}

// Paints a flat coat (road paint, a cover plate) over a surface with coverage m.
// The coat fills the texture's relief, so its normal and occlusion return to
// the geometric ones; on a procedural surface (n == nGeom, ao == 1) only the
// colour and roughness change.
Surface layer(Surface s, vec3 nGeom, vec3 col, float rough, float m) {
    s.albedo = mix(s.albedo, col, m);
    s.n = normalize(mix(s.n, nGeom, m));
    s.rough = mix(s.rough, rough, m);
    s.ao = mix(s.ao, 1.0, m);
    return s;
}

// Sidewalk tangent frame from the angle the seed carries (src/city/streets.c): the block
// grain for paving, the kerb edge for kerb stones. T = +u, B = -v (image up;
// normal maps are OpenGL convention).
void grainFrame(uint seed, out vec3 T, out vec3 B) {
    float a = uintBitsToFloat(seed);
    T = vec3(cos(a), 0.0, sin(a));
    B = vec3(sin(a), 0.0, -cos(a));
}

// How wet the streets are (0 dry .. 1 soaked), from the weather (src/app/main.c).
#define WETNESS (frame.weather.y)

// Shelter map (renderer.c: record_shelter): the highest surface over each spot
// around the camera, seen from above. How sheltered camera-relative point q is
// from the rain: 1 under a canopy, arcade or overhang, 0 in the open (and off
// the map). Five taps across about 0.3 m soften the edge.
layout(set = 0, binding = 4) uniform sampler2D shelterMap;
float shelterAt(vec3 q) {
    vec2 uv = (q.xz - frame.shelter.xy) / frame.shelter.z * 0.5 + 0.5;
    vec2 e = abs(uv * 2.0 - 1.0);
    if (max(e.x, e.y) >= 1.0) return 0.0;
    float s = 0.0;
    for (int k = 0; k < 5; ++k) {
        vec2 o = k == 0 ? vec2(0.0) : vec2(k == 1 ? 2.0 : k == 2 ? -2.0 : 0.0, k == 3 ? 2.0 : k == 4 ? -2.0 : 0.0);
        float top = SHELTER_LO + texture(shelterMap, uv + o / SHELTER_SIZE).r * (SHELTER_HI - SHELTER_LO);
        s += smoothstep(0.25, 0.6, top - q.y);
    }
    return s * 0.2 * (1.0 - smoothstep(0.85, 1.0, max(e.x, e.y)));
}

// Raindrop ripples (ripples): shared with the land's puddles, shaders/ripples.glsl.
#include "ripples.glsl"

// Isolated shallow hollows where rain collects (sunken asphalt, a settled
// slab): a soft ellipse in about `chance` of the 5 m cells. Returns the dip's
// depth profile, 1 at its centre.
float hollow(vec2 xz, float chance, uint salt) {
    vec2 c = floor(xz / 5.0);
    float best = 0.0;
    for (int k = 0; k < 9; ++k) {
        vec2 cc = c + vec2(float(k % 3 - 1), float(k / 3 - 1));
        uint h = hash2i(salt, int(cc.x), int(cc.y));
        if (hfloat(h) > chance) continue;
        vec2 d = xz - (cc + 0.2 + 0.6 * vec2(hf(h, 1u), hf(h, 2u))) * 5.0;
        float a = hf(h, 3u) * PI, ca = cos(a), sa = sin(a);
        vec2 q = vec2(ca * d.x + sa * d.y, -sa * d.x + ca * d.y);
        float r = mix(0.4, 1.4, hf(h, 4u));
        best = max(best, 1.0 - smoothstep(0.2, 1.0, length(q / vec2(r, r * mix(0.5, 1.0, hf(h, 5u))))));
    }
    return best;
}


/* ---------------- graffiti ----------------
 * The pieces of assets/materials/graffiti.json, sprayed over a wall's albedo: brick, tile and stone walls, shut
 * shutters and blacked-out lobby glass (shadeFacade, shadeDoor, shadeArcology). The paint has no
 * relief of its own; the surface it's on keeps its normal, so the walls' joints and a shutter's slats
 * show through it. How much there is follows the district's affluence (city_graffiti in
 * src/city/litter.c), carried to the shader at the chunk's corners (ChunkGPU.info.w). */
#define GRAFFITI_COLS 12   // the atlas's square cells (tools/graffiti_pack.py), in reading order
#define GRAFFITI_ROWS 8

// How much graffiti there is here, 0 (wealthy) .. 1 (poorest): the chunk's corners, interpolated.
float graffitiLevel() {
    uint c = uint(gC.info.w);
    vec2 s = clamp(vLocal.xz / CHUNK_SIZE, 0.0, 1.0);
    vec4 k = vec4(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u), float(c >> 24)) / 255.0;
    return mix(mix(k.x, k.y, s.x), mix(k.z, k.w, s.x), s.y);
}

// Piece `piece` of the atlas, its cell s metres square centred at c and turned by a (radians, anticlockwise
// as seen): its colour (straight) and coverage at q. q: metres along the surface and up it; qx, qy: q's
// screen derivatives.
vec4 graffitiPiece(vec2 q, vec2 qx, vec2 qy, vec2 c, float s, float a, uint piece) {
    float ca = cos(a), sa = sin(a);
    mat2 R = mat2(ca, -sa, sa, ca);   // world to the piece's frame: a turn by -a
    vec2 r = R * (q - c) / s;
    vec2 st = vec2(r.x + 0.5, 0.5 - r.y);   // rows run down the image
    if (any(lessThan(st, vec2(0.0))) || any(greaterThan(st, vec2(1.0)))) return vec4(0.0);
    vec2 grid = vec2(GRAFFITI_COLS, GRAFFITI_ROWS);
    vec2 cell = vec2(float(piece % uint(GRAFFITI_COLS)), float(piece / uint(GRAFFITI_COLS)));
    vec2 gx = R * qx / s, gy = R * qy / s;
    gx.y = -gx.y; gy.y = -gy.y;
    return textureGrad(pbrMaps[PBR_GRAFFITI * PBR_MAPS_PER_MATERIAL + PBR_ALBEDO], (cell + st) / grid, gx / grid, gy / grid);
}

// A band of spots along a surface, between heights lo and hi (q.y), cellW metres apart: in each, by
// `chance`, one piece, as big as fits between lo and hi and inside its spot however it's turned. With two
// layers, a second row of spots offset half a spot, fewer of them, sprays over the first. key: the
// surface's own. Returns the paint premultiplied (rgb by its coverage, a).
vec4 graffitiBand(vec2 q, vec2 qx, vec2 qy, uint key, float lo, float hi, float cellW, float chance, int layers) {
    vec4 acc = vec4(0.0);
    if (q.y < lo || q.y > hi || chance <= 0.0) return acc;
    vec4 P = pbrParams(PBR_GRAFFITI);   // smallest and largest cell (m), the share painted at the poorest, the most a piece turns (degrees)
    float maxA = radians(P.w), fitK = cos(maxA) + sin(maxA);   // a turned square's extent over its side
    float sMax = min(P.y, min(hi - lo, cellW) / fitK);
    if (sMax < 0.3) return acc;
    for (int l = 0; l < layers; ++l) {
        float off = 0.5 * float(l), ci = floor(q.x / cellW + off);
        uint h = hash3i(key ^ uint(l) * 0x2F1Bu, int(ci), int(floor(lo * 4.0)), int(floor(hi * 4.0)));
        if (hfloat(h) >= chance * (l == 0 ? 1.0 : 0.5)) continue;
        float s = mix(min(P.x, sMax), sMax, pow(hf(h, 1u), 1.6));
        float ext = 0.5 * s * fitK;   // half its extent, turned
        vec2 c = vec2((ci - off + 0.5) * cellW + (hf(h, 2u) - 0.5) * max(cellW - 2.0 * ext, 0.0),
                      mix(lo + ext, hi - ext, hf(h, 3u)));
        vec4 pc = graffitiPiece(q, qx, qy, c, s, (hf(h, 4u) * 2.0 - 1.0) * maxA, uint(hf(h, 5u) * float(GRAFFITI_COLS * GRAFFITI_ROWS)));
        pc.a *= l == 0 ? mix(0.65, 0.95, hf(h, 6u)) : mix(0.8, 1.0, hf(h, 6u));   // the older faded by the weather
        acc = acc * (1.0 - pc.a) + vec4(pc.rgb * pc.a, pc.a);
    }
    return acc;
}

// Premultiplied paint (graffitiBand) as the albedo it lays over a surface's (rgb) and how much (a).
vec4 graffitiPaint(vec4 acc) {
    return acc.a > 1e-4 ? vec4(acc.rgb / acc.a * pbrAlbedoScale(PBR_GRAFFITI), acc.a) : vec4(0.0);
}

// Street-level graffiti on a surface (a loading bay's or a stall's shutter): q metres along it and up from
// its foot, `top` the highest it reaches. More on a shutter than a wall: they're asked for.
vec4 graffitiStreet(vec2 q, vec2 qx, vec2 qy, uint key, float top) {
    if (!pbrLoaded(PBR_GRAFFITI)) return vec4(0.0);
    float chance = pbrParams(PBR_GRAFFITI).z * graffitiLevel();
    return graffitiPaint(graffitiBand(q, qx, qy, key, 0.2, min(top - 0.05, 2.7), 3.0, chance, 2));
}
