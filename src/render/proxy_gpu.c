#include "renderer_internal.h"
#include "city.h"
#include "thread.h"

/* The distant proxies' GPU side (src/city/proxy.c makes them). Every tile
 * within PROXY_DISTANCE of the camera is generated on a worker thread of its
 * own and its boxes uploaded into one device-local pool, in pages; tiles that
 * fall out of range give their pages back. Each frame the tiles in view are
 * drawn by one indirect draw list, leaving out the chunks the chunk streamer
 * draws in full this frame (chunks_drawn): whatever of the city isn't built
 * in full, beyond VIEW_DISTANCE or still streaming in, stands there as boxes.
 *
 * Tiles live in a table indexed by their coordinates modulo PT_SIDE, wider
 * than the range is across, so the tiles in range never share a slot; a slot
 * holding a tile out of range is freed for the one now wanting it.
 *
 * A box is drawn as 30 vertices (its four sides and top) that
 * shaders/proxy/proxy.vert builds from gl_VertexIndex: box = index / 30. Each
 * draw's firstInstance picks its tile's camera-relative origin. */

#define PROXY_DISTANCE  TERRAIN_DISTANCE
#define PT_SIDE         128                          /* table side; > 2 * PROXY_DISTANCE / PROXY_TILE + 2 */
#define PT_SLOTS        (PT_SIDE * PT_SIDE)
#define PAGE_BOXES      128
#define POOL_PAGES_P    6144                         /* 768k boxes, 24 MB (the city: about 320k in 985 tiles) */
#define TILE_PAGES      16                           /* at most 2048 boxes a tile (the city's largest: about 820) */
#define UPLOAD_BOXES    (64 * PAGE_BOXES)            /* uploaded a frame, at most (bar one large tile) */
#define MAX_DRAWS       16384
#define MAX_JOBS_P      64                           /* tiles queued, or generated and not yet taken */
#define BOX_VERTS       30
#define RETRY_FRAMES    120                          /* a tile the pool had no room for waits this long to be generated again */

_Static_assert(PT_SIDE > 2.0 * PROXY_DISTANCE / PROXY_TILE + 2.0, "the proxy table is too small for the range");

enum { PT_FREE = 0, PT_QUEUED, PT_READY };

typedef struct PTile {
    int      tx, tz, state;
    uint64_t retry;      /* the pool had no room for it: not queued again before this frame */
    int      n, npages;
    uint16_t page[TILE_PAGES];
    int      first[PROXY_TILE_CELLS + 1];
    float    mn[3], mx[3];
} PTile;

/* A job: the tile to generate, then the tile generated (its storage reused). */
typedef struct PJob { int tx, tz; ProxyTile t; } PJob;
typedef struct PFree { uint16_t page; uint64_t frame; } PFree;
typedef struct PCand { int slot; float d; } PCand;

struct ProxyGpu {
    PTile    tiles[PT_SLOTS];
    Buffer   boxes;                              /* device local: POOL_PAGES_P * PAGE_BOXES ProxyBoxes */
    uint16_t freePages[POOL_PAGES_P];
    int      nfree;
    PFree    pending[POOL_PAGES_P];
    int      npending;
    Buffer   staging[MAX_FRAMES], indirect[MAX_FRAMES], origins[MAX_FRAMES];
    VkBufferCopy copies[MAX_JOBS_P * TILE_PAGES];   /* one per page uploaded */
    uint32_t ndraw[MAX_FRAMES];
    VkDescriptorPool pool;
    VkDescriptorSet  set[MAX_FRAMES];

    /* The worker's ring of jobs, counted from the start (slot = count % MAX_JOBS_P):
     * [head, done) generated, waiting to be taken; [done, started) being generated;
     * [started, tail) queued. Only the worker touches a job between started and
     * done, only the main thread outside them. */
    Thread  *worker;
    Mutex    lock;
    CondVar  cond;
    PJob     job[MAX_JOBS_P];
    uint32_t head, done, started, tail;
    int      quit;
    PCand    cand[PT_SLOTS];

    int      inRange, readyTiles, drawnTiles, drawnBoxes, residentBoxes;
    int      starved;           /* tiles generated that the pool had no room for, so far */
    double   created, filled;   /* when, and how long after it every tile in range was first ready (s) */
};

static int slot_of(int tx, int tz) { return (tz & (PT_SIDE - 1)) * PT_SIDE + (tx & (PT_SIDE - 1)); }

