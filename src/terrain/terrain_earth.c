#include "terrain_internal.h"
#include "plan.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The Earth (see terrain.h, "The Earth"): the real planet's ground, read
 * from the bake tools/earth_bake.c writes (its file layout is at the top of
 * that file) and sampled anywhere on the sphere at any level of detail.
 *
 * The bake holds, per mip, the six cube faces' int16 grids with a 2-sample
 * apron past each edge, and per 8 x 8 block of each grid its minimum,
 * maximum, steepest grade and relief. A height is read so:
 *   - the mips in play come from the spacing: a mip whose samples are 2
 *     spacings apart carries just the wavelengths 4 spacings and up
 *     (TERRAIN_LOD_SAMPLES_PER_WAVE), so the level is log2(spacing / step0)
 *     + 1, and the height blends the mips either side of it (spacing 0 is
 *     mip 0 alone, the full detail; coarser spacings are ever smoother);
 *   - within a mip, each face is sampled by a bicubic Catmull-Rom over the
 *     4 x 4 samples round the point, which the apron lets run right up to
 *     the edge and half a sample past it;
 *   - across the seams, within half a sample of an edge (in the face's own
 *     angle), the faces either side (three at a corner) are each weighted by
 *     a smoothstep of their distance in from their edge and averaged, so
 *     the field is a single-valued, continuous function of the direction
 *     whichever face holds it.
 * The bounds (terrain_earth_range, terrain_earth_lod_error) come from the
 * samples a stencil over the region reads where there are few of them
 * (DIRECT_SAMPLES: every query at the coarse mips, and small regions at the
 * fine), else from the block tables. At load the apron's samples, and the grades between them,
 * are folded into the border blocks beside them, so a block's figures bound
 * every sample a stencil centred in it can read; a bicubic can overshoot its
 * samples' range by at most 2/3 of the largest step between neighbouring
 * samples (its tangents are half the two-sample differences, and the
 * Hermite tangent terms peak at 4/27 each way), which the grade times the
 * mip's sample spacing bounds.
 *
 * The bake's landform grid (terrain_earth_land) holds, per sample of one
 * coarse mip, the relief round it, the ridges' strike and the hints; it is
 * read the same way (a Catmull-Rom per face, the faces blended across the
 * seams) whatever the spacing.
 *
 * The bake's water (terrain_earth_water) is the project's rivers and lakes as
 * lines: runs of points, each river with its width, each lake's ring closed.
 * Every face is cut into buckets (sides on great circles), each listing the
 * segments within the reach of it, so a query measures the exact distance to
 * the few lines near it; whether a point lies in a lake is its bucket's
 * centre's answer (the bake's even-odd test over the rings), flipped by each
 * shore the arc from the centre crosses.
 *
 * The streams (tools/earth_streams.c, a file beside the bake) are the small
 * streams derived from the land, kept as the water is, with a bed and a flow
 * at every point; terrain_earth_water measures them with the rivers. The
 * same file gives the bake's rivers their beds and its lakes their levels.
 * They are dropped, with a warning, when another bake made them, when the
 * land at the file's probes has changed since, or when the settlements have
 * (its plan's probes, checked once they are placed:
 * terrain_earth_check_streams). Without them the water carves no bed.
 *
 * Without a bake the planet's procedural macro field (terrain_macro, in
 * metres) stands in, so a clone without the LFS tiles still runs. */

#define PI          3.14159265358979323846
#define EARTH_MAGIC "NCEARTH1"
#define EARTH_VERSION 3
#define EARTH_HEADER 256
#define APRON       2
#define BLOCK       8
#define MAX_MIPS    16
#define MIN_FACE    8
#define OVERSHOOT   (2.0 / 3.0)   /* of a step, for a bicubic Catmull-Rom (see above) */
#define DIRECT_SAMPLES 1024       /* a stencil box of up to this many samples is bounded by the samples themselves */

typedef struct EarthMip {
    int      n, g, nb;          /* samples a side, with the apron, blocks a side */
    double   step;              /* the largest distance between neighbouring samples (m) */
    double   lo, hi, grade;     /* over the mip, apron included (m, m, m/m) */
    int16_t *q[6];              /* g x g samples, (i, j) at [(j + APRON) * g + (i + APRON)] */
    int16_t *bmin[6], *bmax[6]; /* nb x nb blocks */
    float   *bslope[6], *brelief[6];
} EarthMip;

/* The landform grid (tools/earth_bake.c): n samples a side and the apron. */
typedef struct EarthLand {
    int      mip, n, g, hints;
    double   rough_max;         /* the largest roughness, 1 without the hints */
    int16_t *relief[6];
    int8_t  *se[6], *sn[6];
    uint8_t *override[6], *rough[6], *nosettle[6];
} EarthLand;

/* The water (tools/earth_bake.c): the rivers' lines and the lakes' rings as
 * runs of points, listed in B x B buckets a face, each bucket every segment
 * within the reach (plus a river's half width) of it, and whether its centre
 * lies in a lake. The streams (tools/earth_streams.c) are the same, and each
 * of their points carries its bed and its flow. The streams' file gives the
 * bake's points a bed too: a river's, or on a lake's shore that lake's
 * level. */
typedef struct EarthWater {
    int       B;
    double    reach;            /* m */
    uint32_t  V, E;
    float    *xyz;              /* V x 3 */
    uint16_t *width;            /* a river's width (m), 0 on a lake's shore */
    uint8_t  *kind, *flags;     /* 1 river, 2 lake, 3 stream; 1: a segment runs on to the next point */
    uint32_t *start;            /* 6 B B + 1 */
    uint8_t  *inside;           /* 6 B B */
    uint32_t *entry;            /* E: each segment's first point */
    float    *bed, *flow;       /* V each (flow the streams' only): world Y (m), m3/s; NULL without */
    uint8_t  *shores;           /* 6 B B, the bake's only: whether the bucket lists a lake's shore */
} EarthWater;

/* The streams' file (tools/earth_streams.c's layout). */
#define STREAMS_MAGIC  "NCSTREAM"
#define STREAMS_VERSION 2
#define STREAMS_HEADER 512
#define STREAMS_PROBES 24
#define STREAMS_PROBE_TOLERANCE 0.05   /* m: the land at a probe may differ by this (another build's float rounding) */
#define STREAMS_PLAN_TOLERANCE  0.5    /* m: and the edge measure at a plan probe by this */
#define STREAMS_PLAN_MAX        65536
#define WATER_LEVELS            32     /* the most lakes' levels one query tells apart */

static struct Earth {
    int        loaded, mips, N;
    double     scale, offset;
    double     origin_lat, origin_lon, origin_heading;
    TerrainChart chart;         /* the world's chart, about the origin */
    double     datum;           /* E0: the Earth at the origin at TERRAIN_LIFT_SPACING (m) */
    char       path[1024], stage_sha[65], streams_path[1100];
    EarthMip   mip[MAX_MIPS];
    EarthLand  land;
    EarthWater water, streams;
    /* from the streams' file: the lakes' levels where no shore comes within
     * the reach, by bucket (sorted), and the plan's probes (x, y, z, the
     * edge measure there; the first's x the settlements' count), checked by
     * terrain_earth_check_streams */
    uint32_t   nlevels, *level_bucket, nplan, plan_checked;
    float     *level, *plan_probe;
} g_earth;

/* --- Reading the file ------------------------------------------------------------ */

static uint16_t get_u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const unsigned char *p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }
static float    get_f32(const unsigned char *p) { uint32_t u = get_u32(p); float v; memcpy(&v, &u, 4); return v; }
static double   get_f64(const unsigned char *p) { uint64_t u = get_u64(p); double v; memcpy(&v, &u, 8); return v; }

static int read_i16s(FILE *f, int16_t *v, size_t n) {
    static unsigned char buf[1 << 16];
    while (n > 0) {
        size_t take = n < sizeof buf / 2 ? n : sizeof buf / 2;
        if (fread(buf, 2, take, f) != take) return 0;
        for (size_t i = 0; i < take; ++i) v[i] = (int16_t)get_u16(buf + 2 * i);
        v += take; n -= take;
    }
    return 1;
}

static int read_f32s(FILE *f, float *v, size_t n) {
    static unsigned char buf[1 << 16];
    while (n > 0) {
        size_t take = n < sizeof buf / 4 ? n : sizeof buf / 4;
        if (fread(buf, 4, take, f) != take) return 0;
        for (size_t i = 0; i < take; ++i) v[i] = get_f32(buf + 4 * i);
        v += take; n -= take;
    }
    return 1;
}

