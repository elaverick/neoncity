#include "plan_internal.h"
#include "terrain.h"
#include "thread.h"
#include <stdlib.h>
#include <string.h>

/* The rivers through the cities (see plan.h, "Rivers through the cities").
 *
 * For each settlement, the bake's rivers near it are gathered
 * (terrain_earth_rivers_near) and joined into paths: a reach runs on into
 * the reach starting nearest its end, within JOIN_M, and where two run into
 * one the wider goes on and the other ends there. Each path is walked
 * downstream, read every WALK_STEP metres, and each place it passes is one of:
 * outside the settlement's footprint (its edge measure 0 or more, or nearer
 * another settlement), in a town that isn't built, or in a built town's
 * piece. Where that changes between two readings the place is found by
 * bisection. Out of the built towns the channel follows the river's own
 * points; through a built town, the points where the river crosses from one
 * piece to the next, each on the road between them, so the channel runs
 * straight across every piece. A stretch that comes back into a piece it
 * has left is cut short, straight across that piece. A stretch that starts
 * or ends inside a piece (the river's source, the end of a tributary) is
 * left out, so the channel ends at the road before it.
 *
 * Each stretch is a PlanChannel: across a piece, the piece cut to the
 * channel's width; out in the belt, a rectangle along the river, cut square
 * at a road it crosses into a built town by. Each crossing that is left is a
 * bridge. Ids follow the settlements in order, then each settlement's paths
 * and their stretches in order, so they are the same on every run. */

#define WALK_STEP   25.0     /* m */
#define JOIN_M      1500.0   /* a reach runs on into a reach starting this near its end (HydroRIVERS steps 15" cells) */
#define BISECT_M    1e-4     /* the crossings are found this finely (m) */
#define INDEX_CELL  512.0    /* the channels are listed in cells this wide (m) */
#define BRIDGE_PAD  0.5      /* a deck reaches this far past the channel's walls under it (m) */
#define CHANNEL_IN_MAPPED 4000.0   /* plan_channel_in looks no wider than this in another settlement's frame (m) */

typedef struct Run {
    PlanChannel ch;
    PlanPiece   pc;          /* the piece it crosses, if built */
} Run;

typedef struct Vec { void *p; size_t n, cap, sz; } Vec;

static void *vec_push(Vec *v) {
    if (v->n == v->cap) {
        v->cap = v->cap ? 2 * v->cap : 64;
        v->p = realloc(v->p, v->cap * v->sz);
        if (!v->p) fatal("out of memory laying out the rivers");
    }
    return (char *)v->p + v->sz * v->n++;
}

typedef struct CellEntry { int64_t cell; int kind, id; } CellEntry;   /* kind 0 a channel, 1 a bridge */
#define BRIDGE_BIT 0x80000000u   /* an index entry's id is a bridge's */

static struct {
    int          ready;
    int          nch, nbr;
    PlanChannel *ch;
    PlanBridge  *br;
    int         *ch_k, *br_k;   /* each channel's and bridge's settlement */
    int         *piece_order;   /* the built channels, by piece */
    PlanPiece   *piece;         /* each channel's piece (built ones) */
    int          npiece;
    const PlanChannel **piece_ptr;
    int64_t     *keys;          /* the index: each cell listing anything, sorted, */
    uint32_t    *start;         /* its entries' first (nkeys + 1), */
    uint32_t    *ids;           /* and the entries: a channel's id, or a bridge's | BRIDGE_BIT, each cell's in order */
    size_t       nkeys;
} G;

/* --- Reading a path ---------------------------------------------------------------- */

enum { AT_OUT = 0, AT_UNBUILT, AT_BUILT };
typedef struct At { int kind; PlanPiece pc; } At;

static int same_at(const At *a, const At *b) {
    if (a->kind != b->kind) return 0;
    return a->kind != AT_BUILT || (a->pc.ti == b->pc.ti && a->pc.tj == b->pc.tj && a->pc.ni == b->pc.ni && a->pc.nj == b->pc.nj);
}

typedef struct Walker { int k; PlanPieces *pp; } Walker;

static At at_of(Walker *w, DVec2 p) {
    At a;
    memset(&a, 0, sizeof a);
    if (plan_settlement_near(p.x, p.z) != w->k || plan_city_edge(p.x, p.z) >= 0.0) { a.kind = AT_OUT; return a; }
    plan_piece_at(w->pp, p.x, p.z, &a.pc);
    a.kind = a.pc.built ? AT_BUILT : AT_UNBUILT;
    return a;
}

