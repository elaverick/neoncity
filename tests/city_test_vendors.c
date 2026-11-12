/* Part of tests/city_test.c (#included): where the street vendors stand
 * (src/city/vendor_sites.c, roadmap SV1).
 *
 *   check_vendor_sites   placement rules, reachability, the same sites from
 *                        every chunk's view, the grammar on every site
 *   vendor_sites_cams    city_test --vendor-sites [X Z]: counts and --cam views
 *   check_vendor_shells  the vendors on them (SV2): in their sites, in the
 *                        chunk's mesh and its collision
 *   vendor_cams          city_test --vendors [X Z]: counts, costs, --cam views */
#include "vendor_sites.h"

/* Even-odd point in polygon. */
static int vs_in_poly(const double (*v)[2], int n, double x, double z) {
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((v[i][1] > z) != (v[j][1] > z) && x < (v[j][0] - v[i][0]) * (z - v[i][1]) / (v[j][1] - v[i][1]) + v[i][0]) in = !in;
    return in;
}

/* Least distance between two convex outlines (0 if they meet). */
static double vs_seg_pt(const double *a, const double *b, double x, double z) {
    double dx = b[0] - a[0], dz = b[1] - a[1], l = dx * dx + dz * dz;
    double t = l > 0.0 ? ((x - a[0]) * dx + (z - a[1]) * dz) / l : 0.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    double ex = a[0] + dx * t - x, ez = a[1] + dz * t - z;
    return sqrt(ex * ex + ez * ez);
}
static double vs_gap(const double (*a)[2], int na, const double (*b)[2], int nb) {
    for (int i = 0; i < na; ++i) if (vs_in_poly(b, nb, a[i][0], a[i][1])) return 0.0;
    for (int i = 0; i < nb; ++i) if (vs_in_poly(a, na, b[i][0], b[i][1])) return 0.0;
    double best = 1e30;
    for (int i = 0; i < na; ++i)
        for (int j = 0; j < nb; ++j) {
            best = fmin(best, vs_seg_pt(b[j], b[(j + 1) % nb], a[i][0], a[i][1]));
            best = fmin(best, vs_seg_pt(a[i], a[(i + 1) % na], b[j][0], b[j][1]));
        }
    return best;
}

/* A raster of the ground round the spawn: what blocks a walker, and where the lots are. */
typedef struct VsGrid { double x0, z0, h; int nx, nz; uint8_t *m; } VsGrid;
enum { VS_BLOCKED = 1, VS_LOT = 2, VS_SITE = 4, VS_REACHED = 8, VS_GROWN = 16 };

static void vs_mark_poly(VsGrid *G, const double (*v)[2], int n, uint8_t bit, int set) {
    double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
    for (int i = 0; i < n; ++i) { x0 = fmin(x0, v[i][0]); x1 = fmax(x1, v[i][0]); z0 = fmin(z0, v[i][1]); z1 = fmax(z1, v[i][1]); }
    int i0 = (int)floor((x0 - G->x0) / G->h), i1 = (int)ceil((x1 - G->x0) / G->h);
    int j0 = (int)floor((z0 - G->z0) / G->h), j1 = (int)ceil((z1 - G->z0) / G->h);
    if (i0 < 0) i0 = 0;
    if (j0 < 0) j0 = 0;
    if (i1 > G->nx) i1 = G->nx;
    if (j1 > G->nz) j1 = G->nz;
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            if (!vs_in_poly(v, n, G->x0 + (i + 0.5) * G->h, G->z0 + (j + 0.5) * G->h)) continue;
            if (set) G->m[(size_t)j * G->nx + i] |= bit; else G->m[(size_t)j * G->nx + i] &= (uint8_t)~bit;
        }
}

