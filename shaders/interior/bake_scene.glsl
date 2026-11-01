/* The interior baker's scene (tools/interior_bake): one office module's
 * geometry in a TLAS, its materials and lights, and IM1's simple direct light
 * (N.L from point lights, with shadows), which the representation lab uses.
 * The path tracer that bakes with full GI is pathtrace.glsl. Shared by the
 * capture (capture.comp, capture_mpi.comp), the ground truth (truth.comp) and
 * the reconstruction (recon.comp), so the truth and the impostor are lit the
 * same and their difference is the representation's alone.
 *
 * Module frame as shaders/city/impostor.glsl: x along the wall, y up, z into
 * the building, the near glass at z = 0. A periodic module's instances are
 * repeated along x in the TLAS, so rays cross its ends into the next copy. */
#extension GL_EXT_ray_query : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(std430, set = 0, binding = 1) readonly buffer Verts { float verts[]; };
layout(std430, set = 0, binding = 2) readonly buffer Indices { uint indices[]; };
struct Inst { uint firstIndex, baseVertex, material, flags, firstEmitter, hasAttrs, pad1, pad2; vec4 bmin, bmax; };   // firstEmitter: ~0u, none; bounds in its mesh's frame
layout(std430, set = 0, binding = 3) readonly buffer Insts { Inst insts[]; };
// albedo.a roughness; emission.a metallic; surface: a dielectric's F0 (0: no specular lobe), clearcoat, its roughness;
// tex: texture layers, -1 none (colour, metal-roughness, normal, how an emitter looks); tile.x: metres a tile of box mapping, or 0: the mesh's UVs
struct Mat { vec4 albedo; vec4 emission; vec4 surface; ivec4 tex; vec4 tile; };
layout(std430, set = 0, binding = 4) readonly buffer Mats { Mat mats[]; };
struct Light { vec4 pos; vec4 power; };   // a point emitting downwards with a cosine lobe; power: intensity straight down
layout(std430, set = 0, binding = 5) readonly buffer Lights { Light lights[]; };

// A normal and a UV per vertex (nx, ny, nz, u, v), beside the positions; and the textures, in arrays of one size.
layout(std430, set = 0, binding = 11) readonly buffer Attrs { float attrs[]; };
layout(set = 0, binding = 12) uniform sampler2DArray gColour;   // sRGB
layout(set = 0, binding = 13) uniform sampler2DArray gData;     // linear: metal-roughness, normal

/* tools/interior_bake/bake.h: BakeParams */
layout(set = 0, binding = 10, std140) uniform Params {
    vec4  P_module;     // W, the room's height (floor to the slab's underside), D, period (0: not periodic)
    vec4  P_volume;     // the building's x range (a corner module's side glass), ambient, thin-shell thickness
    vec4  P_grid;       // the impostor grid's origin x, y, texels per metre, wrapX
    ivec4 P_gridDim;    // nx, ny, views, capture slots a texel
    ivec4 P_views;      // Nu, Nv, kind (IMP_GRID_*), K
    vec4  P_viewRange;  // ImpViews.range
    vec4  P_eval;       // the evaluation grid's origin x, y and pixels per metre in x, y
    ivec4 P_evalDim;    // w, h, lastSlot, light count
    vec4  P_cam;        // camera position, the weight of a view whose march guessed
    vec4  P_outside;    // radiance seen out of the far glass; light range
    vec4  P_glass;      // partition glass: R0, transmittance, radiance it reflects; rays a pixel across (truth, recon)
    ivec4 P_march;      // max steps, bisection steps, MPI slices, -
    vec4  P_march2;     // max texels a step, the weight of a view that looked off its grid, nearest-hit sigma (m; 0 off), both ends
    ivec4 P_flags;      // the hit's colour filtered (impHitColor), a perspective camera (truth.comp: the beauty render), -, -
    ivec4 P_trace;      // the path tracer: this pass's first sample, its samples, the samples in all, most bounces
    ivec4 P_trace2;     // light (0: IM1's direct, 1: path traced), layers path traced (capture), emitters, seed
    vec4  P_trace3;     // the most an indirect sample may bring (0: no clamp), next-event estimation (1) or BSDF sampling alone (0),
                        // the brightest a light may look to the eye (0: as it shines), the exterior's period along x (m)
    ivec4 P_trace4;     // the first view of this pass (capture), whether paths leave by the glass planes (0: a sealed test box),
                        // the light basis this pass bakes (BASIS_*), -
    vec4  P_camRight;   // the perspective camera's axes; right and up are scaled by the half-size of the view at 1 m
    vec4  P_camUp;
    vec4  P_camFwd;
};

