/* Determinism and layout sanity tests for the city generator.
 *
 * The core promise of the streamer is that a chunk is rebuilt bit-identically
 * whenever it is revisited, no matter the order chunks are generated in or
 * which worker thread builds them. This checks exactly that, then checks that
 * the plan-driven layout is physically sensible.
 *
 * city_test --fingerprint prints a hash of each layer over a fixed set of
 * chunks instead, to compare what two builds generate; city_test --proxies
 * surveys the distant proxies of the whole city; --props and --screens print
 * views of the street furniture and the video screens; --grass views of the
 * parks, fields, verge and land grass round a point; --zfight X Z R lists the
 * coplanar overlapping surfaces (z-fighting) within R m of a point. */
#include "city.h"
#include "grass_rules.h"
#include "terrain.h"
#include "hash.h"
#include "mathlib.h"
#include "thread.h"
#include "models.h"
#include "advert_def.h"
#include "pmodel.h"
#include <time.h>

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

/* All five layers: the chunk itself, its office lobbies, its litter, the lobbies' plants and its street furniture. */
static uint64_t chunk_checksum(int cx, int cz, uint32_t *countOut, uint32_t *lightsOut, uint32_t *interiorOut, uint32_t *litterOut, uint32_t *plantOut,
                               uint32_t *propsOut) {
    Mesh m = { 0 };
    city_build_mesh(cx, cz, &m);
    uint64_t h = fnv(m.v, (size_t)m.count * sizeof(Vertex), 0xcbf29ce484222325ULL);
    h = fnv(m.mn, sizeof(m.mn), h);
    h = fnv(m.mx, sizeof(m.mx), h);
    h = fnv(m.lights, (size_t)m.nlights * 16, h);
    h = fnv(m.grass, (size_t)m.ngrass * sizeof(CityGrassTri), h);   /* the grass the blades grow on */
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
    Mesh pr = { 0 };
    city_build_props(cx, cz, &pr);
    h = fnv(pr.v, (size_t)pr.count * sizeof(Vertex), h);
    if (propsOut) *propsOut = pr.count;
    mesh_free(&pr);
    return h;
}

typedef struct Work { int start, reverse; uint64_t out[N_CHUNKS]; } Work;

static int worker(void *arg) {
    Work *w = (Work *)arg;
    /* Each thread walks the list in a different order. */
    for (int k = 0; k < N_CHUNKS; ++k) {
        int j = (w->start + k) % N_CHUNKS;
        int i = w->reverse ? N_CHUNKS - 1 - j : j;
        w->out[i] = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL, NULL, NULL, NULL, NULL);
    }
    return 0;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static double ray_end(PlanArea *pa, DVec2 c, double dx, double dz, int past);

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

/* The grass the blades grow on (Mesh.grass, src/city/grass_city.c), over the
 * chunks round (x, z): each piece lies in its patch and on the ground the
 * chunk draws (its centroid at the height of the green triangle it lies in),
 * none inside a building's site, and together they cover the green ground
 * that grows blades less the sites standing on it; where none stand, all of
 * it. Adds to *area the grass's area (m^2). */
static void check_city_grass(double x, double z, int rad, double *area, int *pieces, int *built) {
    int cx0 = (int)floor(x / CHUNK_SIZE), cz0 = (int)floor(z / CHUNK_SIZE);
    static BlockLayout L[9];
    for (int cz = cz0 - rad; cz <= cz0 + rad; ++cz)
        for (int cx = cx0 - rad; cx <= cx0 + rad; ++cx) {
            Mesh m = { 0 };
            city_build_mesh(cx, cz, &m);
            int nl = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) city_layout(cx + dx, cz + dz, &L[nl++]);
            double ox = cx * (double)CHUNK_SIZE, oz = cz * (double)CHUNK_SIZE, src = 0.0, got = 0.0;
            int sites = 0;
            uint32_t *green = (uint32_t *)malloc(sizeof(uint32_t) * (m.count / 3 + 1)), ng = 0;
            for (uint32_t t = 0; t + 2 < m.count; t += 3) {
                const Vertex *v = &m.v[t];
                if (v[0].params[0] != MAT_LOT || v[0].params[1] != 1 || v[0].params[3] == CITY_GRASS_NONE || v[0].nrm[1] < 32) continue;
                src += 0.5 * fabs((v[1].pos[0] - v[0].pos[0]) * (v[2].pos[2] - v[0].pos[2]) - (v[2].pos[0] - v[0].pos[0]) * (v[1].pos[2] - v[0].pos[2]));
                green[ng++] = t;
            }
            for (int l = 0; l < nl; ++l)
                for (int i = 0; i < L[l].nbuild; ++i) {
                    const Building *b = &L[l].build[i];
                    double reach = 0.5 * CHUNK_SIZE + CITY_OVERHANG;   /* might stand on the chunk's ground */
                    if (fabs(b->ox - (ox + 0.5 * CHUNK_SIZE)) < reach && fabs(b->oz - (oz + 0.5 * CHUNK_SIZE)) < reach) sites++;
                }
            for (uint32_t i = 0; i < m.ngrass; ++i) {
                const CityGrassTri *g = &m.grass[i];
                const float (*p)[3] = g->v;
                double ar = 0.5 * fabs((p[1][0] - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (p[1][2] - p[0][2]));
                got += ar;
                int inPatch = 1;
                for (int q = 0; q < 3; ++q)
                    inPatch &= p[q][0] >= -1e-3f && p[q][0] <= GRASS_PATCH_M + 1e-3f && p[q][2] >= -1e-3f && p[q][2] <= GRASS_PATCH_M + 1e-3f;
                CHECK(inPatch, "chunk (%d,%d) grass piece %u strays out of its patch", cx, cz, i);
                double wx = g->px * GRASS_PATCH_M + (p[0][0] + p[1][0] + p[2][0]) / 3.0;
                double wz = g->pz * GRASS_PATCH_M + (p[0][2] + p[1][2] + p[2][2]) / 3.0;
                double wy = (p[0][1] + p[1][1] + p[2][1]) / 3.0;
                CHECK(g->maintained >= 0.0f && g->maintained <= 1.0f, "chunk (%d,%d) grass piece %u kept %.2f", cx, cz, i, g->maintained);
                for (int l = 0; l < nl; ++l)
                    for (int b = 0; b < L[l].nbuild; ++b)
                        CHECK(!point_in_building(&L[l].build[b], wx, wz, 0.0), "chunk (%d,%d) grass piece %u in a building's site", cx, cz, i);
                /* on the green triangle drawn there, at its height */
                double lx = wx - ox, lz = wz - oz, best = 1e9;
                for (uint32_t gi = 0; gi < ng; ++gi) {
                    const Vertex *v = &m.v[green[gi]];
                    double ax = v[0].pos[0], az = v[0].pos[2], bx = v[1].pos[0], bz = v[1].pos[2], qx = v[2].pos[0], qz = v[2].pos[2];
                    double d = (bz - qz) * (ax - qx) + (qx - bx) * (az - qz);
                    if (fabs(d) < 1e-9) continue;
                    double u = ((bz - qz) * (lx - qx) + (qx - bx) * (lz - qz)) / d, w = ((qz - az) * (lx - qx) + (ax - qx) * (lz - qz)) / d;
                    if (u < -1e-4 || w < -1e-4 || u + w > 1.0 + 1e-4) continue;
                    double y = u * v[0].pos[1] + w * v[1].pos[1] + (1.0 - u - w) * v[2].pos[1];
                    if (fabs(y - wy) < fabs(best)) best = y - wy;
                }
                CHECK(fabs(best) < 2e-3, "chunk (%d,%d) grass piece %u at (%.2f, %.2f) stands %.3f m off the green ground", cx, cz, i, wx, wz, best);
            }
            CHECK(got <= src * (1.0 + 1e-4) + 1e-3, "chunk (%d,%d) grass covers %.1f m2 of %.1f m2 green", cx, cz, got, src);
            if (!sites) CHECK(fabs(got - src) <= src * 1e-4 + 1e-2, "chunk (%d,%d) with no buildings: grass %.2f m2 of %.2f m2 green", cx, cz, got, src);
            else (*built)++;
            *area += got;
            *pieces += (int)m.ngrass;
            free(green);
            mesh_free(&m);
        }
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

enum { FP_MESH, FP_LIGHTS, FP_SKY, FP_AIR, FP_INTERIORS, FP_PLANTS, FP_LITTER, FP_LAYOUT, FP_PROPS, FP_GRASS, FP_COUNT };
static const char *const FP_NAMES[FP_COUNT] = { "mesh", "lights", "sky", "air", "interiors", "plants", "litter", "layout", "props", "grass" };

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
    if (L->bounded) {   /* only where the city ends, so the chunks inside it hash as they always did */
        FNV_FIELD(h, L->nground);
        for (int i = 0; i < L->nground; ++i) {
            FNV_FIELD(h, L->ground[i].n);
            h = fnv(L->ground[i].v, sizeof(L->ground[i].v[0]) * (size_t)L->ground[i].n, h);
        }
    }
    FNV_FIELD(h, L->ncoll); FNV_FIELD(h, L->ncollv);
    h = fnv(L->coll, sizeof(L->coll[0]) * (size_t)L->ncoll, h);
    h = fnv(L->collv, sizeof(L->collv[0]) * (size_t)L->ncollv, h);
    FNV_FIELD(h, L->nfloor);
    h = fnv(L->floor, sizeof(L->floor[0]) * (size_t)L->nfloor, h);
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
    fp[FP_GRASS] = fnv(&m.ngrass, sizeof(m.ngrass), fp[FP_GRASS]);
    fp[FP_GRASS] = fnv(m.grass, (size_t)m.ngrass * sizeof(CityGrassTri), fp[FP_GRASS]);
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
    city_build_props(cx, cz, &m);
    fp[FP_PROPS] = fnv_mesh(&m, fp[FP_PROPS]);
    mesh_free(&m);
    city_layout(cx, cz, &L);
    fp[FP_LAYOUT] = fnv_layout(&L, fp[FP_LAYOUT]);
}

