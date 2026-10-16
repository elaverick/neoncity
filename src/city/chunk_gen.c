#include "city_internal.h"

void chunk_gen_init(ChunkGen *g, int cx, int cz, double planPad, double buildPad, double lampPad) {
    const double P = CHUNK_SIZE;
    memset(g, 0, sizeof(*g));
    g->cx = cx; g->cz = cz;
    g->ox = cx * P; g->oz = cz * P;
    g->pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!g->pa) fatal("out of memory generating a chunk");
    plan_area(g->ox - planPad, g->oz - planPad, g->ox + P + planPad, g->oz + P + planPad, g->pa);

    Building tmp[256];
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->x1 < g->ox - buildPad || bk->x0 > g->ox + P + buildPad ||
            bk->z1 < g->oz - buildPad || bk->z0 > g->oz + P + buildPad) continue;
        int n = city_block_buildings(bk, tmp, 256);
        for (int i = 0; i < n; ++i) {
            if (g->nb == g->capb) {
                g->capb = g->capb ? g->capb * 2 : 256;
                g->bld = (Building *)realloc(g->bld, sizeof(Building) * g->capb);
                g->bblk = (int *)realloc(g->bblk, sizeof(int) * g->capb);
                if (!g->bld || !g->bblk) fatal("out of memory generating a chunk");
            }
            g->bld[g->nb] = tmp[i];
            g->bblk[g->nb++] = bi;
        }
    }
    Lamp lt[128];
    for (int ri = 0; ri < g->pa->nroads; ++ri) {
        int n = city_road_lamps(g->pa, ri, g->ox - lampPad, g->oz - lampPad, g->ox + P + lampPad, g->oz + P + lampPad, lt, 128);
        for (int i = 0; i < n; ++i) {
            if (g->nl == g->capl) {
                g->capl = g->capl ? g->capl * 2 : 128;
                g->lamp = (Lamp *)realloc(g->lamp, sizeof(Lamp) * g->capl);
                if (!g->lamp) fatal("out of memory generating a chunk");
            }
            g->lamp[g->nl++] = lt[i];
        }
    }
}

const BGraph *chunk_gen_graph(ChunkGen *g, int i) {
    if (!g->graph) {
        g->graph = (BGraph **)calloc((size_t)(g->nb ? g->nb : 1), sizeof(BGraph *));
        if (!g->graph) fatal("out of memory generating a chunk");
    }
    if (!g->graph[i]) {
        g->graph[i] = (BGraph *)malloc(sizeof(BGraph));
        if (!g->graph[i]) fatal("out of memory generating a chunk");
        bgen_build(&g->bld[i], g->graph[i], NULL);
    }
    return g->graph[i];
}

void chunk_gen_free(ChunkGen *g) {
    if (g->graph) for (int i = 0; i < g->nb; ++i) free(g->graph[i]);
    free(g->graph);
    free(g->pa); free(g->bld); free(g->bblk); free(g->lamp);
    memset(g, 0, sizeof(*g));
}

int chunk_gen_owns(const ChunkGen *g, double x, double z) {
    return (int)floor(x / CHUNK_SIZE) == g->cx && (int)floor(z / CHUNK_SIZE) == g->cz;
}

int chunk_gen_near(const ChunkGen *g, double x0, double z0, double x1, double z1, double pad) {
    return x1 >= g->ox - pad && x0 <= g->ox + CHUNK_SIZE + pad && z1 >= g->oz - pad && z0 <= g->oz + CHUNK_SIZE + pad;
}
