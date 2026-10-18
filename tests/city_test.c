/* Determinism and layout sanity tests for the city generator.
 *
 * The core promise of the streamer is that a chunk is rebuilt bit-identically
 * whenever it is revisited, no matter the order chunks are generated in or
 * which worker thread builds them. This checks exactly that, then checks that
 * the plan-driven layout is physically sensible.
 *
 * city_test --fingerprint prints a hash of each layer over a fixed set of
 * chunks instead, to compare what two builds generate. */
#include "city.h"
#include "hash.h"
#include "mathlib.h"
#include "thread.h"

#define N_CHUNKS 400
#define THREADS  8

static int g_cx[N_CHUNKS], g_cz[N_CHUNKS];
static uint64_t g_ref[N_CHUNKS];
static int g_failures;

static uint64_t fnv(const void *data, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

/* All four layers: the chunk itself, its office lobbies, its litter and the lobbies' plants. */
static uint64_t chunk_checksum(int cx, int cz, uint32_t *countOut, uint32_t *lightsOut, uint32_t *interiorOut, uint32_t *litterOut, uint32_t *plantOut) {
    Mesh m = { 0 };
    city_build_mesh(cx, cz, &m);
    uint64_t h = fnv(m.v, (size_t)m.count * sizeof(Vertex), 0xcbf29ce484222325ULL);
    h = fnv(m.mn, sizeof(m.mn), h);
    h = fnv(m.mx, sizeof(m.mx), h);
    h = fnv(m.lights, (size_t)m.nlights * 16, h);
    if (countOut) *countOut = m.count;
    if (lightsOut) *lightsOut = m.nlights;
    mesh_free(&m);
    Mesh in = { 0 };
    city_build_interiors(cx, cz, &in);
    h = fnv(in.v, (size_t)in.count * sizeof(Vertex), h);
    if (interiorOut) *interiorOut = in.count;
    mesh_free(&in);
    Mesh lt = { 0 };
    city_build_litter(cx, cz, &lt);
    h = fnv(lt.v, (size_t)lt.count * sizeof(Vertex), h);
    if (litterOut) *litterOut = lt.count;
    mesh_free(&lt);
    Mesh pl = { 0 };
    city_build_plants(cx, cz, &pl);
    h = fnv(pl.v, (size_t)pl.count * sizeof(Vertex), h);
    if (plantOut) *plantOut = pl.count;
    mesh_free(&pl);
    return h;
}

typedef struct Work { int start, reverse; uint64_t out[N_CHUNKS]; } Work;

static int worker(void *arg) {
    Work *w = (Work *)arg;
    /* Each thread walks the list in a different order. */
    for (int k = 0; k < N_CHUNKS; ++k) {
        int j = (w->start + k) % N_CHUNKS;
        int i = w->reverse ? N_CHUNKS - 1 - j : j;
        w->out[i] = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL, NULL, NULL, NULL);
    }
    return 0;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static int in_curb(const CurbPoly *c, double x, double z) {
    double sgn = 0.0;
    for (int i = 0; i < c->n; ++i) {
        const double *a = c->v[i], *b = c->v[(i + 1) % c->n];
        double cr = (b[0] - a[0]) * (z - a[1]) - (b[1] - a[1]) * (x - a[0]);
        if (fabs(cr) < 1e-6) continue;
        if (sgn == 0.0) sgn = cr;
        else if ((cr > 0.0) != (sgn > 0.0)) return 0;
    }
    return c->n >= 3;
}

static void corners(const Building *b, double out[4][2]) {
    float xs[4] = { b->foot.x0, b->foot.x1, b->foot.x1, b->foot.x0 };
    float zs[4] = { b->foot.z0, b->foot.z0, b->foot.z1, b->foot.z1 };
    for (int k = 0; k < 4; ++k) {
        out[k][0] = b->ox + b->c * xs[k] - b->s * zs[k];
        out[k][1] = b->oz + b->s * xs[k] + b->c * zs[k];
    }
}

static int point_in_building(const Building *b, double x, double z, double shrink) {
    double dx = x - b->ox, dz = z - b->oz;
    double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
    return lx > b->foot.x0 + shrink && lx < b->foot.x1 - shrink && lz > b->foot.z0 + shrink && lz < b->foot.z1 - shrink;
}

/* Separating-axis test on two oriented footprints, allowing a small touch. */
static int buildings_overlap(const Building *a, const Building *b) {
    double ca[4][2], cb[4][2];
    corners(a, ca); corners(b, cb);
    double ax[4][2] = { { a->c, a->s }, { -a->s, a->c }, { b->c, b->s }, { -b->s, b->c } };
    for (int k = 0; k < 4; ++k) {
        double amin = 1e300, amax = -1e300, bmin = 1e300, bmax = -1e300;
        for (int i = 0; i < 4; ++i) {
            double pa = ca[i][0] * ax[k][0] + ca[i][1] * ax[k][1], pb = cb[i][0] * ax[k][0] + cb[i][1] * ax[k][1];
            amin = fmin(amin, pa); amax = fmax(amax, pa); bmin = fmin(bmin, pb); bmax = fmax(bmax, pb);
        }
        if (amax <= bmin + 0.05 || bmax <= amin + 0.05) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * Fingerprint (city_test --fingerprint): one hash per layer over a fixed set
 * of chunks, printed so that runs of two builds can be compared. The tests
 * above only compare chunks within one run; this catches a change in what
 * the generator makes at all (a refactor that should be pure, say). Layout
 * structs are hashed field by field: their padding is not deterministic.
 * ------------------------------------------------------------------------- */

enum { FP_MESH, FP_LIGHTS, FP_SKY, FP_AIR, FP_INTERIORS, FP_PLANTS, FP_LITTER, FP_LAYOUT, FP_COUNT };
static const char *const FP_NAMES[FP_COUNT] = { "mesh", "lights", "sky", "air", "interiors", "plants", "litter", "layout" };

#define FNV_FIELD(h, x) ((h) = fnv(&(x), sizeof(x), (h)))

static uint64_t fnv_mesh(const Mesh *m, uint64_t h) {
    h = fnv(&m->count, sizeof(m->count), h);
    h = fnv(m->v, (size_t)m->count * sizeof(Vertex), h);
    if (m->count) { h = fnv(m->mn, sizeof(m->mn), h); h = fnv(m->mx, sizeof(m->mx), h); }
    return h;
}

static uint64_t fnv_layout(const BlockLayout *L, uint64_t h) {
    FNV_FIELD(h, L->density);
    FNV_FIELD(h, L->nbuild);
    for (int i = 0; i < L->nbuild; ++i) {
        const Building *b = &L->build[i];
        FNV_FIELD(h, b->foot); FNV_FIELD(h, b->ox); FNV_FIELD(h, b->oz); FNV_FIELD(h, b->c); FNV_FIELD(h, b->s);
        FNV_FIELD(h, b->height); FNV_FIELD(h, b->seed); FNV_FIELD(h, b->kind); FNV_FIELD(h, b->frontage);
        FNV_FIELD(h, b->shop); FNV_FIELD(h, b->zone); FNV_FIELD(h, b->density); FNV_FIELD(h, b->base);
    }
    FNV_FIELD(h, L->nlamps);
    for (int i = 0; i < L->nlamps; ++i) {
        const Lamp *lp = &L->lamp[i];
        FNV_FIELD(h, lp->x); FNV_FIELD(h, lp->z); FNV_FIELD(h, lp->dx); FNV_FIELD(h, lp->dz); FNV_FIELD(h, lp->type);
    }
    FNV_FIELD(h, L->ncurbs);
    for (int i = 0; i < L->ncurbs; ++i) {
        FNV_FIELD(h, L->curb[i].n);
        h = fnv(L->curb[i].v, sizeof(L->curb[i].v[0]) * (size_t)L->curb[i].n, h);
    }
    FNV_FIELD(h, L->ndrops);
    for (int i = 0; i < L->ndrops; ++i) {
        const DropKerb *d = &L->drop[i];
        FNV_FIELD(h, d->edge); FNV_FIELD(h, d->ox); FNV_FIELD(h, d->oz); FNV_FIELD(h, d->tx); FNV_FIELD(h, d->tz);
        FNV_FIELD(h, d->nx); FNV_FIELD(h, d->nz); FNV_FIELD(h, d->n);
        h = fnv(d->v, sizeof(d->v[0]) * (size_t)d->n, h);
        h = fnv(d->lab, (size_t)d->n, h);
        FNV_FIELD(h, d->tc); FNV_FIELD(h, d->tu); FNV_FIELD(h, d->nopen);
        h = fnv(d->open, sizeof(d->open[0]) * (size_t)d->nopen, h);
    }
    FNV_FIELD(h, L->ncoll); FNV_FIELD(h, L->ncollv);
    h = fnv(L->coll, sizeof(L->coll[0]) * (size_t)L->ncoll, h);
    h = fnv(L->collv, sizeof(L->collv[0]) * (size_t)L->ncollv, h);
    return h;
}

static void fingerprint_chunk(int cx, int cz, uint64_t fp[FP_COUNT]) {
    static BlockLayout L;
    Mesh m = { 0 };
    city_build_mesh(cx, cz, &m);
    fp[FP_MESH] = fnv_mesh(&m, fp[FP_MESH]);
    fp[FP_LIGHTS] = fnv(&m.nlights, sizeof(m.nlights), fp[FP_LIGHTS]);
    fp[FP_LIGHTS] = fnv(m.lights, (size_t)m.nlights * 16, fp[FP_LIGHTS]);
    fp[FP_SKY] = fnv(&m.nsky, sizeof(m.nsky), fp[FP_SKY]);
    fp[FP_SKY] = fnv(m.sky, (size_t)m.nsky * sizeof(SkyLight), fp[FP_SKY]);
    fp[FP_AIR] = fnv(&m.nair, sizeof(m.nair), fp[FP_AIR]);
    fp[FP_AIR] = fnv(m.air, (size_t)m.nair * sizeof(SkyLight), fp[FP_AIR]);
    mesh_free(&m);
    city_build_interiors(cx, cz, &m);
    fp[FP_INTERIORS] = fnv_mesh(&m, fp[FP_INTERIORS]);
    mesh_free(&m);
    city_build_plants(cx, cz, &m);
    fp[FP_PLANTS] = fnv_mesh(&m, fp[FP_PLANTS]);
    mesh_free(&m);
    city_build_litter(cx, cz, &m);
    fp[FP_LITTER] = fnv_mesh(&m, fp[FP_LITTER]);
    mesh_free(&m);
    city_layout(cx, cz, &L);
    fp[FP_LAYOUT] = fnv_layout(&L, fp[FP_LAYOUT]);
}

/* The 9 x 9 chunks round the home town centre, the 3 x 3 round the nearest
 * arcology to it, and the first 100 scattered chunks of the tests. */
static int fingerprint(void) {
    uint64_t fp[FP_COUNT];
    for (int k = 0; k < FP_COUNT; ++k) fp[k] = 0xcbf29ce484222325ULL;
    DVec2 home = plan_town_centre(0, 0);
    int hx = (int)floor(home.x / CHUNK_SIZE), hz = (int)floor(home.z / CHUNK_SIZE);
    int n = 0;
    for (int cz = hz - 4; cz <= hz + 4; ++cz)
        for (int cx = hx - 4; cx <= hx + 4; ++cx) { fingerprint_chunk(cx, cz, fp); n++; }

    static BlockLayout L;
    int megaCx = 0, megaCz = 0, found = 0;
    for (int ring = 0; ring <= 40 && !found; ++ring)
        for (int cz = hz - ring; cz <= hz + ring && !found; ++cz)
            for (int cx = hx - ring; cx <= hx + ring && !found; ++cx) {
                if (abs(cx - hx) != ring && abs(cz - hz) != ring) continue;
                city_layout(cx, cz, &L);
                for (int i = 0; i < L.nbuild && !found; ++i)
                    if (L.build[i].kind == BK_MEGA) { found = 1; megaCx = cx; megaCz = cz; }
            }
    if (found)
        for (int cz = megaCz - 1; cz <= megaCz + 1; ++cz)
            for (int cx = megaCx - 1; cx <= megaCx + 1; ++cx) { fingerprint_chunk(cx, cz, fp); n++; }

    for (int i = 0; i < 100; ++i) { fingerprint_chunk(g_cx[i], g_cz[i], fp); n++; }

    printf("fingerprint over %d chunks (arcology chunk %s (%d, %d))\n", n, found ? "at" : "not found near", megaCx, megaCz);
    for (int k = 0; k < FP_COUNT; ++k) printf("  %-10s %016llx\n", FP_NAMES[k], (unsigned long long)fp[k]);
    return 0;
}

int main(int argc, char **argv) {
    /* A scattered set of chunks, including far-away and negative coordinates. */
    Rng r = rng_make(1234);
    for (int i = 0; i < N_CHUNKS; ++i) {
        int far = i % 4 == 0 ? 1000000 : 40;
        g_cx[i] = rng_int(&r, -far, far);
        g_cz[i] = rng_int(&r, -far, far);
    }
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();

    /* 1. Reference pass, in order. Track mesh sizes against the GPU page budget. */
    uint32_t maxVerts = 0, maxLights = 0, maxInterior = 0, maxLitter = 0, maxPlants = 0;
    double total = 0, totalLights = 0, totalInterior = 0, totalLitter = 0, totalPlants = 0;
    for (int i = 0; i < N_CHUNKS; ++i) {
        uint32_t c, l, in, lt, pv;
        g_ref[i] = chunk_checksum(g_cx[i], g_cz[i], &c, &l, &in, &lt, &pv);
        if (pv > maxPlants) maxPlants = pv;
        totalPlants += pv;
        if (c > maxVerts) maxVerts = c;
        if (l > maxLights) maxLights = l;
        if (in > maxInterior) maxInterior = in;
        if (lt > maxLitter) maxLitter = lt;
        total += c;
        totalLights += l;
        totalInterior += in;
        totalLitter += lt;
    }
    printf("litter vertices per chunk: avg %.0f, max %u (budget %d)\n", totalLitter / N_CHUNKS, maxLitter, MAX_CHUNK_PAGES * PAGE_VERTS);
    CHECK(maxLitter <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk litter exceeds page budget");
    printf("lobby vertices per chunk: avg %.0f, max %u (budget %d)\n", totalInterior / N_CHUNKS, maxInterior, MAX_CHUNK_PAGES * PAGE_VERTS);
    CHECK(maxInterior <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk lobbies exceed page budget");
    printf("plant vertices per chunk: avg %.0f, max %u (budget %d)\n", totalPlants / N_CHUNKS, maxPlants, MAX_CHUNK_PAGES * PAGE_VERTS);
    CHECK(maxPlants <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk plants exceed page budget");
    printf("mesh vertices per chunk: avg %.0f, max %u (budget %d)\n", total / N_CHUNKS, maxVerts, MAX_CHUNK_PAGES * PAGE_VERTS);
    printf("light records per chunk: avg %.0f, max %u (page %d)\n", totalLights / N_CHUNKS, maxLights, LIGHT_PAGE_RECS);
    CHECK(maxVerts <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk exceeds page budget");
    CHECK(maxLights <= (uint32_t)LIGHT_PAGE_RECS, "chunk light table exceeds its page");

    /* 2. Reverse order, interleaved with unrelated generation, must match. */
    for (int i = N_CHUNKS - 1; i >= 0; --i) {
        chunk_checksum(g_cx[i] + 7, g_cz[i] - 3, NULL, NULL, NULL, NULL, NULL);
        uint64_t h = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL, NULL, NULL, NULL);
        CHECK(h == g_ref[i], "chunk (%d,%d) differs when generated in a different order", g_cx[i], g_cz[i]);
    }

    /* 3. Concurrent generation on several threads, each in its own order. */
    static Work work[THREADS];
    Thread *t[THREADS];
    for (int k = 0; k < THREADS; ++k) {
        work[k].start = k * 37;
        work[k].reverse = k & 1;
        t[k] = thread_create(worker, &work[k]);
    }
    for (int k = 0; k < THREADS; ++k) thread_join(t[k]);
    for (int k = 0; k < THREADS; ++k)
        for (int i = 0; i < N_CHUNKS; ++i)
            CHECK(work[k].out[i] == g_ref[i], "thread %d: chunk (%d,%d) differs", k, g_cx[i], g_cz[i]);

    /* 4. Layout sanity around the spawn: buildings stand on blocks (never on a
     *    road), do not overlap, and fit in the overhang budget; lamps stand on
     *    sidewalks clear of buildings; geometry stays under the culling bound. */
    DVec2 home = plan_town_centre(0, 0);
    int hx = (int)floor(home.x / CHUNK_SIZE), hz = (int)floor(home.z / CHUNK_SIZE);
    int buildings = 0, lamps = 0, drops = 0, slabs = 0, kinds[3] = { 0 }, zones[5] = { 0 }, megaX = 0, megaZ = 0, foundMega = 0;
    uint32_t coreLights = 0, coreVerts = 0, litterMax = 0;
    double litterSum = 0;
    int litterChunks = 0;
    const int R = 20;
    for (int cz = hz - R; cz <= hz + R; ++cz)
        for (int cx = hx - R; cx <= hx + R; ++cx) {
            static BlockLayout L;
            city_layout(cx, cz, &L);
            for (int i = 0; i < L.nbuild; ++i) {
                const Building *b = &L.build[i];
                double c[4][2];
                corners(b, c);
                for (int k = 0; k < 4; ++k) {
                    int ok = 0;
                    for (int j = 0; j < L.ncurbs && !ok; ++j) ok = in_curb(&L.curb[j], c[k][0], c[k][1]);
                    CHECK(ok, "chunk (%d,%d) building %d corner %d is off its block", cx, cz, i, k);
                }
                float w = b->foot.x1 - b->foot.x0, d = b->foot.z1 - b->foot.z0;
                CHECK(0.5f * sqrtf(w * w + d * d) <= CITY_OVERHANG - 4.0f, "chunk (%d,%d) building %d too large to stream", cx, cz, i);
                for (int j = i + 1; j < L.nbuild; ++j)
                    CHECK(!buildings_overlap(b, &L.build[j]), "chunk (%d,%d) buildings %d and %d overlap", cx, cz, i, j);
                kinds[b->kind]++;
                zones[b->zone]++;
                buildings++;
                if (b->kind == BK_MEGA && !foundMega) { foundMega = 1; megaX = (int)b->ox; megaZ = (int)b->oz; }
            }
            for (int i = 0; i < L.nlamps; ++i) {
                const Lamp *lp = &L.lamp[i];
                int ok = 0;
                for (int j = 0; j < L.ncurbs && !ok; ++j) ok = in_curb(&L.curb[j], lp->x, lp->z);
                CHECK(ok, "chunk (%d,%d) lamp %d is not on a sidewalk", cx, cz, i);
                for (int j = 0; j < L.nbuild; ++j)
                    CHECK(!point_in_building(&L.build[j], lp->x, lp->z, -0.25), "chunk (%d,%d) lamp %d inside building %d", cx, cz, i, j);
                for (int j = 0; j < L.ndrops; ++j)
                    CHECK(!city_drop_contains(&L.drop[j], lp->x, lp->z, 0.2), "chunk (%d,%d) lamp %d stands in a drop kerb", cx, cz, i);
                lamps++;
            }
            /* Drop kerbs lie on their blocks, keep a lowered kerb wide enough to
             * cross by, and don't run into each other (bar a shared corner). */
            for (int i = 0; i < L.ndrops; ++i) {
                const DropKerb *d = &L.drop[i];
                double kerb = 0.0, mx = 0.0, mz = 0.0;
                for (int k = 0; k < d->n; ++k) {
                    int ok = 0;
                    for (int j = 0; j < L.ncurbs && !ok; ++j) ok = in_curb(&L.curb[j], d->v[k][0], d->v[k][1]);
                    CHECK(ok, "chunk (%d,%d) drop kerb %d corner %d is off its block", cx, cz, i, k);
                    if (d->lab[k] == DE_KERB) kerb += hypot(d->v[(k + 1) % d->n][0] - d->v[k][0], d->v[(k + 1) % d->n][1] - d->v[k][1]);
                    mx += d->v[k][0] / d->n; mz += d->v[k][1] / d->n;
                }
                CHECK(kerb >= 1.5 - 1e-6, "chunk (%d,%d) drop kerb %d is only %.2f m wide", cx, cz, i, kerb);
                for (int j = 0; j < L.ndrops; ++j)
                    CHECK(j == i || !city_drop_contains(&L.drop[j], mx, mz, -0.05), "chunk (%d,%d) drop kerbs %d and %d overlap", cx, cz, i, j);
                CHECK(fabs(city_drop_height(d, mx, mz) - 0.5 * (DROP_KERB_HEIGHT + SIDEWALK_HEIGHT)) < 0.1f,
                      "chunk (%d,%d) drop kerb %d slopes the wrong way", cx, cz, i);
                /* owned by the chunk holding its middle, to count each once */
                if ((int)floor(mx / CHUNK_SIZE) == cx && (int)floor(mz / CHUNK_SIZE) == cz) {
                    drops++;
                    for (int r = 0; r < DROP_ROWS; ++r) slabs += (int)((d->tu[r][1] - d->tu[r][0]) / DROP_TILE + 0.5);
                }
            }
            if ((cx + cz) % 3 == 0) {
                Mesh m = { 0 };
                city_build_mesh(cx, cz, &m);
                if (m.nlights > coreLights) coreLights = m.nlights;
                if (m.count > coreVerts) coreVerts = m.count;
                CHECK(m.nlights < LIGHT_PAGE_RECS, "chunk (%d,%d) light table is full (%u records)", cx, cz, m.nlights);
                float g0, g1;
                city_ground_range(cx, cz, &g0, &g1);
                CHECK(m.mx[1] <= g1 + city_height_cap(cx, cz) && m.mn[1] >= g0 - 1.0f,
                      "chunk (%d,%d) geometry (%.1f..%.1f m) exceeds culling bounds %.1f..%.1f",
                      cx, cz, m.mn[1], m.mx[1], g0 - 1.0f, g1 + city_height_cap(cx, cz));
                CHECK(m.count == 0 || (m.mn[0] >= -CITY_OVERHANG && m.mx[0] <= CHUNK_SIZE + CITY_OVERHANG &&
                                       m.mn[2] >= -CITY_OVERHANG && m.mx[2] <= CHUNK_SIZE + CITY_OVERHANG),
                      "chunk (%d,%d) geometry overhangs by more than %.0f m", cx, cz, CITY_OVERHANG);
                mesh_free(&m);
            }
            /* Litter near the spawn: within its page budget, on the ground, never far outside its chunk. */
            if (abs(cx - hx) <= 4 && abs(cz - hz) <= 4) {
                Mesh m = { 0 };
                city_build_litter(cx, cz, &m);
                if (m.count > litterMax) litterMax = m.count;
                litterSum += m.count;
                litterChunks++;
                CHECK(m.count <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk (%d,%d) litter exceeds page budget", cx, cz);
                float up0 = 0.0f, up1 = 0.0f;   /* heights above the ground under each vertex */
                for (uint32_t i = 0; i < m.count; ++i) {
                    float up = m.v[i].pos[1] - m.v[i].base;
                    if (i == 0 || up < up0) up0 = up;
                    if (i == 0 || up > up1) up1 = up;
                }
                CHECK(m.count == 0 || (m.mn[0] >= -1.0f && m.mx[0] <= CHUNK_SIZE + 1.0f && m.mn[2] >= -1.0f && m.mx[2] <= CHUNK_SIZE + 1.0f &&
                                       up0 >= -0.05f && up1 <= SIDEWALK_HEIGHT + 1.2f),
                      "chunk (%d,%d) litter strays from the ground (%.2f..%.2f m up)", cx, cz, up0, up1);
                mesh_free(&m);
            }
        }
    int n = (2 * R + 1) * (2 * R + 1);
    printf("%d chunks around the spawn: %.1f buildings and %.1f lamps per chunk\n", n, (double)buildings / n, (double)lamps / n);
    printf("  busiest chunk: %u vertices, %u light records\n", coreVerts, coreLights);
    printf("  %.1f drop kerbs per chunk, %.1f tactile slabs each\n", (double)drops / n, drops ? (double)slabs / drops : 0.0);
    printf("  litter near the spawn: avg %.0f, max %u vertices per chunk\n", litterSum / litterChunks, litterMax);
    printf("  kinds: %d blocks, %d towers, %d arcologies\n", kinds[0], kinds[1], kinds[2]);
    printf("  zones: %d core, %d centre, %d estate, %d industrial\n", zones[0], zones[1], zones[2], zones[3]);
    if (foundMega) printf("  first arcology near world (%d, %d)\n", megaX, megaZ);
    CHECK(buildings > n * 3, "too few buildings around the spawn");
    CHECK(lamps > n, "too few lamps around the spawn");
    CHECK(drops > n, "too few drop kerbs around the spawn");

    /* 5. The spawn point is on a sidewalk and clear of buildings. */
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    {
        static BlockLayout L;
        city_layout((int)floor(sx / CHUNK_SIZE), (int)floor(sz / CHUNK_SIZE), &L);
        int ok = 0;
        for (int j = 0; j < L.ncurbs && !ok; ++j) ok = in_curb(&L.curb[j], sx, sz);
        CHECK(ok, "spawn (%.1f, %.1f) is not on a sidewalk", sx, sz);
        printf("spawn at (%.1f, %.1f) yaw %.0f deg, plan density %.2f\n", sx, sz, yaw * 57.2958f, plan_density(sx, sz));
    }

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all city tests passed (%d chunks x %d orders/threads)\n", N_CHUNKS, THREADS + 2);
    return 0;
}
