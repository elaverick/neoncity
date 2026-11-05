#include <stdlib.h>
#include <string.h>
#include "terrain_internal.h"
#include "noise.h"
#include "plan.h"

/* The caves (terrain.h, "Caves"; terrain_internal.h, "Caves"): one system or
 * none per cell of a lattice on each cube face, placed and grown from the
 * cell's hash against the ground, and their signed distance field. */

#define PI_D 3.14159265358979323846

static float hf(uint32_t h, uint32_t k) { return hash_float(hash_combine(h, k)); }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t cell_hash(int f, int i, int j) { return hash3i(WORLD_SEED ^ 0x7EC1u, f, i, j); }

/* How far a passage of radius r can have air from its axis: its noise and
 * the smooth union's bulge. */
static double air_of(double r) { return r + TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M; }

/* No point of a system nearer a city's edge than TERRAIN_CAVE_CITY_M: the
 * edge measure at a node, which changes by at most CITY_EDGE_RATE a metre,
 * must leave room for the step either side and a chamber's half-width. */
#define EDGE_NEED (TERRAIN_CAVE_CITY_M + CITY_EDGE_RATE * (0.5 * TERRAIN_CAVE_STEP_MAX_M + TERRAIN_CAVE_ROOM_M + TERRAIN_CAVE_R_MAX + 2.0 * TERRAIN_CAVE_ROUGH_M + TERRAIN_CAVE_BLEND_M))

void terrain_cave_cell(TerrainDir p, int *f, int *i, int *j) {
    double u, v;
    const int N = TERRAIN_CAVE_SIDE;
    *f = terrain_dir_face(p, &u, &v);
    *i = (int)floor((u + 1.0) * 0.5 * N);
    *j = (int)floor((v + 1.0) * 0.5 * N);
    *i = *i < 0 ? 0 : *i >= N ? N - 1 : *i;
    *j = *j < 0 ? 0 : *j >= N ? N - 1 : *j;
}

/* --- The ground, in a system's chart ------------------------------------------
 * Read in the world's frame (bound while a system is built), so a system is
 * the same whichever frame asks for it. */
static double ground(const TerrainCave *c, double x, double z) { return terrain_height_dir(terrain_chart_dir(&c->chart, x, z), 0.0); }

/* Whether the ground there is the land's own: no road between the
 * settlements and no water's bed cut into it, nor the real water near. */
static int ground_plain(const TerrainCave *c, double x, double z) {
    TerrainDir d = terrain_chart_dir(&c->chart, x, z);
    return fabs(terrain_height_dir(d, 0.0) - terrain_ground_bare_dir(d, 0.0)) < 0.01 && terrain_earth_valley(d) > 300.0;
}

/* The same over a site's slope stencil (slope_at, 10 m) and a little beyond:
 * a mouth stands on the land's own slope, never on a road's cutting. */
static int site_plain(const TerrainCave *c, double x, double z) {
    static const double off[5][2] = { { 0.0, 0.0 }, { 15.0, 0.0 }, { -15.0, 0.0 }, { 0.0, 15.0 }, { 0.0, -15.0 } };
    for (int k = 0; k < 5; ++k)
        if (!ground_plain(c, x + off[k][0], z + off[k][1])) return 0;
    return 1;
}

static double edge_at(const TerrainCave *c, double x, double z) {
    double wx, wz;
    terrain_dir_world(terrain_chart_dir(&c->chart, x, z), &wx, &wz);
    return plan_city_edge(wx, wz);
}

/* The ground's slope at (x, z), by differences `d` either side (m/m). */
static void slope_at(const TerrainCave *c, double x, double z, double d, double *gx, double *gz) {
    *gx = (ground(c, x + d, z) - ground(c, x - d, z)) / (2.0 * d);
    *gz = (ground(c, x, z + d) - ground(c, x, z - d)) / (2.0 * d);
}

/* --- The roof -----------------------------------------------------------------
 * The least rock over some air, less TERRAIN_CAVE_ROOF_M (m; at least 0
 * passes). The air lies under top(t) = h0 + (h1 - h0) clamp(t / L, 0, 1) +
 * up over a strip: from (x0, z0) along (ux, uz), t in [t0, t1], and across
 * it, s in [-w, w]. The ground is sampled every TERRAIN_CAVE_ROOF_STEP_M or
 * finer, each sample less half the most it differs from its four
 * neighbours (so ground falling away between samples counts) and a quarter
 * metre. */
#define STRIP_MAX 4096
static double strip_cover(const TerrainCave *c, double x0, double z0, double ux, double uz, double t0, double t1, double w, double L,
                          double h0, double h1, double up) {
    int na = (int)ceil((t1 - t0) / TERRAIN_CAVE_ROOF_STEP_M) + 1, nl = (int)ceil(2.0 * w / TERRAIN_CAVE_ROOF_STEP_M) + 1;
    if (na < 2) na = 2;
    if (nl < 3) nl = 3;
    if (na * nl > STRIP_MAX) return -1e30;   /* never so large: refuse it */
    double g[STRIP_MAX];
    for (int a = 0; a < na; ++a) {
        double t = t0 + (t1 - t0) * a / (na - 1);
        for (int l = 0; l < nl; ++l) {
            double s = -w + 2.0 * w * l / (nl - 1);
            g[a * nl + l] = ground(c, x0 + ux * t - uz * s, z0 + uz * t + ux * s);
        }
    }
    double least = 1e30;
    for (int a = 0; a < na; ++a) {
        double t = t0 + (t1 - t0) * a / (na - 1), f = L > 0.0 ? clampd(t / L, 0.0, 1.0) : 0.0, top = h0 + (h1 - h0) * f + up;
        for (int l = 0; l < nl; ++l) {
            double v = g[a * nl + l], m = 0.0;
            if (a > 0) m = fmax(m, fabs(v - g[(a - 1) * nl + l]));
            if (a < na - 1) m = fmax(m, fabs(v - g[(a + 1) * nl + l]));
            if (l > 0) m = fmax(m, fabs(v - g[a * nl + l - 1]));
            if (l < nl - 1) m = fmax(m, fabs(v - g[a * nl + l + 1]));
            least = fmin(least, v - 0.5 * m - 0.25 - top);
        }
    }
    return least - TERRAIN_CAVE_ROOF_M;
}

