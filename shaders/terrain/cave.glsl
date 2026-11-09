#ifndef CAVE_GLSL
#define CAVE_GLSL

/* The caves' frame data (src/render/cave_gpu.c: CaveFrameGPU), shared by
 * cave.vert and cave.frag. */

// CaveSysGPU: a system near the camera, drawn about its anchor (the camera's
// place in its chart, snapped)
struct CaveSys {
    vec4 anchor;   // camera-relative world x, z of the anchor; its height less the camera's (from doubles); its height
    vec4 J;        // the chart's map into the frame there: world x = J.x cx + J.y cz, world z = J.z cx + J.w cz
    vec4 chart;    // the anchor in the chart (x, h, z)
    vec4 box[4];   // the discard boxes, from the anchor: level 0's shrunk (min, max), then level 1's
};
// CaveDrawGPU: a brick
struct CaveDraw {
    vec4  off;     // its corner from the anchor (chart, m: whole cells), and a vertex's unit (m)
    ivec4 info;    // its system, its discard box (-1 none), its level, 0
};
layout(std430, set = 0, binding = 2) readonly buffer CaveFrame { CaveSys caveSys[4]; CaveDraw caveDraw[]; };

#ifdef CAVE_SHELTER
// The shelter map's (renderer.c: record_shelter): camera-relative to the map
layout(push_constant) uniform CaveShelter { mat4 topDown; } caveShelter;
#else
// The --cave-debug view (renderer_set_cave_debug): 0 off, 1 normals, 2 lit
// from the eye, 3 that tinted by level; and the camera: whether it is in a
// cave's air under the ground, and the sky it sees there (terrain_cave_sky)
layout(push_constant) uniform CaveView { int view; int camIn; float camSky; int traced; } caveView;   // traced: the caves are in the TLAS this frame (cave_gpu.c)
#endif

#endif
