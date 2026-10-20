#include "chunks.h"
#include "city.h"
#include "rt.h"
#include "thread.h"

#define MAX_ENTRIES    8192
#define HASH_BUCKETS   16384
#define MAX_JOBS       1024
#define MAX_WORKERS    16
#define KNOWN_BOUNDS   16384              /* chunks whose true bounds have been seen (a power of two) */
#define INDIRECT_CAP   (POOL_PAGES * 3)   /* the view's draws, the shelter map's, then the depth prepass's */

enum { CS_QUEUED = 1, CS_READY, CS_RESIDENT };

/* Layers: 0 the chunk itself, 1 its office lobbies (city_build_interiors),
 * wanted only within INTERIOR_DISTANCE of the camera, 2 its litter
 * (city_build_litter), wanted only within LITTER_DISTANCE and lit by the
 * chunk's own light table, 3 the lobbies' plants (city_build_plants), wanted
 * with the lobbies and drawn last, without back-face culling. */
#define LAYER_PLANTS 3

/* Prefetching (want_chunks). Ahead of a moving camera chunks are queued
 * sooner: their priority is cut by up to LEAD_GAIN straight ahead, in full
 * from LEAD_SPEED. Just outside the frustum's sides, within GUARD_DISTANCE,
 * chunks are queued too (after those in view), so turning finds them built:
 * the guard band widens the view by about GUARD_SLOPE (a tangent) each side. */
#define LEAD_GAIN      0.4f
#define LEAD_SPEED     10.0f    /* m/s */
#define MOTION_MAX     400.0f   /* m/s: faster is a jump */
#define GUARD_DISTANCE 700.0f
#define GUARD_SLOPE    0.18f
/* In-view chunks this near and not drawn yet count as missing (ChunkStats). */
#define MISSING_DISTANCE 700.0f
/* The pool's reach (relax_reach): when it has to evict chunks still wanted,
 * chunks are not queued again beyond the nearest of them, so they stay as
 * their proxies instead of being built, uploaded, drawn and evicted over
 * and over. It grows back at REACH_GROW while the pool has REACH_ROOM of its
 * pages free or held by chunks no longer wanted, and never falls under
 * REACH_MIN. */
#define REACH_MIN      500.0f
#define REACH_GROW     150.0f   /* m/s */
#define REACH_ROOM     0.4f
#define REACH_ALL      1e30f
typedef struct Entry {
    int      cx, cz, layer;
    int      state;
    int      next;          /* hash chain */
    uint64_t lastSeen;
    uint64_t drawn;         /* 1 + the last frame the view drew it in */
    Mesh     mesh;          /* CPU mesh while READY */
    uint16_t pages[MAX_CHUNK_PAGES];
    int      npages;
    int      lightPage;     /* -1 if the chunk has no light table */
    uint32_t vcount;
    float    mn[3], mx[3];  /* chunk-local bounds */
    SkyLight *sky;          /* kept while RESIDENT (chunks_sky_lights) */
    uint32_t  nsky;
    SkyLight *air;
    uint32_t  nair;
    Flame    *flame;        /* kept while RESIDENT (chunks_flames) */
    uint32_t  nflame;
    Plume    *plume;        /* kept while RESIDENT (chunks_plumes) */
    uint32_t  nplume;
    /* ray-traced shadows (src/render/rt.c): the casting triangles of each page, as
     * page-local vertex indices, kept while resident so the BLAS can be
     * rebuilt whenever the chunk comes back within RT_RANGE */
    uint16_t *rtIdx;
    uint32_t  rtCount[MAX_CHUNK_PAGES];
    uint32_t  rtTotal;
    int       blas;         /* -1: none */
} Entry;

typedef struct Job { int cx, cz, layer; } Job;
typedef struct KnownBounds { int cx, cz, used; float mn[3], mx[3]; } KnownBounds;
typedef struct Result { int cx, cz, layer; Mesh mesh; } Result;
typedef struct PendingFree { uint16_t page; uint64_t frame; } PendingFree;
/* key orders the draws (front to back); prio, the jobs (want_chunks). */
typedef struct Want { int cx, cz, layer; float key, prio; int inView; } Want;

struct ChunkSystem {
    VkCore *vk;

    /* chunk table (main thread only) */
    Entry    entries[MAX_ENTRIES];
    int      buckets[HASH_BUCKETS];
    int      freeEntries[MAX_ENTRIES];
    int      freeEntryCount;

    /* GPU page pool */
    Buffer       pool;
    uint16_t     freePages[POOL_PAGES];
    int          freePageCount;
    PendingFree *pendingFree;
    int          pendingFreeCount;

    /* light table pool: one page per resident chunk */
    Buffer       lightPool;
    uint16_t     freeLight[LIGHT_PAGES];
    int          freeLightCount;
    PendingFree *pendingLight;
    int          pendingLightCount;

    /* per frame-in-flight */
    Buffer   staging[MAX_FRAMES];
    Buffer   indirect[MAX_FRAMES];
    Buffer   chunkData[MAX_FRAMES];
    uint32_t drawCount[MAX_FRAMES], plantFirst[MAX_FRAMES];
    uint32_t shelterFirst[MAX_FRAMES], shelterCount[MAX_FRAMES];
    uint32_t preFirst[MAX_FRAMES], preCount[MAX_FRAMES];

    /* jobs (shared with workers) */
    Mutex    jobLock;
    CondVar  jobCond;
    Job      jobs[MAX_JOBS];
    int      jobHead, jobCount;
    int      quit;
    Mutex    doneLock;
    Result  *done;
    int      doneCount, doneCap;
    Thread  *workers[MAX_WORKERS];
    int      workerCount;

    /* scratch */
    Want    *want;
    int     *wantOrder;              /* want indices by prio (queue_wanted) */
    int      wantCount, wantCap;
    /* the camera's motion (track_motion) */
    double   motionCam[2], motionTime;
    float    vel[2];
    int      inViewMissing;
    float    reach;           /* chunks are queued only this near (m; relax_reach) */
    double   reachTime;
    int      ready[MAX_ENTRIES];
    int      readyCount;

    Rt        *rt;      /* NULL without ray queries */
    /* The mesh bounds of chunks generated before, kept after they are evicted.
     * A chunk is a pure function of its coordinates, so they stay true, and the
     * frustum test needn't fall back to the conservative bounds (which can
     * want a chunk that its true bounds then don't, over and over). */
    KnownBounds known[KNOWN_BOUNDS];
    int        uploaded[MAX_ENTRIES];

    uint64_t   frame;   /* this frame's index (chunks_update) */
    ChunkStats stats;
    SkyGrid    sky;     /* built here, then copied out: the destination is write-combined GPU memory */
    SkyLight   airCand[16384];
    float      airScore[16384];
    int        airOrder[16384];
};

/* ---------------------------------------------------------------- table --- */

static uint32_t bucket_of(int cx, int cz, int layer) {
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u ^ (uint32_t)layer * 83492791u;
    return (h ^ (h >> 15)) & (HASH_BUCKETS - 1);
}

static int table_find(ChunkSystem *cs, int cx, int cz, int layer) {
    for (int i = cs->buckets[bucket_of(cx, cz, layer)]; i >= 0; i = cs->entries[i].next)
        if (cs->entries[i].cx == cx && cs->entries[i].cz == cz && cs->entries[i].layer == layer) return i;
    return -1;
}

