#include "city_internal.h"
#include "building.h"
#include "hash.h"
#include "mathlib.h"

/* ===========================================================================
 * Street furniture: the fourth near-only layer of chunk meshes (src/render/
 * chunks.c), within PROPS_DISTANCE. Each piece is one of the models in
 * src/city/models/ (baked by tools/model_pack.c from a .glb, with its maps in
 * assets/textures/<model>/), turned and set down whole: MAT_PROP, shaded by
 * shaders/city/props.glsl. Where it stands is a pure function of the plan:
 *
 *   - litter bins along the sidewalks, by the kerb, most in the busy centres;
 *   - now and then a post box on its post at the kerb;
 *   - rows of bollards along the kerbs of the cores and centres, and one at
 *     each side of a crossing's dropped kerb there;
 *   - at a crossing of an arterial or a boulevard (and some local streets), a
 *     signal pole beside the dropped kerb: the push-button unit on it at waist
 *     height facing the crossing, the red hand and green man at its head facing
 *     across the road to the far kerb, both kerbs' in step;
 *   - dumpsters in the back alleys and yards, their backs to a wall: by the
 *     service doors and along the walls that face the lot, never in front of a
 *     door and never where they would close a way through (each leaves
 *     DUMP_CLEAR in front of it, so two facing across an alley still leave 1.5 m);
 *   - air-conditioning condensers on the walls of the walk-ups, shophouses and
 *     estate blocks, at the windows the facade shader draws: hung on brackets
 *     under a sill, or sat on the sill in front of the lower part of the window.
 *
 * Every source (a kerb edge, a crossing, a building) places its own pieces
 * apart from each other and from what the plan already holds (lamp posts,
 * walls, doors, drop kerbs), and sources can't meet: kerb edges keep off the
 * corners and crossings, a building's pieces stay on its own walls. So a
 * piece comes out the same whichever chunk asks, and belongs to the chunk it
 * stands in (city_props' rectangle). The collision layout (city_layout) walks
 * round what stands on the ground, and the litter (litter.c) lies clear of it.
 * ========================================================================= */

typedef struct ModelVertex { float p[3]; int8_t n[3]; float uv[2]; } ModelVertex;
#include "models/aircon.inc"
#include "models/bollard.inc"
#include "models/crossing_button.inc"
#include "models/crossing_sign.inc"
#include "models/dumpster_a.inc"
#include "models/dumpster_b.inc"
#include "models/litter_bin_a.inc"
#include "models/litter_bin_b.inc"
#include "models/mailbox.inc"
#include "models/sofa.inc"

typedef struct Model { const ModelVertex *v; const uint16_t *ix; int ni; const float *box; uint8_t pbr; } Model;
#define MODEL(n, id) { n##_v, n##_i, (int)(sizeof(n##_i) / sizeof(n##_i[0])), n##_box, id }
static const Model MODELS[PM_COUNT] = {
    MODEL(bollard, PBR_BOLLARD), MODEL(litter_bin_a, PBR_LITTER_BIN_A), MODEL(litter_bin_b, PBR_LITTER_BIN_B),
    MODEL(mailbox, PBR_MAILBOX), MODEL(dumpster_a, PBR_DUMPSTER_A), MODEL(dumpster_b, PBR_DUMPSTER_B),
    MODEL(aircon, PBR_AIRCON), MODEL(crossing_sign, PBR_CROSSING_SIGN), MODEL(crossing_button, PBR_CROSSING_BUTTON),
    MODEL(sofa, PBR_SOFA),
};

void model_box(int pm, float mn[3], float mx[3]) {
    for (int k = 0; k < 3; ++k) { mn[k] = MODELS[pm].box[k]; mx[k] = MODELS[pm].box[3 + k]; }
}

uint8_t model_material(int pm) { return MODELS[pm].pbr; }

void emit_model(Mesh *m, int pm, vec3 at, float fx, float fz, Mat mt) {
    const Model *M = &MODELS[pm];
    for (int t = 0; t + 2 < M->ni; t += 3) {
        vec3 p[3], n[3];
        float uv[3][2];
        for (int k = 0; k < 3; ++k) {
            const ModelVertex *v = &M->v[M->ix[t + k]];
            /* local x (its right) -> (fz, -fx), local z (its front) -> (fx, fz) */
            p[k] = v3(at.x + v->p[0] * fz + v->p[2] * fx, at.y + v->p[1], at.z - v->p[0] * fx + v->p[2] * fz);
            float nx = v->n[0] / 127.0f, ny = v->n[1] / 127.0f, nz = v->n[2] / 127.0f;
            n[k] = v3_norm(v3(nx * fz + nz * fx, ny, -nx * fx + nz * fz));
            uv[k][0] = v->uv[0]; uv[k][1] = v->uv[1];
        }
        /* winding follows the normals, as emit_tri */
        int o[3] = { 0, 1, 2 };
        if (v3_dot(v3_cross(v3_sub(p[1], p[0]), v3_sub(p[2], p[0])), v3_add(v3_add(n[0], n[1]), n[2])) < 0.0f) { o[1] = 2; o[2] = 1; }
        for (int k = 0; k < 3; ++k) mesh_push(m, p[o[k]], n[o[k]], uv[o[k]][0], uv[o[k]][1], mt.seed, mt.p, mt.aux);
    }
}

/* ------------------------------------------------------------- the rules --- */

#define KERB_END      3.0f    /* kerb furniture keeps this far from an edge's ends (the corners) */
#define KERB_DROP     1.6f    /* ...and from a drop kerb (the crossing's own) */
#define KERB_LAMP     1.1f    /* ...and from a lamp post, centre to centre */
#define WALK_LEFT     1.5f    /* the sidewalk left clear behind kerb furniture */
#define DUMP_CLEAR    2.6f    /* clear ground in front of a dumpster */
#define DUMP_BACK     0.08f   /* a dumpster's back off its wall */
#define DUMP_IN       2.5f    /* how far into the lot from the sidewalk a dumpster must stand */
#define DOOR_KEEP     0.6f    /* a door's way is kept this much wider than the door each side... */
#define DOOR_REACH    2.2f    /* ...and out this far */
#define POLE_HALF     0.045f  /* a signal pole's half width */
#define POLE_TOP      2.95f
#define SIGNAL_Y      2.25f   /* the signal head's underside above the sidewalk */
#define BUTTON_Y      0.86f   /* the push-button unit's */
#define LAMP_HALF     0.11f   /* a lamp post's half width (build_lamp), when a crossing's signal is on one... */
#define LAMP_BUTTON_Y 1.04f   /* ...where the button clears its base */
#define AIRCON_GAP    0.03f   /* a condenser's back off its wall */
#define MAX_AIRCONS   28      /* per chunk */
#define MAX_PER_BLD   28

