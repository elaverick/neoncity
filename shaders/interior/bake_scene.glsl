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
struct Inst { uint firstIndex, baseVertex, material, flags, firstEmitter, pad0, pad1, pad2; };   // firstEmitter: ~0u, none
layout(std430, set = 0, binding = 3) readonly buffer Insts { Inst insts[]; };
// albedo.a roughness; emission.a metallic; surface: a dielectric's F0 (0: no specular lobe), clearcoat, its roughness
struct Mat { vec4 albedo; vec4 emission; vec4 surface; };
layout(std430, set = 0, binding = 4) readonly buffer Mats { Mat mats[]; };
struct Light { vec4 pos; vec4 power; };   // a point emitting downwards with a cosine lobe; power: intensity straight down
layout(std430, set = 0, binding = 5) readonly buffer Lights { Light lights[]; };

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
    ivec4 P_flags;      // the hit's colour filtered (impHitColor), -, -, -
    ivec4 P_trace;      // the path tracer: this pass's first sample, its samples, the samples in all, most bounces
    ivec4 P_trace2;     // light (0: IM1's direct, 1: path traced), layers path traced (capture), emitters, seed
    vec4  P_trace3;     // the most an indirect sample may bring (0: no clamp), next-event estimation (1) or BSDF sampling alone (0), -, -
    ivec4 P_trace4;     // the first view of this pass (capture), whether paths leave by the glass planes (0: a sealed test box), -, -
};

/* Instance flags (bake.h: BF_*) and masks (gpu.c). */
#define BF_GLASS   1u
#define BF_OPEN    2u
#define BF_EMITTER 4u
#define MASK_CASTER  1u
#define MASK_GLASS   2u
#define MASK_EMITTER 4u

struct Hit { float t; vec3 n; uint inst; uint prim; bool front; };

vec3 bakeVertex(uint i) { return vec3(verts[3u * i], verts[3u * i + 1u], verts[3u * i + 2u]); }

/* The nearest surface of the instances in mask along o + t d with t in
 * (tmin, tmax). n is the world geometric normal, from the triangle's winding
 * (instances are never mirrored), pointing out of a solid. */
bool traceMasked(vec3 o, vec3 d, float tmin, float tmax, uint mask, out Hit h) {
    h.t = 0.0; h.n = vec3(0.0, 1.0, 0.0); h.inst = 0u; h.prim = 0u; h.front = true;
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
    vec3 n = cross(v1 - v0, v2 - v0);
    h.n = length(n) > 0.0 ? normalize(n) : vec3(0.0, 1.0, 0.0);
    h.front = dot(h.n, d) < 0.0;
    return true;
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
