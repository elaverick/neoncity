#include <stdlib.h>
#include <string.h>
#include "terrain_internal.h"

/* The light under the rock (terrain.h, "Caves: light"): how much of the sky
 * a point of a cave sees, from how far it lies along the passages from the
 * nearest mouth.
 *
 * A system's passages make a graph: its nodes are the passages' ends, the
 * places where an end meets another passage along it, and each mouth's
 * place on its own passage (the foot of the mouth on its axis); its edges
 * join the nodes along each passage, in order, as long as the stretch of
 * axis between them. From each mouth the path to every node (Dijkstra). A
 * point's path from a mouth is then, over the pieces near it: along a
 * passage, the path at the foot of the point on its axis (between the nodes
 * either side of it); in a chamber, the path to its middle (the least, from
 * any passage's axis) and straight on from there; each piece's less how much
 * nearer its wall is than the nearest's, TERRAIN_CAVE_SKY_ACROSS times over,
 * so that a wall takes its own passage's light and not a neighbour's through
 * the rock between them. The pieces counted are those whose walls lie within
 * TERRAIN_CAVE_NEAR_M of the nearest's: every list that holds the pieces
 * near the point holds them (terrain_cave_near_box), so a brick's mesh and
 * its neighbour's give a vertex they share the same light, and so does the
 * whole cave's list (terrain_cave_sky). */

#define NODES   (4 * TERRAIN_CAVE_SEGS + TERRAIN_CAVE_MOUTHS)
#define ENTRIES (8 * TERRAIN_CAVE_SEGS)
#define ON_AXIS 0.5   /* a node lies on a passage within this of its axis (m) */

struct TerrainCaveLight {
    uint32_t id, sum;                       /* the cave it was made for */
    int      nnode;
    double   p[NODES][3];
    double   D[TERRAIN_CAVE_MOUTHS][NODES]; /* the path from each mouth (m; 1e30 none) */
    int      first[TERRAIN_CAVE_SEGS + 1];  /* each passage's nodes along it: entries first[s] .. first[s + 1] - 1 */
    double   t[ENTRIES];                    /* in order along the passage (0 .. 1) */
    int      node[ENTRIES];
    double   roomD[TERRAIN_CAVE_MOUTHS][TERRAIN_CAVE_ROOMS];   /* to each chamber's middle */
    double   len[TERRAIN_CAVE_MOUTHS];      /* the light's length from each mouth: its passage's radius there, TERRAIN_CAVE_SKY_RADII times */
};

static uint32_t cave_sum(const TerrainCave *c) {
    uint32_t h = 2166136261u;
    const unsigned char *b = (const unsigned char *)c->seg;
    for (size_t k = 0; k < sizeof(TerrainCaveSeg) * (size_t)c->nseg; ++k) h = (h ^ b[k]) * 16777619u;
    b = (const unsigned char *)c->mouth;
    for (size_t k = 0; k < sizeof(TerrainCaveMouth) * (size_t)c->nmouth; ++k) h = (h ^ b[k]) * 16777619u;
    return h ^ (uint32_t)c->nseg ^ (uint32_t)c->nroom << 8 ^ (uint32_t)c->nmouth << 16;
}

static int node_at(TerrainCaveLight *L, const double p[3]) {
    for (int k = 0; k < L->nnode; ++k)
        if (fabs(L->p[k][0] - p[0]) + fabs(L->p[k][1] - p[1]) + fabs(L->p[k][2] - p[2]) < 1e-6) return k;
    if (L->nnode >= NODES) return -1;
    memcpy(L->p[L->nnode], p, sizeof L->p[0]);
    return L->nnode++;
}

/* Where P's foot lies along passage s (0 .. 1, clamped), and its distance from the axis there. */
static double seg_foot(const TerrainCaveSeg *s, const double P[3], double *off) {
    double ab[3] = { s->b[0] - s->a[0], s->b[1] - s->a[1], s->b[2] - s->a[2] }, ap[3] = { P[0] - s->a[0], P[1] - s->a[1], P[2] - s->a[2] };
    double l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], u = l2 > 0.0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0;
    u = u < 0.0 ? 0.0 : u > 1.0 ? 1.0 : u;
    double e[3] = { ap[0] - u * ab[0], ap[1] - u * ab[1], ap[2] - u * ab[2] };
    *off = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    return u;
}

