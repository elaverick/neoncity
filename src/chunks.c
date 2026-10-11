#include "chunks.h"
#include "city.h"
#include "thread.h"

#define MAX_ENTRIES    8192
#define HASH_BUCKETS   16384
#define MAX_JOBS       1024
#define MAX_WORKERS    16
#define INDIRECT_CAP   (POOL_PAGES * 2)   /* the view's draws, then the shelter map's */

enum { CS_QUEUED = 1, CS_READY, CS_RESIDENT };

typedef struct Entry {
    int      cx, cz;
    int      state;
    int      next;          /* hash chain */
    uint64_t lastSeen;
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
} Entry;

typedef struct Job { int cx, cz; } Job;
typedef struct Result { int cx, cz; Mesh mesh; } Result;
typedef struct PendingFree { uint16_t page; uint64_t frame; } PendingFree;
typedef struct Want { int cx, cz; float key; int inView; } Want;

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
    uint32_t drawCount[MAX_FRAMES];
    uint32_t shelterFirst[MAX_FRAMES], shelterCount[MAX_FRAMES];

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
    int      wantCount, wantCap;
    int      ready[MAX_ENTRIES];
    int      readyCount;

    ChunkStats stats;
    SkyGrid    sky;     /* built here, then copied out: the destination is write-combined GPU memory */
    SkyLight   airCand[16384];
    float      airScore[16384];
    int        airOrder[16384];
};

/* ---------------------------------------------------------------- table --- */

static uint32_t bucket_of(int cx, int cz) {
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cz * 19349663u;
    return (h ^ (h >> 15)) & (HASH_BUCKETS - 1);
}

static int table_find(ChunkSystem *cs, int cx, int cz) {
    for (int i = cs->buckets[bucket_of(cx, cz)]; i >= 0; i = cs->entries[i].next)
        if (cs->entries[i].cx == cx && cs->entries[i].cz == cz) return i;
    return -1;
}

static int table_insert(ChunkSystem *cs, int cx, int cz) {
    if (!cs->freeEntryCount) return -1;
    int i = cs->freeEntries[--cs->freeEntryCount];
    Entry *e = &cs->entries[i];
    memset(e, 0, sizeof(*e));
    e->cx = cx; e->cz = cz;
    e->lightPage = -1;
    uint32_t b = bucket_of(cx, cz);
    e->next = cs->buckets[b];
    cs->buckets[b] = i;
    return i;
}