enum { NODE_END = 0, NODE_POINT, NODE_CROSS };
typedef struct Node {
    DVec2    p;
    int      kind;
    At       after;            /* what the path passes through from here to the next node */
    double   width;            /* the river's there */
    RoadAttr road;             /* NODE_CROSS: the road it crosses */
    DVec2    rq, rn;           /* ...its centre line */
} Node;

static DVec2 lerp2(DVec2 a, DVec2 b, double t) { return dv(a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t); }

/* Walks pts[0..m] (width[i] from point i to i + 1), adding each stretch of
 * nodes within the footprint to *nodes, a stretch ending at a NODE_END. */
static void walk_path(Walker *w, const DVec2 *pts, const double *width, int m, Vec *nodes) {
    At cur = at_of(w, pts[0]);
    if (cur.kind != AT_OUT) {
        Node *n = (Node *)vec_push(nodes);
        memset(n, 0, sizeof *n);
        n->p = pts[0]; n->kind = NODE_END; n->after = cur; n->width = width[0];
    }
    for (int i = 0; i < m; ++i) {
        DVec2 A = pts[i], B = pts[i + 1];
        double L = dlen(dsub(B, A));
        int steps = (int)ceil(L / WALK_STEP);
        if (steps < 1) steps = 1;
        double tp = 0.0;
        for (int s = 1; s <= steps; ++s) {
            double t = (double)s / steps;
            At st = at_of(w, lerp2(A, B, t));
            while (!same_at(&st, &cur)) {
                double lo = tp, hi = t;
                while ((hi - lo) * L > BISECT_M) {
                    double mid = 0.5 * (lo + hi);
                    At am = at_of(w, lerp2(A, B, mid));
                    if (same_at(&am, &cur)) lo = mid; else hi = mid;
                }
                At before = at_of(w, lerp2(A, B, lo)), nxt = at_of(w, lerp2(A, B, hi));
                DVec2 X = lerp2(A, B, 0.5 * (lo + hi));
                if (cur.kind == AT_OUT || nxt.kind == AT_OUT) {
                    /* into the footprint: a stretch starts; out of it: one ends */
                    Node *n = (Node *)vec_push(nodes);
                    memset(n, 0, sizeof *n);
                    n->p = X; n->kind = NODE_END; n->after = nxt; n->width = width[i];
                } else {
                    Node *n = (Node *)vec_push(nodes);
                    memset(n, 0, sizeof *n);
                    n->p = X; n->kind = NODE_CROSS; n->after = nxt; n->width = width[i];
                    n->road = plan_piece_road(&before.pc, &nxt.pc, &n->rq, &n->rn);
                }
                cur = nxt;
                tp = hi;
            }
            tp = t;
        }
        if (cur.kind == AT_UNBUILT && i + 1 < m) {
            Node *n = (Node *)vec_push(nodes);
            memset(n, 0, sizeof *n);
            n->p = B; n->kind = NODE_POINT; n->after = cur; n->width = fmax(width[i], width[i + 1]);
        }
    }
    if (cur.kind != AT_OUT) {
        Node *n = (Node *)vec_push(nodes);
        memset(n, 0, sizeof *n);
        n->p = pts[m]; n->kind = NODE_END; n->after.kind = AT_OUT; n->width = width[m - 1];
    }
}

/* --- Stretches to channels ------------------------------------------------------------ */

static RoadAttr edge_attr(int kind) {
    RoadAttr a;
    memset(&a, 0, sizeof a);
    a.cls = (uint8_t)kind;   /* CHANNEL_EDGE_*, carried through the clips */
    return a;
}

static void channel_bounds(PlanChannel *c) {
    c->x0 = c->z0 = 1e300; c->x1 = c->z1 = -1e300;
    for (int i = 0; i < c->n; ++i) {
        c->x0 = fmin(c->x0, c->v[i].x); c->x1 = fmax(c->x1, c->v[i].x);
        c->z0 = fmin(c->z0, c->v[i].z); c->z1 = fmax(c->z1, c->v[i].z);
    }
}

static int channel_from_poly(PlanChannel *c, const Poly *p) {
    if (p->n < 3 || p->n > PLAN_CHANNEL_PV) return 0;
    c->n = p->n;
    for (int i = 0; i < p->n; ++i) { c->v[i] = p->v[i]; c->edge[i] = p->e[i].cls; }
    channel_bounds(c);
    return 1;
}

