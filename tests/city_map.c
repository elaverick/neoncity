/* Renders a top-down map of the generated plan to a BMP, for eyeballing the
 * layout without the renderer:
 *
 *   city_map [out.bmp] [centreX centreZ] [sizeMetres] [metresPerPixel] [buildings 0|1]
 *
 * Defaults to a 6 km square around the central regional centre at 3 m/px.
 * Roads are coloured by class, blocks by zone, building sections by height;
 * the city's ground is dark asphalt, and the land beyond it black. A map of
 * the whole city (30 km at 20 m/px, say) is quicker without the buildings. */
#include "building.h"
#include "hash.h"
#include "mathlib.h"

static uint8_t *g_px;
static int g_w, g_h;
static double g_x0, g_z0, g_mpp;

static void put(int x, int y, uint32_t rgb) {
    uint8_t *p = &g_px[((size_t)y * g_w + x) * 3];
    p[0] = (uint8_t)(rgb & 255); p[1] = (uint8_t)((rgb >> 8) & 255); p[2] = (uint8_t)((rgb >> 16) & 255);
}

static uint32_t rgb(float r, float g, float b) {
    int R = (int)(clampf(r, 0, 1) * 255), G = (int)(clampf(g, 0, 1) * 255), B = (int)(clampf(b, 0, 1) * 255);
    return (uint32_t)(R << 16 | G << 8 | B);
}

/* Fill pixels whose centres pass `inside`, limited to a world rectangle. */
typedef int (*InsideFn)(const void *shape, double x, double z);

static void fill(const void *shape, InsideFn inside, double bx0, double bz0, double bx1, double bz1,
                 const double clip[4], uint32_t col) {
    bx0 = fmax(bx0, clip[0]); bz0 = fmax(bz0, clip[1]); bx1 = fmin(bx1, clip[2]); bz1 = fmin(bz1, clip[3]);
    int px0 = (int)floor((bx0 - g_x0) / g_mpp), px1 = (int)ceil((bx1 - g_x0) / g_mpp);
    int py0 = (int)floor((bz0 - g_z0) / g_mpp), py1 = (int)ceil((bz1 - g_z0) / g_mpp);
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > g_w) px1 = g_w;
    if (py1 > g_h) py1 = g_h;
    for (int y = py0; y < py1; ++y)
        for (int x = px0; x < px1; ++x) {
            double wx = g_x0 + (x + 0.5) * g_mpp, wz = g_z0 + (y + 0.5) * g_mpp;
            if (wx < clip[0] || wx >= clip[2] || wz < clip[1] || wz >= clip[3]) continue;
            if (inside(shape, wx, wz)) put(x, y, col);
        }
}

static int in_poly(const void *s, double x, double z) { return poly_contains((const Poly *)s, x, z, 0.0); }

typedef struct SecShape { const Building *b; const BFoot *f; } SecShape;

/* A building section's footprint, through the building's frame. */
static int in_section(const void *s, double x, double z) {
    const SecShape *sh = (const SecShape *)s;
    double dx = x - sh->b->ox, dz = z - sh->b->oz;
    return bfoot_contains(sh->f, (float)(sh->b->c * dx + sh->b->s * dz), (float)(-sh->b->s * dx + sh->b->c * dz));
}

