/* earth_bake: bakes the Earth project (data/earth, staged by tools/earth/earth.py)
 * into the engine's cube-sphere elevation file.
 *
 *   earth_bake <stage.r16> <out.bake> [--size N] [--threads T]
 *              [--water-min-discharge Q] [--water-min-lake A] [--water-tolerance M]
 *   earth_bake --info <bake>
 *   earth_bake --dump-face <k|all> <bake> <out.png> [--mip M]
 *
 * The stage is the whole equirectangular grid as raw little-endian int16
 * metres with its <stage.r16>.txt sidecar (`earth.py stage data/earth
 * build/earth.r16`); its checksum is verified against the sidecar's, and the
 * hints' too if the sidecar names a hints file (the project's optional hints
 * layer, staged beside it as uint8 x 3 per cell), and the water's if it
 * names a water file (the project's rivers and lakes as lines, the format
 * set out in earth.py's stage_water). The --water options keep only the
 * rivers carrying at least Q m3/s and the lakes of at least A km2, and
 * simplify the lines to within M metres (the test bake's). The bake
 * holds the same ground resampled onto the six equal-angle cube faces
 * (src/terrain/terrain_cube.c, which this tool links and nothing else of the
 * engine, so the bake and the engine evaluate every direction bit for bit the
 * same): N samples a side at mip 0 (default 4096, 2.44 km at a face's
 * centre), halving per mip down to 8. Baking the same stage at the same size
 * gives the same bytes, whatever the thread count.
 *
 * File layout, all little-endian (i16 int16, f32 float, f64 double):
 *   header, 256 bytes:
 *      0  char[8] "NCEARTH1"
 *      8  u32 version (3)      12  u32 size N        16  u32 apron (2)
 *     20  u32 mips             24  u32 block (8)     28  u32 0
 *     32  f32 scale (metres per unit, 1)             36  f32 offset (m, 0)
 *     40  f64 origin latitude  48  f64 origin longitude  56  f64 origin heading (degrees)
 *     64  char[64] the source's sha256 (hex)        128  char[64] the stage's sha256 (hex)
 *    192  i16 min, i16 max over the planet (mip 0)  196  f32 steepest grade  200  f32 largest relief
 *    204  u32 stage width, u32 stage height        212  f64 stage cell (degrees)
 *    220  u32 the landform grid's mip L               224  u32 flags (1: it carries the hints)
 *    228  f32 the landform grid's largest relief
 *    232  u32 the water's buckets a side B (0: no water)  236  f32 the water's reach (m)
 *    240  u32 the water's points V  244  u32 its bucket entries E  248  u32 its rivers' lines  252  u32 its lakes' rings
 *   mip table, mips x 16 bytes: i16 min, i16 max, f32 steepest grade, f32 largest relief, u32 0
 *   then for each mip m (n = N >> m samples a side, g = n + 2 apron, nb = n / block), for each face f:
 *      i16 grid[g * g]      sample (i, j), i and j in [-apron, n + apron), at [(j + apron) * g + (i + apron)]:
 *                           the mean elevation (scale * value + offset metres) over the sample's cell, whose
 *                           centre is terrain_face_dir(f, (2 i + 1) / n - 1, (2 j + 1) / n - 1). The apron's
 *                           samples lie past the face's edges, over the neighbouring faces, resampled at
 *                           their own directions (not copied), so a filter can run up to the edge.
 *      i16 bmin[nb * nb], i16 bmax[nb * nb]   over each block of block x block interior samples
 *      f32 bslope[nb * nb]  the steepest grade in the block, metres per metre, between each sample
 *                           and its next along u and along v (the apron supplies the last)
 *      f32 brelief[nb * nb] the elevation range (m) within about RELIEF_M of the block at mip 0;
 *                           at coarser mips the mean of the four blocks under it
 *   then the landform grid, the first mip at least LAND_STEP_M a sample (or the coarsest), n = N >> L,
 *   with the same apron and sample directions, for each face f:
 *      i16 relief[g * g]    the range of the elevation (m) within LAND_RELIEF_M of the sample
 *      i8  strike_e[g * g], i8 strike_n[g * g]   the ridges' strike there, as a doubled angle from
 *                           east towards north (cos 2a, sin 2a) times its coherence, times 127: from
 *                           the structure of the gradients within LAND_STRIKE_M of the stage averaged
 *                           over LAND_COARSE cells a side (the strike runs
 *                           across the dominant gradient); 0 where the ground is level
 *      with the hints only:
 *      u8  override[g * g]  the hints' landform under the sample (0 none, 1 plains .. 4 plateau)
 *      u8  rough[g * g]     the hints' roughness, its mean within LAND_RELIEF_M (128 is 1; 0 reads as 128)
 *      u8  nosettle[g * g]  the hints' no-settlement mask, its largest within LAND_RELIEF_M
 *   then, with water (B > 0), the rivers' lines and the lakes' rings, each a run of points:
 *      points[V], 16 bytes each: f32 x, y, z (the direction, unit as far as f32 holds it),
 *                           u16 a river's width (m; 0 on a lake's shore), u8 kind (1 river, 2 lake),
 *                           u8 flags (1: a segment runs from this point to the next; the last of a
 *                           run has none, and a lake's ring repeats its first point last)
 *      u32 start[6 B B + 1] bucket (face f, i, j) at (f B + j) B + i lists entries start[b] .. start[b + 1]
 *      u8  inside[6 B B]    1 where the bucket's centre lies in a lake (even-odd over the rings)
 *      u32 entry[E]         a segment's first point; a bucket lists every segment that comes within
 *                           the reach (plus a river's half width) of any of its points
 *   Bucket (i, j) of face f spans u in [-1 + 2 i / B, -1 + 2 (i + 1) / B], v likewise, so its
 *   sides are great circles and the arc from its centre to any point in it stays inside it: a
 *   point lies in a lake if the bucket's centre does, flipped by each lake segment the arc
 *   between them crosses.
 *
 * Resampling: a sample's cell is split into K x K sub-cells (K such that a
 * sub-cell is no larger than a stage cell at the equator, at most KMAX);
 * each sub-cell's corners are taken to latitude and longitude, and the stage
 * cells within their bounding box are averaged by the area each contributes
 * (the box over-covers a tilted cell a little, which only blurs). Mip 0 is
 * built so, every sample; mip m + 1's interior is the 2 x 2 mean of mip m's,
 * and its apron is resampled from the stage with K doubled. The landform
 * grid reads the stage directly, bilinearly between its cells' centres, on
 * stencils laid out along east and north round each sample. */
#include "terrain.h"
#include "thread.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#undef STBI_WRITE_NO_STDIO

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PI        3.14159265358979323846
#define APRON     2
#define BLOCK     8
#define MAX_MIPS  16
#define MIN_FACE  8
#define RELIEF_M  20000.0
#define KMAX      64
#define BAND      8         /* sample rows per resampling task */
#define HEADER    256
#define MAGIC     "NCEARTH1"
#define VERSION   3
#define LAND_STEP_M     8000.0    /* the landform grid: the first mip at least this coarse */
#define LAND_RELIEF_M   20000.0   /* relief and the hints: within this of a sample... */
#define LAND_RELIEF_N   8         /* ...on a stencil of this many steps each way */
#define LAND_COARSE     8         /* strike: the stage averaged over this many cells a side (15') */
#define LAND_STRIKE_M   100000.0  /* ...its gradients within this... */
#define LAND_STRIKE_N   10        /* ...on a stencil of this many steps each way... */
#define LAND_STRIKE_SIGMA 40000.0 /* ...weighted by a Gaussian this wide */
#define WATER_MAGIC     "NCWATER1"
#define WATER_DIV       4         /* the water's buckets: this many mip 0 samples a side (9.8 km at N = 4096) */
#define WATER_MIN_B     8
#define WATER_REACH_M   2000.0    /* a bucket lists every line within this (plus a river's half width) of it */
#define WATER_FACE_COS  0.2       /* a segment is bucketed on a face where both its ends lie this far up its axis */

/* --- SHA-256 (FIPS 180-4), to check the stage against its sidecar --------------- */

typedef struct Sha256 { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t n; } Sha256;

static const uint32_t SHA_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha_block(Sha256 *s, const unsigned char *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; ++i) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
    static const uint32_t h0[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, h0, sizeof h0); s->len = 0; s->n = 0;
}

static void sha_update(Sha256 *s, const unsigned char *p, size_t n) {
    s->len += n;
    while (n > 0) {
        size_t take = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->buf + s->n, p, take); s->n += take; p += take; n -= take;
        if (s->n == 64) { sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_hex(Sha256 *s, char out[65]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80;
    sha_update(s, &pad, 1);
    unsigned char zero = 0;
    while (s->n != 56) sha_update(s, &zero, 1);
    unsigned char len[8];
    for (int i = 0; i < 8; ++i) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; ++i) snprintf(out + 8 * i, 9, "%08x", s->h[i]);
}

/* --- Little-endian output ------------------------------------------------------- */

static void put_u16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_u32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_u64(unsigned char *p, uint64_t v) { for (int i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_f32(unsigned char *p, float v) { uint32_t u; memcpy(&u, &v, 4); put_u32(p, u); }
static void put_f64(unsigned char *p, double v) { uint64_t u; memcpy(&u, &v, 8); put_u64(p, u); }
static uint16_t get_u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get_u64(const unsigned char *p) { return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32; }
static float    get_f32(const unsigned char *p) { uint32_t u = get_u32(p); float v; memcpy(&v, &u, 4); return v; }
static double   get_f64(const unsigned char *p) { uint64_t u = get_u64(p); double v; memcpy(&v, &u, 8); return v; }

static int write_i16s(FILE *f, const int16_t *v, size_t n) {
    unsigned char buf[8192];
    while (n > 0) {
        size_t take = n < sizeof buf / 2 ? n : sizeof buf / 2;
        for (size_t i = 0; i < take; ++i) put_u16(buf + 2 * i, (uint16_t)v[i]);
        if (fwrite(buf, 2, take, f) != take) return 0;
        v += take; n -= take;
    }
    return 1;
}

static int write_f32s(FILE *f, const float *v, size_t n) {
    unsigned char buf[8192];
    while (n > 0) {
        size_t take = n < sizeof buf / 4 ? n : sizeof buf / 4;
        for (size_t i = 0; i < take; ++i) put_f32(buf + 4 * i, v[i]);
        if (fwrite(buf, 4, take, f) != take) return 0;
        v += take; n -= take;
    }
    return 1;
}

static int read_i16s(FILE *f, int16_t *v, size_t n) {
    unsigned char buf[8192];
    while (n > 0) {
        size_t take = n < sizeof buf / 2 ? n : sizeof buf / 2;
        if (fread(buf, 2, take, f) != take) return 0;
        for (size_t i = 0; i < take; ++i) v[i] = (int16_t)get_u16(buf + 2 * i);
        v += take; n -= take;
    }
    return 1;
}

static int read_f32s(FILE *f, float *v, size_t n) {
    unsigned char buf[8192];
    while (n > 0) {
        size_t take = n < sizeof buf / 4 ? n : sizeof buf / 4;
        if (fread(buf, 4, take, f) != take) return 0;
        for (size_t i = 0; i < take; ++i) v[i] = get_f32(buf + 4 * i);
        v += take; n -= take;
    }
    return 1;
}

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "earth_bake: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

/* --- The stage --------------------------------------------------------------------- */

typedef struct Stage {
    int      W, H;                 /* columns (longitude) and rows (latitude, north first) */
    double   lon0, lat0, cell;     /* the north-west corner and the cell size, degrees */
    double   origin_lat, origin_lon, origin_heading;
    char     source_sha[65], stage_sha[65];
    int16_t *h;                    /* W x H metres */
    double  *sinlat;               /* sin of the H + 1 row edges' latitudes, for areas */
    char     hints_file[960], hints_sha[65];
    uint8_t *hints;                /* W x H x 3 (landform, roughness, nosettle), or NULL */
    float   *coarse;               /* (W / LAND_COARSE) x (H / LAND_COARSE) cell means, for the strike */
    char     water_file[960], water_sha[65];
    unsigned char *water;          /* the staged water file, or NULL */
    size_t   water_bytes;
} Stage;

/* Reads a whole file of exactly `bytes` bytes and checks it against a sha256. */
static unsigned char *read_checked(const char *path, size_t bytes, const char *sha, const char *what) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "earth_bake: cannot open %s\n", path); return NULL; }
    unsigned char *raw = (unsigned char *)xmalloc(bytes);
    size_t got = fread(raw, 1, bytes, f);
    int more = fgetc(f) != EOF;
    fclose(f);
    if (got != bytes || more) { fprintf(stderr, "earth_bake: %s should be exactly %zu bytes\n", path, bytes); free(raw); return NULL; }
    Sha256 h; char hex[65];
    sha_init(&h); sha_update(&h, raw, bytes); sha_hex(&h, hex);
    if (strcmp(hex, sha) != 0) { fprintf(stderr, "earth_bake: %s doesn't match its sidecar's %s (%s, not %s): stage it again\n", path, what, hex, sha); free(raw); return NULL; }
    return raw;
}

