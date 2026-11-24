#include "city_internal.h"
#include "home.h"

/* Distant proxies (see city.h): every building centred in a tile, as a box
 * round each section of its graph; and every house row whose middle lies in
 * it, as a prism round its main volume (HM3b). */

/* IEEE half from float, rounding to nearest even; tiny values flush to zero
 * (nothing a proxy holds needs them). Unpacked by the shader's unpackHalf2x16. */
static uint32_t f16(float f) {
    union { float f; uint32_t u; } v;
    v.f = f;
    uint32_t s = (v.u >> 16) & 0x8000u, m = v.u & 0x7FFFFFu;
    int e = (int)((v.u >> 23) & 0xFFu) - 127 + 15;
    if (e <= 0) return s;
    if (e >= 31) return s | 0x7C00u;
    uint32_t h = s | (uint32_t)e << 10 | m >> 13, r = m & 0x1FFFu;
    if (r > 0x1000u || (r == 0x1000u && (h & 1u))) h++;
    return h;
}

static uint32_t pack2(float a, float b) { return f16(a) | f16(b) << 16; }

typedef struct Pending { int chunk, order; float reach, size; ProxyBox b; } Pending;

/* IEEE half to float (normal values and zero: what f16 makes of a box). */
static float h16(uint32_t h) {
    uint32_t e = (h >> 10) & 0x1Fu;
    if (e == 0) return 0.0f;
    union { float f; uint32_t u; } v;
    v.u = (h & 0x8000u) << 16 | (e - 15 + 127) << 23 | (h & 0x3FFu) << 13;
    return v.f;
}

float proxy_box_size(const ProxyBox *b) {
    return maxf(b->y1 - b->y0, 2.0f * maxf(h16(b->half & 0xFFFFu), h16(b->half >> 16)));
}

/* By chunk, then largest first, then in the order generated. */
static int cmp_pending(const void *a, const void *b) {
    const Pending *p = (const Pending *)a, *q = (const Pending *)b;
    if (p->chunk != q->chunk) return p->chunk - q->chunk;
    if (p->size != q->size) return p->size > q->size ? -1 : 1;
    return p->order - q->order;
}