/* A passage from A to B (chart x, h, z) of radii ra .. rb: its air lies
 * within air_of(r) of its axis, so over the strip of that half-width round
 * the axis's plan, its ends' caps too, under the axis's height there plus
 * that radius stretched by the passage's grade (a sloping tube's vertical
 * section is taller). */
static double cave_cover(const TerrainCave *c, const double A[3], const double B[3], double ra, double rb) {
    double dx = B[0] - A[0], dz = B[2] - A[2], L = hypot(dx, dz), ux = 1.0, uz = 0.0;
    if (L > 1e-6) { ux = dx / L; uz = dz / L; }
    double R = air_of(fmax(ra, rb)), g = L > 1e-6 ? (B[1] - A[1]) / L : 0.0;
    return strip_cover(c, A[0], A[2], ux, uz, -R, L + R, R, L, A[1], B[1], R * sqrt(1.0 + g * g));
}

/* A chamber's air, under its top over the square of its widest half-width. */
static double room_cover(const TerrainCave *c, const TerrainCaveRoom *r) {
    double R = fmax(r->r[0], r->r[2]) + TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M;
    return strip_cover(c, r->c[0], r->c[2], r->cs, r->sn, -R, R, R, 0.0, r->c[1], r->c[1], r->r[1] + TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M);
}

/* --- Building a system --------------------------------------------------------- */

#define MAX_NODES (TERRAIN_CAVE_SEGS + 16)
#define MAX_TIPS  64
#define ENTRY_STEP_M 6.0
#define ENTRY_SLACK_M 3.0   /* an entrance ends with this much rock over it to spare, twice running */

#define FLOOR_MATCH_M 0.6   /* air of two pieces within a metre: their floors this near, or they'd stack */

typedef struct Node { double p[3]; double r, cover, grade; } Node;   /* grade: the passage's into it from its parent */
typedef struct Tip  { int node; double dir, r, grade, in; } Tip;   /* grade and in: the passage's into the node, and its heading */

typedef struct Build {
    TerrainCave *c;
    Rng          rng;
    Node         node[MAX_NODES];
    int          nnode;
    double       total;   /* passage length so far (m) */
} Build;

static double rngr(Build *b, double lo, double hi) { return lo + (hi - lo) * rng_float(&b->rng); }

static double dist3(const double a[3], const double b[3]) {
    return sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
}

static void add_seg(Build *b, int na, int nb, double ra, double rb) {
    TerrainCaveSeg *s = &b->c->seg[b->c->nseg++];
    memcpy(s->a, b->node[na].p, sizeof s->a);
    memcpy(s->b, b->node[nb].p, sizeof s->b);
    s->ra = (float)ra; s->rb = (float)rb;
    b->total += dist3(s->a, s->b);
}

static int add_node(Build *b, const double p[3], double r, int parent) {
    Node *n = &b->node[b->nnode];
    memcpy(n->p, p, sizeof n->p);
    n->r = r;
    n->grade = 0.0;
    if (parent >= 0) {
        const double *q = b->node[parent].p;
        double run = hypot(p[0] - q[0], p[2] - q[2]);
        n->grade = run > 0.0 ? (p[1] - q[1]) / run : 0.0;
    }
    n->cover = ground(b->c, p[0], p[2]) - (p[1] + r);
    return b->nnode++;
}

/* --- Stacking --------------------------------------------------------------------
 * Where two pieces' air comes within a metre, their floors blend (the
 * field's floor). So that the blend never holes one into the other nor
 * makes a cliff, their floors there may differ by at most FLOOR_MATCH_M plus
 * TERRAIN_CAVE_GRADE_MAX times how far apart they are across: passages meet
 * and loop at one level, leave a junction at any grade, and never stack. A
 * chamber's floor is level: a passage under it keeps to it, and parts from
 * it at most at that grade from the edge of its floor. */
static int stacked(double fa, double fb, double across) { return fabs(fa - fb) > FLOOR_MATCH_M + TERRAIN_CAVE_GRADE_MAX * across; }

static double room_reach(const TerrainCaveRoom *m) { return fmax(m->r[0], fmax(m->r[1], m->r[2])) + TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M; }
static double room_floor(const TerrainCaveRoom *m) { return m->c[1] - TERRAIN_CAVE_ROOM_FLOOR * m->r[1]; }

/* How far (x, z) lies outside chamber m's floor (its section at the floor's
 * height), at least: 0 on it. */
static double room_out(const TerrainCaveRoom *m, double x, double z) {
    const double s = 0.8930285549745876;   /* sqrt(1 - ROOM_FLOOR^2): the section's share of the semi-axes */
    double dx = x - m->c[0], dz = z - m->c[2], u = (dx * m->cs + dz * m->sn) / (s * m->r[0]), v = (-dx * m->sn + dz * m->cs) / (s * m->r[2]);
    return fmax(0.0, sqrt(u * u + v * v) - 1.0) * s * fmin(m->r[0], m->r[2]);
}

