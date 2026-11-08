#include "renderer_internal.h"
#include "terrain.h"
#include "thread.h"
#include "hash.h"

/* The caves' GPU side (terrain.h, "Caves' meshes"): the systems near the
 * camera, meshed a brick at a time on worker threads, cached and drawn in
 * three rings of detail. It never generates terrain itself.
 *
 * Systems: a worker looks up the systems whose air can come within
 * SYS_REACH of the camera (terrain_caves_near_dir), when the camera has moved
 * REFRESH_M since it last did, and lists each one's live bricks at every
 * level (terrain_cave_bricks) once. The render thread takes the set whole.
 *
 * Rings: about the camera's place in each system's chart, level l draws the
 * bricks within terrain_cave_lod_reach(l) on each axis, a box snapped
 * outwards to the next level's bricks (so a coarser brick lies wholly inside
 * a finer level's box or wholly outside it). A level is drawn only while every
 * brick in its box is meshed (the coarsest whatever is), so a ring never has
 * a hole; each coarser level drawn leaves out its bricks inside the next finer
 * one's box shrunk by one of its own cells, and its fragments inside that
 * shrunk box (cave.frag): the two overlap by that cell, so no crack opens
 * where they meet. The bricks in a band one brick wider are asked for too,
 * nearest (for their level's reach) first, so a ring's next bricks are ready
 * before it moves onto them.
 *
 * Bricks: meshed by the workers (terrain_cave_brick_mesh) into a cache keyed
 * by the cave, level and brick, whose vertices and indices live together in
 * one device-local pool, a buddy allocator's blocks. A brick's block is
 * copied in through this frame slot's staging buffer behind a barrier on
 * every earlier frame's reads; when the pool or the table is full, the
 * bricks not wanted for longest go first.
 *
 * Positions: each system is drawn about an anchor, the camera's place in its
 * chart snapped to ANCHOR_M, mapped into the active frame from doubles with
 * the chart's local linear map there (J). A vertex is its brick's offset from
 * the anchor (a whole number of cells) plus its place in the brick (a whole
 * number of 1/TERRAIN_CAVE_QUANT cells): exact in float, the same point for
 * two bricks that share it, so the seams stay shut on the GPU too. Nothing
 * meshed depends on the frame: a change of frame only moves the anchors.
 *
 * Mouths: each system's mouths' holes (terrain_cave_hole) are found with its
 * bricks, and the live bricks of each level within HOLE_BRICK_PAD of each.
 * Each frame the holes go to the slot's CaveHolesGPU (shaders/terrain/
 * cave_hole.glsl) with the terrain's drawn levels: the terrain is cut where
 * its ground lies in the air (terrain.frag), the grass kept off (grass/
 * prepare.comp), and cave.frag cuts the meshes off at the drawn ground
 * (terrain/cave_clip.glsl). A hole is cut only while its box lies within the
 * coarsest ring and every brick drawn round it is meshed, so the ground never
 * opens on a missing piece of cave; its margin is that of the coarsest level
 * drawn over it (HOLE_MARGIN, the meshes' rounding there). The terrain draws
 * only its tiles near a cut hole's box with the cut (terrain_gpu.c), since a
 * shader that can discard loses the early depth test everywhere it draws.
 * cave_gpu_update runs before the terrain's, which reads the boxes;
 * cave_gpu_levels after it. */

#define SYS_MAX      4
#define LODS         TERRAIN_CAVE_LODS
#define REFRESH_M    100.0
#define SYS_REACH    (TERRAIN_CAVE_DRAW_M + 2.0 * TERRAIN_CAVE_CELLS * TERRAIN_CAVE_VOXEL0 * 4.0 + REFRESH_M)
#define ANCHOR_M     32.0
#define LIVE_MAX     65536            /* a system's live bricks at a level, at most */
#define ENTRIES      16384            /* the cache's table (a power of two) */
#define ENTRIES_KEEP (ENTRIES * 3 / 4)
#define QUEUE_MAX    1024
#define RESULTS      64
#define UPLOAD_BYTES ((VkDeviceSize)4 << 20)   /* a frame's uploads, at most */
#define MAX_DRAWS    16384
#define POOL_LOG     26               /* the pool: 64 MB */
#define BLOCK_LOG    10               /* its smallest block: 1 KB */
#define ORDERS       (POOL_LOG - BLOCK_LOG + 1)
#define NBLOCKS      (1 << (POOL_LOG - BLOCK_LOG))
#define WORKERS_MAX  3

_Static_assert(sizeof(TerrainCaveVertex) == 16, "cave.vert reads a vertex as 4 words");
_Static_assert(ANCHOR_M >= TERRAIN_CAVE_CELLS * TERRAIN_CAVE_VOXEL0 * 4.0 && (int)ANCHOR_M % (int)(TERRAIN_CAVE_CELLS * TERRAIN_CAVE_VOXEL0 * 4.0) == 0,
               "the anchor must sit on every level's bricks");

/* One system's place in this frame's draws (cave.vert: CaveSys). */
typedef struct CaveSysGPU {
    float anchor[4];   /* camera-relative world x, z of the anchor; its height less the camera's; its height */
    float J[4];        /* world x = J0 cx + J1 cz, world z = J2 cx + J3 cz, about the anchor */
    float chart[4];    /* the anchor in the chart (x, h, z) */
    float box[2][2][4];/* the discard boxes: level 0's and level 1's shrunk, min and max, from the anchor */
} CaveSysGPU;
/* One brick's draw (cave.vert: CaveDraw). */
typedef struct CaveDrawGPU {
    float   off[4];    /* the brick's corner from the anchor (chart), and a vertex's unit (m) */
    int32_t info[4];   /* its system, its discard box (-1 none), its level, 0 */
} CaveDrawGPU;
typedef struct CaveFrameGPU { CaveSysGPU sys[SYS_MAX]; CaveDrawGPU draw[MAX_DRAWS]; } CaveFrameGPU;

/* The mouths' holes and the terrain's drawn levels (shaders/terrain/cave_hole.glsl: CaveHoles). */
#define HOLE_PIECES    TERRAIN_CAVE_HOLE_PIECES
_Static_assert(TERRAIN_CAVE_HOLE_PIECES >= TERRAIN_CAVE_HOLE_SEGS + TERRAIN_CAVE_ROOMS, "a hole's pieces: its passages and every chamber");
#define HOLE_BRICK_PAD 4.0   /* the bricks round a hole that must be meshed before it is cut (m) */
static const float HOLE_MARGIN[LODS] = { 0.25f, 0.5f, 1.2f };   /* the field's units: past the meshes' vertices' worst (terrain_test --cave-mesh) */
typedef struct CaveLevelGPU { float a[4], b[4]; int32_t c[4], d[4]; } CaveLevelGPU;   /* as GrassLevelGPU */
typedef struct CaveHoleGPU {
    float   anchor[4];   /* camera-relative world x, z of the system's anchor; the mouth's height; the margin */
    float   Jinv[4];     /* chart x = Jinv0 dx + Jinv1 dz, chart z = Jinv2 dx + Jinv3 dz, from the anchor */
    float   off[4];      /* the anchor's place in the chart less the mouth's */
    float   lo[4], hi[4];/* the box, from the mouth */
    float   noise[4];    /* the mouth in the chart */
    int32_t info[4];     /* first piece, passages, chambers, 0 */
    uint32_t mask[4];    /* TerrainCaveHole.mask */
} CaveHoleGPU;
typedef struct CavePieceGPU { float a[4], b[4]; } CavePieceGPU;
typedef struct CaveHolesGPU {
    int32_t      info[4];   /* holes, the finest terrain level drawn, the noise's seeds */
    CaveLevelGPU lv[TERRAIN_CLIP_LEVELS];
    CaveHoleGPU  hole[TERRAIN_CAVE_HOLES];
    CavePieceGPU piece[TERRAIN_CAVE_HOLES * HOLE_PIECES];
} CaveHolesGPU;
_Static_assert(SYS_MAX * TERRAIN_CAVE_MOUTHS <= TERRAIN_CAVE_HOLES, "every near system's mouths must have room for a hole");
_Static_assert(sizeof(CaveHolesGPU) <= 16384, "CaveHoles must fit the least maxUniformBufferRange Vulkan guarantees");
_Static_assert(sizeof(CaveLevelGPU) == 64 && sizeof(CaveHoleGPU) == 128 && sizeof(CavePieceGPU) == 32, "CaveHoles' std430 layout");