static void mip_free(EarthMip *m) {
    for (int f = 0; f < 6; ++f) { free(m->q[f]); free(m->bmin[f]); free(m->bmax[f]); free(m->bslope[f]); free(m->brelief[f]); }
    memset(m, 0, sizeof *m);
}

static void land_free(EarthLand *l) {
    for (int f = 0; f < 6; ++f) { free(l->relief[f]); free(l->se[f]); free(l->sn[f]); free(l->override[f]); free(l->rough[f]); free(l->nosettle[f]); }
    memset(l, 0, sizeof *l);
}

static void water_free(EarthWater *w) {
    free(w->xyz); free(w->width); free(w->kind); free(w->flags); free(w->start); free(w->inside); free(w->entry);
    free(w->bed); free(w->flow); free(w->shores);
    memset(w, 0, sizeof *w);
}

/* The streams and everything their file gives the bake's water. */
static void streams_free(void) {
    water_free(&g_earth.streams);
    free(g_earth.water.bed); g_earth.water.bed = NULL;
    free(g_earth.level_bucket); free(g_earth.level); free(g_earth.plan_probe);
    g_earth.level_bucket = NULL; g_earth.level = NULL; g_earth.plan_probe = NULL;
    g_earth.nlevels = g_earth.nplan = g_earth.plan_checked = 0;
}

void terrain_earth_unload(void) {
    for (int m = 0; m < g_earth.mips; ++m) mip_free(&g_earth.mip[m]);
    land_free(&g_earth.land);
    streams_free();
    water_free(&g_earth.water);
    memset(&g_earth, 0, sizeof g_earth);
}

/* The water's points (16 bytes each, see the bake's layout) and its u32 tables. */
static int read_water_points(FILE *f, EarthWater *w) {
    static unsigned char buf[16 * 4096];
    for (uint32_t k = 0; k < w->V;) {
        uint32_t take = w->V - k < 4096 ? w->V - k : 4096;
        if (fread(buf, 16, take, f) != take) return 0;
        for (uint32_t i = 0; i < take; ++i, ++k) {
            const unsigned char *r = buf + 16 * i;
            w->xyz[3 * k] = get_f32(r); w->xyz[3 * k + 1] = get_f32(r + 4); w->xyz[3 * k + 2] = get_f32(r + 8);
            w->width[k] = get_u16(r + 12); w->kind[k] = r[14]; w->flags[k] = r[15];
        }
    }
    return 1;
}

static int read_u32s(FILE *f, uint32_t *v, size_t n) {
    static unsigned char buf[1 << 16];
    while (n > 0) {
        size_t take = n < sizeof buf / 4 ? n : sizeof buf / 4;
        if (fread(buf, 4, take, f) != take) return 0;
        for (size_t i = 0; i < take; ++i) v[i] = get_u32(buf + 4 * i);
        v += take; n -= take;
    }
    return 1;
}

/* A water section (the bake's layout), B, reach, V and E set: NULL, or what
 * is wrong with it. */
static const char *read_water(FILE *f, EarthWater *w) {
    size_t cells = 6 * (size_t)w->B * w->B;
    w->xyz = (float *)malloc((size_t)w->V * 3 * sizeof(float));
    w->width = (uint16_t *)malloc((size_t)w->V * sizeof(uint16_t));
    w->kind = (uint8_t *)malloc((size_t)w->V + 1); w->flags = (uint8_t *)malloc((size_t)w->V + 1);
    w->start = (uint32_t *)malloc((cells + 1) * sizeof(uint32_t)); w->inside = (uint8_t *)malloc(cells);
    w->entry = (uint32_t *)malloc((size_t)w->E * sizeof(uint32_t) + 1);
    if (!w->xyz || !w->width || !w->kind || !w->flags || !w->start || !w->inside || !w->entry) return "out of memory";
    if (!read_water_points(f, w) || !read_u32s(f, w->start, cells + 1) || fread(w->inside, 1, cells, f) != cells || !read_u32s(f, w->entry, w->E))
        return "truncated";
    if (w->start[cells] != w->E) return "water buckets that don't add up";
    for (uint32_t e = 0; e < w->E; ++e)
        if (w->entry[e] + 1 >= w->V || !(w->flags[w->entry[e]] & 1)) return "a water bucket lists no segment";
    return NULL;
}

/* Sample (i, j) of a face of n a side, as the bake places it. */
static TerrainDir sample_dir(int face, int n, int i, int j) {
    return terrain_face_dir(face, (double)(2 * i + 1) / n - 1.0, (double)(2 * j + 1) / n - 1.0);
}

static double dir_dist(TerrainDir a, TerrainDir b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return PLANET_RADIUS * sqrt(dx * dx + dy * dy + dz * dz);
}

/* Folds the apron's samples, and the grades between each and its next along
 * u and along v, into the border block beside them (the block of the sample
 * clamped into the interior): the stencil of any point in that block, or in
 * the blend band past the edge, reads them. The interior's own steps to
 * their next are in the bake's tables already. */
static void fold_apron(EarthMip *m, int face) {
    int n = m->n, g = m->g, nb = m->nb;
    int16_t *q = m->q[face];
    for (int j = -APRON; j < n + APRON; ++j) {
        int interior_row = j >= 0 && j < n;
        for (int i = -APRON; i < n + APRON; ++i) {
            if (interior_row && i == 0) i = n;   /* past the interior to the far apron */
            int ci = i < 0 ? 0 : i >= n ? n - 1 : i, cj = j < 0 ? 0 : j >= n ? n - 1 : j;
            size_t b = (size_t)(cj / BLOCK) * nb + (ci / BLOCK);
            int h = q[(size_t)(j + APRON) * g + (i + APRON)];
            if (h < m->bmin[face][b]) m->bmin[face][b] = (int16_t)h;
            if (h > m->bmax[face][b]) m->bmax[face][b] = (int16_t)h;
            TerrainDir p = sample_dir(face, n, i, j);
            double grade = m->bslope[face][b];
            if (i + 1 < n + APRON) {
                int hu = q[(size_t)(j + APRON) * g + (i + 1 + APRON)];
                double gu = fabs((double)(hu - h)) / dir_dist(p, sample_dir(face, n, i + 1, j));
                if (gu > grade) grade = gu;
            }
            if (j + 1 < n + APRON) {
                int hv = q[(size_t)(j + 1 + APRON) * g + (i + APRON)];
                double gv = fabs((double)(hv - h)) / dir_dist(p, sample_dir(face, n, i, j + 1));
                if (gv > grade) grade = gv;
            }
            m->bslope[face][b] = (float)grade;
        }
    }
}

static int fail(FILE *f, const char *path, const char *why) {
    fprintf(stderr, "earth: %s: %s\n", path, why);
    if (f) fclose(f);
    terrain_earth_unload();
    return 0;
}