/* The point of passage s nearest P, its radius and its floor there. */
static void seg_nearest(const TerrainCaveSeg *s, const double P[3], double Q[3], double *r, double *floor) {
    double ab[3] = { s->b[0] - s->a[0], s->b[1] - s->a[1], s->b[2] - s->a[2] }, l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    double u = l2 > 0.0 ? ((P[0] - s->a[0]) * ab[0] + (P[1] - s->a[1]) * ab[1] + (P[2] - s->a[2]) * ab[2]) / l2 : 0.0;
    u = clampd(u, 0.0, 1.0);
    for (int a = 0; a < 3; ++a) Q[a] = s->a[a] + u * ab[a];
    *r = s->ra + (s->rb - s->ra) * u;
    *floor = Q[1] - TERRAIN_CAVE_FLOOR * *r;
}

/* Whether a point P of a passage's axis, of radius r, stacks on passage s. */
static int point_stacks(const TerrainCaveSeg *s, const double P[3], double r) {
    double Q[3], rq, fq;
    seg_nearest(s, P, Q, &rq, &fq);
    return dist3(P, Q) < air_of(r) + 1.0 + air_of(rq) && stacked(P[1] - TERRAIN_CAVE_FLOOR * r, fq, hypot(P[0] - Q[0], P[2] - Q[2]));
}

/* Whether a passage from A to B (radii ra .. rb) would stack on a piece, or
 * on `also` if not NULL (one not yet laid). */
static int overlaps_also(const Build *b, const double A[3], const double B[3], double ra, double rb, const TerrainCaveSeg *also) {
    const TerrainCave *c = b->c;
    int n = (int)ceil(dist3(A, B) / 2.0);
    for (int q = 0; q <= n; ++q) {
        double t = n ? (double)q / n : 0.0, P[3], r = ra + (rb - ra) * t;
        for (int a = 0; a < 3; ++a) P[a] = A[a] + (B[a] - A[a]) * t;
        double fp = P[1] - TERRAIN_CAVE_FLOOR * r, ap = air_of(r) + 1.0;
        if (also && point_stacks(also, P, r)) return 1;
        for (int k = 0; k < c->nseg; ++k)
            if (point_stacks(&c->seg[k], P, r)) return 1;
        for (int k = 0; k < c->nroom; ++k) {
            const TerrainCaveRoom *m = &c->room[k];
            if (dist3(P, m->c) < ap + room_reach(m) && stacked(fp, room_floor(m), room_out(m, P[0], P[2]))) return 1;
        }
    }
    return 0;
}

static int overlaps(const Build *b, const double A[3], const double B[3], double ra, double rb) { return overlaps_also(b, A, B, ra, rb, NULL); }

/* Whether chamber r would stack on a piece. */
static int room_overlaps(const Build *b, const TerrainCaveRoom *r) {
    const TerrainCave *c = b->c;
    double reach = room_reach(r) + 1.0, fr = room_floor(r);
    for (int q = 0; q < c->nseg; ++q) {
        const TerrainCaveSeg *s = &c->seg[q];
        int n = (int)ceil(dist3(s->a, s->b) / 2.0);
        for (int i = 0; i <= n; ++i) {
            double t = n ? (double)i / n : 0.0, rr = s->ra + (s->rb - s->ra) * t, P[3];
            for (int a = 0; a < 3; ++a) P[a] = s->a[a] + (s->b[a] - s->a[a]) * t;
            if (dist3(P, r->c) < reach + air_of(rr) && stacked(fr, P[1] - TERRAIN_CAVE_FLOOR * rr, room_out(r, P[0], P[2]))) return 1;
        }
    }
    for (int q = 0; q < c->nroom; ++q) {
        const TerrainCaveRoom *m = &c->room[q];
        double apart = hypot(r->c[0] - m->c[0], r->c[2] - m->c[2]), floors = 0.8930285549745876 * (fmax(r->r[0], r->r[2]) + fmax(m->r[0], m->r[2]));
        if (dist3(r->c, m->c) < reach + room_reach(m) && stacked(fr, room_floor(m), fmax(0.0, apart - floors))) return 1;
    }
    return 0;
}

/* An entrance: its mouth at (mx, mz) on the ground, its passage of radius r
 * heading (ux, uz) into it and falling `fall` a metre, its floor meeting the
 * ground at the mouth. Its axis from r outside the mouth to the first node,
 * ENTRY_STEP_M apart, past which the rock over it is ENTRY_SLACK_M past the
 * roof twice running: written to e[0] (outside) and e[1] (inside). Fails on
 * a road or water's bed, near a city, or with no roof by
 * TERRAIN_CAVE_ENTRY_MAX_M. */
static int entrance(Build *b, double mx, double mz, double ux, double uz, double fall, double r, double e[2][3], TerrainCaveMouth *m) {
    const TerrainCave *c = b->c;
    double mh = ground(c, mx, mz), axis = TERRAIN_CAVE_FLOOR * r;
    int good = 0;
    for (double t = ENTRY_STEP_M; t <= TERRAIN_CAVE_ENTRY_MAX_M; t += ENTRY_STEP_M) {
        double x = mx + ux * t, z = mz + uz * t, P[3] = { x, mh - fall * t + axis, z };
        if (!ground_plain(c, x, z) || edge_at(c, x, z) < EDGE_NEED) return 0;
        good = cave_cover(c, P, P, r, r) >= ENTRY_SLACK_M ? good + 1 : 0;
        if (good < 2) continue;
        e[0][0] = mx - ux * r; e[0][1] = mh + fall * r + axis; e[0][2] = mz - uz * r;
        memcpy(e[1], P, sizeof P);
        m->p[0] = mx; m->p[1] = mh; m->p[2] = mz;
        m->dir[0] = (float)ux; m->dir[1] = (float)uz;
        double far = 0.0;
        for (int k = 0; k < 2; ++k)
            far = fmax(far, sqrt((e[k][0] - mx) * (e[k][0] - mx) + (e[k][1] - mh) * (e[k][1] - mh) + (e[k][2] - mz) * (e[k][2] - mz)));
        m->reach = (float)(far + air_of(r) + 0.5);
        return 1;
    }
    return 0;
}

