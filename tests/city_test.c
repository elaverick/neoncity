/* Determinism and layout sanity tests for the city generator.
 *
 * The core promise of the streamer is that a chunk is rebuilt bit-identically
 * whenever it is revisited, no matter the order chunks are generated in or
 * which worker thread builds them. This checks exactly that, then checks that
 * the plan-driven layout is physically sensible. */
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

static uint64_t chunk_checksum(int cx, int cz, uint32_t *countOut, uint32_t *lightsOut) {
    Mesh m = { 0 };
    city_build_mesh(cx, cz, &m);
    uint64_t h = fnv(m.v, (size_t)m.count * sizeof(Vertex), 0xcbf29ce484222325ULL);
    h = fnv(m.mn, sizeof(m.mn), h);
    h = fnv(m.mx, sizeof(m.mx), h);
    h = fnv(m.lights, (size_t)m.nlights * 16, h);
    if (countOut) *countOut = m.count;
    if (lightsOut) *lightsOut = m.nlights;
    mesh_free(&m);
    return h;
}

typedef struct Work { int start, reverse; uint64_t out[N_CHUNKS]; } Work;

static int worker(void *arg) {
    Work *w = (Work *)arg;
    /* Each thread walks the list in a different order. */
    for (int k = 0; k < N_CHUNKS; ++k) {
        int j = (w->start + k) % N_CHUNKS;
        int i = w->reverse ? N_CHUNKS - 1 - j : j;
        w->out[i] = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL);
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

int main(void) {
    /* A scattered set of chunks, including far-away and negative coordinates. */
    Rng r = rng_make(1234);
    for (int i = 0; i < N_CHUNKS; ++i) {
        int far = i % 4 == 0 ? 1000000 : 40;
        g_cx[i] = rng_int(&r, -far, far);
        g_cz[i] = rng_int(&r, -far, far);
    }

    /* 1. Reference pass, in order. Track mesh sizes against the GPU page budget. */
    uint32_t maxVerts = 0, maxLights = 0;
    double total = 0, totalLights = 0;
    for (int i = 0; i < N_CHUNKS; ++i) {
        uint32_t c, l;
        g_ref[i] = chunk_checksum(g_cx[i], g_cz[i], &c, &l);
        if (c > maxVerts) maxVerts = c;
        if (l > maxLights) maxLights = l;
        total += c;
        totalLights += l;
    }
    printf("mesh vertices per chunk: avg %.0f, max %u (budget %d)\n", total / N_CHUNKS, maxVerts, MAX_CHUNK_PAGES * PAGE_VERTS);
    printf("light records per chunk: avg %.0f, max %u (page %d)\n", totalLights / N_CHUNKS, maxLights, LIGHT_PAGE_RECS);
    CHECK(maxVerts <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk exceeds page budget");
    CHECK(maxLights <= (uint32_t)LIGHT_PAGE_RECS, "chunk light table exceeds its page");

    /* 2. Reverse order, interleaved with unrelated generation, must match. */
    for (int i = N_CHUNKS - 1; i >= 0; --i) {
        chunk_checksum(g_cx[i] + 7, g_cz[i] - 3, NULL, NULL);
        uint64_t h = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL);
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
    int buildings = 0, lamps = 0, kinds[3] = { 0 }, zones[5] = { 0 }, megaX = 0, megaZ = 0, foundMega = 0;
    uint32_t coreLights = 0, coreVerts = 0;
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
                lamps++;
            }
            if ((cx + cz) % 3 == 0) {
                Mesh m = { 0 };
                city_build_mesh(cx, cz, &m);
                if (m.nlights > coreLights) coreLights = m.nlights;
                if (m.count > coreVerts) coreVerts = m.count;
                CHECK(m.nlights < LIGHT_PAGE_RECS, "chunk (%d,%d) light table is full (%u records)", cx, cz, m.nlights);
                CHECK(m.mx[1] <= city_height_cap(cx, cz), "chunk (%d,%d) geometry (%.1f m) exceeds culling bound %.1f",
                      cx, cz, m.mx[1], city_height_cap(cx, cz));
                CHECK(m.count == 0 || (m.mn[0] >= -CITY_OVERHANG && m.mx[0] <= CHUNK_SIZE + CITY_OVERHANG &&
                                       m.mn[2] >= -CITY_OVERHANG && m.mx[2] <= CHUNK_SIZE + CITY_OVERHANG),
                      "chunk (%d,%d) geometry overhangs by more than %.0f m", cx, cz, CITY_OVERHANG);
                mesh_free(&m);
            }
        }
    int n = (2 * R + 1) * (2 * R + 1);
    printf("%d chunks around the spawn: %.1f buildings and %.1f lamps per chunk\n", n, (double)buildings / n, (double)lamps / n);
    printf("  busiest chunk: %u vertices, %u light records\n", coreVerts, coreLights);
    printf("  kinds: %d blocks, %d towers, %d arcologies\n", kinds[0], kinds[1], kinds[2]);
    printf("  zones: %d core, %d centre, %d estate, %d industrial\n", zones[0], zones[1], zones[2], zones[3]);
    if (foundMega) printf("  first arcology near world (%d, %d)\n", megaX, megaZ);
    CHECK(buildings > n * 3, "too few buildings around the spawn");
    CHECK(lamps > n, "too few lamps around the spawn");

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