/* The light bases (bake.h: BASIS_*): a bake traces each alone, so the engine can colour the exterior
 * ones and the screens by where the module lands. BASIS_ALL: every emitter and no exterior (the tests). */
#define BASIS_INTERIOR 0
#define BASIS_STREET   1
#define BASIS_NEON     2
#define BASIS_SKY      3
#define BASIS_SCREENS  4
#define BASIS_ALL      5

/* Instance flags (bake.h: BF_*) and masks (gpu.c). */
#define BF_GLASS   1u
#define BF_OPEN    2u
#define BF_EMITTER 4u
#define BF_LEAKY   8u
#define MASK_CASTER  1u
#define MASK_GLASS   2u
#define MASK_EMITTER 4u

struct Hit { float t; vec3 n; uint inst; uint prim; bool front; vec2 bary; vec3 e1, e2; mat4x3 w2o; };   // e1, e2: the triangle's world edges

vec3 bakeVertex(uint i) { return vec3(verts[3u * i], verts[3u * i + 1u], verts[3u * i + 2u]); }

/* The nearest surface of the instances in mask along o + t d with t in
 * (tmin, tmax). n is the world geometric normal, from the triangle's winding
 * (instances are never mirrored), pointing out of a solid. */
bool traceMasked(vec3 o, vec3 d, float tmin, float tmax, uint mask, out Hit h) {
    h.t = 0.0; h.n = vec3(0.0, 1.0, 0.0); h.inst = 0u; h.prim = 0u; h.front = true;
    h.bary = vec2(0.0); h.e1 = vec3(0.0); h.e2 = vec3(0.0); h.w2o = mat4x3(1.0);
    if (tmax <= tmin) return false;
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT, mask, o, tmin, d, tmax);
    while (rayQueryProceedEXT(q)) {}
    if (rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionTriangleEXT) return false;
    h.t = rayQueryGetIntersectionTEXT(q, true);
    h.inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, true));
    h.prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true));
    mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(q, true);
    Inst in_ = insts[h.inst];
    uint b = in_.firstIndex + h.prim * 3u;
    vec3 v0 = o2w * vec4(bakeVertex(indices[b] + in_.baseVertex), 1.0);
    vec3 v1 = o2w * vec4(bakeVertex(indices[b + 1u] + in_.baseVertex), 1.0);
    vec3 v2 = o2w * vec4(bakeVertex(indices[b + 2u] + in_.baseVertex), 1.0);
    h.e1 = v1 - v0;
    h.e2 = v2 - v0;
    h.bary = rayQueryGetIntersectionBarycentricsEXT(q, true);
    h.w2o = rayQueryGetIntersectionWorldToObjectEXT(q, true);
    vec3 n = cross(h.e1, h.e2);
    h.n = length(n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    h.front = dot(h.n, d) < 0.0;
    return true;
}

/* What a surface is at a hit, from its material and its textures. ng: the triangle's normal on the
 * ray's side; ns: what it shades with, from the mesh's normals and the normal map. A surface with
 * neither is its material alone, so ns is ng. footprint: the metres a sample covers there, which
 * picks the mips. look: how an emitter's face looks (its light is even: pathtrace.glsl). */
struct Surf { vec3 albedo; float rough, metal, f0, coat, coatRough; vec3 ng, ns, look; };

vec3 attrNormal(uint i) { return vec3(attrs[5u * i], attrs[5u * i + 1u], attrs[5u * i + 2u]); }
vec2 attrUv(uint i) { return vec2(attrs[5u * i + 3u], attrs[5u * i + 4u]); }