/* An entrance's passage, in pieces of at most the longest step. */
static int entrance_pieces(double e[2][3]) { return (int)ceil(hypot(e[1][0] - e[0][0], e[1][2] - e[0][2]) / TERRAIN_CAVE_STEP_MAX_M); }

/* Commits an entrance, its pieces' ends as nodes; returns its inner end's. */
static int commit_entrance(Build *b, double e[2][3], double r) {
    int n = entrance_pieces(e), prev = add_node(b, e[0], r, -1);
    for (int k = 1; k <= n; ++k) {
        double q[3];
        for (int a = 0; a < 3; ++a) q[a] = e[0][a] + (e[1][a] - e[0][a]) * k / n;
        int next = add_node(b, q, r, prev);
        add_seg(b, prev, next, r, r);
        prev = next;
    }
    return prev;
}

/* A chamber at node k, its floor the passage's there, if its roof holds and
 * it stacks on nothing (shrunk once if not). */
static void try_room(Build *b, int k) {
    TerrainCave *c = b->c;
    const Node *n = &b->node[k];
    double ax = rngr(b, 8.0, TERRAIN_CAVE_ROOM_M), az = ax * rngr(b, 0.5, 1.0), ay = fmin(rngr(b, 5.0, 12.0), 0.8 * ax), yaw = rngr(b, 0.0, 2.0 * PI_D);
    double floor = n->p[1] - TERRAIN_CAVE_FLOOR * n->r;
    if (edge_at(c, n->p[0], n->p[2]) < EDGE_NEED) return;
    for (int at = 0; at < 2; ++at, ax *= 0.6, az *= 0.6, ay *= 0.6) {
        TerrainCaveRoom r = { { n->p[0], floor + TERRAIN_CAVE_ROOM_FLOOR * ay, n->p[2] }, { (float)ax, (float)ay, (float)az }, (float)cos(yaw), (float)sin(yaw) };
        if (hypot(r.c[0], r.c[2]) + fmax(ax, az) > TERRAIN_CAVE_EXTENT_M + TERRAIN_CAVE_ROOM_M) return;
        if (room_overlaps(b, &r) || room_cover(c, &r) < 0.0) continue;
        c->room[c->nroom++] = r;
        return;
    }
}

/* The passages from the node `start`, heading `dir`, until `budget` metres
 * of passage are laid or every tip has died (see terrain_internal.h). */
static void grow(Build *b, int start, double dir, double grade, double budget) {
    TerrainCave *c = b->c;
    Tip tips[MAX_TIPS];
    int ntip = 0;
    tips[ntip++] = (Tip){ start, dir, b->node[start].r, grade, dir };
    while (ntip > 0 && b->total < budget && c->nseg < TERRAIN_CAVE_SEGS - 12 && b->nnode < MAX_NODES - 12) {
        int pick = rng_float(&b->rng) < 0.3f ? rng_int(&b->rng, 0, ntip - 1) : ntip - 1;
        Tip t = tips[pick];
        tips[pick] = tips[--ntip];
        const Node A = b->node[t.node];
        for (int at = 0; at < 4; ++at) {
            double len = rngr(b, TERRAIN_CAVE_STEP_MIN_M, TERRAIN_CAVE_STEP_MAX_M);
            double d = t.dir + rngr(b, -1.0, 1.0) * (0.5 + 0.4 * at);
            double turn = remainder(d - t.in, 2.0 * PI_D);   /* never back over the passage it leaves */
            if (fabs(turn) > TERRAIN_CAVE_TURN_MAX) d = t.in + copysign(TERRAIN_CAVE_TURN_MAX, turn);
            double r1 = clampd(t.r + rngr(b, -0.8, 0.8), TERRAIN_CAVE_R_MIN, TERRAIN_CAVE_R_MAX);
            /* the grade: back towards level, towards the cover wanted, and never far from the last */
            double grade = 0.5 * t.grade + rngr(b, -0.08, 0.08), lo = t.grade - TERRAIN_CAVE_GRADE_STEP, hi = t.grade + TERRAIN_CAVE_GRADE_STEP;
            if (A.cover > TERRAIN_CAVE_COVER_HI_M) grade += 0.12;
            else if (A.cover < TERRAIN_CAVE_COVER_LO_M) grade -= 0.15;
            grade = clampd(grade, fmax(lo, -TERRAIN_CAVE_GRADE_MAX), fmin(hi, TERRAIN_CAVE_GRADE_MAX));
            double B[3] = { A.p[0] + cos(d) * len, A.p[1] + grade * len, A.p[2] + sin(d) * len };
            if (hypot(B[0], B[2]) > TERRAIN_CAVE_EXTENT_M) continue;
            if (edge_at(c, B[0], B[2]) < EDGE_NEED) continue;
            if (overlaps(b, A.p, B, t.r, r1)) continue;
            double cov = cave_cover(c, A.p, B, t.r, r1);
            if (cov < 0.0) {   /* sent down by what it lacks, if not too steep */
                B[1] += cov - 0.5;
                grade = (B[1] - A.p[1]) / len;
                if (grade < fmax(lo, -TERRAIN_CAVE_GRADE_MAX)) continue;
                if (overlaps(b, A.p, B, t.r, r1) || cave_cover(c, A.p, B, t.r, r1) < 0.0) continue;
            }
            int k = add_node(b, B, r1, t.node);
            add_seg(b, t.node, k, t.r, r1);
            if (rng_float(&b->rng) < (float)TERRAIN_CAVE_BRANCH && ntip < MAX_TIPS) {
                double side = rng_float(&b->rng) < 0.5f ? -1.0 : 1.0;
                tips[ntip++] = (Tip){ k, d + side * rngr(b, 0.9, 1.4), fmax(TERRAIN_CAVE_R_MIN, 0.8 * r1), grade, d };
            }
            if (ntip < MAX_TIPS) tips[ntip++] = (Tip){ k, d, r1, grade, d };
            if (c->nroom < TERRAIN_CAVE_ROOMS && rng_float(&b->rng) < (float)TERRAIN_CAVE_ROOM_CHANCE) try_room(b, k);
            break;
        }
    }
}

