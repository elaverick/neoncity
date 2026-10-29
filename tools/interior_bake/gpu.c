#include "bake.h"
#include "vkcore.h"
#include <time.h>

/* The baker's GPU work: a headless device (vk_init_headless), the scene's
 * acceleration structures and emitters, and four compute passes
 * (shaders/interior/). Every call records, submits and waits: the baker is a
 * batch tool. A path-traced capture or truth runs as many short passes, so no
 * one submission keeps the GPU long enough for the driver to reset it. */

static const uint32_t SPV_capture[] = {
#include "interior/capture.comp.inc"
};
static const uint32_t SPV_capture_mpi[] = {
#include "interior/capture_mpi.comp.inc"
};
static const uint32_t SPV_truth[] = {
#include "interior/truth.comp.inc"
};
static const uint32_t SPV_recon[] = {
#include "interior/recon.comp.inc"
};

/* shaders/interior/bake_scene.glsl: Params (std140, every member 16 bytes) */
typedef struct BakeParams {
    float   module[4], volume[4], grid[4];
    int32_t gridDim[4], views[4];
    float   viewRange[4], eval[4];
    int32_t evalDim[4];
    float   cam[4], outside[4], glass[4];
    int32_t march[4];
    float   march2[4];
    int32_t flags[4];
    int32_t trace[4], trace2[4];
    float   trace3[4];
    int32_t trace4[4];
} BakeParams;

/* pathtrace.glsl: Emitter */
typedef struct GpuEmitter { float p0[4], e1[4], e2[4]; uint32_t info[4]; } GpuEmitter;
static int build_emitters(const Scene *s, uint32_t *inst, GpuEmitter **out);

enum { PIPE_CAPTURE, PIPE_CAPTURE_MPI, PIPE_TRUTH, PIPE_RECON, PIPE_COUNT };
enum { MASK_CASTER = 1, MASK_GLASS = 2, MASK_EMITTER = 4 };   /* bake_scene.glsl: shadows trace MASK_CASTER */
#define LIGHT_RANGE 9.0f      /* metres: lights further than this from a point don't reach it */
#define THIN_SHELL  0.02f     /* metres: an open mesh's interval */
#define TIMED_RUNS  20
#define TIMED_ROUNDS 3
#define WARM_SECONDS 0.4
#define MAX_SLOPE   8.0f      /* the steepest sideways slope (dx / dz) a ray may have: copies of a periodic module cover it */

struct Gpu {
    VkCore vk;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkDescriptorSet set;
    VkPipeline pipes[PIPE_COUNT];
    VkQueryPool ts;
    Buffer params, staging;
    Buffer verts, idx, insts, mats, lights, emitters;
    int nemitters;
    TraceConfig trace;
    bool traced;
    Buffer *blasBuf;
    VkAccelerationStructureKHR *blas;
    int nblas;
    Buffer tlasBuf, tlasInst;
    VkAccelerationStructureKHR tlas;
    Buffer out0, out1, in0;
};

static const VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
static const VkMemoryPropertyFlags DEVICE = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
static const VkBufferUsageFlags STORE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
static const VkBufferUsageFlags GEOM = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

static double now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void begin(Gpu *g) {
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkResetCommandBuffer(g->cmd, 0));
    VK_CHECK(vkBeginCommandBuffer(g->cmd, &bi));
}

