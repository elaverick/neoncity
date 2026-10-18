#include "renderer_internal.h"
#include "terrain.h"

/* The terrain's GPU side. It draws the clipmap src/terrain keeps round the
 * camera and never generates terrain itself.
 *
 * Samples: every level's N x N slots in one device-local storage buffer, laid
 * out as the clipmap stores them (toroidally, TerrainSample's five floats
 * each), so a move uploads only the rectangles terrain_clip_update reports
 * dirty: a copy per row, through this frame slot's staging buffer.
 *
 * Mesh: one W x W grid of vertices, whose (i, j) the vertex shader takes from
 * gl_VertexIndex, so there is no vertex buffer. Index lists cover the whole
 * window (for the finest level drawn) or the ring round a hole where the
 * finer level is drawn. The finer window's corner lies 31 or 32 cells in from
 * this one's on each axis (the camera's side of its parent sample), so there
 * are four rings. Cells are split along the (i, j)-(i + 1, j + 1) diagonal,
 * as terrain_mesh_fill's hc assumes. Every list ends with zero-area triangles
 * along the window's edge: there the level has blended into its parent, so
 * each odd vertex lies on the parent's edge, and these fill the hairline
 * cracks rounding can open at the T-junctions.
 *
 * The camera-relative positions are built from doubles on the CPU (see
 * TerrainPush) and the heights are world Y, so the terrain stays exact
 * however far the camera goes. */

#define CLIP_N TERRAIN_CLIP_N
#define CLIP_W TERRAIN_CLIP_W
#define HOLE   ((CLIP_W - 1) / 2)     /* the finer window's side, in this level's cells */
#define LIST_WINDOW 4                 /* index lists 0-3: rings (hole offset 31 or 32 on x, + 2 * on z) */

_Static_assert(TERRAIN_DISTANCE <= ((TERRAIN_CLIP_W - 1) / 2 - 1) * TERRAIN_CLIP_SPACING0 * (1 << (TERRAIN_CLIP_LEVELS - 1)),
               "TERRAIN_DISTANCE reaches past the coarsest clipmap window");
_Static_assert(TERRAIN_CLIP_W * TERRAIN_CLIP_W <= 65536, "the grid's vertices don't fit 16-bit indices");

struct TerrainGpu {
    TerrainClipmap clip;
    Buffer         samples;               /* device local: LEVELS * N * N TerrainSamples */
    Buffer         staging[MAX_FRAMES];   /* host visible: one update's worth */
    VkBufferCopy  *copies;                /* one per uploaded row */
    Buffer         indices;               /* device local, uint16 */
    uint32_t       first[LIST_WINDOW + 1], count[LIST_WINDOW + 1];
    VkDescriptorPool pool;
    VkDescriptorSet  set[MAX_FRAMES];
    /* the last update, and the slowest so far (--shot prints them) */
    int            filled;
    double         updateMs, slowestMs;
};

/* A triangle facing up (+y) seen from above, as the city's ground does
 * (emit_flat: counter-clockwise on screen), from grid vertices. */
static uint16_t *tri(uint16_t *o, int ai, int aj, int bi, int bj, int ci, int cj) {
    o[0] = (uint16_t)(aj * CLIP_W + ai);
    o[1] = (uint16_t)(bj * CLIP_W + bi);
    o[2] = (uint16_t)(cj * CLIP_W + ci);
    return o + 3;
}

/* The cells of the window outside [hx, hx + HOLE) x [hz, hz + HOLE) (no hole
 * if hx < 0), then the edge's zero-area triangles. For those the middle
 * vertex is taken as nudged inwards, where a crack would open, to wind them
 * facing up. */
