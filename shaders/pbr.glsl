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
    vec4  wet;      // its file's wet and snow: read by the land's shaders
    vec4  snow;
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
// In the rain: how much of its albedo it loses (x), how readily water stands
// in its hollows (y, 0 none .. 1 most), and how its wet film shines (z).
vec3 pbrWet(int m) { return materials[m].wet.xyz; }
// Snow over it: how soon it lies (x: the cover's snow weight s lies as
// 1 - (1 - s)^x), and where first (y: 0 in its height map's hollows .. 1 on its tops).
vec2 pbrSnow(int m) { return materials[m].snow.xy; }

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
// pbrGrain at image coordinates st (q over the tile), gx, gy st's screen derivatives.
Grain pbrGrainAt(int m, vec2 st, vec2 gx, vec2 gy, vec3 T, vec3 B, vec3 N) {
    int i = m * PBR_MAPS_PER_MATERIAL;
    vec4 k = pbrParams(m);
    Grain g;
    g.tint = mix(vec3(1.0), textureGrad(pbrMaps[i + PBR_ALBEDO], st, gx, gy).rgb / max(pbrAlbedoMean(m), vec3(1e-3)), k.x);
    vec3 tn = textureGrad(pbrMaps[i + PBR_NORMAL], st, gx, gy).xyz * 2.0 - 1.0;
    g.n = normalize(N + (T * tn.x + B * tn.y) * k.y / max(tn.z, 0.2));
    vec3 orm = textureGrad(pbrMaps[i + PBR_ORM], st, gx, gy).rgb;
    g.ao = orm.r; g.rough = orm.g;
    return g;
}
Grain pbrGrain(int m, vec2 q, vec2 gx, vec2 gy, vec3 T, vec3 B, vec3 N) {
    vec2 tile = pbrTile(m);
    return pbrGrainAt(m, q / tile, gx / tile, gy / tile, T, B, N);
}

/* pbrGrain with its relief in depth, for a material whose params.z gives one (metres from the
 * height map's tops, at height 1 in the surface's plane, down to height 0; the corrugated sheets):
 * parallax occlusion mapping. The view ray steps down through the height field in even layers
 * until it passes under it, and the hit goes between the last two by where it crossed, so the
 * ribs hide the troughs behind them as the eye moves. The layers follow how many pixels the
 * deepest shift spans: none where it is under one (far off, or seen square on, the plain grain),
 * the depth fading in over the next two so the shift never pops, up to 32 near and at a slant.
 * Down in the relief the light reaches less (a cavity term, into ao, by the height where the ray
 * met it, so the same far off as near): the trough's floor takes 0.6. V: towards the eye.
 * Materials with no params.z take pbrGrain as it is.
 *
 * It also leaves where the ray met the relief (gRel*) for its lights' self-shadowing
 * (reliefShadow), which the caller turns on round its lighting (reliefLightsOn, reliefLightsOff). */
bool  gReliefReady = false, gReliefOn = false;
int   gRelM = 0;
vec2  gRelSt = vec2(0.0), gRelSx = vec2(0.0), gRelSy = vec2(0.0), gRelScale = vec2(0.0);
vec3  gRelT = vec3(1.0, 0.0, 0.0), gRelB = vec3(0.0, 1.0, 0.0), gRelN = vec3(0.0, 0.0, 1.0);
float gRelH = 1.0, gRelFade = 0.0;
void reliefLightsOn()  { gReliefOn = gReliefReady; }
void reliefLightsOff() { gReliefOn = false; }

