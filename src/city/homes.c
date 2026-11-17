#include "city_internal.h"
#include "home_shell.h"

/* ===========================================================================
 * The houses (residential roadmap HM3a, ~/.claude/plans/residential-buildings.md)
 *
 * The rows of the suburbs' plots (plots.c) near a chunk, their house graphs
 * (src/gen/building_residential.c), the ground each unit stands on, and their
 * shells (src/gen/home_shell.c) set down in the chunk that holds the row's
 * middle: the far shell in the base layer (city_build_mesh), the near one in
 * a layer of its own (city_build_homes, within HOME_NEAR_DISTANCE), which the
 * far shell gives way to while it is drawn (CHUNK_HOMES_NEAR_BIT).
 * ========================================================================= */

#define HOME_REACH   2.0    /* m: rows whose plots come this near the chunk are gathered (grass, collision) */
#define STEP_JOIN    0.25f  /* m: units whose ground differs by less stand at one height, the highest */

/* The row's middle and bounds (world). */
static void row_extent(const HomePlot *P, int n, HomeRow *r) {
    const HomePlot *a = &P[0], *b = &P[n - 1];
    const double um = 0.5 * (a->u0 + b->u1), vm = 0.5 * a->depth;
    r->mx = a->ox + um * a->tx + vm * a->nx;
    r->mz = a->oz + um * a->tz + vm * a->nz;
    r->x0 = r->z0 = 1e300; r->x1 = r->z1 = -1e300;
    for (int c = 0; c < 4; ++c) {
        const double u = c & 1 ? b->u1 : a->u0, v = c & 2 ? a->depth : 0.0;
        const double x = a->ox + u * a->tx + v * a->nx, z = a->oz + u * a->tz + v * a->nz;
        r->x0 = fmin(r->x0, x); r->x1 = fmax(r->x1, x); r->z0 = fmin(r->z0, z); r->z1 = fmax(r->z1, z);
    }
}

void chunk_gen_homes(ChunkGen *g) {
    if (g->homesDone) return;
    g->homesDone = 1;
    HomePlot *P = (HomePlot *)malloc(sizeof(HomePlot) * HOME_MAX_PLOTS);
    if (!P) fatal("out of memory gathering houses");
    for (int bi = 0; bi < g->pa->nblocks; ++bi) {
        const Block *bk = &g->pa->blocks[bi];
        if (bk->zone != ZONE_SUBURB || !chunk_gen_near(g, bk->x0, bk->z0, bk->x1, bk->z1, HOME_REACH)) continue;
        const int n = city_block_plots(bk, P, HOME_MAX_PLOTS);
        for (int i = 0; i < n;) {
            int j = i + 1;
            while (j < n && P[j].row == P[i].row) j++;
            HomeRow r;
            memset(&r, 0, sizeof(r));
            row_extent(&P[i], j - i, &r);
            if (chunk_gen_near(g, r.x0, r.z0, r.x1, r.z1, HOME_REACH)) {
                if (g->nhplot + (j - i) > g->caphplot) {
                    g->caphplot = (g->caphplot ? g->caphplot * 2 : 256) + (j - i);
                    g->hplot = (HomePlot *)realloc(g->hplot, sizeof(HomePlot) * (size_t)g->caphplot);
                }
                if (g->nhrow == g->caphrow) {
                    g->caphrow = g->caphrow ? g->caphrow * 2 : 64;
                    g->hrow = (HomeRow *)realloc(g->hrow, sizeof(HomeRow) * (size_t)g->caphrow);
                }
                if (!g->hplot || !g->hrow) fatal("out of memory gathering houses");
                r.first = g->nhplot;
                r.n = j - i;
                memcpy(&g->hplot[g->nhplot], &P[i], sizeof(HomePlot) * (size_t)(j - i));
                g->nhplot += j - i;
                g->hrow[g->nhrow++] = r;
            }
            i = j;
        }
    }
    free(P);
}

/* Each unit's ground: the highest under what of the house stands on its plot, so nothing is buried (the walls
 * reach down to the ground where it falls away: Mesh.footing). Neighbours within STEP_JOIN of each other stand
 * at one height, the row stepping only where the ground does. */
