#include "terrain_internal.h"
#include "hash.h"
#include "plan.h"

/* The planet's patches (see terrain.h): filling a patch's mesh, the cache of
 * filled patches and the quadtree selection of which to draw. Meshes are pure
 * functions of a patch's identity; the selection depends on the camera and on
 * which patches happen to be filled, never the meshes. */

#define PI 3.14159265358979323846
#define PC TERRAIN_PATCH_CELLS
#define PS TERRAIN_PATCH_SIDE
#define AS (PC + 3)        /* the grid with a one-sample apron, for normals */
#define PP (PC / 2 + 1)    /* the parent's samples under a patch */
/* plan_urban is 0 this far from the chart's origin and beyond (m): well past
 * the city's edge and belt. */
#define URBAN_REACH_M (3.0 * (CITY_RADIUS + CITY_BELT))

double terrain_patch_spacing(int level) {
    return 0.5 * PI * PLANET_RADIUS / (double)((int64_t)PC << level);
}

/* --- Filling a patch ---------------------------------------------------------------- */

static TerrainDir point_at(TerrainDir d, double h) { return scale3(d, PLANET_RADIUS + h); }

static void set3(float out[3], TerrainDir v) { out[0] = (float)v.x; out[1] = (float)v.y; out[2] = (float)v.z; }

int terrain_patch_fill(TerrainPatch t, TerrainPatchMesh *m) {
    TerrainDir pt[AS * AS], dir[PS * PS], par[PP * PP];
    double sp = terrain_patch_spacing(t.level);
    int evals = 0;
    TerrainPatchInfo *in = &m->info;
    memset(in, 0, sizeof(*in));
    in->id = t;
    TerrainDir dc = terrain_patch_dir(t, 2, 1, 1);
    in->centre = scale3(dc, PLANET_RADIUS);
    terrain_dir_chart(dc, &in->chart[0], &in->chart[1]);
    in->lo = 1e30f; in->hi = -1e30f;

    /* The grid and its apron: heights at the patch's spacing. */
    for (int gj = -1; gj <= PC + 1; ++gj)
        for (int gi = -1; gi <= PC + 1; ++gi) {
            TerrainDir d = terrain_patch_dir(t, PC, gi, gj);
            double h = terrain_planet_surface_lod(d, sp);
            evals++;
            pt[(gj + 1) * AS + gi + 1] = point_at(d, h);
            if (gi >= 0 && gi <= PC && gj >= 0 && gj <= PC) {
                dir[gj * PS + gi] = d;
                in->lo = fminf(in->lo, (float)h);
                in->hi = fmaxf(in->hi, (float)h);
            }
        }
    /* The parent's surface under it: its samples at the even vertices. */
    if (t.level > 0)
        for (int pj = 0; pj < PP; ++pj)
            for (int pi = 0; pi < PP; ++pi) {
                TerrainDir d = dir[(2 * pj) * PS + 2 * pi];
                par[pj * PP + pi] = point_at(d, terrain_planet_surface_lod(d, 2.0 * sp));
                evals++;
            }

    /* err: the cells' centres at the children's spacing, against this
     * patch's triangles there (the middle of each cell's diagonal). */
    double err = 0.0;
    for (int cj = 0; cj < PC; ++cj)
        for (int ci = 0; ci < PC; ++ci) {
            TerrainDir d = terrain_patch_dir(t, 2 * PC, 2 * ci + 1, 2 * cj + 1);
            TerrainDir truth = point_at(d, terrain_planet_surface_lod(d, 0.5 * sp));
            evals++;
            TerrainDir mid = scale3(add3(pt[(cj + 1) * AS + ci + 1], pt[(cj + 2) * AS + ci + 2]), 0.5);
            TerrainDir e = sub3(truth, mid);
            err = fmax(err, sqrt(dot3(e, e)));
        }
    in->err = (float)err;

    double lox = 1e300, loz = 1e300, hix = -1e300, hiz = -1e300, r2 = 0.0;
    for (int gj = 0; gj <= PC; ++gj)
        for (int gi = 0; gi <= PC; ++gi) {
            TerrainPatchVertex *v = &m->v[gj * PS + gi];
            TerrainDir P = pt[(gj + 1) * AS + gi + 1];
            TerrainDir a = sub3(pt[(gj + 1) * AS + gi + 2], pt[(gj + 1) * AS + gi]);
            TerrainDir b = sub3(pt[(gj + 2) * AS + gi + 1], pt[gj * AS + gi + 1]);
            TerrainDir n = unit3(cross3(a, b));
            if (dot3(n, P) < 0.0) n = scale3(n, -1.0);   /* the odd faces' (u, v) are left-handed */
            TerrainDir target = P;
            if (t.level > 0) {
                int i0 = gi >> 1, j0 = gj >> 1, oi = gi & 1, oj = gj & 1;
                /* on the parent's grid: its sample, the middle of its edge, or of its cell's diagonal */
                target = scale3(add3(par[j0 * PP + i0], par[(j0 + oj) * PP + i0 + oi]), 0.5);
            }
            TerrainDir rel = sub3(P, in->centre);
            set3(v->pos, rel);
            set3(v->morph, sub3(target, P));
            set3(v->n, n);
            double x, z;
            terrain_dir_chart(dir[gj * PS + gi], &x, &z);
            v->chart[0] = (float)(x - in->chart[0]);
            v->chart[1] = (float)(z - in->chart[1]);
            v->urban = hypot(x, z) < URBAN_REACH_M ? plan_urban(x, z) : 0.0f;
            lox = fmin(lox, x); hix = fmax(hix, x); loz = fmin(loz, z); hiz = fmax(hiz, z);
            r2 = fmax(r2, dot3(rel, rel));
        }
    in->chart_lo[0] = lox; in->chart_lo[1] = loz;
    in->chart_hi[0] = hix; in->chart_hi[1] = hiz;

    /* The skirt: each edge vertex again, hanging down twice the err and a
     * little more, deeper than any crack to a coarser or finer neighbour. */
    double depth = 2.0 * err + 0.05 * sp + 1.0;
    for (int side = 0; side < 4; ++side)
        for (int k = 0; k <= PC; ++k) {
            int gi = side < 2 ? k : (side == 2 ? 0 : PC), gj = side < 2 ? (side == 0 ? 0 : PC) : k;
            const TerrainPatchVertex *e = &m->v[gj * PS + gi];
            TerrainPatchVertex *v = &m->v[PS * PS + side * PS + k];
            *v = *e;
            TerrainDir rel = sub3(sub3(pt[(gj + 1) * AS + gi + 1], scale3(dir[gj * PS + gi], depth)), in->centre);
            set3(v->pos, rel);
            r2 = fmax(r2, dot3(rel, rel));
        }
    in->radius = (float)(sqrt(r2) * (1.0 + 1e-6) + 0.01);   /* float rounding of pos */
    return evals;
}