static int load_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hd[EARTH_HEADER];
    if (fread(hd, 1, EARTH_HEADER, f) != EARTH_HEADER || memcmp(hd, EARTH_MAGIC, 8) != 0)
        return fail(f, path, "not an Earth bake");
    if (get_u32(hd + 8) != EARTH_VERSION) return fail(f, path, "an older Earth bake: bake it again (cmake --build build --target earth)");
    int N = (int)get_u32(hd + 12), mips = (int)get_u32(hd + 20);
    if (get_u32(hd + 16) != APRON || get_u32(hd + 24) != BLOCK || mips < 1 || mips > MAX_MIPS || N < MIN_FACE || (N & (N - 1)) != 0 || (N >> (mips - 1)) % BLOCK != 0)
        return fail(f, path, "an apron, block, size or mip count this build can't read");
    g_earth.N = N; g_earth.mips = mips;
    g_earth.scale = get_f32(hd + 32); g_earth.offset = get_f32(hd + 36);
    g_earth.origin_lat = get_f64(hd + 40); g_earth.origin_lon = get_f64(hd + 48); g_earth.origin_heading = get_f64(hd + 56);
    if (fseek(f, 16L * mips, SEEK_CUR) != 0) return fail(f, path, "truncated");   /* the mip table: recomputed below, apron included */
    for (int k = 0; k < mips; ++k) {
        EarthMip *m = &g_earth.mip[k];
        m->n = N >> k; m->g = m->n + 2 * APRON; m->nb = m->n / BLOCK;
        m->step = PLANET_RADIUS * (PI / 2.0) / m->n;
        size_t cells = (size_t)m->g * m->g, blocks = (size_t)m->nb * m->nb;
        for (int face = 0; face < 6; ++face) {
            m->q[face] = (int16_t *)malloc(cells * sizeof(int16_t));
            m->bmin[face] = (int16_t *)malloc(blocks * sizeof(int16_t));
            m->bmax[face] = (int16_t *)malloc(blocks * sizeof(int16_t));
            m->bslope[face] = (float *)malloc(blocks * sizeof(float));
            m->brelief[face] = (float *)malloc(blocks * sizeof(float));
            if (!m->q[face] || !m->bmin[face] || !m->bmax[face] || !m->bslope[face] || !m->brelief[face]) return fail(f, path, "out of memory");
            if (!read_i16s(f, m->q[face], cells) || !read_i16s(f, m->bmin[face], blocks) || !read_i16s(f, m->bmax[face], blocks) ||
                !read_f32s(f, m->bslope[face], blocks) || !read_f32s(f, m->brelief[face], blocks))
                return fail(f, path, "truncated");
            fold_apron(m, face);
        }
        m->lo = 1e30; m->hi = -1e30; m->grade = 0.0;
        for (int face = 0; face < 6; ++face) {
            for (size_t b = 0; b < blocks; ++b) {
                if (m->bmin[face][b] < m->lo) m->lo = m->bmin[face][b];
                if (m->bmax[face][b] > m->hi) m->hi = m->bmax[face][b];
                if (m->bslope[face][b] > m->grade) m->grade = m->bslope[face][b];
            }
        }
        m->lo = g_earth.scale * m->lo + g_earth.offset;
        m->hi = g_earth.scale * m->hi + g_earth.offset;
        m->grade *= g_earth.scale;
    }
    EarthLand *l = &g_earth.land;
    l->mip = (int)get_u32(hd + 220); l->hints = (get_u32(hd + 224) & 1u) != 0;
    if (l->mip >= mips) return fail(f, path, "a landform grid past the mips");
    l->n = N >> l->mip; l->g = l->n + 2 * APRON;
    size_t lcells = (size_t)l->g * l->g;
    l->rough_max = 1.0;
    for (int face = 0; face < 6; ++face) {
        l->relief[face] = (int16_t *)malloc(lcells * sizeof(int16_t));
        l->se[face] = (int8_t *)malloc(lcells);
        l->sn[face] = (int8_t *)malloc(lcells);
        if (!l->relief[face] || !l->se[face] || !l->sn[face]) return fail(f, path, "out of memory");
        if (!read_i16s(f, l->relief[face], lcells) || fread(l->se[face], 1, lcells, f) != lcells || fread(l->sn[face], 1, lcells, f) != lcells)
            return fail(f, path, "truncated");
        if (!l->hints) continue;
        l->override[face] = (uint8_t *)malloc(lcells);
        l->rough[face] = (uint8_t *)malloc(lcells);
        l->nosettle[face] = (uint8_t *)malloc(lcells);
        if (!l->override[face] || !l->rough[face] || !l->nosettle[face]) return fail(f, path, "out of memory");
        if (fread(l->override[face], 1, lcells, f) != lcells || fread(l->rough[face], 1, lcells, f) != lcells || fread(l->nosettle[face], 1, lcells, f) != lcells)
            return fail(f, path, "truncated");
        for (size_t k = 0; k < lcells; ++k)
            if (l->rough[face][k] / 128.0 > l->rough_max) l->rough_max = l->rough[face][k] / 128.0;
    }
    EarthWater *w = &g_earth.water;
    w->B = (int)get_u32(hd + 232); w->reach = get_f32(hd + 236); w->V = get_u32(hd + 240); w->E = get_u32(hd + 244);
    if (w->B) {
        const char *why = read_water(f, w);
        if (why) return fail(f, path, why);
        size_t cells = 6 * (size_t)w->B * w->B;
        if (!(w->shores = (uint8_t *)calloc(cells, 1))) return fail(f, path, "out of memory");
        for (size_t b = 0; b < cells; ++b)
            for (uint32_t e = w->start[b]; e < w->start[b + 1] && !w->shores[b]; ++e) w->shores[b] = w->kind[w->entry[e]] == 2;
    }
    if (fgetc(f) != EOF) return fail(f, path, "overlong");
    fclose(f);
    snprintf(g_earth.path, sizeof g_earth.path, "%s", path);
    memcpy(g_earth.stage_sha, hd + 128, 64);
    g_earth.stage_sha[64] = 0;
    g_earth.loaded = 1;
    return 1;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

/* The chart about the origin, turned by the heading. */
static void set_chart(void) {
    g_earth.chart = terrain_chart_about(terrain_latlon_dir(g_earth.origin_lat, g_earth.origin_lon), g_earth.origin_heading);
}

/* The bake beside the executable, in the working directory, or in build/ under it. */
static int exe_dir(char *out, size_t cap) {
    char exe[1024] = "";
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (len == 0 || len >= sizeof exe) exe[0] = 0;
#else
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[len > 0 ? len : 0] = 0;
#endif
    char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
    if (bs > slash) slash = bs;
    if (!slash) return 0;
    slash[1] = 0;
    snprintf(out, cap, "%s", exe);
    return 1;
}

static int streams_refused(FILE *f, const char *path, const char *why) {
    fprintf(stderr, "earth: streams %s: %s; the bake's rivers and lakes stand without them, and carve no bed\n", path, why);
    if (f) fclose(f);
    streams_free();
    return 0;
}

/* The streams' file: refused if another bake made it, or if the land at its
 * probes isn't what it was when it was made (the land's code has changed);
 * its plan's probes wait for the settlements. */
static int load_streams(const char *path) {
    streams_free();
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("earth: no streams %s (cmake --build build --target earth derives them)\n", path);
        return 0;
    }
    unsigned char hd[STREAMS_HEADER];
    if (fread(hd, 1, STREAMS_HEADER, f) != STREAMS_HEADER || memcmp(hd, STREAMS_MAGIC, 8) != 0) return streams_refused(f, path, "not a streams file");
    if (get_u32(hd + 8) != STREAMS_VERSION) return streams_refused(f, path, "an older streams file: derive them again (cmake --build build --target earth)");
    if (memcmp(hd + 32, g_earth.stage_sha, 64) != 0) return streams_refused(f, path, "derived from another bake: derive them again (cmake --build build --target earth)");
    double cell = get_f32(hd + 16), worst = 0.0;
    uint32_t probes = get_u32(hd + 28);
    if (probes > STREAMS_PROBES) return streams_refused(f, path, "too many probes");
    for (uint32_t k = 0; k < probes; ++k) {
        const unsigned char *r = hd + 120 + 16 * k;
        TerrainDir d = { get_f32(r), get_f32(r + 4), get_f32(r + 8) };
        double lift, h = terrain_land_parts(unit3(d), TERRAIN_WILD_EDGE, cell, &lift);
        worst = fmax(worst, fabs(h - get_f32(r + 12)));
    }
    if (worst > STREAMS_PROBE_TOLERANCE) return streams_refused(f, path, "the land has changed since they were derived: derive them again (cmake --build build --target earth)");
    EarthWater *w = &g_earth.streams;
    w->B = (int)get_u32(hd + 96); w->reach = get_f32(hd + 100); w->V = get_u32(hd + 104); w->E = get_u32(hd + 108);
    if (!w->B || w->V < 2) return streams_refused(f, path, "empty");
    const char *why = read_water(f, w);
    if (why) return streams_refused(f, path, why);
    w->bed = (float *)malloc((size_t)w->V * sizeof(float));
    w->flow = (float *)malloc((size_t)w->V * sizeof(float));
    if (!w->bed || !w->flow) return streams_refused(f, path, "out of memory");
    if (!read_f32s(f, w->bed, w->V) || !read_f32s(f, w->flow, w->V)) return streams_refused(f, path, "truncated");
    /* the plan's probes, then the bake's points' beds and the lakes' levels by bucket */
    unsigned char nb[4];
    uint32_t nplan = get_u32(hd + 116);
    if (nplan < 1 || nplan > STREAMS_PLAN_MAX) return streams_refused(f, path, "no plan probes, or too many");
    g_earth.nplan = nplan;
    g_earth.plan_probe = (float *)malloc((size_t)nplan * 4 * sizeof(float));
    if (!g_earth.plan_probe) return streams_refused(f, path, "out of memory");
    if (!read_f32s(f, g_earth.plan_probe, (size_t)nplan * 4) || fread(nb, 1, 4, f) != 4) return streams_refused(f, path, "truncated");
    EarthWater *bw = &g_earth.water;
    if (get_u32(nb) != bw->V) return streams_refused(f, path, "made for other water: derive them again (cmake --build build --target earth)");
    if (bw->V) {
        if (!(bw->bed = (float *)malloc((size_t)bw->V * sizeof(float)))) return streams_refused(f, path, "out of memory");
        if (!read_f32s(f, bw->bed, bw->V)) return streams_refused(f, path, "truncated");
    }
    if (fread(nb, 1, 4, f) != 4) return streams_refused(f, path, "truncated");
    g_earth.nlevels = get_u32(nb);
    if (g_earth.nlevels > 6u * (uint32_t)bw->B * (uint32_t)bw->B) return streams_refused(f, path, "too many lakes' buckets");
    g_earth.level_bucket = (uint32_t *)malloc((size_t)g_earth.nlevels * sizeof(uint32_t) + 4);
    g_earth.level = (float *)malloc((size_t)g_earth.nlevels * sizeof(float) + 4);
    if (!g_earth.level_bucket || !g_earth.level) return streams_refused(f, path, "out of memory");
    if (!read_u32s(f, g_earth.level_bucket, g_earth.nlevels) || !read_f32s(f, g_earth.level, g_earth.nlevels)) return streams_refused(f, path, "truncated");
    for (uint32_t k = 1; k < g_earth.nlevels; ++k)
        if (g_earth.level_bucket[k] <= g_earth.level_bucket[k - 1]) return streams_refused(f, path, "lakes' buckets out of order");
    if (g_earth.nlevels && g_earth.level_bucket[g_earth.nlevels - 1] >= 6u * (uint32_t)bw->B * (uint32_t)bw->B) return streams_refused(f, path, "a lake's bucket off the faces");
    if (fgetc(f) != EOF) return streams_refused(f, path, "overlong");
    fclose(f);
    snprintf(g_earth.streams_path, sizeof g_earth.streams_path, "%s", path);
    printf("earth: streams %s, %u links, %u points (%.0f m grid, from %.2f m3/s), with the water's beds\n", path, get_u32(hd + 112), w->V, cell,
           get_f32(hd + 20));
    return 1;
}

