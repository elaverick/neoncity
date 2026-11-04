#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/ground.glsl"
#include "grass_rules.h"

/* Grass blades (blade.vert). Coloured by the species' profile
 * (frame.grassSpecies: assets/grass/<name>.json): root to tip, and towards
 * the dry colour by the blade's tint, mostly its clump's (spec 10.1: colour
 * coherent over a clump, not per blade), times the land's large-scale
 * variation at the root (blade.vert: vVary) and darkened by the rain as the
 * land's grass is, so it agrees with the ground's field (grass/field.glsl)
 * where the blades give way to it. Lit, until GR7, by the light the land
 * takes at that point facing the ground's normal (groundLightIn), a little
 * more where the blade's face turns up to it, and hazed as the land is. --grass-debug lod colours them by bucket (full red, mid yellow, clump
 * blue), cull by patch. Two-sided. Nothing for the reflections. */

layout(location = 0) in vec3 vRel;
layout(location = 1) in float vT;
layout(location = 2) flat in vec3 vRoot;
layout(location = 3) flat in ivec2 vKind;
layout(location = 4) flat in float vTint;
layout(location = 5) in vec3 vN;
layout(location = 6) flat in vec3 vGround;
layout(location = 7) flat in vec3 vVary;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;

const vec3 GRASS_LOD_COLOUR[GRASS_BUCKETS] = vec3[](vec3(0.85, 0.10, 0.06), vec3(0.85, 0.70, 0.05), vec3(0.10, 0.35, 0.95));

float lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

void main() {
    int view = int(frame.grass.z + 0.5);
    vec3 col;
    if (view == GRASS_DEBUG_LOD) {
        col = GRASS_LOD_COLOUR[clamp(vKind.y, 0, GRASS_BUCKETS - 1)] * (0.4 + 0.6 * vT);
    } else if (view == GRASS_DEBUG_CULL) {
        vec2 q = frame.grass.xy + vRoot.xz;
        ivec2 gp = frame.grassPatch.xy + ivec2(floor(q / float(GRASS_PATCH_M)));
        uint h = hash2i(0x9A7C4u, gp.x, gp.y);
        col = (0.15 + 0.6 * vec3(hfloat(h), hfloat(hash_u32(h)), hfloat(hash_u32(h + 7u)))) * (0.4 + 0.6 * vT);
    } else {
        GrassSpecies sp = frame.grassSpecies[clamp(vKind.x, 0, GRASS_SPECIES - 1)];
        float up = pow(clamp(vT, 0.0, 1.0), 0.7);
        vec3 lush = mix(sp.root.rgb, sp.tip.rgb, up);
        // the dry colour is the tip's; down the blade it darkens as the lush one does
        float rootShare = lum(sp.root.rgb) / max(lum(sp.tip.rgb), 1e-4);
        vec3 dry = sp.dry.rgb * mix(rootShare, 1.0, up);
        float dryness = clamp(vTint + 0.5, 0.0, 1.0) * sp.dry.w;
        LandSky sky = landSky(vRel);
        vec3 albedo = mix(lush, dry, dryness) * (1.0 + 0.3 * vTint) * vVary * landDarken(2, sky.wet);
        vec3 N = normalize(vN);
        float face = 0.8 + 0.2 * max(dot(N, vGround), 0.0);
        col = groundHazed(albedo * face * groundLightIn(sky, vGround), vRel, 0.0);
    }
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