static int proxy_worker(void *arg) {
    ProxyGpu *p = (ProxyGpu *)arg;
    mutex_lock(&p->lock);
    for (;;) {
        while (!p->quit && p->started == p->tail) cond_wait(&p->cond, &p->lock);
        if (p->quit) break;
        PJob *j = &p->job[p->started++ % MAX_JOBS_P];
        mutex_unlock(&p->lock);
        city_proxy_tile(j->tx, j->tz, &j->t);
        mutex_lock(&p->lock);
        p->done++;
    }
    mutex_unlock(&p->lock);
    return 0;
}

static void free_tile(ProxyGpu *p, PTile *t, uint64_t frame) {
    for (int i = 0; i < t->npages; ++i) p->pending[p->npending++] = (PFree){ t->page[i], frame };
    memset(t, 0, sizeof(*t));
}

void proxy_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    ProxyGpu *p = (ProxyGpu *)calloc(1, sizeof(ProxyGpu));
    if (!p) fatal("out of memory allocating the proxies");
    r->proxies = p;
    const VkDeviceSize boxBytes = (VkDeviceSize)POOL_PAGES_P * PAGE_BOXES * sizeof(ProxyBox);
    p->boxes = vk_buffer(vk, boxBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    for (int i = 0; i < POOL_PAGES_P; ++i) p->freePages[i] = (uint16_t)(POOL_PAGES_P - 1 - i);
    p->nfree = POOL_PAGES_P;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        p->staging[i] = vk_buffer(vk, (VkDeviceSize)(UPLOAD_BOXES + TILE_PAGES * PAGE_BOXES) * sizeof(ProxyBox), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        p->indirect[i] = vk_buffer(vk, sizeof(VkDrawIndirectCommand) * MAX_DRAWS, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
        p->origins[i] = vk_buffer(vk, sizeof(float) * 4 * PT_SLOTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    }

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
        ai.pSetLayouts = &r->proxyLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &p->set[i]));
        write_buf_set(vk, p->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, p->set[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p->boxes.buf, boxBytes);
        write_buf_set(vk, p->set[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p->origins[i].buf, p->origins[i].size);
    }

    mutex_init(&p->lock);
    cond_init(&p->cond);
    p->worker = thread_create(proxy_worker, p);
    p->created = glfwGetTime();
    printf("Distant proxies: tiles of %.0f m to %.0f km, %.0f MB box pool\n", PROXY_TILE, PROXY_DISTANCE / 1000.0,
           boxBytes / (1024.0 * 1024.0));
}

void proxy_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    ProxyGpu *p = r->proxies;
    if (!p) return;
    mutex_lock(&p->lock);
    p->quit = 1;
    cond_broadcast(&p->cond);
    mutex_unlock(&p->lock);
    thread_join(p->worker);
    mutex_destroy(&p->lock);
    cond_destroy(&p->cond);
    for (int i = 0; i < MAX_JOBS_P; ++i) proxy_tile_free(&p->job[i].t);
    vkDestroyDescriptorPool(vk->device, p->pool, NULL);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vk_buffer_destroy(vk, &p->staging[i]);
        vk_buffer_destroy(vk, &p->indirect[i]);
        vk_buffer_destroy(vk, &p->origins[i]);
    }
    vk_buffer_destroy(vk, &p->boxes);
    free(p);
    r->proxies = NULL;
}

static int cmp_cand(const void *a, const void *b) {
    float x = ((const PCand *)a)->d, y = ((const PCand *)b)->d;
    return x < y ? -1 : x > y;
}

/* Nearest distance from the camera (x, z) to tile (tx, tz). */
static float tile_dist(int tx, int tz, const double cam[3]) {
    double x0 = tx * PROXY_TILE, z0 = tz * PROXY_TILE;
    double dx = fmax(fmax(x0 - cam[0], cam[0] - x0 - PROXY_TILE), 0.0), dz = fmax(fmax(z0 - cam[2], cam[2] - z0 - PROXY_TILE), 0.0);
    return (float)sqrt(dx * dx + dz * dz);
}

/* Takes the generated tiles off the front of the queue while their uploads fit
 * this frame's staging; frees tiles out of range; queues the nearest missing
 * ones. Then builds the frame's draws. */
