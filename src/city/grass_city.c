#include "city_internal.h"
#include "grass_rules.h"

/* ===========================================================================
 * The city's grass for the blades (GR6 of the grass: src/grass,
 * src/render/grass_gpu.c, shaders/grass/prepare_city.comp).
 *
 * city_build_mesh hands over the vertices its streets drew (the roads and the
 * city's edge, the channels, the blocks: draped on the ground, so these are
 * the very triangles the chunk draws). Each triangle of a green lot whose
 * params say blades grow there (city.h city_grass_param: a park, a field,
 * the edge verge, not a skirt's side) has what stands on it cut out (the
 * footprints of a farmstead's buildings, GRASS_CLEAR round them) and is cut
 * along the grass's patches, so that the GPU grows each patch's roots on its
 * pieces from the same world-fixed root cells it grows the land's from: no
 * seams where triangles meet, nothing crawls. Pure: a function of the mesh
 * and the chunk's buildings, so the chunk is still one of its coordinates.
 * ========================================================================= */

#define GRASS_CLEAR 0.3      /* m kept clear round a building's site */
#define GP_MAX      24       /* corners of a convex piece */
#define GP_PIECES   64       /* pieces of one triangle round the buildings on it */
#define GP_AREA     1e-4     /* m^2: smaller pieces are left out */

typedef struct Piece { int n; double x[GP_MAX], z[GP_MAX]; } Piece;

static double piece_area2(const Piece *p) {
    double s = 0.0;
    for (int i = 0; i < p->n; ++i) {
        int j = (i + 1) % p->n;
        s += p->x[i] * p->z[j] - p->x[j] * p->z[i];
    }
    return s;
}

/* p's part where a x + b z <= c (Sutherland-Hodgman). */
static void piece_clip(const Piece *p, double a, double b, double c, Piece *o) {
    o->n = 0;
    for (int i = 0; i < p->n; ++i) {
        int j = (i + 1) % p->n;
        double di = a * p->x[i] + b * p->z[i] - c, dj = a * p->x[j] + b * p->z[j] - c;
        if (di <= 0.0 && o->n < GP_MAX) { o->x[o->n] = p->x[i]; o->z[o->n] = p->z[i]; o->n++; }
        if (((di < 0.0 && dj > 0.0) || (di > 0.0 && dj < 0.0)) && o->n < GP_MAX) {
            double t = di / (di - dj);
            o->x[o->n] = p->x[i] + (p->x[j] - p->x[i]) * t;
            o->z[o->n] = p->z[i] + (p->z[j] - p->z[i]) * t;
            o->n++;
        }
    }
    if (o->n < 3) o->n = 0;
}

/* The pieces of each of in[0 .. n) outside convex quad q, into out (at most
 * max); returns how many. Each piece stays convex: what lies beyond each of
 * q's sides in turn, of what is left inside the sides before it. */
static int piece_minus_quad(const Piece *in, int n, const double q[4][2], Piece *out, int max) {
    double s = 0.0;
    for (int i = 0; i < 4; ++i) s += q[i][0] * q[(i + 1) & 3][1] - q[(i + 1) & 3][0] * q[i][1];
    double qx0 = fmin(fmin(q[0][0], q[1][0]), fmin(q[2][0], q[3][0])), qx1 = fmax(fmax(q[0][0], q[1][0]), fmax(q[2][0], q[3][0]));
    double qz0 = fmin(fmin(q[0][1], q[1][1]), fmin(q[2][1], q[3][1])), qz1 = fmax(fmax(q[0][1], q[1][1]), fmax(q[2][1], q[3][1]));
    int k = 0;
    for (int p = 0; p < n; ++p) {
        double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
        for (int i = 0; i < in[p].n; ++i) {
            x0 = fmin(x0, in[p].x[i]); x1 = fmax(x1, in[p].x[i]);
            z0 = fmin(z0, in[p].z[i]); z1 = fmax(z1, in[p].z[i]);
        }
        if (x1 <= qx0 || x0 >= qx1 || z1 <= qz0 || z0 >= qz1) {   /* clear of it */
            if (k < max) out[k++] = in[p];
            continue;
        }
        Piece rem = in[p], tmp, part;
        for (int e = 0; e < 4 && rem.n; ++e) {
            double dx = q[(e + 1) & 3][0] - q[e][0], dz = q[(e + 1) & 3][1] - q[e][1];
            double a = s > 0.0 ? dz : -dz, b = s > 0.0 ? -dx : dx, c = a * q[e][0] + b * q[e][1];   /* a x + b z > c: outside */
            piece_clip(&rem, -a, -b, -c, &part);
            if (part.n && fabs(piece_area2(&part)) > 2.0 * GP_AREA && k < max) out[k++] = part;
            piece_clip(&rem, a, b, c, &tmp);
            rem = tmp;
        }
    }
    return k;
}