static uint64_t fnv_tile(const ProxyTile *t, uint64_t h);

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
    uint64_t afterHome = fnv(fp, sizeof(fp), 0xcbf29ce484222325ULL);

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
    uint64_t afterMega = fnv(fp, sizeof(fp), 0xcbf29ce484222325ULL);

    for (int i = 0; i < 100; ++i) { fingerprint_chunk(g_cx[i], g_cz[i], fp); n++; }

    printf("fingerprint over %d chunks (arcology chunk %s (%d, %d))\n", n, found ? "at" : "not found near", megaCx, megaCz);
    for (int k = 0; k < FP_COUNT; ++k) printf("  %-10s %016llx\n", FP_NAMES[k], (unsigned long long)fp[k]);
    /* Every layer folded together after the 9 x 9, then after the arcology's 3 x 3: a change
     * that only reaches the scattered chunks leaves both alone. */
    printf("  %-10s %016llx\n  %-10s %016llx\n", "home", (unsigned long long)afterHome, "+arcology", (unsigned long long)afterMega);
    /* The proxies of the 3 x 3 tiles round the home town's centre, apart from the layers above. */
    ProxyTile pt = { 0 };
    uint64_t px = 0xcbf29ce484222325ULL;
    int htx = (int)floor(home.x / PROXY_TILE), htz = (int)floor(home.z / PROXY_TILE);
    for (int tz = htz - 1; tz <= htz + 1; ++tz)
        for (int tx = htx - 1; tx <= htx + 1; ++tx) { city_proxy_tile(tx, tz, &pt); px = fnv_tile(&pt, px); }
    proxy_tile_free(&pt);
    printf("  %-10s %016llx\n", "proxies", (unsigned long long)px);
    /* The city's edge, apart from the layers above: every layer of the 3 x 3
     * chunks where the city ends along 8 rays from the centre, folded together. */
    uint64_t efp[FP_COUNT];
    for (int k = 0; k < FP_COUNT; ++k) efp[k] = 0xcbf29ce484222325ULL;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory fingerprinting the edge");
    for (int ray = 0; ray < 8; ++ray) {
        double th = (ray + 0.25) * 2.0 * 3.14159265358979 / 8.0, dx = cos(th), dz = sin(th), rad = ray_end(pa, home, dx, dz, 1);
        int ex = (int)floor((home.x + (rad - 150.0) * dx) / CHUNK_SIZE), ez = (int)floor((home.z + (rad - 150.0) * dz) / CHUNK_SIZE);
        for (int cz = ez - 1; cz <= ez + 1; ++cz)
            for (int cx = ex - 1; cx <= ex + 1; ++cx) fingerprint_chunk(cx, cz, efp);
    }
    free(pa);
    printf("  %-10s %016llx\n", "edge", (unsigned long long)fnv(efp, sizeof(efp), 0xcbf29ce484222325ULL));
    return 0;
}

/* How far along the ray from c (unit dx, dz) the plan first reaches the
 * city's outline (pa->bounded), and with `past` the last block too (no
 * blocks either), in 100 m steps from a little before the edge measure first
 * passes -HOME_CORE (deep inside, the plan is unbounded); the megacity's
 * reach and more if it never does. pa holds the plan there. */
#define EDGE_NEAR (CITY_BELT + CITY_FRINGE + 2.0 * TOWN_SIZE)
static double ray_end(PlanArea *pa, DVec2 c, double dx, double dz, int past) {
    double far = dlen(c) + plan_home_reach() + 3000.0, rad = 0.0;
    while (rad < far && plan_city_edge(c.x + rad * dx, c.z + rad * dz) < -EDGE_NEAR) rad += 500.0;
    for (rad = fmax(0.0, rad - 1000.0); rad < far; rad += 100.0) {
        plan_area(c.x + rad * dx - 1.0, c.z + rad * dz - 1.0, c.x + rad * dx + 1.0, c.z + rad * dz + 1.0, pa);
        if (pa->bounded && (!past || pa->nblocks == 0)) break;
    }
    return rad;
}

/* ---------------------------------------------------------------------------
 * The city's edge: the city is its built towns, all where plan_urban is 1;
 * its ground is the union of their grounds, which don't overlap, inside a
 * closed outline; everything the plan lays out lies on that ground; the
 * perimeter boulevards are drawn; and beyond the city there is nothing.
 * ------------------------------------------------------------------------- */

static int on_ground(const PlanArea *pa, double x, double z, double margin) {
    if (!pa->bounded) return 1;
    for (int t = 0; t < pa->ntowns; ++t)
        if (poly_contains(&pa->ground[t], x, z, margin)) return 1;
    return 0;
}

/* Checks the plan round one point near the edge; returns the outline pieces it holds. */
static int check_edge_area(PlanArea *pa, double x, double z) {
    const double H = 150.0;
    plan_area(x - H, z - H, x + H, z + H, pa);
    CHECK(pa->bounded, "the plan at (%.0f, %.0f), where the city ends, is not bounded", x, z);
    for (int t = 0; t < pa->ntowns; ++t)
        for (int k = 0; k < pa->ground[t].n; ++k) {
            DVec2 v = pa->ground[t].v[k];
            CHECK(plan_urban(v.x, v.z) == 1.0f, "ground corner (%.1f, %.1f) is where plan_urban is %.3f", v.x, v.z, plan_urban(v.x, v.z));
        }
    /* No point is on two towns' ground. */
    for (double sz = z - H; sz <= z + H; sz += 7.0)
        for (double sx = x - H; sx <= x + H; sx += 7.0) {
            int n = 0;
            for (int t = 0; t < pa->ntowns; ++t) n += poly_contains(&pa->ground[t], sx, sz, -0.01);
            CHECK(n <= 1, "(%.1f, %.1f) lies on %d towns' ground", sx, sz, n);
        }
    /* Roads and blocks lie on the ground, within the area (ground[] covers no more):
     * both sides of each carriageway between its trims, and each kerb, every 2 m. */
    for (int i = 0; i < pa->nroads; ++i) {
        const Road *r = &pa->roads[i];
        DVec2 d = dsub(r->b, r->a);
        double len = dlen(d);
        if (len < 1.0) continue;
        DVec2 u = dscale(d, 1.0 / len), nrm = dv(-u.z, u.x);
        for (double t = r->trimA; t <= len - r->trimB; t += 2.0)
            for (int side = -1; side <= 1; side += 2) {
                DVec2 p = dadd(dadd(r->a, dscale(u, t)), dscale(nrm, side * r->at.half));
                if (p.x < x - H || p.x > x + H || p.z < z - H || p.z > z + H) continue;
                CHECK(on_ground(pa, p.x, p.z, 0.01), "road %08x's carriageway at (%.1f, %.1f) is off the city's ground", r->at.id, p.x, p.z);
            }
    }
    for (int i = 0; i < pa->nblocks; ++i) {
        const Poly *cb = &pa->blocks[i].curb;
        for (int k = 0; k < cb->n; ++k) {
            DVec2 a = cb->v[k], b = cb->v[(k + 1) % cb->n];
            double len = dlen(dsub(b, a));
            for (double t = 0.0; t <= len; t += 2.0) {
                DVec2 p = dadd(a, dscale(dsub(b, a), len > 0.0 ? t / len : 0.0));
                if (p.x < x - H || p.x > x + H || p.z < z - H || p.z > z + H) continue;
                CHECK(on_ground(pa, p.x, p.z, 0.01), "block %d's kerb at (%.1f, %.1f) is off the city's ground", i, p.x, p.z);
            }
        }
    }
    for (int i = 0; i < pa->nedges; ++i) {
        const PlanEdge *e = &pa->edges[i];
        /* The outline is closed: inside the area every end meets another piece's other end. */
        for (int end = 0; end < 2; ++end) {
            DVec2 p = end ? e->b : e->a;
            if (p.x < x - H + 1.0 || p.x > x + H - 1.0 || p.z < z - H + 1.0 || p.z > z + H - 1.0) continue;
            int met = 0;
            for (int j = 0; j < pa->nedges && !met; ++j) {
                DVec2 q = end ? pa->edges[j].a : pa->edges[j].b;
                met = j != i && dlen(dsub(p, q)) < 1e-4;
            }
            CHECK(met, "the city's outline is open at (%.2f, %.2f)", p.x, p.z);
        }
        /* The ground on its inner side, the land on its outer. */
        double len = dlen(dsub(e->b, e->a));
        DVec2 m = dscale(dadd(e->a, e->b), 0.5);
        if (len > 1.0) {
            CHECK(on_ground(pa, m.x - e->n.x * 0.2, m.z - e->n.z * 0.2, 0.0), "inside the outline at (%.1f, %.1f) is off the ground", m.x, m.z);
            CHECK(!on_ground(pa, m.x + e->n.x * 0.2, m.z + e->n.z * 0.2, 0.0), "outside the outline at (%.1f, %.1f) is on the ground", m.x, m.z);
        }
        /* A long piece runs along a perimeter boulevard, a shoulder beyond its carriageway. */
        if (len > 60.0 && m.x > x - H && m.x < x + H && m.z > z - H && m.z < z + H) {
            int found = 0;
            for (int r = 0; r < pa->nroads && !found; ++r) {
                const Road *rd = &pa->roads[r];
                if (rd->at.cls == ROAD_BOULEVARD) found = fabs(seg_dist(m, rd->a, rd->b) - (rd->at.half + CITY_SHOULDER)) < 0.01;
            }
            CHECK(found, "no perimeter boulevard along the city's edge at (%.1f, %.1f)", m.x, m.z);
        }
    }
    return pa->nedges;
}

/* The towns within `half` of town (ci, cj) each way, each built one built over
 * nothing but plan_urban 1; counts the built and the unbuilt. */
static void survey_towns(int ci, int cj, int half, int *built, int *unbuilt) {
    for (int j = cj - half; j <= cj + half; ++j)
        for (int i = ci - half; i <= ci + half; ++i) {
            if (!plan_town_built(i, j)) { (*unbuilt)++; continue; }
            (*built)++;
            DVec2 c = plan_town_centre(i, j);
            for (double dz = -1500.0; dz <= 1500.0; dz += 50.0)
                for (double dx = -1500.0; dx <= 1500.0; dx += 50.0) {
                    double x = c.x + dx, z = c.z + dz, best = hypot(dx, dz);
                    int mine = 1;   /* in (i, j)'s cell: no other site nearer */
                    for (int q = 0; q < 9 && mine; ++q) {
                        int oi = (int)floor(x / TOWN_SIZE) + q % 3 - 1, oj = (int)floor(z / TOWN_SIZE) + q / 3 - 1;
                        if (oi != i || oj != j) mine = dlen(dsub(dv(x, z), plan_town_centre(oi, oj))) >= best;
                    }
                    if (mine) CHECK(plan_urban(x, z) == 1.0f, "town (%d,%d) is built over (%.0f, %.0f), where plan_urban is %.3f", i, j, x, z, plan_urban(x, z));
                }
        }
}

