#include "renderer_internal.h"
#include "city.h"
#include "plan.h"
#include "terrain.h"
#include "thread.h"

/* The distant proxies' GPU side (src/city/proxy.c makes them). Every tile
 * within PROXY_DISTANCE of the camera is generated on a worker thread of its
 * own and its boxes uploaded into one device-local pool, in pages; tiles that
 * fall out of range give their pages back. Each frame the tiles in view are
 * drawn by one indirect draw list, leaving out the chunks the chunk streamer
 * draws in full this frame (chunks_drawn): whatever of the city isn't built
 * in full, beyond VIEW_DISTANCE or still streaming in, stands there as boxes.
 *
 * Settlements: a frame lays out its own settlement's towns only
 * (plan_built_edge), so the tiles are kept by settlement, in layers: one for
 * each settlement whose disc the camera is within PROXY_DISTANCE of (the
 * active frame's and any neighbour's), up to PROXY_LAYERS, the nearest first.
 * A layer's tiles are made in its settlement's own frame, so they stand
 * whatever frame is active: a change of frame keeps them all. The active
 * frame's settlement's layer is drawn as it was made; another's through the
 * turn and offset from its frame into the active one at the camera (a
 * rigid map: off by a metre or two at PROXY_DISTANCE, a twentieth of a
 * pixel), into its tiles' origins. A layer is let go LAYER_KEEP beyond
 * the range.
 *
 * Tiles live in a table per layer indexed by their coordinates modulo
 * PT_SIDE, wider than the range is across, so the tiles in range never share
 * a slot; a slot holding a tile out of range is freed for the one now wanting
 * it.
 *
 * A box is drawn as 30 vertices (its four sides and top) that
 * shaders/proxy/proxy.vert builds from gl_VertexIndex: box = index / 30. Each
 * draw's firstInstance picks its tile's camera-relative origin and turn. */

#define PROXY_DISTANCE  TERRAIN_DISTANCE
#define PT_SIDE         128                          /* table side; > 2 * PROXY_DISTANCE / PROXY_TILE + 2 */
#define PT_SLOTS        (PT_SIDE * PT_SIDE)
#define PAGE_BOXES      128
#define POOL_PAGES_P    6144                         /* 768k boxes, 24 MB (the city: about 320k in 985 tiles) */
#define TILE_PAGES      16                           /* at most 2048 boxes a tile (the city's largest: about 820) */
#define UPLOAD_BOXES    (64 * PAGE_BOXES)            /* uploaded a frame, at most (bar one large tile) */
#define MAX_DRAWS       32768                        /* a tile drawn in part takes a draw per chunk run */
#define PROXY_LOD_PX    1.0f                         /* boxes smaller than this on screen (pixels) are left out */
#define MAX_JOBS_P      64                           /* tiles queued, or generated and not yet taken */
#define BOX_VERTS       30
#define RETRY_FRAMES    120                          /* a tile the pool had no room for waits this long to be generated again */
#define PROXY_LAYERS    4                            /* settlements drawn at once */
#define LAYER_KEEP      2000.0                       /* a layer is let go this far beyond PROXY_DISTANCE (m) */

_Static_assert(PT_SIDE > 2.0 * PROXY_DISTANCE / PROXY_TILE + 2.0, "the proxy table is too small for the range");

/* PT_EMPTY: a tile that can hold none of the city (tile_empty), never generated. */
enum { PT_FREE = 0, PT_QUEUED, PT_READY, PT_EMPTY };

typedef struct PTile {
    int      tx, tz, state;
    uint64_t retry;      /* the pool had no room for it: not queued again before this frame */
    int      n, npages;
    uint16_t page[TILE_PAGES];
    int      first[PROXY_TILE_CELLS + 1];
    uint16_t lod[PROXY_TILE_CELLS][PROXY_LOD_BINS];
    float    mn[3], mx[3];
} PTile;

/* A settlement's tiles, and where the camera is in its frame: (x, z) at
 * cam, and its axes turned by (c, s) into the active frame's there (a vector
 * (x, z) in it is (c x - s z, s x + c z) in the active one). */