Surf surfAt(Hit h, vec3 p, vec3 d, float footprint) {
    Inst in_ = insts[h.inst];
    Mat mt = mats[in_.material];
    Surf s;
    s.albedo = mt.albedo.rgb; s.rough = mt.albedo.a; s.metal = mt.emission.a;
    s.f0 = mt.surface.x; s.coat = mt.surface.y; s.coatRough = mt.surface.z;
    s.ng = h.front ? h.n : -h.n; s.ns = s.ng; s.look = vec3(1.0);
    bool textured = any(greaterThanEqual(mt.tex, ivec4(0)));
    if (!textured && in_.hasAttrs == 0u) return s;

    uint b = in_.firstIndex + h.prim * 3u;
    uint i0 = indices[b] + in_.baseVertex, i1 = indices[b + 1u] + in_.baseVertex, i2 = indices[b + 2u] + in_.baseVertex;
    vec3 w = vec3(1.0 - h.bary.x - h.bary.y, h.bary.x, h.bary.y);
    if (in_.hasAttrs != 0u) {
        vec3 obj = attrNormal(i0) * w.x + attrNormal(i1) * w.y + attrNormal(i2) * w.z;
        if (dot(obj, obj) > 1e-6) {
            s.ns = normalize(obj * mat3(h.w2o));   // the inverse transpose
            if (dot(s.ns, s.ng) < 0.0) s.ns = -s.ns;
        }
    }
    if (textured) {
        float size = float(textureSize(gColour, 0).x);
        vec2 uv = vec2(0.0);
        vec3 T = vec3(0.0), B = vec3(0.0);
        float density = 0.0;   // texels a metre
        if (mt.tile.x > 0.0) {   // box mapping, by the face's axis
            vec3 a = abs(h.n);
            if (a.x >= a.y && a.x >= a.z) { uv = p.zy; T = vec3(0.0, 0.0, 1.0); B = vec3(0.0, 1.0, 0.0); }
            else if (a.y >= a.z) { uv = p.xz; T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, 0.0, 1.0); }
            else { uv = p.xy; T = vec3(1.0, 0.0, 0.0); B = vec3(0.0, 1.0, 0.0); }
            uv /= mt.tile.x;
            density = size / mt.tile.x;
        } else {
            vec2 t0 = attrUv(i0), t1 = attrUv(i1), t2 = attrUv(i2);
            uv = t0 * w.x + t1 * w.y + t2 * w.z;
            vec2 d1 = t1 - t0, d2 = t2 - t0;
            float det = d1.x * d2.y - d1.y * d2.x, world = length(cross(h.e1, h.e2));
            if (abs(det) > 1e-12 && world > 1e-14) {
                T = (h.e1 * d2.y - h.e2 * d1.y) / det;
                B = (h.e1 * d2.x - h.e2 * d1.x) / det;   // image up: against +v
                density = size * sqrt(abs(det) / world);
            }
        }
        float lod = log2(max(density * footprint, 1.0));
        if (mt.tex.x >= 0) s.albedo *= textureLod(gColour, vec3(uv, float(mt.tex.x)), lod).rgb;
        if (mt.tex.y >= 0) {
            vec3 orm = textureLod(gData, vec3(uv, float(mt.tex.y)), lod).rgb;
            s.rough *= orm.g;
            s.metal *= orm.b;
        }
        if (mt.tex.z >= 0 && dot(T, T) > 0.0) {
            vec3 ts = textureLod(gData, vec3(uv, float(mt.tex.z)), lod).rgb * 2.0 - 1.0;
            vec3 tt = T - s.ns * dot(s.ns, T), bt = B - s.ns * dot(s.ns, B);
            if (dot(tt, tt) > 1e-12 && dot(bt, bt) > 1e-12) {
                vec3 m = normalize(tt) * ts.x + normalize(bt) * ts.y + s.ns * max(ts.z, 0.0);
                if (dot(m, m) > 1e-8) s.ns = normalize(m);
            }
        }
        if (mt.tex.w >= 0) s.look = textureLod(gColour, vec3(uv, float(mt.tex.w)), lod).rgb;
    }
    if (dot(s.ns, -d) < 0.02) s.ns = s.ng;   // past the horizon: the triangle's own
    return s;
}