static void check_edge(void) {
    /* The towns of the city, each built over nothing but plan_urban 1: round
     * its core, and across its edge along 8 rays. */
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory checking the city's edge");
    DVec2 c = plan_town_centre(0, 0);
    int built = 0, unbuilt = 0, coreBuilt = 0, coreUnbuilt = 0;
    survey_towns(0, 0, 8, &coreBuilt, &coreUnbuilt);
    for (int ray = 0; ray < 8; ++ray) {
        double th = (ray + 0.5) * 2.0 * 3.14159265358979 / 8.0, dx = cos(th), dz = sin(th), rad = ray_end(pa, c, dx, dz, 0);
        int ti, tj;
        plan_town_at(c.x + rad * dx, c.z + rad * dz, &ti, &tj);
        survey_towns(ti, tj, 3, &built, &unbuilt);
    }
    CHECK(plan_town_built(0, 0), "the home town is not built");
    CHECK(coreBuilt >= 40, "the city has only %d towns round its core", coreBuilt);
    CHECK(built > 0 && unbuilt > 0, "the search for built towns across the edge finds %d built, %d not", built, unbuilt);
    printf("the city: %d of %d towns built round its core; across its edge %d built, %d not\n", coreBuilt, coreBuilt + coreUnbuilt, built, unbuilt);

    /* Along rays from the centre, the plan where the city ends. */
    int areas = 0, edges = 0, rural = 0, blocks = 0;
    double far = dlen(c) + plan_home_reach() + 3000.0;
    for (int ray = 0; ray < 48; ++ray) {
        double th = ray * 2.0 * 3.14159265358979 / 48.0, dx = cos(th), dz = sin(th), rad = ray_end(pa, c, dx, dz, 0);
        CHECK(rad < far, "the city doesn't end along %.1f deg", th * 57.2958);
        edges += check_edge_area(pa, c.x + rad * dx, c.z + rad * dz);
        areas++;
        for (int b = 0; b < pa->nblocks; ++b) { blocks++; rural += pa->blocks[b].zone == ZONE_RURAL; }
    }
    printf("  %d areas where it ends: %d outline pieces, %d blocks (%d rural)\n", areas, edges, blocks, rural);
    CHECK(edges >= areas, "too little of the city's outline found");
    /* Beyond the city: nothing at all. */
    plan_area(c.x + far, c.z, c.x + far + 300.0, c.z + 300.0, pa);
    CHECK(pa->bounded && pa->nroads == 0 && pa->nblocks == 0 && pa->ntowns == 0 && pa->nedges == 0,
          "%.0f km out the plan still lays out %d roads, %d blocks, %d towns' ground", far / 1000.0, pa->nroads, pa->nblocks, pa->ntowns);
    free(pa);
}

/* The skyline steps down towards the edge rather than ending in a cliff.
 * Along rays from the centre, the tallest building in 600 m squares centred
 * every 600 m in from where the city ends, averaged over the rays: no band
 * may stand more than a margin taller than the band inside it, and the band
 * at the edge stays well under the city's inner bands. */
static void check_skyline(void) {
    enum { RAYS = 24, BANDS = 8, MAX_B = 4096 };
    DVec2 c = plan_town_centre(0, 0);
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bs = (Building *)malloc(sizeof(Building) * MAX_B);
    if (!pa || !bs) fatal("out of memory checking the skyline");
    double tall[BANDS] = { 0 };
    for (int ray = 0; ray < RAYS; ++ray) {
        double th = (ray + 0.5) * 2.0 * 3.14159265358979 / RAYS, dx = cos(th), dz = sin(th), rad = ray_end(pa, c, dx, dz, 1);   /* past the last block */
        for (int k = 0; k < BANDS; ++k) {
            double r = rad - 300.0 - 600.0 * k, x = c.x + r * dx, z = c.z + r * dz, top = 0.0;
            plan_area(x - 300.0, z - 300.0, x + 300.0, z + 300.0, pa);
            for (int b = 0; b < pa->nblocks; ++b) {
                int n = city_block_buildings(&pa->blocks[b], bs, MAX_B);
                for (int i = 0; i < n; ++i) top = fmax(top, bs[i].height);
            }
            tall[k] += top / RAYS;
        }
    }
    printf("  skyline in from the edge, tallest per 600 m (mean of %d rays):", RAYS);
    for (int k = 0; k < BANDS; ++k) printf(" %.0f", tall[k]);
    printf(" m\n");
    for (int k = 0; k + 1 < BANDS; ++k)
        CHECK(tall[k] <= tall[k + 1] * 1.15 + 10.0, "the skyline rises towards the edge: %.0f m at band %d, %.0f m inside it", tall[k], k, tall[k + 1]);
    CHECK(tall[0] < 0.4 * tall[BANDS - 1], "the edge's skyline (%.0f m) is no lower than %.1f km in (%.0f m)", tall[0], 0.6 * (BANDS - 1), tall[BANDS - 1]);
    free(bs);
    free(pa);
}

/* ---------------------------------------------------------------------------
 * Distant proxies: a tile holds exactly the buildings city_layout puts in each
 * of its chunks, every box inside its building's plot and under its chunk's
 * culling bound; a tile comes out the same whenever it is generated.
 * ------------------------------------------------------------------------- */

static float h2f(uint32_t h) {
    uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 1023u;
    union { uint32_t u; float f; } v;
    v.u = e == 0 ? s : s | (e - 15 + 127) << 23 | m << 13;
    return v.f;
}

static uint64_t fnv_tile(const ProxyTile *t, uint64_t h) {
    h = fnv(&t->n, sizeof(t->n), h);
    h = fnv(t->box, sizeof(ProxyBox) * (size_t)t->n, h);
    h = fnv(t->lod, sizeof(t->lod), h);
    return fnv(t->first, sizeof(t->first), h);
}

static void check_proxy_tile(int tx, int tz, ProxyTile *t, int *boxes, int *builds) {
    static BlockLayout L;
    city_proxy_tile(tx, tz, t);
    const double ox = tx * PROXY_TILE, oz = tz * PROXY_TILE;
    int found = 0;
    for (int k = 0; k < PROXY_TILE_CELLS; ++k) {
        int cx = tx * PROXY_TILE_CHUNKS + k % PROXY_TILE_CHUNKS, cz = tz * PROXY_TILE_CHUNKS + k / PROXY_TILE_CHUNKS;
        city_layout(cx, cz, &L);
        float g0, g1;
        city_ground_range(cx, cz, &g0, &g1);
        for (int i = 0; i < L.nbuild; ++i) {
            int has = 0;
            for (int j = t->first[k]; j < t->first[k + 1] && !has; ++j) has = t->box[j].seed == L.build[i].seed;
            CHECK(has, "chunk (%d,%d) building %08x has no proxy", cx, cz, L.build[i].seed);
            found += has;
        }
        for (int j = t->first[k]; j < t->first[k + 1]; ++j) {
            const ProxyBox *p = &t->box[j];
            const Building *b = NULL;
            for (int i = 0; i < L.nbuild && !b; ++i) if (L.build[i].seed == p->seed) b = &L.build[i];
            CHECK(b, "proxy %d of tile (%d,%d) belongs to no building of chunk (%d,%d)", j, tx, tz, cx, cz);
            CHECK((int)((p->info >> PX_CHUNK_SHIFT) & 15u) == k, "proxy %d of tile (%d,%d) is labelled with the wrong chunk", j, tx, tz);
            if (!b) continue;
            float hx = h2f(p->half & 0xFFFFu), hz = h2f(p->half >> 16), c = h2f(p->axis & 0xFFFFu), s = h2f(p->axis >> 16);
            CHECK(fabsf(c - b->c) < 1e-3f && fabsf(s - b->s) < 1e-3f, "proxy %d of building %08x is turned from it", j, b->seed);
            double dx = ox + p->x - b->ox, dz = oz + p->z - b->oz;
            double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
            CHECK(lx - hx >= b->foot.x0 - 0.05 && lx + hx <= b->foot.x1 + 0.05 && lz - hz >= b->foot.z0 - 0.05 && lz + hz <= b->foot.z1 + 0.05,
                  "proxy %d of building %08x reaches out of its plot", j, b->seed);
            CHECK(p->y0 < p->y1 && p->y0 >= g0 - 1.5f && p->y1 <= g1 + city_height_cap(cx, cz),
                  "proxy %d of building %08x (%.1f..%.1f m) is outside chunk (%d,%d)'s culling bounds", j, b->seed, p->y0, p->y1, cx, cz);
            CHECK(p->x >= t->mn[0] && p->x <= t->mx[0] && p->z >= t->mn[2] && p->z <= t->mx[2] && p->y0 >= t->mn[1] && p->y1 <= t->mx[1],
                  "proxy %d of tile (%d,%d) is outside the tile's bounds", j, tx, tz);
        }
        /* level of detail: largest first, and the bins count them */
        for (int b = 0; b < PROXY_LOD_BINS; ++b) {
            int n = 0;
            for (int j = t->first[k]; j < t->first[k + 1]; ++j) n += proxy_box_size(&t->box[j]) >= PROXY_LOD_SIZE(b);
            CHECK(n == t->lod[k][b], "tile (%d,%d) chunk %d counts %d boxes of %.0f m or more, holds %d", tx, tz, k, t->lod[k][b], PROXY_LOD_SIZE(b), n);
        }
        for (int j = t->first[k] + 1; j < t->first[k + 1]; ++j)
            CHECK(proxy_box_size(&t->box[j]) <= proxy_box_size(&t->box[j - 1]), "tile (%d,%d) chunk %d's boxes are not largest first", tx, tz, k);
    }
    CHECK(found == t->nbuild, "tile (%d,%d) holds %d buildings, its chunks %d", tx, tz, t->nbuild, found);
    *boxes += t->n;
    *builds += t->nbuild;
}