Grain pbrGrainRelief(int m, vec2 q, vec2 gx, vec2 gy, vec3 T, vec3 B, vec3 N, vec3 V) {
    vec2 tile = pbrTile(m);
    vec2 st = q / tile, sx = gx / tile, sy = gy / tile;
    float depth = pbrParams(m).z;
    float vn = dot(V, N);
    float px = max(min(length(gx), length(gy)), 1e-5);   // metres a pixel spans, the finer way
    gReliefReady = false;
    if (depth > 0.0 && vn > 0.0) {
        // st's shift per metre of depth: the ray runs against V, along T (q.x) and down B (q.y runs down the image);
        // held to about 80 degrees off square, where the shift is under six depths
        vec2 dir = vec2(-dot(V, T), dot(V, B)) / max(vn, 0.17);
        float shiftPx = depth * length(dir) / px;
        if (shiftPx > 1.0) {
            float fade = smoothstep(1.0, 3.0, shiftPx);
            dir *= depth * fade / tile;
            int layers = int(clamp(shiftPx * 0.5, 4.0, 32.0));
            float inc = 1.0 / float(layers), d = 0.0;
            int oi = m * PBR_MAPS_PER_MATERIAL + PBR_ORM;
            float gap = textureGrad(pbrMaps[oi], st, sx, sy).b - 1.0, prev = gap;   // height above the ray (<= 0: above)
            vec2 p = st;
            for (int k = 0; k < 32; ++k) {
                if (k >= layers || gap >= 0.0) break;
                prev = gap;
                p += dir * inc; d += inc;
                gap = textureGrad(pbrMaps[oi], p, sx, sy).b - (1.0 - d);
            }
            float back = gap > 0.0 ? inc * (gap / max(gap - prev, 1e-5)) : 0.0;   // back to where it crossed
            st = p - dir * back;
        }
    }
    Grain g = pbrGrainAt(m, st, sx, sy, T, B, N);
    if (depth > 0.0) {
        float h = textureGrad(pbrMaps[m * PBR_MAPS_PER_MATERIAL + PBR_ORM], st, sx, sy).b;
        g.ao *= 1.0 - 0.4 * (1.0 - h);
        // the shadows show while a pixel spans less than the relief's depth twice over (the ribs a few
        // pixels across), seen at any angle: a lamp raking a wall seen square on still throws them
        gRelFade = 1.0 - smoothstep(depth, 2.0 * depth, px);
        gReliefReady = gRelFade > 0.0 && h < 0.98;   // on a crest nothing stands over it
        gRelM = m; gRelSt = st; gRelSx = sx; gRelSy = sy; gRelScale = depth / tile;
        gRelT = T; gRelB = B; gRelN = N; gRelH = h;
    }
    return g;
}

/* How much of a light from direction L (towards it) reaches the point of the relief pbrGrainRelief
 * found, while a caller has it on (reliefLightsOn): a ray from there up towards the light, eight
 * steps until it clears the height map's tops, and how far the relief rises over it on the way,
 * soft (a rise of a sixth of the depth is all shadow; nearer steps count more than farther ones,
 * so a shadow's edge softens away from its rib, as a lamp's wide head would). Light from behind
 * the surface is left to the lighting's own n.L. 1 with no relief on. */
float reliefShadow(vec3 L) {
    if (!gReliefOn) return 1.0;
    float ln = dot(L, gRelN);
    if (ln <= 0.0) return 1.0;
    float rise = 1.0 - gRelH;
    // st's shift per unit of height climbed towards the light (q.x along T, q.y down B), held to
    // about 84 degrees off square: a lamp just along the wall throws ten depths' shadows
    vec2 dir = vec2(dot(L, gRelT), -dot(L, gRelB)) / max(ln, 0.1) * gRelScale;
    int oi = gRelM * PBR_MAPS_PER_MATERIAL + PBR_ORM;
    float block = 0.0;
    for (int k = 1; k <= 8; ++k) {
        float t = rise * float(k) * 0.125;
        float h = textureGrad(pbrMaps[oi], gRelSt + dir * t, gRelSx, gRelSy).b;
        block = max(block, (h - gRelH - t) * (1.0 - 0.06 * float(k)));
    }
    return 1.0 - smoothstep(0.0, 0.17, block) * gRelFade;
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
