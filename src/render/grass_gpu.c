#include "renderer_internal.h"
#include "grass_gpu.h"

/* The grass's GPU path (GR2, docs/procedural_grass_rendering_implementation.md
 * section 6): the land's blades chosen, made and drawn on the GPU every
 * frame, with no per-blade work on the CPU.
 *
 * Prepare (outside any rendering, after the terrain's upload): the frame
 * slot's counters are cleared; shaders/grass/prepare.comp, a workgroup per
 * patch round the camera, culls the patches, evaluates their roots on the
 * ground the terrain draws (the clipmap's samples, as terrain.vert reads
 * them: GrassFrame carries the levels' pushes) and writes the blades of the
 * roots kept into the slot's instance buffer, a range per bucket (level of
 * detail); finish.comp writes each bucket's VkDrawIndirectCommand and the
 * distance the next frame keeps each to (keepTo, the one buffer shared by
 * the slots: frames run in order on the queue). The counters are copied to a
 * host buffer, read when the slot comes round again (the --shot log).
 *
 * Draw (in the scene pass after the terrain): a draw per bucket, its count
 * the GPU's; GR2's stand-in blade is two triangles (shaders/grass/blade.*).
 *
 * Each bucket's room is its own (GRASS_CAPS, or --grass-cap), so a far
 * bucket can't take a near one's; within one, finish.comp's distance keeps
 * the near blades and lets the far ones go. */

static const uint32_t SPV_prepare[] = {
#include "grass/prepare.comp.inc"
};
static const uint32_t SPV_finish[] = {
#include "grass/finish.comp.inc"
};
static const uint32_t SPV_blade_vert[] = {
#include "grass/blade.vert.inc"
};
static const uint32_t SPV_blade_frag[] = {
#include "grass/blade.frag.inc"
};

typedef struct BladePush { uint32_t base; int32_t lod; } BladePush;

struct GrassGpu {
    bool supported, on;
    uint32_t cap[GRASS_BUCKETS], base[GRASS_BUCKETS];
    VkDescriptorSetLayout compLayout, drawLayout;
    VkPipelineLayout compPL, drawPL;
    VkPipeline preparePipe, finishPipe, bladePipe;
    VkDescriptorPool pool;
    VkDescriptorSet compSet[MAX_FRAMES], drawSet[MAX_FRAMES];
    Buffer frame[MAX_FRAMES];      /* GrassFrameGPU, host visible */
    Buffer instances[MAX_FRAMES];  /* the buckets' ranges, one after another */
    Buffer counters[MAX_FRAMES];   /* GRASS_C_WORDS */
    Buffer draws[MAX_FRAMES];      /* a VkDrawIndirectCommand per bucket */
    Buffer patches[MAX_FRAMES];    /* GRASS_PATCH_* per patch of the square */
    Buffer readback[MAX_FRAMES];   /* the counters, host visible */
    Buffer keepTo;                 /* per bucket: the distance blades are kept to (m) */
    bool keepToFresh, ran[MAX_FRAMES], drawing;
    uint32_t stats[GRASS_C_STATS];
    bool haveStats;
};