typedef struct CaveSys {   /* a system near the camera, as a worker found it; never changed */
    TerrainCave cave;
    int         nlive[LODS];
    int32_t   (*live[LODS])[3];
    int         hasHole[TERRAIN_CAVE_MOUTHS];
    TerrainCaveHole hole[TERRAIN_CAVE_MOUTHS];
    int         nround[TERRAIN_CAVE_MOUTHS][LODS];     /* the live bricks round each hole */
    int32_t   (*round[TERRAIN_CAVE_MOUTHS][LODS])[3];
} CaveSys;

typedef struct BrickKey { uint32_t cave; int16_t i, j, k; uint8_t lod, pad; } BrickKey;
static int key_eq(BrickKey a, BrickKey b) { return a.cave == b.cave && a.i == b.i && a.j == b.j && a.k == b.k && a.lod == b.lod; }
static uint32_t key_hash(BrickKey k) {
    uint32_t h = hash_u32(k.cave ^ 0x9E3779B9u * (uint32_t)(k.lod + 1));
    h = hash_u32(h ^ (uint32_t)(uint16_t)k.i);
    h = hash_u32(h ^ (uint32_t)(uint16_t)k.j << 3);
    return hash_u32(h ^ (uint32_t)(uint16_t)k.k << 7);
}

typedef struct Entry {
    BrickKey key;
    uint8_t  used_;     /* the slot holds a brick */
    int32_t  block;     /* its pool block, -1 for an empty mesh */
    uint32_t nidx, vbytes;
    uint64_t used;      /* the frame it was last wanted in */
} Entry;

typedef struct Job { BrickKey key; const CaveSys *sys; int32_t b[3]; float pri; } Job;
typedef struct Result { BrickKey key; int nv, ni; void *data; double ms; } Result;

/* --- The pool: a buddy allocator over NBLOCKS blocks ------------------------------ */
typedef struct Buddy {
    int32_t head[ORDERS];
    int32_t next[NBLOCKS], prev[NBLOCKS];
    int8_t  order[NBLOCKS];
    uint8_t isfree[NBLOCKS];
    int64_t used;       /* bytes */
} Buddy;

static void bud_push(Buddy *b, int32_t blk, int o) {
    b->order[blk] = (int8_t)o; b->isfree[blk] = 1;
    b->prev[blk] = -1; b->next[blk] = b->head[o];
    if (b->head[o] >= 0) b->prev[b->head[o]] = blk;
    b->head[o] = blk;
}
static void bud_unlink(Buddy *b, int32_t blk) {
    int o = b->order[blk];
    if (b->prev[blk] >= 0) b->next[b->prev[blk]] = b->next[blk]; else b->head[o] = b->next[blk];
    if (b->next[blk] >= 0) b->prev[b->next[blk]] = b->prev[blk];
    b->isfree[blk] = 0;
}
static void bud_init(Buddy *b) {
    for (int o = 0; o < ORDERS; ++o) b->head[o] = -1;
    memset(b->isfree, 0, sizeof b->isfree);
    b->used = 0;
    bud_push(b, 0, ORDERS - 1);
}
static int bud_order(VkDeviceSize bytes) {
    int o = 0;
    while (((VkDeviceSize)1 << (BLOCK_LOG + o)) < bytes) o++;
    return o;
}
static int32_t bud_alloc(Buddy *b, VkDeviceSize bytes) {
    int o = bud_order(bytes), k = o;
    if (o >= ORDERS) return -1;
    while (k < ORDERS && b->head[k] < 0) k++;
    if (k >= ORDERS) return -1;
    int32_t blk = b->head[k];
    bud_unlink(b, blk);
    while (k > o) { k--; bud_push(b, blk + (1 << k), k); }
    b->order[blk] = (int8_t)o;
    b->used += (int64_t)1 << (BLOCK_LOG + o);
    return blk;
}
static void bud_free(Buddy *b, int32_t blk) {
    int o = b->order[blk];
    b->used -= (int64_t)1 << (BLOCK_LOG + o);
    while (o < ORDERS - 1) {
        int32_t bud = blk ^ (1 << o);
        if (!b->isfree[bud] || b->order[bud] != o) break;
        bud_unlink(b, bud);
        blk = blk < bud ? blk : bud;
        o++;
    }
    bud_push(b, blk, o);
}

struct CaveGpu {
    /* the workers' side, guarded by lock */
    Thread  *worker[WORKERS_MAX];
    int      nworkers;
    Mutex    lock;
    CondVar  wake;
    int      quit;
    int      refreshAsked, refreshRunning, pendingReady;
    TerrainDir refreshAt;
    CaveSys *pending[SYS_MAX];
    int      npending;
    Job      queue[QUEUE_MAX];
    int      nqueue, qhead;
    BrickKey busy[WORKERS_MAX];
    int      busyOn[WORKERS_MAX];
    Result   result[RESULTS];
    int      nresult;
    double   meshMs, slowestMs, refreshMs;
    int      meshed;
    /* the render thread's */
    CaveSys *sys[SYS_MAX];
    int      nsys;
    TerrainDir lastRefresh;
    int      haveRefresh;
    Entry   *table;
    int      entries;
    Buddy   *bud;
    Buffer   pool;                  /* device local: vertices and indices */
    Buffer   staging[MAX_FRAMES];
    Buffer   frameBuf[MAX_FRAMES];  /* host visible: CaveFrameGPU */
    Buffer   indirect[MAX_FRAMES];  /* host visible: VkDrawIndexedIndirectCommand[2 * MAX_DRAWS]: the view's, then from MAX_DRAWS the shelter map's */
    Buffer   holeBuf[MAX_FRAMES];   /* host visible: CaveHolesGPU */
    int      nholes, holesCut, holePieces;   /* this frame's holes, those cut, the most pieces one has */
    float    holeBox[TERRAIN_CAVE_HOLES][2][3];  /* the cut ones' boxes, camera-relative world (min, max) */
    double   holeMs;                /* finding a system's holes, the last time (a worker's) */
    VkDescriptorPool dpool;
    VkDescriptorSet  set[MAX_FRAMES];
    Job     *want;                  /* this frame's wanted bricks, before sorting */
    int      ndraw, nshelter, drawnLod[LODS], drawnTris, missing, uploads, levels[SYS_MAX], evicted;
    int      camIn;                 /* the camera in a cave's air under the ground, and the sky it sees there (terrain_cave_sky) */
    float    camSky;
    int      debugView;
    int      frames, fellBack, missingMost;   /* frames drawn, those in which a finer level near a system had to wait, the most bricks missing */
    double   updateMs, slowestUpdateMs;
};