enum { GK_NONE = 0, GK_SIDEWALK, GK_LOT, GK_DROP };

typedef struct PGen {
    ChunkGen *g;
    double    x0, z0, x1, z1;   /* what is collected: pieces standing in this rectangle */
    int       mask;
    Prop     *out;
    int       n, max, aircons;
    DropKerb *drops[PLAN_MAX_BLOCKS];
    int       nd[PLAN_MAX_BLOCKS];
} PGen;

static int owned(const PGen *P, double x, double z) { return x >= P->x0 && x < P->x1 && z >= P->z0 && z < P->z1; }

static int block_drops(PGen *P, int bi, const DropKerb **out) {
    if (P->nd[bi] < 0) {
        P->drops[bi] = (DropKerb *)malloc(sizeof(DropKerb) * MAX_BLOCK_DROPS);
        if (!P->drops[bi]) fatal("out of memory placing street furniture");
        P->nd[bi] = city_block_drops(P->g->pa, &P->g->pa->blocks[bi], P->drops[bi], MAX_BLOCK_DROPS);
    }
    *out = P->drops[bi];
    return P->nd[bi];
}

/* What the ground is at (x, z): a block's sidewalk or lot (and which block), a drop kerb's slope, or neither. */
static int ground_kind(PGen *P, double x, double z, int *blk) {
    const PlanArea *pa = P->g->pa;
    for (int k = 0; k < pa->nblocks; ++k) {
        const Block *bk = &pa->blocks[k];
        if (x < bk->x0 || x > bk->x1 || z < bk->z0 || z > bk->z1 || !poly_contains(&bk->curb, x, z, 0.0)) continue;
        *blk = k;
        if (bk->lot.n && poly_contains(&bk->lot, x, z, 0.0)) return GK_LOT;
        if (!poly_contains(&bk->curb, x, z, -KERB_STONE_WIDTH)) return GK_DROP;   /* on the kerb stones: not a footing */
        const DropKerb *dk;
        int nd = block_drops(P, k, &dk);
        for (int i = 0; i < nd; ++i)
            if (city_drop_contains(&dk[i], x, z, 0.1)) return GK_DROP;
        return GK_SIDEWALK;
    }
    *blk = -1;
    return GK_NONE;
}

static int in_plot(const Building *b, double x, double z, double margin, float *lx, float *lz) {
    double dx = x - b->ox, dz = z - b->oz;
    double px = b->c * dx + b->s * dz, pz = -b->s * dx + b->c * dz;
    *lx = (float)px; *lz = (float)pz;
    return px > b->foot.x0 - margin && px < b->foot.x1 + margin && pz > b->foot.z0 - margin && pz < b->foot.z1 + margin;
}

/* Whether a circle of radius rad at (x, z) runs into a wall standing at street
 * level, a column, a plant's fence or a lamp post. */