static bool subgroups_do_sums(VkCore *vk) {
    VkPhysicalDeviceSubgroupProperties sp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    p2.pNext = &sp;
    vkGetPhysicalDeviceProperties2(vk->phys, &p2);
    const VkSubgroupFeatureFlags need = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT;
    return (sp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && (sp.supportedOperations & need) == need;
}

void grass_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    GrassGpu *g = (GrassGpu *)calloc(1, sizeof(GrassGpu));
    if (!g) fatal("out of memory allocating the grass");
    r->grass = g;
    g->supported = subgroups_do_sums(vk);
    g->on = g->supported;
    uint32_t total = 0;
    for (int b = 0; b < GRASS_BUCKETS; ++b) { g->cap[b] = GRASS_CAPS[b]; g->base[b] = total; total += GRASS_CAPS[b]; }
    printf("Grass: %s (room for %u blades a frame, %.0f MB a frame slot)\n", g->supported ? "on" : "off (the GPU's subgroups can't add up)",
           total, (double)total * sizeof(GrassInstanceGPU) / (1024.0 * 1024.0));

    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT, V = VK_SHADER_STAGE_VERTEX_BIT, F = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutBinding comp[9] = {
        { 0, UB, 1, C, NULL },    /* FrameUBO */
        { 1, SB, 1, C, NULL },    /* the clipmap's samples */
        { 2, SB, 1, C, NULL },    /* the roads (road_gpu.c) */
        { 4, UB, 1, C, NULL },    /* GrassFrame */
        { 5, SB, 1, C, NULL },    /* the instances */
        { 6, SB, 1, C, NULL },    /* the counters */
        { 7, SB, 1, C, NULL },    /* the draws */
        { 9, SB, 1, C, NULL },    /* keepTo */
        { 10, SB, 1, C, NULL },   /* the patch states */
    };
    g->compLayout = make_set_layout(vk, comp, 9);
    VkDescriptorSetLayoutBinding draw[4] = {
        { 0, UB, 1, V | F, NULL },
        { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT, F, NULL },   /* the materials (ground.glsl's light and haze) */
        { 5, SB, 1, V, NULL },
        { 8, UB, 1, F, NULL },
    };
    g->drawLayout = make_set_layout(vk, draw, 4);
    g->compPL = make_pipeline_layout(vk, g->compLayout, 0, 0);
    g->drawPL = make_pipeline_layout(vk, g->drawLayout, sizeof(BladePush), V);
    g->preparePipe = make_compute(vk, SPV_prepare, sizeof(SPV_prepare), g->compPL);
    g->finishPipe = make_compute(vk, SPV_finish, sizeof(SPV_finish), g->compPL);
    GfxDesc blade = { SPV_blade_vert, sizeof(SPV_blade_vert), SPV_blade_frag, sizeof(SPV_blade_frag),
                      g->drawPL, HDR_FORMAT, DEPTH_FORMAT, vk->msaa, false, true, true, false };   /* two-sided */
    blade.colorFormat2 = HDR_FORMAT;   /* the reflection G-buffer: nothing */
    g->bladePipe = make_graphics(vk, &blade);

    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags DEV = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkDeviceSize patchBytes = (VkDeviceSize)GRASS_PATCH_GRID * GRASS_PATCH_GRID * sizeof(uint32_t);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        g->frame[i] = vk_buffer(vk, sizeof(GrassFrameGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HOST);
        g->instances[i] = vk_buffer(vk, (VkDeviceSize)total * sizeof(GrassInstanceGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEV);
        g->counters[i] = vk_buffer(vk, GRASS_C_WORDS * sizeof(uint32_t),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, DEV);
        g->draws[i] = vk_buffer(vk, GRASS_BUCKETS * sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, DEV);
        g->patches[i] = vk_buffer(vk, patchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEV);
        g->readback[i] = vk_buffer(vk, GRASS_C_WORDS * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST);
    }
    g->keepTo = vk_buffer(vk, 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, DEV);
    g->keepToFresh = true;

    VkDescriptorPoolSize sizes[3] = {
        { UB, 4 * MAX_FRAMES },
        { SB, 8 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT * MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2 * MAX_FRAMES;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &g->pool));
    VkDeviceSize sampleBytes;
    VkBuffer samples = terrain_gpu_samples(r, &sampleBytes);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g->compLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &g->compSet[i]));
        ai.pSetLayouts = &g->drawLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &g->drawSet[i]));
        VkDeviceSize roadBytes;
        VkBuffer roads = road_gpu_buffer(r, i, &roadBytes);
        VkDescriptorSet s = g->compSet[i];
        write_buf_set(vk, s, 0, UB, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, s, 1, SB, samples, sampleBytes);
        write_buf_set(vk, s, 2, SB, roads, roadBytes);
        write_buf_set(vk, s, 4, UB, g->frame[i].buf, sizeof(GrassFrameGPU));
        write_buf_set(vk, s, 5, SB, g->instances[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 6, SB, g->counters[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 7, SB, g->draws[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 9, SB, g->keepTo.buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 10, SB, g->patches[i].buf, VK_WHOLE_SIZE);
        s = g->drawSet[i];
        write_buf_set(vk, s, 0, UB, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, s, 5, SB, g->instances[i].buf, VK_WHOLE_SIZE);
        write_material_set(r, s);
        write_buf_set(vk, terrain_gpu_set(r, i), 10, SB, g->patches[i].buf, VK_WHOLE_SIZE);   /* --grass-debug cull */
    }
}

void grass_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    GrassGpu *g = r->grass;
    if (!g) return;
    vkDestroyPipeline(vk->device, g->preparePipe, NULL);
    vkDestroyPipeline(vk->device, g->finishPipe, NULL);
    vkDestroyPipeline(vk->device, g->bladePipe, NULL);
    vkDestroyPipelineLayout(vk->device, g->compPL, NULL);
    vkDestroyPipelineLayout(vk->device, g->drawPL, NULL);
    vkDestroyDescriptorSetLayout(vk->device, g->compLayout, NULL);
    vkDestroyDescriptorSetLayout(vk->device, g->drawLayout, NULL);
    vkDestroyDescriptorPool(vk->device, g->pool, NULL);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vk_buffer_destroy(vk, &g->frame[i]);
        vk_buffer_destroy(vk, &g->instances[i]);
        vk_buffer_destroy(vk, &g->counters[i]);
        vk_buffer_destroy(vk, &g->draws[i]);
        vk_buffer_destroy(vk, &g->patches[i]);
        vk_buffer_destroy(vk, &g->readback[i]);
    }
    vk_buffer_destroy(vk, &g->keepTo);
    free(g);
    r->grass = NULL;
}