/* The collision outlines round the spawn, exactly (the raster is for the flood). */
typedef struct VsPoly { int n, hn; double x0, z0, x1, z1; double v[64][2], h[64][2]; } VsPoly;
static VsPoly *g_vsp;
static int g_nvsp, g_capvsp;
static void vs_add_poly(const BlockLayout *L, const CollPoly *cp) {
    if (cp->n > 64 || cp->holeN > 64) return;
    if (g_nvsp == g_capvsp) { g_capvsp = g_capvsp ? 2 * g_capvsp : 4096; g_vsp = (VsPoly *)realloc(g_vsp, sizeof(VsPoly) * g_capvsp); }
    VsPoly *p = &g_vsp[g_nvsp++];
    p->n = cp->n; p->hn = cp->holeN;
    p->x0 = p->z0 = 1e300; p->x1 = p->z1 = -1e300;
    for (int k = 0; k < cp->n; ++k) {
        p->v[k][0] = L->collv[cp->first + k][0]; p->v[k][1] = L->collv[cp->first + k][1];
        p->x0 = fmin(p->x0, p->v[k][0]); p->x1 = fmax(p->x1, p->v[k][0]); p->z0 = fmin(p->z0, p->v[k][1]); p->z1 = fmax(p->z1, p->v[k][1]);
    }
    for (int k = 0; k < cp->holeN; ++k) { p->h[k][0] = L->collv[cp->holeFirst + k][0]; p->h[k][1] = L->collv[cp->holeFirst + k][1]; }
}
static int vs_solid(double x, double z) {
    for (int i = 0; i < g_nvsp; ++i) {
        const VsPoly *p = &g_vsp[i];
        if (x < p->x0 || x > p->x1 || z < p->z0 || z > p->z1) continue;
        if (vs_in_poly((const double (*)[2])p->v, p->n, x, z) && !(p->hn && vs_in_poly((const double (*)[2])p->h, p->hn, x, z))) return 1;
    }
    return 0;
}

