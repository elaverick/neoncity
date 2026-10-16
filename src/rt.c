#include "rt.h"

#define RT_ARENA_BYTES    (768ull << 20)  /* every BLAS, compacted (about 60 bytes a triangle) */
#define RT_MAX_BLAS       4096
#define RT_MAX_INSTANCES  4096
#define RT_INDEX_BYTES    (8u << 20)      /* per frame in flight: the index lists of its builds */
#define RT_SCRATCH_BYTES  (128ull << 20)  /* per frame in flight: build scratch */
#define RT_MAX_BUILDS     256             /* per frame: one compacted-size query each */
#define RT_MAX_FREE       8192            /* free blocks in the arena */
#define RT_AS_ALIGN       256ull          /* acceleration structure offsets */

typedef struct Range { uint64_t off, size; } Range;

typedef struct Blas {
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
    uint64_t off, size;   /* in the arena */
    int      live;
} Blas;

/* A structure no longer used by new frames, destroyed once the frame that retired it has finished. */
typedef struct Retired { VkAccelerationStructureKHR as; uint64_t off, size, frame; } Retired;

/* A BLAS built in this slot's frame, waiting for its compacted size (query q). */
typedef struct Pending { int h; uint32_t q; } Pending;

struct Rt {
    VkCore *vk;
    Buffer   arena;
    Range    freeList[RT_MAX_FREE];   /* sorted by offset, never adjacent */
    int      nfree;
    uint64_t used;
    bool     arenaFullWarned;

    Blas     blas[RT_MAX_BLAS];
    int      freeHandles[RT_MAX_BLAS];
    int      nfreeHandles, nlive;
    Retired *retired;
    int      nretired, capRetired;

    /* per frame in flight */
    Buffer      idx[MAX_FRAMES];
    uint32_t    idxUsed[MAX_FRAMES];
    Buffer      scratch[MAX_FRAMES];
    uint64_t    scratchUsed[MAX_FRAMES];
    VkQueryPool query[MAX_FRAMES];
    Pending     pending[MAX_FRAMES][RT_MAX_BUILDS];
    int         npending[MAX_FRAMES];
    uint32_t    nextQuery[MAX_FRAMES];
    Buffer      inst[MAX_FRAMES];
    uint32_t    ninst[MAX_FRAMES];
    Buffer      tlasBuf[MAX_FRAMES], tlasScratch[MAX_FRAMES];
    VkAccelerationStructureKHR tlas[MAX_FRAMES];
    uint32_t    lastInstances;
    int         builds;
};

/* ---------------------------------------------------------------- arena --- */

static bool arena_alloc(Rt *rt, uint64_t size, uint64_t *off) {
    size = (size + RT_AS_ALIGN - 1) & ~(RT_AS_ALIGN - 1);
    for (int i = 0; i < rt->nfree; ++i) {
        Range *r = &rt->freeList[i];
        if (r->size < size) continue;
        *off = r->off;
        r->off += size;
        r->size -= size;
        if (!r->size) {
            memmove(r, r + 1, sizeof(Range) * (rt->nfree - i - 1));
            rt->nfree--;
        }
        rt->used += size;
        return true;
    }
    return false;
}

static void arena_free(Rt *rt, uint64_t off, uint64_t size) {
    size = (size + RT_AS_ALIGN - 1) & ~(RT_AS_ALIGN - 1);
    rt->used -= size;
    int i = 0;
    while (i < rt->nfree && rt->freeList[i].off < off) ++i;
    bool joinPrev = i > 0 && rt->freeList[i - 1].off + rt->freeList[i - 1].size == off;
    bool joinNext = i < rt->nfree && off + size == rt->freeList[i].off;
    if (joinPrev && joinNext) {
        rt->freeList[i - 1].size += size + rt->freeList[i].size;
        memmove(&rt->freeList[i], &rt->freeList[i + 1], sizeof(Range) * (rt->nfree - i - 1));
        rt->nfree--;
    } else if (joinPrev) {
        rt->freeList[i - 1].size += size;
    } else if (joinNext) {
        rt->freeList[i].off = off;
        rt->freeList[i].size += size;
    } else {
        if (rt->nfree == RT_MAX_FREE) return;   /* lost to fragmentation; never seen in practice */
        memmove(&rt->freeList[i + 1], &rt->freeList[i], sizeof(Range) * (rt->nfree - i));
        rt->freeList[i].off = off;
        rt->freeList[i].size = size;
        rt->nfree++;
    }
}