/* The boxes of building b, owned by the tile's chunk k. */
static int building_boxes(const Building *b, const BGraph *g, int k, double ox, double oz, Pending *out, int max) {
    int n = 0;
    for (int i = 0; i < g->nsec && n < max; ++i) {
        const BSection *s = &g->sec[i];
        const BLoop *l = &s->foot.outer;
        if (l->n < 3) continue;
        float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
        for (int v = 0; v < l->n; ++v) {
            x0 = minf(x0, l->x[v]); x1 = maxf(x1, l->x[v]);
            z0 = minf(z0, l->z[v]); z1 = maxf(z1, l->z[v]);
        }
        int top = 1;
        for (int j = 0; j < g->nsec && top; ++j)
            for (int q = 0; q < g->sec[j].nbelow; ++q) top &= g->sec[j].below[q] != i;
        int grounded = bsec_grounded(s);
        DVec2 c = to_world(b, 0.5 * (x0 + x1), 0.5 * (z0 + z1));
        Pending *p = &out[n++];
        p->chunk = k;
        p->reach = 0.5f * sqrtf((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
        p->b.x = (float)(c.x - ox); p->b.z = (float)(c.z - oz);
        /* standing on the ground, it reaches into it as far as the walls' footing might */
        p->b.y0 = b->base + (grounded ? -1.0f : s->y0);
        p->b.y1 = b->base + s->y1;
        p->b.half = pack2(0.5f * (x1 - x0), 0.5f * (z1 - z0));
        p->b.axis = pack2(b->c, b->s);
        p->b.seed = b->seed;
        uint32_t floor0 = (uint32_t)clampf(s->y0 * 2.0f + 0.5f, 0.0f, 2047.0f);
        p->b.info = (uint32_t)k << PX_CHUNK_SHIFT | (uint32_t)(s->win & 3u) << PX_WIN_SHIFT |
                    (uint32_t)(s->palette & 7u) << PX_PALETTE_SHIFT | (s->bands ? PX_BANDS : 0u) |
                    (g->fine ? PX_FINE : 0u) | (s->ledTop ? PX_LEDTOP : 0u) | (top ? PX_TOP : 0u) |
                    (uint32_t)i << PX_SECTION_SHIFT | floor0 << PX_FLOOR_SHIFT |
                    (g->archetype == ARCH_ARCOLOGY ? PX_ARCOLOGY : 0u) | (bsec_facade(s) ? 0u : PX_PLAIN);
    }
    return n;
}

#define TILE_PRISMS 4096   /* house rows' prisms a block may give a tile */

/* House row prism q (city.h: PX_HOUSE), owned by the tile's chunk k. */
static void house_prism(const HomePrism *q, int k, double ox, double oz, Pending *p) {
    const float top = q->eaves + q->rise;
    p->chunk = k;
    p->reach = sqrtf(q->hx * q->hx + q->hz * q->hz);
    p->b.x = (float)(q->x - ox); p->b.z = (float)(q->z - oz);
    p->b.y0 = q->y0;
    p->b.y1 = q->eaves;
    p->b.half = pack2(q->hx, q->hz);
    p->b.axis = pack2(q->ax, q->az);
    p->b.seed = pack2(q->rise, q->inset);
    p->b.info = (uint32_t)k << PX_CHUNK_SHIFT | PX_HOUSE | (uint32_t)(q->wall & 7u) << PXH_WALL_SHIFT |
                (uint32_t)(q->wallCol & 31u) << PXH_WALLCOL_SHIFT | (uint32_t)(q->roof & 15u) << PXH_ROOF_SHIFT |
                (uint32_t)(q->roofCol & 7u) << PXH_ROOFCOL_SHIFT | (uint32_t)(q->storeys & 3u) << PXH_STOREYS_SHIFT |
                (uint32_t)(q->wall >> 3 & 1u) << PXH_WALL_HI_SHIFT;
    p->size = maxf(top - q->y0, 2.0f * maxf(q->hx, q->hz));
}

/* Sorts pend (n of them) by chunk, largest first, counts each chunk's run (first, counted from base) and its
 * levels of detail, and copies them to out. */
static void lay_runs(Pending *pend, int n, int base, ProxyBox *out, int *first, uint16_t (*lod)[PROXY_LOD_BINS]) {
    qsort(pend, (size_t)n, sizeof(Pending), cmp_pending);
    for (int k = 0; k <= PROXY_TILE_CELLS; ++k) first[k] = 0;
    memset(lod, 0, sizeof(uint16_t) * PROXY_TILE_CELLS * PROXY_LOD_BINS);
    for (int i = 0; i < n; ++i) {
        int k = pend[i].chunk;
        first[k + 1]++;
        for (int b = 0; b < PROXY_LOD_BINS && pend[i].size >= PROXY_LOD_SIZE(b); ++b) lod[k][b]++;
    }
    first[0] = base;
    for (int k = 0; k < PROXY_TILE_CELLS; ++k) first[k + 1] += first[k];
    for (int i = 0; i < n; ++i) out[i] = pend[i].b;
}

void city_proxy_tile(int tx, int tz, ProxyTile *t) {
    const double T = PROXY_TILE, ox = tx * T, oz = tz * T;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    Building *bl = (Building *)malloc(sizeof(Building) * 256);
    BGraph *g = (BGraph *)malloc(sizeof(BGraph));
    int npend = 0, cappend = 1024;
    Pending *pend = (Pending *)malloc(sizeof(Pending) * cappend);
    int nhp = 0, caphp = 1024;
    Pending *hpend = (Pending *)malloc(sizeof(Pending) * caphp);
    HomePrism *hp = (HomePrism *)malloc(sizeof(HomePrism) * TILE_PRISMS);
    HGraph *hg = (HGraph *)malloc(sizeof(HGraph));
    if (!pa || !bl || !g || !pend || !hpend || !hp || !hg) fatal("out of memory generating proxies");
    /* Every building centred in the tile stands on a block that reaches into it. */
    plan_area(ox, oz, ox + T, oz + T, pa);
    if (pa->nblocks >= PLAN_MAX_BLOCKS) fatal("proxies: a tile may have more blocks than a plan area holds");
    t->tx = tx; t->tz = tz;
    t->nbuild = 0;
    t->nrow = 0;
    for (int bi = 0; bi < pa->nblocks; ++bi) {
        /* the house rows whose middles lie in the tile: a prism each (HM3b) */
        const int nh = city_home_prisms(&pa->blocks[bi], ox, oz, ox + T, oz + T, hg, hp, TILE_PRISMS);
        for (int i = 0; i < nh; ++i) {
            int cx = (int)floor(hp[i].mx / CHUNK_SIZE) - tx * PROXY_TILE_CHUNKS, cz = (int)floor(hp[i].mz / CHUNK_SIZE) - tz * PROXY_TILE_CHUNKS;
            if (cx < 0 || cx >= PROXY_TILE_CHUNKS || cz < 0 || cz >= PROXY_TILE_CHUNKS) continue;
            if (nhp == caphp) {
                caphp *= 2;
                hpend = (Pending *)realloc(hpend, sizeof(Pending) * caphp);
                if (!hpend) fatal("out of memory generating proxies");
            }
            house_prism(&hp[i], cx + PROXY_TILE_CHUNKS * cz, ox, oz, &hpend[nhp++]);
            t->nrow++;
        }
        int n = city_block_buildings(&pa->blocks[bi], bl, 256);
        for (int i = 0; i < n; ++i) {
            const Building *b = &bl[i];
            int cx = (int)floor(b->ox / CHUNK_SIZE) - tx * PROXY_TILE_CHUNKS, cz = (int)floor(b->oz / CHUNK_SIZE) - tz * PROXY_TILE_CHUNKS;
            if (cx < 0 || cx >= PROXY_TILE_CHUNKS || cz < 0 || cz >= PROXY_TILE_CHUNKS) continue;
            bgen_build(b, g, NULL);
            if (npend + BG_MAX_SECT > cappend) {
                cappend *= 2;
                pend = (Pending *)realloc(pend, sizeof(Pending) * cappend);
                if (!pend) fatal("out of memory generating proxies");
            }
            npend += building_boxes(b, g, cx + PROXY_TILE_CHUNKS * cz, ox, oz, pend + npend, BG_MAX_SECT);
            t->nbuild++;
        }
    }

    /* Grouped by chunk, largest first within each (see PROXY_LOD_BINS): the boxes, then the prisms. */
    if (npend + nhp > t->cap) {
        t->cap = npend + nhp;
        t->box = (ProxyBox *)realloc(t->box, sizeof(ProxyBox) * t->cap);
        if (!t->box) fatal("out of memory generating proxies");
    }
    t->mn[0] = t->mn[1] = t->mn[2] = 1e30f;
    t->mx[0] = t->mx[1] = t->mx[2] = -1e30f;
    for (int i = 0; i < npend + nhp; ++i) {
        const Pending *q = i < npend ? &pend[i] : &hpend[i - npend];
        const ProxyBox *b = &q->b;
        /* a box's reach on the ground plane is at most its half diagonal; a prism's top is its ridge */
        float r = q->reach, top = (b->info & PX_HOUSE) ? b->y1 + h16(b->seed & 0xFFFFu) : b->y1;
        t->mn[0] = minf(t->mn[0], b->x - r); t->mx[0] = maxf(t->mx[0], b->x + r);
        t->mn[2] = minf(t->mn[2], b->z - r); t->mx[2] = maxf(t->mx[2], b->z + r);
        t->mn[1] = minf(t->mn[1], b->y0); t->mx[1] = maxf(t->mx[1], top);
    }
    for (int i = 0; i < npend; ++i) { pend[i].order = i; pend[i].size = proxy_box_size(&pend[i].b); }
    for (int i = 0; i < nhp; ++i) hpend[i].order = i;
    lay_runs(pend, npend, 0, t->box, t->first, t->lod);
    lay_runs(hpend, nhp, npend, t->box + npend, t->hfirst, t->hlod);
    t->n = npend + nhp;
    free(hg); free(hp); free(hpend);
    free(pend); free(g); free(bl); free(pa);
}

/* Buildings stand only on built towns' ground, where the frame's own
 * settlement's edge measure (plan_built_edge) is at most CITY_BUILT_EDGE, and
 * the edge measure changes by at most CITY_EDGE_RATE a metre, so from the
 * tile's centre it bounds the whole tile. */
int city_proxy_tile_empty(int tx, int tz) {
    double h = 0.5 * PROXY_TILE, r = h * 1.4142135623730951;
    return plan_built_edge(tx * PROXY_TILE + h, tz * PROXY_TILE + h) - CITY_EDGE_RATE * r > CITY_BUILT_EDGE;
}

void proxy_tile_free(ProxyTile *t) { free(t->box); memset(t, 0, sizeof(*t)); }
