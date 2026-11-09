#pragma once
#include "vkcore.h"

/* Acceleration structures for ray-traced shadows.
 *
 * Every chunk layer that casts shadows gets a bottom-level structure (BLAS)
 * over its pages in the geometry pool, built on the GPU straight from the
 * pool's vertices through an index list of the triangles that cast (glass,
 * light veils and flat litter don't). A BLAS is built with compaction allowed;
 * once its compacted size is known (MAX_FRAMES later) it is copied into a
 * compacted one. All BLASes live in one arena buffer.
 *
 * Each frame the chunk streamer lists the BLASes that are in range as
 * instances, at their chunk's camera-relative origin, the terrain adds its
 * near levels (terrain_rt.c: meshes of its own, refit in place, rt_create_mesh),
 * the caves their drawn bricks (cave_gpu.c: built once from their pool,
 * rt_build_mesh), and the renderer builds a fresh top-level structure (TLAS)
 * over them. The
 * TLAS does not depend on the view frustum, so buildings behind or beside the
 * camera shadow what it sees. */

typedef struct Rt Rt;

/* Instance masks: one bit per chunk layer, so a receiver can ignore a layer. */
#define RT_MASK_CITY      0x01u
#define RT_MASK_INTERIOR  0x02u
#define RT_MASK_LITTER    0x04u
#define RT_MASK_TERRAIN   0x08u   /* the clipmap's near levels (terrain_rt.c) */
#define RT_MASK_CAVE      0x10u   /* the caves' bricks (cave_gpu.c): only the moon's rays in the caves trace them */

/* Instance custom indices' low two bits: what an instance is, for the rays
 * that test its non-opaque ones' hits (terrain/dir_shadow.glsl). The rest is
 * the kind's own. */
#define RT_INST_CITY      0u
#define RT_INST_TERRAIN   1u
#define RT_INST_CAVE      2u

Rt  *rt_create(VkCore *vk);
void rt_destroy(Rt *rt);

/* Once per frame, after the frame's fence and before any build: frees
 * structures retired MAX_FRAMES ago and compacts those built MAX_FRAMES ago. */
void rt_begin_frame(Rt *rt, VkCommandBuffer cmd, int slot, uint64_t frame);

/* Whether a BLAS over `indices` vertex indices still fits this frame's budget. */
bool rt_can_build(Rt *rt, int slot, uint32_t indices);

/* Builds a BLAS over pool pages: page p's vertices start at vertex
 * pages[p] * PAGE_VERTS of the pool, it holds pageVerts[p] vertices, and its
 * casting triangles are idxCount[p] page-local indices, taken in turn from
 * idx. The pool's vertices must already be written and visible to
 * acceleration-structure builds. Returns a handle, or -1 if there was no room
 * (try again later). */
int  rt_build_blas(Rt *rt, VkCommandBuffer cmd, int slot, VkBuffer pool, const uint16_t *pages, int npages,
                   const uint32_t *pageVerts, const uint16_t *idx, const uint32_t *idxCount);
/* A mesh of the caller's that moves in place (the terrain's levels): float3
 * vertices at `verts`, `stride` bytes apart, and uint16 indices at `idx`,
 * both in buffers made for acceleration-structure build input. Its counts are
 * fixed for its life; its vertices may change between updates. */
typedef struct RtMesh { VkDeviceAddress verts, idx; uint32_t stride, nverts, nidx; VkFormat format; /* 0: float3 */ } RtMesh;
/* A BLAS built once over such a mesh (any vertex format acceleration
 * structures take: the caves' are int16 as SNORM), compacted as the chunks'
 * are. -1 if this frame's budget or the arena has no room (try again later). */
int  rt_build_mesh(Rt *rt, VkCommandBuffer cmd, int slot, const RtMesh *m);
/* A BLAS for such a mesh, made able to be refit and not compacted; not yet
 * built (rt_update_mesh with full first). -1 if there was no room. */
int  rt_create_mesh(Rt *rt, const RtMesh *m);
/* Builds (full) or refits the mesh's BLAS in place from its vertices as they
 * stand, which must be visible to acceleration-structure builds. The frames
 * in flight may still trace it: the caller waits for them first. False if
 * this frame's scratch is spent (try again next frame). */
bool rt_update_mesh(Rt *rt, VkCommandBuffer cmd, int slot, int h, const RtMesh *m, bool full);
/* Retires a BLAS; it is destroyed once no frame in flight can use it. */
void rt_free_blas(Rt *rt, int h, uint64_t frame);

/* This frame's TLAS: the instances added since rt_begin_frame, then built. */
void rt_add_instance(Rt *rt, int slot, int h, const float origin[3], uint32_t mask);
/* An instance by its whole transform (rows: x, y, z), with its custom index
 * (RT_INST_*); a non-opaque one's hits go to the rays that test them (those
 * without gl_RayFlagsOpaqueEXT; the others take it as opaque). */
void rt_add_instance_xf(Rt *rt, int slot, int h, const float xf[3][4], uint32_t mask, uint32_t custom, bool opaque);
void rt_build_tlas(Rt *rt, VkCommandBuffer cmd, int slot);
VkAccelerationStructureKHR rt_tlas(Rt *rt, int slot);

typedef struct RtStats { int blas, instances, builds; double arenaMB, arenaUsedMB; } RtStats;
RtStats rt_stats(Rt *rt);