static uint16_t *build_list(uint16_t *o, int hx, int hz) {
    for (int j = 0; j < CLIP_W - 1; ++j)
        for (int i = 0; i < CLIP_W - 1; ++i) {
            if (hx >= 0 && i >= hx && i < hx + HOLE && j >= hz && j < hz + HOLE) continue;
            o = tri(o, i, j, i + 1, j + 1, i + 1, j);
            o = tri(o, i, j, i, j + 1, i + 1, j + 1);
        }
    const int E = CLIP_W - 1;
    for (int k = 0; k + 2 <= E; k += 2) {
        o = tri(o, k, 0, k + 1, 0, k + 2, 0);     /* low z edge, inwards is +z */
        o = tri(o, k, E, k + 2, E, k + 1, E);     /* high z edge */
        o = tri(o, 0, k, 0, k + 2, 0, k + 1);     /* low x edge, inwards is +x */
        o = tri(o, E, k, E, k + 1, E, k + 2);     /* high x edge */
    }
    return o;
}

/* Builds the five index lists and uploads them, waiting for the copy. */
static void upload_indices(Renderer *r, TerrainGpu *t) {
    VkCore *vk = r->vk;
    const size_t cells = (size_t)(CLIP_W - 1) * (CLIP_W - 1), edge = 4 * ((CLIP_W - 1) / 2);
    const size_t windowIdx = 6 * cells + 3 * edge, ringIdx = 6 * (cells - (size_t)HOLE * HOLE) + 3 * edge;
    const size_t total = windowIdx + LIST_WINDOW * ringIdx;
    Buffer staging = vk_buffer(vk, total * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uint16_t *base = (uint16_t *)staging.mapped, *o = base;
    for (int k = 0; k <= LIST_WINDOW; ++k) {
        uint16_t *start = o;
        o = k == LIST_WINDOW ? build_list(o, -1, -1) : build_list(o, HOLE / 2 + (k & 1), HOLE / 2 + (k >> 1));
        t->first[k] = (uint32_t)(start - base);
        t->count[k] = (uint32_t)(o - start);
        if (t->count[k] != (k == LIST_WINDOW ? windowIdx : ringIdx)) fatal("terrain: an index list came out the wrong length");
    }
    t->indices = vk_buffer(vk, total * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkBufferCopy bc = { 0, 0, total * sizeof(uint16_t) };
    vkCmdCopyBuffer(cmd, staging.buf, t->indices.buf, 1, &bc);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkFreeCommandBuffers(vk->device, r->cmdPool, 1, &cmd);
    vk_buffer_destroy(vk, &staging);
}

void terrain_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    TerrainGpu *t = (TerrainGpu *)calloc(1, sizeof(TerrainGpu));
    if (!t) fatal("out of memory allocating the terrain");
    r->terrain = t;
    terrain_clip_init(&t->clip);
    const VkDeviceSize sampleBytes = (VkDeviceSize)TERRAIN_CLIP_LEVELS * CLIP_N * CLIP_N * sizeof(TerrainSample);
    t->samples = vk_buffer(vk, sampleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    /* An update fills at most its budget of samples (terrain_clip_update) */
    for (int i = 0; i < MAX_FRAMES; ++i)
        t->staging[i] = vk_buffer(vk, (VkDeviceSize)TERRAIN_CLIP_BUDGET * sizeof(TerrainSample), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    t->copies = (VkBufferCopy *)malloc(sizeof(VkBufferCopy) * TERRAIN_CLIP_BUDGET);
    if (!t->copies) fatal("out of memory allocating the terrain");
    upload_indices(r, t);

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &t->pool));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = t->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->terrainLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &t->set[i]));
        write_buf_set(vk, t->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, t->set[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, t->samples.buf, sampleBytes);
    }
}

void terrain_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    TerrainGpu *t = r->terrain;
    if (!t) return;
    vkDestroyDescriptorPool(vk->device, t->pool, NULL);
    vk_buffer_destroy(vk, &t->indices);
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(vk, &t->staging[i]);
    vk_buffer_destroy(vk, &t->samples);
    free(t->copies);
    terrain_clip_free(&t->clip);
    free(t);
    r->terrain = NULL;
}

/* The slot's last frame has finished (frame_acquire waited on its fence), so
 * its staging buffer is free. Earlier frames may still be drawing from the
 * slots this overwrites: the copy waits for their vertex shaders. */