/* Across a built piece: the piece cut to the strip either side of a -> b. */
static int built_channel(PlanPieces *pp, const PlanPiece *pc, DVec2 a, DVec2 b, double hw, PlanChannel *c) {
    Poly p;
    if (!plan_piece_poly(pp, pc, &p)) return 0;
    for (int i = 0; i < p.n; ++i) p.e[i] = edge_attr(CHANNEL_EDGE_ROAD);
    DVec2 u = dnorm(dsub(b, a)), nl = dv(-u.z, u.x);
    poly_clip(&p, dadd(a, dscale(nl, hw)), nl, edge_attr(CHANNEL_EDGE_BANK), &p);
    poly_clip(&p, dsub(a, dscale(nl, hw)), dv(-nl.x, -nl.z), edge_attr(CHANNEL_EDGE_BANK), &p);
    return channel_from_poly(c, &p);
}

/* Out in the belt: a rectangle along a -> b, cut square at a road (from or to
 * a built piece) where an end crosses one. */
static int belt_channel(const Node *na, const Node *nb, double hw, PlanChannel *c) {
    DVec2 a = na->p, b = nb->p, d = dsub(b, a);
    double L = dlen(d);
    if (L < 1e-3) return 0;
    DVec2 u = dscale(d, 1.0 / L), nl = dv(-u.z, u.x);
    double ext = na->kind == NODE_CROSS || nb->kind == NODE_CROSS ? 4.0 * hw + 20.0 : 0.0;   /* to be cut at the road */
    DVec2 a0 = na->kind == NODE_CROSS ? dsub(a, dscale(u, ext)) : a, b0 = nb->kind == NODE_CROSS ? dadd(b, dscale(u, ext)) : b;
    Poly p;
    p.n = 4;
    p.v[0] = dadd(a0, dscale(nl, hw)); p.v[1] = dsub(a0, dscale(nl, hw));
    p.v[2] = dsub(b0, dscale(nl, hw)); p.v[3] = dadd(b0, dscale(nl, hw));
    p.e[0] = edge_attr(CHANNEL_EDGE_OPEN); p.e[1] = edge_attr(CHANNEL_EDGE_BANK);
    p.e[2] = edge_attr(CHANNEL_EDGE_OPEN); p.e[3] = edge_attr(CHANNEL_EDGE_BANK);
    for (int e = 0; e < 2; ++e) {
        const Node *n = e ? nb : na;
        if (n->kind != NODE_CROSS) continue;
        DVec2 keep = e ? a : b, rn = n->rn;
        if (ddot(dsub(keep, n->rq), rn) > 0.0) rn = dv(-rn.x, -rn.z);   /* the normal points away from the side kept */
        poly_clip(&p, n->rq, rn, edge_attr(CHANNEL_EDGE_ROAD), &p);
    }
    return channel_from_poly(c, &p);
}

/* Where the line p + s d passes through convex c: s in [*s0, *s1]; 0 if it misses. */
static int line_through(const PlanChannel *c, DVec2 p, DVec2 d, double *s0, double *s1) {
    double lo = -1e300, hi = 1e300;
    double area = 0.0;
    for (int i = 1; i + 1 < c->n; ++i) area += dcross(dsub(c->v[i], c->v[0]), dsub(c->v[i + 1], c->v[0]));
    double o = area >= 0.0 ? 1.0 : -1.0;
    for (int i = 0; i < c->n; ++i) {
        DVec2 a = c->v[i], e = dsub(c->v[(i + 1) % c->n], a);
        /* inside: o * cross(e, x - a) >= 0, with x = p + s d */
        double k0 = o * dcross(e, dsub(p, a)), k1 = o * dcross(e, d);
        if (fabs(k1) < 1e-12) { if (k0 < 0.0) return 0; continue; }
        double s = -k0 / k1;
        if (k1 > 0.0) lo = fmax(lo, s); else hi = fmin(hi, s);
    }
    if (lo > hi) return 0;
    *s0 = lo; *s1 = hi;
    return 1;
}

typedef struct Settled { Vec runs, bridges; } Settled;

/* One stretch of nodes (n[0] .. n[m - 1], the last a NODE_END): its pieces
 * of channel and its bridges. */
