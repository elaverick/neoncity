/* The water section of the Earth tools' files (see earth_water.h): bucketing
 * the lines, testing the buckets' centres against the lakes' rings, writing
 * and reading. Everything works from the f32 points, as the engine will. */
#include "earth_water.h"
#include "thread.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI             3.14159265358979323846
#define WATER_FACE_COS 0.2   /* a segment is bucketed on a face where both its ends lie this far up its axis */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "earth water: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}

static void put_u16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put_u32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
static void put_f32(unsigned char *p, float v) { uint32_t u; memcpy(&u, &v, 4); put_u32(p, u); }
static uint16_t get_u16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get_u32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float    get_f32(const unsigned char *p) { uint32_t u = get_u32(p); float v; memcpy(&v, &u, 4); return v; }

static double comp3(TerrainDir a, int k) { return k == 0 ? a.x : k == 1 ? a.y : a.z; }

TerrainDir water_point(const Water *w, uint32_t k) {
    TerrainDir p = { w->xyz[3 * k], w->xyz[3 * k + 1], w->xyz[3 * k + 2] };
    return p;
}

double water_arc_dist(TerrainDir p, TerrainDir a, TerrainDir b) {
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

void water_simplify(const TerrainDir *p, int n, double tol, unsigned char *keep, int *stack) {
    memset(keep, 0, (size_t)n);
    keep[0] = keep[n - 1] = 1;
    int top = 0;
    stack[top++] = 0; stack[top++] = n - 1;
    while (top > 0) {
        int j = stack[--top], i = stack[--top];
        if (j <= i + 1) continue;
        double worst = -1.0; int at = -1;
        for (int k = i + 1; k < j; ++k) {
            double d = water_arc_dist(p[k], p[i], p[j]);
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
    const WaterRings *rings;
    double   *ring_box;              /* rings x 4: lon lo, lon hi, lat lo, lat hi */
    uint32_t *cell_start, *cell_ring; /* 360 x 180 one-degree cells: the rings whose box meets each */
} WaterBuild;

/* Calls back for each bucket of face f segment k comes near; returns how many. */
static size_t water_segment(WaterBuild *wb, int f, uint32_t k, uint32_t *out) {
    const Water *w = wb->w;
    int B = w->B;
    TerrainDir a = water_point(w, k), b = water_point(w, k + 1);
    double ax, ay, bx, by;
    if (!face_gnomonic(f, a, &ax, &ay) || !face_gnomonic(f, b, &bx, &by)) return 0;
    double r = (w->reach + 0.5 * w->width[k]) / PLANET_RADIUS;
    double L2 = (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
    double slack = 0.125 * L2 + 1e-9;   /* the arc's sagitta over its chord, and rounding */
    double S = fmax(1.0 + ax * ax + ay * ay, 1.0 + bx * bx + by * by);   /* the gnomonic's largest scale */
    double m = 1.1 * (r + slack) * S;
    int i0 = water_cell(fmin(ax, bx) - m, B), i1 = water_cell(fmax(ax, bx) + m, B);
    int j0 = water_cell(fmin(ay, by) - m, B), j1 = water_cell(fmax(ay, by) + m, B);
    size_t n = 0;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (water_arc_dist(bucket_centre(f, i, j, B), a, b) <= r + slack + wb->rad[(size_t)j * B + i]) {
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
    const double *p = wb->rings->ll + 2 * (size_t)wb->rings->first[r];
    int in = 0;
    for (uint32_t k = 0; k + 1 < wb->rings->count[r]; ++k) {
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

int water_index(Water *w, int B, double reach, const WaterRings *rings, int threads) {
    w->B = B;
    w->reach = (float)reach;
    WaterBuild wb;
    memset(&wb, 0, sizeof wb);
    wb.w = w;
    wb.rings = rings;
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
    if (E > 0xffffffffu) { fprintf(stderr, "earth water: too many bucket entries (%zu)\n", E); return 0; }
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

    w->inside = (uint8_t *)xmalloc(6 * cells);
    memset(w->inside, 0, 6 * cells);
    uint32_t nrings = rings ? rings->n : 0;
    if (nrings == 0) return 1;
    /* the rings' boxes, and the one-degree cells each meets */
    wb.ring_box = (double *)xmalloc((size_t)nrings * 4 * sizeof(double));
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
        for (uint32_t r = 0; r < nrings; ++r) {
            double *box = wb.ring_box + 4 * (size_t)r;
            const double *q = rings->ll + 2 * (size_t)rings->first[r];
            box[0] = box[1] = q[0]; box[2] = box[3] = q[1];
            for (uint32_t k = 1; k < rings->count[r]; ++k) {
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
    thread_parallel_for(6 * B, threads, water_inside_task, &wb);
    free(wb.ring_box); free(wb.cell_start); free(wb.cell_ring);
    return 1;
}

int water_write(FILE *f, const Water *w) {
    int ok = 1;
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
    return ok;
}

int water_read(FILE *f, Water *w) {
    size_t cells = 6 * (size_t)w->B * w->B;
    w->xyz = (float *)xmalloc((size_t)w->V * 3 * sizeof(float)); w->width = (uint16_t *)xmalloc((size_t)w->V * sizeof(uint16_t));
    w->kind = (uint8_t *)xmalloc(w->V); w->flags = (uint8_t *)xmalloc(w->V);
    w->start = (uint32_t *)xmalloc((cells + 1) * sizeof(uint32_t)); w->inside = (uint8_t *)xmalloc(cells);
    w->entry = (uint32_t *)xmalloc((size_t)w->E * sizeof(uint32_t) + 1);
    unsigned char rec[16];
    int ok = 1;
    for (uint32_t k = 0; k < w->V && ok; ++k) {
        ok = fread(rec, 1, 16, f) == 16;
        w->xyz[3 * k] = get_f32(rec); w->xyz[3 * k + 1] = get_f32(rec + 4); w->xyz[3 * k + 2] = get_f32(rec + 8);
        w->width[k] = get_u16(rec + 12); w->kind[k] = rec[14]; w->flags[k] = rec[15];
    }
    for (size_t k = 0; k <= cells && ok; ++k) { ok = fread(rec, 1, 4, f) == 4; w->start[k] = get_u32(rec); }
    ok = ok && fread(w->inside, 1, cells, f) == cells;
    for (size_t k = 0; k < w->E && ok; ++k) { ok = fread(rec, 1, 4, f) == 4; w->entry[k] = get_u32(rec); }
    return ok && w->start[cells] == w->E;
}

void water_free(Water *w) {
    free(w->xyz); free(w->width); free(w->kind); free(w->flags); free(w->start); free(w->inside); free(w->entry);
    memset(w, 0, sizeof *w);
}

void water_stats(const Water *w, size_t *used, size_t *most, size_t *wet, double *mb) {
    size_t cells = 6 * (size_t)w->B * w->B;
    *used = *most = *wet = 0;
    for (size_t b = 0; b < cells; ++b) {
        size_t n = w->start[b + 1] - w->start[b];
        *used += n > 0; *wet += w->inside[b]; if (n > *most) *most = n;
    }
    *mb = (16.0 * w->V + 4.0 * (cells + 1) + cells + 4.0 * w->E) / 1e6;
}
