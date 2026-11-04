#ifndef GRASS_DEBUG_GLSL
#define GRASS_DEBUG_GLSL
#include "grass/rules.glsl"

/* --grass-debug (spec 11.1): the grass's rules painted on the land, unlit,
 * before any blade exists (terrain.frag). FrameUBO.grass.z picks the view:
 *   suit     the suitability: dark grey where none, red (little) to green (all);
 *   density  the effective blades per m^2 (grassExpected), dark blue (none)
 *            through green to yellow (GRASS_DEBUG_DENSE and more), with the
 *            kept roots as dots near the camera, their tufts' size;
 *   species  the species' shares (meadow green, reeds cyan, lawn yellow,
 *            rough olive), dimmed, under each kept root's dot in its colour;
 *   clump    the clumps of the species with the most of the ground: a colour
 *            each, darker towards their borders, bare spots dark red; dim
 *            where nothing grows.
 *   lod      the bucket a meadow root of typical height (GRASS_DEBUG_TYPICAL)
 *            would go in at the pixel's distance: full red, mid yellow, clump
 *            blue, none grey (the blades take their own bucket's colour);
 *   cull     each patch's state in the grass's prepare pass this frame
 *            (GRASS_PATCH_*): out of range or too small dark grey, out of
 *            view dark blue, bare dark red, no root kept olive, drawn green;
 *            the patches' outlines drawn.
 * The ground's site is the pixel's, also for the roots near it: close enough
 * for a picture. */

// The prepare pass's patch states (src/render/grass_gpu.c), the square round the camera's patch.
layout(std430, set = 0, binding = 10) readonly buffer GrassPatches { uint grassPatchState[]; };

const float GRASS_DEBUG_DENSE = 250.0;
const float GRASS_DEBUG_TYPICAL = 0.5;
const vec3  GRASS_DEBUG_COLOUR[GRASS_SPECIES] = vec3[](vec3(0.10, 0.55, 0.08), vec3(0.05, 0.45, 0.55), vec3(0.60, 0.65, 0.10), vec3(0.35, 0.32, 0.08));

// The grass patch and the point in it under camera-relative x, z: the camera's
// patch and place in it (FrameUBO.grassPatch, grass.xy, from doubles) plus rel.
void grassWhere(vec2 rel, out ivec2 gpatch, out vec2 local) {
    vec2 q = frame.grass.xy + rel;
    vec2 step = floor(q / float(GRASS_PATCH_M));
    gpatch = frame.grassPatch.xy + ivec2(step);
    local = q - step * float(GRASS_PATCH_M);
}

vec3 grassHue(uint h) {
    vec3 c = vec3(hfloat(h), hfloat(hash_u32(h)), hfloat(hash_u32(h + 7u)));
    return 0.08 + 0.5 * c;
}

// The kept roots of species k round the point: how much of the pixel (px m
// across) their dots cover, the dots GRASS_TUFT_R (at least a pixel) across.
float grassDebugRoots(int k, GrassSite s, ivec2 gpatch, vec2 local, float px) {
    int n = GRASS_GRID[k];
    float cs = float(GRASS_PATCH_M) / float(n);
    if (px > 0.5 * cs) return 0.0;   // too far to tell one root from the next
    ivec2 cell = ivec2(floor(local / cs));
    float cover = 0.0, r = max(GRASS_TUFT_R[k], 0.5 * px);
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            ivec2 c = cell + ivec2(di, dj), p = gpatch;
            // a cell past the patch's side is the next patch's
            ivec2 wrap = ivec2(floor(vec2(c) / float(n)));
            p += wrap;
            c -= wrap * n;
            GrassCandidate cand = grassCandidate(k, p, c);
            GrassRoot root;
            if (!grassKeep(cand, s, root)) continue;
            vec2 at = cand.p + vec2(wrap) * float(GRASS_PATCH_M);
            float d = length(local - at);
            cover = max(cover, clamp((r - d) / px + 0.5, 0.0, 1.0));
        }
    return cover * (1.0 - smoothstep(0.25 * cs, 0.5 * cs, px));
}