static int solid(PGen *P, double x, double z, float rad) {
    ChunkGen *g = P->g;
    for (int i = 0; i < g->nl; ++i) {
        double dx = x - g->lamp[i].x, dz = z - g->lamp[i].z, r = 0.22 + rad;
        if (dx * dx + dz * dz < r * r) return 1;
    }
    for (int i = 0; i < g->nb; ++i) {
        float lx, lz;
        if (!in_plot(&g->bld[i], x, z, rad + 0.05, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        if (bgen_industrial(bg)) {
            const float *f = bg->fence;
            int inside = lx > f[0] - rad && lx < f[2] + rad && lz > f[1] - rad && lz < f[3] + rad;
            int within = lx > f[0] + rad && lx < f[2] - rad && lz > f[1] + rad && lz < f[3] - rad;
            if (inside && !within) return 1;
        }
        for (int si = 0; si < bg->nsec; ++si) {
            const BSection *s = &bg->sec[si];
            if (s->y0 > SIDEWALK_HEIGHT + 0.5f || bsec_open(s)) continue;
            if (bfoot_contains(&s->foot, lx, lz) || bfoot_boundary_dist(&s->foot, lx, lz, NULL, NULL) < rad) return 1;
        }
        for (int k = 0; k < bg->nsup; ++k) {
            const BSupport *sp = &bg->sup[k];
            if (sp->type != SUP_COLUMN && sp->type != SUP_STRUT && sp->type != SUP_LEG) continue;
            float h = sp->size * 0.5f + rad;
            if (sp->y0 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x0 - lx) < h && fabsf(sp->z0 - lz) < h) return 1;
            if (sp->y1 <= SIDEWALK_HEIGHT + 0.5f && fabsf(sp->x1 - lx) < h && fabsf(sp->z1 - lz) < h) return 1;
        }
    }
    return 0;
}

/* The ground under a footprint (centre (x, z), half extents hw across and hd
 * along f): all of kind `want` in one block, clear of anything solid. Gives
 * the lowest of the ground at its corners, world Y, so nothing floats. */
static int footing(PGen *P, double x, double z, float fx, float fz, float hw, float hd, int want, float *y) {
    int blk0 = -2;
    float lo = 1e30f;
    for (int j = -2; j <= 2; ++j)
        for (int i = -2; i <= 2; ++i) {
            double a = hw * i * 0.5, b = hd * j * 0.5;
            double px = x + fz * a + fx * b, pz = z - fx * a + fz * b;
            int blk, k = ground_kind(P, px, pz, &blk);
            if (k != want || (blk0 != -2 && blk != blk0)) return 0;
            blk0 = blk;
            if (abs(i) == 2 && abs(j) == 2) lo = minf(lo, city_ground_y(px, pz));
            if (solid(P, px, pz, 0.03f)) return 0;
        }
    *y = lo + SIDEWALK_HEIGHT + (want == GK_LOT ? 0.006f : 0.0f);
    return 1;
}

/* A door's way: from its face out DOOR_REACH, DOOR_KEEP wider each side. Whether (x, z) is in any. */
static int in_doorway(PGen *P, double x, double z) {
    ChunkGen *g = P->g;
    for (int i = 0; i < g->nb; ++i) {
        float lx, lz;
        if (!in_plot(&g->bld[i], x, z, DOOR_REACH + 1.0, &lx, &lz)) continue;
        const BGraph *bg = chunk_gen_graph(g, i);
        for (int di = 0; di < bg->ndoor; ++di) {
            const BDoor *d = &bg->door[di];
            const BLoop *l = &bg->sec[d->section].foot.outer;
            int k = d->face, k1 = (k + 1) % l->n;
            float ex = l->x[k1] - l->x[k], ez = l->z[k1] - l->z[k], el = sqrtf(ex * ex + ez * ez);
            if (el < 1e-4f) continue;
            ex /= el; ez /= el;
            float rx = lx - l->x[k], rz = lz - l->z[k];
            float nx = ez, nz = -ex;   /* out of the wall */
            if (bloop_inside(l, l->x[k] + ex * d->u + nx * 0.1f, l->z[k] + ez * d->u + nz * 0.1f)) { nx = -nx; nz = -nz; }
            float u = rx * ex + rz * ez, o = rx * nx + rz * nz;
            if (fabsf(u - d->u) < d->width * 0.5f + DOOR_KEEP && o > -0.2f && o < DOOR_REACH) return 1;
        }
    }
    return 0;
}

static void add(PGen *P, const Prop *p) {
    if (P->n < P->max && owned(P, p->x, p->z)) P->out[P->n++] = *p;
}

/* The disc a piece keeps others out of, round its centre. */
static float keep_radius(int kind) {
    switch (kind) {
    case PROP_BOLLARD:  return 0.26f;
    case PROP_BIN:      return 0.45f;
    case PROP_MAILBOX:  return 0.33f;
    case PROP_DUMPSTER: return 0.95f;
    case PROP_CROSSING: return 0.2f;
    default:            return 0.3f;
    }
}

static int clear_of(const Prop *list, int n, double x, double z, float r) {
    for (int i = 0; i < n; ++i) {
        double dx = list[i].x - x, dz = list[i].z - z, k = keep_radius(list[i].kind) + r + 0.25;
        if (dx * dx + dz * dz < k * k) return 0;
    }
    return 1;
}

/* Stations along a length, exponential gaps for `rate` per metre (as the litter's). */
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

/* ------------------------------------------------------------ the kerbs --- */

/* Bins, post boxes and rows of bollards along one kerb edge of a block, by the
 * kerb, clear of its ends, its crossings and the lamp posts. */
static void kerb_edge(PGen *P, int bi, int i) {
    const Block *bk = &P->g->pa->blocks[bi];
    const Poly *c = &bk->curb;
    DVec2 A = c->v[i], B = c->v[(i + 1) % c->n];
    double len = dlen(dsub(B, A));
    float walk = c->e[i].walk;
    if (len < 2.0 * KERB_END + 1.0) return;
    DVec2 t = dscale(dsub(B, A), 1.0 / len), in = dv(-t.z, t.x);
    DVec2 cen = poly_centroid(c);
    if (ddot(in, dsub(cen, A)) < 0.0) in = dv(-in.x, -in.z);
    const DropKerb *dk;
    int nd = block_drops(P, bi, &dk);
    Prop list[96];
    int n = 0;
    uint32_t es = hash_combine(bk->seed ^ 0xF0E17u, (uint32_t)i);
    int zone = bk->zone, busy = zone == ZONE_CORE || zone == ZONE_CENTRE;

    /* A candidate centred d in from the kerb line at u along it, its half width
     * and depth (across the kerb); its front facing in from the kerb (or out). */
    #define TRY(KIND, PM, U, D, HW, HD, FACEIN, SEED) do { \
        float u_ = (U), d_ = (D); \
        if (u_ < KERB_END || u_ > len - KERB_END || walk - (d_ + (HD)) < WALK_LEFT) break; \
        double x_ = A.x + t.x * u_ + in.x * d_, z_ = A.z + t.z * u_ + in.z * d_; \
        int near_ = 0; \
        for (int q_ = 0; q_ < nd && !near_; ++q_) near_ = city_drop_contains(&dk[q_], x_, z_, KERB_DROP); \
        for (int q_ = 0; q_ < P->g->nl && !near_; ++q_) { double lx_ = P->g->lamp[q_].x - x_, lz_ = P->g->lamp[q_].z - z_; near_ = lx_ * lx_ + lz_ * lz_ < KERB_LAMP * KERB_LAMP; } \
        if (near_ || !clear_of(list, n, x_, z_, keep_radius(KIND))) break; \
        float fx_ = (float)((FACEIN) ? in.x : -in.x), fz_ = (float)((FACEIN) ? in.z : -in.z), y_; \
        if (!footing(P, x_, z_, fx_, fz_, (HW), (HD), GK_SIDEWALK, &y_)) break; \
        Prop *p_ = &list[n++]; \
        p_->kind = (KIND); p_->model = (PM); p_->flags = 0; p_->x = x_; p_->z = z_; p_->y = y_; \
        p_->ground = city_ground_y(x_, z_); p_->fx = fx_; p_->fz = fz_; p_->seed = (SEED); \
    } while (0)

    float u[64];
    uint32_t ss[64];
    /* litter bins: a few to a street in the centres, sparser out in the estates */
    static const float BIN_RATE[ZONE_COUNT] = { 1.0f / 45.0f, 1.0f / 50.0f, 1.0f / 90.0f, 1.0f / 150.0f, 1.0f / 70.0f, 0.0f };
    int nb = stations(hash_combine(es, 1u), len, BIN_RATE[zone], u, ss, 64);
    for (int k = 0; k < nb && n < 90; ++k) {
        int pm = (ss[k] & 1u) ? PM_BIN_B : PM_BIN_A;
        float mn[3], mx[3];
        model_box(pm, mn, mx);
        float hw = 0.5f * (mx[0] - mn[0]), hd = 0.5f * (mx[2] - mn[2]);
        TRY(PROP_BIN, pm, u[k], KERB_STONE_WIDTH + 0.12f + hd, hw, hd, 1, ss[k]);
    }
    /* a post box to a block, now and then */
    static const float MAIL_P[ZONE_COUNT] = { 0.3f, 0.45f, 0.45f, 0.1f, 0.15f, 0.05f };
    Rng r = rng_make(hash_combine(bk->seed ^ 0x9057u, 0u));
    int mailEdge = (int)(rng_u32(&r) % (uint32_t)c->n);
    if (i == mailEdge && rng_float(&r) < MAIL_P[zone]) {
        float mn[3], mx[3];
        model_box(PM_MAILBOX, mn, mx);
        float hw = 0.5f * (mx[0] - mn[0]), hd = 0.5f * (mx[2] - mn[2]), at = rng_range(&r, 0.2f, 0.8f) * (float)len;
        for (int tries = 0; tries < 4; ++tries, at = fmodf(at + 7.3f, (float)len)) {
            int before = n;
            TRY(PROP_MAILBOX, PM_MAILBOX, at, KERB_STONE_WIDTH + 0.2f + hd, hw, hd, 1, rng_u32(&r));
            if (n > before) break;
        }
    }
    /* rows of bollards keeping cars off the sidewalks of the centres */
    if (busy) {
        int nr = stations(hash_combine(es, 2u), len, 1.0f / 140.0f, u, ss, 64);
        for (int k = 0; k < nr; ++k) {
            Rng rr = rng_make(ss[k]);
            int cnt = rng_int(&rr, 3, 6);
            float pitch = rng_range(&rr, 1.5f, 1.9f);
            for (int j = 0; j < cnt && n < 90; ++j)
                TRY(PROP_BOLLARD, PM_BOLLARD, u[k] + pitch * j, KERB_STONE_WIDTH + 0.28f, 0.22f, 0.22f, 0, rng_u32(&rr));
        }
    }
    #undef TRY
    for (int k = 0; k < n; ++k) add(P, &list[k]);
}