static void row_bases(const HGraph *G, HomeRow *r) {
    float raw[HG_MAX_UNITS];
    for (int k = 0; k < G->nunit; ++k) {
        const HUnit *u = &G->unit[k];
        float z0 = 1e9f, z1 = -1e9f;
        for (int i = 0; i < G->nvol; ++i) {
            const HVol *v = &G->vol[i];
            if (v->unit != k && v->unit != HG_ROW_WIDE) continue;
            if (v->x1 <= u->x0 + 0.01f || v->x0 >= u->x1 - 0.01f) continue;
            z0 = fminf(z0, v->z0); z1 = fmaxf(z1, v->z1);
        }
        if (z0 > z1) { z0 = u->front; z1 = u->front + 8.0f; }
        float hi = -1e9f;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b) {
                double x, z;
                home_to_world(G, u->x0 + (u->x1 - u->x0) * (0.05f + 0.45f * a), z0 + (z1 - z0) * 0.5f * b, &x, &z);
                hi = fmaxf(hi, city_ground_y(x, z));
            }
        raw[k] = hi + 0.02f;
    }
    float base[HG_MAX_UNITS];
    int start = 0;
    while (start < G->nunit) {
        float lo = raw[start], top = raw[start];
        int end = start + 1;
        while (end < G->nunit && fmaxf(top, raw[end]) - fminf(lo, raw[end]) <= STEP_JOIN) {
            lo = fminf(lo, raw[end]); top = fmaxf(top, raw[end]);
            end++;
        }
        for (int k = start; k < end; ++k) base[k] = top;
        start = end;
    }
    r->base = 1e30f;
    for (int k = 0; k < G->nunit; ++k) r->base = fminf(r->base, base[k]);
    for (int k = 0; k < HG_MAX_UNITS; ++k) r->dy[k] = k < G->nunit ? base[k] - r->base : 0.0f;
}

const HGraph *chunk_gen_home(ChunkGen *g, int ri) {
    HomeRow *r = &g->hrow[ri];
    if (!r->graph) {
        r->graph = (HGraph *)malloc(sizeof(HGraph));
        if (!r->graph) fatal("out of memory building a house");
        city_home_row(&g->hplot[r->first], r->n, r->graph);
        if (!r->graph->empty) row_bases(r->graph, r);
    }
    return r->graph;
}

int chunk_gen_owns_home(const ChunkGen *g, int ri) { return chunk_gen_owns(g, g->hrow[ri].mx, g->hrow[ri].mz); }

/* ---------------------------------------------------------------------------
 * The far proxies' prisms (HM3b)
 * ------------------------------------------------------------------------- */

