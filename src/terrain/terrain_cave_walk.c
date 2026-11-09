#include <stdlib.h>
#include <string.h>
#include "terrain_internal.h"

/* Walking in the caves (terrain.h, "Caves: walking"). The solid is the rock
 * of the ground's heightfield less the caves' air: a point is in it where it
 * lies under the ground and the caves' field there is not negative. What the
 * feet stand on is the top of that solid under them; the walls and ceilings
 * are the field's surface under the ground, pushed out of along its gradient
 * in plan.
 *
 * The walker keeps copies of the systems near it and, for each, the pieces
 * near a box of its chart round it (terrain_cave_near_box), so the field
 * there costs a handful of passages rather than every one; the field from
 * them is the cave's bit for bit in the box, and outside it the whole cave's
 * is asked. */

#define WALK_SYSTEMS   4
#define LOOK_R_M       60.0    /* the systems whose bounds come within this (about) are kept */
#define LOOK_MOVE_M    30.0    /* looked up again after a move of this */
#define BOX_M          16.0    /* the near pieces' box: this either way of where it was made */
#define BOX_KEEP_M     10.0    /* remade when the walker is further than this from its middle */
#define HOLE_MARGIN    0.25    /* the ground is cut where the field under it is below minus this (terrain.frag, level 0) */
#define GRAD_H         0.01    /* the field's gradient by differences this either side (m) */

typedef struct WalkSys {
    TerrainCave     cave;
    int             has_box;
    double          lo[3], hi[3], mid[3];
    TerrainCaveNear near;
} WalkSys;

struct TerrainCaveWalk {
    int     n, looked;
    double  lx, lz;            /* where they were looked up (world, this thread's frame) */
    double  fx, fy, fz;        /* where the walker is (terrain_cave_walk_at) */
    int     under;             /* and whether its feet are under the ground there (in a cave) */
    WalkSys sys[WALK_SYSTEMS];
};

/* A column (x, z): each system's chart place there, and whether any system's
 * bounds hold it in plan (with a margin: the field's bound outside them). */
typedef struct Column { double cx[WALK_SYSTEMS], cz[WALK_SYSTEMS]; int live; } Column;

TerrainCaveWalk *terrain_cave_walk_new(void) {
    TerrainCaveWalk *w = (TerrainCaveWalk *)calloc(1, sizeof *w);
    if (!w) fatal("out of memory in terrain_cave_walk_new");
    return w;
}

void terrain_cave_walk_free(TerrainCaveWalk *w) { free(w); }

int terrain_cave_walk_at(TerrainCaveWalk *w, double x, double y, double z) {
    if (!w->looked || hypot(x - w->lx, z - w->lz) > LOOK_MOVE_M) {
        static _Thread_local TerrainCave found[WALK_SYSTEMS];
        int n = terrain_caves_near(x, z, LOOK_R_M, found, WALK_SYSTEMS);
        WalkSys keep[WALK_SYSTEMS];
        int nk = 0;
        for (int k = 0; k < n; ++k) {
            int same = -1;
            for (int q = 0; q < w->n && same < 0; ++q)
                if (w->sys[q].cave.id == found[k].id) same = q;
            if (same >= 0) keep[nk] = w->sys[same];
            else { keep[nk].cave = found[k]; keep[nk].has_box = 0; }
            nk++;
        }
        memcpy(w->sys, keep, sizeof(WalkSys) * (size_t)nk);
        w->n = nk;
        w->lx = x; w->lz = z;
        w->looked = 1;
    }
    w->fx = x; w->fy = y; w->fz = z;
    w->under = 0;
    int any = 0;
    for (int k = 0; k < w->n; ++k) {
        WalkSys *s = &w->sys[k];
        double cx, cz;
        terrain_cave_to_chart(&s->cave, x, z, &cx, &cz);
        double p[3] = { cx, y, cz }, out = 0.0;
        for (int a = 0; a < 3; ++a) out = fmax(out, fmax(s->cave.lo[a] - p[a], p[a] - s->cave.hi[a]));
        if (out > BOX_M) continue;   /* far outside its bounds: the whole cave's field is cheap there */
        any = 1;
        if (s->has_box && fabs(p[0] - s->mid[0]) <= BOX_KEEP_M && fabs(p[1] - s->mid[1]) <= BOX_KEEP_M && fabs(p[2] - s->mid[2]) <= BOX_KEEP_M) continue;
        for (int a = 0; a < 3; ++a) { s->mid[a] = p[a]; s->lo[a] = p[a] - BOX_M; s->hi[a] = p[a] + BOX_M; }
        terrain_cave_near_box(&s->cave, s->lo, s->hi, &s->near);
        s->has_box = 1;
    }
    if (any) w->under = y < terrain_surface(x, z) - 0.05;
    return any;
}