typedef struct PLayer {
    int                 settlement;   /* -1: unused */
    const TerrainFrame *frame;        /* the settlement's */
    int                 active;       /* it is the active frame: drawn as made */
    double              cam[3];
    float               c, s;
    PTile               tiles[PT_SLOTS];
} PLayer;

/* A job: the tile to generate, of a settlement in its frame, then the tile
 * generated (its storage reused); none if its layer has gone since. */
typedef struct PJob { int tx, tz, settlement; const TerrainFrame *frame; ProxyTile t; } PJob;
typedef struct PFree { uint16_t page; uint64_t frame; } PFree;
typedef struct PCand { int layer, slot; float d; } PCand;

struct ProxyGpu {
    PLayer   layer[PROXY_LAYERS];
    int      nset;                                     /* settlements, and the discs their footprints lie in */
    TerrainDir disc[PLAN_MAX_SETTLEMENTS];
    double   reach[PLAN_MAX_SETTLEMENTS];
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
    PCand    cand[PROXY_LAYERS * PT_SLOTS];

    const TerrainFrame *tf;     /* the active frame (proxy_gpu_set_frame) */
    int      inRange, readyTiles, drawnTiles, drawnBoxes, residentBoxes, layers;
    int      starved;           /* tiles generated that the pool had no room for, so far */
    double   created, filled;   /* when, and how long after it every tile in range was first ready (s) */
};

static int slot_of(int tx, int tz) { return (tz & (PT_SIDE - 1)) * PT_SIDE + (tx & (PT_SIDE - 1)); }

/* The layer holding settlement k's tiles, or -1. The worker asks under lock. */
static int layer_of(const ProxyGpu *p, int k) {
    for (int l = 0; l < PROXY_LAYERS; ++l)
        if (p->layer[l].settlement == k) return l;
    return -1;
}