#ifdef _WIN32
#define ftell64 _ftelli64
#else
#define ftell64 ftello
#endif

/* A file the sidecar names, beside the stage: whole, checked against its sha256. */
static unsigned char *read_beside(const char *stage, const char *name, const char *sha, const char *what, size_t *bytes) {
    char p[2048];
    const char *slash = strrchr(stage, '/'), *bs = strrchr(stage, '\\');
    if (bs > slash) slash = bs;
    snprintf(p, sizeof p, "%.*s%s", slash ? (int)(slash - stage + 1) : 0, stage, name);
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "earth_bake: cannot open %s\n", p); return NULL; }
    long long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = (long long)ftell64(f);
    fclose(f);
    if (n < 0) { fprintf(stderr, "earth_bake: cannot size %s\n", p); return NULL; }
    *bytes = (size_t)n;
    return read_checked(p, (size_t)n, sha, what);
}

static int stage_read(Stage *s, const char *path) {
    char side[1024];
    snprintf(side, sizeof side, "%s.txt", path);
    FILE *f = fopen(side, "r");
    if (!f) { fprintf(stderr, "earth_bake: cannot open %s\n", side); return 0; }
    memset(s, 0, sizeof *s);
    s->lat0 = 90.0; s->lon0 = -180.0;
    int format = 0, dtype = 0, order = 0, rows = 0, pixel = 0;
    char line[1024], key[64], val[960];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%63s %959s", key, val) != 2) continue;
        if (!strcmp(key, "format")) format = !strcmp(val, "earth-stage-1");
        else if (!strcmp(key, "dtype")) dtype = !strcmp(val, "int16");
        else if (!strcmp(key, "byte_order")) order = !strcmp(val, "little");
        else if (!strcmp(key, "row_order")) rows = !strcmp(val, "north_to_south");
        else if (!strcmp(key, "pixel")) pixel = !strcmp(val, "area");
        else if (!strcmp(key, "width")) s->W = atoi(val);
        else if (!strcmp(key, "height")) s->H = atoi(val);
        else if (!strcmp(key, "lon0")) s->lon0 = atof(val);
        else if (!strcmp(key, "lat0")) s->lat0 = atof(val);
        else if (!strcmp(key, "cell_deg")) s->cell = atof(val);
        else if (!strcmp(key, "origin_lat")) s->origin_lat = atof(val);
        else if (!strcmp(key, "origin_lon")) s->origin_lon = atof(val);
        else if (!strcmp(key, "origin_heading_deg")) s->origin_heading = atof(val);
        else if (!strcmp(key, "source_sha256")) snprintf(s->source_sha, sizeof s->source_sha, "%s", val);
        else if (!strcmp(key, "stage_sha256")) snprintf(s->stage_sha, sizeof s->stage_sha, "%s", val);
        else if (!strcmp(key, "hints_file")) snprintf(s->hints_file, sizeof s->hints_file, "%s", val);
        else if (!strcmp(key, "hints_sha256")) snprintf(s->hints_sha, sizeof s->hints_sha, "%s", val);
        else if (!strcmp(key, "water_file")) snprintf(s->water_file, sizeof s->water_file, "%s", val);
        else if (!strcmp(key, "water_sha256")) snprintf(s->water_sha, sizeof s->water_sha, "%s", val);
    }
    fclose(f);
    if (!format || !dtype || !order || !rows || !pixel) { fprintf(stderr, "earth_bake: %s isn't an earth-stage-1 sidecar (little-endian int16, rows north to south, pixel-is-area)\n", side); return 0; }
    if (s->W <= 0 || s->H <= 0 || s->cell <= 0.0 || fabs(s->W * s->cell - 360.0) > 1e-6 || fabs(s->H * s->cell - 180.0) > 1e-6 ||
        fabs(s->lon0 + 180.0) > 1e-9 || fabs(s->lat0 - 90.0) > 1e-9) {
        fprintf(stderr, "earth_bake: the stage must cover the globe from 90 N 180 W (%d x %d cells of %g degrees from %g, %g)\n", s->W, s->H, s->cell, s->lat0, s->lon0);
        return 0;
    }
    if (strlen(s->stage_sha) != 64) { fprintf(stderr, "earth_bake: %s carries no stage_sha256\n", side); return 0; }

    if (s->hints_file[0] && strlen(s->hints_sha) != 64) { fprintf(stderr, "earth_bake: %s names a hints file but no hints_sha256\n", side); return 0; }
    if (s->water_file[0] && strlen(s->water_sha) != 64) { fprintf(stderr, "earth_bake: %s names a water file but no water_sha256\n", side); return 0; }
    if (s->water_file[0]) {
        s->water = read_beside(path, s->water_file, s->water_sha, "water_sha256", &s->water_bytes);
        if (!s->water) return 0;
    }
    size_t cells = (size_t)s->W * (size_t)s->H;
    unsigned char *raw = read_checked(path, cells * 2, s->stage_sha, "stage_sha256");
    if (!raw) return 0;
    if (s->hints_file[0]) {   /* beside the stage */
        char hp[2048];
        const char *slash = strrchr(path, '/'), *bs = strrchr(path, '\\');
        if (bs > slash) slash = bs;
        snprintf(hp, sizeof hp, "%.*s%s", slash ? (int)(slash - path + 1) : 0, path, s->hints_file);
        s->hints = read_checked(hp, cells * 3, s->hints_sha, "hints_sha256");
        if (!s->hints) { free(raw); return 0; }
    }
    s->h = (int16_t *)xmalloc(cells * 2);
    for (size_t i = 0; i < cells; ++i) s->h[i] = (int16_t)get_u16(raw + 2 * i);
    free(raw);
    s->sinlat = (double *)xmalloc(sizeof(double) * (size_t)(s->H + 1));
    for (int r = 0; r <= s->H; ++r) s->sinlat[r] = sin((s->lat0 - r * s->cell) * (PI / 180.0));
    return 1;
}

/* Adds the stage's cells within the box of latitudes [lat_lo, lat_hi] and
 * longitudes [lon_lo, lon_hi] (degrees; the longitudes may run past 180 and
 * wrap), each weighted by the area of its overlap, to sum and wsum. */
static void box_sum(const Stage *s, double lat_lo, double lat_hi, double lon_lo, double lon_hi, double *sum, double *wsum) {
    double y0 = (s->lat0 - lat_hi) / s->cell, y1 = (s->lat0 - lat_lo) / s->cell;
    if (y0 < 0.0) y0 = 0.0;
    if (y1 > (double)s->H) y1 = (double)s->H;
    if (y1 <= y0) return;
    double x0, x1;
    if (lon_hi - lon_lo >= 360.0) { x0 = 0.0; x1 = (double)s->W; }
    else { x0 = (lon_lo - s->lon0) / s->cell; x1 = (lon_hi - s->lon0) / s->cell; }
    if (x1 <= x0) return;
    int r0 = (int)floor(y0), r1 = (int)ceil(y1) - 1;
    long c0 = (long)floor(x0), c1 = (long)ceil(x1) - 1;
    if (r1 > s->H - 1) r1 = s->H - 1;
    for (int r = r0; r <= r1; ++r) {
        double a = (double)r > y0 ? (double)r : y0, b = (double)(r + 1) < y1 ? (double)(r + 1) : y1;
        double wl;
        if (a == (double)r && b == (double)(r + 1)) wl = s->sinlat[r] - s->sinlat[r + 1];
        else wl = sin((s->lat0 - a * s->cell) * (PI / 180.0)) - sin((s->lat0 - b * s->cell) * (PI / 180.0));
        const int16_t *row = s->h + (size_t)r * (size_t)s->W;
        for (long c = c0; c <= c1; ++c) {
            double ca = (double)c > x0 ? (double)c : x0, cb = (double)(c + 1) < x1 ? (double)(c + 1) : x1;
            double w = wl * (cb - ca);
            long cc = c % s->W;
            if (cc < 0) cc += s->W;
            *sum += w * (double)row[cc];
            *wsum += w;
        }
    }
}

/* --- Resampling a face ------------------------------------------------------------- */

typedef struct LL { double lat, lon; } LL;

/* Sub-cell corner (si, sj) of a face of n samples with K sub-cells a sample,
 * counted from the apron's outer edge, to latitude and longitude. */
static LL corner_ll(int face, int K, int n, int si, int sj) {
    double kn = (double)(K * n);
    double u = (double)(2 * (si - K * APRON) - K * n) / kn;
    double v = (double)(2 * (sj - K * APRON) - K * n) / kn;
    LL r;
    terrain_dir_latlon(terrain_face_dir(face, u, v), &r.lat, &r.lon);
    return r;
}

/* The mean elevation over the sample whose sub-cell corners start at c
 * (rows of `stride` corners, K + 1 of them), from the stage. */
