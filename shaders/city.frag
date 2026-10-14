#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNrm;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint  vSeed;
layout(location = 5) flat in uvec4 vParams;
layout(location = 6) flat in uint  vChunk;
layout(location = 7) flat in uint  vFacePack;
layout(location = 8) flat in uint  vAux;
layout(location = 9) in vec2 vLitter;     // litter: ambient occlusion, how soaked (city.vert: litter)

layout(location = 0) out vec4 outColor;
// Reflection G-buffer for screen-space reflections (ssr.comp): world normal
// x and z, roughness, and the weight of the sky reflection this pixel added
// (zero where nothing reflective was drawn).
layout(location = 1) out vec4 outRefl;

ChunkGPU gC;
float gTime;
vec3 gWorld;
vec4 gRefl = vec4(0.0);   // -> outRefl
vec3 gWinE = vec3(0.0);   // light reaching a window from outside, through its glass: all a dark room has (shadeFacade)

const float SHEEN = 3.5;   // pane sheen strength over glass's physical reflectance (paneSheen)

// Glass marks itself for screen-space reflections: w is the weight its sky
// reflection (envReflection) carries in this pixel's final colour, which the
// reflections pass swaps for the screen's reflection (ssr_apply.frag). Glass is
// a mirror; the negative roughness tells the passes it's glass, not wet ground.
const float GLASS_ROUGH = -1.0;
void glassRefl(vec3 n, float w) { gRefl = vec4(n.x, n.z, GLASS_ROUGH, w); }
float gStreak = 1.0;      // wet surfaces: how far GGX highlights stretch toward the viewer    // chunk-local position + wrapped chunk offset: continuous for noise

const float PI = 3.14159265;

/* ======================================================================
 * Small helpers
 * ==================================================================== */

float band(float x, float a, float b, float aa) {
    return smoothstep(a - aa, a + aa, x) * (1.0 - smoothstep(b - aa, b + aa, x));
}

// Integral of a unit-periodic pulse that is 1 on [a,b]; used for box-filtered window masks.
float pulseInt(float x, float a, float b) { return floor(x) * (b - a) + clamp(fract(x), a, b) - a; }
float filteredPulse(float x, float a, float b, float w) {
    w = max(w, 1e-4);
    return clamp((pulseInt(x + 0.5 * w, a, b) - pulseInt(x - 0.5 * w, a, b)) / w, 0.0, 1.0);
}

float hf(uint h, uint k) { return hfloat(hash_combine(h, k)); }
float glyphDist(uint g, vec2 q);   // signs (below)


/* ======================================================================
 * Lighting: street lamps and lit shopfronts come from the chunk's light
 * table, built from the same plan as the geometry, so light pools sit
 * exactly under the lamp heads and shop spill matches the lit storefronts.
 * ==================================================================== */

vec3 ambientLight(vec3 n, float h) {
    vec3 sky = vec3(0.006, 0.007, 0.013);
    vec3 bounce = vec3(0.013, 0.008, 0.006);
    float up = n.y * 0.5 + 0.5;
    return mix(bounce, sky, up) * (0.8 + 0.8 * exp(-h / 30.0));
}

/* Specular lobes. Procedural materials use a normalised Blinn-Phong lobe
 * (shin, specK) and pass rough < 0. Authored PBR materials pass rough >= 0
 * and get GGX (Trowbridge-Reitz distribution, height-correlated Smith
 * visibility, Schlick Fresnel, alpha = rough^2) with F0 = specK. Lighting here
 * leaves the 1/pi out of Lambert diffuse (diffuse is alb * E), so the GGX
 * lobe carries a factor of pi to stay in the same units. */
float ggxSpec(vec3 n, vec3 L, vec3 V, float alpha, float f0, float ndl) {
    vec3 H = normalize(L + V);
    float ndh = max(dot(n, H), 0.0), ndv = max(dot(n, V), 1e-4), vdh = max(dot(V, H), 0.0);
    float a2 = max(alpha * alpha, 1e-6);
    float D;
    vec3 tv = V - n * dot(n, V);   // the view direction across the surface
    float s = 1.0 + (gStreak - 1.0) * smoothstep(0.2, 0.7, length(tv));
    if (s > 1.001) {
        // Anisotropic GGX, wider along the view direction and narrower across
        // it (same area): rain-slicked streets streak lights toward the viewer.
        vec3 t = normalize(tv), b = cross(n, t);
        float ax = min(alpha * s, 1.0), ay = max(alpha / s, 1e-3);
        vec3 h = vec3(dot(H, t) / ax, dot(H, b) / ay, ndh);
        float q = dot(h, h);
        D = 1.0 / (PI * ax * ay * q * q);
    } else {
        float dd = ndh * ndh * (a2 - 1.0) + 1.0;
        D = a2 / (PI * dd * dd);
    }
    float vis = 0.5 / (ndl * sqrt(ndv * ndv * (1.0 - a2) + a2) + ndv * sqrt(ndl * ndl * (1.0 - a2) + a2) + 1e-5);
    float F = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
    return PI * D * vis * F * ndl;
}

float specLobe(vec3 n, vec3 L, vec3 V, float shin, float specK, float rough, float ndl) {
    if (rough >= 0.0) return ggxSpec(n, L, V, max(rough * rough, 0.002), specK, ndl);
    vec3 H = normalize(L + V);
    return pow(max(dot(n, H), 0.0), shin) * specK * ndl * (shin + 8.0) / 25.0;
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
float envBRDF(float f0, float rough, float ndv) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * AB.x + AB.y;
}

void addLamp(vec3 p, vec3 n, vec3 V, float shin, float specK, float rough, vec3 lpos, vec3 lcol,
             inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    float d2 = dot(l, l);
    float d = sqrt(d2);
    float range = smoothstep(34.0, 14.0, d);
    if (range <= 0.0) return;
    vec3 L = l / d;
    // the beam below the head, and a spill all round it (the glowing bowl, light
    // scattered off the wet street) that carries up the columns and facades
    float spot = mix(0.12, 1.0, smoothstep(0.05, 0.55, L.y));
    float att = 90.0 / (d2 + 1.0) * range * spot;
    float ndl = max(dot(n, L), 0.0);
    diff += lcol * att * ndl;
    spec += lcol * att * specLobe(n, L, V, shin, specK, rough, ndl);
}