/* What a site makes of its ground: an adit straight up a steep slope, a
 * shaft from a level top, or nothing (-1); the heading and fall. `strict`
 * holds the first mouth to the landforms; a second mouth needs only the
 * ground's shape. */
static int site_kind(const TerrainCave *c, double x, double z, int strict, double *ux, double *uz, double *fall) {
    double gx, gz, sx, sz;
    slope_at(c, x, z, 10.0, &gx, &gz);
    slope_at(c, x, z, 3.0, &sx, &sz);
    double s = fmin(hypot(gx, gz), hypot(sx, sz));   /* steep at both scales: a face, not a bump */
    TerrainLandform lf;
    terrain_landform_dir(terrain_chart_dir(&c->chart, x, z), &lf);
    int adit = strict ? (lf.canyon >= TERRAIN_CAVE_WALL_DEEP_M && s >= TERRAIN_CAVE_WALL_SLOPE) ||
                            (lf.w[TERRAIN_MOUNTAINS] >= 0.5f && s >= TERRAIN_CAVE_FLANK_SLOPE)
                      : s >= TERRAIN_CAVE_FLANK_SLOPE;
    if (adit) {
        double l = hypot(gx, gz);
        *ux = gx / l; *uz = gz / l;
        *fall = clampd(TERRAIN_CAVE_COVER_RATE - l, 0.05, TERRAIN_CAVE_SHAFT_GRADE);
        return TERRAIN_CAVE_ADIT;
    }
    if (s >= TERRAIN_CAVE_LEVEL_SLOPE || lf.w[TERRAIN_PLATEAU] < (float)(strict ? TERRAIN_CAVE_PLATEAU_W : 0.5 * TERRAIN_CAVE_PLATEAU_W)) return -1;
    if (strict) {   /* on a top, not at a foot */
        double h = ground(c, x, z), low = 1e30;
        for (int k = 0; k < 8; ++k) {
            double a = k * (PI_D / 4.0);
            low = fmin(low, ground(c, x + TERRAIN_CAVE_TOP_RING_M * cos(a), z + TERRAIN_CAVE_TOP_RING_M * sin(a)));
        }
        if (h - low < TERRAIN_CAVE_TOP_RISE_M) return -1;
    }
    *fall = TERRAIN_CAVE_SHAFT_GRADE;
    return TERRAIN_CAVE_SHAFT;
}

/* A second mouth: a site near some node past the first entrance, its
 * entrance joined to the nearest node by a straight passage under its roof. */
static void second_mouth(Build *b, int first) {
    TerrainCave *c = b->c;
    for (int at = 0; at < 16; ++at) {
        const Node *N = &b->node[rng_int(&b->rng, first, b->nnode - 1)];
        double a = rngr(b, 0.0, 2.0 * PI_D), d = rngr(b, 60.0, 260.0), x = N->p[0] + d * cos(a), z = N->p[2] + d * sin(a);
        if (hypot(x, z) < 120.0 || hypot(x, z) > TERRAIN_CAVE_EXTENT_M - 60.0) continue;
        if (edge_at(c, x, z) < EDGE_NEED || !site_plain(c, x, z)) continue;
        double ux, uz, fall;
        int kind = site_kind(c, x, z, 0, &ux, &uz, &fall);
        if (kind < 0) continue;
        double tx = N->p[0] - x, tz = N->p[2] - z, tl = hypot(tx, tz);
        if (kind == TERRAIN_CAVE_SHAFT) { ux = tx / tl; uz = tz / tl; }
        else if ((ux * tx + uz * tz) / tl < 0.3) continue;   /* the slope must climb towards the cave */
        double r = rngr(b, 2.0, 3.5), e[2][3];
        TerrainCaveMouth m;
        if (!entrance(b, x, z, ux, uz, fall, r, e, &m)) continue;
        int best = -1;
        double bd = 1e30;
        for (int k = first; k < b->nnode; ++k) {
            const double *p = b->node[k].p;
            double dd = (p[0] - e[1][0]) * (p[0] - e[1][0]) + (p[1] - e[1][1]) * (p[1] - e[1][1]) + (p[2] - e[1][2]) * (p[2] - e[1][2]);
            if (dd < bd) { bd = dd; best = k; }
        }
        const Node *K = &b->node[best];
        double run = hypot(K->p[0] - e[1][0], K->p[2] - e[1][2]);
        /* its grade within a step of its entrance's and of the passage it joins */
        double gc = (K->p[1] - e[1][1]) / fmax(run, 1e-3);
        if (run > 280.0 || fabs(gc + fall) > TERRAIN_CAVE_GRADE_STEP || fabs(gc - K->grade) > TERRAIN_CAVE_GRADE_STEP) continue;
        int n = (int)ceil(run / TERRAIN_CAVE_STEP_MAX_M), ok = !overlaps(b, e[0], e[1], r, r), segs = entrance_pieces(e);
        if (n < 1) n = 1;   /* at most 7 */
        if (c->nseg + segs + n > TERRAIN_CAVE_SEGS || b->nnode + segs + n + 1 > MAX_NODES) return;
        double J[8][3];
        TerrainCaveSeg own = { { e[0][0], e[0][1], e[0][2] }, { e[1][0], e[1][1], e[1][2] }, (float)r, (float)r };   /* its entrance, not yet laid */
        for (int k = 0; k <= n && ok; ++k) {
            for (int q = 0; q < 3; ++q) J[k][q] = e[1][q] + (K->p[q] - e[1][q]) * k / n;
            if (edge_at(c, J[k][0], J[k][2]) < EDGE_NEED) ok = 0;
            if (k > 0) {
                double r0 = r + (K->r - r) * (k - 1) / n, r1 = r + (K->r - r) * k / n;
                if (overlaps_also(b, J[k - 1], J[k], r0, r1, &own) || cave_cover(c, J[k - 1], J[k], r0, r1) < 0.0) ok = 0;
            }
        }
        if (!ok) continue;
        int prev = commit_entrance(b, e, r);
        for (int k = 1; k <= n; ++k) {
            double rk = r + (K->r - r) * k / n;
            int next = k < n ? add_node(b, J[k], rk, prev) : best;
            add_seg(b, prev, next, b->node[prev].r, rk);
            prev = next;
        }
        m.kind = kind;
        c->mouth[c->nmouth++] = m;
        return;
    }
}