void terrain_earth_check_streams(void) {
    if (!g_earth.loaded || !g_earth.streams.B || g_earth.plan_checked) return;
    g_earth.plan_checked = 1;
    char why[256];
    int n = plan_settlement_count();
    if ((int)g_earth.plan_probe[0] != n) {
        snprintf(why, sizeof why, "derived with %d settlements, not %d: derive them again (cmake --build build --target earth)", (int)g_earth.plan_probe[0], n);
        streams_refused(NULL, g_earth.streams_path, why);
        return;
    }
    TerrainChart wc = terrain_world_chart();
    for (uint32_t k = 1; k < g_earth.nplan; ++k) {
        const float *r = g_earth.plan_probe + 4 * k;
        TerrainDir d = { r[0], r[1], r[2] };
        double x, z;
        terrain_dir_chart(&wc, unit3(d), &x, &z);
        double e = plan_city_edge(x, z);
        if (fabs(e - r[3]) <= STREAMS_PLAN_TOLERANCE) continue;
        snprintf(why, sizeof why, "the settlements have changed since they were derived (the edge at (%.0f, %.0f) is %.2f m, not %.2f): derive them again "
                 "(cmake --build build --target earth)", x, z, e, r[3]);
        streams_refused(NULL, g_earth.streams_path, why);
        return;
    }
}

int terrain_earth_load_streams(const char *path) {
    if (!g_earth.loaded) return 0;
    if (path && !path[0]) { streams_free(); return 0; }
    if (path) return load_streams(path);
    char cand[1100];
    size_t n = strlen(g_earth.path);
    if (n >= 5 && !strcmp(g_earth.path + n - 5, ".bake")) snprintf(cand, sizeof cand, "%.*s.streams", (int)(n - 5), g_earth.path);
    else snprintf(cand, sizeof cand, "%s.streams", g_earth.path);
    return load_streams(cand);
}

int terrain_earth_load(const char *path) {
    terrain_earth_unload();
    int ok = 0;
    if (path) ok = load_file(path);
    else {
        const char *env = getenv("NEONCITY_EARTH");
        char dir[1024], cand[1200];
        if (env && env[0]) ok = load_file(env);
        if (!ok && exe_dir(dir, sizeof dir)) { snprintf(cand, sizeof cand, "%searth.bake", dir); ok = load_file(cand); }
        if (!ok) ok = load_file("earth.bake");
        if (!ok) ok = load_file("build/earth.bake");
    }
    if (ok) {
        set_chart();
        g_earth.datum = terrain_earth_height(g_earth.chart.o, TERRAIN_LIFT_SPACING);
        const EarthMip *m0 = &g_earth.mip[0];
        printf("earth: %s, %d a side (%.0f m), %d mips, %.0f to %.0f m, grade %.2f; origin %.4f %.4f heading %.1f, datum %.1f m; landforms %d a side%s\n",
               g_earth.path, g_earth.N, m0->step, g_earth.mips, m0->lo, m0->hi, m0->grade, g_earth.origin_lat, g_earth.origin_lon, g_earth.origin_heading,
               g_earth.datum, g_earth.land.n, g_earth.land.hints ? ", with hints" : "");
        terrain_earth_load_streams(NULL);
    } else {
        fprintf(stderr, "earth: no bake %s (cmake --build build --target earth makes build/earth.bake, see data/earth/SOURCE.md); the procedural macro field stands in\n",
                path ? path : "found (NEONCITY_EARTH, earth.bake by the executable or here, build/earth.bake)");
    }
    return ok;
}

int terrain_earth_loaded(void) { return g_earth.loaded; }

int terrain_earth_chart(TerrainChart *c) {
    if (!g_earth.loaded) return 0;
    *c = g_earth.chart;
    return 1;
}

double terrain_earth_datum(void) { return g_earth.loaded ? g_earth.datum : 0.0; }

double terrain_earth_rough_max(void) { return g_earth.loaded ? g_earth.land.rough_max : 1.0; }

void terrain_earth_origin(double *lat, double *lon, double *heading) {
    if (g_earth.loaded) { *lat = g_earth.origin_lat; *lon = g_earth.origin_lon; *heading = g_earth.origin_heading; return; }
    terrain_dir_latlon(terrain_world_chart().o, lat, lon);
    *heading = 0.0;
}

_Static_assert(TERRAIN_MACRO_OCTAVES == 4, "the stand-in's grade takes its octave weights to sum to 1.875");

void terrain_earth_extremes(double *lo, double *hi, double *grade) {
    if (g_earth.loaded) { *lo = g_earth.mip[0].lo; *hi = g_earth.mip[0].hi; *grade = g_earth.mip[0].grade; return; }
    *lo = -TERRAIN_MACRO_M; *hi = TERRAIN_MACRO_M;
    *grade = TERRAIN_MACRO_M * 3.0 * 1.7320508075688772 * TERRAIN_MACRO_OCTAVES / (1.875 * TERRAIN_MACRO_WAVE);   /* value noise of wavelength L: 3 sqrt(3) / L a metre an octave */
}

/* --- Sampling ------------------------------------------------------------------------ */

/* The level of detail for a spacing: mips `m` and `m + 1` blended by `t`
 * (t = 0 at the coarsest mip, which has no finer partner). */
static void mips_for(double spacing, int *m, double *t) {
    double l = spacing > 0.0 ? log2(spacing / g_earth.mip[0].step) + 1.0 : 0.0;
    if (!(l > 0.0)) { *m = 0; *t = 0.0; return; }
    if (l >= g_earth.mips - 1) { *m = g_earth.mips - 1; *t = 0.0; return; }
    *m = (int)floor(l);
    *t = l - *m;
}

/* The coarsest mip read at `spacing` or any finer one. */
static int mip_to(double spacing) { int m; double t; mips_for(spacing, &m, &t); return t > 0.0 ? m + 1 : m; }

/* The mip and the blend, t in 2^-24ths: spacings that share them read the
 * same heights (and two that differ by less than that are never asked for). */
uint32_t terrain_earth_lod_key(double spacing) {
    if (!g_earth.loaded) return 0;
    int m; double t;
    mips_for(spacing, &m, &t);
    if (m == 0 && t == 0.0) return 0;
    return (uint32_t)(m + 1) << 24 | (uint32_t)(t * 16777216.0);
}