/* ------------------------------------------------------------ crossings --- */

/* The crossing at drop kerb d of block bi: its signal pole, and in the cores
 * and centres a bollard at each side of the lowered kerb. */
static void crossing(PGen *P, int bi, const DropKerb *d) {
    const PlanArea *pa = P->g->pa;
    const Block *bk = &pa->blocks[bi];
    const RoadAttr *at = &bk->curb.e[d->edge];
    DVec2 o = dv(d->ox, d->oz), t = dv(d->tx, d->tz), in = dv(d->nx, d->nz);
    /* the lowered kerb's span along t, and the edge's */
    double ua = 1e30, ub = -1e30;
    for (int j = 0; j < d->n; ++j) {
        if (d->lab[j] != DE_KERB) continue;
        for (int q = 0; q < 2; ++q) {
            const double *v = d->v[(j + q) % d->n];
            double u = (v[0] - o.x) * t.x + (v[1] - o.z) * t.z;
            ua = fmin(ua, u); ub = fmax(ub, u);
        }
    }
    if (ua > ub) return;
    DVec2 B = bk->curb.v[(d->edge + 1) % bk->curb.n];
    double ue = ddot(dsub(B, o), t), e0 = fmin(0.0, ue), e1 = fmax(0.0, ue);
    /* the crossing's identity, which both its kerbs share: its road, and which end */
    uint32_t id = at->id;
    for (int ri = 0; ri < pa->nroads; ++ri) {
        const Road *rd = &pa->roads[ri];
        if (rd->at.id != at->id) continue;
        DVec2 mid = dadd(o, dscale(t, 0.5 * (ua + ub)));
        id = hash_combine(at->id, dlen(dsub(mid, rd->a)) < dlen(dsub(mid, rd->b)) ? 1u : 2u);
        break;
    }
    Rng r = rng_make(hash_combine(id ^ 0xC2055u, hash2i(bk->seed, (int)floor(o.x * 2.0), (int)floor(o.z * 2.0))));
    float walk = at->walk;
    int zone = bk->zone;
    const DropKerb *dk;
    int nd = block_drops(P, bi, &dk);
    Prop list[4];
    int n = 0;
    /* a point u along t (beyond the lowered kerb) and dd in from the kerb line, if it is plain sidewalk */
    #define SPOT(U, DD, X, Z, OK) do { \
        X = o.x + t.x * (U) + in.x * (DD); Z = o.z + t.z * (U) + in.z * (DD); \
        int blk_; OK = (U) > e0 + 1.5 && (U) < e1 - 1.5 && ground_kind(P, X, Z, &blk_) == GK_SIDEWALK && blk_ == bi && !solid(P, X, Z, 0.25f); \
        for (int q_ = 0; q_ < nd && OK; ++q_) if (city_drop_contains(&dk[q_], X, Z, 0.3)) OK = 0; \
    } while (0)

    int signalled = at->cls >= ROAD_ARTERIAL || hash_float(hash_combine(id, 7u)) < 0.5f;
    if (signalled && walk >= 2.2f) {
        /* On a lamp post just beyond the lowered kerb, if one stands there (the first along a street
         * usually does), the side farther from the corner first; else on a pole of its own there. The
         * button faces the crossing: along -t from beyond ub (the right of the signal's front), +t from before ua. */
        int far = (ua - e0) > (e1 - ub) ? -1 : 1;
        for (int pass = 0; pass < 2 && !n; ++pass) {
            int side = pass ? -far : far;
            for (int i = 0; i < P->g->nl; ++i) {
                const Lamp *L = &P->g->lamp[i];
                double u = (L->x - o.x) * t.x + (L->z - o.z) * t.z, dd = (L->x - o.x) * in.x + (L->z - o.z) * in.z;
                double beyond = side > 0 ? u - ub : ua - u;
                int blk;
                if (dd < 0.0 || dd > 1.5 || beyond < 0.2 || beyond > 2.5 || ground_kind(P, L->x, L->z, &blk) != GK_SIDEWALK || blk != bi) continue;
                Prop *p = &list[n++];
                p->kind = PROP_CROSSING; p->model = PM_SIGNAL;
                p->flags = PF_ON_LAMP | (side < 0 ? PF_BUTTON_BACK : 0);
                p->x = L->x; p->z = L->z;
                p->ground = city_ground_y(L->x, L->z);
                p->y = p->ground + SIDEWALK_HEIGHT;
                p->fx = (float)-in.x; p->fz = (float)-in.z;   /* across the road */
                p->seed = id;
                break;
            }
        }
        static const float OFF[4] = { 0.8f, 1.2f, 1.7f, 2.3f };
        for (int tries = 0; tries < 8 && !n; ++tries) {
            int side = tries < 4 ? far : -far;
            double u = side > 0 ? ub + OFF[tries & 3] : ua - OFF[tries & 3], x, z;
            int ok;
            SPOT(u, KERB_STONE_WIDTH + 0.25, x, z, ok);
            if (!ok) continue;
            Prop *p = &list[n++];
            p->kind = PROP_CROSSING; p->model = PM_SIGNAL;
            p->flags = side < 0 ? PF_BUTTON_BACK : 0;
            p->x = x; p->z = z;
            p->ground = city_ground_y(x, z);
            p->y = p->ground + SIDEWALK_HEIGHT;
            p->fx = (float)-in.x; p->fz = (float)-in.z;
            p->seed = id;
        }
    }
    if ((zone == ZONE_CORE || zone == ZONE_CENTRE || zone == ZONE_INDUSTRIAL) && rng_float(&r) < 0.6f) {
        for (int side = -1; side <= 1; side += 2) {
            double u = side > 0 ? ub + 0.4 : ua - 0.4, x, z;
            int ok;
            SPOT(u, KERB_STONE_WIDTH + 0.28, x, z, ok);
            if (!ok || !clear_of(list, n, x, z, keep_radius(PROP_BOLLARD))) continue;
            float y;
            if (!footing(P, x, z, (float)in.x, (float)in.z, 0.2f, 0.2f, GK_SIDEWALK, &y)) continue;
            Prop *p = &list[n++];
            p->kind = PROP_BOLLARD; p->model = PM_BOLLARD; p->flags = 0;
            p->x = x; p->z = z; p->y = y; p->ground = city_ground_y(x, z);
            p->fx = (float)-in.x; p->fz = (float)-in.z;
            p->seed = rng_u32(&r);
        }
    }
    #undef SPOT
    for (int k = 0; k < n; ++k) add(P, &list[k]);
}