static int proxy_worker(void *arg) {
    ProxyGpu *p = (ProxyGpu *)arg;
    mutex_lock(&p->lock);
    for (;;) {
        while (!p->quit && p->started == p->tail) cond_wait(&p->cond, &p->lock);
        if (p->quit) break;
        PJob *j = &p->job[p->started++ % MAX_JOBS_P];
        int stale = layer_of(p, j->settlement) < 0;
        mutex_unlock(&p->lock);
        if (stale) j->t.n = 0;   /* not taken up (proxy_gpu_update) */
        else {
            terrain_frame_bind(j->frame);
            city_proxy_tile(j->tx, j->tz, &j->t);
        }
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
        p->origins[i] = vk_buffer(vk, sizeof(float) * 4 * PROXY_LAYERS * PT_SLOTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
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

    for (int l = 0; l < PROXY_LAYERS; ++l) p->layer[l].settlement = -1;
    p->nset = plan_settlement_count();
    for (int k = 0; k < p->nset; ++k) {
        PlanSettlement s = plan_settlement(k);
        p->disc[k] = terrain_frame_dir(plan_frame_settlement(k), s.disc.x, s.disc.z);
        p->reach[k] = plan_settlement_reach(k);
    }
    mutex_init(&p->lock);
    cond_init(&p->cond);
    p->tf = terrain_frame();
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

/* Every tile stands: each is made in its settlement's frame, and drawn into
 * whichever is active (proxy_gpu_update). */
void proxy_gpu_set_frame(Renderer *r, const TerrainFrame *f) { r->proxies->tf = f; }

/* Frees layer l's tiles (their pages once the frames drawing them retire)
 * and the layer: jobs still queued for it are skipped, and what the worker
 * is making for it is not taken up. */
static void free_layer(ProxyGpu *p, int l, uint64_t frame) {
    PLayer *L = &p->layer[l];
    for (int s = 0; s < PT_SLOTS; ++s) {
        PTile *t = &L->tiles[s];
        if (t->state == PT_FREE) { t->tx = t->tz = 0; t->retry = 0; continue; }
        if (t->state == PT_READY) p->residentBoxes -= t->n;
        free_tile(p, t, frame);
    }
    mutex_lock(&p->lock);
    L->settlement = -1;
    mutex_unlock(&p->lock);
}

/* The layers for the camera (cam in the active frame): those of settlements
 * within PROXY_DISTANCE of it kept or taken, the nearest first, those beyond
 * it by LAYER_KEEP let go; and where the camera is in each one's frame. */
static void place_layers(ProxyGpu *p, const double cam[3], uint64_t frame) {
    const TerrainFrame *af = p->tf;
    TerrainDir d = terrain_frame_dir(af, cam[0], cam[2]), ax, up, az;
    terrain_frame_axes(af, cam[0], cam[2], &ax, &up, &az);
    double beyond[PLAN_MAX_SETTLEMENTS];
    for (int k = 0; k < p->nset; ++k) {
        double c = d.x * p->disc[k].x + d.y * p->disc[k].y + d.z * p->disc[k].z;
        beyond[k] = PLANET_RADIUS * acos(c > 1.0 ? 1.0 : c < -1.0 ? -1.0 : c) - p->reach[k];
    }
    for (int l = 0; l < PROXY_LAYERS; ++l)
        if (p->layer[l].settlement >= 0 && beyond[p->layer[l].settlement] > PROXY_DISTANCE + LAYER_KEEP) free_layer(p, l, frame);
    for (;;) {   /* the nearest settlement in range without a layer, while one is free */
        int k = -1, spare = layer_of(p, -1);
        for (int q = 0; q < p->nset && spare >= 0; ++q)
            if (beyond[q] < PROXY_DISTANCE && layer_of(p, q) < 0 && (k < 0 || beyond[q] < beyond[k])) k = q;
        if (k < 0) break;
        mutex_lock(&p->lock);
        p->layer[spare].settlement = k;
        p->layer[spare].frame = plan_frame_settlement(k);
        mutex_unlock(&p->lock);
    }
    p->layers = 0;
    for (int l = 0; l < PROXY_LAYERS; ++l) {
        PLayer *L = &p->layer[l];
        if (L->settlement < 0) continue;
        p->layers++;
        L->active = L->frame == af;
        L->cam[1] = cam[1];
        if (L->active) {
            L->cam[0] = cam[0]; L->cam[2] = cam[2];
            L->c = 1.0f; L->s = 0.0f;
            continue;
        }
        TerrainDir lx, lu, lz;
        terrain_dir_frame(L->frame, d, &L->cam[0], &L->cam[2]);
        terrain_frame_axes(L->frame, L->cam[0], L->cam[2], &lx, &lu, &lz);
        L->c = (float)(lx.x * ax.x + lx.y * ax.y + lx.z * ax.z);
        L->s = (float)(lx.x * az.x + lx.y * az.y + lx.z * az.z);
    }
}

/* Nearest distance from the camera (x, z) to tile (tx, tz). */
static float tile_dist(int tx, int tz, const double cam[3]) {
    double x0 = tx * PROXY_TILE, z0 = tz * PROXY_TILE;
    double dx = fmax(fmax(x0 - cam[0], cam[0] - x0 - PROXY_TILE), 0.0), dz = fmax(fmax(z0 - cam[2], cam[2] - z0 - PROXY_TILE), 0.0);
    return (float)sqrt(dx * dx + dz * dz);
}

/* Takes the generated tiles off the front of the queue while their uploads fit
 * this frame's staging; places the layers; in each, frees tiles out of range
 * and queues the nearest missing ones. Then builds the frame's draws. */
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
        int l = layer_of(p, j->settlement);
        PTile *t = l >= 0 ? &p->layer[l].tiles[slot_of(j->tx, j->tz)] : NULL;
        int want = t && t->state == PT_QUEUED && t->tx == j->tx && t->tz == j->tz;   /* else freed or replaced meanwhile */
        int pages = (j->t.n + PAGE_BOXES - 1) / PAGE_BOXES;
        if (want && pages > TILE_PAGES) fatal("proxies: a tile has more boxes than it can hold");
        if (want && staged > 0 && staged + (uint32_t)j->t.n > UPLOAD_BOXES) break;   /* the rest next frame */
        if (want && pages > p->nfree) { t->state = PT_FREE; t->retry = frame + RETRY_FRAMES; want = 0; p->starved++; }   /* asked for again later */
        if (!want) continue;
        t->n = j->t.n;
        t->npages = pages;
        memcpy(t->first, j->t.first, sizeof(t->first));
        memcpy(t->lod, j->t.lod, sizeof(t->lod));
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

    /* 2. Each layer's tiles in range, round the camera in its frame: free
     *    whatever holds their slots, queue the nearest missing. Which tiles
     *    can hold any of the city is asked in the layer's frame. */
    place_layers(p, cam, frame);
    int ncand = 0;
    p->inRange = p->readyTiles = 0;
    for (int l = 0; l < PROXY_LAYERS; ++l) {
        PLayer *L = &p->layer[l];
        if (L->settlement < 0) continue;
        const double *lc = L->cam;
        terrain_frame_bind(L->frame);
        int tx0 = (int)floor((lc[0] - PROXY_DISTANCE) / PROXY_TILE), tx1 = (int)floor((lc[0] + PROXY_DISTANCE) / PROXY_TILE);
        int tz0 = (int)floor((lc[2] - PROXY_DISTANCE) / PROXY_TILE), tz1 = (int)floor((lc[2] + PROXY_DISTANCE) / PROXY_TILE);
        for (int tz = tz0; tz <= tz1; ++tz)
            for (int tx = tx0; tx <= tx1; ++tx) {
                float d = tile_dist(tx, tz, lc);
                if (d > PROXY_DISTANCE) continue;
                p->inRange++;
                int s = slot_of(tx, tz);
                PTile *t = &L->tiles[s];
                if (t->state != PT_FREE && (t->tx != tx || t->tz != tz)) {
                    if (t->state == PT_READY) p->residentBoxes -= t->n;
                    free_tile(p, t, frame);   /* a queued job for it is simply not taken up */
                }
                if (t->state == PT_FREE && (t->tx != tx || t->tz != tz)) {   /* another tile's slot */
                    t->retry = 0;
                    t->tx = tx; t->tz = tz;
                    if (city_proxy_tile_empty(tx, tz)) t->state = PT_EMPTY;
                }
                if (t->state == PT_FREE && frame >= t->retry) { p->cand[ncand].layer = l; p->cand[ncand].slot = s; p->cand[ncand].d = d; ncand++; }
                else if (t->state == PT_READY || t->state == PT_EMPTY) p->readyTiles++;
            }
        terrain_frame_bind(NULL);
    }
    if (p->filled == 0.0 && p->readyTiles == p->inRange) p->filled = glfwGetTime() - p->created;
    if (ncand) {
        qsort(p->cand, (size_t)ncand, sizeof(PCand), cmp_cand);
        mutex_lock(&p->lock);
        for (int i = 0; i < ncand && p->tail - p->head < MAX_JOBS_P; ++i) {
            const PLayer *L = &p->layer[p->cand[i].layer];
            PTile *t = &p->layer[p->cand[i].layer].tiles[p->cand[i].slot];
            PJob *j = &p->job[p->tail % MAX_JOBS_P];
            t->state = PT_QUEUED;
            j->tx = t->tx;
            j->tz = t->tz;
            j->settlement = L->settlement;
            j->frame = L->frame;
            p->tail++;
        }
        cond_signal(&p->cond);
        mutex_unlock(&p->lock);
    }

    /* 3. Draws: the tiles in view, less the chunks drawn in full. A tile's
     *    origin and its boxes are turned into the active frame by its layer's
     *    (c, s) about the camera. K is the viewport's pixels per unit of
     *    tangent (as terrain_gpu.c's). */
    Frustum fr = frustum_from_matrix(*viewProj);
    const float *vm = viewProj->m;
    float K = 0.5f * (float)r->vk->extent.height * sqrtf(vm[1] * vm[1] + vm[5] * vm[5] + vm[9] * vm[9]);
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)p->indirect[slot].mapped;
    float (*org)[4] = (float (*)[4])p->origins[slot].mapped;
    uint32_t nd = 0;
    p->drawnTiles = p->drawnBoxes = 0;
    for (int l = 0; l < PROXY_LAYERS; ++l) {
        const PLayer *L = &p->layer[l];
        if (L->settlement < 0) continue;
        float c = L->c, sn = L->s;
        for (int s = 0; s < PT_SLOTS; ++s) {
            const PTile *t = &L->tiles[s];
            if (t->state != PT_READY || t->n == 0) continue;
            double rx = t->tx * PROXY_TILE - L->cam[0], rz = t->tz * PROXY_TILE - L->cam[2];
            float ox = (float)(c * rx - sn * rz), oz = (float)(sn * rx + c * rz);
            float bmn[3] = { ox + t->mn[0], t->mn[1] - (float)cam[1], oz + t->mn[2] };
            float bmx[3] = { ox + t->mx[0], t->mx[1] - (float)cam[1], oz + t->mx[2] };
            if (!L->active) {   /* the turned box's bounds on the ground */
                float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
                for (int q = 0; q < 4; ++q) {
                    float u = q & 1 ? t->mx[0] : t->mn[0], v = q & 2 ? t->mx[2] : t->mn[2];
                    float x = ox + c * u - sn * v, z = oz + sn * u + c * v;
                    x0 = fminf(x0, x); x1 = fmaxf(x1, x);
                    z0 = fminf(z0, z); z1 = fmaxf(z1, z);
                }
                bmn[0] = x0; bmx[0] = x1; bmn[2] = z0; bmx[2] = z1;
            }
            terrain_bend_bounds(bmn, bmx);   /* proxy.vert bends the boxes over the planet */
            if (!frustum_test_aabb(&fr, v3(bmn[0], bmn[1], bmn[2]), v3(bmx[0], bmx[1], bmx[2]))) continue;
            int inst = l * PT_SLOTS + s;
            org[inst][0] = ox; org[inst][1] = oz; org[inst][2] = c; org[inst][3] = sn;
            /* Level of detail: each chunk's boxes at least `need` metres (a pixel
             * or so at the tile's nearest), counted by the largest bin under it. */
            float need = PROXY_LOD_PX * tile_dist(t->tx, t->tz, L->cam) / K;
            int bin = -1;
            while (bin + 1 < PROXY_LOD_BINS && PROXY_LOD_SIZE(bin + 1) <= need) bin++;
            int run0 = -1, run1 = -1;
            for (int k = 0; k <= PROXY_TILE_CELLS; ++k) {
                /* the active frame's chunks drawn in full are left out; no other frame's are drawn */
                int keep = k < PROXY_TILE_CELLS && (!L->active || !chunks_drawn(r->chunks, t->tx * PROXY_TILE_CHUNKS + k % PROXY_TILE_CHUNKS,
                                                                                   t->tz * PROXY_TILE_CHUNKS + k / PROXY_TILE_CHUNKS));
                int end = keep ? t->first[k] + (bin < 0 ? t->first[k + 1] - t->first[k] : t->lod[k][bin]) : 0;
                if (keep && end == t->first[k]) keep = 0;   /* nothing big enough */
                if (keep && run0 >= 0 && run1 == t->first[k]) { run1 = end; continue; }   /* runs on from the last chunk */
                if (run0 >= 0) {
                    /* boxes [run0, run1), split where they cross from one page to the next */
                    for (int b = run0, e = run1; b < e && nd < MAX_DRAWS;) {
                        int pg = b / PAGE_BOXES, pe = (pg + 1) * PAGE_BOXES < e ? (pg + 1) * PAGE_BOXES : e;
                        VkDrawIndirectCommand *dc = &cmds[nd++];
                        dc->vertexCount = (uint32_t)(pe - b) * BOX_VERTS;
                        dc->instanceCount = 1;
                        dc->firstVertex = ((uint32_t)t->page[pg] * PAGE_BOXES + (uint32_t)(b - pg * PAGE_BOXES)) * BOX_VERTS;
                        dc->firstInstance = (uint32_t)inst;
                        p->drawnBoxes += pe - b;
                        b = pe;
                    }
                    run0 = -1;
                }
                if (keep) { run0 = t->first[k]; run1 = end; }
            }
            p->drawnTiles++;
        }
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
    snprintf(buf, n, "proxies: %d of %d tiles in range of %d cities ready%s (%d boxes, %d of %d pages%s), %d tiles and %d boxes drawn in %u draws",
             p->readyTiles, p->inRange, p->layers, filled, p->residentBoxes, POOL_PAGES_P - p->nfree - p->npending, POOL_PAGES_P, starved,
             p->drawnTiles, p->drawnBoxes, p->ndraw[(r->frameIndex + MAX_FRAMES - 1) % MAX_FRAMES]);
}