void proxy_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, uint64_t frame, const double cam[3], const mat4 *viewProj) {
    ProxyGpu *p = r->proxies;
    for (int i = 0; i < p->npending;)
        if (frame >= p->pending[i].frame + MAX_FRAMES) {
            p->freePages[p->nfree++] = p->pending[i].page;
            p->pending[i] = p->pending[--p->npending];
        } else ++i;

    /* 1. Generated tiles: their boxes into free pages, through the staging buffer. */
    ProxyBox *stage = (ProxyBox *)p->staging[slot].mapped;
    uint32_t staged = 0, ncopy = 0;
    mutex_lock(&p->lock);
    uint32_t done = p->done;
    mutex_unlock(&p->lock);
    for (; p->head != done; p->head++) {
        PJob *j = &p->job[p->head % MAX_JOBS_P];
        PTile *t = &p->tiles[slot_of(j->tx, j->tz)];
        int want = t->state == PT_QUEUED && t->tx == j->tx && t->tz == j->tz;   /* else freed or replaced meanwhile */
        int pages = (j->t.n + PAGE_BOXES - 1) / PAGE_BOXES;
        if (want && pages > TILE_PAGES) fatal("proxies: a tile has more boxes than it can hold");
        if (want && staged > 0 && staged + (uint32_t)j->t.n > UPLOAD_BOXES) break;   /* the rest next frame */
        if (want && pages > p->nfree) { t->state = PT_FREE; t->retry = frame + RETRY_FRAMES; want = 0; p->starved++; }   /* asked for again later */
        if (!want) continue;
        t->n = j->t.n;
        t->npages = pages;
        memcpy(t->first, j->t.first, sizeof(t->first));
        memcpy(t->mn, j->t.mn, sizeof(t->mn));
        memcpy(t->mx, j->t.mx, sizeof(t->mx));
        for (int k = 0; k < pages; ++k) {
            t->page[k] = p->freePages[--p->nfree];
            uint32_t n = (uint32_t)(t->n - k * PAGE_BOXES < PAGE_BOXES ? t->n - k * PAGE_BOXES : PAGE_BOXES);
            memcpy(stage + staged, j->t.box + k * PAGE_BOXES, n * sizeof(ProxyBox));
            p->copies[ncopy++] = (VkBufferCopy){ staged * sizeof(ProxyBox), (VkDeviceSize)t->page[k] * PAGE_BOXES * sizeof(ProxyBox),
                                                 n * sizeof(ProxyBox) };
            staged += n;
        }
        t->state = PT_READY;
        p->residentBoxes += t->n;
    }
    if (ncopy) {
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBuffer(cmd, p->staging[slot].buf, p->boxes.buf, ncopy, p->copies);
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    /* 2. Tiles in range: free whatever holds their slots, queue the nearest missing. */
    int tx0 = (int)floor((cam[0] - PROXY_DISTANCE) / PROXY_TILE), tx1 = (int)floor((cam[0] + PROXY_DISTANCE) / PROXY_TILE);
    int tz0 = (int)floor((cam[2] - PROXY_DISTANCE) / PROXY_TILE), tz1 = (int)floor((cam[2] + PROXY_DISTANCE) / PROXY_TILE);
    int ncand = 0;
    p->inRange = p->readyTiles = 0;
    for (int tz = tz0; tz <= tz1; ++tz)
        for (int tx = tx0; tx <= tx1; ++tx) {
            float d = tile_dist(tx, tz, cam);
            if (d > PROXY_DISTANCE) continue;
            p->inRange++;
            int s = slot_of(tx, tz);
            PTile *t = &p->tiles[s];
            if (t->state != PT_FREE && (t->tx != tx || t->tz != tz)) {
                if (t->state == PT_READY) p->residentBoxes -= t->n;
                free_tile(p, t, frame);   /* a queued job for it is simply not taken up */
            }
            if (t->state == PT_FREE && (t->tx != tx || t->tz != tz)) t->retry = 0;   /* another tile's wait */
            if (t->state == PT_FREE && frame >= t->retry) { p->cand[ncand].slot = s; p->cand[ncand].d = d; ncand++; t->tx = tx; t->tz = tz; }
            else if (t->state == PT_READY) p->readyTiles++;
        }
    if (p->filled == 0.0 && p->readyTiles == p->inRange) p->filled = glfwGetTime() - p->created;
    if (ncand) {
        qsort(p->cand, (size_t)ncand, sizeof(PCand), cmp_cand);
        mutex_lock(&p->lock);
        for (int i = 0; i < ncand && p->tail - p->head < MAX_JOBS_P; ++i) {
            PTile *t = &p->tiles[p->cand[i].slot];
            t->state = PT_QUEUED;
            p->job[p->tail % MAX_JOBS_P].tx = t->tx;
            p->job[p->tail % MAX_JOBS_P].tz = t->tz;
            p->tail++;
        }
        cond_signal(&p->cond);
        mutex_unlock(&p->lock);
    }

    /* 3. Draws: the tiles in view, less the chunks drawn in full. */
    Frustum fr = frustum_from_matrix(*viewProj);
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)p->indirect[slot].mapped;
    float (*org)[4] = (float (*)[4])p->origins[slot].mapped;
    uint32_t nd = 0;
    p->drawnTiles = p->drawnBoxes = 0;
    for (int s = 0; s < PT_SLOTS; ++s) {
        const PTile *t = &p->tiles[s];
        if (t->state != PT_READY || t->n == 0) continue;
        float ox = (float)(t->tx * PROXY_TILE - cam[0]), oz = (float)(t->tz * PROXY_TILE - cam[2]);
        if (!frustum_test_aabb(&fr, v3(ox + t->mn[0], t->mn[1] - (float)cam[1], oz + t->mn[2]),
                               v3(ox + t->mx[0], t->mx[1] - (float)cam[1], oz + t->mx[2]))) continue;
        org[s][0] = ox; org[s][1] = oz; org[s][2] = 0.0f; org[s][3] = 0.0f;
        int run0 = -1;
        for (int k = 0; k <= PROXY_TILE_CELLS; ++k) {
            int keep = k < PROXY_TILE_CELLS && !chunks_drawn(r->chunks, t->tx * PROXY_TILE_CHUNKS + k % PROXY_TILE_CHUNKS,
                                                             t->tz * PROXY_TILE_CHUNKS + k / PROXY_TILE_CHUNKS);
            if (keep && run0 < 0) run0 = t->first[k];
            if (keep || run0 < 0) continue;
            /* boxes [run0, first[k]), split where they cross from one page to the next */
            for (int b = run0, e = t->first[k]; b < e && nd < MAX_DRAWS;) {
                int pg = b / PAGE_BOXES, end = (pg + 1) * PAGE_BOXES < e ? (pg + 1) * PAGE_BOXES : e;
                VkDrawIndirectCommand *c = &cmds[nd++];
                c->vertexCount = (uint32_t)(end - b) * BOX_VERTS;
                c->instanceCount = 1;
                c->firstVertex = ((uint32_t)t->page[pg] * PAGE_BOXES + (uint32_t)(b - pg * PAGE_BOXES)) * BOX_VERTS;
                c->firstInstance = (uint32_t)s;
                p->drawnBoxes += end - b;
                b = end;
            }
            run0 = -1;
        }
        p->drawnTiles++;
    }
    p->ndraw[slot] = nd;
}

