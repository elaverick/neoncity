#include <stdlib.h>
#include <string.h>
#include "terrain_internal.h"

/* The caves' meshes (terrain.h, "Caves' meshes"): surface nets over the
 * field of terrain_cave.c, a brick at a time.
 *
 * A brick of a level, cell size s, has its corner at G0 s, G0 = (i, j, k) C,
 * in its cave's chart. It samples the field at the corners G0 - 2 .. G0 + C + 1
 * (S a side): every cell from -1 to C - 1 that the surface crosses gets a
 * vertex, the mean of where the surface crosses its edges, and its normal
 * the field's gradient there. Each edge from a corner in
 * [0, C) along an axis that the surface crosses gets a quad joining the four
 * cells round it. A corner's sample is the field at (G0 + a) s, the same
 * number in every brick that reads it, and the field is the same from the
 * passages near the brick as from all of them (terrain_cave_near_box), so a
 * cell two bricks share gets the same vertex in both. */

#define C  TERRAIN_CAVE_CELLS
#define S  (C + 4)   /* samples a side: corners -2 .. C + 1 */
#define V  (C + 1)   /* cells a side that can have a vertex: -1 .. C - 1 */
#define Q  TERRAIN_CAVE_QUANT

_Static_assert((C + 1) * Q < 32768, "a cave vertex's place in its brick doesn't fit 16 bits");
_Static_assert(TERRAIN_CAVE_MESH_VERTS <= 65536, "a cave brick's vertices don't fit 16-bit indices");

double terrain_cave_voxel(int lod) { return TERRAIN_CAVE_VOXEL0 * (double)(1 << lod); }
double terrain_cave_brick(int lod) { return C * terrain_cave_voxel(lod); }
double terrain_cave_lod_reach(int lod) {
    static const double R[TERRAIN_CAVE_LODS] = { 150.0, 400.0, TERRAIN_CAVE_DRAW_M };
    return R[lod];
}

/* The box of corners g0 + a0 .. g0 + a1 along each axis, cells of s. */
static void corner_box(const int64_t g0[3], int a0, int a1, double s, double lo[3], double hi[3]) {
    for (int a = 0; a < 3; ++a) {
        lo[a] = (double)(g0[a] + a0) * s;
        hi[a] = (double)(g0[a] + a1) * s;
    }
}

/* --- The live bricks ------------------------------------------------------------ */

typedef struct BrickSet { int64_t *key; int cap, n; } BrickSet;

static int64_t brick_key(int64_t i, int64_t j, int64_t k) { return ((i & 0x1FFFFF) << 42) | ((j & 0x1FFFFF) << 21) | (k & 0x1FFFFF); }
static int32_t key_part(int64_t key, int shift) { int64_t v = (key >> shift) & 0x1FFFFF; return (int32_t)(v >= 0x100000 ? v - 0x200000 : v); }

static int set_add(BrickSet *s, int64_t key) {
    uint32_t h = (uint32_t)((uint64_t)key * 0x9E3779B97F4A7C15ull >> 32) & (uint32_t)(s->cap - 1);
    while (s->key[h] != INT64_MIN) {
        if (s->key[h] == key) return 0;
        h = (h + 1) & (uint32_t)(s->cap - 1);
    }
    s->key[h] = key;
    s->n++;
    return 1;
}