/* The bounds of the air: every passage's within its air's radius, every
 * chamber's within its own. */
static void bounds(TerrainCave *c) {
    for (int a = 0; a < 3; ++a) { c->lo[a] = 1e30; c->hi[a] = -1e30; }
    for (int k = 0; k < c->nseg; ++k) {
        const TerrainCaveSeg *s = &c->seg[k];
        double R = air_of(fmax(s->ra, s->rb));
        for (int a = 0; a < 3; ++a) {
            c->lo[a] = fmin(c->lo[a], fmin(s->a[a], s->b[a]) - R);
            c->hi[a] = fmax(c->hi[a], fmax(s->a[a], s->b[a]) + R);
        }
    }
    for (int k = 0; k < c->nroom; ++k) {
        const TerrainCaveRoom *r = &c->room[k];
        double extra = TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M, w = fmax(r->r[0], r->r[2]) + extra, h = r->r[1] + extra;
        c->lo[0] = fmin(c->lo[0], r->c[0] - w); c->hi[0] = fmax(c->hi[0], r->c[0] + w);
        c->lo[1] = fmin(c->lo[1], r->c[1] - h); c->hi[1] = fmax(c->hi[1], r->c[1] + h);
        c->lo[2] = fmin(c->lo[2], r->c[2] - w); c->hi[2] = fmax(c->hi[2], r->c[2] + w);
    }
}

/* Candidate k of cell (f, i, j): its system, if the site takes one. */
static int try_site(int f, int i, int j, uint32_t h, int k, TerrainCave *c) {
    double w = 2.0 / TERRAIN_CAVE_SIDE;
    TerrainDir q = terrain_face_dir(f, -1.0 + (i + 0.1 + 0.8 * hf(h, 16u + 2u * (uint32_t)k)) * w, -1.0 + (j + 0.1 + 0.8 * hf(h, 17u + 2u * (uint32_t)k)) * w);
    double wx, wz;
    terrain_dir_world(q, &wx, &wz);
    if (plan_city_edge(wx, wz) < EDGE_NEED) return 0;
    memset(c, 0, sizeof *c);
    c->id = h;
    TerrainDir e = { f / 2 == 2, f / 2 == 0, f / 2 == 1 };   /* the face's u axis, as the canyons' charts */
    c->chart.o = q;
    c->chart.ex = unit3(sub3(e, scale3(q, dot3(e, q))));
    c->chart.ez = cross3(c->chart.ex, q);
    double ux, uz, fall;
    int kind = site_kind(c, 0.0, 0.0, 1, &ux, &uz, &fall);
    if (kind < 0 || !site_plain(c, 0.0, 0.0) || terrain_sea_margin_dir(q) <= 0.0) return 0;
    if (kind == TERRAIN_CAVE_SHAFT) { double a = 2.0 * PI_D * hf(h, 7u + (uint32_t)k); ux = cos(a); uz = sin(a); }
    static _Thread_local Build bt;
    Build *b = &bt;
    memset(b, 0, sizeof *b);
    b->c = c;
    b->rng = rng_make(h ^ (0x9E3779B9u * (uint32_t)(k + 1)));
    double r = rngr(b, 2.0, 3.5), en[2][3];
    TerrainCaveMouth m;
    if (!entrance(b, 0.0, 0.0, ux, uz, fall, r, en, &m)) return 0;
    m.kind = kind;
    c->mouth[c->nmouth++] = m;
    int first = commit_entrance(b, en, r);
    double budget = TERRAIN_CAVE_LEN_MIN_M + (TERRAIN_CAVE_LEN_MAX_M - TERRAIN_CAVE_LEN_MIN_M) * pow(rng_float(&b->rng), 1.6);
    grow(b, first, atan2(uz, ux), -fall, budget);
    if (b->total < TERRAIN_CAVE_LEN_MIN_M) return 0;
    if (rng_float(&b->rng) < (float)TERRAIN_CAVE_SECOND) second_mouth(b, first);
    c->length = (float)b->total;
    bounds(c);
    return 1;
}

int terrain_cave_build(int f, int i, int j, TerrainCave *out) {
    uint32_t h = cell_hash(f, i, j);
    if (hf(h, 0) >= (float)TERRAIN_CAVE_CHANCE) return 0;
    const TerrainFrame *was = terrain_frame_bound();
    terrain_frame_bind(terrain_frame_world());
    int ok = 0;
    for (int k = 0; k < TERRAIN_CAVE_SITES && !ok; ++k) ok = try_site(f, i, j, h, k, out);
    terrain_frame_bind(was);
    return ok;
}

