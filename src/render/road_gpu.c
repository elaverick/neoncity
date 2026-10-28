#include "renderer_internal.h"
#include "plan.h"

/* The roads between the settlements as the terrain draws them
 * (shaders/terrain/roads.glsl): their centre lines' segments within a window
 * ROAD_WINDOW square round the camera, filed by ROAD_GCELL squares, in one
 * host-visible storage buffer per frame slot. The window moves when the
 * camera strays ROAD_RECENTRE from its centre, rebuilt on the render thread
 * from the roads whose bounds reach it (a millisecond or two, now and then);
 * each frame writes its header (the window's corner relative to the camera,
 * so positions stay exact in float), and a slot that hasn't the latest
 * window yet takes it whole.
 *
 * The buffer, in 32-bit words (floats as their bits):
 *   0-7    the window's corner less the camera (x, z), the cell size, the
 *          cells a side, the segments, where the list and the segments begin
 *   8...   each cell's first entry in the list and how many, row by row
 *   list   segment numbers
 *   segs   ROAD_SEG_WORDS each: a.x, a.z, b.x, b.z (m from the corner), the
 *          distance along its road at a, its length, the road's half-width,
 *          its class (PLAN_ROAD_*), plus ROAD_SEG_BRIDGE on a bridge's span */
#define ROAD_WINDOW    32768.0
#define ROAD_GCELL     256.0
#define ROAD_GDIM      128
#define ROAD_RECENTRE  4096.0
#define ROAD_MAX_SEGS  16384
#define ROAD_MAX_LIST  131072
#define ROAD_HEADER    8
#define ROAD_SEG_WORDS 8
#define ROAD_SEG_BRIDGE 4         /* in a segment's class: it is on a bridge (shaders/terrain/roads.glsl) */
#define ROAD_CELL_REACH (PLAN_FREEWAY_HALF + 2.0)   /* a segment is filed in each cell within this of it (m) */
#define ROAD_WORDS     (ROAD_HEADER + 2 * ROAD_GDIM * ROAD_GDIM + ROAD_MAX_LIST + ROAD_SEG_WORDS * ROAD_MAX_SEGS)

_Static_assert(ROAD_GDIM * ROAD_GCELL == ROAD_WINDOW, "the road grid must cover its window");

struct RoadGpu {
    Buffer    buf[MAX_FRAMES];
    uint32_t  version[MAX_FRAMES];   /* the window each slot holds */
    uint32_t *data;                  /* the latest window's body (from the cells on) */
    uint32_t  words;                 /* how much of it is in use */
    uint32_t  latest;
    int       have;
    double    corner[2];             /* world x, z of the window's corner */
    int       nseg, nlist;
    double   *bounds;                /* each road's x0, z0, x1, z1 */
    int       last[ROAD_GDIM * ROAD_GDIM];   /* build_window's: the segment last filed in each cell */
};

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

void road_gpu_create(Renderer *r) {
    RoadGpu *g = (RoadGpu *)calloc(1, sizeof(RoadGpu));
    if (!g) fatal("out of memory allocating the roads");
    r->roads = g;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        g->buf[i] = vk_buffer(r->vk, (VkDeviceSize)ROAD_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        memset(g->buf[i].mapped, 0, (size_t)ROAD_WORDS * 4);   /* an empty window until the first update */
    }
    g->data = (uint32_t *)malloc((size_t)ROAD_WORDS * 4);
    int n = plan_road_count();
    g->bounds = (double *)malloc(sizeof(double) * 4 * (size_t)(n > 0 ? n : 1));
    if (!g->data || !g->bounds) fatal("out of memory allocating the roads");
    road_gpu_set_frame(r);
}

/* Each road's bounds in the active frame (TerrainFrame); a road the frame
 * doesn't hold has no points there, and none. */
