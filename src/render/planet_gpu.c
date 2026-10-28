#include "renderer_internal.h"
#include "terrain.h"
#include "thread.h"

/* The planet's patches' GPU side (see terrain.h, "Planet patches"): the
 * ground from the clipmap's window out to the horizon. It draws what
 * src/terrain selects and fills, and never generates terrain itself.
 *
 * Vertices: one device-local storage buffer of TERRAIN_PLANET_SLOTS runs of
 * TERRAIN_PATCH_VERTS, a run per cache slot (TerrainPlanet.slot), which the
 * vertex shader pulls by gl_VertexIndex, the draw's vertexOffset picking the
 * run. A patch filled is copied into its slot's run through this frame slot's
 * staging buffer, behind a barrier on every earlier frame's vertex reads, so
 * a slot reused for another patch is never read half written.
 *
 * Mesh: one index list per handedness of face (the odd faces' (u, v) are
 * left-handed, so their triangles wind the other way), each the grid's cells
 * split along the (gi, gj)-(gi + 1, gj + 1) diagonal, then the skirt's quads
 * both ways round (they hang down, seen from either side).
 *
 * Fill: a worker thread fills the patches the selection wants, most wanted
 * first, into a few result buffers; each frame the render thread takes what
 * it finished (at most MAX_UPLOADS), inserts them into the cache, uploads
 * them, then selects for this frame's camera and hands the worker the
 * patches still wanted. The selection only ever draws patches already
 * filled, falling back to their parents, so the frame never waits. */

#define MAX_UPLOADS 8        /* patches taken in and uploaded a frame */
#define RESULTS     12       /* the worker's finished patches waiting to be taken */
#define PATCH_BYTES ((VkDeviceSize)TERRAIN_PATCH_VERTS * sizeof(TerrainPatchVertex))

_Static_assert(TERRAIN_PATCH_VERTS <= 65536, "a patch's vertices don't fit 16-bit indices");
_Static_assert(sizeof(TerrainPatchVertex) == 10 * sizeof(float), "planet.vert reads a patch vertex as 10 floats");
_Static_assert(sizeof(PlanetPush) <= 128, "the planet's push constants must fit the guaranteed 128 bytes");

struct PlanetGpu {
    TerrainPlanet    sel;                  /* the render thread's: the cache's info and the selection */
    Thread          *worker;
    Mutex            lock;                 /* guards queue .. slowestFillMs */
    CondVar          wake;
    int              quit;
    TerrainPatch     queue[TERRAIN_PLANET_MAX_WANTS];   /* wanted, most first; the worker takes from qhead */
    int              nqueue, qhead;
    int              filling;              /* the worker is filling `busy` into `work` */
    TerrainPatch     busy;
    TerrainPatchMesh work;                 /* the worker's */
    TerrainPatchMesh result[RESULTS];      /* finished: result[0 .. nresult) */
    int              nresult;
    double           fillMs, slowestFillMs;
    Buffer           verts;                /* device local: SLOTS * VERTS TerrainPatchVertex */
    Buffer           staging[MAX_FRAMES];  /* host visible: MAX_UPLOADS patches */
    Buffer           indices;              /* device local, uint16: the even faces' list, then the odd's */
    uint32_t         nidx;                 /* indices in each list */
    VkDescriptorPool pool;
    VkDescriptorSet  set[MAX_FRAMES];
    /* this frame's draws */
    PlanetPush       push[TERRAIN_PLANET_MAX_DRAWS];
    int              face[TERRAIN_PLANET_MAX_DRAWS], slot[TERRAIN_PLANET_MAX_DRAWS];
    int              ndraw, uploads, levelLo, levelHi, holes;
    double           updateMs, slowestMs;
};

static int planet_worker(void *arg) {
    PlanetGpu *p = (PlanetGpu *)arg;
    terrain_frame_bind(terrain_frame_world());   /* a patch is the same in every frame: made in the world's */
    mutex_lock(&p->lock);
    for (;;) {
        while (!p->quit && (p->qhead >= p->nqueue || p->nresult == RESULTS)) cond_wait(&p->wake, &p->lock);
        if (p->quit) break;
        TerrainPatch t = p->queue[p->qhead++];
        p->busy = t;
        p->filling = 1;
        mutex_unlock(&p->lock);
        double t0 = glfwGetTime();
        terrain_patch_fill(t, &p->work);
        double ms = (glfwGetTime() - t0) * 1000.0;
        mutex_lock(&p->lock);
        p->filling = 0;
        p->result[p->nresult++] = p->work;   /* there is room: it waited for some */
        p->fillMs = ms;
        if (ms > p->slowestFillMs) p->slowestFillMs = ms;
    }
    mutex_unlock(&p->lock);
    return 0;
}