void terrain_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3]) {
    TerrainGpu *t = r->terrain;
    double t0 = glfwGetTime();
    t->filled = terrain_clip_update(&t->clip, cam[0], cam[2], TERRAIN_CLIP_BUDGET);
    t->updateMs = (glfwGetTime() - t0) * 1000.0;
    if (t->updateMs > t->slowestMs) t->slowestMs = t->updateMs;

    uint8_t *dst = (uint8_t *)t->staging[slot].mapped;
    VkDeviceSize used = 0;
    uint32_t n = 0;
    for (int l = 0; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *lv = &t->clip.level[l];
        for (int k = 0; k < lv->n_dirty; ++k) {
            TerrainRect d = lv->dirty[k];
            VkDeviceSize row = (VkDeviceSize)(d.x1 - d.x0) * sizeof(TerrainSample);
            for (int z = d.z0; z < d.z1; ++z) {
                if (used + row > t->staging[slot].size || n >= TERRAIN_CLIP_BUDGET) fatal("terrain: an update overflowed its staging buffer");
                memcpy(dst + used, &lv->samples[z * CLIP_N + d.x0], (size_t)row);
                t->copies[n++] = (VkBufferCopy){ used, (((VkDeviceSize)l * CLIP_N + z) * CLIP_N + d.x0) * sizeof(TerrainSample), row };
                used += row;
            }
        }
    }
    if (n == 0) return;
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(cmd, t->staging[slot].buf, t->samples.buf, n, t->copies);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

/* The ready levels, finest first: the finest as a whole window, each coarser
 * one as the ring round its finer neighbour. Positions are relative to the
 * camera of the last update, which is this frame's. */
void terrain_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    TerrainGpu *t = r->terrain;
    const TerrainClipmap *c = &t->clip;
    if (c->finest >= TERRAIN_CLIP_LEVELS) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->terrainPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->terrainPL, 0, 1, &t->set[slot], 0, NULL);
    vkCmdBindIndexBuffer(cmd, t->indices.buf, 0, VK_INDEX_TYPE_UINT16);
    /* Every level's corner is a whole number of its samples from the anchor,
     * a few tens of km at most, so its offset is exact in float. */
    const TerrainClipLevel *top = &c->level[TERRAIN_CLIP_LEVELS - 1];
    double ax = (double)top->origin[0] * top->spacing, az = (double)top->origin[1] * top->spacing;
    TerrainPush pc;
    pc.anchor[0] = (float)(ax - c->cam[0]);
    pc.anchor[1] = (float)(az - c->cam[1]);
    for (int l = c->finest; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *lv = &c->level[l];
        pc.offset[0] = (float)((double)lv->origin[0] * lv->spacing - ax);
        pc.offset[1] = (float)((double)lv->origin[1] * lv->spacing - az);
        pc.spacing = (float)lv->spacing;
        pc.level = l;
        pc.slot[0] = terrain_clip_slot(lv->origin[0]);
        pc.slot[1] = terrain_clip_slot(lv->origin[1]);
        pc.pslot[0] = terrain_clip_slot(lv->origin[0] / 2);   /* origins are even */
        pc.pslot[1] = terrain_clip_slot(lv->origin[1] / 2);
        pc.top = l == TERRAIN_CLIP_LEVELS - 1;
        int list = LIST_WINDOW;
        if (l > c->finest) {
            const TerrainClipLevel *fine = &c->level[l - 1];
            int64_t hx = fine->origin[0] / 2 - lv->origin[0] - HOLE / 2, hz = fine->origin[1] / 2 - lv->origin[1] - HOLE / 2;
            if (hx < 0 || hx > 1 || hz < 0 || hz > 1) fatal("terrain: a clipmap window is off its parent's centre");
            list = (int)(hx + 2 * hz);
        }
        vkCmdPushConstants(cmd, r->terrainPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, t->count[list], 1, t->first[list], 0, 0);
    }
}

void renderer_terrain_stats(Renderer *r, char *buf, size_t n) {
    const TerrainGpu *t = r->terrain;
    snprintf(buf, n, "terrain: levels %d-%d drawn, last update filled %d samples in %.2f ms (slowest %.2f ms)",
             t->clip.finest, TERRAIN_CLIP_LEVELS - 1, t->filled, t->updateMs, t->slowestMs);
}