static int cmp_key(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* Each brick whose box (its own cells, where its quads' edges start) one of
 * the pieces' air can reach: over each piece's bounds, by
 * terrain_cave_box_live. */
int terrain_cave_bricks(const TerrainCave *c, int lod, int32_t (*out)[3], int max) {
    const double B = terrain_cave_brick(lod), s = terrain_cave_voxel(lod), extra = TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M + 1.0;
    BrickSet set;
    set.cap = 1;
    while (set.cap < 4 * max) set.cap <<= 1;
    set.key = (int64_t *)malloc(sizeof(int64_t) * (size_t)set.cap);
    set.n = 0;
    if (!set.key) return 0;
    for (int k = 0; k < set.cap; ++k) set.key[k] = INT64_MIN;
    for (int q = 0; q < c->nseg + c->nroom; ++q) {
        double lo[3], hi[3];
        if (q < c->nseg) {
            const TerrainCaveSeg *g = &c->seg[q];
            double R = fmax(g->ra, g->rb) + extra;
            for (int a = 0; a < 3; ++a) { lo[a] = fmin(g->a[a], g->b[a]) - R; hi[a] = fmax(g->a[a], g->b[a]) + R; }
        } else {
            const TerrainCaveRoom *r = &c->room[q - c->nseg];
            double w = fmax(r->r[0], r->r[2]) + extra, h = r->r[1] + extra;
            lo[0] = r->c[0] - w; hi[0] = r->c[0] + w;
            lo[1] = r->c[1] - h; hi[1] = r->c[1] + h;
            lo[2] = r->c[2] - w; hi[2] = r->c[2] + w;
        }
        int64_t b0[3], b1[3];
        for (int a = 0; a < 3; ++a) { b0[a] = (int64_t)floor(lo[a] / B); b1[a] = (int64_t)floor(hi[a] / B); }
        for (int64_t k = b0[2]; k <= b1[2]; ++k)
            for (int64_t j = b0[1]; j <= b1[1]; ++j)
                for (int64_t i = b0[0]; i <= b1[0]; ++i) {
                    if (set.n >= max) goto full;
                    int64_t g0[3] = { i * C, j * C, k * C };
                    double blo[3], bhi[3];
                    corner_box(g0, 0, C, s, blo, bhi);
                    int64_t key = brick_key(i, j, k);
                    /* (the set is checked first: most candidates are already in) */
                    uint32_t h = (uint32_t)((uint64_t)key * 0x9E3779B97F4A7C15ull >> 32) & (uint32_t)(set.cap - 1);
                    int seen = 0;
                    while (set.key[h] != INT64_MIN && !seen) { seen = set.key[h] == key; h = (h + 1) & (uint32_t)(set.cap - 1); }
                    if (!seen && terrain_cave_box_live(c, blo, bhi)) set_add(&set, key);
                }
    }
full:;
    int n = 0;
    for (int k = 0; k < set.cap; ++k)
        if (set.key[k] != INT64_MIN) set.key[n++] = set.key[k];
    qsort(set.key, (size_t)n, sizeof(int64_t), cmp_key);
    for (int k = 0; k < n; ++k) {
        out[k][0] = key_part(set.key[k], 42);
        out[k][1] = key_part(set.key[k], 21);
        out[k][2] = key_part(set.key[k], 0);
    }
    free(set.key);
    return n;
}

/* --- A brick's mesh --------------------------------------------------------------- */

static uint32_t oct_encode(const double n[3]) {
    double l = fabs(n[0]) + fabs(n[1]) + fabs(n[2]), px = n[0] / l, py = n[1] / l;
    if (n[2] < 0.0) {
        double ox = (1.0 - fabs(py)) * (px >= 0.0 ? 1.0 : -1.0), oy = (1.0 - fabs(px)) * (py >= 0.0 ? 1.0 : -1.0);
        px = ox; py = oy;
    }
    int ix = (int)lround(fmax(-1.0, fmin(1.0, px)) * 32767.0), iy = (int)lround(fmax(-1.0, fmin(1.0, py)) * 32767.0);
    return (uint32_t)(uint16_t)(int16_t)ix | (uint32_t)(uint16_t)(int16_t)iy << 16;
}

/* The scratch a mesh is made in, per thread. */
typedef struct Scratch {
    double f[S * S * S];
    int    vid[V * V * V];     /* a cell's output vertex, -1 none yet */
    float  pos[V * V * V][3];  /* a crossed cell's vertex, in its cell (0 .. 1) */
    float  nrm[V * V * V][3];
    uint8_t crossed[V * V * V];
    const TerrainCave     *c;      /* the brick being meshed */
    const TerrainCaveNear *near;
    int64_t g0[3];
    double  s;
} Scratch;
static _Thread_local Scratch *t_scratch;

#define SI(x, y, z) ((((z) + 2) * S + ((y) + 2)) * S + ((x) + 2))   /* corner (x, y, z), -2 .. C + 1 */
#define CI(x, y, z) ((((z) + 1) * V + ((y) + 1)) * V + ((x) + 1))   /* cell (x, y, z), -1 .. C - 1 */

static const int EDGE[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };

/* Cell (x, y, z)'s vertex if the surface crosses it: corners numbered by
 * their bits x | y << 1 | z << 2. */
static void cell_vertex(Scratch *w, int x, int y, int z) {
    double f[8];
    int inside = 0;
    for (int k = 0; k < 8; ++k) {
        f[k] = w->f[SI(x + (k & 1), y + (k >> 1 & 1), z + (k >> 2))];
        inside |= (f[k] < 0.0) << k;
    }
    int ci = CI(x, y, z);
    w->crossed[ci] = inside != 0 && inside != 0xFF;
    w->vid[ci] = -1;
    if (!w->crossed[ci]) return;
    double p[3] = { 0.0, 0.0, 0.0 };
    int n = 0;
    for (int e = 0; e < 12; ++e) {
        int a = EDGE[e][0], b = EDGE[e][1];
        if ((f[a] < 0.0) == (f[b] < 0.0)) continue;
        double t = f[a] / (f[a] - f[b]);
        for (int q = 0; q < 3; ++q) {
            double pa = (a >> q) & 1, pb = (b >> q) & 1;
            p[q] += pa + (pb - pa) * t;
        }
        n++;
    }
    double g[3] = { 0.0, 0.0, 0.0 };
    for (int q = 0; q < 3; ++q) p[q] /= n;
    /* the field's gradient: its central differences at the corners, blended as the corners are */
    for (int k = 0; k < 8; ++k) {
        int cx = x + (k & 1), cy = y + (k >> 1 & 1), cz = z + (k >> 2);
        double wk = ((k & 1) ? p[0] : 1.0 - p[0]) * ((k >> 1 & 1) ? p[1] : 1.0 - p[1]) * ((k >> 2) ? p[2] : 1.0 - p[2]);
        g[0] += wk * (w->f[SI(cx + 1, cy, cz)] - w->f[SI(cx - 1, cy, cz)]);
        g[1] += wk * (w->f[SI(cx, cy + 1, cz)] - w->f[SI(cx, cy - 1, cz)]);
        g[2] += wk * (w->f[SI(cx, cy, cz + 1)] - w->f[SI(cx, cy, cz - 1)]);
    }
    double l = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    for (int q = 0; q < 3; ++q) {
        w->pos[ci][q] = (float)p[q];
        w->nrm[ci][q] = l > 0.0 ? (float)(-g[q] / l) : (q == 1 ? 1.0f : 0.0f);   /* into the air, down the field */
    }
}

/* Cell (x, y, z)'s output vertex, made on first use. Its normal is the
 * field's gradient there, by differences a quarter of a cell either side
 * (the corners' differences, blended, smear a passage narrower than a few
 * cells), or theirs where that has none: from where it lies in the whole
 * chart, so a vertex two bricks share gets the same one in both. */
static int use_vertex(Scratch *w, int x, int y, int z, TerrainCaveVertex *v, int *nv) {
    int ci = CI(x, y, z);
    if (w->vid[ci] >= 0) return w->vid[ci];
    TerrainCaveVertex *o = &v[*nv];
    int cell[3] = { x, y, z };
    double P[3], h = 0.25 * w->s, g[3], l = 0.0;
    for (int q = 0; q < 3; ++q) {
        int frac = (int)lroundf(w->pos[ci][q] * (float)Q);
        o->p[q] = (int16_t)(cell[q] * Q + frac);
        P[q] = ((double)(w->g0[q] + cell[q]) + (double)frac / Q) * w->s;
    }
    for (int q = 0; q < 3; ++q) {
        double a[3] = { P[0], P[1], P[2] }, b[3] = { P[0], P[1], P[2] };
        a[q] += h; b[q] -= h;
        g[q] = terrain_cave_sdf_near(w->c, w->near, a[0], a[1], a[2]) - terrain_cave_sdf_near(w->c, w->near, b[0], b[1], b[2]);
        l += g[q] * g[q];
    }
    l = sqrt(l);
    double n[3] = { w->nrm[ci][0], w->nrm[ci][1], w->nrm[ci][2] };
    if (l > 0.0)
        for (int q = 0; q < 3; ++q) n[q] = -g[q] / l;   /* into the air, down the field */
    o->n = oct_encode(n);
    o->sky = 0;
    o->spare = 0;
    w->vid[ci] = (*nv)++;
    return w->vid[ci];
}

static int64_t dist2(const TerrainCaveVertex *a, const TerrainCaveVertex *b) {
    int64_t d = 0;
    for (int q = 0; q < 3; ++q) d += (int64_t)(a->p[q] - b->p[q]) * (a->p[q] - b->p[q]);
    return d;
}

int terrain_cave_brick_mesh(const TerrainCave *c, int lod, int bi, int bj, int bk, TerrainCaveVertex *v, uint16_t *idx, int *nidx) {
    const double s = terrain_cave_voxel(lod);
    const int64_t g0[3] = { (int64_t)bi * C, (int64_t)bj * C, (int64_t)bk * C };
    *nidx = 0;
    double lo[3], hi[3];
    corner_box(g0, 0, C, s, lo, hi);
    if (!terrain_cave_box_live(c, lo, hi)) return 0;
    corner_box(g0, -2, C + 1, s, lo, hi);
    TerrainCaveNear near;
    terrain_cave_near_box(c, lo, hi, &near);
    if (!t_scratch) t_scratch = (Scratch *)malloc(sizeof(Scratch));
    Scratch *w = t_scratch;
    if (!w) return 0;
    w->c = c; w->near = &near; w->s = s;
    for (int a = 0; a < 3; ++a) w->g0[a] = g0[a];
    int air = 0;
    for (int z = -2; z <= C + 1; ++z)
        for (int y = -2; y <= C + 1; ++y)
            for (int x = -2; x <= C + 1; ++x) {
                double f = terrain_cave_sdf_near(c, &near, (double)(g0[0] + x) * s, (double)(g0[1] + y) * s, (double)(g0[2] + z) * s);
                w->f[SI(x, y, z)] = f;
                air += f < 0.0;
            }
    if (air == 0 || air == S * S * S) return 0;
    for (int z = -1; z < C; ++z)
        for (int y = -1; y < C; ++y)
            for (int x = -1; x < C; ++x) cell_vertex(w, x, y, z);

    /* the quads of the crossed edges from the corners in [0, C): the cells
     * round the edge along d, going +u, +v, -u (d, u, v cyclic), wind about
     * +d; turned to face the air */
    int nv = 0, ni = 0;
    for (int z = 0; z < C; ++z)
        for (int y = 0; y < C; ++y)
            for (int x = 0; x < C; ++x) {
                double f0 = w->f[SI(x, y, z)];
                for (int d = 0; d < 3; ++d) {
                    int e[3] = { 0, 0, 0 };
                    e[d] = 1;
                    double f1 = w->f[SI(x + e[0], y + e[1], z + e[2])];
                    if ((f0 < 0.0) == (f1 < 0.0)) continue;
                    int u = (d + 1) % 3, vv = (d + 2) % 3, cc[4][3];
                    for (int q = 0; q < 4; ++q) {
                        cc[q][0] = x; cc[q][1] = y; cc[q][2] = z;
                    }
                    cc[0][u]--; cc[0][vv]--;   /* c00 */
                    cc[1][vv]--;               /* c10 */
                    /* cc[2]: c11, the corner's own cell */
                    cc[3][u]--;                /* c01 */
                    int id[4];
                    for (int q = 0; q < 4; ++q) id[q] = use_vertex(w, cc[q][0], cc[q][1], cc[q][2], v, &nv);
                    int flip = f0 < 0.0;   /* the air at the edge's low end: face -d */
                    int tri[6];
                    if (dist2(&v[id[0]], &v[id[2]]) <= dist2(&v[id[1]], &v[id[3]])) {
                        tri[0] = id[0]; tri[1] = id[1]; tri[2] = id[2]; tri[3] = id[0]; tri[4] = id[2]; tri[5] = id[3];
                    } else {
                        tri[0] = id[1]; tri[1] = id[2]; tri[2] = id[3]; tri[3] = id[1]; tri[4] = id[3]; tri[5] = id[0];
                    }
                    if (flip) { int t = tri[1]; tri[1] = tri[2]; tri[2] = t; t = tri[4]; tri[4] = tri[5]; tri[5] = t; }
                    for (int q = 0; q < 6; ++q) idx[ni++] = (uint16_t)tri[q];
                }
            }
    *nidx = ni;
    return nv;
}