/* --- The cache --------------------------------------------------------------------- */

static int same_patch(TerrainPatch a, TerrainPatch b) { return a.face == b.face && a.level == b.level && a.i == b.i && a.j == b.j; }

static int patch_hash(TerrainPatch t) {
    uint32_t h = hash_combine(hash_combine(hash_combine((uint32_t)t.face, (uint32_t)t.level), (uint32_t)t.i), (uint32_t)t.j);
    return (int)(hash_u32(h) & 4095u);
}

int terrain_planet_find(const TerrainPlanet *tp, TerrainPatch t) {
    for (int s = tp->head[patch_hash(t)]; s >= 0; s = tp->slot[s].next)
        if (same_patch(tp->slot[s].info.id, t)) return s;
    return -1;
}

static void unlink_slot(TerrainPlanet *tp, int s) {
    int *link = &tp->head[patch_hash(tp->slot[s].info.id)];
    while (*link != s) link = &tp->slot[*link].next;
    *link = tp->slot[s].next;
    tp->slot[s].used = 0;
}

int terrain_planet_insert(TerrainPlanet *tp, const TerrainPatchInfo *info) {
    if (terrain_planet_find(tp, info->id) >= 0) return -1;
    int s = -1;
    uint32_t oldest = 0;
    for (int k = 0; k < TERRAIN_PLANET_SLOTS; ++k) {
        const TerrainPlanetSlot *sl = &tp->slot[k];
        if (!sl->used) { s = k; break; }
        if (sl->info.id.level == 0 || sl->seen == tp->count) continue;   /* pinned, or in use */
        uint32_t age = tp->count - sl->seen;
        if (s < 0 || age > oldest) { s = k; oldest = age; }
    }
    if (s < 0) return -1;
    if (tp->slot[s].used) unlink_slot(tp, s);
    TerrainPlanetSlot *sl = &tp->slot[s];
    sl->info = *info;
    sl->used = 1;
    sl->split = 0;
    sl->seen = tp->count;
    int h = patch_hash(info->id);
    sl->next = tp->head[h];
    tp->head[h] = s;
    return s;
}

int terrain_planet_init(TerrainPlanet *tp, TerrainPatchMesh top[6]) {
    memset(tp, 0, sizeof(*tp));
    for (int k = 0; k < 4096; ++k) tp->head[k] = -1;
    int evals = 0;
    for (int f = 0; f < 6; ++f) {
        TerrainPatch t = { f, 0, 0, 0 };
        evals += terrain_patch_fill(t, &top[f]);
        terrain_planet_insert(tp, &top[f].info);
    }
    return evals;
}

/* --- The selection ----------------------------------------------------------------- */

typedef struct Select {
    TerrainPlanet           *tp;
    const TerrainPlanetView *view;
    double                   occluder;   /* the sphere nothing is seen through (m) */
    double                   clear;      /* the camera's distance to the window's edge less the hole's margin, or 0 */
} Select;