typedef struct Collect { CityGrassTri *t; uint32_t n, cap; } Collect;

static void grass_push_tri(Collect *c, int32_t px, int32_t pz, const float v[3][3], float maintained) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->t = (CityGrassTri *)realloc(c->t, c->cap * sizeof(CityGrassTri));
        if (!c->t) fatal("out of memory collecting the city's grass");
    }
    CityGrassTri *t = &c->t[c->n++];
    t->px = px; t->pz = pz;
    memcpy(t->v, v, sizeof(t->v));
    t->maintained = maintained;
}

/* A piece (chunk-local) of a triangle whose plane is y = y0 + gx (x - x0) +
 * gz (z - z0), cut along the patches and fanned into them. */
static void piece_emit(Collect *c, const ChunkGen *g, const Piece *p, double x0, double z0, double y0, double gx, double gz, float maintained) {
    const double S = GRASS_PATCH_M;
    double bx0 = 1e300, bx1 = -1e300, bz0 = 1e300, bz1 = -1e300;
    for (int i = 0; i < p->n; ++i) {
        bx0 = fmin(bx0, p->x[i]); bx1 = fmax(bx1, p->x[i]);
        bz0 = fmin(bz0, p->z[i]); bz1 = fmax(bz1, p->z[i]);
    }
    int32_t px0 = (int32_t)floor((g->ox + bx0) / S), px1 = (int32_t)floor((g->ox + bx1) / S);
    int32_t pz0 = (int32_t)floor((g->oz + bz0) / S), pz1 = (int32_t)floor((g->oz + bz1) / S);
    for (int32_t pz = pz0; pz <= pz1; ++pz)
        for (int32_t px = px0; px <= px1; ++px) {
            double X0 = px * S - g->ox, Z0 = pz * S - g->oz;   /* the patch's corner, chunk-local */
            Piece a, b;
            piece_clip(p, -1.0, 0.0, -X0, &a);
            piece_clip(&a, 1.0, 0.0, X0 + S, &b);
            piece_clip(&b, 0.0, -1.0, -Z0, &a);
            piece_clip(&a, 0.0, 1.0, Z0 + S, &b);
            if (!b.n || fabs(piece_area2(&b)) < 2.0 * GP_AREA) continue;
            for (int i = 1; i + 1 < b.n; ++i) {
                int k3[3] = { 0, i, i + 1 };
                float v[3][3];
                double tx[3], tz[3];
                for (int q = 0; q < 3; ++q) {
                    double x = b.x[k3[q]], z = b.z[k3[q]];
                    tx[q] = x; tz[q] = z;
                    v[q][0] = (float)(x - X0);
                    v[q][1] = (float)(y0 + gx * (x - x0) + gz * (z - z0));
                    v[q][2] = (float)(z - Z0);
                }
                double ar = (tx[1] - tx[0]) * (tz[2] - tz[0]) - (tx[2] - tx[0]) * (tz[1] - tz[0]);
                if (fabs(ar) < 2.0 * GP_AREA) continue;
                grass_push_tri(c, px, pz, (const float (*)[3])v, maintained);
            }
        }
}

/* Chunks are built on several threads at once: the sort's keys travel with it. */
typedef struct SortKey { int32_t pz, px; uint32_t i; } SortKey;
static int cmp_patch(const void *pa, const void *pb) {
    const SortKey *a = (const SortKey *)pa, *b = (const SortKey *)pb;
    if (a->pz != b->pz) return a->pz < b->pz ? -1 : 1;
    if (a->px != b->px) return a->px < b->px ? -1 : 1;
    return a->i < b->i ? -1 : a->i > b->i ? 1 : 0;   /* in the order they were cut */
}