bool traceNext(vec3 o, vec3 d, float tmin, float tmax, out Hit h) { return traceMasked(o, d, tmin, tmax, 0xFFu, h); }

bool shadowed(vec3 o, vec3 d, float tmax) {
    rayQueryEXT q;
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT, MASK_CASTER, o, 0.0, d, tmax);
    while (rayQueryProceedEXT(q)) {}
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionTriangleEXT;
}

/* Radiance leaving surface point p (normal n, facing the viewer) of material m. */
vec3 shadeSurface(vec3 p, vec3 n, uint m) {
    Mat mt = mats[m];
    float W = P_module.x, period = P_module.w, range = P_outside.w;
    vec3 E = vec3(0.0);
    vec3 o = p + n * 2e-3;
    for (int i = 0; i < P_evalDim.w; ++i) {
        vec3 lp = lights[i].pos.xyz;
        float dx = p.x - lp.x;
        if (period > 0.0) dx -= period * floor(dx / period + 0.5);
        for (int k = -1; k <= 1; ++k) {
            if (period <= 0.0 && k != 0) continue;
            vec3 L = vec3(-dx + float(k) * period, lp.y - p.y, lp.z - p.z);   // to the copy of the light k periods along
            float d2 = dot(L, L);
            if (d2 > range * range || d2 < 1e-6) continue;
            float dist = sqrt(d2);
            vec3 l = L / dist;
            float cs = dot(n, l), ce = l.y;
            if (cs <= 0.0 || ce <= 0.0) continue;
            if (shadowed(o, l, dist - 0.02)) continue;
            E += lights[i].power.rgb * (cs * ce / d2);
        }
    }
    return mt.albedo.rgb * (E / 3.14159265 + P_volume.z) + mt.emission.rgb;
}

/* Where a ray from o (in the building) leaves it: the far glass, or a corner
 * module's side glass. */
float volumeExit(vec3 o, vec3 d) {
    float t = d.z != 0.0 ? (P_module.z - o.z) / d.z : 1e9;
    if (d.x < 0.0 && o.x >= P_volume.x) t = min(t, (P_volume.x - o.x) / d.x);
    if (d.x > 0.0 && o.x <= P_volume.y) t = min(t, (P_volume.y - o.x) / d.x);
    return max(t, 0.0);
}

/* Where a path's ray from o (in the building, heading anywhere) leaves it:
 * the near glass, the far glass or a corner module's side glass. A sealed
 * test box has none (P_trace4.y = 0). */
float pathExit(vec3 o, vec3 d) {
    if (P_trace4.y == 0) return 1e9;
    float t = d.z > 0.0 ? (P_module.z - o.z) / d.z : d.z < 0.0 ? -o.z / d.z : 1e9;
    if (d.x < 0.0 && o.x >= P_volume.x) t = min(t, (P_volume.x - o.x) / d.x);
    if (d.x > 0.0 && o.x <= P_volume.y) t = min(t, (P_volume.y - o.x) / d.x);
    return max(t, 0.0);
}

/* Where a ray from o enters the room between the floor (y = 0) and the
 * ceiling slab: 0 if it starts there, else where it leaves the slab it
 * starts in (1e9: never). A slab's copies overlap, so this is decided from
 * y, not from the surfaces the ray meets. */
float roomEnter(vec3 o, vec3 d) {
    if (o.y >= 0.0 && o.y <= P_module.y) return 0.0;
    float t = o.y < 0.0 ? (d.y > 0.0 ? -o.y / d.y : 1e9) : (d.y < 0.0 ? (P_module.y - o.y) / d.y : 1e9);
    return t;
}

/* A partition pane crossed at cosine c: what it reflects and transmits. */
float glassReflect(float c) { float R0 = P_glass.x; return R0 + (1.0 - R0) * pow(1.0 - c, 5.0); }

uint packAlbedo(vec3 a) { return packUnorm4x8(vec4(clamp(a, 0.0, 1.0), 0.0)); }
uint packNormal(vec3 n) { return packUnorm4x8(vec4(n * 0.5 + 0.5, 0.0)); }