/* --- The cache and the lookup ----------------------------------------------------
 * Per thread, direct-mapped by cell, tagged by the Earth's generation (the
 * ground changes with its bake and its streams). A cell's system is a pure
 * function of the cell, so the cache changes nothing. */
#define CACHE_BITS 6
typedef struct CacheEntry { int f, i, j, has; uint32_t gen; TerrainCave *sys; } CacheEntry;
static _Thread_local CacheEntry *t_cache;

static const TerrainCave *cave_of(int f, int i, int j) {
    uint32_t h = cell_hash(f, i, j);
    if (hf(h, 0) >= (float)TERRAIN_CAVE_CHANCE) return NULL;
    if (!t_cache) t_cache = (CacheEntry *)calloc((size_t)1 << CACHE_BITS, sizeof(CacheEntry));
    uint32_t gen = terrain_earth_generation() + 1u;   /* never 0, which the empty entries hold */
    CacheEntry *e = &t_cache[hash_u32(h) & ((1u << CACHE_BITS) - 1u)];
    if (e->gen == gen && e->f == f && e->i == i && e->j == j) return e->has ? e->sys : NULL;
    if (!e->sys) e->sys = (TerrainCave *)malloc(sizeof(TerrainCave));
    e->has = terrain_cave_build(f, i, j, e->sys);
    e->f = f; e->i = i; e->j = j; e->gen = gen;
    return e->has ? e->sys : NULL;
}

int terrain_caves_near_dir(TerrainDir p, double r, TerrainCave *out, int max) {
    double reach = r * 1.4142135623730951 + TERRAIN_CAVE_REACH;
    int n = 0, i0, i1, j0, j1;
    for (int f = 0; f < 6; ++f) {
        if (!terrain_face_cells(p, f, reach, TERRAIN_CAVE_SIDE, &i0, &i1, &j0, &j1)) continue;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1 && n < max; ++i) {
                const TerrainCave *c = cave_of(f, i, j);
                if (!c) continue;
                double x, z;
                terrain_dir_chart(&c->chart, p, &x, &z);
                double dx = fmax(0.0, fmax(c->lo[0] - x, x - c->hi[0])), dz = fmax(0.0, fmax(c->lo[2] - z, z - c->hi[2]));
                if (hypot(dx, dz) > r * 1.4142135623730951 + 1.0) continue;
                memcpy(&out[n++], c, sizeof *c);
            }
    }
    return n;
}

int terrain_caves_near(double x, double z, double r, TerrainCave *out, int max) {
    return terrain_caves_near_dir(terrain_world_dir(x, z), r, out, max);
}

void terrain_cave_to_chart(const TerrainCave *c, double x, double z, double *cx, double *cz) {
    terrain_dir_chart(&c->chart, terrain_world_dir(x, z), cx, cz);
}

void terrain_cave_to_world(const TerrainCave *c, double cx, double cz, double *x, double *z) {
    terrain_dir_world(terrain_chart_dir(&c->chart, cx, cz), x, z);
}

/* --- The field ---------------------------------------------------------------------
 * Each passage is a tapered tube with rough walls (two octaves of value noise
 * in the chart, TERRAIN_CAVE_ROUGH_M at most, fading out over the lowest
 * FLOOR_FADE_M of the walls so the floor stays level), each chamber an
 * ellipsoid (Quilez's bound) so too. The air is where both of these say so:
 *   walls   the smooth union of the pieces, so passages meet in rounded
 *           junctions, held within a quarter of its width of the hard union
 *           (so where many pieces meet it bulges no more than where two do);
 *   floor   above the floor: each piece's floor plane (a passage's along its
 *           axis, carried on past its ends at its own grade; a chamber's
 *           level) weighted by how near its axis or middle the point is, out
 *           to FLOOR_REACH_M past its air, so at a joint the floors of the
 *           passages either side blend with no step;
 * joined by a soft clamp. The air lies within the walls' union, so within
 * air_of(r) of a passage's axis; past every piece's floor reach it is rock
 * (FLOOR_NONE_M under a floor). */
#define FLOOR_FADE_M  2.0
#define FLOOR_REACH_M (TERRAIN_CAVE_ROUGH_M + 0.25 * TERRAIN_CAVE_BLEND_M + 0.5)
#define FLOOR_NONE_M  2.0

static double smin(double a, double b, double k) {
    double h = fmax(k - fabs(a - b), 0.0) / k;
    return fmin(a, b) - h * h * k * 0.25;
}
static double smax(double a, double b, double k) { return -smin(-a, -b, k); }

static double rough(double x, double h, double z) {
    double n1 = 2.0 * noise_value3(WORLD_SEED ^ 0x7EC2u, x / 7.0, h / 7.0, z / 7.0) - 1.0;
    double n2 = 2.0 * noise_value3(WORLD_SEED ^ 0x7EC3u, x / 2.5, h / 2.5, z / 2.5) - 1.0;
    return TERRAIN_CAVE_ROUGH_M * (0.75 * n1 + 0.25 * n2);
}

/* The unions so far: the walls' smooth one w and hard one m, and the floor's
 * weighted sum fs over its weights' sum ws. */
typedef struct Field { double w, m, fs, ws; } Field;

/* Adds a piece: its wall's distance (no noise), the height of its floor
 * less h (positive under it), its reach (the distance from its axis or
 * middle at which its floor's weight falls to 0, with the wall at
 * reach - FLOOR_REACH_M there) and the point's distance from its axis, and
 * the noise n there. */
