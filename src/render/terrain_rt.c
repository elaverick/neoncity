#include "renderer_internal.h"
#include "terrain.h"

/* The terrain in the ray-traced shadows (M11a): the clipmap's levels near the
 * camera as bottom-level structures in the TLAS (RT_MASK_TERRAIN), so the
 * hills, cuttings and embankments shadow the city's lamps.
 *
 * Each level has one structure over its whole window (terrain_gpu.c's window
 * index list), its vertex (i, j) at (i, j) x spacing from the window's corner
 * and the instance placing the corner. terrain_rt.comp writes the heights
 * from the clipmap's samples as terrain.vert draws them, so when the window
 * moves only the heights change, and the structure is refit in place rather
 * than built (built again every REBUILD refits, when most of its samples are
 * new, or when it comes back into use). The finer level's window, but for its
 * boundary, is sunk rather than left out (a refit can't drop triangles). The
 * ground under the city stays in: it is drawn sunk under the streets
 * (terrain_urban_sink), and urban weight 1 reaches well past the built edge
 * (the belt is measured from the edge's contour), over the very hills that
 * cut the city off.
 *
 * The levels are the finest drawn out to the first whose window reaches
 * RT_RANGE from its centre (or just the finest drawn, if that is coarser), so
 * every caster a lamp within SHADOW_FAR can be shadowed by is in. A level is
 * refit when its window moves, its samples change, the finer window in it
 * moves, its fade changes, or the camera its blend is made round has moved a
 * quarter of a sample since. The frames in flight may still trace it: the
 * writes wait for their shaders.
 *
 * NEONCITY_TERRAIN_RT=0 in the environment leaves the terrain out of the
 * TLAS, to compare captures and costs. */

static const uint32_t SPV_terrain_rt[] = {
#include "terrain/terrain_rt.comp.inc"
};

#define LEVELS  TERRAIN_CLIP_LEVELS
#define W       TERRAIN_CLIP_W
#define VERTS   (W * W)
#define HOLE    ((W - 1) / 2)
#define SINK    50.0f    /* m: under any drawn surface a shadow ray could leave */
#define REBUILD 64       /* refits before a build restores the structure's quality */

/* shaders/terrain/terrain_rt.comp: Level */
typedef struct TerrainRtPush {
    float    morph[2], offset[2], spacing;
    int32_t  level, slot[2], hole[2];
    float    fade, sink;
    uint32_t base;
} TerrainRtPush;

typedef struct RtLevel {
    int      blas;        /* -1 until made */
    int      built;       /* built at least once since it came into use */
    int      refits;      /* since its last build */
    int64_t  origin[2];   /* the window as last written */
    int32_t  hole[2];
    float    fade;
    double   cam[2];      /* the camera its blend was made round */
    TerrainRtPush push;   /* as last written (the check's) */
} RtLevel;

struct TerrainRt {
    VkDescriptorSetLayout layout;
    VkPipelineLayout pl;
    VkPipeline       pipe;
    VkDescriptorPool pool;
    VkDescriptorSet  set;
    Buffer           verts;   /* LEVELS x VERTS x 3 floats */
    VkDeviceAddress  idx;     /* the window's index list */
    uint32_t         nidx;
    RtLevel          lv[LEVELS];
    int              first, last, top;   /* the levels in this frame's TLAS (first > last: none); the RT_RANGE level */
    int              reach;              /* the coarsest level added to this frame's TLAS, -1 for none */
    int              refits, builds;     /* the last frame's */
    long             totalRefits, totalBuilds, missed;
    int              off;                /* NEONCITY_TERRAIN_RT=0 */
};