/* --- The workers ------------------------------------------------------------------ */

static void free_sys(CaveSys *s) {
    if (!s) return;
    for (int l = 0; l < LODS; ++l) {
        free(s->live[l]);
        for (int m = 0; m < TERRAIN_CAVE_MOUTHS; ++m) free(s->round[m][l]);
    }
    free(s);
}

/* System s's mouths' holes, and the live bricks round each. */
static void find_holes(CaveSys *s) {
    for (int m = 0; m < s->cave.nmouth; ++m) {
        s->hasHole[m] = terrain_cave_hole(&s->cave, m, &s->hole[m]);
        if (!s->hasHole[m]) continue;
        const TerrainCaveHole *h = &s->hole[m];
        for (int l = 0; l < LODS; ++l) {
            double B = terrain_cave_brick(l);
            int n = 0;
            s->round[m][l] = malloc(sizeof(int32_t[3]) * (size_t)(s->nlive[l] ? s->nlive[l] : 1));
            if (!s->round[m][l]) { s->hasHole[m] = 0; break; }
            for (int b = 0; b < s->nlive[l]; ++b) {
                const int32_t *q = s->live[l][b];
                int touch = 1;
                for (int a = 0; a < 3; ++a) touch &= (q[a] + 1) * B >= h->lo[a] - HOLE_BRICK_PAD && q[a] * B <= h->hi[a] + HOLE_BRICK_PAD;
                if (touch) memcpy(s->round[m][l][n++], q, sizeof(int32_t[3]));
            }
            s->nround[m][l] = n;
        }
    }
}

/* The systems near direction p: those already known kept as they are. */
static void refresh(CaveGpu *g, TerrainDir p, CaveSys *const *known, int nknown) {
    static _Thread_local TerrainCave near[SYS_MAX];
    double t0 = glfwGetTime();
    int n = terrain_caves_near_dir(p, SYS_REACH, near, SYS_MAX);
    CaveSys *out[SYS_MAX];
    int32_t (*tmp)[3] = malloc(sizeof(int32_t[3]) * LIVE_MAX);
    for (int k = 0; k < n; ++k) {
        out[k] = NULL;
        for (int q = 0; q < nknown && !out[k]; ++q)
            if (known[q]->cave.id == near[k].id) out[k] = known[q];
        if (out[k] || !tmp) continue;
        CaveSys *s = (CaveSys *)calloc(1, sizeof(CaveSys));
        if (!s) continue;
        s->cave = near[k];
        for (int l = 0; l < LODS; ++l) {
            s->nlive[l] = terrain_cave_bricks(&s->cave, l, tmp, LIVE_MAX);
            s->live[l] = malloc(sizeof(int32_t[3]) * (size_t)(s->nlive[l] ? s->nlive[l] : 1));
            if (s->live[l]) memcpy(s->live[l], tmp, sizeof(int32_t[3]) * (size_t)s->nlive[l]);
            else s->nlive[l] = 0;
        }
        double th = glfwGetTime();
        find_holes(s);
        mutex_lock(&g->lock);
        g->holeMs = (glfwGetTime() - th) * 1000.0;
        mutex_unlock(&g->lock);
        out[k] = s;
    }
    free(tmp);
    int m = 0;
    for (int k = 0; k < n; ++k) if (out[k]) out[m++] = out[k];
    mutex_lock(&g->lock);
    memcpy(g->pending, out, sizeof(CaveSys *) * (size_t)m);
    g->npending = m;
    g->pendingReady = 1;
    g->refreshRunning = 0;
    g->refreshMs = (glfwGetTime() - t0) * 1000.0;
    mutex_unlock(&g->lock);
}

typedef struct WorkerArg { CaveGpu *g; int id; } WorkerArg;
static WorkerArg g_args[WORKERS_MAX];

static int cave_worker(void *arg) {
    WorkerArg *wa = (WorkerArg *)arg;
    CaveGpu *g = wa->g;
    TerrainCaveVertex *v = (TerrainCaveVertex *)malloc(sizeof(TerrainCaveVertex) * TERRAIN_CAVE_MESH_VERTS);
    uint16_t *idx = (uint16_t *)malloc(sizeof(uint16_t) * TERRAIN_CAVE_MESH_INDICES);
    TerrainCave *cave = (TerrainCave *)malloc(sizeof(TerrainCave));
    if (!v || !idx || !cave) fatal("out of memory starting the caves' workers");
    mutex_lock(&g->lock);
    for (;;) {
        while (!g->quit && !(g->refreshAsked && !g->refreshRunning) && (g->qhead >= g->nqueue || g->nresult >= RESULTS)) cond_wait(&g->wake, &g->lock);
        if (g->quit) break;
        if (g->refreshAsked && !g->refreshRunning) {
            g->refreshAsked = 0;
            g->refreshRunning = 1;
            TerrainDir p = g->refreshAt;
            CaveSys *known[SYS_MAX];
            int nknown = g->nsys;   /* the render thread's set: it frees none while a refresh runs */
            memcpy(known, g->sys, sizeof(CaveSys *) * (size_t)nknown);
            mutex_unlock(&g->lock);
            refresh(g, p, known, nknown);
            mutex_lock(&g->lock);
            continue;
        }
        Job j = g->queue[g->qhead++];
        *cave = j.sys->cave;   /* its system may be dropped while it is meshed */
        g->busy[wa->id] = j.key;
        g->busyOn[wa->id] = 1;
        mutex_unlock(&g->lock);
        double t0 = glfwGetTime();
        int ni, nv = terrain_cave_brick_mesh(cave, j.key.lod, j.b[0], j.b[1], j.b[2], v, idx, &ni);
        double ms = (glfwGetTime() - t0) * 1000.0;
        void *data = NULL;
        if (ni > 0) {
            data = malloc(sizeof(TerrainCaveVertex) * (size_t)nv + sizeof(uint16_t) * (size_t)ni);
            if (data) {
                memcpy(data, v, sizeof(TerrainCaveVertex) * (size_t)nv);
                memcpy((char *)data + sizeof(TerrainCaveVertex) * (size_t)nv, idx, sizeof(uint16_t) * (size_t)ni);
            } else ni = nv = 0;
        } else nv = 0;
        mutex_lock(&g->lock);
        g->busyOn[wa->id] = 0;
        g->result[g->nresult++] = (Result){ j.key, nv, ni, data, ms };   /* there is room: it waited for some */
        g->meshMs = ms;
        g->meshed++;
        if (ms > g->slowestMs) g->slowestMs = ms;
    }
    mutex_unlock(&g->lock);
    free(v); free(idx); free(cave);
    return 0;
}

