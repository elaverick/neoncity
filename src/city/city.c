#include "city_internal.h"
#include "terrain.h"

/* The city generator's entry points (see city.h): a chunk's mesh, its office
 * lobbies and their plants. The parts they are made of are listed in
 * city_internal.h. */

/* ===========================================================================
 * Shared rules
 * ========================================================================= */

/* Conservative: density can rise by at most ~0.2 between the chunk centre and
 * any building centre in it, and buildings are clamped to this anyway. */
float city_height_cap(int cx, int cz) {
    float d = plan_density((cx + 0.5) * (double)CHUNK_SIZE, (cz + 0.5) * (double)CHUNK_SIZE) + 0.22f;
    if (d >= 0.92f) return 800.0f;
    return 140.0f + 420.0f * minf(d, 1.0f);
}

float city_ground_y(double x, double z) { return (float)terrain_urban_height(x, z); }

float building_base(const Building *b) { return city_ground_y(b->ox, b->oz); }

void enter_building(Mesh *m, const ChunkGen *g, const Building *b) {
    m->xf = 1;
    m->xc = b->c; m->xs = b->s;
    m->xx = (float)(b->ox - g->ox); m->xz = (float)(b->oz - g->oz);
    m->xy = building_base(b);
}

/* Facade rhythm shared with shaders/common.glsl */
uint32_t style_hash(uint32_t seed) { return hash_u32(seed ^ 0x5f3759dfu); }
float city_floor_height(uint32_t seed)  { return 3.3f + 0.9f * hash_float(hash_combine(style_hash(seed), 1u)); }
float city_ground_height(uint32_t seed) { return 4.6f + 1.4f * hash_float(hash_combine(style_hash(seed), 2u)); }

/* An office lobby's accent light, the neon colour of its mullion lines and
 * canopy strip (lobbyAccent in shaders/city.frag): mostly cyan. */
uint8_t lobby_accent(uint32_t seed) {
    float a = hash_float(hash_combine(style_hash(seed), 61u));
    return (uint8_t)(a < 0.55f ? 1 : a < 0.7f ? 6 : a < 0.85f ? 4 : 3);
}

/* Storefronts are 6 m bays along each ground-floor face; shaders/city.frag
 * uses the same hash so lit shops, their signs and their light spill agree.
 * Faces are numbered within their section; the bay that wraps a convex
 * corner is keyed as the next face's first, so a corner shop is one shop
 * seen from both sides. */
uint32_t city_shop_hash(uint32_t seed, int section, int face, int seg, float faceWidth, int nextFace, int endShare) {
    int last = (int)floorf((faceWidth - 0.01f) / 6.0f);
    if (endShare && seg >= last && last > 0) { face = nextFace; seg = 0; }
    return hash3i(WORLD_SEED ^ 0x5B0Au, (int32_t)seed, section * 64 + face, seg);
}
int city_shop_lit(uint32_t h, int level) { return hash_float(h) < (float)level * 0.125f; }

/* ===========================================================================
 * Chunk mesh
 * ========================================================================= */

void city_build_mesh(int cx, int cz, Mesh *m) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, 130.0, 90.0, 105.0);
    mesh_begin(m, cx, cz);

    m->drape = 1;   /* the streets' surfaces lie on the ground, vertex by vertex */
    build_roads(m, &g);
    build_blocks(m, &g);
    m->drape = 0;
    for (int i = 0; i < g.nl; ++i)
        if (chunk_gen_owns(&g, g.lamp[i].x, g.lamp[i].z)) build_lamp(m, &g, &g.lamp[i], (uint32_t)i);
    for (int i = 0; i < g.nb; ++i) {
        const Building *b = &g.bld[i];
        if (!chunk_gen_owns(&g, b->ox, b->oz)) continue;
        enter_building(m, &g, b);
        const BGraph *bg = chunk_gen_graph(&g, i);
        Interiors in;
        interiors_plan(b, bg, &in);
        emit_building(m, b, bg, &in);
        interiors_free(&in);
        m->xf = 0;
    }
    build_cables(m, &g);
    build_lights(m, &g);
    chunk_gen_free(&g);
}

