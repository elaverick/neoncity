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
 * as terrain_mesh_fill's hc assumes. Zero-area triangles line the window's
 * edge: there the level has blended into its parent, so each odd vertex lies
 * on the parent's edge, and these fill the hairline cracks rounding can open
 * at the T-junctions.
 *
 * Each list is cut into TILES x TILES tiles of the grid, one after another,
 * each tile's cells then its stretch of the edge. A frame draws the tiles of
 * each level whose bounds (terrain_clip_bounds) touch the view frustum, runs
 * of neighbours as one draw, from the finest level the screen-space error
 * asks for (terrain_clip_select) outwards. Culling only chooses what is drawn,
 * never what the clipmap holds.
 *
 * The camera-relative positions are built from doubles on the CPU (see
 * TerrainPush) and the heights are world Y, so the terrain stays exact
 * however far the camera goes. */

#define CLIP_N TERRAIN_CLIP_N
#define CLIP_W TERRAIN_CLIP_W
#define HOLE   ((CLIP_W - 1) / 2)     /* the finer window's side, in this level's cells */
#define LIST_WINDOW 4                 /* index lists 0-3: rings (hole offset 31 or 32 on x, + 2 * on z) */
#define TILE   18                     /* a tile's side, cells */
#define TILES  ((CLIP_W - 1) / TILE)
#define MAX_DRAWS (TERRAIN_CLIP_LEVELS * TILES * TILES)
/* Bounds round a tile's box, for the float positions the shader builds (m). */
#define TILE_MARGIN 0.5f

_Static_assert(TILES * TILE == CLIP_W - 1 && TILE % 2 == 0, "terrain tiles must cut the window evenly, along its edge's pairs of cells");
_Static_assert(TERRAIN_DISTANCE <= ((TERRAIN_CLIP_W - 1) / 2 - 1) * TERRAIN_CLIP_SPACING0 * (1 << (TERRAIN_CLIP_LEVELS - 1)),
               "TERRAIN_DISTANCE reaches past the coarsest clipmap window");
_Static_assert(TERRAIN_CLIP_W * TERRAIN_CLIP_W <= 65536, "the grid's vertices don't fit 16-bit indices");

struct TerrainGpu {
    TerrainClipmap clip;
    TerrainFade    fade;                  /* which levels are drawn, blending as they change */
    double         lastTime;              /* of the last update (s) */
    Buffer         samples;              /* device local: LEVELS * N * N TerrainSamples */
    Buffer         staging[MAX_FRAMES];   /* host visible: one update's worth */
    VkBufferCopy  *copies;                /* one per uploaded row */
    Buffer         indices;               /* device local, uint16 */
    uint32_t       first[LIST_WINDOW + 1][TILES * TILES], count[LIST_WINDOW + 1][TILES * TILES];
    VkDescriptorPool pool;
    VkDescriptorSet  set[MAX_FRAMES];
    /* this frame's draws (plan_draws), each a run of one level's tiles */
    TerrainPush    push[TERRAIN_CLIP_LEVELS];
    struct { int level; uint32_t first, count; } draw[MAX_DRAWS];
    int            ndraw;
    /* the last update, and the slowest so far (--shot prints them): the fill
     * (with the blocks' bounds), the draw plan, the staging copy, and in all */
    int            evaluated, drawn, tilesDrawn, tilesTotal;
    double         fillMs, planMs, stageMs, updateMs, slowestMs;
};

/* A triangle facing up (+y) seen from above, as the city's ground does
 * (emit_flat: counter-clockwise on screen), from grid vertices. */
static uint16_t *tri(uint16_t *o, int ai, int aj, int bi, int bj, int ci, int cj) {
    o[0] = (uint16_t)(aj * CLIP_W + ai);
    o[1] = (uint16_t)(bj * CLIP_W + bi);
    o[2] = (uint16_t)(cj * CLIP_W + ci);
    return o + 3;
}

/* Tile (tx, tz)'s cells outside [hx, hx + HOLE) x [hz, hz + HOLE) (no hole
 * if hx < 0), then its stretch of the edge's zero-area triangles: those whose
 * pair of cells it holds. For those the middle vertex is taken as nudged
 * inwards, where a crack would open, to wind them facing up. */