static double sample_mean(const Stage *s, const LL *c, int stride, int K) {
    double sum = 0.0, wsum = 0.0;
    for (int sj = 0; sj < K; ++sj) {
        for (int si = 0; si < K; ++si) {
            const LL *q[4] = { c + sj * stride + si, c + sj * stride + si + 1, c + (sj + 1) * stride + si, c + (sj + 1) * stride + si + 1 };
            double lat_lo = 90.0, lat_hi = -90.0, lon_lo = 0.0, lon_hi = 0.0, ref = 0.0;
            int have = 0;
            for (int k = 0; k < 4; ++k) {
                if (q[k]->lat < lat_lo) lat_lo = q[k]->lat;
                if (q[k]->lat > lat_hi) lat_hi = q[k]->lat;
                if (fabs(q[k]->lat) > 90.0 - 1e-9) continue;   /* a pole has no longitude */
                double lon = q[k]->lon;
                if (!have) { ref = lon; lon_lo = lon_hi = lon; have = 1; continue; }
                while (lon - ref > 180.0) lon -= 360.0;
                while (lon - ref < -180.0) lon += 360.0;
                if (lon < lon_lo) lon_lo = lon;
                if (lon > lon_hi) lon_hi = lon;
            }
            if (!have) { lon_lo = -180.0; lon_hi = 180.0; }
            box_sum(s, lat_lo, lat_hi, lon_lo, lon_hi, &sum, &wsum);
        }
    }
    return wsum > 0.0 ? sum / wsum : 0.0;
}

typedef struct Resample {
    const Stage *s;
    int    n, g, K, bands, apron_only;
    float *out[6];
} Resample;

/* One band of BAND sample rows of one face: every sample, or the apron's
 * alone (the interior then comes from the finer mip). */
static void resample_task(void *ctx, int t) {
    Resample *r = (Resample *)ctx;
    int face = t / r->bands, band = t % r->bands;
    int n = r->n, g = r->g, K = r->K;
    int j0 = -APRON + band * BAND, j1 = j0 + BAND < n + APRON ? j0 + BAND : n + APRON;
    int rows = K * (j1 - j0) + 1, stride = K * g + 1;
    int edge_rows = j0 < 0 || j1 > n;
    LL *c = (LL *)xmalloc(sizeof(LL) * (size_t)rows * (size_t)stride);
    for (int sj = 0; sj < rows; ++sj) {
        int gj = K * (j0 + APRON) + sj;
        if (!r->apron_only || edge_rows) {
            for (int si = 0; si < stride; ++si) c[sj * stride + si] = corner_ll(face, K, n, si, gj);
        } else {
            for (int si = 0; si <= K * APRON; ++si) c[sj * stride + si] = corner_ll(face, K, n, si, gj);
            for (int si = K * (n + APRON); si < stride; ++si) c[sj * stride + si] = corner_ll(face, K, n, si, gj);
        }
    }
    for (int j = j0; j < j1; ++j) {
        for (int i = -APRON; i < n + APRON; ++i) {
            if (r->apron_only && i >= 0 && i < n && j >= 0 && j < n) continue;
            const LL *corner = c + (size_t)(K * (j - j0)) * stride + K * (i + APRON);
            r->out[face][(size_t)(j + APRON) * g + (i + APRON)] = (float)sample_mean(r->s, corner, stride, K);
        }
    }
    free(c);
}

/* --- The mips and their blocks ------------------------------------------------------- */

typedef struct Mip {
    int      n, g, nb;
    float   *f[6];                  /* the resampled heights, g x g */
    int16_t *q[6];                  /* quantised, as written */
    int16_t *bmin[6], *bmax[6];
    float   *bslope[6], *brelief[6];
    int16_t  lo, hi;
    float    slope_max, relief_max;
} Mip;

/* The landform grid (see the file layout). */
typedef struct Land {
    int      mip, n, g, hints;
    int16_t *relief[6];
    int8_t  *se[6], *sn[6];
    uint8_t *override[6], *rough[6], *nosettle[6];
    float    relief_max;
} Land;

/* The water section (see the file layout). */
typedef struct Water {
    int       B;                    /* buckets a side per face; 0 without water */
    float     reach;
    uint32_t  V, E, rivers, rings;
    float    *xyz;                  /* V x 3 */
    uint16_t *width;
    uint8_t  *kind, *flags;
    uint32_t *start;                /* 6 B B + 1 */
    uint8_t  *inside;               /* 6 B B */
    uint32_t *entry;                /* E */
} Water;

typedef struct Bake {
    int      N, mips, W, H;
    double   cell, origin_lat, origin_lon, origin_heading;
    float    scale, offset;
    char     source_sha[65], stage_sha[65];
    Mip      mip[MAX_MIPS];
    Land     land;
    Water    water;
} Bake;

typedef struct Downsample { const Mip *from; Mip *to; } Downsample;

static void downsample_task(void *ctx, int t) {
    Downsample *d = (Downsample *)ctx;
    int face = t / d->to->n, j = t % d->to->n;
    const float *src = d->from->f[face];
    float *dst = d->to->f[face];
    int gs = d->from->g, gd = d->to->g;
    for (int i = 0; i < d->to->n; ++i) {
        const float *p = src + (size_t)(2 * j + APRON) * gs + (2 * i + APRON);
        dst[(size_t)(j + APRON) * gd + (i + APRON)] = (float)(0.25 * (((double)p[0] + (double)p[1]) + ((double)p[gs] + (double)p[gs + 1])));
    }
}

typedef struct Quantise { Mip *m; } Quantise;

static void quantise_task(void *ctx, int t) {
    Mip *m = ((Quantise *)ctx)->m;
    int face = t / m->g, j = t % m->g;
    const float *f = m->f[face] + (size_t)j * m->g;
    int16_t *q = m->q[face] + (size_t)j * m->g;
    for (int i = 0; i < m->g; ++i) {
        long v = lrintf(f[i]);
        q[i] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
}

typedef struct Blocks { Mip *m; const Mip *finer; int window; } Blocks;

/* The blocks of one block row of one face: min and max, the steepest grade
 * between neighbouring samples, and the relief (at mip 0, the range within
 * `window` samples round the block; coarser, the mean of the finer mip's). */
static void blocks_task(void *ctx, int t) {
    Blocks *b = (Blocks *)ctx;
    Mip *m = b->m;
    int n = m->n, g = m->g, nb = m->nb;
    int face = t / nb, bj = t % nb;
    const int16_t *q = m->q[face];
    TerrainDir *dir = (TerrainDir *)xmalloc(sizeof(TerrainDir) * (size_t)(BLOCK + 1) * (size_t)g);
    for (int jj = 0; jj <= BLOCK; ++jj) {
        int j = bj * BLOCK + jj;
        for (int i = -APRON; i < n + APRON; ++i)
            dir[(size_t)jj * g + (i + APRON)] = terrain_face_dir(face, (double)(2 * i + 1) / n - 1.0, (double)(2 * j + 1) / n - 1.0);
    }
    for (int bi = 0; bi < nb; ++bi) {
        int lo = 32767, hi = -32768;
        double slope = 0.0;
        for (int jj = 0; jj < BLOCK; ++jj) {
            int j = bj * BLOCK + jj;
            for (int ii = 0; ii < BLOCK; ++ii) {
                int i = bi * BLOCK + ii;
                int h = q[(size_t)(j + APRON) * g + (i + APRON)];
                int hu = q[(size_t)(j + APRON) * g + (i + 1 + APRON)];
                int hv = q[(size_t)(j + 1 + APRON) * g + (i + APRON)];
                if (h < lo) lo = h;
                if (h > hi) hi = h;
                TerrainDir p = dir[(size_t)jj * g + (i + APRON)];
                TerrainDir pu = dir[(size_t)jj * g + (i + 1 + APRON)], pv = dir[(size_t)(jj + 1) * g + (i + APRON)];
                TerrainDir du = { pu.x - p.x, pu.y - p.y, pu.z - p.z }, dv = { pv.x - p.x, pv.y - p.y, pv.z - p.z };
                double lu = sqrt(du.x * du.x + du.y * du.y + du.z * du.z), lv = sqrt(dv.x * dv.x + dv.y * dv.y + dv.z * dv.z);
                double c = (du.x * dv.x + du.y * dv.y + du.z * dv.z) / (lu * lv);
                double gu = (double)(hu - h) / (PLANET_RADIUS * lu), gv = (double)(hv - h) / (PLANET_RADIUS * lv);
                /* the gradient from two directional derivatives along axes meeting at angle acos(c) */
                double g2 = (gu * gu + gv * gv - 2.0 * gu * gv * c) / (1.0 - c * c);
                if (g2 > slope) slope = g2;
            }
        }
        m->bmin[face][(size_t)bj * nb + bi] = (int16_t)lo;
        m->bmax[face][(size_t)bj * nb + bi] = (int16_t)hi;
        m->bslope[face][(size_t)bj * nb + bi] = (float)sqrt(slope);
        float relief;
        if (b->finer) {
            const float *fr = b->finer->brelief[face];
            int fnb = b->finer->nb;
            relief = (float)(0.25 * (((double)fr[(size_t)(2 * bj) * fnb + 2 * bi] + (double)fr[(size_t)(2 * bj) * fnb + 2 * bi + 1]) +
                                     ((double)fr[(size_t)(2 * bj + 1) * fnb + 2 * bi] + (double)fr[(size_t)(2 * bj + 1) * fnb + 2 * bi + 1])));
        } else {
            int w = b->window, rlo = 32767, rhi = -32768;
            int i0 = bi * BLOCK - w, i1 = bi * BLOCK + BLOCK + w, j0 = bj * BLOCK - w, j1 = bj * BLOCK + BLOCK + w;
            if (i0 < -APRON) i0 = -APRON;
            if (j0 < -APRON) j0 = -APRON;
            if (i1 > n + APRON) i1 = n + APRON;
            if (j1 > n + APRON) j1 = n + APRON;
            for (int j = j0; j < j1; ++j) {
                const int16_t *row = q + (size_t)(j + APRON) * g + APRON;
                for (int i = i0; i < i1; ++i) {
                    int h = row[i];
                    if (h < rlo) rlo = h;
                    if (h > rhi) rhi = h;
                }
            }
            relief = (float)(rhi - rlo);
        }
        m->brelief[face][(size_t)bj * nb + bi] = relief;
    }
    free(dir);
}

static void mip_alloc(Mip *m, int n, int with_floats) {
    m->n = n; m->g = n + 2 * APRON; m->nb = n / BLOCK;
    size_t cells = (size_t)m->g * (size_t)m->g, blocks = (size_t)m->nb * (size_t)m->nb;
    for (int f = 0; f < 6; ++f) {
        m->f[f] = with_floats ? (float *)xmalloc(cells * sizeof(float)) : NULL;
        m->q[f] = (int16_t *)xmalloc(cells * sizeof(int16_t));
        m->bmin[f] = (int16_t *)xmalloc(blocks * sizeof(int16_t));
        m->bmax[f] = (int16_t *)xmalloc(blocks * sizeof(int16_t));
        m->bslope[f] = (float *)xmalloc(blocks * sizeof(float));
        m->brelief[f] = (float *)xmalloc(blocks * sizeof(float));
    }
}

static void mip_summary(Mip *m) {
    m->lo = 32767; m->hi = -32768; m->slope_max = 0.0f; m->relief_max = 0.0f;
    for (int f = 0; f < 6; ++f) {
        for (size_t k = 0; k < (size_t)m->nb * m->nb; ++k) {
            if (m->bmin[f][k] < m->lo) m->lo = m->bmin[f][k];
            if (m->bmax[f][k] > m->hi) m->hi = m->bmax[f][k];
            if (m->bslope[f][k] > m->slope_max) m->slope_max = m->bslope[f][k];
            if (m->brelief[f][k] > m->relief_max) m->relief_max = m->brelief[f][k];
        }
    }
}

/* How finely to split a sample of n a side: sub-cells no larger than a stage
 * cell at the equator, allowing for the equal-angle grid's stretch, up to KMAX. */
static int sub_cells(int n, double cell_deg) {
    double sample_rad = (PI / 2.0) / n, cell_rad = cell_deg * (PI / 180.0);
    int K = (int)ceil(1.5 * sample_rad / cell_rad);
    return K < 1 ? 1 : K > KMAX ? KMAX : K;
}

/* --- The landform grid ------------------------------------------------------------- */

/* The stage's elevation at a latitude and longitude (degrees): bilinear
 * between its cells' centres, round the globe in longitude. */
static double stage_at(const Stage *s, double lat, double lon) {
    double y = (s->lat0 - lat) / s->cell - 0.5, x = (lon - s->lon0) / s->cell - 0.5;
    y = y < 0.0 ? 0.0 : y > s->H - 1 ? (double)(s->H - 1) : y;
    double fy = floor(y), fx = floor(x), ty = y - fy, tx = x - fx;
    int r0 = (int)fy, r1 = r0 + 1 < s->H ? r0 + 1 : r0;
    long c0 = (long)fx % s->W;
    if (c0 < 0) c0 += s->W;
    long c1 = c0 + 1 == s->W ? 0 : c0 + 1;
    const int16_t *a = s->h + (size_t)r0 * s->W, *b = s->h + (size_t)r1 * s->W;
    return (1.0 - ty) * ((1.0 - tx) * a[c0] + tx * a[c1]) + ty * ((1.0 - tx) * b[c0] + tx * b[c1]);
}

/* The coarse grid's elevation at a latitude and longitude: as stage_at. */
static double coarse_at(const Stage *s, double lat, double lon) {
    int W = s->W / LAND_COARSE, H = s->H / LAND_COARSE;
    double cell = s->cell * LAND_COARSE;
    double y = (s->lat0 - lat) / cell - 0.5, x = (lon - s->lon0) / cell - 0.5;
    y = y < 0.0 ? 0.0 : y > H - 1 ? (double)(H - 1) : y;
    double fy = floor(y), fx = floor(x), ty = y - fy, tx = x - fx;
    int r0 = (int)fy, r1 = r0 + 1 < H ? r0 + 1 : r0;
    long c0 = (long)fx % W;
    if (c0 < 0) c0 += W;
    long c1 = c0 + 1 == W ? 0 : c0 + 1;
    const float *a = s->coarse + (size_t)r0 * W, *b = s->coarse + (size_t)r1 * W;
    return (1.0 - ty) * ((1.0 - tx) * a[c0] + tx * a[c1]) + ty * ((1.0 - tx) * b[c0] + tx * b[c1]);
}

/* The coarse grid: each LAND_COARSE x LAND_COARSE block of the stage's cells, averaged. */
static void coarse_build(Stage *s) {
    int W = s->W / LAND_COARSE, H = s->H / LAND_COARSE;
    s->coarse = (float *)xmalloc(sizeof(float) * (size_t)W * H);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            double sum = 0.0;
            for (int rr = 0; rr < LAND_COARSE; ++rr) {
                const int16_t *row = s->h + (size_t)(r * LAND_COARSE + rr) * s->W + (size_t)c * LAND_COARSE;
                for (int cc = 0; cc < LAND_COARSE; ++cc) sum += row[cc];
            }
            s->coarse[(size_t)r * W + c] = (float)(sum / (LAND_COARSE * LAND_COARSE));
        }
}

