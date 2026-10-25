// Part of shaders/city.frag, which includes the parts in order: authored PBR materials, wetness, puddles and ripples.

/* ======================================================================
 * Authored PBR materials (src/core/pbr.h): each defined by its file,
 * assets/materials/<name>.json, and loaded by src/render/materials.c. A
 * material's maps are bound at m * PBR_MAPS_PER_MATERIAL + map and its
 * properties are materials[m]. Surfaces opt in per material; one that did
 * not load (pbrLoaded false) is bound as placeholders, and its surfaces
 * keep their procedural look.
 * ==================================================================== */

layout(set = 0, binding = 3) uniform sampler2D pbrMaps[PBR_TEXTURE_COUNT];

// MaterialGPU (src/render/materials.h)
struct Material {
    vec4  tile;     // xy metres per image repeat
    vec4  albedo;   // rgb the albedo map's mean (linear), a the albedo's scale
    vec4  relief;   // x the height map's median, y its spread
    vec4  params;   // the material's own (its file says what they are)
    uvec4 info;     // x PBR_LOADED | PBR_HAS_EMISSIVE
};
layout(std140, set = 0, binding = 8) uniform Materials { Material materials[PBR_MATERIAL_COUNT]; };

struct Surface { vec3 albedo; vec3 n; float ao; float rough; float height; };

bool pbrLoaded(int m) { return (materials[m].info.x & PBR_LOADED) != 0u; }
// Metres of surface per repeat of the material's image.
vec2 pbrTile(int m) { return materials[m].tile.xy; }
// What its albedo is scaled by to sit in the city's night palette.
float pbrAlbedoScale(int m) { return materials[m].albedo.a; }
// Its albedo map's mean (linear): dividing by it leaves the texture's variation, to tint as a surface needs.
vec3 pbrAlbedoMean(int m) { return materials[m].albedo.rgb; }
// A height from its map (orm.b) relative to the map's median, in spreads, so
// water levels mean the same on each material: from about -0.5 (joints,
// hollows) to 0.5.
float pbrRelief(int m, float h) { return (h - materials[m].relief.x) / materials[m].relief.y; }
vec4 pbrParams(int m) { return materials[m].params; }

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