/* hx, hz: the middle chunk; checks the sites of the (2R-1)^2 chunks round it. */
static void check_vendor_sites(int hx, int hz, const char *where) {
    enum { R = 4, W = 2 * R + 1, MAXS = MAX_VENDOR_SPOTS };
    static VendorSpot sp[W][W][MAXS], near[MAXS];
    static int ns[W][W];
    int bySpace[SPACE_KINDS + 1] = { 0 }, total = 0;
    for (int j = 0; j < W; ++j)
        for (int i = 0; i < W; ++i) ns[j][i] = city_chunk_vendor_spots(hx - R + i, hz - R + j, sp[j][i], MAXS);

    /* 1. Every chunk sees its neighbours' sites near it as they do. */
    int seen = 0, differ = 0;
    for (int j = 1; j < W - 1; ++j)
        for (int i = 1; i < W - 1; ++i) {
            int n = city_vendor_spots_near(hx - R + i, hz - R + j, 12.0, near, MAXS);
            for (int k = 0; k < n; ++k) {
                double mx, mz;
                vendor_spot_middle(&near[k], &mx, &mz);
                int oi = (int)floor(mx / CHUNK_SIZE) - (hx - R), oj = (int)floor(mz / CHUNK_SIZE) - (hz - R);
                if (oi < 0 || oj < 0 || oi >= W || oj >= W || (oi == i && oj == j)) continue;
                seen++;
                int found = 0;
                for (int q = 0; q < ns[oj][oi] && !found; ++q) found = memcmp(&sp[oj][oi][q], &near[k], sizeof(VendorSpot)) == 0;
                differ += !found;
                CHECK(found, "%s: a %s site at (%.1f, %.1f) seen from chunk (%d,%d) differs from its own chunk's", where,
                      vendor_space_name(near[k].space), mx, mz, hx - R + i, hz - R + j);
            }
        }

    /* 2. The ground: what blocks a walker (walls, columns, props: the collision
     * layout) and the sites, over the middle chunks and a margin. */
    VsGrid G;
    G.h = 0.25;
    G.x0 = (hx - R + 1) * (double)CHUNK_SIZE; G.z0 = (hz - R + 1) * (double)CHUNK_SIZE;
    G.nx = G.nz = (int)((W - 2) * CHUNK_SIZE / G.h);
    G.m = (uint8_t *)calloc((size_t)G.nx * G.nz, 1);
    g_nvsp = 0;
    static BlockLayout L;
    for (int j = 1; j < W - 1; ++j)
        for (int i = 1; i < W - 1; ++i) {
            city_layout(hx - R + i, hz - R + j, &L);
            for (int c = 0; c < L.vcoll0; ++c) {   /* not the vendors themselves (check_vendor_shells) */
                const CollPoly *cp = &L.coll[c];
                vs_add_poly(&L, cp);
                vs_mark_poly(&G, (const double (*)[2])&L.collv[cp->first], cp->n, VS_BLOCKED, 1);
                if (cp->holeN) vs_mark_poly(&G, (const double (*)[2])&L.collv[cp->holeFirst], cp->holeN, VS_BLOCKED, 0);
            }
        }
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    plan_area(G.x0, G.z0, G.x0 + G.nx * G.h, G.z0 + G.nz * G.h, pa);
    for (int b = 0; b < pa->nblocks; ++b) {
        const Poly *lot = &pa->blocks[b].lot;
        if (!lot->n) continue;
        double v[PLAN_MAX_PV][2];
        for (int k = 0; k < lot->n; ++k) { v[k][0] = lot->v[k].x; v[k][1] = lot->v[k].z; }
        vs_mark_poly(&G, (const double (*)[2])v, lot->n, VS_LOT, 1);
    }
    for (int j = 0; j < W; ++j)
        for (int i = 0; i < W; ++i)
            for (int k = 0; k < ns[j][i]; ++k) {
                double v[4][2];
                vendor_spot_corners(&sp[j][i][k], v);
                vs_mark_poly(&G, (const double (*)[2])v, 4, VS_SITE, 1);
            }
    /* a walker 0.6 m across: grow what blocks by 0.25 m (and the sites), then flood from every cell off the lots */
    size_t N = (size_t)G.nx * G.nz;
    for (int j = 1; j < G.nz - 1; ++j)
        for (int i = 1; i < G.nx - 1; ++i) {
            size_t c = (size_t)j * G.nx + i;
            if (G.m[c] & (VS_BLOCKED | VS_SITE)) continue;
            if ((G.m[c - 1] | G.m[c + 1] | G.m[c - G.nx] | G.m[c + G.nx]) & (VS_BLOCKED | VS_SITE)) G.m[c] |= VS_GROWN;
        }
    int *q = (int *)malloc(sizeof(int) * N);
    size_t qh = 0, qt = 0;
    #define VS_FREE(c) (!(G.m[c] & (VS_BLOCKED | VS_SITE | VS_GROWN | VS_REACHED)))
    for (size_t c = 0; c < N; ++c)
        if (!(G.m[c] & VS_LOT) && VS_FREE(c)) { G.m[c] |= VS_REACHED; q[qt++] = (int)c; }
    while (qh < qt) {
        int c = q[qh++], i = c % G.nx, j = c / G.nx;
        if (i > 0 && VS_FREE(c - 1)) { G.m[c - 1] |= VS_REACHED; q[qt++] = c - 1; }
        if (i < G.nx - 1 && VS_FREE(c + 1)) { G.m[c + 1] |= VS_REACHED; q[qt++] = c + 1; }
        if (j > 0 && VS_FREE(c - G.nx)) { G.m[c - G.nx] |= VS_REACHED; q[qt++] = c - G.nx; }
        if (j < G.nz - 1 && VS_FREE(c + G.nx)) { G.m[c + G.nx] |= VS_REACHED; q[qt++] = c + G.nx; }
    }
    #undef VS_FREE
    free(q);

    /* 3. Each site of the middle chunks: on its block, clear of walls and of the
     * other sites, its way clear, reachable, and the grammar builds a vendor on it. */
    int walls = 0, close = 0, ways = 0, cut = 0, built = 0, tried = 0, propsIn = 0, hosts = 0, lunch = 0, roofed = 0;
    int arch[VSITE_COUNT][VA_COUNT] = { { 0 } };
    static Prop pr[MAX_PROPS];
    Vendor *V = (Vendor *)malloc(sizeof(Vendor));
    for (int j = 2; j < W - 2; ++j)
        for (int i = 2; i < W - 2; ++i) {
            int np = city_chunk_props(hx - R + i, hz - R + j, pr, MAX_PROPS);
            for (int k = 0; k < ns[j][i]; ++k) {
                const VendorSpot *s = &sp[j][i][k];
                total++;
                bySpace[s->space]++;
                hosts += s->site.hostWall;
                lunch += s->site.lunch;
                roofed += s->site.ceiling > 0.0f;
                double v[4][2], mx, mz;
                vendor_spot_corners(s, v);
                vendor_spot_middle(s, &mx, &mz);
                /* walls: sample the site against the blocked cells (not grown) */
                int hit = 0, wayHit = 0;
                double wx = 0.0, wz = 0.0;
                for (int b = 0; b <= 8 && !hit; ++b)
                    for (int a = 0; a <= 8 && !hit; ++a) {
                        double u = (a / 8.0 - 0.5) * (s->site.w - 0.1), e = 0.05 + (s->site.d - 0.1) * b / 8.0;
                        double x = s->x + s->fz * u + s->fx * e, z = s->z - s->fx * u + s->fz * e;
                        hit = vs_solid(x, z);
                    }
                walls += hit;
                CHECK(!hit, "%s: %s site at (%.1f, %.1f) stands in a wall or a piece of street furniture (--cam %.2f %.2f %.1f -20)", where, vendor_space_name(s->space), mx, mz,
                      mx + s->fx * (s->site.d + 3.0), mz + s->fz * (s->site.d + 3.0), atan2(-s->fx, s->fz) * 57.2958);
                for (int b = 1; b <= 4 && !wayHit; ++b)
                    for (int a = 0; a <= 8 && !wayHit; ++a) {
                        double u = (a / 8.0 - 0.5) * s->site.w, e = s->site.d + s->walk * b / 4.0 - 0.02;
                        double x = s->x + s->fz * u + s->fx * e, z = s->z - s->fx * u + s->fz * e;
                        wayHit = vs_solid(x, z);
                        for (int o = 0; o < ns[j][i] && !wayHit; ++o) wayHit = 2 * vendor_spot_contains(&sp[j][i][o], x, z, 0.0, 0.0);
                        wx = x; wz = z;
                    }
                ways += wayHit;
                CHECK(!wayHit, "%s: the way in front of a %s site at (%.1f, %.1f) is blocked at (%.2f, %.2f) by %s", where, vendor_space_name(s->space), mx, mz,
                      wx, wz, wayHit == 2 ? "another site" : "a wall or a piece of street furniture");
                /* the other sites: V_GAP_MIN apart */
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        for (int o = 0; o < ns[j + dj][i + di]; ++o) {
                            const VendorSpot *t = &sp[j + dj][i + di][o];
                            if (t == s) continue;
                            double w[4][2];
                            vendor_spot_corners(t, w);
                            double gap = vs_gap((const double (*)[2])v, 4, (const double (*)[2])w, 4);
                            close += gap < V_GAP_MIN - 0.05;
                            CHECK(gap >= V_GAP_MIN - 0.05, "%s: sites at (%.1f, %.1f) and (%.1f, %.1f) are %.2f m apart", where, s->x, s->z, t->x, t->z, gap);
                        }
                /* street furniture keeps off the sites and their ways */
                for (int p = 0; p < np; ++p) {
                    if (pr[p].kind == PROP_AIRCON) continue;
                    double pv[4][2];
                    int n = prop_outline(&pr[p], pv);
                    int in = vendor_spot_contains(s, pr[p].x, pr[p].z, 0.0, s->walk);
                    for (int c = 0; c < n && !in; ++c) in = vendor_spot_contains(s, pv[c][0], pv[c][1], 0.0, s->walk);
                    propsIn += in;
                    CHECK(!in, "%s: a piece of street furniture (kind %d) at (%.1f, %.1f) stands on a %s site or its way", where, pr[p].kind, pr[p].x, pr[p].z, vendor_space_name(s->space));
                }
                /* reachable: the way's middle reached from off the lots */
                if (s->space != SPACE_KERB) {
                    int ok = 0;
                    for (int a = -2; a <= 2 && !ok; ++a)
                        for (int b = 1; b <= 3 && !ok; ++b) {
                            double u = a * s->site.w * 0.2, e = s->site.d + s->walk * b / 4.0;
                            double x = s->x + s->fz * u + s->fx * e, z = s->z - s->fx * u + s->fz * e;
                            int gi = (int)((x - G.x0) / G.h), gj = (int)((z - G.z0) / G.h);
                            ok = gi >= 0 && gj >= 0 && gi < G.nx && gj < G.nz && (G.m[(size_t)gj * G.nx + gi] & VS_REACHED);
                        }
                    cut += !ok;
                    CHECK(ok, "%s: a %s site at (%.1f, %.1f) can't be reached from the street", where, vendor_space_name(s->space), mx, mz);
                }
                /* the grammar builds a valid vendor on it */
                tried++;
                if (vendor_generate(&s->site, VTIER_STANDARD, NULL, 0, V, NULL)) {
                    built++;
                    arch[s->site.kind][V->archetype]++;
                } else if (tried - built <= 6) {
                    static char buf[1 << 14];
                    BLog lg = { buf, (int)sizeof(buf), 0 };
                    buf[0] = 0;
                    vendor_generate(&s->site, VTIER_STANDARD, NULL, 0, V, &lg);
                    printf("  no vendor on a %s site %.2f x %.2f (wall %d, ceiling %.1f, affluence %.2f):\n", vendor_space_name(s->space), s->site.w, s->site.d,
                           s->site.hostWall, s->site.ceiling, s->site.affluence);
                    const char *t = lg.len > 600 ? buf + lg.len - 600 : buf;
                    printf("%s\n", t);
                }
            }
        }
    free(V);
    free(pa);
    free(G.m);
    printf("vendor sites round %s (%d x %d chunks): %d: %d kerb, %d apron, %d lane, %d pocket market, %d plaza, %d court; "
           "%d against a wall, %d under a roof, %d lunch\n", where, W - 4, W - 4, total, bySpace[SPACE_KERB], bySpace[SPACE_APRON],
           bySpace[SPACE_LANE], bySpace[SPACE_POCKET], bySpace[SPACE_PLAZA], bySpace[SPACE_COURT], hosts, roofed, lunch);
    printf("  %d seen from a neighbour (%d differ); %d in walls, %d too close, %d ways blocked, %d cut off, %d with furniture on them; "
           "%d of %d built valid\n", seen, differ, walls, close, ways, cut, propsIn, built, tried);
    for (int k = 0; k < VSITE_COUNT; ++k) {
        int any = 0;
        for (int a = 0; a < VA_COUNT; ++a) any += arch[k][a];
        if (!any) continue;
        printf("  %-13s", vendor_site_name(k));
        for (int a = 0; a < VA_COUNT; ++a) if (arch[k][a]) printf(" %s %d", vendor_archetype_name(a), arch[k][a]);
        printf("\n");
    }
    CHECK(built == tried, "%s: %d of %d sites have no valid vendor", where, tried - built, tried);
}