static void column(const TerrainCaveWalk *w, double x, double z, Column *c) {
    c->live = 0;
    for (int k = 0; k < w->n; ++k) {
        const TerrainCave *v = &w->sys[k].cave;
        terrain_cave_to_chart(v, x, z, &c->cx[k], &c->cz[k]);
        if (c->cx[k] >= v->lo[0] - 2.0 && c->cx[k] <= v->hi[0] + 2.0 && c->cz[k] >= v->lo[2] - 2.0 && c->cz[k] <= v->hi[2] + 2.0) c->live = 1;
    }
}

/* The caves' field at height y of a column (a point's chart places); with
 * cut, the one the mouths' holes cut the ground by (terrain_cave_cut_*). */
static double field_of_column(const TerrainCaveWalk *w, const double *cx, const double *cz, double y, int cut) {
    double f = 1e30;
    for (int k = 0; k < w->n; ++k) {
        const WalkSys *s = &w->sys[k];
        double p[3] = { cx[k], y, cz[k] };
        int in = s->has_box;
        for (int a = 0; a < 3 && in; ++a) in = p[a] >= s->lo[a] && p[a] <= s->hi[a];
        f = fmin(f, in ? (cut ? terrain_cave_cut_near : terrain_cave_sdf_near)(&s->cave, &s->near, p[0], y, p[2])
                       : (cut ? terrain_cave_cut_local : terrain_cave_sdf_local)(&s->cave, p[0], y, p[2]));
    }
    return f;
}
static double field_at(const TerrainCaveWalk *w, const double *cx, const double *cz, double y) { return field_of_column(w, cx, cz, y, 0); }

double terrain_cave_walk_field(const TerrainCaveWalk *w, double x, double y, double z) {
    Column c;
    column(w, x, z, &c);
    return c.live ? field_at(w, c.cx, c.cz, y) : 1e30;
}

/* Down a column from a point of the caves' air to the rock under it, in
 * steps no longer than the field's bound on the distance to it (as
 * terrain_cave_floor_local), then halved to a millimetre. */
static double march_down(const TerrainCaveWalk *w, const Column *c, double y) {
    double d = field_at(w, c->cx, c->cz, y), air = y, rock = y;
    for (int k = 0; k < 4000; ++k) {
        rock = air - fmax(-d / TERRAIN_CAVE_LIPSCHITZ, 0.005);
        d = field_at(w, c->cx, c->cz, rock);
        if (d >= 0.0) break;
        air = rock;
    }
    while (air - rock > 0.001) {
        double mid = 0.5 * (air + rock);
        if (field_at(w, c->cx, c->cz, mid) < 0.0) air = mid; else rock = mid;
    }
    return 0.5 * (air + rock);
}

/* Whether the straight way from over the walker's feet (a step up) to
 * (x, y, z) is all air: the sky over the ground, or the caves'. Walking out
 * on the ground, a step that comes down under it into a cave's air has come
 * in through a mouth; one that passes through the ground's rock is a step up
 * a slope too steep for a step, with a cave under its skin. */
#define OPEN_STEP_M 0.1
static int open_to(const TerrainCaveWalk *w, double x, double z, double y) {
    double x0 = w->fx, y0 = w->fy + TERRAIN_CAVE_STEP_M, z0 = w->fz;
    double L = sqrt((x - x0) * (x - x0) + (y - y0) * (y - y0) + (z - z0) * (z - z0));
    int n = (int)ceil(L / OPEN_STEP_M);
    for (int i = 1; i < n; ++i) {
        double t = (double)i / n, px = x0 + (x - x0) * t, py = y0 + (y - y0) * t, pz = z0 + (z - z0) * t;
        if (py >= terrain_surface(px, pz)) continue;
        Column c;
        column(w, px, pz, &c);
        if (!c.live || field_at(w, c.cx, c.cz, py) >= 0.0) return 0;
    }
    return 1;
}