int city_home_prisms(const Block *bk, double x0, double z0, double x1, double z1, HGraph *G, HomePrism *out, int max) {
    if (bk->zone != ZONE_SUBURB || city_homes_mode() == HOMES_OFF) return 0;
    HomePlot *P = (HomePlot *)malloc(sizeof(HomePlot) * HOME_MAX_PLOTS);
    if (!P) fatal("out of memory gathering houses");
    const int np = city_block_plots(bk, P, HOME_MAX_PLOTS);
    int n = 0;
    for (int i = 0; i < np && n < max;) {
        int j = i + 1;
        while (j < np && P[j].row == P[i].row) j++;
        HomeRow r;
        memset(&r, 0, sizeof(r));
        row_extent(&P[i], j - i, &r);
        if (r.mx >= x0 && r.mx < x1 && r.mz >= z0 && r.mz < z1) {
            city_home_row(&P[i], j - i, G);
            if (!G->empty) {
                row_bases(G, &r);
                float lift = 0.0f;   /* the units' mean height over the lowest */
                for (int k = 0; k < G->nunit; ++k) lift += r.dy[k] / (float)G->nunit;
                HFinish f;
                home_finish(G, 0, &f);
                for (int vi = 0; vi < G->nvol && n < max; ++vi) {
                    const HVol *v = &G->vol[vi];
                    if (v->role != HV_MAIN && v->role != HV_PAIR && v->role != HV_ROW) continue;
                    HomePrism *q = &out[n++];
                    double cx, cz, ex, ez;
                    const float mx = 0.5f * (v->x0 + v->x1), mz = 0.5f * (v->z0 + v->z1);
                    home_to_world(G, mx, mz, &cx, &cz);
                    if (v->ridgeX) home_to_world(G, mx + 1.0f, mz, &ex, &ez);
                    else home_to_world(G, mx, mz + 1.0f, &ex, &ez);
                    q->x = cx; q->z = cz; q->mx = r.mx; q->mz = r.mz;
                    q->ax = (float)(ex - cx); q->az = (float)(ez - cz);
                    q->hx = 0.5f * (v->ridgeX ? v->x1 - v->x0 : v->z1 - v->z0);
                    q->hz = 0.5f * (v->ridgeX ? v->z1 - v->z0 : v->x1 - v->x0);
                    float lo = 1e9f;
                    for (int c = 0; c < 4; ++c) {
                        double wx, wz;
                        home_to_world(G, c & 1 ? v->x1 : v->x0, c & 2 ? v->z1 : v->z0, &wx, &wz);
                        lo = fminf(lo, city_ground_y(wx, wz));
                    }
                    const float y = r.base + lift + SIDEWALK_HEIGHT;
                    q->y0 = fminf(lo, r.base) - 0.5f;
                    q->wall = f.wall; q->wallCol = f.wallCol; q->storeys = v->storeys;
                    if (v->roof == HR_FLAT) {
                        q->eaves = y + v->top; q->rise = 0.0f; q->inset = 0.0f;
                        q->roof = f.flat; q->roofCol = f.flatCol;
                    } else {
                        q->eaves = y + v->eaves; q->rise = fmaxf(0.0f, v->top - v->eaves);
                        q->inset = v->roof == HR_HIP ? fminf(q->hx, q->hz) : 0.0f;
                        q->roof = f.roof; q->roofCol = f.roofCol;
                    }
                }
            }
        }
        i = j;
    }
    free(P);
    return n;
}

/* ---------------------------------------------------------------------------
 * The shells into a mesh
 * ------------------------------------------------------------------------- */

typedef struct HEmit { Mesh *m; const HomeRow *r; const HGraph *g; float sx; } HEmit;

static void emit_face(void *ctx, const HShellFace *f) {
    HEmit *E = (HEmit *)ctx;
    Mesh *m = E->m;
    const int u = f->unit >= 0 && f->unit < E->g->nunit ? f->unit : 0;
    const float dy = E->r->dy[u];
    m->xy = E->r->base + dy;   /* the unit's ground: what its heights count from (Vertex.base), and its walls' footing */
    const uint8_t prm[4] = { MAT_HOUSE, f->surf, f->finish, f->colour };
    const uint32_t seed = E->g->unit[u].seed;
    const vec3 n = v3(E->sx * f->nrm[0], f->nrm[1], f->nrm[2]);
    vec3 p[HSH_MAXV];
    float uv[HSH_MAXV][2];
    for (int q = 0; q < f->n; ++q) {
        p[q] = v3(E->sx * f->p[q][0], f->p[q][1] - dy + SIDEWALK_HEIGHT, f->p[q][2]);
        uv[q][0] = f->uv[q][0]; uv[q][1] = f->uv[q][1];
    }
    /* an opening's u runs along (-n.z, n.x) in the world, mirrored rows' too (house.glsl finds its corner by it) */
    if (E->sx < 0.0f && (f->surf == HF_GLASS || f->surf == HF_DOOR || f->surf == HF_GARAGE)) {
        const float w = (float)((f->aux >> HOUSE_AUX_W) & 1023u) * 0.01f;
        for (int q = 0; q < f->n; ++q) uv[q][0] = w - uv[q][0];
    }
    for (int t = 1; t + 1 < f->n; ++t) {
        int k[3] = { 0, t, t + 1 };
        if (v3_dot(v3_cross(v3_sub(p[k[1]], p[k[0]]), v3_sub(p[k[2]], p[k[0]])), n) < 0.0f) { k[1] = t + 1; k[2] = t; }
        for (int q = 0; q < 3; ++q) mesh_push(m, p[k[q]], n, uv[k[q]][0], uv[k[q]][1], seed, prm, f->aux);
    }
}