static int table_insert(ChunkSystem *cs, int cx, int cz, int layer) {
    if (!cs->freeEntryCount) return -1;
    int i = cs->freeEntries[--cs->freeEntryCount];
    Entry *e = &cs->entries[i];
    memset(e, 0, sizeof(*e));
    e->cx = cx; e->cz = cz; e->layer = layer;
    e->lightPage = -1;
    e->blas = -1;
    uint32_t b = bucket_of(cx, cz, layer);
    e->next = cs->buckets[b];
    cs->buckets[b] = i;
    return i;
}

static void table_remove(ChunkSystem *cs, int idx) {
    Entry *e = &cs->entries[idx];
    uint32_t b = bucket_of(e->cx, e->cz, e->layer);
    int *link = &cs->buckets[b];
    while (*link >= 0 && *link != idx) link = &cs->entries[*link].next;
    if (*link == idx) *link = e->next;
    mesh_free(&e->mesh);
    free(e->sky);
    free(e->air);
    free(e->flame);
    free(e->plume);
    free(e->rtIdx);
    e->rtIdx = NULL;
    e->sky = e->air = NULL;
    e->flame = NULL;
    e->plume = NULL;
    e->nsky = e->nair = e->nflame = e->nplume = 0;
    e->state = 0;
    cs->freeEntries[cs->freeEntryCount++] = idx;
}

/* -------------------------------------------------------------- workers --- */

static int worker_main(void *arg) {
    ChunkSystem *cs = (ChunkSystem *)arg;
    for (;;) {
        mutex_lock(&cs->jobLock);
        while (cs->jobHead == cs->jobCount && !cs->quit) cond_wait(&cs->jobCond, &cs->jobLock);
        if (cs->quit) { mutex_unlock(&cs->jobLock); break; }
        Job job = cs->jobs[cs->jobHead++];
        mutex_unlock(&cs->jobLock);

        Result r;
        r.cx = job.cx; r.cz = job.cz; r.layer = job.layer;
        memset(&r.mesh, 0, sizeof(r.mesh));
        if (job.layer == 1) city_build_interiors(job.cx, job.cz, &r.mesh);
        else if (job.layer == 2) city_build_litter(job.cx, job.cz, &r.mesh);
        else if (job.layer == LAYER_PLANTS) city_build_plants(job.cx, job.cz, &r.mesh);
        else city_build_mesh(job.cx, job.cz, &r.mesh);

        mutex_lock(&cs->doneLock);
        if (cs->doneCount == cs->doneCap) {
            cs->doneCap = cs->doneCap ? cs->doneCap * 2 : 256;
            cs->done = (Result *)realloc(cs->done, sizeof(Result) * cs->doneCap);
        }
        cs->done[cs->doneCount++] = r;
        mutex_unlock(&cs->doneLock);
    }
    return 0;
}

/* --------------------------------------------------------------- create --- */

ChunkSystem *chunks_create(VkCore *vk) {
    ChunkSystem *cs = (ChunkSystem *)calloc(1, sizeof(ChunkSystem));
    if (!cs) fatal("out of memory");
    cs->vk = vk;
    for (int i = 0; i < HASH_BUCKETS; ++i) cs->buckets[i] = -1;
    for (int i = 0; i < MAX_ENTRIES; ++i) cs->freeEntries[i] = MAX_ENTRIES - 1 - i;
    cs->freeEntryCount = MAX_ENTRIES;
    cs->reach = REACH_ALL;
    for (int i = 0; i < POOL_PAGES; ++i) cs->freePages[i] = (uint16_t)(POOL_PAGES - 1 - i);
    cs->freePageCount = POOL_PAGES;
    cs->pendingFree = (PendingFree *)malloc(sizeof(PendingFree) * POOL_PAGES);
    for (int i = 0; i < LIGHT_PAGES; ++i) cs->freeLight[i] = (uint16_t)(LIGHT_PAGES - 1 - i);
    cs->freeLightCount = LIGHT_PAGES;
    cs->pendingLight = (PendingFree *)malloc(sizeof(PendingFree) * LIGHT_PAGES);

    VkDeviceSize poolBytes = (VkDeviceSize)POOL_PAGES * PAGE_VERTS * sizeof(Vertex);
    VkBufferUsageFlags poolUse = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vk->rt) poolUse |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    cs->pool = vk_buffer(vk, poolBytes, poolUse, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cs->rt = rt_create(vk);
    cs->lightPool = vk_buffer(vk, chunks_light_buffer_size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        cs->staging[i]   = vk_buffer(vk, STAGING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        cs->indirect[i]  = vk_buffer(vk, sizeof(VkDrawIndirectCommand) * INDIRECT_CAP, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
        cs->chunkData[i] = vk_buffer(vk, chunks_gpu_buffer_size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
    }

    mutex_init(&cs->jobLock);
    cond_init(&cs->jobCond);
    mutex_init(&cs->doneLock);
    int n = cpu_count() - 2;
    if (n < 1) n = 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    cs->workerCount = n;
    for (int i = 0; i < n; ++i) cs->workers[i] = thread_create(worker_main, cs);
    printf("Chunk streamer: %d worker threads, %.0f MB geometry pool, %.0f MB light pool\n", n, poolBytes / (1024.0 * 1024.0),
           chunks_light_buffer_size() / (1024.0 * 1024.0));
    return cs;
}

void chunks_destroy(ChunkSystem *cs) {
    mutex_lock(&cs->jobLock);
    cs->quit = 1;
    cond_broadcast(&cs->jobCond);
    mutex_unlock(&cs->jobLock);
    for (int i = 0; i < cs->workerCount; ++i) thread_join(cs->workers[i]);
    for (int i = 0; i < cs->doneCount; ++i) mesh_free(&cs->done[i].mesh);
    free(cs->done);
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        mesh_free(&cs->entries[i].mesh); free(cs->entries[i].sky); free(cs->entries[i].air); free(cs->entries[i].flame); free(cs->entries[i].plume); free(cs->entries[i].rtIdx);
    }
    rt_destroy(cs->rt);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vk_buffer_destroy(cs->vk, &cs->staging[i]);
        vk_buffer_destroy(cs->vk, &cs->indirect[i]);
        vk_buffer_destroy(cs->vk, &cs->chunkData[i]);
    }
    vk_buffer_destroy(cs->vk, &cs->pool);
    vk_buffer_destroy(cs->vk, &cs->lightPool);
    mutex_destroy(&cs->jobLock);
    cond_destroy(&cs->jobCond);
    mutex_destroy(&cs->doneLock);
    free(cs->pendingFree);
    free(cs->pendingLight);
    free(cs->want);
    free(cs->wantOrder);
    free(cs);
}

VkBuffer chunks_gpu_buffer(ChunkSystem *cs, int slot) { return cs->chunkData[slot].buf; }
VkDeviceSize chunks_gpu_buffer_size(void) { return sizeof(ChunkGPU) * MAX_DRAW_CHUNKS; }
VkBuffer chunks_light_buffer(ChunkSystem *cs) { return cs->lightPool.buf; }
Rt *chunks_rt(ChunkSystem *cs) { return cs->rt; }
VkDeviceSize chunks_light_buffer_size(void) { return (VkDeviceSize)LIGHT_PAGES * LIGHT_PAGE_RECS * 16; }
ChunkStats chunks_stats(ChunkSystem *cs) { return cs->stats; }

static KnownBounds *known_slot(ChunkSystem *cs, int cx, int cz, bool insert) {
    uint32_t h = bucket_of(cx, cz, 0) & (KNOWN_BOUNDS - 1);
    for (int i = 0; i < 8; ++i) {
        KnownBounds *k = &cs->known[(h + i) & (KNOWN_BOUNDS - 1)];
        if (k->used && k->cx == cx && k->cz == cz) return k;
        if (!k->used) return insert ? k : NULL;
    }
    return insert ? &cs->known[h] : NULL;   /* full run: overwrite, it's only a cache */
}

/* ---------------------------------------------------------------- pages --- */

static void evict(ChunkSystem *cs, int idx, uint64_t frame) {
    Entry *e = &cs->entries[idx];
    for (int i = 0; i < e->npages; ++i) {
        cs->pendingFree[cs->pendingFreeCount].page = e->pages[i];
        cs->pendingFree[cs->pendingFreeCount].frame = frame;
        cs->pendingFreeCount++;
    }
    e->npages = 0;
    if (cs->rt) rt_free_blas(cs->rt, e->blas, frame);
    e->blas = -1;
    if (e->lightPage >= 0) {
        cs->pendingLight[cs->pendingLightCount].page = (uint16_t)e->lightPage;
        cs->pendingLight[cs->pendingLightCount].frame = frame;
        cs->pendingLightCount++;
        e->lightPage = -1;
    }
    table_remove(cs, idx);
}

static float entry_dist2(const Entry *e, const double cam[3]) {
    double x = (e->cx + 0.5) * CHUNK_SIZE - cam[0], z = (e->cz + 0.5) * CHUNK_SIZE - cam[2];
    return (float)(x * x + z * z);
}

typedef struct EvictCand { int idx; double key; } EvictCand;
static int cmp_evict(const void *a, const void *b) {
    double ka = ((const EvictCand *)a)->key, kb = ((const EvictCand *)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

/* Keep a reserve of free pages. Chunks not wanted this frame go first, oldest
 * first. If that is not enough and nearer chunks are waiting to upload, the
 * farthest visible chunks (deep in the haze) are dropped so the space always
 * goes to what is closest to the player. Evicted pages only become reusable
 * once every frame that might still draw from them has retired. */
static void evict_to_watermark(ChunkSystem *cs, uint64_t frame, const double cam[3]) {
    const int low = POOL_PAGES / 10, high = POOL_PAGES / 5;
    const int lightLow = LIGHT_PAGES / 10, lightHigh = LIGHT_PAGES / 5;
#define POOLS_LOW()  (cs->freePageCount + cs->pendingFreeCount < low || cs->freeLightCount + cs->pendingLightCount < lightLow)
#define POOLS_HIGH() (cs->freePageCount + cs->pendingFreeCount >= high && cs->freeLightCount + cs->pendingLightCount >= lightHigh)
    if (!POOLS_LOW()) return;
    float nearestReady = 1e30f;
    for (int i = 0; i < cs->readyCount; ++i) {
        float d = entry_dist2(&cs->entries[cs->ready[i]], cam);
        if (d < nearestReady) nearestReady = d;
    }
    const float margin = (2.0f * CHUNK_SIZE) * (2.0f * CHUNK_SIZE);
    static EvictCand cand[MAX_ENTRIES];
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT) continue;
        if (e->lastSeen < frame) {
            cand[n].idx = i; cand[n].key = (double)e->lastSeen - 1e15; n++;
        } else {
            float d = entry_dist2(e, cam);
            if (d > nearestReady * 1.5f + margin) { cand[n].idx = i; cand[n].key = -(double)d; n++; }
        }
    }
    qsort(cand, n, sizeof(EvictCand), cmp_evict);
    for (int i = 0; i < n && !POOLS_HIGH(); ++i) {
        Entry *e = &cs->entries[cand[i].idx];
        if (e->lastSeen >= frame) {   /* still wanted: the pool can't hold this far out (relax_reach) */
            float d = sqrtf(entry_dist2(e, cam)) - CHUNK_SIZE;
            cs->reach = fminf(cs->reach, fmaxf(d, REACH_MIN));
        }
        evict(cs, cand[i].idx, frame);
    }
#undef POOLS_LOW
#undef POOLS_HIGH
}

/* Lets the reach grow back while the pool has room for more: REACH_ROOM of its
 * pages free, waiting to be, or held by chunks not wanted this frame (which
 * evict_to_watermark takes first). After an eviction of wanted chunks the pool
 * is full of wanted ones, so the reach stays where that left it until the view
 * changes. Called after queue_wanted, which marks this frame's wanted chunks. */
static void relax_reach(ChunkSystem *cs, uint64_t frame) {
    double now = glfwGetTime();
    float dt = (float)fmin(now - cs->reachTime, 0.1);
    cs->reachTime = now;
    if (cs->reach >= REACH_ALL) return;
    int room = cs->freePageCount + cs->pendingFreeCount;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const Entry *e = &cs->entries[i];
        if (e->state == CS_RESIDENT && e->lastSeen < frame) room += e->npages;
    }
    if (room < (int)(REACH_ROOM * POOL_PAGES)) return;
    cs->reach += REACH_GROW * dt;
    if (cs->reach > VIEW_DISTANCE + 2.0f * CHUNK_SIZE) cs->reach = REACH_ALL;
}