void city_grass_collect(Mesh *m, const ChunkGen *g, uint32_t first, uint32_t end) {
    Collect c = { NULL, 0, 0 };
    Piece *base = (Piece *)malloc(sizeof(Piece) * GP_PIECES * 2);
    if (!base) fatal("out of memory collecting the city's grass");
    Piece *pa = base, *pb = base + GP_PIECES;
    for (uint32_t t = first; t + 2 < end; t += 3) {
        const Vertex *v = &m->v[t];
        if (v[0].params[0] != MAT_LOT || v[0].params[1] != 1 || v[0].params[3] == CITY_GRASS_NONE) continue;
        if (v[0].nrm[1] < 32) continue;   /* a skirt's side, not ground (ny under 0.25) */
        double x0 = v[0].pos[0], z0 = v[0].pos[2], y0 = v[0].pos[1];
        double ux = v[1].pos[0] - x0, uz = v[1].pos[2] - z0, uy = v[1].pos[1] - y0;
        double wx = v[2].pos[0] - x0, wz = v[2].pos[2] - z0, wy = v[2].pos[1] - y0;
        double det = ux * wz - wx * uz;
        if (fabs(det) < 2.0 * GP_AREA) continue;
        /* its plane: y - y0 = gx (x - x0) + gz (z - z0) */
        double gx = (uy * wz - wy * uz) / det, gz = (ux * wy - wx * uy) / det;
        int n = 1;
        pa[0].n = 3;
        for (int q = 0; q < 3; ++q) { pa[0].x[q] = v[q].pos[0]; pa[0].z[q] = v[q].pos[2]; }
        double tx0 = fmin(fmin(v[0].pos[0], v[1].pos[0]), v[2].pos[0]) + g->ox, tx1 = fmax(fmax(v[0].pos[0], v[1].pos[0]), v[2].pos[0]) + g->ox;
        double tz0 = fmin(fmin(v[0].pos[2], v[1].pos[2]), v[2].pos[2]) + g->oz, tz1 = fmax(fmax(v[0].pos[2], v[1].pos[2]), v[2].pos[2]) + g->oz;
        for (int bi = 0; bi < g->nb && n; ++bi) {
            const Building *b = &g->bld[bi];
            double m0 = b->foot.x0 - GRASS_CLEAR, m1 = b->foot.x1 + GRASS_CLEAR, n0 = b->foot.z0 - GRASS_CLEAR, n1 = b->foot.z1 + GRASS_CLEAR;
            DVec2 corner[4] = { to_world(b, m0, n0), to_world(b, m1, n0), to_world(b, m1, n1), to_world(b, m0, n1) };
            double q[4][2], bx0 = 1e300, bx1 = -1e300, bz0 = 1e300, bz1 = -1e300;
            for (int k = 0; k < 4; ++k) {
                bx0 = fmin(bx0, corner[k].x); bx1 = fmax(bx1, corner[k].x);
                bz0 = fmin(bz0, corner[k].z); bz1 = fmax(bz1, corner[k].z);
                q[k][0] = corner[k].x - g->ox; q[k][1] = corner[k].z - g->oz;
            }
            if (bx1 <= tx0 || bx0 >= tx1 || bz1 <= tz0 || bz0 >= tz1) continue;
            n = piece_minus_quad(pa, n, (const double (*)[2])q, pb, GP_PIECES);
            Piece *sw = pa; pa = pb; pb = sw;
        }
        float kept = city_grass_maintained(v[0].params[3]);
        for (int k = 0; k < n; ++k) piece_emit(&c, g, &pa[k], x0, z0, y0, gx, gz, kept);
    }
    free(base);
    /* by patch, so the renderer hands the GPU each patch's run in one go */
    if (c.n) {
        SortKey *order = (SortKey *)malloc(sizeof(SortKey) * c.n);
        CityGrassTri *sorted = (CityGrassTri *)malloc(sizeof(CityGrassTri) * c.n);
        if (!order || !sorted) fatal("out of memory collecting the city's grass");
        for (uint32_t i = 0; i < c.n; ++i) { order[i].pz = c.t[i].pz; order[i].px = c.t[i].px; order[i].i = i; }
        qsort(order, c.n, sizeof(SortKey), cmp_patch);
        for (uint32_t i = 0; i < c.n; ++i) sorted[i] = c.t[order[i].i];
        free(order);
        free(c.t);
        c.t = sorted;
    }
    free(m->grass);
    m->grass = c.t;
    m->ngrass = c.n;
}