/* --- The cache ---------------------------------------------------------------------- */

static Entry *find(CaveGpu *g, BrickKey k) {
    uint32_t h = key_hash(k) & (ENTRIES - 1);
    while (g->table[h].used_) {
        if (key_eq(g->table[h].key, k)) return &g->table[h];
        h = (h + 1) & (ENTRIES - 1);
    }
    return NULL;
}

static Entry *insert(CaveGpu *g, BrickKey k) {
    uint32_t h = key_hash(k) & (ENTRIES - 1);
    while (g->table[h].used_) {
        if (key_eq(g->table[h].key, k)) return &g->table[h];
        h = (h + 1) & (ENTRIES - 1);
    }
    memset(&g->table[h], 0, sizeof(Entry));
    g->table[h].key = k;
    g->table[h].used_ = 1;
    g->table[h].block = -1;
    g->entries++;
    return &g->table[h];
}

/* Removes slot h, shifting back the entries probed past it. */
static void erase(CaveGpu *g, uint32_t h) {
    if (g->table[h].block >= 0) bud_free(g->bud, g->table[h].block);
    g->table[h].used_ = 0;
    g->entries--;
    uint32_t i = h;
    for (;;) {
        i = (i + 1) & (ENTRIES - 1);
        if (!g->table[i].used_) break;
        uint32_t home = key_hash(g->table[i].key) & (ENTRIES - 1);
        /* can entry i move into the hole at h? (its home not cyclically in (h, i]) */
        if ((i > h && (home <= h || home > i)) || (i < h && home <= h && home > i)) {
            g->table[h] = g->table[i];
            g->table[i].used_ = 0;
            h = i;
        }
    }
}

typedef struct Victim { uint64_t used; BrickKey key; } Victim;
static int cmp_victim(const void *a, const void *b) {
    uint64_t x = ((const Victim *)a)->used, y = ((const Victim *)b)->used;
    return x < y ? -1 : x > y;
}

/* Frees the bricks not wanted since before frame `before`, the longest unwanted
 * first, until `bytes` can be had from the pool and the table has room
 * (bytes 0: the table only). Returns whether they can. */
static int make_room(CaveGpu *g, VkDeviceSize bytes, uint64_t before) {
    int need = 0;
    if (bytes > 0) {
        int32_t b = bud_alloc(g->bud, bytes);
        if (b >= 0) { bud_free(g->bud, b); } else need = 1;
    }
    if (!need && g->entries < ENTRIES_KEEP) return 1;
    Victim *v = (Victim *)malloc(sizeof(Victim) * ENTRIES);
    if (!v) return 0;
    int n = 0;
    for (int h = 0; h < ENTRIES; ++h)
        if (g->table[h].used_ && g->table[h].used < before) v[n++] = (Victim){ g->table[h].used, g->table[h].key };
    qsort(v, (size_t)n, sizeof(Victim), cmp_victim);
    int ok = 0;
    for (int k = 0; k < n; ++k) {
        uint32_t h = key_hash(v[k].key) & (ENTRIES - 1);
        while (!key_eq(g->table[h].key, v[k].key) || !g->table[h].used_) h = (h + 1) & (ENTRIES - 1);
        erase(g, h);
        g->evicted++;
        if (g->entries >= ENTRIES_KEEP) continue;
        if (bytes == 0) { ok = 1; if (k > n / 4) break; continue; }
        int32_t b = bud_alloc(g->bud, bytes);
        if (b >= 0) { bud_free(g->bud, b); ok = 1; break; }
    }
    free(v);
    return ok || (bytes == 0 && g->entries < ENTRIES);
}

/* --- Create, destroy ---------------------------------------------------------------- */

void cave_gpu_create(Renderer *r) {
    VkCore *vk = r->vk;
    CaveGpu *g = (CaveGpu *)calloc(1, sizeof(CaveGpu));
    if (!g) fatal("out of memory allocating the caves");
    r->caves = g;
    g->table = (Entry *)calloc(ENTRIES, sizeof(Entry));
    g->bud = (Buddy *)malloc(sizeof(Buddy));
    g->want = (Job *)malloc(sizeof(Job) * QUEUE_MAX * 8);
    if (!g->table || !g->bud || !g->want) fatal("out of memory allocating the caves");
    bud_init(g->bud);
    const VkDeviceSize poolBytes = (VkDeviceSize)1 << POOL_LOG;
    g->pool = vk_buffer(vk, poolBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        g->staging[i] = vk_buffer(vk, UPLOAD_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        g->frameBuf[i] = vk_buffer(vk, sizeof(CaveFrameGPU), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host);
        g->indirect[i] = vk_buffer(vk, sizeof(VkDrawIndexedIndirectCommand) * 2 * MAX_DRAWS, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, host);
        g->holeBuf[i] = vk_buffer(vk, sizeof(CaveHolesGPU), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host);   /* a uniform buffer: every fragment of the terrain reads it (from host memory, a storage buffer cost the terrain 0.1 ms a frame) */
        memset(g->holeBuf[i].mapped, 0, sizeof(CaveHolesGPU));
        ((CaveHolesGPU *)g->holeBuf[i].mapped)->info[1] = TERRAIN_CLIP_LEVELS;
    }
    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * MAX_FRAMES },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PBR_TEXTURE_COUNT * MAX_FRAMES },
    };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = MAX_FRAMES;
    pci.poolSizeCount = 3;
    pci.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(vk->device, &pci, NULL, &g->dpool));
    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g->dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r->caveLayout;
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &ai, &g->set[i]));
        write_buf_set(vk, g->set[i], 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, r->ubo[i].buf, sizeof(FrameUBO));
        write_buf_set(vk, g->set[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, g->pool.buf, poolBytes);
        write_buf_set(vk, g->set[i], 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, g->frameBuf[i].buf, sizeof(CaveFrameGPU));
        VkDeviceSize sampleBytes;
        VkBuffer samples = terrain_gpu_samples(r, &sampleBytes);
        write_buf_set(vk, g->set[i], 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, samples, sampleBytes);
        write_buf_set(vk, g->set[i], 5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, g->holeBuf[i].buf, sizeof(CaveHolesGPU));
        write_buf_set(vk, terrain_gpu_set(r, i), 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, g->holeBuf[i].buf, sizeof(CaveHolesGPU));   /* terrain.frag's cut */
        write_material_set(r, g->set[i]);
    }
    mutex_init(&g->lock);
    cond_init(&g->wake);
    g->nworkers = cpu_count() / 8;
    g->nworkers = g->nworkers < 1 ? 1 : g->nworkers > WORKERS_MAX ? WORKERS_MAX : g->nworkers;
    for (int w = 0; w < g->nworkers; ++w) {
        g_args[w] = (WorkerArg){ g, w };
        g->worker[w] = thread_create(cave_worker, &g_args[w]);
    }
    printf("Caves: %d workers, a pool of %.0f MB, %d cache entries\n", g->nworkers, (double)poolBytes / (1024.0 * 1024.0), ENTRIES);
}