TerrainRt *terrain_rt_create(Renderer *r, VkBuffer samples, VkDeviceSize sampleBytes, VkBuffer indices, VkDeviceSize firstIdx, uint32_t nidx) {
    if (!r->rt) return NULL;
    VkCore *vk = r->vk;
    TerrainRt *tr = (TerrainRt *)calloc(1, sizeof(TerrainRt));
    if (!tr) fatal("out of memory allocating the terrain's ray tracing");
    for (int l = 0; l < LEVELS; ++l) tr->lv[l].blas = -1;
    tr->first = 1;
    tr->last = 0;
    tr->top = LEVELS - 1;
    tr->reach = -1;
    const char *env = getenv("NEONCITY_TERRAIN_RT");
    tr->off = env && atoi(env) == 0;
    if (tr->off) printf("Terrain in the ray-traced shadows: off (NEONCITY_TERRAIN_RT=0)\n");
    for (int l = 0; l < LEVELS; ++l)
        if (HOLE * TERRAIN_CLIP_SPACING0 * (double)(1 << l) >= RT_RANGE) { tr->top = l; break; }

    const VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const VkShaderStageFlags C = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutBinding b[2] = { { 0, SB, 1, C, NULL }, { 1, SB, 1, C, NULL } };
    tr->layout = make_set_layout(vk, b, 2);
    tr->pl = make_pipeline_layout(vk, tr->layout, sizeof(TerrainRtPush), C);
    tr->pipe = make_compute(vk, SPV_terrain_rt, sizeof(SPV_terrain_rt), tr->pl);
    VkDescriptorPoolSize ps = { SB, 2 };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &tr->pool));
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = tr->pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &tr->layout;
    VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &tr->set));

    const VkDeviceSize vbytes = (VkDeviceSize)LEVELS * VERTS * 3 * sizeof(float);
    tr->verts = vk_buffer(vk, vbytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    write_buf_set(vk, tr->set, 0, SB, samples, sampleBytes);
    write_buf_set(vk, tr->set, 1, SB, tr->verts.buf, vbytes);
    tr->idx = vk_buffer_address(vk, indices) + firstIdx * sizeof(uint16_t);
    tr->nidx = nidx;
    return tr;
}

void terrain_rt_destroy(Renderer *r, TerrainRt *tr) {
    if (!tr) return;
    VkCore *vk = r->vk;
    for (int l = 0; l < LEVELS; ++l) rt_free_blas(r->rt, tr->lv[l].blas, r->frameIndex);
    vkDestroyPipeline(vk->device, tr->pipe, NULL);
    vkDestroyPipelineLayout(vk->device, tr->pl, NULL);
    vkDestroyDescriptorPool(vk->device, tr->pool, NULL);
    vkDestroyDescriptorSetLayout(vk->device, tr->layout, NULL);
    vk_buffer_destroy(vk, &tr->verts);
    free(tr);
}