static double comp(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

/* A direction in every face's coordinates: as terrain_dir_face computes
 * them for the face holding it, and past the edges for the faces beside;
 * `front` where the face's axis points its way at all, and `e` how far in
 * from the face's edge it lies (negative past it). */
typedef struct FaceUV { int front; double u, v, e; } FaceUV;

static void face_uvs(TerrainDir p, FaceUV uv[6]) {
    for (int face = 0; face < 6; ++face) {
        int a = face / 2;
        double c = comp(p, a);
        uv[face].front = (face & 1) ? c < 0.0 : c > 0.0;
        if (!uv[face].front) { uv[face].u = uv[face].v = uv[face].e = -1e30; continue; }
        double n = fabs(c);
        uv[face].u = atan(comp(p, (a + 1) % 3) / n) * (4.0 / PI);
        uv[face].v = atan(comp(p, (a + 2) % 3) / n) * (4.0 / PI);
        uv[face].e = 1.0 - fmax(fabs(uv[face].u), fabs(uv[face].v));
    }
}

static void cr_weights(double t, double w[4]) {
    double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t3 + 2.0 * t2 - t);
    w[1] = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
    w[2] = 0.5 * (-3.0 * t3 + 4.0 * t2 + t);
    w[3] = 0.5 * (t3 - t2);
}

/* Face coordinate u to the sample coordinate x: sample i's centre is x = i. */
static double sample_x(const EarthMip *m, double u) { return (u + 1.0) * (0.5 * m->n) - 0.5; }

/* The bicubic over the 4 x 4 samples round (u, v), |u| and |v| under
 * 1 + 1 / n, in sample units. */
static double face_sample(const EarthMip *m, int face, double u, double v) {
    double x = sample_x(m, u), y = sample_x(m, v), fx = floor(x), fy = floor(y);
    int i0 = (int)fx, j0 = (int)fy, n = m->n, g = m->g;
    i0 = i0 < -1 ? -1 : i0 > n - 1 ? n - 1 : i0;
    j0 = j0 < -1 ? -1 : j0 > n - 1 ? n - 1 : j0;
    double wx[4], wy[4];
    cr_weights(x - fx, wx);
    cr_weights(y - fy, wy);
    const int16_t *q = m->q[face] + (size_t)(j0 - 1 + APRON) * g + (i0 - 1 + APRON);
    double s = 0.0;
    for (int jj = 0; jj < 4; ++jj) {
        const int16_t *row = q + (size_t)jj * g;
        s += wy[jj] * (wx[0] * row[0] + wx[1] * row[1] + wx[2] * row[2] + wx[3] * row[3]);
    }
    return s;
}

/* A face's weight at `e` in from its edge, for a mip whose blend band is
 * `band` either side of the edge: 1 inside, 0 past the band, a smoothstep
 * between, and 1/2 on the edge from both sides. */
static double face_weight(double e, double band) {
    if (e >= band) return 1.0;
    if (e <= -band) return 0.0;
    double t = (e + band) / (2.0 * band);
    return t * t * (3.0 - 2.0 * t);
}

/* The mip's height at a direction (sample units): the faces in reach,
 * weighted by their distance in from their edges. */
static double mip_height(const EarthMip *m, const FaceUV uv[6]) {
    double band = 1.0 / m->n, sum = 0.0, wsum = 0.0;
    for (int face = 0; face < 6; ++face) {
        if (!uv[face].front) continue;
        double w = face_weight(uv[face].e, band);
        if (w <= 0.0) continue;
        sum += w * face_sample(m, face, uv[face].u, uv[face].v);
        wsum += w;
    }
    return sum / wsum;
}

double terrain_earth_height(TerrainDir p, double spacing) {
    if (!g_earth.loaded) return TERRAIN_MACRO_M * terrain_macro(p, spacing);
    FaceUV uv[6];
    face_uvs(p, uv);
    int m; double t;
    mips_for(spacing, &m, &t);
    double h = mip_height(&g_earth.mip[m], uv);
    if (t > 0.0) h = (1.0 - t) * h + t * mip_height(&g_earth.mip[m + 1], uv);
    return g_earth.scale * h + g_earth.offset;
}

/* --- The landform grid ------------------------------------------------------------- */

/* The 4 x 4 samples a Catmull-Rom reads round (u, v) on a grid of n a side
 * (as face_sample), and their weights. */
typedef struct Taps { size_t at[16]; double w[16]; } Taps;

static void grid_taps(int n, int g, double u, double v, Taps *t) {
    double x = (u + 1.0) * (0.5 * n) - 0.5, y = (v + 1.0) * (0.5 * n) - 0.5, fx = floor(x), fy = floor(y);
    int i0 = (int)fx, j0 = (int)fy;
    i0 = i0 < -1 ? -1 : i0 > n - 1 ? n - 1 : i0;
    j0 = j0 < -1 ? -1 : j0 > n - 1 ? n - 1 : j0;
    double wx[4], wy[4];
    cr_weights(x - fx, wx);
    cr_weights(y - fy, wy);
    for (int jj = 0; jj < 4; ++jj)
        for (int ii = 0; ii < 4; ++ii) {
            t->at[jj * 4 + ii] = (size_t)(j0 - 1 + jj + APRON) * g + (size_t)(i0 - 1 + ii + APRON);
            t->w[jj * 4 + ii] = wx[ii] * wy[jj];
        }
}

int terrain_earth_land(TerrainDir p, TerrainEarthLand *out) {
    memset(out, 0, sizeof *out);
    out->rough = 1.0;
    if (!g_earth.loaded) return 0;
    const EarthLand *l = &g_earth.land;
    FaceUV uv[6];
    face_uvs(p, uv);
    double band = 1.0 / l->n, wsum = 0.0, relief = 0.0, se = 0.0, sn = 0.0, rough = 0.0, nosettle = 0.0, hint[TERRAIN_TYPES] = { 0.0 };
    for (int face = 0; face < 6; ++face) {
        if (!uv[face].front) continue;
        double fw = face_weight(uv[face].e, band);
        if (fw <= 0.0) continue;
        Taps t;
        grid_taps(l->n, l->g, uv[face].u, uv[face].v, &t);
        double r = 0.0, a = 0.0, b = 0.0;
        for (int k = 0; k < 16; ++k) {
            r += t.w[k] * l->relief[face][t.at[k]];
            a += t.w[k] * l->se[face][t.at[k]];
            b += t.w[k] * l->sn[face][t.at[k]];
        }
        relief += fw * r; se += fw * a; sn += fw * b;
        if (l->hints) {
            /* the override as each type's share: its samples' weights */
            double ro = 0.0, no = 0.0;
            for (int k = 0; k < 16; ++k) {
                int o = l->override[face][t.at[k]];
                if (o >= 1 && o <= TERRAIN_TYPES) hint[o - 1] += fw * t.w[k];
                ro += t.w[k] * l->rough[face][t.at[k]];
                no += t.w[k] * l->nosettle[face][t.at[k]];
            }
            rough += fw * ro; nosettle += fw * no;
        }
        wsum += fw;
    }
    /* A Catmull-Rom can overshoot its samples, so each is clamped to its range. */
    out->relief = fmax(0.0, relief / wsum);
    se /= 127.0 * wsum; sn /= 127.0 * wsum;
    double len = hypot(se, sn);
    if (len > 1.0) { se /= len; sn /= len; }
    out->strike_e = se; out->strike_n = sn;
    if (l->hints) {
        double sum = 0.0;
        for (int t = 0; t < TERRAIN_TYPES; ++t) { hint[t] = fmin(1.0, fmax(0.0, hint[t] / wsum)); sum += hint[t]; }
        for (int t = 0; t < TERRAIN_TYPES; ++t) out->hint[t] = sum > 1.0 ? hint[t] / sum : hint[t];
        out->rough = fmin(l->rough_max, fmax(0.0, rough / (128.0 * wsum)));
        out->nosettle = fmin(1.0, fmax(0.0, nosettle / (255.0 * wsum)));
    }
    return 1;
}

/* --- Bounds ---------------------------------------------------------------------- */

typedef struct Bound { double lo, hi, slack; } Bound;

/* Widens the bound by a mip's blocks covering the samples any stencil
 * centred at sample coordinates [x0, x1] x [y0, y1] of a face reads (one
 * before, two after), with the overshoot the steepest of them allows. */