/* ------------------------------------------------------------ the shells (SV2) --- */

typedef struct VsShells {
    int vendors, arch[VA_COUNT], cls[VC_COUNT], tris, trisMax, drawn, solids, floors, outside, high, collMiss, rooms;
    VendorMesh vm;
    /* one of each archetype, the nearest to a point, for the views */
    double px, pz, best[VA_COUNT];
    VendorSpot spot[VA_COUNT];
    int trade[VA_COUNT];
    const char *where;
} VsShells;

static void vs_shell_visit(void *ctx, const VendorSpot *s, const Vendor *v) {
    VsShells *A = (VsShells *)ctx;
    A->vendors++;
    A->arch[v->archetype]++;
    A->cls[v->cls]++;
    /* what the chunk's mesh should draw of it: its shell (lit neon faces as MAT_NEON, the rest MAT_VENDOR) */
    A->vm.count = 0;
    int t = vendor_mesh(v, VL_SHELL, &A->vm), lit = 0;
    for (uint32_t i = 0; i + 2 < A->vm.count; i += 3) {
        const VendorVertex *x = &A->vm.v[i];
        lit += x->surf == VS_NEON && (x->flags & VPF_EMISSIVE) && !(x->rgb & (1u << 30));
        A->rooms += x->surf == VS_ROOM;
    }
    A->tris += t;
    /* a canopy bar's parts are never drawn: the chunk draws its plan's far shell (MAT_KIT, mesh_vendor.c) */
    if (v->archetype != VA_CANOPY) A->drawn += t - lit;
    if (t > A->trisMax) A->trisMax = t;
    /* everything it is stands in its site, below VENDOR_TOP_MAX (the cables) and its ceiling */
    const float hw = 0.5f * s->site.w + 0.02f, d = s->site.d + 0.02f;
    int out = 0;
    for (int i = 0; i < v->nstructure; ++i) {
        float mn[3], mx[3];
        vendor_part_aabb(&v->part[i], mn, mx);
        out |= mn[0] < -hw || mx[0] > hw || mn[2] < -0.02f || mx[2] > d;
    }
    VendorSolid so[VENDOR_SOLIDS_MAX];
    int ns = vendor_solids(v, so, VENDOR_SOLIDS_MAX);
    for (int i = 0; i < ns; ++i) {
        out |= so[i].x0 < -hw || so[i].x1 > hw || so[i].z0 < -0.02f || so[i].z1 > d;
        A->floors += so[i].floor;
        A->solids += !so[i].floor;
    }
    double mx, mz;
    vendor_spot_middle(s, &mx, &mz);
    A->outside += out;
    CHECK(!out, "%s: a %s at (%.1f, %.1f) reaches out of its %.1f x %.1f site", A->where, vendor_archetype_name(v->archetype), mx, mz, s->site.w, s->site.d);
    int high = v->top > VENDOR_TOP_MAX + 0.01f || (s->site.ceiling > 0.0f && v->top > s->site.ceiling + 0.01f);
    A->high += high;
    CHECK(!high, "%s: a %s at (%.1f, %.1f) stands %.2f m high (ceiling %.1f)", A->where, vendor_archetype_name(v->archetype), mx, mz, v->top, s->site.ceiling);
    CHECK(ns > 0, "%s: a %s at (%.1f, %.1f) has nothing solid", A->where, vendor_archetype_name(v->archetype), mx, mz);
    double dd = hypot(mx - A->px, mz - A->pz);
    if (dd < A->best[v->archetype]) { A->best[v->archetype] = dd; A->spot[v->archetype] = *s; A->trade[v->archetype] = v->trade; }
}