static void field_add(Field *F, double wall, double floor, double reach, double dist, double n) {
    double rise = -floor, s = rise <= 0.0 ? 0.0 : rise >= FLOOR_FADE_M ? 1.0 : rise / FLOOR_FADE_M;
    double v = wall + n * s * s * (3.0 - 2.0 * s);
    F->m = fmin(F->m, v);
    F->w = fmax(smin(F->w, v, TERRAIN_CAVE_BLEND_M), F->m - 0.25 * TERRAIN_CAVE_BLEND_M);
    double q = 1.0 - dist / reach;
    if (q <= 0.0) return;
    q = q * q * (3.0 - 2.0 * q);
    F->fs += q * floor;
    F->ws += q;
}

/* Whether a piece whose wall is at `wall` can change the field: with the
 * noise at its most it would still lie a whole blend beyond the walls'
 * union (so neither union takes anything from it), and its floor's weight
 * is 0 (its wall at least FLOOR_REACH_M off). */
static int field_far(const Field *F, double wall) {
    return wall >= FLOOR_REACH_M && wall - TERRAIN_CAVE_ROUGH_M > F->w + TERRAIN_CAVE_BLEND_M;
}

double terrain_cave_sdf_local(const TerrainCave *c, double x, double h, double z) {
    double p[3] = { x, h, z }, out = -1e30;
    for (int a = 0; a < 3; ++a) out = fmax(out, fmax(c->lo[a] - p[a], p[a] - c->hi[a]));
    if (out > 2.0) return out;   /* the air lies within the bounds: at least this far */
    double n = rough(x, h, z);
    Field F = { 1e30, 1e30, 0.0, 0.0 };
    for (int k = 0; k < c->nseg; ++k) {
        const TerrainCaveSeg *s = &c->seg[k];
        double ab[3] = { s->b[0] - s->a[0], s->b[1] - s->a[1], s->b[2] - s->a[2] }, ap[3] = { x - s->a[0], h - s->a[1], z - s->a[2] };
        double l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], tu = l2 > 0.0 ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l2 : 0.0;
        double t = clampd(tu, 0.0, 1.0);
        double qx = ap[0] - t * ab[0], qy = ap[1] - t * ab[1], qz = ap[2] - t * ab[2], r = s->ra + (s->rb - s->ra) * t;
        double dist = sqrt(qx * qx + qy * qy + qz * qz), tube = dist - r;
        if (field_far(&F, tube)) continue;
        /* the floor: the axis's height at the point's place along it, carried on past the ends, less its depth, less h */
        double floor = s->a[1] + tu * ab[1] - TERRAIN_CAVE_FLOOR * r - h;
        field_add(&F, tube, floor, r + FLOOR_REACH_M, dist, n);
    }
    for (int k = 0; k < c->nroom; ++k) {
        const TerrainCaveRoom *r = &c->room[k];
        double dx = x - r->c[0], dy = h - r->c[1], dz = z - r->c[2];
        double u = dx * r->cs + dz * r->sn, v = -dx * r->sn + dz * r->cs;
        double a = u / r->r[0], bb = dy / r->r[1], cc = v / r->r[2];
        double a2 = u / (r->r[0] * r->r[0]), b2 = dy / (r->r[1] * r->r[1]), c2 = v / (r->r[2] * r->r[2]);
        double k0 = sqrt(a * a + bb * bb + cc * cc), k1 = sqrt(a2 * a2 + b2 * b2 + c2 * c2), rmin = fmin(r->r[0], fmin(r->r[1], r->r[2]));
        double ell = k1 > 0.0 ? k0 * (k0 - 1.0) / k1 : -rmin;
        if (field_far(&F, ell)) continue;
        field_add(&F, ell, -dy - TERRAIN_CAVE_ROOM_FLOOR * r->r[1], rmin + FLOOR_REACH_M, fmax(ell + rmin, 0.0), n);
    }
    double floor = F.ws > 0.0 ? F.fs / F.ws : FLOOR_NONE_M;
    return fmax(smax(F.w, floor, TERRAIN_CAVE_FLOOR_K), out);
}

double terrain_cave_sdf(const TerrainCave *c, double x, double y, double z) {
    double cx, cz;
    terrain_cave_to_chart(c, x, z, &cx, &cz);
    return terrain_cave_sdf_local(c, cx, y, cz);
}

/* Down from a point of the air in steps no longer than the field's bound on
 * the distance to the rock (its value over TERRAIN_CAVE_LIPSCHITZ), so no
 * rock is stepped over; then halved to a millimetre. */
double terrain_cave_floor_local(const TerrainCave *c, double x, double h, double z) {
    double d = terrain_cave_sdf_local(c, x, h, z);
    if (d >= 0.0) return -1e30;
    double air = h, rock = h;
    for (int k = 0; k < 20000; ++k) {
        rock = air - fmax(-d / TERRAIN_CAVE_LIPSCHITZ, 0.005);
        d = terrain_cave_sdf_local(c, x, rock, z);
        if (d >= 0.0) break;
        air = rock;
        if (air < c->lo[1]) return c->lo[1];   /* never: the bounds hold the air */
    }
    while (air - rock > 0.001) {
        double mid = 0.5 * (air + rock);
        if (terrain_cave_sdf_local(c, x, mid, z) < 0.0) air = mid; else rock = mid;
    }
    return 0.5 * (air + rock);
}

double terrain_cave_floor(double x, double y, double z) {
    static _Thread_local TerrainCave near[4];
    int n = terrain_caves_near(x, z, 0.0, near, 4);
    double best = -1e30;
    for (int k = 0; k < n; ++k) {
        double cx, cz;
        terrain_cave_to_chart(&near[k], x, z, &cx, &cz);
        best = fmax(best, terrain_cave_floor_local(&near[k], cx, y, cz));
    }
    return best;
}