static void box_bound(const EarthMip *m, int face, double x0, double x1, double y0, double y1, Bound *b) {
    int n = m->n, nb = m->nb, g = m->g;
    int i0 = (int)floor(x0) - 1, i1 = (int)floor(x1) + 2, j0 = (int)floor(y0) - 1, j1 = (int)floor(y1) + 2;
    i0 = i0 < -APRON ? -APRON : i0; i1 = i1 > n + APRON - 1 ? n + APRON - 1 : i1;
    j0 = j0 < -APRON ? -APRON : j0; j1 = j1 > n + APRON - 1 ? n + APRON - 1 : j1;
    if (i1 < i0 || j1 < j0) return;
    if ((i1 - i0 + 1) * (j1 - j0 + 1) <= DIRECT_SAMPLES) {
        /* Few samples: their own range and steps, exactly. */
        int step = 0;
        for (int j = j0; j <= j1; ++j) {
            const int16_t *row = m->q[face] + (size_t)(j + APRON) * g + APRON;
            for (int i = i0; i <= i1; ++i) {
                int h = row[i];
                if (h < b->lo) b->lo = h;
                if (h > b->hi) b->hi = h;
                if (i < i1 && abs(row[i + 1] - h) > step) step = abs(row[i + 1] - h);
                if (j < j1 && abs(row[i + g] - h) > step) step = abs(row[i + g] - h);
            }
        }
        double slack = OVERSHOOT * step;
        if (slack > b->slack) b->slack = slack;
        return;
    }
    i0 = i0 < 0 ? 0 : i0 > n - 1 ? n - 1 : i0; i1 = i1 < 0 ? 0 : i1 > n - 1 ? n - 1 : i1;
    j0 = j0 < 0 ? 0 : j0 > n - 1 ? n - 1 : j0; j1 = j1 < 0 ? 0 : j1 > n - 1 ? n - 1 : j1;
    double grade = 0.0;
    for (int bj = j0 / BLOCK; bj <= j1 / BLOCK; ++bj) {
        for (int bi = i0 / BLOCK; bi <= i1 / BLOCK; ++bi) {
            size_t k = (size_t)bj * nb + bi;
            if (m->bmin[face][k] < b->lo) b->lo = m->bmin[face][k];
            if (m->bmax[face][k] > b->hi) b->hi = m->bmax[face][k];
            if (m->bslope[face][k] > grade) grade = m->bslope[face][k];
        }
    }
    double slack = OVERSHOOT * grade * m->step;
    if (slack > b->slack) b->slack = slack;
}

/* The bound round one direction at a mip: the faces whose weight there is
 * not 0. */
static void point_bound(const EarthMip *m, const FaceUV uv[6], Bound *b) {
    double band = 1.0 / m->n;
    for (int face = 0; face < 6; ++face) {
        if (!uv[face].front || uv[face].e <= -band) continue;
        double x = sample_x(m, uv[face].u), y = sample_x(m, uv[face].v);
        box_bound(m, face, x, x, y, y, b);
    }
}

/* The face coordinates a cap of angular radius `alpha` round p reaches on
 * face `face`, along its coordinate `k` (0 u, 1 v), within the face's
 * extent of `reach` past 1: the cap meets the plane of coordinate angle
 * theta (through the face's other axis) where |rho sin(phi - theta)| <=
 * sin alpha, rho and phi the direction's length and angle in the plane of
 * the face's axis and the coordinate's; a plane is met at angle theta or
 * at theta + pi, and a cap wholly in front of the face (further from the
 * face's own plane than its radius) only at the angle on its own side.
 * Returns 0 if it reaches none. */
static int cap_range(TerrainDir p, double alpha, int face, int k, double reach, double *lo, double *hi) {
    int a = face / 2, c = (a + 1 + k) % 3;
    double s = (face & 1) ? -1.0 : 1.0, pa = s * comp(p, a), pc = comp(p, c);
    double rho = hypot(pa, pc), sa = sin(alpha), amax = reach * (PI / 4.0);
    if (alpha < 0.5 * PI && pa <= 0.0 && sa <= -pa) return 0;   /* wholly behind the face */
    if (alpha >= 0.5 * PI || rho <= sa) { *lo = -reach; *hi = reach; return 1; }
    double phi = atan2(pc, pa), beta = asin(sa / rho);
    int halves = pa > 0.0 && sa < pa ? 1 : 2;
    *lo = 1e30; *hi = -1e30;
    for (int half = 0; half < halves; ++half) {
        double c0 = phi + half * PI;
        if (c0 > PI) c0 -= 2.0 * PI;
        double l = fmax(c0 - beta, -amax), h = fmin(c0 + beta, amax);
        if (l > h) continue;
        if (l < *lo) *lo = l;
        if (h > *hi) *hi = h;
    }
    if (*lo > *hi) return 0;
    *lo *= 4.0 / PI; *hi *= 4.0 / PI;
    return 1;
}

/* The bound over a cap at a mip: each face's blocks under the cap's
 * coordinates there, out to the blend band past its edges. */
static void cap_bound(const EarthMip *m, TerrainDir p, double alpha, Bound *b) {
    double reach = 1.0 + 1.0 / m->n;
    for (int face = 0; face < 6; ++face) {
        double u0, u1, v0, v1;
        if (!cap_range(p, alpha, face, 0, reach, &u0, &u1) || !cap_range(p, alpha, face, 1, reach, &v0, &v1)) continue;
        box_bound(m, face, sample_x(m, u0), sample_x(m, u1), sample_x(m, v0), sample_x(m, v1), b);
    }
}

static void bound_out(const Bound *b, double *lo, double *hi) {
    *lo = g_earth.scale * b->lo + g_earth.offset - b->slack;
    *hi = g_earth.scale * b->hi + g_earth.offset + b->slack;
}

void terrain_earth_range(TerrainDir p, double radius, double spacing, double *lo, double *hi) {
    if (!g_earth.loaded) { *lo = -TERRAIN_MACRO_M; *hi = TERRAIN_MACRO_M; return; }
    double alpha = radius / PLANET_RADIUS;
    Bound b = { 1e30, -1e30, 0.0 };
    for (int m = 0, last = mip_to(spacing); m <= last; ++m) cap_bound(&g_earth.mip[m], p, alpha, &b);
    bound_out(&b, lo, hi);
}

/* The steepest grade (m a metre) between neighbouring samples any stencil
 * centred in sample coordinates [x0, x1] x [y0, y1] of a face reads: the
 * samples' own where there are few, else the blocks'. */
static double box_grade(const EarthMip *m, int face, double x0, double x1, double y0, double y1) {
    int n = m->n, nb = m->nb, g = m->g;
    int i0 = (int)floor(x0) - 1, i1 = (int)floor(x1) + 2, j0 = (int)floor(y0) - 1, j1 = (int)floor(y1) + 2;
    i0 = i0 < -APRON ? -APRON : i0; i1 = i1 > n + APRON - 1 ? n + APRON - 1 : i1;
    j0 = j0 < -APRON ? -APRON : j0; j1 = j1 > n + APRON - 1 ? n + APRON - 1 : j1;
    double grade = 0.0;
    if ((i1 - i0 + 1) * (j1 - j0 + 1) <= DIRECT_SAMPLES) {
        for (int j = j0; j <= j1; ++j) {
            const int16_t *row = m->q[face] + (size_t)(j + APRON) * g + APRON;
            for (int i = i0; i <= i1; ++i) {
                TerrainDir s = sample_dir(face, n, i, j);
                if (i < i1) grade = fmax(grade, abs(row[i + 1] - row[i]) / dir_dist(s, sample_dir(face, n, i + 1, j)));
                if (j < j1) grade = fmax(grade, abs(row[i + g] - row[i]) / dir_dist(s, sample_dir(face, n, i, j + 1)));
            }
        }
    } else {
        i0 = i0 < 0 ? 0 : i0 > n - 1 ? n - 1 : i0; i1 = i1 < 0 ? 0 : i1 > n - 1 ? n - 1 : i1;
        j0 = j0 < 0 ? 0 : j0 > n - 1 ? n - 1 : j0; j1 = j1 < 0 ? 0 : j1 > n - 1 ? n - 1 : j1;
        for (int bj = j0 / BLOCK; bj <= j1 / BLOCK; ++bj)
            for (int bi = i0 / BLOCK; bi <= i1 / BLOCK; ++bi) grade = fmax(grade, m->bslope[face][(size_t)bj * nb + bi]);
    }
    return grade * g_earth.scale;
}