// Coloured light thrown onto the sidewalk and street by a lit shopfront. The
// storefront is a large source: for GGX its size widens the lobe (alpha grows
// by its ~1.5 m half-width over twice the distance, as for a sphere light), so
// the sheen spreads like a reflection of the window, not a point highlight.
void addShop(vec3 p, vec3 n, vec3 V, float specK, float rough, vec3 lpos, vec3 col, inout vec3 diff, inout vec3 spec) {
    vec3 l = lpos - p;
    float d = length(l);
    if (d > 12.0) return;
    vec3 L = l / max(d, 1e-3);
    float ndl = max(dot(n, L), 0.0);
    vec3 c = col * 0.45 * exp(-d * 0.38) * smoothstep(12.0, 7.0, d);
    diff += c * (0.35 + 0.65 * ndl);
    if (rough >= 0.0) spec += c * ggxSpec(n, L, V, min(max(rough * rough, 0.002) + 0.75 / max(d, 0.5), 1.0), specK, ndl);
    else spec += c * specK * 0.6;
}

/* Facade faces are numbered within their section, round the outline. The
 * cell (window bay or 6 m shop bay) that wraps a convex corner is keyed as
 * the next face's first cell, so both windows of a corner room or shop look
 * into the same space. */
ivec2 cornerKey(int face, int next, bool endShare, int idx, int last) {
    return (endShare && idx >= last && last > 0) ? ivec2(next, 0) : ivec2(face, idx);
}

// Storefronts are 6 m bays along each ground-floor face (city_shop_hash in src/city.c).
uint shopHash(uint seed, int section, int face, int next, bool endShare, float along, float faceW) {
    ivec2 k = cornerKey(face, next, endShare, int(floor(along / 6.0)), int(floor((faceW - 0.01) / 6.0)));
    return hash3i(WORLD_SEED ^ 0x5B0Au, int(seed), section * 64 + k.x, k.y);
}

// rough < 0: procedural Blinn-Phong (shin, specK); rough >= 0: GGX with F0 = specK.
void cityLights(vec3 p, vec3 n, vec3 V, float shin, float specK, float rough, inout vec3 diff, inout vec3 spec) {
    int base = gC.info.z;
    if (base < 0 || p.y > 32.0) return;
    vec4 hdr = uintBitsToFloat(lightRec[base]);
    ivec2 t = clamp(ivec2(floor((p.xz - hdr.xy) * hdr.zw)), ivec2(0), ivec2(3));
    int ti = t.y * 4 + t.x;
    uint tile = lightRec[base + 1 + (ti >> 2)][ti & 3];
    int first = base + int(tile >> 16), count = int(tile & 0xFFFFu);
    for (int i = 0; i < count; ++i) {
        uvec4 r = lightRec[first + i];
        vec3 lp = uintBitsToFloat(r.xyz);
        uint type = r.w & 3u;
        if (type < 2u) addLamp(p, n, V, shin, specK, rough, lp, lampColor(int(type)) * lampState(r.w >> 2, gTime), diff, spec);
        else addShop(p, n, V, specK, rough, lp, type == 2u ? shopColor(r.w) : LOBBY_SPILL, diff, spec);
    }
}

// Soft coloured light from the sea of signs, strongest near street level.
vec3 neonBounce(vec3 w, float h) {
    float m = vnoise(w.xz / 23.0, 0xB0B0u);
    float k = vnoise(w.xz / 17.0 + 7.3, 0xB0B1u);
    vec3 c = mix(vec3(1.0, 0.1, 0.5), vec3(0.1, 0.8, 1.0), smoothstep(0.3, 0.7, m));
    return c * smoothstep(0.35, 0.8, k) * exp(-h / 14.0) * 0.035;
}

/* Pane sheen: a soft, blurred reflection that gives glass its presence where
 * screen-space reflections can't reach (glass seen near head-on reflects the
 * street behind the camera, which isn't on screen). A view-dependent gradient
 * (the dark street below, a glowing band of the lit facades across the way,
 * the sodium-lit overcast above) plus soft blooms where the street lamps
 * nearby would reflect. Strongest head-on, it fades out as the
 * reflected ray turns toward the screen and SSR takes over (ssr.comp), so the
 * two never double up. Added on top of the pane: it shows on dark windows and
 * is drowned out by lit rooms, as real night glass is. */
vec3 paneSheen(vec3 p, vec3 n, vec3 V) {
    vec3 R = reflect(-V, n);
    float reach = 1.0 - smoothstep(-0.3, 0.1, dot(R, frame.camFwd.xyz));   // SSR's own cut-off, inverted
    if (reach <= 0.0) return vec3(0.0);
    float F = glassFresnel(n, V);

    // the gradient: street, facades across, overcast (blurred: low frequencies only)
    float az = atan(R.z, R.x);
    float across = smoothstep(-0.35, -0.02, R.y) * (1.0 - smoothstep(0.05, 0.45, R.y));
    float rhythm = 0.55 + 0.45 * vnoise(vec2(az * 9.0 + gWorld.x * 0.03, 0.0), 0x5EE1u);
    vec3 c = mix(vec3(0.010, 0.010, 0.013), CLOUD_DECK * 0.8, smoothstep(0.0, 0.5, R.y))
           + vec3(0.11, 0.085, 0.06) * across * rhythm
           + neonBounce(gWorld, p.y) * 4.0 * across;

    // soft blooms of the street lamps nearby, where they'd reflect
    int base = gC.info.z;
    if (base >= 0 && p.y < 40.0) {
        vec4 hdr = uintBitsToFloat(lightRec[base]);
        ivec2 t = clamp(ivec2(floor((p.xz - hdr.xy) * hdr.zw)), ivec2(0), ivec2(3));
        int ti = t.y * 4 + t.x;
        uint tile = lightRec[base + 1 + (ti >> 2)][ti & 3];
        int first = base + int(tile >> 16), count = int(tile & 0xFFFFu);
        for (int i = 0; i < count; ++i) {
            uvec4 r = lightRec[first + i];
            vec3 l = uintBitsToFloat(r.xyz) - p;
            float d = length(l);
            if (dot(l, n) <= 0.0) continue;   // behind the pane
            // Shop and lobby lights stand in for the glow spilling out of the
            // glass, not for anything you could see: they'd reflect as balls
            // of light floating at chest height.
            uint type = r.w & 3u;
            if (type >= 2u) continue;
            vec3 lc = lampColor(int(type)) * lampState(r.w >> 2, gTime);
            float k = max(dot(R, l / d), 0.0);
            float bloom = pow(k, 40.0) * 3.0 + pow(k, 6.0) * 0.25;   // a soft core in a wide halo
            c += lc * bloom * 30.0 / (d * d + 30.0);
        }
    }
    return c * F * reach * SHEEN;
}


/* ======================================================================
 * Authored PBR materials (src/textures.c). Surfaces opt in per material;
 * a material whose maps were not found is bound as 1x1 placeholders and
 * its surfaces keep their procedural look.
 * ==================================================================== */