void terrain_rt_update(Renderer *r, TerrainRt *tr, VkCommandBuffer cmd, int slot, const TerrainClipmap *shown,
                       const TerrainPush *push, int drawn, const double cam[3], const int *dirty) {
    if (!tr) return;
    tr->refits = tr->builds = 0;
    tr->reach = -1;
    int first = drawn, last = drawn > tr->top ? drawn : tr->top;
    if (!r->shadowsOn || tr->off || first >= LEVELS) { first = 1; last = 0; }
    if (last >= LEVELS) last = LEVELS - 1;
    for (int l = 0; l < LEVELS; ++l)   /* out of use: built afresh when it comes back */
        if (l < first || l > last) tr->lv[l].built = 0;
    tr->first = first;
    tr->last = last;
    if (first > last) return;

    VkDeviceAddress vaddr = vk_buffer_address(r->vk, tr->verts.buf);
    int work[LEVELS], full[LEVELS], nw = 0;
    for (int l = first; l <= last; ++l) {
        RtLevel *L = &tr->lv[l];
        const TerrainClipLevel *lv = &shown->level[l];
        int32_t hole[2] = { -1, -1 };
        if (l > first) {
            const TerrainClipLevel *fine = &shown->level[l - 1];
            hole[0] = (int32_t)(fine->origin[0] / 2 - lv->origin[0]);
            hole[1] = (int32_t)(fine->origin[1] / 2 - lv->origin[1]);
        }
        const TerrainPush *p = &push[l];
        double q = 0.25 * lv->spacing;
        int moved = L->origin[0] != lv->origin[0] || L->origin[1] != lv->origin[1] || L->hole[0] != hole[0] || L->hole[1] != hole[1] ||
                    L->fade != p->fade || dirty[l] || fabs(L->cam[0] - shown->cam[0]) > q || fabs(L->cam[1] - shown->cam[1]) > q;
        if (L->blas < 0) {
            RtMesh m = { vaddr + (VkDeviceAddress)l * VERTS * 3 * sizeof(float), tr->idx, 3 * sizeof(float), VERTS, tr->nidx };
            L->blas = rt_create_mesh(r->rt, &m);
            if (L->blas < 0) { tr->missed++; continue; }
        }
        if (L->built && !moved) continue;
        TerrainRtPush *rp = &L->push;
        rp->morph[0] = p->morph[0];
        rp->morph[1] = p->morph[1];
        rp->offset[0] = p->offset[0];
        rp->offset[1] = p->offset[1];
        rp->spacing = p->spacing;
        rp->level = l;
        rp->slot[0] = p->slot[0];
        rp->slot[1] = p->slot[1];
        rp->hole[0] = hole[0];
        rp->hole[1] = hole[1];
        rp->fade = p->fade;
        rp->sink = SINK;
        rp->base = (uint32_t)(l * VERTS);
        L->origin[0] = lv->origin[0];
        L->origin[1] = lv->origin[1];
        L->hole[0] = hole[0];
        L->hole[1] = hole[1];
        L->fade = p->fade;
        L->cam[0] = shown->cam[0];
        L->cam[1] = shown->cam[1];
        full[nw] = !L->built || L->refits >= REBUILD || dirty[l] > VERTS / 2;
        work[nw++] = l;
    }

    if (nw) {
        const VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, FS = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        const VkPipelineStageFlags2 BUILD = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
        /* the samples just copied in, and the earlier builds' reads of the vertices */
        vk_memory_barrier(cmd, COPY | BUILD, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr->pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tr->pl, 0, 1, &tr->set, 0, NULL);
        for (int k = 0; k < nw; ++k) {
            vkCmdPushConstants(cmd, tr->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TerrainRtPush), &tr->lv[work[k]].push);
            vkCmdDispatch(cmd, (W + 7) / 8, (W + 7) / 8, 1);
        }
        /* the vertices into the builds, which also wait for the frames in flight still tracing the structures */
        vk_memory_barrier(cmd, CS | FS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                          BUILD, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        for (int k = 0; k < nw; ++k) {
            RtLevel *L = &tr->lv[work[k]];
            RtMesh m = { vaddr + (VkDeviceAddress)work[k] * VERTS * 3 * sizeof(float), tr->idx, 3 * sizeof(float), VERTS, tr->nidx };
            if (!rt_update_mesh(r->rt, cmd, slot, L->blas, &m, full[k])) {   /* no scratch left: next frame */
                L->origin[0] = INT64_MIN;
                tr->missed++;
                continue;
            }
            if (full[k]) { L->built = 1; L->refits = 0; tr->builds++; tr->totalBuilds++; }
            else         { L->refits++; tr->refits++; tr->totalRefits++; }
        }
    }

    for (int l = first; l <= last; ++l) {
        const RtLevel *L = &tr->lv[l];
        if (!L->built) continue;
        tr->reach = l;
        const TerrainClipLevel *lv = &shown->level[l];
        float o[3] = { (float)((double)lv->origin[0] * lv->spacing - cam[0]), -(float)cam[1],
                       (float)((double)lv->origin[1] * lv->spacing - cam[2]) };
        rt_add_instance(r->rt, slot, L->blas, o, RT_MASK_TERRAIN);
    }
}

/* The check: each level in the TLAS read back and set against the CPU's
 * drawn surface (terrain_clip_morph's blend of the clipmap's own samples,
 * round the camera the level was written for) and the sinks. Only while the clipmap is as the GPU last had it: the worker
 * idle and its update taken. Stalls the GPU. */