/* A cell's two triangles, facing out of the planet for a face whose (u, v,
 * out) is left-handed or not; the clipmap's (x, z, up) is left-handed, and
 * these wind as its do then (terrain_gpu.c tri). */
static uint16_t *cell(uint16_t *o, int gi, int gj, int left) {
    const int S = TERRAIN_PATCH_SIDE;
    uint16_t a = (uint16_t)(gj * S + gi), b = (uint16_t)(gj * S + gi + 1), c = (uint16_t)((gj + 1) * S + gi), d = (uint16_t)((gj + 1) * S + gi + 1);
    if (left) { o[0] = a; o[1] = d; o[2] = b; o[3] = a; o[4] = c; o[5] = d; }
    else      { o[0] = a; o[1] = b; o[2] = d; o[3] = a; o[4] = d; o[5] = c; }
    return o + 6;
}

static uint16_t *build_list(uint16_t *o, int left) {
    const int C = TERRAIN_PATCH_CELLS, S = TERRAIN_PATCH_SIDE;
    for (int gj = 0; gj < C; ++gj)
        for (int gi = 0; gi < C; ++gi) o = cell(o, gi, gj, left);
    for (int side = 0; side < 4; ++side)   /* the skirt's quads, both ways round */
        for (int k = 0; k < C; ++k) {
            int gi0 = side < 2 ? k : (side == 2 ? 0 : C), gj0 = side < 2 ? (side == 0 ? 0 : C) : k;
            int gi1 = side < 2 ? k + 1 : gi0, gj1 = side < 2 ? gj0 : k + 1;
            uint16_t e0 = (uint16_t)(gj0 * S + gi0), e1 = (uint16_t)(gj1 * S + gi1);
            uint16_t s0 = (uint16_t)(S * S + side * S + k), s1 = (uint16_t)(s0 + 1);
            const uint16_t q[12] = { e0, e1, s1, e0, s1, s0, e0, s1, e1, e0, s0, s1 };
            memcpy(o, q, sizeof(q));
            o += 12;
        }
    return o;
}

/* Builds the index lists and fills the six top patches, uploading both and
 * waiting for the copy. */
static void upload_static(Renderer *r, PlanetGpu *p) {
    VkCore *vk = r->vk;
    const int C = TERRAIN_PATCH_CELLS;
    p->nidx = (uint32_t)(6 * C * C + 4 * C * 12);
    VkDeviceSize idxBytes = 2 * (VkDeviceSize)p->nidx * sizeof(uint16_t);
    Buffer staging = vk_buffer(vk, idxBytes + 6 * PATCH_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uint16_t *base = (uint16_t *)staging.mapped, *o = build_list(base, 0);
    if ((uint32_t)(o - base) != p->nidx) fatal("planet: an index list came out the wrong length");
    build_list(o, 1);
    TerrainPatchMesh *top = (TerrainPatchMesh *)malloc(6 * sizeof(TerrainPatchMesh));
    if (!top) fatal("out of memory filling the planet");
    terrain_planet_init(&p->sel, top);
    VkBufferCopy copies[7] = { { 0, 0, idxBytes } };
    for (int f = 0; f < 6; ++f) {
        int s = terrain_planet_find(&p->sel, top[f].info.id);
        if (s < 0) fatal("planet: a top patch isn't in the cache");
        memcpy((char *)staging.mapped + idxBytes + f * PATCH_BYTES, top[f].v, (size_t)PATCH_BYTES);
        copies[1 + f] = (VkBufferCopy){ idxBytes + f * PATCH_BYTES, (VkDeviceSize)s * PATCH_BYTES, PATCH_BYTES };
    }
    free(top);
    p->indices = vk_buffer(vk, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VkCommandBufferAllocateInfo ca = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ca.commandPool = r->cmdPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &ca, &cmd));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    vkCmdCopyBuffer(cmd, staging.buf, p->indices.buf, 1, &copies[0]);
    vkCmdCopyBuffer(cmd, staging.buf, p->verts.buf, 6, &copies[1]);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(vk->queue));
    vkFreeCommandBuffers(vk->device, r->cmdPool, 1, &cmd);
    vk_buffer_destroy(vk, &staging);
}