static double seg_len(const TerrainCaveSeg *s) {
    double d[3] = { s->b[0] - s->a[0], s->b[1] - s->a[1], s->b[2] - s->a[2] };
    return sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

/* The path from mouth m at foot u along passage s. */
static double seg_path(const TerrainCaveLight *L, const TerrainCaveSeg *s, int si, int m, double u) {
    double len = seg_len(s), best = 1e30;
    for (int e = L->first[si]; e + 1 < L->first[si + 1]; ++e) {
        if (u < L->t[e] || u > L->t[e + 1]) continue;
        best = fmin(best, fmin(L->D[m][L->node[e]] + (u - L->t[e]) * len, L->D[m][L->node[e + 1]] + (L->t[e + 1] - u) * len));
    }
    return best;
}

static void light_make(const TerrainCave *c, TerrainCaveLight *L) {
    L->nnode = 0;
    for (int s = 0; s < c->nseg; ++s) { node_at(L, c->seg[s].a); node_at(L, c->seg[s].b); }
    /* each mouth's foot on the passage whose wall is nearest it */
    int mnode[TERRAIN_CAVE_MOUTHS];
    for (int m = 0; m < c->nmouth; ++m) {
        int best = 0;
        double bw = 1e30, bu = 0.0;
        for (int s = 0; s < c->nseg; ++s) {
            double off, u = seg_foot(&c->seg[s], c->mouth[m].p, &off), w = off - (c->seg[s].ra + (c->seg[s].rb - c->seg[s].ra) * u);
            if (w < bw) { bw = w; best = s; bu = u; }
        }
        const TerrainCaveSeg *s = &c->seg[best];
        double q[3];
        for (int a = 0; a < 3; ++a) q[a] = s->a[a] + (s->b[a] - s->a[a]) * bu;
        mnode[m] = node_at(L, q);
        L->len[m] = TERRAIN_CAVE_SKY_RADII * (s->ra + (s->rb - s->ra) * bu);
    }
    /* the nodes along each passage, in order */
    int ne = 0;
    for (int s = 0; s < c->nseg; ++s) {
        L->first[s] = ne;
        for (int k = 0; k < L->nnode && ne < ENTRIES; ++k) {
            double off, u = seg_foot(&c->seg[s], L->p[k], &off);
            if (off > ON_AXIS) continue;
            int at = ne++;
            while (at > L->first[s] && L->t[at - 1] > u) { L->t[at] = L->t[at - 1]; L->node[at] = L->node[at - 1]; at--; }
            L->t[at] = u; L->node[at] = k;
        }
    }
    L->first[c->nseg] = ne;
    /* the paths from each mouth: Dijkstra over the stretches between nodes */
    for (int m = 0; m < c->nmouth; ++m) {
        static _Thread_local unsigned char done[NODES];
        double *D = L->D[m];
        for (int k = 0; k < L->nnode; ++k) { D[k] = 1e30; done[k] = 0; }
        if (mnode[m] < 0) continue;
        D[mnode[m]] = 0.0;
        for (;;) {
            int at = -1;
            for (int k = 0; k < L->nnode; ++k)
                if (!done[k] && D[k] < 1e29 && (at < 0 || D[k] < D[at])) at = k;
            if (at < 0) break;
            done[at] = 1;
            for (int s = 0; s < c->nseg; ++s) {
                double len = seg_len(&c->seg[s]);
                for (int e = L->first[s]; e < L->first[s + 1]; ++e) {
                    if (L->node[e] != at) continue;
                    if (e > L->first[s]) { int o = L->node[e - 1]; D[o] = fmin(D[o], D[at] + (L->t[e] - L->t[e - 1]) * len); }
                    if (e + 1 < L->first[s + 1]) { int o = L->node[e + 1]; D[o] = fmin(D[o], D[at] + (L->t[e + 1] - L->t[e]) * len); }
                }
            }
        }
        /* each chamber's middle: from the passage axes, straight in */
        for (int r = 0; r < c->nroom; ++r) {
            double best = 1e30;
            for (int s = 0; s < c->nseg; ++s) {
                double off, u = seg_foot(&c->seg[s], c->room[r].c, &off);
                best = fmin(best, seg_path(L, &c->seg[s], s, m, u) + off);
            }
            L->roomD[m][r] = best;
        }
    }
}

#define LIGHT_CACHE 4
typedef struct LightEntry { int has; uint32_t id, sum, age; TerrainCaveLight *L; } LightEntry;
static _Thread_local LightEntry t_light[LIGHT_CACHE];
static _Thread_local uint32_t t_age;

const TerrainCaveLight *terrain_cave_light(const TerrainCave *c) {
    uint32_t sum = cave_sum(c);
    int victim = 0;
    for (int k = 0; k < LIGHT_CACHE; ++k) {
        LightEntry *e = &t_light[k];
        if (e->has && e->id == c->id && e->sum == sum) { e->age = ++t_age; return e->L; }
        if (!t_light[victim].has) continue;
        if (!e->has || e->age < t_light[victim].age) victim = k;
    }
    LightEntry *e = &t_light[victim];
    if (!e->L) e->L = (TerrainCaveLight *)malloc(sizeof(TerrainCaveLight));
    if (!e->L) return NULL;
    light_make(c, e->L);
    e->L->id = c->id; e->L->sum = sum;
    e->id = c->id; e->sum = sum; e->has = 1; e->age = ++t_age;
    return e->L;
}

/* Chamber r's wall's distance at P (Quilez's bound, as the field's). */
static double room_wall_at(const TerrainCaveRoom *r, const double P[3]) {
    double dx = P[0] - r->c[0], dy = P[1] - r->c[1], dz = P[2] - r->c[2];
    double u = dx * r->cs + dz * r->sn, v = -dx * r->sn + dz * r->cs;
    double a = u / r->r[0], bb = dy / r->r[1], cc = v / r->r[2];
    double a2 = u / (r->r[0] * r->r[0]), b2 = dy / (r->r[1] * r->r[1]), c2 = v / (r->r[2] * r->r[2]);
    double k0 = sqrt(a * a + bb * bb + cc * cc), k1 = sqrt(a2 * a2 + b2 * b2 + c2 * c2);
    return k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -fmin(r->r[0], fmin(r->r[1], r->r[2]));
}

double terrain_cave_light_at(const TerrainCave *c, const TerrainCaveLight *L, const uint8_t *segs, int nseg, const uint8_t *rooms, int nroom,
                             double x, double h, double z, double *path) {
    double P[3] = { x, h, z }, wall[TERRAIN_CAVE_SEGS + TERRAIN_CAVE_ROOMS], foot[TERRAIN_CAVE_SEGS], least = 1e30;
    for (int q = 0; q < nseg; ++q) {
        const TerrainCaveSeg *s = &c->seg[segs[q]];
        double off;
        foot[q] = seg_foot(s, P, &off);
        wall[q] = off - (s->ra + (s->rb - s->ra) * foot[q]);
        least = fmin(least, wall[q]);
    }
    for (int k = 0; k < nroom; ++k) {
        wall[nseg + k] = room_wall_at(&c->room[rooms[k]], P);
        least = fmin(least, wall[nseg + k]);
    }
    double sky = 0.0, near = 1e30;
    for (int m = 0; m < c->nmouth && L; ++m) {
        double d = 1e30;
        for (int q = 0; q < nseg + nroom; ++q) {
            if (wall[q] > least + TERRAIN_CAVE_NEAR_M) continue;
            double across = TERRAIN_CAVE_SKY_ACROSS * (wall[q] - least), at;
            if (q < nseg) at = seg_path(L, &c->seg[segs[q]], segs[q], m, foot[q]);
            else {
                const TerrainCaveRoom *r = &c->room[rooms[q - nseg]];
                double e[3] = { P[0] - r->c[0], P[1] - r->c[1], P[2] - r->c[2] };
                at = L->roomD[m][rooms[q - nseg]] + sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
            }
            d = fmin(d, at + across);
        }
        near = fmin(near, d);
        if (d < 1e29) sky = fmax(sky, exp(-d / L->len[m]));
    }
    if (path) *path = near;
    return sky;
}

double terrain_cave_sky(const TerrainCave *c, double x, double h, double z, double *path) {
    static uint8_t all[TERRAIN_CAVE_SEGS];
    static int made;
    if (!made) { for (int k = 0; k < TERRAIN_CAVE_SEGS; ++k) all[k] = (uint8_t)k; made = 1; }
    return terrain_cave_light_at(c, terrain_cave_light(c), all, c->nseg, all, c->nroom, x, h, z, path);
}