/* The near-only layer (src/render/chunks.c): the lobbies behind the ground-floor
 * glass of the buildings centred in this chunk, in the same chunk-local space. */
void city_build_interiors(int cx, int cz, Mesh *m) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, 25.0, 0.0, 0.0);
    mesh_begin(m, cx, cz);
    for (int i = 0; i < g.nb; ++i) {
        const Building *b = &g.bld[i];
        if (!chunk_gen_owns(&g, b->ox, b->oz)) continue;
        const BGraph *bg = chunk_gen_graph(&g, i);
        Interiors in;
        interiors_plan(b, bg, &in);
        enter_building(m, &g, b);
        for (int k = 0; k < in.n; ++k) emit_interior(m, bg, in.lay[k]);
        if (in.arc) emit_arcology(m, bg, in.arc);
        m->xf = 0;
        interiors_free(&in);
    }
    chunk_gen_free(&g);
}

void city_build_plants(int cx, int cz, Mesh *m) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, 25.0, 0.0, 0.0);
    mesh_begin(m, cx, cz);
    Planting pl = { NULL, 0, 0 };
    ILayout *L = NULL;
    for (int i = 0; i < g.nb; ++i) {
        const Building *b = &g.bld[i];
        if (!chunk_gen_owns(&g, b->ox, b->oz)) continue;
        const BGraph *bg = chunk_gen_graph(&g, i);
        if (bg->archetype == ARCH_ARCOLOGY) continue;   /* arcologies have no planters */
        int secs[2], ns = interior_sections(bg, secs, 2);
        if (!ns) continue;
        if (!L && !(L = (ILayout *)malloc(sizeof(ILayout)))) fatal("out of memory planning an interior");
        enter_building(m, &g, b);
        for (int k = 0; k < ns; ++k)
            if (interior_plan(b, bg, secs[k], 0, L, NULL)) collect_plants(m, bg, L, &pl);
        m->xf = 0;
    }
    free(L);
    chunk_gen_free(&g);
    grow_plants(m, &pl);
    free(pl.s);
}

/* ===========================================================================
 * Spawn point
 * ========================================================================= */

void city_spawn(double *x, double *z, float *yaw) {
    DVec2 c = plan_town_centre(0, 0);
    *x = c.x; *z = c.z; *yaw = 0.0f;
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    if (!pa) fatal("out of memory finding a spawn point");
    plan_area(c.x - 320.0, c.z - 320.0, c.x + 320.0, c.z + 320.0, pa);
    /* The nearest decent street to the town centre, on its sidewalk. */
    double best = 1e300;
    for (int ri = 0; ri < pa->nroads; ++ri) {
        const Road *r = &pa->roads[ri];
        DVec2 d = dsub(r->b, r->a);
        double len = dlen(d);
        if (len - r->trimA - r->trimB < 50.0) continue;
        DVec2 dir = dscale(d, 1.0 / len), nrm = dv(-dir.z, dir.x);
        double s = (r->trimA + len - r->trimB) * 0.5 + LAMP_SPACING * 0.5;
        for (int side = -1; side <= 1; side += 2) {
            DVec2 q = dadd(dadd(r->a, dscale(dir, s)), dscale(nrm, side * (r->at.half + r->at.walk * 0.55)));
            double dist = dlen(dsub(q, c));
            if (dist >= best || q.x < pa->x0 + 40 || q.x > pa->x1 - 40 || q.z < pa->z0 + 40 || q.z > pa->z1 - 40) continue;
            int onWalk = 0;
            for (int k = 0; k < pa->nblocks && !onWalk; ++k) {
                const Block *bk = &pa->blocks[k];
                onWalk = poly_contains(&bk->curb, q.x, q.z, -0.5) && !(bk->lot.n && poly_contains(&bk->lot, q.x, q.z, 0.6));
            }
            if (!onWalk) continue;
            best = dist;
            *x = q.x; *z = q.z;
            *yaw = (float)atan2(dir.x, -dir.z);
        }
    }
    free(pa);
}