static void reclaim_pages(ChunkSystem *cs, uint64_t frame) {
    int w = 0;
    for (int i = 0; i < cs->pendingFreeCount; ++i) {
        PendingFree pf = cs->pendingFree[i];
        if (frame >= pf.frame + MAX_FRAMES) cs->freePages[cs->freePageCount++] = pf.page;
        else cs->pendingFree[w++] = pf;
    }
    cs->pendingFreeCount = w;
    w = 0;
    for (int i = 0; i < cs->pendingLightCount; ++i) {
        PendingFree pf = cs->pendingLight[i];
        if (frame >= pf.frame + MAX_FRAMES) cs->freeLight[cs->freeLightCount++] = pf.page;
        else cs->pendingLight[w++] = pf;
    }
    cs->pendingLightCount = w;
}

/* -------------------------------------------------------------- shadows --- */

/* The casting triangles of each page, as page-local indices (city_casts_shadow). */
static void caster_indices(Entry *e, const Vertex *v, uint32_t vc) {
    free(e->rtIdx);
    e->rtIdx = NULL;
    e->rtTotal = 0;
    memset(e->rtCount, 0, sizeof(e->rtCount));
    if (e->layer == LAYER_PLANTS) return;   /* lit by the lobby panels only, which cast no shadows yet */
    uint16_t *idx = (uint16_t *)malloc(sizeof(uint16_t) * (vc ? vc : 1));
    if (!idx) fatal("out of memory");
    uint32_t n = 0;
    for (int p = 0; p < e->npages; ++p) {
        uint32_t first = p * PAGE_VERTS, cnt = vc - first < PAGE_VERTS ? vc - first : PAGE_VERTS, start = n;
        for (uint32_t t = 0; t + 2 < cnt; t += 3) {
            if (!city_casts_shadow(&v[first + t])) continue;
            idx[n++] = (uint16_t)t; idx[n++] = (uint16_t)(t + 1); idx[n++] = (uint16_t)(t + 2);
        }
        e->rtCount[p] = n - start;
    }
    if (!n) { free(idx); return; }
    e->rtIdx = idx;
    e->rtTotal = n;
}

static uint32_t rt_mask(int layer) { return layer == 0 ? RT_MASK_CITY : layer == 1 ? RT_MASK_INTERIOR : RT_MASK_LITTER; }

/* Distance from the camera to the chunk's square, horizontally. */
static float chunk_dist(const Entry *e, const double cam[3]) {
    float x0 = (float)(e->cx * (double)CHUNK_SIZE - cam[0]), z0 = (float)(e->cz * (double)CHUNK_SIZE - cam[2]);
    float nx = clampf(0.0f, x0, x0 + CHUNK_SIZE), nz = clampf(0.0f, z0, z0 + CHUNK_SIZE);
    return sqrtf(nx * nx + nz * nz);
}