layout(set = 0, binding = 3) uniform sampler2D pbrMaps[PBR_MATERIAL_COUNT * PBR_MAPS_PER_MATERIAL];

struct Surface { vec3 albedo; vec3 n; float ao; float rough; float height; };

bool pbrLoaded(int m) { return textureSize(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ALBEDO], 0).x > 1; }

// st: texture coordinates. T, B: world directions of image right and image up
// (normal maps are OpenGL convention, +Y green = up the image).
Surface samplePBR(int m, vec2 st, vec3 T, vec3 B, vec3 N) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    Surface s;
    s.albedo = texture(pbrMaps[i + PBR_ALBEDO], st).rgb;
    vec3 tn = texture(pbrMaps[i + PBR_NORMAL], st).xyz * 2.0 - 1.0;
    s.n = normalize(T * tn.x + B * tn.y + N * tn.z);
    vec3 orm = texture(pbrMaps[i + PBR_ORM], st).rgb;
    s.ao = orm.r; s.rough = orm.g; s.height = orm.b;
    return s;
}

// Broad relief: the height map averaged over many texels (a biased mip), for
// shapes like puddles that should follow hollows rather than every grain.
float pbrBroadHeight(int m, vec2 st, float bias) { return texture(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, bias).b; }

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

// PavingStones111 (ambientCG): metres per texture repeat (2:1 image, slabs
// about 60 cm), and an albedo scale that keeps it in the city's night palette.
const vec2 PAVING_TILE = vec2(9.6, 4.8);
const float PAVING_ALBEDO = 0.5;   // mean 0.22 -> 0.11, near the procedural kerb stones

// Ground032 (ambientCG) for the kerb stones: fine aggregate, 2:1 image.
const vec2 KERB_TILE = vec2(2.0, 1.0);
const float KERB_ALBEDO = 0.5;   // mean 0.25 -> 0.12, as the procedural kerb stones

// Sidewalk tangent frame from the angle the seed carries (src/city.c): the block
// grain for paving, the kerb edge for kerb stones. T = +u, B = -v (image up;
// normal maps are OpenGL convention).
void grainFrame(uint seed, out vec3 T, out vec3 B) {
    float a = uintBitsToFloat(seed);
    T = vec3(cos(a), 0.0, sin(a));
    B = vec3(sin(a), 0.0, -cos(a));
}

// How wet the streets are (0 dry .. 1 soaked), from the weather (src/main.c).
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

// Raindrop ripples: drops land at random in a grid of small cells, each
// sending out a ring that fades as it spreads; two offset grids overlap so
// the pattern never looks regular. rate = share of cells hit each cycle.
// Returns the surface slope (x, z) the rings add.
vec2 ripples(vec2 xz, float t, float rate) {
    vec2 g = vec2(0.0);
    for (int layer = 0; layer < 2; ++layer) {
        vec2 p = xz / 0.3 + float(layer) * vec2(0.37, 0.71);
        vec2 c = floor(p);
        for (int k = 0; k < 9; ++k) {
            vec2 cc = c + vec2(float(k % 3 - 1), float(k / 3 - 1));
            uint h = hash2i(0x5A1Du + uint(layer), int(cc.x), int(cc.y));
            float cycle = t * 1.4 + hf(h, 1u);
            uint hd = hash_combine(h, uint(floor(cycle)));
            if (hfloat(hd) > rate) continue;
            float f = fract(cycle);
            vec2 d = p - (cc + 0.2 + 0.6 * vec2(hf(hd, 2u), hf(hd, 3u)));
            float r = length(d);
            float x = (r - f * 1.2) * 10.0;                // across the ring, in ring widths
            float wave = sin(clamp(x, -3.0, 3.0) * PI / 3.0 * 2.0) * exp(-x * x * 0.25);
            g += d / max(r, 1e-3) * wave * (1.0 - f) * (1.0 - f);
        }
    }
    return g * 0.3;
}

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

// Height-map statistics of the authored materials (median and spread of
// orm.b), so water levels mean the same on each: relative height
// (h - median) / spread runs from about -0.5 (joints, hollows) to 0.5.
const vec2 ROAD_RELIEF = vec2(0.80, 0.13);
const vec2 PAVING_RELIEF = vec2(0.59, 0.32);
const vec2 KERB_RELIEF = vec2(0.49, 0.19);

// Road012A (ambientCG): mapped in world space so carriageways and the junction
// fill match across their seams. The repeat must divide 64 chunks (7040 m),
// where the shader's world coordinates wrap.
const float ROAD_TILE = 4.0;
const float ROAD_ALBEDO = 1.0;


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

// Tactile paving at the foot of a drop kerb (src/city.c: emit_drop): concrete
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
/* Fine litter, too small and too many for the litter layer (src/litter.c):
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

/* ======================================================================
 * Interiors (emit_interior in src/city.c): real lobby geometry behind the
 * ground-floor glass of office buildings, lit by the ceiling's light panels
 * in the building's lobby colour temperature, as the glass outside shows it.
 * params y surface, z variety (a region or fixture type), w ceiling height
 * above the floor in half metres. Floors and ceilings carry the building's
 * frame angle in aux, so their uv (building metres) can be followed.
 * ==================================================================== */

// Real glass over real geometry: with MSAA the pane covers only some of the
// pixel's samples (always sample 0, so depth and reflections see the pane)
// and the rest show what stands behind it; more of the pane at grazing
// angles, where it reflects more. Returns the covered share. Without MSAA
// the pane vanishes and the view through is clear.
float paneCoverage(float F) {
    int k = F < 0.25 ? 1 : F < 0.5 ? 2 : 3;
    gl_SampleMask[0] = (1 << k) - 1;
    return float(k) * 0.25;
}

const uint IS_FLOOR = 0u, IS_ROOMFLOOR = 1u, IS_WALL = 2u, IS_CEILING = 3u, IS_CORE = 4u, IS_METAL = 5u, IS_WOOD = 6u,
           IS_FABRIC = 7u, IS_GLASS = 8u, IS_LIGHT = 9u, IS_PLANT = 10u, IS_LINING = 11u, IS_STONE = 12u, IS_LIFTDOOR = 13u,
           IS_DOOR = 14u, IS_SCREEN = 15u, IS_SKYLIGHT = 16u, IS_OFFICE = 17u, IS_SIGN = 18u;
// region types (IR_* in src/interior.h) that change a floor's finish
const uint IR_VESTIBULE = 1u, IR_HALL = 2u, IR_SECURE = 3u, IR_TOILET = 7u, IR_WC = 8u, IR_SECURITY = 10u, IR_MAIL = 11u,
           IR_FACILITIES = 12u, IR_MEETING = 14u, IR_SHOWER = 15u;