double terrain_cave_walk_ground(const TerrainCaveWalk *w, double x, double z, double feet, double ground) {
    if (!w->n) return ground;
    Column c;
    column(w, x, z, &c);
    if (!c.live) return ground;
    double p = feet + TERRAIN_CAVE_STEP_M;
    if (p >= ground) {   /* over the ground: on it, unless it is cut there (a mouth's hole), then on the cave's floor */
        if (field_of_column(w, c.cx, c.cz, ground, 1) >= -HOLE_MARGIN) return ground;
        return march_down(w, &c, ground);
    }
    /* under it: in a cave's air, on its floor, if the walker can get there; in the rock (never walking: fly mode), the ground */
    double from = p;
    if (field_at(w, c.cx, c.cz, from) >= 0.0) {
        from = feet + 0.05;
        if (field_at(w, c.cx, c.cz, from) >= 0.0) return ground;
    }
    if (!w->under && !open_to(w, x, z, from)) return ground;   /* a steep slope's skin over a cave: on up it */
    return march_down(w, &c, from);
}

int terrain_cave_walk_in_air(const TerrainCaveWalk *w, double x, double y, double z, double ground) {
    if (!w->n || y >= ground) return 0;
    return terrain_cave_walk_field(w, x, y, z) < 0.0;
}

/* The body's spheres: from just over a step and a floor's grade to its top. */
#define BODY_SPHERES 3
static double sphere_h(int i, double r, double top) {
    double lo = TERRAIN_CAVE_STEP_M + 0.25, hi = top - r;
    return lo + (hi - lo) * i / (BODY_SPHERES - 1);
}

double terrain_cave_walk_push(const TerrainCaveWalk *w, double *x, double *z, double feet, double ground, double r, double top) {
    if (!w->n) return 0.0;
    double press = 0.0;
    for (int iter = 0; iter < 3; ++iter) {
        Column c;
        column(w, *x, *z, &c);
        if (!c.live) return press;
        int moved = 0;
        for (int i = 0; i < BODY_SPHERES; ++i) {
            double y = feet + sphere_h(i, r, top);
            if (y >= ground) continue;   /* over the ground: the sky's, whatever the field says */
            double d = field_at(w, c.cx, c.cz, y);
            if (-d > TERRAIN_CAVE_LIPSCHITZ * r) continue;   /* the rock is surely further than r */
            Column a, b;
            column(w, *x + GRAD_H, *z, &a);
            column(w, *x - GRAD_H, *z, &b);
            double gx = (field_at(w, a.cx, a.cz, y) - field_at(w, b.cx, b.cz, y)) / (2.0 * GRAD_H);
            column(w, *x, *z + GRAD_H, &a);
            column(w, *x, *z - GRAD_H, &b);
            double gz = (field_at(w, a.cx, a.cz, y) - field_at(w, b.cx, b.cz, y)) / (2.0 * GRAD_H);
            double gy = (field_at(w, c.cx, c.cz, y + GRAD_H) - field_at(w, c.cx, c.cz, y - GRAD_H)) / (2.0 * GRAD_H);
            double gl = sqrt(gx * gx + gy * gy + gz * gz);
            if (gl < 1e-6) continue;
            double pen = r + d / gl;   /* r less the distance to the rock, to first order */
            if (pen <= 0.0) continue;
            /* out along the gradient, towards the air: in plan; what presses down from above, reported */
            *x -= pen * gx / gl;
            *z -= pen * gz / gl;
            if (gy > 0.0) press = fmax(press, pen * gy / gl);
            moved = 1;
        }
        if (!moved) break;
    }
    return press;
}

double terrain_cave_walk_headroom(const TerrainCaveWalk *w, double x, double z, double feet, double ground, double top) {
    if (!w->n) return top;
    Column c;
    column(w, x, z, &c);
    if (!c.live) return top;
    double on = terrain_cave_walk_ground(w, x, z, feet, ground);   /* what they would stand on there */
    for (int i = 1; i <= 8; ++i) {   /* the first rock up the column from a step over it, by eighths of the rest of the body */
        double h = TERRAIN_CAVE_STEP_M + (top - TERRAIN_CAVE_STEP_M) * i / 8.0, y = on + h;
        if (y < ground && field_at(w, c.cx, c.cz, y) >= 0.0) return h;
    }
    return top;
}