static void stretch_out(PlanPieces *pp, Node *n, int m, Settled *out) {
    /* a stretch that comes back into a piece it has left runs straight across it */
    for (int j = 0; j + 1 < m; ++j) {
        if (n[j].after.kind != AT_BUILT) continue;
        for (int k = m - 2; k > j; --k) {
            if (!same_at(&n[k].after, &n[j].after)) continue;
            double wmax = n[j].width;
            for (int q = j + 1; q <= k; ++q) wmax = fmax(wmax, n[q].width);
            n[j].width = wmax;
            memmove(&n[j + 1], &n[k + 1], sizeof(Node) * (size_t)(m - k - 1));
            m -= k - j;
            break;
        }
    }
    int *run_at = (int *)malloc(sizeof(int) * (size_t)(m > 0 ? m : 1));   /* the run from node j, or -1 */
    if (!run_at) fatal("out of memory laying out the rivers");
    for (int j = 0; j + 1 < m; ++j) {
        run_at[j] = -1;
        const Node *a = &n[j], *b = &n[j + 1];
        double hw = 0.5 * fmax(fmax(a->width, b->width), PLAN_CHANNEL_MIN_W);
        Run r;
        memset(&r, 0, sizeof r);
        r.ch.a = a->p; r.ch.b = b->p; r.ch.hw = (float)hw;
        int ok = 0;
        if (a->after.kind == AT_BUILT) {
            /* across a piece, from the road it comes in by to the one it leaves by */
            if (a->kind != NODE_CROSS || b->kind != NODE_CROSS) continue;
            r.pc = a->after.pc;
            r.ch.built = 1;
            ok = built_channel(pp, &r.pc, a->p, b->p, hw, &r.ch);
        } else if (a->after.kind == AT_UNBUILT) {
            ok = belt_channel(a, b, hw, &r.ch);
        }
        if (!ok) continue;
        run_at[j] = (int)out->runs.n;
        *(Run *)vec_push(&out->runs) = r;
    }
    /* a bridge wherever the stretch crosses a road with a channel either side of it */
    for (int j = 1; j + 1 < m; ++j) {
        if (n[j].kind != NODE_CROSS) continue;
        int ra = run_at[j - 1], rb = run_at[j];
        if (ra < 0 && rb < 0) continue;
        PlanBridge b;
        memset(&b, 0, sizeof b);
        b.p = n[j].p;
        b.d = dv(-n[j].rn.z, n[j].rn.x);
        b.half = n[j].road.half; b.walk = n[j].road.walk; b.cls = n[j].road.cls; b.road = n[j].road.id;
        double lo = 1e300, hi = -1e300, s0, s1;
        for (int e = 0; e < 2; ++e) {
            int r = e ? rb : ra;
            if (r < 0) continue;
            /* over the deck's width, from one side's parapet to the other's:
             * the channel's extent there is reached on the deck's edges or at
             * a corner of the channel between them */
            const PlanChannel *c = &((Run *)out->runs.p)[r].ch;
            double wd = b.half + b.walk;
            for (int q = -1; q <= 1; q += 2)
                if (line_through(c, dadd(b.p, dscale(n[j].rn, wd * q)), b.d, &s0, &s1)) { lo = fmin(lo, s0); hi = fmax(hi, s1); }
            for (int v = 0; v < c->n; ++v) {
                DVec2 o = dsub(c->v[v], b.p);
                if (fabs(ddot(o, n[j].rn)) > wd) continue;
                lo = fmin(lo, ddot(o, b.d)); hi = fmax(hi, ddot(o, b.d));
            }
        }
        if (lo > hi) continue;
        b.s0 = lo - BRIDGE_PAD; b.s1 = hi + BRIDGE_PAD;
        *(PlanBridge *)vec_push(&out->bridges) = b;
    }
    free(run_at);
}

/* --- Gathering a settlement's rivers --------------------------------------------- */

typedef struct Seg { uint32_t k; DVec2 a, b; double w; } Seg;

static void gather(void *ctx, uint32_t k, TerrainDir a, TerrainDir b, double width) {
    Vec *v = (Vec *)ctx;
    Seg *s = (Seg *)vec_push(v);
    s->k = k;
    terrain_dir_world(a, &s->a.x, &s->a.z);
    terrain_dir_world(b, &s->b.x, &s->b.z);
    s->w = width;
}

typedef struct Reach { int first, n; int next, main_in; double w_end; } Reach;   /* segs[first .. first + n) */