void road_gpu_set_frame(Renderer *r) {
    RoadGpu *g = r->roads;
    for (int k = 0, n = plan_road_count(); k < n; ++k) {
        const PlanRoad *R = plan_road(k);
        double *b = &g->bounds[4 * k];
        b[0] = b[1] = 1e300; b[2] = b[3] = -1e300;
        for (int q = 0; q < R->npts; ++q) {
            b[0] = fmin(b[0], R->pts[q].x); b[1] = fmin(b[1], R->pts[q].z);
            b[2] = fmax(b[2], R->pts[q].x); b[3] = fmax(b[3], R->pts[q].z);
        }
    }
    g->have = 0;
}

void road_gpu_destroy(Renderer *r) {
    RoadGpu *g = r->roads;
    if (!g) return;
    for (int i = 0; i < MAX_FRAMES; ++i) vk_buffer_destroy(r->vk, &g->buf[i]);
    free(g->data);
    free(g->bounds);
    free(g);
    r->roads = NULL;
}

VkBuffer road_gpu_buffer(Renderer *r, int slot, VkDeviceSize *size) {
    *size = (VkDeviceSize)ROAD_WORDS * 4;
    return r->roads->buf[slot].buf;
}

/* Each cell of the window within ROAD_CELL_REACH of the segment a-b (window
 * metres), stepping along it: counted (pass 0) or filed (pass 1), once per
 * segment however often it is reached (last[] holds the segment that last
 * reached each cell). */
static void file_segment(RoadGpu *g, DVec2 a, DVec2 b, int seg, int pass) {
    uint32_t *cells = g->data, *list = &g->data[2 * ROAD_GDIM * ROAD_GDIM];
    int steps = (int)ceil(dlen(dsub(b, a)) / (0.5 * ROAD_GCELL)) + 1;
    for (int k = 0; k <= steps; ++k) {
        DVec2 q = dadd(a, dscale(dsub(b, a), (double)k / steps));
        int i0 = (int)floor((q.x - ROAD_CELL_REACH) / ROAD_GCELL), i1 = (int)floor((q.x + ROAD_CELL_REACH) / ROAD_GCELL);
        int j0 = (int)floor((q.z - ROAD_CELL_REACH) / ROAD_GCELL), j1 = (int)floor((q.z + ROAD_CELL_REACH) / ROAD_GCELL);
        for (int j = j0 < 0 ? 0 : j0; j <= j1 && j < ROAD_GDIM; ++j)
            for (int i = i0 < 0 ? 0 : i0; i <= i1 && i < ROAD_GDIM; ++i) {
                int c = j * ROAD_GDIM + i;
                if (g->last[c] == seg) continue;
                g->last[c] = seg;
                if (pass == 1) list[cells[2 * c] + cells[2 * c + 1]] = (uint32_t)seg;
                cells[2 * c + 1]++;
            }
    }
}

/* The window round (cx, cz): every road segment that reaches it, as many as
 * the list holds (segments past it are left out, nearest roads first only
 * by luck: the window is sized so that doesn't happen). */
