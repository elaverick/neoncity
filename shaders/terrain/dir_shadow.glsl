#ifndef DIR_SHADOW_GLSL
#define DIR_SHADOW_GLSL

/* The directional light's shadows on the land (M11c). The light is the sun
 * by day and the moon by night (frame.dirLight: its direction, set in
 * src/render/renderer.c, DN4). dirShadow says how much of it reaches a point
 * on the ground:
 *
 * - Near the camera, one hard ray through the shadows' TLAS (the clipmap's
 *   near levels, RT_MASK_TERRAIN: src/render/terrain_rt.c) to where the
 *   coarsest level in it ends, so what casts the shadow there is the surface
 *   drawn. Only with DIR_SHADOW_TRACE (the includer has the TLAS, `tlas`) and
 *   DIR_SHADOW_TLAS (the ray-traced shadows are on and the terrain is in it).
 * - From there on (or from the point itself, without the ray), a march
 *   through the clipmap's heights towards the light, out to the coarsest
 *   window. Each step reads the level whose spacing is about the step, and
 *   the steps grow with the distance (DIR_GROW), so a far ridge is sampled as
 *   coarsely as its shadow's edge is soft: the light's disc (frame.dirMix.z)
 *   widens the edge with the distance to what casts it, and the march gives
 *   that soft edge from the least clearance it found. The ground falls away
 *   under the ray with the planet's curve (planetDrop). Where the ray passes
 *   over the highest ground of the storage block it is over (and the eight
 *   round it: frame.terrainBlocks), penumbra and all, it goes on to the
 *   block's far edge in one step. It stops once the ray is above the highest
 *   ground drawn (frame.terrainInfo.y).
 *
 * The heights are each level's own samples, not blended into the parent as
 * terrain.vert blends them near the window's edge, and interpolated across
 * the triangles as it draws them; DIR_BIAS covers the difference. Beyond the
 * clipmap (the planet's patches) nothing casts or takes a shadow: the haze is
 * deep there, and the shadow fades out before the coarsest window's edge.
 *
 * In the caves (DIR_SHADOW_CAVES: cave_rt.frag) the ray also traces the
 * caves' bricks (RT_MASK_CAVE: src/render/cave_gpu.c), and takes the hits on
 * the instances that aren't opaque one by one: the terrain's levels holding a
 * mouth's hole, the bricks at a mouth or in a finer ring's overlap. The
 * includer's dirSeeThrough(custom, h) says which of those to pass (the hole's
 * cut ground, the brick over the drawn ground or inside the finer ring's box),
 * h the hit in the TLAS's frame, custom the instance's custom index. There a
 * point the ray can't be traced from takes the includer's own (dirShadowOr):
 * the march from under the ground would find the rock over every cave.
 *
 * The includer declares the clipmap's samples as `samples` (terrain.vert's
 * buffer: TerrainSample's seven words per slot), or names them DIR_SAMPLES,
 * and includes common.glsl. */

#ifndef DIR_SAMPLES
#define DIR_SAMPLES samples
#endif

const uint  DIR_SHADOW_MARCH = 1u;   // frame.dirLight.w's bits: the march on
const uint  DIR_SHADOW_TLAS  = 2u;   // ...and the ray near the camera
const int   DIR_STEPS = 72;          // the march's most steps
const float DIR_GROW = 0.1;          // its step, a share of the distance marched
const float DIR_BIAS = 0.25;         // the ground's height let off, in the level's spacings

// Level l's sample at storage slot s: its height (world Y).
float dirSampleH(int l, ivec2 s) {
    const int N = TERRAIN_CLIP_N;
    return uintBitsToFloat(DIR_SAMPLES[(((l * N) + (s.y & (N - 1))) * N + (s.x & (N - 1))) * 7]);
}

// Where camera-relative x, z lies in level l's window, in its samples from the corner.
vec2 dirWindow(int l, vec2 xz) {
    TerrainLevel L = frame.terrainLevel[l];
    return (xz - L.a.xy - L.b.xy) / L.b.z;
}

bool dirInside(vec2 q) { return all(greaterThanEqual(q, vec2(0.0))) && all(lessThanEqual(q, vec2(float(TERRAIN_CLIP_W - 1)))); }