void renderer_set_grass(Renderer *r, bool on) { r->grass->on = on && r->grass->supported; }

void renderer_set_grass_cap(Renderer *r, uint32_t cap) {
    for (int b = 0; b < GRASS_BUCKETS; ++b) r->grass->cap[b] = cap > 0 && cap < GRASS_CAPS[b] ? cap : GRASS_CAPS[b];
}

void grass_gpu_prepare(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *viewProj) {
    GrassGpu *g = r->grass;
    g->drawing = false;
    /* this slot's last frame has finished (frame_acquire): its counters are in */
    if (g->ran[slot]) {
        memcpy(g->stats, g->readback[slot].mapped, sizeof(g->stats));
        g->haveStats = true;
        g->ran[slot] = false;
    }
    if (!g->on) return;
    const TerrainPush *push;
    int drawn = terrain_gpu_levels(r, &push);
    if (drawn >= TERRAIN_CLIP_LEVELS) return;

    GrassFrameGPU *f = (GrassFrameGPU *)g->frame[slot].mapped;
    memset(f, 0, sizeof(*f));
    f->info[0] = drawn;
    f->info[1] = GRASS_PATCH_GRID;
    /* f_y, as the terrain's K: half the frame's height times viewProj's second row (1 / tan(fovy / 2) times up) */
    const float *m = viewProj->m;
    f->view[0] = (float)GRASS_RADIUS;
    f->view[1] = (float)(0.5 * r->vk->extent.height * sqrt((double)m[1] * m[1] + (double)m[5] * m[5] + (double)m[9] * m[9]));
    for (int b = 0; b < GRASS_BUCKETS; ++b) { f->cap[b] = g->cap[b]; f->base[b] = g->base[b]; }
    for (int l = drawn; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainPush *p = &push[l];
        GrassLevelGPU *o = &f->lv[l];
        o->a[0] = p->anchor[0]; o->a[1] = p->anchor[1]; o->a[2] = p->morph[0]; o->a[3] = p->morph[1];
        o->b[0] = p->offset[0]; o->b[1] = p->offset[1]; o->b[2] = p->spacing; o->b[3] = p->fade;
        o->c[0] = p->slot[0]; o->c[1] = p->slot[1]; o->c[2] = p->pslot[0]; o->c[3] = p->pslot[1];
        o->d[0] = p->level; o->d[1] = p->top;
    }

    const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (g->keepToFresh) {   /* nothing given way yet */
        float far = 1e9f;
        uint32_t bits;
        memcpy(&bits, &far, sizeof(bits));
        vkCmdFillBuffer(cmd, g->keepTo.buf, 0, VK_WHOLE_SIZE, bits);
        g->keepToFresh = false;
    }
    vkCmdFillBuffer(cmd, g->counters[slot].buf, 0, VK_WHOLE_SIZE, 0);
    /* the clears; the terrain's upload of its samples; the last frame's finish writing keepTo */
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT | VK_PIPELINE_STAGE_2_COPY_BIT | CS,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->compPL, 0, 1, &g->compSet[slot], 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->preparePipe);
    vkCmdDispatch(cmd, GRASS_PATCH_GRID, GRASS_PATCH_GRID, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS, RW);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->finishPipe);
    vkCmdDispatch(cmd, 1, 1, 1);
    vk_memory_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferCopy bc = { 0, 0, GRASS_C_WORDS * sizeof(uint32_t) };
    vkCmdCopyBuffer(cmd, g->counters[slot].buf, g->readback[slot].buf, 1, &bc);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    g->ran[slot] = true;
    g->drawing = true;
}

