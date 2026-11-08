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
 * Draw (in the scene pass after the terrain): an indexed draw per bucket,
 * its count the GPU's: each blade a curved ribbon (shaders/grass/blade.*)
 * of the bucket's segments, its topology from one index buffer holding them
 * all (GRASS_SEG_FIRST). The species' profiles (assets/grass/*.json,
 * src/grass/grass_species.h) are read here and go to the shaders in the
 * frame's uniforms.
 *
 * Each bucket's room is its own (GRASS_CAPS, or --grass-cap), so a far
 * bucket can't take a near one's; within one, finish.comp's distance keeps
 * the near blades and lets the far ones go.
 *
 * The city's grass (GR6): the chunks drawn this frame hand over their grass
 * patches within GRASS_RADIUS (chunks_city_grass: their parks', fields' and
 * edge verge's ground, cut along the patches by src/city/grass_city.c) into
 * the frame slot's host-visible records, and prepare_city.comp, a workgroup
 * a record, grows lawn and rough on them into the same buckets, before
 * finish.comp. Each blade's draw chunk goes beside it (instChunk), and the
 * blades are drawn with the scene's set (set 0: the chunks' records and
 * light tables, the shelter map, the shadow slots, the materials) beside
 * their own (set 1: the instances), so a city blade is lit as the city's
 * ground is (blade.frag). */