/* Each vendor of the 3 x 3 chunks round (hx, hz): in its site, its shell in
 * the chunk's mesh, its solids in the chunk's collision. Prints counts and
 * costs; with views, a --cam on the nearest of each archetype. */
static void check_vendor_shells(int hx, int hz, const char *where, int views) {
    static VsShells A;
    vendor_mesh_free(&A.vm);
    memset(&A, 0, sizeof(A));
    A.where = where;
    A.px = (hx + 0.5) * CHUNK_SIZE; A.pz = (hz + 0.5) * CHUNK_SIZE;
    for (int k = 0; k < VA_COUNT; ++k) A.best[k] = 1e30;
    static BlockLayout L;
    int chunkTris = 0, chunkMax = 0, baseMax = 0, colls = 0, floors = 0;
    double ms = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            int before = A.drawn, solids = A.solids, fl = A.floors, roomsBefore = A.rooms;
            clock_t c0 = clock();
            city_chunk_vendors(hx + i, hz + j, vs_shell_visit, &A);
            ms += 1000.0 * (double)(clock() - c0) / CLOCKS_PER_SEC;
            Mesh m;
            memset(&m, 0, sizeof(m));
            city_build_mesh(hx + i, hz + j, &m);
            int drawn = 0, painted = 0;
            for (uint32_t v = 0; v + 2 < m.count; v += 3) {
                const Vertex *x = &m.v[v];
                drawn += x->params[0] == MAT_VENDOR;
                if (x->params[0] != MAT_VENDOR || x->params[1] != VS_ROOM) continue;
                /* a room's words (mesh_vendor.c room_word): its size, depth, stock and kind, read by vendor_room.glsl */
                uint32_t w = x->aux;
                painted += x->params[2] > 0 && x->params[3] > 0 && x->nrm[3] > 0 && (w & 31u) < VK_COUNT && ((w >> 5) & 31u) < VK_COUNT &&
                           ((w >> 10) & 31u) < VK_COUNT && ((w >> 18) & 7u) <= VO_CABINET && ((w >> 21) & 15u) < V_PALETTES && (x->seed >> 16) != 0;
            }
            /* exactly its vendors' shells, less their lit faces (MAT_NEON, as the facades' signs) */
            CHECK(drawn == A.drawn - before, "%s: chunk (%d,%d) draws %d vendor triangles, its vendors have %d", where, hx + i, hz + j, drawn, A.drawn - before);
            CHECK(painted == A.rooms - roomsBefore, "%s: chunk (%d,%d) paints %d rooms' triangles, its vendors' openings have %d", where, hx + i, hz + j, painted, A.rooms - roomsBefore);
            chunkTris += drawn;
            if (drawn > chunkMax) chunkMax = drawn;
            if ((int)m.count > baseMax) baseMax = (int)m.count;
            mesh_free(&m);
            city_layout(hx + i, hz + j, &L);
            int nc = L.ncoll - L.vcoll0;
            colls += nc;
            CHECK(nc == A.solids - solids, "%s: chunk (%d,%d) collides with %d vendor outlines of %d", where, hx + i, hz + j, nc, A.solids - solids);
            floors += A.floors - fl;
        }
    printf("vendors round %s (3 x 3 chunks): %d (poor %d, middling %d, wealthy %d), %.2f ms a chunk to generate\n", where, A.vendors,
           A.cls[VC_POOR], A.cls[VC_MID], A.cls[VC_RICH], ms / 9.0);
    printf(" ");
    for (int a = 0; a < VA_COUNT; ++a) if (A.arch[a]) printf(" %s %d", vendor_archetype_name(a), A.arch[a]);
    printf("\n  shells: %d triangles (%.0f a vendor, most %d); a chunk's most %d (of %d vertices in its base layer); %d outlines, %d floors; "
           "%d out of their sites, %d too high\n", A.tris, A.vendors ? (double)A.tris / A.vendors : 0.0, A.trisMax, chunkMax, baseMax, colls, floors,
           A.outside, A.high);
    if (views)
        for (int a = 0; a < VA_COUNT; ++a) {
            if (A.best[a] > 1e29) continue;
            const VendorSpot *s = &A.spot[a];
            double mx, mz;
            vendor_spot_middle(s, &mx, &mz);
            double dd = s->site.d * 0.5 + s->walk + 3.0;
            double cx = mx + s->fx * dd + s->fz * 2.0, cz = mz + s->fz * dd - s->fx * 2.0;
            printf("  %-10s %-11s (%s, %.1f x %.1f) at (%.1f, %.1f): --cam %.2f %.2f %.1f -8\n", vendor_archetype_name(a), vendor_trade_name(A.trade[a]),
                   vendor_space_name(s->space), s->site.w, s->site.d, mx, mz, cx, cz, atan2(mx - cx, -(mz - cz)) * 57.2958);
        }
}