void planet_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    PlanetGpu *p = (PlanetGpu *)calloc(1, sizeof(PlanetGpu));
    if (!p) fatal("out of memory allocating the planet");
    r->planet = p;
    const VkDeviceSize vertBytes = (VkDeviceSize)TERRAIN_PLANET_SLOTS * PATCH_BYTES;
    p->verts = vk_buffer(vk, vertBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int i = 0; i < MAX_FRAMES; ++i)
        p->staging[i] = vk_buffer(vk, MAX_UPLOADS * PATCH_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    upload_static(r, p);
    printf("Planet: patches of %d cells, %d slots (%.0f MB of vertices) out to %.0f km\n", TERRAIN_PATCH_CELLS, TERRAIN_PLANET_SLOTS,
           (double)vertBytes / (1024.0 * 1024.0), HORIZON_DISTANCE / 1000.0);

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &p->pool));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = p->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->terrainLayout;   /* the same bindings as the clipmap's: frame, vertices, roads (unused here) */
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &p->set[i]));
        write_buf_set(vk, p->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, p->set[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p->verts.buf, vertBytes);
        VkDeviceSize roadBytes;
        VkBuffer roads = road_gpu_buffer(r, i, &roadBytes);
        write_buf_set(vk, p->set[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, roads, roadBytes);
    }
    mutex_init(&p->lock);
    cond_init(&p->wake);
    p->worker = thread_create(planet_worker, p);
}

void planet_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    PlanetGpu *p = r->planet;
    if (!p) return;
    mutex_lock(&p->lock);
    p->quit = 1;
    cond_broadcast(&p->wake);
    mutex_unlock(&p->lock);
    thread_join(p->worker);
    mutex_destroy(&p->lock);
    cond_destroy(&p->wake);
    vkDestroyDescriptorPool(vk->device, p->pool, NULL);
    vk_buffer_destroy(vk, &p->indices);
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(vk, &p->staging[i]);
    vk_buffer_destroy(vk, &p->verts);
    free(p);
    r->planet = NULL;
}

/* The distance over which the haze leaves 1/e of the ground's contrast, seen
 * from the camera's height: hazeAmount (shaders/common.glsl) along a sight
 * line down to the ground, in clear weather. */
static double planet_haze(double camY) {
    double h = fmax(camY, 0.0), lo = HAZE_LOW_H, hi = HAZE_HIGH_H;
    double mlo = h > 1.0 ? lo * (1.0 - exp(-h / lo)) / h : 1.0, mhi = h > 1.0 ? hi * (1.0 - exp(-h / hi)) / h : 1.0;
    return TERRAIN_HAZE / (0.25 * mhi + 0.75 * mlo);
}

static void set4(float o[4], double x, double y, double z, double w) { o[0] = (float)x; o[1] = (float)y; o[2] = (float)z; o[3] = (float)w; }

