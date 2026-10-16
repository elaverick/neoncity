#include "city_internal.h"
#include "building.h"
#include "hash.h"
#include "mathlib.h"

/* ===========================================================================
 * Street litter: the third, near-only layer of chunk meshes (src/render/chunks.c).
 *
 * Rubbish lies still, so it isn't simulated. Every item is ordinary geometry,
 * generated with its chunk as a pure function of the plan. The vertex shader
 * (litter in shaders/city.vert) does the little that moves: dry paper and bags
 * stir in the gusts, soaked paper and film slump flat, and each item shrinks
 * away before its own fade distance, so nothing small is drawn far off and
 * nothing pops. The CPU does nothing per frame.
 *
 * It lies where rubbish collects:
 *   - along the gutters, in the run-off, so always soaked;
 *   - at the kerb line and the building line of the sidewalks;
 *   - round lamp posts;
 *   - outside office doors, where people stand and smoke (butts);
 *   - bin bags piled by service doors and loading bays, and in poorer streets
 *     on the kerb;
 *   - thinly over the open sidewalk, the carriageway (run flat) and the parks,
 *     and thickly in alleys and courtyards, most at the foot of the walls.
 * How much depends on the block's squalor (city_squalor): its zone, and how
 * well off the district is (plan_affluence). Every street has a little. What
 * is too small and too plentiful to be geometry (flecks of paper, leaves,
 * grit, chewing gum, grime) the ground shader draws from the same squalor,
 * which the paving and carriageways carry (debris in shaders/city.frag).
 *
 * Each source (a kerb edge, a door, a lamp, a 2.5 m cell of ground) has its
 * own random stream, and an item belongs to the chunk it lies in, so an item
 * comes out the same from whichever chunk builds it.
 *
 * Vertex layout (MAT_LITTER):
 *   pos      rest position (chunk-local)
 *   nrm.w    ambient occlusion, 0..127
 *   uv       the item's own surface coordinates (print, labels)
 *   seed     the item's hash (its colours and print)
 *   params   y kind (LK_*); z flex 0..255, how freely the vertex moves in the
 *            wind; w fade distance / 4 m (bits 0-5) | LF_WATER | LF_RUNOVER
 *   aux      offset from the item's anchor on the ground, in chunk axes:
 *            x and z 11-bit signed in 2 mm, y 10-bit in 1 mm
 * ========================================================================= */

#define BASE_Y   (-0.03f)   /* plain asphalt filling the junctions (src/city/city.c) */
#define CELL     2.5        /* area scatter grid; divides CHUNK_SIZE */
#define MAX_LV   768
#define TAU      6.2831853f
#define BUDGET   180000     /* vertices per chunk, a safety margin inside MAX_CHUNK_PAGES * PAGE_VERTS */

enum { LF_WATER = 64, LF_RUNOVER = 128 };   /* params.w: lying in the gutter's run-off; run flat by traffic */
enum { G_ROAD = 0, G_SIDEWALK, G_LOT, G_PARK };
enum { WANT_ANY = -1, WANT_FOOT = -2 };     /* site_ok: any ground; sidewalk or lot */

/* Fade distance (m) and footprint radius (m) of each kind. */
static const float FADE[LK_COUNT]   = { 0, 75, 40, 30, 95, 35, 50, 55, 16, 50, 55, 65, 116 };
static const float RADIUS[LK_COUNT] = { 0, 0.3f, 0.12f, 0.12f, 0.45f, 0.06f, 0.09f, 0.15f, 0.02f, 0.08f, 0.22f, 0.16f, 0.3f };

/* What lies where, by weight (shadow, news, flyer, scrap, cardboard, wad, can, bottle, butt, cup, box, bag, bin bag).
 * Butts are scattered on their own. */
static const float MIX_GUTTER[LK_COUNT] = { 0, 10, 8, 14, 4, 10, 12, 6, 0, 9, 5, 8, 0 };
static const float MIX_WALK[LK_COUNT]   = { 0, 14, 14, 16, 6, 9, 8, 3, 0, 10, 6, 8, 0 };
static const float MIX_LOT[LK_COUNT]    = { 0, 8, 6, 16, 10, 14, 10, 8, 0, 6, 8, 8, 2 };
static const float MIX_ROAD[LK_COUNT]   = { 0, 10, 8, 16, 4, 0, 0, 0, 0, 0, 0, 6, 0 };
static const float MIX_PARK[LK_COUNT]   = { 0, 6, 6, 14, 2, 10, 10, 6, 0, 8, 6, 8, 0 };
static const float MIX_SHOP[LK_COUNT]   = { 0, 2, 8, 20, 0, 8, 2, 0, 0, 10, 4, 2, 0 };
static const float MIX_LAMP[LK_COUNT]   = { 0, 4, 8, 14, 0, 10, 10, 3, 0, 10, 4, 4, 0 };
static const float MIX_BACK[LK_COUNT]   = { 0, 2, 0, 4, 10, 6, 2, 2, 0, 0, 8, 2, 0 };   /* round a bin bag pile */

/* ---------------------------------------------------------------- context --- */

typedef struct LV { vec3 p, n; float u, v, flex; } LV;
typedef struct Tris { int n; LV v[MAX_LV]; } Tris;

typedef struct LGen {
    ChunkGen  c;             /* the plan, buildings and lamps round the chunk */
    float    *sq;            /* squalor of each block, < 0 until asked */
    DropKerb **drops;        /* drop kerbs of each block, found when first asked */
    int      *nd;            /* how many, < 0 until asked */
    Tris     *T;             /* the item being built */
    Mesh     *m;
} LGen;

static void lgen_init(LGen *g, int cx, int cz, Mesh *m) {
    memset(g, 0, sizeof(*g));
    /* lamps are found within 2 m, which must stay 20 m inside the plan area (chunk_gen_init) */
    chunk_gen_init(&g->c, cx, cz, 30.0, 8.0, 2.0);
    g->m = m;
    g->T = (Tris *)malloc(sizeof(Tris));
    g->sq = (float *)malloc(sizeof(float) * PLAN_MAX_BLOCKS);
    g->drops = (DropKerb **)calloc(PLAN_MAX_BLOCKS, sizeof(DropKerb *));
    g->nd = (int *)malloc(sizeof(int) * PLAN_MAX_BLOCKS);
    if (!g->T || !g->sq || !g->drops || !g->nd) fatal("out of memory generating litter");
    for (int k = 0; k < PLAN_MAX_BLOCKS; ++k) { g->sq[k] = -1.0f; g->nd[k] = -1; }
}

static void lgen_free(LGen *g) {
    for (int k = 0; k < PLAN_MAX_BLOCKS; ++k) free(g->drops[k]);
    free(g->sq); free(g->T); free(g->drops); free(g->nd);
    chunk_gen_free(&g->c);
    memset(g, 0, sizeof(*g));
}

/* Industrial land and the estates start dirtier than the cores and parks;
 * poorer districts are dirtier still. */
float city_squalor(int zone, double x, double z) {
    static const float base[6] = { 0.42f, 0.22f, 0.5f, 0.42f, 0.66f, 0.3f };   /* a road, core, centre, estate, industrial, park */
    return clampf(base[zone + 1] + 1.1f * (0.5f - plan_affluence(x, z)), 0.04f, 1.0f);
}