static void table_remove(ChunkSystem *cs, int idx) {
    Entry *e = &cs->entries[idx];
    uint32_t b = bucket_of(e->cx, e->cz);
    int *link = &cs->buckets[b];
    while (*link >= 0 && *link != idx) link = &cs->entries[*link].next;
    if (*link == idx) *link = e->next;
    mesh_free(&e->mesh);
    free(e->sky);
    free(e->air);
    e->sky = e->air = NULL;
    e->nsky = e->nair = 0;
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
        r.cx = job.cx; r.cz = job.cz;
        memset(&r.mesh, 0, sizeof(r.mesh));
        city_build_mesh(job.cx, job.cz, &r.mesh);

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
    for (int i = 0; i < POOL_PAGES; ++i) cs->freePages[i] = (uint16_t)(POOL_PAGES - 1 - i);
    cs->freePageCount = POOL_PAGES;
    cs->pendingFree = (PendingFree *)malloc(sizeof(PendingFree) * POOL_PAGES);
    for (int i = 0; i < LIGHT_PAGES; ++i) cs->freeLight[i] = (uint16_t)(LIGHT_PAGES - 1 - i);
    cs->freeLightCount = LIGHT_PAGES;
    cs->pendingLight = (PendingFree *)malloc(sizeof(PendingFree) * LIGHT_PAGES);

    VkDeviceSize poolBytes = (VkDeviceSize)POOL_PAGES * PAGE_VERTS * sizeof(Vertex);
    cs->pool = vk_buffer(vk, poolBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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
    for (int i = 0; i < MAX_ENTRIES; ++i) { mesh_free(&cs->entries[i].mesh); free(cs->entries[i].sky); free(cs->entries[i].air); }
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
    free(cs);
}

VkBuffer chunks_gpu_buffer(ChunkSystem *cs, int slot) { return cs->chunkData[slot].buf; }
VkDeviceSize chunks_gpu_buffer_size(void) { return sizeof(ChunkGPU) * MAX_DRAW_CHUNKS; }
VkBuffer chunks_light_buffer(ChunkSystem *cs) { return cs->lightPool.buf; }
VkDeviceSize chunks_light_buffer_size(void) { return (VkDeviceSize)LIGHT_PAGES * LIGHT_PAGE_RECS * 16; }
ChunkStats chunks_stats(ChunkSystem *cs) { return cs->stats; }

/* ---------------------------------------------------------------- pages --- */

static void evict(ChunkSystem *cs, int idx, uint64_t frame) {
    Entry *e = &cs->entries[idx];
    for (int i = 0; i < e->npages; ++i) {
        cs->pendingFree[cs->pendingFreeCount].page = e->pages[i];
        cs->pendingFree[cs->pendingFreeCount].frame = frame;
        cs->pendingFreeCount++;
    }
    e->npages = 0;
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
    for (int i = 0; i < n && !POOLS_HIGH(); ++i) evict(cs, cand[i].idx, frame);
#undef POOLS_LOW
#undef POOLS_HIGH
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

/* --------------------------------------------------------------- update --- */

static int cmp_want(const void *a, const void *b) {
    float ka = ((const Want *)a)->key, kb = ((const Want *)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void push_want(ChunkSystem *cs, int cx, int cz, float key, int inView) {
    if (cs->wantCount == cs->wantCap) {
        cs->wantCap = cs->wantCap ? cs->wantCap * 2 : 4096;
        cs->want = (Want *)realloc(cs->want, sizeof(Want) * cs->wantCap);
    }
    Want *w = &cs->want[cs->wantCount++];
    w->cx = cx; w->cz = cz; w->key = key; w->inView = inView;
}

static const double *g_sortCam;
static ChunkSystem *g_sortCs;
static int cmp_ready(const void *a, const void *b) {
    float da = entry_dist2(&g_sortCs->entries[*(const int *)a], g_sortCam);
    float db = entry_dist2(&g_sortCs->entries[*(const int *)b], g_sortCam);
    return da < db ? -1 : da > db ? 1 : 0;
}

void chunks_update(ChunkSystem *cs, VkCommandBuffer cmd, int slot, uint64_t frame,
                   const double cam[3], const mat4 *viewProj) {
    const float P = CHUNK_SIZE;
    Frustum fr = frustum_from_matrix(*viewProj);
    reclaim_pages(cs, frame);

    /* 1. Collect finished meshes from the workers. */
    mutex_lock(&cs->doneLock);
    for (int i = 0; i < cs->doneCount; ++i) {
        Result *r = &cs->done[i];
        int idx = table_find(cs, r->cx, r->cz);
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

    /* 2. Withdraw jobs no worker has started yet; they are re-prioritised below. */
    mutex_lock(&cs->jobLock);
    for (int i = cs->jobHead; i < cs->jobCount; ++i) {
        int idx = table_find(cs, cs->jobs[i].cx, cs->jobs[i].cz);
        if (idx >= 0 && cs->entries[idx].state == CS_QUEUED) table_remove(cs, idx);
    }
    cs->jobHead = cs->jobCount = 0;
    mutex_unlock(&cs->jobLock);

    /* 3. Frustum fill: every chunk inside the view distance whose conservative
     *    bounds touch the frustum, plus a ring around the player. */
    cs->wantCount = 0;
    int ccx = (int)floor(cam[0] / P), ccz = (int)floor(cam[2] / P);
    int rad = (int)ceilf(VIEW_DISTANCE / P) + 1;
    for (int dz = -rad; dz <= rad; ++dz) {
        for (int dx = -rad; dx <= rad; ++dx) {
            int cx = ccx + dx, cz = ccz + dz;
            float x0 = (float)(cx * (double)P - cam[0]), z0 = (float)(cz * (double)P - cam[2]);
            float nx = clampf(0.0f, x0, x0 + P), nz = clampf(0.0f, z0, z0 + P);
            float d = sqrtf(nx * nx + nz * nz);
            if (d > VIEW_DISTANCE) continue;
            int idx = table_find(cs, cx, cz);
            float y1;
            vec3 mn, mx;
            if (idx >= 0 && cs->entries[idx].state == CS_RESIDENT) {
                Entry *e = &cs->entries[idx];
                mn = v3(x0 + e->mn[0], e->mn[1] - (float)cam[1], z0 + e->mn[2]);
                mx = v3(x0 + e->mx[0], e->mx[1] - (float)cam[1], z0 + e->mx[2]);
            } else {
                /* Buildings centred in the chunk may overhang it. */
                y1 = city_height_cap(cx, cz);
                mn = v3(x0 - CITY_OVERHANG, -(float)cam[1] - 1.0f, z0 - CITY_OVERHANG);
                mx = v3(x0 + P + CITY_OVERHANG, y1 - (float)cam[1], z0 + P + CITY_OVERHANG);
            }
            int inView = frustum_test_aabb(&fr, mn, mx);
            if (inView) push_want(cs, cx, cz, d, 1);
            else if (d < NEAR_RING_DIST) push_want(cs, cx, cz, d * 3.0f + 200.0f, 0);
        }
    }
    qsort(cs->want, cs->wantCount, sizeof(Want), cmp_want);

    Job newJobs[MAX_JOBS];
    int nj = 0, queued = 0;
    for (int i = 0; i < cs->wantCount; ++i) {
        Want *w = &cs->want[i];
        int idx = table_find(cs, w->cx, w->cz);
        if (idx < 0) {
            if (nj >= MAX_JOBS || cs->freeEntryCount < 64) continue;
            idx = table_insert(cs, w->cx, w->cz);
            if (idx < 0) continue;
            cs->entries[idx].state = CS_QUEUED;
            newJobs[nj].cx = w->cx; newJobs[nj].cz = w->cz; nj++;
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

    /* 4. Upload ready meshes, nearest first, within the staging budget. */
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
        memcpy(e->mn, e->mesh.mn, sizeof(e->mn));
        memcpy(e->mx, e->mesh.mx, sizeof(e->mx));
        e->sky = e->mesh.sky;
        e->nsky = e->mesh.nsky;
        e->mesh.sky = NULL;
        e->air = e->mesh.air;
        e->nair = e->mesh.nair;
        e->mesh.air = NULL;
        mesh_free(&e->mesh);
        e->state = CS_RESIDENT;
        uploads++;
    }
    memmove(cs->ready, cs->ready + r, sizeof(int) * (cs->readyCount - r));
    cs->readyCount -= r;
    if (nreg) vkCmdCopyBuffer(cmd, stg->buf, cs->pool.buf, (uint32_t)nreg, regions);
    if (nlreg) vkCmdCopyBuffer(cmd, stg->buf, cs->lightPool.buf, (uint32_t)nlreg, lightRegions);
    if (nreg || nlreg)
        vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                          VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

    /* 5. Build the indirect draw list from resident, visible chunks (front to back). */
    VkDrawIndirectCommand *cmds = (VkDrawIndirectCommand *)cs->indirect[slot].mapped;
    ChunkGPU *gpu = (ChunkGPU *)cs->chunkData[slot].mapped;
    uint32_t ndraw = 0, nchunk = 0, residentCount = 0;
    for (int i = 0; i < cs->wantCount && nchunk < MAX_DRAW_CHUNKS; ++i) {
        Want *w = &cs->want[i];
        if (!w->inView) continue;
        int idx = table_find(cs, w->cx, w->cz);
        if (idx < 0 || cs->entries[idx].state != CS_RESIDENT) continue;
        Entry *e = &cs->entries[idx];
        float x0 = (float)(e->cx * (double)P - cam[0]), z0 = (float)(e->cz * (double)P - cam[2]);
        float y0 = -(float)cam[1];
        vec3 mn = v3(x0 + e->mn[0], y0 + e->mn[1], z0 + e->mn[2]);
        vec3 mx = v3(x0 + e->mx[0], y0 + e->mx[1], z0 + e->mx[2]);
        if (!frustum_test_aabb(&fr, mn, mx)) continue;

        ChunkGPU *g = &gpu[nchunk];
        g->origin[0] = x0; g->origin[1] = y0; g->origin[2] = z0; g->origin[3] = 0;
        g->info[0] = e->cx; g->info[1] = e->cz;
        g->info[2] = e->lightPage >= 0 ? e->lightPage * LIGHT_PAGE_RECS : -1;
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

    /* 6. Shelter map draws: resident chunks overlapping its square, whatever the view. */
    const float SR = SHELTER_RADIUS + 1.0f;
    uint32_t sFirst = ndraw, viewChunks = nchunk;
    for (int i = 0; i < MAX_ENTRIES && nchunk < MAX_DRAW_CHUNKS; ++i) {
        Entry *e = &cs->entries[i];
        if (e->state != CS_RESIDENT || ndraw + (uint32_t)e->npages > INDIRECT_CAP) continue;
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

    for (int i = 0; i < MAX_ENTRIES; ++i) if (cs->entries[i].state == CS_RESIDENT) residentCount++;
    cs->stats.resident = (int)residentCount;
    cs->stats.queued = queued;
    cs->stats.ready = cs->readyCount;
    cs->stats.drawnChunks = (int)viewChunks;
    cs->stats.drawCalls = (int)cs->drawCount[slot];
    cs->stats.pagesTotal = POOL_PAGES;
    cs->stats.pagesUsed = POOL_PAGES - cs->freePageCount - cs->pendingFreeCount;
    cs->stats.uploadsThisFrame = uploads;
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

void chunks_draw(ChunkSystem *cs, VkCommandBuffer cmd, int slot) { draw_range(cs, cmd, slot, 0, cs->drawCount[slot]); }

void chunks_draw_shelter(ChunkSystem *cs, VkCommandBuffer cmd, int slot) {
    draw_range(cs, cmd, slot, cs->shelterFirst[slot], cs->shelterCount[slot]);
}
