#ifndef TERRAIN_CLIP_H
#define TERRAIN_CLIP_H

/* The clipmap's shape (see terrain.h), shared by C (src/terrain/terrain.h)
 * and GLSL (shaders/terrain/terrain.vert, which lays out the grid and
 * mirrors terrain_clip_morph): #defines only, in syntax both accept. */
#define TERRAIN_CLIP_LEVELS   10
#define TERRAIN_CLIP_N        128                     /* storage side, slots (power of two) */
#define TERRAIN_CLIP_W        (TERRAIN_CLIP_N - 1)    /* window side, samples; W - 1 even, (W - 1) / 2 odd */
#define TERRAIN_CLIP_SPACING0 1.0                     /* level 0 sample spacing (m); level 9 spans 65 km */
#define TERRAIN_CLIP_MORPH    12                      /* width of the blend into the parent, samples */

#endif