static float squalor(LGen *g, int bi) {
    if (g->sq[bi] < 0.0f) {
        const Block *bk = &g->c.pa->blocks[bi];
        DVec2 c = poly_centroid(&bk->curb);
        g->sq[bi] = city_squalor(bk->zone, c.x, c.z);
    }
    return g->sq[bi];
}

/* How much rubbish lies about, as a share of the base rates: a little everywhere, far more in squalor. */
static float amount(float sq) { return 0.1f + 1.25f * sq * sq; }

/* ----------------------------------------------------------------- ground --- */

typedef struct Ground { int type, block; float y; } Ground;

static int block_drops(LGen *g, int bi, const DropKerb **out) {
    if (g->nd[bi] < 0) {
        g->drops[bi] = (DropKerb *)malloc(sizeof(DropKerb) * MAX_BLOCK_DROPS);
        if (!g->drops[bi]) fatal("out of memory generating litter");
        g->nd[bi] = city_block_drops(g->c.pa, &g->c.pa->blocks[bi], g->drops[bi], MAX_BLOCK_DROPS);
    }
    *out = g->drops[bi];
    return g->nd[bi];
}

/* What the ground is at (x, z) and how high it stands (src/city/city.c: build_roads, build_blocks). */
static Ground ground_at(LGen *g, double x, double z) {
    Ground gr = { G_ROAD, -1, BASE_Y };
    for (int k = 0; k < g->c.pa->nblocks; ++k) {
        const Block *bk = &g->c.pa->blocks[k];
        if (x < bk->x0 || x > bk->x1 || z < bk->z0 || z > bk->z1 || !poly_contains(&bk->curb, x, z, 0.0)) continue;
        gr.block = k;
        if (bk->lot.n && poly_contains(&bk->lot, x, z, 0.0)) {
            gr.type = bk->zone == ZONE_PARK ? G_PARK : G_LOT;
            gr.y = SIDEWALK_HEIGHT + 0.006f;
        } else {
            gr.type = G_SIDEWALK;
            gr.y = SIDEWALK_HEIGHT + (poly_contains(&bk->curb, x, z, -KERB_STONE_WIDTH) ? 0.0f : 0.003f);
            /* down a drop kerb, or on the kerb stones along its sides */
            const DropKerb *dk;
            int nd = block_drops(g, k, &dk);
            for (int i = 0; i < nd; ++i) {
                if (city_drop_contains(&dk[i], x, z, 0.0)) {
                    gr.y = city_drop_height(&dk[i], x, z);
                    if (gr.y <= DROP_KERB_HEIGHT) gr.y += 0.003f;
                    break;
                }
                if (city_drop_contains(&dk[i], x, z, KERB_STONE_WIDTH)) gr.y = SIDEWALK_HEIGHT + 0.003f;
            }
        }
        return gr;
    }
    /* a marked carriageway (y = 0) or the plain asphalt at a junction */
    for (int ri = 0; ri < g->c.pa->nroads; ++ri) {
        const Road *r = &g->c.pa->roads[ri];
        double dx = r->b.x - r->a.x, dz = r->b.z - r->a.z, len = sqrt(dx * dx + dz * dz);
        if (len < 1.0 || len - r->trimA - r->trimB < 1.0) continue;
        double ux = dx / len, uz = dz / len, px = x - r->a.x, pz = z - r->a.z;
        double s = px * ux + pz * uz, t = -px * uz + pz * ux;
        if (s >= r->trimA && s <= len - r->trimB && fabs(t) <= r->at.half) { gr.y = 0.0f; break; }
    }
    return gr;
}

static int in_plot(const Building *b, double x, double z, double margin, float *lx, float *lz) {
    double dx = x - b->ox, dz = z - b->oz;
    double px = b->c * dx + b->s * dz, pz = -b->s * dx + b->c * dz;
    *lx = (float)px; *lz = (float)pz;
    return px > b->foot.x0 - margin && px < b->foot.x1 + margin && pz > b->foot.z0 - margin && pz < b->foot.z1 + margin;
}

/* Whether something of radius rad at (x, z) would stand in a wall, a column or a lamp post. */
static int blocked(LGen *g, double x, double z, float rad) {
    for (int i = 0; i < g->c.nl; ++i) {
        double dx = x - g->c.lamp[i].x, dz = z - g->c.lamp[i].z, r = 0.22 + rad;
        if (dx * dx + dz * dz < r * r) return 1;
    }
    for (int i = 0; i < g->c.nb; ++i) {
        float lx, lz;
        if (!in_plot(&g->c.bld[i], x, z, rad + 0.05, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(&g->c, i);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
            if (bfoot_contains(&s->foot, lx, lz) || bfoot_boundary_dist(&s->foot, lx, lz, NULL, NULL) < rad) return 1;
        }
        for (int k = 0; k < bg->nsup; ++k) {
            const BSupport *sp = &bg->sup[k];
            if (sp->type != SUP_COLUMN && sp->type != SUP_STRUT) continue;
            float h = sp->size * 0.5f + rad;
            if (sp->y0 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x0 - lx) < h && fabsf(sp->z0 - lz) < h) return 1;
            if (sp->y1 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x1 - lx) < h && fabsf(sp->z1 - lz) < h) return 1;
        }
    }
    return 0;
}

/* Distance to the nearest wall standing at street level, up to maxD. */
static float wall_dist(LGen *g, double x, double z, float maxD) {
    float best = maxD;
    for (int i = 0; i < g->c.nb; ++i) {
        float lx, lz;
        if (!in_plot(&g->c.bld[i], x, z, maxD, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(&g->c, i);
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.5f) continue;
            best = minf(best, bfoot_boundary_dist(&s->foot, lx, lz, NULL, NULL));
        }
    }
    return best;
}

/* Where an item of radius rad can lie at (x, z): on ground of one kind (want:
 * a G_* type, WANT_FOOT or WANT_ANY) all round it, clear of walls, columns
 * and lamp posts. Gives the ground height. */
static int site_ok(LGen *g, double x, double z, float rad, int want, float *y) {
    Ground c = ground_at(g, x, z);
    if (want == WANT_FOOT ? (c.type != G_SIDEWALK && c.type != G_LOT) : (want >= 0 && c.type != want)) return 0;
    float top = c.y;
    static const double dx[4] = { 1, -1, 0, 0 }, dz[4] = { 0, 0, 1, -1 };
    for (int k = 0; k < 4; ++k) {
        Ground e = ground_at(g, x + dx[k] * rad, z + dz[k] * rad);
        if (e.type != c.type || e.block != c.block || fabsf(e.y - c.y) > 0.01f) return 0;
        top = maxf(top, e.y);
    }
    if (c.type != G_ROAD && blocked(g, x, z, rad)) return 0;
    *y = top;
    return 1;
}

/* ------------------------------------------------------------ geometry --- */

static LV lv(vec3 p, vec3 n, float u, float v, float flex) { LV r; r.p = p; r.n = n; r.u = u; r.v = v; r.flex = flex; return r; }