static const uint32_t SPV_prepare[] = {
#include "grass/prepare.comp.inc"
};
static const uint32_t SPV_prepare_city[] = {
#include "grass/prepare_city.comp.inc"
};
static const uint32_t SPV_blade_rt_frag[] = {
#include "grass/blade_rt.frag.inc"
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

typedef struct BladePush { uint32_t base; int32_t lod, segs; } BladePush;

struct GrassGpu {
    bool supported, on;
    uint32_t cap[GRASS_BUCKETS], base[GRASS_BUCKETS];
    VkDescriptorSetLayout compLayout, drawLayout;
    VkPipelineLayout compPL, drawPL;
    VkPipeline preparePipe, prepareCityPipe, finishPipe, bladePipe;
    VkDescriptorPool pool;
    VkDescriptorSet compSet[MAX_FRAMES], drawSet[MAX_FRAMES];
    Buffer frame[MAX_FRAMES];      /* GrassFrameGPU, host visible */
    Buffer instances[MAX_FRAMES];  /* the buckets' ranges, one after another */
    Buffer counters[MAX_FRAMES];   /* GRASS_C_WORDS */
    Buffer draws[MAX_FRAMES];      /* a VkDrawIndirectCommand per bucket */
    Buffer patches[MAX_FRAMES];    /* GRASS_PATCH_* per patch of the square */
    Buffer patchLight[MAX_FRAMES]; /* a uvec4 per patch: its freeway (shaders/grass/patch_light.glsl) */
    Buffer readback[MAX_FRAMES];   /* the counters, host visible */
    Buffer instChunk[MAX_FRAMES];  /* each instance's draw chunk (a city blade's) */
    Buffer cityPatch[MAX_FRAMES];  /* GrassCityPatchGPU, host visible (chunks_city_grass) */
    Buffer cityTri[MAX_FRAMES];    /* GrassCityTriGPU, host visible */
    uint32_t cityRecords;          /* the city's patch records this frame */
    Buffer keepTo;                 /* per bucket: the distance blades are kept to (m) */
    Buffer indices;                /* every ribbon topology, GRASS_SEG_FIRST(s) on (uint16) */
    int segs[GRASS_BUCKETS];       /* each bucket's (grass_bucket_segments) */
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
    int species = grass_profiles_load_assets(NULL);
    g->on = g->supported && species > 0;
    for (int b = 0; b < GRASS_BUCKETS; ++b) g->segs[b] = grass_bucket_segments(b);
    uint32_t total = 0;
    for (int b = 0; b < GRASS_BUCKETS; ++b) { g->cap[b] = GRASS_CAPS[b]; g->base[b] = total; total += GRASS_CAPS[b]; }
    printf("Grass: %s (%d species; room for %u blades a frame, %.0f MB a frame slot; segments %d, %d, %d)\n",
           g->on ? "on" : !g->supported ? "off (the GPU's subgroups can't add up)" : "off (no species' profiles)", species,
           total, (double)total * sizeof(GrassInstanceGPU) / (1024.0 * 1024.0), g->segs[0], g->segs[1], g->segs[2]);

    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT, V = VK_SHADER_STAGE_VERTEX_BIT, F = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorType UB = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutBinding comp[14] = {
        { 0, UB, 1, C, NULL },    /* FrameUBO */
        { 1, SB, 1, C, NULL },    /* the clipmap's samples */
        { 2, SB, 1, C, NULL },    /* the roads (road_gpu.c) */
        { 4, UB, 1, C, NULL },    /* GrassFrame */
        { 5, SB, 1, C, NULL },    /* the instances */
        { 6, SB, 1, C, NULL },    /* the counters */
        { 7, SB, 1, C, NULL },    /* the draws */
        { 9, SB, 1, C, NULL },    /* keepTo */
        { 10, SB, 1, C, NULL },   /* the patch states */
        { 13, SB, 1, C, NULL },   /* the city's patch records */
        { 14, SB, 1, C, NULL },   /* their triangles */
        { 15, SB, 1, C, NULL },   /* each instance's draw chunk */
        { 16, SB, 1, C, NULL },   /* the patches' freeways */
        { 17, UB, 1, C, NULL },   /* the caves' mouths' holes (cave_gpu.c) */
    };
    g->compLayout = make_set_layout(vk, comp, 14);
    VkDescriptorSetLayoutBinding draw[3] = {
        { 0, SB, 1, V, NULL },    /* the instances */
        { 1, SB, 1, V, NULL },    /* their draw chunks */
        { 2, SB, 1, V, NULL },    /* the patches' freeways */
    };
    g->drawLayout = make_set_layout(vk, draw, 3);
    g->compPL = make_pipeline_layout(vk, g->compLayout, 0, 0);
    {   /* set 0 the scene's (the frame, the chunks, the lights, the shelter map, the shadow slots, the materials), set 1 the blades' */
        VkDescriptorSetLayout sets[2] = { r->sceneLayout, g->drawLayout };
        VkPushConstantRange pr = { V, 0, sizeof(BladePush) };
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        li.setLayoutCount = 2;
        li.pSetLayouts = sets;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pr;
        VK_CHECK(vkCreatePipelineLayout(vk->device, &li, NULL, &g->drawPL));
    }
    g->preparePipe = make_compute(vk, SPV_prepare, sizeof(SPV_prepare), g->compPL);
    g->prepareCityPipe = make_compute(vk, SPV_prepare_city, sizeof(SPV_prepare_city), g->compPL);
    g->finishPipe = make_compute(vk, SPV_finish, sizeof(SPV_finish), g->compPL);
    /* with ray-traced shadows the scene's set has the slots, and a city blade takes its root's */
    GfxDesc blade = { SPV_blade_vert, sizeof(SPV_blade_vert), r->rt ? SPV_blade_rt_frag : SPV_blade_frag,
                      r->rt ? sizeof(SPV_blade_rt_frag) : sizeof(SPV_blade_frag),
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
        g->draws[i] = vk_buffer(vk, GRASS_BUCKETS * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, DEV);
        g->patches[i] = vk_buffer(vk, patchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEV);
        g->patchLight[i] = vk_buffer(vk, patchBytes * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEV);
        g->readback[i] = vk_buffer(vk, GRASS_C_WORDS * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST);
        g->instChunk[i] = vk_buffer(vk, (VkDeviceSize)total * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, DEV);
        g->cityPatch[i] = vk_buffer(vk, (VkDeviceSize)GRASS_CITY_PATCHES * sizeof(GrassCityPatchGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
        g->cityTri[i] = vk_buffer(vk, (VkDeviceSize)GRASS_CITY_TRIS * sizeof(GrassCityTriGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
    }
    g->keepTo = vk_buffer(vk, 4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, DEV);
    g->keepToFresh = true;
    /* the ribbons: s segments are rows 0 .. s - 1 of a left (2 r) and right (2 r + 1)
     * vertex and the tip (2 s); two triangles a segment, one at the tip */
    g->indices = vk_buffer(vk, GRASS_SEG_INDICES * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, HOST);
    uint16_t *ix = (uint16_t *)g->indices.mapped;
    for (int s = 1; s <= GRASS_SEG_MAX; ++s) {
        uint16_t *o = ix + GRASS_SEG_FIRST(s);
        for (int r = 0; r < s - 1; ++r) {
            uint16_t a = (uint16_t)(2 * r);
            *o++ = a; *o++ = (uint16_t)(a + 1); *o++ = (uint16_t)(a + 2);
            *o++ = (uint16_t)(a + 2); *o++ = (uint16_t)(a + 1); *o++ = (uint16_t)(a + 3);
        }
        uint16_t a = (uint16_t)(2 * (s - 1));
        *o++ = a; *o++ = (uint16_t)(a + 1); *o++ = (uint16_t)(a + 2);
    }

    VkDescriptorPoolSize sizes[2] = {
        { UB, 3 * MAX_FRAMES },
        { SB, 14 * MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 2 * MAX_FRAMES;
    pci.poolSizeCount = 2;
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
        write_buf_set(vk, s, 13, SB, g->cityPatch[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 14, SB, g->cityTri[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 15, SB, g->instChunk[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 16, SB, g->patchLight[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 17, UB, cave_gpu_holes(r, i), VK_WHOLE_SIZE);
        s = g->drawSet[i];
        write_buf_set(vk, s, 0, SB, g->instances[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 1, SB, g->instChunk[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, s, 2, SB, g->patchLight[i].buf, VK_WHOLE_SIZE);
        write_buf_set(vk, terrain_gpu_set(r, i), 10, SB, g->patches[i].buf, VK_WHOLE_SIZE);   /* --grass-debug cull */
    }
}

void grass_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    GrassGpu *g = r->grass;
    if (!g) return;
    vkDestroyPipeline(vk->device, g->preparePipe, NULL);
    vkDestroyPipeline(vk->device, g->prepareCityPipe, NULL);
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
        vk_buffer_destroy(vk, &g->patchLight[i]);
        vk_buffer_destroy(vk, &g->readback[i]);
        vk_buffer_destroy(vk, &g->instChunk[i]);
        vk_buffer_destroy(vk, &g->cityPatch[i]);
        vk_buffer_destroy(vk, &g->cityTri[i]);
    }
    vk_buffer_destroy(vk, &g->keepTo);
    vk_buffer_destroy(vk, &g->indices);
    free(g);
    r->grass = NULL;
}

void renderer_set_grass(Renderer *r, bool on) {
    bool any = false;
    for (int k = 0; k < GRASS_SPECIES; ++k) any = any || grass_profile(k)->present;
    r->grass->on = on && r->grass->supported && any;
}

bool grass_gpu_drawn(Renderer *r) { return r->grass && r->grass->on; }

void renderer_set_grass_cap(Renderer *r, uint32_t cap) {
    for (int b = 0; b < GRASS_BUCKETS; ++b) r->grass->cap[b] = cap > 0 && cap < GRASS_CAPS[b] ? cap : GRASS_CAPS[b];
}

void grass_gpu_prepare(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj) {
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
    /* the city's grass round the camera, from the chunks this frame draws */
    uint32_t ntri = 0, records = chunks_city_grass(r->chunks, cam, GRASS_RADIUS, (GrassCityPatchGPU *)g->cityPatch[slot].mapped,
                                                   GRASS_CITY_PATCHES, (GrassCityTriGPU *)g->cityTri[slot].mapped, GRASS_CITY_TRIS, &ntri);
    if (drawn >= TERRAIN_CLIP_LEVELS && !records) return;

    GrassFrameGPU *f = (GrassFrameGPU *)g->frame[slot].mapped;
    memset(f, 0, sizeof(*f));
    f->info[0] = drawn;
    f->info[1] = GRASS_PATCH_GRID;
    /* f_y, as the terrain's K: half the frame's height times viewProj's second row (1 / tan(fovy / 2) times up) */
    const float *m = viewProj->m;
    f->view[0] = (float)GRASS_RADIUS;
    f->view[1] = (float)(0.5 * r->vk->extent.height * sqrt((double)m[1] * m[1] + (double)m[5] * m[5] + (double)m[9] * m[9]));
    for (int b = 0; b < GRASS_BUCKETS; ++b) { f->cap[b] = g->cap[b]; f->base[b] = g->base[b]; f->seg[b] = g->segs[b]; }
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
    if (drawn < TERRAIN_CLIP_LEVELS) {   /* the land's */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->preparePipe);
        vkCmdDispatch(cmd, GRASS_PATCH_GRID, GRASS_PATCH_GRID, 1);
    }
    if (records) {   /* the city's, into the same buckets (their reservations are atomic) */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->prepareCityPipe);
        vkCmdDispatch(cmd, records, 1, 1);
    }
    g->cityRecords = records;
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
    VkDescriptorSet sets[2] = { r->sceneSet[slot], g->drawSet[slot] };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->drawPL, 0, 2, sets, 0, NULL);
    vkCmdBindIndexBuffer(cmd, g->indices.buf, 0, VK_INDEX_TYPE_UINT16);
    for (int b = 0; b < GRASS_BUCKETS; ++b) {
        BladePush pc = { g->base[b], b, g->segs[b] };
        vkCmdPushConstants(cmd, g->drawPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexedIndirect(cmd, g->draws[slot].buf, (VkDeviceSize)b * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
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
    if (k > 0 && (size_t)k < n)
        k += snprintf(buf + k, n - k, "\n       the city's: %u patch records in range, %u in view, %u roots kept (this frame handed over %u records)",
                      s[GRASS_C_CITY_RANGE], s[GRASS_C_CITY_VIEW], s[GRASS_C_CITY_KEPT], g->cityRecords);
    uint64_t blades = 0, tris = 0;
    for (int b = 0; b < GRASS_BUCKETS && k > 0 && (size_t)k < n; ++b) {
        float to;
        memcpy(&to, &s[GRASS_C_KEEP_TO + b], sizeof(to));
        char keep[32] = "all";
        if (to < 1e8f) snprintf(keep, sizeof(keep), "to %.0f m", to);
        k += snprintf(buf + k, n - k, "\n       %-5s %u blades drawn of %u room (%u past the room, %u past the distance kept to; next kept %s)",
                      NAMES[b], s[GRASS_C_DRAWN + b], g->cap[b], s[GRASS_C_OVER + b], s[GRASS_C_CUT + b], keep);
        blades += s[GRASS_C_DRAWN + b];
        tris += (uint64_t)s[GRASS_C_DRAWN + b] * (uint64_t)(2 * g->segs[b] - 1);
    }
    if (k > 0 && (size_t)k < n)
        k += snprintf(buf + k, n - k, "\n       %llu blades, %llu triangles", (unsigned long long)blades, (unsigned long long)tris);
    /* the effective density over the ground each species was evaluated on (every
     * kept root's tuft, as if all were drawn in full): the profile's density x the
     * suitability and the species' share there, on average */
    for (int sp = 0; sp < GRASS_SPECIES && k > 0 && (size_t)k < n; ++sp) {
        const GrassProfile *p = grass_profile(sp);
        if (!p->present || s[GRASS_C_SP_EVAL + sp] == 0) continue;
        double cell = GRASS_PATCH_M / (double)p->rootGrid;
        double area = (double)s[GRASS_C_SP_EVAL + sp] * cell * cell;
        k += snprintf(buf + k, n - k, "\n       %-6s %.1f blades/m2 over the %.0f m2 evaluated (its profile's %.0f where every rule says yes)",
                      GRASS_SPECIES_NAMES[sp], s[GRASS_C_SP_BLADES + sp] / area, area, p->density);
    }
}