/* ------------------------------------------------------------ buildings --- */

/* Face k of a section's outline in the building frame: its start, unit
 * direction, outward normal and length. */
static float face_of(const BFoot *f, const BLoop *l, int k, float *ax, float *az, float *ex, float *ez, float *nx, float *nz) {
    int k1 = (k + 1) % l->n;
    float dx = l->x[k1] - l->x[k], dz = l->z[k1] - l->z[k], len = sqrtf(dx * dx + dz * dz);
    if (len < 1e-4f) return 0.0f;
    *ax = l->x[k]; *az = l->z[k]; *ex = dx / len; *ez = dz / len;
    *nx = *ez; *nz = -*ex;
    float mx = *ax + dx * 0.5f + *nx * 0.05f, mz = *az + dz * 0.5f + *nz * 0.05f;
    if (bfoot_contains(f, mx, mz)) { *nx = -*nx; *nz = -*nz; }
    return len;
}

/* Whether anything of a building's own, or a neighbour's, stands at (x, y, z)
 * (building i's frame, y above its base): a section's volume. */
static int occupied(PGen *P, int i, float lx, float y, float lz, int self) {
    ChunkGen *g = P->g;
    const Building *b = &g->bld[i];
    DVec2 w = to_world(b, lx, lz);
    float wy = building_base(b) + y;
    for (int j = 0; j < g->nb; ++j) {
        const Building *o = &g->bld[j];
        float ox, oz;
        if (!in_plot(o, w.x, w.z, 0.05, &ox, &oz)) continue;
        const BGraph *og = chunk_gen_graph(g, j);
        float oy = wy - building_base(o);
        for (int si = 0; si < og->nsec; ++si) {
            if (j == i && si == self) continue;
            const BSection *s = &og->sec[si];
            if (oy >= s->y0 - 0.05f && oy <= s->y1 + 0.05f && bfoot_contains(&s->foot, ox, oz)) return 1;
        }
    }
    return 0;
}