vec3 grassDebug(int mode, vec3 rel, vec3 n, float urban, Cover cover, bool onRoad, RoadHit road, float px) {
    GrassSite s;
    s.grass = cover.a.z; s.mud = cover.b.x; s.snow = cover.b.y; s.wet = cover.b.z;
    s.ny = n.y; s.urban = urban;
    s.road = !onRoad ? GRASS_ROAD_FAR : roadPaved(road, px) ? -1.0 : abs(road.off) - road.hw;
    s.maintained = 0.0; s.city = 0;
    float suit = grassSuitability(s);
    float w[GRASS_SPECIES];
    grassSpecies(s, w);
    ivec2 gpatch;
    vec2 local;
    grassWhere(rel.xz, gpatch, local);

    if (mode == GRASS_DEBUG_SUIT)
        return suit <= 0.0 ? vec3(0.03) : mix(vec3(0.35, 0.03, 0.02), vec3(0.08, 0.50, 0.06), suit);

    if (mode == GRASS_DEBUG_LOD) {   // f_y: pixels per unit of height over distance (rayBasis' up is tan(fovy / 2))
        float fy = 0.5 * frame.screen.y / length(frame.rayBasis[1].xyz);
        float hpx = GRASS_DEBUG_TYPICAL * fy / max(length(rel), 1e-3);
        vec3 c = length(rel.xz) > float(GRASS_RADIUS) || hpx < float(GRASS_LOD_CLUMP_PX) ? vec3(0.03)
               : hpx >= float(GRASS_LOD_FULL_PX) ? vec3(0.30, 0.04, 0.03) : hpx >= float(GRASS_LOD_MID_PX) ? vec3(0.30, 0.25, 0.02) : vec3(0.04, 0.10, 0.30);
        return c * (suit > 0.0 ? 1.0 : 0.3);
    }

    if (mode == GRASS_DEBUG_CULL) {
        ivec2 g = gpatch - frame.grassPatch.xy + ivec2(GRASS_PATCH_GRID / 2);
        uint st = g.x < 0 || g.y < 0 || g.x >= GRASS_PATCH_GRID || g.y >= GRASS_PATCH_GRID ? GRASS_PATCH_OUT
                : grassPatchState[g.y * GRASS_PATCH_GRID + g.x];
        vec3 c = st == GRASS_PATCH_HIDDEN ? vec3(0.02, 0.03, 0.18) : st == GRASS_PATCH_BARE ? vec3(0.20, 0.02, 0.02)
               : st == GRASS_PATCH_NONE ? vec3(0.18, 0.18, 0.03) : st == GRASS_PATCH_DRAWN ? vec3(0.04, 0.30, 0.04) : vec3(0.03);
        vec2 e = min(local, float(GRASS_PATCH_M) - local);
        return mix(c, vec3(0.5), clamp(1.0 - min(e.x, e.y) / max(px, 1e-3), 0.0, 1.0));
    }

    if (mode == GRASS_DEBUG_DENSITY) {
        float d = grassExpected(s) / GRASS_DEBUG_DENSE;
        vec3 c = d <= 0.0 ? vec3(0.01, 0.01, 0.05)
               : d < 0.5 ? mix(vec3(0.02, 0.04, 0.25), vec3(0.05, 0.35, 0.10), d / 0.5)
                         : mix(vec3(0.05, 0.35, 0.10), vec3(0.60, 0.55, 0.05), min((d - 0.5) / 0.5, 1.0));
        float dots = 0.0;
        for (int k = 0; k < GRASS_SPECIES; ++k)
            if (w[k] > 0.0 && suit > 0.0) dots = max(dots, grassDebugRoots(k, s, gpatch, local, px));
        return mix(c, vec3(0.8), dots);
    }

    if (mode == GRASS_DEBUG_SPECIES) {
        vec3 c = vec3(0.0);
        for (int k = 0; k < GRASS_SPECIES; ++k) c += w[k] * GRASS_DEBUG_COLOUR[k];
        c *= 0.25 + 0.25 * suit;
        for (int k = 0; k < GRASS_SPECIES; ++k)
            if (w[k] > 0.0 && suit > 0.0) c = mix(c, 1.4 * GRASS_DEBUG_COLOUR[k], grassDebugRoots(k, s, gpatch, local, px));
        return c;
    }

    // GRASS_DEBUG_CLUMP
    int k = w[GRASS_REEDS] > w[GRASS_MEADOW] ? GRASS_REEDS : GRASS_MEADOW;
    GrassClump cl = grassClump(k, gpatch, local);
    vec3 c = cl.bare ? vec3(0.25, 0.02, 0.02) : grassHue(cl.id);
    c *= 1.0 - 0.8 * cl.edge;
    return c * (suit > 0.0 ? 1.0 : 0.15);
}

#endif