void grass_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    GrassGpu *g = r->grass;
    if (!g->drawing) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->bladePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->drawPL, 0, 1, &g->drawSet[slot], 0, NULL);
    for (int b = 0; b < GRASS_BUCKETS; ++b) {
        BladePush pc = { g->base[b], b };
        vkCmdPushConstants(cmd, g->drawPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndirect(cmd, g->draws[slot].buf, (VkDeviceSize)b * sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
    }
}

void renderer_grass_stats(Renderer *r, char *buf, size_t n) {
    GrassGpu *g = r->grass;
    if (!g->on) { snprintf(buf, n, "grass: %s", g->supported ? "off (--nograss)" : "off (the GPU's subgroups can't add up)"); return; }
    if (!g->haveStats) { snprintf(buf, n, "grass: no counts back yet"); return; }
    const uint32_t *s = g->stats;
    static const char *const NAMES[GRASS_BUCKETS] = { "full", "mid", "clump" };
    int k = snprintf(buf, n, "grass (counts from %d frames before): patches %d tested, %u in range, %u in view, %u where grass may grow;\n"
                     "       roots %u candidates, %u evaluated, %u kept;", MAX_FRAMES, GRASS_PATCH_GRID * GRASS_PATCH_GRID,
                     s[GRASS_C_PATCHES_RANGE], s[GRASS_C_PATCHES_VIEW], s[GRASS_C_PATCHES_GRASS], s[GRASS_C_CANDIDATES],
                     s[GRASS_C_EVALUATED], s[GRASS_C_KEPT]);
    uint64_t blades = 0;
    for (int b = 0; b < GRASS_BUCKETS && k > 0 && (size_t)k < n; ++b) {
        float to;
        memcpy(&to, &s[GRASS_C_KEEP_TO + b], sizeof(to));
        char keep[32] = "all";
        if (to < 1e8f) snprintf(keep, sizeof(keep), "to %.0f m", to);
        k += snprintf(buf + k, n - k, "\n       %-5s %u blades drawn of %u room (%u past the room, %u past the distance kept to; next kept %s)",
                      NAMES[b], s[GRASS_C_DRAWN + b], g->cap[b], s[GRASS_C_OVER + b], s[GRASS_C_CUT + b], keep);
        blades += s[GRASS_C_DRAWN + b];
    }
    if (k > 0 && (size_t)k < n)
        snprintf(buf + k, n - k, "\n       %llu blades, %llu triangles", (unsigned long long)blades, (unsigned long long)(2 * blades));
}