/* The stage's cell under a latitude and longitude. */
static size_t stage_cell(const Stage *s, double lat, double lon) {
    long r = (long)floor((s->lat0 - lat) / s->cell), c = (long)floor((lon - s->lon0) / s->cell) % s->W;
    r = r < 0 ? 0 : r >= s->H ? s->H - 1 : r;
    if (c < 0) c += s->W;
    return (size_t)r * s->W + (size_t)c;
}

/* East and north at p (at a pole, east is taken along +y). */
static void tangent(TerrainDir p, TerrainDir *e, TerrainDir *nn) {
    double l = hypot(p.x, p.y);
    if (l < 1e-12) { e->x = 0.0; e->y = 1.0; e->z = 0.0; }
    else { e->x = -p.y / l; e->y = p.x / l; e->z = 0.0; }
    nn->x = p.y * e->z - p.z * e->y; nn->y = p.z * e->x - p.x * e->z; nn->z = p.x * e->y - p.y * e->x;
}

/* p moved de metres east and dn north in its tangent plane, back onto the
 * sphere, in latitude and longitude. */
static void offset_ll(TerrainDir p, TerrainDir e, TerrainDir nn, double de, double dn, double *lat, double *lon) {
    double a = de / PLANET_RADIUS, b = dn / PLANET_RADIUS;
    TerrainDir q = { p.x + a * e.x + b * nn.x, p.y + a * e.y + b * nn.y, p.z + a * e.z + b * nn.z };
    terrain_dir_latlon(q, lat, lon);
}

typedef struct LandTask { const Stage *s; Land *l; } LandTask;

/* One sample row of one face of the landform grid: the relief and the hints
 * over a disc of LAND_RELIEF_M, and the strike from the gradients' structure
 * tensor (each gradient's doubled angle, weighted by its square and the
 * Gaussian) over a disc of LAND_STRIKE_M of the coarse grid, turned a
 * quarter turn. */
static void land_task(void *ctx, int t) {
    const LandTask *lt = (const LandTask *)ctx;
    const Stage *s = lt->s;
    Land *l = lt->l;
    enum { RN = LAND_RELIEF_N, SN = LAND_STRIKE_N, SW = 2 * LAND_STRIKE_N + 1 };
    const double rstep = LAND_RELIEF_M / RN, sstep = LAND_STRIKE_M / SN;
    int n = l->n, g = l->g, face = t / g, j = t % g - APRON;
    double hs[SW][SW];
    for (int i = -APRON; i < n + APRON; ++i) {
        TerrainDir p = terrain_face_dir(face, (double)(2 * i + 1) / n - 1.0, (double)(2 * j + 1) / n - 1.0), e, nn;
        tangent(p, &e, &nn);
        double lo = 1e30, hi = -1e30, rough = 0.0, lat, lon;
        int nosettle = 0, count = 0;
        for (int b = -RN; b <= RN; ++b)
            for (int a = -RN; a <= RN; ++a) {
                if (a * a + b * b > RN * RN) continue;
                offset_ll(p, e, nn, a * rstep, b * rstep, &lat, &lon);
                double h = stage_at(s, lat, lon);
                if (h < lo) lo = h;
                if (h > hi) hi = h;
                if (s->hints) {
                    const uint8_t *c = s->hints + 3 * stage_cell(s, lat, lon);
                    rough += c[1] ? c[1] : 128;
                    if (c[2] > nosettle) nosettle = c[2];
                    ++count;
                }
            }
        for (int b = -SN; b <= SN; ++b)
            for (int a = -SN; a <= SN; ++a) {
                offset_ll(p, e, nn, a * sstep, b * sstep, &lat, &lon);
                hs[b + SN][a + SN] = coarse_at(s, lat, lon);
            }
        double jxx = 0.0, jyy = 0.0, jxy = 0.0;
        for (int b = 1 - SN; b < SN; ++b)
            for (int a = 1 - SN; a < SN; ++a) {
                if (a * a + b * b > (SN - 1) * (SN - 1)) continue;
                double gx = (hs[b + SN][a + SN + 1] - hs[b + SN][a + SN - 1]) / (2.0 * sstep);
                double gy = (hs[b + SN + 1][a + SN] - hs[b + SN - 1][a + SN]) / (2.0 * sstep);
                double w = exp(-(a * a + b * b) * sstep * sstep / (2.0 * LAND_STRIKE_SIGMA * LAND_STRIKE_SIGMA));
                jxx += w * gx * gx; jyy += w * gy * gy; jxy += w * gx * gy;
            }
        /* The gradients' doubled angle is (jxx - jyy, 2 jxy) / (jxx + jyy), no
         * longer than 1; the strike's is a half turn on, so its negative. */
        double tr = jxx + jyy, ce = tr > 1e-12 ? -(jxx - jyy) / tr : 0.0, cn = tr > 1e-12 ? -2.0 * jxy / tr : 0.0;
        size_t k = (size_t)(j + APRON) * g + (i + APRON);
        long rv = lrint(hi - lo);
        l->relief[face][k] = (int16_t)(rv > 32767 ? 32767 : rv);
        l->se[face][k] = (int8_t)lrint(127.0 * ce);
        l->sn[face][k] = (int8_t)lrint(127.0 * cn);
        if (s->hints) {
            offset_ll(p, e, nn, 0.0, 0.0, &lat, &lon);
            const uint8_t *c = s->hints + 3 * stage_cell(s, lat, lon);
            l->override[face][k] = (uint8_t)(c[0] <= 4 ? c[0] : 0);
            l->rough[face][k] = (uint8_t)lrint(rough / count);
            l->nosettle[face][k] = (uint8_t)nosettle;
        }
    }
}

static void land_alloc(Land *l, int mip, int n, int hints) {
    l->mip = mip; l->n = n; l->g = n + 2 * APRON; l->hints = hints;
    size_t cells = (size_t)l->g * (size_t)l->g;
    for (int f = 0; f < 6; ++f) {
        l->relief[f] = (int16_t *)xmalloc(cells * sizeof(int16_t));
        l->se[f] = (int8_t *)xmalloc(cells);
        l->sn[f] = (int8_t *)xmalloc(cells);
        l->override[f] = hints ? (uint8_t *)xmalloc(cells) : NULL;
        l->rough[f] = hints ? (uint8_t *)xmalloc(cells) : NULL;
        l->nosettle[f] = hints ? (uint8_t *)xmalloc(cells) : NULL;
    }
}

static void land_summary(Land *l) {
    l->relief_max = 0.0f;
    for (int f = 0; f < 6; ++f)
        for (size_t k = 0; k < (size_t)l->g * l->g; ++k)
            if (l->relief[f][k] > l->relief_max) l->relief_max = l->relief[f][k];
}

/* --- The seams ---------------------------------------------------------------------- */

/* Each apron sample against the face it lies over, interpolated between that
 * face's samples: the mean and largest difference over a mip. The two aren't
 * the same estimate (an area average against an interpolation), so this
 * measures the seam's roughness, not an error. */