/* Air-conditioning condensers at the windows of building i's walls. */
static void aircons(PGen *P, int i) {
    ChunkGen *g = P->g;
    const Building *b = &g->bld[i];
    const BGraph *bg = chunk_gen_graph(g, i);
    if (bgen_industrial(bg) || b->kind == BK_MEGA) return;
    static const float LOT_P[ZONE_COUNT] = { 0.05f, 0.12f, 0.14f, 0.0f, 0.0f, 0.1f }, STREET_P[ZONE_COUNT] = { 0.0f, 0.0f, 0.07f, 0.0f, 0.0f, 0.0f };
    if (LOT_P[b->zone] <= 0.0f) return;
    const Block *bk = &g->pa->blocks[g->bblk[i]];
    uint32_t sh = style_hash(bg->seed);
    float floorH = city_floor_height(bg->seed), groundH = city_ground_height(bg->seed) * (bg->groundMode == GF_DOUBLE ? 2.0f : 1.0f);
    float baseBay = bgen_bay_width(bg->seed, bg->fine);
    float am[3], ax_[3];
    model_box(PM_AIRCON, am, ax_);
    float aw = ax_[0] - am[0], ah = ax_[1] - am[1], ad = ax_[2] - am[2];
    int placed = 0;
    for (int si = 0; si < bg->nsec && placed < MAX_PER_BLD; ++si) {
        const BSection *s = &bg->sec[si];
        if (!bsec_facade(s) || s->win == 1 || s->win == 3 || s->palette == 4 || s->palette == 7) continue;
        float winW, winH;
        if (s->win == 0) {
            winW = 0.45f + 0.25f * hash_float(hash_combine(sh, 5u));
            winH = 0.45f + 0.20f * hash_float(hash_combine(sh, 9u));
        } else {
            winW = 1.0f;
            winH = 0.4f + 0.15f * hash_float(hash_combine(sh, 9u));
        }
        float sill = (1.0f - winH) * 0.42f;
        const BLoop *l = &s->foot.outer;
        for (int k = 0; k < l->n && placed < MAX_PER_BLD; ++k) {
            float ax, az, ex, ez, nx, nz;
            float len = face_of(&s->foot, l, k, &ax, &az, &ex, &ez, &nx, &nz);
            if (len < 2.0f) continue;
            /* a wall facing the lot (an alley, a yard), or the street */
            DVec2 probe = to_world(b, ax + ex * len * 0.5f + nx * 4.0f, az + ez * len * 0.5f + nz * 4.0f);
            int lotSide = bk->lot.n && poly_contains(&bk->lot, probe.x, probe.z, 0.0);
            float p = lotSide ? LOT_P[b->zone] : STREET_P[b->zone];
            if (p <= 0.0f) continue;
            float nBays = maxf(1.0f, floorf(len / baseBay + 0.5f)), bayW = len / nBays;
            if (winW * bayW < aw + 0.1f && s->win == 0) continue;   /* too narrow a window to sit at */
            int every = s->plant ? 6 + 3 * s->plant : 0;
            for (int fi = 0; fi < 16 && placed < MAX_PER_BLD; ++fi) {
                if (every && fi > 0 && fi % every == every - 1) continue;   /* louvred plant floors */
                float floorY = SIDEWALK_HEIGHT + groundH + fi * floorH, ys = floorY + sill * floorH;
                if (ys > s->y1) break;
                for (int bi2 = 0; bi2 < (int)nBays && placed < MAX_PER_BLD; ++bi2) {
                    uint32_t h = hash3i(bg->seed ^ 0xA1C0u, si * 64 + k, fi, bi2);
                    if (hash_float(h) >= p) continue;
                    /* sat on the sill, in front of the window's lower part, or hung on brackets under it */
                    int onSill = hash_float(hash_combine(h, 1u)) < 0.35f && winH * floorH > ah + 0.3f;
                    float y0 = onSill ? ys + 0.01f : ys - 0.08f - ah;
                    if (y0 < floorY + 0.05f) { if (winH * floorH <= ah + 0.3f) continue; y0 = ys + 0.01f; onSill = 1; }
                    if (y0 < s->y0 + 0.1f || y0 + ah > s->y1 - 0.2f) continue;
                    float uc = (bi2 + 0.5f) * bayW + (onSill ? 0.0f : (hash_float(hash_combine(h, 2u)) - 0.5f) * 0.3f * maxf(bayW - aw, 0.0f));
                    float lx = ax + ex * uc, lz = az + ez * uc;
                    /* nothing of this building's or a neighbour's in the way */
                    float off = AIRCON_GAP + ad * 0.5f;
                    if (occupied(P, i, lx + nx * off, y0 + ah * 0.5f, lz + nz * off, si) ||
                        occupied(P, i, lx + nx * off + ex * aw * 0.5f, y0 + 0.05f, lz + nz * off + ez * aw * 0.5f, si) ||
                        occupied(P, i, lx + nx * off - ex * aw * 0.5f, y0 + 0.05f, lz + nz * off - ez * aw * 0.5f, si)) continue;
                    DVec2 w = to_world(b, lx, lz);
                    Prop pr;
                    pr.kind = PROP_AIRCON; pr.model = PM_AIRCON; pr.flags = onSill ? 0 : PF_BRACKETS;
                    pr.x = w.x; pr.z = w.z;
                    pr.ground = building_base(b);
                    pr.y = pr.ground + y0;
                    pr.fx = b->c * nx - b->s * nz; pr.fz = b->s * nx + b->c * nz;
                    pr.seed = h;
                    placed++;
                    if (owned(P, pr.x, pr.z) && P->aircons < MAX_AIRCONS) { P->aircons++; add(P, &pr); }
                }
            }
        }
    }
}

/* Dumpsters in building i's back alleys and yard: by its service doors, then along its walls that face the lot. */
static void dumpsters(PGen *P, int i) {
    ChunkGen *g = P->g;
    const Building *b = &g->bld[i];
    const BGraph *bg = chunk_gen_graph(g, i);
    /* behind shophouses, walk-ups and sheds: not against an office lobby's glass, nor under a void deck */
    if (bgen_industrial(bg) || b->kind == BK_MEGA || bg->groundMode != GF_RETAIL) return;
    static const float RATE[ZONE_COUNT] = { 1.0f / 60.0f, 1.0f / 40.0f, 1.0f / 80.0f, 1.0f / 40.0f, 0.0f, 0.0f };
    if (RATE[b->zone] <= 0.0f) return;
    const Block *bk = &g->pa->blocks[g->bblk[i]];
    if (!bk->lot.n) return;
    int most = b->zone == ZONE_RESIDENTIAL ? 1 : 2;
    Prop list[8];
    int n = 0;
    float mn[3], mx[3];
    model_box(PM_DUMPSTER_A, mn, mx);
    float hw = 0.5f * (mx[0] - mn[0]), hd = 0.5f * (mx[2] - mn[2]);
    /* candidates: (section, face, u) */
    float cu[48];
    int cs[48], cf[48], nc = 0;
    for (int di = 0; di < bg->ndoor && nc < 46; ++di) {
        const BDoor *d = &bg->door[di];
        if (d->type == DOOR_PRIMARY) continue;
        float side = (hash_combine(bg->seed ^ 0xD0D0u, (uint32_t)di) & 1u) ? 1.0f : -1.0f, off = d->width * 0.5f + DOOR_KEEP + hw + 0.05f;
        cs[nc] = d->section; cf[nc] = d->face; cu[nc++] = d->u + side * off;
        cs[nc] = d->section; cf[nc] = d->face; cu[nc++] = d->u - side * off;
    }
    for (int si = 0; si < bg->nsec && nc < 48; ++si) {
        const BSection *s = &bg->sec[si];
        if (!bsec_grounded(s) || !bsec_facade(s)) continue;
        const BLoop *l = &s->foot.outer;
        for (int k = 0; k < l->n && nc < 48; ++k) {
            float ax, az, ex, ez, nx, nz, len = face_of(&s->foot, l, k, &ax, &az, &ex, &ez, &nx, &nz);
            float u[16];
            uint32_t ss[16];
            int ns = stations(hash3i(bg->seed ^ 0xD4A5u, si, k, 0), len, RATE[b->zone], u, ss, 16);
            for (int q = 0; q < ns && nc < 48; ++q) { cs[nc] = si; cf[nc] = k; cu[nc++] = u[q]; }
        }
    }
    for (int q = 0; q < nc && n < most; ++q) {
        const BSection *s = &bg->sec[cs[q]];
        const BLoop *l = &s->foot.outer;
        float ax, az, ex, ez, nx, nz, len = face_of(&s->foot, l, cf[q], &ax, &az, &ex, &ez, &nx, &nz);
        float u = cu[q];
        if (len < 2.0f * hw + 2.6f || u < hw + 1.3f || u > len - hw - 1.3f) continue;
        float off = DUMP_BACK + hd;
        DVec2 c = to_world(b, ax + ex * u + nx * off, az + ez * u + nz * off);
        float fx = b->c * nx - b->s * nz, fz = b->s * nx + b->c * nz;
        /* well into the lot, facing into it (an alley or yard), not out to the street */
        if (!poly_contains(&bk->lot, c.x, c.z, -DUMP_IN) || !poly_contains(&bk->lot, c.x + fx * 3.5f, c.z + fz * 3.5f, -0.5) ||
            !clear_of(list, n, c.x, c.z, keep_radius(PROP_DUMPSTER))) continue;
        float y;
        if (!footing(P, c.x, c.z, fx, fz, hw, hd - 0.02f, GK_LOT, &y)) continue;
        /* never in a door's way, and never closing the way past it */
        int bad = 0;
        for (int j = -3; j <= 3 && !bad; ++j)
            for (int kk = -2; kk <= 2 && !bad; ++kk) {
                double a = hw * j / 3.0, e = hd * kk / 2.0;
                bad = in_doorway(P, c.x + fz * a + fx * e, c.z - fx * a + fz * e);
            }
        for (int j = -3; j <= 3 && !bad; ++j)
            for (int kk = 1; kk <= 9 && !bad; ++kk) {
                double a = (hw + 0.3) * j / 3.0, e = hd + DUMP_CLEAR * kk / 9.0;
                double px = c.x + fz * a + fx * e, pz = c.z - fx * a + fz * e;
                bad = solid(P, px, pz, 0.0f);
            }
        if (bad) continue;
        uint32_t seed = hash3i(bg->seed ^ 0xD5u, cs[q], cf[q], (int)floorf(u * 4.0f));
        Prop *p = &list[n++];
        p->kind = PROP_DUMPSTER; p->model = (seed & 1u) ? PM_DUMPSTER_B : PM_DUMPSTER_A; p->flags = 0;
        p->x = c.x; p->z = c.z; p->y = y; p->ground = city_ground_y(c.x, c.z);
        p->fx = fx; p->fz = fz; p->seed = seed;
    }
    for (int k = 0; k < n; ++k) add(P, &list[k]);
}