/* The height at a spacing blends its two mips' heights, so lies between
 * their bounds. Where the cap lies on one face alone (clear of the seams'
 * blend), it also lies within the height at p plus or minus the radius
 * times the steepest the interpolation can climb: a Catmull-Rom's slope
 * along an axis is at most 2 of its largest step a sample (at t = 1/2,
 * where the tangent terms peak), and the weights across it sum to at most
 * 1.25 in magnitude, so each axis climbs at most 2.5 times the steepest
 * grade between the samples, and the slope both ways at most sqrt 2 times
 * that; the blend of the two mips no more than the steeper. A little over,
 * for the samples' spacing changing across a face. */
#define CR_CLIMB (2.5 * 1.4142135623730951 * 1.05)
void terrain_earth_range_at(TerrainDir p, double radius, double spacing, double *lo, double *hi) {
    if (!g_earth.loaded) { *lo = -TERRAIN_MACRO_M; *hi = TERRAIN_MACRO_M; return; }
    double alpha = radius / PLANET_RADIUS;
    Bound b = { 1e30, -1e30, 0.0 };
    int m; double t;
    mips_for(spacing, &m, &t);
    cap_bound(&g_earth.mip[m], p, alpha, &b);
    if (t > 0.0) cap_bound(&g_earth.mip[m + 1], p, alpha, &b);
    bound_out(&b, lo, hi);
    int faces = 0, only = 0;
    double box[4];
    for (int face = 0; face < 6; ++face) {
        double reach = 1.0 + 1.0 / g_earth.mip[t > 0.0 ? m + 1 : m].n, u0, u1, v0, v1;
        if (!cap_range(p, alpha, face, 0, reach, &u0, &u1) || !cap_range(p, alpha, face, 1, reach, &v0, &v1)) continue;
        faces++;
        only = face;
        box[0] = u0; box[1] = u1; box[2] = v0; box[3] = v1;
    }
    if (faces != 1) return;
    double grade = 0.0;
    for (int k = m; k <= (t > 0.0 ? m + 1 : m); ++k) {
        const EarthMip *e = &g_earth.mip[k];
        grade = fmax(grade, box_grade(e, only, sample_x(e, box[0]), sample_x(e, box[1]), sample_x(e, box[2]), sample_x(e, box[3])));
    }
    double h = terrain_earth_height(p, spacing), reach = CR_CLIMB * grade * radius + 1e-6;
    *lo = fmax(*lo, h - reach);
    *hi = fmin(*hi, h + reach);
}

double terrain_earth_lod_error(TerrainDir p, double spacing) {
    if (!g_earth.loaded) return TERRAIN_MACRO_M * terrain_dropped(TERRAIN_MACRO_WAVE, TERRAIN_MACRO_OCTAVES, spacing);
    int m; double t;
    mips_for(spacing, &m, &t);
    if (m == 0 && t == 0.0) return 0.0;
    FaceUV uv[6];
    face_uvs(p, uv);
    /* Full detail and the mips in play each lie within their own stencil's
     * bound round p, so their difference is at most the union's width. */
    Bound b = { 1e30, -1e30, 0.0 };
    point_bound(&g_earth.mip[0], uv, &b);
    point_bound(&g_earth.mip[m], uv, &b);
    if (t > 0.0) point_bound(&g_earth.mip[m + 1], uv, &b);
    double lo, hi;
    bound_out(&b, &lo, &hi);
    return hi - lo;
}

/* --- The water ---------------------------------------------------------------------- */

static TerrainDir water_point(const EarthWater *w, uint32_t k) {
    TerrainDir p = { w->xyz[3 * k], w->xyz[3 * k + 1], w->xyz[3 * k + 2] };
    return p;
}

/* The distance (chord, on the unit sphere) from p to the arc a b: to the
 * point of the chord nearest p, taken out to the sphere; and in *t where that
 * point is along the chord, 0 at a and 1 at b. The bake measures the same way. */
static double arc_dist_t(TerrainDir p, TerrainDir a, TerrainDir b, double *t_out) {
    double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    double px = p.x - a.x, py = p.y - a.y, pz = p.z - a.z;
    double dd = dx * dx + dy * dy + dz * dz;
    double t = dd > 0.0 ? (px * dx + py * dy + pz * dz) / dd : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    double cx = a.x + t * dx, cy = a.y + t * dy, cz = a.z + t * dz, len = sqrt(cx * cx + cy * cy + cz * cz);
    if (len > 0.0) { cx /= len; cy /= len; cz /= len; }
    *t_out = t;
    return sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) + (p.z - cz) * (p.z - cz));
}

static double det3(TerrainDir a, TerrainDir b, TerrainDir c) {
    return a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
}

/* Whether the short arcs c p and a b cross. */
static int arcs_cross(TerrainDir c, TerrainDir p, TerrainDir a, TerrainDir b) {
    return (det3(a, b, c) > 0.0) != (det3(a, b, p) > 0.0) && (det3(c, p, a) > 0.0) != (det3(c, p, b) > 0.0);
}

/* The bucket p lies in, and its centre. */
static size_t water_bucket(const EarthWater *w, TerrainDir p, TerrainDir *centre) {
    double u, v;
    int B = w->B, f = terrain_dir_face(p, &u, &v);
    int i = (int)floor((u + 1.0) * 0.5 * B), j = (int)floor((v + 1.0) * 0.5 * B);
    i = i < 0 ? 0 : i >= B ? B - 1 : i;
    j = j < 0 ? 0 : j >= B ? B - 1 : j;
    *centre = terrain_face_dir(f, -1.0 + (2.0 * i + 1.0) / B, -1.0 + (2.0 * j + 1.0) / B);
    return ((size_t)f * B + j) * B + i;
}

/* What one set of water holds near p (unit), on the unit sphere: whether p
 * is in a lake, the nearest shore (within the reach), the segment of the
 * nearest the bucket lists (shore_best, however far: its lake is p's when p
 * is in one), the nearest
 * river's bank, centre line and segment (best; none past the reach); where
 * the set has beds, the channels' floor (world Y, m: the least over its
 * rivers' segments of the bed where p is nearest it, the bank rising at
 * TERRAIN_WATER_BANK beyond the half width) and each lake's level near with
 * its nearest shore (at most WATER_LEVELS lakes; a lake's every ring has the
 * same level). */
typedef struct WaterNear {
    int      inside, nlev;
    double   shore, shore_any, bank, centre, floor;
    uint32_t best, shore_best;
    size_t   bucket;
    double   lev[WATER_LEVELS], lev_d[WATER_LEVELS];
} WaterNear;

static void water_near(const EarthWater *w, TerrainDir p, double reach, WaterNear *n) {
    TerrainDir c;
    size_t b = water_bucket(w, p, &c);
    /* p is in a lake if the bucket's centre is, flipped by each shore the arc between them crosses */
    n->bucket = b;
    n->inside = w->inside[b];
    n->shore = n->bank = n->centre = reach;
    n->shore_any = n->floor = 1e30;
    n->best = n->shore_best = 0xffffffffu;
    n->nlev = 0;
    for (uint32_t e = w->start[b]; e < w->start[b + 1]; ++e) {
        uint32_t k = w->entry[e];
        TerrainDir a = water_point(w, k), q = water_point(w, k + 1);
        double t, d = arc_dist_t(p, a, q, &t);
        if (w->kind[k] == 2) {
            if (d < n->shore) n->shore = d;
            if (d < n->shore_any) { n->shore_any = d; n->shore_best = k; }
            if (arcs_cross(c, p, a, q)) n->inside ^= 1;
            if (!w->bed) continue;
            double L = w->bed[k];
            int m = 0;
            while (m < n->nlev && n->lev[m] != L) ++m;
            if (m == n->nlev) {
                if (m == WATER_LEVELS) continue;
                n->lev[m] = L; n->lev_d[m] = d; n->nlev++;
            } else if (d < n->lev_d[m]) n->lev_d[m] = d;
        } else {
            double hw = 0.5 * w->width[k] / PLANET_RADIUS;
            if (d - hw < n->bank) { n->bank = d - hw; n->centre = d; n->best = k; }
            if (w->bed) {
                double bed = w->bed[k] + t * (w->bed[k + 1] - w->bed[k]);
                double f = bed + TERRAIN_WATER_BANK * PLANET_RADIUS * fmax(0.0, d - hw);
                if (f < n->floor) n->floor = f;
            }
        }
    }
}