/* Winding follows the vertex normals, as emit_quad does (src/city/city.c). Degenerate triangles are dropped. */
static void tri(Tris *T, LV a, LV b, LV c) {
    vec3 fn = v3_cross(v3_sub(b.p, a.p), v3_sub(c.p, a.p));
    if (v3_dot(fn, fn) < 1e-14f || T->n + 3 > MAX_LV) return;
    if (v3_dot(fn, v3_add(v3_add(a.n, b.n), c.n)) < 0.0f) { LV t = b; b = c; c = t; }
    T->v[T->n++] = a; T->v[T->n++] = b; T->v[T->n++] = c;
}

/* A sheet lying on the ground (paper, cardboard): w x d, an nx x nz grid
 * lifted a little, crumpled, with one corner (sometimes two) curling up.
 * Drawn both sides, as a curl toward the camera shows its underside, unless
 * it will always lie flat (sides 1). Edges flex in the wind more than the
 * middle. uv runs 0..1 across it. */
static void sheet(Tris *T, int sides, float w, float d, int nx, int nz, float lift, float crumple, float curl, float flexK, uint32_t h) {
    vec3 P[5][5], N[5][5];
    float F[5][5];
    float sx = (h & 1u) ? 1.0f : -1.0f, sz = (h & 2u) ? 1.0f : -1.0f;
    float curl2 = ((h >> 2) & 3u) == 0u ? curl * 0.7f : 0.0f;
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            float fx = (float)i / nx - 0.5f, fz = (float)j / nz - 0.5f;
            float edge = maxf(fabsf(fx), fabsf(fz)) * 2.0f;
            float t1 = maxf(fx * sx + fz * sz, 0.0f), t2 = maxf(-fx * sx - fz * sz, 0.0f);
            float y = lift + crumple * hash_float(hash3i(h, i, j, 0x5EE7)) * (0.4f + 0.6f * edge) + curl * t1 * t1 + curl2 * t2 * t2;
            P[j][i] = v3(fx * w, y, fz * d);
            F[j][i] = flexK * powf(edge, 1.5f);
        }
    for (int j = 0; j <= nz; ++j)
        for (int i = 0; i <= nx; ++i) {
            vec3 dx = v3_sub(P[j][i < nx ? i + 1 : i], P[j][i > 0 ? i - 1 : i]);
            vec3 dz = v3_sub(P[j < nz ? j + 1 : j][i], P[j > 0 ? j - 1 : j][i]);
            N[j][i] = v3_norm(v3_cross(dz, dx));
        }
    for (int side = 0; side < sides; ++side) {
        float sg = side ? -1.0f : 1.0f;
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i) {
                LV q[4];
                int ii[4] = { i, i + 1, i + 1, i }, jj[4] = { j, j, j + 1, j + 1 };
                for (int k = 0; k < 4; ++k) {
                    vec3 p = P[jj[k]][ii[k]];
                    if (side) p.y = maxf(p.y - 0.0012f, 0.0004f);
                    q[k] = lv(p, v3_scale(N[jj[k]][ii[k]], sg), (float)ii[k] / nx, (float)jj[k] / nz, F[jj[k]][ii[k]]);
                }
                tri(T, q[0], q[1], q[2]);
                tri(T, q[0], q[2], q[3]);
            }
    }
}

/* A solid of revolution about local x, centred on the origin: profile points
 * (along, radius) from one end to the other; a zero radius closes an end. v
 * runs 0..1 along it, u round it. Normals are smooth round the axis and flat
 * along each profile segment, so rims and caps stay crisp. */
static void lathe(Tris *T, const float (*pr)[2], int np, int seg) {
    float a0 = pr[0][0], len = pr[np - 1][0] - a0, mid = a0 + len * 0.5f;
    for (int k = 0; k + 1 < np; ++k) {
        float pa = pr[k][0], ra = pr[k][1], pb = pr[k + 1][0], rb = pr[k + 1][1];
        float da = pb - pa, dr = rb - ra, l = sqrtf(da * da + dr * dr);
        if (l < 1e-6f) continue;
        float na = -dr / l, nr = da / l;
        float va = (pa - a0) / len, vb = (pb - a0) / len;
        for (int s = 0; s < seg; ++s) {
            float t0 = TAU * s / seg, t1 = TAU * (s + 1) / seg;
            float c0 = cosf(t0), s0 = sinf(t0), c1 = cosf(t1), s1 = sinf(t1);
            vec3 n0 = v3(na, nr * c0, nr * s0), n1 = v3(na, nr * c1, nr * s1);
            float u0 = (float)s / seg, u1 = (float)(s + 1) / seg;
            LV p00 = lv(v3(pa - mid, ra * c0, ra * s0), n0, u0, va, 0.0f);
            LV p01 = lv(v3(pa - mid, ra * c1, ra * s1), n1, u1, va, 0.0f);
            LV p10 = lv(v3(pb - mid, rb * c0, rb * s0), n0, u0, vb, 0.0f);
            LV p11 = lv(v3(pb - mid, rb * c1, rb * s1), n1, u1, vb, 0.0f);
            tri(T, p00, p01, p11);
            tri(T, p00, p11, p10);
        }
    }
}

/* An ellipsoid (radii rx, ry, rz) resting on the ground, its surface pushed in
 * and out by `bump`, the top pole drawn up by `neck` (a bin bag's tied
 * neck). Faceted reads as crumpled (paper balls, carrier bags); smooth as
 * stretched film. Flex rises toward the top. Faces lying flat on the ground
 * are left out. */
static void blob(Tris *T, float rx, float ry, float rz, int nseg, int nring, float bump, float neck, int facet, float flexK, uint32_t h) {
    vec3 P[7][9], N[7][9];
    for (int r = 0; r <= nring; ++r) {
        float lat = -0.5f * PI_F + PI_F * r / nring;
        for (int s = 0; s < nseg; ++s) {
            float lon = TAU * s / nseg;
            float k = (r == 0 || r == nring) ? 1.0f : 1.0f + bump * (hash_float(hash3i(h, r, s, 0xB10B)) - 0.5f);
            vec3 p = v3(cosf(lat) * cosf(lon) * rx * k, ry + sinf(lat) * ry * k, cosf(lat) * sinf(lon) * rz * k);
            if (r == nring) p.y += neck;
            if (neck > 0.0f && r == nring - 1) { p.x *= 0.55f; p.z *= 0.55f; p.y += neck * 0.5f; }   /* gathered and tied */
            p.y = maxf(p.y, 0.003f);
            P[r][s] = p;
        }
    }
    vec3 cen = v3(0, ry, 0);
    for (int r = 0; r <= nring; ++r)
        for (int s = 0; s < nseg; ++s) {
            vec3 n;
            if (r == 0) n = v3(0, -1, 0);
            else if (r == nring) n = v3(0, 1, 0);
            else {
                vec3 dl = v3_sub(P[r + 1][s], P[r - 1][s]), dn = v3_sub(P[r][(s + 1) % nseg], P[r][(s + nseg - 1) % nseg]);
                n = v3_norm(v3_cross(dl, dn));
                if (v3_dot(n, v3_sub(P[r][s], cen)) < 0.0f) n = v3_scale(n, -1.0f);
            }
            N[r][s] = n;
        }
    float top = 2.0f * ry + neck;
    for (int r = 0; r < nring; ++r)
        for (int s = 0; s < nseg; ++s) {
            int s1 = (s + 1) % nseg;
            int rr[4] = { r, r, r + 1, r + 1 }, ss[4] = { s, s1, s1, s };
            LV q[4];
            for (int k = 0; k < 4; ++k) {
                vec3 p = P[rr[k]][ss[k]];
                q[k] = lv(p, N[rr[k]][ss[k]], (float)(s + (k == 1 || k == 2)) / nseg, (float)rr[k] / nring, flexK * p.y / top);
            }
            for (int t = 0; t < 2; ++t) {
                LV a = q[0], b = q[t ? 2 : 1], c = q[t ? 3 : 2];
                if (a.p.y < 0.0035f && b.p.y < 0.0035f && c.p.y < 0.0035f) continue;
                if (facet) {
                    vec3 fn = v3_norm(v3_cross(v3_sub(b.p, a.p), v3_sub(c.p, a.p)));
                    if (v3_dot(fn, v3_add(v3_add(a.n, b.n), c.n)) < 0.0f) fn = v3_scale(fn, -1.0f);
                    a.n = b.n = c.n = fn;
                }
                tri(T, a, b, c);
            }
        }
}