static bool build_blas(ChunkSystem *cs, VkCommandBuffer cmd, int slot, Entry *e) {
    uint32_t verts[MAX_CHUNK_PAGES];
    for (int p = 0; p < e->npages; ++p) {
        uint32_t first = p * PAGE_VERTS;
        verts[p] = e->vcount - first < PAGE_VERTS ? e->vcount - first : PAGE_VERTS;
    }
    e->blas = rt_build_blas(cs->rt, cmd, slot, cs->pool.buf, e->pages, e->npages, verts, e->rtIdx, e->rtCount);
    return e->blas >= 0;
}

typedef struct RtCand { int idx; float d; } RtCand;
static int cmp_rtcand(const void *a, const void *b) {
    float da = ((const RtCand *)a)->d, db = ((const RtCand *)b)->d;
    return da < db ? -1 : da > db ? 1 : 0;
}

/* Bottom-level structures for resident chunks within RT_RANGE (nearest first,
 * within this frame's build budget), dropped beyond it; then the frame's TLAS
 * over all of them, wherever the camera is looking. */
static void update_shadows(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint64_t frame, const double cam[3]) {
    static RtCand cand[MAX_ENTRIES];
    int n = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || !e->rtTotal) continue;
        float d = chunk_dist(e, cam);
        if (e->blas >= 0 && d > RT_RANGE + RT_HYSTERESIS) {
            rt_free_blas(cs->rt, e->blas, frame);
            e->blas = -1;
        } else if (e->blas < 0 && d < RT_RANGE) {
            cand[n].idx = i; cand[n].d = d; n++;
        }
    }
    qsort(cand, n, sizeof(RtCand), cmp_rtcand);
    for (int i = 0; i < n; ++i) {
        Entry *e = &cs->entries[cand[i].idx];
        if (!rt_can_build(cs->rt, slot, e->rtTotal)) break;
        build_blas(cs, cmd, slot, e);
    }

    const float P = CHUNK_SIZE;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || e->blas < 0) continue;
        float o[3] = { (float)(e->cx * (double)P - cam[0]), -(float)cam[1], (float)(e->cz * (double)P - cam[2]) };
        rt_add_instance(cs->rt, slot, e->blas, o, rt_mask(e->layer));
    }
    rt_build_tlas(cs->rt, cmd, slot);
}

/* --------------------------------------------------------------- update --- */