vec3 lobbyLight(uint seed) {
    return (hf(styleHash(seed), 8u) < 0.5 ? kelvin(4000.0) : kelvin(3000.0)) * 1.35;   // as the lobby glass (shadeFacade)
}

// Light panels: 1.2 m squares on a 3 m grid in the building's frame.
float ceilingPanel(vec2 q, float aa) {
    vec2 d = abs(fract(q / 3.0) - 0.5) * 3.0;
    return (1.0 - smoothstep(0.6 - aa, 0.6 + aa, d.x)) * (1.0 - smoothstep(0.6 - aa, 0.6 + aa, d.y));
}

vec3 lobbyStone(uint seed, vec2 q, float aa, out float joint) {
    uint sh = styleHash(seed);
    float pick = hf(sh, 21u);
    vec3 c = pick < 0.3 ? vec3(0.42, 0.38, 0.32) : pick < 0.55 ? vec3(0.055, 0.055, 0.06) : pick < 0.8 ? vec3(0.55, 0.55, 0.53) : vec3(0.30, 0.29, 0.27);
    vec2 cell = floor(q / 1.2), t = fract(q / 1.2);
    c *= 0.9 + 0.2 * hfloat(hash3i(sh, int(cell.x), int(cell.y), 5));
    c *= 0.85 + 0.3 * fbm(q * 0.9, sh & 0xFFFFu);
    float w = 0.004 + aa;
    joint = max(1.0 - smoothstep(0.0, w, min(t.x, 1.0 - t.x) * 1.2), 1.0 - smoothstep(0.0, w, min(t.y, 1.0 - t.y) * 1.2));
    return c;
}