/* A box on the ground, sx x sy x sz, open underneath; `crush` pushes the top
 * corners down by up to that share of its height. uv runs 0..1 on each face. */
static void box(Tris *T, float sx, float sy, float sz, float crush, uint32_t h) {
    float hx = sx * 0.5f, hz = sz * 0.5f;
    vec3 c[8];
    for (int k = 0; k < 4; ++k) {
        float x = (k == 1 || k == 2) ? hx : -hx, z = (k >= 2) ? hz : -hz;
        c[k] = v3(x, 0.002f, z);
        c[k + 4] = v3(x, sy * (1.0f - crush * hash_float(hash_combine(h, (uint32_t)k))), z);
    }
    static const int F[5][4] = { { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    vec3 mid = v3(0, sy * 0.5f, 0);
    for (int f = 0; f < 5; ++f)
        for (int t = 0; t < 2; ++t) {
            int i0 = 0, i1 = t ? 2 : 1, i2 = t ? 3 : 2;
            vec3 a = c[F[f][i0]], b = c[F[f][i1]], d = c[F[f][i2]];
            vec3 n = v3_norm(v3_cross(v3_sub(b, a), v3_sub(d, a)));
            if (v3_dot(n, v3_sub(v3_scale(v3_add(v3_add(a, b), d), 1.0f / 3.0f), mid)) < 0.0f) n = v3_scale(n, -1.0f);
            tri(T, lv(a, n, UV[i0][0], UV[i0][1], 0.0f), lv(b, n, UV[i1][0], UV[i1][1], 0.0f), lv(d, n, UV[i2][0], UV[i2][1], 0.0f));
        }
}

/* Turns vertices from `first` by `roll` about local x then `pitch` about local
 * z, and sets them down so the lowest touches the ground (plus lift). */
static void settle(Tris *T, int first, float roll, float pitch, float lift) {
    float cr = cosf(roll), sr = sinf(roll), cp = cosf(pitch), sp = sinf(pitch), lo = 1e30f;
    for (int i = first; i < T->n; ++i) {
        vec3 *v[2] = { &T->v[i].p, &T->v[i].n };
        for (int k = 0; k < 2; ++k) {
            float y = v[k]->y * cr - v[k]->z * sr, z = v[k]->y * sr + v[k]->z * cr;
            float x = v[k]->x * cp - y * sp;
            y = v[k]->x * sp + y * cp;
            v[k]->x = x; v[k]->y = y; v[k]->z = z;
        }
        lo = minf(lo, T->v[i].p.y);
    }
    for (int i = first; i < T->n; ++i) T->v[i].p.y += lift - lo;
}

/* Crushes vertices from `first` to ky of their height, dented by up to jitter. */
static void squash(Tris *T, int first, float ky, float jitter, uint32_t h) {
    for (int i = first; i < T->n; ++i) {
        LV *v = &T->v[i];
        /* the same dent for every copy of a corner, wherever it falls in the list */
        uint32_t k = hash3i(h, (int32_t)floorf(v->p.x * 2000.0f), (int32_t)floorf(v->p.y * 2000.0f), (int32_t)floorf(v->p.z * 2000.0f));
        v->p.y *= ky;
        v->p.x += jitter * (hash_float(k) - 0.5f);
        v->p.z += jitter * (hash_float(hash_u32(k)) - 0.5f);
        v->n = v3_norm(v3(v->n.x * ky, v->n.y, v->n.z * ky));
    }
}

/* A contact shadow: a disc (rx, rz) on the ground round (cx, cz). uv is the
 * position across it, -1..1; the shader covers a share of each pixel's samples. */
static void skirt(Tris *T, float cx, float cz, float rx, float rz) {
    vec3 up = v3(0, 1, 0);
    LV c = lv(v3(cx, 0.0012f, cz), up, 0.0f, 0.0f, 0.0f);
    for (int k = 0; k < 6; ++k) {
        float a0 = TAU * k / 6.0f, a1 = TAU * (k + 1) / 6.0f;
        tri(T, c, lv(v3(cx + cosf(a0) * rx, 0.0012f, cz + sinf(a0) * rz), up, cosf(a0), sinf(a0), 0.0f),
               lv(v3(cx + cosf(a1) * rx, 0.0012f, cz + sinf(a1) * rz), up, cosf(a1), sinf(a1), 0.0f));
    }
}

/* ---------------------------------------------------------------- items --- */

typedef struct Item {
    double   x, z;       /* anchor on the ground, world */
    float    y;          /* the ground's height there */
    float    yaw;
    uint32_t seed;
    int      kind;
    uint8_t  flags;      /* LF_* */
} Item;

static void push(Mesh *m, vec3 p, vec3 n, float ao, float u, float v, uint32_t seed, const uint8_t prm[4], uint32_t aux) {
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8192;
        m->v = (Vertex *)realloc(m->v, m->cap * sizeof(Vertex));
        if (!m->v) fatal("out of memory building litter");
    }
    Vertex *o = &m->v[m->count++];
    o->pos[0] = p.x; o->pos[1] = p.y; o->pos[2] = p.z;
    o->nrm[0] = (int8_t)(n.x * 127.0f); o->nrm[1] = (int8_t)(n.y * 127.0f);
    o->nrm[2] = (int8_t)(n.z * 127.0f); o->nrm[3] = (int8_t)(clampf(ao, 0.0f, 1.0f) * 127.0f);
    o->uv[0] = u; o->uv[1] = v;
    o->seed = seed;
    memcpy(o->params, prm, 4);
    o->aux = aux;
    if (p.x < m->mn[0]) m->mn[0] = p.x;
    if (p.y < m->mn[1]) m->mn[1] = p.y;
    if (p.z < m->mn[2]) m->mn[2] = p.z;
    if (p.x > m->mx[0]) m->mx[0] = p.x;
    if (p.y > m->mx[1]) m->mx[1] = p.y;
    if (p.z > m->mx[2]) m->mx[2] = p.z;
}

static int quant(float v, float unit, int lo, int hi) {
    float q = floorf(v / unit + 0.5f);
    return q < (float)lo ? lo : q > (float)hi ? hi : (int)q;
}

/* Writes T's vertices from `first` into the chunk mesh, turned to the item's
 * yaw and standing on its anchor. aoH: height over which the ambient
 * occlusion of the ground fades (0: none). */
static void commit(LGen *g, const Item *it, int first, int kind, float aoH) {
    const Tris *T = g->T;
    float c = cosf(it->yaw), s = sinf(it->yaw);
    float ax = (float)(it->x - g->c.ox), az = (float)(it->z - g->c.oz);
    int fade = quant(FADE[it->kind], 4.0f, 1, 63);
    for (int i = first; i < T->n; ++i) {
        const LV *v = &T->v[i];
        float ox = c * v->p.x - s * v->p.z, oz = s * v->p.x + c * v->p.z, oy = v->p.y;
        uint32_t aux = ((uint32_t)quant(ox, 0.002f, -1023, 1023) & 0x7FFu) | ((uint32_t)quant(oz, 0.002f, -1023, 1023) & 0x7FFu) << 11 |
                       (uint32_t)quant(oy, 0.001f, 0, 1023) << 22;
        uint8_t prm[4] = { MAT_LITTER, (uint8_t)kind, (uint8_t)(clampf(v->flex, 0.0f, 1.0f) * 255.0f + 0.5f), (uint8_t)(fade | it->flags) };
        vec3 n = v3(c * v->n.x - s * v->n.z, v->n.y, s * v->n.x + c * v->n.z);
        float ao = aoH > 0.0f ? 0.35f + 0.65f * smoothstepf(0.0f, aoH, oy) : 1.0f;
        push(g->m, v3(ax + ox, it->y + oy, az + oz), n, ao, v->u, v->v, it->seed, prm, aux);
    }
}

static void build_item(LGen *g, const Item *it) {
    Tris *T = g->T;
    T->n = 0;
    Rng r = rng_make(it->seed);
    uint32_t h = rng_u32(&r), var = (it->seed >> 4) & 3u;   /* var: as the shader reads it */
    float roll = rng_float(&r) * TAU, aoH = 0.0f, shadow = 0.0f;
    int sides = (it->flags & (LF_WATER | LF_RUNOVER)) ? 1 : 2;   /* sheets that always lie flat never show their undersides */
    switch (it->kind) {
    case LK_NEWS: {
        int spread = rng_float(&r) < 0.35f;
        float w = spread ? rng_range(&r, 0.55f, 0.6f) : rng_range(&r, 0.27f, 0.31f), d = rng_range(&r, 0.36f, 0.4f);
        float lift = rng_range(&r, 0.0025f, 0.006f), crumple = rng_range(&r, 0.003f, 0.02f);
        float curl = rng_range(&r, 0.0f, 0.07f);
        curl *= rng_float(&r);
        sheet(T, sides, w, d, spread ? 4 : 3, 2, lift, crumple, curl, 1.0f, h);
        break;
    }
    case LK_FLYER: {
        float w = rng_range(&r, 0.105f, 0.15f), d = rng_range(&r, 0.15f, 0.21f);
        float lift = rng_range(&r, 0.002f, 0.005f), curl = rng_range(&r, 0.0f, 0.03f);
        sheet(T, sides, w, d, 2, 2, lift, 0.004f, curl, 1.0f, h);
        break;
    }
    case LK_SCRAP:
        if (!(var & 1u)) {   /* a till receipt, curling along its length */
            float w = rng_range(&r, 0.07f, 0.09f), d = rng_range(&r, 0.15f, 0.24f), curl = rng_range(&r, 0.01f, 0.03f);
            sheet(T, sides, w, d, 1, 2, 0.002f, 0.004f, curl, 1.0f, h);
        } else {             /* a sweet or burger wrapper */
            float w = rng_range(&r, 0.09f, 0.15f), d = rng_range(&r, 0.08f, 0.12f), crumple = rng_range(&r, 0.01f, 0.02f);
            sheet(T, sides, w, d, 2, 2, 0.002f, crumple, 0.01f, 1.0f, h);
        }
        break;
    case LK_CARDBOARD: {
        float w = rng_range(&r, 0.45f, 0.9f), d = rng_range(&r, 0.35f, 0.65f);
        float lift = rng_range(&r, 0.003f, 0.008f), curl = rng_range(&r, 0.0f, 0.05f);
        sheet(T, sides, w, d, 2, 2, lift, 0.004f, curl, 0.35f, h);
        break;
    }
    case LK_WAD: {
        float R = rng_range(&r, 0.035f, 0.06f);
        blob(T, R, R * 0.85f, R * 0.95f, 5, 3, 0.55f, 0.0f, 1, 0.3f, h);
        aoH = R; shadow = 1.0f;
        break;
    }
    case LK_CAN: {
        float L = rng_float(&r) < 0.3f ? 0.168f : 0.122f, R = 0.033f;
        float pr[5][2] = { { 0, 0 }, { 0, R }, { L - 0.012f, R }, { L, R * 0.82f }, { L, 0 } };   /* the lid's shoulder */
        lathe(T, pr, 5, 6);
        float mode = rng_float(&r);
        if (mode < 0.12f) settle(T, 0, roll, 0.5f * PI_F, 0.001f);      /* standing */
        else if (mode < 0.3f) {                                          /* crushed flat */
            settle(T, 0, roll, 0.0f, 0.0f);
            squash(T, 0, rng_range(&r, 0.35f, 0.55f), 0.008f, h);
            settle(T, 0, 0.0f, 0.0f, 0.001f);
        } else settle(T, 0, roll, 0.0f, 0.001f);
        aoH = 0.05f; shadow = 1.0f;
        break;
    }
    case LK_BOTTLE: {
        int wine = rng_float(&r) < 0.2f;
        float L = wine ? 0.3f : 0.245f, R = wine ? 0.037f : 0.031f;
        float pr[5][2] = { { 0, 0 }, { 0, R }, { L * 0.6f, R }, { L * 0.8f, R * 0.45f }, { L, R * 0.4f } };   /* open at the mouth */
        lathe(T, pr, 5, 6);
        settle(T, 0, roll, 0.0f, 0.001f);
        aoH = 0.05f; shadow = 1.0f;
        break;
    }
    case LK_BUTT: {
        float L = rng_range(&r, 0.02f, 0.032f), R = 0.0042f;
        float pr[2][2] = { { 0, R }, { L, R * 0.95f } };   /* open ends: too small to see into */
        lathe(T, pr, 2, 3);
        settle(T, 0, roll, 0.0f, 0.0005f);
        break;
    }
    case LK_CUP: {
        float L = rng_range(&r, 0.1f, 0.13f), r0 = 0.027f, r1 = 0.044f;
        float pr[5][2] = { { 0, 0 }, { 0, r0 }, { L, r1 }, { L + 0.012f, r1 - 0.004f }, { L + 0.012f, 0 } };   /* the lid */
        lathe(T, pr, 5, 6);
        float mode = rng_float(&r);
        if (mode < 0.1f) settle(T, 0, roll, 0.5f * PI_F, 0.001f);       /* standing */
        else {
            settle(T, 0, roll, atanf((r1 - r0) / L), 0.001f);           /* on its side, the taper flat on the ground */
            if (mode < 0.3f) { squash(T, 0, 0.5f, 0.01f, h); settle(T, 0, 0.0f, 0.0f, 0.001f); }
        }
        aoH = 0.05f; shadow = 1.0f;
        break;
    }
    case LK_BOX:
        if (var == 0u) box(T, 0.09f, 0.1f, 0.075f, rng_range(&r, 0.0f, 0.4f), h);                 /* noodle box */
        else if (var == 1u) { float s = rng_range(&r, 0.32f, 0.4f); box(T, s, 0.038f, s, rng_range(&r, 0.0f, 0.3f), h); }   /* pizza box */
        else if (var == 2u) box(T, 0.14f, 0.07f, 0.14f, rng_range(&r, 0.0f, 0.5f), h);            /* polystyrene clamshell */
        else box(T, 0.088f, 0.022f, 0.055f, rng_range(&r, 0.0f, 0.5f), h);                         /* cigarette packet */
        if (var == 0u && rng_float(&r) < 0.4f) settle(T, 0, 0.5f * PI_F, 0.0f, 0.001f);            /* on its side */
        aoH = 0.04f; shadow = 1.0f;
        break;
    case LK_BAG: {
        float rx = rng_range(&r, 0.11f, 0.17f), ry = rng_range(&r, 0.03f, 0.06f), rz = rng_range(&r, 0.08f, 0.13f);
        blob(T, rx, ry, rz, 6, 3, 0.6f, 0.0f, 1, 1.0f, h);
        aoH = 0.04f; shadow = 0.6f;
        break;
    }
    case LK_BINBAG: {
        float rx = rng_range(&r, 0.25f, 0.31f), ry = rng_range(&r, 0.22f, 0.28f), rz = rng_range(&r, 0.22f, 0.27f);
        float neck = rng_range(&r, 0.03f, 0.08f);
        blob(T, rx, ry, rz, 7, 4, 0.2f, neck, 0, 0.0f, h);
        float tipRoll = rng_range(&r, -0.2f, 0.2f), tipPitch = rng_range(&r, -0.2f, 0.2f);
        settle(T, 0, tipRoll, tipPitch, 0.0f);
        aoH = 0.3f; shadow = 1.0f;
        break;
    }
    }
    commit(g, it, 0, it->kind, aoH);
    if (shadow > 0.0f && T->n) {
        float x0 = 1e30f, x1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (int i = 0; i < T->n; ++i) {
            x0 = minf(x0, T->v[i].p.x); x1 = maxf(x1, T->v[i].p.x);
            z0 = minf(z0, T->v[i].p.z); z1 = maxf(z1, T->v[i].p.z);
        }
        int first = T->n;
        skirt(T, (x0 + x1) * 0.5f, (z0 + z1) * 0.5f, (x1 - x0) * 0.5f * (1.0f + 0.3f * shadow) + 0.02f, (z1 - z0) * 0.5f * (1.0f + 0.3f * shadow) + 0.02f);
        commit(g, it, first, LK_SHADOW, 0.0f);
    }
}

/* Lays an item of `kind` at (x, z) if it lies in this chunk on suitable ground. */
static int drop(LGen *g, int kind, double x, double z, float yaw, uint32_t seed, int want, uint8_t flags) {
    if (!chunk_gen_owns(&g->c, x, z) || g->m->count > BUDGET) return 0;
    Item it;
    if (!site_ok(g, x, z, RADIUS[kind], want, &it.y)) return 0;
    it.x = x; it.z = z; it.yaw = yaw; it.seed = seed; it.kind = kind; it.flags = flags;
    build_item(g, &it);
    return 1;
}

static int pick(Rng *r, const float *w) {
    float sum = 0.0f;
    for (int k = 0; k < LK_COUNT; ++k) sum += w[k];
    float x = rng_float(r) * sum;
    for (int k = 0; k < LK_COUNT; ++k)
        if ((x -= w[k]) < 0.0f) return k;
    return LK_SCRAP;
}

/* An item at (x, z), turned any way: its yaw, then its seed, drawn from r. */
static int toss(LGen *g, int kind, double x, double z, Rng *r, int want, uint8_t flags) {
    float yaw = rng_float(r) * TAU;
    uint32_t seed = rng_u32(r);
    return drop(g, kind, x, z, yaw, seed, want, flags);
}

/* n cigarette butts scattered round (x, z), thickest in the middle. */
static void butts(LGen *g, double x, double z, int n, float spread, uint32_t seed, int want, uint8_t flags) {
    Rng r = rng_make(seed);
    for (int i = 0; i < n; ++i) {
        float a = rng_float(&r) * TAU, fr = sqrtf(rng_float(&r));
        float d = spread * fr * rng_float(&r);
        toss(g, LK_BUTT, x + cosf(a) * d, z + sinf(a) * d, &r, want, flags);
    }
}

/* Bin bags piled in rows from `at`, along `along`, building out along `out`,
 * with flattened boxes and odds and ends round them. */
static void pile(LGen *g, DVec2 at, DVec2 along, DVec2 out, int count, uint32_t seed, int want) {
    Rng r = rng_make(seed);
    int perRow = count > 4 ? (count + 1) / 2 : count;
    for (int k = 0; k < count; ++k) {
        double u = (k % perRow) * 0.5 + rng_range(&r, -0.08f, 0.08f) + (k >= perRow ? 0.25 : 0.0);
        double v = (k / perRow) * 0.45 + rng_range(&r, -0.05f, 0.05f);
        toss(g, LK_BINBAG, at.x + along.x * u + out.x * v, at.z + along.z * u + out.z * v, &r, want, 0);
    }
    int extra = rng_int(&r, 0, 3);
    for (int k = 0; k < extra; ++k) {
        double u = rng_range(&r, -0.4f, perRow * 0.5f + 0.3f), v = rng_range(&r, 0.4f, 1.2f);
        int kind = pick(&r, MIX_BACK);
        toss(g, kind, at.x + along.x * u + out.x * v, at.z + along.z * u + out.z * v, &r, want, 0);
    }
}

/* Stations along an edge of length len, with exponential gaps for `rate` per
 * metre, each with its own seed; the same whichever chunk asks. */
static int stations(uint32_t seed, double len, float rate, float *u, uint32_t *s, int max) {
    if (rate <= 0.0f) return 0;
    Rng st = rng_make(seed);
    int n = 0;
    double x = 0.0;
    for (;;) {
        x += -log(1.0 - 0.999 * rng_float(&st)) / rate;
        if (x >= len || n >= max) break;
        u[n] = (float)x;
        s[n++] = rng_u32(&st);
    }
    return n;
}

/* ------------------------------------------------------------- placement --- */

/* Along every kerb: the gutter outside it (in the run-off), and the sidewalk
 * inside it, at the kerb line and the building line. */
static void kerbs(LGen *g) {
    enum { MAXS = 512 };
    float u[MAXS];
    uint32_t ss[MAXS];
    for (int bi = 0; bi < g->c.pa->nblocks; ++bi) {
        const Block *bk = &g->c.pa->blocks[bi];
        if (!chunk_gen_near(&g->c, bk->x0, bk->z0, bk->x1, bk->z1, 1.0)) continue;
        const Poly *c = &bk->curb;
        float sq = squalor(g, bi), a = amount(sq);
        DVec2 cen = poly_centroid(c);
        for (int i = 0; i < c->n; ++i) {
            if (!c->e[i].id) continue;
            DVec2 A = c->v[i], B = c->v[(i + 1) % c->n];
            double dx = B.x - A.x, dz = B.z - A.z, len = sqrt(dx * dx + dz * dz);
            float walk = c->e[i].walk;
            if (len < 2.0 || !chunk_gen_near(&g->c, fmin(A.x, B.x), fmin(A.z, B.z), fmax(A.x, B.x), fmax(A.z, B.z), walk + 1.0)) continue;
            DVec2 t = { dx / len, dz / len }, out = { -t.z, t.x };
            if (out.x * (A.x - cen.x) + out.z * (A.z - cen.z) < 0.0) { out.x = -out.x; out.z = -out.z; }
            float along = atan2f((float)t.z, (float)t.x);
            uint32_t es = hash_combine(bk->seed ^ 0x6077E2u, (uint32_t)i);
/* The point U along the kerb from A and OFF out from it, as (X, Z); U and OFF are evaluated once each, U first. */
#define AT(U, OFF, X, Z) double X, Z; { double u_ = (U), off_ = (OFF); X = A.x + t.x * u_ + out.x * off_; Z = A.z + t.z * u_ + out.z * off_; }

            /* gutter: rubbish washed against the kerb; cans and bottles roll to lie along it */
            int n = stations(hash_combine(es, 1u), len, 0.8f * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                int kind = pick(&r, MIX_GUTTER);
                AT(u[k], RADIUS[kind] + 0.03f + rng_range(&r, 0.0f, 0.22f), x, z);
                int rolls = kind == LK_CAN || kind == LK_BOTTLE || kind == LK_CUP;
                float yaw = rolls ? along + rng_range(&r, -0.35f, 0.35f) : rng_float(&r) * TAU;
                drop(g, kind, x, z, yaw, rng_u32(&r), G_ROAD, LF_WATER);
            }
            n = stations(hash_combine(es, 2u), len, 0.5f * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                AT(u[k], rng_range(&r, 0.03f, 0.3f), x, z);
                toss(g, LK_BUTT, x, z, &r, G_ROAD, LF_WATER);
            }
            /* now and then the run-off has dammed a clump of it together */
            n = stations(hash_combine(es, 3u), len, 0.12f * a * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                AT(u[k], RADIUS[LK_NEWS] + 0.03f, nx, nz);   /* a sodden newspaper under it all */
                float yaw = along + rng_range(&r, -0.4f, 0.4f);
                drop(g, LK_NEWS, nx, nz, yaw, rng_u32(&r), G_ROAD, LF_WATER);
                int m = rng_int(&r, 4, 9);
                for (int j = 0; j < m; ++j) {
                    int kind = pick(&r, MIX_GUTTER);
                    AT(u[k] + rng_range(&r, -0.5f, 0.5f), RADIUS[kind] + 0.03f + rng_range(&r, 0.0f, 0.15f), x, z);
                    toss(g, kind, x, z, &r, G_ROAD, LF_WATER);
                }
                AT(u[k], 0.12f, x, z);
                int nb = rng_int(&r, 2, 6);
                butts(g, x, z, nb, 0.5f, rng_u32(&r), G_ROAD, LF_WATER);
            }

            /* sidewalk: along the kerb line, along the building line, now and then in between */
            n = stations(hash_combine(es, 4u), len, 0.7f * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                int kind = pick(&r, MIX_WALK);
                float band = rng_float(&r), rad = RADIUS[kind], d;
                if (band < 0.4f) d = rng_range(&r, 0.35f, 1.0f);
                else if (band < 0.75f) d = walk - rng_range(&r, 0.15f, 0.8f);
                else d = rng_range(&r, 0.3f, maxf(walk - 0.2f, 0.4f));
                d = maxf(d, rad + 0.05f);
                AT(u[k], -d, x, z);
                toss(g, kind, x, z, &r, G_SIDEWALK, 0);
            }
            n = stations(hash_combine(es, 5u), len, 0.3f * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                float d = rng_float(&r) < 0.6f ? rng_range(&r, 0.05f, 0.9f) : walk - rng_range(&r, 0.05f, 0.6f);
                AT(u[k], -d, x, z);
                toss(g, LK_BUTT, x, z, &r, G_SIDEWALK, 0);
            }
            /* and in squalid streets the wind drifts it into heaps against the walls */
            n = stations(hash_combine(es, 7u), len, 0.2f * a * a, u, ss, MAXS);
            for (int k = 0; k < n; ++k) {
                Rng r = rng_make(ss[k]);
                int m = rng_int(&r, 2, 5);
                for (int j = 0; j < m; ++j) {
                    int kind = pick(&r, MIX_WALK);
                    AT(u[k] + rng_range(&r, -0.6f, 0.6f), -(walk - RADIUS[kind] - rng_range(&r, 0.05f, 0.35f)), x, z);
                    toss(g, kind, x, z, &r, G_SIDEWALK, 0);
                }
            }
            /* poorer streets put their bin bags out on the kerb */
            if (sq > 0.45f && bk->zone != ZONE_PARK) {
                n = stations(hash_combine(es, 6u), len, 0.06f * (sq - 0.45f), u, ss, MAXS);
                for (int k = 0; k < n; ++k) {
                    Rng r = rng_make(ss[k]);
                    AT(u[k], -rng_range(&r, 0.45f, 0.7f), x, z);
                    DVec2 at = { x, z }, in = { -out.x, -out.z };
                    int count = rng_int(&r, 2, 2 + (int)(sq * 5.0f));
                    pile(g, at, t, in, count, rng_u32(&r), G_SIDEWALK);
                }
            }
#undef AT
        }
    }
}

/* Open ground on a 2.5 m grid: alleys and courtyards (most at the foot of the
 * walls), the parks, and now and then something run flat on the carriageway.
 * Sidewalks are left to the kerbs. */
static void scatter(LGen *g) {
    const int N = (int)(CHUNK_SIZE / CELL + 0.5);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            int wx = g->c.cx * N + i, wz = g->c.cz * N + j;
            Rng r = rng_make(hash2i(WORLD_SEED ^ 0x11773u, wx, wz));
            double x = (wx + rng_float(&r)) * CELL, z = (wz + rng_float(&r)) * CELL;
            float roll = rng_float(&r);
            Ground gr = ground_at(g, x, z);
            const float *mix;
            uint8_t flags = 0;
            if (gr.type == G_LOT) {
                float a = amount(squalor(g, gr.block));
                if (roll >= 0.3f * a * 2.2f) continue;
                if (roll >= 0.3f * a * (wall_dist(g, x, z, 1.5f) < 1.5f ? 2.2f : 0.6f)) continue;
                if (rng_float(&r) < 0.15f) {
                    int nb = rng_int(&r, 2, 7);
                    butts(g, x, z, nb, 0.6f, rng_u32(&r), G_LOT, 0);
                    continue;
                }
                mix = MIX_LOT;
            } else if (gr.type == G_PARK) {
                if (roll >= 0.05f * amount(squalor(g, gr.block))) continue;
                mix = MIX_PARK;
            } else if (gr.type == G_ROAD) {
                if (roll >= 0.03f) continue;   /* the most it could be, before the cost of asking how poor the area is */
                if (roll >= 0.03f * amount(city_squalor(-1, x, z))) continue;
                mix = MIX_ROAD;
                flags = LF_RUNOVER;
            } else continue;
            int kind = pick(&r, mix);
            toss(g, kind, x, z, &r, gr.type, flags);
        }
}