void cave_gpu_destroy(Renderer *r) {
    VkCore *vk = r->vk;
    CaveGpu *g = r->caves;
    if (!g) return;
    mutex_lock(&g->lock);
    g->quit = 1;
    cond_broadcast(&g->wake);
    mutex_unlock(&g->lock);
    for (int w = 0; w < g->nworkers; ++w) thread_join(g->worker[w]);
    mutex_destroy(&g->lock);
    cond_destroy(&g->wake);
    for (int k = 0; k < g->nresult; ++k) free(g->result[k].data);
    for (int k = 0; k < g->nsys; ++k) free_sys(g->sys[k]);
    if (g->pendingReady)
        for (int k = 0; k < g->npending; ++k) {
            int known = 0;
            for (int q = 0; q < g->nsys; ++q) known |= g->pending[k] == g->sys[q];
            if (!known) free_sys(g->pending[k]);
        }
    vkDestroyDescriptorPool(vk->device, g->dpool, NULL);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vk_buffer_destroy(vk, &g->staging[i]);
        vk_buffer_destroy(vk, &g->frameBuf[i]);
        vk_buffer_destroy(vk, &g->indirect[i]);
        vk_buffer_destroy(vk, &g->holeBuf[i]);
    }
    vk_buffer_destroy(vk, &g->pool);
    free(g->table); free(g->bud); free(g->want);
    free(g);
    r->caves = NULL;
}

/* --- The frame ---------------------------------------------------------------------- */

static int cmp_job(const void *a, const void *b) {
    float x = ((const Job *)a)->pri, y = ((const Job *)b)->pri;
    return x < y ? -1 : x > y;
}

static double snap_down(double v, double g) { return floor(v / g) * g; }
static double snap_up(double v, double g) { return ceil(v / g) * g; }

/* Takes the finished meshes into the cache and this frame's uploads, as many
 * as the staging buffer holds; returns the copies. */
static int take_results(Renderer *r, CaveGpu *g, int slot, VkBufferCopy *copies) {
    VkDeviceSize used = 0;
    int n = 0, take = 0;
    mutex_lock(&g->lock);
    for (; take < g->nresult; ++take) {
        Result *res = &g->result[take];
        VkDeviceSize vb = sizeof(TerrainCaveVertex) * (VkDeviceSize)res->nv, bytes = vb + sizeof(uint16_t) * (VkDeviceSize)res->ni;
        if (used + bytes > UPLOAD_BYTES) break;
        Entry *e = find(g, res->key);
        if (!e) {
            if (!make_room(g, 0, r->frameIndex)) { free(res->data); continue; }
            e = insert(g, res->key);
            e->used = r->frameIndex;
        }
        if (e->block >= 0) { bud_free(g->bud, e->block); e->block = -1; }   /* meshed again: the newer stands */
        e->nidx = (uint32_t)res->ni;
        e->vbytes = (uint32_t)vb;
        if (res->ni > 0) {
            int32_t b = bud_alloc(g->bud, bytes);
            if (b < 0 && make_room(g, bytes, r->frameIndex)) b = bud_alloc(g->bud, bytes);
            e = find(g, res->key);   /* make_room may have moved it */
            if (b < 0 || !e) {   /* no room: dropped, asked for again later */
                if (e) { uint32_t h = (uint32_t)(e - g->table); erase(g, h); }
                free(res->data);
                continue;
            }
            e->block = b;
            memcpy((char *)g->staging[slot].mapped + used, res->data, (size_t)bytes);
            copies[n++] = (VkBufferCopy){ used, (VkDeviceSize)b << BLOCK_LOG, bytes };
            used += bytes;
        }
        free(res->data);
    }
    memmove(g->result, g->result + take, sizeof(Result) * (size_t)(g->nresult - take));
    g->nresult -= take;
    if (take) cond_broadcast(&g->wake);
    mutex_unlock(&g->lock);
    return n;
}

/* Whether brick key k is being meshed or waits to be taken. */
static int in_flight(const CaveGpu *g, BrickKey k) {
    for (int w = 0; w < g->nworkers; ++w) if (g->busyOn[w] && key_eq(g->busy[w], k)) return 1;
    for (int q = 0; q < g->nresult; ++q) if (key_eq(g->result[q].key, k)) return 1;
    return 0;
}