// Level l's ground at q (dirWindow), across the triangle q lies in, split
// along the (i, j)-(i + 1, j + 1) diagonal as the terrain is drawn.
float dirHeight(int l, vec2 q) {
    TerrainLevel L = frame.terrainLevel[l];
    ivec2 c = min(ivec2(floor(q)), ivec2(TERRAIN_CLIP_W - 2));
    vec2 f = q - vec2(c);
    ivec2 s = L.c.xy + c;
    int lv = L.d.x;
    float h0 = dirSampleH(lv, s), h2 = dirSampleH(lv, s + 1);
    if (f.x >= f.y) return (1.0 - f.x) * h0 + f.y * h2 + (f.x - f.y) * dirSampleH(lv, s + ivec2(1, 0));
    return (1.0 - f.y) * h0 + f.x * h2 + (f.y - f.x) * dirSampleH(lv, s + ivec2(0, 1));
}

// The finest drawn level from l0 on whose window holds camera-relative x, z;
// TERRAIN_CLIP_LEVELS if none does.
int dirLevel(int l0, vec2 xz) {
    for (int l = l0; l < TERRAIN_CLIP_LEVELS; ++l)
        if (dirInside(dirWindow(l, xz))) return l;
    return TERRAIN_CLIP_LEVELS;
}

// The march from camera-relative p (x, z not bent; y world Y) towards L,
// from t0 metres out along the light's level direction; s0 the spacing of
// the ground at p, the finest step worth taking.
float dirMarch(vec3 p, vec3 L, float t0, float s0) {
    int finest = int(frame.terrainInfo.x);
    float hor = length(L.xz);
    if (finest >= TERRAIN_CLIP_LEVELS || hor < 1e-4) return 1.0;
    vec2 dh = L.xz / hor;
    float rise = L.y / hor, top = frame.terrainInfo.y;
    // the light's disc as a clearance: ground a height c under the ray, t out,
    // lies c hor^2 / t below it as the light's direction goes, so the disc
    // (its angular radius frame.dirMix.z: the sun's or the moon's) shows past it from c = -soft t / 2 to + soft t / 2
    float soft = 2.0 * frame.dirMix.z / max(hor * hor, 1e-3);
    float vis = 1.0, t = t0;
    for (int i = 0; i < DIR_STEPS; ++i) {
        float y = p.y + t * rise + planetDrop(vec2(t, 0.0));
        if (y > top) break;
        vec2 xz = p.xz + dh * t;
        float step = max(s0, t * DIR_GROW);
        int need = clamp(int(log2(step / float(TERRAIN_CLIP_SPACING0))), finest, TERRAIN_CLIP_LEVELS - 1);
        int l = dirLevel(need, xz);
        if (l >= TERRAIN_CLIP_LEVELS) break;
        float s = frame.terrainLevel[l].b.z;
        vec2 q = dirWindow(l, xz);
        // the block the ray is over: if it passes over all of it, penumbra and all, on to its far edge
        ivec2 c = min(ivec2(floor(q)), ivec2(TERRAIN_CLIP_W - 2));
        ivec2 slot = (frame.terrainLevel[l].c.xy + c) & (TERRAIN_CLIP_N - 1);
        ivec2 b = slot / TERRAIN_CLIP_BLOCK;
        int bi = (l * TERRAIN_CLIP_NB + b.y) * TERRAIN_CLIP_NB + b.x;
        if (y - frame.terrainBlocks[bi >> 2][bi & 3] > 0.5 * soft * t) {
            vec2 lo = vec2(c - (slot & (TERRAIN_CLIP_BLOCK - 1))), far = lo + float(TERRAIN_CLIP_BLOCK);
            vec2 ex = vec2(abs(dh.x) > 1e-6 ? ((dh.x > 0.0 ? far.x : lo.x) - q.x) / dh.x : 1e9,
                           abs(dh.y) > 1e-6 ? ((dh.y > 0.0 ? far.y : lo.y) - q.y) / dh.y : 1e9);
            t += max(min(ex.x, ex.y), 0.0) * s + 0.05 * s;
            continue;
        }
        float clear = y - dirHeight(l, q) + DIR_BIAS * s;
        vis = min(vis, clamp(0.5 + clear / (soft * t), 0.0, 1.0));
        if (vis <= 0.0) break;
        t += max(step, s);
    }
    return vis;
}