static void seam_check(const Mip *m, double *mean, double *worst, int *worst_face) {
    int n = m->n, g = m->g;
    double sum = 0.0, big = 0.0;
    long count = 0;
    *worst_face = 0;
    for (int f = 0; f < 6; ++f) {
        for (int j = -APRON; j < n + APRON; ++j) {
            for (int i = -APRON; i < n + APRON; ++i) {
                if (i >= 0 && i < n && j >= 0 && j < n) continue;
                TerrainDir p = terrain_face_dir(f, (double)(2 * i + 1) / n - 1.0, (double)(2 * j + 1) / n - 1.0);
                double u, v;
                int nf = terrain_dir_face(p, &u, &v);
                double x = (u + 1.0) * 0.5 * n - 0.5, y = (v + 1.0) * 0.5 * n - 0.5;
                int x0 = (int)floor(x), y0 = (int)floor(y);
                double fx = x - x0, fy = y - y0;
                if (x0 < -APRON) { x0 = -APRON; fx = 0.0; }
                if (y0 < -APRON) { y0 = -APRON; fy = 0.0; }
                if (x0 > n + APRON - 2) { x0 = n + APRON - 2; fx = 1.0; }
                if (y0 > n + APRON - 2) { y0 = n + APRON - 2; fy = 1.0; }
                const int16_t *q = m->q[nf] + (size_t)(y0 + APRON) * g + (x0 + APRON);
                double h = (1.0 - fy) * ((1.0 - fx) * q[0] + fx * q[1]) + fy * ((1.0 - fx) * q[g] + fx * q[g + 1]);
                double d = fabs(h - m->q[f][(size_t)(j + APRON) * g + (i + APRON)]);
                sum += d; ++count;
                if (d > big) { big = d; *worst_face = f; }
            }
        }
    }
    *mean = count ? sum / count : 0.0;
    *worst = big;
}

/* --- The water ------------------------------------------------------------------------------ */
/* The staged rivers and lakes kept (by the --water options), each line's
 * points taken to directions and stored in f32, then listed in the buckets of
 * every face they come near, and each bucket's centre tested against the
 * lakes' rings. Everything after the parse works from the f32 points, as the
 * engine will. */

typedef struct WaterOpts { double min_discharge, min_lake, tolerance; } WaterOpts;

static double comp3(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

static TerrainDir water_point(const Water *w, uint32_t k) {
    TerrainDir p = { w->xyz[3 * k], w->xyz[3 * k + 1], w->xyz[3 * k + 2] };
    return p;
}

/* The distance (chord, on the unit sphere) from p to the arc a b: to the
 * point of the chord nearest p, taken out to the sphere. */
static double arc_dist(TerrainDir p, TerrainDir a, TerrainDir b) {
    double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    double px = p.x - a.x, py = p.y - a.y, pz = p.z - a.z;
    double dd = dx * dx + dy * dy + dz * dz;
    double t = dd > 0.0 ? (px * dx + py * dy + pz * dz) / dd : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    double cx = a.x + t * dx, cy = a.y + t * dy, cz = a.z + t * dz, len = sqrt(cx * cx + cy * cy + cz * cz);
    if (len > 0.0) { cx /= len; cy /= len; cz /= len; }
    return sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) + (p.z - cz) * (p.z - cz));
}

/* p on face f's gnomonic plane (great circles are straight lines on it), if
 * it lies far enough up the face's axis. */
static int face_gnomonic(int f, TerrainDir p, double *gx, double *gy) {
    int a = f / 2;
    double n = ((f & 1) ? -1.0 : 1.0) * comp3(p, a);
    if (n < WATER_FACE_COS) return 0;
    *gx = comp3(p, (a + 1) % 3) / n;
    *gy = comp3(p, (a + 2) % 3) / n;
    return 1;
}

static int water_cell(double g, int B) {
    int i = (int)floor((atan(g) * (4.0 / PI) + 1.0) * 0.5 * B);
    return i < 0 ? 0 : i >= B ? B - 1 : i;
}

static TerrainDir bucket_centre(int f, int i, int j, int B) {
    return terrain_face_dir(f, -1.0 + (2.0 * i + 1.0) / B, -1.0 + (2.0 * j + 1.0) / B);
}

/* Douglas-Peucker on directions, to tol (radians): marks the points kept. */
static void water_simplify(const TerrainDir *p, int n, double tol, unsigned char *keep, int *stack) {
    memset(keep, 0, (size_t)n);
    keep[0] = keep[n - 1] = 1;
    int top = 0;
    stack[top++] = 0; stack[top++] = n - 1;
    while (top > 0) {
        int j = stack[--top], i = stack[--top];
        if (j <= i + 1) continue;
        double worst = -1.0; int at = -1;
        for (int k = i + 1; k < j; ++k) {
            double d = arc_dist(p[k], p[i], p[j]);
            if (d > worst) { worst = d; at = k; }
        }
        if (worst > tol) { keep[at] = 1; stack[top++] = i; stack[top++] = at; stack[top++] = at; stack[top++] = j; }
    }
}

typedef struct WaterBuild {
    Water    *w;
    const double *rad;               /* the buckets' circumradii (radians), face 0's (every face's alike) */
    uint32_t *count[6], *start[6];   /* per face: entries per bucket, then where each begins */
    uint32_t *list[6];
    size_t    n[6];
    /* the rings, for the lakes' insides: */
    uint32_t  nrings, *ring_first, *ring_count;
    double   *ll;                    /* V x 2: each lake point's longitude (unwrapped along its ring) and latitude */
    double   *ring_box;              /* nrings x 4: lon lo, lon hi, lat lo, lat hi */
    uint32_t *cell_start, *cell_ring; /* 360 x 180 one-degree cells: the rings whose box meets each */
} WaterBuild;

/* Calls back for each bucket of face f segment k comes near; returns how many. */
static size_t water_segment(WaterBuild *wb, int f, uint32_t k, uint32_t *out) {
    const Water *w = wb->w;
    int B = w->B;
    TerrainDir a = water_point(w, k), b = water_point(w, k + 1);
    double ax, ay, bx, by;
    if (!face_gnomonic(f, a, &ax, &ay) || !face_gnomonic(f, b, &bx, &by)) return 0;
    double r = (WATER_REACH_M + 0.5 * w->width[k]) / PLANET_RADIUS;
    double L2 = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
    double slack = 0.125 * L2 + 1e-9;   /* the arc's sagitta over its chord, and rounding */
    double S = fmax(1.0 + ax * ax + ay * ay, 1.0 + bx * bx + by * by);   /* the gnomonic's largest scale */
    double m = 1.1 * (r + slack) * S;
    int i0 = water_cell(fmin(ax, bx) - m, B), i1 = water_cell(fmax(ax, bx) + m, B);
    int j0 = water_cell(fmin(ay, by) - m, B), j1 = water_cell(fmax(ay, by) + m, B);
    size_t n = 0;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (arc_dist(bucket_centre(f, i, j, B), a, b) <= r + slack + wb->rad[(size_t)j * B + i]) {
                if (out) out[n] = (uint32_t)((size_t)j * B + i);
                ++n;
            }
    return n;
}

static void water_face_task(void *ctx, int f) {
    WaterBuild *wb = (WaterBuild *)ctx;
    Water *w = wb->w;
    size_t cells = (size_t)w->B * w->B, total = 0;
    uint32_t *count = wb->count[f], *start = wb->start[f];
    uint32_t buf[4096], *big = NULL;
    size_t bigcap = 0;
    memset(count, 0, cells * sizeof(uint32_t));
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            for (size_t b = 0; b < cells; ++b) { start[b] = (uint32_t)total; total += count[b]; count[b] = 0; }
            wb->n[f] = total;
            wb->list[f] = (uint32_t *)xmalloc(total * sizeof(uint32_t));
        }
        for (uint32_t k = 0; k + 1 < w->V; ++k) {
            if (!(w->flags[k] & 1)) continue;
            size_t n = water_segment(wb, f, k, NULL);
            if (n == 0) continue;
            uint32_t *out = buf;
            if (n > sizeof buf / sizeof buf[0]) {
                if (n > bigcap) { free(big); bigcap = n; big = (uint32_t *)xmalloc(n * sizeof(uint32_t)); }
                out = big;
            }
            water_segment(wb, f, k, out);
            for (size_t e = 0; e < n; ++e) {
                if (pass == 0) ++count[out[e]];
                else wb->list[f][start[out[e]] + count[out[e]]++] = k;
            }
        }
    }
    free(big);
}

/* Even-odd: whether (lon, lat) lies inside ring r, its longitudes unwrapped. */
static int ring_holds(const WaterBuild *wb, uint32_t r, double lon, double lat) {
    const double *p = wb->ll + 2 * (size_t)wb->ring_first[r];
    int in = 0;
    for (uint32_t k = 0; k + 1 < wb->ring_count[r]; ++k) {
        double x0 = p[2 * k], y0 = p[2 * k + 1], x1 = p[2 * k + 2], y1 = p[2 * k + 3];
        if ((y0 > lat) != (y1 > lat) && lon < x0 + (lat - y0) * (x1 - x0) / (y1 - y0)) in ^= 1;
    }
    return in;
}

static void water_inside_task(void *ctx, int t) {
    WaterBuild *wb = (WaterBuild *)ctx;
    Water *w = wb->w;
    int B = w->B, f = t / B, j = t % B;
    for (int i = 0; i < B; ++i) {
        double lat, lon;
        terrain_dir_latlon(bucket_centre(f, i, j, B), &lat, &lon);
        int cx = (int)floor(lon) + 180, cy = (int)floor(lat) + 90;
        cx = ((cx % 360) + 360) % 360; cy = cy < 0 ? 0 : cy > 179 ? 179 : cy;
        size_t c = (size_t)cy * 360 + cx;
        int in = 0;
        for (uint32_t e = wb->cell_start[c]; e < wb->cell_start[c + 1]; ++e) {
            uint32_t r = wb->cell_ring[e];
            const double *box = wb->ring_box + 4 * (size_t)r;
            if (lat < box[2] || lat > box[3]) continue;
            for (int s = -1; s <= 1; ++s) {
                double L = lon + 360.0 * s;
                if (L >= box[0] && L <= box[1]) { in ^= ring_holds(wb, r, L, lat); break; }
            }
        }
        w->inside[((size_t)f * B + j) * B + i] = (uint8_t)in;
    }
}

