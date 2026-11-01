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
