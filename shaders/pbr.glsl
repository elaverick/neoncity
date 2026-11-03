#ifndef PBR_GLSL
#define PBR_GLSL

/* ======================================================================
 * Authored PBR materials (src/core/pbr.h): each defined by its file,
 * assets/materials/<name>.json, and loaded by src/render/materials.c. A
 * material's maps are bound at m * PBR_MAPS_PER_MATERIAL + map and its
 * properties are materials[m]. Surfaces opt in per material; one that did
 * not load (pbrLoaded false) is bound as placeholders, and its surfaces
 * keep their procedural look.
 *
 * Shared by the city (shaders/city/surface.glsl) and the terrain
 * (shaders/terrain/ground.glsl): both sets bind the maps at 3 and the
 * properties at 8 (src/render/renderer_pipelines.c). Needs common.glsl.
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

/* An authored material under a procedural surface's own colour (a building's palette, a plant's
 * paint, a lobby's tone): the surface keeps its colour and takes the material's grain. tint is
 * the albedo over its mean (albedo_mean, per channel), so the photograph's own hue drops out and
 * only its variation is left: multiply the surface's colour by it. The file's params give how much
 * of that variation shows (x, 1 all) and how strongly the normal map bends the surface (y, 1 as
 * authored). q: metres in the surface (image right, image down: texture rows run down the image,
 * so an upright image on a wall takes q.y = -height and B = +y); gx, gy: q's screen derivatives,
 * taken in uniform control flow, so it can be called from inside per-pixel branches. T, B: the
 * world directions of image right and up. */
struct Grain { vec3 tint; vec3 n; float ao; float rough; };
Grain pbrGrain(int m, vec2 q, vec2 gx, vec2 gy, vec3 T, vec3 B, vec3 N) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec2 tile = pbrTile(m);
    vec2 st = q / tile;
    gx /= tile; gy /= tile;
    vec4 k = pbrParams(m);
    Grain g;
    g.tint = mix(vec3(1.0), textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb / max(pbrAlbedoMean(m), vec3(1e-3)), k.x);
    vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    g.n = normalize(N + (T * tn.x + B * tn.y) * k.y / max(tn.z, 0.2));
    vec3 orm = textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).rgb;
    g.ao = orm.r; g.rough = orm.g;
    return g;
}
// pbrGrain's tint alone (where a surface's lighting takes no normal map, as a lobby's finishes), and the
// roughness map in rough.
vec3 pbrTint(int m, vec2 q, vec2 gx, vec2 gy, out float rough) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec2 tile = pbrTile(m);
    rough = textureGrad(pbrMaps[i + PBR_ORM], q / tile, gx / tile, gy / tile).g;
    return mix(vec3(1.0), textureGrad(pbrMaps[i + PBR_ALBEDO], q / tile, gx / tile, gy / tile).rgb / max(pbrAlbedoMean(m), vec3(1e-3)), pbrParams(m).x);
}

// pbrTint's tint by a footprint instead of screen derivatives, for surfaces found per pixel along a ray
// (the rooms seen through windows), where none can be taken: px, the metres of surface a pixel spans.
vec3 pbrTintLod(int m, vec2 q, float px) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec2 tile = pbrTile(m);
    float lod = log2(max(px / min(tile.x, tile.y) * float(textureSize(pbrMaps[i + PBR_ALBEDO], 0).x), 1.0));
    return mix(vec3(1.0), textureLod(pbrMaps[i + PBR_ALBEDO], q / tile, lod).rgb / max(pbrAlbedoMean(m), vec3(1e-3)), pbrParams(m).x);
}

// What a procedural surface's Blinn-Phong highlight is scaled by where the grain's roughness map is
// glossier or duller than middling: 1 at roughness 0.5.
float grainSpec(Grain g) { return mix(1.75, 0.25, g.rough); }

// The frame a flat procedural surface takes an authored grain in, from its normal: a level one
// is mapped in world x and z (+x image right, -z image up, as the roads), any other runs along
// its level line (image right) and up its slope (image up). q: metres, in that frame (q.y runs
// down the image, as texture rows do).
void planarFrame(vec3 n, vec3 w, out vec3 T, out vec3 B, out vec2 q) {
    if (abs(n.y) > 0.9) { T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, 0.0, -1.0); }
    else { T = normalize(vec3(n.z, 0.0, -n.x)); B = cross(n, T); }
    q = vec2(dot(w, T), -dot(w, B));
}

// Broad relief: the height map averaged over many texels (a biased mip), for
// shapes like puddles that should follow hollows rather than every grain.
float pbrBroadHeight(int m, vec2 st, float bias) { return texture(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, bias).b; }

#endif