static int water_build(Water *w, const Stage *s, int N, const WaterOpts *o, int threads) {
    memset(w, 0, sizeof *w);
    if (!s->water) return 1;
    const unsigned char *p = s->water, *end = s->water + s->water_bytes;
    if (s->water_bytes < 16 || memcmp(p, WATER_MAGIC, 8) != 0 || get_u32(p + 8) != 1) { fprintf(stderr, "earth_bake: the water file isn't NCWATER1 version 1\n"); return 0; }
    uint32_t lines = get_u32(p + 12);
    p += 16;
    size_t cap = 1 << 20, rcap = 1 << 16;
    w->xyz = (float *)xmalloc(cap * 3 * sizeof(float));
    w->width = (uint16_t *)xmalloc(cap * sizeof(uint16_t));
    w->kind = (uint8_t *)xmalloc(cap); w->flags = (uint8_t *)xmalloc(cap);
    WaterBuild wb;
    memset(&wb, 0, sizeof wb);
    wb.w = w;
    wb.ll = (double *)xmalloc(cap * 2 * sizeof(double));
    wb.ring_first = (uint32_t *)xmalloc(rcap * sizeof(uint32_t)); wb.ring_count = (uint32_t *)xmalloc(rcap * sizeof(uint32_t));
    size_t tcap = 0;
    TerrainDir *dir = NULL; double *lonlat = NULL; unsigned char *keep = NULL; int *stack = NULL;
    for (uint32_t l = 0; l < lines; ++l) {
        if (end - p < 20) { fprintf(stderr, "earth_bake: the water file is truncated\n"); return 0; }
        int kind = p[0];
        float width = get_f32(p + 8), size = get_f32(p + 12);
        uint32_t n = get_u32(p + 16);
        p += 20;
        if ((size_t)(end - p) / 16 < n) { fprintf(stderr, "earth_bake: the water file is truncated\n"); return 0; }
        const unsigned char *pts = p;
        p += (size_t)n * 16;
        if (kind != 1 && kind != 2) { fprintf(stderr, "earth_bake: a water line of kind %d\n", kind); return 0; }
        if (kind == 1 ? size < o->min_discharge : size < o->min_lake) continue;
        if (n < (kind == 1 ? 2u : 4u)) continue;
        if (n > tcap) {
            tcap = n;
            free(dir); free(lonlat); free(keep); free(stack);
            dir = (TerrainDir *)xmalloc(n * sizeof(TerrainDir)); lonlat = (double *)xmalloc(n * 2 * sizeof(double));
            keep = (unsigned char *)xmalloc(n); stack = (int *)xmalloc(4 * (size_t)n * sizeof(int));
        }
        for (uint32_t k = 0; k < n; ++k) {
            lonlat[2 * k] = get_f64(pts + 16 * k); lonlat[2 * k + 1] = get_f64(pts + 16 * k + 8);
            dir[k] = terrain_latlon_dir(lonlat[2 * k + 1], lonlat[2 * k]);
        }
        if (o->tolerance > 0.0) water_simplify(dir, (int)n, o->tolerance / PLANET_RADIUS, keep, stack);
        else memset(keep, 1, n);
        uint32_t first = w->V, kept = 0;
        double unwrap = 0.0, prev = lonlat[0];
        for (uint32_t k = 0; k < n; ++k) {
            if (!keep[k]) continue;
            float x = (float)dir[k].x, y = (float)dir[k].y, z = (float)dir[k].z;
            if (kept > 0 && x == w->xyz[3 * (w->V - 1)] && y == w->xyz[3 * (w->V - 1) + 1] && z == w->xyz[3 * (w->V - 1) + 2]) continue;
            if (w->V == cap) {
                cap *= 2;
                w->xyz = (float *)realloc(w->xyz, cap * 3 * sizeof(float)); w->width = (uint16_t *)realloc(w->width, cap * sizeof(uint16_t));
                w->kind = (uint8_t *)realloc(w->kind, cap); w->flags = (uint8_t *)realloc(w->flags, cap);
                wb.ll = (double *)realloc(wb.ll, cap * 2 * sizeof(double));
                if (!w->xyz || !w->width || !w->kind || !w->flags || !wb.ll) { fprintf(stderr, "earth_bake: out of memory for the water\n"); exit(1); }
            }
            double lon = lonlat[2 * k];   /* unwrapped along the line */
            double d = lon - prev;
            if (d > 180.0) unwrap -= 360.0; else if (d < -180.0) unwrap += 360.0;
            prev = lon;
            w->xyz[3 * w->V] = x; w->xyz[3 * w->V + 1] = y; w->xyz[3 * w->V + 2] = z;
            double wm = kind == 1 ? floor(width + 0.5) : 0.0;
            w->width[w->V] = (uint16_t)(wm > 65535.0 ? 65535.0 : wm < 1.0 && kind == 1 ? 1.0 : wm);
            w->kind[w->V] = (uint8_t)kind; w->flags[w->V] = 1;
            wb.ll[2 * w->V] = lon + unwrap; wb.ll[2 * w->V + 1] = lonlat[2 * k + 1];
            ++w->V; ++kept;
        }
        if (kept < (kind == 1 ? 2u : 4u)) { w->V = first; continue; }
        w->flags[w->V - 1] = 0;
        if (kind == 1) { ++w->rivers; continue; }
        if (wb.nrings == rcap) {
            rcap *= 2;
            wb.ring_first = (uint32_t *)realloc(wb.ring_first, rcap * sizeof(uint32_t)); wb.ring_count = (uint32_t *)realloc(wb.ring_count, rcap * sizeof(uint32_t));
            if (!wb.ring_first || !wb.ring_count) { fprintf(stderr, "earth_bake: out of memory for the water\n"); exit(1); }
        }
        wb.ring_first[wb.nrings] = first; wb.ring_count[wb.nrings] = kept; ++wb.nrings;
    }
    free(dir); free(lonlat); free(keep); free(stack);
    if (p != end) { fprintf(stderr, "earth_bake: the water file runs on past its %u lines\n", lines); return 0; }
    w->rings = wb.nrings;
    if (w->V == 0) { free(w->xyz); free(w->width); free(w->kind); free(w->flags); free(wb.ll); free(wb.ring_first); free(wb.ring_count); memset(w, 0, sizeof *w); return 1; }

    w->B = N / WATER_DIV < WATER_MIN_B ? WATER_MIN_B : N / WATER_DIV;
    w->reach = (float)WATER_REACH_M;
    int B = w->B;
    size_t cells = (size_t)B * B;
    double *rad = (double *)xmalloc(cells * sizeof(double));   /* the angle from each bucket's centre to its farthest corner */
    for (int j = 0; j < B; ++j)
        for (int i = 0; i < B; ++i) {
            TerrainDir c = bucket_centre(0, i, j, B);
            double worst = 0.0;
            for (int k = 0; k < 4; ++k) {
                TerrainDir q = terrain_face_dir(0, -1.0 + 2.0 * (i + (k & 1)) / B, -1.0 + 2.0 * (j + (k >> 1)) / B);
                double ch = sqrt((c.x - q.x) * (c.x - q.x) + (c.y - q.y) * (c.y - q.y) + (c.z - q.z) * (c.z - q.z));
                double ang = 2.0 * asin(0.5 * ch);
                if (ang > worst) worst = ang;
            }
            rad[(size_t)j * B + i] = worst;
        }
    wb.rad = rad;
    for (int f = 0; f < 6; ++f) { wb.count[f] = (uint32_t *)xmalloc(cells * sizeof(uint32_t)); wb.start[f] = (uint32_t *)xmalloc(cells * sizeof(uint32_t)); }
    thread_parallel_for(6, threads, water_face_task, &wb);
    size_t E = 0;
    for (int f = 0; f < 6; ++f) E += wb.n[f];
    if (E > 0xffffffffu) { fprintf(stderr, "earth_bake: too many water bucket entries (%zu)\n", E); return 0; }
    w->E = (uint32_t)E;
    w->start = (uint32_t *)xmalloc((6 * cells + 1) * sizeof(uint32_t));
    w->entry = (uint32_t *)xmalloc(E * sizeof(uint32_t));
    size_t at = 0;
    for (int f = 0; f < 6; ++f) {
        for (size_t b = 0; b < cells; ++b) w->start[f * cells + b] = (uint32_t)(at + wb.start[f][b]);
        memcpy(w->entry + at, wb.list[f], wb.n[f] * sizeof(uint32_t));
        at += wb.n[f];
        free(wb.count[f]); free(wb.start[f]); free(wb.list[f]);
    }
    w->start[6 * cells] = (uint32_t)at;
    free(rad);

    /* the rings' boxes, and the one-degree cells each meets */
    wb.ring_box = (double *)xmalloc((size_t)wb.nrings * 4 * sizeof(double));
    wb.cell_start = (uint32_t *)xmalloc((360 * 180 + 1) * sizeof(uint32_t));
    uint32_t *ccount = (uint32_t *)xmalloc(360 * 180 * sizeof(uint32_t));
    memset(ccount, 0, 360 * 180 * sizeof(uint32_t));
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 1) {
            uint32_t tot = 0;
            for (int c = 0; c < 360 * 180; ++c) { wb.cell_start[c] = tot; tot += ccount[c]; ccount[c] = 0; }
            wb.cell_start[360 * 180] = tot;
            wb.cell_ring = (uint32_t *)xmalloc((size_t)tot * sizeof(uint32_t) + 1);
        }
        for (uint32_t r = 0; r < wb.nrings; ++r) {
            double *box = wb.ring_box + 4 * (size_t)r;
            const double *q = wb.ll + 2 * (size_t)wb.ring_first[r];
            box[0] = box[1] = q[0]; box[2] = box[3] = q[1];
            for (uint32_t k = 1; k < wb.ring_count[r]; ++k) {
                box[0] = fmin(box[0], q[2 * k]); box[1] = fmax(box[1], q[2 * k]);
                box[2] = fmin(box[2], q[2 * k + 1]); box[3] = fmax(box[3], q[2 * k + 1]);
            }
            int x0 = (int)floor(box[0]), x1 = (int)floor(box[1]), y0 = (int)floor(box[2]) + 90, y1 = (int)floor(box[3]) + 90;
            if (x1 - x0 >= 360) { x0 = -180; x1 = 179; }
            y0 = y0 < 0 ? 0 : y0 > 179 ? 179 : y0; y1 = y1 < 0 ? 0 : y1 > 179 ? 179 : y1;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    size_t c = (size_t)y * 360 + (size_t)((((x + 180) % 360) + 360) % 360);
                    if (pass == 1) wb.cell_ring[wb.cell_start[c] + ccount[c]] = r;
                    ++ccount[c];
                }
        }
    }
    free(ccount);
    w->inside = (uint8_t *)xmalloc(6 * cells);
    thread_parallel_for(6 * B, threads, water_inside_task, &wb);
    free(wb.ll); free(wb.ring_first); free(wb.ring_count); free(wb.ring_box); free(wb.cell_start); free(wb.cell_ring);
    return 1;
}

static void water_print(const Water *w) {
    if (!w->B) { printf("water: none\n"); return; }
    size_t cells = 6 * (size_t)w->B * w->B, used = 0, wet = 0, most = 0;
    for (size_t b = 0; b < cells; ++b) {
        size_t n = w->start[b + 1] - w->start[b];
        used += n > 0; wet += w->inside[b]; if (n > most) most = n;
    }
    printf("water: %u river lines, %u lake rings, %u points, %u bucket entries; %d buckets a side (%.0f m), reach %.0f m; "
           "%zu buckets list lines (most %zu), %zu centres in lakes; %.1f MB\n",
           w->rivers, w->rings, w->V, w->E, w->B, PLANET_RADIUS * (PI / 2.0) / w->B, w->reach, used, most, wet,
           (16.0 * w->V + 4.0 * (cells + 1) + cells + 4.0 * w->E) / 1e6);
}