/* A lake's floor at a signed distance s (m) from its shore, negative in it:
 * its level, falling inwards at TERRAIN_LAKE_SLOPE to TERRAIN_LAKE_DEEP_M
 * below it, and rising outwards at TERRAIN_WATER_BANK. */
static double lake_floor(double level, double s) {
    return s >= 0.0 ? level + TERRAIN_WATER_BANK * s : level - fmin(TERRAIN_LAKE_DEEP_M, -TERRAIN_LAKE_SLOPE * s);
}

/* The level of the lake whose bucket b lies in, where no shore comes within
 * the reach of it; 1e30 where the streams' file gave none. */
static double bucket_level(size_t b) {
    uint32_t lo = 0, hi = g_earth.nlevels;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_earth.level_bucket[mid] < b) lo = mid + 1; else hi = mid;
    }
    return lo < g_earth.nlevels && g_earth.level_bucket[lo] == b ? g_earth.level[lo] : 1e30;
}

int terrain_earth_water(TerrainDir p, TerrainEarthWater *out) {
    const EarthWater *w = &g_earth.water, *s = &g_earth.streams;
    TerrainEarthWater r;
    memset(&r, 0, sizeof r);
    r.bed = r.level = r.floor = 1e30;
    if (!g_earth.loaded || (!w->B && !s->B)) {
        r.edge = r.river = r.reach = r.shore = 1e30;
        *out = r;
        return 0;
    }
    double len = sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    p.x /= len; p.y /= len; p.z /= len;
    /* the reach is the smaller set's, so every answer within it is exact */
    r.reach = w->B && s->B ? fmin(w->reach, s->reach) : w->B ? w->reach : s->reach;
    double reach = r.reach / PLANET_RADIUS;
    WaterNear n, ns;
    memset(&n, 0, sizeof n);
    n.shore = n.bank = n.centre = reach;
    n.shore_any = n.floor = 1e30;
    n.best = n.shore_best = 0xffffffffu;
    ns = n;
    if (w->B) water_near(w, p, reach, &n);
    if (s->B) water_near(s, p, reach, &ns);
    double floor_ch = fmin(n.floor, ns.floor);
    r.valley = w->B ? fmin(PLANET_RADIUS * n.bank, PLANET_RADIUS * (n.inside ? -n.shore : n.shore)) : 1e30;
    const EarthWater *bw = w;
    if (ns.best != 0xffffffffu && ns.bank < n.bank) { n.bank = ns.bank; n.centre = ns.centre; n.best = ns.best; bw = s; }
    r.lake = n.inside;
    r.river = PLANET_RADIUS * n.centre;
    r.shore = PLANET_RADIUS * (n.inside ? -n.shore : n.shore);
    double river_edge = PLANET_RADIUS * n.bank;
    r.edge = r.shore < river_edge ? r.shore : river_edge;
    if (n.best != 0xffffffffu) {
        TerrainDir a = water_point(bw, n.best), q = water_point(bw, n.best + 1);
        TerrainDir d = { q.x - a.x, q.y - a.y, q.z - a.z };
        double k = d.x * p.x + d.y * p.y + d.z * p.z;
        d.x -= k * p.x; d.y -= k * p.y; d.z -= k * p.z;
        double dl = sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (dl > 0.0) { d.x /= dl; d.y /= dl; d.z /= dl; }
        r.width = bw->width[n.best];
        r.flow = d;
        r.stream = bw == s;
        if (bw->bed) {   /* the bed where p's nearest point on the segment is, between its ends' */
            double t;
            arc_dist_t(p, a, q, &t);
            r.bed = bw->bed[n.best] + t * (bw->bed[n.best + 1] - bw->bed[n.best]);
        }
    }
    /* the lakes' floors: a lake p is in lies under its own level falling
     * inwards; any other lake near, under its level rising outwards */
    double floor_lk = 1e30;
    if (w->B && w->bed) {
        if (n.shore_best != 0xffffffffu) r.level = w->bed[n.shore_best];
        else if (n.inside) r.level = bucket_level(n.bucket);
        if (n.inside && r.level < 1e29) floor_lk = lake_floor(r.level, r.shore);
        for (int m = 0; m < n.nlev; ++m)
            if (!(n.inside && n.lev[m] == r.level)) floor_lk = fmin(floor_lk, lake_floor(n.lev[m], PLANET_RADIUS * n.lev_d[m]));
    }
    r.floor = fmin(floor_ch, floor_lk);
    *out = r;
    return 1;
}

double terrain_earth_shore(TerrainDir p) {
    const EarthWater *w = &g_earth.water;
    if (!g_earth.loaded || !w->B) return 1e30;
    double len = sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    p.x /= len; p.y /= len; p.z /= len;
    TerrainDir c;
    size_t b = water_bucket(w, p, &c);
    int inside = w->inside[b];
    double shore = w->reach / PLANET_RADIUS;
    if (w->shores[b])
        for (uint32_t e = w->start[b]; e < w->start[b + 1]; ++e) {
            uint32_t k = w->entry[e];
            if (w->kind[k] != 2) continue;
            TerrainDir a = water_point(w, k), q = water_point(w, k + 1);
            double t, d = arc_dist_t(p, a, q, &t);
            if (d < shore) shore = d;
            if (arcs_cross(c, p, a, q)) inside ^= 1;
        }
    return PLANET_RADIUS * (inside ? -shore : shore);
}

double terrain_earth_valley(TerrainDir p) {
    const EarthWater *w = &g_earth.water;
    if (!g_earth.loaded || !w->B) return 1e30;
    double len = sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    p.x /= len; p.y /= len; p.z /= len;
    /* as terrain_earth_water reads the bake's set, so the two agree exactly */
    double reach = (g_earth.streams.B ? fmin(w->reach, g_earth.streams.reach) : w->reach) / PLANET_RADIUS;
    TerrainDir c;
    size_t b = water_bucket(w, p, &c);
    if (w->start[b] == w->start[b + 1]) return PLANET_RADIUS * (w->inside[b] ? -reach : reach);
    WaterNear n;
    water_near(w, p, reach, &n);
    return fmin(PLANET_RADIUS * n.bank, PLANET_RADIUS * (n.inside ? -n.shore : n.shore));
}

uint32_t terrain_earth_water_points(const float **xyz, const uint16_t **width, const uint8_t **kind, const uint8_t **flags) {
    const EarthWater *w = &g_earth.water;
    if (!g_earth.loaded || !w->B) return 0;
    *xyz = w->xyz; *width = w->width; *kind = w->kind; *flags = w->flags;
    return w->V;
}

int terrain_earth_water_buckets(const uint8_t **inside, const uint32_t **start, const uint32_t **entry) {
    const EarthWater *w = &g_earth.water;
    if (!g_earth.loaded || !w->B) return 0;
    *inside = w->inside; *start = w->start; *entry = w->entry;
    return w->B;
}

size_t terrain_earth_water_segments(void (*fn)(void *ctx, TerrainDir a, TerrainDir b, double width, int kind), void *ctx) {
    size_t n = 0;
    if (!g_earth.loaded) return 0;
    for (int set = 0; set < 2; ++set) {
        const EarthWater *w = set ? &g_earth.streams : &g_earth.water;
        if (!w->B) continue;
        for (uint32_t k = 0; k + 1 < w->V; ++k) {
            if (!(w->flags[k] & 1)) continue;
            if (fn) fn(ctx, water_point(w, k), water_point(w, k + 1), w->width[k], w->kind[k]);
            ++n;
        }
    }
    return n;
}

int terrain_earth_stream(uint32_t k, TerrainDir *a, TerrainDir *b, double *bed_a, double *bed_b, double *flow_a, double *flow_b) {
    const EarthWater *s = &g_earth.streams;
    if (!g_earth.loaded || !s->B || k + 1 >= s->V || !(s->flags[k] & 1)) return 0;
    *a = water_point(s, k); *b = water_point(s, k + 1);
    *bed_a = s->bed[k]; *bed_b = s->bed[k + 1];
    *flow_a = s->flow[k]; *flow_b = s->flow[k + 1];
    return 1;
}

uint32_t terrain_earth_stream_points(void) { return g_earth.loaded && g_earth.streams.B ? g_earth.streams.V : 0; }

int terrain_earth_beds(void) { return g_earth.loaded && g_earth.streams.B && g_earth.streams.bed; }
