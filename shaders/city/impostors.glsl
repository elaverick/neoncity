// Part of shaders/city.frag, which includes the parts in order: baked interior impostors behind office glass.

/* ======================================================================
 * Baked interior impostors (docs/interior-impostors.md)
 *
 * A share of the office floors behind full-height glass show a module of
 * open-plan floor baked offline (tools/interior_bake) in place of a box room
 * per window: its views, each a stack of depth layers, marched along the view
 * ray (city/impostor.glsl). The modules are loaded by src/render/interiors.c
 * from assets/interiors/; with none loaded, or the share at 0 (--impostors,
 * F8), every floor keeps interior mapping (rooms.glsl).
 * ==================================================================== */

#define INTERIOR_MAX_MODULES 8   // src/render/interior_def.h
#define INTERIOR_CHANNELS    4   // lint, albedo, depth, flags
#define INTERIOR_MAX_BAYS    8

struct ImpModule {   // InteriorGPU (src/render/interiors.h)
    vec4  grid;      // the texel grid's origin x, y (m); texels a metre; 1 if it wraps in x
    ivec4 dim;       // texels across, up; K; the module type (0 through, 1 core, 2 corner)
    ivec4 views;     // the view grid's nodes across, up; its kind (IMP_GRID_*); 1 if the last slot holds the last surface
    vec4  range;     // ImpViews.range
    vec4  size;      // W, H, D; the ceiling (m)
    vec4  march;     // steps at most, texels a step, bisections, a guessing view's weight
    vec4  bays;      // bays, their width (m), the bake scale, an off-grid view's weight
    vec4  bayMean[INTERIOR_MAX_BAYS];   // rgb each bay's mean radiance through the glass, before the bake scale
};
layout(set = 0, binding = 11) uniform Interiors {
    vec4      info;  // x modules loaded, y the share of eligible floors drawn with them
    ImpModule impMod[INTERIOR_MAX_MODULES];
} interiors;
// Each module's channels, module * INTERIOR_CHANNELS + channel; the module is chosen per floor.
layout(set = 0, binding = 12) uniform sampler2DArray impTex[INTERIOR_MAX_MODULES * INTERIOR_CHANNELS];

int gImpTex;   // the marched module's first image
int gImpK;     // its layers per view

vec2 impFetchDepth(int view, int layer, ivec2 t) { return texelFetch(impTex[nonuniformEXT(gImpTex + 2)], ivec3(t, view * gImpK + layer), 0).rg; }
bool impFetchFull(int view, ivec2 t) { return texelFetch(impTex[nonuniformEXT(gImpTex + 3)], ivec3(t, view), 0).r > 0.5; }
vec3 impFetchColor(int view, int layer, ivec2 t) { return texelFetch(impTex[nonuniformEXT(gImpTex)], ivec3(t, view * gImpK + layer), 0).rgb; }

#include "city/impostor.glsl"

/* The module a floor shows, or -1: a share of the floors (the same on every
 * face), each a module by its hash. mirror: this face shows it flipped in x. */
int impostorModule(uint seed, int section, int floorIdx, int faceDir, out bool mirror) {
    mirror = false;
    int n = int(interiors.info.x);
    if (n <= 0) return -1;
    uint h = hash3i(seed ^ 0x1A7Eu, section, floorIdx, 0);
    if (hfloat(h) >= interiors.info.y) return -1;
    mirror = hfloat(hash3i(seed ^ 0x1A7Fu, section, floorIdx, faceDir)) < 0.5;
    return min(int(hf(h, 1u) * float(n)), n - 1);
}

/* How a face's floor maps onto module m: x scaled so the module spans whole
 * bays of the face (bayW apart), y so a floor matches the module's floor to
 * floor. */
vec2 impostorStretch(int m, float bayW, float floorH) {
    float W = interiors.impMod[m].size.x;
    return vec2(W / (max(1.0, floor(W / bayW + 0.5)) * bayW), interiors.impMod[m].size.y / floorH);
}

/* The mean radiance through the glass of the module's bay at x (the mid LOD). */
vec3 impostorBayMean(int m, float x) {
    ImpModule M = interiors.impMod[m];
    float W = M.size.x;
    int b = clamp(int((x - W * floor(x / W)) / M.bays.y), 0, int(M.bays.x) - 1);
    return M.bayMean[b].rgb * M.bays.z;
}

/* What the eye sees through the glass of module m, along d (the module's
 * frame: x along the wall, y up, z into the building), entering at q on the
 * glass plane (metres). The 3 views round d are each marched and blended by
 * weight, less for a view that guessed or looked off its grid; a ray that
 * leaves by the far glass of a through module sees the view out (throughView,
 * from worldDir and the window's height eyeY). Below the floor or above the
 * ceiling the glass covers the slab's edge, slabC. */
vec3 impostorTrace(int m, vec2 q, vec3 d, vec3 worldDir, float eyeY, vec3 slabC) {
    ImpModule M = interiors.impMod[m];
    if (q.y < 0.0 || q.y > M.size.w) return slabC;
    gImpTex = m * INTERIOR_CHANNELS;
    gImpK = M.dim.z;
    ImpViews v;
    v.n = M.views.xy; v.kind = M.views.z; v.range = M.range;
    ImpGrid g;
    g.origin = M.grid.xy; g.tpm = M.grid.z; g.dim = M.dim.xy; g.wrapX = M.grid.w > 0.5;
    g.D = M.size.z; g.K = M.dim.z; g.lastSlot = M.views.w != 0;
    d.z = max(d.z, 1e-3);
    vec2 sigma = d.xy / d.z;

    ivec3 views;
    vec3 w;
    impPickViews(v, d, views, w);
    vec3 sum = vec3(0.0);
    float wsum = 0.0, wOut = 0.0;
    for (int i = 0; i < 3; ++i) {
        if (w[i] <= 1e-4) continue;
        vec3 dk = impViewDirIndex(v, views[i]);
        ImpHit h = impMarch(views[i], g, q, sigma - dk.xy / dk.z, g.D, int(M.march.x), M.march.y, int(M.march.z), false);
        float wt = w[i] * (h.unknown ? M.march.w : 1.0) * (h.outside ? M.bays.w : 1.0);
        if (h.layer >= 0) sum += impFetchColor(views[i], h.layer, h.texel) * (M.bays.z * wt);
        else wOut += wt;
        wsum += wt;
    }
    // past the last layer: the far glass of a through module, else the core wall's dark
    if (wOut > 0.0) sum += (M.dim.w == 0 ? throughView(worldDir, eyeY) : vec3(0.004)) * wOut;
    return sum / max(wsum, 1e-6);
}