/* --- Writing and reading --------------------------------------------------------------- */

static int bake_write(const Bake *b, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "earth_bake: cannot write %s\n", path); return 0; }
    unsigned char hd[HEADER];
    memset(hd, 0, sizeof hd);
    memcpy(hd, MAGIC, 8);
    put_u32(hd + 8, VERSION); put_u32(hd + 12, (uint32_t)b->N); put_u32(hd + 16, APRON); put_u32(hd + 20, (uint32_t)b->mips); put_u32(hd + 24, BLOCK);
    put_f32(hd + 32, b->scale); put_f32(hd + 36, b->offset);
    put_f64(hd + 40, b->origin_lat); put_f64(hd + 48, b->origin_lon); put_f64(hd + 56, b->origin_heading);
    memcpy(hd + 64, b->source_sha, 64); memcpy(hd + 128, b->stage_sha, 64);
    put_u16(hd + 192, (uint16_t)b->mip[0].lo); put_u16(hd + 194, (uint16_t)b->mip[0].hi);
    put_f32(hd + 196, b->mip[0].slope_max); put_f32(hd + 200, b->mip[0].relief_max);
    put_u32(hd + 204, (uint32_t)b->W); put_u32(hd + 208, (uint32_t)b->H); put_f64(hd + 212, b->cell);
    put_u32(hd + 220, (uint32_t)b->land.mip); put_u32(hd + 224, b->land.hints ? 1u : 0u); put_f32(hd + 228, b->land.relief_max);
    const Water *w = &b->water;
    put_u32(hd + 232, (uint32_t)w->B); put_f32(hd + 236, w->reach); put_u32(hd + 240, w->V); put_u32(hd + 244, w->E);
    put_u32(hd + 248, w->rivers); put_u32(hd + 252, w->rings);
    int ok = fwrite(hd, 1, HEADER, f) == HEADER;
    for (int m = 0; m < b->mips && ok; ++m) {
        unsigned char row[16];
        memset(row, 0, sizeof row);
        put_u16(row, (uint16_t)b->mip[m].lo); put_u16(row + 2, (uint16_t)b->mip[m].hi);
        put_f32(row + 4, b->mip[m].slope_max); put_f32(row + 8, b->mip[m].relief_max);
        ok = fwrite(row, 1, 16, f) == 16;
    }
    for (int m = 0; m < b->mips && ok; ++m) {
        const Mip *mp = &b->mip[m];
        size_t cells = (size_t)mp->g * mp->g, blocks = (size_t)mp->nb * mp->nb;
        for (int face = 0; face < 6 && ok; ++face) {
            ok = write_i16s(f, mp->q[face], cells) && write_i16s(f, mp->bmin[face], blocks) && write_i16s(f, mp->bmax[face], blocks) &&
                 write_f32s(f, mp->bslope[face], blocks) && write_f32s(f, mp->brelief[face], blocks);
        }
    }
    const Land *l = &b->land;
    size_t lcells = (size_t)l->g * l->g;
    for (int face = 0; face < 6 && ok; ++face) {
        ok = write_i16s(f, l->relief[face], lcells) && fwrite(l->se[face], 1, lcells, f) == lcells && fwrite(l->sn[face], 1, lcells, f) == lcells;
        if (ok && l->hints)
            ok = fwrite(l->override[face], 1, lcells, f) == lcells && fwrite(l->rough[face], 1, lcells, f) == lcells && fwrite(l->nosettle[face], 1, lcells, f) == lcells;
    }
    if (ok && w->B) {
        unsigned char rec[16 * 256];
        for (uint32_t k = 0; k < w->V && ok; k += 256) {
            uint32_t n = w->V - k < 256 ? w->V - k : 256;
            for (uint32_t i = 0; i < n; ++i) {
                unsigned char *r = rec + 16 * i;
                put_f32(r, w->xyz[3 * (k + i)]); put_f32(r + 4, w->xyz[3 * (k + i) + 1]); put_f32(r + 8, w->xyz[3 * (k + i) + 2]);
                put_u16(r + 12, w->width[k + i]); r[14] = w->kind[k + i]; r[15] = w->flags[k + i];
            }
            ok = fwrite(rec, 16, n, f) == n;
        }
        size_t cells = 6 * (size_t)w->B * w->B;
        unsigned char buf[4096];
        for (size_t k = 0; k <= cells && ok; k += 1024) {
            size_t n = cells + 1 - k < 1024 ? cells + 1 - k : 1024;
            for (size_t i = 0; i < n; ++i) put_u32(buf + 4 * i, w->start[k + i]);
            ok = fwrite(buf, 4, n, f) == n;
        }
        ok = ok && fwrite(w->inside, 1, cells, f) == cells;
        for (size_t k = 0; k < w->E && ok; k += 1024) {
            size_t n = w->E - k < 1024 ? w->E - k : 1024;
            for (size_t i = 0; i < n; ++i) put_u32(buf + 4 * i, w->entry[k + i]);
            ok = fwrite(buf, 4, n, f) == n;
        }
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "earth_bake: writing %s failed\n", path);
    return ok;
}

static int bake_read(Bake *b, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "earth_bake: cannot open %s\n", path); return 0; }
    memset(b, 0, sizeof *b);
    unsigned char hd[HEADER];
    if (fread(hd, 1, HEADER, f) != HEADER || memcmp(hd, MAGIC, 8) != 0 || get_u32(hd + 8) != VERSION) { fprintf(stderr, "earth_bake: %s isn't an earth bake (version %d)\n", path, VERSION); fclose(f); return 0; }
    b->N = (int)get_u32(hd + 12); b->mips = (int)get_u32(hd + 20);
    if (get_u32(hd + 16) != APRON || get_u32(hd + 24) != BLOCK || b->mips < 1 || b->mips > MAX_MIPS || b->N < MIN_FACE || (b->N >> (b->mips - 1)) % BLOCK != 0) {
        fprintf(stderr, "earth_bake: %s has an apron, block or mip count this tool can't read\n", path); fclose(f); return 0;
    }
    b->scale = get_f32(hd + 32); b->offset = get_f32(hd + 36);
    b->origin_lat = get_f64(hd + 40); b->origin_lon = get_f64(hd + 48); b->origin_heading = get_f64(hd + 56);
    memcpy(b->source_sha, hd + 64, 64); memcpy(b->stage_sha, hd + 128, 64);
    b->W = (int)get_u32(hd + 204); b->H = (int)get_u32(hd + 208); b->cell = get_f64(hd + 212);
    int lmip = (int)get_u32(hd + 220);
    if (lmip >= b->mips) { fprintf(stderr, "earth_bake: %s puts its landform grid at mip %d of %d\n", path, lmip, b->mips); fclose(f); return 0; }
    b->land.relief_max = get_f32(hd + 228);
    int ok = 1;
    for (int m = 0; m < b->mips && ok; ++m) {
        unsigned char row[16];
        ok = fread(row, 1, 16, f) == 16;
        b->mip[m].lo = (int16_t)get_u16(row); b->mip[m].hi = (int16_t)get_u16(row + 2);
        b->mip[m].slope_max = get_f32(row + 4); b->mip[m].relief_max = get_f32(row + 8);
    }
    for (int m = 0; m < b->mips && ok; ++m) {
        Mip *mp = &b->mip[m];
        mip_alloc(mp, b->N >> m, 0);
        size_t cells = (size_t)mp->g * mp->g, blocks = (size_t)mp->nb * mp->nb;
        for (int face = 0; face < 6 && ok; ++face) {
            ok = read_i16s(f, mp->q[face], cells) && read_i16s(f, mp->bmin[face], blocks) && read_i16s(f, mp->bmax[face], blocks) &&
                 read_f32s(f, mp->bslope[face], blocks) && read_f32s(f, mp->brelief[face], blocks);
        }
    }
    Land *l = &b->land;
    land_alloc(l, lmip, b->N >> lmip, (get_u32(hd + 224) & 1u) != 0);
    size_t lcells = (size_t)l->g * l->g;
    for (int face = 0; face < 6 && ok; ++face) {
        ok = read_i16s(f, l->relief[face], lcells) && fread(l->se[face], 1, lcells, f) == lcells && fread(l->sn[face], 1, lcells, f) == lcells;
        if (ok && l->hints)
            ok = fread(l->override[face], 1, lcells, f) == lcells && fread(l->rough[face], 1, lcells, f) == lcells && fread(l->nosettle[face], 1, lcells, f) == lcells;
    }
    Water *w = &b->water;
    w->B = (int)get_u32(hd + 232); w->reach = get_f32(hd + 236); w->V = get_u32(hd + 240); w->E = get_u32(hd + 244);
    w->rivers = get_u32(hd + 248); w->rings = get_u32(hd + 252);
    if (ok && w->B) {
        size_t cells = 6 * (size_t)w->B * w->B;
        w->xyz = (float *)xmalloc((size_t)w->V * 3 * sizeof(float)); w->width = (uint16_t *)xmalloc((size_t)w->V * sizeof(uint16_t));
        w->kind = (uint8_t *)xmalloc(w->V); w->flags = (uint8_t *)xmalloc(w->V);
        w->start = (uint32_t *)xmalloc((cells + 1) * sizeof(uint32_t)); w->inside = (uint8_t *)xmalloc(cells);
        w->entry = (uint32_t *)xmalloc((size_t)w->E * sizeof(uint32_t) + 1);
        unsigned char rec[16];
        for (uint32_t k = 0; k < w->V && ok; ++k) {
            ok = fread(rec, 1, 16, f) == 16;
            w->xyz[3 * k] = get_f32(rec); w->xyz[3 * k + 1] = get_f32(rec + 4); w->xyz[3 * k + 2] = get_f32(rec + 8);
            w->width[k] = get_u16(rec + 12); w->kind[k] = rec[14]; w->flags[k] = rec[15];
        }
        for (size_t k = 0; k <= cells && ok; ++k) { ok = fread(rec, 1, 4, f) == 4; w->start[k] = get_u32(rec); }
        ok = ok && fread(w->inside, 1, cells, f) == cells;
        for (size_t k = 0; k < w->E && ok; ++k) { ok = fread(rec, 1, 4, f) == 4; w->entry[k] = get_u32(rec); }
        if (ok && w->start[cells] != w->E) ok = 0;
    }
    if (ok && fgetc(f) != EOF) ok = 0;
    fclose(f);
    if (!ok) fprintf(stderr, "earth_bake: %s is truncated or overlong\n", path);
    return ok;
}