static int cmp_want(const void *a, const void *b) {
    float ka = ((const Want *)a)->key, kb = ((const Want *)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void push_want(ChunkSystem *cs, int cx, int cz, int layer, float key, float prio, int inView) {
    if (cs->wantCount == cs->wantCap) {
        cs->wantCap = cs->wantCap ? cs->wantCap * 2 : 4096;
        cs->want = (Want *)realloc(cs->want, sizeof(Want) * cs->wantCap);
        cs->wantOrder = (int *)realloc(cs->wantOrder, sizeof(int) * cs->wantCap);
        if (!cs->want || !cs->wantOrder) fatal("out of memory growing the chunk wants");
    }
    Want *w = &cs->want[cs->wantCount++];
    w->cx = cx; w->cz = cz; w->layer = layer; w->key = key; w->prio = prio; w->inView = inView;
}

static const Want *g_prioWant;
static int cmp_prio(const void *a, const void *b) {
    float pa = g_prioWant[*(const int *)a].prio, pb = g_prioWant[*(const int *)b].prio;
    return pa < pb ? -1 : pa > pb ? 1 : *(const int *)a - *(const int *)b;
}

/* The camera's ground velocity (m/s), measured over a quarter second or so:
 * the lead want_chunks gives the chunks ahead. A jump (or the first frame)
 * reads as standing still. */
static void track_motion(ChunkSystem *cs, const double cam[3]) {
    double now = glfwGetTime(), dt = now - cs->motionTime;
    if (cs->motionTime > 0.0 && dt < 0.25) return;
    double vx = (cam[0] - cs->motionCam[0]) / dt, vz = (cam[2] - cs->motionCam[1]) / dt;
    int moved = cs->motionTime > 0.0 && dt < 1.0 && vx * vx + vz * vz < MOTION_MAX * MOTION_MAX;
    cs->vel[0] = moved ? (float)vx : 0.0f;
    cs->vel[1] = moved ? (float)vz : 0.0f;
    cs->motionCam[0] = cam[0];
    cs->motionCam[1] = cam[2];
    cs->motionTime = now;
}

static const double *g_sortCam;
static ChunkSystem *g_sortCs;
static int cmp_ready(const void *a, const void *b) {
    float da = entry_dist2(&g_sortCs->entries[*(const int *)a], g_sortCam);
    float db = entry_dist2(&g_sortCs->entries[*(const int *)b], g_sortCam);
    return da < db ? -1 : da > db ? 1 : 0;
}

/* 1. Collect finished meshes from the workers. */
static void collect_finished(ChunkSystem *cs) {
    mutex_lock(&cs->doneLock);
    for (int i = 0; i < cs->doneCount; ++i) {
        Result *r = &cs->done[i];
        int idx = table_find(cs, r->cx, r->cz, r->layer);
        if (idx >= 0 && cs->entries[idx].state == CS_QUEUED) {
            cs->entries[idx].mesh = r->mesh;
            cs->entries[idx].state = CS_READY;
            cs->ready[cs->readyCount++] = idx;
        } else {
            mesh_free(&r->mesh);
        }
    }
    cs->doneCount = 0;
    mutex_unlock(&cs->doneLock);

}

/* 2. Withdraw jobs no worker has started yet; they are re-prioritised below. */
static void withdraw_jobs(ChunkSystem *cs) {
    mutex_lock(&cs->jobLock);
    for (int i = cs->jobHead; i < cs->jobCount; ++i) {
        int idx = table_find(cs, cs->jobs[i].cx, cs->jobs[i].cz, cs->jobs[i].layer);
        if (idx >= 0 && cs->entries[idx].state == CS_QUEUED) table_remove(cs, idx);
    }
    cs->jobHead = cs->jobCount = 0;
    mutex_unlock(&cs->jobLock);

}

/* 3. Frustum fill: every chunk inside the view distance whose conservative
 *    bounds touch the frustum, plus a ring around the player and a guard
 *    band beside the frustum; and the lobbies and litter of the chunks near
 *    the camera, just behind the chunks themselves. Their priority leads a
 *    moving camera (see LEAD_GAIN). */
static void want_chunks(ChunkSystem *cs, const Frustum *frp, const double cam[3]) {
    const float P = CHUNK_SIZE;
    const Frustum fr = *frp;
    cs->wantCount = 0;
    cs->inViewMissing = 0;
    track_motion(cs, cam);
    float speed = sqrtf(cs->vel[0] * cs->vel[0] + cs->vel[1] * cs->vel[1]);
    float lead = LEAD_GAIN * minf(speed / LEAD_SPEED, 1.0f);
    int ccx = (int)floor(cam[0] / P), ccz = (int)floor(cam[2] / P);
    int rad = (int)ceilf(VIEW_DISTANCE / P) + 1;
    for (int dz = -rad; dz <= rad; ++dz) {
        for (int dx = -rad; dx <= rad; ++dx) {
            int cx = ccx + dx, cz = ccz + dz;
            float x0 = (float)(cx * (double)P - cam[0]), z0 = (float)(cz * (double)P - cam[2]);
            float nx = clampf(0.0f, x0, x0 + P), nz = clampf(0.0f, z0, z0 + P);
            float d = sqrtf(nx * nx + nz * nz);
            if (d > VIEW_DISTANCE) continue;
            int idx = table_find(cs, cx, cz, 0);
            float y1;
            vec3 mn, mx;
            const KnownBounds *kb = known_slot(cs, cx, cz, false);
            if (idx >= 0 && cs->entries[idx].state == CS_RESIDENT) {
                Entry *e = &cs->entries[idx];
                mn = v3(x0 + e->mn[0], e->mn[1] - (float)cam[1], z0 + e->mn[2]);
                mx = v3(x0 + e->mx[0], e->mx[1] - (float)cam[1], z0 + e->mx[2]);
            } else if (kb) {
                mn = v3(x0 + kb->mn[0], kb->mn[1] - (float)cam[1], z0 + kb->mn[2]);
                mx = v3(x0 + kb->mx[0], kb->mx[1] - (float)cam[1], z0 + kb->mx[2]);
            } else {
                /* Buildings centred in the chunk may overhang it. */
                float g0, g1;
                city_ground_range(cx, cz, &g0, &g1);
                y1 = g1 + city_height_cap(cx, cz);
                mn = v3(x0 - CITY_OVERHANG, g0 - (float)cam[1] - 1.0f, z0 - CITY_OVERHANG);
                mx = v3(x0 + P + CITY_OVERHANG, y1 - (float)cam[1], z0 + P + CITY_OVERHANG);
            }
            int inView = frustum_test_aabb(&fr, mn, mx);
            /* Within a light's reach of the frustum, a chunk can shadow what is
             * in view, so it is kept too: shadows never wait for it to stream in. */
            const float R = SHADOW_REACH;
            int casts = !inView && cs->rt && d < RT_RANGE &&
                        frustum_test_aabb(&fr, v3(mn.x - R, mn.y - R, mn.z - R), v3(mx.x + R, mx.y + R, mx.z + R));
            const float G = GUARD_SLOPE * d;
            int guard = !inView && !casts && d < GUARD_DISTANCE &&
                        frustum_test_aabb(&fr, v3(mn.x - G, mn.y, mn.z - G), v3(mx.x + G, mx.y, mx.z + G));
            /* ahead of the camera's motion: the cosine to the chunk's nearest point */
            float ahead = d > 0.0f && speed > 0.0f ? maxf((nx * cs->vel[0] + nz * cs->vel[1]) / (d * speed), 0.0f) : 0.0f;
            float f = 1.0f - lead * ahead;
            if (inView) push_want(cs, cx, cz, 0, d, d * f, 1);
            else if (casts) push_want(cs, cx, cz, 0, d + 40.0f, (d + 40.0f) * f, 0);
            else if (d < NEAR_RING_DIST) push_want(cs, cx, cz, 0, d * 3.0f + 200.0f, (d * 3.0f + 200.0f) * f, 0);
            else if (guard) push_want(cs, cx, cz, 0, d * 1.3f + 100.0f, (d * 1.3f + 100.0f) * f, 0);
            if (inView && d < MISSING_DISTANCE && (idx < 0 || cs->entries[idx].state != CS_RESIDENT)) cs->inViewMissing++;
            float kl = d + 60.0f + (inView ? 0.0f : d * 2.0f), kp = d + 70.0f + (inView ? 0.0f : d * 2.0f), kt = d + 30.0f + (inView ? 0.0f : d * 2.0f);
            if (d < INTERIOR_DISTANCE) push_want(cs, cx, cz, 1, kl, kl * f, inView);
            if (d < INTERIOR_DISTANCE) push_want(cs, cx, cz, LAYER_PLANTS, kp, kp * f, inView);
            if (d < LITTER_DISTANCE) push_want(cs, cx, cz, 2, kt, kt * f, inView);
        }
    }
    qsort(cs->want, cs->wantCount, sizeof(Want), cmp_want);

}

/* Queues the wanted chunks not yet known and within the pool's reach, by
 * priority, for the workers. Returns how many wanted chunks are queued. */
static int queue_wanted(ChunkSystem *cs, uint64_t frame, const double cam[3]) {
    const double P = CHUNK_SIZE, reach2 = (double)cs->reach * cs->reach;
    Job newJobs[MAX_JOBS];
    int nj = 0, queued = 0;
    for (int i = 0; i < cs->wantCount; ++i) cs->wantOrder[i] = i;
    g_prioWant = cs->want;
    qsort(cs->wantOrder, (size_t)cs->wantCount, sizeof(int), cmp_prio);
    for (int i = 0; i < cs->wantCount; ++i) {
        Want *w = &cs->want[cs->wantOrder[i]];
        int idx = table_find(cs, w->cx, w->cz, w->layer);
        if (idx < 0) {
            if (nj >= MAX_JOBS || cs->freeEntryCount < 64) continue;
            double x = (w->cx + 0.5) * P - cam[0], z = (w->cz + 0.5) * P - cam[2];
            if (x * x + z * z > reach2) continue;   /* its proxies stand in (relax_reach) */
            idx = table_insert(cs, w->cx, w->cz, w->layer);
            if (idx < 0) continue;
            cs->entries[idx].state = CS_QUEUED;
            newJobs[nj].cx = w->cx; newJobs[nj].cz = w->cz; newJobs[nj].layer = w->layer; nj++;
        }
        cs->entries[idx].lastSeen = frame;
        if (cs->entries[idx].state == CS_QUEUED) queued++;
    }
    if (nj) {
        mutex_lock(&cs->jobLock);
        memcpy(cs->jobs, newJobs, sizeof(Job) * nj);
        cs->jobHead = 0;
        cs->jobCount = nj;
        cond_broadcast(&cs->jobCond);
        mutex_unlock(&cs->jobLock);
    }
    return queued;
}

/* 4. Upload ready meshes, nearest first, within the staging budget; build
 *    the BLAS of those arriving within shadow range. Returns how many. */
static int upload_ready(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint64_t frame, const double cam[3]) {
    evict_to_watermark(cs, frame, cam);
    g_sortCam = cam; g_sortCs = cs;
    qsort(cs->ready, cs->readyCount, sizeof(int), cmp_ready);
    Buffer *stg = &cs->staging[slot];
    VkDeviceSize stgOff = 0;
    VkBufferCopy regions[512], lightRegions[256];
    int nreg = 0, nlreg = 0, uploads = 0, r = 0;
    for (; r < cs->readyCount; ++r) {
        Entry *e = &cs->entries[cs->ready[r]];
        uint32_t vc = e->mesh.count;
        int np = (int)((vc + PAGE_VERTS - 1) / PAGE_VERTS);
        if (np > MAX_CHUNK_PAGES) { np = MAX_CHUNK_PAGES; vc = np * PAGE_VERTS; fprintf(stderr, "chunk too large, truncated\n"); }
        VkDeviceSize bytes = (VkDeviceSize)vc * sizeof(Vertex);
        VkDeviceSize lbytes = (VkDeviceSize)e->mesh.nlights * 16;
        VkDeviceSize lightOff = (stgOff + bytes + 255) & ~(VkDeviceSize)255;
        if (lightOff + lbytes > STAGING_BYTES || nreg + np > (int)ARRAY_LEN(regions) || nlreg >= (int)ARRAY_LEN(lightRegions)) break;
        if (cs->freePageCount < np || (lbytes && !cs->freeLightCount)) break;
        if (lbytes) {
            uint16_t lp = cs->freeLight[--cs->freeLightCount];
            memcpy((uint8_t *)stg->mapped + lightOff, e->mesh.lights, lbytes);
            lightRegions[nlreg].srcOffset = lightOff;
            lightRegions[nlreg].dstOffset = (VkDeviceSize)lp * LIGHT_PAGE_RECS * 16;
            lightRegions[nlreg].size = lbytes;
            nlreg++;
            e->lightPage = lp;
        }
        memcpy((uint8_t *)stg->mapped + stgOff, e->mesh.v, bytes);
        for (int p = 0; p < np; ++p) {
            uint16_t page = cs->freePages[--cs->freePageCount];
            uint32_t first = p * PAGE_VERTS;
            uint32_t cnt = vc - first < PAGE_VERTS ? vc - first : PAGE_VERTS;
            e->pages[p] = page;
            regions[nreg].srcOffset = stgOff + (VkDeviceSize)first * sizeof(Vertex);
            regions[nreg].dstOffset = (VkDeviceSize)page * PAGE_VERTS * sizeof(Vertex);
            regions[nreg].size = (VkDeviceSize)cnt * sizeof(Vertex);
            nreg++;
        }
        stgOff = (lightOff + lbytes + 255) & ~(VkDeviceSize)255;
        e->npages = np;
        e->vcount = vc;
        if (cs->rt) {
            caster_indices(e, e->mesh.v, vc);
            cs->uploaded[uploads] = cs->ready[r];
        }
        memcpy(e->mn, e->mesh.mn, sizeof(e->mn));
        memcpy(e->mx, e->mesh.mx, sizeof(e->mx));
        if (e->layer == 0) {
            KnownBounds *kb = known_slot(cs, e->cx, e->cz, true);
            kb->cx = e->cx; kb->cz = e->cz; kb->used = 1;
            memcpy(kb->mn, e->mn, sizeof(kb->mn));
            memcpy(kb->mx, e->mx, sizeof(kb->mx));
        }
        e->sky = e->mesh.sky;
        e->nsky = e->mesh.nsky;
        e->mesh.sky = NULL;
        e->air = e->mesh.air;
        e->nair = e->mesh.nair;
        e->mesh.air = NULL;
        e->flame = e->mesh.flame;
        e->nflame = e->mesh.nflame;
        e->mesh.flame = NULL;
        e->plume = e->mesh.plume;
        e->nplume = e->mesh.nplume;
        e->mesh.plume = NULL;
        mesh_free(&e->mesh);
        e->state = CS_RESIDENT;
        uploads++;
    }
    memmove(cs->ready, cs->ready + r, sizeof(int) * (cs->readyCount - r));
    cs->readyCount -= r;
    if (nreg) vkCmdCopyBuffer(cmd, stg->buf, cs->pool.buf, (uint32_t)nreg, regions);
    if (nlreg) vkCmdCopyBuffer(cmd, stg->buf, cs->lightPool.buf, (uint32_t)nlreg, lightRegions);
    if (nreg || nlreg) {
        /* the BLAS builds read the vertices too, when there are any (the stage needs the feature) */
        VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        if (cs->rt) readers |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, readers,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT);
    }
    /* A chunk arriving within range gets its BLAS in the frame it is first
     * drawn, so it never appears without its shadows. */
    for (int u = 0; cs->rt && u < uploads; ++u) {
        Entry *e = &cs->entries[cs->uploaded[u]];
        if (e->rtTotal && chunk_dist(e, cam) < RT_RANGE) build_blas(cs, cmd, slot, e);
    }
    return uploads;
}

/* 5. Build the indirect draw list from resident, visible chunks (front to
 *    back), the plants' layer last: it is drawn with its own pipeline.
 *    Returns the draws; *nchunkOut, the chunk records used. */
static uint32_t build_draw_list(ChunkSystem *cs, int slot, const Frustum *frp, const double cam[3], uint32_t *nchunkOut) {
    const float P = CHUNK_SIZE;
    const Frustum fr = *frp;
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)cs->indirect[slot].mapped;
    ChunkGPU *gpu = (ChunkGPU *)cs->chunkData[slot].mapped;
    uint32_t ndraw = 0, nchunk = 0;
    cs->plantFirst[slot] = UINT32_MAX;
    for (int k = 0; k < 2 * cs->wantCount && nchunk < MAX_DRAW_CHUNKS; ++k) {
        if (k == cs->wantCount) cs->plantFirst[slot] = ndraw;
        Want *w = &cs->want[k % cs->wantCount];
        if (!w->inView || (w->layer == LAYER_PLANTS) != (k >= cs->wantCount)) continue;
        int idx = table_find(cs, w->cx, w->cz, w->layer);
        if (idx < 0 || cs->entries[idx].state != CS_RESIDENT || !cs->entries[idx].npages) continue;
        Entry *e = &cs->entries[idx];
        float x0 = (float)(e->cx * (double)P - cam[0]), z0 = (float)(e->cz * (double)P - cam[2]);
        float y0 = -(float)cam[1];
        vec3 mn = v3(x0 + e->mn[0], y0 + e->mn[1], z0 + e->mn[2]);
        vec3 mx = v3(x0 + e->mx[0], y0 + e->mx[1], z0 + e->mx[2]);
        if (!frustum_test_aabb(&fr, mn, mx)) continue;

        e->drawn = cs->frame + 1;
        ChunkGPU *g = &gpu[nchunk];
        g->origin[0] = x0; g->origin[1] = y0; g->origin[2] = z0; g->origin[3] = 0;
        g->info[0] = e->cx; g->info[1] = e->cz;
        int lp = e->lightPage;
        if (e->layer >= 1) {   /* interiors and litter are lit by their chunk's lamps, shopfronts and arcology lights */
            int base = table_find(cs, e->cx, e->cz, 0);
            lp = base >= 0 && cs->entries[base].state == CS_RESIDENT ? cs->entries[base].lightPage : -1;
        }
        g->info[2] = lp >= 0 ? lp * LIGHT_PAGE_RECS : -1;
        g->info[3] = 0;
        for (int p = 0; p < e->npages; ++p) {
            uint32_t first = p * PAGE_VERTS;
            VkDrawIndirectCommand *c = &cmds[ndraw++];
            c->vertexCount = e->vcount - first < PAGE_VERTS ? e->vcount - first : PAGE_VERTS;
            c->instanceCount = 1;
            c->firstVertex = (uint32_t)e->pages[p] * PAGE_VERTS;
            c->firstInstance = nchunk;
        }
        nchunk++;
    }
    cs->drawCount[slot] = ndraw;
    if (cs->plantFirst[slot] > ndraw) cs->plantFirst[slot] = ndraw;   /* no plants this frame */
    *nchunkOut = nchunk;
    return ndraw;
}

