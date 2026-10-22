#include "terrain_internal.h"
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
 * Without a bake the planet's procedural macro field (terrain_macro, in
 * metres) stands in, so a clone without the LFS tiles still runs. */

#define PI          3.14159265358979323846
#define EARTH_MAGIC "NCEARTH1"
#define EARTH_VERSION 2
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

static struct Earth {
    int        loaded, mips, N;
    double     scale, offset;
    double     origin_lat, origin_lon, origin_heading;
    TerrainDir ex, o, ez;       /* the chart's basis about the origin */
    double     datum;           /* E0: the Earth at the origin at TERRAIN_LIFT_SPACING (m) */
    char       path[1024];
    EarthMip   mip[MAX_MIPS];
    EarthLand  land;
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

void terrain_earth_unload(void) {
    for (int m = 0; m < g_earth.mips; ++m) mip_free(&g_earth.mip[m]);
    land_free(&g_earth.land);
    memset(&g_earth, 0, sizeof g_earth);
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
    if (get_u32(hd + 8) != EARTH_VERSION) return fail(f, path, "an older Earth bake (version 1): bake it again (cmake --build build --target earth)");
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
    if (fgetc(f) != EOF) return fail(f, path, "overlong");
    fclose(f);
    snprintf(g_earth.path, sizeof g_earth.path, "%s", path);
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

/* The chart about the origin: o its direction, ex east and ez south there
 * (as the procedural chart's, ez = ex x o), both turned clockwise by the
 * heading, so the chart's -z points along it. */
static void set_chart(void) {
    TerrainDir o = terrain_latlon_dir(g_earth.origin_lat, g_earth.origin_lon);
    TerrainDir east = unit3((TerrainDir){ -o.y, o.x, 0.0 });   /* the pole (+z) cross o */
    TerrainDir north = cross3(o, east);
    double h = g_earth.origin_heading * (PI / 180.0), c = cos(h), s = sin(h);
    g_earth.o = o;
    g_earth.ex = add3(scale3(east, c), scale3(north, -s));
    g_earth.ez = cross3(g_earth.ex, o);
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
        g_earth.datum = terrain_earth_height(g_earth.o, TERRAIN_LIFT_SPACING);
        const EarthMip *m0 = &g_earth.mip[0];
        printf("earth: %s, %d a side (%.0f m), %d mips, %.0f to %.0f m, grade %.2f; origin %.4f %.4f heading %.1f, datum %.1f m; landforms %d a side%s\n",
               g_earth.path, g_earth.N, m0->step, g_earth.mips, m0->lo, m0->hi, m0->grade, g_earth.origin_lat, g_earth.origin_lon, g_earth.origin_heading,
               g_earth.datum, g_earth.land.n, g_earth.land.hints ? ", with hints" : "");
    } else {
        fprintf(stderr, "earth: no bake %s (cmake --build build --target earth makes build/earth.bake, see data/earth/SOURCE.md); the procedural macro field stands in\n",
                path ? path : "found (NEONCITY_EARTH, earth.bake by the executable or here, build/earth.bake)");
    }
    return ok;
}

int terrain_earth_loaded(void) { return g_earth.loaded; }

int terrain_earth_chart(TerrainDir *ex, TerrainDir *o, TerrainDir *ez) {
    if (!g_earth.loaded) return 0;
    *ex = g_earth.ex; *o = g_earth.o; *ez = g_earth.ez;
    return 1;
}

double terrain_earth_datum(void) { return g_earth.loaded ? g_earth.datum : 0.0; }

double terrain_earth_rough_max(void) { return g_earth.loaded ? g_earth.land.rough_max : 1.0; }

void terrain_earth_origin(double *lat, double *lon, double *heading) {
    if (g_earth.loaded) { *lat = g_earth.origin_lat; *lon = g_earth.origin_lon; *heading = g_earth.origin_heading; return; }
    terrain_dir_latlon(terrain_chart_dir(0.0, 0.0), lat, lon);
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

/* The height at a spacing blends its two mips' heights, so lies between
 * their bounds. */
void terrain_earth_range_at(TerrainDir p, double radius, double spacing, double *lo, double *hi) {
    if (!g_earth.loaded) { *lo = -TERRAIN_MACRO_M; *hi = TERRAIN_MACRO_M; return; }
    double alpha = radius / PLANET_RADIUS;
    Bound b = { 1e30, -1e30, 0.0 };
    int m; double t;
    mips_for(spacing, &m, &t);
    cap_bound(&g_earth.mip[m], p, alpha, &b);
    if (t > 0.0) cap_bound(&g_earth.mip[m + 1], p, alpha, &b);
    bound_out(&b, lo, hi);
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