/* Doorways: butts where office workers stand and smoke to one side of the
 * entrance, odds and ends outside shops, bin bags piled by service doors and
 * loading bays. */
static void doorways(LGen *g) {
    for (int i = 0; i < g->c.nb; ++i) {
        const Building *b = &g->c.bld[i];
        double rr = 0.5 * sqrt((double)(b->foot.x1 - b->foot.x0) * (b->foot.x1 - b->foot.x0) +
                               (double)(b->foot.z1 - b->foot.z0) * (b->foot.z1 - b->foot.z0)) + 5.0;
        if (!chunk_gen_near(&g->c, b->ox - rr, b->oz - rr, b->ox + rr, b->oz + rr, 0.0)) continue;
        const BGraph *bg = chunk_gen_graph(&g->c, i);
        float sq = squalor(g, g->c.bblk[i]);
        int office = bg->groundMode == GF_LOBBY || bg->groundMode == GF_DOUBLE;
        for (int di = 0; di < bg->ndoor; ++di) {
            const BDoor *d = &bg->door[di];
            const BLoop *l = &bg->sec[d->section].foot.outer;
            int k = d->face, k1 = (k + 1) % l->n;
            float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], el = sqrtf(ex * ex + ez * ez);
            if (el < 1e-4f) continue;
            ex /= el; ez /= el;
            /* the door's foot, and the directions along the wall and out of it (outward is (-dz, dx)) */
            double lx = l->x[k] + ex * d->u, lz = l->z[k] + ez * d->u;
            DVec2 at = { b->ox + b->c * lx - b->s * lz, b->oz + b->s * lx + b->c * lz };
            DVec2 along = { b->c * ex - b->s * ez, b->s * ex + b->c * ez }, out = { -along.z, along.x };
            if (!chunk_gen_near(&g->c, at.x, at.z, at.x, at.z, 4.0)) continue;
            Rng r = rng_make(hash3i(bg->seed ^ 0xD00Bu, d->section, d->face, di));
            float side = rng_float(&r) < 0.5f ? -1.0f : 1.0f;
            if (d->type == DOOR_PRIMARY && office) {
                if (rng_float(&r) >= 0.75f) continue;
                float s = d->width * 0.5f + rng_range(&r, 0.8f, 2.2f), o = rng_range(&r, 0.6f, 1.6f);
                double x = at.x + along.x * s * side + out.x * o, z = at.z + along.z * s * side + out.z * o;
                int nb = 6 + (int)(26.0f * rng_float(&r) * (0.35f + 0.65f * sq));
                butts(g, x, z, nb, 0.75f, rng_u32(&r), WANT_FOOT, 0);
                if (rng_float(&r) < 0.4f) {
                    double cx = x + rng_range(&r, -0.6f, 0.6f), cz = z + rng_range(&r, -0.6f, 0.6f);
                    toss(g, LK_CUP, cx, cz, &r, WANT_FOOT, 0);
                }
            } else if (d->type == DOOR_PRIMARY) {
                if (rng_float(&r) >= 0.25f + 0.5f * sq) continue;
                int n = rng_int(&r, 1, 3);
                for (int k2 = 0; k2 < n; ++k2) {
                    float s = rng_range(&r, -2.5f, 2.5f), o = rng_range(&r, 0.4f, 2.0f);
                    int kind = pick(&r, MIX_SHOP);
                    toss(g, kind, at.x + along.x * s + out.x * o, at.z + along.z * s + out.z * o, &r, WANT_FOOT, 0);
                }
                int nb = (int)(2.0f + 6.0f * sq * rng_float(&r));
                butts(g, at.x + out.x * 1.0, at.z + out.z * 1.0, nb, 1.5f, rng_u32(&r), WANT_FOOT, 0);
            } else {
                if (rng_float(&r) >= 0.3f + 0.65f * sq) continue;
                float s = d->width * 0.5f + 0.45f;
                DVec2 start = { at.x + along.x * s * side + out.x * 0.36, at.z + along.z * s * side + out.z * 0.36 };
                DVec2 dir = { along.x * side, along.z * side };
                int count = 2 + (int)(rng_float(&r) * (1.0f + 6.0f * sq));
                pile(g, start, dir, out, count, rng_u32(&r), WANT_FOOT);
            }
        }
    }
}

