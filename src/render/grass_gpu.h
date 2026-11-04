#pragma once
#include <stdint.h>
#include "terrain_clip.h"
#include "grass_rules.h"

/* The grass's GPU path's data as the shaders see it (shaders/grass/frame.glsl,
 * instance.glsl; the counters' words are GRASS_C_* in grass_rules.h). Shared
 * by src/render/grass_gpu.c and tests/grass_test.c. */

/* One clipmap level the terrain draws, as its TerrainPush (terrain.vert). */
typedef struct GrassLevelGPU {
    float   a[4];   /* anchor x, z; morph x, z */
    float   b[4];   /* offset x, z; spacing; fade */
    int32_t c[4];   /* slot x, z; parent slot x, z */
    int32_t d[4];   /* level, top, 0, 0 */
} GrassLevelGPU;

/* GrassFrame (std140). */
typedef struct GrassFrameGPU {
    int32_t       info[4];   /* the finest level drawn (TERRAIN_CLIP_LEVELS: none), patches a side, GRASS_MODE_*, 0 */
    float         view[4];   /* radius (m), f_y (pixels per unit of height over distance), 0, 0 */
    uint32_t      cap[4];    /* each bucket's room */
    uint32_t      base[4];   /* each bucket's first instance */
    GrassLevelGPU lv[TERRAIN_CLIP_LEVELS];
} GrassFrameGPU;
_Static_assert(sizeof(GrassLevelGPU) == 64 && sizeof(GrassFrameGPU) == 64 + 64 * TERRAIN_CLIP_LEVELS, "GrassFrame's std140 layout");

/* A blade instance (GrassInstance). */
typedef struct GrassInstanceGPU {
    float    p[3], height;
    uint32_t nrm, dir, shape, misc;
} GrassInstanceGPU;
_Static_assert(sizeof(GrassInstanceGPU) == 4 * GRASS_INSTANCE_WORDS, "a grass instance is GRASS_INSTANCE_WORDS words");

static const uint32_t GRASS_CAPS[GRASS_BUCKETS] = { GRASS_CAP_FULL, GRASS_CAP_MID, GRASS_CAP_CLUMP };
