#pragma once
#include "city.h"
#include "building.h"
#include "geom2d.h"

/* Shared between the files that generate a chunk (src/city/). Not part of
 * the public interface in city.h. */

/* ---------------------------------------------------------------------------
 * Everything near one chunk (src/city/chunk_gen.c): the plan round it, the
 * buildings of the blocks near it and the lamps along its roads.
 * ------------------------------------------------------------------------- */

typedef struct ChunkGen {
    int       cx, cz;
    double    ox, oz;       /* chunk origin, world */
    PlanArea *pa;
    Building *bld;
    int      *bblk;         /* block index of each building */
    int       nb, capb;
    Lamp     *lamp;
    int       nl, capl;
    BGraph  **graph;        /* building graphs, generated on demand */
} ChunkGen;

/* planPad: plan area margin. Buildings are generated for blocks within
 * buildPad of the chunk and lamps within lampPad; lampPad must stay 20 m
 * inside planPad so every road and kerb a lamp is checked against is known. */
void          chunk_gen_init(ChunkGen *g, int cx, int cz, double planPad, double buildPad, double lampPad);
void          chunk_gen_free(ChunkGen *g);
/* Building i's graph, generated when first asked for. */
const BGraph *chunk_gen_graph(ChunkGen *g, int i);
/* Whether (x, z) lies in this chunk, which then owns what stands there. */
int           chunk_gen_owns(const ChunkGen *g, double x, double z);
/* Whether a world rectangle comes within pad of the chunk. */
int           chunk_gen_near(const ChunkGen *g, double x0, double z0, double x1, double z1, double pad);