static void build_window(RoadGpu *g, double cx, double cz) {
    g->corner[0] = floor(cx / 1024.0) * 1024.0 - 0.5 * ROAD_WINDOW;
    g->corner[1] = floor(cz / 1024.0) * 1024.0 - 0.5 * ROAD_WINDOW;
    const double x0 = g->corner[0], z0 = g->corner[1], x1 = x0 + ROAD_WINDOW, z1 = z0 + ROAD_WINDOW, m = ROAD_CELL_REACH;
    uint32_t *cells = g->data, *segs = &g->data[2 * ROAD_GDIM * ROAD_GDIM + ROAD_MAX_LIST];
    memset(cells, 0, sizeof(uint32_t) * 2 * ROAD_GDIM * ROAD_GDIM);
    int ns = 0;
    uint32_t total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        for (int c = 0; c < ROAD_GDIM * ROAD_GDIM; ++c) g->last[c] = -1;
        if (pass == 1) {   /* the counts become starts */
            uint32_t at = 0;
            for (int c = 0; c < ROAD_GDIM * ROAD_GDIM; ++c) { uint32_t n = cells[2 * c + 1]; cells[2 * c] = at; cells[2 * c + 1] = 0; at += n; }
        }
        ns = 0;
        for (int k = 0; k < plan_road_count(); ++k) {
            const double *b = &g->bounds[4 * k];
            if (b[2] < x0 - m || b[0] > x1 + m || b[3] < z0 - m || b[1] > z1 + m) continue;
            const PlanRoad *R = plan_road(k);
            for (int q = 0; q + 1 < R->npts; ++q) {
                DVec2 a = R->pts[q], bb = R->pts[q + 1];
                if (fmax(a.x, bb.x) < x0 - m || fmin(a.x, bb.x) > x1 + m || fmax(a.z, bb.z) < z0 - m || fmin(a.z, bb.z) > z1 + m) continue;
                if (ns >= ROAD_MAX_SEGS) continue;
                DVec2 la = dv(a.x - x0, a.z - z0), lb = dv(bb.x - x0, bb.z - z0);
                if (pass == 0) {
                    file_segment(g, la, lb, ns, 0);
                } else {
                    uint32_t *sg = &segs[(size_t)ns * ROAD_SEG_WORDS];
                    sg[0] = fbits((float)la.x); sg[1] = fbits((float)la.z);
                    sg[2] = fbits((float)lb.x); sg[3] = fbits((float)lb.z);
                    sg[4] = fbits((float)R->s[q]); sg[5] = fbits((float)(R->s[q + 1] - R->s[q]));
                    sg[6] = fbits(R->half); sg[7] = fbits((float)(R->cls + (R->bridge && R->bridge[q] >= 0 ? ROAD_SEG_BRIDGE : 0)));
                    file_segment(g, la, lb, ns, 1);
                }
                ns++;
            }
        }
        if (pass == 0) {
            total = 0;
            for (int c = 0; c < ROAD_GDIM * ROAD_GDIM; ++c) total += cells[2 * c + 1];
            if (total > ROAD_MAX_LIST) fatal("roads: the window's list overflowed (ROAD_MAX_LIST)");
        }
    }
    g->nseg = ns;
    g->nlist = (int)total;
    g->words = (uint32_t)(2 * ROAD_GDIM * ROAD_GDIM + ROAD_MAX_LIST + ROAD_SEG_WORDS * ns);
    g->latest++;
    g->have = 1;
}

void road_gpu_update(Renderer *r, int slot, const double cam[3]) {
    RoadGpu *g = r->roads;
    double cx = g->corner[0] + 0.5 * ROAD_WINDOW, cz = g->corner[1] + 0.5 * ROAD_WINDOW;
    if (!g->have || fabs(cam[0] - cx) > ROAD_RECENTRE || fabs(cam[2] - cz) > ROAD_RECENTRE) build_window(g, cam[0], cam[2]);
    uint32_t *dst = (uint32_t *)g->buf[slot].mapped;
    if (g->version[slot] != g->latest) {
        memcpy(dst + ROAD_HEADER, g->data, (size_t)g->words * 4);
        g->version[slot] = g->latest;
    }
    dst[0] = fbits((float)(g->corner[0] - cam[0]));
    dst[1] = fbits((float)(g->corner[1] - cam[2]));
    dst[2] = fbits((float)ROAD_GCELL);
    dst[3] = ROAD_GDIM;
    dst[4] = (uint32_t)g->nseg;
    dst[5] = ROAD_HEADER + 2 * ROAD_GDIM * ROAD_GDIM;                   /* the list */
    dst[6] = ROAD_HEADER + 2 * ROAD_GDIM * ROAD_GDIM + ROAD_MAX_LIST;   /* the segments */
    dst[7] = 0;
}

void road_gpu_stats(Renderer *r, char *buf, size_t n) {
    RoadGpu *g = r->roads;
    snprintf(buf, n, "roads: %d segments in the %.0f km window round the camera (%d cell entries)", g->nseg, ROAD_WINDOW / 1000.0, g->nlist);
}
