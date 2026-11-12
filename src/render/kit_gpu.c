#include "kit_gpu.h"
#include "rt.h"
#include "thread.h"
#include "city.h"
#include "pmodel.h"

/* The vendors' kit drawn instanced: see kit_gpu.h. */

#define KIT_MAX_INST  65536   /* a frame's drawn instances at most (4 MB a frame in flight) */
#define KIT_GROUPS    (PMK_COUNT * PM_LODS)
#define KIT_L0_FAR    8.0f    /* m: L0 to here, */
#define KIT_L1_FAR    30.0f   /* L1 to here, L2 beyond (docs/noodle-bar-target.md) */
#define KIT_TRI_PX    0.15f   /* triangles a pixel a module's level may put on the screen (its bounding disc) */
#define KIT_PX_GONE   0.6f    /* a module whose radius covers fewer pixels than this is not drawn */
#define KIT_FADE_K    200.0f  /* a module fades out by KIT_FADE_K times its radius (KIT_FADE_MIN .. KIT_DISTANCE) */
#define KIT_FADE_MIN  6.0f
#define KIT_RT_LOD    PM_LOD1 /* the level the modules' BLASes are built over */

typedef struct KitGroup { uint32_t firstIndex, indexCount; int32_t vertexOffset; uint32_t nverts; } KitGroup;

struct KitGpu {
    VkCore  *vk;
    Rt      *rt;
    Buffer   verts, idx, staging;
    VkDeviceSize vertBytes, idxBytes;
    int      uploaded;
    uint64_t stagingFree;           /* the frame after which the staging buffer may go */
    KitGroup group[PMK_COUNT][PM_LODS];
    float    centre[PMK_COUNT][3], radius[PMK_COUNT], fade[PMK_COUNT];
    int      blas[PMK_COUNT];
    int      blasPending;           /* modules still to get a BLAS */

    Buffer   inst[MAX_FRAMES];      /* KitInstGPU, grouped (vertex buffer, per instance) */
    Buffer   cmds[MAX_FRAMES];      /* VkDrawIndexedIndirectCommand */
    uint32_t ncmd[MAX_FRAMES];

    /* this frame (kit_begin .. kit_end) */
    uint64_t frame;
    Frustum  fr;
    float    pixScale;              /* pixels a unit of tangent covers on the screen */
    KitInstGPU *cand;
    uint16_t   *key;
    uint32_t    ncand;
    KitStats    st;
};

/* ------------------------------------------------------------- library --- */

typedef struct BuildCtx { PmMesh *mesh; } BuildCtx;
static void build_one(void *ctx, int i) {
    BuildCtx *b = (BuildCtx *)ctx;
    pm_module_mesh(i / PM_LODS, i % PM_LODS, &b->mesh[i]);
}

static int8_t snorm8(float x) { return (int8_t)lroundf(clampf(x, -1.0f, 1.0f) * 127.0f); }

