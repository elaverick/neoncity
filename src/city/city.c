#include "city_internal.h"
#include "terrain.h"

/* The city generator's entry points (see city.h): a chunk's mesh, its office
 * lobbies and their plants. The parts they are made of are listed in
 * city_internal.h. */

/* ===========================================================================
 * Shared rules
 * ========================================================================= */

/* Conservative: density can rise by at most ~0.2 between the chunk centre and
 * any building centre in it, and buildings are clamped to this anyway. Towards
 * the city's edge the skyline steps down whatever the density (a regional
 * core near the edge would otherwise end the city in a cliff of towers):
 * through the fringe the cap falls from 800 m to 60 m, which still leaves the
 * lowest building (9 m) room for its roof. */
float city_height_cap(int cx, int cz) {
    double x = (cx + 0.5) * (double)CHUNK_SIZE, z = (cz + 0.5) * (double)CHUNK_SIZE;
    float d = plan_density(x, z) + 0.22f;
    float cap = d >= 0.92f ? 800.0f : 140.0f + 420.0f * minf(d, 1.0f);
    float s = smoothstepf(0.0f, 0.7f, plan_fringe(x, z));
    return minf(cap, 60.0f + 740.0f * (1.0f - s) * (1.0f - s));
}

float city_ground_y(double x, double z) { return (float)terrain_urban_height(x, z); }

/* The relief of the ground at (x, z): its settlement's (m). */
static double ground_relief(double x, double z) { return plan_settlement(plan_settlement_near(x, z)).relief_m; }

float city_drape_cell(double x, double z) {
    const float widest = CHUNK_SIZE / 2.0f;
    double r = ground_relief(x, z), most = PLAN_RELIEF_MEGA_M * widest * widest * 1.0001;   /* rounding */
    float c = widest;
    while (c > CHUNK_SIZE / 16.0f && r * c * c > most) c *= 0.5f;
    return c;
}

float city_footing_sink(double x, double z) {
    return 0.05f * maxf(1.0f, (float)(ground_relief(x, z) / PLAN_RELIEF_MEGA_M));
}

void city_ground_range(int cx, int cz, float *lo, float *hi) {
    const double P = CHUNK_SIZE, o = CITY_OVERHANG;
    double l, h;
    terrain_urban_height_range(cx * P - o, cz * P - o, (cx + 1) * P + o, (cz + 1) * P + o, &l, &h);
    *lo = (float)floor(l);
    *hi = (float)ceil(h);
    /* out in the land, a road's bridge stands at its road's height */
    float dl, dh;
    if (city_road_deck_range(cx * P, cz * P, (cx + 1) * P, (cz + 1) * P, &dl, &dh)) {
        *lo = fminf(*lo, floorf(dl));
        *hi = fmaxf(*hi, ceilf(dh));
    }
}

/* The highest ground under the plot, sampled finely enough for the ground's
 * gentle curve, so nothing of the ground floor is buried: where the ground
 * falls away the walls reach down to it (Mesh.footing). Where the ground
 * varies, a little more for the draped lot under the building, which can
 * stand up to about a centimetre proud of the ground (city_drape_cell). */
float plot_base(const Building *b) {
    const float step = city_drape_cell(b->ox, b->oz) * 0.5f;
    float w = b->foot.x1 - b->foot.x0, d = b->foot.z1 - b->foot.z0;
    int nu = 2 + (int)(w / step), nv = 2 + (int)(d / step);
    float lo = 1e30f, hi = -1e30f;
    for (int v = 0; v < nv; ++v)
        for (int u = 0; u < nu; ++u) {
            DVec2 p = to_world(b, b->foot.x0 + w * u / (nu - 1), b->foot.z0 + d * v / (nv - 1));
            float y = city_ground_y(p.x, p.z);
            lo = minf(lo, y); hi = maxf(hi, y);
        }
    return hi > lo ? hi + 0.02f : hi;
}

void city_ground_slope(double x, double z, float *gx, float *gz) {
    const double h = 1.0;   /* the ground's shortest wavelengths are kilometres long */
    *gx = (float)((terrain_urban_height(x + h, z) - terrain_urban_height(x - h, z)) / (2.0 * h));
    *gz = (float)((terrain_urban_height(x, z + h) - terrain_urban_height(x, z - h)) / (2.0 * h));
}

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
    build_channels(m, &g);
    build_blocks(m, &g);
    m->drape = 0;
    build_road_bridges(m, &g);
    build_vents(m, &g);
    for (int i = 0; i < g.nl; ++i)
        if (chunk_gen_owns(&g, g.lamp[i].x, g.lamp[i].z)) build_lamp(m, &g, &g.lamp[i], (uint32_t)i);
    for (int i = 0; i < g.nb; ++i) {
        const Building *b = &g.bld[i];
        if (!chunk_gen_owns(&g, b->ox, b->oz)) continue;
        enter_building(m, &g, b);
        const BGraph *bg = chunk_gen_graph(&g, i);
        Interiors in;
        interiors_plan(b, bg, &in);
        m->footing = 1;
        emit_building(m, b, bg, &in);
        m->footing = 0;
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
