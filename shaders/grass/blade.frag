#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#include "terrain/ground.glsl"
#include "grass_rules.h"

/* GR2's stand-in blades (blade.vert). Shaded as a placeholder until GR3 and
 * GR7: the land_grass hue (assets/materials/land_grass.json's tint), darker
 * at the root, lit by the light the land takes (groundLightIn) and hazed as
 * it is. --grass-debug lod colours them by bucket (full red, mid yellow,
 * clump blue), cull by patch. Two-sided. Nothing for the reflections. */

layout(location = 0) in vec3 vRel;
layout(location = 1) in float vT;
layout(location = 2) flat in vec3 vRoot;
layout(location = 3) flat in ivec2 vKind;
layout(location = 4) flat in float vTint;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRefl;

const vec3 GRASS_LOD_COLOUR[GRASS_BUCKETS] = vec3[](vec3(0.85, 0.10, 0.06), vec3(0.85, 0.70, 0.05), vec3(0.10, 0.35, 0.95));

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
        vec3 albedo = vec3(0.050, 0.066, 0.034) * (1.0 + 0.5 * vTint) * mix(0.35, 1.3, vT);
        if (vKind.x == GRASS_REEDS) albedo *= vec3(1.25, 1.1, 0.8);
        col = groundHazed(albedo * groundLight(vRel, vec3(0.0, 1.0, 0.0)), vRel, 0.0);
    }
    outColor = vec4(col, 1.0);
    outRefl = vec4(0.0);
}