/* --------------------------------------------------------------- collect --- */

int city_props(ChunkGen *g, double x0, double z0, double x1, double z1, int mask, Prop *out, int max) {
    PGen *P = (PGen *)calloc(1, sizeof(PGen));
    if (!P) fatal("out of memory placing street furniture");
    P->g = g; P->x0 = x0; P->z0 = z0; P->x1 = x1; P->z1 = z1; P->mask = mask; P->out = out; P->max = max;
    for (int k = 0; k < PLAN_MAX_BLOCKS; ++k) P->nd[k] = -1;
    const double pad = 3.0;
    if (mask & PROPS_GROUND) {
        for (int bi = 0; bi < g->pa->nblocks; ++bi) {
            const Block *bk = &g->pa->blocks[bi];
            if (bk->x1 < x0 - pad || bk->x0 > x1 + pad || bk->z1 < z0 - pad || bk->z0 > z1 + pad) continue;
            const Poly *c = &bk->curb;
            for (int i = 0; i < c->n; ++i) {
                if (!c->e[i].id) continue;
                DVec2 a = c->v[i], b = c->v[(i + 1) % c->n];
                if (fmax(a.x, b.x) < x0 - pad || fmin(a.x, b.x) > x1 + pad || fmax(a.z, b.z) < z0 - pad || fmin(a.z, b.z) > z1 + pad) continue;
                kerb_edge(P, bi, i);
            }
            const DropKerb *dk;
            int nd = block_drops(P, bi, &dk);
            for (int d = 0; d < nd; ++d) {
                double dx0 = 1e300, dz0 = 1e300, dx1 = -1e300, dz1 = -1e300;
                for (int k = 0; k < dk[d].n; ++k) {
                    dx0 = fmin(dx0, dk[d].v[k][0]); dx1 = fmax(dx1, dk[d].v[k][0]);
                    dz0 = fmin(dz0, dk[d].v[k][1]); dz1 = fmax(dz1, dk[d].v[k][1]);
                }
                if (dx1 < x0 - pad || dx0 > x1 + pad || dz1 < z0 - pad || dz0 > z1 + pad) continue;
                crossing(P, bi, &dk[d]);
            }
        }
    }
    for (int i = 0; i < g->nb; ++i) {
        const Building *b = &g->bld[i];
        double r = 0.5 * sqrt((double)(b->foot.x1 - b->foot.x0) * (b->foot.x1 - b->foot.x0) + (double)(b->foot.z1 - b->foot.z0) * (b->foot.z1 - b->foot.z0)) + pad;
        if (b->ox + r < x0 || b->ox - r > x1 || b->oz + r < z0 || b->oz - r > z1) continue;
        if (mask & PROPS_GROUND) dumpsters(P, i);
        if (mask & PROPS_WALLS) aircons(P, i);
    }
    for (int k = 0; k < PLAN_MAX_BLOCKS; ++k) free(P->drops[k]);
    int n = P->n;
    free(P);
    return n;
}

int city_chunk_props(int cx, int cz, Prop *out, int max) {
    ChunkGen g;
    chunk_gen_init(&g, cx, cz, PROPS_PLAN_PAD, PROPS_BUILD_PAD, PROPS_LAMP_PAD);
    int n = city_props(&g, g.ox, g.oz, g.ox + CHUNK_SIZE, g.oz + CHUNK_SIZE, PROPS_GROUND | PROPS_WALLS, out, max);
    chunk_gen_free(&g);
    return n;
}