#ifdef DIR_SHADOW_TRACE
// The ray near the camera from p (dirShadow's), facing n, lp the finest level
// holding it, to where the TLAS's coarsest level's window ends (out: the level
// metres there): 0 if it strikes something, 1 if not, -1 if it can't be
// traced from p (the ray-traced shadows or the terrain's levels out of it,
// or p beyond them).
float dirRay(vec3 p, vec3 n, vec3 L, float dist, int lp, out float out_) {
    out_ = 0.0;
    int reach = int(frame.terrainInfo.z);
    if ((uint(frame.dirLight.w) & DIR_SHADOW_TLAS) == 0u || reach < 0 || lp > reach) return -1.0;
    TerrainLevel T = frame.terrainLevel[reach];
    vec2 lo = T.a.xy + T.b.xy, hi = lo + float(TERRAIN_CLIP_W - 1) * T.b.z;
    float hor = length(L.xz);
    vec2 dh = hor > 1e-4 ? L.xz / hor : vec2(1.0, 0.0);
    vec2 ex = vec2(abs(dh.x) > 1e-6 ? ((dh.x > 0.0 ? hi.x : lo.x) - p.x) / dh.x : 1e9,
                   abs(dh.y) > 1e-6 ? ((dh.y > 0.0 ? hi.y : lo.y) - p.z) / dh.y : 1e9);
    out_ = max(min(ex.x, ex.y), 0.0);   // level metres to the window's edge
    vec3 o = vec3(p.x, p.y - frame.camPos.y, p.z) + n * (0.03 + dist * 4e-4);   // the TLAS's frame: the camera's x, z; y less the camera's
    float len = hor > 1e-4 ? out_ / hor : 1e5;
    rayQueryEXT q;
#ifdef DIR_SHADOW_CAVES
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT, RT_MASK_TERRAIN | RT_MASK_CAVE, o, 0.0, L, len);
    while (rayQueryProceedEXT(q)) {   // only the instances that aren't opaque come here
        if (rayQueryGetIntersectionTypeEXT(q, false) != gl_RayQueryCandidateIntersectionTriangleEXT) continue;
        vec3 h = o + L * rayQueryGetIntersectionTEXT(q, false);
        if (!dirSeeThrough(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(q, false)), h)) rayQueryConfirmIntersectionEXT(q);
    }
#else
    rayQueryInitializeEXT(q, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, RT_MASK_TERRAIN, o, 0.0, L, len);
    while (rayQueryProceedEXT(q)) {}
#endif
    return rayQueryGetIntersectionTypeEXT(q, true) != gl_RayQueryCommittedIntersectionNoneEXT ? 0.0 : 1.0;
}
#endif

// How much of the directional light (towards L) reaches the ground at
// camera-relative p (x, z not bent, y world Y: terrain.vert's vFlat), facing
// n, dist from the camera. Where the ray near the camera can't be traced from
// p, `untraced` if it is not negative; else (and it is) the march from p.
float dirShadowOr(vec3 p, vec3 n, vec3 L, float dist, float untraced) {
    float none = untraced >= 0.0 ? untraced : 1.0;
    uint flags = uint(frame.dirLight.w);
    if ((flags & DIR_SHADOW_MARCH) == 0u) return none;
    if (dot(n, L) <= 0.0) return 1.0;   // (facing away, it is dark anyway)
    int finest = int(frame.terrainInfo.x);
    if (finest >= TERRAIN_CLIP_LEVELS) return none;
    int lp = dirLevel(finest, p.xz);
    if (lp >= TERRAIN_CLIP_LEVELS) return none;
    float s0 = frame.terrainLevel[lp].b.z, t0 = 2.0 * s0;
    // out towards the coarsest window's edge, the shadow fades (beyond it the land takes none)
    const int TOP = TERRAIN_CLIP_LEVELS - 1;
    vec2 qt = dirWindow(TOP, p.xz) / float(TERRAIN_CLIP_W - 1) * 2.0 - 1.0;
    float fade = smoothstep(0.7, 0.9, max(abs(qt.x), abs(qt.y)));
    if (fade >= 1.0) return 1.0;
    bool traced = false;
#ifdef DIR_SHADOW_TRACE
    float out_, ray = dirRay(p, n, L, dist, lp, out_);
    if (ray == 0.0) return fade;
    if (ray > 0.0) { t0 = max(t0, out_); traced = true; }
#endif
    if (!traced && untraced >= 0.0) return untraced;
    return mix(dirMarch(p, L, t0, s0), 1.0, fade);
}
float dirShadow(vec3 p, vec3 n, vec3 L, float dist) { return dirShadowOr(p, n, L, dist, -1.0); }

#endif