KitGpu *kit_create(VkCore *vk, Rt *rt) {
    KitGpu *k = (KitGpu *)calloc(1, sizeof(KitGpu));
    if (!k) fatal("out of memory");
    k->vk = vk;
    k->rt = rt;
    double t0 = glfwGetTime();
    PmMesh *mesh = (PmMesh *)calloc(KIT_GROUPS, sizeof(PmMesh));
    if (!mesh) fatal("out of memory");
    BuildCtx bc = { mesh };
    int threads = cpu_count();
    thread_parallel_for(KIT_GROUPS, threads < 1 ? 1 : threads, build_one, &bc);

    uint32_t nv = 0, ni = 0;
    for (int i = 0; i < KIT_GROUPS; ++i) {
        if (mesh[i].nv > 65535u) {
            fprintf(stderr, "kit module %s level %d has %u vertices\n", pm_module(i / PM_LODS)->name, i % PM_LODS, mesh[i].nv);
            fatal("a kit module has more vertices than 16-bit indices hold");
        }
        nv += mesh[i].nv;
        ni += mesh[i].ni;
    }
    k->vertBytes = (VkDeviceSize)nv * sizeof(Vertex);
    k->idxBytes = ((VkDeviceSize)ni * sizeof(uint16_t) + 3) & ~(VkDeviceSize)3;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    k->staging = vk_buffer(vk, k->vertBytes + k->idxBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
    Vertex *vd = (Vertex *)k->staging.mapped;
    uint16_t *id = (uint16_t *)((uint8_t *)k->staging.mapped + k->vertBytes);
    uint32_t v0 = 0, i0 = 0;
    for (int i = 0; i < KIT_GROUPS; ++i) {
        const PmMesh *m = &mesh[i];
        int mod = i / PM_LODS, lod = i % PM_LODS;
        KitGroup *g = &k->group[mod][lod];
        g->firstIndex = i0; g->indexCount = m->ni; g->vertexOffset = (int32_t)v0; g->nverts = m->nv;
        for (uint32_t v = 0; v < m->nv; ++v) {
            const PmVertex *p = &m->v[v];
            Vertex *o = &vd[v0 + v];
            memset(o, 0, sizeof(*o));
            memcpy(o->pos, p->p, sizeof(o->pos));
            o->nrm[0] = snorm8(p->n[0]); o->nrm[1] = snorm8(p->n[1]); o->nrm[2] = snorm8(p->n[2]);
            o->uv[0] = p->uv[0]; o->uv[1] = p->uv[1];
            o->params[0] = MAT_KIT; o->params[1] = p->mat; o->params[2] = p->occ; o->params[3] = p->wear;
            o->aux = p->part;
        }
        for (uint32_t t = 0; t < m->ni; ++t) id[i0 + t] = (uint16_t)m->idx[t];
        if (lod == PM_LOD0) {   /* the module's bounding sphere, from its finest level */
            float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
            for (uint32_t v = 0; v < m->nv; ++v)
                for (int a = 0; a < 3; ++a) { lo[a] = minf(lo[a], m->v[v].p[a]); hi[a] = maxf(hi[a], m->v[v].p[a]); }
            float r2 = 0.0f;
            for (int a = 0; a < 3; ++a) k->centre[mod][a] = m->nv ? 0.5f * (lo[a] + hi[a]) : 0.0f;
            for (uint32_t v = 0; v < m->nv; ++v) {
                float dx = m->v[v].p[0] - k->centre[mod][0], dy = m->v[v].p[1] - k->centre[mod][1], dz = m->v[v].p[2] - k->centre[mod][2];
                r2 = maxf(r2, dx * dx + dy * dy + dz * dz);
            }
            k->radius[mod] = sqrtf(r2);
            k->fade[mod] = clampf(KIT_FADE_K * k->radius[mod], KIT_FADE_MIN, KIT_DISTANCE);
        }
        if (lod == PM_LODS - 1) {   /* nor past the reach of its coarsest level with anything in it */
            static const float REACH[PM_LODS] = { KIT_L0_FAR, KIT_L1_FAR, KIT_DISTANCE };
            int last = PM_LOD0;
            for (int l = 0; l < PM_LODS; ++l) if (mesh[mod * PM_LODS + l].ni) last = l;
            k->fade[mod] = minf(k->fade[mod], REACH[last]);
        }
        v0 += m->nv;
        i0 += m->ni;
        k->st.libTris += (int)(m->ni / 3);
    }
    for (int i = 0; i < KIT_GROUPS; ++i) pm_mesh_free(&mesh[i]);
    free(mesh);

    VkBufferUsageFlags extra = vk->rt ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
    k->verts = vk_buffer(vk, k->vertBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    k->idx = vk_buffer(vk, k->idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | extra, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int s = 0; s < MAX_FRAMES; ++s) {
        k->inst[s] = vk_buffer(vk, sizeof(KitInstGPU) * KIT_MAX_INST, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host);
        k->cmds[s] = vk_buffer(vk, sizeof(VkDrawIndexedIndirectCommand) * KIT_GROUPS, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
    }
    k->cand = (KitInstGPU *)malloc(sizeof(KitInstGPU) * KIT_MAX_INST);
    k->key = (uint16_t *)malloc(sizeof(uint16_t) * KIT_MAX_INST);
    if (!k->cand || !k->key) fatal("out of memory");
    for (int m = 0; m < PMK_COUNT; ++m) {
        k->blas[m] = -1;
        if (rt && k->group[m][KIT_RT_LOD].indexCount >= 3 && k->radius[m] >= KIT_RT_RADIUS && pm_module_casts(m)) k->blasPending++;
    }
    k->st.modules = PMK_COUNT;
    k->st.levels = PM_LODS;
    k->st.libMB = (double)(k->vertBytes + k->idxBytes) / (1024.0 * 1024.0);
    k->st.buildMs = (glfwGetTime() - t0) * 1000.0;
    printf("Vendor kit: %d modules at %d levels, %d triangles, %.1f MB, built in %.0f ms on %d threads\n",
           PMK_COUNT, PM_LODS, k->st.libTris, k->st.libMB, k->st.buildMs, threads);
    return k;
}

void kit_destroy(KitGpu *k) {
    if (!k) return;
    vk_buffer_destroy(k->vk, &k->verts);
    vk_buffer_destroy(k->vk, &k->idx);
    if (k->staging.buf) vk_buffer_destroy(k->vk, &k->staging);
    for (int s = 0; s < MAX_FRAMES; ++s) {
        vk_buffer_destroy(k->vk, &k->inst[s]);
        vk_buffer_destroy(k->vk, &k->cmds[s]);
    }
    free(k->cand);
    free(k->key);
    free(k);
}

/* --------------------------------------------------------------- frame --- */

void kit_begin(KitGpu *k, VkCommandBuffer cmd, int slot, uint64_t frame, const mat4 *viewProj, uint32_t screenH) {
    (void)slot;
    k->frame = frame;
    if (!k->uploaded) {
        VkBufferCopy rv = { 0, 0, k->vertBytes }, ri = { k->vertBytes, 0, k->idxBytes };
        vkCmdCopyBuffer(cmd, k->staging.buf, k->verts.buf, 1, &rv);
        vkCmdCopyBuffer(cmd, k->staging.buf, k->idx.buf, 1, &ri);
        VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        if (k->rt) readers |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, readers,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
        k->uploaded = 1;
        k->stagingFree = frame + MAX_FRAMES;
    } else if (k->staging.buf && frame > k->stagingFree) {
        vk_buffer_destroy(k->vk, &k->staging);
    }
    if (k->rt && k->blasPending) {   /* the modules' shadow casters, as the frame's build budget allows */
        VkDeviceAddress va = vk_buffer_address(k->vk, k->verts.buf), ia = vk_buffer_address(k->vk, k->idx.buf);
        for (int m = 0; m < PMK_COUNT; ++m) {
            const KitGroup *g = &k->group[m][KIT_RT_LOD];
            if (k->blas[m] >= 0 || g->indexCount < 3 || k->radius[m] < KIT_RT_RADIUS || !pm_module_casts(m)) continue;   /* lights' glass and flames cast nothing */
            if (!rt_can_build(k->rt, slot, g->indexCount)) break;
            RtMesh rm = { va + (VkDeviceAddress)g->vertexOffset * sizeof(Vertex), ia + (VkDeviceAddress)g->firstIndex * sizeof(uint16_t),
                          (uint32_t)sizeof(Vertex), g->nverts, g->indexCount, VK_FORMAT_UNDEFINED };
            k->blas[m] = rt_build_mesh(k->rt, cmd, slot, &rm);
            if (k->blas[m] < 0) break;
            k->blasPending--;
            k->st.blas++;
        }
    }
    k->fr = frustum_from_matrix(*viewProj);
    /* the projection's y scale: viewProj's second row is the view's (orthonormal) up row scaled by it */
    float sy = sqrtf(viewProj->m[1] * viewProj->m[1] + viewProj->m[5] * viewProj->m[5] + viewProj->m[9] * viewProj->m[9]);
    k->pixScale = sy * 0.5f * (float)(screenH ? screenH : 1080u);
    k->ncand = 0;
    k->st.offered = 0;
    k->st.drawn[0] = k->st.drawn[1] = k->st.drawn[2] = 0;
    k->st.tris = 0;
    k->st.rtInstances = 0;
}

static float smooth01(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

void kit_add(KitGpu *k, int slot, const KitInst *inst, uint32_t n, const float origin[3], int rec) {
    for (uint32_t i = 0; i < n; ++i) {
        const KitInst *ki = &inst[i];
        int m = ki->module;
        if (m >= PMK_COUNT) continue;
        k->st.offered++;
        const float *x = ki->xf, *ce = k->centre[m];
        float sc = sqrtf(x[0] * x[0] + x[4] * x[4] + x[8] * x[8]);
        /* its module's frame to camera-relative; its sphere's centre there */
        float row[3][4], c[3];
        for (int r = 0; r < 3; ++r) {
            row[r][0] = x[r * 4 + 0]; row[r][1] = x[r * 4 + 1]; row[r][2] = x[r * 4 + 2];
            row[r][3] = x[r * 4 + 3] + origin[r];
            c[r] = row[r][0] * ce[0] + row[r][1] * ce[1] + row[r][2] * ce[2] + row[r][3];
        }
        float rad = k->radius[m] * sc, d = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (k->rt && k->blas[m] >= 0 && rad >= KIT_RT_RADIUS && d < KIT_RT_RANGE + rad) {
            rt_add_instance_xf(k->rt, slot, k->blas[m], (const float (*)[4])row, RT_MASK_CITY, RT_INST_CITY, true);
            k->st.rtInstances++;
        }
        if (rec < 0) continue;
        float fade = k->fade[m] * sc;
        if (d > fade) continue;
        float px = rad / maxf(d, 0.05f) * k->pixScale;
        if (px < KIT_PX_GONE) continue;
        if (!frustum_test_aabb(&k->fr, v3(c[0] - rad, c[1] - rad, c[2] - rad), v3(c[0] + rad, c[1] + rad, c[2] + rad))) continue;
        /* the finest level its distance allows, made coarser while it would put more than KIT_TRI_PX
         * triangles on each pixel the module covers and a coarser level has anything in it (a level left
         * empty, as rivets' past L0 and bolts' past L1, is not drawn) */
        int lod = d < KIT_L0_FAR ? PM_LOD0 : d < KIT_L1_FAR ? PM_LOD1 : PM_LOD2;
        float area = 3.14159265f * px * px;
        for (;;) {
            int next = lod + 1;
            while (next < PM_LODS && !k->group[m][next].indexCount) next++;
            if (next >= PM_LODS || (float)(k->group[m][lod].indexCount / 3) <= KIT_TRI_PX * area) break;
            lod = next;
        }
        if (k->group[m][lod].indexCount == 0) continue;
        if (k->ncand >= KIT_MAX_INST) continue;
        /* shrinking into its centre over the last quarter of its fade distance */
        float s = 1.0f - smooth01(0.75f * fade, fade, d);
        KitInstGPU *o = &k->cand[k->ncand];
        for (int r = 0; r < 3; ++r) {
            float lin = row[r][0] * ce[0] + row[r][1] * ce[1] + row[r][2] * ce[2];
            o->row[r][0] = row[r][0] * s; o->row[r][1] = row[r][1] * s; o->row[r][2] = row[r][2] * s;
            o->row[r][3] = row[r][3] + (1.0f - s) * lin;
        }
        o->seed = ki->seed;
        o->chunk = (uint32_t)rec;
        o->part = ki->part | (uint32_t)ki->flags << 8 | (uint32_t)(uint8_t)ki->grime << 24;   /* NB7: the cooking's grime, kit.vert */
        memcpy(&o->base, &ki->base, 4);
        k->key[k->ncand++] = (uint16_t)(m * PM_LODS + lod);
        k->st.drawn[lod]++;
        k->st.tris += k->group[m][lod].indexCount / 3;
    }
}

void kit_end(KitGpu *k, int slot) {
    uint32_t count[KIT_GROUPS] = { 0 }, first[KIT_GROUPS];
    for (uint32_t i = 0; i < k->ncand; ++i) count[k->key[i]]++;
    uint32_t at = 0;
    for (int g = 0; g < KIT_GROUPS; ++g) { first[g] = at; at += count[g]; }
    KitInstGPU *dst = (KitInstGPU *)k->inst[slot].mapped;
    uint32_t fill[KIT_GROUPS];
    memcpy(fill, first, sizeof(fill));
    for (uint32_t i = 0; i < k->ncand; ++i) dst[fill[k->key[i]]++] = k->cand[i];
    VkDrawIndexedIndirectCommand *c = (VkDrawIndexedIndirectCommand *)k->cmds[slot].mapped;
    uint32_t nc = 0;
    for (int g = 0; g < KIT_GROUPS; ++g) {
        if (!count[g]) continue;
        const KitGroup *gr = &k->group[g / PM_LODS][g % PM_LODS];
        c[nc].indexCount = gr->indexCount;
        c[nc].instanceCount = count[g];
        c[nc].firstIndex = gr->firstIndex;
        c[nc].vertexOffset = gr->vertexOffset;
        c[nc].firstInstance = first[g];
        nc++;
    }
    k->ncmd[slot] = nc;
    k->st.draws = (int)nc;
}

void kit_draw(KitGpu *k, VkCommandBuffer cmd, int slot) {
    uint32_t n = k->ncmd[slot];
    if (!n || !k->uploaded) return;
    VkBuffer bufs[2] = { k->verts.buf, k->inst[slot].buf };
    VkDeviceSize offs[2] = { 0, 0 };
    vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offs);
    vkCmdBindIndexBuffer(cmd, k->idx.buf, 0, VK_INDEX_TYPE_UINT16);
    if (k->vk->mdi) {
        vkCmdDrawIndexedIndirect(cmd, k->cmds[slot].buf, 0, n, sizeof(VkDrawIndexedIndirectCommand));
    } else {
        const VkDrawIndexedIndirectCommand *c = (const VkDrawIndexedIndirectCommand *)k->cmds[slot].mapped;
        for (uint32_t i = 0; i < n; ++i)
            vkCmdDrawIndexed(cmd, c[i].indexCount, c[i].instanceCount, c[i].firstIndex, c[i].vertexOffset, c[i].firstInstance);
    }
}

KitStats kit_stats(const KitGpu *k) { return k->st; }
