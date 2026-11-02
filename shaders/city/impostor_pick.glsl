// Shared by shaders/city/impostors.glsl and shaders/impostor_demand.frag: the interior modules' table, and which module a floor shows.

#define INTERIOR_MAX_MODULES 16  // src/render/interior_def.h
#define INTERIOR_TABLE_SIZE  20  // ... and a place for each near tier resident
#define INTERIOR_CHANNELS    5   // lint, albedo, depth, flags, ext
#define INTERIOR_MAX_BAYS    8

struct ImpModule {   // InteriorGPU (src/render/interiors.h)
    vec4  grid;      // the texel grid's origin x, y (m); texels a metre; 1 if it wraps in x
    ivec4 dim;       // texels across, up; K; the module type (0 through, 1 core, 2 corner)
    ivec4 views;     // the view grid's nodes across, up; its kind (IMP_GRID_*); 1 if the last slot holds the last surface
    vec4  range;     // ImpViews.range
    vec4  size;      // W, H, D; the ceiling (m)
    vec4  march;     // steps at most, texels a step, bisections, a guessing view's weight
    vec4  bays;      // bays, their width (m), the bake scale, an off-grid view's weight
    vec4  bayMean[INTERIOR_MAX_BAYS];   // rgb each bay's mean radiance through the glass, before the bake scale; [0].a what ext's 1 means
    vec4  bayExt[INTERIOR_MAX_BAYS];    // each bay's mean under the exterior bases: street, neon, sky, screens
    vec4  tier;      // x this module's near tier's place in the table (-1: not resident), y how far it has faded in
};
layout(set = 0, binding = 11) uniform Interiors {
    vec4      info;  // x modules loaded, y the share of eligible floors drawn with them, z how near (m) colour is filtered, w how near (m) the near tier is whole
    uvec4     pick[16];    // by the plate's depth in whole metres (4 to a vec4): a bit per module that suits it
    uvec4     states;      // the modules baked in each light state, a bit each: after hours, late shift, cleaners, screens
    vec4      windows;     // real windows (impostorSeesOut): x how far (m) they reach, 0 for none; y where they start to fade
    ImpModule impMod[INTERIOR_TABLE_SIZE];
} interiors;

/* The module a floor shows behind a face with this plate (half metres), or -1. A share of the floors
 * are open plan, the same on every face. Each takes a module that suits the plate behind the face
 * (pick), in the floor's light state where one does. */
int impostorModule(uint seed, int section, int floorIdx, uint plate) {
    if (int(interiors.info.x) <= 0) return -1;
    uint h = hash3i(seed ^ 0x1A7Eu, section, floorIdx, 0);
    if (hfloat(h) >= interiors.info.y) return -1;
    int metres = clamp(int(plate >> 1u), 0, 63);
    uint mask = interiors.pick[metres >> 2][metres & 3];
    if (mask == 0u) return -1;
    // the floor's light state, if a module for this plate has it: mostly after hours
    float s = hf(h, 2u);
    uint inState = mask & interiors.states[s < 0.55 ? 0 : s < 0.75 ? 1 : s < 0.87 ? 2 : 3];
    if (inState != 0u) mask = inState;
    for (int k = int(hf(h, 1u) * float(bitCount(mask))); k > 0; --k) mask &= mask - 1u;
    return findLSB(mask);
}

/* Can a ray through this point of a facade leave its open-plan floor by the far glass or the building's
 * side before it meets a slab? If so the glass is a real window (docs/interior-impostors.md, section 21):
 * the prepass leaves it out, and the scene pass covers only some of its samples, so the city behind shows
 * through. The prepass and shadeFacade must agree, or a covered sample has nothing behind it: this is
 * their one test. The scene asks with a margin (m) that the prepass doesn't, so it never goes further.
 * Whatever stands in the way on the floor is found later, by the march: then the sample is simply covered.
 * rel: the eye's ray to here. u: metres along the face. vAbs: height over the building's base. */
bool impostorSeesOut(uint seed, uvec4 prm, uint aux, uint pack, vec3 rel, vec3 n, float u, float vAbs, float margin) {
    uint plate = pack >> 24, groundMode = (prm.z >> 1) & 3u;
    int type = int((prm.z >> 3) & 3u);
    float dist = length(rel);
    if (plate == 0u || dist >= interiors.windows.x || (type != 1 && type != 2)) return false;
    float floorH = facadeFloorH(seed), groundH = facadeGroundH(seed) * (groundMode == 2u ? 2.0 : 1.0);
    float relV = vAbs - SIDEWALK_HEIGHT - groundH, floorIdx = floor(relV / floorH), fv = relV / floorH - floorIdx;
    if (floorIdx < 0.0) return false;
    uint plantCode = aux >> 28;
    if (plantCode > 0u && floorIdx > 0.0 && int(floorIdx) % (6 + 3 * int(plantCode)) == 5 + 3 * int(plantCode)) return false;
    // inside this bay's window (as shadeFacade lays them out)
    uint sh = styleHash(seed);
    float bayW = (prm.z & 64u) != 0u ? 1.6 + 0.8 * hf(sh, 4u) : 2.4 + 2.6 * hf(sh, 4u);
    float faceW = float(pack & 4095u) / 16.0;
    bayW = faceW / max(1.0, floor(faceW / bayW + 0.5));
    float winW = type == 1 ? 0.94 : 1.0, winH = type == 1 ? 0.86 : mix(0.4, 0.55, hf(sh, 9u));
    float fu = fract(u / bayW), sill = (1.0 - winH) * 0.42;
    if (abs(fu - 0.5) > winW * 0.5 || fv < sill || fv > sill + winH) return false;
    int m = impostorModule(seed, int(prm.y), int(floorIdx), plate);
    if (m < 0) return false;
    // where a ray going this way leaves, in the face's metres, and what height it's at by then
    vec3 T = vec3(n.z, 0.0, -n.x), w = rel / max(dist, 1e-4);
    vec3 d = vec3(dot(w, T), w.y, max(-dot(w, n), 1e-3));
    uint corners = (aux >> 24) & 15u;
    float sideX = d.x > 0.0 ? ((corners & 2u) != 0u ? faceW - u : 1e6) : ((corners & 1u) != 0u ? u : 1e6);
    float zSide = sideX / max(abs(d.x), 1e-5) * d.z;
    ImpModule M = interiors.impMod[m];
    bool through = M.dim.w == 0;
    float depth = through ? float(plate) * 0.5 : M.size.z;
    if (!through && zSide >= depth) return false;            // a core wall closes this way
    float y = fv * floorH + d.y / d.z * min(depth, zSide);
    float ceilH = M.size.w * floorH / M.size.y;
    return y > margin && y < ceilH - margin && fv * floorH < ceilH - margin;
}