static int in_building(const void *s, double x, double z) {
    const Building *b = (const Building *)s;
    double dx = x - b->ox, dz = z - b->oz;
    double lx = b->c * dx + b->s * dz, lz = -b->s * dx + b->c * dz;
    return lx >= b->foot.x0 && lx <= b->foot.x1 && lz >= b->foot.z0 && lz <= b->foot.z1;
}

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }
    uint32_t row = ((uint32_t)g_w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)g_h;
    uint8_t hdr[54] = { 'B', 'M' };
    int32_t w = g_w, h = -g_h;   /* top-down rows */
    uint32_t off = 54, dib = 40, planesBpp = 1 | (24 << 16);
    memcpy(hdr + 2, &size, 4); memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); memcpy(hdr + 26, &planesBpp, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t pad[4] = { 0 };
    for (int y = 0; y < g_h; ++y) {
        fwrite(&g_px[(size_t)y * g_w * 3], 1, (size_t)g_w * 3, f);
        fwrite(pad, 1, row - (uint32_t)g_w * 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "city_map.bmp";
    DVec2 c = plan_town_centre(0, 0);
    double size = 6000.0, mpp = 3.0;
    if (argc > 3) { c.x = atof(argv[2]); c.z = atof(argv[3]); }
    if (argc > 4) size = atof(argv[4]);
    if (argc > 5) mpp = atof(argv[5]);
    int withBuildings = argc > 6 ? atoi(argv[6]) : 1;
    g_mpp = mpp;
    g_w = g_h = (int)(size / mpp);
    g_x0 = c.x - size * 0.5; g_z0 = c.z - size * 0.5;
    g_px = (uint8_t *)malloc((size_t)g_w * g_h * 3);
    memset(g_px, 0, (size_t)g_w * g_h * 3);

    static const uint32_t zoneCol[ZONE_COUNT] = { 0x3a2f3f, 0x4a3a28, 0x2c3440, 0x343434, 0x1f3a22, 0x3a4a1c };
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bl = (Building *)malloc(sizeof(Building) * 256);
    BGraph *bg = (BGraph *)malloc(sizeof(BGraph));
    const double T = 400.0;
    int tiles = (int)ceil(size / T), blocks = 0, buildings = 0, roads = 0;
    for (int ty = 0; ty < tiles; ++ty)
        for (int tx = 0; tx < tiles; ++tx) {
            double clip[4] = { g_x0 + tx * T, g_z0 + ty * T, g_x0 + (tx + 1) * T, g_z0 + (ty + 1) * T };
            plan_area(clip[0], clip[1], clip[2], clip[3], pa);
            /* Asphalt over the city's ground, then carriageways by class. */
            if (!pa->bounded) {
                Poly q; memset(&q, 0, sizeof(q)); q.n = 4;
                q.v[0].x = clip[0]; q.v[0].z = clip[1]; q.v[1].x = clip[2]; q.v[1].z = clip[1];
                q.v[2].x = clip[2]; q.v[2].z = clip[3]; q.v[3].x = clip[0]; q.v[3].z = clip[3];
                fill(&q, in_poly, clip[0], clip[1], clip[2], clip[3], clip, 0x1c1c20);
            }
            for (int i = 0; i < pa->ntowns; ++i) {
                double x0, z0, x1, z1;
                poly_bounds(&pa->ground[i], &x0, &z0, &x1, &z1);
                fill(&pa->ground[i], in_poly, x0, z0, x1, z1, clip, 0x1c1c20);
            }
            for (int i = 0; i < pa->nroads; ++i) {
                const Road *r = &pa->roads[i];
                DVec2 d = { r->b.x - r->a.x, r->b.z - r->a.z };
                double len = sqrt(d.x * d.x + d.z * d.z);
                if (len < 1.0) continue;
                DVec2 u = { d.x / len, d.z / len }, n = { -u.z, u.x };
                Poly q; memset(&q, 0, sizeof(q)); q.n = 4;
                double h = r->at.half;
                q.v[0].x = r->a.x + n.x * h; q.v[0].z = r->a.z + n.z * h;
                q.v[1].x = r->b.x + n.x * h; q.v[1].z = r->b.z + n.z * h;
                q.v[2].x = r->b.x - n.x * h; q.v[2].z = r->b.z - n.z * h;
                q.v[3].x = r->a.x - n.x * h; q.v[3].z = r->a.z - n.z * h;
                double x0, z0, x1, z1;
                poly_bounds(&q, &x0, &z0, &x1, &z1);
                uint32_t col = r->at.cls == ROAD_BOULEVARD ? 0x6a4a20 : r->at.cls == ROAD_ARTERIAL ? 0x5a5428 : 0x3c3c40;
                fill(&q, in_poly, x0, z0, x1, z1, clip, col);
                roads++;
            }
            for (int i = 0; i < pa->nblocks; ++i) {
                const Block *bk = &pa->blocks[i];
                fill(&bk->curb, in_poly, bk->x0, bk->z0, bk->x1, bk->z1, clip, 0x707070);
                if (!bk->lot.n) continue;
                double x0, z0, x1, z1;
                poly_bounds(&bk->lot, &x0, &z0, &x1, &z1);
                fill(&bk->lot, in_poly, x0, z0, x1, z1, clip, zoneCol[bk->zone]);
                int n = withBuildings ? city_block_buildings(bk, bl, 256) : 0;
                for (int k = 0; k < n; ++k) {
                    const Building *b = &bl[k];
                    double r0 = b->foot.x1 - b->foot.x0 + b->foot.z1 - b->foot.z0;
                    bgen_build(b, bg, NULL);
                    /* sections from the lowest roof up, so the top view shows each one's highest part */
                    int order[BG_MAX_SECT];
                    for (int i = 0; i < bg->nsec; ++i) order[i] = i;
                    for (int i = 0; i < bg->nsec; ++i)
                        for (int j = i; j > 0 && bg->sec[order[j]].y1 < bg->sec[order[j - 1]].y1; --j) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
                    for (int i = 0; i < bg->nsec; ++i) {
                        const BSection *sec = &bg->sec[order[i]];
                        float t = clampf(sec->y1 / 300.0f, 0.0f, 1.0f);
                        uint32_t col = bg->archetype == ARCH_ARCOLOGY ? 0xff3060 : rgb(0.35f + 0.65f * t, 0.45f + 0.5f * t * t, 0.6f + 0.4f * (1 - t));
                        SecShape sh = { b, &sec->foot };
                        fill(&sh, in_section, b->ox - r0, b->oz - r0, b->ox + r0, b->oz + r0, clip, col);
                    }
                }
                blocks++;
                buildings += n;
            }
        }
    /* The city's town centres: regional ones larger. */
    for (int j = (int)floor(g_z0 / TOWN_SIZE) - 1; j <= (int)floor((g_z0 + size) / TOWN_SIZE) + 1; ++j)
        for (int i = (int)floor(g_x0 / TOWN_SIZE) - 1; i <= (int)floor((g_x0 + size) / TOWN_SIZE) + 1; ++i) {
            if (!plan_town_built(i, j)) continue;
            DVec2 t = plan_town_centre(i, j);
            int reg = plan_town_regional(i, j);
            int px = (int)((t.x - g_x0) / g_mpp), py = (int)((t.z - g_z0) / g_mpp), rr = reg ? 9 : 5;
            for (int y = py - rr; y <= py + rr; ++y)
                for (int x = px - rr; x <= px + rr; ++x)
                    if (x >= 0 && y >= 0 && x < g_w && y < g_h && (x - px) * (x - px) + (y - py) * (y - py) <= rr * rr)
                        put(x, y, reg ? 0xff2020 : 0xffa020);
        }
    write_bmp(out);
    printf("wrote %s: %dx%d px, %.0f m square, %d blocks, %d buildings (tiles overlap, so counts include repeats)\n",
           out, g_w, g_h, size, blocks, buildings);
    free(pa); free(bl); free(bg); free(g_px);
    return 0;
}