static uint16_t *build_tile(uint16_t *o, int hx, int hz, int tx, int tz) {
    for (int j = tz * TILE; j < tz * TILE + TILE; ++j)
        for (int i = tx * TILE; i < tx * TILE + TILE; ++i) {
            if (hx >= 0 && i >= hx && i < hx + HOLE && j >= hz && j < hz + HOLE) continue;
            o = tri(o, i, j, i + 1, j + 1, i + 1, j);
            o = tri(o, i, j, i, j + 1, i + 1, j + 1);
        }
    const int E = CLIP_W - 1;
    for (int k = 0; k + 2 <= E; k += 2) {
        int t = k / TILE;
        if (tz == 0 && tx == t)         o = tri(o, k, 0, k + 1, 0, k + 2, 0);     /* low z edge, inwards is +z */
        if (tz == TILES - 1 && tx == t) o = tri(o, k, E, k + 2, E, k + 1, E);     /* high z edge */
        if (tx == 0 && tz == t)         o = tri(o, 0, k, 0, k + 2, 0, k + 1);     /* low x edge, inwards is +x */
        if (tx == TILES - 1 && tz == t) o = tri(o, E, k, E, k + 1, E, k + 2);     /* high x edge */
    }
    return o;
}

/* Builds the five index lists, tile by tile, and uploads them, waiting for the copy. */
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
        int hx = k == LIST_WINDOW ? -1 : HOLE / 2 + (k & 1), hz = k == LIST_WINDOW ? -1 : HOLE / 2 + (k >> 1);
        for (int tile = 0; tile < TILES * TILES; ++tile) {
            uint16_t *tileStart = o;
            o = build_tile(o, hx, hz, tile % TILES, tile / TILES);
            t->first[k][tile] = (uint32_t)(tileStart - base);
            t->count[k][tile] = (uint32_t)(o - tileStart);
        }
        if ((size_t)(o - start) != (k == LIST_WINDOW ? windowIdx : ringIdx)) fatal("terrain: an index list came out the wrong length");
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
    terrain_fade_init(&t->fade);
    const VkDeviceSize sampleBytes = (VkDeviceSize)TERRAIN_CLIP_LEVELS * CLIP_N * CLIP_N * sizeof(TerrainSample);
    t->samples = vk_buffer(vk, sampleBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    /* An update writes no more samples than the heights it evaluates, at most
     * its budget, or a row when it can't afford one (terrain_clip_update) */
    _Static_assert(TERRAIN_CLIP_BUDGET >= TERRAIN_CLIP_W, "a terrain update's staging must hold at least a row");
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

/* This frame's draws: the levels from the finest the screen-space error asks
 * for (or the finest ready, if coarser), reached a level at a time as the
 * finest blends in or out (terrain_fade_step), the finest as a whole window,
 * each coarser one as the ring round its finer neighbour; of each, the tiles
 * in view. Positions are relative to the camera of this update. K comes from
 * viewProj's second row, the projection's 1 / tan(fovy / 2) times the view's
 * unit up axis. */
static void plan_draws(TerrainGpu *t, const double cam[3], const mat4 *viewProj, uint32_t height, double dt) {
    const TerrainClipmap *c = &t->clip;
    t->ndraw = t->tilesDrawn = t->tilesTotal = 0;
    t->drawn = TERRAIN_CLIP_LEVELS;
    if (c->finest >= TERRAIN_CLIP_LEVELS) return;
    const float *m = viewProj->m;
    double K = 0.5 * height * sqrt((double)m[1] * m[1] + (double)m[5] * m[5] + (double)m[9] * m[9]);
    terrain_fade_step(&t->fade, c, terrain_clip_select(&t->clip, cam[1], K), dt);
    t->drawn = t->fade.shown;
    Frustum fr = frustum_from_matrix(*viewProj);
    /* Every level's corner is a whole number of its samples from the anchor,
     * a few tens of km at most, so its offset is exact in float. */
    const TerrainClipLevel *top = &c->level[TERRAIN_CLIP_LEVELS - 1];
    double ax = (double)top->origin[0] * top->spacing, az = (double)top->origin[1] * top->spacing;
    for (int l = t->drawn; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainClipLevel *lv = &c->level[l];
        TerrainPush *pc = &t->push[l];
        pc->anchor[0] = (float)(ax - c->cam[0]);
        pc->anchor[1] = (float)(az - c->cam[1]);
        pc->offset[0] = (float)((double)lv->origin[0] * lv->spacing - ax);
        pc->offset[1] = (float)((double)lv->origin[1] * lv->spacing - az);
        pc->spacing = (float)lv->spacing;
        pc->level = l;
        pc->slot[0] = terrain_clip_slot(lv->origin[0]);
        pc->slot[1] = terrain_clip_slot(lv->origin[1]);
        pc->pslot[0] = terrain_clip_slot(lv->origin[0] / 2);   /* origins are even */
        pc->pslot[1] = terrain_clip_slot(lv->origin[1] / 2);
        pc->top = l == TERRAIN_CLIP_LEVELS - 1;
        pc->fade = terrain_fade_morph(&t->fade, l);
        int list = LIST_WINDOW;
        if (l > t->drawn) {
            const TerrainClipLevel *fine = &c->level[l - 1];
            int64_t hx = fine->origin[0] / 2 - lv->origin[0] - HOLE / 2, hz = fine->origin[1] / 2 - lv->origin[1] - HOLE / 2;
            if (hx < 0 || hx > 1 || hz < 0 || hz > 1) fatal("terrain: a clipmap window is off its parent's centre");
            list = (int)(hx + 2 * hz);
        }
        for (int tile = 0; tile < TILES * TILES; ++tile) {
            if (t->count[list][tile] == 0) continue;
            t->tilesTotal++;
            int64_t i0 = lv->origin[0] + (tile % TILES) * TILE, j0 = lv->origin[1] + (tile / TILES) * TILE;
            float lo, hi;
            terrain_clip_bounds(lv, i0, j0, i0 + TILE, j0 + TILE, &lo, &hi);
            vec3 mn = v3((float)((double)i0 * lv->spacing - cam[0]) - TILE_MARGIN, (float)(lo - cam[1]) - TILE_MARGIN,
                         (float)((double)j0 * lv->spacing - cam[2]) - TILE_MARGIN);
            vec3 mx = v3((float)((double)(i0 + TILE) * lv->spacing - cam[0]) + TILE_MARGIN, (float)(hi - cam[1]) + TILE_MARGIN,
                         (float)((double)(j0 + TILE) * lv->spacing - cam[2]) + TILE_MARGIN);
            if (!frustum_test_aabb(&fr, mn, mx)) continue;
            t->tilesDrawn++;
            if (t->ndraw > 0 && t->draw[t->ndraw - 1].level == l && t->draw[t->ndraw - 1].first + t->draw[t->ndraw - 1].count == t->first[list][tile]) {
                t->draw[t->ndraw - 1].count += t->count[list][tile];
            } else {
                t->draw[t->ndraw].level = l;
                t->draw[t->ndraw].first = t->first[list][tile];
                t->draw[t->ndraw].count = t->count[list][tile];
                t->ndraw++;
            }
        }
    }
}

/* The slot's last frame has finished (frame_acquire waited on its fence), so
 * its staging buffer is free. Earlier frames may still be drawing from the
 * slots this overwrites: the copy waits for their vertex shaders. */
void terrain_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj) {
    TerrainGpu *t = r->terrain;
    double t0 = glfwGetTime();
    t->evaluated = terrain_clip_update(&t->clip, cam[0], cam[2], TERRAIN_CLIP_BUDGET);
    double t1 = glfwGetTime();
    plan_draws(t, cam, viewProj, r->vk->extent.height, t->lastTime > 0.0 ? t0 - t->lastTime : 0.0);
    t->lastTime = t0;
    double t2 = glfwGetTime();

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
    double t3 = glfwGetTime();
    t->fillMs = (t1 - t0) * 1000.0;
    t->planMs = (t2 - t1) * 1000.0;
    t->stageMs = (t3 - t2) * 1000.0;
    t->updateMs = (t3 - t0) * 1000.0;
    if (t->updateMs > t->slowestMs) t->slowestMs = t->updateMs;
    if (n == 0) return;
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(cmd, t->staging[slot].buf, t->samples.buf, n, t->copies);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

/* The update's plan (plan_draws), a push per level drawn. */
void terrain_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    TerrainGpu *t = r->terrain;
    if (t->ndraw == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->terrainPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->terrainPL, 0, 1, &t->set[slot], 0, NULL);
    vkCmdBindIndexBuffer(cmd, t->indices.buf, 0, VK_INDEX_TYPE_UINT16);
    int pushed = -1;
    for (int k = 0; k < t->ndraw; ++k) {
        if (t->draw[k].level != pushed) {
            pushed = t->draw[k].level;
            vkCmdPushConstants(cmd, r->terrainPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TerrainPush), &t->push[pushed]);
        }
        vkCmdDrawIndexed(cmd, t->draw[k].count, 1, t->draw[k].first, 0, 0);
    }
}

bool renderer_terrain_settled(Renderer *r) {
    const TerrainGpu *t = r->terrain;
    return t->clip.finest <= t->clip.want && t->drawn == t->clip.want && t->fade.level < 0;
}

void renderer_terrain_stats(Renderer *r, char *buf, size_t n) {
    const TerrainGpu *t = r->terrain;
    char fading[48] = "";
    if (t->fade.level >= 0) snprintf(fading, sizeof(fading), ", level %d fading %s", t->fade.level, t->fade.in ? "in" : "out");
    snprintf(buf, n, "terrain: levels %d-%d drawn (finest ready %d, screen-space error asks for %d%s), %d of %d tiles in view in %d draws,\n"
             "         last update evaluated %d heights in %.2f ms (fill %.2f, plan %.2f, staging %.2f; slowest %.2f ms)",
             t->drawn, TERRAIN_CLIP_LEVELS - 1, t->clip.finest, t->clip.want, fading, t->tilesDrawn, t->tilesTotal, t->ndraw,
             t->evaluated, t->updateMs, t->fillMs, t->planMs, t->stageMs, t->slowestMs);
}
