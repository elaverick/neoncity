#pragma once
#include "vkcore.h"
#include "mathlib.h"

/* Frustum-filling chunk streamer.
 *
 * Each frame the set of chunks that intersect the view frustum (out to
 * VIEW_DISTANCE), plus a small ring around the player, is computed. Missing
 * chunks are generated nearest-first on worker threads; because generation is
 * a pure function of chunk coordinates, evicted chunks are simply rebuilt
 * identically when they come back into view. Office lobbies are a second,
 * near-only layer of chunks (city_build_interiors), wanted within
 * INTERIOR_DISTANCE. */

typedef struct ChunkSystem ChunkSystem;

typedef struct ChunkStats {
    int resident, queued, ready, drawnChunks, drawCalls, pagesUsed, pagesTotal, uploadsThisFrame;
} ChunkStats;

ChunkSystem *chunks_create(VkCore *vk);
void         chunks_destroy(ChunkSystem *cs);

/* cam: world-space camera position (double precision). viewProj is camera-relative. */
void         chunks_update(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint64_t frameIndex,
                           const double cam[3], const mat4 *viewProj);
void         chunks_draw(ChunkSystem *cs, VkCommandBuffer cmd, int slot);
/* The lobby plants' layer, drawn with a pipeline that does not cull back faces. */
void         chunks_draw_plants(ChunkSystem *cs, VkCommandBuffer cmd, int slot);
/* Draws resident chunks overlapping the shelter map square (SHELTER_RADIUS around the camera). */
void         chunks_draw_shelter(ChunkSystem *cs, VkCommandBuffer cmd, int slot);
VkBuffer     chunks_gpu_buffer(ChunkSystem *cs, int slot);
VkDeviceSize chunks_gpu_buffer_size(void);
VkBuffer     chunks_light_buffer(ChunkSystem *cs);   /* light tables of resident chunks (device local) */
VkDeviceSize chunks_light_buffer_size(void);
ChunkStats   chunks_stats(ChunkSystem *cs);
/* Sky lights (src/common.h) of resident chunks around the camera, camera-relative and gridded. */
void         chunks_sky_lights(ChunkSystem *cs, const double cam[3], SkyGrid *out);