/* Settlement k's rivers, laid out in its own frame. */
static void settle_in(Settled *out, int k) {
    PlanSettlement s = plan_settlement(k);
    double reach = dlen(dsub(s.disc, s.centre)) + 1.3 * s.radius + 3000.0;
    Vec segs = { NULL, 0, 0, sizeof(Seg) };
    terrain_earth_rivers_near(s.dir, reach, gather, &segs);
    if (!segs.n) return;
    const Seg *sg = (const Seg *)segs.p;
    /* the reaches: runs of segments one after another */
    Vec rv = { NULL, 0, 0, sizeof(Reach) };
    for (size_t i = 0; i < segs.n; ++i) {
        if (i == 0 || sg[i].k != sg[i - 1].k + 1) {
            Reach *r = (Reach *)vec_push(&rv);
            r->first = (int)i; r->n = 0; r->next = -1; r->main_in = -1;
        }
        Reach *r = &((Reach *)rv.p)[rv.n - 1];
        r->n++;
        r->w_end = sg[i].w;
    }
    Reach *R = (Reach *)rv.p;
    int nr = (int)rv.n;
    /* each reach runs on into the one starting nearest its end; the widest
     * running into one goes on through it */
    for (int i = 0; i < nr; ++i) {
        DVec2 e = sg[R[i].first + R[i].n - 1].b;
        double best = JOIN_M;
        for (int j = 0; j < nr; ++j) {
            if (j == i) continue;
            double d = dlen(dsub(sg[R[j].first].a, e));
            if (d < best) { best = d; R[i].next = j; }
        }
        if (R[i].next >= 0) {
            int m = R[R[i].next].main_in;
            if (m < 0 || R[i].w_end > R[m].w_end) R[R[i].next].main_in = i;
        }
    }
    Walker w = { k, plan_pieces_new() };
    Vec nodes = { NULL, 0, 0, sizeof(Node) };
    Vec pts = { NULL, 0, 0, sizeof(DVec2) }, wid = { NULL, 0, 0, sizeof(double) };
    uint8_t *done = (uint8_t *)calloc((size_t)nr, 1);
    if (!done) fatal("out of memory laying out the rivers");
    for (int i = 0; i < nr; ++i) {
        if (R[i].main_in >= 0 || done[i]) continue;   /* a path starts where no reach goes on into it */
        pts.n = wid.n = 0;
        for (int r = i; r >= 0 && !done[r]; ) {
            done[r] = 1;
            for (int q = 0; q < R[r].n; ++q) {
                const Seg *g = &sg[R[r].first + q];
                if (!pts.n) *(DVec2 *)vec_push(&pts) = g->a;
                else if (dlen(dsub(((DVec2 *)pts.p)[pts.n - 1], g->a)) > 1e-6) {   /* the gap from the last reach */
                    *(double *)vec_push(&wid) = g->w;
                    *(DVec2 *)vec_push(&pts) = g->a;
                }
                *(double *)vec_push(&wid) = g->w;
                *(DVec2 *)vec_push(&pts) = g->b;
            }
            int nx = R[r].next;
            if (nx < 0) break;
            if (R[nx].main_in != r) {   /* a tributary: it ends where the reach it runs into starts */
                *(double *)vec_push(&wid) = R[r].w_end;
                *(DVec2 *)vec_push(&pts) = sg[R[nx].first].a;
                break;
            }
            r = nx;
        }
        if (pts.n < 2) continue;
        nodes.n = 0;
        walk_path(&w, (const DVec2 *)pts.p, (const double *)wid.p, (int)pts.n - 1, &nodes);
        Node *nd = (Node *)nodes.p;
        for (size_t a = 0; a < nodes.n; ) {   /* stretch by stretch: each ends at its second NODE_END */
            size_t b = a + 1;
            while (b < nodes.n && nd[b].kind != NODE_END) ++b;
            if (b >= nodes.n) break;
            stretch_out(w.pp, &nd[a], (int)(b - a + 1), out);
            a = b + 1;
        }
    }
    free(done);
    free(nodes.p); free(pts.p); free(wid.p);
    plan_pieces_free(w.pp);
    free(rv.p);
    free(segs.p);
}

static void settle_job(void *ctx, int k) {
    const TerrainFrame *was = terrain_frame_bound();
    terrain_frame_bind(plan_frame_settlement(k));
    settle_in(&((Settled *)ctx)[k], k);
    terrain_frame_bind(was);
}

/* --- The index ----------------------------------------------------------------------- */

static int64_t cell_key(int i, int j) { return (int64_t)(((uint64_t)(uint32_t)i << 32) | (uint32_t)j); }

static int cell_less(const void *a, const void *b) {
    const CellEntry *x = (const CellEntry *)a, *y = (const CellEntry *)b;
    if (x->cell != y->cell) return x->cell < y->cell ? -1 : 1;
    if (x->kind != y->kind) return x->kind - y->kind;
    return x->id - y->id;
}