vec3 shadeInterior(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    if (dot(n, V) < 0.0) n = -n;                       // thin panes and panels are seen from both sides
    uint surf = prm.y, sub = prm.z;
    float ceilH = max(float(prm.w) * 0.5, 2.6), y = p.y - SIDEWALK_HEIGHT;
    vec3 Lc = lobbyLight(seed);
    float aa = max(length(fwidth(uv)), 1e-4);
    // the building's frame: world -> building directions, for floors and ceilings
    float ang = (float(vAux & 0xFFFFu) / 65535.0) * 6.2831853 - 3.14159265;
    mat2 toFrame = mat2(cos(ang), -sin(ang), sin(ang), cos(ang));

    // irradiance from the panels overhead: strong on what faces up, softer on walls, a little bounce above
    float E = n.y > 0.5 ? 0.55 : n.y < -0.5 ? 0.14 : 0.26 + 0.14 * smoothstep(0.0, ceilH, y);
    E *= mix(0.65, 1.0, smoothstep(0.0, 0.7, y)) * mix(1.0, 0.75, smoothstep(5.0, 14.0, y));
    vec3 diff = Lc * E + ambientLight(n, p.y) * 0.5;
    float specB = pow(max(dot(n, normalize(vec3(0.0, 1.0, 0.0) + V)), 0.0), 40.0);

    vec3 alb = vec3(0.5), emit = vec3(0.0);
    float gloss = 0.05;
    uint h = hash_combine(seed, sub);
    if (surf == IS_FLOOR || (surf == IS_ROOMFLOOR && (sub == IR_HALL || sub == IR_SECURE)) || (surf == IS_STONE && sub == 2u)) {
        float joint;
        alb = lobbyStone(seed, uv, aa, joint);
        if (surf == IS_ROOMFLOOR) alb *= 0.6;
        alb *= 1.0 - 0.5 * joint;
        gloss = 0.6 * (1.0 - joint);
    } else if (surf == IS_ROOMFLOOR) {
        if (sub == IR_VESTIBULE) {
            alb = vec3(0.035, 0.035, 0.04) * (0.8 + 0.2 * step(0.5, fract(uv.x * 12.0)));
        } else if (sub == IR_TOILET || sub == IR_WC || sub == IR_SHOWER) {
            vec2 t = fract(uv / 0.3);
            float j = max(1.0 - smoothstep(0.0, 0.03 + aa * 3.0, min(t.x, 1.0 - t.x)), 1.0 - smoothstep(0.0, 0.03 + aa * 3.0, min(t.y, 1.0 - t.y)));
            alb = mix(vec3(0.52, 0.52, 0.50), vec3(0.22), j);
            gloss = 0.3;
        } else if (sub == IR_MEETING || sub == IR_SECURITY || sub == IR_FACILITIES || sub == IR_MAIL) {
            vec3 pal = hf(seed, 23u) < 0.5 ? vec3(0.06, 0.07, 0.09) : vec3(0.05, 0.08, 0.08);
            vec2 cell = floor(uv / 0.5);
            alb = pal * (0.85 + 0.3 * hfloat(hash3i(h, int(cell.x), int(cell.y), 2))) * (0.8 + 0.4 * vnoise(uv * 9.0, 0xCA9Eu));
            gloss = 0.0;
        } else {
            alb = vec3(0.22, 0.21, 0.20) * (0.7 + 0.5 * fbm(uv * 0.7, 0xC0C0u));
            gloss = 0.1;
        }
    } else if (surf == IS_WALL) {
        alb = mix(vec3(0.62, 0.60, 0.56), vec3(0.55, 0.57, 0.58), hf(seed, 24u)) * (sub == 1u ? 0.7 : 1.0);
        if (y < 0.1) alb *= 0.3;                       // skirting
    } else if (surf == IS_CEILING) {
        alb = vec3(0.7, 0.7, 0.68);
        float panel = ceilingPanel(uv, aa);
        emit = Lc * 2.4 * panel;
    } else if (surf == IS_CORE) {
        float pick = hf(styleHash(seed), 22u);
        alb = pick < 0.3 ? vec3(0.07, 0.07, 0.075) : pick < 0.55 ? vec3(0.16, 0.11, 0.06) : pick < 0.8 ? vec3(0.55, 0.54, 0.52) : vec3(0.30, 0.18, 0.10);
        float jx = fract(uv.x / 1.2), jy = fract(y / 3.0);
        alb *= 1.0 - 0.45 * max(1.0 - smoothstep(0.0, 0.004 + aa, min(jx, 1.0 - jx) * 1.2), 1.0 - smoothstep(0.0, 0.004 + aa, min(jy, 1.0 - jy) * 3.0));
        gloss = pick >= 0.3 && pick < 0.55 ? 0.6 : 0.35;
    } else if (surf == IS_METAL) {
        alb = sub == 1u ? vec3(0.30, 0.20, 0.10) : sub == 2u ? vec3(0.18) : vec3(0.06, 0.065, 0.07);   // dark anodised frames
        if (sub == 10u + 10u) alb = vec3(0.25) * (0.6 + 0.4 * step(0.1, fract(y / 0.4)));                    // shelving
        if (sub == 10u + 12u) alb = hf(seed, 25u) < 0.5 ? vec3(0.05, 0.15, 0.06) : vec3(0.2, 0.2, 0.21);   // bins
        if (sub == 10u + 15u) alb = vec3(0.30, 0.32, 0.35) * (0.7 + 0.3 * step(0.03, fract(uv.x / 0.5)));  // lockers
        gloss = 0.5;
    } else if (surf == IS_WOOD) {
        alb = sub == 1u ? vec3(0.40, 0.30, 0.18) : hfloat(h) < 0.5 ? vec3(0.22, 0.12, 0.06) : vec3(0.45, 0.32, 0.18);
        alb *= 0.85 + 0.15 * sin(uv.x * 40.0 + vnoise(uv * 3.0, 0x900Du) * 6.0);
        gloss = 0.25;
    } else if (surf == IS_FABRIC) {
        float k = hfloat(h);
        alb = sub == 200u ? vec3(0.40, 0.28, 0.15) : k < 0.25 ? vec3(0.06) : k < 0.45 ? vec3(0.03, 0.12, 0.12) : k < 0.65 ? vec3(0.35, 0.25, 0.05) :
              k < 0.8 ? vec3(0.18, 0.03, 0.04) : vec3(0.2);
    } else if (surf == IS_GLASS) {
        float F = glassFresnel(n, V);
        float cov = paneCoverage(F);
        return (Lc * (0.05 + 0.6 * specB) * (sub == 1u ? 0.6 : 1.0) * mix(1.0, 4.0, F) + vec3(0.004, 0.006, 0.006)) * 0.25 / cov;
    } else if (surf == IS_LIGHT) {
        return sub == 1u ? vec3(1.0, 0.45, 0.1) * 4.0 : sub == 3u ? neonColor(seed >> 5) * 5.0 : Lc * (sub == 2u ? 3.0 : 4.0);
    } else if (surf == IS_SIGN) {
        return vec3(0.1, 1.0, 0.35) * 3.0;
    } else if (surf == IS_SCREEN) {
        float scan = 0.85 + 0.15 * step(0.5, fract(y * 120.0));
        return vec3(0.25, 0.45, 0.9) * 2.5 * scan * (0.7 + 0.3 * vnoise(vec2(uv.x * 6.0, gTime * 2.0), h & 0xFFFFu));
    } else if (surf == IS_PLANT) {
        alb = mix(vec3(0.02, 0.05, 0.015), vec3(0.08, 0.16, 0.05), fbm(vec2(uv.x * 4.0, y * 4.0), h & 0xFFFFu));
    } else if (surf == IS_LINING) {
        if (sub == 1u) alb = hf(seed, 26u) < 0.5 ? vec3(0.35, 0.36, 0.38) : vec3(0.30, 0.38, 0.40);
        else { alb = vec3(0.015); gloss = 0.8; }
    } else if (surf == IS_STONE) {
        alb = sub == 1u ? vec3(0.10, 0.10, 0.11) : sub == 3u ? vec3(0.70) : vec3(0.65, 0.64, 0.62);
        gloss = 0.5;
    } else if (surf == IS_LIFTDOOR) {
        float seam = 1.0 - smoothstep(0.0, 0.01 + aa, abs(fract(uv.x) - 0.5) - 0.0);
        alb = vec3(0.40) * (0.9 + 0.1 * vnoise(vec2(uv.x * 60.0, y * 2.0), 0x11F7u)) * (1.0 - 0.7 * seam);
        gloss = 0.7;
    } else if (surf == IS_DOOR) {
        alb = sub == 1u ? vec3(0.25, 0.27, 0.30) : vec3(0.30, 0.20, 0.12);
        gloss = 0.2;
    } else if (surf == IS_SKYLIGHT) {
        vec2 t = fract(uv / 1.5);
        float bar = max(1.0 - smoothstep(0.0, 0.02 + aa, min(t.x, 1.0 - t.x)), 1.0 - smoothstep(0.0, 0.02 + aa, min(t.y, 1.0 - t.y)));
        return mix(CLOUD_DECK * 2.2 + Lc * 0.03, vec3(0.01), bar);
    } else if (surf == IS_OFFICE) {
        // the podium floors round an atrium: dark glazing onto lit offices
        float fh = facadeFloorH(seed), fy = fract((y - ceilH) / fh) * fh;
        float bay = floor(uv.x / 1.5), mull = 1.0 - smoothstep(0.0, 0.03 + aa, min(fract(uv.x / 1.5), 1.0 - fract(uv.x / 1.5)) * 1.5);
        float lit = step(0.35, hfloat(hash3i(seed, int(bay), int(floor((y - ceilH) / fh)), 9)));
        vec3 room = Lc * (0.08 + 0.5 * smoothstep(fh - 0.7, fh - 0.4, fy)) * lit + vec3(0.004);
        vec3 c = fy < 0.9 ? vec3(0.03) * diff : room;
        return mix(c, vec3(0.02), mull);
    }

    vec3 col = alb * diff + Lc * specB * gloss * 0.08 + emit;
    if (gloss > 0.4 && n.y > 0.5) {
        // polished floors mirror the ceiling's panels
        vec3 R = reflect(-V, n);
        float ndv = max(dot(n, V), 0.0), F = 0.04 + 0.96 * pow(1.0 - ndv, 5.0);
        if (R.y > 0.05) {
            float t = (ceilH - y) / R.y;
            vec2 q = uv + toFrame * (R.xz * t);
            col += Lc * 1.2 * ceilingPanel(q, aa + 0.12 * t + 0.1) * F * gloss + Lc * 0.03 * F;
        }
    }
    return col;
}

/* ======================================================================
 * Arcologies (emit_arcology in src/city.c). Low-key light: a dark base,
 * neon washing the street stratum, sodium pools under the concourse lamps
 * (worked out here from the lamp row each street floor carries in aux), the
 * arcology's few real lights from the chunk's light table, cold light from
 * the crown higher up, and haze over everything, thick and warm low down,
 * thin and cool above. params y surface (ARS_* in src/arcology.h), z
 * variety, w stratum height * 255.
 * ==================================================================== */