int prop_outline(const Prop *p, double (*v)[2]) {
    float mn[3], mx[3];
    model_box(p->model, mn, mx);
    float hw = 0.5f * (mx[0] - mn[0]), hd = 0.5f * (mx[2] - mn[2]);
    if (p->kind == PROP_CROSSING) hw = hd = POLE_HALF + 0.02f;
    if (p->kind == PROP_AIRCON || (p->flags & PF_ON_LAMP)) return 0;   /* out of reach; the lamp post's own */
    static const float cx[4] = { -1, 1, 1, -1 }, cz[4] = { -1, -1, 1, 1 };
    for (int k = 0; k < 4; ++k) {
        double a = hw * cx[k], e = hd * cz[k];
        v[k][0] = p->x + p->fz * a + p->fx * e;
        v[k][1] = p->z - p->fx * a + p->fz * e;
    }
    return 4;
}

/* ------------------------------------------------------------------ mesh --- */

#define BUDGET 180000   /* vertices: a margin inside MAX_CHUNK_PAGES * PAGE_VERTS */

/* MAT_PROP: params y the model's material (PBR_*), z the model (PM_*), w PF_* | the signal's phase. */
static Mat prop_mat(const Prop *p, int pm) { return mat(MAT_PROP, p->seed, MODELS[pm].pbr, (uint8_t)pm, p->flags); }

static void emit_prop(Mesh *m, const Prop *p) {
    float ox = (float)(p->x - m->ox), oz = (float)(p->z - m->oz);
    m->grounded = 1;
    m->ground = p->ground;
    vec3 at = v3(ox, p->y, oz);
    switch (p->kind) {
    case PROP_CROSSING: {
        /* the pole (unless it is a lamp post's), its cap, the signal head near its top facing across the road, the button facing the crossing */
        Mat metal = mat(MAT_METAL, p->seed, 4, 0, 0);
        float y0 = p->y, bx = p->fz, bz = -p->fx;   /* the right of the signal's front */
        if (p->flags & PF_BUTTON_BACK) { bx = -bx; bz = -bz; }
        int lamp = (p->flags & PF_ON_LAMP) != 0;
        float half = lamp ? LAMP_HALF : POLE_HALF;
        Xf sv = xf_save(m);
        if (!lamp) {
            xf_enter(m, ox, oz, p->fz, -p->fx);
            m->xy = p->ground;   /* the frame's heights count from the ground (Vertex.base) */
            float r0 = y0 - p->ground;
            emit_box(m, v3(-POLE_HALF, r0 - 0.05f, -POLE_HALF), v3(POLE_HALF, r0 + POLE_TOP, POLE_HALF), F_NOBOT, metal, UV_FACADE);
            emit_box(m, v3(-POLE_HALF - 0.012f, r0 + POLE_TOP, -POLE_HALF - 0.012f), v3(POLE_HALF + 0.012f, r0 + POLE_TOP + 0.03f, POLE_HALF + 0.012f),
                     F_NOBOT | F_BOT, metal, UV_FACADE);
        }
        xf_restore(m, sv);
        float sm[3], sx[3];
        model_box(PM_SIGNAL, sm, sx);
        float sd = 0.5f * (sx[2] - sm[2]);
        emit_model(m, PM_SIGNAL, v3(ox + p->fx * (half + sd), y0 + SIGNAL_Y, oz + p->fz * (half + sd)), p->fx, p->fz, prop_mat(p, PM_SIGNAL));
        model_box(PM_BUTTON, sm, sx);
        sd = 0.5f * (sx[2] - sm[2]);
        emit_model(m, PM_BUTTON, v3(ox + bx * (half + sd), y0 + (lamp ? LAMP_BUTTON_Y : BUTTON_Y), oz + bz * (half + sd)), bx, bz, prop_mat(p, PM_BUTTON));
        break;
    }
    case PROP_AIRCON: {
        float am[3], ax[3];
        model_box(PM_AIRCON, am, ax);
        float hd = 0.5f * (ax[2] - am[2]), hw = 0.5f * (ax[0] - am[0]);
        vec3 c = v3(ox + p->fx * (AIRCON_GAP + hd), p->y, oz + p->fz * (AIRCON_GAP + hd));
        emit_model(m, PM_AIRCON, c, p->fx, p->fz, prop_mat(p, PM_AIRCON));
        if (p->flags & PF_BRACKETS) {
            /* two steel angle brackets under it: an arm out from the wall, a strut back up to it */
            Mat metal = mat(MAT_METAL, p->seed, 4, 0, 0);
            Xf sv = xf_save(m);
            xf_enter(m, ox, oz, p->fz, -p->fx);
            m->xy = p->ground;
            float depth = AIRCON_GAP + 2.0f * hd + 0.04f, y = p->y - p->ground;
            for (int s = -1; s <= 1; s += 2) {
                float x = s * (hw - 0.12f);
                emit_box(m, v3(x - 0.02f, y - 0.035f, 0.0f), v3(x + 0.02f, y, depth), F_NOBOT | F_BOT, metal, UV_FACADE);
                emit_box(m, v3(x - 0.015f, y - 0.3f, 0.0f), v3(x + 0.015f, y - 0.035f, 0.025f), F_NOBOT | F_BOT, metal, UV_FACADE);
                emit_beam(m, v3(x, y - 0.28f, 0.02f), v3(x, y - 0.03f, depth * 0.85f), 0.025f, metal);
            }
            xf_restore(m, sv);
        }
        break;
    }
    default:
        emit_model(m, p->model, at, p->fx, p->fz, prop_mat(p, p->model));
        break;
    }
    m->grounded = 0;
}

void city_build_props(int cx, int cz, Mesh *m) {
    ChunkGen g;
    mesh_begin(m, cx, cz);
    chunk_gen_init(&g, cx, cz, PROPS_PLAN_PAD, PROPS_BUILD_PAD, PROPS_LAMP_PAD);
    Prop *list = (Prop *)malloc(sizeof(Prop) * MAX_PROPS);
    if (!list) fatal("out of memory placing street furniture");
    int n = city_props(&g, g.ox, g.oz, g.ox + CHUNK_SIZE, g.oz + CHUNK_SIZE, PROPS_GROUND | PROPS_WALLS, list, MAX_PROPS);
    for (int i = 0; i < n && m->count < BUDGET; ++i) emit_prop(m, &list[i]);
    free(list);
    chunk_gen_free(&g);
}