static void check_levels(Renderer *r, TerrainRt *tr, const TerrainClipmap *clip, char *out, size_t n) {
    VkCore *vk = r->vk;
    VK_CHECK(vkDeviceWaitIdle(vk->device));
    const VkDeviceSize vbytes = tr->verts.size;
    Buffer rb = vk_buffer(vk, vbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkBufferCopy bc = { 0, 0, vbytes };
    vkCmdCopyBuffer(cmd, tr->verts.buf, rb.buf, 1, &bc);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkFreeCommandBuffers(vk->device, r->cmdPool, 1, &cmd);

    const float *v = (const float *)rb.mapped;
    double worst = 0.0, worstXZ = 0.0;
    long bad = 0, sunkHole = 0, total = 0;
    for (int l = tr->first; l <= tr->last; ++l) {
        const RtLevel *L = &tr->lv[l];
        const TerrainClipLevel *lv = &clip->level[l];
        if (!L->built || !lv->ready || L->origin[0] != lv->origin[0] || L->origin[1] != lv->origin[1]) { bad++; continue; }
        for (int j = 0; j < W; ++j)
            for (int i = 0; i < W; ++i) {
                const float *p = v + ((size_t)l * VERTS + (size_t)j * W + i) * 3;
                const TerrainSample *s = terrain_clip_sample(lv, lv->origin[0] + i, lv->origin[1] + j);
                double x = (double)(lv->origin[0] + i) * lv->spacing, z = (double)(lv->origin[1] + j) * lv->spacing;
                /* terrain_clip_morph's, round the camera the level was written for (it can trail the clipmap's by a quarter sample) */
                double d = fmax(fabs(x - L->cam[0]), fabs(z - L->cam[1])) / lv->spacing, end = HOLE - 1;
                float m = clampf((float)((d - (end - TERRAIN_CLIP_MORPH)) / TERRAIN_CLIP_MORPH), 0.0f, 1.0f);
                if (m < L->fade) m = L->fade;
                double want = s->h + m * (s->hc - s->h);
                int hole = L->hole[0] >= 0 && i > L->hole[0] && i < L->hole[0] + HOLE && j > L->hole[1] && j < L->hole[1] + HOLE;
                if (hole) want -= SINK;
                sunkHole += hole;
                double e = fabs(p[1] - want), exz = fmax(fabs(p[0] - i * lv->spacing), fabs(p[2] - j * lv->spacing));
                if (e > worst) worst = e;
                if (exz > worstXZ) worstXZ = exz;
                if (e > 0.01 || exz > 1e-3) bad++;
                total++;
            }
    }
    vk_buffer_destroy(vk, &rb);
    snprintf(out, n, "; check: %ld vertices, worst height %.4f m, x/z %.4f m, %ld off by more than 1 cm%s (%ld sunk under a finer level)",
             total, worst, worstXZ, bad, bad ? " FAILED" : "", sunkHole);
}

void terrain_rt_stats(Renderer *r, TerrainRt *tr, const TerrainClipmap *clip, bool current, char *buf, size_t n) {
    if (!tr) { snprintf(buf, n, "terrain in the TLAS: no ray tracing"); return; }
    RtStats rs = rt_stats(r->rt);
    int used = snprintf(buf, n, "terrain in the TLAS: ");
    if (tr->first > tr->last) used += snprintf(buf + used, n - used, "none");
    else used += snprintf(buf + used, n - used, "levels %d-%d", tr->first, tr->last);
    used += snprintf(buf + used, n - used, " (to level %d for RT_RANGE %.0f m), last frame %d refit and %d built; in all %ld refits, %ld builds, %ld missed; arena %.0f of %.0f MB",
                     tr->top, (double)RT_RANGE, tr->refits, tr->builds, tr->totalRefits, tr->totalBuilds, tr->missed, rs.arenaUsedMB, rs.arenaMB);
    if (used < 0 || (size_t)used >= n) return;
    if (tr->first > tr->last) return;
    if (!current) { snprintf(buf + used, n - used, "; check skipped (an update not yet taken)"); return; }
    check_levels(r, tr, clip, buf + used, n - used);
}

int terrain_rt_reach(const TerrainRt *tr) { return tr ? tr->reach : -1; }