static int piece_cmp(const PlanPiece *a, const PlanPiece *b) {
    if (a->ti != b->ti) return a->ti < b->ti ? -1 : 1;
    if (a->tj != b->tj) return a->tj < b->tj ? -1 : 1;
    if (a->ni != b->ni) return a->ni < b->ni ? -1 : 1;
    if (a->nj != b->nj) return a->nj < b->nj ? -1 : 1;
    return 0;
}

static int order_less(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    int c = piece_cmp(&G.piece[x], &G.piece[y]);
    return c ? c : x - y;
}

static void index_box(Vec *cells, double x0, double z0, double x1, double z1, double pad, int kind, int id) {
    int i0 = (int)floor((x0 - pad) / INDEX_CELL), i1 = (int)floor((x1 + pad) / INDEX_CELL);
    int j0 = (int)floor((z0 - pad) / INDEX_CELL), j1 = (int)floor((z1 + pad) / INDEX_CELL);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            CellEntry *e = (CellEntry *)vec_push(cells);
            e->cell = cell_key(i, j); e->kind = kind; e->id = id;
        }
}

void plan_water_build(int threads) {
    int ns = plan_settlement_count();
    Settled *st = (Settled *)calloc((size_t)ns, sizeof(Settled));
    if (!st) fatal("out of memory laying out the rivers");
    for (int k = 0; k < ns; ++k) { st[k].runs.sz = sizeof(Run); st[k].bridges.sz = sizeof(PlanBridge); }
    thread_parallel_for(ns, threads, settle_job, st);
    size_t nch = 0, nbr = 0;
    for (int k = 0; k < ns; ++k) { nch += st[k].runs.n; nbr += st[k].bridges.n; }
    G.ch = (PlanChannel *)malloc(sizeof(PlanChannel) * (nch ? nch : 1));
    G.piece = (PlanPiece *)malloc(sizeof(PlanPiece) * (nch ? nch : 1));
    G.br = (PlanBridge *)malloc(sizeof(PlanBridge) * (nbr ? nbr : 1));
    G.ch_k = (int *)malloc(sizeof(int) * (nch ? nch : 1));
    G.br_k = (int *)malloc(sizeof(int) * (nbr ? nbr : 1));
    if (!G.ch || !G.piece || !G.br || !G.ch_k || !G.br_k) fatal("out of memory laying out the rivers");
    G.nch = G.nbr = 0;
    for (int k = 0; k < ns; ++k) {
        for (size_t r = 0; r < st[k].runs.n; ++r) {
            const Run *run = &((const Run *)st[k].runs.p)[r];
            G.ch[G.nch] = run->ch;
            G.ch[G.nch].id = (uint32_t)G.nch;
            G.piece[G.nch] = run->pc;
            G.ch_k[G.nch] = k;
            G.nch++;
        }
        for (size_t b = 0; b < st[k].bridges.n; ++b) {
            G.br_k[G.nbr] = k;
            G.br[G.nbr++] = ((const PlanBridge *)st[k].bridges.p)[b];
        }
        free(st[k].runs.p); free(st[k].bridges.p);
    }
    free(st);
    /* the built ones by piece */
    G.piece_order = (int *)malloc(sizeof(int) * (size_t)(G.nch ? G.nch : 1));
    G.piece_ptr = (const PlanChannel **)malloc(sizeof(PlanChannel *) * (size_t)(G.nch ? G.nch : 1));
    if (!G.piece_order || !G.piece_ptr) fatal("out of memory laying out the rivers");
    G.npiece = 0;
    for (int i = 0; i < G.nch; ++i) if (G.ch[i].built) G.piece_order[G.npiece++] = i;
    qsort(G.piece_order, (size_t)G.npiece, sizeof(int), order_less);
    for (int i = 0; i < G.npiece; ++i) G.piece_ptr[i] = &G.ch[G.piece_order[i]];
    /* the cells: each channel within the ground's reach of it, each bridge's deck */
    Vec cells = { NULL, 0, 0, sizeof(CellEntry) };
    for (int i = 0; i < G.nch; ++i) {
        const PlanChannel *c = &G.ch[i];
        index_box(&cells, c->x0, c->z0, c->x1, c->z1, PLAN_CHANNEL_REACH, 0, i);
    }
    for (int i = 0; i < G.nbr; ++i) {
        const PlanBridge *b = &G.br[i];
        double r = fmax(fabs(b->s0), fabs(b->s1)) + b->half + b->walk;
        index_box(&cells, b->p.x - r, b->p.z - r, b->p.x + r, b->p.z + r, 0.0, 1, i);
    }
    qsort(cells.p, cells.n, sizeof(CellEntry), cell_less);
    const CellEntry *ce = (const CellEntry *)cells.p;
    size_t nk = 0;
    for (size_t i = 0; i < cells.n; ++i) nk += i == 0 || ce[i].cell != ce[i - 1].cell;
    G.keys = (int64_t *)malloc(sizeof(int64_t) * (nk ? nk : 1));
    G.start = (uint32_t *)malloc(sizeof(uint32_t) * (nk + 1));
    G.ids = (uint32_t *)malloc(sizeof(uint32_t) * (cells.n ? cells.n : 1));
    if (!G.keys || !G.start || !G.ids) fatal("out of memory indexing the rivers");
    G.nkeys = 0;
    for (size_t i = 0; i < cells.n; ++i) {
        if (i == 0 || ce[i].cell != ce[i - 1].cell) { G.keys[G.nkeys] = ce[i].cell; G.start[G.nkeys++] = (uint32_t)i; }
        G.ids[i] = (uint32_t)ce[i].id | (ce[i].kind ? BRIDGE_BIT : 0u);
    }
    G.start[G.nkeys] = (uint32_t)cells.n;
    free(cells.p);
    G.ready = 1;
}

