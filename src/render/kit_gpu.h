#pragma once
#include "vkcore.h"
#include "mathlib.h"

/* The vendors' kit drawn instanced (roadmap NB2 in ~/.claude/plans/street-vendors.md).
 *
 * The module library: every module of the modelling kit (src/gen/pmodel.h) at
 * its three levels of detail, built once at start-up (on the worker threads),
 * baked (occlusion, wear), as the city's vertices (Vertex, MAT_KIT) with
 * 16-bit indices in two device-local buffers. A module's level is one indexed
 * draw of it.
 *
 * Each frame the chunk streamer hands over the near vendor layer's instances
 * (KitInst, src/city/city.h) of the chunks it draws: each is culled against
 * the view, given its level (the finest its distance allows, L0 to 8 m, L1 to
 * 30 m, L2 beyond, made coarser while that level would put more than
 * KIT_TRI_PX triangles on each pixel the module covers), shrunk away before its own fade
 * distance (a few metres for a rivet, KIT_DISTANCE for a stool), and written to
 * this frame's instance buffer grouped by module and level, one indirect
 * indexed draw a group. The draws go through the city's shading (city.frag,
 * the depth prepass and so the ray-traced shadows it receives): kit.vert reads
 * the instance as a second, per-instance vertex binding.
 *
 * Shadows they cast: a BLAS over each module's middle level (L1), and a TLAS
 * instance for each kit instance within KIT_RT_RANGE whose module is big enough
 * to matter (KIT_RT_RADIUS), whatever the view (as the chunks'). */

#define KIT_RT_RANGE   35.0f    /* m: kit instances this near cast ray-traced shadows */
#define KIT_RT_RADIUS  0.10f    /* m: ...if their module reaches this far (stools, pots, pans, gas, the fan: not jars, knobs, hooks) */

typedef struct KitGpu KitGpu;
struct Rt;
struct KitInst;

/* One frame's instance on the GPU (kit.vert, binding 1): the module's frame to camera-relative, rows. */
typedef struct KitInstGPU {
    float    row[3][4];
    uint32_t seed, chunk, part, base;   /* base: the float's bits (Vertex.base) */
} KitInstGPU;

typedef struct KitStats {
    int      modules, levels;
    int      libTris;             /* over every level */
    double   libMB;               /* vertices and indices */
    double   buildMs;             /* building and baking the library */
    int      offered, drawn[3];   /* this frame: instances handed over; drawn at each level */
    long     tris;                /* triangles drawn */
    int      draws;               /* indirect draws */
    int      rtInstances, blas;   /* TLAS instances added, module BLASes built */
} KitStats;

KitGpu  *kit_create(VkCore *vk, struct Rt *rt);
void     kit_destroy(KitGpu *k);
/* Once per frame before kit_add: uploads the library (the first frame) and builds the modules' BLASes
 * (as the frame's budget allows); starts this frame's list. */
void     kit_begin(KitGpu *k, VkCommandBuffer cmd, int slot, uint64_t frame, const mat4 *viewProj, uint32_t screenH);
/* A chunk's instances: origin its camera-relative origin; rec its chunk record in this frame's draws
 * (-1: not drawn, its instances only cast shadows). */
void     kit_add(KitGpu *k, int slot, const struct KitInst *inst, uint32_t n, const float origin[3], int rec);
/* Writes the frame's instances and draws (after the last kit_add). */
void     kit_end(KitGpu *k, int slot);
/* The frame's draws, with the kit pipeline (or its prepass one) bound. */
void     kit_draw(KitGpu *k, VkCommandBuffer cmd, int slot);
KitStats kit_stats(const KitGpu *k);
