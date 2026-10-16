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
 * instances, at their chunk's camera-relative origin, and a fresh top-level
 * structure (TLAS) is built over them. The TLAS does not depend on the view
 * frustum, so buildings behind or beside the camera shadow what it sees. */

typedef struct Rt Rt;

/* Instance masks: one bit per chunk layer, so a receiver can ignore a layer. */
#define RT_MASK_CITY      0x01u
#define RT_MASK_INTERIOR  0x02u
#define RT_MASK_LITTER    0x04u

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
/* Retires a BLAS; it is destroyed once no frame in flight can use it. */
void rt_free_blas(Rt *rt, int h, uint64_t frame);

/* This frame's TLAS: the instances added since rt_begin_frame, then built. */
void rt_add_instance(Rt *rt, int slot, int h, const float origin[3], uint32_t mask);
void rt_build_tlas(Rt *rt, VkCommandBuffer cmd, int slot);
VkAccelerationStructureKHR rt_tlas(Rt *rt, int slot);

typedef struct RtStats { int blas, instances, builds; double arenaMB, arenaUsedMB; } RtStats;
RtStats rt_stats(Rt *rt);