/* Round the foot of lamp posts: a can, a cup, butts; in poor streets a bin bag left against it. */
static void lampposts(LGen *g) {
    for (int i = 0; i < g->c.nl; ++i) {
        const Lamp *L = &g->c.lamp[i];
        Ground gr = ground_at(g, L->x, L->z);
        if (gr.block < 0) continue;
        float sq = squalor(g, gr.block);
        Rng r = rng_make(hash2i(WORLD_SEED ^ 0x1A3B17u, (int32_t)floor(L->x * 4.0), (int32_t)floor(L->z * 4.0)));
        if (rng_float(&r) >= 0.15f + 0.5f * sq) continue;
        int n = rng_int(&r, 1, 2 + (int)(sq * 2.0f));
        for (int k = 0; k < n; ++k) {
            int kind = pick(&r, MIX_LAMP);
            float a = rng_float(&r) * TAU, d = 0.24f + RADIUS[kind] + rng_range(&r, 0.0f, 0.4f);
            toss(g, kind, L->x + cosf(a) * d, L->z + sinf(a) * d, &r, G_SIDEWALK, 0);
        }
        int nb = (int)(2.0f + 8.0f * sq * rng_float(&r));
        butts(g, L->x, L->z, nb, 0.8f, rng_u32(&r), G_SIDEWALK, 0);
        if (sq > 0.6f && rng_float(&r) < 0.25f) {
            float a = rng_float(&r) * TAU;
            toss(g, LK_BINBAG, L->x + cosf(a) * 0.56, L->z + sinf(a) * 0.56, &r, G_SIDEWALK, 0);
        }
    }
}

void city_build_litter(int cx, int cz, Mesh *m) {
    LGen g;
    m->count = 0;
    m->xf = 0;
    m->mn[0] = m->mn[1] = m->mn[2] = 1e30f;
    m->mx[0] = m->mx[1] = m->mx[2] = -1e30f;
    free(m->lights);
    m->lights = NULL;
    m->nlights = 0;
    lgen_init(&g, cx, cz, m);
    kerbs(&g);
    scatter(&g);
    doorways(&g);
    lampposts(&g);
    lgen_free(&g);
}