static VkAccelerationStructureKHR create_as(Rt *rt, VkBuffer buf, uint64_t off, uint64_t size,
                                            VkAccelerationStructureTypeKHR type, VkDeviceAddress *addr) {
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = buf;
    ci.offset = off;
    ci.size = size;
    ci.type = type;
    VkAccelerationStructureKHR as;
    VK_CHECK(vkCreateAccelerationStructureKHR(rt->vk->device, &ci, NULL, &as));
    if (addr) {
        VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
        ai.accelerationStructure = as;
        *addr = vkGetAccelerationStructureDeviceAddressKHR(rt->vk->device, &ai);
    }
    return as;
}

static void retire(Rt *rt, VkAccelerationStructureKHR as, uint64_t off, uint64_t size, uint64_t frame) {
    if (rt->nretired == rt->capRetired) {
        rt->capRetired = rt->capRetired ? rt->capRetired * 2 : 256;
        rt->retired = (Retired *)realloc(rt->retired, sizeof(Retired) * rt->capRetired);
        if (!rt->retired) fatal("out of memory retiring acceleration structures");
    }
    Retired r = { as, off, size, frame };
    rt->retired[rt->nretired++] = r;
}

/* --------------------------------------------------------------- create --- */

Rt *rt_create(VkCore *vk) {
    if (!vk->rt) return NULL;
    Rt *rt = (Rt *)calloc(1, sizeof(Rt));
    if (!rt) fatal("out of memory");
    rt->vk = vk;
    const VkBufferUsageFlags ADDR = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags INPUT = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | ADDR;
    const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    rt->arena = vk_buffer(vk, RT_ARENA_BYTES, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | ADDR,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    rt->freeList[0].off = 0;
    rt->freeList[0].size = RT_ARENA_BYTES;
    rt->nfree = 1;
    for (int i = 0; i < RT_MAX_BLAS; ++i) rt->freeHandles[i] = RT_MAX_BLAS - 1 - i;
    rt->nfreeHandles = RT_MAX_BLAS;

    /* The TLAS is sized once for the most instances it can hold. */
    VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &g;
    uint32_t maxInst = RT_MAX_INSTANCES;
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxInst, &sz);

    for (int i = 0; i < MAX_FRAMES; ++i) {
        rt->idx[i] = vk_buffer(vk, RT_INDEX_BYTES, INPUT, HOST);
        rt->scratch[i] = vk_buffer(vk, RT_SCRATCH_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | ADDR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        rt->inst[i] = vk_buffer(vk, sizeof(VkAccelerationStructureInstanceKHR) * RT_MAX_INSTANCES, INPUT, HOST);
        rt->tlasBuf[i] = vk_buffer(vk, sz.accelerationStructureSize,
                                   VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | ADDR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        rt->tlasScratch[i] = vk_buffer(vk, sz.buildScratchSize + vk->asProps.minAccelerationStructureScratchOffsetAlignment,
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | ADDR, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        rt->tlas[i] = create_as(rt, rt->tlasBuf[i].buf, 0, sz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, NULL);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        qi.queryCount = RT_MAX_BUILDS;
        VK_CHECK(vkCreateQueryPool(vk->device, &qi, NULL, &rt->query[i]));
    }
    printf("Acceleration structures: %.0f MB arena\n", RT_ARENA_BYTES / (1024.0 * 1024.0));
    return rt;
}

void rt_destroy(Rt *rt) {
    if (!rt) return;
    VkDevice d = rt->vk->device;
    for (int i = 0; i < RT_MAX_BLAS; ++i)
        if (rt->blas[i].live) vkDestroyAccelerationStructureKHR(d, rt->blas[i].as, NULL);
    for (int i = 0; i < rt->nretired; ++i) vkDestroyAccelerationStructureKHR(d, rt->retired[i].as, NULL);
    free(rt->retired);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vkDestroyAccelerationStructureKHR(d, rt->tlas[i], NULL);
        vkDestroyQueryPool(d, rt->query[i], NULL);
        vk_buffer_destroy(rt->vk, &rt->idx[i]);
        vk_buffer_destroy(rt->vk, &rt->scratch[i]);
        vk_buffer_destroy(rt->vk, &rt->inst[i]);
        vk_buffer_destroy(rt->vk, &rt->tlasBuf[i]);
        vk_buffer_destroy(rt->vk, &rt->tlasScratch[i]);
    }
    vk_buffer_destroy(rt->vk, &rt->arena);
    free(rt);
}

/* ---------------------------------------------------------------- frame --- */

void rt_begin_frame(Rt *rt, VkCommandBuffer cmd, int slot, uint64_t frame) {
    VkDevice d = rt->vk->device;
    int w = 0;
    for (int i = 0; i < rt->nretired; ++i) {
        Retired *r = &rt->retired[i];
        if (frame >= r->frame + MAX_FRAMES) {
            vkDestroyAccelerationStructureKHR(d, r->as, NULL);
            arena_free(rt, r->off, r->size);
        } else {
            rt->retired[w++] = *r;
        }
    }
    rt->nretired = w;

    /* This slot's previous frame has finished: its builds' compacted sizes are known. */
    for (int i = 0; i < rt->npending[slot]; ++i) {
        Blas *b = &rt->blas[rt->pending[slot][i].h];
        uint64_t compact = 0;
        if (vkGetQueryPoolResults(d, rt->query[slot], rt->pending[slot][i].q, 1, sizeof(compact), &compact, sizeof(compact),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || !compact || compact >= b->size) continue;
        uint64_t off;
        if (!arena_alloc(rt, compact, &off)) continue;
        VkDeviceAddress addr;
        VkAccelerationStructureKHR as = create_as(rt, rt->arena.buf, off, compact, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &addr);
        VkCopyAccelerationStructureInfoKHR ci = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR };
        ci.src = b->as;
        ci.dst = as;
        ci.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(cmd, &ci);
        retire(rt, b->as, b->off, b->size, frame);   /* the copy reads it this frame */
        b->as = as; b->addr = addr; b->off = off; b->size = compact;
    }
    rt->npending[slot] = 0;
    rt->nextQuery[slot] = 0;
    vkCmdResetQueryPool(cmd, rt->query[slot], 0, RT_MAX_BUILDS);
    rt->idxUsed[slot] = 0;
    rt->scratchUsed[slot] = 0;
    rt->ninst[slot] = 0;
}

bool rt_can_build(Rt *rt, int slot, uint32_t indices) {
    return rt->idxUsed[slot] + indices * 2u + 64u * MAX_CHUNK_PAGES <= RT_INDEX_BYTES && rt->nextQuery[slot] < RT_MAX_BUILDS &&
           rt->nfreeHandles > 0;
}

int rt_build_blas(Rt *rt, VkCommandBuffer cmd, int slot, VkBuffer pool, const uint16_t *pages, int npages,
                  const uint32_t *pageVerts, const uint16_t *idx, const uint32_t *idxCount) {
    VkCore *vk = rt->vk;
    uint32_t total = 0;
    for (int p = 0; p < npages; ++p) total += idxCount[p];
    if (!total || !rt_can_build(rt, slot, total)) return -1;

    VkAccelerationStructureGeometryKHR geo[MAX_CHUNK_PAGES];
    VkAccelerationStructureBuildRangeInfoKHR range[MAX_CHUNK_PAGES];
    uint32_t maxPrims[MAX_CHUNK_PAGES];
    VkDeviceAddress poolAddr = vk_buffer_address(vk, pool), idxAddr = vk_buffer_address(vk, rt->idx[slot].buf);
    uint32_t off = rt->idxUsed[slot], ng = 0;
    const uint16_t *src = idx;
    for (int p = 0; p < npages; ++p) {
        if (!idxCount[p]) continue;
        off = (off + 3u) & ~3u;
        memcpy((uint8_t *)rt->idx[slot].mapped + off, src, idxCount[p] * sizeof(uint16_t));
        VkAccelerationStructureGeometryKHR *g = &geo[ng];
        memset(g, 0, sizeof(*g));
        g->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR *t = &g->geometry.triangles;
        t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t->vertexData.deviceAddress = poolAddr + (VkDeviceAddress)pages[p] * PAGE_VERTS * sizeof(Vertex);
        t->vertexStride = sizeof(Vertex);
        t->maxVertex = pageVerts[p] - 1;
        t->indexType = VK_INDEX_TYPE_UINT16;
        t->indexData.deviceAddress = idxAddr + off;
        maxPrims[ng] = idxCount[p] / 3;
        memset(&range[ng], 0, sizeof(range[ng]));
        range[ng].primitiveCount = maxPrims[ng];
        ng++;
        off += idxCount[p] * sizeof(uint16_t);
        src += idxCount[p];
    }

    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = ng;
    bi.pGeometries = geo;
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(vk->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, maxPrims, &sz);

    uint64_t align = vk->asProps.minAccelerationStructureScratchOffsetAlignment;
    uint64_t sOff = (rt->scratchUsed[slot] + align - 1) & ~(align - 1);
    if (sOff + sz.buildScratchSize > RT_SCRATCH_BYTES) return -1;
    uint64_t aOff;
    if (!arena_alloc(rt, sz.accelerationStructureSize, &aOff)) {
        if (!rt->arenaFullWarned) fprintf(stderr, "acceleration structure arena full: some chunks cast no shadows\n");
        rt->arenaFullWarned = true;
        return -1;
    }
    rt->scratchUsed[slot] = sOff + sz.buildScratchSize;
    rt->idxUsed[slot] = off;

    int h = rt->freeHandles[--rt->nfreeHandles];
    Blas *b = &rt->blas[h];
    b->as = create_as(rt, rt->arena.buf, aOff, sz.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &b->addr);
    b->off = aOff;
    b->size = sz.accelerationStructureSize;
    b->live = 1;
    rt->nlive++;

    bi.dstAccelerationStructure = b->as;
    bi.scratchData.deviceAddress = vk_buffer_address(vk, rt->scratch[slot].buf) + sOff;
    const VkAccelerationStructureBuildRangeInfoKHR *pr = range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);

    rt->builds++;
    Pending *pe = &rt->pending[slot][rt->npending[slot]++];
    pe->h = h;
    pe->q = rt->nextQuery[slot]++;
    return h;
}

void rt_free_blas(Rt *rt, int h, uint64_t frame) {
    if (h < 0) return;
    Blas *b = &rt->blas[h];
    if (!b->live) return;
    retire(rt, b->as, b->off, b->size, frame);
    memset(b, 0, sizeof(*b));
    rt->freeHandles[rt->nfreeHandles++] = h;
    rt->nlive--;
    for (int s = 0; s < MAX_FRAMES; ++s)
        for (int i = 0; i < rt->npending[s]; ++i)
            if (rt->pending[s][i].h == h) rt->pending[s][i--] = rt->pending[s][--rt->npending[s]];
}

void rt_add_instance(Rt *rt, int slot, int h, const float origin[3], uint32_t mask) {
    if (h < 0 || !rt->blas[h].live || rt->ninst[slot] >= RT_MAX_INSTANCES) return;
    VkAccelerationStructureInstanceKHR *in = (VkAccelerationStructureInstanceKHR *)rt->inst[slot].mapped + rt->ninst[slot]++;
    memset(in, 0, sizeof(*in));
    in->transform.matrix[0][0] = in->transform.matrix[1][1] = in->transform.matrix[2][2] = 1.0f;
    in->transform.matrix[0][3] = origin[0];
    in->transform.matrix[1][3] = origin[1];
    in->transform.matrix[2][3] = origin[2];
    in->mask = mask & 0xFFu;
    in->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
    in->accelerationStructureReference = rt->blas[h].addr;
}

void rt_build_tlas(Rt *rt, VkCommandBuffer cmd, int slot) {
    VkCore *vk = rt->vk;
    const VkPipelineStageFlags2 BUILD = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    /* BLAS builds and compaction copies, then their compacted sizes and the TLAS over them */
    vk_memory_barrier(cmd, BUILD, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      BUILD, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    for (int i = 0; i < rt->npending[slot]; ++i)
        vkCmdWriteAccelerationStructuresPropertiesKHR(cmd, 1, &rt->blas[rt->pending[slot][i].h].as,
                                                      VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                      rt->query[slot], rt->pending[slot][i].q);

    VkAccelerationStructureGeometryKHR g = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    g.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    g.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    g.geometry.instances.data.deviceAddress = vk_buffer_address(vk, rt->inst[slot].buf);
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &g;
    bi.dstAccelerationStructure = rt->tlas[slot];
    uint64_t align = vk->asProps.minAccelerationStructureScratchOffsetAlignment;
    bi.scratchData.deviceAddress = (vk_buffer_address(vk, rt->tlasScratch[slot].buf) + align - 1) & ~(align - 1);
    VkAccelerationStructureBuildRangeInfoKHR range = { 0 };
    range.primitiveCount = rt->ninst[slot];
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);
    vk_memory_barrier(cmd, BUILD, VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    rt->lastInstances = rt->ninst[slot];
}

VkAccelerationStructureKHR rt_tlas(Rt *rt, int slot) { return rt->tlas[slot]; }

RtStats rt_stats(Rt *rt) {
    RtStats s = { 0 };
    if (!rt) return s;
    s.blas = rt->nlive;
    s.instances = (int)rt->lastInstances;
    s.builds = rt->builds;
    s.arenaMB = RT_ARENA_BYTES / (1024.0 * 1024.0);
    s.arenaUsedMB = rt->used / (1024.0 * 1024.0);
    return s;
}