/* The middle of a block near (x, z) with a space of this kind (or -1: none found). */
static int vs_find_space(double x, double z, int kind, double *ox, double *oz) {
    PlanArea *pa = (PlanArea *)malloc(sizeof(PlanArea));
    double best = 1e30;
    for (int ring = 0; ring < 12 && best > 1e29; ++ring) {
        double r = 600.0 * (ring + 1);
        plan_area(x - r, z - r, x + r, z + r, pa);
        for (int b = 0; b < pa->nblocks; ++b) {
            CitySpace s[MAX_BLOCK_SPACES];
            const Block *bk = &pa->blocks[b];
            if (bk->zone != ZONE_CENTRE && bk->zone != ZONE_CORE) continue;
            int n = city_block_spaces(bk, s, MAX_BLOCK_SPACES);
            for (int k = 0; k < n; ++k) {
                if (s[k].kind != kind) continue;
                double cu = 0.5 * (s[k].u0 + s[k].u1), cv = 0.5 * (s[k].v0 + s[k].v1);
                double px = s[k].ox + s[k].ux * cu + s[k].vx * cv, pz = s[k].oz + s[k].uz * cu + s[k].vz * cv;
                double d = hypot(px - x, pz - z);
                if (d < best) { best = d; *ox = px; *oz = pz; }
            }
        }
    }
    free(pa);
    return best < 1e29 ? 0 : -1;
}

