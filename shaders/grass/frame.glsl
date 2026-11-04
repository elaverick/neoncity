#ifndef GRASS_FRAME_GLSL
#define GRASS_FRAME_GLSL
#include "terrain_clip.h"
#include "grass_rules.h"

/* The grass pass's own uniforms (GrassFrameGPU, src/render/grass_gpu.h),
 * written by grass_gpu.c each frame: which clipmap levels the terrain draws
 * and how (each as terrain.vert's push constant, TerrainPush), the buckets'
 * room, the screen's scale. */

// One drawn level: terrain.vert's Level, in vec4s.
struct GrassLevel {
    vec4  a;   // xy anchor (the coarsest window's corner, camera-relative), zw morph (the same, relative to the update's camera)
    vec4  b;   // xy offset (this window's corner less the anchor), z spacing (m), w fade (the least morph)
    ivec4 c;   // xy the window's corner's storage slot, zw the parent's slot of the parent sample there
    ivec4 d;   // x level, y top (no parent)
};

layout(std140, set = 0, binding = 4) uniform GrassFrame {
    ivec4 info;      // x the finest level drawn (TERRAIN_CLIP_LEVELS: none), y patches a side (GRASS_PATCH_GRID), z GRASS_MODE_*, w unused
    vec4  view;      // x the radius (m), y f_y: pixels per unit of height over distance, zw unused
    uvec4 cap;       // each bucket's room (instances)
    uvec4 base;      // each bucket's first instance
    ivec4 seg;       // each bucket's segments (the most any species asks: grass_bucket_segments)
    GrassLevel lv[TERRAIN_CLIP_LEVELS];
} gf;

#endif