void cave_gpu_update(Renderer *r, VkCommandBuffer cmd, int slot, const double cam[3], const mat4 *viewProj) {
    CaveGpu *g = r->caves;
    double t0 = glfwGetTime();

    /* 1. The systems a worker found, and the meshes finished. */
    mutex_lock(&g->lock);
    if (g->pendingReady) {
        for (int k = 0; k < g->nsys; ++k) {
            int kept = 0;
            for (int q = 0; q < g->npending; ++q) kept |= g->pending[q] == g->sys[k];
            if (!kept) free_sys(g->sys[k]);
        }
        memcpy(g->sys, g->pending, sizeof(CaveSys *) * (size_t)g->npending);
        g->nsys = g->npending;
        g->pendingReady = 0;
        g->qhead = g->nqueue = 0;   /* its jobs may name a system just dropped */
    }
    mutex_unlock(&g->lock);
    VkBufferCopy copies[RESULTS];
    int ncopy = take_results(r, g, slot, copies);
    g->uploads = ncopy;

    /* 2. Look the systems up again once the camera has moved far enough. */
    TerrainDir camDir = terrain_world_dir(cam[0], cam[2]);
    double moved = g->haveRefresh ? PLANET_RADIUS * acos(fmin(1.0, camDir.x * g->lastRefresh.x + camDir.y * g->lastRefresh.y + camDir.z * g->lastRefresh.z)) : 1e30;
    mutex_lock(&g->lock);
    if (moved > REFRESH_M && !g->refreshAsked && !g->refreshRunning && !g->pendingReady) {
        g->refreshAsked = 1;
        g->refreshAt = camDir;
        g->lastRefresh = camDir;
        g->haveRefresh = 1;
        cond_signal(&g->wake);
    }
    mutex_unlock(&g->lock);

    /* 3. Each system's rings: what is drawn, what is wanted. */
    CaveFrameGPU *fb = (CaveFrameGPU *)g->frameBuf[slot].mapped;
    VkDrawIndexedIndirectCommand *ind = (VkDrawIndexedIndirectCommand *)g->indirect[slot].mapped;
    Frustum fr = frustum_from_matrix(*viewProj);
    int nwant = 0, ndraw = 0, nrec = 0, nshelter = 0, nh = 0, np = 0;
    g->camIn = 0;
    g->camSky = 1.0f;
    CaveHolesGPU *hb = (CaveHolesGPU *)g->holeBuf[slot].mapped;
    g->missing = 0;
    g->nholes = 0;
    g->drawnTris = 0;
    for (int l = 0; l < LODS; ++l) g->drawnLod[l] = 0;
    for (int si = 0; si < g->nsys; ++si) {
        const CaveSys *s = g->sys[si];
        const TerrainCave *c = &s->cave;
        double cx, cz;
        terrain_dir_chart(&c->chart, camDir, &cx, &cz);
        double cp[3] = { cx, cam[1], cz };
        /* the camera in its air, under the ground: the cave's own air and light round it (cave.frag) */
        if (cp[0] > c->lo[0] && cp[0] < c->hi[0] && cp[1] > c->lo[1] && cp[1] < c->hi[1] && cp[2] > c->lo[2] && cp[2] < c->hi[2] &&
            terrain_cave_sdf_local(c, cp[0], cp[1], cp[2]) < 0.0 && cp[1] < terrain_cave_ground(c, cp[0], cp[2])) {
            float sky = (float)terrain_cave_sky(c, cp[0], cp[1], cp[2], NULL);
            g->camSky = g->camIn ? fmaxf(g->camSky, sky) : sky;
            g->camIn = 1;
        }
        double A[3] = { snap_down(cx, ANCHOR_M), snap_down(cam[1], ANCHOR_M), snap_down(cz, ANCHOR_M) };
        /* the anchor in the frame, and the chart's local map there */
        double ax, az, px, pz, mx, mz;
        terrain_dir_world(terrain_chart_dir(&c->chart, A[0], A[2]), &ax, &az);
        const double D = 50.0;
        terrain_dir_world(terrain_chart_dir(&c->chart, A[0] + D, A[2]), &px, &pz);
        terrain_dir_world(terrain_chart_dir(&c->chart, A[0] - D, A[2]), &mx, &mz);
        double J0 = (px - mx) / (2.0 * D), J2 = (pz - mz) / (2.0 * D);
        terrain_dir_world(terrain_chart_dir(&c->chart, A[0], A[2] + D), &px, &pz);
        terrain_dir_world(terrain_chart_dir(&c->chart, A[0], A[2] - D), &mx, &mz);
        double J1 = (px - mx) / (2.0 * D), J3 = (pz - mz) / (2.0 * D);
        CaveSysGPU *sg = &fb->sys[si];
        memset(sg, 0, sizeof *sg);
        sg->anchor[0] = (float)(ax - cam[0]); sg->anchor[1] = (float)(az - cam[2]);
        sg->anchor[2] = (float)(A[1] - cam[1]); sg->anchor[3] = (float)A[1];
        sg->J[0] = (float)J0; sg->J[1] = (float)J1; sg->J[2] = (float)J2; sg->J[3] = (float)J3;
        sg->chart[0] = (float)A[0]; sg->chart[1] = (float)A[1]; sg->chart[2] = (float)A[2];

        /* each level's box, snapped to the next level's bricks, and whether all of it is meshed */
        double box[LODS][2][3];
        int complete[LODS];
        for (int l = 0; l < LODS; ++l) {
            double G = terrain_cave_brick(l + 1 < LODS ? l + 1 : l), R = terrain_cave_lod_reach(l);
            for (int a = 0; a < 3; ++a) { box[l][0][a] = snap_down(cp[a] - R, G); box[l][1][a] = snap_up(cp[a] + R, G); }
            complete[l] = 1;
        }
        /* pass 1: the wanted (box and band) and whether each level is whole */
        for (int l = 0; l < LODS; ++l) {
            double B = terrain_cave_brick(l), G = terrain_cave_brick(l + 1 < LODS ? l + 1 : l), R = terrain_cave_lod_reach(l);
            for (int b = 0; b < s->nlive[l]; ++b) {
                const int32_t *q = s->live[l][b];
                int inBox = 1, inBand = 1;
                double far = 0.0;
                for (int a = 0; a < 3; ++a) {
                    double lo = q[a] * B, hi = lo + B;
                    inBox &= lo >= box[l][0][a] && hi <= box[l][1][a];
                    inBand &= lo >= box[l][0][a] - G && hi <= box[l][1][a] + G;
                    far = fmax(far, fabs(lo + 0.5 * B - cp[a]));
                }
                if (!inBand) continue;
                BrickKey k = { c->id, (int16_t)q[0], (int16_t)q[1], (int16_t)q[2], (uint8_t)l, 0 };
                Entry *e = find(g, k);
                if (e) { e->used = r->frameIndex; continue; }
                if (inBox) { complete[l] = 0; g->missing++; }
                if (nwant < QUEUE_MAX * 8) g->want[nwant++] = (Job){ k, s, { q[0], q[1], q[2] }, (float)(far / R) };
            }
        }
        /* the levels drawn: the coarsest, and each finer one that is whole */
        int drawn[LODS];
        for (int l = 0; l < LODS; ++l) drawn[l] = l == LODS - 1 || complete[l];
        g->levels[si] = drawn[0] + 2 * drawn[1];
        for (int l = 0; l + 1 < LODS; ++l)
            for (int a = 0; a < 3; ++a) {   /* its box, shrunk by one cell of the level outside it, from the anchor */
                double m = terrain_cave_voxel(l + 1);
                sg->box[l][0][a] = (float)(box[l][0][a] + m - A[a]);
                sg->box[l][1][a] = (float)(box[l][1][a] - m - A[a]);
            }
        /* pass 2: the draws */
        for (int l = 0; l < LODS; ++l) {
            if (!drawn[l]) continue;
            int finer = -1;
            for (int f = l - 1; f >= 0 && finer < 0; --f) if (drawn[f]) finer = f;
            double B = terrain_cave_brick(l), v = terrain_cave_voxel(l);
            for (int b = 0; b < s->nlive[l] && nrec < MAX_DRAWS; ++b) {
                const int32_t *q = s->live[l][b];
                int inBox = 1, inside = finer >= 0, cut = finer >= 0;
                double lo[3], hi[3];
                for (int a = 0; a < 3; ++a) {
                    lo[a] = q[a] * B; hi[a] = lo[a] + B;
                    inBox &= lo[a] >= box[l][0][a] && hi[a] <= box[l][1][a];
                    if (finer >= 0) {
                        double m = terrain_cave_voxel(l), s0 = box[finer][0][a] + m, s1 = box[finer][1][a] - m;
                        inside &= lo[a] >= s0 && hi[a] <= s1;
                        cut &= hi[a] > s0 && lo[a] < s1;
                    }
                }
                if (!inBox || inside) continue;
                BrickKey k = { c->id, (int16_t)q[0], (int16_t)q[1], (int16_t)q[2], (uint8_t)l, 0 };
                Entry *e = find(g, k);
                if (!e || e->nidx == 0) continue;
                /* in view? its box about the anchor, in the frame; and in
                 * the shelter map's square (every brick there, in view or
                 * not: a roof off the top of the screen shelters the rain in
                 * view) */
                double ox = lo[0] + 0.5 * B - A[0], oz = lo[2] + 0.5 * B - A[2];
                vec3 mid = v3((float)(ax - cam[0] + J0 * ox + J1 * oz), (float)(lo[1] + 0.5 * B - cam[1]), (float)(az - cam[2] + J2 * ox + J3 * oz));
                float hx = (float)(0.5 * B * (fabs(J0) + fabs(J1)) + 0.5), hz = (float)(0.5 * B * (fabs(J2) + fabs(J3)) + 0.5), hy = (float)(0.5 * B + 0.5);
                int seen = frustum_test_aabb(&fr, v3(mid.x - hx, mid.y - hy, mid.z - hz), v3(mid.x + hx, mid.y + hy, mid.z + hz));
                int over = fabsf(mid.x) - hx < SHELTER_RADIUS + 1.0f && fabsf(mid.z) - hz < SHELTER_RADIUS + 1.0f &&
                           mid.y + hy > SHELTER_LO && mid.y - hy < SHELTER_HI;
                if (!seen && !over) continue;
                CaveDrawGPU *d = &fb->draw[nrec];
                d->off[0] = (float)(lo[0] - A[0]); d->off[1] = (float)(lo[1] - A[1]); d->off[2] = (float)(lo[2] - A[2]);
                d->off[3] = (float)(v / TERRAIN_CAVE_QUANT);
                d->info[0] = si; d->info[1] = cut ? finer : -1; d->info[2] = l; d->info[3] = 0;
                VkDeviceSize at = (VkDeviceSize)e->block << BLOCK_LOG;
                VkDrawIndexedIndirectCommand dc = { e->nidx, 1, (uint32_t)((at + e->vbytes) / sizeof(uint16_t)), (int32_t)(at / sizeof(TerrainCaveVertex)), (uint32_t)nrec };
                nrec++;
                if (over) ind[MAX_DRAWS + nshelter++] = dc;
                if (!seen) continue;
                ind[ndraw++] = dc;
                g->drawnLod[l]++;
                g->drawnTris += (int)(e->nidx / 3);
            }
        }
        /* the mouths' holes: cut while their box is in the coarsest ring and
         * every brick drawn round them is meshed, with the margin of the
         * coarsest level that draws any of them */
        for (int m = 0; m < c->nmouth; ++m) {
            if (!s->hasHole[m]) continue;
            g->nholes++;
            const TerrainCaveHole *h = &s->hole[m];
            int ok = nh < TERRAIN_CAVE_HOLES, coarsest = -1;
            for (int a = 0; a < 3; ++a) ok &= h->lo[a] >= box[LODS - 1][0][a] && h->hi[a] <= box[LODS - 1][1][a];
            for (int l = 0; l < LODS && ok; ++l) {
                if (!drawn[l]) continue;
                int finer = -1;
                for (int f = l - 1; f >= 0 && finer < 0; --f) if (drawn[f]) finer = f;
                double B = terrain_cave_brick(l), v = terrain_cave_voxel(l);
                int covered = finer >= 0;   /* the hole wholly inside the finer level's shrunk box: none of it drawn here */
                for (int a = 0; a < 3 && covered; ++a) covered = h->lo[a] >= box[finer][0][a] + v && h->hi[a] <= box[finer][1][a] - v;
                if (!covered) coarsest = l;
                for (int b = 0; b < s->nround[m][l] && ok; ++b) {
                    const int32_t *q = s->round[m][l][b];
                    int inBox = 1, inside = finer >= 0;
                    for (int a = 0; a < 3; ++a) {
                        double lo = q[a] * B, hi = lo + B;
                        inBox &= lo >= box[l][0][a] && hi <= box[l][1][a];
                        if (finer >= 0) inside &= lo >= box[finer][0][a] + v && hi <= box[finer][1][a] - v;
                    }
                    if (!inBox || inside) continue;
                    BrickKey k = { c->id, (int16_t)q[0], (int16_t)q[1], (int16_t)q[2], (uint8_t)l, 0 };
                    ok = find(g, k) != NULL;
                }
            }
            if (!ok || coarsest < 0) continue;
            const double *P = c->mouth[m].p;
            double det = J0 * J3 - J1 * J2;
            CaveHoleGPU *o = &hb->hole[nh++];
            memset(o, 0, sizeof *o);
            o->anchor[0] = sg->anchor[0]; o->anchor[1] = sg->anchor[1];
            o->anchor[2] = (float)P[1]; o->anchor[3] = HOLE_MARGIN[coarsest];
            o->Jinv[0] = (float)(J3 / det); o->Jinv[1] = (float)(-J1 / det); o->Jinv[2] = (float)(-J2 / det); o->Jinv[3] = (float)(J0 / det);
            o->off[0] = (float)(A[0] - P[0]); o->off[2] = (float)(A[2] - P[2]);
            for (int a = 0; a < 3; ++a) {
                o->lo[a] = (float)(h->lo[a] - P[a]);
                o->hi[a] = (float)(h->hi[a] - P[a]);
                o->noise[a] = (float)P[a];
            }
            o->info[0] = np; o->info[1] = h->nseg; o->info[2] = h->nroom;
            memcpy(o->mask, h->mask, sizeof o->mask);
            /* its box about the camera, in the frame, for the terrain's draws (terrain_gpu.c: the cut's tiles) */
            {
                double cx = 0.5 * (h->lo[0] + h->hi[0]) - A[0], cz = 0.5 * (h->lo[2] + h->hi[2]) - A[2];
                double rx = 0.5 * (h->hi[0] - h->lo[0]), rz = 0.5 * (h->hi[2] - h->lo[2]);
                double wx = ax - cam[0] + J0 * cx + J1 * cz, wz = az - cam[2] + J2 * cx + J3 * cz;
                double ex = fabs(J0) * rx + fabs(J1) * rz + 1.0, ez = fabs(J2) * rx + fabs(J3) * rz + 1.0;
                float (*bx)[3] = g->holeBox[nh - 1];
                bx[0][0] = (float)(wx - ex); bx[1][0] = (float)(wx + ex);
                bx[0][1] = (float)(h->lo[1] - cam[1] - 1.0); bx[1][1] = (float)(h->hi[1] - cam[1] + 1.0);
                bx[0][2] = (float)(wz - ez); bx[1][2] = (float)(wz + ez);
            }
            for (int q = 0; q < h->nseg; ++q) {
                const TerrainCaveSeg *sq = &c->seg[h->seg[q]];
                CavePieceGPU *pc = &hb->piece[np++];
                for (int a = 0; a < 3; ++a) { pc->a[a] = (float)(sq->a[a] - P[a]); pc->b[a] = (float)(sq->b[a] - P[a]); }
                pc->a[3] = sq->ra; pc->b[3] = sq->rb;
            }
            for (int q = 0; q < h->nroom; ++q) {
                const TerrainCaveRoom *rm = &c->room[h->room[q]];
                CavePieceGPU *pc = &hb->piece[np++];
                for (int a = 0; a < 3; ++a) { pc->a[a] = (float)(rm->c[a] - P[a]); pc->b[a] = rm->r[a]; }
                pc->a[3] = rm->cs; pc->b[3] = rm->sn;
            }
        }
    }
    g->holesCut = nh;
    g->holePieces = 0;
    for (int q = 0; q < nh; ++q) g->holePieces = hb->hole[q].info[1] + hb->hole[q].info[2] > g->holePieces ? hb->hole[q].info[1] + hb->hole[q].info[2] : g->holePieces;
    hb->info[0] = nh;
    hb->info[2] = (int32_t)TERRAIN_CAVE_SEED_A; hb->info[3] = (int32_t)TERRAIN_CAVE_SEED_B;
    g->ndraw = ndraw;
    g->nshelter = nshelter;
    g->frames++;
    for (int si = 0; si < g->nsys; ++si)
        if (g->levels[si] != 3) { g->fellBack++; break; }
    if (g->missing > g->missingMost) g->missingMost = g->missing;

    /* 4. Hand the workers what is wanted, nearest for its level first. */
    qsort(g->want, (size_t)nwant, sizeof(Job), cmp_job);
    mutex_lock(&g->lock);
    int nq = 0;
    for (int k = 0; k < nwant && nq < QUEUE_MAX; ++k)
        if (!in_flight(g, g->want[k].key)) g->queue[nq++] = g->want[k];
    g->qhead = 0;
    g->nqueue = nq;
    if (nq) cond_broadcast(&g->wake);
    mutex_unlock(&g->lock);
    if (g->entries >= ENTRIES_KEEP) make_room(g, 0, r->frameIndex);

    g->updateMs = (glfwGetTime() - t0) * 1000.0;
    if (g->updateMs > g->slowestUpdateMs) g->slowestUpdateMs = g->updateMs;
    if (ncopy == 0) return;
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBuffer(cmd, g->staging[slot].buf, g->pool.buf, (uint32_t)ncopy, copies);
    vk_memory_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                      VK_ACCESS_2_INDEX_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

/* Draws n of the slot's indirect commands from `first`. */
static void draw_list(Renderer *r, VkCommandBuffer cmd, int slot, int first, int n) {
    CaveGpu *g = r->caves;
    if (r->vk->mdi) {
        uint32_t maxCount = r->vk->props.limits.maxDrawIndirectCount;
        for (uint32_t k = 0; k < (uint32_t)n; k += maxCount) {
            uint32_t cnt = (uint32_t)n - k < maxCount ? (uint32_t)n - k : maxCount;
            vkCmdDrawIndexedIndirect(cmd, g->indirect[slot].buf, ((uint32_t)first + k) * sizeof(VkDrawIndexedIndirectCommand), cnt, sizeof(VkDrawIndexedIndirectCommand));
        }
    } else {
        const VkDrawIndexedIndirectCommand *c = (const VkDrawIndexedIndirectCommand *)g->indirect[slot].mapped + first;
        for (int i = 0; i < n; ++i) vkCmdDrawIndexed(cmd, c[i].indexCount, 1, c[i].firstIndex, c[i].vertexOffset, c[i].firstInstance);
    }
}

void cave_gpu_draw(Renderer *r, VkCommandBuffer cmd, int slot) {
    CaveGpu *g = r->caves;
    if (g->ndraw == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->debugView ? r->caveXrayPipe : r->cavePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->cavePL, 0, 1, &g->set[slot], 0, NULL);
    vkCmdBindIndexBuffer(cmd, g->pool.buf, 0, VK_INDEX_TYPE_UINT16);
    struct { int32_t view, camIn; float camSky; } view = { g->debugView, g->camIn, g->camSky };   /* cave.glsl: CaveView */
    vkCmdPushConstants(cmd, r->cavePL, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(view), &view);
    draw_list(r, cmd, slot, 0, g->ndraw);
}

void cave_gpu_draw_shelter(Renderer *r, VkCommandBuffer cmd, int slot, const mat4 *topDown) {
    CaveGpu *g = r->caves;
    if (g->nshelter == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->caveShelterPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->caveShelterPL, 0, 1, &g->set[slot], 0, NULL);
    vkCmdBindIndexBuffer(cmd, g->pool.buf, 0, VK_INDEX_TYPE_UINT16);
    vkCmdPushConstants(cmd, r->caveShelterPL, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(mat4), topDown);
    draw_list(r, cmd, slot, MAX_DRAWS, g->nshelter);
}

void cave_gpu_set_debug(Renderer *r, int view) { r->caves->debugView = view; }

VkBuffer cave_gpu_holes(Renderer *r, int slot) { return r->caves->holeBuf[slot].buf; }

int cave_gpu_hole_boxes(Renderer *r, const float (**box)[2][3]) {
    *box = (const float (*)[2][3])r->caves->holeBox;
    return r->caves->holesCut;
}

/* The terrain's levels as it draws them this frame, for cave.frag's cut
 * (terrain/cave_clip.glsl): after terrain_gpu_update. */
void cave_gpu_levels(Renderer *r, int slot) {
    CaveHolesGPU *hb = (CaveHolesGPU *)r->caves->holeBuf[slot].mapped;
    const TerrainPush *push;
    int tdrawn = terrain_gpu_levels(r, &push);
    hb->info[1] = tdrawn;
    for (int l = tdrawn; l < TERRAIN_CLIP_LEVELS; ++l) {
        const TerrainPush *p = &push[l];
        CaveLevelGPU *o = &hb->lv[l];
        o->a[0] = p->anchor[0]; o->a[1] = p->anchor[1]; o->a[2] = p->morph[0]; o->a[3] = p->morph[1];
        o->b[0] = p->offset[0]; o->b[1] = p->offset[1]; o->b[2] = p->spacing; o->b[3] = p->fade;
        o->c[0] = p->slot[0]; o->c[1] = p->slot[1]; o->c[2] = p->pslot[0]; o->c[3] = p->pslot[1];
        o->d[0] = p->level; o->d[1] = p->top; o->d[2] = o->d[3] = 0;
    }
}

/* Settled once every brick the rings want is meshed and nothing is being looked up or meshed. */
bool cave_gpu_settled(Renderer *r) {
    CaveGpu *g = r->caves;
    mutex_lock(&g->lock);
    int idle = !g->refreshAsked && !g->refreshRunning && !g->pendingReady && g->nresult == 0 && g->qhead >= g->nqueue;
    for (int w = 0; w < g->nworkers; ++w) idle &= !g->busyOn[w];
    mutex_unlock(&g->lock);
    return idle && g->missing == 0;
}

void cave_gpu_stats(Renderer *r, char *buf, size_t n) {
    CaveGpu *g = r->caves;
    mutex_lock(&g->lock);
    double meshMs = g->meshMs, slowest = g->slowestMs, refreshMs = g->refreshMs, holeMs = g->holeMs;
    int meshed = g->meshed, queued = g->nqueue - g->qhead;
    mutex_unlock(&g->lock);
    char sys[96] = "";
    size_t used = 0;
    for (int k = 0; k < g->nsys && used + 24 < sizeof sys; ++k)
        used += (size_t)snprintf(sys + used, sizeof sys - used, "%s%08x (levels %s)", k ? ", " : " ", g->sys[k]->cave.id,
                                 g->levels[k] == 3 ? "0-2" : g->levels[k] == 2 ? "1-2" : g->levels[k] == 1 ? "0, 2" : "2");
    snprintf(buf, n, "caves: %d systems near%s; %d bricks drawn (%d, %d, %d by level), %d triangles; %d missing, %d queued;\n"
             "       %d bricks cached (%.1f of %d MB), %d evicted; %d meshed, the last in %.2f ms on a worker (slowest %.2f); systems looked up in %.0f ms;\n"
             "       the frame's part %.2f ms (slowest %.2f), %d uploads; in %d of %d frames a finer level waited, at most %d bricks missing;\n"
             "       %d mouths' holes near, %d cut, up to %d pieces of the field each (a system's found in %.0f ms)",
             g->nsys, sys, g->ndraw, g->drawnLod[0], g->drawnLod[1], g->drawnLod[2], g->drawnTris, g->missing, queued, g->entries,
             (double)g->bud->used / (1024.0 * 1024.0), 1 << (POOL_LOG - 20), g->evicted, meshed, meshMs, slowest, refreshMs, g->updateMs, g->slowestUpdateMs, g->uploads,
             g->fellBack, g->frames, g->missingMost, g->nholes, g->holesCut, g->holePieces, holeMs);
}