static void bake_print(const Bake *b, int seams) {
    printf("size %d, %d mips, apron %d, block %d; scale %g m, offset %g m\n", b->N, b->mips, APRON, BLOCK, b->scale, b->offset);
    printf("origin %.6f %.6f, heading %.2f; stage %d x %d cells of %g degrees\n", b->origin_lat, b->origin_lon, b->origin_heading, b->W, b->H, b->cell);
    printf("source sha256 %.64s\nstage  sha256 %.64s\n", b->source_sha, b->stage_sha);
    printf("%4s %6s %9s %7s %7s %9s %9s", "mip", "n", "m/sample", "min", "max", "grade", "relief");
    if (seams) printf(" %10s %10s %5s", "seam mean", "seam max", "face");
    printf("\n");
    for (int m = 0; m < b->mips; ++m) {
        const Mip *mp = &b->mip[m];
        printf("%4d %6d %9.0f %7d %7d %9.4f %9.0f", m, mp->n, PLANET_RADIUS * (PI / 2.0) / mp->n, mp->lo, mp->hi, mp->slope_max, mp->relief_max);
        if (seams) {
            double mean, worst; int face;
            seam_check(mp, &mean, &worst, &face);
            printf(" %10.2f %10.1f %5d", mean, worst, face);
        }
        printf("\n");
    }
    printf("landform grid: mip %d, %d a side (%.0f m a sample), relief up to %.0f m within %.0f km; %s\n", b->land.mip, b->land.n,
           PLANET_RADIUS * (PI / 2.0) / (b->land.n ? b->land.n : 1), b->land.relief_max, LAND_RELIEF_M / 1000.0, b->land.hints ? "with the hints" : "no hints");
    water_print(&b->water);
}

/* --- Baking -------------------------------------------------------------------------- */

static int bake(const char *stage_path, const char *out_path, int N, int threads, const WaterOpts *wo) {
    if (N < MIN_FACE || (N & (N - 1)) != 0) { fprintf(stderr, "earth_bake: --size must be a power of two, at least %d\n", MIN_FACE); return 0; }
    double t0 = now_s();
    Stage s;
    if (!stage_read(&s, stage_path)) return 0;
    printf("stage %d x %d (%s)%s%s, checksum verified, %.1f s\n", s.W, s.H, stage_path, s.hints ? " with hints" : "", s.water ? " with water" : "", now_s() - t0);

    Bake *b = (Bake *)xmalloc(sizeof(Bake));
    memset(b, 0, sizeof *b);
    b->N = N; b->W = s.W; b->H = s.H; b->cell = s.cell;
    b->origin_lat = s.origin_lat; b->origin_lon = s.origin_lon; b->origin_heading = s.origin_heading;
    b->scale = 1.0f; b->offset = 0.0f;
    memcpy(b->source_sha, s.source_sha, 64); memcpy(b->stage_sha, s.stage_sha, 64);
    for (int n = N; n >= MIN_FACE; n >>= 1) ++b->mips;

    for (int m = 0; m < b->mips; ++m) {
        Mip *mp = &b->mip[m];
        double t1 = now_s();
        mip_alloc(mp, N >> m, 1);
        Resample r;
        r.s = &s; r.n = mp->n; r.g = mp->g; r.K = sub_cells(mp->n, s.cell);
        r.bands = (mp->g + BAND - 1) / BAND; r.apron_only = m > 0;
        for (int f = 0; f < 6; ++f) r.out[f] = mp->f[f];
        if (m > 0) {
            Downsample d = { &b->mip[m - 1], mp };
            thread_parallel_for(6 * mp->n, threads, downsample_task, &d);
        }
        thread_parallel_for(6 * r.bands, threads, resample_task, &r);
        Quantise qz = { mp };
        thread_parallel_for(6 * mp->g, threads, quantise_task, &qz);
        Blocks bl = { mp, m > 0 ? &b->mip[m - 1] : NULL, (int)floor(RELIEF_M / (PLANET_RADIUS * (PI / 2.0) / N) + 0.5) };
        thread_parallel_for(6 * mp->nb, threads, blocks_task, &bl);
        mip_summary(mp);
        printf("mip %d: %d a side, %d x %d sub-cells%s, %.1f s\n", m, mp->n, r.K, r.K, m > 0 ? " for the apron" : "", now_s() - t1);
        if (m > 0) { for (int f = 0; f < 6; ++f) { free(b->mip[m - 1].f[f]); b->mip[m - 1].f[f] = NULL; } }
    }
    double t2 = now_s();
    int lmip = 0;
    while (lmip + 1 < b->mips && PLANET_RADIUS * (PI / 2.0) / (N >> lmip) < LAND_STEP_M) ++lmip;
    land_alloc(&b->land, lmip, N >> lmip, s.hints != NULL);
    coarse_build(&s);
    LandTask lt = { &s, &b->land };
    thread_parallel_for(6 * b->land.g, threads, land_task, &lt);
    land_summary(&b->land);
    printf("landform grid: mip %d, %d a side, %.1f s\n", lmip, b->land.n, now_s() - t2);
    free(s.h); free(s.sinlat); free(s.hints); free(s.coarse);
    double t3 = now_s();
    if (!water_build(&b->water, &s, N, wo, threads)) return 0;
    free(s.water);
    if (b->water.B) printf("water: %.1f s\n", now_s() - t3);
    if (!bake_write(b, out_path)) return 0;
    printf("wrote %s, %.1f s in all\n", out_path, now_s() - t0);
    bake_print(b, 1);
    return 1;
}

/* --- Dumping a face ----------------------------------------------------------------- */

static void png_sink(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ctx); }

static unsigned char ramp(double a, double b, double t) { double v = a + (b - a) * t; return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5); }

/* The land's colour by height, the sea floor's by depth, lit from the upper left. */
static void shade(double h, double gx, double gy, double exaggerate, unsigned char *rgb) {
    double r, g, b;
    if (h < 0.0) {
        double t = -h / 8000.0; if (t > 1.0) t = 1.0;
        r = ramp(130, 10, t); g = ramp(180, 25, t); b = ramp(225, 80, t);
    } else {
        static const double stop[5][4] = { { 0, 90, 140, 70 }, { 800, 175, 165, 85 }, { 2000, 135, 95, 60 }, { 3500, 205, 205, 200 }, { 5500, 255, 255, 255 } };
        int k = 0;
        while (k < 3 && h > stop[k + 1][0]) ++k;
        double t = (h - stop[k][0]) / (stop[k + 1][0] - stop[k][0]); if (t > 1.0) t = 1.0;
        r = ramp(stop[k][1], stop[k + 1][1], t); g = ramp(stop[k][2], stop[k + 1][2], t); b = ramp(stop[k][3], stop[k + 1][3], t);
    }
    double nx = -gx * exaggerate, ny = -gy * exaggerate, nz = 1.0, len = sqrt(nx * nx + ny * ny + nz * nz);
    double lx = -0.5, ly = -0.5, lz = 0.7071;
    double lit = (nx * lx + ny * ly + nz * lz) / len;
    double k = 0.55 + 0.45 * (lit < 0.0 ? 0.0 : lit);
    rgb[0] = (unsigned char)(r * k); rgb[1] = (unsigned char)(g * k); rgb[2] = (unsigned char)(b * k);
}

static int dump(const char *bake_path, const char *png_path, int face, int mip) {
    Bake *b = (Bake *)xmalloc(sizeof(Bake));
    if (!bake_read(b, bake_path)) return 0;
    if (mip < 0) { mip = 0; while (mip + 1 < b->mips && (b->N >> mip) > 1024) ++mip; }
    if (mip >= b->mips) { fprintf(stderr, "earth_bake: %s has %d mips\n", bake_path, b->mips); return 0; }
    const Mip *m = &b->mip[mip];
    int g = m->g, gap = 4;
    int cols = face < 0 ? 3 : 1, rows = face < 0 ? 2 : 1;
    int W = cols * g + (cols - 1) * gap, H = rows * g + (rows - 1) * gap;
    unsigned char *img = (unsigned char *)xmalloc((size_t)W * H * 3);
    memset(img, 0, (size_t)W * H * 3);
    double spacing = PLANET_RADIUS * (PI / 2.0) / m->n;
    for (int k = 0; k < cols * rows; ++k) {
        int f = face < 0 ? k : face, ox = (k % cols) * (g + gap), oy = (k / cols) * (g + gap);
        const int16_t *q = m->q[f];
        for (int j = 0; j < g; ++j) {
            for (int i = 0; i < g; ++i) {
                double h = b->scale * q[(size_t)j * g + i] + b->offset;
                int i0 = i > 0 ? i - 1 : i, i1 = i < g - 1 ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < g - 1 ? j + 1 : j;
                double gx = (double)(q[(size_t)j * g + i1] - q[(size_t)j * g + i0]) * b->scale / ((i1 - i0) * spacing);
                double gy = (double)(q[(size_t)j1 * g + i] - q[(size_t)j0 * g + i]) * b->scale / ((j1 - j0) * spacing);
                shade(h, gx, gy, 3.0, img + ((size_t)(oy + j) * W + (ox + i)) * 3);
            }
        }
    }
    FILE *out = fopen(png_path, "wb");
    if (!out) { fprintf(stderr, "earth_bake: cannot write %s\n", png_path); return 0; }
    int ok = stbi_write_png_to_func(png_sink, out, W, H, 3, img, W * 3);
    fclose(out);
    free(img);
    if (!ok) { fprintf(stderr, "earth_bake: writing %s failed\n", png_path); return 0; }
    printf("wrote %s: mip %d, %d x %d (the outer %d samples of each face are its apron)\n", png_path, mip, W, H, APRON);
    return 1;
}

/* --- main ----------------------------------------------------------------------------- */

static void usage(void) {
    fprintf(stderr,
        "usage: earth_bake <stage.r16> <out.bake> [--size N] [--threads T]\n"
        "                  [--water-min-discharge Q] [--water-min-lake A] [--water-tolerance M]\n"
        "       earth_bake --info <bake>\n"
        "       earth_bake --dump-face <k|all> <bake> <out.png> [--mip M]\n");
}

int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--info")) {
        Bake *b = (Bake *)xmalloc(sizeof(Bake));
        if (!bake_read(b, argv[2])) return 1;
        bake_print(b, 1);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "--dump-face")) {
        int face = !strcmp(argv[2], "all") ? -1 : atoi(argv[2]), mip = -1;
        if (face < -1 || face > 5) { usage(); return 1; }
        for (int i = 5; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "--mip")) mip = atoi(argv[i + 1]);
            else { usage(); return 1; }
        }
        return dump(argv[3], argv[4], face, mip) ? 0 : 1;
    }
    if (argc < 3) { usage(); return 1; }
    int N = 4096, threads = cpu_count();
    WaterOpts wo = { 0.0, 0.0, 0.0 };
    for (int i = 3; i < argc; i += 2) {
        if (i + 1 >= argc) { usage(); return 1; }
        if (!strcmp(argv[i], "--size")) N = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--threads")) threads = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--water-min-discharge")) wo.min_discharge = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--water-min-lake")) wo.min_lake = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--water-tolerance")) wo.tolerance = atof(argv[i + 1]);
        else { usage(); return 1; }
    }
    if (threads < 1) threads = 1;
    return bake(argv[1], argv[2], N, threads, &wo) ? 0 : 1;
}