/* --- Queries ----------------------------------------------------------------------- */

/* The entries of cell (i, j): G.ids[*lo .. *hi). */
static void cell_range(int i, int j, size_t *lo, size_t *hi) {
    int64_t key = cell_key(i, j);
    size_t a = 0, b = G.nkeys;
    while (a < b) { size_t m = (a + b) / 2; if (G.keys[m] < key) a = m + 1; else b = m; }
    if (a == G.nkeys || G.keys[a] != key) { *lo = *hi = 0; return; }
    *lo = G.start[a];
    *hi = G.start[a + 1];
}

/* Signed distance from p to channel c: to its outline across a piece
 * (negative inside), and to its centre line less its half-width in the belt,
 * so the belt's pieces leave no gaps where the river turns. */
static double channel_sdist(const PlanChannel *c, DVec2 p) {
    if (!c->built) return seg_dist(p, c->a, c->b) - c->hw;
    double area = 0.0;
    for (int i = 1; i + 1 < c->n; ++i) area += dcross(dsub(c->v[i], c->v[0]), dsub(c->v[i + 1], c->v[0]));
    double o = area >= 0.0 ? 1.0 : -1.0, inside = -1e300, out = 1e300;
    for (int i = 0; i < c->n; ++i) {
        DVec2 a = c->v[i], b = c->v[(i + 1) % c->n], e = dsub(b, a);
        double l = dlen(e);
        if (l < 1e-9) continue;
        double s = -o * dcross(e, dsub(p, a)) / l;   /* > 0 outside this edge */
        inside = fmax(inside, s);
        out = fmin(out, seg_dist(p, a, b));
    }
    return inside > 0.0 ? out : inside;
}

double plan_channel_dist(double x, double z, double grow) {
    if (!G.ready || !G.nkeys) return 1e30;
    size_t lo, hi;
    cell_range((int)floor(x / INDEX_CELL), (int)floor(z / INDEX_CELL), &lo, &hi);
    double best = 1e30;
    DVec2 p = dv(x, z);
    for (size_t e = lo; e < hi; ++e) {
        if (G.ids[e] & BRIDGE_BIT) break;   /* a cell's channels come first */
        const PlanChannel *c = &G.ch[G.ids[e]];
        if (x < c->x0 - PLAN_CHANNEL_REACH || x > c->x1 + PLAN_CHANNEL_REACH ||
            z < c->z0 - PLAN_CHANNEL_REACH || z > c->z1 + PLAN_CHANNEL_REACH) continue;
        double d = channel_sdist(c, p) - (c->built ? grow : 0.0);
        if (d < best) best = d;
    }
    return best;
}

/* The channels are in their settlements' own frames: the rectangle is
 * looked for in the frame of the settlement nearest its centre, as it is in
 * that settlement's own frame, and as the square round it (turned and moved
 * there) in any other; a large one there may reach another settlement's, so
 * is taken to hold one. */