static void check_proxies(void) {
    DVec2 home = plan_town_centre(0, 0);
    int htx = (int)floor(home.x / PROXY_TILE), htz = (int)floor(home.z / PROXY_TILE);
    ProxyTile t = { 0 }, again = { 0 };
    int tiles = 0, boxes = 0, builds = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) { check_proxy_tile(htx + dx, htz + dz, &t, &boxes, &builds); tiles++; }
    /* outwards across the fringe and the city's edge */
    for (double r = 4000.0; r <= 13000.0; r += 1500.0) {
        check_proxy_tile((int)floor((home.x - r * 0.6) / PROXY_TILE), (int)floor((home.z + r * 0.8) / PROXY_TILE), &t, &boxes, &builds);
        tiles++;
    }
    /* tiles called empty hold nothing: across the edge along 16 rays */
    int empties = 0;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory checking the proxies");
    for (int ray = 0; ray < 16; ++ray) {
        double th = ray * 2.0 * 3.14159265358979 / 16.0, end = ray_end(pa, home, cos(th), sin(th), 1);
        for (double r = end - 2000.0; r <= end + 6000.0; r += PROXY_TILE) {
            int tx = (int)floor((home.x + r * cos(th)) / PROXY_TILE), tz = (int)floor((home.z + r * sin(th)) / PROXY_TILE);
            if (!city_proxy_tile_empty(tx, tz)) continue;
            city_proxy_tile(tx, tz, &t);
            CHECK(t.n == 0, "tile (%d,%d) is called empty but holds %d boxes", tx, tz, t.n);
            empties++;
        }
    }
    free(pa);
    CHECK(empties > 16, "only %d tiles across the edge are called empty", empties);
    /* the same tile after others, and whichever way it is reached */
    city_proxy_tile(htx, htz, &t);
    uint64_t h = fnv_tile(&t, 0xcbf29ce484222325ULL);
    city_proxy_tile(htx + 5, htz - 3, &again);
    city_proxy_tile(htx, htz, &again);
    CHECK(fnv_tile(&again, 0xcbf29ce484222325ULL) == h, "proxy tile (%d,%d) differs when generated again", htx, htz);
    printf("proxies: %d tiles, %d buildings, %d boxes (%.1f a building)\n", tiles, builds, boxes, builds ? (double)boxes / builds : 0.0);
    CHECK(builds > tiles * 40, "too few buildings in the proxy tiles");
    proxy_tile_free(&t);
    proxy_tile_free(&again);
}

/* city_test --proxies: every tile of the city, on THREADS threads, for the
 * proxies' totals, the largest tile and the time they take. */
typedef struct ProxyWork { int tx0, tz0, nx, nz, k; int tiles, builds, boxes, maxBoxes; } ProxyWork;

static int proxy_worker(void *arg) {
    ProxyWork *w = (ProxyWork *)arg;
    ProxyTile t = { 0 };
    for (int i = w->k; i < w->nx * w->nz; i += THREADS) {
        city_proxy_tile(w->tx0 + i % w->nx, w->tz0 + i / w->nx, &t);
        if (t.n == 0) continue;
        w->tiles++;
        w->builds += t.nbuild;
        w->boxes += t.n;
        if (t.n > w->maxBoxes) w->maxBoxes = t.n;
    }
    proxy_tile_free(&t);
    return 0;
}

