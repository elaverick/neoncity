#ifndef TERRAIN_CAVE_FIELD_H
#define TERRAIN_CAVE_FIELD_H

/* The caves' field's shape (terrain_cave.c, "The field"), shared by C
 * (src/terrain/terrain_internal.h) and GLSL (shaders/terrain/cave_hole.glsl,
 * which evaluates the field at the ground round the mouths to cut their
 * holes): #defines only, in syntax both accept. */
#define TERRAIN_CAVE_FLOOR      0.55     /* a passage's floor, radii under its axis */
#define TERRAIN_CAVE_ROOM_FLOOR 0.45     /* a chamber's, of its half-height */
#define TERRAIN_CAVE_ROUGH_M    1.6      /* the walls' noise, at most (m) */
/* A piece's own noise: its radius (a chamber's least semi-axis) less
 * _CLEAR_M, held between _LEAST_M and TERRAIN_CAVE_ROUGH_M, so the narrowest
 * passages (TERRAIN_CAVE_R_MIN, 1.5 m) keep room to walk through: a body
 * 0.32 m round, 1.8 m tall. */
#define TERRAIN_CAVE_ROUGH_CLEAR_M 1.0
#define TERRAIN_CAVE_ROUGH_LEAST_M 0.5
#define TERRAIN_CAVE_BLEND_M    3.0      /* the passages' smooth union: its bulge is a quarter of this */
#define TERRAIN_CAVE_FLOOR_K    1.0      /* the floor's soft clamp */
#define TERRAIN_CAVE_FLOOR_FADE_M  2.0   /* the walls' noise fades out over their lowest this */
#define TERRAIN_CAVE_FLOOR_REACH_M (TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M + 0.5)   /* a floor's weight reaches this past its piece's air */
#define TERRAIN_CAVE_FLOOR_NONE_M  2.0   /* under no floor: rock this far down */
#define TERRAIN_CAVE_NEAR_M     (TERRAIN_CAVE_BLEND_M + 2.0 * TERRAIN_CAVE_ROUGH_M)   /* a piece is near if its wall is within this of the nearest's */
#define TERRAIN_CAVE_NOISE_A_M  7.0      /* the walls' noise's two octaves (m) */
#define TERRAIN_CAVE_NOISE_B_M  2.5

/* The mouths' holes (src/render/cave_gpu.c, CaveHolesGPU): at most this many
 * at once (four systems of two mouths), and the pieces of the field near
 * each one (terrain_cave_hole). */
#define TERRAIN_CAVE_HOLES      8
#define TERRAIN_CAVE_HOLE_SEGS  32       /* (18 at most in the procedural world's tests, 8 on the real Earth's) */
#define TERRAIN_CAVE_HOLE_PIECES (TERRAIN_CAVE_HOLE_SEGS + 2)   /* and every chamber (TERRAIN_CAVE_ROOMS) */
#define TERRAIN_CAVE_HOLE_GRID  11       /* a hole's box's plan cut into this many cells a side, marked in 128 bits */
/* The meshes stand clear of the ground (terrain_cave_mesh.c): a vertex's
 * height over the ground is kept where it is within this under it at most
 * (deeper ones hold TERRAIN_CAVE_DEEP, cave.frag's gate), and a triangle
 * whose every corner stands more than _CLIP_DROP_M over it is left out. */
#define TERRAIN_CAVE_CLIP_GATE_M 6.0
#define TERRAIN_CAVE_CLIP_DROP_M 8.0
#define TERRAIN_CAVE_DEEP       (-1.0e4)

#endif