/* 6. Shelter map draws: resident chunks overlapping its square, whatever the
 *    view, after the view's ndraw draws and nchunk chunk records. */
static uint32_t build_shelter_draws(ChunkSystem *cs, int slot, const double cam[3], uint32_t ndraw, uint32_t nchunk) {
    const float P = CHUNK_SIZE;
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)cs->indirect[slot].mapped;
    ChunkGPU *gpu = (ChunkGPU *)cs->chunkData[slot].mapped;
    const float SR = SHELTER_RADIUS + 1.0f;
    uint32_t sFirst = ndraw;
    for (int i = 0; i < MAX_ENTRIES && nchunk < MAX_DRAW_CHUNKS; ++i) {
        Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || e->layer || ndraw + (uint32_t)e->npages > INDIRECT_CAP) continue;   /* lobbies are under roofs; litter shelters nothing */
        float x0 = (float)(e->cx * (double)P - cam[0]), z0 = (float)(e->cz * (double)P - cam[2]);
        if (x0 + e->mx[0] < -SR || x0 + e->mn[0] > SR || z0 + e->mx[2] < -SR || z0 + e->mn[2] > SR) continue;
        ChunkGPU *g = &gpu[nchunk];
        g->origin[0] = x0; g->origin[1] = -(float)cam[1]; g->origin[2] = z0; g->origin[3] = 0;
        g->info[0] = e->cx; g->info[1] = e->cz; g->info[2] = -1; g->info[3] = 0;
        for (int p = 0; p < e->npages; ++p) {
            uint32_t first = p * PAGE_VERTS;
            VkDrawIndirectCommand *c = &cmds[ndraw++];
            c->vertexCount = e->vcount - first < PAGE_VERTS ? e->vcount - first : PAGE_VERTS;
            c->instanceCount = 1;
            c->firstVertex = (uint32_t)e->pages[p] * PAGE_VERTS;
            c->firstInstance = nchunk;
        }
        nchunk++;
    }
    cs->shelterFirst[slot] = sFirst;
    cs->shelterCount[slot] = ndraw - sFirst;
    return ndraw;
}