/* city_test --vendor-sites [X Z]: the sites round the spawn (or X Z), and
 * round the nearest lane, pocket market, plaza and hawker frontage: counts,
 * checks, and --cam views of a few of each kind. */
static int vendor_sites_cams(int argc, char **argv) {
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    if (argc > 3) { sx = atof(argv[2]); sz = atof(argv[3]); }
    check_vendor_sites((int)floor(sx / CHUNK_SIZE), (int)floor(sz / CHUNK_SIZE), "the spawn");
    static const int kinds[4] = { SPACE_LANE, SPACE_POCKET, SPACE_PLAZA, SPACE_APRON };
    for (int k = 0; k < 4; ++k) {
        double x, z;
        if (vs_find_space(sx, sz, kinds[k], &x, &z) < 0) { printf("no %s found\n", vendor_space_name(kinds[k])); continue; }
        char where[64];
        snprintf(where, sizeof(where), "a %s at (%.0f, %.0f)", vendor_space_name(kinds[k]), x, z);
        check_vendor_sites((int)floor(x / CHUNK_SIZE), (int)floor(z / CHUNK_SIZE), where);
        VendorSpot s[MAX_VENDOR_SPOTS];
        int n = city_chunk_vendor_spots((int)floor(x / CHUNK_SIZE), (int)floor(z / CHUNK_SIZE), s, MAX_VENDOR_SPOTS), shown = 0;
        for (int i = 0; i < n && shown < 2; ++i) {
            if (s[i].space != kinds[k]) continue;
            double mx, mz;
            vendor_spot_middle(&s[i], &mx, &mz);
            double d = s[i].site.d * 0.5 + s[i].walk + 2.5;
            double cx = mx + s[i].fx * d + s[i].fz * 1.5, cz = mz + s[i].fz * d - s[i].fx * 1.5;
            printf("  %s site %.1f x %.1f at (%.1f, %.1f): --cam %.2f %.2f %.1f -15\n", vendor_space_name(kinds[k]), s[i].site.w, s[i].site.d,
                   mx, mz, cx, cz, atan2(mx - cx, -(mz - cz)) * 57.2958);
            shown++;
        }
    }
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    return 0;
}