static int proxy_survey(void) {
    DVec2 home = plan_town_centre(0, 0);
    const double R = dlen(home) + plan_home_reach();
    static ProxyWork w[THREADS];
    Thread *th[THREADS];
    clock_t t0 = clock();
    for (int k = 0; k < THREADS; ++k) {
        w[k].tx0 = (int)floor((home.x - R) / PROXY_TILE); w[k].tz0 = (int)floor((home.z - R) / PROXY_TILE);
        w[k].nx = w[k].nz = (int)ceil(2.0 * R / PROXY_TILE) + 1;
        w[k].k = k;
        th[k] = thread_create(proxy_worker, &w[k]);
    }
    int tiles = 0, builds = 0, boxes = 0, maxBoxes = 0;
    for (int k = 0; k < THREADS; ++k) {
        thread_join(th[k]);
        tiles += w[k].tiles; builds += w[k].builds; boxes += w[k].boxes;
        if (w[k].maxBoxes > maxBoxes) maxBoxes = w[k].maxBoxes;
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("the city's proxies: %d tiles of %.0f m (of %d searched), %d buildings, %d boxes (%.1f MB), at most %d in a tile; %.1f s on %d threads\n",
           tiles, PROXY_TILE, w[0].nx * w[0].nz, builds, boxes, boxes * (double)sizeof(ProxyBox) / 1048576.0, maxBoxes, secs, THREADS);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Street furniture (src/city/props.c) round the spawn: what stands on the
 * ground stands on a block, clear of every other piece; a crossing's signal
 * faces the road; and nothing closes a way through: in front of every
 * dumpster a walker has room to pass, whatever stands nearby. Also how much
 * of it there is.
 * ------------------------------------------------------------------------- */

/* Whether (x, z) lies in a convex outline, either winding. */
static int in_outline(const double (*v)[2], int n, double x, double z) {
    int pos = 0, neg = 0;
    for (int i = 0; i < n; ++i) {
        const double *a = v[i], *b = v[(i + 1) % n];
        double cr = (b[0] - a[0]) * (z - a[1]) - (b[1] - a[1]) * (x - a[0]);
        if (cr > 0.0) pos = 1; else if (cr < 0.0) neg = 1;
    }
    return n >= 3 && !(pos && neg);
}

static void check_props(int hx, int hz) {
    enum { R = 6, W = 2 * R + 1 };
    static Prop pr[W][W][MAX_PROPS];
    static int np[W][W];
    int kinds[PROP_KINDS] = { 0 }, total = 0;
    for (int j = 0; j < W; ++j)
        for (int i = 0; i < W; ++i) {
            np[j][i] = city_chunk_props(hx - R + i, hz - R + j, pr[j][i], MAX_PROPS);
            for (int k = 0; k < np[j][i]; ++k) { kinds[pr[j][i][k].kind]++; total++; }
        }
    int overlaps = 0, closed = 0, faced = 0;
    for (int j = 1; j < W - 1; ++j)
        for (int i = 1; i < W - 1; ++i) {
            int cx = hx - R + i, cz = hz - R + j;
            static BlockLayout L;
            city_layout(cx, cz, &L);
            for (int k = 0; k < np[j][i]; ++k) {
                const Prop *p = &pr[j][i][k];
                if (p->kind == PROP_AIRCON) continue;
                double v[4][2];
                int n = prop_outline(p, v);
                int on = 0;
                for (int c = 0; c < L.ncurbs && !on; ++c) on = in_curb(&L.curb[c], p->x, p->z);
                CHECK(on, "chunk (%d,%d): piece %d (kind %d) stands off its block", cx, cz, k, p->kind);
                if (p->kind == PROP_CROSSING) {
                    int road = 1;
                    for (int c = 0; c < L.ncurbs && road; ++c) road = !in_curb(&L.curb[c], p->x + p->fx * 1.2, p->z + p->fz * 1.2);
                    faced += !road;
                }
                /* every other piece in the 3 x 3 chunks round it */
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        for (int q = 0; q < np[j + dj][i + di]; ++q) {
                            const Prop *o = &pr[j + dj][i + di][q];
                            if (o == p || o->kind == PROP_AIRCON) continue;
                            double dx = o->x - p->x, dz = o->z - p->z;
                            if (dx * dx + dz * dz > 25.0) continue;
                            double w[4][2];
                            int m = prop_outline(o, w);
                            int hit = in_outline((const double (*)[2])v, n, o->x, o->z);
                            for (int c = 0; c < m && !hit; ++c) hit = in_outline((const double (*)[2])v, n, w[c][0], w[c][1]);
                            overlaps += hit;
                            CHECK(!hit, "chunk (%d,%d): pieces %d and (%+d,%+d) %d overlap", cx, cz, k, di, dj, q);
                            if (p->kind != PROP_DUMPSTER) continue;
                            /* the way past it: across a walker's width and more, the first 1.2 m out from its front */
                            double hd = 0.5 * sqrt((v[3][0] - v[0][0]) * (v[3][0] - v[0][0]) + (v[3][1] - v[0][1]) * (v[3][1] - v[0][1]));
                            int shut = 0;
                            for (int s = 0; s <= 12 && !shut; ++s)
                                for (int a = -2; a <= 2 && !shut; ++a) {
                                    double e = hd + 0.05 + s * 0.1, f = a * 0.3;
                                    shut = in_outline((const double (*)[2])w, m, p->x + p->fx * e + p->fz * f, p->z + p->fz * e - p->fx * f);
                                }
                            closed += shut;
                            CHECK(!shut, "chunk (%d,%d): dumpster %d is closed in by piece (%+d,%+d) %d", cx, cz, k, di, dj, q);
                        }
            }
        }
    printf("street furniture round the spawn (%d x %d chunks): %d pieces: %d bins, %d bollards, %d post boxes, %d crossings, %d dumpsters, %d condensers\n",
           W, W, total, kinds[PROP_BIN], kinds[PROP_BOLLARD], kinds[PROP_MAILBOX], kinds[PROP_CROSSING], kinds[PROP_DUMPSTER], kinds[PROP_AIRCON]);
    printf("  %d overlaps, %d dumpsters closed in, %d signals facing a block (the middle %d x %d chunks)\n", overlaps, closed, faced, W - 2, W - 2);
    int middle = (W - 2) * (W - 2);
    CHECK(kinds[PROP_BIN] > middle, "too few litter bins round the spawn");
    CHECK(kinds[PROP_CROSSING] > middle / 2, "too few crossing signals round the spawn");
    CHECK(kinds[PROP_DUMPSTER] > 0 && kinds[PROP_AIRCON] > 0 && kinds[PROP_BOLLARD] > 0 && kinds[PROP_MAILBOX] > 0, "a kind of street furniture is missing round the spawn");
    CHECK(faced == 0, "%d crossing signals face a block, not the road", faced);
}

#include "city_test_vendors.c"
#include "city_test_slice.c"

/* city_test --props: --cam views of the street furniture nearest the spawn, a
 * few of each kind, from in front of each piece (a condenser's from the
 * ground below, looking up at it). */
static int props_cams(void) {
    static const char *const NAMES[PROP_KINDS] = { "bollard", "bin", "post box", "dumpster", "crossing", "condenser" };
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    int hx = (int)floor(sx / CHUNK_SIZE), hz = (int)floor(sz / CHUNK_SIZE), shown[PROP_KINDS] = { 0 };
    static Prop pr[MAX_PROPS];
    for (int ring = 0; ring <= 3; ++ring)
        for (int cz = hz - ring; cz <= hz + ring; ++cz)
            for (int cx = hx - ring; cx <= hx + ring; ++cx) {
                if (abs(cx - hx) != ring && abs(cz - hz) != ring) continue;
                int n = city_chunk_props(cx, cz, pr, MAX_PROPS);
                for (int i = 0; i < n; ++i) {
                    const Prop *p = &pr[i];
                    if (shown[p->kind] >= 3) continue;
                    shown[p->kind]++;
                    double d = p->kind == PROP_AIRCON ? 4.0 : p->kind == PROP_CROSSING ? 4.5 : 3.0, side = p->kind == PROP_CROSSING ? 1.5 : 0.8;
                    double x = p->x + p->fx * d + p->fz * side, z = p->z + p->fz * d - p->fx * side;
                    double tx = p->x - x, tz = p->z - z;
                    float pitch = p->kind == PROP_AIRCON ? (float)(atan2(p->y - (city_ground_y(x, z) + 1.7), sqrt(tx * tx + tz * tz)) * 57.2958) : -12.0f;
                    printf("%-9s at (%.1f, %.1f) y %.1f: --cam %.2f %.2f %.1f %.1f\n", NAMES[p->kind], p->x, p->z, p->y, x, z,
                           atan2(tx, -tz) * 57.2958, pitch);
                }
            }
    return 0;
}

/* city_test --zfight X Z R: z-fighting round world (X, Z). Every layer of the
 * chunks within R m (base, interiors, props, plants, litter) is gathered, and
 * each pair of triangles that face the same way, lie within ZF_GAP of one plane
 * and overlap is a fight. They are grouped by the two surfaces' materials
 * (vertex params: material / y / z / w), each group with a --cam view of one.
 * Back-to-back faces are left out: culling keeps them apart. */
#define ZF_GAP 0.002f
typedef struct ZfTri { float p[3][3], n[3], d, mn[3], mx[3]; uint8_t prm[4]; } ZfTri;
typedef struct ZfGroup { uint8_t a[4], b[4]; int count; float at[3], n[3]; } ZfGroup;

/* Is p (on t's plane) strictly inside triangle t? */
static bool zf_inside(const ZfTri *t, const float *p) {
    float s[3];
    for (int i = 0; i < 3; ++i) {
        const float *a = t->p[i], *b = t->p[(i + 1) % 3];
        float e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, q[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
        s[i] = (e[1] * q[2] - e[2] * q[1]) * t->n[0] + (e[2] * q[0] - e[0] * q[2]) * t->n[1] + (e[0] * q[1] - e[1] * q[0]) * t->n[2];
    }
    return (s[0] > 1e-6f && s[1] > 1e-6f && s[2] > 1e-6f) || (s[0] < -1e-6f && s[1] < -1e-6f && s[2] < -1e-6f);
}

/* Do coplanar triangles a and b overlap in area? (Their centroids, or points just in from their corners, inside the other.) */
static bool zf_overlap(const ZfTri *a, const ZfTri *b) {
    for (int pass = 0; pass < 2; ++pass) {
        const ZfTri *s = pass ? b : a, *o = pass ? a : b;
        float c[3];
        for (int d = 0; d < 3; ++d) c[d] = (s->p[0][d] + s->p[1][d] + s->p[2][d]) / 3.0f;
        if (zf_inside(o, c)) return true;
        for (int k = 0; k < 3; ++k) {
            float q[3];
            for (int d = 0; d < 3; ++d) q[d] = c[d] + 0.9f * (s->p[k][d] - c[d]);
            if (zf_inside(o, q)) return true;
        }
    }
    return false;
}

/* Appends m's triangles near (X, Z), in metres from it, to *tris. */
static void zf_gather(const Mesh *m, double X, double Z, double R, ZfTri **tris, int *n, int *cap) {
    for (uint32_t i = 0; i + 2 < m->count; i += 3) {
        ZfTri t;
        const Vertex *v = &m->v[i];
        for (int k = 0; k < 3; ++k) {
            t.p[k][0] = (float)(m->ox + v[k].pos[0] - X); t.p[k][1] = v[k].pos[1]; t.p[k][2] = (float)(m->oz + v[k].pos[2] - Z);
        }
        for (int d = 0; d < 3; ++d) {
            t.mn[d] = fminf(t.p[0][d], fminf(t.p[1][d], t.p[2][d]));
            t.mx[d] = fmaxf(t.p[0][d], fmaxf(t.p[1][d], t.p[2][d]));
        }
        if (t.mn[0] > R || t.mx[0] < -R || t.mn[2] > R || t.mx[2] < -R) continue;
        float e1[3], e2[3];
        for (int d = 0; d < 3; ++d) { e1[d] = t.p[1][d] - t.p[0][d]; e2[d] = t.p[2][d] - t.p[0][d]; }
        t.n[0] = e1[1] * e2[2] - e1[2] * e2[1]; t.n[1] = e1[2] * e2[0] - e1[0] * e2[2]; t.n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        float L = sqrtf(t.n[0] * t.n[0] + t.n[1] * t.n[1] + t.n[2] * t.n[2]);
        if (L < 1e-6f) continue;   /* degenerate */
        for (int d = 0; d < 3; ++d) t.n[d] /= L;
        t.d = t.n[0] * t.p[0][0] + t.n[1] * t.p[0][1] + t.n[2] * t.p[0][2];
        memcpy(t.prm, v->params, 4);
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 65536;
            *tris = (ZfTri *)realloc(*tris, sizeof(ZfTri) * (size_t)*cap);
            if (!*tris) { fprintf(stderr, "out of memory\n"); exit(1); }
        }
        (*tris)[(*n)++] = t;
    }
}

static int zfight_scan(double X, double Z, double R) {
    static void (*const LAYERS[])(int, int, Mesh *) = { city_build_mesh, city_build_interiors, city_build_props, city_build_plants, city_build_litter };
    ZfTri *tris = NULL;
    int n = 0, cap = 0;
    for (int cz = (int)floor((Z - R) / CHUNK_SIZE); cz <= (int)floor((Z + R) / CHUNK_SIZE); ++cz)
        for (int cx = (int)floor((X - R) / CHUNK_SIZE); cx <= (int)floor((X + R) / CHUNK_SIZE); ++cx)
            for (size_t L = 0; L < ARRAY_LEN(LAYERS); ++L) {
                Mesh m = { 0 };
                LAYERS[L](cx, cz, &m);
                zf_gather(&m, X, Z, R, &tris, &n, &cap);
                mesh_free(&m);
            }
    ZfGroup g[256];
    int ng = 0, pairs = 0;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const ZfTri *a = &tris[i], *b = &tris[j];
            if (a->mn[0] > b->mx[0] + ZF_GAP || b->mn[0] > a->mx[0] + ZF_GAP || a->mn[1] > b->mx[1] + ZF_GAP ||
                b->mn[1] > a->mx[1] + ZF_GAP || a->mn[2] > b->mx[2] + ZF_GAP || b->mn[2] > a->mx[2] + ZF_GAP) continue;
            if (a->n[0] * b->n[0] + a->n[1] * b->n[1] + a->n[2] * b->n[2] < 0.999f) continue;
            bool flush = true;
            for (int k = 0; k < 3 && flush; ++k)
                flush = fabsf(a->n[0] * b->p[k][0] + a->n[1] * b->p[k][1] + a->n[2] * b->p[k][2] - a->d) < ZF_GAP;
            if (!flush || !zf_overlap(a, b)) continue;
            pairs++;
            /* the pair's materials, in a fixed order, so a/b and b/a group together */
            const uint8_t *pa = a->prm, *pb = b->prm;
            if (memcmp(pa, pb, 4) > 0) { const uint8_t *t = pa; pa = pb; pb = t; }
            int k = 0;
            while (k < ng && (memcmp(g[k].a, pa, 4) || memcmp(g[k].b, pb, 4))) ++k;
            if (k == ng) {
                if (ng == (int)ARRAY_LEN(g)) continue;
                memcpy(g[k].a, pa, 4); memcpy(g[k].b, pb, 4); g[k].count = 0;
                for (int d = 0; d < 3; ++d) { g[k].at[d] = (a->p[0][d] + a->p[1][d] + a->p[2][d]) / 3.0f; g[k].n[d] = a->n[d]; }
                ng++;
            }
            g[k].count++;
        }
    printf("%d triangles within %.0f m of (%.1f, %.1f): %d overlapping same-facing pairs within %.0f mm, in %d groups\n",
           n, R, X, Z, pairs, ZF_GAP * 1000.0f, ng);
    for (int k = 0; k < ng; ++k) {
        /* a view of the first: 4 m out along its normal (or, flat, from the side), looking back at it */
        const ZfGroup *q = &g[k];
        float hx = q->n[0], hz = q->n[2], hl = sqrtf(hx * hx + hz * hz);
        if (hl < 0.3f) { hx = 1.0f; hz = 0.0f; } else { hx /= hl; hz /= hl; }
        double cx = X + q->at[0] + hx * 4.0, cz = Z + q->at[2] + hz * 4.0;
        printf("  %4d x  %u/%u/%u/%u vs %u/%u/%u/%u  at (%.2f, %.2f, %.2f) n (%.2f %.2f %.2f)  --cam %.2f %.2f %.1f 0\n", q->count,
               q->a[0], q->a[1], q->a[2], q->a[3], q->b[0], q->b[1], q->b[2], q->b[3], X + q->at[0], q->at[1], Z + q->at[2],
               q->n[0], q->n[1], q->n[2], cx, cz, atan2(-hx, hz) * 57.2958);
    }
    free(tris);
    return 0;
}

/* Whether chunk-local (x, z) is open street in m: a road or sidewalk under it, with nothing over it
 * within 8 m (an arcade, a canopy, a deck). *y: that ground's height. */
static bool open_street(const Mesh *m, float x, float z, float *y) {
    float ground = -1e9f, over = 1e9f;
    for (uint32_t i = 0; i + 2 < m->count; i += 3) {
        const float *a = m->v[i].pos, *b = m->v[i + 1].pos, *c = m->v[i + 2].pos;
        float d = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
        if (fabsf(d) < 1e-6f) continue;
        float u = ((b[0] - x) * (c[2] - z) - (c[0] - x) * (b[2] - z)) / d;
        float v = ((c[0] - x) * (a[2] - z) - (a[0] - x) * (c[2] - z)) / d;
        if (u < 0.0f || v < 0.0f || u + v > 1.0f) continue;
        float h = u * a[1] + v * b[1] + (1.0f - u - v) * c[1];
        int mat = m->v[i].params[0];
        if ((mat == MAT_ROAD || mat == MAT_SIDEWALK) && h > ground) ground = h;
        else over = fminf(over, h);
    }
    *y = ground;
    return ground > -1e8f && !(over > ground + 0.3f && over < ground + 8.0f);
}

/* city_test --graffiti: how much graffiti the chunks round the spawn carry (city_graffiti_corners:
 * graffitiLevel in shaders/city/surface.glsl), then --cam views from open street (open_street), 8 m
 * out and at eye height (flying: nothing moves it while the city streams in), of brick, tile and stone faces 8 m
 * wide or more in the most and the least painted chunks (with the share of their shops lit: the rest are
 * shuttered), of blacked-out lobby glass (a room or core behind it: params[3] & 64), and of loading bays'
 * shutters where there's more than a little. */
static int graffiti_cams(void) {
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    int hx = (int)floor(sx / CHUNK_SIZE), hz = (int)floor(sz / CHUNK_SIZE);
    const int R = 12;
    int hist[5] = { 0 }, nPoor = 0, nRich = 0, nDark = 0, nBay = 0;
    for (int cz = hz - R; cz <= hz + R; ++cz)
        for (int cx = hx - R; cx <= hx + R; ++cx) {
            uint32_t c = city_graffiti_corners(cx, cz);
            float lv = 0.0f;
            for (int k = 0; k < 4; ++k) lv += (float)((c >> (8 * k)) & 255u) / (4.0f * 255.0f);
            hist[lv >= 1.0f ? 4 : (int)(lv * 5.0f)]++;
            bool poor = lv > 0.7f && nPoor < 14, rich = lv < 0.12f && nRich < 3, bays = lv > 0.5f && nBay < 6;
            if (!poor && !rich && !bays && nDark >= 4) continue;
            Mesh m = { 0 };
            city_build_mesh(cx, cz, &m);
            uint32_t lastSeed = 0;
            for (uint32_t i = 0; i + 5 < m.count && bays; i += 6) {   /* loading bays' shutters: a door panel is two triangles */
                const Vertex *v = &m.v[i];
                if (v->params[0] != MAT_DOOR || v->params[1] != 2 || v->seed == lastSeed) continue;
                float nx = v->nrm[0] / 127.0f, nz = v->nrm[2] / 127.0f, cxl = 0.0f, czl = 0.0f, gy;
                for (int k = 0; k < 6; ++k) { cxl += m.v[i + k].pos[0] / 6.0f; czl += m.v[i + k].pos[2] / 6.0f; }
                float lx = cxl + nx * 7.0f, lz = czl + nz * 7.0f;
                if (!open_street(&m, lx, lz, &gy)) continue;
                lastSeed = v->seed;
                printf("bay        level %.2f  shutter at (%.1f, %.1f): --cam %.2f %.2f %.1f 4 %.1f\n", lv, m.ox + cxl, m.oz + czl,
                       m.ox + lx, m.oz + lz, atan2(-nx, nz) * 57.2958, gy + 1.7);
                if (++nBay >= 6) bays = false;
            }
            for (uint32_t i = 0; i < m.count; ++i) {
                const Vertex *v = &m.v[i];
                if (v->params[0] != MAT_FACADE || !(v->params[2] & 1) || v->seed == lastSeed) continue;
                float nx = v->nrm[0] / 127.0f, ny = v->nrm[1] / 127.0f, nz = v->nrm[2] / 127.0f;
                if (fabsf(ny) > 0.1f || v->pos[1] - v->base > 0.5f || v->uv[0] < 8.0f) continue;
                int pal = v->params[3] & 7;
                bool brick = (pal == 1 || pal == 3 || pal == 5) && ((v->params[2] >> 3) & 3) != 1;   /* brick, tile, stone */
                bool dark = (v->params[3] & 64) && !(v->params[3] & 128) && nDark < 4;
                static const char *const POOR[8] = { 0, "poor brick", 0, "poor tile", 0, "poor stone" }, *const RICH[8] = { 0, "rich brick", 0, "rich tile", 0, "rich stone" };
                const char *what = brick && poor ? POOR[pal] : brick && rich ? RICH[pal] : dark ? "dark glass" : NULL;
                if (!what) continue;
                float lx = v->pos[0] - v->uv[0] * 0.5f * nz + nx * 8.0f, lz = v->pos[2] + v->uv[0] * 0.5f * nx + nz * 8.0f, gy;
                if (!open_street(&m, lx, lz, &gy)) continue;
                lastSeed = v->seed;
                printf("%-10s level %.2f  shops lit %d/8  face at (%.1f, %.1f) %.0f m wide: --cam %.2f %.2f %.1f 6 %.1f\n", what, lv,
                       (v->params[3] >> 3) & 7, m.ox + v->pos[0], m.oz + v->pos[2], v->uv[0], m.ox + lx, m.oz + lz, atan2(-nx, nz) * 57.2958, gy + 1.7);
                if (what[0] == 'p') { if (++nPoor >= 14) poor = false; }
                else if (what[0] == 'r') { if (++nRich >= 3) rich = false; }
                else nDark++;
            }
            mesh_free(&m);
        }
    printf("chunks by graffiti level (0-0.2 .. 0.8-1): %d %d %d %d %d\n", hist[0], hist[1], hist[2], hist[3], hist[4]);
    return 0;
}

/* city_test --facades: --cam views from the street of buildings round the spawn in each
 * walled palette (facadeWallAlbedo in shaders/city/facade.glsl: 0 concrete, 1 brick, 2 metal
 * panels, 3 tile, 5 stone, 6 bronze), each 12 m out from a grounded face and looking up a
 * little at its first floors, to judge their authored grain (assets/materials/facade_*.json). */
static int facades_cams(void) {
    static const char *const NAMES[8] = { "concrete", "brick", "metal", "tile", "glass", "stone", "bronze", "glass" };
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    int hx = (int)floor(sx / CHUNK_SIZE), hz = (int)floor(sz / CHUNK_SIZE), shown[8] = { 0 };
    uint32_t seen[8][3] = { { 0 } };
    for (int ring = 0; ring <= 6; ++ring)
        for (int cz = hz - ring; cz <= hz + ring; ++cz)
            for (int cx = hx - ring; cx <= hx + ring; ++cx) {
                if (abs(cx - hx) != ring && abs(cz - hz) != ring) continue;
                Mesh m = { 0 };
                city_build_mesh(cx, cz, &m);
                for (uint32_t i = 0; i < m.count; ++i) {
                    const Vertex *v = &m.v[i];
                    int pal = v->params[3] & 7;
                    if (v->params[0] != MAT_FACADE || !(v->params[2] & 1) || pal == 4 || pal == 7 || shown[pal] >= 3) continue;
                    if (((v->params[2] >> 3) & 3) == 1) continue;   /* a curtain wall: glass */
                    float nx = v->nrm[0] / 127.0f, ny = v->nrm[1] / 127.0f, nz = v->nrm[2] / 127.0f;
                    if (fabsf(ny) > 0.1f || v->pos[1] - v->base < 7.0f || v->uv[0] < 6.0f) continue;
                    bool dup = false;
                    for (int k = 0; k < shown[pal]; ++k) dup |= seen[pal][k] == v->seed;
                    if (dup) continue;
                    seen[pal][shown[pal]++] = v->seed;
                    double x = m.ox + v->pos[0] + nx * 12.0, z = m.oz + v->pos[2] + nz * 12.0;
                    printf("%-8s face at (%.1f, %.1f): --cam %.2f %.2f %.1f 18\n", NAMES[pal], m.ox + v->pos[0], m.oz + v->pos[2],
                           x, z, atan2(-nx, nz) * 57.2958);
                }
                mesh_free(&m);
            }
    return 0;
}

/* city_test --screens: how many of the video screens round the spawn each authored
 * advert fits (its aspect ratio within ADVERT_ASPECT_FIT of theirs), then --cam views
 * of the nearest it fits, from close in front (the LEDs' emitters), 25 m and 90 m off,
 * each with a --time at which it shows an advert 4 s in. The fit, the slot and the
 * pick are the shaders' (advertFits and advertPick in shaders/adverts.glsl,
 * screenAdvert and advertPhase in shaders/common.glsl), with the adverts in assets/. */
static float g_adAspect[ADVERT_TABLE_SIZE], g_adWeight[ADVERT_TABLE_SIZE];

static bool advert_fits(int i, float aspect) {
    return g_adWeight[i] > 0.0f && fabsf(logf(aspect / g_adAspect[i])) <= logf(1.0f + (float)ADVERT_ASPECT_FIT);
}

/* A screen light's aspect (its kind: SKY_KIND_SCREEN in src/core/common.h). */
static float screen_aspect(const SkyLight *L) {
    uint32_t h = L->kind >> 24;
    return (float)((L->kind >> 16) & 255u) / (float)(h ? h : 1u);
}

static int advert_pick(uint32_t h, float aspect) {
    float total = (float)ADVERT_PLACEHOLDER_WEIGHT;
    for (int i = 0; i < ADVERT_COUNT; ++i) if (advert_fits(i, aspect)) total += g_adWeight[i];
    float x = (float)((hash_combine(h, 0xAD5u) >> 8) * (1.0 / 16777216.0)) * total - (float)ADVERT_PLACEHOLDER_WEIGHT;
    for (int i = 0; i < ADVERT_COUNT && x >= 0.0f; ++i)
        if (advert_fits(i, aspect)) {
            if (x < g_adWeight[i]) return i;
            x -= g_adWeight[i];
        }
    return -1;
}

static int screens_cams(void) {
    for (int i = 0; i < ADVERT_COUNT; ++i) {
        char path[1100], err[1300];
        snprintf(path, sizeof(path), "%s/adverts/%s.json", NEONCITY_ASSETS, ADVERT_NAMES[i]);
        AdvertDef d;
        AdvertFrames f;
        if (!advert_def_load(path, &d, err, sizeof(err)) || !advert_frames_load(d.frames, &f, err, sizeof(err))) {
            printf("advert %s: %s\n", ADVERT_NAMES[i], err);
            continue;
        }
        g_adAspect[i] = (float)f.width / (float)f.height;
        g_adWeight[i] = d.weight;
        advert_frames_free(&f);
    }
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    int hx = (int)floor(sx / CHUNK_SIZE), hz = (int)floor(sz / CHUNK_SIZE), shown = 0, screens = 0;
    int fit[ADVERT_TABLE_SIZE] = { 0 };
    const int RINGS = 6;
    for (int ring = 0; ring <= RINGS; ++ring)
        for (int cz = hz - ring; cz <= hz + ring; ++cz)
            for (int cx = hx - ring; cx <= hx + ring; ++cx) {
                if (abs(cx - hx) != ring && abs(cz - hz) != ring) continue;
                Mesh m = { 0 };
                city_build_mesh(cx, cz, &m);
                for (uint32_t i = 0; i < m.nair; ++i) {
                    const SkyLight *L = &m.air[i];
                    if (!(L->kind & SKY_KIND_SCREEN)) continue;
                    screens++;
                    float aspect = screen_aspect(L);
                    bool any = false;
                    for (int a = 0; a < ADVERT_COUNT; ++a) if (advert_fits(a, aspect)) { fit[a]++; any = true; }
                    if (!any || shown >= 8) continue;
                    shown++;
                    double x = m.ox + L->pos[0], z = m.oz + L->pos[2], y = L->pos[1];
                    float area = L->power / 0.7f;   /* sign_lights */
                    float off = (float)((L->seed >> 8) * (1.0 / 16777216.0)) * 30.0f;
                    double t = -1.0;
                    for (int k = (int)ceilf(off / (float)ADVERT_SLOT_SECONDS); k < 400 && t < 0.0; ++k)
                        if (advert_pick(hash_combine(L->seed, (uint32_t)k), aspect) >= 0)
                            t = k * ADVERT_SLOT_SECONDS - off + ADVERT_SLOT_SECONDS * ADVERT_STATIC + 4.0;
                    printf("screen %.0f m2, %.2f:1, at (%.1f, %.1f) y %.1f facing (%.2f, %.2f), an advert at --time %.2f\n",
                           area, aspect, x, z, y, L->nx, L->nz, t);
                    static const double DIST[3] = { 0.0, 25.0, 90.0 };
                    for (int k = 0; k < 3; ++k) {
                        double dd = k ? DIST[k] : 0.5 * sqrt(area / 2.4);   /* near: half a billboard's height */
                        double ex = x + L->nx * dd, ez = z + L->nz * dd;
                        printf("  --cam %.2f %.2f %.1f 0 %.1f\n", ex, ez, atan2(-L->nx, L->nz) * 57.2958, y - 1.7);
                    }
                }
                mesh_free(&m);
            }
    printf("%d screens within %d chunks of the spawn\n", screens, RINGS);
    for (int a = 0; a < ADVERT_COUNT; ++a)
        printf("advert %s (%.3f:1) fits %d of them\n", ADVERT_NAMES[a], g_adAspect[a], fit[a]);
    return 0;
}

/* city_test --grass [X Z]: --cam views of grass round (X, Z) (by default the
 * boulevard at the eastern freeway gate) for the grass roadmap's captures: the
 * biggest parks and fields (ZONE_PARK, ZONE_RURAL) within 3 km, at walking
 * height and 40 m up; and on the land past the edge (urban 0) the grassiest
 * level point (a meadow) and the grassiest slope (15 to 30 degrees), each at
 * walking height, 40 m up and 150 m up. */
static void grass_land_cams(const char *what, double x, double z, double yawDeg, double grass, double ny) {
    double h = terrain_height(x, z);
    printf("%s: grass %.2f, slope %.0f deg, ground %.1f m\n", what, grass, acos(ny) * 57.2958, h);
    printf("  --cam %.1f %.1f %.0f -10\n", x, z, yawDeg);
    printf("  --cam %.1f %.1f %.0f -25 %.1f\n", x, z, yawDeg, h + 40.0);
    printf("  --cam %.1f %.1f %.0f -35 %.1f\n", x, z, yawDeg, h + 150.0);
}

static int grass_cams(double ax, double az) {
    const double R = 3000.0, STEP = 1000.0;
    PlanArea *a = (PlanArea *)malloc(sizeof(PlanArea));
    struct { double area, x, z; int zone; float kept; } best[3][4] = { 0 };   /* parks, fields, and parks mown (lawn) */
    for (double bz = az - R; bz < az + R; bz += STEP)
        for (double bx = ax - R; bx < ax + R; bx += STEP) {
            plan_area(bx, bz, bx + STEP, bz + STEP, a);
            for (int i = 0; i < a->nblocks; ++i) {
                const Block *b = &a->blocks[i];
                int k = b->zone == ZONE_PARK ? 0 : b->zone == ZONE_RURAL ? 1 : -1;
                if (k < 0 || b->lot.n < 3) continue;
                DVec2 c = poly_centroid(&b->lot);
                if (c.x < bx || c.x >= bx + STEP || c.z < bz || c.z >= bz + STEP) continue;   /* each block once */
                if (!poly_contains(&b->lot, c.x, c.z, 0.0)) continue;
                double ar = poly_area(&b->lot);
                /* how well kept its grass is, as streets.c emit_lot has it (city_grass_param) */
                DVec2 bc = poly_centroid(&b->curb);
                float sq = (float)(uint8_t)(city_squalor(b->zone, bc.x, bc.z) * 255.0f + 0.5f) / 255.0f;
                float kept = k == 0 ? 1.0f - smoothstepf(0.45f, 0.8f, sq) : 0.0f;
                for (int pass = 0; pass < 2; ++pass) {
                    int kk = pass ? 2 : k;
                    if (pass && !(k == 0 && kept > 0.9f)) break;
                    for (int s = 0; s < 4; ++s)
                        if (ar > best[kk][s].area) {
                            memmove(&best[kk][s + 1], &best[kk][s], (size_t)(3 - s) * sizeof(best[kk][0]));
                            best[kk][s].area = ar; best[kk][s].x = c.x; best[kk][s].z = c.z; best[kk][s].zone = b->zone; best[kk][s].kept = kept;
                            break;
                        }
                }
            }
        }
    /* the edge verge: the nearest piece of the outline at least 60 m from (X, Z)
     * (off the gate's road), from 8 m inside it looking out, the eye fixed at
     * walking height (fly mode: walking would move the player off the spot) */
    plan_area(ax - 500.0, az - 500.0, ax + 500.0, az + 500.0, a);
    double vd = 1e30, vx = 0, vz = 0, vnx = 0, vnz = 0;
    for (int i = 0; i < a->nedges; ++i) {
        const PlanEdge *e = &a->edges[i];
        double len = hypot(e->b.x - e->a.x, e->b.z - e->a.z);
        for (double s = 0.0; s <= len; s += 5.0) {
            double px = e->a.x + (e->b.x - e->a.x) * s / len, pz = e->a.z + (e->b.z - e->a.z) * s / len, d = hypot(px - ax, pz - az);
            if (d >= 60.0 && d < vd) { vd = d; vx = px; vz = pz; vnx = e->n.x; vnz = e->n.z; }
        }
    }
    if (vd < 1e30) {
        double x = vx - vnx * 8.0, z = vz - vnz * 8.0;
        printf("verge at (%.1f, %.1f), %.0f m from (X, Z), facing out (%.2f, %.2f)\n", vx, vz, vd, vnx, vnz);
        printf("  --cam %.1f %.1f %.0f -20 %.1f\n", x, z, atan2(vnx, -vnz) * 57.2958, city_ground_y(x, z));
    }
    free(a);
    static const char *const KIND[3] = { "park", "field", "mown park" };
    for (int k = 0; k < 3; ++k)
        for (int s = 0; s < 4 && best[k][s].area > 0.0; ++s) {
            double x = best[k][s].x, z = best[k][s].z, y = city_ground_y(x, z);
            printf("%s %.0f m2 at (%.1f, %.1f), ground %.1f m, %.0f m from (X, Z), kept %.2f\n", KIND[k], best[k][s].area, x, z, y,
                   hypot(x - ax, z - az), best[k][s].kept);
            printf("  --cam %.1f %.1f 0 -10\n", x, z);
            printf("  --cam %.1f %.1f 0 -30 %.1f\n", x, z, y + 40.0);
        }
    /* the land past the edge, under the deck's glow (within 600 m of it): a 25 m
     * lattice out to 3 km for the meadow; the slope, rarer, the nearest within
     * 15 km on a 100 m lattice with grass over half */
    double mx = 0, mz = 0, mg = -1, my = 1, sx = 0, sz = 0, sg = -1, sy = 1, sd = 1e30;
    for (int pass = 0; pass < 2; ++pass) {
        double RR = pass ? 15000.0 : R, st = pass ? 100.0 : 25.0;
        for (double z = az - RR; z <= az + RR; z += st)
        for (double x = ax - RR; x <= ax + RR; x += st) {
            if (plan_urban(x, z) > 0.0f || (!pass && plan_city_edge(x, z) > 600.0)) continue;
            TerrainCover c;
            terrain_cover(x, z, 1.0, &c);
            if (c.snow > 0.0f) continue;
            double e = 1.0, hx = terrain_height(x + e, z) - terrain_height(x - e, z), hz = terrain_height(x, z + e) - terrain_height(x, z - e);
            double ny = 2.0 * e / sqrt(hx * hx + hz * hz + 4.0 * e * e);
            double g = c.w[TERRAIN_COVER_GRASS];
            if (!pass && ny > 0.995 && g > mg) { mg = g; mx = x; mz = z; my = ny; }
            double d = hypot(x - ax, z - az);
            if (pass && ny < cos(15.0 / 57.2958) && ny > cos(30.0 / 57.2958) && g > 0.5 && d < sd) { sd = d; sg = g; sx = x; sz = z; sy = ny; }
        }
    }
    if (mg >= 0) grass_land_cams("meadow", mx, mz, atan2(mx - ax, -(mz - az)) * 57.2958, mg, my);   /* looking away from (X, Z) */
    if (sg >= 0) grass_land_cams("slope", sx, sz, atan2(sx - ax, -(sz - az)) * 57.2958, sg, sy);
    return 0;
}

int main(int argc, char **argv) {
    terrain_earth_load(NULL);
    plan_world_init(cpu_count());   /* before the workers below ask the plan */
    models_load(NEONCITY_ASSETS);   /* the street furniture's models (src/city/models.h), from the source tree */
    /* A scattered set of chunks, including far-away and negative coordinates. */
    Rng r = rng_make(1234);
    for (int i = 0; i < N_CHUNKS; ++i) {
        int far = i % 4 == 0 ? 1000000 : 40;
        g_cx[i] = rng_int(&r, -far, far);
        g_cz[i] = rng_int(&r, -far, far);
    }
    if (argc > 1 && strcmp(argv[1], "--fingerprint") == 0) return fingerprint();
    if (argc > 1 && strcmp(argv[1], "--proxies") == 0) return proxy_survey();
    if (argc > 1 && strcmp(argv[1], "--props") == 0) return props_cams();
    if (argc > 1 && strcmp(argv[1], "--vendor-sites") == 0) return vendor_sites_cams(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--vendors") == 0) return vendor_cams(argc, argv);
    if (argc > 1 && strcmp(argv[1], "--slice") == 0) return slice_cams(argc, argv);
    if (argc > 3 && strcmp(argv[1], "--vs-probe") == 0) return vs_probe(atof(argv[2]), atof(argv[3]));
    if (argc > 1 && strcmp(argv[1], "--screens") == 0) return screens_cams();
    if (argc > 1 && strcmp(argv[1], "--facades") == 0) return facades_cams();
    if (argc > 1 && strcmp(argv[1], "--graffiti") == 0) return graffiti_cams();
    if (argc > 1 && strcmp(argv[1], "--zfight") == 0) {
        if (argc < 5) { fprintf(stderr, "city_test --zfight X Z RADIUS\n"); return 2; }
        return zfight_scan(atof(argv[2]), atof(argv[3]), atof(argv[4]));
    }
    if (argc > 1 && strcmp(argv[1], "--grass") == 0) return grass_cams(argc > 3 ? atof(argv[2]) : 64598.0, argc > 3 ? atof(argv[3]) : 11010.0);

    /* 1. Reference pass, in order. Track mesh sizes against the GPU page budget. */
    uint32_t maxVerts = 0, maxLights = 0, maxInterior = 0, maxLitter = 0, maxPlants = 0, maxProps = 0;
    double total = 0, totalLights = 0, totalInterior = 0, totalLitter = 0, totalPlants = 0, totalProps = 0;
    for (int i = 0; i < N_CHUNKS; ++i) {
        uint32_t c, l, in, lt, pv, pr;
        g_ref[i] = chunk_checksum(g_cx[i], g_cz[i], &c, &l, &in, &lt, &pv, &pr);
        if (pr > maxProps) maxProps = pr;
        totalProps += pr;
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
    printf("street furniture vertices per chunk: avg %.0f, max %u (budget %d)\n", totalProps / N_CHUNKS, maxProps, MAX_CHUNK_PAGES * PAGE_VERTS);
    CHECK(maxProps <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk street furniture exceeds page budget");
    CHECK(maxPlants <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk plants exceed page budget");
    printf("mesh vertices per chunk: avg %.0f, max %u (budget %d)\n", total / N_CHUNKS, maxVerts, MAX_CHUNK_PAGES * PAGE_VERTS);
    printf("light records per chunk: avg %.0f, max %u (page %d)\n", totalLights / N_CHUNKS, maxLights, LIGHT_PAGE_RECS);
    CHECK(maxVerts <= (uint32_t)(MAX_CHUNK_PAGES * PAGE_VERTS), "chunk exceeds page budget");
    CHECK(maxLights <= (uint32_t)LIGHT_PAGE_RECS, "chunk light table exceeds its page");

    /* 2. Reverse order, interleaved with unrelated generation, must match. */
    for (int i = N_CHUNKS - 1; i >= 0; --i) {
        chunk_checksum(g_cx[i] + 7, g_cz[i] - 3, NULL, NULL, NULL, NULL, NULL, NULL);
        uint64_t h = chunk_checksum(g_cx[i], g_cz[i], NULL, NULL, NULL, NULL, NULL, NULL);
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

    /* 3b. The grass the blades grow on, round the grass roadmap's park, field and verge (city_test --grass). */
    {
        double area = 0.0;
        int pieces = 0, built = 0;
        check_city_grass(62653.5, 12524.9, 1, &area, &pieces, &built);
        check_city_grass(64282.6, 11755.5, 1, &area, &pieces, &built);
        check_city_grass(64623.7, 11060.8, 1, &area, &pieces, &built);
        printf("city grass: %.0f m2 in %d pieces over 27 chunks (%d with buildings near)\n", area, pieces, built);
        CHECK(area > 10000.0, "too little city grass round the park, field and verge (%.0f m2)", area);
    }

    /* 4. Layout sanity around the spawn: buildings stand on blocks (never on a
     *    road), do not overlap, and fit in the overhang budget; lamps stand on
     *    sidewalks clear of buildings; geometry stays under the culling bound. */
    DVec2 home = plan_town_centre(0, 0);
    int hx = (int)floor(home.x / CHUNK_SIZE), hz = (int)floor(home.z / CHUNK_SIZE);
    int buildings = 0, lamps = 0, drops = 0, slabs = 0, floors = 0, treads = 0, kinds[3] = { 0 }, zones[ZONE_COUNT] = { 0 }, megaX = 0, megaZ = 0, foundMega = 0;
    float floorStep = 0.0f;
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
            /* Lobby and concourse floors lie on their blocks, at their building's
             * base: a step up from the sidewalk of no more than the plot falls
             * (0.6 m at the megacity's 10 m of relief, and what the highlands
             * under it fall across the building: the ground's grade, read 50 m
             * either side, times its footprint's diagonal, and a tenth more).
             * The treads of the steps up to them lie between the two, above
             * the sidewalk at its lowest. */
            for (int i = 0; i < L.nfloor; ++i) {
                const FloorPoly *f = &L.floor[i];
                const double *v0 = L.collv[f->at.first];
                float fall = 0.6f * (float)(plan_settlement(plan_settlement_near(v0[0], v0[1])).relief_m / PLAN_RELIEF_MEGA_M);
                double lgx = (city_ground_y(v0[0] + 50.0, v0[1]) - city_ground_y(v0[0] - 50.0, v0[1])) / 100.0;
                double lgz = (city_ground_y(v0[0], v0[1] + 50.0) - city_ground_y(v0[0], v0[1] - 50.0)) / 100.0;
                int base = 0, tread = 0;
                for (int j = 0; j < L.nbuild && !base; ++j)
                    if (f->y == L.build[j].base + SIDEWALK_HEIGHT) {   /* G0 */
                        base = 1;
                        fall += (float)(1.1 * hypot(lgx, lgz) * hypot(L.build[j].foot.x1 - L.build[j].foot.x0, L.build[j].foot.z1 - L.build[j].foot.z0));
                    }
                for (int j = 0; j < L.nbuild && !base && !tread; ++j) {
                    float fy = L.build[j].base + SIDEWALK_HEIGHT;
                    float fj = fall + (float)(1.1 * hypot(lgx, lgz) * hypot(L.build[j].foot.x1 - L.build[j].foot.x0, L.build[j].foot.z1 - L.build[j].foot.z0));
                    tread = f->at.holeN == 0 && f->at.n == 4 && f->y < fy && f->y > fy - fj;
                    if (tread) fall = fj;
                }
                CHECK(base || tread, "chunk (%d,%d) floor %d (%.2f m) isn't at a building's base or on the steps up to it", cx, cz, i, f->y);
                float low = 1e30f;
                for (int k = 0; k < f->at.n; ++k) low = minf(low, city_ground_y(L.collv[f->at.first + k][0], L.collv[f->at.first + k][1]) + SIDEWALK_HEIGHT);
                CHECK(!tread || f->y > low, "chunk (%d,%d) tread %d (%.2f m) is under the sidewalk (%.2f m)", cx, cz, i, f->y, low);
                for (int k = 0; k < f->at.n; ++k) {
                    const double *v = L.collv[f->at.first + k];
                    int ok = 0;
                    for (int j = 0; j < L.ncurbs && !ok; ++j) ok = in_curb(&L.curb[j], v[0], v[1]);
                    float step = f->y - (city_ground_y(v[0], v[1]) + SIDEWALK_HEIGHT);
                    CHECK(ok && (tread || (step >= 0.0f && step <= fall)), "chunk (%d,%d) floor %d corner %d is off its block or %.2f m off the sidewalk", cx, cz, i, k, step);
                    if (!tread) floorStep = maxf(floorStep, step);
                }
                floors += base;
                treads += tread;
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
    printf("  %d lobby and concourse floors, at most %.2f m above the sidewalk; %d treads of steps up to them\n", floors, floorStep, treads);
    printf("  litter near the spawn: avg %.0f, max %u vertices per chunk\n", litterSum / litterChunks, litterMax);
    printf("  kinds: %d blocks, %d towers, %d arcologies\n", kinds[0], kinds[1], kinds[2]);
    printf("  zones: %d core, %d centre, %d estate, %d industrial\n", zones[0], zones[1], zones[2], zones[3]);
    if (foundMega) printf("  first arcology near world (%d, %d)\n", megaX, megaZ);
    CHECK(buildings > n * 3, "too few buildings around the spawn");
    CHECK(lamps > n, "too few lamps around the spawn");
    CHECK(drops > n, "too few drop kerbs around the spawn");
    CHECK(floors > n / 4, "too few lobby floors around the spawn");

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

    /* 6. The city's edge. */
    check_edge();
    check_skyline();

    /* 7. Distant proxies. */
    check_proxies();

    /* 8. Street furniture. */
    check_props(hx, hz);

    /* 9. Where the street vendors stand. */
    check_vendor_sites(hx, hz, "the spawn");
    check_vendor_shells(hx, hz, "the spawn", 0);

    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    printf("all city tests passed (%d chunks x %d orders/threads)\n", N_CHUNKS, THREADS + 2);
    return 0;
}