const uint ARS_FLOOR = 0u, ARS_STREET = 1u, ARS_SLAT = 2u, ARS_CEILING = 3u, ARS_COLUMN = 4u, ARS_CAPITAL = 5u, ARS_BEAM = 6u,
           ARS_BLOCK = 7u, ARS_ROOF = 8u, ARS_STALL = 9u, ARS_STALLROOF = 10u, ARS_SHUTTER = 11u, ARS_COUNTER = 12u,
           ARS_SPINE = 13u, ARS_CORE = 14u, ARS_GALLERY = 15u, ARS_SOFFIT = 16u, ARS_FASCIA = 17u, ARS_UNITS = 18u,
           ARS_RAIL = 19u, ARS_METAL = 20u, ARS_CROWN = 21u, ARS_SHAFT = 22u, ARS_SEARCH = 23u, ARS_PUDDLE = 24u,
           ARS_STEAM = 25u, ARS_CRATE = 26u, ARS_BRIDGE = 27u, ARS_PORTAL = 28u, ARS_STAIR = 29u, ARS_LINING = 30u,
           ARS_KIOSK = 31u, ARS_LIFTDOOR = 32u, ARS_SKYLOBBY = 33u;
// block uses (AB_* in src/arcology.h)
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
    float y = p.y - SIDEWALK_HEIGHT;
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
    // Facade data (facade_mat in src/city.c):
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

    // Wall material: the section's palette (choose_surfaces in src/building.c)
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

    // uv.x = metres along the face; chunk-local y is absolute height.
    float u = uv.x, vAbs = p.y;
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

    // ---------------- lobbies and void decks ----------------
    if (atGround && relV < 0.0 && groundMode != 0u) {
        float yy = vAbs - SIDEWALK_HEIGHT;
        float glassTop = groundH - (groundMode == 2u ? 1.2 : 0.7);
        float fs = fract(u / 1.5);
        float aa = dU / 1.5 * 1.5 + dV * 0.2;
        float inWin = band(yy, 0.08, glassTop, dV + 0.01) * (1.0 - band(fs, 0.0, 0.04, aa) - band(fs, 0.96, 1.0, aa));
        vec3 spec;
        vec3 diffL = wallLighting(p, n, V, 30.0, 0.15, vAbs, spec);
        vec3 wallC = wallAlb * 0.9 * diffL + spec;
        float lodR = smoothstep(0.08, 0.4, dU / 8.0);
        if ((prm.w & 64u) != 0u) {
            // A closed room or a core of the real lobby stands behind this
            // stretch (emit_walls in src/city.c): opaque back-painted glass.
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
            // A real lobby stands behind this glass (src/interior.c): the pane
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

/* ======================================================================
 * Neon signs, screens and other emitters
 * ==================================================================== */

float segDist(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float t = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * t);
}

// Pseudo-kanji built like real characters: a few horizontal strokes, one or
// two verticals, an occasional sweeping diagonal and sometimes a box radical.
// q in [0,1]^2 (y up); returns distance to the nearest stroke in glyph units.
float glyphDist(uint g, vec2 q) {
    uint h = hash_u32(g);
    float d = 1e9;
    int rows = 2 + int(h % 3u);
    for (int j = 0; j < 4; ++j) {
        if (j >= rows) break;
        uint hj = hash_combine(h, uint(j));
        float y = 0.1 + 0.8 * (float(j) + 0.5) / float(rows) + (hf(hj, 9u) - 0.5) * 0.08;
        float x0 = 0.1 + 0.3 * hfloat(hj) * step(0.5, hf(hj, 1u));
        float x1 = 0.9 - 0.3 * hf(hj, 2u) * step(0.5, hf(hj, 3u));
        d = min(d, segDist(q, vec2(x0, y), vec2(x1, y)));
    }
    int cols = 1 + int((h >> 4) & 1u);
    for (int i = 0; i < 2; ++i) {
        if (i >= cols) break;
        uint hi = hash_combine(h, 10u + uint(i));
        float x = (cols == 1 ? 0.5 : (i == 0 ? 0.3 : 0.7)) + (hfloat(hi) - 0.5) * 0.15;
        float y0 = 0.05 + 0.4 * hf(hi, 1u), y1 = 0.95 - 0.3 * hf(hi, 2u);
        d = min(d, segDist(q, vec2(x, y0), vec2(x, y1)));
    }
    if (((h >> 6) & 1u) == 1u) {
        uint hd = hash_combine(h, 20u);
        vec2 a = vec2(0.15 + 0.3 * hfloat(hd), 0.1 + 0.2 * hf(hd, 1u));
        vec2 b = vec2(0.55 + 0.3 * hf(hd, 2u), 0.5 + 0.4 * hf(hd, 3u));
        if (hf(hd, 4u) < 0.5) { a.x = 1.0 - a.x; b.x = 1.0 - b.x; }
        d = min(d, segDist(q, a, b));
    }
    if (((h >> 7) & 3u) == 0u) {
        vec2 bq = abs(q - vec2(0.5, 0.3)) - vec2(0.25, 0.18);
        d = min(d, abs(max(bq.x, bq.y)) + max(min(max(bq.x, bq.y), 0.0), 0.0));
    }
    return d;
}

float flickerOn(uint seed, uint mode) { return neonFlicker(seed, mode, gTime); }

vec3 shadeNeon(vec2 uv, uint seed, uvec4 prm, vec3 rel) {
    uint colI = prm.y & 7u, style = (prm.y >> 3) & 7u, flick = (prm.y >> 6) & 3u;
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec3 col = neonColor(colI);
    vec2 m = uv * vec2(wM, hM);
    float on = flickerOn(seed, flick);

    // glyph layout
    bool vertical = hM >= wM;
    float cell = vertical ? wM * 0.78 : hM * 0.72;
    float pad = vertical ? (wM - cell) * 0.5 : (hM - cell) * 0.5;
    vec2 g;
    float gi;
    if (vertical) {
        float n = max(floor((hM - pad) / (cell * 1.08)), 1.0);
        float yTop = hM - pad;
        float k = (yTop - m.y) / (cell * 1.08);
        gi = floor(k);
        g = vec2((m.x - pad) / cell, 1.0 - fract(k) * 1.08);
        if (gi < 0.0 || gi >= n || g.y < 0.0) g = vec2(-1.0);
    } else {
        float n = max(floor((wM - pad) / (cell * 0.95)), 1.0);
        float k = (m.x - pad) / (cell * 0.95);
        gi = floor(k);
        g = vec2(fract(k) * 0.95, (m.y - pad) / cell);
        if (gi < 0.0 || gi >= n) g = vec2(-1.0);
    }
    float d = 1e3;
    if (all(greaterThanEqual(g, vec2(0.0))) && all(lessThanEqual(g, vec2(1.0))))
        d = glyphDist(hash_combine(seed, uint(gi)), g) * cell;     // metres
    // border tube on boards and banners
    if (style != 0u) {
        vec2 e = min(m, vec2(wM, hM) - m);
        float bd = abs(min(e.x, e.y) - 0.12);
        d = min(d, bd);
    }
    float px = max(fwidth(m.x), fwidth(m.y));
    float tube = 0.025 + cell * 0.018 + px * 0.5;
    float core = 1.0 - smoothstep(tube * 0.5, tube, d);
    float glow = exp(-d * 7.0 / max(cell, 0.6));
    // dead segments on some signs
    if (flick == 2u && hfloat(hash_combine(seed, uint(gi) + 99u)) < 0.3) { core *= 0.05; glow *= 0.05; }

    vec3 bg = vec3(0.012, 0.010, 0.014) + col * 0.05 * on;
    vec3 c = bg + (mix(col, vec3(1.0), 0.45) * core * 7.0 + col * glow * 1.6) * on;
    // far away: average emission (avoids shimmering strokes)
    float lod = smoothstep(0.03, 0.12, px / max(cell, 0.1));
    vec3 avg = bg + col * 0.9 * on;
    return mix(c, avg, lod);
}

vec3 shadeScreen(vec2 uv, uint seed, uvec4 prm) {
    float wM = float(prm.z) * 0.5, hM = float(prm.w) * 0.5;
    uint h = screenAdvert(seed, gTime);
    uint scene = h & 3u;
    vec3 c1 = neonColor(h >> 4), c2 = neonColor(h >> 8);
    vec3 col;
    float t = gTime;
    vec2 q = uv;
    if (scene == 0u) {
        // portrait advert: gradient, glowing face-like blob and a glyph column
        col = mix(c1 * 0.25, c2 * 0.6, q.y);
        float blob = exp(-dot((q - vec2(0.62, 0.55)) * vec2(wM / hM, 1.0), (q - vec2(0.62, 0.55)) * vec2(wM / hM, 1.0)) * 18.0);
        col += vec3(1.0, 0.75, 0.6) * blob * (0.8 + 0.2 * sin(t * 2.0));
        vec2 g = vec2(q.x / 0.15, fract(q.y * 5.0));
        if (q.x < 0.15) col += c1 * 2.0 * (1.0 - smoothstep(0.03, 0.08, glyphDist(hash_combine(h, uint(q.y * 5.0)), vec2(g.x, g.y))));
    } else if (scene == 1u) {
        // scrolling ticker rows
        float rows = max(floor(hM / 2.5), 2.0);
        float r = floor(q.y * rows);
        float x = q.x * wM / (hM / rows) + t * (1.5 + hfloat(hash_combine(h, uint(r)))) * (mod(r, 2.0) * 2.0 - 1.0);
        vec2 g = vec2(fract(x), fract(q.y * rows));
        float d = glyphDist(hash_combine(h, uint(floor(x)) + uint(r) * 977u), g);
        col = vec3(0.01, 0.02, 0.06) + c1 * 2.2 * (1.0 - smoothstep(0.03, 0.09, d));
    } else if (scene == 2u) {
        // pulsing rings
        vec2 cc = (q - 0.5) * vec2(wM / hM, 1.0);
        float r = length(cc);
        col = mix(c1, c2, 0.5 + 0.5 * sin(r * 22.0 - t * 3.0)) * (0.5 + 0.5 * sin(r * 22.0 - t * 3.0));
        col *= 1.2 * smoothstep(0.75, 0.0, r);
    } else {
        // brand mark: one huge glyph
        vec2 cc = (q - 0.5) * vec2(wM / hM, 1.0) + 0.5;
        float d = glyphDist(h, clamp(cc, 0.0, 1.0));
        col = mix(vec3(0.02, 0.0, 0.03), c2 * 0.15, q.y) + c1 * 2.5 * (1.0 - smoothstep(0.04, 0.1, d)) * (0.8 + 0.2 * sin(t * 4.0));
    }
    // transition static
    float ph = fract((gTime + hfloat(seed) * 30.0) / 9.0);
    if (ph < 0.025) col = vec3(hfloat(hash2i(h, int(q.x * 200.0), int(q.y * 120.0 + t * 60.0)))) * 0.8;
    // LED pixel grid and scanline when close
    vec2 pm = uv * vec2(wM, hM) / 0.12;
    float px = max(fwidth(pm.x), fwidth(pm.y));
    float grid = mix(0.55 + 0.45 * smoothstep(0.45, 0.2, length(fract(pm) - 0.5)), 1.0, smoothstep(0.3, 0.8, px));
    col *= grid * (0.92 + 0.08 * sin(uv.y * 300.0 - gTime * 10.0));
    return col * 1.8;
}

// seed: a street lamp head's fault (lampState)
vec3 shadeEmissive(uvec4 prm, uint seed) {
    uint mode = prm.z;
    if (mode == 1u) {   // aircraft warning light
        float ph = float(prm.w) / 255.0;
        float b = smoothstep(0.0, 0.05, fract(gTime / 1.6 + ph)) * smoothstep(0.35, 0.2, fract(gTime / 1.6 + ph));
        return vec3(1.0, 0.05, 0.02) * (0.3 + 30.0 * b);
    }
    if (mode >= 2u) return vec3(0.02, 0.02, 0.021) + lampColor(int(mode) - 2) * lampState(seed, gTime) * 22.0;   // dead: cold glass
    return neonColor(prm.y) * 8.0;
}

vec3 shadeMetal(vec3 p, vec3 rel, vec3 n, vec2 uv, uvec4 prm) {
    vec3 V = -normalize(rel);
    uint v = prm.y;
    vec3 alb = v == 1u ? vec3(0.09, 0.09, 0.095) : v == 2u ? vec3(0.05) : v == 4u ? vec3(0.04, 0.045, 0.042) : vec3(0.03);
    if (v == 1u) alb *= 0.75 + 0.25 * step(0.5, fract(uv.y / 0.12));  // vents
    if (v == 0u) alb = vec3(0.07, 0.068, 0.065) * (0.7 + 0.5 * fbm(gWorld.xz * 0.5 + uv.y, 0x3E7u));
    if (v == 5u) alb = vec3(0.075, 0.072, 0.068) * (0.75 + 0.45 * fbm(gWorld.xz * 2.0 + p.y * 0.7, 0x3E8u));   // concrete
    if (v == 6u) {                                                                                       // solar panel
        vec2 cell = fract(uv * vec2(6.0, 4.0));
        float grid = step(0.06, cell.x) * step(0.06, cell.y);
        vec3 spec;
        vec3 diff = wallLighting(p, n, V, 120.0, 0.9, p.y, spec);
        return vec3(0.008, 0.012, 0.03) * grid * diff + spec + envReflection(reflect(-V, n)) * 0.25;
    }
    if (v == 7u) {                                                                                       // glass pyramid
        float F = glassFresnel(n, V);
        float mull = max(step(0.94, fract(uv.x * 8.0)), step(0.94, fract(uv.y * 8.0)));
        vec3 glow = mix(vec3(0.9, 0.7, 0.45), vec3(0.6, 0.8, 1.0), hfloat(vSeed)) * 0.35;
        glassRefl(n, F * (1.0 - mull * 0.8));
        return mix(glow * (1.0 - F) + envReflection(reflect(-V, n)) * F, vec3(0.02), mull * 0.8);
    }
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 40.0, 0.25, p.y, spec);
    return alb * diff + spec;
}