static void want(TerrainPlanet *tp, TerrainPatch t, float pri) {
    int at = tp->nwant;
    for (int k = 0; k < tp->nwant; ++k)
        if (same_patch(tp->want[k], t)) return;
    if (at == TERRAIN_PLANET_MAX_WANTS) {
        if (pri <= tp->want_pri[at - 1]) return;
        at--;
    } else {
        tp->nwant++;
    }
    while (at > 0 && tp->want_pri[at - 1] < pri) {
        tp->want[at] = tp->want[at - 1];
        tp->want_pri[at] = tp->want_pri[at - 1];
        at--;
    }
    tp->want[at] = t;
    tp->want_pri[at] = pri;
}

static void visit(Select *x, int s, double parentSplit) {
    TerrainPlanet *tp = x->tp;
    const TerrainPlanetView *vw = x->view;
    TerrainPlanetSlot *sl = &tp->slot[s];
    const TerrainPatchInfo *in = &sl->info;
    sl->seen = tp->count;
    double r = in->radius;
    if (terrain_horizon_hidden(tp->cam, in->centre, r, x->occluder)) { tp->hidden++; sl->split = 0; return; }
    TerrainDir d = sub3(in->centre, tp->cam);
    double lx = dot3(d, tp->ax), ly = dot3(d, tp->up), lz = dot3(d, tp->az);
    if (!frustum_test_aabb(&vw->frustum, v3((float)(lx - r), (float)(ly - r), (float)(lz - r)), v3((float)(lx + r), (float)(ly + r), (float)(lz + r)))) {
        tp->culled++;
        sl->split = 0;
        return;
    }
    int hole = 0;
    if (vw->window) {
        const double *w = vw->win, m = TERRAIN_PLANET_HOLE_M;
        if (in->chart_lo[0] >= w[0] + m && in->chart_hi[0] <= w[2] - m && in->chart_lo[1] >= w[1] + m && in->chart_hi[1] <= w[3] - m) {
            tp->inside++;
            sl->split = 0;
            return;
        }
        hole = in->chart_hi[0] > w[0] && in->chart_lo[0] < w[2] && in->chart_hi[1] > w[1] && in->chart_lo[1] < w[3];
    }
    /* Distance to the nearest of it that is drawn: past the window's edge
     * where the window takes the rest. */
    double dist = fmax(sqrt(lx * lx + ly * ly + lz * lz) - r, TERRAIN_SSE_NEAR);
    if (hole) dist = fmax(dist, x->clear);
    /* Through the haze an error shows as much less as the contrast left, but
     * never less than TERRAIN_PLANET_HAZE_FLOOR of it: a silhouette against
     * the sky still shows in the haze's colour. The morph's distances leave
     * it out: a child's surface still reaches its own before the camera gets
     * to it. */
    double fade = vw->haze > 0.0 ? fmax(exp(-dist / vw->haze), TERRAIN_PLANET_HAZE_FLOOR) : 1.0;
    double rho = in->err * vw->K / dist * fade, split = in->err * vw->K / TERRAIN_PLANET_REFINE;
    if (in->id.level + 1 < TERRAIN_PATCH_LEVELS && rho > (sl->split ? TERRAIN_PLANET_COARSEN : TERRAIN_PLANET_REFINE)) {
        int cs[4], ready = 1;
        for (int k = 0; k < 4; ++k) {
            TerrainPatch c = terrain_patch_child(in->id, k);
            cs[k] = terrain_planet_find(tp, c);
            if (cs[k] < 0) { ready = 0; want(tp, c, (float)rho); }
            else tp->slot[cs[k]].seen = tp->count;
        }
        if (ready) {
            sl->split = 1;
            for (int k = 0; k < 4; ++k) visit(x, cs[k], split);
            return;
        }
    }
    sl->split = 0;
    if (tp->ndraw == TERRAIN_PLANET_MAX_DRAWS) return;
    TerrainPlanetDraw *dr = &tp->draw[tp->ndraw++];
    dr->slot = s;
    dr->morph_hi = (float)parentSplit;
    dr->morph_lo = (float)(parentSplit * TERRAIN_PLANET_MORPH_IN);
    dr->hole = hole;
}

void terrain_planet_select(TerrainPlanet *tp, const TerrainPlanetView *view) {
    tp->count++;
    tp->ndraw = tp->nwant = 0;
    tp->culled = tp->hidden = tp->inside = 0;
    tp->cam = terrain_chart_point(view->cam[0], view->cam[1], view->cam[2]);
    terrain_chart_frame(view->cam[0], view->cam[2], &tp->ax, &tp->up, &tp->az);
    Select x = { tp, view, PLANET_RADIUS + terrain_planet_lowest(), 0.0 };
    if (view->window) {
        const double *w = view->win;
        double cx = view->cam[0], cz = view->cam[2];
        double edge = fmin(fmin(cx - w[0], w[2] - cx), fmin(cz - w[1], w[3] - cz)) - TERRAIN_PLANET_HOLE_M;
        x.clear = fmax(edge, 0.0) * 0.99;   /* chart distance to 3D: never less than 0.99 of it this near */
    }
    for (int f = 0; f < 6; ++f) {
        TerrainPatch t = { f, 0, 0, 0 };
        int s = terrain_planet_find(tp, t);
        if (s >= 0) visit(&x, s, 0.0);
    }
}