/* 7. Depth prepass draws (ray-traced shadows): the view's draws of chunks
 *    close enough to hold a surface that receives shadows, as the same
 *    draws (the same chunk records), so both passes agree on every pixel. */
static uint32_t build_prepass_draws(ChunkSystem *cs, int slot, uint32_t ndraw) {
    const float P = CHUNK_SIZE;
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)cs->indirect[slot].mapped;
    const ChunkGPU *gpu = (const ChunkGPU *)cs->chunkData[slot].mapped;
    uint32_t pFirst = ndraw;
    for (uint32_t i = 0; cs->rt && i < cs->plantFirst[slot] && ndraw < INDIRECT_CAP; ++i) {
        const ChunkGPU *g = &gpu[cmds[i].firstInstance];
        float nx = clampf(0.0f, g->origin[0], g->origin[0] + P), nz = clampf(0.0f, g->origin[2], g->origin[2] + P);
        if (sqrtf(nx * nx + nz * nz) > SHADOW_FAR + 80.0f) continue;   /* 80: a building's overhang of its chunk, and some */
        cmds[ndraw++] = cmds[i];
    }
    cs->preFirst[slot] = pFirst;
    cs->preCount[slot] = ndraw - pFirst;

    return ndraw;
}

static void update_stats(ChunkSystem *cs, int slot, int queued, uint32_t viewChunks, int uploads) {
    uint32_t residentCount = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) if (cs->entries[i].state == CS_RESIDENT) residentCount++;
    cs->stats.resident = (int)residentCount;
    cs->stats.queued = queued;
    cs->stats.ready = cs->readyCount;
    cs->stats.drawnChunks = (int)viewChunks;
    cs->stats.drawCalls = (int)cs->drawCount[slot];
    cs->stats.pagesTotal = POOL_PAGES;
    cs->stats.pagesUsed = POOL_PAGES - cs->freePageCount - cs->pendingFreeCount;
    cs->stats.uploadsThisFrame = uploads;
    cs->stats.inViewMissing = cs->inViewMissing;
    cs->stats.reach = cs->reach < REACH_ALL ? cs->reach : 0.0f;
    RtStats rs = rt_stats(cs->rt);
    cs->stats.blas = rs.blas;
    cs->stats.blasBuilds = rs.builds;
    cs->stats.instances = rs.instances;
    cs->stats.blasMB = (int)(rs.arenaUsedMB + 0.5);
}

void chunks_update(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint64_t frame,
                   const double cam[3], const mat4 *viewProj) {
    Frustum fr = frustum_from_matrix(*viewProj);
    cs->frame = frame;
    reclaim_pages(cs, frame);
    if (cs->rt) rt_begin_frame(cs->rt, cmd, slot, frame);
    collect_finished(cs);
    withdraw_jobs(cs);
    want_chunks(cs, &fr, cam);
    int queued = queue_wanted(cs, frame, cam);
    relax_reach(cs, frame);
    int uploads = upload_ready(cs, cmd, slot, frame, cam);
    uint32_t viewChunks;
    uint32_t ndraw = build_draw_list(cs, slot, &fr, cam, &viewChunks);
    ndraw = build_shelter_draws(cs, slot, cam, ndraw, viewChunks);
    build_prepass_draws(cs, slot, ndraw);
    if (cs->rt) update_shadows(cs, cmd, slot, frame, cam);
    update_stats(cs, slot, queued, viewChunks, uploads);
}

static const float *g_airScore;
static int cmp_air(const void *a, const void *b) {
    float sa = g_airScore[*(const int *)a], sb = g_airScore[*(const int *)b];
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

void chunks_sky_lights(ChunkSystem *cs, const double cam[3], SkyGrid *dst) {
    const double P = CHUNK_SIZE;
    SkyGrid *out = &cs->sky;
    const float half = SKY_GRID * SKY_CELL * 0.5f;
    out->origin[0] = out->origin[1] = -half;
    out->cell = SKY_CELL;
    out->count = 0;
    static uint32_t counts[SKY_GRID * SKY_GRID];
    memset(counts, 0, sizeof(counts));

    /* Gather, camera-relative, those whose reach touches the grid. */
    for (int i = 0; i < MAX_ENTRIES && out->count < SKY_MAX_LIGHTS; ++i) {
        const Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || !e->nsky) continue;
        double ox = e->cx * P - cam[0], oz = e->cz * P - cam[2];
        if (fabs(ox + 0.5 * P) > half + P + SKY_RANGE_MAX || fabs(oz + 0.5 * P) > half + P + SKY_RANGE_MAX) continue;
        for (uint32_t k = 0; k < e->nsky && out->count < SKY_MAX_LIGHTS; ++k) {
            SkyLight L = e->sky[k];
            L.pos[0] = (float)(ox + L.pos[0]);
            L.pos[1] = (float)(L.pos[1] - cam[1]);
            L.pos[2] = (float)(oz + L.pos[2]);
            float r = sky_light_range(L.power);
            if (fabsf(L.pos[0]) > half + r || fabsf(L.pos[2]) > half + r) continue;
            out->lights[out->count++] = L;
        }
    }

    /* Bin each light into every cell its reach overlaps: count, prefix-sum, fill. */
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            uint32_t first = 0;
            for (int c = 0; c < SKY_GRID * SKY_GRID; ++c) {
                uint32_t n = counts[c] > 255 ? 255 : counts[c];
                if (first + n > SKY_MAX_INDEX) n = first < SKY_MAX_INDEX ? SKY_MAX_INDEX - first : 0;
                out->cells[c] = first << 8;
                counts[c] = n;
                first += n;
            }
        }
        for (uint32_t li = 0; li < out->count; ++li) {
            const SkyLight *L = &out->lights[li];
            float r = sky_light_range(L->power);
            int x0 = (int)floorf((L->pos[0] - r + half) / SKY_CELL), x1 = (int)floorf((L->pos[0] + r + half) / SKY_CELL);
            int z0 = (int)floorf((L->pos[2] - r + half) / SKY_CELL), z1 = (int)floorf((L->pos[2] + r + half) / SKY_CELL);
            if (x0 < 0) x0 = 0;
            if (z0 < 0) z0 = 0;
            if (x1 > SKY_GRID - 1) x1 = SKY_GRID - 1;
            if (z1 > SKY_GRID - 1) z1 = SKY_GRID - 1;
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x) {
                    int c = z * SKY_GRID + x;
                    if (pass == 0) { counts[c]++; continue; }
                    uint32_t filled = out->cells[c] & 255u;
                    if (filled < counts[c]) {
                        out->index[(out->cells[c] >> 8) + filled] = li;
                        out->cells[c]++;
                    }
                }
        }
    }

    uint32_t nindex = 0;
    for (int c = 0; c < SKY_GRID * SKY_GRID; ++c) nindex += counts[c];
    memcpy(dst, out, offsetof(SkyGrid, index) + nindex * sizeof(uint32_t));
    memcpy(dst->lights, out->lights, out->count * sizeof(SkyLight));

    /* Air lights: those that matter most to the camera's rain mist. */
    int na = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || !e->nair) continue;
        double ox = e->cx * P - cam[0], oz = e->cz * P - cam[2];
        if (fabs(ox + 0.5 * P) > AIR_RANGE + P || fabs(oz + 0.5 * P) > AIR_RANGE + P) continue;
        for (uint32_t k = 0; k < e->nair && na < (int)ARRAY_LEN(cs->airCand); ++k) {
            SkyLight L = e->air[k];
            L.pos[0] = (float)(ox + L.pos[0]);
            L.pos[1] = (float)(L.pos[1] - cam[1]);
            L.pos[2] = (float)(oz + L.pos[2]);
            float d2 = L.pos[0] * L.pos[0] + L.pos[1] * L.pos[1] + L.pos[2] * L.pos[2];
            if (d2 > AIR_RANGE * AIR_RANGE) continue;
            cs->airCand[na] = L;
            cs->airScore[na] = L.power / (d2 + 25.0f);
            cs->airOrder[na] = na;
            na++;
        }
    }
    g_airScore = cs->airScore;
    qsort(cs->airOrder, (size_t)na, sizeof(int), cmp_air);
    uint32_t nkeep = na < AIR_MAX_LIGHTS ? (uint32_t)na : AIR_MAX_LIGHTS;
    for (uint32_t k = 0; k < nkeep; ++k) dst->air[k] = cs->airCand[cs->airOrder[k]];
    dst->airCount = nkeep;
}