int plan_channel_in(double x0, double z0, double x1, double z1) {
    if (!G.ready || !G.nkeys) return 0;
    double cx = 0.5 * (x0 + x1), cz = 0.5 * (z0 + z1), px, pz;
    plan_locate(cx, cz, &px, &pz);
    if (px != cx || pz != cz) {
        double h = 0.5 * hypot(x1 - x0, z1 - z0) * PLAN_LOCATE_STRETCH + 1.0;
        if (h > CHANNEL_IN_MAPPED) return 1;
        x0 = px - h; x1 = px + h; z0 = pz - h; z1 = pz + h;
    }
    int i0 = (int)floor(x0 / INDEX_CELL), i1 = (int)floor(x1 / INDEX_CELL);
    int j0 = (int)floor(z0 / INDEX_CELL), j1 = (int)floor(z1 / INDEX_CELL);
    if ((double)(i1 - i0 + 1) * (j1 - j0 + 1) > 4096.0) return 1;   /* too large a rectangle to look: say yes */
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            size_t lo, hi;
            cell_range(i, j, &lo, &hi);
            for (size_t e = lo; e < hi; ++e) {
                if (G.ids[e] & BRIDGE_BIT) break;
                const PlanChannel *c = &G.ch[G.ids[e]];
                if (c->x1 + PLAN_CHANNEL_REACH >= x0 && c->x0 - PLAN_CHANNEL_REACH <= x1 &&
                    c->z1 + PLAN_CHANNEL_REACH >= z0 && c->z0 - PLAN_CHANNEL_REACH <= z1) return 1;
            }
        }
    return 0;
}

int plan_water_piece(int ti, int tj, int ni, int nj, const PlanChannel *const **out) {
    if (!G.ready || !G.npiece) return 0;
    PlanPiece key = { ti, tj, ni, nj, 1 };
    int a = 0, b = G.npiece;
    while (a < b) { int m = (a + b) / 2; if (piece_cmp(&G.piece[G.piece_order[m]], &key) < 0) a = m + 1; else b = m; }
    int c = a;
    while (c < G.npiece && piece_cmp(&G.piece[G.piece_order[c]], &key) == 0) ++c;
    *out = &G.piece_ptr[a];
    return c - a;
}

static int id_less(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return x - y; }

void plan_water_area(PlanArea *out) {
    out->nchannels = out->nbridges = 0;
    if (!G.ready || !G.nkeys) return;
    int own = terrain_frame()->settlement;   /* the frame's own city's: no other is laid out in it */
    const double pad = PLAN_CHANNEL_AREA_PAD;
    double x0 = out->x0 - pad, z0 = out->z0 - pad, x1 = out->x1 + pad, z1 = out->z1 + pad;
    int i0 = (int)floor(x0 / INDEX_CELL), i1 = (int)floor(x1 / INDEX_CELL);
    int j0 = (int)floor(z0 / INDEX_CELL), j1 = (int)floor(z1 / INDEX_CELL);
    int ids[2][PLAN_MAX_CHANNELS * 4], nid[2] = { 0, 0 };
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            size_t lo, hi;
            cell_range(i, j, &lo, &hi);
            for (size_t e = lo; e < hi; ++e) {
                int kind = (G.ids[e] & BRIDGE_BIT) != 0, id = (int)(G.ids[e] & ~BRIDGE_BIT);
                if (nid[kind] < PLAN_MAX_CHANNELS * 4) ids[kind][nid[kind]++] = id;
            }
        }
    for (int kind = 0; kind < 2; ++kind) {
        qsort(ids[kind], (size_t)nid[kind], sizeof(int), id_less);
        for (int q = 0; q < nid[kind]; ++q) {
            if (q && ids[kind][q] == ids[kind][q - 1]) continue;
            if (kind == 0) {
                const PlanChannel *c = &G.ch[ids[0][q]];
                if (G.ch_k[ids[0][q]] != own) continue;
                if (c->x1 < x0 || c->x0 > x1 || c->z1 < z0 || c->z0 > z1 || out->nchannels >= PLAN_MAX_CHANNELS) continue;
                out->channels[out->nchannels++] = *c;
            } else {
                const PlanBridge *b = &G.br[ids[1][q]];
                if (G.br_k[ids[1][q]] != own) continue;
                double r = fmax(fabs(b->s0), fabs(b->s1)) + b->half + b->walk;
                if (b->p.x + r < x0 || b->p.x - r > x1 || b->p.z + r < z0 || b->p.z - r > z1 || out->nbridges >= PLAN_MAX_BRIDGES) continue;
                out->bridges[out->nbridges++] = *b;
            }
        }
    }
}

int plan_channel_count(void) { return G.nch; }
int plan_bridge_count(void) { return G.nbr; }
const PlanChannel *plan_channel(int k) { return &G.ch[k]; }
int plan_channel_settlement(int k) { return G.ch_k[k]; }
const PlanBridge *plan_bridge(int k) { return &G.br[k]; }