vec3 shadeRoof(vec3 p, vec3 rel, vec3 n, vec2 uv, uvec4 prm) {
    vec3 V = -normalize(rel);
    float g = fbm(gWorld.xz * 0.6, 0x400Fu);
    vec3 alb = vec3(0.04, 0.04, 0.042) * (0.6 + 0.8 * g);
    if (prm.w == 1u) {   // clay tiles on a hip roof
        float row = fract(p.y / 0.32);
        alb = vec3(0.075, 0.035, 0.025) * (0.6 + 0.4 * smoothstep(0.0, 0.25, row)) * (0.75 + 0.5 * g);
    }
    if (prm.z == 1u) alb *= 0.8;   // soffit under an overhang
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 10.0, 0.05, p.y, spec);
    return alb * diff + spec;
}

/* Entrances (BDoor in src/building.h): glass doors into a lit lobby, steel
 * service doors and roller-shuttered loading bays. */
vec3 shadeDoor(vec3 p, vec3 rel, vec3 n, vec2 uv, uint seed, uvec4 prm) {
    vec3 V = -normalize(rel);
    float wM = max(float(prm.z) * 0.5, 0.5), hM = max(float(prm.w) * 0.5, 0.5);
    vec2 m = uv * vec2(wM, hM);
    float aa = max(fwidth(m.x), fwidth(m.y)) + 0.002;
    vec3 spec;
    vec3 diff = wallLighting(p, n, V, 60.0, 0.3, p.y, spec);
    float edge = min(min(m.x, wM - m.x), min(m.y, hM - m.y));
    if (prm.y == 0u) {
        vec3 inside = mix(vec3(1.0, 0.86, 0.66), vec3(0.75, 0.86, 1.0), hfloat(seed)) * 0.45;
        float frame = 1.0 - smoothstep(0.07, 0.07 + aa, edge);
        if (wM > 1.6) frame = max(frame, 1.0 - smoothstep(0.03, 0.03 + aa, abs(m.x - wM * 0.5)));   // double doors
        float bar = band(m.y, 1.0, 1.08, aa) * step(0.15, edge);
        float F = glassFresnel(n, V);
        vec3 glass = inside * (1.0 - F) + envReflection(reflect(-V, n)) * F + paneSheen(p, n, V);
        glassRefl(n, F * (1.0 - clamp(frame + bar, 0.0, 1.0)));
        return mix(glass, vec3(0.03) * diff + spec, clamp(frame + bar, 0.0, 1.0));
    }
    if (prm.y == 1u) {
        vec3 steel = vec3(0.06, 0.065, 0.07) * (0.8 + 0.4 * fbm(m * 3.0, seed & 0xFFFFu));
        float win = band(m.x, wM * 0.3, wM * 0.7, aa) * band(m.y, hM * 0.6, hM * 0.85, aa);
        return mix(steel * diff + spec, vec3(0.5, 0.45, 0.35) * 0.4, win);
    }
    float rib = 0.6 + 0.4 * smoothstep(0.3, 0.5, abs(fract(m.y / 0.12) - 0.5) * 2.0);
    vec3 shutter = vec3(0.075, 0.07, 0.06) * rib * (0.6 + 0.6 * fbm(m * 0.8, seed & 0xFFFFu));
    float stripe = step(hM - 0.35, m.y) * step(0.5, fract(m.x / 0.6 + m.y / 0.6));   // hazard stripes at the top
    shutter = mix(shutter, vec3(0.12, 0.09, 0.0), stripe);
    return shutter * diff + spec;
}