void planet_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj) {
    PlanetGpu *p = r->planet;
    TerrainPlanet *tp = &p->sel;
    double t0 = glfwGetTime();

    /* 1. Take in what the worker finished, into the cache and this frame's uploads. */
    VkBufferCopy copies[MAX_UPLOADS];
    int n = 0;
    mutex_lock(&p->lock);
    int take = p->nresult < MAX_UPLOADS ? p->nresult : MAX_UPLOADS;
    for (int k = 0; k < take; ++k) {
        int s = terrain_planet_insert(tp, &p->result[k].info);
        if (s < 0) continue;   /* there already, or no slot free: wanted again later if need be */
        memcpy((char *)p->staging[slot].mapped + n * PATCH_BYTES, p->result[k].v, (size_t)PATCH_BYTES);
        copies[n] = (VkBufferCopy){ n * PATCH_BYTES, (VkDeviceSize)s * PATCH_BYTES, PATCH_BYTES };
        n++;
    }
    memmove(p->result, p->result + take, sizeof(TerrainPatchMesh) * (size_t)(p->nresult - take));
    p->nresult -= take;
    mutex_unlock(&p->lock);
    p->uploads = n;

    /* 2. Select for this frame's camera. */
    TerrainPlanetView v;
    memset(&v, 0, sizeof(v));
    v.cam[0] = cam[0]; v.cam[1] = cam[1]; v.cam[2] = cam[2];
    const float *m = viewProj->m;
    v.K = 0.5 * r->vk->extent.height * sqrt((double)m[1] * m[1] + (double)m[5] * m[5] + (double)m[9] * m[9]);
    v.frustum = frustum_from_matrix(*viewProj);
    v.haze = planet_haze(cam[1]);
    v.window = terrain_gpu_window(r, v.win);
    terrain_planet_select(tp, &v);

    /* 3. Hand the worker what is still wanted (and not already filled or filling). */
    mutex_lock(&p->lock);
    int q = 0;
    for (int k = 0; k < tp->nwant; ++k) {
        int dup = 0;
        for (int j = 0; j < p->nresult && !dup; ++j) dup = memcmp(&p->result[j].info.id, &tp->want[k], sizeof(TerrainPatch)) == 0;
        if (p->filling && memcmp(&p->busy, &tp->want[k], sizeof(TerrainPatch)) == 0) dup = 1;
        if (!dup) p->queue[q++] = tp->want[k];
    }
    p->qhead = 0;
    p->nqueue = q;
    cond_signal(&p->wake);
    mutex_unlock(&p->lock);

    /* 4. This frame's draws: each patch's centre relative to the camera, in its local frame. */
    p->ndraw = p->holes = 0;
    p->levelLo = TERRAIN_PATCH_LEVELS; p->levelHi = -1;
    for (int k = 0; k < tp->ndraw; ++k) {
        const TerrainPlanetDraw *d = &tp->draw[k];
        const TerrainPatchInfo *in = &tp->slot[d->slot].info;
        PlanetPush *pc = &p->push[p->ndraw];
        TerrainDir c = { in->centre.x - tp->cam.x, in->centre.y - tp->cam.y, in->centre.z - tp->cam.z };
        set4(pc->ax, tp->ax.x, tp->ax.y, tp->ax.z, 0.0);
        set4(pc->up, tp->up.x, tp->up.y, tp->up.z, 0.0);
        set4(pc->az, tp->az.x, tp->az.y, tp->az.z, 0.0);
        set4(pc->centre, c.x * tp->ax.x + c.y * tp->ax.y + c.z * tp->ax.z, c.x * tp->up.x + c.y * tp->up.y + c.z * tp->up.z,
             c.x * tp->az.x + c.y * tp->az.y + c.z * tp->az.z, 2.0 * in->err + 5.0);
        set4(pc->morph, d->morph_lo, d->morph_hi, 0.0, 0.0);
        if (d->hole) set4(pc->window, v.win[0] - cam[0], v.win[1] - cam[2], v.win[2] - cam[0], v.win[3] - cam[2]);
        else set4(pc->window, 0.0, 0.0, 0.0, 0.0);
        p->face[p->ndraw] = in->id.face;
        p->slot[p->ndraw] = d->slot;
        p->ndraw++;
        p->holes += d->hole;
        if (in->id.level < p->levelLo) p->levelLo = in->id.level;
        if (in->id.level > p->levelHi) p->levelHi = in->id.level;
    }
    p->updateMs = (glfwGetTime() - t0) * 1000.0;
    if (p->updateMs > p->slowestMs) p->slowestMs = p->updateMs;

    if (n == 0) return;
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(cmd, p->staging[slot].buf, p->verts.buf, (uint32_t)n, copies);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

/* After the clipmap, so where it covers the patches' depth test fails early. */
void planet_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    PlanetGpu *p = r->planet;
    if (p->ndraw == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->planetPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->planetPL, 0, 1, &p->set[slot], 0, NULL);
    vkCmdBindIndexBuffer(cmd, p->indices.buf, 0, VK_INDEX_TYPE_UINT16);
    for (int k = 0; k < p->ndraw; ++k) {
        vkCmdPushConstants(cmd, r->planetPL, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PlanetPush), &p->push[k]);
        vkCmdDrawIndexed(cmd, p->nidx, 1, (p->face[k] & 1) * p->nidx, p->slot[k] * TERRAIN_PATCH_VERTS, 0);
    }
}

/* Settled once nothing is wanted and nothing is being filled or waiting. */
bool planet_gpu_settled(Renderer *r) {
    PlanetGpu *p = r->planet;
    mutex_lock(&p->lock);
    bool idle = !p->filling && p->nresult == 0 && p->qhead >= p->nqueue;
    mutex_unlock(&p->lock);
    return idle && p->sel.nwant == 0;
}

void planet_gpu_stats(Renderer *r, char *buf, size_t n) {
    PlanetGpu *p = r->planet;
    const TerrainPlanet *tp = &p->sel;
    int used = 0;
    for (int k = 0; k < TERRAIN_PLANET_SLOTS; ++k) used += tp->slot[k].used;
    mutex_lock(&p->lock);
    double fillMs = p->fillMs, slowest = p->slowestFillMs;
    mutex_unlock(&p->lock);
    snprintf(buf, n, "planet: %d patches drawn (levels %d-%d, %d cut by the clipmap's window), %d culled, %d past the horizon, %d under the window;\n"
             "        %d of %d slots used, %d wanted; last fill %.2f ms on its worker (slowest %.2f); the frame's part %.2f ms (slowest %.2f)",
             p->ndraw, p->levelLo, p->levelHi, p->holes, tp->culled, tp->hidden, tp->inside, used, TERRAIN_PLANET_SLOTS, tp->nwant,
             fillMs, slowest, p->updateMs, p->slowestMs);
}