/* Row r's shell at level lod (HSH_*) into m, in its frame. */
static void emit_row(Mesh *m, ChunkGen *g, int ri, int lod) {
    const HGraph *G = chunk_gen_home(g, ri);
    if (G->empty) return;
    const HomeRow *r = &g->hrow[ri];
    /* the row's frame: x along its street from its first plot, z in; a frame whose z is t turned clockwise is a
     * mirror image of the mesh's, which is drawn with x reversed */
    const float sx = G->tx * G->nz - G->tz * G->nx > 0.0f ? 1.0f : -1.0f;
    const double ox = G->ox + (double)G->u0 * G->tx, oz = G->oz + (double)G->u0 * G->tz;
    m->xf = 1;
    m->xc = sx * G->tx; m->xs = sx * G->tz;
    m->xx = (float)(ox - g->ox); m->xz = (float)(oz - g->oz);
    m->footing = 1;
    HEmit E = { m, r, G, sx };
    home_shell(G, lod, r->dy, emit_face, &E, NULL);
    m->footing = 0;
    m->xf = 0;
}

static int g_homes = HOMES_NEAR;
void city_set_homes(int mode) { g_homes = mode; }
int  city_homes_mode(void) { return g_homes; }

void build_homes(Mesh *m, ChunkGen *g, int lod) {
    if (g_homes == HOMES_OFF) return;
    chunk_gen_homes(g);
    for (int ri = 0; ri < g->nhrow; ++ri) {
        if (!chunk_gen_owns_home(g, ri)) continue;
        if (lod == HSH_FAR && g_homes == HOMES_NEAR) m->nhome += (uint32_t)g->hrow[ri].n;
        emit_row(m, g, ri, lod);
    }
}

void city_build_homes(int cx, int cz, Mesh *m) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, 25.0, 0.0, 0.0);
    mesh_begin(m, cx, cz);
    build_homes(m, &g, HSH_NEAR);
    chunk_gen_free(&g);
}

/* ---------------------------------------------------------------------------
 * What stands on the ground (collision, the grass)
 * ------------------------------------------------------------------------- */

int city_home_outlines(ChunkGen *g, int ri, double (*v)[4][2], int max) {
    const HGraph *G = chunk_gen_home(g, ri);
    if (G->empty) return 0;
    int n = 0;
    for (int i = 0; i < G->nvol && n < max; ++i) {
        const HVol *vo = &G->vol[i];
        if (vo->y0 > 0.01f || (vo->role == HV_PORCH && vo->kind != HP_ENCLOSED)) continue;   /* canopies hang; open porches stand on posts */
        float q[6][2];
        if (hvol_outline(vo, q) != 4) continue;
        for (int c = 0; c < 4; ++c) home_to_world(G, q[c][0], q[c][1], &v[n][c][0], &v[n][c][1]);
        n++;
    }
    /* open porches' posts */
    for (int i = 0; i < G->nvol && n < max; ++i) {
        const HVol *p = &G->vol[i];
        if (p->role != HV_PORCH || (p->kind != HP_OPEN && p->kind != HP_LEAN && p->kind != HP_VERANDA)) continue;
        const int np = p->kind == HP_VERANDA ? (int)fmaxf(2.0f, ceilf((p->x1 - p->x0) / 2.4f) + 1.0f) : 2;
        for (int k = 0; k < np && n < max; ++k) {
            const float x = p->x0 + 0.1f + (p->x1 - p->x0 - 0.2f) * (float)k / (float)(np - 1), z = p->z0 + 0.1f;
            static const float C[4][2] = { { -0.08f, -0.08f }, { 0.08f, -0.08f }, { 0.08f, 0.08f }, { -0.08f, 0.08f } };
            for (int c = 0; c < 4; ++c) home_to_world(G, x + C[c][0], z + C[c][1], &v[n][c][0], &v[n][c][1]);
            n++;
        }
    }
    return n;
}