uint32_t chunks_flames(ChunkSystem *cs, const double cam[3], FlameSet *dst) {
    const double P = CHUNK_SIZE;
    Flame near[FLAME_MAX];
    float d2s[FLAME_MAX];
    uint32_t n = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || !e->nflame) continue;
        double ox = e->cx * P - cam[0], oz = e->cz * P - cam[2];
        for (uint32_t k = 0; k < e->nflame; ++k) {
            Flame f = e->flame[k];
            f.pos[0] = (float)(ox + f.pos[0]);
            f.pos[1] = (float)(f.pos[1] - cam[1]);
            f.pos[2] = (float)(oz + f.pos[2]);
            float d2 = f.pos[0] * f.pos[0] + f.pos[1] * f.pos[1] + f.pos[2] * f.pos[2];
            if (d2 > FLAME_RANGE * FLAME_RANGE) continue;
            /* the nearest FLAME_MAX: insertion into a sorted list */
            uint32_t at = n < FLAME_MAX ? n : FLAME_MAX;
            while (at > 0 && d2s[at - 1] > d2) at--;
            if (at >= FLAME_MAX) continue;
            uint32_t last = n < FLAME_MAX ? n : FLAME_MAX - 1;
            for (uint32_t j = last; j > at; --j) { near[j] = near[j - 1]; d2s[j] = d2s[j - 1]; }
            near[at] = f; d2s[at] = d2;
            if (n < FLAME_MAX) n++;
        }
    }
    dst->count = n;
    memcpy(dst->flames, near, n * sizeof(Flame));
    return n;
}

/* Inserts p (at squared distance d2) into the max nearest so far, near[] sorted nearest first. */
static void keep_nearest(Plume *near, float *d2s, uint32_t *n, uint32_t max, const Plume *p, float d2) {
    uint32_t at = *n < max ? *n : max;
    while (at > 0 && d2s[at - 1] > d2) at--;
    if (at >= max) return;
    uint32_t last = *n < max ? *n : max - 1;
    for (uint32_t j = last; j > at; --j) { near[j] = near[j - 1]; d2s[j] = d2s[j - 1]; }
    near[at] = *p; d2s[at] = d2;
    if (*n < max) (*n)++;
}

uint32_t chunks_plumes(ChunkSystem *cs, const double cam[3], PlumeSet *dst) {
    const double P = CHUNK_SIZE;
    /* stacks and vents kept apart, so the street's many wisps never crowd out a refinery */
    Plume stack[PLUME_STACKS], vent[PLUME_VENTS];
    float sd2[PLUME_STACKS], vd2[PLUME_VENTS];
    uint32_t ns = 0, nv = 0;
    for (int i = 0; i < MAX_ENTRIES; ++i) {
        const Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || !e->nplume) continue;
        double ox = e->cx * P - cam[0], oz = e->cz * P - cam[2];
        for (uint32_t k = 0; k < e->nplume; ++k) {
            Plume p = e->plume[k];
            p.pos[0] = (float)(ox + p.pos[0]);
            p.pos[1] = (float)(p.pos[1] - cam[1]);
            p.pos[2] = (float)(oz + p.pos[2]);
            float d2 = p.pos[0] * p.pos[0] + p.pos[1] * p.pos[1] + p.pos[2] * p.pos[2];
            if (p.kind == PLUME_VENT) {
                if (d2 <= VENT_RANGE * VENT_RANGE) keep_nearest(vent, vd2, &nv, PLUME_VENTS, &p, d2);
            } else if (d2 <= PLUME_RANGE * PLUME_RANGE) keep_nearest(stack, sd2, &ns, PLUME_STACKS, &p, d2);
        }
    }
    /* farthest first, merging the two: the pass blends them back to front */
    uint32_t n = 0, a = ns, b = nv;
    while (a || b) {
        if (b == 0 || (a && sd2[a - 1] >= vd2[b - 1])) dst->plumes[n++] = stack[--a];
        else dst->plumes[n++] = vent[--b];
    }
    dst->count = n;
    return n;
}

static void draw_range(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint32_t start, uint32_t n) {
    if (!n) return;
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &cs->pool.buf, &off);
    if (cs->vk->mdi) {
        uint32_t maxCount = cs->vk->props.limits.maxDrawIndirectCount;
        for (uint32_t first = 0; first < n; first += maxCount) {
            uint32_t cnt = n - first < maxCount ? n - first : maxCount;
            vkCmdDrawIndirect(cmd, cs->indirect[slot].buf, (start + first) * sizeof(VkDrawIndirectCommand), cnt,
                              sizeof(VkDrawIndirectCommand));
        }
    } else {
        const VkDrawIndirectCommand *c = (const VkDrawIndirectCommand *)cs->indirect[slot].mapped + start;
        for (uint32_t i = 0; i < n; ++i)
            vkCmdDraw(cmd, c[i].vertexCount, 1, c[i].firstVertex, c[i].firstInstance);
    }
}

bool chunks_drawn(ChunkSystem *cs, int cx, int cz) {
    int idx = table_find(cs, cx, cz, 0);
    return idx >= 0 && cs->entries[idx].drawn == cs->frame + 1;
}

void chunks_draw(ChunkSystem *cs, VkCommandBuffer cmd, int slot) { draw_range(cs, cmd, slot, 0, cs->plantFirst[slot]); }

void chunks_draw_plants(ChunkSystem *cs, VkCommandBuffer cmd, int slot) {
    draw_range(cs, cmd, slot, cs->plantFirst[slot], cs->drawCount[slot] - cs->plantFirst[slot]);
}

void chunks_draw_prepass(ChunkSystem *cs, VkCommandBuffer cmd, int slot) {
    draw_range(cs, cmd, slot, cs->preFirst[slot], cs->preCount[slot]);
}

void chunks_draw_shelter(ChunkSystem *cs, VkCommandBuffer cmd, int slot) {
    draw_range(cs, cmd, slot, cs->shelterFirst[slot], cs->shelterCount[slot]);
}