void proxy_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    ProxyGpu *p = r->proxies;
    uint32_t n = p->ndraw[slot];
    if (!n) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->proxyPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->proxyPL, 0, 1, &p->set[slot], 0, NULL);
    if (r->vk->mdi) {
        uint32_t maxCount = r->vk->props.limits.maxDrawIndirectCount;
        for (uint32_t first = 0; first < n; first += maxCount)
            vkCmdDrawIndirect(cmd, p->indirect[slot].buf, first * sizeof(VkDrawIndirectCommand), n - first < maxCount ? n - first : maxCount,
                              sizeof(VkDrawIndirectCommand));
    } else {
        const VkDrawIndirectCommand *c = (const VkDrawIndirectCommand *)p->indirect[slot].mapped;
        for (uint32_t i = 0; i < n; ++i) vkCmdDraw(cmd, c[i].vertexCount, 1, c[i].firstVertex, c[i].firstInstance);
    }
}

bool renderer_proxies_ready(Renderer *r) { return r->proxies->readyTiles == r->proxies->inRange; }

void renderer_proxy_stats(Renderer *r, char *buf, size_t n) {
    const ProxyGpu *p = r->proxies;
    char filled[48] = "";
    if (p->filled > 0.0) snprintf(filled, sizeof(filled), ", all first ready after %.2f s", p->filled);
    char starved[48] = "";
    if (p->starved > 0) snprintf(starved, sizeof(starved), ", %d turned away by a full pool", p->starved);
    snprintf(buf, n, "proxies: %d of %d tiles in range ready%s (%d boxes, %d of %d pages%s), %d tiles and %d boxes drawn in %u draws",
             p->readyTiles, p->inRange, filled, p->residentBoxes, POOL_PAGES_P - p->nfree - p->npending, POOL_PAGES_P, starved,
             p->drawnTiles, p->drawnBoxes, p->ndraw[(r->frameIndex + MAX_FRAMES - 1) % MAX_FRAMES]);
}