static void submit(Gpu *g) {
    VK_CHECK(vkEndCommandBuffer(g->cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g->cmd;
    VK_CHECK(vkQueueSubmit(g->vk.queue, 1, &si, g->fence));
    VK_CHECK(vkWaitForFences(g->vk.device, 1, &g->fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(g->vk.device, 1, &g->fence));
}

/* Grows b to at least size bytes (its contents are lost). */
static void ensure(Gpu *g, Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    if (size < 16) size = 16;
    if (b->buf && b->size >= size) return;
    vk_buffer_destroy(&g->vk, b);
    *b = vk_buffer(&g->vk, size, usage, props);
}

static void barrier(Gpu *g) {
    vk_memory_barrier(g->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

static void upload(Gpu *g, Buffer *dst, VkBufferUsageFlags usage, const void *data, size_t bytes) {
    ensure(g, dst, bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, DEVICE);
    if (!bytes) return;
    ensure(g, &g->staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST);
    memcpy(g->staging.mapped, data, bytes);
    begin(g);
    VkBufferCopy c = { 0, 0, bytes };
    vkCmdCopyBuffer(g->cmd, g->staging.buf, dst->buf, 1, &c);
    submit(g);
}

static void download(Gpu *g, Buffer *src, void *data, size_t bytes) {
    ensure(g, &g->staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, HOST);
    begin(g);
    VkBufferCopy c = { 0, 0, bytes };
    vkCmdCopyBuffer(g->cmd, src->buf, g->staging.buf, 1, &c);
    submit(g);
    memcpy(data, g->staging.mapped, bytes);
}

static VkPipeline make_pipe(Gpu *g, const uint32_t *code, size_t bytes) {
    VkShaderModule cs = vk_shader(&g->vk, code, bytes);
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = "main";
    ci.layout = g->pl;
    VkPipeline p;
    VK_CHECK(vkCreateComputePipelines(g->vk.device, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    vkDestroyShaderModule(g->vk.device, cs, NULL);
    return p;
}

Gpu *gpu_create(bool validation) {
    Gpu *g = calloc(1, sizeof(Gpu));
    if (!g) fatal("out of memory");
    vk_init_headless(&g->vk, validation, true);
    if (!g->vk.rt) fatal("the interior baker needs a GPU with ray queries");
    VkDevice d = g->vk.device;

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = g->vk.queueFamily;
    VK_CHECK(vkCreateCommandPool(d, &pi, NULL, &g->pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(d, &ai, &g->cmd));
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_CHECK(vkCreateFence(d, &fi, NULL, &g->fence));

    /* bake_scene.glsl, pathtrace.glsl and the passes: 0 TLAS, 1-9 storage buffers, 10 the parameters */
    VkDescriptorSetLayoutBinding b[11];
    memset(b, 0, sizeof(b));
    for (int i = 0; i < 11; ++i) {
        b[i].binding = (uint32_t)i;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                            : i == 10 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 11;
    li.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(d, &li, NULL, &g->dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &g->dsl;
    VK_CHECK(vkCreatePipelineLayout(d, &pli, NULL, &g->pl));
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 9 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.maxSets = 1;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(d, &dpi, NULL, &g->dpool));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = g->dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &g->dsl;
    VK_CHECK(vkAllocateDescriptorSets(d, &dai, &g->set));

    g->pipes[PIPE_CAPTURE] = make_pipe(g, SPV_capture, sizeof(SPV_capture));
    g->pipes[PIPE_CAPTURE_MPI] = make_pipe(g, SPV_capture_mpi, sizeof(SPV_capture_mpi));
    g->pipes[PIPE_TRUTH] = make_pipe(g, SPV_truth, sizeof(SPV_truth));
    g->pipes[PIPE_RECON] = make_pipe(g, SPV_recon, sizeof(SPV_recon));

    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * TIMED_RUNS;
    VK_CHECK(vkCreateQueryPool(d, &qi, NULL, &g->ts));
    g->params = vk_buffer(&g->vk, sizeof(BakeParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HOST);
    return g;
}

static void free_scene(Gpu *g) {
    for (int i = 0; i < g->nblas; ++i) {
        vkDestroyAccelerationStructureKHR(g->vk.device, g->blas[i], NULL);
        vk_buffer_destroy(&g->vk, &g->blasBuf[i]);
    }
    free(g->blas); free(g->blasBuf);
    g->blas = NULL; g->blasBuf = NULL; g->nblas = 0;
    if (g->tlas) vkDestroyAccelerationStructureKHR(g->vk.device, g->tlas, NULL);
    g->tlas = VK_NULL_HANDLE;
    vk_buffer_destroy(&g->vk, &g->tlasBuf);
    vk_buffer_destroy(&g->vk, &g->tlasInst);
}

void gpu_destroy(Gpu *g) {
    if (!g) return;
    VkDevice d = g->vk.device;
    vkDeviceWaitIdle(d);
    free_scene(g);
    Buffer *bs[] = { &g->params, &g->staging, &g->verts, &g->idx, &g->insts, &g->mats, &g->lights, &g->emitters,
                     &g->out0, &g->out1, &g->in0 };
    for (size_t i = 0; i < sizeof(bs) / sizeof(bs[0]); ++i) vk_buffer_destroy(&g->vk, bs[i]);
    for (int i = 0; i < PIPE_COUNT; ++i) vkDestroyPipeline(d, g->pipes[i], NULL);
    vkDestroyQueryPool(d, g->ts, NULL);
    vkDestroyDescriptorPool(d, g->dpool, NULL);
    vkDestroyPipelineLayout(d, g->pl, NULL);
    vkDestroyDescriptorSetLayout(d, g->dsl, NULL);
    vkDestroyFence(d, g->fence, NULL);
    vkDestroyCommandPool(d, g->pool, NULL);
    vk_shutdown(&g->vk);
    free(g);
}

const char *gpu_name(const Gpu *g) { return g->vk.props.deviceName; }

static VkAccelerationStructureKHR build_as(Gpu *g, VkAccelerationStructureBuildGeometryInfoKHR *bi, uint32_t prims, Buffer *store) {
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR(g->vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, bi, &prims, &sz);
    *store = vk_buffer(&g->vk, sz.accelerationStructureSize,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, DEVICE);
    uint64_t align = g->vk.asProps.minAccelerationStructureScratchOffsetAlignment;
    Buffer scratch = vk_buffer(&g->vk, sz.buildScratchSize + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, DEVICE);
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    ci.buffer = store->buf;
    ci.size = sz.accelerationStructureSize;
    ci.type = bi->type;
    VkAccelerationStructureKHR as;
    VK_CHECK(vkCreateAccelerationStructureKHR(g->vk.device, &ci, NULL, &as));
    bi->dstAccelerationStructure = as;
    bi->scratchData.deviceAddress = (vk_buffer_address(&g->vk, scratch.buf) + align - 1) & ~(align - 1);
    VkAccelerationStructureBuildRangeInfoKHR range = { 0 };
    range.primitiveCount = prims;
    const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
    begin(g);
    vkCmdBuildAccelerationStructuresKHR(g->cmd, 1, bi, &pr);
    submit(g);
    vk_buffer_destroy(&g->vk, &scratch);
    return as;
}

static VkDeviceAddress as_address(Gpu *g, VkAccelerationStructureKHR as) {
    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    ai.accelerationStructure = as;
    return vkGetAccelerationStructureDeviceAddressKHR(g->vk.device, &ai);
}

void gpu_load_scene(Gpu *g, const Scene *s) {
    free_scene(g);
    upload(g, &g->verts, STORE | GEOM, s->verts, sizeof(float) * 3 * s->nverts);
    upload(g, &g->idx, STORE | GEOM, s->idx, sizeof(uint32_t) * s->nidx);
    upload(g, &g->mats, STORE, s->mats, sizeof(BakeMat) * (size_t)s->nmats);
    upload(g, &g->lights, STORE, s->lights, sizeof(BakeLight) * (size_t)s->nlights);
    uint32_t *inst = malloc(sizeof(uint32_t) * 8 * (size_t)s->ninsts);
    if (!inst) fatal("out of memory");
    GpuEmitter *em = NULL;
    int nem = build_emitters(s, inst, &em);
    for (int i = 0; i < s->ninsts; ++i) {
        const BakeMesh *m = &s->meshes[s->insts[i].mesh];
        inst[i * 8 + 0] = m->firstIndex;
        inst[i * 8 + 1] = m->baseVertex;
        inst[i * 8 + 2] = s->insts[i].mat;
        inst[i * 8 + 3] = s->insts[i].flags;
        inst[i * 8 + 5] = inst[i * 8 + 6] = inst[i * 8 + 7] = 0;
    }
    upload(g, &g->insts, STORE, inst, sizeof(uint32_t) * 8 * (size_t)s->ninsts);
    upload(g, &g->emitters, STORE, em, sizeof(GpuEmitter) * (size_t)(nem ? nem : 1));
    g->nemitters = nem;
    free(inst);
    free(em);

    /* a BLAS per mesh */
    g->nblas = s->nmeshes;
    g->blas = calloc((size_t)s->nmeshes, sizeof(*g->blas));
    g->blasBuf = calloc((size_t)s->nmeshes, sizeof(*g->blasBuf));
    VkDeviceAddress va = vk_buffer_address(&g->vk, g->verts.buf), ia = vk_buffer_address(&g->vk, g->idx.buf);
    for (int i = 0; i < s->nmeshes; ++i) {
        const BakeMesh *m = &s->meshes[i];
        VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
        geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        VkAccelerationStructureGeometryTrianglesDataKHR *t = &geo.geometry.triangles;
        t->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t->vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        t->vertexData.deviceAddress = va + (VkDeviceAddress)m->baseVertex * 12u;
        t->vertexStride = 12;
        t->maxVertex = m->vertexCount - 1;
        t->indexType = VK_INDEX_TYPE_UINT32;
        t->indexData.deviceAddress = ia + (VkDeviceAddress)m->firstIndex * 4u;
        VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geo;
        g->blas[i] = build_as(g, &bi, m->indexCount / 3, &g->blasBuf[i]);
    }

    /* The TLAS: every instance, and for a periodic module its copies along x far enough that the
     * most sideways ray stays among them. */
    int copies = s->period > 0 ? (int)ceilf(MAX_SLOPE * s->D / s->period) + 1 : 0;
    uint32_t n = (uint32_t)s->ninsts * (uint32_t)(2 * copies + 1);
    VkAccelerationStructureInstanceKHR *ti = malloc(sizeof(*ti) * n);
    VkDeviceAddress *addr = malloc(sizeof(VkDeviceAddress) * (size_t)s->nmeshes);
    if (!ti || !addr) fatal("out of memory");
    for (int i = 0; i < s->nmeshes; ++i) addr[i] = as_address(g, g->blas[i]);
    uint32_t k = 0;
    for (int c = -copies; c <= copies; ++c)
        for (int i = 0; i < s->ninsts; ++i) {
            const BakeInst *in = &s->insts[i];
            VkAccelerationStructureInstanceKHR *o = &ti[k++];
            memset(o, 0, sizeof(*o));
            memcpy(o->transform.matrix, in->xf, sizeof(in->xf));
            o->transform.matrix[0][3] += (float)c * s->period;
            o->instanceCustomIndex = (uint32_t)i;
            o->mask = (in->flags & BF_GLASS) ? MASK_GLASS : (in->flags & BF_EMITTER) ? MASK_EMITTER : MASK_CASTER;
            o->flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR | VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR;
            o->accelerationStructureReference = addr[in->mesh];
        }
    free(addr);
    upload(g, &g->tlasInst, GEOM, ti, sizeof(*ti) * n);
    free(ti);
    VkAccelerationStructureGeometryKHR geo = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geo.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geo.geometry.instances.data.deviceAddress = vk_buffer_address(&g->vk, g->tlasInst.buf);
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    bi.geometryCount = 1;
    bi.pGeometries = &geo;
    g->tlas = build_as(g, &bi, n, &g->tlasBuf);
    printf("  %u TLAS instances (%d copies a side)\n", n, copies);
}

/* The emissive triangles, a light each (pathtrace.glsl: Emitter), in world space for the instances
 * as they stand (the shader finds a periodic module's copies), and the alias table that chooses
 * one by its power (Vose's method). Sets each instance's first emitter (inst[i * 8 + 4]; its
 * triangles' follow in order), or ~0u where it doesn't emit. */
static int build_emitters(const Scene *s, uint32_t *inst, GpuEmitter **out) {
    int n = 0;
    for (int i = 0; i < s->ninsts; ++i) {
        const float *e = s->mats[s->insts[i].mat].emission;
        inst[i * 8 + 4] = 0xFFFFFFFFu;
        if (e[0] > 0.0f || e[1] > 0.0f || e[2] > 0.0f) {
            inst[i * 8 + 4] = (uint32_t)n;
            n += (int)(s->meshes[s->insts[i].mesh].indexCount / 3);
        }
    }
    size_t cap = (size_t)(n ? n : 1);
    *out = calloc(cap, sizeof(GpuEmitter));
    double *power = malloc(sizeof(double) * cap), *q = malloc(sizeof(double) * cap);
    int *small = malloc(sizeof(int) * cap), *large = malloc(sizeof(int) * cap);
    if (!*out || !power || !q || !small || !large) fatal("out of memory building the emitters");
    double total = 0.0;
    for (int i = 0; i < s->ninsts; ++i) {
        if (inst[i * 8 + 4] == 0xFFFFFFFFu) continue;
        const BakeInst *in = &s->insts[i];
        const BakeMesh *m = &s->meshes[in->mesh];
        const float *e = s->mats[in->mat].emission;
        double lum = 0.2126 * e[0] + 0.7152 * e[1] + 0.0722 * e[2];
        for (uint32_t t = 0; t < m->indexCount / 3; ++t) {
            float w[3][3];
            for (int k = 0; k < 3; ++k) {
                const float *v = s->verts + 3 * (s->idx[m->firstIndex + t * 3 + k] + m->baseVertex);
                for (int r = 0; r < 3; ++r)
                    w[k][r] = in->xf[r * 4 + 0] * v[0] + in->xf[r * 4 + 1] * v[1] + in->xf[r * 4 + 2] * v[2] + in->xf[r * 4 + 3];
            }
            size_t k = inst[i * 8 + 4] + t;
            GpuEmitter *o = &(*out)[k];
            for (int r = 0; r < 3; ++r) {
                o->p0[r] = w[0][r];
                o->e1[r] = w[1][r] - w[0][r];
                o->e2[r] = w[2][r] - w[0][r];
            }
            float c[3] = { o->e1[1] * o->e2[2] - o->e1[2] * o->e2[1], o->e1[2] * o->e2[0] - o->e1[0] * o->e2[2],
                           o->e1[0] * o->e2[1] - o->e1[1] * o->e2[0] };
            float area = 0.5f * sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
            o->p0[3] = area > 1e-12f ? area : 1e-12f;
            o->info[0] = in->mat;
            power[k] = area * lum * 3.14159265;
            total += power[k];
        }
    }
    /* Vose: slot k keeps emitter k with probability e1[3], else gives its alias, info[1] */
    int ns = 0, nl = 0;
    for (int k = 0; k < n; ++k) {
        (*out)[k].e2[3] = (float)(total > 0.0 ? power[k] / total : 1.0 / n);
        q[k] = total > 0.0 ? power[k] / total * n : 1.0;
        if (q[k] < 1.0) small[ns++] = k; else large[nl++] = k;
    }
    while (ns && nl) {
        int a = small[--ns], b = large[--nl];
        (*out)[a].e1[3] = (float)q[a];
        (*out)[a].info[1] = (uint32_t)b;
        q[b] -= 1.0 - q[a];
        if (q[b] < 1.0) small[ns++] = b; else large[nl++] = b;
    }
    while (nl) { int b = large[--nl]; (*out)[b].e1[3] = 1.0f; (*out)[b].info[1] = (uint32_t)b; }
    while (ns) { int a = small[--ns]; (*out)[a].e1[3] = 1.0f; (*out)[a].info[1] = (uint32_t)a; }
    free(small); free(large); free(q); free(power);
    return n;
}

TraceConfig trace_defaults(int K) {
    TraceConfig t = { 16384, 256, 8, K, 16.0f, 1, 1u };
    return t;
}

void gpu_set_trace(Gpu *g, const TraceConfig *t) {
    g->traced = t != NULL;
    if (t) g->trace = *t;
}

static void fill_params(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, const ImpConfig *ic,
                        const Camera *c, const EvalGrid *e) {
    BakeParams p;
    memset(&p, 0, sizeof(p));
    float m[4] = { s->W, s->ceilY, s->D, s->period }, vol[4] = { s->xmin, s->xmax, s->ambient, THIN_SHELL };
    memcpy(p.module, m, sizeof(m));
    memcpy(p.volume, vol, sizeof(vol));
    if (t) {
        float gr[4] = { t->x0, t->y0, t->tpm, t->wrap ? 1.0f : 0.0f };
        memcpy(p.grid, gr, sizeof(gr));
        p.gridDim[0] = t->nx; p.gridDim[1] = t->ny;
    }
    if (v) {
        p.gridDim[2] = view_count(v);
        p.views[0] = v->nu; p.views[1] = v->nv; p.views[2] = v->kind;
        view_range(v, p.viewRange);
    }
    p.gridDim[3] = CAP_SLOTS;
    p.evalDim[3] = s->nlights;
    if (e) {
        float ev[4] = { e->x0, e->y0, e->ppmx, e->ppmy };
        memcpy(p.eval, ev, sizeof(ev));
        p.evalDim[0] = e->w; p.evalDim[1] = e->h;
    }
    if (c) memcpy(p.cam, c->pos, sizeof(float) * 3);
    if (ic) {
        p.views[3] = ic->K;
        p.evalDim[2] = ic->lastSlot;
        p.cam[3] = ic->unknownW;
        p.march[0] = ic->maxSteps; p.march[1] = ic->refine; p.march[2] = ic->mpiSlices;
        p.march2[0] = ic->maxTexStep; p.march2[1] = ic->offGridW; p.march2[2] = ic->nearSigma; p.march2[3] = ic->bothEnds ? 1.0f : 0.0f;
        p.flags[0] = ic->filtered;
    }
    float out[4] = { s->outside[0], s->outside[1], s->outside[2], LIGHT_RANGE }, gl[4] = { 0.04f, 0.92f, 0.08f, (float)(e ? e->ss : 1) };
    memcpy(p.outside, out, sizeof(out));
    memcpy(p.glass, gl, sizeof(gl));
    if (g->traced) {
        p.trace[2] = g->trace.spp;
        p.trace[3] = g->trace.bounces < 64 ? g->trace.bounces : 64;
        p.trace2[0] = 1;
        p.trace2[1] = g->trace.slots;
        p.trace2[2] = g->nemitters;
        p.trace2[3] = (int32_t)g->trace.seed;
        p.trace3[0] = g->trace.clamp;
        p.trace3[1] = g->trace.nee ? 1.0f : 0.0f;
    }
    p.trace4[1] = s->sealed ? 0 : 1;
    memcpy(g->params.mapped, &p, sizeof(p));
}

/* A path-traced pass: samples [first, first + count) of the bake's, and for a capture its view. */
static void set_pass(Gpu *g, int first, int count, int view) {
    BakeParams *p = (BakeParams *)g->params.mapped;
    p->trace[0] = first;
    p->trace[1] = count;
    p->trace4[0] = view;
}

static void bind(Gpu *g) {
    VkWriteDescriptorSetAccelerationStructureKHR wa = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
    wa.accelerationStructureCount = 1;
    wa.pAccelerationStructures = &g->tlas;
    Buffer *bufs[9] = { &g->verts, &g->idx, &g->insts, &g->mats, &g->lights, &g->out0, &g->out1, &g->in0, &g->emitters };
    VkDescriptorBufferInfo bi[10];
    VkWriteDescriptorSet w[11];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 11; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = g->set;
        w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[0].pNext = &wa;
    for (int i = 0; i < 9; ++i) {
        bi[i].buffer = bufs[i]->buf; bi[i].offset = 0; bi[i].range = VK_WHOLE_SIZE;
        w[i + 1].dstBinding = (uint32_t)(i + 1);
        w[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i + 1].pBufferInfo = &bi[i];
    }
    bi[9].buffer = g->params.buf; bi[9].offset = 0; bi[9].range = sizeof(BakeParams);
    w[10].dstBinding = 10;
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[10].pBufferInfo = &bi[9];
    vkUpdateDescriptorSets(g->vk.device, 11, w, 0, NULL);
}

static void dispatch(Gpu *g, int pipe, uint32_t x, uint32_t y, uint32_t z) {
    vkCmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipes[pipe]);
    vkCmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl, 0, 1, &g->set, 0, NULL);
    vkCmdDispatch(g->cmd, (x + 7) / 8, (y + 7) / 8, z);
}

void gpu_capture(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, Capture *out) {
    int views = view_count(v);
    size_t texels = (size_t)views * t->nx * t->ny;
    size_t lb = texels * CAP_SLOTS * sizeof(CapLayer), cb = texels * sizeof(uint32_t);
    ensure(g, &g->out0, lb, STORE, DEVICE);
    ensure(g, &g->out1, cb, STORE, DEVICE);
    ensure(g, &g->in0, 16, STORE, DEVICE);
    fill_params(g, s, v, t, NULL, NULL, NULL);
    bind(g);
    if (!g->traced) {
        begin(g);
        dispatch(g, PIPE_CAPTURE, (uint32_t)t->nx, (uint32_t)t->ny, (uint32_t)views);
        submit(g);
    } else {
        double t0 = now_seconds();
        for (int view = 0; view < views; ++view) {
            for (int first = 0; first < g->trace.spp; first += g->trace.batch) {
                int count = g->trace.spp - first < g->trace.batch ? g->trace.spp - first : g->trace.batch;
                set_pass(g, first, count, view);
                begin(g);
                dispatch(g, PIPE_CAPTURE, (uint32_t)t->nx, (uint32_t)t->ny, 1);
                submit(g);
            }
            if (views > 4) {
                double el = now_seconds() - t0;
                printf("\r  view %d/%d, %.0f s, about %.0f s to go   ", view + 1, views, el, el / (view + 1) * (views - view - 1));
                fflush(stdout);
            }
        }
        if (views > 4) printf("\n  path traced %d views at %d samples in %.1f s\n", views, g->trace.spp, now_seconds() - t0);
    }
    out->views = *v;
    out->grid = *t;
    out->layers = malloc(lb);
    out->counts = malloc(cb);
    if (!out->layers || !out->counts) fatal("out of memory reading a capture");
    download(g, &g->out0, out->layers, lb);
    download(g, &g->out1, out->counts, cb);
}

void gpu_capture_mpi(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, int slices, MpiCapture *out) {
    int views = view_count(v);
    size_t bytes = (size_t)views * slices * t->nx * t->ny * 8u;
    ensure(g, &g->out0, bytes, STORE, DEVICE);
    ensure(g, &g->out1, 16, STORE, DEVICE);
    ensure(g, &g->in0, 16, STORE, DEVICE);
    ImpConfig ic = { 0 };
    ic.mpiSlices = slices;
    fill_params(g, s, v, t, &ic, NULL, NULL);
    bind(g);
    begin(g);
    dispatch(g, PIPE_CAPTURE_MPI, (uint32_t)t->nx, (uint32_t)t->ny, (uint32_t)views);
    submit(g);
    out->views = *v;
    out->grid = *t;
    out->slices = slices;
    out->rgba = malloc(bytes);
    if (!out->rgba) fatal("out of memory reading a capture");
    download(g, &g->out0, out->rgba, bytes);
}

void gpu_truth(Gpu *g, const Scene *s, const Camera *c, const EvalGrid *e, float *rgba) {
    size_t bytes = (size_t)e->w * e->h * 16u;
    ensure(g, &g->out0, bytes, STORE, DEVICE);
    ensure(g, &g->out1, 16, STORE, DEVICE);
    ensure(g, &g->in0, 16, STORE, DEVICE);
    fill_params(g, s, NULL, NULL, NULL, c, e);
    bind(g);
    int passes = g->traced ? (g->trace.spp + g->trace.batch - 1) / g->trace.batch : 1;
    for (int i = 0; i < passes; ++i) {
        if (g->traced) {
            int first = i * g->trace.batch;
            set_pass(g, first, g->trace.spp - first < g->trace.batch ? g->trace.spp - first : g->trace.batch, 0);
        }
        begin(g);
        dispatch(g, PIPE_TRUTH, (uint32_t)e->w, (uint32_t)e->h, 1);
        submit(g);
    }
    download(g, &g->out0, rgba, bytes);
}

void gpu_set_impostor(Gpu *g, const void *imp, size_t bytes) { upload(g, &g->in0, STORE, imp, bytes); }

void gpu_recon(Gpu *g, const Scene *s, const ViewGrid *v, const TexGrid *t, const ImpConfig *ic,
               const Camera *c, const EvalGrid *e, float *rgba, ReconStats *stats, double *ms) {
    size_t ob = (size_t)e->w * e->h * 16u;
    ensure(g, &g->out0, ob, STORE, DEVICE);
    ensure(g, &g->out1, 16, STORE, DEVICE);
    fill_params(g, s, v, t, ic, c, e);
    ((BakeParams *)g->params.mapped)->march[3] = stats != NULL;
    bind(g);
    begin(g);
    vkCmdFillBuffer(g->cmd, g->out1.buf, 0, 16, 0);
    barrier(g);
    if (!ms) {
        dispatch(g, PIPE_RECON, (uint32_t)e->w, (uint32_t)e->h, 1);
        submit(g);
    } else {
        /* Timed: keep the GPU busy for WARM_SECONDS first, so its clocks are up (they take a few
         * hundred milliseconds to rise from idle), then take the fastest of TIMED_ROUNDS rounds of
         * TIMED_RUNS: other programs share the GPU in slices long enough to slow a whole round. */
        submit(g);
        double t0 = now_seconds();
        while (now_seconds() - t0 < WARM_SECONDS) {
            begin(g);
            for (int i = 0; i < 16; ++i) { dispatch(g, PIPE_RECON, (uint32_t)e->w, (uint32_t)e->h, 1); barrier(g); }
            submit(g);
        }
        uint64_t best = UINT64_MAX;
        for (int round = 0; round < TIMED_ROUNDS; ++round) {
            begin(g);
            vkCmdResetQueryPool(g->cmd, g->ts, 0, 2 * TIMED_RUNS);
            for (int i = 0; i < TIMED_RUNS; ++i) {
                vkCmdWriteTimestamp2(g->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->ts, (uint32_t)(2 * i));
                dispatch(g, PIPE_RECON, (uint32_t)e->w, (uint32_t)e->h, 1);
                vkCmdWriteTimestamp2(g->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->ts, (uint32_t)(2 * i + 1));
                barrier(g);
            }
            submit(g);
            uint64_t q[2 * TIMED_RUNS];
            VK_CHECK(vkGetQueryPoolResults(g->vk.device, g->ts, 0, 2 * TIMED_RUNS, sizeof(q), q, sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
            for (int i = 0; i < TIMED_RUNS; ++i) if (q[2 * i + 1] - q[2 * i] < best) best = q[2 * i + 1] - q[2 * i];
        }
        *ms = (double)best * g->vk.props.limits.timestampPeriod * 1e-6;
    }
    if (rgba) download(g, &g->out0, rgba, ob);
    if (stats) {
        uint32_t st[4];
        download(g, &g->out1, st, sizeof(st));
        stats->steps += st[0]; stats->guessed += st[1]; stats->offGrid += st[2]; stats->marched += st[3];
    }
}

/* -------------------------------------------------------- shared on the CPU --- */

int view_count(const ViewGrid *v) { return v->nu * v->nv; }

void view_range(const ViewGrid *v, float r[4]) {
    r[0] = v->azMax;
    if (v->kind == GRID_HEMIOCT) { r[1] = r[2] = 0.0f; r[3] = v->inset; return; }
    if (v->kind == GRID_AZEL) { r[1] = v->elMin; r[2] = v->elMax; r[3] = 0.0f; return; }
    /* GRID_SLOPE: in asinh(s / s0), the whole range shifted by under half a row so that a row lies on
     * the horizontal (t = 0) */
    float t0 = asinhf(tanf(v->elMin) / v->s0), t1 = asinhf(tanf(v->elMax) / v->s0);
    float dt = (t1 - t0) / (float)(v->nv - 1), shift = -t0 - dt * roundf(-t0 / dt);
    r[1] = t0 + shift; r[2] = t1 + shift; r[3] = v->s0;
}

void view_dir(const ViewGrid *v, int view, float d[3]) {
    float u = (float)(view % v->nu) / (float)(v->nu - 1), w = (float)(view / v->nu) / (float)(v->nv - 1);
    float r[4];
    view_range(v, r);
    if (v->kind != GRID_HEMIOCT) {
        float az = -r[0] + 2.0f * r[0] * u, el = r[1] + (r[2] - r[1]) * w;
        if (v->kind == GRID_SLOPE) el = atanf(r[3] * sinhf(el));
        d[0] = sinf(az) * cosf(el); d[1] = sinf(el); d[2] = cosf(az) * cosf(el);
        return;
    }
    float qx = -r[3] + 2.0f * r[3] * u, qy = -r[3] + 2.0f * r[3] * w;
    float px = (qx - qy) * 0.5f, py = (qx + qy) * 0.5f, pz = fmaxf(1.0f - fabsf(px) - fabsf(py), 1e-3f);
    float l = sqrtf(px * px + py * py + pz * pz);
    d[0] = px / l; d[1] = py / l; d[2] = pz / l;
}

TexGrid tex_grid(const Scene *s, float tpm, float margin) {
    TexGrid t;
    t.tpm = tpm;
    t.wrap = s->period > 0;
    float x0 = t.wrap ? 0.0f : -margin, x1 = t.wrap ? s->W : s->W + margin;
    t.x0 = x0;
    t.y0 = -margin;
    t.nx = (int)lroundf((x1 - x0) * tpm);
    t.ny = (int)lroundf((s->ceilY + 2.0f * margin) * tpm);   /* round the room: floor to the slab's underside */
    return t;
}