/* ======================================================================
 * Litter (src/litter.c): rubbish lying about the streets, as ordinary
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
    vec3 diff = ambientLight(n, p.y) * ao;
    vec3 spec = vec3(0.0);
    cityLights(p, n, V, 0.0, f0, rough, diff, spec);
    diff += neonBounce(gWorld, p.y) * ao;
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
    uint h = seed, var = (seed >> 4) & 3u;   // var: the variant src/litter.c built
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

/* ====================================================================== */

void main() {
    gl_SampleMask[0] = -1;     // glass over real interiors covers fewer samples (paneCoverage)
    gC = chunks[vChunk];
    gTime = frame.camPos.w;
    gWorld = vLocal + vec3(float(gC.info.x & 63) * CHUNK_SIZE, 0.0, float(gC.info.y & 63) * CHUNK_SIZE);
    vec3 n = normalize(vNrm);
    uint mat = vParams.x;
    vec3 V = -normalize(vRel);

    vec3 col;
    if (mat <= MAT_LOT)            col = shadeGround(mat, vLocal, n, V, vUV, vSeed, vParams);
    else if (mat == MAT_FACADE)    col = shadeFacade(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_ROOF)      col = shadeRoof(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_DOOR)      col = shadeDoor(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_NEON)      col = shadeNeon(vUV, vSeed, vParams, vRel);
    else if (mat == MAT_METAL)     col = shadeMetal(vLocal, vRel, n, vUV, vParams);
    else if (mat == MAT_EMISSIVE)  col = shadeEmissive(vParams, vSeed);
    else if (mat == MAT_SCREEN)    col = shadeScreen(vUV, vSeed, vParams);
    else if (mat == MAT_INTERIOR)  col = shadeInterior(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_LITTER)    col = shadeLitter(vLocal, vRel, n, vUV, vSeed, vParams);
    else if (mat == MAT_ARCOLOGY)  col = shadeArcology(vLocal, vRel, n, vUV, vSeed, vParams);
    else {
        // LED strip: steady or chasing
        float chase = vParams.z == 1u ? 0.25 + 0.75 * step(0.5, fract(vUV.x / 6.0 - gTime * 0.7)) : 1.0;
        col = neonColor(vParams.y) * 6.0 * chase;
    }

    float dist = length(vRel);
    float fog = fogAmount(vRel, dist);
    outColor = vec4(mix(col, hazeColor(vRel / max(dist, 1e-4)), fog), 1.0);
    outRefl = vec4(gRefl.xyz, gRefl.w * (1.0 - fog));
}