/* city_test --vendors [X Z]: the vendors round the spawn (or X Z) and round the
 * nearest lane, pocket market, plaza and hawker frontage: checks, counts, costs
 * and a --cam view of the nearest of each archetype. */
static int vendor_cams(int argc, char **argv) {
    double sx, sz;
    float yaw;
    city_spawn(&sx, &sz, &yaw);
    if (argc > 3) { sx = atof(argv[2]); sz = atof(argv[3]); }
    check_vendor_shells((int)floor(sx / CHUNK_SIZE), (int)floor(sz / CHUNK_SIZE), "the spawn", 1);
    static const int kinds[4] = { SPACE_LANE, SPACE_POCKET, SPACE_PLAZA, SPACE_APRON };
    for (int k = 0; k < 4; ++k) {
        double x, z;
        if (vs_find_space(sx, sz, kinds[k], &x, &z) < 0) { printf("no %s found\n", vendor_space_name(kinds[k])); continue; }
        char where[64];
        snprintf(where, sizeof(where), "a %s at (%.0f, %.0f)", vendor_space_name(kinds[k]), x, z);
        check_vendor_shells((int)floor(x / CHUNK_SIZE), (int)floor(z / CHUNK_SIZE), where, 1);
    }
    if (g_failures) { printf("%d failure(s)\n", g_failures); return 1; }
    return 0;
}

/* city_test --vs-probe X Z: what stands at (X, Z) (debugging the checks above). */
static int vs_probe(double x, double z) {
    int cx = (int)floor(x / CHUNK_SIZE), cz = (int)floor(z / CHUNK_SIZE);
    static BlockLayout L;
    for (int dj = -1; dj <= 1; ++dj)
        for (int di = -1; di <= 1; ++di) {
            city_layout(cx + di, cz + dj, &L);
            for (int c = 0; c < L.ncoll; ++c) {
                const CollPoly *cp = &L.coll[c];
                if (vs_in_poly((const double (*)[2])&L.collv[cp->first], cp->n, x, z))
                    printf("chunk (%d,%d) coll %d: %d points, first (%.2f, %.2f)\n", cx + di, cz + dj, c, cp->n, L.collv[cp->first][0], L.collv[cp->first][1]);
            }
            for (int b = 0; b < L.nbuild; ++b) {
                const Building *B = &L.build[b];
                double dx = x - B->ox, dz = z - B->oz, lx = B->c * dx + B->s * dz, lz = -B->s * dx + B->c * dz;
                if (lx > B->foot.x0 - 1 && lx < B->foot.x1 + 1 && lz > B->foot.z0 - 1 && lz < B->foot.z1 + 1) printf("  near the plot of building %d of chunk (%d,%d), seed %08x (local %.2f, %.2f; plot %.1f..%.1f x %.1f..%.1f)\n",
                                                                                                         b, cx + di, cz + dj, B->seed, lx, lz, B->foot.x0, B->foot.x1, B->foot.z0, B->foot.z1);
            }
        }
    static Prop pr[MAX_PROPS];
    int n = city_chunk_props(cx, cz, pr, MAX_PROPS);
    for (int i = 0; i < n; ++i)
        if (hypot(pr[i].x - x, pr[i].z - z) < 2.0) printf("prop kind %d at (%.2f, %.2f)\n", pr[i].kind, pr[i].x, pr[i].z);
    return 0;
}
